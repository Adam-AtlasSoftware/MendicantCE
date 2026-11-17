/*
PRACTICE.C

Practice keys for runners (game.practice_keys, off by default), and the
PRACTICE mark that shows a run was not played straight.

- The keys: game.practice_save_key, game.practice_load_key and
  game.practice_restart_key (unbound by default; config.toml) save the game
  state as the console's core_save does, load it as core_load does, and
  start the level over as the pause menu's RESTART MISSION does
  (main_reset_map). Only in a game of this machine's (not a network game,
  not a demo's playback), not paused, not with the console open.
- The mark: set once any of these is used, whether or not the keys are on,
  until the level's next fresh start (a map loaded or started over:
  game_initialize_for_new_map): a practice key; a console line that uses a
  cheat (cheat_*), game_speed or a core's load (the console's, init.txt's,
  the telnet console's, a cheat's buttons': hs_compile_and_evaluate); a
  cheat left on (deathless, infinite ammo and the rest of struct
  cheat_globals but its buttons' switch, and an MCC map's own infinite
  ammo, its campaign's option), which sets it again at once after
  a fresh start; debug.skip_cinematics. A restart by the key is a fresh
  start, so it ends the mark as the menu's restart does.
- It is drawn as PRACTICE at the top of every view (Hide HUD does not hide
  it), set in the speedrun state block's settings
  (SPEEDRUN_STATE_SETTING_PRACTICE) each frame, and recorded in the demo
  (its Q records, demo.c), so that a timer, a demo and a video all show it.
  A demo's playback shows the recording's mark, not the playing machine's.
- It reads the game and changes nothing of it but what the keys ask for:
  the game plays the same with the keys off as without them (determinism).
*/

#include "cseries.h"
#include "game/cheats.h"
#include "game/game.h"
#include "interface/interface.h"
#include "main/console.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "demo.h"
#include "halo_keyboard.h"
#include "mcc_cache.h"
#include "speedrun_state.h"
#include "practice.h"

#include <string.h>

/* ---------- constants */

enum
{
	PRACTICE_REASON_SIZE = 32,
	/* draw_string.c's centred text (its enum is its own) */
	_practice_text_center = 2
};

/* the practice keys */
enum
{
	_practice_key_save,
	_practice_key_load,
	_practice_key_restart,
	NUMBER_OF_PRACTICE_KEYS
};

/* the mark's place: this share of the view's height down, centred */
#define PRACTICE_TOP_FRACTION 0.06f

/* ---------- prototypes */

/* port_config.c's, platform.h's */
int config_boolean(const char *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);

/* ---------- structures */

struct practice_state
{
	boolean marked;
	char reason[PRACTICE_REASON_SIZE];
};

/* ---------- globals */

static struct practice_state practice_state;
static char const *const practice_key_settings[NUMBER_OF_PRACTICE_KEYS] =
{
	"game.practice_save_key", "game.practice_load_key", "game.practice_restart_key"
};
static struct
{
	unsigned long changes;
	boolean keys;
	boolean skip_cinematics;
	/* the keys held last frame */
	unsigned long held;
} practice_settings = { (unsigned long)-1, FALSE, FALSE, 0 };

/* ---------- private code */

/* the mark set (the reason kept is the first's); TRUE if it was off */
static boolean practice_mark(
	struct practice_state *state,
	char const *reason)
{
	if (state->marked)
		return FALSE;
	state->marked = TRUE;
	strncpy(state->reason, reason, PRACTICE_REASON_SIZE - 1);
	state->reason[PRACTICE_REASON_SIZE - 1] = 0;
	return TRUE;
}

static void practice_clear(
	struct practice_state *state)
{
	state->marked = FALSE;
	state->reason[0] = 0;
}

/* the tool a console line uses (its words: a cheat's, game_speed, a core's
load), NULL for none */
static char const *practice_line_tool(
	char const *line)
{
	while (line && *line)
	{
		char word[64];
		size_t length = 0;

		while (*line && !((*line >= 'a' && *line <= 'z') || (*line >= 'A' && *line <= 'Z') ||
			(*line >= '0' && *line <= '9') || *line == '_'))
		{
			line++;
		}
		while (((*line >= 'a' && *line <= 'z') || (*line >= 'A' && *line <= 'Z') || (*line >= '0' && *line <= '9') ||
			*line == '_'))
		{
			if (length < sizeof(word) - 1)
				word[length++] = (char)(*line >= 'A' && *line <= 'Z' ? *line - 'A' + 'a' : *line);
			line++;
		}
		word[length] = 0;
		if (!strncmp(word, "cheat_", 6))
			return "cheat";
		if (!strcmp(word, "game_speed"))
			return "game_speed";
		if (!strncmp(word, "core_load", 9))
			return "core_load";
	}
	return NULL;
}

