/*
CUSTOM_EDITION_SOUNDS.C

The sounds of Halo Custom Edition maps this build would not play
(custom_edition_cache.h).

The sound manager plays sounds of the Xbox's formats only, Xbox ADPCM mono at
22 kHz or stereo at 22 or 44 kHz (sound_manager.c), and refuses any other
each time it is played. Halo PC also has Ogg Vorbis (music, dialogue, the
announcer), uncompressed samples and mono sounds at 44 kHz. When a map is
loaded, each such permutation is decoded here (Ogg Vorbis with
port/third_party/stb_vorbis) and brought to the sound's channel count, into
one buffer kept for as long as the map is loaded, its samples in a region of
the combined offset space of its own (custom_edition_cache_read serves it
from the buffer).

It is kept as 16-bit PCM at its own rate (OpenH1 issues 1110 and 1111), so
nothing is lost: sound_definitions.h's sound_permutation_pcm_rate marks it,
and the channel that plays it switches its stream to that format
(sound_dsound_xbox.c). A rate over the mixer's 48 kHz (which nothing past it
is heard at) is brought down to it. A mod's permutation longer than a sound
cache block holds as PCM (NATIVE_MAXIMUM_BYTES) stays PCM all the same:
mod_sounds.c splits it into a chain of linked pieces of NATIVE_PIECE_BYTES
(OpenH1 issue 1167). A map's cannot be split (its permutations are in the
map's tag cache, which has no room for more), so it, and any permutation
past the PCM budget of all of them (NATIVE_TOTAL_BYTES), is encoded as Xbox
ADPCM in a format the channels play, as before, and said once. Wherever the
rate changes, the samples are resampled with a windowed sinc low pass (frames_resample), not by the
nearest frame, which folded what lies above the new rate's band back into it
as tones that were never there (a 16 kHz tone came out at 6 kHz).
The sound itself stays one of the Xbox's formats, so the sound manager and
its channels see nothing new.

Xbox ADPCM is IMA ADPCM in blocks of 64 samples per channel: a 4-byte
header per channel (the predictor and step index the block starts from),
then 4-byte groups of eight nibbles, low nibble first, alternating between
the channels (port/linux/src/dsound_sdl.c decodes it).
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "tag_files/tag_groups.h"
#include "sound/sound_definitions.h"
#include "cache_file_formats.h"
#include "custom_edition_cache.h"

#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/stb_vorbis/stb_vorbis.c"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define SOUND_COMPRESSION_NONE 0
#define SOUND_COMPRESSION_XBOX_ADPCM 1
#define SOUND_COMPRESSION_OGG_VORBIS 3

#define ADPCM_BLOCK_SAMPLES 64
#define ADPCM_BLOCK_BYTES 36
#define ADPCM_STEP_INDEX_MAXIMUM 88

/* the decoded buffer grows by this much at a time */
#define DECODED_GROWTH 0x100000

/* an Ogg Vorbis stream is decoded into a buffer that grows by this many
frames; no packet decodes to more than 4096 frames */
#define VORBIS_GROWTH_FRAMES 4096
/* The most frames a permutation has at any stage (decoded, or brought to
the rate this build plays): over six minutes at 44 kHz, and 64 MB of
stereo samples. Every buffer of frames is sized from a count within it
(frames_allocate), so that count * channels * 2 cannot wrap the 32-bit
size_t of this build: a stream that said it was at 1 Hz once made a count
whose bytes wrapped to a few KB, which the resampler then wrote 4 GB into */
#define MAXIMUM_FRAMES 0x1000000L
/* The most bytes a permutation's samples are read in: as many frames as
that, stereo, uncompressed. Any longer decode to more frames than that, so
are not read at all (Xbox ADPCM and Ogg Vorbis are smaller than the
uncompressed samples they hold, and no stream has more than two channels). */
#define MAXIMUM_SAMPLES_BYTES (MAXIMUM_FRAMES * 2 * (long)sizeof(short))
/* the highest rate a stream is taken to be at (its own rate is read from
it, and the resampler scales by it) */
#define MAXIMUM_SAMPLE_RATE 192000

/* The most bytes of 16-bit PCM a permutation is kept in: what one sound
cache block takes, and no more than a quarter of the sound cache (a
quarter of the phones' 4 MB, 12 seconds of mono at 44 kHz; the desktop's
4 MB, 47 seconds of mono at 44 kHz or 23 of stereo). A longer one of a
map's is Xbox ADPCM, 3.6 times smaller; a mod's is split into pieces. */
#define NATIVE_MAXIMUM_BYTES MIN((long)MAXIMUM_SOUND_DATA_SIZE, (long)(HALO_PORT_SOUND_CACHE_SIZE / 4))
/* The bytes of each piece a longer one is split into (mod_sounds.c): half
of that, in whole 4 KB pages (whole frames, mono or stereo), so that the
piece playing and the next one queued after it hold no more of the sound
cache than one permutation of the most did (512 KB on the phones, 6 s of
mono at 44 kHz; 2 MB on the desktop). */
#define NATIVE_PIECE_BYTES ((unsigned long)(NATIVE_MAXIMUM_BYTES / 2) & ~0xFFFUL)
/* the most bytes of PCM a map's permutations are kept in, together (128 MB
on the desktop, 32 MB on the phones); those past it are Xbox ADPCM */
#define NATIVE_TOTAL_BYTES ((unsigned long)HALO_PORT_SOUND_CACHE_SIZE * 8)

