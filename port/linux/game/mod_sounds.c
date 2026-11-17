/*
MOD_SOUNDS.C

The sounds of mods (mods.c): each of a map's sound tags that a mod has a
tag file of (tags\sound\...\name.sound) is played from that file instead of
the map, and mods_reload, typed at the console, reads them again: a changed
file is taken, a removed one gives the map's sound back.

The existing audio.loose_sounds setting and console commands also read
the data root's tags folder, with priority over mod packs when enabled.
The same decoder, cache and override table serve both sources.

A tag file is Halo PC's (big-endian, version 4): the sound, the path of its
promotion sound, its pitch ranges, then each range's permutations, each
followed by its samples, mouth data and subtitle data. Every count, size and
offset is checked against the file, which must end where the last of them
does, and every value the game divides by or steps through against what the
game takes; a file that is not is said and left out. Its samples (16-bit
PCM, Xbox ADPCM or Ogg Vorbis, mono or stereo at 22 or 44 kHz) are kept as
a Custom Edition map's are (custom_edition_sounds_encode: 16-bit PCM at
their own rate, or past the memory the mods may keep, Xbox ADPCM, mono at
22 kHz and stereo at its own rate, said once; Xbox ADPCM in that format is
kept as it is), so they play through the map's channels as its sounds do.
A permutation of PCM longer than the sound cache takes one in is split
into a chain of linked pieces (range_split, OpenH1 issue 1167), which plays
as one. The values a map's
build (tool.exe, Invader) fills in are filled in as it does: the defaults
of the sound's class, the pitch ranges' playback rates, the per-tick bend.
The longest permutation length is Tool's measure of a permutation, a split
(linked) sound's chain of them summed: the whole chain plays, and the
scripts and AI speech that wait for a sound wait for it (on the host, the
only machine of a network game that runs them).

The map's definition takes the file's values in place, so everything that
holds the sound keeps holding it; only what the game counts as it plays
(promotion counter and time, scripting time and sound) stays the map's
definition's own, kept across a reload or switch. The map's own values come
back when the file goes. The samples are read into the sound cache from
memory (mod_sounds_read, xbox_sound_cache.c).

A sound playing holds its permutation by index and the sound cache by
address, so nothing is changed under one: when a reload changes anything,
every sound stops and the sound cache lets go what is not playing first,
then the definitions change, and the permutations that went are freed once
no cache block holds them (at the latest when the map goes).
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "cache/cache_files.h"
#include "cache/sound_cache.h"
#include "main/console.h"
#include "sound/sound_definitions.h"
#include "sound/sound_manager.h"
#include "tag_files/tag_groups.h"
#include "custom_edition_cache.h"
#include "mods.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* the game's (port) */
char *tag_get_name(long tag_index);

/* ---------- constants */

/* a sound tag's file: tags\<its name>.sound */
#define TAG_FILE_FOLDER "d:\\tags\\"
#define TAG_FILE_EXTENSION ".sound"

/* the sizes of a tag file's sound, pitch range and permutation
(sound_definitions.h's structures, as the file has them) */
#define SOUND_SIZE 0xA4
#define PITCH_RANGE_SIZE 0x48
#define PERMUTATION_SIZE 0x7C

#define SOUND_COMPRESSION_NONE 0
#define SOUND_COMPRESSION_XBOX_ADPCM 1
#define SOUND_COMPRESSION_OGG_VORBIS 3
#define ADPCM_BLOCK_BYTES 36

/* (sound_classes.c) */
#define NUMBER_OF_SOUND_CLASSES 51

enum
{
	_class_projectile_impact = 0,
	_class_projectile_detonation = 1,
	_class_weapon_fire = 4,
	_class_weapon_ready = 5,
	_class_weapon_reload = 6,
	_class_weapon_empty = 7,
	_class_weapon_charge = 8,
	_class_weapon_overheat = 9,
	_class_weapon_idle = 10,
	_class_object_impacts = 13,
	_class_particle_impacts = 14,
	_class_slow_particle_impacts = 15,
	_class_unit_footsteps = 18,
	_class_unit_dialog = 19,
	_class_vehicle_collision = 22,
	_class_vehicle_engine = 23,
	_class_device_door = 26,
	_class_device_force_field = 27,
	_class_device_machinery = 28,
	_class_device_nature = 29,
	_class_device_computers = 30,
	_class_music = 32,
	_class_ambient_nature = 33,
	_class_ambient_machinery = 34,
	_class_ambient_computers = 35,
	_class_first_person_damage = 39,
	_class_scripted_dialog_player = 44,
	_class_scripted_effect = 45,
	_class_scripted_dialog_other = 46,
	_class_scripted_dialog_force_unspatialized = 47,
	_class_game_event = 50,
};

/* ---------- structures */

/* a sound tag file read, and what it makes */
struct mod_sound
{
	struct mod_sound *next;
	long tag_index;
	/* the file, kept to tell whether it changed (its mouth and subtitle
	data are read from it) */
	byte *file;
	unsigned long file_size;
	/* the definition it makes (its counters unused), its pitch ranges and
	their permutations, and each permutation's samples */
	struct sound_definition definition;
	struct sound_pitch_range *ranges;
	struct sound_permutation *permutations;
	long permutation_count;
	unsigned long kept_bytes;
	/* its permutations kept as Xbox ADPCM, not 16-bit PCM at their own
	rates: past the memory the mods may keep (said once, mod_sounds_update) */
	long adpcm_count;
};

/* a sound of the map played from a file: the map's own values, given back
when it goes */
struct mod_sound_override
{
	struct mod_sound *sound;
	struct sound_definition map_definition;
};

