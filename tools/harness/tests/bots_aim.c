/*
BOTS_AIM.C (test)

A bot's aim as a player's hand moves it (bots.c's bots_turn and the aim's
hand, bots_aim_*): the spring flicks past a target and comes back for the
less skilled, settles cleanly for a legendary bot, and follows a target
crossing the view without trailing it; the aim error settles to a wander
that never reaches zero; a change of the target's motion leaves the aim on
the old motion for the bot's reaction time and then grows the error; a hit
knocks the aim off; the bot's form wanders slowly within its bounds.
test_bots_aim.py takes the constants, structures and skill table
(config.inc) and the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

#define PIN(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
#define csmemset memset

/* (what struct bots_bot holds of the engine's: not read here) */
struct path_result
{
	boolean valid;
};

#include "config.inc"

#include "under_test.inc"

#define DEGREES(radians) ((radians) / BOTS_DEGREES)

static struct bots_bot bot;

static void reset(unsigned long seed)
{
	memset(&bot, 0, sizeof(bot));
	bot.random = seed;
	bot.form = 1.0f;
	bot.motion_change_tick = NONE;
}

/* a step of 30 degrees: the most the aim goes past the mark (a part of the
step) and the ticks until it stays within a twentieth of the step */
static void step(struct bots_skill_definition const *skill, real *overshoot, long *settle_ticks)
{
	real target = 30.0f * BOTS_DEGREES, most = 0.0f;
	long tick;

	reset(1);
	*settle_ticks = NONE;
	for (tick = 0; tick < 3 * TICKS_PER_SECOND; tick++)
	{
		bots_turn(&bot, target - bot.yaw, -bot.pitch, skill->turn_degrees_per_tick * BOTS_DEGREES, skill->aim_frequency,
			skill->aim_damping, 0.0f, 0.0f);
		most = MAX(most, bot.yaw - target);
		if (fabsf(bot.yaw - target) > target * 0.05f)
			*settle_ticks = NONE;
		else if (*settle_ticks == NONE)
			*settle_ticks = tick + 1;
	}
	*overshoot = most / target;
}

/* the easy bot flicks past and corrects; each skill goes past less than the
one below it; a legendary bot settles cleanly; all settle within a second */
static void case_spring(void)
{
	real overshoot[NUMBER_OF_BOTS_SKILLS];
	long settle[NUMBER_OF_BOTS_SKILLS];
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		step(&bots_skills[skill], &overshoot[skill], &settle[skill]);
		printf("%s: overshoot %.1f%%, settled in %ld ticks\n", bots_skills[skill].name, overshoot[skill] * 100.0f,
			settle[skill]);
		CHECK(settle[skill] != NONE && settle[skill] <= TICKS_PER_SECOND, "%s did not settle within a second",
			bots_skills[skill].name);
		if (skill > 0)
		{
			CHECK(overshoot[skill] < overshoot[skill - 1], "%s overshoots %.3f, no less than %s's %.3f",
				bots_skills[skill].name, overshoot[skill], bots_skills[skill - 1].name, overshoot[skill - 1]);
		}
	}
	CHECK(overshoot[0] >= 0.10f, "the easy bot's flick goes past by %.3f only", overshoot[0]);
	CHECK(overshoot[NUMBER_OF_BOTS_SKILLS - 1] <= 0.01f, "the legendary bot goes past by %.3f",
		overshoot[NUMBER_OF_BOTS_SKILLS - 1]);
}

/* a target crossing the view at a radian a second, its rate matched: once
on it, the aim stays on it (no trailing) */
static void case_tracks(void)
{
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		struct bots_skill_definition const *skill_definition = &bots_skills[skill];
		real target = 0.0f, rate = 1.0f;
		long tick;

		reset(1);
		for (tick = 0; tick < 3 * TICKS_PER_SECOND; tick++)
		{
			target += rate / TICKS_PER_SECOND;
			bots_turn(&bot, target - bot.yaw, 0.0f, skill_definition->turn_degrees_per_tick * BOTS_DEGREES,
				skill_definition->aim_frequency, skill_definition->aim_damping, rate, 0.0f);
		}
		/* (where the target is at the next tick, which the aim turned to) */
		target += rate / TICKS_PER_SECOND;
		CHECK(fabsf(DEGREES(target - bot.yaw)) < 0.5f, "%s trails a tracked target by %.2f degrees",
			skill_definition->name, DEGREES(target - bot.yaw));
	}
}

