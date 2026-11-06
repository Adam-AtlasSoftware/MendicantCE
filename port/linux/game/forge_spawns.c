/*
FORGE_SPAWNS.C

One of Forge's parts (halo_mod.h, halo_mods.c): the map's spawn points, shown, moved,
removed and added with the dev tools' crosshair (port/linux/game/forge.c).

A map has two kinds, both lists of its scenario and neither an object:

	player spawns   where players start and come back (scenario->players,
	                struct player_starting_location): a place, a facing, a
	                team and the game types it is for. The game rates every
	                one for the player and takes the best
	                (find_best_starting_location_index, game/players.c;
	                game_engine_get_starting_location_rating,
	                game/game_engine.c): none of another game type or with
	                a vehicle on it, in team games none of the other team,
	                less the nearer an enemy is (none within 2 world
	                units), more near teammates, and each by a random part
	item spawns     where weapons, grenades and powerups come back
	                (scenario->netgame_equipment, struct
	                scenario_netgame_equipment): a place, the game types it
	                is for, an item collection and a time. Each makes one
	                item of its collection every time the game's clock
	                passes a multiple of its time (its own, else its
	                collection's, else 30 seconds;
	                game_engine_update_item_spawn, game/game_engine.c)

Everything that reads them reads those two lists, so a kind that is changed
here is the map's list copied to one of this mod's, which the scenario then
has in its place; "Restore the map's own" gives the scenario its own back.
The map's file is never changed.

Everything is in the dev tools' menu (1, D-pad right or, flying, X), on the
"Spawns" tab:

	Show spawns     draw them in the world: a post with an arrow the way it
	                faces, red or blue by team (white for neither), item
	                spawns yellow and shorter. Only in forge mode (flying)
	                or while one is held: never on foot
	Kind            player spawns or item spawns: what the rows below are
	                about
	New spawn       close the menu and hold a new one at the crosshair,
	                like the chosen one in everything but its place
	Spawn           which one the rows below are about (with the spawns
	                shown, aiming at one with the menu closed chooses it,
	                and X on a controller opens the menu here on it)
	Team            (player) red or blue
	Game types      every game, every game but CTF, every game but CTF and
	                race, or one of them
	Items           (item) the item collection
	Respawn time    (item) the collection's own, or seconds
	Pick up and move
	                hold it as the tools hold an object: aim to move it,
	                left/right turn it (V / X: free, or in steps), up/down
	                raise and lower it, enter or A put it down, escape or B
	                put it back, delete, Back or Y remove it. Aiming at one
	                and pressing F4 (or enter / A while flying) picks it up
	                the same way, unless an object is nearer.
	Move here       to the crosshair, turned the way the camera looks
	Remove
	Remove all      of the kind, to place the map's spawns anew
	Restore the map's own

With no player spawn nobody can start: a layout saved then keeps the map's.
The spawns are part of layouts (forge_layout.c) and so reach the clients of
a system link game, where they are the host's to change.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "camera/observer.h"
#include "cseries/cseries_windows.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "items/item_definitions.h"
#include "rasterizer/rasterizer.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	FORGE_SPAWNS_LOCAL_PLAYER_INDEX = 0,
	FORGE_SPAWNS_MAXIMUM_COLLECTIONS = 256,
	/* the game types a spawn is for hold these as well as a game's own
	number (game/game_engine.c) */
	FORGE_SPAWNS_GAME_TYPE_ALL = 12,
	FORGE_SPAWNS_GAME_TYPE_ALL_NON_TEAM,
	FORGE_SPAWNS_GAME_TYPE_ALL_NORMAL,
	NUMBER_OF_FORGE_SPAWNS_GAME_TYPES = 4
};

enum forge_spawn_kind
{
	_forge_spawn_player = 0,
	_forge_spawn_item,
	NUMBER_OF_FORGE_SPAWN_KINDS
};

/* how far the crosshair chooses spawns and how far they are drawn, how
wide a spawn is to the crosshair, and how far from the origin a layout may
put one (world units) */
#define FORGE_SPAWNS_SELECT_RANGE 200.f
#define FORGE_SPAWNS_DRAW_RANGE 120.f
#define FORGE_SPAWNS_SELECT_RADIUS 0.3f
#define FORGE_SPAWNS_WORLD_BOUND 32768.f
/* the most a layout's line may give a spawn: its team's index (a game has
16 teams at most), and how far round it faces (radians) */
#define FORGE_SPAWNS_MAXIMUM_TEAM_INDEX 15
#define FORGE_SPAWNS_FACING_BOUND 64.f
/* world units a second a held spawn rises and sinks, and radians a second
it turns: the tools' own speeds for an object (port/linux/game/forge.c) */
#define FORGE_SPAWNS_HOLD_RAISE_SPEED 2.f
#define FORGE_SPAWNS_HOLD_TURN_SPEED _pi

/* ---------- globals */

static struct player_starting_location forge_spawns_players[MAXIMUM_SCENARIO_PLAYERS_PER_BLOCK];
static struct scenario_netgame_equipment forge_spawns_items[MAXIMUM_SCENARIO_NETGAME_EQUIPMENT_PER_SCENARIO];

static struct
{
	char const *name;
	/* this mod's list of the kind, how many it holds and the size of one */
	void *storage;
	long maximum;
	unsigned long element_size;
	/* a marker's height */
	real height;
} const forge_spawn_kinds[NUMBER_OF_FORGE_SPAWN_KINDS] =
{
	{
		"player spawn",
		forge_spawns_players,
		MAXIMUM_SCENARIO_PLAYERS_PER_BLOCK,
		sizeof(struct player_starting_location),
		0.7f
	},
	{
		"item spawn",
		forge_spawns_items,
		MAXIMUM_SCENARIO_NETGAME_EQUIPMENT_PER_SCENARIO,
		sizeof(struct scenario_netgame_equipment),
		0.35f
	}
};

/* what the Game types row goes through, and every game type's name */
static short const forge_spawns_game_type_choices[] =
{
	FORGE_SPAWNS_GAME_TYPE_ALL,
	FORGE_SPAWNS_GAME_TYPE_ALL_NON_TEAM,
	FORGE_SPAWNS_GAME_TYPE_ALL_NORMAL,
	game_engine_ctf,
	game_engine_slayer,
	game_engine_oddball,
	game_engine_king,
	game_engine_race
};

