/*
PRISM_BAKE_FILTER.C

The baked texels' robust mean and their filter in chart space
(prism_bake_filter.h).
*/

#include "prism_bake_filter.h"

#include <math.h>
#include <string.h>

void prism_bake_robust_defaults(struct prism_bake_robust_parameters *parameters)
{
	parameters->spread = 3.0f;
	parameters->largest_share = 0.25f;
	parameters->minimum_groups = 4;
}

uint32_t prism_bake_group_samples(uint32_t count, uint32_t group)
{
	uint32_t whole = count / PRISM_BAKE_GROUP_BATCH, left = count % PRISM_BAKE_GROUP_BATCH, samples;

	if (group >= PRISM_BAKE_GROUPS)
		return 0;
	/* (batches group, group + 8, ...: whole ones, then the partial batch
	where it falls to this group) */
	samples = (whole / PRISM_BAKE_GROUPS + (group < whole % PRISM_BAKE_GROUPS ? 1u : 0u)) * PRISM_BAKE_GROUP_BATCH;
	if (left && whole % PRISM_BAKE_GROUPS == group)
		samples += left;
	return samples;
}

static void sort(float *v, int n)
{
	int i, j;

	for (i = 1; i < n; i++)
	{
		float x = v[i];

		for (j = i - 1; j >= 0 && v[j] > x; j--)
			v[j + 1] = v[j];
		v[j + 1] = x;
	}
}

static float median_of(float *v, int n)
{
	sort(v, n);
	return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

float prism_bake_robust_factor(float const sums[PRISM_BAKE_GROUPS], uint32_t count, float largest,
	float *variance, struct prism_bake_robust_parameters const *parameters)
{
	float means[PRISM_BAKE_GROUPS], deviations[PRISM_BAKE_GROUPS], median, sigma, excess;
	double total = 0.0;
	int g, n = 0;

	if (!count)
		return 1.0f;
	for (g = 0; g < PRISM_BAKE_GROUPS; g++)
	{
		uint32_t samples = prism_bake_group_samples(count, (uint32_t)g);

		total += sums[g];
		if (samples >= PRISM_BAKE_GROUP_BATCH / 2)
			means[n++] = sums[g] / (float)samples;
	}
	if (n < (int)parameters->minimum_groups || total <= 0.0)
		return 1.0f;
	median = median_of(means, n);
	excess = (float)(total / count) - median;
	if (excess <= 0.0f)
		return 1.0f;
	/* (the groups' spread, robustly: their median absolute deviation as a
	normal's; where half of them agree exactly, the spread of all but the
	largest) */
	for (g = 0; g < n; g++)
		deviations[g] = (float)fabs(means[g] - median);
	sigma = 1.4826f * median_of(deviations, n);
	if (sigma <= 0.0f)
	{
		double mean = 0.0, m2 = 0.0;

		for (g = 0; g < n - 1; g++)
			mean += means[g];
		mean /= n - 1;
		for (g = 0; g < n - 1; g++)
			m2 += (means[g] - mean) * (means[g] - mean);
		sigma = n > 2 ? (float)sqrt(m2 / (n - 2)) : 0.0f;
	}
	/* (the mean is over the median by more than the noise explains, and its
	largest sample alone explains a good part of that: a firefly) */
	if (excess <= parameters->spread * sigma / (float)sqrt((double)n))
		return 1.0f;
	if (largest / (float)count < parameters->largest_share * excess)
		return 1.0f;
	if (variance && sigma > 0.0f)
		*variance = 1.5707963f * sigma * sigma / (float)n;
	return median / (float)(total / count);
}

void prism_bake_filter_defaults(struct prism_bake_filter_parameters *parameters, int quality)
{
	parameters->passes = quality >= 3 ? 1 : 3;
	parameters->luminance_sigma = 3.0f;
	parameters->normal_cos = 0.9f;
	parameters->variance_floor = 0.002f;
}

static float luma(float const c[3])
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* one a-trous pass at step over the square [low, high) of the window */
static void filter_pass(struct prism_bake_filter_texel const *from, struct prism_bake_filter_texel *to, int step,
	int low, int high, struct prism_bake_filter_parameters const *p)
{
	static float const kernel[3] = { 0.25f, 0.5f, 0.25f };
	int x, y, dx, dy, k;
	float two_sigma2 = 2.0f * p->luminance_sigma * p->luminance_sigma;

	for (y = low; y < high; y++)
		for (x = low; x < high; x++)
		{
			struct prism_bake_filter_texel const *c = &from[y * PRISM_BAKE_FILTER_WINDOW + x];
			struct prism_bake_filter_texel *o = &to[y * PRISM_BAKE_FILTER_WINDOW + x];
			float lc, sum = 0.0f, variance = 0.0f, acc[9];

			*o = *c;
			if (c->chart < 0)
				continue;
			lc = luma(c->light);
			memset(acc, 0, sizeof(acc));
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++)
				{
					struct prism_bake_filter_texel const *q =
						&from[(y + dy * step) * PRISM_BAKE_FILTER_WINDOW + x + dx * step];
					float w = kernel[dx + 1] * kernel[dy + 1];

					if (q->chart != c->chart)
						continue;
					if (dx || dy)
					{
						float cosine = c->normal[0] * q->normal[0] + c->normal[1] * q->normal[1] +
							c->normal[2] * q->normal[2], wn, lq, d, v, f;

						if (cosine < p->normal_cos)
							continue;
						wn = (cosine - p->normal_cos) / (1.0f - p->normal_cos);
						wn = wn > 1.0f ? 1.0f : wn * wn;
						lq = luma(q->light);
						d = lc - lq;
						f = p->variance_floor * (lc > lq ? lc : lq);
						v = c->variance + q->variance + f * f + 1.0e-20f;
						w *= wn * (float)exp(-(double)(d * d / (two_sigma2 * v)));
					}
					for (k = 0; k < 3; k++)
					{
						acc[k] += w * q->light[k];
						acc[3 + k] += w * q->indirect[k];
						acc[6 + k] += w * q->moment[k];
					}
					variance += w * w * q->variance;
					sum += w;
				}
			for (k = 0; k < 3; k++)
			{
				o->light[k] = acc[k] / sum;
				o->indirect[k] = acc[3 + k] / sum;
				o->moment[k] = acc[6 + k] / sum;
			}
			o->variance = variance / (sum * sum);
		}
}

