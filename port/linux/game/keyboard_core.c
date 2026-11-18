/*
KEYBOARD_CORE.C

The on-screen keyboard's rules (keyboard_core.h), apart from the game: the
layouts (data: a page of rows of keys, each its kind, width and characters),
the accents (data: each letter's letters with a mark, Latin-1's and Latin
Extended-A's), the text typed into, the focus's moves, shift and caps lock,
a held key's accents and repeats, and the keys' places on the menus' 640 by
480. The game's side (its screen, its input, what each text takes and what
the font draws) is keyboard.c.
*/

#include "keyboard_core.h"

#include <string.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NONE
#define NONE (-1)
#endif

/* ---------- the layouts */

/* a character's key (its shifted character) of a width (tenths), a
letter's (its capital), a sign's (the same shifted), and the others */
#define KEY_WIDE(width, character, shifted) { KEYBOARD_KEY_CHARACTER, width, character, shifted, 0, 0 }
#define KEY(character, shifted) KEY_WIDE(10, character, shifted)
#define SIGN(character) KEY(character, character)
#define WIDE(kind, width, label) { kind, width, 0, 0, 0, label }
#define PAGE(width, page, label) { KEYBOARD_KEY_PAGE, width, 0, 0, page, label }
#define LETTER_WIDE(width, lower) KEY_WIDE(width, lower, (lower) - 'a' + 'A')
#define LETTER(lower) LETTER_WIDE(10, lower)
#define SHIFT(width) WIDE(KEYBOARD_KEY_SHIFT, width, "SHIFT")
#define DELETE(width) WIDE(KEYBOARD_KEY_BACKSPACE, width, "DEL")

/* the number row, the same on both pages and in every layout (its digits
plain: a physical AZERTY's shifted digits are its own keys', not these) */
#define DIGITS { 0, 10, { SIGN('1'), SIGN('2'), SIGN('3'), SIGN('4'), SIGN('5'), SIGN('6'), SIGN('7'), SIGN('8'), \
	SIGN('9'), SIGN('0') } }
/* the letters' page's last row, and the signs' page (every one of ASCII's
but "|", the game's text's own mark), every layout's */
#define LETTERS_FOOT { 0, 4, { PAGE(15, 1, "?123"), SIGN(','), WIDE(KEYBOARD_KEY_SPACE, 65, "SPACE"), SIGN('.') } }
#define SIGNS_PAGE { { \
	DIGITS, \
	{ 0, 10, { SIGN('!'), SIGN('@'), SIGN('#'), SIGN('$'), SIGN('%'), SIGN('^'), SIGN('&'), SIGN('*'), \
		SIGN('('), SIGN(')') } }, \
	{ 0, 10, { SIGN('-'), SIGN('_'), SIGN('='), SIGN('+'), SIGN('['), SIGN(']'), SIGN('{'), SIGN('}'), \
		SIGN(';'), SIGN(':') } }, \
	{ 0, 9, { SIGN('\''), SIGN('"'), SIGN('<'), SIGN('>'), SIGN('/'), SIGN('\\'), SIGN('`'), SIGN('~'), \
		DELETE(20) } }, \
	{ 0, 5, { PAGE(15, 0, "ABC"), SIGN(','), WIDE(KEYBOARD_KEY_SPACE, 55, "SPACE"), SIGN('.'), SIGN('?') } }, \
} }

/* the letters with a mark a layout has keys of (Latin-1's, each with its
capital: all the menus' font draws), of a width; one the text does not take
types its plain letter (keyboard_key_character) */
#define A_DIAERESIS(width) KEY_WIDE(width, 0xE4, 0xC4)
#define A_RING(width) KEY_WIDE(width, 0xE5, 0xC5)
#define AE(width) KEY_WIDE(width, 0xE6, 0xC6)
#define C_CEDILLA(width) KEY_WIDE(width, 0xE7, 0xC7)
#define N_TILDE(width) KEY_WIDE(width, 0xF1, 0xD1)
#define O_DIAERESIS(width) KEY_WIDE(width, 0xF6, 0xD6)
#define O_STROKE(width) KEY_WIDE(width, 0xF8, 0xD8)
#define U_DIAERESIS(width) KEY_WIDE(width, 0xFC, 0xDC)

