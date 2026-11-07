/*
MESH_STORE_TEST.C

PRISM's mesh store's core (xgpu_mesh_store.c) over a fake backend that keeps
each buffer in memory: the .pmesh checks at the map's record (magic, the
model's nodes, the parts it draws, the sections' CRCs, levels, helper bones,
a file changed since the guest found it, a path outside a mods folder), the
reading's checks (the vertices' and indices' CRCs, joints, weights, indices,
the stock layout's uv range), residency (stock until read, then each level
by its pixels, a helper bone skinned by its nearest node), the budget (the
least recently drawn let go of, nothing drawn lately let go of), a new
generation's ids and a closed store's buffers. Run by tools/test_mesh_store.py
with a folder to write its files in (argv[1]), and again with
ThreadSanitizer and the worker thread (argv[2] "threads").
*/

#include "xgpu_mesh_store.h"
#include "xgpu_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { failures++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* ---------- the fake backend */

struct fake_buffer
{
	int kind;
	uint64_t size;
	unsigned char *data;
};

static int live_buffers, copies;

static void *fake_create(void *context, int kind, uint64_t size, void **mapped, uint64_t *bytes)
{
	struct fake_buffer *buffer = calloc(1, sizeof(*buffer));

	(void)context;
	buffer->kind = kind;
	buffer->size = size;
	buffer->data = calloc(1, (size_t)size);
	if (mapped)
		*mapped = buffer->data;
	*bytes = size;
	live_buffers++;
	return buffer;
}

static void fake_destroy(void *context, void *opaque)
{
	struct fake_buffer *buffer = opaque;

	(void)context;
	free(buffer->data);
	free(buffer);
	live_buffers--;
}

static void fake_copy(void *context, void *from, uint64_t from_offset, void *to, uint64_t to_offset, uint64_t size)
{
	struct fake_buffer *source = from, *destination = to;

	(void)context;
	CHECK(from_offset + size <= source->size && to_offset + size <= destination->size);
	memcpy(destination->data + to_offset, source->data + from_offset, (size_t)size);
	copies++;
}

static int fake_memory(void *context, uint64_t *budget, uint64_t *usage)
{
	(void)context;
	*budget = 1ull << 30;
	*usage = 0;
	return 1;
}

static int verbose;

static void fake_log(void *context, int warning, const char *text)
{
	(void)context;
	if (verbose)
		printf("%s%s\n", warning ? "warning: " : "", text);
}

/* ---------- .pmesh files */

static void put32(unsigned char *at, uint32_t value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
	at[2] = (unsigned char)(value >> 16);
	at[3] = (unsigned char)(value >> 24);
}

static void put16(unsigned char *at, uint16_t value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
}

static void put_float(unsigned char *at, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	put32(at, bits);
}

/* what a made file breaks (one at a time) */
struct breakage
{
	int magic, node_crc, table, skeleton_crc, vertex_crc, joint, index, uv, lod_order, helper_parent, cut;
};

