"""The on-screen keyboard (#1135: port/linux/game/keyboard_core.c and keyboard.c; screens/keyboard.xml): the QWERTY
layout table and its pages; a held key's accents, only those the text takes (a name: Latin-1 and Latin
Extended-A; a menu's field, a byte each: Latin-1; a chat line: none) and the font draws; the controller's
shortcuts (X deletes and repeats, Y a space, LB and RB the cursor, Start done, B cancel, the accents closed first);
a name, a field and a chat line typed through it; its fit at 4:3 to 32:9 and in split screen's views; and which
keyboard each mode has (the new menus the new one, the original-faithful modes the game's own), decided in
keyboard.c alone. Each with a negative control."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, function, mutated, read, run  # noqa: E402

CASES = ["layout", "paging", "accents", "shortcuts", "round-trip", "fit", "modes"]

CORE = "port/linux/game/keyboard_core.c"
GLUE = "port/linux/game/keyboard.c"

# a fault in the code (in the core, or keyboard.c), and the case that must catch it
NEGATIVE_CONTROLS = {
    # a sign gone from the table
    "sign-gone": ("core", ("SIGN('\\\\'), ", ""), "layout"),
    # the page key showing its own page
    "page-stays": ("core", ("\tcase KEYBOARD_KEY_PAGE:\n\t\tkeyboard_page(state, key->page);",
                            "\tcase KEYBOARD_KEY_PAGE:\n\t\tkeyboard_page(state, state->page);"), "paging"),
    # every accent offered, whatever the text takes or the font draws
    "accents-unfiltered": ("core", ("\t\tif (keyboard_offered(state, variants[index]))\n", "\t\tif (1)\n"),
                           "accents"),
    # X a space, not a delete
    "x-swapped": ("glue", ("keyboard_repeat_step(&keyboard.repeat_x, TEST_FLAG(held, KEYBOARD_PAD_X), now)",
                           "keyboard_repeat_step(&keyboard.repeat_x, TEST_FLAG(held, KEYBOARD_PAD_Y), now)"),
                  "shortcuts"),
    # a held button acting every frame
    "no-repeat-delay": ("core", ("\tif ((long)(now - repeat->next) < 0)\n\t\treturn 0;\n", ""), "shortcuts"),
    # a name's first letter not a capital
    "no-capital": ("core", ("\tif (state->capital_first && state->length == 0 && !state->caps)",
                            "\tif (0)"), "round-trip"),
    # the bigger keys too wide for the 640
    "too-wide": ("core", ("geometry->key_unit = (short)(large ? 58 : 52);",
                          "geometry->key_unit = (short)(large ? 66 : 52);"), "fit"),
    # the classic modes given the new keyboard
    "one-keyboard": ("glue", ("\tif (!keyboard_new())\n\t\treturn virtual_keyboard_launch(text, size, caption_index);\n",
                              ""), "modes"),
}

GLUE_FUNCTIONS = ("keyboard_name_takes", "keyboard_field_takes", "keyboard_chat_takes", "keyboard_result",
                  "keyboard_frame", "keyboard_name_launch", "keyboard_field_launch", "keyboard_active",
                  "keyboard_last_kept", "keyboard_close")


def generated(fault=None):
    core = read(CORE)
    glue = read(GLUE)
    header = read("port/linux/game/keyboard.h")
    if fault:
        where, change, _ = NEGATIVE_CONTROLS[fault]
        if where == "core":
            core = mutated(core, *change)
        else:
            glue = mutated(glue, *change)
    request = (header[header.index("enum keyboard_kind"):header.index("};", header.index("enum keyboard_kind")) + 2] +
               "\n" + header[header.index("struct keyboard_request\n"):
                             header.index("};", header.index("struct keyboard_request\n")) + 2] + "\n")
    enums = "\n".join(glue[start:glue.index("};", start) + 2] for start in
                      (glue.index("/* the menus' sounds"), glue.index("/* a controller's buttons, as the keyboard")))
    state = glue[glue.index("static struct\n{\n\t/* one is up"):glue.index("} keyboard;") + len("} keyboard;")]
    prelude = ("static boolean keyboard_new(void);\nstatic boolean keyboard_open(struct keyboard_request const *);\n"
               "static void keyboard_finish(boolean kept);\nstatic void keyboard_keys(void);\n"
               "static unsigned long keyboard_pad_read(void);\n"
               "static wchar_t keyboard_title[64], keyboard_caption[128];\n")
    code = "\n".join([enums, state, prelude] + [function(glue, name) for name in GLUE_FUNCTIONS]) + "\n"
    code = code.replace("\nboolean keyboard_", "\nstatic boolean keyboard_").replace("\nvoid keyboard_", "\nstatic void keyboard_")
    code = re.sub(r"^boolean keyboard_", "static boolean keyboard_", code, flags=re.M)
    code = re.sub(r"^void keyboard_", "static void keyboard_", code, flags=re.M)
    return (("keyboard_core.h", read("port/linux/game/keyboard_core.h")),
            ("keyboard_core.inc", core.replace('#include "keyboard_core.h"', "")),
            ("keyboard_request.inc", request), ("keyboard_glue.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_keyboard(case):
    status, output = run(build("keyboard", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("keyboard", generated(control)), NEGATIVE_CONTROLS[control][2])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


# the game's own keyboard's and the chat's, which only keyboard.c (the mode's choice), the game's keyboard itself
# and the menus' hooks for it (ui_widget.c, interface.c) may name
OLD_KEYBOARD_CALLS = re.compile(r"\bvirtual_keyboard_(launch|port_launch|last_exit_saved_text)\s*\(")
ALLOWED = {"port/linux/game/keyboard.c", "source/interface/virtual_keyboard.c", "source/interface/virtual_keyboard.h"}


def old_keyboard_problems(sources):
    """Where the new menus could still reach the old keyboards: a caller naming the game's keyboard past keyboard.c,
    the chat's own keyboard opened without asking keyboard_new, or the choice made in a second place."""
    problems = []
    for path, text in sources.items():
        if path not in ALLOWED and OLD_KEYBOARD_CALLS.search(text):
            problems.append(f"{path} calls the game's keyboard itself")
        if path != "port/linux/game/keyboard.c" and re.search(r"^boolean keyboard_new\(", text, re.M):
            problems.append(f"{path} decides the keyboard too")
    chat = sources["port/linux/game/network_chat.c"]
    for name in ("chat_lobby_input_begin", "network_chat_update"):
        body = function(chat, name)
        opened = body.index("chat_keyboard.open = TRUE")
        if "keyboard_new()" not in body[:opened]:
            problems.append(f"{name} opens the chat's keyboard in the new menus")
    return problems


