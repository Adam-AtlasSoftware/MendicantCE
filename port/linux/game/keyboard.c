/*
KEYBOARD.C

The on-screen keyboard (keyboard.h), the game's side: a pop-up of the menu
engine's, screens/keyboard.xml, whose <keyboard> block it draws and whose
input it reads, over the screen it is typed for (which waits under it, as it
was: ui_widget.c's pop-ups) or over a game's view (the chat); its rules are
keyboard_core.c's.

- Layout: the player's letters (input.keyboard_layout: QWERTY, AZERTY,
  QWERTZ, the Nordic ones, Spanish and the others of keyboard_core.c's table;
  "auto" the system's languages' layout, else game.language's, else
  QWERTY), each with the number row and a page of signs. A layout's own
  letters with a mark (ñ, ö, å) type their plain letter where the text does
  not take them or the font does not draw them, as the accents are filtered.
  A name's first letter is a capital. Shift, and caps lock (shift
  twice, quickly).
- A controller: the D-pad and the left stick move the focus, A presses the
  key, held it offers the key's accents (move left and right, let go to type
  the one chosen); X deletes (held, again and again), Y is a space, LB and RB
  move the cursor, Start (Menu) is done, B cancels (closes the accents first),
  the left stick's click is shift. The prompts show each with the
  controller's own buttons (draw_string_and_hack_in_icons).
- The pointer: a click or a tap presses a key; held on a letter it offers its
  accents (move onto one, let go); a right click offers them at once (a click
  picks one).
- The computer's keyboard types into it (platform_text_typing): its letters,
  those of its layout with a mark too, Backspace; Enter is done, Esc cancels.
- Accents: Latin-1's and Latin Extended-A's, only those the text takes (a
  name: both; a menu's field, a byte each: Latin-1's; a chat line: none) and
  the keyboard's font draws.
- Phones keep their system's keyboard, as before: the pop-up shows the text
  and its title, and the system's keyboard types.
- Where: the menus' 640 by 480 (the block in the middle; past 4:3 the room
  either side is margins), bigger keys with the Steam Deck's controls, and in
  split screen the player's view (menu_view.c, as a pop-up's).

The original-faithful modes (the classic engine, the Xbox's menus) keep the
game's own keyboard (source/interface/virtual_keyboard.c) and the chat's
(network_chat.c): keyboard_new says which, and every caller asks it through
the functions here.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "input/input.h"
#include "interface/ui_widget.h"
#include "interface/virtual_keyboard.h"
#include "rasterizer/rasterizer.h"
#include "saved games/saved_game_files.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "game/players.h"
#include "controller_buttons.h"
#include "halo_keyboard.h"
#include "halo_ui_pointer.h"
#include "text_filter.h"
#include "keyboard.h"
#include "keyboard_core.h"
#include "menu_widget.h"
#include "../src/port_config.h" /* port: port/linux/src/port_config.c */

#include <string.h>

/* ---------- the game's */

/* menu_screen.c's */
int menu_screen_open_ready(char const *name);
/* port/linux/src/xinput_sdl.c's */
void platform_text_typing(int typing);
void platform_text_field(int typing, int password);
/* ... the system's languages, the most wanted first ("fr_FR de_CH"), and
their count */
int platform_locales(char *locales, int size);
/* menu_shapes.c's */
void menu_shape_rounded_box(real x0, real y0, real x1, real y1, real radius, real edge, pixel32 fill,
	pixel32 color);
void menu_shape_shaded_box(real x0, real y0, real x1, real y1, real radius, real edge, pixel32 top, pixel32 bottom,
	pixel32 color);
void menu_shape_band(real x0, real y0, real x1, real y1, pixel32 top, pixel32 bottom);
void menu_shape_caret(real x, real y0, real y1, real width, pixel32 color);
/* ui_widget.c's */
unsigned long ui_widget_port_milliseconds(void);
struct widget_instance *ui_widget_port_active_screen(short local_player_index);

/* ---------- constants */

/* the menus' widget definitions' tag group */
#define UI_WIDGET_DEFINITION_TAG 'DeLa'

/* the screen's stand-in (screens/blocks.xml) and its file's name */
#define KEYBOARD_SCREEN "keyboard"
#define KEYBOARD_SCREEN_TAG "pc\\screens\\keyboard"

/* the menus' sounds (ui_widget.c's) */
enum
{
	KEYBOARD_SOUND_CURSOR = 1,
	KEYBOARD_SOUND_FORWARD,
	KEYBOARD_SOUND_BACK,
	KEYBOARD_SOUND_FAILURE
};

/* a controller's buttons, as the keyboard reads them */
enum
{
	KEYBOARD_PAD_UP,
	KEYBOARD_PAD_DOWN,
	KEYBOARD_PAD_LEFT,
	KEYBOARD_PAD_RIGHT,
	KEYBOARD_PAD_A,
	KEYBOARD_PAD_B,
	KEYBOARD_PAD_X,
	KEYBOARD_PAD_Y,
	KEYBOARD_PAD_LB,
	KEYBOARD_PAD_RB,
	KEYBOARD_PAD_START,
	KEYBOARD_PAD_BACK,
	KEYBOARD_PAD_L3
};

/* a stick pushed this far is a direction */
#define KEYBOARD_STICK_PUSHED 22000

/* the colors: the screen's dim, the block's panel, its edge, the text's box,
a key's (plain, held on, focused, not offered), the words' */
#define KEYBOARD_DIM 0x99000000UL
#define KEYBOARD_PANEL_TOP 0xF002142AUL
#define KEYBOARD_PANEL_BOTTOM 0xF002070DUL
#define KEYBOARD_EDGE 0xFF31AAFFUL
#define KEYBOARD_TEXT_FILL 0xF0000C1CUL
#define KEYBOARD_KEY_FILL 0xCC0A1E3CUL
#define KEYBOARD_KEY_EDGE 0x802299FFUL
#define KEYBOARD_KEY_ON_FILL 0xE60D3A7AUL
#define KEYBOARD_FOCUS_FILL 0xF0124A96UL
#define KEYBOARD_FOCUS_EDGE 0xFFFFFFFFUL
#define KEYBOARD_CARET 0xFFFFFFFFUL

