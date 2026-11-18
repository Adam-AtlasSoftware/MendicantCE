"""display.lighting's v4 modes' G-buffer twins (port/linux/src/xgpu_spirv.h's xgpu_spirv_options gbuffer;
tools/gbuffer_twins/check.c): the SPIR-V emitter's shaders of a skinned model's twin pass spirv-val. An object's
twin's vertex shader with gbuffer_previous runs its program a second time on the last frame's constants (a uniform
block at set 0, binding XGPU_SPIRV_BINDING_LAST_CONSTANTS) and hands on both clip positions (locations
XGPU_SPIRV_LOCATION_PREVIOUS and _CURRENT), which its pixel shader reads; without it, and the level's, none of them.
The last frame's constants the Vulkan backend makes for it (port/linux/vulkan/xgpu_vulkan_stream.c): the part's, its
nodes moved back as its instance's moved; a replacement mesh's pieces' (display.prism_models), node 0's identity moved
as the root moved; none of its own for the first-person weapon."""
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


def stream_function(text, name):
    """a function of xgpu_vulkan_stream.c's, taken by matching braces"""
    match = re.search(r"^static [\w *]+?\b" + re.escape(name) + r"\(", text, re.M)
    end = text.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


LAST_CONSTANTS_CHECK = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xgpu_stream.h"
typedef uint64_t VkBuffer;
typedef uint64_t VkDeviceSize;
struct xgpu_vulkan_range { VkBuffer buffer; VkDeviceSize offset; };
struct stream_pipeline { int unused; };
@DEFINES@
@INSTANCE@
static struct { struct { int32_t viewport[4]; } dynamic; float constants[CONSTANTS_RANGE / 16][4]; } stream;
static struct
{
    struct stream_pipeline *drawing;
    uint8_t kind;
    uint32_t instance;
    struct gbuffer_instance *instances;
    struct xgpu_stream_scene_view view;
    int view_known;
    uint32_t pass_number;
    float (*last_constants)[4];
    int previous;
    float last_projection[4][4];
    uint32_t last_projection_pass;
    float (*moves)[3][4];
    uint32_t moves_instance, moves_pass, move_count;
} gbuffer;
/* (the ring: each upload's bytes kept, its range numbered) */
static float ring[4][CONSTANTS_RANGE / 16][4];
static int uploads, ring_full;
static void *xgpu_vulkan_upload(uint64_t size, uint64_t alignment, struct xgpu_vulkan_range *range)
{
    assert(size == CONSTANTS_RANGE && alignment == 256 && uploads < 4);
    if (ring_full)
        return NULL;
    range->buffer = 9;
    range->offset = (VkDeviceSize)uploads * 4096;
    return ring[uploads++];
}
@CODE@

static struct gbuffer_instance instances[4];
static struct stream_pipeline twin;

