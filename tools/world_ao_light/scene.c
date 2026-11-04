/*
SCENE.C

display.ambient_occlusion "world"'s made-up scene for the shading's checks
(tools/test_world_ao_light.py): a field of a floor and a wall, a sphere, a
capsule, a box and a second sphere of the first's owner, and points about
them with their normals, owners and references, the same every run. Built
with the shading's CPU reference (port/linux/game/world_ao_shading.c) and
the parts it reads (world_proxies.c's arithmetic, world_sdf_bake.c's
sample), whose game side is stood in for here.

  reference <count>   the scene and each point's occlusion, as JSON
*/

#include "world_ao.h"
#include "world_proxies.h"
#include "world_proxies_source.h"
#include "world_sdf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (world_proxies.c's game side: none) */
int world_proxies_source_definition(long definition, struct world_proxies_source *source)
{
	(void)definition;
	(void)source;
	return 0;
}

long world_proxies_source_instances(struct world_proxies_source_instance *instances, long maximum)
{
	(void)instances;
	(void)maximum;
	return 0;
}

short world_proxies_source_nodes(long id, struct world_proxies_frame *frames, short maximum)
{
	(void)id;
	(void)frames;
	(void)maximum;
	return 0;
}

int world_proxies_source_frustum(float planes[4][4])
{
	(void)planes;
	return 0;
}

int world_proxies_source_view(void)
{
	return 0;
}

void world_proxies_source_line(float const a[3], float const b[3], unsigned long color)
{
	(void)a;
	(void)b;
	(void)color;
}

enum
{
	SCENE_WIDTH = 40,
	SCENE_HEIGHT = 40,
	SCENE_DEPTH = 24,
	SCENE_PROXIES = 5,
};

struct world_sdf_volume scene_volume;
static signed char scene_voxels[SCENE_WIDTH * SCENE_HEIGHT * SCENE_DEPTH];
struct world_proxy scene_proxies[SCENE_PROXIES];
long scene_proxy_count = SCENE_PROXIES;
float scene_parameters[2][4];
/* the proxies' grid about a camera among them (world_ao_grid) */
float scene_grid[2][4];
unsigned int scene_cells[WORLD_AO_GRID_CELLS / 2][4];

static unsigned long scene_seed = 12345;

/* a number 0 to 1 */
float scene_random(void)
{
	scene_seed = scene_seed * 1103515245UL + 12345UL;
	return (float)((scene_seed >> 8) & 0xffffff) / 16777216.0f;
}

