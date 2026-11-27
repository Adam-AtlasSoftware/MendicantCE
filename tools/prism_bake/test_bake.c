/*
TEST_BAKE.C

The baker's host libraries on synthetic data (tools/test_prism_bake.py
builds and runs it): port/linux/vulkan/prism_bake_reach.c (a collision bsp
built here from boxes), prism_bake_atlas.c, prism_bake_batch.c (the
volume's batches, their readbacks in flight), prism_bake_cache.c,
prism_bake_converge.c, prism_bake_filter.c, prism_bake_pack.c (the packed
records) and prism_bake_layers.c (the layered bake's recombining).

	test_bake <scratch folder>

prints one line per check ("ok <name>" or "FAIL <name>: why") and exits
non-zero if any failed.
*/

#include "prism_bake_atlas.h"
#include "prism_bake_batch.h"
#include "prism_bake_cache.h"
#include "prism_bake_converge.h"
#include "prism_bake_filter.h"
#include "prism_bake_layers.h"
#include "prism_bake_pack.h"
#include "prism_bake_reach.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, char const *name, char const *why)
{
	if (condition)
		printf("ok %s\n", name);
	else
	{
		printf("FAIL %s: %s\n", name, why);
		failures++;
	}
}

/* ---------- a random source */

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static double uniform(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return ((double)(rng_state >> 11) + 0.5) / 9007199254740992.0;
}

static double gaussian(void)
{
	return sqrt(-2.0 * log(uniform())) * cos(6.283185307179586 * uniform());
}

/* ---------- a collision bsp from boxes: air is inside an air box and
outside every solid box (a kd-tree split on the boxes' faces) */

struct box
{
	float minimum[3], maximum[3];
	int solid;
	int cluster;
};

struct builder
{
	struct box const *boxes;
	int box_count;
	int32_t nodes[3 * 4096];
	float planes[4 * 4096];
	int32_t node_count, plane_count;
	int16_t leaf_clusters[4096];
	int32_t leaf_count;
};

static int box_overlaps(struct box const *b, float const *lo, float const *hi)
{
	int a;

	for (a = 0; a < 3; a++)
	{
		if (b->maximum[a] <= lo[a] || b->minimum[a] >= hi[a])
			return 0;
	}
	return 1;
}

static int box_contains(struct box const *b, float const *lo, float const *hi)
{
	int a;

	for (a = 0; a < 3; a++)
	{
		if (lo[a] < b->minimum[a] || hi[a] > b->maximum[a])
			return 0;
	}
	return 1;
}

static int32_t build(struct builder *b, float const *lo, float const *hi)
{
	int i, a, any_air = 0, solid_overlap = 0, air_container = -1;
	int32_t node;

	for (i = 0; i < b->box_count; i++)
	{
		struct box const *x = &b->boxes[i];

		if (!box_overlaps(x, lo, hi))
			continue;
		if (x->solid)
		{
			if (box_contains(x, lo, hi))
				return PRISM_BAKE_NONE;
			solid_overlap = 1;
		}
		else
		{
			any_air = 1;
			if (air_container < 0 && box_contains(x, lo, hi))
				air_container = i;
		}
	}
	if (!any_air)
		return PRISM_BAKE_NONE;
	if (air_container >= 0 && !solid_overlap)
	{
		b->leaf_clusters[b->leaf_count] = (int16_t)b->boxes[air_container].cluster;
		return (int32_t)(0x80000000u | (uint32_t)b->leaf_count++);
	}
	for (i = 0; i < b->box_count; i++)
	{
		struct box const *x = &b->boxes[i];

		if (!box_overlaps(x, lo, hi))
			continue;
		for (a = 0; a < 3; a++)
		{
			int side;

			for (side = 0; side < 2; side++)
			{
				float v = side ? x->maximum[a] : x->minimum[a];

				if (v > lo[a] && v < hi[a])
				{
					float lo2[3], hi2[3];
					int32_t back, front;

					node = b->node_count++;
					b->planes[b->plane_count * 4 + 0] = a == 0;
					b->planes[b->plane_count * 4 + 1] = a == 1;
					b->planes[b->plane_count * 4 + 2] = a == 2;
					b->planes[b->plane_count * 4 + 3] = v;
					b->nodes[node * 3] = b->plane_count++;
					memcpy(lo2, lo, sizeof(lo2));
					memcpy(hi2, hi, sizeof(hi2));
					hi2[a] = v;
					back = build(b, lo, hi2);
					lo2[a] = v;
					front = build(b, lo2, hi);
					b->nodes[node * 3 + 1] = back;
					b->nodes[node * 3 + 2] = front;
					return node;
				}
			}
		}
	}
	return PRISM_BAKE_NONE;
}

/* room A (cluster 0): 10 x 10 x 3 with a step block (0.3 high), a high
block (1.5), a mezzanine (1.2) reached by stairs of 0.3, a chimney to
z 30; room B (cluster 1): sealed, 10 wu away */
static struct box const scene_boxes[] =
{
	{ { 0, 0, 0 }, { 10, 10, 3 }, 0, 0 },
	{ { 0, 0, 0 }, { 2, 2, 30 }, 0, 0 },
	{ { 20, 0, 0 }, { 25, 5, 3 }, 0, 1 },
	{ { 6, 6, 0 }, { 8, 8, 0.3f }, 1, 0 },
	{ { 6, 1, 0 }, { 8, 3, 1.5f }, 1, 0 },
	{ { 0, 7, 0 }, { 4, 10, 1.2f }, 1, 0 },
	{ { 5, 7, 0 }, { 5.5f, 10, 0.3f }, 1, 0 },
	{ { 4.5f, 7, 0 }, { 5, 10, 0.6f }, 1, 0 },
	{ { 4, 7, 0 }, { 4.5f, 10, 0.9f }, 1, 0 },
};

static struct builder scene;

static void scene_build(struct prism_bake_bsp3d *bsp)
{
	float lo[3] = { -50, -50, -50 }, hi[3] = { 50, 50, 50 };
	int32_t root;

	memset(&scene, 0, sizeof(scene));
	scene.boxes = scene_boxes;
	scene.box_count = (int)(sizeof(scene_boxes) / sizeof(scene_boxes[0]));
	root = build(&scene, lo, hi);
	(void)root;
	bsp->nodes = scene.nodes;
	bsp->node_count = scene.node_count;
	bsp->planes = scene.planes;
	bsp->plane_count = scene.plane_count;
	bsp->leaf_count = scene.leaf_count;
}

static int walk_at(struct prism_bake_reach const *reach, float x, float y, float z)
{
	float cell = reach->parameters.cell;

	return (prism_bake_voxels_get(&reach->voxels, (int32_t)floorf(x / cell), (int32_t)floorf(y / cell),
		(int32_t)floorf((z + 0.01f) / cell)) & PRISM_BAKE_REACH_WALK) != 0;
}

