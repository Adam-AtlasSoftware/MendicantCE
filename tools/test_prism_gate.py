"""PRISM's quality gates (tools/prism_gate, docs/prism_gates.md) without the game: the metrics on made-up frames, FLIP,
the OpenEXR writer of debug.pt_reference (port/linux/vulkan/xgpu_exr.c) against the reader, the camera paths the
captures run (their poses exact at every saved frame, within camera_pose.c's capacities), the frozen capture set and
the thresholds, and gate.py's judging of a capture folder."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from prism_gate import capture, exr, flip, gate, metrics, paths  # noqa: E402

WORK = ROOT / "build" / "test_prism_gate"


@pytest.fixture(scope="module")
def work():
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    yield WORK
    shutil.rmtree(WORK, ignore_errors=True)


def noise(seed, shape=(540, 960, 3), level=128.0, spread=40.0):
    return np.clip(np.random.default_rng(seed).normal(level, spread, shape), 0, 255).astype(np.float32)


# ---------- metrics

def test_metrics_zero_against_itself():
    a = noise(1)
    assert metrics.rel_mse(a, a) == 0.0
    assert metrics.blotch(a, a) == 0.0


def test_blotch_sees_patches_not_fine_noise():
    ref = np.full((540, 960, 3), 100.0, np.float32)
    fine = ref + np.random.default_rng(2).normal(0, 6, ref.shape).astype(np.float32)
    # (the same error's size, in squares of 16 pixels: a reservoir's or a filter's patch)
    yy, xx = np.mgrid[0:540, 0:960]
    patches = ref + np.where(((yy // 16) + (xx // 16)) % 2, 6.0, -6.0)[..., None].astype(np.float32)
    assert metrics.blotch(patches, ref) > 3 * metrics.blotch(fine, ref)


def test_keep_mask_leaves_out_what_moved():
    still = noise(3)
    moving = still.copy()
    moving[100:200, 100:300] = 255.0
    keep = metrics.keep_mask(moving, still)
    assert not keep[150, 200] and keep[400, 800]
    assert metrics.keep_mask(noise(4), noise(5)).all()


def test_flicker_is_a_ratio_to_rt_off():
    frames_off = [np.full((54, 96, 3), float(i), np.float32) for i in range(10)]
    frames = [f * 1.5 for f in frames_off]
    assert metrics.flicker(frames, frames_off) == pytest.approx(1.5)


def moving_square(trail, frames=40, step=6):
    """a still camera's frames as a bright square crosses a textured ground; the path-traced frames keep a ghost of
    the square's last `trail` places, fading"""
    ground = noise(6, (90, 300, 3), 80.0, 10.0)
    off, traced = [], []
    for t in range(frames):
        o = ground.copy()
        x = 10 + t * step
        o[30:60, x:x + 20] = 240.0
        off.append(o)
        p = o.copy()
        for k in range(1, trail + 1):
            xk = 10 + (t - k) * step
            if xk >= 0:
                region = p[30:60, xk:xk + 20]
                mask = region < 200.0
                region[mask] += 60.0 * (1 - k / (trail + 1))
        traced.append(p)
    return traced, off


@pytest.mark.parametrize("trail", [0, 2, 5])
def test_ghost_trail_measures_a_known_trail(trail):
    traced, off = moving_square(trail)
    found, residuals = metrics.ghost_trail(traced, off, lags=16)
    assert found == trail, residuals


def test_sky_share():
    z = np.zeros((10, 10))
    z[:2] = 5.0
    assert metrics.sky_share(z) == pytest.approx(0.8)


# ---------- FLIP

def test_flip_identical_is_zero_and_opposites_large():
    a = noise(7, (64, 64, 3))
    assert flip.mean_flip(a, a) == 0.0
    black, white = np.zeros((64, 64, 3)), np.full((64, 64, 3), 255.0)
    assert flip.mean_flip(black, white) > 0.9


def test_flip_grows_with_the_error():
    a = noise(8, (96, 96, 3))
    errors = [flip.mean_flip(a, np.clip(a + noise(9, a.shape, 0.0, s), 0, 255)) for s in (2.0, 8.0, 32.0)]
    assert 0.0 < errors[0] < errors[1] < errors[2] < 1.0