/* an item spawn's time, seconds; 0 its collection's own */
static short const forge_spawns_times[] = { 0, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180 };

static struct
{
	/* the "Spawns" tab */
	boolean showing;
	short kind;
	long selected[NUMBER_OF_FORGE_SPAWN_KINDS];

	/* a kind whose list in the scenario is this mod's, and the map's own
	(the block's count and address) to give back */
	boolean replaced[NUMBER_OF_FORGE_SPAWN_KINDS];
	long map_count[NUMBER_OF_FORGE_SPAWN_KINDS];
	void *map_address[NUMBER_OF_FORGE_SPAWN_KINDS];
} forge_spawns_globals = { TRUE, _forge_spawn_player, { NONE, NONE } };

/* ---------- private code */

static struct tag_block *forge_spawns_block(
	short kind)
{
	struct scenario *scenario = global_scenario_get();

	return kind == _forge_spawn_player ? &scenario->players : &scenario->netgame_equipment;
}

static long forge_spawns_count(
	short kind)
{
	return forge_spawns_block(kind)->count;
}

static struct player_starting_location *forge_spawns_player(
	long index)
{
	return TAG_BLOCK_GET_ELEMENT(forge_spawns_block(_forge_spawn_player), index, struct player_starting_location);
}

static struct scenario_netgame_equipment *forge_spawns_item(
	long index)
{
	return TAG_BLOCK_GET_ELEMENT(forge_spawns_block(_forge_spawn_item), index, struct scenario_netgame_equipment);
}

static real_point3d *forge_spawns_position(
	short kind,
	long index)
{
	return kind == _forge_spawn_player ? &forge_spawns_player(index)->position : &forge_spawns_item(index)->position;
}

static real *forge_spawns_facing(
	short kind,
	long index)
{
	return kind == _forge_spawn_player ? &forge_spawns_player(index)->facing : &forge_spawns_item(index)->facing;
}

static short *forge_spawns_game_types(
	short kind,
	long index)
{
	return kind == _forge_spawn_player ? forge_spawns_player(index)->game_types : forge_spawns_item(index)->game_type;
}

static boolean forge_spawns_has_selection(
	short kind)
{
	return forge_spawns_globals.selected[kind] >= 0 &&
		forge_spawns_globals.selected[kind] < forge_spawns_count(kind);
}

/* the kind's list in the scenario becomes this mod's, a copy of the map's */
static void forge_spawns_take_over(
	short kind)
{
	struct tag_block *block = forge_spawns_block(kind);

	if (!forge_spawns_globals.replaced[kind])
	{
		long count = PIN(block->count, 0, forge_spawn_kinds[kind].maximum);

		if (!block->address)
			count = 0;
		if (count > 0)
			memcpy(forge_spawn_kinds[kind].storage, block->address, count * forge_spawn_kinds[kind].element_size);
		forge_spawns_globals.map_count[kind] = block->count;
		forge_spawns_globals.map_address[kind] = block->address;
		block->count = count;
		block->address = forge_spawn_kinds[kind].storage;
		forge_spawns_globals.replaced[kind] = TRUE;
	}

	return;
}

/* the scenario has the map's own list of the kind again (a scenario loaded
since has it already) */
static void forge_spawns_restore(
	short kind)
{
	struct tag_block *block = forge_spawns_block(kind);

	if (forge_spawns_globals.replaced[kind] && block->address == forge_spawn_kinds[kind].storage)
	{
		block->count = forge_spawns_globals.map_count[kind];
		block->address = forge_spawns_globals.map_address[kind];
	}
	forge_spawns_globals.replaced[kind] = FALSE;
	forge_spawns_globals.selected[kind] = NONE;

	return;
}

/* a new, empty spawn at the end of the kind's list, or NONE when it is full */
static long forge_spawns_append(
	short kind)
{
	struct tag_block *block = forge_spawns_block(kind);

	forge_spawns_take_over(kind);
	if (block->count >= forge_spawn_kinds[kind].maximum)
		return NONE;
	memset((char *)block->address + block->count * forge_spawn_kinds[kind].element_size, 0,
		forge_spawn_kinds[kind].element_size);

	return block->count++;
}

static void forge_spawns_remove(
	short kind,
	long index)
{
	struct tag_block *block = forge_spawns_block(kind);
	unsigned long element_size = forge_spawn_kinds[kind].element_size;

	forge_spawns_take_over(kind);
	if (index < 0 || index >= block->count)
		return;
	memmove((char *)block->address + index * element_size, (char *)block->address + (index + 1) * element_size,
		(block->count - index - 1) * element_size);
	block->count--;
	forge_spawns_globals.selected[kind] = MIN(index, block->count - 1);
	terminal_printf(global_real_argb_green, "forge_spawns: %s removed", forge_spawn_kinds[kind].name);
	if (kind == _forge_spawn_player && block->count == 0)
		terminal_printf(global_real_argb_orange, "forge_spawns: nobody can start until a player spawn is placed");

	return;
}

/* every spawn of the kind */
static void forge_spawns_remove_all(
	short kind)
{
	forge_spawns_take_over(kind);
	forge_spawns_block(kind)->count = 0;
	forge_spawns_globals.selected[kind] = NONE;

	return;
}

static boolean forge_spawns_editable(
	void)
{
	/* a local game, or the host of a system link game, whose layout takes
	the spawns to its clients (forge_layout.c) */
	boolean editable = halo_mods_authoritative();

	if (!editable)
		terminal_printf(global_real_argb_orange, "forge_spawns: the spawns are the host's");

	return editable;
}

static char const *forge_spawns_game_type_name(
	short game_type)
{
	switch (game_type)
	{
	case game_engine_ctf: return "CTF";
	case game_engine_slayer: return "slayer";
	case game_engine_oddball: return "oddball";
	case game_engine_king: return "king";
	case game_engine_race: return "race";
	case FORGE_SPAWNS_GAME_TYPE_ALL: return "every game";
	case FORGE_SPAWNS_GAME_TYPE_ALL_NON_TEAM: return "all but CTF";
	case FORGE_SPAWNS_GAME_TYPE_ALL_NORMAL: return "all but CTF and race";
	}

	return "?";
}

