"""display.lighting's v4 modes' G-buffer twins (port/linux/src/xgpu_spirv.h's xgpu_spirv_options gbuffer;
tools/gbuffer_twins/check.c): the SPIR-V emitter's shaders of a skinned model's twin pass spirv-val. An object's
twin's vertex shader with gbuffer_previous runs its program a second time on the last frame's constants (a uniform
block at set 0, binding XGPU_SPIRV_BINDING_LAST_CONSTANTS) and hands on both clip positions (locations
XGPU_SPIRV_LOCATION_PREVIOUS and _CURRENT), which its pixel shader reads; without it, and the level's, none of them."""
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
# (SPIR-V's OpDecorate and its Location, Binding, DescriptorSet decorations; OpVariable and its storage classes)
OP_DECORATE, OP_VARIABLE = 71, 59
DECORATION_LOCATION, DECORATION_BINDING, DECORATION_SET = 30, 33, 34
STORAGE_INPUT, STORAGE_UNIFORM, STORAGE_OUTPUT = 1, 2, 3
# (a model's skinned program of the game's: its microcode, by its entry in the vertex shaders' table)
SKINNED_PROGRAM = 10


def program_words(index):
    """the microcode of the game's vertex program at index (tools/render_manifest.py's reading)"""
    source = ROOT / "source/rasterizer/xbox"
    shaders = (source / "rasterizer_xbox_vertex_shaders.c").read_text()
    data = (source / "rasterizer_xbox_vertex_shaders_data.inc").read_text()
    code = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]{8})", data.split("*/", 1)[1])]
    entries = [(int(offset, 16), int(size, 16)) for offset, size in
               re.findall(r"VERTEX_SHADER_ENTRY\((0x[0-9A-F]+), (0x[0-9A-F]+)\)", shaders)]
    offset, _ = entries[index]
    count = code[offset // 4] >> 16
    return code[offset // 4 + 1:offset // 4 + 1 + count * 4]


def enumerator(name):
    """an enumerator of xgpu_spirv.h's (its position in its enum, counting from its first)"""
    text = (SOURCE / "xgpu_spirv.h").read_text()
    match = re.search(r"#define " + name + r" (\d+)", text)
    if match:
        return int(match.group(1))
    body = re.search(r"enum\s*\{([^}]*\b" + name + r"\b[^}]*)\}", text).group(1)
    names = [part.strip() for part in re.sub(r"/\*.*?\*/", "", body, flags=re.S).split(",") if part.strip()]
    return names.index(name)


@pytest.fixture(scope="module")
def modules(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    build = tmp_path_factory.mktemp("gbuffer_twins")
    (build / "src").mkdir()
    for name in SHARED:
        shutil.copy(SOURCE / name, build / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (build / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = build / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{build}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{build / 'src'}", "-o", str(executable), str(ROOT / "tools/gbuffer_twins/check.c")] +
                   [str(build / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    microcode = build / "program.bin"
    words = program_words(SKINNED_PROGRAM)
    microcode.write_bytes(struct.pack(f"<{len(words)}I", *words))
    out = build / "modules"
    out.mkdir()
    subprocess.run([str(executable), "spirv", str(microcode), str(out)], check=True)
    return {path.stem: path for path in out.glob("*.spv")}


def interface(path):
    """a module's variables: {id: (storage class, decorations)}"""
    data = path.read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    decorations, variables = {}, {}
    index = 5
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xffff
        if opcode == OP_DECORATE and count >= 4:
            decorations.setdefault(words[index + 1], {})[words[index + 2]] = words[index + 3]
        elif opcode == OP_VARIABLE and count >= 4:
            variables[words[index + 2]] = words[index + 3]
        index += max(count, 1)
    return {variable: (storage, decorations.get(variable, {})) for variable, storage in variables.items()}


def locations(path, storage):
    return sorted(d[DECORATION_LOCATION] for s, d in interface(path).values() if s == storage and DECORATION_LOCATION in d)


def bindings(path):
    return sorted((d.get(DECORATION_SET), d[DECORATION_BINDING]) for s, d in interface(path).values()
                  if s == STORAGE_UNIFORM and DECORATION_BINDING in d)


def test_modules_are_valid(modules):
    assert set(modules) == {"vertex-object-previous", "vertex-object", "vertex-level", "pixel-object-previous",
                            "pixel-level"}
    if not VALIDATOR:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for path in modules.values():
        subprocess.run([VALIDATOR, "--target-env", "vulkan1.1", str(path)], check=True)


def test_the_last_frames_constants_and_clip_positions(modules):
    last = enumerator("XGPU_SPIRV_BINDING_LAST_CONSTANTS")
    previous, current = enumerator("XGPU_SPIRV_LOCATION_PREVIOUS"), enumerator("XGPU_SPIRV_LOCATION_CURRENT")
    assert (0, last) in bindings(modules["vertex-object-previous"])
    assert {previous, current} <= set(locations(modules["vertex-object-previous"], STORAGE_OUTPUT))
    assert {previous, current} <= set(locations(modules["pixel-object-previous"], STORAGE_INPUT))
    for name in ("vertex-object", "vertex-level"):
        assert (0, last) not in bindings(modules[name])
        assert not {previous, current} & set(locations(modules[name], STORAGE_OUTPUT))
    assert not {previous, current} & set(locations(modules["pixel-level"], STORAGE_INPUT))
