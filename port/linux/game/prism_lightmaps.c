/*
PRISM_LIGHTMAPS.C

display.prism_lightmaps "smooth" with display.prism_lightmap_upsample 2 or
4: each page of the structure's lightmap drawn from a finer copy rebuilt
on this machine from the player's own map, never shipped (PRISM's
licensing rule: nothing made of the game's pixels leaves the machine).

The copy is the same light in the same places, read better:

- the page's R5G6B5 texels (the texture the lightmap pass draws, decoded)
  dequantized: each channel of each texel the BSP's triangles cover
  smoothed with its covered 4-neighbours of the same chart, again and
  again, but never out of its own 16-bit step (the true light was within
  half a step of it), so that a band's edge becomes the ramp it stands for
  and a real edge stays where it is;
- upsampled by the factor, chart by chart: the triangles drawn again at the
  finer size say which chart each new texel is in, and it is the
  Catmull-Rom cubic of the 4 by 4 texels about it that are in its chart
  (the others' weights 0, the rest's sum made 1), clamped to the least and
  most of its 2 by 2 (no ring); a new texel no triangle covers (a gutter)
  is the bilinear sample of the dequantized page, as the stock texture's
  sampler would read there;
- RGBA8, alpha 255, its levels made by the host.

The shader's own filter (xgpu.h's "The lightmaps smoothed") reads it as it
reads a stock page. The models' light (object_lights.c's lightmap sample on
the CPU) still comes from the stock page: nothing the game plays changes.

When: the first time a page is drawn with the setting on (the lightmap
pass's page begun: world_ao.c's world_ao_lightmap_begin calls
prism_lightmaps_begin), its texels and the BSP's triangles' lightmap
coordinates are copied (the game's thread, a few ms) and handed to a
background thread at the lowest priority, which looks in the cache first
and else builds it and writes the cache. Until it is ready the stock page
is drawn (with the shader's filter). The image is drawn in the page's place
by xbox_textures.c's xgpu_texture_replacement while the lightmap pass draws
that page (the texture of the page last begun, its texels' address).

The cache (local, never shipped): <data>/prism_lightmaps/<map>_<bsp>_<page>
_x<factor>.plm: "PLM1", the version, the map's cache checksum, the page's
width and height, the factor, a hash of the stock texels (FNV-1a of the
decoded ARGB), the deflated size; then the deflated RGBA texels and a
CRC-32 of the header and them. A file that fails a check is built again.

display.prism_lightmaps "rebaked": each page drawn from the page baked
again offline on this machine from CE's own light sources (art_pipeline's
relight_bake.py, relight_calib.py and relight_encode.py: Cycles, the BSP's
sky lights and emitting shaders, bounce light, the factor times CE's size;
each light's strength and colour fitted to the pools of light CE's page
shows, so a lamp's pool keeps CE's hue and brightness, then matched area by
area to CE's page, the BSP's mean brightness CE's or a little above, never
below), read from <data>/prism_lightmaps/rebaked/
<map>_<bsp>_<page>.prl (local, never shipped): "PRL1", the version (2),
CE's page's width and height, the factor, the hash of CE's page's texels
(as above: a page baked against another map's page is not drawn), the
flags (1: the direction follows), the deflated size, the inflated size, a
CRC-32 of the 36 bytes before it and the deflated bytes; then the deflated
bytes: the light (RGBA8, rows from v = 0), then its direction (RGB d * 0.5
+ 0.5, A a), each the factor times CE's size. The image made of it is the
two side by side (xgpu.h's XGPU_LIGHTMAP_FILTER_REBAKED), its levels made
by the host; the draws of the pages that have one are keyed so, draw by
draw (prism_lightmaps_rebaked_drawing). A page with no file, or a file
that fails a check, is drawn as "smooth" draws it (its finer copy, with
display.prism_lightmap_upsample 2 or 4; else the stock page filtered).
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_geometry_environment.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_files.h"

#include "memory/zlib/zlib.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, which the building thread may call) */
#undef malloc
#undef free
#undef realloc

/* ---------- prototypes */

const char *platform_data_root(void);
void platform_log(char const *format, ...);
long config_integer(const char *name);
const char *config_string(const char *name);
int config_boolean(const char *name);
unsigned long config_changes(void);
int halo_server_mode(void);
unsigned long long SDL_GetTicksNS(void);
long __guest_syscall(long long number, long long a, long long b, long long c, long long d, long long e, long long f);
int mkdir(const char *path, unsigned int mode);
int z_uncompress(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size);
unsigned int xgpu_image_create(void *pixels, unsigned long width, unsigned long height, unsigned long levels,
	int format, int owned);
void xgpu_image_delete(unsigned int image);
/* (xbox_textures.c) a texture's first level as ARGB (malloc's), from its
header; NULL if it is not an R5G6B5 2D texture of one face */
unsigned long *xgpu_texture_565_argb(void const *header, unsigned long *width, unsigned long *height);
/* (d3d8_device.c) prism_lightmaps_filter's: the lightmaps smoothed; the
texture bound to a stage */
int prism_lightmaps_filter(void);
void const *halo_texture_bound(int stage);

void prism_lightmaps_begin(void const *lightmap_bitmap);
void prism_lightmaps_bsp_switched(void);
void prism_lightmaps_map_unloaded(void);
unsigned int prism_lightmaps_replacement(unsigned long data, unsigned long *levels);
int prism_lightmaps_rebaked_drawing(void);

/* ---------- constants */

