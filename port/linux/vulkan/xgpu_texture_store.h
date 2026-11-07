/*
XGPU_TEXTURE_STORE.H

PRISM's texture store (display.prism_textures; docs/mods.md, "Textures"):
replacement textures a mod ships as KTX2 files (xgpu_ktx2.h), read by the
host straight from their files into the GPU's memory, never into the
guest's. The guest goes on loading the stock bitmap, which stays the
fallback and the source of every size the game computes with; it learns
only an id for each bitmap that has a replacement (the stream's
TEXTURE_STORE_MAP), and names the id with each draw's stages (SET_TEXTURES).

This is the store's portable core (xgpu_texture_store.c), with no device
of its own: a backend (Vulkan's, xgpu_vulkan_store.c; the tests' fake)
makes and destroys the images, copies bytes from a staging buffer the
backend maps into them, numbers its submissions and says how much memory
the device has. The core:
- checks each file's header at the map's record, synchronously;
- streams a replacement's levels when it is first drawn, on worker threads
  that read the file's bytes into pieces of the staging buffer (a level
  larger than a piece in bands of whole rows of blocks), smallest level
  first: an entry is usable once it holds the stock bitmap's resolution
  (its level scale), drawn stock until then; the levels above follow one at
  a time, every entry reaching the stock resolution before any goes past it;
- copies the pieces read into the images at the frame's start, at most so
  many bytes a frame (more while a map loads), and gives a piece back once
  the submission that copied it is done;
- keeps the images within a budget (auto: a part of the device's free
  memory, or the setting): an entry not drawn in the last
  XGPU_TEXTURE_STORE_KEEP_FRAMES frames is cut back to the stock
  resolution, then let go of, the least recently drawn first.

The render thread calls every function but the workers' own; none may be
called from two threads at once.
*/

#ifndef __HALO_LINUX_XGPU_TEXTURE_STORE_H
#define __HALO_LINUX_XGPU_TEXTURE_STORE_H

#include <stdint.h>

#include "xgpu_ktx2.h"

/* entries drawn in this many frames are never cut back or let go of */
#define XGPU_TEXTURE_STORE_KEEP_FRAMES 10u
/* an entry let go of is not made larger again for this long, unless it is
drawn in this many frames in a row (the thrash guard) */
#define XGPU_TEXTURE_STORE_THRASH_SECONDS 5.0
#define XGPU_TEXTURE_STORE_THRASH_FRAMES 30u
/* the most entries a map has (the id's low 20 bits are the entry's
number plus one) */
#define XGPU_TEXTURE_STORE_ENTRIES_MAXIMUM 0xffffeu

/* an image the backend makes: format (XGPU_KTX2_FORMAT_*), a cube map's six
faces, its first level's size and its levels */
struct xgpu_texture_store_image_description
{
	uint8_t format, cube;
	uint32_t width, height, levels;
};

/* bytes of the staging buffer copied into an image: at staging_offset,
rows of blocks_wide blocks (row_bytes each), into level (the image's) and
face, the texels x 0, y y to width by height (the rows' texels) */
struct xgpu_texture_store_copy
{
	void *image;
	uint32_t level, face;
	uint32_t y, width, height;
	uint32_t staging_offset, bytes, row_bytes;
};

struct xgpu_texture_store_backend
{
	void *context;
	/* NULL if it cannot be made (the device's memory is out); bytes: its
	memory's */
	void *(*image_create)(void *context, const struct xgpu_texture_store_image_description *description,
		uint64_t *bytes);
	/* after the work recorded so far */
	void (*image_destroy)(void *context, void *image);
	/* in the work recorded now, before the frame's draws */
	void (*copy)(void *context, const struct xgpu_texture_store_copy *copy);
	/* every face of a level of one image into a level of another of the
	same size (an entry made smaller or larger keeps what it has) */
	void (*image_copy_level)(void *context, void *source, uint32_t source_level, void *destination,
		uint32_t destination_level);
	/* the number of the submission being recorded, and of the last the
	GPU has done */
	uint64_t (*serial)(void *context);
	uint64_t (*completed)(void *context);
	/* the device's memory for images: its budget and what is used of it
	(VK_EXT_memory_budget), or its size and 0; 0 if not known */
	int (*memory)(void *context, uint64_t *budget, uint64_t *usage);
	/* nonzero if the device samples a format with linear filtering */
	int (*format_supported)(void *context, uint8_t format);
	void (*log)(void *context, int warning, const char *text);
	/* the staging buffer, mapped, and its size */
	unsigned char *staging;
	uint32_t staging_size;
};

