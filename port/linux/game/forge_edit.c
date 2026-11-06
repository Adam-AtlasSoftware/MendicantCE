/*
FORGE_EDIT.C

One of Forge's parts (halo_mod.h, halo_mods.c): an object's popup, with the dev tools'
crosshair (port/linux/game/forge.c): move it, copy it, change its size,
turn and height, remove it.

While the camera flies, X (on a keyboard or a controller) with the
crosshair on an object opens the popup beside the crosshair. It is the
object's own: the object is the one the crosshair was on, which is marked
with a box while the popup is shown, wherever the camera then looks. X
while an object is held (picked up, or just spawned) opens the popup about
that object: it stays where it is while the popup is shown, and is held
again when the popup closes. Up and
down choose a row, left and right change its value, enter or A act, escape,
B or X again close it. With the mouse, the wheel chooses a row, the wheel
with the left button held changes its value, the left button acts and the
right button closes it. The rows:

	Move              pick it up, as the tools' pick up key does: put down
	                  where it is let go, put back when that is cancelled
	Duplicate         a copy of it (its tag, turn and size), held until it
	                  is put down; cancelled, the copy is removed
	Size              its scale, in tenths of its own size
	Turn              about the upright, by 15 degrees
	Tilt              its nose up and down, by 15 degrees
	Roll              about its length, by 15 degrees
	Height            up and down, by a twentieth of a world unit
	Prefab            select it for a prefab, or leave it out again
	                  (forge_prefabs.c)
	Team              the team it is kept for: none, red, blue, or 2 to 15
	Placed at start   made when the map variant loads, or only while
	                  Forge is on (to edit)
	Game types        every game type, or one of them: with Forge off it
	                  is made only in that one
	Respawn           the seconds it respawns after, by 5 (0 the game
	                  type's; kept with the variant, not used yet)
	Group             the editor's group it is in (1 to 99, 0 none)
	Comment           a note about it, typed on the keyboard
	Remove            remove it
	Undo remove       bring back the last removed object (the last 16, one
	                  at a time)

About a held object, Move is "Put down" (where it is), Duplicate leaves a
copy where it is, Turn, Tilt, Roll and Height change how it is held, and in place of
Undo remove a Step row has the step it turns in while held (as V, which X
was on a controller).

On a keyboard, Delete removes the object under the crosshair with the popup
closed, and so does Y on a controller while the camera flies (the tools'
remove hook). The object under the crosshair is the one the dev tools would pick
up: flying, where the crosshair points, else where the player looks. While
the camera flies it is marked on the screen with its name.

Objects are made and removed only with the game's own object_new and
object_delete. The player, what they ride and carry, and anything that
holds a player (a vehicle with another player in it) are never marked, so
they have no popup; object indices are checked with object_try_and_get
before every use. As with the dev tools, all of this works only in Forge
games, and not during cutscenes or while the tools' menu is open or they
hold an object (their own delete then removes the held object). In a system
link game a client's popup moves the object (the host is asked, as for the
tools' pick up key); the other rows are the host's.

A changed turn, tilt, roll, height or size is kept by the tools' layout as a moved object is
(port/linux/game/forge_layout.c), for the objects the tools made and the
map's own alike. Team, Placed at start, Game types, Respawn, Group and Comment are the
object's properties in the map variant (forge_layout_set_properties): the
host's in a system link game, kept for the objects the tools made and the
map's own alike; a copy and an object brought back have them too.

An object brought back is a new object of the same tag, place, turn and
scale: what it carried (a biped's weapon) and its state (damage, an actor's
orders) are gone. Removals are forgotten when the map changes, and when the
game goes back to a checkpoint, which brings the removed objects back
itself.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "camera/director.h"
#include "camera/director_forge.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/object_types.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include <math.h>
#include <stdio.h>

/* ---------- constants */

enum
{
	FORGE_EDIT_UNDO_COUNT = 16,
	FORGE_EDIT_LOCAL_PLAYER_INDEX = 0,
	FORGE_EDIT_LABEL_OFFSET = 14,
	FORGE_EDIT_LABEL_MARGIN = 4,
	FORGE_EDIT_TITLE_SIZE = 64,
	/* the teams an object may be kept for, and a step of its respawn time
	(seconds) */
	FORGE_EDIT_TEAM_COUNT = 16,
	FORGE_EDIT_RESPAWN_STEP = 5
};

/* the popup's key on a keyboard (a controller's X is the tools' own) */
#define FORGE_EDIT_POPUP_KEY HALO_MOD_KEY_X

