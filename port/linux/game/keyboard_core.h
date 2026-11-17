/*
KEYBOARD_CORE.H

The on-screen keyboard's rules, apart from the game (port/linux/game/
keyboard_core.c; the game's side, its screen and its input, is keyboard.c):
its layouts as data, the text typed, the key focused and its moves, shift and
caps lock, the pages, the accents a held key offers, the repeats of a held
key, and where its keys are on the menus' 640 by 480. Nothing here reads the
game, so the harness compiles it as it is (tools/harness/tests/
test_keyboard.py).
*/

#ifndef KEYBOARD_CORE_H
#define KEYBOARD_CORE_H

/* the most characters a text may have, the rows of a page, the keys of a
row, the pages of a layout, and the accents a key offers (its own first) */
#define KEYBOARD_MAXIMUM_TEXT 127
#define KEYBOARD_ROWS 5
#define KEYBOARD_MAXIMUM_ROW_KEYS 12
#define KEYBOARD_PAGES 2
#define KEYBOARD_MAXIMUM_ACCENTS 12

/* a key's width, and a row's, in tenths of a letter's key: a row is at most
KEYBOARD_ROW_WIDTH */
#define KEYBOARD_ROW_WIDTH 100

/* holding a key this long offers its accents; a held key or direction acts
again after the first delay, then at each repeat; two presses of shift this
close together are caps lock */
#define KEYBOARD_HOLD_MILLISECONDS 400
#define KEYBOARD_REPEAT_FIRST_MILLISECONDS 400
#define KEYBOARD_REPEAT_MILLISECONDS 70
#define KEYBOARD_CAPS_MILLISECONDS 400

enum keyboard_key_kind
{
	KEYBOARD_KEY_NONE,
	KEYBOARD_KEY_CHARACTER,
	KEYBOARD_KEY_SHIFT,
	KEYBOARD_KEY_BACKSPACE,
	KEYBOARD_KEY_PAGE,
	KEYBOARD_KEY_SPACE,
	KEYBOARD_KEY_DONE
};

/* a key: what it is, its width (tenths), its character and its shifted one
(a character's), the page it shows (a page key's) and its words (a key
that is not a character's) */
struct keyboard_key
{
	unsigned char kind;
	unsigned char width;
	unsigned short character;
	unsigned short shifted;
	unsigned char page;
	char const *label;
};

/* a row: how far in its first key is (tenths), and its keys */
struct keyboard_row
{
	unsigned char indent;
	unsigned char count;
	struct keyboard_key keys[KEYBOARD_MAXIMUM_ROW_KEYS];
};

struct keyboard_page
{
	struct keyboard_row rows[KEYBOARD_ROWS];
};

/* a layout (#1136: QWERTY, AZERTY, QWERTZ, the Nordic ones and the others,
each its letters' rows; the number row and the signs' page are every one's):
its name (input.keyboard_layout's value), the languages and places it is
the one of ("fr", "fr_CH": an automatic choice's, keyboard_layout_auto), and
its pages */
struct keyboard_layout
{
	char const *name;
	char const *locales;
	struct keyboard_page pages[KEYBOARD_PAGES];
};

/* a key's accents: its character, then the letters with a mark it offers */
struct keyboard_accents
{
	unsigned short base;
	unsigned short variants[KEYBOARD_MAXIMUM_ACCENTS];
};

/* a held button's or key's repeats */
struct keyboard_repeat
{
	int held;
	unsigned long next;
};

/* what the keyboard says of a character: whether its text takes it (a
name's, a field's, a chat line's) and its font draws it */
typedef int (*keyboard_offered_function)(unsigned short character, void *context);

/* the keyboard: its text, the cursor in it, the key focused, shift and caps
lock, the page, the accents open, and the key held */
struct keyboard_state
{
	struct keyboard_layout const *layout;
	unsigned short text[KEYBOARD_MAXIMUM_TEXT + 1];
	short length;
	short cursor;
	short maximum;
	short page;
	short row;
	short column;
	int shift;
	int caps;
	unsigned long shift_at;
	/* a name's first letter a capital */
	int capital_first;
	keyboard_offered_function offered;
	void *context;
	/* the accents offered for a key held: open, theirs, the one chosen, and
	whether they stay open after the pointer's button is let go (a right
	click opened them) */
	int accents_open;
	int accents_sticky;
	short accent_count;
	short accent_chosen;
	short accent_row;
	short accent_column;
	unsigned short accents[KEYBOARD_MAXIMUM_ACCENTS];
	/* the key held: since when, the character it typed (taken back if its
	accents open), and whether shift was on for it */
	int holding;
	unsigned long held_since;
	unsigned short held_typed;
	int held_shifted;
};

/* where the keyboard is drawn, on the menus' 640 by 480 (each screen
size's: a view of split screen is the 640 by 480 scaled into it, menu_view.c):
the block's left, top, width and height, its title's, caption's, text's,
keys' and prompts' tops, a key's unit width and height and the gap between
rows */
struct keyboard_geometry
{
	short left;
	short top;
	short width;
	short height;
	short title_y;
	short caption_y;
	short text_y;
	short text_height;
	short keys_x;
	short keys_y;
	short key_unit;
	short key_height;
	short row_gap;
	short prompts_y;
};

