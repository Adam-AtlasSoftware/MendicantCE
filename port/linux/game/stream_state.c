/*
STREAM_STATE.C

The game's state in a small JSON file, game_state.json in the data root
(display.state_file, off by default), for stream overlays (an OBS text
source) and scene switchers (a cutscene, a load or the pause menu its own
scene), as Counter-Strike's game state integration gives:

    {"state":"playing","map":"levels\\a10\\a10","mode":"campaign","clock":95,
     "score":0,"kills":3,"deaths":1,"practice":false}

- state: "loading", "menu" (the main menu), "cutscene", "paused" or
  "playing", in that order of precedence; map and clock are the speedrun
  state block's (speedrun_state.c: the map, the level's clock in whole
  seconds); mode "menu", "campaign" (and co-op) or the multiplayer game type;
  score, kills and deaths the first local player's (0 with none); practice
  the PRACTICE mark (practice.c, the block's SPEEDRUN_STATE_SETTING_PRACTICE).
- The map passes through streamer mode's filter (text_filter.c), as what
  the game draws of it does.
- Written only when it changed, at most four times a second, from the main
  loop: to game_state.json.tmp, then renamed over game_state.json, so a
  reader never sees half a file. No socket, no thread.
- It reads the game and writes nothing of it: the game plays the same with
  it as without (determinism). Off, nothing is read or written.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "memory/data.h"
#include "speedrun_state.h"
#include "stream_state.h"

#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* the least time between two writes */
	STREAM_STATE_MILLISECONDS = 250,
	STREAM_STATE_TEXT_SIZE = 512,
	STREAM_STATE_PATH_SIZE = 1024
};

/* ---------- prototypes */

/* port_config.c's, cseries_windows.c's, platform.h's, text_filter.c's */
int config_boolean(const char *name);
unsigned long config_changes(void);
unsigned long system_milliseconds(void);
void platform_log(const char *format, ...);
char const *text_filter_string(char const *string);

/* ---------- structures */

/* what the file says */
struct stream_state_input
{
	/* the speedrun state block's flags (SPEEDRUN_STATE_FLAG_*) */
	unsigned long flags;
	boolean paused;
	char const *map;
	char const *mode;
	/* the level's clock, in whole seconds */
	unsigned long seconds;
	long score;
	long kills;
	long deaths;
	/* the PRACTICE mark (practice.c) */
	boolean practice;
};

/* what the writer remembers between frames */
struct stream_state_tracker
{
	/* the text written last, and when (system_milliseconds) */
	char written[STREAM_STATE_TEXT_SIZE];
	unsigned long written_at;
	boolean ever_written;
	/* a write failed (logged once) */
	boolean failed;
};

/* ---------- globals */

static struct stream_state_tracker stream_state_tracker;

/* ---------- private code */

/* text as a JSON string's contents: quotes, backslashes and control
characters escaped */
static void stream_state_append_string(
	char *text,
	size_t size,
	char const *string)
{
	size_t length = strlen(text);

	for (; string && *string && length + 7 < size; string++)
	{
		unsigned char character = (unsigned char)*string;

		if (character == '"' || character == '\\')
		{
			text[length++] = '\\';
			text[length++] = (char)character;
		}
		else if (character < 0x20)
			length += (size_t)snprintf(text + length, size - length, "\\u%04x", character);
		else
			text[length++] = (char)character;
	}
	text[length] = 0;
}

static char const *stream_state_name(
	struct stream_state_input const *input)
{
	if (input->flags & SPEEDRUN_STATE_FLAG_LOADING)
		return "loading";
	if ((input->flags & SPEEDRUN_STATE_FLAG_MAIN_MENU) || !(input->flags & SPEEDRUN_STATE_FLAG_IN_GAME))
		return "menu";
	if (input->flags & SPEEDRUN_STATE_FLAG_CUTSCENE)
		return "cutscene";
	if (input->paused)
		return "paused";
	return "playing";
}

/* the file's text */
static void stream_state_text(
	struct stream_state_input const *input,
	char *text,
	size_t size)
{
	size_t length;

	snprintf(text, size, "{\"state\":\"%s\",\"map\":\"", stream_state_name(input));
	stream_state_append_string(text, size, input->map);
	length = strlen(text);
	snprintf(text + length, size - length, "\",\"mode\":\"");
	stream_state_append_string(text, size, input->mode);
	length = strlen(text);
	snprintf(text + length, size - length,
		"\",\"clock\":%lu,\"score\":%ld,\"kills\":%ld,\"deaths\":%ld,\"practice\":%s}\n", input->seconds, input->score,
		input->kills, input->deaths, input->practice ? "true" : "false");
}

