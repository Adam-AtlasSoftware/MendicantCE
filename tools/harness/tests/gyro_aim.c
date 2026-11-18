#include "harness.h"
#include <math.h>

/* Gyro aiming and the flick stick (port/linux/src/gyro_aim.c), fed samples as a controller's sensors give them
(SDL's axes: x right, y up, z toward the player; radians a second): the automatic and the asked-for calibration,
player space's yaw from a tilted hold, the smoothing's small motions only, and the flick stick's flick and turn. */

#include "gyro_aim.h"
#include "under_test.inc"

#define RATE 500
#define STEP (1.0f / RATE)
#define DEGREES (3.14159265f / 180.0f)
#define TEST_PI 3.14159265f

static struct gyro_aim_settings settings(int player_space, int smoothing)
{
	struct gyro_aim_settings result;

	result.sensitivity = 2.0f;
	result.vertical_sensitivity = 1.0f;
	result.player_space = player_space;
	result.smoothing = smoothing;
	return result;
}

/* a small steady noise, as a real sensor's at rest (a few hundredths of a degree a second) */
static float noise(int sample, int axis)
{
	return 0.0004f * sinf(sample * 1.7f + axis * 2.1f);
}

/* the controller held with its up along up (a unit vector in its axes), turning at rate (its axes) plus bias for
seconds: the turn made (yaw, pitch) */
static void play(struct gyro_aim *aim, const struct gyro_aim_settings *s, const float up[3], const float rate[3],
	const float bias[3], float seconds, float turned[2], float *largest_step_error, float expected_yaw_rate)
{
	static int sample;
	int count = (int)(seconds * RATE + 0.5f), index, axis;

	turned[0] = turned[1] = 0.0f;
	for (index = 0; index < count; index++, sample++)
	{
		float accelerometer[3], gyro[3], turn[2];

		for (axis = 0; axis < 3; axis++)
		{
			accelerometer[axis] = up[axis] * 9.80665f;
			gyro[axis] = rate[axis] + bias[axis] + noise(sample, axis);
		}
		gyro_aim_accelerometer(aim, accelerometer);
		gyro_aim_sample(aim, gyro, STEP, s, turn);
		turned[0] += turn[0];
		turned[1] += turn[1];
		if (largest_step_error)
		{
			float error = fabsf(turn[0] - expected_yaw_rate * STEP * s->sensitivity);

			if (error > *largest_step_error)
				*largest_step_error = error;
		}
	}
}

static const float level[3] = { 0.0f, 1.0f, 0.0f };
static const float still[3] = { 0.0f, 0.0f, 0.0f };

/* a constant bias at rest drifts the view only until the controller has been still a moment: then it is measured
and taken off, and a real turn turns as far as it should */
static void case_bias(void)
{
	struct gyro_aim aim;
	struct gyro_aim_settings s = settings(1, 1);
	const float bias[3] = { 0.02f, -0.03f, 0.015f };
	const float turning[3] = { 0.0f, 90.0f * DEGREES, 0.0f };
	float turned[2];

	gyro_aim_reset(&aim);
	play(&aim, &s, level, still, bias, 3.0f, turned, NULL, 0.0f);
	CHECK(aim.bias_known, "no bias measured after 3 seconds still");
	play(&aim, &s, level, still, bias, 2.0f, turned, NULL, 0.0f);
	CHECK(fabsf(turned[0]) < 0.05f * DEGREES && fabsf(turned[1]) < 0.05f * DEGREES,
		"still, the view drifted %f, %f degrees in 2 seconds (the bias alone: %f, %f)", turned[0] / DEGREES,
		turned[1] / DEGREES, bias[1] * 2.0f * s.sensitivity / DEGREES, bias[0] * 2.0f * s.sensitivity / DEGREES);
	play(&aim, &s, level, turning, bias, 0.5f, turned, NULL, 0.0f);
	CHECK(fabsf(turned[0] - 45.0f * DEGREES * s.sensitivity) < 0.2f * DEGREES,
		"a 45 degree turn turned the view %f degrees, of %f", turned[0] / DEGREES, 45.0f * s.sensitivity);
}

