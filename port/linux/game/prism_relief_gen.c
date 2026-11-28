/*
PRISM_RELIEF_GEN.C

prism_relief_gen.h has what is made and why. C89, no game headers, integer
arithmetic alone (tools/test_prism_relief.py builds it on its own and pins
its bytes).
*/

#include "prism_relief_gen.h"

#include <stdlib.h>
#include <string.h>

/* ---------- constants */

/* the bump map's slopes as read: 1/256ths of a texel's height a texel, at
most 4 texels a texel; its z at least 16 (of 127) */
#define SLOPE_ONE 256
#define SLOPE_MOST 1024
#define BUMP_Z_LEAST 16
/* a bump map with relief: at least one texel in a hundred this steep (an
eighth, x or y; b30's flat metal's mottling has none, its strips' grooves 9%
to 15%, its floors' plates 27%) */
#define BUMP_STEEP 32
#define BUMP_STEEP_PERCENT 1
/* a base map with a design: its band's range of luminance (2nd to 98th
percentile, of 255) at least this (b30's plain metal's mottling, 16 and
17, is no design; its ext panels' seams are 41, its narrow strips' 100) */
#define BASE_RANGE_LEAST 20
/* the designs' band: the tile's longer side over this, each way (twice a
box of that radius about each texel: the drift of the solve and the base
map's painted light taken out) */
#define BAND_DIVISOR 4
/* the relief's height, 1024 its whole depth */
#define HEIGHT_ONE 1024
/* the depth drawn, 1/256ths of a texel: from the bump map, its own times
the gain, within these; from the base map alone, BASE_DEPTH at a side of
256 (as many texels more a larger side has) */
#define DEPTH_LEAST 64
#define DEPTH_MOST 1024
#define BASE_DEPTH 144
/* the solve: V-cycles, and the smoothing passes of each level, and of the
coarsest */
#define CYCLES 12
#define SMOOTHING 2
#define COARSEST_SMOOTHING 256
/* the KTX2 file (xgpu_ktx2.c reads it) */
#define KTX2_HEADER_BYTES 80
#define KTX2_LEVEL_BYTES 24
#define VK_FORMAT_BC5_UNORM_BLOCK 141

/* ---------- integer helpers */

/* a / b rounded down (b > 0) */
static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;

	if (a % b != 0 && a < 0)
		q--;
	return q;
}

/* a / b rounded to the nearest, halves away from 0 (b > 0) */
static int64_t round_div(int64_t a, int64_t b)
{
	return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

static int64_t clamp64(int64_t value, int64_t lo, int64_t hi)
{
	return value < lo ? lo : value > hi ? hi : value;
}

/* the integer square root (the largest r with r * r <= value) */
static int64_t isqrt64(int64_t value)
{
	uint64_t v = (uint64_t)value, r = 0, bit = (uint64_t)1 << 62;

	if (value <= 0)
		return 0;
	while (bit > v)
		bit >>= 2;
	while (bit)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;
		bit >>= 2;
	}
	return (int64_t)r;
}

static int power_of_two(unsigned long value)
{
	return value && !(value & (value - 1));
}

static int compare64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

/* ---------- fields on the tile (each edge wrapping to the other) */

/* a box of radius r about each texel, along x then y, its mean (rounded
down) */
static int box(int64_t *values, int64_t *scratch, long width, long height, long r)
{
	long x, y, k, n = 2 * r + 1;

	for (y = 0; y < height; y++)
	{
		int64_t *row = values + y * width, sum = 0;

		for (k = -r; k <= r; k++)
			sum += row[((k % width) + width) % width];
		for (x = 0; x < width; x++)
		{
			scratch[y * width + x] = floor_div(sum, n);
			sum += row[(x + r + 1) % width] - row[((x - r) % width + width) % width];
		}
	}
	for (x = 0; x < width; x++)
	{
		int64_t sum = 0;

		for (k = -r; k <= r; k++)
			sum += scratch[(((k % height) + height) % height) * width + x];
		for (y = 0; y < height; y++)
		{
			values[y * width + x] = floor_div(sum, n);
			sum += scratch[((y + r + 1) % height) * width + x] -
				scratch[(((y - r) % height + height) % height) * width + x];
		}
	}
	return 1;
}