/* a step of the popup's rows: of the size (and the least and the most it
may be), of the turn (degrees) and of the height (world units) */
#define FORGE_EDIT_SCALE_STEP 0.1f
#define FORGE_EDIT_MINIMUM_SCALE 0.1f
#define FORGE_EDIT_MAXIMUM_SCALE 10.f
#define FORGE_EDIT_TURN_STEP 15.f
#define FORGE_EDIT_HEIGHT_STEP 0.05f
/* the box about the popup's object is at least this far across (half of it) */
#define FORGE_EDIT_MINIMUM_RADIUS 0.1f

/* 0xAARRGGBB */
#define FORGE_EDIT_LABEL_COLOR 0xffffff80UL
#define FORGE_EDIT_LABEL_BACKGROUND_COLOR 0x90000000UL

/* ---------- structures */

/* what makes an object again */
struct forge_edit_object
{
	long definition_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real scale;
	/* its properties in the map variant (forge_layout.c) */
	struct forge_object_properties properties;
};

/* ---------- globals */

static struct
{
	struct halo_mod_key_state delete_key;
	struct halo_mod_key_state popup_key;

	/* the map the removals belong to, and the game time of the last update
	(a checkpoint's revert takes it back) */
	char map_name[256];
	long game_time;
	short removed_count;
	struct forge_edit_object removed[FORGE_EDIT_UNDO_COUNT];

	/* this frame's marked object, for the render hook */
	boolean marking;
	long marked_object_index;

	/* the popup's object (NONE: none), and its name, the popup's title */
	long popup_object_index;
	char popup_title[FORGE_EDIT_TITLE_SIZE];
	/* its angles (forge_angles_of_orientation) while the rows turn it, and
	the forward and up they gave it: taken from the object again only when
	something else has turned it, so that a nose straight up, where the yaw
	and the roll are one, does not lose them */
	real popup_angles[3];
	real_vector3d popup_forward;
	real_vector3d popup_up;
} forge_edit_globals = { { 0 } };

/* the prefab an object is selected for (forge_prefabs.c) */
int forge_prefabs_object_selected(long object_index);
void forge_prefabs_object_toggle(long object_index);

/* ---------- private code */

/* the object's name for messages: the last part of its tag's name */
static char const *forge_edit_object_name(
	long definition_index)
{
	char const *name = tag_get_name(definition_index);
	char const *separator = strrchr(name, '\\');

	return separator ? separator + 1 : name;
}

/* TRUE when the object is a player's unit, or holds one (a vehicle) */
static boolean forge_edit_holds_player(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	boolean holds_player = FALSE;

	if (object)
	{
		long child_index;

		if (TEST_FLAG(_object_mask_unit, object->object.type) &&
			unit_get(object_index)->unit.player_index != NONE)
		{
			holds_player = TRUE;
		}
		for (child_index = object->object.first_child_object_index;
			!holds_player && child_index != NONE;
			child_index = object_get(child_index)->object.next_object_index)
		{
			holds_player = forge_edit_holds_player(child_index);
		}
	}

	return holds_player;
}

/* the object under the crosshair that may be copied and removed, or NONE */
static long forge_edit_marked_object(
	void)
{
	long object_index = forge_object_at_crosshair();

	if (object_index != NONE &&
		(!object_try_and_get(object_index) || forge_edit_holds_player(object_index)))
	{
		object_index = NONE;
	}

	return object_index;
}

static void forge_edit_record(
	long object_index,
	struct forge_edit_object *record)
{
	struct object_datum *object = object_get(object_index);

	record->definition_index = object->definition_index;
	record->position = object->object.position;
	record->forward = object->object.forward;
	record->up = object->object.up;
	record->scale = object->object.scale;
	forge_layout_get_properties(object_index, &record->properties);

	return;
}

/* makes the object of the record at the position; NONE if the game cannot */
static long forge_edit_create(
	struct forge_edit_object const *record,
	real_point3d const *position)
{
	struct object_placement_data data;
	long object_index;

	object_placement_data_new(&data, record->definition_index, NONE);
	data.position = *position;
	data.forward = record->forward;
	data.up = record->up;
	object_index = object_new(&data);
	if (object_index != NONE && record->scale != 1.f)
		objects_scripting_set_scale(object_index, record->scale, 0);
	/* kept by the tools' layout (port/linux/game/forge_layout.c), with the
	properties (its own id) */
	if (object_index != NONE)
	{
		forge_layout_note_spawned(object_index);
		forge_layout_set_properties(object_index, &record->properties);
	}

	return object_index;
}

