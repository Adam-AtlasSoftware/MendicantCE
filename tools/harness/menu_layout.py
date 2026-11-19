"""The menu files' widgets (port/assets/menus/ce) where the engine draws them on a screen of a width: the wide layout
of the port's own screens (port/linux/game/menu_tags.c's ui_widget_port_wide_layout: a widget's wide attribute, "dx
dw", moves it and what is in it, and widens it, by its part of the extra width), as ui_widget.c's
widget_instance_render_recursive adds a widget's offsets and its move to its parents', for the tests of their layout.
The extra width and a value's part of it are tools/port_settings.py's, which test_wide_layout.py checks against
menu_tags.c's."""

import sys
import xml.etree.ElementTree as ElementTree
from functools import lru_cache
from typing import NamedTuple

from harness import ROOT

sys.path.insert(0, str(ROOT / "tools"))
import port_settings  # noqa: E402

CE = ROOT / "port/assets/menus/ce"
# the platforms of a desktop computer's build (the menus' MENU_PLATFORMS a desktop has)
DESKTOP = frozenset({"desktop", "pointer", "computer", "hdr"})
# Android's (menu_files.c's MENU_PLATFORM and MENU_TOUCH)
ANDROID = frozenset({"android", "touch"})


class Box(NamedTuple):
    """a widget drawn: its name (the last part) and its area on the screen, in the menus' units (0 the centred 640's
    left), and its parents' names"""
    name: str
    x0: int
    y0: int
    x1: int
    y1: int
    path: tuple

    def inside(self, other, slack=0):
        return (self.x0 >= other.x0 - slack and self.x1 <= other.x1 + slack and self.y0 >= other.y0 - slack
                and self.y1 <= other.y1 + slack)

    def overlaps(self, other):
        return self.x0 < other.x1 and other.x0 < self.x1 and self.y0 < other.y1 and other.y0 < self.y1


class Widget(NamedTuple):
    attributes: dict
    # (widget, x, y, platform)
    children: list


def _platform_shown(platform, platforms):
    return platform is None or bool(set(platform.split()) & platforms)


@lru_cache(maxsize=None)
def widgets():
    """every <widget> of the menu files, by its name (a nested one a child of the one it is in)"""
    return widgets_of(ElementTree.parse(path).getroot() for path in sorted(CE.glob("*.xml")))


def widgets_of(roots):
    """every <widget> of the files' <menus> elements (roots), by its name"""
    found = {}

    def read(element):
        children = []
        for item in element:
            if item.tag == "child":
                children.append((item.get("widget"), int(item.get("x", 0)), int(item.get("y", 0)),
                                 item.get("platform")))
            elif item.tag == "widget":
                read(item)
                children.append((item.get("name"), int(item.get("x", 0)), int(item.get("y", 0)), item.get("platform")))
        found[element.get("name")] = Widget(dict(element.attrib), children)

    for root in roots:
        for element in root:
            if element.tag == "widget":
                read(element)
    return found


def screen(width):
    """the screen's area a width has, in the menus' units: the centred 640 and as much each side as it is wider"""
    side = max(width - 640, 0) // 2
    return Box("screen", -side, 0, 640 + side, 480, ())


def usable(width):
    """the area the port's screens are laid out in: the screen's, to 16:9's (wider: centred)"""
    side = port_settings.wide_extra_of(width) // 2
    return Box("usable", -side, 0, 640 + side, 480, ())


def inner(width):
    """the usable area less the margin kept from the screen's edges (port_settings.WIDE_MARGIN): where the rows,
    panels, tabs and buttons of the port's screens are"""
    area = usable(width)
    margin = port_settings.WIDE_MARGIN
    return Box("inner", area.x0 + margin, 0, area.x1 - margin, 480, ())


