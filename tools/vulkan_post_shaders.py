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
SAMPLER = re.compile(r"^uniform (u?sampler2D(?:Array)?) (\w+);\n", re.M)
# the uniforms' block: each pass's vec4s, in this order (std140), as far as
# the last its program uses (hdr in display.hdr's passes, window, panini
# and tangents in display.panini's; gamut, display.hdr_gamut's, in the
# encoding's)
UNIFORMS = ("metrics", "bounds", "hdr", "window", "panini", "tangents", "gamut", "view_eye", "view_axis",
            "view_across", "view_down", "view_corners", "class_cue0", "class_cue1") + \
    tuple(f"layer{index}" for index in range(16)) + tuple(f"prints{index}" for index in range(4))
# the fragment programs, in xgpu_vulkan_post_shaders.h's order
PROGRAMS = ("fxaa", "smaa_edges", "smaa_weights", "smaa_blend", "rotate", "hdr_tone_map", "hdr_tone_map_ms",
            "hdr_encode", "panini", "bloom_down", "bloom_up", "prism_tone", "deferred_resolve", "albedo_over",
            "terrain_debug", "terrain_layers", "terrain_layers_local", "terrain_prints", "terrain_prints_local")
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
         "classes_texture": 1, "events_texture": 2, "extras_texture": 2, "detail_texture": 4,
         "surface_texture": 5, "macro_texture": 6, "prints_texture": 7, "pages_texture": 8}
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
# the terrain overhaul's HQ layers (T1; Vulkan's: xgpu_vulkan_post_terrain_layers; port/linux/vulkan/
# xgpu_vulkan_terrain.c), drawn at the texture pass's end (BASE_TEXTURED) into the base target (its colour) and the
# G-buffer's words (location 1, all four: the copy's, the normal and the roughness changed). Each level pixel (the
# words' copy at gbuffer_texture: present, instance 0) whose material has a layer (classes_texture's blue: the first
# class's layer + 1, the second's from bit 4) and that is nearer than bounds.z world units (extras_texture's
# distance) is found in the world (SCENE_VIEW's camera: view_*), and the pack's layers (detail_texture: the detail,
# its mean 1, as half, and the height; surface_texture: the normal's x and y, the roughness over its mean as half,
# the occlusion) are sampled in world space: from above on the ground, from all three axes on steep faces (whiteout
# blended), each projection hex-tiled (Mikkelsen 2022: three offset and turned samples on a triangle grid, weighted
# by their place and height), the macro map (macro_texture) varying the strength over tens of metres. CE's base
# colour is multiplied by the detail (detail transfer, design 3.2: in linear light, its lightness and some of its
# own hue by the layer's chroma, faded from bounds.y to bounds.z world units), so its mean and CE's colour stay; the
# normal is the HQ normal's whiteout blend over the G-buffer's (its lookups isotropic, faded from 1 to 6 world
# units: at mid range CE's light is the surface's own); the roughness is scaled by the layer's. A material
# of two classes takes the second by its lightness against the classes' cues (class_cue0, 1: Oklab L by class)
# and, with rock, by the slope, then by the layers' heights. layer{2i}: per world unit, strength, chroma, normal;
# layer{2i+1}: roughness, height contrast, the grain's weight (a second, finer, turned lookup of the layer from
# above, FINE_SCALE times as many tiles, within 1 to 12 world units: the sand's grain, which one tile at its size
# draws too large and soft up close), the steepness triplanar starts at, negative where the taps are not turned
# (rock: its strata stay level). bounds.w 1: the weight shown. window.x: the view's pixels over the screen's each way
# under an upscaler (1 without one): the layers' lookups' footprints are the screen's pixels' (the prints march at
# the view's own).
#
# T4's prints (TERRAIN_PRINTS: the same with the parts marked PRINTS; the plain pass is the text without them, its
# words unchanged): the prints' pool (prints_texture, a 2D array of pages: the displacement in world units, the
# compaction, the ground's height over the page's own times the coverage, the coverage) through the page table
# about the camera (pages_texture: a page's pool layer + 1, its x and y, its height's bits; a page at x, y modulo the
# window's side). Where the pixel's ground takes prints (sand, dirt, mud, snow) within prints2.x world units, its
# view ray is marched through the field from the highest rim to the deepest floor (linear steps of about a pixel's
# footprint, at most prints2.y, refined between the last two), the layers looked up at the point met (CE's own colour
# stays the pixel's), the normal bent by the field's slope there, the floor a little darker (damp sand under the
# dry: snow packed greyer and smoother), and the field marched toward the sun (prints1: toward it) over prints2.z
# steps for the micro-shadow, written with the cavity's occlusion (the floor under the field's mean about it) at
# location 2 (micro_out: Raster's light pass multiplies its sun by red). prints0: pages a world unit, a texel's
# world units, a page's own texels, the window's side; prints3: the deepest floor and the highest rim (world
# units), a page's texels with its apron, its last level. prints2.w 1: the micro-shadow, the cavity and the depth
# shown.
TERRAIN_LAYERS_RAW = """uniform sampler2D color_texture;
uniform sampler2D classes_texture;
uniform usampler2D extras_texture;
uniform usampler2D gbuffer_texture;
uniform sampler2DArray detail_texture;
uniform sampler2DArray surface_texture;
uniform sampler2D macro_texture;
/*PRINTS{*/uniform sampler2DArray prints_texture;
uniform usampler2D pages_texture;
/*}PRINTS*/uniform vec4 metrics;
uniform vec4 bounds;
uniform vec4 hdr;
uniform vec4 window;
uniform vec4 panini;
uniform vec4 tangents;
uniform vec4 gamut;
uniform vec4 view_eye;
uniform vec4 view_axis;
uniform vec4 view_across;
uniform vec4 view_down;
uniform vec4 view_corners;
uniform vec4 class_cue0;
uniform vec4 class_cue1;
uniform vec4 layer0;
uniform vec4 layer1;
uniform vec4 layer2;
uniform vec4 layer3;
uniform vec4 layer4;
uniform vec4 layer5;
uniform vec4 layer6;
uniform vec4 layer7;
uniform vec4 layer8;
uniform vec4 layer9;
uniform vec4 layer10;
uniform vec4 layer11;
uniform vec4 layer12;
uniform vec4 layer13;
uniform vec4 layer14;
uniform vec4 layer15;
/*PRINTS{*/uniform vec4 prints0;
uniform vec4 prints1;
uniform vec4 prints2;
uniform vec4 prints3;
/*}PRINTS*/out vec4 result;
layout(location = 1) out uvec4 words_out;
/*PRINTS{*/layout(location = 2) out vec4 micro_out;
/*}PRINTS*/
vec4 layer_a(int i)
{
	return i == 0 ? layer0 : i == 1 ? layer2 : i == 2 ? layer4 : i == 3 ? layer6 : i == 4 ? layer8 : i == 5 ? layer10 :
		i == 6 ? layer12 : layer14;
}

vec4 layer_b(int i)
{
	return i == 0 ? layer1 : i == 1 ? layer3 : i == 2 ? layer5 : i == 3 ? layer7 : i == 4 ? layer9 : i == 5 ? layer11 :
		i == 6 ? layer13 : layer15;
}

float cue(uint c)
{
	return c < 4u ? class_cue0[c] : class_cue1[c - 4u];
}

vec3 unfold(vec2 e)
{
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

vec2 fold(vec3 n)
{
	vec2 f = n.xy / max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-24);
	if (n.z < 0.0)
		f = (1.0 - abs(f.yx)) * vec2(f.x >= 0.0 ? 1.0 : -1.0, f.y >= 0.0 ? 1.0 : -1.0);
	return f;
}

vec2 hash2(ivec2 p)
{
	uvec2 q = uvec2(p) * uvec2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	uvec2 r = uvec2(n, n * 3812015801u);
	return vec2(r >> 8) * (1.0 / 16777216.0);
}

/* (the grain's lookup: this many times the layer's tiles; not a whole number, so that the two never line up) */
#define FINE_SCALE 3.71

struct taps
{
	vec4 detail;
	vec4 surface;
};

/* a layer sampled hex-tiled at uv (tile units) with its gradients: the detail (rgb mean 1, height) and the surface
(normal xy in the projection's frame, -1..1; roughness ratio; occlusion) */
taps hex_sample(int layer, vec2 uv, vec2 gx, vec2 gy, float turn, float contrast, bool surfaced)
{
	vec2 st = uv * 1.7320508;
	vec2 skew = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
	ivec2 base = ivec2(floor(skew));
	vec3 t = vec3(fract(skew), 0.0);
	t.z = 1.0 - t.x - t.y;
	float s = t.z < 0.0 ? 1.0 : 0.0;
	float s2 = 2.0 * s - 1.0;
	vec3 w = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	ivec2 v[3] = ivec2[3](base + ivec2(int(s), int(s)), base + ivec2(int(s), int(1.0 - s)),
		base + ivec2(int(1.0 - s), int(s)));
	vec4 d[3];
	vec4 f[3];
	/* (the surface's lookups isotropic, at the footprint's longer side: a normal map filtered along a long
	footprint aliases into streaks toward the horizon, which the lit ground shows) */
	vec2 iso = vec2(max(length(gx), length(gy)), 0.0);
	vec3 h = vec3(0.5);
	/* (a tap whose place leaves it no weight, whatever its height, is not looked up) */
	vec3 reach = pow(max(w + contrast * 0.5, vec3(1.0e-4)), vec3(6.0));
	reach /= reach.x + reach.y + reach.z;
	for (int k = 0; k < 3; k++)
	{
		d[k] = vec4(0.5);
		f[k] = vec4(0.0, 0.0, 0.5, 1.0);
		if (reach[k] < 0.01)
		{
			w[k] = 0.0;
			continue;
		}
		vec2 r = hash2(v[k]);
		/* (the turn a unit vector of the hash's, no sine or cosine: the taps' cost) */
		vec2 q = vec2(r.y - 0.5, fract(r.x * 7.13 + r.y * 3.71) - 0.5);
		q = turn > 0.5 && dot(q, q) > 1.0e-6 ? normalize(q) : vec2(1.0, 0.0);
		mat2 m = mat2(q.x, q.y, -q.y, q.x);
		vec2 at = m * uv + r * 7.31;
		d[k] = textureGrad(detail_texture, vec3(at, float(layer)), m * gx, m * gy);
		if (surfaced)
		{
			f[k] = textureGrad(surface_texture, vec3(at, float(layer)), iso, iso.yx);
			f[k].xy = m * (f[k].xy * 2.0 - 1.0);
		}
		h[k] = d[k].a;
	}
	vec3 W = pow(max(w + contrast * (h - 0.5), vec3(1.0e-4)), vec3(6.0)) * step(vec3(1.0e-6), w);
	W /= max(W.x + W.y + W.z, 1.0e-6);
	taps result_taps;
	result_taps.detail = d[0] * W.x + d[1] * W.y + d[2] * W.z;
	result_taps.surface = f[0] * W.x + f[1] * W.y + f[2] * W.z;
	return result_taps;
}

/* a layer at a point: from above, or from all three axes where the surface is steep (whiteout normals); the
detail, the world normal and the roughness ratio */
void layer_at(int layer, vec3 p, vec3 dpx, vec3 dpy, vec3 n, bool surfaced, float fine, out vec4 detail,
	out vec3 normal, out float rough, out float ao)
{
	vec4 a = layer_a(layer), b = layer_b(layer);
	float scale = a.x;
	float strength = a.w;
	float turn = b.w > 0.0 ? 1.0 : 0.0;
	vec3 blend = pow(abs(n), vec3(4.0));
	blend /= blend.x + blend.y + blend.z;
	bool planar = abs(n.z) >= abs(b.w);
	if (planar)
		blend = vec3(0.0, 0.0, 1.0);
	detail = vec4(0.0);
	vec3 nsum = vec3(0.0);
	rough = 0.0;
	ao = 0.0;
	if (blend.z > 0.01)
	{
		taps z = hex_sample(layer, vec2(p.x, -p.y) * scale, vec2(dpx.x, -dpx.y) * scale, vec2(dpy.x, -dpy.y) * scale,
			turn, b.y, surfaced);
		/* (the grain: the layer's detail again FINE_SCALE times finer and turned (one lookup, not hex-tiled: at
		a grain's size under the hex-tiled layer its repeat does not show, and the pass's cost is its lookups;
		its normal left out, a millimetre's relief the lit ground does not show), its mean 1 multiplied in by its
		weight, near the camera alone) */
		float wf = b.z * fine;
		if (wf > 0.01)
		{
			float fs = scale * FINE_SCALE;
			const mat2 gm = mat2(0.8, 0.6, -0.6, 0.8);
			vec2 at = gm * (vec2(p.x, -p.y) * fs) + vec2(0.37, 0.61);
			vec2 ggx = gm * vec2(dpx.x, -dpx.y) * fs, ggy = gm * vec2(dpy.x, -dpy.y) * fs;
			vec4 gd = textureGrad(detail_texture, vec3(at, float(layer)), ggx, ggy);
			z.detail.rgb *= mix(vec3(1.0), gd.rgb * 2.0, wf);
		}
		vec2 txy = z.surface.xy * strength;
		vec3 tz = vec3(txy + n.xy, abs(n.z) + 1.0e-4);
		tz.z *= n.z >= 0.0 ? 1.0 : -1.0;
		detail += z.detail * blend.z;
		nsum += tz * blend.z;
		rough += z.surface.z * blend.z;
		ao += z.surface.w * blend.z;
	}
	if (blend.x > 0.01)
	{
		taps x = hex_sample(layer, vec2(p.y, -p.z) * scale, vec2(dpx.y, -dpx.z) * scale, vec2(dpy.y, -dpy.z) * scale,
			0.0, b.y, surfaced);
		vec2 txy = x.surface.xy * strength;
		vec3 tx = vec3(txy + n.yz, abs(n.x));
		detail += x.detail * blend.x;
		nsum += vec3(tx.z * (n.x >= 0.0 ? 1.0 : -1.0), tx.x, tx.y) * blend.x;
		rough += x.surface.z * blend.x;
		ao += x.surface.w * blend.x;
	}
	if (blend.y > 0.01)
	{
		taps y = hex_sample(layer, vec2(p.x, -p.z) * scale, vec2(dpx.x, -dpx.z) * scale, vec2(dpy.x, -dpy.z) * scale,
			0.0, b.y, surfaced);
		vec2 txy = y.surface.xy * strength;
		vec3 ty = vec3(txy + n.xz, abs(n.y));
		detail += y.detail * blend.y;
		nsum += vec3(ty.x, ty.z * (n.y >= 0.0 ? 1.0 : -1.0), ty.y) * blend.y;
		rough += y.surface.z * blend.y;
		ao += y.surface.w * blend.y;
	}
	normal = normalize(nsum);
	rough *= 2.0;
}

/*PRINTS{*//* the prints' field at a world point: the displacement (world units), the compaction, the ground's height,
the coverage; none where no page is, or where the texel's ground is not the pixel's (z: more than 0.3 world units
off, a bridge over the print or a floor under it) */
vec4 print_at(vec2 xy, float z, float lod)
{
	vec2 pc = floor(xy * prints0.x);
	uvec4 e = texelFetch(pages_texture, ivec2(mod(pc, prints0.w)), 0);
	if (e.x == 0u || int(e.y) != int(pc.x) || int(e.z) != int(pc.y))
		return vec4(0.0);
	vec2 local = xy / prints0.y - pc * prints0.z + 1.0;
	vec4 v = textureLod(prints_texture, vec3(local / prints3.z, float(e.x - 1u)), lod);
	float ground = uintBitsToFloat(e.w) + v.z / max(v.w, 1.0e-3);
	float own = clamp(v.w, 0.0, 1.0) * (1.0 - smoothstep(0.15, 0.3, abs(z - ground)));
	return vec4(v.x * own, v.y * own, ground, own);
}

/* the view ray (d, from the eye) through the prints about p (the flat surface's point, its footprint dpx, dpy):
the point met, the field's slope there, its depth, its compaction, the sun's visibility there (the field marched
toward the sun) and the cavity's occlusion; p and no print where there is none */
void print_march(vec3 p, vec3 d, vec3 dpx, vec3 dpy, float deepest, out vec2 hit, out vec2 slope, out float depth,
	out float pack, out float micro, out float cavity)
{
	hit = p.xy;
	slope = vec2(0.0);
	depth = 0.0;
	pack = 0.0;
	micro = 1.0;
	cavity = 1.0;
	float texel = prints0.y;
	float foot = max(length(dpx), length(dpy));
	float lod = clamp(log2(max(foot / texel, 1.0)), 0.0, prints3.w);
	float dz = min(d.z, -0.03);
	float top = prints3.y, bottom = -deepest;
	/* (the ray's span over the field's heights, from the highest rim to the deepest floor; nothing where neither
	end nor the pixel has a page) */
	vec2 a = p.xy + d.xy * (top / dz), b = p.xy + d.xy * (bottom / dz);
	float first = print_at(a, p.z, lod).x;
	if (print_at(p.xy, p.z, lod).w <= 0.0 && print_at(a, p.z, lod).w <= 0.0 && print_at(b, p.z, lod).w <= 0.0)
		return;
	int steps = int(clamp(length(b - a) / max(foot, texel * 1.5), 4.0, prints2.y));
	float last_h = top, last_f = first;
	vec2 last_xy = a, met = b;
	for (int i = 1; i <= 24; i++)
	{
		if (i > steps)
			break;
		float t = float(i) / float(steps);
		float h = mix(top, bottom, t);
		vec2 xy = mix(a, b, t);
		float f = print_at(xy, p.z, lod).x;
		if (h <= f)
		{
			/* (the crossing found between the last two steps by halving it thrice, then the secant) */
			vec2 xa = last_xy, xb = xy;
			float ha = last_h, hb = h, fa = last_f, fb = f;
			for (int j = 0; j < 3; j++)
			{
				vec2 xm = 0.5 * (xa + xb);
				float hm = 0.5 * (ha + hb), fm = print_at(xm, p.z, lod).x;
				if (hm <= fm)
				{
					xb = xm;
					hb = hm;
					fb = fm;
				}
				else
				{
					xa = xm;
					ha = hm;
					fa = fm;
				}
			}
			float d0 = ha - fa, d1 = hb - fb;
			met = mix(xa, xb, clamp(d0 / max(d0 - d1, 1.0e-6), 0.0, 1.0));
			break;
		}
		last_h = h;
		last_f = f;
		last_xy = xy;
	}
	hit = met;
	vec4 v = print_at(met, p.z, lod);
	if (v.w <= 0.0)
		return;
	depth = v.x;
	pack = v.y;
	float e = texel * exp2(lod);
	float hx = print_at(met + vec2(e, 0.0), p.z, lod).x - print_at(met - vec2(e, 0.0), p.z, lod).x;
	float hy = print_at(met + vec2(0.0, e), p.z, lod).x - print_at(met - vec2(0.0, e), p.z, lod).x;
	slope = vec2(hx, hy) / (2.0 * e);
	float mean = print_at(met, p.z, min(lod + 2.0, prints3.w)).x;
	cavity = 1.0 - 0.3 * smoothstep(0.0, 0.01, mean - v.x);
	if (prints1.w > 0.5 && prints1.z > 0.02)
	{
		float shade = 0.0;
		int count = int(prints2.z);
		for (int k = 1; k <= 12; k++)
		{
			if (k > count)
				break;
			float t = 0.09 * float(k) / float(count);
			float rise = v.x + prints1.z * t;
			float f = print_at(met + prints1.xy * t, p.z, lod).x;
			shade = max(shade, clamp((f - rise) / (texel * 0.75), 0.0, 1.0));
		}
		micro = 1.0 - shade;
	}
}

/*}PRINTS*/float oklab_l(vec3 c)
{
	/* (Oklab's lightness of a grey of the colour's luminance: its cube root; near enough for the cues) */
	vec3 lin = c * c;
	return pow(max(dot(lin, vec3(0.2126, 0.7152, 0.0722)), 0.0), 1.0 / 3.0);
}

/* the classes' physical table (design 3.3; T2, T3), by class (none, sand, dirt, mud, rock, snow, ice, water): the
dry roughness (the layer's variation over it; negative: the material's own kept), the porosity (T6's wetness reads
it; xgpu_vulkan_terrain.c's class_porosity is the same table), the light passes' model (XGPU_GBUFFER_MODEL_*; 0:
the material's own kept) and the specular scale the model's pixels carry times the layer's occlusion
(TERRAIN_COMMON's terrain_specular) */
const float TERRAIN_ROUGHNESS[8] = float[8](-1.0, 0.85, 0.90, 0.70, 0.70, 0.60, -1.0, -1.0);
const float TERRAIN_POROSITY[8] = float[8](0.0, 0.6, 0.7, 0.8, 0.3, 0.2, 0.0, 0.0);
const uint TERRAIN_MODEL[8] = uint[8](0u, 5u, 6u, 0u, 0u, 7u, 0u, 0u);
float terrain_specular(uint model)
{
	return model == 7u ? 0.45 : 0.5;
}

/* the words with a class's model, roughness and (where the model is the ground's) the specular byte: the
class's scale times the occlusion */
uvec4 classed(uvec4 w, uint cls, float r, float ao)
{
	uint model = TERRAIN_MODEL[cls];
	uint z = w.z;
	if (TERRAIN_ROUGHNESS[cls] >= 0.0)
		z = (z & 0xffffff00u) | uint(clamp(r, 0.02, 1.0) * 255.0 + 0.5);
	if (model != 0u)
	{
		z = (z & 0xf000ffffu) | (model << 24) | (uint(clamp(terrain_specular(model) * ao, 0.0, 1.0) * 255.0 + 0.5) << 16);
		/* (the ground is never metal: the byte the light passes would read as metalness) */
		z &= 0xffff00ffu;
	}
	return uvec4(w.x, w.y, z, w.w);
}

/* sand's wind ripples (design 3.4): crests across the map's wind (tangents.xy, its strength tangents.z), about 11 cm
apart, an asymmetric profile (the stoss side long, the lee short), their line warped by the macro map, in patches of
it (flat sand between); on near-level sand only, faded where the pixel's footprint nears the wavelength (no
aliasing) and on slopes: the normal's tilt along the wind */
vec2 ripple_tilt(vec3 p, vec3 n, float footprint)
{
	const float wavelength = 0.036;
	float level = smoothstep(0.86, 0.97, n.z);
	float sharp = 1.0 - smoothstep(wavelength * 0.08, wavelength * 0.3, footprint);
	if (level * sharp <= 0.0)
		return vec2(0.0);
	vec2 wind = tangents.xy;
	vec2 across = vec2(-wind.y, wind.x);
	vec4 warp = texture(macro_texture, p.xy * (1.0 / 1.7));
	float patches = smoothstep(0.3, 0.7, texture(macro_texture, p.xy * (1.0 / 9.0) + 0.37).b);
	float phase = dot(p.xy, wind) / wavelength + (warp.r - 0.5) * 5.0 + sin(dot(p.xy, across) / (wavelength * 4.3)) * 0.6;
	float x = fract(phase) * 6.2831853;
	float slope = sin(x) + 0.35 * sin(2.0 * x);
	return wind * (slope * 0.18 * tangents.z * level * sharp * patches);
}

/* (the frequency split's taps: the same material there; under local reads (TERRAIN_LAYERS_LOCAL) the words are
read only at the pixel, so the distance alone decides) */
bool tap_same(ivec2 tp, uint material, vec3 n)
{
	uvec4 tw = texelFetch(gbuffer_texture, tp, 0);
	/* (round 5: and facing the same way, within about 45 degrees: the ground where it meets the cliff is another
	face, whose colour a tap there smeared into the wall in a green haze, M1) */
	return (tw.w & 0xfffu) == material && dot(unfold(unpackSnorm2x16(tw.y)), n) > 0.7;
}

void main()
{
	ivec2 texel = ivec2(gl_FragCoord.xy);
	uvec4 w = texelFetch(gbuffer_texture, texel, 0);
	uint material = w.w & 0xfffu;
	if ((w.w & 0x8000u) == 0u || (w.w >> 16) != 0u || material == 0u)
		discard;
	uvec4 c = uvec4(texelFetch(classes_texture, ivec2(int(material & 63u), int(material >> 6)), 0) * 255.0 + 0.5);
	int la = int(c.b & 15u) - 1, lb = int(c.b >> 4) - 1;
	if (la < 0 && lb < 0)
		discard;
	uint ca = c.r, cb = c.g;
	if (la < 0)
	{
		la = lb;
		ca = cb;
	}
	if (lb < 0)
	{
		lb = la;
		cb = ca;
	}
	ca = min(ca, 7u);
	cb = min(cb, 7u);
	uvec4 x = texelFetch(extras_texture, texel, 0);
	float dist = uintBitsToFloat(x.w);
	float fade = 1.0 - smoothstep(bounds.y, bounds.z, dist);
	uint flags = (w.z >> 24) >> 4;
	if (!(dist > 0.0) || (flags & 8u) == 0u)
		discard;
	vec3 n = unfold(unpackSnorm2x16(w.y));
	vec4 base = texelFetch(color_texture, texel, 0);
	float weight = 0.0;
	if (lb != la)
	{
		float lightness = oklab_l(base.rgb);
		float span_l = cue(cb) - cue(ca);
		weight = abs(span_l) > 0.08 ? clamp((lightness - cue(ca)) / span_l, 0.0, 1.0) : 0.5;
		if (cb == 4u)
			weight = max(weight, smoothstep(0.75, 0.5, abs(n.z)));
		if (ca == 4u)
			weight = min(weight, smoothstep(0.5, 0.75, abs(n.z)));
	}
	/* (snow on the rock's tops (T3): on a map with snow (gamut.x), the faces turned up of a material of rock alone,
	where CE's lightmap light (the G-buffer's code: Tool's radiosity left overhangs and ledges under cover dark) says
	the sky reaches; gamut.yzw the snow's colour. Never on a material that blends rock with another class: there the
	rock's texels are CE's painted dirt in the snow (b40's brown streaks), which snow over them erased in hard-edged
	pale patches) */
	float snow_cover = 0.0;
	bool rock_here = ca == 4u && cb == 4u;
	if (gamut.x > 0.5 && rock_here)
	{
		float code = float(x.z >> 24) / 255.0;
		float sky = code > 0.0 ? smoothstep(0.2, 0.42, code) : 1.0;
		snow_cover = smoothstep(0.74, 0.9, n.z) * sky;
	}
	/* (beyond the HQ detail's reach: the class's model, roughness and colour of snow alone, so that the light
	passes' response does not end where the detail does) */
	if (fade <= 0.0)
	{
		uint far_class = weight >= 0.5 ? cb : ca;
		vec3 far_color = base.rgb;
		if (snow_cover > 0.0)
		{
			snow_cover = smoothstep(0.35, 0.65, snow_cover);
			far_color = mix(far_color, gamut.yzw, snow_cover);
			far_class = snow_cover > 0.5 ? 5u : far_class;
		}
		float rf = TERRAIN_ROUGHNESS[far_class];
		result = vec4(far_color, base.a);
		words_out = classed(w, far_class, rf >= 0.0 ? rf : float(w.z & 255u) / 255.0, 1.0);
/*PRINTS{*/		micro_out = vec4(1.0, 1.0, 0.0, 1.0);
/*}PRINTS*/		return;
	}
	vec4 corners = view_corners;
	vec2 span = max(corners.zw - corners.xy, vec2(1.0));
	vec2 sxy = 2.0 * (gl_FragCoord.xy - corners.xy) / span - 1.0;
	vec3 ray = view_axis.xyz + sxy.x * view_across.xyz + sxy.y * view_down.xyz;
	vec3 p = view_eye.xyz + dist * ray;
	/* (the point's change a pixel across and down, on the surface's plane: no derivative instructions, so that the
	discards above cost nothing) */
	vec3 rx = view_across.xyz * (2.0 / span.x), ry = view_down.xyz * (2.0 / span.y);
	/* (the plane: level where the ground is, not the bumped normal, whose noise would make the footprints
	noisy and the lookups streak) */
	vec3 plane = abs(n.z) >= 0.75 ? vec3(0.0, 0.0, n.z >= 0.0 ? 1.0 : -1.0) : n;
	float rn = dot(ray, plane);
	rn = abs(rn) < 0.05 ? (rn < 0.0 ? -0.05 : 0.05) : rn;
	vec3 dpx = dist * (rx - ray * (dot(rx, plane) / rn));
	vec3 dpy = dist * (ry - ray * (dot(ry, plane) / rn));
/*PRINTS{*/	/* (the prints: their ground's pixels near enough; the layers looked up where the ray meets them) */
	float print_micro = 1.0, print_cavity = 1.0, print_depth = 0.0, print_pack = 0.0;
	vec2 print_slope = vec2(0.0);
	bool printed = (c.r == 1u || c.r == 2u || c.r == 3u || c.r == 5u || c.g == 1u || c.g == 2u || c.g == 3u ||
		c.g == 5u) && dist < prints2.x && n.z > 0.6;
	if (printed)
	{
		vec2 hit;
		/* (the deepest a print goes: snow's prints far deeper than the rest's) */
		float deepest = c.r == 5u || c.g == 5u ? prints3.x : prints3.x * 0.5;
		print_march(p, normalize(ray), dpx, dpy, deepest, hit, print_slope, print_depth, print_pack, print_micro,
			print_cavity);
		p.xy = hit;
	}
/*}PRINTS*/	/* (under an upscaler the view's pixels are coarser than the screen's: window.x their ratio each way, 1 without
	one. The layers' footprint is the screen's pixel's, as the game's mip bias (log2 of the ratio) samples CE's own
	maps, so the layers and the grain keep the detail the upscaler's jittered history gives back, not the view's
	coarser levels) */
	float sharp = window.x > 0.0 && window.x < 1.0 ? window.x : 1.0;
	dpx *= sharp;
	dpy *= sharp;
	vec4 detail, detail_b;
	vec3 normal, normal_b;
	float rough, rough_b, ao, ao_b;
	/* (the surface (normal, roughness) only near, where it is drawn; one class alone where the other has
	little of the pixel) */
	float near = 1.0 - smoothstep(1.0, 6.0, dist);
	bool surfaced = near > 0.0;
	float fine = 1.0 - smoothstep(1.0, 12.0, dist);
	uint cls = ca;
	if (lb != la && weight >= 0.85)
	{
		la = lb;
		cls = cb;
	}
	layer_at(la, p, dpx, dpy, n, surfaced, fine, detail, normal, rough, ao);
	if (lb != la && weight > 0.15)
	{
		layer_at(lb, p, dpx, dpy, n, surfaced, fine, detail_b, normal_b, rough_b, ao_b);
		float hb = smoothstep(-0.25, 0.25, (detail_b.a + weight * 1.4) - (detail.a + (1.0 - weight) * 1.4));
		detail = mix(detail, detail_b, hb);
		normal = normalize(mix(normal, normal_b, hb));
		rough = mix(rough, rough_b, hb);
		ao = mix(ao, ao_b, hb);
		weight = hb;
		la = hb > 0.5 ? lb : la;
		cls = hb > 0.5 ? cb : ca;
	}
	vec4 a = layer_a(la), b = layer_b(la);
	float rep_index = floor(tangents.w);
	float rep = tangents.w - rep_index;
	/* (only on a material of rock alone: on one that blends rock with another class (b30's cliff foot into grass)
	CE's mean smeared the blend's edge into a haze over both) */
	bool split = rep > 0.0 && float(la) == rep_index && rock_here;
	/* (round 5, the split's layer alone: its detail held to 30 world units and gone by the pass's far end (60), not
	faded from 15 as the rest, so that the scan, not CE's painted streaks, carries the cliffs at 10 to 40 units; and
	the faces turned up (shelves, a chunk's top) given less of it: they face the sky and caught its sheen) */
	if (split)
		fade = max(fade, 1.0 - smoothstep(30.0, bounds.z, dist));
	float up = split ? smoothstep(0.3, 0.75, n.z) : 0.0;
	/* (the frequency split, for the one layer whose pack asks (tangents.w: its index, plus the share): CE's colour
	kept under about a metre, its own finer paint (b30's cliffs: streaks smeared along a stretched v) replaced by
	the layer's detail. CE's local mean is taken from the base itself: twelve taps on an ellipse in the surface's
	plane (wider along the level than up the face, the streaks' way), each projected to the screen and kept only
	where the G-buffer there is the same material at the distance expected (no other surface's colour, no sky);
	fixed in the world, so it does not crawl as the camera moves. Fewer than six kept: less of it) */
	vec3 base_rgb = base.rgb;
	if (split)
	{
		vec3 t1 = abs(plane.z) > 0.9 ? normalize(cross(plane, vec3(1.0, 0.0, 0.0))) :
			normalize(cross(vec3(0.0, 0.0, 1.0), plane));
		vec3 t2 = cross(plane, t1);
		float aa = dot(view_axis.xyz, view_axis.xyz), ac = dot(view_across.xyz, view_across.xyz),
			ad = dot(view_down.xyz, view_down.xyz);
		vec3 sum = base.rgb;
		float kept = 1.0;
		/* (round 5: the ellipse widens with distance, 1x to 4 units, 2x by 20, as the far octave below takes over
		the band it leaves: CE's streaks replaced where they carry the look; wider left a blur the scan could not fill) */
		float widen = 1.0 + clamp((dist - 4.0) / 16.0, 0.0, 1.0);
		for (int i = 0; i < 12; i++)
		{
			float ang = float(i) * 0.5235988 + (i >= 6 ? 0.2617994 : 0.0);
			float rr = (i >= 6 ? 1.0 : 0.5) * widen;
			vec3 q = p + (t1 * (cos(ang) * 0.20) + t2 * (sin(ang) * 0.11)) * rr;
			vec3 dq = q - view_eye.xyz;
			float tq = dot(dq, view_axis.xyz) / aa;
			if (tq <= 0.05)
				continue;
			vec2 sq = vec2(dot(dq, view_across.xyz) / (tq * ac), dot(dq, view_down.xyz) / (tq * ad));
			vec2 px = corners.xy + (sq + 1.0) * 0.5 * span;
			if (any(lessThan(px, corners.xy)) || any(greaterThanEqual(px, corners.zw)))
				continue;
			ivec2 tp = ivec2(px);
			float d2 = uintBitsToFloat(texelFetch(extras_texture, tp, 0).w);
			if (!tap_same(tp, material, n) || !(abs(d2 - tq) < 0.04 * tq + 0.03))
				continue;
			sum += texelFetch(color_texture, tp, 0).rgb;
			kept += 1.0;
		}
		vec3 low = sum / kept;
		/* (round 5: none of it within 2 units, all of it past 10: up close CE's fine paint and the scan's detail
		together read best (round 4's S1 and D1, where the split had never run); at 10 to 40 units the streaks go) */
		base_rgb = mix(base.rgb, low, rep * fade * smoothstep(3.0, 7.0, kept) * smoothstep(2.0, 10.0, dist));
	}
	vec4 macro = texture(macro_texture, p.xy * (1.0 / 24.0));
	float k = a.y * fade * clamp(0.7 + (macro.r - 0.5) * 1.2 + (macro.g - 0.5) * 0.6, 0.35, 1.25);
/*PRINTS{*/	/* (a print's floor pressed smooth: the sand's grain and relief less where the foot packed it) */
	float print_press = printed ? clamp(max(-print_depth / 0.008, clamp(print_pack, 0.0, 1.0)), 0.0, 1.0) : 0.0;
	k *= 1.0 - 0.75 * print_press;
/*}PRINTS*/	vec3 ratio = detail.rgb * 2.0;
	float lum = max(dot(ratio, vec3(0.2126, 0.7152, 0.0722)), 1.0e-3);
	ratio = lum * mix(vec3(1.0), ratio / lum, a.z);
	if (split)
	{
		/* (round 5, the split's far octave: the same scan FAR_OCTAVE times larger (its own low pass with it, so the
		band of about 0.8 to 3 world units the near tile's high pass leaves out), turned and offset so the two never
		line up, mip-correct by its own footprint, blended in from 3 to 12 units: its lightness ratio times the
		near one's, which the mips flatten there anyway) */
		float wfar = smoothstep(3.0, 12.0, dist);
		if (wfar > 0.01)
		{
			const mat3 fm = mat3(0.8, -0.6, 0.0, 0.6, 0.8, 0.0, 0.0, 0.0, 1.0);
			const float FAR_OCTAVE = 3.5;
			vec4 fd;
			vec3 fnrm;
			float frough, fao;
			layer_at(la, fm * p / FAR_OCTAVE + vec3(5.17, 2.93, 1.31), fm * dpx / FAR_OCTAVE, fm * dpy / FAR_OCTAVE,
				fm * n, false, 0.0, fd, fnrm, frough, fao);
			vec3 rf = fd.rgb * 2.0;
			float lf = max(dot(rf, vec3(0.2126, 0.7152, 0.0722)), 1.0e-3);
			/* (its lightness's contrast raised (power 1.8): a scan's broad band is faint beside its fine one) */
			ratio *= pow(lf, 1.8 * wfar);
		}
		k *= 1.0 - 0.6 * up;
	}
	ratio = mix(vec3(1.0), ratio, k);
	/* (a strength past 1 carries a scan's darkest grains past black: below 0.3 the ratio rolls off toward 0 along
	an exponential that meets it there with its slope, so a pebble stays a dark grain and never a black hole) */
	ratio = max(ratio, 0.3 * exp(min(ratio - 0.3, vec3(0.0)) / 0.3));
	vec3 color = clamp(base_rgb * pow(ratio, vec3(1.0 / 2.2)), 0.0, 1.0);
	normal = normalize(mix(n, normal, fade * near * (1.0 - 0.6 * up)));
	/* (sand's ripples, over the HQ normal and beyond its reach, as far as the footprint resolves them) */
	if (cls == 1u && tangents.z > 0.0)
		normal = normalize(normal - vec3(ripple_tilt(p, n, max(length(dpx), length(dpy))), 0.0) * fade);
	float rd = TERRAIN_ROUGHNESS[cls];
	float r = rd >= 0.0 ? rd : float(w.z & 255u) / 255.0;
	r = clamp(r * mix(1.0, rough, b.x * fade * near), 0.02, 1.0);
	float occlusion = mix(1.0, clamp(ao, 0.0, 1.0), fade * near);
	if (snow_cover > 0.0)
	{
		/* (the snow settled where the rock's height is low among its faces turned up: its cracks and hollows first) */
		snow_cover = smoothstep(0.3, 0.7, snow_cover + (0.5 - detail.a) * 0.6);
		color = mix(color, gamut.yzw, snow_cover);
		normal = normalize(mix(normal, n, snow_cover * 0.7));
		r = mix(r, TERRAIN_ROUGHNESS[5], snow_cover);
		occlusion = mix(occlusion, 1.0, snow_cover);
		cls = snow_cover > 0.5 ? 5u : cls;
	}
/*PRINTS{*/	/* (a print's floor a little darker, the damp sand under the dry; snow packed greyer and smoother; the
	normal bent by the field's slope) */
	if (printed)
	{
		float snow = c.r == 5u ? 1.0 : 0.0;
		float under = clamp(-print_depth / 0.008, 0.0, 1.0);
		color *= mix(1.0, mix(0.80, 0.93, snow), max(under * (1.0 - snow), clamp(print_pack, 0.0, 1.0) * snow));
		color *= 1.0 + 0.07 * (1.0 - snow) * clamp(print_depth / 0.0015, 0.0, 1.0);
		r = clamp(r * mix(1.0, mix(1.06, 0.78, snow), clamp(print_pack, 0.0, 1.0)), 0.02, 1.0);
		normal = normalize(mix(normal, n, 0.6 * print_press));
		normal = normalize(normal + vec3(-print_slope, 0.0));
	}
	if (prints2.w > 0.5)
		color = vec3(print_micro, print_cavity, clamp(-print_depth * 60.0, 0.0, 1.0));
	micro_out = vec4(print_micro, print_cavity, 0.0, 1.0);
/*}PRINTS*/	if (bounds.w > 0.5)
		color = vec3(weight, fade, float(la) / 6.0);
	result = vec4(color, base.a);
	uvec4 o = classed(w, cls, r, occlusion);
	if (TERRAIN_ROUGHNESS[cls] < 0.0)
		o.z = (o.z & 0xffffff00u) | uint(r * 255.0 + 0.5);
	/* (round 5, the split's layer: the material's own specular scale and metal (the pack's, from CE's specular mask,
	smeared along CE's stretched coordinates over a remodel's tilted facets: pale blue-grey sheets of sky in Path
	Traced and Rebaked alike, D1) replaced by plain rock's: a dielectric of 4 % (the byte 0.5 of the scale's 8 %),
	times the layer's occlusion, never metal; the roughness kept at 0.6 at least) */
	if (split && (o.z >> 24 & 15u) == 0u)
	{
		uint spec = uint(clamp(0.5 * occlusion, 0.0, 1.0) * 255.0 + 0.5);
		uint rb = max(o.z & 255u, 153u);
		o.z = (o.z & 0xff000000u) | (spec << 16) | rb;
	}
	words_out = uvec4(w.x, packSnorm2x16(fold(normal)), o.z, w.w);
}
"""
PRINTS_BLOCK = re.compile(r"/\*PRINTS\{\*/(.*?)/\*\}PRINTS\*/", re.S)
TERRAIN_LAYERS_SOURCE = PRINTS_BLOCK.sub("", TERRAIN_LAYERS_RAW)
TERRAIN_PRINTS_SOURCE = PRINTS_BLOCK.sub(lambda match: match[1], TERRAIN_LAYERS_RAW)


