"""Every event handler function the menu files run (run="..." in port/assets/menus: the generated screens, the model,
the hand-written screens' blocks) is one the game has: the port's (port/linux/game/menu_tags.c's port_function_names)
or the engine's (source/interface/ui_widget_event_handler_functions.c's event_handler_function_list names). One
missing makes menu_tags.c refuse the port's menus at load ("there is no event handler function ...", "menus: not
added; using the game's own menus"), as "port settings add maps" did; the negative control takes a name out."""

import html
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import ROOT, read  # noqa: E402

MENUS = ROOT / "port/assets/menus"


def table_names(table_name, source=None):
    """a menu_tags.c table of names' names"""
    source = source if source is not None else read("port/linux/game/menu_tags.c")
    table = re.search(table_name + r"\[\] =\n\{\n(.*?)\n\};", source, re.S).group(1)
    table = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
    return set(re.findall(r'"([^"]+)"', table))


def port_names(source=None):
    return table_names("port_function_names", source)


def engine_names():
    source = read("source/interface/ui_widget_event_handler_functions.c")
    start = source.index("event_handler_function_list =\n{")
    body = source[start:source.index("\n};", start)]
    names = body[body.index("\n\t},\n\t{") + 5:]
    return set(re.findall(r'"([^"]+)"', re.sub(r"/\*.*?\*/", "", names, flags=re.S)))


def menu_runs(pattern=r'\brun="([^"]+)"'):
    runs = {}
    for path in sorted(MENUS.rglob("*.xml")):
        for name in re.findall(pattern, path.read_text(encoding="utf-8")):
            runs.setdefault(html.unescape(name), path.relative_to(MENUS).as_posix())
    return runs


def missing(port):
    known = port | engine_names()
    return {name: where for name, where in menu_runs().items()
            if name not in known and not name.startswith("unwired ")}


def test_every_run_registered():
    assert menu_runs(), "no run= found in the menu files"
    assert not missing(port_names()), missing(port_names())


def test_every_data_input_registered():
    """and every game data input (<data input="...">: menu_tags.c's port_game_data_input_names, then the game's
    game_data_input_names), refused at load the same way ("there is no game data input")"""
    known = table_names("port_game_data_input_names") | table_names("game_data_input_names")
    inputs = menu_runs(r'<data input="([^"]+)"')
    assert inputs, "no data inputs found in the menu files"
    assert not {name: where for name, where in inputs.items() if name not in known and not name.startswith(
        "unwired ")}


@pytest.mark.parametrize("name", ["port settings add maps"])
def test_negative_control(name):
    """a name taken out of port_function_names is caught"""
    source = read("port/linux/game/menu_tags.c")
    assert f'"{name}"' in source
    assert name in missing(port_names(source.replace(f'"{name}", ', "").replace(f'"{name}"', "")))
