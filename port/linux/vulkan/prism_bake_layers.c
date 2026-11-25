/*
PRISM_BAKE_LAYERS.C

The layered bake: prism_bake_layers.h.
*/

#include "prism_bake_layers.h"

#include <math.h>
#include <string.h>

/* the SH L0 of a flat floor of light f seen from every direction: f x 4 pi x
Y00 (the volume pass's l x 4 pi x Y, its mean over the sphere) */
#define FLOOR_L0 (12.566371f * 0.282095f)

static uint32_t float_word(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	return bits;
}

static float word_float(uint32_t bits)
{
	float value;

	memcpy(&value, &bits, 4);
	return value;
}

void prism_bake_layer_base_write(struct prism_bake_layer_light const *base, uint32_t words[PRISM_BAKE_LAYER_BASE_WORDS])
{
	int k;

	memset(words, 0, PRISM_BAKE_LAYER_BASE_WORDS * 4);
	words[0] = PRISM_BAKE_LAYER_BASE_VERSION;
	words[1] = base->stock ? 1u : 0u;
	for (k = 0; k < 3; k++)
	{
		words[2 + k] = float_word(base->sun[k]);
		words[5 + k] = float_word(base->sky[k]);
		words[8 + k] = float_word(base->domes[k]);
	}
	words[11] = float_word(base->fixtures);
	words[12] = float_word(base->emission);
	words[13] = float_word(base->emissive_scale);
}

int prism_bake_layer_base_read(uint32_t const *words, size_t count, struct prism_bake_layer_light *base)
{
	int k;

	memset(base, 0, sizeof(*base));
	if (!words || count < PRISM_BAKE_LAYER_BASE_WORDS || words[0] != PRISM_BAKE_LAYER_BASE_VERSION)
		return -1;
	base->stock = words[1] & 1u;
	for (k = 0; k < 3; k++)
	{
		base->sun[k] = word_float(words[2 + k]);
		base->sky[k] = word_float(words[5 + k]);
		base->domes[k] = word_float(words[8 + k]);
	}
	base->fixtures = word_float(words[11]);
	base->emission = word_float(words[12]);
	base->emissive_scale = word_float(words[13]);
	return 0;
}

/* live over base, where the base has light (else 1: a layer of no light
stays none, whatever the weight) */
static float over(float live, float base)
{
	if (!(base > 1.0e-12f) || !(live >= 0.0f) || !(live < 1.0e12f))
		return 1.0f;
	return live / base;
}

void prism_bake_layer_weights_identity(struct prism_bake_layer_weights *weights)
{
	int layer, k;

	memset(weights, 0, sizeof(*weights));
	for (layer = 0; layer < PRISM_BAKE_LAYERS; layer++)
		for (k = 0; k < 3; k++)
			weights->w[layer][k] = 1.0f;
}

void prism_bake_layer_weights_make(struct prism_bake_layer_light const *base, struct prism_bake_layer_light const *live,
	struct prism_bake_layer_weights *weights)
{
	int k;
	float lamps = over(live->emission * live->emissive_scale, base->emission * base->emissive_scale);

	prism_bake_layer_weights_identity(weights);
	for (k = 0; k < 3; k++)
	{
		weights->w[PRISM_BAKE_LAYER_SUN][k] = over(live->sun[k], base->sun[k]);
		weights->w[PRISM_BAKE_LAYER_SKY][k] = live->flat ? 0.0f : over(live->sky[k], base->sky[k]);
		weights->w[PRISM_BAKE_LAYER_DOMES][k] = over(live->domes[k], base->domes[k]);
		weights->w[PRISM_BAKE_LAYER_FIXTURES][k] = over(live->fixtures, base->fixtures);
		weights->w[PRISM_BAKE_LAYER_LAMPS][k] = lamps;
		weights->floor_outdoor[k] = live->flat && live->floor_outdoor[k] > 0.0f ? live->floor_outdoor[k] : 0.0f;
		weights->floor_indoor[k] = live->flat && live->floor_indoor[k] > 0.0f ? live->floor_indoor[k] : 0.0f;
	}
	weights->flat = live->flat ? 1 : 0;
}

