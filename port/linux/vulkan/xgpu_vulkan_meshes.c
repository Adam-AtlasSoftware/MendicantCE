/*
XGPU_VULKAN_MESHES.C

PRISM's mesh store in the Vulkan backend (xgpu_vulkan_meshes.h).

The store is made at the first MESH_STORE_MAP with a worker thread and an
auto budget of a fifth of the device's free memory, 128 MB to 1 GB. Each
MESH_INSTANCE keeps its node matrices and asks the store for its mesh at its
size (the level of detail); a MESH_DRAW names the part the draws that follow
are of. A draw of a part of an instance whose mesh is in:
- whose region and permutation the mesh has no piece of: drawn stock;
- of a carrier (the first part of its geometry with its shader): skinned
  once a submission (its level's vertices, by its bones: the instance's node
  matrices, and a helper bone's by its rig, xgpu_rig.c, on the game's clock
  the instance carries), then drawn as each of the
  level's pieces with the part's region, permutation and shader, with the
  part's own pipeline, its constants but node 0's matrix the identity (the
  skinned vertices are in the world), 32-bit indices, a triangle list;
- of any other part: not drawn.
The rest of a part drawn in several draws (no state between them: the
engine's 10000 triangles a draw) is not drawn again. Instances whose bones
are the same (the same mesh, level and matrices: a shadow's and the view's)
share one skinning in a submission. An object's level is the resolver's
(xgpu_geometry.h): one decision a frame per object, by its datum and mesh,
with the hysteresis against the level it had however long ago, which its
view, its shadows and its traced shape all draw (its shadow and view share
the one skinning); its traced level the resolver's within the traced
triangles' budget; a remodel drawn once never drawn stock again on the map
(the store keeps it: xgpu_mesh_store_pin). HALO_GEOMETRY_RESOLVER=legacy
puts back the choices before it (each draw's own level from a 256-slot table
of last frame's, the shadows' at the view's size where its slot remembered
it, the traced shape per draw within the share), for comparisons;
HALO_GEOMETRY_LOG=1 or 2 logs what each pass drew (xgpu_geometry.h).
A draw with a material (display.prism_materials) whose stream has its wide
pipeline (xgpu_vulkan_stream.c) reads the skinning's wide copy instead
(xgpu_vulkan_mesh_skin): the same 32-byte vertices with the mesh's uv0 as
floats in place of the stock vertex's 16-bit coordinates over the base map
scale and its nodes and weight (the pipeline's constants then). A mesh
drawn so once is skinned wide from then on (WIDE_MESHES), so its draws
without a material and its shadow share that skinning.
*/

#include "host.h"
#include "xgpu_geometry.h"
#include "xgpu_mesh_store.h"
#include "xgpu_rig.h"
#include "xgpu_vulkan_meshes.h"
#include "xgpu_vulkan_rt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (port: the instances named by their id, a frame's apart: 512 slots were
shared by b30's rocks, the cast and their shadows, and a part named after
its slot was taken was drawn stock) */
#define INSTANCES 4096u
#define SHARED_SKINS 32u
/* objects' levels in the view, by object and mesh (for their shadows) */
#define VIEW_LEVELS 256u
/* meshes skinned with their wide stream (a draw with a material read it) */
#define WIDE_MESHES 64u
/* the still objects' skinnings seen (direct-mapped): a skinning seen again
on a later frame is kept (xgpu_vulkan_mesh_skin_kept) */
#define SEEN_SKINS 4096u
/* (rasterizer_xbox.c's node matrices' first vertex constant register: c60,
D3D's -36) */
#define NODE_MATRIX_REGISTER 60u

struct instance
{
	uint32_t id;
	int drawable;
	struct xgpu_mesh_store_view view;
	/* the object (0xffffffff none), mesh, flags, the game's clock and the
	object's functions: a rig's springs and drivers */
	uint32_t object, mesh, flags;
	double game_seconds;
	float functions[4];
	uint32_t node_count;
	float matrices[XGPU_STREAM_MESH_NODES_MAXIMUM][3][4];
	uint32_t hash;
	/* its skinned vertices (and their wide stream, where skinned_wide),
	and the submission they were skinned in */
	struct xgpu_vulkan_range skinned, wide;
	int skinned_wide;
	uint64_t skinned_serial;
	/* display.prism_rt: the pre-pass window its pieces were last captured
	in, and the parts' keys captured in it */
	uint32_t rt_window, rt_key_count;
	uint32_t rt_keys[16];
	/* the resolver's traced level (the view's where legacy), and the cell an
	object of no datum is keyed by (xgpu_geometry_cell) */
	uint32_t traced_lod, cell;
};

static struct
{
	struct xgpu_mesh_store *store;
	int failed;
	struct instance instances[INSTANCES];
	/* the part the draws are of (MESH_DRAW): its instance (NULL none) */
	struct instance *part;
	struct xgpu_stream_mesh_draw key;
	/* a draw of the part swapped or skipped since a state record */
	int drawn_since_state;
	/* the swap prepared: its constants and pieces' first, and whether its
	pieces read the wide stream */
	struct xgpu_vulkan_range constants;
	uint32_t piece_cursor;
	int piece_wide;
	/* the meshes skinned wide (store ids; 0 none) */
	uint32_t wide_meshes[WIDE_MESHES];
	int layout_logged;
	/* skinnings in the submission, shared by instances of the same bones */
	struct
	{
		uint64_t serial;
		void *rest;
		uint32_t lod, hash;
		/* the instance skinned (its matrices compared, not the hash alone) */
		const struct instance *owner;
		uint32_t owner_id;
		struct xgpu_vulkan_range skinned, wide;
		int skinned_wide;
	} shared[SHARED_SKINS];
	uint32_t shared_next;
	/* the size each object was drawn at in the view (its shadow drawn at
	it), and the frame */
	struct
	{
		uint32_t object, mesh;
		uint64_t frame;
		float lod_pixels;
		/* the level it was drawn at (lod_known: once drawn from the store),
		and the frame of that (the hysteresis's level before; a switch
		counted between frames in a row) */
		uint32_t lod, lod_known;
		uint64_t lod_frame;
	} views[VIEW_LEVELS];
	uint64_t frame;
	float bones[XGPU_PMESH_BONES_MAXIMUM][3][4];
	/* the springs' state of every instance, and the latest game clock */
	struct xgpu_rig_states *rig_states;
	double game_seconds;
	uint64_t rig_evaluations, rig_failures;
	/* statistics */
	uint64_t instances_seen, instances_drawable, swaps, pieces, skips, chunks, stock_parts, layout_refusals,
		prepare_failures, shared_skins, shadows_at_view, wide_swaps;
	uint64_t level_instances[XGPU_PMESH_LODS_MAXIMUM];
	float largest_pixels;
	/* an object's level switched from one frame to the next */
	uint64_t level_switches;
	/* (HALO_GEOMETRY_LOG) a part named whose instance's slot another
	instance had taken since (drawn stock), and the traced captures of a
	remodelled part that failed: its skinning, no piece */
	uint64_t parts_lost, rt_skin_failed, rt_no_piece, rt_left_out;
	/* draws skinned at a coarser level of the remodel, the ring full */
	uint64_t coarser_skins;
	/* the skinned budget's share of its most (cut where the ring filled in
	the frame before, grown back slowly), and the ring's fills then */
	float skinned_scale;
	uint64_t ring_full_seen;
	/* the resolver (xgpu_geometry.h); legacy: the choices before it, by
	HALO_GEOMETRY_RESOLVER=legacy or (port) display.lighting Classic's (the
	instances without XGPU_STREAM_MESH_INSTANCE_RESOLVED: Classic draws as
	it always did); map_resolved the map's record's (the skin ring's size,
	from the next mesh made) */
	struct xgpu_geometry *geometry;
	int legacy, settings_known, environment_legacy, map_resolved;
	/* the set's number (its record's generation), and the remodels-drawn
	pairing (RT_FRAME's game signal against the store's, on the same frame):
	frames paired, agreeing and not */
	uint32_t generation;
	uint64_t pairs, pairs_agreed, pairs_disagreed;
	uint32_t hysteresis_percent;
	struct
	{
		uint64_t key, frame;
	} seen[SEEN_SKINS];
} meshes;