/* the game types the spawn is for, as the map names up to four */
static void forge_spawns_describe_game_types(
	short const *game_types,
	char *text,
	unsigned long size)
{
	unsigned long length = 0;
	short index;

	text[0] = 0;
	for (index = 0; index < NUMBER_OF_FORGE_SPAWNS_GAME_TYPES && length < size - 1; index++)
	{
		if (game_types[index] != game_engine_none)
		{
			_snprintf(text + length, size - length, "%s%s", length ? ", " : "",
				forge_spawns_game_type_name(game_types[index]));
			text[size - 1] = 0;
			length = (unsigned long)strlen(text);
		}
	}
	if (!length)
		_snprintf(text, size, "none");
	text[size - 1] = 0;

	return;
}

/* the last part of the collection's name, or "none" */
static char const *forge_spawns_collection_name(
	long collection_index)
{
	char const *name = collection_index != NONE ? tag_get_name(collection_index) : NULL;
	char const *last;

	if (!name)
		return "none";
	last = strrchr(name, '\\');

	return last ? last + 1 : name;
}

/* the map's item collection before or after this one (direction -1 or 1),
the first for none of them; NONE when the map has none */
static long forge_spawns_next_collection(
	long collection_index,
	int direction)
{
	long collections[FORGE_SPAWNS_MAXIMUM_COLLECTIONS];
	struct tag_iterator iterator;
	long tag_index;
	short count = 0;
	short index;

	tag_iterator_new(&iterator, ITEM_COLLECTION_DEFINITION_TAG);
	while (count < FORGE_SPAWNS_MAXIMUM_COLLECTIONS && (tag_index = tag_iterator_next(&iterator)) != NONE)
		collections[count++] = tag_index;
	if (!count)
		return NONE;
	for (index = 0; index < count; index++)
	{
		if (collections[index] == collection_index)
			return collections[(index + count + (direction < 0 ? -1 : 1)) % count];
	}

	return collections[0];
}

/* an item of the spawn's collection where it is, as the game makes one
there each time its clock comes round (game_engine_update_item_spawn) */
static void forge_spawns_make_item(
	struct scenario_netgame_equipment const *item)
{
	long definition_index = game_engine_random_item(item->item_collection.index);

	if (definition_index != NONE)
	{
		struct object_placement_data data;
		long object_index;

		object_placement_data_new(&data, definition_index, NONE);
		data.position = item->position;
		object_index = object_new(&data);
		if (object_index != NONE)
			object_set_garbage(object_index, FALSE);
	}

	return;
}

/* whether the spawns are drawn and can be aimed at: shown, and only in forge
mode (the tools' flying camera) or while the tools hold something or have
their menu open; never on foot, so a game can be played with them shown */
static boolean forge_spawns_visible(
	void)
{
	return forge_spawns_globals.showing && forge_mode_on() &&
		(forge_flying(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) || forge_busy());
}

/* the spawn the crosshair points at, nearest first; FALSE for none */
static boolean forge_spawns_aimed(
	short *aimed_kind,
	long *aimed_index,
	real *aimed_distance)
{
	struct observer_result const *camera = observer_get_camera(FORGE_SPAWNS_LOCAL_PLAYER_INDEX);
	real nearest_distance = FORGE_SPAWNS_SELECT_RANGE;
	boolean aimed = FALSE;
	short kind;

	for (kind = 0; kind < NUMBER_OF_FORGE_SPAWN_KINDS; kind++)
	{
		long count = forge_spawns_count(kind);
		long index;

		for (index = 0; index < count; index++)
		{
			real_point3d middle = *forge_spawns_position(kind, index);
			real_vector3d to_middle;
			real along;

			middle.z += forge_spawn_kinds[kind].height / 2.f;
			vector_from_points3d(&camera->position, &middle, &to_middle);
			along = dot_product3d(&to_middle, &camera->forward);
			if (along >= 0.f && along <= nearest_distance &&
				dot_product3d(&to_middle, &to_middle) - along * along <=
					FORGE_SPAWNS_SELECT_RADIUS * FORGE_SPAWNS_SELECT_RADIUS)
			{
				nearest_distance = along;
				*aimed_kind = kind;
				*aimed_index = index;
				aimed = TRUE;
			}
		}
	}
	if (aimed_distance)
		*aimed_distance = nearest_distance;

	return aimed;
}

/* the spawn the crosshair points at is the one the tab is about */
static void forge_spawns_select_aimed(
	void)
{
	short kind;
	long index;

	if (forge_spawns_aimed(&kind, &index, NULL))
	{
		forge_spawns_globals.kind = kind;
		forge_spawns_globals.selected[kind] = index;
	}

	return;
}

static void forge_spawns_update(
	void)
{
	/* with the spawns shown, aiming at one chooses it, unless the tools'
	menu is in use (its rows say which spawn) or a cutscene plays */
	if (local_player_get_player_index(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) != NONE &&
		game_connection() != _game_connection_film_playback &&
		forge_spawns_visible() && !forge_busy() && !cinematic_in_progress())
	{
		forge_spawns_select_aimed();
	}

	return;
}