static struct
{
	/* by the map's sound tag, its override (none: the map's own) */
	struct mod_sound_override *overrides;
	long override_count;
	/* the sounds made and not yet let go: those of the overrides, and those
	gone but still in the sound cache */
	struct mod_sound *sounds;
} mod_sounds_globals;

/* ---------- private code */

/* the tag file of the sound tag `name`, in the data root's tags folder (an
Xbox path, which the game's fopen finds there: port/linux/src/xbox_files.c);
FALSE when the name could reach outside it */
static boolean tag_file_path(
	char const *name,
	char *path,
	size_t path_size)
{
	char const *character;

	if (!name || !*name || strlen(name) + sizeof(TAG_FILE_FOLDER TAG_FILE_EXTENSION) > path_size)
	{
		return FALSE;
	}
	/* (a name is relative and of the folder: no drive, no parent, no
	absolute or empty part) */
	for (character = name; *character; character++)
	{
		boolean separator = *character == '\\' || *character == '/';

		if (*character == ':' ||
			(separator && (character == name || !character[1] || character[1] == '\\' || character[1] == '/')) ||
			(*character == '.' && character[1] == '.' &&
				(character == name || character[-1] == '\\' || character[-1] == '/')))
		{
			return FALSE;
		}
	}
	sprintf(path, "%s%s%s", TAG_FILE_FOLDER, name, TAG_FILE_EXTENSION);

	return TRUE;
}

/* the file at `path` in a buffer of the game's allocator, or NULL (no file:
*missing) */
static byte *file_read(
	char const *path,
	unsigned long *size,
	boolean *missing)
{
	FILE *file = fopen(path, "rb");
	byte *data = NULL;
	long length;

	*missing = file == NULL;
	if (!file)
	{
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) == 0 &&
		(length = ftell(file)) >= MOD_TAG_FILE_HEADER_SIZE + SOUND_SIZE &&
		(unsigned long)length <= MAXIMUM_MOD_TAG_FILE_SIZE &&
		fseek(file, 0, SEEK_SET) == 0 &&
		(data = malloc((size_t)length)) != NULL)
	{
		if (fread(data, 1, (size_t)length, file) == (size_t)length)
		{
			*size = (unsigned long)length;
		}
		else
		{
			free(data);
			data = NULL;
		}
	}
	fclose(file);

	return data;
}

static void mod_sound_free(
	struct mod_sound *sound)
{
	long permutation_index;

	for (permutation_index = 0; permutation_index < sound->permutation_count; permutation_index++)
	{
		if (sound->permutations[permutation_index].samples.address)
			free(sound->permutations[permutation_index].samples.address);
	}
	/* (the game's free, debug_free, does not take NULL) */
	if (sound->permutations)
		free(sound->permutations);
	if (sound->ranges)
		free(sound->ranges);
	if (sound->file)
		free(sound->file);
	free(sound);

	return;
}

/* the defaults of a class for a sound's distances, which a map's build
fills in for none (Invader's, as tool.exe's) */
static void class_distances(
	short sound_class,
	real *minimum,
	real *maximum)
{
	switch (sound_class)
	{
	case _class_device_machinery: case _class_device_force_field: case _class_ambient_machinery:
	case _class_ambient_nature: case _class_device_door: case _class_music: case _class_device_nature:
		*minimum = 0.9f; *maximum = 5.f;
		break;
	case _class_weapon_empty: case _class_weapon_idle: case _class_weapon_ready:
	case _class_weapon_reload: case _class_weapon_charge: case _class_weapon_overheat:
		*minimum = 1.f; *maximum = 9.f;
		break;
	case _class_scripted_dialog_other: case _class_scripted_dialog_player: case _class_game_event:
	case _class_unit_dialog: case _class_scripted_dialog_force_unspatialized:
		*minimum = 3.f; *maximum = 20.f;
		break;
	case _class_first_person_damage: case _class_object_impacts: case _class_ambient_computers:
	case _class_particle_impacts: case _class_device_computers: case _class_slow_particle_impacts:
		*minimum = 0.5f; *maximum = 3.f;
		break;
	case _class_vehicle_engine: case _class_projectile_impact: case _class_vehicle_collision:
		*minimum = 1.4f; *maximum = 8.f;
		break;
	case _class_weapon_fire:
		*minimum = 4.f; *maximum = 70.f;
		break;
	case _class_scripted_effect:
		*minimum = 2.f; *maximum = 5.f;
		break;
	case _class_projectile_detonation:
		*minimum = 8.f; *maximum = 120.f;
		break;
	case _class_unit_footsteps:
		*minimum = 0.9f; *maximum = 10.f;
		break;
	default:
		*minimum = 0.f; *maximum = 0.f;
		break;
	}

	return;
}

/* the defaults a map's build fills in for none (Invader's, as tool.exe's) */
static void definition_defaults(
	struct sound_definition *definition)
{
	real minimum_distance;
	real maximum_distance;

