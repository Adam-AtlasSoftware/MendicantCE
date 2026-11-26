"""PRISM v4's frame upscaling (docs/prism_plugin.md) without a GPU or game data: FSR 3.1's vendored files as their
README lists them (port/third_party/fidelityfx-fsr3), the generated SPIR-V still their GLSL's
(tools/vulkan_upscale_shaders.py --check, where glslang and SPIRV-Tools are installed), FSR's host half
(port/linux/vulkan/xgpu_vulkan_fsr3.c, driven by port/linux/vulkan/tests/upscale_test.c over stubs of the core: its
jitter, the SDK's Halton sequence; a dispatch with no device refused), the denoiser's hooks and the plugins'
extensions (port/linux/vulkan/xgpu_vulkan_upscale.c over the same stubs and fake plugins: the path tracer's light
given raw only in a view begun for the plugin, a plugin that denoises and upscales only with the view's G-buffer; an
extension missing turns off the plugins that asked for it, no other), and the settings (port_config.c's entries, the
README's rows, the menus' values)."""

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


def test_extension_missing_turns_off_its_plugins_alone(driver):
    """an instance extension the loader lacks turns off the plugin that asked for it, not the other; a device
    extension both plugins asked for (asked once of the device) turns off both"""
    text = run_driver(driver, "extensions", "instance")
    assert "instance VK_A VK_B\n" in text
    assert "device VK_C VK_D\n" in text
    off = re.findall(r"^log: prism plugin (\w+): turned off: (.*)$", text, re.M)
    assert off == [("askB", "the Vulkan loader has no VK_B; the renderer's own denoiser is drawn")]
    assert re.search(r"^log: prism plugin askA: ready", text, re.M)
    text = run_driver(driver, "extensions", "device")
    assert "device VK_C VK_D VK_E\n" in text
    off = re.findall(r"^log: prism plugin (\w+): turned off: (.*)$", text, re.M)
    assert sorted(name for name, _ in off) == ["askA", "askB"]
    assert all(why.startswith("the graphics card has no VK_C;") for _, why in off)
    assert "ready" not in text


def test_settings():
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    for name, default, variable in (("display.upscaler", '"\\"off\\""', "HALO_UPSCALER"),
                                    ("display.upscaler_sharpness", '"0.0"', "HALO_UPSCALER_SHARPNESS"),
                                    ("display.denoiser", '"\\"prism\\""', "HALO_DENOISER"),
                                    ("display.upscaler_scale", '"0.0"', "HALO_UPSCALER_SCALE")):
        entry = re.search(r'\{ "' + re.escape(name) + r'", _config_\w+, (.*?), "(\w+)"', config)
        assert entry, name
        assert entry.group(1) == default and entry.group(2) == variable, name
    readme = (ROOT / "port/linux/README.md").read_text()
    for name in ("display.upscaler", "display.upscaler_scale", "display.upscaler_sharpness", "display.denoiser"):
        assert f"| `{name}` |" in readme, name
    folder = ROOT / "port/assets/menus/ce"
    menus = (folder / "main_menu.settings_select.player_setup.player_profile_edit.video_settings.prism."
             "path_tracing.xml").read_text()
    upscaling = (folder / "main_menu.settings_select.player_setup.player_profile_edit.video_settings.prism."
                 "upscaling.xml").read_text()
    # (PRISM's Path Tracing's DENOISER and its UPSCALING page's rows: the DLSS choices last, which menu_tags.c's
    # spinner_expand leaves out where the plugin is not ready (sdl_platform.c's platform_prism_plugin_ready); PRISM's
    # own denoiser first, the default)
    assert 'setting="display.upscaler" values="off|fsr3|plugin:dlss"' in upscaling
    assert 'setting="display.denoiser" values="prism|off|dlss_rr"' in menus
    assert 'setting="display.upscaler_scale" values="0|0.58|0.5|0.33|1"' in upscaling
    tags = (ROOT / "port/linux/game/menu_tags.c").read_text()
    expand = tags.split("static long spinner_expand(", 1)[1].split("\n}\n", 1)[0]
    assert '!strcmp(setting, "display.denoiser") || !strcmp(setting, "display.upscaler")' in expand
    assert '!platform_prism_plugin_ready("dlss") && count > 1 ? count - 1 : count' in expand
    # (Classic, display.lighting "classic" (and its "ce_deferred" diagnostic), draws without an upscaler or a
    # denoiser's plugin: neither jitters its view)
    upscale = (ROOT / "port/linux/game/prism_upscale.c").read_text()
    classic = upscale.split('else if (!strcmp(config_string("display.lighting"), "classic") ||', 1)[1].split("}", 1)[0]
    assert "prism_upscale.upscaler = _upscaler_none;" in classic
    assert "prism_upscale.denoiser = _denoiser_prism;" in classic
    # (Ray Reconstruction is the path tracer's: under Enhanced, "enhanced", a "dlss_rr" kept from Path Traced is the
    # in-tree filter's)
    assert '(!strcmp(denoiser, "dlss_rr") && strcmp(config_string("display.lighting"), "enhanced"))' in upscale
    platform =(ROOT / "port/linux/src/sdl_platform.c").read_text()
    assert "return host_vulkan_plugin_ready(name) != 0;" in platform
    assert "host_vulkan_plugin_ready" in (ROOT / "port/linux/vulkan/host_imports.list").read_text().split()