/* ---------- the keyboard */

static struct
{
	/* one is up, and how the last closed */
	boolean up;
	boolean last_kept;
	struct keyboard_request request;
	struct keyboard_state state;
	/* a name's text, the game's own callers' (keyboard_name_launch) */
	boolean name;
	/* the system's keyboard types (a phone's) */
	boolean system;
	/* bigger keys (the Steam Deck's controls) */
	boolean large;
	struct keyboard_geometry geometry;
	/* the controller: its buttons last frame, those held as it opened (they
	act once let go of), their repeats, and the key held: by A or the
	pointer */
	unsigned long held;
	unsigned long opening;
	struct keyboard_repeat repeat_up, repeat_down, repeat_left, repeat_right, repeat_x, repeat_lb, repeat_rb;
	boolean hold_by_pointer;
	/* the pointer's button (or finger) down, and where */
	boolean pointer_down;
	short pointer_x, pointer_y;
	/* the prompts' buttons as last drawn (a click's) */
	struct keyboard_box cancel_box, done_box;
	/* the fonts */
	long title_font, text_font, small_font;
} keyboard;

/* ---------- which keyboard (the one place it is decided) */

boolean keyboard_new(void)
{
	/* the new menus' (the menu engine's, with the PC version's menus): the
	screen is there only then (menu_screen.c's screen_place_named) */
	return menu_screen_open_ready(KEYBOARD_SCREEN) != 0;
}

/* ---------- what a text takes */

/* a name's: ASCII's visible characters but "|", and the letters with a mark
the game's names keep (players.c's player_name_clean), Latin-1's and Latin
Extended-A's, U+00C0 to U+017F, but for the signs x and / */
static boolean keyboard_name_takes(unsigned short character)
{
	return (character >= ' ' && character <= '~' && character != '|') ||
		(character >= 0xC0 && character <= 0x17F && character != 0xD7 && character != 0xF7);
}

/* a menu's field's, a byte each (menu_functions.c's text_field_character_kept):
Latin-1's letters with a mark alone */
static boolean keyboard_field_takes(unsigned short character)
{
	return (character >= ' ' && character <= '~' && character != '|') ||
		(character >= 0xC0 && character <= 0xFF && character != 0xD7 && character != 0xF7);
}

/* a chat line's (network_chat.c's chat_text_clean): ASCII's visible ones but
"|" */
static boolean keyboard_chat_takes(unsigned short character)
{
	return character >= ' ' && character <= '~' && character != '|';
}

/* (keyboard_core.c's offered) the text takes it and the font draws it */
static int keyboard_offered_character(unsigned short character, void *context)
{
	boolean takes;
	struct font_header *font;

	switch (keyboard.request.kind)
	{
	case KEYBOARD_NAME: takes = keyboard_name_takes(character); break;
	case KEYBOARD_FIELD: takes = keyboard_field_takes(character); break;
	default: takes = keyboard_chat_takes(character); break;
	}
	if (!takes)
		return FALSE;
	if (character == ' ')
		return TRUE;
	font = keyboard.text_font != NONE ? font_definition_get(keyboard.text_font) : NULL;
	return font && font_get_character_by_ascii_code(font, character) != NULL;
}

/* ---------- open and closed */

static void keyboard_fonts(void)
{
	keyboard.text_font = tag_loaded(FONT_GROUP_TAG, "ui\\large_ui");
	keyboard.small_font = tag_loaded(FONT_GROUP_TAG, "ui\\small_ui");
	keyboard.title_font = tag_loaded(FONT_GROUP_TAG, "pc\\heading");
	if (keyboard.small_font == NONE)
		keyboard.small_font = keyboard.text_font;
	if (keyboard.title_font == NONE)
		keyboard.title_font = keyboard.text_font;
}

/* whether the Steam Deck's own controls are the controller's (bigger keys) */
static boolean keyboard_large(short controller)
{
	struct controller_device device;

	controller_device_get((short)(controller == NONE ? 0 : controller), &device);
	return device.family == CONTROLLER_STEAM_DECK;
}

static unsigned long keyboard_pad_read(void);

/* the player's layout (input.keyboard_layout): one by name, or "auto", the
system's languages' (SDL's), else game.language's, else QWERTY */
static struct keyboard_layout const *keyboard_layout_chosen(void)
{
	char const *setting = config_string("input.keyboard_layout");
	char locales[256];

	if (setting[0] && strcmp(setting, "auto") != 0)
		return keyboard_layout_named(setting);
	platform_locales(locales, (int)sizeof(locales));
	return keyboard_layout_auto(locales, config_string("game.language"));
}

