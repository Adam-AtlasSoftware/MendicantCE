/*
MOD_LIGHTMAPS.C

What tells whether a mod's lightmap (mod_bitmaps.c) was made for the
layout of the structure bsp it would light: the bsp's lightmap layout
fingerprint, and the picture check of a lightmap that carries none.

A bsp's lightmaps each name a page of its lightmap bitmap and lay their
surfaces' corners out in it, at coordinates the bsp keeps (its materials'
lightmap vertices). A lightmap baked for another layout (another build of
the level: Halo PC's Blood Gulch, say) puts other pixels under the same
corners, and lights the level wrongly.

The fingerprint is a 64-bit FNV-1a hash of 32-bit little-endian words:
	'lm1 ', the pages the lightmaps name (the largest page index, plus 1),
	then for each of the bsp's lightmaps in order that names a page (its
	page index 0 or more): its page index, its materials' count, and for
	each material its surfaces' count, then for each surface its three
	corners' lightmap coordinates, u then v, as the compressed lightmap
	vertex keeps them (a short each, -32768 to 32767, sign-extended: the
	coordinate is (2 s + 1) / 65535), or 'none' and nothing more for a
	material whose vertices cannot be read.
It is written "lm1-" and 16 lower-case hexadecimal digits. It is the same
for every bitmap of the layout, at any size, and changes with any surface's
corner. tools/lightmap_fingerprint.py prints a map's (docs/mods.md).

The picture check (with no fingerprint) compares each page of the mod's
lightmap with the map's of the same index: the mod's page reduced to the
map's size (each of the map's pixels the mean of the mod's under it), both
pages' luminance blurred by a 3x3 box (a redraw resampled moves its fine
detail a pixel, a lightmap's charts do not move), then correlated. A page
whose correlation is below MOD_LIGHTMAP_MINIMUM_CORRELATION is not the
map's layout. Measured with this code on Blood Gulch's Xbox lightmap (15
pages): each page re-encoded (R5G6B5 again: 1; DXT1: at least 0.974) or
upscaled 2x (bilinear, 32-bit: at least 0.986) correlates with the map's at
least 0.974; Halo PC's pages of the same size laid out anew (pages 0, 3, 8,
11 to 14) 0.18 to 0.78, and the map's own pages of one size with each other
at most 0.62. 0.9 is between them, 0.12 above the highest of another layout
and 0.07 under the lowest of the map's. A page of the map with no picture to
compare (its luminance's deviation under one level of 255: a flat page)
passes.
*/

/* ---------- headers */

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "rasterizer/rasterizer_geometry.h"
#include "structures/structure_bsp_definitions.h"
#include "mods.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

/* bitmaps.c's formats, and the swizzle flag */
enum
{
	_lightmap_format_a8,
	_lightmap_format_y8,
	_lightmap_format_ay8,
	_lightmap_format_a8y8,
	_lightmap_format_r5g6b5 = 6,
	_lightmap_format_a1r5g5b5 = 8,
	_lightmap_format_a4r4g4b4,
	_lightmap_format_x8r8g8b8,
	_lightmap_format_a8r8g8b8,
	_lightmap_format_dxt1 = 14,
	_lightmap_format_dxt3,
	_lightmap_format_dxt5,

	_lightmap_swizzled_bit = 3,
};

#define FINGERPRINT_OFFSET_BASIS 0xCBF29CE484222325ULL
#define FINGERPRINT_PRIME 0x100000001B3ULL
/* (the words of the bytes "lm1 " and "none", little-endian) */
#define FINGERPRINT_VERSION 0x20316D6CUL
#define FINGERPRINT_NONE 0x656E6F6EUL

/* ---------- private code */

static void fingerprint_word(
	unsigned long long *hash,
	long word)
{
	unsigned long value = (unsigned long)word;
	int index;

	for (index = 0; index < 4; index++)
	{
		*hash ^= (value >> (8 * index)) & 0xFF;
		*hash *= FINGERPRINT_PRIME;
	}

	return;
}

/* whether the material's compressed vertices, then its lightmap vertices,
are there to read, its surfaces among the bsp's */
static boolean material_readable(
	struct structure_bsp const *structure_bsp,
	struct structure_material const *material,
	long vertex_size,
	long lightmap_vertex_size)
{
	return material->compressed_vertex_data.address && material->vertices.count > 0 &&
		material->lightmap_vertices.count == material->vertices.count &&
		material->compressed_vertex_data.size >= material->vertices.count * (vertex_size + lightmap_vertex_size) &&
		material->surface_count >= 0 && material->first_surface_index >= 0 &&
		material->first_surface_index <= structure_bsp->surfaces.count - material->surface_count;
}