static void test_reach(void)
{
	struct prism_bake_bsp3d bsp;
	struct prism_bake_reach reach;
	struct prism_bake_reach_parameters parameters;
	struct prism_bake_seed seeds[2];
	float a[3], b[3], fraction, normal[3];
	char why[256];
	size_t slot;
	int in_b = 0;

	scene_build(&bsp);
	a[0] = 5;
	a[1] = 5;
	a[2] = 1;
	check(prism_bake_bsp3d_test_point(&bsp, a) != PRISM_BAKE_NONE, "bsp3d_air", "room A's middle is solid");
	a[2] = -1;
	check(prism_bake_bsp3d_test_point(&bsp, a) == PRISM_BAKE_NONE, "bsp3d_solid", "below the floor is air");
	a[0] = 7;
	a[1] = 2;
	a[2] = 1;
	check(prism_bake_bsp3d_test_point(&bsp, a) == PRISM_BAKE_NONE, "bsp3d_block", "inside the high block is air");
	a[0] = 5;
	a[1] = 5;
	a[2] = 2;
	b[0] = 5;
	b[1] = 5;
	b[2] = -10;
	check(prism_bake_bsp3d_cast(&bsp, a, b, &fraction, normal) && fabsf(a[2] + (b[2] - a[2]) * fraction) < 1e-4f &&
		normal[2] > 0.99f, "bsp3d_cast_floor", "the cast down does not meet the floor at 0 facing up");
	b[0] = 15;
	b[1] = 5;
	b[2] = 2;
	sprintf(why, "the cast across meets x %g", a[0] + (b[0] - a[0]) * fraction);
	check(prism_bake_bsp3d_cast(&bsp, a, b, &fraction, normal) && fabsf(a[0] + (b[0] - a[0]) * fraction - 10) < 1e-4f &&
		normal[0] < -0.99f, "bsp3d_cast_wall", why);
	a[2] = -5;
	check(prism_bake_bsp3d_cast(&bsp, a, b, &fraction, normal) && fraction == 0.0f, "bsp3d_cast_start_solid",
		"a cast from solid is not a hit at 0");
	a[0] = 1;
	a[1] = 1;
	a[2] = 1;
	b[0] = 1;
	b[1] = 1;
	b[2] = 25;
	check(!prism_bake_bsp3d_cast(&bsp, a, b, NULL, NULL), "bsp3d_cast_clear", "the chimney is not clear");

	prism_bake_reach_defaults(&parameters);
	parameters.flight = 1;
	parameters.flight_cell = 1.0f;
	seeds[0].position[0] = 5.1f;
	seeds[0].position[1] = 5.1f;
	seeds[0].position[2] = 0.3f;
	seeds[0].kind = PRISM_BAKE_SEED_WALK;
	/* (a seed in solid, of another bsp, far below) */
	seeds[1].position[0] = 5;
	seeds[1].position[1] = 5;
	seeds[1].position[2] = -20;
	seeds[1].kind = PRISM_BAKE_SEED_WALK;
	check(!prism_bake_reach_build(&reach, &bsp, seeds, 2, &parameters), "reach_build", "out of memory");
	check(reach.seeds_used == 1 && reach.seeds_outside == 1, "reach_seeds", "the seeds are not one in, one out");
	check(walk_at(&reach, 5.1f, 5.1f, 0), "reach_seed_floor", "the seed's floor is not reached");
	check(walk_at(&reach, 9.6f, 0.4f, 0) && walk_at(&reach, 0.6f, 0.6f, 0), "reach_corners",
		"room A's floor corners are not reached");
	check(walk_at(&reach, 7.1f, 7.1f, 0.3f), "reach_step", "the step block's top is not reached");
	check(walk_at(&reach, 5.2f, 8.6f, 0.3f) && walk_at(&reach, 4.2f, 8.6f, 0.9f) && walk_at(&reach, 2.1f, 8.6f, 1.2f),
		"reach_stairs",
		"the stairs do not reach the mezzanine");
	check(!walk_at(&reach, 7.1f, 2.1f, 1.5f), "reach_high_block", "the high block's top is reached");
	for (slot = 0; slot < reach.voxels.capacity; slot++)
	{
		int32_t xyz[3];

		if (prism_bake_voxels_slot(&reach.voxels, slot, xyz) && xyz[0] * reach.voxels.cell >= 15.0f)
			in_b = 1;
	}
	for (slot = 0; slot < reach.flight.capacity; slot++)
	{
		int32_t xyz[3];

		if (prism_bake_voxels_slot(&reach.flight, slot, xyz) && xyz[0] * reach.flight.cell >= 15.0f)
			in_b = 1;
	}
	check(!in_b, "reach_sealed_room", "the sealed room is reached");
	check(prism_bake_voxels_get(&reach.flight, 1, 1, 28) != 0, "reach_flight", "flight does not reach up the chimney");
	check(!(prism_bake_voxels_get(&reach.voxels, 4, 4, 20 * 4) & PRISM_BAKE_REACH_CAMERA), "reach_camera_bounded",
		"the camera volume reaches far up the chimney");

	/* importance */
	{
		static int32_t const surfaces[4] = { 0, 1, 2, 3 };
		static int32_t const references[4] = { 0, 1, 2, 3 };
		float positions[4 * 9] =
		{
			/* the floor near the seed, up */
			4, 4, 0, 6, 4, 0, 4, 6, 0,
			/* the chimney's ceiling, down */
			0.5f, 0.5f, 29.9f, 0.5f, 1.5f, 29.9f, 1.5f, 0.5f, 29.9f,
			/* a floor facing down */
			4, 4, 0, 4, 6, 0, 6, 4, 0,
			/* room B's floor, up */
			21, 1, 0, 23, 1, 0, 21, 3, 0,
		};
		int32_t first[4096];
		int16_t counts[4096];
		uint32_t pvs[2] = { 1u, 2u };
		struct prism_bake_clusters clusters;
		struct prism_bake_importance_parameters ip;
		struct prism_bake_importance_counts ic;
		uint8_t tiers[4];
		int32_t leaf;

		reach.parameters.flight = 0;
		prism_bake_voxels_free(&reach.flight);
		prism_bake_voxels_init(&reach.flight, parameters.flight_cell);
		for (leaf = 0; leaf < scene.leaf_count; leaf++)
		{
			first[leaf] = scene.leaf_clusters[leaf] ? 3 : 0;
			counts[leaf] = scene.leaf_clusters[leaf] ? 1 : 3;
		}
		memset(&clusters, 0, sizeof(clusters));
		clusters.leaf_clusters = scene.leaf_clusters;
		clusters.leaf_count = scene.leaf_count;
		clusters.leaf_first_reference = first;
		clusters.leaf_reference_count = counts;
		clusters.reference_surfaces = references;
		clusters.reference_count = 4;
		clusters.pvs = pvs;
		clusters.pvs_words = 1;
		clusters.cluster_count = 2;
		prism_bake_importance_defaults(&ip);
		check(!prism_bake_importance(&reach, &bsp, &clusters, positions, surfaces, 4, &ip, tiers, &ic),
			"importance_run", "out of memory");
		sprintf(why, "tiers %d %d %d %d, not 0 1 2 2", tiers[0], tiers[1], tiers[2], tiers[3]);
		check(tiers[0] == 0 && tiers[1] == 1 && tiers[2] == 2 && tiers[3] == 2, "importance_tiers", why);
		check(ic.reachable_clusters == 1 && ic.visible_clusters == 1, "importance_clusters",
			"the reachable and visible clusters are not room A alone");
	}
	prism_bake_reach_free(&reach);

	/* water at z 1: from the mezzanine, the stairs' top (its eye above the
	water) is walked, the step and the room's floor (drowned) are not */
	{
		static float const water[4] = { 0, 0, 1, 1.0f };

		parameters.flight = 0;
		parameters.water_planes = water;
		parameters.water_plane_count = 1;
		seeds[0].position[0] = 2.1f;
		seeds[0].position[1] = 8.6f;
		seeds[0].position[2] = 1.3f;
		check(!prism_bake_reach_build(&reach, &bsp, seeds, 1, &parameters), "reach_water_build", "out of memory");
		check(walk_at(&reach, 2.1f, 8.6f, 1.2f) && walk_at(&reach, 4.2f, 8.6f, 0.9f) &&
			!walk_at(&reach, 5.2f, 8.6f, 0.3f) && !walk_at(&reach, 5.1f, 5.1f, 0.0f), "reach_water",
			"the walk goes under the water, or not to the floors above it");
		prism_bake_reach_free(&reach);
		parameters.water_planes = NULL;
		parameters.water_plane_count = 0;
	}

	/* the walk stopped (parameters.cancel): another BSP's inputs replaced
	its own, and the render thread, which joins the walk's thread there, must
	not wait out the whole walk (d40's BSP 8's, 696 s: the game stopped at
	the switch from BSP 9 to 0). Asked before it starts, the build stops at
	once, whatever the flood still had to do; a flag left at 0 changes
	nothing */
	{
		int cancel = 1;
		size_t walked;

		seeds[0].position[0] = 5.1f;
		seeds[0].position[1] = 5.1f;
		seeds[0].position[2] = 0.3f;
		parameters.flight = 1;
		parameters.cancel = &cancel;
		check(prism_bake_reach_build(&reach, &bsp, seeds, 1, &parameters) == PRISM_BAKE_REACH_CANCELLED,
			"reach_cancelled", "a walk asked to stop is not stopped");
		check(reach.walk_count <= 1 && reach.camera_count == 0 && reach.flight_count == 0, "reach_cancelled_at_once",
			"a walk asked to stop before it starts floods on");
		prism_bake_reach_free(&reach);
		cancel = 0;
		check(!prism_bake_reach_build(&reach, &bsp, seeds, 1, &parameters), "reach_cancel_unset", "out of memory");
		walked = reach.walk_count;
		check(walked > 1000 && walk_at(&reach, 9.6f, 0.4f, 0) && reach.flight_count > 0, "reach_cancel_unset_whole",
			"a walk with its stop flag at 0 is not whole");
		prism_bake_reach_free(&reach);
		parameters.cancel = NULL;
	}
}

/* ---------- the atlas */

static void quad(struct prism_bake_atlas_triangle *t, int32_t page, float u0, float v0, float u1, float v1,
	float x0, float y0, float size, uint8_t tier)
{
	float const uv[2][6] = { { u0, v0, u1, v0, u0, v1 }, { u1, v0, u1, v1, u0, v1 } };
	float const p[2][9] =
	{
		{ x0, y0, 0, x0 + size, y0, 0, x0, y0 + size, 0 },
		{ x0 + size, y0, 0, x0 + size, y0 + size, 0, x0, y0 + size, 0 }
	};
	int k;

	for (k = 0; k < 2; k++)
	{
		t[k].page = page;
		memcpy(t[k].uv, uv[k], sizeof(uv[k]));
		memcpy(t[k].position, p[k], sizeof(p[k]));
		t[k].tier = tier;
	}
}

static void test_atlas(void)
{
	struct prism_bake_atlas_page pages[1] = { { 64, 64 } };
	struct prism_bake_atlas_triangle triangles[7];
	struct prism_bake_atlas_parameters parameters;
	struct prism_bake_atlas atlas;
	char why[256];
	int32_t vp;
	float x, y;

	/* chart 0: 1 x 1 wu over 8 x 8 CE texels (8 per wu), tier 0; chart 1:
	2 x 2 wu over 8 x 8 (4 per wu), tier 2; chart 2: 1 x 1 wu over 4 x 4
	(4 per wu) tier 1; one triangle without UVs */
	quad(triangles, 0, 2.0f / 64, 2.0f / 64, 10.0f / 64, 10.0f / 64, 0, 0, 1, 0);
	quad(triangles + 2, 0, 20.0f / 64, 2.0f / 64, 28.0f / 64, 10.0f / 64, 10, 0, 2, 2);
	quad(triangles + 4, 0, 40.0f / 64, 40.0f / 64, 44.0f / 64, 44.0f / 64, 20, 0, 1, 1);
	triangles[6] = triangles[0];
	triangles[6].page = -1;
	prism_bake_atlas_defaults(&parameters, PRISM_BAKE_QUALITY_MAX);
	parameters.tile = 8;
	check(!prism_bake_atlas_build(&atlas, pages, 1, triangles, 7, &parameters), "atlas_build", "out of memory");
	sprintf(why, "%d charts", atlas.chart_count);
	check(atlas.chart_count == 3, "atlas_charts", why);
	sprintf(why, "scales %d %d %d (densities %g %g %g)", atlas.charts[0].scale, atlas.charts[1].scale,
		atlas.charts[2].scale, atlas.charts[0].density, atlas.charts[1].density, atlas.charts[2].density);
	/* (tier 0 at 16: 16 / 8 = 2; tier 2 at 0.5: CE's own; tier 1 at 8: 8 / 4 = 2) */
	check(atlas.charts[0].scale == 2 && atlas.charts[1].scale == 1 && atlas.charts[2].scale == 2, "atlas_scales", why);
	sprintf(why, "%d virtual pages", atlas.virtual_page_count);
	check(atlas.virtual_page_count == 2 && atlas.charts[0].virtual_page == atlas.charts[2].virtual_page,
		"atlas_virtual_pages", why);
	check(atlas.totals.triangles_without_atlas == 1, "atlas_without", "the triangle without UVs is counted wrong");
	/* (chart 0's box with its gutter: CE 1..11 -> 2..22 at scale 2: tiles 0..2
	of 8, both axes: 9; chart 2's 39..45 -> 78..90: tiles 9..11: 9; chart 1's
	19..29 at scale 1: tiles 2..3 x 0..1: 4) */
	sprintf(why, "%lu tiles", (unsigned long)atlas.totals.tiles);
	check(atlas.totals.tiles == 22, "atlas_tiles", why);
	check(fabs(atlas.totals.texels_covered - (64.0 * 4 + 64.0 + 16.0 * 4)) < 1e-3, "atlas_covered",
		"the covered texels are not the charts' areas at their scales");
	prism_bake_atlas_texel(&atlas, 0, 6.0f / 64, 6.0f / 64, &vp, &x, &y);
	check(vp == atlas.charts[0].virtual_page && fabsf(x - 12.0f) < 1e-4f && fabsf(y - 12.0f) < 1e-4f, "atlas_texel",
		"a UV's texel is not at the scaled place");
	{
		float errors[3] = { 0.5f, 0.0f, 0.0f };

		check(prism_bake_atlas_refine_by_error(&atlas, errors, 0.1f) == 1 && !prism_bake_atlas_layout(&atlas) &&
			atlas.charts[0].scale == 4 && atlas.charts[2].scale == 2 && atlas.virtual_page_count == 3,
			"atlas_refine", "refining chart 0 does not double its scale alone");
		check(prism_bake_atlas_refine(&atlas, 0, 10) == 1 && atlas.charts[0].refine == parameters.maximum_refine &&
			!prism_bake_atlas_refine(&atlas, 0, 1), "atlas_refine_limit", "refinement is not limited");
	}
	prism_bake_atlas_free(&atlas);

	/* a scale clamped by the largest side */
	quad(triangles, 0, 0, 0, 1.0f / 64, 1.0f / 64, 0, 0, 1, 0);
	parameters.maximum_dimension = 256;
	check(!prism_bake_atlas_build(&atlas, pages, 1, triangles, 2, &parameters) && atlas.charts[0].scale == 4 &&
		atlas.charts[0].clamped, "atlas_clamp", "a page wider than maximum_dimension is not clamped");
	prism_bake_atlas_free(&atlas);
}

