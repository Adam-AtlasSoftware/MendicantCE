/*
INPUT_DISPLAY.C

The on-screen input display (display.input_display, off by default): a
small panel at the bottom right of each view, above the match timer, with
what its player's input asks of the game this tick, for streams and videos:

- the move stick (a dot in its box), or with the keyboard and mouse the W,
  A, S and D keys lit by the way the player moves;
- the look (the turn the tick asks for: a dot from the box's middle, by the
  stick's or the mouse's turn alike);
- the fire trigger's pull (a bar beside them);
- the game's actions held: grenade, melee, jump, crouch, reload, action,
  swap weapons and flashlight, as the game names them (not a controller's
  buttons, which a player can remap).

It reads each tick's players' actions (struct player_action) as the tick
takes them (players.c's players_update_before_game): the machine's own
players', or in a demo's playback the recorded ones, so a demo shows its
players' input as they played; each split-screen view its own player's.
The keys are drawn for the first player while the keyboard and mouse play
(input.button_prompts' choice of the device, halo_keyboard_prompts) and
never in a playback, whose recording holds no device: there the move stick
is drawn.

Hide HUD hides it with the HUD (hud.c draws it while the HUD is shown). It
reads the actions and writes nothing of the game: the game plays the same
with it as without (determinism). Off, nothing is kept or drawn.
*/

#include "cseries.h"
#include "game/players.h"
#include "units/units.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "demo.h"
#include "halo_keyboard.h"
#include "input_display.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* the panel's place: its right edge in from the view's, its bottom
	above the view's (clear of the match timer's line), its padding */
	INPUT_DISPLAY_RIGHT_MARGIN = 8,
	INPUT_DISPLAY_BOTTOM_MARGIN = 34,
	INPUT_DISPLAY_PADDING = 3,
	/* the stick boxes' size, the trigger bar's width, the gap between
	them */
	INPUT_DISPLAY_BOX = 30,
	INPUT_DISPLAY_BAR = 8,
	INPUT_DISPLAY_GAP = 2,
	/* the actions' cells: four a row, two rows */
	INPUT_DISPLAY_CELL_WIDTH = 30,
	INPUT_DISPLAY_CELL_HEIGHT = 13,
	INPUT_DISPLAY_CELLS_A_ROW = 4,
	/* the dot's size, the keys' */
	INPUT_DISPLAY_DOT = 4,
	INPUT_DISPLAY_KEY = 9,
	/* draw_string.c's centred text (its enum is its own) */
	_input_display_text_center = 2
};

/* the actions drawn, in their cells' order */
enum
{
	_input_display_grenade,
	_input_display_melee,
	_input_display_jump,
	_input_display_crouch,
	_input_display_reload,
	_input_display_action,
	_input_display_swap,
	_input_display_flashlight,
	NUMBER_OF_INPUT_DISPLAY_ACTIONS
};

/* the W, A, S, D keys */
enum
{
	_input_display_key_forward,
	_input_display_key_left,
	_input_display_key_backward,
	_input_display_key_right,
	NUMBER_OF_INPUT_DISPLAY_KEYS
};

/* the look's turn in a tick (radians) that reaches the box's edge, and a
stick's or the keys' push that lights a key */
#define INPUT_DISPLAY_FULL_TURN 0.12f
#define INPUT_DISPLAY_KEY_PUSH 0.3f
#define INPUT_DISPLAY_PI 3.14159265f

/* ---------- prototypes */

/* port_config.c's, the HUD's quads (rasterizer) */
int config_boolean(const char *name);
unsigned long config_changes(void);
void draw_quad(rectangle2d *rectangle, pixel32 color);

/* ---------- structures */

/* what a player's input was this tick */
struct input_display_player
{
	boolean valid;
	struct player_action action;
	/* the look's turn this tick (the facing's change since the last),
	radians */
	real turn_yaw;
	real turn_pitch;
	boolean have_facing;
	real last_yaw;
	real last_pitch;
};

/* ---------- globals */

