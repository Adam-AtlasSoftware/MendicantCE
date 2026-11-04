"""display.ambient_occlusion "world" (docs/world-ao.md, port/linux/game/world_ao.c): nothing runs unless the
setting is "world" (off, subtle, strong and the Classic preset draw as before); the hooks where render.c and
scenario.c call it; the command stream's records and the bindings both backends take. Workstreams A and D grow it."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["off", "world"]
GAME = "port/linux/game/world_ao.c"
CONTROLS = [
    # on whatever the setting says
    ('world_ao.mode = setting && !strcmp(setting, "world") ?', 'world_ao.mode = setting ?', "off"),
    # the frame's work done when it is off
    ("void world_ao_frame_begin(void)\n{\n\tif (!world_ao_mode())\n\t\treturn;",
     "void world_ao_frame_begin(void)\n{", "off"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    headers = [(name, read(f"port/linux/include/{name}"))
               for name in ("world_ao.h", "world_proxies.h", "world_scene.h", "world_sdf.h")]
    return tuple(headers) + (("under_test.inc", game),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_ao", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_ao", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_hooks():
    render = read("source/render/render.c")
    # the window's block handed on before the opaque surfaces, taken away after the lightmap pass
    assert render.index("world_ao_window_begin(") < render.index("\t\trender_objects();\n\t\t/* port: and through")
    assert render.index("halo_screen_ambient_occlusion_end();") < render.index("world_ao_window_end();") < \
        render.index("rasterizer_lens_flares_submit_occlusion_tests();")
    assert "world_ao_frame_begin();" in render
    scenario = read("source/scenario/scenario.c")
    for hook in ("world_ao_map_loaded();", "world_ao_map_unloaded();",
                 "world_ao_bsp_switched(global_structure_bsp_index);"):
        assert hook in scenario, hook


def test_the_classic_preset_bypasses_it():
    # Classic sets display.ambient_occlusion "off", which world_ao_mode reads as off
    assert '{ "display.ambient_occlusion", "\\"off\\"" },' in read("port/linux/src/port_config.c")
    assert '#define GRAPHICS_AMBIENT_OCCLUSION "\\"off\\""' in read("port/linux/src/port_config.c")


def test_the_stream_and_the_bindings():
    stream = read("port/linux/src/xgpu_stream.h")
    assert "XGPU_STREAM_WORLD_AO_WINDOW," in stream and "XGPU_STREAM_WORLD_AO_FIELD," in stream
    decode = read("port/android/host/host_render_stream.c")
    assert "[XGPU_STREAM_WORLD_AO_WINDOW] = sizeof(struct xgpu_stream_world_ao_window)," in decode
    assert "[XGPU_STREAM_WORLD_AO_FIELD] = sizeof(struct xgpu_stream_world_ao_field)," in decode
    # (in #574's set: a draw binds at most four)
    spirv = read("port/linux/src/xgpu_spirv.h")
    assert "#define XGPU_SPIRV_BINDING_WORLD_SDF 1" in spirv and "#define XGPU_SPIRV_BINDING_WORLD_AO 2" in spirv
    msl = read("port/linux/src/xgpu_msl.h")
    metal = read("port/macos/xgpu_metal_bindings.h")
    assert "#define XGPU_MSL_TEXTURE_WORLD_SDF 5" in msl and "#define XGPU_METAL_TEXTURE_WORLD_SDF 5" in metal
    assert "#define XGPU_MSL_BUFFER_WORLD_AO 23" in msl and "#define XGPU_METAL_BUFFER_WORLD_AO 23" in metal