/* texels a triangle's gutter reaches (sampled over the triangle, a square of
the gutter's half-side around each sample) that lie in a tile left
unallocated: 0 when no texel is missed */
static long atlas_missed(struct prism_bake_atlas const *atlas)
{
	long missed = 0;
	size_t index;

	for (index = 0; index < atlas->triangle_count; index++)
	{
		int32_t c = atlas->triangle_charts[index], scale, a, b, x, y, x0, x1, y0, y1;
		struct prism_bake_virtual_page const *vp;
		float const *t = atlas->triangle_texels + index * 6;
		double d;

		if (c < 0)
			continue;
		scale = atlas->charts[c].scale;
		vp = &atlas->virtual_pages[atlas->charts[c].virtual_page];
		d = (double)atlas->parameters.gutter * scale;
		for (a = 0; a <= 40; a++)
		{
			for (b = 0; a + b <= 40; b++)
			{
				double w1 = a / 40.0, w2 = b / 40.0, w0 = 1.0 - w1 - w2;
				double sx = (w0 * t[0] + w1 * t[2] + w2 * t[4]) * scale;
				double sy = (w0 * t[1] + w1 * t[3] + w2 * t[5]) * scale;

				x0 = (int32_t)floor(sx - d);
				y0 = (int32_t)floor(sy - d);
				x1 = (int32_t)ceil(sx + d);
				y1 = (int32_t)ceil(sy + d);
				x0 = x0 < 0 ? 0 : x0;
				y0 = y0 < 0 ? 0 : y0;
				x1 = x1 > vp->width ? vp->width : x1;
				y1 = y1 > vp->height ? vp->height : y1;
				for (y = y0; y < y1; y++)
				{
					for (x = x0; x < x1; x++)
					{
						if (!vp->tiles[(size_t)(y / atlas->parameters.tile) * vp->tiles_x +
							x / atlas->parameters.tile])
						{
							missed++;
						}
					}
				}
			}
		}
	}
	return missed;
}

/* a triangle from corners in CE texels of a page, its world size the texels
times metres per texel (so its density is the inverse) */
static void texel_triangle(struct prism_bake_atlas_triangle *t, struct prism_bake_atlas_page const *page,
	float const *texels, float wu_per_texel, uint8_t tier)
{
	int k;

	t->page = 0;
	for (k = 0; k < 3; k++)
	{
		t->uv[k * 2] = texels[k * 2] / (float)page->width;
		t->uv[k * 2 + 1] = texels[k * 2 + 1] / (float)page->height;
		t->position[k * 3] = texels[k * 2] * wu_per_texel;
		t->position[k * 3 + 1] = texels[k * 2 + 1] * wu_per_texel;
		t->position[k * 3 + 2] = 0.0f;
	}
	t->tier = tier;
}

static void test_atlas_tiles(void)
{
	static int32_t const tiles[2] = { 16, 32 };
	struct prism_bake_atlas_page pages[1] = { { 100, 70 } };
	struct prism_bake_atlas_triangle triangles[64];
	struct prism_bake_atlas_parameters parameters;
	struct prism_bake_atlas atlas;
	double allocated[2] = { 0, 0 }, covered[2] = { 0, 0 };
	size_t tile_count[2] = { 0, 0 };
	char why[256];
	int n, i, k;

	/* a thin diagonal, tier 2 at CE's own density (scale 1) on a 128 square
	page: only the tiles along it */
	{
		static float const diagonal[6] = { 2, 2, 124, 120, 124, 123 };
		struct prism_bake_atlas_page big[1] = { { 128, 128 } };
		struct prism_bake_virtual_page const *vp;
		int32_t x, y, diagonal_off = 0;
		size_t along = 0;

		texel_triangle(triangles, big, diagonal, 1.0f, 2);
		prism_bake_atlas_defaults(&parameters, PRISM_BAKE_QUALITY_MAX);
		parameters.tile = 16;
		check(!prism_bake_atlas_build(&atlas, big, 1, triangles, 1, &parameters) && atlas.charts[0].scale == 1,
			"atlas_thin_build", "the thin triangle's chart is not at scale 1");
		vp = &atlas.virtual_pages[0];
		for (y = 0; y < vp->tiles_y; y++)
		{
			for (x = 0; x < vp->tiles_x; x++)
			{
				int on = vp->tiles[y * vp->tiles_x + x];

				/* (a tile more than one off the line y = x is no neighbour of it) */
				if (on && (x - y > 1 || y - x > 1))
					diagonal_off++;
				if (!on && x == y)
					along++;
			}
		}
		sprintf(why, "%lu tiles of %d, %d off the diagonal, %lu diagonal tiles missed", (unsigned long)vp->tile_count,
			vp->tiles_x * vp->tiles_y, diagonal_off, (unsigned long)along);
		check(vp->tile_count >= 8 && vp->tile_count <= 24 && !diagonal_off && !along, "atlas_thin_diagonal", why);
		sprintf(why, "%ld texels missed", atlas_missed(&atlas));
		check(atlas_missed(&atlas) == 0, "atlas_thin_missed", why);
		prism_bake_atlas_free(&atlas);
	}

	/* random triangles on a page that is not a whole number of tiles, at
	every tier and several scales, tiles of 16 and of 32: no texel a
	triangle's gutter reaches lies in an unallocated tile */
	n = 48;
	for (i = 0; i < n; i++)
	{
		float c[6], origin_x = (float)(uniform() * 90.0), origin_y = (float)(uniform() * 60.0);
		float size = 1.0f + (float)(uniform() * (i % 3 == 0 ? 40.0 : 8.0));

		for (k = 0; k < 3; k++)
		{
			/* (every third one is a sliver) */
			c[k * 2] = origin_x + (float)(uniform() * size);
			c[k * 2 + 1] = origin_y + (float)(uniform() * (i % 3 == 1 ? 0.5 : size));
		}
		texel_triangle(&triangles[i], pages, c, 0.25f + (float)(uniform() * 0.5), (uint8_t)(i % 3));
	}
	for (k = 0; k < 2; k++)
	{
		long missed;

		prism_bake_atlas_defaults(&parameters, PRISM_BAKE_QUALITY_MAX);
		parameters.tile = tiles[k];
		check(!prism_bake_atlas_build(&atlas, pages, 1, triangles, (size_t)n, &parameters), "atlas_random_build",
			"out of memory");
		missed = atlas_missed(&atlas);
		sprintf(why, "tile %d: %ld texels in unallocated tiles", tiles[k], missed);
		check(missed == 0, "atlas_no_texel_missed", why);
		allocated[k] = atlas.totals.texels_allocated;
		covered[k] = atlas.totals.texels_covered;
		tile_count[k] = atlas.totals.tiles;
		/* (a refined chart's tiles are marked the same way) */
		prism_bake_atlas_refine(&atlas, 0, 1);
		prism_bake_atlas_layout(&atlas);
		sprintf(why, "tile %d refined: %ld texels missed", tiles[k], atlas_missed(&atlas));
		check(atlas_missed(&atlas) == 0, "atlas_refined_no_texel_missed", why);
		prism_bake_atlas_free(&atlas);
	}
	sprintf(why, "16: %lu tiles, %g texels; 32: %lu tiles, %g texels; covered %g and %g", (unsigned long)tile_count[0],
		allocated[0], (unsigned long)tile_count[1], allocated[1], covered[0], covered[1]);
	check(allocated[0] <= allocated[1] && tile_count[0] >= tile_count[1] && covered[0] == covered[1] &&
		allocated[0] >= covered[0], "atlas_tile_sizes", why);
}

/* ---------- the cache file */

static void key_make(struct prism_bake_cache_key *key)
{
	memset(key, 0, sizeof(*key));
	key->map_checksum = 0x12345678u;
	strcpy(key->map_name, "testmap");
	key->bsp_index = 1;
	strcpy(key->bsp_name, "levels\\test\\testmap_b");
	key->bsp_hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, "bsp", 3);
	key->geometry_hash = 0;
	key->material_hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, "materials", 9);
	key->light_hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, "lights", 6);
	key->settings_hash = 7;
	key->baker_version = 1;
	key->quality = PRISM_BAKE_QUALITY_MAX;
	key->convergence_threshold = 0.01f;
}

static long file_size(char const *path)
{
	FILE *file = fopen(path, "rb");
	long size;

	if (!file)
		return -1;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fclose(file);
	return size;
}

