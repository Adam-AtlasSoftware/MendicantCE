"""PRISM v4 M4's G-buffer uber-shaders (port/linux/src/xgpu_spirv.c's gbuffer_pack; docs/prism_materials.md): the
SPIR-V emitter's G-buffer twins of a level's lightmap pass and of a model's draw, with a material's maps and without,
with the vertices' normal and without, and the level's vertex shader handing on the base map's coordinates or not
(tools/gbuffer_pack/check.c), pass spirv-val; a twin of the maps reads its six images at the material bindings
(12 to 17 of the textures' set) and writes the albedo target (location 2), and no other twin does either; the level's
twin reads the base map's coordinates exactly where its vertex shader hands them on. No game data and no GPU."""
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
WORK = ROOT / "build" / "test_gbuffer_pack"
# (SPIR-V's OpDecorate, its Location, Binding and DescriptorSet decorations; OpVariable and its storage classes)
OP_DECORATE, OP_VARIABLE = 71, 59
DECORATION_LOCATION, DECORATION_BINDING, DECORATION_SET = 30, 33, 34
STORAGE_INPUT, STORAGE_OUTPUT = 1, 3
SET_TEXTURES = 1
# (xgpu_spirv.h's XGPU_SPIRV_LOCATION_BASE_UV: after the light's two)
LOCATION_BASE_UV = 13


@pytest.fixture(scope="module")
def modules():
    if not CC:
        pytest.skip("no C compiler")
    shutil.rmtree(WORK, ignore_errors=True)
    (WORK / "src").mkdir(parents=True)
    for name in SHARED:
        shutil.copy(SOURCE / name, WORK / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (WORK / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = WORK / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{WORK}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{WORK / 'src'}", "-o", str(executable), str(ROOT / "tools/gbuffer_pack/check.c")] +
                   [str(WORK / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    out = WORK / "modules"
    out.mkdir()
    subprocess.run([str(executable), "spirv", str(out)], check=True)
    return {path.stem: path.read_bytes() for path in out.glob("*.spv")}


def interface(module):
    """a module's texture bindings (of the textures' set) and its inputs' and outputs' locations"""
    words = struct.unpack(f"<{len(module) // 4}I", module)
    decorations, storage = {}, {}
    index = 5
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xffff
        if opcode == OP_DECORATE and count >= 4:
            decorations.setdefault(words[index + 1], {})[words[index + 2]] = words[index + 3]
        elif opcode == OP_VARIABLE and count >= 4:
            storage[words[index + 2]] = words[index + 3]
        index += max(count, 1)
    bindings = sorted(d[DECORATION_BINDING] for d in decorations.values()
                      if d.get(DECORATION_SET) == SET_TEXTURES and DECORATION_BINDING in d)
    locations = {STORAGE_INPUT: set(), STORAGE_OUTPUT: set()}
    for variable, d in decorations.items():
        if DECORATION_LOCATION in d and storage.get(variable) in locations:
            locations[storage[variable]].add(d[DECORATION_LOCATION])
    return bindings, locations[STORAGE_INPUT], locations[STORAGE_OUTPUT]


def test_cases_made(modules):
    # (the object's 2 x 2, the level's 2 x 2 x 2; and the level's vertex shader, 2)
    assert len(modules) == 4 + 8 + 2


def test_spirv_valid(modules):
    if not VALIDATOR:
        pytest.skip("no spirv-val")
    for name, module in sorted(modules.items()):
        path = WORK / f"{name}.spv"
        path.write_bytes(module)
        run = subprocess.run([VALIDATOR, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
        assert run.returncode == 0, name + ": " + run.stdout + run.stderr


def test_maps_read_by_the_maps_twins_alone(modules):
    for name, module in modules.items():
        if name.startswith("vertex"):
            continue
        kind, pack, normal, base_uv = name.split("-")
        bindings, inputs, outputs = interface(module)
        material = [binding for binding in bindings if 12 <= binding <= 17]
        if pack == "1":
            assert material == list(range(12, 18)), name
            assert outputs == {0, 1, 2}, name
        else:
            assert not material, name
            assert outputs == {0, 1}, name
        # (the base map's coordinates read by the level's twin of the maps alone, where they are handed on and it
        # has the vertices' frame)
        wanted = kind == "level" and pack == "1" and base_uv == "1" and normal == "1"
        assert (LOCATION_BASE_UV in inputs) == wanted, name


def test_vertex_hands_on_the_base_map_coordinates(modules):
    for base_uv in (0, 1):
        _bindings, _inputs, outputs = interface(modules[f"vertex-{base_uv}"])
        assert (LOCATION_BASE_UV in outputs) == bool(base_uv)
