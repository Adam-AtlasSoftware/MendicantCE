"""Letters with a mark typed into a name (#1134): a keyboard layout's own and the system's on-screen keyboard's
text (port/linux/src/sdl_platform.c), put in by the on-screen keyboard's typing (source/interface/virtual_keyboard.c)
and the menus' text field (port/linux/game/menu_functions.c), and a name so typed kept by player_name_clean and folded
for the host (source/game/players.c)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["layout", "text_event", "name", "text_field"]
FUNCTIONS = [
    ("port/linux/src/sdl_platform.c", "typed_letter_kept"),
    ("port/linux/src/sdl_platform.c", "keystroke_character"),
    ("port/linux/src/sdl_platform.c", "utf8_next"),
    ("source/interface/virtual_keyboard.c", "virtual_keyboard_port_character_kept"),
    ("port/linux/game/menu_functions.c", "text_field_character_kept"),
    ("source/game/players.c", "player_name_character_ascii"),
    ("source/game/players.c", "player_name_clean"),
]

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (as it was) the on-screen keyboard's typing keeping ASCII alone
    "ascii-only": ("virtual_keyboard_port_character_kept",
                   ("return (code >= ' ' && code <= '~') ||", "return (code >= ' ' && code <= '~'); return"), "name"),
    # a layout's letter with a mark lost (its key's ASCII alone, as before)
    "layout-lost": ("keystroke_character", ("\tif (typed_letter_kept((unsigned long)typed))\n", "\tif (0)\n"),
                    "layout"),
}


def generated(fault=None):
    code = ""
    for path, name in FUNCTIONS:
        text = function(read(path), name)
        if fault and NEGATIVE_CONTROLS[fault][0] == name:
            text = mutated(text, *NEGATIVE_CONTROLS[fault][1])
        code += text.replace("boolean ", "int ", 1) + "\n"
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("typed_accents", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("typed_accents", generated(control)), NEGATIVE_CONTROLS[control][2])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_the_typing_reads_the_character():
    """The on-screen keyboard's typing and the text field put in the key's character (input_key_character), not its
    ASCII, and the system's on-screen keyboard's text events reach the keystrokes."""
    port_type = function(read("source/interface/virtual_keyboard.c"), "virtual_keyboard_port_type")
    assert "input_key_character()" in port_type and "key.ascii_code" not in port_type
    menus = read("port/linux/game/menu_functions.c")
    assert "text_field_insert_character((char)input_key_character());" in menus
    assert "case SDL_EVENT_TEXT_INPUT:\n\t\t\tqueue_text(event.text.text);" in read("port/linux/src/sdl_platform.c")
