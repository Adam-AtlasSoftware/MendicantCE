"""PRISM v4's baker groundwork (port/linux/vulkan/prism_bake_*.c): the reachable space, the atlas, the bake cache
file, the convergence rule and the volume's batches.

- tools/prism_bake/test_bake.c on synthetic data: a collision bsp built from boxes (a room with a step, stairs to a
  mezzanine, a block too high to jump, a chimney, a sealed room), an atlas of three charts, the cache file written,
  torn, corrupted, resumed and replaced, the stopping rule on Gaussian, heavy-tailed and rare-firefly samples, and
  the irradiance volume's batches with several readbacks in flight (each probe written once, none past the end);
  built warning-free as C89 and run, also under the address and undefined-behaviour sanitizers where they link.
- With the player's maps (HALO_DATA_ROOT or assets, never committed): Rat Race's reachable space reaches every
  spawn and almost all its floor, and none of what is left is within jumping height of a floor walked.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
VULKAN = ROOT / "port/linux/vulkan"
TOOL = ROOT / "tools/prism_bake"
CC = shutil.which("cc") or shutil.which("clang")
SOURCES = sorted(str(p) for p in VULKAN.glob("prism_bake_*.c"))


def build(out, name, main, extra=()):
    exe = out / name
    subprocess.run([CC, "-std=gnu89", "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
                    f"-I{VULKAN}", "-o", str(exe), str(main)] + SOURCES + list(extra) + ["-lm"], check=True)
    return exe


@pytest.mark.skipif(not CC, reason="no C compiler")
def test_libraries(tmp_path):
    exe = build(tmp_path, "test_bake", TOOL / "test_bake.c")
    result = subprocess.run([str(exe), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASSED" in result.stdout


@pytest.mark.skipif(not CC, reason="no C compiler")
def test_libraries_sanitized(tmp_path):
    try:
        exe = build(tmp_path, "test_bake_asan", TOOL / "test_bake.c", ["-g", "-fsanitize=address,undefined",
                                                                          "-fno-sanitize-recover=undefined"])
    except subprocess.CalledProcessError:
        pytest.skip("the sanitizers do not link here")
    result = subprocess.run([str(exe), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def data_root():
    for root in (os.environ.get("HALO_DATA_ROOT"), str(ROOT / "assets")):
        if root and (Path(root) / "maps/ratrace.map").exists():
            return Path(root)
    return None


@pytest.mark.skipif(not CC or data_root() is None, reason="no ratrace.map (HALO_DATA_ROOT)")
def test_ratrace_reachable(tmp_path):
    subprocess.run([sys.executable, "-I", str(TOOL / "bake_scene.py"), "--data", str(data_root()), "--out",
                    str(tmp_path), "--density", "16", "ratrace"], check=True, capture_output=True)
    import json
    summary = json.loads((tmp_path / "ratrace/0/summary.json").read_text())
    assert summary["reach"]["seeds_outside"] == 0
    assert summary["walk_seeds_walked"] == summary["walk_seeds_in_bsp"] > 0
    floors = summary["floors"]
    assert floors["area_reached"] > 0.9 * floors["area"]
    # (what is left is out of reach: each unreached floor is more than a jump above the floor walked under it)
    walk = {}
    data = (tmp_path / "ratrace/0/voxels.bin").read_bytes()
    for x, y, z, flags in struct.iter_unpack("<iiiB", data):
        if flags & 2:
            walk.setdefault((x, y), []).append(z * 0.25)
    for item in summary["unreached_largest"]:
        x, y, z = item["centroid"]
        below = [f for dx in (-1, 0, 1) for dy in (-1, 0, 1) for f in walk.get((int(x // 0.25) + dx, int(y // 0.25) + dy), [])
                 if f <= z]
        assert not below or z - max(below) > 0.5, item
    atlas = summary["atlas"][0]
    assert atlas["tiles"] > 0 and atlas["texels_allocated"] >= atlas["texels_covered"]


@pytest.mark.skipif(not os.environ.get("HALO_DATA_ROOT"), reason="no HALO_DATA_ROOT")
def test_placed_flying_vehicles():
    # (the palette alone turns flight on for whole shells: only the scenario's placed flying vehicles count)
    maps = Path(os.environ["HALO_DATA_ROOT"]) / "maps"
    for name in ("ratrace", "bloodgulch", "b30"):
        assert (maps / (name + ".map")).exists(), name
    sys.path.insert(0, str(TOOL))
    import ce_map
    for name in ("ratrace", "bloodgulch"):
        assert ce_map.Map(str(maps / (name + ".map"))).placed_flying_vehicles() == [], name
    print("b30's placed flying vehicles:", ce_map.Map(str(maps / "b30.map")).placed_flying_vehicles())


@pytest.mark.skipif(not (shutil.which("glslangValidator") and shutil.which("spirv-val") and CC),
                    reason="no glslangValidator, spirv-val or C preprocessor")
def test_bake_shaders_generated():
    """the baker's and PRISM Raster's SPIR-V (xgpu_vulkan_bake_shaders.c) is its GLSL's (xgpu_vulkan_bake_glsl.inc)"""
    result = subprocess.run([sys.executable, str(ROOT / "tools/vulkan_bake_shaders.py"), "--check"], capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_reach_serialised_layout():
    """the game's reach inputs (prism_bake.c's reach_serialise) and the renderer's reader (xgpu_vulkan_bake.c's
    reach_thread) agree on the fields' order: the writer's comment lists what the reader takes, in turn"""
    writer = (ROOT / "port/linux/game/prism_bake.c").read_text()
    reader = (VULKAN / "xgpu_vulkan_bake.c").read_text()
    order = ["bounds = (const float *)reach_take(&r, 6)", "fighter_count = reach_count(&r, 3)",
             "water_count = reach_count(&r, 4)", "bsp.node_count = reach_count", "bsp.plane_count = reach_count",
             "bsp.leaf_count = reach_count", "clusters.leaf_count = reach_count", "clusters.reference_count = reach_count",
             "clusters.cluster_count = reach_count", "clusters.pvs_words = (size_t)reach_count",
             "seed_count = reach_count", "surface_count = reach_count", "triangle_count = (size_t)reach_count",
             "int32_t count = reach_count(&r, SCENERY_FLOATS)", "count = reach_count(&r, FIXTURE_FLOATS)"]
    body = reader[reader.index("static void *reach_thread"):reader.index("prism_bake_reach_defaults(&parameters);")]
    at = [body.index(name) for name in order]
    written = ["blob_put(&blob, input->bounds", "blob_word(&blob, input->fighter_count)",
               "blob_word(&blob, input->water_count)", "blob_word(&blob, input->bsp.node_count)",
               "blob_word(&blob, input->bsp.plane_count)", "blob_word(&blob, input->bsp.leaf_count)",
               "blob_word(&blob, input->clusters.leaf_count)", "blob_word(&blob, input->clusters.reference_count)",
               "blob_word(&blob, input->clusters.cluster_count)", "input->clusters.pvs_words : 0",
               "blob_word(&blob, (long)input->seed_count)", "blob_word(&blob, input->surface_count)",
               "blob_word(&blob, (long)input->triangle_count)", "blob_word(&blob, input->scenery_count)",
               "blob_word(&blob, input->fixture_count)"]
    assert [writer.index(name) for name in written] == sorted(writer.index(name) for name in written)
    assert at == sorted(at)
    assert '"PBSC", 4' in writer and 'blob_word(&blob, 4);' in writer and 'head[1] != 4' in reader
    # (the static scenery's triangle: the same floats on both sides)
    assert "#define SCENERY_FLOATS 13" in writer and "#define SCENERY_FLOATS 13" in reader
    assert "#define FIXTURE_FLOATS 12" in writer and "#define FIXTURE_FLOATS 12" in reader


def test_key_settings_hash_budgets():
    """the bake's key holds the budgets that change what it bakes: `max`'s sample cap (at `max`) and the volume's
    fine brick budget (at every quality), both hashed into key_settings_hash, which bake_begin's key and the better
    bake's check both take"""
    source = (VULKAN / "xgpu_vulkan_bake.c").read_text()
    body = source[source.index("static uint64_t key_settings_hash("):]
    body = body[:body.index("\n}\n")]
    assert "float cap = MAX_SAMPLES_CAP;" in body and "hash = prism_bake_hash64(hash, &cap, sizeof(cap));" in body
    assert "uint32_t fine_most = VOLUME_FINE_BRICKS_MOST;" in body
    fine = body.index("hash = prism_bake_hash64(hash, &fine_most, sizeof(fine_most));")
    # (not under the `max` branch: the budget plans every quality's volume)
    assert fine < body.index("if (quality == PRISM_BAKE_QUALITY_MAX)")
    assert "b->key.settings_hash = key_settings_hash(b, b->quality);" in source
    assert "better.settings_hash = key_settings_hash(b, better.quality);" in source


BLOB_MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
static int failed;
/* (the game's memcpy, cseries.c's csmemcpy: its assert counted) */
static void *checked_memcpy(void *destination, const void *source, unsigned long size)
{
	if (!destination || !source)
		failed++;
	return destination && source ? memcpy(destination, source, size) : destination;
}
#define memcpy(d, s, n) checked_memcpy((d), (s), (n))
%s
%s
int main(void)
{
	struct blob blob;
	float bounds[6] = { 1, 2, 3, 4, 5, 6 };

	memset(&blob, 0, sizeof(blob));
	/* (an empty array first, before the blob has bytes: a BSP with no fighters) */
	blob_put(&blob, NULL, 0);
	blob_put(&blob, "PBSC", 4);
	blob_put(&blob, NULL, 0);
	blob_put(&blob, bounds, sizeof(bounds));
	printf("%%d %%lu %%d\n", failed, blob.size, blob.size == 28 && !memcmp(blob.bytes, "PBSC", 4) &&
		!memcmp(blob.bytes + 4, bounds, sizeof(bounds)));
	free(blob.bytes);
	return 0;
}
"""


@pytest.mark.skipif(not CC, reason="no C compiler")
def test_the_reach_blob_puts_an_empty_array_without_a_copy(tmp_path):
    """reach_serialise puts each of the BSP's arrays, some of them empty and NULL (no fighters, no placements a mod's
    mesh replaces, no alpha maps), into a blob whose own bytes are NULL until its first put: an empty one copies
    nothing, so the game's memcpy (cseries.c's, which asserts "destination && source") is never called with NULL. It
    was, twice at each load and BSP switch under Enhanced lighting. blob_put compiled as prism_bake.c has it"""
    writer = (ROOT / "port/linux/game/prism_bake.c").read_text()
    struct = writer[writer.index("struct blob\n{"):]
    struct = struct[:struct.index("};") + 2]
    put = writer[writer.index("static void blob_put("):]
    put = put[:put.index("\n}\n") + 3]
    assert "if (blob->failed || !size)" in put
    main = tmp_path / "blob.c"
    main.write_text(BLOB_MAIN % (struct, put))
    exe = tmp_path / "blob"
    subprocess.run([CC, "-std=gnu89", "-O1", "-Wall", "-Werror", "-o", str(exe), str(main)], check=True)
    assert subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.split() == ["0", "28", "1"]


def _volume_read(coefficients, n, floored):
    """PRISM Raster's read of one probe's channel at the normal n (xgpu_vulkan_bake_glsl.inc's cascade_at): its SH
    L1 (L0, then y, z, x as the probe stores them), the light over pi; with floored, the ambient floor taken under it"""
    l0, ly, lz, lx = coefficients
    c0, c1 = 0.282095 * math.pi, 0.488603 * 2.0943951
    linear = (l0 * c0 + c1 * (ly * n[1] + lz * n[2] + lx * n[0])) / math.pi
    if not floored:
        return max(linear, 0.0)
    ambient = max(l0 * c0 - 0.5 * c1 * math.sqrt(lx * lx + ly * ly + lz * lz), 0.0) / math.pi
    return max(linear, ambient)


def test_the_volume_read_turned_from_its_light_keeps_the_ambient():
    """A side turned from a probe's main light reads the probe's ambient part, not the linear L1 reading's ringing
    below it: c20's tp_sec4 back wall (a door, which the bake has not), its probes lit through the doorway by the room
    behind it, read 0.28x the level about it at its face turned from that room, where the path tracer has it 2.3x.
    The shader floors both reads (the normal's and the reflection's) by it; the side toward the light is as before"""
    source = (VULKAN / "xgpu_vulkan_bake_glsl.inc").read_text()
    body = source[source.index("bool cascade_at("):source.index("bool volume_at(")]
    assert "vec3 floor_a = max(vec3(cr.x, cg.x, cb.x) * (0.282095 * 3.14159265) - (0.5 * 0.488603 * 2.0943951) *" in body
    assert "vec3(length(cr.yzw), length(cg.yzw), length(cb.yzw)), vec3(0.0)) / 3.14159265;" in body
    assert body.count("/ 3.14159265, floor_a);") == 2
    # an ambient radiance a and a light of intensity i from +x, projected into the probe's SH: the true light at a
    # normal is a + i max(cos, 0) / pi; turned from the light, a alone
    a, i = 0.2, 3.0
    probe = (4.0 * math.pi * 0.282095 * a + 0.282095 * i, 0.0, 0.0, 0.488603 * i)
    away, toward, side = (-1.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)
    assert _volume_read(probe, away, False) < 0.25 * a
    assert _volume_read(probe, away, True) == pytest.approx(a, rel=1e-4)
    for n in (toward, side):
        assert _volume_read(probe, n, True) == _volume_read(probe, n, False)
    # a probe in front of c20's door (bsp2.pbk's at 38.875 -7.875 12.125, red): its L1 past its L0, the door's face
    # read nothing before; a light with no main direction is as before
    door = (0.0964, 0.0022, 0.0370, 0.0845)
    assert _volume_read(door, away, False) == 0.0
    assert _volume_read(door, away, True) > 0.2 * _volume_read(door, side, True)
    even = (0.1, 0.001, -0.002, 0.001)
    for n in (away, toward, side):
        assert _volume_read(even, n, True) == _volume_read(even, n, False)
