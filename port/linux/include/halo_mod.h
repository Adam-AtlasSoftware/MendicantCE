/*
HALO_MOD.H

What the ports offer Forge's parts (port/linux/game/forge_ai.c and the
files beside it, listed in halo_mods.c) and source mods, so that they need not change the game's files to run or to read keys: each
is a struct halo_mod of its own file, and reads the keyboard with
halo_mod_key_down. Forge's parts are in halo_mods.c's list, in every build; a
source mod of a native Linux build registers its own with
HALO_MOD_REGISTER, after them.

Shared by the game, through halo_linux_source_fixups.h, and the platform
layer (port/linux/src/xinput_sdl.c). The hooks are called by
port/linux/game/halo_mods.c.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __HALO_MOD_H
#define __HALO_MOD_H

struct halo_layout_writer;

/* ---------- hooks */

/* a page of the dev tools' menu (1, D-pad right or, flying, X; Forge games)
that a mod adds after the map's object tabs: rows of a label and a value, driven with
the menu's own keys and buttons, so it works on a keyboard and a
controller alike. Up and down choose a row; left and right change its
value; enter, the left mouse button or A select it (an action). LB / RB (Page
Up / Page Down), and T and V, go to the previous and next tab. */
struct halo_mod_menu
{
	/* the tab's name */
	char const *title;
	/* how many rows there are now (it may change while the menu is open) */
	short (*row_count)(void);
	/* row's label, and its value for the right column: "" for an action or
	a note */
	void (*row_text)(short row, char *label, unsigned long label_size, char *value, unsigned long value_size);
	/* direction -1 or +1 (left, right): change the row's value; 0 (select):
	do the row's action. Returns TRUE to close the menu (so an action that
	needs the crosshair can be seen). May be NULL for a page of notes. */
	int (*row_change)(short row, int direction);
	/* the page has just been shown; may be NULL */
	void (*opened)(void);
};

struct halo_mod
{
	char const *name;
	/* once a frame while a game is in progress and the console is closed,
	before the dev tools (forge_update); may be NULL */
	void (*update)(void);
	/* once a frame over the other overlays, before the dev tools
	(forge_render); may be NULL */
	void (*render)(void);
	/* once a game tick (30 a second, whatever the frame rate), at the
	start of game_tick (game/game.c) before units and physics update, in
	local and system link games (not saved films): the place for changes to
	the game state such as gravity. In system link the host's objects are
	the game's (network_distributed.c) and its clients copy them: what
	decides the game (killing, teleporting) only where
	halo_mods_authoritative() is TRUE; what every machine's physics needs
	(gravity) everywhere. May be NULL */
	void (*tick)(void);
	/* once a map has been loaded and its objects placed, before its first
	tick, in every game (local or not): the place to undo what the last
	map changed; may be NULL */
	void (*new_map)(void);
	/* once a frame in each local player's view, after the game's own debug
	drawing (render_debug, render/render.c) and before its debug geometry
	is drawn: rasterizer_debug_triangle and rasterizer_debug_line_shaded
	(rasterizer/rasterizer.h) then draw in the world, with the
	view's camera and depth; may be NULL */
	void (*render_world)(void);
	/* a page in the dev tools' menu; may be NULL */
	struct halo_mod_menu const *menu;
	/* the tools' pick up (F4, or enter / A while flying) just before they
	look for an object to pick up: return TRUE when the mod takes the
	press (it picks up something of its own, with forge_mod_hold_begin); may be
	NULL */
	int (*grab)(void);
	/* Y while flying, with nothing held and the menu closed: return TRUE
	when the mod removes the object under the crosshair itself (so it can
	bring it back); else the tools remove it. May be NULL */
	int (*remove_object)(void);

	/* layouts (port/linux/game/forge_layout.c): a layout keeps what Forge
	changed in a map, the tools' objects and each mod's part. layout_save
	writes the mod's part, lines of text (halo_layout_printf, no line
	breaks); layout_clear puts the mod's part back to how the map has it;
	layout_load gets the lines of the mod's part one by one, after a
	layout_clear. All three may be NULL. The lines are a file's or, on a
	system link client, the host's: layout_load checks what it takes from
	them. */
	void (*layout_save)(struct halo_layout_writer *writer);
	void (*layout_clear)(void);
	void (*layout_load)(char const *line);