boolean keyboard_open(struct keyboard_request const *request)
{
	unsigned short text[KEYBOARD_MAXIMUM_TEXT + 1];
	short length = 0;
	long tag_index;

	if (keyboard.up || !request || !request->text || request->maximum <= 0 || !keyboard_new())
		return FALSE;
	tag_index = tag_loaded(UI_WIDGET_DEFINITION_TAG, KEYBOARD_SCREEN_TAG);
	if (tag_index == NONE)
		return FALSE;
	keyboard_fonts();
	if (keyboard.text_font == NONE)
		return FALSE;
	keyboard.request = *request;
	keyboard.request.maximum = (short)MIN(request->maximum, KEYBOARD_MAXIMUM_TEXT);
	while (request->text[length] && length < keyboard.request.maximum)
	{
		text[length] = (unsigned short)request->text[length];
		length++;
	}
	text[length] = 0;
	keyboard_begin(&keyboard.state, keyboard_layout_chosen(), text, keyboard.request.maximum,
		request->kind == KEYBOARD_NAME, keyboard_offered_character, NULL);
#if defined(HALO_ANDROID) || defined(HALO_IOS)
	keyboard.system = TRUE;
#else
	keyboard.system = FALSE;
#endif
	keyboard.large = keyboard_large(request->controller);
	keyboard_geometry_get(keyboard.large, &keyboard.geometry);
	/* (a phone's: no keys, its prompts under the text, at the screen's top,
	clear of the system's keyboard at its foot) */
	if (keyboard.system)
	{
		struct keyboard_geometry *geometry = &keyboard.geometry;
		short up = (short)(geometry->top - 24);

		geometry->prompts_y = (short)(geometry->text_y + geometry->text_height + 12);
		geometry->height = (short)(geometry->prompts_y + 22 + 16 - geometry->top);
		geometry->top = (short)(geometry->top - up);
		geometry->title_y = (short)(geometry->title_y - up);
		geometry->caption_y = (short)(geometry->caption_y - up);
		geometry->text_y = (short)(geometry->text_y - up);
		geometry->keys_y = (short)(geometry->keys_y - up);
		geometry->prompts_y = (short)(geometry->prompts_y - up);
	}
	memset(&keyboard.repeat_up, 0, sizeof(keyboard.repeat_up));
	keyboard.repeat_down = keyboard.repeat_left = keyboard.repeat_right = keyboard.repeat_x = keyboard.repeat_lb =
		keyboard.repeat_rb = keyboard.repeat_up;
	keyboard.hold_by_pointer = FALSE;
	keyboard.pointer_down = FALSE;
	keyboard.last_kept = FALSE;
	keyboard.up = TRUE;
	/* (what is held as it opens acts once let go of: the A that opened it) */
	keyboard.opening = keyboard_pad_read();
	keyboard.held = keyboard.opening;
	/* (the keys typed before it opened let go of) */
	{
		struct key_stroke key;

		while (input_get_key(&key))
			;
	}
	if (keyboard.system)
		platform_text_field(TRUE, request->masked);
	else
		platform_text_typing(TRUE);
	if (!ui_widget_load_by_name_or_tag(NULL, tag_index, NULL, request->local_player, NONE, NONE, NONE))
	{
		keyboard.up = FALSE;
		platform_text_typing(FALSE);
		platform_text_field(FALSE, FALSE);
		return FALSE;
	}
	ui_play_audio_feedback_sound(KEYBOARD_SOUND_FORWARD);
	error(_error_log, "keyboard: opened (%s)", request->kind == KEYBOARD_NAME ? "a name" :
		request->kind == KEYBOARD_FIELD ? "a field" : "a chat line");
	return TRUE;
}

/* its text, as the caller's text */
static void keyboard_text_out(wchar_t *text)
{
	short index;

	for (index = 0; index <= keyboard.state.length; index++)
		text[index] = (wchar_t)keyboard.state.text[index];
}

/* closed, done (kept) or cancelled: its screen closed, the caller told */
static void keyboard_finish(boolean kept)
{
	struct widget_instance *popup;
	short local_player = keyboard.request.local_player;
	void (*closed)(boolean kept, wchar_t const *text, void *context) = keyboard.request.closed;
	void *context = keyboard.request.context;

	if (!keyboard.up)
		return;
	/* (a name: as the game's keyboard kept one, its spaces before and after
	dropped and one the host's ban command cannot name refused, and the name
	of another saved file refused) */
	if (kept && keyboard.name)
	{
		wchar_t name[KEYBOARD_MAXIMUM_TEXT + 1];

		keyboard_text_out(name);
		if (!player_name_clean(name, keyboard.request.maximum + 1))
			name[0] = 0;
		if (ustrcmp(name, keyboard.request.text) != 0)
		{
			if (!name[0])
			{
				display_error(_error_cannot_create_saved_game_file_with_empty_name, NONE, TRUE, FALSE);
				kept = FALSE;
			}
			else if (!saved_game_file_name_unique(name))
			{
				display_error(_error_already_a_saved_game_file_with_that_name, NONE, TRUE, FALSE);
				kept = FALSE;
			}
		}
		if (kept)
			ustrncpy_terminated(keyboard.request.text, name, keyboard.request.maximum + 1);
	}
	else if (kept)
	{
		keyboard_text_out(keyboard.request.text);
	}
	keyboard.up = FALSE;
	keyboard.last_kept = kept;
	platform_text_typing(FALSE);
	platform_text_field(FALSE, FALSE);
	ui_play_audio_feedback_sound(kept ? KEYBOARD_SOUND_FORWARD : KEYBOARD_SOUND_BACK);
	error(_error_log, "keyboard: %s", kept ? "done" : "cancelled");
	/* (its screen, the pop-up, closed: the screen under it up again) */
	popup = ui_widget_port_popup(local_player);
	if (popup && popup->definition_tag_index == tag_loaded(UI_WIDGET_DEFINITION_TAG, KEYBOARD_SCREEN_TAG))
		ui_widget_delete(popup);
	if (closed)
		closed(kept, keyboard.request.text, context);
}

void keyboard_screen_closed(void)
{
	/* (closed by another than it: every screen closed, the map let go of, the
	screen under it gone) a Cancel */
	if (keyboard.up)
		keyboard_finish(FALSE);
}

/* ---------- either mode's (keyboard_new decides) */

static wchar_t keyboard_title[64], keyboard_caption[128];

