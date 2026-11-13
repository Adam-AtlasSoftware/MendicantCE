/*
PRISM_BAKE_ATLAS.C

The baker's lightmap atlas: CE's layout scaled per chart by importance
(prism_bake_atlas.h).
*/

#include "prism_bake_atlas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void prism_bake_atlas_defaults(struct prism_bake_atlas_parameters *parameters, enum prism_bake_quality quality)
{
	static float const tier0[NUMBER_OF_PRISM_BAKE_QUALITIES] = { 4.0f, 8.0f, 16.0f, 16.0f };

	memset(parameters, 0, sizeof(*parameters));
	if ((int)quality < 0 || quality >= NUMBER_OF_PRISM_BAKE_QUALITIES)
		quality = PRISM_BAKE_QUALITY_MAX;
	/* (tier 1 a quarter of tier 0's texels, half its density; tier 2 one
	texel per 2 wu: section 3.3) */
	parameters->density[0] = tier0[quality];
	parameters->density[1] = tier0[quality] * 0.5f;
	parameters->density[2] = 0.5f;
	parameters->tile = 32;
	parameters->gutter = 1.0f;
	parameters->maximum_scale = 64;
	parameters->maximum_refine = 3;
	parameters->maximum_dimension = 16384;
}

/* ---------- charts: triangles joined by a shared lightmap UV */

struct corner
{
	uint64_t key;
	int32_t triangle;
};

static int corner_compare(void const *a, void const *b)
{
	uint64_t ka = ((struct corner const *)a)->key, kb = ((struct corner const *)b)->key;

	return ka < kb ? -1 : ka > kb;
}

static int32_t find(int32_t *parent, int32_t x)
{
	while (parent[x] != x)
	{
		parent[x] = parent[parent[x]];
		x = parent[x];
	}
	return x;
}

static uint64_t corner_key(int32_t page, float u, float v)
{
	/* (UVs to 2^-20 of the page: a shared vertex's UVs are the same bits) */
	int64_t qu = (int64_t)floor((double)u * 1048576.0 + 0.5), qv = (int64_t)floor((double)v * 1048576.0 + 0.5);

	return ((uint64_t)(uint32_t)page << 48) ^ ((uint64_t)(qu & 0xFFFFFF) << 24) ^ (uint64_t)(qv & 0xFFFFFF);
}

