/*
MAP_SOURCES.H

The two sources of the game's maps, and which of them a game has
(map_sources.c): the one place the maps screen (first_start.c, Windows'
sdl_platform.c) and Settings' ADD MAPS (menu_functions.c) ask.
*/

#ifndef __HALO_LINUX_MAP_SOURCES_H
#define __HALO_LINUX_MAP_SOURCES_H

enum
{
	/* the Xbox disc's maps, in <data root>/maps: the game needs them */
	_map_source_xbox,
	/* Custom Edition's installer's multiplayer maps and resource maps, in
	the custom_maps folder (map_import.c): optional */
	_map_source_custom_edition,
	NUMBER_OF_MAP_SOURCES
};

/* what the maps screen does next */
enum
{
	/* the list of what can be added, again */
	_map_sources_list,
	/* the game starts */
	_map_sources_play,
};

struct map_sources
{
	int present[NUMBER_OF_MAP_SOURCES];
};

/* which sources the game whose data root is destination has */
void map_sources_read(const char *destination, struct map_sources *sources);
/* the same for the running game (its d:\) */
void map_sources_read_game(struct map_sources *sources);

/* after the screen added a source (or found it added already): the game
starts once it has every source, else the list comes back, the game's
Start among its items when it has the Xbox's maps */
int map_sources_next(const struct map_sources *sources);

/* the sources in a few words, for Settings' value column: "XBOX + CE",
"XBOX ONLY", "CE ONLY", "NONE" */
void map_sources_summary(const struct map_sources *sources, char *text, int size);

/* Settings' ADD MAPS: the maps screen asked for at the next start, or no
longer; whether it is; and its request taken (xbox_files.c, as it shows) */
void map_sources_request(int request);
int map_sources_requested(void);

#endif
