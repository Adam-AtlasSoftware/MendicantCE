/*
TEXTURE_STORE_TEST.C

PRISM's texture store's core (xgpu_texture_store.c, xgpu_ktx2.c) over a fake
backend that keeps each image's levels in memory: the KTX2 rules, the
map's answers, and the residency (levels smallest first, usable at the
stock resolution, the frame's bytes, bands, the staging ring given back
only after its submission, the budget's cuts least recently drawn first,
nothing drawn lately cut, the thrash guard, a generation cancelling the
pieces in flight, the device's memory running out). Run by
tools/test_texture_store.py with a folder to write its files in (argv[1]),
and again with ThreadSanitizer and two workers (argv[2] "threads").
*/

#include "xgpu_texture_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { failures++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

/* ---------- KTX2 files */

static unsigned char pattern(uint64_t offset, uint32_t salt)
{
	uint64_t value = (offset + salt) * 0x9e3779b97f4a7c15ull;

	return (unsigned char)(value >> 56);
}

struct made
{
	uint32_t vk_format, width, height, depth, layers, faces, levels, supercompression;
	uint32_t block_bytes;
	/* one-field breakages: the level index's first offset past the file, a
	level's length wrong, the file cut short */
	int offset_outside, length_wrong, truncate;
	uint32_t salt;
};

static void put32(unsigned char *at, uint32_t value)
{
	at[0] = (unsigned char)value;
	at[1] = (unsigned char)(value >> 8);
	at[2] = (unsigned char)(value >> 16);
	at[3] = (unsigned char)(value >> 24);
}

static void put64(unsigned char *at, uint64_t value)
{
	put32(at, (uint32_t)value);
	put32(at + 4, (uint32_t)(value >> 32));
}

static uint32_t whole_levels(uint32_t width, uint32_t height)
{
	uint32_t side = width > height ? width : height, levels = 1;

	while (side > 1)
	{
		side >>= 1;
		levels++;
	}
	return levels;
}

