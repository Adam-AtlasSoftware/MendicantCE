/*
MOD_LIGHTMAP_CHECK.C (test)

The real mod_bitmaps.c's and mod_lightmaps.c's decision on a mod's lightmap
(OpenH1 issue 1172): it waits, the map's drawn, until the structure BSP it
lights is read, then is taken or left out once, by its layout file's
fingerprint (the BSP's lightmap layout) or, with none, by the map's pages'
sizes and pictures. A fake map of two BSPs, each with its own lightmap of
Blood Gulch's Xbox pages (made-up pictures of charts on a dark ground), and
mods' lightmaps for them: their fingerprint the BSP's or another's, or none
and the map's pictures at 1x, 2x (bilinear, 32-bit), re-encoded as DXT1, or
another layout's of the same sizes, or Halo PC's sizes.
test_mod_lightmap_check.py takes the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TAG_BLOCK_GET_ELEMENT(b, i, t) (&((t *)(b)->address)[i])
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
typedef unsigned short word;
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;
typedef struct { real x, y; } real_point2d;
union point2d { short n[2]; };

/* the tags' and bitmaps' structures (tag_groups.h, bitmap_group.h) */
#include "structures.inc"

/* ---------- the fake world */

/* the structure BSP's (structure_bsp_definitions.h, rasterizer_geometry.h): only what the code reads */
#define NUMBER_OF_VERTICES_PER_TRIANGLE 3
#define MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO 16
enum { _rasterizer_vertex_type_environment_compressed, _rasterizer_vertex_type_environment_lightmap_compressed };
#define rasterizer_geometry_get_vertex_size(type) ((type) == _rasterizer_vertex_type_environment_compressed ? 32 : 8)

struct vertex_buffer { short type; word pad; long count; long offset; void *base_address; void *hardware_format; };
struct structure_surface { word vertex_indices[NUMBER_OF_VERTICES_PER_TRIANGLE]; };
struct structure_material
{
	long first_surface_index;
	long surface_count;
	struct vertex_buffer vertices;
	struct vertex_buffer lightmap_vertices;
	struct tag_data compressed_vertex_data;
};
struct structure_lightmap { short bitmap_index; struct tag_block materials; };
struct structure_bsp { struct tag_reference lightmap_group; struct tag_block surfaces; struct tag_block lightmaps; };

#define TAGS 4
#define PAGES 15
#define BSPS 2
#define FILES 8
#define _error_silent 0

static struct bitmap_group map_groups[TAGS];
#define bitmap_group_get(tag_index) (&map_groups[(tag_index) & 0xFFFF])

static char log_line[1024];
static long flushes;
static void error(int priority, char const *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vsnprintf(log_line, sizeof(log_line), format, arguments);
	va_end(arguments);
	fprintf(stderr, "%s\n", log_line);
}
static void console_warning(char const *format, ...) { (void)format; }
static char *tag_get_name(long tag_index) { static char name[32]; snprintf(name, sizeof(name), "levels\\test\\l%ld", tag_index); return name; }
static void texture_cache_flush(void) { flushes++; }
static void xgpu_backend_consume(void) { }
static void mods_memory_give(unsigned long size) { (void)size; }
static boolean custom_edition_cache_tags_loaded(void) { return FALSE; }
static boolean custom_edition_loaded_reordered_bitmaps_find(void) { return FALSE; }
static void SwitchToThread(void) { }

/* the map's file (its pages' pixels), and the mods' files */
static byte map_file[1 << 22];
static long map_file_used;
static byte *mod_files[FILES];
static unsigned long mod_file_sizes[FILES];
static void cache_file_read(long tag_index, long offset, long size, void *buffer, boolean *done, boolean blocking)
{
	(void)tag_index;
	(void)blocking;
	memcpy(buffer, map_file + offset, (size_t)size);
	*done = TRUE;
}
static int port_mods_file_read(long file, unsigned long offset, void *buffer, unsigned long size)
{
	if (file < 0 || file >= FILES || !mod_files[file] || offset + size > mod_file_sizes[file])
		return 0;
	memcpy(buffer, mod_files[file] + offset, size);
	return 1;
}
static char const *port_mods_file_mod(long file) { (void)file; return "test_mod"; }

#include "under_test.inc"

/* ---------- pictures */

