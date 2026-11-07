/*
XGPU_KTX2.H

The KTX2 files of the texture store (xgpu_texture_store.h): their header
and level index read and checked against the stock bitmap a file replaces
(xgpu_ktx2.c). Plain C on bytes, without the device or the file system, so
that the store's tests build it alone.

A replacement is a KTX2 file of one of the block formats below, with no
supercompression, a whole chain of levels (floor(log2(the longer side)) +
1), one face (six for a stock cube map), no layers, no depth, and a size
the stock bitmap's times 2^k on both sides (the same k, 0 or more), its
longer side at most the store's largest. Its levels' bytes are where the
level index says, inside the file, each exactly as long as its blocks.
*/

#ifndef __HALO_LINUX_XGPU_KTX2_H
#define __HALO_LINUX_XGPU_KTX2_H

#include <stdint.h>

/* the formats a replacement may be in */
enum
{
	XGPU_KTX2_FORMAT_NONE,
	XGPU_KTX2_FORMAT_BC1,       /* VK_FORMAT_BC1_RGBA (133) and _RGB (131) */
	XGPU_KTX2_FORMAT_BC1_RGB,
	XGPU_KTX2_FORMAT_BC3,       /* 137 */
	XGPU_KTX2_FORMAT_BC4,       /* 139 */
	XGPU_KTX2_FORMAT_BC5,       /* 141 */
	XGPU_KTX2_FORMAT_BC7,       /* 145 */
	XGPU_KTX2_FORMAT_ASTC,      /* ASTC 4x4 (157) */
	XGPU_KTX2_FORMAT_COUNT
};

/* the bytes the header and level index are read from (the key/value data
past them is not read) */
#define XGPU_KTX2_HEADER_BYTES 80u
#define XGPU_KTX2_LEVELS_MAXIMUM 16u
#define XGPU_KTX2_INDEX_BYTES (XGPU_KTX2_HEADER_BYTES + 24u * XGPU_KTX2_LEVELS_MAXIMUM)
/* the longer side of a replacement (Vulkan allows 16384; the store's
budget is planned for this) */
#define XGPU_KTX2_SIDE_MAXIMUM 4096u

/* why a file is refused (the stream's answers' status: xgpu_stream.h's
XGPU_STREAM_TEXTURE_STORE_*) */
enum
{
	XGPU_KTX2_ACCEPTED,
	XGPU_KTX2_UNREADABLE,  /* missing, unreadable, short */
	XGPU_KTX2_CHANGED,     /* its size or time is not the one the guest found */
	XGPU_KTX2_INVALID,     /* not KTX2, or a field this reader refuses */
	XGPU_KTX2_SIZE,        /* not the stock size times 2^k, or too large */
	XGPU_KTX2_DEVICE,      /* the device does not sample its format */
	XGPU_KTX2_PATH,        /* outside a mods folder, or not a .ktx2 */
};

/* the stock bitmap a file replaces (its first level's size, its levels,
whether it is a cube map) */
struct xgpu_ktx2_stock
{
	uint32_t width, height, levels;
	int cube;
};

struct xgpu_ktx2_level
{
	uint64_t offset, length;
	/* blocks across and down, and a face's bytes (length / faces) */
	uint32_t blocks_wide, blocks_high;
	uint64_t face_bytes;
};

struct xgpu_ktx2_file
{
	uint32_t vk_format;
	/* XGPU_KTX2_FORMAT_*; the format was an _SRGB one (sampled as UNORM) */
	uint8_t format, srgb;
	uint32_t width, height, levels, faces;
	/* the size is the stock's times 2^scale */
	uint32_t scale;
	/* a block's bytes (8 or 16) */
	uint32_t block_bytes;
	struct xgpu_ktx2_level level[XGPU_KTX2_LEVELS_MAXIMUM];
};

/* reads the header and level index from bytes (the file's first size
bytes, at least XGPU_KTX2_HEADER_BYTES; XGPU_KTX2_INDEX_BYTES are enough)
of a file of file_size bytes, and checks them against the stock bitmap
(stock NULL: no size rule) and the largest side: XGPU_KTX2_ACCEPTED, or why
not */
int xgpu_ktx2_parse(const void *bytes, uint32_t size, uint64_t file_size, const struct xgpu_ktx2_stock *stock,
	uint32_t side_maximum, struct xgpu_ktx2_file *file);
/* a format's block bytes (0: none) */
uint32_t xgpu_ktx2_block_bytes(uint8_t format);
/* the KTX2 vkFormat of a format (its UNORM one) */
uint32_t xgpu_ktx2_vk_format(uint8_t format);
const char *xgpu_ktx2_format_name(uint8_t format);
const char *xgpu_ktx2_status_name(int status);

#endif
