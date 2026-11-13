/*
FORGE.C

In-game dev tools for the native ports (port/linux, port/windows,
port/android): on in local and system link games of the Forge game type,
which the main menu's FORGE plays (a slayer variant with GAME_VARIANT_FORGE_FLAG,
game/game_engine.c; it is not one of the gametypes to choose; see
port/linux/README.md, "Dev tools"). The byte-matching build never compiles
this file.

	Home / D-pad up
	        forge mode: the player becomes the monitor and flies
	        (forge_monitor.c); again gives them their body back.
	        Without the monitor (game.forge_monitor off, or no c40.map):
	        detach the camera from the player and fly it freely, through
	        walls; again moves the player to the camera and returns the
	        camera to the player
	1 / D-pad right or X, only while flying
	        open or close the spawn menu: every vehicle, weapon, equipment,
	        biped and scenery object of the map (the menu is part of forge
	        mode: landing the camera closes it). X with the crosshair on a
	        spawn point, a zone or an object opens it on the tab that is
	        about that thing (the mods' properties hook, halo_mod.h), with
	        it chosen
	F4, and while flying also enter, the left mouse button or A
	        pick up the object under the crosshair
	Y, while flying
	        remove the object under the crosshair (Delete with forge_edit)

While flying: W A S D (left stick) move level, the way the view faces
whatever its pitch, the mouse (right stick) looks, R and F (RB and LB) rise
and sink straight up and down, and shift (the left trigger, LT) is faster:
the monitor flies its unit so (units/bipeds.c), the free camera itself. The
rest of this paragraph is the free camera's: the arrows' up and down change
the speed. Z (right stick click) hands the controls back to the player and
leaves the camera where it is, as the game's own debug flying camera does
(camera/director.c).

In the menu: up and down choose, left and right change the category, enter,
the left mouse button or A take the object, escape, backspace, the right
mouse button or B close the menu. After the object categories come the
pages that source mods add (halo_mod.h, struct halo_mod_menu): rows with a
value that left and right change and that enter or A act on. T and V, or LB
and RB, go to the previous and next tab on every page; X closes the menu too.
The mouse's wheel chooses as up and down do, and with the left button held
on a mod's row changes its value as left and right do.

The menu is drawn after the forge
menu of Halo: Reach: a navy panel with the tab's name and its place among
the tabs (the map's objects by kind, then the mods' pages) between LB and
RB, the tab's rows under it (how many of each object the map has, and its
name in capitals), and the A and B buttons along the bottom. The highlight
glides to the chosen row and the panel fades in. It keeps its
place while it is closed: the category, and in each category the chosen row
(by tag name, so it carries over to other maps that have the same tag) and
how far the list is scrolled. The place lasts for the whole session and is
written to u:\forge_ui.txt (forge_ui.txt in the directory u/ of the save
root, ~/.local/share/halo-linux/u/) when the menu closes, and read back the
first time it opens after the game starts. A closed menu does nothing else.

A taken or picked up object is held where the crosshair (the middle of the
view, which is not drawn) points, standing on the surface there, until it
is placed:
	left, right     turn it about the chosen axis
	up, down        raise and lower it
	T               choose the axis: yaw, pitch or roll
	V / X           choose the step: free, or 15, 45, 90 or 180 degrees
	enter, left mouse button / A
	                put it down
	escape, backspace / B
	                cancel: a spawned object goes, a picked up one returns
	right mouse button, held while flying
	                the mouse turns it: left and right about its yaw, up and
	                down about its pitch, by the chosen step
	delete / back, Y
	                remove it from the map
While flying, the right stick orbits the camera around the held object,
which then stays that far in front of the camera wherever it flies (LB and
RB lower and raise both), and with the right trigger (RT) held the right
stick turns the object instead: left and right about its yaw, up and down
about its pitch, while the left stick's forward and back take the camera
nearer to it and further away.
The platform layer keeps these keys and buttons from the game while the
menu is open or an object is held.

The keys belong to the player on controller 1, the keyboard and the first
gamepad, and act on local player 0.

System link: every player of a Forge game builds. Spawning, moving and
removing objects change the game state, which is the host's
(network_distributed.c): the host's tools change it as in a local game, and
its clients see it as they see the rest of its game (its units and items
through network_objects.c, its scenery, devices and the mods' parts through
the layout it sends, forge_layout.c). A client's tools ask the host
(forge_layout_client_edit): the menu's object is spawned where the
crosshair points, to be picked up from there; an object picked up moves on
the client alone until it is put down, which the host then does; a removed
one the host removes. The mods' pages and the Map tab are the host's.

A page may ask for a line of text (forge_text_entry_begin, the Map tab's
names and descriptions, a prefab's name, an object's comment): it is typed on
the keyboard, which until enter or escape belongs to it alone, or on an
on-screen keyboard laid out and worked as the one a profile's name is typed
on (virtual_keyboard.c, whose tags only ui.map has): the D-pad chooses a key,
A presses it, X deletes, the triggers move the cursor, start takes the line
and B leaves it. As there, the first key replaces the line it began with.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "camera/director.h"
#include "camera/director_forge.h"
#include "camera/observer.h"
#include "cseries/cseries_windows.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "interface/interface.h"
#include "interface/terminal.h"
#include "interface/ui_widget.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "math/integer_math.h"
#include "math/real_math.h"
#include "objects/object_definitions.h"
#include "objects/object_types.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/text_group.h"
#include "units/units.h"

#include <stdio.h>
#include <stdlib.h>

/* ---------- constants */

enum
{
	FORGE_LOCAL_PLAYER_INDEX = 0,
	FORGE_MAXIMUM_MENU_ENTRIES = 1024,
	FORGE_MENU_VISIBLE_ENTRIES = 10,
	FORGE_MENU_MARGIN = 24,
	/* the menu's panel (forge_ui): padding inside it, its widest, the gap
	between hints, and the fade in and highlight glide speeds (per second) */
	FORGE_MENU_PADDING = 5,
	FORGE_MENU_MAX_WIDTH = 260,
	/* a popup: how wide, and how far right of the crosshair */
	FORGE_POPUP_WIDTH = 250,
	FORGE_POPUP_OFFSET = 30,
	/* (its rows are few: all of them are shown) */
	FORGE_POPUP_VISIBLE_ENTRIES = 12,
	/* the menu's font fits this many lines on the screen: smaller than the
	mods' large font, so the menu leaves most of the view free */
	FORGE_MENU_FONT_LINES = 26,
	FORGE_MENU_HINT_GAP = 14,
	FORGE_MENU_FADE_SPEED = 8,
	FORGE_MENU_GLIDE_SPEED = 22,
	/* a menu not drawn for this long is opening again */
	FORGE_MENU_FRESH_MILLISECONDS = 150,
	FORGE_CATEGORY_COUNT = 5,
	FORGE_TAG_NAME_SIZE = 256,
	/* the sides a disc of the menu is drawn with (forge_menu_disc), and the
	steps of a rounded corner's quarter turn (forge_menu_corner) */
	FORGE_MENU_DISC_SIDES = 16,
	FORGE_MENU_CORNER_STEPS = 6,
	FORGE_KEY_REPEAT_DELAY_MILLISECONDS = 400,
	FORGE_KEY_REPEAT_INTERVAL_MILLISECONDS = 70,
	/* surfaces of objects the aim passes through (the player's own vehicle,
	objects that cannot be picked up) */
	FORGE_MAXIMUM_AIM_TESTS = 4,
	/* with a mod's objects held along with one (forge_hold_object), which
	the aim passes through too */
	FORGE_MAXIMUM_GROUP_AIM_TESTS = 32
};

/* draw_string's justifications (text/draw_string.c) */
enum
{
	_text_justification_left = 0,
	_text_justification_right,
	_text_justification_center
};

enum forge_axis
{
	_forge_axis_yaw = 0,
	_forge_axis_pitch,
	_forge_axis_roll,
	NUMBER_OF_FORGE_AXES
};

/* what can be picked up: not projectiles, sound scenery or placeholders */
#define FORGE_PICKABLE_OBJECT_MASK (_object_mask_unit | _object_mask_item | _object_mask_scenery | _object_mask_device)

/* what the pick up aim hits: the structure and every object but projectiles */
#define FORGE_PICK_COLLISION_FLAGS (FLAG(_collision_test_front_facing_surfaces_bit) | \
	FLAG(_collision_test_ignore_invisible_surfaces_bit) | \
	FLAG(_collision_test_structure_bit) | \
	FLAG(_collision_test_objects_bit) | \
	(_collision_test_objects_all_types_flags & ~FLAG(_collision_test_objects_projectiles_bit)))

/* world units the crosshair reaches; beyond it, or in the open, objects are
held this far in front of the camera instead */
#define FORGE_AIM_RANGE 200.f
#define FORGE_AIM_DISTANCE 3.f
/* how far past a surface the aim goes on when it passes through it */
#define FORGE_AIM_STEP 0.01f
/* items (weapons, equipment) lie without collision geometry and are picked
up by their bounding sphere, at least this big */
#define FORGE_ITEM_PICK_MINIMUM_RADIUS 0.25f
/* a held object turns half a circle, and rises two world units, a second */
#define FORGE_TURN_SPEED _pi
#define FORGE_RAISE_SPEED 2.f
/* how near and far an object held in front of the flying camera may be */
#define FORGE_ORBIT_MINIMUM_DISTANCE 1.5f
#define FORGE_ORBIT_MAXIMUM_DISTANCE 50.f

/* the menu's colours (forge_ui), 0xAARRGGBB: the deep navy panel and bright
blue highlight of Halo's own menus, laid out as Halo: Reach's forge menu */
#define FORGE_MENU_CAP_COLOR 0xe00a1628UL
#define FORGE_MENU_CAP_TEXT_COLOR 0xffb8d8ffUL
/* a name across the White button's face */
#define FORGE_MENU_CAP_DARK_TEXT_COLOR 0xff10141cUL
/* the edge around the Black button's face */
#define FORGE_MENU_CAP_EDGE_COLOR 0xff8a94a3UL
#define FORGE_MENU_BAR_LEFT_COLOR 0xe82896ffUL
#define FORGE_MENU_BAR_RIGHT_COLOR 0x901a5aa0UL
#define FORGE_MENU_RULE_COLOR 0x602896ffUL
#define FORGE_MENU_SCROLL_COLOR 0xc02896ffUL
#define FORGE_MENU_TITLE_TEXT_COLOR 0xfff0f2f4UL
#define FORGE_MENU_PLACE_TEXT_COLOR 0xff6ab8ffUL
#define FORGE_MENU_TEXT_COLOR 0xffdce8f4UL
#define FORGE_MENU_SELECTED_TEXT_COLOR 0xffffffffUL
#define FORGE_MENU_DIM_TEXT_COLOR 0xff5f7894UL
#define FORGE_MENU_BUTTON_A_COLOR 0xff58ad30UL
#define FORGE_MENU_BUTTON_B_COLOR 0xffd4392bUL
#define FORGE_MENU_BUTTON_TEXT_COLOR 0xffffffffUL
/* the pause menu's A and B (every map has them; the high-res redraws stand
for them, tools/button_assets.py): the art is in the top left of the bitmap,
this many texels wide and high */
#define FORGE_MENU_BUTTON_A_BITMAP "ui\\shell\\bitmaps\\a_butn_sm"
#define FORGE_MENU_BUTTON_B_BITMAP "ui\\shell\\bitmaps\\b_butn_sm"
#define FORGE_MENU_BUTTON_ART_WIDTH 16
#define FORGE_MENU_BUTTON_ART_HEIGHT 20
/* the count of objects when few more can be made, and when none can */
/* a popup is shaped as the pause menu's box is (port/assets/menus/port_svg/pause,
pausebox_*.svg), a blue border and round corners (screen units), but filled
black three quarters opaque. The menu's header and buttons are as black, its
rows the pause box's navy, as opaque; the header is opaque black. */
#define FORGE_POPUP_FILL_COLOR 0xbf000000UL
#define FORGE_MENU_HEADER_FILL_COLOR 0xff000000UL
/* the on-screen keyboard's (as the profile's name's): its panels, its chosen
key, and the line it began with until the first key replaces it */
#define FORGE_TEXT_KEYBOARD_FILL_COLOR 0xe8020814UL
#define FORGE_TEXT_KEYBOARD_CHOSEN_FILL_COLOR 0x40102a4aUL
#define FORGE_TEXT_KEYBOARD_REPLACED_FILL_COLOR 0xc0808080UL
#define FORGE_MENU_FILL_COLOR 0xbf0a2649UL
#define FORGE_POPUP_BORDER_COLOR 0xff2896ffUL
#define FORGE_POPUP_RADIUS 8.5f
#define FORGE_POPUP_BORDER 2.f
/* the chosen row of the menu and of a popup: a black box with the popup's
border and round corners */
#define FORGE_MENU_HIGHLIGHT_FILL_COLOR 0xe0000000UL
#define FORGE_MENU_BUDGET_LOW_COLOR 0xffffb347UL
#define FORGE_MENU_BUDGET_FULL_COLOR 0xffff5a4aUL

/* where the menu's place is kept (see above) */
#define FORGE_MENU_STATE_FILE "u:\\forge_ui.txt"

/* ---------- structures */

struct forge_category
{
	short object_type;
	char const *name;
};

struct forge_snap
{
	real step;
	char const *name;
};

struct forge_key
{
	boolean down;
	unsigned long repeat_milliseconds;
};

struct forge_globals
{
	long held_object_index;
	boolean held_spawned;
	short held_axis;
	short held_snap_index;
	real held_angles[NUMBER_OF_FORGE_AXES];
	real held_height;
	real_point3d held_original_position;
	real_vector3d held_original_forward;
	real_vector3d held_original_up;
	/* held in front of the flying camera, this far, since the right stick
	orbited it (rather than where the crosshair points) */
	boolean held_anchored;
	real held_distance;
	/* the right trigger turns it with the right stick */
	boolean held_turning;
	/* the mouse turns it (its right button, flying): the yaw and pitch it
	has turned to, before the snap's steps */
	boolean held_mouse_turning;
	real held_mouse_angles[NUMBER_OF_FORGE_AXES];
	/* the right button closed the menu: it turns nothing until it is let go */
	boolean held_mouse_blocked;
	/* a mod's objects go with it (forge_hold_object), or NULL */
	struct halo_forge_object_hold const *held_group;
	/* where it is kept while a popup is shown about it (its physics would
	otherwise let it fall) */
	real_point3d held_popup_position;

	boolean active;
	boolean menu_open;
	/* while the menu is open: a mod's popup is shown in its place
	(forge_popup_open), or NULL */
	struct halo_mod_menu const *popup;
	boolean keys_captured;
	short category_index;
	short entry_count;
	short selected_entry_index;
	/* the first row shown */
	short first_entry_index;
	/* how many objects each object category has in this map */
	short category_counts[FORGE_CATEGORY_COUNT];
	/* the menu's place in each category (see above) */
	char category_selected_names[FORGE_CATEGORY_COUNT][FORGE_TAG_NAME_SIZE];
	short category_first_entry_indices[FORGE_CATEGORY_COUNT];
	boolean menu_state_loaded;
	boolean menu_state_changed;
	long entries[FORGE_MAXIMUM_MENU_ENTRIES];
	long game_time;
	unsigned long milliseconds;

	struct forge_key toggle_flying_key;
	struct forge_key menu_key;
	struct forge_key menu_up_key;
	struct forge_key menu_down_key;
	struct forge_key menu_left_key;
	struct forge_key menu_right_key;
	struct forge_key menu_select_key;
	struct forge_key menu_close_key;
	struct forge_key mouse_right_key;
	struct forge_key grab_key;
	struct forge_key rotation_axis_key;
	struct forge_key rotation_snap_key;
	struct forge_key remove_object_key;
	struct forge_key tab_previous_key;
	struct forge_key tab_next_key;
	struct forge_key pad_menu_key;
	struct forge_key pad_remove_key;

	/* a mod's own thing held (halo_forge.h), and when it was last updated */
	struct halo_forge_hold const *mod_hold;
	unsigned long mod_hold_milliseconds;
	/* how far the mouse turned it (forge_camera_turn_held) since its last
	update: yaw, pitch */
	real mod_hold_mouse_turn[2];

	/* where the flying camera is and looks, after its last update */
	boolean camera_valid;
	real_point3d camera_position;
	real_vector3d camera_forward;
};

/* ---------- globals */

static struct forge_category const forge_categories[FORGE_CATEGORY_COUNT] =
{
	{ _object_type_vehicle, "Vehicles" },
	{ _object_type_weapon, "Weapons" },
	{ _object_type_equipment, "Equipment" },
	{ _object_type_biped, "Bipeds" },
	{ _object_type_scenery, "Scenery" }
};

/* a step of 0 turns freely while the key is held */
static struct forge_snap const forge_snaps[] =
{
	{ 0.f, "free" },
	{ DEGREES_TO_RADIANS(15), "15 degrees" },
	{ DEGREES_TO_RADIANS(45), "45 degrees" },
	{ DEGREES_TO_RADIANS(90), "90 degrees" },
	{ DEGREES_TO_RADIANS(180), "180 degrees" }
};

static struct forge_globals forge_globals = { NONE };

