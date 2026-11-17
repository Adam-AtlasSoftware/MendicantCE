/*
MAP_SOURCES.C

port/linux/src/map_sources.c's reading of which sources of maps a game has
(its Xbox maps, Custom Edition's installer's), what the maps screen does
after each is added (first_start.c), and Settings' request for that screen
at the next start, over a made-up folder of files.
*/

#include "harness.h"

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef unsigned long posix_ulong;
struct posix_file_information { posix_ulong size; };

#define BOOL int
#define platform_log(...) ((void)0)

/* the files on the made-up disk, each "<folder>/<name>" */
#define FILES_MAXIMUM 64
static char files[FILES_MAXIMUM][256];
static int file_count;
static const char *custom_maps_setting = "";
static const char *custom_edition_setting = "";
static char config_directory[512];

static void file_add(const char *path)
{
	CHECK(file_count < FILES_MAXIMUM, "too many files");
	snprintf(files[file_count++], sizeof(files[0]), "%s", path);
}

/* a folder's entry, matched case-insensitively (a folder is there when a
file is in it) */
static int posix_find_entry_case_insensitive(const char *directory, const char *name, char *result, posix_ulong size)
{
	size_t length = strlen(directory);
	int index;

	for (index = 0; index < file_count; index++)
	{
		const char *entry = files[index] + length + 1;
		size_t entry_length = strcspn(entry, "/");

		if (strncmp(files[index], directory, length) || files[index][length] != '/')
			continue;
		if (entry_length == strlen(name) && !strncasecmp(entry, name, entry_length))
		{
			snprintf(result, size, "%.*s", (int)entry_length, entry);
			return 1;
		}
	}
	return 0;
}

static int posix_stat(const char *path, struct posix_file_information *information)
{
	struct stat status;

	information->size = 0;
	return stat(path, &status);
}

static const char *config_string(const char *name)
{
	if (!strcmp(name, "paths.custom_maps"))
		return custom_maps_setting;
	if (!strcmp(name, "paths.custom_edition"))
		return custom_edition_setting;
	return "";
}

static void config_folder(char *path, size_t size)
{
	snprintf(path, size, "%s/", config_directory);
}

static void platform_translate_path(const char *xbox_path, char *host_path, unsigned long size)
{
	(void)xbox_path;
	snprintf(host_path, size, "/game/");
}

#include "under_test.inc"

static const char *const custom_edition_names[] =
{
	"bitmaps.map", "sounds.map", "loc.map", "icefields.map", "deathisland.map", "dangercanyon.map", "infinity.map",
	"timberland.map", "gephyrophobia.map",
};

/* what copying the disc image leaves (xiso.c: the maps folder) */
static void add_xbox(void)
{
	file_add("/game/maps/bloodgulch.map");
	file_add("/game/maps/ui.map");
}

/* what Custom Edition's installer leaves in a folder (map_import.c) */
static void add_custom_edition(const char *folder)
{
	char path[256];
	int index;

	for (index = 0; index < (int)(sizeof(custom_edition_names) / sizeof(custom_edition_names[0])); index++)
	{
		snprintf(path, sizeof(path), "%s/%s", folder, custom_edition_names[index]);
		file_add(path);
	}
}

/* the sources read, the screen's next step, the next start's screen
(unasked, and with Settings' request) and the summary */
static void expect(int xbox, int custom_edition, int next, int screen, const char *summary)
{
	struct map_sources sources;
	char text[32];

	map_sources_read("/game", &sources);
	CHECK(sources.present[_map_source_xbox] == xbox, "the Xbox maps read as %d", sources.present[_map_source_xbox]);
	CHECK(sources.present[_map_source_custom_edition] == custom_edition, "Custom Edition's maps read as %d",
		sources.present[_map_source_custom_edition]);
	CHECK(map_sources_next(&sources) == next, "the screen's next is %d, not %d", map_sources_next(&sources), next);
	CHECK(map_sources_screen_at_start(&sources, 0) == screen, "the next start %s the maps screen unasked",
		screen ? "does not show" : "shows");
	CHECK(map_sources_screen_at_start(&sources, 1), "the next start ignores Settings' ADD MAPS");
	map_sources_summary(&sources, text, sizeof(text));
	CHECK(!strcmp(text, summary), "the summary is %s, not %s", text, summary);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "iso-then-ce"))
	{
		/* (the disc image first: the game starts, and no start shows the
		screen again unasked; Custom Edition's installer is ADD MAPS's) */
		expect(0, 0, _map_sources_list, 1, "NONE");
		add_xbox();
		expect(1, 0, _map_sources_play, 0, "XBOX ONLY");
		add_custom_edition("/game/custom_maps");
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
	}
	else if (!strcmp(name, "ce-then-iso"))
	{
		/* (Custom Edition's alone cannot start the game: the screen
		stays, at this start and the next) */
		add_custom_edition("/game/custom_maps");
		expect(0, 1, _map_sources_list, 1, "CE ONLY");
		add_xbox();
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
	}
	else if (!strcmp(name, "rerun"))
	{
		/* (Settings' request taken by the screen it opens: the next
		start opens none; a source added again changes nothing) */
		char template[] = "/tmp/map_sources_XXXXXX";
		char request[600];

		CHECK(mkdtemp(template) != NULL, "no folder for config.toml");
		snprintf(config_directory, sizeof(config_directory), "%s", template);
		add_xbox();
		add_custom_edition("/game/custom_maps");
		CHECK(!map_sources_requested(), "asked for before Settings asked");
		map_sources_request(1);
		CHECK(map_sources_requested(), "Settings' request was not kept");
		map_sources_request(0);
		CHECK(!map_sources_requested(), "the screen asked for again at the start after it showed");
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
		add_custom_edition("/game/custom_maps");
		add_xbox();
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
		snprintf(request, sizeof(request), "%s/add_maps", template);
		unlink(request);
		rmdir(template);
	}
	else if (!strcmp(name, "not-custom-edition"))
	{
		/* (a map shared in a game, with the resource maps it came with,
		is not Custom Edition's installer's; nor is part of it) */
		add_xbox();
		file_add("/game/custom_maps/bitmaps.map");
		file_add("/game/custom_maps/sounds.map");
		file_add("/game/custom_maps/hugeass_0e2b3ee4.map");
		expect(1, 0, _map_sources_play, 0, "XBOX ONLY");
		file_add("/game/maps/ICEFIELDS.MAP");
		expect(1, 0, _map_sources_play, 0, "XBOX ONLY");
	}
	else if (!strcmp(name, "settings-folders"))
	{
		/* (paths.custom_maps' folder, matched case-insensitively; and
		paths.custom_edition's install) */
		add_xbox();
		custom_maps_setting = "/elsewhere";
		add_custom_edition("/game/custom_maps");
		expect(1, 0, _map_sources_play, 0, "XBOX ONLY");
		file_add("/elsewhere/BITMAPS.MAP");
		add_custom_edition("/elsewhere");
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
		file_count = 0;
		add_xbox();
		custom_edition_setting = "/halo_ce";
		add_custom_edition("/halo_ce/MAPS");
		expect(1, 1, _map_sources_play, 0, "XBOX + CE");
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
