/*
FORGE_LAYOUT.C

Layouts of the dev tools (port/linux/game/forge.c): what Forge changed in a
map, saved to a file and loaded again. A layout loads only when it is asked
for: by the Map tab's "Load it", or by choosing it as a map in the map list.
The map chosen by its own name is the map as the game has it.

A layout holds:
	the objects the tools or mods made (spawned, pasted, brought back):
	        their tag, place, turn and scale
	the map's own objects that the tools moved or removed: found again by
	        their tag and the place the map puts them
	each mod's part (halo_mod.h, layout_save / layout_clear / layout_load):
	        spawns, zones, AI characters and waypoints, gravity, the sky

The map's own objects are noted when the map has placed them (the new map
hook, forge_layout_new_map), before a layout is loaded. Objects that the
game makes later (the weapons of a multiplayer map's spawn points, vehicles
that respawn) are not the map's own here: moving one saves a copy of it,
and the game still makes its own.

Files: a layout is a map variant, d:\forge_maps\<name>.mvar beside the
maps' custom_maps folder (forge_mvar.h, docs/forge_mvar.md): binary chunks
of the variant's name, description and base map, its objects (an id, the
type and tag, place, turn as a quaternion, scale, team, spawn flags and the
game types it is in), each mod's part and the editor's notes. The Map tab
lists the variants of the map it is on, in the order of their file names
(up to FORGE_LAYOUT_SLOT_COUNT); a new one is named for its name. Its name
and description are changed by the Map tab's Name and Description rows
(typed on the keyboard). Only the game's machine writes them (a local game, a system link
host): a client plays the host's variant and keeps none.

The layouts of before, u:\forge\<map>\layout_NN.txt in the save root (text,
one thing a line), are made variants once, when the variants are first
looked for: each gets a .mvar, and the .txt stays where it was (the slots
made are noted in u:\forge\<map>\converted.txt).

The "Map" tab of the tools' menu: choose a layout (left, right), name and
describe it, save it, load it, show it in
the map list, reset the map to its own look, remove every object of the
map (asked twice: the map's own come back with the reset, the tools' do
not), delete it. In a system link game the tab is the host's.

System link: the host's game is the game. What it changes reaches its
clients as its objects do (units and items, network_objects.c) and, for the
rest (scenery, devices, the mods' parts), as the layout's variant: sent to a
client when it has loaded, and again to every client when it has changed
(forge_layout_host_sync); the client applies it and saves nothing. A client's own tools ask the host to
spawn, move or remove an object (forge_layout_client_edit, a
_distributed_message_forge_edit), and the host does it.

A layout shown in the map list (its mvhd's listed) is a map of its
own: the multiplayer map list of local (splitscreen) games offers it after
the 13 maps of the game, with its base map's picture and the layout's name
(interface/ui_widget_event_handler_functions.c and
ui_widget_game_data_input_functions.c ask forge_custom_map_*; its name
comes through text/text_group.c's fallback strings). Choosing it plays the
base map with that layout loaded (forge_custom_map_select).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "forge_mvar.h"
#include "network_distributed.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_manager_internal.h" /* network_game_server_get_game */

#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xtl.h>

/* the game's malloc and free are debugging macros; the lines copied with
_strdup are the C library's */
#undef malloc
#undef free

/* ---------- constants */

enum
{
	/* the variants of a map the Map tab lists */
	FORGE_LAYOUT_SLOT_COUNT = 64,
	FORGE_LAYOUT_MAXIMUM_MAP_OBJECTS = 1024,
	FORGE_LAYOUT_MAXIMUM_SPAWNED = 512,
	FORGE_LAYOUT_NAME_SIZE = FORGE_MVAR_NAME_SIZE,
	FORGE_LAYOUT_DESCRIPTION_SIZE = FORGE_MVAR_DESCRIPTION_SIZE,
	FORGE_LAYOUT_PATH_SIZE = 256,
	/* a variant's file name in forge_maps, with ".mvar" */
	FORGE_LAYOUT_FILE_SIZE = 64,
	FORGE_LAYOUT_LINE_SIZE = 512,
	/* the slots of the layouts of before (layout_NN.txt) */
	FORGE_LAYOUT_OLD_SLOT_COUNT = 16,
	/* the multiplayer maps of the game, in the order of the map list and
	of its pictures */
	FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT = 13,
	FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS = 64,
	/* the most of a mod's part, or of a layout of before, that is read or
	written */
	FORGE_LAYOUT_MAXIMUM_TEXT = 65536,
	/* the objects a variant may hold, and the tags they name */
	FORGE_LAYOUT_MAXIMUM_RECORDS = FORGE_LAYOUT_MAXIMUM_SPAWNED + FORGE_LAYOUT_MAXIMUM_MAP_OBJECTS,
	/* how often the host looks whether its clients' part of the layout has
	changed (ticks) */
	FORGE_LAYOUT_SYNC_INTERVAL_TICKS = 15,
	/* the game's object data array, and how long the Map tab's "Remove all
	objects" waits to be chosen again (ticks) */
	FORGE_LAYOUT_MAXIMUM_OBJECTS = 2048,
	FORGE_LAYOUT_CONFIRM_TICKS = 150
};

/* where the variants are, beside custom_maps (the data root's) */
#define FORGE_LAYOUT_DIRECTORY "d:\\forge_maps"
#define FORGE_LAYOUT_EXTENSION ".mvar"

/* what is kept: units (not the players', who come later), items, scenery
and devices */
#define FORGE_LAYOUT_OBJECT_MASK (_object_mask_unit | _object_mask_item | _object_mask_scenery | _object_mask_device)

/* how near the place the map puts an object must be to be that object, and
how far one must be moved (or turned) to be saved as moved */
#define FORGE_LAYOUT_MATCH_DISTANCE 0.05f
#define FORGE_LAYOUT_MOVED_DISTANCE 0.005f
#define FORGE_LAYOUT_TURNED_DOT 0.9999f
#define FORGE_LAYOUT_SCALED_DIFFERENCE 0.001f
/* how near the place a client names an object that the host has no index
for (scenery, devices) must be */
#define FORGE_LAYOUT_EDIT_DISTANCE 0.5f
/* how far from the origin a client may ask for an object, and a layout's
line may put one (world units) */
#define FORGE_LAYOUT_WORLD_BOUND 32768.0f
/* the scales a layout's line may give an object (the tools' own are within) */
#define FORGE_LAYOUT_MINIMUM_SCALE 0.01f
#define FORGE_LAYOUT_MAXIMUM_SCALE 100.0f

enum
{
	_forge_layout_row_layout = 0,
	_forge_layout_row_name,
	_forge_layout_row_description,
	_forge_layout_row_save,
	_forge_layout_row_load,
	_forge_layout_row_listed,
	_forge_layout_row_reset,
	_forge_layout_row_remove_all,
	_forge_layout_row_delete,
	NUMBER_OF_FORGE_LAYOUT_ROWS
};

/* (halo_forge.h's spawn flags are a variant's) */
typedef char verify_forge_layout_spawn_at_start[FORGE_OBJECT_SPAWN_AT_START == FORGE_MVAR_SPAWN_AT_START ? 1 : -1];
typedef char verify_forge_layout_never_respawn[FORGE_OBJECT_SPAWN_NEVER_RESPAWN == FORGE_MVAR_SPAWN_NEVER_RESPAWN ? 1 : -1];

/* ---------- structures */

struct forge_layout_map_object
{
	long object_index;
	long definition_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	/* its scale as the map has it (1 for the tag's own) */
	real scale;
	/* found by the layout being loaded */
	boolean matched;
	/* the tools placed it or removed it (a vehicle settling, a weapon
	picked up are not a layout's) */
	boolean placed;
	boolean removed;
	/* its id (0 none yet: given when it is first saved), team, spawn flags
	and the editor's notes */
	struct forge_object_properties properties;
};

/* a layout's text as it is written; overflowed when it did not all fit */
struct halo_layout_writer
{
	char *text;
	unsigned long size;
	unsigned long length;
	boolean overflowed;
};

/* a variant of the map: its file in forge_maps ("" none: slot 0, a new one
not saved yet), what its mvhd says, and the base map's short name */
struct forge_layout_variant
{
	char file[FORGE_LAYOUT_FILE_SIZE];
	char name[FORGE_LAYOUT_NAME_SIZE];
	char description[FORGE_LAYOUT_DESCRIPTION_SIZE];
	char author[FORGE_MVAR_AUTHOR_SIZE];
	unsigned long created;
	unsigned long modified;
	boolean listed;
};

/* a layout shown in the map list */
struct forge_custom_map
{
	short base_index;
	char file[FORGE_LAYOUT_FILE_SIZE];
	wchar_t title[FORGE_LAYOUT_NAME_SIZE];
	/* "" none: the map list then says which map it is made on */
	wchar_t description[FORGE_LAYOUT_DESCRIPTION_SIZE];
};

/* an object of a variant as it is written or read, and its tag */
struct forge_layout_record
{
	struct forge_mvar_object object;
	struct forge_mvar_editor editor;
	long definition_index;
	/* with definition_index NONE (a layout of before being made a variant),
	the path of its tag */
	char const *path;
};

/* ---------- globals */

static struct
{
	/* the map these are about, the last part of its scenario's name */
	char map_name[FORGE_LAYOUT_NAME_SIZE];

	short map_object_count;
	struct forge_layout_map_object map_objects[FORGE_LAYOUT_MAXIMUM_MAP_OBJECTS];

	short spawned_count;
	long spawned[FORGE_LAYOUT_MAXIMUM_SPAWNED];
	struct forge_object_properties spawned_properties[FORGE_LAYOUT_MAXIMUM_SPAWNED];
	/* the objects of the variant loaded that this game does not make (not
	placed at start, or of other game types, with the tools off): saved
	with it again as they were */
	short dormant_count;
	struct forge_layout_record dormant[FORGE_LAYOUT_MAXIMUM_SPAWNED];
	/* the id the next object gets */
	unsigned long next_object_id;

	/* the Map tab: the chosen slot (0 a new variant), the one loaded or
	saved last, and the variants of this map (1 to variant_count); slot 0's
	name and description are those typed for a variant not saved yet */
	short chosen_slot;
	short current_slot;
	short variant_count;
	struct forge_layout_variant variants[FORGE_LAYOUT_SLOT_COUNT + 1];
	/* the slot whose name or description is being typed */
	short typing_slot;
	/* when "Remove all objects" was chosen and waits to be chosen again
	(NONE: it does not) */
	long remove_all_time;

	/* the layouts in the map list, and the one chosen there to play (its
	base map and file; -1 none), kept until another map is chosen */
	short custom_map_count;
	struct forge_custom_map custom_maps[FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS];
	short selected_base_index;
	char selected_file[FORGE_LAYOUT_FILE_SIZE];

	/* the variant of this map as its clients get it (sent to the clients of
	a system link game as they join, and again when it has changed), and
	whether one is being loaded on a client or written for the clients */
	unsigned long loaded_length;
	unsigned char loaded_variant[FORGE_MVAR_MAXIMUM_SIZE];
	boolean loading_for_client;
	boolean saving_for_client;
	/* the host: a checksum of the variant its clients have, and when it
	last looked whether it has changed */
	unsigned long synced_checksum;
	long sync_time;

	/* a variant as it is written or read, before it goes to its file or
	the clients; a mod's part as it is written, and a layout of before as it
	is read */
	unsigned char variant[FORGE_MVAR_MAXIMUM_SIZE];
	char written_text[FORGE_LAYOUT_MAXIMUM_TEXT];
	/* the objects of a variant being written or read */
	short record_count;
	struct forge_layout_record records[FORGE_LAYOUT_MAXIMUM_RECORDS];
	/* where each string of the string table of a variant being read is */
	unsigned long string_count;
	unsigned long string_offsets[FORGE_LAYOUT_MAXIMUM_RECORDS];

	/* the tag the last line of a layout named ("" none), in this map */
	char line_tag_name[FORGE_LAYOUT_PATH_SIZE];
	long line_tag_group;
	long line_tag_index;

	/* a client's variant from the host, as its parts come, and a checksum
	of the one applied (the host may send it again) */
	unsigned long received_length;
	unsigned char received_variant[FORGE_MVAR_MAXIMUM_SIZE];
	unsigned long applied_checksum;
} forge_layout_globals;

/* TRUE while a layout's spawned object is made (forge_layout_creating_object) */
static boolean forge_layout_creating;

/* the multiplayer maps (interface/ui_widget_event_handler_functions.c) */
static char const *const forge_layout_multiplayer_maps[FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT] =
{
	"beavercreek",
	"sidewinder",
	"damnation",
	"ratrace",
	"prisoner",
	"hangemhigh",
	"chillout",
	"carousel",
	"boardingaction",
	"bloodgulch",
	"wizard",
	"putput",
	"longest"
};

/* ---------- files written whole

A file Forge writes (a layout, a prefab, the menu's place) is written beside
itself first, "<name>.new", and takes the old one's place only once it is all
there: a game stopped while it writes (an Android app closed, a full disk)
leaves the file as it was. */

FILE *forge_file_begin(
	char const *path,
	char *temporary_path,
	unsigned long size)
{
	_snprintf(temporary_path, size, "%s.new", path);
	temporary_path[size - 1] = 0;

	return fopen(temporary_path, "wb");
}

/* FALSE when it could not all be written: the file stays as it was */
int forge_file_commit(
	FILE *file,
	char const *temporary_path,
	char const *path)
{
	boolean written = !ferror(file);

	written = fclose(file) == 0 && written;
	/* (rename replaces a file at once where the system can, but not on
	Windows, whose old file goes first) */
	if (written && rename(temporary_path, path) != 0)
	{
		remove(path);
		written = rename(temporary_path, path) == 0;
	}
	if (!written)
		remove(temporary_path);

	return written;
}

/* ---------- private code */

/* cache_files.c's, and the platform's log */
boolean tag_index_is_group(long tag_index, long group_tag);
void platform_log(char const *format, ...);

/* a machine whose game state is the game's: a local game, or the host of a
system link game (network_distributed.c, whose clients copy its objects) */
static boolean forge_layout_authoritative(
	void)
{
	return (game_connection() == _game_connection_local || game_connection() == _game_connection_network_server) &&
		forge_layout_globals.map_name[0] != 0;
}

static void forge_layout_variant_path(
	char const *file,
	char *path,
	unsigned long size)
{
	_snprintf(path, size, "%s\\%s", FORGE_LAYOUT_DIRECTORY, file);
	path[size - 1] = 0;

	return;
}

