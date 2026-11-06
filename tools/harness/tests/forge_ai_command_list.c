/*
FORGE_AI_COMMAND_LIST.C (test)

The AI's command list of a map (port/linux/game/forge_ai.c,
forge_ai_install_command_list): the scenario's lists are copied with Forge's
at the end: its patrol, then one for each character a squad's follower can
be. A map reset starts the map again without loading its tags, so the
scenario has Forge's lists already: they are kept, not copied with more at
the end. The previous map's copy is freed when the next map's is
made. test_forge_ai_command_list.py takes the code (under_test.inc).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "harness.h"

#define MAXIMUM_COMMANDS 8
#define FORGE_AI_MAXIMUM_UNITS 4
/* Forge's lists: the patrol and the followers' */
#define FORGE_LISTS (1 + FORGE_AI_MAXIMUM_UNITS)

struct tag_block_fake
{
	long count;
	void *address;
};

struct ai_command_list_definition
{
	char name[32];
	long flags;
	short manual_structure_bsp_reference_index;
	short runtime_structure_bsp_reference_index;
	struct tag_block_fake commands;
	struct tag_block_fake points;
};

struct ai_command_definition { long atom_type; };
struct ai_command_point_definition { real_point3d position; };

struct scenario
{
	struct tag_block_fake ai_command_lists;
};

static struct scenario the_scenario;
static short global_structure_bsp_index = 0;
#define global_scenario_get() (&the_scenario)

/* the blocks made and freed */
static int allocations;
static int frees;

static void *counted_calloc(size_t count, size_t size)
{
	allocations++;
	return calloc(count, size);
}

static void counted_free(void *block)
{
	if (block)
		frees++;
	free(block);
}

#define calloc(count, size) counted_calloc(count, size)
#define free(block) counted_free(block)

static struct
{
	struct ai_command_list_definition *command_lists;
	struct ai_command_list_definition *previous_command_lists;
	struct ai_command_definition commands[MAXIMUM_COMMANDS];
	struct ai_command_point_definition points[MAXIMUM_COMMANDS];
	short command_list_index;
	struct ai_command_definition follow_commands[FORGE_AI_MAXIMUM_UNITS];
	struct ai_command_point_definition follow_points[FORGE_AI_MAXIMUM_UNITS];
} forge_ai_globals = { NULL, NULL, { { 0 } }, { { { 0 } } }, NONE };

#include "under_test.inc"

/* a map's own scenario, its lists in the cache as a map has them */
static struct ai_command_list_definition map_lists[3][2];

static void load_map(int map, long count)
{
	long index;

	for (index = 0; index < count; index++)
		snprintf(map_lists[map][index].name, sizeof(map_lists[map][index].name), "map%d_list%ld", map, index);
	the_scenario.ai_command_lists.count = count;
	the_scenario.ai_command_lists.address = count ? map_lists[map] : NULL;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* the map's lists, then Forge's: the patrol, then a follower's for each
	character, each with its own command and point */
	CASE("copies")
	{
		struct ai_command_list_definition *lists;
		int follower;

		load_map(0, 2);
		forge_ai_install_command_list();
		lists = (struct ai_command_list_definition *)the_scenario.ai_command_lists.address;
		CHECK(the_scenario.ai_command_lists.count == 2 + FORGE_LISTS, "the map's 2 lists became %ld",
			the_scenario.ai_command_lists.count);
		CHECK(!strcmp(lists[0].name, "map0_list0") && !strcmp(lists[1].name, "map0_list1"), "the map's lists changed");
		CHECK(!strcmp(lists[2].name, "forge_ai_patrol") && forge_ai_globals.command_list_index == 2,
			"Forge's patrol is not after the map's (index %d)", forge_ai_globals.command_list_index);
		CHECK(lists[2].commands.address == forge_ai_globals.commands, "the patrol's commands are not Forge's");
		for (follower = 0; follower < FORGE_AI_MAXIMUM_UNITS; follower++)
		{
			struct ai_command_list_definition *list = &lists[3 + follower];

			CHECK(!strcmp(list->name, "forge_ai_follow") &&
				list->commands.address == &forge_ai_globals.follow_commands[follower] &&
				list->points.address == &forge_ai_globals.follow_points[follower],
				"follower list %d is not its own (%s)", follower, list->name);
		}
		return 0;
	}
	/* a map reset, again and again: one list of Forge's, one block */
	CASE("reset")
	{
		int reset;

		load_map(0, 2);
		forge_ai_install_command_list();
		for (reset = 0; reset < 5; reset++)
			forge_ai_install_command_list();
		CHECK(the_scenario.ai_command_lists.count == 2 + FORGE_LISTS, "5 resets left %ld lists (the map's 2 and Forge's %d)",
			the_scenario.ai_command_lists.count, FORGE_LISTS);
		CHECK(allocations == 1 && frees == 0, "5 resets made %d blocks and freed %d", allocations, frees);
		CHECK(forge_ai_globals.command_list_index == 2, "Forge's list is %d", forge_ai_globals.command_list_index);
		return 0;
	}
	/* new maps: each one's block is freed when the map after the next is made */
	CASE("new-maps")
	{
		int map;

		for (map = 0; map < 3; map++)
		{
			load_map(map, 1);
			forge_ai_install_command_list();
		}
		CHECK(allocations == 3 && frees == 1, "3 maps made %d blocks and freed %d (the first only)", allocations, frees);
		CHECK(the_scenario.ai_command_lists.count == 1 + FORGE_LISTS, "the third map has %ld lists",
			the_scenario.ai_command_lists.count);
		CHECK(forge_ai_globals.previous_command_lists != NULL &&
			forge_ai_globals.previous_command_lists != forge_ai_globals.command_lists,
			"the second map's block is not the one kept");
		return 0;
	}
	/* a map without lists of its own */
	CASE("no-lists")
	{
		load_map(0, 0);
		forge_ai_install_command_list();
		CHECK(the_scenario.ai_command_lists.count == FORGE_LISTS && forge_ai_globals.command_list_index == 0,
			"a map without lists has %ld, Forge's at %d", the_scenario.ai_command_lists.count,
			forge_ai_globals.command_list_index);
		forge_ai_install_command_list();
		CHECK(the_scenario.ai_command_lists.count == FORGE_LISTS && allocations == 1,
			"a reset of it has %ld lists, %d blocks", the_scenario.ai_command_lists.count, allocations);
		return 0;
	}
	/* retired (a client, whose AI is the host's): nothing of Forge's is used */
	CASE("retire")
	{
		load_map(0, 1);
		forge_ai_install_command_list();
		forge_ai_retire_command_lists();
		CHECK(forge_ai_globals.command_lists == NULL && forge_ai_globals.command_list_index == NONE,
			"a retired list is still in use (index %d)", forge_ai_globals.command_list_index);
		forge_ai_retire_command_lists();
		CHECK(frees == 1 && forge_ai_globals.previous_command_lists == NULL, "retired twice, %d freed", frees);
		return 0;
	}

	fprintf(stderr, "no case named %s\n", case_name);
	return 2;
}