/* The resampler (frames_resample): a Kaiser windowed sinc of
RESAMPLE_ZERO_CROSSINGS zero crossings each side at the lower of the two
rates, its pass band to RESAMPLE_CUTOFF of that rate's half, tabulated in
RESAMPLE_TABLE_STEPS steps per zero crossing. Its stop band is about 90 dB
down: what lies above the lower rate's band is gone, not folded back. */
#define RESAMPLE_ZERO_CROSSINGS 32
#define RESAMPLE_TABLE_STEPS 512
#define RESAMPLE_CUTOFF 0.9
#define RESAMPLE_KAISER_BETA 9.0
#define RESAMPLE_PI 3.14159265358979323846

/* A sound as sounds.map holds it (custom_edition_sounds_resource_sound;
cache_file_formats.c, resource_sound_load): the entry named by the tag's
path is the sound's header (the Xbox's 32-bit layout), then its pitch
ranges and their permutations, whose addresses count from the first pitch
range; a permutation's samples lie in sounds.map, at their file offset,
when the first bit of their flags is set. */
#define ENTRY_SOUND_BYTES 0xA4
#define ENTRY_SAMPLE_RATE_OFFSET 0x06
#define ENTRY_RANDOM_PITCH_BOUNDS_OFFSET 0x14
#define ENTRY_ZERO_PITCH_MODIFIER_OFFSET 0x44
#define ENTRY_ONE_PITCH_MODIFIER_OFFSET 0x5C
#define ENTRY_ENCODING_OFFSET 0x6C
#define ENTRY_PITCH_RANGES_OFFSET 0x98
#define ENTRY_PITCH_RANGE_BYTES 0x48
#define ENTRY_PITCH_RANGE_PERMUTATIONS_OFFSET 0x3C
#define ENTRY_PERMUTATION_BYTES 0x7C
#define ENTRY_PERMUTATION_COMPRESSION_OFFSET 0x28
#define ENTRY_PERMUTATION_SAMPLES_OFFSET 0x40
#define ENTRY_DATA_SIZE_OFFSET 0x00
#define ENTRY_DATA_FLAGS_OFFSET 0x04
#define ENTRY_DATA_FILE_OFFSET_OFFSET 0x08
#define ENTRY_SAMPLES_IN_RESOURCE_MAP_BIT 0
/* the most bytes of an entry read (a short sound's: its pitch ranges and
permutations) and of its first permutation's samples */
#define ENTRY_MAXIMUM_BYTES 0x10000
#define ENTRY_MAXIMUM_SAMPLES_BYTES 0x100000

/* ---------- globals */

static float resample_table[RESAMPLE_ZERO_CROSSINGS * RESAMPLE_TABLE_STEPS + 2];
static boolean resample_table_made;

static int const adpcm_step_table[ADPCM_STEP_INDEX_MAXIMUM + 1] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};

static int const adpcm_index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static struct
{
	byte *decoded;
	unsigned long decoded_bytes;
	unsigned long decoded_capacity;
	/* the most the region of the combined offset space for them holds */
	unsigned long decoded_limit;
	/* the bytes of them that are PCM (NATIVE_TOTAL_BYTES) */
	unsigned long native_bytes;
	/* those too long to keep as PCM, Xbox ADPCM instead */
	long adpcm_count;
} custom_edition_sounds_globals;

/* ---------- private code */

struct adpcm_state
{
	int predictor;
	int step_index;
};

/* The nibble that brings `state` nearest `sample`, moving it as the decoder will. */
static byte adpcm_encode_sample(
	struct adpcm_state *state,
	int sample)
{
	int step = adpcm_step_table[state->step_index];
	int difference = sample - state->predictor;
	int change = step >> 3;
	byte nibble = 0;

	if (difference < 0)
	{
		nibble = 8;
		difference = -difference;
	}
	if (difference >= step)
	{
		nibble |= 4;
		difference -= step;
		change += step;
	}
	if (difference >= step >> 1)
	{
		nibble |= 2;
		difference -= step >> 1;
		change += step >> 1;
	}
	if (difference >= step >> 2)
	{
		nibble |= 1;
		change += step >> 2;
	}

	state->predictor += (nibble & 8) ? -change : change;
	state->predictor = PIN(state->predictor, -32768, 32767);
	state->step_index += adpcm_index_table[nibble & 7];
	state->step_index = PIN(state->step_index, 0, ADPCM_STEP_INDEX_MAXIMUM);

	return nibble;
}

