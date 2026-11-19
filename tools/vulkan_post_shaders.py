#!/usr/bin/env python3
"""Build Vulkan's anti-aliasing passes.

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_post_shaders.py
The generated C (port/linux/vulkan/xgpu_vulkan_post_shaders.c) embeds the
SPIR-V of display.anti_aliasing's passes and SMAA's lookup textures,
inflated; the game needs no shader compiler and no zlib for them.
--check verifies that the checked-in file still matches its sources.

The passes are the GLSL below: a vertex shader, FXAA, and the main
functions of SMAA's three passes after SMAA.hlsl (port/third_party/smaa) at
its HIGH preset. Each pass draws one triangle covering the rectangle (its
corners from gl_VertexID, with no vertex data) and finds its texels from
gl_FragCoord. FXAA is written here, after Timothy Lottes' description: the
direction of an edge from the luma around a pixel, the edge's ends found by
stepping along it, and the pixel blended across it by where it lies between
them, or by how much it stands out from its neighbours. Only the interface
changes when they are built: the uniforms become a block (set 0, binding 1,
as the backend's other draws have their uniforms), the samplers take the
texture units UNITS gives them as their bindings (set 1), the output a
location, and gl_VertexID is gl_VertexIndex. The rotate program
(ROTATE_SOURCE) is Vulkan's own: the present step's turn for a swapchain the
display turns. display.hdr's tone map and encoding are xgpu_hdr.c's GLSL
(hdr_programs), the same source tools/metal_post_shaders.py compiles for
Metal: the tone map reads the pixel it draws over as its input attachment
(set 2, binding 0: VK_KHR_dynamic_rendering_local_read), and again as each
sample's own for a multisampled target (hdr_tone_map_ms). The coordinates
stay: the passes' textures and targets have their first row at the top, as
Vulkan's images do, and gl_FragCoord counts rows from it. glslang 16.6.0 and
SPIRV-Tools 2026.4 (Homebrew's) produce the checked-in words.

The encoding is the frame's last pass in both backends (#1108): HDR's
encoding, display.gamma's curve over an SDR frame, and the dither.
display.panini's projection (PANINI) is a pass over a window's 3D view as
FXAA's is, in both backends; its uniforms add window, panini and tangents.
display.prism_bloom's halving and doubling (BLOOM_DOWN, BLOOM_UP) are
Vulkan's alone, so far, as is display.prism_tone's grade (PRISM_TONE), which
reads them.

Beside them, CLEAR is the pixel shader of the core's clears of some of a
target's colour channels (xgpu_vulkan.c, xgpu_vulkan_clear), which
vkCmdClearAttachments cannot do: drawn with the vertex shader above,
through the channels' write mask, it writes the uniforms' colour.
"""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
# display.hdr's tone map and encoding (their functions and main functions)
HDR_SOURCE = ROOT / "port/linux/src/xgpu_hdr.c"
SMAA = ROOT / "port/third_party/smaa"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c"

STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
COMMENTS = re.compile(r"/\*.*?\*/", re.S)
UNIFORM = re.compile(r"^uniform vec4 (\w+);\n", re.M)
SAMPLER = re.compile(r"^uniform (u?sampler2D) (\w+);\n", re.M)
# the uniforms' block: each pass's vec4s, in this order (std140), as far as
# the last its program uses (hdr in display.hdr's passes, window, panini
# and tangents in display.panini's; gamut, display.hdr_gamut's, in the
# encoding's)
UNIFORMS = ("metrics", "bounds", "hdr", "window", "panini", "tangents", "gamut")
# the fragment programs, in xgpu_vulkan_post_shaders.h's order
PROGRAMS = ("fxaa", "smaa_edges", "smaa_weights", "smaa_blend", "rotate", "hdr_tone_map", "hdr_tone_map_ms",
            "hdr_encode", "panini", "bloom_down", "bloom_up", "prism_tone", "deferred_resolve", "albedo_over",
            "terrain_debug")