/* a post where the spawn is, an arrow the way it faces from the middle of
the post and a wedge on the ground under it; the chosen one has a square on
top */
static void forge_spawns_draw(
	short kind,
	long index,
	boolean selected)
{
	real_point3d const *position = forge_spawns_position(kind, index);
	real height = forge_spawn_kinds[kind].height;
	real_argb_color color = { { 1.f, 1.f, 0.9f, 0.2f } };
	real_argb_color wedge_color;
	real_vector3d forward;
	real_point3d top = *position;
	real_point3d middle = *position;
	real_point3d tip;
	real_point3d points[4];
	short point_index;

	if (kind == _forge_spawn_player)
	{
		short team_index = forge_spawns_player(index)->team_index;

		color.red = team_index == _team_red ? 1.f : team_index == _team_blue ? 0.3f : 1.f;
		color.green = team_index == _team_red ? 0.25f : team_index == _team_blue ? 0.5f : 1.f;
		color.blue = team_index == _team_red ? 0.2f : 1.f;
	}
	wedge_color = color;
	wedge_color.alpha = selected ? 0.5f : 0.25f;
	/* (the lines opaque, dimmer when not chosen: the game draws opaque debug
	lines together, and each see-through one by itself) */
	color.alpha = 1.f;
	if (!selected)
	{
		color.red *= 0.6f;
		color.green *= 0.6f;
		color.blue *= 0.6f;
	}

	vector3d_from_angle(&forward, *forge_spawns_facing(kind, index));
	top.z += height;
	middle.z += height / 2.f;
	rasterizer_debug_line_shaded(position, &top, &color, &color);

	/* (the arrow's head: left and right of the way it faces) */
	set_real_point3d(&tip, middle.x + forward.i * height * 0.7f, middle.y + forward.j * height * 0.7f, middle.z);
	set_real_point3d(&points[0], middle.x + (forward.i * 0.5f - forward.j * 0.12f) * height,
		middle.y + (forward.j * 0.5f + forward.i * 0.12f) * height, middle.z);
	set_real_point3d(&points[1], middle.x + (forward.i * 0.5f + forward.j * 0.12f) * height,
		middle.y + (forward.j * 0.5f - forward.i * 0.12f) * height, middle.z);
	rasterizer_debug_line_shaded(&middle, &tip, &color, &color);
	rasterizer_debug_line_shaded(&tip, &points[0], &color, &color);
	rasterizer_debug_line_shaded(&tip, &points[1], &color, &color);

	set_real_point3d(&tip, position->x + forward.i * height * 0.6f, position->y + forward.j * height * 0.6f,
		position->z + 0.02f);
	set_real_point3d(&points[0], position->x - forward.j * height * 0.25f, position->y + forward.i * height * 0.25f,
		position->z + 0.02f);
	set_real_point3d(&points[1], position->x + forward.j * height * 0.25f, position->y - forward.i * height * 0.25f,
		position->z + 0.02f);
	rasterizer_debug_triangle(&points[0], &points[1], &tip, &wedge_color);

	if (selected)
	{
		real half = height * 0.2f;

		set_real_point3d(&points[0], top.x - half, top.y - half, top.z);
		set_real_point3d(&points[1], top.x + half, top.y - half, top.z);
		set_real_point3d(&points[2], top.x + half, top.y + half, top.z);
		set_real_point3d(&points[3], top.x - half, top.y + half, top.z);
		for (point_index = 0; point_index < 4; point_index++)
			rasterizer_debug_line_shaded(&points[point_index], &points[(point_index + 1) % 4], &color, &color);
	}

	return;
}

/* the spawns, in each player's view (the world drawing hook): the game
draws the queued debug geometry at the end of the view's window
(rasterizer_debug_draw) */
static void forge_spawns_render_world(
	void)
{
	struct observer_result const *camera;
	short kind;

	if (!forge_spawns_visible() || game_connection() == _game_connection_film_playback ||
		local_player_get_player_index(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) == NONE)
	{
		return;
	}

	camera = observer_get_camera(FORGE_SPAWNS_LOCAL_PLAYER_INDEX);
	for (kind = 0; kind < NUMBER_OF_FORGE_SPAWN_KINDS; kind++)
	{
		long count = forge_spawns_count(kind);
		long index;

		for (index = 0; index < count; index++)
		{
			if (distance3d(&camera->position, forge_spawns_position(kind, index)) <= FORGE_SPAWNS_DRAW_RANGE)
			{
				forge_spawns_draw(kind, index,
					kind == forge_spawns_globals.kind && index == forge_spawns_globals.selected[kind]);
			}
		}
	}

	return;
}

static void forge_spawns_new_map(
	void)
{
	short kind;

	for (kind = 0; kind < NUMBER_OF_FORGE_SPAWN_KINDS; kind++)
		forge_spawns_restore(kind);

	return;
}

/* ---------- picking a spawn up */

/* as the tools hold an object (port/linux/game/forge.c): how it turns in
steps (V / X): free, then these degrees */
static real const forge_spawns_hold_snaps[] = { 0.f, 15.f, 45.f, 90.f };
static char const *const forge_spawns_hold_snap_names[] = { "free", "15 degrees", "45 degrees", "90 degrees" };

static struct
{
	short kind;
	long index;
	union
	{
		struct player_starting_location player;
		struct scenario_netgame_equipment item;
	} original;
	real height;
	short snap_index;
	/* a new spawn, not yet put down: cancelling removes it */
	boolean spawned;
} forge_spawns_hold_globals = { 0, NONE };

static void forge_spawns_hold_describe(
	char *line,
	unsigned long size)
{
	_snprintf(line, size, "step: %s, raised %.2f",
		forge_spawns_hold_snap_names[forge_spawns_hold_globals.snap_index], forge_spawns_hold_globals.height);

	return;
}

static int forge_spawns_hold_update(
	struct halo_forge_hold_input const *input)
{
	short kind = forge_spawns_hold_globals.kind;
	long index = forge_spawns_hold_globals.index;
	real *facing;
	float point[3];
	float normal[3];

	/* the spawn is gone (a new map, a layout, the map's own restored):
	nothing to hold */
	if (!forge_spawns_globals.replaced[kind] || index < 0 || index >= forge_spawns_count(kind))
		return TRUE;

	if (input && (input->delete_pressed || (input->cancel && forge_spawns_hold_globals.spawned)))
	{
		/* (a new one as a spawned object: gone when it is not put down) */
		forge_spawns_remove(kind, index);
		return TRUE;
	}
	if (!input || input->cancel)
	{
		memcpy((char *)forge_spawn_kinds[kind].storage + index * forge_spawn_kinds[kind].element_size,
			&forge_spawns_hold_globals.original, forge_spawn_kinds[kind].element_size);
		return TRUE;
	}
	if (input->place)
	{
		/* a new item spawn has its item now, not when its time comes round */
		if (kind == _forge_spawn_item && forge_spawns_hold_globals.spawned)
			forge_spawns_make_item(forge_spawns_item(index));
		return TRUE;
	}