static void geometry_log(void *context, const char *line)
{
	(void)context;
	host_logf(HOST_LOG_INFO, "%s", line);
}

/* the resolver, made once (NULL: none could be; legacy then) */
static struct xgpu_geometry *geometry_get(void)
{
	if (!meshes.geometry && !meshes.settings_known)
	{
		const char *mode = getenv("HALO_GEOMETRY_RESOLVER"), *log = getenv("HALO_GEOMETRY_LOG");

		meshes.settings_known = 1;
		meshes.environment_legacy = mode && !strcmp(mode, "legacy");
		meshes.legacy = meshes.environment_legacy || !meshes.map_resolved;
		meshes.geometry = xgpu_geometry_create();
		if (meshes.geometry && log && *log >= '1' && *log <= '9')
			xgpu_geometry_log(meshes.geometry, *log - '0', geometry_log, NULL);
		if (meshes.legacy)
			host_logf(HOST_LOG_INFO, "meshstore: HALO_GEOMETRY_RESOLVER=legacy: each pass's level chosen as before the "
				"resolver (for comparisons)");
	}
	if (!meshes.geometry)
		meshes.legacy = 1;
	else
		meshes.legacy = meshes.environment_legacy || !meshes.map_resolved;
	return meshes.geometry;
}

int xgpu_vulkan_meshes_drawn(void)
{
	struct xgpu_mesh_store_statistics statistics;

	if (!meshes.store)
		return 0;
	xgpu_mesh_store_statistics(meshes.store, &statistics);
	return statistics.entries > 0;
}

void xgpu_vulkan_meshes_pair(uint32_t frame, int game_drawn, uint32_t game_generation)
{
	int host_drawn = xgpu_vulkan_meshes_drawn(), agree;
	const char *log = getenv("HALO_GEOMETRY_LOG");

	/* (the game's number of its set, 24 bits of it, as RT_FRAME carries it;
	the store's set the last record's) */
	agree = !game_drawn == !host_drawn && (game_generation & 0xffffffu) == (meshes.generation & 0xffffffu);
	meshes.pairs++;
	if (agree)
		meshes.pairs_agreed++;
	else
		meshes.pairs_disagreed++;
	if (log && (*log >= '2' || (!agree && *log >= '1')))
		host_logf(HOST_LOG_INFO, "geometry: remodels drawn: game frame %u: game %d (set %u), host %d (set %u)%s", frame,
			game_drawn, game_generation & 0xffffffu, host_drawn, meshes.generation & 0xffffffu,
			agree ? "" : " DISAGREE");
}

int xgpu_vulkan_meshes_resolved(void)
{
	geometry_get();
	return !meshes.legacy;
}

/* ---------- the store's backend */

static void *mesh_buffer_create(void *context, int kind, uint64_t size, void **mapped, uint64_t *bytes)
{
	struct xgpu_vulkan_mesh_buffer *buffer;

	int last = xgpu_vulkan_memory_use(XGPU_VULKAN_MEMORY_MESHES);

	(void)context;
	buffer = xgpu_vulkan_mesh_buffer_create(size, kind == XGPU_MESH_STORE_BUFFER_REST ? XGPU_VULKAN_MESH_BUFFER_REST :
		kind == XGPU_MESH_STORE_BUFFER_INDICES ? XGPU_VULKAN_MESH_BUFFER_INDICES : XGPU_VULKAN_MESH_BUFFER_STAGING, mapped);
	xgpu_vulkan_memory_use(last);
	*bytes = xgpu_vulkan_mesh_buffer_bytes(buffer);
	return buffer;
}

static void mesh_buffer_destroy(void *context, void *buffer)
{
	(void)context;
	xgpu_vulkan_mesh_buffer_destroy(buffer);
}

static void mesh_copy(void *context, void *from, uint64_t from_offset, void *to, uint64_t to_offset, uint64_t size)
{
	(void)context;
	xgpu_vulkan_mesh_buffer_copy(from, from_offset, to, to_offset, size);
}

static int mesh_memory(void *context, uint64_t *budget, uint64_t *usage)
{
	(void)context;
	return xgpu_vulkan_memory_budget(budget, usage);
}

