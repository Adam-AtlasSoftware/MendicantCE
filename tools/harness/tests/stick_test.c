#include "harness.h"
#include <math.h>
#include <stdlib.h>

/* Advanced Stick Settings' readings (#1140: port/linux/game/menu_functions.c's stick_test_text): each stick as the
game read it, then after the dead zones and the look stick's shape the page shows, its changes not yet saved among
them, by the game's own code for both (input_xbox.c's input_port_fix_stick, xinput_sdl.c's halo_stick_ranges). */

typedef struct { short x, y; } point2d;
#define SHORT_MAX 32767
#define SHORT_MIN (-32768)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, low, high) ((x) < (low) ? (low) : (x) > (high) ? (high) : (x))
enum { _gamepad_stick_left, _gamepad_stick_right };
static real square_root(real x) { return sqrtf(x); }

/* the fake world: the page's values (Settings' changes kept, else the saved ones), the sticks as last read */
static char const *page_inner[2] = { "-1", "-1" };
static char const *page_outer[2] = { "0", "0" };
static char const *page_shape = "auto";
static char const *page_aim = "classic";
static point2d world_raw[2];

static boolean settings_value(char const *name, char *text, unsigned int size)
{
	char const *value = NULL;

	if (!strcmp(name, "input.left_stick_dead_zone")) value = page_inner[0];
	else if (!strcmp(name, "input.right_stick_dead_zone")) value = page_inner[1];
	else if (!strcmp(name, "input.left_stick_outer_dead_zone")) value = page_outer[0];
	else if (!strcmp(name, "input.right_stick_outer_dead_zone")) value = page_outer[1];
	else if (!strcmp(name, "input.look_dead_zone_shape")) value = page_shape;
	else if (!strcmp(name, "input.aim_style")) value = page_aim;
	if (!value)
		return FALSE;
	snprintf(text, size, "%s", value);
	return TRUE;
}

static short input_abstraction_port_look_stick(short gamepad) { (void)gamepad; return _gamepad_stick_right; }
static void halo_controller_stick(short gamepad, short stick, short *x, short *y)
{
	(void)gamepad;
	*x = world_raw[stick].x;
	*y = world_raw[stick].y;
}

#include "under_test.inc"

static char const *reading(short stick)
{
	static char text[40];

	stick_test_text(0, stick, text, sizeof(text));
	return text;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a stick let go of: its drift, read as none by the game's own dead zone (27%) */
	CASE("drift")
	{
		world_raw[_gamepad_stick_left].x = 3000;
		world_raw[_gamepad_stick_left].y = -2000;
		CHECK(!strcmp(reading(_gamepad_stick_left), "9,-6 | 0,0"), "drift: %s", reading(_gamepad_stick_left));
		page_inner[0] = "5";
		CHECK(!strcmp(reading(_gamepad_stick_left), "9,-6 | 4,-1"), "drift past a 5%% dead zone: %s",
			reading(_gamepad_stick_left));
		return 0;
	}
	/* the page's spinners moved, not yet saved: the reading follows them at once */
	CASE("live")
	{
		world_raw[_gamepad_stick_left].x = 16384;
		page_inner[0] = "25";
		CHECK(!strcmp(reading(_gamepad_stick_left), "50,0 | 33,0"), "at 25%%: %s", reading(_gamepad_stick_left));
		page_inner[0] = "50";
		CHECK(!strcmp(reading(_gamepad_stick_left), "50,0 | 0,0"), "at 50%%: %s", reading(_gamepad_stick_left));
		page_inner[0] = "0";
		page_outer[0] = "30";
		world_raw[_gamepad_stick_left].x = -23000;
		CHECK(!strcmp(reading(_gamepad_stick_left), "-70,0 | -100,0"), "a 30%% outer dead zone: %s",
			reading(_gamepad_stick_left));
		/* (the other stick its own) */
		CHECK(!strcmp(reading(_gamepad_stick_right), "0,0 | 0,0"), "the right stick: %s", reading(_gamepad_stick_right));
		return 0;
	}
	/* the look stick's shape: Auto round under Modern aim, Axial each direction its own; the other stick never */
	CASE("shape")
	{
		char axial[40];

		page_inner[0] = page_inner[1] = "25";
		world_raw[_gamepad_stick_left].x = world_raw[_gamepad_stick_right].x = 9000;
		world_raw[_gamepad_stick_left].y = world_raw[_gamepad_stick_right].y = 9000;
		snprintf(axial, sizeof(axial), "%s", reading(_gamepad_stick_right));
		CHECK(!strcmp(axial, "27,27 | 3,3"), "Classic's axial: %s", axial);
		page_aim = "modern";
		CHECK(!strcmp(reading(_gamepad_stick_right), "27,27 | 13,13"), "Modern's round: %s",
			reading(_gamepad_stick_right));
		CHECK(!strcmp(reading(_gamepad_stick_left), axial), "the move stick stays axial: %s",
			reading(_gamepad_stick_left));
		page_shape = "axial";
		CHECK(!strcmp(reading(_gamepad_stick_right), axial), "Axial whatever the aim: %s", reading(_gamepad_stick_right));
		page_shape = "round";
		page_aim = "classic";
		CHECK(!strcmp(reading(_gamepad_stick_right), "27,27 | 13,13"), "Round whatever the aim: %s",
			reading(_gamepad_stick_right));
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
