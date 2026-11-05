"""The HUD's marks over the world drawn where their points project through the frame's view: a waypoint and the
distance under it (a scenario's flag or object, a game engine goal, Basic Training's spots: hud_nav_points.c), a
teammate's triangle and a player's name (hud.c) and the passenger's reticle (passenger_camera.c). The game places
them in whole units of its 480-line screen, three to four of a 1080p or a laptop's display's pixels: a mark stepped a
unit at a time, short of its point by up to a unit, while the world under it moved smoothly, so it jittered against
the world as the view turned or moved. They are placed where the point projects, the part past the whole units drawn
as an offset of the screen quads and the text (rasterizer.c's rasterizer_screen_offset_set) for that mark alone. The
number under a waypoint is the meters to the nearest tenth: it took the whole meters rounded to the nearest and the
tenth cut short, 108.6 m shown as 109.6."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["waypoint", "distance", "triangle", "name", "passenger_reticle"]
CONTROLS = {
    # the waypoint at the whole unit short of its point (as it was)
    "waypoint_whole": (("\thud_draw_mark_begin(screen_position.x, screen_position.y, &corner);\n",
                        "\thud_draw_mark_begin(screen_position.x, screen_position.y, &corner);\n"
                        "\thud_draw_mark_end();\n"), "waypoint"),
    # the screen quads not moved by the offset
    "quads_unmoved": (("\t\t\tmoved[vertex_index].position.x += rasterizer_screen_offset.i;\n", ""), "waypoint"),
    # the offset left on for the rest of the HUD
    "offset_left_on": (("\thud_draw_mark_end();\n\n\t{\n\t\tshort corrupt_index", "\n\t{\n\t\tshort corrupt_index"),
                       "waypoint"),
    # the distance as it was: the whole meters rounded, the tenth cut short
    "distance_split": (("tenths = (long)floor(distance * 3.048f * 10.0f + 0.5f);",
                        "tenths = fast_ftol_C(distance * 3.048f) * 10 + (long)(distance * 3.048f * 10.0f) % 10;"),
                       "distance"),
    # the triangle at the nearest whole unit (as it was)
    "triangle_whole": (("\t\t\t\t&position);\n", "\t\t\t\t&position);\n\t\t\thud_draw_mark_end();\n"), "triangle"),
    # the text not moved by the offset
    "text_unmoved": (("scaled[vertex_index].position.x = offset.i +", "scaled[vertex_index].position.x = 0.0f +"),
                     "name"),
    # the name at the nearest whole unit (as it was)
    "name_whole": (("\t\tx = whole.x;\n", "\t\tx = whole.x;\n\t\thud_draw_mark_end();\n"), "name"),
    # the passenger's reticle at the whole unit short of the shot (as it was)
    "reticle_whole": (("rest->i = (x - point->x) * reticle_scale;", "rest->i = 0.0f;"), "passenger_reticle"),
}


def generated(fault=None):
    definitions = read("source/interface/hud_definitions.h")
    nav = read("source/interface/hud_nav_points.c")
    rasterizer = read("source/rasterizer/rasterizer.c")
    text_code = read("source/rasterizer/rasterizer_text.c")
    passenger = read("port/linux/game/passenger_camera.c")

    decls = ""
    for name in ("hud_absolute_placement_definition", "hud_placement_definition", "hud_color_definition"):
        decls += structure(definitions, name) + "\n"
    for member in ("_waypoint_on_screen", "_hud_waypoint_dont_rotate_offscreen_bit", "_hud_number_show_trailing_m_bit"):
        decls += enum_with(nav, member) + "\n"
    for name in ("number_hud_element_definition", "hud_waypoint_arrow"):
        decls += structure(nav, name) + "\n"
    assert "static real_vector2d rasterizer_screen_offset;" in rasterizer
    decls += "static real_vector2d rasterizer_screen_offset;\n"
    assert "static boolean reticle_active;" in passenger and "static real_point2d reticle_offset;" in passenger
    decls += "static boolean reticle_active;\nstatic real_point2d reticle_offset;\n"
    decls += ("void rasterizer_psuedo_dynamic_screen_quad_draw(struct rasterizer_dynamic_screen_geometry_parameters "
              "*parameters, struct dynamic_screen_vertex *vertices);\n"
              "void rasterizer_screen_offset_get(real_vector2d *offset);\n")

    text = ""
    for name in ("rasterizer_screen_offset_set", "rasterizer_screen_offset_get",
                 "rasterizer_psuedo_dynamic_screen_quad_draw"):
        text += function(rasterizer, name) + "\n"
    for name in ("rasterizer_text_set_scale", "rasterizer_text_draw_scaled_character"):
        text += function(text_code, name) + "\n"
    hud_draw = read("source/interface/hud_draw.c")
    for name in ("hud_draw_mark_begin", "hud_draw_mark_end"):
        text += function(hud_draw, name) + "\n"
    text += function(read("source/interface/interface.c"), "interface_draw_bitmap") + "\n"
    text += function(nav, "custom_render_nav_point") + "\n"
    hud = read("source/interface/hud.c")
    for name in ("hud_draw_friendly_indicator", "hud_draw_player_name"):
        text += function(hud, name) + "\n"
    text += function(passenger, "passenger_camera_reticle_offset") + "\n"
    if fault:
        text = mutated(text, *fault)
    return (("decls.inc", decls), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("hud_marks", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    fault, case = CONTROLS[control]
    status, output = run(build("hud_marks", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it\n{output}"


def test_every_mark_placed_so():
    # every mark the HUD projects through the view, and only while it draws: begun and ended in each
    nav = function(read("source/interface/hud_nav_points.c"), "custom_render_nav_point")
    hud = read("source/interface/hud.c")
    for code in (nav, function(hud, "hud_draw_friendly_indicator"), function(hud, "hud_draw_player_name")):
        assert "render_camera_view_to_screen(" in code
        assert code.count("hud_draw_mark_begin(") == 1
        # (ended on the way out of the VR mode's early return too)
        assert code.count("hud_draw_mark_end();") == 2
        assert not re.search(r"\(short\)fast_ftol\(screen_position\.[xy]\)", code)
    # the game engine's goals and Basic Training's spots are drawn by custom_render_nav_point
    assert "custom_render_nav_point(" in function(read("source/game/game_engine.c"), "game_engine_render_nav_points")
    assert "custom_render_nav_point(" in read("port/linux/game/basic_training.c")
    # the reticle's rest drawn with it, and cleared after
    meter = function(read("source/interface/hud_draw.c"), "hud_draw_bitmap_with_meter")
    assert meter.index("rasterizer_screen_offset_set(reticle_rest.i, reticle_rest.j);") < \
        meter.index("hud_draw_bitmap_internal(") < meter.index("rasterizer_screen_offset_set(0.0f, 0.0f);")
