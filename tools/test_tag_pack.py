"""The tag packs' manifests (port/assets/tag_packs), as the game reads them
(port/linux/src/port_tag_packs.c) and tools/tag_pack.py writes their packs.

Watanoge's (halo-content-evolved, opence_floodfection c5c0e3c86), with
OpenH1's pack, Forge's, in place of Floodfection's."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import tag_pack  # noqa: E402

GAME = ROOT / "port/linux/game"


def test_manifests_are_named_and_sized_as_the_game_reads_them():
    """Each is <owner>_<what>.toml, named so inside, of 1 to 32 maps of at
    most 31 letters, wanted by forms the game knows, and its hash its own."""
    found = tag_pack.manifests()
    assert found
    conditions = re.findall(r'\{ "([a-z_]+)", ', (GAME / "tag_pack_conditions.c").read_text())
    settings = set(re.findall(r'\{ "([a-z_.]+)", _config_boolean', (ROOT / "port/linux/src/port_config.c").read_text()))
    hashes = set()
    for name, path in found.items():
        assert re.fullmatch(r"[a-z0-9]+_[a-z0-9_]+", name), name
        manifest = tag_pack.Manifest(path)
        assert 1 <= len(manifest.maps) <= tag_pack.MAXIMUM_MAPS
        assert all(len(leaf) < tag_pack.MAP_NAME for leaf in manifest.maps)
        for wanted in tag_pack.tomllib.loads(path.read_text()).get("wanted", []):
            assert wanted == "always" or (wanted.startswith("setting:") and wanted[8:] in settings) or \
                (wanted.startswith("condition:") and wanted[10:] in conditions), (name, wanted)
        hashes.add(manifest.content())
    assert len(hashes) == len(found)


def test_the_game_names_packs_that_exist():
    """Every pack the game's code asks for by name has a manifest."""
    found = tag_pack.manifests()
    names = set()
    for source in sorted(GAME.glob("*.[ch]")):
        names.update(re.findall(r'#define [A-Z_]+_PACK "([a-z0-9_]+)"', source.read_text()))
    assert names and names <= set(found), names - set(found)


def test_keep_rules():
    """Includes narrow, excludes remove, by the tag's name in any case."""
    forge = tag_pack.Manifest(tag_pack.manifests()["openh1_forge"])
    assert forge.keep(tag_pack.BITM, "characters\\monitor\\bitmaps\\monitor")
    assert forge.keep(tag_pack.BITM, "CHARACTERS\\ELITE\\bitmaps\\elite")
    assert forge.keep(tag_pack.SND, "sound\\dialog\\grunt\\x")
    assert not forge.keep(tag_pack.BITM, "characters\\hunter\\bitmaps\\hunter")
    assert not forge.keep(tag_pack.BITM, "levels\\b30\\bitmaps\\x")