	/* X on a controller while flying, with nothing held and the menu
	closed: return TRUE, and how far from the camera it is, when the
	crosshair is on something the mod's page is about (a zone, a spawn, an
	object). The tools' menu then opens on the page of the mod whose thing
	is nearest, and the page's opened hook chooses it; with none, the menu
	opens where it was. May be NULL */
	int (*properties)(float *distance);
	/* a page shown alone, as a popup beside the crosshair, in place of the
	menu when the properties hook's thing is the nearest: the thing's own
	rows (an object's size and turn), without the tabs; its opened hook
	chooses the thing. May be NULL: the menu then opens on the mod's page */
	struct halo_mod_menu const *popup;
};

/* writes one line of a mod's part of a layout (layout_save) */
struct halo_layout_writer;
void halo_layout_printf(struct halo_layout_writer *writer, char const *format, ...);

/* at most this many mods run; more are refused with a message on stderr */
#define HALO_MOD_MAXIMUM_COUNT 32

void halo_mod_register(struct halo_mod const *mod);
void halo_mods_update(void);
void halo_mods_render(void);
void halo_mods_tick(void);
/* TRUE in a local game and on the host of a system link game, whose game
state is the game's; FALSE on a system link client */
int halo_mods_authoritative(void);
void halo_mods_new_map(void);
void halo_mods_render_world(void);
/* the menu pages of every mod, in registration order (game/forge.c) */
int halo_mods_grab(void);
int halo_mods_remove(void);
/* the layout hooks of every mod (forge_layout.c) */
short halo_mods_count(void);
struct halo_mod const *halo_mods_get(short index);
short halo_mods_menu_page_count(void);
/* the menu page (as halo_mods_menu_page counts them) of the mod whose thing
under the crosshair is nearest (the properties hook), or -1 (NONE); when
that mod has a popup, the popup, and -1 unless it has a page too */
short halo_mods_properties_page(struct halo_mod_menu const **popup);
struct halo_mod_menu const *halo_mods_menu_page(short index);

/* registers a source mod's struct halo_mod before the game starts, after
Forge's own parts (halo_mods.c) */
#define HALO_MOD_REGISTER(mod) \
	static void __attribute__((constructor)) halo_mod_register_##mod(void) \
	{ \
		halo_mod_register(&(mod)); \
	}

/* ---------- keys */

/* keyboard keys: SDL scancodes, which are the USB HID usage numbers
(xinput_sdl.c checks that they agree) */
enum
{
	HALO_MOD_KEY_A = 4,
	HALO_MOD_KEY_B,
	HALO_MOD_KEY_C,
	HALO_MOD_KEY_D,
	HALO_MOD_KEY_E,
	HALO_MOD_KEY_F,
	HALO_MOD_KEY_G,
	HALO_MOD_KEY_H,
	HALO_MOD_KEY_I,
	HALO_MOD_KEY_J,
	HALO_MOD_KEY_K,
	HALO_MOD_KEY_L,
	HALO_MOD_KEY_M,
	HALO_MOD_KEY_N,
	HALO_MOD_KEY_O,
	HALO_MOD_KEY_P,
	HALO_MOD_KEY_Q,
	HALO_MOD_KEY_R,
	HALO_MOD_KEY_S,
	HALO_MOD_KEY_T,
	HALO_MOD_KEY_U,
	HALO_MOD_KEY_V,
	HALO_MOD_KEY_W,
	HALO_MOD_KEY_X,
	HALO_MOD_KEY_Y,
	HALO_MOD_KEY_Z,
	HALO_MOD_KEY_1,
	HALO_MOD_KEY_2,
	HALO_MOD_KEY_3,
	HALO_MOD_KEY_4,
	HALO_MOD_KEY_5,
	HALO_MOD_KEY_6,
	HALO_MOD_KEY_7,
	HALO_MOD_KEY_8,
	HALO_MOD_KEY_9,
	HALO_MOD_KEY_0,
	HALO_MOD_KEY_RETURN,
	HALO_MOD_KEY_ESCAPE,
	HALO_MOD_KEY_BACKSPACE,
	HALO_MOD_KEY_TAB,
	HALO_MOD_KEY_SPACE,
	HALO_MOD_KEY_F1 = 58,
	HALO_MOD_KEY_F2,
	HALO_MOD_KEY_F3,
	HALO_MOD_KEY_F4,
	HALO_MOD_KEY_F5,
	HALO_MOD_KEY_F6,
	HALO_MOD_KEY_F7,
	HALO_MOD_KEY_F8,
	HALO_MOD_KEY_F9,
	HALO_MOD_KEY_F10,
	HALO_MOD_KEY_F11,
	HALO_MOD_KEY_F12,
	HALO_MOD_KEY_INSERT = 73,
	HALO_MOD_KEY_HOME,
	HALO_MOD_KEY_PAGE_UP,
	HALO_MOD_KEY_DELETE,
	HALO_MOD_KEY_END,
	HALO_MOD_KEY_PAGE_DOWN,
	HALO_MOD_KEY_RIGHT,
	HALO_MOD_KEY_LEFT,
	HALO_MOD_KEY_DOWN,
	HALO_MOD_KEY_UP,
	HALO_MOD_KEY_LEFT_CTRL = 224,
	HALO_MOD_KEY_LEFT_SHIFT,
	HALO_MOD_KEY_LEFT_ALT,
	HALO_MOD_KEY_RIGHT_CTRL = 228,
	HALO_MOD_KEY_RIGHT_SHIFT,
	HALO_MOD_KEY_RIGHT_ALT,

