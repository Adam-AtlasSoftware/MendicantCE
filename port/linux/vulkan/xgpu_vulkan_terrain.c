/*
XGPU_VULKAN_TERRAIN.C

The terrain, weather and water overhaul's host half, T0 (docs/
prism_terrain.md; overhaul_research/prism_v4_renderer/terrain/design.md
sections 2.1 and 2.2): the scaffolding the later milestones draw with.

- The surface classes. Each material of the map (SCENE_MATERIAL) gets a
  pair of classes (XGPU_TERRAIN_CLASS_*: CE's blended environment shaders
  mix two detail maps by the base map's alpha, so a material may be two
  grounds at once), its HQ layer, one HQ tile's metres, its porosity and
  whether drifts pile on it. An environment shader's come from CE's
  material type (dirt, sand, stone, snow, ice); the curated table
  (tools/prism_terrain_classes.py, port/assets/prism_terrain/classes.json,
  built in as xgpu_vulkan_terrain_classes.inc) replaces them by the
  shader's path hash. A model shader typed stone (scenery's rocks) is
  rock; every other is none. CE's material type is kept too.
- PRISM_TERRAIN's records (port/linux/game/prism_terrain.c): the frame
  (generation, time steps, camera, settings, weather and wind) and the
  frame's events and waders. A new generation forgets every event kept.
  Nothing here is sent back: the game never sees it.
- The debug views, over a window's resolved frame (the resolve's end,
  xgpu_vulkan_stream.c): debug.gbuffer_view "terrain_class" (each opaque
  pixel coloured by its material's classes, striped where it has two; an
  object's only where its class is rock, design 2.1; the rest dark grey)
  and debug.terrain_events (the last events kept, each a disc where it
  happened, projected by the window's camera: feet green (left) and red
  (right), landings magenta, a vehicle's contacts orange, impacts cyan,
  detonations and explosions white). Drawn by the post program
  TERRAIN_DEBUG (tools/vulkan_post_shaders.py).

Without a FRAME this frame (Classic, multiplayer, display.terrain "off")
nothing here draws, and the classes are only kept.
*/

#include "xgpu_vulkan_terrain.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_post.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* (xgpu_vulkan_stream.c's GBUFFER_MATERIALS: material ids are 12 bits) */
#define TERRAIN_MATERIALS 4096u
#define CLASS_TABLE_SIDE 64u
/* the events kept for debug.terrain_events: a vehicle's contacts come every
tick a mass point moves on the ground, so they have a ring of their own,
and so do the rest (feet, impacts, explosions); each shown for so many
seconds of real time, fading */
#define FEET_KEPT 128u
#define CONTACTS_KEPT 128u
#define EVENTS_SHOWN (FEET_KEPT + CONTACTS_KEPT)
#define EVENT_SECONDS 30.0f
/* (an event's disc: its radius in world units by kind) */
#define FOOT_RADIUS 0.06f
#define CONTACT_RADIUS 0.05f
#define IMPACT_RADIUS 0.05f

struct terrain_curated
{
	uint32_t hash[2];
	uint8_t classes[2], layer, flags;
	float scale, porosity;
};

#include "xgpu_vulkan_terrain_classes.inc"

struct terrain_material
{
	uint8_t classes[2], layer, flags;
	uint8_t material_type, curated, known, pad;
	float scale, porosity;
};

struct terrain_event_kept
{
	struct xgpu_stream_terrain_event event;
	float seconds;
};

static struct
{
	struct terrain_material materials[TERRAIN_MATERIALS];
	/* the class table's image (CLASS_TABLE_SIDE squared RGBA8: a texel a
	material, its two classes), made again when a class changes */
	struct xgpu_vulkan_image *class_image;
	int classes_changed;
	/* the last FRAME's flags (fresh while this frame's) */
	uint32_t frame_flags;
	int fresh, ever;
	uint32_t generation;
	float seconds;
	/* the kept events, in two rings, and the debug view's events' image
	(two RGBA32F texels each) */
	struct terrain_event_kept feet[FEET_KEPT], contacts[CONTACTS_KEPT];
	uint32_t feet_next, contacts_next, feet_count, contacts_count;
	struct xgpu_vulkan_image *event_image;
	/* the waders this frame */
	struct xgpu_stream_terrain_wader waders[XGPU_TERRAIN_WADERS_MOST];
	uint32_t wader_count;
	/* (counts, logged: events by kind since the last log, the last log's
	seconds, the largest record's bytes) */
	uint32_t counts[XGPU_TERRAIN_EVENT_KINDS];
	float logged_seconds;
	uint32_t largest;
} terrain;

