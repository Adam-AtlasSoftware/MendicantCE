/*
RENDER_PREPARED.C

A map's preparation (render_enumerate.c: its first frame drawn, unseen,
through every shader, about a hundred times) recorded the first time it is
made, and played back after, in its place: what the walk gave the renderer
(the front end's journal, port/linux/src/d3d8_device.c: each draw that was
not a repeat, in the state it was drawn in, each window operation a backend
prepares for, each texture loaded, in order, and the geometry's pages
uploaded) is given to it again, without the walk. Every backend is given the
same draws, as it was by the walk: Vulkan and Metal make the same pipelines
and bind the same textures.

A record is kept in z:\prepared (beside the decompressed maps' cache
files), a file for each map (its cache file's checksum and scenario), build
and the settings that change what the walk draws (the lights, the ambient
occlusion, the anti-aliasing, the HDR, Panini and so on, and whether the
frame draws mirrors). The screen's size is not among them: the journal holds
the game's state, which the front end turns into pipelines and targets as
the screen is now. Played back, the map's enumeration (the set of distinct
draws as the front end describes them: xgpu_manifest_enumeration) must be
the record's; a record that is missing, cut short, malformed, for another
build or settings, whose objects cannot all be found again, or whose draws
are not the record's, is deleted and the walk made instead, which records
again. A map is recorded at its first load, the stock maps at the game's
first start (menu_pictures_render.c: the menus' pictures' pass loads them
all). HALO_PREPARED_RECORDS=0 turns the records off (the walk every time).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "cseries/cseries_windows.h"

#include "../src/xgpu_manifest.h"
#include "render_enumerate.h"
#include "render_prepared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* the platform layer's (port/linux/src/port_config.c, xbox_files.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);
int halo_dynamic_lights_enhanced(void);

#define PREPARED_MAGIC 0x50314f48u /* "HO1P" */
/* the journal's form and this file's: a change makes every record again */
#define PREPARED_VERSION 1
/* the most words a record may hold */
#define PREPARED_MAXIMUM_WORDS (64UL * 1024 * 1024)

struct prepared_header
{
	uint32_t magic, version, header_size, checksum;
	uint64_t key;
	/* the map's enumeration (its draws' hashes, in order: prepared_draws),
	then the journal's words, and the hash of both */
	uint32_t word_count;
	uint32_t draw_count;
	uint64_t words_hash;
};

/* the settings that change what a map's preparation draws or makes */
static char const *const prepared_settings[] =
{
	"display.ambient_occlusion", "display.anisotropic_filtering", "display.anti_aliasing", "display.astc_textures",
	"display.dynamic_lights", "display.graphics", "display.hdr", "display.hdr_mode", "display.high_res_hud",
	"display.high_res_text", "display.model_detail", "display.overshield_effect", "display.panini",
	"display.per_pixel_lighting", "display.shadow_resolution", "display.shield_glow", "display.structure_culling",
	"display.first_person_legs", "display.viewmodel_shield", "display.viewmodel_visible",
};

static struct
{
	boolean recording;
	char path[64];
	unsigned long checksum;
	uint64_t key;
} prepared;

static struct halo_gpu_journal_bitmaps const prepared_bitmaps =
{
	texture_cache_port_identify,
	texture_cache_port_load,
};

static boolean prepared_enabled(void)
{
#ifdef HALO_VR
	/* (the VR mode's eyes and HUD prepare apart: not recorded) */
	return FALSE;
#else
	char const *setting = getenv("HALO_PREPARED_RECORDS");

	return !setting || strcmp(setting, "0") != 0;
#endif
}

static uint64_t prepared_hash_string(char const *text, uint64_t hash)
{
	return xgpu_manifest_hash(text, strlen(text) + 1, hash);
}

