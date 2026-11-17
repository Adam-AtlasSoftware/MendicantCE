/*
PRACTICE.C (test)

The practice keys and the PRACTICE mark of port/linux/game/practice.c: every
tool sets the mark (a practice key, a console line's cheat, game_speed or
core load, a cheat left on, a skipped cutscene) and nothing else does; only
a fresh start clears it (not the keys' setting turned off); a cheat left on
sets it again after one; the keys act once a press, only in a game of this
machine's being played; a demo's playback shows the recording's mark alone.
test_practice.py takes the code (under_test.inc).
*/

#include "harness.h"

#include "../../../port/linux/game/speedrun_state.h"

enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };

/* the fake world */
static struct speedrun_state block;
static int connection, playing_demo, paused, console_open, mcc_map;
static int setting_keys, setting_skip, changes = 1;
static int keys_held[3];
static int saves, loads, restarts;

struct speedrun_state const *speedrun_state_get(void) { return &block; }
static int game_connection(void) { return connection; }
static boolean demo_playing(void) { return playing_demo; }
static boolean game_time_initialized(void) { return TRUE; }
static boolean game_time_get_paused(void) { return paused; }
static boolean console_is_active(void) { return console_open; }
static boolean mcc_cache_tags_loaded(void) { return mcc_map; }
static void main_save_core(void) { saves++; }
static void main_load_core(void) { loads++; }
static void main_reset_map(void) { restarts++; }
static unsigned long config_changes(void) { return (unsigned long)changes; }
static int config_boolean(const char *name)
{
	return !strcmp(name, "game.practice_keys") ? setting_keys : !strcmp(name, "debug.skip_cinematics") ? setting_skip : 0;
}
static int halo_named_binding_held(const char *setting)
{
	return !strcmp(setting, "game.practice_save_key") ? keys_held[0] :
		!strcmp(setting, "game.practice_load_key") ? keys_held[1] : keys_held[2];
}
#define platform_log(...) ((void)0)
#define NUMBEROF_TOOLS(array) (sizeof(array) / sizeof((array)[0]))

#include "under_test.inc"

static void settings(int keys, int skip)
{
	setting_keys = keys;
	setting_skip = skip;
	changes++;
}

static void reset(void)
{
	memset(&cheat, 0, sizeof(cheat));
	memset(keys_held, 0, sizeof(keys_held));
	practice_new_level();
	practice_frame();
	saves = loads = restarts = 0;
}

/* a key pressed and let go of over two frames */
static void press(int key)
{
	keys_held[key] = 1;
	practice_frame();
	keys_held[key] = 0;
	practice_frame();
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static char const *const tools[] = { "(cheat_all_weapons)", "game_speed 2", "core_load",
		"(core_load_name \"a\")", "cheat_deathless_player true", "(begin (Cheat_Super_Jump 1))" };
	static char const *const others[] = { "player_enable_input 1", "core_save", "debug_camera_set \"0 0 0 0 0 0 50\"",
		"(game_speed_value)", "sv_kick 3", "" };
	size_t index;

	block.flags = SPEEDRUN_STATE_FLAG_IN_GAME;
	settings(1, 0);

	/* every tool sets the mark, with its reason; nothing else does */
	CASE("every_tool")
	{
		for (index = 0; index < NUMBEROF_TOOLS(tools); index++)
		{
			reset();
			practice_console_line(tools[index]);
			CHECK(practice_marked(), "'%s' did not set the mark", tools[index]);
		}
		reset();
		practice_console_line("game_speed 0.5");
		CHECK(!strcmp(practice_reason(), "game_speed"), "the reason: %s", practice_reason());
		for (index = 0; index < NUMBEROF_TOOLS(others); index++)
		{
			reset();
			practice_console_line(others[index]);
			practice_frame();
			CHECK(!practice_marked(), "'%s' set the mark", others[index]);
		}
		for (index = 0; index < 3; index++)
		{
			reset();
			press((int)index);
			CHECK(practice_marked() && !strcmp(practice_reason(), "practice key"), "key %d", (int)index);
		}
		reset();
		cheat.medusa = TRUE;
		practice_frame();
		CHECK(practice_marked() && !strcmp(practice_reason(), "cheat"), "a cheat on");
		reset();
		settings(0, 1);
		practice_frame();
		CHECK(practice_marked() && !strcmp(practice_reason(), "skip_cinematics"), "skip_cinematics");
		/* (an MCC map's own infinite ammo is not a cheat; elsewhere it is) */
		settings(1, 0);
		reset();
		mcc_map = TRUE;
		cheat.infinite_ammo = TRUE;
		practice_frame();
		CHECK(!practice_marked(), "an MCC map's infinite ammo set the mark");
		mcc_map = FALSE;
		practice_frame();
		CHECK(practice_marked(), "infinite ammo on a stock map");
		return 0;
	}
	/* only a fresh start clears it: not frames, not the keys' setting off;
	a cheat left on sets it again at once */
	CASE("fresh_start")
	{
		reset();
		practice_console_line("game_speed 2");
		practice_frame();
		settings(0, 0);
		practice_frame();
		practice_frame();
		CHECK(practice_marked(), "cleared without a fresh start");
		practice_new_level();
		practice_frame();
		CHECK(!practice_marked() && !strcmp(practice_reason(), ""), "a fresh start kept it");
		cheat.deathless_player = TRUE;
		practice_frame();
		practice_new_level();
		practice_frame();
		CHECK(practice_marked(), "a cheat left on over a fresh start");
		return 0;
	}
	/* the keys: once a press, each its own; only with the setting on, in a
	game of this machine's being played */
	CASE("keys")
	{
		reset();
		keys_held[0] = 1;
		practice_frame();
		practice_frame();
		practice_frame();
		keys_held[0] = 0;
		practice_frame();
		CHECK(saves == 1 && loads == 0 && restarts == 0, "held save: %d %d %d", saves, loads, restarts);
		press(1);
		press(2);
		CHECK(saves == 1 && loads == 1 && restarts == 1, "load, restart: %d %d %d", saves, loads, restarts);
		paused = TRUE;
		press(0);
		paused = FALSE;
		console_open = TRUE;
		press(0);
		console_open = FALSE;
		connection = _game_connection_network_server;
		press(0);
		connection = _game_connection_local;
		block.flags = SPEEDRUN_STATE_FLAG_IN_GAME | SPEEDRUN_STATE_FLAG_MAIN_MENU;
		press(0);
		block.flags = SPEEDRUN_STATE_FLAG_IN_GAME;
		settings(0, 0);
		press(0);
		CHECK(saves == 1, "saved while it could not: %d", saves);
		settings(1, 0);
		press(0);
		CHECK(saves == 2, "not saved again: %d", saves);
		return 0;
	}
	/* a playback: the recording's mark only */
	CASE("playback")
	{
		reset();
		playing_demo = TRUE;
		practice_new_level();
		practice_console_line("cheat_omnipotent 1");
		cheat.omnipotent = TRUE;
		press(0);
		CHECK(!practice_marked() && saves == 0, "the playing machine's own use in a playback");
		practice_playback_mark(TRUE, "core_load");
		CHECK(practice_marked() && !strcmp(practice_reason(), "core_load"), "the recording's mark");
		practice_playback_mark(FALSE, "");
		CHECK(!practice_marked(), "the recording's mark off");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
