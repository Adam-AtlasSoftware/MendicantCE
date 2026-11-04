/* display.ambient_occlusion "world"'s distance field: its layout, encoding,
sample and bake (port/linux/game/world_sdf_bake.c), and its cache file and
the sign's copy of the collision BSP's point test (port/linux/game/world_sdf.c;
test_world_sdf.py). The bake against exact distances: a plane, a box room,
a sphere, a box signed by a BSP of its six planes, and a soup of triangles
against every triangle's distance (the narrow band misses none). */

#include "harness.h"

#include <limits.h>
#include <math.h>
#include <unistd.h>

#include "under_test.inc"

/* (world_sdf.c's file functions: the platform's log and the game's zlib,
here a stored copy, which is all the format needs of it) */
static int logged;
static void platform_log(char const *format, ...)
{
	(void)format;
	logged++;
}

static int compress2(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size, int level)
{
	(void)level;
	if (*destination_size < source_size + 4)
		return -5;
	destination[0] = 0x5A;
	destination[1] = (unsigned char)source_size;
	destination[2] = (unsigned char)(source_size >> 8);
	destination[3] = (unsigned char)(source_size >> 16);
	memcpy(destination + 4, source, source_size);
	*destination_size = source_size + 4;
	return 0;
}

static int uncompress(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size)
{
	unsigned long size;

	if (source_size < 4 || source[0] != 0x5A)
		return -3;
	size = source[1] | (unsigned long)source[2] << 8 | (unsigned long)source[3] << 16;
	if (size != source_size - 4 || size > *destination_size)
		return -5;
	memcpy(destination, source + 4, size);
	*destination_size = size;
	return 0;
}

#include "file_under_test.inc"

/* ---------- the geometry */

#define MAXIMUM_TRIANGLES 4096

static float positions[MAXIMUM_TRIANGLES * 9];
static long triangle_count;

static void triangle_add(float const a[3], float const b[3], float const c[3])
{
	float *out = positions + triangle_count * 9;

	memcpy(out, a, sizeof(float) * 3);
	memcpy(out + 3, b, sizeof(float) * 3);
	memcpy(out + 6, c, sizeof(float) * 3);
	triangle_count++;
}

static void quad_add(float const a[3], float const b[3], float const c[3], float const d[3])
{
	triangle_add(a, b, c);
	triangle_add(a, c, d);
}

/* an axis-aligned box's six faces */
static void box_add(float const low[3], float const high[3])
{
	float corner[8][3];
	int index;

	for (index = 0; index < 8; index++)
	{
		corner[index][0] = index & 1 ? high[0] : low[0];
		corner[index][1] = index & 2 ? high[1] : low[1];
		corner[index][2] = index & 4 ? high[2] : low[2];
	}
	quad_add(corner[0], corner[1], corner[3], corner[2]);
	quad_add(corner[4], corner[5], corner[7], corner[6]);
	quad_add(corner[0], corner[1], corner[5], corner[4]);
	quad_add(corner[2], corner[3], corner[7], corner[6]);
	quad_add(corner[0], corner[2], corner[6], corner[4]);
	quad_add(corner[1], corner[3], corner[7], corner[5]);
}

static void bounds_of(float bounds[6])
{
	long index;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		bounds[axis] = 1e30f;
		bounds[3 + axis] = -1e30f;
	}
	for (index = 0; index < triangle_count * 3; index++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			float value = positions[index * 3 + axis];

			if (value < bounds[axis])
				bounds[axis] = value;
			if (value > bounds[3 + axis])
				bounds[3 + axis] = value;
		}
	}
}

/* the field baked in slabs of three slices, as threads would */
static void bake(struct world_sdf_volume *volume, int (*inside)(void *, float const *), void *context)
{
	float bounds[6];
	long z;

	bounds_of(bounds);
	memset(&volume->header, 0, sizeof(volume->header));
	world_sdf_layout(bounds, &volume->header);
	volume->voxels = malloc(volume->header.width * volume->header.height * volume->header.depth);
	for (z = 0; z < (long)volume->header.depth; z += 3)
		world_sdf_bake_slab(volume, positions, triangle_count, inside, context, z, z + 3);
}