boolean keyboard_name_launch(wchar_t *text, word size, short caption_index)
{
	struct keyboard_request request;
	boolean opened;

	if (!keyboard_new())
		return virtual_keyboard_launch(text, size, caption_index);
	memset(&request, 0, sizeof(request));
	request.kind = KEYBOARD_NAME;
	request.local_player = NONE;
	request.controller = NONE;
	request.text = text;
	/* (the game's keyboard's: at most 0x40 bytes, its end among them) */
	request.maximum = (short)(MIN(size, 0x40) / sizeof(wchar_t) - 1);
	ustrncpy_terminated(keyboard_title, L"ENTER NAME", NUMBEROF(keyboard_title));
	ustrncpy_terminated(keyboard_caption, caption_index == 9 ? L"A name for the game type" :
		caption_index == 10 ? L"A new name" : L"A name for your profile", NUMBEROF(keyboard_caption));
	request.title = keyboard_title;
	request.caption = keyboard_caption;
	opened = keyboard_open(&request);
	keyboard.name = opened;
	return opened;
}

boolean keyboard_field_launch(wchar_t *text, word size, wchar_t const *title, wchar_t const *caption)
{
	struct keyboard_request request;
	boolean opened;

	if (!keyboard_new())
		return virtual_keyboard_port_launch(text, size, title, caption);
	memset(&request, 0, sizeof(request));
	request.kind = KEYBOARD_FIELD;
	request.local_player = NONE;
	request.controller = NONE;
	request.text = text;
	request.maximum = (short)(size / sizeof(wchar_t) - 1);
	request.title = title;
	request.caption = caption;
	opened = keyboard_open(&request);
	keyboard.name = FALSE;
	return opened;
}

boolean keyboard_active(void)
{
	return keyboard.up || virtual_keyboard_active();
}

boolean keyboard_last_kept(void)
{
	return keyboard_new() ? keyboard.last_kept : virtual_keyboard_last_exit_saved_text();
}

void keyboard_close(void)
{
	if (keyboard.up)
		keyboard_finish(FALSE);
	if (virtual_keyboard_active())
		virtual_keyboard_close();
}

boolean keyboard_chat_up(void)
{
	return keyboard.up && keyboard.request.kind == KEYBOARD_CHAT;
}

boolean keyboard_over_game(void)
{
	return keyboard.up && !ui_widget_port_active_screen(keyboard.request.local_player);
}

short keyboard_controller(void)
{
	return keyboard.up && keyboard_over_game() ? keyboard.request.controller : NONE;
}

/* ---------- its input */

/* a gamepad's buttons, as the keyboard reads them (the game's reading:
the keyboard's arrows, Enter and Esc on the first controller's, the
headset's, the test scripts'; and the controller's own, which a game keeps
from itself while the chat's keyboard is up: xinput_sdl.c) */
static unsigned long keyboard_pad_of(short gamepad)
{
	struct gamepad_state const *state = input_get_gamepad_state(gamepad);
	unsigned long physical = halo_chat_pad_buttons_for_port(gamepad);
	unsigned long bits = 0;

	if (state)
	{
		static short const buttons[][2] =
		{
			{ _gamepad_binary_button_dpad_up, KEYBOARD_PAD_UP }, { _gamepad_binary_button_dpad_down, KEYBOARD_PAD_DOWN },
			{ _gamepad_binary_button_dpad_left, KEYBOARD_PAD_LEFT },
			{ _gamepad_binary_button_dpad_right, KEYBOARD_PAD_RIGHT },
			{ _gamepad_analog_button_a, KEYBOARD_PAD_A }, { _gamepad_analog_button_b, KEYBOARD_PAD_B },
			{ _gamepad_analog_button_x, KEYBOARD_PAD_X }, { _gamepad_analog_button_y, KEYBOARD_PAD_Y },
			{ _gamepad_analog_button_white, KEYBOARD_PAD_LB }, { _gamepad_analog_button_black, KEYBOARD_PAD_RB },
			{ _gamepad_binary_button_start, KEYBOARD_PAD_START }, { _gamepad_binary_button_back, KEYBOARD_PAD_BACK },
			{ _gamepad_binary_button_left_thumb, KEYBOARD_PAD_L3 },
		};
		short index;

		for (index = 0; index < (short)NUMBEROF(buttons); index++)
		{
			if (state->buttons[buttons[index][0]])
				bits |= FLAG(buttons[index][1]);
		}
		if (state->sticks[_gamepad_stick_left].y > KEYBOARD_STICK_PUSHED) bits |= FLAG(KEYBOARD_PAD_UP);
		if (state->sticks[_gamepad_stick_left].y < -KEYBOARD_STICK_PUSHED) bits |= FLAG(KEYBOARD_PAD_DOWN);
		if (state->sticks[_gamepad_stick_left].x < -KEYBOARD_STICK_PUSHED) bits |= FLAG(KEYBOARD_PAD_LEFT);
		if (state->sticks[_gamepad_stick_left].x > KEYBOARD_STICK_PUSHED) bits |= FLAG(KEYBOARD_PAD_RIGHT);
	}
	if (physical & (FLAG(HALO_CHAT_PAD_UP) | FLAG(HALO_CHAT_PAD_STICK_UP))) bits |= FLAG(KEYBOARD_PAD_UP);
	if (physical & (FLAG(HALO_CHAT_PAD_DOWN) | FLAG(HALO_CHAT_PAD_STICK_DOWN))) bits |= FLAG(KEYBOARD_PAD_DOWN);
	if (physical & (FLAG(HALO_CHAT_PAD_LEFT) | FLAG(HALO_CHAT_PAD_STICK_LEFT))) bits |= FLAG(KEYBOARD_PAD_LEFT);
	if (physical & (FLAG(HALO_CHAT_PAD_RIGHT) | FLAG(HALO_CHAT_PAD_STICK_RIGHT))) bits |= FLAG(KEYBOARD_PAD_RIGHT);
	if (physical & FLAG(HALO_CHAT_PAD_A)) bits |= FLAG(KEYBOARD_PAD_A);
	if (physical & FLAG(HALO_CHAT_PAD_B)) bits |= FLAG(KEYBOARD_PAD_B);
	if (physical & FLAG(HALO_CHAT_PAD_X)) bits |= FLAG(KEYBOARD_PAD_X);
	if (physical & FLAG(HALO_CHAT_PAD_Y)) bits |= FLAG(KEYBOARD_PAD_Y);
	if (physical & FLAG(HALO_CHAT_PAD_LB)) bits |= FLAG(KEYBOARD_PAD_LB);
	if (physical & FLAG(HALO_CHAT_PAD_RB)) bits |= FLAG(KEYBOARD_PAD_RB);
	if (physical & FLAG(HALO_CHAT_PAD_START)) bits |= FLAG(KEYBOARD_PAD_START);
	return bits;
}

