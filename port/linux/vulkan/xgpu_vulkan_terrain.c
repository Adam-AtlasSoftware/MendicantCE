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
#include "xgpu_vulkan_terrain_prints.h"
#include "xgpu_vulkan_scene.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_post.h"
#include "xgpu_vulkan_post_shaders.h"

#include <math.h>
#include <stdio.h>
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

/* the terrain pack's layers (T1; tools/prism_terrain_layers: layers.ptl), at
most PACK_LAYERS (the pass's uniforms hold eight) */
#define PACK_LAYERS 8u
#define PACK_HEADER 32u
#define PACK_LAYER_BYTES 48u
/* (the detail transfer's fade: whole to FADE_NEAR world units, none past
FADE_FAR: design 3.2) */
#define FADE_NEAR 15.0f
#define FADE_FAR 60.0f

struct terrain_pack
{
	char folder[XGPU_TERRAIN_PATH];
	/* tried (loaded or refused) for this folder; loaded */
	int tried, loaded;
	/* (what the game is told: xgpu_vulkan_terrain_pack_state, written
	atomically, 0 while a new folder is not tried) */
	int state;
	uint32_t size, layers;
	char names[PACK_LAYERS][16];
	/* each layer's: tile metres, strength, chroma, normal, roughness, height
	contrast, the grain's weight, a spare */
	float values[PACK_LAYERS][8];
	struct xgpu_vulkan_image *detail, *surface, *macro;
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
	/* (the map's materials of the water class: an opaque water's, d40's) */
	uint32_t water_materials;
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
	struct terrain_pack pack;
	/* (the pass drawn this frame for this window: BASE_TEXTURED, or the
	resolve where that did not come; logged once) */
	int layers_drawn, layers_logged;
	/* (whether the pass was drawn in any window of this frame; of the last
	whole frame, written atomically: the pack is reported loaded (1) only
	while its layers are drawn, so CE's detail maps never fade under a
	frame with no layers over them: the class view, Classic's frames, a
	target the pass cannot draw to) */
	int drawn_frame, drawn_shown;
	/* (T4) the prints' micro-shadow target (RGBA8, the base's size: the sun's
	visibility at the print's surface, its cavity's occlusion), drawn this
	frame for the window whose base it is (Raster's light pass reads it:
	xgpu_vulkan_terrain_micro); the last FRAME's step, camera and wind */
	struct xgpu_vulkan_image *micro;
	int micro_drawn, prints_logged;
	float step, camera[3], wind[3];
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
			terrain_prints_forget();
		}
		terrain.frame_flags = record->flags;
		terrain.fresh = terrain.ever = 1;
		/* (T4: the prints' clock, relax and infill, with display.terrain_tracks) */
		if (record->flags & XGPU_TERRAIN_FLAG_TRACKS)
			terrain_prints_frame(record->game_seconds, record->camera, record->wind);
		if (record->pack_folder)
		{
			const char *folder = (const char *)host_pointer(record->pack_folder);
			char copy[XGPU_TERRAIN_PATH];

			snprintf(copy, sizeof(copy), "%s", folder ? folder : "");
			if (strcmp(copy, terrain.pack.folder))
			{
				__atomic_store_n(&terrain.pack.state, 0, __ATOMIC_RELEASE);
				memcpy(terrain.pack.folder, copy, sizeof(copy));
				terrain.pack.tried = 0;
			}
		}
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
		{
			event_keep(&record->items[index]);
			if (terrain.frame_flags & XGPU_TERRAIN_FLAG_TRACKS)
				terrain_prints_event(&record->items[index]);
		}
	}
	else if (record->phase == XGPU_TERRAIN_WADERS)
	{
		const struct xgpu_stream_terrain_wader *waders = (const struct xgpu_stream_terrain_wader *)record->items;

		terrain.wader_count = record->count < XGPU_TERRAIN_WADERS_MOST ? record->count : XGPU_TERRAIN_WADERS_MOST;
		memcpy(terrain.waders, waders, terrain.wader_count * sizeof(waders[0]));
		terrain_prints_waders(terrain.waders, terrain.wader_count);
	}
}

int xgpu_vulkan_terrain_view_held(void)
{
	return terrain.fresh && (terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_CLASS) != 0;
}

void xgpu_vulkan_terrain_window(void)
{
	terrain.layers_drawn = 0;
	/* (a window's own: another's micro-shadows are not this one's) */
	terrain.micro_drawn = 0;
}

