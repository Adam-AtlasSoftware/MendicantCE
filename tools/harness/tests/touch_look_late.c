#include "harness.h"
#include <math.h>

/* The touch controls' swipe and gyroscope as the host gathers them between reads (host_touch.c's
host_touch_look_read: what came since the last read, the read taking it), read by the frame's input
(touch_input_look) and late in the frame by the drawn view (touch_input_look_late), which keeps what it read for
the next frame's input. */

static float host_delta[4];

static void host_touch_look_read(float *delta)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		delta[index] = host_delta[index];
		host_delta[index] = 0.0f;
	}
}

/* (the gyroscope's turn as its settings make it, gyro_aim.c's, which test_gyro_aim.py's touch case tests: here, as
it came, in radians) */
static void touch_gyro_turn(float turn[2])
{
	(void)turn;
}

#include "under_test.inc"

#define SCALE 0.5f

static void swiped(float x, float y, float gyro_x, float gyro_y)
{
	host_delta[0] += x;
	host_delta[1] += y;
	host_delta[2] += gyro_x;
	host_delta[3] += gyro_y;
}

static int close_to(float a, float b) { return fabsf(a - b) < 1e-6f; }

/* each frame: a swipe before its input, another while it is calculated (which the view reads late), and the next
frame's input turns the player by both: nothing lost, nothing twice; the view's late reading is what came since the
input's */
static void case_frames(void)
{
	float player_yaw = 0.0f, player_pitch = 0.0f, gyro_yaw = 0.0f, gyro_pitch = 0.0f;
	float swiped_yaw = 0.0f, swiped_pitch = 0.0f, swiped_gyro_yaw = 0.0f, swiped_gyro_pitch = 0.0f;
	int frame;

	for (frame = 0; frame < 20; frame++)
	{
		float late_yaw, late_pitch, late_gyro_yaw, late_gyro_pitch;
		float before = (float)(frame % 3);
		float during = (float)(frame % 4) * 0.5f;

		swiped(before, -before, 0.25f * before, 0.0f);
		swiped_yaw += before;
		swiped_pitch -= before;
		swiped_gyro_yaw += 0.25f * before;
		touch_input_look(SCALE, &player_yaw, &player_pitch, &gyro_yaw, &gyro_pitch);
		CHECK(close_to(player_yaw, -swiped_yaw * SCALE) && close_to(player_pitch, -swiped_pitch * SCALE) &&
			close_to(gyro_yaw, swiped_gyro_yaw) && close_to(gyro_pitch, swiped_gyro_pitch),
			"frame %d: the player turned %f, %f by the swipe and %f, %f by the gyroscope, of %f, %f and %f, %f", frame,
			player_yaw, player_pitch, gyro_yaw, gyro_pitch, -swiped_yaw * SCALE, -swiped_pitch * SCALE,
			swiped_gyro_yaw, swiped_gyro_pitch);
		swiped(during, 2.0f * during, 0.0f, -during);
		swiped_yaw += during;
		swiped_pitch += 2.0f * during;
		swiped_gyro_pitch -= during;
		touch_input_look_late(SCALE, &late_yaw, &late_pitch, &late_gyro_yaw, &late_gyro_pitch);
		CHECK(close_to(late_yaw, -during * SCALE) && close_to(late_pitch, -2.0f * during * SCALE) &&
			close_to(late_gyro_yaw, 0.0f) && close_to(late_gyro_pitch, -during),
			"frame %d: the view read %f, %f and %f, %f late, of %f, %f and 0, %f", frame, late_yaw, late_pitch,
			late_gyro_yaw, late_gyro_pitch, -during * SCALE, -2.0f * during * SCALE, -during);
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2, "usage: %s <case>", argv[0]);
	if (!strcmp(argv[1], "frames"))
		case_frames();
	else
		CHECK(0, "no case %s", argv[1]);
	return 0;
}
