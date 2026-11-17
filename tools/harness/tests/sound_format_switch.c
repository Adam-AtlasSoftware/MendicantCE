/*
SOUND_FORMAT_SWITCH.C (test)

The real sound_dsound_xbox.c switching a channel's one stream between its
type's Xbox ADPCM and 16-bit PCM at a permutation's own rate
(dsound_channel_use_format, OpenH1 issue 1111), over fake DirectSound
streams that record what they are given: the stream switched in has the
channel's settings, the one switched out is kept for the switch back, and
the channels and the kept streams never come to more than the 256
(MAXIMUM_SOUND_CHANNELS) the game has, every channel switching. A
permutation queued after one of another format waits for its packets, then
plays in its own (dsound_channel_queue_packet, dsound_channel_fill, the
callback). test_sound_format_switch.py takes the code (under_test.inc) and
the structures (types.inc).
*/

#include "harness.h"
#include <math.h>
#include "types.inc"

#define csmemset memset
#define NUMBEROF(a) ((long)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define channel_get(i) (&dsound_globals.channels[i])
#define match_vassert(...) ((void)0)
/* (the Xbox's pointer-sized context is a channel number) */
#pragma clang diagnostic ignored "-Wvoid-pointer-to-int-cast"
#pragma clang diagnostic ignored "-Wint-to-void-pointer-cast"

typedef unsigned short WORD;
typedef float FLOAT;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample,
	cbSize; } WAVEFORMATEX;
typedef struct { WAVEFORMATEX wfx; WORD wSamplesPerBlock; } XBOXADPCMWAVEFORMAT;
typedef struct { DWORD dwFlags, dwMaxAttachedPackets; WAVEFORMATEX *lpwfxFormat; LPFNXMEDIAOBJECTCALLBACK lpfnCallback;
	LPVOID lpvContext; } DSSTREAMDESC;

static struct dsound_globals dsound_globals;
static char temporary[256];
static unsigned long const sound_sample_rate_samples_per_second[2] = { 22050, 44100 };
static unsigned long sound_samples_per_second(short sample_rate) { return sound_sample_rate_samples_per_second[sample_rate]; }
static void dsound_error(HRESULT result, char const *format, ...) { (void)result; (void)format; }
#define interrupt_time_error(result, message) CHECK(FALSE, "%s", message)
static byte sound_cache[1 << 20];
static void *physical_memory_get_sound_cache_base_address(void) { return sound_cache; }
static int locks;
#define sound_cache_sound_hardware_lock(sound) ((void)locks++)
#define sound_cache_sound_hardware_unlock(sound) ((void)locks--)

static void CALLBACK dsound_channel_callback(void *stream_context, void *packet_context, unsigned long status);
static void dsound_channel_fill(short index);

/* the fake streams: what each was made as and given */
struct fake_stream
{
	IDirectSoundStream object;
	int alive, pcm, channels, has_3d, context;
	unsigned long rate, frequency, mix_bins, mode;
	long volume, mix_bin_volume, cone_outside_volume;
	float position[3], velocity[3], minimum_distance, maximum_distance;
	int i3dl2_given, stereo_positioned;
	float stereo_position[3];
	int packets;
	unsigned long packet_bytes[256];
	void *packet_contexts[256];
	int packet_count;
};
/* the bytes every packet sent to any stream held, in the order sent (the "chained" case) */
static byte captured[1 << 18];
static unsigned long captured_bytes;
static struct fake_stream fake_streams[2048];
static int fake_stream_count, alive_streams, most_alive_streams;
static struct fake_stream *fake(IDirectSoundStream *stream) { return (struct fake_stream *)(void *)stream; }