/* the values less their mean over the band (twice a box of radius r) */
static int band(int64_t *values, long width, long height, long r)
{
	long count = width * height, index;
	int64_t *low = malloc((size_t)count * sizeof(*low)), *scratch = malloc((size_t)count * sizeof(*scratch));

	if (!low || !scratch)
	{
		free(low);
		free(scratch);
		return 0;
	}
	memcpy(low, values, (size_t)count * sizeof(*low));
	box(low, scratch, width, height, r);
	box(low, scratch, width, height, r);
	for (index = 0; index < count; index++)
		values[index] -= low[index];
	free(low);
	free(scratch);
	return 1;
}

/* the values' 2nd and 98th percentiles; 0 no memory */
static int percentiles(const int64_t *values, long count, int64_t *lo, int64_t *hi)
{
	int64_t *sorted = malloc((size_t)count * sizeof(*sorted));

	if (!sorted)
		return 0;
	memcpy(sorted, values, (size_t)count * sizeof(*sorted));
	qsort(sorted, (size_t)count, sizeof(*sorted), compare64);
	*lo = sorted[count * 2 / 100];
	*hi = sorted[count * 98 / 100];
	free(sorted);
	return 1;
}

/* the values from lo to hi as the relief's height (0 to HEIGHT_ONE), its
plateaus flattened and its walls steepened (3 t^2 - 2 t^3) */
static void shape(int64_t *values, long count, int64_t lo, int64_t hi)
{
	long index;

	for (index = 0; index < count; index++)
	{
		int64_t t = hi > lo ? clamp64(floor_div((values[index] - lo) * HEIGHT_ONE, hi - lo), 0, HEIGHT_ONE) : 0;

		values[index] = (3 * t * t * HEIGHT_ONE - 2 * t * t * t) / ((int64_t)HEIGHT_ONE * HEIGHT_ONE);
	}
}

static int median3(int64_t *values, long width, long height);

/* ---------- the bump map's height (a Poisson solve of its slopes) */

/* red-black Gauss-Seidel passes of 2 (the neighbours' sum - 4 u) = f */
static void smooth(int64_t *u, const int64_t *f, long width, long height, int passes)
{
	int pass, colour;
	long x, y;

	for (pass = 0; pass < passes; pass++)
	{
		for (colour = 0; colour < 2; colour++)
		{
			for (y = 0; y < height; y++)
			{
				long up = ((y + height - 1) % height) * width, down = ((y + 1) % height) * width;

				for (x = (y + colour) & 1; x < width; x += 2)
				{
					long left = (x + width - 1) % width, right = (x + 1) % width;
					int64_t sum = u[y * width + left] + u[y * width + right] + u[up + x] + u[down + x];

					u[y * width + x] = floor_div(2 * sum - f[y * width + x], 8);
				}
			}
		}
	}
}

/* one V-cycle of the solve at a level (its coarser levels in turn): the
residual's sum of each 2 x 2 the coarser level's f, its correction added to
each of the 2 x 2 */
static int cycle(int64_t *u, const int64_t *f, long width, long height)
{
	long cw = width / 2, ch = height / 2, x, y;
	int64_t *r, *cu, *cf;

	if (width <= 4 || height <= 4)
	{
		smooth(u, f, width, height, COARSEST_SMOOTHING);
		return 1;
	}
	smooth(u, f, width, height, SMOOTHING);
	r = malloc((size_t)(width * height) * sizeof(*r));
	cu = calloc((size_t)(cw * ch), sizeof(*cu));
	cf = calloc((size_t)(cw * ch), sizeof(*cf));
	if (!r || !cu || !cf)
	{
		free(r);
		free(cu);
		free(cf);
		return 0;
	}
	for (y = 0; y < height; y++)
	{
		long up = ((y + height - 1) % height) * width, down = ((y + 1) % height) * width;

		for (x = 0; x < width; x++)
		{
			int64_t sum = u[y * width + (x + width - 1) % width] + u[y * width + (x + 1) % width] + u[up + x] +
				u[down + x];

			r[y * width + x] = f[y * width + x] - 2 * (sum - 4 * u[y * width + x]);
		}
	}
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			cf[(y / 2) * cw + x / 2] += r[y * width + x];
	free(r);
	if (!cycle(cu, cf, cw, ch))
	{
		free(cu);
		free(cf);
		return 0;
	}
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			u[y * width + x] += cu[(y / 2) * cw + x / 2];
	free(cu);
	free(cf);
	smooth(u, f, width, height, SMOOTHING);
	return 1;
}

