"""Controller Layout's screen (#1143: port/assets/menus/screens/controller_layout.xml) fits at 4:3, 16:9 and 21:9:
its table's columns in the table's width at 4:3 (menu_screen_tall.inc's TALL_TABLE_WIDTH), their growth no more than
the table's at 16:9 (TALL_TABLE_GROW; past 16:9 the table only grows, its columns as 16:9's), a touchscreen's places
in its row; each button's words on any controller (controller_remap.c's labels) in its column unshortened; the widest
legend (3 W's) and its cap's insets in the symbol column, the cap in its row; the panel's words on their lines. And
Button Remapping's CONTROLLER LAYOUTS › opens the page, whose EDIT BUTTONS › opens the screen, which takes its place
in menu_screen.c. A negative control: a column made wider fails the fit."""

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import ROOT, read  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

SCREEN = ROOT / "port/assets/menus/screens/controller_layout.xml"


def _constants():
    source = read("port/linux/game/menu_screen_tall.inc")
    return {name: int(re.search(rf"^#define {name} (\d+)", source, re.M).group(1))
            for name in ("TALL_TABLE_WIDTH", "TALL_TABLE_GROW", "TALL_CELLS_X", "TALL_SYMBOL_INSET", "TALL_SYMBOL_TOP",
                         "TALL_SYMBOL_BOTTOM", "TALL_ROW_HEIGHT", "TOUCH_TABLE_WIDTH", "TALL_PANEL_WIDTH",
                         "TALL_PANEL_INSET")}


def _button_words():
    """every family's words for the extra buttons (the last 11 of each of controller_remap.c's label rows), and
    PlayStation's Back's"""
    table = read("port/linux/src/controller_remap.c")
    table = table[table.index("labels[5][CONTROLLER_REMAP_BUTTONS] = {"):table.index("int i;", table.index("labels[5]"))]
    words = re.findall(r'"([^"]+)"', table)
    assert len(words) == 5 * 26, len(words)
    return [word for family in range(5) for word in words[family * 26 + 15:family * 26 + 26]]


def _wrapped_lines(text, width):
    lines, line = 1, ""
    for word in text.split():
        candidate = f"{line} {word}".strip()
        if line and port_settings.small_ui_width(candidate) > width:
            lines, line = lines + 1, word
        else:
            line = candidate
    return lines


def _problems(screen_text):
    c = _constants()
    root = ElementTree.fromstring(screen_text)
    columns = list(root.iter("column"))
    problems = []
    x, grow = c["TALL_CELLS_X"], 0
    for index, column in enumerate(columns):
        x += (int(column.get("gap", "4")) if index else 0) + int(column.get("width"))
        grow += int(column.get("grow", "0"))
        touch = [int(n) for n in column.get("touch", "0 0 0 0 0").split()]
        if touch[0] + touch[2] > c["TOUCH_TABLE_WIDTH"]:
            problems.append(f"{column.get('id')}'s touch place past the touch table")
    if x > c["TALL_TABLE_WIDTH"] - c["TALL_CELLS_X"]:
        problems.append(f"the columns {x} wide in the table's {c['TALL_TABLE_WIDTH']} at 4:3")
    if grow > c["TALL_TABLE_GROW"]:
        problems.append(f"the columns grow {grow}, the table {c['TALL_TABLE_GROW']} at 16:9")
    widths = {column.get("id"): int(column.get("width")) for column in columns}
    # (each column's title whole, drawn up to 1.2 times the small font's size, as the rows' words)
    for column in columns:
        if port_settings.small_ui_width(column.get("label", "")) * 1.2 > int(column.get("width")):
            problems.append(f"{column.get('label')!r} cut short in its title")
    for words in _button_words():
        if port_settings.small_ui_width(words) > widths.get("button", 0):
            problems.append(f"{words!r} does not fit its column")
    if port_settings.small_ui_width("WWW") + 2 * c["TALL_SYMBOL_INSET"] > widths.get("symbol", 0):
        problems.append("the widest legend's cap does not fit its column")
    if not 0 <= c["TALL_SYMBOL_TOP"] < c["TALL_SYMBOL_BOTTOM"] <= c["TALL_ROW_HEIGHT"]:
        problems.append("the cap is not in its row")
    about = re.search(r'"layout\.about"\)\)\n\t\tsnprintf\(value, size, "%s", (".*?")\);',
                      read("port/linux/game/controller_layout_menu.inc"), re.S).group(1)
    about = "".join(re.findall(r'"([^"]*)"', about))
    lines = int(next(text.get("lines") for text in root.iter("text") if text.get("text") == "{layout.about}"))
    if _wrapped_lines(about, c["TALL_PANEL_WIDTH"] - 2 * c["TALL_PANEL_INSET"]) > lines:
        problems.append("the panel's words take more than their lines")
    return problems


def test_screen_fits():
    assert _problems(SCREEN.read_text()) == []


def test_fit_check_sees_a_wide_column():
    """negative control: the name's column 20 wider overruns the table at 4:3"""
    text = SCREEN.read_text().replace('id="name" label="NAME" width="162"', 'id="name" label="NAME" width="182"')
    assert text != SCREEN.read_text()
    assert _problems(text)


def test_pages_open_the_screen():
    menus = ROOT / "port/assets/menus/ce"
    remapping = (menus / "main_menu.settings_select.player_setup.player_profile_edit.gamepad_setup.button_remapping"
                 ".xml").read_text()
    assert "controller_layouts_screen" in remapping
    page = (menus / "main_menu.settings_select.player_setup.player_profile_edit.gamepad_setup.button_remapping"
            ".controller_layouts.xml").read_text()
    assert re.search(r'<on event="a" open="screens/controller_layout"/>', page)
    assert '{ "controller_layout", "screens\\\\controller_layout", NULL, NULL }' in read("port/linux/game/menu_screen.c")
    assert "screens/controller_layout.xml" in read("port/assets/menus/menus.json")
