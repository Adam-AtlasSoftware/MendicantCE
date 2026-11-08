"""PRISM's texture store (port/linux/vulkan/xgpu_texture_store.c, xgpu_ktx2.c) over a fake backend
(port/linux/vulkan/tests/texture_store_test.c): the KTX2 rules, the map's answers, the residency and
the budget; and again with two workers under ThreadSanitizer (any report fails it). No GPU or game
data is needed."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "port/linux/vulkan/tests/texture_store_test.c", ROOT / "port/linux/vulkan/xgpu_texture_store.c",
           ROOT / "port/linux/vulkan/xgpu_ktx2.c"]


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


@pytest.mark.parametrize("mode", ["plain", "address", "thread"])
def test_texture_store(tmp_path, mode):
    cc = compiler()
    if not cc or not sys.platform.startswith(("linux", "darwin")):
        pytest.skip("needs a C compiler on Linux or macOS")
    flags = {"plain": [], "address": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
             "thread": ["-fsanitize=thread"]}[mode]
    binary = tmp_path / "texture_store_test"
    try:
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                        f"-I{ROOT / 'port/linux/vulkan'}", *map(str, SOURCES), "-o", str(binary), "-lpthread"],
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if mode != "plain" and "sanitize" in (error.stderr or ""):
            pytest.skip(f"the compiler has no {mode} sanitizer")
        raise AssertionError(error.stderr)
    folder = tmp_path / "files"
    folder.mkdir()
    environment = dict(os.environ, TSAN_OPTIONS="halt_on_error=1 exitcode=66",
                       ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
    arguments = [str(binary), str(folder)] + (["threads"] if mode == "thread" else [])
    run = subprocess.run(arguments, capture_output=True, text=True, env=environment, timeout=300)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all passed" in run.stdout
