"""The terrain overhaul's water layer, T7 (docs/prism_terrain.md "T7"): its shader is its GLSL, the game's half takes
CE's colour draw's place only where the layer is wanted, and the host's steps and layer fit together. No GPU or game
data is needed."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WATER = ROOT / "source/rasterizer/xbox/rasterizer_xbox_water.c"
TERRAIN = ROOT / "port/linux/game/prism_terrain.c"
STREAM = ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c"
LAYER = ROOT / "port/linux/vulkan/xgpu_vulkan_water.c"
GLSL = ROOT / "port/linux/vulkan/xgpu_vulkan_water.glsl"


@pytest.mark.skipif(not shutil.which("glslangValidator") or not shutil.which("spirv-val"),
                    reason="glslangValidator and spirv-val are not on PATH")
def test_shader_is_its_glsl():
    subprocess.run([sys.executable, "-B", str(ROOT / "tools/vulkan_water_shaders.py"), "--check"], check=True)


def test_the_game_draws_ces_water_unless_the_layer_is_wanted():
    """Classic, multiplayer, display.water "ce" and a backend without ray tracing draw CE's water as they did: the
    layer's path is taken only where prism_terrain_water_wanted, which is prism_terrain_wanted's (display.lighting
    "raster" or "pathtraced", a campaign) and more"""
    text = WATER.read_text()
    assert "prism_terrain_water_wanted();" in text
    wanted = TERRAIN.read_text()
    body = wanted[wanted.index("boolean prism_terrain_water_wanted("):]
    body = body[:body.index("}")]
    assert "prism_terrain_wanted() && prism_terrain.water && xgpu_backend_ray_tracing()" in body
    # (a sky's water stays CE's; a water drawn before the fog takes the layer at its queued draw)
    assert "!TEST_FLAG(group->geometry_flags, _rasterizer_geometry_sky_bit) &&\n\t\t\tprism_terrain_water_wanted()" \
        in text
    assert "_shader_transparent_water_draw_before_fog_bit) &&\n\t\t\tTEST_FLAG(group->geometry_flags, " \
        "_rasterizer_geometry_no_queue_bit)" in text
    # (the steps in order: BEGIN, the ripples, CAPTURED, the base map, CE's depth, END; none while preparing)
    prism = text[text.rindex("static void rasterizer_water_draw_prism(\n\tstruct"):]
    order = [prism.index(s) for s in ("PRISM_TERRAIN_WATER_BEGIN", "_vertex_shader_transparent_water_reflection",
                                      "PRISM_TERRAIN_WATER_CAPTURED", "_vertex_shader_transparent_water_opacity")]
    assert order == sorted(order)
    assert "if (render_enumerating)\n\t\treturn;" in text


def test_the_hosts_steps_and_layer():
    stream = STREAM.read_text()
    # (BEGIN taken at the next draw, once the game's targets are bound; the mask emptied there)
    assert "water_begin_pending();" in stream[stream.index("static void draw_input("):]
    assert "XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_ALPHA" in stream
    # (where the layer cannot be drawn, what was behind the water is put back)
    assert "xgpu_vulkan_image_copy(water_layer.behind, 0, 0, color->image, 0, 0);" in stream
    layer = LAYER.read_text()
    match = re.search(r"#define BINDINGS (\d+)", layer)
    glsl = GLSL.read_text()
    assert match and f"binding = {int(match.group(1)) - 1}, rgba16f" in glsl
    assert "typedef char verify_water_push[sizeof(struct water_push) <= 256" in layer


def test_a_water_before_the_fog_leaves_ces_fog_to_the_floor():
    """c10's swamp (a water CE draws before the fog): nothing of it at its early draw, not even its depth, so that
    CE's environment fog fogs the floor under it as Classic's; the layer takes that fog at its queued draw"""
    text = WATER.read_text()
    assert "if (prism_later)\n\t\t{\n\t\t\t/* port: (drawn at its queued draw) */\n\t\t}\n\t\telse if (draw_depth_only)" \
        in text
    glsl = GLSL.read_text()
    assert "const uint BEFORE_FOG = 8u;" in glsl
    assert "fogged = clamp(dot(was.rgb, fog_color)" in glsl


def test_path_traced_water_rays():
    """T8: the pass "water" of Path Traced traces the reflection and the refraction (TIR from below) through the
    frame's scene, and the composite takes them in the screen march's place"""
    pt_glsl = (ROOT / "port/linux/vulkan/xgpu_vulkan_pt.glsl").read_text()
    assert "//@pass water" in pt_glsl
    water_pass = pt_glsl[pt_glsl.index("//@pass water"):]
    assert "hit_light(o, d, 2000.0, up, length_)" in water_pass
    assert "reference_path(o, d, REFERENCE_BOUNCES, up, true)" in water_pass
    assert "refract(-v, -normal, 1.33)" in water_pass and "reflect(-v, -normal)" in water_pass
    assert '"water")' in (ROOT / "tools/vulkan_pt_shaders.py").read_text()
    pt = (ROOT / "port/linux/vulkan/xgpu_vulkan_pt.c").read_text()
    assert "PASS_WATER," in pt and "Path Traced's water rays" in pt
    stream = STREAM.read_text()
    assert "xgpu_vulkan_pt_water(water_layer.ripples, color->image" in stream
    assert "xgpu_vulkan_pt_water_frame_end();" in stream
    glsl = GLSL.read_text()
    assert "uniform sampler2D traced_up;" in glsl and "uniform sampler2D traced_down;" in glsl
