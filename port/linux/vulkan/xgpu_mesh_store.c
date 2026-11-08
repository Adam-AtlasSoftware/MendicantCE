/*
XGPU_MESH_STORE.C

PRISM's mesh store's portable core (xgpu_mesh_store.h).

An entry is indexed at the map's record (its header, skeleton, levels and
pieces read and checked), read when it is first drawn (its vertices and
indices, by the worker, into a staging buffer the backend made for it, every
value checked), copied into its buffers at the next frame's start and drawn
from then on (resident), until the next map's record or the budget lets it
go. A failed read leaves it failed (drawn stock) for the map.

Windows's host (port/windows/host) has no pread, and opening a guest's path
there goes through its system calls' translation, not checked yet: the store
answers every query refused there (the clean disabled path), as the texture
store does.
*/

#include "xgpu_mesh_store.h"
#include "xgpu_rig.h"
#include "xgpu_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define STORE_FILES 1
#else
#define STORE_FILES 0
#endif

enum
{
	_entry_indexed,
	_entry_queued,
	_entry_reading,
	_entry_read,
	_entry_resident,
	_entry_failed,
};

struct entry
{
	char *path;
	uint64_t file_size;
	uint32_t stamp_seconds;
	uint32_t node_count, bone_count, lod_count, piece_count;
	float radius;
	float inverse_scale[2];
	uint16_t *bone_nodes;
	struct xgpu_rig *rig;
	struct
	{
		float cutoff;
		uint32_t piece_first, piece_count, vertex_first, vertex_count, index_first, index_count;
	} lods[XGPU_PMESH_LODS_MAXIMUM];
	struct xgpu_mesh_store_piece *pieces;
	uint8_t present[128];
	/* VERT and INDX in the file */
	uint64_t vertex_offset, vertex_size, index_offset, index_size;
	uint32_t vertex_crc, index_crc;
	int state;
	/* the worker's verdict (0 good), its reason */
	int read_failed;
	char reason[160];
	/* (the worker's) vertices whose uv0 is past the base map scale: the
	stock layout's clamped there, the wide stream's whole */
	uint64_t uv_wide_vertices;
	void *rest, *indices, *staging;
	unsigned char *staging_mapped;
	uint64_t bytes;
	uint64_t used_frame;
	uint64_t retry_frame;
};

struct xgpu_mesh_store
{
	struct xgpu_mesh_store_backend backend;
	struct xgpu_mesh_store_settings settings;
	uint32_t generation;
	struct entry *entries;
	uint32_t entry_count;
	uint64_t budget_bytes, resident_bytes;
	uint32_t budget_megabytes;
	uint64_t frame;
	double map_time;
	struct xgpu_mesh_store_statistics statistics;
#if STORE_FILES
	pthread_t thread;
	int thread_made;
	pthread_mutex_t lock;
	pthread_cond_t wake, idle;
	int stopping, busy;
#endif
	/* the queue of entries to read (indices), under lock */
	uint32_t *queue;
	uint32_t queue_count, queue_capacity;
};

/* ---------- small things */

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static void say(struct xgpu_mesh_store *store, int warning, const char *format, ...)
{
	char text[512];
	va_list arguments;

	if (!store->backend.log)
		return;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	store->backend.log(store->backend.context, warning, text);
}

uint32_t xgpu_mesh_store_crc32(uint32_t crc, const void *data, uint64_t size)
{
	static uint32_t table[256];
	static int made;
	const unsigned char *bytes = data;
	uint64_t index;

	if (!made)
	{
		uint32_t value, bit, n;

		for (n = 0; n < 256; n++)
		{
			value = n;
			for (bit = 0; bit < 8; bit++)
				value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
			table[n] = value;
		}
		made = 1;
	}
	crc = ~crc;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ bytes[index]) & 0xffu] ^ (crc >> 8);
	return ~crc;
}