/* whether a cheat is on (its buttons' switch is not one; nor, on an MCC
map, infinite ammo, which its campaign's own option sets: mcc_campaign.c) */
static boolean practice_cheats_on(
	struct cheat_globals const *cheats,
	boolean mcc_map)
{
	return cheats->deathless_player || cheats->jetpack || (cheats->infinite_ammo && !mcc_map) ||
		cheats->bump_possession || cheats->super_jump || cheats->reflexive_damage_effects || cheats->medusa ||
		cheats->omnipotent || cheats->bottomless_clip;
}

/* the keys pressed this frame (held now, not last frame) */
static unsigned long practice_keys_pressed(
	unsigned long held,
	unsigned long last_held)
{
	return held & ~last_held;
}

/* the mark set, logged, unless a demo plays (whose mark is the
recording's) */
static void practice_note(
	char const *reason)
{
	if (demo_playing())
		return;
	if (practice_mark(&practice_state, reason))
		platform_log("practice: %s used: the PRACTICE mark is on until the level's next fresh start", reason);
}

static void practice_read_settings(
	void)
{
	if (practice_settings.changes == config_changes())
		return;
	practice_settings.changes = config_changes();
	practice_settings.keys = config_boolean("game.practice_keys") != 0;
	practice_settings.skip_cinematics = config_boolean("debug.skip_cinematics") != 0;
}

/* whether the keys may act: a game of this machine's being played, not a
playback, not paused, not with the console open */
static boolean practice_keys_usable(
	void)
{
	unsigned long flags = speedrun_state_get()->flags;

	return (flags & SPEEDRUN_STATE_FLAG_IN_GAME) && !(flags & (SPEEDRUN_STATE_FLAG_MAIN_MENU |
		SPEEDRUN_STATE_FLAG_LOADING)) && game_connection() == _game_connection_local && !demo_playing() &&
		game_time_initialized() && !game_time_get_paused() && !console_is_active();
}

static void practice_keys_frame(
	void)
{
	unsigned long held = 0;
	unsigned long pressed;
	short key;

	for (key = 0; key < NUMBER_OF_PRACTICE_KEYS; key++)
	{
		if (halo_named_binding_held(practice_key_settings[key]))
			held |= 1UL << key;
	}
	pressed = practice_keys_usable() ? practice_keys_pressed(held, practice_settings.held) : 0;
	practice_settings.held = held;
	if (!pressed)
		return;
	practice_note("practice key");
	if (pressed & (1UL << _practice_key_save))
	{
		main_save_core();
		platform_log("practice: state saved (core_save)");
	}
	else if (pressed & (1UL << _practice_key_load))
	{
		main_load_core();
		platform_log("practice: state loaded (core_load)");
	}
	else if (pressed & (1UL << _practice_key_restart))
	{
		main_reset_map();
		platform_log("practice: the level started over");
	}
}

/* ---------- public code */

boolean practice_marked(
	void)
{
	return practice_state.marked;
}

char const *practice_reason(
	void)
{
	return practice_state.reason;
}

void practice_console_line(
	char const *line)
{
	char const *tool = practice_line_tool(line);

	if (tool)
		practice_note(tool);
}

void practice_new_level(
	void)
{
	practice_clear(&practice_state);
}

void practice_frame(
	void)
{
	practice_read_settings();
	if (practice_settings.keys)
		practice_keys_frame();
	else
		practice_settings.held = 0;
	if (practice_cheats_on(&cheat, mcc_cache_tags_loaded()))
		practice_note("cheat");
	if (practice_settings.skip_cinematics)
		practice_note("skip_cinematics");
}

void practice_playback_mark(
	boolean marked,
	char const *reason)
{
	if (marked)
	{
		practice_clear(&practice_state);
		practice_mark(&practice_state, reason);
	}
	else
		practice_clear(&practice_state);
}

void practice_draw(
	void)
{
	long font_index;
	real_argb_color color;
	rectangle2d bounds;

	if (!practice_state.marked)
		return;
	font_index = interface_get_tag_index(_interface_font_terminal);
	if (font_index == NONE)
		return;
	bounds = render.camera.window_bounds;
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	bounds.y0 = (short)(bounds.y0 + (bounds.y1 - bounds.y0) * PRACTICE_TOP_FRACTION);
	bounds.y1 = (short)(bounds.y0 + 24);
	color.alpha = 1.0f;
	color.red = 1.0f;
	color.green = 0.6f;
	color.blue = 0.15f;
	draw_string_set_draw_mode(font_index, NONE, _practice_text_center, 0, &color);
	rasterizer_draw_string(&bounds, NULL, NULL, 0, "PRACTICE");
}