static long lightmap_coordinate(
	byte const *vertex,
	int which)
{
	short value;

	/* (environment_lightmap_vertex_compressed: its incident radiosity, then
	its u and v) */
	memcpy(&value, vertex + 4 + 2 * which, sizeof(value));

	return value;
}

static real luminance_of(
	long red,
	long green,
	long blue)
{
	return 0.299f * (real)red + 0.587f * (real)green + 0.114f * (real)blue;
}

static real luminance_565(
	unsigned long value)
{
	long red = (value >> 11) & 31;
	long green = (value >> 5) & 63;
	long blue = value & 31;

	return luminance_of((red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2));
}

/* (rasterizer_swizzle's: the bits of x and y alternate until each runs out) */
static void swizzle_masks(
	long width,
	long height,
	unsigned long *x_mask,
	unsigned long *y_mask)
{
	unsigned long bit = 1;
	unsigned long mask_bit = 1;
	boolean more = TRUE;

	*x_mask = *y_mask = 0;
	while (more)
	{
		more = FALSE;
		if (bit < (unsigned long)width)
		{
			*x_mask |= mask_bit;
			mask_bit <<= 1;
			more = TRUE;
		}
		if (bit < (unsigned long)height)
		{
			*y_mask |= mask_bit;
			mask_bit <<= 1;
			more = TRUE;
		}
		bit <<= 1;
	}

	return;
}

static unsigned long swizzle_spread(
	unsigned long mask,
	unsigned long value)
{
	unsigned long result = 0;
	unsigned long bit;

	for (bit = 1; value && bit; bit <<= 1)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
	}

	return result;
}

static unsigned long read16(
	byte const *bytes)
{
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8);
}

/* a DXT block's 16 colors' luminance */
static void dxt_block_luminance(
	byte const *block,
	boolean dxt1,
	real luminance[16])
{
	unsigned long first = read16(block);
	unsigned long second = read16(block + 2);
	unsigned long bits = (unsigned long)block[4] | ((unsigned long)block[5] << 8) |
		((unsigned long)block[6] << 16) | ((unsigned long)block[7] << 24);
	real colors[4];
	long index;

	colors[0] = luminance_565(first);
	colors[1] = luminance_565(second);
	if (!dxt1 || first > second)
	{
		colors[2] = (2.f * colors[0] + colors[1]) / 3.f;
		colors[3] = (colors[0] + 2.f * colors[1]) / 3.f;
	}
	else
	{
		colors[2] = (colors[0] + colors[1]) / 2.f;
		colors[3] = 0.f;
	}
	for (index = 0; index < 16; index++)
		luminance[index] = colors[(bits >> (2 * index)) & 3];

	return;
}

/* the 3x3 box blur of `pixels` (width x height), into `blurred` */
static void luminance_blur(
	real const *pixels,
	long width,
	long height,
	real *blurred)
{
	long x, y;

	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			real sum = 0.f;
			long i, j;

			for (j = -1; j <= 1; j++)
			{
				long row = MIN(MAX(y + j, 0), height - 1);

				for (i = -1; i <= 1; i++)
					sum += pixels[row * width + MIN(MAX(x + i, 0), width - 1)];
			}
			blurred[y * width + x] = sum / 9.f;
		}
	}

	return;
}

/* ---------- public code */

unsigned long long mod_lightmap_fingerprint(
	struct structure_bsp const *structure_bsp)
{
	unsigned long long hash = FINGERPRINT_OFFSET_BASIS;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	long pages = 0;
	long lightmap_index;

	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
			struct structure_lightmap);

		pages = MAX(pages, lightmap->bitmap_index + 1);
	}
	fingerprint_word(&hash, (long)FINGERPRINT_VERSION);
	fingerprint_word(&hash, pages);
	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		long material_index;

		if (lightmap->bitmap_index < 0)
			continue;
		fingerprint_word(&hash, lightmap->bitmap_index);
		fingerprint_word(&hash, lightmap->materials.count);
		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material const *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
				struct structure_material);
			byte const *vertices = (byte const *)material->compressed_vertex_data.address;
			long surface_index;

			fingerprint_word(&hash, material->surface_count);
			if (!material_readable(structure_bsp, material, vertex_size, lightmap_vertex_size))
			{
				fingerprint_word(&hash, (long)FINGERPRINT_NONE);
				continue;
			}
			vertices += material->vertices.count * vertex_size;
			for (surface_index = 0; surface_index < material->surface_count; surface_index++)
			{
				struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&structure_bsp->surfaces,
					material->first_surface_index + surface_index, struct structure_surface);
				long corner;

				for (corner = 0; corner < NUMBER_OF_VERTICES_PER_TRIANGLE; corner++)
				{
					long vertex = surface->vertex_indices[corner];

					/* (a corner past the material's vertices: none) */
					if (vertex >= material->vertices.count)
					{
						fingerprint_word(&hash, (long)FINGERPRINT_NONE);
						fingerprint_word(&hash, (long)FINGERPRINT_NONE);
						continue;
					}
					fingerprint_word(&hash, lightmap_coordinate(vertices + vertex * lightmap_vertex_size, 0));
					fingerprint_word(&hash, lightmap_coordinate(vertices + vertex * lightmap_vertex_size, 1));
				}
			}
		}
	}

	return hash;
}