/* a line of text being typed (forge_text_entry_begin); the keys are SDL
scancodes (halo_linux_forge_read_text_keys) */
enum
{
	FORGE_TEXT_MAXIMUM_LENGTH = 127,
	FORGE_TEXT_KEY_COUNT = 57,
	FORGE_TEXT_KEY_A = 4,
	FORGE_TEXT_KEY_1 = 30,
	FORGE_TEXT_KEY_ESCAPE = 41,
	FORGE_TEXT_KEY_BACKSPACE = 42,
	FORGE_TEXT_KEY_SPACE = 44,
	FORGE_TEXT_KEY_LEFT_SHIFT = 225,
	FORGE_TEXT_KEY_RIGHT_SHIFT = 229,
	/* the keys read each frame: all up to the right shift */
	FORGE_TEXT_KEYS_READ = 230,
	/* the on-screen keyboard (forge_text_keyboard_layout): its rows and
	columns, and how many columns wide the first column is drawn */
	FORGE_TEXT_KEYBOARD_ROWS = 5,
	FORGE_TEXT_KEYBOARD_COLUMNS = 11,
	FORGE_TEXT_KEYBOARD_FIRST_COLUMN_WIDTH = 3,
	FORGE_TEXT_KEYBOARD_WIDTH = FORGE_TEXT_KEYBOARD_COLUMNS - 1 + FORGE_TEXT_KEYBOARD_FIRST_COLUMN_WIDTH
};

/* the on-screen keyboard's keys: first those that type, a character each
of forge_text_keyboard_characters, then the others */
enum
{
	FORGE_TEXT_KEYBOARD_CHARACTER_KEYS = 36,
	_forge_text_keyboard_done = FORGE_TEXT_KEYBOARD_CHARACTER_KEYS,
	_forge_text_keyboard_shift,
	_forge_text_keyboard_caps,
	_forge_text_keyboard_symbols,
	_forge_text_keyboard_backspace,
	_forge_text_keyboard_left,
	_forge_text_keyboard_right,
	_forge_text_keyboard_space,
	NUMBER_OF_FORGE_TEXT_KEYBOARD_KEYS
};

/* what the keys from 1 on type, plain and with shift (a US keyboard); a
space where a key types nothing (the space bar is read by itself, and a
percent sign would be taken for a format by the menus' strings) */
static char const forge_text_plain[] = "1234567890     -=[]  ;'`,./";
static char const forge_text_shifted[] = "!@#$ ^&*()     _+()  :\"~<>?";

/* the on-screen keyboard, for a line typed without a keyboard (a controller,
the touch controls), laid out as the one a profile's name is typed on
(virtual_keyboard.c): the D-pad chooses a key and A presses it. Shift makes
the next letter a capital, caps all of them, and symbols puts the second
characters on the keys (none the menus' strings would take for a code) */
static char const forge_text_keyboard_characters[FORGE_TEXT_KEYBOARD_CHARACTER_KEYS + 1] =
	"1234567890abcdefghijklmnopqrstuvwxyz";
static char const forge_text_keyboard_symbols[FORGE_TEXT_KEYBOARD_CHARACTER_KEYS + 1] =
	"!@#$~^&*()-_=+[]();:'\",.<>/?`~!@#$&*";
static char const *const forge_text_keyboard_labels[NUMBER_OF_FORGE_TEXT_KEYBOARD_KEYS - FORGE_TEXT_KEYBOARD_CHARACTER_KEYS] =
	{ "Done", "Shift", "Caps Lock", "Symbols", "Backspace", "<-", "->", "Space" };
static char const forge_text_keyboard_layout[FORGE_TEXT_KEYBOARD_ROWS][FORGE_TEXT_KEYBOARD_COLUMNS] =
{
	{ _forge_text_keyboard_done, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	{ _forge_text_keyboard_shift, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
	{ _forge_text_keyboard_caps, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 },
	{ _forge_text_keyboard_symbols, 30, 31, 32, 33, 34, 35, _forge_text_keyboard_backspace,
		_forge_text_keyboard_backspace, _forge_text_keyboard_backspace, _forge_text_keyboard_backspace },
	{ _forge_text_keyboard_space, _forge_text_keyboard_space, _forge_text_keyboard_space,
		_forge_text_keyboard_space, _forge_text_keyboard_space, _forge_text_keyboard_space,
		_forge_text_keyboard_space, _forge_text_keyboard_left, _forge_text_keyboard_left,
		_forge_text_keyboard_right, _forge_text_keyboard_right }
};

static struct
{
	boolean active;
	/* the keys held when it began do not type */
	boolean primed;
	char const *prompt;
	void (*done)(char const *text);
	unsigned long maximum_length;
	unsigned long length;
	char text[FORGE_TEXT_MAXIMUM_LENGTH + 1];
	/* where the next character goes, and whether the first replaces the
	line it began with (as the profile's keyboard's does) */
	unsigned long cursor;
	boolean replaces;
	struct forge_key keys[FORGE_TEXT_KEY_COUNT];
	/* the on-screen keyboard's chosen key, and its shift, caps and symbols */
	short keyboard_row;
	short keyboard_column;
	boolean keyboard_shift;
	boolean keyboard_caps;
	boolean keyboard_symbols;
	struct forge_key pad_select_key;
	struct forge_key pad_cancel_key;
	struct forge_key pad_start_key;
	struct forge_key pad_cursor_left_key;
	struct forge_key pad_cursor_right_key;
} forge_text_entry_globals;

/* ---------- private code */

/* the platform layer's (port/linux/src/port_config.c) */
int config_boolean(char const *name);

/* TRUE when the key goes down, and again every repeat interval while it is
held past the repeat delay, if it repeats */
static boolean forge_key_pressed(
	struct forge_key *key,
	int down,
	boolean repeats,
	unsigned long milliseconds)
{
	boolean pressed = FALSE;

	if (!down)
	{
		key->down = FALSE;
	}
	else if (!key->down)
	{
		key->down = TRUE;
		key->repeat_milliseconds = milliseconds + FORGE_KEY_REPEAT_DELAY_MILLISECONDS;
		pressed = TRUE;
	}
	else if (repeats && (long)(milliseconds - key->repeat_milliseconds) >= 0)
	{
		key->repeat_milliseconds = milliseconds + FORGE_KEY_REPEAT_INTERVAL_MILLISECONDS;
		pressed = TRUE;
	}

	return pressed;
}

/* whether this machine's game is the game's (a local game, the host of a
system link game), and whether it is a system link client, whose tools ask
the host (forge_layout_client_edit); neither in a saved film */
static boolean forge_authoritative(
	void)
{
	return halo_mods_authoritative();
}

static boolean forge_client(
	void)
{
	return game_connection() == _game_connection_network_client;
}

/* the object the player is: the vehicle they ride, else their unit */
static long forge_player_object_index(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	long object_index = NONE;

	if (player_index != NONE)
	{
		long unit_index = player_get(player_index)->unit_index;

		if (unit_index != NONE)
		{
			object_index = unit_get(unit_index)->object.parent_object_index;
			if (object_index == NONE)
				object_index = unit_index;
		}
	}

	return object_index;
}

/* forward and up of yaw (about the world's up), then pitch (the nose up),
then roll (about forward) */
static void forge_orientation_from_angles(
	real const angles[NUMBER_OF_FORGE_AXES],
	real_vector3d *forward,
	real_vector3d *up)
{
	real yaw = angles[_forge_axis_yaw];
	real pitch = angles[_forge_axis_pitch];
	real roll = angles[_forge_axis_roll];

	set_real_vector3d(forward, cosine(pitch) * cosine(yaw), cosine(pitch) * sine(yaw), sine(pitch));
	set_real_vector3d(up, -sine(pitch) * cosine(yaw), -sine(pitch) * sine(yaw), cosine(pitch));
	rotate_vector_about_axis(up, forward, sine(roll), cosine(roll));

	return;
}

/* the angles of an orientation, as forge_orientation_from_angles builds it */
static void forge_angles_from_orientation(
	real_vector3d const *forward,
	real_vector3d const *up,
	real angles[NUMBER_OF_FORGE_AXES])
{
	real_vector3d unrolled_forward;
	real_vector3d unrolled_up;
	real_vector3d side;

	angles[_forge_axis_yaw] = arctangent(forward->j, forward->i);
	angles[_forge_axis_pitch] = arctangent(
		forward->k,
		square_root(forward->i * forward->i + forward->j * forward->j));
	angles[_forge_axis_roll] = 0.f;
	forge_orientation_from_angles(angles, &unrolled_forward, &unrolled_up);
	/* rolling turns up towards forward x up */
	cross_product3d(&unrolled_forward, &unrolled_up, &side);
	angles[_forge_axis_roll] = arctangent(dot_product3d(up, &side), dot_product3d(up, &unrolled_up));

	return;
}

/* the level direction of a yaw */
static void forge_forward_from_yaw(
	real yaw,
	real_vector3d *forward)
{
	set_real_vector3d(forward, cosine(yaw), sine(yaw), 0.f);

	return;
}

/* where the crosshair points: the first surface along the camera's view,
past the player (and what they ride) and the ignored object, up to the aim
range; else the aim distance in front of the camera, with an upward
normal. FALSE when the camera is outside the map. */
static boolean forge_aim(
	short local_player_index,
	long ignore_object_index,
	real_point3d *point,
	real_vector3d *normal)
{
	struct observer_result const *camera = observer_get_camera(local_player_index);
	long player_object_index = forge_player_object_index(local_player_index);
	real_point3d origin = camera->position;
	real_vector3d ray;
	boolean hit = FALSE;
	/* what goes with the held object is no surface to put it on */
	struct halo_forge_object_hold const *group =
		ignore_object_index != NONE && ignore_object_index == forge_globals.held_object_index
			? forge_globals.held_group
			: NULL;
	short test_count = group && group->with ? FORGE_MAXIMUM_GROUP_AIM_TESTS : FORGE_MAXIMUM_AIM_TESTS;
	short test_index;

	if (camera->location.cluster_index == NONE)
		return FALSE;

	scale_vector3d(&camera->forward, FORGE_AIM_RANGE, &ray);
	for (test_index = 0; !hit && test_index < test_count; test_index++)
	{
		struct collision_result collision;

		if (!collision_test_vector(
			_collision_test_for_line_of_sight_flags,
			&origin,
			&ray,
			ignore_object_index,
			&collision))
		{
			break;
		}

		if (collision.type == _collision_result_object &&
			((player_object_index != NONE && collision.object_index == player_object_index) ||
				(group && group->with && group->with(collision.object_index))))
		{
			/* through the player's own vehicle */
			origin.x = collision.point.x + camera->forward.i * FORGE_AIM_STEP;
			origin.y = collision.point.y + camera->forward.j * FORGE_AIM_STEP;
			origin.z = collision.point.z + camera->forward.k * FORGE_AIM_STEP;
		}
		else
		{
			*point = collision.point;
			*normal = collision.plane.n;
			hit = TRUE;
		}
	}

	if (!hit)
	{
		real_vector3d forward;

		forge_forward_from_yaw(arctangent(camera->forward.j, camera->forward.i), &forward);
		point->x = camera->position.x + forward.i * FORGE_AIM_DISTANCE;
		point->y = camera->position.y + forward.j * FORGE_AIM_DISTANCE;
		point->z = camera->position.z;
		*normal = *global_up3d;
	}

	return TRUE;
}

/* the object that would be picked up for this object: what it is attached
to, if anything; NONE for the player, what they ride and what they carry,
and for objects that cannot be picked up */
static long forge_pickable_object_index(
	short local_player_index,
	long object_index)
{
	long result = object_get_ultimate_parent(object_index);
	long player_index = local_player_get_player_index(local_player_index);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;

	if (!object_try_and_get_and_verify_type(result, FORGE_PICKABLE_OBJECT_MASK) ||
		(unit_index != NONE && result == object_get_ultimate_parent(unit_index)))
	{
		result = NONE;
	}
	/* (nor any other player's unit, or what he rides: the host's tools moved
	and removed other players' bodies, which a client's are refused,
	forge_layout_handle_edit) */
	if (result != NONE && player_data)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		data_iterator_new(&iterator, player_data);
		while (result != NONE && (player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			if (player->unit_index != NONE && object_try_and_get(player->unit_index) &&
				result == object_get_ultimate_parent(player->unit_index))
			{
				result = NONE;
			}
		}
	}

	return result;
}

/* the object under the crosshair, or NONE: the first object surface along
the view before the structure, or an item whose bounding sphere the view
passes through nearer than that */
static long forge_pick(
	short local_player_index)
{
	struct observer_result const *camera = observer_get_camera(local_player_index);
	real_point3d origin = camera->position;
	real_vector3d ray;
	real nearest_distance = FORGE_AIM_RANGE;
	long picked_object_index = NONE;
	short test_index;

	scale_vector3d(&camera->forward, FORGE_AIM_RANGE, &ray);
	for (test_index = 0; test_index < FORGE_MAXIMUM_AIM_TESTS; test_index++)
	{
		struct collision_result collision;
		long object_index;

		if (!collision_test_vector(FORGE_PICK_COLLISION_FLAGS, &origin, &ray, NONE, &collision))
			break;

		nearest_distance = distance3d(&camera->position, &collision.point);
		if (collision.type != _collision_result_object)
			break;

		object_index = forge_pickable_object_index(local_player_index, collision.object_index);
		if (object_index != NONE)
		{
			picked_object_index = object_index;
			break;
		}

		/* through what cannot be picked up */
		origin.x = collision.point.x + camera->forward.i * FORGE_AIM_STEP;
		origin.y = collision.point.y + camera->forward.j * FORGE_AIM_STEP;
		origin.z = collision.point.z + camera->forward.k * FORGE_AIM_STEP;
		nearest_distance = FORGE_AIM_RANGE;
	}

	{
		struct object_iterator iterator;
		struct object_datum *object;

		object_iterator_new(&iterator, _object_mask_item, 0);
		while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real_vector3d to_center;
			real along;
			real radius = MAX(object->object.bounding_sphere_radius, FORGE_ITEM_PICK_MINIMUM_RADIUS);

			vector_from_points3d(&camera->position, &object->object.bounding_sphere_center, &to_center);
			along = dot_product3d(&to_center, &camera->forward);
			if (along > 0.f && along < nearest_distance &&
				magnitude_squared3d(&to_center) - along * along <= radius * radius &&
				forge_pickable_object_index(local_player_index, iterator.index) == iterator.index)
			{
				picked_object_index = iterator.index;
				nearest_distance = along;
			}
		}
	}

	return picked_object_index;
}

/* the object's origin on the aimed surface: out of it by the object's
radius, as the origin of most objects is at their middle (scenery stands
on its origin), then raised by the held height */
static void forge_held_position(
	long definition_index,
	real_point3d const *point,
	real_vector3d const *normal,
	real height,
	real_point3d *position)
{
	struct object_definition *definition = object_definition_get(definition_index);
	real lift = definition->object.type == _object_type_scenery
		? 0.f
		: definition->object.bounding_radius;

	position->x = point->x + normal->i * lift;
	position->y = point->y + normal->j * lift;
	position->z = point->z + normal->k * lift + height;

	return;
}

static void forge_return_to_player(
	short local_player_index)
{
	struct observer_result const *camera = observer_get_camera(local_player_index);
	real_point3d position = camera->position;
	real_vector3d forward = camera->forward;
	long object_index = forge_player_object_index(local_player_index);

	if (object_index == NONE)
	{
		terminal_printf(global_real_argb_green, "forge: camera returned to the player");
	}
	else if (!forge_authoritative())
	{
		/* (a client's own player is where the host takes it to be) */
		terminal_printf(global_real_argb_green, "forge: camera returned to the player");
	}
	else if (camera->location.cluster_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: the camera is outside the map, the player stays");
	}
	else
	{
		object_set_position(object_index, &position, NULL, NULL);
		player_control_set_facing(local_player_index, &forward);
		terminal_printf(global_real_argb_green, "forge: player moved to the camera");
	}

	director_forge_set_flying(local_player_index, FALSE);

	return;
}

static void forge_toggle_flying(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);

	if (director_forge_flying(local_player_index))
	{
		forge_return_to_player(local_player_index);
	}
	/* port: (game.forge_monitor: this machine's builder as the monitor, or
	the free camera) */
	else if (player_index != NONE && forge_monitor_definition() != NONE && config_boolean("game.forge_monitor"))
	{
		/* the builder as the monitor (forge_monitor.c), which a client asks
		the host for */
		boolean monitor = forge_player_is_monitor(player_index);

		if (forge_client())
		{
			forge_layout_client_edit(monitor ? _forge_edit_monitor_leave : _forge_edit_monitor_enter, NONE, NONE,
				&global_origin3d->x, &global_origin3d->x, &global_forward3d->i, &global_up3d->i);
		}
		else if (!forge_authoritative() || !forge_monitor_set(player_index, !monitor))
		{
			terminal_printf(global_real_argb_orange, monitor ?
				"forge: the monitor stays" : "forge: no monitor now (dead, or in a vehicle)");
		}
	}
	else if (*director_camera_scripted)
	{
		terminal_printf(global_real_argb_orange, "forge: the camera is scripted");
	}
	else
	{
		director_forge_set_flying(local_player_index, TRUE);
		terminal_printf(global_real_argb_green, "forge: flying camera");
	}

	return;
}

static struct halo_mod_menu const *forge_menu_page(
	void);

static int forge_compare_entries(
	void const *a,
	void const *b)
{
	return _stricmp(tag_get_name(*(long const *)a), tag_get_name(*(long const *)b));
}

/* the mod page of the menu's current tab, or NULL on an object category;
a popup is the page while it is shown */
static struct halo_mod_menu const *forge_menu_page(
	void)
{
	short page_index = (short)(forge_globals.category_index - NUMBEROF(forge_categories));

	if (forge_globals.popup)
		return forge_globals.popup;

	return page_index >= 0 ? halo_mods_menu_page(page_index) : NULL;
}

/* how many rows the menu shows at once */
static short forge_menu_visible_entries(
	void)
{
	return forge_globals.popup ? FORGE_POPUP_VISIBLE_ENTRIES : FORGE_MENU_VISIBLE_ENTRIES;
}

