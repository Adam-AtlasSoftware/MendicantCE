/* mendicant_traits.h: the soldier character's tables and draw (MENDICANT AI,
port/linux/game/mendicant; docs/mendicant_port.md, "The soldier character")

Plain C with no game header, so that tools/mendicant/character_test.c can
check the draw's statistics on the build machine: included by
mendicant_character.c alone. Everything here is a pure function of a 64-bit
seed and the two settings (personality strength, quirk visibility).

Ten core traits, each 0..1 with the kind's mean: every later behaviour reads
these (or a multiplier made from one), so no behaviour keeps a private trait
table. Each trait is drawn triangular around the kind's mean (no long tails),
its spread times the personality strength. At most one quirk per soldier: a
named archetype the player can learn to recognise, which pushes two to four
traits toward the edge of the kind's range and carries one signature beat
(the one thing it does, loudly, once in a fight). Hunters, the Flood,
Sentinels, Engineers and mounted weapons have no character: every reader gets
the neutral value. */
#ifndef HALO_MENDICANT_TRAITS_H
#define HALO_MENDICANT_TRAITS_H

#include "mendicant_character.h"

/* ---------- tables */

/* means (courage, aggression, skill, discipline, caution, grenade,
initiative, vocal, loyalty, reflex), from the area notes' table
(authenticity_research/character.md 3.3) */
static float const mendicant_kind_mean[MENDICANT_KINDS][MENDICANT_TRAITS] =
{
	{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },       /* none */
	{ .25f, .35f, .30f, .30f, .55f, .45f, .25f, .75f, .60f, .45f },       /* grunt minor */
	{ .45f, .45f, .40f, .45f, .50f, .60f, .40f, .70f, .65f, .50f },       /* grunt major */
	{ .65f, .55f, .55f, .70f, .45f, .65f, .55f, .50f, .75f, .65f },       /* grunt specops */
	{ .10f, .20f, .30f, .30f, .70f, .30f, .20f, .80f, .50f, .50f },       /* grunt flee */
	{ .45f, .35f, .55f, .70f, .65f, .05f, .35f, .45f, .55f, .60f },       /* jackal minor */
	{ .55f, .40f, .65f, .75f, .60f, .05f, .45f, .45f, .60f, .65f },       /* jackal major */
	{ .70f, .65f, .55f, .55f, .35f, .40f, .60f, .55f, .60f, .70f },       /* elite minor */
	{ .80f, .75f, .65f, .60f, .35f, .45f, .70f, .60f, .65f, .75f },       /* elite major */
	{ .90f, .70f, .75f, .80f, .40f, .50f, .85f, .65f, .80f, .80f },       /* elite commander */
	{ .80f, .60f, .70f, .80f, .50f, .55f, .70f, .40f, .80f, .80f },       /* elite specops */
	{ .75f, .70f, .65f, .60f, .30f, .30f, .80f, .25f, .60f, .85f },       /* elite stealth */
	{ .60f, .50f, .50f, .60f, .55f, .40f, .50f, .70f, .70f, .55f },       /* marine */
	{ .70f, .55f, .60f, .70f, .55f, .40f, .55f, .65f, .75f, .60f },       /* marine armored / major */
	{ .85f, .60f, .65f, .85f, .50f, .45f, .80f, .85f, .85f, .65f },       /* sergeant */
};

/* the spread of each trait (the triangle's half-width is twice this; 0.12
unless the table says otherwise) */
static float const mendicant_kind_spread[MENDICANT_KINDS][MENDICANT_TRAITS] =
{
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ .15f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .05f, .05f, .05f, .05f, .05f, .05f, .05f, .05f, .05f, .05f },
	{ .12f, .12f, .15f, .12f, .12f, .03f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .15f, .12f, .12f, .03f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .08f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .15f, .12f, .15f, .12f, .12f, .12f, .12f, .18f, .12f, .12f },
	{ .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
	{ .06f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f, .12f },
};

static unsigned char const mendicant_kind_group[MENDICANT_KINDS] =
{
	MENDICANT_GROUP_NONE,
	MENDICANT_GROUP_GRUNT, MENDICANT_GROUP_GRUNT, MENDICANT_GROUP_GRUNT,
	MENDICANT_GROUP_NONE, /* the scripted cowards: no quirk */
	MENDICANT_GROUP_JACKAL, MENDICANT_GROUP_JACKAL,
	MENDICANT_GROUP_ELITE, MENDICANT_GROUP_ELITE, MENDICANT_GROUP_ELITE, MENDICANT_GROUP_ELITE, MENDICANT_GROUP_ELITE,
	MENDICANT_GROUP_MARINE, MENDICANT_GROUP_MARINE,
	MENDICANT_GROUP_NONE, /* the sergeant is already the squad's voice: no quirk */
};

