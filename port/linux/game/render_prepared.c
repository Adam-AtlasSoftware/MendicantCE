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
files), a file for each map (its cache file's checksum and scenario), the
records' form (PREPARED_VERSION, not the build: an update keeps them),
whether the frame draws mirrors and the structure it starts in. No setting
is in its key, nor the backend, the platform or the screen's size: one
record is for every player's. The journal holds the game's state, which the
front end turns into pipelines and targets as the settings and the screen
are now (prepared_settings says of each setting why), and the walk draws,
as it records, what every setting's would (the enhanced lights' passes,
the ambient occlusion's pre-pass and Panini's pass, whatever the settings:
the front end plays back what the settings now want of them). (One choice
of the backend's is in what the walk draws: a cut-out's depth first, beside
the cut-out as one draw, where xgpu_backend_capabilities.cutout_prepass
(HALO_VULKAN_CUTOUT_PREPASS=1). A record made without it leaves those two
draws' pipelines to be made in play.) Played back
under the settings, backend and GPU it was made with (its derivation), the
map's enumeration (the set of distinct draws as the front end describes
them: xgpu_manifest_enumeration) must be the record's; under others, which
make other pipelines of the same draws, its objects must all be found. A
record that is missing, cut short, malformed, of another form, whose
objects cannot all be found again, or whose draws are not the record's
under its own derivation, is deleted and the walk made instead, which
records again. A map is recorded at its first load, the stock maps at the game's
first start (menu_pictures_render.c: the menus' pictures' pass loads them
all). HALO_PREPARED_RECORDS=0 turns the records off (the walk every time).

The stock maps' records are shipped with the game too (port/assets/prepared,
made by tools/prepared_records.py: prepared_shipped.h), a file beside the
game holding each compressed, read as it is, never written to z:\prepared. A map's record is looked for in
z:\prepared first, then among the shipped ones; a shipped record that is not
the map's, form's or settings', is malformed, or whose replay does not make
its draws, is passed over for the rest of the game (it cannot be deleted)
and the walk made, which records into z:\prepared as ever. The first start's
pass does not load a map only to record it (record_only) where a record of
it is shipped.

A map prepared again in play (a setting that changes its draws changed:
render_enumerate.c) plays the record it was prepared from (played), whatever
structure it is in now: a record holds every structure's materials, its
objects and effects, so that the one it started in serves them all. A
record that does not play then is left as it is (it played as the map
loaded) and the walk made.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "cseries/cseries_windows.h"
#include "network_standard.h"
#include "menu_models.h"

#include "../src/xgpu_manifest.h"
#include "render_enumerate.h"
#include "render_prepared.h"
#include "prism_world_source.h"
#include "../src/prepared_shipped.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* the platform layer's (port/linux/src/port_config.c, xbox_files.c) */
int config_text(char const *name, char *text, size_t size);
void platform_log(char const *format, ...);
/* (port/linux/src/d3d8_device.c's) */
unsigned long long halo_gpu_journal_derivation(unsigned long long hash);
/* (panini.c's) */
boolean panini_replay_projection(float projection[8]);
/* (d3d8_device.c's: the screen's width in the game's units, its shape) */
long halo_screen_width(void);
/* (cache_files.c's: the tag table as it is now, the menus' tags in it) */
void *cache_files_tag_instances(long *count);

#define PREPARED_MAGIC 0x50314f48u /* "HO1P" */
/* what a record holds: a change to what the walk draws (render_enumerate.c,
and the game's code it draws through) or to this file's form must make it 2,
3 and so on, which makes every record again. Records are not kept for a
build, so that an update keeps them; the journal's own form is in the key
too (halo_gpu_journal_form), and a record whose replay does not make its
draws is made again all the same. */
#define PREPARED_VERSION 7
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
	/* the settings, backend and GPU its draws were derived under
	(prepared_derivation): the enumeration is checked under these alone */
	uint64_t derivation;
};