def test_flip_colour_shift_counts():
    grey = np.full((64, 64, 3), 128.0)
    red = grey.copy()
    red[..., 0] += 40.0
    assert flip.mean_flip(grey, red) > 0.1


# ---------- OpenEXR

def test_exr_round_trip(work):
    channels = {"light.R": np.random.default_rng(10).random((7, 5)).astype(np.float32), "Z": np.arange(35.0).reshape(7, 5)}
    exr.write(work / "a.exr", channels, {"prismReferenceFrames": 64})
    read, attributes = exr.read(work / "a.exr")
    assert attributes["prismReferenceFrames"] == 64
    for name, values in channels.items():
        assert np.array_equal(read[name], values.astype(np.float32))


WRITER_TEST = r"""
#include "xgpu_exr.h"
#include <stdio.h>
int main(int argc, char **argv)
{
	static const char *const names[] = { "light.R", "light.G", "Z" };
	struct xgpu_exr_attribute attributes[1] = { { "prismReferenceFrames", 4096 } };
	float pixels[4 * 3 * 3];
	char error[256];
	int i;

	for (i = 0; i < 4 * 3 * 3; i++)
		pixels[i] = (float)i * 0.5f - 3.0f;
	if (!xgpu_exr_write(argv[1], 4, 3, 3, names, pixels, attributes, 1, error, sizeof(error)))
	{
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	/* (refused: a channel named twice, no pixels) */
	if (xgpu_exr_write(argv[2], 4, 3, 2, (const char *const[]){ "A", "A" }, pixels, NULL, 0, error, sizeof(error)) ||
		xgpu_exr_write(argv[2], 4, 3, 1, names, NULL, NULL, 0, error, sizeof(error)))
		return 2;
	return 0;
}
"""