int prism_bake_layers_identity(struct prism_bake_layer_weights const *weights)
{
	int layer, k;

	if (weights->flat)
		return 0;
	for (layer = 0; layer < PRISM_BAKE_LAYERS; layer++)
		for (k = 0; k < 3; k++)
			if (fabsf(weights->w[layer][k] - 1.0f) > 1.0e-5f)
				return 0;
	return 1;
}

int prism_bake_layer_weights_differ(struct prism_bake_layer_weights const *a, struct prism_bake_layer_weights const *b)
{
	int layer, k;

	if (a->flat != b->flat)
		return 1;
	for (k = 0; k < 3; k++)
	{
		for (layer = 0; layer < PRISM_BAKE_LAYERS; layer++)
			if (fabsf(a->w[layer][k] - b->w[layer][k]) > 1.0e-4f * (fabsf(b->w[layer][k]) + 1.0e-3f))
				return 1;
		if (fabsf(a->floor_outdoor[k] - b->floor_outdoor[k]) > 1.0e-6f ||
			fabsf(a->floor_indoor[k] - b->floor_indoor[k]) > 1.0e-6f)
			return 1;
	}
	return 0;
}

void prism_bake_layer_shares_put(float const total[3], float const layers[PRISM_BAKE_LAYER_SHARES][3], int indoor,
	uint8_t *bytes, uint32_t index, uint32_t stride)
{
	int layer, k;

	for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
		for (k = 0; k < 3; k++)
		{
			float share = total[k] > 1.0e-12f ? layers[layer][k] / total[k] : 0.0f;

			share = share > 0.0f ? (share < 1.0f ? share : 1.0f) : 0.0f;
			bytes[(size_t)(layer * 3 + k) * stride + index] = (uint8_t)(share * 255.0f + 0.5f);
		}
	bytes[(size_t)PRISM_BAKE_LAYER_SHARES * 3 * stride + index] = indoor ? PRISM_BAKE_LAYER_INDOOR : 0;
}

/* a texel's shares (the rest the lamps', which may be a step under 0: the
shares at their bytes sum to 1 exactly) */
static void shares_get(uint8_t const *bytes, uint32_t index, uint32_t stride, float s[PRISM_BAKE_LAYERS][3])
{
	int layer, k;

	for (k = 0; k < 3; k++)
	{
		float rest = 1.0f;

		for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
		{
			s[layer][k] = (float)bytes[(size_t)(layer * 3 + k) * stride + index] / 255.0f;
			rest -= s[layer][k];
		}
		s[PRISM_BAKE_LAYER_LAMPS][k] = rest;
	}
}

void prism_bake_layer_ratio(struct prism_bake_layer_weights const *weights, uint8_t const *bytes, uint32_t index,
	uint32_t stride, float ratio[3])
{
	float s[PRISM_BAKE_LAYERS][3];
	int layer, k;

	shares_get(bytes, index, stride, s);
	for (k = 0; k < 3; k++)
	{
		float r = 0.0f;

		for (layer = 0; layer < PRISM_BAKE_LAYERS; layer++)
			r += weights->w[layer][k] * s[layer][k];
		ratio[k] = r > 0.0f ? r : 0.0f;
	}
}

void prism_bake_layer_texel(struct prism_bake_layer_weights const *weights, uint8_t const *bytes, uint32_t index,
	uint32_t stride, float total[3], float indirect[3])
{
	float s[PRISM_BAKE_LAYERS][3];
	int layer, k, indoor = (bytes[(size_t)PRISM_BAKE_LAYER_SHARES * 3 * stride + index] & PRISM_BAKE_LAYER_INDOOR) != 0;

	shares_get(bytes, index, stride, s);
	for (k = 0; k < 3; k++)
	{
		float r = 0.0f, i_sun, i_sky, i_rest, rest_share, rest_weight, before, after;

		for (layer = 0; layer < PRISM_BAKE_LAYERS; layer++)
			r += weights->w[layer][k] * s[layer][k];
		/* (the indirect: the sun's bounce and the sky wholly so, the rest by
		the domes', fixtures' and lamps' weights over their shares) */
		i_sun = total[k] * s[PRISM_BAKE_LAYER_SUN][k];
		i_sky = total[k] * s[PRISM_BAKE_LAYER_SKY][k];
		i_rest = indirect[k] - i_sun - i_sky;
		i_rest = i_rest > 0.0f ? i_rest : 0.0f;
		rest_share = s[PRISM_BAKE_LAYER_DOMES][k] + s[PRISM_BAKE_LAYER_FIXTURES][k] + s[PRISM_BAKE_LAYER_LAMPS][k];
		rest_weight = rest_share > 1.0e-6f ? (weights->w[PRISM_BAKE_LAYER_DOMES][k] * s[PRISM_BAKE_LAYER_DOMES][k] +
			weights->w[PRISM_BAKE_LAYER_FIXTURES][k] * s[PRISM_BAKE_LAYER_FIXTURES][k] +
			weights->w[PRISM_BAKE_LAYER_LAMPS][k] * s[PRISM_BAKE_LAYER_LAMPS][k]) / rest_share : 1.0f;
		before = i_sun + i_sky + i_rest;
		after = weights->w[PRISM_BAKE_LAYER_SUN][k] * i_sun + weights->w[PRISM_BAKE_LAYER_SKY][k] * i_sky +
			rest_weight * i_rest;
		indirect[k] = before > 1.0e-12f ? indirect[k] * (after > 0.0f ? after : 0.0f) / before : indirect[k] * r;
		total[k] = total[k] * (r > 0.0f ? r : 0.0f);
		if (weights->flat)
			total[k] += indoor ? weights->floor_indoor[k] : weights->floor_outdoor[k];
	}
}