	if (input->snap_pressed)
	{
		forge_spawns_hold_globals.snap_index = (short)((forge_spawns_hold_globals.snap_index + 1) %
			(sizeof(forge_spawns_hold_snaps) / sizeof(forge_spawns_hold_snaps[0])));
	}
	facing = forge_spawns_facing(kind, index);
	if (forge_spawns_hold_snaps[forge_spawns_hold_globals.snap_index] == 0.f)
	{
		/* half a circle a second, left is anticlockwise */
		if (input->left != input->right)
			*facing += (input->left ? 1.f : -1.f) * input->seconds * FORGE_SPAWNS_HOLD_TURN_SPEED;
	}
	else if (input->left_pressed != input->right_pressed)
	{
		/* to the next whole step, as an object */
		real step = DEGREES_TO_RADIANS(forge_spawns_hold_snaps[forge_spawns_hold_globals.snap_index]);

		*facing = (real)floor(*facing / step + 0.5f) * step + (input->left_pressed ? step : -step);
	}
	forge_spawns_hold_globals.height += ((input->up ? 1.f : 0.f) - (input->down ? 1.f : 0.f)) * input->seconds *
		FORGE_SPAWNS_HOLD_RAISE_SPEED;

	/* it stands where the crosshair points, raised as asked */
	if (forge_point_at_crosshair(point, normal))
	{
		set_real_point3d(forge_spawns_position(kind, index), point[0], point[1],
			point[2] + forge_spawns_hold_globals.height);
	}

	return FALSE;
}

static struct halo_forge_hold const forge_spawns_hold =
{
	"a spawn",
	forge_spawns_hold_describe,
	forge_spawns_hold_update
};

/* holds the spawn (spawned: a new one, gone when it is not put down); FALSE
when the tools cannot take it now */
static boolean forge_spawns_pick_up(
	short kind,
	long index,
	boolean spawned)
{
	boolean picked_up = FALSE;

	if (forge_spawns_editable())
	{
		forge_spawns_take_over(kind);
		if (index >= 0 && index < forge_spawns_count(kind))
		{
			forge_spawns_hold_globals.kind = kind;
			forge_spawns_hold_globals.index = index;
			memcpy(&forge_spawns_hold_globals.original,
				(char *)forge_spawn_kinds[kind].storage + index * forge_spawn_kinds[kind].element_size,
				forge_spawn_kinds[kind].element_size);
			forge_spawns_hold_globals.height = 0.f;
			forge_spawns_hold_globals.spawned = spawned;
			picked_up = forge_mod_hold_begin(&forge_spawns_hold);
			if (picked_up)
			{
				forge_spawns_globals.kind = kind;
				forge_spawns_globals.selected[kind] = index;
				forge_spawns_globals.showing = TRUE;
			}
		}
	}

	return picked_up;
}

/* the spawn stands on the crosshair's surface, turned the way the camera
looks; FALSE when the camera is outside the map */
static boolean forge_spawns_place_at_crosshair(
	short kind,
	long index)
{
	struct observer_result const *camera = observer_get_camera(FORGE_SPAWNS_LOCAL_PLAYER_INDEX);
	float point[3];
	float normal[3];

	if (!forge_point_at_crosshair(point, normal))
		return FALSE;
	set_real_point3d(forge_spawns_position(kind, index), point[0], point[1], point[2]);
	*forge_spawns_facing(kind, index) = arctangent(camera->forward.j, camera->forward.i);

	return TRUE;
}

/* a new spawn of the tab's kind at the crosshair, like the chosen one in
everything but its place, held as a spawned object is until it is put down */
static void forge_spawns_new(
	void)
{
	short kind = forge_spawns_globals.kind;
	struct player_starting_location player;
	struct scenario_netgame_equipment item;
	float point[3];
	float normal[3];
	long index;

	memset(&player, 0, sizeof(player));
	memset(&item, 0, sizeof(item));
	player.game_types[0] = FORGE_SPAWNS_GAME_TYPE_ALL;
	item.game_type[0] = FORGE_SPAWNS_GAME_TYPE_ALL;
	item.item_collection.index = NONE;
	if (forge_spawns_has_selection(kind))
	{
		if (kind == _forge_spawn_player)
			player = *forge_spawns_player(forge_spawns_globals.selected[kind]);
		else
			item = *forge_spawns_item(forge_spawns_globals.selected[kind]);
	}
	item.run_time_spawned_item_index = NONE;
	if (item.item_collection.index == NONE)
	{
		item.item_collection.group_tag = ITEM_COLLECTION_DEFINITION_TAG;
		item.item_collection.index = forge_spawns_next_collection(NONE, 1);
	}

	if (kind == _forge_spawn_item && item.item_collection.index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_spawns: this map has no item collections");
	}
	else if (!forge_point_at_crosshair(point, normal))
	{
		terminal_printf(global_real_argb_orange, "forge_spawns: the camera is outside the map");
	}
	else if ((index = forge_spawns_append(kind)) == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_spawns: at most %d %ss", (int)forge_spawn_kinds[kind].maximum,
			forge_spawn_kinds[kind].name);
	}
	else
	{
		if (kind == _forge_spawn_player)
			*forge_spawns_player(index) = player;
		else
			*forge_spawns_item(index) = item;
		forge_spawns_place_at_crosshair(kind, index);
		forge_spawns_globals.selected[kind] = index;
		forge_spawns_globals.showing = TRUE;
		terminal_printf(global_real_argb_green, "forge_spawns: new %s", forge_spawn_kinds[kind].name);
		forge_spawns_pick_up(kind, index, TRUE);
	}

	return;
}

/* the tools' pick up: a spawn under the crosshair, when no object is nearer */
static int forge_spawns_grab(
	void)
{
	real spawn_distance;
	long object_index;
	short kind;
	long index;

	if (!forge_spawns_visible() || !halo_mods_authoritative() ||
		local_player_get_player_index(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) == NONE ||
		!forge_spawns_aimed(&kind, &index, &spawn_distance))
	{
		return FALSE;
	}

	object_index = forge_object_at_crosshair();
	if (object_index != NONE && object_try_and_get(object_index))
	{
		struct observer_result const *camera = observer_get_camera(FORGE_SPAWNS_LOCAL_PLAYER_INDEX);
		real_vector3d to_object;

		vector_from_points3d(&camera->position, &object_get(object_index)->object.bounding_sphere_center, &to_object);
		if (magnitude3d(&to_object) < spawn_distance)
			return FALSE;
	}

	return forge_spawns_pick_up(kind, index, FALSE);
}