/* Blood Gulch's lightmap pages: the Xbox map's, and Halo PC's (5 of them other sizes) */
static short const xbox_pages[PAGES][2] = {{256, 128}, {128, 256}, {64, 32}, {64, 64}, {128, 128}, {32, 32}, {64, 32},
	{8, 8}, {128, 128}, {32, 16}, {16, 32}, {32, 16}, {32, 16}, {128, 64}, {64, 128}};
static short const pc_pages[PAGES][2] = {{256, 128}, {128, 256}, {32, 32}, {64, 64}, {32, 32}, {32, 32}, {64, 32},
	{8, 8}, {128, 128}, {32, 16}, {16, 32}, {16, 16}, {32, 16}, {64, 64}, {64, 128}};

static unsigned long random_state;
static long random_below(long limit)
{
	random_state = random_state * 1103515245UL + 12345UL;
	return (long)((random_state >> 8) % (unsigned long)limit);
}

/* a lightmap page's picture laid out by `layout`: lit charts (each a gradient) on a dark ground, 0 to 255 */
static void picture_make(float *picture, long width, long height, unsigned long layout)
{
	long chart, x, y;

	random_state = layout * 7919UL + (unsigned long)(width * 31 + height);
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			picture[y * width + x] = 12.f;
	for (chart = 0; chart < 6; chart++)
	{
		long x0 = random_below(width), y0 = random_below(height);
		long w = 1 + random_below(MAX(width / 2, 1)), h = 1 + random_below(MAX(height / 2, 1));
		float base = 60.f + (float)random_below(150);
		float slope_x = (float)(random_below(60) - 30) / (float)width;
		float slope_y = (float)(random_below(60) - 30) / (float)height;

		for (y = y0; y < MIN(y0 + h, height); y++)
			for (x = x0; x < MIN(x0 + w, width); x++)
				picture[y * width + x] = MIN(MAX(base + slope_x * (float)(x - x0) * 4.f + slope_y * (float)(y - y0) * 4.f,
					0.f), 255.f);
	}
}

static unsigned short grey565(float value)
{
	long level = (long)(value + 0.5f);

	return (unsigned short)(((level >> 3) << 11) | ((level >> 2) << 5) | (level >> 3));
}

/* (the test's own swizzle: Morton order, the longer side's bits last) */
static unsigned long swizzled_at(long x, long y, long width, long height)
{
	unsigned long at = 0, bit = 0;
	long shift;

	for (shift = 0; (1L << shift) < width || (1L << shift) < height; shift++)
	{
		if ((1L << shift) < width)
			at |= (unsigned long)((x >> shift) & 1) << bit++;
		if ((1L << shift) < height)
			at |= (unsigned long)((y >> shift) & 1) << bit++;
	}
	return at;
}

enum encoding { _encoding_565, _encoding_565_swizzled, _encoding_8888, _encoding_dxt1 };

/* the picture's pixels in an encoding, at `bytes`: their size */
static unsigned long pixels_write(float const *picture, long width, long height, enum encoding encoding, byte *bytes)
{
	long x, y;

	if (encoding == _encoding_dxt1)
	{
		long blocks_wide = (width + 3) / 4;

		for (y = 0; y < height; y += 4)
		{
			for (x = 0; x < width; x += 4)
			{
				byte *block = bytes + ((y / 4) * blocks_wide + x / 4) * 8;
				float low = 255.f, high = 0.f, colors[4];
				unsigned long bits = 0;
				unsigned short first, second;
				long i, j;

				for (j = 0; j < 4 && y + j < height; j++)
					for (i = 0; i < 4 && x + i < width; i++)
					{
						low = MIN(low, picture[(y + j) * width + x + i]);
						high = MAX(high, picture[(y + j) * width + x + i]);
					}
				first = grey565(high);
				second = grey565(low);
				if (first <= second)
					first = second, second = 0;
				colors[0] = high, colors[1] = second ? low : 0.f;
				colors[2] = (2.f * colors[0] + colors[1]) / 3.f, colors[3] = (colors[0] + 2.f * colors[1]) / 3.f;
				for (j = 0; j < 4; j++)
					for (i = 0; i < 4; i++)
					{
						float value = picture[MIN(y + j, height - 1) * width + MIN(x + i, width - 1)];
						long best = 0, k;

						for (k = 1; k < 4; k++)
							if (fabsf(colors[k] - value) < fabsf(colors[best] - value))
								best = k;
						bits |= (unsigned long)best << (2 * (j * 4 + i));
					}
				block[0] = (byte)first, block[1] = (byte)(first >> 8);
				block[2] = (byte)second, block[3] = (byte)(second >> 8);
				block[4] = (byte)bits, block[5] = (byte)(bits >> 8), block[6] = (byte)(bits >> 16), block[7] = (byte)(bits >> 24);
			}
		}
		return (unsigned long)(blocks_wide * ((height + 3) / 4) * 8);
	}
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			float value = picture[y * width + x];
			unsigned long at = encoding == _encoding_565_swizzled ? swizzled_at(x, y, width, height) :
				(unsigned long)(y * width + x);

			if (encoding == _encoding_8888)
			{
				byte level = (byte)(value + 0.5f);

				bytes[at * 4] = bytes[at * 4 + 1] = bytes[at * 4 + 2] = level;
				bytes[at * 4 + 3] = 255;
			}
			else
			{
				unsigned short texel = grey565(value);

				bytes[at * 2] = (byte)texel;
				bytes[at * 2 + 1] = (byte)(texel >> 8);
			}
		}
	}
	return (unsigned long)(width * height * (encoding == _encoding_8888 ? 4 : 2));
}

