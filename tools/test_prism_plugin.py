"""PRISM's renderer plugins (docs/prism_plugin.md): the ABI header (port/linux/vulkan/prism_plugin.h), the
passthrough plugin built as a shared library that exports its entry alone, and the host's loader
(port/linux/vulkan/plugins/loader/xgpu_plugin.c) driven by port/linux/vulkan/tests/plugin_test.c: the passthrough opened and its
commands recorded over a fake device, and each kind of bad plugin, a missing one and a wrong ABI refused with a
logged reason; no name, no file touched. No GPU or game data is needed: a C compiler, nm and Vulkan's headers (the
build's, build/<name>/third_party/vulkan-headers, or the system's)."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
VULKAN = ROOT / "port/linux/vulkan"
PASSTHROUGH = VULKAN / "plugins/passthrough/prism_plugin_passthrough.c"
WORK = ROOT / "build/test_prism_plugin"


def compiler():
    return os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")


def vulkan_headers():
    for found in sorted(ROOT.glob("build/*/third_party/vulkan-headers")):
        if (found / "vulkan/vulkan_core.h").exists():
            return found
    for system in (Path("/usr/include"), Path("/usr/local/include")):
        if (system / "vulkan/vulkan_core.h").exists():
            return system
    return None


@pytest.fixture(scope="module")
def built():
    cc = compiler()
    if not cc or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler on Linux")
    headers = vulkan_headers()
    if headers is None:
        pytest.skip("no Vulkan headers: run configure.py (it fetches them) or install the system's")
    if WORK.exists():
        shutil.rmtree(WORK)
    plugins = WORK / "plugins"
    plugins.mkdir(parents=True)
    flags = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", f"-I{headers}", f"-I{VULKAN}",
             f"-I{VULKAN / 'plugins/loader'}"]
    shared = ["-shared", "-fPIC", "-fvisibility=hidden"]

    def run(*arguments):
        result = subprocess.run([cc, *flags, *arguments], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr

    run(*shared, str(PASSTHROUGH), "-o", str(plugins / "passthrough.so"))
    for kind, name in ((1, "no_symbol"), (2, "other_abi"), (3, "small_info"), (4, "no_evaluate"), (5, "big_frame"),
                       (6, "no_table")):
        run(*shared, f"-DBAD_KIND={kind}", str(VULKAN / "tests/plugin_bad.c"), "-o", str(plugins / f"{name}.so"))
    (plugins / "not_a_library.so").write_text("not a library\n")
    driver = WORK / "plugin_test"
    run("-Wl,--wrap=fopen,--wrap=dlopen", str(VULKAN / "tests/plugin_test.c"),
        str(VULKAN / "plugins/loader/xgpu_plugin.c"), "-o",
        str(driver), "-ldl")
    return driver, plugins


def drive(built, *arguments):
    driver, plugins = built
    return subprocess.run([str(driver), arguments[0], str(plugins), *arguments[1:]], capture_output=True, text=True,
                          timeout=60)


def test_passthrough_exports_only_its_entry(built):
    if not shutil.which("nm"):
        pytest.skip("needs nm")
    _, plugins = built
    symbols = subprocess.run(["nm", "-D", "--defined-only", str(plugins / "passthrough.so")], capture_output=True,
                             text=True, check=True).stdout.split("\n")
    names = sorted(line.split()[-1] for line in symbols if line.strip() and line.split()[-2] in "TtDdBbRr")
    assert names == ["prism_plugin_entry"], names


def test_passthrough_opens_and_records_a_copy(built):
    result = drive(built, "record", "passthrough")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "name passthrough vendor PRISM version 1 capabilities 1 abi 1" in result.stdout
    assert "log: prism plugin passthrough: opened (1)" in result.stdout
    # (the same sizes copied, others blitted; four transitions an evaluate, every image left as it came)
    assert "copies 1 blits 1 barriers 8 balance 0" in result.stdout
    assert "refused small frame: passthrough:" in result.stdout


@pytest.mark.parametrize("name, reason", [
    ("missing", "none at"),
    ("no_symbol", "it has no prism_plugin_entry"),
    ("other_abi", "it was built for another ABI"),
    ("small_info", "its info is smaller than this ABI's"),
    ("no_evaluate", "its table lacks a call"),
    ("big_frame", "it expects structures this host does not give"),
    ("no_table", "it serves no table for this ABI"),
    ("not_a_library", "cannot be opened"),
    ("../passthrough", "is not a plugin's name"),
])
def test_refused_with_a_reason(built, name, reason):
    result = drive(built, "open", name)
    assert result.returncode == 3, result.stdout + result.stderr
    logged = [line for line in result.stdout.splitlines() if line.startswith("log: ")]
    assert len(logged) == 1 and reason in logged[0], result.stdout


def test_another_abi_asked_is_refused(built):
    result = drive(built, "abi", "passthrough", "2")
    assert result.returncode == 3, result.stdout + result.stderr
    assert "it serves no table for this ABI" in result.stdout


def test_no_name_touches_nothing(built):
    result = drive(built, "empty")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "touched 0" in result.stdout and "log:" not in result.stdout


def test_no_name_opens_nothing_under_strace(built):
    if not shutil.which("strace"):
        pytest.skip("strace is not installed (the wrapped fopen and dlopen check it without)")
    driver, plugins = built
    result = subprocess.run(["strace", "-f", "-e", "trace=openat,open", str(driver), "empty", str(plugins)],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert str(plugins) not in result.stderr