/* Encodes `frame_count` frames of interleaved 16-bit samples as Xbox ADPCM into
`output`, which holds adpcm_encoded_bytes(frame_count, channel_count) bytes.
A last block short of 64 frames is filled with the final sample. */
static unsigned long adpcm_encoded_bytes(
	long frame_count,
	long channel_count)
{
	return (unsigned long)((frame_count + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES) *
		ADPCM_BLOCK_BYTES * channel_count;
}

static void adpcm_encode(
	short const *samples,
	long frame_count,
	long channel_count,
	byte *output)
{
	struct adpcm_state states[2] = { { 0, 0 }, { 0, 0 } };
	long block_count = (frame_count + ADPCM_BLOCK_SAMPLES - 1) / ADPCM_BLOCK_SAMPLES;
	long block_index;

	for (block_index = 0; block_index < block_count; block_index++)
	{
		byte *block = output + block_index * ADPCM_BLOCK_BYTES * channel_count;
		byte *nibbles = block + 4 * channel_count;
		long channel;

		for (channel = 0; channel < channel_count; channel++)
		{
			struct adpcm_state *state = &states[channel];
			byte *header = block + channel * 4;
			long group;

			header[0] = (byte)(state->predictor & 0xFF);
			header[1] = (byte)((state->predictor >> 8) & 0xFF);
			header[2] = (byte)state->step_index;
			header[3] = 0;
			for (group = 0; group < 8; group++)
			{
				byte *group_bytes = nibbles + (group * channel_count + channel) * 4;
				long byte_index;

				for (byte_index = 0; byte_index < 4; byte_index++)
				{
					long frame = block_index * ADPCM_BLOCK_SAMPLES + group * 8 + byte_index * 2;
					long first = MIN(frame, frame_count - 1);
					long second = MIN(frame + 1, frame_count - 1);
					byte low = adpcm_encode_sample(state, samples[first * channel_count + channel]);
					byte high = adpcm_encode_sample(state, samples[second * channel_count + channel]);

					group_bytes[byte_index] = (byte)(low | (high << 4));
				}
			}
		}
	}

	return;
}

/* 16-bit frames: their samples, channel count and rate */
struct frames
{
	short *samples;
	long count;
	long channels;
	long rate;
};

/* (none may have been made: the game's free, debug_free, does not take NULL) */
static void frames_free(
	struct frames *frames)
{
	if (frames->samples)
		free(frames->samples);
	frames->samples = NULL;
}

/* room for `count` frames of `channels` channels, or NULL when there are
none, too many (MAXIMUM_FRAMES), or no memory: the only way a buffer of
frames is made */
static short *frames_allocate(
	long count,
	long channels)
{
	if (count <= 0 || count > MAXIMUM_FRAMES || channels < 1 || channels > 2)
	{
		return NULL;
	}

	return malloc((size_t)count * (size_t)channels * sizeof(short));
}

/* the Ogg Vorbis stream `data`, at its own channel count and rate; FALSE
when it cannot be decoded. It is decoded a packet at a time, so a stream
of more channels than a sound has, or one longer than any sound, is
refused as soon as that shows, before it has used the memory. */
static boolean vorbis_decode(
	byte const *data,
	long data_bytes,
	struct frames *frames)
{
	int error = 0;
	stb_vorbis *vorbis = stb_vorbis_open_memory(data, (int)data_bytes, &error, NULL);
	stb_vorbis_info info;
	long capacity;
	boolean decoded = FALSE;

	if (!vorbis)
	{
		return FALSE;
	}
	info = stb_vorbis_get_info(vorbis);
	frames->count = 0;
	frames->channels = info.channels;
	frames->rate = (long)info.sample_rate;
	capacity = VORBIS_GROWTH_FRAMES;
	frames->samples = NULL;
	if (info.channels >= 1 && info.channels <= 2 && info.sample_rate > 0 && info.sample_rate <= MAXIMUM_SAMPLE_RATE)
	{
		frames->samples = frames_allocate(capacity, info.channels);
	}
	while (frames->samples)
	{
		int count;

		if (frames->count + VORBIS_GROWTH_FRAMES > capacity)
		{
			short *larger;

			/* (the limit is a power of two, so doubling reaches it exactly) */
			if (capacity >= MAXIMUM_FRAMES)
			{
				break;
			}
			larger = realloc(frames->samples, (size_t)capacity * 2 * (size_t)info.channels * sizeof(short));
			if (!larger)
			{
				break;
			}
			frames->samples = larger;
			capacity *= 2;
		}
		count = stb_vorbis_get_frame_short_interleaved(vorbis, info.channels,
			frames->samples + frames->count * info.channels,
			(int)((capacity - frames->count) * info.channels));
		if (count == 0)
		{
			decoded = frames->count > 0;
			break;
		}
		frames->count += count;
	}
	stb_vorbis_close(vorbis);
	if (!decoded)
	{
		frames_free(frames);
	}

	return decoded;
}

/* uncompressed samples, 16-bit little-endian as Halo PC's maps keep them
(big-endian as its tag files do: mod_sounds.c) */
static boolean pcm_decode(
	byte const *data,
	long data_bytes,
	long channels,
	long rate,
	boolean big_endian,
	struct frames *frames)
{
	int low = big_endian ? 1 : 0;
	long index;

	frames->count = data_bytes / (2 * channels);
	frames->channels = channels;
	frames->rate = rate;
	frames->samples = frames_allocate(frames->count, channels);
	for (index = 0; frames->samples && index < frames->count * channels; index++)
	{
		frames->samples[index] = (short)(data[2 * index + low] | (data[2 * index + 1 - low] << 8));
	}

	return frames->samples != NULL;
}

/* Xbox ADPCM, as the mixer decodes it (port/linux/src/dsound_sdl.c) */
static int adpcm_decode_nibble(
	struct adpcm_state *state,
	int nibble)
{
	int step = adpcm_step_table[state->step_index];
	int difference = step >> 3;

	if (nibble & 1)
		difference += step >> 2;
	if (nibble & 2)
		difference += step >> 1;
	if (nibble & 4)
		difference += step;
	state->predictor += (nibble & 8) ? -difference : difference;
	state->predictor = PIN(state->predictor, -32768, 32767);
	state->step_index = PIN(state->step_index + adpcm_index_table[nibble & 7], 0, ADPCM_STEP_INDEX_MAXIMUM);

	return state->predictor;
}

static boolean adpcm_decode(
	byte const *data,
	long data_bytes,
	long channels,
	long rate,
	struct frames *frames)
{
	long block_count = data_bytes / (ADPCM_BLOCK_BYTES * channels);
	long block_index;

	frames->count = block_count * ADPCM_BLOCK_SAMPLES;
	frames->channels = channels;
	frames->rate = rate;
	frames->samples = frames_allocate(frames->count, channels);
	for (block_index = 0; frames->samples && block_index < block_count; block_index++)
	{
		byte const *block = data + block_index * ADPCM_BLOCK_BYTES * channels;
		short *output = frames->samples + block_index * ADPCM_BLOCK_SAMPLES * channels;
		long channel;

		for (channel = 0; channel < channels; channel++)
		{
			byte const *header = block + channel * 4;
			struct adpcm_state state;
			long group;

			state.predictor = (short)(header[0] | (header[1] << 8));
			state.step_index = MIN(header[2], ADPCM_STEP_INDEX_MAXIMUM);
			for (group = 0; group < 8; group++)
			{
				byte const *group_bytes = block + 4 * channels + (group * channels + channel) * 4;
				long byte_index;

				for (byte_index = 0; byte_index < 4; byte_index++)
				{
					long frame = group * 8 + byte_index * 2;

					output[frame * channels + channel] = (short)adpcm_decode_nibble(&state, group_bytes[byte_index] & 0xF);
					output[(frame + 1) * channels + channel] = (short)adpcm_decode_nibble(&state, group_bytes[byte_index] >> 4);
				}
			}
		}
	}

	return frames->samples != NULL;
}

/* the samples `data` of `compression` at `channels` and `rate` (an Ogg
Vorbis stream's own); FALSE when they cannot be decoded */
static boolean samples_decode(
	byte const *data,
	long data_bytes,
	short compression,
	boolean big_endian,
	long channels,
	long rate,
	struct frames *frames)
{
	switch (compression)
	{
	case SOUND_COMPRESSION_OGG_VORBIS:
		return vorbis_decode(data, data_bytes, frames);
	case SOUND_COMPRESSION_NONE:
		return pcm_decode(data, data_bytes, channels, rate, big_endian, frames);
	case SOUND_COMPRESSION_XBOX_ADPCM:
		return adpcm_decode(data, data_bytes, channels, rate, frames);
	}

	return FALSE;
}

static double bessel_i0(
	double x)
{
	double sum = 1.0;
	double term = 1.0;
	long k;

	for (k = 1; k < 64; k++)
	{
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
		if (term < sum * 1e-17)
			break;
	}

	return sum;
}

/* the windowed sinc, from its middle out to RESAMPLE_ZERO_CROSSINGS, made once */
static void resample_table_make(
	void)
{
	long index;
	double window_scale = 1.0 / bessel_i0(RESAMPLE_KAISER_BETA);

	if (resample_table_made)
		return;
	for (index = 0; index <= RESAMPLE_ZERO_CROSSINGS * RESAMPLE_TABLE_STEPS; index++)
	{
		double x = (double)index / RESAMPLE_TABLE_STEPS;
		double ratio = x / RESAMPLE_ZERO_CROSSINGS;
		double sinc = index ? sin(RESAMPLE_PI * x) / (RESAMPLE_PI * x) : 1.0;
		double window = bessel_i0(RESAMPLE_KAISER_BETA * sqrt(MAX(0.0, 1.0 - ratio * ratio))) * window_scale;

		resample_table[index] = (float)(sinc * window);
	}
	resample_table[RESAMPLE_ZERO_CROSSINGS * RESAMPLE_TABLE_STEPS + 1] = 0.0f;
	resample_table_made = TRUE;
}

/* the windowed sinc `distance` zero crossings from its middle */
static double resample_weight(
	double distance)
{
	double at;
	long index;

	if (distance < 0.0)
		distance = -distance;
	if (distance >= RESAMPLE_ZERO_CROSSINGS)
		return 0.0;
	at = distance * RESAMPLE_TABLE_STEPS;
	index = (long)at;

	return resample_table[index] + (resample_table[index + 1] - resample_table[index]) * (at - index);
}

/* `frames` resampled to `rate` with the windowed sinc low pass at the lower
of the two rates' bands (band limited: nothing above the new rate's band is
folded into it); FALSE when it comes to nothing */
static boolean frames_resample(
	struct frames *frames,
	long rate)
{
	/* (the count at the new rate, refused before it is a long when it is
	more than any sound has: a stream's own rate can be anything) */
	double scaled_count = frames->rate > 0 ? (double)frames->count * rate / frames->rate : -1.0;
	double cutoff;
	double reach;
	long channels = frames->channels;
	long count;
	short *samples;
	long frame;

	if (frames->rate <= 0 || rate <= 0 || !(scaled_count >= 0.0) || scaled_count > (double)MAXIMUM_FRAMES)
	{
		frames_free(frames);
		return FALSE;
	}
	resample_table_make();
	/* (the pass band, as a fraction of the source's, and the source frames
	each side the low pass reaches) */
	cutoff = RESAMPLE_CUTOFF * MIN(1.0, (double)rate / frames->rate);
	reach = RESAMPLE_ZERO_CROSSINGS / cutoff;
	count = (long)scaled_count;
	samples = frames_allocate(count, channels);
	for (frame = 0; samples && frame < count; frame++)
	{
		double at = (double)frame * frames->rate / rate;
		long first = MAX((long)ceil(at - reach), 0);
		long last = MIN((long)floor(at + reach), frames->count - 1);
		double sums[2] = { 0.0, 0.0 };
		long source;
		long channel;

		for (source = first; source <= last; source++)
		{
			double weight = resample_weight((at - source) * cutoff);

			for (channel = 0; channel < channels; channel++)
			{
				sums[channel] += frames->samples[source * channels + channel] * weight;
			}
		}
		for (channel = 0; channel < channels; channel++)
		{
			long value = lround(sums[channel] * cutoff);

			samples[frame * channels + channel] = (short)PIN(value, -32768, 32767);
		}
	}
	frames_free(frames);
	frames->samples = samples;
	frames->count = count;
	frames->rate = rate;

	return samples != NULL;
}

/* `frames` at `channels` (a mono stream fills both channels, a stereo one
is averaged for mono); FALSE when it comes to nothing */
static boolean frames_channels(
	struct frames *frames,
	long channels)
{
	short *samples;
	long frame;

	if (frames->channels == channels)
	{
		return frames->count > 0;
	}
	samples = frames_allocate(frames->count, channels);
	for (frame = 0; samples && frame < frames->count; frame++)
	{
		short const *in = frames->samples + frame * frames->channels;
		short *out = samples + frame * channels;

		if (channels == 2)
			out[0] = out[1] = in[0];
		else
			out[0] = (short)((in[0] + in[1]) / 2);
	}
	frames_free(frames);
	frames->samples = samples;
	frames->channels = channels;

	return samples != NULL;
}

/* `frames` at `channels` and `rate` (frames_channels, then frames_resample
when the rate is another); FALSE when it comes to nothing. At their own
rate they are not touched. */
static boolean frames_conform(
	struct frames *frames,
	long channels,
	long rate)
{
	if (!frames_channels(frames, channels))
	{
		frames_free(frames);
		return FALSE;
	}
	if (frames->rate == rate)
	{
		return frames->count > 0;
	}

	return frames_resample(frames, rate) && frames->count > 0;
}

/* the rate a permutation decoded at `rate` is kept at: its own, up to the
mixer's (no more is heard) */
static long native_rate(
	long rate)
{
	return MIN(rate, (long)SOUND_PERMUTATION_PCM_MAXIMUM_RATE);
}

/* Room for `bytes` more in the decoded buffer: its offset, or NONE when the
memory or the region for them runs out. */
static long decoded_reserve(
	unsigned long bytes)
{
	unsigned long needed = custom_edition_sounds_globals.decoded_bytes + bytes;

	if (needed < bytes || needed > custom_edition_sounds_globals.decoded_limit)
	{
		return NONE;
	}
	if (needed > custom_edition_sounds_globals.decoded_capacity)
	{
		unsigned long capacity = (needed + DECODED_GROWTH - 1) / DECODED_GROWTH * DECODED_GROWTH;
		byte *grown = realloc(custom_edition_sounds_globals.decoded, capacity);

		if (!grown)
		{
			return NONE;
		}
		custom_edition_sounds_globals.decoded = grown;
		custom_edition_sounds_globals.decoded_capacity = capacity;
	}

	return (long)custom_edition_sounds_globals.decoded_bytes;
}

/* the format this build plays a sound in (sound_manager.c): Xbox ADPCM,
mono at 22 kHz or stereo at 22 or 44 kHz */
static long playable_rate(
	struct sound_definition const *sound)
{
	return sound->encoding == 0 ? 22050 : (sound->sample_rate == 0 ? 22050 : 44100);
}

/* whether a permutation of `sound` must be made playable here: Ogg Vorbis
and uncompressed ones, and mono ones at 44 kHz */
static boolean permutation_needs_conversion(
	struct sound_definition const *sound,
	struct sound_permutation const *permutation)
{
	return permutation->compression == SOUND_COMPRESSION_OGG_VORBIS ||
		permutation->compression == SOUND_COMPRESSION_NONE ||
		(sound->encoding == 0 && sound->sample_rate != 0);
}

/* Whether `frames` (at the sound's channel count) are kept as 16-bit PCM
at their own rate, up to the mixer's: not when that is past `budget`, the
bytes of PCM left of the budget of all of them (at most NATIVE_TOTAL_BYTES),
nor, `whole` (a permutation that cannot be split: a map's), when it is
longer than a sound cache block holds (NATIVE_MAXIMUM_BYTES); *rate is that
rate and *bytes their bytes. */
static boolean frames_kept_native(
	struct frames const *frames,
	unsigned long budget,
	boolean whole,
	long *rate,
	unsigned long *bytes)
{
	double kept_bytes;

	*rate = native_rate(frames->rate);
	*bytes = 0;
	if (frames->rate <= 0 || *rate < SOUND_PERMUTATION_PCM_MINIMUM_RATE || frames->count <= 0)
	{
		return FALSE;
	}
	kept_bytes = (double)frames->count * *rate / frames->rate * frames->channels * (double)sizeof(short);
	if ((whole && kept_bytes > (double)NATIVE_MAXIMUM_BYTES) || kept_bytes > (double)MIN(budget, NATIVE_TOTAL_BYTES))
	{
		return FALSE;
	}
	*bytes = (unsigned long)kept_bytes;

	return TRUE;
}

/* Makes the permutation `permutation` of `sound` one this build plays, its
samples in the decoded buffer: 16-bit PCM at its own rate (up to the
mixer's) where it fits (frames_kept_native), else Xbox ADPCM in the format
the sound's channels play; FALSE when it cannot be either. */
static boolean permutation_convert(
	struct sound_definition const *sound,
	struct sound_permutation *permutation,
	long decoded_offset)
{
	long channels = sound->encoding == 0 ? 1 : 2;
	long rate = sound->sample_rate == 0 ? 22050 : 44100;
	struct frames frames = { 0 };
	boolean decoded = FALSE;
	unsigned long encoded_bytes;
	unsigned long native_bytes;
	long kept_rate;
	byte *data;
	long offset = NONE;

	/* (the loader checked the samples lie in the file; none at all is no sound) */
	if (permutation->samples.size <= 0 || permutation->samples.size > MAXIMUM_SAMPLES_BYTES ||
		!(data = malloc(permutation->samples.size)))
	{
		return FALSE;
	}
	custom_edition_cache_read(NONE, permutation->samples.file_offset, permutation->samples.size, data);
	decoded = samples_decode(data, permutation->samples.size, permutation->compression, FALSE,
		channels, rate, &frames);
	free(data);

	decoded = decoded && frames_channels(&frames, channels);
	if (decoded && frames_kept_native(&frames, NATIVE_TOTAL_BYTES - custom_edition_sounds_globals.native_bytes, TRUE,
		&kept_rate, &native_bytes))
	{
		/* (only a rate over the mixer's is resampled) */
		if (frames.rate == kept_rate || frames_resample(&frames, kept_rate))
		{
			native_bytes = (unsigned long)frames.count * channels * sizeof(short);
			offset = decoded_reserve(native_bytes);
		}
		if (offset != NONE)
		{
			memcpy(custom_edition_sounds_globals.decoded + offset, frames.samples, native_bytes);
			custom_edition_sounds_globals.decoded_bytes += native_bytes;
			custom_edition_sounds_globals.native_bytes += native_bytes;
			permutation->compression = SOUND_COMPRESSION_NONE;
			permutation->samples.file_offset = decoded_offset + offset;
			permutation->samples.size = (long)native_bytes;
			permutation->sample_buffer_size = (unsigned long)kept_rate;
			frames_free(&frames);
			return TRUE;
		}
		decoded = frames.samples != NULL;
	}
	else if (decoded && frames.count > 0)
	{
		/* (too long to keep as PCM: said once, custom_edition_sounds_decode) */
		custom_edition_sounds_globals.adpcm_count++;
	}

	if (decoded && frames_conform(&frames, channels, playable_rate(sound)))
	{
		encoded_bytes = adpcm_encoded_bytes(frames.count, channels);
		offset = decoded_reserve(encoded_bytes);
		if (offset != NONE)
		{
			adpcm_encode(frames.samples, frames.count, channels, custom_edition_sounds_globals.decoded + offset);
			custom_edition_sounds_globals.decoded_bytes += encoded_bytes;
			permutation->compression = SOUND_COMPRESSION_XBOX_ADPCM;
			permutation->samples.file_offset = decoded_offset + offset;
			permutation->samples.size = (long)encoded_bytes;
			permutation->sample_buffer_size = 0;
		}
	}
	frames_free(&frames);

	return offset != NONE;
}

static int resource_sound_file_read(
	void *context,
	uint32_t offset,
	uint32_t size,
	void *buffer)
{
	FILE *stream = context;

	return fseek(stream, (long)offset, SEEK_SET) == 0 && fread(buffer, 1, size, stream) == size;
}

static long entry_long(
	byte const *bytes)
{
	int32_t value;

	memcpy(&value, bytes, sizeof(value));
	return (long)value;
}

static short entry_short(
	byte const *bytes)
{
	int16_t value;

	memcpy(&value, bytes, sizeof(value));
	return (short)value;
}

static real entry_real(
	byte const *bytes)
{
	float value;

	memcpy(&value, bytes, sizeof(value));
	return (real)value;
}

/* the pitch a sound's header gives it played at a scale of 1, as the
sound manager works it out (sound_scale_random_value: the middle of its
random pitch bounds, times its pitch modifier at 1), or 1 where the tag
gives none (as a map's build fills it in: mod_sounds.c) */
static real entry_pitch(
	byte const *header)
{
	real lower = entry_real(header + ENTRY_RANDOM_PITCH_BOUNDS_OFFSET);
	real upper = entry_real(header + ENTRY_RANDOM_PITCH_BOUNDS_OFFSET + 4);
	real zero = entry_real(header + ENTRY_ZERO_PITCH_MODIFIER_OFFSET);
	real one = entry_real(header + ENTRY_ONE_PITCH_MODIFIER_OFFSET);
	real base = lower > 0.f && upper > 0.f ? (lower + upper) / 2.f : 1.f;
	real pitch;

	if (zero == 0.f && one == 0.f)
	{
		one = 1.f;
	}
	pitch = base * one;

	return pitch >= 0.25f && pitch <= 4.f ? pitch : 1.f;
}

/* The first permutation of the sound sounds.map (`source`) holds in the
entry of `entry_size` bytes at `entry_offset`, decoded to mono 16-bit
frames at its own rate: a buffer of the game's allocator of *frame_count
frames at *rate, played at *pitch; NULL when the entry is not such a
sound. */
static short *resource_sound_entry_decode(
	struct cache_file_source *source,
	uint32_t entry_offset,
	uint32_t entry_size,
	long *frame_count,
	long *rate,
	real *pitch)
{
	byte *entry;
	byte const *range;
	byte const *permutation;
	byte const *data_field;
	byte *data = NULL;
	struct frames frames = { 0 };
	short *samples = NULL;
	long range_count, permutation_count, permutations_address, data_size, data_offset;
	long channels;

	if (entry_size < ENTRY_SOUND_BYTES + ENTRY_PITCH_RANGE_BYTES || entry_size > ENTRY_MAXIMUM_BYTES ||
		!(entry = malloc(entry_size)))
	{
		return NULL;
	}
	if (!source->read(source->context, entry_offset, entry_size, entry))
	{
		goto done;
	}
	range_count = entry_long(entry + ENTRY_PITCH_RANGES_OFFSET);
	channels = entry_short(entry + ENTRY_ENCODING_OFFSET) ? 2 : 1;
	*rate = entry_short(entry + ENTRY_SAMPLE_RATE_OFFSET) ? 44100 : 22050;
	*pitch = entry_pitch(entry);
	/* (the entry's copy of the header may leave its pitch range count zero:
	the first pitch range is there all the same) */
	if (range_count < 0 || range_count > (long)((entry_size - ENTRY_SOUND_BYTES) / ENTRY_PITCH_RANGE_BYTES))
	{
		goto done;
	}
	range = entry + ENTRY_SOUND_BYTES;
	permutation_count = entry_long(range + ENTRY_PITCH_RANGE_PERMUTATIONS_OFFSET);
	permutations_address = entry_long(range + ENTRY_PITCH_RANGE_PERMUTATIONS_OFFSET + 4);
	if (permutation_count < 1 || permutations_address < 0 ||
		(unsigned long)permutations_address + ENTRY_PERMUTATION_BYTES > entry_size - ENTRY_SOUND_BYTES)
	{
		goto done;
	}
	permutation = range + permutations_address;
	data_field = permutation + ENTRY_PERMUTATION_SAMPLES_OFFSET;
	data_size = entry_long(data_field + ENTRY_DATA_SIZE_OFFSET);
	data_offset = entry_long(data_field + ENTRY_DATA_FILE_OFFSET_OFFSET);
	if (!TEST_FLAG(entry_long(data_field + ENTRY_DATA_FLAGS_OFFSET), ENTRY_SAMPLES_IN_RESOURCE_MAP_BIT) ||
		data_size <= 0 || data_size > ENTRY_MAXIMUM_SAMPLES_BYTES || data_offset < 0 ||
		(unsigned long)data_offset + (unsigned long)data_size > source->size ||
		!(data = malloc(data_size)) ||
		!source->read(source->context, (uint32_t)data_offset, (uint32_t)data_size, data))
	{
		goto done;
	}
	if (samples_decode(data, data_size, entry_short(permutation + ENTRY_PERMUTATION_COMPRESSION_OFFSET),
			FALSE, channels, *rate, &frames) &&
		frames_channels(&frames, 1) &&
		frames.rate >= SOUND_PERMUTATION_PCM_MINIMUM_RATE && frames.rate <= MAXIMUM_SAMPLE_RATE)
	{
		samples = frames.samples;
		frames.samples = NULL;
		*frame_count = frames.count;
		*rate = frames.rate;
	}

done:
	frames_free(&frames);
	if (data)
		free(data);
	free(entry);

	return samples;
}

/* ---------- public code */

boolean custom_edition_sounds_decode(
	byte *tag_cache,
	unsigned long loaded_bytes,
	long decoded_offset,
	unsigned long decoded_limit)
{
	struct sound_definition *sound;
	int32_t tag_index = NONE;
	long converted_count = 0;
	long failed_count = 0;

	custom_edition_sounds_globals.decoded_limit = decoded_limit;
	custom_edition_sounds_globals.adpcm_count = 0;
	while ((sound = custom_edition_cache_tag_next(tag_cache, loaded_bytes, SOUND_DEFINITION_TAG, sizeof(*sound), &tag_index)) != NULL)
	{
		boolean converted = TRUE;
		boolean any = FALSE;
		long range_index;

		for (range_index = 0; range_index < sound->pitch_ranges.count; range_index++)
		{
			struct sound_pitch_range *range = custom_edition_cache_block_element(
				tag_cache, loaded_bytes, &sound->pitch_ranges, range_index, sizeof(*range));
			long permutation_index;

			for (permutation_index = 0; range && permutation_index < range->permutations.count; permutation_index++)
			{
				struct sound_permutation *permutation = custom_edition_cache_block_element(
					tag_cache, loaded_bytes, &range->permutations, permutation_index, sizeof(*permutation));

				if (!permutation || !permutation_needs_conversion(sound, permutation))
				{
					continue;
				}
				any = TRUE;
				/* (a sound whose permutations are not linked follows none, and
				one kept as 16-bit PCM only the pieces of a long one, which a
				map's never is: sound_permutation_next_piece) */
				if (!TEST_FLAG(sound->flags, _sound_definition_linked_permutations_bit))
					permutation->next_permutation_index = NONE;
				if (permutation_convert(sound, permutation, decoded_offset))
				{
					converted_count++;
				}
				else
				{
					converted = FALSE;
				}
			}
		}
		if (any || sound->compression != SOUND_COMPRESSION_XBOX_ADPCM)
		{
			sound->compression = SOUND_COMPRESSION_XBOX_ADPCM;
			if (sound->encoding == 0)
				sound->sample_rate = 0;
		}
		if (!converted)
		{
			/* with no pitch ranges the game neither plays nor loads the sound */
			error(_error_silent, "custom edition: cannot convert the sound '%s'; it will not play",
				custom_edition_cache_tag_name(tag_cache, loaded_bytes, tag_index));
			sound->pitch_ranges.count = 0;
			failed_count++;
		}
	}
	if (converted_count || failed_count)
	{
		error(_error_silent, "custom edition: %ld sound permutations converted to %lu bytes (%lu of them 16-bit PCM at their own rates)%s",
			converted_count, custom_edition_sounds_globals.decoded_bytes, custom_edition_sounds_globals.native_bytes,
			failed_count ? " (some sounds could not be converted)" : "");
	}
	if (custom_edition_sounds_globals.adpcm_count)
	{
		error(_error_silent, "custom edition: %ld sound permutations kept as Xbox ADPCM, not 16-bit PCM at their own rates: "
			"each longer than %ld KB of PCM, or past the %lu MB of PCM a map's sounds are kept in",
			custom_edition_sounds_globals.adpcm_count, (long)NATIVE_MAXIMUM_BYTES / 1024,
			(unsigned long)NATIVE_TOTAL_BYTES >> 20);
	}

	return TRUE;
}

byte *custom_edition_sounds_encode(
	byte const *data,
	long data_bytes,
	short compression,
	boolean big_endian,
	long channels,
	long rate,
	long encoded_rate,
	unsigned long native_budget,
	unsigned long *encoded_bytes,
	long *kept_rate)
{
	struct frames frames = { 0 };
	byte *encoded = NULL;
	unsigned long native_bytes;

	*kept_rate = 0;
	if (data_bytes > 0 && channels >= 1 && channels <= 2 &&
		samples_decode(data, data_bytes, compression, big_endian, channels, rate, &frames) &&
		frames_channels(&frames, channels))
	{
		/* (16-bit PCM at its own rate, as a map's: permutation_convert; of
		any length within the budget, split into pieces by the caller) */
		if (frames_kept_native(&frames, native_budget, FALSE, kept_rate, &native_bytes) &&
			(frames.rate == *kept_rate || frames_resample(&frames, *kept_rate)))
		{
			*encoded_bytes = (unsigned long)frames.count * channels * sizeof(short);
			encoded = malloc(*encoded_bytes);
			if (encoded)
			{
				memcpy(encoded, frames.samples, *encoded_bytes);
			}
		}
		else if (frames.samples && frames_conform(&frames, channels, encoded_rate))
		{
			*kept_rate = 0;
			*encoded_bytes = adpcm_encoded_bytes(frames.count, channels);
			encoded = malloc(*encoded_bytes);
			if (encoded)
			{
				adpcm_encode(frames.samples, frames.count, channels, encoded);
			}
		}
	}
	frames_free(&frames);
	if (!encoded)
	{
		*kept_rate = 0;
	}

	return encoded;
}

short *custom_edition_sounds_resource_sound(
	char const *name,
	long *frame_count,
	long *rate,
	real *pitch)
{
	char path[256];
	FILE *stream;
	long size;
	struct cache_file_source source;
	struct resource_map map;
	short *samples = NULL;
	int32_t item_index;

	if (!custom_edition_resource_map_file_path(_resource_map_sounds, path) || !(stream = fopen(path, "rb")))
	{
		return NULL;
	}
	if (fseek(stream, 0, SEEK_END) == 0 && (size = ftell(stream)) > 0)
	{
		source.context = stream;
		source.read = resource_sound_file_read;
		source.size = (uint32_t)size;
		if (resource_map_open(&source, _resource_map_sounds, &map) == _cache_file_status_ok)
		{
			for (item_index = 0; item_index < map.item_count && !samples; item_index++)
			{
				if (!csstrcasecmp(map.items[item_index].name, name))
				{
					samples = resource_sound_entry_decode(&source, map.items[item_index].data_offset,
						map.items[item_index].size, frame_count, rate, pitch);
				}
			}
			resource_map_close(&map);
		}
	}
	fclose(stream);

	return samples;
}

unsigned long custom_edition_sounds_piece_bytes(
	void)
{
	return NATIVE_PIECE_BYTES;
}

unsigned long custom_edition_sounds_decoded_bytes(
	void)
{
	return custom_edition_sounds_globals.decoded_bytes;
}

boolean custom_edition_sounds_read(
	long offset,
	long size,
	void *buffer)
{
	if (offset < 0 || size < 0 ||
		(unsigned long)offset + (unsigned long)size > custom_edition_sounds_globals.decoded_bytes)
	{
		return FALSE;
	}
	memcpy(buffer, custom_edition_sounds_globals.decoded + offset, size);

	return TRUE;
}

void custom_edition_sounds_dispose(
	void)
{
	/* (none, when no sound needed decoding: debug_free does not take NULL) */
	if (custom_edition_sounds_globals.decoded)
		free(custom_edition_sounds_globals.decoded);
	memset(&custom_edition_sounds_globals, 0, sizeof(custom_edition_sounds_globals));

	return;
}
