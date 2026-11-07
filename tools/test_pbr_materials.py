"""display.prism_materials' shaders (port/linux/src/xgpu.h's "PBR materials"): the SPIR-V emitter's
pixel shaders of a model lit for each pixel with a material (tools/pbr_materials/check.c), for every
material_pbr a key may have, lit by the point lights or the enhanced dynamic lights, with and without
the material overlay and the reflection cube, pass spirv-val; each reads the material's images at
bindings 11 to 16 of the textures' set exactly where its bits say, and its uniform's members; the
shaders without a material read none of it. display.prism_models' wide variant (a mesh store mesh drawn
with a material): its pixel shaders read the mesh's tangent and uv0 (xMeshTangent, xMeshUv), its
vertex shader hands them on from the wide stream's input (location 15), a program reading that input
itself gets no variant, and the shaders without the variant name none of it."""
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src"
CC = shutil.which("cc") or shutil.which("clang")
VALIDATOR = shutil.which("spirv-val")
SHARED = ["xgpu.h", "xgpu_backend.h", "xgpu_hdr.h", "xgpu_shader.h", "xgpu_msl.h", "xgpu_spirv.h", "xgpu_stream.h",
          "nv2a_vsh.c", "nv2a_psh.c", "xgpu_text.c", "xgpu_msl.c", "xgpu_spirv.c"]
# (SPIR-V's OpDecorate and its Binding, DescriptorSet decorations; OpVariable)
OP_DECORATE, DECORATION_BINDING, DECORATION_SET = 71, 33, 34
SET_TEXTURES = 1


@pytest.fixture(scope="module")
def modules(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    build = tmp_path_factory.mktemp("pbr_materials")
    (build / "src").mkdir()
    for name in SHARED:
        shutil.copy(SOURCE / name, build / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (build / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = build / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{build}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{build / 'src'}", "-o", str(executable), str(ROOT / "tools/pbr_materials/check.c")] +
                   [str(build / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    out = build / "modules"
    out.mkdir()
    subprocess.run([str(executable), "spirv", str(out)], check=True)
    return {path.stem: path.read_bytes() for path in out.glob("*.spv")}


def texture_bindings(words):
    """the bindings of the textures' set a module's variables have"""
    decorations = {}
    index = 5
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xffff
        if opcode == OP_DECORATE and count >= 4:
            decorations.setdefault(words[index + 1], {})[words[index + 2]] = words[index + 3]
        index += max(count, 1)
    return sorted(d[DECORATION_BINDING] for d in decorations.values()
                  if d.get(DECORATION_SET) == SET_TEXTURES and DECORATION_BINDING in d)


def words_of(module):
    return struct.unpack(f"<{len(module) // 4}I", module)


def test_cases_made(modules):
    # 2 lights x 2 overlays x (1 without a material + 2 cubes x 128 bits + 128 wide), 3 vertex shaders
    assert len(modules) == 2 * 2 * (1 + 2 * 128 + 128) + 3


def test_wide_variant(modules):
    for name, module in modules.items():
        if name.startswith("wide"):
            assert b"xMeshTangent" in module and b"xMeshUv" in module, name
        elif name in ("vertex-wide",):
            assert b"xMeshWide" in module and b"xMeshTangent" in module and b"xMeshUv" in module, name
        else:
            assert b"xMesh" not in module, name


def test_material_images_where_the_bits_say(modules):
    for name, module in modules.items():
        if name.startswith("vertex"):
            continue
        bindings = texture_bindings(words_of(module))
        material = [binding for binding in bindings if 11 <= binding <= 16]
        if name.startswith("base"):
            assert not material, name
            continue
        bits = int(name.split("-")[1])
        wanted = [11 + image for image in range(6) if bits & (2 << image)]
        assert material == wanted, name
        # (the stages' and, with the overlay's surface map, its two images)
        overlay = name.split("-")[3] == "1"
        assert [binding for binding in bindings if binding < 11] == ([0, 2, 3, 9, 10] if overlay else [0, 2, 3]) or \
            [binding for binding in bindings if binding < 11] == ([0, 2, 9, 10] if overlay else [0, 2]), name


def test_none_without_a_material(modules):
    # (no material's image named where the key has none)
    for name, module in modules.items():
        if name.startswith("base"):
            assert b"material_" not in module, name


def test_spirv_valid(modules, tmp_path):
    if not VALIDATOR:
        pytest.skip("no spirv-val")
    for name, module in sorted(modules.items()):
        path = tmp_path / f"{name}.spv"
        path.write_bytes(module)
        run = subprocess.run([VALIDATOR, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
        assert run.returncode == 0, name + ": " + run.stdout + run.stderr