void scene_make(void)
{
	struct world_sdf_header *header = &scene_volume.header;
	unsigned long x, y, z;
	float axis_x[3] = { 0.8f, 0.3f, 0.0f }, length;

	memset(&scene_volume, 0, sizeof(scene_volume));
	header->width = SCENE_WIDTH;
	header->height = SCENE_HEIGHT;
	header->depth = SCENE_DEPTH;
	header->voxel = 0.25f;
	header->range = WORLD_SDF_RANGE_VOXELS * header->voxel;
	header->origin[0] = -5.0f;
	header->origin[1] = -5.0f;
	header->origin[2] = -1.0f;
	scene_volume.voxels = scene_voxels;
	/* a floor at z 0 and a wall at x 3: solid below and past them */
	for (z = 0; z < SCENE_DEPTH; z++)
	{
		for (y = 0; y < SCENE_HEIGHT; y++)
		{
			for (x = 0; x < SCENE_WIDTH; x++)
			{
				float px = header->origin[0] + (float)x * header->voxel;
				float pz = header->origin[2] + (float)z * header->voxel;
				float floor_distance = pz, wall_distance = 3.0f - px;

				scene_voxels[(z * SCENE_HEIGHT + y) * SCENE_WIDTH + x] = world_sdf_encode(
					floor_distance < wall_distance ? floor_distance : wall_distance, header->range);
			}
		}
	}
	memset(scene_proxies, 0, sizeof(scene_proxies));
	/* a sphere, owner 0 */
	scene_proxies[0].v[0][0] = 1.0f;
	scene_proxies[0].v[0][2] = 0.8f;
	scene_proxies[0].v[0][3] = _world_proxy_sphere;
	scene_proxies[0].v[1][3] = 0.0f;
	scene_proxies[0].v[2][0] = 0.4f;
	/* a capsule, owner 1 */
	scene_proxies[1].v[0][0] = -1.0f;
	scene_proxies[1].v[0][1] = 1.0f;
	scene_proxies[1].v[0][2] = 0.3f;
	scene_proxies[1].v[0][3] = _world_proxy_capsule;
	scene_proxies[1].v[1][0] = -0.6f;
	scene_proxies[1].v[1][1] = 1.2f;
	scene_proxies[1].v[1][2] = 1.6f;
	scene_proxies[1].v[1][3] = 1.0f;
	scene_proxies[1].v[2][0] = 0.3f;
	/* a box turned about z, owner 2 */
	length = sqrtf(axis_x[0] * axis_x[0] + axis_x[1] * axis_x[1]);
	scene_proxies[2].v[0][1] = -2.0f;
	scene_proxies[2].v[0][2] = 0.6f;
	scene_proxies[2].v[0][3] = _world_proxy_box;
	scene_proxies[2].v[1][0] = axis_x[0];
	scene_proxies[2].v[1][1] = axis_x[1];
	scene_proxies[2].v[1][3] = 2.0f;
	scene_proxies[2].v[2][0] = -axis_x[1] / length * 0.5f;
	scene_proxies[2].v[2][1] = axis_x[0] / length * 0.5f;
	scene_proxies[2].v[2][3] = 0.6f;
	/* a second sphere of owner 0 */
	scene_proxies[3] = scene_proxies[0];
	scene_proxies[3].v[0][2] = 1.6f;
	scene_proxies[3].v[2][0] = 0.3f;
	/* a box along the axes, owner 3 */
	scene_proxies[4].v[0][0] = 1.8f;
	scene_proxies[4].v[0][1] = 1.5f;
	scene_proxies[4].v[0][2] = 0.4f;
	scene_proxies[4].v[0][3] = _world_proxy_box;
	scene_proxies[4].v[1][0] = 0.6f;
	scene_proxies[4].v[1][3] = 3.0f;
	scene_proxies[4].v[2][1] = 0.4f;
	scene_proxies[4].v[2][3] = 0.4f;
	memset(scene_parameters, 0, sizeof(scene_parameters));
	scene_parameters[0][0] = 0.9f;
	scene_parameters[0][1] = 0.7f;
	scene_parameters[0][2] = 0.75f;
	scene_parameters[0][3] = (float)SCENE_PROXIES;
	scene_parameters[1][0] = 0.8f;
	{
		static const float camera[3] = { 0.4f, -0.3f, 1.5f };

		world_ao_grid(scene_proxies, scene_proxy_count, camera, scene_grid, scene_cells);
	}
}

/* a point about the shapes and the walls, a unit normal, an owner (-1 to 3)
and a reference */
void scene_point(float point[3], float normal[3], float *owner, float *reference)
{
	float length;
	int axis;

	point[0] = -3.0f + 6.0f * scene_random();
	point[1] = -3.0f + 6.0f * scene_random();
	point[2] = 0.02f + 2.5f * scene_random();
	do
	{
		for (axis = 0; axis < 3; axis++)
			normal[axis] = 2.0f * scene_random() - 1.0f;
		length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	}
	while (length < 0.1f || length > 1.0f);
	for (axis = 0; axis < 3; axis++)
		normal[axis] /= length;
	*owner = (float)((int)(scene_random() * 5.0f) - 1);
	*reference = 0.1f + 0.9f * scene_random();
}

