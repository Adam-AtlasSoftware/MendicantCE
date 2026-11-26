/*
PRISM_BAKE_MENU.C

PRISM's light bake on the main menu (docs/prism_bake.md, "The bake on the
main menu" and "The shipped bake"): with display.lighting "raster" or
"pathtraced" (both read the bake), a graphics card that traces rays and
display.bake "auto", the main menu bakes the campaign's levels that have no
bake of the settings now chosen, before they are played, as other games
compile their shaders. A release ships every campaign level's light baked
at MAX for both worlds (display.bake_shipped_folder, its manifest.txt): a
BSP it has is ready with nothing baked, and the worker only checks it once
against this machine's maps (the baker's shipped.txt); a BSP whose shipped
light does not match (a mod, a custom map, a remodel built otherwise) is
baked here, that BSP alone. No setting a player has bakes again but World
Geometry where the shipped bake lacks that world.

- The bake is the in-game baker's (prism_bake.c, xgpu_vulkan_bake.c), which
  bakes the level drawn: the main menu has none, so a level is baked by a
  worker, the game started again as a process of its own (update.h's
  update_child_start), hidden and silent (no sound, controllers, network,
  updates or reports; its own saves and pipeline cache), with
  debug.bake_worker its map: it loads the map at once (console.c's
  console_startup), stops the game's clock, bakes each BSP in turn in the
  foreground (40 ms of each frame's GPU time) and quits. It reads the same
  config.toml, so its bake is of the same settings. One worker at a time;
  ended with the game (Linux's parent death signal, a Windows job).
- Which levels: the campaign's ten whose maps there are, in this order: the
  one WAIT chose (below), the one played last, the saved game's, the one
  after the last finished, then the campaign's order.
- What is baked is noted beside the bake (<folder>/<map>/baked.txt: the
  map's BSPs, and each BSP baked with its key), by whichever process
  finished it, the worker or a game played (prism_bake_note, each frame): a
  BSP whose note's key is not the settings' (prism_bake_key: the baker's
  version, display.bake_quality, the rig's class, prism_rt_alpha; and the
  BSP's own geometry, geometry_token: stock, or its remodel's file) is
  baked again; a setting that only scales or tints a light (the sky's
  light, the rig's scales) is recombined from the layered bake as it
  loads, with no rebake. The baker itself
  checks the whole key as it loads a bake (the map, its geometry, its
  lights): a bake this note calls good that is not is baked again as the
  level is played.
- Resumed across starts: the bake file is a log the baker resumes at its
  next unfinished job, so a worker ended (the game quit, a level started)
  loses at most the work in flight.
- On the main menu: lines under the menu (the level and its progress, a
  bar, the time left by the baker's estimate and the levels left); the main
  menu's frames held to 30 a second while a worker bakes, so the GPU is
  the bake's (prism_bake_menu_frame_cap, sdl_platform.c). A level started
  stops the worker; the game's own baker goes on with that level in the
  background as it is played.
- The questions (menu_functions.c's, the hand-written pop-ups
  screens/bake_play.xml, bake_rebake.xml, bake_rebake_all.xml): a level not
  baked yet as it starts (PLAY NOW with Classic's light until its bake is
  done, or WAIT: the menu bakes it first); a change of a setting the bake
  holds (REBAKE, or UNDO); Light Bake's REBAKE ALL.

Nothing here changes what the game simulates, and nothing of it runs in
Classic ("ce"). The worker's game is a separate process whose clock stands
still; it writes only its bake, its note and its progress.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "tag_files/tag_files.h"
#include "game/game.h"
#include "hs/hs.h"
#include "interface/interface.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "text/draw_string.h"
#include "text/font_group.h"

#include "../src/xgpu_stream.h"
#include "../src/posix.h"
#include "../src/update.h"
#include "prism_bake_menu.h"
#include "prism_identity.h"
#include "prism_world_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the bake folder's files by their host paths, as the renderer names them:
the C library's own, not port/linux/include/stdio.h's, which read a path as
the Xbox's and put it under the data folder) */
#undef fopen
#undef remove
#undef rename

/* ---------- prototypes */

char const *config_string(char const *name);
long config_integer(char const *name);
int config_boolean(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
char const *platform_data_root(void);
char const *platform_save_root(void);
unsigned long system_milliseconds(void);
int halo_prism_rt_available(void);
boolean prism_bake_wanted(void);
unsigned long prism_bake_status(int word);
float prism_bake_status_real(int word);
unsigned long prism_scene_static_generation(void);
int prism_scene_static_empty(void);
void draw_quad(rectangle2d *rectangle, pixel32 color);
/* menu_functions.c's: the saved game's level and the one after the last
finished, player 1's profile's (NONE for none) */
void pc_menu_bake_levels(short *saved, short *next);
/* menu_functions.c's: the question asked opened (a pop-up), between frames */
void pc_menu_bake_questions_open(void);

/* ---------- constants */

#define BAKE_LEVELS 10
#define BAKE_BSPS 16
#define BAKE_PATH 600
#define BAKE_KEY 256
/* the levels' notes read again this often, the worker's progress this often */
#define NOTES_MILLISECONDS 2000ul
#define PROGRESS_MILLISECONDS 500ul
/* a worker that writes no progress for this long is ended (and its level
left until the next start) */
#define WORKER_SILENT_MILLISECONDS 240000ul
/* the main menu's frames while a worker bakes */
#define MENU_FRAME_CAP 30
/* the worker's GPU time a frame (prism_bake.c's control) */
#define WORKER_BUDGET_MILLISECONDS 40.0f
/* the lines' foot: this share of the screen's height over its foot */
#define LINE_FOOT_FRACTION 0.04f
#define BAR_WIDTH_FRACTION 0.40f
#define BAR_HEIGHT 3
/* "PRISM light baked" shown this long once the last level is done */
#define DONE_MILLISECONDS 6000ul
/* draw_string.c's centred text */
#define TEXT_CENTRED 2

enum
{
	LEVEL_UNKNOWN,
	LEVEL_NO_MAP,
	LEVEL_NOT_BAKED,
	LEVEL_PARTLY,
	LEVEL_BAKED,
	LEVEL_FAILED,
	/* every BSP's light the release's shipped bake (or baked here) */
	LEVEL_SHIPPED,
};


/* the campaign's levels, in its order (main.c's scenario_paths) */
static struct
{
	char const *map;
	char const *name;
} const bake_levels[BAKE_LEVELS] =
{
	{ "a10", "The Pillar of Autumn" }, { "a30", "Halo" }, { "a50", "The Truth and Reconciliation" },
	{ "b30", "The Silent Cartographer" }, { "b40", "Assault on the Control Room" }, { "c10", "343 Guilty Spark" },
	{ "c20", "The Library" }, { "c40", "Two Betrayals" }, { "d20", "Keyes" }, { "d40", "The Maw" },
};

/* ---------- globals */

static struct
{
	/* the settings, read again as they change */
	unsigned long changes;
	boolean wanted;
	char folder[BAKE_PATH];
	char key[BAKE_KEY];
	/* the shipped bake's folder (a release's, read only) and the world
	whose set of it is drawn ("stock", "remodel") */
	char shipped[BAKE_PATH];
	char world[16];
	/* the levels: their state, BSPs and those baked; when read; whether
	every BSP the shipped bake gives was seen to match this machine's maps
	(shipped.txt's "used": else the worker checks it, last) */
	int state[BAKE_LEVELS];
	int bsps[BAKE_LEVELS], baked[BAKE_LEVELS];
	boolean verified[BAKE_LEVELS];
	/* (a level the worker checked this start: not checked again) */
	boolean checked[BAKE_LEVELS];
	unsigned long notes_read;
	/* the level WAIT chose (first), NONE */
	short first;
	/* the worker: its handle, level, when it began and last told; its
	progress (its BSP, of how many, the share, seconds left: -1 unknown) */
	int worker;
	short worker_level;
	boolean worker_checks;
	unsigned long worker_began, worker_heard, progress_read;
	int progress_bsp, progress_bsps;
	float progress, seconds_left;
	char progress_state[16];
	/* the main menu drawn (its last frame's time), and the last level done */
	unsigned long main_shown;
	unsigned long all_done_at;
	boolean was_baking;
	/* the BSP last noted baked (prism_bake_note): its map, index and scene */
	char noted_map[64];
	short noted_bsp;
	unsigned long noted_generation;
} bake_menu = { (unsigned long)-1, FALSE, "", "", "", "", { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, 0, NONE, 0, NONE };

/* the worker's own state (debug.bake_worker) */
static struct
{
	unsigned long changes;
	char map[64];
	boolean paused;
	short target;
	unsigned long written;
	unsigned long switch_asked;
	/* debug.bake_bsp: the one BSP baked (a farm's job a BSP), -1 all; the
	BSPs that failed, each passed over so that the next is still baked */
	short only;
	short failed;
} bake_worker = { (unsigned long)-1, "", FALSE, 0, 0, 0, -1, 0 };

/* ---------- private code */

static void folder_read(char *folder, unsigned long size)
{
	char const *value = config_string("display.bake_folder");

	if (value && *value)
		snprintf(folder, size, "%s", value);
	else
		snprintf(folder, size, "%s/prism_bake", platform_data_root());
}

/* the shipped bake's folder (prism_bake.c's alike) */
static void shipped_folder_read(char *folder, unsigned long size)
{
	char const *value = config_string("display.bake_shipped_folder");

	if (value && *value)
		snprintf(folder, size, "%s", value);
	else
		snprintf(folder, size, "%s/prism_bake_shipped", platform_data_root());
}

/* the map's name as the baker names its folder (xgpu_vulkan_bake.c's
name_clean): the file's name, its letters, digits and '-', '_' else */
static void map_folder_name(char const *map, char *name, unsigned long size)
{
	char const *part;
	unsigned long index;

	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
			map = part + 1;
	}
	for (index = 0; map[index] && index + 1 < size; index++)
	{
		char c = map[index];

		name[index] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
	}
	name[index] = 0;
}

static short level_of_map(char const *map)
{
	char name[64];
	short level;

	if (!map)
		return NONE;
	map_folder_name(map, name, sizeof(name));
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		if (!strcmp(name, bake_levels[level].map))
			return level;
	}
	return NONE;
}

