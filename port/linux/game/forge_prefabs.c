/*
FORGE_PREFABS.C

One of Forge's parts (halo_mod.h, halo_mods.c): several objects selected with the dev
tools' crosshair (port/linux/game/forge.c), saved together as a prefab, and
placed again as often as wanted.

All of it is on the "Prefab" tab of the dev tools' menu (1, D-pad right or,
flying, X; LB / RB, Page Up / Page Down, or T / V change tab). The rows:

	At the crosshair  the object the dev tools would pick up, or nothing
	Select            select it, or leave it out again (B on a keyboard
	                  while the camera flies, with the menu closed; an
	                  object's popup, X on it, has a row for it too:
	                  forge_edit.c)
	Clear             select nothing
	Save              the selection as a new prefab; its name is typed on
	                  the keyboard
	Prefab            the saved prefab to place or delete (left, right)
	Place             make its objects and hold them at the crosshair, as
	                  the tools hold one object: left and right turn them
	                  (T: about another axis; V or X: in steps), up and
	                  down raise them, the right stick orbits the camera
	                  around them (with RT, or the right mouse button, it
	                  turns them), enter or A put them down, escape or B
	                  remove them again
	Delete            delete the prefab (asked twice)

The selected objects are marked in the world with a box each. A prefab that
is held is its objects themselves, made with the game's own object_new: the
tools hold the one nearest the prefab's middle (forge_hold_object,
port/linux/game/forge.c) and the others keep their places and turns from
it, so the prefab moves and turns as one thing. Put down, they are kept by
the tools' layout as spawned objects are (port/linux/game/forge_layout.c).

A prefab holds each object's tag, turn and scale, and its place from the
prefab's own origin: the middle of its objects, at the height of the
lowest. Put down, that origin is where the crosshair points.

Files: u:\forge\<map>\prefab_NN.txt (NN 01 to 32) in the save root, beside
the map's layouts, text, one thing a line: prefabs belong to the map they
were made on, whose tags they name. An object whose tag is not in the map
(a layout's imports are not loaded) is left out when the prefab is placed.

Selecting and saving read the game only, and work on every machine; placing
makes objects, and is the host's in a system link game.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "camera/director_forge.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/object_types.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "forge_prefabs_keys.h"

#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	FORGE_PREFABS_SLOT_COUNT = 32,
	FORGE_PREFABS_MAXIMUM_OBJECTS = 64,
	FORGE_PREFABS_NAME_SIZE = 48,
	FORGE_PREFABS_MAP_NAME_SIZE = 64,
	FORGE_PREFABS_PATH_SIZE = 256,
	FORGE_PREFABS_LINE_SIZE = 512,
	FORGE_PREFABS_VERSION = 1,
	FORGE_PREFABS_LOCAL_PLAYER_INDEX = 0,
	/* how long "Delete" waits to be chosen again (ticks) */
	FORGE_PREFABS_CONFIRM_TICKS = 150,
	FORGE_PREFABS_LABEL_OFFSET = 14,
	FORGE_PREFABS_LABEL_MARGIN = 4
};

/* what is selected: units (never a player's), items, scenery and devices,
as a layout keeps them */
#define FORGE_PREFABS_OBJECT_MASK (_object_mask_unit | _object_mask_item | _object_mask_scenery | _object_mask_device)

/* how far from its prefab's origin an object may be, and how large (world
units, and times its own size) */
#define FORGE_PREFABS_OFFSET_BOUND 1024.f
#define FORGE_PREFABS_MINIMUM_SCALE 0.01f
#define FORGE_PREFABS_MAXIMUM_SCALE 100.f
/* an object's box is at least this far across (half of it) */
#define FORGE_PREFABS_MINIMUM_RADIUS 0.1f

/* 0xAARRGGBB */
#define FORGE_PREFABS_LABEL_COLOR 0xff80ff80UL
#define FORGE_PREFABS_LABEL_BACKGROUND_COLOR 0x90000000UL

/* ---------- structures */

/* what makes an object of a prefab again, from the prefab's origin */
struct forge_prefab_object
{
	long definition_index;
	real_point3d offset;
	real_vector3d forward;
	real_vector3d up;
	real scale;
};

/* ---------- globals */

boolean tag_index_is_group(long tag_index, long group_tag);
/* forge_layout.c's: a file written beside itself, then in its place */
FILE *forge_file_begin(char const *path, char *temporary_path, unsigned long size);
int forge_file_commit(FILE *file, char const *temporary_path, char const *path);

static struct
{
	struct halo_mod_key_state select_key;

	/* the map the selection and the prefabs belong to, the last part of
	its scenario's name */
	char map_name[FORGE_PREFABS_MAP_NAME_SIZE];

	short selected_count;
	long selected[FORGE_PREFABS_MAXIMUM_OBJECTS];

	/* the saved prefabs' names ("" none there), and the chosen one (0:
	none) */
	char slot_names[FORGE_PREFABS_SLOT_COUNT + 1][FORGE_PREFABS_NAME_SIZE];
	short chosen_slot;
	/* when "Delete" was chosen and waits to be chosen again (NONE: it does
	not) */
	long delete_time;