static HRESULT IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, DSSTREAMDESC *description, IDirectSoundStream **stream,
	void *outer)
{
	struct fake_stream *made;

	(void)sound;
	(void)outer;
	CHECK(fake_stream_count < (int)NUMBEROF(fake_streams), "too many streams made");
	made = &fake_streams[fake_stream_count++];
	made->alive = 1;
	made->pcm = description->lpwfxFormat->wFormatTag == WAVE_FORMAT_PCM;
	made->channels = description->lpwfxFormat->nChannels;
	made->rate = description->lpwfxFormat->nSamplesPerSec;
	made->has_3d = (description->dwFlags & DSSTREAMCAPS_CTRL3D) != 0;
	made->context = (int)(long)description->lpvContext;
	CHECK(description->lpfnCallback == dsound_channel_callback, "the stream's callback is not the channels'");
	alive_streams++;
	most_alive_streams = MAX(most_alive_streams, alive_streams);
	*stream = &made->object;
	return 0;
}
static void IDirectSoundStream_Release(IDirectSoundStream *stream)
{
	CHECK(fake(stream)->alive, "a stream released twice");
	CHECK(fake(stream)->packets == 0, "a stream released with its packets");
	fake(stream)->alive = 0;
	alive_streams--;
}
#define IDirectSound_GetSpeakerConfig(sound, config) (*(config) = 0, 0)
#define IDirectSound_CommitDeferredSettings(sound) 0
static HRESULT IDirectSoundStream_SetVolume(IDirectSoundStream *s, LONG v) { fake(s)->volume = v; return 0; }
static HRESULT IDirectSoundStream_SetFrequency(IDirectSoundStream *s, DWORD f) { fake(s)->frequency = f; return 0; }
static HRESULT IDirectSoundStream_SetMode(IDirectSoundStream *s, DWORD m, DWORD a) { (void)a; fake(s)->mode = m; return 0; }
static HRESULT IDirectSoundStream_SetPosition(IDirectSoundStream *s, FLOAT x, FLOAT y, FLOAT z, DWORD a)
{ (void)a; fake(s)->position[0] = x; fake(s)->position[1] = y; fake(s)->position[2] = z; return 0; }
static HRESULT IDirectSoundStream_SetVelocity(IDirectSoundStream *s, FLOAT x, FLOAT y, FLOAT z, DWORD a)
{ (void)a; fake(s)->velocity[0] = x; fake(s)->velocity[1] = y; fake(s)->velocity[2] = z; return 0; }
static HRESULT IDirectSoundStream_SetConeOrientation(IDirectSoundStream *s, FLOAT x, FLOAT y, FLOAT z, DWORD a)
{ (void)s; (void)x; (void)y; (void)z; (void)a; return 0; }
static HRESULT IDirectSoundStream_SetMaxDistance(IDirectSoundStream *s, FLOAT d, DWORD a)
{ (void)a; fake(s)->maximum_distance = d; return 0; }
static HRESULT IDirectSoundStream_SetMinDistance(IDirectSoundStream *s, FLOAT d, DWORD a)
{ (void)a; fake(s)->minimum_distance = d; return 0; }
static HRESULT IDirectSoundStream_SetConeAngles(IDirectSoundStream *s, DWORD i, DWORD o, DWORD a)
{ (void)s; (void)i; (void)o; (void)a; return 0; }
static HRESULT IDirectSoundStream_SetConeOutsideVolume(IDirectSoundStream *s, LONG v, DWORD a)
{ (void)a; fake(s)->cone_outside_volume = v; return 0; }
static HRESULT IDirectSoundStream_SetMixBins(IDirectSoundStream *s, DWORD bins) { fake(s)->mix_bins = bins; return 0; }
static HRESULT IDirectSoundStream_SetMixBinVolumes(IDirectSoundStream *s, DWORD bins, const LONG *volumes)
{ (void)bins; fake(s)->mix_bin_volume = volumes[0]; return 0; }
static HRESULT IDirectSoundStream_GetStatus(IDirectSoundStream *s, DWORD *status)
{ *status = fake(s)->packets < MAXIMUM_SOUND_PACKETS ? 1 : 0; return 0; }
static HRESULT IDirectSoundStream_Process(IDirectSoundStream *s, XMEDIAPACKET const *packet, XMEDIAPACKET *out)
{
	(void)out;
	CHECK(fake(s)->alive, "a packet sent to a stream released");
	CHECK(fake(s)->packet_count < 256, "too many packets");
	CHECK(captured_bytes + packet->dwMaxSize <= sizeof(captured), "too many bytes sent");
	memcpy(captured + captured_bytes, packet->pvBuffer, packet->dwMaxSize);
	captured_bytes += packet->dwMaxSize;
	fake(s)->packet_bytes[fake(s)->packet_count] = packet->dwMaxSize;
	fake(s)->packet_contexts[fake(s)->packet_count++] = packet->pContext;
	fake(s)->packets++;
	return 0;
}
static void dsound_channel_set_I3DL2_properties(short index) { fake(channel_get(index)->stream)->i3dl2_given++; }
void dsound_sdl_stream_set_stereo_position(IDirectSoundStream *stream, BOOL positioned, const float *position,
	const float *velocity, float minimum_distance)
{
	(void)velocity; (void)minimum_distance;
	fake(stream)->stereo_positioned = positioned;
	memcpy(fake(stream)->stereo_position, position, sizeof(fake(stream)->stereo_position));
}