static void mesh_log(void *context, int warning, const char *text)
{
	(void)context;
	host_logf(warning ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", text);
}

static int store_make(void)
{
	struct xgpu_mesh_store_backend backend;
	struct xgpu_mesh_store_settings settings;

	if (meshes.store)
		return 1;
	if (meshes.failed)
		return 0;
	memset(&backend, 0, sizeof(backend));
	backend.buffer_create = mesh_buffer_create;
	backend.buffer_destroy = mesh_buffer_destroy;
	backend.copy = mesh_copy;
	backend.memory = mesh_memory;
	backend.log = mesh_log;
	memset(&settings, 0, sizeof(settings));
	settings.workers = 1;
	settings.budget_floor = 128ull << 20;
	settings.budget_ceiling = 1ull << 30;
	meshes.store = xgpu_mesh_store_create(&backend, &settings);
	if (!meshes.store)
		meshes.failed = 1;
	return meshes.store != NULL;
}

/* ---------- the stream's records */

int xgpu_vulkan_meshes_available(void)
{
#ifdef _WIN32
	return 0;
#else
	return getenv("HALO_MESH_STORE_OFF") == NULL;
#endif
}

void xgpu_vulkan_meshes_map(void *context, const struct xgpu_stream_mesh_store_map *record,
	const struct xgpu_stream_mesh_store_query *queries, const char *strings,
	struct xgpu_stream_mesh_store_answer *answers)
{
	struct xgpu_mesh_store_query *asked = NULL;
	struct xgpu_mesh_store_answer *answered = NULL;
	uint32_t index, count = record->query_count;

	(void)context;
	/* (port: the resolver in PRISM Rebaked and Path Traced alone, the record
	says; Classic's choices as they always were, and its skin ring's size:
	xgpu_vulkan_mesh_skin_mode, taken at the next mesh made) */
	meshes.map_resolved = (record->flags & XGPU_STREAM_MESH_STORE_RESOLVED) != 0;
	meshes.generation = record->generation;
	geometry_get();
	xgpu_vulkan_mesh_skin_mode(!meshes.environment_legacy && meshes.map_resolved);
	/* (the instances and the part of the old meshes forgotten) */
	memset(meshes.instances, 0, sizeof(meshes.instances));
	if (meshes.rig_states)
		xgpu_rig_states_clear(meshes.rig_states);
	memset(meshes.shared, 0, sizeof(meshes.shared));
	memset(meshes.views, 0, sizeof(meshes.views));
	memset(meshes.wide_meshes, 0, sizeof(meshes.wide_meshes));
	memset(meshes.seen, 0, sizeof(meshes.seen));
	xgpu_vulkan_mesh_skin_kept_clear();
	meshes.part = NULL;
	if (!count && !meshes.store)
		return;
	if (!xgpu_vulkan_meshes_available() || !store_make())
	{
		for (index = 0; index < count; index++)
			answers[index].status = XGPU_STREAM_MESH_STORE_DEVICE;
		return;
	}
	if (count)
	{
		asked = calloc(count, sizeof(*asked));
		answered = calloc(count, sizeof(*answered));
		if (!asked || !answered)
		{
			free(asked);
			free(answered);
			for (index = 0; index < count; index++)
				answers[index].status = XGPU_STREAM_MESH_STORE_UNREADABLE;
			return;
		}
	}
	for (index = 0; index < count; index++)
	{
		asked[index].path = strings + queries[index].path;
		asked[index].file_size = queries[index].file_size;
		asked[index].stamp_seconds = queries[index].stamp_seconds;
		asked[index].node_count = queries[index].node_count;
		asked[index].node_crc = queries[index].node_crc;
		asked[index].table = (const uint8_t *)strings + queries[index].table;
		asked[index].table_count = queries[index].table_count;
		asked[index].base_map_scale[0] = queries[index].base_map_scale[0];
		asked[index].base_map_scale[1] = queries[index].base_map_scale[1];
	}
	xgpu_mesh_store_map(meshes.store, record->generation, record->budget_megabytes, count, asked, answered);
	xgpu_mesh_store_options(meshes.store, (record->flags & XGPU_STREAM_MESH_STORE_PREFETCH) != 0,
		(record->flags >> XGPU_STREAM_MESH_STORE_HYSTERESIS_SHIFT) & XGPU_STREAM_MESH_STORE_HYSTERESIS_MASK);
	meshes.hysteresis_percent = (record->flags >> XGPU_STREAM_MESH_STORE_HYSTERESIS_SHIFT) &
		XGPU_STREAM_MESH_STORE_HYSTERESIS_MASK;
	/* (the new set's ids: every object's choice made again) */
	if (geometry_get())
		xgpu_geometry_map(meshes.geometry);
	memset(meshes.views, 0, sizeof(meshes.views));
	for (index = 0; index < count; index++)
	{
		answers[index].id = answered[index].id;
		answers[index].render_radius = answered[index].render_radius;
		answers[index].bones = (uint16_t)answered[index].bones;
		answers[index].lods = (uint8_t)answered[index].lods;
		answers[index].status = (uint8_t)answered[index].status;
	}
	free(asked);
	free(answered);
}

static uint32_t bytes_hash(const void *data, size_t size, uint32_t hash)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	return hash;
}

/* the choices before the resolver (HALO_GEOMETRY_RESOLVER=legacy): each
draw's level from the 256-slot table of last frame's levels, a shadow's at
its view's size where the slot remembered it; 1 and the view where drawn */
static int instance_legacy(const struct xgpu_stream_mesh_instance *record, struct instance *instance)
{
	float lod_pixels = record->lod_pixels;
	int previous_lod = -1;
	uint32_t slot = 0;
	int viewed = 0;

	/* (an object's view's size kept; its shadow drawn at it, where the
	view was drawn this frame or the last) */
	if (record->object != 0xffffffffu)
	{
		slot = (record->object ^ (record->mesh * 2654435761u)) % VIEW_LEVELS;
		if (!(record->flags & XGPU_STREAM_MESH_INSTANCE_SHADOW))
		{
			/* (the level this object was drawn at the frame before, where
			the slot is still its) */
			if (meshes.views[slot].object == record->object && meshes.views[slot].mesh == record->mesh &&
				meshes.views[slot].lod_known && meshes.views[slot].lod_frame + 1 >= meshes.frame)
				previous_lod = (int)meshes.views[slot].lod;
			else
				meshes.views[slot].lod_known = 0;
			viewed = 1;
			meshes.views[slot].object = record->object;
			meshes.views[slot].mesh = record->mesh;
			meshes.views[slot].frame = meshes.frame + 1;
			meshes.views[slot].lod_pixels = record->lod_pixels;
		}
		else if (meshes.views[slot].frame && meshes.views[slot].object == record->object &&
			meshes.views[slot].mesh == record->mesh && meshes.views[slot].frame >= meshes.frame)
		{
			lod_pixels = meshes.views[slot].lod_pixels;
			meshes.shadows_at_view++;
		}
	}
	if (!meshes.store || !xgpu_mesh_store_use_from(meshes.store, record->mesh, lod_pixels, previous_lod,
		&instance->view))
		return 0;
	/* (the view's level kept for the next frame's hysteresis; a change
	from the frame before counted) */
	if (viewed)
	{
		if (previous_lod >= 0 && (uint32_t)previous_lod != instance->view.lod &&
			meshes.views[slot].lod_frame != meshes.frame)
			meshes.level_switches++;
		meshes.views[slot].lod = instance->view.lod;
		meshes.views[slot].lod_known = 1;
		meshes.views[slot].lod_frame = meshes.frame;
	}
	instance->traced_lod = instance->view.lod;
	if (lod_pixels > meshes.largest_pixels)
		meshes.largest_pixels = lod_pixels;
	return 1;
}

/* the resolver's choice (xgpu_geometry.h); 1 and the view where drawn as
the remodel */
static int instance_resolved(const struct xgpu_stream_mesh_instance *record, struct instance *instance,
	const float (*matrices)[3][4])
{
	struct xgpu_geometry_ladder ladder;
	struct xgpu_geometry_choice choice;
	struct xgpu_mesh_store_view probe;
	float origin[3];
	int resident = 0;

	/* (the store asked first: the mesh's reading begun, its use counted) */
	if (meshes.store && xgpu_mesh_store_use_from(meshes.store, record->mesh, record->lod_pixels, -1, &probe))
	{
		memset(&ladder, 0, sizeof(ladder));
		resident = xgpu_mesh_store_ladder(meshes.store, record->mesh, &ladder.lod_count, ladder.triangles,
			ladder.cutoff) && xgpu_mesh_store_ladder_vertices(meshes.store, record->mesh, ladder.vertices,
			&ladder.per_frame);
		if (ladder.lod_count > XGPU_GEOMETRY_LODS)
			ladder.lod_count = XGPU_GEOMETRY_LODS;
	}
	if (record->node_count)
	{
		origin[0] = matrices[0][0][3];
		origin[1] = matrices[0][1][3];
		origin[2] = matrices[0][2][3];
	}
	xgpu_geometry_resolve(meshes.geometry, record->object, record->mesh,
		(record->flags & XGPU_STREAM_MESH_INSTANCE_SHADOW) != 0, resident ? &ladder : NULL, record->lod_pixels,
		record->node_count ? origin : NULL, &choice);
	instance->cell = choice.cell;
	if (choice.variant != XGPU_GEOMETRY_REMODEL ||
		!xgpu_mesh_store_use_lod(meshes.store, record->mesh, choice.lod, &instance->view))
		return 0;
	xgpu_mesh_store_pin(meshes.store, record->mesh);
	instance->traced_lod = choice.traced_lod;
	if (record->lod_pixels > meshes.largest_pixels)
		meshes.largest_pixels = record->lod_pixels;
	return 1;
}