static struct input_display_player input_display_players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

static wchar_t const *const input_display_action_names[NUMBER_OF_INPUT_DISPLAY_ACTIONS] =
{
	L"GRN", L"MEL", L"JMP", L"CRH", L"RLD", L"ACT", L"SWP", L"FLS"
};

/* ---------- private code */

/* an angle's change taken the short way round; 0 for one that is no
number or too large to be a turn (the slots of the tick's actions that no
player has hold whatever was there: never a loop over them) */
static real input_display_wrap(
	real angle)
{
	if (!(angle > -100.0f && angle < 100.0f))
		return 0.0f;
	angle -= 2.0f * INPUT_DISPLAY_PI * floorf((angle + INPUT_DISPLAY_PI) / (2.0f * INPUT_DISPLAY_PI));
	return angle;
}

/* a player's action this tick kept, and the look's turn since the last */
static void input_display_note(
	struct input_display_player *player,
	struct player_action const *action)
{
	player->action = *action;
	player->valid = TRUE;
	if (player->have_facing)
	{
		player->turn_yaw = input_display_wrap(action->desired_facing.yaw - player->last_yaw);
		player->turn_pitch = input_display_wrap(action->desired_facing.pitch - player->last_pitch);
	}
	else
		player->turn_yaw = player->turn_pitch = 0.0f;
	player->last_yaw = action->desired_facing.yaw;
	player->last_pitch = action->desired_facing.pitch;
	player->have_facing = TRUE;
}

/* the actions held (a bit for each _input_display_*) */
static unsigned long input_display_actions_held(
	struct player_action const *action)
{
	static short const bits[NUMBER_OF_INPUT_DISPLAY_ACTIONS] =
	{
		_unit_control_throw_grenade_bit, _unit_control_use_equipment_bit, _unit_control_jump_bit,
		_unit_control_crouch_modifier_bit, _unit_control_weapon_reload_bit, _unit_control_action_bit,
		_unit_control_swap_weapons_bit, _unit_control_integrated_light_bit
	};
	unsigned long held = 0;
	short index;

	for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_ACTIONS; index++)
	{
		if (action->control_flags & (1UL << bits[index]))
			held |= 1UL << index;
	}
	return held;
}

/* the keys lit by the move (the throttle: i forward, j to the left) */
static unsigned long input_display_keys_lit(
	struct player_action const *action)
{
	unsigned long lit = 0;

	if (action->throttle.i > INPUT_DISPLAY_KEY_PUSH)
		lit |= 1UL << _input_display_key_forward;
	if (action->throttle.i < -INPUT_DISPLAY_KEY_PUSH)
		lit |= 1UL << _input_display_key_backward;
	if (action->throttle.j > INPUT_DISPLAY_KEY_PUSH)
		lit |= 1UL << _input_display_key_left;
	if (action->throttle.j < -INPUT_DISPLAY_KEY_PUSH)
		lit |= 1UL << _input_display_key_right;
	return lit;
}

/* whether a view's move is drawn as the keys: the first player's, with the
keyboard and mouse playing, never a playback's */
static boolean input_display_keyboard_style(
	short local_player_index,
	boolean keyboard_prompts,
	boolean demo_playing)
{
	return local_player_index == 0 && keyboard_prompts && !demo_playing;
}

static boolean input_display_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;

	if (changes != config_changes())
	{
		changes = config_changes();
		enabled = config_boolean("display.input_display") != 0;
	}
	return enabled;
}

static void input_display_box(
	short x0,
	short y0,
	short width,
	short height,
	pixel32 color)
{
	rectangle2d box;

	box.x0 = x0;
	box.y0 = y0;
	box.x1 = (short)(x0 + width);
	box.y1 = (short)(y0 + height);
	draw_quad(&box, color);
}

