/*
CUSTOM_EDITION_SOUND_IMPORT.C (test)

The real custom_edition_sounds.c over a map of one sound (OpenH1 issues 1110
and 1111): a permutation the channels would not play is kept as 16-bit PCM
at its own rate, sample for sample (a high tone is not folded down: there is
no conversion at all), a 48 kHz one at 48 kHz; one too long for that is Xbox
ADPCM at a rate the channels play, resampled with a band-limited low pass, so
a tone above the new rate's band is gone, not folded back into it as another
tone, and one inside it keeps its level. test_custom_edition_sound_import.py
takes the module from its constants on (under_test.inc) and the game's
structures (types.inc).
*/

#include "harness.h"
#include <math.h>
#include <stdint.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define _error_silent 0
#define error(priority, ...) ((void)(priority))
typedef void *byte_swap_data_proc;

#include "types.inc"

/* the fake map: one sound, one pitch range, one permutation */
static struct sound_definition the_sound;
static struct sound_pitch_range the_range;
static struct sound_permutation the_permutation;
/* the permutation's samples, as the map holds them */
static byte map_samples[1 << 20];

static void custom_edition_cache_read(long tag_index, long offset, long size, void *buffer)
{
	(void)tag_index;
	CHECK(offset >= 0 && offset + size <= (long)sizeof(map_samples), "read outside the map");
	memcpy(buffer, map_samples + offset, (size_t)size);
}

static void *custom_edition_cache_tag_next(byte *tag_cache, unsigned long loaded_bytes, unsigned long group_tag,
	unsigned long size, int32_t *tag_index)
{
	(void)tag_cache;
	(void)loaded_bytes;
	(void)group_tag;
	(void)size;
	if (*tag_index != NONE)
		return NULL;
	*tag_index = 0;
	return &the_sound;
}

static void *custom_edition_cache_block_element(byte *tag_cache, unsigned long loaded_bytes, struct tag_block *block,
	long index, unsigned long size)
{
	(void)tag_cache;
	(void)loaded_bytes;
	(void)size;
	return index >= 0 && index < block->count ? (byte *)block->address + index * (long)size : NULL;
}

static char const *custom_edition_cache_tag_name(byte *tag_cache, unsigned long loaded_bytes, int32_t tag_index)
{
	(void)tag_cache;
	(void)loaded_bytes;
	(void)tag_index;
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
	(void)length;
	(void)alloc;
	*error = 0;
	return (stb_vorbis *)(void *)data;
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

	memcpy(buffer, vorbis->samples + vorbis->read * channels, (size_t)frames * channels * sizeof(short));
	vorbis->read += frames;
	return frames;
}

static void stb_vorbis_close(stb_vorbis *vorbis)
{
	(void)vorbis;
}

/* sounds.map (cache_file_formats.h): a source of bytes, and the index resource_map_open reads (not used: the
cases read an entry directly) */
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
#include <strings.h>

#include "under_test.inc"

/* a fake sounds.map: bytes read from memory */
static byte resource_bytes[0x4000];
static int resource_read(void *context, uint32_t offset, uint32_t size, void *buffer)
{
	(void)context;
	if (offset + size > sizeof(resource_bytes))
		return 0;
	memcpy(buffer, resource_bytes + offset, size);
	return 1;
}
static void put_long(long at, int32_t value) { memcpy(resource_bytes + at, &value, sizeof(value)); }
static void put_short(long at, int16_t value) { memcpy(resource_bytes + at, &value, sizeof(value)); }
static void put_real(long at, float value) { memcpy(resource_bytes + at, &value, sizeof(value)); }

#define PI 3.14159265358979323846
#define FRAMES 8192

static short tone[2 * FRAMES];

/* a tone of `frequency` Hz at `rate`, at half of full scale, `channels` channels */
static void tone_make(double frequency, double rate, int channels, int frames)
{
	int frame, channel;

	for (frame = 0; frame < frames; frame++)
		for (channel = 0; channel < channels; channel++)
			tone[frame * channels + channel] = (short)lround(16384.0 * sin(2.0 * PI * frequency * frame / rate));
}

