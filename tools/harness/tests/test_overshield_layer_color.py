"""The overshield's color by its layer (port/linux/game/overshield_effect.c, display.overshield_layer_color): green
while the green layer the HUD's shield meter shows lasts, red once only the red is left, red throughout with the
setting off. And where it is wired: the flare on the body and the arms (overshield_effect_color) and its glow
(shield_glow.c) take the unit's layer's color; the setting's default and its Shields row."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["powerup", "full", "decayed", "red_layer", "past_three", "off", "no_unit"]
CONTROLS = [
    # the layer's edge taken as reached: red at 2.5
    ("shield_vitality > (real)(layer + 2)", "shield_vitality > (real)(layer + 3)", "decayed"),
    # the edge held: green with the green layer gone
    ("shield_vitality > (real)(layer + 2)", "shield_vitality >= (real)(layer + 2)", "red_layer"),
    # the setting unread: layers when off
    ("while (layers && ", "while (", "off"),
    # read once, not on a change
    ("if (changes != config_changes())", "if (changes == (unsigned long)-1)", "off"),
    # not held at the table's end
    ("layer + 1 < (short)NUMBEROF(overshield_effect_layer_rgb) &&", "", "past_three"),
    # the layers' colors in the wrong order: green first
    ("{ 0.992f, 0.306f, 0.455f }, /* (253, 78, 116) */\n\t{ 0.306f, 0.992f, 0.455f },",
     "{ 0.306f, 0.992f, 0.455f }, /* (253, 78, 116) */\n\t{ 0.992f, 0.306f, 0.455f },", "full"),
]
SOURCE = "port/linux/game/overshield_effect.c"


def generated(fault=None):
    source = read(SOURCE)
    table = re.search(r"static real_rgb_color const overshield_effect_layer_rgb\[\] =.*?\};", source, re.S).group(0)
    code = "\n".join([table, function(source, "overshield_effect_layers_enabled"),
                      function(source, "overshield_effect_layer_color"),
                      function(source, "overshield_effect_unit_color")])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("overshield_layer_color", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("overshield_layer_color", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_wired():
    source = read(SOURCE)
    color = function(source, "overshield_effect_color")
    assert "*color = *overshield_effect_unit_color(effect->unit_index);" in color, "the flare: the unit's layer's"
    assert "return overshield_effect_unit_color(unit_index);" in function(source, "overshield_effect_glow_color")
    assert "*color = *overshield_effect_glow_color(unit_index);" in read("port/linux/game/shield_glow.c"), \
        "the glow: its unit's layer's"
    # (only drawing: nothing of the simulation's is written)
    assert "shield_vitality =" not in "\n".join(
        function(source, name) for name in ("overshield_effect_layer_color", "overshield_effect_unit_color"))
    config = read("port/linux/src/port_config.c")
    assert re.search(r'\{ "display.overshield_layer_color", _config_boolean, "true", "HALO_OVERSHIELD_LAYER_COLOR"',
                     config), "on by default"
    shields = read("port/assets/menus/ce/main_menu.settings_select.player_setup.player_profile_edit."
                   "video_settings.first_person.shields.xml")
    assert "display.overshield_layer_color" in shields, "its row on the Shields page"
