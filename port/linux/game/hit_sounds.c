/* The hit sounds (audio.hit_sounds, each player's own, off by default): a
short beep as this machine's player damages another player, as Halo PC
played one, if that player's setting is on. Only damage
that was dealt sounds, as the machine that decides it dealt it: the host's
(and a game of this machine's own) as object_cause_damage deals it
(network_damage_aftermath), a client's as the host's damage of the hit comes
(network_damage_handle_events, the damage events the host sends the machine
of the damage's owner), not as the client shows its own hits at once. So a
hit the host refuses never sounds, nor a player's damage of themselves, nor
of a teammate the gametype's friendly fire spares (it deals nothing), nor of
anything that is not a player's unit (vehicles, the campaign's actors).

The host may forbid them for its players (network.allow_hit_sounds false):
network_standard_hit_sounds_allowed, which an OpenH1 game's record carries
to its clients.

The sound is Halo PC's own (OpenH1 issue 673) where this machine has
Custom Edition's sounds.map (its maps extracted to custom_maps, or its
install): sound\sfx\impulse\ting\ting, the last of a Custom Edition map's
multiplayer sounds (globals\globals), at the pitch the tag gives it, 0.6
(custom_edition_sounds_resource_sound), read the first time one plays.
Elsewhere it is a beep made for this build (hit_sound_pcm.inc, made by
tools/hit_sound_asset.py), which the Xbox's maps have nothing like. Either
plays through the game's DirectSound buffers
(port/linux/src/dsound_sdl.c), dry, in the front pair, under
audio.effects_volume and audio.volume, at HIT_SOUND_VOLUME. Hits closer
together than HIT_SOUND_SPACING_MILLISECONDS (a shotgun's pellets, an
explosion's players) sound once, and each beep has a buffer of its own, so
that one does not cut the last short. */
#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/damage.h"
#include "bots.h"
#include "hit_sounds.h"
#include "network_standard.h"
#include "player_settings.h"

#include <xtl.h>

#include "hit_sound_pcm.inc"

/* the platform layer's (port/linux/src/port_config.c) */
unsigned long system_milliseconds(void);
/* (custom_edition_sounds.c) */
short *custom_edition_sounds_resource_sound(char const *name, long *frame_count, long *rate, real *pitch);
/* (network_test.c: a line of debug.network_test's log) */
void network_test_note_hit_sound(long attacker_player_index, long victim_player_index);

/* its volume (millibels: about half), and the least time between beeps */
#define HIT_SOUND_VOLUME (-600)
#define HIT_SOUND_SPACING_MILLISECONDS 50
/* (a beep is 65 ms: two at most play at once) */
#define HIT_SOUND_BUFFERS 3
/* Halo PC's hit sound, in Custom Edition's sounds.map */
#define HIT_SOUND_CUSTOM_EDITION_NAME "sound\\sfx\\impulse\\ting\\ting"

static struct
{
	boolean made;
	boolean failed;
	IDirectSoundBuffer *buffers[HIT_SOUND_BUFFERS];
	short next;
	unsigned long played_at;
	boolean played;
	/* audio.hit_sounds, each local player's own (player_settings.h's
	slots), read again as the settings change */
	boolean read[PLAYER_SETTINGS_SLOTS];
	unsigned long read_at[PLAYER_SETTINGS_SLOTS];
	boolean on[PLAYER_SETTINGS_SLOTS];
} hit_sounds = { FALSE, FALSE, { NULL }, 0, 0, FALSE, { FALSE }, { 0 }, { FALSE } };