	/* the prefab that is held: what it is made of, its objects (NONE: one
	the game could not make), the one of them the tools hold, which the
	others follow, and the game time it was last moved at (a checkpoint's
	revert takes it back) */
	boolean holding;
	short object_count;
	struct forge_prefab_object objects[FORGE_PREFABS_MAXIMUM_OBJECTS];
	long held_objects[FORGE_PREFABS_MAXIMUM_OBJECTS];
	short held_anchor;
	long held_time;
	char held_name[FORGE_PREFABS_NAME_SIZE];

	/* this frame's label, for the render hook */
	boolean marking;
} forge_prefabs_globals = { { 0 } };

/* ---------- private code */

/* the object's name for messages: the last part of its tag's name */
static char const *forge_prefabs_object_name(
	long definition_index)
{
	char const *name = tag_get_name(definition_index);
	char const *separator = strrchr(name, '\\');

	return separator ? separator + 1 : name;
}

/* TRUE when the object is a player's unit, or holds one (a vehicle) */
static boolean forge_prefabs_holds_player(
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
			holds_player = forge_prefabs_holds_player(child_index);
		}
	}

	return holds_player;
}

/* the object under the crosshair that may be selected, or NONE */
static long forge_prefabs_marked_object(
	void)
{
	long object_index = local_player_get_player_index(FORGE_PREFABS_LOCAL_PLAYER_INDEX) != NONE
		? forge_object_at_crosshair()
		: NONE;
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	if (!object || !TEST_FLAG(FORGE_PREFABS_OBJECT_MASK, object->object.type) ||
		object->object.parent_object_index != NONE || forge_prefabs_holds_player(object_index) ||
		forge_object_holds_flag(object_index))
	{
		object_index = NONE;
	}

	return object_index;
}

/* ---------- the selection */

/* where the object is in the selection, or NONE */
static short forge_prefabs_selection_find(
	long object_index)
{
	short index;

	for (index = 0; index < forge_prefabs_globals.selected_count; index++)
	{
		if (forge_prefabs_globals.selected[index] == object_index)
			return index;
	}

	return NONE;
}

/* without the objects that are gone, or that something has picked up */
static void forge_prefabs_selection_prune(
	void)
{
	short read;
	short write = 0;

	for (read = 0; read < forge_prefabs_globals.selected_count; read++)
	{
		long object_index = forge_prefabs_globals.selected[read];
		struct object_datum *object = object_try_and_get(object_index);

		if (object && object->object.parent_object_index == NONE)
			forge_prefabs_globals.selected[write++] = object_index;
	}
	forge_prefabs_globals.selected_count = write;

	return;
}

static void forge_prefabs_selection_toggle(
	long object_index)
{
	short index;

	if (object_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: nothing to select there");
		return;
	}

	forge_prefabs_selection_prune();
	index = forge_prefabs_selection_find(object_index);
	if (index != NONE)
	{
		forge_prefabs_globals.selected[index] =
			forge_prefabs_globals.selected[--forge_prefabs_globals.selected_count];
		terminal_printf(global_real_argb_green, "forge_prefabs: %s left out (%d selected)",
			forge_prefabs_object_name(object_get(object_index)->definition_index),
			forge_prefabs_globals.selected_count);
	}
	else if (forge_prefabs_globals.selected_count == FORGE_PREFABS_MAXIMUM_OBJECTS)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: a prefab holds %d objects at most",
			FORGE_PREFABS_MAXIMUM_OBJECTS);
	}
	else
	{
		forge_prefabs_globals.selected[forge_prefabs_globals.selected_count++] = object_index;
		terminal_printf(global_real_argb_green, "forge_prefabs: selected %s (%d selected)",
			forge_prefabs_object_name(object_get(object_index)->definition_index),
			forge_prefabs_globals.selected_count);
	}

	return;
}

/* ---------- files */

static void forge_prefabs_directory(
	char *path,
	unsigned long size)
{
	_snprintf(path, size, "u:\\forge\\%s", forge_prefabs_globals.map_name);
	path[size - 1] = 0;

	return;
}

static void forge_prefabs_slot_path(
	short slot,
	char *path,
	unsigned long size)
{
	_snprintf(path, size, "u:\\forge\\%s\\prefab_%02d.txt", forge_prefabs_globals.map_name, slot);
	path[size - 1] = 0;

	return;
}

/* a name as it is kept: printable, without spaces around it */
static void forge_prefabs_clean_text(
	char const *text,
	char *result,
	unsigned long size)
{
	unsigned long length = 0;

	while (*text == ' ')
		text++;
	for (; *text && length < size - 1; text++)
	{
		if (*text >= ' ' && *text < 127 && *text != '%')
			result[length++] = *text;
	}
	while (length > 0 && result[length - 1] == ' ')
		length--;
	result[length] = 0;

	return;
}

