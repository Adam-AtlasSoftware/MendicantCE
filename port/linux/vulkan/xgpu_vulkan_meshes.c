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
share one skinning in a submission. An object's shadow is drawn at the
level its view was drawn at this frame (or the last, if the shadow comes
first), not at the shadow's own smaller size, so the two share the one
skinning rather than skinning a second level.
A draw with a material (display.prism_materials) whose stream has its wide
pipeline (xgpu_vulkan_stream.c) reads the skinning's wide stream too, at
binding 1 (xgpu_vulkan_mesh_skin): the mesh's uv0 as floats in place of the
stock vertex's 16-bit coordinates over the base map scale, and its own
tangents where the shaders have room for them. A mesh
drawn so once is skinned wide from then on (WIDE_MESHES), so its draws
without a material and its shadow share that skinning.
*/

#include "host.h"
#include "xgpu_mesh_store.h"
#include "xgpu_rig.h"
#include "xgpu_vulkan_meshes.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTANCES 512u
#define SHARED_SKINS 32u
/* objects' levels in the view, by object and mesh (for their shadows) */
#define VIEW_LEVELS 256u
/* meshes skinned with their wide stream (a draw with a material read it) */
#define WIDE_MESHES 64u
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
} meshes;

/* ---------- the store's backend */

static void *mesh_buffer_create(void *context, int kind, uint64_t size, void **mapped, uint64_t *bytes)
{
	struct xgpu_vulkan_mesh_buffer *buffer;

	(void)context;
	buffer = xgpu_vulkan_mesh_buffer_create(size, kind == XGPU_MESH_STORE_BUFFER_REST ? XGPU_VULKAN_MESH_BUFFER_REST :
		kind == XGPU_MESH_STORE_BUFFER_INDICES ? XGPU_VULKAN_MESH_BUFFER_INDICES : XGPU_VULKAN_MESH_BUFFER_STAGING, mapped);
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
	/* (the instances and the part of the old meshes forgotten) */
	memset(meshes.instances, 0, sizeof(meshes.instances));
	if (meshes.rig_states)
		xgpu_rig_states_clear(meshes.rig_states);
	memset(meshes.shared, 0, sizeof(meshes.shared));
	memset(meshes.views, 0, sizeof(meshes.views));
	memset(meshes.wide_meshes, 0, sizeof(meshes.wide_meshes));
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

void xgpu_vulkan_meshes_instance(void *context, const struct xgpu_stream_mesh_instance *record,
	const float (*matrices)[3][4])
{
	struct instance *instance = &meshes.instances[record->instance % INSTANCES];
	float lod_pixels = record->lod_pixels;

	(void)context;
	/* (an object's view's size kept; its shadow drawn at it, where the
	view was drawn this frame or the last) */
	if (record->object != 0xffffffffu)
	{
		uint32_t slot = (record->object ^ (record->mesh * 2654435761u)) % VIEW_LEVELS;

		if (!(record->flags & XGPU_STREAM_MESH_INSTANCE_SHADOW))
		{
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
	if (!meshes.store || !xgpu_mesh_store_use(meshes.store, record->mesh, lod_pixels, &instance->view))
		return;
	if (instance->view.node_count > instance->node_count)
		return;
	instance->hash = bytes_hash(matrices, record->node_count * sizeof(matrices[0]), 2166136261u ^ instance->view.lod);
	instance->drawable = 1;
	meshes.instances_drawable++;
	meshes.level_instances[instance->view.lod]++;
	if (lod_pixels > meshes.largest_pixels)
		meshes.largest_pixels = lod_pixels;
}

void xgpu_vulkan_meshes_draw_part(void *context, const struct xgpu_stream_mesh_draw *record)
{
	struct instance *instance = &meshes.instances[record->instance % INSTANCES];

	(void)context;
	meshes.part = record->instance && instance->id == record->instance ? instance : NULL;
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
	if (!xgpu_vulkan_mesh_skin(view->rest, view->vertex_first, view->vertex_count,
		(const float (*)[4])meshes.bones, view->bone_count, view->inverse_scale, &instance->skinned,
		wide ? &instance->wide : NULL))
		return 0;
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
		meshes.prepare_failures++;
		return 0;
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
		draw->vertices[0] = instance->skinned;
		if (meshes.piece_wide)
		{
			draw->vertices[1] = instance->wide;
			draw->bindings |= 2u;
		}
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

void xgpu_vulkan_meshes_frame(void)
{
	meshes.frame++;
	if (meshes.store)
		xgpu_mesh_store_frame(meshes.store);
	if (meshes.rig_states)
		xgpu_rig_states_age(meshes.rig_states, meshes.game_seconds);
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
		"vertices skinned wide",
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
		(unsigned long long)xgpu_vulkan_mesh_skin_wide_vertices());
	return 1;
}