static void voxel_centre(struct world_sdf_header const *header, unsigned long x, unsigned long y, unsigned long z,
	float point[3])
{
	point[0] = header->origin[0] + (float)x * header->voxel;
	point[1] = header->origin[1] + (float)y * header->voxel;
	point[2] = header->origin[2] + (float)z * header->voxel;
}

static float clamped(float distance, float range)
{
	return distance > range ? range : distance < -range ? -range : distance;
}

/* every voxel's decoded distance within one step of the encoding of the
exact one (clamped to range) */
static void check_field(struct world_sdf_volume const *volume, float (*exact)(float const point[3]))
{
	struct world_sdf_header const *header = &volume->header;
	float step = header->range / 127.0f;
	unsigned long x, y, z;

	for (z = 0; z < header->depth; z++)
		for (y = 0; y < header->height; y++)
			for (x = 0; x < header->width; x++)
			{
				float point[3], expected, found;

				voxel_centre(header, x, y, z, point);
				expected = clamped(exact(point), header->range);
				found = world_sdf_decode(volume->voxels[(z * header->height + y) * header->width + x], header->range);
				CHECK(fabsf(found - expected) <= step * 0.51f + 1.0e-4f, "(%g %g %g): %g, not %g", point[0], point[1],
					point[2], found, expected);
			}
}

/* ---------- the layout, encoding and sample */

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
	/* no triangles: everything far outside */
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

/* ---------- the bake against exact distances */

/* a floor at z = 1 over x, y in 0..6, solid below */
static int below_floor(void *context, float const point[3])
{
	(void)context;
	return point[2] < 1.0f;
}

static float floor_distance(float const point[3])
{
	float dx = point[0] < 0.0f ? -point[0] : point[0] > 6.0f ? point[0] - 6.0f : 0.0f;
	float dy = point[1] < 0.0f ? -point[1] : point[1] > 6.0f ? point[1] - 6.0f : 0.0f;
	float distance = sqrtf(dx * dx + dy * dy + (point[2] - 1.0f) * (point[2] - 1.0f));

	return below_floor(NULL, point) ? -distance : distance;
}

static void plane_case(void)
{
	float const a[3] = { 0.0f, 0.0f, 1.0f }, b[3] = { 6.0f, 0.0f, 1.0f }, c[3] = { 6.0f, 6.0f, 1.0f },
		d[3] = { 0.0f, 6.0f, 1.0f };
	struct world_sdf_volume volume;

	triangle_count = 0;
	quad_add(a, b, c, d);
	bake(&volume, below_floor, NULL);
	CHECK(volume.header.voxel == 0.25f, "%g", volume.header.voxel);
	check_field(&volume, floor_distance);
	free(volume.voxels);
}

/* a room's inside, 0..8 x 0..6 x 0..3: solid all round it */
static float const room_low[3] = { 0.0f, 0.0f, 0.0f }, room_high[3] = { 8.0f, 6.0f, 3.0f };

static int outside_room(void *context, float const point[3])
{
	int axis;

	(void)context;
	for (axis = 0; axis < 3; axis++)
	{
		if (point[axis] < room_low[axis] || point[axis] > room_high[axis])
			return 1;
	}
	return 0;
}

/* a box's signed distance: positive inside the room (air), negative out */
static float box_distance(float const point[3], float const low[3], float const high[3])
{
	float outside = 0.0f, inside = 1e30f;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float below = low[axis] - point[axis], above = point[axis] - high[axis];
		float out = below > above ? below : above;

		if (out > 0.0f)
			outside += out * out;
		if (-out < inside)
			inside = -out;
	}
	return outside > 0.0f ? sqrtf(outside) : inside;
}

static float room_distance(float const point[3])
{
	float distance = box_distance(point, room_low, room_high);

	return outside_room(NULL, point) ? -distance : distance;
}