/* a layout of before: u:\forge\<map>\layout_NN.txt */
static void forge_layout_old_slot_path(
	char const *map_name,
	short slot,
	char *path,
	unsigned long size)
{
	_snprintf(path, size, "u:\\forge\\%s\\layout_%02d.txt", map_name, slot);
	path[size - 1] = 0;

	return;
}

/* a name or a description, typed or read from a file, without what a line
of the file and the menus' strings cannot hold */
static void forge_layout_clean_text(
	char const *text,
	char *result,
	unsigned long size)
{
	unsigned long length = 0;

	while (*text == ' ')
		text++;
	for (; *text && length < size - 1; text++)
	{
		/* (a percent sign would be taken for a format by the menus) */
		if (*text >= ' ' && *text < 127 && *text != '%')
			result[length++] = *text;
	}
	while (length > 0 && result[length - 1] == ' ')
		length--;
	result[length] = 0;

	return;
}

/* the file at path in a buffer of size bytes; its length, or -1 when it
cannot be read or is longer than size - 1 (it is then not taken in part).
The buffer is ended after what was read. */
static long forge_layout_read_file(
	char const *path,
	void *buffer,
	unsigned long size)
{
	FILE *file = fopen(path, "rb");
	long length = -1;

	if (file)
	{
		length = (long)fread(buffer, 1, size - 1, file);
		if (fgetc(file) != EOF)
			length = -1;
		fclose(file);
	}
	((char *)buffer)[length > 0 ? length : 0] = 0;

	return length;
}

/* the line without its line break */
static void forge_layout_chomp(
	char *line)
{
	line[strcspn(line, "\r\n")] = 0;

	return;
}

/* the variant in the buffer's mvhd, its texts as the menus may show them;
FALSE when it is not a variant (or a damaged one) */
static boolean forge_layout_variant_header(
	void const *data,
	unsigned long length,
	struct forge_mvar_header *header)
{
	struct forge_mvar_reader reader;
	struct forge_mvar_reader chunk;
	char tag[5];
	unsigned int version;
	boolean found = FALSE;

	if (!forge_mvar_open(&reader, data, length))
		return FALSE;
	while (!found && forge_mvar_next_chunk(&reader, tag, &version, &chunk))
	{
		if (strcmp(tag, "mvhd") == 0)
			found = forge_mvar_get_header(&chunk, header);
	}
	if (!found)
		return FALSE;
	/* (a file may hold anything: only what could have been typed is kept) */
	forge_layout_clean_text(header->name, header->name, sizeof(header->name));
	forge_layout_clean_text(header->description, header->description, sizeof(header->description));
	forge_layout_clean_text(header->author, header->author, sizeof(header->author));
	forge_layout_clean_text(header->scenario, header->scenario, sizeof(header->scenario));
	if (!header->name[0])
		_snprintf(header->name, sizeof(header->name), "Untitled");

	return TRUE;
}

static int __cdecl forge_layout_compare_variants(
	void const *a,
	void const *b)
{
	return _stricmp(((struct forge_layout_variant const *)a)->file, ((struct forge_layout_variant const *)b)->file);
}

static void forge_layout_convert_old_layouts(
	void);

/* the variants in forge_maps whose base map is map_name (a scenario's last
part, as forge_map_folder_name has it; NULL every map), at most count, in
the order of their file names; how many. With base_names, each one's base
map's short name too. */
static short forge_layout_list_variants(
	char const *map_name,
	struct forge_layout_variant *variants,
	char (*base_names)[FORGE_LAYOUT_NAME_SIZE],
	short count)
{
	WIN32_FIND_DATAA data;
	HANDLE find;
	short found = 0;

	forge_layout_convert_old_layouts();
	/* (a listing of its own, never find_files_start: custom_edition_maps.c
	says why) */
	find = FindFirstFileA(FORGE_LAYOUT_DIRECTORY "\\*" FORGE_LAYOUT_EXTENSION, &data);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		char file[sizeof(data.cFileName) + 1];
		char path[FORGE_LAYOUT_PATH_SIZE];
		char base_name[FORGE_LAYOUT_NAME_SIZE];
		struct forge_mvar_header header;
		long length;
		unsigned long file_length;

		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		csmemcpy(file, data.cFileName, sizeof(data.cFileName));
		file[sizeof(data.cFileName)] = 0;
		file_length = (unsigned long)strlen(file);
		/* (the pattern may find "x.mvarx" on some systems) */
		if (file_length >= FORGE_LAYOUT_FILE_SIZE || file_length <= strlen(FORGE_LAYOUT_EXTENSION) ||
			_stricmp(file + file_length - strlen(FORGE_LAYOUT_EXTENSION), FORGE_LAYOUT_EXTENSION) != 0)
		{
			continue;
		}
		forge_layout_variant_path(file, path, sizeof(path));
		length = forge_layout_read_file(path, forge_layout_globals.variant, sizeof(forge_layout_globals.variant));
		if (length < 0 || !forge_layout_variant_header(forge_layout_globals.variant, (unsigned long)length, &header))
			continue;
		forge_map_folder_name(header.scenario, base_name, sizeof(base_name));
		if (map_name && _stricmp(base_name, map_name) != 0)
			continue;
		if (found >= count)
			break;
		csmemset(&variants[found], 0, sizeof(variants[found]));
		csstrcpy(variants[found].file, file);
		csstrcpy(variants[found].name, header.name);
		csstrcpy(variants[found].description, header.description);
		csstrcpy(variants[found].author, header.author);
		variants[found].created = header.created;
		variants[found].modified = header.modified;
		variants[found].listed = header.listed;
		if (base_names)
			csstrcpy(base_names[found], base_name);
		found++;
	} while (FindNextFileA(find, &data));
	CloseHandle(find);

	/* (base_names, when asked for, are of the multiplayer maps alone: the
	map list's, which sorts them itself) */
	if (!base_names)
		qsort(variants, found, sizeof(variants[0]), forge_layout_compare_variants);

	return found;
}

/* the variants of this map */
static void forge_layout_read_slots(
	void)
{
	short slot;

	forge_layout_globals.variant_count = forge_layout_list_variants(forge_layout_globals.map_name,
		&forge_layout_globals.variants[1], NULL, FORGE_LAYOUT_SLOT_COUNT);
	for (slot = forge_layout_globals.variant_count + 1; slot <= FORGE_LAYOUT_SLOT_COUNT; slot++)
		csmemset(&forge_layout_globals.variants[slot], 0, sizeof(forge_layout_globals.variants[slot]));

	return;
}

/* the slot of this map's variant in the file, or 0 */
static short forge_layout_slot_of_file(
	char const *file)
{
	short slot;

	for (slot = 1; file[0] && slot <= forge_layout_globals.variant_count; slot++)
	{
		if (_stricmp(forge_layout_globals.variants[slot].file, file) == 0)
			return slot;
	}

	return 0;
}

/* a file name in forge_maps for a new variant of the map with the name:
the map's name, then the name's letters and digits, the rest as
underscores, and a number after it when that file is there already; FALSE
when none is free */
static boolean forge_layout_new_file_name(
	char const *map_name,
	char const *name,
	char *file,
	unsigned long size)
{
	char base[40];
	unsigned long length = 0;
	short number;

	for (; *name && length < sizeof(base) - 1; name++)
	{
		char character = *name;

		base[length++] = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' ? character : '_';
	}
	base[length] = 0;
	if (!length)
		csstrcpy(base, "variant");

	for (number = 1; number < 1000; number++)
	{
		char path[FORGE_LAYOUT_PATH_SIZE];
		FILE *existing;

		if (number == 1)
			_snprintf(file, size, "%s_%s%s", map_name, base, FORGE_LAYOUT_EXTENSION);
		else
			_snprintf(file, size, "%s_%s_%d%s", map_name, base, number, FORGE_LAYOUT_EXTENSION);
		file[size - 1] = 0;
		forge_layout_variant_path(file, path, sizeof(path));
		if ((existing = fopen(path, "rb")) == NULL)
			return TRUE;
		fclose(existing);
	}

	return FALSE;
}

/* an object's tag group, four letters as a number */
static long forge_layout_group_tag(
	char const *name)
{
	return (long)(((unsigned long)(unsigned char)name[0] << 24) | ((unsigned long)(unsigned char)name[1] << 16) |
		((unsigned long)(unsigned char)name[2] << 8) | (unsigned long)(unsigned char)name[3]);
}

static struct forge_layout_map_object *forge_layout_map_object(
	long object_index)
{
	short index;

	for (index = 0; index < forge_layout_globals.map_object_count; index++)
	{
		if (forge_layout_globals.map_objects[index].object_index == object_index)
			return &forge_layout_globals.map_objects[index];
	}

	return NULL;
}

/* an object's properties as a new one has them: no team, placed at start,
in every game type; with the id, 0 none yet */
static void forge_layout_default_properties(
	struct forge_object_properties *properties,
	unsigned long id)
{
	csmemset(properties, 0, sizeof(*properties));
	properties->id = id;
	properties->team = NONE;
	properties->spawn_flags = FORGE_MVAR_SPAWN_AT_START;

	return;
}

/* whether the properties are other than a new object's (a map's own
object is then saved, even where the map puts it) */
static boolean forge_layout_properties_changed(
	struct forge_object_properties const *properties)
{
	return properties->team != NONE || properties->spawn_flags != FORGE_MVAR_SPAWN_AT_START ||
		properties->respawn_seconds != 0 || properties->game_types != 0 || properties->group != 0 ||
		properties->locked || properties->comment[0] != 0;
}

/* the map's own objects, as it placed them */
static void forge_layout_note_map_objects(
	void)
{
	struct object_iterator iterator;
	struct object_datum *object;

	forge_layout_globals.map_object_count = 0;
	object_iterator_new(&iterator, FORGE_LAYOUT_OBJECT_MASK, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL &&
		forge_layout_globals.map_object_count < FORGE_LAYOUT_MAXIMUM_MAP_OBJECTS)
	{
		struct forge_layout_map_object *entry;

		/* what something carries goes with it */
		if (object->object.parent_object_index != NONE)
			continue;
		entry = &forge_layout_globals.map_objects[forge_layout_globals.map_object_count++];
		entry->object_index = iterator.index;
		entry->definition_index = object->definition_index;
		entry->position = object->object.position;
		entry->forward = object->object.forward;
		entry->up = object->object.up;
		entry->scale = object->object.scale > 0.f ? object->object.scale : 1.f;
		entry->matched = FALSE;
		entry->placed = FALSE;
		entry->removed = FALSE;
		forge_layout_default_properties(&entry->properties, 0);
	}

	return;
}

static long forge_layout_new_object(
	long definition_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real scale)
{
	struct object_placement_data data;
	long object_index;

	object_placement_data_new(&data, definition_index, NONE);
	data.position = *position;
	data.forward = *forward;
	data.up = *up;
	object_index = object_new(&data);
	if (object_index != NONE)
	{
		struct object_datum *object = object_get(object_index);

		if (scale > 0.f && scale != 1.f)
			objects_scripting_set_scale(object_index, scale, 0);
		object->object.translational_velocity = *global_zero_vector3d;
		object->object.angular_velocity = *global_zero_vector3d;
	}

	return object_index;
}

static void forge_layout_place(
	long object_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up)
{
	struct object_datum *object = object_get(object_index);

	/* an object not in the world (garbage, another structure) stays */
	if (!TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		return;
	object_set_position(object_index, (real_point3d *)position, (real_vector3d *)forward, (real_vector3d *)up);
	object->object.translational_velocity = *global_zero_vector3d;
	object->object.angular_velocity = *global_zero_vector3d;

	return;
}

/* the map as it has it: the mods' parts, the spawned objects gone, the
map's own objects back where it puts them */
static void forge_layout_reset(
	void)
{
	short index;

	for (index = 0; index < halo_mods_count(); index++)
	{
		struct halo_mod const *mod = halo_mods_get(index);

		if (mod->layout_clear)
			mod->layout_clear();
	}

	for (index = 0; index < forge_layout_globals.spawned_count; index++)
	{
		if (object_try_and_get(forge_layout_globals.spawned[index]))
			object_delete(forge_layout_globals.spawned[index]);
	}
	forge_layout_globals.spawned_count = 0;

	for (index = 0; index < forge_layout_globals.map_object_count; index++)
	{
		struct forge_layout_map_object *entry = &forge_layout_globals.map_objects[index];
		struct object_datum *object = object_try_and_get(entry->object_index);

		/* only what the tools changed: the rest is the game's (a vehicle
		that settled, a weapon picked up) */
		if (entry->placed && object && object->definition_index == entry->definition_index)
		{
			if (object->object.parent_object_index == NONE)
				forge_layout_place(entry->object_index, &entry->position, &entry->forward, &entry->up);
			if (object->object.scale != entry->scale)
				objects_scripting_set_scale(entry->object_index, entry->scale, 0);
		}
		else if (entry->removed && (!object || object->definition_index != entry->definition_index))
		{
			entry->object_index = forge_layout_new_object(
				entry->definition_index, &entry->position, &entry->forward, &entry->up, 0.f);
		}
		entry->matched = FALSE;
		entry->placed = FALSE;
		entry->removed = FALSE;
		forge_layout_default_properties(&entry->properties, 0);
	}
	forge_layout_globals.dormant_count = 0;

	forge_layout_globals.current_slot = 0;

	return;
}

/* every object of the map goes: the map's own and the tools', but not the
players, what they ride and what they carry. The map's own are removed as
the tools remove one (saved as removed, back with forge_layout_reset); how
many went */
static short forge_layout_remove_all(
	void)
{
	static long object_indices[FORGE_LAYOUT_MAXIMUM_OBJECTS];
	struct object_iterator iterator;
	struct object_datum *object;
	short count = 0;
	short removed_count = 0;
	short index;

	object_iterator_new(&iterator, FORGE_LAYOUT_OBJECT_MASK, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL &&
		count < FORGE_LAYOUT_MAXIMUM_OBJECTS)
	{
		/* (what something carries goes or stays with it) */
		if (object->object.parent_object_index == NONE)
			object_indices[count++] = iterator.index;
	}
	for (index = 0; index < count; index++)
	{
		struct data_iterator player_iterator;
		struct player_datum *player;
		boolean players = FALSE;

		data_iterator_new(&player_iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&player_iterator)) != NULL)
		{
			if (player->unit_index != NONE && object_try_and_get(player->unit_index) &&
				object_get_ultimate_parent(player->unit_index) == object_indices[index])
			{
				players = TRUE;
			}
		}
		if (!players && object_try_and_get(object_indices[index]))
		{
			forge_layout_note_removed(object_indices[index]);
			object_delete(object_indices[index]);
			removed_count++;
		}
	}

	return removed_count;
}