/* the controller that types (any, for the menus' own) */
static unsigned long keyboard_pad_read(void)
{
	unsigned long bits = 0;
	short gamepad;

	if (keyboard.request.controller != NONE)
		return keyboard_pad_of(keyboard.request.controller);
	for (gamepad = 0; gamepad < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; gamepad++)
		bits |= keyboard_pad_of(gamepad);
	return bits;
}

/* a key pressed's result heard, done finishing it */
static void keyboard_result(int result)
{
	switch (result)
	{
	case KEYBOARD_TYPED: ui_play_audio_feedback_sound(KEYBOARD_SOUND_FORWARD); break;
	case KEYBOARD_CHANGED: ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR); break;
	case KEYBOARD_REFUSED: ui_play_audio_feedback_sound(KEYBOARD_SOUND_FAILURE); break;
	case KEYBOARD_DONE: keyboard_finish(TRUE); break;
	}
}

/* the computer's keys typed (its layout's letters with a mark, a system
keyboard's text), Backspace; Enter is done where the system's keyboard
types (elsewhere Enter is Start, Esc B: xinput_sdl.c's typing_gamepad) */
static void keyboard_keys(void)
{
	struct key_stroke key;

	while (keyboard.up && input_get_key(&key))
	{
		wchar_t character = input_key_character();

		if (key.key_code == _key_backspace)
			keyboard_result(keyboard_backspace(&keyboard.state) ? KEYBOARD_TYPED : KEYBOARD_REFUSED);
		else if (keyboard.system && (key.key_code == _key_return || key.key_code == _keypad_enter))
			keyboard_finish(TRUE);
		else if ((unsigned short)character >= ' ')
			keyboard_result(keyboard_type(&keyboard.state, (unsigned short)character) ? KEYBOARD_TYPED :
				KEYBOARD_REFUSED);
	}
}

void keyboard_frame(struct widget_instance *screen)
{
	unsigned long now = ui_widget_port_milliseconds();
	unsigned long bits, pressed, held;
	struct keyboard_state *state = &keyboard.state;

	if (!keyboard.up)
	{
		/* (its screen opened by itself, debug.menu_open's: closed) */
		return;
	}
	keyboard_keys();
	if (!keyboard.up)
		return;
	bits = keyboard_pad_read();
	/* (what was held as it opened acts once let go of) */
	keyboard.opening &= bits;
	held = bits & ~keyboard.opening;
	pressed = held & ~keyboard.held;
	keyboard.held = held;
	if (TEST_FLAG(pressed, KEYBOARD_PAD_START))
	{
		keyboard_finish(TRUE);
		return;
	}
	if (TEST_FLAG(pressed, KEYBOARD_PAD_B) || TEST_FLAG(pressed, KEYBOARD_PAD_BACK))
	{
		if (state->accents_open)
		{
			keyboard_accents_close(state);
			ui_play_audio_feedback_sound(KEYBOARD_SOUND_BACK);
		}
		else
			keyboard_finish(FALSE);
		return;
	}
	if (keyboard.system)
		return;
	/* (the accents open: left and right choose; A let go of types the one
	chosen, or A pressed, where a right click opened them) */
	if (state->accents_open)
	{
		if (keyboard_repeat_step(&keyboard.repeat_left, TEST_FLAG(held, KEYBOARD_PAD_LEFT), now))
			keyboard_accents_step(state, -1);
		if (keyboard_repeat_step(&keyboard.repeat_right, TEST_FLAG(held, KEYBOARD_PAD_RIGHT), now))
			keyboard_accents_step(state, 1);
		if (state->accents_sticky ? TEST_FLAG(pressed, KEYBOARD_PAD_A) :
			(!keyboard.hold_by_pointer && !TEST_FLAG(held, KEYBOARD_PAD_A)))
		{
			keyboard_result(keyboard_accents_pick(state) ? KEYBOARD_TYPED : KEYBOARD_REFUSED);
		}
		return;
	}
	if (keyboard_repeat_step(&keyboard.repeat_up, TEST_FLAG(held, KEYBOARD_PAD_UP), now))
		keyboard_move(state, 0, -1);
	if (keyboard_repeat_step(&keyboard.repeat_down, TEST_FLAG(held, KEYBOARD_PAD_DOWN), now))
		keyboard_move(state, 0, 1);
	if (keyboard_repeat_step(&keyboard.repeat_left, TEST_FLAG(held, KEYBOARD_PAD_LEFT), now))
		keyboard_move(state, -1, 0);
	if (keyboard_repeat_step(&keyboard.repeat_right, TEST_FLAG(held, KEYBOARD_PAD_RIGHT), now))
		keyboard_move(state, 1, 0);
	if (TEST_FLAG(pressed, KEYBOARD_PAD_A))
	{
		keyboard.hold_by_pointer = FALSE;
		keyboard_result(keyboard_press(state, now));
		if (!keyboard.up)
			return;
	}
	if (state->holding && !keyboard.hold_by_pointer)
	{
		if (TEST_FLAG(held, KEYBOARD_PAD_A))
		{
			if (keyboard_hold(state, now))
				ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR);
		}
		else
			keyboard_release(state);
	}
	if (keyboard_repeat_step(&keyboard.repeat_x, TEST_FLAG(held, KEYBOARD_PAD_X), now))
		keyboard_result(keyboard_backspace(state) ? KEYBOARD_TYPED : KEYBOARD_REFUSED);
	if (TEST_FLAG(pressed, KEYBOARD_PAD_Y))
		keyboard_result(keyboard_type(state, ' ') ? KEYBOARD_TYPED : KEYBOARD_REFUSED);
	if (keyboard_repeat_step(&keyboard.repeat_lb, TEST_FLAG(held, KEYBOARD_PAD_LB), now) &&
		keyboard_cursor(state, -1))
	{
		ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR);
	}
	if (keyboard_repeat_step(&keyboard.repeat_rb, TEST_FLAG(held, KEYBOARD_PAD_RB), now) &&
		keyboard_cursor(state, 1))
	{
		ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR);
	}
	if (TEST_FLAG(pressed, KEYBOARD_PAD_L3))
	{
		keyboard_shift(state, now);
		keyboard_result(KEYBOARD_CHANGED);
	}
	/* (a key held by the pointer offers its accents) */
	if (state->holding && keyboard.hold_by_pointer && keyboard.pointer_down && keyboard_hold(state, now))
		ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR);
}

