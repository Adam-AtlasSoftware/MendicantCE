/*
XGPU_KTX2.C

The texture store's KTX2 header and level index (xgpu_ktx2.h).
*/

#include "xgpu_ktx2.h"

#include <string.h>

static const unsigned char identifier[12] =
{
	0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a,
};

static uint32_t read32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read64(const unsigned char *bytes)
{
	return (uint64_t)read32(bytes) | ((uint64_t)read32(bytes + 4) << 32);
}

uint32_t xgpu_ktx2_block_bytes(uint8_t format)
{
	switch (format)
	{
	case XGPU_KTX2_FORMAT_BC1:
	case XGPU_KTX2_FORMAT_BC1_RGB:
	case XGPU_KTX2_FORMAT_BC4:
		return 8;
	case XGPU_KTX2_FORMAT_BC3:
	case XGPU_KTX2_FORMAT_BC5:
	case XGPU_KTX2_FORMAT_BC7:
	case XGPU_KTX2_FORMAT_ASTC:
		return 16;
	default:
		return 0;
	}
}

uint32_t xgpu_ktx2_vk_format(uint8_t format)
{
	static const uint32_t formats[XGPU_KTX2_FORMAT_COUNT] = { 0, 133, 131, 137, 139, 141, 145, 157 };

	return format < XGPU_KTX2_FORMAT_COUNT ? formats[format] : 0;
}

const char *xgpu_ktx2_format_name(uint8_t format)
{
	static const char *names[XGPU_KTX2_FORMAT_COUNT] = { "none", "BC1", "BC1 RGB", "BC3", "BC4", "BC5", "BC7", "ASTC 4x4" };

	return format < XGPU_KTX2_FORMAT_COUNT ? names[format] : "none";
}

const char *xgpu_ktx2_status_name(int status)
{
	static const char *names[] = { "accepted", "unreadable", "changed", "format", "size", "device", "path" };

	return status >= 0 && status <= XGPU_KTX2_PATH ? names[status] : "unknown";
}

/* a KTX2 vkFormat's: the format, and whether it is an _SRGB one */
static uint8_t format_of(uint32_t vk_format, uint8_t *srgb)
{
	*srgb = 0;
	switch (vk_format)
	{
	case 134: *srgb = 1; /* fall through */
	case 133: return XGPU_KTX2_FORMAT_BC1;
	case 132: *srgb = 1; /* fall through */
	case 131: return XGPU_KTX2_FORMAT_BC1_RGB;
	case 138: *srgb = 1; /* fall through */
	case 137: return XGPU_KTX2_FORMAT_BC3;
	case 139: return XGPU_KTX2_FORMAT_BC4;
	case 141: return XGPU_KTX2_FORMAT_BC5;
	case 146: *srgb = 1; /* fall through */
	case 145: return XGPU_KTX2_FORMAT_BC7;
	case 158: *srgb = 1; /* fall through */
	case 157: return XGPU_KTX2_FORMAT_ASTC;
	default: return XGPU_KTX2_FORMAT_NONE;
	}
}

static uint32_t levels_whole(uint32_t width, uint32_t height)
{
	uint32_t side = width > height ? width : height, levels = 1;

	while (side > 1)
	{
		side >>= 1;
		levels++;
	}
	return levels;
}

static int power_of_two(uint32_t value)
{
	return value && !(value & (value - 1));
}

int xgpu_ktx2_parse(const void *bytes, uint32_t size, uint64_t file_size, const struct xgpu_ktx2_stock *stock,
	uint32_t side_maximum, struct xgpu_ktx2_file *file)
{
	const unsigned char *header = bytes;
	uint32_t depth, layers, supercompression, type_size, level;

	memset(file, 0, sizeof(*file));
	if (!bytes || size < XGPU_KTX2_HEADER_BYTES || file_size < XGPU_KTX2_HEADER_BYTES)
		return XGPU_KTX2_UNREADABLE;
	if (memcmp(header, identifier, sizeof(identifier)))
		return XGPU_KTX2_INVALID;
	file->vk_format = read32(header + 12);
	type_size = read32(header + 16);
	file->width = read32(header + 20);
	file->height = read32(header + 24);
	depth = read32(header + 28);
	layers = read32(header + 32);
	file->faces = read32(header + 36);
	file->levels = read32(header + 40);
	supercompression = read32(header + 44);
	file->format = format_of(file->vk_format, &file->srgb);
	file->block_bytes = xgpu_ktx2_block_bytes(file->format);
	/* (block formats' typeSize is 1; zstd and Basis are not read) */
	if (!file->format || type_size != 1 || supercompression || depth || layers || !file->width || !file->height ||
		(file->faces != 1 && file->faces != 6) || !file->levels || file->levels > XGPU_KTX2_LEVELS_MAXIMUM)
		return XGPU_KTX2_INVALID;
	if (file->faces == 6 && file->width != file->height)
		return XGPU_KTX2_INVALID;
	if (stock && (file->faces == 6) != (stock->cube != 0))
		return XGPU_KTX2_INVALID;
	if (!power_of_two(file->width) || !power_of_two(file->height) ||
		(file->width > side_maximum || file->height > side_maximum))
		return XGPU_KTX2_SIZE;
	if (file->levels != levels_whole(file->width, file->height))
		return XGPU_KTX2_INVALID;
	if (stock)
	{
		uint32_t scale = 0;

		if (!stock->width || !stock->height)
			return XGPU_KTX2_SIZE;
		while (scale < 16 && (stock->width << scale) < file->width)
			scale++;
		if ((stock->width << scale) != file->width || (stock->height << scale) != file->height)
			return XGPU_KTX2_SIZE;
		file->scale = scale;
	}
	if (size < XGPU_KTX2_HEADER_BYTES + 24u * file->levels)
		return XGPU_KTX2_UNREADABLE;
	for (level = 0; level < file->levels; level++)
	{
		const unsigned char *entry = header + XGPU_KTX2_HEADER_BYTES + 24u * level;
		struct xgpu_ktx2_level *to = &file->level[level];
		uint32_t width = file->width >> level, height = file->height >> level;

		if (!width)
			width = 1;
		if (!height)
			height = 1;
		to->offset = read64(entry);
		to->length = read64(entry + 8);
		to->blocks_wide = (width + 3) / 4;
		to->blocks_high = (height + 3) / 4;
		to->face_bytes = (uint64_t)to->blocks_wide * to->blocks_high * file->block_bytes;
		if (to->length != to->face_bytes * file->faces || to->offset < XGPU_KTX2_HEADER_BYTES ||
			to->offset > file_size || to->length > file_size - to->offset)
			return XGPU_KTX2_INVALID;
	}
	return XGPU_KTX2_ACCEPTED;
}