uint32_t prism_bake_rgb9e5(float const rgb[3])
{
	float most, r, g, b, scale;
	int exponent, shared;
	uint32_t ri, gi, bi;

	r = rgb[0] > 0.0f ? (rgb[0] < 65408.0f ? rgb[0] : 65408.0f) : 0.0f;
	g = rgb[1] > 0.0f ? (rgb[1] < 65408.0f ? rgb[1] : 65408.0f) : 0.0f;
	b = rgb[2] > 0.0f ? (rgb[2] < 65408.0f ? rgb[2] : 65408.0f) : 0.0f;
	most = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (!(most > 1.0e-30f))
		return 0;
	exponent = (int)floorf(log2f(most)) + 1;
	shared = exponent + 15;
	if (shared < 0)
		shared = 0;
	if (shared > 31)
		shared = 31;
	scale = exp2f((float)(shared - 15 - 9));
	if ((uint32_t)floorf(most / scale + 0.5f) == 512u && shared < 31)
	{
		shared++;
		scale *= 2.0f;
	}
	ri = (uint32_t)floorf(r / scale + 0.5f);
	gi = (uint32_t)floorf(g / scale + 0.5f);
	bi = (uint32_t)floorf(b / scale + 0.5f);
	ri = ri > 511u ? 511u : ri;
	gi = gi > 511u ? 511u : gi;
	bi = bi > 511u ? 511u : bi;
	return ri | (gi << 9) | (bi << 18) | ((uint32_t)shared << 27);
}

void prism_bake_rgb9e5_decode(uint32_t v, float rgb[3])
{
	float e = exp2f((float)((int)(v >> 27) - 15 - 9));

	rgb[0] = (float)(v & 511u) * e;
	rgb[1] = (float)((v >> 9) & 511u) * e;
	rgb[2] = (float)((v >> 18) & 511u) * e;
}

void prism_bake_layers_tile(struct prism_bake_layer_weights const *weights, uint32_t const *file, uint8_t const *bytes,
	uint32_t count, uint32_t *raster, uint32_t *indirect)
{
	uint32_t index;
	int same = !bytes || prism_bake_layers_identity(weights);

	for (index = 0; index < count; index++)
	{
		float total[3], light[3];

		raster[index * 2 + 1] = file[index * 4 + 1];
		/* (an empty texel, or the bake's own light: its words as they are) */
		if (same || (((file[index * 4 + 3] >> 24) & 3u) == 0u))
		{
			raster[index * 2] = file[index * 4];
			if (indirect)
				indirect[index] = file[index * 4 + 2];
			continue;
		}
		prism_bake_rgb9e5_decode(file[index * 4], total);
		prism_bake_rgb9e5_decode(file[index * 4 + 2], light);
		prism_bake_layer_texel(weights, bytes, index, count, total, light);
		raster[index * 2] = prism_bake_rgb9e5(total);
		if (indirect)
			indirect[index] = prism_bake_rgb9e5(light);
	}
}

