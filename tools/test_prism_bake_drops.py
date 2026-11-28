"""Enhanced lighting's drops to Classic and its light's reload at a BSP switch (Adam's play test of the 58449a4a9 kit):
the renderer's whole bakes and the game's verdict on them, read from the source (port/linux/vulkan/xgpu_vulkan_bake.c,
port/linux/game/prism_bake.c), and the bake cache's open-ahead run through its own C:

- a whole bake (the shipped one, or this machine's) makes on the card only what the frame draws: none of the
  baking's buffers, no Path Traced indirect pool (nothing has read it since Path Traced took no baked light), its
  masks as many as its file has; so b30's enhanced first BSP leaves the card the 1.6 GB its volume found no room in;
- a whole bake that failed after its light was drawn does not drop the map to its classic geometry;
- the reach's seeds are the level's own firing positions, never MENDICANT AI's maneuver slots;
- a BSP's whole bake is kept on the card when the game draws another BSP, and drawn again at once when the game draws
  its BSP again, its key checked once the set is in;
- a whole bake is read and decoded by a thread of its own, its volume before its tiles;
- the map's other BSPs' shipped bakes are opened ahead, and an opened file's key read the same as a file opened with
  the key.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
VULKAN = ROOT / "port/linux/vulkan"
GAME = ROOT / "port/linux/game"
CC = shutil.which("cc") or shutil.which("clang")


def _code(path):
    return re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)


def _function(code, name):
    """the body of the C function named, from its definition to the closing brace at the line's start"""
    match = re.search(r"\n[a-z][^\n;]*\b" + name + r"\([^;{]*\)\n\{\n(.*?)\n\}\n", code, re.S)
    assert match, name
    return match.group(1)


BAKE = _code(VULKAN / "xgpu_vulkan_bake.c")


def test_a_whole_bake_makes_none_of_the_baking_buffers():
    upload = _function(BAKE, "bake_upload")
    assert "b->bakes = !b->chosen && bake_allowed();" in upload
    # (the frame's tables first, then the baking's alone where it may bake)
    early, late = upload.split("if (!b->bakes)", 1)
    for drawn in ("&b->pages", "&b->charts", "&b->vpages", "&b->table", "&b->sun", "&b->pool", "&b->masks",
                  "&b->raster_job"):
        assert drawn in early, drawn
    for baking in ("&b->triangles", "&b->ranges", "&b->plan_atlas", "&b->cache_block", "&b->texels",
                   "&b->accumulators", "&b->states", "&b->running", "&b->tile_origins", "&b->compact_list",
                   "&b->mask_ranges", "&b->mask_out"):
        assert baking not in early and baking in late, baking
    # (a whole bake's masks: its file's count, not a quarter of the tiles)
    assert "PRISM_BAKE_RECORD_SUN_MASK_TILE" in early
    # (Path Traced's indirect pool and lightmap coordinates are gone: nothing read them)
    assert "pool_indirect" not in BAKE and "b->lightmaps" not in BAKE
    volume = _function(BAKE, "volume_make")
    assert re.search(r"if \(b->bakes\)\s*b->probe_host", volume)
    assert re.search(r"b->bakes && \(!b->probe_host \|\|\s*!buffer_filled\(commands, plan->probes", volume)


def test_a_whole_bake_never_bakes_what_its_file_lacks():
    frame = _function(BAKE, "bake_frame")
    lightmap = frame.split("case STEP_LIGHTMAP:", 1)[1].split("case STEP_MASK:", 1)[0]
    assert lightmap.lstrip().startswith("if (!b->bakes)")
    assert "b->grid_ready || !b->bakes ? STEP_LIGHTMAP : STEP_IMPORTANCE" in frame
    assert "(!b->bakes && b->volume_loaded)" in frame and "(!b->bakes && b->reflections_loaded)" in frame
    assert "else if (b->bakes)\n\t\t\treflections_frame(commands, b);" in frame
    mask = _function(BAKE, "mask_frame")
    assert mask.index("if (!b->bakes)") < mask.index("upload(commands, &b->tile_origins")
    volume = _function(BAKE, "volume_frame")
    assert volume.index("if (!b->bakes)") < volume.index("b->volume.batched > b->volume.done")


def test_a_failed_bake_whose_light_was_drawn_keeps_the_enhanced_geometry():
    game = _code(GAME / "prism_bake.c")
    frame = _function(game, "prism_bake_frame")
    verdict = frame.split("prism_world_source_bake_refused", 1)[0].rsplit("if (prism_world_source_bsp_enhanced", 1)[1]
    assert "state == XGPU_BAKE_STATE_PAUSED" in verdict
    assert re.search(r"state == XGPU_BAKE_STATE_FAILED &&\s*prism_bake_status\(XGPU_BAKE_STATUS_SHOWN\) == "
                     r"XGPU_BAKE_SHOWN_NONE", verdict)
    # (the renderer says a failed bake's light was drawn: its tiles)
    status = _function(BAKE, "status_write")
    assert "b->tiles_done ?\n\t\tXGPU_BAKE_SHOWN_PARTIAL : XGPU_BAKE_SHOWN_NONE" in status


