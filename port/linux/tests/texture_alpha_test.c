/*
TEXTURE_ALPHA_TEST.C

A texture's least alpha (xgpu_texture_minimum_alpha, xbox_texture_decode.c),
which the front end drops a draw's alpha test by (d3d8_device.c, "an alpha
test that cannot fail"), against the alphas the decoder gives the same
texels: never more than the least of them, and the least itself where the
format lets it be told. On the build machine, without a GPU or game data
(tools/test_texture_alpha.py).
*/

#include "xgpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, ...) \
	do { if (!(condition)) { fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); exit(1); } } while (0)

enum
{
	FORMAT_A4R4G4B4 = 0x04, FORMAT_A8R8G8B8 = 0x06, FORMAT_X8R8G8B8 = 0x07, FORMAT_DXT1 = 0x0c, FORMAT_DXT3 = 0x0e,
	FORMAT_DXT5 = 0x0f, FORMAT_LINEAR_A8R8G8B8 = 0x12
};

static uint32_t random_state = 2342;

static uint32_t random_next(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return random_state >> 8;
}

static struct xgpu_texture_description describe(DWORD format, unsigned long size, unsigned long levels)
{
	struct xgpu_texture_description description;

	memset(&description, 0, sizeof(description));
	description.format = format;
	description.width = description.height = size;
	description.depth = 1;
	description.levels = levels;
	description.linear = format == FORMAT_LINEAR_A8R8G8B8;
	description.compressed = format == FORMAT_DXT1 || format == FORMAT_DXT3 || format == FORMAT_DXT5;
	description.pitch = description.linear ? size * 4 : 0;
	return description;
}

/* the least alpha of the texels as the decoder reads them, every level's */
static int decoded_minimum(const struct xgpu_texture_description *description, const unsigned char *texels)
{
	unsigned long level, least = 255;

	for (level = 0; level < description->levels; level++)
	{
		unsigned long width = xgpu_texture_level_dimension(description->width, level);
		unsigned long height = xgpu_texture_level_dimension(description->height, level), index;
		const unsigned char *source = texels + xgpu_texture_level_offset(description, level);
		DWORD *pixels = calloc(width * height, sizeof(DWORD));

		CHECK(pixels, "no memory");
		if (description->compressed)
			xgpu_texture_decode_dxt(description, level, source, pixels);
		else
			CHECK(xgpu_texture_decode_level(description, level, source, NULL, pixels), "a level not decoded");
		for (index = 0; index < width * height; index++)
		{
			if (pixels[index] >> 24 < least)
				least = pixels[index] >> 24;
		}
		free(pixels);
	}
	return (int)least;
}

/* random texels: all of them, or (shaped) each block's alphas kept away
from nothing, so that the least is not always 0 */
static void fill(const struct xgpu_texture_description *description, unsigned char *texels, unsigned long bytes, int shaped)
{
	unsigned long offset;

	for (offset = 0; offset < bytes; offset++)
		texels[offset] = (unsigned char)random_next();
	if (!shaped)
		return;
	switch (description->format)
	{
	case FORMAT_DXT1:
		/* (four colours: the first the greater, no transparent index) */
		for (offset = 0; offset + 8 <= bytes; offset += 8)
		{
			texels[offset] = texels[offset + 1] = 0xff;
			texels[offset + 2] = texels[offset + 3] = 0x00;
		}
		break;
	case FORMAT_DXT3:
		for (offset = 0; offset + 16 <= bytes; offset += 16)
		{
			int byte;

			for (byte = 0; byte < 8; byte++)
				texels[offset + byte] |= 0x88;
		}
		break;
	case FORMAT_DXT5:
		/* (eight alphas, from the first down to the second) */
		for (offset = 0; offset + 16 <= bytes; offset += 16)
		{
			texels[offset] = (unsigned char)(200 + random_next() % 56);
			texels[offset + 1] = (unsigned char)(100 + random_next() % 100);
		}
		break;
	default:
		for (offset = 3; offset < bytes; offset += 4)
			texels[offset] |= 0x90;
		break;
	}
}

static void format_check(DWORD format, const char *name, int exact)
{
	int trial;

	for (trial = 0; trial < 400; trial++)
	{
		/* (a first level alone, of whole blocks: every texel read is one of
		the texture's; with the levels below, a level smaller than a block
		reads part of one) */
		unsigned long levels = format == FORMAT_LINEAR_A8R8G8B8 || trial % 2 ? 1 : 5;
		struct xgpu_texture_description description = describe(format, 16, levels);
		unsigned long bytes = xgpu_texture_face_size(&description);
		unsigned char *texels = malloc(bytes);
		int minimum, decoded;

		CHECK(texels && bytes, "%s: no texels", name);
		fill(&description, texels, bytes, trial % 4 >= 2);
		minimum = xgpu_texture_minimum_alpha(&description, texels);
		decoded = decoded_minimum(&description, texels);
		CHECK(minimum >= 0 && minimum <= decoded, "%s, trial %d: least alpha %d, the decoder's %d", name, trial, minimum,
			decoded);
		if (exact && levels == 1)
			CHECK(minimum == decoded, "%s, trial %d: least alpha %d, not the decoder's %d", name, trial, minimum, decoded);
		free(texels);
	}
}

int main(void)
{
	struct xgpu_texture_description description;
	unsigned char block[16];

	format_check(FORMAT_DXT1, "DXT1", 1);
	format_check(FORMAT_DXT3, "DXT3", 1);
	format_check(FORMAT_DXT5, "DXT5", 0);
	format_check(FORMAT_A8R8G8B8, "A8R8G8B8", 1);
	format_check(FORMAT_LINEAR_A8R8G8B8, "linear A8R8G8B8", 1);

	/* a DXT5 block of one alpha: that alpha */
	description = describe(FORMAT_DXT5, 4, 1);
	memset(block, 0, sizeof(block));
	block[0] = block[1] = 200;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 200 && decoded_minimum(&description, block) == 200,
		"a DXT5 block of one alpha");
	/* ... of six alphas that uses its 0 (index 6): nothing */
	block[0] = 100;
	block[2] = 6;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 0 && decoded_minimum(&description, block) == 0,
		"a DXT5 block of six alphas and nothing");
	/* a DXT1 block of three colours that uses the transparent index */
	description = describe(FORMAT_DXT1, 4, 1);
	memset(block, 0, sizeof(block));
	block[4] = 0x03;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 0 && decoded_minimum(&description, block) == 0,
		"a DXT1 block with a transparent texel");
	/* ... and of four colours with the same index: opaque */
	block[0] = 1;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 255 && decoded_minimum(&description, block) == 255,
		"a DXT1 block of four colours");

	/* a format with no alpha passes whatever its texels; one not read, and a
	cube map, are not known */
	description = describe(FORMAT_X8R8G8B8, 4, 1);
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 255, "a format with no alpha");
	description = describe(FORMAT_A4R4G4B4, 4, 1);
	CHECK(xgpu_texture_minimum_alpha(&description, block) == -1, "a format that is not read");
	description = describe(FORMAT_A8R8G8B8, 4, 1);
	description.cube_map = TRUE;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == -1, "a cube map");
	printf("texture_alpha_test: passed\n");
	return 0;
}