# one triangle over the whole viewport
VERTEX_SOURCE = (
    "void main()\n"
    "{\n"
    "\tgl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
    "}\n"
)
FXAA_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "const float EDGE_THRESHOLD = 0.125;\n"
    "const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
    "const float SUBPIXEL_QUALITY = 0.75;\n"
    "const int STEPS = 12;\n"
    "const float STEP_LENGTHS[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
    "\n"
    "vec3 color_at(vec2 position)\n"
    "{\n"
    "\treturn textureLod(color_texture, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
    "}\n"
    "\n"
    "float luma_at(vec2 position)\n"
    "{\n"
    "\treturn dot(color_at(position), vec3(0.299, 0.587, 0.114));\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 position = gl_FragCoord.xy * metrics.xy;\n"
    "\tfloat luma = luma_at(position);\n"
    "\tfloat up = luma_at(position + vec2(0.0, -metrics.y));\n"
    "\tfloat down = luma_at(position + vec2(0.0, metrics.y));\n"
    "\tfloat left = luma_at(position + vec2(-metrics.x, 0.0));\n"
    "\tfloat right = luma_at(position + vec2(metrics.x, 0.0));\n"
    "\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
    "\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
    "\n"
    "\t\n"
    "\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
    "\t\tdiscard;\n"
    "\tfloat up_left = luma_at(position + vec2(-metrics.x, -metrics.y));\n"
    "\tfloat up_right = luma_at(position + vec2(metrics.x, -metrics.y));\n"
    "\tfloat down_left = luma_at(position + vec2(-metrics.x, metrics.y));\n"
    "\tfloat down_right = luma_at(position + vec2(metrics.x, metrics.y));\n"
    "\n"
    "\t\n"
    "\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
    "\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
    "\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
    "\n"
    "\t\n"
    "\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
    "\t\tabs(up_right + down_right - 2.0 * right) >=\n"
    "\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
    "\t\tabs(down_left + down_right - 2.0 * down);\n"
    "\tfloat before = horizontal ? up : left;\n"
    "\tfloat after = horizontal ? down : right;\n"
    "\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
    "\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
    "\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
    "\tvec2 across = (horizontal ? vec2(0.0, metrics.y) : vec2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
    "\tvec2 along = horizontal ? vec2(metrics.x, 0.0) : vec2(0.0, metrics.y);\n"
    "\n"
    "\t\n"
    "\tvec2 end_before = position + 0.5 * across - along;\n"
    "\tvec2 end_after = position + 0.5 * across + along;\n"
    "\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
    "\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
    "\tbool reached_before = abs(luma_before) >= gradient;\n"
    "\tbool reached_after = abs(luma_after) >= gradient;\n"
    "\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
    "\t{\n"
    "\t\tif (!reached_before)\n"
    "\t\t{\n"
    "\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
    "\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
    "\t\t\treached_before = abs(luma_before) >= gradient;\n"
    "\t\t}\n"
    "\t\tif (!reached_after)\n"
    "\t\t{\n"
    "\t\t\tend_after += along * STEP_LENGTHS[index];\n"
    "\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
    "\t\t\treached_after = abs(luma_after) >= gradient;\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\t\n"
    "\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
    "\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
    "\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
    "\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
    "\tresult = vec4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
    "}\n"
)
# the main functions of SMAA's passes, after SMAA.hlsl; its vertex functions'
# offsets are found for each pixel
SMAA_EDGES_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec4 offset[3];\n"
    "\tSMAAEdgeDetectionVS(texcoord, offset);\n"
    "\tresult = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
    "}\n"
)
SMAA_WEIGHTS_SOURCE = (
    "uniform sampler2D edges_texture;\n"
    "uniform sampler2D area_texture;\n"
    "uniform sampler2D search_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec2 pixcoord;\n"
    "\tvec4 offset[3];\n"
    "\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
    "\tresult = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
    "\t\tsearch_texture, vec4(0.0));\n"
    "}\n"
)
SMAA_BLEND_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "uniform sampler2D weights_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec4 offset;\n"
    "\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
    "\tresult = SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture);\n"
    "}\n"
)
SMAA_MAINS = (SMAA_EDGES_SOURCE, SMAA_WEIGHTS_SOURCE, SMAA_BLEND_SOURCE)
# what comes before SMAA.hlsl and a pass's main function
SMAA_PREFIX = (
    "#define SMAA_GLSL_4 1\n"
    "uniform vec4 metrics;\n"
    "#define SMAA_RT_METRICS metrics\n"
    "#define SMAA_PRESET_HIGH 1\n"
)
# each sampler's texture unit, which becomes its binding
UNITS = {"color_texture": 0, "edges_texture": 0, "area_texture": 1, "search_texture": 2, "weights_texture": 1,
         "local_texture": 1, "base_texture": 1, "additions_texture": 2, "gbuffer_texture": 3, "albedo_texture": 0,
         "classes_texture": 1, "events_texture": 2}