/* ---------- saving */

static void forge_layout_writer_new(
	struct halo_layout_writer *writer,
	char *text,
	unsigned long size)
{
	writer->text = text;
	writer->size = size;
	writer->length = 0;
	writer->overflowed = FALSE;
	text[0] = 0;

	return;
}

/* a line of the layout */
static void forge_layout_vprintf(
	struct halo_layout_writer *writer,
	char const *format,
	va_list arguments)
{
	unsigned long available = writer->size - writer->length;
	int length = writer->overflowed ? -1 : vsnprintf(writer->text + writer->length, available, format, arguments);

	/* (and its line break) */
	if (length < 0 || (unsigned long)length + 2 > available)
	{
		writer->overflowed = TRUE;
		writer->text[writer->length] = 0;
	}
	else
	{
		writer->length += (unsigned long)length;
		writer->text[writer->length++] = '\n';
		writer->text[writer->length] = 0;
	}

	return;
}

void halo_layout_printf(
	struct halo_layout_writer *writer,
	char const *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	forge_layout_vprintf(writer, format, arguments);
	va_end(arguments);

	return;
}

/* whether the host's objects of the definition reach its clients by
themselves (network_objects.c: units, vehicles, weapons, equipment) */
static boolean forge_layout_host_brings(
	long definition_index)
{
	return (FLAG(object_definition_get(definition_index)->object.type) &
		(_object_mask_unit | _object_mask_item)) != 0;
}

/* an orientation as the variant keeps it: the unit quaternion (x, y, z, w,
w not below 0) that turns the world's axes x, y and z onto the object's
forward, left and up; and back (docs/forge_mvar.md) */
static void forge_layout_rotation_of_axes(
	real_vector3d const *forward,
	real_vector3d const *up,
	float rotation[4])
{
	real_vector3d left;
	real m[3][3];
	real trace;
	real s;
	real length;
	short index;

	cross_product3d(up, forward, &left);
	/* (the axes as a matrix's columns) */
	m[0][0] = forward->i; m[0][1] = left.i; m[0][2] = up->i;
	m[1][0] = forward->j; m[1][1] = left.j; m[1][2] = up->j;
	m[2][0] = forward->k; m[2][1] = left.k; m[2][2] = up->k;
	trace = m[0][0] + m[1][1] + m[2][2];
	if (trace > 0.f)
	{
		s = square_root(trace + 1.f) * 2.f;
		rotation[3] = 0.25f * s;
		rotation[0] = (m[2][1] - m[1][2]) / s;
		rotation[1] = (m[0][2] - m[2][0]) / s;
		rotation[2] = (m[1][0] - m[0][1]) / s;
	}
	else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
	{
		s = square_root(1.f + m[0][0] - m[1][1] - m[2][2]) * 2.f;
		rotation[3] = (m[2][1] - m[1][2]) / s;
		rotation[0] = 0.25f * s;
		rotation[1] = (m[0][1] + m[1][0]) / s;
		rotation[2] = (m[0][2] + m[2][0]) / s;
	}
	else if (m[1][1] > m[2][2])
	{
		s = square_root(1.f + m[1][1] - m[0][0] - m[2][2]) * 2.f;
		rotation[3] = (m[0][2] - m[2][0]) / s;
		rotation[0] = (m[0][1] + m[1][0]) / s;
		rotation[1] = 0.25f * s;
		rotation[2] = (m[1][2] + m[2][1]) / s;
	}
	else
	{
		s = square_root(1.f + m[2][2] - m[0][0] - m[1][1]) * 2.f;
		rotation[3] = (m[1][0] - m[0][1]) / s;
		rotation[0] = (m[0][2] + m[2][0]) / s;
		rotation[1] = (m[1][2] + m[2][1]) / s;
		rotation[2] = 0.25f * s;
	}
	length = square_root(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] +
		rotation[3] * rotation[3]);
	for (index = 0; index < 4; index++)
		rotation[index] = length > 0.f ? rotation[index] / length * (rotation[3] < 0.f ? -1.f : 1.f) : 0.f;
	if (!(length > 0.f))
		rotation[3] = 1.f;

	return;
}

/* FALSE when the rotation cannot be one (not numbers, or far from a unit
quaternion): from a file or the host */
static boolean forge_layout_axes_of_rotation(
	float const rotation[4],
	real_vector3d *forward,
	real_vector3d *up)
{
	real x = rotation[0];
	real y = rotation[1];
	real z = rotation[2];
	real w = rotation[3];
	real length_squared = x * x + y * y + z * z + w * w;
	real length;

	/* (a number that is none fails every comparison) */
	if (!(length_squared > 0.5f && length_squared < 2.f))
		return FALSE;
	length = square_root(length_squared);
	x /= length;
	y /= length;
	z /= length;
	w /= length;
	set_real_vector3d(forward, 1.f - 2.f * (y * y + z * z), 2.f * (x * y + w * z), 2.f * (x * z - w * y));
	set_real_vector3d(up, 2.f * (x * z + w * y), 2.f * (y * z - w * x), 1.f - 2.f * (x * x + y * y));

	return distributed_axes_make_valid(forward, up);
}

/* the object type that objects of a tag group are, or 0xff */
static int forge_layout_type_of_group(
	unsigned long group)
{
	static struct
	{
		unsigned long group;
		int type;
	} const types[] =
	{
		{ 'bipd', _object_type_biped },
		{ 'vehi', _object_type_vehicle },
		{ 'weap', _object_type_weapon },
		{ 'eqip', _object_type_equipment },
		{ 'garb', _object_type_garbage },
		{ 'proj', _object_type_projectile },
		{ 'scen', _object_type_scenery },
		{ 'mach', _object_type_machine },
		{ 'ctrl', _object_type_control },
		{ 'lifi', _object_type_light_fixture },
		{ 'plac', _object_type_placeholder },
		{ 'ssce', _object_type_sound_scenery }
	};
	short index;

	for (index = 0; index < NUMBEROF(types); index++)
	{
		if (types[index].group == group)
			return types[index].type;
	}

	return 0xff;
}

/* an object of the variant being written: its record, with its tag's path
in the string table (the path of definition_index, or path when that is
NONE) */
static struct forge_layout_record *forge_layout_add_record(
	int kind,
	long definition_index,
	char const *path,
	unsigned long group,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real scale,
	real_point3d const *original,
	struct forge_object_properties const *properties)
{
	struct forge_layout_record *record;
	short index;

	if (forge_layout_globals.record_count >= FORGE_LAYOUT_MAXIMUM_RECORDS)
		return NULL;
	record = &forge_layout_globals.records[forge_layout_globals.record_count++];
	csmemset(record, 0, sizeof(*record));
	record->definition_index = definition_index;
	record->path = path;
	record->object.id = properties->id;
	record->object.kind = kind;
	record->object.group = group;
	record->object.type = definition_index != NONE
		? object_definition_get(definition_index)->object.type
		: forge_layout_type_of_group(group);
	record->object.team = properties->team;
	record->object.spawn_flags = (unsigned int)properties->spawn_flags;
	record->object.respawn_seconds = (unsigned int)properties->respawn_seconds;
	record->object.game_types = (unsigned int)properties->game_types;
	record->object.position[0] = position->x;
	record->object.position[1] = position->y;
	record->object.position[2] = position->z;
	forge_layout_rotation_of_axes(forward, up, record->object.rotation);
	record->object.scale = scale;
	record->object.original[0] = original->x;
	record->object.original[1] = original->y;
	record->object.original[2] = original->z;
	record->editor.id = properties->id;
	record->editor.group = (unsigned int)properties->group;
	record->editor.flags = properties->locked ? FORGE_MVAR_EDITOR_LOCKED : 0;
	csstrcpy(record->editor.comment, properties->comment);
	/* its tag's string: the one an object before it has, else the next */
	record->object.tag = 0;
	for (index = 0; index < forge_layout_globals.record_count - 1; index++)
	{
		struct forge_layout_record const *other = &forge_layout_globals.records[index];

		if (other->object.group == group && (definition_index != NONE
			? other->definition_index == definition_index
			: other->path && strcmp(other->path, path) == 0))
		{
			record->object.tag = other->object.tag;
			return record;
		}
		record->object.tag = MAX(record->object.tag, other->object.tag + 1);
	}

	return record;
}

/* the path of a record's tag */
static char const *forge_layout_record_path(
	struct forge_layout_record const *record)
{
	return record->definition_index != NONE ? tag_get_name(record->definition_index) : record->path;
}