void xgpu_vulkan_meshes_instance(void *context, const struct xgpu_stream_mesh_instance *record,
	const float (*matrices)[3][4])
{
	struct instance *instance = &meshes.instances[record->instance % INSTANCES];
	int drawn, shadow = (record->flags & XGPU_STREAM_MESH_INSTANCE_SHADOW) != 0;

	(void)context;
	/* (port: the resolver in PRISM Rebaked and Path Traced, as the instance
	says (display.lighting is read each frame by the game); Classic's own
	choices else) */
	meshes.map_resolved = (record->flags & XGPU_STREAM_MESH_INSTANCE_RESOLVED) != 0;
	geometry_get();
	xgpu_vulkan_mesh_skin_budget(!meshes.legacy);
	memset(instance, 0, offsetof(struct instance, matrices));
	instance->hash = 0;
	memset(&instance->skinned, 0, sizeof(instance->skinned));
	memset(&instance->wide, 0, sizeof(instance->wide));
	instance->skinned_wide = 0;
	instance->skinned_serial = 0;
	instance->id = record->instance;
	instance->object = record->object;
	instance->mesh = record->mesh;
	instance->flags = record->flags;
	instance->game_seconds = (double)record->game_ticks / 30.0 +
		(record->game_fraction > 0.0f && record->game_fraction < 1.0f ? (double)record->game_fraction / 30.0 : 0.0);
	memcpy(instance->functions, record->functions, sizeof(instance->functions));
	if (instance->game_seconds > meshes.game_seconds || instance->game_seconds + 1.0 < meshes.game_seconds)
		meshes.game_seconds = instance->game_seconds;
	instance->node_count = record->node_count;
	memcpy(instance->matrices, matrices, record->node_count * sizeof(matrices[0]));
	meshes.instances_seen++;
	/* (an object of no datum's cell, which its notes name: legacy's too) */
	if (record->node_count)
	{
		float origin[3];

		origin[0] = matrices[0][0][3];
		origin[1] = matrices[0][1][3];
		origin[2] = matrices[0][2][3];
		instance->cell = xgpu_geometry_cell(record->object, origin);
	}
	drawn = meshes.legacy ? instance_legacy(record, instance) : instance_resolved(record, instance, matrices);
	if (drawn && instance->view.node_count > instance->node_count)
		drawn = 0;
	/* (what the pass drew noted: stock here where the mesh is not in; a
	remodel's as its draws are made, or stock where one cannot be,
	xgpu_vulkan_meshes_note_drawn) */
	if (meshes.geometry && !drawn)
		xgpu_geometry_note(meshes.geometry, record->object, record->mesh, instance->cell,
			shadow ? XGPU_GEOMETRY_SHADOW : XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_STOCK, 0u);
	if (!drawn)
		return;
	instance->hash = bytes_hash(matrices, record->node_count * sizeof(matrices[0]), 2166136261u ^ instance->view.lod);
	instance->drawable = 1;
	meshes.instances_drawable++;
	meshes.level_instances[instance->view.lod]++;
}

void xgpu_vulkan_meshes_draw_part(void *context, const struct xgpu_stream_mesh_draw *record)
{
	struct instance *instance = &meshes.instances[record->instance % INSTANCES];

	(void)context;
	meshes.part = record->instance && instance->id == record->instance ? instance : NULL;
	/* (a part named after its instance's slot was taken: drawn stock, as the
	game's part; counted and said, HALO_GEOMETRY_LOG=2) */
	if (record->instance && instance->id != record->instance)
	{
		meshes.parts_lost++;
		if (meshes.geometry && getenv("HALO_GEOMETRY_LOG") && *getenv("HALO_GEOMETRY_LOG") >= '2')
			host_logf(HOST_LOG_INFO, "geometry: frame %llu part of instance %u lost (its slot holds %u): drawn stock",
				(unsigned long long)xgpu_geometry_frame_number(meshes.geometry), record->instance, instance->id);
	}
	meshes.key = *record;
	meshes.drawn_since_state = 0;
}

void xgpu_vulkan_meshes_state_changed(void)
{
	meshes.drawn_since_state = 0;
}

/* ---------- the swap */

int xgpu_vulkan_meshes_verdict(const struct xgpu_vulkan_pipeline_description *pipeline, uint32_t draw_bindings,
	int triangles)
{
	const struct instance *instance = meshes.part;
	uint32_t region = meshes.key.region, permutation = meshes.key.permutation, index;

	if (!instance || !instance->drawable)
		return XGPU_VULKAN_MESHES_STOCK;
	/* (a region and permutation the mesh has nothing of: the stock part) */
	if (region >= 32 || permutation >= 32 ||
		!(instance->view.present[(region * 32 + permutation) / 8] & (1u << ((region * 32 + permutation) % 8))))
	{
		meshes.stock_parts++;
		return XGPU_VULKAN_MESHES_STOCK;
	}
	if (!(meshes.key.flags & XGPU_STREAM_MESH_DRAW_CARRIER))
	{
		meshes.skips++;
		return XGPU_VULKAN_MESHES_SKIP;
	}
	if (meshes.drawn_since_state)
	{
		meshes.chunks++;
		return XGPU_VULKAN_MESHES_SKIP;
	}
	/* (the stock compressed model vertex: one stream of 32 bytes, its
	position three floats at its start; the first refused logged) */
	if (!meshes.layout_logged)
	{
		char text[512];
		size_t at = 0;

		at += (size_t)snprintf(text + at, sizeof(text) - at, "meshstore: a carrier's draw: triangles %d, bindings %x, "
			"stride %u; attributes", triangles, draw_bindings, pipeline->strides[0]);
		for (index = 0; index < 16 && at < sizeof(text); index++)
		{
			if (pipeline->attributes[index].format)
				at += (size_t)snprintf(text + at, sizeof(text) - at, " %u:%u/%u@%u", index,
					pipeline->attributes[index].format, pipeline->attributes[index].binding,
					pipeline->attributes[index].offset);
		}
		host_logf(HOST_LOG_INFO, "%s", text);
		meshes.layout_logged = 1;
	}
	if (!triangles || !(draw_bindings & 1u) || (draw_bindings & ~(1u | (1u << XGPU_VULKAN_BINDING_CONSTANT))) ||
		pipeline->strides[0] != 32 || pipeline->attributes[0].binding != 0 || pipeline->attributes[0].offset != 0)
	{
		meshes.layout_refusals++;
		return XGPU_VULKAN_MESHES_STOCK;
	}
	for (index = 0; index < 16; index++)
	{
		if (pipeline->attributes[index].format && pipeline->attributes[index].binding != 0 &&
			pipeline->attributes[index].binding != XGPU_VULKAN_BINDING_CONSTANT)
		{
			meshes.layout_refusals++;
			return XGPU_VULKAN_MESHES_STOCK;
		}
	}
	return XGPU_VULKAN_MESHES_SWAP;
}