# display.lighting "ce_deferred"'s resolve (Vulkan's: xgpu_vulkan_post_
# deferred_resolve): each pixel the copy of the light (the colour target's
# pixels) times the base plus the additions, the additions' alpha (CE's
# destination alpha after the specular and reflection passes); or, by
# bounds.x, one of them alone (debug.gbuffer_view: 1 the base, 2 the light,
# 3 the additions), or the G-buffer's (xgpu_stream.h's GBUFFER_BEGIN: its
# words at gbuffer_texture, texelFetch): 4 the base colour (the level's the
# base's), 5 the normal, 6 the roughness, 7 the metalness, 8 the material
# source (the window's depth in additions_texture's place: a pixel drawn,
# its depth not bounds.y, the cleared depth, that the G-buffer has not is
# magenta; one the G-buffer has that the window did not draw, red), 9 the
# instances. bounds.z 1 where the G-buffer was drawn. The
# images are the target's size: texels by gl_FragCoord.
DEFERRED_RESOLVE_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "uniform sampler2D base_texture;\n"
    "uniform sampler2D additions_texture;\n"
    "uniform usampler2D gbuffer_texture;\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "vec3 unfold(vec2 e)\n"
    "{\n"
    "\tvec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));\n"
    "\tfloat t = max(-n.z, 0.0);\n"
    "\tn.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);\n"
    "\treturn normalize(n);\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "\tvec4 light = texelFetch(color_texture, texel, 0);\n"
    "\tvec4 base = texelFetch(base_texture, texel, 0);\n"
    "\tvec4 additions = texelFetch(additions_texture, texel, 0);\n"
    "\tint view = int(bounds.x + 0.5);\n"
    "\tvec3 color = light.rgb * base.rgb + additions.rgb;\n"
    "\tfloat alpha = additions.a;\n"
    "\tif (view == 1)\n"
    "\t\tcolor = base.rgb;\n"
    "\telse if (view == 2)\n"
    "\t\tcolor = light.rgb;\n"
    "\telse if (view == 3)\n"
    "\t\tcolor = additions.rgb;\n"
    "\telse if (view >= 4)\n"
    "\t{\n"
    "\t\tuvec4 words = bounds.z > 0.5 ? texelFetch(gbuffer_texture, texel, 0) : uvec4(0u);\n"
    "\t\tbool present = (words.w & 0x8000u) != 0u;\n"
    "\t\tvec4 parameters = unpackUnorm4x8(words.z);\n"
    "\t\tuint flags = uint(parameters.w * 255.0 + 0.5) >> 4;\n"
    "\t\tuint tier = (words.w >> 12) & 7u;\n"
    "\t\tuint instance = words.w >> 16;\n"
    "\t\tcolor = vec3(0.0);\n"
    "\t\talpha = 1.0;\n"
    "\t\tif (view == 4 && present)\n"
    "\t\t\tcolor = (flags & 1u) != 0u ? base.rgb : unpackUnorm4x8(words.x).rgb;\n"
    "\t\telse if (view == 5 && present && (flags & 8u) != 0u)\n"
    "\t\t\tcolor = unfold(unpackSnorm2x16(words.y)) * 0.5 + 0.5;\n"
    "\t\telse if (view == 6 && present)\n"
    "\t\t\tcolor = vec3(parameters.x);\n"
    "\t\telse if (view == 7 && present)\n"
    "\t\t\tcolor = vec3(parameters.y);\n"
    "\t\telse if (view == 8)\n"
    "\t\t{\n"
    "\t\t\tbool drawn = abs(additions.r - bounds.y) > 1.0e-7;\n"
    "\t\t\tif (present && !drawn)\n"
    "\t\t\t\tcolor = vec3(1.0, 0.0, 0.0);\n"
    "\t\t\telse if (present)\n"
    "\t\t\t\tcolor = tier == 4u ? vec3(0.0, 1.0, 1.0) : tier == 3u ? vec3(0.0, 0.0, 1.0) : tier == 2u ?\n"
    "\t\t\t\t\tvec3(0.0, 1.0, 0.0) : tier == 1u ? vec3(1.0, 1.0, 0.0) : vec3(1.0, 0.5, 0.0);\n"
    "\t\t\telse if (drawn)\n"
    "\t\t\t\tcolor = vec3(1.0, 0.0, 1.0);\n"
    "\t\t}\n"
    "\t\telse if (view == 10 && bounds.z > 0.5)\n"
    "\t\t{\n"
    "\t\t\tvec2 motion = unpackHalf2x16(words.x);\n"
    "\t\t\tbool history = (words.z & 0x10000u) != 0u;\n"
    "\t\t\tcolor = history ? vec3(clamp(motion / 16.0 + 0.5, 0.0, 1.0), 0.5) : vec3(0.0, 0.0, 1.0);\n"
    "\t\t\tif (words.w == 0u)\n"
    "\t\t\t\tcolor = vec3(0.0);\n"
    "\t\t}\n"
    "\t\telse if (view == 11 && bounds.z > 0.5)\n"
    "\t\t\tcolor = vec3(unpackHalf2x16(words.y), unpackHalf2x16(words.z).x);\n"
    "\t\telse if (view == 9 && present)\n"
    "\t\t{\n"
    "\t\t\tuint h = instance * 2654435761u;\n"
    "\t\t\tcolor = instance == 0u ? vec3(0.25) : instance == 0xffffu ? vec3(1.0) :\n"
    "\t\t\t\tvec3(float((h >> 8) & 255u), float((h >> 16) & 255u), float(h >> 24)) / 255.0 * 0.8 + 0.2;\n"
    "\t\t}\n"
    "\t}\n"
    "\tresult = vec4(color, alpha);\n"
    "}\n"
)
# PRISM v4 M4's (Vulkan's: xgpu_vulkan_post_albedo_over): the level's base
# colour where the G-buffer pass drew a pack's or a mod's material's maps, in
# the base target's place before the resolve: each pixel of the level (the
# G-buffer's fourth word: present, instance 0) whose material is of the pack
# or mod tier (its bits 12 to 14: 3 or 4) and whose albedo target's alpha is
# past 0 takes the albedo's colour (drawn through the red, green and blue
# alone: the base's alpha is CE's destination alpha); every other is left as
# it is.
ALBEDO_OVER_SOURCE = (
    "uniform sampler2D albedo_texture;\n"
    "uniform usampler2D gbuffer_texture;\n"
    "uniform vec4 metrics;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "\tvec4 albedo = texelFetch(albedo_texture, texel, 0);\n"
    "\tuint word = texelFetch(gbuffer_texture, texel, 0).w;\n"
    "\tuint tier = (word >> 12) & 7u;\n"
    "\tif ((word & 0x8000u) == 0u || (word >> 16) != 0u || tier < 3u || albedo.a <= 0.0)\n"
    "\t\tdiscard;\n"
    "\tresult = vec4(albedo.rgb, 1.0);\n"
    "}\n"
)
# the terrain overhaul's debug views (Vulkan's: xgpu_vulkan_post_terrain_debug; port/linux/vulkan/
# xgpu_vulkan_terrain.c), over the resolved frame (its copy, color_texture): by bounds.x's bits, 1 each opaque pixel
# of the G-buffer (its words at gbuffer_texture, bounds.z 1 where it was drawn) coloured by its material's classes
# (classes_texture: a texel a material, its id's low 6 bits across and high 6 down, its two classes in red and green
# as bytes; xgpu_vulkan_terrain.h's XGPU_TERRAIN_CLASS_*: sand gold, dirt brown, mud dark olive, rock blue-grey,
# snow white, ice cyan, water blue), striped 8 pixels wide where its second class differs, an object's only where
# its class is rock, a little of the frame's lightness kept, every other pixel the frame's lightness darkened; 2
# the events (events_texture: two texels each, x, y, the radius in pixels and the strength, then the kind and the
# side; bounds.y of them), each a disc with a solid rim, coloured by kind: a left foot green, a right foot red, a
# landing magenta, a vehicle's contact orange, an impact cyan, a detonation or an explosion white.
TERRAIN_DEBUG_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "uniform sampler2D classes_texture;\n"
    "uniform sampler2D events_texture;\n"
    "uniform usampler2D gbuffer_texture;\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "vec3 class_color(uint c)\n"
    "{\n"
    "\treturn c == 1u ? vec3(0.95, 0.75, 0.25) : c == 2u ? vec3(0.55, 0.32, 0.12) : c == 3u ?\n"
    "\t\tvec3(0.30, 0.26, 0.06) : c == 4u ? vec3(0.45, 0.55, 0.72) : c == 5u ? vec3(1.0) : c == 6u ?\n"
    "\t\tvec3(0.30, 0.95, 1.0) : c == 7u ? vec3(0.05, 0.25, 1.0) : vec3(0.0);\n"
    "}\n"
    "\n"
    "vec3 event_color(int kind, bool right)\n"
    "{\n"
    "\treturn kind <= 5 ? (right ? vec3(1.0, 0.1, 0.1) : vec3(0.1, 1.0, 0.1)) : kind == 6 ? vec3(1.0, 0.0, 1.0) :\n"
    "\t\tkind == 7 ? vec3(1.0, 0.55, 0.0) : kind == 8 ? vec3(0.0, 1.0, 1.0) : vec3(1.0);\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "\tvec4 frame = texelFetch(color_texture, texel, 0);\n"
    "\tint mode = int(bounds.x + 0.5);\n"
    "\tvec3 color = frame.rgb;\n"
    "\tif ((mode & 1) != 0)\n"
    "\t{\n"
    "\t\tuvec4 words = bounds.z > 0.5 ? texelFetch(gbuffer_texture, texel, 0) : uvec4(0u);\n"
    "\t\tuint material = words.w & 0xfffu;\n"
    "\t\tuint instance = words.w >> 16;\n"
    "\t\tfloat grey = clamp(dot(frame.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);\n"
    "\t\tcolor = vec3(grey * 0.3);\n"
    "\t\tif ((words.w & 0x8000u) != 0u && material != 0u)\n"
    "\t\t{\n"
    "\t\t\tuvec2 classes = uvec2(texelFetch(classes_texture, ivec2(int(material & 63u), int(material >> 6)), 0).rg *\n"
    "\t\t\t\t255.0 + 0.5);\n"
    "\t\t\tif (instance != 0u)\n"
    "\t\t\t\tclasses = classes.x == 4u ? uvec2(4u) : uvec2(0u);\n"
    "\t\t\tuint shown = (((texel.x + texel.y) >> 3) & 1) != 0 ? classes.y : classes.x;\n"
    "\t\t\tif (shown != 0u)\n"
    "\t\t\t\tcolor = mix(class_color(shown), vec3(grey), 0.3);\n"
    "\t\t}\n"
    "\t}\n"
    "\tif ((mode & 2) != 0)\n"
    "\t{\n"
    "\t\tint count = int(bounds.y + 0.5);\n"
    "\t\tfor (int i = 0; i < count; i++)\n"
    "\t\t{\n"
    "\t\t\tvec4 place = texelFetch(events_texture, ivec2(2 * i, 0), 0);\n"
    "\t\t\tfloat d = distance(gl_FragCoord.xy, place.xy);\n"
    "\t\t\tif (d < place.z)\n"
    "\t\t\t{\n"
    "\t\t\t\tvec4 what = texelFetch(events_texture, ivec2(2 * i + 1, 0), 0);\n"
    "\t\t\t\tfloat rim = d > place.z - 2.0 ? 1.0 : 0.45;\n"
    "\t\t\t\tcolor = mix(color, event_color(int(what.x + 0.5), what.y > 0.5), rim * place.w);\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "\tresult = vec4(color, frame.a);\n"
    "}\n"
)
# Vulkan's own program: the frame turned for a swapchain the display turns
# (a phone held sideways: xgpu_vulkan_post_rotate). Each pixel takes the
# texel the bounds' matrix (its rows xy and zw) turns it to, about the
# image's centre.
ROTATE_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "/* 1 / width, 1 / height, width, height of the turned image */\n"
    "uniform vec4 metrics;\n"
    "/* the turn: rows of a 2x2 matrix, from the turned image's coordinates to the frame's */\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 centred = gl_FragCoord.xy * metrics.xy - 0.5;\n"
    "\tvec2 coordinates = vec2(dot(bounds.xy, centred), dot(bounds.zw, centred)) + 0.5;\n"
    "\tresult = vec4(texture(color_texture, coordinates).rgb, 1.0);\n"
    "}\n"
)
# display.panini's projection (both backends': xgpu_vulkan_post_panini,
# xgpu_metal_post_panini; port/linux/include/panini_projection.h has the
# arithmetic, which this is the same as). Its uniforms: metrics, the
# target's; bounds, the rectangle's outermost texel centres (no texel of
# another window is read); window, the 3D view's corners in the target's
# pixels (x0, y0, width, height); panini, the projection's d, s and
# magnification at the middle; tangents, the rectilinear view's at the
# view's left, right, top and bottom. Each pixel takes the copy's pixel the
# rectilinear view saw its direction at, sampled over the pixels it covers
# there (the sides are drawn smaller): a bilinear tap for each two pixels
# each way, so that the taps' filters together cover them (up to 4 taps, 8
# pixels)
PANINI_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 window;\n"
    "uniform vec4 panini;\n"
    "uniform vec4 tangents;\n"
    "out vec4 result;\n"
    "\n"
    "/* the rectilinear tangents a point of the screen at tangents p shows */\n"
    "vec2 rectilinear(vec2 p)\n"
    "{\n"
    "\tfloat d = panini.x;\n"
    "\tvec2 projected = p / panini.z;\n"
    "\tfloat k = projected.x * projected.x / ((d + 1.0) * (d + 1.0));\n"
    "\tfloat c = max((sqrt(1.0 + k * (1.0 - d * d)) - k * d) / (k + 1.0), 1.0e-6);\n"
    "\tfloat S = (d + 1.0) / (d + c);\n"
    "\treturn vec2(projected.x / (S * c), projected.y / (S * (panini.y + (1.0 - panini.y) * c)));\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 place = (gl_FragCoord.xy - window.xy) / window.zw;\n"
    "\tvec2 seen = rectilinear(vec2(mix(tangents.x, tangents.y, place.x), mix(tangents.z, tangents.w, place.y)));\n"
    "\tvec2 source = window.xy + window.zw * (seen - tangents.xz) / (tangents.yw - tangents.xz);\n"
    "\tvec2 across = dFdx(source);\n"
    "\tvec2 down = dFdy(source);\n"
    "\tivec2 taps = ivec2(clamp(ceil(vec2(length(across), length(down)) * 0.5 - 0.01), 1.0, 4.0));\n"
    "\tvec4 sum = vec4(0.0);\n"
    "\tfor (int row = 0; row < taps.y; row++)\n"
    "\t{\n"
    "\t\tfor (int column = 0; column < taps.x; column++)\n"
    "\t\t{\n"
    "\t\t\tvec2 offset = (vec2(column, row) + 0.5) / vec2(taps) - 0.5;\n"
    "\t\t\tvec2 at = (source + across * offset.x + down * offset.y) * metrics.xy;\n"
    "\t\t\tsum += textureLod(color_texture, clamp(at, bounds.xy, bounds.zw), 0.0);\n"
    "\t\t}\n"
    "\t}\n"
    "\tresult = sum / float(taps.x * taps.y);\n"
    "}\n"
)
# display.prism_bloom's passes (xgpu_vulkan_post_bloom; xgpu_backend.h's
# XGPU_BLOOM_*), dual filtering over the 3D view's half floats before its
# tone map. Their uniforms: metrics, the source's 1 / width, 1 / height,
# width, height; bounds, the source's texels read (the rectangle's outermost
# texel centres, as FXAA's); hdr, 1 / width and 1 / height of the image
# drawn (its pixels' coordinates as the source's), the threshold (the first
# halving's: each tap's light past it alone, at most 16; 0 none; less than 0,
# display.prism_tone's first halving: each tap's light within 0 and minus it,
# a tap that is no number none, so that no stray pixel spreads) and the
# strength (the tent's sum by it). The halving (BLOOM_DOWN) is Jimenez's
# 13 taps (Call of Duty: Advanced Warfare), its first's groups weighed by one
# over one plus their luma (Karis's: no fireflies); the doubling back
# (BLOOM_UP) a 3 by 3 tent of the smaller, added (the pipeline's blend) to
# the larger, and at last to the view
BLOOM_DOWN_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 hdr;\n"
    "out vec4 result;\n"
    "\n"
    "vec3 tap(vec2 uv, vec2 offset)\n"
    "{\n"
    "\tvec3 color = textureLod(color_texture, clamp(uv + offset * metrics.xy, bounds.xy, bounds.zw), 0.0).rgb;\n"
    "\tif (hdr.z < 0.0)\n"
    "\t\treturn mix(clamp(color, vec3(0.0), vec3(-hdr.z)), vec3(0.0), isnan(color));\n"
    "\tcolor = max(color, vec3(0.0));\n"
    "\treturn hdr.z > 0.0 ? min(max(color - vec3(hdr.z), vec3(0.0)), vec3(16.0)) : color;\n"
    "}\n"
    "\n"
    "float karis(vec3 color)\n"
    "{\n"
    "\treturn hdr.z > 0.0 ? 1.0 / (1.0 + dot(color, vec3(0.2126, 0.7152, 0.0722)) * 0.25) : 1.0;\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 uv = gl_FragCoord.xy * hdr.xy;\n"
    "\tvec3 a = tap(uv, vec2(-2.0, -2.0)), b = tap(uv, vec2(0.0, -2.0)), c = tap(uv, vec2(2.0, -2.0));\n"
    "\tvec3 d = tap(uv, vec2(-2.0, 0.0)), e = tap(uv, vec2(0.0, 0.0)), f = tap(uv, vec2(2.0, 0.0));\n"
    "\tvec3 g = tap(uv, vec2(-2.0, 2.0)), h = tap(uv, vec2(0.0, 2.0)), i = tap(uv, vec2(2.0, 2.0));\n"
    "\tvec3 j = tap(uv, vec2(-1.0, -1.0)), k = tap(uv, vec2(1.0, -1.0));\n"
    "\tvec3 l = tap(uv, vec2(-1.0, 1.0)), m = tap(uv, vec2(1.0, 1.0));\n"
    "\tvec3 groups[5];\n"
    "\tfloat weights[5], sum = 0.0;\n"
    "\tvec3 color = vec3(0.0);\n"
    "\tgroups[0] = (j + k + l + m) * 0.25;\n"
    "\tgroups[1] = (a + b + d + e) * 0.25;\n"
    "\tgroups[2] = (b + c + e + f) * 0.25;\n"
    "\tgroups[3] = (d + e + g + h) * 0.25;\n"
    "\tgroups[4] = (e + f + h + i) * 0.25;\n"
    "\tfor (int n = 0; n < 5; n++)\n"
    "\t{\n"
    "\t\tweights[n] = (n == 0 ? 0.5 : 0.125) * karis(groups[n]);\n"
    "\t\tcolor += groups[n] * weights[n];\n"
    "\t\tsum += weights[n];\n"
    "\t}\n"
    "\tresult = vec4(color / sum, 1.0);\n"
    "}\n"
)
BLOOM_UP_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 hdr;\n"
    "out vec4 result;\n"
    "\n"
    "vec3 tap(vec2 uv, vec2 offset)\n"
    "{\n"
    "\treturn textureLod(color_texture, clamp(uv + offset * metrics.xy, bounds.xy, bounds.zw), 0.0).rgb;\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 uv = gl_FragCoord.xy * hdr.xy;\n"
    "\tvec3 color = tap(uv, vec2(0.0)) * 4.0;\n"
    "\tcolor += (tap(uv, vec2(-1.0, 0.0)) + tap(uv, vec2(1.0, 0.0)) + tap(uv, vec2(0.0, -1.0)) +\n"
    "\t\ttap(uv, vec2(0.0, 1.0))) * 2.0;\n"
    "\tcolor += tap(uv, vec2(-1.0, -1.0)) + tap(uv, vec2(1.0, -1.0)) + tap(uv, vec2(-1.0, 1.0)) +\n"
    "\t\ttap(uv, vec2(1.0, 1.0));\n"
    "\tresult = vec4(max(color, vec3(0.0)) * (hdr.w / 16.0), 0.0);\n"
    "}\n"
)
# display.prism_tone's grade (xgpu_vulkan_post_prism_tone), over the 3D
# view's half floats before the bloom and the tone map: each pixel's light by
# the local mean about it (local_texture: the view halved by BLOOM_DOWN to a
# sixteenth and brought back to a quarter by BLOOM_UP's tent, without its
# threshold), the mean lifted (hdr.y, a power under white; the light past
# white kept) and scaled (hdr.w), the pixel's own ratio to it raised to
# 1 + hdr.x (local contrast, the ratio kept within a quarter and four times
# so that no edge rings), then the colour's saturation by hdr.z about its
# luma; the exposure besides by the view's mean (sixteen taps of the local
# mean, each at most white): (panini.x / mean) to the panini.y, 1 to
# panini.z, a dark view lifted, a bright one left. Its uniforms: metrics, the
# target's 1 / width, 1 / height; bounds, the view's copy's texels read (as
# FXAA's); window, the local mean's (as texture coordinates); panini, the
# exposure's aim, power and most
PRISM_TONE_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "uniform sampler2D local_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 hdr;\n"
    "uniform vec4 window;\n"
    "uniform vec4 panini;\n"
    "out vec4 result;\n"
    "\n"
    "const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 uv = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec3 color = max(textureLod(color_texture, clamp(uv, bounds.xy, bounds.zw), 0.0).rgb, vec3(0.0));\n"
    "\tvec3 local = max(textureLod(local_texture, clamp(uv, window.xy, window.zw), 0.0).rgb, vec3(0.0));\n"
    "\tfloat average = 0.0;\n"
    "\tfor (int row = 0; row < 4; row++)\n"
    "\t\tfor (int column = 0; column < 4; column++)\n"
    "\t\t\taverage += min(dot(textureLod(local_texture, mix(window.xy, window.zw,\n"
    "\t\t\t\t(vec2(column, row) + 0.5) * 0.25), 0.0).rgb, LUMA), 1.0);\n"
    "\taverage = max(average * 0.0625, 1.0e-3);\n"
    "\tfloat exposure = clamp(pow(panini.x / average, panini.y), 1.0, panini.z) * hdr.w;\n"
    "\tfloat luma = dot(color, LUMA);\n"
    "\tfloat mean = max(dot(local, LUMA), 1.0e-4);\n"
    "\tfloat lifted = (pow(min(mean, 1.0), hdr.y) + max(mean - 1.0, 0.0)) * exposure;\n"
    "\tfloat ratio = clamp(max(luma, 1.0e-5) / mean, 0.25, 4.0);\n"
    "\tfloat graded = lifted * pow(ratio, 1.0 + hdr.x);\n"
    "\tvec3 outcome = luma > 1.0e-5 ? color * (graded / luma) : color * exposure;\n"
    "\tfloat outcome_luma = dot(outcome, LUMA);\n"
    "\tresult = vec4(max(mix(vec3(outcome_luma), outcome, hdr.z), vec3(0.0)), 1.0);\n"
    "}\n"
)
# the clears' pixel shader (its uniforms at the passes' binding)
CLEAR = """#version 450
layout(std140, set = 0, binding = 1) uniform ClearUniforms
{
\tvec4 color;
};
layout(location = 0) out vec4 result;
void main()
{
\tresult = color;
}
"""
# SMAA's lookup textures: their file, size and channels
TEXTURES = (("area", "area_tex.zlib", 160, 560, 2), ("search", "search_tex.zlib", 64, 16, 1))


