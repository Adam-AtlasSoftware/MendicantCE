/* character_test.c: the soldier character's draw checked on the build machine,
no game (port/linux/game/mendicant/mendicant_traits.h; docs/mendicant_port.md,
"The soldier character"):

    cc -O2 -std=c11 -Iport/linux/game/mendicant tools/mendicant/character_test.c -o build/character_test -lm
    build/character_test

For every kind, personality strength (0, 0.5, 1, 2) and quirk visibility:
every trait in 0..1; the multipliers' mean over a fresh sample 1 +- 0.01
(lo/hi pairs either way round); the quirks' frequencies within 0.5 points
of their chances; the constants in range; the same seed the same soldier;
the same soldiers under 10 nonces uncorrelated (|r| < 0.15); a Covenant
soldier new in a new epoch, a Marine the same; the moods' frequencies and
each scale's weighted mean within 1.5% of 1. It prints each kind's measured
means and spreads at the default settings, and exits 1 on a failure. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mendicant_traits.h"

#define SAMPLES 100000

static int failures = 0;

#define CHECK(condition, ...) \
	do { if (!(condition)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double correlation(double const *a, double const *b, int count)
{
	double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
	int index;

	for (index = 0; index < count; index++)
	{
		ma += a[index];
		mb += b[index];
	}
	ma /= count;
	mb /= count;
	for (index = 0; index < count; index++)
	{
		sab += (a[index] - ma) * (b[index] - mb);
		saa += (a[index] - ma) * (a[index] - ma);
		sbb += (b[index] - mb) * (b[index] - mb);
	}
	return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0.0;
}

static void check_kind(int kind, float strength, int visibility, int print)
{
	static double mult_pairs[4][2] = { { 0.65, 1.4 }, { 1.4, 0.65 }, { 0.5, 1.5 }, { 1.6, 0.6 } };
	float population[MENDICANT_TRAITS];
	double sum[MENDICANT_TRAITS] = { 0 }, square[MENDICANT_TRAITS] = { 0 };
	double mult_sum[MENDICANT_TRAITS][4] = { { 0 } };
	long quirks[MENDICANT_QUIRKS] = { 0 };
	struct mendicant_traits traits;
	int sample, trait, pair, quirk;
	int group = mendicant_kind_group[kind];

	mendicant_traits_population_means(kind, strength, visibility, population);
	for (sample = 0; sample < SAMPLES; sample++)
	{
		/* (other seeds than the population means') */
		mendicant_traits_draw(&traits, kind, mendicant_mix64(0xABCDEF12345ULL + (unsigned long long)kind * 7919,
			(unsigned long long)sample), strength, visibility);
		quirks[traits.quirk]++;
		for (trait = 0; trait < MENDICANT_TRAITS; trait++)
		{
			double value = traits.trait[trait] / 255.0;

			sum[trait] += value;
			square[trait] += value * value;
			for (pair = 0; pair < 4; pair++)
				mult_sum[trait][pair] += mendicant_traits_mult((float)value, population[trait],
					(float)mult_pairs[pair][0], (float)mult_pairs[pair][1]);
		}
		CHECK(traits.lead_bias >= 0.6f && traits.lead_bias <= 1.25f, "%s lead_bias %f", mendicant_kind_names[kind], traits.lead_bias);
		CHECK(traits.buddy_pref >= 0.0f && traits.buddy_pref <= 1.0f, "%s buddy %f", mendicant_kind_names[kind], traits.buddy_pref);
		CHECK(kind != MENDICANT_KIND_NONE || traits.quirk == MENDICANT_QUIRK_NONE, "no-character kind with a quirk");
	}
	for (trait = 0; trait < MENDICANT_TRAITS; trait++)
	{
		for (pair = 0; pair < 4; pair++)
		{
			double mean = mult_sum[trait][pair] / SAMPLES;

			CHECK(fabs(mean - 1.0) <= 0.01, "%s %s mult(%g,%g) mean %.4f (strength %g, visibility %d)",
				mendicant_kind_names[kind], mendicant_trait_names[trait], mult_pairs[pair][0], mult_pairs[pair][1],
				mean, strength, visibility);
		}
		if (strength == 0.0f && visibility == MENDICANT_QUIRKS_OFF)
			CHECK(fabs(sum[trait] / SAMPLES - (kind ? mendicant_kind_mean[kind][trait] : 0.5)) < 0.003,
				"%s %s at strength 0 is not the kind's mean", mendicant_kind_names[kind], mendicant_trait_names[trait]);
	}
	for (quirk = 1; quirk < MENDICANT_QUIRKS; quirk++)
	{
		double expected = mendicant_quirk_definitions[quirk].group == group ?
			mendicant_quirk_definitions[quirk].chance * mendicant_visibility_rate[visibility] : 0.0;
		double measured = (double)quirks[quirk] / SAMPLES;

		CHECK(fabs(measured - expected) <= 0.005, "%s quirk %s %.4f expected %.4f", mendicant_kind_names[kind],
			mendicant_quirk_definitions[quirk].name, measured, expected);
	}
	if (print)
	{
		printf("%-16s quirk %4.1f%%  mean/sd:", mendicant_kind_names[kind], 100.0 * (SAMPLES - quirks[0]) / SAMPLES);
		for (trait = 0; trait < MENDICANT_TRAITS; trait++)
		{
			double mean = sum[trait] / SAMPLES;

			printf(" %.2f/%.2f", mean, sqrt(square[trait] / SAMPLES - mean * mean));
		}
		printf("\n");
	}
}