static void forge_edit_delete(
	long object_index)
{
	/* nothing marked: nothing happens */
	if (object_index != NONE && forge_object_holds_flag(object_index))
	{
		terminal_printf(global_real_argb_orange, "forge_edit: %s is the game type's: it can be moved, not removed",
			forge_edit_object_name(object_get(object_index)->definition_index));
	}
	else if (object_index != NONE)
	{
		struct forge_edit_object *record;

		if (forge_edit_globals.removed_count == FORGE_EDIT_UNDO_COUNT)
		{
			/* the oldest is forgotten */
			memmove(
				&forge_edit_globals.removed[0],
				&forge_edit_globals.removed[1],
				sizeof(forge_edit_globals.removed[0]) * (FORGE_EDIT_UNDO_COUNT - 1));
			forge_edit_globals.removed_count--;
		}
		record = &forge_edit_globals.removed[forge_edit_globals.removed_count++];
		forge_edit_record(object_index, record);
		forge_layout_note_removed(object_index);
		object_delete(object_index);
		terminal_printf(global_real_argb_green, "forge_edit: removed %s (an object's popup, Undo remove, brings it back)",
			forge_edit_object_name(record->definition_index));
	}

	return;
}

static void forge_edit_undo(
	void)
{
	if (forge_edit_globals.removed_count == 0)
	{
		terminal_printf(global_real_argb_orange, "forge_edit: nothing removed to bring back");
	}
	else
	{
		struct forge_edit_object const *record =
			&forge_edit_globals.removed[--forge_edit_globals.removed_count];

		if (forge_edit_create(record, &record->position) == NONE)
		{
			terminal_printf(global_real_argb_orange, "forge_edit: %s could not be brought back",
				forge_edit_object_name(record->definition_index));
		}
		else
		{
			terminal_printf(global_real_argb_green, "forge_edit: brought back %s (%d more)",
				forge_edit_object_name(record->definition_index), forge_edit_globals.removed_count);
		}
	}

	return;
}

/* removals name the tags of the map they were made in, and a revert to a
checkpoint brings back what was removed since */
static void forge_edit_check_map(
	void)
{
	char const *map_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
	long game_time = game_time_get();

	if (strcmp(map_name, forge_edit_globals.map_name) != 0)
	{
		_snprintf(forge_edit_globals.map_name, sizeof(forge_edit_globals.map_name), "%s", map_name);
		forge_edit_globals.removed_count = 0;
		forge_edit_globals.popup_object_index = NONE;
	}
	else if (game_time < forge_edit_globals.game_time && forge_edit_globals.removed_count > 0)
	{
		forge_edit_globals.removed_count = 0;
		terminal_printf(global_real_argb_green, "forge_edit: back at a checkpoint, removals forgotten");
	}
	forge_edit_globals.game_time = game_time;

	return;
}

static struct halo_mod_menu const forge_edit_popup;

static void forge_edit_update(
	void)
{
	boolean delete_object = halo_mod_key_pressed(&forge_edit_globals.delete_key, HALO_MOD_KEY_DELETE, FALSE);
	boolean open_popup = halo_mod_key_pressed(&forge_edit_globals.popup_key, FORGE_EDIT_POPUP_KEY, FALSE);

	forge_edit_globals.marking = FALSE;
	forge_edit_globals.marked_object_index = NONE;
	if (local_player_get_player_index(FORGE_EDIT_LOCAL_PLAYER_INDEX) == NONE)
		return;
	forge_edit_check_map();
	/* only in Forge games: elsewhere Delete is the game's */
	if (!forge_mode_on())
		return;

	/* X again closes the popup; X while the tools hold an object: the popup
	about it */
	if (open_popup && forge_popup_is_open(&forge_edit_popup))
		forge_popup_close(&forge_edit_popup);
	else if (open_popup && forge_held_object() != NONE && !cinematic_in_progress())
		forge_popup_open(&forge_edit_popup);

	/* the tools' own keys come first while their menu is open or they hold
	an object; a cutscene's objects are the cutscene's */
	if (forge_busy() || cinematic_in_progress())
		return;

	forge_edit_globals.marking = forge_flying(FORGE_EDIT_LOCAL_PLAYER_INDEX);
	if (forge_edit_globals.marking || delete_object)
		forge_edit_globals.marked_object_index = forge_edit_marked_object();

	if (delete_object)
	{
		if (!halo_mods_authoritative())
		{
			terminal_printf(global_real_argb_orange, "forge_edit: removing an object is the host's");
		}
		else
		{
			forge_edit_delete(forge_edit_globals.marked_object_index);
			forge_edit_globals.marked_object_index = NONE;
		}
	}
	else if (open_popup && forge_edit_globals.marking && forge_edit_globals.marked_object_index != NONE)
	{
		/* (its opened hook takes the object at the crosshair) */
		forge_popup_open(&forge_edit_popup);
	}

	return;
}

