/*
PRISM_BAKE.C

PRISM v4's in-game baker, the game's half (overhaul_research/
prism_v4_renderer/design.md, section 3; docs/prism_bake.md). The bake
itself is the renderer's (port/linux/vulkan/xgpu_vulkan_bake.c): it traces
the level as drawn (SCENE_STATIC), its materials and lights (display.
prism_rt's scene and rig) and writes the bake file. What only the game has
is sent from here, in the v4 modes that read a bake (display.lighting
"raster"), never with "ce":

- each frame, PRISM_BAKE's control: the map and the BSP drawn, display.bake
  ("auto" bakes in the background while playing, "off" never; the older
  "ask" is "auto": the main menu bakes the campaign's levels before they are
  played, prism_bake_menu.c), display.bake_quality, the rebakes asked for
  (display.bake_rebake: each change starts the BSP's bake again from
  nothing), the bake folder (display.bake_folder; <data>/prism_bake), the
  GPU time a frame may give the bake, and where the host writes how the
  bake goes (the status block);
- once a BSP, what the reachable space is found from (the BSP's collision
  BSP, its clusters, leaves and PVS, its water, its render triangles, and
  the scenario's places and the fighters it places): copied from the tags
  and sent a slice a frame, for the renderer, whose 64-bit memory holds the
  walk of a large BSP (b30's millions of cells), to find with the baker's
  library (prism_bake_reach.c): each render surface's importance tier, which
  sets its texels' density, and the bricks of the irradiance volume.

debug.bake_exit: the bake in the foreground, the game quitting once the
BSP's is done (the measurements' runs). debug.bake_worker (the main menu's
worker, prism_bake_menu.c): in the foreground too, at its own budget; the
worker goes from BSP to BSP and quits itself. Nothing here changes what the game
simulates: it reads the scenario and the BSP, read-only once loaded, and
the settings. Nothing crosses the network.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp_definitions.h"
#include "tag_files/tag_files.h"
#include "bitmaps/bitmap_group.h"
#include "cache/texture_cache.h"
#include "models/model_animation_definitions.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "physics/collision_model_definitions.h"
#include "rasterizer/rasterizer_model_types.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "prism_deferred.h"
#include "prism_bake_menu.h"
#include "../src/xgpu_coverage.h"

#include <xtl.h>
#include "../vulkan/prism_bake_reach.h"
#include "../src/xgpu_stream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator: the reach's thread calls it) */
#undef malloc
#undef free
#undef realloc
#undef calloc

/* ---------- prototypes */

char const *config_string(char const *name);
int config_boolean(char const *name);
long config_integer(char const *name);
double config_real(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
char const *platform_data_root(void);
int halo_prism_rt_available(void);
/* prism_rt.c's: the bounces' albedo the base maps' stored mean (the light's
units) rather than made linear */
int prism_rt_albedo_stored(void);
void xgpu_backend_prism_bake(const struct xgpu_stream_prism_bake *bake, const void *data, unsigned long size);
unsigned long prism_scene_static_generation(void);
int prism_scene_static_ended(void);
/* render_enumerate.c's (the window drawn is a map's first frame's, unseen) */
extern unsigned char render_enumerating;
/* ui_widget.c's, game.c's */
boolean main_menu_is_active(void);
boolean game_in_progress(void);
/* mod_meshes.c's: a model drawn by a mod's mesh (mods.h) */
boolean mod_meshes_model_replaced(long model_index);

/* ---------- constants */

/* the scenario's places (scenario_definitions.h; their blocks' offsets in
the tag and their elements' sizes, as tools/prism_bake/ce_map.py reads
them from the map) */
#define SCNR_BIPEDS 0x228
#define SCNR_VEHICLES 0x240
#define SCNR_VEHICLE_PALETTE 0x24C
#define SCNR_EQUIPMENT 0x258
#define SCNR_WEAPONS 0x270
#define SCNR_PLAYERS 0x354
#define SCNR_NETGAME_FLAGS 0x378
#define SCNR_NETGAME_EQUIPMENT 0x384
#define SCNR_ENCOUNTERS 0x42C
#define SCNR_CUTSCENE_FLAGS 0x4E4
#define SCNR_CUTSCENE_CAMERA_POINTS 0x4F0
/* a vehicle's type in its tag, and the type the player flies (an alien
fighter, the Banshee: a Pelican, a plane, is flown only by the game) */
#define VEHI_TYPE 0x2F4
#define VEHICLE_FIGHTER 5
#define SEEDS_MOST 65536
/* the static scenery's triangles: the floats each (corners, albedo,
coverage), the most sent (past it the rest are left out), a model's detail
levels (super low to super high) and its parts' most; the albedo's bounds
(display.prism_rt's: prism_rt.c's PRISM_RT_ALBEDO_*) */
#define SCENERY_FLOATS 13
#define FIXTURE_FLOATS 12
/* a light fixture placement's bytes (tag_schema_scenario.c's) */
#define SCNR_LIGHT_FIXTURE 0x58
#define SCENERY_MOST 3000000
#define SCENERY_DETAIL_LEVELS 5
#define SCENERY_ALBEDO_LEAST 0.03f
#define SCENERY_ALBEDO_MOST 0.85f
/* the models a mod's mesh draws among the static scenery's, at most (b30's
three boulders; past it the rest stay in the frame's traced scene) */
#define SCENERY_REPLACED_MODELS 256
/* the static scenery's cutouts' alpha maps: the most (each its base map's),
and their largest side (a leaf card's alpha at most this many texels a side:
display.prism_rt's level maps' PRISM_RT_ALPHA_SIDE) */
#define SCENERY_ALPHA_MAPS 256
#define SCENERY_ALPHA_SIDE 256
#define STATUS_WORDS XGPU_BAKE_STATUS_WORDS

/* what the reach's thread works from: copies of the tags' arrays */
struct reach_input
{
	struct prism_bake_bsp3d bsp;
	struct prism_bake_clusters clusters;
	int32_t *nodes;
	float *planes;
	int16_t *leaf_clusters, *leaf_counts;
	int32_t *leaf_first, *references;
	uint32_t *pvs;
	struct prism_bake_seed *seeds;
	size_t seed_count;
	float bounds[6];
	float *water;
	int32_t water_count;
	int flight;
	/* the placed vehicles the player could fly (their places) */
	float *fighters;
	long fighter_count;
	/* the stock triangles (oriented to their vertices' normals) and the
	surface each is of */
	float *positions;
	int32_t *surfaces;
	size_t triangle_count;
	long surface_count;
	/* the static scenery's triangles (SCENERY_FLOATS each: its corners, its
	albedo, its coverage) */
	float *scenery;
	long scenery_count, scenery_room, scenery_placements;
	/* the runs of those triangles that are of a placement a mod's mesh
	draws (first triangle and count each), and their count */
	int32_t *replaced;
	long replaced_count, replaced_room;
	/* per static scenery triangle its corners' base map coordinates (6
	floats, as drawn: the model's and its shader's map scales applied) and
	its cutout's alpha map (-1 none, opaque); the alpha maps (each its
	width, height and texels, a byte each, padded to a word) and their base
	maps' tags */
	float *scenery_uv;
	int32_t *scenery_map;
	unsigned char *maps;
	long maps_bytes, maps_room, map_count;
	long map_tags[SCENERY_ALPHA_MAPS];
	/* the scenario's light fixtures in this BSP (FIXTURE_FLOATS each: the
	place, the forward axis, the colour, the intensity, the falloff and
	cutoff angles) */
	float *fixtures;
	long fixture_count;
};

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	boolean active;
	int mode, quality, foreground, exit_when_done;
	long request;
	float budget, threshold, noise_floor;
	char folder[XGPU_BAKE_PATH];
	/* the host's answers */
	uint32_t status[STATUS_WORDS];
	/* the reach's inputs: of which BSP (its address) and scene generation,
	serialised (the host's reader: xgpu_vulkan_bake.c's), and how much of it
	is sent */
	short reach_bsp;
	void const *reach_bsp_address;
	unsigned long reach_generation;
	unsigned char *reach_blob;
	unsigned long reach_bytes, reach_sent;
	boolean reach_failed, reach_ended;
	boolean logged_done;
	/* the static scenery's models a mod's mesh draws, as the reach's inputs
	last found them (scenery_copy), and that finding's number
	(prism_bake_object_static's answers kept until it changes) */
	long replaced_models[SCENERY_REPLACED_MODELS];
	long replaced_model_count;
	unsigned long replaced_generation;
} prism_bake;

