#!/usr/bin/env python3
"""Build PRISM Path Traced's compute shaders (display.lighting "pathtraced", port/linux/vulkan/xgpu_vulkan_pt.c) from
their GLSL (port/linux/vulkan/xgpu_vulkan_pt.glsl).

Run with a C preprocessor (cc), glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_pt_shaders.py
Each pass is the baker's BAKE_COMMON (port/linux/vulkan/xgpu_vulkan_bake_glsl.inc: the bake's scene and its light
functions, which the C preprocessor gives) and EXPOSURE_COMMON (the exposure's meter and the tone curve), then the file's common part ("//@common"), then the pass's own ("//@pass
<name>"), so that the path tracer traces the bake's scene by the bake's own functions. The generated C embeds SPIR-V;
the game does not need a shader compiler. --check verifies that the checked-in words still match their source.
"""

from __future__ import annotations

import argparse
import ast
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/vulkan/xgpu_vulkan_pt.glsl"
BAKE = ROOT / "port/linux/vulkan/xgpu_vulkan_bake_glsl.inc"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_pt_shaders.c"
NAMES = ("light", "temporal", "fix", "atrous", "composite", "reference", "reference_stats", "lamps", "cache",
         "water")
DECLARATION = re.compile(r"static const char (\w+)_source\[\]\s*=\s*((?:\s|\"(?:\\.|[^\"\\])*\")+);", re.S)
STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
MARK = re.compile(r"^//@(common|pass (\w+))\s*$", re.M)


def bake_common() -> str:
    """the baker's BAKE_COMMON, then its EXPOSURE_COMMON (the exposure's meter, its read and the tone curve, which
    Raster's frame and the composite share), as their GLSL text"""
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        raise SystemExit("no C preprocessor (cc) on PATH")
    probe = (f'#include "{BAKE}"\nstatic const char bake_common_source[] = BAKE_COMMON;\n'
             'static const char exposure_common_source[] = EXPOSURE_COMMON;\n'
             'static const char terrain_common_source[] = TERRAIN_COMMON;\n')
    text = subprocess.run([cc, "-E", "-P", "-x", "c", "-"], input=probe, check=True, capture_output=True,
                          text=True).stdout
    found = {}
    for match in DECLARATION.finditer(text):
        if match.group(1) in ("bake_common", "exposure_common", "terrain_common"):
            found[match.group(1)] = "".join(ast.literal_eval(s) for s in STRINGS.findall(match.group(2)))
    for name in ("bake_common", "exposure_common", "terrain_common"):
        if name not in found:
            raise SystemExit(f"{BAKE}: no {name.upper()}")
    return objects_traced(found["bake_common"]) + found["exposure_common"] + found["terrain_common"]


# the moving objects (PRISM v4 M3, xgpu_vulkan_rt.c's objects for "pathtraced"): BAKE_COMMON's visibility tests
# renamed to the structure's alone and wrapped, so that every test the path tracer makes through them (its lamps',
# fixtures', sun's and domes', the bake's estimators it shares) sees the frame's objects too (objects_blocked,
# xgpu_vulkan_pt.glsl); the baker's own text is not changed
VISIBILITY = (
    ("bool blocked(vec3 o, vec3 d, float t_max)\n{", "blocked", "t_max"),
    ("bool sky_blocked(vec3 o, vec3 d, float reach)\n{", "sky_blocked", "reach"),
)


def objects_traced(common: str) -> str:
    head = "bool objects_blocked(vec3 o, vec3 d, float t_max);\n"
    for signature, name, last in VISIBILITY:
        if common.count(signature) != 1:
            raise SystemExit(f"{BAKE}: BAKE_COMMON's {name} is not where it was")
        renamed = signature.replace(f"bool {name}(", f"bool {name}_structure(")
        wrapper = (f"{renamed.split(chr(10))[0]};\n"
                   f"bool {name}(vec3 o, vec3 d, float {last})\n"
                   f"{{\n    return {name}_structure(o, d, {last}) || objects_blocked(o, d, {last});\n}}\n")
        common = common.replace(signature, head + wrapper + renamed)
        head = ""
    return common


def sources() -> dict[str, str]:
    text = SOURCE.read_text()
    marks = list(MARK.finditer(text))
    common, passes = None, {}
    for index, mark in enumerate(marks):
        end = marks[index + 1].start() if index + 1 < len(marks) else len(text)
        body = text[mark.end():end]
        if mark.group(1) == "common":
            common = body
        else:
            passes[mark.group(2)] = body
    if common is None:
        raise SystemExit(f"{SOURCE}: no //@common")
    missing = [name for name in NAMES if name not in passes]
    if missing:
        raise SystemExit(f"{SOURCE}: no //@pass {', '.join(missing)}")
    head = bake_common()
    # (the path tracer's own extensions beside the baker's: the subgroups' ballot and arithmetic, for the dynamic
    # lights' culling by each subgroup's points, dynamic_change)
    first = "#extension GL_EXT_ray_query : require\n"
    if head.count(first) != 1:
        raise SystemExit(f"{BAKE}: BAKE_COMMON's extensions are not where they were")
    head = head.replace(first, first + "#extension GL_KHR_shader_subgroup_ballot : require\n"
                        "#extension GL_KHR_shader_subgroup_arithmetic : require\n")
    return {name: head + "\n#line 1 1\n" + common + "\n#line 1 2\n" + passes[name] for name in NAMES}


def compile_words(name: str, glsl: str) -> list[int]:
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as folder:
        spv = Path(folder) / f"{name}.spv"
        result = subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.3", "--stdin", "-S", "comp",
                                 "-o", str(spv)], input=glsl.encode(), capture_output=True)
        if result.returncode:
            raise SystemExit(f"{name}: {result.stdout.decode()}{result.stderr.decode()}")
        subprocess.run(["spirv-val", "--target-env", "vulkan1.3", str(spv)], check=True)
        data = spv.read_bytes()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def generate() -> str:
    lines = ["/* Generated by tools/vulkan_pt_shaders.py from xgpu_vulkan_pt.glsl and the baker's BAKE_COMMON. Do not "
             "edit. */", "", '#include "xgpu_vulkan_pt_shaders.h"', ""]
    for name, glsl in sources().items():
        words = compile_words(name, glsl)
        lines.append(f"const uint32_t xgpu_vulkan_pt_{name}_words[] =")
        lines.append("{")
        for start in range(0, len(words), 8):
            lines.append("\t" + " ".join(f"0x{w:08x}," for w in words[start:start + 8]))
        lines.append("};")
        lines.append(f"const size_t xgpu_vulkan_pt_{name}_count = sizeof(xgpu_vulkan_pt_{name}_words) / "
                     f"sizeof(xgpu_vulkan_pt_{name}_words[0]);")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    (ROOT / "build").mkdir(exist_ok=True)
    text = generate()
    if arguments.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != text:
            raise SystemExit(f"{OUTPUT} differs from its source: run tools/vulkan_pt_shaders.py")
        return
    OUTPUT.write_text(text)


if __name__ == "__main__":
    main()