static uint32_t read32(const unsigned char *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint16_t read16(const unsigned char *at)
{
	return (uint16_t)(at[0] | (at[1] << 8));
}

static float read_float(const unsigned char *at)
{
	uint32_t bits = read32(at);
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static int finite_float(float value)
{
	return value == value && value < 3.0e38f && value > -3.0e38f;
}

static int ends_with(const char *text, const char *end)
{
	size_t length = strlen(text), end_length = strlen(end);

	return length >= end_length && !strcmp(text + length - end_length, end);
}

static void entry_buffers_destroy(struct xgpu_mesh_store *store, struct entry *entry)
{
	if (entry->rest)
		store->backend.buffer_destroy(store->backend.context, entry->rest);
	if (entry->indices)
		store->backend.buffer_destroy(store->backend.context, entry->indices);
	if (entry->staging)
		store->backend.buffer_destroy(store->backend.context, entry->staging);
	if (entry->state == _entry_resident || entry->rest || entry->indices)
		store->resident_bytes -= entry->bytes <= store->resident_bytes ? entry->bytes : store->resident_bytes;
	entry->rest = entry->indices = entry->staging = NULL;
	entry->staging_mapped = NULL;
}

/* ---------- the file's check (the map's record) */

#if STORE_FILES
static int read_exactly(int descriptor, void *buffer, uint64_t size, uint64_t at)
{
	uint64_t done = 0;

	while (done < size)
	{
		ssize_t got = pread(descriptor, (unsigned char *)buffer + done, (size_t)(size - done), (off_t)(at + done));

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return 0;
		done += (uint64_t)got;
	}
	return 1;
}
#endif

/* a section of the table: its offset, size and CRC, or 0 if none */
static int section_find(const unsigned char *table, uint32_t count, uint32_t kind, uint64_t file_size,
	uint64_t *offset, uint64_t *size, uint32_t *crc)
{
	uint32_t index;

	for (index = 0; index < count; index++)
	{
		const unsigned char *section = table + index * XGPU_PMESH_SECTION_BYTES;

		if (read32(section) != kind)
			continue;
		*offset = read32(section + 4);
		*size = read32(section + 8);
		*crc = read32(section + 12);
		return *offset <= file_size && *size <= file_size - *offset;
	}
	return 0;
}

/* a section read whole and its CRC checked: malloc'd, or NULL */
#if STORE_FILES
static unsigned char *section_read(int descriptor, uint64_t offset, uint64_t size, uint32_t crc)
{
	unsigned char *data = malloc(size ? (size_t)size : 1);

	if (!data)
		return NULL;
	if (!read_exactly(descriptor, data, size, offset) || xgpu_mesh_store_crc32(0, data, size) != crc)
	{
		free(data);
		return NULL;
	}
	return data;
}
#endif

/* the query's file opened and its header, skeleton, levels and pieces
checked against it: XGPU_STREAM_MESH_STORE_*, the entry filled */
static int query_check(struct xgpu_mesh_store *store, const struct xgpu_mesh_store_query *query, struct entry *entry,
	char *reason, size_t reason_size)
{
#if STORE_FILES
	unsigned char header[XGPU_PMESH_HEADER_BYTES];
	unsigned char table[XGPU_PMESH_SECTIONS_MAXIMUM * XGPU_PMESH_SECTION_BYTES];
	unsigned char *skeleton = NULL, *levels = NULL, *pieces = NULL;
	char resolved[PATH_MAX];
	struct stat information;
	uint64_t offset, size, vertex_total, index_total;
	uint32_t crc, section_count, index, status = XGPU_STREAM_MESH_STORE_FORMAT;
	int descriptor;

	(void)store;
	reason[0] = 0;
	if (!query->path || !query->path[0] || !ends_with(query->path, ".pmesh"))
		return XGPU_STREAM_MESH_STORE_PATH;
	if (!realpath(query->path, resolved))
		return XGPU_STREAM_MESH_STORE_UNREADABLE;
	/* (a file the guest found in a mods folder, a .pmesh file wherever its
	links lead: a mods folder linked to another disk is the player's own) */
	if (!strstr(query->path, "/mods/") || !ends_with(resolved, ".pmesh"))
		return XGPU_STREAM_MESH_STORE_PATH;
	descriptor = open(resolved, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return XGPU_STREAM_MESH_STORE_UNREADABLE;
	if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode))
	{
		close(descriptor);
		return XGPU_STREAM_MESH_STORE_UNREADABLE;
	}
	if ((uint64_t)information.st_size != query->file_size || (uint32_t)information.st_mtime != query->stamp_seconds)
	{
		close(descriptor);
		return XGPU_STREAM_MESH_STORE_CHANGED;
	}
	entry->file_size = (uint64_t)information.st_size;
	if (!read_exactly(descriptor, header, sizeof(header), 0))
	{
		close(descriptor);
		return XGPU_STREAM_MESH_STORE_UNREADABLE;
	}
#define REFUSE(code, ...) do { status = (code); snprintf(reason, reason_size, __VA_ARGS__); goto done; } while (0)
	if (read32(header) != XGPU_PMESH_MAGIC || read16(header + 4) != XGPU_PMESH_VERSION ||
		read32(header + 8) != entry->file_size)
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "not a .pmesh of version %u, or cut short", XGPU_PMESH_VERSION);
	section_count = read32(header + 12);
	offset = read32(header + 16);
	if (!section_count || section_count > XGPU_PMESH_SECTIONS_MAXIMUM || offset > entry->file_size ||
		(uint64_t)section_count * XGPU_PMESH_SECTION_BYTES > entry->file_size - offset ||
		!read_exactly(descriptor, table, section_count * XGPU_PMESH_SECTION_BYTES, offset))
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its section table is outside the file");
	entry->node_count = read32(header + 20);
	entry->bone_count = read32(header + 28);
	entry->lod_count = read32(header + 32);
	entry->radius = read_float(header + 48);
	if (entry->node_count != query->node_count || read32(header + 24) != query->node_crc)
		REFUSE(XGPU_STREAM_MESH_STORE_NODES, "its skeleton is of %u nodes, checksum %08x; the map's model has %u, %08x",
			entry->node_count, read32(header + 24), query->node_count, query->node_crc);
	if (!entry->bone_count || entry->bone_count > XGPU_PMESH_BONES_MAXIMUM || entry->bone_count < entry->node_count ||
		!entry->lod_count || entry->lod_count > XGPU_PMESH_LODS_MAXIMUM || !finite_float(entry->radius) ||
		entry->radius <= 0.0f)
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "%u bones, %u levels, radius %g", entry->bone_count, entry->lod_count,
			(double)entry->radius);
	/* the skeleton: the model's nodes first, in order, then helpers whose
	parents come before them */
	if (!section_find(table, section_count, XGPU_PMESH_SKEL, entry->file_size, &offset, &size, &crc) ||
		size < (uint64_t)entry->bone_count * XGPU_PMESH_BONE_BYTES ||
		!(skeleton = section_read(descriptor, offset, size, crc)))
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its skeleton is missing, short or damaged");
	entry->bone_nodes = calloc(entry->bone_count, sizeof(*entry->bone_nodes));
	if (!entry->bone_nodes)
		REFUSE(XGPU_STREAM_MESH_STORE_UNREADABLE, "out of memory");
	for (index = 0; index < entry->bone_count; index++)
	{
		const unsigned char *bone = skeleton + index * XGPU_PMESH_BONE_BYTES;
		int16_t parent = (int16_t)read16(bone + 4), node = (int16_t)read16(bone + 8);

		if (index < entry->node_count)
		{
			if (node != (int16_t)index)
				REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "bone %u is not the model's node %u", index, index);
			entry->bone_nodes[index] = (uint16_t)index;
		}
		else
		{
			if (node != -1 || parent < 0 || (uint32_t)parent >= index)
				REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "helper bone %u has no parent before it", index);
			entry->bone_nodes[index] = entry->bone_nodes[parent];
		}
	}
	/* (helper bones: their rig, the RIG section's rules or none, each
	helper then following its parent) */
	if (entry->bone_count > entry->node_count)
	{
		struct xgpu_rig_bone *rig_bones = calloc(entry->bone_count, sizeof(*rig_bones));
		unsigned char *rig_bytes = NULL;
		uint64_t rig_offset, rig_size, names_size = size - (uint64_t)entry->bone_count * XGPU_PMESH_BONE_BYTES;
		uint32_t rig_crc;
		enum xgpu_rig_status rig_status;
		char rig_error[128];

		if (!rig_bones)
			REFUSE(XGPU_STREAM_MESH_STORE_UNREADABLE, "out of memory");
		for (index = 0; index < entry->bone_count; index++)
		{
			const unsigned char *bone = skeleton + index * XGPU_PMESH_BONE_BYTES;
			const char *names = (const char *)skeleton + (uint64_t)entry->bone_count * XGPU_PMESH_BONE_BYTES;
			uint32_t name = read32(bone + 12), at;

			for (at = name; at < names_size && names[at]; at++)
				;
			rig_bones[index].name_hash = name < names_size ? xgpu_rig_name_hash(names + name, at - name) : 0;
			rig_bones[index].parent = (int16_t)read16(bone + 4);
			rig_bones[index].flags = read16(bone + 6);
			rig_bones[index].ce_node = (int16_t)read16(bone + 8);
			memcpy(rig_bones[index].bind_world, bone + 16, 48);
			memcpy(rig_bones[index].inverse_bind, bone + 64, 48);
		}
		if (section_find(table, section_count, XGPU_PMESH_RIG, entry->file_size, &rig_offset, &rig_size, &rig_crc) &&
			!(rig_bytes = section_read(descriptor, rig_offset, rig_size, rig_crc)))
		{
			free(rig_bones);
			REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its rig is damaged");
		}
		entry->rig = xgpu_rig_create(rig_bones, entry->bone_count, entry->node_count, rig_bytes,
			rig_bytes ? (size_t)rig_size : 0, &rig_status, rig_error, sizeof(rig_error));
		free(rig_bones);
		free(rig_bytes);
		if (!entry->rig)
			REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its rig is refused (%d): %s", (int)rig_status, rig_error);
	}
	/* the levels */
	if (!section_find(table, section_count, XGPU_PMESH_VERT, entry->file_size, &entry->vertex_offset,
		&entry->vertex_size, &entry->vertex_crc) || entry->vertex_size % XGPU_PMESH_VERTEX_BYTES ||
		!section_find(table, section_count, XGPU_PMESH_INDX, entry->file_size, &entry->index_offset,
		&entry->index_size, &entry->index_crc) || entry->index_size % 4)
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its vertices or indices are missing");
	vertex_total = entry->vertex_size / XGPU_PMESH_VERTEX_BYTES;
	index_total = entry->index_size / 4;
	if (!vertex_total || vertex_total > XGPU_PMESH_ELEMENTS_MAXIMUM || !index_total ||
		index_total > XGPU_PMESH_ELEMENTS_MAXIMUM)
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "%llu vertices, %llu indices", (unsigned long long)vertex_total,
			(unsigned long long)index_total);
	if (!section_find(table, section_count, XGPU_PMESH_LODS, entry->file_size, &offset, &size, &crc) ||
		size < (uint64_t)entry->lod_count * XGPU_PMESH_LOD_BYTES || !(levels = section_read(descriptor, offset, size, crc)))
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its levels are missing, short or damaged");
	if (!section_find(table, section_count, XGPU_PMESH_SUBM, entry->file_size, &offset, &size, &crc) ||
		!size || size % XGPU_PMESH_PIECE_BYTES || size / XGPU_PMESH_PIECE_BYTES > XGPU_PMESH_PIECES_MAXIMUM ||
		!(pieces = section_read(descriptor, offset, size, crc)))
		REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "its pieces are missing, too many or damaged");
	entry->piece_count = (uint32_t)(size / XGPU_PMESH_PIECE_BYTES);
	entry->pieces = calloc(entry->piece_count, sizeof(*entry->pieces));
	if (!entry->pieces)
		REFUSE(XGPU_STREAM_MESH_STORE_UNREADABLE, "out of memory");
	for (index = 0; index < entry->piece_count; index++)
	{
		const unsigned char *from = pieces + index * XGPU_PMESH_PIECE_BYTES;
		struct xgpu_mesh_store_piece *piece = &entry->pieces[index];
		uint32_t entry_index;

		piece->region = from[0];
		piece->permutation = from[1];
		piece->shader = from[2];
		piece->flags = from[3];
		piece->material = read32(from + 4);
		piece->vertex_first = read32(from + 8);
		piece->vertex_count = read32(from + 12);
		piece->index_first = read32(from + 16);
		piece->index_count = read32(from + 20);
		if (piece->vertex_first > vertex_total || piece->vertex_count > vertex_total - piece->vertex_first ||
			piece->index_first > index_total || piece->index_count > index_total - piece->index_first ||
			!piece->vertex_count || !piece->index_count || piece->index_count % 3)
			REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "piece %u's vertices or indices are outside the mesh's", index);
		for (entry_index = 0; entry_index < query->table_count; entry_index++)
		{
			const uint8_t *row = query->table + entry_index * 4;

			if (row[0] == piece->region && row[1] == piece->permutation && row[2] == piece->shader)
				break;
		}
		if (entry_index == query->table_count)
			REFUSE(XGPU_STREAM_MESH_STORE_TABLE, "piece %u's region %u, permutation %u and shader %u are no part's of "
				"the map's model", index, piece->region, piece->permutation, piece->shader);
		if (piece->region < 32 && piece->permutation < 32)
			entry->present[(piece->region * 32 + piece->permutation) / 8] |=
				(uint8_t)(1u << ((piece->region * 32 + piece->permutation) % 8));
	}
	for (index = 0; index < entry->lod_count; index++)
	{
		const unsigned char *from = levels + index * XGPU_PMESH_LOD_BYTES;
		uint32_t piece_index;

		entry->lods[index].cutoff = read_float(from);
		entry->lods[index].piece_first = read32(from + 4);
		entry->lods[index].piece_count = read32(from + 8);
		entry->lods[index].vertex_first = read32(from + 12);
		entry->lods[index].vertex_count = read32(from + 16);
		entry->lods[index].index_first = read32(from + 20);
		entry->lods[index].index_count = read32(from + 24);
		if (!finite_float(entry->lods[index].cutoff) || entry->lods[index].piece_first > entry->piece_count ||
			entry->lods[index].piece_count > entry->piece_count - entry->lods[index].piece_first ||
			entry->lods[index].vertex_first > vertex_total ||
			entry->lods[index].vertex_count > vertex_total - entry->lods[index].vertex_first ||
			!entry->lods[index].vertex_count || (index && entry->lods[index].cutoff > entry->lods[index - 1].cutoff))
			REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "level %u is outside the mesh, or more detailed than the one before",
				index);
		/* (each of its pieces inside its vertices: skinned together) */
		for (piece_index = 0; piece_index < entry->lods[index].piece_count; piece_index++)
		{
			const struct xgpu_mesh_store_piece *piece = &entry->pieces[entry->lods[index].piece_first + piece_index];

			if (piece->vertex_first < entry->lods[index].vertex_first || piece->vertex_first + piece->vertex_count >
				entry->lods[index].vertex_first + entry->lods[index].vertex_count)
				REFUSE(XGPU_STREAM_MESH_STORE_FORMAT, "level %u's piece %u is outside its vertices", index, piece_index);
		}
	}
	if (!(query->base_map_scale[0] > 0.0f) || !(query->base_map_scale[1] > 0.0f) ||
		!finite_float(query->base_map_scale[0]) || !finite_float(query->base_map_scale[1]))
		REFUSE(XGPU_STREAM_MESH_STORE_UV_RANGE, "the model's base map scale is %g by %g",
			(double)query->base_map_scale[0], (double)query->base_map_scale[1]);
	entry->inverse_scale[0] = 1.0f / query->base_map_scale[0];
	entry->inverse_scale[1] = 1.0f / query->base_map_scale[1];
	entry->bytes = entry->vertex_size + entry->index_size;
	entry->path = strdup(resolved);
	status = entry->path ? XGPU_STREAM_MESH_STORE_ACCEPTED : XGPU_STREAM_MESH_STORE_UNREADABLE;
