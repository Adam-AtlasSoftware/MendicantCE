"""display.ambient_occlusion "world" (docs/world-ao.md, port/linux/game/world_ao.c): nothing runs unless the
setting is "world" (off, subtle, strong and the Classic preset draw as before); the hooks where render.c and
scenario.c call it; the command stream's records and the bindings both backends take. Workstreams A and D grow it."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["off", "world", "window", "test"]
GAME = "port/linux/game/world_ao.c"
CONTROLS = [
    # on whatever the setting says
    ('world_ao.mode = setting && !strcmp(setting, "world") ?', 'world_ao.mode = setting ?', "off"),
    # the frame's work done when it is off
    ("\tif (!world_ao_mode())\n\t\treturn;\n\tworld_scene_frame_begin();", "\tworld_scene_frame_begin();", "off"),
    # an object's slot taken from its shape's place, not its owner's slot
    ("owner = (int)world_ao.proxies[index].v[1][3];", "owner = (int)index;", "window"),
    # the reference found without the field faded in
    ("instance = world_ao.volume && world_ao.fade > 0.0f && !baked ?", "instance = !baked ?", "window"),
    # the first-person weapon lit by it
    ("halo_world_ao_object(drawing ? WORLD_AO_OWNER_UNLIT : WORLD_AO_OWNER_NONE, 1.0f);",
     "halo_world_ao_object(WORLD_AO_OWNER_NONE, 1.0f);", "window"),
    # a window given while a map's pipelines are prepared (the world's then made in the load)
    ("\tif (render_enumerating)\n\t{\n\t\tworld_sdf_frame_prepared();\n\t\treturn;\n\t}", "", "window"),
    # the window's span told only while the setting is on (the manifest's world draws then lost)
    ("\t(void)rasterizer_target;\n\t/* (its span, whatever the setting: halo_world_ao_span) */\n"
     "\thalo_world_ao_span(WORLD_AO_SPAN_OPEN);\n", "\t(void)rasterizer_target;\n", "off"),
    ("\thalo_world_ao_span(drawing ? WORLD_AO_SPAN_FIRST_PERSON : WORLD_AO_SPAN_OPEN);\n", "", "off"),
    # the made-up field uploaded each window
    ("\tif (!world_ao_test.uploaded)\n\t\tworld_ao_test_field(camera_position);",
     "\tworld_ao_test_field(camera_position);", "test"),
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
    assert render.index("structure_render_lightmaps();") < render.index("world_ao_window_end();") < \
        render.index("rasterizer_lens_flares_submit_occlusion_tests();")
    assert "world_ao_frame_begin();" in render
    # each object named before its model draws, the first-person weapon unlit, the structure after
    objects = read("source/render/render_objects.c")
    assert objects.index("world_ao_object_begin(object_index);") < objects.index("\t\t\t\t\trender_model(\n")
    assert "world_ao_first_person(TRUE);\n\t\t\tfirst_person_weapon_draw();\n\t\t\tworld_ao_first_person(FALSE);" in \
        objects
    assert objects.index("while (first_person_pass);") < objects.index("world_ao_object_begin(NONE);")
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