/* the keys on a mod's page: choose a row, change its value, act on it */
static void forge_menu_page_keys(
	struct halo_mod_menu const *page,
	boolean up,
	boolean down,
	boolean left,
	boolean right,
	boolean select)
{
	short count = page->row_count();

	forge_globals.entry_count = count;
	forge_globals.selected_entry_index = (short)PIN(forge_globals.selected_entry_index, 0, MAX(count - 1, 0));
	if (count > 0)
	{
		if (up)
			forge_globals.selected_entry_index = (short)((forge_globals.selected_entry_index + count - 1) % count);
		if (down)
			forge_globals.selected_entry_index = (short)((forge_globals.selected_entry_index + 1) % count);
		if (page->row_change)
		{
			if (left != right)
				page->row_change(forge_globals.selected_entry_index, right ? 1 : -1);
			if (select && page->row_change(forge_globals.selected_entry_index, 0))
				forge_globals.menu_open = FALSE;
		}
	}

	return;
}

/* scrolls the list as little as it takes to show the chosen row */
static void forge_menu_scroll_to_selection(
	void)
{
	short visible_entries = forge_menu_visible_entries();
	short last_first_entry_index = (short)MAX(forge_globals.entry_count - visible_entries, 0);

	if (forge_globals.selected_entry_index < forge_globals.first_entry_index)
		forge_globals.first_entry_index = forge_globals.selected_entry_index;
	if (forge_globals.selected_entry_index >= forge_globals.first_entry_index + visible_entries)
		forge_globals.first_entry_index = (short)(forge_globals.selected_entry_index - visible_entries + 1);
	forge_globals.first_entry_index = PIN(forge_globals.first_entry_index, 0, last_first_entry_index);

	return;
}

/* how many objects each object category has in the map */
static void forge_menu_count_categories(
	void)
{
	struct tag_iterator iterator;
	long tag_index;
	short category_index;

	for (category_index = 0; category_index < FORGE_CATEGORY_COUNT; category_index++)
		forge_globals.category_counts[category_index] = 0;
	tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		short object_type = object_definition_get(tag_index)->object.type;

		for (category_index = 0; category_index < FORGE_CATEGORY_COUNT; category_index++)
		{
			if (forge_categories[category_index].object_type == object_type && !forge_definition_is_flag(tag_index))
				forge_globals.category_counts[category_index]++;
		}
	}

	return;
}

/* the map's objects of the menu's category, by name (the categories' counts
are the map's: counted when the menu opens, forge_menu_open) */
static void forge_menu_build(
	void)
{
	struct halo_mod_menu const *page = forge_menu_page();
	short object_type;
	struct tag_iterator iterator;
	long tag_index;

	if (page)
	{
		forge_globals.entry_count = page->row_count();
		forge_globals.selected_entry_index = 0;
		forge_globals.first_entry_index = 0;
		if (page->opened)
			page->opened();
		return;
	}

	object_type = forge_categories[forge_globals.category_index].object_type;
	forge_globals.entry_count = 0;
	tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE &&
		forge_globals.entry_count < FORGE_MAXIMUM_MENU_ENTRIES)
	{
		if (object_definition_get(tag_index)->object.type == object_type && !forge_definition_is_flag(tag_index))
			forge_globals.entries[forge_globals.entry_count++] = tag_index;
	}
	qsort(
		forge_globals.entries,
		forge_globals.entry_count,
		sizeof(forge_globals.entries[0]),
		forge_compare_entries);

	/* back where the menu was in this category */
	forge_globals.selected_entry_index = 0;
	{
		char const *selected_name = forge_globals.category_selected_names[forge_globals.category_index];
		short entry_index;

		for (entry_index = 0; *selected_name && entry_index < forge_globals.entry_count; entry_index++)
		{
			if (_stricmp(tag_get_name(forge_globals.entries[entry_index]), selected_name) == 0)
			{
				forge_globals.selected_entry_index = entry_index;
				break;
			}
		}
	}
	forge_globals.first_entry_index = forge_globals.category_first_entry_indices[forge_globals.category_index];
	forge_menu_scroll_to_selection();

	return;
}

static void forge_menu_fonts_choose(
	void);

/* opens the menu on its category */
static void forge_menu_open(
	void)
{
	forge_globals.menu_open = TRUE;
	forge_globals.popup = NULL;
	forge_menu_fonts_choose();
	forge_menu_count_categories();
	forge_menu_build();

	return;
}

/* shows a mod's popup in the menu's place (the menu's tab stays as it is) */
static void forge_menu_open_popup(
	struct halo_mod_menu const *popup)
{
	forge_globals.menu_open = TRUE;
	forge_globals.popup = popup;
	if (forge_globals.held_object_index != NONE && object_try_and_get(forge_globals.held_object_index))
		forge_globals.held_popup_position = object_get(forge_globals.held_object_index)->object.position;
	forge_menu_fonts_choose();
	forge_menu_build();

	return;
}

/* keeps the menu's place in its category, for when it opens again */
static void forge_menu_remember(
	void)
{
	short category_index = forge_globals.category_index;
	char const *selected_name = forge_globals.entry_count > 0
		? tag_get_name(forge_globals.entries[forge_globals.selected_entry_index])
		: "";

	if (strcmp(forge_globals.category_selected_names[category_index], selected_name) != 0 ||
		forge_globals.category_first_entry_indices[category_index] != forge_globals.first_entry_index)
	{
		_snprintf(
			forge_globals.category_selected_names[category_index],
			FORGE_TAG_NAME_SIZE,
			"%s",
			selected_name);
		forge_globals.category_first_entry_indices[category_index] = forge_globals.first_entry_index;
		forge_globals.menu_state_changed = TRUE;
	}

	return;
}

/* reads the menu's place from the last session, once */
static void forge_menu_state_load(
	void)
{
	FILE *file;

	forge_globals.menu_state_loaded = TRUE;
	if ((file = fopen(FORGE_MENU_STATE_FILE, "r")) != NULL)
	{
		char line[FORGE_TAG_NAME_SIZE + 64];

		while (fgets(line, sizeof(line), file))
		{
			int category_index;
			int first_entry_index;
			int name_offset = 0;

			line[strcspn(line, "\r\n")] = 0;
			if (sscanf(line, "category %d", &category_index) == 1)
			{
				if (category_index >= 0 && category_index < FORGE_CATEGORY_COUNT)
					forge_globals.category_index = (short)category_index;
			}
			else if (sscanf(line, "place %d %d %n", &category_index, &first_entry_index, &name_offset) >= 2 &&
				name_offset > 0 &&
				category_index >= 0 && category_index < FORGE_CATEGORY_COUNT &&
				first_entry_index >= 0 && first_entry_index < FORGE_MAXIMUM_MENU_ENTRIES)
			{
				_snprintf(
					forge_globals.category_selected_names[category_index],
					FORGE_TAG_NAME_SIZE,
					"%s",
					line + name_offset);
				forge_globals.category_first_entry_indices[category_index] = (short)first_entry_index;
			}
		}
		fclose(file);
	}

	return;
}

/* forge_layout.c's: a file written beside itself, then in its place */
FILE *forge_file_begin(char const *path, char *temporary_path, unsigned long size);
int forge_file_commit(FILE *file, char const *temporary_path, char const *path);

/* writes the menu's place for the next session */
static void forge_menu_state_save(
	void)
{
	char temporary_path[256];
	FILE *file;

	forge_globals.menu_state_changed = FALSE;
	if ((file = forge_file_begin(FORGE_MENU_STATE_FILE, temporary_path, sizeof(temporary_path))) != NULL)
	{
		short category_index;

		fprintf(file, "# the forge menu's place: its category, and in each\n");
		fprintf(file, "# category the first row shown and the chosen tag\n");
		fprintf(file, "category %d\n", forge_globals.category_index);
		for (category_index = 0; category_index < FORGE_CATEGORY_COUNT; category_index++)
		{
			fprintf(
				file,
				"place %d %d %s\n",
				category_index,
				forge_globals.category_first_entry_indices[category_index],
				forge_globals.category_selected_names[category_index]);
		}
	}
	if (!file || !forge_file_commit(file, temporary_path, FORGE_MENU_STATE_FILE))
		terminal_printf(global_real_argb_orange, "forge: could not write %s", FORGE_MENU_STATE_FILE);

	return;
}

/* starts holding an object, turned as it is */
static void forge_hold_begin(
	long object_index,
	boolean spawned)
{
	struct object_datum *object = object_get(object_index);

	/* (its name is drawn in the menu's fonts, forge_render_held_name) */
	forge_menu_fonts_choose();
	forge_globals.held_object_index = object_index;
	forge_globals.held_spawned = spawned;
	forge_globals.held_group = NULL;
	forge_globals.held_height = 0.f;
	forge_globals.held_anchored = FALSE;
	forge_globals.held_turning = FALSE;
	forge_globals.held_original_position = object->object.position;
	forge_globals.held_original_forward = object->object.forward;
	forge_globals.held_original_up = object->object.up;
	/* (a mod may pick it up while a popup is shown: it stays there) */
	forge_globals.held_popup_position = object->object.position;
	forge_angles_from_orientation(&object->object.forward, &object->object.up, forge_globals.held_angles);

	return;
}

/* whether the layout keeps as many of the tools' objects as it can
(forge_layout.c) */
static boolean forge_budget_full(
	void)
{
	short used;
	short maximum;

	forge_layout_object_budget(&used, &maximum);

	return used >= maximum;
}

/* spawns the object where the crosshair points and holds it there */
static void forge_take(
	short local_player_index,
	long definition_index)
{
	struct observer_result const *camera = observer_get_camera(local_player_index);
	real_point3d point;
	real_vector3d normal;

	if (!forge_authoritative() && !forge_client())
	{
		terminal_printf(global_real_argb_orange, "forge: objects spawn only in a game being played");
	}
	else if (!forge_aim(local_player_index, NONE, &point, &normal))
	{
		terminal_printf(global_real_argb_orange, "forge: the camera is outside the map");
	}
	else if (forge_authoritative() && forge_budget_full())
	{
		terminal_printf(global_real_argb_orange, "forge: the map is full, remove an object first");
	}
	else
	{
		struct object_placement_data data;
		long object_index;

		object_placement_data_new(&data, definition_index, NONE);
		forge_held_position(definition_index, &point, &normal, 0.f, &data.position);
		forge_forward_from_yaw(arctangent(camera->forward.j, camera->forward.i), &data.forward);
		data.up = *global_up3d;

		if (forge_client())
		{
			/* the host makes it there; picked up from there to move it */
			forge_layout_client_edit(_forge_edit_spawn, NONE, definition_index, &data.position.x, &data.position.x,
				&data.forward.i, &data.up.i);
			terminal_printf(global_real_argb_green, "forge: asked the host for %s", tag_get_name(definition_index));
			return;
		}

		object_index = object_new(&data);
		if (object_index == NONE)
		{
			terminal_printf(global_real_argb_orange, "forge: %s could not be created", tag_get_name(definition_index));
		}
		else
		{
			/* kept by the layout (forge_layout.c) */
			forge_layout_note_spawned(object_index);
			forge_hold_begin(object_index, TRUE);
		}
	}

	return;
}

/* picks up the object under the crosshair */
static void forge_grab(
	short local_player_index)
{
	long object_index;

	if (!forge_authoritative() && !forge_client())
	{
		terminal_printf(global_real_argb_orange, "forge: objects move only in a game being played");
	}
	else if (observer_get_camera(local_player_index)->location.cluster_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: the camera is outside the map");
	}
	else if ((object_index = forge_pick(local_player_index)) == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: nothing to pick up there");
	}
	else
	{
		forge_hold_begin(object_index, FALSE);
	}

	return;
}

int forge_pick_up(
	long object_index)
{
	boolean picked_up = FALSE;

	if (forge_globals.active && (forge_authoritative() || forge_client()) && !forge_globals.mod_hold &&
		forge_globals.held_object_index == NONE && object_try_and_get(object_index))
	{
		forge_hold_begin(object_index, FALSE);
		picked_up = TRUE;
	}

	return picked_up;
}

/* removes the object under the crosshair, or lets a mod do it (forge_edit,
which can bring it back) */
static void forge_remove_at_crosshair(
	short local_player_index)
{
	long object_index;

	if (!forge_authoritative() && !forge_client())
	{
		terminal_printf(global_real_argb_orange, "forge: objects change only in a game being played");
	}
	else if (forge_authoritative() && halo_mods_remove())
	{
		/* the mod removed it */
	}
	else if (observer_get_camera(local_player_index)->location.cluster_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: the camera is outside the map");
	}
	else if ((object_index = forge_pick(local_player_index)) == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: nothing to remove there");
	}
	else if (forge_object_holds_flag(object_index))
	{
		terminal_printf(global_real_argb_orange, "forge: %s is the game type's: it can be moved, not removed",
			tag_get_name(object_get(object_index)->definition_index));
	}
	else if (forge_client())
	{
		struct object_datum *object = object_get(object_index);

		forge_layout_client_edit(_forge_edit_remove, object_index, object->definition_index, &object->object.position.x,
			&object->object.position.x, &object->object.forward.i, &object->object.up.i);
		terminal_printf(global_real_argb_green, "forge: asked the host to remove %s",
			tag_get_name(object->definition_index));
	}
	else
	{
		terminal_printf(global_real_argb_green, "forge: removed %s",
			tag_get_name(object_get(object_index)->definition_index));
		forge_layout_note_removed(object_index);
		object_delete(object_index);
	}

	return;
}

/* turns the held angle of the chosen axis: freely while turning, or to the
next step of the snap */
static void forge_turn(
	real direction,
	boolean pressed,
	real seconds)
{
	real *angle = &forge_globals.held_angles[forge_globals.held_axis];
	real step = forge_snaps[forge_globals.held_snap_index].step;

	if (step == 0.f)
	{
		*angle += direction * FORGE_TURN_SPEED * seconds;
	}
	else if (pressed)
	{
		*angle = (real)floor(*angle / step + 0.5f) * step + direction * step;
	}

	return;
}

/* puts the held object at the position, turned as held, and still */
static void forge_hold_place(
	real_point3d const *position)
{
	struct object_datum *object = object_get(forge_globals.held_object_index);
	real_vector3d forward;
	real_vector3d up;

	forge_orientation_from_angles(forge_globals.held_angles, &forward, &up);
	object_set_position(forge_globals.held_object_index, position, &forward, &up);
	object->object.translational_velocity = *global_zero_vector3d;
	object->object.angular_velocity = *global_zero_vector3d;
	/* a system link host: not at rest, so that its clients are told where it
	is every tick (network_objects.c) and see it move as it is held */
	if (game_connection() == _game_connection_network_server)
		SET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);
	/* a mod's objects that go with it follow */
	if (forge_globals.held_group && forge_globals.held_group->moved)
		forge_globals.held_group->moved(forge_globals.held_object_index);

	return;
}

/* the held object is no longer held, without being put down: a mod's
objects that went with it are told */
static void forge_hold_release(
	void)
{
	struct halo_forge_object_hold const *group = forge_globals.held_group;
	long object_index = forge_globals.held_object_index;

	forge_globals.held_object_index = NONE;
	forge_globals.held_group = NULL;
	if (group && group->ended)
		group->ended(object_index, FALSE);

	return;
}

/* the held object in front of the flying camera (held_anchored) */
static void forge_hold_place_anchored(
	void)
{
	real_point3d position;

	position.x = forge_globals.camera_position.x + forge_globals.camera_forward.i * forge_globals.held_distance;
	position.y = forge_globals.camera_position.y + forge_globals.camera_forward.j * forge_globals.held_distance;
	position.z = forge_globals.camera_position.z + forge_globals.camera_forward.k * forge_globals.held_distance +
		forge_globals.held_height;
	forge_hold_place(&position);

	return;
}

/* keeps the held object where the crosshair points (or, anchored, in front
of the flying camera), turned and raised as asked, and still */
static void forge_hold(
	struct halo_linux_forge_keys const *keys,
	boolean left,
	boolean right,
	real seconds)
{
	struct object_datum *object = object_get(forge_globals.held_object_index);
	boolean flying = forge_flying(FORGE_LOCAL_PLAYER_INDEX);
	boolean mouse_turn = keys->mouse_right && !forge_globals.held_mouse_blocked;
	real_point3d point;
	real_vector3d normal;

	if (keys->menu_left && !keys->menu_right)
		forge_turn(1.f, left, seconds);
	else if (keys->menu_right && !keys->menu_left)
		forge_turn(-1.f, right, seconds);
	forge_globals.held_height += (real)((keys->menu_up != 0) - (keys->menu_down != 0)) *
		FORGE_RAISE_SPEED * seconds;

	/* the controller's right stick: with the right trigger it turns the
	object, else (camera/director.c) it orbits the camera around it, which
	from then on holds the object in front of it */
	forge_globals.held_turning = flying && (keys->pad_turn || mouse_turn);
	if (flying && mouse_turn && !forge_globals.held_mouse_turning)
	{
		forge_globals.held_mouse_angles[_forge_axis_yaw] = forge_globals.held_angles[_forge_axis_yaw];
		forge_globals.held_mouse_angles[_forge_axis_pitch] = forge_globals.held_angles[_forge_axis_pitch];
	}
	forge_globals.held_mouse_turning = flying && mouse_turn;
	if (flying && keys->pad_turn)
	{
		forge_globals.held_angles[_forge_axis_yaw] -= keys->pad_look_x * FORGE_TURN_SPEED * seconds;
		forge_globals.held_angles[_forge_axis_pitch] += keys->pad_look_y * FORGE_TURN_SPEED * seconds;
	}
	if (!flying || !forge_globals.camera_valid)
	{
		forge_globals.held_anchored = FALSE;
	}
	else if (!forge_globals.held_anchored &&
		(keys->pad_turn || keys->pad_look_x != 0.f || keys->pad_look_y != 0.f))
	{
		real_vector3d to_object;

		vector_from_points3d(&forge_globals.camera_position, &object->object.position, &to_object);
		forge_globals.held_distance = PIN(
			dot_product3d(&to_object, &forge_globals.camera_forward),
			FORGE_ORBIT_MINIMUM_DISTANCE,
			FORGE_ORBIT_MAXIMUM_DISTANCE);
		forge_globals.held_height = 0.f;
		forge_globals.held_anchored = TRUE;
	}

	if (forge_globals.held_anchored)
	{
		/* and again after the camera moves (forge_flying_camera_moved) */
		forge_hold_place_anchored();
	}
	else if (forge_aim(FORGE_LOCAL_PLAYER_INDEX, forge_globals.held_object_index, &point, &normal))
	{
		real_point3d position;

		forge_held_position(object->definition_index, &point, &normal, forge_globals.held_height, &position);
		forge_hold_place(&position);
	}

	return;
}

