/*
XGPU_VULKAN_SCENE.C

PRISM v4's scene records as the Vulkan backend keeps them
(xgpu_vulkan_scene.h; docs/prism_scene_records.md).

SCENE_STATIC: the level's geometry as drawn (the map's triangles, or the
remodel's near and far levels both), received a slice at a time into a set
of its own, which becomes the whole set at _END if every triangle and its
attributes came; the last whole set stays until another replaces it or
_RELEASE. It is the one copy of the level the backend's consumers read:
display.prism_rt's acceleration structures (xgpu_vulkan_rt.c builds its own
from it), M2's baker. _LEVELS, each surface's level drawn now, is kept
apart, with a serial that moves when it changes, so that the traced level
follows the drawn one.

Every count is checked against the set's own totals; a slice of another
generation, or past the end, is dropped.
*/

#include "host.h"
#include "xgpu_vulkan_scene.h"

#include <stdlib.h>
#include <string.h>

/* a BSP's triangles at most (the remodel's near and far levels of the
largest map: b30's about 2.6 million) */
#define SCENE_STATIC_TRIANGLES_MOST (16u << 20)
#define SCENE_STATIC_PAGES_MOST 4096u
#define SCENE_STATIC_SURFACES_MOST (4u << 20)

struct scene_static_receiving
{
	uint32_t generation;
	uint32_t total, surfaces, page_count;
	uint32_t triangles_received, attributes_received;
	float *positions;
	uint32_t *attributes;
	uint32_t *pages;
};

static struct
{
	struct scene_static_receiving receiving;
	struct scene_static_receiving whole_storage;
	struct xgpu_scene_static_set whole;
	int whole_valid;
	uint32_t *levels;
	uint32_t level_words, level_generation, level_serial;
	/* SCENE_LIGHTS: the table being received (its total, the count come in
	order) and the last whole one */
	struct xgpu_stream_scene_light *lights_receiving, *lights;
	uint32_t lights_total, lights_received, light_count, lights_serial, lights_logged;
} scene;

static void receiving_free(struct scene_static_receiving *set)
{
	free(set->positions);
	free(set->attributes);
	free(set->pages);
	memset(set, 0, sizeof(*set));
}

static void levels_free(void)
{
	free(scene.levels);
	scene.levels = NULL;
	scene.level_words = 0;
	scene.level_generation = 0;
	/* (the serial moves on: none now is a change) */
	scene.level_serial++;
}

void xgpu_vulkan_scene_static(const struct xgpu_stream_scene_static *record, const void *data)
{
	struct scene_static_receiving *set = &scene.receiving;

	switch (record->phase)
	{
	case XGPU_SCENE_STATIC_BEGIN:
		receiving_free(set);
		if (!record->total || record->total > SCENE_STATIC_TRIANGLES_MOST || record->count > SCENE_STATIC_SURFACES_MOST)
			break;
		set->positions = malloc((size_t)record->total * 9 * sizeof(float));
		set->attributes = calloc((size_t)record->total, XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * 4);
		if (!set->positions || !set->attributes)
		{
			receiving_free(set);
			host_logf(HOST_LOG_WARN, "prism scene: no memory for the level's %u triangles", record->total);
			break;
		}
		set->generation = record->generation;
		set->total = record->total;
		set->surfaces = record->count;
		break;
	case XGPU_SCENE_STATIC_PAGES:
		if (!set->positions || record->generation != set->generation || !data ||
			record->total > SCENE_STATIC_PAGES_MOST || record->first > record->total ||
			record->count > record->total - record->first)
			break;
		if (!set->pages || set->page_count != record->total)
		{
			free(set->pages);
			set->pages = calloc(record->total ? record->total : 1, 8);
			set->page_count = set->pages ? record->total : 0;
		}
		if (set->pages)
			memcpy(set->pages + (size_t)record->first * 2, data, (size_t)record->count * 8);
		break;
	case XGPU_SCENE_STATIC_TRIANGLES:
		if (!set->positions || record->generation != set->generation || !data || record->first > set->total ||
			record->count > set->total - record->first)
			break;
		memcpy(set->positions + (size_t)record->first * 9, data, (size_t)record->count * 36);
		/* (the slices come in order: the count received is where the last ended) */
		if (record->first == set->triangles_received)
			set->triangles_received += record->count;
		break;
	case XGPU_SCENE_STATIC_ATTRIBUTES:
		if (!set->attributes || record->generation != set->generation || !data || record->first > set->total ||
			record->count > set->total - record->first)
			break;
		memcpy(set->attributes + (size_t)record->first * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS, data,
			(size_t)record->count * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * 4);
		if (record->first == set->attributes_received)
			set->attributes_received += record->count;
		break;
	case XGPU_SCENE_STATIC_END:
		if (!set->positions || record->generation != set->generation || set->triangles_received != set->total ||
			set->attributes_received != set->total)
		{
			if (set->positions)
				host_logf(HOST_LOG_WARN, "prism scene: the level's set %u ended unwhole (%u and %u of %u)",
					set->generation, set->triangles_received, set->attributes_received, set->total);
			receiving_free(set);
			break;
		}
		receiving_free(&scene.whole_storage);
		scene.whole_storage = *set;
		memset(set, 0, sizeof(*set));
		scene.whole.generation = scene.whole_storage.generation;
		scene.whole.triangle_count = scene.whole_storage.total;
		scene.whole.surface_count = scene.whole_storage.surfaces;
		scene.whole.page_count = scene.whole_storage.page_count;
		scene.whole.positions = scene.whole_storage.positions;
		scene.whole.attributes = scene.whole_storage.attributes;
		scene.whole.pages = scene.whole_storage.pages;
		scene.whole_valid = 1;
		if (scene.level_generation != scene.whole.generation)
			levels_free();
		host_logf(HOST_LOG_INFO, "prism scene: the level's set %u: %u triangles, %u surfaces, %u lightmap pages",
			scene.whole.generation, scene.whole.triangle_count, scene.whole.surface_count, scene.whole.page_count);
		break;
	case XGPU_SCENE_STATIC_LEVELS:
	{
		uint32_t words = (scene.whole_valid && record->generation == scene.whole.generation ? scene.whole.surface_count :
			set->positions && record->generation == set->generation ? set->surfaces : 0) + 31u;

		words /= 32u;
		if (!words || !data || record->total != words || record->first > words || record->count > words - record->first)
			break;
		if (!scene.levels || scene.level_words != words || scene.level_generation != record->generation)
		{
			levels_free();
			scene.levels = calloc(words, 4);
			if (!scene.levels)
				break;
			scene.level_words = words;
			scene.level_generation = record->generation;
		}
		if (memcmp(scene.levels + record->first, data, (size_t)record->count * 4))
		{
			memcpy(scene.levels + record->first, data, (size_t)record->count * 4);
			scene.level_serial++;
		}
		break;
	}
	default:
		receiving_free(set);
		receiving_free(&scene.whole_storage);
		memset(&scene.whole, 0, sizeof(scene.whole));
		scene.whole_valid = 0;
		levels_free();
		break;
	}
}