/* the letters the layouts share: QWERTY's and QWERTZ's first rows' ten,
the home row's nine, and the third rows' seven */
#define QWERTY_TOP(width) LETTER_WIDE(width, 'q'), LETTER_WIDE(width, 'w'), LETTER_WIDE(width, 'e'), \
	LETTER_WIDE(width, 'r'), LETTER_WIDE(width, 't'), LETTER_WIDE(width, 'y'), LETTER_WIDE(width, 'u'), \
	LETTER_WIDE(width, 'i'), LETTER_WIDE(width, 'o'), LETTER_WIDE(width, 'p')
#define QWERTZ_TOP(width) LETTER_WIDE(width, 'q'), LETTER_WIDE(width, 'w'), LETTER_WIDE(width, 'e'), \
	LETTER_WIDE(width, 'r'), LETTER_WIDE(width, 't'), LETTER_WIDE(width, 'z'), LETTER_WIDE(width, 'u'), \
	LETTER_WIDE(width, 'i'), LETTER_WIDE(width, 'o'), LETTER_WIDE(width, 'p')
#define HOME(width) LETTER_WIDE(width, 'a'), LETTER_WIDE(width, 's'), LETTER_WIDE(width, 'd'), \
	LETTER_WIDE(width, 'f'), LETTER_WIDE(width, 'g'), LETTER_WIDE(width, 'h'), LETTER_WIDE(width, 'j'), \
	LETTER_WIDE(width, 'k'), LETTER_WIDE(width, 'l')
#define QWERTY_THIRD(width) LETTER_WIDE(width, 'z'), LETTER_WIDE(width, 'x'), LETTER_WIDE(width, 'c'), \
	LETTER_WIDE(width, 'v'), LETTER_WIDE(width, 'b'), LETTER_WIDE(width, 'n'), LETTER_WIDE(width, 'm')
#define QWERTZ_THIRD(width) LETTER_WIDE(width, 'y'), LETTER_WIDE(width, 'x'), LETTER_WIDE(width, 'c'), \
	LETTER_WIDE(width, 'v'), LETTER_WIDE(width, 'b'), LETTER_WIDE(width, 'n'), LETTER_WIDE(width, 'm')

/* a layout of ten letters and one more on its first row, nine and two more
on its home row and seven on its third (QWERTZ's, the Nordic ones'), at 9
tenths a key */
#define ELEVEN(name, locales, top, top_last, home_last_1, home_last_2, third) { name, locales, { { { \
	DIGITS, \
	{ 0, 11, { top(9), top_last(9) } }, \
	{ 0, 11, { HOME(9), home_last_1(9), home_last_2(9) } }, \
	{ 0, 9, { SHIFT(18), third(9), DELETE(18) } }, \
	LETTERS_FOOT, \
} }, SIGNS_PAGE } }
/* ... of ten, ten and seven (the Spanish ñ's, the Portuguese ç's) */
#define TEN(name, locales, home_last) { name, locales, { { { \
	DIGITS, \
	{ 0, 10, { QWERTY_TOP(10) } }, \
	{ 0, 10, { HOME(10), home_last(10) } }, \
	{ 0, 9, { SHIFT(15), QWERTY_THIRD(10), DELETE(15) } }, \
	LETTERS_FOOT, \
} }, SIGNS_PAGE } }