/* lets go of the held object: placed where it is, put back, or removed */
static void forge_hold_end(
	boolean cancel,
	boolean remove_object)
{
	long object_index = forge_globals.held_object_index;
	struct object_datum *object = object_get(object_index);
	char const *name = tag_get_name(object->definition_index);

	/* a flag or ball is put back instead */
	if (remove_object && forge_object_holds_flag(object_index))
	{
		terminal_printf(global_real_argb_orange, "forge: %s is the game type's: it can be moved, not removed", name);
		cancel = TRUE;
		remove_object = FALSE;
	}

	/* a mod's objects that go with it: put down with it, or gone with it
	(told before it is removed) */
	if (forge_globals.held_group && forge_globals.held_group->ended)
		forge_globals.held_group->ended(object_index, !cancel && !remove_object);
	forge_globals.held_group = NULL;

	if (forge_client())
	{
		/* the host does it: this machine's copy goes back where it was (a
		removed one until the host's word comes) or stays where it was put */
		if (!cancel || remove_object)
		{
			forge_layout_client_edit(remove_object ? _forge_edit_remove : _forge_edit_move, object_index,
				object->definition_index, &forge_globals.held_original_position.x, &object->object.position.x,
				&object->object.forward.i, &object->object.up.i);
			terminal_printf(global_real_argb_green, remove_object
				? "forge: asked the host to remove %s"
				: "forge: asked the host to place %s", name);
		}
		if (cancel || remove_object)
		{
			object_set_position(
				object_index,
				&forge_globals.held_original_position,
				&forge_globals.held_original_forward,
				&forge_globals.held_original_up);
		}
		object->object.translational_velocity = *global_zero_vector3d;
		object->object.angular_velocity = *global_zero_vector3d;
	}
	else if (remove_object || (cancel && forge_globals.held_spawned))
	{
		forge_layout_note_removed(object_index);
		object_delete(object_index);
		if (remove_object)
			terminal_printf(global_real_argb_green, "forge: removed %s", name);
	}
	else
	{
		if (cancel)
		{
			object_set_position(
				object_index,
				&forge_globals.held_original_position,
				&forge_globals.held_original_forward,
				&forge_globals.held_original_up);
		}
		else
		{
			/* kept by the layout: a map's own object as moved, one the game
			made (a spawn point's weapon) as one of the tools' (forge_layout.c) */
			forge_layout_note_placed(object_index);
			terminal_printf(global_real_argb_green, "forge: placed %s", name);
		}
		object->object.translational_velocity = *global_zero_vector3d;
		object->object.angular_velocity = *global_zero_vector3d;
	}
	forge_globals.held_object_index = NONE;

	return;
}

/* the line it began with let go, if the first key replaces it */
static void forge_text_entry_replace(
	void)
{
	if (forge_text_entry_globals.replaces)
	{
		forge_text_entry_globals.replaces = FALSE;
		forge_text_entry_globals.length = 0;
		forge_text_entry_globals.cursor = 0;
		forge_text_entry_globals.text[0] = 0;
	}

	return;
}

/* a character at the cursor, while the line has room */
static void forge_text_entry_type(
	char character)
{
	unsigned long cursor;

	forge_text_entry_replace();
	cursor = forge_text_entry_globals.cursor;
	if (forge_text_entry_globals.length < forge_text_entry_globals.maximum_length)
	{
		/* (the characters after it move along, with the terminator) */
		memmove(&forge_text_entry_globals.text[cursor + 1], &forge_text_entry_globals.text[cursor],
			forge_text_entry_globals.length - cursor + 1);
		forge_text_entry_globals.text[cursor] = character;
		forge_text_entry_globals.length++;
		forge_text_entry_globals.cursor++;
	}

	return;
}

/* the character before the cursor deleted (all of the line it began with,
the first time) */
static void forge_text_entry_delete(
	void)
{
	unsigned long cursor = forge_text_entry_globals.cursor;

	if (forge_text_entry_globals.replaces)
	{
		forge_text_entry_replace();
	}
	else if (cursor > 0)
	{
		memmove(&forge_text_entry_globals.text[cursor - 1], &forge_text_entry_globals.text[cursor],
			forge_text_entry_globals.length - cursor + 1);
		forge_text_entry_globals.length--;
		forge_text_entry_globals.cursor--;
	}

	return;
}

/* the cursor a character left or right, within the line */
static void forge_text_entry_move_cursor(
	boolean right)
{
	forge_text_entry_globals.replaces = FALSE;
	if (right && forge_text_entry_globals.cursor < forge_text_entry_globals.length)
		forge_text_entry_globals.cursor++;
	else if (!right && forge_text_entry_globals.cursor > 0)
		forge_text_entry_globals.cursor--;

	return;
}

/* the line is taken */
static void forge_text_entry_take(
	void)
{
	forge_text_entry_globals.active = FALSE;
	forge_text_entry_globals.done(forge_text_entry_globals.text);

	return;
}

/* the on-screen keyboard's chosen key */
static short forge_text_keyboard_key(
	void)
{
	return forge_text_keyboard_layout[forge_text_entry_globals.keyboard_row][forge_text_entry_globals.keyboard_column];
}

/* what a key of the on-screen keyboard that types types, with its shift,
caps and symbols as they are */
static char forge_text_keyboard_character(
	short key)
{
	char character = (forge_text_entry_globals.keyboard_symbols
		? forge_text_keyboard_symbols
		: forge_text_keyboard_characters)[key];

	if ((forge_text_entry_globals.keyboard_shift || forge_text_entry_globals.keyboard_caps) &&
		character >= 'a' && character <= 'z')
	{
		character = (char)(character - 'a' + 'A');
	}

	return character;
}

/* the chosen key moved by a row down or up, or a column right or left, to
the next key that is not the same (the wide keys are a key), round the
edges */
static void forge_text_keyboard_move(
	short rows_down,
	short columns_right)
{
	short key = forge_text_keyboard_key();

	do
	{
		forge_text_entry_globals.keyboard_row = (short)((forge_text_entry_globals.keyboard_row + rows_down +
			FORGE_TEXT_KEYBOARD_ROWS) % FORGE_TEXT_KEYBOARD_ROWS);
		forge_text_entry_globals.keyboard_column = (short)((forge_text_entry_globals.keyboard_column +
			columns_right + FORGE_TEXT_KEYBOARD_COLUMNS) % FORGE_TEXT_KEYBOARD_COLUMNS);
	}
	while (forge_text_keyboard_key() == key);

	return;
}

/* the on-screen keyboard's chosen key is pressed: its character typed, or
its key's doing; shift lasts until another key is pressed */
static void forge_text_keyboard_press(
	void)
{
	short key = forge_text_keyboard_key();

	if (key < FORGE_TEXT_KEYBOARD_CHARACTER_KEYS)
	{
		forge_text_entry_type(forge_text_keyboard_character(key));
	}
	else
	{
		switch (key)
		{
		case _forge_text_keyboard_done:
			forge_text_entry_take();
			break;
		case _forge_text_keyboard_shift:
			forge_text_entry_globals.keyboard_shift = !forge_text_entry_globals.keyboard_shift;
			break;
		case _forge_text_keyboard_caps:
			forge_text_entry_globals.keyboard_caps = !forge_text_entry_globals.keyboard_caps;
			break;
		case _forge_text_keyboard_symbols:
			forge_text_entry_globals.keyboard_symbols = !forge_text_entry_globals.keyboard_symbols;
			break;
		case _forge_text_keyboard_backspace:
			forge_text_entry_delete();
			break;
		case _forge_text_keyboard_left:
			forge_text_entry_move_cursor(FALSE);
			break;
		case _forge_text_keyboard_right:
			forge_text_entry_move_cursor(TRUE);
			break;
		case _forge_text_keyboard_space:
			forge_text_entry_type(' ');
			break;
		}
	}
	if (key != _forge_text_keyboard_shift)
		forge_text_entry_globals.keyboard_shift = FALSE;

	return;
}

/* the line being typed: the keys that went down (and those held, again and
again) type, backspace deletes, enter takes the line, escape leaves it; and
the on-screen keyboard, as the profile's name's: the D-pad chooses a key and
A presses it, X deletes, the triggers move the cursor, start takes the line
and B leaves it */
static void forge_text_entry_update(
	struct halo_linux_forge_keys const *keys,
	boolean up,
	boolean down,
	boolean left,
	boolean right,
	boolean take,
	boolean pad_delete,
	unsigned long milliseconds)
{
	unsigned char held[FORGE_TEXT_KEYS_READ];
	boolean shift;
	boolean primed = forge_text_entry_globals.primed;
	boolean pad_select = forge_key_pressed(&forge_text_entry_globals.pad_select_key, keys->pad_select, TRUE,
		milliseconds);
	boolean pad_cancel = forge_key_pressed(&forge_text_entry_globals.pad_cancel_key, keys->pad_cancel, FALSE,
		milliseconds);
	boolean pad_start = forge_key_pressed(&forge_text_entry_globals.pad_start_key, keys->pad_start, FALSE,
		milliseconds);
	boolean pad_cursor_left = forge_key_pressed(&forge_text_entry_globals.pad_cursor_left_key,
		keys->pad_left_trigger, TRUE, milliseconds);
	boolean pad_cursor_right = forge_key_pressed(&forge_text_entry_globals.pad_cursor_right_key, keys->pad_turn,
		TRUE, milliseconds);
	short key;

	halo_linux_forge_read_text_keys(held, sizeof(held));
	shift = held[FORGE_TEXT_KEY_LEFT_SHIFT] || held[FORGE_TEXT_KEY_RIGHT_SHIFT];
	forge_text_entry_globals.primed = TRUE;
	for (key = FORGE_TEXT_KEY_A; key < FORGE_TEXT_KEY_COUNT; key++)
	{
		struct forge_key *state = &forge_text_entry_globals.keys[key];
		char character = 0;

		if (!forge_key_pressed(state, held[key], TRUE, milliseconds))
			continue;
		if (!primed)
		{
			/* held since before the line: it types when pressed again */
			state->repeat_milliseconds = milliseconds + 0x3fffffff;
			continue;
		}
		if (key == FORGE_TEXT_KEY_ESCAPE)
		{
			forge_text_entry_globals.active = FALSE;
			return;
		}
		if (key == FORGE_TEXT_KEY_BACKSPACE)
		{
			forge_text_entry_delete();
		}
		else if (key < FORGE_TEXT_KEY_1)
		{
			character = (char)((shift ? 'A' : 'a') + key - FORGE_TEXT_KEY_A);
		}
		else if (key == FORGE_TEXT_KEY_SPACE)
		{
			character = ' ';
		}
		else if (key - FORGE_TEXT_KEY_1 < (short)(sizeof(forge_text_plain) - 1) &&
			forge_text_plain[key - FORGE_TEXT_KEY_1] != ' ')
		{
			character = (shift ? forge_text_shifted : forge_text_plain)[key - FORGE_TEXT_KEY_1];
			if (character == ' ')
				character = 0;
		}
		if (character)
			forge_text_entry_type(character);
	}
	/* (the buttons held when it began, the A that chose the row, do nothing) */
	if (!primed)
		return;

	if (pad_cancel)
	{
		forge_text_entry_globals.active = FALSE;
		return;
	}
	if (up != down)
		forge_text_keyboard_move(down ? 1 : -1, 0);
	if (left != right)
		forge_text_keyboard_move(0, right ? 1 : -1);
	if (pad_cursor_left != pad_cursor_right)
		forge_text_entry_move_cursor(pad_cursor_right);
	if (pad_delete)
		forge_text_entry_delete();
	if (pad_select)
		forge_text_keyboard_press();
	/* (enter takes the line; the A that is also menu_select typed a key) */
	if (forge_text_entry_globals.active && (pad_start || (take && !keys->pad_select)))
		forge_text_entry_take();

	return;
}