/* ---------- classes */

/* an environment shader's classes by CE's material type (dirt 0, sand 1,
stone 2, snow 3, ice 31: xgpu_vulkan_stream.c's gbuffer_priors' order); a
model's (scenery's rocks) rock at most (design 2.1) */
static uint8_t class_of_type(uint32_t family, uint32_t material_type)
{
	if (family != XGPU_SCENE_FAMILY_SENV)
		return family == XGPU_SCENE_FAMILY_SOSO && material_type == 2 ? XGPU_TERRAIN_CLASS_ROCK :
			XGPU_TERRAIN_CLASS_NONE;
	switch (material_type)
	{
	case 0: return XGPU_TERRAIN_CLASS_DIRT;
	case 1: return XGPU_TERRAIN_CLASS_SAND;
	case 2: return XGPU_TERRAIN_CLASS_ROCK;
	case 3: return XGPU_TERRAIN_CLASS_SNOW;
	case 31: return XGPU_TERRAIN_CLASS_ICE;
	default: return XGPU_TERRAIN_CLASS_NONE;
	}
}

static const float class_scale[XGPU_TERRAIN_CLASSES] = { 0.0f, 2.0f, 2.5f, 2.0f, 4.0f, 3.0f, 4.0f, 0.0f };
static const float class_porosity[XGPU_TERRAIN_CLASSES] = { 0.0f, 0.6f, 0.7f, 0.8f, 0.3f, 0.2f, 0.0f, 0.0f };

/* the layer named as a class is (terrain_layer_names), the first else */
static uint8_t layer_of_class(uint8_t class_index)
{
	static const char *const names[XGPU_TERRAIN_CLASSES] = { "none", "sand", "dirt", "mud", "rock", "snow", "ice",
		"water" };
	uint32_t index;

	for (index = 0; index < sizeof(terrain_layer_names) / sizeof(terrain_layer_names[0]); index++)
		if (!strcmp(terrain_layer_names[index], names[class_index]))
			return (uint8_t)index;
	return 0;
}

void xgpu_vulkan_terrain_material(uint32_t material, const uint32_t hash[2], uint32_t family,
	uint32_t material_type)
{
	struct terrain_material *entry;
	uint32_t index;

	if (!material || material >= TERRAIN_MATERIALS)
		return;
	if (material == 1)
	{
		memset(terrain.materials, 0, sizeof(terrain.materials));
		terrain.classes_changed = 1;
	}
	entry = &terrain.materials[material];
	memset(entry, 0, sizeof(*entry));
	entry->known = 1;
	entry->material_type = material_type < 255u ? (uint8_t)material_type : 255u;
	entry->classes[0] = entry->classes[1] = class_of_type(family, material_type);
	for (index = 0; hash && index < sizeof(terrain_curated) / sizeof(terrain_curated[0]); index++)
	{
		const struct terrain_curated *curated = &terrain_curated[index];

		if (curated->hash[0] == hash[0] && curated->hash[1] == hash[1])
		{
			entry->classes[0] = curated->classes[0];
			entry->classes[1] = curated->classes[1];
			entry->layer = curated->layer;
			entry->flags = curated->flags;
			entry->scale = curated->scale;
			entry->porosity = curated->porosity;
			entry->curated = 1;
			break;
		}
	}
	if (!entry->curated)
	{
		entry->layer = layer_of_class(entry->classes[0]);
		entry->scale = class_scale[entry->classes[0]];
		entry->porosity = class_porosity[entry->classes[0]];
	}
	terrain.classes_changed = 1;
}

uint8_t xgpu_vulkan_terrain_class(uint32_t material, int which)
{
	if (material >= TERRAIN_MATERIALS || which < 0 || which > 1)
		return XGPU_TERRAIN_CLASS_NONE;
	return terrain.materials[material].classes[which];
}

uint8_t xgpu_vulkan_terrain_material_type(uint32_t material)
{
	if (material >= TERRAIN_MATERIALS || !terrain.materials[material].known)
		return 255u;
	return terrain.materials[material].material_type;
}

/* ---------- the records */