#define PLM_MAGIC 0x314d4c50UL /* "PLM1" */
#define PLM_VERSION 1
#define PLM_HEADER 40
#define PLM_PAGES_MOST 256
/* the dequantization's passes, and the largest side of a copy */
#define PLM_DEQUANTIZE_PASSES 32
#define PLM_SIDE_MOST 2048
/* xgpu_images.h's XGPU_IMAGE_RGBA */
#define PLM_IMAGE_RGBA 0
/* the rebaked pages' files */
#define PRL_MAGIC 0x314c5250UL /* "PRL1" */
#define PRL_VERSION 2
#define PRL_HEADER 40
#define PRL_FACTOR_MOST 8

enum
{
	_plm_page_none,
	_plm_page_queued,
	_plm_page_ready,
	_plm_page_failed,
};

/* ---------- structures */

struct plm_job
{
	struct plm_job *next;
	unsigned long generation;
	long page;
	unsigned long width, height, factor, hash, checksum;
	unsigned long *argb;
	/* the page's triangles' lightmap coordinates (6 floats each) */
	float *uvs;
	long triangle_count;
	char path[256];
	/* the result: RGBA, (width, height) times the factor; NULL failed */
	unsigned char *rgba;
	int from_cache;
	unsigned long milliseconds;
	/* display.prism_lightmaps "rebaked": its file's path (empty none),
	and whether rgba is the rebaked page's two halves (its factor the
	file's) */
	char rebaked_path[256];
	int rebaked;
};

struct plm_page
{
	int state, rebaked;
	unsigned int image;
	unsigned long levels, data;
};

static struct
{
	/* the map, BSP and factor the pages are of (a change lets them go) */
	char map[64];
	short bsp;
	unsigned long checksum, factor, generation;
	struct plm_page pages[PLM_PAGES_MOST];
	/* the BSP's triangles copied, by page (on first need) */
	float *uvs[PLM_PAGES_MOST];
	long triangle_counts[PLM_PAGES_MOST];
	int copied;
	/* the page last begun: its index and its texture's texels' address */
	long current;
	unsigned long current_data;
	/* the settings as config_changes() was */
	unsigned long read_at;
	int read, enabled, rebaked_mode;
	unsigned long ready_count, built_count, cached_count;
	/* display.prism_lightmaps "rebaked": the structure's pages made without
	a rebaked file (the first is said in the log) */
	unsigned long rebaked_missing;
} plm;

static pthread_mutex_t plm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t plm_wake = PTHREAD_COND_INITIALIZER;
static struct plm_job *plm_queue, *plm_done;
static int plm_thread_running;

/* ---------- bytes */

static void plm_put32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

static unsigned long plm_get32(unsigned char const *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static unsigned long plm_crc32(unsigned long crc, unsigned char const *bytes, unsigned long count)
{
	unsigned long index;
	int bit;

	crc = ~crc & 0xffffffffUL;
	for (index = 0; index < count; index++)
	{
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
	}
	return ~crc & 0xffffffffUL;
}

static voidpf plm_zalloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void plm_zfree(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static int plm_deflate(unsigned char *destination, unsigned long *destination_size, unsigned char const *source,
	unsigned long source_size)
{
	z_stream stream;
	int result;

	memset(&stream, 0, sizeof(stream));
	stream.zalloc = plm_zalloc;
	stream.zfree = plm_zfree;
	if (deflateInit(&stream, 6) != Z_OK)
		return -1;
	stream.next_in = (Bytef *)source;
	stream.avail_in = (uInt)source_size;
	stream.next_out = destination;
	stream.avail_out = (uInt)*destination_size;
	result = deflate(&stream, Z_FINISH);
	*destination_size = stream.total_out;
	deflateEnd(&stream);
	return result == Z_STREAM_END ? 0 : -1;
}

/* ---------- the cache */

static void plm_header(unsigned char *header, struct plm_job const *job, unsigned long deflated)
{
	plm_put32(header + 0, PLM_MAGIC);
	plm_put32(header + 4, PLM_VERSION);
	plm_put32(header + 8, job->checksum);
	plm_put32(header + 12, job->width);
	plm_put32(header + 16, job->height);
	plm_put32(header + 20, job->factor);
	plm_put32(header + 24, job->hash);
	plm_put32(header + 28, deflated);
	plm_put32(header + 32, (unsigned long)job->page);
	plm_put32(header + 36, 0);
}

/* the copy from the cache, where its file holds this page's: 1 */
static int plm_cache_read(struct plm_job *job)
{
	unsigned char expected[PLM_HEADER], *bytes = NULL, *rgba = NULL;
	unsigned long size = 0, deflated, inflated, out = job->width * job->factor * job->height * job->factor * 4;
	FILE *file = fopen(job->path, "rb");
	int good = 0;

	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) == 0)
		size = (unsigned long)ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size > PLM_HEADER + 4 && size < 512UL * 1024 * 1024 && (bytes = malloc(size)) != NULL &&
		fread(bytes, 1, size, file) == size)
	{
		deflated = plm_get32(bytes + 28);
		plm_header(expected, job, deflated);
		if (!memcmp(bytes, expected, PLM_HEADER) && PLM_HEADER + deflated + 4 == size &&
			plm_get32(bytes + size - 4) == plm_crc32(0, bytes, size - 4) && (rgba = malloc(out)) != NULL)
		{
			inflated = out;
			if (z_uncompress(rgba, &inflated, bytes + PLM_HEADER, deflated) == 0 && inflated == out)
			{
				job->rgba = rgba;
				rgba = NULL;
				good = 1;
			}
		}
	}
	fclose(file);
	free(bytes);
	free(rgba);
	if (!good)
		remove(job->path);
	return good;
}

