"""PRISM v4's frame upscaling (docs/prism_plugin.md) without a GPU or game data: FSR 3.1's vendored files as their
README lists them (port/third_party/fidelityfx-fsr3), the generated SPIR-V still their GLSL's
(tools/vulkan_upscale_shaders.py --check, where glslang and SPIRV-Tools are installed), FSR's host half
(port/linux/vulkan/xgpu_vulkan_fsr3.c, driven by port/linux/vulkan/tests/upscale_test.c over stubs of the core: its
jitter, the SDK's Halton sequence; a dispatch with no device refused), the denoiser's hooks
(port/linux/vulkan/xgpu_vulkan_upscale.c over the same stubs and fake plugins: the path tracer's light given raw only
in a view begun for the plugin, a plugin that denoises and upscales only with the view's G-buffer), and the settings
(port_config.c's entries, the README's rows, the menus' values)."""

import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
FSR3 = ROOT / "port/third_party/fidelityfx-fsr3"
VULKAN = ROOT / "port/linux/vulkan"


def vulkan_headers():
    for found in sorted(ROOT.glob("build/*/third_party/vulkan-headers")):
        if (found / "vulkan/vulkan_core.h").exists():
            return found
    for system in (Path("/usr/include"), Path("/usr/local/include")):
        if (system / "vulkan/vulkan_core.h").exists():
            return system
    return None


def test_vendored_files_are_the_readmes():
    text = (FSR3 / "README.md").read_text()
    listed = dict((name, digest) for digest, name in re.findall(r"^([0-9a-f]{64})  (\S+)$", text, re.M))
    files = sorted(str(p.relative_to(FSR3)) for p in FSR3.rglob("*") if p.is_file() and p.name != "README.md")
    assert sorted(listed) == files
    for name, digest in listed.items():
        assert hashlib.sha256((FSR3 / name).read_bytes()).hexdigest() == digest, name
    assert "Permission is hereby granted, free of charge" in (FSR3 / "LICENSE.txt").read_text()


def test_generated_spirv_matches_its_glsl(tmp_path):
    if not shutil.which("glslangValidator") or not shutil.which("spirv-val"):
        pytest.skip("needs glslangValidator and spirv-val")
    result = subprocess.run([sys.executable, str(ROOT / "tools/vulkan_upscale_shaders.py"), "--check",
                             "--build-dir", str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    # (clang, as the hosts are built: host.h's guest contract trips gcc's -Werror)
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    headers = vulkan_headers()
    if not cc or headers is None or not sys.platform.startswith("linux"):
        pytest.skip("needs a C compiler and Vulkan's headers on Linux")
    out = tmp_path_factory.mktemp("upscale") / "upscale_test"
    result = subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Werror", f"-I{headers}", f"-I{VULKAN}",
                             f"-I{ROOT / 'port/android/host'}", f"-I{ROOT / 'port/android/include'}",
                             str(VULKAN / "tests/upscale_test.c"), str(VULKAN / "xgpu_vulkan_fsr3.c"),
                             str(VULKAN / "xgpu_vulkan_upscale.c"), "-o", str(out), "-lm"], capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stderr
    return out


def halton(index, base):
    f, result = 1.0, 0.0
    while index > 0:
        f /= base
        result += f * (index % base)
        index //= base
    return result


@pytest.mark.parametrize("render, output", [(1286, 1920), (960, 1920), (1920, 1920)])
def test_jitter_is_fsrs_halton_sequence(driver, render, output):
    lines = subprocess.run([str(driver), "jitter", str(render), str(output)], capture_output=True, text=True,
                           check=True).stdout.split("\n")
    phases = int(lines[0])
    # (the SDK's: 8 times the area's ratio, truncated)
    assert phases == int(8.0 * (output / render) ** 2)
    offsets = [tuple(float(v) for v in line.split()) for line in lines[1:] if line.strip()]
    for index, (x, y) in enumerate(offsets):
        assert -0.5 <= x <= 0.5 and -0.5 <= y <= 0.5
        assert abs(x - (halton(index % phases + 1, 2) - 0.5)) < 1e-6
        assert abs(y - (halton(index % phases + 1, 3) - 0.5)) < 1e-6
    # (the sequence begins again after its length)
    assert offsets[phases] == offsets[0]


def test_no_device_no_dispatch(driver):
    result = subprocess.run([str(driver), "nodevice"], capture_output=True, text=True, check=True)
    assert "dispatch 0" in result.stdout


def run_driver(driver, *arguments):
    return subprocess.run([str(driver), *arguments], capture_output=True, text=True, check=True).stdout


def steps_and_log(text):
    steps = {}
    for what, raw, denoised in re.findall(r"^([^:\n]+): raw (\d) denoised (\d)$", text, re.M):
        steps[what] = (int(raw), int(denoised))
    return steps, re.findall(r"^log: (denoiser: .*)$", text, re.M)


@pytest.mark.parametrize("kind", ["denoise", "rr"])
def test_raw_light_only_in_a_view_begun_for_it(driver, kind):
    """the path tracer's light is given raw to display.denoiser's plugin (its own filter off) only between the
    BEGIN and END of a view begun for the plugin: never in a frame with no view (split screen, a replay's held view,
    a movie, or the setting changed back to "prism" and no view begun), nor in a view begun for PRISM's filter; a
    plugin that denoises takes it in place then, one that denoises and upscales (Ray Reconstruction's kind) takes the
    view at END, so only where the view has its G-buffer (its guides); each change logged"""
    steps, log = steps_and_log(run_driver(driver, "denoiser", kind))
    rr = kind == "rr"
    expected = {
        "no view yet": (0, 0),
        "view begun": (0, 0) if rr else (1, 1),
        "another target's G-buffer": (0, 0) if rr else (1, 1),
        "the view's G-buffer": (1, 0) if rr else (1, 1),
        "view ended": (0, 0),
        "a frame with no view": (0, 0),
        "a frame with no view, its G-buffer": (0, 0),
        "a view for PRISM's filter": (0, 0),
        "a view for the plugin again": (1, 0) if rr else (1, 1),
        "ended again": (0, 0),
    }
    assert steps == expected
    own = f"denoiser: the path tracer's own filter ({kind} not given the light)"
    raw = f"denoiser: the path tracer's light given raw to {kind}"
    assert log == [own, raw, own, raw, own]


def test_settings():
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    for name, default, variable in (("display.upscaler", '"\\"off\\""', "HALO_UPSCALER"),
                                    ("display.upscaler_sharpness", '"0.0"', "HALO_UPSCALER_SHARPNESS"),
                                    ("display.denoiser", '"\\"prism\\""', "HALO_DENOISER"),
                                    ("display.render_scale", '"0.0"', "HALO_RENDER_SCALE")):
        entry = re.search(r'\{ "' + re.escape(name) + r'", _config_\w+, (.*?), "(\w+)"', config)
        assert entry, name
        assert entry.group(1) == default and entry.group(2) == variable, name
    readme = (ROOT / "port/linux/README.md").read_text()
    for name in ("display.upscaler", "display.render_scale", "display.upscaler_sharpness", "display.denoiser"):
        assert f"| `{name}` |" in readme, name
    menus = (ROOT / "port/assets/menus/ce/main_menu.settings_select.player_setup.player_profile_edit."
             "mods_settings.prism_lighting.xml").read_text()
    assert 'setting="display.upscaler" values="off|fsr3|plugin:dlss"' in menus
    assert 'setting="display.denoiser" values="prism|plugin:dlss|off"' in menus
