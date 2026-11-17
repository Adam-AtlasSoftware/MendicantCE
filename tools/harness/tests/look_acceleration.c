#include "harness.h"
#include <math.h>

/* A controller's look acceleration (source/game/player_control.c's player_control_look_acceleration): the globals'
ramp, from 1 up to look_acceleration_scale over look_acceleration_time, times the player's strength
(input.look_acceleration_strength) and over their time (input.look_acceleration_time), as
player_control_look_acceleration_tuning reads them. The defaults, 1 and 1, must give the original's ramp to the bit on
any map's globals; strength 0 never accelerates (player_control_stick_look); 2 doubles the extra speed; time 0.5
reaches it in half the time. */

typedef struct { real yaw, pitch; } real_euler_angles2d;
#define PIN(x, low, high) ((x) < (low) ? (low) : (x) > (high) ? (high) : (x))
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4

enum { _button_scope_zoom, NUMBER_OF_BUTTONS };
struct game_globals_player_control
{
	struct { long count; real *address; } look_function;
	real look_pegging_threshold;
	real look_acceleration_time;
	real look_acceleration_scale;
};
struct player_control
{
	real look_acceleration_time;
};
struct game_input_state
{
	boolean buttons[NUMBER_OF_BUTTONS];
	real yaw, pitch;
};

static boolean controls_enable_doubled_spin = TRUE;
static boolean controls_swap_doubled_spin_state = FALSE;

/* the fake world: the player's two settings, as config.toml gives them, and the settings' change count */
static double world_strength = 1.0;
static double world_time = 1.0;
static unsigned long world_changes;

static unsigned long config_changes(void) { return world_changes; }
static double config_player_real(int player, const char *name)
{
	(void)player;
	if (!strcmp(name, "input.look_acceleration_strength"))
		return world_strength;
	if (!strcmp(name, "input.look_acceleration_time"))
		return world_time;
	fprintf(stderr, "no setting %s\n", name);
	exit(2);
}
static float halo_look_vertical_sensitivity(short gamepad) { (void)gamepad; return 1.0f; }
static int player_control_aim_modern(short gamepad, float *tangent, float *threshold)
{
	(void)gamepad;
	*tangent = 0.0f;
	*threshold = -1.0f;
	return FALSE;
}
struct player_control_aim;
static boolean player_control_stick_aim(struct player_control_aim const *aim, long count, real const *function,
	real pegging, real yaw, real pitch, real *curved_yaw, real *curved_pitch, real *clamped_yaw, real *clamped_pitch);

#include "under_test.inc"

/* (the stick as Classic's at its edge: accelerating past the pegging threshold) */
static boolean player_control_stick_aim(struct player_control_aim const *aim, long count, real const *function,
	real pegging, real yaw, real pitch, real *curved_yaw, real *curved_pitch, real *clamped_yaw, real *clamped_pitch)
{
	(void)aim, (void)count, (void)function;
	*curved_yaw = *clamped_yaw = yaw;
	*curved_pitch = *clamped_pitch = pitch;
	return fabsf(yaw) >= pegging;
}

/* the original's ramp, as player_control.c had it before the settings */
static real original_ramp(struct game_globals_player_control const *constants, struct player_control const *control)
{
	real acceleration = PIN(control->look_acceleration_time / constants->look_acceleration_time, 0.f, 1.f);

	return (constants->look_acceleration_scale - 1.f) * acceleration + 1.f;
}

static void settings(double strength, double time)
{
	world_strength = strength;
	world_time = time;
	world_changes++;
}

static real ramp(real scale, real time, real held)
{
	struct game_globals_player_control constants = { { 0, NULL }, 0.85f, time, scale };
	struct player_control control = { held };

	return player_control_look_acceleration(&constants, &control, 0);
}

static boolean stick_accelerates(real push)
{
	static real curve[] = { 0.0f, 1.0f };
	struct game_globals_player_control constants = { { 2, curve }, 0.85f, 1.0f, 3.0f };
	struct game_input_state input = { { FALSE }, push, 0.0f };
	real_euler_angles2d delta;
	real clamped_yaw, clamped_pitch;

	return player_control_stick_look(&constants, 0, &input, 0.07f, 0.035f, &delta, &clamped_yaw, &clamped_pitch);
}

/* every stock map's globals, and others */
static const real globals[][2] = { { 3.0f, 1.0f }, { 2.5f, 0.6f }, { 1.8f, 1.7f }, { 4.0f, 0.35f } };
#define GLOBALS (sizeof(globals) / sizeof(globals[0]))

