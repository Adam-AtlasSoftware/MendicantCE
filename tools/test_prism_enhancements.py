"""The lighting modes and the enhancements (v4 geopatch PLAN_FINAL.md sections 4 and 10, M3): the enhancements'
registry (port/linux/src/port_config.c's config_enhancements) and its menu twin (tools/port_settings.py's
ENHANCEMENTS) agree and keep the rules that stop variant explosion; Classic, Enhanced and Path Traced never mix in
the menus; no bake option or bake status exists; the release's defaults are Enhanced everything; and an older
player's config (Adam's kit config: bake "auto", bake_rebake 1, Raster, "prism" quality, Classic's PRISM extras)
loads without baking and maps onto the new settings."""
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
from tools import port_settings  # noqa: E402
from tools.port_settings import LIGHTING_MODE_ROWS, PAGES, SECTION  # noqa: E402
from tools.test_linux_port import config_check_build  # noqa: E402

CONFIG = (ROOT / "port/linux/src/port_config.c").read_text()
MENU = (ROOT / "port/linux/game/menu_functions.c").read_text()
PRISM = "video_settings/prism"


def _key(entry):
    label, setting, _choices, _help, _platform, *named = entry
    return named[0] if named else setting.split(".", 1)[1] if setting else None


def _c_table(name):
    body = CONFIG.split("} %s[] =" % name, 1)[1].split("};", 1)[0]
    return body


def _c_enhancements():
    rows = {}
    for match in re.finditer(r'\{ "([\w.]+)", ([^{}]+?),\s+(_enhancement_\w+),\s+(NULL|"\w+") \}',
                             _c_table("config_enhancements")):
        name, middle, kind, content = match.groups()
        strings = re.findall(r'"((?:\\"|[^"])*)"', middle)
        levels = tuple(value.replace('\\"', "") for value in strings[:3])
        rows[name] = {"levels": levels, "group": strings[3], "modes": strings[4],
                      "kind": kind.replace("_enhancement_", ""), "content": None if content == "NULL" else content.strip('"')}
    return rows


def _setting_entry(name):
    match = re.search(r'\t\{ "%s", (_config_\w+), ("(?:\\"|[^"])*"|\w+), "(\w+)", _environment_\w+,\s*([_\w| ]+),' %
                      re.escape(name), CONFIG)
    assert match, name
    return match.groups()


# ---------- the registry and its twin

def test_registry_twin_agrees():
    c = _c_enhancements()
    py = {setting: {"levels": levels, "group": group, "modes": modes, "kind": kind, "content": content}
          for group, setting, _label, _choices, levels, modes, kind, content, _help in port_settings.ENHANCEMENTS}
    assert c == py
    # (each level is one of its row's choices, and each group's rows are on its own page)
    for group, setting, _label, choices, levels, _modes, _kind, _content, _help in port_settings.ENHANCEMENTS:
        assert set(levels) <= {value for _, value in choices}, setting
        page = port_settings.ENHANCEMENT_GROUPS[group][0][0]
        assert setting in {entry[1] for entry in PAGES[page]["rows"] if entry[0] != SECTION}, setting


def test_registry_rules():
    """only the world's geometry changes the bake (WORLD_BAKED); no BAKE_LAYER row until a layer ships in a
    BAKE_VERSION; collision (SIM) is never the preset's; the preset never sets the lighting, display.graphics'
    settings or collision; the only content kind is the foliage's, and its rows are greyed until it ships"""
    rows = _c_enhancements()
    assert [name for name, row in rows.items() if row["kind"] == "world_baked"] == ["display.world_geometry"]
    assert not [name for name, row in rows.items() if row["kind"] in ("bake_layer", "sim")]
    graphics = set(re.findall(r'\{ "([\w.]+)",', _c_table("config_graphics")))
    assert not graphics & set(rows)
    assert not {"display.lighting", "game.campaign_collision", "display.graphics"} & set(rows)
    assert {row["content"] for row in rows.values()} == {None, "foliage"}
    assert all(set(row["modes"]) <= set("CEP") and row["modes"] for row in rows.values())
    # (World Geometry needs a PRISM lighting: greyed in Classic)
    assert rows["display.world_geometry"]["modes"] == "EP"
    # (every member says the preset chooses it, and the group's settings exist in the table)
    for name in rows:
        entry = CONFIG.split('\t{ "%s",' % name, 1)[1].split("},\n", 1)[0]
        assert "ENHANCEMENTS_CHOOSES" in entry, name


