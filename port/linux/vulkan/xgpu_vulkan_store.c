/*
XGPU_VULKAN_STORE.C

PRISM's texture store in the Vulkan backend (xgpu_vulkan_store.h).

The store is made at the first TEXTURE_STORE_MAP, with a staging buffer of
its own (64 MB on the desktop, 16 MB on Android; pieces of at most a
quarter of it), two workers (one on Android), a frame's bytes of 24 MB in
play and 256 MB for the frames after a map's record (LOADING_FRAMES), and
the auto budget's floor and ceiling of 128 MB and 6 GB. Its images are the
Vulkan core's, made in the GENERAL layout as every image is; its pieces are
copied into them in the stream's order, before the frame's draws that read
them (xgpu_vulkan_image_copy_from_buffer).
*/

#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#define STAGING_BYTES (16u << 20)
#define WORKERS 1
#else
#define STAGING_BYTES (64u << 20)
#define WORKERS 2
#endif
#define FRAME_BYTES (24u << 20)
#define LOADING_BYTES (256u << 20)
/* frames after a map's record at the loading rate */
#define LOADING_FRAMES 300u

static struct
{
	struct xgpu_texture_store *store;
	VkBuffer staging;
	void *mapped;
	int failed;
	uint32_t loading_frames;
} store_glue;

uint8_t xgpu_vulkan_store_format(uint8_t format)
{
	switch (format)
	{
	case XGPU_KTX2_FORMAT_BC1: return XGPU_VULKAN_FORMAT_DXT1;
	case XGPU_KTX2_FORMAT_BC1_RGB: return XGPU_VULKAN_FORMAT_BC1_RGB;
	case XGPU_KTX2_FORMAT_BC3: return XGPU_VULKAN_FORMAT_DXT5;
	case XGPU_KTX2_FORMAT_BC4: return XGPU_VULKAN_FORMAT_BC4;
	case XGPU_KTX2_FORMAT_BC5: return XGPU_VULKAN_FORMAT_BC5;
	case XGPU_KTX2_FORMAT_BC7: return XGPU_VULKAN_FORMAT_BC7;
	case XGPU_KTX2_FORMAT_ASTC: return XGPU_VULKAN_FORMAT_ASTC;
	default: return XGPU_VULKAN_FORMAT_NONE;
	}
}

static void *store_image_create(void *context, const struct xgpu_texture_store_image_description *description,
	uint64_t *bytes)
{
	struct xgpu_vulkan_image_description made;
	struct xgpu_vulkan_image *image;

	(void)context;
	memset(&made, 0, sizeof(made));
	made.type = description->cube ? XGPU_VULKAN_IMAGE_CUBE : XGPU_VULKAN_IMAGE_2D;
	made.format = xgpu_vulkan_store_format(description->format);
	made.usage = XGPU_VULKAN_USAGE_SAMPLED;
	made.width = description->width;
	made.height = description->height;
	made.depth_or_layers = description->cube ? 6 : 1;
	made.levels = description->levels;
	image = xgpu_vulkan_image_create(&made);
	*bytes = xgpu_vulkan_image_bytes(image);
	return image;
}

static void store_image_destroy(void *context, void *image)
{
	(void)context;
	xgpu_vulkan_image_destroy(image);
}

static void store_copy(void *context, const struct xgpu_texture_store_copy *copy)
{
	(void)context;
	xgpu_vulkan_image_copy_from_buffer(copy->image, copy->level, copy->face, copy->y, copy->width, copy->height,
		store_glue.staging, copy->staging_offset);
}

static void store_image_copy_level(void *context, void *source, uint32_t source_level, void *destination,
	uint32_t destination_level)
{
	uint32_t layer, layers = xgpu_vulkan_image_description(source)->type == XGPU_VULKAN_IMAGE_CUBE ? 6 : 1;

	(void)context;
	for (layer = 0; layer < layers; layer++)
		xgpu_vulkan_image_copy(source, source_level, layer, destination, destination_level, layer);
}

static uint64_t store_serial(void *context)
{
	(void)context;
	return xgpu_vulkan_serial();
}

static uint64_t store_completed(void *context)
{
	(void)context;
	return xgpu_vulkan_completed();
}

static int store_memory(void *context, uint64_t *budget, uint64_t *usage)
{
	(void)context;
	return xgpu_vulkan_memory_budget(budget, usage);
}

static int store_format_supported(void *context, uint8_t format)
{
	(void)context;
	return xgpu_vulkan_format_sampled(xgpu_vulkan_store_format(format));
}

