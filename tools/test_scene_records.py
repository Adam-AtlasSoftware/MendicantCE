"""PRISM v4's scene records (port/linux/src/xgpu_stream.h's SCENE_* and
GBUFFER_DRAW; docs/prism_scene_records.md) and their encoders
(port/linux/src/xgpu_scene_encode.c), with no game data and no GPU.

A driver (tools/fixtures/scene_records_test.c) is built with the build
machine's C compiler as a 64-bit program and, where the compiler can, a
32-bit one (the guests are ILP32, the hosts LP64). The test checks that
every field lies at the same offset in both, that every record is a
multiple of 4 bytes, that the encoders write exactly the bytes the
documented layouts give (packed here with struct, so the specification
and the encoder are checked against each other), their refusals and
clamps, and that the records change nothing of the stream as it is: its
version still 29, no type of theirs one of the stream's own, none decoded
(their types past XGPU_STREAM_RECORD_TYPES)."""
import math
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port" / "linux" / "src"
DRIVER = ROOT / "tools" / "fixtures" / "scene_records_test.c"
OUT = ROOT / "build" / "test_scene_records"


def compiler():
    for name in ("cc", "clang", "gcc"):
        path = shutil.which(name)
        if path:
            return path
    return None


def build(bits):
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / f"scene_records_{bits}"
    command = [cc, f"-m{bits}", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O1", f"-I{SOURCE}", str(DRIVER),
               str(SOURCE / "xgpu_scene_encode.c"), "-o", str(binary)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        if bits == 32:
            reason = result.stderr.strip().splitlines()[-1] if result.stderr.strip() else "no message"
            pytest.skip("the compiler cannot build 32-bit programs here (gcc-multilib or clang's -m32 runtime "
                        f"missing): {reason}")
        raise AssertionError(result.stderr)
    # (the encoder itself also as the guests' C, declarations first)
    subprocess.run([cc, f"-m{bits}", "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
                    "-c", f"-I{SOURCE}", str(SOURCE / "xgpu_scene_encode.c"), "-o", str(OUT / f"encode_{bits}.o")],
                   check=True)
    return binary


def run(binary, mode):
    return subprocess.run([str(binary), mode], check=True, capture_output=True, text=True).stdout


@pytest.fixture(scope="module")
def driver64():
    return build(64)


@pytest.fixture(scope="module")
def encoded(driver64):
    out = {}
    for line in run(driver64, "encode").splitlines():
        name, rest = line.split(" ", 1)
        out[name] = rest
    return out


def record(encoded, name):
    size, _, data = encoded[name].partition(" ")
    data = bytes.fromhex(data)
    assert int(size) == len(data)
    return data


def f32(value):
    """a float as the encoder keeps it: not finite, 0"""
    return 0.0 if not math.isfinite(value) else value


def pack_floats(values):
    return struct.pack(f"<{len(values)}f", *[f32(v) for v in values])


def header(kind, size):
    return struct.pack("<I", kind | (size << 8))


@pytest.fixture(scope="module")
def constants(driver64):
    values = {}
    for line in run(driver64, "constants").splitlines():
        name, value = line.split()
        values[name] = int(value)
    return values


def test_layout_alike_on_ilp32_and_lp64(driver64):
    layout64 = run(driver64, "layout")
    driver32 = build(32)
    assert run(driver32, "layout") == layout64
    assert run(driver32, "encode") == run(driver64, "encode")


def test_sizes_multiples_of_four(driver64):
    sizes = {}
    for line in run(driver64, "layout").splitlines():
        parts = line.split()
        if len(parts) == 2:
            sizes[parts[0]] = int(parts[1])
        else:
            name, offset, size = parts
            assert int(offset) % 4 == 0, name
            assert int(size) % 4 == 0, name
    assert sizes == {
        "xgpu_stream_scene_material": 200,
        "xgpu_stream_scene_material_animate": 344,
        "xgpu_stream_scene_node": 96,
        "xgpu_stream_scene_instance": 88 + 64 * 96,
        "xgpu_stream_gbuffer_draw": 32,
        "xgpu_stream_scene_static": 32,
        "xgpu_stream_scene_light": 80,
        "xgpu_stream_scene_lights": 16 + 256 * 80,
        "xgpu_stream_scene_decal": 120,
        "xgpu_stream_scene_fog_plane": 64,
        "xgpu_stream_scene_fog": 48 + 8 * 64,
        "xgpu_stream_scene_view": 16 + 128 + 32,
        "xgpu_stream_prism_bake": 80 + 64,
        "xgpu_stream_scene_material_maps": 148,
        "xgpu_stream_terrain_event": 32,
        "xgpu_stream_terrain_wader": 32,
        "xgpu_stream_prism_terrain": 176,
    }


def test_stream_types(constants):
    # (39: the terrain overhaul's PRISM_TERRAIN (T0), the last scene record; 38: the merge with GOAT H1 (OpenH1/goath1 ff83e8d62), whose own 20 to 23 come first in the list, PRISM's
    # records after them, the scene records still last; 37: prism-v4-raster-int's RT_FRAME flag RASTER_OBJECTS, 32, and GBUFFER_DRAW's STATIC_SCENERY; 36: M3's
    # GBUFFER_PHASE_PATHTRACED, its profile and denoiser bits in the resolve's value; 35: M4's
    # SCENE_MATERIAL_MAPS; 34: M2's PRISM_BAKE, GBUFFER_DRAW's lightmap page; 33: the ring's streams_pending; 32:
    # SCENE_STATIC's pages, levels and attributes as sent, SCENE_LIGHTS' emitters, SCENE_VIEW; 31: M1 sends
    # SCENE_MATERIAL and GBUFFER_DRAW, display.lighting "ce_deferred"'s G-buffer pass; 30 was GBUFFER_PHASE, its first
    # record)
    assert constants["version"] == 39
    text = (SOURCE / "xgpu_stream.h").read_text()
    assert re.search(r"#define XGPU_STREAM_VERSION 39\b", text)
    # (the scene's types joined the stream's own list after GBUFFER_PHASE, the numbers they were specified with:
    # the decoder takes every one of them)
    assert constants["rt_frame"] == constants["record_types"] - 14
    scene = [constants[name] for name in ("scene_material", "scene_material_animate", "scene_instance",
                                          "gbuffer_draw", "scene_static", "scene_lights", "scene_decal",
                                          "scene_fog", "scene_view", "prism_bake", "scene_material_maps",
                                          "prism_terrain")]
    assert scene == list(range(constants["rt_frame"] + 2, constants["rt_frame"] + 14))
    assert scene[-1] == constants["record_types"] - 1
    assert constants["scene_record_types"] == constants["record_types"] <= 256
    # (the guest's stream writes SCENE_MATERIAL, GBUFFER_DRAW, SCENE_STATIC, SCENE_LIGHTS, SCENE_INSTANCE and
    # SCENE_VIEW)
    body = (SOURCE / "xgpu_stream_encode.c").read_text()
    for name in ("SCENE_MATERIAL", "GBUFFER_DRAW", "SCENE_STATIC", "SCENE_LIGHTS", "SCENE_INSTANCE", "SCENE_VIEW",
                 "PRISM_BAKE", "SCENE_MATERIAL_MAPS", "PRISM_TERRAIN"):
        assert f"XGPU_STREAM_{name}" in body, name
    decoder = (ROOT / "port/android/host/host_render_stream.c").read_text()
    for name in ("SCENE_MATERIAL", "SCENE_MATERIAL_ANIMATE", "SCENE_INSTANCE", "GBUFFER_DRAW", "SCENE_STATIC",
                 "SCENE_LIGHTS", "SCENE_DECAL", "SCENE_FOG", "SCENE_VIEW", "PRISM_BAKE", "SCENE_MATERIAL_MAPS",
                 "PRISM_TERRAIN"):
        assert f"[XGPU_STREAM_{name}] =" in decoder, name


def test_path_hash(encoded):
    def fnv(text):
        value = 0xcbf29ce484222325
        for byte in text.encode():
            value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
        return value
    assert int(encoded["hash"], 16) == fnv("levels\\b30\\shaders\\sand")
    assert int(encoded["hash_empty"], 16) == fnv("")


def test_material(encoded, constants):
    value = 0xcbf29ce484222325
    for byte in b"levels\\b30\\shaders\\sand":
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    scalars = [i * 0.5 for i in range(32)]
    scalars[1] = math.nan
    scalars[2] = math.inf
    # (the fixed words: material, the hash's two, family, type, the shader's flags, the detail functions, flags,
    # the bitmaps' count)
    expected = (header(constants["scene_material"], 200) +
                struct.pack("<9I", 7, value & 0xffffffff, value >> 32, 0, 12, 5, 0x010200, 1, 8) +
                struct.pack("<8I", *[0xe0000000 + i for i in range(8)]) + pack_floats(scalars))
    assert record(encoded, "material") == expected
    for name in ("material_none", "material_family", "material_room"):
        assert record(encoded, name) == b""


def test_material_maps(encoded, constants):
    # (tier 3 the pack's, model 1 foliage; the flags as given; the first 8 of 10 ids, the first 24 of 40 scalars)
    scalars = [i * 0.5 for i in range(24)]
    scalars[1] = math.nan
    scalars[2] = math.inf
    expected = (header(constants["scene_material_maps"], 148) + struct.pack("<4I", 7, 3, 1, 1 | (2 << 8)) +
                struct.pack("<8I", *[0xe0000000 + i for i in range(8)]) + pack_floats(scalars))
    assert record(encoded, "maps") == expected
    # (the automatic tier: the maps forgotten, every map and scalar 0)
    assert record(encoded, "maps_forget") == header(constants["scene_material_maps"], 148) + \
        struct.pack("<4I", 9, 2, 0, 0) + bytes(4 * 8 + 4 * 24)
    for name in ("maps_none", "maps_tier", "maps_model", "maps_room"):
        assert record(encoded, name) == b""


def test_material_animate(encoded, constants):
    body = header(constants["scene_material_animate"], 344) + struct.pack("<I", 7) + pack_floats([0.25, 0.5, 1, 0])
    for slot in range(8):
        if slot < 2:
            body += pack_floats([2, 0, 0, slot, 0, 3, 0, 0])
        else:
            body += pack_floats([1, 0, 0, 0, 0, 1, 0, 0])
    colors = [[i, 0.5, 0.25, 0] for i in range(4)]
    colors[1][1] = math.nan
    for c in colors:
        body += pack_floats(c)
    assert record(encoded, "animate") == body
    identity = header(constants["scene_material_animate"], 344) + struct.pack("<I", 9) + pack_floats([0] * 4)
    identity += pack_floats([1, 0, 0, 0, 0, 1, 0, 0]) * 8 + pack_floats([0] * 16)
    assert record(encoded, "animate_identity") == identity


def matrix(index, sign=1):
    return [sign * float(index * 100 + row * 10 + column) for row in range(3) for column in range(4)]


def test_instance(encoded, constants):
    size = 88 + 3 * 96
    body = header(constants["scene_instance"], size) + struct.pack("<5I", 0x80010002, 32, 3, 11, 3)
    body += pack_floats([0, 0.5, 0.25, 0]) + pack_floats([1, math.nan, 0.25, 0]) + pack_floats([0] * 8)
    for node in range(3):
        now = matrix(node)
        if node == 1:
            now[11] = math.nan
        body += pack_floats(now) + pack_floats(matrix(node, -1))
    assert record(encoded, "instance") == body
    # (first seen: NO_HISTORY set, this frame's matrices twice)
    first = header(constants["scene_instance"], 88 + 2 * 96) + struct.pack("<5I", 5, 64, 4, 0, 2) + \
        pack_floats([0] * 16)
    for node in range(2):
        now = matrix(node)
        if node == 1:
            now[11] = math.nan
        first += pack_floats(now) * 2
    assert record(encoded, "instance_first") == first
    assert encoded["instance_many"] == f"{88 + 64 * 96} 64"
    assert record(encoded, "instance_level") == b""


def test_gbuffer_draw(encoded, constants):
    assert record(encoded, "gbuffer") == header(constants["gbuffer_draw"], 32) + \
        struct.pack("<7I", 7, 0, (2 << 16) | 3, 41, 8, 4, 0)
    assert record(encoded, "gbuffer_none") == b""


def test_static(encoded, constants):
    assert record(encoded, "static") == header(constants["scene_static"], 32) + \
        struct.pack("<7I", 1, 3, 65536, 1000, 200000, 0x12345670, 36000)
    assert record(encoded, "static_phase") == b""
    assert record(encoded, "static_slice") == b""
    words = encoded["attributes"].split()
    assert words[0] == "1"
    expected = struct.pack("<3I", 7, 99, 1 | (12 << 16)) + pack_floats([0.1, 0.2, 0.3, math.nan, 0.5, 0.6,
                                                                        1, 2, 3, 4, 5, 6]) + struct.pack("<I", 0)
    assert b"".join(struct.pack("<I", int(w, 16)) for w in words[1:]) == expected
    assert encoded["attributes_page"] == "0"


def light(index, radiance_g=0.0):
    return struct.pack("<4I", 1000 + index, index % 5, index % 4, 1) + \
        pack_floats([index, 0, 0, 2, 0, 0, 0, 2, 1, radiance_g, 0, 3, 0.01, 0, 0, 0])


def test_lights(encoded, constants):
    # (lights 1 and 2, an unknown type and class, left out; light 3's infinity written 0)
    size = 16 + 3 * 80
    expected = header(constants["scene_lights"], size) + struct.pack("<3I", 0, 3, 5) + light(0) + \
        light(3, math.inf) + light(4)
    assert record(encoded, "lights") == expected
    assert encoded["lights_many"] == f"{16 + 254 * 80} 254"
    assert record(encoded, "lights_none") == header(constants["scene_lights"], 16) + struct.pack("<3I", 0, 0, 0)


def test_decal(encoded, constants):
    expected = header(constants["scene_decal"], 120) + struct.pack("<5I", 77, 0xe0001234, 2, 2, 2) + \
        pack_floats([1, 2, 3, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, math.nan, 0, 1, 0.5, 0.25, 0.75, 0, 0, 0.5, 0.5])
    assert record(encoded, "decal") == expected
    assert record(encoded, "decal_blend") == b""


def plane(index):
    return struct.pack("<4I", index, 2 if index & 1 else 0, 0, 0) + \
        pack_floats([0, 0, 1, index, 0.5, 0, 0, 0.8, 30, 4, 0, 0])


def test_fog(encoded, constants):
    # (plane 1, with no normal, left out; the pad written 0 whatever was given)
    expected = header(constants["scene_fog"], 48 + 2 * 64) + struct.pack("<3I", 1, 2, 0) + \
        pack_floats([0.1, 0.2, 0.3, 0.9, 10, 200, 0, 0]) + plane(0) + plane(2)
    assert record(encoded, "fog") == expected
    # (ten planes: cut to eight, the one with no normal left out)
    assert encoded["fog_many"] == f"{48 + 7 * 64} 7"


def terrain_fixed(constants, size, phase, count, winds):
    """PRISM_TERRAIN's fixed part as the driver's frame gives it (the header and count the encoder's own; a NaN
    written 0; the winds past wind_count 0)"""
    return (header(constants["prism_terrain"], size) + struct.pack("<3I", phase, 3, count) +
            pack_floats([1.0 / 30.0, math.nan, -70.0, 22.0, 3.5]) + struct.pack("<i3Ii", 0, 1 | 2 | (1 << 17), 3, 1, 0) +
            pack_floats([1.0, 1.0, 0.0, 0.0]) + struct.pack("<I", winds) +
            pack_floats([v if i // 3 < winds and v != 5 else 0.0 for i, v in enumerate(float(n) for n in range(24))]) +
            struct.pack("<I", 0x12345678))


def test_terrain_frame(encoded, constants):
    # (wind_count 12 cut to the 8 there are; the second's z, a NaN, written 0)
    assert record(encoded, "terrain_frame") == terrain_fixed(constants, 176, 0, 0, 8)
    assert record(encoded, "terrain_frame_winds") == terrain_fixed(constants, 176, 0, 0, 2)


def terrain_event(index, z=0.25, magnitude=1.0):
    return struct.pack("<I", 1 | (0x100 if index & 1 else 0) | (1 << 16) | (1 << 24)) + \
        pack_floats([index, 2.0, z]) + struct.pack("<2I", 0, 0x7fff0000) + pack_floats([magnitude]) + \
        struct.pack("<I", 0xe0010000 + index)


def test_terrain_events(encoded, constants):
    size = 176 + 3 * 32
    expected = terrain_fixed(constants, size, 1, 3, 2) + terrain_event(0) + terrain_event(1, z=math.nan) + \
        terrain_event(2, magnitude=math.inf)
    assert record(encoded, "terrain_events") == expected
    # (a frame's most: 256 events, under 12 KB, the design's bound; one more, refused; too little room, or no
    # events given, refused)
    assert encoded["terrain_events_most"] == str(176 + 256 * 32)
    assert 176 + 256 * 32 < 12 * 1024
    for name in ("terrain_events_many", "terrain_events_room", "terrain_events_none"):
        assert record(encoded, name) == b""


def test_terrain_waders(encoded, constants):
    size = 176 + 3 * 32
    body = terrain_fixed(constants, size, 2, 3, 2)
    for index in range(3):
        velocity = [-0.0, -1.0, -2.0]
        if index == 2:
            velocity[1] = math.nan
        body += struct.pack("<I", 1 | 4 | (index << 16)) + pack_floats([index, index + 1, index + 2] + velocity +
                                                                         [0.25])
    assert record(encoded, "terrain_waders") == body
    # (65 waders, items for a marker, a phase past the last: refused)
    for name in ("terrain_waders_many", "terrain_marker_items", "terrain_phase"):
        assert record(encoded, name) == b""


def test_terrain_decoded():
    """the host's decoder takes PRISM_TERRAIN's fixed part and checks its count against its phase's most and the
    record's size"""
    decoder = (ROOT / "port/android/host/host_render_stream.c").read_text()
    case = decoder[decoder.index("case XGPU_STREAM_PRISM_TERRAIN:"):]
    case = case[:case.index("break;")]
    assert "XGPU_TERRAIN_EVENTS_MOST" in case and "XGPU_TERRAIN_WADERS_MOST" in case
    assert "terrain->count * sizeof(terrain->items[0])" in case and "CALL(prism_terrain, terrain)" in case
