/*
PRISM_BAKE_CONVERGE.H

When a baked texel has enough samples (PRISM v4's design, 10.1: "samples
per texel are adaptive until converged against a noise threshold set in
M0, not a fixed count; no texel stops on a sample budget").

Each texel keeps running statistics of one scalar per sample: the
estimate whose noise matters (by default the luminance of the sample's
L0 radiance; a caller that wants the directional terms converged too
feeds max(L0, k |L1|)). The bake adds samples in batches; between batches
a texel stops when all of these hold:

- it has at least minimum_samples;
- its relative error is below the threshold: confidence x the standard
  error of the mean <= threshold x max(mean, absolute_floor). The standard
  error is the samples' (sqrt(variance / n)), or with use_batch_means the
  batch means' (for correlated samples within a batch: reused paths,
  shared light samples), whichever is larger once there are enough
  batches;
- no single sample moves the mean by more than the threshold: maximum /
  (n x max(mean, absolute_floor)) <= threshold, so a texel that has just
  met its first firefly is not stopped by a variance estimate that has
  not yet seen the tail;
- it has seen at least minimum_hits nonzero samples, or none at all in
  zero_samples (by the rule of three, below 3 / zero_samples of its paths
  carry light at 95% confidence: black, converged).

With maximum_samples 0 (the `max` tier) there is no sample budget. The
threshold is a parameter: M0 fixes it from the gates against the
reference.

Bounces stop the same way (10.1, "more bounces until the added energy is
below the threshold"): prism_bake_bounces_converged.

The statistics merge (Chan et al.'s parallel form of Welford's), so a GPU
batch computes its own and merges them into the texel's, in any order.
Host code with no engine dependency, written as C89; the texel's state is
plain data (20 bytes) for a GPU buffer.
*/

#ifndef PRISM_BAKE_CONVERGE_H
#define PRISM_BAKE_CONVERGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct prism_bake_texel_stats
{
	uint32_t count; /* samples */
	uint32_t hits; /* nonzero samples */
	float mean;
	float m2; /* the sum of squared differences from the mean */
	float maximum; /* the largest sample */
};

/* the batch means' statistics (use_batch_means) */
struct prism_bake_batch_stats
{
	uint32_t batches;
	float mean, m2;
};

struct prism_bake_converge_parameters
{
	float threshold; /* the relative error at which a texel stops (M0 fixes it) */
	float confidence; /* z: 1.96 for 95% */
	float absolute_floor; /* radiance below which a texel is black: the error's floor */
	uint32_t minimum_samples;
	uint32_t minimum_hits;
	uint32_t zero_samples;
	uint32_t batch; /* samples per batch (the stop test runs between batches) */
	uint32_t maximum_samples; /* 0: none */
	int use_batch_means;
	uint32_t minimum_batches; /* before the batch means' error is trusted */
};

enum prism_bake_converge_state
{
	PRISM_BAKE_CONVERGE_RUNNING = 0,
	PRISM_BAKE_CONVERGE_CONVERGED, /* the relative error is below the threshold */
	PRISM_BAKE_CONVERGE_BLACK, /* no light in zero_samples */
	PRISM_BAKE_CONVERGE_BUDGET /* maximum_samples reached (never with 0) */
};

/* the defaults, with the threshold given (M0's) */
void prism_bake_converge_defaults(struct prism_bake_converge_parameters *parameters, float threshold);

void prism_bake_texel_reset(struct prism_bake_texel_stats *stats);
void prism_bake_texel_add(struct prism_bake_texel_stats *stats, float sample);
/* another set's statistics merged in (a batch's) */
void prism_bake_texel_merge(struct prism_bake_texel_stats *stats, struct prism_bake_texel_stats const *other);
/* a batch's mean added to the batch means */
void prism_bake_batch_add(struct prism_bake_batch_stats *batches, float batch_mean);

/* the variance of the samples (0 with fewer than 2) */
float prism_bake_texel_variance(struct prism_bake_texel_stats const *stats);
/* the relative error (confidence x standard error over max(mean, floor)),
with the batch means' when given and trusted */
float prism_bake_texel_relative_error(struct prism_bake_texel_stats const *stats,
	struct prism_bake_batch_stats const *batches, struct prism_bake_converge_parameters const *parameters);

/* the stopping rule (batches may be NULL) */
enum prism_bake_converge_state prism_bake_texel_state(struct prism_bake_texel_stats const *stats,
	struct prism_bake_batch_stats const *batches, struct prism_bake_converge_parameters const *parameters);

/* how many of count texels have stopped */
size_t prism_bake_converge_count(struct prism_bake_texel_stats const *stats, struct prism_bake_batch_stats const *batches,
	size_t count, struct prism_bake_converge_parameters const *parameters);

/* whether the bounces stop: the last bounce added at most threshold of the
light so far */
int prism_bake_bounces_converged(double added, double total, float threshold);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_CONVERGE_H */