/* the window as an object's twin finds it: the pass with history, the
instance (2) with node 0 moved 3 units along x since last frame and node 1
not at all */
static void window(uint32_t instance)
{
    int row, column;

    memset(&gbuffer, 0, sizeof(gbuffer));
    memset(instances, 0, sizeof(instances));
    memset(ring, 0, sizeof(ring));
    uploads = ring_full = 0;
    gbuffer.drawing = &twin;
    gbuffer.kind = XGPU_SPIRV_GBUFFER_OBJECT;
    gbuffer.instances = instances;
    gbuffer.instance = instance;
    gbuffer.view_known = 1;
    gbuffer.view.flags = XGPU_SCENE_VIEW_HISTORY;
    gbuffer.pass_number = 8;
    gbuffer.last_projection_pass = 7;
    for (row = 0; row < 4; row++)
        for (column = 0; column < 4; column++)
            gbuffer.last_projection[row][column] = (float)(100 + row * 4 + column);
    for (row = 0; row < CONSTANTS_RANGE / 16; row++)
        for (column = 0; column < 4; column++)
            stream.constants[row][column] = (float)(row * 4 + column) * 0.25f;
    /* (the constants' nodes 0 and 1: the identity, each placed) */
    for (row = 0; row < 6; row++)
        memset(stream.constants[60 + row], 0, sizeof(stream.constants[0]));
    stream.constants[60][0] = stream.constants[61][1] = stream.constants[62][2] = 1.0f;
    stream.constants[63][0] = stream.constants[64][1] = stream.constants[65][2] = 1.0f;
    stream.constants[60][3] = 10.0f;
    stream.constants[63][3] = 20.0f;
    stream.dynamic.viewport[2] = 640;
    stream.dynamic.viewport[3] = 480;
    instances[2].pass = 8;
    instances[2].node_count = 2;
    for (row = 0; row < 2; row++)
    {
        struct xgpu_stream_scene_node *node = &instances[2].nodes[row];

        memset(node, 0, sizeof(*node));
        node->now[0][0] = node->now[1][1] = node->now[2][2] = 1.0f;
        node->last[0][0] = node->last[1][1] = node->last[2][2] = 1.0f;
    }
    instances[2].nodes[0].now[0][3] = 5.0f;
    instances[2].nodes[0].last[0][3] = 2.0f;
}

