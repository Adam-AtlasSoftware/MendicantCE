"""display.vehicle_fov (view_fov.c's render_fov_vertical, render_fov_reticle_scale): a seat's view, first or third
person, and the moments of entering and leaving it take the vehicle's field of view ("default": the game's own, its
reticle at the stock size; "match": display.fov's; a number: its angle), on foot display.fov's. Getting in and out the
view eases between them, and each frame's reticle is scaled from that frame's view (the Shade's turret, #441); death,
the director's neutral view, a scripted camera and a scope's full zoom keep the game's."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["turret_default", "turret_match", "turret_number", "third_person_seat", "turret_exit",
         "leaving_third_person", "match_exit", "entering", "turned_back", "respawn", "scoped_in_seat", "dead",
         "neutral", "scripted", "wider_with_panini", "past_the_widest", "zoomed_reticle_size"]
CONTROLS = [
    # display.vehicle_fov unread: the seat at display.fov's (#449's)
    ("\t\trequested_tangent = vehicle_tangent;", "\t\trequested_tangent = foot_tangent;", "turret_default"),
    ("\t\trequested_tangent = vehicle_tangent;", "\t\trequested_tangent = foot_tangent;", "turret_number"),
    # "match" read as "default"
    ("\t\t\tvehicle_tangent = foot_tangent;", "\t\t\tvehicle_tangent = 0.0f;", "turret_match"),
    # a number unread
    ("\t\t\tvehicle_tangent = fov_tangent(degrees);", "\t\t\tvehicle_tangent = 0.0f;", "turret_number"),
    # no way between them: the view and its reticle jump as the player gets out or in
    ("real eased = progress * progress * (3.0f - 2.0f * progress);", "real eased = 1.0f;", "turret_exit"),
    ("real eased = progress * progress * (3.0f - 2.0f * progress);", "real eased = 1.0f;", "entering"),
    # turning back from the way's end, not from where it is
    ("vehicle_blends[local_player_index].from = vehicle_blends[local_player_index].weight;\n\t\tvehicle_blends[local_player_index].start = now;\n\t}",
     "vehicle_blends[local_player_index].from = in_vehicle ? 0.0f : 1.0f;\n\t\tvehicle_blends[local_player_index].start = now;\n\t}",
     "turned_back"),
    # the way kept across death: a respawned view eases from the seat's
    ("\t\tvehicle_blends[local_player_index].valid = FALSE;\n", "\n", "respawn"),
    # the first person alone: the following camera's seats at the game's view
    ("perspective != _director_perspective_third_person", "TRUE", "third_person_seat"),
    # above 150 without the projection
    ("strength > 0.0f || degrees <= FIELD_OF_VIEW_MOST", "TRUE", "wider_with_panini"),
    # not above 150 with it
    ("strength > 0.0f || degrees <= FIELD_OF_VIEW_MOST", "degrees <= FIELD_OF_VIEW_MOST", "wider_with_panini"),
    # no widest: any angle below 180
    ("degrees <= PANINI_FIELD_OF_VIEW_MOST))", "degrees < 180.0))", "past_the_widest"),
    # (#594) the reticle sized by the zoomed view: it grows as the scope zooms in
    ("\t\treticle_sizes[local_player_index] = reticle_size;", "\t\treticle_sizes[local_player_index] = scale;",
     "zoomed_reticle_size"),
    # display.panini's magnification asked of the zoomed view, not the unzoomed one
    ("\t\t\t\treticle_widenings[local_player_index] = widening;", "\t\t\t\treticle_widenings[local_player_index] = 1.0f;",
     "zoomed_reticle_size"),
    # its size the requested view's alone, not the stock view's against it
    ("*reticle_size = PIN(unzoomed_tangent / requested_tangent, 0.001f, 1000.0f);",
     "*reticle_size = PIN(1.0f / requested_tangent, 0.001f, 1000.0f);", "zoomed_reticle_size"),
    # left as the last frame's where the view is the game's own (death)
    ("\t*reticle_size = 1.0f;\n\t*unzoomed_drawn_tangent",
     "\t*reticle_size = reticle_sizes[local_player_index];\n\t*unzoomed_drawn_tangent", "dead"),
    # the director's neutral view widened
    ("perspective != _director_perspective_first_person &&", "perspective == _director_perspective_scripted &&",
     "neutral"),
]


def generated(fault=None):
    code = read("port/linux/game/view_fov.c")
    world = code[code.index("/* ---------- the world's field of view */"):
                 code.index("/* ---------- the mouse's turn zoomed")]
    # (display.fov's widest, and above 150 with the projection alone: panini_projection.h)
    header = read("port/linux/include/panini_projection.h")
    widest = "\n".join(line for line in header.splitlines() if "FIELD_OF_VIEW_MOST " in line and
                       line.startswith("#define")) + "\n" + function(header, "panini_field_of_view").replace("static __inline", "static", 1)
    world = widest + "\n" + world
    if fault:
        world = mutated(world, *fault)
    config = "\n".join([
        enum_with(read("source/camera/director.h"), "_director_perspective_first_person"),
        enum_with(read("source/objects/objects.h"), "_object_dead_bit"),
    ]) + "\n"
    text = "\n".join([
        function(read("source/render/render_cameras.c"), "render_camera_get_adjusted_field_of_view_tangent"),
        # (the zoom's arithmetic, display.scale_scoped_fov's: the game's own header)
        read("port/linux/include/zoom_fov.h"),
        world,
    ])
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("vehicle_fov", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("vehicle_fov", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_reticle_takes_it():
    # the HUD scales a centred reticle by it, and the passenger view's offset (a place in the drawn view) is given at
    # the HUD's own size, so that scaling it lands it there, at any vehicle field of view
    hud = read("source/interface/hud_draw.c")
    # (display.panini's magnification of the middle too: test_panini.py; the weapon's reticle at its size, which a
    # zoom keeps, a scope's own pieces with the view: #594)
    assert "\t\treticle_scale = is_crosshair_bitmap == HUD_CROSSHAIR_SCOPE_PIECE ?\n" \
        "\t\t\trender_fov_reticle_scale(render.local_player_index) * panini_center_scale() :\n" \
        "\t\t\trender_fov_reticle_size(render.local_player_index);" in hud
    assert "\t\tcolor,\n\t\treticle_scale);" in hud
    # the passenger's and the lowered reticle's offsets are given at the scale the reticle is drawn at
    assert "!passenger_camera_reticle_offset(&point, reticle_scale, &reticle_rest))\n" \
        "\t\tlowered_reticle_offset_point(&point, reticle_scale);" in hud
    passenger = read("port/linux/game/passenger_camera.c")
    assert "real x = point->x + reticle_offset.x / reticle_scale;" in passenger
    assert "real y = point->y + reticle_offset.y / reticle_scale;" in passenger
    # (and what is past the whole units, at the scale it is drawn at: hud_draw.c draws the reticle moved by it)
    assert "rest->i = (x - point->x) * reticle_scale;" in passenger
    assert "rasterizer_screen_offset_set(reticle_rest.i, reticle_rest.j);" in hud
    # a scope's pieces are the items drawn only zoomed (the VR smart-link's zoomed layout's)
    weapon = read("source/interface/hud_weapon.c")
    assert "HUD_CROSSHAIR_SCOPE_PIECE : TRUE);" in weapon


def test_the_setting():
    # a string setting ("default" by default) that config.toml may also give as a number, and a slider on First Person
    config = read("port/linux/src/port_config.c")
    assert '{ "display.vehicle_fov", _config_string, "\\"default\\"", "HALO_VEHICLE_FOV",' in config
    assert '!strcmp(setting->name, "display.vehicle_fov")' in config
    menu = read("port/linux/game/menu_functions.c")
    assert '{ "op_vehicle_fov", "display.vehicle_fov",' in menu