/* the error the bot takes a target with settles in its skill's time, but
its wander never does: never nearer the mark than 0.4 of the floor, and
across about the floor's size */
static void case_floor(void)
{
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		struct bots_skill_definition const *skill_definition = &bots_skills[skill];
		real floor = skill_definition->aim_floor_degrees;
		double sum = 0.0;
		long tick, ticks = 600 * TICKS_PER_SECOND;

		reset(0x1234 + skill);
		bot.wander_radius = 1.0f;
		bot.aim_error_yaw = 10.0f * BOTS_DEGREES;
		for (tick = 0; tick < 3 * (long)(skill_definition->aim_settle_seconds * TICKS_PER_SECOND); tick++)
			bots_aim_settle(&bot, skill_definition, 1.0f, 1.0f);
		CHECK(fabsf(DEGREES(bot.aim_error_yaw)) < 10.0f * 0.1f, "%s: the error did not settle (%.2f degrees)",
			skill_definition->name, DEGREES(bot.aim_error_yaw));
		for (tick = 0; tick < ticks; tick++)
		{
			real yaw, pitch;

			bots_aim_settle(&bot, skill_definition, 1.0f, 1.0f);
			yaw = DEGREES(bot.wander_yaw);
			pitch = DEGREES(bot.wander_pitch) / 0.6f;
			CHECK(sqrt(yaw * yaw + pitch * pitch) >= floor * 0.4f - 0.01f,
				"%s: the aim %.3f degrees off the mark, nearer than 0.4 of the floor %.2f", skill_definition->name,
				sqrt(yaw * yaw + pitch * pitch), floor);
			sum += yaw * yaw;
		}
		printf("%s: spread across %.2f degrees (floor %.2f)\n", skill_definition->name, sqrt(sum / ticks), floor);
		CHECK(sqrt(sum / ticks) > floor * 0.4f && sqrt(sum / ticks) < floor * 1.2f,
			"%s: the spread %.2f degrees is not about the floor %.2f", skill_definition->name, sqrt(sum / ticks), floor);
	}
}

/* a target strafing one way turns about: until two fifths of the bot's reaction time the
aim stays on the old motion (the drift grows), then the drift is let go and
the error grows; a target keeping its motion moves nothing */
static void case_motion(void)
{
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		struct bots_skill_definition const *skill_definition = &bots_skills[skill];
		long reaction = (long)(skill_definition->reaction_seconds * 0.4f * TICKS_PER_SECOND);
		real_vector3d right = { 0.08f, 0.0f, 0.0f }, left = { -0.08f, 0.0f, 0.0f };
		long tick;

		reset(77 + skill);
		bots_aim_target_motion(&bot, skill_definition, &right, 100, TRUE);
		for (tick = 101; tick < 130; tick++)
			bots_aim_target_motion(&bot, skill_definition, &right, tick, FALSE);
		CHECK(bot.motion_drift.i == 0.0f && bot.aim_error_yaw == 0.0f, "%s: a steady target moved the aim",
			skill_definition->name);
		for (tick = 130; tick < 130 + reaction; tick++)
		{
			bots_aim_target_motion(&bot, skill_definition, &left, tick, FALSE);
			CHECK(fabsf(bot.motion_drift.i - 0.16f * (real)(tick - 129)) < 0.001f || fabsf(bot.motion_drift.i) >= 0.599f,
				"%s: the aim is off the old motion's by %.3f at %ld ticks", skill_definition->name, bot.motion_drift.i,
				tick - 129);
		}
		CHECK(bot.aim_error_yaw == 0.0f, "%s: the error grew before the bot reacted", skill_definition->name);
		bots_aim_target_motion(&bot, skill_definition, &left, 130 + reaction, FALSE);
		CHECK(bot.motion_drift.i == 0.0f, "%s: the drift kept once it reacted", skill_definition->name);
		CHECK(fabsf(DEGREES(bot.aim_error_yaw)) >= skill_definition->aim_error_degrees * 0.1f - 0.001f,
			"%s: the error grew by %.2f degrees only as it reacted", skill_definition->name, DEGREES(bot.aim_error_yaw));
		CHECK(bot.seen_velocity.i == left.i, "%s: the new motion not taken in", skill_definition->name);
	}
}

/* a hit knocks the aim off: the harder, the more; a shield popped more */
static void case_flinch(void)
{
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		struct bots_skill_definition const *skill_definition = &bots_skills[skill];
		real hard, popped, light;

		reset(5 + skill);
		bots_aim_flinch(&bot, skill_definition, 0.5f, FALSE);
		hard = fabsf(DEGREES(bot.aim_error_yaw)) + fabsf(DEGREES(bot.aim_error_pitch));
		reset(5 + skill);
		bots_aim_flinch(&bot, skill_definition, 0.5f, TRUE);
		popped = fabsf(DEGREES(bot.aim_error_yaw)) + fabsf(DEGREES(bot.aim_error_pitch));
		reset(5 + skill);
		bots_aim_flinch(&bot, skill_definition, 0.03f, FALSE);
		light = fabsf(DEGREES(bot.aim_error_yaw)) + fabsf(DEGREES(bot.aim_error_pitch));
		CHECK(fabsf(DEGREES(bot.aim_error_yaw)) > 0.0f && bot.aim_error_pitch > 0.0f, "%s: no flinch",
			skill_definition->name);
		CHECK(hard >= skill_definition->flinch_degrees * 0.8f, "%s: a hard hit's flinch %.2f degrees",
			skill_definition->name, hard);
		CHECK(popped > hard && light < hard, "%s: flinches %.2f (popped) %.2f %.2f (light)", skill_definition->name,
			popped, hard, light);
	}
}