def literal(text: str) -> str:
    """The C string literals of text (comments removed), joined."""
    return "".join(ast.literal_eval(value) for value in STRINGS.findall(COMMENTS.sub("", text)))


def declaration(source: str, name: str) -> str:
    match = re.search(r"(?:static )?const char " + name + r"\[\]\s*=\s*(.*?);\n", source, re.S)
    if not match:
        raise ValueError(f"{name} not found in {HDR_SOURCE.name}")
    return literal(match[1])


def smaa_hlsl() -> str:
    """SMAA.hlsl, its few characters beyond ASCII (in comments) spaces."""
    return bytes(byte if byte < 0x80 else 0x20 for byte in (SMAA / "SMAA.hlsl").read_bytes()).decode("ascii")


def hdr_programs() -> dict[str, str]:
    """display.hdr's fragment programs (xgpu_hdr.c): the tone map, in place
    (the pixel drawn over is the pass's input attachment), and the encoding,
    each its main function after the functions they share."""
    source = HDR_SOURCE.read_text()
    functions = declaration(source, "xgpu_hdr_functions_source")
    return {name: functions + declaration(source, f"{name}_source") for name in ("tone_map", "encode")}


def multisampled(code: str) -> str:
    """The tone map of a multisampled target: each sample's own pixel (which
    makes the pass run for each sample)."""
    changed = code.replace("uniform subpassInput ", "uniform subpassInputMS ").replace(
        "subpassLoad(frame_input)", "subpassLoad(frame_input, gl_SampleID)")
    if changed.count("subpassInputMS") != 1 or "gl_SampleID" not in changed:
        raise ValueError("the tone map's input attachment changed")
    return changed