/* text written to path at once: to path.tmp, then renamed over it. FALSE if
it could not be */
static boolean stream_state_write(
	char const *path,
	char const *text)
{
	char temporary[STREAM_STATE_PATH_SIZE + 8];
	FILE *file;
	boolean written;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
		return FALSE;
	written = fputs(text, file) >= 0;
	written = fclose(file) == 0 && written;
	if (!written || rename(temporary, path) != 0)
	{
		remove(temporary);
		return FALSE;
	}
	return TRUE;
}

/* the frame's text written if it changed and the last write was long
enough ago; TRUE if it was written */
static boolean stream_state_offer(
	struct stream_state_tracker *tracker,
	char const *path,
	char const *text,
	unsigned long now)
{
	if (tracker->ever_written && !strcmp(text, tracker->written))
		return FALSE;
	if (tracker->ever_written && now - tracker->written_at < STREAM_STATE_MILLISECONDS)
		return FALSE;
	if (!stream_state_write(path, text))
	{
		if (!tracker->failed)
			platform_log("state file: could not write %s", path);
		tracker->failed = TRUE;
		/* (tried again no sooner than a write would be) */
		tracker->written_at = now;
		return FALSE;
	}
	tracker->failed = FALSE;
	tracker->ever_written = TRUE;
	tracker->written_at = now;
	snprintf(tracker->written, sizeof(tracker->written), "%s", text);
	return TRUE;
}

static boolean stream_state_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;

	if (changes != config_changes())
	{
		changes = config_changes();
		enabled = config_boolean("display.state_file") != 0;
	}
	return enabled;
}

static char const *stream_state_mode(
	unsigned long flags)
{
	if ((flags & SPEEDRUN_STATE_FLAG_MAIN_MENU) || !(flags & SPEEDRUN_STATE_FLAG_IN_GAME))
		return "menu";
	if (!game_engine_running())
		return "campaign";
	switch (game_engine_get_variant()->game_engine_index)
	{
	case game_engine_ctf: return "ctf";
	case game_engine_slayer: return "slayer";
	case game_engine_oddball: return "oddball";
	case game_engine_king: return "king";
	case game_engine_race: return "race";
	case game_engine_terminator: return "juggernaut";
	default: return "game";
	}
}

/* the first local player's score, kills and deaths */
static void stream_state_player(
	struct stream_state_input *input)
{
	short local_player_index = local_player_get_next(NONE);
	long player_index = local_player_index != NONE ? local_player_get_player_index(local_player_index) : NONE;
	struct player_datum *player;

	if (player_index == NONE)
		return;
	player = player_get(player_index);
	input->kills = player->statistics.kills[0];
	input->deaths = player->statistics.deaths;
	if (game_engine_running() && game_engine && game_engine->get_player_score)
		input->score = game_engine->get_player_score(DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), _get_score_individual);
}

/* ---------- public code */

void stream_state_frame(
	void)
{
	struct speedrun_state const *block;
	struct stream_state_input input;
	char text[STREAM_STATE_TEXT_SIZE];

	if (!stream_state_enabled())
		return;
	block = speedrun_state_get();
	memset(&input, 0, sizeof(input));
	input.flags = block->flags;
	input.map = text_filter_string(block->map);
	input.mode = stream_state_mode(input.flags);
	input.seconds = block->game_ticks / SPEEDRUN_STATE_TICKS_PER_SECOND;
	input.practice = (block->settings & SPEEDRUN_STATE_SETTING_PRACTICE) != 0;
	if ((input.flags & SPEEDRUN_STATE_FLAG_IN_GAME) && game_time_initialized() && game_in_progress())
	{
		input.paused = game_time_get_paused();
		stream_state_player(&input);
	}
	stream_state_text(&input, text, sizeof(text));
	/* (an Xbox path, as the game's files are opened: d:\ is the data root,
	halo_linux_fopen) */
	stream_state_offer(&stream_state_tracker, "d:\\" STREAM_STATE_FILE_NAME, text, system_milliseconds());
}