static const char *event_name(uint32_t kind)
{
	static const char *const names[XGPU_TERRAIN_EVENT_KINDS] = { "none", "walk", "run", "slide", "shuffle", "jump",
		"land", "contact", "impact", "detonation", "explosion" };

	return kind < XGPU_TERRAIN_EVENT_KINDS ? names[kind] : "?";
}

static void events_forget(void)
{
	terrain.feet_next = terrain.contacts_next = terrain.feet_count = terrain.contacts_count = 0;
}

static void event_keep(const struct xgpu_stream_terrain_event *event)
{
	uint32_t kind = event->kind & 0xffu;
	struct terrain_event_kept *kept;

	if (kind >= XGPU_TERRAIN_EVENT_KINDS || kind == XGPU_TERRAIN_EVENT_NONE)
		return;
	terrain.counts[kind]++;
	if (kind == XGPU_TERRAIN_EVENT_CONTACT)
	{
		kept = &terrain.contacts[terrain.contacts_next];
		terrain.contacts_next = (terrain.contacts_next + 1) % CONTACTS_KEPT;
		if (terrain.contacts_count < CONTACTS_KEPT)
			terrain.contacts_count++;
		/* (debug.terrain_events: one contact in 16 logged; they come every
		tick a wheel moves) */
		if ((terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_EVENTS) && terrain.counts[kind] % 16 == 1)
			host_logf(HOST_LOG_INFO, "prism terrain: event contact at %.3f %.3f %.3f, material %u, speed %.3f, "
				"object %08x", event->position[0], event->position[1], event->position[2],
				(event->kind >> 16) & 0xffu, event->magnitude, event->object);
	}
	else
	{
		kept = &terrain.feet[terrain.feet_next];
		terrain.feet_next = (terrain.feet_next + 1) % FEET_KEPT;
		if (terrain.feet_count < FEET_KEPT)
			terrain.feet_count++;
		/* (debug.terrain_events: each foot, impact and explosion logged, for
		the captures' checks: the events' places against the bipeds') */
		if (terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_EVENTS)
			host_logf(HOST_LOG_INFO, "prism terrain: event %s%s at %.3f %.3f %.3f, material %u, magnitude %.3f, "
				"object %08x", event_name(kind), event->kind & XGPU_TERRAIN_EVENT_RIGHT ? " (right)" : "",
				event->position[0], event->position[1], event->position[2], (event->kind >> 16) & 0xffu,
				event->magnitude, event->object);
	}
	kept->event = *event;
	kept->seconds = terrain.seconds;
}

void xgpu_vulkan_terrain_record(const struct xgpu_stream_prism_terrain *record)
{
	uint32_t index, bytes = (uint32_t)(sizeof(*record) + record->count * sizeof(record->items[0]));

	if (bytes > terrain.largest)
		terrain.largest = bytes;
	if (record->phase == XGPU_TERRAIN_FRAME)
	{
		if (!terrain.ever || record->generation != terrain.generation)
		{
			host_logf(HOST_LOG_INFO, "prism terrain: generation %u (map %08x, BSP %d, %s, %s): the dynamic "
				"state forgotten", record->generation, record->map_checksum, record->bsp_index,
				record->flags & XGPU_TERRAIN_FLAG_PATHTRACED ? "Path Traced" : "Rebaked",
				record->flags & XGPU_TERRAIN_FLAG_CAMPAIGN ? "campaign" : "not a campaign");
			terrain.generation = record->generation;
			events_forget();
		}
		terrain.frame_flags = record->flags;
		terrain.fresh = terrain.ever = 1;
		terrain.seconds += record->real_seconds > 0.0f && record->real_seconds < 1.0f ? record->real_seconds : 0.0f;
		terrain.wader_count = 0;
		/* (every 10 seconds: what came, for the log) */
		if (terrain.seconds - terrain.logged_seconds >= 10.0f)
		{
			uint32_t total = 0;

			for (index = 0; index < XGPU_TERRAIN_EVENT_KINDS; index++)
				total += terrain.counts[index];
			if (total)
				host_logf(HOST_LOG_INFO, "prism terrain: in %.0f s %u events (feet %u, landings %u, contacts %u, "
					"impacts %u, detonations %u, explosions %u); the largest record %u bytes; wind %.2f %.2f %.2f, "
					"weather entry %d", terrain.seconds - terrain.logged_seconds, total,
					terrain.counts[XGPU_TERRAIN_EVENT_WALK] + terrain.counts[XGPU_TERRAIN_EVENT_RUN] +
					terrain.counts[XGPU_TERRAIN_EVENT_SHUFFLE] + terrain.counts[XGPU_TERRAIN_EVENT_SLIDE] +
					terrain.counts[XGPU_TERRAIN_EVENT_JUMP], terrain.counts[XGPU_TERRAIN_EVENT_LAND],
					terrain.counts[XGPU_TERRAIN_EVENT_CONTACT], terrain.counts[XGPU_TERRAIN_EVENT_IMPACT],
					terrain.counts[XGPU_TERRAIN_EVENT_DETONATION], terrain.counts[XGPU_TERRAIN_EVENT_EXPLOSION],
					terrain.largest, record->wind[0], record->wind[1], record->wind[2], record->weather_entry);
			memset(terrain.counts, 0, sizeof(terrain.counts));
			terrain.logged_seconds = terrain.seconds;
		}
		return;
	}
	if (record->generation != terrain.generation)
		return;
	if (record->phase == XGPU_TERRAIN_EVENTS)
	{
		for (index = 0; index < record->count; index++)
			event_keep(&record->items[index]);
	}
	else if (record->phase == XGPU_TERRAIN_WADERS)
	{
		const struct xgpu_stream_terrain_wader *waders = (const struct xgpu_stream_terrain_wader *)record->items;

		terrain.wader_count = record->count < XGPU_TERRAIN_WADERS_MOST ? record->count : XGPU_TERRAIN_WADERS_MOST;
		memcpy(terrain.waders, waders, terrain.wader_count * sizeof(waders[0]));
	}
}