/* the names of the map's saved prefabs; the chosen one stays if it is
still there, else the first is chosen */
static void forge_prefabs_read_slots(
	void)
{
	char path[FORGE_PREFABS_PATH_SIZE];
	char line[FORGE_PREFABS_LINE_SIZE];
	short slot;

	for (slot = 1; slot <= FORGE_PREFABS_SLOT_COUNT; slot++)
	{
		char *name = forge_prefabs_globals.slot_names[slot];
		FILE *file = NULL;

		name[0] = 0;
		if (forge_prefabs_globals.map_name[0])
		{
			forge_prefabs_slot_path(slot, path, sizeof(path));
			file = fopen(path, "r");
		}
		if (file)
		{
			if (fgets(line, sizeof(line), file) && strncmp(line, "name ", 5) == 0)
				forge_prefabs_clean_text(line + 5, name, FORGE_PREFABS_NAME_SIZE);
			if (!name[0])
				_snprintf(name, FORGE_PREFABS_NAME_SIZE, "Prefab %d", slot);
			fclose(file);
		}
	}

	if (forge_prefabs_globals.chosen_slot < 1 || forge_prefabs_globals.chosen_slot > FORGE_PREFABS_SLOT_COUNT ||
		!forge_prefabs_globals.slot_names[forge_prefabs_globals.chosen_slot][0])
	{
		forge_prefabs_globals.chosen_slot = 0;
		for (slot = 1; slot <= FORGE_PREFABS_SLOT_COUNT && !forge_prefabs_globals.chosen_slot; slot++)
		{
			if (forge_prefabs_globals.slot_names[slot][0])
				forge_prefabs_globals.chosen_slot = slot;
		}
	}

	return;
}

static short forge_prefabs_saved_count(
	void)
{
	short count = 0;
	short slot;

	for (slot = 1; slot <= FORGE_PREFABS_SLOT_COUNT; slot++)
	{
		if (forge_prefabs_globals.slot_names[slot][0])
			count++;
	}

	return count;
}

static short forge_prefabs_first_free_slot(
	void)
{
	short slot;

	for (slot = 1; slot <= FORGE_PREFABS_SLOT_COUNT; slot++)
	{
		if (!forge_prefabs_globals.slot_names[slot][0])
			return slot;
	}

	return 0;
}

/* the selection as a prefab of that name in the slot's file */
static boolean forge_prefabs_save(
	short slot,
	char const *name)
{
	char path[FORGE_PREFABS_PATH_SIZE];
	char temporary_path[FORGE_PREFABS_PATH_SIZE];
	real_point3d origin;
	short index;
	FILE *file;

	forge_prefabs_selection_prune();
	if (forge_prefabs_globals.selected_count == 0)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: nothing is selected any more");
		return FALSE;
	}

	/* the middle of the objects, at the height of the lowest */
	set_real_point3d(&origin, 0.f, 0.f, 0.f);
	for (index = 0; index < forge_prefabs_globals.selected_count; index++)
	{
		real_point3d const *position = &object_get(forge_prefabs_globals.selected[index])->object.position;

		origin.x += position->x / forge_prefabs_globals.selected_count;
		origin.y += position->y / forge_prefabs_globals.selected_count;
		if (index == 0 || position->z < origin.z)
			origin.z = position->z;
	}

	_mkdir("u:\\forge");
	forge_prefabs_directory(path, sizeof(path));
	_mkdir(path);
	forge_prefabs_slot_path(slot, path, sizeof(path));
	if ((file = forge_file_begin(path, temporary_path, sizeof(temporary_path))) == NULL)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: could not write %s", path);
		return FALSE;
	}
	fprintf(file, "name %s\n", name);
	fprintf(file, "# a Forge prefab (port/linux/game/forge_prefabs.c)\n");
	fprintf(file, "version %d\n", FORGE_PREFABS_VERSION);
	fprintf(file, "map %s\n", tag_get_name(global_scenario_index));
	for (index = 0; index < forge_prefabs_globals.selected_count; index++)
	{
		struct object_datum *object = object_get(forge_prefabs_globals.selected[index]);
		unsigned long group = tag_get_group_tag(object->definition_index);

		fprintf(file, "object %c%c%c%c %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.5f %.5f %.4f %s\n",
			(char)(group >> 24), (char)(group >> 16), (char)(group >> 8), (char)group,
			object->object.position.x - origin.x, object->object.position.y - origin.y,
			object->object.position.z - origin.z,
			object->object.forward.i, object->object.forward.j, object->object.forward.k,
			object->object.up.i, object->object.up.j, object->object.up.k,
			object->object.scale > 0.f ? object->object.scale : 1.f,
			tag_get_name(object->definition_index));
	}
	if (!forge_file_commit(file, temporary_path, path))
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: could not write %s", path);
		return FALSE;
	}

	return TRUE;
}

/* TRUE when the number is one, and no further from 0 than bound */
static boolean forge_prefabs_real_valid(
	real value,
	real bound)
{
	return value == value && value >= -bound && value <= bound;
}

/* an "object" line of a prefab's file as an object of this map; FALSE when
it is none, or cannot be made here */
static boolean forge_prefabs_read_object(
	char const *line,
	struct forge_prefab_object *result)
{
	char group[5];
	float values[10];
	int name_offset = 0;
	long group_tag;
	short index;

	if (sscanf(line, "object %4s %f %f %f %f %f %f %f %f %f %f %n", group,
		&values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
		&values[6], &values[7], &values[8], &values[9], &name_offset) < 11 || name_offset <= 0 ||
		strlen(group) != 4 || !line[name_offset])
	{
		return FALSE;
	}
	for (index = 0; index < 10; index++)
	{
		if (!forge_prefabs_real_valid(values[index], FORGE_PREFABS_OFFSET_BOUND))
			return FALSE;
	}