/* asked for, the calibration measures the bias over its seconds, turning nothing meanwhile */
static void case_calibrate(void)
{
	struct gyro_aim aim;
	struct gyro_aim_settings s = settings(1, 0);
	const float bias[3] = { -0.05f, 0.04f, 0.0f };
	float turned[2];

	gyro_aim_reset(&aim);
	/* (moving: the automatic calibration never takes it) */
	{
		const float wobble[3] = { 0.0f, 0.3f, 0.0f };

		play(&aim, &s, level, wobble, bias, 0.5f, turned, NULL, 0.0f);
	}
	gyro_aim_calibrate(&aim);
	{
		int samples;

		for (samples = 0; aim.calibrating && samples < 2 * RATE; samples++)
		{
			play(&aim, &s, level, still, bias, STEP, turned, NULL, 0.0f);
			CHECK(turned[0] == 0.0f && turned[1] == 0.0f, "the view turned %g, %g while calibrating", turned[0],
				turned[1]);
		}
		CHECK(samples * STEP >= GYRO_AIM_CALIBRATION_SECONDS - 2 * STEP, "the calibration took %d samples", samples);
	}
	CHECK(!aim.calibrating && aim.bias_known, "the calibration did not end");
	CHECK(fabsf(aim.bias[0] - bias[0]) < 0.001f && fabsf(aim.bias[1] - bias[1]) < 0.001f,
		"measured %f, %f of %f, %f", aim.bias[0], aim.bias[1], bias[0], bias[1]);
}

/* turning about the vertical, the controller held at any tilt (pitched toward the player, upright), turns the view
side to side as far in player space; local space takes only the controller's own vertical axis */
static void case_player_space(void)
{
	static const float tilts[] = { 0.0f, 30.0f, 60.0f, 80.0f, 90.0f };
	const float rate = 60.0f * DEGREES;
	unsigned int index;

	for (index = 0; index < sizeof(tilts) / sizeof(tilts[0]); index++)
	{
		struct gyro_aim aim;
		struct gyro_aim_settings player = settings(1, 0), local = settings(0, 0);
		float tilt = tilts[index] * DEGREES;
		float up[3], turning[3], turned[2];
		int axis;

		/* (pitched back by tilt about x: its up leans toward z) */
		up[0] = 0.0f;
		up[1] = cosf(tilt);
		up[2] = sinf(tilt);
		/* (a turn about the world's vertical, seen in the controller's axes) */
		for (axis = 0; axis < 3; axis++)
			turning[axis] = up[axis] * rate;
		gyro_aim_reset(&aim);
		play(&aim, &player, up, still, still, 0.2f, turned, NULL, 0.0f);
		play(&aim, &player, up, turning, still, 1.0f, turned, NULL, 0.0f);
		CHECK(fabsf(turned[0] - rate * player.sensitivity) < 0.01f * rate * player.sensitivity &&
			fabsf(turned[1]) < 0.01f * rate,
			"held %g degrees back, a turn of 60 degrees about the vertical turned the view %f, %f degrees, of %f",
			tilts[index], turned[0] / DEGREES, turned[1] / DEGREES, 60.0f * player.sensitivity);
		gyro_aim_reset(&aim);
		play(&aim, &local, up, still, still, 0.2f, turned, NULL, 0.0f);
		play(&aim, &local, up, turning, still, 1.0f, turned, NULL, 0.0f);
		CHECK(fabsf(turned[0] - rate * cosf(tilt) * local.sensitivity) < 0.01f * rate * local.sensitivity,
			"held %g degrees back, local space turned %f degrees, of %f", tilts[index], turned[0] / DEGREES,
			60.0f * cosf(tilt) * local.sensitivity);
	}
}

