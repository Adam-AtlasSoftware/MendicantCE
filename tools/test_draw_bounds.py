"""The draws' bounds end with the frame (port/linux/src/draw_bounds_state.h): a light's or a shadow's pass that sets
its bounds (halo_draw_bounds) and never ends clips nothing of the next frame. The state is built alone
(port/linux/tests/draw_bounds_test.c), with its negative control, a present that leaves the bounds, which must fail;
and D3DDevice_Present must be what ends them, checked in d3d8_device.c, with a copy that does not as its negative
control. No GPU or game data."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DEVICE = ROOT / "port" / "linux" / "src" / "d3d8_device.c"


def build_and_run(tmp_path: Path, name: str, defines: list[str]) -> subprocess.CompletedProcess:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a C compiler")
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'port/linux/src'}", *defines,
                    str(ROOT / "port/linux/tests/draw_bounds_test.c"), "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def present_ends_bounds(text: str) -> bool:
    """D3DDevice_Present's body ends the draws' bounds, outside its renderer's branches (the VR one's frames too)"""
    start = text.index("void WINAPI D3DDevice_Present(")
    body = text[start:text.index("\n}\n", start)]
    after = body[body.index("device.frame++;"):]
    return re.search(r"^\tdraw_bounds_state_frame_end\(&draw_bounds\);", after, re.M) is not None


def test_bounds_end_with_the_frame(tmp_path):
    run = build_and_run(tmp_path, "draw_bounds_test", [])
    assert run.returncode == 0, run.stdout + run.stderr


def test_bounds_kept_without_the_reset_fails(tmp_path):
    # (the negative control: the test sees bounds that survive the present)
    run = build_and_run(tmp_path, "draw_bounds_test_no_reset", ["-DDRAW_BOUNDS_TEST_NO_RESET"])
    assert run.returncode != 0
    assert "survive into the next frame" in run.stdout


def test_present_ends_the_bounds():
    text = DEVICE.read_text(encoding="utf-8")
    assert present_ends_bounds(text)
    # (the negative control: d3d8_device.c without the call)
    assert not present_ends_bounds(text.replace("\tdraw_bounds_state_frame_end(&draw_bounds);\n", ""))
