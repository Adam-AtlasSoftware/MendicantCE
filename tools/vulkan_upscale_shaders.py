#!/usr/bin/env python3
"""Build the Vulkan backend's frame upscaling passes (display.upscaler, docs/prism_plugin.md).

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_upscale_shaders.py
The generated C (port/linux/vulkan/xgpu_vulkan_upscale_shaders.c) embeds the SPIR-V of:

- FidelityFX Super Resolution 3.1's upscaler passes (port/third_party/fidelityfx-fsr3, AMD's GLSL, MIT),
  compiled as the SDK's own build compiles them for Vulkan (glslang, compute, Vulkan 1.2, FFX_GLSL) in one
  permutation: LDR colour, motion vectors at the render size and not jittered, reversed depth, the reference
  Lanczos for the history's reprojection (the SDK's choice on GPUs of 32 lanes), 32-bit floats; the accumulation
  twice, without and with sharpening (RCAS then writes the output). xgpu_vulkan_fsr3.c runs them.
- INPUTS (below, the renderer's own): the view's colour, depth, motion and reactive mask for an upscaler, at the
  render size from the view's corner: the colour as it is; the depth (reversed, as the backends keep it); the motion
  from the G-buffer's second target where its pass drew the pixel with history, else the camera's from the depth
  (none on the first-person weapon, which the stencil marks: it moves with the camera); the reactive mask where the
  transparent surfaces changed the colour (FSR's own generation's rule: the largest change of the tone-mapped
  colour, 0.9 past 0.2, else 0). xgpu_vulkan_upscale.c runs it.

--check verifies that the checked-in file still matches its sources. glslang 16.6.0 and SPIRV-Tools produce the
checked-in words.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FSR3 = ROOT / "port/third_party/fidelityfx-fsr3"
GPU = FSR3 / "include/FidelityFX/gpu"
PASSES = FSR3 / "src/backends/vk/shaders/fsr3upscaler"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_upscale_shaders.c"

# the permutation (the SDK's CMakeCompileFSR3UpscalerShaders.txt: its base arguments, and one value of each
# permutation's)
FSR3_DEFINES = [
    "FFX_GPU=1", "FFX_GLSL=1",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1",
]
# the programs, in xgpu_vulkan_upscale_shaders.h's order: (name, pass file, sharpening)
FSR3_PROGRAMS = [
    ("fsr3_prepare_inputs", "prepare_inputs", 0),
    ("fsr3_luma_pyramid", "luma_pyramid", 0),
    ("fsr3_shading_change_pyramid", "shading_change_pyramid", 0),
    ("fsr3_shading_change", "shading_change", 0),
    ("fsr3_prepare_reactivity", "prepare_reactivity", 0),
    ("fsr3_luma_instability", "luma_instability", 0),
    ("fsr3_accumulate", "accumulate", 0),
    ("fsr3_accumulate_sharpen", "accumulate", 1),
    ("fsr3_rcas", "rcas", 1),
]

INPUTS_SOURCE = r"""#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D view_color;
layout(set = 0, binding = 1) uniform sampler2D view_opaque;
layout(set = 0, binding = 2) uniform sampler2D view_depth;
layout(set = 0, binding = 3) uniform usampler2D view_stencil;
layout(set = 0, binding = 4) uniform usampler2D gbuffer_extras;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D color_out;
layout(set = 0, binding = 6, r32f) uniform writeonly image2D depth_out;
layout(set = 0, binding = 7, rg16f) uniform writeonly image2D motion_out;
layout(set = 0, binding = 8, r8) uniform writeonly image2D reactive_out;
layout(set = 0, binding = 10) uniform usampler2D gbuffer_words;
layout(set = 0, binding = 11) uniform sampler2D gbuffer_base;
layout(set = 0, binding = 12, rgba8) uniform writeonly image2D diffuse_out;
layout(set = 0, binding = 13, rgba8) uniform writeonly image2D specular_out;
layout(set = 0, binding = 14, rgba16f) uniform writeonly image2D normal_out;
layout(set = 0, binding = 9, std140) uniform Inputs
{
    // the view's left, top, width and height in its targets' pixels
    ivec4 rect;
    // the jitter (pixels), the flags (1 the G-buffer's motion, 2 history, 4 the opaque colour kept, 8 the
    // G-buffer's guides wanted), unused
    vec4 jitter;
    // the camera this frame (SCENE_VIEW's: the eye and near, the axis at the middle and far, its change a unit of
    // the screen's x and y)
    vec4 now[4];
    // last frame's eye, and the inverse of its axis and changes (rows), which give its screen's coordinates
    vec4 last_eye;
    vec4 last_inverse[3];
    // last frame's view: left, top, width, height
    vec4 last_rect;
    // the reactive mask's threshold, its value past it, the scale, unused
    vec4 reactive;
} u;

float largest(vec3 c)
{
    return max(c.r, max(c.g, c.b));
}