/* the bump map's height (1/256ths of a texel; its mean 0) into height, and
how many of its texels are steep (BUMP_STEEP); 0 no memory */
static int bump_height(const uint32_t *bump, long width, long height, int64_t *out, long *steep)
{
	long count = width * height, index, x, y, k;
	int64_t *gx = malloc((size_t)count * sizeof(*gx)), *gy = malloc((size_t)count * sizeof(*gy));
	int64_t *f = malloc((size_t)count * sizeof(*f)), sum = 0;

	if (!gx || !gy || !f)
	{
		free(gx);
		free(gy);
		free(f);
		return 0;
	}
	/* (the slopes, -x / z and -y / z: the height rises against the normal's
	lean) */
	for (index = 0; index < count; index++)
	{
		uint32_t word = bump[index];
		int64_t nx = (int64_t)((word >> 16) & 0xffu) - 128, ny = (int64_t)((word >> 8) & 0xffu) - 128;
		int64_t nz = (int64_t)(word & 0xffu) - 128;

		if (nz < BUMP_Z_LEAST)
			nz = BUMP_Z_LEAST;
		gx[index] = clamp64(-nx * SLOPE_ONE / nz, -SLOPE_MOST, SLOPE_MOST);
		gy[index] = clamp64(-ny * SLOPE_ONE / nz, -SLOPE_MOST, SLOPE_MOST);
		if (gx[index] >= BUMP_STEEP || gx[index] <= -BUMP_STEEP || gy[index] >= BUMP_STEEP || gy[index] <= -BUMP_STEEP)
			(*steep)++;
	}
	/* (the slopes' divergence, by central differences, twice: the solve's f) */
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			f[y * width + x] = gx[y * width + (x + 1) % width] - gx[y * width + (x + width - 1) % width] +
				gy[((y + 1) % height) * width + x] - gy[((y + height - 1) % height) * width + x];
	free(gx);
	free(gy);
	memset(out, 0, (size_t)count * sizeof(*out));
	for (k = 0; k < CYCLES; k++)
	{
		if (!cycle(out, f, width, height))
		{
			free(f);
			return 0;
		}
	}
	free(f);
	for (index = 0; index < count; index++)
		sum += out[index];
	sum = floor_div(sum, count);
	for (index = 0; index < count; index++)
		out[index] -= sum;
	return 1;
}

/* ---------- the base map's design */

/* the base map's luminance, 1/256ths, into out */
static void base_luminance(const uint32_t *base, long count, int64_t *out)
{
	long index;

	for (index = 0; index < count; index++)
	{
		uint32_t word = base[index];

		out[index] = 77 * (int64_t)((word >> 16) & 0xffu) + 150 * (int64_t)((word >> 8) & 0xffu) +
			29 * (int64_t)(word & 0xffu);
	}
}

/* each texel the median of the 3 x 3 about it (the base map's speckle
taken out, its edges kept) */
static int median3(int64_t *values, long width, long height)
{
	long count = width * height, x, y;
	int64_t *copy = malloc((size_t)count * sizeof(*copy));

	if (!copy)
		return 0;
	memcpy(copy, values, (size_t)count * sizeof(*copy));
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			int64_t v[9];
			int i, j, n = 0;

			for (j = -1; j <= 1; j++)
				for (i = -1; i <= 1; i++)
					v[n++] = copy[((y + j + height) % height) * width + (x + i + width) % width];
			/* (an insertion sort: nine values) */
			for (i = 1; i < 9; i++)
			{
				int64_t t = v[i];

				for (j = i - 1; j >= 0 && v[j] > t; j--)
					v[j + 1] = v[j];
				v[j + 1] = t;
			}
			values[y * width + x] = v[4];
		}
	}
	free(copy);
	return 1;
}

/* ---------- public code */

