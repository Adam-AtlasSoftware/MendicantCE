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