/* the form wanders within its bounds, slowly, in good and bad stretches */
static void case_form(void)
{
	real low = 10.0f, high = 0.0f, last = 1.0f;
	long tick;

	reset(99);
	for (tick = 0; tick < 3600 * TICKS_PER_SECOND; tick++)
	{
		bots_aim_form(&bot, tick);
		CHECK(bot.form >= 0.6f && bot.form <= 1.6f, "the form %.2f out of its bounds", bot.form);
		CHECK(fabsf(bot.form - last) <= 0.13f, "the form jumped from %.2f to %.2f", last, bot.form);
		last = bot.form;
		low = MIN(low, bot.form);
		high = MAX(high, bot.form);
	}
	CHECK(high - low > 0.3f, "the form kept within %.2f to %.2f", low, high);
}

/* where each weapon aims: the sniper rifle and the pistol the head once the
shield is down, the body before, the rocket launcher and fuel rod gun the feet,
anything else the body; the body when the bot does not go for the head, or
on a target in a vehicle */
static void case_aim_point(void)
{
	CHECK(bots_aim_point_kind(_bots_family_sniper, 0.0f, FALSE, TRUE) == _bots_aim_head,
		"the sniper rifle not at the head of a target whose shield is down");
	CHECK(bots_aim_point_kind(_bots_family_sniper, 1.0f, FALSE, TRUE) == _bots_aim_body,
		"the sniper rifle at the head of a target whose shield is up");
	CHECK(bots_aim_point_kind(_bots_family_sniper, 0.0f, FALSE, FALSE) == _bots_aim_body,
		"the sniper rifle at the head when the bot does not go for it");
	CHECK(bots_aim_point_kind(_bots_family_pistol, 1.0f, FALSE, TRUE) == _bots_aim_body,
		"the pistol at the head of a target whose shield is up");
	CHECK(bots_aim_point_kind(_bots_family_pistol, 0.4f, FALSE, TRUE) == _bots_aim_body,
		"the pistol at the head of a target whose shield is half up");
	CHECK(bots_aim_point_kind(_bots_family_pistol, 0.0f, FALSE, TRUE) == _bots_aim_head,
		"the pistol not at the head of a target whose shield is down");
	CHECK(bots_aim_point_kind(_bots_family_explosive, 1.0f, FALSE, TRUE) == _bots_aim_feet, "rockets not at the feet");
	CHECK(bots_aim_point_kind(_bots_family_explosive, 0.0f, FALSE, FALSE) == _bots_aim_feet, "rockets not at the feet");
	CHECK(bots_aim_point_kind(_bots_family_assault_rifle, 0.0f, FALSE, TRUE) == _bots_aim_body,
		"the assault rifle not at the body");
	CHECK(bots_aim_point_kind(_bots_family_plasma, 0.0f, FALSE, TRUE) == _bots_aim_body, "plasma not at the body");
	CHECK(bots_aim_point_kind(_bots_family_other, 0.0f, FALSE, TRUE) == _bots_aim_body, "the others not at the body");
	CHECK(bots_aim_point_kind(_bots_family_sniper, 0.0f, TRUE, TRUE) == _bots_aim_body,
		"the head of a target in a vehicle");
}

/* how often each skill goes for the head: about its chance, the less
skilled less often, the legendary always; the choice held two seconds */
static void case_head_chance(void)
{
	real share[NUMBER_OF_BOTS_SKILLS];
	short skill;

	for (skill = 0; skill < NUMBER_OF_BOTS_SKILLS; skill++)
	{
		struct bots_skill_definition const *skill_definition = &bots_skills[skill];
		long tick, heads = 0, choices = 0;
		boolean held;

		reset(31 + skill);
		for (tick = 0; tick < 4000 * TICKS_PER_SECOND; tick += 2 * TICKS_PER_SECOND)
		{
			heads += bots_aim_head_wanted(&bot, skill_definition, tick, FALSE);
			choices++;
			held = bot.head_wanted;
			CHECK(bots_aim_head_wanted(&bot, skill_definition, tick + TICKS_PER_SECOND, FALSE) == held,
				"%s changed its mind within two seconds", skill_definition->name);
		}
		share[skill] = (real)heads / (real)choices;
		printf("%s: the head %.0f%% of the time (chance %.0f%%)\n", skill_definition->name, share[skill] * 100.0f,
			skill_definition->head_chance * 100.0f);
		CHECK(fabsf(share[skill] - skill_definition->head_chance) < 0.05f, "%s goes for the head %.2f of the time",
			skill_definition->name, share[skill]);
		if (skill > 0)
		{
			CHECK(share[skill] > share[skill - 1] || share[skill] == 1.0f, "%s no more often at the head than %s",
				skill_definition->name, bots_skills[skill - 1].name);
		}
	}
	CHECK(share[0] < 0.5f, "the easy bot at the head %.2f of the time", share[0]);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("spring") case_spring();
	else CASE("tracks") case_tracks();
	else CASE("floor") case_floor();
	else CASE("motion") case_motion();
	else CASE("flinch") case_flinch();
	else CASE("form") case_form();
	else CASE("aim-point") case_aim_point();
	else CASE("head-chance") case_head_chance();
	else
	{
		fprintf(stderr, "unknown case %s\n", case_name);
		return 2;
	}
	return 0;
}
