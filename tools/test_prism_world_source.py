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
    """the game sends the BSP's world_id with the bake's control (the enhanced one, else the classic one where the
    map's classic_world_id is known: M4) and the baker keys the bake to it (the key's own world_id, SCENE_VERSION
    and variant, the map's checksum 0, and the geometry hash), so a bake is used only for the world it was made
    from"""
    stream = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
    record = stream[stream.index("struct xgpu_stream_prism_bake\n"):]
    record = record[:record.index("};")]
    assert "unsigned char world_id[32];" in record
    assert "XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_prism_bake, 80 + XGPU_BAKE_NAME + 32);" in stream
    assert "prism_world_source_world_id((short)bake->bsp_index, bake->world_id);" in _source("prism_bake.c")
    baker = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c").read_text()
    assert re.search(r"memcmp\(bake\.control\.world_id, zero, sizeof\(zero\)\)\)\s*\n\s*\{\s*\n\s*b->key\.geometry_hash = "
                     r"prism_bake_hash64\(b->key\.geometry_hash, bake\.control\.world_id, 32\);\s*\n\s*"
                     r"memcpy\(b->key\.world_id, bake\.control\.world_id, 32\);\s*\n\s*"
                     r"b->key\.scene_version = XGPU_BAKE_SCENE_VERSION;\s*\n\s*b->key\.map_checksum = 0;", baker)
    cache = (ROOT / "port/linux/vulkan/prism_bake_cache.c").read_text()
    assert "memcmp(a->world_id, b->world_id, 32)" in cache and "HEADER_WORLD_ID" in cache
    # (the classic world's ids, from the map's classic_world_id)
    source = _source("prism_world_source.c")
    assert "prism_world_id_classic(world_source.classic_world_id" in source
    assert 'getenv("HALO_CLASSIC_WORLD_ID")' in source
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


def test_players_never_bake():
    """the bake lock (M4, PLAN_FINAL section 3.5): a bake is wanted only where the farm's HALO_BAKE_FARM=1 is set
    (and never in a PRISM_BAKER_COMPILED_OUT build), and only then are a reach's file and a bake's folder written;
    the foliage-ready layers' records are light the shipped bake may carry"""
    baker = re.sub(r"/\*.*?\*/", "", (ROOT / "port/linux/vulkan/xgpu_vulkan_bake.c").read_text(), flags=re.S)
    assert re.search(r"wanted = \(bake\.control\.mode == XGPU_BAKE_MODE_AUTO \|\| bake\.control\.request != 0\) && "
                     r"!in_play && bake_allowed\(\);", baker)
    farm = baker[baker.index("static int bake_farm(void)\n{"):]
    farm = farm[:farm.index("\n}\n")]
    assert "#ifdef PRISM_BAKER_COMPILED_OUT" in farm and 'getenv("HALO_BAKE_FARM")' in farm
    assert "farm[0] == '1' && farm[1] == 0" in farm
    write = baker[baker.index("static void reach_file_write(uint64_t hash)"):]
    write = write[:write.index("memcpy(head, \"PRCH\", 4);")]
    assert "!bake_farm()" in write
    assert re.search(r"if \(bake_farm\(\)\)\s*\n\s*folder_make\(folder\);", baker)
    assert re.search(r"if \(bake_farm\(\)\)\s*\n\s*folder_make\(directory\);", baker)
    assert baker.count("folder_make(") == 3
    import prism_bake_ship as ship
    assert {"MSKF", "VOLF", "FCLS"} <= set(ship.LIGHT_RECORDS.values())


def _code(path):
    return re.sub(r"/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)


def _function(code, head):
    body = code[code.index(head):]
    return body[:body.index("\n}\n")]


def test_own_page_draws_never_read_the_stock_lightmap():
    """a remodelled material on the enhanced world's own lightmap page (LMUV v2) is never lit by CE's lightmap passes
    from the BSP's stock bitmap read at its coordinates (b30's orange, red and yellow beach): the lightmap pass draws it
    with the fullbright combiner and a neutral grey, the specular lightmap and reflection mask passes skip it; a stock
    material, or a remodel on CE's own coordinates, is drawn as CE draws it"""
    geometry = _code("port/linux/game/prism_world_geometry.c")
    own = _function(geometry, "int prism_world_geometry_own_page_draw(")
    assert "world_geometry.current" in own
    assert "vertex_buffer == made->buffers" in own and "made->page != NONE" in own
    assert "int prism_world_geometry_own_page_draw(struct vertex_buffer const *vertex_buffer);" in \
        (ROOT / "port/linux/game/prism_world_geometry.h").read_text()
    environment = _code("source/rasterizer/xbox/rasterizer_xbox_environment.c")
    assert '#include "prism_world_geometry.h"' in environment
    lightmap = _function(environment, "void _rasterizer_environment_lightmap_draw(")
    branch = lightmap[lightmap.index("else if (prism_world_geometry_own_page_draw(vertex_buffer))"):]
    branch = branch[:branch.index("switch (rasterizer_debug_options.drawing_mode)")]
    assert "PRISM_OWN_PAGE_NEUTRAL_LIGHT" in branch
    assert "pixel_shader.final_combiner_constant_1 = real_rgb_color_to_pixel32(&neutral);" in branch
    assert "pixel_shader.final_combiner_inputs_abcd = 0x2002000C;" in branch
    # (after the debug lightmap modes, which keep theirs, and before the draw)
    assert lightmap.index("_rasterizer_lightmap_mode_fullbright") < lightmap.index("prism_world_geometry_own_page_draw")
    assert lightmap.index("prism_world_geometry_own_page_draw") < lightmap.index(
        "rasterizer_draw_dynamic_triangles_static_vertices2(\n\t\t\tdynamic_triangle_buffer_index")
    for head in ("void _rasterizer_environment_specular_lightmap_draw(",
                 "void _rasterizer_environment_reflection_lightmap_mask_draw("):
        body = _function(environment, head)
        gate = body[:body.index("{", body.index("if (rasterizer_debug_options.drawing_mode"))]
        assert "!prism_world_geometry_own_page_draw(vertex_buffer)" in gate, head
    match = re.search(r"#define PRISM_OWN_PAGE_NEUTRAL_LIGHT ([0-9.]+)f", (ROOT / "source/rasterizer/xbox/"
                      "rasterizer_xbox_environment.c").read_text())
    assert match and 0.2 <= float(match.group(1)) <= 1.0