	if (definition->zero_skip_fraction_modifier == 0.f && definition->one_skip_fraction_modifier == 0.f)
	{
		definition->zero_skip_fraction_modifier = 1.f;
		definition->one_skip_fraction_modifier = 1.f;
	}
	if (definition->random_pitch_bounds.lower == 0.f)
		definition->random_pitch_bounds.lower = 1.f;
	if (definition->random_pitch_bounds.upper == 0.f)
		definition->random_pitch_bounds.upper = 1.f;
	if (definition->zero_gain_modifier == 0.f && definition->one_gain_modifier == 0.f)
	{
		definition->one_gain_modifier = 1.f;
		switch (definition->sound_class)
		{
		case _class_object_impacts: case _class_particle_impacts: case _class_slow_particle_impacts:
		case _class_unit_dialog: case _class_music: case _class_ambient_nature: case _class_ambient_machinery:
		case _class_ambient_computers: case _class_scripted_dialog_player: case _class_scripted_dialog_other:
		case _class_scripted_dialog_force_unspatialized: case _class_scripted_effect:
			definition->zero_gain_modifier = 0.f;
			break;
		default:
			definition->zero_gain_modifier = 1.f;
			break;
		}
	}
	if (definition->zero_pitch_modifier == 0.f && definition->one_pitch_modifier == 0.f)
	{
		definition->zero_pitch_modifier = 1.f;
		definition->one_pitch_modifier = 1.f;
	}
	class_distances(definition->sound_class, &minimum_distance, &maximum_distance);
	if (definition->minimum_distance == 0.f)
		definition->minimum_distance = minimum_distance;
	if (definition->maximum_distance == 0.f)
		definition->maximum_distance = maximum_distance;
	/* (a map holds the bend a tick, the file a second) */
	definition->maximum_bend_per_second = definition->maximum_bend_per_second > 0.f ?
		(real)pow(definition->maximum_bend_per_second, 1.0 / 30.0) : 0.f;

	return;
}

/* The bytes of 16-bit PCM a permutation of `sound` may be kept in: what
the mods may still keep, less what the sound already keeps and its file.
Past it, a permutation is Xbox ADPCM (custom_edition_sounds_encode). */
static unsigned long native_budget(
	struct mod_sound const *sound,
	unsigned long file_size)
{
	unsigned long left = mods_memory_left();
	unsigned long held = sound->kept_bytes + file_size;

	return left > held ? left - held : 0;
}

/* the permutations `bytes` of 16-bit PCM are kept in: one, or where that
is longer than one permutation is kept in (twice a piece), its pieces */
static long piece_count(
	unsigned long bytes)
{
	unsigned long piece_bytes = custom_edition_sounds_piece_bytes();

	return bytes <= 2 * piece_bytes ? 1 : (long)((bytes + piece_bytes - 1) / piece_bytes);
}

/* Splits each permutation of the pitch range `range` (the file's `count`
of them, from sound->permutations[first], the last made) kept as 16-bit PCM
longer than one permutation is kept in (piece_count) into a chain of linked
pieces (OpenH1 issue 1167), so that it stays PCM at its own rate: the
permutation keeps the first piece and its mouth and subtitle data, and the
others are new permutations after the range's, each followed by the next
and the last by what followed the permutation. The pieces are its frames
in order, none left out or repeated, at its rate, so the chain plays them
as one: sound_dsound_xbox.c queues a piece's packets after the last of the
one before, of the same format (no switch of the channel's stream between
them). The range's count takes them; FALSE when out of memory. */
static boolean range_split(
	struct mod_sound *sound,
	struct sound_pitch_range *range,
	long first,
	unsigned long count,
	long *capacity)
{
	unsigned long piece_bytes = custom_edition_sounds_piece_bytes();
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		struct sound_permutation *permutation = &sound->permutations[first + index];
		unsigned long bytes = (unsigned long)permutation->samples.size;
		long more = piece_count(bytes) - 1;
		short previous = (short)index;
		short next = permutation->next_permutation_index;
		unsigned long offset;
		byte *samples;
		byte *shrunk;

		if (!sound_permutation_pcm_rate(permutation) || more <= 0)
		{
			continue;
		}
		if (sound->permutation_count + more > *capacity)
		{
			struct sound_permutation *grown = realloc(sound->permutations,
				(size_t)(*capacity + more) * sizeof(*sound->permutations));

			if (!grown)
			{
				return FALSE;
			}
			memset(grown + *capacity, 0, (size_t)more * sizeof(*grown));
			sound->permutations = grown;
			*capacity += more;
			permutation = &sound->permutations[first + index];
		}
		samples = permutation->samples.address;
		for (offset = piece_bytes; offset < bytes; offset += piece_bytes)
		{
			struct sound_permutation *piece = &sound->permutations[sound->permutation_count];
			unsigned long size = MIN(piece_bytes, bytes - offset);
			short piece_index = (short)(sound->permutation_count - first);

			/* (its name, gain, format and rate, and the sound cache's values) */
			*piece = *permutation;
			piece->skip_fraction = 0.f;
			piece->mouth_data.size = 0;
			piece->mouth_data.address = NULL;
			piece->subtitle_data.size = 0;
			piece->subtitle_data.address = NULL;
			piece->samples.size = (long)size;
			piece->samples.address = malloc(size);
			if (!piece->samples.address)
			{
				return FALSE;
			}
			memcpy(piece->samples.address, samples + offset, size);
			piece->next_permutation_index = next;
			sound->permutations[first + previous].next_permutation_index = piece_index;
			previous = piece_index;
			sound->permutation_count++;
		}
		permutation->samples.size = (long)piece_bytes;
		shrunk = realloc(samples, piece_bytes);
		if (shrunk)
		{
			permutation->samples.address = shrunk;
		}
	}
	range->permutations.count = sound->permutation_count - first;

	return TRUE;
}

