"""Stick Test's readings (#1140: stick_test.c), a hand-written screen's (port/assets/menus/screens/stick_test.xml,
port/linux/game/stick_test_menu.inc): each stick as the game read it, then after the dead zones and the look stick's
shape Advanced Stick Settings shows, unsaved changes among them, by the game's own code (stick_test_text,
input_xbox.c's input_port_fix_stick, xinput_sdl.c's halo_stick_ranges); the circle's rings and dots; the drift at
rest. The screen's circles and words fit their columns and the room between the band and the button bar (a
hand-written screen keeps the menus' 640 by 480 at every aspect, so 4:3, 16:9 and 21:9 are the same); and the
generated page's classic-path drawing (its circles drawn from a hook on its last row's text) is gone."""

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, function, mutated, read, run  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

PAGE = "port/linux/game/stick_test_menu.inc"
SCREEN = ROOT / "port/assets/menus/screens/stick_test.xml"

CASES = ["drift", "live", "shape", "view", "rest"]

CONTROLS = [
    # the page's dead zone not read: the reading by the game's own, whatever the spinner shows
    ("\tif (settings_value(inner[stick], value, sizeof(value)))\n\t\tinner_percent = atol(value);\n", "", "live"),
    # the dots drawn down for a push up
    ("PIN(-reading->raw.y * scale", "PIN(reading->raw.y * scale", "view"),
    # any push near the middle the drift, still or not
    ("else if (now - stick_test.still_since[stick] >= STICK_TEST_REST_MILLISECONDS)", "else", "rest"),
]

# the longest reading: both sticks' directions at the full travel, before and after
LONGEST = "-100,-100 | -100,-100"


