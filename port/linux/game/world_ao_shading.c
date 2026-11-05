/*
WORLD_AO_SHADING.C

display.ambient_occlusion "world"'s shading on the CPU (world_ao.h, "the
arithmetic"; docs/world-ao.md, "Shading"): the occlusion a pixel's ambient
light is multiplied by, as the shaders find it (port/linux/src/xgpu.h's
XGPU_AMBIENT_OCCLUSION_WORLD; the emitters' world_ao_*, operation for
operation), and an object's reference, which world_ao.c hands the shaders
with its draws. Pure: the field's sample is world_sdf_sample's
(world_sdf_bake.c), a proxy's occlusion and reach world_proxies.c's.
Tested against both emitters' shaders (tools/test_world_ao_light.py).
Workstream D.
*/

#include "world_ao.h"
#include "world_proxies.h"
#include "world_sdf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float saturate(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float mix(float a, float b, float t)
{
	return a + (b - a) * t;
}

float world_ao_field_occlusion(struct world_sdf_volume const *volume, float const point[3], float const normal[3])
{
	float first, occlusion = 0.0f;
	int tap, axis;

	if (!volume)
		return 1.0f;
	first = volume->header.voxel > WORLD_AO_STEP ? volume->header.voxel : WORLD_AO_STEP;
	for (tap = 0; tap < WORLD_AO_TAPS; tap++)
	{
		float h = first * (float)(1 << tap), at[3];

		for (axis = 0; axis < 3; axis++)
			at[axis] = point[axis] + normal[axis] * h;
		occlusion += 1.0f / (float)(2 << tap) * saturate((h - world_sdf_sample(volume, at)) / h);
	}
	return 1.0f - saturate(WORLD_AO_K * occlusion);
}

float world_ao_reference(struct world_sdf_volume const *volume, float const lighting_point[3])
{
	static const float up[3] = { 0.0f, 0.0f, 1.0f };
	float above[3];

	if (!volume)
		return 1.0f;
	above[0] = lighting_point[0];
	above[1] = lighting_point[1];
	above[2] = lighting_point[2] + (volume->header.voxel > WORLD_AO_STEP ? volume->header.voxel : WORLD_AO_STEP);
	return world_ao_field_occlusion(volume, above, up);
}

/* the radius of a sphere about a shape's centre that holds it */
static float world_ao_proxy_bound(struct world_proxy const *proxy)
{
	float axis[3];
	int k;

	for (k = 0; k < 3; k++)
		axis[k] = proxy->v[1][k] - proxy->v[0][k];
	switch ((int)proxy->v[0][3])
	{
	case _world_proxy_capsule:
		return 0.5f * sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) + proxy->v[2][0];
	case _world_proxy_box:
		return sqrtf(proxy->v[1][0] * proxy->v[1][0] + proxy->v[1][1] * proxy->v[1][1] + proxy->v[1][2] * proxy->v[1][2] +
			proxy->v[2][0] * proxy->v[2][0] + proxy->v[2][1] * proxy->v[2][1] + proxy->v[2][2] * proxy->v[2][2] +
			proxy->v[2][3] * proxy->v[2][3]);
	default:
		return proxy->v[2][0];
	}
}

/* one proxy's factor of A_prox at a point (1 - occlusion x fade); FALSE
none (its owner's, past its reach, wholly below the tangent plane, or a box
whose bound is under 1/510: the product as it was). The brute-force loop
and the grid's call it alike, in the same order */
static int world_ao_proxy_factor(struct world_proxy const *proxy, float const point[3], float const normal[3],
	float owner, float *factor)
{
	float centre[3], offset[3], reach, away, fade;
	int axis;

	if (proxy->v[1][3] == owner)
		return 0;
	/* (from its centre: a capsule's segment's middle, a sphere's or a
	box's a) */
	for (axis = 0; axis < 3; axis++)
	{
		centre[axis] = (int)proxy->v[0][3] == _world_proxy_capsule ?
			0.5f * (proxy->v[0][axis] + proxy->v[1][axis]) : proxy->v[0][axis];
		offset[axis] = point[axis] - centre[axis];
	}
	reach = world_proxy_reach(proxy);
	away = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
	if (away >= reach)
		return 0;
	/* (wholly below the point's tangent plane: none) */
	if (normal[0] * -offset[0] + normal[1] * -offset[1] + normal[2] * -offset[2] < -world_ao_proxy_bound(proxy))
		return 0;
	/* (a sphere and a capsule fade out over the last of their reach; a box
	by its bound (world_proxy_box_fade), which is under 1/510 well inside
	its reach on a floor beside it or past its thin sides) */
	if ((int)proxy->v[0][3] == _world_proxy_box)
	{
		fade = world_proxy_box_fade(proxy, point, normal);
		if (fade <= 0.0f)
			return 0;
	}
	else
	{
		fade = saturate((reach - away) / (reach * WORLD_AO_REACH_FADE));
	}
	*factor = 1.0f - world_proxy_occlusion(proxy, point, normal) * fade;
	return 1;
}