def test_reference_writer_against_the_reader(work):
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    (work / "writer_test.c").write_text(WRITER_TEST)
    program = work / "writer_test"
    vulkan = ROOT / "port/linux/vulkan"
    result = subprocess.run([cc, "-std=gnu11", "-Wall", "-Werror", f"-I{vulkan}", str(work / "writer_test.c"),
                             str(vulkan / "xgpu_exr.c"), "-o", str(program)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    out = work / "sub" / "ref.exr"
    result = subprocess.run([str(program), str(out), str(work / "bad.exr")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert not (work / "bad.exr").exists() and not Path(str(out) + ".part").exists()
    channels, attributes = exr.read(out)
    assert attributes["prismReferenceFrames"] == 4096
    pixels = np.arange(36, dtype=np.float32).reshape(3, 4, 3) * 0.5 - 3.0
    for index, name in enumerate(("light.R", "light.G", "Z")):
        assert np.array_equal(channels[name], pixels[..., index])
    try:
        import OpenEXR
    except ImportError:
        return
    other = OpenEXR.File(str(out)).channels()
    assert np.array_equal(np.asarray(other["Z"].pixels, np.float32), pixels[..., 2])


# ---------- the camera paths

def test_a_hold_is_exact_at_every_frame():
    t = paths.Timeline()
    a = [1, 2, 3, 10, -5, 0, 70]
    b = [9, 8, 7, 170, 5, 0, 60]
    t.hold(a, 5, save="a")
    t.hold(b, 9, save="b")
    t.hold(a, 1)
    keys = paths.read_keys(t.text())
    for frame in range(5):
        assert paths.pose_at(keys, frame) == pytest.approx(a)
    for frame in range(5, 14):
        assert paths.pose_at(keys, frame) == pytest.approx(b, abs=1e-4)
    assert t.saves == {4: "a", 13: "b"}
    assert "save 4\nsave 13" in t.text()


def test_moves_follow_the_first_gate():
    keys = [[0, 0, 0, 0, 0, 0, 70], [40, 0, 0, 90, 0, 0, 70], [40, 40, 0, 90, 10, 0, 70]]
    poses = paths.move_poses(keys, 40)
    assert len(poses) == 80
    assert poses[0] == pytest.approx([1, 0, 0, 2.25, 0, 0, 70])
    assert poses[39] == pytest.approx(keys[1]) and poses[-1] == pytest.approx(keys[2])


def path_capacity(name):
    return int(re.search(rf"#define {name} (\d+)", (ROOT / "port/linux/game/camera_pose.c").read_text()).group(1))


@pytest.mark.parametrize("scene_id", sorted(capture.load_scenes()))
def test_every_capture_fits_the_game_and_holds_its_poses(scene_id):
    scene = capture.load_scenes()[scene_id]
    moves = capture.moves_timeline(scene, list(capture.STATES))
    refs = capture.refs_timeline(scene, scene["reference_frames"])
    poses = paths.move_poses(scene["keys"], scene["steps"])
    for t in (moves, refs):
        text = t.text(scene["start"])
        assert len(t.commands) <= path_capacity("MAXIMUM_PATH_COMMANDS")
        assert len(t.keys) <= path_capacity("MAXIMUM_PATH_KEYS")
        assert len(t.saves) <= path_capacity("MAXIMUM_PATH_SAVES")
        assert all(len(c) < 128 for _, c in t.commands)
        keys = paths.read_keys(text)
        for frame, name in t.saves.items():
            if name.startswith("moves/"):
                step = int(name.split("_")[-1][:3])
                expected = poses[step - 1]
            else:
                expected = poses[capture.probe_steps(scene)[int(name[6])] - 1]
            # (the frame saved and the one before it at the pose: a pose put in a frame is drawn by the next)
            for f in (frame - 1, frame):
                got = paths.pose_at(keys, f)
                got[3] = expected[3] + ((got[3] - expected[3] + 180) % 360 - 180)
                assert got == pytest.approx(expected, abs=2e-3), (name, f)
    assert len([n for n in moves.saves.values() if n.startswith("moves/")]) == len(capture.STATES) * len(poses)
    assert sorted(refs.saves.values()) == sorted(f"refs/p{i}_{k}.png" for i in range(3) for k in ("off", "ref"))


def test_the_capture_set_is_frozen_as_the_design_names_it():
    scenes = capture.load_scenes()
    assert {f"G{i}" for i in range(1, 8)} <= set(scenes)
    for scene in scenes.values():
        assert len(scene["keys"]) >= 2 and scene["map"] and scene["tests"]
        assert scene["reference_frames"] == 4096


def test_the_thresholds():
    limits = json.loads((ROOT / "tools/prism_gate/thresholds.json").read_text())
    assert set(limits["gate"]) == {"error", "blotch", "flicker", "flip_still", "flip_moving", "ghost_frames",
                                   "sky_most"}
    assert 0 < limits["bake"]["noise"] < 0.1


# ---------- the gate's judging

def fake_capture(folder, scene, error=0.0, sky=False):
    """a capture folder as capture.py writes it, of made-up frames: the moves a fixed ground, the references the
    same, the path traced moves off by `error`"""
    moves, refs = folder / "moves", folder / "refs"
    moves.mkdir(parents=True)
    refs.mkdir()
    total = scene["steps"] * (len(scene["keys"]) - 1)
    ground = noise(11, (54, 96, 3)).astype(np.uint8)
    for step in range(1, total + 1):
        for state in capture.STATES:
            frame = ground.astype(np.float32) + (error if state == "pathtraced" else 0.0)
            Image.fromarray(np.clip(frame, 0, 255).astype(np.uint8)).save(moves / f"m_{state}_{step:03d}.png")
    for index in range(3):
        Image.fromarray(ground).save(refs / f"p{index}_ref.png")
        Image.fromarray(ground).save(refs / f"p{index}_off.png")
        z = np.zeros((6, 8), np.float32) if sky else np.ones((6, 8), np.float32)
        exr.write(refs / f"p{index}.exr", {"Z": z})


@pytest.mark.parametrize("error, sky, passed", [(0.0, False, True), (60.0, False, False), (0.0, True, False)])
def test_gate_judges_a_capture(work, error, sky, passed):
    scene = dict(capture.load_scenes()["G4"])
    scene["steps"] = 4
    folder = work / f"gate_{int(error)}_{sky}"
    fake_capture(folder, scene, error, sky)
    result = gate.measure_scene(scene, folder, gate.thresholds())
    assert result["pass"] is passed
    assert result["harness_fault"] is sky