	group_tag = (long)(((unsigned long)(unsigned char)group[0] << 24) | ((unsigned long)(unsigned char)group[1] << 16) |
		((unsigned long)(unsigned char)group[2] << 8) | (unsigned long)(unsigned char)group[3]);
	result->definition_index = tag_loaded(group_tag, line + name_offset);
	if (result->definition_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: %s is not in this map, left out", line + name_offset);
		return FALSE;
	}
	/* (a file may name any tag: only the objects a layout keeps are made) */
	if (!tag_index_is_group(result->definition_index, OBJECT_DEFINITION_TAG) ||
		!TEST_FLAG(FORGE_PREFABS_OBJECT_MASK, object_definition_get(result->definition_index)->object.type))
	{
		return FALSE;
	}

	set_real_point3d(&result->offset, values[0], values[1], values[2]);
	set_real_vector3d(&result->forward, values[3], values[4], values[5]);
	set_real_vector3d(&result->up, values[6], values[7], values[8]);
	/* two axes one long and about square, then made exactly so, else
	upright */
	if (normalize3d(&result->forward) == 0.f || normalize3d(&result->up) == 0.f ||
		fabs(dot_product3d(&result->forward, &result->up)) > 0.05f)
	{
		result->forward = *global_forward3d;
		result->up = *global_up3d;
	}
	else
	{
		real along = dot_product3d(&result->forward, &result->up);

		result->up.i -= along * result->forward.i;
		result->up.j -= along * result->forward.j;
		result->up.k -= along * result->forward.k;
		normalize3d(&result->up);
	}
	result->scale = values[9] >= FORGE_PREFABS_MINIMUM_SCALE && values[9] <= FORGE_PREFABS_MAXIMUM_SCALE
		? values[9]
		: 1.f;

	return TRUE;
}

/* the slot's prefab as the one to hold; FALSE when none of it can be made
on this map */
static boolean forge_prefabs_load(
	short slot)
{
	char path[FORGE_PREFABS_PATH_SIZE];
	char line[FORGE_PREFABS_LINE_SIZE];
	FILE *file;

	forge_prefabs_globals.object_count = 0;
	forge_prefabs_slot_path(slot, path, sizeof(path));
	if ((file = fopen(path, "r")) == NULL)
		return FALSE;
	while (fgets(line, sizeof(line), file) && forge_prefabs_globals.object_count < FORGE_PREFABS_MAXIMUM_OBJECTS)
	{
		line[strcspn(line, "\r\n")] = 0;
		if (forge_prefabs_read_object(line, &forge_prefabs_globals.objects[forge_prefabs_globals.object_count]))
			forge_prefabs_globals.object_count++;
	}
	fclose(file);
	csstrcpy(forge_prefabs_globals.held_name, forge_prefabs_globals.slot_names[slot]);

	return forge_prefabs_globals.object_count > 0;
}

/* ---------- holding a prefab */

/* a vector of the prefab as it is now: as far along the held anchor's axes
as it is along the anchor's axes in the prefab */
static void forge_prefabs_turn(
	real_vector3d const *vector,
	struct forge_prefab_object const *anchor,
	struct object_datum const *anchor_object,
	real_vector3d *result)
{
	real_vector3d left;
	real_vector3d held_left;
	real along_forward;
	real along_left;
	real along_up;

	cross_product3d(&anchor->up, &anchor->forward, &left);
	cross_product3d(&anchor_object->object.up, &anchor_object->object.forward, &held_left);
	along_forward = dot_product3d(vector, &anchor->forward);
	along_left = dot_product3d(vector, &left);
	along_up = dot_product3d(vector, &anchor->up);
	set_real_vector3d(result,
		along_forward * anchor_object->object.forward.i + along_left * held_left.i + along_up * anchor_object->object.up.i,
		along_forward * anchor_object->object.forward.j + along_left * held_left.j + along_up * anchor_object->object.up.j,
		along_forward * anchor_object->object.forward.k + along_left * held_left.k + along_up * anchor_object->object.up.k);

	return;
}

