/*
INPUT_DISPLAY.C (test)

The input display of port/linux/game/input_display.c: what it keeps of each
tick's actions (the actions held, the move's keys, the look's turn the short
way round), by the players' indices; nothing while it is off; the keys drawn
only for the first player with the keyboard and mouse, never in a playback.
test_input_display.py takes the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 16
#define PIN(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))

typedef struct { real yaw, pitch; } real_euler_angles2d;
typedef struct { real i, j; } real_vector2d;

static int setting_on;
static int config_boolean(const char *name) { (void)name; return setting_on; }
static unsigned long config_changes(void) { return (unsigned long)setting_on; }

#include "under_test.inc"

static struct player_action actions[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

/* the terminal font's capitals as the HUD draws them (high-res text's Hack
Bold), in the HUD's units: measured in the first picture, "GRN" 54 pixels
wide at 2.2 pixels a unit, its capitals 20 pixels tall */
#define GLYPH_WIDTH 8.2f
#define GLYPH_HEIGHT 9.0f
/* the room a label keeps either side of it, a key's letter */
#define LABEL_PADDING 3.0f
#define KEY_PADDING 1.5f

static boolean inside(rectangle2d const *rectangle, short x0, short y0, short x1, short y1)
{
	return rectangle->x0 >= x0 && rectangle->y0 >= y0 && rectangle->x1 <= x1 && rectangle->y1 <= y1 &&
		rectangle->x0 < rectangle->x1 && rectangle->y0 < rectangle->y1;
}

/* a label of so many capitals fits with padding either side, and its
capitals' height with a unit above and below */
static boolean fits(rectangle2d const *rectangle, short letters, real padding)
{
	return rectangle->x1 - rectangle->x0 >= letters * GLYPH_WIDTH + 2 * padding &&
		rectangle->y1 - rectangle->y0 >= GLYPH_HEIGHT + 2;
}