/* Tool's measure of a permutation of `size` bytes in `compression` (and,
for Ogg Vorbis, of `buffer_size` decoded), in milliseconds (Invader's
SoundPermutation length, as tool.exe's) */
static real permutation_length(
	struct sound_definition const *definition,
	struct sound_pitch_range const *range,
	short compression,
	unsigned long size,
	unsigned long buffer_size,
	long rate)
{
	real channel_modifier = definition->encoding == 0 ? 1.f : 0.5f;
	real samples;
	real format_modifier;
	real slowest = MIN(definition->random_pitch_bounds.lower, definition->random_pitch_bounds.upper) *
		MIN(definition->zero_pitch_modifier, definition->one_pitch_modifier);

	switch (compression)
	{
	case SOUND_COMPRESSION_NONE:
		samples = (real)size;
		format_modifier = 0.5f;
		break;
	case SOUND_COMPRESSION_OGG_VORBIS:
		samples = (real)buffer_size / 4.f;
		format_modifier = 2.2f;
		break;
	default:
		samples = (real)size;
		format_modifier = 2.f;
		break;
	}

	return samples * 1000.f * channel_modifier * format_modifier * range->natural_pitch / ((real)rate * slowest);
}

/* The sound the tag file `file` makes for the map's sound tag
`tag_index`, or NULL with *reason saying why not. */
static struct mod_sound *mod_sound_new(
	long tag_index,
	byte *file,
	unsigned long file_size,
	char const **reason)
{
	struct mod_sound *sound;
	struct sound_definition *definition;
	byte const *root = file + MOD_TAG_FILE_HEADER_SIZE;
	unsigned long cursor = MOD_TAG_FILE_HEADER_SIZE + SOUND_SIZE;
	unsigned long range_count;
	unsigned long promotion_length;
	unsigned long range_index;
	long permutation_total = 0;
	long permutation_capacity;
	long range_first[MAXIMUM_PITCH_RANGES_PER_SOUND];
	boolean split = FALSE;
	boolean linked;
	long rate;
	long encoded_rate;
	long channels;
	real longest = 0.f;

	*reason = "not a sound tag file of this version";
	if (file_size < MOD_TAG_FILE_HEADER_SIZE + SOUND_SIZE ||
		!mods_tag_file_header_valid(file, SOUND_DEFINITION_TAG, SOUND_DEFINITION_VERSION))
	{
		return NULL;
	}
	sound = malloc(sizeof(*sound));
	if (!sound)
	{
		*reason = "out of memory";
		return NULL;
	}
	memset(sound, 0, sizeof(*sound));
	sound->tag_index = tag_index;
	definition = &sound->definition;

	/* the sound's values */
#define SOUND_SHORT(field) definition->field = mods_read_short(root + offsetof(struct sound_definition, field))
#define SOUND_REAL(field) definition->field = mods_read_real(root + offsetof(struct sound_definition, field))
	definition->flags = mods_read_be32(root + offsetof(struct sound_definition, flags));
	SOUND_SHORT(sound_class);
	SOUND_SHORT(sample_rate);
	SOUND_SHORT(encoding);
	SOUND_SHORT(compression);
	SOUND_SHORT(promotion_count);
	SOUND_REAL(minimum_distance);
	SOUND_REAL(maximum_distance);
	SOUND_REAL(skip_fraction);
	SOUND_REAL(random_pitch_bounds.lower);
	SOUND_REAL(random_pitch_bounds.upper);
	SOUND_REAL(inner_cone_angle);
	SOUND_REAL(outer_cone_angle);
	SOUND_REAL(outer_cone_gain);
	SOUND_REAL(gain_modifier);
	SOUND_REAL(maximum_bend_per_second);
	SOUND_REAL(zero_skip_fraction_modifier);
	SOUND_REAL(zero_gain_modifier);
	SOUND_REAL(zero_pitch_modifier);
	SOUND_REAL(one_skip_fraction_modifier);
	SOUND_REAL(one_gain_modifier);
	SOUND_REAL(one_pitch_modifier);
#undef SOUND_SHORT
#undef SOUND_REAL
	*reason = "a value of the sound is out of range";
	if (definition->sound_class < 0 || definition->sound_class >= NUMBER_OF_SOUND_CLASSES ||
		(definition->sample_rate != 0 && definition->sample_rate != 1) ||
		(definition->encoding != 0 && definition->encoding != 1) ||
		definition->promotion_count < 0 ||
		!mods_finite_real(definition->minimum_distance) || !mods_finite_real(definition->maximum_distance) ||
		definition->minimum_distance < 0.f || definition->maximum_distance < 0.f ||
		!mods_finite_real(definition->skip_fraction) ||
		!mods_finite_real(definition->random_pitch_bounds.lower) || !mods_finite_real(definition->random_pitch_bounds.upper) ||
		definition->random_pitch_bounds.lower < 0.f || definition->random_pitch_bounds.upper < 0.f ||
		!mods_finite_real(definition->inner_cone_angle) || !mods_finite_real(definition->outer_cone_angle) ||
		!mods_finite_real(definition->outer_cone_gain) || !mods_finite_real(definition->gain_modifier) ||
		!mods_finite_real(definition->maximum_bend_per_second) || definition->maximum_bend_per_second < 0.f ||
		!mods_finite_real(definition->zero_skip_fraction_modifier) || !mods_finite_real(definition->one_skip_fraction_modifier) ||
		!mods_finite_real(definition->zero_gain_modifier) || !mods_finite_real(definition->one_gain_modifier) ||
		!mods_finite_real(definition->zero_pitch_modifier) || !mods_finite_real(definition->one_pitch_modifier) ||
		definition->zero_pitch_modifier < 0.f || definition->one_pitch_modifier < 0.f)
	{
		goto refused;
	}
	*reason = "its samples are IMA ADPCM, which this build does not decode";
	if (definition->compression != SOUND_COMPRESSION_NONE &&
		definition->compression != SOUND_COMPRESSION_XBOX_ADPCM &&
		definition->compression != SOUND_COMPRESSION_OGG_VORBIS)
	{
		goto refused;
	}
	definition_defaults(definition);
	*reason = "a value of the sound is out of range";
	if (!mods_finite_real(definition->maximum_bend_per_second) ||
		MIN(definition->random_pitch_bounds.lower, definition->random_pitch_bounds.upper) *
			MIN(definition->zero_pitch_modifier, definition->one_pitch_modifier) <= 0.f)
	{
		goto refused;
	}
	linked = TEST_FLAG(definition->flags, _sound_definition_linked_permutations_bit);
	channels = definition->encoding == 0 ? 1 : 2;
	rate = definition->sample_rate == 0 ? 22050 : 44100;
	/* (what this build plays: mono at 22 kHz, stereo at either) */
	encoded_rate = channels == 1 ? 22050 : rate;

	/* the promotion sound: one of the map's, or none */
	promotion_length = mods_read_be32(root + offsetof(struct sound_definition, promotion_sound.name_length));
	definition->promotion_sound.group_tag = SOUND_DEFINITION_TAG;
	definition->promotion_sound.index = NONE;
	definition->promotion_sound.name = NULL;
	definition->promotion_sound.name_length = 0;
	if (promotion_length)
	{
		char promotion[256];
		long promotion_index;

		*reason = "the promotion sound's name is not in the file";
		if (promotion_length >= sizeof(promotion) || !mods_in_file(cursor, promotion_length + 1, file_size) ||
			file[cursor + promotion_length] != 0 || memchr(file + cursor, 0, promotion_length))
		{
			goto refused;
		}
		memcpy(promotion, file + cursor, promotion_length + 1);
		cursor += promotion_length + 1;
		promotion_index = tag_loaded(SOUND_DEFINITION_TAG, promotion);
		if (promotion_index != NONE)
		{
			definition->promotion_sound.index = promotion_index;
			definition->promotion_sound.name = tag_get_name(promotion_index);
			definition->promotion_sound.name_length = (long)strlen(definition->promotion_sound.name);
		}
	}

	/* the pitch ranges, then each one's permutations and their data */
	range_count = mods_read_be32(root + offsetof(struct sound_definition, pitch_ranges.count));
	*reason = "its pitch ranges are not in the file";
	if (range_count < 1 || range_count > MAXIMUM_PITCH_RANGES_PER_SOUND ||
		!mods_in_file(cursor, range_count * PITCH_RANGE_SIZE, file_size))
	{
		goto refused;
	}
	for (range_index = 0; range_index < range_count; range_index++)
	{
		/* (no more than a range takes: a larger count is refused below) */
		permutation_total += (long)MIN(mods_read_be32(file + cursor + range_index * PITCH_RANGE_SIZE +
			offsetof(struct sound_pitch_range, permutations.count)), MAXIMUM_PERMUTATIONS_PER_PITCH_RANGE);
	}
	sound->ranges = malloc(range_count * sizeof(*sound->ranges));
	sound->permutations = malloc((size_t)MAX(permutation_total, 1) * sizeof(*sound->permutations));
	*reason = "out of memory";
	if (!sound->ranges || !sound->permutations)
	{
		goto refused;
	}
	memset(sound->ranges, 0, range_count * sizeof(*sound->ranges));
	memset(sound->permutations, 0, (size_t)MAX(permutation_total, 1) * sizeof(*sound->permutations));
	permutation_capacity = MAX(permutation_total, 1);
	{
		byte const *ranges = file + cursor;

		cursor += range_count * PITCH_RANGE_SIZE;
		for (range_index = 0; range_index < range_count; range_index++)
		{
			byte const *range_bytes = ranges + range_index * PITCH_RANGE_SIZE;
			struct sound_pitch_range *range = &sound->ranges[range_index];
			unsigned long count = mods_read_be32(range_bytes + offsetof(struct sound_pitch_range, permutations.count));
			struct sound_permutation *permutations = sound->permutations + sound->permutation_count;
			byte const *permutation_bytes = file + cursor;
			unsigned long permutation_index;
			long range_pieces = 0;
			real *lengths;

			*reason = count < 1 ? "a pitch range has no permutations (an empty sound)" :
				"a pitch range's permutations are not in the file";
			if (count < 1 || count > MAXIMUM_PERMUTATIONS_PER_PITCH_RANGE ||
				!mods_in_file(cursor, count * PERMUTATION_SIZE, file_size))
			{
				goto refused;
			}
			cursor += count * PERMUTATION_SIZE;
			range_first[range_index] = sound->permutation_count;
			memcpy(range->name, range_bytes, sizeof(range->name));
			range->name[sizeof(range->name) - 1] = 0;
			range->natural_pitch = mods_read_real(range_bytes + offsetof(struct sound_pitch_range, natural_pitch));
			range->bend_bounds.lower = mods_read_real(range_bytes + offsetof(struct sound_pitch_range, bend_bounds.lower));
			range->bend_bounds.upper = mods_read_real(range_bytes + offsetof(struct sound_pitch_range, bend_bounds.upper));
			range->actual_permutation_count = linked ?
				mods_read_short(range_bytes + offsetof(struct sound_pitch_range, actual_permutation_count)) : (short)count;
			*reason = "a value of a pitch range is out of range";
			if (!mods_finite_real(range->natural_pitch) || !mods_finite_real(range->bend_bounds.lower) ||
				!mods_finite_real(range->bend_bounds.upper) ||
				range->actual_permutation_count < 1 ||
				range->actual_permutation_count > MAXIMUM_PERMUTATIONS_PER_RANDOM_PITCH_RANGE ||
				(unsigned long)range->actual_permutation_count > count)
			{
				goto refused;
			}
			/* (as a map's build makes them) */
			if (range->natural_pitch <= 0.f)
				range->natural_pitch = 1.f;
			range->bend_bounds.lower = MIN(range->bend_bounds.lower, range->natural_pitch);
			range->bend_bounds.upper = MAX(range->bend_bounds.upper, range->natural_pitch);
			if (range->bend_bounds.lower < 0.f)
				goto refused;
			range->playback_rate = 1.f / range->natural_pitch;
			range->played_permutation_mask = 0xFFFFFFFF;
			range->previous_permutation_index = NONE;
			range->forced_permutation_index = NONE;
			range->permutations.count = (long)count;
			range->permutations.address = permutations;
			range->permutations.definition = NULL;

			/* each permutation, its samples made this build's */
			lengths = (real *)malloc(count * sizeof(real));
			if (!lengths)
			{
				*reason = "out of memory";
				goto refused;
			}
			for (permutation_index = 0; permutation_index < count; permutation_index++)
			{
				byte const *bytes = permutation_bytes + permutation_index * PERMUTATION_SIZE;
				struct sound_permutation *permutation = &permutations[permutation_index];
				unsigned long samples_size = mods_read_be32(bytes + offsetof(struct sound_permutation, samples.size));
				unsigned long mouth_size = mods_read_be32(bytes + offsetof(struct sound_permutation, mouth_data.size));
				unsigned long subtitle_size = mods_read_be32(bytes + offsetof(struct sound_permutation, subtitle_data.size));
				unsigned long buffer_size = mods_read_be32(bytes + offsetof(struct sound_permutation, sample_buffer_size));
				short compression = mods_read_short(bytes + offsetof(struct sound_permutation, compression));
				short next = mods_read_short(bytes + offsetof(struct sound_permutation, next_permutation_index));
				byte const *samples = file + cursor;
				unsigned long encoded_bytes = 0;
				long kept_rate = 0;

				sound->permutation_count++;
				*reason = "a permutation's data is not in the file";
				if (samples_size == 0 || samples_size > MAXIMUM_MOD_TAG_FILE_SIZE ||
					mouth_size > MAXIMUM_SOUND_MOUTH_DATA_SIZE || subtitle_size > MAXIMUM_SOUND_SUBTITLE_DATA_SIZE ||
					!mods_in_file(cursor, samples_size, file_size) ||
					!mods_in_file(cursor + samples_size, mouth_size, file_size) ||
					!mods_in_file(cursor + samples_size + mouth_size, subtitle_size, file_size))
				{
					free(lengths);
					goto refused;
				}
				*reason = "a permutation's values are out of range";
				memcpy(permutation->name, bytes, sizeof(permutation->name));
				permutation->name[sizeof(permutation->name) - 1] = 0;
				permutation->skip_fraction = mods_read_real(bytes + offsetof(struct sound_permutation, skip_fraction));
				permutation->gain = mods_read_real(bytes + offsetof(struct sound_permutation, gain));
				if (compression != definition->compression ||
					!mods_finite_real(permutation->skip_fraction) || !mods_finite_real(permutation->gain) ||
					(linked && next != NONE && (next < 0 || (unsigned long)next >= count)) ||
					(compression == SOUND_COMPRESSION_XBOX_ADPCM && samples_size % (ADPCM_BLOCK_BYTES * channels)) ||
					(compression == SOUND_COMPRESSION_NONE && samples_size % (2 * channels)))
				{
					free(lengths);
					goto refused;
				}
				permutation->next_permutation_index = linked ? next : NONE;
				lengths[permutation_index] = permutation_length(definition, range, compression,
					samples_size, buffer_size, rate);

				/* the samples (Xbox ADPCM at the rate played is played as it is) */
				if (compression == SOUND_COMPRESSION_XBOX_ADPCM && rate == encoded_rate)
				{
					permutation->samples.address = malloc(samples_size);
					if (permutation->samples.address)
					{
						memcpy(permutation->samples.address, samples, samples_size);
						encoded_bytes = samples_size;
					}
				}
				else
				{
					permutation->samples.address = custom_edition_sounds_encode(samples, (long)samples_size,
						compression, TRUE, channels, rate, encoded_rate, native_budget(sound, file_size),
						&encoded_bytes, &kept_rate);
					/* (PCM longer than one permutation is kept in is split into
					pieces, range_split: Xbox ADPCM where the range has no room
					for them) */
					if (permutation->samples.address && kept_rate)
					{
						long more = piece_count(encoded_bytes) - 1;

						if (count + range_pieces + more > MAXIMUM_PERMUTATIONS_PER_PITCH_RANGE)
						{
							byte *pcm = permutation->samples.address;

							permutation->samples.address = custom_edition_sounds_encode(pcm, (long)encoded_bytes,
								SOUND_COMPRESSION_NONE, FALSE, channels, kept_rate, encoded_rate, 0,
								&encoded_bytes, &kept_rate);
							free(pcm);
						}
						else
						{
							range_pieces += more;
						}
					}
					if (permutation->samples.address && !kept_rate)
					{
						sound->adpcm_count++;
					}
				}
				/* (PCM of any length: it is split, range_split) */
				if (!permutation->samples.address || (!kept_rate && encoded_bytes > MAXIMUM_SOUND_DATA_SIZE))
				{
					*reason = permutation->samples.address ?
						"a permutation is longer than a sound cache block takes" :
						"a permutation's samples cannot be decoded";
					free(lengths);
					goto refused;
				}
				permutation->samples.size = (long)encoded_bytes;
				permutation->samples.file_offset = 0;
				/* (16-bit PCM at its own rate: sound_permutation_pcm_rate) */
				permutation->compression = kept_rate ? SOUND_COMPRESSION_NONE : SOUND_COMPRESSION_XBOX_ADPCM;
				permutation->sample_buffer_size = (unsigned long)kept_rate;
				sound->kept_bytes += encoded_bytes;
				cursor += samples_size;
				/* (the mouth and subtitle data stay in the file kept) */
				permutation->mouth_data.size = (long)mouth_size;
				permutation->mouth_data.address = mouth_size ? file + cursor : NULL;
				cursor += mouth_size;
				permutation->subtitle_data.size = (long)subtitle_size;
				permutation->subtitle_data.address = subtitle_size ? file + cursor : NULL;
				cursor += subtitle_size;
				/* the sound cache's (xbox_sound_cache.c: block, address, tag) */
				permutation->unknown0 = NONE;
				permutation->unknown1 = 0;
				permutation->unknown2 = (unsigned long)tag_index;
				permutation->unknown3 = (unsigned long)tag_index;
			}

			/* a map's build's gains, and the longest the range plays: a
			permutation, or a chain of them that must end */
			for (permutation_index = 0; permutation_index < (unsigned long)range->actual_permutation_count; permutation_index++)
			{
				struct sound_permutation *first = &permutations[permutation_index];
				short link = first->next_permutation_index;
				real length = lengths[permutation_index];
				unsigned long steps = 0;

				if (first->gain == 0.f)
					first->gain = 1.f;
				while (link != NONE)
				{
					if (++steps > count)
					{
						*reason = "a chain of permutations never ends";
						free(lengths);
						goto refused;
					}
					permutations[link].gain = first->gain;
					permutations[link].skip_fraction = 0.f;
					length += lengths[link];
					link = permutations[link].next_permutation_index;
				}
				longest = MAX(longest, length);
			}
			if (!linked)
			{
				for (permutation_index = 0; permutation_index < count; permutation_index++)
				{
					longest = MAX(longest, lengths[permutation_index]);
				}
			}
			free(lengths);

			/* the permutations too long for one, in pieces after the range's */
			if (range_pieces)
			{
				split = TRUE;
				if (!range_split(sound, range, range_first[range_index], count, &permutation_capacity))
				{
					*reason = "out of memory";
					goto refused;
				}
			}
		}
	}
	*reason = "the file goes on past its last permutation";
	if (cursor != file_size || !(longest < 2147483647.f))
	{
		goto refused;
	}
	/* (the pieces may have moved the permutations; a chain of them is a
	linked sound's, game_sound.c and sound_manager.c following it) */
	for (range_index = 0; range_index < range_count; range_index++)
	{
		sound->ranges[range_index].permutations.address = sound->permutations + range_first[range_index];
	}
	if (split)
	{
		SET_FLAG(definition->flags, _sound_definition_linked_permutations_bit, TRUE);
	}

	definition->longest_permutation_length = (long)longest;
	definition->compression = SOUND_COMPRESSION_XBOX_ADPCM;
	definition->sample_rate = encoded_rate == 44100 ? 1 : 0;
	definition->pitch_ranges.count = (long)range_count;
	definition->pitch_ranges.address = sound->ranges;
	sound->file = file;
	sound->file_size = file_size;
	sound->kept_bytes += file_size;

	return sound;

refused:
	mod_sound_free(sound);

	return NULL;
}