int xgpu_vulkan_terrain_view_held(void)
{
	return terrain.fresh && (terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_CLASS) != 0;
}

void xgpu_vulkan_terrain_frame_end(void)
{
	terrain.fresh = 0;
}

/* ---------- the debug views */

static struct xgpu_vulkan_image *image_of(uint8_t format, uint32_t width, uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

static int classes_upload(void)
{
	static unsigned char texels[CLASS_TABLE_SIDE * CLASS_TABLE_SIDE * 4];
	uint32_t material;

	if (!terrain.class_image)
	{
		terrain.class_image = image_of(XGPU_VULKAN_FORMAT_RGBA8, CLASS_TABLE_SIDE, CLASS_TABLE_SIDE);
		terrain.classes_changed = 1;
	}
	if (!terrain.class_image)
		return 0;
	if (terrain.classes_changed)
	{
		for (material = 0; material < TERRAIN_MATERIALS; material++)
		{
			texels[material * 4 + 0] = terrain.materials[material].classes[0];
			texels[material * 4 + 1] = terrain.materials[material].classes[1];
			texels[material * 4 + 2] = terrain.materials[material].flags;
			texels[material * 4 + 3] = terrain.materials[material].known;
		}
		xgpu_vulkan_image_upload(terrain.class_image, 0, 0, 0, 0, 0, CLASS_TABLE_SIDE, CLASS_TABLE_SIDE, 1, texels,
			CLASS_TABLE_SIDE * 4, sizeof(texels));
		terrain.classes_changed = 0;
	}
	return 1;
}

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* a point of the world in the window's pixels (SCENE_VIEW's camera: the
eye, the axis a unit along it, its change a unit of the screen's x and y,
the view's corners), its distance along the axis and the pixels a world
unit there spans; 0 behind the eye */
static int project(const struct xgpu_stream_scene_view *view, const float point[3], float pixel[2], float *depth,
	float *pixels_per_unit)
{
	const float *eye = view->now[0], *axis = view->now[1], *across = view->now[2], *down = view->now[3];
	float d[3], along, axis_length2 = dot3(axis, axis), across2 = dot3(across, across), down2 = dot3(down, down);
	float x, y, ray[3];
	int k;

	if (axis_length2 <= 0.0f || across2 <= 0.0f || down2 <= 0.0f)
		return 0;
	for (k = 0; k < 3; k++)
		d[k] = point[k] - eye[k];
	along = dot3(d, axis) / axis_length2;
	if (along <= 0.01f)
		return 0;
	for (k = 0; k < 3; k++)
		ray[k] = d[k] / along - axis[k];
	x = dot3(ray, across) / across2;
	y = dot3(ray, down) / down2;
	pixel[0] = (float)view->corners[0] + (x + 1.0f) * 0.5f * (float)(view->corners[2] - view->corners[0]);
	pixel[1] = (float)view->corners[1] + (y + 1.0f) * 0.5f * (float)(view->corners[3] - view->corners[1]);
	*depth = along * sqrtf(axis_length2);
	*pixels_per_unit = 0.5f * (float)(view->corners[2] - view->corners[0]) / (along * sqrtf(across2));
	return 1;
}

static float radius_of(uint32_t kind, float magnitude)
{
	switch (kind)
	{
	case XGPU_TERRAIN_EVENT_CONTACT: return CONTACT_RADIUS;
	case XGPU_TERRAIN_EVENT_IMPACT: return IMPACT_RADIUS;
	case XGPU_TERRAIN_EVENT_DETONATION: return 0.15f;
	case XGPU_TERRAIN_EVENT_EXPLOSION: return magnitude > 0.1f ? magnitude * 0.25f : 0.25f;
	case XGPU_TERRAIN_EVENT_LAND: return FOOT_RADIUS * 1.5f;
	default: return FOOT_RADIUS;
	}
}

/* the kept events projected into the events' image (two texels each: x, y,
radius in pixels, strength; kind, right, 0, 0); their count */
static uint32_t events_upload(const struct xgpu_stream_scene_view *view)
{
	static float texels[EVENTS_SHOWN * 2 * 4];
	uint32_t shown = 0, ring, index;

	if (!terrain.event_image)
		terrain.event_image = image_of(XGPU_VULKAN_FORMAT_RGBA32F, EVENTS_SHOWN * 2, 1);
	if (!terrain.event_image || !view)
		return 0;
	for (ring = 0; ring < 2; ring++)
	{
		const struct terrain_event_kept *kept = ring ? terrain.contacts : terrain.feet;
		uint32_t count = ring ? terrain.contacts_count : terrain.feet_count;

		for (index = 0; index < count; index++)
		{
			const struct xgpu_stream_terrain_event *event = &kept[index].event;
			float age = terrain.seconds - kept[index].seconds, pixel[2], depth, per_unit, radius;
			uint32_t kind = event->kind & 0xffu;
			float *to = &texels[shown * 8];

			if (age > EVENT_SECONDS || !project(view, event->position, pixel, &depth, &per_unit))
				continue;
			radius = radius_of(kind, event->magnitude) * per_unit;
			radius = radius < 5.0f ? 5.0f : radius > 200.0f ? 200.0f : radius;
			to[0] = pixel[0];
			to[1] = pixel[1];
			to[2] = radius;
			to[3] = 1.0f - 0.8f * age / EVENT_SECONDS;
			to[4] = (float)kind;
			to[5] = event->kind & XGPU_TERRAIN_EVENT_RIGHT ? 1.0f : 0.0f;
			to[6] = depth;
			to[7] = 0.0f;
			shown++;
		}
	}
	if (shown)
		xgpu_vulkan_image_upload(terrain.event_image, 0, 0, 0, 0, 0, shown * 2, 1, 1, texels,
			shown * 2 * 4 * sizeof(float), shown * 2 * 4 * sizeof(float));
	return shown;
}

void xgpu_vulkan_terrain_debug(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *words,
	const struct xgpu_stream_scene_view *view)
{
	uint32_t mode = 0, shown = 0;
	float bounds[4];

	if (!terrain.fresh || !target)
		return;
	if (!(terrain.frame_flags & (XGPU_TERRAIN_FLAG_VIEW_CLASS | XGPU_TERRAIN_FLAG_VIEW_EVENTS)))
		return;
	/* (the classes' table is the pass's texture either way) */
	if (!classes_upload())
		return;
	if ((terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_CLASS) && words)
		mode |= 1u;
	if (terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_EVENTS)
	{
		shown = events_upload(view);
		if (shown)
			mode |= 2u;
	}
	if (!mode)
		return;
	bounds[0] = (float)mode;
	bounds[1] = (float)shown;
	bounds[2] = words ? 1.0f : 0.0f;
	bounds[3] = 0.0f;
	xgpu_vulkan_post_terrain_debug(target, words, terrain.class_image, shown ? terrain.event_image : NULL, bounds);
}