static void forge_update_keys(
	struct halo_linux_forge_keys const *keys,
	unsigned long milliseconds,
	real seconds)
{
	boolean toggle_flying = forge_key_pressed(&forge_globals.toggle_flying_key, keys->toggle_flying, FALSE, milliseconds);
	boolean menu = forge_key_pressed(&forge_globals.menu_key, keys->menu, FALSE, milliseconds);
	boolean close = forge_key_pressed(&forge_globals.menu_close_key, keys->menu_close, FALSE, milliseconds);
	boolean mouse_right = forge_key_pressed(&forge_globals.mouse_right_key, keys->mouse_right, FALSE, milliseconds);
	boolean up = forge_key_pressed(&forge_globals.menu_up_key, keys->menu_up, TRUE, milliseconds);
	boolean down = forge_key_pressed(&forge_globals.menu_down_key, keys->menu_down, TRUE, milliseconds);
	boolean left = forge_key_pressed(&forge_globals.menu_left_key, keys->menu_left, TRUE, milliseconds);
	boolean right = forge_key_pressed(&forge_globals.menu_right_key, keys->menu_right, TRUE, milliseconds);
	boolean select = forge_key_pressed(&forge_globals.menu_select_key, keys->menu_select, FALSE, milliseconds);
	boolean grab = forge_key_pressed(&forge_globals.grab_key, keys->grab, FALSE, milliseconds);
	boolean rotation_axis = forge_key_pressed(&forge_globals.rotation_axis_key, keys->rotation_axis, FALSE, milliseconds);
	boolean rotation_snap = forge_key_pressed(&forge_globals.rotation_snap_key, keys->rotation_snap, FALSE, milliseconds);
	boolean remove_object = forge_key_pressed(&forge_globals.remove_object_key, keys->remove_object, FALSE, milliseconds);
	boolean tab_previous = forge_key_pressed(&forge_globals.tab_previous_key, keys->tab_previous, FALSE, milliseconds);
	boolean tab_next = forge_key_pressed(&forge_globals.tab_next_key, keys->tab_next, FALSE, milliseconds);
	boolean pad_menu = forge_key_pressed(&forge_globals.pad_menu_key, keys->pad_menu, FALSE, milliseconds);
	boolean pad_remove = forge_key_pressed(&forge_globals.pad_remove_key, keys->pad_remove, FALSE, milliseconds);
	int wheel = halo_linux_forge_read_wheel();
	boolean flying = forge_globals.active && forge_flying(FORGE_LOCAL_PLAYER_INDEX);
	/* as the monitor (forge_monitor.c), whose view is the flying camera */
	boolean monitor = forge_globals.active &&
		forge_player_is_monitor(local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX));

	/* in the menu the mouse's wheel chooses a row, as up and down do; with
	the left button held on a mod's row it changes the row's value, as left
	and right do (up is more) */
	if (forge_globals.menu_open && wheel != 0)
	{
		if (keys->mouse_left && forge_menu_page())
		{
			left = left || wheel < 0;
			right = right || wheel > 0;
		}
		else
		{
			up = up || wheel > 0;
			down = down || wheel < 0;
		}
	}
	if (!keys->mouse_right)
		forge_globals.held_mouse_blocked = FALSE;

	/* a held object that is gone (a new map, a revert, a deletion) is let go */
	if (forge_globals.held_object_index != NONE &&
		!object_try_and_get(forge_globals.held_object_index))
	{
		forge_hold_release();
	}
	/* (and one a popup is shown about stays where it was, still) */
	if (forge_globals.held_object_index != NONE && forge_globals.menu_open && forge_globals.popup)
		forge_hold_place(&forge_globals.held_popup_position);

	/* the entries are the tags of the map they were listed in: a new map
	(or a revert, as game time also goes back) closes the menu */
	if (!forge_globals.active || game_time_get() < forge_globals.game_time)
	{
		if (forge_globals.mod_hold)
		{
			forge_globals.mod_hold->update(NULL);
			forge_globals.mod_hold = NULL;
		}
		forge_globals.menu_open = FALSE;
		/* what was held stays where it is */
		forge_hold_release();
		forge_text_entry_globals.active = FALSE;
	}
	else if (forge_text_entry_globals.active)
	{
		/* the keyboard is the line's until it is taken or left */
		if (flying)
			forge_text_entry_update(keys, up, down, left, right, select, pad_menu, milliseconds);
		else
			forge_text_entry_globals.active = FALSE;
	}
	else
	{
		if (toggle_flying)
			forge_toggle_flying(FORGE_LOCAL_PLAYER_INDEX);

		if (forge_globals.mod_hold)
		{
			struct halo_forge_hold_input input;

			csmemset(&input, 0, sizeof(input));
			input.seconds = seconds;
			input.left = keys->menu_left;
			input.right = keys->menu_right;
			input.up = keys->menu_up;
			input.down = keys->menu_down;
			input.left_pressed = left;
			input.right_pressed = right;
			input.up_pressed = up;
			input.down_pressed = down;
			input.axis_pressed = rotation_axis;
			input.snap_pressed = rotation_snap || pad_menu;
			input.place = select;
			input.cancel = close || (mouse_right && !forge_globals.mod_hold->turns_with_look);
			input.delete_pressed = remove_object || pad_remove;
			if (forge_globals.mod_hold->turns_with_look)
			{
				/* the right stick and the mouse, as forge_hold turns an object */
				boolean mouse_turn = keys->mouse_right && !forge_globals.held_mouse_blocked;

				forge_globals.held_turning = flying && (keys->pad_turn || mouse_turn);
				forge_globals.held_mouse_turning = flying && mouse_turn;
				input.mouse_turning = forge_globals.held_mouse_turning;
				input.turn_yaw = forge_globals.mod_hold_mouse_turn[0];
				input.turn_pitch = forge_globals.mod_hold_mouse_turn[1];
				if (flying && keys->pad_turn)
				{
					input.turn_yaw -= keys->pad_look_x * FORGE_TURN_SPEED * seconds;
					input.turn_pitch += keys->pad_look_y * FORGE_TURN_SPEED * seconds;
				}
			}
			forge_globals.mod_hold_mouse_turn[0] = 0.f;
			forge_globals.mod_hold_mouse_turn[1] = 0.f;
			if (forge_globals.mod_hold->update(&input))
				forge_globals.mod_hold = NULL;
		}
		else if (forge_globals.held_object_index != NONE && !(forge_globals.menu_open && forge_globals.popup))
		{
			/* X, flying: the held object's popup, when a mod has one for it
			(the keys are then the popup's, and the object stays where it
			is until it closes); else the step, as V */
			struct halo_mod_menu const *popup = NULL;

			if (pad_menu && flying)
				halo_mods_properties_page(&popup);
			if (select || close || remove_object || pad_remove)
			{
				forge_hold_end(close, remove_object || pad_remove);
			}
			else if (popup)
			{
				forge_menu_open_popup(popup);
			}
			else
			{
				if (rotation_axis)
					forge_globals.held_axis = (forge_globals.held_axis + 1) % NUMBER_OF_FORGE_AXES;
				if (rotation_snap || pad_menu)
					forge_globals.held_snap_index = (forge_globals.held_snap_index + 1) % NUMBEROF(forge_snaps);
				forge_hold(keys, left, right, seconds);
			}
		}
		else if (forge_globals.menu_open)
		{
			if (menu || close || mouse_right || pad_menu)
			{
				forge_globals.menu_open = FALSE;
				forge_globals.held_mouse_blocked = mouse_right;
			}
			else
			{
				short category_count = (short)(NUMBEROF(forge_categories) + halo_mods_menu_page_count());
				struct halo_mod_menu const *page = forge_menu_page();

				/* (a popup has no tabs) */
				if (!forge_globals.popup &&
					(rotation_axis || rotation_snap || tab_previous || tab_next || (!page && (left || right))))
				{
					boolean next = rotation_snap || tab_next || (!rotation_axis && !tab_previous && right);

					forge_globals.category_index =
						(forge_globals.category_index + (next ? 1 : category_count - 1)) % category_count;
					forge_globals.menu_state_changed = TRUE;
					forge_menu_build();
					page = forge_menu_page();
				}
				if (page)
				{
					forge_menu_page_keys(page, up, down, left, right, select);
					forge_menu_scroll_to_selection();
				}
				else if (forge_globals.entry_count > 0)
				{
					if (up)
					{
						forge_globals.selected_entry_index =
							(forge_globals.selected_entry_index + forge_globals.entry_count - 1) % forge_globals.entry_count;
					}
					if (down)
					{
						forge_globals.selected_entry_index =
							(forge_globals.selected_entry_index + 1) % forge_globals.entry_count;
					}
					if (up || down)
					{
						forge_menu_scroll_to_selection();
						forge_menu_remember();
					}
					if (select)
					{
						forge_globals.menu_open = FALSE;
						forge_take(
							FORGE_LOCAL_PLAYER_INDEX,
							forge_globals.entries[forge_globals.selected_entry_index]);
					}
				}
			}
		}
		else if ((menu || pad_menu) && !flying)
		{
			/* the menu belongs to forge mode: on foot, 1 is the player's */
		}
		else if (menu || pad_menu)
		{
			/* X on something a mod's page is about (a spawn, a zone, an
			object) opens the menu on that page, which chooses it */
			struct halo_mod_menu const *popup = NULL;
			short page_index = pad_menu ? halo_mods_properties_page(&popup) : NONE;

			if (!forge_globals.menu_state_loaded)
				forge_menu_state_load();
			if (page_index != NONE)
				forge_globals.category_index = (short)(NUMBEROF(forge_categories) + page_index);
			/* (or the thing's popup, when its mod has one) */
			if (popup)
				forge_menu_open_popup(popup);
			else
				forge_menu_open();
		}
		else if (grab || (select && flying))
		{
			/* a mod may have something of its own to pick up there */
			if (!forge_authoritative() || !halo_mods_grab())
				forge_grab(FORGE_LOCAL_PLAYER_INDEX);
		}
		else if (pad_remove && flying)
		{
			forge_remove_at_crosshair(FORGE_LOCAL_PLAYER_INDEX);
		}
	}
	/* (it may have changed: the toggle, a death) */
	flying = forge_globals.active && forge_flying(FORGE_LOCAL_PLAYER_INDEX);
	monitor = forge_globals.active && forge_player_is_monitor(local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX));
	if (!flying)
	{
		/* the menu belongs to forge mode */
		forge_globals.camera_valid = FALSE;
		forge_globals.menu_open = FALSE;
	}
	/* the monitor rises and sinks with the flying camera's keys (in the
	menu they are its tabs'), and what it holds is held in front of its view */
	forge_monitor_set_keys(
		monitor && !forge_globals.menu_open && !forge_text_entry_globals.active
			? (keys->up != 0) - (keys->down != 0)
			: 0,
		monitor && !forge_text_entry_globals.active && keys->fast);
	if (monitor && !director_forge_flying(FORGE_LOCAL_PLAYER_INDEX))
	{
		struct observer_result const *camera = observer_get_camera(FORGE_LOCAL_PLAYER_INDEX);

		forge_flying_camera_moved(&camera->position.x, &camera->forward.i);
	}
	if (forge_globals.held_object_index == NONE)
		forge_globals.held_anchored = FALSE;
	/* (nor while a popup is shown about it: the look is then the camera's) */
	if ((forge_globals.held_object_index == NONE && !forge_globals.mod_hold) ||
		(forge_globals.menu_open && forge_globals.popup))
	{
		forge_globals.held_turning = FALSE;
		forge_globals.held_mouse_turning = FALSE;
	}

	/* a closed menu's place is written once, whichever way it closed */
	if (!forge_globals.menu_open && forge_globals.menu_state_changed)
		forge_menu_state_save();

	/* the keys stay away from the game until they are released, so the
	escape that closes the menu does not also pause the game */
	halo_linux_forge_capture_text(forge_text_entry_globals.active);
	forge_globals.keys_captured =
		forge_globals.menu_open ||
		forge_text_entry_globals.active ||
		forge_globals.held_object_index != NONE ||
		forge_globals.mod_hold != NULL ||
		(forge_globals.keys_captured &&
			(keys->menu_up || keys->menu_down || keys->menu_left || keys->menu_right ||
			keys->menu_select || keys->menu_close || keys->mouse_right || keys->rotation_axis || keys->rotation_snap ||
			keys->remove_object || keys->tab_previous || keys->tab_next || keys->pad_menu ||
			keys->pad_remove));
	halo_linux_forge_capture_menu_keys(forge_globals.keys_captured);
	halo_linux_forge_set_flying(flying);
	halo_linux_forge_set_free_camera(forge_globals.active && director_forge_flying(FORGE_LOCAL_PLAYER_INDEX));
	forge_globals.game_time = forge_globals.active ? game_time_get() : 0;

	return;
}

/* ---------- the menu's fonts (forge_ui) */

/* the menu's two fonts, as its drawing names them */
enum
{
	_forge_menu_font_large = 0,
	_forge_menu_font_small,
	NUMBER_OF_FORGE_MENU_FONTS
};

/* a font of the menu: the font tag that draws it, how much its text is
scaled, and its lines' height once scaled. With the high-res text
(display.high_res_text, port/linux/src/text_hires.c) the tags are those it
has fonts for, scaled to the height of the map's fonts the menu would
otherwise use (the terminal's, and the largest that fits the menu's lines),
so the menu is laid out the same either way. */
static struct
{
	long tag_index;
	real scale;
	short line_height;
} forge_menu_fonts[NUMBER_OF_FORGE_MENU_FONTS];

/* chooses the menu's fonts in this map: when the menu opens */
static void forge_menu_fonts_choose(
	void)
{
	static char const *const high_res_names[NUMBER_OF_FORGE_MENU_FONTS] =
	{
		"ui\\large_ui",
		"ui\\small_ui"
	};
	static int high_res = -1;
	long plain[NUMBER_OF_FORGE_MENU_FONTS];
	short font;

	if (high_res < 0)
		high_res = config_boolean("display.high_res_text");
	plain[_forge_menu_font_large] = halo_mod_font_for_lines(FORGE_MENU_FONT_LINES);
	plain[_forge_menu_font_small] = halo_mod_font(FALSE);
	for (font = 0; font < NUMBER_OF_FORGE_MENU_FONTS; font++)
	{
		long high_res_index = high_res ? tag_loaded(FONT_GROUP_TAG, high_res_names[font]) : NONE;

		forge_menu_fonts[font].tag_index = plain[font];
		forge_menu_fonts[font].scale = 1.f;
		forge_menu_fonts[font].line_height = halo_mod_line_height(plain[font]);
		if (high_res_index != NONE && high_res_index != plain[font] && halo_mod_line_height(high_res_index) > 0)
		{
			forge_menu_fonts[font].tag_index = high_res_index;
			forge_menu_fonts[font].scale =
				(real)forge_menu_fonts[font].line_height / (real)halo_mod_line_height(high_res_index);
		}
	}

	return;
}

static short forge_menu_line_height(
	long font)
{
	return forge_menu_fonts[font].line_height;
}

static short forge_menu_text_width(
	long font,
	char const *text)
{
	return (short)((real)halo_mod_text_width(forge_menu_fonts[font].tag_index, text) * forge_menu_fonts[font].scale +
		0.999f);
}

/* a name as the menu shows it: the last part of a tag's name, in capitals,
with spaces for underscores */
static void forge_menu_display_name(
	char const *name,
	char *display,
	short display_size)
{
	char const *separator = strrchr(name, '\\');
	short index;

	_snprintf(display, display_size, "%s", separator ? separator + 1 : name);
	display[display_size - 1] = 0;
	for (index = 0; display[index]; index++)
	{
		if (display[index] == '_')
			display[index] = ' ';
		else if (display[index] >= 'a' && display[index] <= 'z')
			display[index] = (char)(display[index] - 'a' + 'A');
	}

	return;
}

/* a tab's name: an object category, or a mod's page */
static char const *forge_tab_name(
	short tab_index)
{
	struct halo_mod_menu const *page = tab_index >= FORGE_CATEGORY_COUNT
		? halo_mods_menu_page((short)(tab_index - FORGE_CATEGORY_COUNT))
		: NULL;

	return page ? page->title : forge_categories[tab_index].name;
}

/* ---------- the menu's look (forge_ui) */

/* the controller buttons' faces the HUD draws, in its button style
(display.hud_button_style; port/linux/game/hud_hires_tags.c) */
enum
{
	_forge_menu_face_a = 0,
	_forge_menu_face_b = 1,
	_forge_menu_face_black = 4,
	_forge_menu_face_white = 5
};
struct bitmap_data *hud_hires_button_face(short index, rectangle2d *art);

/* one hint of the buttons along the bottom: the button's face, as the HUD
draws it (or the pause menu's icon, or, in a map without it, its letter on a
disc of its colour), then what it does */
struct forge_menu_hint
{
	char const *button;
	unsigned long argb;
	short face;
	char const *bitmap;
	char const *action;
};

/* what the menu's motion remembers between frames */
static struct
{
	unsigned long milliseconds;
	/* 0 to 1 while the menu fades in */
	real fade;
	/* the highlight's top edge, gliding to the chosen row */
	real entry_bar_y;
} forge_menu_look;

/* the fade of the frame being drawn (forge_menu_look.fade) */
static real forge_menu_fade = 1.f;

static unsigned long forge_menu_color(
	unsigned long argb)
{
	return ((unsigned long)((real)(argb >> 24) * forge_menu_fade) << 24) | (argb & 0x00ffffffUL);
}

static void forge_menu_box(
	short x0,
	short y0,
	short x1,
	short y1,
	unsigned long argb)
{
	if (x1 > x0 && y1 > y0)
		halo_mod_draw_box(x0, y0, x1, y1, forge_menu_color(argb));

	return;
}

/* a filled disc: a fan of triangles about its middle, one draw each */
static void forge_menu_disc(
	real center_x,
	real center_y,
	real radius,
	unsigned long argb)
{
	short side;

	for (side = 0; side < FORGE_MENU_DISC_SIDES; side++)
	{
		real from = 2.f * _pi * (real)side / FORGE_MENU_DISC_SIDES;
		real to = 2.f * _pi * (real)(side + 1) / FORGE_MENU_DISC_SIDES;

		/* (the screen's y runs down: a growing angle turns clockwise) */
		halo_mod_draw_triangle_real(center_x, center_y,
			center_x + cosine(from) * radius, center_y + sine(from) * radius,
			center_x + cosine(to) * radius, center_y + sine(to) * radius,
			forge_menu_color(argb));
	}

	return;
}

/* a small triangle pointing up or down, its middle at x, between y0 and y1,
in one draw */
static void forge_menu_arrow(
	real x,
	real y0,
	real y1,
	boolean down,
	unsigned long argb)
{
	real height = y1 - y0;

	if (down)
		halo_mod_draw_triangle_real(x - height, y0, x + height, y0, x, y1, forge_menu_color(argb));
	else
		halo_mod_draw_triangle_real(x, y0, x + height, y1, x - height, y1, forge_menu_color(argb));

	return;
}

/* a quarter turn of a ring about a corner's middle, from the angle on,
clockwise (the screen's y runs down); a filled quarter of a disc when inner
is 0. A triangle or two a step, each one draw */
static void forge_menu_corner(
	real center_x,
	real center_y,
	real outer,
	real inner,
	real angle,
	unsigned long argb)
{
	short step;

	for (step = 0; step < FORGE_MENU_CORNER_STEPS; step++)
	{
		real from = angle + 0.5f * _pi * (real)step / FORGE_MENU_CORNER_STEPS;
		real to = angle + 0.5f * _pi * (real)(step + 1) / FORGE_MENU_CORNER_STEPS;
		real outer_from_x = center_x + cosine(from) * outer;
		real outer_from_y = center_y + sine(from) * outer;
		real outer_to_x = center_x + cosine(to) * outer;
		real outer_to_y = center_y + sine(to) * outer;

		if (inner <= 0.f)
		{
			halo_mod_draw_triangle_real(center_x, center_y, outer_from_x, outer_from_y, outer_to_x, outer_to_y,
				forge_menu_color(argb));
		}
		else
		{
			real inner_from_x = center_x + cosine(from) * inner;
			real inner_from_y = center_y + sine(from) * inner;
			real inner_to_x = center_x + cosine(to) * inner;
			real inner_to_y = center_y + sine(to) * inner;

			halo_mod_draw_triangle_real(outer_from_x, outer_from_y, outer_to_x, outer_to_y, inner_to_x, inner_to_y,
				forge_menu_color(argb));
			halo_mod_draw_triangle_real(outer_from_x, outer_from_y, inner_to_x, inner_to_y, inner_from_x, inner_from_y,
				forge_menu_color(argb));
		}
	}

	return;
}

/* a panel as the pause menu's box: filled, round cornered, with a border
along its edge; band_y0 to band_y1 (below the top corners and above the
bottom ones) is filled with band_fill, and above it with header_fill */
static void forge_menu_banded_panel(
	short x0,
	short y0,
	short x1,
	short y1,
	short band_y0,
	short band_y1,
	unsigned long header_fill,
	unsigned long fill,
	unsigned long band_fill,
	unsigned long border)
{
	real left = (real)x0;
	real top = (real)y0;
	real right = (real)x1;
	real bottom = (real)y1;
	real radius = MIN(FORGE_POPUP_RADIUS, MIN(right - left, bottom - top) / 2.f);
	real inner = radius - FORGE_POPUP_BORDER;
	real band_top = PIN((real)band_y0, top + radius, bottom - radius);
	real band_bottom = PIN((real)band_y1, band_top, bottom - radius);
	real span_tops[3];
	real span_bottoms[3];
	short span_index;

	span_tops[0] = top;
	span_bottoms[0] = band_top;
	span_tops[1] = band_top;
	span_bottoms[1] = band_bottom;
	span_tops[2] = band_bottom;
	span_bottoms[2] = bottom;

	/* the fill: a cross, cut across at the band's edges, and a quarter disc
	in each corner (none overlap, so the fill is as see-through everywhere) */
	for (span_index = 0; span_index < 3; span_index++)
	{
		unsigned long argb = span_index == 0 ? header_fill : span_index == 1 ? band_fill : fill;
		real span_top = span_tops[span_index];
		real span_bottom = span_bottoms[span_index];
		real side_top = MAX(span_top, top + radius);
		real side_bottom = MIN(span_bottom, bottom - radius);

		if (span_bottom <= span_top)
			continue;
		halo_mod_draw_box_real(left + radius, span_top, right - radius, span_bottom, forge_menu_color(argb));
		if (side_bottom > side_top)
		{
			halo_mod_draw_box_real(left, side_top, left + radius, side_bottom, forge_menu_color(argb));
			halo_mod_draw_box_real(right - radius, side_top, right, side_bottom, forge_menu_color(argb));
		}
	}
	forge_menu_corner(left + radius, top + radius, radius, 0.f, _pi, header_fill);
	forge_menu_corner(right - radius, top + radius, radius, 0.f, 1.5f * _pi, header_fill);
	forge_menu_corner(right - radius, bottom - radius, radius, 0.f, 0.f, fill);
	forge_menu_corner(left + radius, bottom - radius, radius, 0.f, 0.5f * _pi, fill);

	/* the border: its four sides, and a quarter ring in each corner */
	halo_mod_draw_box_real(left + radius, top, right - radius, top + FORGE_POPUP_BORDER, forge_menu_color(border));
	halo_mod_draw_box_real(left + radius, bottom - FORGE_POPUP_BORDER, right - radius, bottom, forge_menu_color(border));
	halo_mod_draw_box_real(left, top + radius, left + FORGE_POPUP_BORDER, bottom - radius, forge_menu_color(border));
	halo_mod_draw_box_real(right - FORGE_POPUP_BORDER, top + radius, right, bottom - radius, forge_menu_color(border));
	forge_menu_corner(left + radius, top + radius, radius, inner, _pi, border);
	forge_menu_corner(right - radius, top + radius, radius, inner, 1.5f * _pi, border);
	forge_menu_corner(right - radius, bottom - radius, radius, inner, 0.f, border);
	forge_menu_corner(left + radius, bottom - radius, radius, inner, 0.5f * _pi, border);

	return;
}