static void room_case(void)
{
	struct world_sdf_volume volume;

	triangle_count = 0;
	box_add(room_low, room_high);
	bake(&volume, outside_room, NULL);
	check_field(&volume, room_distance);
	free(volume.voxels);
}

/* a sphere of radius 2 about (3, 3, 3), solid inside, of 48 x 24 quads:
the mesh's distance is the sphere's within its flats' depth */
static float const sphere_centre[3] = { 3.0f, 3.0f, 3.0f };

static int inside_sphere(void *context, float const point[3])
{
	float dx = point[0] - sphere_centre[0], dy = point[1] - sphere_centre[1], dz = point[2] - sphere_centre[2];

	(void)context;
	return dx * dx + dy * dy + dz * dz < 4.0f;
}

static void sphere_case(void)
{
	struct world_sdf_volume volume;
	struct world_sdf_header const *header = &volume.header;
	int slice, segment;
	unsigned long x, y, z;

	triangle_count = 0;
	for (slice = 0; slice < 24; slice++)
	{
		for (segment = 0; segment < 48; segment++)
		{
			float corner[4][3];
			int index;

			for (index = 0; index < 4; index++)
			{
				float theta = (float)M_PI * (float)(slice + (index >> 1)) / 24.0f;
				float phi = 2.0f * (float)M_PI * (float)(segment + ((index ^ (index >> 1)) & 1)) / 48.0f;

				corner[index][0] = sphere_centre[0] + 2.0f * sinf(theta) * cosf(phi);
				corner[index][1] = sphere_centre[1] + 2.0f * sinf(theta) * sinf(phi);
				corner[index][2] = sphere_centre[2] + 2.0f * cosf(theta);
			}
			quad_add(corner[0], corner[1], corner[2], corner[3]);
		}
	}
	bake(&volume, inside_sphere, NULL);
	for (z = 0; z < header->depth; z++)
		for (y = 0; y < header->height; y++)
			for (x = 0; x < header->width; x++)
			{
				float point[3], dx, dy, dz, expected, found;

				voxel_centre(header, x, y, z, point);
				dx = point[0] - sphere_centre[0];
				dy = point[1] - sphere_centre[1];
				dz = point[2] - sphere_centre[2];
				expected = clamped(sqrtf(dx * dx + dy * dy + dz * dz) - 2.0f, header->range);
				found = world_sdf_decode(volume.voxels[(z * header->height + y) * header->width + x], header->range);
				/* (within half a voxel: the flats lie up to 0.009 inside) */
				CHECK(fabsf(found - expected) <= header->voxel * 0.5f, "(%g %g %g): %g, not %g", point[0], point[1],
					point[2], found, expected);
			}
	free(volume.voxels);
}

/* a solid box (1..5 x 1..4 x 1..3) in air, signed by a BSP of its six
planes (facing in) on world_sdf.c's copy of the point test: behind any
plane, an empty leaf; in front of each, the next plane's node; in front of
the last, solid */
static float const solid_low[3] = { 1.0f, 1.0f, 1.0f }, solid_high[3] = { 5.0f, 4.0f, 3.0f };

static float solid_distance(float const point[3])
{
	float distance = box_distance(point, solid_low, solid_high);
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (point[axis] < solid_low[axis] || point[axis] > solid_high[axis])
			return distance;
	}
	return -distance;
}