#define NODES 3
#define HELPERS 1
#define TRIANGLES 4
/* two levels: level 0 two pieces (region 0 permutation 0, shaders 1 and 2),
TRIANGLES triangles each of their own three vertices per triangle; level 1
one piece of one triangle */
static unsigned char *pmesh_make(const struct breakage *broken, uint32_t node_crc, uint64_t *size)
{
	uint32_t bones = NODES + HELPERS, levels = 2, pieces = 3;
	uint32_t vertex_count = 2 * TRIANGLES * 3 + 3, index_count = vertex_count;
	uint32_t skeleton_bytes = bones * XGPU_PMESH_BONE_BYTES + 32, offsets[5], sizes[5], kinds[5], at, section, index;
	unsigned char *file;

	kinds[0] = XGPU_PMESH_SKEL;
	kinds[1] = XGPU_PMESH_LODS;
	kinds[2] = XGPU_PMESH_SUBM;
	kinds[3] = XGPU_PMESH_VERT;
	kinds[4] = XGPU_PMESH_INDX;
	sizes[0] = skeleton_bytes;
	sizes[1] = levels * XGPU_PMESH_LOD_BYTES;
	sizes[2] = pieces * XGPU_PMESH_PIECE_BYTES;
	sizes[3] = vertex_count * XGPU_PMESH_VERTEX_BYTES;
	sizes[4] = index_count * 4;
	at = XGPU_PMESH_HEADER_BYTES + 5 * XGPU_PMESH_SECTION_BYTES;
	for (section = 0; section < 5; section++)
	{
		offsets[section] = at;
		at += (sizes[section] + 15) & ~15u;
	}
	*size = at;
	file = calloc(1, at);
	put32(file, broken->magic ? 0x12345678u : XGPU_PMESH_MAGIC);
	put16(file + 4, XGPU_PMESH_VERSION);
	put32(file + 8, (uint32_t)*size);
	put32(file + 12, 5);
	put32(file + 16, XGPU_PMESH_HEADER_BYTES);
	put32(file + 20, NODES);
	put32(file + 24, broken->node_crc ? node_crc ^ 1 : node_crc);
	put32(file + 28, bones);
	put32(file + 32, levels);
	put_float(file + 48, 0.875f);
	/* the skeleton: nodes 0..2 (0 the root), a helper on node 2 */
	for (index = 0; index < bones; index++)
	{
		unsigned char *bone = file + offsets[0] + index * XGPU_PMESH_BONE_BYTES;

		put32(bone, index * 7919u);
		put16(bone + 4, (uint16_t)(index ? (index == NODES ? (broken->helper_parent ? NODES + 1 : 2) : index - 1) :
			0xffff));
		put16(bone + 6, index < NODES ? 1 : 2);
		put16(bone + 8, (uint16_t)(index < NODES ? index : 0xffff));
		put_float(bone + 16, 1.0f);
		put_float(bone + 16 + 20, 1.0f);
		put_float(bone + 16 + 40, 1.0f);
	}
	/* the levels */
	{
		unsigned char *level = file + offsets[1];

		put_float(level, broken->lod_order ? 10.0f : 100.0f);
		put32(level + 4, 0);
		put32(level + 8, 2);
		put32(level + 12, 0);
		put32(level + 16, 2 * TRIANGLES * 3);
		put32(level + 20, 0);
		put32(level + 24, 2 * TRIANGLES * 3);
		level += XGPU_PMESH_LOD_BYTES;
		put_float(level, broken->lod_order ? 50.0f : 0.0f);
		put32(level + 4, 2);
		put32(level + 8, 1);
		put32(level + 12, 2 * TRIANGLES * 3);
		put32(level + 16, 3);
		put32(level + 20, 2 * TRIANGLES * 3);
		put32(level + 24, 3);
	}
	/* the pieces */
	for (index = 0; index < pieces; index++)
	{
		unsigned char *piece = file + offsets[2] + index * XGPU_PMESH_PIECE_BYTES;
		uint32_t first = index * TRIANGLES * 3, count = index < 2 ? TRIANGLES * 3 : 3;

		piece[0] = 0;
		piece[1] = 0;
		piece[2] = (unsigned char)(index == 1 ? 2 : 1);
		if (broken->table && index == 1)
			piece[2] = 7;
		put32(piece + 8, first);
		put32(piece + 12, count);
		put32(piece + 16, first);
		put32(piece + 20, count);
	}
	/* the vertices: each on a bone, the last on the helper */
	for (index = 0; index < vertex_count; index++)
	{
		unsigned char *vertex = file + offsets[3] + index * XGPU_PMESH_VERTEX_BYTES;

		put_float(vertex, (float)index * 0.01f);
		put_float(vertex + 24, broken->uv && index == 5 ? 3.0f : 0.25f);
		put_float(vertex + 28, -0.5f);
		put16(vertex + 32, (uint16_t)(broken->joint && index == 4 ? bones : index % bones));
		vertex[40] = index % 3 ? 255 : 0;
	}
	/* the indices: a piece's relative to its first vertex */
	for (index = 0; index < index_count; index++)
	{
		uint32_t relative = index < 2 * TRIANGLES * 3 ? index % (TRIANGLES * 3) : index - 2 * TRIANGLES * 3;

		put32(file + offsets[4] + index * 4, broken->index && index == 7 ? 99 : relative);
	}
	/* the table, each section's CRC */
	for (section = 0; section < 5; section++)
	{
		unsigned char *entry = file + XGPU_PMESH_HEADER_BYTES + section * XGPU_PMESH_SECTION_BYTES;
		uint32_t crc = xgpu_mesh_store_crc32(0, file + offsets[section], sizes[section]);

		if ((broken->skeleton_crc && section == 0) || (broken->vertex_crc && section == 3))
			crc ^= 0x100;
		put32(entry, kinds[section]);
		put32(entry + 4, offsets[section]);
		put32(entry + 8, sizes[section]);
		put32(entry + 12, crc);
	}
	if (broken->cut)
		*size = XGPU_PMESH_HEADER_BYTES + 10;
	return file;
}

