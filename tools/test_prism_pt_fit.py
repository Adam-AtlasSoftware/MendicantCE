"""PRISM Path Traced's light grid where the card has no room for its images (port/linux/vulkan/xgpu_vulkan_pt_fit.h)
on the CPU (port/linux/vulkan/tests/pt_fit_test.c): the grid asked where it fits, else the next coarser one, one try
a frame with the failed try's memory let go of first, said once and kept for the window's size, none past the
denoiser off; again with ASan and UBSan. 2026-11-27: an 8 GB card at 3440x1440 drew CE's unlit frame after a change
to Path Traced with World Geometry Enhanced (b30), every one of the path tracer's images refused. No GPU or game
data is needed."""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/vulkan/tests/pt_fit_test.c"
PT = ROOT / "port/linux/vulkan/xgpu_vulkan_pt.c"


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


@pytest.mark.parametrize("mode", ["plain", "address"])
def test_pt_fit(tmp_path, mode):
    cc = compiler()
    if not cc or not sys.platform.startswith(("linux", "darwin")):
        pytest.skip("needs a C compiler on Linux or macOS")
    flags = {"plain": [], "address": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]}[mode]
    binary = tmp_path / "pt_fit_test"
    try:
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags, str(SOURCE), "-o",
                        str(binary), "-lm"], check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if mode != "plain" and "sanitize" in (error.stderr or ""):
            pytest.skip(f"the compiler has no {mode} sanitizer")
        raise AssertionError(error.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60,
                         env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1"))
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all passed" in run.stdout


def test_the_path_tracer_fits_its_images_before_tracing():
    """the frame's images made through the fit (never images_make alone, whose failure drew CE's frame with nothing
    said), a failed try's images let go of, and the shortage said in the log"""
    text = PT.read_text()
    frame = text[text.index("struct xgpu_vulkan_image *xgpu_vulkan_pt_frame("):]
    frame = frame[:frame.index("\n}\n")]
    assert "images_fit(description->width, description->height, light_scale," in frame
    assert "images_make(" not in frame
    made = text[text.index("static int images_made("):]
    made = made[:made.index("\n}\n")]
    assert "images_free();" in made
    assert "no memory on the card for the light's images" in text


BAKE = ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c"


def function(text, head):
    body = text[text.index(head):]
    return body[:body.index("\n}\n")]


def test_the_bake_light_put_aside_never_holds_the_scene_path_traced_reads():
    """2026-11-27: on the 8 GB card the bake's light (2.6 GB with the volume's) left no room for Path Traced's images.
    xgpu_vulkan_bake_park moves only buffers Path Traced never reads: none the scene block it traces by
    (xgpu_vulkan_bake_lookup) points into, nor a structure's storage, nor the final gather's cache block (written
    again, its addresses moved)"""
    text = BAKE.read_text()
    parked = function(text, "static struct bake_buffer *park_buffer(")
    names = set(re.findall(r"&b->(\w+)", parked))
    assert {"pool", "pool_indirect", "table", "sun", "probes", "probe_points", "bricks"} <= names
    block = set(re.findall(r"b->block\.\w+ = b->(\w+)\.address", text))
    assert block >= {"vertices", "info", "materials", "alpha", "lamps", "grid"}
    assert not names & (block | {"scene", "scene_runtime", "cache_block", "instances", "tlas_storage",
                                 "runtime_instances", "runtime_tlas_storage", "fixtures_buffer", "blas_storage",
                                 "runtime_blas_storage", "compact_args", "raster_job", "job", "running"})
    move = function(text, "static int64_t park_move(")
    assert "park_cache_block(commands, b);" in move and "garbage_add(from, VK_NULL_HANDLE);" in move
    cache = function(text, "static void park_cache_block(")
    first = text[text.index("/* (the final gather's cache block: the addresses"):]
    first = first[:first.index("buffer_filled(commands, cache")]
    assert re.findall(r"cache\[\d\] = b->\w+\.address", cache) == re.findall(r"cache\[\d\] = b->\w+\.address", first)
    # (only a whole bake's: nothing more baked into it, nothing read into it from the file)
    allowed = function(text, "static int park_allowed(")
    assert "STEP_DONE" in allowed and "b->complete" in allowed and "!b->relighting" in allowed
    # (Raster brings it back before it draws; Path Traced's images let go of where both do not fit)
    raster = function(text, "struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(")
    assert "bake_unpark(" in raster and "xgpu_vulkan_pt_release()" in raster


def test_the_path_tracer_puts_the_bake_light_aside_before_a_coarser_grid():
    text = PT.read_text()
    made = function(text, "static int images_made(")
    assert made.index("images_room_made()") < made.index("images_make(")
    assert "return images_room_made() ? PT_FIT_FREEING : PT_FIT_REFUSED;" in made
    assert "xgpu_vulkan_bake_park()" in function(text, "static int images_room_made(")
    fit = function(text, "static float images_fit(")
    assert "pt_fit_retry(&pt.memory)" in fit and "xgpu_vulkan_bake_pt_memory(" in fit