/* the tools have put the anchor somewhere (the hold's moved hook): the
prefab's other objects go where they are from it, and stay still */
static void forge_prefabs_hold_moved(
	long anchor_object_index)
{
	struct forge_prefab_object const *anchor = &forge_prefabs_globals.objects[forge_prefabs_globals.held_anchor];
	struct object_datum *anchor_object = object_try_and_get(anchor_object_index);
	short index;

	if (!forge_prefabs_globals.holding || !anchor_object)
		return;
	forge_prefabs_globals.held_time = game_time_get();
	for (index = 0; index < forge_prefabs_globals.object_count; index++)
	{
		struct forge_prefab_object const *prefab_object = &forge_prefabs_globals.objects[index];
		long object_index = forge_prefabs_globals.held_objects[index];
		struct object_datum *object = index != forge_prefabs_globals.held_anchor && object_index != NONE
			? object_try_and_get(object_index)
			: NULL;
		real_vector3d offset;
		real_point3d position;
		real_vector3d forward;
		real_vector3d up;

		/* (an object not in the world, garbage, stays) */
		if (!object || !TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
			continue;
		set_real_vector3d(&offset, prefab_object->offset.x - anchor->offset.x, prefab_object->offset.y - anchor->offset.y,
			prefab_object->offset.z - anchor->offset.z);
		forge_prefabs_turn(&offset, anchor, anchor_object, &offset);
		set_real_point3d(&position, anchor_object->object.position.x + offset.i,
			anchor_object->object.position.y + offset.j, anchor_object->object.position.z + offset.k);
		forge_prefabs_turn(&prefab_object->forward, anchor, anchor_object, &forward);
		forge_prefabs_turn(&prefab_object->up, anchor, anchor_object, &up);
		object_set_position(object_index, &position, &forward, &up);
		object->object.translational_velocity = *global_zero_vector3d;
		object->object.angular_velocity = *global_zero_vector3d;
		/* a system link host: not at rest, so that its clients see it move
		as it is held (network_objects.c) */
		if (game_connection() == _game_connection_network_server)
			SET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);
	}

	return;
}

/* the crosshair points through the held prefab's objects (the hold's with
hook) */
static int forge_prefabs_hold_with(
	long object_index)
{
	short index;

	for (index = 0; forge_prefabs_globals.holding && index < forge_prefabs_globals.object_count; index++)
	{
		if (forge_prefabs_globals.held_objects[index] == object_index)
			return TRUE;
	}

	return FALSE;
}

/* the held prefab's objects are the map's now, kept by the tools' layout
(port/linux/game/forge_layout.c); how many there are */
static short forge_prefabs_keep(
	void)
{
	short kept = 0;
	short index;

	for (index = 0; index < forge_prefabs_globals.object_count; index++)
	{
		long object_index = forge_prefabs_globals.held_objects[index];

		if (object_index != NONE && object_try_and_get(object_index))
		{
			forge_layout_note_spawned(object_index);
			kept++;
		}
	}
	forge_prefabs_globals.holding = FALSE;

	return kept;
}

/* the held prefab's objects are removed again, but for the anchor, which
is the tools' to remove */
static void forge_prefabs_discard(
	long anchor_object_index)
{
	short index;

	for (index = 0; index < forge_prefabs_globals.object_count; index++)
	{
		long object_index = forge_prefabs_globals.held_objects[index];

		if (object_index != NONE && object_index != anchor_object_index && object_try_and_get(object_index))
			object_delete(object_index);
	}
	forge_prefabs_globals.holding = FALSE;

	return;
}

/* the tools let go of the anchor (the hold's ended hook) */
static void forge_prefabs_hold_ended(
	long anchor_object_index,
	int placed)
{
	if (!forge_prefabs_globals.holding)
		return;
	if (placed)
	{
		short kept = forge_prefabs_keep();

		terminal_printf(kept == forge_prefabs_globals.object_count ? global_real_argb_green : global_real_argb_orange,
			"forge_prefabs: placed %s (%d of %d objects)", forge_prefabs_globals.held_name, kept,
			forge_prefabs_globals.object_count);
	}
	else if (game_time_get() < forge_prefabs_globals.held_time)
	{
		/* back at a checkpoint: its objects are the checkpoint's */
		forge_prefabs_globals.holding = FALSE;
	}
	else
	{
		forge_prefabs_discard(anchor_object_index);
	}

	return;
}

static struct halo_forge_object_hold const forge_prefabs_hold =
{
	forge_prefabs_globals.held_name,
	forge_prefabs_hold_moved,
	forge_prefabs_hold_with,
	forge_prefabs_hold_ended
};

/* makes the loaded prefab's objects with its origin at the point, turned as
they were saved, and chooses the anchor: the one nearest the prefab's
middle; how many were made */
static short forge_prefabs_create(
	real_point3d const *origin)
{
	real nearest_distance = 0.f;
	short created = 0;
	short index;

	forge_prefabs_globals.held_anchor = NONE;
	for (index = 0; index < forge_prefabs_globals.object_count; index++)
	{
		struct forge_prefab_object const *prefab_object = &forge_prefabs_globals.objects[index];
		struct object_placement_data data;
		long object_index;

		object_placement_data_new(&data, prefab_object->definition_index, NONE);
		set_real_point3d(&data.position, origin->x + prefab_object->offset.x, origin->y + prefab_object->offset.y,
			origin->z + prefab_object->offset.z);
		data.forward = prefab_object->forward;
		data.up = prefab_object->up;
		object_index = object_new(&data);
		forge_prefabs_globals.held_objects[index] = object_index;
		if (object_index != NONE)
		{
			struct object_datum *object = object_get(object_index);
			real distance = prefab_object->offset.x * prefab_object->offset.x +
				prefab_object->offset.y * prefab_object->offset.y;

			if (prefab_object->scale != 1.f)
				objects_scripting_set_scale(object_index, prefab_object->scale, 0);
			object->object.translational_velocity = *global_zero_vector3d;
			object->object.angular_velocity = *global_zero_vector3d;
			if (forge_prefabs_globals.held_anchor == NONE || distance < nearest_distance)
			{
				forge_prefabs_globals.held_anchor = index;
				nearest_distance = distance;
			}
			created++;
		}
	}
	forge_prefabs_globals.held_time = game_time_get();
	forge_prefabs_globals.holding = created > 0;

	return created;
}