static void forge_edit_render(
	void)
{
	short x0;
	short y0;
	short x1;
	short y1;
	struct object_datum *object;

	/* the marked object's name alone, in the tools' menu font */
	if (forge_edit_globals.marking &&
		forge_edit_globals.marked_object_index != NONE &&
		(object = object_try_and_get(forge_edit_globals.marked_object_index)) != NULL &&
		halo_mod_screen(&x0, &y0, &x1, &y1))
	{
		char const *label = forge_edit_object_name(object->definition_index);
		short center_x = (short)((x0 + x1) / 2);
		short top = (short)((y0 + y1) / 2 + FORGE_EDIT_LABEL_OFFSET);
		short width = (short)(forge_label_width(label) + 4 * FORGE_EDIT_LABEL_MARGIN);

		halo_mod_draw_box(
			(short)(center_x - width / 2),
			(short)(top - FORGE_EDIT_LABEL_MARGIN),
			(short)(center_x + width / 2),
			(short)(top + forge_label_height() + FORGE_EDIT_LABEL_MARGIN),
			FORGE_EDIT_LABEL_BACKGROUND_COLOR);
		forge_draw_label(x0, top, x1, HALO_MOD_TEXT_CENTER, FORGE_EDIT_LABEL_COLOR, label);
	}

	/* drawn only after an update: nothing is left over when the game stops
	updating the mods (the console, menus, loading) */
	forge_edit_globals.marking = FALSE;

	return;
}

/* a box about the popup's object, in each player's view (the world drawing
hook), so that it is seen which object the popup is about */
static void forge_edit_render_world(
	void)
{
	real_argb_color color = { { 1.f, 1.f, 1.f, 0.5f } };
	struct object_datum *object;
	real_point3d corners[8];
	real radius;
	short corner;

	if (!forge_popup_is_open(&forge_edit_popup) || forge_edit_globals.popup_object_index == NONE ||
		(object = object_try_and_get(forge_edit_globals.popup_object_index)) == NULL)
	{
		return;
	}

	radius = MAX(object->object.bounding_sphere_radius, FORGE_EDIT_MINIMUM_RADIUS);
	for (corner = 0; corner < 8; corner++)
	{
		set_real_point3d(&corners[corner],
			object->object.bounding_sphere_center.x + ((corner & 1) ? radius : -radius),
			object->object.bounding_sphere_center.y + ((corner & 2) ? radius : -radius),
			object->object.bounding_sphere_center.z + ((corner & 4) ? radius : -radius));
	}
	for (corner = 0; corner < 8; corner++)
	{
		short axis;

		/* each edge once, from its corner with the axis' bit clear */
		for (axis = 1; axis < 8; axis <<= 1)
		{
			if (!(corner & axis))
				rasterizer_debug_line_shaded(&corners[corner], &corners[corner | axis], &color, &color);
		}
	}

	return;
}

/* ---------- the object's popup */

enum
{
	_forge_edit_row_move = 0,
	_forge_edit_row_duplicate,
	_forge_edit_row_size,
	/* (the three in the order of the tools' angles) */
	_forge_edit_row_turn,
	_forge_edit_row_tilt,
	_forge_edit_row_roll,
	_forge_edit_row_height,
	_forge_edit_row_prefab,
	/* (its properties in the map variant) */
	_forge_edit_row_team,
	_forge_edit_row_at_start,
	_forge_edit_row_game_types,
	_forge_edit_row_respawn,
	_forge_edit_row_group,
	_forge_edit_row_comment,
	_forge_edit_row_remove,
	/* (about a held object: the step it turns in) */
	_forge_edit_row_undo,
	NUMBER_OF_FORGE_EDIT_ROWS
};

/* TRUE while the popup is about the object the tools hold */
static boolean forge_edit_popup_held(
	void)
{
	return forge_edit_globals.popup_object_index != NONE &&
		forge_edit_globals.popup_object_index == forge_held_object();
}

/* the popup's object, or NULL when it is gone or may not be changed now */
static struct object_datum *forge_edit_popup_object(
	void)
{
	long object_index = forge_edit_globals.popup_object_index;
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	if (object && (object->object.parent_object_index != NONE || forge_edit_holds_player(object_index)))
		object = NULL;

	return object;
}

/* the popup's object's angles: as the tools hold it, else its own */
static real const *forge_edit_popup_angles(
	struct object_datum const *object)
{
	real *angles = forge_edit_globals.popup_angles;