/* `picture` (width x height) twice the size, bilinear, into `larger` */
static void picture_upscale(float const *picture, long width, long height, float *larger)
{
	long x, y;

	for (y = 0; y < 2 * height; y++)
	{
		for (x = 0; x < 2 * width; x++)
		{
			float sx = MIN(MAX(((float)x + 0.5f) / 2.f - 0.5f, 0.f), (float)(width - 1));
			float sy = MIN(MAX(((float)y + 0.5f) / 2.f - 0.5f, 0.f), (float)(height - 1));
			long x0 = (long)sx, y0 = (long)sy, x1 = MIN(x0 + 1, width - 1), y1 = MIN(y0 + 1, height - 1);
			float ax = sx - (float)x0, ay = sy - (float)y0;

			larger[y * 2 * width + x] =
				(picture[y0 * width + x0] * (1.f - ax) + picture[y0 * width + x1] * ax) * (1.f - ay) +
				(picture[y1 * width + x0] * (1.f - ax) + picture[y1 * width + x1] * ax) * ay;
		}
	}
}

/* ---------- the map */

static struct bitmap_data map_bitmaps[TAGS][PAGES];
static struct mod_bitmap_override overrides[TAGS];

/* the map: tag `tag_index` a lightmap of Blood Gulch's Xbox pages, its pictures of `layout`, R5G6B5 swizzled */
static void map_lightmap_make(long tag_index, unsigned long layout)
{
	struct bitmap_group *group = &map_groups[tag_index];
	long page;

	memset(group, 0, sizeof(*group));
	group->usage = _bitmap_usage_light_map;
	group->bitmaps.count = PAGES;
	group->bitmaps.address = map_bitmaps[tag_index];
	for (page = 0; page < PAGES; page++)
	{
		struct bitmap_data *bitmap = &map_bitmaps[tag_index][page];
		long width = xbox_pages[page][0], height = xbox_pages[page][1];
		float *picture = malloc((size_t)(width * height) * sizeof(float));

		memset(bitmap, 0, sizeof(*bitmap));
		bitmap->width = (short)width, bitmap->height = (short)height, bitmap->depth = 1;
		bitmap->format = 6; /* R5G6B5 */
		bitmap->flags = FLAG(_bitmap_swizzled_bit);
		bitmap->tag_index = tag_index;
		bitmap->cache_block_index = NONE;
		bitmap->pixels_offset = map_file_used;
		picture_make(picture, width, height, layout);
		map_file_used += (long)pixels_write(picture, width, height, _encoding_565_swizzled, map_file + map_file_used);
		map_file_used = (map_file_used + 0x1FF) & ~0x1FFL;
		free(picture);
	}
}

static void map_make(void)
{
	memset(&mod_bitmaps_globals, 0, sizeof(mod_bitmaps_globals));
	memset(overrides, 0, sizeof(overrides));
	mod_bitmaps_globals.overrides = overrides;
	mod_bitmaps_globals.override_count = TAGS;
	map_file_used = 0;
	map_lightmap_make(0, 1);
	map_lightmap_make(1, 2);
}