static boolean file_exists(char const *path)
{
	struct posix_file_information information;

	return posix_stat(path, &information) == 0;
}

/* a BSP's geometry as the note keeps it (per BSP, so that a change of the
world's geometry bakes again only the BSPs it changes): "geo:prsx:<the BSP's
enhanced world_id, 64 hex digits>:<tag>" where the enhanced world is drawn
for the BSP drawn (prism_world_source.c: the map's sidecar), else
"geo:stock:<tag>"; its tag's path with / for \ (the note keeps the rest of a
line as the key, so a space in it is kept) */
static void geometry_token(char const *tag, char *token, unsigned long size)
{
	char name[160];
	unsigned long k;
	long tag_index = global_structure_bsp_tag_index_get();
	char const *drawn = tag_index != NONE ? tag_get_name(tag_index) : NULL;

	snprintf(name, sizeof(name), "%s", tag ? tag : "");
	for (k = 0; name[k]; k++)
	{
		if (name[k] == '\\')
			name[k] = '/';
	}
	if (name[0] && drawn && prism_world_enhanced())
	{
		char drawn_name[160], hex[65];
		unsigned char id[32];

		snprintf(drawn_name, sizeof(drawn_name), "%s", drawn);
		for (k = 0; drawn_name[k]; k++)
		{
			if (drawn_name[k] == '\\')
				drawn_name[k] = '/';
		}
		if (!strcmp(drawn_name, name))
		{
			prism_world_source_world_id(global_structure_bsp_index_get(), id);
			prism_id_hex(id, hex);
			snprintf(token, size, "geo:prsx:%s:%s", hex, name);
			return;
		}
	}
	snprintf(token, size, "geo:stock:%s", name);
}

/* a BSP's whole key: the settings' and its geometry's */
static void bsp_key(char const *base, char const *tag, char *key, unsigned long size)
{
	char token[BAKE_KEY];

	geometry_token(tag, token, sizeof(token));
	snprintf(key, size, "%s %s", base, token);
}

/* whether a BSP's noted key is good for the settings now: its settings' key
the same, and its geometry the same (its token made again from the tag it
names). An older note's key, with no geometry, is not */
/* a quality's rank (draft 0 to max 3; -1 none) */
static int quality_rank(char const *word, unsigned long length)
{
	static char const *const words[] = { "draft", "high", "ultra", "max" };
	int rank;

	for (rank = 0; rank < 4; rank++)
	{
		if (strlen(words[rank]) == length && !strncmp(word, words[rank], length))
			return rank;
	}
	return -1;
}

/* whether a settings' key noted ("v19 max units cutouts") is good for the
settings' now: the same but for its quality, which may be better (a bake of
max is good where ultra is asked: the baker takes it, xgpu_vulkan_bake.c's
file_whole) */
static boolean base_key_good(char const *noted, unsigned long noted_length, char const *base)
{
	char const *noted_quality = strchr(noted, ' '), *base_quality = strchr(base, ' ');
	char const *noted_rest, *base_rest;
	int noted_rank, base_rank;

	if (!noted_quality || !base_quality || noted_quality - noted != base_quality - base ||
		strncmp(noted, base, (unsigned long)(base_quality - base)) ||
		(unsigned long)(noted_quality - noted) >= noted_length)
	{
		return FALSE;
	}
	noted_quality++;
	base_quality++;
	noted_rest = strchr(noted_quality, ' ');
	base_rest = strchr(base_quality, ' ');
	if (!noted_rest || !base_rest || (unsigned long)(noted_rest - noted) > noted_length)
		return FALSE;
	noted_rank = quality_rank(noted_quality, (unsigned long)(noted_rest - noted_quality));
	base_rank = quality_rank(base_quality, (unsigned long)(base_rest - base_quality));
	if (noted_rank < 0 || base_rank < 0 || noted_rank < base_rank)
		return FALSE;
	return noted_length - (unsigned long)(noted_rest - noted) == (unsigned long)strlen(base_rest) &&
		!strncmp(noted_rest, base_rest, (unsigned long)strlen(base_rest));
}

static boolean bsp_key_good(char const *noted, char const *base)
{
	char const *token = strstr(noted, " geo:");
	char tag[160], now[BAKE_KEY + 8];
	char const *at;
	char const *now_token;

	if (!token || !base_key_good(noted, (unsigned long)(token - noted), base))
		return FALSE;
	token++;
	/* (the tag: after "geo:stock:", or after "geo:prsx:<id>:"; an older
	note's "geo:pwg:" is of a remodel no longer read) */
	if (!strncmp(token, "geo:stock:", 10))
		at = token + 10;
	else if (!strncmp(token, "geo:prsx:", 9) && strlen(token) > 9 + 64 && token[9 + 64] == ':')
		at = token + 9 + 64 + 1;
	else
		return FALSE;
	snprintf(tag, sizeof(tag), "%s", at);
	bsp_key(base, tag, now, sizeof(now));
	now_token = strstr(now, " geo:");
	return now_token && !strcmp(now_token, token - 1);
}