/* tilted while shaken (the accelerometer's reading no use: a shake to the side, far from gravity's), the up direction follows the
gyroscope's own turn, and a turn about the vertical right after still turns the view side to side as far */
static void case_tilt_followed(void)
{
	struct gyro_aim aim;
	struct gyro_aim_settings s = settings(1, 0);
	const float rate = 60.0f * DEGREES, tilt_rate = 400.0f * DEGREES;
	float turned[2], up[3];
	int index;

	gyro_aim_reset(&aim);
	play(&aim, &s, level, still, still, 0.2f, turned, NULL, 0.0f);
	for (index = 0; index < (int)(0.2f * RATE); index++)
	{
		/* (its top tipped away about x, 80 degrees in all, a clockwise turn seen from its right: the world's up,
		seen from the controller, leans toward z) */
		float angle = tilt_rate * STEP * (index + 1);
		float shaken[3] = { 3.0f * 9.80665f, cosf(angle) * 9.80665f, sinf(angle) * 9.80665f };
		float gyro[3] = { -tilt_rate, 0.0f, 0.0f }, turn[2];

		gyro_aim_accelerometer(&aim, shaken);
		gyro_aim_sample(&aim, gyro, STEP, &s, turn);
	}
	up[0] = 0.0f;
	up[1] = cosf(80.0f * DEGREES);
	up[2] = sinf(80.0f * DEGREES);
	turned[0] = 0.0f;
	for (index = 0; index < (int)(0.5f * RATE); index++)
	{
		float shaken[3] = { 3.0f * 9.80665f, up[1] * 9.80665f, up[2] * 9.80665f };
		float gyro[3] = { 0.0f, up[1] * rate, up[2] * rate }, turn[2];

		gyro_aim_accelerometer(&aim, shaken);
		gyro_aim_sample(&aim, gyro, STEP, &s, turn);
		turned[0] += turn[0];
	}
	CHECK(fabsf(turned[0] - 0.5f * rate * s.sensitivity) < 0.02f * 0.5f * rate * s.sensitivity,
		"tilted 80 degrees while shaken, a 30 degree turn about the vertical turned the view %f degrees, of %f",
		turned[0] / DEGREES, 30.0f * s.sensitivity);
}

/* the smoothing takes the smallest motions only: a real turn turns the view sample for sample as it comes, with no
lag; a tremor is spread out, and none of it lost */
static void case_smoothing(void)
{
	struct gyro_aim aim;
	struct gyro_aim_settings s = settings(0, 1);
	float turned[2], largest = 0.0f;
	const float fast[3] = { 0.0f, 120.0f * DEGREES, 0.0f };
	int index;

	gyro_aim_reset(&aim);
	aim.bias_known = 1;
	play(&aim, &s, level, still, still, 0.2f, turned, NULL, 0.0f);
	play(&aim, &s, level, fast, still, 0.25f, turned, &largest, fast[1]);
	CHECK(largest < 0.002f * fast[1] * STEP * s.sensitivity + 1e-7f,
		"a 120 degree a second turn lagged: a sample's turn was %g radians off", largest);
	{
		/* a tremor of 1 degree a second, by turns: steadied (each sample's turn less than it came), and all of it
		turned once it settles */
		float sum = 0.0f, came = 0.0f, first = 0.0f;

		gyro_aim_reset(&aim);
		aim.bias_known = 1;
		aim.accelerometer_known = 1;
		aim.accelerometer[1] = 9.80665f;
		/* (the automatic calibration kept out of it, which would take the tremor's mean for a bias: the smoothing
		alone) */
		aim.still_seconds = -1e9f;
		for (index = 0; index < RATE; index++)
		{
			float gyro[3] = { 0.0f, (index / 25) % 2 ? 1.0f * DEGREES : -0.5f * DEGREES, 0.0f };
			float turn[2];

			gyro_aim_sample(&aim, gyro, STEP, &s, turn);
			if (index == 0)
				first = turn[0];
			sum += turn[0];
			came += gyro[1] * STEP * s.sensitivity;
		}
		for (index = 0; index < RATE; index++)
		{
			float gyro[3] = { 0.0f, 0.0f, 0.0f };
			float turn[2];

			gyro_aim_sample(&aim, gyro, STEP, &s, turn);
			sum += turn[0];
		}
		CHECK(fabsf(first) < 0.5f * 0.5f * DEGREES * STEP * s.sensitivity, "a tremor was not smoothed: %g", first);
		CHECK(fabsf(sum - came) < 1e-3f * fabsf(came) + 1e-7f, "the tremor turned %g of %g", sum, came);
	}
}