/* the records' string table, objects and (but for the clients of a system
link game) the editor's notes */
static void forge_layout_put_records(
	struct forge_mvar_writer *writer,
	boolean for_client)
{
	unsigned int string_count = 0;
	unsigned long editor_count = 0;
	short index;

	forge_mvar_chunk_begin(writer, "strt", FORGE_MVAR_VERSION);
	for (index = 0; index < forge_layout_globals.record_count; index++)
		string_count = MAX(string_count, forge_layout_globals.records[index].object.tag + 1);
	forge_mvar_put_u32(writer, string_count);
	/* (each string where the first object to name it is) */
	for (index = 0; index < forge_layout_globals.record_count; index++)
	{
		struct forge_layout_record const *record = &forge_layout_globals.records[index];
		short other;

		for (other = 0; other < index && forge_layout_globals.records[other].object.tag != record->object.tag; other++)
		{
		}
		if (other == index)
			forge_mvar_put_string(writer, forge_layout_record_path(record));
	}
	forge_mvar_chunk_end(writer);

	forge_mvar_chunk_begin(writer, "mvob", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(writer, (unsigned long)forge_layout_globals.record_count);
	forge_mvar_put_u16(writer, FORGE_MVAR_OBJECT_RECORD_SIZE);
	forge_mvar_put_u16(writer, 0);
	for (index = 0; index < forge_layout_globals.record_count; index++)
		forge_mvar_put_object(writer, &forge_layout_globals.records[index].object);
	forge_mvar_chunk_end(writer);

	if (for_client)
		return;
	for (index = 0; index < forge_layout_globals.record_count; index++)
	{
		struct forge_mvar_editor const *editor = &forge_layout_globals.records[index].editor;

		if (editor->group || editor->flags || editor->comment[0])
			editor_count++;
	}
	forge_mvar_chunk_begin(writer, "mved", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(writer, editor_count);
	forge_mvar_put_u16(writer, FORGE_MVAR_EDITOR_RECORD_SIZE);
	forge_mvar_put_u16(writer, 0);
	for (index = 0; index < forge_layout_globals.record_count; index++)
	{
		struct forge_mvar_editor const *editor = &forge_layout_globals.records[index].editor;

		if (editor->group || editor->flags || editor->comment[0])
			forge_mvar_put_editor(writer, editor);
	}
	forge_mvar_chunk_end(writer);

	return;
}

/* the id the next object gets */
static unsigned long forge_layout_new_id(
	void)
{
	if (forge_layout_globals.next_object_id == 0)
		forge_layout_globals.next_object_id = 1;

	return forge_layout_globals.next_object_id++;
}

/* the objects of the layout, as records: for the clients of a system link
game (for_client), without what the host's objects bring them, which only
moves about */
static void forge_layout_collect_records(
	boolean for_client)
{
	short index;

	forge_layout_globals.record_count = 0;
	for (index = 0; index < forge_layout_globals.spawned_count; index++)
	{
		struct object_datum *object = object_try_and_get(forge_layout_globals.spawned[index]);
		struct forge_object_properties *properties = &forge_layout_globals.spawned_properties[index];

		if (!object || object->object.parent_object_index != NONE ||
			(for_client && forge_layout_host_brings(object->definition_index)))
		{
			continue;
		}
		if (!properties->id && !for_client)
			properties->id = forge_layout_new_id();
		forge_layout_add_record(_forge_mvar_object_placed, object->definition_index, NULL,
			tag_get_group_tag(object->definition_index), &object->object.position, &object->object.forward,
			&object->object.up, object->object.scale > 0.f ? object->object.scale : 1.f, &object->object.position,
			properties);
	}
	/* (as they were loaded: this game did not make them) */
	for (index = 0; index < forge_layout_globals.dormant_count && !for_client; index++)
	{
		if (forge_layout_globals.record_count < FORGE_LAYOUT_MAXIMUM_RECORDS)
			forge_layout_globals.records[forge_layout_globals.record_count++] = forge_layout_globals.dormant[index];
	}
	for (index = 0; index < forge_layout_globals.map_object_count; index++)
	{
		struct forge_layout_map_object *entry = &forge_layout_globals.map_objects[index];
		boolean changed = forge_layout_properties_changed(&entry->properties);
		struct object_datum *object;
		real scale = 0.f;

		/* (most are as the map has them) */
		if ((!entry->removed && !entry->placed && !changed) ||
			(for_client && forge_layout_host_brings(entry->definition_index)))
		{
			continue;
		}
		object = object_try_and_get(entry->object_index);
		if (entry->removed && (!object || object->definition_index != entry->definition_index))
		{
			if (!entry->properties.id && !for_client)
				entry->properties.id = forge_layout_new_id();
			forge_layout_add_record(_forge_mvar_object_removed, entry->definition_index, NULL,
				tag_get_group_tag(entry->definition_index), &entry->position, &entry->forward, &entry->up, 0.f,
				&entry->position, &entry->properties);
		}
		else if (object && object->definition_index == entry->definition_index &&
			object->object.parent_object_index == NONE)
		{
			real object_scale = object->object.scale > 0.f ? object->object.scale : 1.f;

			/* (a scale the tools changed is saved; the map's own is 0) */
			if (entry->placed && (real)fabs(object_scale - entry->scale) > FORGE_LAYOUT_SCALED_DIFFERENCE)
				scale = object_scale;
			if (changed || scale > 0.f ||
				(entry->placed &&
					(distance3d(&object->object.position, &entry->position) > FORGE_LAYOUT_MOVED_DISTANCE ||
						dot_product3d(&object->object.forward, &entry->forward) < FORGE_LAYOUT_TURNED_DOT ||
						dot_product3d(&object->object.up, &entry->up) < FORGE_LAYOUT_TURNED_DOT)))
			{
				if (!entry->properties.id && !for_client)
					entry->properties.id = forge_layout_new_id();
				forge_layout_add_record(_forge_mvar_object_moved, entry->definition_index, NULL,
					tag_get_group_tag(entry->definition_index), &object->object.position, &object->object.forward,
					&object->object.up, scale, &entry->position, &entry->properties);
			}
		}
	}

	return;
}

/* the layout as a variant, in the variant buffer: its header (the slot's
name, description and file's times; the clients of a system link game get
none of them), its objects and each mod's part (the mods leave out for the
clients what the host's objects bring them, forge_layout_saving_for_client);
its length, or 0 when it did not all fit */
static unsigned long forge_layout_write_variant(
	short slot,
	boolean for_client)
{
	struct forge_layout_variant const *variant = &forge_layout_globals.variants[slot];
	struct forge_mvar_writer writer;
	struct forge_mvar_header header;
	unsigned long mod_count = 0;
	boolean mod_overflowed = FALSE;
	short index;

	forge_mvar_writer_new(&writer, forge_layout_globals.variant, sizeof(forge_layout_globals.variant));

	forge_layout_collect_records(for_client);
	csmemset(&header, 0, sizeof(header));
	if (!for_client)
	{
		csstrcpy(header.name, variant->name);
		csstrcpy(header.description, variant->description);
		csstrcpy(header.author, variant->author);
		header.created = variant->created;
		header.modified = variant->modified;
		header.listed = variant->listed;
	}
	_snprintf(header.scenario, sizeof(header.scenario), "%s", tag_get_name(global_scenario_index));
	header.scenario[sizeof(header.scenario) - 1] = 0;
	header.next_object_id = forge_layout_globals.next_object_id;
	forge_mvar_chunk_begin(&writer, "mvhd", FORGE_MVAR_VERSION);
	forge_mvar_put_header(&writer, &header);
	forge_mvar_chunk_end(&writer);

	forge_layout_put_records(&writer, for_client);

	forge_layout_globals.saving_for_client = for_client;
	for (index = 0; index < halo_mods_count(); index++)
	{
		if (halo_mods_get(index)->layout_save)
			mod_count++;
	}
	forge_mvar_chunk_begin(&writer, "mvst", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(&writer, mod_count);
	for (index = 0; index < halo_mods_count(); index++)
	{
		struct halo_mod const *mod = halo_mods_get(index);
		struct halo_layout_writer text;

		if (!mod->layout_save)
			continue;
		forge_layout_writer_new(&text, forge_layout_globals.written_text, sizeof(forge_layout_globals.written_text));
		mod->layout_save(&text);
		mod_overflowed = mod_overflowed || text.overflowed;
		forge_mvar_put_text(&writer, mod->name, FORGE_MVAR_MOD_NAME_SIZE);
		forge_mvar_put_u32(&writer, text.length);
		forge_mvar_put_bytes(&writer, text.text, text.length);
	}
	forge_mvar_chunk_end(&writer);
	forge_layout_globals.saving_for_client = FALSE;

	return mod_overflowed ? 0 : forge_mvar_writer_finish(&writer);
}

static unsigned long forge_layout_checksum(
	void const *data,
	unsigned long length)
{
	unsigned char const *bytes = (unsigned char const *)data;
	unsigned long checksum = 5381;
	unsigned long index;

	for (index = 0; index < length; index++)
		checksum = checksum * 33 + bytes[index];

	return checksum;
}

/* the name of local player 0, as a variant's author */
static void forge_layout_author(
	char *author,
	unsigned long size)
{
	long player_index = local_player_get_player_index(0);
	unsigned long length = 0;

	if (player_index != NONE)
	{
		wchar_t const *name = player_get(player_index)->name;
		short index;

		for (index = 0; index < NUMBEROF(player_get(player_index)->name) && name[index] && length < size - 1; index++)
		{
			if (name[index] >= ' ' && name[index] < 127 && name[index] != '%')
				author[length++] = (char)name[index];
		}
	}
	author[length] = 0;

	return;
}

/* the variant buffer's length bytes to the file in forge_maps, whole or
not at all */
static boolean forge_layout_write_file(
	char const *file,
	unsigned long length)
{
	char path[FORGE_LAYOUT_PATH_SIZE];
	char temporary_path[FORGE_LAYOUT_PATH_SIZE];
	FILE *stream;

	_mkdir(FORGE_LAYOUT_DIRECTORY);
	forge_layout_variant_path(file, path, sizeof(path));
	stream = forge_file_begin(path, temporary_path, sizeof(temporary_path));
	if (stream)
		fwrite(forge_layout_globals.variant, 1, length, stream);
	if (!stream || !forge_file_commit(stream, temporary_path, path))
	{
		terminal_printf(global_real_argb_orange, "forge: could not write %s", path);
		return FALSE;
	}

	return TRUE;
}

/* saves the layout as the slot's variant (a new slot's gets a file); only
the game's machine does (a local game, a system link host) */
static boolean forge_layout_save(
	short slot)
{
	struct forge_layout_variant *variant = &forge_layout_globals.variants[slot];
	unsigned long length;

	if (!forge_layout_authoritative())
		return FALSE;
	if (!variant->name[0])
		_snprintf(variant->name, FORGE_LAYOUT_NAME_SIZE, "Variant %d", slot);
	if (!variant->file[0])
	{
		if (!forge_layout_new_file_name(forge_layout_globals.map_name, variant->name, variant->file,
			sizeof(variant->file)))
		{
			terminal_printf(global_real_argb_orange, "forge: no file name is free for %s", variant->name);
			return FALSE;
		}
		variant->created = (unsigned long)time(NULL);
	}
	if (!variant->author[0])
		forge_layout_author(variant->author, sizeof(variant->author));
	variant->modified = (unsigned long)time(NULL);

	if ((length = forge_layout_write_variant(slot, FALSE)) == 0)
	{
		/* (loading reads no more than this either) */
		terminal_printf(global_real_argb_orange, "forge: the variant is too large to save (%d bytes at most)",
			FORGE_MVAR_MAXIMUM_SIZE);
		return FALSE;
	}
	if (!forge_layout_write_file(variant->file, length))
		return FALSE;

	forge_layout_globals.current_slot = slot;
	terminal_printf(global_real_argb_green, "forge: saved %s (forge_maps\\%s)", variant->name, variant->file);

	return TRUE;
}

/* ---------- loading */

/* the map's own object of the tag at the place the map puts it */
static struct forge_layout_map_object *forge_layout_find_map_object(
	long definition_index,
	real_point3d const *position)
{
	short index;

	for (index = 0; index < forge_layout_globals.map_object_count; index++)
	{
		struct forge_layout_map_object *entry = &forge_layout_globals.map_objects[index];

		if (!entry->matched && entry->definition_index == definition_index &&
			distance3d(&entry->position, position) < FORGE_LAYOUT_MATCH_DISTANCE)
		{
			entry->matched = TRUE;
			return entry;
		}
	}

	return NULL;
}

/* the tag of a line: its group and the name at the end of the line */
static long forge_layout_line_tag(
	char const *group,
	char const *name)
{
	long group_tag = forge_layout_group_tag(group);
	long definition_index;

	/* most lines name the tag of the line before: found once (the game
	looks through every tag of the map for a name) */
	if (forge_layout_globals.line_tag_name[0] && group_tag == forge_layout_globals.line_tag_group &&
		strcmp(name, forge_layout_globals.line_tag_name) == 0)
	{
		return forge_layout_globals.line_tag_index;
	}
	definition_index = tag_loaded(group_tag, name);
	if (definition_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge: %s is not in this map, left out", name);
	}
	else if (strlen(name) < sizeof(forge_layout_globals.line_tag_name))
	{
		forge_layout_globals.line_tag_group = group_tag;
		forge_layout_globals.line_tag_index = definition_index;
		csstrcpy(forge_layout_globals.line_tag_name, name);
	}

	return definition_index;
}

/* the tag of a line as an object the tools keep, or NONE: a line may name
any tag of the map, and a client's lines are the host's */
static long forge_layout_line_object(
	char const *group,
	char const *name,
	boolean for_client)
{
	long definition_index;

	if (strlen(group) != 4 || !name[0] || (definition_index = forge_layout_line_tag(group, name)) == NONE)
		return NONE;
	if (!tag_index_is_group(definition_index, OBJECT_DEFINITION_TAG) ||
		!TEST_FLAG(FORGE_LAYOUT_OBJECT_MASK, object_definition_get(definition_index)->object.type) ||
		(for_client && forge_layout_host_brings(definition_index)))
	{
		return NONE;
	}

	return definition_index;
}

/* the record's tag group as four letters */
static void forge_layout_group_letters(
	unsigned long group,
	char letters[5])
{
	letters[0] = (char)(group >> 24);
	letters[1] = (char)(group >> 16);
	letters[2] = (char)(group >> 8);
	letters[3] = (char)group;
	letters[4] = 0;

	return;
}

/* whether this game makes an object with the properties: always while the
tools are on (to be edited); else when it is placed at start and of this
game type */
static boolean forge_layout_made_in_this_game(
	struct forge_object_properties const *properties)
{
	long type = game_engine ? game_engine->type : game_engine_none;

	if (forge_mode_on())
		return TRUE;

	return TEST_FLAG(properties->spawn_flags, 0) &&
		(properties->game_types == 0 || (type > game_engine_none && type < 16 &&
			TEST_FLAG(properties->game_types, type)));
}

/* a record's properties, as this version may take them */
static void forge_layout_record_properties(
	struct forge_mvar_object const *object,
	struct forge_object_properties *properties)
{
	forge_layout_default_properties(properties, object->id);
	properties->team = object->team >= 0 && object->team < 16 ? (short)object->team : NONE;
	properties->spawn_flags = (short)(object->spawn_flags & (FORGE_MVAR_SPAWN_AT_START | FORGE_MVAR_SPAWN_NEVER_RESPAWN));
	properties->respawn_seconds = (short)MIN(object->respawn_seconds, FORGE_OBJECT_MAXIMUM_RESPAWN_SECONDS);
	/* (the game types there are) */
	properties->game_types = (unsigned short)(object->game_types &
		(FLAG(game_engine_ctf) | FLAG(game_engine_slayer) | FLAG(game_engine_oddball) | FLAG(game_engine_king) |
			FLAG(game_engine_race)));

	return;
}

/* the spawned object's properties, or NULL when it is not the layout's */
static struct forge_object_properties *forge_layout_spawned_properties(
	long object_index)
{
	short index;

	for (index = 0; index < forge_layout_globals.spawned_count; index++)
	{
		if (forge_layout_globals.spawned[index] == object_index)
			return &forge_layout_globals.spawned_properties[index];
	}

	return NULL;
}

/* the properties of the layout's object with the id, or NULL */
static struct forge_object_properties *forge_layout_properties_of_id(
	unsigned long id)
{
	short index;

	for (index = 0; id && index < forge_layout_globals.spawned_count; index++)
	{
		if (forge_layout_globals.spawned_properties[index].id == id)
			return &forge_layout_globals.spawned_properties[index];
	}
	for (index = 0; id && index < forge_layout_globals.map_object_count; index++)
	{
		if (forge_layout_globals.map_objects[index].properties.id == id)
			return &forge_layout_globals.map_objects[index].properties;
	}

	return NULL;
}

/* an object of a variant: made, moved or removed as the record says, when
it can be (a record comes from a file or the host); path is its tag's */
static void forge_layout_apply_record(
	struct forge_mvar_object const *object,
	char const *path,
	boolean for_client)
{
	struct forge_object_properties properties;
	char group[5];
	real_point3d position;
	real_point3d original;
	real_vector3d forward;
	real_vector3d up;
	long definition_index;

	forge_layout_group_letters(object->group, group);
	if (object->kind < 0 || object->kind >= NUMBER_OF_FORGE_MVAR_OBJECT_KINDS ||
		(definition_index = forge_layout_line_object(group, path, for_client)) == NONE)
	{
		return;
	}
	set_real_point3d(&position, object->position[0], object->position[1], object->position[2]);
	set_real_point3d(&original, object->original[0], object->original[1], object->original[2]);
	/* (what cannot be, from a file or the host: not made) */
	if (!distributed_point_valid(&position, FORGE_LAYOUT_WORLD_BOUND) ||
		!distributed_point_valid(&original, FORGE_LAYOUT_WORLD_BOUND) ||
		!forge_layout_axes_of_rotation(object->rotation, &forward, &up))
	{
		return;
	}
	forge_layout_record_properties(object, &properties);

	if (object->kind == _forge_mvar_object_placed)
	{
		real scale = object->scale;
		long object_index;

		/* (a scale that is none, or out of bounds: the tag's own) */
		if (!(scale >= FORGE_LAYOUT_MINIMUM_SCALE && scale <= FORGE_LAYOUT_MAXIMUM_SCALE))
			scale = 0.f;
		if (!for_client && !forge_layout_made_in_this_game(&properties))
		{
			/* kept, to be saved again as it is */
			if (forge_layout_globals.dormant_count < FORGE_LAYOUT_MAXIMUM_SPAWNED)
			{
				struct forge_layout_record *record = &forge_layout_globals.dormant[forge_layout_globals.dormant_count++];

				csmemset(record, 0, sizeof(*record));
				record->object = *object;
				record->object.scale = scale;
				record->editor.id = object->id;
				record->definition_index = definition_index;
			}
			return;
		}
		if (forge_layout_globals.spawned_count >= FORGE_LAYOUT_MAXIMUM_SPAWNED)
			return;
		/* (as it was saved, whatever the game type's vehicles and weapons) */
		forge_layout_creating = TRUE;
		object_index = forge_layout_new_object(definition_index, &position, &forward, &up, scale);
		forge_layout_creating = FALSE;
		if (object_index != NONE)
		{
			struct forge_object_properties *kept;

			forge_layout_note_spawned(object_index);
			if ((kept = forge_layout_spawned_properties(object_index)) != NULL)
				*kept = properties;
		}
	}
	else
	{
		struct forge_layout_map_object *entry = forge_layout_find_map_object(definition_index, &original);

		if (!entry || !object_try_and_get(entry->object_index))
			return;
		if (object->kind == _forge_mvar_object_moved)
		{
			real scale = object->scale;

			forge_layout_place(entry->object_index, &position, &forward, &up);
			/* (0, or out of bounds: the map's own) */
			if (scale >= FORGE_LAYOUT_MINIMUM_SCALE && scale <= FORGE_LAYOUT_MAXIMUM_SCALE)
				objects_scripting_set_scale(entry->object_index, scale, 0);
			entry->placed = TRUE;
			entry->properties = properties;
		}
		/* (never a flag or ball, forge_object_holds_flag) */
		else if (!forge_object_holds_flag(entry->object_index))
		{
			object_delete(entry->object_index);
			entry->removed = TRUE;
			entry->properties.id = properties.id;
		}
	}

	return;
}

/* a mod's part of a variant, its lines one by one */
static void forge_layout_apply_mod_text(
	struct halo_mod const *mod,
	unsigned char const *text,
	unsigned long length)
{
	unsigned long offset = 0;

	while (offset < length)
	{
		char line[FORGE_LAYOUT_LINE_SIZE];
		unsigned long line_length = 0;

		while (offset + line_length < length && text[offset + line_length] != '\n')
			line_length++;
		_snprintf(line, sizeof(line), "%.*s", (int)MIN(line_length, sizeof(line) - 1), (char const *)text + offset);
		line[sizeof(line) - 1] = 0;
		/* (a line may hold anything: it ends at its first zero) */
		forge_layout_chomp(line);
		offset += line_length + 1;
		if (line[0] && line[0] != '#')
			mod->layout_load(line);
	}

	return;
}

/* the string of the variant's string table */
static boolean forge_layout_variant_string(
	void const *data,
	unsigned long length,
	unsigned int index,
	char *result,
	unsigned long size)
{
	struct forge_mvar_reader reader;

	if (index >= forge_layout_globals.string_count)
		return FALSE;
	reader.data = (unsigned char const *)data;
	reader.length = length;
	reader.offset = forge_layout_globals.string_offsets[index];
	reader.failed = FALSE;

	return forge_mvar_get_string(&reader, result, size);
}

/* a variant: the map as it has it, then the variant's objects, each mod's
part and the editor's notes; on a system link client (for_client), only
what the host's objects do not bring (the tools' scenery and devices, the
mods' parts: the mods leave out what is the host's,
forge_layout_loading_for_client). FALSE, with nothing changed, when it is
not a variant or is damaged. */
static boolean forge_layout_apply_variant(
	void const *data,
	unsigned long length,
	boolean for_client)
{
	struct forge_mvar_reader reader;
	struct forge_mvar_reader chunk;
	char tag[5];
	unsigned int version;
	unsigned long next_object_id = 1;
	short index;

	if (!forge_mvar_open(&reader, data, length))
		return FALSE;

	forge_layout_globals.loading_for_client = for_client;
	forge_layout_globals.string_count = 0;
	forge_layout_reset();
	while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
	{
		unsigned long count;
		unsigned long record_size;
		unsigned long record_index;

		if (strcmp(tag, "mvhd") == 0)
		{
			struct forge_mvar_header header;

			if (forge_mvar_get_header(&chunk, &header))
				next_object_id = MAX(next_object_id, header.next_object_id);
		}
		else if (strcmp(tag, "strt") == 0)
		{
			count = forge_mvar_get_u32(&chunk);
			for (record_index = 0; record_index < count && record_index < FORGE_LAYOUT_MAXIMUM_RECORDS && !chunk.failed;
				record_index++)
			{
				forge_layout_globals.string_offsets[record_index] =
					(unsigned long)(chunk.data - (unsigned char const *)data) + chunk.offset;
				forge_mvar_get_bytes(&chunk, forge_mvar_get_u16(&chunk));
				if (!chunk.failed)
					forge_layout_globals.string_count = record_index + 1;
			}
		}
		else if (strcmp(tag, "mvob") == 0 && forge_mvar_get_table(&chunk, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size))
		{
			for (record_index = 0; record_index < count; record_index++)
			{
				struct forge_mvar_object object;
				char path[FORGE_MVAR_MAXIMUM_STRING_LENGTH + 1];

				if (!forge_mvar_get_object(&chunk, record_size, &object))
					break;
				next_object_id = MAX(next_object_id, object.id + 1);
				if (forge_layout_variant_string(data, length, object.tag, path, sizeof(path)))
					forge_layout_apply_record(&object, path, for_client);
			}
		}
		else if (strcmp(tag, "mved") == 0 && forge_mvar_get_table(&chunk, FORGE_MVAR_EDITOR_RECORD_SIZE, &count, &record_size))
		{
			for (record_index = 0; record_index < count; record_index++)
			{
				struct forge_mvar_editor editor;
				struct forge_object_properties *properties;

				if (!forge_mvar_get_editor(&chunk, record_size, &editor))
					break;
				editor.group = MIN(editor.group, FORGE_OBJECT_MAXIMUM_GROUP);
				editor.flags &= FORGE_MVAR_EDITOR_LOCKED;
				forge_layout_clean_text(editor.comment, editor.comment, sizeof(editor.comment));
				if ((properties = forge_layout_properties_of_id(editor.id)) != NULL)
				{
					properties->group = (short)editor.group;
					properties->locked = TEST_FLAG(editor.flags, 0);
					csstrcpy(properties->comment, editor.comment);
				}
				/* (an object this game did not make keeps its notes too) */
				for (index = 0; editor.id && index < forge_layout_globals.dormant_count; index++)
				{
					if (forge_layout_globals.dormant[index].object.id == editor.id)
						forge_layout_globals.dormant[index].editor = editor;
				}
			}
		}
		else if (strcmp(tag, "mvst") == 0)
		{
			count = forge_mvar_get_u32(&chunk);
			for (record_index = 0; record_index < count && !chunk.failed; record_index++)
			{
				char name[FORGE_MVAR_MOD_NAME_SIZE];
				unsigned long text_length;
				unsigned char const *text;

				forge_mvar_get_text(&chunk, name, sizeof(name));
				text_length = forge_mvar_get_u32(&chunk);
				if ((text = forge_mvar_get_bytes(&chunk, text_length)) == NULL)
					break;
				for (index = 0; index < halo_mods_count(); index++)
				{
					struct halo_mod const *mod = halo_mods_get(index);

					if (mod->layout_load && strcmp(mod->name, name) == 0)
						forge_layout_apply_mod_text(mod, text, text_length);
				}
			}
		}
	}
	forge_layout_globals.loading_for_client = FALSE;
	/* (ids go on from the variant's) */
	forge_layout_globals.next_object_id = MAX(forge_layout_globals.next_object_id, next_object_id);

	return TRUE;
}

static void forge_layout_host_sync(
	boolean send);

static boolean forge_layout_load(
	short slot)
{
	char path[FORGE_LAYOUT_PATH_SIZE];
	long length;

	forge_layout_variant_path(forge_layout_globals.variants[slot].file, path, sizeof(path));
	length = forge_layout_read_file(path, forge_layout_globals.variant, sizeof(forge_layout_globals.variant));
	if (length < 0 || !forge_layout_apply_variant(forge_layout_globals.variant, (unsigned long)length, FALSE))
	{
		terminal_printf(global_real_argb_orange, "forge: %s is not a Forge map variant, or is damaged (or over %d bytes)",
			path, FORGE_MVAR_MAXIMUM_SIZE - 1);
		return FALSE;
	}

	forge_layout_globals.current_slot = slot;
	forge_layout_globals.chosen_slot = slot;
	terminal_printf(global_real_argb_green, "forge: loaded %s", forge_layout_globals.variants[slot].name);
	/* (a system link host: its clients get it now) */
	forge_layout_host_sync(TRUE);

	return TRUE;
}

/* ---------- the layouts of before */

/* a layout of before's [forge] line ("spawn", "move" or "remove", its tag's
group, numbers, its tag's path) as a record; the path is kept in the pool */
static void forge_layout_old_line(
	char const *line,
	char *pool,
	unsigned long pool_size,
	unsigned long *pool_length)
{
	char group[5] = { 0 };
	float values[13];
	int name_offset = 0;
	int kind;
	real_point3d position;
	real_point3d original;
	real_vector3d forward;
	real_vector3d up;
	real scale = 0.f;
	struct forge_object_properties properties;
	char const *name;
	unsigned long name_length;

	if (sscanf(line, "spawn %4s %f %f %f %f %f %f %f %f %f %f %n", group,
		&values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
		&values[6], &values[7], &values[8], &values[9], &name_offset) >= 11 && name_offset > 0)
	{
		kind = _forge_mvar_object_placed;
		set_real_point3d(&position, values[0], values[1], values[2]);
		original = position;
		set_real_vector3d(&forward, values[3], values[4], values[5]);
		set_real_vector3d(&up, values[6], values[7], values[8]);
		scale = values[9];
	}
	else if (sscanf(line, "move %4s %f %f %f %f %f %f %f %f %f %f %f %f %n", group,
		&values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
		&values[6], &values[7], &values[8], &values[9], &values[10], &values[11], &name_offset) >= 13 &&
		name_offset > 0)
	{
		kind = _forge_mvar_object_moved;
		set_real_point3d(&original, values[0], values[1], values[2]);
		set_real_point3d(&position, values[3], values[4], values[5]);
		set_real_vector3d(&forward, values[6], values[7], values[8]);
		set_real_vector3d(&up, values[9], values[10], values[11]);
	}
	else if (sscanf(line, "remove %4s %f %f %f %n", group, &values[0], &values[1], &values[2], &name_offset) >= 4 &&
		name_offset > 0)
	{
		kind = _forge_mvar_object_removed;
		set_real_point3d(&original, values[0], values[1], values[2]);
		position = original;
		forward = *global_forward3d;
		up = *global_up3d;
	}
	else
	{
		return;
	}

	name = line + name_offset;
	name_length = (unsigned long)strlen(name);
	/* (what the game could not read either is left out; the record's numbers
	are checked when the variant is loaded) */
	if (strlen(group) != 4 || !name_length || name_length > FORGE_MVAR_MAXIMUM_STRING_LENGTH ||
		*pool_length + name_length + 1 > pool_size || !distributed_axes_make_valid(&forward, &up))
	{
		return;
	}
	csmemcpy(pool + *pool_length, name, name_length + 1);
	forge_layout_default_properties(&properties, forge_layout_new_id());
	forge_layout_add_record(kind, NONE, pool + *pool_length, (unsigned long)forge_layout_group_tag(group), &position,
		&forward, &up, scale, &original, &properties);
	*pool_length += name_length + 1;

	return;
}

/* the layout of before in the slot of the map's folder of u:\forge as a
variant in forge_maps; the file it was written to in file, or FALSE */
static boolean forge_layout_convert_old_layout(
	char const *map_name,
	short slot,
	char *file,
	unsigned long file_size)
{
	static char pool[FORGE_LAYOUT_MAXIMUM_TEXT];
	char path[FORGE_LAYOUT_PATH_SIZE];
	char *text = forge_layout_globals.written_text;
	char const *next;
	struct forge_mvar_writer writer;
	struct forge_mvar_header header;
	unsigned long pool_length = 0;
	unsigned long mod_count = 0;
	unsigned long length;
	boolean forge_section = FALSE;
	boolean in_header = TRUE;

	forge_layout_old_slot_path(map_name, slot, path, sizeof(path));
	if (forge_layout_read_file(path, text, FORGE_LAYOUT_MAXIMUM_TEXT) < 0)
		return FALSE;

	csmemset(&header, 0, sizeof(header));
	_snprintf(header.name, sizeof(header.name), "Layout %d", slot);
	_snprintf(header.scenario, sizeof(header.scenario), "levels\\test\\%s\\%s", map_name, map_name);
	header.created = header.modified = (unsigned long)time(NULL);
	forge_layout_globals.record_count = 0;
	forge_layout_globals.next_object_id = 1;
	/* the header, the [forge] part's objects, and how many mods' parts */
	for (next = text; *next;)
	{
		char line[FORGE_LAYOUT_LINE_SIZE];
		unsigned long line_length = (unsigned long)strcspn(next, "\n");

		_snprintf(line, sizeof(line), "%.*s", (int)MIN(line_length, sizeof(line) - 1), next);
		line[sizeof(line) - 1] = 0;
		next += line_length;
		if (*next == '\n')
			next++;
		forge_layout_chomp(line);
		if (line[0] == '[')
		{
			in_header = FALSE;
			forge_section = strncmp(line, "[forge]", 7) == 0;
			if (!forge_section)
				mod_count++;
		}
		else if (in_header && strncmp(line, "name ", 5) == 0 && line[5])
		{
			forge_layout_clean_text(line + 5, header.name, sizeof(header.name));
		}
		else if (in_header && strncmp(line, "description ", 12) == 0)
		{
			forge_layout_clean_text(line + 12, header.description, sizeof(header.description));
		}
		else if (in_header && strcmp(line, "listed 1") == 0)
		{
			header.listed = TRUE;
		}
		else if (in_header && strncmp(line, "map ", 4) == 0 && line[4])
		{
			_snprintf(header.scenario, sizeof(header.scenario), "%s", line + 4);
			header.scenario[sizeof(header.scenario) - 1] = 0;
		}
		else if (forge_section && line[0] && line[0] != '#')
		{
			forge_layout_old_line(line, pool, sizeof(pool), &pool_length);
		}
	}
	if (!header.name[0])
		_snprintf(header.name, sizeof(header.name), "Layout %d", slot);
	header.next_object_id = forge_layout_globals.next_object_id;

	forge_mvar_writer_new(&writer, forge_layout_globals.variant, sizeof(forge_layout_globals.variant));
	forge_mvar_chunk_begin(&writer, "mvhd", FORGE_MVAR_VERSION);
	forge_mvar_put_header(&writer, &header);
	forge_mvar_chunk_end(&writer);
	forge_layout_put_records(&writer, FALSE);
	/* each mod's part as its lines were */
	forge_mvar_chunk_begin(&writer, "mvst", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(&writer, mod_count);
	for (next = text; *next;)
	{
		unsigned long line_length = (unsigned long)strcspn(next, "\n");

		if (next[0] == '[' && strncmp(next, "[forge]", 7) != 0)
		{
			char name[FORGE_MVAR_MOD_NAME_SIZE];
			unsigned long name_length = (unsigned long)strcspn(next + 1, "]\r\n");
			char const *part = next + line_length + (next[line_length] == '\n');
			char const *end = part;

			while (*end && *end != '[')
				end += strcspn(end, "\n") + (end[strcspn(end, "\n")] == '\n');
			_snprintf(name, sizeof(name), "%.*s", (int)MIN(name_length, sizeof(name) - 1), next + 1);
			name[sizeof(name) - 1] = 0;
			forge_mvar_put_text(&writer, name, sizeof(name));
			forge_mvar_put_u32(&writer, (unsigned long)(end - part));
			forge_mvar_put_bytes(&writer, part, (unsigned long)(end - part));
		}
		next += line_length;
		if (*next == '\n')
			next++;
	}
	forge_mvar_chunk_end(&writer);
	if ((length = forge_mvar_writer_finish(&writer)) == 0)
		return FALSE;

	return forge_layout_new_file_name(map_name, header.name, file, file_size) && forge_layout_write_file(file, length);
}

/* every layout of before not made a variant yet becomes one: once, before
the variants are first looked for. The slots made are noted in the map's
folder of u:\forge (converted.txt, a line "NN file" each); the layouts stay
where they are. */
static void forge_layout_convert_old_layouts(
	void)
{
	static boolean converted = FALSE;
	WIN32_FIND_DATAA data;
	HANDLE find;
	long id_before = forge_layout_globals.next_object_id;

	if (converted)
		return;
	converted = TRUE;
	find = FindFirstFileA("u:\\forge\\*", &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char map_name[sizeof(data.cFileName) + 1];
		char path[FORGE_LAYOUT_PATH_SIZE];
		boolean done[FORGE_LAYOUT_OLD_SLOT_COUNT + 1];
		FILE *notes;
		short slot;

		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == '.')
			continue;
		csmemcpy(map_name, data.cFileName, sizeof(data.cFileName));
		map_name[sizeof(data.cFileName)] = 0;
		if (strlen(map_name) >= FORGE_LAYOUT_NAME_SIZE)
			continue;
		csmemset(done, 0, sizeof(done));
		_snprintf(path, sizeof(path), "u:\\forge\\%s\\converted.txt", map_name);
		path[sizeof(path) - 1] = 0;
		if ((notes = fopen(path, "r")) != NULL)
		{
			char line[FORGE_LAYOUT_LINE_SIZE];
			int noted;

			while (fgets(line, sizeof(line), notes))
			{
				if (sscanf(line, "%d", &noted) == 1 && noted >= 1 && noted <= FORGE_LAYOUT_OLD_SLOT_COUNT)
					done[noted] = TRUE;
			}
			fclose(notes);
		}
		for (slot = 1; slot <= FORGE_LAYOUT_OLD_SLOT_COUNT; slot++)
		{
			char old_path[FORGE_LAYOUT_PATH_SIZE];
			char file[FORGE_LAYOUT_FILE_SIZE];
			FILE *old_file;

			forge_layout_old_slot_path(map_name, slot, old_path, sizeof(old_path));
			if (done[slot] || (old_file = fopen(old_path, "rb")) == NULL)
				continue;
			fclose(old_file);
			if (!forge_layout_convert_old_layout(map_name, slot, file, sizeof(file)))
			{
				platform_log("forge: could not make %s a map variant", old_path);
				continue;
			}
			if ((notes = fopen(path, "a")) != NULL)
			{
				fprintf(notes, "%02d %s\n", slot, file);
				fclose(notes);
			}
			platform_log("forge: %s is now forge_maps\\%s", old_path, file);
		}
	} while (FindNextFileA(find, &data));
	CloseHandle(find);
	forge_layout_globals.record_count = 0;
	forge_layout_globals.next_object_id = id_before;

	return;
}

/* ---------- the map list */

/* writes the slot's name, description and place in the map list again,
over those its file's mvhd has; FALSE when there is no file or it could not
be written */
static boolean forge_layout_rewrite_header(
	short slot)
{
	struct forge_layout_variant const *variant = &forge_layout_globals.variants[slot];
	char path[FORGE_LAYOUT_PATH_SIZE];
	struct forge_mvar_reader reader;
	struct forge_mvar_reader chunk;
	char tag[5];
	unsigned int version;
	long length;

	if (!variant->file[0])
		return FALSE;
	forge_layout_variant_path(variant->file, path, sizeof(path));
	length = forge_layout_read_file(path, forge_layout_globals.variant, sizeof(forge_layout_globals.variant));
	if (length < 0 || !forge_mvar_open(&reader, forge_layout_globals.variant, (unsigned long)length))
		return FALSE;
	while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
	{
		struct forge_mvar_header header;
		struct forge_mvar_writer writer;

		if (strcmp(tag, "mvhd") != 0 || !forge_mvar_get_header(&chunk, &header))
			continue;
		csstrcpy(header.name, variant->name);
		csstrcpy(header.description, variant->description);
		header.listed = variant->listed;
		/* (where it is: the rest of the file stays as it is) */
		writer.data = (unsigned char *)chunk.data;
		writer.size = chunk.length;
		writer.length = 0;
		writer.chunk_start = 0;
		writer.overflowed = FALSE;
		forge_mvar_put_header(&writer, &header);
		forge_mvar_seal(forge_layout_globals.variant, (unsigned long)length);

		return !writer.overflowed && forge_layout_write_file(variant->file, (unsigned long)length);
	}

	return FALSE;
}

/* puts the variant in the map list or takes it out: its mvhd's listed */
static boolean forge_layout_set_listed(
	short slot,
	boolean listed)
{
	boolean was_listed = forge_layout_globals.variants[slot].listed;
	boolean written;

	forge_layout_globals.variants[slot].listed = listed;
	written = forge_layout_rewrite_header(slot);
	if (!written)
		forge_layout_globals.variants[slot].listed = was_listed;

	return written;
}

/* ---------- typing a name or a description (the Map tab) */

static void forge_layout_name_typed(
	char const *text)
{
	short slot = forge_layout_globals.typing_slot;
	char name[FORGE_LAYOUT_NAME_SIZE];

	forge_layout_clean_text(text, name, sizeof(name));
	if (!name[0])
	{
		terminal_printf(global_real_argb_orange, "forge: the name is empty, the variant keeps its own");
		return;
	}
	csstrcpy(forge_layout_globals.variants[slot].name, name);
	/* (a new variant gets it when it is saved; its file keeps its name) */
	if (slot == 0 || forge_layout_rewrite_header(slot))
		terminal_printf(global_real_argb_green, "forge: the variant is now called %s", name);
	else
		terminal_printf(global_real_argb_orange, "forge: could not write the variant's file");

	return;
}

static void forge_layout_description_typed(
	char const *text)
{
	short slot = forge_layout_globals.typing_slot;

	forge_layout_clean_text(text, forge_layout_globals.variants[slot].description, FORGE_LAYOUT_DESCRIPTION_SIZE);
	if (slot == 0 || forge_layout_rewrite_header(slot))
	{
		terminal_printf(global_real_argb_green, forge_layout_globals.variants[slot].description[0]
			? "forge: the description is changed"
			: "forge: the description is gone (the map list says which map it is made on)");
	}
	else
	{
		terminal_printf(global_real_argb_orange, "forge: could not write the variant's file");
	}

	return;
}

/* ---------- the Map tab */

/* the slot a new variant is saved in, or 0 when the map has as many as the
Map tab lists */
static short forge_layout_first_free_slot(
	void)
{
	return forge_layout_globals.variant_count < FORGE_LAYOUT_SLOT_COUNT
		? (short)(forge_layout_globals.variant_count + 1)
		: 0;
}

/* saves the layout in the slot, a new variant's there with the name and
description typed for it; the slot it is in afterwards (the variants are
listed by file name), or 0 when it was not saved */
static short forge_layout_save_slot(
	short slot)
{
	boolean new_variant = slot == 0;
	char file[FORGE_LAYOUT_FILE_SIZE];

	if (new_variant && (slot = forge_layout_first_free_slot()) == 0)
	{
		terminal_printf(global_real_argb_orange, "forge: this map has %d variants, the most the Map tab lists",
			FORGE_LAYOUT_SLOT_COUNT);
		return 0;
	}
	if (new_variant)
	{
		csmemset(&forge_layout_globals.variants[slot], 0, sizeof(forge_layout_globals.variants[slot]));
		csstrcpy(forge_layout_globals.variants[slot].name, forge_layout_globals.variants[0].name);
		csstrcpy(forge_layout_globals.variants[slot].description, forge_layout_globals.variants[0].description);
	}
	if (!forge_layout_save(slot))
	{
		if (new_variant)
			csmemset(&forge_layout_globals.variants[slot], 0, sizeof(forge_layout_globals.variants[slot]));
		return 0;
	}
	csstrcpy(file, forge_layout_globals.variants[slot].file);
	forge_layout_globals.variants[0].name[0] = 0;
	forge_layout_globals.variants[0].description[0] = 0;
	forge_layout_read_slots();
	slot = forge_layout_slot_of_file(file);
	forge_layout_globals.current_slot = slot;

	return slot;
}

static char const *forge_layout_chosen_name(
	void)
{
	return forge_layout_globals.chosen_slot
		? forge_layout_globals.variants[forge_layout_globals.chosen_slot].name
		: "(a new variant)";
}

static short forge_layout_menu_row_count(
	void)
{
	return NUMBER_OF_FORGE_LAYOUT_ROWS;
}

/* whether "Remove all objects" has been chosen once, a moment ago */
static boolean forge_layout_remove_all_asked(
	void)
{
	long asked_time = forge_layout_globals.remove_all_time;

	return asked_time != NONE && game_time_get() >= asked_time &&
		game_time_get() - asked_time < FORGE_LAYOUT_CONFIRM_TICKS;
}

static void forge_layout_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	short chosen = forge_layout_globals.chosen_slot;

	switch (row)
	{
	case _forge_layout_row_layout:
		_snprintf(label, label_size, "Variant");
		_snprintf(value, value_size, "%s", forge_layout_chosen_name());
		break;
	case _forge_layout_row_name:
		_snprintf(label, label_size, "Name");
		_snprintf(value, value_size, "%s", forge_layout_globals.variants[chosen].name[0]
			? forge_layout_globals.variants[chosen].name
			: "(type one)");
		break;
	case _forge_layout_row_description:
		_snprintf(label, label_size, "Description");
		_snprintf(value, value_size, "%s", forge_layout_globals.variants[chosen].description[0]
			? forge_layout_globals.variants[chosen].description
			: "(type one)");
		break;
	case _forge_layout_row_save:
		_snprintf(label, label_size, "Save");
		_snprintf(value, value_size, "%s", chosen ? "over it" : "as a new variant");
		break;
	case _forge_layout_row_load:
		_snprintf(label, label_size, "Load it");
		_snprintf(value, value_size, "%s", chosen && chosen == forge_layout_globals.current_slot ? "loaded" : "");
		break;
	case _forge_layout_row_listed:
		_snprintf(label, label_size, "Show in the map list");
		_snprintf(value, value_size, "%s", chosen && forge_layout_globals.variants[chosen].listed ? "on" : "off");
		break;
	case _forge_layout_row_reset:
		_snprintf(label, label_size, "Reset the map");
		break;
	case _forge_layout_row_remove_all:
		_snprintf(label, label_size, "Remove all objects");
		_snprintf(value, value_size, "%s", forge_layout_remove_all_asked() ? "again to confirm" : "");
		break;
	case _forge_layout_row_delete:
		_snprintf(label, label_size, "Delete it");
		break;
	}

	return;
}

static int forge_layout_menu_row_change(
	short row,
	int direction)
{
	short chosen = forge_layout_globals.chosen_slot;
	boolean remove_all_asked = forge_layout_remove_all_asked();

	/* (any other row: not asked any more) */
	forge_layout_globals.remove_all_time = NONE;
	if (!forge_layout_authoritative())
	{
		if (direction == 0 || row != _forge_layout_row_layout)
			terminal_printf(global_real_argb_orange, "forge: the variants are the host's");
		return FALSE;
	}

	switch (row)
	{
	case _forge_layout_row_layout:
		if (direction != 0)
		{
			/* the saved variants, then a new one */
			short count = (short)(forge_layout_globals.variant_count + 1);

			forge_layout_globals.chosen_slot = (short)((chosen + count + (direction < 0 ? -1 : 1)) % count);
		}
		break;
	case _forge_layout_row_name:
	case _forge_layout_row_description:
		if (direction == 0)
		{
			/* typed on the keyboard (forge.c); the launcher changes them too */
			boolean name = row == _forge_layout_row_name;

			forge_layout_globals.typing_slot = chosen;
			if (!forge_text_entry_begin(
				name ? "Name of the variant" : "Description of the variant (the map list shows it)",
				name ? forge_layout_globals.variants[chosen].name : forge_layout_globals.variants[chosen].description,
				name ? FORGE_LAYOUT_NAME_SIZE - 1 : FORGE_LAYOUT_DESCRIPTION_SIZE - 1,
				name ? forge_layout_name_typed : forge_layout_description_typed))
			{
				terminal_printf(global_real_argb_orange, "forge: something is being typed already");
			}
		}
		break;
	case _forge_layout_row_save:
		if (direction == 0 && (chosen = forge_layout_save_slot(chosen)) != 0)
			forge_layout_globals.chosen_slot = chosen;
		break;
	case _forge_layout_row_load:
		if (direction == 0)
		{
			if (chosen == 0)
				terminal_printf(global_real_argb_orange, "forge: choose a saved variant first");
			else
				forge_layout_load(chosen);
		}
		break;
	case _forge_layout_row_listed:
		if (chosen == 0)
		{
			terminal_printf(global_real_argb_orange, "forge: save the variant first");
		}
		else if (forge_layout_set_listed(chosen, !forge_layout_globals.variants[chosen].listed))
		{
			terminal_printf(global_real_argb_green, forge_layout_globals.variants[chosen].listed
				? "forge: %s is in the multiplayer map list"
				: "forge: %s is out of the map list", forge_layout_globals.variants[chosen].name);
		}
		break;
	case _forge_layout_row_reset:
		if (direction == 0)
		{
			forge_layout_reset();
			terminal_printf(global_real_argb_green, "forge: the map is back to its own look");
		}
		break;
	case _forge_layout_row_remove_all:
		if (direction != 0)
		{
			/* nothing */
		}
		else if (!remove_all_asked)
		{
			forge_layout_globals.remove_all_time = game_time_get();
			terminal_printf(global_real_argb_orange,
				"forge: choose it again to remove every object (Reset the map brings back only the map's own)");
		}
		else
		{
			/* (weapons and powerups come back where the map spawns them:
			the Spawns tab, forge_spawns.c) */
			terminal_printf(global_real_argb_green, "forge: removed %d objects (items return at their spawns)",
				forge_layout_remove_all());
			forge_layout_host_sync(TRUE);
		}
		break;
	case _forge_layout_row_delete:
		if (direction == 0 && chosen != 0)
		{
			char path[FORGE_LAYOUT_PATH_SIZE];

			forge_layout_variant_path(forge_layout_globals.variants[chosen].file, path, sizeof(path));
			remove(path);
			terminal_printf(global_real_argb_green, "forge: deleted %s", forge_layout_globals.variants[chosen].name);
			if (forge_layout_globals.current_slot == chosen)
				forge_layout_globals.current_slot = 0;
			forge_layout_globals.chosen_slot = 0;
			forge_layout_read_slots();
		}
		break;
	}

	return FALSE;
}

static void forge_layout_menu_opened(
	void)
{
	forge_layout_globals.remove_all_time = NONE;
	forge_layout_read_slots();
	if (forge_layout_globals.chosen_slot > forge_layout_globals.variant_count)
		forge_layout_globals.chosen_slot = 0;
	if (forge_layout_globals.current_slot > forge_layout_globals.variant_count)
		forge_layout_globals.current_slot = 0;

	return;
}

static struct halo_mod_menu const forge_layout_menu =
{
	"Map",
	forge_layout_menu_row_count,
	forge_layout_menu_row_text,
	forge_layout_menu_row_change,
	forge_layout_menu_opened
};

static void forge_layout_debug_update(
	void);

struct halo_mod const forge_layout_mod =
{
	"forge_layout",
	forge_layout_debug_update,
	NULL,
	NULL,
	NULL,
	NULL,
	&forge_layout_menu
};

/* port: SaftiGlas's test hooks, kept (forge-dev-tools), off unless set.
debug.forge_layout_save (port_config.c): a few seconds into a local game,
saves the layout to that slot, once, and logs where local player 0 is, for
automated tests; 0 never */
long config_integer(char const *name);
/* (and game.forge: forge_custom_maps_refresh) */
int config_boolean(char const *name);

/* debug.forge_test_edit (port_config.c): in a system link game, the host
spawns a scenery object beside its player a few seconds in; a client asks
the host to spawn one beside its own, then to move it, then to remove it */
static void forge_layout_test_edit(
	void)
{
	static long enabled = -1;
	static long step_time;
	static short step;
	long player_index;
	struct unit_datum *unit;
	struct tag_iterator iterator;
	long definition_index;
	real_point3d position;
	real_point3d moved;

	if (enabled < 0)
		enabled = config_integer("debug.forge_test_edit");
	if (enabled <= 0)
		return;
	player_index = local_player_get_player_index(0);
	unit = player_index != NONE ? unit_try_and_get(player_get(player_index)->unit_index) : NULL;
	if (!unit || game_connection() == _game_connection_local || !forge_mode_on())
		return;
	/* (a new game starts over) */
	if (game_time_get() < step_time)
		step = 0;
	if (step >= 3 || game_time_get() < 300 + step * 150)
		return;
	step_time = game_time_get();

	tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
	while ((definition_index = tag_iterator_next(&iterator)) != NONE &&
		object_definition_get(definition_index)->object.type != _object_type_scenery)
	{
	}
	if (definition_index == NONE)
		return;
	/* (each machine's own a little apart) */
	position = unit->object.position;
	position.x += game_connection() == _game_connection_network_server ? 2.f : -2.f;
	moved = position;
	moved.z += 1.f;

	if (game_connection() == _game_connection_network_server)
	{
		if (step == 0)
		{
			long object_index = forge_layout_new_object(definition_index, &position, global_forward3d, global_up3d, 0.f);

			forge_layout_note_spawned(object_index);
			platform_log("forge test: the host spawned %s (%s)", tag_get_name(definition_index),
				object_index != NONE ? "made" : "not made");
		}
	}
	else
	{
		forge_layout_client_edit(step == 0 ? _forge_edit_spawn : step == 1 ? _forge_edit_move : _forge_edit_remove,
			NONE, definition_index, step == 2 ? &moved.x : &position.x, step == 0 ? &position.x : &moved.x,
			&global_forward3d->i, &global_up3d->i);
		platform_log("forge test: the client asked the host to %s %s",
			step == 0 ? "spawn" : step == 1 ? "move" : "remove", tag_get_name(definition_index));
	}
	step++;

	return;
}

static void forge_layout_debug_update(
	void)
{
	static long slot = -1;

	/* a system link host: what has changed, to its clients, now and then
	(not while something is held: it changes every frame) */
	if (game_connection() == _game_connection_network_server && forge_layout_globals.map_name[0] != 0 &&
		(game_time_get() < forge_layout_globals.sync_time ||
			game_time_get() - forge_layout_globals.sync_time >= FORGE_LAYOUT_SYNC_INTERVAL_TICKS) &&
		forge_mode_on() && !forge_busy())
	{
		forge_layout_host_sync(TRUE);
	}

	forge_layout_test_edit();

	if (slot < 0)
		slot = config_integer("debug.forge_layout_save");
	/* port: (in a Forge game, not the main menu's; fifteen seconds in, after
	debug.forge_test_edit's spawn) */
	if (slot >= 1 && slot <= FORGE_LAYOUT_SLOT_COUNT && game_time_get() >= 450 && forge_layout_authoritative() &&
		forge_mode_on())
	{
		long player_index = local_player_get_player_index(0);
		struct unit_datum *unit = player_index != NONE ? unit_try_and_get(player_get(player_index)->unit_index) : NULL;
		short saved_slot;

		/* (a slot past the variants there are: a new one) */
		forge_layout_read_slots();
		saved_slot = forge_layout_save_slot(slot <= forge_layout_globals.variant_count ? (short)slot : 0);
		platform_log("forge layout: saved slot %d (%s), %d objects placed", (int)saved_slot,
			saved_slot ? forge_layout_globals.variants[saved_slot].file : "not saved",
			(int)forge_layout_globals.spawned_count);
		if (unit)
		{
			platform_log("forge layout: player at %.3f %.3f %.3f facing %.3f %.3f",
				unit->object.position.x, unit->object.position.y, unit->object.position.z,
				unit->object.forward.i, unit->object.forward.j);
		}
		slot = 0;
	}

	return;
}

/* ---------- public code */

boolean forge_layout_creating_object(
	void)
{
	return forge_layout_creating;
}

void forge_layout_note_spawned(
	long object_index)
{
	struct forge_object_properties properties;
	short read;
	short write = 0;

	if (object_index == NONE || forge_layout_map_object(object_index))
		return;

	/* the ones still there, packed, without this one (which keeps its
	properties, when it was there already) */
	csmemset(&properties, 0, sizeof(properties));
	for (read = 0; read < forge_layout_globals.spawned_count; read++)
	{
		long index = forge_layout_globals.spawned[read];

		if (index == object_index)
		{
			properties = forge_layout_globals.spawned_properties[read];
		}
		else if (object_try_and_get(index))
		{
			forge_layout_globals.spawned_properties[write] = forge_layout_globals.spawned_properties[read];
			forge_layout_globals.spawned[write++] = index;
		}
	}
	forge_layout_globals.spawned_count = write;
	if (!properties.id)
		forge_layout_default_properties(&properties, forge_layout_new_id());
	if (forge_layout_globals.spawned_count < FORGE_LAYOUT_MAXIMUM_SPAWNED)
	{
		forge_layout_globals.spawned_properties[forge_layout_globals.spawned_count] = properties;
		forge_layout_globals.spawned[forge_layout_globals.spawned_count++] = object_index;
	}
	else
	{
		terminal_printf(global_real_argb_orange,
			"forge: a layout keeps %d objects at most, this one is not saved with it", FORGE_LAYOUT_MAXIMUM_SPAWNED);
	}

	return;
}

void forge_layout_object_budget(
	short *used,
	short *maximum)
{
	short index;

	/* (those still there: the list is packed only when one is added) */
	*used = 0;
	for (index = 0; index < forge_layout_globals.spawned_count; index++)
	{
		if (object_try_and_get(forge_layout_globals.spawned[index]))
			(*used)++;
	}
	*maximum = FORGE_LAYOUT_MAXIMUM_SPAWNED;

	return;
}

void forge_layout_note_placed(
	long object_index)
{
	struct forge_layout_map_object *entry = forge_layout_map_object(object_index);

	if (entry)
		entry->placed = TRUE;
	else
		forge_layout_note_spawned(object_index);

	return;
}

void forge_layout_note_removed(
	long object_index)
{
	struct forge_layout_map_object *entry = forge_layout_map_object(object_index);

	if (entry)
		entry->removed = TRUE;

	return;
}

/* the properties of the layout's object: a spawned one's, or a map's own
one's */
static struct forge_object_properties *forge_layout_object_properties(
	long object_index)
{
	struct forge_layout_map_object *entry = forge_layout_map_object(object_index);

	return entry ? &entry->properties : forge_layout_spawned_properties(object_index);
}

int forge_layout_get_properties(
	long object_index,
	struct forge_object_properties *properties)
{
	struct forge_object_properties const *kept = object_index != NONE && object_try_and_get(object_index)
		? forge_layout_object_properties(object_index)
		: NULL;

	if (kept)
		*properties = *kept;
	else
		forge_layout_default_properties(properties, 0);

	return kept != NULL;
}

int forge_layout_set_properties(
	long object_index,
	struct forge_object_properties const *properties)
{
	struct forge_object_properties *kept;

	if (!forge_layout_authoritative() || object_index == NONE || !object_try_and_get(object_index) ||
		(kept = forge_layout_object_properties(object_index)) == NULL)
	{
		return FALSE;
	}
	/* (an object keeps its id; what cannot be is as a new object has it) */
	kept->team = properties->team >= 0 && properties->team < 16 ? properties->team : NONE;
	kept->spawn_flags = (short)(properties->spawn_flags & (FORGE_OBJECT_SPAWN_AT_START | FORGE_OBJECT_SPAWN_NEVER_RESPAWN));
	kept->respawn_seconds = (short)PIN(properties->respawn_seconds, 0, FORGE_OBJECT_MAXIMUM_RESPAWN_SECONDS);
	kept->game_types = (unsigned short)(properties->game_types &
		(FLAG(game_engine_ctf) | FLAG(game_engine_slayer) | FLAG(game_engine_oddball) | FLAG(game_engine_king) |
			FLAG(game_engine_race)));
	kept->group = (short)PIN(properties->group, 0, FORGE_OBJECT_MAXIMUM_GROUP);
	kept->locked = properties->locked != 0;
	forge_layout_clean_text(properties->comment, kept->comment, sizeof(kept->comment));

	return TRUE;
}

/* the text as the menus' strings are */
static void forge_layout_widen(
	char const *text,
	wchar_t *result,
	unsigned long count)
{
	unsigned long character;

	for (character = 0; text[character] && character < count - 1; character++)
		result[character] = (wchar_t)(unsigned char)text[character];
	result[character] = 0;

	return;
}

short forge_custom_maps_refresh(
	void)
{
	static struct forge_layout_variant variants[FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS * 4];
	static char base_names[FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS * 4][FORGE_LAYOUT_NAME_SIZE];
	short count;
	short base_index;

	/* port: with game.forge off, no Forge maps in the map lists, as the game
	was (and no layout played from one: forge_custom_map_selected) */
	forge_layout_globals.custom_map_count = 0;
	if (!config_boolean("game.forge"))
		return 0;
	count = forge_layout_list_variants(NULL, variants, base_names, NUMBEROF(variants));
	/* the listed variants of the game's multiplayer maps, map by map in the
	order of the map list, each map's by file name */
	for (base_index = 0; base_index < FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT; base_index++)
	{
		short first = forge_layout_globals.custom_map_count;
		short index;

		for (index = 0; index < count && forge_layout_globals.custom_map_count < FORGE_LAYOUT_MAXIMUM_CUSTOM_MAPS;
			index++)
		{
			struct forge_custom_map *map;
			short place;

			if (!variants[index].listed || _stricmp(base_names[index], forge_layout_multiplayer_maps[base_index]) != 0)
				continue;
			/* (in its place among this map's) */
			for (place = forge_layout_globals.custom_map_count;
				place > first && _stricmp(forge_layout_globals.custom_maps[place - 1].file, variants[index].file) > 0;
				place--)
			{
				forge_layout_globals.custom_maps[place] = forge_layout_globals.custom_maps[place - 1];
			}
			map = &forge_layout_globals.custom_maps[place];
			forge_layout_globals.custom_map_count++;
			map->base_index = base_index;
			csstrcpy(map->file, variants[index].file);
			forge_layout_widen(variants[index].name, map->title, NUMBEROF(map->title));
			forge_layout_widen(variants[index].description, map->description, NUMBEROF(map->description));
		}
	}

	return forge_layout_globals.custom_map_count;
}

static struct forge_custom_map const *forge_custom_map_get(
	short index)
{
	return index >= 0 && index < forge_layout_globals.custom_map_count
		? &forge_layout_globals.custom_maps[index]
		: NULL;
}

short forge_custom_map_base_index(
	short index)
{
	struct forge_custom_map const *map = forge_custom_map_get(index);

	return map ? map->base_index : 0;
}

char const *forge_custom_map_base_name(
	short index)
{
	return forge_layout_multiplayer_maps[forge_custom_map_base_index(index)];
}

wchar_t const *forge_custom_map_title(
	short index)
{
	struct forge_custom_map const *map = forge_custom_map_get(index);

	return map ? map->title : NULL;
}

wchar_t const *forge_custom_map_description(
	short index)
{
	struct forge_custom_map const *map = forge_custom_map_get(index);

	return map && map->description[0] ? map->description : NULL;
}

/* whether this machine hosts a network game on the map of this name (the
game record's scenario, by its name alone, as the listing has it) */
static boolean forge_layout_hosted_map_is(
	char const *base_name)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;

	return game && base_name && _stricmp(tag_name_strip_path(game->map.name), base_name) == 0;
}

void forge_custom_map_select(
	short index)
{
	struct forge_custom_map const *map = forge_custom_map_get(index);

	forge_layout_globals.selected_base_index = map ? map->base_index : -1;
	_snprintf(forge_layout_globals.selected_file, sizeof(forge_layout_globals.selected_file), "%s",
		map ? map->file : "");

	return;
}

/* port: whether the game hosted plays a Forge layout, which its clients are
sent (forge_layout_send_to_client): one chosen as a map in the map list, or
one loaded in the game (its slot); its advertisement is then marked,
whatever its game type (halo_port_limits.h's HALO_PORT_FORGE_VERSION_BIT).
Not the layout kept for clients (loaded_length): forge_layout_new_map keeps
one, of no Forge objects, on every map a host loads, campaign co-op's and
every multiplayer game's, which marked them all */
int forge_layout_hosting(
	void)
{
	short selected = forge_custom_map_selected();

	if (forge_layout_globals.current_slot != 0)
		return TRUE;
	/* (a map chosen in the list marks the game hosted on its base map, from
	the lobby on, and no other: the choice is kept between games, and marked
	every game made afterwards, a co-op level's and another map's, which
	builds without Forge then could not join) */
	return selected != NONE && forge_layout_hosted_map_is(forge_custom_map_base_name(selected));
}

short forge_custom_map_selected(
	void)
{
	short index;

	for (index = 0; forge_layout_globals.selected_file[0] && index < forge_layout_globals.custom_map_count; index++)
	{
		struct forge_custom_map const *map = &forge_layout_globals.custom_maps[index];

		if (map->base_index == forge_layout_globals.selected_base_index &&
			_stricmp(map->file, forge_layout_globals.selected_file) == 0)
		{
			return index;
		}
	}

	return NONE;
}

char const *forge_custom_map_base_title(
	short index)
{
	static char const *const titles[FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT] =
	{
		"Battle Creek",
		"Sidewinder",
		"Damnation",
		"Rat Race",
		"Prisoner",
		"Hang 'Em High",
		"Chill Out",
		"Derelict",
		"Boarding Action",
		"Blood Gulch",
		"Wizard",
		"Chiron TL-34",
		"Longest"
	};

	return titles[forge_custom_map_base_index(index)];
}

/* the slot of the variant chosen in the map list for this map, or 0 */
static short forge_layout_selected_slot(
	void)
{
	short base_index = forge_layout_globals.selected_base_index;

	return base_index >= 0 && base_index < FORGE_LAYOUT_MULTIPLAYER_MAP_COUNT &&
		_stricmp(forge_layout_multiplayer_maps[base_index], forge_layout_globals.map_name) == 0
		? forge_layout_slot_of_file(forge_layout_globals.selected_file)
		: 0;
}

/* ---------- system link: the host's layout to its clients */

/* a part of the layout as a variant (_distributed_message_forge_layout) */
enum
{
	FORGE_LAYOUT_MESSAGE_DATA_SIZE = 3072
};

struct forge_layout_message
{
	struct distributed_message_header header;
	unsigned long total_length;
	unsigned long offset;
	word length;
	unsigned char data[FORGE_LAYOUT_MESSAGE_DATA_SIZE];
};

/* a client's own tools ask the host for a change
(_distributed_message_forge_edit) */
struct forge_layout_edit_message
{
	struct distributed_message_header header;
	long kind;
	/* the host's object (a unit or an item, which a client has at the
	host's index), or NONE: then the one of the definition at original */
	long object_index;
	long definition_index;
	real_point3d original;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
};

int forge_layout_loading_for_client(
	void)
{
	return forge_layout_globals.loading_for_client;
}

int forge_layout_saving_for_client(
	void)
{
	return forge_layout_globals.saving_for_client;
}

/* the host: the layout as its clients get it, kept for those that join, and,
when it has changed, sent to every client (send) */
static void forge_layout_host_sync(
	boolean send)
{
	unsigned long length;
	unsigned long checksum;

	if (game_connection() != _game_connection_network_server || forge_layout_globals.map_name[0] == 0)
		return;
	forge_layout_globals.sync_time = game_time_get();
	if ((length = forge_layout_write_variant(forge_layout_globals.current_slot, TRUE)) == 0)
		return;
	checksum = forge_layout_checksum(forge_layout_globals.variant, length);
	if (forge_layout_globals.loaded_length == 0 || checksum != forge_layout_globals.synced_checksum)
	{
		forge_layout_globals.synced_checksum = checksum;
		csmemcpy(forge_layout_globals.loaded_variant, forge_layout_globals.variant, length);
		forge_layout_globals.loaded_length = length;
		if (send)
		{
			long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
			short count = distributed_client_machines(machine_indices, NUMBEROF(machine_indices));
			short index;

			for (index = 0; index < count; index++)
				forge_layout_send_to_client(machine_indices[index]);
			if (count > 0)
				platform_log("forge layout: a variant of %lu bytes to %d client(s)", length, (int)count);
		}
	}

	return;
}

int forge_layout_client_edit(
	int kind,
	long object_index,
	long definition_index,
	float const original[3],
	float const position[3],
	float const forward[3],
	float const up[3])
{
	struct forge_layout_edit_message message;

	if (game_connection() != _game_connection_network_client)
		return FALSE;
	csmemset(&message, 0, sizeof(message));
	message.kind = kind;
	/* only the host's own objects have its index here */
	message.object_index = network_objects_client_has(object_index) ? object_index : NONE;
	message.definition_index = definition_index;
	set_real_point3d(&message.original, original[0], original[1], original[2]);
	set_real_point3d(&message.position, position[0], position[1], position[2]);
	set_real_vector3d(&message.forward, forward[0], forward[1], forward[2]);
	set_real_vector3d(&message.up, up[0], up[1], up[2]);
	distributed_send(&message, _distributed_message_forge_edit, 0, (word)sizeof(message), _distributed_to_host_reliably);

	return TRUE;
}

/* the host's object a client's message names, or NONE: never a player's
unit, nor what something carries */
static long forge_layout_edit_object(
	struct forge_layout_edit_message const *message)
{
	long object_index = NONE;

	if (message->object_index != NONE)
	{
		if (distributed_object_index_valid(message->object_index) && object_try_and_get(message->object_index))
			object_index = message->object_index;
	}
	else
	{
		struct object_iterator iterator;
		struct object_datum *object;
		real nearest_distance = FORGE_LAYOUT_EDIT_DISTANCE;

		object_iterator_new(&iterator, FORGE_LAYOUT_OBJECT_MASK, 0);
		while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real distance;

			if (object->definition_index != message->definition_index ||
				(distance = distance3d(&object->object.position, &message->original)) >= nearest_distance)
			{
				continue;
			}
			nearest_distance = distance;
			object_index = iterator.index;
		}
	}
	if (object_index != NONE)
	{
		struct object_datum *object = object_get(object_index);

		if (object->definition_index != message->definition_index ||
			!TEST_FLAG(FORGE_LAYOUT_OBJECT_MASK, object->object.type) ||
			object->object.parent_object_index != NONE ||
			(TEST_FLAG(_object_mask_unit, object->object.type) && unit_get(object_index)->unit.player_index != NONE))
		{
			object_index = NONE;
		}
	}

	return object_index;
}