	/* either side */
	HALO_MOD_KEY_CTRL = 0x1000,
	HALO_MOD_KEY_SHIFT,
	HALO_MOD_KEY_ALT
};

/* TRUE while the key is held; always FALSE while the console is open */
int halo_mod_key_down(int key);
/* the key's name, as the keyboard labels it ("F5", "Delete", "Ctrl") */
char const *halo_mod_key_name(int key);
/* from now on, while either Ctrl is held, key reaches neither the game's
controller 1 (so Ctrl+C does not also crouch) nor the dev tools */
void halo_mod_ctrl_shortcut(int key);

/* a key that works when it goes down, not while it is held */
struct halo_mod_key_state
{
	int down;
};

/* TRUE on the frame the key (or, with ctrl, Ctrl and the key) goes down;
ctrl FALSE ignores Ctrl, ctrl TRUE needs it */
int halo_mod_key_pressed(struct halo_mod_key_state *state, int key, int ctrl);

/* ---------- drawing, from a render hook */

/* screen coordinates: 640x480 for one player, whatever the window's size */

enum
{
	HALO_MOD_TEXT_LEFT = 0,
	HALO_MOD_TEXT_RIGHT,
	HALO_MOD_TEXT_CENTER
};

/* local player 0's view; FALSE when there is none (menus, loading) */
int halo_mod_screen(short *x0, short *y0, short *x1, short *y1);
/* the terminal font, or with large the map's largest font that holds
ordinary text and still fits 18 lines on the screen; NONE (-1) when no map
is loaded */
long halo_mod_font(int large);
/* the map's largest font that holds ordinary text and still fits that many
lines on the screen (at least the terminal font) */
long halo_mod_font_for_lines(short lines);
short halo_mod_line_height(long font);
short halo_mod_text_width(long font, char const *text);
/* text in the box, colour 0xAARRGGBB */
void halo_mod_draw_text(long font, short x0, short y0, short x1, short y1, int justification,
	unsigned long argb, char const *text);
/* a filled box, colour 0xAARRGGBB, blended by its alpha */
void halo_mod_draw_box(short x0, short y0, short x1, short y1, unsigned long argb);
/* the same with corners between the screen's units, for shapes thinner than
one (the display has several pixels to a unit) */
void halo_mod_draw_box_real(float x0, float y0, float x1, float y1, unsigned long argb);
/* a filled triangle, its corners clockwise on the screen, in one draw (as
halo_mod_draw_box_real, between the screen's units) */
void halo_mod_draw_triangle_real(float x0, float y0, float x1, float y1, float x2, float y2, unsigned long argb);

#endif