static float flick_play(struct flick_stick *flick, float x, float y, float seconds)
{
	float turned = 0.0f;
	int index, count = (int)(seconds * 60.0f + 0.5f);

	for (index = 0; index < count; index++)
		turned += flick_stick_update(flick, x, y, 1.0f / 60.0f, FLICK_STICK_SECONDS);
	return turned;
}

/* pushed to the rim the flick stick turns the player to where it points, in its tenth of a second; turned round
the rim the player turns with it; let go, nothing more; a stick wavering at the rim does not flick again */
static void case_flick(void)
{
	struct flick_stick flick;
	float turned, first;
	int index;

	flick_stick_reset(&flick);
	CHECK(flick_play(&flick, 0.5f, 0.0f, 0.5f) == 0.0f, "half way, the stick flicked");
	first = flick_stick_update(&flick, 1.0f, 0.0f, 1.0f / 60.0f, FLICK_STICK_SECONDS);
	turned = first + flick_play(&flick, 1.0f, 0.0f, FLICK_STICK_SECONDS);
	CHECK(first < 0.0f && first > -TEST_PI / 2.0f, "the flick's first frame turned %f", first);
	CHECK(fabsf(turned + TEST_PI / 2.0f) < 1e-4f, "pushed right, the player turned %f degrees, of -90",
		turned / DEGREES);
	CHECK(flick_play(&flick, 1.0f, 0.0f, 0.5f) == 0.0f, "held still at the rim, it turned on");
	/* (round the rim, a quarter, from the right to the top: counter-clockwise, a turn to the left) */
	turned = 0.0f;
	for (index = 1; index <= 30; index++)
	{
		float angle = (TEST_PI / 2.0f) * (1.0f - index / 30.0f);

		turned += flick_stick_update(&flick, sinf(angle), cosf(angle), 1.0f / 60.0f, FLICK_STICK_SECONDS);
	}
	turned += flick_play(&flick, 0.0f, 1.0f, 0.2f);
	CHECK(fabsf(turned - TEST_PI / 2.0f) < 1e-3f, "turning round the rim a quarter, the player turned %f degrees, of 90",
		turned / DEGREES);
	CHECK(flick_play(&flick, 0.0f, 0.0f, 0.3f) == 0.0f, "let go, it turned");
	/* about face, the stick pulled down */
	turned = flick_play(&flick, 0.0f, -1.0f, 0.3f);
	CHECK(fabsf(fabsf(turned) - TEST_PI) < 1e-3f, "pulled down, the player turned %f degrees, of 180",
		turned / DEGREES);
	flick_play(&flick, 0.0f, 0.0f, 0.3f);
	/* wavering at the rim, 0.75 to 0.95 of the way, pointing up-left: one flick, of 45 degrees left */
	turned = 0.0f;
	for (index = 0; index < 60; index++)
	{
		float magnitude = index % 2 ? 0.95f : 0.75f;

		turned += flick_stick_update(&flick, -magnitude * 0.70710678f, magnitude * 0.70710678f, 1.0f / 60.0f,
			FLICK_STICK_SECONDS);
	}
	CHECK(fabsf(turned - TEST_PI / 4.0f) < 1e-3f, "wavering at the rim, the player turned %f degrees, of 45",
		turned / DEGREES);
}

