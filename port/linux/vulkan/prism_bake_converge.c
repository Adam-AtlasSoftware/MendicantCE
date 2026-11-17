/*
PRISM_BAKE_CONVERGE.C

The baked texels' stopping rule (prism_bake_converge.h).
*/

#include "prism_bake_converge.h"

#include <math.h>
#include <string.h>

void prism_bake_converge_defaults(struct prism_bake_converge_parameters *parameters, float threshold)
{
	memset(parameters, 0, sizeof(*parameters));
	parameters->threshold = threshold;
	parameters->confidence = 1.96f;
	parameters->absolute_floor = 1e-4f;
	parameters->minimum_samples = 64;
	parameters->minimum_hits = 16;
	parameters->zero_samples = 4096;
	parameters->batch = 32;
	parameters->maximum_samples = 0;
	parameters->use_batch_means = 0;
	parameters->minimum_batches = 8;
}

void prism_bake_texel_reset(struct prism_bake_texel_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

void prism_bake_texel_add(struct prism_bake_texel_stats *stats, float sample)
{
	double delta, mean;

	stats->count++;
	stats->hits += sample != 0.0f;
	delta = (double)sample - stats->mean;
	mean = stats->mean + delta / stats->count;
	stats->m2 = (float)(stats->m2 + delta * ((double)sample - mean));
	stats->mean = (float)mean;
	if (sample > stats->maximum)
		stats->maximum = sample;
}

void prism_bake_texel_merge(struct prism_bake_texel_stats *stats, struct prism_bake_texel_stats const *other)
{
	double n = (double)stats->count + other->count, delta;

	if (!other->count)
		return;
	if (!stats->count)
	{
		*stats = *other;
		return;
	}
	delta = (double)other->mean - stats->mean;
	stats->m2 = (float)((double)stats->m2 + other->m2 + delta * delta * ((double)stats->count * other->count / n));
	stats->mean = (float)(stats->mean + delta * (other->count / n));
	stats->count += other->count;
	stats->hits += other->hits;
	if (other->maximum > stats->maximum)
		stats->maximum = other->maximum;
}

void prism_bake_batch_add(struct prism_bake_batch_stats *batches, float batch_mean)
{
	double delta, mean;

	batches->batches++;
	delta = (double)batch_mean - batches->mean;
	mean = batches->mean + delta / batches->batches;
	batches->m2 = (float)(batches->m2 + delta * ((double)batch_mean - mean));
	batches->mean = (float)mean;
}

float prism_bake_texel_variance(struct prism_bake_texel_stats const *stats)
{
	return stats->count > 1 ? (stats->m2 > 0.0f ? stats->m2 : 0.0f) / (float)(stats->count - 1) : 0.0f;
}

static float scale_of(struct prism_bake_texel_stats const *stats, struct prism_bake_converge_parameters const *p)
{
	float mean = (float)fabs(stats->mean), floor_ = p->absolute_floor > p->error_floor ? p->absolute_floor :
		p->error_floor;

	return mean > floor_ ? mean : floor_;
}

float prism_bake_texel_relative_error(struct prism_bake_texel_stats const *stats,
	struct prism_bake_batch_stats const *batches, struct prism_bake_converge_parameters const *parameters)
{
	double standard_error;

	if (stats->count < 2)
		return (float)HUGE_VAL;
	standard_error = sqrt((double)prism_bake_texel_variance(stats) / stats->count);
	if (parameters->use_batch_means && batches && batches->batches >= parameters->minimum_batches &&
		batches->batches > 1)
	{
		double batch_error = sqrt((double)(batches->m2 > 0.0f ? batches->m2 : 0.0f) / (batches->batches - 1) /
			batches->batches);

		if (batch_error > standard_error)
			standard_error = batch_error;
	}
	return (float)(parameters->confidence * standard_error / scale_of(stats, parameters));
}

enum prism_bake_converge_state prism_bake_texel_state(struct prism_bake_texel_stats const *stats,
	struct prism_bake_batch_stats const *batches, struct prism_bake_converge_parameters const *parameters)
{
	float scale;

	if (parameters->maximum_samples && stats->count >= parameters->maximum_samples)
		return PRISM_BAKE_CONVERGE_BUDGET;
	if (stats->count < parameters->minimum_samples)
		return PRISM_BAKE_CONVERGE_RUNNING;
	if (!stats->hits)
		return stats->count >= parameters->zero_samples ? PRISM_BAKE_CONVERGE_BLACK : PRISM_BAKE_CONVERGE_RUNNING;
	if (stats->hits < parameters->minimum_hits)
		return PRISM_BAKE_CONVERGE_RUNNING;
	if (parameters->use_batch_means && (!batches || batches->batches < parameters->minimum_batches))
		return PRISM_BAKE_CONVERGE_RUNNING;
	scale = scale_of(stats, parameters);
	/* (no one sample moves the mean by more than the threshold) */
	if (stats->maximum > parameters->threshold * scale * (float)stats->count)
		return PRISM_BAKE_CONVERGE_RUNNING;
	if (prism_bake_texel_relative_error(stats, batches, parameters) > parameters->threshold)
		return PRISM_BAKE_CONVERGE_RUNNING;
	return PRISM_BAKE_CONVERGE_CONVERGED;
}

size_t prism_bake_converge_count(struct prism_bake_texel_stats const *stats, struct prism_bake_batch_stats const *batches,
	size_t count, struct prism_bake_converge_parameters const *parameters)
{
	size_t index, stopped = 0;

	for (index = 0; index < count; index++)
		stopped += prism_bake_texel_state(&stats[index], batches ? &batches[index] : NULL, parameters) !=
			PRISM_BAKE_CONVERGE_RUNNING;
	return stopped;
}

int prism_bake_bounces_converged(double added, double total, float threshold)
{
	if (total <= 0.0)
		return added <= 0.0;
	return added <= (double)threshold * total;
}