/* ---------- BSPs */

#define MATERIAL_SURFACES 4
#define MATERIAL_VERTICES 6

struct bsp_world
{
	struct structure_bsp bsp;
	struct structure_lightmap lightmaps[PAGES + 1];
	struct structure_material materials[PAGES + 1];
	struct structure_surface surfaces[(PAGES + 1) * MATERIAL_SURFACES];
	byte vertices[PAGES + 1][MATERIAL_VERTICES * (32 + 8)];
};
static struct bsp_world bsps[BSPS];

/* BSP `index`: lighted by tag `lightmap_tag`, its lightmaps of pages 0 to `pages`-1 (then one of none), each of one
material of 4 surfaces, whose corners' coordinates `layout` gives */
static struct structure_bsp *bsp_make(long index, long lightmap_tag, long pages, unsigned long layout)
{
	struct bsp_world *world = &bsps[index];
	long lightmap, surface, vertex;

	memset(world, 0, sizeof(*world));
	world->bsp.lightmap_group.index = lightmap_tag;
	world->bsp.lightmaps.count = pages + 1;
	world->bsp.lightmaps.address = world->lightmaps;
	world->bsp.surfaces.count = (pages + 1) * MATERIAL_SURFACES;
	world->bsp.surfaces.address = world->surfaces;
	for (lightmap = 0; lightmap <= pages; lightmap++)
	{
		struct structure_material *material = &world->materials[lightmap];

		world->lightmaps[lightmap].bitmap_index = (short)(lightmap < pages ? lightmap : -1);
		world->lightmaps[lightmap].materials.count = 1;
		world->lightmaps[lightmap].materials.address = material;
		material->first_surface_index = lightmap * MATERIAL_SURFACES;
		material->surface_count = MATERIAL_SURFACES;
		material->vertices.count = MATERIAL_VERTICES;
		material->lightmap_vertices.count = MATERIAL_VERTICES;
		material->compressed_vertex_data.size = sizeof(world->vertices[lightmap]);
		material->compressed_vertex_data.address = world->vertices[lightmap];
		for (surface = 0; surface < MATERIAL_SURFACES; surface++)
		{
			world->surfaces[lightmap * MATERIAL_SURFACES + surface].vertex_indices[0] = (word)surface;
			world->surfaces[lightmap * MATERIAL_SURFACES + surface].vertex_indices[1] = (word)(surface + 1);
			world->surfaces[lightmap * MATERIAL_SURFACES + surface].vertex_indices[2] = (word)(surface + 2);
		}
		for (vertex = 0; vertex < MATERIAL_VERTICES; vertex++)
		{
			short u = (short)((vertex * 4000 + lightmap * 300 + (long)layout * 17) % 32000);
			short v = (short)((vertex * 2500 + lightmap * 700 + (long)layout * 29) % 32000 - 16000);
			byte *lightmap_vertex = world->vertices[lightmap] + MATERIAL_VERTICES * 32 + vertex * 8;

			memcpy(lightmap_vertex + 4, &u, sizeof(u));
			memcpy(lightmap_vertex + 6, &v, sizeof(v));
		}
	}
	return &world->bsp;
}

/* ---------- mods' lightmaps */

enum pictures { _pictures_map, _pictures_double, _pictures_dxt1, _pictures_other_layout, _pictures_pc_sizes };