static double triangle_area3(float const *p)
{
	double e1[3], e2[3], n[3];
	int i;

	for (i = 0; i < 3; i++)
	{
		e1[i] = (double)p[3 + i] - p[i];
		e2[i] = (double)p[6 + i] - p[i];
	}
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	return 0.5 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

int prism_bake_atlas_build(struct prism_bake_atlas *atlas, struct prism_bake_atlas_page const *pages,
	int32_t page_count, struct prism_bake_atlas_triangle const *triangles, size_t triangle_count,
	struct prism_bake_atlas_parameters const *parameters)
{
	struct corner *corners = NULL;
	int32_t *parent = NULL, *root_chart = NULL;
	size_t corner_count = 0, index;
	int32_t t;
	int result = -1;

	memset(atlas, 0, sizeof(*atlas));
	atlas->parameters = *parameters;
	atlas->page_count = page_count;
	atlas->triangle_count = triangle_count;
	atlas->pages = (struct prism_bake_atlas_page *)malloc((size_t)(page_count > 0 ? page_count : 1) *
		sizeof(*atlas->pages));
	atlas->triangle_charts = (int32_t *)malloc((triangle_count ? triangle_count : 1) * sizeof(int32_t));
	atlas->triangle_boxes = (float *)malloc((triangle_count ? triangle_count : 1) * 4 * sizeof(float));
	corners = (struct corner *)malloc((triangle_count ? triangle_count : 1) * 3 * sizeof(*corners));
	parent = (int32_t *)malloc((triangle_count ? triangle_count : 1) * sizeof(int32_t));
	root_chart = (int32_t *)malloc((triangle_count ? triangle_count : 1) * sizeof(int32_t));
	if (!atlas->pages || !atlas->triangle_charts || !atlas->triangle_boxes || !corners || !parent || !root_chart)
		goto done;
	if (page_count > 0)
		memcpy(atlas->pages, pages, (size_t)page_count * sizeof(*pages));

	/* (union the triangles that share a corner's UV on a page) */
	for (index = 0; index < triangle_count; index++)
	{
		struct prism_bake_atlas_triangle const *triangle = &triangles[index];
		int k;

		parent[index] = (int32_t)index;
		root_chart[index] = -1;
		atlas->triangle_charts[index] = -1;
		if (triangle->page < 0 || triangle->page >= page_count || pages[triangle->page].width <= 0 ||
			pages[triangle->page].height <= 0)
		{
			continue;
		}
		for (k = 0; k < 3; k++)
		{
			corners[corner_count].key = corner_key(triangle->page, triangle->uv[k * 2], triangle->uv[k * 2 + 1]);
			corners[corner_count].triangle = (int32_t)index;
			corner_count++;
		}
	}
	qsort(corners, corner_count, sizeof(*corners), corner_compare);
	for (index = 1; index < corner_count; index++)
	{
		if (corners[index].key == corners[index - 1].key)
		{
			int32_t a = find(parent, corners[index].triangle), b = find(parent, corners[index - 1].triangle);

			if (a != b)
				parent[a < b ? b : a] = a < b ? a : b;
		}
	}

	/* the charts */
	for (index = 0; index < triangle_count; index++)
	{
		struct prism_bake_atlas_triangle const *triangle = &triangles[index];
		struct prism_bake_chart *chart;
		struct prism_bake_atlas_page const *page;
		int32_t root;
		double texel_area, u[3], v[3];
		int k;

		if (triangle->page < 0 || triangle->page >= page_count || pages[triangle->page].width <= 0 ||
			pages[triangle->page].height <= 0)
		{
			atlas->totals.triangles_without_atlas++;
			atlas->totals.area_without_atlas += triangle_area3(triangle->position);
			continue;
		}
		root = find(parent, (int32_t)index);
		if (root_chart[root] < 0)
		{
			struct prism_bake_chart *charts = (struct prism_bake_chart *)realloc(atlas->charts,
				(size_t)(atlas->chart_count + 1) * sizeof(*charts));

			if (!charts)
				goto done;
			atlas->charts = charts;
			chart = &atlas->charts[atlas->chart_count];
			memset(chart, 0, sizeof(*chart));
			chart->page = triangle->page;
			chart->tier = 2;
			chart->texel_minimum[0] = chart->texel_minimum[1] = 1e30f;
			chart->texel_maximum[0] = chart->texel_maximum[1] = -1e30f;
			chart->virtual_page = -1;
			root_chart[root] = atlas->chart_count++;
		}
		chart = &atlas->charts[root_chart[root]];
		atlas->triangle_charts[index] = root_chart[root];
		page = &pages[triangle->page];
		for (k = 0; k < 3; k++)
		{
			u[k] = (double)triangle->uv[k * 2] * page->width;
			v[k] = (double)triangle->uv[k * 2 + 1] * page->height;
			if (u[k] < chart->texel_minimum[0])
				chart->texel_minimum[0] = (float)u[k];
			if (v[k] < chart->texel_minimum[1])
				chart->texel_minimum[1] = (float)v[k];
			if (u[k] > chart->texel_maximum[0])
				chart->texel_maximum[0] = (float)u[k];
			if (v[k] > chart->texel_maximum[1])
				chart->texel_maximum[1] = (float)v[k];
		}
		{
			float *box = atlas->triangle_boxes + index * 4;

			box[0] = (float)(u[0] < u[1] ? (u[0] < u[2] ? u[0] : u[2]) : (u[1] < u[2] ? u[1] : u[2]));
			box[1] = (float)(v[0] < v[1] ? (v[0] < v[2] ? v[0] : v[2]) : (v[1] < v[2] ? v[1] : v[2]));
			box[2] = (float)(u[0] > u[1] ? (u[0] > u[2] ? u[0] : u[2]) : (u[1] > u[2] ? u[1] : u[2]));
			box[3] = (float)(v[0] > v[1] ? (v[0] > v[2] ? v[0] : v[2]) : (v[1] > v[2] ? v[1] : v[2]));
		}
		texel_area = 0.5 * fabs((u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]));
		chart->texel_area += texel_area;
		chart->world_area += triangle_area3(triangle->position);
		chart->triangle_count++;
		if (triangle->tier < chart->tier)
			chart->tier = triangle->tier;
	}
	for (t = 0; t < atlas->chart_count; t++)
	{
		struct prism_bake_chart *chart = &atlas->charts[t];

		chart->density = chart->world_area > 1e-9 && chart->texel_area > 0.0 ?
			(float)sqrt(chart->texel_area / chart->world_area) : 0.0f;
	}
	result = prism_bake_atlas_layout(atlas);

done:
	free(corners);
	free(parent);
	free(root_chart);
	return result;
}