def _settings_rows_packed(children, platforms, mode):
    """a Settings list's children (widget, x, y, platform) as menu_functions.c's settings_rows_pack places them each
    frame: the rows shown (of the platforms; of the lighting mode, as lighting_rows_show shows them by the mode
    alone, the most it shows; one of the rows in the same place at most, as video_rows_show; none of VR's hidden
    ones in the headset; with a profile, as the tests lay it out, not CREATE PROFILE: settings_row_profile) a step
    apart from the first's place, a section's title over its rows (none for a section with no row shown), a gap
    above a section after rows; the rows and titles hidden left out. Its other children as they are."""
    def key(name):
        return name.rsplit("/", 1)[-1][len("op_"):]

    def row(name):
        return name.rsplit("/", 1)[-1].startswith("op_")

    def section(name):
        return name.rsplit("/", 1)[-1].startswith("section_")

    listed = [child for child in children if row(child[0]) or section(child[0])]
    if not listed:
        return children
    shown, previous_shown = set(), False
    for name, _, _, platform in listed:
        if section(name):
            previous_shown = False
            continue
        visible = _platform_shown(platform, platforms)
        if "vr" in platforms and key(name) in port_settings.SETTINGS_NOT_IN_VR:
            visible = False
        if key(name) == "create_profile":
            visible = False
        if mode and mode not in port_settings.LIGHTING_MODE_ROWS.get(key(name), mode):
            visible = False
        if visible and previous_shown and key(name) in port_settings.SETTINGS_SAME_PLACE | {"window_size"}:
            continue
        previous_shown = visible
        if visible:
            shown.add(name)
    y, rows_above, packed = listed[0][2], False, {}
    for index, (name, x, _, platform) in enumerate(listed):
        if section(name):
            following = []
            for other in listed[index + 1:]:
                if section(other[0]):
                    break
                following.append(other[0])
            if not _platform_shown(platform, platforms) or not shown.intersection(following):
                continue
            y += port_settings.SETTINGS_SECTION_GAP if rows_above else 0
            packed[name] = (name, x, y, platform)
            y += port_settings.SETTINGS_SECTION_STEP
        elif name in shown:
            packed[name] = (name, x, y, platform)
            y += port_settings.SETTINGS_ROW_STEP
            rows_above = True
    return [packed.get(child[0], child) for child in children
            if child[0] in packed or not (row(child[0]) or section(child[0]))]


def layout(root, width, platforms=DESKTOP, every=None, mode=None):
    """the widgets under a screen (root) as drawn on a screen this wide: a Box each, in drawing order (a list's
    description after its children); of the menu files', or of every (widgets_of's). A Settings tab's or page's
    rows are packed as the engine packs them (_settings_rows_packed): with a lighting mode
    (port_settings.LIGHTING_MODES), its rows; None: every mode's, the most a list can have."""
    extra = port_settings.wide_extra_of(width)
    every = every or widgets()
    boxes = []

    def walk(name, x, y, path):
        widget = every[name]
        attributes = widget.attributes
        if not _platform_shown(attributes.get("platform"), platforms):
            return
        # (and past 16:9, a menu engine's look's past: its move and growth at 21:9 past 16:9's, menu_tags.c's
        # ui_widget_port_past_scale: engine_widgets)
        dx, dw = (int(value) for value in attributes.get("wide", "0 0").split())
        past_dx, past_dw = (int(value) for value in attributes.get("past", "0 0").split())
        past = min(max(width - port_settings.WIDE_WIDTH, 0), 4 * PAST_EXTRA)
        x += port_settings.wide_scale(dx, extra) + past_scale(past_dx, past)
        left, top = int(attributes.get("left", 0)), int(attributes.get("top", 0))
        right = left + int(attributes.get("width", 640)) + port_settings.wide_scale(dw, extra) + past_scale(past_dw,
                                                                                                             past)
        bottom = top + int(attributes.get("height", 480))
        boxes.append(Box(name.rsplit("/", 1)[-1], x + left, y + top, x + right, y + bottom, path))
        children = widget.children
        if name.startswith(port_settings.PE + "/") and name.endswith("/options_menu"):
            children = _settings_rows_packed(children, platforms, mode)
        for child, child_x, child_y, platform in children:
            if _platform_shown(platform, platforms):
                walk(child, x + child_x, y + child_y, path + (name.rsplit("/", 1)[-1],))
        if attributes.get("description"):
            walk(attributes["description"], x, y, path + (name.rsplit("/", 1)[-1],))

    walk(root, 0, 0, ())
    return boxes


# a past's width, 21:9's past 16:9's (menu_tags.c's WIDE_LAYOUT_PAST_EXTRA)
PAST_EXTRA = port_settings.WIDE_SCREENS["21:9"] - port_settings.WIDE_WIDTH


def past_scale(value, past):
    """a past attribute's move or growth at a width past 16:9's, rounded as menu_tags.c's
    ui_widget_port_past_scale (half away from 0)"""
    scaled = value * past
    return (abs(scaled) + PAST_EXTRA // 2) // PAST_EXTRA * (-1 if scaled < 0 else 1)


@lru_cache(maxsize=None)
def engine_widgets():
    """the widgets as the menu engine lays them out (display.menu_engine "new"): the menu files', those of its tall
    layout in their place (tools/port_settings.py's tall_widgets: the browsers')"""
    every = dict(widgets())
    every.update(widgets_of([ElementTree.fromstring("<menus>" + "".join(port_settings.tall_widgets())
                                                    + "</menus>")]))
    return every


def named(boxes, name):
    """the boxes of the widgets of a name"""
    return [box for box in boxes if box.name == name]


def one(boxes, name):
    found = named(boxes, name)
    assert len(found) == 1, f"{name}: {len(found)} drawn"
    return found[0]
