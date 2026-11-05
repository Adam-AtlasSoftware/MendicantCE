/*
WORLD_SDF_BAKE.C

The distance field's pure arithmetic (world_sdf.h): its layout of cells
over a BSP's bounds, the cells a surface may be within range of, the bake
of one cell's brick, the field made from a whole grid of voxels, its
encoding, its GPU atlas, and its trilinear sample as the shaders take it.
No game state: the bake's threads run it on a copy of the geometry. Tested
asset-free (tools/harness/tests/test_world_sdf.py). Workstream B.

The field is sparse (docs/world-ao.md, "Geometry, bounds and
resolution"): a grid of cells WORLD_SDF_BRICK voxels' steps a side, each
either a brick of voxels or one kind, all outside (+range) or all inside
(-range). A cell holds a brick where a triangle may be within range of one
of its voxels; its memory follows the surfaces' area, so the voxel stays
fine where a whole grid's would have to grow with the bounds' volume.
Bricks share their faces' voxels with their neighbours, so a sample
filtered within one brick is the whole grid's, exactly, everywhere.

The bake (docs/world-ao.md, "The bake"): each voxel's distance is the least
exact distance from its centre to a triangle, found only for the triangles
within range of it (the narrow band: the triangles binned to the cells they
may reach; each visits the voxels of its cell's rows that pass within range
of its plane and of its bounding sphere, and only works out the exact
distance where its plane is nearer than the best so far); past range a
voxel keeps range, which the encoding clamps to anyway. The sign is the
collision BSP's own point test (inside), not the triangles' facing: Halo's
BSPs are not perfectly closed.
*/

#include "world_sdf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the layout and the encoding */

void world_sdf_layout(float const bounds[6], float voxel, struct world_sdf_header *header)
{
	float step = WORLD_SDF_BRICK * voxel;
	int axis;
	unsigned long count[3];

	for (axis = 0; axis < 3; axis++)
	{
		float size = bounds[3 + axis] - bounds[axis] + 2.0f * WORLD_SDF_PADDING_VOXELS * voxel;
		/* (centred over the bounds: the first voxel's centre) */
		float middle = 0.5f * (bounds[axis] + bounds[3 + axis]);

		count[axis] = size > step ? (unsigned long)ceilf(size / step) : 1;
		header->origin[axis] = middle - 0.5f * (float)count[axis] * step;
	}
	header->width = count[0];
	header->height = count[1];
	header->depth = count[2];
	header->voxel = voxel;
	header->range = WORLD_SDF_RANGE_VOXELS * voxel;
	header->brick_count = 0;
}