static int close_to(real a, real b) { return fabsf(a - b) < 1e-5f; }

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	unsigned index;

	CASE("default_is_the_original")
	{
		/* 1 and 1 (and a value out of range, read as the map's own): the original's ramp to the bit, held through
		frames of 30, 60 and 144 a second, on each globals */
		static const double defaults[][2] = { { 1.0, 1.0 }, { 5.0, 0.0 }, { -1.0, 3.0 } };
		static const real steps[] = { 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f };
		unsigned setting, step;

		for (setting = 0; setting < sizeof(defaults) / sizeof(defaults[0]); setting++)
		{
			settings(defaults[setting][0], defaults[setting][1]);
			for (index = 0; index < GLOBALS; index++)
			{
				for (step = 0; step < sizeof(steps) / sizeof(steps[0]); step++)
				{
					struct game_globals_player_control constants = { { 0, NULL }, 0.85f, globals[index][1],
						globals[index][0] };
					struct player_control control = { 0.0f };

					while (control.look_acceleration_time < 2.5f)
					{
						real ours = player_control_look_acceleration(&constants, &control, 0);
						real theirs = original_ramp(&constants, &control);

						CHECK(!memcmp(&ours, &theirs, sizeof(ours)), "%gX over %g s, held %.9g s: %.9g, not the "
							"original's %.9g", globals[index][0], globals[index][1], control.look_acceleration_time,
							ours, theirs);
						control.look_acceleration_time += steps[step];
					}
				}
			}
		}
		CHECK(stick_accelerates(1.0f), "a full push no longer accelerates at the default");
		return 0;
	}
	CASE("strength_zero_no_ramp")
	{
		settings(0.0, 1.0);
		for (index = 0; index < GLOBALS; index++)
		{
			real held;

			for (held = 0.0f; held < 2.5f; held += 0.05f)
			{
				real turn = ramp(globals[index][0], globals[index][1], held);

				CHECK(turn == 1.0f, "Off: %gX over %g s held %g s turned %.9g times", globals[index][0],
					globals[index][1], held, turn);
			}
		}
		/* and the stick never counts as accelerating, so its clock stays at 0 */
		CHECK(!stick_accelerates(1.0f), "Off: a full push still accelerates");
		settings(1.0, 1.0);
		CHECK(stick_accelerates(1.0f), "1X: a full push does not accelerate");
		return 0;
	}
	CASE("strength_two")
	{
		/* twice the extra speed: 3X becomes 5X, at every point of the ramp; its time unchanged */
		settings(2.0, 1.0);
		CHECK(close_to(ramp(3.0f, 1.0f, 1.0f), 5.0f), "2X of 3X over 1 s: %.9g at 1 s, not 5", ramp(3.0f, 1.0f, 1.0f));
		CHECK(close_to(ramp(3.0f, 1.0f, 0.5f), 3.0f), "2X of 3X over 1 s: %.9g at 0.5 s, not 3", ramp(3.0f, 1.0f, 0.5f));
		CHECK(ramp(3.0f, 1.0f, 0.0f) == 1.0f, "2X: %.9g at 0 s, not 1", ramp(3.0f, 1.0f, 0.0f));
		CHECK(close_to(ramp(2.5f, 0.6f, 0.6f), 4.0f), "2X of 2.5X over 0.6 s: %.9g, not 4", ramp(2.5f, 0.6f, 0.6f));
		settings(0.5, 1.0);
		CHECK(close_to(ramp(3.0f, 1.0f, 2.0f), 2.0f), "0.5X of 3X: %.9g, not 2", ramp(3.0f, 1.0f, 2.0f));
		return 0;
	}
	CASE("time_half")
	{
		/* half the time: the full speed at 0.5 s of 1 s, halfway at 0.25 s; the full speed itself unchanged */
		settings(1.0, 0.5);
		CHECK(close_to(ramp(3.0f, 1.0f, 0.5f), 3.0f), "0.5X of 1 s: %.9g at 0.5 s, not 3", ramp(3.0f, 1.0f, 0.5f));
		CHECK(close_to(ramp(3.0f, 1.0f, 0.25f), 2.0f), "0.5X of 1 s: %.9g at 0.25 s, not 2", ramp(3.0f, 1.0f, 0.25f));
		CHECK(close_to(ramp(3.0f, 1.0f, 1.5f), 3.0f), "0.5X of 1 s: %.9g at 1.5 s, not 3", ramp(3.0f, 1.0f, 1.5f));
		CHECK(close_to(ramp(2.5f, 0.6f, 0.3f), 2.5f), "0.5X of 0.6 s: %.9g at 0.3 s, not 2.5", ramp(2.5f, 0.6f, 0.3f));
		settings(1.0, 2.0);
		CHECK(close_to(ramp(3.0f, 1.0f, 1.0f), 2.0f), "2X of 1 s: %.9g at 1 s, not 2", ramp(3.0f, 1.0f, 1.0f));
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
