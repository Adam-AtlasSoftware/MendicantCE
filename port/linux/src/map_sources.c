/*
MAP_SOURCES.C

The game's maps come from two sources, each the player's own: the Xbox
disc's (its disc image or maps folder, copied into <data root>/maps: the
game needs them) and Custom Edition's installer (its multiplayer maps and
resource maps, written into the custom_maps folder by map_import.c:
optional). They can be added in either order and at any time: at the first
start, which shows the screen until the game has its Xbox maps and starts
the game the moment it has them, and later from Settings' ADD MAPS, which
asks for the same screen at the next start. A game with one source never
sees the screen again unasked; Custom Edition's maps alone cannot start it (first_start.c on Linux
and macOS, sdl_platform.c on Windows).

Which sources a game has is told here alone, by what its folders hold, so
the screen and Settings say the same: the Xbox's maps by maps/ui.map (the
game's menus, which it cannot start without), Custom Edition's by its
resource maps and Gearbox's six maps, which are on no Xbox disc and which
no map shared in a game brings under their own names (a download is named
with its checksum: map_share.c). A Custom Edition install that
paths.custom_edition names counts as well (its maps folder, xbox_files.c's
h:\).
*/

#include "platform.h"

#include "map_sources.h"
#include "port_config.h"
#include "posix.h"

#include <stdio.h>
#include <string.h>

#ifndef HALO_ANDROID

#include "map_import.h"

#define MAP_SOURCES_PATH 1024
/* the request's file, in config.toml's folder */
#define MAP_SOURCES_REQUEST "add_maps"

/* what each source leaves in its folder */
static const char *const xbox_files[] = { "ui.map" };
static const char *const custom_edition_files[] =
{
	"bitmaps.map", "sounds.map", "loc.map",
	"icefields.map", "deathisland.map", "dangercanyon.map", "infinity.map", "timberland.map", "gephyrophobia.map",
};

/* whether folder holds every one of the files (their names matched as the
game matches them, case-insensitively) */
static int map_sources_folder_has(const char *folder, const char *const *files, int count)
{
	char on_disk[256];
	int index;

	if (!folder[0])
		return 0;
	for (index = 0; index < count; index++)
	{
		if (!posix_find_entry_case_insensitive(folder, files[index], on_disk, sizeof(on_disk)))
			return 0;
	}
	return 1;
}

/* <parent>/<child>, with child's on-disk spelling ("" if there is none) */
static void map_sources_child(const char *parent, const char *child, char *path, int size)
{
	char on_disk[256];

	path[0] = 0;
	if (parent[0] && posix_find_entry_case_insensitive(parent, child, on_disk, sizeof(on_disk)))
		snprintf(path, (size_t)size, "%s/%s", parent, on_disk);
}

void map_sources_read(const char *destination, struct map_sources *sources)
{
	char folder[MAP_SOURCES_PATH];
	char maps[MAP_SOURCES_PATH + 256];
	const char *install = config_string("paths.custom_edition");

	memset(sources, 0, sizeof(*sources));
	map_sources_child(destination, "maps", maps, (int)sizeof(maps));
	sources->present[_map_source_xbox] = map_sources_folder_has(maps, xbox_files,
		(int)(sizeof(xbox_files) / sizeof(xbox_files[0])));
	map_import_custom_maps_folder(destination, folder, (int)sizeof(folder));
	sources->present[_map_source_custom_edition] = map_sources_folder_has(folder, custom_edition_files,
		(int)(sizeof(custom_edition_files) / sizeof(custom_edition_files[0])));
	if (!sources->present[_map_source_custom_edition] && install[0])
	{
		map_sources_child(install, "maps", maps, (int)sizeof(maps));
		sources->present[_map_source_custom_edition] = map_sources_folder_has(maps, custom_edition_files,
			(int)(sizeof(custom_edition_files) / sizeof(custom_edition_files[0])));
	}
}

void map_sources_read_game(struct map_sources *sources)
{
	char root[MAP_SOURCES_PATH];
	size_t length;

	platform_translate_path("d:\\", root, sizeof(root));
	length = strlen(root);
	while (length > 1 && root[length - 1] == '/')
		root[--length] = 0;
	map_sources_read(root, sources);
}

static void map_sources_request_path(char *path, int size)
{
	char folder[MAP_SOURCES_PATH];

	config_folder(folder, sizeof(folder));
	snprintf(path, (size_t)size, "%s" MAP_SOURCES_REQUEST, folder);
}

void map_sources_request(int request)
{
	char path[MAP_SOURCES_PATH + 32];

	map_sources_request_path(path, (int)sizeof(path));
	if (request)
	{
		FILE *file = fopen(path, "wb");

		if (file)
			fclose(file);
	}
	else
	{
		remove(path);
	}
	platform_log("map sources: the maps screen %s at the next start", request ? "asked for" : "no longer asked for");
}

int map_sources_requested(void)
{
	char path[MAP_SOURCES_PATH + 32];
	struct posix_file_information information;

	map_sources_request_path(path, (int)sizeof(path));
	return posix_stat(path, &information) == 0;
}

#else

/* (the Android app imports the game data itself: it has its Xbox maps) */
void map_sources_read(const char *destination, struct map_sources *sources)
{
	(void)destination;
	memset(sources, 0, sizeof(*sources));
	sources->present[_map_source_xbox] = 1;
}

void map_sources_read_game(struct map_sources *sources)
{
	map_sources_read("", sources);
}

void map_sources_request(int request)
{
	(void)request;
}

int map_sources_requested(void)
{
	return 0;
}

#endif

int map_sources_next(const struct map_sources *sources)
{
	return sources->present[_map_source_xbox] ? _map_sources_play : _map_sources_list;
}

int map_sources_screen_at_start(const struct map_sources *sources, int requested)
{
	return !sources->present[_map_source_xbox] || requested;
}

void map_sources_summary(const struct map_sources *sources, char *text, int size)
{
	int xbox = sources->present[_map_source_xbox];
	int custom_edition = sources->present[_map_source_custom_edition];

	snprintf(text, (size_t)size, "%s", xbox && custom_edition ? "XBOX + CE" : xbox ? "XBOX ONLY" :
		custom_edition ? "CE ONLY" : "NONE");
}
