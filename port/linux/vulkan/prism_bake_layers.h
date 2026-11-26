/*
PRISM_BAKE_LAYERS.H

The layered bake (docs/prism_bake.md, "The layers"): the bake's light is
kept by the kind of light it came from, so that a setting that only scales
or tints a light (display.prism_sky_light, the rig's scales, the lamps'
brightness) is recombined from the bake at load, with no rebake.

Every term of the bake's estimator is linear in its light's colour x power,
channel by channel, so a texel's light (less the sun's direct light, which
Raster and Path Traced take live) is the sum of five layers:

	0 the sun's bounce		by the rig's sun (colour x power)
	1 the sky			by the rig's sky (its open radiance); 0
					under Tool's flat fill
	2 the domes			by the domes' colour x power, summed
	3 the fixtures			by the rig's fixtures' scale
	4 the lamps and glows		by the materials' largest emission and
					display.prism_rt_emissive_scale

each with every bounce it starts. A texel keeps its light as baked (the
filtered total, the indirect alone) and its layers' shares of it: per
channel, layers 0 to 3 as bytes (share x 255), layer 4 the rest. At load:

	total' = total x sum_i w_i s_i (+ the flat fill's floor)
	indirect' = indirect x (w_0 I_0 + w_1 I_1 + w_r I_r) / (I_0 + I_1 + I_r)

where w_i is the light's live colour over the one baked, I_0 and I_1 the
sun's bounce and the sky (wholly indirect), I_r the indirect light left
(the domes', fixtures' and lamps' bounces) and w_r their weights by their
shares. At the bake's own light every w is 1 and the light is the bake's,
exactly (prism_bake_layers_identity: the words are copied). The probes'
shares are of their SH L0, and one ratio a channel scales all four of their
coefficients; the reflection probes take one ratio a channel for the BSP.

The shares' bytes, a tile's or a brick's, are planar: layer 0's red for
every texel, then its green, its blue, then layer 1's..., then a byte of
flags a texel (1: an indoor cluster's, whose flat fill is sky 0's indoor
ambient). PRISM_BAKE_LAYER_BYTES a texel.

Host code with no engine dependency, written as C89.
*/

#ifndef PRISM_BAKE_LAYERS_H
#define PRISM_BAKE_LAYERS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PRISM_BAKE_LAYERS = 5,
	PRISM_BAKE_LAYER_SHARES = 4, /* the layers kept as shares (the last is the rest) */
	PRISM_BAKE_LAYER_BYTES = PRISM_BAKE_LAYER_SHARES * 3 + 1,
	PRISM_BAKE_LAYER_INDOOR = 1, /* a texel's flag */
	PRISM_BAKE_LAYER_BASE_VERSION = 1,
	PRISM_BAKE_LAYER_BASE_WORDS = 24
};

enum
{
	PRISM_BAKE_LAYER_SUN,
	PRISM_BAKE_LAYER_SKY,
	PRISM_BAKE_LAYER_DOMES,
	PRISM_BAKE_LAYER_FIXTURES,
	PRISM_BAKE_LAYER_LAMPS
};

/* a light's colours and powers: the bake's (its base) or the live rig's */
struct prism_bake_layer_light
{
	float sun[3], sky[3], domes[3];
	float fixtures; /* the fixtures' scale */
	float emission; /* the lamps' materials' largest emission */
	float emissive_scale; /* display.prism_rt_emissive_scale (the bake's: 1) */
	/* (live only) Tool's flat fill (display.prism_sky_light "flat"): its
	outdoor and indoor floors */
	int flat;
	float floor_outdoor[3], floor_indoor[3];
	/* (the base only) the geometry it was baked from was the stock world's
	(1) or a remodel's (0): a distributable bake is a stock one's */
	int stock;
};

/* the weights of each layer (per channel) and the flat fill's floors */
struct prism_bake_layer_weights
{
	float w[PRISM_BAKE_LAYERS][3];
	int flat;
	float floor_outdoor[3], floor_indoor[3];
};

/* the base's record (PRISM_BAKE_RECORD_LAYER_BASE): PRISM_BAKE_LAYER_BASE_WORDS
little-endian words; and read back (0, or -1 where it is not one) */
void prism_bake_layer_base_write(struct prism_bake_layer_light const *base, uint32_t words[PRISM_BAKE_LAYER_BASE_WORDS]);
int prism_bake_layer_base_read(uint32_t const *words, size_t count, struct prism_bake_layer_light *base);

/* the live light over the base: each layer's weights */
void prism_bake_layer_weights_make(struct prism_bake_layer_light const *base, struct prism_bake_layer_light const *live,
	struct prism_bake_layer_weights *weights);
/* the weights that leave the bake as it is (every one 1, no fill) */
void prism_bake_layer_weights_identity(struct prism_bake_layer_weights *weights);
int prism_bake_layers_identity(struct prism_bake_layer_weights const *weights);
/* whether two weights differ enough to recombine again */
int prism_bake_layer_weights_differ(struct prism_bake_layer_weights const *a, struct prism_bake_layer_weights const *b);

/* a texel's shares from its total light and its four layers' (RGB each),
into its count's planar bytes at index (stride: the texels in the block) */
void prism_bake_layer_shares_put(float const total[3], float const layers[PRISM_BAKE_LAYER_SHARES][3], int indoor,
	uint8_t *bytes, uint32_t index, uint32_t stride);

/* a texel's light recombined: total and indirect (in place) by its shares
(the planar bytes at index of stride texels) and the weights */
void prism_bake_layer_texel(struct prism_bake_layer_weights const *weights, uint8_t const *bytes, uint32_t index,
	uint32_t stride, float total[3], float indirect[3]);

/* a channel ratio of a texel (its total's factor, without the fill) */
void prism_bake_layer_ratio(struct prism_bake_layer_weights const *weights, uint8_t const *bytes, uint32_t index,
	uint32_t stride, float ratio[3]);

/* RGB9E5 (the bake's light words) */
uint32_t prism_bake_rgb9e5(float const rgb[3]);
void prism_bake_rgb9e5_decode(uint32_t v, float rgb[3]);

/* a lightmap tile recombined: count texels' file words (4 each: the light,
the direction, the indirect light, the sun's word) and their shares (NULL:
none, as baked) into the raster's words (2 each: the light, the direction)
and the indirect pool's (1 each) */
void prism_bake_layers_tile(struct prism_bake_layer_weights const *weights, uint32_t const *file, uint8_t const *bytes,
	uint32_t count, uint32_t *raster, uint32_t *indirect);

/* a brick's probes recombined: count probes' words (8 each: SH as 12 half
floats, red's 4 then green's and blue's, the sun's visibility, whether it is
valid) by their shares (NULL: as baked) into out (the same layout). The
flat fill's floor is added to L0 by the probe's flag */
void prism_bake_layers_probes(struct prism_bake_layer_weights const *weights, uint32_t const *words,
	uint8_t const *bytes, uint32_t count, uint32_t *out);

/* the light-weighted mean ratio a channel of a brick's probes (to sum[3]
and weight[3], added to) */
void prism_bake_layers_probes_mean(struct prism_bake_layer_weights const *weights, uint32_t const *words,
	uint8_t const *bytes, uint32_t count, double sum[3], double weight[3]);

/* RGB9E5 words scaled by a ratio a channel */
void prism_bake_layers_scale(uint32_t *words, uint32_t count, float const ratio[3]);

/* half floats (the probes' SH) */
float prism_bake_half_to_float(uint16_t h);
uint16_t prism_bake_float_to_half(float f);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_LAYERS_H */