/* (each layout's locales: the languages, "fr", and the places, "fr_CA",
whose layout it is; a place's comes before its language's) */
struct keyboard_layout const keyboard_layouts[] =
{
	/* (US, UK, Italian, Dutch, Polish, Romanian, Canadian French, Japanese
	romaji; and the layout where none is another's) */
	{ "qwerty", "en it nl pl ro fr_CA ja", { { {
		DIGITS,
		{ 0, 10, { QWERTY_TOP(10) } },
		{ 5, 9, { HOME(10) } },
		{ 0, 9, { SHIFT(15), QWERTY_THIRD(10), DELETE(15) } },
		LETTERS_FOOT,
	} }, SIGNS_PAGE } },
	/* (France, Belgium) */
	{ "azerty", "fr nl_BE", { { {
		DIGITS,
		{ 0, 10, { LETTER('a'), LETTER('z'), LETTER('e'), LETTER('r'), LETTER('t'), LETTER('y'), LETTER('u'),
			LETTER('i'), LETTER('o'), LETTER('p') } },
		{ 0, 10, { LETTER('q'), LETTER('s'), LETTER('d'), LETTER('f'), LETTER('g'), LETTER('h'), LETTER('j'),
			LETTER('k'), LETTER('l'), LETTER('m') } },
		{ 5, 8, { SHIFT(15), LETTER('w'), LETTER('x'), LETTER('c'), LETTER('v'), LETTER('b'), LETTER('n'),
			DELETE(15) } },
		LETTERS_FOOT,
	} }, SIGNS_PAGE } },
	/* (Germany, Austria, Switzerland; Slovak, Slovene and Croatian too) */
	ELEVEN("qwertz", "de fr_CH it_CH sk sl hr", QWERTZ_TOP, U_DIAERESIS, O_DIAERESIS, A_DIAERESIS, QWERTZ_THIRD),
	/* (Sweden and Finland, Norway, Denmark) */
	ELEVEN("swedish", "sv fi", QWERTY_TOP, A_RING, O_DIAERESIS, A_DIAERESIS, QWERTY_THIRD),
	ELEVEN("norwegian", "nb nn no", QWERTY_TOP, A_RING, O_STROKE, AE, QWERTY_THIRD),
	ELEVEN("danish", "da", QWERTY_TOP, A_RING, AE, O_STROKE, QWERTY_THIRD),
	/* (Spain and Latin America; Portugal and Brazil) */
	TEN("spanish", "es", N_TILDE),
	TEN("portuguese", "pt", C_CEDILLA),
	/* (none whose own letters the menus' font, ui\large_ui, does not draw:
	it has Windows-1252's, so not Czech's ů, Hungarian's ő ű nor Turkish's
	ı ğ ş İ; those languages' players get QWERTY and the accents) */
};

long const keyboard_layout_count = sizeof(keyboard_layouts) / sizeof(keyboard_layouts[0]);

struct keyboard_layout const *keyboard_layout_named(char const *name)
{
	long index;

	for (index = 0; name && index < keyboard_layout_count; index++)
	{
		if (!strcmp(keyboard_layouts[index].name, name))
			return &keyboard_layouts[index];
	}
	return &keyboard_layouts[0];
}

/* whether a layout is a locale's ("fr" or "fr_CA", length characters), as
a whole word of its locales */
static int keyboard_layout_has(struct keyboard_layout const *layout, char const *locale, int length)
{
	char const *each = layout->locales;

	while (each && *each)
	{
		int each_length = 0;

		while (each[each_length] && each[each_length] != ' ')
			each_length++;
		if (each_length == length && !strncmp(each, locale, (size_t)length))
			return TRUE;
		each += each_length;
		while (*each == ' ')
			each++;
	}
	return FALSE;
}

/* the layout of a locale ("fr_CA", "fr-CA", "fr"; length characters): its
place's ("fr_CA"), else its language's ("fr"); NULL for none */
static struct keyboard_layout const *keyboard_layout_of_locale(char const *locale, int length)
{
	char tag[8];
	int index, language = 0;
	long each;

	/* (the language and its place's two letters alone, "fr_CA" of
	"fr-CA.UTF-8", in the table's cases) */
	for (index = 0; index < length && index < (int)sizeof(tag) - 1; index++)
	{
		char character = locale[index];

		if (character == '-')
			character = '_';
		if (character == '_' && !language)
			language = index;
		else if (!(character >= 'a' && character <= 'z') && !(character >= 'A' && character <= 'Z'))
			break;
		else if (!language && character >= 'A' && character <= 'Z')
			character = (char)(character - 'A' + 'a');
		else if (language && character >= 'a' && character <= 'z')
			character = (char)(character - 'a' + 'A');
		tag[index] = character;
	}
	length = index;
	if (!language)
		language = length;
	if (language < 2)
		return NULL;
	for (each = 0; length == language + 3 && each < keyboard_layout_count; each++)
	{
		if (keyboard_layout_has(&keyboard_layouts[each], tag, length))
			return &keyboard_layouts[each];
	}
	for (each = 0; each < keyboard_layout_count; each++)
	{
		if (keyboard_layout_has(&keyboard_layouts[each], tag, language))
			return &keyboard_layouts[each];
	}
	return NULL;
}

struct keyboard_layout const *keyboard_layout_auto(char const *locales, char const *language)
{
	char const *each = locales;