#ifndef SCENE_LIBRARY
/* ---------- cull: the grid's product against every proxy's, at random
windows of 64 shapes (boxes the most, as at b30's start) and random points
about them: equal, bit for bit. And how far a box's bound (its fade and
reach) moves the product from the 17 half diagonals' reach faded over its
last quarter that it replaced (the shapes alone, no grid) */

static float scene_between(float low, float high)
{
	return low + (high - low) * scene_random();
}

static void scene_direction(float out[3])
{
	float length;

	do
	{
		out[0] = scene_between(-1.0f, 1.0f);
		out[1] = scene_between(-1.0f, 1.0f);
		out[2] = scene_between(-1.0f, 1.0f);
		length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	}
	while (length < 0.1f || length > 1.0f);
	out[0] /= length;
	out[1] /= length;
	out[2] /= length;
}

static void scene_random_proxy(float const camera[3], struct world_proxy *proxy)
{
	float kind = scene_random(), centre[3];
	int k;

	memset(proxy, 0, sizeof(*proxy));
	centre[0] = camera[0] + scene_between(-60.0f, 60.0f);
	centre[1] = camera[1] + scene_between(-60.0f, 60.0f);
	centre[2] = camera[2] + scene_between(-8.0f, 4.0f);
	for (k = 0; k < 3; k++)
		proxy->v[0][k] = centre[k];
	proxy->v[1][3] = (float)(int)scene_between(0.0f, 20.0f);
	if (kind < 0.7f)
	{
		float x[3], y[3], along, length, ex = scene_between(0.1f, 3.0f), ey = scene_between(0.1f, 3.0f);

		proxy->v[0][3] = _world_proxy_box;
		scene_direction(x);
		scene_direction(y);
		along = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
		for (k = 0; k < 3; k++)
			y[k] -= along * x[k];
		length = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
		for (k = 0; k < 3; k++)
		{
			proxy->v[1][k] = ex * x[k];
			proxy->v[2][k] = ey * y[k] / length;
		}
		proxy->v[2][3] = scene_between(0.1f, 3.0f);
	}
	else if (kind < 0.85f)
	{
		proxy->v[0][3] = _world_proxy_sphere;
		proxy->v[2][0] = scene_between(0.1f, 1.0f);
	}
	else
	{
		float axis[3], length = scene_between(0.2f, 1.5f);

		proxy->v[0][3] = _world_proxy_capsule;
		scene_direction(axis);
		for (k = 0; k < 3; k++)
			proxy->v[1][k] = centre[k] + axis[k] * length;
		proxy->v[2][0] = scene_between(0.05f, 0.4f);
	}
}

/* the product as it was: every proxy, a box's reach 17 half diagonals and
faded over the last quarter of it */
static float scene_product_before(struct world_proxy const *proxies, long count, float const point[3],
	float const normal[3], float owner)
{
	float product = 1.0f;
	long index;

	for (index = 0; index < count; index++)
	{
		struct world_proxy const *proxy = &proxies[index];
		float centre[3], offset[3], reach, away, bound, fade;
		int axis;

		if (proxy->v[1][3] == owner)
			continue;
		for (axis = 0; axis < 3; axis++)
		{
			centre[axis] = (int)proxy->v[0][3] == _world_proxy_capsule ?
				0.5f * (proxy->v[0][axis] + proxy->v[1][axis]) : proxy->v[0][axis];
			offset[axis] = point[axis] - centre[axis];
		}
		if ((int)proxy->v[0][3] == _world_proxy_box)
		{
			bound = sqrtf(proxy->v[1][0] * proxy->v[1][0] + proxy->v[1][1] * proxy->v[1][1] +
				proxy->v[1][2] * proxy->v[1][2] + proxy->v[2][0] * proxy->v[2][0] + proxy->v[2][1] * proxy->v[2][1] +
				proxy->v[2][2] * proxy->v[2][2] + proxy->v[2][3] * proxy->v[2][3]);
			reach = 17.0f * bound;
		}
		else
		{
			reach = world_proxy_reach(proxy);
			bound = (int)proxy->v[0][3] == _world_proxy_capsule ? 0.5f * sqrtf((proxy->v[1][0] - proxy->v[0][0]) *
				(proxy->v[1][0] - proxy->v[0][0]) + (proxy->v[1][1] - proxy->v[0][1]) * (proxy->v[1][1] - proxy->v[0][1]) +
				(proxy->v[1][2] - proxy->v[0][2]) * (proxy->v[1][2] - proxy->v[0][2])) + proxy->v[2][0] : proxy->v[2][0];
		}
		away = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
		if (away >= reach)
			continue;
		if (normal[0] * -offset[0] + normal[1] * -offset[1] + normal[2] * -offset[2] < -bound)
			continue;
		fade = (reach - away) / (reach * WORLD_AO_REACH_FADE);
		fade = fade > 1.0f ? 1.0f : fade;
		product *= 1.0f - world_proxy_occlusion(proxy, point, normal) * fade;
	}
	return product;
}