def callers():
    paths = [path for path in (ROOT / "port/linux/game").glob("*.c")] + \
        [path for path in (ROOT / "port/linux/game").glob("*.inc")] + \
        [ROOT / "source/interface" / name for name in ("player_ui.c", "ui_widget_event_handler_functions.c",
                                                         "virtual_keyboard.c")] + \
        [ROOT / "source/input/input_abstraction.c"]
    return {str(path.relative_to(ROOT)): path.read_text(encoding="latin-1") for path in paths}


def test_old_keyboards_gone():
    assert old_keyboard_problems(callers()) == []


def test_old_keyboards_gone_control():
    sources = callers()
    path = "source/interface/player_ui.c"
    sources[path] = sources[path].replace("keyboard_name_launch(", "virtual_keyboard_launch(", 1)
    chat = "port/linux/game/network_chat.c"
    sources[chat] = sources[chat].replace("\tif (keyboard && keyboard_new())\n", "\tif (0)\n", 1)
    problems = old_keyboard_problems(sources)
    assert any("player_ui.c" in problem for problem in problems), problems
    assert any("chat_lobby_input_begin" in problem for problem in problems), problems


def help_fit_problems(glue):
    """The prompts' help line made to fit its room: each list of its forms ends with none, and the first that fits
    the room left of CANCEL is shown; the footer's CANCEL and DONE the prompts' buttons, B and Start."""
    prompts = function(glue, "keyboard_draw_prompts")
    problems = []
    for name in ("system_help", "keys_help", "pad_help"):
        forms = re.search(name + r"\[\] =\s*\{(.*?)\};", prompts, re.S)
        if not forms or not forms.group(1).strip().rstrip(",").endswith('L""'):
            problems.append(f"{name} has no empty last form")
    if "keyboard_width(keyboard.small_font, help) > room" not in prompts:
        problems.append("the help is not measured against its room")
    if 'L"%b-button CANCEL"' not in prompts or 'L"%start-button DONE"' not in prompts:
        problems.append("CANCEL and DONE are not B's and Start's")
    return problems


def test_help_fits():
    assert help_fit_problems(read(GLUE)) == []


def test_help_fits_control():
    glue = read(GLUE).replace("keyboard_width(keyboard.small_font, help) > room", "0", 1)
    assert help_fit_problems(glue)