struct keyboard_box
{
	short x0, y0, x1, y1;
};

/* the layouts (QWERTY first), and one by name (the first, for none or one
not there) */
extern struct keyboard_layout const keyboard_layouts[];
extern long const keyboard_layout_count;
struct keyboard_layout const *keyboard_layout_named(char const *name);
/* the layout of the system's languages (locales: theirs, the first the most
wanted, each "fr_FR", "fr-FR" or "fr", apart by spaces or commas): the first
of them a layout is the one of, its place's before its language's; else the
game's language's (game.language: "de", "fr"...); else QWERTY */
struct keyboard_layout const *keyboard_layout_auto(char const *locales, char const *language);

/* the keyboard begun on a text (its maximum characters, at most
KEYBOARD_MAXIMUM_TEXT), the cursor at its end, the letters' page, the first
letter's key focused; capital_first: a name's, shift on while it is empty */
void keyboard_begin(struct keyboard_state *state, struct keyboard_layout const *layout, unsigned short const *text,
	short maximum, int capital_first, keyboard_offered_function offered, void *context);
/* a key of the page shown (NULL for none) */
struct keyboard_key const *keyboard_key_at(struct keyboard_state const *state, short row, short column);
/* the character a character's key types now (shift, caps lock: a letter's
capital; a sign keeps its own); a layout's own letter with a mark (ñ, ö, å)
that the text does not take or the font does not draw types its plain
letter (n, o, a) and shows it */
unsigned short keyboard_key_character(struct keyboard_state const *state, struct keyboard_key const *key);
/* whether a character is offered: the text takes it and its font draws it */
int keyboard_offered(struct keyboard_state const *state, unsigned short character);
/* a character put in at the cursor: FALSE if the text does not take it or
is full; shift (not caps lock) let go after it */
int keyboard_type(struct keyboard_state *state, unsigned short character);
/* the character before the cursor taken out: FALSE for none */
int keyboard_backspace(struct keyboard_state *state);
/* the cursor moved by characters, kept in the text: FALSE if it did not move */
int keyboard_cursor(struct keyboard_state *state, short characters);
/* shift pressed: on, off, or caps lock (two presses close together), off
again from caps lock */
void keyboard_shift(struct keyboard_state *state, unsigned long now);
/* a page shown, its first character's key focused */
void keyboard_page(struct keyboard_state *state, short page);
/* the focus moved a key right (columns) and down (rows), round each row and
the rows; down and up go to the key of the next row nearest under it */
void keyboard_move(struct keyboard_state *state, short columns, short rows);

/* what pressing the focused key did */
enum keyboard_result
{
	KEYBOARD_NOTHING,
	KEYBOARD_TYPED,
	KEYBOARD_REFUSED,
	KEYBOARD_CHANGED,
	KEYBOARD_DONE
};

/* the focused key pressed (a character's types; shift, the page, space,
backspace and done do theirs), and held from now: a character's key held
KEYBOARD_HOLD_MILLISECONDS offers its accents (keyboard_hold) */
int keyboard_press(struct keyboard_state *state, unsigned long now);
/* the key pressed still held, now: TRUE as its accents open (the character
it typed taken back, the accents its own first, then those the text takes and
the font draws: a key with none opens nothing) */
int keyboard_hold(struct keyboard_state *state, unsigned long now);
/* the key pressed let go of: the accent chosen typed if they were open (not
sticky ones, which a pointer's click picks); TRUE if one was */
int keyboard_release(struct keyboard_state *state);
/* the accents of a key opened at once (a right click's: sticky), the key
focused: FALSE if it has none */
int keyboard_accents_open(struct keyboard_state *state, short row, short column);
/* the accent chosen moved, kept among them */
void keyboard_accents_step(struct keyboard_state *state, short step);
/* the accent chosen typed and the accents closed; or closed with none */
int keyboard_accents_pick(struct keyboard_state *state);
void keyboard_accents_close(struct keyboard_state *state);
/* a character's accents, as the table has them (none: 0); for tests and the
key's mark that it has some */
int keyboard_accents_of(unsigned short character, unsigned short *variants, int maximum);
/* the plain letter of a letter with a mark (n of ñ, A of Å, a of æ: the
accents' table's letter it is among), 0 for one that is none's */
unsigned short keyboard_accent_plain(unsigned short character);

/* a held button's repeats: 1 as it is pressed, then after the first delay
and at each repeat while held, else 0 */
int keyboard_repeat_step(struct keyboard_repeat *repeat, int held, unsigned long now);

/* where the keyboard is drawn (large: bigger keys, the Steam Deck's) */
void keyboard_geometry_get(int large, struct keyboard_geometry *geometry);
/* a key's box on the page shown; FALSE for none */
int keyboard_key_box(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short row,
	short column, struct keyboard_box *box);
/* the key at a point; FALSE for none */
int keyboard_key_find(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short x,
	short y, short *row, short *column);
/* an accent's box over its key, kept on the block; FALSE for none */
int keyboard_accent_box(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short index,
	struct keyboard_box *box);
/* the accent at a point: its index, NONE (-1) for none */
short keyboard_accent_find(struct keyboard_state const *state, struct keyboard_geometry const *geometry, short x,
	short y);

#endif