	if (forge_edit_popup_held())
	{
		forge_held_angles(angles);
	}
	else if (dot_product3d(&object->object.forward, &forge_edit_globals.popup_forward) < 0.99999f ||
		dot_product3d(&object->object.up, &forge_edit_globals.popup_up) < 0.99999f)
	{
		forge_angles_of_orientation(&object->object.forward.i, &object->object.up.i, angles);
		forge_edit_globals.popup_forward = object->object.forward;
		forge_edit_globals.popup_up = object->object.up;
	}

	return angles;
}

/* the popup has just been shown: it is about the object at the crosshair */
static void forge_edit_popup_opened(
	void)
{
	long object_index = local_player_get_player_index(FORGE_EDIT_LOCAL_PLAYER_INDEX) == NONE
		? NONE
		: forge_held_object() != NONE ? forge_held_object() : forge_edit_marked_object();

	forge_edit_globals.popup_object_index = object_index;
	/* (its angles are taken when first asked for) */
	forge_edit_globals.popup_forward = *global_zero_vector3d;
	forge_edit_globals.popup_up = *global_zero_vector3d;
	_snprintf(forge_edit_globals.popup_title, sizeof(forge_edit_globals.popup_title), "%s",
		object_index != NONE ? forge_edit_object_name(object_get(object_index)->definition_index) : "no object");
	forge_edit_globals.popup_title[sizeof(forge_edit_globals.popup_title) - 1] = 0;

	return;
}

/* the game types a row of Game types names, in the order it goes through
them: every one (0), then each alone */
static unsigned short const forge_edit_game_types[] =
{
	0,
	FLAG(game_engine_ctf),
	FLAG(game_engine_slayer),
	FLAG(game_engine_oddball),
	FLAG(game_engine_king),
	FLAG(game_engine_race)
};
static char const *const forge_edit_game_type_names[] =
{
	"every one",
	"Capture the Flag",
	"Slayer",
	"Oddball",
	"King of the Hill",
	"Race"
};

/* the place of the game types in that order, or NONE (several of them,
from a file) */
static short forge_edit_game_type_index(
	unsigned short game_types)
{
	short index;

	for (index = 0; index < NUMBEROF(forge_edit_game_types); index++)
	{
		if (forge_edit_game_types[index] == game_types)
			return index;
	}

	return NONE;
}

/* a property row's label and value */
static void forge_edit_property_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	struct forge_object_properties properties;
	boolean kept = forge_edit_popup_object() &&
		forge_layout_get_properties(forge_edit_globals.popup_object_index, &properties);
	short game_type_index;

	switch (row)
	{
	case _forge_edit_row_team:
		_snprintf(label, label_size, "Team");
		if (kept)
		{
			_snprintf(value, value_size, properties.team == NONE ? "none" : properties.team == 0 ? "red" :
				properties.team == 1 ? "blue" : "%d", (int)properties.team);
		}
		break;
	case _forge_edit_row_at_start:
		_snprintf(label, label_size, "Placed at start");
		if (kept)
			_snprintf(value, value_size, "%s", TEST_FLAG(properties.spawn_flags, 0) ? "yes" : "no, only in Forge");
		break;
	case _forge_edit_row_game_types:
		_snprintf(label, label_size, "Game types");
		if (kept)
		{
			game_type_index = forge_edit_game_type_index(properties.game_types);
			_snprintf(value, value_size, "%s", game_type_index != NONE ? forge_edit_game_type_names[game_type_index] :
				"several");
		}
		break;
	case _forge_edit_row_respawn:
		_snprintf(label, label_size, "Respawn");
		if (kept)
		{
			if (properties.respawn_seconds)
				_snprintf(value, value_size, "%d seconds", (int)properties.respawn_seconds);
			else
				_snprintf(value, value_size, "the game type's");
		}
		break;
	case _forge_edit_row_group:
		_snprintf(label, label_size, "Group");
		if (kept)
		{
			if (properties.group)
				_snprintf(value, value_size, "%d", (int)properties.group);
			else
				_snprintf(value, value_size, "none");
		}
		break;
	case _forge_edit_row_comment:
		_snprintf(label, label_size, "Comment");
		if (kept)
			_snprintf(value, value_size, "%s", properties.comment[0] ? properties.comment : "(type one)");
		break;
	}
	/* (the host's, or the game's own object: not one the variant keeps) */
	if (!kept && forge_edit_popup_object())
		_snprintf(value, value_size, "%s", halo_mods_authoritative() ? "not kept" : "the host's");

	return;
}

/* the object the comment being typed is about */
static long forge_edit_comment_object_index = NONE;