/* ---------- private code */

/* the menus are up: the game is not played */
static boolean prism_bake_menus_up(
	void)
{
	return main_menu_is_active() || !game_in_progress();
}

static void settings_read(
	void)
{
	char const *value;

	if (prism_bake.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_bake.settings_read = config_changes() + 1;
	value = config_string("display.bake");
	/* ("ask", the older word, is read as "auto": port_config.c) */
	prism_bake.mode = value && !strcmp(value, "off") ? XGPU_BAKE_MODE_OFF : XGPU_BAKE_MODE_AUTO;
	value = config_string("display.bake_quality");
	/* (ultra the default; max only where it is asked for) */
	prism_bake.quality = !value ? 2 : !strcmp(value, "draft") ? 0 : !strcmp(value, "high") ? 1 :
		!strcmp(value, "max") ? 3 : 2;
	prism_bake.request = config_integer("display.bake_rebake");
	prism_bake.budget = (float)config_real("display.bake_budget");
	if (!(prism_bake.budget >= 0.25f && prism_bake.budget <= 200.0f))
	{
		prism_bake.budget = 2.0f;
	}
	prism_bake.threshold = 0.01f;
	/* (D4 amended: the `max` tier's error against 0.1 of middle grey where a
	texel's own light is less) */
	prism_bake.noise_floor = (float)config_real("debug.bake_noise_floor");
	if (!(prism_bake.noise_floor >= 0.0f && prism_bake.noise_floor <= 10.0f))
	{
		prism_bake.noise_floor = 0.1f;
	}
	prism_bake.exit_when_done = config_boolean("debug.bake_exit");
	prism_bake.foreground = prism_bake.exit_when_done || prism_bake_worker_map() != NULL;
	value = config_string("display.bake_folder");
	if (value && *value)
	{
		snprintf(prism_bake.folder, sizeof(prism_bake.folder), "%s", value);
	}
	else
	{
		snprintf(prism_bake.folder, sizeof(prism_bake.folder), "%s/prism_bake", platform_data_root());
	}

	return;
}

static void const *block_of(
	void const *base,
	long offset,
	long element_size,
	long most,
	long *count)
{
	struct tag_block const *block = (struct tag_block const *)((char const *)base + offset);

	*count = 0;
	if (!block->address || block->count <= 0 || block->count > most || element_size <= 0)
	{
		return NULL;
	}
	*count = block->count;
	return block->address;
}

static void seed_add(
	struct reach_input *input,
	void const *at,
	int kind)
{
	struct prism_bake_seed *seed;

	if (input->seed_count >= SEEDS_MOST)
	{
		return;
	}
	seed = &input->seeds[input->seed_count];
	memcpy(seed->position, at, 3 * sizeof(float));
	if (!(seed->position[0] == seed->position[0]) || !(seed->position[1] == seed->position[1]) ||
		!(seed->position[2] == seed->position[2]))
	{
		return;
	}
	seed->kind = kind;
	input->seed_count++;
}

/* each element's place (at offset in it) of one of the scenario's blocks */
static void seeds_of(
	struct reach_input *input,
	void const *scenario,
	long block_offset,
	long element_size,
	long position_offset,
	int kind)
{
	long count, index;
	char const *elements = (char const *)block_of(scenario, block_offset, element_size, 65536, &count);

	for (index = 0; elements && index < count; index++)
	{
		seed_add(input, elements + index * element_size + position_offset, kind);
	}
}

/* the scenario's places the reachable space starts from (the walk's) and
the cameras' (views) */
static void seeds_find(
	struct reach_input *input,
	void const *scenario)
{
	long count, index;
	char const *encounters;

	seeds_of(input, scenario, SCNR_PLAYERS, 0x34, 0, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_NETGAME_FLAGS, 0x94, 0, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_NETGAME_EQUIPMENT, 0x90, 0x40, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_VEHICLES, 0x78, 0x08, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_BIPEDS, 0x78, 0x08, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_WEAPONS, 0x5C, 0x08, PRISM_BAKE_SEED_WALK);
	seeds_of(input, scenario, SCNR_EQUIPMENT, 0x28, 0x08, PRISM_BAKE_SEED_WALK);
	encounters = (char const *)block_of(scenario, SCNR_ENCOUNTERS, 0xB0, 1024, &count);
	for (index = 0; encounters && index < count; index++)
	{
		char const *encounter = encounters + index * 0xB0;
		long squad_count, squad, entries, entry;
		char const *squads = (char const *)block_of(encounter, 0x80, 0xE8, 1024, &squad_count);
		char const *list;

		for (squad = 0; squads && squad < squad_count; squad++)
		{
			list = (char const *)block_of(squads + squad * 0xE8, 0xD0, 0x1C, 1024, &entries);
			for (entry = 0; list && entry < entries; entry++)
			{
				seed_add(input, list + entry * 0x1C, PRISM_BAKE_SEED_WALK);
			}
		}
		list = (char const *)block_of(encounter, 0x98, 0x18, 4096, &entries);
		for (entry = 0; list && entry < entries; entry++)
		{
			seed_add(input, list + entry * 0x18, PRISM_BAKE_SEED_VIEW);
		}
		list = (char const *)block_of(encounter, 0xA4, 0x34, 1024, &entries);
		for (entry = 0; list && entry < entries; entry++)
		{
			seed_add(input, list + entry * 0x34, PRISM_BAKE_SEED_WALK);
		}
	}
	seeds_of(input, scenario, SCNR_CUTSCENE_FLAGS, 0x5C, 0x24, PRISM_BAKE_SEED_VIEW);
	seeds_of(input, scenario, SCNR_CUTSCENE_CAMERA_POINTS, 0x68, 0x28, PRISM_BAKE_SEED_VIEW);
}

/* the places of the scenario's placed vehicles of the type the player
flies (the fighters: a Banshee), each a candidate for flight (the reach
turns flight on only where one stands where the player walks) */
static void fighters_find(
	struct reach_input *input,
	void const *scenario)
{
	long palette_count, count, index;
	char const *palette = (char const *)block_of(scenario, SCNR_VEHICLE_PALETTE, 0x30, 1024, &palette_count);
	char const *vehicles = (char const *)block_of(scenario, SCNR_VEHICLES, 0x78, 65536, &count);

	input->fighters = (float *)malloc((size_t)(count + 1) * 3 * sizeof(float));
	for (index = 0; input->fighters && palette && vehicles && index < count; index++)
	{
		short entry = *(short const *)(vehicles + index * 0x78);
		/* (struct scenario_object_datum's placement flags: one made by a
		script, not placed as the level begins, is the game's to fly) */
		word placement = *(word const *)(vehicles + index * 0x78 + 4);
		struct tag_reference const *reference;
		char const *vehicle;

		if (entry < 0 || entry >= palette_count || (placement & 1))
		{
			continue;
		}
		reference = (struct tag_reference const *)(palette + entry * 0x30);
		if (reference->index == NONE || !(vehicle = (char const *)tag_get('vehi', reference->index)))
		{
			continue;
		}
		if (*(short const *)(vehicle + VEHI_TYPE) == VEHICLE_FIGHTER)
		{
			memcpy(input->fighters + input->fighter_count * 3, vehicles + index * 0x78 + 0x08, 3 * sizeof(float));
			input->fighter_count++;
		}
	}
}

static void input_free(
	struct reach_input *input)
{
	free(input->nodes);
	free(input->planes);
	free(input->leaf_clusters);
	free(input->leaf_counts);
	free(input->leaf_first);
	free(input->references);
	free(input->pvs);
	free(input->seeds);
	free(input->water);
	free(input->positions);
	free(input->surfaces);
	free(input->fighters);
	free(input->scenery);
	free(input->replaced);
	free(input->scenery_uv);
	free(input->scenery_map);
	free(input->maps);
	free(input->fixtures);
	memset(input, 0, sizeof(*input));
}

/* a compressed vertex's normal (rasterizer_geometry's 11:11:10) */
static void normal_uncompress(
	unsigned long packed,
	float n[3])
{
	long x = (long)(packed & 0x7FF), y = (long)((packed >> 11) & 0x7FF), z = (long)((packed >> 22) & 0x3FF);

	if (x & 0x400) x -= 0x800;
	if (y & 0x400) y -= 0x800;
	if (z & 0x200) z -= 0x400;
	n[0] = ((float)x * 2.0f + 1.0f) / 2047.0f;
	n[1] = ((float)y * 2.0f + 1.0f) / 2047.0f;
	n[2] = ((float)z * 2.0f + 1.0f) / 1023.0f;
}

/* the BSP's stock render triangles, each wound to face its vertices'
normals, with its surface */
static boolean triangles_copy(
	struct reach_input *input,
	struct structure_bsp *bsp)
{
	long lightmap_index, material_index, total = 0, pass;

	for (pass = 0; pass < 2; pass++)
	{
		size_t made = 0;

		if (pass)
		{
			input->positions = (float *)malloc((size_t)(total + 1) * 9 * sizeof(float));
			input->surfaces = (int32_t *)malloc((size_t)(total + 1) * sizeof(int32_t));
			if (!input->positions || !input->surfaces)
			{
				return FALSE;
			}
		}
		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
				struct structure_lightmap);

			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
					struct structure_material);
				long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
				byte const *vertices = (byte const *)material->compressed_vertex_data.address;
				long surface;

				if (!vertices || vertex_size < 24 || material->vertices.count <= 0 || material->surface_count <= 0 ||
					material->first_surface_index < 0 ||
					material->first_surface_index + material->surface_count > bsp->surfaces.count ||
					(unsigned long)material->compressed_vertex_data.size <
					(unsigned long)material->vertices.count * (unsigned long)vertex_size)
				{
					continue;
				}
				if (!pass)
				{
					total += material->surface_count;
					continue;
				}
				for (surface = 0; surface < material->surface_count; surface++)
				{
					struct structure_surface const *triangle = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
						material->first_surface_index + surface, struct structure_surface);
					float *to = input->positions + made * 9, normal[3] = { 0.0f, 0.0f, 0.0f }, e1[3], e2[3], c[3];
					int corner, axis, valid = TRUE;

					for (corner = 0; corner < 3; corner++)
					{
						long index = (unsigned short)triangle->vertex_indices[corner];
						byte const *vertex;
						float n[3];

						if (index >= material->vertices.count)
						{
							valid = FALSE;
							break;
						}
						vertex = vertices + index * vertex_size;
						memcpy(to + corner * 3, vertex, 3 * sizeof(float));
						if (vertex_size == 32)
						{
							normal_uncompress(*(unsigned long const *)(vertex + 12), n);
						}
						else
						{
							memcpy(n, vertex + 12, sizeof(n));
						}
						for (axis = 0; axis < 3; axis++)
						{
							normal[axis] += n[axis];
						}
					}
					if (!valid)
					{
						continue;
					}
					for (axis = 0; axis < 3; axis++)
					{
						e1[axis] = to[3 + axis] - to[axis];
						e2[axis] = to[6 + axis] - to[axis];
					}
					c[0] = e1[1] * e2[2] - e1[2] * e2[1];
					c[1] = e1[2] * e2[0] - e1[0] * e2[2];
					c[2] = e1[0] * e2[1] - e1[1] * e2[0];
					if (c[0] * normal[0] + c[1] * normal[1] + c[2] * normal[2] < 0.0f)
					{
						float swap[3];

						memcpy(swap, to + 3, sizeof(swap));
						memcpy(to + 3, to + 6, sizeof(swap));
						memcpy(to + 6, swap, sizeof(swap));
					}
					input->surfaces[made] = (int32_t)(material->first_surface_index + surface);
					made++;
				}
			}
		}
		if (pass)
		{
			input->triangle_count = made;
		}
	}

	return TRUE;
}