# the same, where the device reads a target where it is drawn (dynamic rendering's local reads: Vulkan's
# xgpu_vulkan_post_terrain_layers, capabilities color_read): the G-buffer's words are the pass's first target,
# read where it is drawn (input attachment 0 of set 2) and written through a mask of their second and third words
# (the normal and the roughness), the base the second target, written whole (its alpha the copy's), so that the
# words are not copied.
def terrain_local(source: str) -> str:
    local = source.replace(
        "uniform usampler2D gbuffer_texture;\n",
        "layout(input_attachment_index = 0, set = 2, binding = 0) uniform usubpassInput words_input;\n").replace(
        "out vec4 result;\nlayout(location = 1) out uvec4 words_out;\n",
        "layout(location = 1) out vec4 base_out;\nlayout(location = 0) out uvec4 words_out;\n").replace(
        "\tuvec4 w = texelFetch(gbuffer_texture, texel, 0);\n", "\tuvec4 w = subpassLoad(words_input);\n").replace(
        "result = vec4(", "base_out = vec4(").replace(
        "\tuvec4 tw = texelFetch(gbuffer_texture, tp, 0);\n", "").replace(
        "\treturn (tw.w & 0xfffu) == material && dot(unfold(unpackSnorm2x16(tw.y)), n) > 0.7;\n", "\treturn true;\n")
    assert local.count("words_input") == 2 and "base_out" in local and "gbuffer_texture" not in local
    return local


TERRAIN_LAYERS_LOCAL_SOURCE = terrain_local(TERRAIN_LAYERS_SOURCE)
TERRAIN_PRINTS_LOCAL_SOURCE = terrain_local(TERRAIN_PRINTS_SOURCE)
assert "PRINTS" not in TERRAIN_LAYERS_SOURCE and "micro_out" in TERRAIN_PRINTS_LOCAL_SOURCE
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
    programs["terrain_layers"] = ("frag", TERRAIN_LAYERS_SOURCE)
    programs["terrain_layers_local"] = ("frag", TERRAIN_LAYERS_LOCAL_SOURCE)
    programs["terrain_prints"] = ("frag", TERRAIN_PRINTS_SOURCE)
    programs["terrain_prints_local"] = ("frag", TERRAIN_PRINTS_LOCAL_SOURCE)
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