/* Gives `definition` the values `from` has of a sound tag, keeping what
the game counts as it plays the sound (from promotion_counter on). */
static void definition_set(
	struct sound_definition *definition,
	struct sound_definition const *from)
{
	memcpy(definition, from, offsetof(struct sound_definition, promotion_counter));
	definition->pitch_ranges = from->pitch_ranges;

	return;
}

/* Lets go of the sounds not in use: none of an override's, and none that
holds a sound cache block (the cache's blocks point at their permutations). */
static void sounds_let_go(
	boolean all)
{
	struct mod_sound **reference = &mod_sounds_globals.sounds;

	while (*reference)
	{
		struct mod_sound *sound = *reference;
		boolean in_use = FALSE;
		long index;

		if (!all)
		{
			struct mod_sound_override const *override =
				&mod_sounds_globals.overrides[DATUM_INDEX_TO_ABSOLUTE_INDEX(sound->tag_index)];

			in_use = override->sound == sound;
			for (index = 0; !in_use && index < sound->permutation_count; index++)
			{
				in_use = (long)sound->permutations[index].unknown0 != NONE;
			}
		}
		if (in_use)
		{
			reference = &sound->next;
		}
		else
		{
			*reference = sound->next;
			mods_memory_give(sound->kept_bytes);
			mod_sound_free(sound);
		}
	}

	return;
}