static int scene_cull(long windows, long points)
{
	static struct world_proxy proxies[64];
	static float grid[2][4];
	static unsigned int cells[WORLD_AO_GRID_CELLS / 2][4];
	long window, index, unequal = 0, evaluated = 0, total = 0, boxes_kept = 0, boxes_in_reach = 0;
	double worst_before = 0.0, sum_before = 0.0;

	for (window = 0; window < windows; window++)
	{
		float camera[3];

		camera[0] = scene_between(-500.0f, 500.0f);
		camera[1] = scene_between(-500.0f, 500.0f);
		camera[2] = scene_between(-50.0f, 50.0f);
		for (index = 0; index < 64; index++)
			scene_random_proxy(camera, &proxies[index]);
		world_ao_grid(proxies, 64, camera, grid, cells);
		for (index = 0; index < points; index++)
		{
			float point[3], normal[3], owner = (float)((int)scene_between(0.0f, 21.0f) - 1), all, culled, before;
			int cell, word, shape;

			/* (near the shapes: about one of them, or anywhere about the camera) */
			if (index & 1)
			{
				struct world_proxy const *near = &proxies[(int)scene_between(0.0f, 63.99f)];
				float distance = scene_between(0.0f, 1.0f), direction[3];
				int k;

				distance = 0.05f + 30.0f * distance * distance;
				scene_direction(direction);
				for (k = 0; k < 3; k++)
					point[k] = near->v[0][k] + direction[k] * distance;
			}
			else
			{
				point[0] = camera[0] + scene_between(-140.0f, 140.0f);
				point[1] = camera[1] + scene_between(-140.0f, 140.0f);
				point[2] = camera[2] + scene_between(-12.0f, 8.0f);
			}
			scene_direction(normal);
			all = world_ao_proxies_occlusion(proxies, 64, point, normal, owner);
			culled = world_ao_proxies_occlusion_grid(proxies, (float const (*)[4])grid,
				(unsigned int const (*)[4])cells, point, normal, owner);
			if (all != culled)
			{
				if (unequal < 10)
					printf("unequal at %g %g %g: %.9g, grid %.9g\n", point[0], point[1], point[2], all, culled);
				unequal++;
			}
			before = scene_product_before(proxies, 64, point, normal, owner);
			worst_before = fabs(before - all) > worst_before ? fabs(before - all) : worst_before;
			sum_before += fabs(before - all);
			cell = world_ao_grid_cell((float const (*)[4])grid, point);
			for (word = 0; word < 2; word++)
				for (shape = 0; shape < 32; shape++)
					evaluated += (cells[cell >> 1][((cell & 1) << 1) + word] >> shape) & 1u;
			total += 64;
			for (shape = 0; shape < 64; shape++)
			{
				struct world_proxy const *proxy = &proxies[shape];
				float offset[3], away;
				int k;

				if ((int)proxy->v[0][3] != _world_proxy_box)
					continue;
				for (k = 0; k < 3; k++)
					offset[k] = point[k] - proxy->v[0][k];
				away = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
				if (away < 17.0f * sqrtf(proxy->v[1][0] * proxy->v[1][0] + proxy->v[1][1] * proxy->v[1][1] +
					proxy->v[1][2] * proxy->v[1][2] + proxy->v[2][0] * proxy->v[2][0] + proxy->v[2][1] * proxy->v[2][1] +
					proxy->v[2][2] * proxy->v[2][2] + proxy->v[2][3] * proxy->v[2][3]))
				{
					boxes_in_reach++;
					boxes_kept += world_proxy_box_fade(proxy, point, normal) > 0.0f;
				}
			}
		}
	}
	printf("{\"points\": %ld, \"unequal\": %ld, \"cell_share\": %.4f, \"boxes_computed_share\": %.4f, "
		"\"before_worst\": %.6f, \"before_mean\": %.7f}\n", windows * points, unequal, (double)evaluated / (double)total,
		boxes_in_reach ? (double)boxes_kept / (double)boxes_in_reach : 0.0, worst_before,
		sum_before / (double)(windows * points));
	return unequal ? 1 : 0;
}

