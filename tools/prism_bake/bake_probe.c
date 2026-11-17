/*
BAKE_PROBE.C

The baker's host libraries run on one structure bsp of the player's own
map, offline (tools/prism_bake/bake_scene.py writes the scene and draws
what this writes): the reachable space, each triangle's importance, the
floors the walk did not reach, and the atlas at the qualities asked for.

	bake_probe <scene.bin> <out folder> [--flight 0|1] [--density <tier 0 texels per wu>]... [--tile 8|16|32]

The scene (bake_scene.py's, little endian):
	"PBSC", version 2
	world bounds: 6 floats (x0 x1 y0 y1 z0 z1)
	flight: int32 (the scenario has a flying vehicle)
	water planes: count, then 4 floats each
	bsp3d: node count, nodes (3 int32 each); plane count, planes (4 floats);
		collision leaf count
	structure leaves: count, then (cluster int16, reference count int16,
		first reference int32)
	surface references: count, then surface int32
	clusters: count, pvs words per cluster, the pvs (count x words uint32)
	seeds: count, then (kind int32, position 3 floats)
	lightmap pages: count, then (width int32, height int32)
	triangles: count, then (surface int32, page int32, positions 9 floats,
		UVs 6 floats); oriented to their vertices' normals

Out:
	voxels.bin, flight.bin  (x, y, z int32, flags uint8) per voxel
	tiers.bin               one byte per triangle
	unreached.bin           per floor triangle the walk missed: (triangle int32, area float)
	summary.json
*/

#include "prism_bake_atlas.h"
#include "prism_bake_reach.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *scene;
static size_t scene_size, scene_at;

static void fail(char const *why)
{
	fprintf(stderr, "bake_probe: %s\n", why);
	exit(1);
}

static void const *take(size_t size)
{
	void const *p;

	if (size > scene_size - scene_at)
		fail("the scene is cut short");
	p = scene + scene_at;
	scene_at += size;
	return p;
}

static int32_t take32(void)
{
	int32_t value;

	memcpy(&value, take(4), 4);
	return value;
}

