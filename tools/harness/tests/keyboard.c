/*
KEYBOARD.C (test)

The on-screen keyboard (port/linux/game/keyboard_core.c, its rules, and
keyboard.c's controller and the mode's keyboard): the layout table and its
pages, the accents a held key offers (only those the text takes and the font
draws), the controller's shortcuts (X deletes and repeats, Y is a space, LB
and RB the cursor, Start done, B cancel), a name, a field and a chat line
typed through it, the fit at each aspect and in split screen, and which
keyboard each mode has. test_keyboard.py takes the game's code.
*/

#include "harness.h"

#include <wchar.h>

typedef unsigned short word;
typedef unsigned long pixel32;
typedef struct { real alpha, red, green, blue; } real_argb_color;
struct widget_instance;
struct event_record;
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define _error_log 0
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4

static char const *case_name;

#include "keyboard_core.h"
#include "keyboard_core.inc"

/* keyboard.h's request (the test's own copy of it: keyboard_request.inc) */
#include "keyboard_request.inc"

/* ---------- the game, as keyboard.c's code under test sees it */

static unsigned long now_ms;
static unsigned long pad_bits;
static int finished = -1;
static boolean mode_new = TRUE;
static int new_opened, old_opened;
static int sounds;
/* (the font draws all but this) */
static unsigned short font_lacks;
/* (the layout a keyboard opens with: input.keyboard_layout's) */
static char const *layout_name = "qwerty";

static unsigned long ui_widget_port_milliseconds(void) { return now_ms; }
static void ui_play_audio_feedback_sound(short sound) { sounds++; }
static void error(int code, char const *format, ...) { }

static boolean virtual_keyboard_launch(wchar_t *text, word size, short caption_index) { old_opened++; return TRUE; }
static boolean virtual_keyboard_port_launch(wchar_t *text, word size, wchar_t const *title, wchar_t const *caption)
{
	old_opened++;
	return TRUE;
}
static boolean virtual_keyboard_active(void) { return FALSE; }
static boolean virtual_keyboard_last_exit_saved_text(void) { return FALSE; }
static void virtual_keyboard_close(void) { }
static void ustrncpy_terminated(wchar_t *destination, wchar_t const *source, unsigned long size)
{
	wcsncpy(destination, source, size);
	destination[size - 1] = 0;
}

#include "keyboard_glue.inc"

static boolean keyboard_new(void) { return mode_new; }

static int keyboard_offered_character(unsigned short character, void *context)
{
	boolean takes = keyboard.request.kind == KEYBOARD_NAME ? keyboard_name_takes(character) :
		keyboard.request.kind == KEYBOARD_FIELD ? keyboard_field_takes(character) : keyboard_chat_takes(character);

	return takes && character != font_lacks;
}

static boolean keyboard_open(struct keyboard_request const *request)
{
	unsigned short text[KEYBOARD_MAXIMUM_TEXT + 1];
	short length = 0;

	keyboard.request = *request;
	while (request->text[length] && length < request->maximum)
	{
		text[length] = (unsigned short)request->text[length];
		length++;
	}
	text[length] = 0;
	keyboard_begin(&keyboard.state, keyboard_layout_named(layout_name), text, request->maximum,
		request->kind == KEYBOARD_NAME, keyboard_offered_character, NULL);
	keyboard_geometry_get(FALSE, &keyboard.geometry);
	keyboard.up = TRUE;
	keyboard.held = keyboard.opening = 0;
	new_opened++;
	return TRUE;
}

static void keyboard_finish(boolean kept)
{
	keyboard.up = FALSE;
	keyboard.last_kept = kept;
	finished = kept;
}

static void keyboard_keys(void) { }
static unsigned long keyboard_pad_read(void) { return pad_bits; }

/* a frame at a time, ms apart, with buttons held */
static void frames(unsigned long held, int count, unsigned long ms)
{
	while (count-- > 0)
	{
		pad_bits = held;
		keyboard_frame(NULL);
		now_ms += ms;
	}
}

static void open_kind(short kind, wchar_t *text, short maximum)
{
	struct keyboard_request request;

	memset(&request, 0, sizeof(request));
	request.kind = kind;
	request.text = text;
	request.maximum = maximum;
	finished = -1;
	keyboard_open(&request);
}