def _tone_curved(c, curve):
    """xgpu_vulkan_bake_glsl.inc's tone_curved (the path tracer's composite's), in numpy"""
    import numpy as np
    c = np.maximum(c, 0.0)
    if curve == 2:
        return np.clip((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0)
    if curve not in (1, 3):
        return c
    brightest = c.max(axis=-1, keepdims=True)
    k = 0.6
    mapped = np.where(brightest <= k, brightest, k + (1 - k) * (1 - np.exp(-(brightest - k) / (1 - k))))
    return np.where(brightest > 1e-5, c * mapped / np.maximum(brightest, 1e-30), c)


def _untone(c, curve):
    """INPUTS' untone (tools/vulkan_upscale_shaders.py), in numpy"""
    import numpy as np
    c = np.maximum(c, 0.0)
    if curve == 2:
        y = np.minimum(c, 0.995)
        a, b, k = 2.51 - 2.43 * y, 0.03 - 0.59 * y, -0.14 * y
        return (-b + np.sqrt(np.maximum(b * b - 4 * a * k, 0.0))) / (2 * a)
    if curve not in (1, 3):
        return c
    mapped = c.max(axis=-1, keepdims=True)
    knee = 0.6
    t = np.minimum((mapped - knee) / (1 - knee), 0.995)
    brightest = knee - (1 - knee) * np.log(1 - t)
    return np.where(mapped <= knee, c, c * brightest / np.maximum(mapped, 1e-30))


def test_hdr_hand_off_undoes_the_tone_curve():
    """the HDR hand-off (PRISM Path Traced's frame to a plugin): INPUTS' untone inverts the composite's curves, so
    FINISH's tone_curved of a plugin that returned its colour unchanged draws the frame the composite drew; the
    shaders' curves are the bake's"""
    import numpy as np
    rng = np.random.default_rng(1)
    light = rng.uniform(0.0, 3.0, (4096, 3)) * rng.uniform(0.0, 1.0, (4096, 1))
    for curve in (0, 1, 2, 3):
        shown = _tone_curved(light, curve)
        if curve == 0:
            shown = np.minimum(shown, 1.0)
        again = _tone_curved(_untone(shown, curve), curve)
        if curve == 0:
            again = np.minimum(again, 1.0)
        # (what is past the curve's last step from white is held there: under 1/255 elsewhere)
        keep = shown.max(axis=-1) < 0.99
        assert np.abs(again - shown)[keep].max() < 1.0 / 255.0, curve
    source = (ROOT / "tools/vulkan_upscale_shaders.py").read_text()
    bake = (VULKAN / "xgpu_vulkan_bake_glsl.inc").read_text()
    for constant in ("2.51 * c + 0.03", "2.43 * c + 0.59) + 0.14", "k = 0.6"):
        assert constant in source and constant in bake, constant


def test_pt_hands_its_frame_to_the_plugin():
    """the path tracer gives its exposure every frame and, in a view begun for a denoiser that upscales, its noisy light
    and guides (composite's source 4); END takes them as the HDR hand-off (flags 16 and 32) and FINISH tone maps"""
    pt = (VULKAN / "xgpu_vulkan_pt.c").read_text()
    glsl = (VULKAN / "xgpu_vulkan_pt.glsl").read_text()
    upscale = (VULKAN / "xgpu_vulkan_upscale.c").read_text()
    assert "xgpu_vulkan_upscale_pt_raw()" in pt and "xgpu_vulkan_upscale_pt_frame(pt.exposure" in pt
    assert "source = 4u;" in pt
    assert "layout(set = 0, binding = 37, rgba16f) uniform image2D exposure_out;" in glsl
    assert "if (source == 4u)" in glsl
    assert "#define INPUT_HDR 16u" in upscale and "#define INPUT_PT_NOISY 32u" in upscale
    assert "up.finish" in upscale and "PRISM_FRAME_HDR" in upscale
    header = (VULKAN / "prism_plugin.h").read_text()
    assert "PRISM_FRAME_HDR = 1u << 0" in header and "struct prism_plugin_image exposure_image;" in header