/* the flick's time (input.flick_stick_time): pushed right at 120 frames a second, the flick turns a quarter turn
whatever its time, done once its time has passed and not before; at once with none; past the most, the most */
static void case_flick_time(void)
{
	static const float times[] = { 0.0f, 0.05f, 0.1f, 0.25f, 1.0f };
	const float frame = 1.0f / 120.0f;
	unsigned int index;

	for (index = 0; index < sizeof(times) / sizeof(times[0]); index++)
	{
		struct flick_stick flick;
		float time = fminf(times[index], FLICK_STICK_SECONDS_MOST);
		float turned = 0.0f, first;
		int frames = (int)(time / frame + 0.5f), at;

		flick_stick_reset(&flick);
		first = flick_stick_update(&flick, 1.0f, 0.0f, frame, times[index]);
		turned = first;
		for (at = 1; at < frames - 1; at++)
			turned += flick_stick_update(&flick, 1.0f, 0.0f, frame, times[index]);
		if (frames > 1)
		{
			/* (a frame before its time: not all of it yet) */
			CHECK(fabsf(turned) < TEST_PI / 2.0f - 1e-5f, "a %g s flick turned %f degrees a frame before its time",
				times[index], turned / DEGREES);
			turned += flick_stick_update(&flick, 1.0f, 0.0f, frame, times[index]);
		}
		CHECK(fabsf(turned + TEST_PI / 2.0f) < 1e-4f, "a %g s flick turned %f degrees, of -90", times[index],
			turned / DEGREES);
		CHECK(flick_stick_update(&flick, 1.0f, 0.0f, frame, times[index]) == 0.0f, "a %g s flick turned on",
			times[index]);
		/* (its first frame's share: the ease's over a frame of its time) */
		CHECK(time == 0.0f ? first == -TEST_PI / 2.0f :
			fabsf(first + TEST_PI / 2.0f * (1.0f - (1.0f - frame / time) * (1.0f - frame / time))) < 1e-5f,
			"a %g s flick's first frame turned %f degrees", times[index], first / DEGREES);
	}
}

/* when the turn counts: each mode, its button and the right stick's touch; which controllers say when their right
stick is touched (the Steam Deck's own controls and the Steam Controller, by SDL's drivers), which is the Deck */
static void case_modes(void)
{
	int held, touched;

	for (held = 0; held < 2; held++)
	{
		for (touched = -1; touched < 2; touched++)
		{
			CHECK(!gyro_aim_active(GYRO_AIM_OFF, held, touched), "off, it turned (held %d, touched %d)", held, touched);
			CHECK(gyro_aim_active(GYRO_AIM_ON, held, touched), "always on, it did not turn");
			CHECK(gyro_aim_active(GYRO_AIM_HOLD, held, touched) == held, "hold to aim: %d with the button %d",
				gyro_aim_active(GYRO_AIM_HOLD, held, touched), held);
			CHECK(gyro_aim_active(GYRO_AIM_PAUSE, held, touched) == !held, "hold to pause: %d with the button %d",
				gyro_aim_active(GYRO_AIM_PAUSE, held, touched), held);
			CHECK(gyro_aim_active(GYRO_AIM_TOUCH, held, touched) == (touched > 0),
				"thumb on stick: %d, touched %d (the button %d)", gyro_aim_active(GYRO_AIM_TOUCH, held, touched),
				touched, held);
		}
	}
	CHECK(gyro_stick_touch_known(0x28de, 0x1205), "the Steam Deck's controls: no stick touch");
	CHECK(gyro_stick_touch_known(0x28de, 0x1302) && gyro_stick_touch_known(0x28de, 0x1305),
		"the Steam Controller: no stick touch");
	CHECK(!gyro_stick_touch_known(0x28de, 0x11ff), "Steam Input's virtual controller: a stick touch");
	CHECK(!gyro_stick_touch_known(0x28de, 0x1102) && !gyro_stick_touch_known(0x054c, 0x0ce6) &&
		!gyro_stick_touch_known(0x045e, 0x0b12), "the 2015 Steam Controller, a DualSense, an Xbox pad: a stick touch");
	CHECK(gyro_steam_deck(0x28de, 0x1205) && !gyro_steam_deck(0x28de, 0x1302) && !gyro_steam_deck(0x28de, 0x11ff),
		"the Steam Deck's controls told from the others wrong");
}