/* X on a spawn: the tools' menu opens on the tab, which chooses it */
static int forge_spawns_properties(
	float *distance)
{
	short kind;
	long index;

	return forge_spawns_visible() && local_player_get_player_index(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) != NONE &&
		forge_spawns_aimed(&kind, &index, distance);
}

/* ---------- the tools' menu page */

/* the rows, in order; a spawn's own rows come only with a spawn chosen */
enum forge_spawns_row
{
	_forge_spawns_row_show = 0,
	_forge_spawns_row_kind,
	_forge_spawns_row_new,
	_forge_spawns_row_spawn,
	_forge_spawns_row_team,
	_forge_spawns_row_game_types,
	_forge_spawns_row_collection,
	_forge_spawns_row_time,
	_forge_spawns_row_pick_up,
	_forge_spawns_row_move,
	_forge_spawns_row_remove,
	_forge_spawns_row_remove_all,
	_forge_spawns_row_remove_every_kind,
	_forge_spawns_row_restore,
	NUMBER_OF_FORGE_SPAWNS_ROWS
};

/* the row's meaning at this place in the menu; NONE past the last row */
static short forge_spawns_row_kind(
	short row)
{
	short kind = forge_spawns_globals.kind;
	short count = 0;
	short id;

	for (id = 0; id < NUMBER_OF_FORGE_SPAWNS_ROWS; id++)
	{
		if ((id >= _forge_spawns_row_team && id <= _forge_spawns_row_remove && !forge_spawns_has_selection(kind)) ||
			(id == _forge_spawns_row_team && kind != _forge_spawn_player) ||
			((id == _forge_spawns_row_collection || id == _forge_spawns_row_time) && kind != _forge_spawn_item) ||
			(id == _forge_spawns_row_remove_all && forge_spawns_count(kind) == 0) ||
			(id == _forge_spawns_row_remove_every_kind &&
				forge_spawns_count(_forge_spawn_player) + forge_spawns_count(_forge_spawn_item) == 0) ||
			(id == _forge_spawns_row_restore && !forge_spawns_globals.replaced[kind]))
		{
			continue;
		}
		if (count++ == row)
			return id;
	}

	return NONE;
}

static short forge_spawns_menu_row_count(
	void)
{
	short count = 0;

	while (forge_spawns_row_kind(count) != NONE)
		count++;

	return count;
}

static void forge_spawns_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	short kind = forge_spawns_globals.kind;
	long selected = forge_spawns_globals.selected[kind];

	switch (forge_spawns_row_kind(row))
	{
	case _forge_spawns_row_show:
		_snprintf(label, label_size, "Show spawns");
		_snprintf(value, value_size, "%s", forge_spawns_globals.showing ? "on" : "off");
		break;
	case _forge_spawns_row_kind:
		_snprintf(label, label_size, "Kind");
		_snprintf(value, value_size, "%ss (%s)", forge_spawn_kinds[kind].name,
			forge_spawns_globals.replaced[kind] ? "changed" : "the map's own");
		break;
	case _forge_spawns_row_new:
		_snprintf(label, label_size, "New %s (place at the crosshair)", forge_spawn_kinds[kind].name);
		break;
	case _forge_spawns_row_spawn:
		_snprintf(label, label_size, "Spawn");
		if (forge_spawns_has_selection(kind))
			_snprintf(value, value_size, "%d of %d", (int)selected + 1, (int)forge_spawns_count(kind));
		else
			_snprintf(value, value_size, "%s", forge_spawns_count(kind) ? "aim at one" : "none");
		break;
	case _forge_spawns_row_team:
	{
		short team_index = forge_spawns_player(selected)->team_index;

		_snprintf(label, label_size, "Team");
		if (team_index == _team_red || team_index == _team_blue)
			_snprintf(value, value_size, "%s", team_index == _team_red ? "red" : "blue");
		else
			_snprintf(value, value_size, "team %d", team_index);
		break;
	}
	case _forge_spawns_row_game_types:
		_snprintf(label, label_size, "Game types");
		forge_spawns_describe_game_types(forge_spawns_game_types(kind, selected), value, value_size);
		break;
	case _forge_spawns_row_collection:
		_snprintf(label, label_size, "Items");
		_snprintf(value, value_size, "%s",
			forge_spawns_collection_name(forge_spawns_item(selected)->item_collection.index));
		break;
	case _forge_spawns_row_time:
		_snprintf(label, label_size, "Respawn time");
		if (forge_spawns_item(selected)->spawn_time)
			_snprintf(value, value_size, "%d s", forge_spawns_item(selected)->spawn_time);
		else
			_snprintf(value, value_size, "the items' own");
		break;
	case _forge_spawns_row_pick_up:
		_snprintf(label, label_size, "Pick up and move (aim, turn, raise)");
		break;
	case _forge_spawns_row_move:
		_snprintf(label, label_size, "Move here (to the crosshair)");
		break;
	case _forge_spawns_row_remove:
		_snprintf(label, label_size, "Remove");
		break;
	case _forge_spawns_row_remove_all:
		_snprintf(label, label_size, "Remove all %ss", forge_spawn_kinds[kind].name);
		break;
	case _forge_spawns_row_remove_every_kind:
		_snprintf(label, label_size, "Remove all spawns (player and item)");
		break;
	case _forge_spawns_row_restore:
		_snprintf(label, label_size, "Restore the map's own");
		break;
	}

	return;
}

/* (the value before or after this one of count values; the first for one
that is none of them) */
static short forge_spawns_next_choice(
	short const *choices,
	short count,
	short value,
	int direction)
{
	short index;

	for (index = 0; index < count; index++)
	{
		if (choices[index] == value)
			return choices[(index + count + (direction < 0 ? -1 : 1)) % count];
	}

	return choices[0];
}

