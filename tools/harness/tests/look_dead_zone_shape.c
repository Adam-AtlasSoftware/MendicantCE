#include "harness.h"
#include <math.h>

/* The look stick's dead zone shape (input.look_dead_zone_shape: source/input/input_xbox.c's
input_port_fix_stick_dead_zones). Auto, the default, must read both sticks as before it, to the bit, under either aim
style and either stick layout: the look stick round under Modern, each direction its own under Classic. Axial and
Round set the look stick's shape whatever the aim style. */

typedef struct { short x, y; } point2d;
#define SHORT_MAX 32767
#define SHORT_MIN (-32768)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, low, high) ((x) < (low) ? (low) : (x) > (high) ? (high) : (x))
enum { _gamepad_stick_left, _gamepad_stick_right };
enum { HALO_LOOK_DEAD_ZONE_AUTO, HALO_LOOK_DEAD_ZONE_AXIAL, HALO_LOOK_DEAD_ZONE_ROUND };
static real square_root(real x) { return sqrtf(x); }

/* the fake world: the player's aim style, stick layout (the look stick) and shape; each stick's dead zones */
static int world_modern;
static short world_look_stick = _gamepad_stick_right;
static int world_shape = HALO_LOOK_DEAD_ZONE_AUTO;

static short halo_stick_dead_range(short gamepad, short stick) { (void)gamepad; return stick ? 9000 : 6000; }
static short halo_stick_live_range(short gamepad, short stick) { (void)gamepad; (void)stick; return SHORT_MAX; }
static int halo_look_dead_zone_shape(short gamepad) { (void)gamepad; return world_shape; }
static int player_control_aim_modern(short gamepad, float *tangent, float *threshold)
{
	(void)gamepad;
	*tangent = 0.36f;
	*threshold = -1.0f;
	return world_modern;
}
static short input_abstraction_port_look_stick(short gamepad) { (void)gamepad; return world_look_stick; }

#include "under_test.inc"

/* the sticks as they were read before the shape, word for word (but its live ranges, the rim) */
static void original(short gamepad_index, short left_x, short left_y, short right_x, short right_y, point2d *sticks)
{
	short look_stick = NONE;
	short left_range = halo_stick_dead_range(gamepad_index, _gamepad_stick_left);
	short right_range = halo_stick_dead_range(gamepad_index, _gamepad_stick_right);
	float acceleration_tangent;
	float acceleration_threshold;

	if (player_control_aim_modern(gamepad_index, &acceleration_tangent, &acceleration_threshold))
	{
		look_stick = input_abstraction_port_look_stick(gamepad_index);
	}
	if (look_stick == _gamepad_stick_left)
	{
		fix_round_dead_zone(left_x, left_y, left_range, SHORT_MAX, &sticks[_gamepad_stick_left]);
	}
	else
	{
		sticks[_gamepad_stick_left].x = fix_dead_zone(left_x, left_range);
		sticks[_gamepad_stick_left].y = fix_dead_zone(left_y, left_range);
	}
	if (look_stick == _gamepad_stick_right)
	{
		fix_round_dead_zone(right_x, right_y, right_range, SHORT_MAX, &sticks[_gamepad_stick_right]);
	}
	else
	{
		sticks[_gamepad_stick_right].x = fix_dead_zone(right_x, right_range);
		sticks[_gamepad_stick_right].y = fix_dead_zone(right_y, right_range);
	}
}

static boolean same(point2d const *a, point2d const *b)
{
	return a[0].x == b[0].x && a[0].y == b[0].y && a[1].x == b[1].x && a[1].y == b[1].y;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static const short looks[] = { _gamepad_stick_right, _gamepad_stick_left, NONE };
	long x, y;
	unsigned look;

	CASE("default_is_the_original")
	{
		world_shape = HALO_LOOK_DEAD_ZONE_AUTO;
		for (world_modern = 0; world_modern <= 1; world_modern++)
		{
			for (look = 0; look < sizeof(looks) / sizeof(looks[0]); look++)
			{
				world_look_stick = looks[look];
				for (x = SHORT_MIN; x <= SHORT_MAX; x += 1531)
				{
					for (y = SHORT_MIN; y <= SHORT_MAX; y += 1777)
					{
						point2d got[2], was[2];

						input_port_fix_stick_dead_zones(0, (short)x, (short)y, (short)y, (short)x, got);
						original(0, (short)x, (short)y, (short)y, (short)x, was);
						CHECK(same(got, was), "%s, look stick %d: %ld, %ld read %d, %d and %d, %d, not the "
							"original's %d, %d and %d, %d", world_modern ? "Modern" : "Classic", looks[look], x, y,
							got[0].x, got[0].y, got[1].x, got[1].y, was[0].x, was[0].y, was[1].x, was[1].y);
					}
				}
			}
		}
		return 0;
	}
	CASE("shape_takes_effect")
	{
		point2d got[2];

		/* a diagonal at 7000 each way: inside the right stick's square dead zone (9000), outside its round one */
		world_look_stick = _gamepad_stick_right;
		world_modern = 0;
		world_shape = HALO_LOOK_DEAD_ZONE_ROUND;
		input_port_fix_stick_dead_zones(0, 0, 0, 7000, 7000, got);
		CHECK(got[1].x > 0 && got[1].y > 0, "Round under Classic read %d, %d, inside the dead zone", got[1].x,
			got[1].y);
		world_modern = 1;
		world_shape = HALO_LOOK_DEAD_ZONE_AXIAL;
		input_port_fix_stick_dead_zones(0, 0, 0, 7000, 7000, got);
		CHECK(got[1].x == 0 && got[1].y == 0, "Axial under Modern read %d, %d, not inside the dead zone", got[1].x,
			got[1].y);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