int prism_relief_make(const struct prism_relief_source *source, struct prism_relief *relief)
{
	long width, height, count, x, y, r;
	int64_t *height_field = NULL, lo, hi;
	long steep = 0;
	int use_bump = 0;

	memset(relief, 0, sizeof(*relief));
	if (source->bump && (!power_of_two(source->bump_width) || !power_of_two(source->bump_height) ||
		source->bump_width > PRISM_RELIEF_SIDE_MAXIMUM || source->bump_height > PRISM_RELIEF_SIDE_MAXIMUM ||
		source->bump_width < 8 || source->bump_height < 8))
		return PRISM_RELIEF_SIZE;
	if (source->base && (!power_of_two(source->base_width) || !power_of_two(source->base_height) ||
		source->base_width > PRISM_RELIEF_SIDE_MAXIMUM || source->base_height > PRISM_RELIEF_SIDE_MAXIMUM ||
		source->base_width < 8 || source->base_height < 8))
		return PRISM_RELIEF_SIZE;
	/* (the bump map's relief, where it has some) */
	if (source->bump)
	{
		width = (long)source->bump_width;
		height = (long)source->bump_height;
		if (!(height_field = malloc((size_t)(width * height) * sizeof(*height_field))))
			return PRISM_RELIEF_MEMORY;
		if (!bump_height(source->bump, width, height, height_field, &steep))
		{
			free(height_field);
			return PRISM_RELIEF_MEMORY;
		}
		use_bump = steep * 100 >= width * height * BUMP_STEEP_PERCENT;
		if (!use_bump)
		{
			free(height_field);
			height_field = NULL;
		}
	}
	/* (else the base map's design: CE's bump maps carry the designs where
	they have any, and the base map's painted cracks and mottling raised
	beside them were noise) */
	if (!use_bump)
	{
		if (!source->base)
			return PRISM_RELIEF_NO_DESIGN;
		width = (long)source->base_width;
		height = (long)source->base_height;
		if (!(height_field = malloc((size_t)(width * height) * sizeof(*height_field))))
			return PRISM_RELIEF_MEMORY;
		base_luminance(source->base, width * height, height_field);
		if (!median3(height_field, width, height))
		{
			free(height_field);
			return PRISM_RELIEF_MEMORY;
		}
	}
	count = width * height;
	r = (width > height ? width : height) / BAND_DIVISOR;
	if (r < 1)
		r = 1;
	/* (the designs' band; the bump map's speckle taken out) */
	if (!band(height_field, width, height, r) ||
		(use_bump && (!median3(height_field, width, height) || !median3(height_field, width, height))) ||
		!percentiles(height_field, count, &lo, &hi))
	{
		free(height_field);
		return PRISM_RELIEF_MEMORY;
	}
	if (use_bump)
		relief->bump_range = (long)(hi - lo);
	else
	{
		relief->base_range = (long)((hi - lo) / 256);
		if (relief->base_range < BASE_RANGE_LEAST)
		{
			free(height_field);
			return PRISM_RELIEF_NO_DESIGN;
		}
	}
	shape(height_field, count, lo, hi);
	relief->at_bump = use_bump;
	relief->depth = use_bump ? (long)clamp64((int64_t)relief->bump_range * PRISM_RELIEF_GAIN_EIGHTHS / 8, DEPTH_LEAST,
		DEPTH_MOST) : (long)clamp64((int64_t)BASE_DEPTH * (width > height ? width : height) / 256, DEPTH_LEAST,
		DEPTH_MOST);
	if (!(relief->normal = malloc((size_t)count * 2)))
	{
		free(height_field);
		return PRISM_RELIEF_MEMORY;
	}
	relief->width = (unsigned long)width;
	relief->height = (unsigned long)height;
	/* (the normal: the height's central differences, the depth's slopes,
	normalised; y down the picture, as CE's bump maps) */
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			int64_t dx = height_field[y * width + (x + 1) % width] - height_field[y * width + (x + width - 1) % width];
			int64_t dy = height_field[((y + 1) % height) * width + x] -
				height_field[((y + height - 1) % height) * width + x];
			int64_t vx = -round_div(dx * relief->depth, 2 * HEIGHT_ONE), vy = -round_div(dy * relief->depth,
				2 * HEIGHT_ONE);
			int64_t length = isqrt64(vx * vx + vy * vy + (int64_t)SLOPE_ONE * SLOPE_ONE);

			relief->normal[(y * width + x) * 2] = (unsigned char)clamp64(128 + round_div(127 * vx, length), 1, 255);
			relief->normal[(y * width + x) * 2 + 1] = (unsigned char)clamp64(128 + round_div(127 * vy, length), 1,
				255);
		}
	}
	free(height_field);
	return PRISM_RELIEF_MADE;
}

