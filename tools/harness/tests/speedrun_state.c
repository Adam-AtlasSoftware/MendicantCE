/*
SPEEDRUN_STATE.C (test)

The speedrun state block of port/linux/game/speedrun_state.c: its layout
(speedrun_state.h, which this includes as it is), its magic, the run's ticks
across a checkpoint's revert, the loading and cutscene flags, and when a run
begins. test_speedrun_state.py takes the structures, globals and code
(under_test.inc).
*/

#include "harness.h"

#include <stddef.h>

#include "../../../port/linux/game/speedrun_state.h"

#include "under_test.inc"

static struct speedrun_state_input input;

/* (speedrun_state_frame's update, with the frame's input as the test sets it) */
static void speedrun_state_frame_test(
	void)
{
	speedrun_state_update(&speedrun_state_block, &speedrun_state_tracker, &input);
}

/* a frame of the game: ticks run, the level's clock after them */
static void frame(
	long ticks,
	unsigned long game_ticks)
{
	input.frame_ticks = ticks;
	input.game_ticks = game_ticks;
	speedrun_state_frame_test();
	input.picture = FALSE;
}

/* a map loaded (game_load), its first frame with no tick yet */
static void load(
	char const *map,
	boolean main_menu)
{
	speedrun_state_loading();
	speedrun_state_map_load();
	input.map_name = map;
	input.main_menu = main_menu;
	input.in_game = TRUE;
	frame(0, 0);
}

static void settings(
	unsigned long value,
	unsigned long sim_rate)
{
	input.settings = value;
	input.sim_rate = sim_rate;
	input.build_number = 77;
	input.build = "GOAT H1 | build 77 (test)";
}