/* the override of the sound tag `tag_index` */
static struct mod_sound_override *override_get(
	long tag_index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index);

	return absolute_index >= 0 && absolute_index < mod_sounds_globals.override_count ?
		&mod_sounds_globals.overrides[absolute_index] : NULL;
}

/* Makes the table of overrides, one for each of the map's tags up to its
last sound tag; FALSE when it cannot be. */
static boolean overrides_new(
	void)
{
	struct tag_iterator iterator;
	long tag_index;
	long count = 0;

	if (mod_sounds_globals.overrides)
	{
		return TRUE;
	}
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		count = MAX(count, DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) + 1);
	}
	if (count <= 0 || !(mod_sounds_globals.overrides = malloc((size_t)count * sizeof(struct mod_sound_override))))
	{
		return FALSE;
	}
	memset(mod_sounds_globals.overrides, 0, (size_t)count * sizeof(struct mod_sound_override));
	mod_sounds_globals.override_count = count;

	return TRUE;
}

/* ---------- public code */

long mod_sounds_update(
	boolean remove_all,
	boolean at_load)
{
	struct tag_iterator iterator;
	long tag_index;
	struct mod_sound *made = NULL;
	long loaded_count = 0;
	long removed_count = 0;
	long refused_count = 0;
	long played_count = 0;
	boolean stopped = FALSE;

	if (!mods_map_loaded() || !overrides_new())
	{
		return 0;
	}

	/* what each file makes, before anything is changed */
	tag_iterator_new(&iterator, SOUND_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct mod_sound_override *override = override_get(tag_index);
		long file_index = remove_all ? NONE : mods_tag_file_find(tag_index, TAG_FILE_EXTENSION);
		byte *file = NULL;
		unsigned long file_size = 0;
		char const *reason = NULL;
		char path[512];
		char const *file_path = path;
		char const *file_owner = "the data root's tags folder";
		boolean missing = TRUE;

		if (!override)
		{
			continue;
		}
		if (loose_sounds_enabled() && tag_file_path(tag_get_name(tag_index), path, sizeof(path)))
		{
			file = file_read(path, &file_size, &missing);
			if (!missing && !file)
				reason = "it cannot be read or is not the size of a sound tag file";
		}
		if (missing && file_index != NONE)
		{
			file_path = port_mods_file_path(file_index);
			file_owner = port_mods_file_mod(file_index);
			file = mods_tag_file_read(file_index, &file_size, &reason);
		}
		if (!file && reason)
		{
			error(_error_silent, "mods: %s left out: %s", file_path, reason);
			console_warning("mods: %s.sound of %s left out: %s", tag_get_name(tag_index),
				file_owner, reason);
			refused_count++;
			continue;
		}
		if (file && override->sound && override->sound->file_size == file_size &&
			!memcmp(override->sound->file, file, file_size))
		{
			/* (unchanged) */
			free(file);
			continue;
		}
		if (file)
		{
			/* (refused before its samples are decoded when the file alone
			is more than the mods may keep) */
			struct mod_sound *sound = mods_memory_left() < file_size ? NULL :
				mod_sound_new(tag_index, file, file_size, &reason);

			if (!sound && mods_memory_left() < file_size)
			{
				reason = "the mods' tags already hold too much memory";
			}
			if (sound && !mods_memory_take(sound->kept_bytes))
			{
				reason = "the mods' tags already hold too much memory";
				sound->file = NULL;
				mod_sound_free(sound);
				sound = NULL;
			}
			if (!sound)
			{
				/* (one refused leaves what was played before) */
				error(_error_silent, "mods: %s left out: %s", file_path, reason);
				console_warning("mods: %s.sound of %s left out: %s", tag_get_name(tag_index),
					file_owner, reason);
				free(file);
				refused_count++;
				continue;
			}
			sound->next = made;
			made = sound;
			if (sound->adpcm_count)
			{
				error(_error_silent, "mods: %s: %ld of its permutations kept as Xbox ADPCM, not 16-bit PCM at their own rates "
					"(past the memory the mods may keep)", file_path, sound->adpcm_count);
			}
		}
		else if (!override->sound)
		{
			continue;
		}

		/* a change: nothing plays while it is made */
		if (!stopped && !at_load)
		{
			sound_stop_all();
			sound_cache_flush();
			stopped = TRUE;
		}
		if (override->sound)
		{
			definition_set(sound_definition_get(tag_index), &override->map_definition);
			override->sound = NULL;
			if (!file)
				removed_count++;
		}
		if (file)
		{
			struct sound_definition *definition = sound_definition_get(tag_index);

			memcpy(&override->map_definition, definition, sizeof(*definition));
			definition_set(definition, &made->definition);
			override->sound = made;
			loaded_count++;
		}
	}

	/* the sounds made join those kept, and those no longer played go */
	while (made)
	{
		struct mod_sound *next = made->next;

		made->next = mod_sounds_globals.sounds;
		mod_sounds_globals.sounds = made;
		made = next;
	}
	sounds_let_go(FALSE);

	for (tag_index = 0; tag_index < mod_sounds_globals.override_count; tag_index++)
	{
		if (mod_sounds_globals.overrides[tag_index].sound)
			played_count++;
	}
	if (!at_load || loaded_count || refused_count)
	{
		error(_error_silent, "mods: %ld sounds from their files (%ld new), %ld given back, %ld left out",
			played_count, loaded_count, removed_count, refused_count);
		if (!at_load && mods_say())
		{
			console_printf(FALSE, "mods: %ld sounds from their files (%ld new), %ld given back, %ld left out",
				played_count, loaded_count, removed_count, refused_count);
		}
	}

	return played_count;
}

/* (once the sound cache is closed: no block holds a permutation) */
void mod_sounds_tags_unloaded(
	void)
{
	sounds_let_go(TRUE);
	if (mod_sounds_globals.overrides)
		free(mod_sounds_globals.overrides);
	memset(&mod_sounds_globals, 0, sizeof(mod_sounds_globals));

	return;
}

boolean mod_sounds_read(
	struct sound_permutation const *permutation,
	void *buffer)
{
	struct mod_sound const *sound;

	for (sound = mod_sounds_globals.sounds; sound; sound = sound->next)
	{
		if (permutation >= sound->permutations && permutation < sound->permutations + sound->permutation_count)
		{
			memcpy(buffer, permutation->samples.address, (size_t)permutation->samples.size);
			return TRUE;
		}
	}

	return FALSE;
}