	while (each && *each)
	{
		struct keyboard_layout const *layout;
		int length = 0;

		while (*each == ' ' || *each == ',')
			each++;
		while (each[length] && each[length] != ' ' && each[length] != ',')
			length++;
		layout = length ? keyboard_layout_of_locale(each, length) : NULL;
		if (layout)
			return layout;
		each += length;
	}
	if (language && *language)
	{
		struct keyboard_layout const *layout = keyboard_layout_of_locale(language, (int)strlen(language));

		if (layout)
			return layout;
	}
	return &keyboard_layouts[0];
}

/* ---------- the accents: each letter's, small and capital, in Latin-1 and
Latin Extended-A (what a player's name keeps: players.c's player_name_clean,
U+00C0 to U+017F) */

static struct keyboard_accents const keyboard_accent_table[] =
{
	{ 'a', { 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0x101, 0x103, 0x105 } },
	{ 'A', { 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0x100, 0x102, 0x104 } },
	{ 'c', { 0xE7, 0x107, 0x109, 0x10B, 0x10D } },
	{ 'C', { 0xC7, 0x106, 0x108, 0x10A, 0x10C } },
	{ 'd', { 0x10F, 0x111, 0xF0 } },
	{ 'D', { 0x10E, 0x110, 0xD0 } },
	{ 'e', { 0xE8, 0xE9, 0xEA, 0xEB, 0x113, 0x115, 0x117, 0x119, 0x11B } },
	{ 'E', { 0xC8, 0xC9, 0xCA, 0xCB, 0x112, 0x114, 0x116, 0x118, 0x11A } },
	{ 'g', { 0x11D, 0x11F, 0x121, 0x123 } },
	{ 'G', { 0x11C, 0x11E, 0x120, 0x122 } },
	{ 'h', { 0x125, 0x127 } },
	{ 'H', { 0x124, 0x126 } },
	{ 'i', { 0xEC, 0xED, 0xEE, 0xEF, 0x129, 0x12B, 0x12D, 0x12F, 0x131, 0x133 } },
	{ 'I', { 0xCC, 0xCD, 0xCE, 0xCF, 0x128, 0x12A, 0x12C, 0x12E, 0x130, 0x132 } },
	{ 'j', { 0x135 } },
	{ 'J', { 0x134 } },
	{ 'k', { 0x137, 0x138 } },
	{ 'K', { 0x136 } },
	{ 'l', { 0x13A, 0x13C, 0x13E, 0x140, 0x142 } },
	{ 'L', { 0x139, 0x13B, 0x13D, 0x13F, 0x141 } },
	{ 'n', { 0xF1, 0x144, 0x146, 0x148, 0x14B } },
	{ 'N', { 0xD1, 0x143, 0x145, 0x147, 0x14A } },
	{ 'o', { 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF8, 0x14D, 0x14F, 0x151, 0x153 } },
	{ 'O', { 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD8, 0x14C, 0x14E, 0x150, 0x152 } },
	{ 'r', { 0x155, 0x157, 0x159 } },
	{ 'R', { 0x154, 0x156, 0x158 } },
	{ 's', { 0xDF, 0x15B, 0x15D, 0x15F, 0x161 } },
	{ 'S', { 0x15A, 0x15C, 0x15E, 0x160 } },
	{ 't', { 0x163, 0x165, 0x167, 0xFE } },
	{ 'T', { 0x162, 0x164, 0x166, 0xDE } },
	{ 'u', { 0xF9, 0xFA, 0xFB, 0xFC, 0x169, 0x16B, 0x16D, 0x16F, 0x171, 0x173 } },
	{ 'U', { 0xD9, 0xDA, 0xDB, 0xDC, 0x168, 0x16A, 0x16C, 0x16E, 0x170, 0x172 } },
	{ 'w', { 0x175 } },
	{ 'W', { 0x174 } },
	{ 'y', { 0xFD, 0xFF, 0x177 } },
	{ 'Y', { 0xDD, 0x178, 0x176 } },
	{ 'z', { 0x17A, 0x17C, 0x17E } },
	{ 'Z', { 0x179, 0x17B, 0x17D } },
};

