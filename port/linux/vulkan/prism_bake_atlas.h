/*
PRISM_BAKE_ATLAS.H

The baker's lightmap atlas (PRISM v4's design, sections 3.5 and 3.6, and
10.1's `max` tier): CE's own lightmap layout, scaled up, never repacked.

- A structure bsp's lightmap pages are already a valid layout of charts
  with gutters between them. Scaling a page by an integer N keeps every
  chart where it was and scales its gutters by N, so the baked texels
  nest in CE's (each CE texel is N x N of ours).
- A chart is a set of triangles joined by their lightmap UVs on one page.
  Each chart's density is measured (CE texels per world unit, from its UV
  and world areas) and its scale chosen from its importance tier
  (prism_bake_reach.h): the tier's density over the chart's, rounded up
  to an integer, at least 1 (CE's own density: never scaled down, so a
  gutter never shrinks below CE's).
- The charts of a page that share a scale form a virtual page: the page
  at that scale. Only the tiles a chart's triangles (each with the
  gutter around it) touch are allocated, so a page's tier 2 charts at scale 1 and its tier 0 charts
  at scale 8 cost only what they cover. Sampling reads a virtual page
  through its tiles' table (or Vulkan's sparse residency).
- Refinement: a chart's level doubles its scale per level. The bake asks
  for it where a converged comparison against the reference still shows
  error (10.1: "refined further where a converged comparison against R
  still shows error"), and the layout is made again; the other charts
  keep their texels, so finished tiles stay valid.

Host code with no engine dependency, written as C89.
*/

#ifndef PRISM_BAKE_ATLAS_H
#define PRISM_BAKE_ATLAS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum prism_bake_quality
{
	PRISM_BAKE_QUALITY_DRAFT = 0,
	PRISM_BAKE_QUALITY_HIGH,
	PRISM_BAKE_QUALITY_ULTRA,
	PRISM_BAKE_QUALITY_MAX,
	NUMBER_OF_PRISM_BAKE_QUALITIES
};

struct prism_bake_atlas_page
{
	int32_t width, height; /* CE's page, in texels */
};

struct prism_bake_atlas_triangle
{
	int32_t page; /* the lightmap page, or -1 (no lightmap UVs: the volume lights it) */
	float uv[6]; /* the lightmap UVs, 0 to 1 across the page */
	float position[9];
	uint8_t tier; /* prism_bake_reach.h's PRISM_BAKE_TIER_* */
};

struct prism_bake_atlas_parameters
{
	float density[3]; /* texels per world unit at each tier */
	int32_t tile; /* the sparse tiles' side, in texels */
	float gutter; /* CE texels kept around each chart */
	int32_t maximum_scale; /* a chart's scale before refinement */
	int32_t maximum_refine; /* refinement levels */
	int32_t maximum_dimension; /* a virtual page's side */
};

struct prism_bake_chart
{
	int32_t page;
	int32_t triangle_count;
	float texel_minimum[2], texel_maximum[2]; /* its box, in CE texels */
	double world_area, texel_area; /* (CE texels squared) */
	float density; /* CE texels per world unit */
	uint8_t tier;
	uint8_t refine;
	int32_t scale; /* after refinement */
	int32_t virtual_page;
	int32_t clamped; /* the scale wanted did not fit maximum_dimension */
};

struct prism_bake_virtual_page
{
	int32_t page, scale;
	int32_t width, height; /* in texels */
	int32_t tiles_x, tiles_y;
	uint8_t *tiles; /* tiles_x * tiles_y: 1 where allocated */
	size_t tile_count;
	int32_t chart_count;
};

struct prism_bake_atlas_totals
{
	size_t charts, virtual_pages, tiles;
	double texels_allocated; /* tiles x tile squared */
	double texels_charts; /* the charts' boxes with their gutters */
	double texels_covered; /* inside the triangles */
	double texels_dense; /* the virtual pages whole */
	double texels_ce; /* CE's own pages */
	size_t triangles_without_atlas;
	double area_without_atlas;
	size_t charts_by_tier[3];
	double texels_by_tier[3]; /* covered */
	double world_area_by_tier[3];
	size_t charts_clamped;
};

struct prism_bake_atlas
{
	struct prism_bake_atlas_parameters parameters;
	struct prism_bake_atlas_page *pages;
	int32_t page_count;
	struct prism_bake_chart *charts;
	int32_t chart_count;
	int32_t *triangle_charts; /* each triangle's chart, or -1 */
	float *triangle_boxes; /* each triangle's box in CE texels: x0 y0 x1 y1 */
	size_t triangle_count;
	struct prism_bake_virtual_page *virtual_pages;
	int32_t virtual_page_count;
	struct prism_bake_atlas_totals totals;
};

/* the quality's densities (section 3.6; max: 10.1's 16 texels per wu at
tier 0, the start of refinement) */
void prism_bake_atlas_defaults(struct prism_bake_atlas_parameters *parameters, enum prism_bake_quality quality);

/* the charts of the triangles and their layout: 0, or -1 out of memory */
int prism_bake_atlas_build(struct prism_bake_atlas *atlas, struct prism_bake_atlas_page const *pages,
	int32_t page_count, struct prism_bake_atlas_triangle const *triangles, size_t triangle_count,
	struct prism_bake_atlas_parameters const *parameters);

/* the layout again (after refinement): 0, or -1 out of memory */
int prism_bake_atlas_layout(struct prism_bake_atlas *atlas);

/* a chart refined by levels more (clamped to maximum_refine): whether its
level changed; the layout is made again by prism_bake_atlas_layout */
int prism_bake_atlas_refine(struct prism_bake_atlas *atlas, int32_t chart, int32_t levels);

/* every chart whose error (one per chart, from the comparison against the
reference) is above threshold refined a level: how many were */
int32_t prism_bake_atlas_refine_by_error(struct prism_bake_atlas *atlas, float const *errors, float threshold);

/* where a CE lightmap UV of a chart is baked: its virtual page and texel
coordinates (continuous; texel centres at .5) */
void prism_bake_atlas_texel(struct prism_bake_atlas const *atlas, int32_t chart, float u, float v,
	int32_t *virtual_page, float *x, float *y);

void prism_bake_atlas_free(struct prism_bake_atlas *atlas);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_ATLAS_H */