static void plm_cache_write(struct plm_job const *job)
{
	unsigned long size = job->width * job->factor * job->height * job->factor * 4;
	unsigned long room = size + size / 100 + 1024, deflated = room;
	unsigned char *bytes = malloc(PLM_HEADER + room + 4);
	char temporary[300], folder[1100];
	FILE *file;

	if (!bytes)
		return;
	if (plm_deflate(bytes + PLM_HEADER, &deflated, job->rgba, size) != 0)
	{
		free(bytes);
		return;
	}
	plm_header(bytes, job, deflated);
	plm_put32(bytes + PLM_HEADER + deflated, plm_crc32(0, bytes, PLM_HEADER + deflated));
	snprintf(folder, sizeof(folder), "%s/prism_lightmaps", platform_data_root());
	mkdir(folder, 0755);
	snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
	file = fopen(temporary, "wb");
	if (file)
	{
		int written = fwrite(bytes, 1, PLM_HEADER + deflated + 4, file) == PLM_HEADER + deflated + 4;

		written = fclose(file) == 0 && written;
		if (!written || rename(temporary, job->path) != 0)
			remove(temporary);
	}
	free(bytes);
}

/* ---------- the build (the background thread's) */

static float plm_clamp(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

static long plm_floor(float value)
{
	long whole = (long)value;

	return (float)whole > value ? whole - 1 : whole;
}

/* the triangles' cover of a grid of width by height at the coordinates'
scale: each cell whose centre a triangle holds marked with the triangle's
mark (marks[cell] = mark, by triangle: triangle_marks, or 1) */
static void plm_rasterize(float const *uvs, long triangle_count, long width, long height, long const *triangle_marks,
	long *marks)
{
	long triangle;

	for (triangle = 0; triangle < triangle_count; triangle++)
	{
		float const *t = uvs + triangle * 6;
		float x[3], y[3], area, low_x, high_x, low_y, high_y;
		long corner, column, row;

		for (corner = 0; corner < 3; corner++)
		{
			x[corner] = t[corner * 2] * (float)width;
			y[corner] = t[corner * 2 + 1] * (float)height;
		}
		area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (area == 0.0f)
			continue;
		low_x = high_x = x[0];
		low_y = high_y = y[0];
		for (corner = 1; corner < 3; corner++)
		{
			low_x = x[corner] < low_x ? x[corner] : low_x;
			high_x = x[corner] > high_x ? x[corner] : high_x;
			low_y = y[corner] < low_y ? y[corner] : low_y;
			high_y = y[corner] > high_y ? y[corner] : high_y;
		}
		/* (a cell a triangle only touches, its centre outside, still its:
		the bounds widened by half a cell, the edge test by a margin) */
		for (row = plm_floor(low_y - 0.5f); row <= plm_floor(high_y + 0.5f); row++)
		{
			if (row < 0 || row >= height)
				continue;
			for (column = plm_floor(low_x - 0.5f); column <= plm_floor(high_x + 0.5f); column++)
			{
				float px = (float)column + 0.5f, py = (float)row + 0.5f, w[3], margin = 0.75f;
				float scale;

				if (column < 0 || column >= width)
					continue;
				w[0] = (x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py);
				w[1] = (x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py);
				w[2] = (x[0] - px) * (y[1] - py) - (x[1] - px) * (y[0] - py);
				if (area < 0.0f)
				{
					w[0] = -w[0];
					w[1] = -w[1];
					w[2] = -w[2];
				}
				/* (each edge's function by its length: a half-cell margin
				in cells) */
				for (corner = 0; corner < 3; corner++)
				{
					float ex = x[(corner + 2) % 3] - x[(corner + 1) % 3], ey = y[(corner + 2) % 3] - y[(corner + 1) % 3];
					float length = ex * ex + ey * ey;

					scale = length > 0.0f ? length : 1.0f;
					if (w[corner] * w[corner] < margin * margin * scale && w[corner] < 0.0f)
						w[corner] = 0.0f;
				}
				if (w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f)
					marks[row * width + column] = triangle_marks ? triangle_marks[triangle] : 1;
			}
		}
	}
}

/* the covered cells' charts: 4-connected groups (from 1), in place of the
marks (0 stays 0); the count */
static long plm_charts(long *marks, long width, long height)
{
	long *stack = malloc((size_t)(width * height) * sizeof(*stack)), cell, charts = 0;

	if (!stack)
		return 0;
	for (cell = 0; cell < width * height; cell++)
	{
		long top = 0;

		if (marks[cell] != 1)
			continue;
		charts++;
		marks[cell] = -charts;
		stack[top++] = cell;
		while (top)
		{
			long at = stack[--top], x = at % width, y = at / width, n;
			long next[4];

			next[0] = x > 0 ? at - 1 : -1;
			next[1] = x < width - 1 ? at + 1 : -1;
			next[2] = y > 0 ? at - width : -1;
			next[3] = y < height - 1 ? at + width : -1;
			for (n = 0; n < 4; n++)
			{
				if (next[n] >= 0 && marks[next[n]] == 1)
				{
					marks[next[n]] = -charts;
					stack[top++] = next[n];
				}
			}
		}
	}
	for (cell = 0; cell < width * height; cell++)
		marks[cell] = marks[cell] < 0 ? -marks[cell] : 0;
	free(stack);
	return charts;
}

static float plm_catmull_rom(float t, int tap)
{
	/* the weight of the tap at -1, 0, 1, 2 for t in [0, 1) */
	float t2 = t * t, t3 = t2 * t;

	switch (tap)
	{
	case 0: return 0.5f * (-t3 + 2.0f * t2 - t);
	case 1: return 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
	case 2: return 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
	default: return 0.5f * (t3 - t2);
	}
}

static int plm_build(struct plm_job *job)
{
	long width = (long)job->width, height = (long)job->height, factor = (long)job->factor;
	long out_width = width * factor, out_height = height * factor, cell, channel, pass, triangle;
	float *value = malloc((size_t)(width * height * 3) * sizeof(float));
	float *next = malloc((size_t)(width * height * 3) * sizeof(float));
	float *stock = malloc((size_t)(width * height * 3) * sizeof(float));
	long *charts = calloc((size_t)(width * height), sizeof(long));
	long *out_charts = calloc((size_t)(out_width * out_height), sizeof(long));
	long *triangle_charts = malloc((size_t)(job->triangle_count > 0 ? job->triangle_count : 1) * sizeof(long));
	unsigned char *rgba = malloc((size_t)(out_width * out_height * 4));
	/* (half of each channel's 16-bit step, as decoded to 0..1: 5 bits' red
	and blue, 6 bits' green) */
	static const float half_step[3] = { 0.5f / 31.0f, 0.5f / 63.0f, 0.5f / 31.0f };
	int good = 0;

	if (!value || !next || !stock || !charts || !out_charts || !triangle_charts || !rgba)
		goto done;
	for (cell = 0; cell < width * height; cell++)
	{
		unsigned long argb = job->argb[cell];

		stock[cell * 3 + 0] = (float)((argb >> 16) & 0xff) / 255.0f;
		stock[cell * 3 + 1] = (float)((argb >> 8) & 0xff) / 255.0f;
		stock[cell * 3 + 2] = (float)(argb & 0xff) / 255.0f;
	}
	memcpy(value, stock, (size_t)(width * height * 3) * sizeof(float));
	/* the charts: the cells the triangles cover, in 4-connected groups */
	plm_rasterize(job->uvs, job->triangle_count, width, height, NULL, charts);
	plm_charts(charts, width, height);
	/* dequantized: within each chart, toward the neighbours' mean, never
	out of the cell's own step */
	for (pass = 0; pass < PLM_DEQUANTIZE_PASSES; pass++)
	{
		for (cell = 0; cell < width * height; cell++)
		{
			long x = cell % width, y = cell / width, n, count = 1, chart = charts[cell];
			long neighbours[4];
			float sum[3];

			for (channel = 0; channel < 3; channel++)
				sum[channel] = value[cell * 3 + channel];
			if (chart)
			{
				neighbours[0] = x > 0 ? cell - 1 : -1;
				neighbours[1] = x < width - 1 ? cell + 1 : -1;
				neighbours[2] = y > 0 ? cell - width : -1;
				neighbours[3] = y < height - 1 ? cell + width : -1;
				for (n = 0; n < 4; n++)
				{
					if (neighbours[n] < 0 || charts[neighbours[n]] != chart)
						continue;
					for (channel = 0; channel < 3; channel++)
						sum[channel] += value[neighbours[n] * 3 + channel];
					count++;
				}
			}
			for (channel = 0; channel < 3; channel++)
			{
				float s = stock[cell * 3 + channel];

				next[cell * 3 + channel] = plm_clamp(sum[channel] / (float)count, s - half_step[channel],
					s + half_step[channel]);
			}
		}
		memcpy(value, next, (size_t)(width * height * 3) * sizeof(float));
	}
	/* each triangle's chart: the chart of the covered cell nearest its
	middle (a triangle smaller than a cell: none, 0) */
	for (triangle = 0; triangle < job->triangle_count; triangle++)
	{
		float const *t = job->uvs + triangle * 6;
		long x = plm_floor((t[0] + t[2] + t[4]) / 3.0f * (float)width);
		long y = plm_floor((t[1] + t[3] + t[5]) / 3.0f * (float)height);
		long chart = 0, dy, dx;

		for (dy = 0; dy <= 1 && !chart; dy++)
		{
			for (dx = -dy; dx <= dy && !chart; dx++)
			{
				long r, c;

				for (r = y - dy; r <= y + dy && !chart; r++)
				{
					for (c = x - dy; c <= x + dy && !chart; c++)
					{
						if (r >= 0 && r < height && c >= 0 && c < width)
							chart = charts[r * width + c];
					}
				}
			}
		}
		triangle_charts[triangle] = chart;
	}
	plm_rasterize(job->uvs, job->triangle_count, out_width, out_height, triangle_charts, out_charts);
	/* the finer texels */
	for (cell = 0; cell < out_width * out_height; cell++)
	{
		long ox = cell % out_width, oy = cell / out_width, chart = out_charts[cell];
		/* (the source's coordinates of the finer texel's centre) */
		float sx = ((float)ox + 0.5f) / (float)factor - 0.5f, sy = ((float)oy + 0.5f) / (float)factor - 0.5f;
		long bx = plm_floor(sx), by = plm_floor(sy), i, j;
		float fx = sx - (float)bx, fy = sy - (float)by, result[3], weight_sum = 0.0f;
		float low[3] = { 1.0f, 1.0f, 1.0f }, high[3] = { 0.0f, 0.0f, 0.0f };
		int near_found = 0;

		result[0] = result[1] = result[2] = 0.0f;
		if (chart)
		{
			for (j = 0; j < 4; j++)
			{
				long yy = by - 1 + j;
				float wy = plm_catmull_rom(fy, (int)j);

				if (yy < 0 || yy >= height)
					continue;
				for (i = 0; i < 4; i++)
				{
					long xx = bx - 1 + i, at;
					float w;

					if (xx < 0 || xx >= width)
						continue;
					at = yy * width + xx;
					if (charts[at] != chart)
						continue;
					w = plm_catmull_rom(fx, (int)i) * wy;
					for (channel = 0; channel < 3; channel++)
					{
						result[channel] += w * value[at * 3 + channel];
						if ((i == 1 || i == 2) && (j == 1 || j == 2))
						{
							low[channel] = value[at * 3 + channel] < low[channel] ? value[at * 3 + channel] : low[channel];
							high[channel] = value[at * 3 + channel] > high[channel] ? value[at * 3 + channel] : high[channel];
						}
					}
					if ((i == 1 || i == 2) && (j == 1 || j == 2))
						near_found = 1;
					weight_sum += w;
				}
			}
		}
		if (chart && near_found && weight_sum > 0.05f)
		{
			for (channel = 0; channel < 3; channel++)
				result[channel] = plm_clamp(result[channel] / weight_sum, low[channel], high[channel]);
		}
		else
		{
			/* (a gutter, or a chart's corner with none of its own near:
			the bilinear sample of the dequantized page, clamped at its
			edges as the sampler is) */
			for (channel = 0; channel < 3; channel++)
			{
				long x0 = bx < 0 ? 0 : bx >= width ? width - 1 : bx, x1 = bx + 1 >= width ? width - 1 : bx + 1 < 0 ? 0 : bx + 1;
				long y0 = by < 0 ? 0 : by >= height ? height - 1 : by, y1 = by + 1 >= height ? height - 1 : by + 1 < 0 ? 0 : by + 1;
				float top = value[(y0 * width + x0) * 3 + channel] * (1.0f - fx) + value[(y0 * width + x1) * 3 + channel] * fx;
				float bottom = value[(y1 * width + x0) * 3 + channel] * (1.0f - fx) + value[(y1 * width + x1) * 3 + channel] * fx;

				result[channel] = top * (1.0f - fy) + bottom * fy;
			}
		}
		for (channel = 0; channel < 3; channel++)
			rgba[cell * 4 + channel] = (unsigned char)(plm_clamp(result[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
		rgba[cell * 4 + 3] = 255;
	}
	job->rgba = rgba;
	rgba = NULL;
	good = 1;
done:
	free(value);
	free(next);
	free(stock);
	free(charts);
	free(out_charts);
	free(triangle_charts);
	free(rgba);
	return good;
}

/* the page's rebaked file, where it holds this page's light: 1 (job->rgba
the two halves side by side, job->factor the file's) */
static int plm_rebaked_read(struct plm_job *job)
{
	unsigned char *bytes = NULL, *payload = NULL, *rgba = NULL;
	unsigned long size = 0, factor, deflated, inflated, side_w, side_h, row, half;
	FILE *file;
	int good = 0;

	if (!job->rebaked_path[0] || !(file = fopen(job->rebaked_path, "rb")))
		return 0;
	if (fseek(file, 0, SEEK_END) == 0)
		size = (unsigned long)ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size > PRL_HEADER && size < 512UL * 1024 * 1024 && (bytes = malloc(size)) != NULL &&
		fread(bytes, 1, size, file) == size)
	{
		factor = plm_get32(bytes + 16);
		deflated = plm_get32(bytes + 28);
		inflated = plm_get32(bytes + 32);
		side_w = job->width * factor;
		side_h = job->height * factor;
		half = side_w * side_h * 4;
		if (plm_get32(bytes) != PRL_MAGIC || plm_get32(bytes + 4) != PRL_VERSION)
			platform_log("prism lightmaps: %s: not a rebaked page of this version", job->rebaked_path);
		else if (plm_get32(bytes + 8) != job->width || plm_get32(bytes + 12) != job->height)
			platform_log("prism lightmaps: %s: baked for a page of another size", job->rebaked_path);
		else if (plm_get32(bytes + 20) != job->hash)
			platform_log("prism lightmaps: %s: baked against another page (its hash %08lx, the page's %08lx)",
				job->rebaked_path, plm_get32(bytes + 20), job->hash);
		else if (factor < 1 || factor > PRL_FACTOR_MOST || side_w > PLM_SIDE_MOST || side_h > PLM_SIDE_MOST ||
			!(plm_get32(bytes + 24) & 1) || inflated != half * 2 || PRL_HEADER + deflated != size)
			platform_log("prism lightmaps: %s: damaged", job->rebaked_path);
		else
			good = 2;
	}
	fclose(file);
	if (good == 2)
	{
		good = 0;
		/* (the CRC runs over the header's first 36 bytes and the deflated
		bytes, as one run) */
		good = plm_crc32(plm_crc32(0, bytes, 36), bytes + PRL_HEADER, deflated) == plm_get32(bytes + 36);
		if (!good)
			platform_log("prism lightmaps: %s: damaged (its CRC)", job->rebaked_path);
		if (good)
		{
			good = 0;
			factor = plm_get32(bytes + 16);
			side_w = job->width * factor;
			side_h = job->height * factor;
			half = side_w * side_h * 4;
			inflated = half * 2;
			if ((payload = malloc(inflated)) != NULL && (rgba = malloc(inflated)) != NULL &&
				z_uncompress(payload, &inflated, bytes + PRL_HEADER, deflated) == 0 && inflated == half * 2)
			{
				/* the light's rows on the left, the direction's on the right */
				for (row = 0; row < side_h; row++)
				{
					memcpy(rgba + row * side_w * 8, payload + row * side_w * 4, side_w * 4);
					memcpy(rgba + row * side_w * 8 + side_w * 4, payload + half + row * side_w * 4, side_w * 4);
				}
				job->rgba = rgba;
				rgba = NULL;
				job->factor = factor;
				job->rebaked = 1;
				good = 1;
			}
		}
	}
	free(bytes);
	free(payload);
	free(rgba);
	return good;
}

static void *plm_worker(void *context)
{
	(void)context;
	/* (the lowest priority: nice 19, as world_sdf.c's bakes) */
	__guest_syscall(140, 0, 0, 19, 0, 0, 0);
	for (;;)
	{
		struct plm_job *job, **last;
		unsigned long long started;

		pthread_mutex_lock(&plm_lock);
		while (!plm_queue)
			pthread_cond_wait(&plm_wake, &plm_lock);
		job = plm_queue;
		plm_queue = job->next;
		pthread_mutex_unlock(&plm_lock);
		started = SDL_GetTicksNS();
		job->from_cache = 0;
		/* (display.prism_lightmaps "rebaked": the rebaked page, else the
		finer copy where display.prism_lightmap_upsample asks for one) */
		if (!plm_rebaked_read(job) && job->factor)
		{
			job->from_cache = plm_cache_read(job);
			if (!job->from_cache && plm_build(job))
				plm_cache_write(job);
		}
		job->milliseconds = (unsigned long)((SDL_GetTicksNS() - started) / 1000000ULL);
		pthread_mutex_lock(&plm_lock);
		job->next = NULL;
		for (last = &plm_done; *last; last = &(*last)->next)
			;
		*last = job;
		pthread_mutex_unlock(&plm_lock);
	}
	return NULL;
}

static void plm_job_free(struct plm_job *job)
{
	if (!job)
		return;
	free(job->argb);
	free(job->uvs);
	free(job->rgba);
	free(job);
}

/* ---------- the game's thread */

static void plm_release(void)
{
	long page;

	for (page = 0; page < PLM_PAGES_MOST; page++)
	{
		if (plm.pages[page].image)
			xgpu_image_delete(plm.pages[page].image);
		free(plm.uvs[page]);
		plm.uvs[page] = NULL;
		plm.triangle_counts[page] = 0;
	}
	memset(plm.pages, 0, sizeof(plm.pages));
	plm.copied = 0;
	plm.current = -1;
	plm.current_data = 0;
	plm.map[0] = 0;
	plm.rebaked_missing = 0;
	/* (results of jobs for these pages still on their way are dropped) */
	plm.generation++;
}

static int plm_settings(void)
{
	if (!plm.read || plm.read_at != config_changes())
	{
		long factor = config_integer("display.prism_lightmap_upsample");

		plm.read = 1;
		plm.read_at = config_changes();
		char const *mode = config_string("display.prism_lightmaps");
		int rebaked = mode && !strcmp(mode, "rebaked");

		factor = factor >= 4 ? 4 : factor >= 2 ? 2 : 0;
		{
			int enabled = (factor || rebaked) && prism_lightmaps_filter() && !config_boolean("debug.null_renderer") &&
				!halo_server_mode();

			if (enabled != plm.enabled || rebaked != plm.rebaked_mode)
				platform_log("prism lightmaps: %s %s (finer copies x%ld)",
					rebaked ? "rebaked pages, where a page has one," : "the finer copies", enabled ? "on" : "off", factor);
			plm.enabled = enabled;
		}
		/* (the pages are let go whenever the kind they were made as
		changes, on or off: else turning "rebaked" off and "smooth" on
		again found the rebaked pages, two halves side by side, still ready
		and drew them as smooth pages) */
		if ((unsigned long)factor != plm.factor || rebaked != plm.rebaked_mode)
		{
			plm_release();
			plm.factor = (unsigned long)factor;
		}
		plm.rebaked_mode = rebaked;
	}
	return plm.enabled;
}

/* the BSP's pages' triangles' lightmap coordinates (sdf_pages_copy's walk) */
static void plm_copy_triangles(struct structure_bsp const *structure, long page_count)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	long rooms[PLM_PAGES_MOST], lightmap_index;

	memset(rooms, 0, sizeof(rooms));
	plm.copied = 1;
	for (lightmap_index = 0; lightmap_index < structure->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap = TAG_BLOCK_GET_ELEMENT(&structure->lightmaps, lightmap_index,
			struct structure_lightmap);
		long page = lightmap->bitmap_index, material_index;

		if (page < 0 || page >= page_count || page >= PLM_PAGES_MOST || !lightmap->materials.address)
			continue;
		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material const *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
				struct structure_material);
			byte const *vertices = material->compressed_vertex_data.address;
			long surface, corner;

			if (material->surface_count <= 0 || !vertices || material->vertices.count <= 0 ||
				material->lightmap_vertices.count != material->vertices.count ||
				material->compressed_vertex_data.size < material->vertices.count * (vertex_size + lightmap_vertex_size) ||
				material->first_surface_index < 0 ||
				material->first_surface_index + material->surface_count > structure->surfaces.count)
				continue;
			if (plm.triangle_counts[page] + material->surface_count > rooms[page])
			{
				long room = (plm.triangle_counts[page] + material->surface_count) * 2;
				float *grown = realloc(plm.uvs[page], (size_t)room * 6 * sizeof(float));

				if (!grown)
					continue;
				plm.uvs[page] = grown;
				rooms[page] = room;
			}
			for (surface = 0; surface < material->surface_count; surface++)
			{
				struct structure_surface const *triangle = TAG_BLOCK_GET_ELEMENT(&structure->surfaces,
					material->first_surface_index + surface, struct structure_surface);
				float *out = plm.uvs[page] + plm.triangle_counts[page] * 6;
				int good = 1;

				for (corner = 0; corner < 3; corner++)
				{
					long vertex = triangle->vertex_indices[corner];
					real_point2d texcoord;

					if (vertex < 0 || vertex >= material->vertices.count)
					{
						good = 0;
						break;
					}
					environment_lightmap_vertex_compressed_get_texcoord((struct environment_lightmap_vertex_compressed const *)
						(vertices + material->vertices.count * vertex_size + vertex * lightmap_vertex_size), &texcoord);
					out[corner * 2] = texcoord.x;
					out[corner * 2 + 1] = texcoord.y;
				}
				plm.triangle_counts[page] += good;
			}
		}
	}
}