def test_the_reach_seeds_are_the_levels_own_firing_positions():
    game = _code(GAME / "prism_bake.c")
    seeds = _function(game, "seeds_find")
    firing = seeds.split("block_of(encounter, 0x98, 0x18, 4096, &entries);", 1)[1]
    assert firing.lstrip().startswith("entries = mendicant_encounter_own_firing_position_count(index, entries);")
    move = _code(GAME / "mendicant/mendicant_move.c")
    own = _function(move, "mendicant_encounter_own_firing_position_count")
    assert "maneuver_original[index]" in own and "return count;" in own
    # (its slots follow the level's own positions in the longer list)
    extend = _function(move, "maneuver_extend")
    assert "memcpy(block, positions, count * sizeof(struct firing_position_definition));" in extend
    assert "maneuver_original[index] = count;" in extend


def test_a_bsps_whole_bake_is_kept_and_drawn_again_at_once():
    frame = _function(BAKE, "bake_frame")
    let_go = frame.split("int rebake = ", 1)[1].split("if (!b)\n", 1)[0]
    assert "bake_keep(b);" in let_go and "if (rebake || b->request != bake.control.request)\n\t\t\tbake_free(b);" in let_go
    begin = frame.split("if (!b)\n", 1)[1]
    assert begin.index("kept_take()") < begin.index("bake_begin(in_play, NULL, &held)")
    assert "b->provisional = 1;" in begin and "b->generation = bake.control.generation;" in begin
    # (a kept bake drawn: its key checked against the set's once it is in, never drawn for another key)
    check = frame.split("if (b && b->provisional", 1)[1].split("goto keep;", 1)[0]
    assert "next = bake_begin(in_play, &b->key, &held);" in check
    assert "b->provisional = 0;" in check and "bake_free(b);\n\t\t\tbake.bake = b = next;" in check
    assert frame.index("if (b && b->provisional") < frame.index("int rebake = ")
    keep = _function(BAKE, "bake_keep")
    assert "!b->complete || b->step <= STEP_LOAD || b->step == STEP_FAILED || b->load_state == 1 || b->writing" in keep
    # (its host copies and the file's index let go of only once its steps are done)
    assert keep.index("if (b->step != STEP_DONE)\n\t\tgoto kept;") < keep.index("prism_bake_cache_close(&b->cache);")
    assert "budget / 3" in keep and "budget / 4 * 3" in keep
    take = _function(BAKE, "kept_take")
    for match in ("b->bsp != bake.control.bsp_index", "b->request != bake.control.request", "bake_world_changed(b)",
                  "PRISM_BAKE_QUALITY_MAX"):
        assert match in take, match
    assert 'kept_drop(index, "another map")' in take
    # (bake_begin compares the bake drawn before it opens any file)
    begun = _function(BAKE, "bake_begin")
    assert begun.index("if (drawn)") < begun.index("file_choose(b);")
    assert "better.settings_hash = key_settings_hash(b, better.quality);" in begun
    # (the room a new bake takes made first; every kept bake let go of at the end)
    assert 'kept_room(' in frame.split("case STEP_UPLOAD:", 1)[1].split("bake_upload(b)", 1)[0]
    assert "bake_free(bake.kept[--bake.kept_count]);" in _function(BAKE, "xgpu_vulkan_bake_shutdown")


def test_a_whole_bake_is_loaded_off_the_render_thread_volume_first():
    loader = _function(BAKE, "load_main")
    assert loader.index("PRISM_BAKE_RECORD_VOLUME_BRICK") < loader.index("PRISM_BAKE_RECORD_ATLAS_TILE")
    assert "&b->load_weights" in loader and "&b->weights" not in loader
    # (each thread reads through its own file handle: part 0 the volume, the others a share of the tiles each)
    assert "&b->cache" not in loader and "struct prism_bake_cache *reader = &worker->reader;" in loader
    assert "worker->part == 0 && b->probes.buffer" in loader
    assert "index += (uint32_t)worker->parts - 1" in loader
    # (nothing of the GPU on the loader's thread: it decodes, the frame draws)
    for gpu in ("upload(", "tile_draw(", "commands"):
        assert gpu not in loader, gpu
    drain = _function(BAKE, "load_drain")
    assert "tile_draw(commands, b, result->index" in drain and "LOAD_FRAME_BYTES" in drain
    assert "prism_bake_batches_skip(&b->volume, result->index + BRICK_PROBES);" in drain
    start = _function(BAKE, "load_start")
    assert "!b->chosen" in start and "volume_make(commands, b)" in start
    assert "worker->reader = b->cache;\n\t\tworker->reader.file = fopen(b->path, \"rb\");" in start
    assert "ended = ++b->load_ended >= b->load_worker_count;" in drain
    stop = _function(BAKE, "load_stop")
    assert "fclose(b->load_workers[index].reader.file);" in stop and "prism_bake_cache_close" not in stop
    frame = _function(BAKE, "bake_frame")
    load = frame.split("case STEP_LOAD:", 1)[1].split("case STEP_IMPORTANCE:", 1)[0]
    assert load.index("file_load_grid(commands, b);") < load.index("load_start(commands, b)")
    assert "b->load_state == 1 ? load_drain(commands, b) : file_load(commands, b)" in load
    assert "b->load_state == 1)\n\t\treturn;" in _function(BAKE, "layers_frame")
    assert "load_stop(b);" in _function(BAKE, "bake_free")