/* ---------- the static scenery (the bake traces it as the reference's
path tracer traces the objects it draws: docs/prism_bake.md)

A placement is static as world_sdf.c's sdf_scenery_static has it: no script
can name it, it is made on every difficulty, in this BSP, and nothing moves
or destroys it (no animations, no body to damage). Its model's most detailed
geometry (each region's first permutation) is sent in the world, each
triangle wound to face its vertices' normals, with its shader's albedo (the
base map's mean colour, linear) and coverage (an alpha-tested model shader's
share of its base map's texels drawn: its leaves' cards; 1 opaque; a
transparent shader's triangles left out).

A placement whose model a mod's mesh draws (display.prism_models,
mod_meshes_model_replaced) is sent as the others are, so that the bake's
light and its geometry's hash are the stock scenery's whatever the remodels
(a bake travels without them), and its triangles' run is named after the
fixtures: the scene the frame traces (Rebaked's object pixels' sun, every
ray of Path Traced) leaves those runs out, and the object is traced as it
is drawn, its mesh's level as the frame's other objects are
(prism_bake_object_static answers FALSE for it). Before, the stock rock
stayed in the traced scene around the mesh drawn in its place: a shell of
CE's facets just outside or inside the rounded remodel that shadowed it in
patches, and flickered with each frame's jitter and sun sample. */

/* this build's model geometry, part and shader reference (models.c's, as
world_sdf.c and object_mesh.c read them) */
struct scenery_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct scenery_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

struct scenery_model_shader
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* a model shader's flags and base map (rasterizer_xbox_models.c's) */
struct scenery_shader_model
{
	struct shader shader;
	word flags;
	byte reserved[0x7A];
	struct tag_reference base_map;
};

typedef char verify_scenery_part_size[sizeof(struct scenery_part) == 0x68 ? 1 : -1];
typedef char verify_scenery_model_shader_size[sizeof(struct scenery_model_shader) == 0x20 ? 1 : -1];
typedef char verify_scenery_shader_model_base_map[offsetof(struct scenery_shader_model, base_map) == 0xA4 ? 1 : -1];

enum
{
	_scenery_part_stripped_bit = 0,
	_scenery_shader_type_model = 4,
	_scenery_shader_model_not_alpha_tested_bit = 2,
};

/* a shader's albedo (its base map's mean colour, linear; a grey where it
cannot be read) and coverage (0 transparent: left out) */
static void scenery_shader(
	long shader_tag,
	float albedo[3],
	float *coverage,
	long *alpha_map,
	float map_scale[2])
{
	struct shader *shader = shader_tag != NONE ? shader_definition_get(shader_tag) : NULL;
	long base_map = NONE;
	int channel;

	albedo[0] = albedo[1] = albedo[2] = 0.35f;
	*coverage = 1.0f;
	*alpha_map = NONE;
	map_scale[0] = map_scale[1] = 1.0f;
	if (!shader || !shader_type_is_valid_for_model(shader->base.type) || shader_type_is_transparent(shader->base.type))
	{
		*coverage = 0.0f;
		return;
	}
	if (shader->base.type == _scenery_shader_type_model)
	{
		struct scenery_shader_model const *model = (struct scenery_shader_model const *)shader;

		base_map = model->base_map.index;
		/* (its map scales, 0 none: rasterizer_xbox_models.c's map_u_scale
		and map_v_scale, at 0x9C and 0xA0) */
		{
			float u_scale, v_scale;

			memcpy(&u_scale, (byte const *)model + 0x9C, sizeof(u_scale));
			memcpy(&v_scale, (byte const *)model + 0xA0, sizeof(v_scale));
			map_scale[0] = u_scale != 0.0f && u_scale == u_scale && fabsf(u_scale) < 1.0e4f ? u_scale : 1.0f;
			map_scale[1] = v_scale != 0.0f && v_scale == v_scale && fabsf(v_scale) < 1.0e4f ? v_scale : 1.0f;
		}
		if (base_map != NONE && !TEST_FLAG(model->flags, _scenery_shader_model_not_alpha_tested_bit))
		{
			struct bitmap_group *group = bitmap_group_get(base_map);
			unsigned long const *header = group && group->bitmaps.address && group->bitmaps.count > 0 ?
				(unsigned long const *)texture_cache_port_load((unsigned long)base_map, 0) : NULL;
			struct bitmap_data const *bitmap = header ? TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data) : NULL;

			if (bitmap && bitmap->base_address)
			{
				*coverage = xgpu_texture_alpha_coverage((unsigned int)header[3], (unsigned int)header[4],
					(unsigned char const *)bitmap->base_address, 0x7F, 512);
				if (*coverage < 0.999f)
				{
					*alpha_map = base_map;
				}
			}
		}
	}
	if (base_map != NONE)
	{
		struct bitmap_group *group = bitmap_group_get(base_map);
		unsigned long const *header = group && group->bitmaps.address && group->bitmaps.count > 0 ?
			(unsigned long const *)texture_cache_port_load((unsigned long)base_map, 0) : NULL;
		struct bitmap_data const *bitmap = header ? TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data) : NULL;
		float mean[4];

		/* (the rule prism_rt.c's materials take: in the light's units the
		stored mean) */
		if (bitmap && bitmap->base_address && xgpu_texture_mean_color_as((unsigned int)header[3], (unsigned int)header[4],
			(unsigned char const *)bitmap->base_address, 64, mean, prism_rt_albedo_stored()))
		{
			for (channel = 0; channel < 3; channel++)
			{
				albedo[channel] = PIN(mean[channel], SCENERY_ALBEDO_LEAST, SCENERY_ALBEDO_MOST);
			}
		}
	}
}

