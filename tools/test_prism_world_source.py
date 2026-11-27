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
    every BSP's enhanced pack (or the farm's unbaked override, or Path Traced, which reads no lightmap of it); rung 2
    otherwise: geometry and light never disagree"""
    table = tool.ladder()
    assert len(table) == 64
    for (classic, geometry, sidecar, packs, unbaked, path_traced), rung in table.items():
        if classic:
            expected = 3
        elif geometry and sidecar and (packs or unbaked or path_traced):
            expected = 1
        else:
            expected = 2
        assert rung == expected, (classic, geometry, sidecar, packs, unbaked, path_traced, rung)


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
    # (the sidecar opened only by lit_choose, which choose calls only under Enhanced or Path Traced, and held until
    # the map's tags go)
    assert "prsx_open(" in _function(source, "static void lit_choose(char const *map)")
    choose = _function(source, "static void choose(void)")
    assert choose.index("prism_world_ladder(lighting_classic()") < choose.index("lit_choose(map);")
    assert "if (world_source.real && !world_source.lit_chosen && !lighting_classic() &&\n\t\t" \
        "(world_source.loaded_lit || prism_deferred_pathtraced()))\n\t\tlit_choose(map);" in choose
    assert "prism_world_source_map_unloaded();" in code


def test_classic_lighting_never_draws_it():
    """the enhanced geometry is drawn only where the ladder, asked every frame of the map's state and the frame's
    lighting, is rung 1 (a change to Classic in a map draws the map's own at once)"""
    source = re.sub(r"/\*.*?\*/", "", _source("prism_world_source.c"), flags=re.S)
    body = _function(source, "boolean prism_world_enhanced(void)")
    assert "return prism_world_rung() == PRISM_WORLD_RUNG_ENHANCED;" in body
    rung = _function(source, "enum prism_world_rung prism_world_rung(void)")
    # (and, each frame, its packs, the farm's override or Path Traced: Enhanced without the packs draws classic)
    assert re.search(r"prism_world_ladder\(lighting_classic\(\), geometry,\s*"
                     r"world_source\.sidecar_ok && world_source\.open && !world_source\.refused, world_source\.packs_ok,"
                     r"\s*world_source\.unbaked, prism_deferred_pathtraced\(\)\)", rung)
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


def test_the_ladder_reads_the_shipped_packs_where_the_renderer_does():
    """the ladder's packs and the map's classic_world_id are read from the shipped folder by its host path, as the
    renderer that loads them and the menu do (an absolute display.bake_shipped_folder was read under the data folder by
    port/linux/include/stdio.h's fopen: the packs never found); the sidecar stays a data root file"""
    source = _source("prism_world_source.c")
    undef = source.index("#undef fopen")
    assert source.index("static FILE *data_file_open(") < undef
    assert undef < source.index("static boolean packs_present(")
    assert undef < source.index("static boolean classic_world_id_find(")
    code = _code("port/linux/game/prism_world_source.c")
    assert "data_file_open(path, \"rb\")" in _function(code, "static boolean sidecar_path(")
    assert "world_source.file = data_file_open(world_source.path, \"rb\");" in code
    packs = _function(code, "static boolean packs_present(")
    assert '"%s/prism_bake_shipped/remodel/%s", platform_data_root(), map' in packs
    shipped = _function(code, "static void shipped_folder(")
    assert '"%s/prism_bake_shipped", platform_data_root()' in shipped
    for name in ("prism_bake.c", "prism_bake_menu.c"):
        assert '"%s/prism_bake_shipped", platform_data_root()' in _source(name), name


def test_a_pack_the_renderer_does_not_load_drops_the_map_to_rung_2():
    """a pack the ladder found (its manifest names the BSP's enhanced world_id) that the renderer does not load whole
    (its own key another geometry's, a damaged file): a player bakes nothing, so the BSP's bake is paused there, or it
    failed; the map is then drawn with its classic geometry for the rest of it, with a warning, never enhanced geometry
    without its light under Enhanced. Never under debug.world_unbaked, whose rung 1 has no packs by design, nor under
    Path Traced, which needs none"""
    bake = _code("port/linux/game/prism_bake.c")
    frame = _function(bake, "void prism_bake_frame(")
    check = frame[frame.index("if (prism_world_enhanced() && !prism_deferred_pathtraced())"):
                  frame.index("if (prism_bake.exit_when_done)")]
    assert "XGPU_BAKE_STATE_PAUSED" in check and "XGPU_BAKE_STATE_FAILED" in check
    assert "XGPU_BAKE_STATUS_BSP) == (long)global_structure_bsp_index_get()" in check
    assert "XGPU_BAKE_STATUS_GENERATION) == (unsigned long)prism_scene_static_generation()" in check
    assert "prism_world_source_bake_refused(global_structure_bsp_index_get());" in check
    assert frame.index("xgpu_backend_prism_bake(&bake, NULL, 0);") < frame.index("prism_world_source_bake_refused(")
    source = _code("port/linux/game/prism_world_source.c")
    refused = _function(source, "void prism_world_source_bake_refused(")
    assert ("if (!prism_world_enhanced() || !world_source.packs_ok || world_source.unbaked || "
            "prism_deferred_pathtraced() ||\n\t\t!bsp_has_geometry(bsp_index))") in refused
    assert "warning" in refused and "rung 2" in refused
    assert "world_source.packs_ok = FALSE;" in refused
    # (the packs refused: under Enhanced the geometry is drawn classic at once, and the bake keyed to the classic
    # world; Path Traced, which needs no pack, keeps the enhanced geometry)
    rung = _function(source, "enum prism_world_rung prism_world_rung(void)")
    assert "world_source.packs_ok," in rung
    assert "if (prism_world_enhanced() && !prism_deferred_pathtraced())" in frame
    world_id = _function(source, "void prism_world_source_world_id(")
    assert world_id.index("prism_world_enhanced()") < world_id.index("world_source.classic_ids")
    # (the host's state words this reads: a player's run never bakes, so a BSP without a whole bake is paused)
    baker = _code("port/linux/vulkan/xgpu_vulkan_bake.c")
    assert re.search(r"if \(!wanted && !b->complete && b->step > STEP_LOAD && b->step < STEP_COMMIT\)\s*\n\s*\{\s*\n\s*"
                     r"status_write\(b\);\s*\n\s*status_set\(XGPU_BAKE_STATUS_STATE, XGPU_BAKE_STATE_PAUSED\);", baker)


def test_path_traced_takes_the_enhanced_geometry_without_its_packs():
    """Path Traced reads no lightmap of the enhanced world: it traces the bake's scene (made for the BSP whether or not
    a pack is loaded: a player's paused bake), lit by the bake's light where it has it and its own bounce else; only
    until that scene is made does it show CE's frame, whose lightmap pass draws the enhanced world flat. So the map's
    choice passes the lighting to the ladder, and a map chosen under Path Traced without the packs is rung 1 (logged)"""
    pt = _code("port/linux/vulkan/xgpu_vulkan_pt.c")
    assert re.search(r"if \(!xgpu_vulkan_bake_lookup\(&lookup\)\)\s*\n\s*\{[^}]*CE's frame until it is made[^}]*"
                     r"return NULL;", pt)
    baker = _code("port/linux/vulkan/xgpu_vulkan_bake.c")
    lookup = _function(baker, "int xgpu_vulkan_bake_lookup(")
    assert "if (!bake.made || !bake_traceable(b) || !b->block.tlas)" in lookup
    identity = _code("port/linux/game/prism_identity.c")
    ladder = _function(identity, "int prism_world_ladder(")
    assert "if (geometry_enhanced && sidecar_ok && (packs_ok || unbaked || path_traced))" in ladder
    source = _code("port/linux/game/prism_world_source.c")
    assert "Path Traced draws it (rung 1), traced as it is" in source


def test_a_bsp_without_enhanced_geometry_asks_for_no_enhanced_pack():
    # (d40's space BSP: no GEOM in the sidecar, drawn classic, shipped empty by the farm; the
    # map's other BSPs still reach rung 1, and that BSP stays keyed to its classic world)
    source = _code("port/linux/game/prism_world_source.c")
    has = _function(source, "static boolean bsp_has_geometry(short bsp)")
    assert "prsx_find(&world_source.prsx, PRSX_GEOM, (uint16_t)bsp, 0) >= 0" in has
    present = _function(source, "static boolean packs_present(")
    assert present.index("if (!bsp_has_geometry(bsp))") < present.index('"%s/bsp%d.pbk"')
    world_id = _function(source, "void prism_world_source_world_id(")
    assert "if (prism_world_enhanced() && bsp_has_geometry(bsp_index))" in world_id
    assert "world_source.classic_ids[bsp_index]" in world_id


def test_path_traced_draws_the_selected_geometry_whatever_the_bakes(tool):
    """Path Traced's geometry and lighting never depend on a bake: with the sidecar verified, World Geometry Enhanced
    is rung 1 under Path Traced whatever the packs and debug.world_unbaked say, and World Geometry Classic (or no
    sidecar) rung 2; Enhanced and Classic keep their rules"""
    table = tool.ladder()
    for geometry in (0, 1):
        for sidecar in (0, 1):
            traced = {table[(0, geometry, sidecar, packs, unbaked, 1)] for packs in (0, 1) for unbaked in (0, 1)}
            assert traced == {1 if geometry and sidecar else 2}, (geometry, sidecar, traced)
    assert table[(0, 1, 1, 0, 0, 0)] == 2 and table[(0, 1, 1, 1, 0, 0)] == 1
    assert {rung for key, rung in table.items() if key[0]} == {3}


def test_the_ladder_is_asked_each_frame_of_a_state_chosen_once():
    """a map's state is chosen once (World Geometry as it loads, the sidecar held whatever the packs), its lighting
    read each frame: a map loaded under Enhanced without its packs keeps the sidecar, so a change to Path Traced in it
    draws the enhanced geometry; a map loaded under Classic chooses the rest at its first change from Classic; a pack
    the renderer refused under Enhanced leaves Path Traced's geometry alone"""
    source = _code("port/linux/game/prism_world_source.c")
    lit = _function(source, "static void lit_choose(char const *map)")
    # (no rung decided here, and the sidecar never closed for the packs' sake)
    assert "prism_world_ladder(" not in lit
    after = lit[lit.index("world_source.packs_ok = packs_present("):]
    assert "sidecar_close()" not in after and "world_source.sidecar_ok = TRUE;" in lit
    assert lit.index("world_source.sidecar_ok = TRUE;") < lit.index("world_source.packs_ok = packs_present(")
    assert "if (!world_source.geometry_wanted)" in lit and "prism_world_geometry_wanted()" not in lit
    choose = _function(source, "static void choose(void)")
    assert "world_source.geometry_wanted = prism_world_geometry_wanted();" in choose
    assert choose.index("world_source.geometry_wanted =") < choose.index("prism_world_ladder(lighting_classic()")
    # (the state forgotten with the map's tags)
    unloaded = _function(source, "void prism_world_source_map_unloaded(void)")
    for field in ("chosen", "real", "lit_chosen", "sidecar_ok", "refused"):
        assert "world_source.%s = FALSE;" % field in unloaded, field
    assert "sidecar_close();" in unloaded
    # (what is drawn logged as it changes: the runs' record of each switch)
    rung = _function(source, "enum prism_world_rung prism_world_rung(void)")
    assert "drawn now: %s geometry, %s lighting (rung %d)" in rung
    # (the classic world's bake key only once its classic_world_id has been looked for)
    world_id = _function(source, "void prism_world_source_world_id(")
    assert "world_source.classic_known && world_source.lit_chosen" in world_id


def test_path_traced_never_traces_another_geometrys_bake():
    """a set begun again in play (the geometry swapped by a change of lighting) under Path Traced with no whole bake
    of its key makes its own scene, nothing baked, never keeps the bake drawn before it (another geometry's scene,
    which the path tracer would trace under the new geometry); a hold made under another lighting is let go; under
    Enhanced, as it was"""
    stream = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
    assert "XGPU_BAKE_FLAG_PATH_TRACED = 32," in stream
    bake = _code("port/linux/game/prism_bake.c")
    fill = _function(bake, "static void control_fill(")
    assert "(prism_deferred_pathtraced() ? XGPU_BAKE_FLAG_PATH_TRACED : 0)" in fill
    baker = _code("port/linux/vulkan/xgpu_vulkan_bake.c")
    begin = _function(baker, "static struct bake_state *bake_begin(")
    branch = begin[begin.index("if (in_play)"):begin.index("b->started = now_seconds();")]
    assert branch.index("if (!b->chosen && (bake.control.flags & XGPU_BAKE_FLAG_PATH_TRACED))") < \
        branch.index("else if (!b->chosen)") < branch.index("*held = 1;")
    pt_part = branch[branch.index("XGPU_BAKE_FLAG_PATH_TRACED"):branch.index("else if (!b->chosen)")]
    assert "return NULL" not in pt_part and "bake_free" not in pt_part
    frame = _function(baker, "static void bake_frame(void)")
    assert "path_traced = (bake.control.flags & XGPU_BAKE_FLAG_PATH_TRACED) != 0;" in frame
    assert re.search(r"if \(path_traced && bake\.held_generation\)\s*\n\s*bake\.held_generation = 0;", frame)
    # (still nothing baked in play)
    assert "&& !in_play && bake_allowed();" in frame
    # (and the path tracer never traces a bake of another set than the game's now: while the set's own is begun, the
    # bake drawn before it is kept, and its scene is another geometry's)
    lookup = _function(baker, "int xgpu_vulkan_bake_lookup(")
    assert re.search(r"if \(b->generation != bake\.control\.generation\)\s*\n\s*return 0;", lookup)
    assert lookup.index("bake_traceable(b)") < lookup.index("b->generation != bake.control.generation")


def test_path_traced_history_never_crosses_a_new_scene():
    """the path tracer's history and the denoiser plugin's are dropped where the bake's scene is another (a new set's
    generation, the world's geometry changed in play), not only where its address differs"""
    pt = _code("port/linux/vulkan/xgpu_vulkan_pt.c")
    assert re.search(r"scene == pt\.scene_seen &&\s*lookup\.generation == pt\.generation_seen &&", pt)
    assert "pt.generation_seen = lookup.generation;" in pt
    assert "pt.history_kept = (flags & FLAG_HISTORY) != 0;" in pt
    upscale = _code("port/linux/vulkan/xgpu_vulkan_upscale.c")
    assert "plugin_upscale(PLUGIN_DENOISER, commands, record, reset || !xgpu_vulkan_pt_history_kept()," in upscale


def test_a_map_loaded_under_classic_keeps_its_enhanced_behaviour():
    """a map loaded under Classic and changed to Enhanced draws its classic geometry with its bakes keyed by its
    checksum, as it always has (Enhanced and Classic keep their rules); only Path Traced, whose geometry never depends
    on a bake, draws the enhanced geometry in it, and the rest of its choice is made at its first Path Traced frame"""
    source = _code("port/linux/game/prism_world_source.c")
    choose = _function(source, "static void choose(void)")
    assert choose.index("world_source.loaded_lit = FALSE;") < choose.index("prism_world_ladder(lighting_classic()")
    assert choose.index("prism_world_ladder(lighting_classic()") < choose.index("world_source.loaded_lit = TRUE;")
    rung = _function(source, "enum prism_world_rung prism_world_rung(void)")
    assert "geometry = world_source.geometry_wanted && (world_source.loaded_lit || prism_deferred_pathtraced());" in rung
    world_id = _function(source, "void prism_world_source_world_id(")
    assert "world_source.classic_known && world_source.lit_chosen && world_source.loaded_lit" in world_id
    unloaded = _function(source, "void prism_world_source_map_unloaded(void)")
    assert "world_source.loaded_lit = FALSE;" in unloaded
