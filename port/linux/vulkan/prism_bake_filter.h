/*
PRISM_BAKE_FILTER.H

The baked texels' noise after their samples (docs/prism_bake.md, "The
noise"): two steps between the estimator and the bake file.

- The robust mean. A texel's samples come in batches of 64 (their first
  directions stratified); batch b belongs to group b mod 8, and the bake
  keeps each group's sum of the samples' luminance. Where the groups
  disagree badly (the mean over the groups' median by more than three of
  the median's standard errors, measured from the groups' spread, and the
  texel's largest sample alone a good part of that excess: a firefly) the
  texel's level is the groups' median (a median of means), and its noise
  the median's. Each group's mean tends to the texel's light as its
  samples grow, so the median does too: unbiased in the limit. The level
  steers the filter (below): the texel's light is not scaled, since a
  rare path's light is light the texel's neighbours should have had too
  (scaling the texels to their medians took half the light of b30's
  interior walls lit by such paths); the filter spreads it over them.
- The filter in chart space. The tiers under `max` stop on a sample
  budget, not on their noise, and their texels keep a grain between two
  seeds; `max` runs to its noise floor and takes at most the mildest pass.
  Each pass of an a-trous filter (3 x 3 taps of 1/4, 1/2, 1/4, their
  spacing doubled each pass: 1, 2, 4) averages a texel with those of its
  neighbours on the same chart whose faces turn little from its own and
  whose level differs from its own by no more than their noise explains:
  the weight of a neighbour falls with the difference of the two levels
  over the two texels' measured standard errors (the 1% rule's statistics), so an
  edge that the samples resolve (a shadow's, a lamp's pool) stays, and a
  difference the noise explains is averaged away. The variance of each
  result is carried to the next pass. Three passes reach 7 texels, less
  than a tile, so a tile is filtered from its own texels and its eight
  neighbours' (a window of 3 x 3 tiles), with no seam between tiles.

Host code with no engine dependency, written as C89.
*/

#ifndef PRISM_BAKE_FILTER_H
#define PRISM_BAKE_FILTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PRISM_BAKE_GROUPS = 8, /* a texel's groups of batches */
	PRISM_BAKE_GROUP_BATCH = 64, /* the samples a batch */
	PRISM_BAKE_FILTER_TILE = 16, /* the tiles' side, in texels */
	PRISM_BAKE_FILTER_WINDOW = 48, /* a tile and its neighbours */
	PRISM_BAKE_FILTER_PASSES_MOST = 4
};

/* the robust mean's rule */
struct prism_bake_robust_parameters
{
	float spread; /* the median's standard errors the mean may be over it (3) */
	float largest_share; /* the share of the excess the largest sample must explain (0.25) */
	uint32_t minimum_groups; /* groups of at least half a batch each (4) */
};

void prism_bake_robust_defaults(struct prism_bake_robust_parameters *parameters);

/* the samples of a texel's group g (whole batches of 64 by the group's turn,
the last one partial) when it has count samples */
uint32_t prism_bake_group_samples(uint32_t count, uint32_t group);

/* a texel's robust mean: from its groups' sums of luminance, its samples and
its largest sample, the factor its light is multiplied by (1 where its
groups agree); where it is not 1 the variance of its luminance's mean is
replaced by the median's (else left as it is) */
float prism_bake_robust_factor(float const sums[PRISM_BAKE_GROUPS], uint32_t count, float largest,
	float *variance, struct prism_bake_robust_parameters const *parameters);

/* a texel in the filter: its light less the sun's, the indirect light
alone, the light's moment (the direction's share k times the light's
luminance), its level (the luminance the weights compare: the robust
mean's), the variance of its level, its face's normal and its chart (-1:
not baked, a fill or none: neither filtered nor read) */
struct prism_bake_filter_texel
{
	float light[3], indirect[3], moment[3];
	float level, variance;
	float normal[3];
	int32_t chart;
};

struct prism_bake_filter_parameters
{
	int passes; /* 0 none, at most PRISM_BAKE_FILTER_PASSES_MOST */
	float luminance_sigma; /* a light's difference over the texels' standard error (3) */
	float normal_cos; /* the cosine below which a neighbour is never read (0.9) */
	float variance_floor; /* relative: the variance is at least (floor x light)^2 (0.002) */
};

/* the defaults of a quality (prism_bake_atlas.h's prism_bake_quality: 3
passes under `max`, one at `max`) */
void prism_bake_filter_defaults(struct prism_bake_filter_parameters *parameters, int quality);

/* a tile filtered: window holds its 3 x 3 tiles' texels (48 x 48, row by
row; the tile the middle one, a missing neighbour's texels with chart -1),
out its 16 x 16 texels. scratch: 2 x 48 x 48 texels. A texel of chart -1
is copied as it is */
void prism_bake_filter_tile(struct prism_bake_filter_texel const *window, struct prism_bake_filter_texel *out,
	struct prism_bake_filter_texel *scratch, struct prism_bake_filter_parameters const *parameters);

/* a unit normal to 32 bits and back (octahedral, 16 bits each) */
uint32_t prism_bake_normal_pack(float const n[3]);
void prism_bake_normal_unpack(uint32_t packed, float n[3]);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_FILTER_H */