/* TRUE when the layout can keep that many more objects */
static boolean forge_prefabs_budget_allows(
	short count)
{
	short used;
	short maximum;

	forge_layout_object_budget(&used, &maximum);

	return used + count <= maximum;
}

/* makes the chosen prefab's objects at the crosshair and holds them; FALSE,
with a message, when that cannot be */
static boolean forge_prefabs_place(
	void)
{
	short slot = forge_prefabs_globals.chosen_slot;
	boolean held = FALSE;
	float point[3];
	float normal[3];

	if (!halo_mods_authoritative())
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: placing a prefab is the host's");
	}
	else if (!slot)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: no prefab is saved for this map yet");
	}
	else if (forge_prefabs_globals.holding)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: a prefab is held already");
	}
	else if (!forge_point_at_crosshair(point, normal))
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: the camera is outside the map");
	}
	else if (!forge_prefabs_load(slot))
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: %s has no object of this map",
			forge_prefabs_globals.slot_names[slot]);
	}
	else if (!forge_prefabs_budget_allows(forge_prefabs_globals.object_count))
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: the layout cannot keep %d more objects",
			forge_prefabs_globals.object_count);
	}
	else
	{
		real_point3d origin;

		set_real_point3d(&origin, point[0], point[1], point[2]);
		if (forge_prefabs_create(&origin) == 0)
		{
			terminal_printf(global_real_argb_orange, "forge_prefabs: %s could not be created",
				forge_prefabs_globals.held_name);
		}
		else
		{
			long anchor_object_index = forge_prefabs_globals.held_objects[forge_prefabs_globals.held_anchor];

			held = forge_hold_object(anchor_object_index, &forge_prefabs_hold);
			if (!held)
			{
				forge_prefabs_discard(NONE);
				terminal_printf(global_real_argb_orange, "forge_prefabs: the tools hold something already");
			}
		}
	}

	return held;
}

/* ---------- the mod's hooks */

static void forge_prefabs_new_map(
	void)
{
	char const *scenario_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;

	forge_prefabs_globals.selected_count = 0;
	forge_prefabs_globals.holding = FALSE;
	forge_prefabs_globals.object_count = 0;
	forge_prefabs_globals.chosen_slot = 0;
	forge_prefabs_globals.delete_time = NONE;
	forge_prefabs_globals.map_name[0] = 0;
	if (scenario_name)
		forge_map_folder_name(scenario_name, forge_prefabs_globals.map_name, sizeof(forge_prefabs_globals.map_name));
	forge_prefabs_read_slots();

	return;
}

static void forge_prefabs_update(
	void)
{
	boolean select_object = halo_mod_key_pressed(&forge_prefabs_globals.select_key, FORGE_PREFABS_SELECT_KEY, FALSE);

	forge_prefabs_globals.marking = FALSE;
	if (local_player_get_player_index(FORGE_PREFABS_LOCAL_PLAYER_INDEX) == NONE || !forge_mode_on())
		return;
	forge_prefabs_selection_prune();
	forge_prefabs_globals.marking = forge_flying(FORGE_PREFABS_LOCAL_PLAYER_INDEX) &&
		forge_prefabs_globals.selected_count > 0;

	/* the tools' own keys come first while their menu is open or they hold
	something; a cutscene's objects are the cutscene's; on foot the key is
	the game's */
	if (select_object && !forge_busy() && !cinematic_in_progress() &&
		forge_flying(FORGE_PREFABS_LOCAL_PLAYER_INDEX))
	{
		forge_prefabs_selection_toggle(forge_prefabs_marked_object());
	}

	return;
}

static void forge_prefabs_render(
	void)
{
	short x0;
	short y0;
	short x1;
	short y1;

	/* how many are selected, under the name of the object at the crosshair
	(forge_edit) */
	if (forge_prefabs_globals.marking && !forge_busy() && halo_mod_screen(&x0, &y0, &x1, &y1))
	{
		char label[64];
		short center_x = (short)((x0 + x1) / 2);
		short top = (short)((y0 + y1) / 2 + FORGE_PREFABS_LABEL_OFFSET + forge_label_height() +
			3 * FORGE_PREFABS_LABEL_MARGIN);
		short width;

		_snprintf(label, sizeof(label), "%d selected for a prefab", forge_prefabs_globals.selected_count);
		label[sizeof(label) - 1] = 0;
		width = (short)(forge_label_width(label) + 4 * FORGE_PREFABS_LABEL_MARGIN);
		halo_mod_draw_box(
			(short)(center_x - width / 2),
			(short)(top - FORGE_PREFABS_LABEL_MARGIN),
			(short)(center_x + width / 2),
			(short)(top + forge_label_height() + FORGE_PREFABS_LABEL_MARGIN),
			FORGE_PREFABS_LABEL_BACKGROUND_COLOR);
		forge_draw_label(x0, top, x1, HALO_MOD_TEXT_CENTER, FORGE_PREFABS_LABEL_COLOR, label);
	}

	/* drawn only after an update: nothing is left over when the game stops
	updating the mods (the console, menus, loading) */
	forge_prefabs_globals.marking = FALSE;

	return;
}