/* The settings that change a map's draws, none of them in a record's key.
Each is either applied by the front end as a record plays (its pipelines,
targets and textures made as the setting is now), or the walk draws, as it
records, what every value of it would:
- ambient_occlusion: the journal notes each draw's window's span
  whatever the setting (d3d8_device.c, world_span); the draws' pipelines
  lit by the world's occlusion as the setting is now (world_ao_key);
- anisotropic_filtering: the samplers', made as the record plays;
- battery_saver, output_depth: the back buffer's format (RGBA16 or BGRA8),
  made as the record plays;
- anti_aliasing: its pass journaled whatever the setting, the targets'
  samples and the pass made as the record plays;
- astc_textures: the textures' upload, as the record loads them;
- dynamic_lights: the walk draws the passes lit by the enhanced lights as it
  records, whatever the setting (halo_dynamic_lights_walked); the draws'
  pipelines lit as the setting is now;
- graphics: a preset, nothing but the other settings' values;
- hdr, hdr_mode, hdr_highlight_boost: the targets' formats and the tone
  map, made as the record plays; the draws' HDR tags are journaled whatever
  they are, and the boost's taken as they are made;
- high_res_hud, high_res_text: the bitmaps' pixels, as the record loads them
  (the game draws a bitmap as its tag says either way);
- model_detail: the walk draws each level of each model as the Xbox chose it
  (debug_model_lod: model_detail_rules' stock);
- overshield_effect, shield_glow, viewmodel_shield: a unit's constants and
  lights as it plays; the walk draws every modifier shader on every model;
- panini, panini_vertical: its pass journaled whatever the setting
  (panini.c), played back where it is on now (panini_replay_projection);
- per_pixel_lighting: the model lighting's pipelines, made as the record
  plays;
- shadow_resolution: the shadow maps' size, which the journal does not hold;
- soft_particles: the effect's group that can be soft is journaled whatever
  the setting (d3d8_device.c, soft_effect_key), each draw of it soft as the
  setting is now;
- structure_culling: none in the walk (structure_culling.c), the game's own
  visibility's frame;
- first_person_legs, viewmodel_visible: the walk draws every first-person and
  every unit's model whatever the setting.
They are the record's derivation (prepared_derivation), that its
enumeration is checked under, with the backend, the GPU and the screen's
shape (its width in the game's units: the menus and the screen's passes
draw other draws on a display of another). */
static char const *const prepared_settings[] =
{
	"display.ambient_occlusion", "display.anisotropic_filtering", "display.anti_aliasing", "display.astc_textures",
	"display.battery_saver", "display.dynamic_lights", "display.graphics", "display.hdr", "display.hdr_highlight_boost",
	"display.hdr_mode", "display.high_res_hud", "display.high_res_text", "display.model_detail",
	"display.output_depth", "display.overshield_effect", "display.panini", "display.panini_vertical",
	"display.per_pixel_lighting", "display.prism_rt", "display.shadow_resolution", "display.shield_glow", "display.soft_particles",
	"display.structure_culling", "display.first_person_legs", "display.viewmodel_shield", "display.viewmodel_visible",
};

static struct
{
	boolean recording;
	char path[64];
	unsigned long checksum;
	uint64_t key, derivation;
	/* the record played is a shipped one's */
	boolean shipped;
	/* the shipped records passed over (their checksum and key's hash) */
	uint64_t rejected[64];
	unsigned long rejected_count;
	/* the record the map loaded now was prepared from (played back, or
	recorded): its map, whether it draws mirrors, its key; the record
	chosen is it, for a preparation in play (not the one the map's
	structure now would key); the walk recorded draws mirrors */
	struct
	{
		boolean known, mirrors;
		unsigned long checksum;
		char scenario[64];
		uint64_t key;
	} played;
	boolean again, recording_mirrors;
} prepared;

static int prepared_panini(float projection[8])
{
	return panini_replay_projection(projection);
}

/* A bitmap is kept by its tag's index: the map's own tags', and the menus'
(menu_tags.c: the files built into the game, after the map's own), are at
the same indices in every run of a build. The menus' models' tags
(menu_models.c: ui.map's, after the menus', read from a file the first
start's pass makes) are not: they are there only once the file is made, so
a bitmap of theirs is kept by its place among them (PREPARED_MODELS_TAG
and the place: prepared_keep), and a record that names them is keyed by
what they are (prepared_key_models). */
#define PREPARED_MODELS_TAG 0x00010000UL

