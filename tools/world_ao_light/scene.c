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
int main(int argc, char **argv)
{
	long count = argc > 2 && !strcmp(argv[1], "reference") ? atol(argv[2]) : 0, index;
	unsigned long voxel;
	int i, j;

	if (count <= 0)
	{
		fprintf(stderr, "scene reference <count>\n");
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
