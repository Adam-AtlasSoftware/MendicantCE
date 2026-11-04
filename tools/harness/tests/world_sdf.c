/* display.ambient_occlusion "world"'s distance field: its layout, encoding,
sample and bake (port/linux/game/world_sdf_bake.c, test_world_sdf.py).
Workstream B grows the bake's cases here: a plane, a box room, a sphere
against their exact distances. */

#include "harness.h"

#include <math.h>

#include "under_test.inc"

static int nothing_inside(void *context, float const point[3])
{
	(void)context;
	(void)point;
	return 0;
}

static void layout_case(void)
{
	/* a small room: the finest voxel; a huge map: within the budget */
	float const room[6] = { 0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 4.0f };
	float const huge[6] = { -500.0f, -500.0f, -50.0f, 500.0f, 500.0f, 150.0f };
	struct world_sdf_header header;

	world_sdf_layout(room, &header);
	CHECK(header.voxel == 0.25f, "%g", header.voxel);
	CHECK(header.width >= 40 && header.height >= 40 && header.depth >= 16, "%lu %lu %lu", header.width,
		header.height, header.depth);
	CHECK(header.origin[0] <= 0.0f && header.origin[0] + (float)(header.width - 1) * header.voxel >= 10.0f, "x");
	world_sdf_layout(huge, &header);
	CHECK((double)header.width * header.height * header.depth <= WORLD_SDF_MAXIMUM_VOXELS, "budget");
	CHECK(header.width <= WORLD_SDF_MAXIMUM_DIMENSION, "dimension");
	CHECK(header.range == WORLD_SDF_RANGE_VOXELS * header.voxel, "range");
}

static void encoding_case(void)
{
	float range = 3.0f, distance;

	for (distance = -3.0f; distance <= 3.0f; distance += 0.125f)
	{
		float back = world_sdf_decode(world_sdf_encode(distance, range), range);

		CHECK(fabsf(back - distance) <= range / 254.0f + 1.0e-6f, "%g -> %g", distance, back);
	}
	CHECK(world_sdf_encode(100.0f, range) == 127 && world_sdf_encode(-100.0f, range) == -127, "clamped");
}

static void sample_case(void)
{
	/* a field that is a plane's (distance = z): trilinear holds it exactly */
	float const bounds[6] = { 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f };
	struct world_sdf_volume volume;
	unsigned long x, y, z;
	float point[3] = { 0.8f, 1.1f, 0.0f };

	world_sdf_layout(bounds, &volume.header);
	volume.voxels = malloc(volume.header.width * volume.header.height * volume.header.depth);
	for (z = 0; z < volume.header.depth; z++)
		for (y = 0; y < volume.header.height; y++)
			for (x = 0; x < volume.header.width; x++)
				volume.voxels[(z * volume.header.height + y) * volume.header.width + x] =
					world_sdf_encode(volume.header.origin[2] + (float)z * volume.header.voxel - 1.0f,
						volume.header.range);
	for (point[2] = 0.5f; point[2] <= 1.5f; point[2] += 0.1f)
	{
		float sampled = world_sdf_sample(&volume, point);

		CHECK(fabsf(sampled - (point[2] - 1.0f)) < 0.02f, "%g at %g", sampled, point[2]);
	}
	free(volume.voxels);
}

static void bake_case(void)
{
	/* the skeleton's bake: no triangles, everything far outside */
	float const bounds[6] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	struct world_sdf_volume volume;
	unsigned long index, count;

	world_sdf_layout(bounds, &volume.header);
	count = volume.header.width * volume.header.height * volume.header.depth;
	volume.voxels = calloc(count, 1);
	world_sdf_bake_slab(&volume, NULL, 0, nothing_inside, NULL, 0, (long)volume.header.depth);
	for (index = 0; index < count; index++)
		CHECK(volume.voxels[index] == 127, "voxel %lu", index);
	free(volume.voxels);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "layout"))
		layout_case();
	else if (!strcmp(name, "encoding"))
		encoding_case();
	else if (!strcmp(name, "sample"))
		sample_case();
	else if (!strcmp(name, "bake"))
		bake_case();
	else
		return 2;
	return 0;
}
