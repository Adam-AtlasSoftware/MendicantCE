"""The HUD's clock (display.match_timer, source/interface/hud.c): the match's in multiplayer and, since it no longer
waits for a game engine, the mission's timer in the campaign and co-op, the level's game time. None in a cinematic,
with the setting off, or with the scripts' show_hud off. The player's Hide HUD (controls.hide_hud's key) hides the HUD
and the clock until pressed again, over map loads (which reset the scripts' show_hud) and other settings' changes;
display.hide_hud is its state as the game starts."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["campaign", "multiplayer", "off", "hide", "hide_setting"]
FUNCTIONS = ["hud_match_timer_setting", "hud_hidden_by_player", "hud_shown", "hud_match_timer_shown",
             "hud_draw_match_timer"]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (as it was) the clock in multiplayer alone
    "multiplayer-only": ("hud_match_timer_shown", ("return hud_match_timer_setting() &&",
                                                   "return game_engine_running() && hud_match_timer_setting() &&"),
                         "campaign"),
    # the key held taken as a press each frame (the HUD flickering)
    "held-toggles": ("hud_hidden_by_player", ("if (key && !hud_hide.key_held)", "if (key)"), "hide"),
    # any setting's change taking back the key's choice
    "any-change-resets": ("hud_hidden_by_player", ("\t\tif (setting != hud_hide.setting)\n", ""), "hide"),
}


def generated(fault=None):
    hud = read("source/interface/hud.c")
    config = enum_with(read("port/linux/include/halo_keyboard.h"), "NUMBER_OF_HALO_KEYBOARD_ACTIONS") + "\n"
    hide = re.search(r"^static struct\n\{\n\tboolean hidden;.*?^\} hud_hide = [^\n]*\n", hud, re.M | re.S)
    assert hide, "hud.c's hud_hide"
    code = hide.group(0)
    code += function(read("source/game/game_engine.c"), "ticks_to_unicode_time_string") + "\n"
    for name in FUNCTIONS:
        text = function(hud, name)
        if fault and NEGATIVE_CONTROLS[fault][0] == name:
            text = mutated(text, *NEGATIVE_CONTROLS[fault][1])
        code += text + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("hud_clock", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("hud_clock", generated(control)), NEGATIVE_CONTROLS[control][2])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_every_game_draws_it():
    """hud_draw_screen asks for the clock in every game: hud_draw_match_timer decides."""
    screen = function(read("source/interface/hud.c"), "hud_draw_screen")
    call = screen.index("\t\thud_draw_match_timer();\n")
    assert "game_engine_running()" not in screen[screen.rindex(";", 0, call):call]


def test_the_hud_drawn_by_it():
    """The HUD's drawing asks hud_shown (the scripts' show_hud and the player's Hide HUD), not show_hud alone; its
    sounds stay the scripts'."""
    hud = read("source/interface/hud.c")
    screen = function(hud, "hud_draw_screen")
    assert "\t\tif (hud_shown())\n" in screen
    assert "if (hud_scripted_globals->show_hud)" not in screen
    assert "hud_play_unit_sounds(player, hud_scripted_globals->show_hud);" in screen