/* a file written whole or not at all (written beside it, then renamed over
it: Windows renames over no file, so it goes first) */
static boolean file_write(char const *path, char const *text)
{
	char temporary[BAKE_PATH + 8];
	FILE *file;
	boolean written;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
		return FALSE;
	written = fputs(text, file) >= 0;
	written = fclose(file) == 0 && written;
	if (written && rename(temporary, path) != 0)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

/* the note of the map's BSPs baked (baked.txt): its BSPs (0 unknown) and
each BSP's key (empty: none) */
static int note_read(char const *map, char keys[BAKE_BSPS][BAKE_KEY])
{
	char path[BAKE_PATH + 96], name[64], line[BAKE_KEY + 32];
	FILE *file;
	int bsps = 0, index;

	for (index = 0; index < BAKE_BSPS; index++)
		keys[index][0] = 0;
	map_folder_name(map, name, sizeof(name));
	snprintf(path, sizeof(path), "%s/%s/baked.txt", bake_menu.folder, name);
	file = fopen(path, "rb");
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file))
	{
		int length = (int)strlen(line);

		while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
			line[--length] = 0;
		if (!strncmp(line, "bsps ", 5))
			bsps = atoi(line + 5);
		else if (!strncmp(line, "bsp", 3) && line[3] >= '0' && line[3] <= '9')
		{
			char *space = strchr(line, ' ');

			index = atoi(line + 3);
			if (space && index >= 0 && index < BAKE_BSPS)
				snprintf(keys[index], BAKE_KEY, "%s", space + 1);
		}
	}
	fclose(file);
	return bsps > 0 && bsps <= BAKE_BSPS ? bsps : 0;
}

static void note_write(char const *map, int bsps, char keys[BAKE_BSPS][BAKE_KEY])
{
	char path[BAKE_PATH + 96], name[64], text[BAKE_BSPS * (BAKE_KEY + 16) + 64];
	unsigned long length;
	int index;

	map_folder_name(map, name, sizeof(name));
	snprintf(path, sizeof(path), "%s/%s", bake_menu.folder, name);
	posix_make_directory(bake_menu.folder);
	posix_make_directory(path);
	snprintf(path, sizeof(path), "%s/%s/baked.txt", bake_menu.folder, name);
	length = (unsigned long)snprintf(text, sizeof(text), "bsps %d\n", bsps);
	for (index = 0; index < BAKE_BSPS && length < sizeof(text); index++)
	{
		if (keys[index][0])
			length += (unsigned long)snprintf(text + length, sizeof(text) - length, "bsp%d %s\n", index, keys[index]);
	}
	if (!file_write(path, text))
		platform_log("prism bake: could not write %s", path);
}

/* ---------- the shipped bake (docs/prism_bake.md, "The shipped bake") */

/* whether a BSP's key in the shipped bake's manifest ("v19 max units
cutouts geo:stock", or "geo:pwg" for a remodel's BSP, each with ":<tag>"
where the export knew the BSP's tag) is good for the settings now and the
BSP's geometry here (its tag's remodel, where it names one; the remodel's
file is the player's own build, which the baker checks by its hash as it
loads) */
static boolean shipped_key_good(char const *shipped, char const *base)
{
	char const *token = strstr(shipped, " geo:");
	char tag[160], now[BAKE_KEY];
	boolean remodel;

	if (!token || !base_key_good(shipped, (unsigned long)(token - shipped), base))
		return FALSE;
	token++;
	if (!strncmp(token, "geo:stock", 9) && (token[9] == 0 || token[9] == ':'))
		remodel = FALSE;
	else if (!strncmp(token, "geo:prsx:", 9))
		remodel = TRUE;
	else
		return FALSE;
	/* (no tag: the stock world's BSPs are stock) */
	if (!remodel && !strchr(token + 4, ':'))
		return TRUE;
	if (remodel)
	{
		/* (the enhanced world's: its world_id and tag must be the BSP's
		drawn now, the sidecar's opened) */
		char const *colon = strlen(token) > 9 + 64 ? token + 9 + 64 : NULL;

		if (!colon || *colon != ':')
			return FALSE;
		snprintf(tag, sizeof(tag), "%s", colon + 1);
		geometry_token(tag, now, sizeof(now));
		return !strcmp(now, token);
	}
	snprintf(tag, sizeof(tag), "%s", strchr(token + 4, ':') + 1);
	geometry_token(tag, now, sizeof(now));
	return !strncmp(now, "geo:stock:", 10);
}

/* the shipped bake's manifest's BSPs of the map in the world drawn: its
BSPs (0 none) and each one's key */
static int shipped_read(char const *map, char keys[BAKE_BSPS][BAKE_KEY])
{
	char path[BAKE_PATH + 32], line[BAKE_KEY + 96], prefix[96];
	FILE *file;
	int bsps = 0, index;
	unsigned long prefix_length;

	for (index = 0; index < BAKE_BSPS; index++)
		keys[index][0] = 0;
	if (!bake_menu.shipped[0])
		return 0;
	snprintf(path, sizeof(path), "%s/manifest.txt", bake_menu.shipped);
	file = fopen(path, "rb");
	if (!file)
		return 0;
	/* ("bsp <world> <map> <index> <of> <key>") */
	snprintf(prefix, sizeof(prefix), "bsp %s %s ", bake_menu.world, map);
	prefix_length = (unsigned long)strlen(prefix);
	while (fgets(line, sizeof(line), file))
	{
		char *at, *key;
		int of;

		line[strcspn(line, "\r\n")] = 0;
		if (strncmp(line, prefix, prefix_length))
			continue;
		at = line + prefix_length;
		index = atoi(at);
		at = strchr(at, ' ');
		if (!at)
			continue;
		of = atoi(at + 1);
		key = strchr(at + 1, ' ');
		if (!key || index < 0 || index >= BAKE_BSPS || of <= 0 || of > BAKE_BSPS)
			continue;
		bsps = of;
		snprintf(keys[index], BAKE_KEY, "%s", key + 1);
	}
	fclose(file);
	return bsps;
}

/* what the baker found of the map's shipped bake here (<folder>/<map>/
shipped.txt, xgpu_vulkan_bake.c's shipped_note), in the world drawn: each
BSP's 1 used, -1 not this machine's (baked here instead), 0 not seen yet */
static void shipped_seen(char const *map, int seen[BAKE_BSPS])
{
	char path[BAKE_PATH + 96], name[64], line[160], prefix[32];
	FILE *file;
	int index;

	for (index = 0; index < BAKE_BSPS; index++)
		seen[index] = 0;
	map_folder_name(map, name, sizeof(name));
	snprintf(path, sizeof(path), "%s/%s/shipped.txt", bake_menu.folder, name);
	file = fopen(path, "rb");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		for (index = 0; index < BAKE_BSPS; index++)
		{
			snprintf(prefix, sizeof(prefix), "bsp%d %s ", index, bake_menu.world);
			if (!strncmp(line, prefix, strlen(prefix)))
				seen[index] = !strncmp(line + strlen(prefix), "used", 4) ? 1 : -1;
		}
	}
	fclose(file);
}

/* whether the shipped bake has every campaign level here of a world
("stock", "remodel"): a change of the world's geometry then bakes nothing */
static boolean shipped_world_whole(char const *world)
{
	char keep[16], keys[BAKE_BSPS][BAKE_KEY], path[BAKE_PATH + 96];
	short level;
	boolean whole = TRUE;

	snprintf(keep, sizeof(keep), "%s", bake_menu.world);
	snprintf(bake_menu.world, sizeof(bake_menu.world), "%s", world);
	for (level = 0; level < BAKE_LEVELS && whole; level++)
	{
		snprintf(path, sizeof(path), "%s/maps/%s.map", platform_data_root(), bake_levels[level].map);
		if (file_exists(path) && !shipped_read(bake_levels[level].map, keys))
			whole = FALSE;
	}
	snprintf(bake_menu.world, sizeof(bake_menu.world), "%s", keep);
	return whole;
}