#define BLOCK speedrun_state_block

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	memset(&input, 0, sizeof(input));
	input.main_menu = TRUE;
	input.map_name = "";
	settings(SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED, 60);

	/* every field's offset and the block's size, for the test to compare
	with the header's table, the docs' and the sample script's */
	CASE("layout")
	{
#define FIELD(name) printf("%s 0x%02X\n", #name, (unsigned)offsetof(struct speedrun_state, name))
		FIELD(magic); FIELD(version); FIELD(size); FIELD(sequence); FIELD(frame); FIELD(flags); FIELD(run);
		FIELD(run_ticks); FIELD(levels_completed); FIELD(game_ticks); FIELD(ticks_per_second); FIELD(sim_rate);
		FIELD(settings); FIELD(build_number); FIELD(build); FIELD(map);
		printf("(end) 0x%02X\n", (unsigned)sizeof(struct speedrun_state));
		return 0;
	}

	/* zero until the first frame, then the magic, the version, the size */
	CASE("magic")
	{
		CHECK(BLOCK.magic[0] == 0 && BLOCK.version == 0, "the block is written before any frame");
		frame(0, 0);
		CHECK(!memcmp((char const *)BLOCK.magic, SPEEDRUN_STATE_MAGIC, SPEEDRUN_STATE_MAGIC_SIZE), "magic %.16s",
			(char const *)BLOCK.magic);
		CHECK(BLOCK.version == SPEEDRUN_STATE_VERSION && BLOCK.size == sizeof(struct speedrun_state) &&
			BLOCK.ticks_per_second == 30, "version %u size %u", (unsigned)BLOCK.version, (unsigned)BLOCK.size);
		CHECK(BLOCK.sequence % 2 == 0 && BLOCK.frame == 1, "sequence %u frame %u", (unsigned)BLOCK.sequence,
			(unsigned)BLOCK.frame);
		CHECK(BLOCK.sim_rate == 60 && BLOCK.settings == SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED &&
			BLOCK.build_number == 77 && !strcmp((char const *)BLOCK.build, "GOAT H1 | build 77 (test)"),
			"settings");
		return 0;
	}

	/* the run's ticks go on across a revert, which takes the level's clock
	back (the negative control: the level's own clock does go back) */
	CASE("revert")
	{
		unsigned long tick, before_revert;

		frame(0, 0);
		load("levels\\a10\\a10", FALSE);
		for (tick = 1; tick <= 300; tick++)
			frame(1, tick);
		before_revert = BLOCK.run_ticks;
		CHECK(before_revert == 300 && BLOCK.game_ticks == 300, "%u ticks", (unsigned)before_revert);
		/* the checkpoint of tick 120 restored: the clock back, no tick run */
		frame(0, 120);
		CHECK(BLOCK.game_ticks == 120, "the level's clock was not taken back: %u", (unsigned)BLOCK.game_ticks);
		CHECK(BLOCK.run_ticks == before_revert, "the revert changed the run's ticks: %u",
			(unsigned)BLOCK.run_ticks);
		for (tick = 121; tick <= 150; tick++)
			frame(1, tick);
		CHECK(BLOCK.run_ticks == before_revert + 30, "the ticks after the revert: %u", (unsigned)BLOCK.run_ticks);
		/* a core loaded ahead of the level's clock adds nothing */
		frame(0, 5000);
		CHECK(BLOCK.run_ticks == before_revert + 30, "a core's load added ticks: %u", (unsigned)BLOCK.run_ticks);
		/* two ticks in one frame, and none while paused */
		frame(2, 5002);
		frame(0, 5002);
		CHECK(BLOCK.run_ticks == before_revert + 32, "%u", (unsigned)BLOCK.run_ticks);
		CHECK(BLOCK.run == 1, "the revert began a run: %u", (unsigned)BLOCK.run);
		return 0;
	}

	/* loading: from the load's start to the new map's first picture, or with
	no pictures, its first tick */
	CASE("loading")
	{
		frame(0, 0);
		CHECK(!(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING), "loading before any load");
		load("levels\\a10\\a10", FALSE);
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING, "not loading after the load");
		frame(1, 1);
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING, "a tick ended the load though pictures are drawn");
		input.picture = TRUE;
		frame(1, 2);
		CHECK(!(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING), "the first picture did not end the load");
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_IN_GAME, "not in game");
		/* with no pictures drawn */
		input.pictures_off = TRUE;
		load("levels\\a30\\a30", FALSE);
		frame(0, 0);
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING, "a frame with no tick ended the load");
		frame(1, 1);
		CHECK(!(BLOCK.flags & SPEEDRUN_STATE_FLAG_LOADING), "the first tick did not end the load");
		CHECK(!strcmp((char const *)BLOCK.map, "levels\\a30\\a30"), "map %s", (char const *)BLOCK.map);
		return 0;
	}

	/* the cutscene flag follows the game's */
	CASE("cutscene")
	{
		frame(0, 0);
		load("levels\\a10\\a10", FALSE);
		input.cutscene = TRUE;
		frame(1, 1);
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_CUTSCENE, "no cutscene flag in a cutscene");
		input.cutscene = FALSE;
		frame(1, 2);
		CHECK(!(BLOCK.flags & SPEEDRUN_STATE_FLAG_CUTSCENE), "the cutscene flag stayed");
		return 0;
	}

	/* a run begins from the main menu or with the same map again; a level
	won goes on with it; the main menu's ticks are not the run's */
	CASE("runs")
	{
		load("levels\\ui\\ui", TRUE);
		frame(1, 1);
		CHECK(BLOCK.run == 0 && BLOCK.run_ticks == 0, "the main menu: run %u, %u ticks", (unsigned)BLOCK.run,
			(unsigned)BLOCK.run_ticks);
		CHECK(BLOCK.flags & SPEEDRUN_STATE_FLAG_MAIN_MENU, "no main menu flag");
		load("levels\\a10\\a10", FALSE);
		frame(1, 1);
		frame(1, 2);
		CHECK(BLOCK.run == 1 && BLOCK.run_ticks == 2, "a new game: run %u, %u ticks", (unsigned)BLOCK.run,
			(unsigned)BLOCK.run_ticks);
		speedrun_state_level_won();
		load("levels\\a30\\a30", FALSE);
		frame(1, 1);
		CHECK(BLOCK.run == 1 && BLOCK.run_ticks == 3 && BLOCK.levels_completed == 1,
			"the next level: run %u, %u ticks, %u levels", (unsigned)BLOCK.run, (unsigned)BLOCK.run_ticks,
			(unsigned)BLOCK.levels_completed);
		/* the settings are read again as each map begins, not between */
		settings(SPEEDRUN_STATE_SETTING_MODS, 120);
		frame(1, 2);
		CHECK(BLOCK.settings == SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED, "settings read between maps");
		CHECK(BLOCK.sim_rate == 120, "the rate not read each frame: %u", (unsigned)BLOCK.sim_rate);
		/* the same map again: a restart */
		load("levels\\a30\\a30", FALSE);
		CHECK(BLOCK.run == 2 && BLOCK.run_ticks == 0 && BLOCK.levels_completed == 0, "a restart: run %u",
			(unsigned)BLOCK.run);
		CHECK(BLOCK.settings == SPEEDRUN_STATE_SETTING_MODS && BLOCK.sim_rate == 120, "settings not read");
		frame(1, 1);
		speedrun_state_level_won();
		/* back to the main menu: the run's numbers stay */
		load("levels\\ui\\ui", TRUE);
		frame(1, 1);
		frame(1, 2);
		CHECK(BLOCK.run == 2 && BLOCK.run_ticks == 1 && BLOCK.levels_completed == 1,
			"the main menu after: run %u, %u ticks, %u levels", (unsigned)BLOCK.run, (unsigned)BLOCK.run_ticks,
			(unsigned)BLOCK.levels_completed);
		CHECK(!(BLOCK.flags & SPEEDRUN_STATE_FLAG_IN_GAME), "in game at the main menu");
		return 0;
	}

	/* the PRACTICE mark in the settings as it comes on mid-level (not only
	as a map begins), off as it goes off, the map's settings kept */
	CASE("practice")
	{
		load("levels\\a10\\a10", FALSE);
		frame(1, 1);
		CHECK(!(BLOCK.settings & SPEEDRUN_STATE_SETTING_PRACTICE), "the mark at the start");
		input.practice = TRUE;
		frame(1, 2);
		CHECK(BLOCK.settings == (SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED | SPEEDRUN_STATE_SETTING_PRACTICE),
			"the mark mid-level: settings 0x%x", (unsigned)BLOCK.settings);
		input.practice = FALSE;
		load("levels\\a10\\a10", FALSE);
		CHECK(BLOCK.settings == SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED, "the mark after a fresh start: 0x%x",
			(unsigned)BLOCK.settings);
		return 0;
	}

	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