/* the map's sound: `encoding` (0 mono, 1 stereo), its tag's rate (0 22 kHz, 1 44 kHz), one permutation of
`compression` whose `bytes` of samples are at offset 0 of the map */
static void map_make(short encoding, short sample_rate, short compression, long bytes)
{
	memset(&the_sound, 0, sizeof(the_sound));
	memset(&the_range, 0, sizeof(the_range));
	memset(&the_permutation, 0, sizeof(the_permutation));
	custom_edition_sounds_dispose();
	the_sound.encoding = encoding;
	the_sound.sample_rate = sample_rate;
	the_sound.compression = compression;
	the_sound.pitch_ranges.count = 1;
	the_sound.pitch_ranges.address = &the_range;
	the_range.permutations.count = 1;
	the_range.permutations.address = &the_permutation;
	the_permutation.compression = compression;
	the_permutation.samples.size = bytes;
	the_permutation.samples.file_offset = 0;
	CHECK(custom_edition_sounds_decode(NULL, 0, 0, 0x10000000), "the sounds were not decoded");
	CHECK(the_sound.pitch_ranges.count == 1, "the sound was silenced");
	CHECK(the_sound.compression == 1, "the sound is not one of the Xbox's formats");
}

/* the permutation's samples, as the sound cache reads them */
static byte const *kept_samples(void)
{
	static byte kept[1 << 20];

	CHECK(the_permutation.samples.size > 0 && the_permutation.samples.size <= (long)sizeof(kept), "no samples kept");
	CHECK(custom_edition_sounds_read(the_permutation.samples.file_offset, the_permutation.samples.size, kept),
		"the samples kept cannot be read");
	return kept;
}

/* the level of `frequency` Hz in mono `samples` at `rate`, relative to the tone's half of full scale, in dB
(Goertzel, Hann windowed) */
static double level_db(short const *samples, long frames, double frequency, double rate)
{
	double coefficient = 2.0 * cos(2.0 * PI * frequency / rate);
	double previous = 0.0, before = 0.0, power, window_sum = 0.0;
	long frame;

	for (frame = 0; frame < frames; frame++)
	{
		double window = 0.5 - 0.5 * cos(2.0 * PI * frame / (frames - 1));
		double value = samples[frame] * window + coefficient * previous - before;

		window_sum += window;
		before = previous;
		previous = value;
	}
	power = previous * previous + before * before - coefficient * previous * before;
	return 20.0 * log10(sqrt(power) / (window_sum * 16384.0 / 2.0) + 1e-12);
}