/* a table of lights at most (SCENE_LIGHTS' total: the fixtures, emitters
and dynamic lights of a frame) */
#define SCENE_LIGHTS_MOST 65536u

void xgpu_vulkan_scene_lights(const struct xgpu_stream_scene_lights *record)
{
	struct xgpu_stream_scene_light *swap;
	uint32_t index;

	if (record->total > SCENE_LIGHTS_MOST)
		return;
	if (!record->first)
	{
		if (!scene.lights_receiving || scene.lights_total < record->total)
		{
			free(scene.lights_receiving);
			scene.lights_receiving = malloc((size_t)(record->total ? record->total : 1) * sizeof(*scene.lights_receiving));
		}
		scene.lights_total = scene.lights_receiving ? record->total : 0;
		scene.lights_received = 0;
	}
	if (!scene.lights_receiving || record->total != scene.lights_total || record->first != scene.lights_received ||
		record->count > scene.lights_total - record->first)
		return;
	memcpy(scene.lights_receiving + record->first, record->lights, (size_t)record->count * sizeof(record->lights[0]));
	/* (the numbers finite: as sent, but checked) */
	for (index = record->first; index < record->first + record->count; index++)
	{
		float *values = scene.lights_receiving[index].position;
		int value;

		for (value = 0; value < 16; value++)
			if (!(values[value] == values[value]) || values[value] > 1.0e30f || values[value] < -1.0e30f)
				values[value] = 0.0f;
	}
	scene.lights_received += record->count;
	if (scene.lights_received != scene.lights_total)
		return;
	swap = scene.lights;
	scene.lights = scene.lights_receiving;
	scene.lights_receiving = swap;
	scene.light_count = scene.lights_total;
	scene.lights_serial++;
	/* (the receiving one's room is the old table's: its total unknown) */
	free(scene.lights_receiving);
	scene.lights_receiving = NULL;
	if (scene.light_count != scene.lights_logged)
	{
		uint32_t classes[XGPU_SCENE_LIGHT_CLASSES] = { 0 }, types[XGPU_SCENE_LIGHT_TYPES] = { 0 };

		for (index = 0; index < scene.light_count; index++)
		{
			if (scene.lights[index].light_class < XGPU_SCENE_LIGHT_CLASSES)
				classes[scene.lights[index].light_class]++;
			if (scene.lights[index].type < XGPU_SCENE_LIGHT_TYPES)
				types[scene.lights[index].type]++;
		}
		scene.lights_logged = scene.light_count;
		host_logf(HOST_LOG_INFO, "prism scene: the light table: %u lights (sun %u, domes %u, sky %u, points %u, spots "
			"%u, emitters %u; static %u, animated %u, switchable %u, dynamic %u)", scene.light_count,
			types[XGPU_SCENE_LIGHT_SUN], types[XGPU_SCENE_LIGHT_DOME], types[XGPU_SCENE_LIGHT_SKY],
			types[XGPU_SCENE_LIGHT_POINT], types[XGPU_SCENE_LIGHT_SPOT], types[XGPU_SCENE_LIGHT_EMITTER],
			classes[XGPU_SCENE_LIGHT_CLASS_STATIC], classes[XGPU_SCENE_LIGHT_CLASS_ANIMATED],
			classes[XGPU_SCENE_LIGHT_CLASS_SWITCHABLE], classes[XGPU_SCENE_LIGHT_CLASS_DYNAMIC]);
	}
}

const struct xgpu_stream_scene_light *xgpu_vulkan_scene_light_table(uint32_t *count, uint32_t *serial)
{
	*count = scene.lights ? scene.light_count : 0;
	*serial = scene.lights_serial;
	return scene.lights;
}

const struct xgpu_scene_static_set *xgpu_vulkan_scene_static_set(void)
{
	return scene.whole_valid ? &scene.whole : NULL;
}

const uint32_t *xgpu_vulkan_scene_levels(uint32_t *words, uint32_t *serial)
{
	*serial = scene.level_serial;
	if (!scene.levels || !scene.whole_valid || scene.level_generation != scene.whole.generation)
	{
		*words = 0;
		return NULL;
	}
	*words = scene.level_words;
	return scene.levels;
}
