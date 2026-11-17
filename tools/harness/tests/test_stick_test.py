"""Advanced Stick Settings' readings (#1140: stick_test.c): each stick as the game read it, then after the dead zones
and the look stick's shape the page shows, unsaved changes among them, by the game's own code (menu_functions.c's
stick_test_text, input_xbox.c's input_port_fix_stick, xinput_sdl.c's halo_stick_ranges); and the longest reading fits
its row's value at 4:3, 16:9 and 21:9."""

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, function, mutated, read, run  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

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
    menus = read("port/linux/game/menu_functions.c")
    defines = "\n".join(re.search(rf"^#define {name} [^\n]+", sdl, re.M).group(0)
                        for name in ("STICK_DEAD_RANGE", "STICK_DEAD_ZONE_MAXIMUM", "STICK_OUTER_DEAD_ZONE_MAXIMUM"))
    start = menus.index("/* the circles: their radius")
    page = menus[start:menus.index("static int stick_test_percent", start)]
    code = "\n".join([defines, function(sdl, "halo_stick_ranges"), function(xbox, "fix_dead_zone"),
                      function(xbox, "fix_dead_zones"), function(xbox, "fix_round_dead_zone"),
                      function(xbox, "input_port_fix_stick"), page] +
                      [function(menus, name) for name in ("stick_test_percent", "stick_test_read", "stick_test_text",
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


def _controller_only(menus, sdl):
    """the readings' raw stick is the controller's own: the page reads halo_controller_stick, which xinput_sdl.c's
    XInputGetState records from the port's controller before the keyboard, mouse, touch and script are merged"""
    text = function(menus, "stick_test_read")
    state = function(sdl, "XInputGetState")
    recorded = state.find("controller_sticks_read(port, port_gamepad(gamepads, count, port));")
    merged = min(state.find(mark) for mark in ("keyboard_gamepad(", "keyboard_controls(", "touch_input_gamepad("))
    return "halo_controller_stick(" in text and "input_get_gamepad_state" not in text and \
        "raw_gamepad_states" not in text and 0 <= recorded < merged


def test_reading_is_the_controllers_alone():
    assert _controller_only(read("port/linux/game/menu_functions.c"), read("port/linux/src/xinput_sdl.c"))


def test_reading_check_sees_the_whole_reading():
    """negative control: the page reading the port's merged reading fails the check"""
    menus = mutated(read("port/linux/game/menu_functions.c"), *WHOLE_READING)
    assert not _controller_only(menus, read("port/linux/src/xinput_sdl.c"))


def _look(name):
    root = ElementTree.parse(ROOT / "port/assets/menus/model/look" /
                             "main_menu.settings_select.player_setup.player_profile_edit.gamepad_setup.stick_test.xml")
    found = [look for look in root.iter("look") if look.get("name", "").endswith("/" + name)]
    assert len(found) == 1, name
    return found[0]


def test_circles_constants_are_the_generators():
    menus = read("port/linux/game/menu_functions.c")
    for name in ("STICK_TEST_RADIUS", "STICK_TEST_GAP"):
        assert int(re.search(rf"^#define {name} (\d+)", menus, re.M).group(1)) == getattr(port_settings, name)


@pytest.mark.parametrize("aspect", ["4:3", "16:9", "21:9"])
def test_circles_fit_under_the_rows(aspect):
    """each circle under its column (the left under DRIFT AT REST's label, the right under its value), as wide as
    it at least, apart from each other, and above the footer's band in both layouts"""
    sys.path.insert(0, str(ROOT))
    from tools import test_port_settings as settings_test
    extra = port_settings.wide_extra_of(port_settings.WIDE_SCREENS[aspect])
    diameter = 2 * port_settings.STICK_TEST_RADIUS
    middles = []
    for name, x in (("stick_drift_label", 0), ("stick_drift_value", None)):
        look = _look(name)
        width = int(look.get("box").split()[2]) + port_settings.wide_scale(int(look.get("wide", "0 0").split()[1]),
                                                                           extra)
        if x is None:
            row = _look("op_stick_drift")
            x = int(next(at.get("x") for at in row.iter("at") if at.get("widget").endswith("stick_drift_value")))
            x += port_settings.wide_scale(int(look.get("wide", "0 0").split()[0]), extra)
        assert diameter <= width, f"{name} at {aspect}: a circle {diameter} wide in {width}"
        middles.append(x + width / 2)
    assert middles[1] - middles[0] >= diameter + 8, f"at {aspect} the circles {middles} overlap"
    page = settings_test.PAGES[port_settings.STICK_TEST[0]]
    for layout, geometry in settings_test.LAYOUTS.items():
        bottom = settings_test._rows_bottom(page, settings_test.BUILDS["desktop"], True, geometry)
        assert bottom + port_settings.STICK_TEST_GAP + diameter <= geometry.rows_bottom, layout


def test_circles_check_sees_too_big_a_circle(monkeypatch):
    """negative control: a circle wider than the value's column fails"""
    monkeypatch.setattr(port_settings, "STICK_TEST_RADIUS", 80)
    with pytest.raises(AssertionError):
        test_circles_fit_under_the_rows("4:3")


def _value_looks():
    """the readings' value boxes in the page's model (the tall layout's look)"""
    name = "main_menu.settings_select.player_setup.player_profile_edit.gamepad_setup.stick_test.xml"
    root = ElementTree.parse(ROOT / "port/assets/menus/model/look" / name).getroot()
    looks = [look for look in root.iter("look") if re.search(r"/stick_test_(left|right)_value$", look.get("name", ""))]
    assert len(looks) == 2, [look.get("name") for look in looks]
    return looks


@pytest.mark.parametrize("aspect", ["4:3", "16:9", "21:9"])
def test_longest_reading_fits(aspect):
    """at 4:3 the box as the 640 have it, at its words' least scale; at 16:9 grown by its wide; at 21:9 by its past
    too, at the most scale"""
    extra = port_settings.wide_extra_of(port_settings.WIDE_SCREENS[aspect])
    for look in _value_looks():
        width = int(look.get("box").split()[2]) + port_settings.wide_scale(int(look.get("wide", "0 0").split()[1]),
                                                                           extra)
        if aspect == "21:9":
            width += int(look.get("past", "0 0").split()[1])
        least, most = (float(value) for value in (look.find("text").get("scale") or "1 1").split())
        scale = least + (most - least) * extra / port_settings.WIDE_EXTRA
        words = port_settings.small_ui_width(LONGEST) * scale
        assert words <= width, f"{look.get('name')} at {aspect}: {LONGEST!r} {words:.0f} wide in {width}"


def test_fit_check_sees_a_reading_too_long():
    """the check's own negative control: the reading with the arrow the links use, and spaces, is too long at 4:3"""
    look = _value_looks()[0]
    assert port_settings.small_ui_width(LONGEST.replace(" | ", "  ›  ")) > int(look.get("box").split()[2])