static void forge_edit_comment_typed(
	char const *text)
{
	struct forge_object_properties properties;

	if (forge_edit_comment_object_index != NONE &&
		forge_layout_get_properties(forge_edit_comment_object_index, &properties))
	{
		_snprintf(properties.comment, sizeof(properties.comment), "%s", text);
		properties.comment[sizeof(properties.comment) - 1] = 0;
		if (forge_layout_set_properties(forge_edit_comment_object_index, &properties))
			terminal_printf(global_real_argb_green, "forge_edit: the comment is kept with the variant");
	}
	forge_edit_comment_object_index = NONE;

	return;
}

/* left and right change a property row's value, enter or A types the
comment */
static void forge_edit_property_change(
	long object_index,
	short row,
	int direction)
{
	struct forge_object_properties properties;
	short step = direction < 0 ? -1 : 1;
	short game_type_index;

	if (!forge_layout_get_properties(object_index, &properties))
	{
		terminal_printf(global_real_argb_orange, "forge_edit: the map variant does not keep this object");
		return;
	}
	if (row == _forge_edit_row_comment)
	{
		if (direction == 0)
		{
			forge_edit_comment_object_index = object_index;
			if (!forge_text_entry_begin("Comment (kept with the map variant)", properties.comment,
				FORGE_OBJECT_COMMENT_SIZE - 1, forge_edit_comment_typed))
			{
				forge_edit_comment_object_index = NONE;
				terminal_printf(global_real_argb_orange, "forge_edit: something is being typed already");
			}
		}
		return;
	}
	/* (enter or A as right) */
	switch (row)
	{
	case _forge_edit_row_team:
		/* none (NONE), then 0 to 15 */
		properties.team = (short)((properties.team + 1 + step + FORGE_EDIT_TEAM_COUNT + 1) % (FORGE_EDIT_TEAM_COUNT + 1) - 1);
		break;
	case _forge_edit_row_at_start:
		properties.spawn_flags ^= FORGE_OBJECT_SPAWN_AT_START;
		break;
	case _forge_edit_row_game_types:
		game_type_index = forge_edit_game_type_index(properties.game_types);
		game_type_index = (short)(game_type_index == NONE ? 0 :
			(game_type_index + step + NUMBEROF(forge_edit_game_types)) % NUMBEROF(forge_edit_game_types));
		properties.game_types = forge_edit_game_types[game_type_index];
		break;
	case _forge_edit_row_respawn:
		properties.respawn_seconds = (short)PIN(properties.respawn_seconds + step * FORGE_EDIT_RESPAWN_STEP, 0,
			FORGE_OBJECT_MAXIMUM_RESPAWN_SECONDS);
		break;
	case _forge_edit_row_group:
		properties.group = (short)((properties.group + step + FORGE_OBJECT_MAXIMUM_GROUP + 1) % (FORGE_OBJECT_MAXIMUM_GROUP + 1));
		break;
	}
	forge_layout_set_properties(object_index, &properties);

	return;
}

static short forge_edit_popup_row_count(
	void)
{
	return NUMBER_OF_FORGE_EDIT_ROWS;
}

static void forge_edit_popup_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	struct object_datum *object = forge_edit_popup_object();

	switch (row)
	{
	case _forge_edit_row_move:
		_snprintf(label, label_size, forge_edit_popup_held() ? "Put down" : "Move");
		break;
	case _forge_edit_row_duplicate:
		_snprintf(label, label_size, "Duplicate");
		break;
	case _forge_edit_row_size:
		_snprintf(label, label_size, "Size");
		if (object)
			_snprintf(value, value_size, "%.1f", object->object.scale > 0.f ? object->object.scale : 1.f);
		break;
	case _forge_edit_row_turn:
	case _forge_edit_row_tilt:
	case _forge_edit_row_roll:
		_snprintf(label, label_size,
			row == _forge_edit_row_turn ? "Turn" : row == _forge_edit_row_tilt ? "Tilt" : "Roll");
		if (object)
		{
			real degrees = forge_edit_popup_angles(object)[row - _forge_edit_row_turn] * 180.f / 3.14159265f;

			_snprintf(value, value_size, "%d degrees", (((int)floor(degrees + 0.5f) % 360) + 360) % 360);
		}
		break;
	case _forge_edit_row_height:
		_snprintf(label, label_size, "Height");
		if (object)
			_snprintf(value, value_size, "%.2f", object->object.position.z);
		break;
	case _forge_edit_row_prefab:
		_snprintf(label, label_size, "Prefab");
		_snprintf(value, value_size, "%s",
			forge_prefabs_object_selected(forge_edit_globals.popup_object_index) ? "selected" : "not selected");
		break;
	case _forge_edit_row_team:
	case _forge_edit_row_at_start:
	case _forge_edit_row_game_types:
	case _forge_edit_row_respawn:
	case _forge_edit_row_group:
	case _forge_edit_row_comment:
		forge_edit_property_text(row, label, label_size, value, value_size);
		break;
	case _forge_edit_row_remove:
		_snprintf(label, label_size, "Remove");
		break;
	case _forge_edit_row_undo:
		if (forge_edit_popup_held())
		{
			_snprintf(label, label_size, "Step (turning, held)");
			_snprintf(value, value_size, "%s", forge_held_step(0));
		}
		else
		{
			_snprintf(label, label_size, "Undo remove");
			_snprintf(value, value_size, "%d to bring back", forge_edit_globals.removed_count);
		}
		break;
	}

	return;
}