/* a KTX2 file's bytes (levels stored smallest first, as the tools do) */
static unsigned char *ktx2_make(const struct made *made, uint64_t *size)
{
	static const unsigned char identifier[12] = { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
	uint32_t levels = made->levels, level, index_levels = levels ? levels : 1;
	uint64_t data = 80 + 24ull * index_levels, total = data, at;
	uint64_t lengths[16];
	unsigned char *bytes;

	for (level = 0; level < index_levels && level < 16; level++)
	{
		uint32_t width = made->width >> level ? made->width >> level : 1;
		uint32_t height = made->height >> level ? made->height >> level : 1;

		lengths[level] = (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * made->block_bytes * made->faces;
		total += lengths[level];
	}
	bytes = calloc(1, (size_t)total);
	memcpy(bytes, identifier, 12);
	put32(bytes + 12, made->vk_format);
	put32(bytes + 16, 1);
	put32(bytes + 20, made->width);
	put32(bytes + 24, made->height);
	put32(bytes + 28, made->depth);
	put32(bytes + 32, made->layers);
	put32(bytes + 36, made->faces);
	put32(bytes + 40, made->levels);
	put32(bytes + 44, made->supercompression);
	at = data;
	for (level = index_levels; level-- > 0;)
	{
		uint64_t offset;

		if (level >= 16)
			continue;
		put64(bytes + 80 + 24 * level, at);
		put64(bytes + 80 + 24 * level + 8, lengths[level] + (made->length_wrong && level == 0 ? 16 : 0));
		for (offset = 0; offset < lengths[level]; offset++)
			bytes[at + offset] = pattern(at + offset, made->salt);
		at += lengths[level];
	}
	if (made->offset_outside)
		put64(bytes + 80, total + 4);
	*size = made->truncate ? total - 32 : total;
	return bytes;
}

static struct made made_bc7(uint32_t width, uint32_t height)
{
	struct made made;

	memset(&made, 0, sizeof(made));
	made.vk_format = 145;
	made.width = width;
	made.height = height;
	made.faces = 1;
	made.levels = whole_levels(width, height);
	made.block_bytes = 16;
	return made;
}

static int parse_made(const struct made *made, const struct xgpu_ktx2_stock *stock, struct xgpu_ktx2_file *file)
{
	uint64_t size;
	unsigned char *bytes = ktx2_make(made, &size);
	int status = xgpu_ktx2_parse(bytes, size < XGPU_KTX2_INDEX_BYTES ? (uint32_t)size : XGPU_KTX2_INDEX_BYTES, size,
		stock, XGPU_KTX2_SIDE_MAXIMUM, file);

	free(bytes);
	return status;
}

static void test_ktx2(void)
{
	struct xgpu_ktx2_stock stock = { 256, 256, 9, 0 }, cube = { 64, 64, 7, 1 }, wide = { 256, 128, 9, 0 };
	struct xgpu_ktx2_file file;
	struct made made;

	/* (k 0, 1, 2) */
	made = made_bc7(256, 256);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.scale == 0 && file.levels == 9);
	made = made_bc7(512, 512);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.scale == 1);
	made = made_bc7(1024, 1024);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.scale == 2 && file.levels == 11 &&
		file.block_bytes == 16 && file.format == XGPU_KTX2_FORMAT_BC7);
	made = made_bc7(1024, 512);
	CHECK(parse_made(&made, &wide, &file) == XGPU_KTX2_ACCEPTED && file.scale == 2);
	/* (an aspect not the stock's, k not the same on both sides) */
	made = made_bc7(1024, 1024);
	CHECK(parse_made(&made, &wide, &file) == XGPU_KTX2_SIZE);
	made = made_bc7(1024, 512);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_SIZE);
	/* (smaller than the stock) */
	made = made_bc7(128, 128);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_SIZE);
	/* (past 4096) */
	made = made_bc7(8192, 8192);
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_SIZE);
	/* (not a power of two) */
	made = made_bc7(768, 768);
	CHECK(parse_made(&made, NULL, &file) == XGPU_KTX2_SIZE);
	/* (each field broken alone) */
	made = made_bc7(1024, 1024);
	made.supercompression = 2;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.levels = 0;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.levels = 10;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.depth = 1;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.layers = 2;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.vk_format = 37;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.offset_outside = 1;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.length_wrong = 1;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	made = made_bc7(1024, 1024);
	made.truncate = 1;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	/* (faces against the stock's kind) */
	made = made_bc7(256, 256);
	made.faces = 6;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	CHECK(parse_made(&made, &cube, &file) == XGPU_KTX2_ACCEPTED && file.scale == 2 && file.faces == 6);
	made.faces = 1;
	CHECK(parse_made(&made, &cube, &file) == XGPU_KTX2_INVALID);
	/* (sRGB read as UNORM; BC1 and BC4 blocks of 8 bytes) */
	made = made_bc7(512, 512);
	made.vk_format = 146;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.srgb && file.format == XGPU_KTX2_FORMAT_BC7);
	made.vk_format = 131;
	made.block_bytes = 8;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.format == XGPU_KTX2_FORMAT_BC1_RGB &&
		file.block_bytes == 8);
	made.vk_format = 139;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_ACCEPTED && file.format == XGPU_KTX2_FORMAT_BC4);
	/* (BC1's 8-byte blocks with BC7's lengths: the lengths are wrong) */
	made.block_bytes = 16;
	CHECK(parse_made(&made, &stock, &file) == XGPU_KTX2_INVALID);
	/* (a header alone) */
	CHECK(xgpu_ktx2_parse("KTX", 3, 3, &stock, 4096, &file) == XGPU_KTX2_UNREADABLE);
}

/* ---------- the fake backend */

struct fake_image
{
	struct xgpu_texture_store_image_description description;
	unsigned char *levels[16];
	uint64_t level_bytes[16];
	int destroyed;
	struct fake_image *next;
};

static struct
{
	unsigned char *staging;
	uint32_t staging_size;
	uint64_t serial, completed;
	/* completed follows serial (the GPU keeps up), else the test moves it */
	int gpu_follows;
	int fail_next;
	uint64_t memory_budget, memory_usage;
	struct fake_image *images;
	uint32_t live_images;
	uint64_t frame_copied;
	uint32_t copies;
	uint32_t copy_levels[4096];
	int written_after_destroy;
	int stale_staging;
	int asked_astc;
	/* the file whose bytes a copy's staging must hold, for the check */
	const unsigned char *expected;
	uint64_t expected_size;
	uint32_t expected_salt;
	int quiet;
} fake;