/* a placement's definition if it is static and in this BSP (NONE if not) */
/* a definition that nothing moves or destroys: a model, no animations, no
body to damage */
static boolean definition_static(
	long index)
{
	struct object_definition const *definition;

	if (index == NONE || !(definition = object_definition_get(index)) || definition->object.model.index == NONE)
	{
		return FALSE;
	}
	if (definition->object.animation_graph.index != NONE)
	{
		struct animation_graph const *graph = animation_graph_definition_get(definition->object.animation_graph.index);

		if (graph && graph->animations.count > 0)
		{
			return FALSE;
		}
	}
	if (definition->object.collision_model.index != NONE)
	{
		struct collision_model const *collision = collision_model_definition_get(definition->object.collision_model.index);

		if (collision && collision->resistance.maximum_body_vitality > 0.0f)
		{
			return FALSE;
		}
	}

	return TRUE;
}

static long scenery_static(
	struct tag_block const *palette,
	struct scenario_object_datum const *placement)
{
	struct scenario_object_palette_entry const *entry;
	struct object_definition const *definition;
	real_matrix4x3 matrix;
	real_point3d offset;
	long index;

	if (placement->name_index != NONE ||
		(placement->placement_flags & ((1 << NUMBER_OF_SCENARIO_OBJECT_LOCATION_PLACEMENT_FLAGS) - 1)) ||
		placement->palette_entry_index < 0 || placement->palette_entry_index >= palette->count || !palette->address)
	{
		return NONE;
	}
	entry = TAG_BLOCK_GET_ELEMENT(palette, placement->palette_entry_index, struct scenario_object_palette_entry);
	index = entry->reference.index;
	if (!definition_static(index))
	{
		return NONE;
	}
	definition = object_definition_get(index);
	if (scenario_leaf_index_from_point(&placement->position) != NONE)
	{
		return index;
	}
	matrix4x3_rotation_from_angles(&matrix, placement->rotation.yaw, placement->rotation.pitch, placement->rotation.roll);
	matrix.position = placement->position;
	matrix4x3_transform_point(&matrix, &definition->object.bounding_offset, &offset);
	return scenario_leaf_index_from_point(&offset) != NONE ? index : NONE;
}

/* a part's drawn triangles into the input, in the world (by matrix);
FALSE if no memory */
/* a cutout's alpha map among the reach's inputs' (its index, added where
new; -1 where it cannot be read or there is no room: the card then blocks
by its coverage, as before) */
static int32_t scenery_alpha_map(
	struct reach_input *input,
	long bitmap_tag)
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	unsigned long const *header;
	unsigned long width = 0, height = 0;
	long index, start, need;

	if (bitmap_tag == NONE)
	{
		return -1;
	}
	for (index = 0; index < input->map_count; index++)
	{
		if (input->map_tags[index] == bitmap_tag)
		{
			return input->map_tags[index] == NONE ? -1 : (int32_t)index;
		}
	}
	if (input->map_count >= SCENERY_ALPHA_MAPS || !(group = bitmap_group_get(bitmap_tag)) ||
		!group->bitmaps.address || group->bitmaps.count <= 0)
	{
		return -1;
	}
	header = (unsigned long const *)texture_cache_port_load((unsigned long)bitmap_tag, 0);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (!header || !bitmap->base_address)
	{
		return -1;
	}
	start = input->maps_bytes;
	need = start + 8 + SCENERY_ALPHA_SIDE * SCENERY_ALPHA_SIDE + 4;
	if (need > input->maps_room)
	{
		long room = MAX(input->maps_room * 2, need + SCENERY_ALPHA_SIDE * SCENERY_ALPHA_SIDE * 4);
		unsigned char *grown = (unsigned char *)realloc(input->maps, (size_t)room);

		if (!grown)
		{
			return -1;
		}
		input->maps = grown;
		input->maps_room = room;
	}
	if (!xgpu_texture_alpha_map((unsigned int)header[3], (unsigned int)header[4],
		(unsigned char const *)bitmap->base_address, SCENERY_ALPHA_SIDE, input->maps + start + 8, &width, &height) ||
		!width || !height)
	{
		return -1;
	}
	{
		int32_t size[2];

		size[0] = (int32_t)width;
		size[1] = (int32_t)height;
		memcpy(input->maps + start, size, 8);
	}
	/* (a whole number of words) */
	need = (long)(width * height);
	while (need & 3)
	{
		input->maps[start + 8 + need++] = 0;
	}
	input->maps_bytes = start + 8 + need;
	input->map_tags[input->map_count] = bitmap_tag;
	return (int32_t)input->map_count++;
}

