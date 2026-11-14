"""PRISM v4's baker groundwork (port/linux/vulkan/prism_bake_*.c): the reachable space, the atlas, the bake cache
file and the convergence rule.

- tools/prism_bake/test_bake.c on synthetic data: a collision bsp built from boxes (a room with a step, stairs to a
  mezzanine, a block too high to jump, a chimney, a sealed room), an atlas of three charts, the cache file written,
  torn, corrupted, resumed and replaced, and the stopping rule on Gaussian, heavy-tailed and rare-firefly samples;
  built warning-free as C89 and run, also under the address and undefined-behaviour sanitizers where they link.
- With the player's maps (HALO_DATA_ROOT or assets, never committed): Rat Race's reachable space reaches every
  spawn and almost all its floor, and none of what is left is within jumping height of a floor walked.
"""
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
             "seed_count = reach_count", "surface_count = reach_count", "triangle_count = (size_t)reach_count"]
    body = reader[reader.index("static void *reach_thread"):reader.index("prism_bake_reach_defaults(&parameters);")]
    at = [body.index(name) for name in order]
    written = ["blob_put(&blob, input->bounds", "blob_word(&blob, input->fighter_count)",
               "blob_word(&blob, input->water_count)", "blob_word(&blob, input->bsp.node_count)",
               "blob_word(&blob, input->bsp.plane_count)", "blob_word(&blob, input->bsp.leaf_count)",
               "blob_word(&blob, input->clusters.leaf_count)", "blob_word(&blob, input->clusters.reference_count)",
               "blob_word(&blob, input->clusters.cluster_count)", "input->clusters.pvs_words : 0",
               "blob_word(&blob, (long)input->seed_count)", "blob_word(&blob, input->surface_count)",
               "blob_word(&blob, (long)input->triangle_count)"]
    assert [writer.index(name) for name in written] == sorted(writer.index(name) for name in written)
    assert at == sorted(at)
    assert '"PBSC", 4' in writer and 'blob_word(&blob, 3);' in writer and 'head[1] != 3' in reader