void prism_relief_free(struct prism_relief *relief)
{
	free(relief->normal);
	relief->normal = NULL;
}

/* ---------- the KTX2 file */

/* a BC4 block of 16 values (each row of 4 in turn) */
static void bc4_block(const int values[16], unsigned char block[8])
{
	int lo = 255, hi = 0, palette[8], index, k;
	uint64_t bits = 0;

	for (index = 0; index < 16; index++)
	{
		if (values[index] < lo)
			lo = values[index];
		if (values[index] > hi)
			hi = values[index];
	}
	memset(block, 0, 8);
	block[0] = (unsigned char)hi;
	block[1] = (unsigned char)lo;
	if (hi == lo)
		return;
	/* (hi above lo: the eight values' mode) */
	palette[0] = hi;
	palette[1] = lo;
	for (k = 2; k < 8; k++)
		palette[k] = ((8 - k) * hi + (k - 1) * lo + 3) / 7;
	for (index = 0; index < 16; index++)
	{
		int best = 0, error = 256;

		for (k = 0; k < 8; k++)
		{
			int e = values[index] > palette[k] ? values[index] - palette[k] : palette[k] - values[index];

			if (e < error)
			{
				error = e;
				best = k;
			}
		}
		bits |= (uint64_t)best << (3 * index);
	}
	for (k = 0; k < 6; k++)
		block[2 + k] = (unsigned char)(bits >> (8 * k));
}

static void put32(unsigned char *to, uint32_t value)
{
	to[0] = (unsigned char)value;
	to[1] = (unsigned char)(value >> 8);
	to[2] = (unsigned char)(value >> 16);
	to[3] = (unsigned char)(value >> 24);
}

static void put64(unsigned char *to, uint64_t value)
{
	put32(to, (uint32_t)value);
	put32(to + 4, (uint32_t)(value >> 32));
}

static unsigned long level_side(unsigned long side, unsigned long level)
{
	side >>= level;
	return side ? side : 1;
}

static unsigned long level_bytes(unsigned long width, unsigned long height, unsigned long level)
{
	return ((level_side(width, level) + 3) / 4) * ((level_side(height, level) + 3) / 4) * 16;
}