#undef REFUSE
done:
	free(skeleton);
	free(levels);
	free(pieces);
	close(descriptor);
	return (int)status;
#else
	(void)store;
	(void)query;
	(void)entry;
	(void)reason;
	(void)reason_size;
	return XGPU_STREAM_MESH_STORE_DEVICE;
#endif
}

/* ---------- reading (the worker) */

#if STORE_FILES
static int entry_check(struct entry *entry, unsigned char *staging, const unsigned char *indices,
	uint64_t vertex_total, char *reason, size_t reason_size);
#endif

/* an entry's vertices and indices read into its staging buffer and checked
(the worker's, or the tests' calling thread): 0 good, else why in reason */
static int entry_read(struct entry *entry, const char *path, unsigned char *staging, char *reason, size_t reason_size)
{
#if STORE_FILES
	uint64_t vertex_total = entry->vertex_size / XGPU_PMESH_VERTEX_BYTES;
	unsigned char *read, *indices;
	int descriptor, failed;

	/* (read and checked in the CPU's own memory, then copied once into the
	staging buffer, which the CPU writes quickly but reads slowly) */
	read = malloc((size_t)(entry->vertex_size + entry->index_size));
	if (!read)
	{
		snprintf(reason, reason_size, "out of memory");
		return 1;
	}
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
	{
		free(read);
		snprintf(reason, reason_size, "cannot be opened");
		return 1;
	}
	indices = read + entry->vertex_size;
	if (!read_exactly(descriptor, read, entry->vertex_size, entry->vertex_offset) ||
		!read_exactly(descriptor, indices, entry->index_size, entry->index_offset))
	{
		close(descriptor);
		free(read);
		snprintf(reason, reason_size, "cannot be read");
		return 1;
	}
	close(descriptor);
	failed = entry_check(entry, read, indices, vertex_total, reason, reason_size);
	if (!failed)
		memcpy(staging, read, (size_t)(entry->vertex_size + entry->index_size));
	free(read);
	return failed;
#else
	(void)entry;
	(void)path;
	(void)staging;
	snprintf(reason, reason_size, "no files here");
	return 1;
#endif
}