void xgpu_vulkan_terrain_frame_end(void)
{
	__atomic_store_n(&terrain.drawn_shown, terrain.drawn_frame, __ATOMIC_RELEASE);
	terrain.drawn_frame = 0;
	terrain.fresh = 0;
	terrain.layers_drawn = 0;
	terrain.micro_drawn = 0;
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

/* the pack's layer of a material's class (which: 0 the first, whose layer
the curated table may name; 1 the second, by its class), + 1; 0 none */
static unsigned pack_layer(const struct terrain_material *material, int which)
{
	static const char *const class_names[XGPU_TERRAIN_CLASSES] = { "none", "sand", "dirt", "mud", "rock", "snow",
		"ice", "water" };
	uint8_t class_index = material->classes[which];
	const char *named = NULL;
	char stem[16];
	uint32_t index;

	if (!terrain.pack.loaded || class_index == XGPU_TERRAIN_CLASS_NONE || class_index >= XGPU_TERRAIN_CLASSES ||
		class_index == XGPU_TERRAIN_CLASS_WATER)
		return 0;
	/* (the named layer is the first class's, and the second's too where it
	is the same class: d40's sand is sand and sand, and its second class
	once drew b30's beach sand, grain and all, over d40's paler texels) */
	if ((which == 0 || material->classes[1] == material->classes[0]) &&
		material->layer < sizeof(terrain_layer_names) / sizeof(terrain_layer_names[0]))
		named = terrain_layer_names[material->layer];
	/* (the layer named, else its stem: "sand_desert" is sand's until the pack
	has its own, else the class's) */
	for (index = 0; named && index < terrain.pack.layers; index++)
		if (!strcmp(terrain.pack.names[index], named))
			return index + 1;
	if (named)
	{
		snprintf(stem, sizeof(stem), "%s", named);
		if (strchr(stem, '_'))
			*strchr(stem, '_') = 0;
		for (index = 0; index < terrain.pack.layers; index++)
			if (!strcmp(terrain.pack.names[index], stem))
				return index + 1;
	}
	for (index = 0; index < terrain.pack.layers; index++)
		if (!strcmp(terrain.pack.names[index], class_names[class_index]))
			return index + 1;
	return 0;
}

static int classes_upload(void)
{
	static unsigned char texels[CLASS_TABLE_SIDE * CLASS_TABLE_SIDE * 4];
	uint32_t material, water = 0;

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
			/* (T1: each class's pack layer + 1, the first's in the low 4 bits;
			0 none; the flags and known in alpha) */
			texels[material * 4 + 2] = (unsigned char)(pack_layer(&terrain.materials[material], 0) |
				pack_layer(&terrain.materials[material], 1) << 4);
			texels[material * 4 + 3] = (unsigned char)(terrain.materials[material].known |
				terrain.materials[material].flags << 1);
			if (terrain.materials[material].classes[0] == XGPU_TERRAIN_CLASS_WATER ||
				terrain.materials[material].classes[1] == XGPU_TERRAIN_CLASS_WATER)
				water++;
		}
		terrain.water_materials = water;
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

/* ---------- the HQ layers (T1)

The pack (tools/prism_terrain_layers/prism_terrain_layers.py's layers.ptl in
FRAME's pack folder) read once a folder, its layers made 2D arrays with
whole chains of levels (RGBA8: the detail and height, the normal, roughness
and occlusion), the macro map a 2D image; then, each frame with
display.terrain_layers "hq", the pass over the level's base colour and
normals (xgpu_vulkan_post_terrain_layers) at the texture pass's end. */

static uint32_t levels_of(uint32_t size)
{
	uint32_t levels = 1;

	while (size > 1)
	{
		size >>= 1;
		levels++;
	}
	return levels;
}

static struct xgpu_vulkan_image *pack_image(uint32_t size, uint32_t layers)
{
	struct xgpu_vulkan_image_description description;

	memset(&description, 0, sizeof(description));
	description.type = layers ? XGPU_VULKAN_IMAGE_2D_ARRAY : XGPU_VULKAN_IMAGE_2D;
	description.format = XGPU_VULKAN_FORMAT_RGBA8;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = description.height = size;
	description.depth_or_layers = layers ? layers : 1;
	description.levels = levels_of(size);
	return xgpu_vulkan_image_create(&description);
}

static uint32_t get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void pack_forget(void)
{
	xgpu_vulkan_image_destroy(terrain.pack.detail);
	xgpu_vulkan_image_destroy(terrain.pack.surface);
	xgpu_vulkan_image_destroy(terrain.pack.macro);
	terrain.pack.detail = terrain.pack.surface = terrain.pack.macro = NULL;
	terrain.pack.loaded = 0;
	terrain.pack.layers = 0;
}

/* the pack read and made images of: 1 if it is */
static int pack_load(void)
{
	char path[XGPU_TERRAIN_PATH + 32];
	unsigned char head[PACK_HEADER], entry[PACK_LAYER_BYTES], *texels = NULL;
	uint32_t size, layers, macro, index, which;
	long expected, length;
	FILE *file;

	if (terrain.pack.tried)
		return terrain.pack.loaded;
	terrain.pack.tried = 1;
	pack_forget();
	terrain.classes_changed = 1;
	if (!terrain.pack.folder[0])
	{
		__atomic_store_n(&terrain.pack.state, -1, __ATOMIC_RELEASE);
		return 0;
	}
	snprintf(path, sizeof(path), "%s/layers.ptl", terrain.pack.folder);
	file = fopen(path, "rb");
	if (!file)
	{
		host_logf(HOST_LOG_INFO, "prism terrain: no terrain pack at %s: CE's ground alone "
			"(tools/prism_terrain_layers builds it)", path);
		__atomic_store_n(&terrain.pack.state, -1, __ATOMIC_RELEASE);
		return 0;
	}
	if (fread(head, 1, sizeof(head), file) != sizeof(head) || memcmp(head, "PTL1", 4) || get32(head + 4) != 1)
		goto refused;
	size = get32(head + 8);
	layers = get32(head + 12);
	macro = get32(head + 16);
	if (size < 16 || size > 4096 || (size & (size - 1)) || !layers || layers > PACK_LAYERS || macro < 16 ||
		macro > 4096 || (macro & (macro - 1)))
		goto refused;
	expected = (long)PACK_HEADER + (long)layers * PACK_LAYER_BYTES + 2L * layers * size * size * 4 +
		(long)macro * macro * 4;
	if (fseek(file, 0, SEEK_END) || (length = ftell(file)) != expected || fseek(file, PACK_HEADER, SEEK_SET))
		goto refused;
	for (index = 0; index < layers; index++)
	{
		uint32_t value;

		if (fread(entry, 1, sizeof(entry), file) != sizeof(entry))
			goto refused;
		memcpy(terrain.pack.names[index], entry, 15);
		terrain.pack.names[index][15] = 0;
		for (value = 0; value < 8; value++)
		{
			uint32_t bits = get32(entry + 16 + value * 4);

			memcpy(&terrain.pack.values[index][value], &bits, 4);
			if (!isfinite(terrain.pack.values[index][value]))
				goto refused;
		}
		if (!(terrain.pack.values[index][0] > 0.05f))
			goto refused;
	}
	terrain.pack.detail = pack_image(size, layers);
	terrain.pack.surface = pack_image(size, layers);
	terrain.pack.macro = pack_image(macro, 0);
	texels = malloc((size_t)(size > macro ? size : macro) * (size > macro ? size : macro) * 4);
	if (!terrain.pack.detail || !terrain.pack.surface || !terrain.pack.macro || !texels)
		goto refused;
	for (which = 0; which < 2; which++)
	{
		struct xgpu_vulkan_image *image = which ? terrain.pack.surface : terrain.pack.detail;

		for (index = 0; index < layers; index++)
		{
			if (fread(texels, 1, (size_t)size * size * 4, file) != (size_t)size * size * 4)
				goto refused;
			xgpu_vulkan_image_upload(image, 0, index, 0, 0, 0, size, size, 1, texels, size * 4, size * size * 4);
		}
		xgpu_vulkan_image_make_levels(image, 1);
	}
	if (fread(texels, 1, (size_t)macro * macro * 4, file) != (size_t)macro * macro * 4)
		goto refused;
	xgpu_vulkan_image_upload(terrain.pack.macro, 0, 0, 0, 0, 0, macro, macro, 1, texels, macro * 4, macro * macro * 4);
	xgpu_vulkan_image_make_levels(terrain.pack.macro, 1);
	free(texels);
	fclose(file);
	terrain.pack.size = size;
	terrain.pack.layers = layers;
	terrain.pack.loaded = 1;
	{
		char names[PACK_LAYERS * 18] = "";

		for (index = 0; index < layers; index++)
		{
			strncat(names, index ? ", " : "", sizeof(names) - strlen(names) - 1);
			strncat(names, terrain.pack.names[index], sizeof(names) - strlen(names) - 1);
		}
		host_logf(HOST_LOG_INFO, "prism terrain: the terrain pack %s: %u layers at %u (%s)", path, layers, size, names);
	}
	__atomic_store_n(&terrain.pack.state, 1, __ATOMIC_RELEASE);
	return 1;
refused:
	free(texels);
	fclose(file);
	pack_forget();
	host_logf(HOST_LOG_WARN, "prism terrain: %s is not a whole terrain pack (version 1): CE's ground alone", path);
	__atomic_store_n(&terrain.pack.state, -1, __ATOMIC_RELEASE);
	return 0;
}

int xgpu_vulkan_terrain_pack_state(const char *folder)
{
	char copy[XGPU_TERRAIN_PATH];
	int state;

	/* (the folder the game asks of is the one FRAME last brought: else not
	known yet; the folder changes only with display.terrain_pack_folder, so
	a compare that meets a copy being made says 0 for a frame) */
	if (!folder)
		return 0;
	snprintf(copy, sizeof(copy), "%s", folder);
	if (strncmp(copy, terrain.pack.folder, sizeof(copy)))
		return 0;
	state = __atomic_load_n(&terrain.pack.state, __ATOMIC_ACQUIRE);
	/* (loaded, but its layers not drawn in the last whole frame: 0, as not
	known yet, so nothing fades with nothing over it) */
	if (state == 1 && !__atomic_load_n(&terrain.drawn_shown, __ATOMIC_ACQUIRE))
		return 0;
	return state;
}

/* (T4) the sun toward which the prints' micro-shadows are marched: SCENE_LIGHTS'
sun (its direction toward it, w 1), else w 0 */
static void sun_of(float sun[4])
{
	const struct xgpu_stream_scene_light *lights;
	uint32_t count = 0, serial = 0, index;

	memset(sun, 0, 4 * sizeof(float));
	lights = xgpu_vulkan_scene_light_table(&count, &serial);
	for (index = 0; lights && index < count; index++)
		if (lights[index].type == XGPU_SCENE_LIGHT_SUN && (lights[index].flags & XGPU_SCENE_LIGHT_FLAG_ON))
		{
			float length = sqrtf(dot3(lights[index].position, lights[index].position));

			if (!(length > 1.0e-6f))
				return;
			sun[0] = lights[index].position[0] / length;
			sun[1] = lights[index].position[1] / length;
			sun[2] = lights[index].position[2] / length;
			sun[3] = 1.0f;
			return;
		}
}

/* (T4) the micro-shadow target, the base's size, cleared to lit (1) and
unoccluded (1) for the pass to draw the prints' pixels into; NULL where it
cannot be */
static struct xgpu_vulkan_image *micro_target(struct xgpu_vulkan_image *base)
{
	static const float lit[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
	const struct xgpu_vulkan_image_description *b = xgpu_vulkan_image_description(base);
	int32_t whole[1][4];

	if (!terrain.micro || xgpu_vulkan_image_description(terrain.micro)->width != b->width ||
		xgpu_vulkan_image_description(terrain.micro)->height != b->height)
	{
		struct xgpu_vulkan_image_description description;

		xgpu_vulkan_image_destroy(terrain.micro);
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA8;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
		description.width = b->width;
		description.height = b->height;
		description.depth_or_layers = 1;
		description.levels = 1;
		terrain.micro = xgpu_vulkan_image_create(&description);
		if (!terrain.micro)
			return NULL;
	}
	whole[0][0] = whole[0][1] = 0;
	whole[0][2] = (int32_t)b->width;
	whole[0][3] = (int32_t)b->height;
	xgpu_vulkan_set_targets(terrain.micro, NULL, 0);
	xgpu_vulkan_clear((const int32_t (*)[4])whole, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS, lit, 1.0f,
		0);
	return terrain.micro;
}

struct xgpu_vulkan_image *xgpu_vulkan_terrain_micro(uint32_t width, uint32_t height)
{
	const struct xgpu_vulkan_image_description *d;

	if (!terrain.micro_drawn || !terrain.micro)
		return NULL;
	d = xgpu_vulkan_image_description(terrain.micro);
	return d->width == width && d->height == height ? terrain.micro : NULL;
}

void xgpu_vulkan_terrain_layers(struct xgpu_vulkan_image *base, struct xgpu_vulkan_image *words,
	struct xgpu_vulkan_image *extras, const struct xgpu_stream_scene_view *view)
{
	struct terrain_prints_draw prints;
	struct xgpu_vulkan_image *micro = NULL;
	/* (each class's lightness, Oklab L of CE's colours: none, sand, dirt, mud,
	rock, snow, ice, water) */
	static const float cues[XGPU_TERRAIN_CLASSES] = { 0.5f, 0.74f, 0.52f, 0.42f, 0.55f, 0.92f, 0.85f, 0.5f };
	struct xgpu_vulkan_post_uniforms values;
	uint32_t index;

	if (terrain.layers_drawn || !terrain.fresh || !base || !words || !extras || !view)
		return;
	if (!(terrain.frame_flags & XGPU_TERRAIN_FLAG_LAYERS_HQ) || (terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_CLASS))
		return;
	if (!pack_load() || !classes_upload())
		return;
	memset(&values, 0, sizeof(values));
	values.bounds[0] = (float)terrain.pack.layers;
	values.bounds[1] = FADE_NEAR;
	values.bounds[2] = FADE_FAR;
	values.bounds[3] = getenv("HALO_TERRAIN_LAYERS_VIEW") ? 1.0f : 0.0f;
	memcpy(values.view_eye, view->now[0], sizeof(values.view_eye));
	memcpy(values.view_axis, view->now[1], sizeof(values.view_axis));
	memcpy(values.view_across, view->now[2], sizeof(values.view_across));
	memcpy(values.view_down, view->now[3], sizeof(values.view_down));
	for (index = 0; index < 4; index++)
		values.view_corners[index] = (float)view->corners[index];
	for (index = 0; index < XGPU_TERRAIN_CLASSES; index++)
		values.class_cue[index / 4][index % 4] = cues[index];
	for (index = 0; index < terrain.pack.layers; index++)
	{
		const float *v = terrain.pack.values[index];

		/* (tiles a world unit, 3.048 m; strength, chroma, normal; roughness,
		height contrast, the grain's weight (the pack's first spare: its
		finer lookup), the steepness the three axes start at, negative where
		the taps are not turned (rock: its strata stay level)) */
		values.layer[index * 2][0] = 3.048f / v[0];
		values.layer[index * 2][1] = v[1];
		values.layer[index * 2][2] = v[2];
		values.layer[index * 2][3] = v[3];
		values.layer[index * 2 + 1][0] = v[4];
		values.layer[index * 2 + 1][1] = v[5];
		values.layer[index * 2 + 1][2] = v[6] > 0.0f && v[6] <= 1.0f ? v[6] : 0.0f;
		values.layer[index * 2 + 1][3] = strcmp(terrain.pack.names[index], "rock") ? 0.75f : -0.75f;
	}
	/* (T4: the prints, where display.terrain_tracks is on and a page of them
	lies about the eye: the pass's prints variant, its micro-shadows into
	their own target) */
	memset(&prints, 0, sizeof(prints));
	if ((terrain.frame_flags & XGPU_TERRAIN_FLAG_TRACKS) && !getenv("HALO_TERRAIN_NO_PRINTS"))
	{
		float sun[4];

		sun_of(sun);
		if (terrain_prints_prepare(view->now[0], sun, &prints))
		{
			const char *shown = getenv("HALO_TERRAIN_PRINTS_VIEW");

			memcpy(values.prints, prints.values, sizeof(values.prints));
			values.prints[2][3] = shown ? (float)atof(shown) : 0.0f;
			micro = micro_target(base);
		}
	}
	terrain.layers_drawn = xgpu_vulkan_post_terrain_layers(base, words, extras, terrain.class_image,
		terrain.pack.detail, terrain.pack.surface, terrain.pack.macro, micro ? prints.pool : NULL,
		micro ? prints.table : NULL, micro, &values);
	terrain.micro_drawn = micro && terrain.layers_drawn;
	if (terrain.micro_drawn && !terrain.prints_logged)
	{
		host_logf(HOST_LOG_INFO, "prism terrain: the prints drawn by parallax over the HQ layers, their sun's "
			"micro-shadows for Raster's light (sun %s)", prints.values[1][3] > 0.5f ? "found" : "none");
		terrain.prints_logged = 1;
	}
	terrain.drawn_frame |= terrain.layers_drawn;
	if (!terrain.layers_logged)
	{
		const struct xgpu_vulkan_image_description *b = xgpu_vulkan_image_description(base);

		host_logf(HOST_LOG_INFO, "prism terrain: the HQ layers drawn over %ux%u (base format %u)%s", b->width,
			b->height, b->format, terrain.layers_drawn ? "" : ": not drawn (the targets' formats)");
		terrain.layers_logged = 1;
	}
}

struct xgpu_vulkan_image *xgpu_vulkan_terrain_water_classes(void)
{
	if (!terrain.fresh || !(terrain.frame_flags & XGPU_TERRAIN_FLAG_WATER) ||
		(terrain.frame_flags & XGPU_TERRAIN_FLAG_VIEW_CLASS) || !classes_upload() || !terrain.water_materials)
		return NULL;
	return terrain.class_image;
}