/* puts the object there, still, and has the tools' layout keep it as moved */
static void forge_edit_set_placement(
	long object_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up)
{
	struct object_datum *object = object_get(object_index);

	/* (an object not in the world, garbage, stays) */
	if (!TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		return;
	object_set_position(object_index, (real_point3d *)position, (real_vector3d *)forward, (real_vector3d *)up);
	object->object.translational_velocity = *global_zero_vector3d;
	object->object.angular_velocity = *global_zero_vector3d;
	forge_layout_note_placed(object_index);
	/* a system link host: not at rest, so that its clients are told where it
	is (network_objects.c) */
	if (game_connection() == _game_connection_network_server)
		SET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);

	return;
}

/* a copy of the object, where it is, held by the tools until it is put down
(cancelled, it is removed); of the object the tools hold already, a copy
left where it is */
static boolean forge_edit_duplicate(
	long object_index)
{
	struct forge_edit_object record;
	short used;
	short maximum;
	long copy_index;
	boolean held = FALSE;

	forge_layout_object_budget(&used, &maximum);
	forge_edit_record(object_index, &record);
	if (forge_object_holds_flag(object_index))
	{
		terminal_printf(global_real_argb_orange, "forge_edit: %s is the game type's: it cannot be copied",
			forge_edit_object_name(record.definition_index));
	}
	else if (used >= maximum)
	{
		terminal_printf(global_real_argb_orange, "forge_edit: the layout cannot keep another object");
	}
	else if ((copy_index = forge_edit_create(&record, &record.position)) == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_edit: %s could not be created",
			forge_edit_object_name(record.definition_index));
	}
	else if (forge_held_object() != NONE)
	{
		terminal_printf(global_real_argb_green, "forge_edit: left a copy of %s here",
			forge_edit_object_name(record.definition_index));
	}
	else if (!(held = forge_hold_object(copy_index, NULL)))
	{
		object_delete(copy_index);
		terminal_printf(global_real_argb_orange, "forge_edit: the tools hold something already");
	}

	return held;
}

/* a step of a turning row, in radians: right turns it clockwise from above,
lifts its nose and rolls it to the right */
static real forge_edit_turn_step(
	short row,
	int direction)
{
	return DEGREES_TO_RADIANS(FORGE_EDIT_TURN_STEP) * (real)(row == _forge_edit_row_turn ? -direction : direction);
}