/* the jobs done: their pages' images made */
static void plm_collect(void)
{
	struct plm_job *done;

	if (!plm_done)
		return;
	pthread_mutex_lock(&plm_lock);
	done = plm_done;
	plm_done = NULL;
	pthread_mutex_unlock(&plm_lock);
	while (done)
	{
		struct plm_job *job = done;
		struct plm_page *page = job->page >= 0 && job->page < PLM_PAGES_MOST ? &plm.pages[job->page] : NULL;

		done = job->next;
		if (job->generation == plm.generation && page && page->state == _plm_page_queued)
		{
			unsigned long width = job->width * job->factor * (job->rebaked ? 2 : 1), height = job->height * job->factor, side,
				levels = 1;

			for (side = width > height ? width : height; side > 1; side >>= 1)
				levels++;
			page->image = job->rgba ? xgpu_image_create(job->rgba, width, height, levels, PLM_IMAGE_RGBA, 1) : 0;
			if (page->image)
			{
				/* (the image owns the texels now) */
				job->rgba = NULL;
				page->levels = levels;
				page->rebaked = job->rebaked;
				page->state = _plm_page_ready;
				plm.ready_count++;
				if (job->from_cache)
					plm.cached_count++;
				else
					plm.built_count++;
				platform_log("prism lightmaps: %s bsp %d page %ld: %lux%lu to %lux%lu, %ld triangles, %s in %lu ms",
					plm.map, (int)plm.bsp, job->page, job->width, job->height, width, height, job->triangle_count,
					job->rebaked ? "rebaked (light and direction)" : job->from_cache ? "from the cache" : "built and cached",
					job->milliseconds);
				/* (the setting asks for rebaked pages and this one has no
				file, or one refused above: no download has any, and the
				line above alone read as a rebaked page drawn) */
				if (job->rebaked_path[0] && !job->rebaked && !plm.rebaked_missing++)
					platform_log("prism lightmaps: display.prism_lightmaps \"rebaked\": no rebaked page of %s bsp %d "
						"page %ld (%s/%s): drawn as smooth, as every page of it without one (said of the first)",
						plm.map, (int)plm.bsp, job->page, platform_data_root(), job->rebaked_path);
			}
			else
			{
				page->state = _plm_page_failed;
				platform_log("prism lightmaps: %s bsp %d page %ld: %s; drawn as it is", plm.map, (int)plm.bsp, job->page,
					job->rgba ? "not built (memory)" : "no rebaked page and no finer copy");
			}
		}
		plm_job_free(job);
	}
}

