"""What the menu files' rows, buttons and prompts hold, where the engine draws it (test_menu_fit.py): the rows of a
list apart, a one-line text's capitals on the middle of the framed picture it is in or on, a prompt's words in its
picture, and Choose Mission's words in their boxes and its arrows by its cards. Each check returns the problems it
finds, so that a deliberate fault can be shown to be found."""

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


def stock_objectives():
    """the missions' objectives as the game shows them (campaign_menu.inc's campaign_string: the lines joined, a word
    broken at a line's end whole again)"""
    import xml.etree.ElementTree as ElementTree
    root = ElementTree.parse(menu_layout.CE / "strings.xml").getroot()
    strings = next(element for element in root.iter("strings")
                   if element.get("name") == "main_menu/solo_level_select/map_data")
    found = []
    for string in strings.iter("string"):
        # (the file's line breaks are "\\n", two characters)
        text = re.sub(r"(?<! )-\\n", "", string.get("text"))
        found.append(" ".join(text.replace("\\n", " ").split()))
    return found[:10]


def wrapped_lines(words, width):
    """how many lines words take wrapped to a width in small_ui (settings_text_wrap: at spaces)"""
    lines, line = 1, ""
    for word in words.split(" "):
        longer = f"{line} {word}" if line else word
        if line and port_settings.small_ui_width(longer) > width:
            lines, line = lines + 1, word
        else:
            line = longer
    return lines


def mission_overflows(widgets, names):
    """Choose Mission (issue 681): its arrows as far from the row of cards at every width (MISSION_ARROW_GAP, a unit
    either way: the row wider apart on a wide screen, campaign_menu.inc's mission_cards_place); NOT YET BEATEN within
    its box on either card; each mission's objective on no more lines than its box holds and the game wraps it to
    (MISSION_OBJECTIVE_LINES), at every width"""
    base = port_settings.CAMPAIGN
    if f"{base}/mission_screen" not in names:
        return []
    problems = []
    big, small = port_settings.MISSION_BIG[0], port_settings.MISSION_SMALL[0]
    gaps = set()
    for aspect, width in port_settings.WIDE_SCREENS.items():
        extra = port_settings.wide_extra_of(width)
        boxes = menu_layout.layout(f"{base}/mission_screen", width, every=widgets)
        row = big + 2 * small + 2 * (port_settings.MISSION_CARD_GAP + extra // 8)
        left, right = 320 - row // 2, 320 - row // 2 + row
        previous, following = menu_layout.one(boxes, "arrow_previous"), menu_layout.one(boxes, "arrow_next")
        gaps |= {(aspect, left - previous.x1), (aspect, following.x0 - right)}
        objective = menu_layout.one(boxes, "detail_objective")
        lines = port_settings.MISSION_OBJECTIVE_LINES
        holds = 1 + (objective.y1 - objective.y0 - port_settings.SMALL_UI_ASCENT - port_settings.SMALL_UI_DESCENT) // \
            port_settings.SMALL_UI_LINE
        if holds < lines:
            problems.append(f"{aspect}: the objective's box holds {holds} lines, {lines} are drawn")
        for words in stock_objectives():
            needs = wrapped_lines(words, objective.x1 - objective.x0)
            if needs > min(lines, holds):
                problems.append(f"{aspect}: \"{words}\" takes {needs} lines, {min(lines, holds)} are shown")
    for aspect, gap in sorted(gaps):
        if abs(gap - port_settings.MISSION_ARROW_GAP) > 1:
            problems.append(f"{aspect}: an arrow {gap} units from the cards, not {port_settings.MISSION_ARROW_GAP}")
    for kind in ("big", "small"):
        box = widgets[f"{base}/card_unbeaten_{kind}"].attributes
        need = port_settings.small_ui_width("NOT YET BEATEN")
        if int(box["width"]) < need:
            problems.append(f"card_unbeaten_{kind}: {box['width']} wide, NOT YET BEATEN {need}")
    return problems