/* a mod's lightmap for tag `tag_index`, its pictures as `pictures` says (of the map's `layout`), its layout file
`fingerprint` (NULL: none), waiting in the mods' bitmaps */
static struct mod_bitmap *mod_lightmap_make(long tag_index, long file, unsigned long layout, enum pictures pictures,
	char const *fingerprint)
{
	struct mod_bitmap *made = calloc(1, sizeof(*made));
	unsigned long used = 0;
	long page;

	mod_files[file] = malloc(1 << 22);
	made->tag_index = tag_index;
	made->file = file;
	made->bitmaps = calloc(PAGES, sizeof(struct bitmap_data));
	made->group = map_groups[tag_index];
	made->group.bitmaps.address = made->bitmaps;
	for (page = 0; page < PAGES; page++)
	{
		struct bitmap_data *bitmap = &made->bitmaps[page];
		long scale = pictures == _pictures_double ? 2 : 1;
		long width = (pictures == _pictures_pc_sizes ? pc_pages : xbox_pages)[page][0];
		long height = (pictures == _pictures_pc_sizes ? pc_pages : xbox_pages)[page][1];
		float *picture = malloc((size_t)(width * height) * sizeof(float));
		float *larger = malloc((size_t)(4 * width * height) * sizeof(float));
		enum encoding encoding = pictures == _pictures_double ? _encoding_8888 :
			pictures == _pictures_dxt1 ? _encoding_dxt1 : _encoding_565;

		picture_make(picture, width, height, pictures == _pictures_other_layout ? layout + 100 : layout);
		if (scale == 2)
			picture_upscale(picture, width, height, larger);
		bitmap->width = (short)(width * scale), bitmap->height = (short)(height * scale), bitmap->depth = 1;
		bitmap->format = (short)(encoding == _encoding_8888 ? 11 : encoding == _encoding_dxt1 ? 14 : 6);
		bitmap->tag_index = tag_index;
		bitmap->cache_block_index = NONE;
		bitmap->pixels_offset = (long)used;
		used += pixels_write(scale == 2 ? larger : picture, width * scale, height * scale, encoding, mod_files[file] + used);
		free(picture);
		free(larger);
	}
	mod_file_sizes[file] = used;
	made->lightmap = TRUE;
	made->waiting = TRUE;
	made->layout_file = NONE;
	if (fingerprint)
	{
		long layout_file = file + FILES / 2;

		mod_files[layout_file] = malloc(256);
		mod_file_sizes[layout_file] = (unsigned long)snprintf((char *)mod_files[layout_file], 256,
			"# the lightmap layout of a test map's BSP\n%s\n", fingerprint);
		made->layout_file = layout_file;
		made->layout_size = mod_file_sizes[layout_file];
	}
	made->next = mod_bitmaps_globals.bitmaps;
	mod_bitmaps_globals.bitmaps = made;
	return made;
}

/* whether the map's bitmap tag draws the mod's lightmap `made` (else the map's own) */
static boolean drawn(long tag_index, struct mod_bitmap const *made)
{
	return map_groups[tag_index].bitmaps.address == made->bitmaps;
}

static char const *fingerprint_of(struct structure_bsp const *bsp)
{
	static char text[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE];

	mod_lightmap_fingerprint_text(mod_lightmap_fingerprint(bsp), text);
	return text;
}