#include "under_test.inc"

/* the stream's oldest packet completed, as the mixer completes it (DirectSoundDoWork) */
static void complete_one(short index)
{
	IDirectSoundStream *stream = channel_get(index)->stream;
	struct fake_stream *record = fake(stream);
	int done = record->packet_count - record->packets;

	CHECK(record->packets > 0, "no packet to complete");
	record->packets--;
	dsound_channel_callback((void *)(long)record->context, record->packet_contexts[done], XMEDIAPACKET_STATUS_SUCCESS);
}

static struct sound_permutation adpcm_sound, pcm_sound;
/* the channels the desktop has (halo_port_capacity.h), each of a type: mono 2D, mono 3D, stereo 22 kHz, stereo 44 kHz */
static short const desktop_actual[NUMBER_OF_SOUND_CHANNEL_TYPES] = DESKTOP_ACTUAL_CHANNELS;
static short const type_flags[NUMBER_OF_SOUND_CHANNEL_TYPES] = SOUND_CHANNEL_TYPE_FLAGS;

static void setup(void)
{
	short type, index, channel = 0;

	for (type = 0; type < NUMBER_OF_SOUND_CHANNEL_TYPES; type++)
	{
		for (index = 0; index < desktop_actual[type]; index++, channel++)
		{
			struct sound_channel *record = channel_get(channel);

			record->type_flags = type_flags[type];
			CHECK(dsound_channel_create_stream(channel, 0, &record->stream) >= 0, "no stream");
			dsound_port_formats.pcm_rates[channel] = 0;
		}
	}
	dsound_globals.actual_channel_count = channel;
	CHECK(alive_streams == channel, "%d streams for %d channels", alive_streams, channel);
	adpcm_sound.compression = 1;
	adpcm_sound.samples.size = 36 * 64 * 10;
	pcm_sound.compression = 0;
	pcm_sound.sample_buffer_size = 44100;
	pcm_sound.samples.size = 2 * 44100;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	setup();
	if (!strcmp(name, "every-channel"))
	{
		/* every channel switches to PCM and back, twice: the streams never come to more than 256, every channel has
		one of the format it plays, and switching back takes the one it kept */
		short channel, round;

		for (round = 0; round < 2; round++)
		{
			for (channel = 0; channel < dsound_globals.actual_channel_count; channel++)
			{
				CHECK(dsound_channel_use_format(channel, 44100), "channel %d did not switch", channel);
				CHECK(fake(channel_get(channel)->stream)->pcm && fake(channel_get(channel)->stream)->alive,
					"channel %d's stream is not 16-bit PCM", channel);
				CHECK(fake(channel_get(channel)->stream)->context == channel, "the stream's context is not its channel");
			}
			for (channel = 0; channel < dsound_globals.actual_channel_count; channel++)
			{
				int made = fake_stream_count;
				int had_one = dsound_port_formats.kept[channel].stream != NULL;

				CHECK(dsound_channel_use_format(channel, 0), "channel %d did not switch back", channel);
				CHECK(!fake(channel_get(channel)->stream)->pcm && fake(channel_get(channel)->stream)->alive,
					"channel %d's stream is not Xbox ADPCM", channel);
				if (had_one)
					CHECK(fake_stream_count == made, "channel %d made a stream with one kept", channel);
			}
		}
		CHECK(most_alive_streams <= MAXIMUM_SOUND_CHANNELS, "%d streams at once, past the game's %d",
			most_alive_streams, MAXIMUM_SOUND_CHANNELS);
		CHECK(alive_streams == dsound_globals.actual_channel_count + dsound_port_formats.kept_count,
			"%d streams for %d channels and %d kept", alive_streams, dsound_globals.actual_channel_count,
			dsound_port_formats.kept_count);
	}
	else if (!strcmp(name, "settings"))
	{
		/* a 3D channel (mono 3D: the first after the mono 2D ones) and a stereo one, as they last set them: the
		stream switched in is given them, its frequency at the permutation's rate times the channel's pitch */
		short channel_3d = desktop_actual[0];
		short channel_stereo = desktop_actual[0] + desktop_actual[1];
		struct sound_channel *record = channel_get(channel_3d);
		struct fake_stream *stream;

		record->gain = 0.5f;
		record->pitch = 0.6f;
		record->spatialized = TRUE;
		record->position.x = 1.f; record->position.y = 2.f; record->position.z = 3.f;
		record->translational_velocity.i = 4.f;
		record->minimum_distance = 5.f;
		record->maximum_distance = 50.f;
		record->cone_outside_gain = 1.f;
		CHECK(dsound_channel_use_format(channel_3d, 44100), "no switch");
		stream = fake(record->stream);
		CHECK(stream->pcm && stream->has_3d && stream->channels == 1, "the 3D channel's stream is not mono 3D PCM");
		CHECK(stream->frequency == 26460, "its frequency is %lu, not 44100 * 0.6", stream->frequency);
		CHECK(stream->volume == dsound_volume_from_gain(0.5f, 0), "its volume is %ld", stream->volume);
		CHECK(stream->mode == DS3DMODE_NORMAL && stream->position[0] == 1.f && stream->position[1] == 3.f &&
			stream->position[2] == 2.f && stream->velocity[0] == 4.f, "its place is not the channel's");
		CHECK(stream->minimum_distance == 5.f && stream->maximum_distance == 50.f, "its distances are not the channel's");
		CHECK(stream->i3dl2_given > 0, "its I3DL2 source was not given");

		record = channel_get(channel_stereo);
		record->gain = 1.f;
		record->pitch = 1.f;
		dsound_port_formats.stereo_positions[channel_stereo].given = TRUE;
		dsound_port_formats.stereo_positions[channel_stereo].positioned = TRUE;
		dsound_port_formats.stereo_positions[channel_stereo].position[0] = -0.5f;
		CHECK(dsound_channel_use_format(channel_stereo, 48000), "no switch");
		stream = fake(record->stream);
		CHECK(stream->pcm && stream->channels == 2 && stream->frequency == 48000, "the stereo channel's stream");
		CHECK(stream->stereo_positioned && stream->stereo_position[0] == -0.5f, "its stereo position was not given");

		/* back to Xbox ADPCM: the stream kept, given the frequency of the channel's type again */
		CHECK(dsound_channel_use_format(channel_stereo, 0), "no switch back");
		CHECK(fake(record->stream)->frequency == 22050, "the Xbox ADPCM stream's frequency is %lu",
			fake(record->stream)->frequency);
	}
	else if (!strcmp(name, "queued"))
	{
		/* an Xbox ADPCM permutation with a 16-bit PCM one queued after it, on a mono 2D channel: the PCM one's
		packets go to a PCM stream, once the ADPCM one's have all played, never to the ADPCM stream */
		struct sound_channel *record = channel_get(0);
		IDirectSoundStream *adpcm_stream = record->stream;
		int rounds;

		adpcm_sound.unknown1 = (unsigned long)(void *)sound_cache;
		pcm_sound.unknown1 = (unsigned long)(void *)(sound_cache + 4096);
		record->pitch = 1.f;
		dsound_channel_queue_sound(0, &adpcm_sound);
		dsound_channel_queue_sound(0, &pcm_sound);
		CHECK(record->stream == adpcm_stream, "the stream switched before the Xbox ADPCM sound played");
		for (rounds = 0; rounds < 40 && record->stream == adpcm_stream; rounds++)
		{
			complete_one(0);
		}
		CHECK(record->stream != adpcm_stream && fake(record->stream)->pcm, "the channel did not switch to PCM");
		CHECK(fake(adpcm_stream)->packets == 0, "the switch came before the Xbox ADPCM sound's packets played");
		for (rounds = 0; rounds < fake(adpcm_stream)->packet_count; rounds++)
		{
			CHECK(fake(adpcm_stream)->packet_contexts[rounds] == &adpcm_sound, "the PCM sound went to the ADPCM stream");
		}
		CHECK(fake(record->stream)->packet_count > 0 && fake(record->stream)->packet_contexts[0] == &pcm_sound,
			"the PCM sound's packets are not on the PCM stream");
		CHECK(fake(record->stream)->frequency == 44100, "the PCM stream's frequency is %lu",
			fake(record->stream)->frequency);
		CHECK(fake(record->stream)->packet_bytes[0] % 2 == 0, "a PCM packet of %lu bytes",
			fake(record->stream)->packet_bytes[0]);
	}
	else if (!strcmp(name, "chained"))
	{
		/* a permutation split into three linked pieces of 16-bit PCM at 44.1 kHz (mod_sounds.c's range_split), each
		queued while the one before plays, as the sound manager queues them: the channel's one PCM stream takes
		every piece's packets, no switch and no stream more, the stream never without a packet until the last has
		played, and the bytes it is given are the pieces' joined, none left out or repeated (each piece longer than
		the packets queued ahead, as a split one's 512 KB is) */
		static struct sound_permutation pieces[3];
		static long const sizes[3] = { 100000, 100000, 60002 };
		struct sound_channel *record = channel_get(0);
		IDirectSoundStream *pcm_stream;
		long at = 0, byte_index;
		int piece, queued = 1, rounds, streams;

		for (piece = 0; piece < 3; piece++)
		{
			pieces[piece] = pcm_sound;
			pieces[piece].samples.size = sizes[piece];
			pieces[piece].unknown1 = (unsigned long)(void *)(sound_cache + at);
			at += sizes[piece];
		}
		for (byte_index = 0; byte_index < at; byte_index++)
			sound_cache[byte_index] = (byte)(byte_index * 7 + byte_index / 251);
		record->pitch = 1.f;
		captured_bytes = 0;
		dsound_channel_queue_sound(0, &pieces[0]);
		pcm_stream = record->stream;
		streams = alive_streams;
		CHECK(fake(pcm_stream)->pcm && fake(pcm_stream)->rate == 44100, "the channel is not playing PCM at 44.1 kHz");
		dsound_channel_queue_sound(0, &pieces[1]);
		for (rounds = 0; rounds < 200 && fake(pcm_stream)->packets > 0; rounds++)
		{
			complete_one(0);
			CHECK(!dsound_port_formats.switch_pending[0] && record->stream == pcm_stream,
				"the stream switched between pieces of one format");
			if (!record->queued_permutation && queued < 2 && record->playing_permutation == &pieces[queued])
			{
				queued++;
				dsound_channel_queue_sound(0, &pieces[queued]);
			}
			CHECK(fake(pcm_stream)->packets > 0 || (queued == 2 && !record->playing_permutation),
				"the stream ran out of packets at a join (piece %d)", queued);
		}
		CHECK(queued == 2 && fake(pcm_stream)->packets == 0 && record->state == _sound_channel_idle,
			"the chain did not play out");
		CHECK(alive_streams == streams && alive_streams <= MAXIMUM_SOUND_CHANNELS, "%d streams, %d before",
			alive_streams, streams);
		CHECK(captured_bytes == (unsigned long)at, "%lu bytes sent of the pieces' %ld", captured_bytes, at);
		for (byte_index = 0; byte_index < at && captured[byte_index] == sound_cache[byte_index]; byte_index++)
			;
		CHECK(byte_index == at, "byte %ld sent is not the pieces' (a gap or a repeat at a join)", byte_index);
		CHECK(locks == 0, "%d of the pieces' sound cache locks were not let go", locks);
	}
	else
	{
		fprintf(stderr, "no case '%s'\n", name);
		return 2;
	}
	return 0;
}
