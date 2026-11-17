#include "harness.h"
#include <math.h>

/* A stick's outer dead zone (input.left_stick_outer_dead_zone, input.right_stick_outer_dead_zone: source/input/
input_xbox.c's fix_dead_zones and fix_round_dead_zone, live_range the deflection read as full). With none, the
default, each direction's dead zone and the round one must read every deflection as the original did, to the bit;
with one, a stick that stops short of its rim must still read as fully pushed. */

typedef struct { short x, y; } point2d;
#define SHORT_MAX 32767
#define SHORT_MIN (-32768)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, low, high) ((x) < (low) ? (low) : (x) > (high) ? (high) : (x))
static real square_root(real x) { return sqrtf(x); }

#include "under_test.inc"

/* the round dead zone as it was before the outer one, word for word */
static void original_round(short x, short y, short dead_range, point2d *stick)
{
	real length = square_root((real)x * (real)x + (real)y * (real)y);

	if (length <= (real)dead_range)
	{
		stick->x = 0;
		stick->y = 0;
	}
	else
	{
		real push = (MIN(length, (real)SHORT_MAX) - dead_range) / (real)(SHORT_MAX - dead_range);
		real scale = push * SHORT_MAX / length;

		stick->x = (short)PIN(x * scale, -SHORT_MAX, SHORT_MAX);
		stick->y = (short)PIN(y * scale, -SHORT_MAX, SHORT_MAX);
	}
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static const short dead_ranges[] = { 0, 1638, 9000, 16383 };
	unsigned index;
	long value;

	CASE("default_is_the_original")
	{
		for (index = 0; index < sizeof(dead_ranges) / sizeof(dead_ranges[0]); index++)
		{
			short dead = dead_ranges[index];

			for (value = SHORT_MIN; value <= SHORT_MAX; value++)
			{
				short got = fix_dead_zones((short)value, dead, SHORT_MAX);
				short was = fix_dead_zone((short)value, dead);

				CHECK(got == was, "%ld with a dead range of %d read %d, not the original's %d", value, dead, got, was);
			}
			for (value = SHORT_MIN; value <= SHORT_MAX; value += 97)
			{
				point2d got, was;

				fix_round_dead_zone((short)value, (short)(value / 3), dead, SHORT_MAX, &got);
				original_round((short)value, (short)(value / 3), dead, &was);
				CHECK(got.x == was.x && got.y == was.y, "round: %ld read %d, %d, not the original's %d, %d", value,
					got.x, got.y, was.x, was.y);
			}
		}
		return 0;
	}
	CASE("outer_reads_full")
	{
		/* 20% outer, the game's own inner (9000): from 80% of the travel on, a full push either way */
		short live = (short)(SHORT_MAX - 20 * SHORT_MAX / 100);
		point2d round;
		double length;

		CHECK(fix_dead_zones(live, 9000, live) == SHORT_MAX, "the live range read %d, not full",
			fix_dead_zones(live, 9000, live));
		CHECK(fix_dead_zones(30000, 9000, live) == SHORT_MAX, "past it read %d", fix_dead_zones(30000, 9000, live));
		CHECK(fix_dead_zones(-live, 9000, live) == -SHORT_MAX, "the live range back read %d",
			fix_dead_zones(-live, 9000, live));
		CHECK(fix_dead_zones(SHORT_MIN, 9000, live) == -SHORT_MAX, "the rim back read %d",
			fix_dead_zones(SHORT_MIN, 9000, live));
		CHECK(fix_dead_zones(9000, 9000, live) == 0, "the dead range read %d", fix_dead_zones(9000, 9000, live));
		value = fix_dead_zones((short)((9000 + live) / 2), 9000, live);
		CHECK(labs(value - SHORT_MAX / 2) < 3, "halfway between read %ld", value);
		/* a worn stick's diagonal, at 80% of the travel: a full push, its direction kept */
		fix_round_dead_zone(18536, 18536, 9000, live, &round);
		length = sqrt((double)round.x * round.x + (double)round.y * round.y);
		CHECK(fabs(length - SHORT_MAX) < 3 && abs(round.x - round.y) <= 1, "round: a short diagonal read %d, %d",
			round.x, round.y);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