static char folder[512];

static void file_write(const char *name, const struct breakage *broken, uint32_t node_crc,
	struct xgpu_mesh_store_query *query)
{
	static char paths[32][768];
	static int used;
	char *path = paths[used++ % 32];
	uint64_t size;
	unsigned char *data = pmesh_make(broken, node_crc, &size);
	FILE *file;
	struct stat information;

	snprintf(path, sizeof(paths[0]), "%s/%s", folder, name);
	file = fopen(path, "wb");
	fwrite(data, 1, (size_t)size, file);
	fclose(file);
	free(data);
	stat(path, &information);
	memset(query, 0, sizeof(*query));
	query->path = path;
	query->file_size = (uint64_t)information.st_size;
	query->stamp_seconds = (uint32_t)information.st_mtime;
	query->node_count = NODES;
	query->node_crc = node_crc;
	query->base_map_scale[0] = 1.0f;
	query->base_map_scale[1] = 1.0f;
}

static const uint8_t table[2][4] = { { 0, 0, 1, 0 }, { 0, 0, 2, 0 } };

/* reads done (the worker's, or here) and copied */
static void settle(struct xgpu_mesh_store *store, int threads)
{
	int round;

	for (round = 0; round < 200; round++)
	{
		struct xgpu_mesh_store_statistics statistics;

		if (!threads)
			while (xgpu_mesh_store_work(store))
				;
		xgpu_mesh_store_frame(store);
		xgpu_mesh_store_statistics(store, &statistics);
		if (!statistics.reading)
			return;
		if (threads)
			usleep(2000);
	}
}