static void store_log(void *context, int warning, const char *text)
{
	(void)context;
	host_logf(warning ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", text);
}

static int store_make(void)
{
	struct xgpu_texture_store_backend backend;
	struct xgpu_texture_store_settings settings;

	if (store_glue.store)
		return 1;
	if (store_glue.failed)
		return 0;
	store_glue.staging = xgpu_vulkan_staging_create(STAGING_BYTES, &store_glue.mapped);
	if (!store_glue.staging || !store_glue.mapped)
	{
		host_logf(HOST_LOG_WARN, "texstore: no staging buffer of %u MB: replacement textures are drawn stock",
			STAGING_BYTES >> 20);
		store_glue.failed = 1;
		return 0;
	}
	memset(&backend, 0, sizeof(backend));
	backend.image_create = store_image_create;
	backend.image_destroy = store_image_destroy;
	backend.copy = store_copy;
	backend.image_copy_level = store_image_copy_level;
	backend.serial = store_serial;
	backend.completed = store_completed;
	backend.memory = store_memory;
	backend.format_supported = store_format_supported;
	backend.log = store_log;
	backend.staging = store_glue.mapped;
	backend.staging_size = STAGING_BYTES;
	memset(&settings, 0, sizeof(settings));
	settings.workers = WORKERS;
	settings.piece_maximum = STAGING_BYTES / 4;
	settings.frame_bytes = FRAME_BYTES;
	settings.loading_bytes = LOADING_BYTES;
	settings.budget_floor = 128ull << 20;
	settings.budget_ceiling = 6ull << 30;
	settings.side_maximum = XGPU_KTX2_SIDE_MAXIMUM;
	settings.material_side_maximum = XGPU_STREAM_TEXTURE_STORE_MATERIAL_SIDE;
	store_glue.store = xgpu_texture_store_create(&backend, &settings);
	if (!store_glue.store)
		store_glue.failed = 1;
	return store_glue.store != NULL;
}

int xgpu_vulkan_store_available(void)
{
#ifdef _WIN32
	return 0;
#else
	return getenv("HALO_TEXTURE_STORE_OFF") == NULL;
#endif
}

void xgpu_vulkan_store_map(void *context, const struct xgpu_stream_texture_store_map *record,
	const struct xgpu_stream_texture_store_query *queries, const char *strings,
	struct xgpu_stream_texture_store_answer *answers)
{
	struct xgpu_store_query *asked = NULL;
	struct xgpu_store_answer *answered = NULL;
	uint32_t index, count = record->query_count;

	(void)context;
	if (!count && !store_glue.store)
		return;
	if (!xgpu_vulkan_store_available() || !store_make())
	{
		for (index = 0; index < count; index++)
			answers[index].status = XGPU_STREAM_TEXTURE_STORE_DEVICE;
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
				answers[index].status = XGPU_STREAM_TEXTURE_STORE_UNREADABLE;
			return;
		}
	}
	for (index = 0; index < count; index++)
	{
		asked[index].path = strings + queries[index].path;
		asked[index].file_size = queries[index].file_size;
		asked[index].stamp_seconds = queries[index].stamp_seconds;
		asked[index].stock.width = queries[index].stock_width;
		asked[index].stock.height = queries[index].stock_height;
		asked[index].stock.levels = queries[index].stock_levels;
		asked[index].stock.cube = queries[index].stock_type == 1;
		asked[index].material = queries[index].kind == XGPU_STREAM_TEXTURE_STORE_KIND_MATERIAL;
	}
	xgpu_texture_store_map(store_glue.store, record->generation, record->budget_megabytes,
		(record->flags & XGPU_STREAM_TEXTURE_STORE_LOADING) != 0, count, asked, answered);
	store_glue.loading_frames = (record->flags & XGPU_STREAM_TEXTURE_STORE_LOADING) ? LOADING_FRAMES : 0;
	for (index = 0; index < count; index++)
	{
		answers[index].id = answered[index].id;
		answers[index].width = (uint16_t)answered[index].width;
		answers[index].height = (uint16_t)answered[index].height;
		answers[index].levels = (uint8_t)answered[index].levels;
		answers[index].status = (uint8_t)answered[index].status;
		answers[index].format = answered[index].format;
	}
	free(asked);
	free(answered);
}

void xgpu_vulkan_store_frame(void)
{
	if (!store_glue.store)
		return;
	if (store_glue.loading_frames && !--store_glue.loading_frames)
		xgpu_texture_store_loading(store_glue.store, 0);
	xgpu_texture_store_frame(store_glue.store);
}

int xgpu_vulkan_store_use(uint32_t id, struct xgpu_texture_store_view *view)
{
	return id && store_glue.store && xgpu_texture_store_use(store_glue.store, id, view);
}

uint32_t xgpu_vulkan_store_epoch(void)
{
	return store_glue.store ? xgpu_texture_store_epoch(store_glue.store) : 0;
}

int xgpu_vulkan_store_statistics(char *text, unsigned long size)
{
	struct xgpu_texture_store_statistics statistics;

	if (!store_glue.store || !size)
		return 0;
	xgpu_texture_store_statistics(store_glue.store, &statistics);
	if (!statistics.entries)
		return 0;
	snprintf(text, size, "texstore: %u indexed, %u drawn lately, %u usable, %u full, %u failed, %u streaming; %llu uses "
		"(%llu stale); resident %.1f MB of %.1f MB; "
		"uploaded %.1f MB in %llu pieces, at most %.1f MB a frame; %u evictions, %u demotions, %u promotions, "
		"%u allocations failed; read %.1f MB; %u jobs queued; last usable %.2f s, last full %.2f s after the map",
		statistics.entries, statistics.drawn, statistics.usable, statistics.full, statistics.failed, statistics.streaming,
		(unsigned long long)statistics.uses, (unsigned long long)statistics.stale_uses,
		(double)statistics.resident_bytes / 1048576.0, (double)statistics.budget_bytes / 1048576.0,
		(double)statistics.uploaded_bytes / 1048576.0, (unsigned long long)statistics.pieces,
		(double)statistics.frame_bytes_most / 1048576.0, statistics.evictions, statistics.demotions,
		statistics.promotions, statistics.allocation_failures, (double)statistics.read_bytes / 1048576.0,
		statistics.queued_jobs, statistics.last_usable_seconds, statistics.last_full_seconds);
	return 1;
}
