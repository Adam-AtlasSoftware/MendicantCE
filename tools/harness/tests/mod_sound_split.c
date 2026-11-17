/*
MOD_SOUND_SPLIT.C (test)

The real mod_sounds.c loading a sound tag file of a mod (mod_sound_new),
with the real custom_edition_sounds.c keeping its samples (OpenH1 issue
1167): a permutation of 16-bit PCM longer than one permutation is kept in
(a quarter of the ARM64 guests' 4 MB sound cache) is split into a chain of
linked pieces, each under that cap, PCM at its own rate, whose frames
joined in the chain's order are the source's, sample for sample. The chain
ends where the permutation's did, the pieces are never picked to start a
sound (sound_definitions.c's next permutation), and past the memory the
mods may keep a permutation is Xbox ADPCM, as before.
test_mod_sound_split.py takes the modules (under_test.inc) and the game's
structures (types.inc).
*/

#include "harness.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define VALID_INDEX(i, c) ((i) >= 0 && (i) < (c))
#define SET_FLAG(f, b, v) ((f) = (v) ? (f) | (1U << (b)) : (f) & ~(1U << (b)))
#define TAG_BLOCK_GET_ELEMENT(b, i, t) (&((t *)(b)->address)[i])
#define _error_silent 0
#define error(priority, ...) ((void)(priority))
typedef void *byte_swap_data_proc;

#include "types.inc"

/* ---------- the game's, faked */

/* (no map's sounds here: custom_edition_sounds_decode is not called) */
static void custom_edition_cache_read(long tag_index, long offset, long size, void *buffer)
{
	(void)tag_index; (void)offset; (void)size; (void)buffer;
	CHECK(FALSE, "a map read");
}
static void *custom_edition_cache_tag_next(byte *tag_cache, unsigned long loaded_bytes, unsigned long group_tag,
	unsigned long size, int32_t *tag_index)
{
	(void)tag_cache; (void)loaded_bytes; (void)group_tag; (void)size; (void)tag_index;
	return NULL;
}
static void *custom_edition_cache_block_element(byte *tag_cache, unsigned long loaded_bytes, struct tag_block *block,
	long index, unsigned long size)
{
	(void)tag_cache; (void)loaded_bytes; (void)block; (void)index; (void)size;
	return NULL;
}
static char const *custom_edition_cache_tag_name(byte *tag_cache, unsigned long loaded_bytes, int32_t tag_index)
{
	(void)tag_cache; (void)loaded_bytes; (void)tag_index;
	return "sound";
}

/* stb_vorbis, faked: the "stream" is a fake_vorbis record of 16-bit frames at its own rate */
struct fake_vorbis
{
	short const *samples;
	int frames;
	int channels;
	unsigned int rate;
	int read;
};
typedef struct fake_vorbis stb_vorbis;
typedef struct { unsigned int sample_rate; int channels; } stb_vorbis_info;
static stb_vorbis *stb_vorbis_open_memory(byte const *data, int length, int *error, void *alloc)
{
	static struct fake_vorbis opened;

	(void)length; (void)alloc;
	*error = 0;
	memcpy(&opened, data, sizeof(opened));
	opened.read = 0;
	return &opened;
}
static stb_vorbis_info stb_vorbis_get_info(stb_vorbis *vorbis)
{
	stb_vorbis_info info;

	info.sample_rate = vorbis->rate;
	info.channels = vorbis->channels;
	return info;
}
static int stb_vorbis_get_frame_short_interleaved(stb_vorbis *vorbis, int channels, short *buffer, int shorts)
{
	int frames = MIN(MIN(vorbis->frames - vorbis->read, shorts / channels), 1024);

	memcpy(buffer, vorbis->samples + (long)vorbis->read * channels, (size_t)frames * channels * sizeof(short));
	vorbis->read += frames;
	return frames;
}
static void stb_vorbis_close(stb_vorbis *vorbis) { (void)vorbis; }

typedef int (*cache_file_read_proc)(void *context, uint32_t offset, uint32_t size, void *buffer);
struct cache_file_source { void *context; cache_file_read_proc read; uint32_t size; };
struct resource_map_item { uint32_t data_offset; uint32_t size; char const *name; };
struct resource_map { struct cache_file_source *source; int type; int32_t item_count; struct resource_map_item *items;
	char *names; };