def test_menu_greys_what_cannot_apply():
    """menu_functions.c greys a group row by the registry's modes and content (enhancement_rows_show) and Campaign
    Collision until it ships; the preset's row shows CUSTOM as Quality does"""
    body = MENU.split("static void enhancement_rows_show(struct widget_instance *list)", 1)[1].split("\n}\n", 1)[0]
    assert "config_enhancement_info(" in body and '"op_campaign_collision"' in body and "content != NULL" in body
    assert "enhancements_preset_show(list);" in MENU and '"enhancements_spinner"' in MENU


# ---------- the modes never mix

def _shown(folder, mode):
    return [entry for entry in PAGES[folder]["rows"] if entry[0] != SECTION and
            mode in LIGHTING_MODE_ROWS.get(_key(entry), "CEP")]


def _reachable(mode):
    """every row Settings shows in a lighting mode under Video > PRISM: PRISM's own and those of each page a shown
    link opens"""
    rows = list(_shown(PRISM, mode))
    for entry in list(rows):
        if entry[1] is None and entry[2] and "/video_settings/prism/" in str(entry[2]):
            folder = next(f for f, page in PAGES.items()
                          if port_settings._page_screen(f, page["screen"]) == entry[2])
            rows += _shown(folder, mode)
    return rows


def test_classic_reaches_no_ray_tracing():
    """in Classic no reachable row has a key matching prism_rt*, prism_pt*, *traced* or ray_tracing, nor a
    choice of traced anything; the Classic page is CE's own options alone"""
    for entry in _reachable("C"):
        key, setting = _key(entry) or "", entry[1] or ""
        for text in (key, setting):
            assert not re.search(r"prism_rt|prism_pt|traced|ray_tracing", text), entry[:2]
        for _shown_word, value in entry[2] if isinstance(entry[2], list) and entry[1] != "display.lighting" else []:
            assert "traced" not in value, entry[:2]
    assert [entry[1] for entry in _shown(f"{PRISM}/classic", "C")] == [
        "display.shadow_resolution", "display.per_pixel_lighting", "display.dynamic_lights"]


def test_no_key_on_two_mode_pages():
    """no setting is reachable from two lighting modes' own pages (each PRISM lighting has its own keys:
    port_config.c's config_mode_keys)"""
    pages = {mode: {entry[1] for entry in _shown(folder, mode) if entry[1]}
             for mode, folder in (("C", f"{PRISM}/classic"), ("E", f"{PRISM}/enhanced"),
                                  ("P", f"{PRISM}/path_tracing"))}
    assert not pages["C"] & pages["E"] and not pages["C"] & pages["P"] and not pages["E"] & pages["P"]
    keys = re.findall(r'\{ "([\w.]+)", (NULL|"[\w.]+"), (NULL|"[\w.]+") \}', _c_table("config_mode_keys"))
    assert ("display.dynamic_lights", '"display.enhanced_dynamic_lights"', '"display.pt_dynamic_lights"') in keys
    assert "config_mode_value(config_value(name, type), type)" in CONFIG