static char const *const mendicant_kind_names[MENDICANT_KINDS] =
{
	"none", "grunt_minor", "grunt_major", "grunt_specops", "grunt_flee", "jackal_minor", "jackal_major",
	"elite_minor", "elite_major", "elite_commander", "elite_specops", "elite_stealth",
	"marine", "marine_armored", "sergeant",
};

static char const *const mendicant_trait_names[MENDICANT_TRAITS] =
{
	"courage", "aggression", "skill", "discipline", "caution", "grenade", "initiative", "vocal", "loyalty", "reflex",
};

/* each quirk: its group, its chance within the group (a few loud archetypes:
about 35% of Grunts and Marines, 27% of Jackals, 30% of Elites), and how far
it pushes each trait */
struct mendicant_quirk_definition
{
	char const *name;
	unsigned char group;
	float chance;
	float push[MENDICANT_TRAITS];
};

static struct mendicant_quirk_definition const mendicant_quirk_definitions[MENDICANT_QUIRKS] =
{
	/*                                         cou    agg    ski    dis    cau    gre    ini    voc    loy    ref */
	{ "none", MENDICANT_GROUP_NONE, 0.0f,    { 0 } },
	{ "coward", MENDICANT_GROUP_GRUNT, .17f, { -.20f,  0,     0,     -.05f, .10f,  0,     0,     .15f,  0,     0 } },
	{ "brave", MENDICANT_GROUP_GRUNT, .08f,  { .40f,   .20f,  0,     .10f,  -.10f, .15f,  0,     0,     .10f,  0 } },
	{ "panic_thrower", MENDICANT_GROUP_GRUNT, .10f, { -.05f, 0, -.10f, -.15f, 0,     .30f,  0,     .10f,  0,     0 } },
	{ "sharpshooter", MENDICANT_GROUP_JACKAL, .12f, { 0, 0,  .20f,  .10f,  0,     0,     0,     0,     0,     .10f } },
	{ "skittish", MENDICANT_GROUP_JACKAL, .10f, { -.15f, -.10f, 0,    0,     .20f,  0,     0,     0,     0,     .05f } },
	{ "charger", MENDICANT_GROUP_JACKAL, .05f, { .15f,   .35f,  0,     -.10f, -.20f, 0,     .10f,  0,     0,     0 } },
	{ "duelist", MENDICANT_GROUP_ELITE, .10f, { .10f,    .20f,  0,     0,     -.15f, -.15f, .05f,  0,     0,     0 } },
	{ "tactician", MENDICANT_GROUP_ELITE, .12f, { 0,     -.05f, .05f,  .20f,  .15f,  .25f,  .10f,  .10f,  0,     0 } },
	{ "berserker", MENDICANT_GROUP_ELITE, .08f, { .05f,  .25f,  0,     -.15f, -.15f, 0,     0,     .05f,  0,     0 } },
	{ "veteran", MENDICANT_GROUP_MARINE, .09f, { .20f,   0,     .15f,  .05f,  0,     -.10f, 0,     -.10f, 0,     .05f } },
	{ "rookie", MENDICANT_GROUP_MARINE, .09f, { -.20f,   0,     -.15f, -.05f, .05f,  0,     0,     .10f,  0,     .10f } },
	{ "hothead", MENDICANT_GROUP_MARINE, .07f, { .05f,   .30f,  0,     -.20f, -.15f, 0,     .10f,  .05f,  0,     0 } },
	{ "joker", MENDICANT_GROUP_MARINE, .06f, { 0,        0,     0,     -.05f, 0,     0,     0,     .25f,  0,     0 } },
	{ "packrat", MENDICANT_GROUP_MARINE, .04f, { 0,      0,     0,     0,     0,     0,     .25f,  0,     -.05f, 0 } },
};

static char const *const mendicant_mood_names[MENDICANT_MOODS] =
{
	"steady", "jittery", "fired_up", "sparing", "liberal",
};

/* each mood's chance and what it scales; the chances weigh each scale's mean
to within 1.5% of 1, so the moods spread fights without moving the average */
static float const mendicant_mood_chance[MENDICANT_MOODS] = { .35f, .20f, .20f, .125f, .125f };
static float const mendicant_mood_scale[MENDICANT_MOODS][MENDICANT_MOOD_SCALES] =
{
	/* aim error, grenade lull, spacing, reaction */
	{ 1.00f, 1.00f, 1.00f, 1.00f }, /* steady */
	{ 1.10f, 1.00f, 0.95f, 0.90f }, /* jittery: quick and loose */
	{ 0.95f, 0.80f, 0.80f, 0.95f }, /* fired up: closer, more grenades */
	{ 0.95f, 1.60f, 1.25f, 1.15f }, /* sparing: careful, few grenades */
	{ 1.05f, 0.60f, 1.05f, 1.00f }, /* liberal: grenades freely */
};