static void test_cache(char const *folder)
{
	struct prism_bake_cache_key key, other;
	struct prism_bake_cache_writer writer;
	struct prism_bake_cache cache;
	struct prism_bake_record const *record;
	char path[1024], new_path[1100], why[256];
	unsigned char payload[1000], back[1000];
	uint64_t job, plan3[3] = { 1, 2, 3 }, plan_one[1] = { 1 };
	uint32_t value;
	long size;
	FILE *file;

	sprintf(path, "%s/test.pbk", folder);
	sprintf(new_path, "%s.new", path);
	remove(path);
	remove(new_path);
	key_make(&key);
	check(prism_bake_crc32(0, "123456789", 9) == 0xCBF43926u, "cache_crc32", "CRC-32 of 123456789 is not CBF43926");
	check(prism_bake_hash64(PRISM_BAKE_HASH64_SEED, "a", 1) == 0xAF63DC4C8601EC8CULL, "cache_fnv", "FNV-1a of a");
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_MISSING, "cache_missing", "no file is not MISSING");
	prism_bake_cache_close(&cache);

	/* a plan and three jobs, the third checkpointed and unfinished */
	check(!prism_bake_cache_create(&writer, path, &key, 0), "cache_create", "it cannot be made");
	memset(payload, 0xAB, sizeof(payload));
	prism_bake_cache_append_plan(&writer, plan3, 3);
	for (job = 1; job <= 2; job++)
	{
		payload[0] = (unsigned char)job;
		prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, job, payload, 999);
	}
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_CHECKPOINT, 3, payload, 100);
	check(!prism_bake_cache_writer_close(&writer), "cache_write", "the writes fail");

	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT && cache.record_count == 4 &&
		!cache.torn && !cache.done, "cache_read", "the log does not read back whole and current");
	check(prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 2) &&
		!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 3), "cache_jobs",
		"the finished and unfinished jobs are wrong");
	record = prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 2);
	check(record && record->size == 999 && !prism_bake_cache_read(&cache, record, back, sizeof(back)) && back[0] == 2 &&
		back[998] == 0xAB, "cache_payload", "a job's payload does not read back");
	check(prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_CHECKPOINT, 3) != NULL, "cache_checkpoint",
		"the checkpoint is not found");
	prism_bake_cache_close(&cache);

	/* stale: each key field */
	other = key;
	other.light_hash ^= 1;
	other.quality = PRISM_BAKE_QUALITY_HIGH;
	check(prism_bake_cache_open(&cache, path, &other, 0) == PRISM_BAKE_CACHE_STALE &&
		cache.differences == (PRISM_BAKE_KEY_LIGHTS | PRISM_BAKE_KEY_QUALITY), "cache_stale",
		"another key's differences are wrong");
	prism_bake_cache_close(&cache);
	other = key;
	other.convergence_threshold = 0.005f;
	check(prism_bake_key_compare(&key, &other) == PRISM_BAKE_KEY_QUALITY, "cache_key_threshold",
		"the threshold is not part of the key");
	other = key;
	other.geometry_hash = 5;
	other.map_checksum++;
	check(prism_bake_key_compare(&key, &other) == (PRISM_BAKE_KEY_GEOMETRY | PRISM_BAKE_KEY_MAP), "cache_key_fields",
		"the map and geometry are not part of the key");
	check(prism_bake_cache_resume(&writer, &cache, path, &other, 0) != 0, "cache_resume_stale",
		"a stale bake is resumed");
	prism_bake_cache_close(&cache);

	/* a crash during a record: the file cut part-way through a new one */
	size = file_size(path);
	file = fopen(path, "ab");
	fwrite(payload, 1, 57, file);
	fclose(file);
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT && cache.torn &&
		cache.record_count == 4 && cache.valid_end == (uint64_t)size, "cache_torn", "a torn tail is not found");
	prism_bake_cache_close(&cache);
	check(!prism_bake_cache_resume(&writer, &cache, path, &key, 1) && writer.sequence == 4, "cache_resume",
		"the bake does not resume");
	check(!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 3), "cache_resume_next",
		"the resumed bake's next job is not 3");
	prism_bake_cache_close(&cache);
	payload[0] = 3;
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, 3, payload, 999);
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_DONE, 0, NULL, 0);
	prism_bake_cache_writer_close(&writer);
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT && !cache.torn && cache.done &&
		cache.record_count == 6 && prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 3),
		"cache_resume_finish", "the resumed bake does not finish whole");
	prism_bake_cache_close(&cache);

	/* a payload's byte changed: the log ends before it */
	file = fopen(path, "r+b");
	fseek(file, PRISM_BAKE_CACHE_HEADER_SIZE + PRISM_BAKE_CACHE_RECORD_HEADER_SIZE + 16 +
		PRISM_BAKE_CACHE_RECORD_HEADER_SIZE + 500, SEEK_SET);
	fputc(0x00, file);
	fclose(file);
	sprintf(why, "the corrupt payload is read");
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT && cache.record_count == 1 &&
		cache.torn, "cache_corrupt_payload", why);
	prism_bake_cache_close(&cache);
	check(prism_bake_cache_open(&cache, path, &key, 0) == PRISM_BAKE_CACHE_CURRENT && cache.record_count == 6,
		"cache_unverified_open", "an open without payload checks does not read the headers");
	record = prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 1);
	check(record && prism_bake_cache_read(&cache, record, back, sizeof(back)) != 0, "cache_corrupt_read",
		"the corrupt payload reads");
	prism_bake_cache_close(&cache);

	/* a header's byte changed: not a bake file */
	file = fopen(path, "r+b");
	fseek(file, 30, SEEK_SET);
	fputc('X', file);
	fclose(file);
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_INVALID, "cache_corrupt_header",
		"a corrupt header is read");
	prism_bake_cache_close(&cache);

	/* a record that claims a huge payload */
	prism_bake_cache_create(&writer, path, &key, 0);
	prism_bake_cache_append_plan(&writer, plan_one, 1);
	prism_bake_cache_writer_close(&writer);
	file = fopen(path, "r+b");
	{
		unsigned char h[PRISM_BAKE_CACHE_RECORD_HEADER_SIZE];
		uint32_t crc;

		fseek(file, PRISM_BAKE_CACHE_HEADER_SIZE, SEEK_SET);
		fread(h, sizeof(h), 1, file);
		h[20] = 0xFF;
		h[21] = 0xFF;
		h[22] = 0xFF;
		h[23] = 0x7F;
		crc = prism_bake_crc32(0, h, sizeof(h) - 4);
		h[36] = (unsigned char)crc;
		h[37] = (unsigned char)(crc >> 8);
		h[38] = (unsigned char)(crc >> 16);
		h[39] = (unsigned char)(crc >> 24);
		fseek(file, PRISM_BAKE_CACHE_HEADER_SIZE, SEEK_SET);
		fwrite(h, sizeof(h), 1, file);
	}
	fclose(file);
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT && cache.record_count == 0 &&
		cache.torn, "cache_oversize", "a record past the file's end is read");
	prism_bake_cache_close(&cache);

	/* the rebake beside the stale one, renamed over it */
	prism_bake_cache_create(&writer, new_path, &other, 0);
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_DONE, 0, NULL, 0);
	prism_bake_cache_writer_close(&writer);
	check(!prism_bake_cache_commit(new_path, path) && prism_bake_cache_open(&cache, path, &other, 1) ==
		PRISM_BAKE_CACHE_CURRENT && cache.done && file_size(new_path) < 0, "cache_commit",
		"the new bake does not replace the old");
	prism_bake_cache_close(&cache);

	/* many jobs: the index */
	prism_bake_cache_create(&writer, path, &key, 0);
	for (job = 0; job < 5000; job++)
	{
		value = (uint32_t)job;
		prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_VOLUME_BRICK, job, &value, 4);
	}
	prism_bake_cache_writer_close(&writer);
	prism_bake_cache_open(&cache, path, &key, 1);
	record = prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_VOLUME_BRICK, 4321);
	value = 0;
	check(record && !prism_bake_cache_read(&cache, record, &value, 4) && value == 4321 &&
		!prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_VOLUME_BRICK, 5000), "cache_index",
		"the index does not find the job");
	prism_bake_cache_close(&cache);
}

static int job_compare(void const *a, void const *b)
{
	uint64_t x = *(uint64_t const *)a, y = *(uint64_t const *)b;

	return x < y ? -1 : x > y;
}