/* the mesh skinned wide from now on (a draw with a material read it), or
whether it is */
static int wide_mesh(uint32_t mesh, int mark)
{
	uint32_t slot = mesh % WIDE_MESHES;

	if (mark)
		meshes.wide_meshes[slot] = mesh;
	return mesh && meshes.wide_meshes[slot] == mesh;
}

/* the instance's level skinned in the submission being recorded (shared
with an instance of the same bones skinned in it), with its wide stream
where wide or where its mesh is skinned wide */
static int instance_skin(struct instance *instance, int wide)
{
	uint64_t serial = xgpu_vulkan_serial();
	const struct xgpu_mesh_store_view *view = &instance->view;
	uint32_t index;

	wide = wide_mesh(instance->mesh, wide);
	if (instance->skinned_serial == serial && instance->skinned.buffer && (instance->skinned_wide || !wide))
		return 1;
	for (index = 0; index < SHARED_SKINS; index++)
	{
		const struct instance *owner = meshes.shared[index].owner;

		if (meshes.shared[index].serial == serial && (meshes.shared[index].skinned_wide || !wide) &&
			meshes.shared[index].rest == view->rest &&
			meshes.shared[index].lod == view->lod && meshes.shared[index].hash == instance->hash && owner &&
			owner->id == meshes.shared[index].owner_id && owner->node_count == instance->node_count &&
			(!view->rig || (owner->object == instance->object && owner->game_seconds == instance->game_seconds)) &&
			!memcmp(owner->matrices, instance->matrices, instance->node_count * sizeof(instance->matrices[0])))
		{
			instance->skinned = meshes.shared[index].skinned;
			instance->wide = meshes.shared[index].wide;
			instance->skinned_wide = meshes.shared[index].skinned_wide;
			instance->skinned_serial = serial;
			meshes.shared_skins++;
			return 1;
		}
	}
	/* (a bone's matrix: its node's, the instance's; helpers' by the rig) */
	if (view->rig)
	{
		struct xgpu_rig_instance rig_instance;

		if (!meshes.rig_states)
			meshes.rig_states = xgpu_rig_states_create();
		memset(&rig_instance, 0, sizeof(rig_instance));
		rig_instance.key = instance->object != 0xffffffffu ?
			((uint64_t)instance->object << 32) | instance->mesh : 0;
		rig_instance.game_seconds = instance->game_seconds;
		rig_instance.ce_matrices = (const float (*)[12])instance->matrices;
		rig_instance.ce_matrix_count = instance->node_count;
		rig_instance.flags = (instance->flags & XGPU_STREAM_MESH_INSTANCE_NO_SECONDARY) ?
			XGPU_RIG_INSTANCE_NO_SECONDARY : 0;
		memcpy(rig_instance.functions, instance->functions, sizeof(rig_instance.functions));
		meshes.rig_evaluations++;
		if (xgpu_rig_evaluate(view->rig, meshes.rig_states, &rig_instance, (float (*)[12])meshes.bones, NULL) !=
			XGPU_RIG_OK)
		{
			meshes.rig_failures++;
			return 0;
		}
	}
	else
	{
		for (index = 0; index < view->bone_count; index++)
		{
			uint32_t node = view->bone_nodes[index];

			if (node >= instance->node_count)
				return 0;
			memcpy(meshes.bones[index], instance->matrices[node], sizeof(meshes.bones[0]));
		}
	}
	/* (port: a still object, its bones its nodes' matrices, as it was a
	frame before: its skinning kept and read frame after frame, not skinned
	again into the ring each submission, whose filling had drawn a few of a
	beach's rocks stock each frame) */
	if (!view->rig && !meshes.legacy)
	{
		/* (the wide stream's a key of its own: a narrow skinning kept, which
		the shadows read every frame, is never one a submission in flight
		does not read, so it could not be made wide in its place) */
		uint64_t key = ((uint64_t)instance->hash << 32 | instance->mesh) ^ ((uint64_t)view->lod << 20) ^
			((uint64_t)view->vertex_first * 0x9e3779b97f4a7c15ull) ^ (wide ? 0x8000000000000000ull : 0u);
		uint32_t slot = (uint32_t)(key % SEEN_SKINS);
		int make = meshes.seen[slot].key == key && meshes.seen[slot].frame < meshes.frame;

		if (meshes.seen[slot].key != key)
		{
			meshes.seen[slot].key = key;
			meshes.seen[slot].frame = meshes.frame;
		}
		if (xgpu_vulkan_mesh_skin_kept(key, make, view->rest, view->vertex_first, view->vertex_count,
			(const float (*)[4])meshes.bones, view->bone_count, view->inverse_scale, &instance->skinned,
			wide ? &instance->wide : NULL))
		{
			if (meshes.geometry)
				xgpu_geometry_skinned(meshes.geometry, instance->object, instance->mesh, instance->cell, 1);
			goto skinned;
		}
	}
	/* (into the ring: counted in the resolver's skinned vertices' budget) */
	if (meshes.geometry && !meshes.legacy)
		xgpu_geometry_skinned(meshes.geometry, instance->object, instance->mesh, instance->cell, 0);
	if (!xgpu_vulkan_mesh_skin(view->rest, view->vertex_first, view->vertex_count,
		(const float (*)[4])meshes.bones, view->bone_count, view->inverse_scale, &instance->skinned,
		wide ? &instance->wide : NULL))
		return 0;
skinned:
	instance->skinned_wide = wide;
	instance->skinned_serial = xgpu_vulkan_serial();
	index = meshes.shared_next++ % SHARED_SKINS;
	meshes.shared[index].serial = instance->skinned_serial;
	meshes.shared[index].rest = view->rest;
	meshes.shared[index].lod = view->lod;
	meshes.shared[index].hash = instance->hash;
	meshes.shared[index].owner = instance;
	meshes.shared[index].owner_id = instance->id;
	meshes.shared[index].skinned = instance->skinned;
	meshes.shared[index].wide = instance->wide;
	meshes.shared[index].skinned_wide = instance->skinned_wide;
	return 1;
}