/* a panel as the pause menu's box, filled with one colour */
static void forge_menu_rounded_panel(
	short x0,
	short y0,
	short x1,
	short y1,
	unsigned long fill,
	unsigned long border)
{
	forge_menu_banded_panel(x0, y0, x1, y1, y0, y0, fill, fill, fill, border);

	return;
}

/* text at the left, right or center of x0 to x1, its line's top at y */
static void forge_menu_text(
	long font,
	short x0,
	short y,
	short x1,
	int justification,
	unsigned long argb,
	char const *text)
{
	long tag_index = forge_menu_fonts[font].tag_index;
	real scale = forge_menu_fonts[font].scale;
	short height = halo_mod_line_height(tag_index);

	if (scale == 1.f)
	{
		halo_mod_draw_text(tag_index, x0, y, x1, (short)(y + height), justification, forge_menu_color(argb), text);
	}
	else
	{
		/* laid out in a box as wide as x0 to x1 is before the scale, then
		scaled about the edge (or the middle) the text is justified to */
		short width = (short)((real)(x1 - x0) / scale);
		real origin_x = justification == HALO_MOD_TEXT_RIGHT
			? (real)x1
			: justification == HALO_MOD_TEXT_CENTER ? (real)(x0 + x1) / 2.f : (real)x0;
		short left = justification == HALO_MOD_TEXT_RIGHT
			? (short)(x1 - width)
			: justification == HALO_MOD_TEXT_CENTER ? (short)(origin_x - (real)width / 2.f) : x0;

		rasterizer_text_set_scale(scale, origin_x, (real)y);
		halo_mod_draw_text(tag_index, left, y, (short)(left + width), (short)(y + height), justification,
			forge_menu_color(argb), text);
		rasterizer_text_set_scale(1.f, 0.f, 0.f);
	}

	return;
}

/* what is held, under the window's top margin: its name alone (the last part
of an object's tag name), in the menu's large font */
static void forge_render_held_name(
	rectangle2d const *window,
	char const *name)
{
	char const *separator = strrchr(name, '\\');

	/* (not the fade the menu was last drawn with) */
	forge_menu_fade = 1.f;
	forge_menu_text(_forge_menu_font_large, (short)(window->x0 + FORGE_MENU_MARGIN),
		(short)(window->y0 + FORGE_MENU_MARGIN), (short)(window->x1 - FORGE_MENU_MARGIN), HALO_MOD_TEXT_LEFT,
		FORGE_MENU_SELECTED_TEXT_COLOR, separator ? separator + 1 : name);

	return;
}

/* text shortened, with "...", to fit the width (the font's measure runs a
little short, so with room to spare) */
static void forge_menu_fit(
	long font,
	short width,
	char const *text,
	char *fitted,
	short fitted_size)
{
	int length = (int)strlen(text);

	_snprintf(fitted, fitted_size, "%s", text);
	if (length > 1 && forge_menu_text_width(font, fitted) > width - 2)
	{
		/* the most of it that fits: found by halves, as this is asked of every
		row each frame and measuring a line is what it costs */
		int fits = 1;
		int too_long = length;

		while (too_long - fits > 1)
		{
			int middle = (fits + too_long) / 2;

			_snprintf(fitted, fitted_size, "%.*s...", middle, text);
			if (forge_menu_text_width(font, fitted) > width - 2)
				too_long = middle;
			else
				fits = middle;
		}
		_snprintf(fitted, fitted_size, "%.*s...", fits, text);
	}

	return;
}

/* the highlight's top edge gliding from where it was to its row's: most of the
way in a tenth of a second, or straight there (snap) when the menu has just
opened */
static real forge_menu_glide(
	real from,
	real to,
	real seconds,
	boolean snap)
{
	return snap ? to : from + (to - from) * MIN(seconds * FORGE_MENU_GLIDE_SPEED, 1.f);
}

/* a shoulder button: its face (White for LB, Black for RB) as the HUD draws
it, with its name across it, or its name on a dark cap; its middle at y;
returns its width */
static short forge_menu_cap(
	long font,
	short x,
	short y,
	boolean right,
	short face,
	char const *name)
{
	short height = (short)(forge_menu_line_height(font) + 2);
	short width = (short)(forge_menu_text_width(font, name) * 106 / 100 + 10);
	rectangle2d art;
	struct bitmap_data *bitmap = hud_hires_button_face(face, &art);
	short x0;
	short top;

	if (bitmap)
	{
		short face_height = (short)(height + 6);
		short face_width = (short)(face_height * (art.x1 - art.x0) / MAX(art.y1 - art.y0, 1));
		rectangle2d bounds;

		width = MAX(width, face_width);
		x0 = right ? (short)(x - width) : x;
		top = (short)(y - face_height / 2);
		set_rectangle2d(&bounds, (short)(x0 + (width - face_width) / 2), top,
			(short)(x0 + (width - face_width) / 2 + face_width), (short)(top + face_height));
		/* (Black's face on the black header: a grey edge around it, the White
		face a little larger behind it) */
		if (face == _forge_menu_face_black)
		{
			rectangle2d edge_art;
			struct bitmap_data *edge = hud_hires_button_face(_forge_menu_face_white, &edge_art);
			rectangle2d edge_bounds = bounds;

			if (edge)
			{
				inset_rectangle2d(&edge_bounds, -1, -1);
				draw_bitmap_in_rect(edge, &edge_bounds, &edge_art, NULL,
					(pixel32)forge_menu_color(FORGE_MENU_CAP_EDGE_COLOR), NULL, TRUE);
			}
		}
		draw_bitmap_in_rect(bitmap, &bounds, &art, NULL, (pixel32)forge_menu_color(0xffffffffUL), NULL, TRUE);
		forge_menu_text(font, x0, (short)(y - height / 2 + 1), (short)(x0 + width), HALO_MOD_TEXT_CENTER,
			face == _forge_menu_face_white ? FORGE_MENU_CAP_DARK_TEXT_COLOR : FORGE_MENU_BUTTON_TEXT_COLOR, name);
		return width;
	}
	x0 = right ? (short)(x - width) : x;
	top = (short)(y - height / 2);
	forge_menu_box(x0, top, (short)(x0 + width), (short)(top + height), FORGE_MENU_CAP_COLOR);
	forge_menu_text(font, x0, (short)(top + 1), (short)(x0 + width), HALO_MOD_TEXT_CENTER,
		FORGE_MENU_CAP_TEXT_COLOR, name);

	return width;
}

/* the hints along the bottom, from x0, their line's top at y */
static void forge_menu_hints(
	long font,
	short x0,
	short x1,
	short y,
	struct forge_menu_hint const *hints,
	short hint_count)
{
	short line_height = forge_menu_line_height(font);
	real radius = (real)line_height * 0.5f;
	short x = x0;
	short hint_index;

	for (hint_index = 0; hint_index < hint_count && x < x1; hint_index++)
	{
		struct forge_menu_hint const *hint = &hints[hint_index];
		rectangle2d art;
		struct bitmap_data *bitmap = hud_hires_button_face(hint->face, &art);
		short disc_width = (short)(2.f * radius + 0.999f);

		/* (the HUD's face, else the pause menu's icon) */
		if (!bitmap)
		{
			long bitmap_index = tag_loaded(BITMAP_GROUP_TAG, hint->bitmap);

			bitmap = bitmap_index != NONE ? bitmap_group_try_and_get_bitmap(bitmap_index, 0) : NULL;
			set_rectangle2d(&art, 0, 0, FORGE_MENU_BUTTON_ART_WIDTH, FORGE_MENU_BUTTON_ART_HEIGHT);
		}
		if (bitmap)
		{
			short icon_height = (short)(line_height + 4);
			short icon_width = (short)(icon_height * (art.x1 - art.x0) / MAX(art.y1 - art.y0, 1));
			rectangle2d bounds;

			set_rectangle2d(&bounds, x, (short)(y + line_height / 2 - icon_height / 2), (short)(x + icon_width),
				(short)(y + line_height / 2 - icon_height / 2 + icon_height));
			draw_bitmap_in_rect(bitmap, &bounds, &art, NULL, (pixel32)forge_menu_color(0xffffffffUL), NULL, TRUE);
			disc_width = icon_width;
		}
		else
		{
			forge_menu_disc((real)x + radius, (real)y + (real)line_height / 2.f, radius, hint->argb);
			forge_menu_text(font, x, y, (short)(x + disc_width), HALO_MOD_TEXT_CENTER,
				FORGE_MENU_BUTTON_TEXT_COLOR, hint->button);
		}
		x += disc_width + 3;
		forge_menu_text(font, x, y, x1, HALO_MOD_TEXT_LEFT, FORGE_MENU_TEXT_COLOR, hint->action);
		x += (short)(forge_menu_text_width(font, hint->action) * 106 / 100 + FORGE_MENU_HINT_GAP);
	}

	return;
}

/* how many objects of each of the shown rows' tags are in the map */
static void forge_menu_count_shown(
	short *counts)
{
	struct object_iterator iterator;
	struct object_datum *object;
	short shown = (short)MIN(forge_globals.entry_count - forge_globals.first_entry_index, FORGE_MENU_VISIBLE_ENTRIES);
	short entry_index;

	for (entry_index = 0; entry_index < shown; entry_index++)
		counts[entry_index] = 0;
	object_iterator_new(&iterator, FORGE_PICKABLE_OBJECT_MASK, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		for (entry_index = 0; entry_index < shown; entry_index++)
		{
			if (forge_globals.entries[forge_globals.first_entry_index + entry_index] == object->definition_index)
			{
				counts[entry_index]++;
				break;
			}
		}
	}

	return;
}