void mod_lightmap_fingerprint_text(
	unsigned long long fingerprint,
	char text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE])
{
	static char const digits[] = "0123456789abcdef";
	int index;

	memcpy(text, "lm1-", 4);
	for (index = 0; index < 16; index++)
		text[4 + index] = digits[(fingerprint >> (60 - 4 * index)) & 0xF];
	text[20] = 0;

	return;
}

boolean mod_lightmap_sidecar_parse(
	char const *bytes,
	unsigned long size,
	char text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE])
{
	unsigned long cursor = 0;

	/* (lines of "#" and blank lines, then the fingerprint, then only space) */
	while (cursor < size)
	{
		if (bytes[cursor] == '#')
		{
			while (cursor < size && bytes[cursor] != '\n')
				cursor++;
		}
		else if (bytes[cursor] == ' ' || bytes[cursor] == '\t' || bytes[cursor] == '\r' || bytes[cursor] == '\n')
		{
			cursor++;
		}
		else
		{
			break;
		}
	}
	if (size - cursor < MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE - 1 || memcmp(bytes + cursor, "lm1-", 4))
	{
		return FALSE;
	}
	{
		int index;

		for (index = 4; index < MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE - 1; index++)
		{
			char digit = bytes[cursor + index];

			if (digit >= 'A' && digit <= 'F')
				digit = (char)(digit - 'A' + 'a');
			if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
				return FALSE;
			text[index] = digit;
		}
		memcpy(text, "lm1-", 4);
		text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE - 1] = 0;
	}
	for (cursor += MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE - 1; cursor < size; cursor++)
	{
		if (bytes[cursor] != ' ' && bytes[cursor] != '\t' && bytes[cursor] != '\r' && bytes[cursor] != '\n')
			return FALSE;
	}

	return TRUE;
}

unsigned long mod_lightmap_page_size(
	struct bitmap_data const *bitmap)
{
	unsigned long width = (unsigned long)bitmap->width;
	unsigned long height = (unsigned long)bitmap->height;

	switch (bitmap->format)
	{
	case _lightmap_format_a8:
	case _lightmap_format_y8:
	case _lightmap_format_ay8:
		return width * height;
	case _lightmap_format_a8y8:
	case _lightmap_format_r5g6b5:
	case _lightmap_format_a1r5g5b5:
	case _lightmap_format_a4r4g4b4:
		return width * height * 2;
	case _lightmap_format_x8r8g8b8:
	case _lightmap_format_a8r8g8b8:
		return width * height * 4;
	case _lightmap_format_dxt1:
		return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case _lightmap_format_dxt3:
	case _lightmap_format_dxt5:
		return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	default:
		return 0;
	}
}