int main(int argc, char **argv)
{
    float motion[XGPU_SPIRV_GBUFFER_UNIFORMS - 2][4];
    struct xgpu_vulkan_range range;
    const char *which = argc > 1 ? argv[1] : "";
    int row;

    if (!strcmp(which, "part"))
    {
        /* (a stock part's twin: the draw's constants, the projection the pass
        before's, each node moved back as its instance's node moved) */
        window(2);
        assert(gbuffer_previous_motion(motion) == 1 && gbuffer.previous == 1);
        assert(gbuffer_last_constants(&range, 0) == 1 && range.buffer == 9 && uploads == 1);
        assert(!memcmp(ring[0][0], gbuffer.last_projection, sizeof(gbuffer.last_projection)));
        assert(ring[0][60][3] == 7.0f && ring[0][60][0] == 1.0f && ring[0][63][3] == 20.0f);
        assert(!memcmp(ring[0][4], stream.constants[4], (60 - 4) * sizeof(ring[0][0])));
        return 0;
    }
    if (!strcmp(which, "pieces"))
    {
        /* (a replacement mesh's pieces: their vertices in the world, node 0's
        matrix the identity in their constants: last frame's that identity
        moved as the root moved, 3 units back along x; the rest the part's) */
        window(2);
        assert(gbuffer_previous_motion(motion) == 1);
        assert(gbuffer_last_constants(&range, 1) == 1 && range.buffer == 9 && uploads == 1);
        assert(ring[0][60][0] == 1.0f && ring[0][61][1] == 1.0f && ring[0][62][2] == 1.0f);
        assert(ring[0][60][3] == -3.0f && ring[0][61][3] == 0.0f && ring[0][62][3] == 0.0f);
        assert(ring[0][60][1] == 0.0f && ring[0][61][0] == 0.0f);
        assert(!memcmp(ring[0][0], gbuffer.last_projection, sizeof(gbuffer.last_projection)));
        assert(ring[0][63][3] == 20.0f);
        return 0;
    }
    if (!strcmp(which, "first_person"))
    {
        /* (the first-person weapon, its pieces too: no constants of its own,
        its draw's bound for its second run; history all the same) */
        window(XGPU_GBUFFER_INSTANCE_FIRST_PERSON);
        assert(gbuffer_previous_motion(motion) == 1 && gbuffer.previous == 0);
        for (row = 0; row < 2; row++)
        {
            assert(gbuffer_last_constants(&range, row) == 1);
            assert(!range.buffer && !range.offset && uploads == 0);
        }
        return 0;
    }
    if (!strcmp(which, "none"))
    {
        /* (no history: the view's, an instance of another pass; a draw not
        an object's twin; the ring full) */
        window(2);
        gbuffer.view.flags = 0;
        assert(gbuffer_previous_motion(motion) == 0 && gbuffer.previous == 0);
        assert(gbuffer_last_constants(&range, 0) == 1 && !range.buffer && uploads == 0);
        window(2);
        instances[2].pass = 7;
        assert(gbuffer_previous_motion(motion) == 0 && gbuffer_last_constants(&range, 1) == 1 && !uploads);
        window(2);
        assert(gbuffer_previous_motion(motion) == 1);
        gbuffer.drawing = NULL;
        assert(gbuffer_last_constants(&range, 0) == 1 && !range.buffer && !uploads);
        gbuffer.drawing = &twin;
        ring_full = 1;
        assert(gbuffer_last_constants(&range, 0) == 0);
        return 0;
    }
    fprintf(stderr, "no case %s\n", which);
    return 2;
}
"""


def last_constants_check(tmp_path, fault=None):
    stream = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    spirv = (SOURCE / "xgpu_spirv.h").read_text()
    defines = "\n".join([
        re.search(r"^#define CONSTANTS_RANGE .*$", stream, re.M).group(0),
        re.search(r"^#define GBUFFER_INSTANCES .*$", stream, re.M).group(0),
        re.search(r"^#define XGPU_SPIRV_GBUFFER_UNIFORMS .*$", spirv, re.M).group(0),
        re.search(r"^#define XGPU_SPIRV_GBUFFER_OBJECT .*$", spirv, re.M).group(0),
    ])
    instance = re.search(r"^struct gbuffer_instance\n\{.*?\n\};", stream, re.M | re.S).group(0)
    code = "\n".join(stream_function(stream, name) for name in
                     ("affine_inverse", "affine_product", "gbuffer_previous_motion", "gbuffer_last_constants"))
    if fault:
        assert fault[0] in code
        code = code.replace(fault[0], fault[1], 1)
    source = tmp_path / "last_constants.c"
    source.write_text(LAST_CONSTANTS_CHECK.replace("@DEFINES@", defines).replace("@INSTANCE@", instance)
                      .replace("@CODE@", code))
    executable = tmp_path / "last_constants"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", "-Werror", "-Wno-unused-function", f"-I{SOURCE}", "-o",
                    str(executable), str(source), "-lm"], check=True)
    return executable


LAST_CONSTANTS_CASES = ["part", "pieces", "first_person", "none"]


@pytest.mark.parametrize("case", LAST_CONSTANTS_CASES)
def test_the_last_frames_constants_of_a_twin_and_its_pieces(tmp_path, case):
    # an object's twin's last frame's constants (xgpu_vulkan_stream.c): the part's, its nodes moved back; where a
    # replacement mesh's pieces draw it (display.prism_models), node 0's identity moved as the root moved, not the
    # part's node 0 applied again to vertices skinned in the world; the first-person weapon none of its own
    if not CC:
        pytest.skip("no C compiler")
    run = subprocess.run([str(last_constants_check(tmp_path)), case], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


@pytest.mark.parametrize("fault,case", [
    # (the pieces given the part's block: its node 0's matrix, the object's placement in it, on world vertices)
    (("\tif (pieces)\n\t\tmemcpy(to[60], gbuffer.moves[0], sizeof(gbuffer.moves[0]));\n", ""), "pieces"),
    # (the first-person weapon given a block of its own again)
    (("FIRST_PERSON)\n\t\treturn 1;", "FIRST_PERSON)\n\t\treturn gbuffer.previous = 1;"), "first_person"),
], ids=["pieces", "first_person"])
def test_the_last_frames_constants_negative_controls(tmp_path, fault, case):
    if not CC:
        pytest.skip("no C compiler")
    run = subprocess.run([str(last_constants_check(tmp_path, fault)), case], capture_output=True, text=True)
    assert run.returncode != 0, run.stdout + run.stderr