/* a mod's lightmap for BSP 0 (its fingerprint `fingerprint`, or none), decided as BSP 0 is read: whether taken */
static boolean decided(enum pictures pictures, char const *fingerprint)
{
	struct structure_bsp *bsp;
	struct mod_bitmap *made;
	boolean taken;

	map_make();
	bsp = bsp_make(0, 0, PAGES, 1);
	made = mod_lightmap_make(0, 0, 1, pictures, fingerprint);
	mod_bitmaps_structure_bsp_read(0, bsp);
	taken = drawn(0, made);
	CHECK(taken == (strstr(log_line, ": accepted") != NULL), "the log says what was done: %s", log_line);
	return taken;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("fingerprint-matches") {
		map_make();
		CHECK(decided(_pictures_other_layout, fingerprint_of(bsp_make(1, 0, PAGES, 1))),
			"a lightmap whose fingerprint is the BSP's is taken, whatever its pictures: %s", log_line);
		CHECK(strstr(log_line, "fingerprint check"), "by the fingerprint: %s", log_line);
	}
	CASE("fingerprint-differs") {
		map_make();
		CHECK(!decided(_pictures_map, fingerprint_of(bsp_make(1, 0, PAGES, 2))),
			"a lightmap made for another layout is left out, its pictures the map's though: %s", log_line);
		CHECK(strstr(log_line, "made for the layout lm1-") && strstr(log_line, "fingerprint check"), "%s", log_line);
		CHECK(!decided(_pictures_map, "lm1-not a fingerprint"), "a layout file of no fingerprint: %s", log_line);
		CHECK(strstr(log_line, "holds no lightmap fingerprint"), "%s", log_line);
	}
	CASE("fingerprint-stable") {
		/* the same layout, the same fingerprint; a corner moved, another */
		char first[MOD_LIGHTMAP_FINGERPRINT_TEXT_SIZE];
		short u = 1;

		strcpy(first, fingerprint_of(bsp_make(0, 0, PAGES, 1)));
		CHECK(!strcmp(first, fingerprint_of(bsp_make(1, 0, PAGES, 1))), "the same layout: %s", first);
		memcpy(bsps[1].vertices[3] + MATERIAL_VERTICES * 32 + 2 * 8 + 4, &u, sizeof(u));
		CHECK(strcmp(first, fingerprint_of(&bsps[1].bsp)), "one corner moved: %s", first);
		CHECK(!strcmp(first, fingerprint_of(bsp_make(1, 0, PAGES, 1))) &&
			strcmp(first, fingerprint_of(bsp_make(1, 0, PAGES - 1, 1))), "a page fewer: %s", first);
	}
	CASE("fingerprint-print") {
		/* (test_mod_lightmap_check.py compares it with tools/lightmap_fingerprint.py's) */
		struct structure_bsp *bsp = bsp_make(0, 0, 3, 5);

		bsps[0].materials[1].lightmap_vertices.count = 0; /* (its vertices unread: none) */
		bsps[0].surfaces[2 * MATERIAL_SURFACES + 1].vertex_indices[2] = 40; /* (past its vertices: none) */
		printf("%s\n", fingerprint_of(bsp));
	}
	CASE("pictures-same-layout") {
		CHECK(decided(_pictures_map, NULL), "the map's pictures at 1x: %s", log_line);
		CHECK(strstr(log_line, "size and picture check"), "by the pictures: %s", log_line);
		CHECK(decided(_pictures_double, NULL), "at 2x, bilinear, 32-bit: %s", log_line);
		CHECK(decided(_pictures_dxt1, NULL), "re-encoded as DXT1: %s", log_line);
	}
	CASE("pictures-pc-sizes") {
		CHECK(!decided(_pictures_pc_sizes, NULL), "Halo PC's page sizes: %s", log_line);
		CHECK(strstr(log_line, "page 2 is 32x32, not the map's 64x32"), "by the size: %s", log_line);
	}
	CASE("pictures-other-layout") {
		CHECK(!decided(_pictures_other_layout, NULL), "another layout's pictures at the map's sizes: %s", log_line);
		CHECK(strstr(log_line, "page 0's picture is not the map's"), "by the picture, naming the page: %s", log_line);
	}
	CASE("per-bsp") {
		/* two BSPs, each its own lightmap: the map's drawn until a lightmap's BSP is read */
		struct structure_bsp *first, *second;
		struct mod_bitmap *made_first, *made_second;

		map_make();
		first = bsp_make(0, 0, PAGES, 1);
		second = bsp_make(1, 1, PAGES, 2);
		made_first = mod_lightmap_make(0, 0, 1, _pictures_map, fingerprint_of(first));
		made_second = mod_lightmap_make(1, 1, 2, _pictures_map, fingerprint_of(second));
		CHECK(!drawn(0, made_first) && !drawn(1, made_second) && mod_bitmaps_waiting_count() == 2,
			"the map's lightmaps until a BSP is read");
		CHECK(mod_bitmaps_structure_bsp_read(0, first) == 1, "one played: %s", log_line);
		CHECK(drawn(0, made_first) && !drawn(1, made_second) && mod_bitmaps_waiting_count() == 1,
			"BSP 0's taken as it is read, BSP 1's waits: %s", log_line);
		CHECK(mod_bitmaps_structure_bsp_read(1, second) == 2 && drawn(1, made_second) && !mod_bitmaps_waiting_count(),
			"BSP 1's taken as it is read: %s", log_line);
		CHECK(strstr(log_line, "BSP 1 ("), "the log names the BSP: %s", log_line);
		/* (read again, at a switch back: nothing changes) */
		CHECK(mod_bitmaps_structure_bsp_read(0, first) == 2 && drawn(0, made_first), "BSP 0 again");
	}
	CASE("held-textures") {
		/* the map's page held by the texture cache when the mod's is taken: let go of first */
		struct structure_bsp *bsp;

		map_make();
		bsp = bsp_make(0, 0, PAGES, 1);
		mod_lightmap_make(0, 0, 1, _pictures_map, NULL);
		flushes = 0;
		map_bitmaps[0][4].cache_block_index = 3;
		mod_bitmaps_structure_bsp_read(0, bsp);
		CHECK(flushes == 1, "the texture cache let go: %ld", flushes);
	}
	return 0;
}