static void settings_read(void)
{
	char const *lighting;
	char key[BAKE_KEY];

	if (bake_menu.changes == config_changes())
		return;
	bake_menu.changes = config_changes();
	folder_read(bake_menu.folder, sizeof(bake_menu.folder));
	shipped_folder_read(bake_menu.shipped, sizeof(bake_menu.shipped));
	{
		char const *world = prism_world_enhanced() ? "remodel" : "stock";

		if (strcmp(world, bake_menu.world))
		{
			snprintf(bake_menu.world, sizeof(bake_menu.world), "%s", world);
			bake_menu.notes_read = 0;
		}
	}
	prism_bake_key(key, sizeof(key));
	if (strcmp(key, bake_menu.key))
	{
		snprintf(bake_menu.key, sizeof(bake_menu.key), "%s", key);
		/* (the notes read again at once: their keys against the new one) */
		bake_menu.notes_read = 0;
	}
	lighting = config_string("display.lighting");
	bake_menu.wanted = lighting && (!strcmp(lighting, "raster") || !strcmp(lighting, "pathtraced")) &&
		strcmp(config_string("display.bake"), "off") != 0 && !prism_bake_worker_map();
}

/* each level's state from the maps folder, its note and the shipped bake:
a BSP is ready where its note's key is good (baked here, or the shipped
bake drawn and noted), or where the shipped bake's manifest has it for the
world drawn and the baker has not found it to differ from this machine's
maps (shipped.txt); a level of shipped BSPs is SHIPPED, else BAKED as
before */
static void levels_read(void)
{
	char keys[BAKE_BSPS][BAKE_KEY], shipped[BAKE_BSPS][BAKE_KEY];
	char path[BAKE_PATH + 96];
	int seen[BAKE_BSPS];
	short level;

	bake_menu.notes_read = system_milliseconds() | 1;
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		int bsps, index, baked = 0;

		if (bake_menu.state[level] == LEVEL_FAILED)
			continue;
		snprintf(path, sizeof(path), "%s/maps/%s.map", platform_data_root(), bake_levels[level].map);
		if (!file_exists(path))
		{
			bake_menu.state[level] = LEVEL_NO_MAP;
			continue;
		}
		bsps = note_read(bake_levels[level].map, keys);
		{
			int shipped_bsps = shipped_read(bake_levels[level].map, shipped), from_shipped = 0, checked = 0;

			if (shipped_bsps && (!bsps || bsps == shipped_bsps))
			{
				bsps = shipped_bsps;
				shipped_seen(bake_levels[level].map, seen);
			}
			else
				shipped_bsps = 0;
			for (index = 0; index < bsps; index++)
			{
				boolean shipped_good = shipped_bsps && seen[index] >= 0 && shipped_key_good(shipped[index],
					bake_menu.key);

				if (shipped_good && seen[index] > 0)
					checked++;
				if (bsp_key_good(keys[index], bake_menu.key))
					baked++;
				else if (shipped_good)
				{
					baked++;
					from_shipped++;
				}
			}
			bake_menu.verified[level] = !shipped_bsps || checked == bsps || !from_shipped;
			bake_menu.bsps[level] = bsps;
			bake_menu.baked[level] = baked;
			bake_menu.state[level] = bsps && baked == bsps ? (shipped_bsps && (from_shipped || checked) ? LEVEL_SHIPPED :
				LEVEL_BAKED) : baked ? LEVEL_PARTLY : LEVEL_NOT_BAKED;
		}
	}
}

static boolean level_needs(short level)
{
	return level != NONE && (bake_menu.state[level] == LEVEL_NOT_BAKED || bake_menu.state[level] == LEVEL_PARTLY);
}

/* the level played last, as campaign_start noted it */
static short last_played(void)
{
	char path[BAKE_PATH + 32], line[64];
	FILE *file;
	short level = NONE;

	snprintf(path, sizeof(path), "%s/last_played.txt", bake_menu.folder);
	file = fopen(path, "rb");
	if (!file)
		return NONE;
	if (fgets(line, sizeof(line), file))
	{
		line[strcspn(line, "\r\n")] = 0;
		level = level_of_map(line);
	}
	fclose(file);
	return level;
}

/* the next level to bake, in the order above; NONE when none needs it */
static short level_next(void)
{
	short order[4], saved, next, level;
	int index;

	pc_menu_bake_levels(&saved, &next);
	order[0] = bake_menu.first;
	order[1] = last_played();
	order[2] = saved;
	order[3] = next;
	for (index = 0; index < 4; index++)
	{
		if (order[index] >= 0 && order[index] < BAKE_LEVELS && level_needs(order[index]))
			return order[index];
	}
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		if (level_needs(level))
			return level;
	}
	/* (last, a shipped level not yet checked against this machine's maps:
	the worker loads it, the baker takes each BSP's shipped bake whose key
	is this machine's at once and bakes any other BSP here, that BSP alone) */
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		if (bake_menu.state[level] == LEVEL_SHIPPED && !bake_menu.verified[level] && !bake_menu.checked[level])
			return level;
	}
	return NONE;
}

static void progress_path(char *path, unsigned long size)
{
	snprintf(path, size, "%s/menu_worker.txt", bake_menu.folder);
}

static void progress_read(void)
{
	char path[BAKE_PATH + 32], line[128];
	FILE *file;

	bake_menu.progress_read = system_milliseconds() | 1;
	progress_path(path, sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "map ", 4) && level_of_map(line + 4) != bake_menu.worker_level)
			break;
		if (!strncmp(line, "serial ", 7))
		{
			static char last[32];

			if (strcmp(last, line + 7))
			{
				snprintf(last, sizeof(last), "%s", line + 7);
				bake_menu.worker_heard = system_milliseconds();
			}
		}
		else if (!strncmp(line, "state ", 6))
			snprintf(bake_menu.progress_state, sizeof(bake_menu.progress_state), "%s", line + 6);
		else if (!strncmp(line, "bsp ", 4))
			bake_menu.progress_bsp = atoi(line + 4);
		else if (!strncmp(line, "bsps ", 5))
			bake_menu.progress_bsps = atoi(line + 5);
		else if (!strncmp(line, "progress ", 9))
			bake_menu.progress = (float)atof(line + 9);
		else if (!strncmp(line, "left ", 5))
			bake_menu.seconds_left = (float)atof(line + 5);
	}
	fclose(file);
}

#if defined(HALO_ANDROID) || defined(HALO_MACOS)
/* (no worker: the Android app's and the Mac's renderers bake nothing) */
static int worker_start(char const *executable, char const *environment)
{
	(void)executable;
	(void)environment;
	return 0;
}
#define update_child_state(handle) 255
#define update_child_stop(handle) ((void)(handle))
#define update_executable_path(path, size) 0
#else
static int worker_start(char const *executable, char const *environment)
{
	return update_child_start(executable, environment);
}
#endif