struct xgpu_texture_store_settings
{
	/* worker threads (0: none, the tests' xgpu_texture_store_work) */
	uint32_t workers;
	/* the largest piece of the staging buffer, a frame's bytes copied in
	play and while a map loads */
	uint32_t piece_maximum;
	uint32_t frame_bytes, loading_bytes;
	/* the auto budget's least and most */
	uint64_t budget_floor, budget_ceiling;
	/* the longer side of a replacement */
	uint32_t side_maximum;
};

/* a file the map's record asks about: its path (host's), size and
modification time as the guest found them, and the stock bitmap */
struct xgpu_texture_store_query
{
	const char *path;
	uint64_t file_size;
	uint32_t stamp_seconds;
	struct xgpu_ktx2_stock stock;
};

/* the store's answer: an id (0 refused) and why (XGPU_KTX2_*), the file's
size, levels and format */
struct xgpu_texture_store_answer
{
	uint32_t id;
	uint32_t status;
	uint32_t width, height, levels;
	uint8_t format;
};

/* what a draw samples of an entry that is usable: its image, the asset's
level the image's first is (top), the most detailed level resident (an
asset's level: the image's is resident - top), the asset's size over the
stock bitmap's (2^scale), its levels and format, and the stock bitmap it
replaces */
struct xgpu_texture_store_view
{
	void *image;
	uint32_t top, resident, scale, levels;
	uint8_t format, cube;
	struct xgpu_ktx2_stock stock;
};

struct xgpu_texture_store_statistics
{
	uint32_t entries, usable, full, failed, streaming;
	uint64_t resident_bytes, budget_bytes;
	uint64_t uploaded_bytes, pieces, frame_bytes_most;
	uint32_t evictions, demotions, promotions, allocation_failures;
	uint64_t read_bytes;
	uint32_t queued_jobs;
	/* seconds from the map's record to the last entry made usable and
	full (of those drawn) */
	double last_usable_seconds, last_full_seconds;
};

struct xgpu_texture_store;

struct xgpu_texture_store *xgpu_texture_store_create(const struct xgpu_texture_store_backend *backend,
	const struct xgpu_texture_store_settings *settings);
/* the workers stopped, every image destroyed */
void xgpu_texture_store_destroy(struct xgpu_texture_store *store);

/* A map's files (TEXTURE_STORE_MAP): a new generation (nonzero), what the
old one had let go of (the queue cancelled, the pieces read for it dropped,
its images destroyed), each query's header read and checked, its answer
written. count 0 closes the store. budget_megabytes: 0 auto. loading: the
loading rate of uploads until the next call of xgpu_texture_store_loading. */
void xgpu_texture_store_map(struct xgpu_texture_store *store, uint32_t generation, uint32_t budget_megabytes,
	int loading, uint32_t count, const struct xgpu_texture_store_query *queries,
	struct xgpu_texture_store_answer *answers);
void xgpu_texture_store_budget(struct xgpu_texture_store *store, uint32_t budget_megabytes);
void xgpu_texture_store_loading(struct xgpu_texture_store *store, int loading);
/* an id drawn this frame: 1 and the view where its entry is usable, else
0 (drawn stock); its levels are streamed where they are not resident */
int xgpu_texture_store_use(struct xgpu_texture_store *store, uint32_t id, struct xgpu_texture_store_view *view);
/* the frame's start: pieces read copied (up to the frame's bytes), pieces
done with given back, the budget kept, work queued */
void xgpu_texture_store_frame(struct xgpu_texture_store *store);
/* changes whenever an entry turns usable or not, or its resident level or
image changes: what a draw bound may be looked up again */
uint32_t xgpu_texture_store_epoch(const struct xgpu_texture_store *store);
void xgpu_texture_store_statistics(const struct xgpu_texture_store *store,
	struct xgpu_texture_store_statistics *statistics);
/* the entry of an id: its path and, if it is usable, its view (the
console's list); NULL if there is none */
const char *xgpu_texture_store_describe(const struct xgpu_texture_store *store, uint32_t id,
	struct xgpu_texture_store_view *view, uint32_t *state);
/* (tests, with no workers) one queued job read on the calling thread: 0
if none was queued */
int xgpu_texture_store_work(struct xgpu_texture_store *store);
/* the generation's ids are this one's (the id's top 12 bits) */
#define XGPU_TEXTURE_STORE_ID(generation, index) ((((generation) & 0xfffu) << 20) | ((index) + 1u))

/* an entry's state (xgpu_texture_store_describe) */
enum
{
	XGPU_TEXTURE_STORE_INDEXED,
	XGPU_TEXTURE_STORE_STREAMING,
	XGPU_TEXTURE_STORE_USABLE,
	XGPU_TEXTURE_STORE_FULL,
	XGPU_TEXTURE_STORE_DEMOTED,
	XGPU_TEXTURE_STORE_FAILED,
};

#endif