def translate(stage: str, code: str, bindings: dict[str, int]) -> str:
    found = UNIFORM.findall(code)
    unknown = set(found) - set(UNIFORMS)
    if unknown:
        raise ValueError(f"uniforms {sorted(unknown)} are not in the block")
    code = UNIFORM.sub("", code)
    used = max([1] + [UNIFORMS.index(name) for name in found])

    def sampler(match):
        return f"layout(set = 1, binding = {bindings[match[2]]}) uniform {match[1]} {match[2]};\n"

    code = SAMPLER.sub(sampler, code)
    code = code.replace("out vec4 result;\n", "layout(location = 0) out vec4 result;\n")
    code = code.replace("gl_VertexID", "gl_VertexIndex")
    block = ""
    if stage == "frag":
        block = "layout(std140, set = 0, binding = 1) uniform PostUniforms\n{\n"
        block += "".join(f"\tvec4 {name};\n" for name in UNIFORMS[:used + 1]) + "};\n"
    return "#version 450\n" + block + code


def words_array(name: str, data: bytes, public: bool = False) -> str:
    data += b"\0" * (-len(data) % 4)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"{'' if public else 'static '}const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def generate(args):
    build = args.build_dir
    build.mkdir(parents=True, exist_ok=True)
    hlsl = smaa_hlsl()
    programs = {
        "post": ("vert", VERTEX_SOURCE),
        "fxaa": ("frag", FXAA_SOURCE),
    }
    for name, main in zip(PROGRAMS[1:4], SMAA_MAINS):
        programs[name] = ("frag", SMAA_PREFIX + hlsl + "\n" + main)
    programs["rotate"] = ("frag", ROTATE_SOURCE)
    hdr = hdr_programs()
    programs["hdr_tone_map"] = ("frag", hdr["tone_map"])
    programs["hdr_tone_map_ms"] = ("frag", multisampled(hdr["tone_map"]))
    programs["hdr_encode"] = ("frag", hdr["encode"])
    programs["panini"] = ("frag", PANINI_SOURCE)
    programs["bloom_down"] = ("frag", BLOOM_DOWN_SOURCE)
    programs["bloom_up"] = ("frag", BLOOM_UP_SOURCE)
    programs["prism_tone"] = ("frag", PRISM_TONE_SOURCE)
    programs["deferred_resolve"] = ("frag", DEFERRED_RESOLVE_SOURCE)
    programs["albedo_over"] = ("frag", ALBEDO_OVER_SOURCE)
    programs["terrain_debug"] = ("frag", TERRAIN_DEBUG_SOURCE)
    programs["clear"] = ("frag", CLEAR)
    text = [
        "/* Generated by tools/vulkan_post_shaders.py from its GLSL, xgpu_hdr.c and port/third_party/smaa. "
        "Do not edit. */\n",
        '#include "xgpu_vulkan_post_shaders.h"\n',
    ]
    for name, (stage, code) in programs.items():
        path = build / f"{name}.{stage}"
        path.write_text(code if name == "clear" else translate(stage, code, UNITS))
        spv = path.with_suffix(f".{stage}.spv")
        spv.unlink(missing_ok=True)
        subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.1", "-o", str(spv), str(path)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        subprocess.run([args.validator, "--target-env", "vulkan1.1", str(spv)], check=True)
        text.append(words_array(f"{name}_{stage}", spv.read_bytes()))
        print(f"{name}: {spv.stat().st_size} bytes of SPIR-V")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_vertex =\n{")
    text.append("\tpost_vert, sizeof(post_vert) / 4,\n};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PROGRAM_COUNT] =\n{")
    for name in PROGRAMS:
        text.append(f"\t{{ {name}_frag, sizeof({name}_frag) / 4 }},")
    text.append("};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_clear =\n{")
    text.append("\tclear_frag, sizeof(clear_frag) / 4,\n};\n")
    for name, file, width, height, channels in TEXTURES:
        texels = zlib.decompress((SMAA / file).read_bytes())
        if len(texels) != width * height * channels:
            raise ValueError(f"{file} is {len(texels)} bytes, not {width}x{height}x{channels}")
        text.append(words_array(f"xgpu_vulkan_post_{name}_texture", texels, public=True))
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{len(programs)} modules validated for Vulkan 1.1")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_post_shaders")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        generate(args)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout.decode())
        raise


if __name__ == "__main__":
    main()