/* the worker started on the level: the game again, hidden and silent, its
map the level's, its saves and pipelines its own */
static void worker_begin(short level)
{
	static char environment[4096];
	char executable[1024], saves[BAKE_PATH], path[BAKE_PATH + 32];
	unsigned long length = 0;
	char const *variables[48];
	char cache[BAKE_PATH + 32], logs[BAKE_PATH + 32], window[32];
	int count = 0, index;

	if (!update_executable_path(executable, sizeof(executable)))
	{
		platform_log("prism bake: the main menu's bake cannot start (this game's executable is not known)");
		bake_menu.state[level] = LEVEL_FAILED;
		return;
	}
	snprintf(saves, sizeof(saves), "%s/bake_worker", platform_save_root());
	posix_make_directory(saves);
	snprintf(cache, sizeof(cache), "HALO_VULKAN_PIPELINE_CACHE=%s/vulkan_pipelines.bin", saves);
	snprintf(path, sizeof(path), "HALO_SAVE_ROOT=%s", saves);
	snprintf(logs, sizeof(logs), "HALO_LOG_ROOT=%s", saves);
	snprintf(window, sizeof(window), "HALO_WINDOW_SIZE=640x360");
	{
		static char map[80];

		snprintf(map, sizeof(map), "HALO_BAKE_WORKER=%s", bake_levels[level].map);
		variables[count++] = map;
	}
	variables[count++] = path;
	variables[count++] = cache;
	variables[count++] = logs;
	variables[count++] = window;
	variables[count++] = "HALO_HIDDEN_WINDOW=1";
	variables[count++] = "HALO_FULLSCREEN=0";
	variables[count++] = "HALO_DISPLAY_MODE=windowed";
	variables[count++] = "HALO_NO_VSYNC=1";
	variables[count++] = "HALO_MAX_FPS=0";
	variables[count++] = "HALO_NO_AUDIO=1";
	variables[count++] = "SDL_AUDIODRIVER=dummy";
	variables[count++] = "SDL_AUDIO_DRIVER=dummy";
	variables[count++] = "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000";
	variables[count++] = "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0";
	variables[count++] = "SDL_JOYSTICK_HIDAPI=0";
	variables[count++] = "HALO_VOICE_CHAT=off";
	variables[count++] = "HALO_UPDATE_AUTO=false";
	variables[count++] = "HALO_NO_CLIPBOARD=1";
	variables[count++] = "HALO_NET_ONLINE=false";
	variables[count++] = "HALO_NET_PUBLIC_LOBBY=false";
	variables[count++] = "HALO_NET_MAP_SHARE=false";
	variables[count++] = "HALO_NET_MAP_SEED=false";
	variables[count++] = "HALO_NET_MAP_DOWNLOAD=false";
	variables[count++] = "HALO_NET_JOIN_FROM_CLIPBOARD=false";
	variables[count++] = "HALO_DISCORD_APPLICATION=";
	variables[count++] = "HALO_CRASH_REPORTS=no";
	variables[count++] = "HALO_SKIP_CINEMATICS=1";
	variables[count++] = "HALO_MOVIES=false";
	variables[count++] = "HALO_MENU_PICTURES=false";
	variables[count++] = "HALO_AI_MENDICANT=false";
	variables[count++] = "HALO_BAKE=auto";
	variables[count++] = "HALO_BAKE_EXIT=false";
	variables[count++] = "HALO_BAKE_INDICATOR=false";
	variables[count++] = "HALO_EXIT_AFTER=0";
	variables[count++] = "HALO_CAMERA_PATH=";
	variables[count++] = "HALO_TEST_INPUT=";
	variables[count++] = "HALO_MENU_OPEN=";
	variables[count++] = "HALO_SCREENSHOT_EVERY=0";
	for (index = 0; index < count; index++)
	{
		unsigned long size = (unsigned long)strlen(variables[index]) + 1;

		if (length + size + 1 >= sizeof(environment))
			break;
		memcpy(environment + length, variables[index], size);
		length += size;
	}
	environment[length] = 0;
	progress_path(path, sizeof(path));
	remove(path);
	bake_menu.worker = worker_start(executable, environment);
	if (!bake_menu.worker)
	{
		platform_log("prism bake: the main menu's bake of %s could not start", bake_levels[level].map);
		bake_menu.state[level] = LEVEL_FAILED;
		return;
	}
	bake_menu.worker_level = level;
	bake_menu.worker_checks = bake_menu.state[level] == LEVEL_SHIPPED;
	bake_menu.worker_began = bake_menu.worker_heard = system_milliseconds();
	bake_menu.progress = 0.0f;
	bake_menu.seconds_left = -1.0f;
	bake_menu.progress_bsp = 0;
	bake_menu.progress_bsps = bake_menu.bsps[level];
	snprintf(bake_menu.progress_state, sizeof(bake_menu.progress_state), "loading");
	bake_menu.was_baking = !bake_menu.worker_checks;
	platform_log("prism bake: the main menu %s %s (%s) in a process of its own (%s)", bake_menu.worker_checks ?
		"checks the shipped light of" : "bakes", bake_levels[level].map, bake_levels[level].name, bake_menu.key);
}

/* the worker's end: done, failed (its level left until the next start), or
ended by us (its level resumed later) */
static void worker_ended(int code, boolean stopped)
{
	short level = bake_menu.worker_level;

	bake_menu.worker = 0;
	bake_menu.worker_level = NONE;
	if (level == NONE)
		return;
	if (bake_menu.worker_checks && !stopped)
		bake_menu.checked[level] = TRUE;
	if (stopped)
		platform_log("prism bake: the main menu's bake of %s paused (it resumes where it stopped)",
			bake_levels[level].map);
	else if (code == 0)
	{
		platform_log("prism bake: the main menu baked %s in %.0f s", bake_levels[level].map,
			(double)(system_milliseconds() - bake_menu.worker_began) / 1000.0);
		/* (its note read again: a worker done whose level the note does not
		call baked is not started again until the next start) */
		levels_read();
		if (bake_menu.state[level] == LEVEL_SHIPPED && !bake_menu.verified[level])
		{
			/* (its shipped.txt does not say every BSP used: not checked again
			until the next start; the baker checks each BSP as it is played) */
			platform_log("prism bake: %s's shipped light was not seen used for every BSP; left until the next start",
				bake_levels[level].map);
		}
		else if (level_needs(level))
		{
			platform_log("prism bake: %s's note (%s/%s/baked.txt) does not hold the bake just made; left until the "
				"next start", bake_levels[level].map, bake_menu.folder, bake_levels[level].map);
			bake_menu.state[level] = LEVEL_FAILED;
		}
	}
	else
	{
		platform_log("prism bake: the main menu's %s of %s failed (its process ended with %d; its log is in %s/"
			"bake_worker); left until the next start", bake_menu.worker_checks ? "check" : "bake", bake_levels[level].map,
			code, platform_save_root());
		/* (a check that failed leaves the shipped light as it is: the baker
		checks each BSP again as it is played) */
		if (!bake_menu.worker_checks)
			bake_menu.state[level] = LEVEL_FAILED;
	}
	if (bake_menu.first == level && !stopped)
		bake_menu.first = NONE;
	bake_menu.notes_read = 0;
}

static void worker_stop(void)
{
	int code;
	unsigned long began;

	if (!bake_menu.worker)
		return;
	update_child_stop(bake_menu.worker);
	/* (its end waited for, a little: a second's at most, then let go of;
	Linux's zombie is taken by the next state asked) */
	began = system_milliseconds();
	while ((code = update_child_state(bake_menu.worker)) < 0 && system_milliseconds() - began < 1000)
		;
	worker_ended(code, TRUE);
}

/* the main menu up (no game played) */
static boolean menu_up(void)
{
	return main_menu_is_active();
}

/* ---------- the worker's own (debug.bake_worker) */

static void worker_progress_write(char const *state)
{
	char path[BAKE_PATH + 32], text[512];
	short count = global_scenario_get() ? global_scenario_get()->structure_bsp_references.count : 0;

	progress_path(path, sizeof(path));
	snprintf(text, sizeof(text), "map %s\nserial %lu\nstate %s\nbsp %d\nbsps %d\nprogress %.4f\nleft %.0f\n",
		bake_worker.map, system_milliseconds(), state, (int)bake_worker.target, (int)count,
		(double)prism_bake_status_real(XGPU_BAKE_STATUS_PROGRESS),
		(double)prism_bake_status_real(XGPU_BAKE_STATUS_SECONDS_LEFT));
	file_write(path, text);
	bake_worker.written = system_milliseconds();
}