unsigned char *prism_relief_ktx2(const unsigned char *normal, unsigned long width, unsigned long height,
	unsigned long *size)
{
	static const unsigned char identifier[12] = { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a,
		0x0a };
	unsigned long levels = 1, side = width > height ? width : height, level, offset, total;
	unsigned char *file, *at;
	int *signed_now, *signed_next;

	while (side > 1)
	{
		side >>= 1;
		levels++;
	}
	/* (the levels after the index, the smallest first, each at 16 bytes) */
	offset = KTX2_HEADER_BYTES + KTX2_LEVEL_BYTES * levels;
	offset = (offset + 15) & ~15UL;
	total = offset;
	for (level = 0; level < levels; level++)
		total += level_bytes(width, height, level);
	file = calloc(total, 1);
	signed_now = malloc(width * height * 2 * sizeof(*signed_now));
	signed_next = malloc(width * height * 2 * sizeof(*signed_next));
	if (!file || !signed_now || !signed_next)
	{
		free(file);
		free(signed_now);
		free(signed_next);
		return NULL;
	}
	memcpy(file, identifier, sizeof(identifier));
	put32(file + 12, VK_FORMAT_BC5_UNORM_BLOCK);
	put32(file + 16, 1);
	put32(file + 20, (uint32_t)width);
	put32(file + 24, (uint32_t)height);
	put32(file + 36, 1);
	put32(file + 40, (uint32_t)levels);
	for (level = 0; level < width * height * 2; level++)
		signed_now[level] = (int)normal[level] - 128;
	/* (each level's place: after the smaller ones) */
	at = file + total;
	for (level = 0; level < levels; level++)
	{
		unsigned long lw = level_side(width, level), lh = level_side(height, level), bytes = level_bytes(width, height,
			level), bx, by, x, y;
		unsigned char *blocks;

		at -= bytes;
		blocks = at;
		put64(file + KTX2_HEADER_BYTES + KTX2_LEVEL_BYTES * level, (uint64_t)(at - file));
		put64(file + KTX2_HEADER_BYTES + KTX2_LEVEL_BYTES * level + 8, bytes);
		put64(file + KTX2_HEADER_BYTES + KTX2_LEVEL_BYTES * level + 16, bytes);
		for (by = 0; by < (lh + 3) / 4; by++)
		{
			for (bx = 0; bx < (lw + 3) / 4; bx++)
			{
				int channel;

				for (channel = 0; channel < 2; channel++)
				{
					int values[16], k = 0;

					/* (a level under 4 texels: its texels again) */
					for (y = 0; y < 4; y++)
					{
						for (x = 0; x < 4; x++)
						{
							unsigned long tx = bx * 4 + x < lw ? bx * 4 + x : lw - 1;
							unsigned long ty = by * 4 + y < lh ? by * 4 + y : lh - 1;

							values[k++] = signed_now[(ty * lw + tx) * 2 + channel] + 128;
						}
					}
					bc4_block(values, blocks);
					blocks += 8;
				}
			}
		}
		/* (the next level: each 2 x 2's mean, rounded) */
		if (level + 1 < levels)
		{
			unsigned long nw = level_side(width, level + 1), nh = level_side(height, level + 1);
			unsigned long sx = lw > 1 ? 2 : 1, sy = lh > 1 ? 2 : 1;

			for (y = 0; y < nh; y++)
			{
				for (x = 0; x < nw; x++)
				{
					int channel;

					for (channel = 0; channel < 2; channel++)
					{
						unsigned long i, j;
						int64_t sum = 0;

						for (j = 0; j < sy; j++)
							for (i = 0; i < sx; i++)
								sum += signed_now[((y * sy + j) * lw + x * sx + i) * 2 + channel];
						signed_next[(y * nw + x) * 2 + channel] = (int)round_div(sum, (int64_t)(sx * sy));
					}
				}
			}
			{
				int *swap = signed_now;

				signed_now = signed_next;
				signed_next = swap;
			}
		}
	}
	free(signed_now);
	free(signed_next);
	*size = total;
	return file;
}

/* ---------- the key */

static uint64_t fnv_byte(uint64_t hash, unsigned char byte)
{
	return (hash ^ byte) * (uint64_t)0x100000001B3ULL;
}

static uint64_t fnv_word(uint64_t hash, uint32_t word)
{
	hash = fnv_byte(hash, (unsigned char)word);
	hash = fnv_byte(hash, (unsigned char)(word >> 8));
	hash = fnv_byte(hash, (unsigned char)(word >> 16));
	return fnv_byte(hash, (unsigned char)(word >> 24));
}

uint64_t prism_relief_key(const struct prism_relief_source *source)
{
	uint64_t hash = (uint64_t)0xCBF29CE484222325ULL;
	unsigned long index;

	hash = fnv_word(hash, 0x52535250u);
	hash = fnv_word(hash, PRISM_RELIEF_VERSION);
	hash = fnv_word(hash, PRISM_RELIEF_GAIN_EIGHTHS);
	hash = fnv_word(hash, source->base ? (uint32_t)source->base_width : 0u);
	hash = fnv_word(hash, source->base ? (uint32_t)source->base_height : 0u);
	for (index = 0; source->base && index < source->base_width * source->base_height; index++)
		hash = fnv_word(hash, source->base[index]);
	hash = fnv_word(hash, source->bump ? (uint32_t)source->bump_width : 0u);
	hash = fnv_word(hash, source->bump ? (uint32_t)source->bump_height : 0u);
	for (index = 0; source->bump && index < source->bump_width * source->bump_height; index++)
		hash = fnv_word(hash, source->bump[index]);
	return hash;
}
