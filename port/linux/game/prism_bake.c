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
  ("auto" bakes in the background while playing, "ask" only when asked,
  "off" never), display.bake_quality, the rebakes asked for
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
BSP's is done (the measurements' runs). Nothing here changes what the game
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
#include "prism_deferred.h"
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
void xgpu_backend_prism_bake(const struct xgpu_stream_prism_bake *bake, const void *data, unsigned long size);
unsigned long prism_scene_static_generation(void);
int prism_scene_static_ended(void);
/* render_enumerate.c's (the window drawn is a map's first frame's, unseen) */
extern unsigned char render_enumerating;
/* ui_widget.c's, game.c's */
boolean main_menu_is_active(void);
boolean game_in_progress(void);

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
};

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	boolean active;
	int mode, quality, foreground;
	long request;
	float budget, threshold;
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
	prism_bake.mode = value && !strcmp(value, "off") ? XGPU_BAKE_MODE_OFF : value && !strcmp(value, "ask") ?
		XGPU_BAKE_MODE_ASK : XGPU_BAKE_MODE_AUTO;
	value = config_string("display.bake_quality");
	prism_bake.quality = !value ? 3 : !strcmp(value, "draft") ? 0 : !strcmp(value, "high") ? 1 :
		!strcmp(value, "ultra") ? 2 : 3;
	prism_bake.request = config_integer("display.bake_rebake");
	prism_bake.budget = (float)config_real("display.bake_budget");
	if (!(prism_bake.budget >= 0.25f && prism_bake.budget <= 200.0f))
	{
		prism_bake.budget = 2.0f;
	}
	prism_bake.threshold = 0.01f;
	prism_bake.foreground = config_boolean("debug.bake_exit");
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
		their vertices' normals */

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
	blob_word(&blob, 3);
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
	bake->budget_milliseconds = prism_bake.foreground ? 60.0f : prism_bake_menus_up() ? 30.0f : prism_bake.budget;
	bake->threshold = prism_bake.threshold;
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
			platform_log("prism bake: bsp %d: the reach's inputs (%lu seeds, %lu triangles, %d nodes, %ld fighters): %lu "
				"KB", (int)bsp_index, (unsigned long)input.seed_count, (unsigned long)input.triangle_count,
				(int)input.bsp.node_count, input.fighter_count, prism_bake.reach_bytes / 1024);
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
	if (prism_bake.foreground)
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
}