/* the page's copy asked of the thread */
static void plm_queue_page(void const *texture, long page_index, unsigned long data)
{
	struct plm_page *page = &plm.pages[page_index];
	struct plm_job *job, **last;
	char name[64];
	char const *map = plm.map, *part;
	unsigned long width = 0, height = 0, cell, hash = 2166136261UL;
	size_t index;

	page->state = _plm_page_failed;
	job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->argb = xgpu_texture_565_argb(texture, &width, &height);
	if (!job->argb || !width || !height || width * plm.factor > PLM_SIDE_MOST || height * plm.factor > PLM_SIDE_MOST ||
		(!plm.triangle_counts[page_index] && !plm.rebaked_mode) || (!plm.factor && !plm.rebaked_mode))
	{
		platform_log("prism lightmaps: %s bsp %d page %ld drawn as it is (%s, %lux%lu, %ld triangles)", plm.map,
			(int)plm.bsp, page_index, job->argb ? "its size" : "not an R5G6B5 texture", width, height,
			plm.triangle_counts[page_index]);
		plm_job_free(job);
		return;
	}
	for (cell = 0; cell < width * height; cell++)
	{
		unsigned long argb = job->argb[cell];
		int byte;

		for (byte = 0; byte < 4; byte++)
			hash = ((hash ^ ((argb >> (byte * 8)) & 0xff)) * 16777619UL) & 0xffffffffUL;
	}
	job->uvs = malloc(((size_t)plm.triangle_counts[page_index] + 1) * 6 * sizeof(float));
	if (!job->uvs)
	{
		plm_job_free(job);
		return;
	}
	memcpy(job->uvs, plm.uvs[page_index], (size_t)plm.triangle_counts[page_index] * 6 * sizeof(float));
	job->triangle_count = plm.triangle_counts[page_index];
	job->generation = plm.generation;
	job->page = page_index;
	job->width = width;
	job->height = height;
	job->factor = plm.factor;
	job->hash = hash;
	job->checksum = plm.checksum;
	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
			map = part + 1;
	}
	for (index = 0; map[index] && index < sizeof(name) - 1; index++)
	{
		char letter = map[index];

		name[index] = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
			(letter >= '0' && letter <= '9') || letter == '-' ? letter : '_';
	}
	name[index] = 0;
	/* (from the data root, as the game's fopen, remove and rename find
	files: port/linux/include/stdio.h; world_sdf.c's sdf_path) */
	snprintf(job->path, sizeof(job->path), "prism_lightmaps/%s_%d_%ld_x%lu.plm", name, (int)plm.bsp, page_index,
		plm.factor);
	if (plm.rebaked_mode)
		snprintf(job->rebaked_path, sizeof(job->rebaked_path), "prism_lightmaps/rebaked/%s_%d_%ld.prl", name, (int)plm.bsp,
			page_index);
	/* (no finer copy to fall back on without display.prism_lightmap_upsample,
	or without the page's triangles) */
	if (!plm.triangle_counts[page_index])
		job->factor = 0;
	page->state = _plm_page_queued;
	page->data = data;
	pthread_mutex_lock(&plm_lock);
	if (!plm_thread_running)
	{
		pthread_t thread;
		pthread_attr_t attributes;

		pthread_attr_init(&attributes);
		pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
		plm_thread_running = pthread_create(&thread, &attributes, plm_worker, NULL) == 0;
		pthread_attr_destroy(&attributes);
	}
	if (!plm_thread_running)
	{
		pthread_mutex_unlock(&plm_lock);
		page->state = _plm_page_failed;
		plm_job_free(job);
		return;
	}
	for (last = &plm_queue; *last; last = &(*last)->next)
		;
	*last = job;
	pthread_cond_signal(&plm_wake);
	pthread_mutex_unlock(&plm_lock);
}