static unsigned long prepared_keep(unsigned long tag_index)
{
	long place = menu_models_tag_place((long)tag_index);

	return place != NONE ? PREPARED_MODELS_TAG | (unsigned long)place : tag_index;
}

static void *prepared_load(unsigned long tag_index, unsigned long bitmap_index)
{
	if ((tag_index & 0xffff0000UL) == PREPARED_MODELS_TAG)
	{
		long found = menu_models_tag_at((long)(tag_index & 0xffffUL));

		if (found == NONE)
			return NULL;
		tag_index = (unsigned long)found;
	}
	return texture_cache_port_load(tag_index, bitmap_index);
}

/* the tags a record may name by their indices (prepared_identify): the
table's, but the menus' models' after them */
static long prepared_stable_tags(void)
{
	long count = 0, models = 0;

	(void)cache_files_tag_instances(&count);
	while (menu_models_tag_at(models) != NONE)
		models++;
	return count - models;
}

static struct halo_gpu_journal_bitmaps const prepared_bitmaps =
{
	texture_cache_port_identify,
	prepared_load,
	prepared_panini,
	prepared_keep,
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

/* a record's key, up to the map's own (prepared_key): its form, the
journal's and the map's scenario, as 32-bit words (the same on every
platform); no setting (prepared_settings) */
static uint64_t prepared_key_of(char const *scenario, unsigned long journal_form)
{
	uint32_t words[2];

	words[0] = PREPARED_VERSION;
	words[1] = (uint32_t)journal_form;
	return prepared_hash_string(scenario, xgpu_manifest_hash(words, sizeof(words), XGPU_MANIFEST_HASH_START));
}

static uint64_t prepared_key_settings(char const *scenario)
{
	return prepared_key_of(scenario, halo_gpu_journal_form());
}

/* ...and the map's: whether it draws mirrors, the structure it starts in */
static uint64_t prepared_key(uint64_t hash, boolean mirrors, long structure_bsp_index)
{
	uint32_t words[2];

	words[0] = mirrors ? 1 : 0;
	words[1] = (uint32_t)structure_bsp_index;
	return xgpu_manifest_hash(words, sizeof(words), hash);
}

/* ...and the menus' models' tags in the table (menu_models_identity: none,
or what they are), which a record that names them is for */
static uint64_t prepared_key_models(uint64_t hash)
{
	uint32_t identity = (uint32_t)menu_models_identity();

	return identity ? xgpu_manifest_hash(&identity, sizeof(identity), hash) : hash;
}

/* the enumeration's label for HALO_RENDER_COVERAGE (xgpu_manifest.h): the
map, how it is prepared (walked, played back from its record), the
screen's width and the menus' models' tags in the table (prepared_key_models) */
void prepared_coverage_label(char const *how)
{
	char label[160];

	snprintf(label, sizeof(label), "%s %s %ld %lx", global_scenario_index != NONE ?
		tag_get_name(global_scenario_index) : "", how, halo_screen_width(), menu_models_identity());
	xgpu_manifest_coverage_label(label);
}

/* the settings, backend and GPU a record's draws are derived under now */
static uint64_t prepared_derivation(void);

unsigned long long render_prepared_derivation(void)
{
	return prepared_derivation();
}

static uint64_t prepared_derivation(void)
{
	uint64_t hash = halo_gpu_journal_derivation(XGPU_MANIFEST_HASH_START);
	/* (the screen's shape, which the menus' and the screen's passes are drawn
	to: a record made on a display of another plays as under other settings) */
	int32_t screen = (int32_t)halo_screen_width();
	short index;

	hash = xgpu_manifest_hash(&screen, sizeof(screen), hash);
	for (index = 0; index < (short)NUMBEROF(prepared_settings); index++)
	{
		/* (as text, whatever its type: a boolean's, a number's) */
		char value[256];

		if (!config_text(prepared_settings[index], value, sizeof(value)))
			value[0] = 0;
		/* (the dynamic lights a Pro 2v2 game draws: the classic ones,
		whatever the setting, as d3d8_device.c draws them) */
		if (!strcmp(prepared_settings[index], "display.dynamic_lights") && !network_standard_enhanced_lights_allowed())
			snprintf(value, sizeof(value), "classic");
		hash = prepared_hash_string(prepared_settings[index], hash);
		hash = prepared_hash_string(value, hash);
	}
	return hash;
}

/* the record the map loaded now is prepared from (prepared_choose's) */
static void prepared_played(boolean mirrors)
{
	char const *scenario = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";

	prepared.played.known = TRUE;
	prepared.played.mirrors = mirrors;
	prepared.played.checksum = prepared.checksum;
	strncpy(prepared.played.scenario, scenario, sizeof(prepared.played.scenario) - 1);
	prepared.played.scenario[sizeof(prepared.played.scenario) - 1] = 0;
	prepared.played.key = prepared.key;
}

/* the record's key and file, for the map loaded now */
static boolean prepared_choose(boolean mirrors)
{
	char const *scenario = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
	uint64_t hash;

	if (!prepared_enabled())
		return FALSE;
	/* (the structure the map starts in: a saved game's may be another) */
	hash = prepared_key_models(prepared_key(prepared_key_settings(scenario), mirrors,
		global_structure_bsp_index_get()));
	/* (v4 geopatch M2: a map drawn with its enhanced world geometry keys its
	records by the checksum and the BSP's enhanced world_id, as the render
	caches built from that geometry are: prism_world_source.c) */
	{
		unsigned char id[32];
		uint64_t world = 0;
		int k;

		prism_world_source_world_id(global_structure_bsp_index_get(), id);
		for (k = 0; k < 8; k++)
			world |= (uint64_t)id[k] << (8 * k);
		hash ^= world;
	}
	prepared.checksum = cache_files_get_checksum();
	/* (prepared again in play: from its record, whatever structure it is in) */
	prepared.again = prepared.played.known && prepared.played.checksum == prepared.checksum &&
		prepared.played.mirrors == mirrors && !strcmp(prepared.played.scenario, scenario) &&
		prepared.played.key != hash;
	if (prepared.again)
		hash = prepared.played.key;
	prepared.key = hash;
	prepared.derivation = prepared_derivation();
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
		hashes[index] = xgpu_manifest_draw_hash(&enumeration->draws[index], XGPU_MANIFEST_HASH_START);
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

/* a record played back is good: well formed, its objects all found, and,
played under the derivation it was made with, its draws all made */
boolean render_prepared_played(int formed, unsigned long unknown, unsigned long missing, boolean same_derivation)
{
	return formed && !unknown && (!missing || !same_derivation);
}

/* a record's header is of this form and map (key) */
static boolean prepared_header_fits(struct prepared_header const *header, unsigned long checksum, uint64_t key)
{
	return header->magic == PREPARED_MAGIC && header->version == PREPARED_VERSION &&
		header->header_size == sizeof(*header) && header->checksum == (uint32_t)checksum && header->key == key &&
		header->word_count && header->word_count <= PREPARED_MAXIMUM_WORDS &&
		header->draw_count <= PREPARED_MAXIMUM_WORDS / 2;
}

/* a shipped record of a map and key passed over */
static uint64_t prepared_rejected_hash(unsigned long checksum, uint64_t key)
{
	uint32_t value = (uint32_t)checksum;

	return xgpu_manifest_hash(&value, sizeof(value), xgpu_manifest_hash(&key, sizeof(key), XGPU_MANIFEST_HASH_START));
}

static boolean prepared_shipped_rejected(unsigned long checksum, uint64_t key)
{
	uint64_t hash = prepared_rejected_hash(checksum, key);
	unsigned long index;

	for (index = 0; index < prepared.rejected_count; index++)
	{
		if (prepared.rejected[index] == hash)
			return TRUE;
	}
	return FALSE;
}

static void prepared_shipped_reject(unsigned long checksum, uint64_t key)
{
	if (!prepared_shipped_rejected(checksum, key) && prepared.rejected_count < NUMBEROF(prepared.rejected))
		prepared.rejected[prepared.rejected_count++] = prepared_rejected_hash(checksum, key);
}

/* the shipped record of a map and key (its draws, then its words), inflated
and checked; NULL if none is shipped, or it is passed over (never deleted:
the game's own) */
static uint64_t *prepared_shipped_take(unsigned long checksum, uint64_t key, struct prepared_header *header)
{
	unsigned int size;
	unsigned char *data;
	unsigned long payload;

	if (prepared_shipped_rejected(checksum, key))
		return NULL;
	size = prepared_shipped_size((unsigned int)checksum, (unsigned int)(key >> 32), (unsigned int)key);
	if (!size)
		return NULL;
	/* (the game's memory, which the caller frees) */
	data = malloc(size);
	if (data && size >= sizeof(*header) &&
		prepared_shipped_inflate((unsigned int)checksum, (unsigned int)(key >> 32), (unsigned int)key, data, size))
	{
		memcpy(header, data, sizeof(*header));
		payload = size - sizeof(*header);
		if (prepared_header_fits(header, checksum, key) &&
			payload == header->word_count * sizeof(unsigned int) + header->draw_count * sizeof(uint64_t) &&
			xgpu_manifest_hash(data + sizeof(*header), payload, XGPU_MANIFEST_HASH_START) == header->words_hash)
		{
			/* (the draws first, where the caller frees them) */
			memmove(data, data + sizeof(*header), payload);
			return (uint64_t *)data;
		}
	}
	if (data)
		free(data);
	platform_log("render: the map's preparation's shipped record is not this map's, form's or settings', or does "
		"not inflate whole: passed over");
	prepared_shipped_reject(checksum, key);
	return NULL;
}

/* the record (its draws, then its words), read and checked: the player's,
else a shipped one; NULL if there is none to play */
static uint64_t *prepared_read(struct prepared_header *header)
{
	FILE *file = fopen(prepared.path, "rb");
	uint64_t *words = NULL;
	unsigned long size;
	boolean good = FALSE;

	prepared.shipped = FALSE;
	if (!file)
	{
		words = prepared_shipped_take(prepared.checksum, prepared.key, header);
		prepared.shipped = words != NULL;
		return words;
	}
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
		platform_log("render: the map's preparation's record is not this map's, form's or settings', or is cut "
			"short: made again");
		/* (the game's free halts on NULL) */
		if (words)
			free(words);
		DeleteFileA(prepared.path);
		words = prepared_shipped_take(prepared.checksum, prepared.key, header);
		prepared.shipped = words != NULL;
		return words;
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
		uint64_t hash = xgpu_manifest_draw_hash(draw, XGPU_MANIFEST_HASH_START);

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

/* the stock maps' records made in this form (the pass's note, z:\prepared\stock) */
static void prepared_stock_form(char *text, unsigned long size)
{
	snprintf(text, size, "%d %u\n", PREPARED_VERSION, halo_gpu_journal_form());
}

boolean render_prepared_stock_recorded(void)
{
	char form[32], read[32];
	FILE *file;
	boolean same = FALSE;

	if (!prepared_enabled())
		return TRUE;
	prepared_stock_form(form, sizeof(form));
	file = fopen("z:\\prepared\\stock", "rb");
	if (file)
	{
		size_t length = fread(read, 1, sizeof(read) - 1, file);

		read[length] = 0;
		same = !strcmp(read, form);
		fclose(file);
	}
	return same;
}

void render_prepared_stock_done(void)
{
	char form[32];
	FILE *file;

	if (!prepared_enabled())
		return;
	prepared_stock_form(form, sizeof(form));
	CreateDirectoryA("z:\\prepared", NULL);
	file = fopen("z:\\prepared\\stock", "wb");
	if (file)
	{
		fwrite(form, 1, strlen(form), file);
		fclose(file);
	}
}

boolean render_prepared_shipped(unsigned long checksum, unsigned long long key)
{
	struct prepared_header header;
	uint64_t *record;

	if (!prepared_enabled())
		return FALSE;
	record = prepared_shipped_take(checksum, key, &header);
	if (!record)
		return FALSE;
	free(record);
	return TRUE;
}

boolean render_prepared_shipped_map(unsigned long checksum, char const *scenario)
{
	uint64_t hash;
	long structure_bsp_index;
	int mirrors;

	if (!prepared_enabled() || !checksum || !scenario)
		return FALSE;
	hash = prepared_key_settings(scenario);
	/* (the map's own part of its key known once it is loaded: any of them) */
	for (structure_bsp_index = 0; structure_bsp_index < LONG_BITS; structure_bsp_index++)
	{
		for (mirrors = 0; mirrors < 2; mirrors++)
		{
			if (render_prepared_shipped(checksum, prepared_key(hash, (boolean)mirrors, structure_bsp_index)))
				return TRUE;
		}
	}
	return FALSE;
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
	prepared_coverage_label("replay");
	halo_gpu_prepare(TRUE);
	texture_cache_preparation_begin();
	formed = halo_gpu_journal_replay((unsigned int const *)(record + header.draw_count), header.word_count,
		&prepared_bitmaps, &unknown);
	made = prepared_draws(&count);
	halo_load_trace("enumerate.replay.draws.end", NULL);
	waited = halo_gpu_prepared();
	texture_cache_idle();
	halo_load_trace("enumerate.replay.end", NULL);
	/* (every draw the walk made made again, under the derivation it was
	made with: more, a draw the walk did not need, makes no difference to
	play; under another, the draws' pipelines are others) */
	missing = made ? render_prepared_draws_missing(record, header.draw_count, made, count) : header.draw_count;
	if (missing && made && header.derivation == prepared.derivation)
		prepared_log_new(record, header.draw_count);
	if (made)
		free(made);
	free(record);
	if (!render_prepared_played(formed, unknown, missing, header.derivation == prepared.derivation))
	{
		platform_log("render: the map's preparation's record played back does not make its draws (%s, %u objects "
			"not found, %lu of its %lu draws not made, %lu made): it is made again%s", formed ? "well formed" :
			"malformed", unknown, missing, (unsigned long)header.draw_count, count, prepared.again ?
			" (in play, from another structure than its own: the record kept)" : "");
		/* (in play, from another structure: the record played as the map
		loaded, and is kept) */
		if (prepared.again)
			prepared.played.known = FALSE;
		else if (prepared.shipped)
			prepared_shipped_reject(prepared.checksum, prepared.key);
		else
			DeleteFileA(prepared.path);
		return FALSE;
	}
	prepared_played(mirrors);
	platform_log("render: the map's shaders and pipelines made from its %s record%s (%lu draws, %lu recorded, %lu words, "
		"%s, in %lu ms, %.0f ms waited for the GPU's)", prepared.shipped ? "shipped" : "own",
		prepared.again ? ", again in play" : "", count,
		(unsigned long)header.draw_count, (unsigned long)header.word_count, header.derivation == prepared.derivation ?
		"its own settings" : "other settings", system_milliseconds() - started, waited * 1000.0);
	return TRUE;
}

void render_prepared_record_begin(boolean mirrors)
{
	prepared.recording = prepared_choose(mirrors);
	prepared.recording_mirrors = mirrors;
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
	header.derivation = prepared.derivation;
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
	if (written)
		prepared_played(prepared.recording_mirrors);
	/* (its file, scenario and form, and the tags it may name by their index
	(prepared_stable_tags), as tools/prepared_records.py reads them) */
	platform_log("render: the map's preparation %s (%lu draws, %u words; %s, %s, form %d %u, tags %ld)", written ?
		"recorded" : "could not be recorded", draw_count, count, prepared.path, global_scenario_index != NONE ?
		tag_get_name(global_scenario_index) : "", PREPARED_VERSION, halo_gpu_journal_form(), prepared_stable_tags());
	halo_gpu_journal_free(words);
	free(draws);
}
