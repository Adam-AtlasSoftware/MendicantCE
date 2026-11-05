"""What the menu files' rows, buttons and prompts hold, where the engine draws it (test_menu_fit.py): the rows of a
list apart, a one-line text's capitals on the middle of the framed picture it is in or on, and a prompt's words in
its picture. Each check returns the problems it finds, so that a deliberate fault can be shown to be found."""

import re
from typing import NamedTuple

from harness import menu_layout

port_settings = menu_layout.port_settings

# the fonts' measures (port_settings.py's: draw_string puts the baseline the ascent below a text box's top and
# text_y, the capitals rising their height above it)
FONTS = {"ui\\small_ui": (port_settings.SMALL_UI_ASCENT, port_settings.SMALL_UI_CAPS),
         "ui\\large_ui": (port_settings.LARGE_UI_ASCENT, port_settings.LARGE_UI_CAPS)}
# the pictures of the port's parts that frame what is in them, their whole height (a section's picture is a line
# under its title, not a frame: not one of them)
FRAMES = re.compile(r"main_menu/parts/(item|outline|button|row|option_row|pill|player_row)_\d+x\d+$")
# how far off the middle capitals may be: half a unit, where whole units cannot centre them
SLACK = 0.5


class Placed(NamedTuple):
    """a widget placed in its parent: its name, attributes, and its box's left and top in the parent"""
    name: str
    attributes: dict
    x: int
    y: int


def port_names():
    """the names of the port's own screens' widgets (tools/port_settings.py's files)"""
    names = set()
    for name in [*port_settings.settings_files(), *port_settings.multiplayer_files(), *port_settings.campaign_files()]:
        names |= set(re.findall(r'<widget name="([^"]+)"', (menu_layout.CE / name).read_text()))
    return sorted(names)


def children(widgets, name):
    return [Placed(child, widgets[child].attributes, x, y) for child, x, y, _ in widgets[name].children]


def rows_overlapping(widgets, names):
    """the rows (op_*) of each list that overlap another of it at another place (those at one place are shown
    one at a time)"""
    problems = []
    for name in names:
        widget = widgets[name]
        if widget.attributes.get("type") != "column_list":
            continue
        rows = []
        for child, x, y, platform in widget.children:
            attributes = widgets[child].attributes
            if child.rsplit("/", 1)[-1].startswith("op_") and attributes.get("bitmap"):
                top = y + int(attributes.get("top", 0))
                rows.append((child, top, top + int(attributes.get("height", 0)), platform))
        for index, (row, top, bottom, platform) in enumerate(rows):
            for other, other_top, other_bottom, other_platform in rows[index + 1:]:
                if top == other_top or (platform and other_platform and
                                        not set(platform.split()) & set(other_platform.split())):
                    continue
                if top < other_bottom and other_top < bottom:
                    problems.append(f"{name}: {row} ({top} to {bottom}) over {other} ({other_top} to {other_bottom})")
    return problems


def capitals_middle(attributes, top):
    """where a text's (or a spinner's value's) capitals' middle is, from its parent's top; a spinner draws its
    value at its box's top, not text_y's"""
    ascent, caps = FONTS[attributes["font"]]
    text_y = 0 if attributes.get("type") == "spinner" else int(attributes.get("text_y", 0))
    return top + int(attributes.get("top", 0)) + text_y + ascent - caps / 2


def one_line(attributes):
    return attributes.get("font") in FONTS and "\\n" not in attributes.get("text", "") and \
        int(attributes.get("height", 0)) <= 32


def texts_off_middle(widgets, names):
    """the one-line texts whose capitals are off the middle of the framed part they are on (their own picture's)
    or in (their row's), and the spinners' values in such a row"""
    problems = []
    for name in names:
        widget = widgets[name]
        attributes = widget.attributes
        if not FRAMES.search(attributes.get("bitmap", "")):
            continue
        height = int(attributes.get("height", 0))
        if one_line(attributes) and attributes.get("type") == "text":
            middle = capitals_middle(attributes, -int(attributes.get("top", 0)))
            if abs(middle - height / 2) > SLACK:
                problems.append(f"{name}: its capitals' middle {middle:g}, its picture's {height / 2:g}")
        for child in children(widgets, name):
            if child.attributes.get("type") in ("text", "spinner") and one_line(child.attributes) and \
                    not child.attributes.get("bitmap"):
                middle = capitals_middle(child.attributes, child.y)
                if abs(middle - height / 2) > SLACK:
                    problems.append(f"{child.name}: its capitals' middle {middle:g}, its row's {height / 2:g}")
    return problems


def prompt_overflows(widgets, names):
    """the footers' prompts (prompt_*, with a part's picture) whose words end past their picture's frame: their
    key's cap and words as drawn (port_settings.prompt_words_end), in a server browser with its keys"""
    problems = []
    for name in names:
        widget = widgets[name]
        attributes = widget.attributes
        if not name.rsplit("/", 1)[-1].startswith("prompt_") or not FRAMES.search(attributes.get("bitmap", "")) or \
                "text" not in attributes:
            continue
        browser = "/join_game/" in name
        end = int(attributes.get("text_x", 0)) + port_settings.prompt_words_end(attributes["text"], browser)
        room = int(attributes["width"]) - port_settings.PROMPT_RIGHT_ROOM
        if end > room:
            problems.append(f"{name}: its words end at {end}, its picture's room at {room}")
    return problems