float world_ao_proxies_occlusion(struct world_proxy const *proxies, long count, float const point[3],
	float const normal[3], float owner)
{
	float product = 1.0f, factor;
	long index;

	for (index = 0; index < count; index++)
	{
		if (world_ao_proxy_factor(&proxies[index], point, normal, owner, &factor))
			product *= factor;
	}
	return product;
}

/* ---------- the window's proxy grid */

static const int world_ao_grid_sizes[3] = { WORLD_AO_GRID_X, WORLD_AO_GRID_Y, WORLD_AO_GRID_Z };

/* a cell's index along an axis for an offset from the camera, as the
shaders find it: floor(sign(offset) log2(1 + |offset| grid[1].w) grid[1][axis]
+ size / 2), clamped */
static int world_ao_grid_axis(float const grid[2][4], int axis, float offset)
{
	int size = world_ao_grid_sizes[axis];
	float cell = log2f(1.0f + fabsf(offset) * grid[1][3]) * grid[1][axis];
	int index = (int)floorf((offset > 0.0f ? cell : offset < 0.0f ? -cell : 0.0f) + (float)(size / 2));

	return index < 0 ? 0 : index >= size ? size - 1 : index;
}

int world_ao_grid_cell(float const grid[2][4], float const point[3])
{
	int index[3], axis;

	for (axis = 0; axis < 3; axis++)
		index[axis] = world_ao_grid_axis(grid, axis, point[axis] - grid[0][axis]);
	return (index[2] * WORLD_AO_GRID_Y + index[1]) * WORLD_AO_GRID_X + index[0];
}

/* the half extents along the world's axes of the box about a proxy's
centre (world_proxy_reach's) outside which it is never computed: its
reach's; a box's own bounds grown by its reach less its half diagonal
(where its bound falls under 1/510), where that is less */
static void world_ao_grid_extent(struct world_proxy const *proxy, float reach, float half[3])
{
	float ex, ey, ez, grow, z_axis[3];
	int k;

	for (k = 0; k < 3; k++)
		half[k] = reach;
	if ((int)proxy->v[0][3] != _world_proxy_box)
		return;
	ex = sqrtf(proxy->v[1][0] * proxy->v[1][0] + proxy->v[1][1] * proxy->v[1][1] + proxy->v[1][2] * proxy->v[1][2]);
	ey = sqrtf(proxy->v[2][0] * proxy->v[2][0] + proxy->v[2][1] * proxy->v[2][1] + proxy->v[2][2] * proxy->v[2][2]);
	ez = proxy->v[2][3];
	if (ex <= 0.0f || ey <= 0.0f || ez <= 0.0f)
		return;
	grow = reach - sqrtf(ex * ex + ey * ey + ez * ez);
	z_axis[0] = (proxy->v[1][1] * proxy->v[2][2] - proxy->v[1][2] * proxy->v[2][1]) / (ex * ey);
	z_axis[1] = (proxy->v[1][2] * proxy->v[2][0] - proxy->v[1][0] * proxy->v[2][2]) / (ex * ey);
	z_axis[2] = (proxy->v[1][0] * proxy->v[2][1] - proxy->v[1][1] * proxy->v[2][0]) / (ex * ey);
	for (k = 0; k < 3; k++)
	{
		float extent = fabsf(proxy->v[1][k]) + fabsf(proxy->v[2][k]) + ez * fabsf(z_axis[k]) + grow;

		half[k] = extent < reach ? extent : reach;
	}
}

