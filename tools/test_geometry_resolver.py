"""PRISM's resolved geometry of the drawn objects (port/linux/vulkan/xgpu_geometry.c) on the CPU
(port/linux/vulkan/tests/geometry_test.c): each object's level kept with its hysteresis however many objects
there are and however long one is not drawn, a shadow drawn at its view's level, the ladder's drawable floor,
the traced triangles' budget shared out the farthest first and never to stock, the variant stock until the mesh
is in and then the remodel, a new map (a settings change) forgetting every choice, the notes' flips and
mismatches, the table's ageing; again with ASan and UBSan. No GPU or game data is needed."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "port/linux/vulkan/tests/geometry_test.c", ROOT / "port/linux/vulkan/xgpu_geometry.c"]


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


@pytest.mark.parametrize("mode", ["plain", "address"])
def test_geometry_resolver(tmp_path, mode):
    cc = compiler()
    if not cc or not sys.platform.startswith(("linux", "darwin")):
        pytest.skip("needs a C compiler on Linux or macOS")
    flags = {"plain": [], "address": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]}[mode]
    binary = tmp_path / "geometry_test"
    try:
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE", *flags,
                        f"-I{ROOT / 'port/linux/vulkan'}", *map(str, SOURCES), "-o", str(binary), "-lm"],
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if mode != "plain" and "sanitize" in (error.stderr or ""):
            pytest.skip(f"the compiler has no {mode} sanitizer")
        raise AssertionError(error.stderr)
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
    run = subprocess.run([str(binary)], capture_output=True, text=True, env=environment, timeout=300)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all passed" in run.stdout