int keyboard_accents_of(unsigned short character, unsigned short *variants, int maximum)
{
	int index, count = 0;

	for (index = 0; index < (int)(sizeof(keyboard_accent_table) / sizeof(keyboard_accent_table[0])); index++)
	{
		struct keyboard_accents const *accents = &keyboard_accent_table[index];
		int variant;

		if (accents->base != character)
			continue;
		for (variant = 0; variant < KEYBOARD_MAXIMUM_ACCENTS && accents->variants[variant] && count < maximum; variant++)
			variants[count++] = accents->variants[variant];
	}
	return count;
}

unsigned short keyboard_accent_plain(unsigned short character)
{
	int index, variant;

	for (index = 0; character >= 0x80 && index < (int)(sizeof(keyboard_accent_table) / sizeof(keyboard_accent_table[0]));
		index++)
	{
		for (variant = 0; variant < KEYBOARD_MAXIMUM_ACCENTS && keyboard_accent_table[index].variants[variant]; variant++)
		{
			if (keyboard_accent_table[index].variants[variant] == character)
				return keyboard_accent_table[index].base;
		}
	}
	return 0;
}

/* ---------- the keyboard */

static struct keyboard_row const *keyboard_row_of(struct keyboard_state const *state, short row)
{
	if (row < 0 || row >= KEYBOARD_ROWS || state->page < 0 || state->page >= KEYBOARD_PAGES)
		return NULL;
	return &state->layout->pages[state->page].rows[row];
}

struct keyboard_key const *keyboard_key_at(struct keyboard_state const *state, short row, short column)
{
	struct keyboard_row const *keys = keyboard_row_of(state, row);

	if (!keys || column < 0 || column >= keys->count)
		return NULL;
	return &keys->keys[column];
}

int keyboard_offered(struct keyboard_state const *state, unsigned short character)
{
	if (!character || character == '|')
		return FALSE;
	return !state->offered || state->offered(character, state->context);
}

unsigned short keyboard_key_character(struct keyboard_state const *state, struct keyboard_key const *key)
{
	unsigned short character, plain;

	if (!key || key->kind != KEYBOARD_KEY_CHARACTER)
		return 0;
	character = state->shift || state->caps ? key->shifted : key->character;
	/* (a layout's own letter with a mark that the text does not take or the
	font does not draw: its plain letter, as the accents' filter has it) */
	plain = keyboard_accent_plain(character);
	if (plain && !keyboard_offered(state, character))
		return plain;
	return character;
}

/* a name's first letter a capital: shift on while it is empty */
static void keyboard_capital(struct keyboard_state *state)
{
	if (state->capital_first && state->length == 0 && !state->caps)
		state->shift = TRUE;
}

void keyboard_begin(struct keyboard_state *state, struct keyboard_layout const *layout, unsigned short const *text,
	short maximum, int capital_first, keyboard_offered_function offered, void *context)
{
	memset(state, 0, sizeof(*state));
	state->layout = layout ? layout : &keyboard_layouts[0];
	state->maximum = maximum < 0 ? 0 : maximum > KEYBOARD_MAXIMUM_TEXT ? KEYBOARD_MAXIMUM_TEXT : maximum;
	while (text && text[state->length] && state->length < state->maximum)
	{
		state->text[state->length] = text[state->length];
		state->length++;
	}
	state->text[state->length] = 0;
	state->cursor = state->length;
	state->capital_first = capital_first;
	state->offered = offered;
	state->context = context;
	state->row = 1;
	keyboard_capital(state);
}

int keyboard_type(struct keyboard_state *state, unsigned short character)
{
	if (!keyboard_offered(state, character) || state->length >= state->maximum)
		return FALSE;
	memmove(&state->text[state->cursor + 1], &state->text[state->cursor],
		(size_t)(state->length - state->cursor + 1) * sizeof(state->text[0]));
	state->text[state->cursor++] = character;
	state->length++;
	if (!state->caps)
		state->shift = FALSE;
	return TRUE;
}

int keyboard_backspace(struct keyboard_state *state)
{
	if (state->cursor <= 0)
		return FALSE;
	memmove(&state->text[state->cursor - 1], &state->text[state->cursor],
		(size_t)(state->length - state->cursor + 1) * sizeof(state->text[0]));
	state->cursor--;
	state->length--;
	keyboard_capital(state);
	return TRUE;
}

int keyboard_cursor(struct keyboard_state *state, short characters)
{
	short cursor = (short)(state->cursor + characters);

	cursor = cursor < 0 ? 0 : cursor > state->length ? state->length : cursor;
	if (cursor == state->cursor)
		return FALSE;
	state->cursor = cursor;
	return TRUE;
}

