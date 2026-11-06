/*
FORGE_TEXT_ENTRY.C (test)

A line of text typed in Forge (port/linux/game/forge.c, forge_text_entry_*
and the on-screen keyboard, forge_text_keyboard_*), worked as the keyboard a
profile's name is typed on (virtual_keyboard.c): the first key replaces the
line it began with, characters go in at the cursor and backspace deletes the
one before it, the cursor moves within the line, shift lasts a key and caps
until it is pressed again, symbols changes the characters, and the chosen
key moves over the wide keys a key at a time, round the edges.
test_forge_text_entry.py takes the code (under_test.inc).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "harness.h"

#include "under_test.inc"

static char taken[FORGE_TEXT_MAXIMUM_LENGTH + 1];
static int takes;

static void took(char const *text)
{
	snprintf(taken, sizeof(taken), "%s", text);
	takes++;
}

/* a line begun as forge_text_entry_begin begins it */
static void begin(char const *initial, unsigned long maximum_length)
{
	memset(&forge_text_entry_globals, 0, sizeof(forge_text_entry_globals));
	forge_text_entry_globals.active = TRUE;
	forge_text_entry_globals.done = took;
	forge_text_entry_globals.maximum_length = maximum_length;
	snprintf(forge_text_entry_globals.text, sizeof(forge_text_entry_globals.text), "%s", initial);
	forge_text_entry_globals.length = (unsigned long)strlen(initial);
	forge_text_entry_globals.cursor = forge_text_entry_globals.length;
	forge_text_entry_globals.replaces = TRUE;
	taken[0] = 0;
	takes = 0;
}

/* the on-screen keyboard's key that is a key, chosen and pressed */
static void press(short key)
{
	short row;
	short column;

	for (row = 0; row < FORGE_TEXT_KEYBOARD_ROWS; row++)
	{
		for (column = 0; column < FORGE_TEXT_KEYBOARD_COLUMNS; column++)
		{
			if (forge_text_keyboard_layout[row][column] == key)
			{
				forge_text_entry_globals.keyboard_row = row;
				forge_text_entry_globals.keyboard_column = column;
				forge_text_keyboard_press();
				return;
			}
		}
	}
	CHECK(FALSE, "no key %d on the keyboard", key);
}

/* the key of the keyboard that types a character, plain */
static short key_of(char character)
{
	char const *at = strchr(forge_text_keyboard_characters, character);

	CHECK(at != NULL, "no key types %c", character);
	return (short)(at - forge_text_keyboard_characters);
}

static void press_text(char const *text)
{
	for (; *text; text++)
		press(*text == ' ' ? _forge_text_keyboard_space : key_of(*text));
}

