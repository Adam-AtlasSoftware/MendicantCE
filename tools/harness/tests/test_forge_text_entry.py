"""A line of text typed in Forge (forge.c): the editing and the on-screen keyboard, worked as the keyboard a profile's
name is typed on: the first key replaces the line, characters go in at the cursor, shift, caps and symbols, and the
chosen key's moves over the wide keys.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["replaces", "first-delete", "cursor", "full", "shift", "done", "move"]

FUNCTIONS = ("forge_text_entry_take", "forge_text_entry_replace", "forge_text_entry_type", "forge_text_entry_delete",
             "forge_text_entry_move_cursor", "forge_text_keyboard_key", "forge_text_keyboard_character",
             "forge_text_keyboard_move", "forge_text_keyboard_press")

TABLES = ("forge_text_keyboard_characters", "forge_text_keyboard_symbols", "forge_text_keyboard_labels",
          "forge_text_keyboard_layout")

# a fault in forge.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "appends": (("forge_text_entry_globals.text[cursor] = character;",
                 "forge_text_entry_globals.text[forge_text_entry_globals.length] = character;"),
                "forge_text_entry_type", "cursor"),
    "keeps-initial": (("\tforge_text_entry_replace();\n\tcursor", "\tcursor"), "forge_text_entry_type", "replaces"),
    "deletes-at-end": (("&forge_text_entry_globals.text[cursor - 1], &forge_text_entry_globals.text[cursor]",
                        "&forge_text_entry_globals.text[forge_text_entry_globals.length - 1], "
                        "&forge_text_entry_globals.text[forge_text_entry_globals.length]"),
                       "forge_text_entry_delete", "cursor"),
    "shift-sticks": (("\tif (key != _forge_text_keyboard_shift)\n\t\tforge_text_entry_globals.keyboard_shift = FALSE;\n",
                      ""), "forge_text_keyboard_press", "shift"),
    "stops-on-wide-keys": (("while (forge_text_keyboard_key() == key);", "while (FALSE);"),
                           "forge_text_keyboard_move", "move"),
}


def table(source, name):
    """A static table's definition, to the semicolon that ends a line (the symbols have one of their own)."""
    match = re.search(r"^static [\w *]+?\b" + re.escape(name) + r"\[", source, re.M)
    if not match:
        raise LookupError(f"table not found in the sources: {name}")
    return source[match.start():source.index(";\n", source.index("=", match.start())) + 1]


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/forge.c")
    globals_match = re.search(r"static struct\s*\{[^{}]*\} forge_text_entry_globals;", source)
    assert globals_match, "forge_text_entry_globals not found in forge.c"
    text = structure(source, "forge_key") + "\n"
    text += enum_with(source, "FORGE_TEXT_KEYBOARD_ROWS") + "\n"
    text += enum_with(source, "_forge_text_keyboard_done") + "\n"
    for name in TABLES:
        text += table(source, name) + "\n"
    text += globals_match.group(0) + "\n"
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("forge_text_entry", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("forge_text_entry", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"forge.c with a fault ({control}) passed '{case}': the test cannot see it"