static void worker_frame(void)
{
	struct scenario *scenario;
	short bsp, count;
	unsigned long state;

	if (system_milliseconds() - bake_worker.written > PROGRESS_MILLISECONDS)
	{
		if (!game_time_initialized() || !game_in_progress() || main_menu_is_active())
			worker_progress_write("loading");
	}
	if (!game_time_initialized() || !game_in_progress() || main_menu_is_active() || !(scenario = global_scenario_get()))
		return;
	/* (the game's clock stopped: nothing plays while it bakes) */
	count = scenario->structure_bsp_references.count;
	if (!bake_worker.paused)
	{
		bake_worker.paused = TRUE;
		hs_compile_and_evaluate("game_speed 0");
		if (bake_worker.only >= 0)
		{
			platform_log("prism bake: the main menu's worker bakes %s's BSP %d alone (debug.bake_bsp; %d BSPs)",
				bake_worker.map, (int)bake_worker.only, (int)count);
			bake_worker.target = bake_worker.only < count ? bake_worker.only : count;
		}
		else
			platform_log("prism bake: the main menu's worker bakes %s's %d BSPs", bake_worker.map, (int)count);
	}
	bsp = global_structure_bsp_index_get();
	if (bake_worker.target >= count || (bake_worker.only >= 0 && bake_worker.target != bake_worker.only))
	{
		if (bake_worker.only >= count)
		{
			worker_progress_write("failed");
			platform_log("prism bake: the main menu's worker: %s has no BSP %d (debug.bake_bsp): quitting",
				bake_worker.map, (int)bake_worker.only);
			exit(2);
		}
		if (bake_worker.failed)
		{
			worker_progress_write("failed");
			platform_log("prism bake: the main menu's worker baked %s but for %d BSPs that failed: quitting",
				bake_worker.map, (int)bake_worker.failed);
			exit(3);
		}
		worker_progress_write("done");
		platform_log("prism bake: the main menu's worker baked %s: quitting", bake_worker.map);
		exit(0);
	}
	if (bsp != bake_worker.target)
	{
		if (!bake_worker.switch_asked || system_milliseconds() - bake_worker.switch_asked > 10000)
		{
			bake_worker.switch_asked = system_milliseconds() | 1;
			scenario_switch_structure_bsp(bake_worker.target);
		}
		if (system_milliseconds() - bake_worker.written > PROGRESS_MILLISECONDS)
			worker_progress_write("loading");
		return;
	}
	bake_worker.switch_asked = 0;
	if (prism_scene_static_empty())
	{
		/* (nothing to bake: noted by prism_bake_note, this frame) */
		platform_log("prism bake: the main menu's worker: %s's BSP %d has no lightmapped surface: nothing to bake",
			bake_worker.map, (int)bsp);
		bake_worker.target++;
		worker_progress_write("baking");
		return;
	}
	state = prism_bake_status(XGPU_BAKE_STATUS_STATE);
	if ((long)prism_bake_status(XGPU_BAKE_STATUS_BSP) == (long)bsp &&
		prism_bake_status(XGPU_BAKE_STATUS_GENERATION) == prism_scene_static_generation())
	{
		if (state == XGPU_BAKE_STATE_DONE)
		{
			/* (noted by prism_bake_note, this frame) */
			bake_worker.target++;
			worker_progress_write("baking");
			return;
		}
		if (state == XGPU_BAKE_STATE_FAILED)
		{
			/* (passed over: the map's other BSPs are still baked, and the
			worker ends failed once they are) */
			bake_worker.failed++;
			bake_worker.target++;
			worker_progress_write("baking");
			platform_log("prism bake: the main menu's worker could not bake %s's BSP %d: on to the next", bake_worker.map,
				(int)bsp);
			return;
		}
	}
	if (system_milliseconds() - bake_worker.written > PROGRESS_MILLISECONDS)
		worker_progress_write(state == XGPU_BAKE_STATE_BAKING ? "baking" : "waiting");
}

/* ---------- public code */

/* the rig's class the bake holds: the fitted rig's sky lights test their
rays the whole level's length, the units rig's as far as their tags say
(transport); "auto" is the units rig under both PRISM lightings */
static char const *rig_class(char const *rig)
{
	return rig && !strcmp(rig, "fitted") ? "fitted" : "units";
}

/* the settings every BSP's bake holds, as the note keeps them: the baker's
version, the quality (at least: a better bake is good, base_key_good), the
rig's class, the cutouts (always traced in the bake: prism_rt.c's
level_alpha_traced, so Cutout Shadows never rebakes). Not the sky's light,
the rig's scales or the lamps' brightness: the layered bake recombines its
layers by those as it loads (docs/prism_bake.md, "The layers"), with no
rebake. The level's geometry is each BSP's own (geometry_token) */
void prism_bake_key(char *key, unsigned long size)
{
	char const *quality = config_string("display.bake_quality");

	snprintf(key, size, "v%u %s %s cutouts", (unsigned)XGPU_BAKE_VERSION,
		quality && quality_rank(quality, (unsigned long)strlen(quality)) >= 0 ? quality : "ultra",
		rig_class(config_string("display.prism_rig")));
}

/* whether a change of a setting from before to after changes what the bake
holds (a REBAKE asked): the world's geometry where the shipped bake has
not every level of the world chosen (it ships both worlds: switching is
instant then), and the rig's class (config.toml's alone: the units rig is
every player's). Not the quality (config.toml's and the console's alone: a
release ships MAX, and a better bake is good for every quality), not the
cutouts (always in the bake) and not the sky's light (recombined) */
boolean prism_bake_key_setting(char const *name, char const *before, char const *after)
{
	if (!name)
		return FALSE;
	if (!strcmp(name, "display.prism_rig"))
		return strcmp(rig_class(before), rig_class(after)) != 0;
	if (!strcmp(name, "display.prism_world_geometry"))
	{
		settings_read();
		return !shipped_world_whole(after && !strcmp(after, "remodel") ? "remodel" : "stock");
	}
	return FALSE;
}

/* the main menu's bake runs (the lighting reads a bake, the card traces
rays, display.bake "auto") */
boolean prism_bake_menu_wanted(void)
{
	settings_read();
	return bake_menu.wanted && halo_prism_rt_available();
}

/* debug.bake_worker's map, or NULL: this game is a worker */
char const *prism_bake_worker_map(void)
{
	if (bake_worker.changes != config_changes())
	{
		char const *value = config_string("debug.bake_worker");
		long only = config_integer("debug.bake_bsp");

		bake_worker.changes = config_changes();
		snprintf(bake_worker.map, sizeof(bake_worker.map), "%s", value ? value : "");
		bake_worker.only = only >= 0 && only < 32767 ? (short)only : -1;
	}
	return bake_worker.map[0] ? bake_worker.map : NULL;
}

/* the worker's GPU time a frame (prism_bake.c's control), 0 for a game */
float prism_bake_worker_budget(void)
{
	return prism_bake_worker_map() ? WORKER_BUDGET_MILLISECONDS : 0.0f;
}