static void test_cache_jobs(char const *folder)
{
	struct prism_bake_cache_key key;
	struct prism_bake_cache_writer writer;
	struct prism_bake_cache cache;
	struct prism_bake_record const *record;
	uint64_t *ids, plan[8];
	size_t count = 0, capacity = 220000, k, duplicates = 0, wrong_type = 0;
	uint32_t a, b, c, d;
	unsigned char data[8], header[PRISM_BAKE_CACHE_HEADER_SIZE];
	char path[1024], why[256];
	FILE *file;
	uint64_t tile_a, tile_b, tile_c, child[4];
	uint32_t crc;

	/* a large synthetic plan of every type: no two ids meet, each id says its type */
	ids = (uint64_t *)malloc(capacity * sizeof(*ids));
	check(ids != NULL, "cache_ids_memory", "out of memory");
	if (!ids)
		return;
	for (a = 0; a < 400; a++)
		for (b = 0; b < 4; b++)
			for (c = 0; c < 8; c++)
				for (d = 0; d < 8; d++)
					ids[count++] = prism_bake_job_lightmap_tile(a, b, c, d);
	for (a = 0; a < 50; a++)
		for (b = 0; b < 4; b++)
			for (c = 0; c < 8; c++)
				for (d = 0; d < 8; d++)
					ids[count++] = prism_bake_job_sun_mask_tile(a, b, c, d);
	for (a = 0; a < 4; a++)
		for (b = 0; b < 16; b++)
			for (c = 0; c < 16; c++)
				for (d = 0; d < 16; d++)
				{
					ids[count++] = prism_bake_job_volume_brick(a, b, c, d);
					ids[count++] = prism_bake_job_importance_cell(a, b, c, d);
				}
	for (a = 0; a < 3; a++)
		for (k = 0; k < 20000; k++)
			ids[count++] = prism_bake_job_probe(a, k);
	for (k = 0; k < count; k++)
	{
		uint32_t type = prism_bake_job_type_of(ids[k]);

		if (type == PRISM_BAKE_JOB_TYPE_NONE || type >= NUMBER_OF_PRISM_BAKE_JOB_TYPES)
			wrong_type++;
	}
	qsort(ids, count, sizeof(*ids), job_compare);
	for (k = 1; k < count; k++)
		duplicates += ids[k] == ids[k - 1];
	/* (the bricks and the importance cells share every key: only the type tells them apart) */
	sprintf(why, "%lu ids, %lu duplicates, %lu with no type", (unsigned long)count, (unsigned long)duplicates,
		(unsigned long)wrong_type);
	check(count > 200000 && !duplicates && !wrong_type, "cache_ids_unique", why);
	free(ids);
	check(prism_bake_job_type_of(prism_bake_job_lightmap_tile(7, 2, 3, 4)) == PRISM_BAKE_JOB_TYPE_LIGHTMAP_TILE &&
		prism_bake_job_type_of(prism_bake_job_sun_mask_tile(7, 2, 3, 4)) == PRISM_BAKE_JOB_TYPE_SUN_MASK_TILE &&
		prism_bake_job_type_of(prism_bake_job_volume_brick(7, 2, 3, 4)) == PRISM_BAKE_JOB_TYPE_VOLUME_BRICK &&
		prism_bake_job_type_of(prism_bake_job_probe(7, 2)) == PRISM_BAKE_JOB_TYPE_PROBE &&
		prism_bake_job_type_of(prism_bake_job_importance_cell(7, 2, 3, 4)) == PRISM_BAKE_JOB_TYPE_IMPORTANCE_CELL &&
		prism_bake_job_volume_brick(7, 2, 3, 4) != prism_bake_job_importance_cell(7, 2, 3, 4),
		"cache_ids_types", "an id's type is not its own");
	/* (the documented bits, and the largest keys of each field) */
	check(prism_bake_job_lightmap_tile(5, 3, 7, 9) == ((1ULL << 56) | (5ULL << 32) | (3ULL << 28) | (9ULL << 14) | 7) &&
		prism_bake_job_lightmap_tile((1u << 24) - 1, 15, (1u << 14) - 1, (1u << 14) - 1) ==
		((1ULL << 56) | 0x00FFFFFFFFFFFFFFULL) &&
		prism_bake_job_lightmap_tile(1u << 24, 0, 0, 0) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_lightmap_tile(0, 16, 0, 0) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_lightmap_tile(0, 0, 1u << 14, 0) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_sun_mask_tile(1u << 16, 0, 0, 0) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_volume_brick(256, 0, 0, 0) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_probe(0, 1ULL << 48) == PRISM_BAKE_JOB_NONE, "cache_ids_bits",
		"the id's bits are not as documented, or a key out of range wraps");
	check(prism_bake_job_parent(prism_bake_job_lightmap_tile(5, 2, 7, 9)) == prism_bake_job_lightmap_tile(5, 1, 3, 4) &&
		prism_bake_job_parent(prism_bake_job_lightmap_tile(5, 0, 7, 9)) == PRISM_BAKE_JOB_NONE &&
		prism_bake_job_parent(prism_bake_job_sun_mask_tile(9, 1, 5, 6)) == prism_bake_job_sun_mask_tile(9, 0, 2, 3) &&
		prism_bake_job_parent(prism_bake_job_volume_brick(1, 2, 3, 4)) == PRISM_BAKE_JOB_NONE, "cache_ids_parent",
		"a tile's parent is not the tile it was refined from");

	/* a plan of generation 1: chart 1's two tiles and chart 2's, all done */
	sprintf(path, "%s/jobs.pbk", folder);
	key_make(&key);
	tile_a = prism_bake_job_lightmap_tile(1, 0, 0, 0);
	tile_b = prism_bake_job_lightmap_tile(1, 0, 1, 0);
	tile_c = prism_bake_job_lightmap_tile(2, 0, 0, 0);
	memset(data, 0x5A, sizeof(data));
	check(!prism_bake_cache_create(&writer, path, &key, 0), "cache_jobs_create", "it cannot be made");
	plan[0] = tile_a;
	plan[1] = tile_b;
	plan[2] = tile_c;
	check(!prism_bake_cache_append_plan(&writer, plan, 3) &&
		prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_PLAN, 0, NULL, 0) != 0, "cache_plan_write",
		"a plan is written, or a plan is appended without a generation");
	data[0] = 1;
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, tile_a, data, 8);
	data[0] = 2;
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, tile_b, data, 8);
	data[0] = 3;
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, tile_c, data, 8);
	prism_bake_cache_writer_close(&writer);
	prism_bake_cache_open(&cache, path, &key, 1);
	check(cache.plan_generation == 1 && cache.plan_count == 3 && prism_bake_cache_job_done(&cache,
		PRISM_BAKE_RECORD_ATLAS_TILE, tile_a) && prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_c) &&
		!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(3, 0, 0, 0)),
		"cache_plan_read", "the first plan's jobs are not done");
	prism_bake_cache_close(&cache);

	/* chart 1 refined: its tiles split, chart 2's stays; a result for a job
	nobody planned does not count */
	check(!prism_bake_cache_resume(&writer, &cache, path, &key, 0) && writer.generation == 1, "cache_jobs_resume",
		"the bake does not resume at its plan's generation");
	prism_bake_cache_close(&cache);
	child[0] = prism_bake_job_lightmap_tile(1, 1, 0, 0);
	child[1] = prism_bake_job_lightmap_tile(1, 1, 1, 0);
	child[2] = prism_bake_job_lightmap_tile(1, 1, 0, 1);
	child[3] = prism_bake_job_lightmap_tile(1, 1, 1, 1);
	for (k = 0; k < 4; k++)
		plan[k] = child[k];
	plan[4] = prism_bake_job_lightmap_tile(1, 1, 2, 0); /* the second tile's first child */
	plan[5] = tile_c;
	check(!prism_bake_cache_append_plan(&writer, plan, 6) && writer.generation == 2, "cache_refine_plan",
		"the refined plan is not generation 2");
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(9, 0, 0, 0), data, 8);
	prism_bake_cache_writer_close(&writer);
	prism_bake_cache_open(&cache, path, &key, 1);
	check(cache.plan_generation == 2 && cache.plan_count == 6 && prism_bake_cache_job_done(&cache,
		PRISM_BAKE_RECORD_ATLAS_TILE, tile_c), "cache_refine_keeps", "an unrelated chart's result is lost");
	sprintf(why, "the superseded tiles: %d %d, in plan %d, stray %d",
		prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_a),
		prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_b),
		prism_bake_cache_in_plan(&cache, tile_a),
		prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(9, 0, 0, 0)));
	check(!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_a) &&
		!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_b) &&
		!prism_bake_cache_in_plan(&cache, tile_a) && prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_a) &&
		!prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(9, 0, 0, 0)),
		"cache_superseded", why);

	/* the parent's result is the fallback of each unfinished child, until
	the child is done */
	record = prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, child[3]);
	check(record && record->job == tile_a && !prism_bake_cache_read(&cache, record, data, 8) && data[0] == 1 &&
		prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, plan[4]) != NULL &&
		prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, plan[4])->job == tile_b,
		"cache_fallback", "an unfinished child does not read its parent's texels");
	check(!prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile_c) &&
		!prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(7, 1, 0, 0)),
		"cache_fallback_none", "a finished tile, or a tile with no parent result, has a fallback");
	prism_bake_cache_close(&cache);
	prism_bake_cache_resume(&writer, &cache, path, &key, 0);
	prism_bake_cache_close(&cache);
	data[0] = 11;
	prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, child[3], data, 8);
	/* (a refinement of a refinement: the grandparent's result serves) */
	prism_bake_cache_writer_close(&writer);
	prism_bake_cache_open(&cache, path, &key, 1);
	check(prism_bake_cache_job_done(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, child[3]) &&
		!prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, child[3]) &&
		prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, child[0]) != NULL &&
		prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(1, 2, 3, 3)) != NULL &&
		prism_bake_cache_fallback(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, prism_bake_job_lightmap_tile(1, 2, 3, 3))->job ==
		child[3], "cache_fallback_done", "a finished child still falls back, or its own result is not the nearest");
	prism_bake_cache_close(&cache);

	/* a plan that does not follow the one before (a generation that is not the
	next) ends the log there, as a torn record does */
	check(!prism_bake_cache_resume(&writer, &cache, path, &key, 0), "cache_jobs_resume_again", "it does not resume");
	prism_bake_cache_close(&cache);
	writer.generation = 5;
	prism_bake_cache_append_plan(&writer, plan, 1);
	prism_bake_cache_writer_close(&writer);
	prism_bake_cache_open(&cache, path, &key, 1);
	check(cache.torn && cache.plan_generation == 2 && cache.plan_count == 6, "cache_plan_order",
		"a plan out of order is taken");
	prism_bake_cache_close(&cache);

	/* a file of the previous format (version 1) is not read, not resumed, and left as it was */
	file = fopen(path, "r+b");
	fread(header, sizeof(header), 1, file);
	header[8] = 1;
	crc = prism_bake_crc32(0, header, PRISM_BAKE_CACHE_HEADER_SIZE - 4);
	header[PRISM_BAKE_CACHE_HEADER_SIZE - 4] = (unsigned char)crc;
	header[PRISM_BAKE_CACHE_HEADER_SIZE - 3] = (unsigned char)(crc >> 8);
	header[PRISM_BAKE_CACHE_HEADER_SIZE - 2] = (unsigned char)(crc >> 16);
	header[PRISM_BAKE_CACHE_HEADER_SIZE - 1] = (unsigned char)(crc >> 24);
	fseek(file, 0, SEEK_SET);
	fwrite(header, sizeof(header), 1, file);
	fclose(file);
	a = (uint32_t)file_size(path);
	check(prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_INVALID && !cache.record_count &&
		!cache.plan_count, "cache_old_version", "a version 1 file is read");
	prism_bake_cache_close(&cache);
	check(prism_bake_cache_resume(&writer, &cache, path, &key, 0) != 0 && file_size(path) == (long)a,
		"cache_old_version_resume", "a version 1 file is resumed or changed");
	prism_bake_cache_close(&cache);
}

/* ---------- convergence */

typedef double (*sampler)(void);

static double sample_constant(void) { return 0.7; }
static double sample_zero(void) { return 0.0; }
static double sample_gaussian(void) { double x = 1.0 + 0.5 * gaussian(); return x > 0 ? x : 0; }
static double sample_rare(void) { return uniform() < 0.002 ? 500.0 : 0.0; } /* mean 1, a firefly per 500 */
static double sample_pareto(void) { return 0.6 * pow(uniform(), -1.0 / 2.5); } /* alpha 2.5: mean 1 */
static double sample_mixed(void) { return uniform() < 0.05 ? 10.0 * uniform() : 0.5 * uniform(); }
/* a dark texel's: mean 0.002, as sample_gaussian's spread */
static double sample_dark(void) { return 0.002 * sample_gaussian(); }

/* runs one texel to its stop: the samples taken, the estimate */
static enum prism_bake_converge_state run_texel(struct prism_bake_converge_parameters const *p, sampler source,
	uint32_t limit, uint32_t *taken, double *estimate)
{
	struct prism_bake_texel_stats stats;
	struct prism_bake_batch_stats batches;
	enum prism_bake_converge_state state = PRISM_BAKE_CONVERGE_RUNNING;

	prism_bake_texel_reset(&stats);
	memset(&batches, 0, sizeof(batches));
	while (stats.count < limit)
	{
		struct prism_bake_texel_stats batch;
		uint32_t k;

		prism_bake_texel_reset(&batch);
		for (k = 0; k < p->batch; k++)
			prism_bake_texel_add(&batch, (float)source());
		prism_bake_texel_merge(&stats, &batch);
		prism_bake_batch_add(&batches, batch.mean);
		state = prism_bake_texel_state(&stats, &batches, p);
		if (state != PRISM_BAKE_CONVERGE_RUNNING)
			break;
	}
	*taken = stats.count;
	*estimate = stats.mean;
	return state;
}

/* the share of texels (of trials) that stopped within the threshold of the
true mean, and their mean sample count */
static double coverage(struct prism_bake_converge_parameters const *p, sampler source, double truth, int trials,
	double *mean_samples)
{
	int t, within = 0;
	double samples = 0.0;

	for (t = 0; t < trials; t++)
	{
		uint32_t taken;
		double estimate;

		run_texel(p, source, 1u << 26, &taken, &estimate);
		within += fabs(estimate - truth) <= p->threshold * truth;
		samples += taken;
	}
	*mean_samples = samples / trials;
	return (double)within / trials;
}