/* the touch controls' gyroscope (a phone's, its turn already made between reads: gyro_aim_turned, touch_input.c):
at the phone's defaults (1, 1X, no smoothing) the view turns as far as the phone, read by read, as it did before
it came through here, whatever the reads' times (none, a frame's, a gap); its sensitivities scale it as a
controller's do, through the same code: a phone and a controller turned alike turn the view alike */
static void case_touch(void)
{
	static const float reads[][3] = {
		/* yaw, pitch (radians), seconds since the last read */
		{ 0.004f, -0.002f, 1.0f / 60.0f }, { 0.0f, 0.0f, 1.0f / 60.0f }, { -0.03f, 0.011f, 1.0f / 30.0f },
		{ 0.0005f, 0.0f, 0.0f }, { 0.2f, -0.1f, 0.4f }, { 1e-5f, 2e-5f, 1.0f / 144.0f },
	};
	struct gyro_aim_settings phone = { 1.0f, 1.0f, 0, 0 }, scaled = { 2.0f, 0.5f, 0, 0 };
	float smoothed[2] = { 0.0f, 0.0f }, scaled_smoothed[2] = { 0.0f, 0.0f };
	unsigned int index;

	for (index = 0; index < sizeof(reads) / sizeof(reads[0]); index++)
	{
		float turn[2];

		gyro_aim_turned(reads[index], smoothed, reads[index][2], &phone, turn);
		CHECK(fabsf(turn[0] - reads[index][0]) <= 1e-6f * fabsf(reads[index][0]) &&
			fabsf(turn[1] - reads[index][1]) <= 1e-6f * fabsf(reads[index][1]),
			"read %u: the phone's defaults turned %g, %g of %g, %g", index, turn[0], turn[1], reads[index][0],
			reads[index][1]);
		gyro_aim_turned(reads[index], scaled_smoothed, reads[index][2], &scaled, turn);
		CHECK(fabsf(turn[0] - 2.0f * reads[index][0]) <= 1e-6f * fabsf(reads[index][0]) &&
			fabsf(turn[1] - reads[index][1]) <= 1e-6f * fabsf(reads[index][1]),
			"read %u: sensitivity 2 and vertical 0.5X turned %g, %g of %g, %g", index, turn[0], turn[1],
			2.0f * reads[index][0], reads[index][1]);
	}
	{
		/* a controller held flat in local space turned as a phone is, 500 samples a second, and the phone read
		at 60 a second: the same turn, smoothing and all */
		struct gyro_aim aim;
		struct gyro_aim_settings controller = settings(0, 1), touch = settings(0, 1);
		float controller_turned[2] = { 0.0f, 0.0f }, touch_turned[2] = { 0.0f, 0.0f };
		float rate[2] = { 30.0f * DEGREES, -10.0f * DEGREES };

		touch.vertical_sensitivity = controller.vertical_sensitivity = 0.5f;
		gyro_aim_reset(&aim);
		aim.bias_known = 1;
		aim.still_seconds = -1e9f;
		for (index = 0; index < RATE / 2; index++)
		{
			float gyro[3] = { rate[1], rate[0], 0.0f }, turn[2];

			gyro_aim_sample(&aim, gyro, STEP, &controller, turn);
			controller_turned[0] += turn[0];
			controller_turned[1] += turn[1];
		}
		smoothed[0] = smoothed[1] = 0.0f;
		for (index = 0; index < 30; index++)
		{
			float turned[2] = { rate[0] / 60.0f, rate[1] / 60.0f }, turn[2];

			gyro_aim_turned(turned, smoothed, 1.0f / 60.0f, &touch, turn);
			touch_turned[0] += turn[0];
			touch_turned[1] += turn[1];
		}
		CHECK(fabsf(touch_turned[0] - controller_turned[0]) < 1e-3f * fabsf(controller_turned[0]) &&
			fabsf(touch_turned[1] - controller_turned[1]) < 1e-3f * fabsf(controller_turned[1]),
			"the phone turned %g, %g and the controller %g, %g", touch_turned[0], touch_turned[1],
			controller_turned[0], controller_turned[1]);
	}
}