enum { _resource_map_sounds = 2, _cache_file_status_ok = 0 };
static int resource_map_open(struct cache_file_source *source, int type, struct resource_map *map)
{ (void)source; (void)type; (void)map; return 1; }
static void resource_map_close(struct resource_map *map) { (void)map; }
static boolean custom_edition_resource_map_file_path(short type, char *path) { (void)type; (void)path; return FALSE; }
#define csstrcasecmp strcasecmp

/* the map's tags and the mods' memory */
static long tag_loaded(unsigned long group_tag, char const *name) { (void)group_tag; (void)name; return NONE; }
static char *tag_get_name(long tag_index) { (void)tag_index; return "sound\\test"; }
static unsigned long memory_left = 512UL * 1024 * 1024;
static unsigned long mods_memory_left(void) { return memory_left; }
/* the game's seeded random (real_math.c's: a linear congruential step) */
static short seed_random_range(unsigned long *seed, short lower, short upper)
{
	*seed = *seed * 1664525UL + 1013904223UL;
	return (short)(lower + (short)(((*seed >> 16) & 0xFFFF) * (unsigned long)(upper - lower) >> 16));
}
static real real_seed_random(unsigned long *seed)
{
	*seed = *seed * 1664525UL + 1013904223UL;
	return (real)((*seed >> 16) & 0xFFFF) / 65536.f;
}

#include "under_test.inc"

/* ---------- a sound tag file */

enum { MONO = 0, STEREO = 1, RATE_22 = 0, RATE_44 = 1, PCM = 0, OGG = 3 };
#define MOUTH_BYTES 30
#define MOST_PERMUTATIONS 8

/* a permutation of the file: PCM frames (`frames` of `channels`, native shorts), or a fake Ogg Vorbis stream */
struct source
{
	short *samples;
	long frames;
	long channels;
	unsigned int rate;
	short next;
};

static byte file_bytes[0x2000000];
static unsigned long file_size;

static void put_be16(unsigned long at, unsigned value) { file_bytes[at] = (byte)(value >> 8); file_bytes[at + 1] = (byte)value; }
static void put_be32(unsigned long at, unsigned long value)
{
	put_be16(at, (unsigned)(value >> 16));
	put_be16(at + 2, (unsigned)(value & 0xFFFF));
}
static void put_real(unsigned long at, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	put_be32(at, bits);
}

/* The tag file of a sound: `encoding`, its tag's rate, `compression`, linked or not, of one pitch range of `count`
permutations, `actual` of them picked; the first permutation has MOUTH_BYTES of mouth data. */
static void file_make(short encoding, short sample_rate, short compression, boolean linked, short actual,
	struct source const *sources, int count)
{
	unsigned long root = 0x40, range = root + 0xA4, permutations = range + 0x48, cursor;
	int index;