float prism_bake_half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h >> 15) << 31, exponent = (h >> 10) & 31u, mantissa = h & 1023u, bits;
	float value;

	if (!exponent)
		value = (float)mantissa * (1.0f / 16777216.0f);
	else if (exponent == 31)
		value = 65504.0f;
	else
	{
		bits = ((exponent + 112u) << 23) | (mantissa << 13);
		memcpy(&value, &bits, 4);
	}
	return sign ? -value : value;
}

uint16_t prism_bake_float_to_half(float f)
{
	uint32_t bits, sign, exponent, mantissa;

	memcpy(&bits, &f, 4);
	sign = (bits >> 16) & 0x8000u;
	exponent = (bits >> 23) & 0xffu;
	mantissa = bits & 0x7fffffu;
	if (exponent >= 143)
		return (uint16_t)(sign | 0x7bffu);
	if (exponent <= 112)
		return (uint16_t)sign;
	return (uint16_t)(sign | ((exponent - 112u) << 10) | (mantissa >> 13));
}

/* a probe's SH coefficient j (0 to 11: red's 4, green's, blue's) */
static float probe_get(uint32_t const *p, int j)
{
	return prism_bake_half_to_float((uint16_t)(j & 1 ? p[j / 2] >> 16 : p[j / 2] & 0xffffu));
}

static void probe_set(uint32_t *p, int j, float value)
{
	uint32_t h = prism_bake_float_to_half(value);

	p[j / 2] = j & 1 ? (p[j / 2] & 0xffffu) | (h << 16) : (p[j / 2] & 0xffff0000u) | h;
}

void prism_bake_layers_probes(struct prism_bake_layer_weights const *weights, uint32_t const *words,
	uint8_t const *bytes, uint32_t count, uint32_t *out)
{
	uint32_t index;
	int same = !bytes || prism_bake_layers_identity(weights);

	if (out != words)
		memcpy(out, words, (size_t)count * 8 * 4);
	if (same)
		return;
	for (index = 0; index < count; index++)
	{
		uint32_t *p = out + (size_t)index * 8;
		float ratio[3], valid = word_float(p[7]);
		int k, j, indoor = (bytes[(size_t)PRISM_BAKE_LAYER_SHARES * 3 * count + index] & PRISM_BAKE_LAYER_INDOOR) != 0;

		if (!(valid > 0.0f))
			continue;
		prism_bake_layer_ratio(weights, bytes, index, count, ratio);
		for (k = 0; k < 3; k++)
		{
			for (j = 0; j < 4; j++)
				probe_set(p, k * 4 + j, probe_get(p, k * 4 + j) * ratio[k]);
			if (weights->flat)
				probe_set(p, k * 4, probe_get(p, k * 4) + FLOOR_L0 *
					(indoor ? weights->floor_indoor[k] : weights->floor_outdoor[k]));
		}
	}
}

void prism_bake_layers_probes_mean(struct prism_bake_layer_weights const *weights, uint32_t const *words,
	uint8_t const *bytes, uint32_t count, double sum[3], double weight[3])
{
	uint32_t index;

	for (index = 0; index < count; index++)
	{
		uint32_t const *p = words + (size_t)index * 8;
		float ratio[3];
		int k;

		if (!(word_float(p[7]) > 0.0f))
			continue;
		if (bytes)
			prism_bake_layer_ratio(weights, bytes, index, count, ratio);
		else
			ratio[0] = ratio[1] = ratio[2] = 1.0f;
		for (k = 0; k < 3; k++)
		{
			float l0 = probe_get(p, k * 4);

			if (l0 > 0.0f && l0 < 1.0e6f)
			{
				sum[k] += (double)l0 * ratio[k];
				weight[k] += l0;
			}
		}
	}
}

void prism_bake_layers_scale(uint32_t *words, uint32_t count, float const ratio[3])
{
	uint32_t index;

	if (fabsf(ratio[0] - 1.0f) < 1.0e-5f && fabsf(ratio[1] - 1.0f) < 1.0e-5f && fabsf(ratio[2] - 1.0f) < 1.0e-5f)
		return;
	for (index = 0; index < count; index++)
	{
		float rgb[3];
		int k;

		if (!words[index])
			continue;
		prism_bake_rgb9e5_decode(words[index], rgb);
		for (k = 0; k < 3; k++)
			rgb[k] *= ratio[k];
		words[index] = prism_bake_rgb9e5(rgb);
	}
}