int main(void)
{
	static float const strengths[4] = { 0.0f, 0.5f, 1.0f, 2.0f };
	struct mendicant_traits a, b;
	int kind, strength, visibility, trait, nonce, mood, scale;

	printf("kinds at personality 1, quirks normal (courage aggression skill discipline caution grenade initiative vocal loyalty reflex):\n");
	for (kind = 0; kind < MENDICANT_KINDS; kind++)
		for (strength = 0; strength < 4; strength++)
			for (visibility = 0; visibility < MENDICANT_QUIRK_VISIBILITIES; visibility++)
				check_kind(kind, strengths[strength], visibility, strength == 2 && visibility == MENDICANT_QUIRKS_NORMAL);

	/* the same seed, the same soldier */
	mendicant_traits_draw(&a, MENDICANT_KIND_ELITE_MAJOR, 12345, 1.0f, MENDICANT_QUIRKS_NORMAL);
	mendicant_traits_draw(&b, MENDICANT_KIND_ELITE_MAJOR, 12345, 1.0f, MENDICANT_QUIRKS_NORMAL);
	CHECK(!memcmp(a.trait, b.trait, sizeof(a.trait)) && a.quirk == b.quirk && a.lead_bias == b.lead_bias, "same seed, different soldier");

	/* 10 nonces: the same 2000 actor slots' courage uncorrelated between sessions */
	{
		static double values[10][2000];
		double worst = 0.0;
		int other, slot;

		for (nonce = 0; nonce < 10; nonce++)
			for (slot = 0; slot < 2000; slot++)
			{
				unsigned long long seed = mendicant_seed_covenant(mendicant_mix64(0x1234ULL, (unsigned long long)nonce), 1,
					(unsigned long)(0xE0010000UL | (unsigned long)slot), 0xE1A40123UL);

				mendicant_traits_draw(&a, MENDICANT_KIND_GRUNT_MINOR, seed, 1.0f, MENDICANT_QUIRKS_NORMAL);
				values[nonce][slot] = a.trait[MENDICANT_TRAIT_COURAGE];
			}
		for (nonce = 0; nonce < 10; nonce++)
			for (other = nonce + 1; other < 10; other++)
			{
				double r = fabs(correlation(values[nonce], values[other], 2000));

				if (r > worst)
					worst = r;
			}
		printf("same soldiers under 10 nonces: worst |r| %.3f (pass < 0.15)\n", worst);
		CHECK(worst < 0.15, "same-soldier correlation across nonces %.3f", worst);
	}

	/* revert: a Covenant soldier new in a new epoch, a Marine the same man */
	{
		int covenant_same = 0, marine_same = 0, slot;

		for (slot = 0; slot < 1000; slot++)
		{
			unsigned long actor = 0x80000000UL | (unsigned long)slot;

			mendicant_traits_draw(&a, MENDICANT_KIND_ELITE_MINOR, mendicant_seed_covenant(77, 3, actor, 9), 1.0f, MENDICANT_QUIRKS_NORMAL);
			mendicant_traits_draw(&b, MENDICANT_KIND_ELITE_MINOR, mendicant_seed_covenant(77, 4, actor, 9), 1.0f, MENDICANT_QUIRKS_NORMAL);
			covenant_same += !memcmp(a.trait, b.trait, sizeof(a.trait));
			mendicant_traits_draw(&a, MENDICANT_KIND_MARINE, mendicant_seed_marine(77, actor, 0xE4410022UL, 3), 1.0f, MENDICANT_QUIRKS_NORMAL);
			mendicant_traits_draw(&b, MENDICANT_KIND_MARINE, mendicant_seed_marine(77, actor, 0xE4410022UL, 3), 1.0f, MENDICANT_QUIRKS_NORMAL);
			marine_same += !memcmp(a.trait, b.trait, sizeof(a.trait)) && a.quirk == b.quirk;
		}
		printf("revert: Covenant redrawn %d/1000 the same, Marines kept %d/1000 the same\n", covenant_same, marine_same);
		CHECK(covenant_same < 5, "Covenant soldiers not redrawn (%d)", covenant_same);
		CHECK(marine_same == 1000, "Marines not kept (%d)", marine_same);
	}

	/* the moods */
	{
		long counts[MENDICANT_MOODS] = { 0 };
		long sample;

		for (sample = 0; sample < SAMPLES; sample++)
			counts[mendicant_mood_draw(mendicant_mix64(0xF16ULL, (unsigned long long)sample))]++;
		for (mood = 0; mood < MENDICANT_MOODS; mood++)
			CHECK(fabs((double)counts[mood] / SAMPLES - mendicant_mood_chance[mood]) < 0.005, "mood %s frequency %.4f",
				mendicant_mood_names[mood], (double)counts[mood] / SAMPLES);
		for (scale = 0; scale < MENDICANT_MOOD_SCALES; scale++)
		{
			double mean = 0.0;

			for (mood = 0; mood < MENDICANT_MOODS; mood++)
				mean += mendicant_mood_chance[mood] * mendicant_mood_scale[mood][scale];
			printf("mood scale %d weighted mean %.4f\n", scale, mean);
			CHECK(fabs(mean - 1.0) <= 0.015, "mood scale %d mean %.4f", scale, mean);
		}
	}

	(void)trait;
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