#if STORE_FILES
/* a mesh's vertices and indices checked (and weightless vertices given
their first joint): 0 good, 1 bad. A vertex whose uv0 is past the stock
layout's range (the base map scale) is counted, not refused: the stock
layout clamps it, and a draw with a material reads it whole from the wide
stream (xgpu_vulkan_mesh_skin) */
static int entry_check(struct entry *entry, unsigned char *staging, const unsigned char *indices,
	uint64_t vertex_total, char *reason, size_t reason_size)
{
	uint64_t vertex;
	uint32_t piece_index;

	entry->uv_wide_vertices = 0;
	if (xgpu_mesh_store_crc32(0, staging, entry->vertex_size) != entry->vertex_crc ||
		xgpu_mesh_store_crc32(0, indices, entry->index_size) != entry->index_crc)
	{
		snprintf(reason, reason_size, "its vertices or indices are damaged (CRC)");
		return 1;
	}
	for (vertex = 0; vertex < vertex_total; vertex++)
	{
		unsigned char *at = staging + vertex * XGPU_PMESH_VERTEX_BYTES;
		float u = read_float(at + 24) * entry->inverse_scale[0], v = read_float(at + 28) * entry->inverse_scale[1];
		uint32_t weights = 0, k;

		if (!finite_float(read_float(at)) || !finite_float(read_float(at + 4)) || !finite_float(read_float(at + 8)))
		{
			snprintf(reason, reason_size, "vertex %llu's position is not a number", (unsigned long long)vertex);
			return 1;
		}
		/* (the stock layout's texture coordinates: int16 over the base map
		scale) */
		if (!finite_float(u) || !finite_float(v))
		{
			snprintf(reason, reason_size, "vertex %llu's uv0 is not a number", (unsigned long long)vertex);
			return 1;
		}
		if (!(u >= -1.0001f && u <= 1.0001f && v >= -1.0001f && v <= 1.0001f))
			entry->uv_wide_vertices++;
		for (k = 0; k < 4; k++)
		{
			if (at[40 + k] && read16(at + 32 + 2 * k) >= entry->bone_count)
			{
				snprintf(reason, reason_size, "vertex %llu names bone %u of %u", (unsigned long long)vertex,
					read16(at + 32 + 2 * k), entry->bone_count);
				return 1;
			}
			weights += at[40 + k];
		}
		/* (no weight: the first joint's whole) */
		if (!weights)
		{
			if (read16(at + 32) >= entry->bone_count)
			{
				snprintf(reason, reason_size, "vertex %llu has no weight and no bone", (unsigned long long)vertex);
				return 1;
			}
			at[40] = 255;
		}
	}
	for (piece_index = 0; piece_index < entry->piece_count; piece_index++)
	{
		const struct xgpu_mesh_store_piece *piece = &entry->pieces[piece_index];
		uint32_t index;

		for (index = 0; index < piece->index_count; index++)
		{
			if (read32(indices + (uint64_t)(piece->index_first + index) * 4) >= piece->vertex_count)
			{
				snprintf(reason, reason_size, "piece %u's index %u is past its %u vertices", piece_index, index,
					piece->vertex_count);
				return 1;
			}
		}
	}
	return 0;
}
#endif