/* the focus put on a character's key, on the page shown */
static void focus_on(unsigned short character)
{
	short row, column;

	for (row = 0; row < KEYBOARD_ROWS; row++)
	{
		for (column = 0; keyboard_key_at(&keyboard.state, row, column); column++)
		{
			struct keyboard_key const *key = keyboard_key_at(&keyboard.state, row, column);

			if (key->kind == KEYBOARD_KEY_CHARACTER && (key->character == character || key->shifted == character))
			{
				keyboard.state.row = row;
				keyboard.state.column = column;
				return;
			}
		}
	}
	CHECK(0, "no key for %04X", character);
}

static boolean text_is(char const *expected)
{
	short index;

	for (index = 0; expected[index]; index++)
	{
		if (keyboard.state.text[index] != (unsigned char)expected[index])
			return FALSE;
	}
	return keyboard.state.length == index;
}

#define A FLAG(KEYBOARD_PAD_A)
#define B FLAG(KEYBOARD_PAD_B)
#define X FLAG(KEYBOARD_PAD_X)
#define Y FLAG(KEYBOARD_PAD_Y)
#define LB FLAG(KEYBOARD_PAD_LB)
#define RB FLAG(KEYBOARD_PAD_RB)
#define START FLAG(KEYBOARD_PAD_START)
#define RIGHT FLAG(KEYBOARD_PAD_RIGHT)