/* the first sample's turn of a steady rate (yaw, radians a second) from rest, at a smoothing strength, as a share
of the turn it came with: 1 direct, less smoothed */
static float first_share(float smoothing, float rate)
{
	struct gyro_aim_settings s = settings(0, 0);
	float smoothed[2] = { 0.0f, 0.0f }, look[2] = { rate, 0.0f }, turn[2];

	s.smoothing = smoothing;
	gyro_aim_rate(look, smoothed, STEP, &s, turn);
	return turn[0] / (rate * STEP * s.sensitivity);
}

/* the smoothing's strengths: medium is the smoothing as it was (on), sample for sample; low and high scale its
thresholds (a motion medium smooths, just slower than its slowest, low turns direct; one medium turns direct,
high smooths); off smooths nothing; the words name them */
static void case_strength(void)
{
	struct gyro_aim_settings medium = settings(0, 1);
	float smoothed[2] = { 0.0f, 0.0f }, was[2] = { 0.0f, 0.0f };
	int index;

	CHECK(gyro_aim_smoothing("off", -1.0f) == 0.0f && gyro_aim_smoothing("low", -1.0f) == 0.5f &&
		gyro_aim_smoothing("medium", -1.0f) == 1.0f && gyro_aim_smoothing("high", -1.0f) == 2.0f &&
		gyro_aim_smoothing("on", -1.0f) == -1.0f && gyro_aim_smoothing(NULL, -1.0f) == -1.0f,
		"the strengths' words read wrong");
	medium.smoothing = gyro_aim_smoothing("medium", 0.0f);
	for (index = 0; index < RATE; index++)
	{
		/* (a tremor, then a turn, by turns) */
		float rate = (index / 40) % 2 ? 0.3f * sinf(index * 0.05f) : 0.02f * sinf(index * 0.7f);
		float look[2] = { rate, -0.5f * rate }, old[2] = { rate, -0.5f * rate }, turn[2];

		gyro_aim_rate(look, smoothed, STEP, &medium, turn);
		/* (the smoothing on, as it was before its strengths) */
		smooth_tiered(old, was, 2, SMOOTH_SLOWEST, SMOOTH_FASTEST, SMOOTH_SECONDS, STEP);
		CHECK(turn[0] == old[0] * STEP * medium.sensitivity &&
			turn[1] == old[1] * STEP * medium.sensitivity * medium.vertical_sensitivity,
			"sample %d: medium turned %g, %g, and the smoothing on %g, %g", index, turn[0], turn[1],
			old[0] * STEP * medium.sensitivity, old[1] * STEP * medium.sensitivity);
	}
	CHECK(first_share(1.0f, 0.995f * SMOOTH_SLOWEST) < 0.5f && first_share(0.5f, 0.995f * SMOOTH_SLOWEST) > 0.999f,
		"a slow motion: medium turned %g of it at once, low %g", first_share(1.0f, 0.995f * SMOOTH_SLOWEST),
		first_share(0.5f, 0.995f * SMOOTH_SLOWEST));
	CHECK(first_share(1.0f, 1.5f * SMOOTH_FASTEST) > 0.999f && first_share(2.0f, 1.5f * SMOOTH_FASTEST) < 0.9f,
		"a faster motion: medium turned %g of it at once, high %g", first_share(1.0f, 1.5f * SMOOTH_FASTEST),
		first_share(2.0f, 1.5f * SMOOTH_FASTEST));
	CHECK(first_share(0.0f, 0.1f * SMOOTH_SLOWEST) > 0.999f, "off, a tremor was smoothed");
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "bias"))
		case_bias();
	else if (!strcmp(name, "calibrate"))
		case_calibrate();
	else if (!strcmp(name, "player_space"))
		case_player_space();
	else if (!strcmp(name, "tilt_followed"))
		case_tilt_followed();
	else if (!strcmp(name, "smoothing"))
		case_smoothing();
	else if (!strcmp(name, "flick"))
		case_flick();
	else if (!strcmp(name, "modes"))
		case_modes();
	else if (!strcmp(name, "touch"))
		case_touch();
	else if (!strcmp(name, "strength"))
		case_strength();
	else if (!strcmp(name, "flick_time"))
		case_flick_time();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