void forge_layout_handle_edit(
	long machine_index,
	void const *payload,
	unsigned long size)
{
	struct forge_layout_edit_message message;
	unsigned long header_size = offsetof(struct forge_layout_edit_message, kind);
	long object_index;

	/* only in a game whose players may build */
	if (game_connection() != _game_connection_network_server || size < sizeof(message) - header_size ||
		!forge_mode_on())
	{
		return;
	}
	csmemcpy((byte *)&message + header_size, payload, sizeof(message) - header_size);
	/* the client's player becomes the monitor or gets its body back
	(forge_monitor.c): the first of that machine's players, whom its tools
	act on */
	if (message.kind == _forge_edit_monitor_enter || message.kind == _forge_edit_monitor_leave)
	{
		struct data_iterator iterator;

		data_iterator_new(&iterator, player_data);
		while (data_iterator_next(&iterator))
		{
			if (distributed_machine_has_player(machine_index, (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index)))
			{
				forge_monitor_set(iterator.datum_index, message.kind == _forge_edit_monitor_enter);
				break;
			}
		}
		return;
	}
	/* (what cannot be, from a message: not taken) */
	if (!tag_index_is_group(message.definition_index, OBJECT_DEFINITION_TAG) ||
		!TEST_FLAG(FORGE_LAYOUT_OBJECT_MASK, object_definition_get(message.definition_index)->object.type) ||
		!distributed_point_valid(&message.original, FORGE_LAYOUT_WORLD_BOUND) ||
		!distributed_point_valid(&message.position, FORGE_LAYOUT_WORLD_BOUND))
	{
		return;
	}

	platform_log("forge layout: a client asks to %s %s",
		message.kind == _forge_edit_spawn ? "spawn" : message.kind == _forge_edit_move ? "move" : "remove",
		tag_get_name(message.definition_index));
	switch (message.kind)
	{
	case _forge_edit_spawn:
		if (distributed_axes_make_valid(&message.forward, &message.up) &&
			forge_layout_globals.spawned_count < FORGE_LAYOUT_MAXIMUM_SPAWNED)
		{
			object_index = forge_layout_new_object(message.definition_index, &message.position, &message.forward,
				&message.up, 0.f);
			if (object_index != NONE)
				forge_layout_note_spawned(object_index);
		}
		break;
	case _forge_edit_move:
		if (distributed_axes_make_valid(&message.forward, &message.up) &&
			(object_index = forge_layout_edit_object(&message)) != NONE)
		{
			forge_layout_place(object_index, &message.position, &message.forward, &message.up);
			forge_layout_note_placed(object_index);
			/* (not at rest: where it is goes to every client with the next
			tick, network_objects.c) */
			SET_FLAG(object_get(object_index)->object.flags, _object_at_rest_bit, FALSE);
		}
		break;
	case _forge_edit_remove:
		if ((object_index = forge_layout_edit_object(&message)) != NONE && !forge_object_holds_flag(object_index))
		{
			forge_layout_note_removed(object_index);
			object_delete(object_index);
		}
		break;
	}

	return;
}