/* which build this is: when this file was compiled, and the distances
between functions of many of the game's files (any change of the code
moves them) */
static uint64_t prepared_build_hash(uint64_t hash)
{
	char const *const functions[] =
	{
		(char const *)render_enumerate_begin, (char const *)tag_get_name, (char const *)render_prepared_replay,
		(char const *)texture_cache_port_load, (char const *)texture_cache_idle, (char const *)cache_files_get_checksum,
		(char const *)halo_dynamic_lights_enhanced, (char const *)xgpu_manifest_hash, (char const *)config_string,
	};
	short index;

	hash = prepared_hash_string(__DATE__ " " __TIME__, hash);
	for (index = 1; index < (short)NUMBEROF(functions); index++)
	{
		long distance = (long)(functions[index] - functions[0]);

		hash = xgpu_manifest_hash(&distance, sizeof(distance), hash);
	}
	return hash;
}

/* the record's key and file, for the map loaded now */
static boolean prepared_choose(boolean mirrors)
{
	char const *scenario = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
	uint64_t hash = XGPU_MANIFEST_HASH_START;
	short index;
	unsigned long version = PREPARED_VERSION;

	if (!prepared_enabled())
		return FALSE;
	hash = xgpu_manifest_hash(&version, sizeof(version), hash);
	hash = prepared_hash_string(scenario, hash);
	hash = prepared_build_hash(hash);
	for (index = 0; index < (short)NUMBEROF(prepared_settings); index++)
	{
		char const *value = config_string(prepared_settings[index]);

		hash = prepared_hash_string(prepared_settings[index], hash);
		hash = prepared_hash_string(value ? value : "", hash);
	}
	{
		long enhanced = halo_dynamic_lights_enhanced() ? 1 : 0;
		long mirrored = mirrors ? 1 : 0;
		/* (the structure the map starts in: a saved game's may be another) */
		long structure_bsp_index = global_structure_bsp_index_get();

		hash = xgpu_manifest_hash(&enhanced, sizeof(enhanced), hash);
		hash = xgpu_manifest_hash(&mirrored, sizeof(mirrored), hash);
		hash = xgpu_manifest_hash(&structure_bsp_index, sizeof(structure_bsp_index), hash);
	}
	prepared.checksum = cache_files_get_checksum();
	prepared.key = hash;
	CreateDirectoryA("z:\\prepared", NULL);
	snprintf(prepared.path, sizeof(prepared.path), "z:\\prepared\\%08lx-%08lx%08lx.prepared", prepared.checksum,
		(unsigned long)(hash >> 32), (unsigned long)hash);
	return TRUE;
}

static int prepared_compare(void const *a, void const *b)
{
	uint64_t x = *(uint64_t const *)a, y = *(uint64_t const *)b;

	return x < y ? -1 : x > y;
}

/* the map's enumeration as a set: its draws' hashes, in order (malloc's), and
their count */
static uint64_t *prepared_draws(unsigned long *count)
{
	struct xgpu_manifest const *enumeration = xgpu_manifest_enumeration();
	uint64_t *hashes = malloc((enumeration->draw_count + 1) * sizeof(*hashes));
	unsigned long index;

	*count = 0;
	if (!hashes)
		return NULL;
	for (index = 0; index < enumeration->draw_count; index++)
		hashes[index] = xgpu_manifest_hash(&enumeration->draws[index], sizeof(enumeration->draws[index]),
			XGPU_MANIFEST_HASH_START);
	qsort(hashes, enumeration->draw_count, sizeof(*hashes), prepared_compare);
	*count = enumeration->draw_count;
	return hashes;
}

/* the recorded draws (in order) that the made ones (in order) do not have */
unsigned long render_prepared_draws_missing(unsigned long long const *recorded, unsigned long recorded_count,
	unsigned long long const *made, unsigned long made_count)
{
	unsigned long index, other = 0, missing = 0;

	for (index = 0; index < recorded_count; index++)
	{
		while (other < made_count && made[other] < recorded[index])
			other++;
		if (other == made_count || made[other] != recorded[index])
			missing++;
	}
	return missing;
}

