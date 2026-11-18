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
	/* the move's and the look's boxes' size, the trigger bar's width, the
	gap between them */
	INPUT_DISPLAY_BOX_WIDTH = 38,
	INPUT_DISPLAY_BOX_HEIGHT = 32,
	INPUT_DISPLAY_BAR = 8,
	INPUT_DISPLAY_GAP = 2,
	/* the actions' cells: four a row, two rows, wide enough for a label of
	three letters of the terminal font with room either side
	(test_input_display.py's fit) */
	INPUT_DISPLAY_CELL_WIDTH = 36,
	INPUT_DISPLAY_CELL_HEIGHT = 14,
	INPUT_DISPLAY_CELLS_A_ROW = 4,
	/* the dot's size; the W, A, S, D keys', three across the move's box, a
	letter each */
	INPUT_DISPLAY_DOT = 4,
	INPUT_DISPLAY_KEY_WIDTH = 12,
	INPUT_DISPLAY_KEY_HEIGHT = 14,
	INPUT_DISPLAY_KEY_GAP = 1,
	/* the panel's size */
	INPUT_DISPLAY_WIDTH = INPUT_DISPLAY_CELLS_A_ROW * INPUT_DISPLAY_CELL_WIDTH +
		(INPUT_DISPLAY_CELLS_A_ROW - 1) * INPUT_DISPLAY_GAP,
	INPUT_DISPLAY_HEIGHT = INPUT_DISPLAY_BOX_HEIGHT + 2 * (INPUT_DISPLAY_GAP + INPUT_DISPLAY_CELL_HEIGHT),
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

/* where the panel's parts are drawn in a view (input_display_layout) */
struct input_display_layout
{
	rectangle2d panel;
	rectangle2d move;
	rectangle2d look;
	rectangle2d bar;
	rectangle2d fire;
	rectangle2d keys[NUMBER_OF_INPUT_DISPLAY_KEYS];
	rectangle2d cells[NUMBER_OF_INPUT_DISPLAY_ACTIONS];
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

static void input_display_rectangle(
	rectangle2d *rectangle,
	short x0,
	short y0,
	short width,
	short height)
{
	rectangle->x0 = x0;
	rectangle->y0 = y0;
	rectangle->x1 = (short)(x0 + width);
	rectangle->y1 = (short)(y0 + height);
}

/* the panel's parts, its bottom right corner at right, bottom (the view's
coordinates) */
static void input_display_layout(
	short right,
	short bottom,
	struct input_display_layout *layout)
{
	short left = (short)(right - INPUT_DISPLAY_WIDTH);
	short top = (short)(bottom - INPUT_DISPLAY_HEIGHT);
	short keys_left = (short)(left + (INPUT_DISPLAY_BOX_WIDTH - 3 * INPUT_DISPLAY_KEY_WIDTH -
		2 * INPUT_DISPLAY_KEY_GAP) / 2);
	short keys_top = (short)(top + (INPUT_DISPLAY_BOX_HEIGHT - 2 * INPUT_DISPLAY_KEY_HEIGHT -
		INPUT_DISPLAY_KEY_GAP) / 2);
	short key_step = INPUT_DISPLAY_KEY_WIDTH + INPUT_DISPLAY_KEY_GAP;
	short row_step = INPUT_DISPLAY_KEY_HEIGHT + INPUT_DISPLAY_KEY_GAP;
	short x;
	short index;

	input_display_rectangle(&layout->panel, (short)(left - INPUT_DISPLAY_PADDING), (short)(top - INPUT_DISPLAY_PADDING),
		(short)(INPUT_DISPLAY_WIDTH + 2 * INPUT_DISPLAY_PADDING), (short)(INPUT_DISPLAY_HEIGHT + 2 * INPUT_DISPLAY_PADDING));
	input_display_rectangle(&layout->move, left, top, INPUT_DISPLAY_BOX_WIDTH, INPUT_DISPLAY_BOX_HEIGHT);
	x = (short)(left + INPUT_DISPLAY_BOX_WIDTH + INPUT_DISPLAY_GAP);
	input_display_rectangle(&layout->look, x, top, INPUT_DISPLAY_BOX_WIDTH, INPUT_DISPLAY_BOX_HEIGHT);
	x = (short)(x + INPUT_DISPLAY_BOX_WIDTH + INPUT_DISPLAY_GAP);
	input_display_rectangle(&layout->bar, x, top, INPUT_DISPLAY_BAR, INPUT_DISPLAY_BOX_HEIGHT);
	x = (short)(x + INPUT_DISPLAY_BAR + INPUT_DISPLAY_GAP);
	input_display_rectangle(&layout->fire, x, top, (short)(right - x), INPUT_DISPLAY_CELL_HEIGHT);
	input_display_rectangle(&layout->keys[_input_display_key_forward], (short)(keys_left + key_step), keys_top,
		INPUT_DISPLAY_KEY_WIDTH, INPUT_DISPLAY_KEY_HEIGHT);
	input_display_rectangle(&layout->keys[_input_display_key_left], keys_left, (short)(keys_top + row_step),
		INPUT_DISPLAY_KEY_WIDTH, INPUT_DISPLAY_KEY_HEIGHT);
	input_display_rectangle(&layout->keys[_input_display_key_backward], (short)(keys_left + key_step),
		(short)(keys_top + row_step), INPUT_DISPLAY_KEY_WIDTH, INPUT_DISPLAY_KEY_HEIGHT);
	input_display_rectangle(&layout->keys[_input_display_key_right], (short)(keys_left + 2 * key_step),
		(short)(keys_top + row_step), INPUT_DISPLAY_KEY_WIDTH, INPUT_DISPLAY_KEY_HEIGHT);
	for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_ACTIONS; index++)
	{
		input_display_rectangle(&layout->cells[index],
			(short)(left + (index % INPUT_DISPLAY_CELLS_A_ROW) * (INPUT_DISPLAY_CELL_WIDTH + INPUT_DISPLAY_GAP)),
			(short)(top + INPUT_DISPLAY_BOX_HEIGHT + INPUT_DISPLAY_GAP +
				(index / INPUT_DISPLAY_CELLS_A_ROW) * (INPUT_DISPLAY_CELL_HEIGHT + INPUT_DISPLAY_GAP)),
			INPUT_DISPLAY_CELL_WIDTH, INPUT_DISPLAY_CELL_HEIGHT);
	}
}

