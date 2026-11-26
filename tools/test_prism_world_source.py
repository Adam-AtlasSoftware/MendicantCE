"""The enhanced world's geometry from the map's sidecar (PLAN_FINAL.md sections 1.2 and 3.4, M2): the ladder that
chooses a map's world (port/linux/game/prism_identity.c's prism_world_ladder, run through the game's own C), and the
engine's wiring, read from the source: the geometry comes from maps_enhanced/<map>.prsx and never from a .pwg, the
world is chosen once a map, Classic lighting never draws the enhanced geometry, and the bake's key holds the BSP's
enhanced world_id."""
import os
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "port/linux/game"
sys.path.insert(0, str(ROOT / "tools"))
from geopatch import native  # noqa: E402


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    try:
        return native.Native(tmp_path_factory.mktemp("native"))
    except native.NativeError as error:
        if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS"):
            pytest.fail("no C compiler: %s" % error)
        pytest.skip("no C compiler: %s" % error)


def _source(name):
    return (GAME / name).read_text()


def test_ladder_every_input(tool):
    """rung 3 whenever the lighting is Classic; rung 1 only with the geometry asked for, the sidecar verified and
    every BSP's enhanced pack (or the farm's unbaked override); rung 2 otherwise: geometry and light never
    disagree"""
    table = tool.ladder()
    assert len(table) == 32
    for (classic, geometry, sidecar, packs, unbaked), rung in table.items():
        if classic:
            expected = 3
        elif geometry and sidecar and (packs or unbaked):
            expected = 1
        else:
            expected = 2
        assert rung == expected, (classic, geometry, sidecar, packs, unbaked, rung)


def test_geometry_comes_from_the_sidecar_alone():
    """prism_world_geometry.c reads no file of its own: its remodel image is prism_world_source.c's, decoded from
    the sidecar (prism_geom.c); the .pwg folder is the farm's intermediate"""
    geometry = _source("prism_world_geometry.c")
    code = re.sub(r"/\*.*?\*/", "", geometry, flags=re.S)
    assert "fopen" not in code and "prism_world/" not in code and ".pwg\"" not in code
    assert "prism_world_source_image(" in code
    source = re.sub(r"/\*.*?\*/", "", _source("prism_world_source.c"), flags=re.S)
    assert '#define SIDECAR_FOLDER "maps_enhanced"' in source
    assert "prism_geom_decode(" in source and "prsx_section_read(" in source
    # (the sidecar opened only past the Classic check, and held until the map's tags go)
    assert source.index("prism_world_ladder(lighting_classic()") < source.index("prsx_open(")
    assert "prism_world_source_map_unloaded();" in code


def test_classic_lighting_never_draws_it():
    """the enhanced geometry is drawn only while the map is latched to rung 1 and the lighting is not Classic,
    asked every frame (a change to Classic in a map draws the map's own at once)"""
    source = re.sub(r"/\*.*?\*/", "", _source("prism_world_source.c"), flags=re.S)
    body = source[source.index("boolean prism_world_enhanced(void)"):]
    body = body[:body.index("}")]
    assert "PRISM_WORLD_RUNG_ENHANCED" in body and "!lighting_classic()" in body and "!world_source.refused" in body
    geometry = re.sub(r"/\*.*?\*/", "", _source("prism_world_geometry.c"), flags=re.S)
    assert "boolean enabled = prism_world_enhanced();" in geometry
    assert 'config_string("display.prism_world_geometry")' not in geometry
    for name in ("prism_bake.c", "prism_bake_menu.c"):
        code = re.sub(r"/\*.*?\*/", "", _source(name), flags=re.S)
        assert 'config_string("display.prism_world_geometry")' not in code, name


def test_the_bake_is_keyed_to_the_world_id():
    """the game sends the BSP's enhanced world_id with the bake's control (zeros for the classic world) and the
    baker mixes it into the key's geometry hash, so a bake is used only for the sidecar geometry it was made from"""
    stream = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
    record = stream[stream.index("struct xgpu_stream_prism_bake\n"):]
    record = record[:record.index("};")]
    assert "unsigned char world_id[32];" in record
    assert "XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_prism_bake, 80 + XGPU_BAKE_NAME + 32);" in stream
    assert "prism_world_source_world_id((short)bake->bsp_index, bake->world_id);" in _source("prism_bake.c")
    baker = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c").read_text()
    assert re.search(r"memcmp\(bake\.control\.world_id, zero, sizeof\(zero\)\)\)\s*\n\s*b->key\.geometry_hash = "
                     r"prism_bake_hash64\(b->key\.geometry_hash, bake\.control\.world_id, 32\);", baker)
    # (the shipped bake's manifest names the enhanced world's BSPs by that id, and the ladder looks for it)
    assert '"geo:prsx:%s:%s"' in _source("prism_bake_menu.c")
    assert '"geo:prsx:%s"' in _source("prism_world_source.c")


def test_prepared_records_keyed_to_the_world_id():
    """the prepared draws' records (render_prepared.c) of a map drawn with its enhanced geometry are keyed by the
    checksum and the BSP's enhanced world_id (PLAN_FINAL section 3.4: render caches on checksum ^ world_id), the
    classic world's as before (its id all zeros)"""
    prepared = re.sub(r"/\*.*?\*/", "", _source("render_prepared.c"), flags=re.S)
    choose = prepared[prepared.index("static boolean prepared_choose(boolean mirrors)"):]
    choose = choose[:choose.index("\n}\n")]
    assert "prism_world_source_world_id(global_structure_bsp_index_get(), id);" in choose
    assert choose.index("hash ^= world;") < choose.index("prepared.key = hash;")