int main(int argc, char **argv)
{
	struct xgpu_mesh_store_backend backend;
	struct xgpu_mesh_store_settings settings;
	struct xgpu_mesh_store *store;
	struct xgpu_mesh_store_query queries[16];
	struct xgpu_mesh_store_answer answers[17];
	struct xgpu_mesh_store_view view;
	struct xgpu_mesh_store_statistics statistics;
	struct breakage broken;
	int threads = argc > 2 && !strcmp(argv[2], "threads"), index, frame;
	uint32_t node_crc = 0xf346d05cu;

	if (argc < 2)
		return 2;
	verbose = getenv("MESH_STORE_VERBOSE") != NULL;
	snprintf(folder, sizeof(folder), "%s/mods", argv[1]);
	mkdir(folder, 0755);
	memset(&backend, 0, sizeof(backend));
	backend.buffer_create = fake_create;
	backend.buffer_destroy = fake_destroy;
	backend.copy = fake_copy;
	backend.memory = fake_memory;
	backend.log = fake_log;
	memset(&settings, 0, sizeof(settings));
	settings.workers = threads ? 1 : 0;
	settings.budget_floor = 1ull << 20;
	settings.budget_ceiling = 64ull << 20;
	store = xgpu_mesh_store_create(&backend, &settings);
	CHECK(store != NULL);

	/* ---- the map's checks */
	memset(&broken, 0, sizeof(broken));
	file_write("good.pmesh", &broken, node_crc, &queries[0]);
	broken.magic = 1;
	file_write("magic.pmesh", &broken, node_crc, &queries[1]);
	memset(&broken, 0, sizeof(broken));
	broken.node_crc = 1;
	file_write("nodes.pmesh", &broken, node_crc, &queries[2]);
	memset(&broken, 0, sizeof(broken));
	broken.table = 1;
	file_write("table.pmesh", &broken, node_crc, &queries[3]);
	memset(&broken, 0, sizeof(broken));
	broken.skeleton_crc = 1;
	file_write("skeleton.pmesh", &broken, node_crc, &queries[4]);
	memset(&broken, 0, sizeof(broken));
	broken.lod_order = 1;
	file_write("levels.pmesh", &broken, node_crc, &queries[5]);
	memset(&broken, 0, sizeof(broken));
	broken.helper_parent = 1;
	file_write("helper.pmesh", &broken, node_crc, &queries[6]);
	memset(&broken, 0, sizeof(broken));
	broken.cut = 1;
	file_write("cut.pmesh", &broken, node_crc, &queries[7]);
	memset(&broken, 0, sizeof(broken));
	file_write("changed.pmesh", &broken, node_crc, &queries[8]);
	queries[8].file_size++;
	file_write("wrong_name.txt", &broken, node_crc, &queries[9]);
	broken.vertex_crc = 1;
	file_write("vertex_crc.pmesh", &broken, node_crc, &queries[10]);
	memset(&broken, 0, sizeof(broken));
	broken.joint = 1;
	file_write("joint.pmesh", &broken, node_crc, &queries[11]);
	memset(&broken, 0, sizeof(broken));
	broken.index = 1;
	file_write("index.pmesh", &broken, node_crc, &queries[12]);
	memset(&broken, 0, sizeof(broken));
	broken.uv = 1;
	file_write("uv.pmesh", &broken, node_crc, &queries[13]);
	memset(&broken, 0, sizeof(broken));
	file_write("second.pmesh", &broken, node_crc, &queries[14]);
	for (index = 0; index < 15; index++)
	{
		queries[index].table = &table[0][0];
		queries[index].table_count = 2;
	}
	xgpu_mesh_store_map(store, 0x1234, 0, 15, queries, answers);
	CHECK(answers[0].status == XGPU_STREAM_MESH_STORE_ACCEPTED && answers[0].id == XGPU_MESH_STORE_ID(0x1234, 0) &&
		answers[0].render_radius == 0.875f && answers[0].bones == NODES + HELPERS && answers[0].lods == 2);
	CHECK(!answers[1].id && answers[1].status == XGPU_STREAM_MESH_STORE_FORMAT);
	CHECK(!answers[2].id && answers[2].status == XGPU_STREAM_MESH_STORE_NODES);
	CHECK(!answers[3].id && answers[3].status == XGPU_STREAM_MESH_STORE_TABLE);
	CHECK(!answers[4].id && answers[4].status == XGPU_STREAM_MESH_STORE_FORMAT);
	CHECK(!answers[5].id && answers[5].status == XGPU_STREAM_MESH_STORE_FORMAT);
	CHECK(!answers[6].id && answers[6].status == XGPU_STREAM_MESH_STORE_FORMAT);
	CHECK(!answers[7].id && answers[7].status == XGPU_STREAM_MESH_STORE_FORMAT);
	CHECK(!answers[8].id && answers[8].status == XGPU_STREAM_MESH_STORE_CHANGED);
	CHECK(!answers[9].id && answers[9].status == XGPU_STREAM_MESH_STORE_PATH);
	/* (the read's checks pass the map's) */
	for (index = 10; index < 15; index++)
		CHECK(answers[index].id == XGPU_MESH_STORE_ID(0x1234, (uint32_t)index));
	CHECK(live_buffers == 0);

	/* ---- residency: stock until read, then a level by its pixels */
	CHECK(!xgpu_mesh_store_use(store, answers[0].id, 500.0f, &view));
	CHECK(live_buffers == 3);
	settle(store, threads);
	CHECK(live_buffers == 2 && copies == 2);
	CHECK(xgpu_mesh_store_use(store, answers[0].id, 500.0f, &view));
	CHECK(view.lod == 0 && view.vertex_first == 0 && view.vertex_count == 2 * TRIANGLES * 3 && view.piece_count == 2 &&
		view.pieces[1].shader == 2 && view.bone_count == NODES + HELPERS && view.node_count == NODES);
	/* (the helper bone skinned by its parent's node, node 2) */
	CHECK(view.bone_nodes[0] == 0 && view.bone_nodes[2] == 2 && view.bone_nodes[NODES] == 2);
	CHECK(view.present[0] & 1);
	/* (the vertices and indices copied as the file has them, the weightless
	vertex given its first joint's whole) */
	{
		const struct fake_buffer *rest = view.rest, *indices = view.indices;

		CHECK(rest->size == (2 * TRIANGLES * 3 + 3) * XGPU_PMESH_VERTEX_BYTES && rest->data[40] == 255 &&
			rest->data[XGPU_PMESH_VERTEX_BYTES + 40] == 255);
		CHECK(indices->size == (2 * TRIANGLES * 3 + 3) * 4 && indices->data[4 * 13] == 1);
	}
	CHECK(xgpu_mesh_store_use(store, answers[0].id, 20.0f, &view));
	CHECK(view.lod == 1 && view.vertex_first == 2 * TRIANGLES * 3 && view.vertex_count == 3 && view.piece_count == 1);
	/* ---- the read's checks: each drawn stock for good */
	for (index = 10; index < 14; index++)
	{
		CHECK(!xgpu_mesh_store_use(store, answers[index].id, 500.0f, &view));
		settle(store, threads);
		CHECK(!xgpu_mesh_store_use(store, answers[index].id, 500.0f, &view));
	}
	xgpu_mesh_store_statistics(store, &statistics);
	CHECK(statistics.failed == 4 && statistics.resident == 1 && statistics.entries == 6);
	CHECK(live_buffers == 2);

	/* ---- the budget: room for one of two meshes, the other let go of only
	when not drawn lately */
	xgpu_mesh_store_map(store, 0x1235, 0, 0, NULL, NULL);
	CHECK(live_buffers == 0);
	{
		uint64_t one = (2 * TRIANGLES * 3 + 3) * (XGPU_PMESH_VERTEX_BYTES + 4);
		struct xgpu_mesh_store_settings small = settings;

		xgpu_mesh_store_destroy(store);
		small.budget_floor = one + one / 2;
		small.budget_ceiling = small.budget_floor;
		store = xgpu_mesh_store_create(&backend, &small);
		xgpu_mesh_store_map(store, 0x1236, 0, 15, queries, answers);
		CHECK(answers[0].id && answers[14].id);
		CHECK(!xgpu_mesh_store_use(store, answers[0].id, 500.0f, &view));
		settle(store, threads);
		CHECK(xgpu_mesh_store_use(store, answers[0].id, 500.0f, &view));
		/* (the second does not fit while the first is drawn) */
		CHECK(!xgpu_mesh_store_use(store, answers[14].id, 500.0f, &view));
		settle(store, threads);
		CHECK(!xgpu_mesh_store_use(store, answers[14].id, 500.0f, &view));
		xgpu_mesh_store_statistics(store, &statistics);
		CHECK(statistics.refused_budget >= 1 && statistics.resident == 1);
		/* (the first not drawn for a while: let go of for the second, once
		its retry comes) */
		for (frame = 0; frame < (int)XGPU_MESH_STORE_RETRY_FRAMES + 2; frame++)
			xgpu_mesh_store_frame(store);
		CHECK(!xgpu_mesh_store_use(store, answers[14].id, 500.0f, &view));
		settle(store, threads);
		CHECK(xgpu_mesh_store_use(store, answers[14].id, 500.0f, &view));
		xgpu_mesh_store_statistics(store, &statistics);
		CHECK(statistics.evictions == 1 && statistics.resident == 1);
		CHECK(!xgpu_mesh_store_use(store, answers[0].id, 500.0f, &view));
		/* ---- a new generation: the old ids stale, the buffers let go of */
		xgpu_mesh_store_map(store, 0x1237, 0, 1, queries, &answers[15]);
		CHECK(answers[15].id == XGPU_MESH_STORE_ID(0x1237, 0) && answers[15].id != answers[0].id);
		CHECK(!xgpu_mesh_store_use(store, answers[14].id, 500.0f, &view));
		xgpu_mesh_store_statistics(store, &statistics);
		CHECK(statistics.stale_uses == 1);
		CHECK(live_buffers == 0);
	}
	xgpu_mesh_store_destroy(store);
	CHECK(live_buffers == 0);
	if (failures)
	{
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