	CHECK(sizeof(struct sound_definition) == 0xA4 && sizeof(struct sound_pitch_range) == 0x48 &&
		sizeof(struct sound_permutation) == 0x7C, "not the game's 32-bit structures");
	memset(file_bytes, 0, permutations + (unsigned long)count * 0x7C);
	put_be32(MOD_TAG_FILE_GROUP_OFFSET, SOUND_DEFINITION_TAG);
	put_be32(MOD_TAG_FILE_HEADER_SIZE_OFFSET, MOD_TAG_FILE_HEADER_SIZE);
	put_be16(MOD_TAG_FILE_VERSION_OFFSET, SOUND_DEFINITION_VERSION);
	memcpy(file_bytes + MOD_TAG_FILE_SIGNATURE_OFFSET, "blam", 4);
	put_be32(root + offsetof(struct sound_definition, flags), linked ? 1UL << _sound_definition_linked_permutations_bit : 0);
	put_be16(root + offsetof(struct sound_definition, sound_class), 34);
	put_be16(root + offsetof(struct sound_definition, sample_rate), (unsigned)sample_rate);
	put_be16(root + offsetof(struct sound_definition, encoding), (unsigned)encoding);
	put_be16(root + offsetof(struct sound_definition, compression), (unsigned)compression);
	put_real(root + offsetof(struct sound_definition, random_pitch_bounds.lower), 1.f);
	put_real(root + offsetof(struct sound_definition, random_pitch_bounds.upper), 1.f);
	put_be32(root + offsetof(struct sound_definition, pitch_ranges.count), 1);
	memcpy(file_bytes + range, "default", 7);
	put_real(range + offsetof(struct sound_pitch_range, natural_pitch), 1.f);
	put_be16(range + offsetof(struct sound_pitch_range, actual_permutation_count), (unsigned)actual);
	put_be32(range + offsetof(struct sound_pitch_range, permutations.count), (unsigned long)count);
	cursor = permutations + (unsigned long)count * 0x7C;
	for (index = 0; index < count; index++)
	{
		unsigned long at = permutations + (unsigned long)index * 0x7C;
		struct source const *source = &sources[index];
		unsigned long bytes;

		sprintf((char *)file_bytes + at, "piece %d", index);
		put_real(at + offsetof(struct sound_permutation, gain), 1.f);
		put_be16(at + offsetof(struct sound_permutation, compression), (unsigned)compression);
		put_be16(at + offsetof(struct sound_permutation, next_permutation_index), (unsigned)(unsigned short)source->next);
		if (compression == OGG)
		{
			struct fake_vorbis vorbis = { source->samples, (int)source->frames, (int)source->channels, source->rate, 0 };

			bytes = sizeof(vorbis);
			memcpy(file_bytes + cursor, &vorbis, bytes);
			put_be32(at + offsetof(struct sound_permutation, sample_buffer_size),
				(unsigned long)source->frames * 2 * (unsigned long)source->channels);
		}
		else
		{
			long sample;

			bytes = (unsigned long)source->frames * (unsigned long)source->channels * 2;
			CHECK(cursor + bytes + MOUTH_BYTES <= sizeof(file_bytes), "the file is too small for the test");
			for (sample = 0; sample < source->frames * source->channels; sample++)
				put_be16(cursor + (unsigned long)sample * 2, (unsigned)(unsigned short)source->samples[sample]);
		}
		put_be32(at + offsetof(struct sound_permutation, samples.size), bytes);
		cursor += bytes;
		if (index == 0)
		{
			put_be32(at + offsetof(struct sound_permutation, mouth_data.size), MOUTH_BYTES);
			memset(file_bytes + cursor, 7, MOUTH_BYTES);
			cursor += MOUTH_BYTES;
		}
	}
	file_size = cursor;
}

/* the sound mod_sound_new makes of the file (a copy of it, which the sound keeps) */
static struct mod_sound *sound_load(void)
{
	byte *file = malloc(file_size);
	char const *reason = NULL;
	struct mod_sound *sound;

	CHECK(file, "no memory");
	memcpy(file, file_bytes, file_size);
	sound = mod_sound_new(1, file, file_size, &reason);
	CHECK(sound, "the sound was left out: %s", reason ? reason : "?");
	return sound;
}

/* frames of a sweep of `channels` (each channel its own), every sample distinct from its neighbours */
static short *sweep_make(long frames, long channels)
{
	short *samples = malloc((size_t)(frames * channels) * sizeof(short));
	long sample;

	CHECK(samples, "no memory");
	for (sample = 0; sample < frames * channels; sample++)
		samples[sample] = (short)(lround(12000.0 * sin(0.001 * (double)sample * (1.0 + sample % 7))) + sample % 13);
	return samples;
}

static struct sound_pitch_range *the_range(struct mod_sound *sound)
{
	CHECK(sound->definition.pitch_ranges.count == 1, "%ld pitch ranges", (long)sound->definition.pitch_ranges.count);
	return (struct sound_pitch_range *)sound->definition.pitch_ranges.address;
}

static struct sound_permutation *permutation_get(struct mod_sound *sound, short index)
{
	struct sound_pitch_range *range = the_range(sound);

	CHECK(VALID_INDEX(index, range->permutations.count), "permutation %d of %ld", index, (long)range->permutations.count);
	return TAG_BLOCK_GET_ELEMENT(&range->permutations, index, struct sound_permutation);
}