/* the buffers, made once, with the sound in them: Halo PC's, played at
its tag's pitch by its rate (as the channels pitch a sound), or the beep */
static boolean hit_sounds_make(
	void)
{
	WAVEFORMATEX format;
	DSBUFFERDESC description;
	short index;
	long frame_count = 0;
	long rate = 0;
	real pitch = 1.f;
	short *custom_edition = NULL;
	void const *samples = hit_sound_pcm;
	unsigned long bytes = sizeof(hit_sound_pcm);
	unsigned long samples_per_second = HIT_SOUND_RATE;

	if (hit_sounds.made || hit_sounds.failed)
		return hit_sounds.made;
	custom_edition = custom_edition_sounds_resource_sound(HIT_SOUND_CUSTOM_EDITION_NAME, &frame_count, &rate, &pitch);
	if (custom_edition && frame_count > 0)
	{
		samples = custom_edition;
		bytes = (unsigned long)frame_count * sizeof(short);
		samples_per_second = (unsigned long)(rate * pitch + 0.5f);
	}
	csmemset(&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = samples_per_second;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 2;
	format.nAvgBytesPerSec = samples_per_second * 2;
	csmemset(&description, 0, sizeof(description));
	description.dwSize = sizeof(description);
	description.dwFlags = DSBCAPS_LOCDEFER;
	description.lpwfxFormat = &format;
	for (index = 0; index < HIT_SOUND_BUFFERS; index++)
	{
		if (DirectSoundCreateBuffer(&description, &hit_sounds.buffers[index]) < 0)
		{
			hit_sounds.failed = TRUE;
			break;
		}
		/* (the buffer takes a copy) */
		IDirectSoundBuffer_SetBufferData(hit_sounds.buffers[index], (void *)samples, bytes);
	}
	if (custom_edition)
		free(custom_edition);
	hit_sounds.made = !hit_sounds.failed;
	return hit_sounds.made;
}

static void hit_sounds_play(
	void)
{
	IDirectSoundBuffer *buffer;
	unsigned long now = system_milliseconds();

	if (hit_sounds.played && now - hit_sounds.played_at < HIT_SOUND_SPACING_MILLISECONDS)
		return;
	if (!hit_sounds_make())
		return;
	hit_sounds.played = TRUE;
	hit_sounds.played_at = now;
	buffer = hit_sounds.buffers[hit_sounds.next];
	hit_sounds.next = (short)((hit_sounds.next + 1) % HIT_SOUND_BUFFERS);
	/* (the volume set again: audio.effects_volume as it is now) */
	IDirectSoundBuffer_SetVolume(buffer, HIT_SOUND_VOLUME);
	IDirectSoundBuffer_SetCurrentPosition(buffer, 0);
	IDirectSoundBuffer_Play(buffer, 0, 0, 0);
}

void hit_sounds_note_damage(
	long attacker_player_index,
	long victim_player_index,
	float shield_damage,
	float body_damage,
	unsigned long being_damaged_flags)
{
	struct player_datum *attacker;
	short slot;

	if (attacker_player_index == NONE || victim_player_index == NONE ||
		attacker_player_index == victim_player_index)
	{
		return;
	}
	/* (damage dealt: some of the shield or the body, or a kill outright) */
	if (!(shield_damage > 0.f || body_damage > 0.f ||
		TEST_FLAG(being_damaged_flags, _object_being_damaged_body_depleted_bit)))
	{
		return;
	}
	attacker = player_try_and_get(attacker_player_index);
	if (!attacker || attacker->local_player_index == NONE || bots_player_is_bot(attacker_player_index) ||
		!player_try_and_get(victim_player_index))
	{
		return;
	}
	/* (the attacker's own setting: a split screen's players each have theirs) */
	slot = PLAYER_SETTINGS_SLOT(attacker->local_player_index);
	if (!hit_sounds.read[slot] || hit_sounds.read_at[slot] != config_changes())
	{
		hit_sounds.read[slot] = TRUE;
		hit_sounds.read_at[slot] = config_changes();
		hit_sounds.on[slot] = config_player_boolean(attacker->local_player_index, "audio.hit_sounds") != 0;
	}
	if (!hit_sounds.on[slot] || !network_standard_hit_sounds_allowed())
		return;
	network_test_note_hit_sound(attacker_player_index, victim_player_index);
	hit_sounds_play();
}