#if STORE_FILES
static void *worker_main(void *argument)
{
	struct xgpu_mesh_store *store = argument;

	pthread_mutex_lock(&store->lock);
	for (;;)
	{
		struct entry *entry;
		uint32_t index;
		char reason[160], *path;
		unsigned char *staging;
		int failed;

		while (!store->stopping && !store->queue_count)
			pthread_cond_wait(&store->wake, &store->lock);
		if (store->stopping)
			break;
		index = store->queue[0];
		memmove(store->queue, store->queue + 1, (store->queue_count - 1) * sizeof(*store->queue));
		store->queue_count--;
		entry = &store->entries[index];
		entry->state = _entry_reading;
		path = entry->path;
		staging = entry->staging_mapped;
		store->busy = 1;
		pthread_mutex_unlock(&store->lock);
		reason[0] = 0;
		failed = entry_read(entry, path, staging, reason, sizeof(reason));
		pthread_mutex_lock(&store->lock);
		store->busy = 0;
		entry->read_failed = failed;
		memcpy(entry->reason, reason, sizeof(reason));
		entry->state = _entry_read;
		pthread_cond_broadcast(&store->idle);
	}
	pthread_mutex_unlock(&store->lock);
	return NULL;
}
#endif

static void lock(struct xgpu_mesh_store *store)
{
#if STORE_FILES
	if (store->thread_made)
		pthread_mutex_lock(&store->lock);
#else
	(void)store;
#endif
}