boolean mod_lightmap_page_luminance(
	struct bitmap_data const *bitmap,
	byte const *pixels,
	real *luminance)
{
	long width = bitmap->width;
	long height = bitmap->height;
	boolean swizzled = TEST_FLAG(bitmap->flags, _lightmap_swizzled_bit) != 0;
	unsigned long x_mask = 0, y_mask = 0;
	long x, y;

	if (!mod_lightmap_page_size(bitmap))
	{
		return FALSE;
	}
	if (bitmap->format >= _lightmap_format_dxt1)
	{
		long blocks_wide = (width + 3) / 4;
		long block_size = bitmap->format == _lightmap_format_dxt1 ? 8 : 16;

		for (y = 0; y < height; y += 4)
		{
			for (x = 0; x < width; x += 4)
			{
				byte const *block = pixels + ((y / 4) * blocks_wide + x / 4) * block_size;
				real texels[16];
				long i, j;

				/* (DXT3 and DXT5 keep their alpha first, then a DXT1's colors) */
				dxt_block_luminance(block_size == 16 ? block + 8 : block, block_size == 8, texels);
				for (j = 0; j < 4 && y + j < height; j++)
				{
					for (i = 0; i < 4 && x + i < width; i++)
						luminance[(y + j) * width + x + i] = texels[j * 4 + i];
				}
			}
		}
		return TRUE;
	}
	if (swizzled)
	{
		swizzle_masks(width, height, &x_mask, &y_mask);
	}
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			unsigned long at = swizzled ? swizzle_spread(x_mask, (unsigned long)x) | swizzle_spread(y_mask, (unsigned long)y) :
				(unsigned long)(y * width + x);
			real value;

			switch (bitmap->format)
			{
			case _lightmap_format_a8:
			case _lightmap_format_y8:
			case _lightmap_format_ay8:
				value = pixels[at];
				break;
			case _lightmap_format_a8y8:
				value = pixels[at * 2];
				break;
			case _lightmap_format_r5g6b5:
				value = luminance_565(read16(pixels + at * 2));
				break;
			case _lightmap_format_a1r5g5b5:
			{
				unsigned long texel = read16(pixels + at * 2);
				long red = (texel >> 10) & 31, green = (texel >> 5) & 31, blue = texel & 31;

				value = luminance_of((red << 3) | (red >> 2), (green << 3) | (green >> 2), (blue << 3) | (blue >> 2));
				break;
			}
			case _lightmap_format_a4r4g4b4:
			{
				unsigned long texel = read16(pixels + at * 2);

				value = luminance_of(((texel >> 8) & 15) * 17, ((texel >> 4) & 15) * 17, (texel & 15) * 17);
				break;
			}
			default: /* X8R8G8B8, A8R8G8B8: blue, green, red, alpha */
				value = luminance_of(pixels[at * 4 + 2], pixels[at * 4 + 1], pixels[at * 4]);
				break;
			}
			luminance[y * width + x] = value;
		}
	}

	return TRUE;
}

real mod_lightmap_page_correlation(
	real const *map_luminance,
	long map_width,
	long map_height,
	real const *mod_luminance,
	long mod_width,
	long mod_height)
{
	long count = map_width * map_height;
	real *reduced = malloc((size_t)count * 3 * sizeof(real));
	real *map_blurred, *mod_blurred;
	double map_mean = 0., mod_mean = 0., map_variance = 0., mod_variance = 0., covariance = 0.;
	real correlation;
	long x, y, index;

	if (!reduced)
	{
		return -2.f;
	}
	map_blurred = reduced + count;
	mod_blurred = map_blurred + count;
	/* the mod's page at the map's size: each pixel the mean of the mod's
	pixels whose centers are under it (the nearest, where none is) */
	for (y = 0; y < map_height; y++)
	{
		long y0 = (y * mod_height) / map_height;
		long y1 = MAX(((y + 1) * mod_height) / map_height, y0 + 1);

		for (x = 0; x < map_width; x++)
		{
			long x0 = (x * mod_width) / map_width;
			long x1 = MAX(((x + 1) * mod_width) / map_width, x0 + 1);
			real sum = 0.f;
			long i, j;

			for (j = y0; j < y1; j++)
			{
				for (i = x0; i < x1; i++)
					sum += mod_luminance[j * mod_width + i];
			}
			reduced[y * map_width + x] = sum / (real)((y1 - y0) * (x1 - x0));
		}
	}
	luminance_blur(map_luminance, map_width, map_height, map_blurred);
	luminance_blur(reduced, map_width, map_height, mod_blurred);
	for (index = 0; index < count; index++)
	{
		map_mean += map_blurred[index];
		mod_mean += mod_blurred[index];
	}
	map_mean /= count;
	mod_mean /= count;
	for (index = 0; index < count; index++)
	{
		double map_deviation = map_blurred[index] - map_mean;
		double mod_deviation = mod_blurred[index] - mod_mean;

		map_variance += map_deviation * map_deviation;
		mod_variance += mod_deviation * mod_deviation;
		covariance += map_deviation * mod_deviation;
	}
	free(reduced);
	/* (a flat page of the map has no picture to compare: it passes; the
	mod's flat where the map's is not, it has none of it) */
	if (map_variance < (double)count)
	{
		return 1.f;
	}
	if (mod_variance < (double)count)
	{
		return 0.f;
	}
	correlation = (real)(covariance / sqrt(map_variance * mod_variance));

	return correlation;
}