void world_ao_grid(struct world_proxy const *proxies, long count, float const camera[3], float grid[2][4],
	unsigned int cells[WORLD_AO_GRID_CELLS / 2][4])
{
	long index;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		grid[0][axis] = camera[axis];
		grid[1][axis] = (float)(world_ao_grid_sizes[axis] / 2) / log2f(1.0f + WORLD_AO_GRID_REACH / WORLD_AO_GRID_CELL);
	}
	grid[0][3] = 0.0f;
	grid[1][3] = 1.0f / WORLD_AO_GRID_CELL;
	memset(cells, 0, sizeof(unsigned int) * 4 * (WORLD_AO_GRID_CELLS / 2));
	if (count > 64)
		count = 64;
	for (index = 0; index < count; index++)
	{
		struct world_proxy const *proxy = &proxies[index];
		float centre[3], half[3], reach = world_proxy_reach(proxy);
		int first[3], last[3], x, y, z, k;

		world_ao_grid_extent(proxy, reach, half);
		for (k = 0; k < 3; k++)
		{
			/* (a little past it, so that a pixel at its edge that the GPU's
			log2 rounds into the next cell finds it there too) */
			float pad;

			centre[k] = (int)proxy->v[0][3] == _world_proxy_capsule ?
				0.5f * (proxy->v[0][k] + proxy->v[1][k]) : proxy->v[0][k];
			pad = 1.0e-3f * (half[k] + fabsf(centre[k] - camera[k])) + 1.0e-3f;
			first[k] = world_ao_grid_axis((float const (*)[4])grid, k, centre[k] - half[k] - pad - camera[k]);
			last[k] = world_ao_grid_axis((float const (*)[4])grid, k, centre[k] + half[k] + pad - camera[k]);
		}
		for (z = first[2]; z <= last[2]; z++)
		{
			for (y = first[1]; y <= last[1]; y++)
			{
				for (x = first[0]; x <= last[0]; x++)
				{
					int cell = (z * WORLD_AO_GRID_Y + y) * WORLD_AO_GRID_X + x;

					cells[cell >> 1][((cell & 1) << 1) + (int)(index >> 5)] |= 1u << (index & 31);
				}
			}
		}
	}
}

float world_ao_proxies_occlusion_grid(struct world_proxy const *proxies, float const grid[2][4],
	unsigned int const cells[WORLD_AO_GRID_CELLS / 2][4], float const point[3], float const normal[3], float owner)
{
	int cell = world_ao_grid_cell(grid, point), word, bit;
	float product = 1.0f, factor;

	/* (lowest first, as the shaders' findLSB / ctz) */
	for (word = 0; word < 2; word++)
	{
		unsigned int bits = cells[cell >> 1][((cell & 1) << 1) + word];

		for (bit = 0; bit < 32; bit++)
		{
			if ((bits >> bit) & 1u && world_ao_proxy_factor(&proxies[word * 32 + bit], point, normal, owner, &factor))
				product *= factor;
		}
	}
	return product;
}

long world_ao_proxies_walked(float const parameters[2][4], long count, float reference, int lightmap)
{
	long dynamic = (long)parameters[1][1];

	/* (the lightmap pass where its page has no layer: the static scenery's
	shapes too, as with no pages at all) */
	if (lightmap && reference < 0.0f)
		return count;
	return dynamic < count ? dynamic : count;
}

float world_ao_occlusion(float const parameters[2][4], struct world_sdf_volume const *volume,
	struct world_proxy const *proxies, long count, float const point[3], float const normal[3], float owner,
	float reference, int lightmap, float scenery)
{
	float proxied = world_ao_proxies_occlusion(proxies, world_ao_proxies_walked(parameters, count, reference, lightmap),
		point, normal, owner), field;

	if (lightmap)
	{
		/* (the static scenery's, from its page) */
		if (reference >= 0.0f)
			proxied *= scenery;
		return mix(1.0f, proxied > WORLD_AO_FLOOR ? proxied : WORLD_AO_FLOOR, parameters[0][1]);
	}
	field = world_ao_field_occlusion(volume, point, normal);
	field = mix(field, saturate(field / (reference > WORLD_AO_REFERENCE_LEAST ? reference : WORLD_AO_REFERENCE_LEAST)),
		parameters[1][0]);
	field = mix(1.0f, field, parameters[0][2]) * proxied;
	return mix(1.0f, field > WORLD_AO_FLOOR ? field : WORLD_AO_FLOOR, parameters[0][0]);
}

/* ---------- the static scenery's pages (world_sdf.h's struct world_sdf_scenery) */

/* (an owner no shape has: every shape shades the level) */
#define WORLD_AO_SCENERY_OWNER -3.0f
/* (two triangles at a texel farther apart than this, world units: the
page means two places there) */
#define WORLD_AO_SCENERY_APART 0.25f