static void unlock(struct xgpu_mesh_store *store)
{
#if STORE_FILES
	if (store->thread_made)
		pthread_mutex_unlock(&store->lock);
#else
	(void)store;
#endif
}

/* the queue emptied and the worker idle (its read finished) */
static void reads_cancel(struct xgpu_mesh_store *store)
{
	lock(store);
	store->queue_count = 0;
#if STORE_FILES
	while (store->thread_made && store->busy)
		pthread_cond_wait(&store->idle, &store->lock);
#endif
	unlock(store);
}

/* ---------- the budget */

static void budget_set(struct xgpu_mesh_store *store, uint32_t budget_megabytes)
{
	uint64_t budget = 0, usage = 0;

	store->budget_megabytes = budget_megabytes;
	if (budget_megabytes)
	{
		store->budget_bytes = (uint64_t)budget_megabytes << 20;
		return;
	}
	if (store->backend.memory && store->backend.memory(store->backend.context, &budget, &usage) && budget > usage)
		store->budget_bytes = (budget - usage) / 5;
	else
		store->budget_bytes = store->settings.budget_floor;
	if (store->budget_bytes < store->settings.budget_floor)
		store->budget_bytes = store->settings.budget_floor;
	if (store->budget_bytes > store->settings.budget_ceiling)
		store->budget_bytes = store->settings.budget_ceiling;
}

/* room for bytes more: meshes not drawn lately let go of, the least
recently drawn first; 0 if there cannot be */
static int budget_room(struct xgpu_mesh_store *store, uint64_t bytes)
{
	while (store->resident_bytes + bytes > store->budget_bytes)
	{
		struct entry *oldest = NULL;
		uint32_t index;

		for (index = 0; index < store->entry_count; index++)
		{
			struct entry *entry = &store->entries[index];

			if (entry->state == _entry_resident && entry->used_frame + XGPU_MESH_STORE_KEEP_FRAMES < store->frame &&
				(!oldest || entry->used_frame < oldest->used_frame))
				oldest = entry;
		}
		if (!oldest)
			return 0;
		entry_buffers_destroy(store, oldest);
		lock(store);
		oldest->state = _entry_indexed;
		unlock(store);
		store->statistics.evictions++;
		say(store, 0, "meshstore: %s let go of (not drawn lately; budget %.1f MB)", oldest->path,
			(double)store->budget_bytes / 1048576.0);
	}
	return 1;
}

/* an entry's buffers made and its read queued; 0 where it does not fit */
static int entry_queue(struct xgpu_mesh_store *store, uint32_t index)
{
	struct entry *entry = &store->entries[index];
	uint64_t bytes = 0, staged = 0;

	if (!budget_room(store, entry->bytes))
	{
		if (!entry->retry_frame)
			say(store, 1, "meshstore: %s (%.1f MB) does not fit the budget of %.1f MB: drawn stock", entry->path,
				(double)entry->bytes / 1048576.0, (double)store->budget_bytes / 1048576.0);
		entry->retry_frame = store->frame + XGPU_MESH_STORE_RETRY_FRAMES;
		store->statistics.refused_budget++;
		return 0;
	}
	entry->rest = store->backend.buffer_create(store->backend.context, XGPU_MESH_STORE_BUFFER_REST,
		entry->vertex_size, NULL, &bytes);
	entry->indices = store->backend.buffer_create(store->backend.context, XGPU_MESH_STORE_BUFFER_INDICES,
		entry->index_size, NULL, &bytes);
	entry->staging = store->backend.buffer_create(store->backend.context, XGPU_MESH_STORE_BUFFER_STAGING,
		entry->vertex_size + entry->index_size, (void **)&entry->staging_mapped, &staged);
	store->resident_bytes += entry->bytes;
	if (!entry->rest || !entry->indices || !entry->staging || !entry->staging_mapped)
	{
		say(store, 1, "meshstore: %s: no buffers of %.1f MB: drawn stock", entry->path,
			(double)entry->bytes / 1048576.0);
		entry_buffers_destroy(store, entry);
		lock(store);
		entry->state = _entry_failed;
		unlock(store);
		store->statistics.failed++;
		return 0;
	}
	lock(store);
	if (store->queue_count == store->queue_capacity)
	{
		uint32_t capacity = store->queue_capacity ? store->queue_capacity * 2 : 64;
		uint32_t *grown = realloc(store->queue, capacity * sizeof(*grown));

		if (!grown)
		{
			entry->state = _entry_failed;
			unlock(store);
			entry_buffers_destroy(store, entry);
			return 0;
		}
		store->queue = grown;
		store->queue_capacity = capacity;
	}
	store->queue[store->queue_count++] = index;
	entry->state = _entry_queued;
#if STORE_FILES
	if (store->thread_made)
		pthread_cond_signal(&store->wake);
#endif
	unlock(store);
	return 1;
}

