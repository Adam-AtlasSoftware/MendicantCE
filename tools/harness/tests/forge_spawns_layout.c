/*
FORGE_SPAWNS_LAYOUT.C (test)

A layout's spawn lines (port/linux/game/forge_spawns.c,
forge_spawns_layout_load): a layout comes from a file or from the host of a
system link game, so what a line names is checked before it is a spawn: a
team in range, the game types the game has, a facing that is a number.
test_forge_spawns_layout.py takes the constants (config.inc) and the code
(under_test.inc).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "harness.h"
#include "config.inc"

#define PIN(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SHORT_MAX 32767
#define set_real_point3d(point, px, py, pz) ((point)->x = (px), (point)->y = (py), (point)->z = (pz))
#define terminal_printf(color, ...) ((void)(color))
#define ITEM_COLLECTION_DEFINITION_TAG 0x69746d63
#define FORGE_SPAWNS_TEST_COLLECTION 7
#define tag_loaded(group, name) (strcmp((name), "weapons\\random") == 0 ? FORGE_SPAWNS_TEST_COLLECTION : NONE)

static int global_real_argb_orange;

struct player_starting_location
{
	real_point3d position;
	real facing;
	short team_index;
	short game_types[4];
};

struct scenario_netgame_equipment
{
	long flags;
	short game_type[4];
	short team_index;
	short spawn_time;
	real_point3d position;
	real facing;
	struct { long group_tag; long index; } item_collection;
	long run_time_spawned_item_index;
};

/* the scenario's lists of spawns, as this mod's own (forge_spawns_take_over) */
enum { FORGE_SPAWNS_TEST_MAXIMUM = 8 };
static struct player_starting_location players[FORGE_SPAWNS_TEST_MAXIMUM];
static struct scenario_netgame_equipment items[FORGE_SPAWNS_TEST_MAXIMUM];
static struct { long count; } blocks[NUMBER_OF_FORGE_SPAWN_KINDS];
static struct { boolean replaced[NUMBER_OF_FORGE_SPAWN_KINDS]; } forge_spawns_globals;

#define forge_spawns_block(kind) (&blocks[(kind)])
#define forge_spawns_player(index) (&players[(index)])
#define forge_spawns_item(index) (&items[(index)])

static void forge_spawns_take_over(short kind)
{
	forge_spawns_globals.replaced[kind] = TRUE;
}

static long forge_spawns_append(short kind)
{
	return blocks[kind].count < FORGE_SPAWNS_TEST_MAXIMUM ? blocks[kind].count++ : NONE;
}

#include "under_test.inc"

/* the layout's lines, the list of each kind taken over first */
static void load(char const *line)
{
	forge_spawns_layout_load("players");
	forge_spawns_layout_load("items");
	forge_spawns_layout_load(line);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a spawn as the map has it is taken as it is */
	CASE("valid")
	{
		load("player 1.5 2.5 3.5 1.25 1 2 12 0 0");
		CHECK(blocks[_forge_spawn_player].count == 1, "a valid player spawn was left out");
		CHECK(players[0].team_index == 1 && players[0].facing == 1.25f, "the spawn changed: team %d facing %f",
			players[0].team_index, (double)players[0].facing);
		CHECK(players[0].game_types[0] == game_engine_slayer && players[0].game_types[1] == FORGE_SPAWNS_GAME_TYPE_ALL,
			"its game types changed: %d %d", players[0].game_types[0], players[0].game_types[1]);
		load("item 1 2 3 0.5 0 30 0 1 0 0 0 weapons\\random");
		CHECK(blocks[_forge_spawn_item].count == 1 && items[0].spawn_time == 30 &&
			items[0].item_collection.index == FORGE_SPAWNS_TEST_COLLECTION, "a valid item spawn was not taken as it is");
		return 0;
	}
	/* a team outside the game's 16 is pinned into them */
	CASE("team")
	{
		load("player 0 0 0 0 9999 1 0 0 0");
		CHECK(players[0].team_index == FORGE_SPAWNS_MAXIMUM_TEAM_INDEX, "team 9999 became %d", players[0].team_index);
		load("player 0 0 0 0 -5 1 0 0 0");
		CHECK(players[0].team_index == 0, "team -5 became %d", players[0].team_index);
		load("item 0 0 0 0 70000 30 0 1 0 0 0 weapons\\random");
		CHECK(items[0].team_index >= 0 && items[0].team_index <= FORGE_SPAWNS_MAXIMUM_TEAM_INDEX,
			"an item's team 70000 became %d", items[0].team_index);
		return 0;
	}
	/* a game type the game has not is none */
	CASE("game-types")
	{
		load("player 0 0 0 0 0 9 -1 40 13");
		CHECK(players[0].game_types[0] == game_engine_none, "game type 9 was kept as %d", players[0].game_types[0]);
		CHECK(players[0].game_types[1] == game_engine_none, "game type -1 was kept as %d", players[0].game_types[1]);
		CHECK(players[0].game_types[2] == game_engine_none, "game type 40 was kept as %d", players[0].game_types[2]);
		CHECK(players[0].game_types[3] == FORGE_SPAWNS_GAME_TYPE_ALL_NON_TEAM, "\"all but CTF\" was not kept");
		load("item 0 0 0 0 0 30 0 100 2 -7 14 weapons\\random");
		CHECK(items[0].game_type[0] == game_engine_none && items[0].game_type[1] == game_engine_slayer &&
			items[0].game_type[2] == game_engine_none && items[0].game_type[3] == FORGE_SPAWNS_GAME_TYPE_ALL_NORMAL,
			"an item's game types were %d %d %d %d", items[0].game_type[0], items[0].game_type[1], items[0].game_type[2],
			items[0].game_type[3]);
		return 0;
	}
	/* a facing that is not a number, or turns far round, faces along x */
	CASE("facing")
	{
		load("player 0 0 0 nan 0 1 0 0 0");
		CHECK(blocks[_forge_spawn_player].count == 0 || players[0].facing == 0.f,
			"a facing of nan was kept as %f", (double)players[0].facing);
		load("player 0 0 0 1e30 0 1 0 0 0");
		CHECK(players[0].facing == 0.f, "a facing of 1e30 was kept as %f", (double)players[0].facing);
		load("item 0 0 0 -inf 0 30 0 1 0 0 0 weapons\\random");
		CHECK(blocks[_forge_spawn_item].count == 0 || items[0].facing == 0.f,
			"an item's facing of -inf was kept as %f", (double)items[0].facing);
		return 0;
	}
	/* a time below none is none, one past a short's the most */
	CASE("spawn-time")
	{
		load("item 0 0 0 0 0 -30 0 1 0 0 0 weapons\\random");
		CHECK(items[0].spawn_time == 0, "a time of -30 became %d", items[0].spawn_time);
		load("item 0 0 0 0 0 99999 0 1 0 0 0 weapons\\random");
		CHECK(items[0].spawn_time == SHORT_MAX, "a time of 99999 became %d", items[0].spawn_time);
		return 0;
	}
	/* a place outside the world is no spawn at all */
	CASE("position")
	{
		load("player 1e9 0 0 0 0 1 0 0 0");
		CHECK(blocks[_forge_spawn_player].count == 0, "a spawn outside the world was taken");
		return 0;
	}

	fprintf(stderr, "no case named %s\n", case_name);
	return 2;
}
