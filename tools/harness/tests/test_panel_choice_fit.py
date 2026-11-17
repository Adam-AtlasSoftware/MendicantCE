"""A tall screen's panel choices (docs/menu_screens.md, <choice>: the lobby's BOTS, SKILL, TEAMS and FRIEND,
Controller Layout's SHAPE and COLOR): each label, in the small font (the blocks' text is drawn at its size), in its
own width and clear of its left arrow by a gap (menu_screen_tall.inc's PANEL_CHOICE_*), and the room between the
arrows still the widest value's (NORMAL, SQUARE). A negative control: the spacing before this check (the value,
and its left arrow, at 58, inside the label's 58 from 8), which ran the labels into their arrows ("SHAPE<")."""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import ROOT, read  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

SCALE = 1.0
# (the least room between a label and its left arrow)
GAP = 6


def _constants(source):
    defines = dict(re.findall(r"^#define (PANEL_CHOICE_\w+|TALL_PANEL_WIDTH|TALL_PANEL_INSET) (.+)$", source, re.M))
    values = {}
    for name in sorted(defines, key=lambda n: "VALUE_X" in n):
        values[name] = int(eval(re.sub(r"[A-Z_]+", lambda m: str(values[m.group(0)]), defines[name])))
    return values


def _problems(source):
    c = _constants(source)
    labels = sorted({label for path in (ROOT / "port/assets/menus/screens").glob("*.xml")
                     for label in re.findall(r'<choice label="([^"]+)"', path.read_text())})
    assert labels
    arrow = c["PANEL_CHOICE_VALUE_X"]
    problems = []
    for label in labels:
        drawn = port_settings.small_ui_width(label) * SCALE
        if drawn > c["PANEL_CHOICE_LABEL_WIDTH"]:
            problems.append(f"{label!r} wider than its label's width")
        if c["PANEL_CHOICE_LABEL_X"] + drawn + GAP > arrow:
            problems.append(f"{label!r} runs into its left arrow at {arrow}")
    if c["PANEL_CHOICE_LABEL_X"] + c["PANEL_CHOICE_LABEL_WIDTH"] > arrow:
        problems.append("the label's box reaches past the left arrow")
    room = c["TALL_PANEL_WIDTH"] - 2 * c["TALL_PANEL_INSET"] - arrow - 2 * (c["PANEL_CHOICE_ARROW"] + 4)
    if room < max(port_settings.small_ui_width(word) for word in ("NORMAL", "SQUARE", "ORANGE")) * SCALE:
        problems.append(f"only {room} between the arrows")
    return problems


def test_choice_labels_clear_their_arrows():
    assert _problems(read("port/linux/game/menu_screen_tall.inc")) == []


def test_check_sees_the_old_spacing():
    source = read("port/linux/game/menu_screen_tall.inc")
    old = re.sub(r"^#define PANEL_CHOICE_VALUE_X .+$", "#define PANEL_CHOICE_VALUE_X 58", source, flags=re.M)
    assert old != source
    assert _problems(old)