/* each frame, the BSP drawn noted in its map's note once its bake is done
(the worker's, a game's) */
static void prism_bake_note(void)
{
	char keys[BAKE_BSPS][BAKE_KEY];
	char map[64];
	char const *loaded;
	struct scenario *scenario;
	short bsp;
	unsigned long generation;
	int bsps;

	if (!prism_bake_wanted() || !game_time_initialized() || !game_in_progress() || main_menu_is_active() ||
		!(scenario = global_scenario_get()) || (bsp = global_structure_bsp_index_get()) == NONE || bsp >= BAKE_BSPS)
	{
		return;
	}
	generation = prism_scene_static_generation();
	/* (a BSP with no lightmapped surface, a cinematic's, has nothing to
	bake: noted as baked, so that its level counts whole) */
	if (!prism_scene_static_empty() && (prism_bake_status(XGPU_BAKE_STATUS_STATE) != XGPU_BAKE_STATE_DONE ||
		(long)prism_bake_status(XGPU_BAKE_STATUS_BSP) != (long)bsp ||
		prism_bake_status(XGPU_BAKE_STATUS_GENERATION) != generation))
	{
		return;
	}
	loaded = cache_file_loaded_map_name();
	map_folder_name(loaded ? loaded : "", map, sizeof(map));
	if (!map[0] || (!strcmp(map, bake_menu.noted_map) && bsp == bake_menu.noted_bsp &&
		generation == bake_menu.noted_generation))
	{
		return;
	}
	snprintf(bake_menu.noted_map, sizeof(bake_menu.noted_map), "%s", map);
	bake_menu.noted_bsp = bsp;
	bake_menu.noted_generation = generation;
	settings_read();
	bsps = note_read(map, keys);
	{
		struct scenario_structure_bsp_reference const *reference = TAG_BLOCK_GET_ELEMENT(
			&scenario->structure_bsp_references, bsp, struct scenario_structure_bsp_reference);
		char const *tag = reference && reference->structure_bsp.index != NONE ?
			tag_get_name(reference->structure_bsp.index) : NULL;
		char key[BAKE_KEY];

		bsp_key(bake_menu.key, tag, key, sizeof(key));
		if (strcmp(keys[bsp], key) || bsps != scenario->structure_bsp_references.count)
		{
			snprintf(keys[bsp], BAKE_KEY, "%s", key);
			note_write(map, scenario->structure_bsp_references.count, keys);
			platform_log("prism bake: %s's BSP %d noted baked (%s)", map, (int)bsp, key);
		}
	}
}

/* between frames (d3d8_device.c's present): the note, the worker's own
work, and on the main menu the next level's worker started, watched and
ended */
void prism_bake_menu_frame(void)
{
	int code;

	settings_read();
	prism_bake_note();
	if (prism_bake_worker_map())
	{
		worker_frame();
		return;
	}
	pc_menu_bake_questions_open();
	/* (a worker running: watched; ended as a game starts or the bake is
	no more wanted) */
	if (bake_menu.worker)
	{
		if ((code = update_child_state(bake_menu.worker)) >= 0)
			worker_ended(code, FALSE);
		else if (!menu_up() || !prism_bake_menu_wanted())
			worker_stop();
		else
		{
			if (system_milliseconds() - bake_menu.progress_read > PROGRESS_MILLISECONDS)
				progress_read();
			if (system_milliseconds() - bake_menu.worker_heard > WORKER_SILENT_MILLISECONDS)
			{
				short level = bake_menu.worker_level;

				platform_log("prism bake: the main menu's bake of %s said nothing for %lu s: ended",
					bake_levels[level].map, WORKER_SILENT_MILLISECONDS / 1000);
				if (bake_menu.worker_checks)
					bake_menu.checked[level] = TRUE;
				worker_stop();
				if (!bake_menu.checked[level])
					bake_menu.state[level] = LEVEL_FAILED;
			}
		}
	}
	if (!menu_up() || !prism_bake_menu_wanted())
		return;
	if (!bake_menu.notes_read || system_milliseconds() - bake_menu.notes_read > NOTES_MILLISECONDS)
		levels_read();
	if (!bake_menu.worker)
	{
		short level = level_next();

		if (level != NONE)
			worker_begin(level);
		else if (bake_menu.was_baking)
		{
			bake_menu.was_baking = FALSE;
			bake_menu.all_done_at = system_milliseconds() | 1;
			platform_log("prism bake: the main menu has baked every level");
		}
	}
}

/* the main menu drawn this frame (menu_functions.c's pc_menu_main_menu_shown) */
void prism_bake_menu_main_shown(void)
{
	bake_menu.main_shown = system_milliseconds() | 1;
}

/* the main menu's frames a second while a worker bakes (0: no cap) */
int prism_bake_menu_frame_cap(void)
{
	return bake_menu.worker && menu_up() ? MENU_FRAME_CAP : 0;
}

/* the levels baked, of those whose maps there are, and those left */
static void levels_counted(int *baked, int *present)
{
	short level;

	*baked = *present = 0;
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		if (bake_menu.state[level] == LEVEL_NO_MAP || bake_menu.state[level] == LEVEL_UNKNOWN)
			continue;
		(*present)++;
		if (bake_menu.state[level] == LEVEL_BAKED || bake_menu.state[level] == LEVEL_SHIPPED)
			(*baked)++;
	}
}

static void time_words(float seconds, char *text, unsigned long size)
{
	if (seconds < 0.0f)
		text[0] = 0;
	else if (seconds < 90.0f)
		snprintf(text, size, "about a minute");
	else if (seconds < 3600.0f)
		snprintf(text, size, "about %d min", (int)(seconds / 60.0f + 0.5f));
	else
		snprintf(text, size, "about %d h %02d min", (int)(seconds / 3600.0f), (int)(seconds / 60.0f) % 60);
}

/* the worker's level's share done (its BSPs whole, the one baking's part) */
static float worker_share(void)
{
	int bsps = bake_menu.progress_bsps > 0 ? bake_menu.progress_bsps : 1;
	float share = ((float)bake_menu.progress_bsp + (strcmp(bake_menu.progress_state, "baking") ? 0.0f :
		bake_menu.progress)) / (float)bsps;

	return share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
}

/* the lines under the main menu (the second empty for none), and the bar's
share between them (-1: none) */
static boolean menu_line(char *text, char *detail, unsigned long size, float *share)
{
	char left[48];
	int baked, present;

	*share = -1.0f;
	detail[0] = 0;
	if (bake_menu.worker && bake_menu.worker_level != NONE && bake_menu.worker_checks)
	{
		*share = worker_share();
		snprintf(text, size, "Checking PRISM light: %s", bake_levels[bake_menu.worker_level].name);
		snprintf(detail, size, "The shipped light against your maps - playable now");
		return TRUE;
	}
	if (bake_menu.worker && bake_menu.worker_level != NONE)
	{
		levels_counted(&baked, &present);
		*share = worker_share();
		if (!strcmp(bake_menu.progress_state, "baking"))
		{
			time_words(bake_menu.seconds_left, left, sizeof(left));
			snprintf(text, size, "Baking PRISM light: %s %d%%", bake_levels[bake_menu.worker_level].name,
				(int)(*share * 100.0f));
			snprintf(detail, size, "%s%sLevel %d of %d - best played once it is done", left, left[0] ? " left - " : "",
				baked + 1, present);
		}
		else
		{
			snprintf(text, size, "Baking PRISM light: %s (loading)", bake_levels[bake_menu.worker_level].name);
			snprintf(detail, size, "Level %d of %d - best played once it is done", baked + 1, present);
		}
		/* (the time's words begin a line: its first letter a capital) */
		if (detail[0] >= 'a' && detail[0] <= 'z')
			detail[0] = (char)(detail[0] - 'a' + 'A');
		return TRUE;
	}
	if (bake_menu.all_done_at && system_milliseconds() - bake_menu.all_done_at < DONE_MILLISECONDS)
	{
		snprintf(text, size, "PRISM light baked: every level is ready");
		return TRUE;
	}
	return FALSE;
}