static int32_t take_count(size_t element)
{
	int32_t count = take32();

	if (count < 0 || (size_t)count > (scene_size - scene_at) / (element ? element : 1))
		fail("a count is out of range");
	return count;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void write_voxels(char const *path, struct prism_bake_voxels const *set)
{
	FILE *file = fopen(path, "wb");
	size_t slot;

	if (!file)
		fail("cannot write the voxels");
	for (slot = 0; slot < set->capacity; slot++)
	{
		int32_t xyz[3];
		uint8_t flags = prism_bake_voxels_slot(set, slot, xyz);

		if (flags)
		{
			fwrite(xyz, 4, 3, file);
			fwrite(&flags, 1, 1, file);
		}
	}
	fclose(file);
}

int main(int argc, char **argv)
{
	struct prism_bake_bsp3d bsp;
	struct prism_bake_clusters clusters;
	struct prism_bake_reach_parameters rp;
	struct prism_bake_reach reach;
	struct prism_bake_importance_parameters ip;
	struct prism_bake_importance_counts ic;
	struct prism_bake_seed *seeds;
	struct prism_bake_atlas_page *pages;
	struct prism_bake_atlas_triangle *atlas_triangles;
	float bounds[6], *positions, densities[8];
	int32_t *surfaces, *references, *leaf_first, flight, seed_count, page_count, k, density_count = 0;
	int16_t *leaf_clusters, *leaf_counts;
	uint8_t *tiers;
	size_t triangle_count, t;
	char path[2048];
	FILE *file, *out;
	double started, reach_seconds, importance_seconds;
	double floor_area = 0, floor_reached = 0, unreached_area_near = 0;
	size_t floor_triangles = 0, floor_triangles_reached = 0, seeds_walked = 0;
	int flight_override = -1, tile_override = -1, a;
	int32_t water_count;
	float const *water;

	if (argc < 3)
		fail("usage: bake_probe <scene.bin> <out folder> [--flight 0|1] [--density <texels per wu>]... [--tile 8|16|32]");
	for (a = 3; a + 1 < argc; a += 2)
	{
		if (!strcmp(argv[a], "--flight"))
			flight_override = atoi(argv[a + 1]);
		else if (!strcmp(argv[a], "--density") && density_count < 8)
			densities[density_count++] = (float)atof(argv[a + 1]);
		else if (!strcmp(argv[a], "--tile"))
			tile_override = atoi(argv[a + 1]);
	}
	if (!density_count)
		densities[density_count++] = 16.0f;
	file = fopen(argv[1], "rb");
	if (!file)
		fail("cannot read the scene");
	fseek(file, 0, SEEK_END);
	scene_size = (size_t)ftell(file);
	fseek(file, 0, SEEK_SET);
	scene = (unsigned char *)malloc(scene_size);
	if (!scene || fread(scene, 1, scene_size, file) != scene_size)
		fail("cannot read the scene");
	fclose(file);
	if (memcmp(take(4), "PBSC", 4) || take32() != 2)
		fail("not a scene of version 2");
	memcpy(bounds, take(24), 24);
	flight = take32();
	if (flight_override >= 0)
		flight = flight_override;
	water_count = take_count(16);
	water = (float const *)take((size_t)water_count * 16);
	bsp.node_count = take_count(12);
	bsp.nodes = (int32_t const *)take((size_t)bsp.node_count * 12);
	bsp.plane_count = take_count(16);
	bsp.planes = (float const *)take((size_t)bsp.plane_count * 16);
	bsp.leaf_count = take32();
	memset(&clusters, 0, sizeof(clusters));
	clusters.leaf_count = take_count(8);
	leaf_clusters = (int16_t *)malloc((size_t)(clusters.leaf_count + 1) * 2);
	leaf_counts = (int16_t *)malloc((size_t)(clusters.leaf_count + 1) * 2);
	leaf_first = (int32_t *)malloc((size_t)(clusters.leaf_count + 1) * 4);
	for (k = 0; k < clusters.leaf_count; k++)
	{
		unsigned char const *p = (unsigned char const *)take(8);

		memcpy(&leaf_clusters[k], p, 2);
		memcpy(&leaf_counts[k], p + 2, 2);
		memcpy(&leaf_first[k], p + 4, 4);
	}
	clusters.leaf_clusters = leaf_clusters;
	clusters.leaf_reference_count = leaf_counts;
	clusters.leaf_first_reference = leaf_first;
	clusters.reference_count = take_count(4);
	references = (int32_t *)malloc((size_t)(clusters.reference_count + 1) * 4);
	memcpy(references, take((size_t)clusters.reference_count * 4), (size_t)clusters.reference_count * 4);
	clusters.reference_surfaces = references;
	clusters.cluster_count = take32();
	clusters.pvs_words = (size_t)take32();
	if (clusters.cluster_count < 0 || clusters.cluster_count > 65536 || clusters.pvs_words > 4096)
		fail("the clusters are out of range");
	clusters.pvs = (uint32_t const *)take((size_t)clusters.cluster_count * clusters.pvs_words * 4);
	if (clusters.pvs_words < (size_t)((clusters.cluster_count + 31) / 32))
		clusters.pvs = NULL;
	seed_count = take_count(16);
	seeds = (struct prism_bake_seed *)malloc((size_t)(seed_count + 1) * sizeof(*seeds));
	for (k = 0; k < seed_count; k++)
	{
		seeds[k].kind = take32();
		memcpy(seeds[k].position, take(12), 12);
	}
	page_count = take_count(8);
	pages = (struct prism_bake_atlas_page *)malloc((size_t)(page_count + 1) * sizeof(*pages));
	memcpy(pages, take((size_t)page_count * 8), (size_t)page_count * 8);
	triangle_count = (size_t)take_count(68);
	surfaces = (int32_t *)malloc((triangle_count + 1) * 4);
	positions = (float *)malloc((triangle_count + 1) * 36);
	tiers = (uint8_t *)malloc(triangle_count + 1);
	atlas_triangles = (struct prism_bake_atlas_triangle *)malloc((triangle_count + 1) * sizeof(*atlas_triangles));
	for (t = 0; t < triangle_count; t++)
	{
		surfaces[t] = take32();
		atlas_triangles[t].page = take32();
		memcpy(positions + t * 9, take(36), 36);
		memcpy(atlas_triangles[t].position, positions + t * 9, 36);
		memcpy(atlas_triangles[t].uv, take(24), 24);
	}

	/* the reachable space */
	prism_bake_reach_defaults(&rp);
	memcpy(rp.bounds, bounds, sizeof(bounds));
	rp.flight = flight != 0;
	rp.water_planes = water;
	rp.water_plane_count = water_count;
	rp.maximum_flight = 20000000;
	rp.maximum_walk = getenv("BAKE_MAXIMUM_WALK") ? (size_t)atol(getenv("BAKE_MAXIMUM_WALK")) : 50000000;
	started = now();
	if (prism_bake_reach_build(&reach, &bsp, seeds, (size_t)seed_count, &rp))
		fail("out of memory in the walk");
	reach_seconds = now() - started;
	sprintf(path, "%s/voxels.bin", argv[2]);
	write_voxels(path, &reach.voxels);
	sprintf(path, "%s/flight.bin", argv[2]);
	write_voxels(path, &reach.flight);
	{
		size_t slot;

		for (slot = 0; slot < reach.voxels.capacity; slot++)
		{
			int32_t xyz[3];

			if (prism_bake_voxels_slot(&reach.voxels, slot, xyz) & PRISM_BAKE_REACH_SEED)
				seeds_walked++;
		}
	}

	/* importance */
	prism_bake_importance_defaults(&ip);
	started = now();
	if (prism_bake_importance(&reach, &bsp, &clusters, positions, surfaces, triangle_count, &ip, tiers, &ic))
		fail("out of memory in the importance");
	importance_seconds = now() - started;
	sprintf(path, "%s/tiers.bin", argv[2]);
	out = fopen(path, "wb");
	fwrite(tiers, 1, triangle_count, out);
	fclose(out);

	/* the floors: each walkable-facing triangle with a floor of the walk
	at it (within a cell of its centroid or of its corners pulled in) */
	sprintf(path, "%s/unreached.bin", argv[2]);
	out = fopen(path, "wb");
	for (t = 0; t < triangle_count; t++)
	{
		float const *p = positions + t * 9;
		float e1[3], e2[3], n[3], length, area, cell = rp.cell;
		int found = 0, s;

		for (k = 0; k < 3; k++)
		{
			e1[k] = p[3 + k] - p[k];
			e2[k] = p[6 + k] - p[k];
		}
		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		area = length * 0.5f;
		if (length <= 0.0f || n[2] / length < rp.walkable_normal_z || area < 0.01f)
			continue;
		floor_triangles++;
		floor_area += area;
		/* (the centroid, and each corner a quarter of the way in) */
		for (s = 0; s < 4 && !found; s++)
		{
			float q[3];
			int32_t x, y, z, dx, dy, dz;

			for (k = 0; k < 3; k++)
			{
				float centroid = (p[k] + p[3 + k] + p[6 + k]) / 3.0f;

				q[k] = s == 3 ? centroid : p[s * 3 + k] * 0.75f + centroid * 0.25f;
			}
			x = (int32_t)floorf(q[0] / cell);
			y = (int32_t)floorf(q[1] / cell);
			z = (int32_t)floorf((q[2] + 0.01f) / cell);
			for (dz = -1; dz <= 1 && !found; dz++)
				for (dy = -1; dy <= 1 && !found; dy++)
					for (dx = -1; dx <= 1 && !found; dx++)
						found = (prism_bake_voxels_get(&reach.voxels, x + dx, y + dy, z + dz) & PRISM_BAKE_REACH_WALK) != 0;
		}
		if (found)
		{
			floor_triangles_reached++;
			floor_reached += area;
		}
		else
		{
			int32_t index = (int32_t)t;

			fwrite(&index, 4, 1, out);
			fwrite(&area, 4, 1, out);
			if (tiers[t] == PRISM_BAKE_TIER_NEAR)
				unreached_area_near += area;
		}
	}
	fclose(out);

	sprintf(path, "%s/summary.json", argv[2]);
	out = fopen(path, "w");
	fprintf(out, "{\n  \"reach\": {\"seconds\": %.2f, \"walk\": %lu, \"body\": %lu, \"camera\": %lu, \"view\": %lu, "
		"\"flight\": %lu, \"flight_on\": %d, \"walk_truncated\": %d, \"flight_truncated\": %d, \"seeds_used\": %lu, "
		"\"seeds_outside\": %lu, \"seed_floors\": %lu, \"walk_area_wu2\": %.1f, \"water_planes\": %d},\n",
		reach_seconds, (unsigned long)reach.walk_count, (unsigned long)reach.body_count,
		(unsigned long)reach.camera_count, (unsigned long)reach.view_count, (unsigned long)reach.flight_count, rp.flight,
		reach.walk_truncated, reach.flight_truncated, (unsigned long)reach.seeds_used, (unsigned long)reach.seeds_outside,
		(unsigned long)seeds_walked, reach.walk_count * rp.cell * rp.cell, water_count);
	fprintf(out, "  \"importance\": {\"seconds\": %.2f, \"triangles\": [%lu, %lu, %lu], \"area\": [%.1f, %.1f, %.1f], "
		"\"reachable_clusters\": %d, \"visible_clusters\": %d, \"clusters\": %d},\n", importance_seconds,
		(unsigned long)ic.triangles[0], (unsigned long)ic.triangles[1], (unsigned long)ic.triangles[2], ic.area[0],
		ic.area[1], ic.area[2], ic.reachable_clusters, ic.visible_clusters, clusters.cluster_count);
	fprintf(out, "  \"floors\": {\"triangles\": %lu, \"reached\": %lu, \"area\": %.1f, \"area_reached\": %.1f, "
		"\"area_unreached_tier0\": %.1f},\n", (unsigned long)floor_triangles, (unsigned long)floor_triangles_reached,
		floor_area, floor_reached, unreached_area_near);

	/* the atlas, at each density asked for, with the tiers and with every
	chart at tier 0 (what the importance saves) */
	fprintf(out, "  \"atlas\": [");
	for (a = 0; a < density_count * 2; a++)
	{
		struct prism_bake_atlas_parameters ap;
		struct prism_bake_atlas atlas;
		struct prism_bake_atlas_totals const *x;
		int flat = a % 2;

		for (t = 0; t < triangle_count; t++)
			atlas_triangles[t].tier = flat ? 0 : tiers[t];
		prism_bake_atlas_defaults(&ap, PRISM_BAKE_QUALITY_MAX);
		if (tile_override > 0)
			ap.tile = tile_override;
		ap.density[0] = densities[a / 2];
		ap.density[1] = densities[a / 2] * 0.5f;
		if (prism_bake_atlas_build(&atlas, pages, page_count, atlas_triangles, triangle_count, &ap))
			fail("out of memory in the atlas");
		x = &atlas.totals;
		fprintf(out, "%s\n    {\"density\": %g, \"tiers\": %s, \"tile\": %d, \"charts\": %lu, \"virtual_pages\": %lu, "
			"\"tiles\": %lu, \"texels_allocated\": %.0f, \"texels_charts\": %.0f, \"texels_covered\": %.0f, "
			"\"texels_dense\": %.0f, \"texels_ce\": %.0f, \"charts_by_tier\": [%lu, %lu, %lu], "
			"\"texels_by_tier\": [%.0f, %.0f, %.0f], \"world_area_by_tier\": [%.1f, %.1f, %.1f], "
			"\"charts_clamped\": %lu, \"triangles_without_atlas\": %lu, \"area_without_atlas\": %.1f}",
			a ? "," : "", densities[a / 2], flat ? "false" : "true", ap.tile, (unsigned long)x->charts,
			(unsigned long)x->virtual_pages, (unsigned long)x->tiles, x->texels_allocated, x->texels_charts,
			x->texels_covered, x->texels_dense, x->texels_ce, (unsigned long)x->charts_by_tier[0],
			(unsigned long)x->charts_by_tier[1], (unsigned long)x->charts_by_tier[2], x->texels_by_tier[0],
			x->texels_by_tier[1], x->texels_by_tier[2], x->world_area_by_tier[0], x->world_area_by_tier[1],
			x->world_area_by_tier[2], (unsigned long)x->charts_clamped, (unsigned long)x->triangles_without_atlas,
			x->area_without_atlas);
		prism_bake_atlas_free(&atlas);
	}
	fprintf(out, "\n  ]\n}\n");
	fclose(out);
	prism_bake_reach_free(&reach);
	return 0;
}