int keyboard_event(struct event_record const *event)
{
	/* (every event of its screen is its own: it reads the controller each
	frame, keyboard_frame) */
	return keyboard.up;
}

static boolean keyboard_in(struct keyboard_box const *box, short x, short y)
{
	return x >= box->x0 && x < box->x1 && y >= box->y0 && y < box->y1;
}

/* the pointer pressed at a point: an accent a right click opened, the
prompts' CANCEL and DONE, a key */
static void keyboard_pointer_press(short x, short y)
{
	struct keyboard_state *state = &keyboard.state;
	short row, column;

	if (state->accents_open && state->accents_sticky)
	{
		short accent = keyboard_accent_find(state, &keyboard.geometry, x, y);

		if (accent != NONE)
		{
			state->accent_chosen = accent;
			keyboard_result(keyboard_accents_pick(state) ? KEYBOARD_TYPED : KEYBOARD_REFUSED);
		}
		else
			keyboard_accents_close(state);
		return;
	}
	if (keyboard_in(&keyboard.cancel_box, x, y))
	{
		keyboard_finish(FALSE);
		return;
	}
	if (keyboard_in(&keyboard.done_box, x, y))
	{
		keyboard_finish(TRUE);
		return;
	}
	if (keyboard.system || !keyboard_key_find(state, &keyboard.geometry, x, y, &row, &column))
		return;
	state->row = row;
	state->column = column;
	keyboard.hold_by_pointer = TRUE;
	keyboard_result(keyboard_press(state, ui_widget_port_milliseconds()));
}

static void keyboard_pointer_release(void)
{
	struct keyboard_state *state = &keyboard.state;

	if (!keyboard.hold_by_pointer)
		return;
	if (state->accents_open && !state->accents_sticky)
	{
		short accent = keyboard_accent_find(state, &keyboard.geometry, keyboard.pointer_x, keyboard.pointer_y);

		/* (let go of off them: none typed) */
		if (accent == NONE)
		{
			keyboard_accents_close(state);
			return;
		}
		state->accent_chosen = accent;
	}
	if (keyboard_release(state))
		ui_play_audio_feedback_sound(KEYBOARD_SOUND_FORWARD);
	keyboard.hold_by_pointer = FALSE;
}

void keyboard_pointer(struct halo_ui_pointer const *pointer, boolean active)
{
	boolean touch = pointer->touch != 0;

	if (!keyboard.up)
		return;
	if (!active)
	{
		keyboard.pointer_down = FALSE;
		return;
	}
	keyboard.pointer_x = pointer->x;
	keyboard.pointer_y = pointer->y;
	if (!keyboard.pointer_down && (touch ? pointer->downs != 0 : pointer->left_held != 0))
	{
		keyboard.pointer_down = TRUE;
		keyboard_pointer_press(touch ? pointer->down_x : pointer->x, touch ? pointer->down_y : pointer->y);
	}
	else if (!keyboard.pointer_down && pointer->left_clicks)
	{
		/* (a click let go of before it was seen held) */
		keyboard_pointer_press(pointer->click_x, pointer->click_y);
		keyboard.pointer_x = pointer->click_x;
		keyboard.pointer_y = pointer->click_y;
		if (keyboard.up)
			keyboard_pointer_release();
		return;
	}
	if (!keyboard.up)
		return;
	if (keyboard.pointer_down && keyboard.state.accents_open && !keyboard.state.accents_sticky)
	{
		short accent = keyboard_accent_find(&keyboard.state, &keyboard.geometry, pointer->x, pointer->y);

		if (accent != NONE)
			keyboard.state.accent_chosen = accent;
	}
	if (keyboard.pointer_down && (touch ? pointer->left_clicks != 0 : !pointer->left_held))
	{
		keyboard.pointer_down = FALSE;
		keyboard_pointer_release();
	}
	/* (a right click on a letter: its accents, at once) */
	if (keyboard.up && pointer->right_clicks && !keyboard.system)
	{
		short row, column;

		if (keyboard_key_find(&keyboard.state, &keyboard.geometry, pointer->x, pointer->y, &row, &column) &&
			keyboard_accents_open(&keyboard.state, row, column))
		{
			ui_play_audio_feedback_sound(KEYBOARD_SOUND_CURSOR);
		}
	}
}

/* ---------- drawn */

static void keyboard_color(pixel32 argb, real_argb_color *color)
{
	color->alpha = ((argb >> 24) & 0xFF) / 255.0f;
	color->red = ((argb >> 16) & 0xFF) / 255.0f;
	color->green = ((argb >> 8) & 0xFF) / 255.0f;
	color->blue = (argb & 0xFF) / 255.0f;
}