static void test_converge(void)
{
	struct prism_bake_converge_parameters p;
	struct prism_bake_texel_stats a, b, all;
	char why[256];
	uint32_t taken, k;
	double estimate, samples, samples_half, share;

	/* merging is Welford's in any split */
	prism_bake_texel_reset(&a);
	prism_bake_texel_reset(&b);
	prism_bake_texel_reset(&all);
	for (k = 0; k < 10000; k++)
	{
		float x = (float)sample_mixed();

		prism_bake_texel_add(k < 3333 ? &a : &b, x);
		prism_bake_texel_add(&all, x);
	}
	prism_bake_texel_merge(&a, &b);
	sprintf(why, "merged %g %g, sequential %g %g", a.mean, prism_bake_texel_variance(&a), all.mean,
		prism_bake_texel_variance(&all));
	check(a.count == all.count && a.hits == all.hits && fabsf(a.mean - all.mean) < 1e-5f &&
		fabsf(prism_bake_texel_variance(&a) - prism_bake_texel_variance(&all)) < 1e-4f * prism_bake_texel_variance(&all) &&
		a.maximum == all.maximum, "converge_merge", why);

	prism_bake_converge_defaults(&p, 0.02f);
	check(p.maximum_samples == 0, "converge_no_budget", "the default has a sample budget");
	check(run_texel(&p, sample_constant, 1u << 20, &taken, &estimate) == PRISM_BAKE_CONVERGE_CONVERGED &&
		taken == p.minimum_samples, "converge_constant", "a constant texel does not stop at the minimum");
	check(run_texel(&p, sample_zero, 1u << 20, &taken, &estimate) == PRISM_BAKE_CONVERGE_BLACK &&
		taken == p.zero_samples, "converge_black", "a black texel does not stop at zero_samples");

	/* coverage: stopped estimates within the threshold of the truth (the
	rule is a 95% interval with optional stopping: at least 90% expected) */
	share = coverage(&p, sample_gaussian, 1.0, 400, &samples);
	sprintf(why, "%.1f%% within, %.0f samples", share * 100, samples);
	check(share >= 0.90, "converge_gaussian", why);
	printf("info gaussian: %.1f%% within 2%%, %.0f samples\n", share * 100, samples);
	share = coverage(&p, sample_pareto, 1.0, 400, &samples);
	sprintf(why, "%.1f%% within, %.0f samples", share * 100, samples);
	check(share >= 0.85, "converge_pareto", why);
	printf("info pareto 2.5: %.1f%% within 2%%, %.0f samples\n", share * 100, samples);
	share = coverage(&p, sample_rare, 1.0, 40, &samples);
	sprintf(why, "%.1f%% within, %.0f samples", share * 100, samples);
	check(share >= 0.85, "converge_rare", why);
	printf("info rare fireflies: %.1f%% within 2%%, %.0f samples\n", share * 100, samples);

	/* the threshold halved: about four times the samples */
	p.threshold = 0.04f;
	coverage(&p, sample_gaussian, 1.0, 200, &samples);
	p.threshold = 0.02f;
	coverage(&p, sample_gaussian, 1.0, 200, &samples_half);
	sprintf(why, "%.0f then %.0f samples", samples, samples_half);
	check(samples_half / samples > 3.0 && samples_half / samples < 5.5, "converge_threshold_scaling", why);

	/* a firefly just met is never stopped on */
	{
		struct prism_bake_texel_stats s;

		prism_bake_texel_reset(&s);
		for (k = 0; k < 100000; k++)
			prism_bake_texel_add(&s, 1.0f + 0.01f * (float)(k % 3));
		check(prism_bake_texel_state(&s, NULL, &p) == PRISM_BAKE_CONVERGE_CONVERGED, "converge_before_firefly",
			"a smooth texel does not stop");
		prism_bake_texel_add(&s, 5000.0f);
		check(prism_bake_texel_state(&s, NULL, &p) == PRISM_BAKE_CONVERGE_RUNNING, "converge_firefly",
			"a texel stops on the sample of a firefly");
	}
	/* too few hits */
	{
		struct prism_bake_texel_stats s;

		prism_bake_texel_reset(&s);
		for (k = 0; k < 1000; k++)
			prism_bake_texel_add(&s, k % 100 == 0 ? 1.0f : 0.0f);
		check(prism_bake_texel_state(&s, NULL, &p) == PRISM_BAKE_CONVERGE_RUNNING, "converge_hits",
			"a texel stops with 10 hits");
	}
	/* the batch means catch correlation inside a batch: each batch's
	samples share one random offset */
	{
		struct prism_bake_texel_stats s;
		struct prism_bake_batch_stats bs;
		uint32_t batch;
		int stopped_plain = 0, stopped_batched = 0;

		prism_bake_texel_reset(&s);
		memset(&bs, 0, sizeof(bs));
		for (batch = 0; batch < 64; batch++)
		{
			struct prism_bake_texel_stats one;
			double offset = 0.3 * gaussian();

			prism_bake_texel_reset(&one);
			for (k = 0; k < p.batch; k++)
				prism_bake_texel_add(&one, (float)(1.0 + offset + 0.05 * gaussian()));
			prism_bake_texel_merge(&s, &one);
			prism_bake_batch_add(&bs, one.mean);
		}
		stopped_plain = prism_bake_texel_state(&s, &bs, &p) == PRISM_BAKE_CONVERGE_CONVERGED;
		p.use_batch_means = 1;
		stopped_batched = prism_bake_texel_state(&s, &bs, &p) == PRISM_BAKE_CONVERGE_CONVERGED;
		p.use_batch_means = 0;
		check(stopped_plain && !stopped_batched, "converge_batch_means",
			"the batch means do not hold back a correlated texel");
	}
	/* D4 amended: the noise floor (error_floor) stops a dark texel once its
	standard error is under the threshold of the floor, not of its own light;
	0 the literal rule */
	{
		struct prism_bake_texel_stats s;
		double literal_samples, floor_samples;

		check(p.error_floor == 0.0f, "converge_floor_default", "the literal rule is not the default");
		coverage(&p, sample_dark, 0.002, 100, &literal_samples);
		p.error_floor = 0.018f;
		coverage(&p, sample_dark, 0.002, 100, &floor_samples);
		sprintf(why, "%.0f samples literal, %.0f with the floor", literal_samples, floor_samples);
		check(floor_samples < literal_samples / 10.0, "converge_floor_dark", why);
		printf("info noise floor: a texel at 0.002, %.0f samples literal, %.0f with the floor\n", literal_samples,
			floor_samples);
		/* (a bright texel: the floor changes nothing) */
		prism_bake_texel_reset(&s);
		for (k = 0; k < 100000; k++)
			prism_bake_texel_add(&s, 1.0f + 0.01f * (float)(k % 3));
		check(prism_bake_texel_state(&s, NULL, &p) == PRISM_BAKE_CONVERGE_CONVERGED, "converge_floor_bright",
			"a bright texel does not stop with the floor");
		p.error_floor = 0.0f;
	}
	/* a budget, when one is set */
	p.maximum_samples = 256;
	check(run_texel(&p, sample_rare, 1u << 20, &taken, &estimate) == PRISM_BAKE_CONVERGE_BUDGET && taken == 256,
		"converge_budget", "a budget set is not kept");
	p.maximum_samples = 0;
	check(prism_bake_bounces_converged(0.5, 100.0, 0.01f) && !prism_bake_bounces_converged(2.0, 100.0, 0.01f) &&
		prism_bake_bounces_converged(0.0, 0.0, 0.01f), "converge_bounces", "the bounces' rule");
	{
		struct prism_bake_texel_stats many[3];

		prism_bake_texel_reset(&many[0]);
		prism_bake_texel_reset(&many[1]);
		prism_bake_texel_reset(&many[2]);
		for (k = 0; k < 64; k++)
			prism_bake_texel_add(&many[0], 1.0f);
		check(prism_bake_converge_count(many, NULL, 3, &p) == 1, "converge_count", "the stopped count");
	}
}

/* the irradiance volume's batches as xgpu_vulkan_bake.c runs them, with its
four readback slots collected in their order once the frame they were
recorded in is done, lag frames later: while the game draws, two or three
counts of a batch are in flight and come back 0 together (b30's live bake:
the results asked twice, every other batch skipped, the last past the
probes' end). Each item must be written once, in its batch, and no batch
past the end. */
struct batch_slot
{
	int busy;
	uint32_t frame, batch, finished, running, first;
};

static int batches_run(uint32_t total, uint32_t size, uint32_t lag, uint32_t work, uint32_t starve, char *why)
{
	struct prism_bake_batches batches;
	struct batch_slot slots[4];
	unsigned char *written = (unsigned char *)calloc(total ? total : 1, 1);
	uint32_t frame, index, dispatched = 0, k;
	int ok = 1;

	if (!written)
		return 0;
	memset(slots, 0, sizeof(slots));
	prism_bake_batches_start(&batches, total, size);
	for (frame = 1; frame < 100000 && batches.done < total && ok; frame++)
	{
		/* (the readbacks of done frames, in slot order) */
		for (index = 0; index < 4 && ok; index++)
		{
			struct batch_slot *r = &slots[index];

			if (!r->busy || r->frame + lag > frame)
				continue;
			if (!r->finished)
			{
				uint32_t count = prism_bake_batches_counted(&batches, r->batch, r->running);

				if (count)
				{
					/* (a data slot: the first free, maybe one after this one's) */
					struct batch_slot *data = NULL;

					for (k = 0; k < 4 && !data; k++)
						if (!slots[k].busy && !(starve && frame % starve == 0))
							data = &slots[k];
					if (!data)
						batches.asked = 0;
					else
					{
						data->busy = 1;
						data->frame = frame;
						data->batch = r->batch;
						data->finished = count;
						data->first = batches.done;
					}
				}
			}
			else
			{
				int64_t at = prism_bake_batches_finished(&batches, r->batch, r->finished);

				if (at >= 0)
				{
					if ((uint32_t)at != r->first || (uint64_t)at + r->finished > total)
					{
						sprintf(why, "results of %u at %ld (asked at %u)", r->finished, (long)at, r->first);
						ok = 0;
					}
					for (k = 0; ok && k < r->finished; k++)
						if (written[at + k]++)
						{
							sprintf(why, "item %ld written twice", (long)(at + k));
							ok = 0;
						}
				}
			}
			r->busy = 0;
		}
		/* (the frame's slice: a running batch's samples and count, or the
		next batch) */
		if (batches.batched > batches.done)
		{
			uint32_t number = prism_bake_batches_counting(&batches);
			struct batch_slot *r = NULL;

			if (!number)
				continue;
			for (k = 0; k < 4 && !r; k++)
				if (!slots[k].busy)
					r = &slots[k];
			if (!r)
				continue;
			dispatched++;
			r->busy = 1;
			r->frame = frame;
			r->batch = number;
			r->finished = 0;
			r->running = dispatched >= work ? 0 : work - dispatched;
		}
		else if (prism_bake_batches_begin(&batches))
			dispatched = 0;
	}
	for (index = 0; ok && index < total; index++)
		if (written[index] != 1)
		{
			sprintf(why, "item %u written %u times (done %u of %u)", index, written[index], batches.done, total);
			ok = 0;
		}
	free(written);
	return ok;
}