static void reset(void)
{
	memset(actions, 0, sizeof(actions));
	memset(input_display_players, 0, sizeof(input_display_players));
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	reset();
	/* each player's actions kept by its index, the look's turn from the
	tick before */
	CASE("tick")
	{
		setting_on = 1;
		actions[2].control_flags = (1UL << _unit_control_jump_bit) | (1UL << _unit_control_throw_grenade_bit);
		actions[2].throttle.i = 1.0f;
		actions[2].desired_facing.yaw = 1.0f;
		actions[2].primary_trigger = 0.5f;
		input_display_tick(actions, HALO_PORT_MAXIMUM_NETWORK_PLAYERS);
		CHECK(input_display_players[2].valid && input_display_players[2].turn_yaw == 0.0f, "first tick");
		actions[2].desired_facing.yaw = 1.05f;
		actions[2].desired_facing.pitch = -0.02f;
		input_display_tick(actions, HALO_PORT_MAXIMUM_NETWORK_PLAYERS);
		CHECK(input_display_players[2].turn_yaw > 0.049f && input_display_players[2].turn_yaw < 0.051f, "yaw %f",
			input_display_players[2].turn_yaw);
		CHECK(input_display_players[2].turn_pitch < -0.019f && input_display_players[2].turn_pitch > -0.021f,
			"pitch %f", input_display_players[2].turn_pitch);
		CHECK(input_display_actions_held(&input_display_players[2].action) ==
			((1UL << _input_display_jump) | (1UL << _input_display_grenade)), "held %lx",
			input_display_actions_held(&input_display_players[2].action));
		CHECK(input_display_players[2].action.primary_trigger == 0.5f, "trigger");
		CHECK(input_display_actions_held(&input_display_players[0].action) == 0, "player 0's held");
		return 0;
	}
	/* off: nothing kept */
	CASE("off")
	{
		setting_on = 0;
		actions[0].control_flags = 1UL << _unit_control_jump_bit;
		input_display_tick(actions, HALO_PORT_MAXIMUM_NETWORK_PLAYERS);
		CHECK(!input_display_players[0].valid, "kept while off");
		CHECK(input_display_players[0].action.control_flags == 0, "kept while off");
		return 0;
	}
	/* a turn across the half turn taken the short way */
	CASE("wrap")
	{
		setting_on = 1;
		actions[0].desired_facing.yaw = 3.1f;
		input_display_tick(actions, 1);
		actions[0].desired_facing.yaw = -3.1f;
		input_display_tick(actions, 1);
		CHECK(input_display_players[0].turn_yaw > 0.08f && input_display_players[0].turn_yaw < 0.09f, "yaw %f",
			input_display_players[0].turn_yaw);
		return 0;
	}
	/* a slot no player has holds whatever was there: a huge or no number
	gives no turn (and never a loop) */
	CASE("garbage")
	{
		union { unsigned int bits; real value; } not_a_number;

		setting_on = 1;
		not_a_number.bits = 0x7FC00000u;
		actions[5].desired_facing.yaw = 1e30f;
		input_display_tick(actions, HALO_PORT_MAXIMUM_NETWORK_PLAYERS);
		actions[5].desired_facing.yaw = -3e30f;
		actions[5].desired_facing.pitch = not_a_number.value;
		input_display_tick(actions, HALO_PORT_MAXIMUM_NETWORK_PLAYERS);
		CHECK(input_display_players[5].turn_yaw == 0.0f && input_display_players[5].turn_pitch == 0.0f,
			"turn %g %g", input_display_players[5].turn_yaw, input_display_players[5].turn_pitch);
		return 0;
	}
	/* the panel at every screen shape and split, its labels inside their
	cells with room either side, the panel inside the view (views in the
	HUD's 480 lines: 4:3, 16:9 and 21:9, whole, halved (2 players) and
	quartered (4 players)) */
	CASE("fit")
	{
		static short const widths[] = { 640, 854, 1120 };
		static short const heights[] = { 480, 240 };
		short shape, split, quarter, index;

		for (shape = 0; shape < 3; shape++)
		for (split = 0; split < 2; split++)
		for (quarter = 0; quarter < 2; quarter++)
		{
			short width = (short)(quarter ? widths[shape] / 2 : widths[shape]);
			short height = heights[split || quarter];
			struct input_display_layout layout;

			input_display_layout((short)(width - INPUT_DISPLAY_RIGHT_MARGIN),
				(short)(height - INPUT_DISPLAY_BOTTOM_MARGIN), &layout);
			CHECK(inside(&layout.panel, 0, 0, width, height), "the panel outside a %dx%d view", width, height);
			CHECK(inside(&layout.move, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1) &&
				inside(&layout.look, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1) &&
				inside(&layout.fire, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1),
				"the boxes outside the panel");
			CHECK(fits(&layout.fire, 4, LABEL_PADDING), "FIRE does not fit its room");
			for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_ACTIONS; index++)
			{
				CHECK(inside(&layout.cells[index], layout.panel.x0, layout.panel.y0, layout.panel.x1,
					layout.panel.y1), "cell %d outside the panel", index);
				CHECK(fits(&layout.cells[index], 3, LABEL_PADDING), "cell %d: %d wide, its label %.1f",
					index, layout.cells[index].x1 - layout.cells[index].x0, 3 * GLYPH_WIDTH);
				CHECK(index % INPUT_DISPLAY_CELLS_A_ROW == 0 ||
					layout.cells[index].x0 >= layout.cells[index - 1].x1, "cells %d and %d overlap", index - 1,
					index);
			}
			for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_KEYS; index++)
			{
				CHECK(inside(&layout.keys[index], layout.move.x0, layout.move.y0, layout.move.x1, layout.move.y1),
					"key %d outside the move's box", index);
				CHECK(fits(&layout.keys[index], 1, KEY_PADDING), "key %d: %d wide", index,
					layout.keys[index].x1 - layout.keys[index].x0);
			}
		}
		return 0;
	}
	/* the keys by the move: forward and to the left, then back and right */
	CASE("keys")
	{
		struct player_action action;

		memset(&action, 0, sizeof(action));
		action.throttle.i = 1.0f;
		action.throttle.j = 1.0f;
		CHECK(input_display_keys_lit(&action) ==
			((1UL << _input_display_key_forward) | (1UL << _input_display_key_left)), "%lx",
			input_display_keys_lit(&action));
		action.throttle.i = -1.0f;
		action.throttle.j = -1.0f;
		CHECK(input_display_keys_lit(&action) ==
			((1UL << _input_display_key_backward) | (1UL << _input_display_key_right)), "%lx",
			input_display_keys_lit(&action));
		action.throttle.i = 0.1f;
		action.throttle.j = 0.0f;
		CHECK(input_display_keys_lit(&action) == 0, "a small push lit a key");
		return 0;
	}
	/* the keys only for the first player with the keyboard and mouse, never
	in a playback */
	CASE("style")
	{
		CHECK(input_display_keyboard_style(0, TRUE, FALSE), "the first player's keyboard");
		CHECK(!input_display_keyboard_style(1, TRUE, FALSE), "the second player's");
		CHECK(!input_display_keyboard_style(0, FALSE, FALSE), "a controller");
		CHECK(!input_display_keyboard_style(0, TRUE, TRUE), "a playback");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