void forge_layout_send_to_client(
	long machine_index)
{
	struct forge_layout_message message;
	unsigned long offset = 0;

	if (game_connection() != _game_connection_network_server || forge_layout_globals.loaded_length == 0)
		return;
	/* port: a Forge game's, or a Forge layout's of another game type, alone:
	other games' clients may be builds without Forge, which do not know the
	message */
	if (!game_variant_set_is_forge() && !forge_layout_hosting())
		return;
	while (offset < forge_layout_globals.loaded_length)
	{
		unsigned long length = MIN(forge_layout_globals.loaded_length - offset, FORGE_LAYOUT_MESSAGE_DATA_SIZE);

		message.total_length = forge_layout_globals.loaded_length;
		message.offset = offset;
		message.length = (word)length;
		csmemcpy(message.data, forge_layout_globals.loaded_variant + offset, length);
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_forge_layout, 0,
			(word)(offsetof(struct forge_layout_message, data) + length));
		offset += length;
	}

	return;
}

void forge_layout_handle_message(
	void const *payload,
	unsigned long size)
{
	struct forge_layout_message message;
	unsigned long header_size = offsetof(struct forge_layout_message, total_length);
	unsigned long fixed_size = offsetof(struct forge_layout_message, data) - header_size;

	if (game_connection() != _game_connection_network_client || size < fixed_size)
		return;
	csmemcpy((byte *)&message + header_size, payload, MIN(size, sizeof(message) - header_size));
	if (message.length > size - fixed_size || message.length > FORGE_LAYOUT_MESSAGE_DATA_SIZE ||
		message.total_length > FORGE_MVAR_MAXIMUM_SIZE)
	{
		return;
	}
	/* the parts come in order (reliably); a first part starts over */
	if (message.offset == 0)
		forge_layout_globals.received_length = 0;
	if (message.offset != forge_layout_globals.received_length ||
		message.offset + message.length > message.total_length)
	{
		return;
	}
	csmemcpy(forge_layout_globals.received_variant + message.offset, message.data, message.length);
	forge_layout_globals.received_length += message.length;

	if (forge_layout_globals.received_length == message.total_length)
	{
		unsigned long checksum;

		checksum = forge_layout_checksum(forge_layout_globals.received_variant, message.total_length);
		/* (played, never saved: the variant's file is the host's) */
		if (checksum != forge_layout_globals.applied_checksum)
		{
			forge_layout_globals.applied_checksum = checksum;
			if (forge_layout_apply_variant(forge_layout_globals.received_variant, message.total_length, TRUE))
			{
				platform_log("forge layout: a variant of %lu bytes from the host, %d object(s) of it made here",
					message.total_length, (int)forge_layout_globals.spawned_count);
			}
			else
			{
				platform_log("forge layout: the host's variant (%lu bytes) is damaged, not applied", message.total_length);
			}
		}
		forge_layout_globals.received_length = 0;
	}

	return;
}

