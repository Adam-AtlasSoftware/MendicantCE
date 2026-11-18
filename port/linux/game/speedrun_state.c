/*
SPEEDRUN_STATE.C

The speedrun state block (speedrun_state.h, docs/speedrun_state.md): what a
timer's autosplitter needs, in one static block it finds by its magic.

- The game writes it from the main loop alone (main_loop, at each frame's
  end; and the loading flag, the map's load and a level won as they happen,
  on the same thread): plain stores, no thread, file or socket. Nothing of
  the game's simulation reads it (only the state file's writer,
  stream_state.c, does), and it is not in the game state, so a saved game, a
  checkpoint's revert, a core's load and a demo neither keep nor change it,
  and the game plays the same with it as without (determinism).
- It lives in the game's own memory, the guest's: on Linux and Windows the
  64-bit host process's memory, at the guest's address (x86-64's guest
  addresses are the process's own; with the base register, the base plus
  them), so a reader that scans the process finds it there. It is zero
  until the first frame writes its magic, which is put together then, never
  stored whole: the scan finds the block, not a copy of its magic in the
  executable or the guest's image.
- The run's ticks are the ticks the game ran, counted as it runs them: a
  checkpoint's revert or a core's load, which take the level's clock back
  (or forward), take nothing from them, and the ticks played again after a
  revert count again. A paused game, a load and a cutscene skipped run no
  ticks, so add none. A run begins when a map other than the main menu's
  loads from the main menu (a new game, a mission chosen, a saved game
  resumed), or the map played loads again (a restart): the run's number
  goes up, its ticks and its levels go back to 0. A level won that leads
  to the next goes on with the run. Back at the main menu, the run's
  numbers stay as they ended, for the timer to read.
*/

#include "cseries.h"
#include "game/game.h"
#include "cutscene/cinematics.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "custom_edition_cache.h"
#include "mcc_cache.h"
#include "mods.h"
#include "network_standard.h"
#include "sim_rate.h"
#include "speedrun_state.h"
#include "practice.h"

#include <string.h>

/* ---------- prototypes */

/* port_config.c's, main.c's */
const char *config_string(char const *name);
void main_build_identity(long *number, char *flavor, size_t flavor_size, char *repository, size_t repository_size,
	char *version, size_t version_size);

/* ---------- structures */

/* what a frame tells the block (speedrun_state_frame gathers it) */
struct speedrun_state_input
{
	/* the ticks the game ran this frame */
	long frame_ticks;
	/* a game picture was completed this frame */
	boolean picture;
	/* no pictures are drawn (no display): a load ends at its first tick */
	boolean pictures_off;
	/* the main menu's map is up */
	boolean main_menu;
	boolean in_game;
	boolean cutscene;
	unsigned long game_ticks;
	char const *map_name;
	/* the rate the game simulates at */
	unsigned long sim_rate;
	/* the settings (read only when the tracker asks: speedrun_state_wants_settings) */
	unsigned long settings;
	unsigned long build_number;
	char const *build;
	/* the PRACTICE mark (each frame's) */
	boolean practice;
};

/* what the block's writer remembers between frames */
struct speedrun_state_tracker
{
	/* the constant fields written */
	boolean started;
	/* a map's load begun since the last frame */
	boolean map_loading;
	/* the map of the last frame was the main menu's, or there was none */
	boolean last_main_menu;
	char last_map[SPEEDRUN_STATE_MAP_SIZE];
};

/* ---------- globals */

static volatile struct speedrun_state speedrun_state_block;
static struct speedrun_state_tracker speedrun_state_tracker = { FALSE, FALSE, TRUE, "" };
/* the magic, back to front: put together the right way round at run time
(speedrun_state_update), so the image holds it only so */
static char const speedrun_state_magic_reversed[SPEEDRUN_STATE_MAGIC_SIZE + 1] = "NURDEEPS 1H TAOG";
/* (0: read at run time, so that the compiler cannot put the magic together
itself, as a constant in the image) */
static volatile unsigned char speedrun_state_magic_mask;

/* ---------- private code */

static void speedrun_state_copy(
	volatile char *destination,
	char const *text,
	long size)
{
	long index;

	for (index = 0; index < size - 1 && text && text[index]; index++)
		destination[index] = text[index];
	for (; index < size; index++)
		destination[index] = 0;
}

/* whether the frame's settings are wanted: as the block starts and as each
map begins */
static boolean speedrun_state_wants_settings(
	struct speedrun_state_tracker const *tracker)
{
	return !tracker->started || tracker->map_loading;
}

/* the frame's update of the block */
static void speedrun_state_update(
	volatile struct speedrun_state *block,
	struct speedrun_state_tracker *tracker,
	struct speedrun_state_input const *input)
{
	unsigned long flags;
	long index;