int main(int argc, char **argv)
{
	CHECK(argc == 2, "a case");
	case_name = argv[1];
	CASE("layout")
	{
		/* (QWERTY with its number row; each row within its width; every
		visible ASCII character but "|" on a key once; the pages' keys) */
		struct keyboard_layout const *layout = keyboard_layout_named("qwerty");
		int seen[128] = { 0 };
		short page, row, column, character;
		static char const qwerty[] = "qwertyuiop";

		CHECK(layout == &keyboard_layouts[0] && keyboard_layout_named("none") == layout, "the layouts by name");
		for (column = 0; column < 10; column++)
		{
			CHECK(layout->pages[0].rows[0].keys[column].character == "1234567890"[column], "the number row");
			CHECK(layout->pages[0].rows[1].keys[column].character == qwerty[column] &&
				layout->pages[0].rows[1].keys[column].shifted == qwerty[column] - 'a' + 'A', "QWERTY's row");
		}
		CHECK(layout->pages[0].rows[2].keys[0].character == 'a' && layout->pages[0].rows[3].keys[1].character == 'z',
			"the letters' rows");
		for (page = 0; page < KEYBOARD_PAGES; page++)
		{
			int done = 0, pages = 0, backspaces = 0, spaces = 0;

			for (row = 0; row < KEYBOARD_ROWS; row++)
			{
				struct keyboard_row const *keys = &layout->pages[page].rows[row];
				int width = keys->indent;

				CHECK(keys->count > 0 && keys->count <= KEYBOARD_MAXIMUM_ROW_KEYS, "a row's keys");
				for (column = 0; column < keys->count; column++)
				{
					struct keyboard_key const *key = &keys->keys[column];

					width += key->width;
					done += key->kind == KEYBOARD_KEY_DONE;
					pages += key->kind == KEYBOARD_KEY_PAGE && key->page != page;
					backspaces += key->kind == KEYBOARD_KEY_BACKSPACE;
					spaces += key->kind == KEYBOARD_KEY_SPACE;
					if (key->kind == KEYBOARD_KEY_CHARACTER && key->character < 128 && (page == 1 || row > 0))
						seen[key->character]++;
					if (key->kind == KEYBOARD_KEY_CHARACTER && key->shifted != key->character && key->shifted < 128)
						seen[key->shifted]++;
				}
				CHECK(width <= KEYBOARD_ROW_WIDTH, "page %d row %d is %d wide", page, row, width);
			}
			/* (DONE is the prompts' alone, with Start: no key of it as well) */
			CHECK(done == 0 && pages == 1 && backspaces == 1 && spaces == 1, "page %d's own keys", page);
		}
		for (character = '!'; character <= '~'; character++)
		{
			if (character != '|' && character != ',' && character != '.')
				CHECK(seen[character] == 1, "'%c' on %d keys", character, seen[character]);
		}
		CHECK(!seen['|'], "a key types \"|\"");
		return 0;
	}
	CASE("paging")
	{
		wchar_t text[16] = { 0 };

		open_kind(KEYBOARD_FIELD, text, 15);
		/* (the page key: the signs, the focus on their first; ABC back) */
		keyboard.state.row = 4;
		keyboard.state.column = 0;
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_CHANGED && keyboard.state.page == 1, "?123");
		CHECK(keyboard.state.row == 1 && keyboard.state.column == 0 &&
			keyboard_key_character(&keyboard.state, keyboard_key_at(&keyboard.state, 1, 0)) == '!', "the signs");
		keyboard.state.row = 4;
		keyboard.state.column = 0;
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_CHANGED && keyboard.state.page == 0, "ABC");
		/* (down from SPACE to... up: the nearest key; round the rows) */
		keyboard.state.row = 3;
		keyboard.state.column = 4;
		keyboard_move(&keyboard.state, 0, 1);
		CHECK(keyboard.state.row == 4 && keyboard.state.column == 2, "down to SPACE: %d", keyboard.state.column);
		keyboard_move(&keyboard.state, 0, 1);
		CHECK(keyboard.state.row == 0 && keyboard.state.column == 5, "round the rows, under SPACE: %d",
			keyboard.state.column);
		keyboard.state.column = 0;
		keyboard_move(&keyboard.state, -1, 0);
		CHECK(keyboard.state.column == 9, "round a row");
		return 0;
	}
	CASE("accents")
	{
		wchar_t name[12] = { 0 }, field[32] = { 0 }, chat[64] = { 0 };

		/* (a name: Latin-1's and Latin Extended-A's; held A opens them, the
		letter typed taken back; right and A let go of types the one chosen) */
		open_kind(KEYBOARD_NAME, name, 11);
		keyboard.state.shift = FALSE;
		focus_on('e');
		frames(A, 1, 100);
		CHECK(text_is("e"), "A typed e at once");
		frames(A, 5, 100);
		CHECK(keyboard.state.accents_open && keyboard.state.length == 0, "held A: the accents");
		{
			int index, acute = 0, caron = 0;

			for (index = 0; index < keyboard.state.accent_count; index++)
			{
				acute |= keyboard.state.accents[index] == 0xE9;
				caron |= keyboard.state.accents[index] == 0x11B;
			}
			CHECK(keyboard.state.accents[0] == 'e' && acute && caron, "a name's accents of e");
		}
		frames(A | RIGHT, 1, 50);
		frames(A, 1, 50);
		frames(0, 1, 50);
		CHECK(!keyboard.state.accents_open && keyboard.state.length == 1 && keyboard.state.text[0] == 0xE8,
			"let go of: %04X", keyboard.state.text[0]);
		/* (a field: a byte each, Latin-1's alone) */
		open_kind(KEYBOARD_FIELD, field, 31);
		focus_on('e');
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && keyboard_hold(&keyboard.state, 1000),
			"a field's accents");
		{
			int index;

			for (index = 0; index < keyboard.state.accent_count; index++)
				CHECK(keyboard.state.accents[index] <= 0xFF, "a field offered %04X", keyboard.state.accents[index]);
		}
		CHECK(!keyboard_type(&keyboard.state, 0x142), "a field took a letter past a byte");
		/* (a glyph the font cannot draw: not offered) */
		font_lacks = 0xE9;
		keyboard_accents_close(&keyboard.state);
		CHECK(keyboard_accents_open(&keyboard.state, keyboard.state.row, keyboard.state.column), "opened again");
		{
			int index;

			for (index = 0; index < keyboard.state.accent_count; index++)
				CHECK(keyboard.state.accents[index] != 0xE9, "a glyph the font lacks offered");
		}
		font_lacks = 0;
		/* (a chat line: ASCII, none; held, the letter stays) */
		open_kind(KEYBOARD_CHAT, chat, 63);
		focus_on('e');
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && !keyboard_hold(&keyboard.state, 1000) &&
			text_is("e"), "a chat line's accents");
		/* (a capital's: the capitals') */
		open_kind(KEYBOARD_NAME, name, 11);
		focus_on('e');
		CHECK(keyboard.state.shift && keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED &&
			keyboard_hold(&keyboard.state, 1000) && keyboard.state.accents[0] == 'E', "a capital's accents");
		keyboard_accents_step(&keyboard.state, 2);
		CHECK(keyboard_release(&keyboard.state) && keyboard.state.text[0] == 0xC9, "É: %04X", keyboard.state.text[0]);
		return 0;
	}
	CASE("shortcuts")
	{
		wchar_t text[32] = { 'a', 'b', 'c', 0 };
		int count;

		open_kind(KEYBOARD_FIELD, text, 31);
		/* (X: one, then held again after a while, then quickly) */
		frames(X, 1, 0);
		CHECK(text_is("ab"), "X deletes");
		frames(X, 1, KEYBOARD_REPEAT_FIRST_MILLISECONDS - 10);
		CHECK(text_is("ab"), "X repeated at once");
		frames(X, 1, 20);
		frames(X, 1, 0);
		CHECK(text_is("a"), "X held did not repeat: %d", keyboard.state.length);
		frames(X, 1, KEYBOARD_REPEAT_MILLISECONDS - 10);
		frames(X, 1, 10);
		CHECK(text_is("a"), "X repeated before its repeat's time");
		frames(X, 1, 0);
		CHECK(keyboard.state.length == 0, "X held did not repeat again: %d", keyboard.state.length);
		frames(0, 1, 10);
		/* (Y: a space; LB and RB: the cursor) */
		keyboard_type(&keyboard.state, 'a');
		keyboard_type(&keyboard.state, 'b');
		frames(Y, 1, 10);
		frames(0, 1, 10);
		CHECK(text_is("ab ") && keyboard.state.cursor == 3, "Y");
		frames(LB, 1, 10);
		frames(0, 1, 10);
		frames(LB, 1, 10);
		frames(0, 1, 10);
		CHECK(keyboard.state.cursor == 1, "LB: %d", keyboard.state.cursor);
		frames(RB, 1, 10);
		frames(0, 1, 10);
		CHECK(keyboard.state.cursor == 2, "RB: %d", keyboard.state.cursor);
		/* (Start: done; B: cancel, but the accents first) */
		frames(START, 1, 10);
		CHECK(finished == TRUE, "Start");
		open_kind(KEYBOARD_NAME, text, 11);
		focus_on('a');
		frames(A, 6, 100);
		CHECK(keyboard.state.accents_open, "held A");
		frames(A | B, 1, 10);
		CHECK(!keyboard.state.accents_open && finished == -1, "B closes the accents first");
		frames(A, 1, 10);
		frames(0, 1, 10);
		frames(B, 1, 10);
		CHECK(finished == FALSE, "B cancels");
		/* (what was held as it opened acts once let go of) */
		open_kind(KEYBOARD_FIELD, text, 31);
		keyboard.held = keyboard.opening = START;
		frames(START, 3, 10);
		CHECK(finished == -1, "the opening press acted");
		count = keyboard.state.length;
		frames(0, 1, 10);
		frames(START, 1, 10);
		CHECK(finished == TRUE && count == keyboard.state.length, "Start after");
		return 0;
	}
	CASE("round-trip")
	{
		wchar_t name[12] = { 0 }, field[32] = { 0 }, chat[64] = { 0 };

		/* (a name: its first letter a capital, then small; ë held) */
		open_kind(KEYBOARD_NAME, name, 11);
		focus_on('z');
		frames(A, 1, 10);
		frames(0, 1, 10);
		focus_on('o');
		frames(A, 1, 10);
		frames(0, 1, 10);
		focus_on('e');
		frames(A, 6, 100);
		keyboard_accents_step(&keyboard.state, 4);
		frames(0, 1, 10);
		frames(START, 1, 10);
		CHECK(finished == TRUE && keyboard.state.text[0] == 'Z' && keyboard.state.text[1] == 'o' &&
			keyboard.state.text[2] == 0xEB && keyboard.state.length == 3, "Zoë");
		/* (a field: Latin-1's ñ, not Latin Extended-A's ł; its most letters) */
		open_kind(KEYBOARD_FIELD, field, 4);
		CHECK(keyboard_type(&keyboard.state, 'P') && keyboard_type(&keyboard.state, 'e') &&
			keyboard_type(&keyboard.state, 0xF1) && !keyboard_type(&keyboard.state, 0x142) &&
			keyboard_type(&keyboard.state, 'a') && !keyboard_type(&keyboard.state, 'x'), "Peña");
		frames(START, 1, 10);
		CHECK(finished == TRUE && keyboard.state.length == 4, "the field's text");
		/* (a chat line: ASCII but "|") */
		open_kind(KEYBOARD_CHAT, chat, 63);
		CHECK(keyboard_type(&keyboard.state, 'g') && keyboard_type(&keyboard.state, 'g') &&
			!keyboard_type(&keyboard.state, '|') && !keyboard_type(&keyboard.state, 0xE9) &&
			keyboard_type(&keyboard.state, '!'), "gg!");
		CHECK(text_is("gg!") && !keyboard.state.shift, "the line, with no capital");
		frames(B, 1, 10);
		CHECK(finished == FALSE, "the line let go");
		return 0;
	}
	CASE("fit")
	{
		/* (the block on the menus' 640 by 480 at 4:3 to 32:9: in their middle,
		the extra width margins either side; every key and accent in it; and
		in split screen's views, the 640 by 480 scaled into each) */
		static short const canvases[] = { 640, 768, 853, 1120, 1706 };
		static struct { short width, height; } const views[] = { { 1920, 540 }, { 960, 1080 }, { 960, 540 },
			{ 1280, 400 }, { 640, 360 } };
		int large;

		for (large = 0; large < 2; large++)
		{
			struct keyboard_geometry geometry;
			wchar_t text[12] = { 0 };
			long index;
			short row, column;

			keyboard_geometry_get(large, &geometry);
			CHECK(geometry.left >= 0 && geometry.top >= 0 && geometry.left + geometry.width <= 640 &&
				geometry.top + geometry.height <= 480, "the block off the 640 by 480: %d %d %d %d", geometry.left,
				geometry.top, geometry.width, geometry.height);
			for (index = 0; index < NUMBEROF(canvases); index++)
			{
				short side = (short)((canvases[index] - 640) / 2);
				short left_margin = (short)(geometry.left + side);
				short right_margin = (short)(640 + side - (geometry.left + geometry.width));

				CHECK(left_margin == right_margin || left_margin + 1 == right_margin || left_margin == right_margin + 1,
					"%d wide: margins %d and %d", canvases[index], left_margin, right_margin);
			}
			for (index = 0; index < NUMBEROF(views); index++)
			{
				real scale = MIN((real)views[index].width / 640.0f, (real)views[index].height / 480.0f);

				CHECK(geometry.width * scale <= views[index].width && geometry.height * scale <= views[index].height,
					"a view of %dx%d", views[index].width, views[index].height);
			}
			open_kind(KEYBOARD_NAME, text, 11);
			keyboard_geometry_get(large, &keyboard.geometry);
			CHECK(geometry.prompts_y + 22 <= geometry.top + geometry.height && geometry.text_y > geometry.caption_y,
				"the prompts in the block");
			/* (every layout's keys, the widest's too: QWERTZ's and the Nordic ones'
			eleven; and the accents of each one's last letter of its first row,
			at the block's right) */
			for (index = 0; index < keyboard_layout_count; index++)
			{
				short last;

				keyboard.state.layout = &keyboard_layouts[index];
				for (keyboard.state.page = 0; keyboard.state.page < KEYBOARD_PAGES; keyboard.state.page++)
				{
					for (row = 0; row < KEYBOARD_ROWS; row++)
					{
						for (column = 0; keyboard_key_at(&keyboard.state, row, column); column++)
						{
							struct keyboard_box box;
							short found_row, found_column;

							CHECK(keyboard_key_box(&keyboard.state, &geometry, row, column, &box), "a key's box");
							CHECK(box.x0 >= geometry.left && box.x1 <= geometry.left + geometry.width &&
								box.y0 >= geometry.keys_y && box.y1 < geometry.prompts_y, "%s: a key off the block",
								keyboard.state.layout->name);
							CHECK(keyboard_key_find(&keyboard.state, &geometry, (short)((box.x0 + box.x1) / 2),
								(short)((box.y0 + box.y1) / 2), &found_row, &found_column) && found_row == row &&
								found_column == column, "%s: a key's middle finds another", keyboard.state.layout->name);
						}
					}
				}
				keyboard.state.page = 0;
				keyboard.state.shift = FALSE;
				last = (short)(keyboard.state.layout->pages[0].rows[1].count - 1);
				if (keyboard_accents_open(&keyboard.state, 1, last))
				{
					short accent;

					for (accent = 0; accent < keyboard.state.accent_count; accent++)
					{
						struct keyboard_box box;

						CHECK(keyboard_accent_box(&keyboard.state, &geometry, accent, &box) &&
							box.x0 >= geometry.left && box.x1 <= geometry.left + geometry.width &&
							box.y0 >= geometry.top, "%s: an accent off the block", keyboard.state.layout->name);
					}
					keyboard_accents_close(&keyboard.state);
				}
			}
			keyboard.state.layout = &keyboard_layouts[0];
			keyboard.state.page = 0;
			/* (the most accents, at the row's right end: on the block) */
			keyboard.state.row = 1;
			keyboard.state.column = 9;
			keyboard.state.shift = FALSE;
			CHECK(keyboard_accents_open(&keyboard.state, 1, 7) && keyboard.state.accent_count >= 8, "i's accents");
			for (index = 0; index < keyboard.state.accent_count; index++)
			{
				struct keyboard_box box;

				CHECK(keyboard_accent_box(&keyboard.state, &geometry, (short)index, &box) &&
					box.x0 >= geometry.left && box.x1 <= geometry.left + geometry.width && box.y0 >= geometry.top,
					"an accent off the block");
			}
		}
		return 0;
	}
	CASE("layouts")
	{
		/* (#1136: each layout's letters' rows, in order, each letter with its
		capital; the number row, the letters' last row and the signs' page
		QWERTY's; each row within its width, a key at least 8 tenths; a to z
		each on one key) */
		static struct { char const *name; wchar_t const *rows[3]; } const expected[] =
		{
			{ "qwerty", { L"qwertyuiop", L"asdfghjkl", L"zxcvbnm" } },
			{ "azerty", { L"azertyuiop", L"qsdfghjklm", L"wxcvbn" } },
			{ "qwertz", { L"qwertzuiopü", L"asdfghjklöä", L"yxcvbnm" } },
			{ "swedish", { L"qwertyuiopå", L"asdfghjklöä", L"zxcvbnm" } },
			{ "norwegian", { L"qwertyuiopå", L"asdfghjkløæ", L"zxcvbnm" } },
			{ "danish", { L"qwertyuiopå", L"asdfghjklæø", L"zxcvbnm" } },
			{ "spanish", { L"qwertyuiop", L"asdfghjklñ", L"zxcvbnm" } },
			{ "portuguese", { L"qwertyuiop", L"asdfghjklç", L"zxcvbnm" } },
		};
		struct keyboard_layout const *qwerty = &keyboard_layouts[0];
		long index;

		CHECK(keyboard_layout_count == NUMBEROF(expected), "%ld layouts", keyboard_layout_count);
		for (index = 0; index < NUMBEROF(expected); index++)
		{
			struct keyboard_layout const *layout = keyboard_layout_named(expected[index].name);
			int seen[26] = { 0 };
			short page, row, column;

			CHECK(!strcmp(layout->name, expected[index].name) && layout == &keyboard_layouts[index], "%s by name",
				expected[index].name);
			for (row = 1; row <= 3; row++)
			{
				struct keyboard_row const *keys = &layout->pages[0].rows[row];
				wchar_t const *letters = expected[index].rows[row - 1];
				short letter = 0, others = 0;

				for (column = 0; column < keys->count; column++)
				{
					struct keyboard_key const *key = &keys->keys[column];
					unsigned short capital;

					if (key->kind != KEYBOARD_KEY_CHARACTER)
					{
						others++;
						continue;
					}
					CHECK(letters[letter] && key->character == (unsigned short)letters[letter], "%s row %d key %d: %04X",
						layout->name, row, column, key->character);
					/* (a capital: ASCII's and Latin-1's 0x20 below, Latin Extended-A's
					1 below; Turkish's i's each other's) */
					capital = key->character == 0x131 ? 'I' : key->character == 'i' && !strcmp(layout->name, "turkish") ?
						0x130 : key->character < 0x100 ? (unsigned short)(key->character - 0x20) :
						(unsigned short)(key->character - 1);
					CHECK(key->shifted == capital, "%s: %04X's capital %04X", layout->name, key->character, key->shifted);
					if (key->character >= 'a' && key->character <= 'z')
						seen[key->character - 'a']++;
					letter++;
				}
				CHECK(!letters[letter], "%s row %d: %d letters", layout->name, row, letter);
				/* (the third row's shift and delete; no other key among the letters) */
				CHECK(others == (row == 3 ? 2 : 0) && (row != 3 || (keys->keys[0].kind == KEYBOARD_KEY_SHIFT &&
					keys->keys[keys->count - 1].kind == KEYBOARD_KEY_BACKSPACE)), "%s row %d's keys", layout->name, row);
			}
			for (column = 0; column < 26; column++)
				CHECK(seen[column] == 1, "%s: '%c' on %d keys", layout->name, 'a' + column, seen[column]);
			for (page = 0; page < KEYBOARD_PAGES; page++)
			{
				for (row = 0; row < KEYBOARD_ROWS; row++)
				{
					struct keyboard_row const *keys = &layout->pages[page].rows[row];
					struct keyboard_row const *theirs = &qwerty->pages[page].rows[row];
					int width = keys->indent;

					for (column = 0; column < keys->count; column++)
					{
						width += keys->keys[column].width;
						CHECK(keys->keys[column].width >= 8, "%s: a key %d tenths", layout->name,
							keys->keys[column].width);
					}
					CHECK(width <= KEYBOARD_ROW_WIDTH, "%s page %d row %d is %d wide", layout->name, page, row, width);
					/* (QWERTY's own rows: the digits, the letters' last, the signs) */
					if (page == 1 || row == 0 || row == 4)
					{
						CHECK(keys->indent == theirs->indent && keys->count == theirs->count, "%s page %d row %d",
							layout->name, page, row);
						for (column = 0; column < keys->count; column++)
						{
							CHECK(keys->keys[column].kind == theirs->keys[column].kind &&
								keys->keys[column].width == theirs->keys[column].width &&
								keys->keys[column].character == theirs->keys[column].character &&
								keys->keys[column].shifted == theirs->keys[column].shifted &&
								keys->keys[column].page == theirs->keys[column].page, "%s page %d row %d key %d",
								layout->name, page, row, column);
						}
					}
				}
			}
		}
		/* (a letter with a mark's plain letter: each one's alone) */
		CHECK(keyboard_accent_plain(0xF1) == 'n' && keyboard_accent_plain(0xC5) == 'A' &&
			keyboard_accent_plain(0x131) == 'i' && keyboard_accent_plain(0x130) == 'I' && !keyboard_accent_plain('n') &&
			!keyboard_accent_plain('!'), "the plain letters");
		return 0;
	}
	CASE("auto")
	{
		/* (the system's languages: the first a layout is the one of, a place's
		before its language's; else game.language's; else QWERTY) */
		static struct { char const *locales, *language, *layout; } const choices[] =
		{
			{ "fr_FR", "", "azerty" },
			{ "fr-FR", NULL, "azerty" },
			{ "fr_FR.UTF-8", "", "azerty" },
			{ "fr_CA", "", "qwerty" },
			{ "fr_CH", "", "qwertz" },
			{ "fr_BE", "", "azerty" },
			{ "nl_BE", "", "azerty" },
			{ "nl_NL", "", "qwerty" },
			{ "de_AT", "", "qwertz" },
			{ "DE", "", "qwertz" },
			{ "sv_SE", "", "swedish" },
			{ "fi", "", "swedish" },
			{ "nb_NO", "", "norwegian" },
			{ "da_DK", "", "danish" },
			{ "es_MX", "", "spanish" },
			{ "pt_BR", "", "portuguese" },
			{ "cs_CZ", "", "qwerty" },
			{ "hu_HU", "", "qwerty" },
			{ "tr_TR", "", "qwerty" },
			{ "en_US", "fr", "qwerty" },
			{ "zz_ZZ, de_DE en_US", "", "qwertz" },
			{ "en_GB sv_SE", "", "qwerty" },
			{ "zz", "de", "qwertz" },
			{ "", "es", "spanish" },
			{ NULL, "fr", "azerty" },
			{ "", "ja", "qwerty" },
			{ "", "", "qwerty" },
			{ "zz_ZZ", "zz", "qwerty" },
			{ NULL, NULL, "qwerty" },
		};
		long index;

		for (index = 0; index < NUMBEROF(choices); index++)
		{
			struct keyboard_layout const *layout = keyboard_layout_auto(choices[index].locales, choices[index].language);

			CHECK(!strcmp(layout->name, choices[index].layout), "\"%s\" and \"%s\": %s, not %s",
				choices[index].locales ? choices[index].locales : "(none)",
				choices[index].language ? choices[index].language : "(none)", layout->name, choices[index].layout);
		}
		return 0;
	}
	CASE("filtered")
	{
		/* (a layout's own letters with a mark, typed only where the text
		takes them and the font draws them; else their plain letter, shown
		on the key too) */
		wchar_t name[12] = { 0 }, field[32] = { 0 }, chat[64] = { 0 };
		long index;

		layout_name = "spanish";
		open_kind(KEYBOARD_FIELD, field, 31);
		focus_on(0xF1);
		CHECK(keyboard_key_character(&keyboard.state, keyboard_key_at(&keyboard.state, keyboard.state.row,
			keyboard.state.column)) == 0xF1 && keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED &&
			keyboard.state.text[0] == 0xF1, "a field's ñ: %04X", keyboard.state.text[0]);
		/* (held: ñ's accents, its own first, then n and n's) */
		CHECK(keyboard_hold(&keyboard.state, 1000) && keyboard.state.accents[0] == 0xF1 &&
			keyboard.state.accents[1] == 'n', "ñ held: %d", keyboard.state.accent_count);
		keyboard_accents_close(&keyboard.state);
		/* (the font without ñ: n) */
		open_kind(KEYBOARD_FIELD, field, 31);
		font_lacks = 0xF1;
		focus_on(0xF1);
		CHECK(keyboard_key_character(&keyboard.state, keyboard_key_at(&keyboard.state, keyboard.state.row,
			keyboard.state.column)) == 'n' && keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && text_is("n"),
			"ñ without its glyph");
		font_lacks = 0;
		/* (a chat line, ASCII: n; held, n has no accents the chat takes) */
		open_kind(KEYBOARD_CHAT, chat, 63);
		focus_on(0xF1);
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && text_is("n") &&
			!keyboard_hold(&keyboard.state, 1000), "a chat line's ñ");
		/* (a capital held on its own key, Ö: its accents, shift kept on while they are open) */
		layout_name = "swedish";
		open_kind(KEYBOARD_NAME, name, 11);
		focus_on(0xF6);
		CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && keyboard_hold(&keyboard.state, 1000) &&
			keyboard.state.accents[0] == 0xD6 && keyboard.state.accents[1] == 'O' && keyboard.state.shift &&
			keyboard.state.length == 0, "Ö held");
		keyboard_accents_close(&keyboard.state);
		/* (hold-for-accent on every layout: e's, é among them) */
		for (index = 0; index < keyboard_layout_count; index++)
		{
			int accent, acute = 0;

			layout_name = keyboard_layouts[index].name;
			open_kind(KEYBOARD_FIELD, field, 31);
			focus_on('e');
			CHECK(keyboard_press(&keyboard.state, 0) == KEYBOARD_TYPED && keyboard_hold(&keyboard.state, 1000),
				"%s: e held", layout_name);
			for (accent = 0; accent < keyboard.state.accent_count; accent++)
				acute |= keyboard.state.accents[accent] == 0xE9;
			CHECK(keyboard.state.accents[0] == 'e' && acute, "%s: e's accents", layout_name);
			keyboard_accents_close(&keyboard.state);
		}
		layout_name = "qwerty";
		return 0;
	}
	CASE("drawable")
	{
		/* (no layout has a letter the menus' font, ui\large_ui, does not
		draw: it has Windows-1252's, Latin-1's and these few past it) */
		long index;

		for (index = 0; index < keyboard_layout_count; index++)
		{
			short row, column;

			for (row = 0; row < KEYBOARD_ROWS; row++)
			{
				struct keyboard_row const *keys = &keyboard_layouts[index].pages[0].rows[row];

				for (column = 0; column < keys->count; column++)
				{
					unsigned short both[2];
					int each;

					both[0] = keys->keys[column].character;
					both[1] = keys->keys[column].shifted;
					for (each = 0; each < 2; each++)
					{
						unsigned short character = both[each];

						CHECK(character <= 0xFF || character == 0x152 || character == 0x153 || character == 0x160 ||
							character == 0x161 || character == 0x178 || character == 0x17D || character == 0x17E,
							"%s: %04X, which the font does not draw", keyboard_layouts[index].name, character);
					}
				}
			}
		}
		return 0;
	}
	CASE("modes")
	{
		/* (the new menus: the new keyboard; the original-faithful modes: the
		game's own) */
		wchar_t text[12] = { 0 };

		mode_new = TRUE;
		CHECK(keyboard_name_launch(text, sizeof(text), 8) && new_opened == 1 && !old_opened, "the new menus' name");
		keyboard.up = FALSE;
		CHECK(keyboard_field_launch(text, sizeof(text), L"T", L"C") && new_opened == 2 && !old_opened, "a field");
		keyboard.up = FALSE;
		mode_new = FALSE;
		CHECK(keyboard_name_launch(text, sizeof(text), 8) && old_opened == 1 && new_opened == 2, "the classic's");
		CHECK(keyboard_field_launch(text, sizeof(text), L"T", L"C") && old_opened == 2, "the classic's field");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