/* left and right change a row's value, enter or A do its action; TRUE
closes the popup */
static int forge_edit_popup_row_change(
	short row,
	int direction)
{
	long object_index = forge_edit_globals.popup_object_index;
	struct object_datum *object = forge_edit_popup_object();
	boolean held = forge_edit_popup_held();
	int close_popup = FALSE;

	/* a held object: how it is held, on a system link client too (the host
	is asked when it is put down) */
	if (held && row == _forge_edit_row_undo)
	{
		forge_held_step(direction < 0 ? -1 : 1);
		return FALSE;
	}
	if (held && row == _forge_edit_row_move)
	{
		if (direction == 0)
		{
			forge_held_put_down();
			close_popup = TRUE;
		}
		return close_popup;
	}
	if (held && row >= _forge_edit_row_turn && row <= _forge_edit_row_height)
	{
		if (direction != 0)
		{
			forge_held_adjust(
				row == _forge_edit_row_height ? 0 : row - _forge_edit_row_turn,
				row == _forge_edit_row_height ? 0.f : forge_edit_turn_step(row, direction),
				row == _forge_edit_row_height ? FORGE_EDIT_HEIGHT_STEP * (real)direction : 0.f);
		}
		return FALSE;
	}

	/* (what was removed comes back whatever the popup's object is) */
	if (row == _forge_edit_row_undo)
	{
		if (direction != 0)
			return FALSE;
		if (!halo_mods_authoritative())
		{
			terminal_printf(global_real_argb_orange, "forge_edit: bringing an object back is the host's");
			return FALSE;
		}
		forge_edit_undo();
		return TRUE;
	}
	if (!object)
	{
		terminal_printf(global_real_argb_orange, "forge_edit: the object is gone");
		return TRUE;
	}
	if (row == _forge_edit_row_move)
	{
		/* (a system link client's too: the tools ask the host) */
		if (direction == 0 && !(close_popup = forge_pick_up(object_index)))
			terminal_printf(global_real_argb_orange, "forge_edit: the object cannot be picked up now");
		return close_popup;
	}
	if (row == _forge_edit_row_prefab)
	{
		forge_prefabs_object_toggle(object_index);
		return FALSE;
	}
	if (!halo_mods_authoritative())
	{
		terminal_printf(global_real_argb_orange, "forge_edit: on a client only Move works, the rest is the host's");
		return FALSE;
	}
	if (row >= _forge_edit_row_team && row <= _forge_edit_row_comment)
	{
		forge_edit_property_change(object_index, row, direction);
		return FALSE;
	}

	switch (row)
	{
	case _forge_edit_row_duplicate:
		if (direction == 0)
			close_popup = forge_edit_duplicate(object_index);
		break;
	case _forge_edit_row_size:
		if (direction != 0)
		{
			real scale = object->object.scale > 0.f ? object->object.scale : 1.f;

			/* to the next tenth */
			scale = (real)floor(scale / FORGE_EDIT_SCALE_STEP + 0.5f + (real)direction) * FORGE_EDIT_SCALE_STEP;
			objects_scripting_set_scale(object_index, PIN(scale, FORGE_EDIT_MINIMUM_SCALE, FORGE_EDIT_MAXIMUM_SCALE), 0);
			forge_layout_note_placed(object_index);
		}
		break;
	case _forge_edit_row_turn:
	case _forge_edit_row_tilt:
	case _forge_edit_row_roll:
		if (direction != 0)
		{
			real angles[3];
			real_vector3d forward;
			real_vector3d up;

			csmemcpy(angles, forge_edit_popup_angles(object), sizeof(angles));
			angles[row - _forge_edit_row_turn] += forge_edit_turn_step(row, direction);
			forge_orientation_of_angles(angles, &forward.i, &up.i);
			forge_edit_set_placement(object_index, &object->object.position, &forward, &up);
			/* (as the object now has them) */
			csmemcpy(forge_edit_globals.popup_angles, angles, sizeof(angles));
			forge_edit_globals.popup_forward = object->object.forward;
			forge_edit_globals.popup_up = object->object.up;
		}
		break;
	case _forge_edit_row_height:
		if (direction != 0)
		{
			real_point3d position = object->object.position;

			position.z += FORGE_EDIT_HEIGHT_STEP * (real)direction;
			forge_edit_set_placement(object_index, &position, &object->object.forward, &object->object.up);
		}
		break;
	case _forge_edit_row_remove:
		if (direction == 0)
		{
			forge_edit_delete(object_index);
			forge_edit_globals.popup_object_index = NONE;
			close_popup = TRUE;
		}
		break;
	}

	return close_popup;
}

static struct halo_mod_menu const forge_edit_popup =
{
	forge_edit_globals.popup_title,
	forge_edit_popup_row_count,
	forge_edit_popup_row_text,
	forge_edit_popup_row_change,
	forge_edit_popup_opened
};

/* ---------- the mod */

/* Y while flying (the tools' remove hook): removed here, so Undo remove
brings it back; taken even with nothing marked, so the tools never remove
what may not be (a vehicle holding a player) */
static int forge_edit_remove(
	void)
{
	long object_index = cinematic_in_progress() ? NONE : forge_edit_marked_object();

	if (object_index == NONE)
		terminal_printf(global_real_argb_orange, "forge_edit: nothing to remove there");
	else
		forge_edit_delete(object_index);

	return TRUE;
}

/* X on an object, or with one held: the object's popup opens */
static int forge_edit_properties(
	float *distance)
{
	long object_index = local_player_get_player_index(FORGE_EDIT_LOCAL_PLAYER_INDEX) != NONE
		? forge_edit_marked_object()
		: NONE;

	/* (the object the tools hold, before anything at the crosshair) */
	if (forge_held_object() != NONE)
	{
		*distance = 0.f;
		return TRUE;
	}
	if (object_index == NONE)
		return FALSE;
	*distance = distance3d(&observer_get_camera(FORGE_EDIT_LOCAL_PLAYER_INDEX)->position,
		&object_get(object_index)->object.bounding_sphere_center);

	return TRUE;
}

struct halo_mod const forge_edit_mod =
{
	"forge_edit",
	forge_edit_update,
	forge_edit_render,
	NULL,
	NULL,
	forge_edit_render_world,
	NULL,
	NULL,
	forge_edit_remove,
	NULL,
	NULL,
	NULL,
	forge_edit_properties,
	&forge_edit_popup
};