def test_no_traced_choice_with_ray_tracing_off():
    """each traced row of Enhanced's page is shown only while its RAY TRACING is on (lighting_rows' conditions)"""
    table = MENU.split("} const lighting_rows[] =", 1)[1].split("};", 1)[0]
    entries = re.findall(r'\{ "op_(\w+)", ([A-Z_| ]+), (NULL|"[\w.]+"), (NULL|"[!\w]+") \}', table)
    for row in ("prism_object_shadows", "prism_rt_objects", "prism_rt_cache", "prism_rt_alpha"):
        conditions = [(setting, value) for key, _modes, setting, value in entries if key == row]
        assert conditions == [('"display.prism_ray_tracing"', '"on"')], row
    # (the traced shadows off with it in the renderer's settings too)
    deferred = (ROOT / "port/linux/game/prism_deferred.c").read_text()
    assert 'config_string("display.prism_ray_tracing")' in deferred


def test_no_bake_option_or_status_anywhere():
    """players never bake: no menu row of a bake setting, no bake status page or rows, no bake indicator, and every
    display.bake* setting but the shipped folder is no player's (_platform_internal: never read from the file)"""
    for page in PAGES.values():
        for entry in page["rows"]:
            if entry[0] == SECTION:
                continue
            assert not (entry[1] or "").startswith("display.bake"), entry[:2]
            assert not (_key(entry) or "").startswith(("bake", "light_bake")), entry[:2]
    for name in ("display.bake", "display.bake_quality", "display.bake_rebake", "display.bake_indicator",
                 "display.bake_budget", "display.bake_folder"):
        assert "_platform_internal" in _setting_entry(name)[3], name
    assert _setting_entry("display.bake")[1] == '"\\"off\\""'
    assert "bake_rows_show" not in MENU
    assert "return FALSE;" in (ROOT / "port/linux/game/prism_bake_menu.c").read_text().split(
        "boolean prism_bake_key_setting(", 1)[1].split("\n}\n", 1)[0]


def test_release_defaults():
    """Enhanced everything (PLAN_FINAL section 10): Enhanced lighting, Enhanced geometry, the preset at its
    highest, ray tracing on; Campaign collision original until M8"""
    assert _setting_entry("display.lighting")[1] == '"\\"enhanced\\""'
    assert _setting_entry("display.world_geometry")[1] == '"\\"enhanced\\""'
    assert _setting_entry("display.enhancements")[1] == '"\\"realistic\\""'
    assert _setting_entry("display.prism_ray_tracing")[1] == '"\\"on\\""'
    assert _setting_entry("game.campaign_collision")[1] == '"\\"original\\""'
    # (each enhancement's own default is the realistic level: a new file is REALISTIC, not CUSTOM)
    for name, row in _c_enhancements().items():
        default = _setting_entry(name)[1][1:-1].replace('\\"', '"').strip('"')
        assert default == row["levels"][2], name


# ---------- an older player's config

KIT = '''[display]
lighting = "raster"
graphics = "prism"
bake = "auto"
bake_quality = "high"
bake_rebake = 1
bake_indicator = true
prism_world_geometry = "stock"
dynamic_lights = "enhanced"
shadow_resolution = 1024
prism_rt = "gi"
prism_rt_world = "remodel"
prism_rt_objects = "auto"
prism_rt_alpha = true
prism_lightmaps = "rebaked"
prism_shadows = "cascaded"
prism_baked_shadows = "sharp"
ambient_occlusion = "world"
prism_models = "max"
prism_textures = "max"
prism_materials = "pbr"
'''


@pytest.fixture
def config_check(tmp_path):
    return config_check_build(tmp_path, ["-DHALO_GUEST=1"])