/* the spawn menu (forge_ui), after the forge menu of Halo: Reach: a navy
panel at the top left of the screen with the tab's name between its shoulder
buttons, a column of how many of each object the map has beside the objects'
names, and the buttons along the bottom. The highlight glides to the chosen
row and the panel fades in. Up and down choose a row, left and right (or the
shoulder buttons) another tab: the map's objects by kind, then the mods'
pages, whose rows have a value at the right in place of the count. */
static void forge_render_menu(
	rectangle2d const *window)
{
	static struct forge_menu_hint const object_hints[] =
	{
		{ "A", FORGE_MENU_BUTTON_A_COLOR, _forge_menu_face_a, FORGE_MENU_BUTTON_A_BITMAP, "Place" },
		{ "B", FORGE_MENU_BUTTON_B_COLOR, _forge_menu_face_b, FORGE_MENU_BUTTON_B_BITMAP, "Done" },
	};
	static struct forge_menu_hint const page_hints[] =
	{
		{ "A", FORGE_MENU_BUTTON_A_COLOR, _forge_menu_face_a, FORGE_MENU_BUTTON_A_BITMAP, "Select" },
		{ "B", FORGE_MENU_BUTTON_B_COLOR, _forge_menu_face_b, FORGE_MENU_BUTTON_B_BITMAP, "Done" },
	};
	unsigned long milliseconds = system_milliseconds();
	real seconds = MIN((real)(milliseconds - forge_menu_look.milliseconds) / MILLISECONDS_PER_SECOND, 0.1f);
	boolean fresh = milliseconds - forge_menu_look.milliseconds > FORGE_MENU_FRESH_MILLISECONDS;
	long font = _forge_menu_font_large;
	long small_font = _forge_menu_font_small;
	short line_height = forge_menu_line_height(font);
	short small_height = forge_menu_line_height(small_font);
	short row_height = (short)(line_height + 2);
	short header_height = (short)(line_height + 8);
	short footer_height = (short)(line_height + 8);
	struct halo_mod_menu const *page = forge_menu_page();
	short tab_count = (short)(FORGE_CATEGORY_COUNT + halo_mods_menu_page_count());
	short window_width = (short)(window->x1 - window->x0);
	/* (the border and the corners' round inside the edge) */
	short inset = (short)(FORGE_POPUP_BORDER + 2.f);
	short x0 = (short)(window->x0 + FORGE_MENU_MARGIN);
	short x1 = (short)(x0 + MIN(window_width - 2 * FORGE_MENU_MARGIN, FORGE_MENU_MAX_WIDTH));
	short y0 = (short)(window->y0 + FORGE_MENU_MARGIN);
	short inner_x0 = (short)(x0 + inset);
	short inner_x1 = (short)(x1 - inset);
	short columns_y = (short)(y0 + inset + header_height);
	short rows_y = (short)(columns_y + row_height + 2);
	short rows_y1 = (short)(rows_y + FORGE_MENU_VISIBLE_ENTRIES * row_height);
	short y1 = (short)(rows_y1 + FORGE_MENU_PADDING + footer_height + inset);
	short footer_y = (short)(y1 - inset - footer_height);
	short text_x0 = (short)(inner_x0 + FORGE_MENU_PADDING + 2);
	/* the names leave room at the right for the scroll arrows */
	short text_x1 = (short)(inner_x1 - FORGE_MENU_PADDING - 10);
	short name_x0 = (short)(text_x0 + forge_menu_text_width(font, "000") * 106 / 100 + 8);
	boolean budget_full = FALSE;
	short counts[FORGE_MENU_VISIBLE_ENTRIES];
	char fitted[FORGE_TAG_NAME_SIZE + 64];
	char line[FORGE_TAG_NAME_SIZE + 64];
	short entry_index;

	/* the fade and the glide */
	forge_menu_look.milliseconds = milliseconds;
	forge_menu_look.fade = fresh ? 0.f : MIN(forge_menu_look.fade + seconds * FORGE_MENU_FADE_SPEED, 1.f);
	forge_menu_fade = forge_menu_look.fade;

	/* the panel, as the pause menu's box: the header, the columns and rows,
	the buttons, with a rule between the header and the rest, and above the
	buttons; the header and the buttons on black, the rows on navy */
	forge_menu_banded_panel(x0, y0, x1, y1, columns_y, footer_y, FORGE_MENU_HEADER_FILL_COLOR, FORGE_POPUP_FILL_COLOR,
		FORGE_MENU_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
	forge_menu_box(text_x0, (short)(columns_y - 1), (short)(inner_x1 - FORGE_MENU_PADDING), columns_y,
		FORGE_MENU_RULE_COLOR);
	forge_menu_box(text_x0, footer_y, (short)(inner_x1 - FORGE_MENU_PADDING), (short)(footer_y + 1),
		FORGE_MENU_RULE_COLOR);

	/* the header: the tab between the shoulder buttons, and which of the
	tabs it is */
	{
		short middle_y = (short)(y0 + inset + header_height / 2);
		short left = (short)(inner_x0 + 4 + forge_menu_cap(small_font, (short)(inner_x0 + 4), middle_y, FALSE,
			_forge_menu_face_white, "LB") + 4);
		short right = (short)(inner_x1 - 4 - forge_menu_cap(small_font, (short)(inner_x1 - 4), middle_y, TRUE,
			_forge_menu_face_black, "RB") - 4);
		short title_y = (short)(middle_y - line_height / 2);
		short title_width;
		short place_width;
		short title_x;
		char title[128];
		char place[32];

		forge_menu_display_name(forge_tab_name(forge_globals.category_index), title, sizeof(title));
		_snprintf(place, sizeof(place), " [%d of %d]", forge_globals.category_index + 1, tab_count);
		place_width = (short)(forge_menu_text_width(font, place) * 106 / 100);
		forge_menu_fit(font, (short)(right - left - place_width), title, fitted, sizeof(fitted));
		title_width = (short)(forge_menu_text_width(font, fitted) * 106 / 100);
		title_x = (short)MAX((left + right - title_width - place_width) / 2, left);
		forge_menu_text(font, title_x, title_y, right, HALO_MOD_TEXT_LEFT, FORGE_MENU_TITLE_TEXT_COLOR, fitted);
		forge_menu_text(font, (short)(title_x + title_width), title_y, (short)(right + 4), HALO_MOD_TEXT_LEFT,
			FORGE_MENU_PLACE_TEXT_COLOR, place);
	}

	/* the columns' names; beside them, on the objects' tabs, how many of the
	objects the layout keeps are made (the host's, in a system link game) */
	{
		short text_y = (short)(columns_y + (row_height - line_height) / 2 + 1);

		if (!page)
			forge_menu_text(font, text_x0, text_y, name_x0, HALO_MOD_TEXT_LEFT, FORGE_MENU_TEXT_COLOR, "#");
		forge_menu_text(font, page ? text_x0 : name_x0, text_y, text_x1, HALO_MOD_TEXT_LEFT, FORGE_MENU_TEXT_COLOR,
			page ? "OPTION" : "OBJECT");
		if (!page && forge_authoritative())
		{
			short used;
			short maximum;

			forge_layout_object_budget(&used, &maximum);
			budget_full = used >= maximum;
			_snprintf(line, sizeof(line), "%d / %d", used, maximum);
			forge_menu_text(small_font, text_x0, (short)(text_y + line_height - small_height),
				(short)(inner_x1 - FORGE_MENU_PADDING), HALO_MOD_TEXT_RIGHT,
				budget_full
					? FORGE_MENU_BUDGET_FULL_COLOR
					: used * 10 >= maximum * 9 ? FORGE_MENU_BUDGET_LOW_COLOR : FORGE_MENU_DIM_TEXT_COLOR,
				line);
		}
		forge_menu_box(text_x0, (short)(rows_y - 2), (short)(inner_x1 - FORGE_MENU_PADDING), (short)(rows_y - 1),
			FORGE_MENU_RULE_COLOR);
	}

	/* the rows: the highlight under the chosen one, then every row */
	forge_menu_look.entry_bar_y = forge_menu_glide(
		fresh ? (real)rows_y : forge_menu_look.entry_bar_y,
		(real)(rows_y + (forge_globals.selected_entry_index - forge_globals.first_entry_index) * row_height),
		seconds, fresh);
	if (forge_globals.entry_count > 0)
	{
		short bar_y = (short)PIN((short)forge_menu_look.entry_bar_y, rows_y,
			rows_y + (FORGE_MENU_VISIBLE_ENTRIES - 1) * row_height);

		/* (inside the border) */
		forge_menu_rounded_panel(inner_x0, bar_y, inner_x1, (short)(bar_y + row_height),
			FORGE_MENU_HIGHLIGHT_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
	}
	else
	{
		forge_menu_text(font, text_x0, (short)(rows_y + (row_height - line_height) / 2), text_x1,
			HALO_MOD_TEXT_LEFT, FORGE_MENU_DIM_TEXT_COLOR, page ? "NOTHING HERE" : "NONE IN THIS MAP");
	}
	if (!page)
		forge_menu_count_shown(counts);

	for (entry_index = forge_globals.first_entry_index;
		entry_index < forge_globals.entry_count &&
			entry_index < forge_globals.first_entry_index + FORGE_MENU_VISIBLE_ENTRIES;
		entry_index++)
	{
		boolean selected = entry_index == forge_globals.selected_entry_index;
		short text_y = (short)(rows_y + (entry_index - forge_globals.first_entry_index) * row_height +
			(row_height - line_height) / 2 + 1);
		unsigned long text_color = selected ? FORGE_MENU_SELECTED_TEXT_COLOR : FORGE_MENU_TEXT_COLOR;

		if (page)
		{
			char label[128];
			char value[128];
			char value_text[136];
			short label_width = (short)(text_x1 - text_x0);
			long row_font = font;
			short row_text_y = text_y;

			label[0] = value[0] = 0;
			page->row_text(entry_index, label, sizeof(label), value, sizeof(value));
			/* a mod's value at the right, in brackets on the chosen row: left and right change it */
			_snprintf(value_text, sizeof(value_text), value[0] ? (selected ? "< %s >" : "%s") : "", value);
			if (forge_menu_text_width(font, label) + forge_menu_text_width(font, value_text) + 24 > label_width)
			{
				/* not both in the large font: both in the small */
				row_font = small_font;
				row_text_y = (short)(text_y + (line_height - small_height) / 2);
			}
			if (value_text[0])
			{
				forge_menu_fit(row_font, (short)(label_width * 60 / 100), value_text, value, sizeof(value));
				forge_menu_text(row_font, text_x0, row_text_y, text_x1, HALO_MOD_TEXT_RIGHT,
					selected ? FORGE_MENU_SELECTED_TEXT_COLOR : FORGE_MENU_PLACE_TEXT_COLOR, value);
				label_width -= (short)(forge_menu_text_width(row_font, value) + 12);
			}
			forge_menu_fit(row_font, label_width, label, fitted, sizeof(fitted));
			forge_menu_text(row_font, text_x0, row_text_y, text_x1, HALO_MOD_TEXT_LEFT, text_color, fitted);
		}
		else
		{
			char name[FORGE_TAG_NAME_SIZE];

			/* dim when no more can be made */
			if (budget_full && !selected)
				text_color = FORGE_MENU_DIM_TEXT_COLOR;
			_snprintf(line, sizeof(line), "%d", counts[entry_index - forge_globals.first_entry_index]);
			forge_menu_text(font, text_x0, text_y, name_x0, HALO_MOD_TEXT_LEFT, text_color, line);
			forge_menu_display_name(tag_get_name(forge_globals.entries[entry_index]), name, sizeof(name));
			forge_menu_fit(font, (short)(text_x1 - name_x0), name, fitted, sizeof(fitted));
			forge_menu_text(font, name_x0, text_y, text_x1, HALO_MOD_TEXT_LEFT, text_color, fitted);
		}
	}

	/* arrows at the right when there are rows above or below those shown */
	{
		real arrow_x = (real)(inner_x1 - FORGE_MENU_PADDING - 4);
		real arrow_height = (real)row_height * 0.3f;

		if (forge_globals.first_entry_index > 0)
		{
			forge_menu_arrow(arrow_x, (real)rows_y + (real)row_height * 0.35f,
				(real)rows_y + (real)row_height * 0.35f + arrow_height, FALSE, FORGE_MENU_SCROLL_COLOR);
		}
		if (forge_globals.first_entry_index + FORGE_MENU_VISIBLE_ENTRIES < forge_globals.entry_count)
		{
			forge_menu_arrow(arrow_x, (real)rows_y1 - (real)row_height * 0.35f - arrow_height,
				(real)rows_y1 - (real)row_height * 0.35f, TRUE, FORGE_MENU_SCROLL_COLOR);
		}
	}

	/* the buttons */
	forge_menu_hints(font, (short)(inner_x0 + FORGE_MENU_PADDING), (short)(inner_x1 - FORGE_MENU_PADDING),
		(short)(footer_y + (footer_height - line_height) / 2), page ? page_hints : object_hints,
		page ? NUMBEROF(page_hints) : NUMBEROF(object_hints));

	forge_menu_fade = 1.f;

	return;
}

/* a mod's popup (forge_popup_open): a small panel beside the crosshair, as
the pause menu's box is (round cornered, with a blue border), with the
popup's title, its rows and the buttons, the title and the buttons on black
and the rows on navy, as the menu's */
static void forge_render_popup(
	rectangle2d const *window)
{
	static struct forge_menu_hint const popup_hints[] =
	{
		{ "A", FORGE_MENU_BUTTON_A_COLOR, _forge_menu_face_a, FORGE_MENU_BUTTON_A_BITMAP, "Select" },
		{ "B", FORGE_MENU_BUTTON_B_COLOR, _forge_menu_face_b, FORGE_MENU_BUTTON_B_BITMAP, "Close" },
	};
	struct halo_mod_menu const *page = forge_globals.popup;
	unsigned long milliseconds = system_milliseconds();
	real seconds = MIN((real)(milliseconds - forge_menu_look.milliseconds) / MILLISECONDS_PER_SECOND, 0.1f);
	boolean fresh = milliseconds - forge_menu_look.milliseconds > FORGE_MENU_FRESH_MILLISECONDS;
	long font = _forge_menu_font_large;
	long small_font = _forge_menu_font_small;
	short line_height = forge_menu_line_height(font);
	short small_height = forge_menu_line_height(small_font);
	short row_height = (short)(line_height + 2);
	short title_height = (short)(line_height + 8);
	short footer_height = (short)(line_height + 8);
	short row_count = (short)MIN(forge_globals.entry_count, FORGE_POPUP_VISIBLE_ENTRIES);
	short window_width = (short)(window->x1 - window->x0);
	short width = (short)MIN(window_width - 2 * FORGE_MENU_MARGIN, FORGE_POPUP_WIDTH);
	/* (the border and the corners' round inside the edge) */
	short inset = (short)(FORGE_POPUP_BORDER + 2.f);
	short height = (short)(inset + title_height + FORGE_MENU_PADDING + MAX(row_count, 1) * row_height +
		FORGE_MENU_PADDING + footer_height + inset);
	/* at the right of the crosshair, which stays in sight, and in the window */
	short x0 = (short)PIN((window->x0 + window->x1) / 2 + FORGE_POPUP_OFFSET, window->x0 + FORGE_MENU_MARGIN,
		MAX(window->x1 - FORGE_MENU_MARGIN - width, window->x0 + FORGE_MENU_MARGIN));
	short y0 = (short)PIN((window->y0 + window->y1 - height) / 2, window->y0 + FORGE_MENU_MARGIN,
		MAX(window->y1 - FORGE_MENU_MARGIN - height, window->y0 + FORGE_MENU_MARGIN));
	short x1 = (short)(x0 + width);
	short y1 = (short)(y0 + height);
	short title_y = (short)(y0 + inset);
	short rows_y = (short)(title_y + title_height + FORGE_MENU_PADDING);
	short footer_y = (short)(y1 - inset - footer_height);
	short text_x0 = (short)(x0 + inset + FORGE_MENU_PADDING + 2);
	short text_x1 = (short)(x1 - inset - FORGE_MENU_PADDING);
	char title[128];
	char fitted[192];
	short entry_index;
	short bar_y;

	forge_menu_look.milliseconds = milliseconds;
	forge_menu_look.fade = fresh ? 0.f : MIN(forge_menu_look.fade + seconds * FORGE_MENU_FADE_SPEED, 1.f);
	forge_menu_fade = forge_menu_look.fade;

	forge_menu_banded_panel(x0, y0, x1, y1, (short)(title_y + title_height), footer_y, FORGE_MENU_HEADER_FILL_COLOR,
		FORGE_POPUP_FILL_COLOR, FORGE_MENU_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
	forge_menu_display_name(page->title, title, sizeof(title));
	forge_menu_fit(font, (short)(width - 2 * (inset + FORGE_MENU_PADDING)), title, fitted, sizeof(fitted));
	forge_menu_text(font, x0, (short)(title_y + (title_height - line_height) / 2), x1, HALO_MOD_TEXT_CENTER,
		FORGE_MENU_TITLE_TEXT_COLOR, fitted);
	/* (a rule under the title, short of the border) */
	forge_menu_box(text_x0, (short)(title_y + title_height), text_x1, (short)(title_y + title_height + 1),
		FORGE_MENU_RULE_COLOR);

	/* the rows: the highlight under the chosen one, then every row */
	forge_menu_look.entry_bar_y = forge_menu_glide(
		fresh ? (real)rows_y : forge_menu_look.entry_bar_y,
		(real)(rows_y + (forge_globals.selected_entry_index - forge_globals.first_entry_index) * row_height),
		seconds, fresh);
	bar_y = (short)PIN((short)forge_menu_look.entry_bar_y, rows_y, rows_y + (MAX(row_count, 1) - 1) * row_height);
	if (forge_globals.entry_count > 0)
	{
		/* (inside the border) */
		short bar_x0 = (short)(x0 + inset);
		short bar_x1 = (short)(x1 - inset);

		forge_menu_rounded_panel(bar_x0, bar_y, bar_x1, (short)(bar_y + row_height),
			FORGE_MENU_HIGHLIGHT_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
	}
	for (entry_index = forge_globals.first_entry_index;
		entry_index < forge_globals.entry_count &&
			entry_index < forge_globals.first_entry_index + FORGE_POPUP_VISIBLE_ENTRIES;
		entry_index++)
	{
		boolean selected = entry_index == forge_globals.selected_entry_index;
		short text_y = (short)(rows_y + (entry_index - forge_globals.first_entry_index) * row_height +
			(row_height - line_height) / 2 + 1);
		short label_width = (short)(text_x1 - text_x0);
		long row_font = font;
		char label[128];
		char value[128];
		char value_text[136];

		label[0] = value[0] = 0;
		page->row_text(entry_index, label, sizeof(label), value, sizeof(value));
		/* a value at the right, in brackets on the chosen row: left and right change it */
		_snprintf(value_text, sizeof(value_text), value[0] ? (selected ? "< %s >" : "%s") : "", value);
		if (forge_menu_text_width(font, label) + forge_menu_text_width(font, value_text) + 24 > label_width)
		{
			/* not both in the large font: both in the small */
			row_font = small_font;
			text_y = (short)(text_y + (line_height - small_height) / 2);
		}
		if (value_text[0])
		{
			forge_menu_fit(row_font, (short)(label_width * 60 / 100), value_text, value, sizeof(value));
			forge_menu_text(row_font, text_x0, text_y, text_x1, HALO_MOD_TEXT_RIGHT,
				selected ? FORGE_MENU_SELECTED_TEXT_COLOR : FORGE_MENU_PLACE_TEXT_COLOR, value);
			label_width -= (short)(forge_menu_text_width(row_font, value) + 12);
		}
		forge_menu_fit(row_font, label_width, label, fitted, sizeof(fitted));
		forge_menu_text(row_font, text_x0, text_y, text_x1, HALO_MOD_TEXT_LEFT,
			selected ? FORGE_MENU_SELECTED_TEXT_COLOR : FORGE_MENU_TEXT_COLOR, fitted);
	}

	/* the buttons, under a rule */
	forge_menu_box(text_x0, footer_y, text_x1, (short)(footer_y + 1), FORGE_MENU_RULE_COLOR);
	forge_menu_hints(font, text_x0, text_x1, (short)(footer_y + (footer_height - line_height) / 2), popup_hints,
		NUMBEROF(popup_hints));

	forge_menu_fade = 1.f;

	return;
}

/* the on-screen keyboard's width of a cell, and its panel's left and right
(the box of the line being typed is as wide) */
static short forge_text_keyboard_cell_width(
	rectangle2d const *window,
	short *x0,
	short *x1)
{
	short line_height = forge_menu_line_height(_forge_menu_font_large);
	short window_width = (short)(window->x1 - window->x0);
	short cell_width = (short)PIN((window_width - 2 * FORGE_MENU_MARGIN - 2 * FORGE_MENU_PADDING) /
		FORGE_TEXT_KEYBOARD_WIDTH, 12, 2 * line_height + 8);
	short keyboard_width = (short)(cell_width * FORGE_TEXT_KEYBOARD_WIDTH);

	*x0 = (short)((window->x0 + window->x1 - keyboard_width) / 2 - FORGE_MENU_PADDING);
	*x1 = (short)(*x0 + keyboard_width + 2 * FORGE_MENU_PADDING);

	return cell_width;
}

/* the on-screen keyboard's height, its keys' rows and the buttons' line */
static short forge_text_keyboard_height(
	void)
{
	return (short)(FORGE_TEXT_KEYBOARD_ROWS * (forge_menu_line_height(_forge_menu_font_large) + 12) +
		forge_menu_line_height(_forge_menu_font_small) + 2 * FORGE_MENU_PADDING + 6);
}

/* the on-screen keyboard, drawn as the one a profile's name is typed on
(virtual_keyboard.c): its keys' names over a dark panel, the first column's
three times as wide, space under the letters, the chosen key outlined; shift,
caps lock and symbols lit while they are on; and under them the buttons */
static void forge_render_text_keyboard(
	rectangle2d const *window,
	short y0)
{
	long font = _forge_menu_font_large;
	long small_font = _forge_menu_font_small;
	short line_height = forge_menu_line_height(font);
	short small_height = forge_menu_line_height(small_font);
	short cell_height = (short)(line_height + 12);
	short x0;
	short x1;
	short cell_width = forge_text_keyboard_cell_width(window, &x0, &x1);
	short y1 = (short)(y0 + forge_text_keyboard_height());
	short cells_x0 = (short)(x0 + FORGE_MENU_PADDING);
	short chosen = forge_text_keyboard_key();
	short row;

	forge_menu_fade = 1.f;
	forge_menu_rounded_panel(x0, y0, x1, y1, FORGE_TEXT_KEYBOARD_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
	for (row = 0; row < FORGE_TEXT_KEYBOARD_ROWS; row++)
	{
		short top = (short)(y0 + FORGE_MENU_PADDING + row * cell_height);
		short column = 0;

		while (column < FORGE_TEXT_KEYBOARD_COLUMNS)
		{
			short key = forge_text_keyboard_layout[row][column];
			short first = column;
			short last = column;
			short left;
			short right;
			boolean lit = (key == _forge_text_keyboard_shift && forge_text_entry_globals.keyboard_shift) ||
				(key == _forge_text_keyboard_caps && forge_text_entry_globals.keyboard_caps) ||
				(key == _forge_text_keyboard_symbols && forge_text_entry_globals.keyboard_symbols);
			char label[16];

			/* (a wide key is the cells it is in along the row; space, which
			the first column also chooses, is drawn under the letters) */
			while (last + 1 < FORGE_TEXT_KEYBOARD_COLUMNS && forge_text_keyboard_layout[row][last + 1] == key)
				last++;
			if (key == _forge_text_keyboard_space && first == 0 && last > 0)
				first = 1;
			left = (short)(cells_x0 + cell_width * (first ? first - 1 + FORGE_TEXT_KEYBOARD_FIRST_COLUMN_WIDTH : 0));
			right = (short)(cells_x0 + cell_width * (last + FORGE_TEXT_KEYBOARD_FIRST_COLUMN_WIDTH));
			if (key == chosen)
			{
				forge_menu_rounded_panel((short)(left + 1), (short)(top + 1), (short)(right - 1),
					(short)(top + cell_height - 1), FORGE_TEXT_KEYBOARD_CHOSEN_FILL_COLOR, FORGE_POPUP_BORDER_COLOR);
			}
			if (key < FORGE_TEXT_KEYBOARD_CHARACTER_KEYS)
			{
				label[0] = forge_text_keyboard_character(key);
				label[1] = 0;
			}
			else
			{
				forge_menu_fit(font, (short)(right - left),
					forge_text_keyboard_labels[key - FORGE_TEXT_KEYBOARD_CHARACTER_KEYS], label, sizeof(label));
			}
			forge_menu_text(font, left, (short)(top + (cell_height - line_height) / 2), right, HALO_MOD_TEXT_CENTER,
				lit ? FORGE_MENU_PLACE_TEXT_COLOR : FORGE_MENU_SELECTED_TEXT_COLOR, label);
			column = (short)(last + 1);
		}
	}
	forge_menu_text(small_font, x0, (short)(y1 - FORGE_MENU_PADDING - 3 - small_height), x1, HALO_MOD_TEXT_CENTER,
		FORGE_MENU_DIM_TEXT_COLOR, "A type    X delete    LT RT move the cursor    START done    B leave");

	return;
}

/* the line being typed, near the window's bottom over the menu: as the
profile's name's, in a box of its own over the keyboard and as wide, the
line centred (its end, if it is wider than the box), the line it began with
highlighted until the first key replaces it, else the cursor blinking where
it is; what it is for and how long it may be above it */
static void forge_render_text_entry(
	rectangle2d const *window)
{
	long font = _forge_menu_font_large;
	long small_font = _forge_menu_font_small;
	short line_height = forge_menu_line_height(font);
	short small_height = forge_menu_line_height(small_font);
	short box_height = (short)(small_height + line_height + 2 * FORGE_MENU_PADDING + 12);
	short keyboard_y0 = (short)(window->y1 - FORGE_MENU_MARGIN / 2 - forge_text_keyboard_height());
	short y0 = (short)(keyboard_y0 - 8 - box_height);
	short text_y = (short)(y0 + FORGE_MENU_PADDING + small_height + 6);
	short x0;
	short x1;
	short available;
	short width;
	short text_x0;
	char const *shown = forge_text_entry_globals.text;
	char line[256];

	forge_text_keyboard_cell_width(window, &x0, &x1);
	available = (short)(x1 - x0 - 4 * FORGE_MENU_PADDING);
	forge_render_text_keyboard(window, keyboard_y0);
	forge_menu_fade = 1.f;
	forge_menu_rounded_panel(x0, y0, x1, (short)(y0 + box_height), FORGE_TEXT_KEYBOARD_FILL_COLOR,
		FORGE_POPUP_BORDER_COLOR);
	_snprintf(line, NUMBEROF(line), "%s  (%lu of %lu)", forge_text_entry_globals.prompt,
		forge_text_entry_globals.length, forge_text_entry_globals.maximum_length);
	line[NUMBEROF(line) - 1] = 0;
	forge_menu_text(small_font, x0, (short)(y0 + FORGE_MENU_PADDING + 2), x1, HALO_MOD_TEXT_CENTER,
		FORGE_MENU_PLACE_TEXT_COLOR, line);

	/* (a line wider than the box shows its end, up to the cursor) */
	while (*shown && shown < forge_text_entry_globals.text + forge_text_entry_globals.cursor &&
		forge_menu_text_width(font, shown) > available)
	{
		shown++;
	}
	width = forge_menu_text_width(font, shown);
	text_x0 = (short)((x0 + x1 - width) / 2);
	if (forge_text_entry_globals.replaces && *shown)
		forge_menu_box((short)(text_x0 - 4), text_y, (short)(text_x0 + width + 4), (short)(text_y + line_height),
			FORGE_TEXT_KEYBOARD_REPLACED_FILL_COLOR);
	forge_menu_text(font, text_x0, text_y, (short)(text_x0 + width + 2), HALO_MOD_TEXT_LEFT,
		FORGE_MENU_SELECTED_TEXT_COLOR, shown);
	if (!forge_text_entry_globals.replaces && (forge_globals.milliseconds / 400) % 2)
	{
		short caret_x;

		_snprintf(line, NUMBEROF(line), "%.*s",
			(int)(forge_text_entry_globals.text + forge_text_entry_globals.cursor - shown), shown);
		line[NUMBEROF(line) - 1] = 0;
		caret_x = (short)(text_x0 + (*line ? forge_menu_text_width(font, line) : 0));
		forge_menu_box(caret_x, text_y, (short)(caret_x + 2), (short)(text_y + line_height),
			FORGE_MENU_SELECTED_TEXT_COLOR);
	}

	return;
}

/* ---------- public code */

/* once a frame, with the game's other debug keys (main/main.c) */
void forge_update(
	void)
{
	struct halo_linux_forge_keys keys;
	unsigned long milliseconds = system_milliseconds();
	/* at most a tenth of a second, so a hitch does not fling a held object */
	real seconds = MIN((real)(milliseconds - forge_globals.milliseconds) / MILLISECONDS_PER_SECOND, 0.1f);

	/* whether the tools are on this frame, before the mods are asked: a
	mod's own key opens its popup (forge_popup_open), which needs them on
	(forge_render turns this off after each frame) */
	forge_globals.active = halo_linux_forge_read_keys(&keys) &&
		local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX) != NONE;
	if (!forge_globals.active)
		csmemset(&keys, 0, sizeof(keys));

	/* source mods first (game/halo_mods.c), whether or not the tools are on */
	halo_mods_update();

	forge_globals.milliseconds = milliseconds;
	forge_update_keys(&keys, milliseconds, seconds);

	return;
}

/* once a frame, over everything else (interface/interface.c) */
void forge_render(
	void)
{
	long font_tag_index = interface_get_tag_index(_interface_font_terminal);

	halo_mods_render();

	if (forge_globals.active && font_tag_index != NONE)
	{
		rectangle2d window = render.camera.window_bounds;

		offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
		if (forge_globals.mod_hold)
		{
			forge_render_held_name(&window, forge_globals.mod_hold->name);
		}
		else if (forge_globals.menu_open && forge_globals.popup)
		{
			/* (about what is held, too) */
			forge_render_popup(&window);
		}
		else if (forge_globals.held_object_index != NONE &&
			object_try_and_get(forge_globals.held_object_index))
		{
			forge_render_held_name(&window, forge_globals.held_group && forge_globals.held_group->name
				? forge_globals.held_group->name
				: tag_get_name(object_get(forge_globals.held_object_index)->definition_index));
		}
		else if (forge_globals.menu_open)
		{
			forge_render_menu(&window);
		}
		if (forge_text_entry_globals.active)
			forge_render_text_entry(&window);
	}

	/* drawn only after an update: nothing is left over when the game stops
	updating the tools (the main menu, loading) */
	forge_globals.active = FALSE;

	return;
}

/* ---------- whether the tools are on (halo_forge.h) */

int forge_mode_on(
	void)
{
	/* (not in a saved film, which replays what was recorded) */
	return game_engine_variant_is_forge() && (forge_authoritative() || forge_client());
}

/* ---------- text in the menu's font (halo_forge.h) */

/* the menu's fonts are a map's: chosen again in another */
static void forge_label_fonts(
	void)
{
	static char map_name[256];
	char const *name = tag_get_name(global_scenario_index);

	if (strcmp(name, map_name) != 0)
	{
		forge_menu_fonts_choose();
		_snprintf(map_name, sizeof(map_name), "%s", name);
	}

	return;
}

short forge_label_height(
	void)
{
	forge_label_fonts();

	return forge_menu_line_height(_forge_menu_font_large);
}

short forge_label_width(
	char const *text)
{
	forge_label_fonts();

	return forge_menu_text_width(_forge_menu_font_large, text);
}

void forge_draw_label(
	short x0,
	short y,
	short x1,
	int justification,
	unsigned long argb,
	char const *text)
{
	forge_label_fonts();
	/* (not the fade the menu was last drawn with) */
	forge_menu_fade = 1.f;
	forge_menu_text(_forge_menu_font_large, x0, y, x1, justification, argb, text);

	return;
}

/* ---------- a line of text (halo_forge.h) */

int forge_text_entry_begin(
	char const *prompt,
	char const *initial,
	unsigned long maximum_length,
	void (*done)(char const *text))
{
	if (forge_text_entry_globals.active)
		return FALSE;
	csmemset(&forge_text_entry_globals, 0, sizeof(forge_text_entry_globals));
	forge_text_entry_globals.active = TRUE;
	forge_text_entry_globals.prompt = prompt;
	forge_text_entry_globals.done = done;
	forge_text_entry_globals.maximum_length = MIN(maximum_length, FORGE_TEXT_MAXIMUM_LENGTH);
	_snprintf(forge_text_entry_globals.text, sizeof(forge_text_entry_globals.text), "%.*s",
		(int)forge_text_entry_globals.maximum_length, initial ? initial : "");
	forge_text_entry_globals.text[FORGE_TEXT_MAXIMUM_LENGTH] = 0;
	forge_text_entry_globals.length = (unsigned long)strlen(forge_text_entry_globals.text);
	forge_text_entry_globals.cursor = forge_text_entry_globals.length;
	forge_text_entry_globals.replaces = TRUE;

	return TRUE;
}

/* ---------- what the flying camera asks the tools (halo_forge.h) */

int forge_menu_is_open(
	void)
{
	return forge_globals.menu_open;
}

int forge_camera_turning(
	void)
{
	return (forge_globals.held_object_index != NONE || forge_globals.mod_hold) && forge_globals.held_turning;
}

/* left and right about the yaw, up and down about the pitch, as the right
stick; with a step chosen (V), by that step */
void forge_camera_turn_held(
	float yaw,
	float pitch)
{
	if (forge_camera_turning() && forge_globals.held_mouse_turning && forge_globals.mod_hold)
	{
		/* a mod's hold takes it at its next update */
		forge_globals.mod_hold_mouse_turn[0] += yaw;
		forge_globals.mod_hold_mouse_turn[1] += pitch;
	}
	else if (forge_camera_turning() && forge_globals.held_mouse_turning)
	{
		real step = forge_snaps[forge_globals.held_snap_index].step;
		real deltas[NUMBER_OF_FORGE_AXES] = { 0.f };
		short axis;

		deltas[_forge_axis_yaw] = yaw;
		deltas[_forge_axis_pitch] = pitch;
		for (axis = 0; axis < NUMBER_OF_FORGE_AXES; axis++)
		{
			real *angle = &forge_globals.held_mouse_angles[axis];

			if (deltas[axis] != 0.f)
			{
				*angle += deltas[axis];
				forge_globals.held_angles[axis] =
					step == 0.f ? *angle : (real)floor(*angle / step + 0.5f) * step;
			}
		}
	}

	return;
}

float forge_camera_orbit_distance(
	void)
{
	return forge_globals.held_object_index != NONE && forge_globals.held_anchored && !forge_globals.held_turning
		? forge_globals.held_distance
		: 0.f;
}

float forge_camera_zoom(
	float step)
{
	real distance;

	if (!forge_camera_turning() || !forge_globals.held_anchored)
		return 0.f;
	distance = PIN(forge_globals.held_distance - step, FORGE_ORBIT_MINIMUM_DISTANCE, FORGE_ORBIT_MAXIMUM_DISTANCE);
	step = forge_globals.held_distance - distance;
	forge_globals.held_distance = distance;

	return step;
}

void forge_flying_camera_moved(
	float const position[3],
	float const forward[3])
{
	forge_globals.camera_valid = TRUE;
	set_real_point3d(&forge_globals.camera_position, position[0], position[1], position[2]);
	set_real_vector3d(&forge_globals.camera_forward, forward[0], forward[1], forward[2]);
	/* (a popup's object stays where it is while the popup is shown) */
	if (forge_globals.held_object_index != NONE && forge_globals.held_anchored &&
		!(forge_globals.menu_open && forge_globals.popup) &&
		object_try_and_get(forge_globals.held_object_index))
	{
		forge_hold_place_anchored();
	}

	return;
}

/* ---------- what source mods may ask the tools (halo_forge.h) */

int forge_busy(
	void)
{
	return forge_globals.menu_open || forge_globals.held_object_index != NONE || forge_globals.mod_hold != NULL ||
		forge_text_entry_globals.active;
}

int forge_mod_hold_begin(
	struct halo_forge_hold const *hold)
{
	boolean begun = FALSE;

	if (!forge_globals.mod_hold && forge_globals.held_object_index == NONE)
	{
		forge_menu_fonts_choose();
		forge_globals.mod_hold = hold;
		begun = TRUE;
	}

	return begun;
}

int forge_popup_open(
	struct halo_mod_menu const *popup)
{
	boolean opened = FALSE;

	/* (over a held object too: the popup is then about it) */
	if (forge_globals.active && popup && !forge_globals.menu_open && !forge_globals.mod_hold &&
		!forge_text_entry_globals.active && forge_flying(FORGE_LOCAL_PLAYER_INDEX))
	{
		forge_menu_open_popup(popup);
		opened = TRUE;
	}

	return opened;
}

long forge_held_object(
	void)
{
	return forge_globals.held_object_index != NONE && object_try_and_get(forge_globals.held_object_index)
		? forge_globals.held_object_index
		: NONE;
}

void forge_angles_of_orientation(
	float const forward[3],
	float const up[3],
	float angles[3])
{
	forge_angles_from_orientation((real_vector3d const *)forward, (real_vector3d const *)up, angles);

	return;
}

void forge_orientation_of_angles(
	float const angles[3],
	float forward[3],
	float up[3])
{
	forge_orientation_from_angles(angles, (real_vector3d *)forward, (real_vector3d *)up);

	return;
}

void forge_held_angles(
	float angles[3])
{
	short axis;

	for (axis = 0; axis < NUMBER_OF_FORGE_AXES; axis++)
		angles[axis] = forge_globals.held_angles[axis];

	return;
}

void forge_held_adjust(
	int axis,
	float angle,
	float height)
{
	if (forge_held_object() != NONE && axis >= 0 && axis < NUMBER_OF_FORGE_AXES)
	{
		forge_globals.held_angles[axis] += angle;
		forge_globals.held_height += height;
		/* seen at once: a popup's object stays where it is while it is shown */
		if (forge_globals.menu_open && forge_globals.popup)
		{
			forge_globals.held_popup_position.z += height;
			forge_hold_place(&forge_globals.held_popup_position);
		}
		else
		{
			real_point3d position = object_get(forge_globals.held_object_index)->object.position;

			position.z += height;
			forge_hold_place(&position);
		}
	}

	return;
}

char const *forge_held_step(
	int direction)
{
	if (direction != 0)
	{
		forge_globals.held_snap_index = (short)((forge_globals.held_snap_index +
			(direction > 0 ? 1 : NUMBEROF(forge_snaps) - 1)) % NUMBEROF(forge_snaps));
	}

	return forge_snaps[forge_globals.held_snap_index].name;
}

void forge_held_put_down(
	void)
{
	if (forge_held_object() != NONE)
		forge_hold_end(FALSE, FALSE);

	return;
}

int forge_popup_is_open(
	struct halo_mod_menu const *popup)
{
	return forge_globals.menu_open && popup && forge_globals.popup == popup;
}

void forge_popup_close(
	struct halo_mod_menu const *popup)
{
	if (forge_popup_is_open(popup))
		forge_globals.menu_open = FALSE;

	return;
}

int forge_hold_object(
	long object_index,
	struct halo_forge_object_hold const *hold)
{
	boolean begun = FALSE;

	if (forge_globals.active && forge_authoritative() && !forge_globals.mod_hold &&
		forge_globals.held_object_index == NONE && object_try_and_get(object_index))
	{
		forge_hold_begin(object_index, TRUE);
		forge_globals.held_group = hold;
		begun = TRUE;
	}

	return begun;
}

long forge_object_at_crosshair(
	void)
{
	long object_index = NONE;

	if (local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX) != NONE &&
		observer_get_camera(FORGE_LOCAL_PLAYER_INDEX)->location.cluster_index != NONE)
	{
		object_index = forge_pick(FORGE_LOCAL_PLAYER_INDEX);
	}

	return object_index;
}

int forge_definition_is_flag(
	long definition_index)
{
	/* (as weapon_is_flag, items/weapons.c) */
	return object_definition_get(definition_index)->object.type == _object_type_weapon &&
		TEST_FLAG(weapon_definition_get(definition_index)->weapon.flags, 3);
}

int forge_object_holds_flag(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	boolean holds_flag = FALSE;

	if (object)
	{
		long child_index;

		holds_flag = object->object.type == _object_type_weapon && weapon_is_flag(object_index);
		for (child_index = object->object.first_child_object_index;
			!holds_flag && child_index != NONE;
			child_index = object_get(child_index)->object.next_object_index)
		{
			holds_flag = forge_object_holds_flag(child_index);
		}
	}

	return holds_flag;
}

int forge_placement_at_crosshair(
	long definition_index,
	float position[3])
{
	real_point3d point;
	real_vector3d normal;
	boolean placed = FALSE;

	if (local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX) != NONE &&
		forge_aim(FORGE_LOCAL_PLAYER_INDEX, NONE, &point, &normal))
	{
		real_point3d result;

		forge_held_position(definition_index, &point, &normal, 0.f, &result);
		position[0] = result.x;
		position[1] = result.y;
		position[2] = result.z;
		placed = TRUE;
	}

	return placed;
}

int forge_point_at_crosshair(
	float position[3],
	float normal[3])
{
	real_point3d point;
	real_vector3d surface_normal;
	boolean aimed = FALSE;

	if (local_player_get_player_index(FORGE_LOCAL_PLAYER_INDEX) != NONE &&
		forge_aim(FORGE_LOCAL_PLAYER_INDEX, NONE, &point, &surface_normal))
	{
		position[0] = point.x;
		position[1] = point.y;
		position[2] = point.z;
		normal[0] = surface_normal.i;
		normal[1] = surface_normal.j;
		normal[2] = surface_normal.k;
		aimed = TRUE;
	}

	return aimed;
}