static void bsp_case(void)
{
	long nodes[6][3];
	float planes[6][4];
	struct sdf_bsp bsp;
	struct world_sdf_volume volume;
	int index;

	for (index = 0; index < 6; index++)
	{
		int axis = index >> 1, high = index & 1;

		/* (inward: x >= 1 is n = +x, d = 1; x <= 5 is n = -x, d = -5) */
		planes[index][0] = planes[index][1] = planes[index][2] = 0.0f;
		planes[index][axis] = high ? -1.0f : 1.0f;
		planes[index][3] = high ? -solid_high[axis] : solid_low[axis];
		nodes[index][0] = index;
		/* (behind: outside, an empty leaf 0; in front: the next plane, or
		after the last, solid: -1) */
		nodes[index][1] = LONG_MIN;
		nodes[index][2] = index < 5 ? index + 1 : -1;
	}
	bsp.node_count = 6;
	bsp.plane_count = 6;
	bsp.nodes = nodes;
	bsp.planes = planes;
	{
		float const in[3] = { 3.0f, 2.0f, 2.0f }, out[3] = { 0.5f, 2.0f, 2.0f }, above[3] = { 3.0f, 2.0f, 3.5f };

		CHECK(sdf_bsp_inside(&bsp, in) && !sdf_bsp_inside(&bsp, out) && !sdf_bsp_inside(&bsp, above), "point test");
	}
	triangle_count = 0;
	box_add(solid_low, solid_high);
	bake(&volume, sdf_bsp_inside, &bsp);
	check_field(&volume, solid_distance);
	free(volume.voxels);
}

/* a soup of 300 triangles (a fixed generator): within range, every voxel
is the least of every triangle's exact distance (brute force), the narrow
band's culling missing none */
static float soup_point_triangle(float const p[3], float const *t)
{
	/* (the closest point by the triangle's plane and its edges, as a check
	independent of the bake's regions) */
	float ab[3], ac[3], n[3], ap[3], length, dot, q[3], best = 1e30f;
	float u, v, w, d00, d01, d11, d20, d21, denominator, v1[3];
	int axis, edge;

	for (axis = 0; axis < 3; axis++)
	{
		ab[axis] = t[3 + axis] - t[axis];
		ac[axis] = t[6 + axis] - t[axis];
		ap[axis] = p[axis] - t[axis];
	}
	n[0] = ab[1] * ac[2] - ab[2] * ac[1];
	n[1] = ab[2] * ac[0] - ab[0] * ac[2];
	n[2] = ab[0] * ac[1] - ab[1] * ac[0];
	length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	/* (no area: its edges alone) */
	if (length < 1.0e-6f)
		length = 0.0f;
	dot = length ? (ap[0] * n[0] + ap[1] * n[1] + ap[2] * n[2]) / length : 0.0f;
	for (axis = 0; axis < 3; axis++)
		q[axis] = length ? ap[axis] - dot * n[axis] / length : ap[axis];
	d00 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	d01 = ab[0] * ac[0] + ab[1] * ac[1] + ab[2] * ac[2];
	d11 = ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2];
	d20 = q[0] * ab[0] + q[1] * ab[1] + q[2] * ab[2];
	d21 = q[0] * ac[0] + q[1] * ac[1] + q[2] * ac[2];
	denominator = d00 * d11 - d01 * d01;
	v = (d11 * d20 - d01 * d21) / denominator;
	w = (d00 * d21 - d01 * d20) / denominator;
	u = 1.0f - v - w;
	if (length && u >= 0.0f && v >= 0.0f && w >= 0.0f)
		return fabsf(dot);
	for (edge = 0; edge < 3; edge++)
	{
		float const *a = t + edge * 3, *b = t + ((edge + 1) % 3) * 3;
		float ba[3], pa[3], s, distance;

		for (axis = 0; axis < 3; axis++)
		{
			ba[axis] = b[axis] - a[axis];
			pa[axis] = p[axis] - a[axis];
		}
		s = ba[0] * ba[0] + ba[1] * ba[1] + ba[2] * ba[2];
		s = s > 0.0f ? (pa[0] * ba[0] + pa[1] * ba[1] + pa[2] * ba[2]) / s : 0.0f;
		s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
		for (axis = 0; axis < 3; axis++)
			v1[axis] = pa[axis] - s * ba[axis];
		distance = sqrtf(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
		if (distance < best)
			best = distance;
	}
	return best;
}

static float soup_distance(float const point[3])
{
	float best = 1e30f;
	long index;

	for (index = 0; index < triangle_count; index++)
	{
		float distance = soup_point_triangle(point, positions + index * 9);

		if (distance < best)
			best = distance;
	}
	return best;
}