int xgpu_vulkan_meshes_prepare(const float (*constants)[4], uint32_t constant_count, int wide)
{
	struct instance *instance = meshes.part;
	float (*to)[4];
	uint64_t serial;

	if (!instance || !instance->drawable || constant_count < NODE_MATRIX_REGISTER + 3)
		return 0;
	if (!instance_skin(instance, wide))
	{
		/* (port: the skin ring full: a coarser level of the remodel, which
		takes less of it, never CE's part; the frame's other draws of the
		object keep the level drawn here) */
		struct xgpu_mesh_store_view coarser;
		uint32_t lod = instance->view.lod + 1u;
		int skinned = 0;

		while (!meshes.legacy && !skinned && meshes.store &&
			xgpu_mesh_store_use_lod(meshes.store, instance->mesh, lod, &coarser) &&
			coarser.node_count <= instance->node_count)
		{
			instance->view = coarser;
			instance->hash = bytes_hash(instance->matrices, instance->node_count * sizeof(instance->matrices[0]),
				2166136261u ^ coarser.lod);
			instance->skinned_serial = 0;
			skinned = instance_skin(instance, wide);
			meshes.coarser_skins++;
			lod++;
		}
		if (!skinned)
		{
			meshes.prepare_failures++;
			return 0;
		}
	}
	serial = xgpu_vulkan_serial();
	to = xgpu_vulkan_upload(constant_count * 16, 256, &meshes.constants);
	/* (the upload began another submission: skinned again in it, by the
	caller's next attempt) */
	if (!to || serial != xgpu_vulkan_serial())
		return 0;
	memcpy(to, constants, constant_count * 16);
	memset(to[NODE_MATRIX_REGISTER], 0, 3 * sizeof(to[0]));
	to[NODE_MATRIX_REGISTER][0] = 1.0f;
	to[NODE_MATRIX_REGISTER + 1][1] = 1.0f;
	to[NODE_MATRIX_REGISTER + 2][2] = 1.0f;
	meshes.piece_cursor = 0;
	meshes.piece_wide = wide && instance->skinned_wide;
	return 1;
}

int xgpu_vulkan_meshes_piece(uint32_t index, struct xgpu_vulkan_draw *draw)
{
	const struct instance *instance = meshes.part;
	const struct xgpu_mesh_store_view *view;
	uint32_t found = 0, piece_index;

	if (!instance || !instance->drawable)
		return 0;
	view = &instance->view;
	if (index == 0)
	{
		xgpu_vulkan_meshes_note_drawn(1);
		meshes.swaps++;
		meshes.wide_swaps += meshes.piece_wide ? 1 : 0;
		meshes.drawn_since_state = 1;
	}
	for (piece_index = 0; piece_index < view->piece_count; piece_index++)
	{
		const struct xgpu_mesh_store_piece *piece = &view->pieces[piece_index];

		if (piece->region != meshes.key.region || piece->permutation != meshes.key.permutation ||
			piece->shader != meshes.key.shader || found++ != index)
			continue;
		draw->vertices[0] = meshes.piece_wide ? instance->wide : instance->skinned;
		draw->indices.buffer = xgpu_vulkan_mesh_buffer_handle(view->indices);
		draw->indices.offset = 0;
		draw->index_type = 1;
		draw->first_index = piece->index_first;
		draw->count = piece->index_count;
		draw->vertex_offset = (int32_t)(piece->vertex_first - view->vertex_first);
		draw->topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
		draw->constants = meshes.constants;
		meshes.pieces++;
		return 1;
	}
	return 0;
}

/* the part's pieces of the instance's view, its skinned vertices */
static int rt_pieces_fill(const struct instance *instance, uint32_t region, uint32_t permutation, uint64_t pose,
	struct xgpu_vulkan_meshes_rt_piece *pieces, int most)
{
	const struct xgpu_mesh_store_view *view = &instance->view;
	uint32_t piece_index;
	int count = 0;

	for (piece_index = 0; piece_index < view->piece_count && count < most; piece_index++)
	{
		const struct xgpu_mesh_store_piece *piece = &view->pieces[piece_index];
		struct xgpu_vulkan_meshes_rt_piece *to = &pieces[count];

		if (piece->region != region || piece->permutation != permutation || piece->shader != meshes.key.shader)
			continue;
		to->vertices = instance->skinned.buffer;
		to->vertex_offset = instance->skinned.offset;
		to->vertex_count = view->vertex_count;
		to->indices = xgpu_vulkan_mesh_buffer_handle(view->indices);
		to->index_first = piece->index_first;
		to->index_count = piece->index_count;
		to->index_add = (int32_t)(piece->vertex_first - view->vertex_first);
		/* (the object it is of, which the instance's number is not: a new
		one each draw of the model) */
		to->identity[0] = ((uint64_t)(instance->object != 0xffffffffu ? instance->object : instance->id) << 32) |
			instance->mesh;
		to->identity[1] = ((uint64_t)view->lod << 32) | piece_index;
		to->pose = pose;
		to->origin[0] = instance->node_count ? instance->matrices[0][0][3] : 0.0f;
		to->origin[1] = instance->node_count ? instance->matrices[0][1][3] : 0.0f;
		to->origin[2] = instance->node_count ? instance->matrices[0][2][3] : 0.0f;
		if (to->vertices && to->indices && to->index_count >= 3)
			count++;
	}
	/* (none of the level's pieces is this part's: the view draws nothing of
	it at this level either, so nothing traced, never the stock part; the
	stock part where legacy) */
	return !count && meshes.legacy ? -1 : count;
}

static int rt_pieces_level(struct instance *instance, uint32_t lod, struct xgpu_vulkan_meshes_rt_piece *pieces,
	int most);

int xgpu_vulkan_meshes_rt_pieces(uint32_t window, struct xgpu_vulkan_meshes_rt_piece *pieces, int most)
{
	struct instance *instance = meshes.part;
	const struct xgpu_mesh_store_view *view;
	uint32_t region = meshes.key.region, permutation = meshes.key.permutation, key, index;
	uint64_t pose;

	if (!instance || !instance->drawable || region >= 32 || permutation >= 32 ||
		!(instance->view.present[(region * 32 + permutation) / 8] & (1u << ((region * 32 + permutation) % 8))))
		return -1;
	if (!(meshes.key.flags & XGPU_STREAM_MESH_DRAW_CARRIER))
		return 0;
	/* (a part drawn in several draws, or again in the window: once) */
	key = (region << 24) ^ (permutation << 16) ^ (uint32_t)meshes.key.shader;
	if (instance->rt_window != window)
	{
		instance->rt_window = window;
		instance->rt_key_count = 0;
	}
	for (index = 0; index < instance->rt_key_count; index++)
		if (instance->rt_keys[index] == key)
			return 0;
	if (instance->rt_key_count < sizeof(instance->rt_keys) / sizeof(instance->rt_keys[0]))
		instance->rt_keys[instance->rt_key_count++] = key;
	/* (the resolver's traced level where it is coarser than the drawn one:
the traced triangles' budget, the farthest first) */
	if (!meshes.legacy && instance->traced_lod != instance->view.lod)
	{
		int count = rt_pieces_level(instance, instance->traced_lod, pieces, most);

		if (count > 0)
		{
			xgpu_vulkan_meshes_note_traced(XGPU_GEOMETRY_REMODEL, instance->traced_lod);
			return count;
		}
	}
	if (!instance_skin(instance, 0))
	{
		meshes.rt_skin_failed++;
		if (meshes.legacy)
			return -1;
		/* (port: a coarser level of the remodel, which takes less of the
		ring, never CE's part; none: nothing traced, a stock fallback noted) */
		{
			uint32_t lod;

			for (lod = instance->view.lod + 1u; lod < XGPU_GEOMETRY_LODS; lod++)
			{
				int count = rt_pieces_level(instance, lod, pieces, most);

				if (count > 0)
				{
					xgpu_vulkan_meshes_note_traced(XGPU_GEOMETRY_REMODEL, lod);
					return count;
				}
			}
		}
		meshes.rt_left_out++;
		xgpu_vulkan_meshes_note_traced(XGPU_GEOMETRY_NONE, 0);
		return 0;
	}
	view = &instance->view;
	/* (its bones' matrices and level, and a rig's clock: its springs move
	whatever the matrices) */
	pose = ((uint64_t)instance->hash << 32) ^ (view->rig ? (uint64_t)(instance->game_seconds * 1000.0) : 0) ^
		(uint64_t)view->lod;
	{
		int count = rt_pieces_fill(instance, region, permutation, pose, pieces, most);

		if (count <= 0)
			meshes.rt_no_piece++;
		if (count > 0 && !meshes.legacy)
			xgpu_vulkan_meshes_note_traced(XGPU_GEOMETRY_REMODEL, view->lod);
		return count;
	}
}