static void input_display_fill(
	rectangle2d const *rectangle,
	pixel32 color)
{
	rectangle2d copy = *rectangle;

	draw_quad(&copy, color);
}

/* a label centred in its rectangle (the text's box a unit higher and two
taller, as the terminal font's capitals sit) */
static void input_display_label(
	long font_index,
	rectangle2d const *rectangle,
	wchar_t const *text,
	boolean lit)
{
	real_argb_color color;
	rectangle2d bounds = *rectangle;

	if (font_index == NONE)
		return;
	color.alpha = 1.0f;
	color.red = color.green = color.blue = lit ? 1.0f : 0.6f;
	bounds.y0 = (short)(bounds.y0 - 1);
	bounds.y1 = (short)(bounds.y1 + 1);
	draw_string_set_draw_mode(font_index, NONE, _input_display_text_center, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

/* a box with a dot at x, y (each -1 to 1, y up) from its middle */
static void input_display_stick(
	rectangle2d const *box,
	real x,
	real y)
{
	rectangle2d line, dot;
	short middle_x = (short)((box->x0 + box->x1) / 2);
	short middle_y = (short)((box->y0 + box->y1) / 2);
	short reach_x = (short)((box->x1 - box->x0) / 2 - INPUT_DISPLAY_DOT / 2 - 1);
	short reach_y = (short)((box->y1 - box->y0) / 2 - INPUT_DISPLAY_DOT / 2 - 1);

	input_display_fill(box, 0x60303030);
	input_display_rectangle(&line, box->x0, middle_y, (short)(box->x1 - box->x0), 1);
	input_display_fill(&line, 0x40FFFFFF);
	input_display_rectangle(&line, middle_x, box->y0, 1, (short)(box->y1 - box->y0));
	input_display_fill(&line, 0x40FFFFFF);
	x = PIN(x, -1.0f, 1.0f);
	y = PIN(y, -1.0f, 1.0f);
	input_display_rectangle(&dot, (short)(middle_x + (short)(x * reach_x) - INPUT_DISPLAY_DOT / 2),
		(short)(middle_y - (short)(y * reach_y) - INPUT_DISPLAY_DOT / 2), INPUT_DISPLAY_DOT, INPUT_DISPLAY_DOT);
	input_display_fill(&dot, 0xFFFFFFFF);
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
	static wchar_t const *const key_names[NUMBER_OF_INPUT_DISPLAY_KEYS] = { L"W", L"A", L"S", L"D" };
	struct input_display_player const *player;
	struct input_display_layout layout;
	short absolute_index;
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

	/* (the view's coordinates, as the match timer's; none in a view too
	small to hold it) */
	input_display_layout(
		(short)(render.camera.window_bounds.x1 - render.camera.viewport_bounds.x0 - INPUT_DISPLAY_RIGHT_MARGIN),
		(short)(render.camera.window_bounds.y1 - render.camera.viewport_bounds.y0 - INPUT_DISPLAY_BOTTOM_MARGIN),
		&layout);
	if (layout.panel.x0 < render.camera.window_bounds.x0 - render.camera.viewport_bounds.x0 ||
		layout.panel.y0 < render.camera.window_bounds.y0 - render.camera.viewport_bounds.y0)
	{
		return;
	}
	input_display_fill(&layout.panel, 0x80000000);

	/* the move, the look and the trigger */
	if (input_display_keyboard_style(local_player_index, halo_keyboard_prompts() != 0, demo_playing()))
	{
		unsigned long lit = input_display_keys_lit(&player->action);

		input_display_fill(&layout.move, 0x60303030);
		for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_KEYS; index++)
		{
			boolean on = (lit & (1UL << index)) != 0;

			input_display_fill(&layout.keys[index], on ? 0xE03C8CDC : 0x60505050);
			input_display_label(font_index, &layout.keys[index], key_names[index], on);
		}
	}
	else
		input_display_stick(&layout.move, -player->action.throttle.j, player->action.throttle.i);
	input_display_stick(&layout.look, -player->turn_yaw / INPUT_DISPLAY_FULL_TURN,
		player->turn_pitch / INPUT_DISPLAY_FULL_TURN);
	input_display_fill(&layout.bar, 0x60303030);
	{
		short pull = (short)(PIN(player->action.primary_trigger, 0.0f, 1.0f) * INPUT_DISPLAY_BOX_HEIGHT + 0.5f);
		rectangle2d filled = layout.bar;

		filled.y0 = (short)(filled.y1 - pull);
		if (pull > 0)
			input_display_fill(&filled, 0xE0DC5A3C);
	}
	input_display_label(font_index, &layout.fire, L"FIRE", player->action.primary_trigger > 0.0f);

	/* the actions held */
	held = input_display_actions_held(&player->action);
	for (index = 0; index < NUMBER_OF_INPUT_DISPLAY_ACTIONS; index++)
	{
		boolean on = (held & (1UL << index)) != 0;

		input_display_fill(&layout.cells[index], on ? 0xE03C8CDC : 0x60303030);
		input_display_label(font_index, &layout.cells[index], input_display_action_names[index], on);
	}
}