static void soup_case(void)
{
	unsigned long seed = 12345;
	struct world_sdf_volume volume;
	int index, axis;

	triangle_count = 0;
	for (index = 0; index < 300; index++)
	{
		float corner[3][3], centre[3];
		int vertex;

		for (axis = 0; axis < 3; axis++)
		{
			seed = seed * 1103515245UL + 12345UL;
			centre[axis] = (float)((seed >> 8) & 0xFFFF) / 65535.0f * (axis == 2 ? 4.0f : 10.0f);
		}
		for (vertex = 0; vertex < 3; vertex++)
		{
			for (axis = 0; axis < 3; axis++)
			{
				seed = seed * 1103515245UL + 12345UL;
				/* (from slivers to triangles some voxels across) */
				corner[vertex][axis] = centre[axis] + ((float)((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) *
					(index % 3 == 0 ? 3.0f : 0.8f);
			}
		}
		/* (and every tenth with no area: a line, or a point) */
		if (index % 10 == 9)
		{
			for (axis = 0; axis < 3; axis++)
			{
				corner[2][axis] = 0.5f * (corner[0][axis] + corner[1][axis]);
				if (index % 20 == 19)
					corner[1][axis] = corner[2][axis] = corner[0][axis];
			}
		}
		triangle_add(corner[0], corner[1], corner[2]);
	}
	bake(&volume, nothing_inside, NULL);
	check_field(&volume, soup_distance);
	free(volume.voxels);
}

/* ---------- the cache file */

static char const *cache_path(void)
{
	static char path[256];

	if (!path[0])
		snprintf(path, sizeof(path), "/tmp/world_sdf_harness_%ld.wsdf", (long)getpid());
	return path;
}

static void cache_volume(struct world_sdf_volume *volume)
{
	float const bounds[6] = { -1.0f, 2.0f, 0.0f, 3.0f, 4.0f, 1.5f };
	unsigned long index, count;

	memset(&volume->header, 0, sizeof(volume->header));
	world_sdf_layout(bounds, &volume->header);
	volume->header.magic = WORLD_SDF_MAGIC;
	volume->header.version = WORLD_SDF_VERSION;
	volume->header.map_checksum = 0xC0FFEE12UL;
	volume->header.bsp_index = 2;
	volume->header.bake_milliseconds = 1234;
	volume->header.bake_threads = 7;
	count = volume->header.width * volume->header.height * volume->header.depth;
	volume->voxels = malloc(count);
	for (index = 0; index < count; index++)
		volume->voxels[index] = (signed char)((int)(index * 7 % 255) - 127);
}

static void round_trip_case(void)
{
	struct world_sdf_volume volume, back;
	unsigned long size, count;
	unsigned char *bytes;

	cache_volume(&volume);
	count = volume.header.width * volume.header.height * volume.header.depth;
	bytes = sdf_file_pack(&volume, &size);
	CHECK(bytes && size == 64 + count + 4 + 4, "%lu", size);
	/* (the header field by field, little-endian, at fixed places) */
	CHECK(bytes[0] == 'W' && bytes[1] == 'S' && bytes[2] == 'D' && bytes[3] == 'F', "magic");
	CHECK(sdf_get32(bytes + 8) == 0xC0FFEE12UL && sdf_get32(bytes + 12) == 2, "checksum, bsp");
	CHECK(sdf_get32(bytes + 16) == volume.header.width && sdf_get32(bytes + 20) == volume.header.height &&
		sdf_get32(bytes + 24) == volume.header.depth, "size");
	CHECK(sdf_get_float(bytes + 40) == volume.header.voxel && sdf_get32(bytes + 48) == count + 4, "voxel, deflated");
	CHECK(sdf_get32(bytes + 52) == 1234 && sdf_get32(bytes + 56) == 7, "bake time, threads");
	remove(cache_path());
	CHECK(sdf_file_write(cache_path(), bytes, size), "written");
	CHECK(sdf_cache_load(cache_path(), 0xC0FFEE12UL, 2, &back), "read back");
	CHECK(!memcmp(&back.header.origin, &volume.header.origin, sizeof(volume.header.origin)) &&
		back.header.range == volume.header.range && back.header.width == volume.header.width, "header");
	CHECK(!memcmp(back.voxels, volume.voxels, count), "voxels");
	{
		/* (no .tmp left beside it) */
		char temporary[300];
		FILE *file;

		snprintf(temporary, sizeof(temporary), "%s.tmp", cache_path());
		file = fopen(temporary, "rb");
		CHECK(!file, "temporary left");
	}
	remove(cache_path());
	free(bytes);
	free(back.voxels);
	free(volume.voxels);
}

/* a file failing a check is not taken, and is deleted */
static void rejected(unsigned char const *bytes, unsigned long size, unsigned long checksum, unsigned long bsp,
	char const *why)
{
	struct world_sdf_volume back;
	FILE *file;

	CHECK(sdf_file_write(cache_path(), bytes, size), "written");
	CHECK(!sdf_cache_load(cache_path(), checksum, bsp, &back) && !back.voxels, "%s: taken", why);
	file = fopen(cache_path(), "rb");
	CHECK(!file, "%s: not deleted", why);
}

static void rejection_case(void)
{
	struct world_sdf_volume volume, back;
	unsigned long size;
	unsigned char *bytes, *changed;

	cache_volume(&volume);
	bytes = sdf_file_pack(&volume, &size);
	changed = malloc(size);
	/* (none there: nothing) */
	remove(cache_path());
	CHECK(!sdf_cache_load(cache_path(), 0xC0FFEE12UL, 2, &back), "none");
	rejected(bytes, size, 0xC0FFEE13UL, 2, "another map");
	rejected(bytes, size, 0xC0FFEE12UL, 1, "another bsp");
	rejected(bytes, size - 1, 0xC0FFEE12UL, 2, "cut short");
	rejected(bytes, 40, 0xC0FFEE12UL, 2, "a header's part");
	memcpy(changed, bytes, size);
	changed[0] = 'X';
	rejected(changed, size, 0xC0FFEE12UL, 2, "magic");
	memcpy(changed, bytes, size);
	sdf_put32(changed + 4, WORLD_SDF_VERSION + 1);
	sdf_put32(changed + size - 4, sdf_crc(0, changed, size - 4));
	rejected(changed, size, 0xC0FFEE12UL, 2, "version");
	memcpy(changed, bytes, size);
	changed[64 + 100] ^= 1;
	rejected(changed, size, 0xC0FFEE12UL, 2, "a voxel's bit");
	memcpy(changed, bytes, size);
	sdf_put32(changed + 16, volume.header.width + 1);
	sdf_put32(changed + size - 4, sdf_crc(0, changed, size - 4));
	rejected(changed, size, 0xC0FFEE12UL, 2, "size against the voxels");
	memcpy(changed, bytes, size);
	sdf_put32(changed + 16, 5000);
	sdf_put32(changed + size - 4, sdf_crc(0, changed, size - 4));
	rejected(changed, size, 0xC0FFEE12UL, 2, "too wide");
	/* (and the good one still taken) */
	CHECK(sdf_file_write(cache_path(), bytes, size) && sdf_cache_load(cache_path(), 0xC0FFEE12UL, 2, &back), "good");
	remove(cache_path());
	free(back.voxels);
	free(changed);
	free(bytes);
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
	else if (!strcmp(name, "plane"))
		plane_case();
	else if (!strcmp(name, "room"))
		room_case();
	else if (!strcmp(name, "sphere"))
		sphere_case();
	else if (!strcmp(name, "bsp"))
		bsp_case();
	else if (!strcmp(name, "soup"))
		soup_case();
	else if (!strcmp(name, "round_trip"))
		round_trip_case();
	else if (!strcmp(name, "rejection"))
		rejection_case();
	else
		return 2;
	return 0;
}
