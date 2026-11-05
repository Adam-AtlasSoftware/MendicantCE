"""The Metal backend's anti-aliasing passes (tools/metal_post_shaders.py,
port/macos/xgpu_metal_post_shaders.c) and where the front end runs them.

The generated file must still be what the tool writes from Vulkan's GLSL
(tools/vulkan_post_shaders.py) and SMAA.hlsl (with glslangValidator and spirv-cross on PATH), each function
must compile for the Mac (with Xcode's Metal compiler), and the game must
run the pass over the 3D view after the dynamic lights' glows and before
the HUD and menus, as the Vulkan backend draws it. The passes'
drawing itself is checked on the GPU by port/macos/tests/xgpu_metal_test.m
(tools/test_macos_metal.py)."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GENERATED = ROOT / "port/macos/xgpu_metal_post_shaders.c"
FUNCTIONS = ("xgpu_post_vertex", "xgpu_post_fxaa", "xgpu_post_smaa_edges", "xgpu_post_smaa_weights",
             "xgpu_post_smaa_blend")


def test_functions_named():
    text = GENERATED.read_text()
    for name in FUNCTIONS:
        assert f'"{name}" }}' in text, name
        # (the uniforms are the backend's draw uniforms, xgpu_metal_bindings.h)
    assert "[[buffer(17)]]" in text
    assert "xgpu_metal_post_area_texture[]" in text and "xgpu_metal_post_search_texture[]" in text
    # (display.ambient_occlusion is found in the world, in the surfaces' own
    # shaders: no pass over the screen is left)
    assert "xgpu_occlusion" not in text and "sampling" not in text


@pytest.mark.skipif(not (shutil.which("glslangValidator") and shutil.which("spirv-cross")),
                    reason="needs glslangValidator and spirv-cross")
def test_generated_matches(tmp_path):
    arguments = [sys.executable, str(ROOT / "tools/metal_post_shaders.py"), "--check", "--build-dir", str(tmp_path)]
    if sys.platform == "darwin" and shutil.which("xcrun"):
        arguments.append("--metal")
    subprocess.run(arguments, check=True)


def test_pass_before_the_hud():
    """render_window: the glows, then the anti-aliasing pass over the window's
    3D view, then the HUD and menus (interface_draw_screen)."""
    render = (ROOT / "source/render/render.c").read_text()
    # (the definition: the last of its declaration and definition)
    body = render[render.rindex("static void render_window("):]
    body = body[:body.index("\n}\n")]
    halos = body.index("halo_dynamic_lights_halos_draw();")
    anti_alias = body.index("halo_screen_anti_alias(")
    hud = body.index("interface_draw_screen();")
    assert halos < anti_alias < hud


def test_every_build_draws_smaa():
    """Every build takes "smaa" as SMAA (Vulkan's passes, and Metal's)."""
    device = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    values = device[device.index("anti_aliasing_values[] ="):]
    values = values[:values.index("};")]
    assert re.findall(r'\{ "smaa", (\w+), 0 \}', values) == ["_anti_aliasing_smaa"]