/* a record's header is of this form, map, build and settings (key) */
static boolean prepared_header_fits(struct prepared_header const *header, unsigned long checksum, uint64_t key)
{
	return header->magic == PREPARED_MAGIC && header->version == PREPARED_VERSION &&
		header->header_size == sizeof(*header) && header->checksum == (uint32_t)checksum && header->key == key &&
		header->word_count && header->word_count <= PREPARED_MAXIMUM_WORDS &&
		header->draw_count <= PREPARED_MAXIMUM_WORDS / 2;
}

/* the record (its draws, then its words), read and checked; NULL if there is
none to play */
static uint64_t *prepared_read(struct prepared_header *header)
{
	FILE *file = fopen(prepared.path, "rb");
	uint64_t *words = NULL;
	unsigned long size;
	boolean good = FALSE;

	if (!file)
		return NULL;
	if (fread(header, sizeof(*header), 1, file) == 1 && prepared_header_fits(header, prepared.checksum, prepared.key))
	{
		size = header->word_count * sizeof(unsigned int) + header->draw_count * sizeof(uint64_t);
		words = malloc(size + sizeof(uint64_t));
		good = words && fread(words, 1, size, file) == size &&
			xgpu_manifest_hash(words, size, XGPU_MANIFEST_HASH_START) == header->words_hash;
	}
	fclose(file);
	if (!good)
	{
		platform_log("render: the map's preparation's record is not this map's, build's or settings', or is cut "
			"short: made again");
		free(words);
		DeleteFileA(prepared.path);
		return NULL;
	}
	return words;
}

/* the draws made that the record has not (what tells them apart, a few) */
static void prepared_log_new(uint64_t const *recorded, unsigned long recorded_count)
{
	struct xgpu_manifest const *enumeration = xgpu_manifest_enumeration();
	unsigned long index, logged = 0;

	for (index = 0; index < enumeration->draw_count && logged < 8; index++)
	{
		struct xgpu_manifest_draw const *draw = &enumeration->draws[index];
		uint64_t hash = xgpu_manifest_hash(draw, sizeof(*draw), XGPU_MANIFEST_HASH_START);

		if (!render_prepared_draws_missing(&hash, 1, recorded, recorded_count))
			continue;
		logged++;
		platform_log("render: a draw the record has not: vertex %08lx%08lx immediate %u lighting %u samples %u "
			"colour %u depth %u blend %u/%u/%u cm %x pixel hash %08lx coverage %u threshold %u per-pixel %u lights %u "
			"occlusion %u hdr %x alpha samples %u stages %u.%u %u.%u %u.%u %u.%u", (unsigned long)draw->key.vertex_program[1],
			(unsigned long)draw->key.vertex_program[0], draw->key.immediate, draw->key.lighting, draw->key.samples,
			draw->key.color_format, draw->key.depth_format, draw->key.blend, draw->key.blend_source,
			draw->key.blend_destination, draw->key.color_mask,
			(unsigned long)xgpu_manifest_hash(&draw->key.pixel, sizeof(draw->key.pixel), XGPU_MANIFEST_HASH_START),
			draw->key.pixel.coverage_alpha, draw->key.pixel.point_threshold, draw->key.pixel.per_pixel_lighting,
			draw->key.pixel.dynamic_lights, draw->key.pixel.ambient_occlusion, draw->key.pixel.hdr,
			draw->key.pixel.alpha_test_samples, draw->stages[0].source, draw->stages[0].sampler_type,
			draw->stages[1].source, draw->stages[1].sampler_type, draw->stages[2].source, draw->stages[2].sampler_type,
			draw->stages[3].source, draw->stages[3].sampler_type);
	}
}

boolean render_prepared_recorded(boolean mirrors)
{
	struct prepared_header header;
	FILE *file;
	boolean fits = FALSE;

	if (!prepared_choose(mirrors))
		return TRUE;
	file = fopen(prepared.path, "rb");
	if (file)
	{
		fits = fread(&header, sizeof(header), 1, file) == 1 &&
			prepared_header_fits(&header, prepared.checksum, prepared.key);
		fclose(file);
	}
	return fits;
}

