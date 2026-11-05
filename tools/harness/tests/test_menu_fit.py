"""The port's screens' rows, buttons and prompts hold what is in them (issues 669 and 670; harness/menu_fit.py): in
each list of rows (Server Setup's, VR's, the setup options') no row over the next, its height its pitch's
(port_settings.option_row_height); every one-line text on or in a framed part (a button, a row, a prompt, the
lobby's players' rows and controls) and every spinner's value in such a row with its capitals on its middle
(port_settings.caps_text_y); every footer prompt's key cap and words inside its picture (port_settings.
prompt_fit_width). The files as shipped, and the generator's output with a fault, which each check must find."""

import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import menu_fit, menu_layout  # noqa: E402

port_settings = menu_layout.port_settings
CHECKS = {"rows": menu_fit.rows_overlapping, "centred": menu_fit.texts_off_middle,
          "prompts": menu_fit.prompt_overflows}


def generated():
    """the generator's files, as it writes them now, over the shipped files' other widgets"""
    files = {**port_settings.settings_files(), **port_settings.multiplayer_files(), **port_settings.campaign_files()}
    roots = [ElementTree.fromstring("\n".join(lines).split("?>", 1)[-1]) for lines in files.values()]
    ours = menu_layout.widgets_of(roots)
    return {**menu_layout.widgets(), **ours}, sorted(ours)


@pytest.mark.parametrize("check", CHECKS)
def test_shipped(check):
    problems = CHECKS[check](menu_layout.widgets(), menu_fit.port_names())
    assert not problems, "\n".join(problems)


@pytest.mark.parametrize("check", CHECKS)
def test_generated(check):
    """the generator's output is what is shipped, and passes too"""
    widgets, names = generated()
    assert set(names) == set(menu_fit.port_names())
    problems = CHECKS[check](widgets, names)
    assert not problems, "\n".join(problems)


def test_server_setup_value_column():
    """Server Setup's values (the game's name, typed, as the spinners' values) in one column, in large_ui; the
    invite link, long, in small_ui"""
    every = menu_layout.widgets()
    base = f"{port_settings.MT}/server_settings"
    places = {}
    for row in ("server_name", "max_players", "game_mode", "listing", "password", "invite"):
        for child, x, _, _ in every[f"{base}/op_{row}"].children:
            if child.endswith(("_value", "_spinner")):
                attributes = every[child].attributes
                places[row] = (x + int(attributes.get("left", 0)), int(attributes["width"]), attributes["font"])
    assert len(places) == 6, places
    assert len({(x, width) for x, width, _ in places.values()}) == 1, places
    assert {font for row, (*_, font) in places.items() if row != "invite"} == {"ui\\large_ui"}


# a fault in the generator, and the check that must find it
NEGATIVE_CONTROLS = {
    # the rows as tall as before, whatever their pitch (issue 669)
    "rows-28-tall": ("option_row_height", lambda pitch: 28, "rows"),
    # the rows' picture as tall as the pitch: no room between them
    "rows-no-gap": ("OPTION_ROW_GAP", -1, "rows"),
    # capitals placed without the ascent (text_y as a box's top): low in every box (issue 670)
    "text-y-without-ascent": ("caps_text_y", lambda height, top=0, large=False: (height - 10) // 2 - top, "centred"),
    # the prompts as wide as laid out by hand (issue 670's ADD PLAYER)
    "prompts-not-fitted": ("prompt_fit_width", lambda words, width, text_x, browser=False: width, "prompts"),
    # a key's cap measured without its gap
    "cap-without-gap": ("KEY_CAP_GAP", 0, "prompts"),
}


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, monkeypatch):
    name, value, check = NEGATIVE_CONTROLS[control]
    monkeypatch.setattr(port_settings, name, value)
    monkeypatch.setattr(port_settings, "_PROMPT_WIDTHS", set())
    widgets, names = generated()
    if control == "cap-without-gap":
        # (the files laid out without it, checked with it)
        monkeypatch.setattr(port_settings, "KEY_CAP_GAP", 3)
    assert CHECKS[check](widgets, names), f"the generator with a fault ({control}) passed '{check}'"