void prism_lightmaps_begin(void const *bitmap_pointer)
{
	struct bitmap_data const *lightmap_bitmap = (struct bitmap_data const *)bitmap_pointer;
	void const *texture;
	struct structure_bsp const *structure;
	struct bitmap_group const *group;
	char const *map;
	short bsp;
	long page;

	plm.current = -1;
	plm.current_data = 0;
	if (!plm_settings() || !lightmap_bitmap)
		return;
	/* (the page's texture, as the lightmap pass's begin bound it to stage
	2: the texture cache's) */
	texture = halo_texture_bound(2);
	if (!texture)
		return;
	plm_collect();
	map = cache_file_loaded_map_name();
	bsp = global_structure_bsp_index_get();
	if (!map || !map[0] || !(structure = global_structure_bsp_get()) || structure->lightmap_group.index == NONE ||
		!(group = bitmap_group_get(structure->lightmap_group.index)) || !group->bitmaps.address)
		return;
	if (strcmp(map, plm.map) || bsp != plm.bsp || plm.checksum != cache_files_get_checksum())
	{
		unsigned long factor = plm.factor;

		plm_release();
		plm.factor = factor;
		snprintf(plm.map, sizeof(plm.map), "%s", map);
		plm.bsp = bsp;
		plm.checksum = cache_files_get_checksum();
	}
	page = (long)(lightmap_bitmap - (struct bitmap_data const *)group->bitmaps.address);
	if (page < 0 || page >= group->bitmaps.count || page >= PLM_PAGES_MOST)
		return;
	if (!plm.copied)
		plm_copy_triangles(structure, group->bitmaps.count);
	plm.current = page;
	plm.current_data = ((unsigned long const *)texture)[1];
	if (plm.pages[page].state == _plm_page_none)
		plm_queue_page(texture, page, plm.current_data);
}