/* a level of the part (lod) skinned in the instance's place, and the
instance's own level and skinning put back after, so that the frame's draws
of it after this one keep the level drawn: the pieces' count, -1 none */
static int rt_pieces_level(struct instance *instance, uint32_t lod, struct xgpu_vulkan_meshes_rt_piece *pieces,
	int most)
{
	struct instance kept;
	struct xgpu_mesh_store_view level;
	uint32_t region = meshes.key.region, permutation = meshes.key.permutation;
	uint64_t pose;
	int count;

	if (!instance || !instance->drawable || !meshes.store || region >= 32 || permutation >= 32 ||
		!(meshes.key.flags & XGPU_STREAM_MESH_DRAW_CARRIER) ||
		!xgpu_mesh_store_use_lod(meshes.store, instance->mesh, lod, &level) ||
		level.lod == instance->view.lod || level.node_count > instance->node_count ||
		!(level.present[(region * 32 + permutation) / 8] & (1u << ((region * 32 + permutation) % 8))))
		return -1;
	memcpy(&kept, instance, offsetof(struct instance, matrices));
	kept.hash = instance->hash;
	kept.skinned = instance->skinned;
	kept.wide = instance->wide;
	kept.skinned_wide = instance->skinned_wide;
	kept.skinned_serial = instance->skinned_serial;
	instance->view = level;
	instance->hash = bytes_hash(instance->matrices, instance->node_count * sizeof(instance->matrices[0]),
		2166136261u ^ level.lod);
	instance->skinned_serial = 0;
	count = -1;
	if (instance_skin(instance, 0))
	{
		pose = ((uint64_t)instance->hash << 32) ^ (level.rig ? (uint64_t)(instance->game_seconds * 1000.0) : 0) ^
			(uint64_t)level.lod;
		count = rt_pieces_fill(instance, region, permutation, pose, pieces, most);
	}
	memcpy(instance, &kept, offsetof(struct instance, matrices));
	instance->hash = kept.hash;
	instance->skinned = kept.skinned;
	instance->wide = kept.wide;
	instance->skinned_wide = kept.skinned_wide;
	instance->skinned_serial = kept.skinned_serial;
	return count;
}

/* port: (legacy) the part's least detailed level, traced past
display.prism_rt_objects "auto"'s reach or share */
int xgpu_vulkan_meshes_rt_pieces_coarse(struct xgpu_vulkan_meshes_rt_piece *pieces, int most)
{
	struct instance *instance = meshes.part;
	struct xgpu_mesh_store_view coarse;

	if (!instance || !instance->drawable || !meshes.store ||
		!xgpu_mesh_store_use(meshes.store, instance->mesh, 1.0f, &coarse))
		return -1;
	return rt_pieces_level(instance, coarse.lod, pieces, most);
}

void xgpu_vulkan_meshes_note_drawn(int remodel)
{
	const struct instance *instance = meshes.part;

	if (!meshes.geometry || !instance || !instance->id || !instance->drawable)
		return;
	xgpu_geometry_note(meshes.geometry, instance->object, instance->mesh, instance->cell,
		(instance->flags & XGPU_STREAM_MESH_INSTANCE_SHADOW) ? XGPU_GEOMETRY_SHADOW : XGPU_GEOMETRY_VIEW,
		remodel ? XGPU_GEOMETRY_REMODEL : XGPU_GEOMETRY_STOCK, remodel ? instance->view.lod : 0u);
}

int xgpu_vulkan_meshes_part_remodelled(void)
{
	const struct instance *instance = meshes.part;
	uint32_t region = meshes.key.region, permutation = meshes.key.permutation;

	return instance && instance->drawable && region < 32 && permutation < 32 &&
		(instance->view.present[(region * 32 + permutation) / 8] & (1u << ((region * 32 + permutation) % 8))) != 0;
}

void xgpu_vulkan_meshes_note_traced(int variant, uint32_t lod)
{
	const struct instance *instance = meshes.part;

	if (meshes.geometry && instance && instance->id)
		xgpu_geometry_note(meshes.geometry, instance->object, instance->mesh, instance->cell, XGPU_GEOMETRY_TRACED,
			variant, lod);
}

