/*
FORGE_SELECTION.C (test)

The Forge map chosen in a map list (port/linux/game/forge_layout.c's
forge_custom_map_select) and what it marks (forge_layout_hosting): the game
hosted on the map's base map, from its lobby on, and no other; a game made
by name lets the choice go (network_game_server_change_map_name). The
selection's functions are the game's (under_test.inc), over the layout's
globals cut down to what they read.
*/

#include "harness.h"

#include <strings.h>

#define _snprintf snprintf
#define _stricmp strcasecmp

enum
{
	FORGE_LAYOUT_FILE_SIZE = 64,
	FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS = 32,
	FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT = 13,
};

struct forge_custom_map
{
	short base_index;
	char file[FORGE_LAYOUT_FILE_SIZE];
};

static struct
{
	short current_slot;
	short custom_map_count;
	struct forge_custom_map custom_maps[FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS];
	short selected_base_index;
	char selected_file[FORGE_LAYOUT_FILE_SIZE];
} forge_layout_globals;

/* the game this machine hosts, if any */
struct network_game
{
	struct
	{
		char name[128];
	} map;
};
struct network_game_server
{
	struct network_game game;
};
static struct network_game_server hosted;
static boolean hosting;

static struct network_game_server *global_network_game_server_get(void)
{
	return hosting ? &hosted : NULL;
}

static struct network_game *network_game_server_get_game(struct network_game_server *server)
{
	return &server->game;
}

static char const *tag_name_strip_path(char const *name)
{
	char const *leaf = strrchr(name, '\\');

	return leaf ? leaf + 1 : name;
}

short forge_custom_map_selected(void);

#include "under_test.inc"

/* a game's map set by name, as the game's setters do: the server's record,
through the call that lets the choice go */
static void host_by_name(char const *level)
{
	hosting = TRUE;
	snprintf(hosted.game.map.name, sizeof(hosted.game.map.name), "%s", level);
	CHANGE_MAP_NAME_CLEARS;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a variant of Blood Gulch, listed */
	forge_layout_globals.custom_map_count = 1;
	forge_layout_globals.custom_maps[0].base_index = 9;
	snprintf(forge_layout_globals.custom_maps[0].file, FORGE_LAYOUT_FILE_SIZE, "%s", "bloodgulch_arena.mvar");
	forge_custom_map_select(NONE);

	/* nothing chosen, nothing loaded: no game is Forge's */
	CASE("none")
	{
		CHECK(!forge_layout_hosting(), "marked with nothing chosen");
		host_by_name("levels\\test\\bloodgulch\\bloodgulch");
		CHECK(!forge_layout_hosting(), "a stock game marked");
		return 0;
	}

	/* chosen from the list (after the server's map is set, as the lists do):
	marked in the lobby and on, on its base map */
	CASE("chosen")
	{
		host_by_name("levels\\test\\bloodgulch\\bloodgulch");
		forge_custom_map_select(0);
		CHECK(forge_custom_map_selected() == 0 && forge_layout_hosting(), "the Forge map's lobby is not marked");
		return 0;
	}

	/* ...and then a game made by name: on the same map (Basic Training,
	matchmaking, the host's next game) the stock map, not the variant; on
	another map or a co-op level, not marked */
	CASE("by-name")
	{
		host_by_name("levels\\test\\bloodgulch\\bloodgulch");
		forge_custom_map_select(0);
		host_by_name("levels\\test\\bloodgulch\\bloodgulch");
		CHECK(forge_custom_map_selected() == NONE, "the choice kept for a game made by name");
		CHECK(!forge_layout_hosting(), "a stock game on the same map marked");
		forge_custom_map_select(0);
		host_by_name("levels\\test\\hangemhigh\\hangemhigh");
		CHECK(!forge_layout_hosting(), "a game on another map marked");
		forge_custom_map_select(0);
		host_by_name("levels\\a10\\a10");
		CHECK(!forge_layout_hosting(), "a co-op level marked");
		return 0;
	}

	/* a choice that a setter missed still marks no game on another map, and
	none where this machine hosts nothing */
	CASE("other-map")
	{
		hosting = TRUE;
		snprintf(hosted.game.map.name, sizeof(hosted.game.map.name), "%s", "levels\\test\\hangemhigh\\hangemhigh");
		forge_custom_map_select(0);
		CHECK(!forge_layout_hosting(), "a game on another map marked by a kept choice");
		hosting = FALSE;
		CHECK(!forge_layout_hosting(), "marked with no game hosted");
		return 0;
	}

	/* a layout loaded in the game (its slot) marks it, whatever was chosen */
	CASE("loaded")
	{
		host_by_name("levels\\test\\hangemhigh\\hangemhigh");
		forge_layout_globals.current_slot = 2;
		CHECK(forge_layout_hosting(), "a game with a layout loaded is not marked");
		return 0;
	}
	return 2;
}