static boolean scenery_part_copy(
	struct reach_input *input,
	struct model const *model,
	struct scenery_part const *part,
	real_matrix4x3 const *matrix)
{
	struct vertex_buffer const *vertex_buffer = &part->vertex_buffer;
	struct triangle_buffer const *triangle_buffer = &part->triangle_buffer;
	byte *vertices = NULL;
	BYTE *indices = NULL;
	long stride, index_count, index, alpha_tag;
	float albedo[3], coverage, map_scale[2], uv_scale[2];
	int32_t alpha_map;
	boolean good = TRUE;

	if (TEST_FLAG(part->flags, _scenery_part_stripped_bit) || part->shader_index < 0 ||
		part->shader_index >= model->shaders.count || !model->shaders.address)
	{
		return TRUE;
	}
	scenery_shader(TAG_BLOCK_GET_ELEMENT(&model->shaders, part->shader_index, struct scenery_model_shader)->shader.index,
		albedo, &coverage, &alpha_tag, map_scale);
	/* (the coordinates as drawn: the model's base map scale (0 none) by the
	shader's) */
	uv_scale[0] = (model->base_map_scale.i != 0.0f ? model->base_map_scale.i : 1.0f) * map_scale[0];
	uv_scale[1] = (model->base_map_scale.j != 0.0f ? model->base_map_scale.j : 1.0f) * map_scale[1];
	alpha_map = coverage > 0.0f && coverage < 0.999f ? scenery_alpha_map(input, alpha_tag) : -1;
	if (coverage <= 0.0f || !vertex_buffer->hardware_format || vertex_buffer->count <= 0 ||
		!triangle_buffer->hardware_format || triangle_buffer->count <= 0)
	{
		return TRUE;
	}
	if (vertex_buffer->type == _rasterizer_vertex_type_model_compressed)
	{
		stride = sizeof(struct model_vertex_compressed);
	}
	else if (vertex_buffer->type == _rasterizer_vertex_type_model_uncompressed)
	{
		stride = sizeof(struct model_vertex_uncompressed);
	}
	else
	{
		return TRUE;
	}
	if (triangle_buffer->type == _triangle_buffer_type_precompiled_strip)
	{
		index_count = triangle_buffer->count + 2;
	}
	else if (triangle_buffer->type == _triangle_buffer_type_triangles)
	{
		index_count = triangle_buffer->count * 3;
	}
	else
	{
		return TRUE;
	}
	IDirect3DVertexBuffer8_Lock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format, 0, 0, &vertices,
		D3DLOCK_READONLY);
	IDirect3DIndexBuffer8_Lock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format, 0, 0, &indices,
		D3DLOCK_READONLY);
	if (vertices && indices)
	{
		unsigned short const *list = (unsigned short const *)indices;
		long step = triangle_buffer->type == _triangle_buffer_type_precompiled_strip ? 1 : 3;

		for (index = 0; index + 2 < index_count && input->scenery_count < SCENERY_MOST; index += step)
		{
			unsigned short corners[3];
			float *out, *uv, normal[3] = { 0.0f, 0.0f, 0.0f }, e1[3], e2[3], c[3];
			int corner, axis, finite = TRUE;

			corners[0] = list[index];
			corners[1] = list[index + 1];
			corners[2] = list[index + 2];
			/* (a strip's turns: its degenerate triangles) */
			if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2] ||
				corners[0] >= vertex_buffer->count || corners[1] >= vertex_buffer->count ||
				corners[2] >= vertex_buffer->count)
			{
				continue;
			}
			if (input->scenery_count >= input->scenery_room)
			{
				long room = input->scenery_room ? input->scenery_room * 2 : 65536;
				float *grown = (float *)realloc(input->scenery, (size_t)room * SCENERY_FLOATS * sizeof(float));
				float *grown_uv = (float *)realloc(input->scenery_uv, (size_t)room * 6 * sizeof(float));
				int32_t *grown_map;

				if (grown)
				{
					input->scenery = grown;
				}
				if (grown_uv)
				{
					input->scenery_uv = grown_uv;
				}
				grown_map = (int32_t *)realloc(input->scenery_map, (size_t)room * sizeof(int32_t));
				if (grown_map)
				{
					input->scenery_map = grown_map;
				}
				if (!grown || !grown_uv || !grown_map)
				{
					good = FALSE;
					break;
				}
				input->scenery_room = room;
			}
			out = input->scenery + input->scenery_count * SCENERY_FLOATS;
			uv = input->scenery_uv + input->scenery_count * 6;
			for (corner = 0; corner < 3; corner++)
			{
				byte const *vertex = vertices + corners[corner] * stride;
				real_point3d point;
				real_vector3d n, world_n;

				matrix4x3_transform_point(matrix, (real_point3d const *)vertex, &point);
				if (stride == sizeof(struct model_vertex_compressed))
				{
					short const *t = (short const *)(vertex + 24);

					uv[corner * 2] = ((float)t[0] * 2.0f + 1.0f) * (1.0f / 65535.0f) * uv_scale[0];
					uv[corner * 2 + 1] = ((float)t[1] * 2.0f + 1.0f) * (1.0f / 65535.0f) * uv_scale[1];
				}
				else
				{
					float t[2];

					memcpy(t, vertex + 48, sizeof(t));
					uv[corner * 2] = t[0] * uv_scale[0];
					uv[corner * 2 + 1] = t[1] * uv_scale[1];
				}
				out[corner * 3] = point.x;
				out[corner * 3 + 1] = point.y;
				out[corner * 3 + 2] = point.z;
				if (stride == sizeof(struct model_vertex_compressed))
				{
					float u[3];

					normal_uncompress(*(unsigned long const *)(vertex + 12), u);
					n.i = u[0];
					n.j = u[1];
					n.k = u[2];
				}
				else
				{
					memcpy(&n, vertex + 12, sizeof(n));
				}
				matrix4x3_transform_vector(matrix, &n, &world_n);
				normal[0] += world_n.i;
				normal[1] += world_n.j;
				normal[2] += world_n.k;
				for (axis = 0; axis < 3; axis++)
				{
					float v = out[corner * 3 + axis];

					finite = finite && v == v && fabsf(v) < 1.0e6f;
				}
			}
			if (!finite)
			{
				continue;
			}
			for (axis = 0; axis < 3; axis++)
			{
				e1[axis] = out[3 + axis] - out[axis];
				e2[axis] = out[6 + axis] - out[axis];
			}
			c[0] = e1[1] * e2[2] - e1[2] * e2[1];
			c[1] = e1[2] * e2[0] - e1[0] * e2[2];
			c[2] = e1[0] * e2[1] - e1[1] * e2[0];
			if (c[0] * normal[0] + c[1] * normal[1] + c[2] * normal[2] < 0.0f)
			{
				float swap[3];

				memcpy(swap, out + 3, sizeof(swap));
				memcpy(out + 3, out + 6, sizeof(swap));
				memcpy(out + 6, swap, sizeof(swap));
				memcpy(swap, uv + 2, 2 * sizeof(float));
				memcpy(uv + 2, uv + 4, 2 * sizeof(float));
				memcpy(uv + 4, swap, 2 * sizeof(float));
			}
			memcpy(out + 9, albedo, sizeof(albedo));
			out[12] = coverage;
			input->scenery_map[input->scenery_count] = alpha_map;
			input->scenery_count++;
		}
	}
	if (vertices)
	{
		IDirect3DVertexBuffer8_Unlock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format);
	}
	if (indices)
	{
		IDirect3DIndexBuffer8_Unlock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format);
	}
	return good;
}

/* a placement's run of the static scenery's triangles, its model a mod's
mesh draws: named for the frame's traced scene to leave out, its model kept
for prism_bake_object_static (past SCENERY_REPLACED_MODELS models the rest
stay as they are); FALSE if no memory */
static boolean scenery_replaced_add(
	struct reach_input *input,
	long model_index,
	long first,
	long count)
{
	long index;

	for (index = 0; index < prism_bake.replaced_model_count && prism_bake.replaced_models[index] != model_index; index++)
	{
	}
	if (index == prism_bake.replaced_model_count)
	{
		if (index == SCENERY_REPLACED_MODELS)
		{
			return TRUE;
		}
		prism_bake.replaced_models[prism_bake.replaced_model_count++] = model_index;
	}
	if (input->replaced_count >= input->replaced_room)
	{
		long room = input->replaced_room ? input->replaced_room * 2 : 256;
		int32_t *grown = (int32_t *)realloc(input->replaced, (size_t)room * 2 * sizeof(int32_t));

		if (!grown)
		{
			return FALSE;
		}
		input->replaced = grown;
		input->replaced_room = room;
	}
	input->replaced[input->replaced_count * 2] = (int32_t)first;
	input->replaced[input->replaced_count * 2 + 1] = (int32_t)count;
	input->replaced_count++;
	return TRUE;
}

/* the static scenery's triangles (the scenery's, the controls' and the
light fixtures' placements; not the machines': doors and lifts move, and
Tool's radiosity never had them, so they neither block nor reflect the
bake's light); FALSE if no memory */
static boolean scenery_copy(
	struct reach_input *input,
	struct scenario *scenario)
{
	struct tag_block const *placements[4], *palettes[4];
	long sizes[4] = { 0x48, 0x40, 0x40, 0x58 }, index;
	long left_seen[16], left_count = 0;
	int kind;

	placements[0] = &scenario->scenery;
	palettes[0] = &scenario->scenery_palette;
	placements[1] = &scenario->machines;
	palettes[1] = &scenario->machine_palette;
	placements[2] = &scenario->controls;
	palettes[2] = &scenario->control_palette;
	placements[3] = &scenario->light_fixtures;
	palettes[3] = &scenario->light_fixtures_palette;
	prism_bake.replaced_model_count = 0;
	prism_bake.replaced_generation++;
	/* (HALO_BAKE_NO_SCENERY, a diagnostic: none, the structure alone, as the
	path tracer's reference traces it with HALO_RT_NO_OBJECTS, for the
	bake's gate on equal scenes) */
	if (getenv("HALO_BAKE_NO_SCENERY"))
	{
		return TRUE;
	}
	for (kind = 0; kind < 4; kind++)
	{
		if (kind == 1)
		{
			continue;
		}
		for (index = 0; placements[kind]->address && index < placements[kind]->count; index++)
		{
			struct scenario_object_datum const *placement = (struct scenario_object_datum const *)
				((byte const *)placements[kind]->address + index * sizes[kind]);
			long definition_index = scenery_static(palettes[kind], placement);
			struct model const *model;
			real_matrix4x3 matrix;
			short region_index, part_index;
			long model_index, first = input->scenery_count;

			/* (each scenery model left to the frame's moving objects, said
			once a bake: its tag and why, an animation graph's animations, a
			body to break, or a name or location flag on its placement) */
			if (definition_index == NONE && kind == 0 && palettes[0]->address &&
				placement->palette_entry_index >= 0 && placement->palette_entry_index < palettes[0]->count)
			{
				long left = TAG_BLOCK_GET_ELEMENT(palettes[0], placement->palette_entry_index,
					struct scenario_object_palette_entry)->reference.index;
				long seen_index;

				for (seen_index = 0; seen_index < left_count && left_seen[seen_index] != left; seen_index++)
				{
				}
				if (left != NONE && seen_index == left_count && left_count < 16)
				{
					char const *name = tag_get_name(left);

					left_seen[left_count++] = left;
					platform_log("prism bake: the scenery %s is not static (%s): the frame's moving objects carry it",
						name ? name : "?", !definition_static(left) ? "animated or breakable" : "a named or "
						"location-flagged placement, or one outside this BSP");
				}
			}

			model_index = definition_index != NONE ? object_definition_get(definition_index)->object.model.index : NONE;
			if (definition_index == NONE || !(model = model_definition_get(model_index)) || !model->regions.address ||
				!model->geometries.address)
			{
				continue;
			}
			matrix4x3_rotation_from_angles(&matrix, placement->rotation.yaw, placement->rotation.pitch,
				placement->rotation.roll);
			matrix.position = placement->position;
			input->scenery_placements++;
			for (region_index = 0; region_index < model->regions.count; region_index++)
			{
				struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index,
					struct model_region);
				struct model_region_permutation const *permutation;
				struct scenery_geometry const *geometry;
				short level, geometry_index = NONE;

				if (!region->permutations.address || region->permutations.count <= 0)
				{
					continue;
				}
				permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations, 0, struct model_region_permutation);
				for (level = SCENERY_DETAIL_LEVELS - 1; level >= 0 && geometry_index == NONE; level--)
				{
					short g = permutation->geometry_indices[level];

					geometry_index = g >= 0 && g < model->geometries.count ? g : NONE;
				}
				if (geometry_index == NONE)
				{
					continue;
				}
				geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct scenery_geometry);
				for (part_index = 0; geometry->parts.address && part_index < geometry->parts.count &&
					part_index < MAXIMUM_PARTS_PER_MODEL_GEOMETRY; part_index++)
				{
					if (!scenery_part_copy(input, model, TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
						struct scenery_part), &matrix))
					{
						return FALSE;
					}
				}
			}
			if (input->scenery_count > first && mod_meshes_model_replaced(model_index) &&
				!scenery_replaced_add(input, model_index, first, input->scenery_count - first))
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

