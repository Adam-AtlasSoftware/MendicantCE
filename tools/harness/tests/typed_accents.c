/* Letters with a mark typed into a name (#1134): the keyboard's layout's
own (sdl_platform.c's keystroke_character: é on a French keyboard's 2, ł
with AltGr on a Polish one) and the system's on-screen keyboard's text
(utf8_next, typed_letter_kept), put in by the on-screen keyboard's typing
(virtual_keyboard.c's virtual_keyboard_port_character_kept: Latin-1 and
Latin Extended-A) and the menus' text field (menu_functions.c's
text_field_character_kept: Latin-1, its text's bytes). A name so typed is
one player_name_clean keeps as it is, and the host's fold
(player_name_character_ascii) gives its plain letters. */
#include "harness.h"

#include <wchar.h>

typedef int BOOL;
typedef char CHAR;
typedef int SDL_Keycode;

#include "under_test.inc"

/* a name typed a character at a time, as the on-screen keyboard's typing
puts it in */
static void typed(wchar_t const *keys, wchar_t *name, int size)
{
	int length = 0;

	for (; *keys; keys++)
	{
		if (virtual_keyboard_port_character_kept(*keys) && length < size - 1)
			name[length++] = *keys;
	}
	name[length] = 0;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("layout")
	{
		/* (AZERTY's 2: é, no ASCII; Polish AltGr+l: ł) */
		CHECK(keystroke_character(0xE9, 0) == 0xE9, "é from a layout's key lost");
		CHECK(keystroke_character(0x142, 0) == 0x142, "ł from a layout's key lost");
		CHECK(keystroke_character('a', 'a') == 'a', "ASCII not its own");
		CHECK(keystroke_character(0x20AC, 0) == 0, "€ taken for a letter");
		CHECK(keystroke_character(0xD7, 0) == 0, "× taken for a letter");
		CHECK(keystroke_character(0x0D, '\r') == 0, "Return typed a character");
		return 0;
	}
	CASE("text_event")
	{
		/* (a phone's keyboard's "Zoë Ł": UTF-8) */
		const char *text = "Zo\xC3\xAB \xC5\x81";
		unsigned long expected[] = { 'Z', 'o', 0xEB, ' ', 0x141, 0 };
		int index;

		for (index = 0; expected[index]; index++)
		{
			unsigned long character = utf8_next(&text);

			CHECK(character == expected[index], "character %d: %lx, not %lx", index, character, expected[index]);
		}
		CHECK(utf8_next(&text) == 0, "past the end");
		text = "\xC3";
		CHECK(utf8_next(&text) == 0xFFFD, "a cut sequence not broken");
		CHECK(typed_letter_kept(0xEB) && typed_letter_kept(0x141), "the event's letters not kept");
		CHECK(!typed_letter_kept('Z') && !typed_letter_kept(0x20AC), "ASCII (its keys') or € kept");
		return 0;
	}
	CASE("name")
	{
		static wchar_t const *const names[][2] = {
			{ L"Zo\x00EB", L"Zoe" }, { L"Pe\x00F1" L"a", L"Pena" }, { L"\x0141ukasz", L"Lukasz" },
			{ L"M\x00FCller", L"Muller" }, { L"\x0150z", L"Oz" },
		};
		int index;

		for (index = 0; index < (int)(sizeof(names) / sizeof(names[0])); index++)
		{
			wchar_t name[12], clean[12];
			int at;

			typed(names[index][0], name, 12);
			CHECK(!wcscmp(name, names[index][0]), "name %d lost a letter in typing", index);
			wcscpy(clean, name);
			CHECK(player_name_clean(clean, 12), "name %d not a valid name", index);
			CHECK(!wcscmp(clean, name), "name %d changed by the cleaning", index);
			for (at = 0; name[at]; at++)
			{
				CHECK(player_name_character_ascii(name[at]) == (char)names[index][1][at],
					"name %d's letter %d folded to %c", index, at, player_name_character_ascii(name[at]));
			}
		}
		CHECK(!virtual_keyboard_port_character_kept(0xF7) && !virtual_keyboard_port_character_kept(0x7F),
			"÷ or DEL typed");
		return 0;
	}
	CASE("text_field")
	{
		CHECK(text_field_character_kept(0xE9) && text_field_character_kept(0xD1), "é or Ñ not kept in the field");
		CHECK(text_field_character_kept('a') && text_field_character_kept(' '), "ASCII not kept");
		/* (its text is bytes: Latin Extended-A's are the on-screen keyboard's) */
		CHECK(!text_field_character_kept(0x142), "ł kept in a byte");
		CHECK(!text_field_character_kept(0xD7) && !text_field_character_kept(0x1B), "× or Escape kept");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
