"""A key's cap in the prompts and the menus' footers (port/linux/game/hud_key_prompts.c, issue 596): its name's
letters in the middle of the box with as much room either side, for any name and font; the text after it a gap past
the box; the advance the menus' legends lay their rows out with (source/interface/ui_widget.c) the drawing's; and the
legends measuring with it, not a width of their own."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["centred", "gap", "advance", "wrapped"]

SOURCE = "port/linux/game/hud_key_prompts.c"
FUNCTIONS = ("key_cap_metrics", "key_cap_layout", "hud_key_prompt_cap_advance", "key_cap_draw")

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the name drawn from the margin past the cursor, as before issue 596: its letters off the box's middle
    "name-at-margin": (("*pen = (short)(inset + (width - letters) / 2 - left);", "*pen = margin;"), "centred"),
    # a box an odd unit wider than the letters: half a unit more room on one side
    "odd-room": (("if ((width - letters) % 2)\n\t\twidth++;", "if (0)\n\t\twidth++;"), "centred"),
    # a narrow name's box taller than wide
    "no-least-width": (("width = (short)MAX(letters + 2 * margin, 2 * half - 1);",
                        "width = (short)(letters + 2 * margin);"), "centred"),
    # the text after the cap started inside its edge, as before issue 596
    "no-gap": (("*after = (short)(*box_x1 + gap - left);", "*after = (short)(*box_x1 - inset + 1);"), "gap"),
    # the legends' advance not the drawing's
    "advance-is-box": (("\treturn after;", "\treturn box_x1;"), "advance"),
}


def generated(fault=None):
    code = "\n\n".join(function(read(SOURCE), name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("key_caps", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("key_caps", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_legends_lay_out_with_the_drawing():
    """The pause menu's legend row and the help screens' key in a gap lay the cap out with
    hud_key_prompt_cap_advance, from the key's name alone, not a width of their own."""
    widgets = read("source/interface/ui_widget.c")
    shift = function(widgets, "ui_widget_port_key_legend_shift")
    assert re.search(r"ui_widget_port_text_span\(words, key, &name_left, &name_right\);\s*"
                     r"x = \(short\)\(x \+ hud_key_prompt_cap_advance\(name_left, name_right\) \+ words_width\);",
                     shift), "the legend row is not laid out with the cap's advance"
    extra = function(widgets, "ui_widget_port_key_label_cap_extra")
    assert "hud_key_prompt_cap_advance(left, right) - right" in extra
    assert "hud_key_prompt_cap_widening" not in widgets