/* Walks the chain from `first`: each permutation PCM at `rate` and no larger than the cap, its samples joined
into `joined` (at most `most` samples); its pieces counted; *last the index that follows the chain's last one
before `stop` (NONE: the chain's end). The samples joined. */
static long chain_join(struct mod_sound *sound, short first, short stop, long rate, short *joined, long most,
	long *pieces)
{
	short index = first;
	long count = 0;
	long steps = 0;

	*pieces = 0;
	while (index != NONE && index != stop)
	{
		struct sound_permutation *permutation = permutation_get(sound, index);
		long samples = permutation->samples.size / (long)sizeof(short);

		CHECK(++steps <= MAXIMUM_PERMUTATIONS_PER_PITCH_RANGE, "the chain never ends");
		CHECK(sound_permutation_pcm_rate(permutation) == rate, "permutation %d is not PCM at %ld Hz (compression %d, "
			"rate %lu)", index, rate, permutation->compression, permutation->sample_buffer_size);
		CHECK(permutation->samples.size <= (long)(HALO_PORT_SOUND_CACHE_SIZE / 4) &&
			permutation->samples.size <= (long)custom_edition_sounds_piece_bytes() * 2,
			"permutation %d of %ld bytes is over the cap", index, (long)permutation->samples.size);
		CHECK(count + samples <= most, "the chain is longer than its source");
		memcpy(joined + count, permutation->samples.address, (size_t)permutation->samples.size);
		count += samples;
		(*pieces)++;
		index = permutation->next_permutation_index;
	}
	CHECK(index == stop, "the chain from %d ends at %d, not %d", first, index, stop);
	return count;
}