void keyboard_shift(struct keyboard_state *state, unsigned long now)
{
	if (state->caps)
	{
		state->caps = FALSE;
		state->shift = FALSE;
	}
	else if (state->shift && state->shift_at && now - state->shift_at < KEYBOARD_CAPS_MILLISECONDS)
	{
		state->caps = TRUE;
		state->shift = TRUE;
	}
	else
		state->shift = !state->shift;
	state->shift_at = now;
}

void keyboard_page(struct keyboard_state *state, short page)
{
	if (page < 0 || page >= KEYBOARD_PAGES)
		return;
	state->page = page;
	state->row = 1;
	state->column = 0;
}

/* a key's middle across its row, in tenths */
static short keyboard_key_middle(struct keyboard_row const *keys, short column)
{
	short x = keys->indent;
	short index;

	for (index = 0; index < column; index++)
		x = (short)(x + keys->keys[index].width);
	return (short)(x + keys->keys[column].width / 2);
}

void keyboard_move(struct keyboard_state *state, short columns, short rows)
{
	struct keyboard_row const *keys = keyboard_row_of(state, state->row);

	if (!keys || !keys->count)
		return;
	if (columns)
		state->column = (short)(((state->column + columns) % keys->count + keys->count) % keys->count);
	if (rows)
	{
		short middle = keyboard_key_middle(keys, state->column);
		struct keyboard_row const *next;
		short column, best = 0, best_distance = 0x7FFF;

		state->row = (short)(((state->row + rows) % KEYBOARD_ROWS + KEYBOARD_ROWS) % KEYBOARD_ROWS);
		next = keyboard_row_of(state, state->row);
		for (column = 0; next && column < next->count; column++)
		{
			short distance = (short)(keyboard_key_middle(next, column) - middle);

			distance = distance < 0 ? (short)-distance : distance;
			if (distance < best_distance)
			{
				best_distance = distance;
				best = column;
			}
		}
		state->column = best;
	}
}

int keyboard_press(struct keyboard_state *state, unsigned long now)
{
	struct keyboard_key const *key = keyboard_key_at(state, state->row, state->column);

	state->holding = FALSE;
	state->held_typed = 0;
	if (!key)
		return KEYBOARD_NOTHING;
	switch (key->kind)
	{
	case KEYBOARD_KEY_CHARACTER:
	{
		unsigned short character = keyboard_key_character(state, key);
		int shifted = state->shift || state->caps;

		if (!keyboard_type(state, character))
			return KEYBOARD_REFUSED;
		state->holding = TRUE;
		state->held_since = now;
		state->held_typed = character;
		state->held_shifted = shifted;
		return KEYBOARD_TYPED;
	}
	case KEYBOARD_KEY_SHIFT:
		keyboard_shift(state, now);
		return KEYBOARD_CHANGED;
	case KEYBOARD_KEY_BACKSPACE:
		return keyboard_backspace(state) ? KEYBOARD_TYPED : KEYBOARD_REFUSED;
	case KEYBOARD_KEY_PAGE:
		keyboard_page(state, key->page);
		return KEYBOARD_CHANGED;
	case KEYBOARD_KEY_SPACE:
		return keyboard_type(state, ' ') ? KEYBOARD_TYPED : KEYBOARD_REFUSED;
	case KEYBOARD_KEY_DONE:
		return KEYBOARD_DONE;
	}
	return KEYBOARD_NOTHING;
}

/* the accents of a character offered (its own first, then those the text
takes and the font draws): their count, 0 where it has none offered */
static short keyboard_accents_offered(struct keyboard_state const *state, unsigned short character,
	unsigned short *accents)
{
	unsigned short variants[KEYBOARD_MAXIMUM_ACCENTS];
	int count = keyboard_accents_of(character, variants, KEYBOARD_MAXIMUM_ACCENTS - 1);
	short offered = 0;
	int index;

	/* (a layout's own letter with a mark, ñ: its plain letter's, n and its
	others, after it) */
	if (!count && keyboard_accent_plain(character))
	{
		variants[0] = keyboard_accent_plain(character);
		count = 1 + keyboard_accents_of(variants[0], variants + 1, KEYBOARD_MAXIMUM_ACCENTS - 2);
	}
	accents[offered++] = character;
	for (index = 0; index < count; index++)
	{
		if (variants[index] != character && keyboard_offered(state, variants[index]))
			accents[offered++] = variants[index];
	}
	return offered > 1 ? offered : 0;
}