/* ---------- the layout */

static int32_t chart_base_scale(struct prism_bake_atlas_parameters const *p, struct prism_bake_chart const *chart)
{
	double want;
	int32_t scale;

	if (chart->density <= 0.0f)
		return 1;
	want = ceil((double)p->density[chart->tier > 2 ? 2 : chart->tier] / chart->density - 1e-6);
	scale = want < 1.0 ? 1 : want > (double)p->maximum_scale ? p->maximum_scale : (int32_t)want;
	return scale;
}

static void virtual_pages_free(struct prism_bake_atlas *atlas)
{
	int32_t index;

	for (index = 0; index < atlas->virtual_page_count; index++)
		free(atlas->virtual_pages[index].tiles);
	free(atlas->virtual_pages);
	atlas->virtual_pages = NULL;
	atlas->virtual_page_count = 0;
}

static int32_t virtual_page_find(struct prism_bake_atlas *atlas, int32_t page, int32_t scale)
{
	struct prism_bake_atlas_parameters const *p = &atlas->parameters;
	struct prism_bake_virtual_page *pages, *vp;
	int32_t index;

	for (index = 0; index < atlas->virtual_page_count; index++)
	{
		if (atlas->virtual_pages[index].page == page && atlas->virtual_pages[index].scale == scale)
			return index;
	}
	pages = (struct prism_bake_virtual_page *)realloc(atlas->virtual_pages,
		(size_t)(atlas->virtual_page_count + 1) * sizeof(*pages));
	if (!pages)
		return -1;
	atlas->virtual_pages = pages;
	vp = &pages[atlas->virtual_page_count];
	memset(vp, 0, sizeof(*vp));
	vp->page = page;
	vp->scale = scale;
	vp->width = atlas->pages[page].width * scale;
	vp->height = atlas->pages[page].height * scale;
	vp->tiles_x = (vp->width + p->tile - 1) / p->tile;
	vp->tiles_y = (vp->height + p->tile - 1) / p->tile;
	vp->tiles = (uint8_t *)calloc((size_t)vp->tiles_x * (size_t)vp->tiles_y, 1);
	if (!vp->tiles)
		return -1;
	return atlas->virtual_page_count++;
}