void prism_bake_filter_tile(struct prism_bake_filter_texel const *window, struct prism_bake_filter_texel *out,
	struct prism_bake_filter_texel *scratch, struct prism_bake_filter_parameters const *parameters)
{
	struct prism_bake_filter_texel const *from = window;
	int pass, passes = parameters->passes, margin, y;

	if (passes > PRISM_BAKE_FILTER_PASSES_MOST)
		passes = PRISM_BAKE_FILTER_PASSES_MOST;
	/* (each pass over the tile and the margin the passes after it read:
	their steps' sum) */
	margin = (1 << passes) - 1;
	for (pass = 0; pass < passes; pass++)
	{
		struct prism_bake_filter_texel *to = scratch + (pass & 1) * PRISM_BAKE_FILTER_WINDOW * PRISM_BAKE_FILTER_WINDOW;
		int step = 1 << pass;

		margin -= step;
		filter_pass(from, to, step, PRISM_BAKE_FILTER_TILE - margin, 2 * PRISM_BAKE_FILTER_TILE + margin, parameters);
		from = to;
	}
	for (y = 0; y < PRISM_BAKE_FILTER_TILE; y++)
		memcpy(out + y * PRISM_BAKE_FILTER_TILE, from + (y + PRISM_BAKE_FILTER_TILE) * PRISM_BAKE_FILTER_WINDOW +
			PRISM_BAKE_FILTER_TILE, PRISM_BAKE_FILTER_TILE * sizeof(*out));
}

/* ---------- normals */

static float sign_of(float v)
{
	return v < 0.0f ? -1.0f : 1.0f;
}

static uint32_t snorm16(float v)
{
	v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
	return (uint32_t)(int32_t)floor(v * 32767.0f + 0.5f) & 0xffffu;
}

static float unsnorm16(uint32_t v)
{
	int32_t s = (int32_t)(v & 0xffffu);

	if (s >= 32768)
		s -= 65536;
	return (float)s / 32767.0f;
}

uint32_t prism_bake_normal_pack(float const n[3])
{
	float l = (float)(fabs(n[0]) + fabs(n[1]) + fabs(n[2])), x, y;

	if (l <= 0.0f)
		return snorm16(0.0f) | (snorm16(0.0f) << 16);
	x = n[0] / l;
	y = n[1] / l;
	if (n[2] < 0.0f)
	{
		float ox = x;

		x = (1.0f - (float)fabs(y)) * sign_of(ox);
		y = (1.0f - (float)fabs(ox)) * sign_of(y);
	}
	return snorm16(x) | (snorm16(y) << 16);
}

void prism_bake_normal_unpack(uint32_t packed, float n[3])
{
	float x = unsnorm16(packed), y = unsnorm16(packed >> 16), z = 1.0f - (float)fabs(x) - (float)fabs(y), l;

	if (z < 0.0f)
	{
		float ox = x;

		x = (1.0f - (float)fabs(y)) * sign_of(ox);
		y = (1.0f - (float)fabs(ox)) * sign_of(y);
	}
	l = (float)sqrt(x * x + y * y + z * z);
	n[0] = x / l;
	n[1] = y / l;
	n[2] = z / l;
}