static int forge_spawns_menu_row_change(
	short row,
	int direction)
{
	short kind = forge_spawns_globals.kind;
	long selected = forge_spawns_globals.selected[kind];
	short id = forge_spawns_row_kind(row);
	int close_menu = FALSE;

	switch (id)
	{
	case _forge_spawns_row_show:
		forge_spawns_globals.showing = !forge_spawns_globals.showing;
		break;
	case _forge_spawns_row_kind:
		if (direction != 0)
			forge_spawns_globals.kind = (short)((kind + 1) % NUMBER_OF_FORGE_SPAWN_KINDS);
		break;
	case _forge_spawns_row_new:
		if (direction == 0 && forge_spawns_editable())
		{
			forge_spawns_new();
			close_menu = TRUE;
		}
		break;
	case _forge_spawns_row_spawn:
		if (direction != 0 && forge_spawns_count(kind) > 0)
		{
			long count = forge_spawns_count(kind);

			forge_spawns_globals.selected[kind] = forge_spawns_has_selection(kind)
				? (selected + count + (direction < 0 ? -1 : 1)) % count
				: 0;
			forge_spawns_globals.showing = TRUE;
		}
		break;
	case _forge_spawns_row_team:
	case _forge_spawns_row_game_types:
	case _forge_spawns_row_collection:
	case _forge_spawns_row_time:
		if (direction != 0 && forge_spawns_editable())
		{
			forge_spawns_take_over(kind);
			if (id == _forge_spawns_row_team)
			{
				struct player_starting_location *player = forge_spawns_player(selected);

				player->team_index = player->team_index == _team_red ? _team_blue : _team_red;
			}
			else if (id == _forge_spawns_row_game_types)
			{
				short *game_types = forge_spawns_game_types(kind, selected);
				/* (one of the row's choices only when it is all the spawn is for) */
				short game_type = game_types[1] || game_types[2] || game_types[3] ? NONE : game_types[0];

				memset(game_types, 0, NUMBER_OF_FORGE_SPAWNS_GAME_TYPES * sizeof(short));
				game_types[0] = forge_spawns_next_choice(forge_spawns_game_type_choices,
					(short)(sizeof(forge_spawns_game_type_choices) / sizeof(forge_spawns_game_type_choices[0])),
					game_type, direction);
			}
			else if (id == _forge_spawns_row_collection)
			{
				struct scenario_netgame_equipment *item = forge_spawns_item(selected);

				item->item_collection.group_tag = ITEM_COLLECTION_DEFINITION_TAG;
				item->item_collection.index = forge_spawns_next_collection(item->item_collection.index, direction);
			}
			else
			{
				struct scenario_netgame_equipment *item = forge_spawns_item(selected);

				item->spawn_time = forge_spawns_next_choice(forge_spawns_times,
					(short)(sizeof(forge_spawns_times) / sizeof(forge_spawns_times[0])), item->spawn_time, direction);
			}
		}
		break;
	case _forge_spawns_row_pick_up:
		if (direction == 0 && forge_spawns_pick_up(kind, selected, FALSE))
			close_menu = TRUE;
		break;
	case _forge_spawns_row_move:
		if (direction == 0 && forge_spawns_editable())
		{
			forge_spawns_take_over(kind);
			if (forge_spawns_place_at_crosshair(kind, selected))
				close_menu = TRUE;
			else
				terminal_printf(global_real_argb_orange, "forge_spawns: the camera is outside the map");
		}
		break;
	case _forge_spawns_row_remove:
		if (direction == 0 && forge_spawns_editable())
			forge_spawns_remove(kind, selected);
		break;
	case _forge_spawns_row_remove_all:
		if (direction == 0 && forge_spawns_editable())
		{
			forge_spawns_remove_all(kind);
			terminal_printf(global_real_argb_green, "forge_spawns: every %s removed", forge_spawn_kinds[kind].name);
			if (kind == _forge_spawn_player)
			{
				terminal_printf(global_real_argb_orange,
					"forge_spawns: nobody can start until a player spawn is placed");
			}
		}
		break;
	case _forge_spawns_row_remove_every_kind:
		if (direction == 0 && forge_spawns_editable())
		{
			short every_kind;

			for (every_kind = 0; every_kind < NUMBER_OF_FORGE_SPAWN_KINDS; every_kind++)
				forge_spawns_remove_all(every_kind);
			terminal_printf(global_real_argb_green, "forge_spawns: every spawn removed");
			terminal_printf(global_real_argb_orange, "forge_spawns: nobody can start until a player spawn is placed");
		}
		break;
	case _forge_spawns_row_restore:
		if (direction == 0 && forge_spawns_editable())
		{
			forge_spawns_restore(kind);
			terminal_printf(global_real_argb_green, "forge_spawns: the map's own %ss are back",
				forge_spawn_kinds[kind].name);
		}
		break;
	}

	return close_menu;
}

/* the tab is shown: the spawn the crosshair points at is the one to edit */
static void forge_spawns_menu_opened(
	void)
{
	if (local_player_get_player_index(FORGE_SPAWNS_LOCAL_PLAYER_INDEX) != NONE && forge_spawns_globals.showing)
		forge_spawns_select_aimed();

	return;
}

static struct halo_mod_menu const forge_spawns_menu =
{
	"Spawns",
	forge_spawns_menu_row_count,
	forge_spawns_menu_row_text,
	forge_spawns_menu_row_change,
	forge_spawns_menu_opened
};

/* ---------- layouts (port/linux/game/forge_layout.c) */

/* a changed kind's whole list: a line that says the map's own are not
used, then a line a spawn */
static void forge_spawns_layout_save(
	struct halo_layout_writer *writer)
{
	long index;

