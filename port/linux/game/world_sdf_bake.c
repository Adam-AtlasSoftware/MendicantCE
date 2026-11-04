/*
WORLD_SDF_BAKE.C

The distance field's pure arithmetic (world_sdf.h): its layout over a BSP's
bounds, its encoding, the bake of a slab of it, and its trilinear sample as
the shaders take it. No game state: the bake's threads run it on a copy of
the geometry. Tested asset-free (tools/harness/tests/test_world_sdf.py).
Workstream B.

The bake (docs/world-ao.md, "The bake"): each voxel's distance is the least
exact distance from its centre to a triangle, found only for the triangles
within range of it (the narrow band: each triangle visits the voxels of the
rows that pass within range of its plane and of its bounding sphere, and
only works out the exact distance where its plane is nearer than the best
so far); past range a voxel keeps range, which the encoding clamps to
anyway. The sign is the collision BSP's own point test (inside), not the
triangles' facing: Halo's BSPs are not perfectly closed.
*/

#include "world_sdf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the layout and the encoding */

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

/* ---------- the bake */

static float sdf_dot(float const a[3], float const b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* the squared distance from p to the segment a to a + ab */
static float sdf_segment_squared(float const p[3], float const a[3], float const ab[3])
{
	float ap[3], length = sdf_dot(ab, ab), t = 0.0f, d[3];
	int axis;

	for (axis = 0; axis < 3; axis++)
		ap[axis] = p[axis] - a[axis];
	if (length > 0.0f)
	{
		t = sdf_dot(ap, ab) / length;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	}
	for (axis = 0; axis < 3; axis++)
		d[axis] = ap[axis] - t * ab[axis];
	return sdf_dot(d, d);
}

/* the squared distance from p to the triangle abc: the closest point found
by the Voronoi regions of its corners, edges and face (Ericson, Real-Time
Collision Detection, 5.1.5); a triangle with no area as its three edges */
static float sdf_triangle_squared(float const p[3], float const a[3], float const b[3], float const c[3],
	float const ab[3], float const ac[3], int flat)
{
	float ap[3], bp[3], cp[3], closest[3], d[3];
	float d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denominator;
	int axis;

	if (flat)
	{
		float bc[3], best, other;

		for (axis = 0; axis < 3; axis++)
			bc[axis] = c[axis] - b[axis];
		best = sdf_segment_squared(p, a, ab);
		other = sdf_segment_squared(p, a, ac);
		if (other < best)
			best = other;
		other = sdf_segment_squared(p, b, bc);
		return other < best ? other : best;
	}
	for (axis = 0; axis < 3; axis++)
		ap[axis] = p[axis] - a[axis];
	d1 = sdf_dot(ab, ap);
	d2 = sdf_dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
		return sdf_dot(ap, ap);
	for (axis = 0; axis < 3; axis++)
		bp[axis] = p[axis] - b[axis];
	d3 = sdf_dot(ab, bp);
	d4 = sdf_dot(ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
		return sdf_dot(bp, bp);
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
	{
		v = d1 / (d1 - d3);
		for (axis = 0; axis < 3; axis++)
			d[axis] = ap[axis] - v * ab[axis];
		return sdf_dot(d, d);
	}
	for (axis = 0; axis < 3; axis++)
		cp[axis] = p[axis] - c[axis];
	d5 = sdf_dot(ab, cp);
	d6 = sdf_dot(ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
		return sdf_dot(cp, cp);
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
	{
		w = d2 / (d2 - d6);
		for (axis = 0; axis < 3; axis++)
			d[axis] = ap[axis] - w * ac[axis];
		return sdf_dot(d, d);
	}
	va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
	{
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (axis = 0; axis < 3; axis++)
			d[axis] = bp[axis] - w * (c[axis] - b[axis]);
		return sdf_dot(d, d);
	}
	denominator = 1.0f / (va + vb + vc);
	v = vb * denominator;
	w = vc * denominator;
	for (axis = 0; axis < 3; axis++)
	{
		closest[axis] = ab[axis] * v + ac[axis] * w;
		d[axis] = ap[axis] - closest[axis];
	}
	return sdf_dot(d, d);
}

/* the voxels' indices along an axis whose centres are from low to high
(world units), within the field's count of them: first to last
inclusive; FALSE if none */
static int sdf_span(float low, float high, float origin, float voxel, long count, long *first, long *last)
{
	float from = ceilf((low - origin) / voxel), to = floorf((high - origin) / voxel);

	if (from < 0.0f)
		from = 0.0f;
	if (to > (float)(count - 1))
		to = (float)(count - 1);
	if (from > to)
		return 0;
	*first = (long)from;
	*last = (long)to;
	return 1;
}

void world_sdf_bake_slab(struct world_sdf_volume *volume, float const *positions, long triangle_count,
	int (*inside)(void *context, float const point[3]), void *context, long z_first, long z_end)
{
	struct world_sdf_header const *header = &volume->header;
	long width = (long)header->width, height = (long)header->height, depth = (long)header->depth;
	unsigned long slice = (unsigned long)width * (unsigned long)height;
	float voxel = header->voxel, range = header->range;
	float *best;
	unsigned long count, index;
	long triangle, x, y, z;

	if (z_first < 0)
		z_first = 0;
	if (z_end > depth)
		z_end = depth;
	if (z_end <= z_first)
		return;
	count = slice * (unsigned long)(z_end - z_first);
	/* (each voxel's least distance so far, from range down) */
	best = malloc(count * sizeof(*best));
	if (best)
	{
		for (index = 0; index < count; index++)
			best[index] = range;
	}
	for (triangle = 0; best && triangle < triangle_count; triangle++)
	{
		float const *a = positions + triangle * 9, *b = a + 3, *c = a + 6;
		float ab[3], ac[3], normal[3], centre[3], reach, length, offset, bounds[6];
		long first[3], last[3];
		int axis, flat;

		for (axis = 0; axis < 3; axis++)
		{
			float low = a[axis], high = a[axis];

			if (b[axis] < low)
				low = b[axis];
			if (c[axis] < low)
				low = c[axis];
			if (b[axis] > high)
				high = b[axis];
			if (c[axis] > high)
				high = c[axis];
			bounds[axis] = low - range;
			bounds[3 + axis] = high + range;
			ab[axis] = b[axis] - a[axis];
			ac[axis] = c[axis] - a[axis];
			centre[axis] = (a[axis] + b[axis] + c[axis]) / 3.0f;
		}
		if (!sdf_span(bounds[2], bounds[5], header->origin[2], voxel, depth, &first[2], &last[2]))
			continue;
		if (first[2] < z_first)
			first[2] = z_first;
		if (last[2] > z_end - 1)
			last[2] = z_end - 1;
		if (first[2] > last[2] ||
			!sdf_span(bounds[0], bounds[3], header->origin[0], voxel, width, &first[0], &last[0]) ||
			!sdf_span(bounds[1], bounds[4], header->origin[1], voxel, height, &first[1], &last[1]))
		{
			continue;
		}
		normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
		normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
		normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
		length = sqrtf(sdf_dot(normal, normal));
		/* (no area worth the name: its edges alone, and no plane) */
		flat = !(length > 1.0e-6f * (sdf_dot(ab, ab) + sdf_dot(ac, ac)));
		if (!flat)
		{
			for (axis = 0; axis < 3; axis++)
				normal[axis] /= length;
		}
		offset = flat ? 0.0f : sdf_dot(normal, a);
		/* (the bounding sphere about the centroid, grown by range) */
		reach = 0.0f;
		{
			float const *corners[3];
			int corner;

			corners[0] = a;
			corners[1] = b;
			corners[2] = c;
			for (corner = 0; corner < 3; corner++)
			{
				float d[3], squared;

				for (axis = 0; axis < 3; axis++)
					d[axis] = corners[corner][axis] - centre[axis];
				squared = sdf_dot(d, d);
				if (squared > reach)
					reach = squared;
			}
			reach = sqrtf(reach) + range;
		}
		for (z = first[2]; z <= last[2]; z++)
		{
			float point_z = header->origin[2] + (float)z * voxel;
			float dz = point_z - centre[2];

			for (y = first[1]; y <= last[1]; y++)
			{
				float point_y = header->origin[1] + (float)y * voxel;
				float dy = point_y - centre[1];
				float across = reach * reach - dy * dy - dz * dz;
				float low, high, plane_row;
				long row_first, row_last;
				float *row_best;

				if (across <= 0.0f)
					continue;
				across = sqrtf(across);
				low = centre[0] - across;
				high = centre[0] + across;
				/* (and where the row is within range of the plane) */
				plane_row = normal[1] * point_y + normal[2] * point_z - offset;
				if (!flat)
				{
					if (fabsf(normal[0]) > 1.0e-6f)
					{
						float one = (-range - plane_row) / normal[0], other = (range - plane_row) / normal[0];

						if (one > other)
						{
							float swap = one;

							one = other;
							other = swap;
						}
						if (one > low)
							low = one;
						if (other < high)
							high = other;
					}
					else if (fabsf(plane_row) >= range)
					{
						continue;
					}
				}
				if (!sdf_span(low, high, header->origin[0], voxel, width, &row_first, &row_last))
					continue;
				if (row_first < first[0])
					row_first = first[0];
				if (row_last > last[0])
					row_last = last[0];
				row_best = best + (unsigned long)(z - z_first) * slice + (unsigned long)y * (unsigned long)width;
				for (x = row_first; x <= row_last; x++)
				{
					float point[3], distance;

					point[0] = header->origin[0] + (float)x * voxel;
					/* (never nearer than its plane) */
					if (!flat && fabsf(plane_row + normal[0] * point[0]) >= row_best[x])
						continue;
					point[1] = point_y;
					point[2] = point_z;
					distance = sdf_triangle_squared(point, a, b, c, ab, ac, flat);
					if (distance < row_best[x] * row_best[x])
						row_best[x] = sqrtf(distance);
				}
			}
		}
	}
	/* the sign, and the encoding */
	for (z = z_first; z < z_end; z++)
	{
		for (y = 0; y < height; y++)
		{
			signed char *row = volume->voxels + (unsigned long)z * slice + (unsigned long)y * (unsigned long)width;
			float const *row_best = best ? best + (unsigned long)(z - z_first) * slice + (unsigned long)y *
				(unsigned long)width : NULL;
			float point[3];

			point[1] = header->origin[1] + (float)y * voxel;
			point[2] = header->origin[2] + (float)z * voxel;
			for (x = 0; x < width; x++)
			{
				float distance = row_best ? row_best[x] : range;

				point[0] = header->origin[0] + (float)x * voxel;
				if (inside && inside(context, point))
					distance = -distance;
				row[x] = world_sdf_encode(distance, range);
			}
		}
	}
	free(best);
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