/* the scenario's light fixtures in this BSP, as placed (CE's radiosity's
point and spot lights: the bake's, whatever the game has made live) */
static void fixtures_copy(
	struct reach_input *input,
	struct scenario *scenario)
{
	long index;

	input->fixtures = (float *)malloc((size_t)(scenario->light_fixtures.count + 1) * FIXTURE_FLOATS * sizeof(float));
	for (index = 0; input->fixtures && scenario->light_fixtures.address && index < scenario->light_fixtures.count; index++)
	{
		/* (a placement's bytes: its object's (the place at 0x08, the angles
		at 0x14), its device's, then the colour at 0x30, the intensity, the
		falloff and cutoff angles: device_light_fixtures.h's struct
		scenario_light_fixture_datum, SCNR_LIGHT_FIXTURE bytes) */
		byte const *placement = (byte const *)scenario->light_fixtures.address + index * SCNR_LIGHT_FIXTURE;
		struct scenario_object_datum const *object = (struct scenario_object_datum const *)placement;
		float const *light = (float const *)(placement + 0x30);
		float *to = input->fixtures + input->fixture_count * FIXTURE_FLOATS;
		real_matrix4x3 matrix;

		if (scenario_leaf_index_from_point(&object->position) == NONE)
		{
			continue;
		}
		matrix4x3_rotation_from_angles(&matrix, object->rotation.yaw, object->rotation.pitch, object->rotation.roll);
		to[0] = object->position.x;
		to[1] = object->position.y;
		to[2] = object->position.z;
		to[3] = matrix.forward.i;
		to[4] = matrix.forward.j;
		to[5] = matrix.forward.k;
		memcpy(to + 6, light, 6 * sizeof(float));
		input->fixture_count++;
	}
}

/* the reach's inputs copied from the BSP and the scenario (the game's
thread) */
static boolean input_copy(
	struct reach_input *input,
	struct structure_bsp *bsp)
{
	struct collision_bsp *collision;
	struct scenario *scenario = global_scenario_get();
	long index, count;

	memset(input, 0, sizeof(*input));
	if (!scenario || bsp->collision_bsp.count < 1 || !bsp->collision_bsp.address)
	{
		return FALSE;
	}
	collision = TAG_BLOCK_GET_ELEMENT(&bsp->collision_bsp, 0, struct collision_bsp);
	input->bsp.node_count = collision->bsp3d.nodes.count;
	input->bsp.plane_count = collision->bsp3d.planes.count;
	input->bsp.leaf_count = collision->leaves.count;
	input->nodes = (int32_t *)malloc((size_t)(input->bsp.node_count + 1) * 12);
	input->planes = (float *)malloc((size_t)(input->bsp.plane_count + 1) * 16);
	if (!input->nodes || !input->planes || input->bsp.node_count < 0 || input->bsp.plane_count < 0)
	{
		return FALSE;
	}
	if (input->bsp.node_count)
	{
		memcpy(input->nodes, collision->bsp3d.nodes.address, (size_t)input->bsp.node_count * 12);
	}
	if (input->bsp.plane_count)
	{
		memcpy(input->planes, collision->bsp3d.planes.address, (size_t)input->bsp.plane_count * 16);
	}
	input->bsp.nodes = input->nodes;
	input->bsp.planes = input->planes;
	/* the clusters: each leaf's, the surfaces' references, the PVS */
	count = bsp->leaves.count;
	input->leaf_clusters = (int16_t *)malloc((size_t)(count + 1) * 2);
	input->leaf_counts = (int16_t *)malloc((size_t)(count + 1) * 2);
	input->leaf_first = (int32_t *)malloc((size_t)(count + 1) * 4);
	input->references = (int32_t *)malloc((size_t)(bsp->surface_references.count + 1) * 4);
	if (!input->leaf_clusters || !input->leaf_counts || !input->leaf_first || !input->references)
	{
		return FALSE;
	}
	for (index = 0; index < count; index++)
	{
		struct structure_leaf *leaf = TAG_BLOCK_GET_ELEMENT(&bsp->leaves, index, struct structure_leaf);

		input->leaf_clusters[index] = leaf->cluster_index;
		input->leaf_counts[index] = leaf->surface_reference_count;
		input->leaf_first[index] = leaf->first_surface_reference_index;
	}
	for (index = 0; index < bsp->surface_references.count; index++)
	{
		input->references[index] = *(long const *)((char const *)bsp->surface_references.address + index * 8);
	}
	input->clusters.leaf_clusters = input->leaf_clusters;
	input->clusters.leaf_count = (int32_t)count;
	input->clusters.leaf_first_reference = input->leaf_first;
	input->clusters.leaf_reference_count = input->leaf_counts;
	input->clusters.reference_surfaces = input->references;
	input->clusters.reference_count = (int32_t)bsp->surface_references.count;
	input->clusters.cluster_count = (int32_t)bsp->clusters.count;
	input->clusters.pvs_words = (size_t)((bsp->clusters.count + 31) / 32);
	if (bsp->cluster_data.address && bsp->clusters.count > 0 &&
		(size_t)bsp->cluster_data.size >= (size_t)bsp->clusters.count * input->clusters.pvs_words * 4)
	{
		size_t bytes = (size_t)bsp->clusters.count * input->clusters.pvs_words * 4;

		input->pvs = (uint32_t *)malloc(bytes);
		if (input->pvs)
		{
			memcpy(input->pvs, bsp->cluster_data.address, bytes);
		}
	}
	input->clusters.pvs = input->pvs;
	/* the water: the fog planes with a material */
	input->water = (float *)malloc((size_t)(bsp->fog_planes.count + 1) * 16);
	for (index = 0; input->water && index < bsp->fog_planes.count; index++)
	{
		struct structure_fog_plane *plane = TAG_BLOCK_GET_ELEMENT(&bsp->fog_planes, index, struct structure_fog_plane);

		if (plane->runtime_material_type >= 0)
		{
			memcpy(input->water + input->water_count * 4, &plane->plane, 16);
			input->water_count++;
		}
	}
	input->bounds[0] = bsp->world_bounds.x0;
	input->bounds[1] = bsp->world_bounds.x1;
	input->bounds[2] = bsp->world_bounds.y0;
	input->bounds[3] = bsp->world_bounds.y1;
	input->bounds[4] = bsp->world_bounds.z0;
	input->bounds[5] = bsp->world_bounds.z1;
	input->seeds = (struct prism_bake_seed *)malloc(SEEDS_MOST * sizeof(struct prism_bake_seed));
	if (!input->seeds)
	{
		return FALSE;
	}
	seeds_find(input, scenario);
	fighters_find(input, scenario);
	input->surface_count = bsp->surfaces.count;
	if (!scenery_copy(input, scenario))
	{
		return FALSE;
	}
	fixtures_copy(input, scenario);

	return triangles_copy(input, bsp);
}