int prism_bake_atlas_layout(struct prism_bake_atlas *atlas)
{
	struct prism_bake_atlas_parameters const *p = &atlas->parameters;
	struct prism_bake_atlas_totals *totals = &atlas->totals;
	size_t without = totals->triangles_without_atlas;
	double without_area = totals->area_without_atlas;
	int32_t c, index;

	virtual_pages_free(atlas);
	memset(totals, 0, sizeof(*totals));
	totals->triangles_without_atlas = without;
	totals->area_without_atlas = without_area;
	for (index = 0; index < atlas->page_count; index++)
		totals->texels_ce += (double)atlas->pages[index].width * atlas->pages[index].height;
	for (c = 0; c < atlas->chart_count; c++)
	{
		struct prism_bake_chart *chart = &atlas->charts[c];
		struct prism_bake_atlas_page const *page = &atlas->pages[chart->page];
		struct prism_bake_virtual_page *vp;
		int32_t scale = chart_base_scale(p, chart) << chart->refine, side, vindex, x0, x1, y0, y1;
		float g = p->gutter;
		double box;
		int tier = chart->tier > 2 ? 2 : chart->tier;

		/* (the largest scale that fits the page's side) */
		side = page->width > page->height ? page->width : page->height;
		chart->clamped = 0;
		while (scale > 1 && (int64_t)side * scale > p->maximum_dimension)
		{
			scale--;
			chart->clamped = 1;
		}
		chart->scale = scale;
		vindex = virtual_page_find(atlas, chart->page, scale);
		if (vindex < 0)
			return -1;
		chart->virtual_page = vindex;
		vp = &atlas->virtual_pages[vindex];
		vp->chart_count++;
		/* (its box with the gutter, at the scale, in tiles) */
		x0 = (int32_t)floor(((double)chart->texel_minimum[0] - g) * scale);
		y0 = (int32_t)floor(((double)chart->texel_minimum[1] - g) * scale);
		x1 = (int32_t)ceil(((double)chart->texel_maximum[0] + g) * scale);
		y1 = (int32_t)ceil(((double)chart->texel_maximum[1] + g) * scale);
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > vp->width ? vp->width : x1;
		y1 = y1 > vp->height ? vp->height : y1;
		box = x1 > x0 && y1 > y0 ? (double)(x1 - x0) * (double)(y1 - y0) : 0.0;
		totals->texels_charts += box;
		totals->texels_covered += chart->texel_area * scale * scale;
		totals->texels_by_tier[tier] += chart->texel_area * scale * scale;
		totals->world_area_by_tier[tier] += chart->world_area;
		totals->charts_by_tier[tier]++;
		totals->charts_clamped += chart->clamped;
	}
	/* the tiles: each triangle's box with the gutter, at its chart's scale */
	for (index = 0; index < (int32_t)atlas->triangle_count; index++)
	{
		int32_t chart_index = atlas->triangle_charts[index], scale, x0, x1, y0, y1, x, y;
		struct prism_bake_virtual_page *vp;
		float const *box = atlas->triangle_boxes + (size_t)index * 4;
		float g = p->gutter;

		if (chart_index < 0)
			continue;
		scale = atlas->charts[chart_index].scale;
		vp = &atlas->virtual_pages[atlas->charts[chart_index].virtual_page];
		x0 = (int32_t)floor(((double)box[0] - g) * scale);
		y0 = (int32_t)floor(((double)box[1] - g) * scale);
		x1 = (int32_t)ceil(((double)box[2] + g) * scale);
		y1 = (int32_t)ceil(((double)box[3] + g) * scale);
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > vp->width ? vp->width : x1;
		y1 = y1 > vp->height ? vp->height : y1;
		for (y = y0 / p->tile; y1 > y0 && y <= (y1 - 1) / p->tile; y++)
		{
			for (x = x0 / p->tile; x1 > x0 && x <= (x1 - 1) / p->tile; x++)
			{
				uint8_t *tile = &vp->tiles[(size_t)y * vp->tiles_x + x];

				if (!*tile)
				{
					*tile = 1;
					vp->tile_count++;
				}
			}
		}
	}
	totals->charts = (size_t)atlas->chart_count;
	totals->virtual_pages = (size_t)atlas->virtual_page_count;
	for (index = 0; index < atlas->virtual_page_count; index++)
	{
		struct prism_bake_virtual_page const *vp = &atlas->virtual_pages[index];

		totals->tiles += vp->tile_count;
		totals->texels_dense += (double)vp->width * vp->height;
	}
	totals->texels_allocated = (double)totals->tiles * p->tile * p->tile;
	return 0;
}

/* ---------- refinement */

int prism_bake_atlas_refine(struct prism_bake_atlas *atlas, int32_t chart, int32_t levels)
{
	struct prism_bake_chart *c;
	int32_t level;

	if (chart < 0 || chart >= atlas->chart_count || levels <= 0)
		return 0;
	c = &atlas->charts[chart];
	level = c->refine + levels;
	if (level > atlas->parameters.maximum_refine)
		level = atlas->parameters.maximum_refine;
	if (level == c->refine)
		return 0;
	c->refine = (uint8_t)level;
	return 1;
}

int32_t prism_bake_atlas_refine_by_error(struct prism_bake_atlas *atlas, float const *errors, float threshold)
{
	int32_t c, count = 0;

	for (c = 0; c < atlas->chart_count; c++)
	{
		if (errors[c] > threshold)
			count += prism_bake_atlas_refine(atlas, c, 1);
	}
	return count;
}

void prism_bake_atlas_texel(struct prism_bake_atlas const *atlas, int32_t chart, float u, float v,
	int32_t *virtual_page, float *x, float *y)
{
	struct prism_bake_chart const *c;
	struct prism_bake_virtual_page const *vp;

	if (chart < 0 || chart >= atlas->chart_count || atlas->charts[chart].virtual_page < 0)
	{
		*virtual_page = -1;
		*x = *y = 0.0f;
		return;
	}
	c = &atlas->charts[chart];
	vp = &atlas->virtual_pages[c->virtual_page];
	*virtual_page = c->virtual_page;
	*x = u * (float)vp->width;
	*y = v * (float)vp->height;
}

void prism_bake_atlas_free(struct prism_bake_atlas *atlas)
{
	virtual_pages_free(atlas);
	free(atlas->pages);
	free(atlas->charts);
	free(atlas->triangle_charts);
	free(atlas->triangle_boxes);
	memset(atlas, 0, sizeof(*atlas));
}