	block->sequence++;
	if (!tracker->started)
	{
		for (index = 0; index < SPEEDRUN_STATE_MAGIC_SIZE; index++)
		{
			block->magic[index] = (char)(speedrun_state_magic_reversed[SPEEDRUN_STATE_MAGIC_SIZE - 1 - index] ^
				speedrun_state_magic_mask);
		}
		block->version = SPEEDRUN_STATE_VERSION;
		block->size = sizeof(struct speedrun_state);
		block->ticks_per_second = SPEEDRUN_STATE_TICKS_PER_SECOND;
	}
	if (speedrun_state_wants_settings(tracker))
	{
		block->settings = input->settings;
		block->build_number = input->build_number;
		speedrun_state_copy(block->build, input->build, SPEEDRUN_STATE_BUILD_SIZE);
	}
	/* (the PRACTICE mark, each frame: it comes on as a tool is used and goes
	off only at a fresh start, which a run's timer must see as it happens) */
	if (input->practice)
		block->settings |= SPEEDRUN_STATE_SETTING_PRACTICE;
	else
		block->settings &= ~(unsigned long)SPEEDRUN_STATE_SETTING_PRACTICE;
	/* a map loaded since the last frame: a new run, from the main menu or
	the same map again */
	if (tracker->map_loading && !input->main_menu &&
		(tracker->last_main_menu ||
		!strncmp(tracker->last_map, input->map_name ? input->map_name : "", SPEEDRUN_STATE_MAP_SIZE - 1)))
	{
		block->run++;
		block->run_ticks = 0;
		block->levels_completed = 0;
	}
	tracker->started = TRUE;
	tracker->map_loading = FALSE;
	tracker->last_main_menu = input->main_menu;
	speedrun_state_copy(tracker->last_map, input->map_name, SPEEDRUN_STATE_MAP_SIZE);

	if (!input->main_menu && input->frame_ticks > 0)
		block->run_ticks += (unsigned long)input->frame_ticks;

	/* the load ends with the new map's first picture (with none drawn, its
	first tick) */
	flags = block->flags & SPEEDRUN_STATE_FLAG_LOADING;
	if (flags && (input->picture || (input->pictures_off && input->frame_ticks > 0)))
		flags = 0;
	if (input->cutscene)
		flags |= SPEEDRUN_STATE_FLAG_CUTSCENE;
	if (input->main_menu)
		flags |= SPEEDRUN_STATE_FLAG_MAIN_MENU;
	if (input->in_game && !input->main_menu)
		flags |= SPEEDRUN_STATE_FLAG_IN_GAME;
	block->flags = flags;
	block->game_ticks = input->game_ticks;
	block->sim_rate = input->sim_rate;
	speedrun_state_copy(block->map, input->map_name, SPEEDRUN_STATE_MAP_SIZE);
	block->frame++;
	block->sequence++;
}

/* the settings the run is played with (speedrun_state.h's
SPEEDRUN_STATE_SETTING_*), as the map begins */
static void speedrun_state_read_settings(
	struct speedrun_state_input *input,
	char *build,
	size_t build_size)
{
	char const *graphics = config_string("display.graphics");
	long counts[4];
	long number = 0;
	char flavor[32], repository[64];

	input->settings = 0;
	if (graphics && !strcmp(graphics, "enhanced"))
		input->settings |= SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED;
	else if (graphics && !strcmp(graphics, "custom"))
		input->settings |= SPEEDRUN_STATE_SETTING_GRAPHICS_CUSTOM;
	if (network_standard_enhanced_animations())
		input->settings |= SPEEDRUN_STATE_SETTING_ENHANCED_ANIMATIONS;
	if (mods_played_on_map(counts) && (counts[0] || counts[1] || counts[2] || counts[3]))
		input->settings |= SPEEDRUN_STATE_SETTING_MODS;
	if (custom_edition_cache_tags_loaded())
		input->settings |= SPEEDRUN_STATE_SETTING_CUSTOM_EDITION_MAP;
	if (input->map_name && mcc_level_name(input->map_name))
		input->settings |= SPEEDRUN_STATE_SETTING_MCC_MAP;
	main_build_identity(&number, flavor, sizeof(flavor), repository, sizeof(repository), build, build_size);
	input->build_number = (unsigned long)number;
	input->build = build;
}

/* ---------- public code */

void speedrun_state_frame(
	long frame_ticks,
	int picture,
	int pictures_off,
	int main_menu)
{
	struct speedrun_state_input input;
	char build[SPEEDRUN_STATE_BUILD_SIZE];
	boolean playing = game_time_initialized() && game_in_progress();

	memset(&input, 0, sizeof(input));
	input.frame_ticks = frame_ticks;
	input.picture = picture != 0;
	input.pictures_off = pictures_off != 0;
	input.main_menu = main_menu != 0;
	input.in_game = playing;
	input.cutscene = playing && cinematic_in_progress();
	input.game_ticks = playing ? (unsigned long)game_time_get() : 0;
	/* (the next tick's: a map's first frame has run none yet) */
	input.sim_rate = (unsigned long)sim_rate_next();
	input.map_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
	if (speedrun_state_wants_settings(&speedrun_state_tracker))
		speedrun_state_read_settings(&input, build, sizeof(build));
	input.practice = practice_marked();
	speedrun_state_update(&speedrun_state_block, &speedrun_state_tracker, &input);
}

void speedrun_state_loading(
	void)
{
	speedrun_state_block.flags |= SPEEDRUN_STATE_FLAG_LOADING;
}

void speedrun_state_map_load(
	void)
{
	speedrun_state_tracker.map_loading = TRUE;
}

void speedrun_state_level_won(
	void)
{
	speedrun_state_block.levels_completed++;
}

struct speedrun_state const *speedrun_state_get(
	void)
{
	return (struct speedrun_state const *)&speedrun_state_block;
}