unsigned int prism_lightmaps_replacement(unsigned long data, unsigned long *levels)
{
	struct plm_page const *page;

	if (plm.current < 0 || !plm.enabled || data != plm.current_data)
		return 0;
	page = &plm.pages[plm.current];
	/* (a rebaked page is drawn only as one: never read as a smooth page) */
	if (page->state != _plm_page_ready || !page->image || (page->rebaked && !plm.rebaked_mode))
		return 0;
	*levels = page->levels;
	return page->image;
}

/* (d3d8_device.c, draw by draw) whether the draw is the lightmap pass of a
page drawn from its rebaked copy: the page last begun is ready rebaked and
its texture is the one bound to stage 2 */
int prism_lightmaps_rebaked_drawing(void)
{
	void const *texture;

	if (plm.current < 0 || !plm.enabled || !plm.rebaked_mode || plm.pages[plm.current].state != _plm_page_ready ||
		!plm.pages[plm.current].rebaked || !plm.pages[plm.current].image)
		return 0;
	texture = halo_texture_bound(2);
	return texture && ((unsigned long const *)texture)[1] == plm.current_data;
}

void prism_lightmaps_bsp_switched(void)
{
	/* (prism_lightmaps_begin lets the old BSP's pages go as the new one's
	are first drawn) */
	plm.current = -1;
	plm.current_data = 0;
}

void prism_lightmaps_map_unloaded(void)
{
	unsigned long factor = plm.factor;

	plm_collect();
	plm_release();
	plm.factor = factor;
}