boolean render_prepared_replay(boolean mirrors)
{
	struct prepared_header header;
	uint64_t *record;
	unsigned int unknown = 0;
	unsigned long started = system_milliseconds(), count, missing;
	uint64_t *made;
	double waited;
	int formed;

	prepared.recording = FALSE;
	if (!prepared_choose(mirrors) || (record = prepared_read(&header)) == NULL)
		return FALSE;
	halo_load_trace("enumerate.replay.begin", NULL);
	halo_gpu_prepare(TRUE);
	texture_cache_preparation_begin();
	formed = halo_gpu_journal_replay((unsigned int const *)(record + header.draw_count), header.word_count,
		&prepared_bitmaps, &unknown);
	made = prepared_draws(&count);
	halo_load_trace("enumerate.replay.draws.end", NULL);
	waited = halo_gpu_prepared();
	texture_cache_idle();
	halo_load_trace("enumerate.replay.end", NULL);
	/* (every draw the walk made made again: more, a draw the walk did not
	need, makes no difference to play) */
	missing = made ? render_prepared_draws_missing(record, header.draw_count, made, count) : header.draw_count;
	if (missing && made)
		prepared_log_new(record, header.draw_count);
	free(made);
	free(record);
	if (!formed || unknown || missing)
	{
		platform_log("render: the map's preparation's record played back does not make its draws (%s, %u objects "
			"not found, %lu of its %lu draws not made, %lu made): it is made again", formed ? "well formed" :
			"malformed", unknown, missing, (unsigned long)header.draw_count, count);
		DeleteFileA(prepared.path);
		return FALSE;
	}
	platform_log("render: the map's shaders and pipelines made from its record (%lu draws, %lu recorded, %lu words, "
		"in %lu ms, %.0f ms waited for the GPU's)", count, (unsigned long)header.draw_count,
		(unsigned long)header.word_count, system_milliseconds() - started, waited * 1000.0);
	return TRUE;
}

void render_prepared_record_begin(boolean mirrors)
{
	prepared.recording = prepared_choose(mirrors);
	if (prepared.recording)
		halo_gpu_journal_record(&prepared_bitmaps);
}

void render_prepared_record_end(void)
{
	struct prepared_header header;
	unsigned int count, unknown;
	unsigned int *words;
	uint64_t *draws;
	unsigned long draw_count;
	char temporary[80];
	FILE *file;
	boolean written = FALSE;

	if (!prepared.recording)
		return;
	prepared.recording = FALSE;
	words = halo_gpu_journal_take(&count, &unknown);
	draws = prepared_draws(&draw_count);
	if (!words || !count || unknown || !draws)
	{
		platform_log("render: the map's preparation is not recorded (%s, %u objects not kept)", words && draws ?
			"kept" : "out of memory", unknown);
		halo_gpu_journal_free(words);
		free(draws);
		return;
	}
	memset(&header, 0, sizeof(header));
	header.magic = PREPARED_MAGIC;
	header.version = PREPARED_VERSION;
	header.header_size = sizeof(header);
	header.checksum = (uint32_t)prepared.checksum;
	header.key = prepared.key;
	header.draw_count = (uint32_t)draw_count;
	header.word_count = count;
	header.words_hash = xgpu_manifest_hash(words, count * sizeof(*words),
		xgpu_manifest_hash(draws, draw_count * sizeof(*draws), XGPU_MANIFEST_HASH_START));
	snprintf(temporary, sizeof(temporary), "%s.partial", prepared.path);
	file = fopen(temporary, "wb");
	if (file)
	{
		written = fwrite(&header, sizeof(header), 1, file) == 1 &&
			fwrite(draws, sizeof(*draws), draw_count, file) == draw_count &&
			fwrite(words, sizeof(*words), count, file) == count;
		written = fclose(file) == 0 && written;
		DeleteFileA(prepared.path);
		written = written && MoveFileA(temporary, prepared.path);
		if (!written)
			DeleteFileA(temporary);
	}
	platform_log("render: the map's preparation %s (%lu draws, %u words)", written ? "recorded" :
		"could not be recorded", draw_count, count);
	halo_gpu_journal_free(words);
	free(draws);
}