/* words drawn in a box (justification: 0 at its left, 2 in its middle),
the font's line in the middle of the box's height */
static void keyboard_words(long font_index, pixel32 argb, short justification, real x0, real y0, real x1, real y1,
	wchar_t const *words, boolean icons)
{
	struct font_header *font = font_definition_get(font_index);
	real_argb_color color;
	rectangle2d bounds;
	short line = font ? (short)(font->ascending_height + font->descending_height) : 16;

	keyboard_color(argb, &color);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, &color);
	bounds.x0 = (short)x0;
	bounds.x1 = (short)x1;
	bounds.y0 = (short)(y0 + ((y1 - y0) - line) / 2);
	bounds.y1 = (short)(bounds.y0 + line + 4);
	if (icons)
		draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, words, FALSE);
	else
		rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, words);
}

/* a text's width in a font */
static short keyboard_width(long font_index, wchar_t const *words)
{
	rectangle2d bounds = { 0, 0, 0x3FFF, 0x3FFF }, text_bounds, cursor_bounds;
	real_argb_color color = { 1.0f, 1.0f, 1.0f, 1.0f };

	if (!words[0])
		return 0;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
	draw_unicode_string_compute_bounds(&bounds, words, &text_bounds, &cursor_bounds);
	return (short)(cursor_bounds.x0 - bounds.x0);
}

/* the text typed in its box: from its start, or as far along as keeps the
cursor in it; the cursor a caret, blinking */
static void keyboard_draw_text(real x, real y)
{
	struct keyboard_geometry const *geometry = &keyboard.geometry;
	wchar_t shown[KEYBOARD_MAXIMUM_TEXT + 1], before[KEYBOARD_MAXIMUM_TEXT + 1];
	real x0 = x + geometry->keys_x, x1 = x + geometry->left + geometry->width - (geometry->keys_x - geometry->left);
	real y0 = y + geometry->text_y, y1 = y0 + geometry->text_height;
	short index, cursor_x, scroll = 0, room = (short)(x1 - x0 - 16);
	real_argb_color color;
	rectangle2d bounds, clip;
	struct font_header *font = font_definition_get(keyboard.text_font);
	short line = font ? (short)(font->ascending_height + font->descending_height) : 16;

	menu_shape_rounded_box(x0, y0, x1, y1, 6.0f, 2.0f, KEYBOARD_TEXT_FILL, KEYBOARD_EDGE);
	for (index = 0; index < keyboard.state.length; index++)
		shown[index] = keyboard.request.masked ? L'*' : (wchar_t)keyboard.state.text[index];
	shown[keyboard.state.length] = 0;
	ustrncpy_terminated(before, shown, keyboard.state.cursor + 1);
	cursor_x = keyboard_width(keyboard.text_font, before);
	if (cursor_x > room)
		scroll = (short)(cursor_x - room);
	keyboard_color(0xFFFFFFFFUL, &color);
	draw_string_set_draw_mode(keyboard.text_font, NONE, 0, 0, &color);
	bounds.x0 = (short)(x0 + 8 - scroll);
	bounds.x1 = (short)(bounds.x0 + 0x3FFF);
	bounds.y0 = (short)(y0 + (geometry->text_height - line) / 2);
	bounds.y1 = (short)(bounds.y0 + line + 4);
	clip.x0 = (short)(x0 + 4);
	clip.x1 = (short)(x1 - 4);
	clip.y0 = (short)y0;
	clip.y1 = (short)y1;
	/* (what is typed shown as it is, not hidden by streamer mode) */
	text_filter_suspend();
	rasterizer_draw_unicode_string(&bounds, &clip, NULL, 0, shown);
	text_filter_resume();
	if ((ui_widget_port_milliseconds() / 500) & 1)
		return;
	menu_shape_caret(x0 + 8 - scroll + cursor_x + 1, (real)bounds.y0 + 1, (real)bounds.y0 + line - 1, 2.0f,
		KEYBOARD_CARET);
}

/* a key's words: its character, or its own (shift's CAPS while caps lock is on) */
static void keyboard_key_words(struct keyboard_key const *key, wchar_t *words, short size)
{
	char const *label = key->label;
	short index;

	if (key->kind == KEYBOARD_KEY_CHARACTER)
	{
		words[0] = (wchar_t)keyboard_key_character(&keyboard.state, key);
		words[1] = 0;
		return;
	}
	if (key->kind == KEYBOARD_KEY_SHIFT && keyboard.state.caps)
		label = "CAPS";
	for (index = 0; label && label[index] && index < size - 1; index++)
		words[index] = (wchar_t)(unsigned char)label[index];
	words[index] = 0;
}

static void keyboard_draw_keys(real x, real y)
{
	struct keyboard_state const *state = &keyboard.state;
	short row, column;

	for (row = 0; row < KEYBOARD_ROWS; row++)
	{
		for (column = 0; keyboard_key_at(state, row, column); column++)
		{
			struct keyboard_key const *key = keyboard_key_at(state, row, column);
			struct keyboard_box box;
			boolean focused = row == state->row && column == state->column;
			boolean on = key->kind == KEYBOARD_KEY_SHIFT && (state->shift || state->caps);
			boolean offered = key->kind != KEYBOARD_KEY_CHARACTER ||
				keyboard_offered(state, keyboard_key_character(state, key));
			wchar_t words[16];

			if (!keyboard_key_box(state, &keyboard.geometry, row, column, &box))
				continue;
			menu_shape_rounded_box(x + box.x0 + 1, y + box.y0, x + box.x1 - 1, y + box.y1, 6.0f, focused ? 2.5f : 1.5f,
				focused ? KEYBOARD_FOCUS_FILL : on ? KEYBOARD_KEY_ON_FILL : KEYBOARD_KEY_FILL,
				focused ? KEYBOARD_FOCUS_EDGE : KEYBOARD_KEY_EDGE);
			keyboard_key_words(key, words, NUMBEROF(words));
			keyboard_words(key->kind == KEYBOARD_KEY_CHARACTER ? keyboard.text_font : keyboard.small_font,
				!offered ? 0x60FFFFFFUL : focused ? 0xFFFFFFFFUL : 0xFFCFE3FFUL, 2, x + box.x0, y + box.y0, x + box.x1,
				y + box.y1, words, FALSE);
		}
	}
	/* (a key's accents, over it) */
	if (state->accents_open)
	{
		short index;

		for (index = 0; index < state->accent_count; index++)
		{
			struct keyboard_box box;
			wchar_t words[2];
			boolean chosen = index == state->accent_chosen;

			if (!keyboard_accent_box(state, &keyboard.geometry, index, &box))
				continue;
			menu_shape_rounded_box(x + box.x0, y + box.y0, x + box.x1, y + box.y1, 6.0f, 2.0f,
				chosen ? KEYBOARD_FOCUS_FILL : 0xF8061C3AUL, chosen ? KEYBOARD_FOCUS_EDGE : KEYBOARD_EDGE);
			words[0] = (wchar_t)state->accents[index];
			words[1] = 0;
			keyboard_words(keyboard.text_font, 0xFFFFFFFFUL, 2, x + box.x0, y + box.y0, x + box.x1, y + box.y1, words,
				FALSE);
		}
	}
}