/* a box about the point */
static void forge_prefabs_draw_box(
	real_point3d const *center,
	real radius,
	real_argb_color const *color)
{
	real_point3d corners[8];
	short corner;

	for (corner = 0; corner < 8; corner++)
	{
		set_real_point3d(&corners[corner],
			center->x + ((corner & 1) ? radius : -radius),
			center->y + ((corner & 2) ? radius : -radius),
			center->z + ((corner & 4) ? radius : -radius));
	}
	for (corner = 0; corner < 8; corner++)
	{
		short axis;

		/* each edge once, from its corner with the axis' bit clear */
		for (axis = 1; axis < 8; axis <<= 1)
		{
			if (!(corner & axis))
				rasterizer_debug_line_shaded(&corners[corner], &corners[corner | axis], color, color);
		}
	}

	return;
}

/* the selection, in each player's view (the world drawing hook) */
static void forge_prefabs_render_world(
	void)
{
	real_argb_color selected_color = { { 1.f, 0.3f, 1.f, 0.3f } };
	short index;

	if (!forge_mode_on() || game_connection() == _game_connection_film_playback ||
		local_player_get_player_index(FORGE_PREFABS_LOCAL_PLAYER_INDEX) == NONE)
	{
		return;
	}

	for (index = 0; index < forge_prefabs_globals.selected_count; index++)
	{
		struct object_datum *object = object_try_and_get(forge_prefabs_globals.selected[index]);

		if (object)
		{
			forge_prefabs_draw_box(&object->object.bounding_sphere_center,
				MAX(object->object.bounding_sphere_radius, FORGE_PREFABS_MINIMUM_RADIUS), &selected_color);
		}
	}

	return;
}

/* ---------- the tools' menu page */

enum
{
	_forge_prefabs_row_target = 0,
	_forge_prefabs_row_select,
	_forge_prefabs_row_clear,
	_forge_prefabs_row_save,
	_forge_prefabs_row_prefab,
	_forge_prefabs_row_place,
	_forge_prefabs_row_delete,
	NUMBER_OF_FORGE_PREFABS_ROWS
};

static boolean forge_prefabs_delete_asked(
	void)
{
	long elapsed = game_time_get() - forge_prefabs_globals.delete_time;

	return forge_prefabs_globals.delete_time != NONE && elapsed >= 0 && elapsed < FORGE_PREFABS_CONFIRM_TICKS;
}

static short forge_prefabs_menu_row_count(
	void)
{
	return NUMBER_OF_FORGE_PREFABS_ROWS;
}

static void forge_prefabs_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	short chosen = forge_prefabs_globals.chosen_slot;

	switch (row)
	{
	case _forge_prefabs_row_target:
	{
		long object_index = forge_prefabs_marked_object();

		_snprintf(label, label_size, "At the crosshair");
		if (object_index == NONE)
		{
			_snprintf(value, value_size, "nothing");
		}
		else
		{
			_snprintf(value, value_size, "%s%s", forge_prefabs_object_name(object_get(object_index)->definition_index),
				forge_prefabs_selection_find(object_index) != NONE ? " (selected)" : "");
		}
		break;
	}
	case _forge_prefabs_row_select:
		_snprintf(label, label_size, "Select it, or leave it out (%s, flying)",
			halo_mod_key_name(FORGE_PREFABS_SELECT_KEY));
		_snprintf(value, value_size, "%d selected", forge_prefabs_globals.selected_count);
		break;
	case _forge_prefabs_row_clear:
		_snprintf(label, label_size, "Clear the selection");
		break;
	case _forge_prefabs_row_save:
		_snprintf(label, label_size, "Save the selection as a prefab");
		_snprintf(value, value_size, "%d of %d saved", forge_prefabs_saved_count(), FORGE_PREFABS_SLOT_COUNT);
		break;
	case _forge_prefabs_row_prefab:
		_snprintf(label, label_size, "Prefab");
		_snprintf(value, value_size, "%s", chosen ? forge_prefabs_globals.slot_names[chosen] : "none saved");
		break;
	case _forge_prefabs_row_place:
		_snprintf(label, label_size, "Place it (its objects, held at the crosshair)");
		break;
	case _forge_prefabs_row_delete:
		_snprintf(label, label_size, "Delete it");
		if (forge_prefabs_delete_asked())
			_snprintf(value, value_size, "choose again to delete");
		break;
	}

	return;
}

/* the name typed for the selection's prefab */
static void forge_prefabs_name_typed(
	char const *text)
{
	char name[FORGE_PREFABS_NAME_SIZE];
	short slot;

	/* (as they are now: another prefab may have been saved or deleted) */
	forge_prefabs_read_slots();
	slot = forge_prefabs_first_free_slot();
	forge_prefabs_clean_text(text, name, sizeof(name));
	if (!name[0])
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: the name is empty, nothing saved");
	}
	else if (!slot)
	{
		terminal_printf(global_real_argb_orange, "forge_prefabs: a map keeps %d prefabs at most", FORGE_PREFABS_SLOT_COUNT);
	}
	else if (forge_prefabs_save(slot, name))
	{
		terminal_printf(global_real_argb_green, "forge_prefabs: saved %s (%d objects)", name,
			forge_prefabs_globals.selected_count);
		forge_prefabs_globals.selected_count = 0;
		forge_prefabs_globals.chosen_slot = slot;
		forge_prefabs_read_slots();
	}

	return;
}