unsigned long world_sdf_bytes(struct world_sdf_header const *header, unsigned long bricks)
{
	unsigned long cells = header->width * header->height * header->depth;

	return cells * WORLD_SDF_CELL_BYTES +
		(bricks + WORLD_SDF_ATLAS_LAYER - 1) / WORLD_SDF_ATLAS_LAYER * WORLD_SDF_ATLAS_LAYER_BYTES;
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

/* ---------- distances */

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

/* a triangle's edges, unit normal, plane offset and bounding sphere (about
its centroid), and whether it has no area worth the name (its edges alone,
and no plane) */
struct sdf_triangle
{
	float const *a, *b, *c;
	float ab[3], ac[3], normal[3], offset, centre[3], radius, low[3], high[3];
	int flat;
};

static void sdf_triangle_make(float const *corners, struct sdf_triangle *triangle)
{
	float const *a = corners, *b = corners + 3, *c = corners + 6, *each[3];
	float length;
	int axis, corner;

	triangle->a = a;
	triangle->b = b;
	triangle->c = c;
	for (axis = 0; axis < 3; axis++)
	{
		triangle->ab[axis] = b[axis] - a[axis];
		triangle->ac[axis] = c[axis] - a[axis];
		triangle->centre[axis] = (a[axis] + b[axis] + c[axis]) / 3.0f;
		triangle->low[axis] = fminf(a[axis], fminf(b[axis], c[axis]));
		triangle->high[axis] = fmaxf(a[axis], fmaxf(b[axis], c[axis]));
	}
	triangle->normal[0] = triangle->ab[1] * triangle->ac[2] - triangle->ab[2] * triangle->ac[1];
	triangle->normal[1] = triangle->ab[2] * triangle->ac[0] - triangle->ab[0] * triangle->ac[2];
	triangle->normal[2] = triangle->ab[0] * triangle->ac[1] - triangle->ab[1] * triangle->ac[0];
	length = sqrtf(sdf_dot(triangle->normal, triangle->normal));
	triangle->flat = !(length > 1.0e-6f * (sdf_dot(triangle->ab, triangle->ab) + sdf_dot(triangle->ac, triangle->ac)));
	if (!triangle->flat)
	{
		for (axis = 0; axis < 3; axis++)
			triangle->normal[axis] /= length;
	}
	triangle->offset = triangle->flat ? 0.0f : sdf_dot(triangle->normal, a);
	each[0] = a;
	each[1] = b;
	each[2] = c;
	triangle->radius = 0.0f;
	for (corner = 0; corner < 3; corner++)
	{
		float d[3], squared;

		for (axis = 0; axis < 3; axis++)
			d[axis] = each[corner][axis] - triangle->centre[axis];
		squared = sdf_dot(d, d);
		if (squared > triangle->radius)
			triangle->radius = squared;
	}
	triangle->radius = sqrtf(triangle->radius);
}

static float sdf_triangle_distance_squared(struct sdf_triangle const *triangle, float const point[3])
{
	return sdf_triangle_squared(point, triangle->a, triangle->b, triangle->c, triangle->ab, triangle->ac,
		triangle->flat);
}

/* the indices along an axis of points origin + step * i (i 0 to count - 1)
from low to high (world units), first to last inclusive; FALSE if none */
static int sdf_span(float low, float high, float origin, float step, long count, long *first, long *last)
{
	float from = ceilf((low - origin) / step), to = floorf((high - origin) / step);

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

/* along a row of points (y and z fixed) within reach of the triangle's
plane and of its bounding sphere grown by reach: the x span (world units)
low to high narrowed; FALSE if none of the row is */
static int sdf_row(struct sdf_triangle const *triangle, float y, float z, float reach, float *low, float *high)
{
	float dy = y - triangle->centre[1], dz = z - triangle->centre[2];
	float sphere = triangle->radius + reach;
	float across = sphere * sphere - dy * dy - dz * dz, plane_row;

	if (across <= 0.0f)
		return 0;
	across = sqrtf(across);
	if (triangle->centre[0] - across > *low)
		*low = triangle->centre[0] - across;
	if (triangle->centre[0] + across < *high)
		*high = triangle->centre[0] + across;
	if (triangle->flat)
		return *low <= *high;
	plane_row = triangle->normal[1] * y + triangle->normal[2] * z - triangle->offset;
	if (fabsf(triangle->normal[0]) > 1.0e-6f)
	{
		float one = (-reach - plane_row) / triangle->normal[0], other = (reach - plane_row) / triangle->normal[0];

		if (one > other)
		{
			float swap = one;

			one = other;
			other = swap;
		}
		if (one > *low)
			*low = one;
		if (other < *high)
			*high = other;
	}
	else if (fabsf(plane_row) >= reach)
	{
		return 0;
	}
	return *low <= *high;
}

/* ---------- the cells a surface may be within range of */

enum
{
	_sdf_cells_mark,
	_sdf_cells_count,
	_sdf_cells_fill,
};

/* the cells of slices z_first to z_end - 1 within range and half a cell's
diagonal of a triangle (from their middles): marked, counted for it
(firsts[cell + 1]++, a marked one), or given it (indices[firsts[cell]++]);
how many */
static long sdf_cells_near(struct world_sdf_header const *header, struct sdf_triangle const *triangle,
	long triangle_index, int what, unsigned char *marks, long *firsts, long *indices, long z_first, long z_end)
{
	float step = WORLD_SDF_BRICK * header->voxel;
	float reach = header->range + 0.5f * step * 1.7320508f;
	long count[3], first[3], last[3], x, y, z, found = 0;
	float middle[3];
	int axis;

	count[0] = (long)header->width;
	count[1] = (long)header->height;
	count[2] = (long)header->depth;
	for (axis = 0; axis < 3; axis++)
	{
		middle[axis] = header->origin[axis] + 0.5f * step;
		if (!sdf_span(triangle->low[axis] - reach, triangle->high[axis] + reach, middle[axis], step, count[axis],
			&first[axis], &last[axis]))
		{
			return 0;
		}
	}
	if (first[2] < z_first)
		first[2] = z_first;
	if (last[2] > z_end - 1)
		last[2] = z_end - 1;
	for (z = first[2]; z <= last[2]; z++)
	{
		float point_z = middle[2] + (float)z * step;

		for (y = first[1]; y <= last[1]; y++)
		{
			float point_y = middle[1] + (float)y * step;
			float low = middle[0] + (float)first[0] * step, high = middle[0] + (float)last[0] * step;
			long row_first, row_last;

			if (!sdf_row(triangle, point_y, point_z, reach, &low, &high) ||
				!sdf_span(low, high, middle[0], step, count[0], &row_first, &row_last))
			{
				continue;
			}
			for (x = row_first; x <= row_last; x++)
			{
				unsigned long cell = ((unsigned long)z * (unsigned long)count[1] + (unsigned long)y) *
					(unsigned long)count[0] + (unsigned long)x;
				float point[3];

				if (what != _sdf_cells_mark && !marks[cell])
					continue;
				point[0] = middle[0] + (float)x * step;
				point[1] = point_y;
				point[2] = point_z;
				if (sdf_triangle_distance_squared(triangle, point) >= reach * reach)
					continue;
				found++;
				if (what == _sdf_cells_mark)
					marks[cell] = 1;
				else if (what == _sdf_cells_count)
					firsts[cell + 1]++;
				else
					indices[firsts[cell]++] = triangle_index;
			}
		}
	}
	return found;
}

/* whether a triangle may reach slices z_first to z_end - 1 (its corners' z
against theirs grown by reach), before it is made */
static int sdf_triangle_in_slices(struct world_sdf_header const *header, float const *corners, long z_first,
	long z_end)
{
	float step = WORLD_SDF_BRICK * header->voxel, reach = header->range + step;
	float low = fminf(corners[2], fminf(corners[5], corners[8])), high = fmaxf(corners[2], fmaxf(corners[5], corners[8]));

	return high + reach >= header->origin[2] + (float)z_first * step &&
		low - reach <= header->origin[2] + (float)z_end * step;
}

long world_sdf_mark_slices(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char *marks, long z_first, long z_end)
{
	unsigned long slice = header->width * header->height, cell;
	long triangle, count = 0;

	memset(marks + (unsigned long)z_first * slice, 0, (unsigned long)(z_end - z_first) * slice);
	for (triangle = 0; triangle < triangle_count; triangle++)
	{
		struct sdf_triangle made;

		if (!sdf_triangle_in_slices(header, positions + triangle * 9, z_first, z_end))
			continue;
		sdf_triangle_make(positions + triangle * 9, &made);
		sdf_cells_near(header, &made, triangle, _sdf_cells_mark, marks, NULL, NULL, z_first, z_end);
	}
	for (cell = (unsigned long)z_first * slice; cell < (unsigned long)z_end * slice; cell++)
		count += marks[cell];
	return count;
}

long world_sdf_mark(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char *marks)
{
	return world_sdf_mark_slices(header, positions, triangle_count, marks, 0, (long)header->depth);
}

void world_sdf_bin_slices(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char const *marks, long *firsts, long *indices, long z_first, long z_end)
{
	long triangle;

	for (triangle = 0; triangle < triangle_count; triangle++)
	{
		struct sdf_triangle made;

		if (!sdf_triangle_in_slices(header, positions + triangle * 9, z_first, z_end))
			continue;
		sdf_triangle_make(positions + triangle * 9, &made);
		sdf_cells_near(header, &made, triangle, indices ? _sdf_cells_fill : _sdf_cells_count, (unsigned char *)marks,
			firsts, indices, z_first, z_end);
	}
}

long *world_sdf_bin(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char const *marks, long **firsts_out)
{
	unsigned long cells = header->width * header->height * header->depth;
	long *firsts = calloc(cells + 1, sizeof(*firsts)), *indices;

	*firsts_out = NULL;
	if (!firsts)
		return NULL;
	world_sdf_bin_slices(header, positions, triangle_count, marks, firsts, NULL, 0, (long)header->depth);
	indices = world_sdf_bin_place(header, firsts);
	if (!indices)
	{
		free(firsts);
		return NULL;
	}
	world_sdf_bin_slices(header, positions, triangle_count, marks, firsts, indices, 0, (long)header->depth);
	world_sdf_bin_placed(header, firsts);
	*firsts_out = firsts;
	return indices;
}

long *world_sdf_bin_place(struct world_sdf_header const *header, long *firsts)
{
	unsigned long cells = header->width * header->height * header->depth, cell;

	for (cell = 0; cell < cells; cell++)
		firsts[cell + 1] += firsts[cell];
	return malloc(((size_t)firsts[cells] + 1) * sizeof(long));
}

void world_sdf_bin_placed(struct world_sdf_header const *header, long *firsts)
{
	unsigned long cell;

	/* (each cell's start, moved up as its triangles were put: put back) */
	for (cell = header->width * header->height * header->depth; cell > 0; cell--)
		firsts[cell] = firsts[cell - 1];
	firsts[0] = 0;
}

static long sdf_mark_all(void *context, struct world_sdf_header const *header, float const *positions,
	long triangle_count, unsigned char *marks)
{
	(void)context;
	return world_sdf_mark(header, positions, triangle_count, marks);
}

unsigned char *world_sdf_plan(float const bounds[6], float const *positions, long triangle_count,
	struct world_sdf_header *header, world_sdf_marker mark, void *context)
{
	float voxel = (float)WORLD_SDF_FINEST_VOXEL_MILLI / 1000.0f;
	unsigned char *marks = NULL;

	if (!mark)
		mark = sdf_mark_all;
	for (;;)
	{
		unsigned long cells, bytes;
		unsigned char *grown;
		long marked;
		int steps;

		world_sdf_layout(bounds, voxel, header);
		cells = header->width * header->height * header->depth;
		if (header->width > WORLD_SDF_MAXIMUM_DIMENSION || header->height > WORLD_SDF_MAXIMUM_DIMENSION ||
			header->depth > WORLD_SDF_MAXIMUM_DIMENSION || world_sdf_bytes(header, 0) > WORLD_SDF_MAXIMUM_BYTES)
		{
			voxel *= 1.125f;
			continue;
		}
		grown = realloc(marks, cells);
		if (!grown)
		{
			free(marks);
			return NULL;
		}
		marks = grown;
		marked = mark(context, header, positions, triangle_count, marks);
		bytes = world_sdf_bytes(header, (unsigned long)marked);
		if (bytes <= WORLD_SDF_MAXIMUM_BYTES)
			return marks;
		/* (the bricks' memory falls at least as the voxel's square grows: a
		step short of as many steps as that says, so that none finer that
		fits is passed over) */
		{
			double predicted = (double)bytes;

			for (steps = 0; predicted > (double)WORLD_SDF_MAXIMUM_BYTES && steps < 8; steps++)
				predicted /= 1.125 * 1.125;
		}
		for (steps = steps > 1 ? steps - 1 : 1; steps > 0; steps--)
			voxel *= 1.125f;
	}
}

/* ---------- a cell's brick */

static void sdf_cell_place(struct world_sdf_header const *header, unsigned long cell, long base[3])
{
	base[0] = (long)(cell % header->width) * WORLD_SDF_BRICK;
	base[1] = (long)(cell / header->width % header->height) * WORLD_SDF_BRICK;
	base[2] = (long)(cell / (header->width * header->height)) * WORLD_SDF_BRICK;
}

/* a voxel's centre (its whole grid's indices) */
static void sdf_voxel_centre(struct world_sdf_header const *header, long x, long y, long z, float point[3])
{
	point[0] = header->origin[0] + (float)x * header->voxel;
	point[1] = header->origin[1] + (float)y * header->voxel;
	point[2] = header->origin[2] + (float)z * header->voxel;
}

int world_sdf_bake_cell(struct world_sdf_header const *header, unsigned long cell, float const *positions,
	long const *triangles, long triangle_count, int (*inside)(void *context, float const point[3]),
	void *context, signed char *brick)
{
	float best[WORLD_SDF_BRICK_VOXELS], voxel = header->voxel, range = header->range;
	long base[3], index, x, y, z;
	int outside_all = 1, inside_all = 1;

	sdf_cell_place(header, cell, base);
	for (index = 0; index < WORLD_SDF_BRICK_VOXELS; index++)
		best[index] = range;
	for (index = 0; index < triangle_count; index++)
	{
		struct sdf_triangle triangle;
		long first[3], last[3];
		int axis;

		sdf_triangle_make(positions + triangles[index] * 9, &triangle);
		for (axis = 0; axis < 3; axis++)
		{
			float origin = header->origin[axis] + (float)base[axis] * voxel;

			if (!sdf_span(triangle.low[axis] - range, triangle.high[axis] + range, origin, voxel,
				WORLD_SDF_BRICK_SPAN, &first[axis], &last[axis]))
			{
				break;
			}
		}
		if (axis < 3)
			continue;
		for (z = first[2]; z <= last[2]; z++)
		{
			float point_z = header->origin[2] + (float)(base[2] + z) * voxel;

			for (y = first[1]; y <= last[1]; y++)
			{
				float point_y = header->origin[1] + (float)(base[1] + y) * voxel;
				float low = header->origin[0] + (float)(base[0] + first[0]) * voxel;
				float high = header->origin[0] + (float)(base[0] + last[0]) * voxel;
				float plane_row = triangle.normal[1] * point_y + triangle.normal[2] * point_z - triangle.offset;
				float *row_best = best + (z * WORLD_SDF_BRICK_SPAN + y) * WORLD_SDF_BRICK_SPAN;
				long row_first, row_last;

				if (!sdf_row(&triangle, point_y, point_z, range, &low, &high) ||
					!sdf_span(low, high, header->origin[0] + (float)base[0] * voxel, voxel, WORLD_SDF_BRICK_SPAN,
					&row_first, &row_last))
				{
					continue;
				}
				for (x = row_first; x <= row_last; x++)
				{
					float point[3], distance;

					point[0] = header->origin[0] + (float)(base[0] + x) * voxel;
					/* (never nearer than its plane) */
					if (!triangle.flat && fabsf(plane_row + triangle.normal[0] * point[0]) >= row_best[x])
						continue;
					point[1] = point_y;
					point[2] = point_z;
					distance = sdf_triangle_distance_squared(&triangle, point);
					if (distance < row_best[x] * row_best[x])
						row_best[x] = sqrtf(distance);
				}
			}
		}
	}
	/* the sign, and the encoding */
	for (z = 0, index = 0; z < WORLD_SDF_BRICK_SPAN; z++)
	{
		for (y = 0; y < WORLD_SDF_BRICK_SPAN; y++)
		{
			for (x = 0; x < WORLD_SDF_BRICK_SPAN; x++, index++)
			{
				float point[3], distance = best[index];

				sdf_voxel_centre(header, base[0] + x, base[1] + y, base[2] + z, point);
				if (inside && inside(context, point))
					distance = -distance;
				brick[index] = world_sdf_encode(distance, range);
				outside_all = outside_all && brick[index] == 127;
				inside_all = inside_all && brick[index] == -127;
			}
		}
	}
	return outside_all ? WORLD_SDF_CELL_OUTSIDE : inside_all ? WORLD_SDF_CELL_INSIDE : WORLD_SDF_CELL_BRICK;
}

int world_sdf_cell_kind(struct world_sdf_header const *header, unsigned long cell,
	int (*inside)(void *context, float const point[3]), void *context)
{
	long base[3];
	float point[3];

	sdf_cell_place(header, cell, base);
	sdf_voxel_centre(header, base[0] + WORLD_SDF_BRICK / 2, base[1] + WORLD_SDF_BRICK / 2,
		base[2] + WORLD_SDF_BRICK / 2, point);
	return inside && inside(context, point) ? WORLD_SDF_CELL_INSIDE : WORLD_SDF_CELL_OUTSIDE;
}

/* ---------- a field of a whole grid */

int world_sdf_pack(struct world_sdf_volume *volume, signed char const *voxels)
{
	struct world_sdf_header *header = &volume->header;
	unsigned long cells = header->width * header->height * header->depth, cell;
	unsigned long row = WORLD_SDF_BRICK * header->width + 1, slice = row * (WORLD_SDF_BRICK * header->height + 1);
	signed char brick[WORLD_SDF_BRICK_VOXELS];
	int pass;

	volume->cells = calloc(cells, sizeof(*volume->cells));
	volume->bricks = NULL;
	memset(&volume->scenery, 0, sizeof(volume->scenery));
	if (!volume->cells)
		return 0;
	/* (counted, then copied) */
	for (pass = 0; pass < 2; pass++)
	{
		header->brick_count = 0;
		for (cell = 0; cell < cells; cell++)
		{
			long base[3], x, y, z, index = 0;
			int outside_all = 1, inside_all = 1;

			sdf_cell_place(header, cell, base);
			for (z = 0; z < WORLD_SDF_BRICK_SPAN; z++)
			{
				for (y = 0; y < WORLD_SDF_BRICK_SPAN; y++)
				{
					for (x = 0; x < WORLD_SDF_BRICK_SPAN; x++, index++)
					{
						brick[index] = voxels[(unsigned long)(base[2] + z) * slice + (unsigned long)(base[1] + y) * row +
							(unsigned long)(base[0] + x)];
						outside_all = outside_all && brick[index] == 127;
						inside_all = inside_all && brick[index] == -127;
					}
				}
			}
			if (outside_all || inside_all)
			{
				volume->cells[cell][3] = outside_all ? WORLD_SDF_CELL_OUTSIDE : WORLD_SDF_CELL_INSIDE;
				continue;
			}
			if (pass)
			{
				unsigned long n = header->brick_count;

				volume->cells[cell][0] = (unsigned char)(n % WORLD_SDF_ATLAS);
				volume->cells[cell][1] = (unsigned char)(n / WORLD_SDF_ATLAS % WORLD_SDF_ATLAS);
				volume->cells[cell][2] = (unsigned char)(n / WORLD_SDF_ATLAS_LAYER);
				volume->cells[cell][3] = WORLD_SDF_CELL_BRICK;
				memcpy(volume->bricks + n * WORLD_SDF_BRICK_VOXELS, brick, sizeof(brick));
			}
			header->brick_count++;
		}
		if (!pass)
		{
			/* (the atlas's layers are at most 256: the table's z is a byte) */
			if (header->brick_count > 256UL * WORLD_SDF_ATLAS_LAYER ||
				!(volume->bricks = malloc(header->brick_count * WORLD_SDF_BRICK_VOXELS + 1)))
			{
				free(volume->cells);
				volume->cells = NULL;
				return 0;
			}
		}
	}
	return 1;
}

/* ---------- the sample, and the GPU's atlas */

float world_sdf_sample(struct world_sdf_volume const *volume, float const point[3])
{
	struct world_sdf_header const *header = &volume->header;
	unsigned long size[3];
	long cell[3], base[3];
	float local[3], fraction[3], result = 0.0f;
	unsigned char const *entry;
	signed char const *brick;
	int axis, corner;

	size[0] = header->width;
	size[1] = header->height;
	size[2] = header->depth;
	for (axis = 0; axis < 3; axis++)
	{
		/* (in voxels from the first voxel's centre, clamped to the edge,
		as a GPU's linear filter with clamp-to-edge reads the voxels; the
		cell, and where in it) */
		float coordinate = (point[axis] - header->origin[axis]) / header->voxel;
		float last = (float)(WORLD_SDF_BRICK * size[axis]);

		if (coordinate < 0.0f)
			coordinate = 0.0f;
		if (coordinate > last)
			coordinate = last;
		cell[axis] = (long)floorf(coordinate / (float)WORLD_SDF_BRICK);
		if (cell[axis] > (long)size[axis] - 1)
			cell[axis] = (long)size[axis] - 1;
		local[axis] = coordinate - (float)(cell[axis] * WORLD_SDF_BRICK);
		base[axis] = (long)floorf(local[axis]);
		if (base[axis] > WORLD_SDF_BRICK - 1)
			base[axis] = WORLD_SDF_BRICK - 1;
		fraction[axis] = local[axis] - (float)base[axis];
	}
	entry = volume->cells[((unsigned long)cell[2] * size[1] + (unsigned long)cell[1]) * size[0] +
		(unsigned long)cell[0]];
	if (entry[3] != WORLD_SDF_CELL_BRICK)
		return entry[3] == WORLD_SDF_CELL_INSIDE ? -header->range : header->range;
	brick = volume->bricks + ((unsigned long)entry[0] + (unsigned long)entry[1] * WORLD_SDF_ATLAS +
		(unsigned long)entry[2] * WORLD_SDF_ATLAS_LAYER) * WORLD_SDF_BRICK_VOXELS;
	for (corner = 0; corner < 8; corner++)
	{
		long x = base[0] + (corner & 1), y = base[1] + ((corner >> 1) & 1), z = base[2] + ((corner >> 2) & 1);
		float weight = ((corner & 1) ? fraction[0] : 1.0f - fraction[0]) *
			(((corner >> 1) & 1) ? fraction[1] : 1.0f - fraction[1]) *
			(((corner >> 2) & 1) ? fraction[2] : 1.0f - fraction[2]);

		if (weight == 0.0f)
			continue;
		result += weight * world_sdf_decode(brick[(z * WORLD_SDF_BRICK_SPAN + y) * WORLD_SDF_BRICK_SPAN + x],
			header->range);
	}
	return result;
}

unsigned long world_sdf_atlas_layers(struct world_sdf_header const *header)
{
	return (header->brick_count + WORLD_SDF_ATLAS_LAYER - 1) / WORLD_SDF_ATLAS_LAYER;
}

void world_sdf_atlas_layer(struct world_sdf_volume const *volume, unsigned long layer, signed char *voxels)
{
	unsigned long row = WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN, slice = row * row, index;

	memset(voxels, 0, WORLD_SDF_ATLAS_LAYER_BYTES);
	for (index = 0; index < WORLD_SDF_ATLAS_LAYER; index++)
	{
		unsigned long n = layer * WORLD_SDF_ATLAS_LAYER + index;
		unsigned long at = index % WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN +
			index / WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN * row;
		signed char const *brick;
		int y, z;

		if (n >= volume->header.brick_count)
			break;
		brick = volume->bricks + n * WORLD_SDF_BRICK_VOXELS;
		for (z = 0; z < WORLD_SDF_BRICK_SPAN; z++)
		{
			for (y = 0; y < WORLD_SDF_BRICK_SPAN; y++)
			{
				memcpy(voxels + z * slice + at + y * row, brick + (z * WORLD_SDF_BRICK_SPAN + y) * WORLD_SDF_BRICK_SPAN,
					WORLD_SDF_BRICK_SPAN);
			}
		}
	}
}