#define CHECK_LINE(expected, expected_cursor) \
	CHECK(!strcmp(forge_text_entry_globals.text, (expected)) && \
		forge_text_entry_globals.length == strlen(expected) && \
		forge_text_entry_globals.cursor == (expected_cursor), \
		"the line is \"%s\" (%lu long), the cursor at %lu; expected \"%s\" at %d", forge_text_entry_globals.text, \
		forge_text_entry_globals.length, forge_text_entry_globals.cursor, (expected), (int)(expected_cursor))

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* the first key replaces the line it began with; the next add to it */
	CASE("replaces")
	{
		begin("layout 1", 63);
		press_text("ab");
		CHECK_LINE("ab", 2);
		return 0;
	}
	/* the first backspace (or X) clears the line it began with; a cursor key
	keeps it, and then backspace deletes a character */
	CASE("first-delete")
	{
		begin("layout 1", 63);
		forge_text_entry_delete();
		CHECK_LINE("", 0);
		begin("layout 1", 63);
		press(_forge_text_keyboard_right);
		press(_forge_text_keyboard_backspace);
		CHECK_LINE("layout ", 7);
		return 0;
	}
	/* characters go in at the cursor; backspace deletes the one before it */
	CASE("cursor")
	{
		begin("", 63);
		press_text("acd");
		press(_forge_text_keyboard_left);
		press(_forge_text_keyboard_left);
		press_text("b");
		CHECK_LINE("abcd", 2);
		press(_forge_text_keyboard_right);
		press(_forge_text_keyboard_backspace);
		CHECK_LINE("abd", 2);
		/* (the cursor stays within the line) */
		forge_text_entry_move_cursor(TRUE);
		forge_text_entry_move_cursor(TRUE);
		CHECK_LINE("abd", 3);
		forge_text_entry_move_cursor(FALSE);
		forge_text_entry_move_cursor(FALSE);
		forge_text_entry_move_cursor(FALSE);
		forge_text_entry_move_cursor(FALSE);
		forge_text_entry_delete();
		CHECK_LINE("abd", 0);
		return 0;
	}
	/* the line is no longer than its maximum, wherever the cursor is */
	CASE("full")
	{
		begin("", 3);
		press_text("abcd");
		CHECK_LINE("abc", 3);
		press(_forge_text_keyboard_left);
		press_text("x");
		CHECK_LINE("abc", 2);
		return 0;
	}
	/* shift makes the next letter a capital; caps all of them; symbols puts
	the second characters on the keys */
	CASE("shift")
	{
		begin("", 63);
		press(_forge_text_keyboard_shift);
		press_text("ab1");
		CHECK_LINE("Ab1", 3);
		press(_forge_text_keyboard_caps);
		press_text("cd");
		press(_forge_text_keyboard_caps);
		press_text("e");
		CHECK_LINE("Ab1CDe", 6);
		press(_forge_text_keyboard_symbols);
		press(key_of('1'));
		press(_forge_text_keyboard_symbols);
		press(key_of('1'));
		CHECK_LINE("Ab1CDe!1", 8);
		return 0;
	}
	/* done takes the line; the symbols are characters the menus can draw */
	CASE("done")
	{
		short key;

		begin("x", 63);
		press_text("name");
		press(_forge_text_keyboard_done);
		CHECK(takes == 1 && !strcmp(taken, "name") && !forge_text_entry_globals.active, "done took \"%s\" %d times",
			taken, takes);
		for (key = 0; key < FORGE_TEXT_KEYBOARD_CHARACTER_KEYS; key++)
		{
			char symbol = forge_text_keyboard_symbols[key];

			CHECK(symbol > ' ' && symbol <= '~' && !strchr("%|{}\\", symbol), "key %d's symbol is %c", key, symbol);
		}
		CHECK(strlen(forge_text_keyboard_symbols) == FORGE_TEXT_KEYBOARD_CHARACTER_KEYS,
			"%d symbols for %d keys", (int)strlen(forge_text_keyboard_symbols), FORGE_TEXT_KEYBOARD_CHARACTER_KEYS);
		return 0;
	}
	/* the chosen key moves a key at a time over the wide keys, round the
	edges, as the profile's keyboard's does */
	CASE("move")
	{
		begin("", 63);
		/* done, left round to the right edge: 0 */
		forge_text_keyboard_move(0, -1);
		CHECK(forge_text_keyboard_key() == key_of('0'), "left of done is key %d", forge_text_keyboard_key());
		/* up round the bottom: right, then right again to space, the row's
		start round the edge */
		forge_text_keyboard_move(-1, 0);
		CHECK(forge_text_keyboard_key() == _forge_text_keyboard_right, "above 0 is key %d", forge_text_keyboard_key());
		forge_text_keyboard_move(0, 1);
		CHECK(forge_text_keyboard_key() == _forge_text_keyboard_space, "right of > is key %d",
			forge_text_keyboard_key());
		forge_text_keyboard_move(0, 1);
		CHECK(forge_text_keyboard_key() == _forge_text_keyboard_left, "right of space is key %d",
			forge_text_keyboard_key());
		/* up from <: backspace, then left of it z */
		forge_text_keyboard_move(-1, 0);
		CHECK(forge_text_keyboard_key() == _forge_text_keyboard_backspace, "above < is key %d",
			forge_text_keyboard_key());
		forge_text_keyboard_move(0, -1);
		CHECK(forge_text_keyboard_key() == key_of('z'), "left of backspace is key %d", forge_text_keyboard_key());
		return 0;
	}

	fprintf(stderr, "no case named %s\n", case_name);
	return 2;
}
