#include "harness.h"
#include <math.h>

/* A controller's look stick's turn up and down (source/game/player_control.c's player_control_stick_look) times its
player's vertical sensitivity (input.look_vertical_sensitivity: halo_look_vertical_sensitivity). Its default, 1, must
turn as the original did, to the bit; another value scales the turn up and down alone. */

typedef struct { real yaw, pitch; } real_euler_angles2d;

enum { _button_scope_zoom, NUMBER_OF_BUTTONS };
struct game_globals_player_control
{
	struct { long count; real *address; } look_function;
	real look_pegging_threshold;
};
struct game_input_state
{
	boolean buttons[NUMBER_OF_BUTTONS];
	real yaw, pitch;
};

static boolean controls_enable_doubled_spin = TRUE;
static boolean controls_swap_doubled_spin_state = FALSE;

/* the fake world: the player's vertical sensitivity; the stick's curve leaves each push as it is */
static float world_vertical = 1.0f;

static float halo_look_vertical_sensitivity(short gamepad) { (void)gamepad; return world_vertical; }
static int player_control_aim_modern(short gamepad, float *tangent, float *threshold)
{
	(void)gamepad;
	*tangent = 0.0f;
	*threshold = -1.0f;
	return FALSE;
}
/* (the look acceleration at its defaults: test_look_acceleration.py's) */
static void player_control_look_acceleration_tuning(short gamepad, real *strength, real *time)
{
	(void)gamepad;
	*strength = 1.0f;
	*time = 1.0f;
}
struct player_control_aim;
static boolean player_control_stick_aim(struct player_control_aim const *aim, long count, real const *function,
	real pegging, real yaw, real pitch, real *curved_yaw, real *curved_pitch, real *clamped_yaw, real *clamped_pitch);

#include "under_test.inc"

static boolean player_control_stick_aim(struct player_control_aim const *aim, long count, real const *function,
	real pegging, real yaw, real pitch, real *curved_yaw, real *curved_pitch, real *clamped_yaw, real *clamped_pitch)
{
	(void)aim, (void)count, (void)function, (void)pegging;
	*curved_yaw = *clamped_yaw = yaw;
	*curved_pitch = *clamped_pitch = pitch;
	return FALSE;
}

static real curve[] = { 0.0f, 1.0f };
/* LOOK SENSITIVITY's middle step's rates (120 and 60 degrees a second), in radians a tick */
#define YAW_RATE (120.0f * 0.0174532925f / 30.0f)
#define PITCH_RATE (60.0f * 0.0174532925f / 30.0f)

/* the turn of a push, zoomed (its spin doubled) or not */
static real_euler_angles2d look(real yaw, real pitch, boolean zoomed)
{
	struct game_globals_player_control constants = { { 2, curve }, 0.9f };
	struct game_input_state input = { { zoomed }, yaw, pitch };
	real_euler_angles2d delta;
	real clamped_yaw, clamped_pitch;

	player_control_stick_look(&constants, 0, &input, YAW_RATE, PITCH_RATE, &delta, &clamped_yaw, &clamped_pitch);
	return delta;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static const real pushes[] = { 0.1f, 0.37f, 0.5f, 0.83f, 1.0f, -0.6f };
	unsigned index;

	CASE("default_is_the_original")
	{
		/* 1: the original's turn (its push times its spin times its rate), bit for bit */
		world_vertical = 1.0f;
		for (index = 0; index < sizeof(pushes) / sizeof(pushes[0]); index++)
		{
			boolean zoomed;

			for (zoomed = FALSE; zoomed <= TRUE; zoomed++)
			{
				real_euler_angles2d delta = look(pushes[index], pushes[index], zoomed);
				volatile real spin = zoomed ? 2.0f : 1.0f;
				volatile real yaw = pushes[index] * spin * YAW_RATE;
				volatile real pitch = pushes[index] * spin * PITCH_RATE;

				CHECK(delta.yaw == yaw && delta.pitch == pitch, "a push of %g turned %.9g, %.9g, not the original's "
					"%.9g, %.9g", pushes[index], delta.yaw, delta.pitch, yaw, pitch);
			}
		}
		return 0;
	}
	CASE("scales_up_and_down_alone")
	{
		real_euler_angles2d original, scaled;

		world_vertical = 1.0f;
		original = look(0.5f, 0.5f, FALSE);
		world_vertical = 2.0f;
		scaled = look(0.5f, 0.5f, FALSE);
		CHECK(scaled.yaw == original.yaw, "side to side turned %.9g, not %.9g as before", scaled.yaw, original.yaw);
		CHECK(fabsf(scaled.pitch - 2.0f * original.pitch) < 1e-7f, "up and down turned %.9g at 2X, not %.9g",
			scaled.pitch, 2.0f * original.pitch);
		world_vertical = 0.5f;
		scaled = look(0.5f, 0.5f, FALSE);
		CHECK(fabsf(scaled.pitch - 0.5f * original.pitch) < 1e-7f, "up and down turned %.9g at 0.5X, not %.9g",
			scaled.pitch, 0.5f * original.pitch);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