void forge_map_folder_name(
	char const *scenario_name,
	char *name,
	unsigned long size)
{
	char const *separator = strrchr(scenario_name, '\\');
	unsigned long index;

	_snprintf(name, size, "%s", separator ? separator + 1 : scenario_name);
	name[size - 1] = 0;
	/* (a map's file names its scenario as it likes: nothing that would
	make the folder another one) */
	for (index = 0; name[index]; index++)
	{
		if ((unsigned char)name[index] < ' ' || strchr("/\\:", name[index]) || (index == 0 && name[index] == '.'))
			name[index] = '_';
	}

	return;
}

void forge_layout_new_map(
	void)
{
	char const *scenario_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;

	forge_layout_globals.spawned_count = 0;
	forge_layout_globals.dormant_count = 0;
	forge_layout_globals.next_object_id = 1;
	forge_layout_globals.current_slot = 0;
	forge_layout_globals.chosen_slot = 0;
	forge_layout_globals.map_name[0] = 0;
	forge_layout_globals.loaded_length = 0;
	forge_layout_globals.received_length = 0;
	forge_layout_globals.applied_checksum = 0;
	forge_layout_globals.synced_checksum = 0;
	forge_layout_globals.sync_time = 0;
	forge_layout_globals.line_tag_name[0] = 0;
	forge_layout_globals.variants[0].name[0] = 0;
	forge_layout_globals.variants[0].description[0] = 0;
	if (scenario_name)
		forge_map_folder_name(scenario_name, forge_layout_globals.map_name, sizeof(forge_layout_globals.map_name));
	forge_layout_note_map_objects();

	/* a system link client gets the host's layout when it has loaded
	(forge_layout_handle_message) */
	if (forge_layout_authoritative())
	{
		/* the layout chosen as a map in the map list, if one was: the map
		chosen by its own name is the game's, whatever was saved on it */
		short slot;

		forge_layout_read_slots();
		slot = forge_layout_selected_slot();
		/* port: (debug.forge_layout_load: the map's variant of that slot,
		for tests that start a game without the menus) */
		if (!slot && config_integer("debug.forge_layout_load") > 0 &&
			config_integer("debug.forge_layout_load") <= forge_layout_globals.variant_count)
		{
			slot = (short)config_integer("debug.forge_layout_load");
		}
		if (slot)
		{
			forge_layout_load(slot);
			platform_log("forge layout: loaded slot %d (%s), %d objects placed", (int)slot,
				forge_layout_globals.variants[slot].file, (int)forge_layout_globals.spawned_count);
		}
		/* a system link host: the layout as the clients that join get it */
		forge_layout_host_sync(FALSE);
	}

	return;
}