def generated(fault=None):
    xbox = read("source/input/input_xbox.c")
    sdl = read("port/linux/src/xinput_sdl.c")
    page = read(PAGE)
    defines = "\n".join(re.search(rf"^#define {name} [^\n]+", sdl, re.M).group(0)
                        for name in ("STICK_DEAD_RANGE", "STICK_DEAD_ZONE_MAXIMUM", "STICK_OUTER_DEAD_ZONE_MAXIMUM"))
    start = page.index("/* still: moved less")
    state = page[start:page.index("static int stick_test_percent", start)]
    code = "\n".join([defines, function(sdl, "halo_stick_ranges"), function(xbox, "fix_dead_zone"),
                      function(xbox, "fix_dead_zones"), function(xbox, "fix_round_dead_zone"),
                      function(xbox, "input_port_fix_stick"), state] +
                     [function(page, name) for name in ("stick_test_percent", "stick_test_read", "stick_test_text",
                                                        "stick_test_rest", "stick_test_view_of")]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_stick_test(case):
    status, output = run(build("stick_test", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_stick_test_negative_control(before, after, case):
    status, output = run(build("stick_test", generated((before, after))), case)
    assert status == CHECK_FAILED, output


# the reading taken where the keyboard is merged in: the port's whole reading, not its controller's
WHOLE_READING = ("\thalo_controller_stick(gamepad, stick, &reading->raw.x, &reading->raw.y);",
                 "\treading->raw = input_get_gamepad_state(gamepad)->sticks[stick];")


def _controller_only(page, sdl):
    """the readings' raw stick is the controller's own: the screen reads halo_controller_stick, which xinput_sdl.c's
    XInputGetState records from the port's controller before the keyboard, mouse, touch and script are merged"""
    text = function(page, "stick_test_read")
    state = function(sdl, "XInputGetState")
    recorded = state.find("controller_sticks_read(port, port_gamepad(gamepads, count, port));")
    merged = min(state.find(mark) for mark in ("keyboard_gamepad(", "keyboard_controls(", "touch_input_gamepad("))
    return "halo_controller_stick(" in text and "input_get_gamepad_state" not in text and \
        "raw_gamepad_states" not in text and 0 <= recorded < merged


def test_reading_is_the_controllers_alone():
    assert _controller_only(read(PAGE), read("port/linux/src/xinput_sdl.c"))


def test_reading_check_sees_the_whole_reading():
    """negative control: the screen reading the port's merged reading fails the check"""
    page = mutated(read(PAGE), *WHOLE_READING)
    assert not _controller_only(page, read("port/linux/src/xinput_sdl.c"))


# ---------- the old page gone


def _old_page_left(menus, settings):
    """what is left of the generated page and its hook: menu_functions.c's text-box hook drawing anything of the
    sticks, the generated page's rows' names or its reading code there, the generator's page, its menu files"""
    left = []
    hook = function(menus, "menu_functions_text_box_drawn")
    if re.search(r"stick", hook, re.I):
        left.append("menu_functions_text_box_drawn draws a stick")
    for name in ("stick_test_drawn", "stick_test_rows_show", "stick_test_draw(", "stick_drift", "STICK_TEST_GAP",
                 "STICK_TEST_RADIUS"):
        if name in menus:
            left.append(f"menu_functions.c has {name}")
    if "gamepad_setup/stick_test" in settings or "STICK_TEST_RADIUS" in settings or "stick_test_left" in settings:
        left.append("tools/port_settings.py still writes the page")
    for folder in ("ce", "model/look", "model/behaviour"):
        if list((ROOT / "port/assets/menus" / folder).glob("*stick_test*")):
            left.append(f"menus/{folder} has the page")
    return left


def test_old_hook_and_page_gone():
    assert _old_page_left(read("port/linux/game/menu_functions.c"), read("tools/port_settings.py")) == []


def test_old_hook_check_sees_the_hook():
    """negative control: the circles drawn again from the text-box hook fail the check"""
    menus = mutated(read("port/linux/game/menu_functions.c"), "\tshort row;\n\n\t/* (as the lobby",
                    "\tshort row;\n\n\tif (stick_test_drawn(widget, bounds))\n\t\treturn;\n\t/* (as the lobby")
    assert _old_page_left(menus, read("tools/port_settings.py"))


def test_stick_test_opens_the_screen():
    """Advanced Stick Settings' STICK TEST › opens the hand-written screen, which takes its place in menu_screen.c"""
    dead_zones = (ROOT / "port/assets/menus/ce" /
                  "main_menu.settings_select.player_setup.player_profile_edit.gamepad_setup.dead_zones.xml").read_text()
    assert re.search(r'op_stick_test"[^>]*>\s*<on event="a" open="screens/stick_test"/>', dead_zones)
    assert '{ "stick_test", "screens\\\\stick_test", pc_menu_stick_test_frame, NULL }' in \
        read("port/linux/game/menu_screen.c")
    assert "screens/stick_test.xml" in read("port/assets/menus/menus.json")


# ---------- the screen's layout


def _layout(screen_text):
    """each <stick>'s column and lines (menu_screen.c's STICK_* and the blocks' sizes): its x, radius and its foot,
    and the room between the band and the bar"""
    source = read("port/linux/game/menu_screen.c")
    constant = {name: int(re.search(rf"^#define {name} (\d+)", source, re.M).group(1))
                for name in ("STICK_COLUMN_WIDTH", "STICK_CIRCLE_TOP", "STICK_VALUE_GAP", "STICK_DETAIL_STEP",
                             "STICK_HINT_STEP", "HEADER_ROOM_TALL", "BUTTONS_ROOM", "SCREEN_HEIGHT")}
    blocks = ElementTree.parse(ROOT / "port/assets/menus/screens/blocks.xml").getroot()
    sizes = {widget.get("name"): (int(widget.get("width")), int(widget.get("height")))
             for widget in blocks.iter("widget") if widget.get("name", "").startswith("screens/stick")}
    sticks = []
    for stick in ElementTree.fromstring(screen_text).iter("stick"):
        x, y, radius = int(stick.get("x")), int(stick.get("y")), int(stick.get("radius", "88"))
        foot = y + constant["STICK_CIRCLE_TOP"] + 2 * radius + constant["STICK_VALUE_GAP"] + \
            constant["STICK_DETAIL_STEP"] + constant["STICK_HINT_STEP"] + sizes["screens/stick_hint"][1]
        sticks.append((x, y, radius, foot))
    return constant, sizes, sticks


def _layout_problems(screen_text):
    constant, sizes, sticks = _layout(screen_text)
    width = constant["STICK_COLUMN_WIDTH"]
    problems = []
    if any(sizes[name][0] != width for name in ("screens/stick_label", "screens/stick_value", "screens/stick_note",
                                                 "screens/stick_hint")):
        problems.append(f"the blocks are not {width} wide: {sizes}")
    if len(sticks) != 2:
        problems.append(f"{len(sticks)} sticks")
    for x, y, radius, foot in sticks:
        if 2 * radius > width:
            problems.append(f"a circle {2 * radius} wide in its column of {width}")
        if x - width // 2 < 0 or x + width // 2 > 640:
            problems.append(f"the column at {x} leaves the 640")
        if y < constant["HEADER_ROOM_TALL"]:
            problems.append(f"the stick at {y} under the band")
        if foot > constant["SCREEN_HEIGHT"] - constant["BUTTONS_ROOM"]:
            problems.append(f"the stick's foot {foot} in the bar")
    if len(sticks) == 2:
        (x0, _, r0, _), (x1, _, r1, _) = sticks
        if abs(x1 - x0) < max(width, r0 + r1 + 8):
            problems.append(f"the columns at {x0} and {x1} meet")
    # (the longest reading in its value's box, at the small font's size: the screen draws it unscaled)
    if port_settings.small_ui_width(LONGEST) > sizes["screens/stick_value"][0]:
        problems.append(f"{LONGEST!r} does not fit")
    # (and each hint on its one line: its box is not wrapped)
    for stick in ElementTree.fromstring(screen_text).iter("stick"):
        hint = stick.get("hint", "")
        if port_settings.small_ui_width(hint) > sizes["screens/stick_hint"][0]:
            problems.append(f"the hint {hint!r} does not fit its line")
    return problems


def test_screen_fits():
    assert _layout_problems(SCREEN.read_text()) == []


def test_fit_check_sees_too_big_a_circle():
    """negative control: a circle wider than its column fails"""
    text = SCREEN.read_text().replace('radius="88"', 'radius="130"', 1)
    assert _layout_problems(text)