/* quirk visibility: the quirk rate, how far quirks push, and the chance that
a soldier's signature beat is due in a fight */
static float const mendicant_visibility_rate[MENDICANT_QUIRK_VISIBILITIES] = { 0.0f, 0.6f, 1.0f, 1.3f };
static float const mendicant_visibility_push[MENDICANT_QUIRK_VISIBILITIES] = { 0.0f, 0.7f, 1.0f, 1.35f };
static float const mendicant_visibility_signature[MENDICANT_QUIRK_VISIBILITIES] = { 0.0f, 0.4f, 0.75f, 1.0f };

/* ---------- random numbers: splitmix64, the character's own (never the game's) */

static unsigned long long mendicant_splitmix64(
	unsigned long long *state)
{
	unsigned long long z = (*state += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

/* one value's hash: splitmix64's finaliser of a and b together */
static unsigned long long mendicant_mix64(
	unsigned long long a,
	unsigned long long b)
{
	unsigned long long state = a ^ (b * 0xD6E8FEB86659FD93ULL);

	return mendicant_splitmix64(&state);
}

/* 0..1 (24 bits) */
static float mendicant_random_unit(
	unsigned long long *state)
{
	return (float)(mendicant_splitmix64(state) >> 40) * (1.0f / 16777216.0f);
}

/* -1..1, triangular (no long tails) */
static float mendicant_random_triangle(
	unsigned long long *state)
{
	return mendicant_random_unit(state) + mendicant_random_unit(state) - 1.0f;
}

static float mendicant_clamp_unit(
	float value)
{
	return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static unsigned char mendicant_trait_byte(
	float value)
{
	return (unsigned char)(mendicant_clamp_unit(value) * 255.0f + 0.5f);
}

/* ---------- seeds */

#define MENDICANT_MARINE_SALT 0x4D4152494E45ULL /* "MARINE" */

/* a Covenant soldier: new in each epoch (each map start and loaded game state) */
static unsigned long long mendicant_seed_covenant(
	unsigned long long nonce,
	unsigned long epoch,
	unsigned long actor_index,
	unsigned long variant_tag)
{
	return mendicant_mix64(mendicant_mix64(nonce, (unsigned long long)epoch + 1),
		(unsigned long long)actor_index | (unsigned long long)variant_tag << 32);
}

/* a Marine: no epoch, so a revert (which restores his datum index) brings the
same man back; his unit's tag and face and voice variant are part of who he is */
static unsigned long long mendicant_seed_marine(
	unsigned long long nonce,
	unsigned long actor_index,
	unsigned long unit_tag,
	unsigned short variant_number)
{
	unsigned long long who = (unsigned long long)actor_index ^ (unsigned long long)unit_tag << 32 ^
		(unsigned long long)variant_number << 20;

	return mendicant_mix64(mendicant_mix64(nonce, MENDICANT_MARINE_SALT), who);
}

/* ---------- the draw */

/* a soldier of a kind from its seed: traits, quirk, constants. strength scales
the spread (0: every soldier at the kind's mean); visibility the quirks. */
static void mendicant_traits_draw(
	struct mendicant_traits *traits,
	int kind,
	unsigned long long seed,
	float strength,
	int visibility)
{
	unsigned long long state = seed;
	float values[MENDICANT_TRAITS];
	int group = kind > 0 && kind < MENDICANT_KINDS ? mendicant_kind_group[kind] : MENDICANT_GROUP_NONE;
	int trait, quirk;
	float roll, rate;

	if (kind <= 0 || kind >= MENDICANT_KINDS)
		kind = MENDICANT_KIND_NONE;
	if (!(strength >= 0.0f))
		strength = 0.0f;
	if (strength > 2.0f)
		strength = 2.0f;
	if (visibility < 0 || visibility >= MENDICANT_QUIRK_VISIBILITIES)
		visibility = MENDICANT_QUIRKS_NORMAL;

	traits->kind = (unsigned char)kind;
	traits->quirk = MENDICANT_QUIRK_NONE;
	for (trait = 0; trait < MENDICANT_TRAITS; trait++)
	{
		/* (the draws happen whatever the strength, so a soldier's quirk and
		constants do not change with it) */
		float offset = mendicant_random_triangle(&state) * 2.0f * mendicant_kind_spread[kind][trait];

		values[trait] = mendicant_kind_mean[kind][trait] + offset * strength;
	}

	/* the quirk: at most one; the scripted cowards and the sergeant have none */
	roll = mendicant_random_unit(&state);
	rate = mendicant_visibility_rate[visibility];
	if (group != MENDICANT_GROUP_NONE && rate > 0.0f)
	{
		float cumulative = 0.0f;

		for (quirk = 1; quirk < MENDICANT_QUIRKS; quirk++)
		{
			if (mendicant_quirk_definitions[quirk].group != group)
				continue;
			cumulative += mendicant_quirk_definitions[quirk].chance * rate;
			if (roll < cumulative)
			{
				traits->quirk = (unsigned char)quirk;
				break;
			}
		}
	}
	if (traits->quirk != MENDICANT_QUIRK_NONE)
	{
		float push = mendicant_visibility_push[visibility];

		for (trait = 0; trait < MENDICANT_TRAITS; trait++)
			values[trait] += mendicant_quirk_definitions[traits->quirk].push[trait] * push;
	}
	for (trait = 0; trait < MENDICANT_TRAITS; trait++)
		traits->trait[trait] = mendicant_trait_byte(kind == MENDICANT_KIND_NONE ? 0.5f : values[trait]);

	/* the constants */
	roll = mendicant_random_unit(&state);
	if (roll < 0.1f)
		traits->lead_bias = 1.1f + 0.15f * mendicant_random_unit(&state); /* an over-leader */
	else
		traits->lead_bias = 0.85f + 0.25f * mendicant_random_triangle(&state);
	traits->peek_side = mendicant_random_unit(&state) < 0.5f ? MENDICANT_SIDE_LEFT : MENDICANT_SIDE_RIGHT;
	traits->buddy_pref = mendicant_clamp_unit(0.5f + 0.5f * mendicant_random_triangle(&state));
	roll = mendicant_random_unit(&state);
	traits->favourite = MENDICANT_FAVOURITE_NONE;
	traits->seat = MENDICANT_SEAT_ANY;
	if (group == MENDICANT_GROUP_MARINE || kind == MENDICANT_KIND_SERGEANT)
	{
		/* about 1 Marine in 5 has a favourite weapon; 1 in 6 a seat he wants */
		if (roll < 0.1f)
			traits->favourite = MENDICANT_FAVOURITE_SHOTGUN;
		else if (roll < 0.2f)
			traits->favourite = MENDICANT_FAVOURITE_SNIPER;
		roll = mendicant_random_unit(&state);
		if (roll < 0.08f)
			traits->seat = MENDICANT_SEAT_DRIVER;
		else if (roll < 0.16f)
			traits->seat = MENDICANT_SEAT_GUNNER;
	}
	if (kind == MENDICANT_KIND_NONE)
	{
		traits->lead_bias = 1.0f;
		traits->buddy_pref = 0.5f;
	}

	return;
}

/* the fight's mood from its seed */
static int mendicant_mood_draw(
	unsigned long long seed)
{
	unsigned long long state = seed;
	float roll = mendicant_random_unit(&state), cumulative = 0.0f;
	int mood;

	for (mood = 0; mood < MENDICANT_MOODS; mood++)
	{
		cumulative += mendicant_mood_chance[mood];
		if (roll < cumulative)
			return mood;
	}
	return MENDICANT_MOOD_STEADY;
}

/* a mean-preserving multiplier: lo at trait 0, hi at trait 1, divided by its
value at the kind's population mean, so that its mean over the kind is 1
(it is linear in the trait). lo and hi above 0. */
static float mendicant_traits_mult(
	float value,
	float population_mean,
	float lo,
	float hi)
{
	float at_mean = lo + (hi - lo) * population_mean;
	float at_value = lo + (hi - lo) * value;

	if (!(at_mean > 0.0f) || !(at_value > 0.0f))
		return 1.0f;
	return at_value / at_mean;
}

/* the kind's population mean of every trait (with its quirks), from the same
draw: MENDICANT_POPULATION_SAMPLES soldiers of fixed seeds */
#define MENDICANT_POPULATION_SAMPLES 4096
static void mendicant_traits_population_means(
	int kind,
	float strength,
	int visibility,
	float *means)
{
	double sums[MENDICANT_TRAITS] = { 0 };
	struct mendicant_traits traits;
	int sample, trait;

	for (sample = 0; sample < MENDICANT_POPULATION_SAMPLES; sample++)
	{
		mendicant_traits_draw(&traits, kind, mendicant_mix64(0x5EED0000ULL + (unsigned long long)kind,
			(unsigned long long)sample), strength, visibility);
		for (trait = 0; trait < MENDICANT_TRAITS; trait++)
			sums[trait] += traits.trait[trait] / 255.0;
	}
	for (trait = 0; trait < MENDICANT_TRAITS; trait++)
		means[trait] = (float)(sums[trait] / MENDICANT_POPULATION_SAMPLES);

	return;
}

#endif