static void *fake_image_create(void *context, const struct xgpu_texture_store_image_description *description,
	uint64_t *bytes)
{
	struct fake_image *image;
	uint32_t level, faces = description->cube ? 6 : 1, block = xgpu_ktx2_block_bytes(description->format);

	(void)context;
	if (fake.fail_next)
	{
		fake.fail_next--;
		return NULL;
	}
	image = calloc(1, sizeof(*image));
	image->description = *description;
	*bytes = 0;
	for (level = 0; level < description->levels; level++)
	{
		uint32_t width = description->width >> level ? description->width >> level : 1;
		uint32_t height = description->height >> level ? description->height >> level : 1;

		image->level_bytes[level] = (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * block * faces;
		image->levels[level] = calloc(1, (size_t)image->level_bytes[level]);
		*bytes += image->level_bytes[level];
	}
	image->next = fake.images;
	fake.images = image;
	fake.live_images++;
	return image;
}

static void fake_image_destroy(void *context, void *pointer)
{
	struct fake_image *image = pointer;

	(void)context;
	image->destroyed = 1;
	fake.live_images--;
}

static void fake_copy(void *context, const struct xgpu_texture_store_copy *copy)
{
	struct fake_image *image = copy->image;
	uint32_t faces = image->description.cube ? 6 : 1;
	uint64_t face_bytes = image->level_bytes[copy->level] / faces;
	uint64_t at = copy->face * face_bytes + (uint64_t)(copy->y / 4) * copy->row_bytes;

	(void)context;
	if (image->destroyed)
		fake.written_after_destroy = 1;
	if (copy->level >= image->description.levels || at + copy->bytes > image->level_bytes[copy->level] ||
		copy->staging_offset + copy->bytes > fake.staging_size)
	{
		fake.stale_staging = 1;
		return;
	}
	memcpy(image->levels[copy->level] + at, fake.staging + copy->staging_offset, copy->bytes);
	fake.frame_copied += copy->bytes;
	if (fake.copies < 4096)
		fake.copy_levels[fake.copies] = copy->level + (image->description.levels << 8);
	fake.copies++;
}

static void fake_image_copy_level(void *context, void *source, uint32_t source_level, void *destination,
	uint32_t destination_level)
{
	struct fake_image *from = source, *to = destination;

	(void)context;
	if (from->level_bytes[source_level] == to->level_bytes[destination_level])
		memcpy(to->levels[destination_level], from->levels[source_level], (size_t)from->level_bytes[source_level]);
	else
		fake.stale_staging = 1;
}

static uint64_t fake_serial(void *context)
{
	(void)context;
	return fake.serial;
}

static uint64_t fake_completed(void *context)
{
	(void)context;
	return fake.completed;
}

static int fake_memory(void *context, uint64_t *budget, uint64_t *usage)
{
	(void)context;
	*budget = fake.memory_budget;
	*usage = fake.memory_usage;
	return fake.memory_budget != 0;
}

static int fake_format_supported(void *context, uint8_t format)
{
	(void)context;
	if (format == XGPU_KTX2_FORMAT_ASTC)
		fake.asked_astc = 1;
	return format != XGPU_KTX2_FORMAT_ASTC;
}

static void fake_log(void *context, int warning, const char *text)
{
	(void)context;
	if (!fake.quiet)
		fprintf(stderr, "%s%s\n", warning ? "warning: " : "", text);
}

static struct xgpu_texture_store *store_make(uint32_t staging, uint32_t piece, uint32_t frame_bytes, uint32_t workers)
{
	struct xgpu_texture_store_backend backend;
	struct xgpu_texture_store_settings settings;

	free(fake.staging);
	fake.staging = calloc(1, staging);
	fake.staging_size = staging;
	memset(&backend, 0, sizeof(backend));
	backend.image_create = fake_image_create;
	backend.image_destroy = fake_image_destroy;
	backend.copy = fake_copy;
	backend.image_copy_level = fake_image_copy_level;
	backend.serial = fake_serial;
	backend.completed = fake_completed;
	backend.memory = fake_memory;
	backend.format_supported = fake_format_supported;
	backend.log = fake_log;
	backend.staging = fake.staging;
	backend.staging_size = staging;
	memset(&settings, 0, sizeof(settings));
	settings.workers = workers;
	settings.piece_maximum = piece;
	settings.frame_bytes = frame_bytes;
	settings.loading_bytes = frame_bytes * 8;
	settings.budget_floor = 1u << 20;
	settings.budget_ceiling = 1ull << 34;
	return xgpu_texture_store_create(&backend, &settings);
}

/* a frame: the store's start, the submission ended (the GPU done with it
if it follows) */
static void frame(struct xgpu_texture_store *store)
{
	fake.frame_copied = 0;
	xgpu_texture_store_frame(store);
	fake.serial++;
	if (fake.gpu_follows)
		fake.completed = fake.serial - 1;
}

static char folder[4096];

static void file_write(const char *path, const unsigned char *bytes, uint64_t size)
{
	FILE *file = fopen(path, "wb");

	fwrite(bytes, 1, (size_t)size, file);
	fclose(file);
}

static void query_make(struct xgpu_store_query *query, const char *path, const struct xgpu_ktx2_stock *stock)
{
	struct stat information;

	memset(query, 0, sizeof(*query));
	query->path = path;
	if (stat(path, &information) == 0)
	{
		query->file_size = (uint64_t)information.st_size;
		query->stamp_seconds = (uint32_t)information.st_mtime;
	}
	query->stock = *stock;
}

/* an image's levels against the file's */
static int image_matches(const struct fake_image *image, const unsigned char *bytes, uint32_t file_levels,
	uint32_t top, uint32_t from_level)
{
	uint32_t level;

	for (level = from_level; level < file_levels; level++)
	{
		uint64_t offset = (uint64_t)bytes[80 + 24 * level] | ((uint64_t)bytes[81 + 24 * level] << 8) |
			((uint64_t)bytes[82 + 24 * level] << 16) | ((uint64_t)bytes[83 + 24 * level] << 24);

		if (level < top || memcmp(image->levels[level - top], bytes + offset, (size_t)image->level_bytes[level - top]))
			return 0;
	}
	return 1;
}

static void run_work(struct xgpu_texture_store *store, int threads)
{
	if (threads)
	{
		struct timespec wait = { 0, 2000000 };

		nanosleep(&wait, NULL);
	}
	else
	{
		while (xgpu_texture_store_work(store))
		{
		}
	}
}

static void test_map(void)
{
	struct xgpu_texture_store *store = store_make(1u << 20, 256u << 10, 1u << 20, 0);
	struct xgpu_ktx2_stock stock = { 64, 64, 7, 0 };
	struct xgpu_store_query queries[6];
	struct xgpu_store_answer answers[6];
	char paths[6][4200];
	struct made made = made_bc7(256, 256);
	unsigned char *bytes;
	uint64_t size;

	fake.quiet = 1;
	snprintf(paths[0], sizeof(paths[0]), "%s/mods/test/textures/max/good#0.ktx2", folder);
	snprintf(paths[1], sizeof(paths[1]), "%s/mods/test/textures/max/missing#0.ktx2", folder);
	snprintf(paths[2], sizeof(paths[2]), "%s/outside#0.ktx2", folder);
	snprintf(paths[3], sizeof(paths[3]), "%s/mods/test/textures/max/astc#0.ktx2", folder);
	snprintf(paths[4], sizeof(paths[4]), "%s/mods/test/textures/max/good#0.ktx2", folder);
	snprintf(paths[5], sizeof(paths[5]), "%s/mods/test/textures/max/good#0.ktx2", folder);
	bytes = ktx2_make(&made, &size);
	file_write(paths[0], bytes, size);
	file_write(paths[2], bytes, size);
	free(bytes);
	made.vk_format = 157;
	bytes = ktx2_make(&made, &size);
	file_write(paths[3], bytes, size);
	free(bytes);
	query_make(&queries[0], paths[0], &stock);
	query_make(&queries[1], paths[1], &stock);
	query_make(&queries[2], paths[2], &stock);
	query_make(&queries[3], paths[3], &stock);
	query_make(&queries[4], paths[4], &stock);
	queries[4].file_size++;
	query_make(&queries[5], paths[5], &stock);
	queries[5].stamp_seconds--;
	xgpu_texture_store_map(store, 7, 0, 0, 6, queries, answers);
	CHECK(answers[0].id == XGPU_TEXTURE_STORE_ID(7, 0) && answers[0].status == XGPU_KTX2_ACCEPTED &&
		answers[0].width == 256 && answers[0].levels == 9 && answers[0].format == XGPU_KTX2_FORMAT_BC7);
	CHECK(!answers[1].id && answers[1].status == XGPU_KTX2_UNREADABLE);
	CHECK(!answers[2].id && answers[2].status == XGPU_KTX2_PATH);
	CHECK(!answers[3].id && answers[3].status == XGPU_KTX2_DEVICE && fake.asked_astc);
	CHECK(!answers[4].id && answers[4].status == XGPU_KTX2_CHANGED);
	CHECK(!answers[5].id && answers[5].status == XGPU_KTX2_CHANGED);
	/* (an id of another generation, or none, is drawn stock) */
	{
		struct xgpu_texture_store_view view;

		CHECK(!xgpu_texture_store_use(store, XGPU_TEXTURE_STORE_ID(6, 0), &view));
		CHECK(!xgpu_texture_store_use(store, XGPU_TEXTURE_STORE_ID(7, 5), &view));
		CHECK(!xgpu_texture_store_use(store, 0, &view));
	}
	xgpu_texture_store_destroy(store);
	fake.quiet = 0;
}

/* levels smallest first, usable at the stock resolution, then the levels
above one at a time; bands; the frame's bytes; staging given back only after
its submission */
static void test_residency(int threads)
{
	/* (a 1024 BC7 over a 256 stock: k 2; level 0 is 1 MB, read in 64 KB
	bands; 256 KB of staging; 128 KB a frame) */
	struct xgpu_texture_store *store = store_make(256u << 10, 64u << 10, 128u << 10, threads ? 2 : 0);
	struct xgpu_ktx2_stock stock = { 256, 256, 9, 0 };
	struct xgpu_store_query query;
	struct xgpu_store_answer answer;
	struct xgpu_texture_store_view view;
	struct made made = made_bc7(1024, 1024);
	char path[4200];
	unsigned char *bytes;
	uint64_t size, most = 0;
	int frames, usable_frame = -1, full_frame = -1, usable_at_scale = 0, stalled = 0;
	uint32_t seen_resident = 99;

	made.salt = 3;
	snprintf(path, sizeof(path), "%s/mods/test/textures/max/chief#0.ktx2", folder);
	bytes = ktx2_make(&made, &size);
	file_write(path, bytes, size);
	query_make(&query, path, &stock);
	fake.serial = 1;
	fake.completed = 0;
	fake.gpu_follows = 0;
	fake.copies = 0;
	xgpu_texture_store_map(store, 1, 64, 0, 1, &query, &answer);
	CHECK(answer.id != 0);
	CHECK(!xgpu_texture_store_use(store, answer.id, &view));
	for (frames = 0; frames < 2000 && full_frame < 0; frames++)
	{
		int usable = xgpu_texture_store_use(store, answer.id, &view);

		if (usable && usable_frame < 0)
		{
			usable_frame = frames;
			usable_at_scale = view.resident == 2 && view.scale == 2 && view.top == 0;
		}
		if (usable)
		{
			/* (the levels above come one at a time, never back) */
			CHECK(view.resident <= seen_resident);
			seen_resident = view.resident;
			if (view.resident == 0)
				full_frame = frames;
		}
		run_work(store, threads);
		/* (the GPU lags: the pieces are not given back until it is done,
		which the first frames do not let it be) */
		if (frames < 6)
		{
			fake.gpu_follows = 0;
			if (!threads && frames == 5 && xgpu_texture_store_work(store) == 0)
				stalled = 1;
		}
		else
		{
			fake.gpu_follows = 1;
		}
		frame(store);
		if (fake.frame_copied > most)
			most = fake.frame_copied;
	}
	CHECK(usable_frame >= 0 && usable_at_scale);
	CHECK(full_frame > usable_frame);
	/* (the frame's bytes: 128 KB, pieces of 64 KB) */
	CHECK(most <= 128u << 10);
	if (!threads)
	{
		/* (with the GPU behind, the ring filled and the reading waited) */
		CHECK(stalled);
		/* (the first copies were the smallest levels, from level 10 (the
		image's 10 of 11) down) */
		CHECK(fake.copies > 16 && (fake.copy_levels[0] & 0xff) == 10 && (fake.copy_levels[1] & 0xff) == 9);
	}
	CHECK(!fake.stale_staging && !fake.written_after_destroy);
	CHECK(fake.images && image_matches(fake.images, bytes, 11, 0, 0));
	{
		struct xgpu_texture_store_statistics statistics;

		xgpu_texture_store_statistics(store, &statistics);
		CHECK(statistics.full == 1 && statistics.usable == 1 && statistics.uploaded_bytes >= size - 80 - 24 * 11);
	}
	free(bytes);
	xgpu_texture_store_destroy(store);
	CHECK(fake.live_images == 0);
}

/* the budget: the least recently drawn cut back to the stock resolution,
then let go of; nothing drawn in the last frames; the thrash guard; the
device's memory running out; a generation cancelling the pieces in flight */
static void test_budget(void)
{
	struct xgpu_texture_store *store = store_make(4u << 20, 1u << 20, 64u << 20, 0);
	struct xgpu_ktx2_stock stock = { 256, 256, 9, 0 };
	struct xgpu_store_query queries[3];
	struct xgpu_store_answer answers[3];
	struct xgpu_texture_store_view view;
	struct xgpu_texture_store_statistics statistics;
	char paths[3][4200];
	unsigned char *bytes[3];
	uint64_t size;
	uint32_t state;
	int index, frames;

	fake.quiet = 1;
	fake.gpu_follows = 1;
	fake.memory_budget = 0;
	for (index = 0; index < 3; index++)
	{
		struct made made = made_bc7(1024, 1024);

		made.salt = (uint32_t)index * 77;
		snprintf(paths[index], sizeof(paths[index]), "%s/mods/test/textures/max/b%d#0.ktx2", folder, index);
		bytes[index] = ktx2_make(&made, &size);
		file_write(paths[index], bytes[index], size);
		query_make(&queries[index], paths[index], &stock);
	}
	/* (a 1024 BC7 chain is 1398128 bytes, from the stock resolution down
	87408: a budget of 3 MB holds two whole and a third cut back) */
	xgpu_texture_store_map(store, 2, 3, 0, 3, queries, answers);
	for (frames = 0; frames < 20; frames++)
	{
		xgpu_texture_store_use(store, answers[0].id, &view);
		run_work(store, 0);
		frame(store);
	}
	for (frames = 0; frames < 20; frames++)
	{
		xgpu_texture_store_use(store, answers[1].id, &view);
		run_work(store, 0);
		frame(store);
	}
	CHECK(xgpu_texture_store_use(store, answers[0].id, &view) && view.resident == 0);
	/* (0 drawn just now: never cut while the third comes in; 1, the least
	recently drawn, is) */
	for (frames = 0; frames < 20; frames++)
	{
		xgpu_texture_store_use(store, answers[0].id, &view);
		xgpu_texture_store_use(store, answers[2].id, &view);
		run_work(store, 0);
		frame(store);
	}
	xgpu_texture_store_statistics(store, &statistics);
	CHECK(statistics.resident_bytes <= statistics.budget_bytes);
	CHECK(statistics.demotions == 1 && statistics.evictions == 0);
	CHECK(xgpu_texture_store_use(store, answers[0].id, &view) && view.top == 0 && view.resident == 0);
	CHECK(xgpu_texture_store_use(store, answers[2].id, &view) && view.top == 0 && view.resident == 0);
	/* (cut back to its stock resolution, its levels kept) */
	CHECK(xgpu_texture_store_describe(store, answers[1].id, &view, &state) && state == XGPU_TEXTURE_STORE_DEMOTED &&
		view.top == 2 && view.image && image_matches(view.image, bytes[1], 11, 2, 2));
	/* (a budget of 1 MB with 2 drawn: 0 cut back, then 1 and 0 let go of,
	the least recently drawn first; 2 kept though it is over) */
	xgpu_texture_store_budget(store, 1);
	for (frames = 0; frames < 15; frames++)
	{
		xgpu_texture_store_use(store, answers[2].id, &view);
		run_work(store, 0);
		frame(store);
	}
	xgpu_texture_store_statistics(store, &statistics);
	CHECK(statistics.evictions == 2 && statistics.demotions == 2);
	CHECK(xgpu_texture_store_use(store, answers[2].id, &view) && view.resident == 0);
	CHECK(xgpu_texture_store_describe(store, answers[1].id, &view, &state) && !view.image);
	/* (thrash guard: 1 let go of, drawn again with room, is not made again
	until it has been drawn 30 frames in a row) */
	xgpu_texture_store_budget(store, 64);
	for (frames = 0; frames < 29; frames++)
	{
		xgpu_texture_store_use(store, answers[1].id, &view);
		frame(store);
	}
	CHECK(xgpu_texture_store_describe(store, answers[1].id, &view, &state) && !view.image);
	for (frames = 0; frames < 3; frames++)
	{
		xgpu_texture_store_use(store, answers[1].id, &view);
		run_work(store, 0);
		frame(store);
	}
	CHECK(xgpu_texture_store_describe(store, answers[1].id, &view, &state) && view.image);
	/* (the device's memory out: the entry drawn stock, the budget lowered) */
	{
		uint64_t before;

		xgpu_texture_store_statistics(store, &statistics);
		before = statistics.budget_bytes;
		xgpu_texture_store_map(store, 3, 64, 0, 3, queries, answers);
		fake.fail_next = 1;
		xgpu_texture_store_use(store, answers[0].id, &view);
		frame(store);
		xgpu_texture_store_statistics(store, &statistics);
		CHECK(statistics.allocation_failures == 1 && statistics.budget_bytes < before);
		CHECK(!xgpu_texture_store_use(store, answers[0].id, &view));
	}
	/* (a generation with pieces in flight: none copied into the old images) */
	{
		uint32_t images = fake.live_images;

		xgpu_texture_store_map(store, 4, 64, 0, 3, queries, answers);
		for (index = 0; index < 3; index++)
			xgpu_texture_store_use(store, answers[index].id, &view);
		frame(store);
		/* (some read, none copied yet) */
		xgpu_texture_store_work(store);
		xgpu_texture_store_work(store);
		xgpu_texture_store_map(store, 5, 64, 0, 3, queries, answers);
		CHECK(fake.live_images == 0 || fake.live_images <= images);
		frame(store);
		run_work(store, 0);
		frame(store);
		CHECK(!fake.written_after_destroy);
		CHECK(!xgpu_texture_store_use(store, XGPU_TEXTURE_STORE_ID(4, 0), &view));
	}
	for (index = 0; index < 3; index++)
		free(bytes[index]);
	xgpu_texture_store_destroy(store);
	CHECK(fake.live_images == 0);
	fake.quiet = 0;
}

int main(int argc, char **argv)
{
	char mods[4300];
	int threads = argc > 2 && !strcmp(argv[2], "threads");

	if (argc < 2)
	{
		fprintf(stderr, "texture_store_test <folder> [threads]\n");
		return 2;
	}
	snprintf(folder, sizeof(folder), "%s", argv[1]);
	snprintf(mods, sizeof(mods), "%s/mods", folder);
	mkdir(mods, 0755);
	snprintf(mods, sizeof(mods), "%s/mods/test", folder);
	mkdir(mods, 0755);
	snprintf(mods, sizeof(mods), "%s/mods/test/textures", folder);
	mkdir(mods, 0755);
	snprintf(mods, sizeof(mods), "%s/mods/test/textures/max", folder);
	mkdir(mods, 0755);
	test_ktx2();
	test_map();
	test_residency(threads);
	if (!threads)
		test_budget();
	if (failures)
	{
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("texture store: all passed%s\n", threads ? " (two workers)" : "");
	return 0;
}
