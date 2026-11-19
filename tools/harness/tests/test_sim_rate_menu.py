"""Video Setup > Graphics' SIMULATION row (game.sim_rate): the row tools/port_settings.py writes, and
port/linux/game/menu_functions.c's sim_rate_row_show, which locks it at the game's rate (ORIGINAL (30), or a Standard game's agreed rate) where the game sets the setting
aside (sim_rate.c's sim_rate_locked: a network game of other machines, a film's playback), with OK passing over the locked
spinner."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["unlocked", "locked", "locked-agreed", "unlock", "unrelated", "lighting-mode"]

NEGATIVE_CONTROLS = {
    "ok-writes-locked": (("\tif (spinner->disabled || index < 0", "\tif (index < 0"), "locked"),
    "never-disabled": (("\trow->disabled = locked;\n", ""), "locked"),
    "not-dimmed": (("locked ? 0.4f : 1.0f", "1.0f"), "locked"),
    "shows-setting-locked": (("\t\tspinner->parameters.list.selected_index = setting_value_index(setting, classic);\n",
                              ""), "locked"),
    "locked-shows-30": (('"%ld", sim_rate_next()', '"%d", SIM_RATE_CLASSIC'), "locked-agreed"),
    "focus-kept": (("\tif (locked && list->focused_child == row)", "\tif (0)"), "locked"),
    "unlock-keeps-30": (("\t\tspinner->parameters.list.selected_index = setting->loaded_index;\n", ""), "unlock"),
    # (Defaults sets a row the lighting mode hides)
    "hidden-defaulted": (("\tif (lighting_row_hidden(spinner->parent))\n\t\treturn TRUE;\n", ""), "lighting-mode"),
    # (a row of no lighting mode's, as SIMULATION, hidden too)
    "hidden-by-any-mode": (("!lighting_row_shown(row->name, TRUE, &found) && found",
                            "!lighting_row_shown(row->name, TRUE, &found)"), "lighting-mode"),
}


def lighting_rows(menus):
    """menu_functions.c's lighting_rows table (the rows of one lighting mode or two)."""
    at = menus.index("lighting_rows[] =")
    return menus[menus.rindex("static struct", 0, at):menus.index("};", at) + 2]


def generated(fault=None):
    menus = read("port/linux/game/menu_functions.c")
    config = enum_with(read("port/linux/game/sim_rate.h"), "SIM_RATE_CLASSIC") + "\n"
    config += structure(menus, "pc_menu_setting") + "\n"
    config += enum_with(menus, "LIGHTING_C = 1") + "\n" + lighting_rows(menus) + "\n"
    # (Defaults leaves a row the lighting mode shown hides: lighting_row_hidden, before setting_default_show)
    code = "\n\n".join(function(menus, name) for name in (
        "text_is_number", "setting_value_index", "setting_changed_save", "lighting_mode_shown", "lighting_row_shown",
        "lighting_row_hidden", "setting_default_show", "sim_rate_row_show"))
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sim_rate_menu", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sim_rate_menu", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_the_row_on_advanced_graphics():
    """Advanced Graphics has the row, its values the rates port_config.c's game.sim_rate takes (its default, 30,
    first and called ORIGINAL), on every platform's page, and the page runs sim_rate_row_show each frame."""
    sys.path.insert(0, str(ROOT / "tools"))
    import port_settings

    name = "main_menu.settings_select.player_setup.player_profile_edit.video_settings.advanced.xml"
    text = "\n".join(port_settings.settings_files()[name])
    spinner = re.search(r'<widget name="[^"]*/sim_rate_spinner"[^>]*>', text).group(0)
    assert 'strings="ORIGINAL (30)|60|120"' in spinner and 'values="30|60|120"' in spinner, spinner
    assert 'setting="game.sim_rate"' in spinner
    child = re.search(r'<child widget="[^"]*/op_sim_rate"[^>]*/>', text).group(0)
    assert "platform=" not in child, child
    labels = re.search(r'<strings name="[^"]*/advanced/labels">(.*?)</strings>', text, re.S).group(1)
    assert '<string text="TICK RATE"/>' in labels
    # its row fits above the buttons (at 414), packed as the engine packs the rows each frame (the files' places
    # are every lighting mode's and every platform's rows), in each lighting mode, on the desktop and Android
    from harness import menu_layout
    root = f"{port_settings.PE}/video_settings/advanced/advanced_graphics_screen"
    for platforms in (menu_layout.DESKTOP, menu_layout.ANDROID):
        for mode in port_settings.LIGHTING_MODES:
            row = menu_layout.one(menu_layout.layout(root, 640, platforms, mode=mode), "op_sim_rate")
            assert row.y1 <= 414, (sorted(platforms), mode, row)
    assert '{ "game.sim_rate", _config_integer, "60", "HALO_SIM_RATE"' in read("port/linux/src/port_config.c")
    update = read("port/linux/game/menu_functions.c")
    update = update[update.index("static void settings_page_update("):]
    assert update.index("sim_rate_row_show(list);") < update.index("settings_each(screen, setting_stash);")
