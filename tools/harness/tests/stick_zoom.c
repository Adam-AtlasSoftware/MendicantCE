#include "harness.h"

/* The look stick's zoomed turn (input.stick_zoom_sensitivity: source/game/player_control.c's
player_control_stick_zoom_look). 1, the default, must leave the turn as the original's, to the bit; another value scales
the turn zoomed in alone. */

typedef struct { real yaw, pitch; } real_euler_angles2d;

/* the fake world: the player's stick zoom sensitivity */
static float world_zoom = 1.0f;
static float halo_stick_zoom_sensitivity(short gamepad) { (void)gamepad; return world_zoom; }

#include "under_test.inc"

/* a turn, zoomed (zoom level 0) or not (NONE) */
static real_euler_angles2d turn(short zoom_level)
{
	real_euler_angles2d look = { 0.0123456f, -0.00654321f };

	player_control_stick_zoom_look(0, 7, zoom_level, &look);
	return look;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("default_is_the_original")
	{
		real_euler_angles2d zoomed, unzoomed;

		world_zoom = 1.0f;
		zoomed = turn(0);
		unzoomed = turn(NONE);
		CHECK(zoomed.yaw == 0.0123456f && zoomed.pitch == -0.00654321f, "1X zoomed turned %.9g, %.9g", zoomed.yaw,
			zoomed.pitch);
		CHECK(unzoomed.yaw == 0.0123456f && unzoomed.pitch == -0.00654321f, "1X turned %.9g, %.9g", unzoomed.yaw,
			unzoomed.pitch);
		return 0;
	}
	CASE("scales_zoomed_alone")
	{
		real_euler_angles2d zoomed, unzoomed;

		world_zoom = 2.0f;
		zoomed = turn(1);
		unzoomed = turn(NONE);
		CHECK(zoomed.yaw == 2.0f * 0.0123456f && zoomed.pitch == 2.0f * -0.00654321f, "2X zoomed turned %.9g, %.9g",
			zoomed.yaw, zoomed.pitch);
		CHECK(unzoomed.yaw == 0.0123456f && unzoomed.pitch == -0.00654321f, "2X unzoomed turned %.9g, %.9g",
			unzoomed.yaw, unzoomed.pitch);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