/* ---------- public code */

struct xgpu_mesh_store *xgpu_mesh_store_create(const struct xgpu_mesh_store_backend *backend,
	const struct xgpu_mesh_store_settings *settings)
{
	struct xgpu_mesh_store *store = calloc(1, sizeof(*store));

	if (!store)
		return NULL;
	store->backend = *backend;
	store->settings = *settings;
	if (!store->settings.budget_floor)
		store->settings.budget_floor = 128ull << 20;
	if (store->settings.budget_ceiling < store->settings.budget_floor)
		store->settings.budget_ceiling = store->settings.budget_floor;
#if STORE_FILES
	if (settings->workers)
	{
		pthread_mutex_init(&store->lock, NULL);
		pthread_cond_init(&store->wake, NULL);
		pthread_cond_init(&store->idle, NULL);
		if (pthread_create(&store->thread, NULL, worker_main, store) == 0)
			store->thread_made = 1;
		else
		{
			pthread_mutex_destroy(&store->lock);
			pthread_cond_destroy(&store->wake);
			pthread_cond_destroy(&store->idle);
		}
	}
#endif
	return store;
}

static void entries_free(struct xgpu_mesh_store *store)
{
	uint32_t index;

	reads_cancel(store);
	for (index = 0; index < store->entry_count; index++)
	{
		struct entry *entry = &store->entries[index];

		entry_buffers_destroy(store, entry);
		free(entry->path);
		free(entry->bone_nodes);
		free(entry->pieces);
		xgpu_rig_destroy(entry->rig);
	}
	free(store->entries);
	store->entries = NULL;
	store->entry_count = 0;
	store->resident_bytes = 0;
}

void xgpu_mesh_store_destroy(struct xgpu_mesh_store *store)
{
	if (!store)
		return;
	entries_free(store);
#if STORE_FILES
	if (store->thread_made)
	{
		pthread_mutex_lock(&store->lock);
		store->stopping = 1;
		pthread_cond_broadcast(&store->wake);
		pthread_mutex_unlock(&store->lock);
		pthread_join(store->thread, NULL);
		pthread_mutex_destroy(&store->lock);
		pthread_cond_destroy(&store->wake);
		pthread_cond_destroy(&store->idle);
	}
#endif
	free(store->queue);
	free(store);
}

void xgpu_mesh_store_map(struct xgpu_mesh_store *store, uint32_t generation, uint32_t budget_megabytes,
	uint32_t count, const struct xgpu_mesh_store_query *queries, struct xgpu_mesh_store_answer *answers)
{
	uint32_t index, accepted = 0;
	double started = seconds_now();

	entries_free(store);
	memset(&store->statistics, 0, sizeof(store->statistics));
	store->generation = generation;
	store->map_time = started;
	budget_set(store, budget_megabytes);
	if (!count)
		return;
	if (count > XGPU_MESH_STORE_ENTRIES_MAXIMUM)
		count = XGPU_MESH_STORE_ENTRIES_MAXIMUM;
	store->entries = calloc(count, sizeof(*store->entries));
	if (!store->entries)
	{
		for (index = 0; index < count; index++)
			answers[index].status = XGPU_STREAM_MESH_STORE_UNREADABLE;
		return;
	}
	store->entry_count = count;
	for (index = 0; index < count; index++)
	{
		struct entry *entry = &store->entries[index];
		char reason[160];
		int status;

		memset(&answers[index], 0, sizeof(answers[index]));
		status = query_check(store, &queries[index], entry, reason, sizeof(reason));
		answers[index].status = (uint32_t)status;
		if (status != XGPU_STREAM_MESH_STORE_ACCEPTED)
		{
			say(store, 1, "meshstore: %s refused (status %d)%s%s", queries[index].path ? queries[index].path : "?",
				status, reason[0] ? ": " : "", reason);
			free(entry->bone_nodes);
			free(entry->pieces);
			free(entry->path);
			xgpu_rig_destroy(entry->rig);
			memset(entry, 0, sizeof(*entry));
			entry->state = _entry_failed;
			continue;
		}
		entry->state = _entry_indexed;
		answers[index].id = XGPU_MESH_STORE_ID(generation, index);
		answers[index].render_radius = entry->radius;
		answers[index].bones = entry->bone_count;
		answers[index].lods = entry->lod_count;
		accepted++;
	}
	store->statistics.entries = accepted;
	say(store, 0, "meshstore: generation %u: %u files, %u accepted; budget %.1f MB; checked in %.1f ms", generation,
		count, accepted, (double)store->budget_bytes / 1048576.0, (seconds_now() - started) * 1000.0);
}