def test_kit_config_loads_without_baking(config_check):
    """Adam's kit config: bake "auto" and bake_rebake 1 are read as nothing (the bake off, nothing rebaked), Raster
    is Enhanced, "prism" quality is Enhanced, the world geometry's old key becomes the new one, each PRISM lighting
    gets the shared dynamic lights it had, Classic's ray tracer and PRISM extras are off; the file loses the old
    lines and gains the new ones, and loads again unchanged"""
    config_check.config.write_text(KIT)
    assert config_check("get", "display.bake") == "off"
    assert config_check("get", "display.bake_rebake") == "0"
    assert config_check("get", "display.bake_indicator") == "false"
    assert config_check("get", "display.lighting") == "enhanced"
    assert config_check("get", "display.graphics") == "enhanced"
    assert config_check("get", "display.world_geometry") == "classic"
    assert config_check("get", "display.enhanced_dynamic_lights") == "enhanced"
    assert config_check("get", "display.pt_dynamic_lights") == "enhanced"
    assert config_check("get", "display.enhanced_shadow_resolution") == "1024"
    assert config_check("get", "display.prism_rt") == "off"
    assert config_check("get", "display.prism_lightmaps") == "stock"
    assert config_check("get", "display.prism_baked_shadows") == "off"
    assert config_check("get", "display.prism_models") == "max"
    text = config_check.config.read_text()
    for gone in ("bake", "bake_rebake", "bake_quality", "prism_world_geometry", "prism_rt", "prism_rt_world",
                 "prism_lightmaps", "prism_shadows", "prism_baked_shadows", "ambient_occlusion"):
        assert not re.search(r"^(# )?%s = " % gone, text, re.M), gone
    assert re.search(r'^world_geometry = "classic"$', text, re.M)
    assert re.search(r'^enhanced_dynamic_lights = "enhanced"$', text, re.M)
    assert re.search(r'^lighting = "enhanced"$', text, re.M)
    # (the shared key the game reads is the lighting's own: Enhanced's)
    assert config_check("read", "display.dynamic_lights") == "enhanced"
    assert config_check("read", "display.prism_rt_alpha") == "true"
    # (loaded again: nothing more changes)
    config_check("get", "display.lighting")
    assert config_check.config.read_text() == text


def test_new_file_is_enhanced_everything(config_check):
    assert config_check("get", "display.lighting") == "enhanced"
    assert config_check("get", "display.world_geometry") == "enhanced"
    assert config_check("get", "display.enhancements") == "realistic"
    assert config_check("get", "display.prism_models") == "max"
    text = config_check.config.read_text()
    assert "\n# world_geometry = \"enhanced\"\n" in text and "\nbake =" not in text and "# bake =" not in text


def test_enhancements_preset_chooses_its_rows(config_check):
    """display.enhancements gives its rows not set by hand their level (Classic: the original game), a row set at
    the preset's value is left to it again, a row set otherwise stays (CUSTOM in the menu)"""
    config_check("write", "display.enhancements", "classic")
    assert config_check("get", "display.world_geometry") == "classic"
    assert config_check("get", "display.prism_models") == "stock"
    assert config_check("get", "display.terrain") == "off"
    config_check("write", "display.prism_models", "max")
    config_check("write", "display.enhancements", "remastered")
    assert config_check("get", "display.prism_models") == "max"
    assert config_check("get", "display.world_geometry") == "enhanced"
    assert config_check("get", "display.foliage_grass") == "enhanced"
    config_check("write", "display.prism_models", "enhanced")
    assert re.search(r'^# prism_models = "enhanced"$', config_check.config.read_text(), re.M)
    config_check("write", "display.enhancements", "realistic")
    assert config_check("get", "display.prism_models") == "max"


def test_lighting_words_renamed(config_check):
    """the older words of display.lighting and the world's geometry, in the file or the environment, are the new
    ones"""
    for old, new in (("ce", "classic"), ("raster", "enhanced"), ("rebaked", "enhanced"), ("path_traced", "pathtraced"),
                     ("pathtraced", "pathtraced"), ("classic", "classic")):
        config_check.config.write_text('[display]\nlighting = "%s"\n' % old)
        assert config_check("get", "display.lighting") == new, old
    config_check.config.write_text('[display]\nprism_world_geometry = "remodel"\n')
    assert config_check("get", "display.world_geometry") == "enhanced"
