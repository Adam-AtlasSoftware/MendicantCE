/*
MOD_LIGHTMAP_LAYOUT.C (test)

The real mod_bitmaps.c's size check of a mod's lightmap with no layout file
(lightmap_layout_differs, OpenH1 issue 1172): a lightmap is tied page for
page to the BSP's lightmaps, so a mod's is taken only with the map's pages,
each the map's at one scale. Blood Gulch's 15 pages, the Xbox map's, are
taken at 1x and 2x; Halo PC's, 5 of them at other sizes, are left out,
naming the page; what the map's other tags ask of any bitmap
(map_requirements_differ) takes other sizes, as before.
test_mod_lightmap_layout.py takes the code (under_test.inc).
*/

#include "harness.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TAG_BLOCK_GET_ELEMENT(b, i, t) (&((t *)(b)->address)[i])
typedef unsigned short word;
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;
typedef struct { real x, y; } real_point2d;
union point2d { short n[2]; };

#include "under_test.inc"

static char const *mod_made(short const (*pages)[2], long count, short scale);

#define PAGES 15
/* levels\test\bloodgulch\bloodgulch.bitmap's pages: the Xbox map's, and Halo PC's (5 of them other sizes) */
static short const xbox_pages[PAGES][2] = {{256, 128}, {128, 256}, {64, 32}, {64, 64}, {128, 128}, {32, 32}, {64, 32},
	{8, 8}, {128, 128}, {32, 16}, {16, 32}, {32, 16}, {32, 16}, {128, 64}, {64, 128}};
static short const pc_pages[PAGES][2] = {{256, 128}, {128, 256}, {32, 32}, {64, 64}, {32, 32}, {32, 32}, {64, 32},
	{8, 8}, {128, 128}, {32, 16}, {16, 32}, {16, 16}, {32, 16}, {64, 64}, {64, 128}};

static struct bitmap_data map_bitmaps[PAGES];
static struct bitmap_group map_group;
static struct bitmap_data mod_bitmaps[PAGES + 1];
static struct mod_bitmap mod;

static void map_made(short usage, long count)
{
	long index;

	memset(&map_group, 0, sizeof(map_group));
	memset(map_bitmaps, 0, sizeof(map_bitmaps));
	map_group.usage = usage;
	map_group.bitmaps.count = count;
	map_group.bitmaps.address = map_bitmaps;
	for (index = 0; index < count; index++)
	{
		map_bitmaps[index].width = xbox_pages[index][0];
		map_bitmaps[index].height = xbox_pages[index][1];
	}
}

/* the mod's bitmap of `count` bitmaps, the pages `pages` scaled by `scale`, checked */
static char const *mod_checked(short const (*pages)[2], long count, short scale)
{
	char const *reason = mod_made(pages, count, scale);

	return reason ? reason : lightmap_layout_differs(&map_group, &mod, (unsigned long)count);
}

/* the same, checked only for what the map's other tags ask of it */
static char const *mod_made(short const (*pages)[2], long count, short scale)
{
	long index;

	memset(&mod, 0, sizeof(mod));
	memset(mod_bitmaps, 0, sizeof(mod_bitmaps));
	mod.bitmaps = mod_bitmaps;
	for (index = 0; index < count; index++)
	{
		mod_bitmaps[index].width = (short)(pages[index % PAGES][0] * scale);
		mod_bitmaps[index].height = (short)(pages[index % PAGES][1] * scale);
	}
	return map_requirements_differ(&map_group, &mod, (unsigned long)count, 0);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	char const *reason;

	CASE("xbox-layout") {
		map_made(_bitmap_usage_light_map, PAGES);
		reason = mod_checked(xbox_pages, PAGES, 1);
		CHECK(!reason, "the map's own lightmap, at 1x: %s", reason);
		reason = mod_checked(xbox_pages, PAGES, 2);
		CHECK(!reason, "the map's lightmap redrawn at 2x: %s", reason);
		reason = mod_checked(xbox_pages, PAGES, 4);
		CHECK(!reason, "at 4x: %s", reason);
	}
	CASE("pc-layout") {
		map_made(_bitmap_usage_light_map, PAGES);
		reason = mod_checked(pc_pages, PAGES, 1);
		CHECK(reason && strstr(reason, "page 2 is 32x32, not the map's 64x32"), "Halo PC's lightmap: %s",
			reason ? reason : "taken");
		reason = mod_checked(pc_pages, PAGES, 2);
		CHECK(reason && strstr(reason, "page 2 is 64x64"), "Halo PC's at 2x: %s", reason ? reason : "taken");
		reason = mod_checked(xbox_pages, PAGES + 1, 1);
		CHECK(reason && strstr(reason, "16 pages, not the map's 15"), "a page more: %s", reason ? reason : "taken");
	}
	CASE("not-lightmap") {
		map_made(1, PAGES);
		reason = mod_made(pc_pages, PAGES, 1);
		CHECK(!reason, "a default-usage bitmap of other sizes, as before: %s", reason);
		reason = mod_made(xbox_pages, PAGES + 1, 3);
		CHECK(!reason, "with a bitmap more, at 3x: %s", reason);
		reason = mod_made(xbox_pages, PAGES - 1, 1);
		CHECK(reason && strstr(reason, "fewer bitmaps"), "fewer bitmaps are still left out: %s",
			reason ? reason : "taken");
	}
	return 0;
}
