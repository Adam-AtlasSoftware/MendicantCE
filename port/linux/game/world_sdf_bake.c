/*
WORLD_SDF_BAKE.C

The distance field's pure arithmetic (world_sdf.h): its layout over a BSP's
bounds, its encoding, the bake of a slab of it, and its trilinear sample as
the shaders take it. No game state: the bake's threads run it on a copy of
the geometry. Tested asset-free (tools/harness/tests/test_world_sdf.py).
Workstream B.
*/

#include "world_sdf.h"

#include <math.h>
#include <string.h>

void world_sdf_layout(float const bounds[6], struct world_sdf_header *header)
{
	float voxel = (float)WORLD_SDF_FINEST_VOXEL_MILLI / 1000.0f;
	unsigned long count[3];
	int axis;

	for (;;)
	{
		float padding = WORLD_SDF_PADDING_VOXELS * voxel;
		double voxels = 1.0;

		for (axis = 0; axis < 3; axis++)
		{
			float size = bounds[3 + axis] - bounds[axis] + 2.0f * padding;

			if (size < voxel)
				size = voxel;
			count[axis] = (unsigned long)ceilf(size / voxel);
			voxels *= (double)count[axis];
		}
		if (voxels <= (double)WORLD_SDF_MAXIMUM_VOXELS && count[0] <= WORLD_SDF_MAXIMUM_DIMENSION &&
			count[1] <= WORLD_SDF_MAXIMUM_DIMENSION && count[2] <= WORLD_SDF_MAXIMUM_DIMENSION)
		{
			break;
		}
		voxel *= 1.125f;
	}
	header->width = count[0];
	header->height = count[1];
	header->depth = count[2];
	header->voxel = voxel;
	header->range = WORLD_SDF_RANGE_VOXELS * voxel;
	for (axis = 0; axis < 3; axis++)
	{
		/* (centred over the bounds: the first voxel's centre) */
		float middle = 0.5f * (bounds[axis] + bounds[3 + axis]);

		header->origin[axis] = middle - 0.5f * (float)(count[axis] - 1) * voxel;
	}
}

signed char world_sdf_encode(float distance, float range)
{
	float value = distance / range * 127.0f;

	if (value > 127.0f)
		value = 127.0f;
	if (value < -127.0f)
		value = -127.0f;
	return (signed char)(value < 0.0f ? value - 0.5f : value + 0.5f);
}

float world_sdf_decode(signed char value, float range)
{
	return (float)value / 127.0f * range;
}

void world_sdf_bake_slab(struct world_sdf_volume *volume, float const *positions, long triangle_count,
	int (*inside)(void *context, float const point[3]), void *context, long z_first, long z_end)
{
	/* B: the bake (docs/world-ao.md, "The bake"). The skeleton: every
	voxel as far outside as the encoding holds */
	unsigned long slice = volume->header.width * volume->header.height;

	(void)positions;
	(void)triangle_count;
	(void)inside;
	(void)context;
	if (z_end > z_first)
		memset(volume->voxels + slice * (unsigned long)z_first, 127, slice * (unsigned long)(z_end - z_first));
}

float world_sdf_sample(struct world_sdf_volume const *volume, float const point[3])
{
	struct world_sdf_header const *header = &volume->header;
	unsigned long size[3];
	long base[3];
	float fraction[3];
	float result = 0.0f;
	int axis, corner;

	size[0] = header->width;
	size[1] = header->height;
	size[2] = header->depth;
	for (axis = 0; axis < 3; axis++)
	{
		/* (texel centres at the voxels' centres, clamped to the edge, as a
		GPU's linear filter with clamp-to-edge reads them) */
		float coordinate = (point[axis] - header->origin[axis]) / header->voxel;

		if (coordinate < 0.0f)
			coordinate = 0.0f;
		if (coordinate > (float)(size[axis] - 1))
			coordinate = (float)(size[axis] - 1);
		base[axis] = (long)floorf(coordinate);
		if (size[axis] > 1 && base[axis] > (long)size[axis] - 2)
			base[axis] = (long)size[axis] - 2;
		fraction[axis] = coordinate - (float)base[axis];
	}
	for (corner = 0; corner < 8; corner++)
	{
		long x = base[0] + (corner & 1), y = base[1] + ((corner >> 1) & 1), z = base[2] + ((corner >> 2) & 1);
		float weight = ((corner & 1) ? fraction[0] : 1.0f - fraction[0]) *
			(((corner >> 1) & 1) ? fraction[1] : 1.0f - fraction[1]) *
			(((corner >> 2) & 1) ? fraction[2] : 1.0f - fraction[2]);

		if (weight == 0.0f)
			continue;
		if (x >= (long)size[0])
			x = (long)size[0] - 1;
		if (y >= (long)size[1])
			y = (long)size[1] - 1;
		if (z >= (long)size[2])
			z = (long)size[2] - 1;
		result += weight * world_sdf_decode(volume->voxels[((unsigned long)z * size[1] + (unsigned long)y) *
			size[0] + (unsigned long)x], header->range);
	}
	return result;
}