int xgpu_mesh_store_use(struct xgpu_mesh_store *store, uint32_t id, float lod_pixels,
	struct xgpu_mesh_store_view *view)
{
	struct entry *entry;
	uint32_t index = (id & 0xfffffu) - 1u, lod;
	int state;

	if (!store || !id)
		return 0;
	store->statistics.uses++;
	if ((id >> 20) != (store->generation & 0xfffu) || index >= store->entry_count)
	{
		store->statistics.stale_uses++;
		return 0;
	}
	entry = &store->entries[index];
	entry->used_frame = store->frame;
	lock(store);
	state = entry->state;
	unlock(store);
	if (state == _entry_indexed && store->frame >= entry->retry_frame)
		entry_queue(store, index);
	if (state != _entry_resident)
	{
		store->statistics.stock_uses++;
		return 0;
	}
	/* (the most detailed level drawn from a size the draw has: else the
	least) */
	for (lod = 0; lod + 1 < entry->lod_count && lod_pixels < entry->lods[lod].cutoff; lod++)
		;
	memset(view, 0, sizeof(*view));
	view->rest = entry->rest;
	view->indices = entry->indices;
	view->lod = lod;
	view->vertex_first = entry->lods[lod].vertex_first;
	view->vertex_count = entry->lods[lod].vertex_count;
	view->pieces = entry->pieces + entry->lods[lod].piece_first;
	view->piece_count = entry->lods[lod].piece_count;
	view->bone_count = entry->bone_count;
	view->bone_nodes = entry->bone_nodes;
	view->rig = entry->rig;
	view->node_count = entry->node_count;
	view->inverse_scale[0] = entry->inverse_scale[0];
	view->inverse_scale[1] = entry->inverse_scale[1];
	view->present = entry->present;
	return 1;
}

void xgpu_mesh_store_frame(struct xgpu_mesh_store *store)
{
	uint32_t index;

	if (!store)
		return;
	store->frame++;
	if (!store->budget_megabytes && store->frame % 600 == 0)
		budget_set(store, 0);
	lock(store);
	for (index = 0; index < store->entry_count; index++)
	{
		struct entry *entry = &store->entries[index];

		if (entry->state != _entry_read)
			continue;
		if (entry->read_failed)
		{
			say(store, 1, "meshstore: %s: %s: drawn stock", entry->path, entry->reason);
			entry_buffers_destroy(store, entry);
			entry->state = _entry_failed;
			store->statistics.failed++;
			continue;
		}
		store->backend.copy(store->backend.context, entry->staging, 0, entry->rest, 0, entry->vertex_size);
		store->backend.copy(store->backend.context, entry->staging, entry->vertex_size, entry->indices, 0,
			entry->index_size);
		store->backend.buffer_destroy(store->backend.context, entry->staging);
		entry->staging = NULL;
		entry->staging_mapped = NULL;
		entry->state = _entry_resident;
		store->statistics.read_bytes += entry->bytes;
		store->statistics.last_resident_seconds = seconds_now() - store->map_time;
		say(store, 0, "meshstore: %s in: %.1f MB, %u levels, %u pieces, %u bones, %.2f s after the map", entry->path,
			(double)entry->bytes / 1048576.0, entry->lod_count, entry->piece_count, entry->bone_count,
			store->statistics.last_resident_seconds);
		if (entry->uv_wide_vertices)
			say(store, 0, "meshstore: %s: %llu vertices' uv0 past the model's base map scale: clamped in the stock "
				"layout, whole in a material's draws", entry->path, (unsigned long long)entry->uv_wide_vertices);
	}
	unlock(store);
}

void xgpu_mesh_store_statistics(const struct xgpu_mesh_store *store, struct xgpu_mesh_store_statistics *statistics)
{
	uint32_t index;

	*statistics = store->statistics;
	statistics->resident = statistics->reading = 0;
	lock((struct xgpu_mesh_store *)store);
	for (index = 0; index < store->entry_count; index++)
	{
		int state = store->entries[index].state;

		statistics->resident += state == _entry_resident;
		statistics->reading += state == _entry_queued || state == _entry_reading || state == _entry_read;
	}
	unlock((struct xgpu_mesh_store *)store);
	statistics->resident_bytes = store->resident_bytes;
	statistics->budget_bytes = store->budget_bytes;
}

int xgpu_mesh_store_work(struct xgpu_mesh_store *store)
{
	struct entry *entry;
	char reason[160];

	if (!store->queue_count)
		return 0;
	entry = &store->entries[store->queue[0]];
	memmove(store->queue, store->queue + 1, (store->queue_count - 1) * sizeof(*store->queue));
	store->queue_count--;
	reason[0] = 0;
	entry->read_failed = entry_read(entry, entry->path, entry->staging_mapped, reason, sizeof(reason));
	memcpy(entry->reason, reason, sizeof(reason));
	entry->state = _entry_read;
	return 1;
}