/* ---------- the reach's inputs serialised (little endian words; the
host's reach_read in xgpu_vulkan_bake.c reads them):
	"PBSC", version 3
	world bounds: 6 floats (x0 x1 y0 y1 z0 z1)
	fighters: count, then 3 floats each
	water planes: count, then 4 floats each
	bsp3d: node count, nodes (3 int32 each); plane count, planes (4 floats);
		collision leaf count
	structure leaves: count, then (cluster int16, reference count int16,
		first reference int32)
	surface references: count, then surface int32
	clusters: count, pvs words per cluster, the pvs (count x words uint32)
	seeds: count, then (kind int32, position 3 floats)
	surfaces: count
	triangles: count, then (surface int32, positions 9 floats); oriented to
		their vertices' normals
	(version 4) the static scenery: count, then (positions 9 floats, albedo 3
		floats, coverage float); oriented to their vertices' normals
	the light fixtures in this BSP: count, then (place 3 floats, forward 3,
		colour 3, intensity, falloff and cutoff angles)
	the static scenery's runs drawn by a mod's mesh: count, then (first
		triangle, triangle count); after the inputs the reach's file and
		the bake's key are made of (the host's hash ends before it), and
		absent from an older game's, which the host reads as none
	the static scenery's base map coordinates and cutouts' alpha maps: the
		count (the scenery's, 0 none), then per triangle its corners'
		coordinates (6 floats, as drawn), then per triangle its alpha map
		(int32, -1 none); the maps' count and their words, then each map
		(width, height int32, its alpha a byte a texel, padded to a word).
		After the runs (absent from an older game's: the cards then block by
		their coverage); in the bake's geometry hash, not the reach's */

struct blob
{
	unsigned char *bytes;
	unsigned long size, capacity;
	boolean failed;
};

static void blob_put(
	struct blob *blob,
	void const *data,
	unsigned long size)
{
	if (blob->failed)
	{
		return;
	}
	if (blob->size + size > blob->capacity)
	{
		unsigned long capacity = blob->capacity ? blob->capacity * 2 : 65536;
		unsigned char *grown;

		while (capacity < blob->size + size)
		{
			capacity *= 2;
		}
		grown = (unsigned char *)realloc(blob->bytes, capacity);
		if (!grown)
		{
			blob->failed = TRUE;
			return;
		}
		blob->bytes = grown;
		blob->capacity = capacity;
	}
	memcpy(blob->bytes + blob->size, data, size);
	blob->size += size;
}

static void blob_word(
	struct blob *blob,
	long value)
{
	int32_t word = (int32_t)value;

	blob_put(blob, &word, 4);
}

static unsigned char *reach_serialise(
	struct reach_input const *input,
	unsigned long *bytes)
{
	struct blob blob;
	long index;

	memset(&blob, 0, sizeof(blob));
	blob_put(&blob, "PBSC", 4);
	blob_word(&blob, 4);
	blob_put(&blob, input->bounds, 24);
	blob_word(&blob, input->fighter_count);
	blob_put(&blob, input->fighters, (unsigned long)input->fighter_count * 12);
	blob_word(&blob, input->water_count);
	blob_put(&blob, input->water, (unsigned long)input->water_count * 16);
	blob_word(&blob, input->bsp.node_count);
	blob_put(&blob, input->nodes, (unsigned long)input->bsp.node_count * 12);
	blob_word(&blob, input->bsp.plane_count);
	blob_put(&blob, input->planes, (unsigned long)input->bsp.plane_count * 16);
	blob_word(&blob, input->bsp.leaf_count);
	blob_word(&blob, input->clusters.leaf_count);
	for (index = 0; index < input->clusters.leaf_count; index++)
	{
		blob_put(&blob, &input->leaf_clusters[index], 2);
		blob_put(&blob, &input->leaf_counts[index], 2);
		blob_put(&blob, &input->leaf_first[index], 4);
	}
	blob_word(&blob, input->clusters.reference_count);
	blob_put(&blob, input->references, (unsigned long)input->clusters.reference_count * 4);
	blob_word(&blob, input->clusters.cluster_count);
	blob_word(&blob, input->pvs ? (long)input->clusters.pvs_words : 0);
	if (input->pvs)
	{
		blob_put(&blob, input->pvs, (unsigned long)input->clusters.cluster_count * input->clusters.pvs_words * 4);
	}
	blob_word(&blob, (long)input->seed_count);
	for (index = 0; index < (long)input->seed_count; index++)
	{
		blob_word(&blob, input->seeds[index].kind);
		blob_put(&blob, input->seeds[index].position, 12);
	}
	blob_word(&blob, input->surface_count);
	blob_word(&blob, (long)input->triangle_count);
	for (index = 0; index < (long)input->triangle_count; index++)
	{
		blob_word(&blob, input->surfaces[index]);
		blob_put(&blob, input->positions + index * 9, 36);
	}
	blob_word(&blob, input->scenery_count);
	blob_put(&blob, input->scenery, (unsigned long)input->scenery_count * SCENERY_FLOATS * 4);
	blob_word(&blob, input->fixture_count);
	blob_put(&blob, input->fixtures, (unsigned long)input->fixture_count * FIXTURE_FLOATS * 4);
	blob_word(&blob, input->replaced_count);
	blob_put(&blob, input->replaced, (unsigned long)input->replaced_count * 8);
	blob_word(&blob, input->scenery_uv && input->scenery_map ? input->scenery_count : 0);
	if (input->scenery_uv && input->scenery_map)
	{
		blob_put(&blob, input->scenery_uv, (unsigned long)input->scenery_count * 24);
		blob_put(&blob, input->scenery_map, (unsigned long)input->scenery_count * 4);
	}
	blob_word(&blob, input->map_count);
	blob_word(&blob, input->maps_bytes / 4);
	blob_put(&blob, input->maps, (unsigned long)input->maps_bytes);
	/* (a whole number of words) */
	while (blob.size & 3)
	{
		blob_put(&blob, "", 1);
	}
	if (blob.failed)
	{
		free(blob.bytes);
		return NULL;
	}
	*bytes = blob.size;
	return blob.bytes;
}

static void reach_reset(
	void)
{
	free(prism_bake.reach_blob);
	prism_bake.reach_blob = NULL;
	prism_bake.reach_bytes = prism_bake.reach_sent = 0;
	prism_bake.reach_bsp = NONE;
	prism_bake.reach_bsp_address = NULL;
	prism_bake.reach_generation = 0;
	prism_bake.reach_failed = prism_bake.reach_ended = FALSE;
}

static void control_fill(
	struct xgpu_stream_prism_bake *bake,
	int phase)
{
	char const *map = cache_file_loaded_map_name(), *part;

	memset(bake, 0, sizeof(*bake));
	bake->phase = (uint32_t)phase;
	bake->generation = (uint32_t)prism_scene_static_generation();
	bake->mode = (uint32_t)prism_bake.mode;
	bake->quality = (uint32_t)prism_bake.quality;
	bake->request = (uint32_t)prism_bake.request;
	bake->flags = (prism_bake.foreground ? XGPU_BAKE_FLAG_FOREGROUND : 0) |
		(prism_bake_menus_up() ? XGPU_BAKE_FLAG_MENU : 0) |
		(prism_bake.reach_ended ? XGPU_BAKE_FLAG_REACH : 0);
	bake->map_checksum = (uint32_t)cache_files_get_checksum();
	bake->bsp_index = global_structure_bsp_index_get();
	bake->folder = (uint32_t)(unsigned long)prism_bake.folder;
	bake->status = (uint32_t)(unsigned long)prism_bake.status;
	bake->budget_milliseconds = prism_bake_worker_map() ? prism_bake_worker_budget() : prism_bake.foreground ? 60.0f :
		prism_bake_menus_up() ? 30.0f : prism_bake.budget;
	bake->threshold = prism_bake.threshold;
	bake->noise_floor = prism_bake.noise_floor;
	if (!map)
	{
		map = "";
	}
	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
		{
			map = part + 1;
		}
	}
	snprintf(bake->map_name, sizeof(bake->map_name), "%s", map);
}