long world_ao_scenery_page(struct world_proxy const *shapes, long shape_count, float const *triangles,
	long triangle_count, unsigned long width, unsigned long height, unsigned char *texels, long *shared)
{
	unsigned long count = width * height, texel;
	/* (each texel's place once covered, and whether it is: 0 none, 1 a
	triangle's, 2 its neighbours' after) */
	float *places = malloc(count * 3 * sizeof(float));
	unsigned char *covered = calloc(count, 1);
	long triangle, total = 0;
	int pass;

	*shared = 0;
	memset(texels, 255, count);
	if (!places || !covered)
	{
		free(places);
		free(covered);
		return 0;
	}
	for (triangle = 0; triangle < triangle_count; triangle++)
	{
		float const *t = triangles + triangle * WORLD_AO_SCENERY_TRIANGLE;
		float u[3], v[3], area, low[2], high[2];
		long x0, x1, y0, y1, x, y;
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			u[corner] = t[18 + corner * 2] * (float)width;
			v[corner] = t[19 + corner * 2] * (float)height;
		}
		area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
		if (fabsf(area) < 1.0e-12f)
			continue;
		low[0] = fminf(u[0], fminf(u[1], u[2]));
		low[1] = fminf(v[0], fminf(v[1], v[2]));
		high[0] = fmaxf(u[0], fmaxf(u[1], u[2]));
		high[1] = fmaxf(v[0], fmaxf(v[1], v[2]));
		/* (the texels whose centres, x + 0.5, are within) */
		x0 = (long)floorf(low[0] - 0.5f) + 1;
		y0 = (long)floorf(low[1] - 0.5f) + 1;
		x1 = (long)floorf(high[0] - 0.5f);
		y1 = (long)floorf(high[1] - 0.5f);
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 >= (long)width ? (long)width - 1 : x1;
		y1 = y1 >= (long)height ? (long)height - 1 : y1;
		for (y = y0; y <= y1; y++)
		{
			for (x = x0; x <= x1; x++)
			{
				float cu = (float)x + 0.5f, cv = (float)y + 0.5f, b[3], place[3], normal[3], length, value;
				int axis;

				b[1] = ((cu - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (cv - v[0])) / area;
				b[2] = ((u[1] - u[0]) * (cv - v[0]) - (cu - u[0]) * (v[1] - v[0])) / area;
				b[0] = 1.0f - b[1] - b[2];
				if (b[0] < 0.0f || b[1] < 0.0f || b[2] < 0.0f)
					continue;
				for (axis = 0; axis < 3; axis++)
				{
					place[axis] = b[0] * t[axis] + b[1] * t[3 + axis] + b[2] * t[6 + axis];
					normal[axis] = b[0] * t[9 + axis] + b[1] * t[12 + axis] + b[2] * t[15 + axis];
				}
				length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
				if (length < 1.0e-12f)
					continue;
				for (axis = 0; axis < 3; axis++)
					normal[axis] /= length;
				texel = (unsigned long)y * width + (unsigned long)x;
				if (covered[texel])
				{
					float apart[3];

					for (axis = 0; axis < 3; axis++)
						apart[axis] = place[axis] - places[texel * 3 + axis];
					if (apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2] >
						WORLD_AO_SCENERY_APART * WORLD_AO_SCENERY_APART)
					{
						(*shared)++;
					}
					continue;
				}
				value = world_ao_proxies_occlusion(shapes, shape_count, place, normal, WORLD_AO_SCENERY_OWNER);
				texels[texel] = (unsigned char)(saturate(value) * 255.0f + 0.5f);
				memcpy(places + texel * 3, place, sizeof(place));
				covered[texel] = 1;
				total++;
			}
		}
	}
	/* (the texels about the surfaces, which a filter at their edges reads:
	their covered neighbours' mean, two steps out) */
	for (pass = 0; pass < 2; pass++)
	{
		unsigned char mark = (unsigned char)(pass + 2);

		for (texel = 0; texel < count; texel++)
		{
			long x = (long)(texel % width), y = (long)(texel / width), dx, dy, sum = 0, n = 0;

			if (covered[texel])
				continue;
			for (dy = -1; dy <= 1; dy++)
			{
				for (dx = -1; dx <= 1; dx++)
				{
					long nx = x + dx, ny = y + dy;
					unsigned long near;

					if (nx < 0 || ny < 0 || nx >= (long)width || ny >= (long)height)
						continue;
					near = (unsigned long)ny * width + (unsigned long)nx;
					if (covered[near] && covered[near] < mark)
					{
						sum += texels[near];
						n++;
					}
				}
			}
			if (n)
			{
				texels[texel] = (unsigned char)((sum + n / 2) / n);
				covered[texel] = mark;
			}
		}
	}
	free(places);
	free(covered);
	return total;
}