static void input_display_label(
	long font_index,
	short x0,
	short y0,
	short width,
	short height,
	wchar_t const *text,
	boolean lit)
{
	real_argb_color color;
	rectangle2d bounds;

	if (font_index == NONE)
		return;
	color.alpha = 1.0f;
	color.red = color.green = color.blue = lit ? 1.0f : 0.6f;
	bounds.x0 = x0;
	bounds.x1 = (short)(x0 + width);
	bounds.y0 = y0;
	bounds.y1 = (short)(y0 + height);
	draw_string_set_draw_mode(font_index, NONE, _input_display_text_center, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

/* a box with a dot at x, y (each -1 to 1, y up) from its middle */
static void input_display_stick(
	short x0,
	short y0,
	real x,
	real y)
{
	short middle_x = (short)(x0 + INPUT_DISPLAY_BOX / 2);
	short middle_y = (short)(y0 + INPUT_DISPLAY_BOX / 2);
	short reach = (short)(INPUT_DISPLAY_BOX / 2 - INPUT_DISPLAY_DOT / 2 - 1);

	input_display_box(x0, y0, INPUT_DISPLAY_BOX, INPUT_DISPLAY_BOX, 0x60303030);
	input_display_box(x0, middle_y, INPUT_DISPLAY_BOX, 1, 0x40FFFFFF);
	input_display_box(middle_x, y0, 1, INPUT_DISPLAY_BOX, 0x40FFFFFF);
	x = PIN(x, -1.0f, 1.0f);
	y = PIN(y, -1.0f, 1.0f);
	input_display_box((short)(middle_x + (short)(x * reach) - INPUT_DISPLAY_DOT / 2),
		(short)(middle_y - (short)(y * reach) - INPUT_DISPLAY_DOT / 2), INPUT_DISPLAY_DOT, INPUT_DISPLAY_DOT,
		0xFFFFFFFF);
}

/* the W, A, S, D keys in the move's box */
static void input_display_keys(
	long font_index,
	short x0,
	short y0,
	unsigned long lit)
{
	static wchar_t const *const names[NUMBER_OF_INPUT_DISPLAY_KEYS] = { L"W", L"A", L"S", L"D" };
	short places[NUMBER_OF_INPUT_DISPLAY_KEYS][2];
	short step = INPUT_DISPLAY_KEY + 1;
	short left = (short)(x0 + (INPUT_DISPLAY_BOX - 3 * step + 1) / 2);
	short top = (short)(y0 + (INPUT_DISPLAY_BOX - 2 * step + 1) / 2);
	short key;

	places[_input_display_key_forward][0] = (short)(left + step);
	places[_input_display_key_forward][1] = top;
	places[_input_display_key_left][0] = left;
	places[_input_display_key_left][1] = (short)(top + step);
	places[_input_display_key_backward][0] = (short)(left + step);
	places[_input_display_key_backward][1] = (short)(top + step);
	places[_input_display_key_right][0] = (short)(left + 2 * step);
	places[_input_display_key_right][1] = (short)(top + step);
	input_display_box(x0, y0, INPUT_DISPLAY_BOX, INPUT_DISPLAY_BOX, 0x60303030);
	for (key = 0; key < NUMBER_OF_INPUT_DISPLAY_KEYS; key++)
	{
		boolean on = (lit & (1UL << key)) != 0;

		input_display_box(places[key][0], places[key][1], INPUT_DISPLAY_KEY, INPUT_DISPLAY_KEY,
			on ? 0xE03C8CDC : 0x60505050);
		input_display_label(font_index, places[key][0], (short)(places[key][1] - 2), INPUT_DISPLAY_KEY,
			INPUT_DISPLAY_KEY + 2, names[key], on);
	}
}

/* ---------- public code */

void input_display_tick(
	struct player_action const *actions,
	short count)
{
	short index;

	if (!input_display_enabled())
		return;
	for (index = 0; index < count && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		input_display_note(&input_display_players[index], &actions[index]);
}

void input_display_draw(
	short local_player_index,
	long player_index,
	long font_index)
{
	struct input_display_player const *player;
	short absolute_index;
	short width = INPUT_DISPLAY_CELLS_A_ROW * INPUT_DISPLAY_CELL_WIDTH +
		(INPUT_DISPLAY_CELLS_A_ROW - 1) * INPUT_DISPLAY_GAP;
	short height = INPUT_DISPLAY_BOX + 2 * (INPUT_DISPLAY_GAP + INPUT_DISPLAY_CELL_HEIGHT);
	short right, bottom, left, top, x, y;
	unsigned long held;
	short index;

	if (!input_display_enabled() || player_index == NONE)
		return;
	absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (absolute_index < 0 || absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		return;
	player = &input_display_players[absolute_index];
	if (!player->valid)
		return;

	/* (the view's coordinates, as the match timer's) */
	right = (short)(render.camera.window_bounds.x1 - render.camera.viewport_bounds.x0 - INPUT_DISPLAY_RIGHT_MARGIN);
	bottom = (short)(render.camera.window_bounds.y1 - render.camera.viewport_bounds.y0 - INPUT_DISPLAY_BOTTOM_MARGIN);
	left = (short)(right - width);
	top = (short)(bottom - height);
	input_display_box((short)(left - INPUT_DISPLAY_PADDING), (short)(top - INPUT_DISPLAY_PADDING),
		(short)(width + 2 * INPUT_DISPLAY_PADDING), (short)(height + 2 * INPUT_DISPLAY_PADDING), 0x80000000);

	/* the move, the look and the trigger */
	if (input_display_keyboard_style(local_player_index, halo_keyboard_prompts() != 0, demo_playing()))
		input_display_keys(font_index, left, top, input_display_keys_lit(&player->action));
	else
		input_display_stick(left, top, -player->action.throttle.j, player->action.throttle.i);
	x = (short)(left + INPUT_DISPLAY_BOX + INPUT_DISPLAY_GAP);
	input_display_stick(x, top, -player->turn_yaw / INPUT_DISPLAY_FULL_TURN,
		player->turn_pitch / INPUT_DISPLAY_FULL_TURN);
	x = (short)(x + INPUT_DISPLAY_BOX + INPUT_DISPLAY_GAP);
	input_display_box(x, top, INPUT_DISPLAY_BAR, INPUT_DISPLAY_BOX, 0x60303030);
	{
		short pull = (short)(PIN(player->action.primary_trigger, 0.0f, 1.0f) * INPUT_DISPLAY_BOX + 0.5f);

		if (pull > 0)
			input_display_box(x, (short)(top + INPUT_DISPLAY_BOX - pull), INPUT_DISPLAY_BAR, pull, 0xE0DC5A3C);
	}
	input_display_label(font_index, (short)(x + INPUT_DISPLAY_BAR + INPUT_DISPLAY_GAP), top,
		(short)(right - x - INPUT_DISPLAY_BAR - INPUT_DISPLAY_GAP), INPUT_DISPLAY_CELL_HEIGHT, L"FIRE",
		player->action.primary_trigger > 0.0f);

	/* the actions held */
	held = input_display_actions_held(&player->action);
	for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_ACTIONS; index++)
	{
		boolean on = (held & (1UL << index)) != 0;

		x = (short)(left + (index % INPUT_DISPLAY_CELLS_A_ROW) * (INPUT_DISPLAY_CELL_WIDTH + INPUT_DISPLAY_GAP));
		y = (short)(top + INPUT_DISPLAY_BOX + INPUT_DISPLAY_GAP +
			(index / INPUT_DISPLAY_CELLS_A_ROW) * (INPUT_DISPLAY_CELL_HEIGHT + INPUT_DISPLAY_GAP));
		input_display_box(x, y, INPUT_DISPLAY_CELL_WIDTH, INPUT_DISPLAY_CELL_HEIGHT, on ? 0xE03C8CDC : 0x60303030);
		input_display_label(font_index, x, (short)(y - 1), INPUT_DISPLAY_CELL_WIDTH, INPUT_DISPLAY_CELL_HEIGHT + 2,
			input_display_action_names[index], on);
	}
}