/* the chain from `first` (up to `stop`) is the source's samples, in order */
static long chain_check(struct mod_sound *sound, short first, short stop, struct source const *source, long rate)
{
	long samples = source->frames * source->channels;
	short *joined = malloc((size_t)samples * sizeof(short));
	long pieces, count, sample;

	CHECK(joined, "no memory");
	count = chain_join(sound, first, stop, rate, joined, samples, &pieces);
	CHECK(count == samples, "the chain has %ld samples, its source %ld", count, samples);
	for (sample = 0; sample < samples && joined[sample] == source->samples[sample]; sample++)
		;
	CHECK(sample == samples, "the chain's sample %ld is %d, its source's %d", sample, joined[sample],
		source->samples[sample]);
	free(joined);
	return pieces;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	unsigned long piece_bytes = custom_edition_sounds_piece_bytes();

	CHECK(HALO_PORT_SOUND_CACHE_SIZE == 0x400000 && piece_bytes == 0x80000, "not the ARM64 guests' cap: pieces of %lu",
		piece_bytes);
	if (!strcmp(name, "mono-30s") || !strcmp(name, "stereo-15s-48k"))
	{
		/* 30 s of mono at 44.1 kHz (2.6 MB of PCM), or 15 s of stereo at 48 kHz (Ogg Vorbis, 2.9 MB of PCM): a
		chain of six pieces of at most 512 KB, PCM at the source's rate, joined the source's samples; the mouth
		data on the first; the sound's longest time the whole chain's */
		boolean mono = !strcmp(name, "mono-30s");
		struct source source = { NULL, mono ? 30L * 44100 : 15L * 48000, mono ? 1 : 2, mono ? 44100 : 48000, NONE };
		struct mod_sound *sound;
		struct sound_permutation *first;
		long pieces;
		short index;

		source.samples = sweep_make(source.frames, source.channels);
		file_make(mono ? MONO : STEREO, RATE_44, mono ? PCM : OGG, FALSE, 1, &source, 1);
		sound = sound_load();
		CHECK(!TEST_FLAG(sound->definition.flags, _sound_definition_linked_permutations_bit),
			"the sound was made a linked one: it would no longer play as it did");
		CHECK(sound->definition.compression == 1, "the sound is not one of the Xbox's formats");
		CHECK(the_range(sound)->actual_permutation_count == 1, "%d permutations picked from",
			the_range(sound)->actual_permutation_count);
		pieces = chain_check(sound, 0, NONE, &source, (long)source.rate);
		CHECK(pieces == 6 && the_range(sound)->permutations.count == 6, "%ld pieces of %ld permutations", pieces,
			(long)the_range(sound)->permutations.count);
		first = permutation_get(sound, 0);
		CHECK(first->mouth_data.size == MOUTH_BYTES, "the first piece's mouth data is %ld bytes",
			(long)first->mouth_data.size);
		for (index = 1; index < 6; index++)
		{
			struct sound_permutation *piece = permutation_get(sound, index);

			CHECK(piece->mouth_data.size == 0 && piece->subtitle_data.size == 0 && piece->skip_fraction == 0.f &&
				piece->gain == first->gain, "piece %d has the first's mouth data, skip or another gain", index);
			CHECK((long)piece->unknown0 == NONE && piece->unknown1 == 0 && piece->unknown2 == 1,
				"piece %d is not the sound cache's to load", index);
		}
		/* (Tool's measure of an Ogg Vorbis permutation is from its tag's buffer size, which the fake's is not) */
		CHECK(!mono || (sound->definition.longest_permutation_length >= 29900 &&
			sound->definition.longest_permutation_length <= 30100),
			"the sound's longest is %ld ms", (long)sound->definition.longest_permutation_length);
		CHECK(sound->adpcm_count == 0, "a permutation was kept as Xbox ADPCM");
	}
	else if (!strcmp(name, "linked-chain"))
	{
		/* a linked sound of a 30 s permutation followed by a 1 s one: the long one's pieces between them */
		struct source sources[2] = { { NULL, 30L * 44100, 1, 44100, 1 }, { NULL, 44100, 1, 44100, NONE } };
		struct mod_sound *sound;
		long pieces;

		sources[0].samples = sweep_make(sources[0].frames, 1);
		sources[1].samples = sweep_make(sources[1].frames, 1);
		file_make(MONO, RATE_44, PCM, TRUE, 1, sources, 2);
		sound = sound_load();
		pieces = chain_check(sound, 0, 1, &sources[0], 44100);
		CHECK(pieces == 6, "%ld pieces", pieces);
		CHECK(chain_check(sound, 1, NONE, &sources[1], 44100) == 1, "the second permutation was split");
		CHECK(the_range(sound)->permutations.count == 7, "%ld permutations", (long)the_range(sound)->permutations.count);
	}
	else if (!strcmp(name, "under-cap"))
	{
		/* 10 s of mono at 44.1 kHz (861 KB): one permutation as before, not split, the sound not linked */
		struct source source = { NULL, 10L * 44100, 1, 44100, NONE };
		struct mod_sound *sound;

		source.samples = sweep_make(source.frames, 1);
		file_make(MONO, RATE_44, PCM, FALSE, 1, &source, 1);
		sound = sound_load();
		CHECK(chain_check(sound, 0, NONE, &source, 44100) == 1, "a permutation under the cap was split");
		CHECK(the_range(sound)->permutations.count == 1, "%ld permutations", (long)the_range(sound)->permutations.count);
		CHECK(!TEST_FLAG(sound->definition.flags, _sound_definition_linked_permutations_bit), "the sound was linked");
	}
	else if (!strcmp(name, "loop-wrap"))
	{
		/* two 30 s permutations picked from: from each, the next permutation (as a looping sound's track takes it)
		is its pieces in turn, and after the last a pick of one of the two again, never a piece nor none */
		struct source sources[2] = { { NULL, 30L * 44100, 1, 44100, NONE }, { NULL, 30L * 44100, 1, 44100, NONE } };
		struct mod_sound *sound;
		unsigned long seed;
		short first;

		sources[0].samples = sweep_make(sources[0].frames, 1);
		sources[1].samples = sweep_make(sources[1].frames, 1);
		file_make(MONO, RATE_44, PCM, FALSE, 2, sources, 2);
		sound = sound_load();
		CHECK(the_range(sound)->actual_permutation_count == 2, "%d picked from", the_range(sound)->actual_permutation_count);
		for (first = 0; first < 2; first++)
		{
			short index = first;
			long steps = 0;

			CHECK(chain_check(sound, first, NONE, &sources[first], 44100) == 6, "permutation %d's chain", first);
			seed = 1;
			for (steps = 1; steps < 6; steps++)
			{
				short next = sound_definition_next_permutation_from_seed(&sound->definition, 0, index, &seed, FALSE);

				CHECK(next == permutation_get(sound, index)->next_permutation_index && next >= 2,
					"piece %ld of %d's chain is %d", steps, first, next);
				index = next;
			}
			index = sound_definition_next_permutation_from_seed(&sound->definition, 0, index, &seed, FALSE);
			CHECK(index == 0 || index == 1, "after %d's last piece came %d, not a pick", first, index);
		}
		for (seed = 1; seed < 400; seed += 3)
		{
			unsigned long state = seed;
			short pick = sound_definition_next_permutation_from_seed(&sound->definition, 0, NONE, &state, FALSE);

			CHECK(pick == 0 || pick == 1, "the loop wrapped to permutation %d, a piece", pick);
		}
	}
	else if (!strcmp(name, "fake-impulse-repeats"))
	{
		/* a looping sound's one 30 s permutation, not linked, which the sound manager played over and over (a
		fake impulse one stops only at a linked chain's end, NONE: update_channel_for_looping_sound): split, its
		track takes its pieces in turn and back to its first, round after round, never NONE */
		struct source source = { NULL, 30L * 44100, 1, 44100, NONE };
		struct mod_sound *sound;
		unsigned long seed = 7;
		short index, expected = 0;
		long step;

		source.samples = sweep_make(source.frames, 1);
		file_make(MONO, RATE_44, PCM, FALSE, 1, &source, 1);
		sound = sound_load();
		CHECK(chain_check(sound, 0, NONE, &source, 44100) == 6, "not six pieces");
		index = sound_definition_next_permutation_from_seed(&sound->definition, 0, NONE, &seed, FALSE);
		for (step = 0; step < 4 * 6; step++)
		{
			CHECK(index == expected, "step %ld played %d, not %d", step, index, expected);
			index = sound_definition_next_permutation_from_seed(&sound->definition, 0, index, &seed, FALSE);
			CHECK(index != NONE, "the sound stopped after round %ld (a fake impulse loop plays once)", step / 6 + 1);
			expected = expected == 0 ? 1 : expected == 5 ? 0 : (short)(expected + 1);
		}
	}
	else if (!strcmp(name, "too-many-pieces"))
	{
		/* five 5-minute permutations of mono at 48 kHz (Ogg Vorbis, 55 pieces each): the range has room for the
		pieces of four (5 + 4 * 54 = 221 permutations of the 256 it may have), the fifth Xbox ADPCM, said */
		struct source sources[5];
		short *samples = sweep_make(300L * 48000, 1);
		struct mod_sound *sound;
		short index;

		for (index = 0; index < 5; index++)
		{
			struct source source = { samples, 300L * 48000, 1, 48000, NONE };

			sources[index] = source;
		}
		file_make(MONO, RATE_44, OGG, FALSE, 5, sources, 5);
		sound = sound_load();
		CHECK(the_range(sound)->permutations.count == 221, "%ld permutations",
			(long)the_range(sound)->permutations.count);
		for (index = 0; index < 4; index++)
			CHECK(chain_check(sound, index, NONE, &sources[index], 48000) == 55, "permutation %d's chain", index);
		CHECK(permutation_get(sound, 4)->compression == 1 && !sound_permutation_pcm_rate(permutation_get(sound, 4)),
			"the fifth is not Xbox ADPCM");
		CHECK(sound->adpcm_count == 1, "%ld permutations said kept as Xbox ADPCM", sound->adpcm_count);
	}
	else if (!strcmp(name, "over-budget"))
	{
		/* the mods may keep 1 MB more: the 30 s permutation is Xbox ADPCM at 22.05 kHz, one permutation, said */
		struct source source = { NULL, 30L * 44100, 1, 44100, NONE };
		struct mod_sound *sound;
		struct sound_permutation *permutation;

		memory_left = 1UL << 20;
		source.samples = sweep_make(source.frames, 1);
		file_make(MONO, RATE_44, PCM, FALSE, 1, &source, 1);
		sound = sound_load();
		permutation = permutation_get(sound, 0);
		CHECK(the_range(sound)->permutations.count == 1, "%ld permutations", (long)the_range(sound)->permutations.count);
		CHECK(permutation->compression == 1 && !sound_permutation_pcm_rate(permutation), "it is not Xbox ADPCM");
		CHECK(permutation->samples.size >= (long)adpcm_encoded_bytes(30L * 22050 - 64, 1) &&
			permutation->samples.size <= (long)adpcm_encoded_bytes(30L * 22050 + 64, 1), "%ld bytes of Xbox ADPCM",
			(long)permutation->samples.size);
		CHECK(sound->definition.sample_rate == 0 && sound->adpcm_count == 1, "not the old fallback, or not said");
	}
	else
	{
		fprintf(stderr, "no case '%s'\n", name);
		return 2;
	}
	return 0;
}
