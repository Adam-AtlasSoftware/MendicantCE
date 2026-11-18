"""The input display of port/linux/game/input_display.c (display.input_display): what it keeps of each tick's actions
and draws of them; that the actions it reads are the tick's as taken, a demo's playback's recorded ones too
(players.c); that the HUD draws it only while the HUD is shown (Hide HUD); off, nothing kept."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

SOURCE = "port/linux/game/input_display.c"
CASES = ["tick", "off", "wrap", "garbage", "keys", "style", "fit"]
FUNCTIONS = ("input_display_wrap", "input_display_note", "input_display_actions_held", "input_display_keys_lit",
             "input_display_keyboard_style", "input_display_enabled", "input_display_tick", "input_display_rectangle",
             "input_display_layout")

# a fault in input_display.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "kept-while-off": (("\tif (!input_display_enabled())\n\t\treturn;\n", ""), "input_display_tick", "off"),
    "turn-not-wrapped": (("input_display_wrap(action->desired_facing.yaw - player->last_yaw)",
                          "(action->desired_facing.yaw - player->last_yaw)"), "input_display_note", "wrap"),
    "left-and-right-swapped": (("action->throttle.j > INPUT_DISPLAY_KEY_PUSH", "action->throttle.j < -1.0f"),
                               "input_display_keys_lit", "keys"),
    "keys-in-a-playback": (("keyboard_prompts && !demo_playing", "keyboard_prompts"),
                           "input_display_keyboard_style", "style"),
    "garbage-taken-as-a-turn": (("\tif (!(angle > -100.0f && angle < 100.0f))\n\t\treturn 0.0f;\n", ""),
                                "input_display_wrap", "garbage"),
    "grenade-not-held": (("_unit_control_throw_grenade_bit, ", "_unit_control_force_alert_bit, "),
                         "input_display_actions_held", "tick"),
    # (the first picture's cells and keys, whose labels touched their edges)
    "cells-as-narrow-as-before": (("INPUT_DISPLAY_CELL_WIDTH = 36,", "INPUT_DISPLAY_CELL_WIDTH = 30,"), "constants",
                                  "fit"),
    "keys-as-small-as-before": (("INPUT_DISPLAY_KEY_WIDTH = 12,", "INPUT_DISPLAY_KEY_WIDTH = 9,"), "constants", "fit"),
    "panel-past-a-split-views-top": (("short top = (short)(bottom - INPUT_DISPLAY_HEIGHT);",
                                      "short top = (short)(bottom - 4 * INPUT_DISPLAY_HEIGHT);"),
                                     "input_display_layout", "fit"),
}


def generated(fault=None, faulty_function=None):
    source = read(SOURCE)
    units = read("source/units/units.h")
    text = re.search(r"^union rectangle2d\n\{.*?^\};\ntypedef union rectangle2d rectangle2d;\n",
                     read("source/math/integer_math.h"), re.M | re.S).group(0)
    text += enum_with(units, "_unit_control_jump_bit") + "\n"
    text += structure(read("source/game/players.h"), "player_action") + "\n"
    enums = "".join(match.group(0) for match in re.finditer(r"^enum\n\{\n.*?^\};\n", source, re.M | re.S))
    text += mutated(enums, *fault) if fault and faulty_function == "constants" else enums
    text += "".join(re.findall(r"^#define INPUT_DISPLAY_\w+ [^\n]+\n", source, re.M))
    text += structure(source, "input_display_player") + "\n"
    text += structure(source, "input_display_layout") + "\n"
    text += re.search(r"^static struct input_display_player input_display_players\[[^\n]+;\n", source, re.M).group(0)
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("input_display", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("input_display", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"input_display.c with a fault ({control}) passed '{case}': the test cannot see it"


def _reads_the_ticks_actions(players):
    """players_update_before_game gives the display the actions after a playback put in the recorded ones (or the
    bots theirs), and only when the tick has actions"""
    update = function(players, "players_update_before_game")
    playback = update.find("if (demo_playback_tick_actions(actions))")
    taken = update.find("\tif (have_actions)\n\t{", playback)
    call = update.find("input_display_tick(actions, NETWORK_GAME_MAXIMUM_PLAYER_COUNT);")
    return 0 <= playback < taken < call and update.count("input_display_tick(") == 1


def _drawn_with_the_hud(hud):
    """the HUD draws it only while it is shown: the scripts' show_hud and the player's Hide HUD"""
    screen = function(hud, "hud_draw_screen")
    return bool(re.search(r"if \(hud_shown\(\) && !cinematic_in_progress\(\)\)\n\t+input_display_draw\(", screen))


def test_reads_the_ticks_actions_a_playbacks_too():
    assert _reads_the_ticks_actions(read("source/game/players.c"))


def test_reads_the_ticks_actions_negative_control():
    """the check sees a display fed before the playback put in the recorded input"""
    players = read("source/game/players.c")
    call = "\t\tinput_display_tick(actions, NETWORK_GAME_MAXIMUM_PLAYER_COUNT);\n"
    moved = players.replace(call, "", 1).replace("\tif (demo_playback_tick_actions(actions))",
                                                  "\tinput_display_tick(actions, NETWORK_GAME_MAXIMUM_PLAYER_COUNT);\n"
                                                  "\tif (demo_playback_tick_actions(actions))", 1)
    assert moved != players and not _reads_the_ticks_actions(moved)


def test_hidden_by_hide_hud():
    hud = read("source/interface/hud.c")
    assert _drawn_with_the_hud(hud)
    assert not _drawn_with_the_hud(hud.replace("if (hud_shown() && !cinematic_in_progress())\n",
                                               "if (!cinematic_in_progress())\n", 1))


def test_off_by_default():
    assert '{ "display.input_display", _config_boolean, "false",' in read("port/linux/src/port_config.c")
    draw = function(read(SOURCE), "input_display_draw")
    assert "if (!input_display_enabled() || player_index == NONE)\n\t\treturn;" in draw