	/* (a map nobody can start on is not saved: it keeps the map's) */
	if (forge_spawns_globals.replaced[_forge_spawn_player] && forge_spawns_count(_forge_spawn_player) > 0)
	{
		halo_layout_printf(writer, "players");
		for (index = 0; index < forge_spawns_count(_forge_spawn_player); index++)
		{
			struct player_starting_location const *player = forge_spawns_player(index);

			halo_layout_printf(writer, "player %.4f %.4f %.4f %.5f %d %d %d %d %d",
				player->position.x, player->position.y, player->position.z, player->facing, player->team_index,
				player->game_types[0], player->game_types[1], player->game_types[2], player->game_types[3]);
		}
	}
	if (forge_spawns_globals.replaced[_forge_spawn_item])
	{
		halo_layout_printf(writer, "items");
		for (index = 0; index < forge_spawns_count(_forge_spawn_item); index++)
		{
			struct scenario_netgame_equipment const *item = forge_spawns_item(index);

			if (item->item_collection.index == NONE || !tag_get_name(item->item_collection.index))
				continue;
			halo_layout_printf(writer, "item %.4f %.4f %.4f %.5f %d %d %ld %d %d %d %d %s",
				item->position.x, item->position.y, item->position.z, item->facing, item->team_index,
				item->spawn_time, item->flags,
				item->game_type[0], item->game_type[1], item->game_type[2], item->game_type[3],
				tag_get_name(item->item_collection.index));
		}
	}

	return;
}

static void forge_spawns_layout_clear(
	void)
{
	forge_spawns_new_map();

	return;
}

static boolean forge_spawns_layout_position(
	float const *values,
	real_point3d *position)
{
	short index;

	/* (a number that is none fails every comparison) */
	for (index = 0; index < 3; index++)
	{
		if (!(values[index] > -FORGE_SPAWNS_WORLD_BOUND && values[index] < FORGE_SPAWNS_WORLD_BOUND))
			return FALSE;
	}
	set_real_point3d(position, values[0], values[1], values[2]);

	return TRUE;
}

/* a layout's game type as a spawn's: a game's own, one of the "every game"
kinds, or none for anything else */
static short forge_spawns_layout_game_type(
	int game_type)
{
	return (game_type >= game_engine_none && game_type < number_of_game_engines) ||
		(game_type >= FORGE_SPAWNS_GAME_TYPE_ALL && game_type <= FORGE_SPAWNS_GAME_TYPE_ALL_NORMAL)
		? (short)game_type
		: (short)game_engine_none;
}

/* a layout's facing: what is not a number, or turns too far round, faces
along x */
static real forge_spawns_layout_facing(
	float facing)
{
	return facing > -FORGE_SPAWNS_FACING_BOUND && facing < FORGE_SPAWNS_FACING_BOUND ? facing : 0.f;
}

/* (a layout's line comes from a file or from the host of a system link
game: what it names is checked as another machine's message is) */
static void forge_spawns_layout_load(
	char const *line)
{
	float values[4];
	int numbers[7];
	long flags;
	int name_offset = 0;
	real_point3d position;
	long index;

	if (strcmp(line, "players") == 0 || strcmp(line, "items") == 0)
	{
		short kind = line[0] == 'p' ? _forge_spawn_player : _forge_spawn_item;

		forge_spawns_take_over(kind);
		forge_spawns_block(kind)->count = 0;
	}
	else if (sscanf(line, "player %f %f %f %f %d %d %d %d %d", &values[0], &values[1], &values[2], &values[3],
		&numbers[0], &numbers[1], &numbers[2], &numbers[3], &numbers[4]) == 9)
	{
		if (forge_spawns_globals.replaced[_forge_spawn_player] && forge_spawns_layout_position(values, &position) &&
			(index = forge_spawns_append(_forge_spawn_player)) != NONE)
		{
			struct player_starting_location *player = forge_spawns_player(index);

			player->position = position;
			player->facing = forge_spawns_layout_facing(values[3]);
			player->team_index = (short)PIN(numbers[0], 0, FORGE_SPAWNS_MAXIMUM_TEAM_INDEX);
			player->game_types[0] = forge_spawns_layout_game_type(numbers[1]);
			player->game_types[1] = forge_spawns_layout_game_type(numbers[2]);
			player->game_types[2] = forge_spawns_layout_game_type(numbers[3]);
			player->game_types[3] = forge_spawns_layout_game_type(numbers[4]);
		}
	}
	else if (sscanf(line, "item %f %f %f %f %d %d %ld %d %d %d %d %n", &values[0], &values[1], &values[2], &values[3],
		&numbers[0], &numbers[1], &flags, &numbers[2], &numbers[3], &numbers[4], &numbers[5], &name_offset) >= 11 &&
		name_offset > 0)
	{
		long collection_index = tag_loaded(ITEM_COLLECTION_DEFINITION_TAG, line + name_offset);

		if (collection_index == NONE)
		{
			terminal_printf(global_real_argb_orange, "forge_spawns: %s is not in this map, left out",
				line + name_offset);
		}
		else if (forge_spawns_globals.replaced[_forge_spawn_item] && forge_spawns_layout_position(values, &position) &&
			(index = forge_spawns_append(_forge_spawn_item)) != NONE)
		{
			struct scenario_netgame_equipment *item = forge_spawns_item(index);

			item->position = position;
			item->facing = forge_spawns_layout_facing(values[3]);
			item->team_index = (short)PIN(numbers[0], 0, FORGE_SPAWNS_MAXIMUM_TEAM_INDEX);
			item->spawn_time = (short)PIN(numbers[1], 0, SHORT_MAX);
			item->flags = flags;
			item->game_type[0] = forge_spawns_layout_game_type(numbers[2]);
			item->game_type[1] = forge_spawns_layout_game_type(numbers[3]);
			item->game_type[2] = forge_spawns_layout_game_type(numbers[4]);
			item->game_type[3] = forge_spawns_layout_game_type(numbers[5]);
			item->run_time_spawned_item_index = NONE;
			item->item_collection.group_tag = ITEM_COLLECTION_DEFINITION_TAG;
			item->item_collection.index = collection_index;
		}
	}

	return;
}

/* ---------- the mod */

struct halo_mod const forge_spawns_mod =
{
	"forge_spawns",
	forge_spawns_update,
	NULL,
	NULL,
	forge_spawns_new_map,
	forge_spawns_render_world,
	&forge_spawns_menu,
	forge_spawns_grab,
	NULL,
	forge_spawns_layout_save,
	forge_spawns_layout_clear,
	forge_spawns_layout_load,
	forge_spawns_properties
};
