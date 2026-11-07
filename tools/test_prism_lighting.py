"""PRISM's lighting settings (display.prism_lightmaps, display.prism_bloom):
off, they change nothing; on, what they draw is valid.

- The lightmap pass's pixel shaders (tools/prism_lighting/check.c) without
  the key's lightmap_filter read stage 2 as they always did (one sample, no
  smoothing); with it, the lightmap is smoothed (xgpu.h's "The lightmaps
  smoothed") in both emitters, and only where stage 2 is a plain 2D lookup
  (a border keeps the plain sample). The SPIR-V modules pass spirv-val.
- Vulkan's bloom passes are in the post shaders' generated file, and the
  generator's other programs are as they were (tools/vulkan_post_shaders.py
  --check, where glslangValidator and spirv-val are installed).
- The settings are off by default, and "prism" (display.graphics) turns on
  the smooth lightmaps and the world's ambient occlusion, never the bloom.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src"
CC = shutil.which("cc") or shutil.which("clang")
VALIDATOR = shutil.which("spirv-val")
SHARED = ["xgpu.h", "xgpu_backend.h", "xgpu_hdr.h", "xgpu_shader.h", "xgpu_msl.h", "xgpu_spirv.h", "xgpu_stream.h",
          "nv2a_vsh.c", "nv2a_psh.c", "xgpu_text.c", "xgpu_msl.c", "xgpu_spirv.c"]


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    build = tmp_path_factory.mktemp("prism_lighting")
    (build / "src").mkdir()
    for name in SHARED:
        shutil.copy(SOURCE / name, build / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (build / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = build / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{build}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{build / 'src'}", "-o", str(executable), str(ROOT / "tools/prism_lighting/check.c")] +
                   [str(build / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    return executable


@pytest.fixture(scope="module")
def shaders(check):
    text = subprocess.run([str(check), "msl"], check=True, capture_output=True, text=True).stdout
    parts = re.split(r"^==== (\w+) (\d)\n", text, flags=re.M)
    return {(parts[index], int(parts[index + 1])): parts[index + 2] for index in range(1, len(parts), 3)}


def test_msl_off_is_unchanged(shaders):
    for (name, smoothed), source in shaders.items():
        if not smoothed:
            assert "lightmap_smooth" not in source, name
            assert "tex2.sample(" in source, name


def test_msl_smoothed(shaders):
    for name in ("lightmap", "lightmap_dynamic", "lightmap_world"):
        source = shaders[name, 1]
        assert "static float4 lightmap_smooth(" in source, name
        assert "t2 = lightmap_smooth(tex2, tex2_sampler," in source, name
        assert "float4 frag_position [[position]];" in source, name
    # (a border on the stage: the plain sample)
    assert "lightmap_smooth" not in shaders["lightmap_border", 1]


def test_spirv(check, tmp_path):
    subprocess.run([str(check), "spirv", str(tmp_path)], check=True)
    modules = {path.stem: path.read_bytes() for path in tmp_path.glob("*.spv")}
    assert len(modules) == 8
    for name in ("lightmap", "lightmap_dynamic", "lightmap_world"):
        # (the smoothed one is longer: its taps, gathers and dither)
        assert len(modules[f"{name}-1"]) > len(modules[f"{name}-0"]) + 400, name
    assert modules["lightmap_border-1"] == modules["lightmap_border-0"]
    if not VALIDATOR:
        pytest.skip("no spirv-val")
    for path in sorted(tmp_path.glob("*.spv")):
        subprocess.run([VALIDATOR, "--target-env", "vulkan1.3", str(path)], check=True)


def test_post_shaders_generated():
    if not shutil.which("glslangValidator") or not VALIDATOR:
        pytest.skip("no glslangValidator or spirv-val")
    subprocess.run(["python3", str(ROOT / "tools/vulkan_post_shaders.py"), "--check"], check=True,
                   capture_output=True)
    generated = (ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c").read_text()
    assert "bloom_down_frag" in generated and "bloom_up_frag" in generated


def test_settings_off_by_default():
    config = (SOURCE / "port_config.c").read_text()
    assert re.search(r'"display\.prism_lightmaps", _config_string, "\\"stock\\""', config)
    assert re.search(r'"display\.prism_bloom", _config_string, "\\"off\\""', config)
    assert '{ "display.prism_lightmaps", "\\"stock\\"", 0, "\\"smooth\\"" }' in config
    assert '{ "display.ambient_occlusion", "\\"off\\"", 0, "\\"world\\"" }' in config
    assert "display.prism_bloom\", \"" not in config.split("config_graphics[] =", 1)[1].split("};", 1)[0]


def test_prism_preset_is_faithful():
    """display.graphics "prism": the model light's faithful choices on, off in classic and enhanced; what is beyond
    CE (bloom, the remodelled level geometry) and the local reflections (they change the scene's colour) in no
    preset"""
    config = (SOURCE / "port_config.c").read_text()
    for name, default in (("prism_specular", '"off"'), ("prism_shadows", '"stock"'), ("prism_reflections", '"stock"'),
                          ("prism_world_geometry", '"stock"')):
        assert re.search(r'"display\.%s", _config_string, "%s"' % (name, re.escape(default).replace('"', '\\\\"')),
                         config), name
    assert re.search(r'"display\.prism_surface_maps", _config_boolean, "false"', config)
    table = config.split("config_graphics[] =", 1)[1].split("};", 1)[0]
    assert '{ "display.prism_surface_maps", "false", 0, "true" }' in table
    assert '{ "display.prism_specular", "\\"off\\"", 0, "\\"subtle\\"" }' in table
    assert '{ "display.prism_shadows", "\\"stock\\"", 0, "\\"soft\\"" }' in table
    for name in ("prism_bloom", "prism_world_geometry", "prism_reflections"):
        assert '"display.%s"' % name not in table, name