int keyboard_hold(struct keyboard_state *state, unsigned long now)
{
	short count;

	if (!state->holding || state->accents_open || now - state->held_since < KEYBOARD_HOLD_MILLISECONDS)
		return FALSE;
	state->holding = FALSE;
	count = keyboard_accents_offered(state, state->held_typed, state->accents);
	if (!count)
		return FALSE;
	/* (the character it typed taken back: the accent chosen is typed in its
	place) */
	keyboard_backspace(state);
	if (state->held_shifted && !state->caps)
		state->shift = TRUE;
	state->accents_open = TRUE;
	state->accents_sticky = FALSE;
	state->accent_count = count;
	state->accent_chosen = 0;
	state->accent_row = state->row;
	state->accent_column = state->column;
	return TRUE;
}

int keyboard_release(struct keyboard_state *state)
{
	state->holding = FALSE;
	if (!state->accents_open || state->accents_sticky)
		return FALSE;
	return keyboard_accents_pick(state);
}

int keyboard_accents_open(struct keyboard_state *state, short row, short column)
{
	struct keyboard_key const *key = keyboard_key_at(state, row, column);
	short count;

	if (!key || key->kind != KEYBOARD_KEY_CHARACTER)
		return FALSE;
	count = keyboard_accents_offered(state, keyboard_key_character(state, key), state->accents);
	if (!count)
		return FALSE;
	state->row = row;
	state->column = column;
	state->holding = FALSE;
	state->accents_open = TRUE;
	state->accents_sticky = TRUE;
	state->accent_count = count;
	state->accent_chosen = 0;
	state->accent_row = row;
	state->accent_column = column;
	return TRUE;
}

void keyboard_accents_step(struct keyboard_state *state, short step)
{
	if (!state->accents_open || !state->accent_count)
		return;
	state->accent_chosen = (short)(((state->accent_chosen + step) % state->accent_count + state->accent_count) %
		state->accent_count);
}

int keyboard_accents_pick(struct keyboard_state *state)
{
	unsigned short character;

	if (!state->accents_open)
		return FALSE;
	character = state->accents[state->accent_chosen];
	keyboard_accents_close(state);
	return keyboard_type(state, character);
}

void keyboard_accents_close(struct keyboard_state *state)
{
	state->accents_open = FALSE;
	state->accents_sticky = FALSE;
	state->accent_count = 0;
	state->accent_chosen = 0;
}

int keyboard_repeat_step(struct keyboard_repeat *repeat, int held, unsigned long now)
{
	if (!held)
	{
		repeat->held = FALSE;
		return 0;
	}
	if (!repeat->held)
	{
		repeat->held = TRUE;
		repeat->next = now + KEYBOARD_REPEAT_FIRST_MILLISECONDS;
		return 1;
	}
	if ((long)(now - repeat->next) < 0)
		return 0;
	repeat->next = now + KEYBOARD_REPEAT_MILLISECONDS;
	return 1;
}

/* ---------- where it is drawn */

/* (the block's room about its keys, the text's box, the prompts' row) */
#define KEYBOARD_PADDING 16
#define KEYBOARD_TITLE_ROOM 30
#define KEYBOARD_CAPTION_ROOM 22
#define KEYBOARD_TEXT_HEIGHT 32
#define KEYBOARD_TEXT_GAP 12
#define KEYBOARD_PROMPTS_GAP 10
#define KEYBOARD_PROMPTS_HEIGHT 22