def test_the_maps_shipped_bakes_are_opened_ahead():
    whole = _function(BAKE, "file_whole")
    assert whole.index("prefetch_take(path, &b->cache)") < whole.index("prism_bake_cache_open(&b->cache, path")
    assert "prism_bake_key_compare(&b->cache.key, &b->key)" in whole
    prefetch = _function(BAKE, "prefetch_frame")
    assert "bake.control.bsp_index + side * distance" in prefetch
    assert "for (distance = 1;" in prefetch
    assert "prefetch_frame();" in _function(BAKE, "bake_frame")
    assert "prefetch_stop();" in _function(BAKE, "xgpu_vulkan_bake_shutdown")


OPEN_AHEAD = r"""
#include <stdio.h>
#include <string.h>
#include "prism_bake_cache.h"

int main(int argc, char **argv)
{
	static const struct prism_bake_cache_key none;
	struct prism_bake_cache_key key, other;
	struct prism_bake_cache_writer writer;
	struct prism_bake_cache ahead, direct;
	char path[4096];
	unsigned char payload[64];
	int index;

	snprintf(path, sizeof(path), "%s/bsp0.pbk", argc > 1 ? argv[1] : ".");
	memset(&key, 0, sizeof(key));
	strcpy(key.map_name, "b40");
	strcpy(key.bsp_name, "bsp0");
	key.bsp_hash = 0x1234;
	key.quality = 3;
	key.baker_version = 20;
	if (prism_bake_cache_create(&writer, path, &key, 0))
		return 1;
	for (index = 0; index < 100; index++)
	{
		memset(payload, index, sizeof(payload));
		if (prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(0, 0,
			(unsigned)index, 0), payload, sizeof(payload)))
			return 2;
	}
	if (prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_DONE, PRISM_BAKE_JOB_NONE, NULL, 0) ||
		prism_bake_cache_writer_close(&writer))
		return 3;
	/* (opened ahead with no key, as prefetch_main opens it, then compared as file_whole compares it) */
	if (prism_bake_cache_open(&ahead, path, &none, 0) != PRISM_BAKE_CACHE_STALE)
		return 4;
	if (prism_bake_cache_open(&direct, path, &key, 0) != PRISM_BAKE_CACHE_CURRENT)
		return 5;
	if (prism_bake_key_compare(&ahead.key, &key) != 0 || ahead.record_count != direct.record_count ||
		ahead.done != direct.done || !ahead.done)
		return 6;
	other = key;
	other.bsp_hash = 0x4321;
	if (prism_bake_key_compare(&ahead.key, &other) != direct.differences + PRISM_BAKE_KEY_BSP)
		return 7;
	/* (its records read the same) */
	{
		unsigned char a[64], d[64];

		if (prism_bake_cache_read(&ahead, prism_bake_cache_find(&ahead, PRISM_BAKE_RECORD_ATLAS_TILE,
			prism_bake_job_lightmap_tile(0, 0, 57, 0)), a, sizeof(a)) ||
			prism_bake_cache_read(&direct, prism_bake_cache_find(&direct, PRISM_BAKE_RECORD_ATLAS_TILE,
			prism_bake_job_lightmap_tile(0, 0, 57, 0)), d, sizeof(d)) || memcmp(a, d, sizeof(a)) || a[0] != 57)
			return 8;
	}
	prism_bake_cache_close(&ahead);
	prism_bake_cache_close(&direct);
	puts("PASSED");
	return 0;
}
"""


@pytest.mark.skipif(not CC, reason="no C compiler")
def test_a_file_opened_ahead_reads_as_one_opened_with_its_key(tmp_path):
    main = tmp_path / "open_ahead.c"
    main.write_text(OPEN_AHEAD)
    exe = tmp_path / "open_ahead"
    sources = sorted(str(p) for p in VULKAN.glob("prism_bake_*.c"))
    subprocess.run([CC, "-std=gnu89", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    f"-I{VULKAN}", "-o", str(exe), str(main)] + sources + ["-lm"], check=True)
    result = subprocess.run([str(exe), str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 0 and "PASSED" in result.stdout, (result.returncode, result.stdout, result.stderr)


def test_the_frames_light_is_logged_on_request():
    note = _function(BAKE, "xgpu_vulkan_bake_raster_note")
    assert 'getenv("HALO_RASTER_FRAME_LOG")' in note
    raster = _function(BAKE, "xgpu_vulkan_bake_raster")
    assert "xgpu_vulkan_bake_raster_note(copy && base && additions && words && extras && view);" in raster
    stream = _code(VULKAN / "xgpu_vulkan_stream.c")
    assert "if (raster && !words && !pathtraced)\n\t\t\t\txgpu_vulkan_bake_raster_note(0);" in stream