/* the prompts: what the controller's buttons (or the keys) do at the left,
CANCEL and DONE at the right, each a button a click presses */
static void keyboard_draw_prompts(real x, real y)
{
	struct keyboard_geometry const *geometry = &keyboard.geometry;
	boolean keys = halo_keyboard_prompts() != 0 || keyboard.system;
	short top = geometry->prompts_y, bottom = (short)(geometry->prompts_y + 22);
	short right = (short)(geometry->left + geometry->width - (geometry->keys_x - geometry->left));
	/* (the help, its shorter forms after it: the first that fits its room is
	shown, measured with its buttons' codes as words, wider than their
	pictures, so it fits whatever their family) */
	static wchar_t const *const system_help[] = { L"Type with your keyboard", L"" };
	static wchar_t const *const keys_help[] =
		{ L"Hold or right-click a letter for accents", L"Right-click a letter for accents", L"" };
	static wchar_t const *const pad_help[] =
	{
		L"%x-button DELETE  %y-button SPACE  %white-button %black-button CURSOR  HOLD %a-button ACCENTS",
		L"%x-button DELETE  %y-button SPACE  %white-button %black-button CURSOR",
		L"%x-button DELETE  %y-button SPACE",
		L"",
	};
	wchar_t const *const *helps = keyboard.system ? system_help : keys ? keys_help : pad_help;
	wchar_t const *help;
	short width = (short)(keyboard.large ? 112 : 100);
	short room;

	keyboard.done_box.x1 = right;
	keyboard.done_box.x0 = (short)(right - width);
	keyboard.cancel_box.x1 = (short)(keyboard.done_box.x0 - 8);
	keyboard.cancel_box.x0 = (short)(keyboard.cancel_box.x1 - width);
	keyboard.done_box.y0 = keyboard.cancel_box.y0 = top;
	keyboard.done_box.y1 = keyboard.cancel_box.y1 = bottom;
	room = (short)(keyboard.cancel_box.x0 - 8 - geometry->keys_x);
	for (help = *helps; help[0] && keyboard_width(keyboard.small_font, help) > room; help = *++helps)
		;
	keyboard_words(keyboard.small_font, 0xFFCFE3FFUL, 0, x + geometry->keys_x, y + top,
		x + keyboard.cancel_box.x0 - 8, y + bottom, help, !keys);
	menu_shape_rounded_box(x + keyboard.cancel_box.x0, y + top, x + keyboard.cancel_box.x1, y + bottom, 6.0f, 1.5f,
		KEYBOARD_KEY_FILL, KEYBOARD_KEY_EDGE);
	menu_shape_rounded_box(x + keyboard.done_box.x0, y + top, x + keyboard.done_box.x1, y + bottom, 6.0f, 1.5f,
		KEYBOARD_KEY_FILL, KEYBOARD_KEY_EDGE);
	keyboard_words(keyboard.small_font, 0xFFFFFFFFUL, 2, x + keyboard.cancel_box.x0, y + top,
		x + keyboard.cancel_box.x1, y + bottom, keys ? L"CANCEL" : L"%b-button CANCEL", !keys);
	keyboard_words(keyboard.small_font, 0xFFFFFFFFUL, 2, x + keyboard.done_box.x0, y + top, x + keyboard.done_box.x1,
		y + bottom, keys ? L"DONE" : L"%start-button DONE", !keys);
}

void keyboard_draw(real x, real y)
{
	struct keyboard_geometry const *geometry = &keyboard.geometry;
	real left = x + geometry->left, top = y + geometry->top;
	real right = left + geometry->width, bottom = top + geometry->height;

	if (!keyboard.up || keyboard.text_font == NONE)
		return;
	/* (the screen or the game under it dimmed, past the menus' 640 on a wide
	screen too) */
	menu_shape_band(x - 2000.0f, y - 1000.0f, x + 2640.0f, y + 1480.0f, KEYBOARD_DIM, KEYBOARD_DIM);
	menu_shape_shaded_box(left, top, right, bottom, 9.5f, 2.0f, KEYBOARD_PANEL_TOP, KEYBOARD_PANEL_BOTTOM,
		KEYBOARD_EDGE);
	keyboard_words(keyboard.title_font, 0xFF2995FDUL, 0, x + geometry->keys_x, y + geometry->title_y,
		right - 16, y + geometry->title_y + 26, keyboard.request.title ? keyboard.request.title : L"", FALSE);
	if (keyboard.request.caption)
	{
		keyboard_words(keyboard.small_font, 0xFF8DB6E6UL, 0, x + geometry->keys_x, y + geometry->caption_y,
			right - 16, y + geometry->caption_y + 18, keyboard.request.caption, FALSE);
	}
	keyboard_draw_text(x, y);
	if (!keyboard.system)
		keyboard_draw_keys(x, y);
	keyboard_draw_prompts(x, y);
}
