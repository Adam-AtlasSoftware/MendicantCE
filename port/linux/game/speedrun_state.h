/*
SPEEDRUN_STATE.H

The speedrun state block (speedrun_state.c, docs/speedrun_state.md): one
static block in the game's memory that a timer's autosplitter (LiveSplit's
ASL, its auto-splitting runtime) finds by scanning the game's process for
SPEEDRUN_STATE_MAGIC, then reads at the fixed offsets below. Its layout is a
contract: a field changed or moved is a new SPEEDRUN_STATE_VERSION; fields
added at the end keep the version and grow `size`.

The game only writes it, from the main loop, with plain stores; nothing of
the game reads it back. Every field is little-endian; the strings are
NUL-terminated.

   offset  field              type
   0x00    magic              char[16]  "GOAT H1 SPEEDRUN" (no NUL)
   0x10    version            uint32    SPEEDRUN_STATE_VERSION
   0x14    size               uint32    sizeof(struct speedrun_state)
   0x18    sequence           uint32    odd while the game writes the block
   0x1C    frame              uint32    main loop frames since the game began
   0x20    flags              uint32    SPEEDRUN_STATE_FLAG_*
   0x24    run                uint32    runs begun since the game began
   0x28    run_ticks          uint32    ticks of the run (30 a second)
   0x2C    levels_completed   uint32    levels won in the run
   0x30    game_ticks         uint32    the level's own clock (reverts roll it back)
   0x34    ticks_per_second   uint32    30
   0x38    sim_rate           uint32    game.sim_rate in effect: 30, 60 or 120
   0x3C    settings           uint32    SPEEDRUN_STATE_SETTING_*
   0x40    build_number       uint32    the release's build number, 0 for a local build
   0x44    build              char[44]  the build's label ("GOAT H1 | build 123 (release)")
   0x70    map                char[64]  the map played ("levels\a10\a10")
   0xB0    (end)
*/

#ifndef SPEEDRUN_STATE_H
#define SPEEDRUN_STATE_H

#include <stdint.h>

/* ---------- constants */

#define SPEEDRUN_STATE_MAGIC "GOAT H1 SPEEDRUN"
#define SPEEDRUN_STATE_MAGIC_SIZE 16
#define SPEEDRUN_STATE_VERSION 1
#define SPEEDRUN_STATE_TICKS_PER_SECOND 30
#define SPEEDRUN_STATE_BUILD_SIZE 44
#define SPEEDRUN_STATE_MAP_SIZE 64

/* flags: the game's state this frame */
enum
{
	/* a map is being loaded or disposed of: from the start of the load until
	the new map's first picture (with no pictures drawn, its first tick) */
	SPEEDRUN_STATE_FLAG_LOADING = 1 << 0,
	/* a cutscene plays (cinematic_in_progress) */
	SPEEDRUN_STATE_FLAG_CUTSCENE = 1 << 1,
	/* the main menu's map is up (or the menus' pictures' maps, at start) */
	SPEEDRUN_STATE_FLAG_MAIN_MENU = 1 << 2,
	/* a map other than the main menu's is played */
	SPEEDRUN_STATE_FLAG_IN_GAME = 1 << 3
};

/* settings: what the run is played with, read as each map begins (the
build too; sim_rate each frame) */
enum
{
	/* display.graphics "enhanced"; neither this nor CUSTOM: "classic" */
	SPEEDRUN_STATE_SETTING_GRAPHICS_ENHANCED = 1 << 0,
	/* display.graphics "custom" */
	SPEEDRUN_STATE_SETTING_GRAPHICS_CUSTOM = 1 << 1,
	/* game.enhanced_animations (a rule of the game that display.graphics
	sets: off in Classic) */
	SPEEDRUN_STATE_SETTING_ENHANCED_ANIMATIONS = 1 << 2,
	/* mods change the map played (docs/mods.md) */
	SPEEDRUN_STATE_SETTING_MODS = 1 << 3,
	/* the map played is a Custom Edition map */
	SPEEDRUN_STATE_SETTING_CUSTOM_EDITION_MAP = 1 << 4,
	/* the map played is an MCC map */
	SPEEDRUN_STATE_SETTING_MCC_MAP = 1 << 5,
	/* the PRACTICE mark is on (practice.c: a practice key, a cheat,
	game_speed, a core's load or a skipped cutscene used since the level's
	fresh start), set and cleared each frame, not only as a map begins */
	SPEEDRUN_STATE_SETTING_PRACTICE = 1 << 6
};

/* ---------- structures */

struct speedrun_state
{
	char magic[SPEEDRUN_STATE_MAGIC_SIZE];
	uint32_t version;
	uint32_t size;
	uint32_t sequence;
	uint32_t frame;
	uint32_t flags;
	uint32_t run;
	uint32_t run_ticks;
	uint32_t levels_completed;
	uint32_t game_ticks;
	uint32_t ticks_per_second;
	uint32_t sim_rate;
	uint32_t settings;
	uint32_t build_number;
	char build[SPEEDRUN_STATE_BUILD_SIZE];
	char map[SPEEDRUN_STATE_MAP_SIZE];
};

typedef char speedrun_state_size_check[sizeof(struct speedrun_state) == 0xB0 ? 1 : -1];

/* ---------- prototypes/SPEEDRUN_STATE.C (the game's) */

/* the main loop's frame, at its end: the ticks the game ran in it, whether a
game picture was completed, whether no pictures are drawn at all, and
whether the main menu's map is up */
void speedrun_state_frame(long frame_ticks, int picture, int pictures_off, int main_menu);
/* a map's load or disposal begun (halo_loading_begin) */
void speedrun_state_loading(void);
/* a map's load begun (game_load) */
void speedrun_state_map_load(void);
/* a level won (main_won_map_private) */
void speedrun_state_level_won(void);
/* the block as the last frame left it, for the game's own readers of its
fields (stream_state.c), which only read it */
struct speedrun_state const *speedrun_state_get(void);

#endif
