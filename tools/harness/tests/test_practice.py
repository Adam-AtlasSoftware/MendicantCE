"""The practice keys and the PRACTICE mark of port/linux/game/practice.c: every tool sets the mark and nothing else
does; only a fresh start clears it; the keys act once a press and only where they may; a demo's playback shows the
recording's mark. And where the mark is kept: the console's lines, the fresh start, the speedrun block, the demo."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

SOURCE = "port/linux/game/practice.c"
CASES = ["every_tool", "fresh_start", "keys", "playback"]
FUNCTIONS = ("practice_mark", "practice_clear", "practice_line_tool", "practice_cheats_on", "practice_keys_pressed",
             "practice_note", "practice_read_settings", "practice_keys_usable", "practice_keys_frame",
             "practice_marked", "practice_reason", "practice_console_line", "practice_new_level", "practice_frame",
             "practice_playback_mark")

# a fault in practice.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "game-speed-unseen": (('\t\tif (!strcmp(word, "game_speed"))\n\t\t\treturn "game_speed";\n', ""),
                          "practice_line_tool", "every_tool"),
    "key-without-mark": (('\tpractice_note("practice key");\n', ""), "practice_keys_frame", "every_tool"),
    "fresh-start-keeps-it": (("\tpractice_clear(&practice_state);\n}", "\t(void)0;\n}"), "practice_new_level",
                             "fresh_start"),
    "keys-off-clear-it": (("\telse\n\t\tpractice_settings.held = 0;\n",
                           "\telse\n\t{\n\t\tpractice_settings.held = 0;\n\t\tpractice_clear(&practice_state);\n\t}\n"),
                          "practice_frame", "fresh_start"),
    "cheat-left-on-unseen": (("cheats->deathless_player || ", ""), "practice_cheats_on", "fresh_start"),
    "keys-while-paused": (("!game_time_get_paused()", "TRUE"), "practice_keys_usable", "keys"),
    "key-held-repeats": (("return held & ~last_held;", "return held;"), "practice_keys_pressed", "keys"),
    "playback-takes-own-use": (("\tif (demo_playing())\n\t\treturn;\n\tif (practice_mark(",
                                "\tif (practice_mark("), "practice_note", "playback"),
}


def generated(fault=None, faulty_function=None):
    source = read(SOURCE)
    constants = source[source.index("/* ---------- constants */"):source.index("/* ---------- prototypes */")]
    globals_ = source[source.index("/* ---------- globals */"):source.index("/* ---------- private code */")]
    text = structure(read("source/game/cheats.h"), "cheat_globals") + "\nstatic struct cheat_globals cheat;\n"
    text += constants + structure(source, "practice_state") + "\n" + globals_
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("practice", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("practice", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"practice.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_the_mark_is_kept_everywhere():
    """every console line is looked at before it runs; every fresh start clears; the main loop keeps it before the
    speedrun block reads it; the block and the demo record it; the HUD draws it outside Hide HUD's test"""
    evaluate = function(read("source/hs/hs.c"), "hs_compile_and_evaluate")
    assert evaluate.index("practice_console_line(expression);") < evaluate.index("terminal_command_running = TRUE;")
    assert "practice_new_level();" in function(read("source/game/game.c"), "game_initialize_for_new_map")
    main = read("source/main/main.c")
    assert main.index("\t\tpractice_frame();") < main.index("\t\tspeedrun_state_frame(")
    assert "input.practice = practice_marked();" in function(read("port/linux/game/speedrun_state.c"),
                                                             "speedrun_state_frame")
    demo = read("port/linux/game/demo.c")
    begin = function(demo, "demo_record_tick_begin")
    assert begin.count("demo_record_practice(tick);") == 2
    assert "practice_playback_mark(bytes[4] != 0, reason);" in function(demo, "demo_playback_tick_begin")
    screen = function(read("source/interface/hud.c"), "hud_draw_screen")
    assert re.search(r"\n\t\tpractice_draw\(\);", screen) and "if (hud_shown())\n\t\t\tpractice_draw" not in screen


def test_keys_off_by_default_and_unbound():
    config = read("port/linux/src/port_config.c")
    assert '{ "game.practice_keys", _config_boolean, "false",' in config
    for key in ("save", "load", "restart"):
        assert f'{{ "game.practice_{key}_key", _config_string, "\\"\\"",' in config