/* the reach's inputs copied from the BSP drawn once its scene is whole,
then sent a slice a frame (again for a new scene of the same BSP) */
static void reach_frame(
	void)
{
	short bsp_index = global_structure_bsp_index_get();
	struct structure_bsp *bsp = bsp_index != NONE ? global_structure_bsp_get() : NULL;
	unsigned long generation = prism_scene_static_generation();
	struct xgpu_stream_prism_bake bake;

	if (!bsp || !generation)
	{
		return;
	}
	if (prism_bake.reach_blob && (bsp_index != prism_bake.reach_bsp || (void const *)bsp != prism_bake.reach_bsp_address))
	{
		reach_reset();
	}
	if (generation != prism_bake.reach_generation && prism_bake.reach_blob)
	{
		/* (the same BSP's new scene: its inputs sent again) */
		prism_bake.reach_generation = generation;
		prism_bake.reach_sent = 0;
		prism_bake.reach_ended = FALSE;
	}
	if (!prism_bake.reach_blob && !prism_bake.reach_failed)
	{
		struct reach_input input;

		if (!prism_scene_static_ended())
		{
			return;
		}
		prism_bake.reach_bsp = bsp_index;
		prism_bake.reach_bsp_address = bsp;
		prism_bake.reach_generation = generation;
		if (!input_copy(&input, bsp) || !(prism_bake.reach_blob = reach_serialise(&input, &prism_bake.reach_bytes)))
		{
			prism_bake.reach_failed = TRUE;
			platform_log("prism bake: bsp %d: its reachable space cannot be found (no collision BSP, or no memory)",
				(int)bsp_index);
		}
		else
		{
			platform_log("prism bake: bsp %d: the reach's inputs (%lu seeds, %lu triangles, %d nodes, %ld fighters; the static "
				"scenery's %ld placements, %ld triangles, %ld placements of %ld models drawn by a mod's mesh): %lu KB",
				(int)bsp_index, (unsigned long)input.seed_count,
				(unsigned long)input.triangle_count, (int)input.bsp.node_count, input.fighter_count,
				input.scenery_placements, input.scenery_count, input.replaced_count, prism_bake.replaced_model_count,
				prism_bake.reach_bytes / 1024);
		}
		input_free(&input);
		return;
	}
	if (!prism_bake.reach_blob || prism_bake.reach_ended)
	{
		return;
	}
	if (prism_bake.reach_sent < prism_bake.reach_bytes)
	{
		unsigned long words = (prism_bake.reach_bytes - prism_bake.reach_sent) / 4;

		if (words > XGPU_BAKE_SLICE)
		{
			words = XGPU_BAKE_SLICE;
		}
		control_fill(&bake, XGPU_BAKE_REACH_INPUT);
		bake.first = (uint32_t)(prism_bake.reach_sent / 4);
		bake.count = (uint32_t)words;
		bake.total = (uint32_t)(prism_bake.reach_bytes / 4);
		xgpu_backend_prism_bake(&bake, prism_bake.reach_blob + prism_bake.reach_sent, words * 4);
		prism_bake.reach_sent += words * 4;
		return;
	}
	control_fill(&bake, XGPU_BAKE_REACH_END);
	xgpu_backend_prism_bake(&bake, NULL, 0);
	prism_bake.reach_ended = TRUE;
}

/* ---------- public code */

/* the v4 modes that read a bake, where the renderer can make one */
boolean prism_bake_wanted(
	void)
{
	return prism_deferred_raster() && halo_prism_rt_available();
}

/* the host's status word (XGPU_BAKE_STATUS_*), 0 where no bake runs */
unsigned long prism_bake_status(
	int word)
{
	if (word < 0 || word >= STATUS_WORDS)
	{
		return 0;
	}
	return __atomic_load_n(&prism_bake.status[word], __ATOMIC_ACQUIRE);
}

float prism_bake_status_real(
	int word)
{
	unsigned long bits = prism_bake_status(word);
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

/* each frame, after SCENE_STATIC and display.prism_rt's scene
(world_ao_frame_begin) */
void prism_bake_frame(
	void)
{
	struct xgpu_stream_prism_bake bake;

	if (render_enumerating)
	{
		return;
	}
	settings_read();
	if (!prism_bake_wanted())
	{
		if (prism_bake.active)
		{
			reach_reset();
			prism_bake.active = FALSE;
		}
		return;
	}
	prism_bake.active = TRUE;
	reach_frame();
	control_fill(&bake, XGPU_BAKE_CONTROL);
	xgpu_backend_prism_bake(&bake, NULL, 0);
	/* debug.bake_exit: the game quits once the BSP's bake is done (as
	debug.exit_after ends a run, with exit) */
	if (prism_bake.exit_when_done)
	{
		unsigned long state = prism_bake_status(XGPU_BAKE_STATUS_STATE);

		if ((state == XGPU_BAKE_STATE_DONE || state == XGPU_BAKE_STATE_FAILED) &&
			(long)prism_bake_status(XGPU_BAKE_STATUS_BSP) == (long)global_structure_bsp_index_get() &&
			prism_bake_status(XGPU_BAKE_STATUS_GENERATION) == (unsigned long)prism_scene_static_generation())
		{
			if (!prism_bake.logged_done)
			{
				platform_log("prism bake: debug.bake_exit: bsp %d's bake %s in %.1f s (%.1f MB): quitting",
					(int)global_structure_bsp_index_get(), state == XGPU_BAKE_STATE_DONE ? "done" : "failed",
					(double)prism_bake_status_real(XGPU_BAKE_STATUS_SECONDS),
					(double)prism_bake_status_real(XGPU_BAKE_STATUS_MEGABYTES));
				prism_bake.logged_done = TRUE;
			}
			exit(state == XGPU_BAKE_STATE_DONE ? 0 : 3);
		}
	}
}

void prism_bake_map_unloaded(
	void)
{
	reach_reset();
	prism_bake.replaced_model_count = 0;
	prism_bake.replaced_generation++;
}

/* port: whether an object is one the bake's static scenery holds (a
placement of scenery, a control or a light fixture that scenery_static
takes, the object at its placement and of its definition), for the path
tracer's moving objects (prism_deferred.c's SCENE_INSTANCE flags): each
object's answer kept by its datum index until the scenario changes, or the
static scenery's models a mod's mesh draws are found again. Such a model's
placement is FALSE: the frame's traced scene leaves its stock triangles out
(scenery_copy) and traces it as drawn, as a moving object */
#define OBJECT_STATIC_KEPT 2048
boolean prism_bake_object_static(
	long object_index)
{
	static long kept_index[OBJECT_STATIC_KEPT];
	static char kept_answer[OBJECT_STATIC_KEPT];
	static void const *kept_scenario;
	static unsigned long kept_generation;
	struct scenario *scenario = global_scenario_get();
	struct object_datum *object = object_try_and_get_and_verify_type(object_index, _object_mask_all);
	struct tag_block const *placements, *palette;
	long size, index, slot;
	boolean found = FALSE;

	if (!object || !scenario)
	{
		return FALSE;
	}
	if (kept_scenario != scenario || kept_generation != prism_bake.replaced_generation)
	{
		memset(kept_index, 0xff, sizeof(kept_index));
		kept_scenario = scenario;
		kept_generation = prism_bake.replaced_generation;
	}
	slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index) % OBJECT_STATIC_KEPT;
	if (kept_index[slot] == object_index)
	{
		return kept_answer[slot] != 0;
	}
	switch (object->object.type)
	{
	case _object_type_scenery:
		placements = &scenario->scenery;
		palette = &scenario->scenery_palette;
		size = 0x48;
		break;
	case _object_type_control:
		placements = &scenario->controls;
		palette = &scenario->control_palette;
		size = 0x40;
		break;
	case _object_type_light_fixture:
		placements = &scenario->light_fixtures;
		palette = &scenario->light_fixtures_palette;
		size = 0x58;
		break;
	default:
		placements = palette = NULL;
		size = 0;
		break;
	}
	for (index = 0; placements && placements->address && palette->address && index < placements->count && !found;
		index++)
	{
		struct scenario_object_datum const *placement = (struct scenario_object_datum const *)
			((byte const *)placements->address + index * size);
		struct scenario_object_palette_entry const *entry;
		real dx = placement->position.x - object->object.position.x;
		real dy = placement->position.y - object->object.position.y;
		real dz = placement->position.z - object->object.position.z;

		if (placement->palette_entry_index < 0 || placement->palette_entry_index >= palette->count ||
			dx * dx + dy * dy + dz * dz > 1.0e-4f)
		{
			continue;
		}
		entry = TAG_BLOCK_GET_ELEMENT(palette, placement->palette_entry_index, struct scenario_object_palette_entry);
		if (entry->reference.index == object->definition_index && scenery_static(palette, placement) != NONE)
		{
			found = TRUE;
		}
	}
	if (found)
	{
		long model_index = object_definition_get(object->definition_index)->object.model.index;

		for (index = 0; index < prism_bake.replaced_model_count && found; index++)
		{
			found = prism_bake.replaced_models[index] != model_index;
		}
	}
	kept_index[slot] = object_index;
	kept_answer[slot] = (char)found;
	return found;
}