vec3 tone(vec3 c)
{
    return c / (1.0 + largest(c));
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= u.rect.z || pixel.y >= u.rect.w)
        return;
    ivec2 at = u.rect.xy + pixel;
    uint flags = uint(u.jitter.z);
    vec4 view = texelFetch(view_color, at, 0);
    vec3 color = view.rgb;
    float depth = texelFetch(view_depth, at, 0).r;
    imageStore(color_out, pixel, view);
    imageStore(depth_out, pixel, vec4(depth));

    vec2 motion = vec2(0.0);
    bool found = false;
    if ((flags & 1u) != 0u)
    {
        uvec4 extras = texelFetch(gbuffer_extras, at, 0);
        // (the motion's history bit: word 2, XGPU_GBUFFER_EXTRA_HISTORY)
        if ((extras.z & 0x10000u) != 0u)
        {
            motion = unpackHalf2x16(extras.x);
            found = true;
        }
    }
    if (!found && (flags & 2u) != 0u && (texelFetch(view_stencil, at, 0).r & 1u) == 0u)
    {
        // where the pixel's surface is: its place on the screen this frame (the jitter taken away), its distance
        // along the view's axis from the reversed depth (D3D's [0, 1] between the near and far planes)
        vec2 s = (vec2(pixel) + 0.5 - u.jitter.xy) / vec2(u.rect.zw) * 2.0 - 1.0;
        float near = u.now[0].w, far = u.now[1].w;
        float z = near * far / (far - (1.0 - depth) * (far - near));
        vec3 world = u.now[0].xyz + (u.now[1].xyz + s.x * u.now[2].xyz + s.y * u.now[3].xyz) * z;
        vec3 v = world - u.last_eye.xyz;
        vec3 l = vec3(dot(u.last_inverse[0].xyz, v), dot(u.last_inverse[1].xyz, v), dot(u.last_inverse[2].xyz, v));
        if (l.x > 1e-4)
        {
            vec2 last = (l.yz / l.x + 1.0) * 0.5 * u.last_rect.zw;
            motion = last - (vec2(pixel) + 0.5 - u.jitter.xy);
        }
    }
    imageStore(motion_out, pixel, vec4(motion, 0.0, 0.0));

    float reactive = 0.0;
    if ((flags & 4u) != 0u)
    {
        vec3 before = tone(texelFetch(view_opaque, at, 0).rgb);
        float change = largest(abs(tone(color) - before)) * u.reactive.z;
        reactive = change < u.reactive.x ? 0.0 : u.reactive.y;
    }
    imageStore(reactive_out, pixel, vec4(reactive));

    if ((flags & 8u) != 0u)
    {
        // the guides a denoiser that upscales asks for, from the G-buffer (xgpu_stream.h's GBUFFER_BEGIN): the
        // diffuse albedo (an object's base colour in word 0, the level's the base target's), the specular albedo
        // (F0: the specular scale's dielectric, the base colour where metal), the shading normal (word 1,
        // octahedral) and roughness (word 2); where the pass drew nothing (the sky), white, none, the view's
        uvec4 words = texelFetch(gbuffer_words, at, 0);
        vec3 diffuse = vec3(1.0), specular = vec3(0.0), normal = -normalize(u.now[1].xyz);
        float roughness = 1.0;
        if ((words.w & 0x8000u) != 0u)
        {
            vec4 surface = unpackUnorm4x8(words.z);
            vec3 base = (words.w >> 16) == 0u ? texelFetch(gbuffer_base, at, 0).rgb : unpackUnorm4x8(words.x).rgb;
            float metal = surface.y;
            diffuse = base * (1.0 - metal);
            specular = mix(vec3(0.08 * surface.z), base, metal);
            roughness = surface.x;
            if ((((words.z >> 24) >> 4) & 8u) != 0u)
            {
                vec2 e = unpackSnorm2x16(words.y);
                vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
                if (n.z < 0.0)
                    n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
                normal = normalize(n);
            }
        }
        imageStore(diffuse_out, pixel, vec4(diffuse, 1.0));
        imageStore(specular_out, pixel, vec4(specular, 1.0));
        imageStore(normal_out, pixel, vec4(normal, roughness));
    }
}
"""


def words_array(name: str, data: bytes) -> str:
    data += b"\0" * (-len(data) % 4)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"static const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def compile_program(args, name: str, source: Path, defines: list[str], includes: list[Path]) -> bytes:
    spv = args.build_dir / f"{name}.spv"
    spv.unlink(missing_ok=True)
    command = [args.compiler, "-V", "--target-env", "vulkan1.2", "-S", "comp", "-o", str(spv)]
    command += [f"-D{define}" for define in defines]
    command += [f"-I{include}" for include in includes]
    command.append(str(source))
    subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    subprocess.run([args.validator, "--target-env", "vulkan1.2", str(spv)], check=True)
    return spv.read_bytes()


def generate(args):
    args.build_dir.mkdir(parents=True, exist_ok=True)
    programs = []
    for name, file, sharpen in FSR3_PROGRAMS:
        source = PASSES / f"ffx_fsr3upscaler_{file}_pass.glsl"
        defines = FSR3_DEFINES + [f"FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={sharpen}"]
        programs.append((name, compile_program(args, name, source, defines, [GPU, GPU / "fsr3upscaler"])))
    inputs = args.build_dir / "upscale_inputs.comp"
    inputs.write_text(INPUTS_SOURCE)
    programs.append(("upscale_inputs", compile_program(args, "upscale_inputs", inputs, [], [])))
    text = [
        "/* Generated by tools/vulkan_upscale_shaders.py from port/third_party/fidelityfx-fsr3 (AMD's FidelityFX "
        "Super Resolution 3.1, MIT: its LICENSE.txt) and its own GLSL. Do not edit. */\n",
        '#include "xgpu_vulkan_upscale_shaders.h"\n',
    ]
    for name, words in programs:
        text.append(words_array(f"{name}_comp", words))
        print(f"{name}: {len(words)} bytes of SPIR-V")
    text.append("const struct xgpu_vulkan_upscale_program "
                "xgpu_vulkan_upscale_programs[XGPU_VULKAN_UPSCALE_PROGRAM_COUNT] =\n{")
    for name, _ in programs:
        text.append(f"\t{{ {name}_comp, sizeof({name}_comp) / 4 }},")
    text.append("};\n")
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{len(programs)} modules validated for Vulkan 1.2")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_upscale_shaders")
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