static void test_batches(void)
{
	struct prism_bake_batches batches;
	char why[256], name[64];
	uint32_t lag, work;

	/* b30's volume in small: a last batch shorter than the rest */
	for (lag = 1; lag <= 3; lag++)
		for (work = 1; work <= 4; work++)
		{
			why[0] = 0;
			sprintf(name, "batches_lag%u_work%u", lag, work);
			check(batches_run(64u * 31u + 34u, 64u, lag, work, 0, why), name, why);
		}
	why[0] = 0;
	check(batches_run(64u * 7u, 64u, 3, 2, 3, why), "batches_no_slot", why);
	why[0] = 0;
	check(batches_run(0, 64u, 2, 1, 0, why), "batches_none", why);
	/* (a count of another batch, or results not asked for, let go) */
	prism_bake_batches_start(&batches, 100, 40);
	check(prism_bake_batches_begin(&batches) == 40 && prism_bake_batches_begin(&batches) == 0 &&
		prism_bake_batches_counted(&batches, 2, 0) == 0 && prism_bake_batches_finished(&batches, 1, 40) < 0 &&
		prism_bake_batches_counted(&batches, 1, 5) == 0 && prism_bake_batches_counted(&batches, 1, 0) == 40 &&
		prism_bake_batches_counting(&batches) == 0 && prism_bake_batches_counted(&batches, 1, 0) == 0 &&
		prism_bake_batches_finished(&batches, 1, 40) == 0 && prism_bake_batches_finished(&batches, 1, 40) < 0 &&
		batches.done == 40 && prism_bake_batches_begin(&batches) == 40 && batches.number == 2,
		"batches_stale", "a stale count or result taken");
	prism_bake_batches_start(&batches, 100, 40);
	prism_bake_batches_skip(&batches, 64);
	check(batches.done == 64 && prism_bake_batches_begin(&batches) == 36 && prism_bake_batches_begin(&batches) == 0,
		"batches_skip", "the file's items skipped");
}

/* ---------- the robust mean and the filter (prism_bake_filter.c) */

/* a texel's samples as the bake takes them: count of them, each from
source, their groups' sums, mean and largest */
static void robust_texel(double (*source)(void), uint32_t count, float sums[PRISM_BAKE_GROUPS], double *mean,
	float *largest)
{
	uint32_t k;
	double total = 0.0;

	memset(sums, 0, PRISM_BAKE_GROUPS * sizeof(float));
	*largest = 0.0f;
	for (k = 0; k < count; k++)
	{
		double x = source();

		sums[(k / PRISM_BAKE_GROUP_BATCH) % PRISM_BAKE_GROUPS] += (float)x;
		total += x;
		if ((float)x > *largest)
			*largest = (float)x;
	}
	*mean = total / count;
}

static double source_normal(void)
{
	double x = 1.0 + 0.3 * gaussian();

	return x > 0.0 ? x : 0.0;
}

/* a lamp's light, and rarely a path that meets a small bright lamp: a
tenth of the light in one sample of 10,000 (mean 1.1) */
static double source_firefly(void)
{
	return 1.0 + 0.2 * gaussian() + (uniform() < 1.0e-4 ? 1000.0 : 0.0);
}

/* a heavy tail (Pareto, shape 2.5, mean 5/3) */
static double source_pareto(void)
{
	return pow(uniform(), -1.0 / 2.5);
}

static void filter_flat(struct prism_bake_filter_texel *t, float light, float variance, int32_t chart, float nz)
{
	memset(t, 0, sizeof(*t));
	t->light[0] = t->light[1] = t->light[2] = light;
	t->indirect[0] = t->indirect[1] = t->indirect[2] = 0.5f * light;
	t->level = 0.2126f * light + 0.7152f * light + 0.0722f * light;
	t->variance = variance;
	t->normal[2] = nz;
	t->normal[0] = nz < 1.0f ? (float)sqrt(1.0 - nz * nz) : 0.0f;
	t->chart = chart;
}

static void test_filter(void)
{
	struct prism_bake_robust_parameters rp;
	struct prism_bake_filter_parameters fp;
	static struct prism_bake_filter_texel window[48 * 48], scratch[2 * 48 * 48], world[64 * 64];
	struct prism_bake_filter_texel out[256], out2[256];
	float sums[PRISM_BAKE_GROUPS], largest, variance, factor;
	double mean, before, after, sum_out, m;
	uint32_t g, total, k;
	int trials, triggered, x, y, ok;

	prism_bake_robust_defaults(&rp);
	/* the groups' samples */
	for (ok = 1, k = 0; k < 3000; k += 37)
	{
		for (total = 0, g = 0; g < PRISM_BAKE_GROUPS; g++)
			total += prism_bake_group_samples(k, g);
		ok &= total == k;
	}
	check(ok && prism_bake_group_samples(2048, 3) == 256 && prism_bake_group_samples(100, 0) == 64 &&
		prism_bake_group_samples(100, 1) == 36 && prism_bake_group_samples(100, 2) == 0, "robust_groups",
		"the groups' samples");
	/* without a firefly the mean stands (almost always) */
	for (trials = 0, triggered = 0; trials < 2000; trials++)
	{
		robust_texel(source_normal, 2048, sums, &mean, &largest);
		variance = 1.0f;
		triggered += prism_bake_robust_factor(sums, 2048, largest, &variance, &rp) != 1.0f;
	}
	check(triggered <= 10, "robust_quiet", "the mean replaced where no firefly was");
	/* a texel at the budget with a firefly in it: taken near its light
	without the firefly, and with less noise than the mean's */
	for (trials = 0, triggered = 0, before = after = 0.0; trials < 2000; trials++)
	{
		robust_texel(source_firefly, 512, sums, &mean, &largest);
		factor = prism_bake_robust_factor(sums, 512, largest, NULL, &rp);
		triggered += factor != 1.0f;
		before += (mean - 1.1) * (mean - 1.1);
		after += (mean * factor - 1.1) * (mean * factor - 1.1);
	}
	check(triggered > 50 && triggered < 200 && after < 0.25 * before, "robust_firefly", "the fireflies' texels not taken in");
	/* unbiased in the limit: a heavy tail's texels' mean light over many
	trials, at a budget and at 32 times it */
	for (k = 2048; k <= 65536; k *= 32)
	{
		for (trials = 0, m = 0.0; trials < 400; trials++)
		{
			robust_texel(source_pareto, k, sums, &mean, &largest);
			m += mean * prism_bake_robust_factor(sums, k, largest, NULL, &rp);
		}
		m /= 400.0;
		if (k == 2048)
			before = fabs(m / (5.0 / 3.0) - 1.0);
		else
			after = fabs(m / (5.0 / 3.0) - 1.0);
	}
	check(after < 0.01 && after <= before + 0.002, "robust_limit", "the robust mean's bias does not fall away");

	/* the filter: a flat chart's noise falls, its light stays */
	prism_bake_filter_defaults(&fp, PRISM_BAKE_QUALITY_ULTRA);
	check(fp.passes == 3, "filter_passes", "three passes under max");
	prism_bake_filter_defaults(&fp, PRISM_BAKE_QUALITY_MAX);
	check(fp.passes == 1, "filter_passes_max", "max's mildest");
	prism_bake_filter_defaults(&fp, PRISM_BAKE_QUALITY_HIGH);
	for (k = 0; k < 48 * 48; k++)
		filter_flat(&window[k], (float)(1.0 + 0.1 * gaussian()), 0.01f, 7, 1.0f);
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (before = after = sum_out = 0.0, k = 0; k < 256; k++)
	{
		double in = window[(16 + k / 16) * 48 + 16 + k % 16].light[0] - 1.0;

		before += in * in;
		after += (out[k].light[0] - 1.0) * (out[k].light[0] - 1.0);
		sum_out += out[k].light[0];
	}
	check(after < before / 8.0 && fabs(sum_out / 256.0 - 1.0) < 0.02 && out[0].variance < 0.01f / 8.0f,
		"filter_flat", "a flat chart's noise not reduced, or its light moved");
	/* a firefly's texel (its level its groups' median, its light as
	sampled): its light spread over its neighbours, not lost */
	for (k = 0; k < 48 * 48; k++)
		filter_flat(&window[k], 1.0f, 1.0e-4f, 7, 1.0f);
	window[24 * 48 + 24].light[0] = window[24 * 48 + 24].light[1] = window[24 * 48 + 24].light[2] = 50.0f;
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (sum_out = 0.0, m = 0.0, k = 0; k < 256; k++)
	{
		sum_out += out[k].light[0];
		m = out[k].light[0] > m ? out[k].light[0] : m;
	}
	check(fabs(sum_out - (256.0 + 49.0)) < 3.0 && m < 5.0, "filter_firefly", "a firefly's light lost or kept in place");
	/* an edge the samples resolve stays: half the chart at 1, half at 4,
	each with 1% noise */
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			filter_flat(&window[y * 48 + x], (float)((x < 24 ? 1.0 : 4.0) * (1.0 + 0.01 * gaussian())),
				(float)(1.0e-4 * (x < 24 ? 1.0 : 16.0)), 7, 1.0f);
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (ok = 1, y = 0; y < 16; y++)
		ok &= fabs(out[y * 16 + 7].light[0] - 1.0) < 0.02 && fabs(out[y * 16 + 8].light[0] - 4.0) < 0.08;
	check(ok, "filter_edge", "an edge the samples resolve blurred");
	/* never across a chart's edge, nor a turn of the face; a texel of
	chart -1 is neither read nor changed */
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			filter_flat(&window[y * 48 + x], x < 24 ? 1.0f : 1.5f, 1.0f, x < 24 ? 3 : 4, 1.0f);
	window[20 * 48 + 20].chart = -1;
	window[20 * 48 + 20].light[0] = 100.0f;
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (ok = 1, k = 0; k < 256; k++)
		ok &= k == 4 * 16 + 4 ? out[k].light[0] == 100.0f : out[k].light[0] == (k % 16 < 8 ? 1.0f : 1.5f);
	check(ok, "filter_chart", "light taken across a chart's edge or from a texel not baked");
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			filter_flat(&window[y * 48 + x], x < 24 ? 1.0f : 1.5f, 1.0f, 3, x < 24 ? 1.0f : 0.0f);
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (ok = 1, k = 0; k < 256; k++)
		ok &= out[k].light[0] == (k % 16 < 8 ? 1.0f : 1.5f);
	check(ok, "filter_normal", "light taken across a turn of the face");
	/* no seams: a texel's result does not depend on where the tiles fall
	(the same texel from two windows 8 texels apart) */
	for (k = 0; k < 64 * 64; k++)
		filter_flat(&world[k], (float)(1.0 + 0.2 * gaussian() + ((k % 64) > 30 ? 0.5 : 0.0)), 0.01f, 1, 1.0f);
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			window[y * 48 + x] = world[y * 64 + x];
	prism_bake_filter_tile(window, out, scratch, &fp);
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			window[y * 48 + x] = world[(y + 8) * 64 + x + 8];
	prism_bake_filter_tile(window, out2, scratch, &fp);
	for (ok = 1, y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			ok &= memcmp(&out[(y + 8) * 16 + x + 8], &out2[y * 16 + x], sizeof(out[0])) == 0;
	check(ok, "filter_seams", "a texel's result depends on its tile's place");
	/* the normals' 32 bits */
	for (ok = 1, k = 0; k < 1000; k++)
	{
		float n[3], back[3], l;

		n[0] = (float)gaussian();
		n[1] = (float)gaussian();
		n[2] = (float)gaussian();
		l = (float)sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		n[0] /= l;
		n[1] /= l;
		n[2] /= l;
		prism_bake_normal_unpack(prism_bake_normal_pack(n), back);
		ok &= n[0] * back[0] + n[1] * back[1] + n[2] * back[2] > 0.99999f;
	}
	check(ok, "filter_normals", "a normal's 32 bits");
}


/* ---------- the packing (prism_bake_pack.h) and packed records */

static void test_pack(char const *folder)
{
	enum { WORDS = 4096 };
	uint32_t *words = (uint32_t *)malloc(WORDS * 4), *back = (uint32_t *)malloc(WORDS * 4 + 64);
	unsigned char *packed = (unsigned char *)malloc(prism_bake_pack_bound(WORDS * 4));
	size_t size, k, stride;
	int ok = 1, damaged_refused = 1;
	struct prism_bake_cache_key key;
	struct prism_bake_cache_writer writer;
	struct prism_bake_cache cache;
	struct prism_bake_record const *record;
	char path[1024];

	/* (a smooth light: RGB9E5 words of a gradient, as a tile's) */
	for (k = 0; k < WORDS; k++)
	{
		float rgb[3];

		rgb[0] = 0.2f + 0.0001f * (float)k;
		rgb[1] = 0.3f + 0.00005f * (float)(k % 97);
		rgb[2] = 0.1f;
		words[k] = prism_bake_rgb9e5(rgb);
	}
	for (stride = 1; stride <= 8; stride *= 2)
	{
		size = prism_bake_pack(words, WORDS * 4, (uint32_t)stride, packed, prism_bake_pack_bound(WORDS * 4));
		ok &= size > 0 && prism_bake_unpacked_size(packed, size) == WORDS * 4 &&
			prism_bake_unpack(packed, size, back, WORDS * 4 + 64) == WORDS * 4 && !memcmp(back, words, WORDS * 4);
	}
	check(ok, "pack_round_trip", "a payload packed by each stride unpacks to itself");
	size = prism_bake_pack(words, WORDS * 4, 4, packed, prism_bake_pack_bound(WORDS * 4));
	check(size > 0 && size < WORDS * 4 / 3, "pack_smooth", "a smooth tile's words pack to under a third");
	/* (noise: does not pack, but round-trips; an odd size, a tail of bytes) */
	for (k = 0; k < WORDS; k++)
		words[k] = (uint32_t)(uniform() * 4294967295.0);
	size = prism_bake_pack(words, WORDS * 4 - 3, 4, packed, prism_bake_pack_bound(WORDS * 4));
	check(size > 0 && prism_bake_unpack(packed, size, back, WORDS * 4) == WORDS * 4 - 3 &&
		!memcmp(back, words, WORDS * 4 - 3), "pack_noise", "noise and a tail of bytes round-trip");
	/* (damaged: every byte changed in turn, past the header, is refused or
	unpacks within its bounds; a short block is refused) */
	for (k = 0; k < WORDS; k++)
		words[k] = (uint32_t)(k / 7);
	size = prism_bake_pack(words, WORDS * 4, 4, packed, prism_bake_pack_bound(WORDS * 4));
	for (k = PRISM_BAKE_PACK_HEADER; k < size; k++)
	{
		unsigned char was = packed[k];
		size_t got;

		packed[k] = (unsigned char)(was ^ 0x5a);
		got = prism_bake_unpack(packed, size, back, WORDS * 4);
		damaged_refused &= got == 0 || got == WORDS * 4;
		packed[k] = was;
	}
	/* (cut short: refused, or (its last token an empty one) the same payload) */
	k = prism_bake_unpack(packed, size - 1, back, WORDS * 4);
	damaged_refused &= k == 0 || (k == WORDS * 4 && !memcmp(back, words, WORDS * 4));
	damaged_refused &= prism_bake_unpack(packed, size / 2, back, WORDS * 4) == 0;
	damaged_refused &= prism_bake_unpack(packed, size, back, WORDS * 4 - 1) == 0;
	check(damaged_refused, "pack_damaged", "a damaged or short block refused, never past its buffers");
	/* (a packed record: read back whole, its size the payload's, its
	stored bytes fewer; a plain record beside it) */
	key_make(&key);
	snprintf(path, sizeof(path), "%s/packed.pbk", folder);
	ok = prism_bake_cache_create(&writer, path, &key, 0) == 0 &&
		prism_bake_cache_append_packed(&writer, PRISM_BAKE_RECORD_ATLAS_TILE, 77, words, WORDS * 4, 4) == 0 &&
		prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_ATLAS_LAYERS, 77, "plain", 5) == 0 &&
		prism_bake_cache_writer_close(&writer) == 0;
	ok &= prism_bake_cache_open(&cache, path, &key, 1) == PRISM_BAKE_CACHE_CURRENT;
	record = prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_ATLAS_TILE, 77);
	memset(back, 0, WORDS * 4);
	ok &= record && record->size == WORDS * 4 && record->stored < record->size &&
		prism_bake_cache_read(&cache, record, back, WORDS * 4) == 0 && !memcmp(back, words, WORDS * 4);
	record = prism_bake_cache_find(&cache, PRISM_BAKE_RECORD_ATLAS_LAYERS, 77);
	ok &= record && record->size == 5 && record->stored == 5;
	prism_bake_cache_close(&cache);
	check(ok, "pack_record", "a packed record read back as its payload, a plain one beside it");
	free(words);
	free(back);
	free(packed);
}