/* over the main menu (hud.c, with the HUD's draws, under the menu's
widgets: the menu's foot is clear there): the line, its bar, the detail */
void prism_bake_menu_draw(void)
{
	long font_index;
	struct font_header *font;
	short line_height, top, width;
	char text[160], detail[160];
	float share;
	real_argb_color color;
	rectangle2d bounds, bar, line;

	if (prism_bake_worker_map() || !menu_up() || !bake_menu.main_shown ||
		system_milliseconds() - bake_menu.main_shown > 250 || !menu_line(text, detail, sizeof(text), &share))
	{
		return;
	}
	font_index = interface_get_tag_index(_interface_font_terminal);
	if (font_index == NONE)
		return;
	font = font_definition_get(font_index);
	line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	if (line_height <= 0)
		return;
	bounds = render.camera.window_bounds;
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	/* (its foot this share of the screen's height over the screen's foot,
	clear of the menu's last item at 4:3 and 16:9 alike) */
	top = (short)(bounds.y1 - (bounds.y1 - bounds.y0) * LINE_FOOT_FRACTION - (2 * line_height + 3 + BAR_HEIGHT + 4));
	width = (short)((bounds.x1 - bounds.x0) * BAR_WIDTH_FRACTION);
	color.alpha = 0.95f;
	color.red = 0.80f;
	color.green = 0.88f;
	color.blue = 1.0f;
	line = bounds;
	line.y0 = top;
	line.y1 = (short)(top + line_height);
	draw_string_set_draw_mode(font_index, NONE, TEXT_CENTRED, 0, &color);
	rasterizer_draw_string(&line, NULL, NULL, 0, text);
	top = (short)(top + line_height + 3);
	if (share >= 0.0f)
	{
		bar.x0 = (short)((bounds.x0 + bounds.x1 - width) / 2);
		bar.x1 = (short)(bar.x0 + width);
		bar.y0 = top;
		bar.y1 = (short)(bar.y0 + BAR_HEIGHT);
		draw_quad(&bar, 0x60203040);
		bar.x1 = (short)(bar.x0 + (short)(width * share));
		if (bar.x1 > bar.x0)
			draw_quad(&bar, 0xE07FB8FF);
		top = (short)(top + BAR_HEIGHT + 4);
	}
	if (detail[0])
	{
		color.alpha = 0.80f;
		line.y0 = top;
		line.y1 = (short)(top + line_height);
		draw_string_set_draw_mode(font_index, NONE, TEXT_CENTRED, 0, &color);
		rasterizer_draw_string(&line, NULL, NULL, 0, detail);
	}
}

/* whether starting the map should ask first: the main menu's bake wanted
and the level a campaign one not yet baked */
boolean prism_bake_menu_level_unbaked(char const *map_name)
{
	short level = level_of_map(map_name);

	if (level == NONE || !prism_bake_menu_wanted())
		return FALSE;
	if (!bake_menu.notes_read)
		levels_read();
	return level_needs(level) || bake_menu.state[level] == LEVEL_FAILED;
}

/* WAIT: the level baked first (its worker started at once, another's
paused) */
void prism_bake_menu_first(char const *map_name)
{
	short level = level_of_map(map_name);

	if (level == NONE)
		return;
	bake_menu.first = level;
	if (bake_menu.state[level] == LEVEL_FAILED)
		bake_menu.state[level] = LEVEL_UNKNOWN;
	if (bake_menu.worker && bake_menu.worker_level != level)
		worker_stop();
	bake_menu.notes_read = 0;
}

/* a level started: noted as played last, the worker ended (the game's own
baker goes on with the level as it plays) */
void prism_bake_menu_level_started(char const *map_name)
{
	char path[BAKE_PATH + 32], name[64];

	settings_read();
	worker_stop();
	if (level_of_map(map_name) == NONE)
		return;
	map_folder_name(map_name, name, sizeof(name));
	strcat(name, "\n");
	posix_make_directory(bake_menu.folder);
	snprintf(path, sizeof(path), "%s/last_played.txt", bake_menu.folder);
	file_write(path, name);
}

/* REBAKE LOCAL (the row's key bake_rebake_all): every level's bake made
here and its notes deleted (the worker ended first; the shipped bake, read
only, kept), so the main menu checks the shipped bake again and bakes each
BSP it does not cover from nothing */
void prism_bake_menu_rebake_all(void)
{
	char path[BAKE_PATH + 96];
	static char const *const suffixes[] = { ".pbk", ".pbk.new", ".pbk.log" };
	short level;
	int bsp, suffix;

	settings_read();
	worker_stop();
	for (level = 0; level < BAKE_LEVELS; level++)
	{
		for (bsp = 0; bsp < BAKE_BSPS; bsp++)
		{
			for (suffix = 0; suffix < (int)NUMBEROF(suffixes); suffix++)
			{
				snprintf(path, sizeof(path), "%s/%s/bsp%d%s", bake_menu.folder, bake_levels[level].map, bsp,
					suffixes[suffix]);
				remove(path);
			}
		}
		snprintf(path, sizeof(path), "%s/%s/baked.txt", bake_menu.folder, bake_levels[level].map);
		remove(path);
		/* (what the baker found of the shipped light: checked again) */
		snprintf(path, sizeof(path), "%s/%s/shipped.txt", bake_menu.folder, bake_levels[level].map);
		remove(path);
		bake_menu.checked[level] = FALSE;
		if (bake_menu.state[level] == LEVEL_FAILED)
			bake_menu.state[level] = LEVEL_UNKNOWN;
	}
	bake_menu.notes_read = 0;
	platform_log("prism bake: every level's bake made here deleted (Rebake Local; the shipped light kept): the main menu "
		"checks the shipped light again and bakes what it does not cover");
}

/* Light Bake's row of a level (its map, "a10"), its words: SHIPPED (the
release's light, every BSP matching this machine's maps), CHECKING, BAKED,
BAKING 37%, LOADING, PART BAKED (some of its BSPs), NEXT, NOT BAKED, NO
MAP, FAILED (test_port_settings.py's test_light_bake_level_names_fit: each fits
the row's value); FALSE for a map not the campaign's */
boolean prism_bake_menu_level_words(char const *map, char *text, unsigned long size)
{
	short level = level_of_map(map);

	if (level == NONE)
		return FALSE;
	settings_read();
	if (!bake_menu.notes_read || system_milliseconds() - bake_menu.notes_read > NOTES_MILLISECONDS)
		levels_read();
	if (bake_menu.worker && bake_menu.worker_level == level)
	{
		if (bake_menu.worker_checks)
			snprintf(text, size, "CHECKING");
		else if (!strcmp(bake_menu.progress_state, "baking"))
		{
			int percent = (int)(worker_share() * 100.0f);

			snprintf(text, size, "BAKING %d%%", percent > 99 ? 99 : percent);
		}
		else
			snprintf(text, size, "LOADING");
		return TRUE;
	}
	switch (bake_menu.state[level])
	{
	case LEVEL_SHIPPED:
		snprintf(text, size, "SHIPPED");
		break;
	case LEVEL_NO_MAP:
		snprintf(text, size, "NO MAP");
		break;
	case LEVEL_BAKED:
		snprintf(text, size, "BAKED");
		break;
	case LEVEL_FAILED:
		snprintf(text, size, "FAILED");
		break;
	case LEVEL_PARTLY:
		snprintf(text, size, "PART BAKED");
		break;
	default:
		snprintf(text, size, level == bake_menu.first ? "NEXT" : "NOT BAKED");
		break;
	}
	return TRUE;
}

/* the hand-written questions' bindings (menu_screen.c's "bake."): the
level asked of, its time left by the baker's estimate (" (about 12 min)" or
empty) */
static char bake_menu_asked[64];

void prism_bake_menu_asked(char const *map_name)
{
	short level = level_of_map(map_name);

	snprintf(bake_menu_asked, sizeof(bake_menu_asked), "%s", level != NONE ? bake_levels[level].name : "This level");
}

int prism_bake_menu_value(char const *name, char *value, unsigned long size)
{
	if (!strcmp(name, "bake.level"))
		snprintf(value, size, "%s", bake_menu_asked[0] ? bake_menu_asked : "This level");
	else if (!strcmp(name, "bake.left"))
	{
		char left[48];

		time_words(bake_menu.worker ? bake_menu.seconds_left : -1.0f, left, sizeof(left));
		if (left[0])
			snprintf(value, size, " (%s left)", left);
		else
			value[0] = 0;
	}
	else
		return 0;
	return 1;
}