void keyboard_geometry_get(int large, struct keyboard_geometry *geometry)
{
	short keys_width, keys_height;

	memset(geometry, 0, sizeof(*geometry));
	geometry->key_unit = (short)(large ? 58 : 52);
	geometry->key_height = (short)(large ? 42 : 34);
	geometry->row_gap = (short)(large ? 6 : 5);
	keys_width = (short)(geometry->key_unit * KEYBOARD_ROW_WIDTH / 10);
	keys_height = (short)(KEYBOARD_ROWS * geometry->key_height + (KEYBOARD_ROWS - 1) * geometry->row_gap);
	geometry->width = (short)(keys_width + 2 * KEYBOARD_PADDING);
	geometry->height = (short)(KEYBOARD_PADDING + KEYBOARD_TITLE_ROOM + KEYBOARD_CAPTION_ROOM + KEYBOARD_TEXT_HEIGHT +
		KEYBOARD_TEXT_GAP + keys_height + KEYBOARD_PROMPTS_GAP + KEYBOARD_PROMPTS_HEIGHT + KEYBOARD_PADDING);
	/* (in the middle of the menus' 640 by 480: past 4:3 the room either side
	is margins; a split screen's view has the 640 by 480 scaled into it) */
	geometry->left = (short)((640 - geometry->width) / 2);
	geometry->top = (short)((480 - geometry->height) / 2);
	geometry->title_y = (short)(geometry->top + KEYBOARD_PADDING);
	geometry->caption_y = (short)(geometry->title_y + KEYBOARD_TITLE_ROOM);
	geometry->text_y = (short)(geometry->caption_y + KEYBOARD_CAPTION_ROOM);
	geometry->text_height = KEYBOARD_TEXT_HEIGHT;
	geometry->keys_x = (short)(geometry->left + KEYBOARD_PADDING);
	geometry->keys_y = (short)(geometry->text_y + KEYBOARD_TEXT_HEIGHT + KEYBOARD_TEXT_GAP);
	geometry->prompts_y = (short)(geometry->keys_y + keys_height + KEYBOARD_PROMPTS_GAP);
}

int keyboard_key_box(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short row,
	short column, struct keyboard_box *box)
{
	struct keyboard_row const *keys = keyboard_row_of(state, row);
	short x, index;

	if (!keys || column < 0 || column >= keys->count)
		return FALSE;
	x = keys->indent;
	for (index = 0; index < column; index++)
		x = (short)(x + keys->keys[index].width);
	box->x0 = (short)(geometry->keys_x + x * geometry->key_unit / 10);
	box->x1 = (short)(geometry->keys_x + (x + keys->keys[column].width) * geometry->key_unit / 10);
	box->y0 = (short)(geometry->keys_y + row * (geometry->key_height + geometry->row_gap));
	box->y1 = (short)(box->y0 + geometry->key_height);
	return TRUE;
}

int keyboard_key_find(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short x,
	short y, short *row, short *column)
{
	short each_row, each_column;

	for (each_row = 0; each_row < KEYBOARD_ROWS; each_row++)
	{
		struct keyboard_row const *keys = keyboard_row_of(state, each_row);

		for (each_column = 0; keys && each_column < keys->count; each_column++)
		{
			struct keyboard_box box;

			if (keyboard_key_box(state, geometry, each_row, each_column, &box) && x >= box.x0 && x < box.x1 &&
				y >= box.y0 && y < box.y1 + geometry->row_gap)
			{
				*row = each_row;
				*column = each_column;
				return TRUE;
			}
		}
	}
	return FALSE;
}

int keyboard_accent_box(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short index,
	struct keyboard_box *box)
{
	struct keyboard_box key;
	short width = geometry->key_unit, left, right_edge = (short)(geometry->left + geometry->width);

	if (!state->accents_open || index < 0 || index >= state->accent_count ||
		!keyboard_key_box(state, geometry, state->accent_row, state->accent_column, &key))
	{
		return FALSE;
	}
	/* (a row of them over the key, from its left, kept on the block: as
	narrow as fits it, where they are many) */
	if (state->accent_count * width > geometry->width)
		width = (short)(geometry->width / state->accent_count);
	left = key.x0;
	if (left + state->accent_count * width > right_edge)
		left = (short)(right_edge - state->accent_count * width);
	if (left < geometry->left)
		left = geometry->left;
	box->x0 = (short)(left + index * width);
	box->x1 = (short)(box->x0 + width);
	box->y1 = (short)(key.y0 - 2);
	box->y0 = (short)(box->y1 - geometry->key_height);
	return TRUE;
}

short keyboard_accent_find(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short x,
	short y)
{
	short index;

	for (index = 0; index < state->accent_count; index++)
	{
		struct keyboard_box box;

		if (keyboard_accent_box(state, geometry, index, &box) && x >= box.x0 && x < box.x1 && y >= box.y0 &&
			y < box.y1)
		{
			return index;
		}
	}
	return NONE;
}