int main(int argc, char **argv)
{
	long count = argc > 2 && !strcmp(argv[1], "reference") ? atol(argv[2]) : 0, index;
	unsigned long voxel;
	int i, j;

	if (argc > 3 && !strcmp(argv[1], "cull"))
		return scene_cull(atol(argv[2]), atol(argv[3]));
	if (count <= 0)
	{
		fprintf(stderr, "scene reference <count> | scene cull <windows> <points>\n");
		return 2;
	}
	scene_make();
	printf("{\"size\": [%d, %d, %d], \"origin\": [%.9g, %.9g, %.9g], \"voxel\": %.9g, \"range\": %.9g,\n",
		SCENE_WIDTH, SCENE_HEIGHT, SCENE_DEPTH, scene_volume.header.origin[0], scene_volume.header.origin[1],
		scene_volume.header.origin[2], scene_volume.header.voxel, scene_volume.header.range);
	printf("\"voxels\": [");
	for (voxel = 0; voxel < sizeof(scene_voxels); voxel++)
		printf("%s%d", voxel ? "," : "", scene_voxels[voxel]);
	printf("],\n\"parameters\": [");
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			printf("%s%.9g", i || j ? "," : "", scene_parameters[i][j]);
	printf("],\n\"proxies\": [");
	for (index = 0; index < scene_proxy_count; index++)
		for (i = 0; i < 3; i++)
			for (j = 0; j < 4; j++)
				printf("%s%.9g", index || i || j ? "," : "", scene_proxies[index].v[i][j]);
	printf("],\n\"grid\": [");
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			printf("%s%.9g", i || j ? "," : "", scene_grid[i][j]);
	printf("],\n\"cells\": [");
	for (index = 0; index < WORLD_AO_GRID_CELLS / 2; index++)
		for (j = 0; j < 4; j++)
			printf("%s%u", index || j ? "," : "", scene_cells[index][j]);
	printf("],\n\"cases\": [\n");
	for (index = 0; index < count; index++)
	{
		float point[3], normal[3], owner, reference;
		int lightmap;

		scene_point(point, normal, &owner, &reference);
		for (lightmap = 0; lightmap < 2; lightmap++)
		{
			printf("%s{\"point\": [%.9g, %.9g, %.9g], \"normal\": [%.9g, %.9g, %.9g], \"owner\": %.9g, "
				"\"reference\": %.9g, \"lightmap\": %d, \"occlusion\": %.9g}\n", index || lightmap ? "," : "",
				point[0], point[1], point[2], normal[0], normal[1], normal[2], owner, reference, lightmap,
				world_ao_occlusion((float const (*)[4])scene_parameters, &scene_volume, scene_proxies,
				scene_proxy_count, point, normal, owner, reference, lightmap));
		}
	}
	printf("]}\n");
	return 0;
}
#endif