/* ---------- the layered bake's recombining (prism_bake_layers.h) */

static void test_layers(void)
{
	struct prism_bake_layer_light base, live;
	struct prism_bake_layer_weights weights;
	uint8_t bytes[4 * PRISM_BAKE_LAYER_BYTES];
	float layers[PRISM_BAKE_LAYER_SHARES][3], total[3], indirect[3], t[3], i[3];
	uint32_t file[4 * 4], raster[4 * 2], pool[4], words[PRISM_BAKE_LAYER_BASE_WORDS];
	int k, ok = 1;

	memset(&base, 0, sizeof(base));
	for (k = 0; k < 3; k++)
	{
		base.sun[k] = 1.5f;
		base.sky[k] = 0.4f + 0.1f * (float)k;
		base.domes[k] = 0.2f;
	}
	base.fixtures = 1.0f;
	base.emission = 3.0f;
	base.emissive_scale = 1.0f;
	base.stock = 1;
	prism_bake_layer_base_write(&base, words);
	ok = prism_bake_layer_base_read(words, PRISM_BAKE_LAYER_BASE_WORDS, &live) == 0 && live.sky[2] == base.sky[2] &&
		live.emission == 3.0f && live.stock == 1;
	check(ok, "layers_base", "the base's record read back");
	/* (a texel: the sun's bounce 0.2, the sky 0.3, the domes 0.05, the
	fixtures 0.1, the lamps 0.35 of 1.0; its indirect the sun's and sky's and
	half the rest) */
	for (k = 0; k < 3; k++)
	{
		total[k] = 1.0f;
		layers[0][k] = 0.2f;
		layers[1][k] = 0.3f;
		layers[2][k] = 0.05f;
		layers[3][k] = 0.1f;
		indirect[k] = 0.5f + 0.25f;
	}
	prism_bake_layer_shares_put(total, (const float (*)[3])layers, 1, bytes, 0, 4);
	prism_bake_layer_weights_make(&base, &live, &weights);
	check(prism_bake_layers_identity(&weights), "layers_identity", "the base's own light: every weight 1");
	memcpy(t, total, sizeof(t));
	memcpy(i, indirect, sizeof(i));
	prism_bake_layer_texel(&weights, bytes, 0, 4, t, i);
	check(fabsf(t[0] - 1.0f) < 1.0e-6f && fabsf(i[0] - 0.75f) < 1.0e-6f, "layers_identity_texel",
		"at weights of 1 a texel is its light as baked");
	/* (the sky doubled: the total gains the sky's share, the indirect too) */
	live.sky[0] = base.sky[0] * 2.0f;
	live.sky[1] = base.sky[1] * 2.0f;
	live.sky[2] = base.sky[2] * 2.0f;
	prism_bake_layer_weights_make(&base, &live, &weights);
	memcpy(t, total, sizeof(t));
	memcpy(i, indirect, sizeof(i));
	prism_bake_layer_texel(&weights, bytes, 0, 4, t, i);
	ok = fabsf(t[1] - (1.0f + 77.0f / 255.0f)) < 2.0e-3f && fabsf(i[1] - (0.75f + 77.0f / 255.0f)) < 2.0e-3f;
	check(ok, "layers_sky", "the sky doubled adds its share to the light and the indirect");
	/* (Tool's flat fill: the sky's share gone, the indoor floor added) */
	live = base;
	live.flat = 1;
	live.floor_indoor[0] = live.floor_indoor[1] = live.floor_indoor[2] = 0.07f;
	live.floor_outdoor[0] = live.floor_outdoor[1] = live.floor_outdoor[2] = 0.5f;
	prism_bake_layer_weights_make(&base, &live, &weights);
	memcpy(t, total, sizeof(t));
	memcpy(i, indirect, sizeof(i));
	prism_bake_layer_texel(&weights, bytes, 0, 4, t, i);
	ok = fabsf(t[0] - (1.0f - 77.0f / 255.0f + 0.07f)) < 2.0e-3f && fabsf(i[0] - (0.75f - 77.0f / 255.0f)) < 2.0e-3f;
	check(ok, "layers_flat", "Tool's fill: the sky's light out, the indoor floor in (not in the indirect)");
	/* (the lamps' brightness: display.prism_rt_emissive_scale 2) */
	live = base;
	live.emissive_scale = 2.0f;
	prism_bake_layer_weights_make(&base, &live, &weights);
	memcpy(t, total, sizeof(t));
	memcpy(i, indirect, sizeof(i));
	prism_bake_layer_texel(&weights, bytes, 0, 4, t, i);
	check(fabsf(t[2] - (1.0f + 88.0f / 255.0f)) < 2.0e-3f, "layers_lamps", "the lamps doubled add their share");
	/* (a tile's words: identity copies them; a change recombines) */
	memset(file, 0, sizeof(file));
	file[0] = prism_bake_rgb9e5(total);
	file[1] = 0x80808080u;
	file[2] = prism_bake_rgb9e5(indirect);
	file[3] = 1u << 24;
	prism_bake_layers_tile(&weights, file, bytes, 4, raster, pool);
	prism_bake_rgb9e5_decode(raster[0], t);
	ok = raster[1] == 0x80808080u && fabsf(t[0] - (1.0f + 88.0f / 255.0f)) < 1.0e-2f && raster[2] == 0 && pool[1] == 0;
	prism_bake_layer_weights_identity(&weights);
	prism_bake_layers_tile(&weights, file, bytes, 4, raster, pool);
	ok &= raster[0] == file[0] && pool[0] == file[2];
	check(ok, "layers_tile", "a tile recombined; at weights of 1 its words as they are");
	(void)live;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: test_bake <scratch folder>\n");
		return 2;
	}
	test_reach();
	test_atlas();
	test_atlas_tiles();
	test_cache(argv[1]);
	test_cache_jobs(argv[1]);
	test_converge();
	test_batches();
	test_filter();
	test_pack(argv[1]);
	test_layers();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