/* the saved prefab before or after the chosen one, around */
static void forge_prefabs_choose(
	int direction)
{
	short slot = forge_prefabs_globals.chosen_slot;
	short step;

	for (step = 0; step < FORGE_PREFABS_SLOT_COUNT; step++)
	{
		slot = (short)(slot + direction);
		if (slot < 1)
			slot = FORGE_PREFABS_SLOT_COUNT;
		else if (slot > FORGE_PREFABS_SLOT_COUNT)
			slot = 1;
		if (forge_prefabs_globals.slot_names[slot][0])
		{
			forge_prefabs_globals.chosen_slot = slot;
			break;
		}
	}

	return;
}

static int forge_prefabs_menu_row_change(
	short row,
	int direction)
{
	int close_menu = FALSE;

	if (row != _forge_prefabs_row_delete || direction != 0)
		forge_prefabs_globals.delete_time = NONE;

	if (row == _forge_prefabs_row_prefab)
	{
		forge_prefabs_choose(direction < 0 ? -1 : 1);
		return FALSE;
	}
	if (direction != 0)
		return FALSE;

	switch (row)
	{
	case _forge_prefabs_row_select:
		/* (closed: the next object can be aimed at) */
		forge_prefabs_selection_toggle(forge_prefabs_marked_object());
		close_menu = TRUE;
		break;
	case _forge_prefabs_row_clear:
		forge_prefabs_globals.selected_count = 0;
		break;
	case _forge_prefabs_row_save:
		forge_prefabs_selection_prune();
		if (forge_prefabs_globals.selected_count == 0)
		{
			terminal_printf(global_real_argb_orange, "forge_prefabs: select the prefab's objects first");
		}
		else if (!forge_prefabs_first_free_slot())
		{
			terminal_printf(global_real_argb_orange, "forge_prefabs: a map keeps %d prefabs at most, delete one first",
				FORGE_PREFABS_SLOT_COUNT);
		}
		else
		{
			char name[FORGE_PREFABS_NAME_SIZE];

			_snprintf(name, sizeof(name), "Prefab %d", forge_prefabs_first_free_slot());
			if (!forge_text_entry_begin("Name of the prefab", name, FORGE_PREFABS_NAME_SIZE - 1,
				forge_prefabs_name_typed))
			{
				terminal_printf(global_real_argb_orange, "forge_prefabs: something is being typed already");
			}
		}
		break;
	case _forge_prefabs_row_place:
		close_menu = forge_prefabs_place();
		break;
	case _forge_prefabs_row_delete:
		if (!forge_prefabs_globals.chosen_slot)
		{
			terminal_printf(global_real_argb_orange, "forge_prefabs: no prefab is saved for this map yet");
		}
		else if (!forge_prefabs_delete_asked())
		{
			forge_prefabs_globals.delete_time = game_time_get();
		}
		else
		{
			char path[FORGE_PREFABS_PATH_SIZE];

			forge_prefabs_slot_path(forge_prefabs_globals.chosen_slot, path, sizeof(path));
			remove(path);
			terminal_printf(global_real_argb_green, "forge_prefabs: deleted %s",
				forge_prefabs_globals.slot_names[forge_prefabs_globals.chosen_slot]);
			forge_prefabs_globals.delete_time = NONE;
			forge_prefabs_read_slots();
		}
		break;
	}

	return close_menu;
}

static void forge_prefabs_menu_opened(
	void)
{
	forge_prefabs_globals.delete_time = NONE;
	/* (the launcher, or another game, may have changed the files) */
	forge_prefabs_read_slots();

	return;
}

static struct halo_mod_menu const forge_prefabs_menu =
{
	"Prefab",
	forge_prefabs_menu_row_count,
	forge_prefabs_menu_row_text,
	forge_prefabs_menu_row_change,
	forge_prefabs_menu_opened
};

/* ---------- for an object's popup (forge_edit.c) */

int forge_prefabs_object_selected(
	long object_index)
{
	return forge_prefabs_selection_find(object_index) != NONE;
}

void forge_prefabs_object_toggle(
	long object_index)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	if (!object || !TEST_FLAG(FORGE_PREFABS_OBJECT_MASK, object->object.type) ||
		object->object.parent_object_index != NONE || forge_prefabs_holds_player(object_index) ||
		forge_object_holds_flag(object_index))
	{
		object_index = NONE;
	}
	forge_prefabs_selection_toggle(object_index);

	return;
}

/* ---------- the mod */

struct halo_mod const forge_prefabs_mod =
{
	"forge_prefabs",
	forge_prefabs_update,
	forge_prefabs_render,
	NULL,
	forge_prefabs_new_map,
	forge_prefabs_render_world,
	&forge_prefabs_menu,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};