void xgpu_vulkan_meshes_frame(void)
{
	meshes.frame++;
	/* (port: the next frame's share of the skin ring made free now, at the
	frame's end, never within a frame) */
	xgpu_vulkan_mesh_skin_frame();
	/* (the resolver's frame: the eye the traced scene was last built from,
	the traced triangles' budget, the hysteresis the map's record gave) */
	if (geometry_get())
	{
		struct xgpu_geometry_settings settings;
		float eye[3];

		memset(&settings, 0, sizeof(settings));
		settings.hysteresis = (float)(meshes.hysteresis_percent > 50u ? 50u : meshes.hysteresis_percent) / 100.0f;
		settings.traced_budget = xgpu_vulkan_rt_replacement_budget(&settings.coarse_reach);
		{
			struct xgpu_vulkan_mesh_skin_budget budget;
			uint64_t ring_full;

			/* (the ring filled since the last frame, or a frame past its
			share of it: the budget cut by a sixth, to a quarter at least;
			else grown back a tenth of a percent a frame) */
			xgpu_vulkan_mesh_skin_budget_statistics(&budget);
			ring_full = budget.ring_full + budget.over_frames;
			if (meshes.skinned_scale <= 0.0f)
				meshes.skinned_scale = 1.0f;
			if (ring_full != meshes.ring_full_seen)
				meshes.skinned_scale = meshes.skinned_scale * 0.85f < 0.25f ? 0.25f : meshes.skinned_scale * 0.85f;
			else if (meshes.skinned_scale < 1.0f)
				meshes.skinned_scale = meshes.skinned_scale * 1.001f > 1.0f ? 1.0f : meshes.skinned_scale * 1.001f;
			meshes.ring_full_seen = ring_full;
			settings.skinned_budget = (uint32_t)((float)xgpu_vulkan_mesh_skin_budget_vertices() * meshes.skinned_scale);
		}
		xgpu_geometry_settings_set(meshes.geometry, &settings);
		xgpu_geometry_frame(meshes.geometry, xgpu_vulkan_rt_eye(eye) ? eye : NULL);
		if (getenv("HALO_GEOMETRY_LOG") && meshes.frame % 600u == 0)
		{
			uint64_t dispatches, vertices, ring_full;

			uint64_t kept_hits, kept_made, kept_evicted, kept_refused;

			xgpu_vulkan_mesh_skin_statistics(&dispatches, &vertices, &ring_full);
			xgpu_vulkan_mesh_skin_kept_statistics(&kept_hits, &kept_made, &kept_evicted, &kept_refused);
			host_logf(HOST_LOG_INFO, "meshstore: kept skinnings: %llu read, %llu made, %llu let go of, %llu refused",
				(unsigned long long)kept_hits, (unsigned long long)kept_made, (unsigned long long)kept_evicted,
				(unsigned long long)kept_refused);
			host_logf(HOST_LOG_INFO, "meshstore: %llu frames: %llu instances (%llu drawable), %llu swaps, %llu parts named "
				"after their instance's slot was taken (drawn stock), %llu skinnings failed (drawn stock; the skin ring "
				"full %llu times, %llu dispatches, %llu vertices), traced captures failed: %llu skinnings, %llu without a "
				"piece; %llu drawn a level coarser for the ring", (unsigned long long)meshes.frame, (unsigned long long)meshes.instances_seen,
				(unsigned long long)meshes.instances_drawable, (unsigned long long)meshes.swaps,
				(unsigned long long)meshes.parts_lost, (unsigned long long)meshes.prepare_failures,
				(unsigned long long)ring_full, (unsigned long long)dispatches, (unsigned long long)vertices,
				(unsigned long long)meshes.rt_skin_failed, (unsigned long long)meshes.rt_no_piece,
				(unsigned long long)meshes.coarser_skins);
			{
				struct xgpu_vulkan_mesh_skin_budget budget;

				xgpu_vulkan_mesh_skin_budget_statistics(&budget);
				host_logf(HOST_LOG_INFO, "meshstore: %llu frames: the skin ring %llu MB (%llu MB kept), %llu MB a frame; "
					"the most a frame took %.1f MB, in use %.1f MB, kept %.1f MB; %llu frames past their share (the "
					"budget cut), %llu skinnings past it, the ring full %llu times, waited for at a "
					"frame's start %llu times (%.1f ms); the upload ring submitted mid-frame %llu times; at most %llu submissions a frame; budget scale %.3f",
					(unsigned long long)meshes.frame, (unsigned long long)(budget.ring_size >> 20),
					(unsigned long long)(budget.keep_size >> 20), (unsigned long long)(budget.frame_cap >> 20),
					(double)budget.frame_most / 1048576.0, (double)budget.in_use_most / 1048576.0,
					(double)budget.keep_most / 1048576.0, (unsigned long long)budget.over_frames,
					(unsigned long long)budget.over_drawn, (unsigned long long)budget.ring_full,
					(unsigned long long)budget.frame_waits, budget.frame_wait_seconds * 1000.0,
					(unsigned long long)budget.upload_flushes, (unsigned long long)budget.submissions_most,
					(double)meshes.skinned_scale);
				host_logf(HOST_LOG_INFO, "meshstore: %llu frames: %s; ring waited for %llu; lost parts %llu, traced left "
					"out %llu; remodels drawn paired %llu frames, %llu agreed, %llu disagreed", (unsigned long long)meshes.frame,
					meshes.legacy ? "legacy choices" : "resolved", (unsigned long long)budget.frame_waits,
					(unsigned long long)meshes.parts_lost,
					(unsigned long long)meshes.rt_left_out, (unsigned long long)meshes.pairs,
					(unsigned long long)meshes.pairs_agreed, (unsigned long long)meshes.pairs_disagreed);
			}
		}
	}
	if (meshes.store)
		xgpu_mesh_store_frame(meshes.store);
	if (meshes.rig_states)
		xgpu_rig_states_age(meshes.rig_states, meshes.game_seconds);
}

uint32_t xgpu_vulkan_meshes_pending(void)
{
	return meshes.store ? xgpu_mesh_store_pending(meshes.store) : 0;
}

int xgpu_vulkan_meshes_statistics(char *text, unsigned long size)
{
	struct xgpu_mesh_store_statistics statistics;
	uint64_t dispatches, vertices, ring_full;

	if (!meshes.store || !size)
		return 0;
	xgpu_mesh_store_statistics(meshes.store, &statistics);
	if (!statistics.entries)
		return 0;
	xgpu_vulkan_mesh_skin_statistics(&dispatches, &vertices, &ring_full);
	snprintf(text, size, "meshstore: %u accepted, %u resident, %u reading, %u failed, %u over budget; resident %.1f MB "
		"of %.1f MB, read %.1f MB, %u evictions; %llu uses (%llu stale, %llu stock); instances %llu (%llu drawable); "
		"swaps %llu (%llu pieces), %llu parts skipped, %llu draws of parts drawn, %llu parts stock, %llu layouts "
		"refused, %llu skinnings failed; %llu dispatches (%llu shared), %llu vertices skinned, %llu ring full; "
		"instances by level %llu/%llu/%llu/%llu, largest %.0f px; last resident %.2f s after the map; rigs %llu "
		"evaluated (%llu failed), %u spring states; %llu shadows at their view's level; %llu swaps wide, %llu "
		"vertices skinned wide; %u drawn stock then swapped, %u read before drawn, %llu levels switched",
		statistics.entries, statistics.resident, statistics.reading, statistics.failed, statistics.refused_budget,
		(double)statistics.resident_bytes / 1048576.0, (double)statistics.budget_bytes / 1048576.0,
		(double)statistics.read_bytes / 1048576.0, statistics.evictions, (unsigned long long)statistics.uses,
		(unsigned long long)statistics.stale_uses, (unsigned long long)statistics.stock_uses,
		(unsigned long long)meshes.instances_seen, (unsigned long long)meshes.instances_drawable,
		(unsigned long long)meshes.swaps, (unsigned long long)meshes.pieces, (unsigned long long)meshes.skips,
		(unsigned long long)meshes.chunks, (unsigned long long)meshes.stock_parts,
		(unsigned long long)meshes.layout_refusals, (unsigned long long)meshes.prepare_failures,
		(unsigned long long)dispatches, (unsigned long long)meshes.shared_skins, (unsigned long long)vertices,
		(unsigned long long)ring_full, (unsigned long long)meshes.level_instances[0],
		(unsigned long long)meshes.level_instances[1], (unsigned long long)meshes.level_instances[2],
		(unsigned long long)meshes.level_instances[3], (double)meshes.largest_pixels, statistics.last_resident_seconds,
		(unsigned long long)meshes.rig_evaluations, (unsigned long long)meshes.rig_failures,
		meshes.rig_states ? xgpu_rig_states_count(meshes.rig_states) : 0u,
		(unsigned long long)meshes.shadows_at_view, (unsigned long long)meshes.wide_swaps,
		(unsigned long long)xgpu_vulkan_mesh_skin_wide_vertices(), statistics.drawn_swaps, statistics.prefetched,
		(unsigned long long)meshes.level_switches);
	return 1;
}