/* the permutation's Xbox ADPCM decoded, as the mixer decodes it */
static long kept_adpcm_decoded(short *out, long most)
{
	struct frames frames = { 0 };
	long count;

	CHECK(the_permutation.compression == 1, "the permutation is not Xbox ADPCM");
	CHECK(adpcm_decode(kept_samples(), the_permutation.samples.size, 1, 22050, &frames), "it does not decode");
	count = MIN(frames.count, most);
	memcpy(out, frames.samples, (size_t)count * sizeof(short));
	frames_free(&frames);
	return count;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "high-tone"))
	{
		/* a mono 44.1 kHz uncompressed permutation of a 16 kHz tone: kept as it is, at 44.1 kHz */
		tone_make(16000.0, 44100.0, 1, FRAMES);
		memcpy(map_samples, tone, FRAMES * sizeof(short));
		map_make(0, 1, 0, FRAMES * (long)sizeof(short));
		CHECK(sound_permutation_pcm_rate(&the_permutation) == 44100, "the permutation is not 16-bit PCM at 44.1 kHz "
			"(compression %d, rate %lu)", the_permutation.compression, the_permutation.sample_buffer_size);
		CHECK(the_permutation.samples.size == FRAMES * (long)sizeof(short), "%ld bytes kept",
			the_permutation.samples.size);
		CHECK(!memcmp(kept_samples(), tone, FRAMES * sizeof(short)), "the samples kept are not the map's");
		CHECK(the_sound.sample_rate == 0, "a mono sound is not given the mono channels' rate");
	}
	else if (!strcmp(name, "48k-pcm"))
	{
		/* a stereo Ogg Vorbis permutation at 48 kHz: kept at 48 kHz, frame for frame */
		struct fake_vorbis vorbis = { tone, FRAMES, 2, 48000, 0 };

		tone_make(1000.0, 48000.0, 2, FRAMES);
		memcpy(map_samples, &vorbis, sizeof(vorbis));
		map_make(1, 1, 3, (long)sizeof(vorbis));
		CHECK(sound_permutation_pcm_rate(&the_permutation) == 48000, "the permutation is not 16-bit PCM at 48 kHz "
			"(compression %d, rate %lu)", the_permutation.compression, the_permutation.sample_buffer_size);
		CHECK(the_permutation.samples.size == FRAMES * 2 * (long)sizeof(short), "%ld bytes kept",
			the_permutation.samples.size);
		CHECK(!memcmp(kept_samples(), tone, FRAMES * 2 * sizeof(short)), "the samples kept are not the stream's");
	}
	else if (!strcmp(name, "too-long"))
	{
		/* a mono 44.1 kHz permutation longer than PCM is kept for (NATIVE_MAXIMUM_BYTES): Xbox ADPCM at 22.05 kHz.
		A 16 kHz tone, above 22.05 kHz's band, is gone (nearest frames folded it to 6.05 kHz at full level); a
		3 kHz one keeps its level */
		static short decoded[FRAMES];
		long long_frames = NATIVE_MAXIMUM_BYTES / (long)sizeof(short) + 2 * FRAMES;
		long frame, count;

		CHECK(long_frames * (long)sizeof(short) <= (long)sizeof(map_samples), "the map is too small for the test");
		for (frame = 0; frame < long_frames; frame++)
			((short *)map_samples)[frame] = (short)lround(16384.0 * sin(2.0 * PI * 16000.0 * frame / 44100.0));
		map_make(0, 1, 0, long_frames * (long)sizeof(short));
		CHECK(custom_edition_sounds_globals.adpcm_count == 1, "%ld permutations said kept as Xbox ADPCM",
			custom_edition_sounds_globals.adpcm_count);
		count = kept_adpcm_decoded(decoded, FRAMES);
		CHECK(count == FRAMES, "%ld frames", count);
		CHECK(level_db(decoded + 1024, FRAMES - 2048, 6050.0, 22050.0) < -60.0,
			"the 16 kHz tone came back at 6.05 kHz, %.1f dB", level_db(decoded + 1024, FRAMES - 2048, 6050.0, 22050.0));

		for (frame = 0; frame < long_frames; frame++)
			((short *)map_samples)[frame] = (short)lround(16384.0 * sin(2.0 * PI * 3000.0 * frame / 44100.0));
		map_make(0, 1, 0, long_frames * (long)sizeof(short));
		count = kept_adpcm_decoded(decoded, FRAMES);
		CHECK(fabs(level_db(decoded + 1024, FRAMES - 2048, 3000.0, 22050.0)) < 0.5,
			"the 3 kHz tone is at %.2f dB", level_db(decoded + 1024, FRAMES - 2048, 3000.0, 22050.0));
	}
	else if (!strcmp(name, "over-total"))
	{
		/* a short permutation (16 KB of PCM) when the map's permutations already keep all but 8 KB of the PCM they
		may (NATIVE_TOTAL_BYTES): Xbox ADPCM, as before (OpenH1 issue 1167 splits only a mod's), and said */
		tone_make(3000.0, 44100.0, 1, FRAMES);
		memcpy(map_samples, tone, FRAMES * sizeof(short));
		map_make(0, 1, 0, FRAMES * (long)sizeof(short));
		CHECK(sound_permutation_pcm_rate(&the_permutation) == 44100, "under the total it is not PCM");
		the_permutation.compression = 0;
		the_permutation.sample_buffer_size = 0;
		the_permutation.samples.size = FRAMES * (long)sizeof(short);
		the_permutation.samples.file_offset = 0;
		custom_edition_sounds_dispose();
		custom_edition_sounds_globals.native_bytes = NATIVE_TOTAL_BYTES - 0x2000;
		CHECK(custom_edition_sounds_decode(NULL, 0, 0, 0x10000000), "the sounds were not decoded");
		CHECK(the_permutation.compression == 1 && !sound_permutation_pcm_rate(&the_permutation),
			"past the map's total it is not Xbox ADPCM");
		CHECK(custom_edition_sounds_globals.adpcm_count == 1, "it was not said");
	}
	else if (!strcmp(name, "resource-entry"))
	{
		/* Halo PC's hit sound as sounds.map holds it (sound\sfx\impulse\ting\ting): a mono 22 kHz sound whose
		pitch modifier at 1 is 0.6 (its zero 0), one pitch range of one permutation of 16-bit samples in
		sounds.map: its frames as they are, at 22.05 kHz, played at 0.6 */
		struct cache_file_source source = { NULL, resource_read, sizeof(resource_bytes) };
		long const range = ENTRY_SOUND_BYTES;
		long const permutation = range + ENTRY_PITCH_RANGE_BYTES;
		long const entry_bytes = permutation + ENTRY_PERMUTATION_BYTES;
		long frame_count = 0, rate = 0, frame;
		real pitch = 0.f;
		short *samples;

		memset(resource_bytes, 0, sizeof(resource_bytes));
		put_short(ENTRY_SAMPLE_RATE_OFFSET, 0);
		put_real(ENTRY_RANDOM_PITCH_BOUNDS_OFFSET, 1.f);
		put_real(ENTRY_RANDOM_PITCH_BOUNDS_OFFSET + 4, 1.f);
		put_real(ENTRY_ZERO_PITCH_MODIFIER_OFFSET, 0.f);
		put_real(ENTRY_ONE_PITCH_MODIFIER_OFFSET, 0.6f);
		put_short(ENTRY_ENCODING_OFFSET, 0);
		put_long(ENTRY_PITCH_RANGES_OFFSET, 1);
		put_long(range + ENTRY_PITCH_RANGE_PERMUTATIONS_OFFSET, 1);
		put_long(range + ENTRY_PITCH_RANGE_PERMUTATIONS_OFFSET + 4, ENTRY_PITCH_RANGE_BYTES);
		put_short(permutation + ENTRY_PERMUTATION_COMPRESSION_OFFSET, 0);
		put_long(permutation + ENTRY_PERMUTATION_SAMPLES_OFFSET + ENTRY_DATA_SIZE_OFFSET, 400 * 2);
		put_long(permutation + ENTRY_PERMUTATION_SAMPLES_OFFSET + ENTRY_DATA_FLAGS_OFFSET, 1);
		put_long(permutation + ENTRY_PERMUTATION_SAMPLES_OFFSET + ENTRY_DATA_FILE_OFFSET_OFFSET, 0x1000);
		for (frame = 0; frame < 400; frame++)
			put_short(0x1000 + frame * 2, (int16_t)(frame * 50 - 10000));
		samples = resource_sound_entry_decode(&source, 0, (uint32_t)entry_bytes, &frame_count, &rate, &pitch);
		CHECK(samples, "the entry was not read");
		CHECK(frame_count == 400 && rate == 22050, "%ld frames at %ld Hz", frame_count, rate);
		CHECK(pitch == 0.6f, "it plays at a pitch of %.3f, not 0.6", pitch);
		CHECK(!memcmp(samples, resource_bytes + 0x1000, 400 * 2), "its frames are not sounds.map's");
		free(samples);

		/* (samples in the map file, not sounds.map: not read) */
		put_long(permutation + ENTRY_PERMUTATION_SAMPLES_OFFSET + ENTRY_DATA_FLAGS_OFFSET, 0);
		CHECK(!resource_sound_entry_decode(&source, 0, (uint32_t)entry_bytes, &frame_count, &rate, &pitch),
			"samples outside sounds.map were read from it");
	}
	else
	{
		fprintf(stderr, "no case '%s'\n", name);
		return 2;
	}
	return 0;
}
