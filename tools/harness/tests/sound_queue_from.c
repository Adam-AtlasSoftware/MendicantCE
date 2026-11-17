/* A sound queued from a byte into its samples (sound_dsound_xbox.c's
dsound_port_queue_sound_from, for coop_sound_sync.c): an idle channel starts
there, at its block's start; a channel that plays already queues it as it
always has; and the byte is that one call's, never the next sound's. */
#include "harness.h"
#include "types.inc"

static struct dsound_globals dsound_globals;
static struct sound_permutation sounds[2];
static long filled_from[4];
static int fills,mapped;
#define channel_get(i) (&dsound_globals.channels[i])
#define IDirectSound_CommitDeferredSettings(...) 0
#define dsound_error(...) CHECK(FALSE,"DirectSound error")
#define match_vassert(...) ((void)0)
/* (a virtual channel maps to the actual channel of its number, or to none) */
static short dsound_virtual_touch(short virtual_channel_index){return mapped?virtual_channel_index:NONE;}
/* the first packet is queued from where the channel starts */
static void dsound_channel_fill(short index)
{
 CHECK(fills<4,"filled too often");
 filled_from[fills++]=dsound_globals.channels[index].sample_offset;
}
/* the channel's stream made ready for the sound's format (dsound_channel_use_format) */
static long formats_used[4];
static int formats;
static boolean dsound_channel_use_format(short index,long pcm_rate)
{
 CHECK(formats<4,"formats set too often");
 formats_used[formats++]=pcm_rate;
 dsound_port_formats.pcm_rates[index]=pcm_rate;
 return TRUE;
}
#include "under_test.inc"

static void setup(void)
{
 memset(&dsound_globals,0,sizeof(dsound_globals));memset(sounds,0,sizeof(sounds));
 dsound_globals.actual_channel_count=2;
 sounds[0].samples.size=36*1000;sounds[1].samples.size=36*1000;
 /* channel 0 Xbox ADPCM stereo, channel 1 uncompressed */
 dsound_globals.channels[0].type_flags=(short)(FLAG(_sound_channel_compressed_bit)|FLAG(_sound_channel_stereo_bit));
 dsound_globals.channels[1].type_flags=0;
 fills=0;mapped=TRUE;formats=0;memset(&dsound_port_formats,0,sizeof(dsound_port_formats));
}

int main(int argc,char **argv)
{
 char const *name=argc>1?argv[1]:"";
 setup();
 if(!strcmp(name,"from"))
 {
  /* a stereo ADPCM block is 72 bytes: byte 7310 is in the one from 7272 (a mono one's from 7308) */
  dsound_port_queue_sound_from(0,&sounds[0],7310);
  CHECK(fills==1&&filled_from[0]==7272,"an idle channel started from byte %ld",filled_from[0]);
  CHECK(dsound_globals.channels[0].state==_sound_channel_playing,"the channel is not playing");
 }
 else if(!strcmp(name,"pcm"))
 {
  /* a permutation kept as 16-bit PCM at 44.1 kHz: the stereo channel's stream switched to it, and started on a
  frame of it (4 bytes), not a block of Xbox ADPCM */
  sounds[0].compression=0;sounds[0].sample_buffer_size=44100;
  dsound_port_queue_sound_from(0,&sounds[0],7310);
  CHECK(formats==1&&formats_used[0]==44100,"the channel's stream was not made 16-bit PCM at 44.1 kHz");
  CHECK(fills==1&&filled_from[0]==7308,"a PCM sound started from byte %ld",filled_from[0]);
 }
 else if(!strcmp(name,"uncompressed"))
 {
  /* an uncompressed channel's sample is 4 bytes */
  dsound_port_queue_sound_from(1,&sounds[1],4002);
  CHECK(fills==1&&filled_from[0]==4000,"an uncompressed channel started from byte %ld",filled_from[0]);
 }
 else if(!strcmp(name,"next-sound"))
 {
  /* the next sound, queued as sounds are, starts at its beginning */
  dsound_port_queue_sound_from(0,&sounds[0],7300);
  dsound_globals.channels[1].state=_sound_channel_idle;
  dsound_virtual_queue_sound(1,&sounds[1]);
  CHECK(fills==2&&filled_from[1]==0,"the next sound started from byte %ld",filled_from[1]);
 }
 else if(!strcmp(name,"no-channel"))
 {
  /* a virtual channel with no actual one queues nothing, and the byte is not kept for the next */
  mapped=FALSE;
  dsound_port_queue_sound_from(0,&sounds[0],7300);
  CHECK(fills==0,"a virtual channel with no channel played");
  mapped=TRUE;
  dsound_virtual_queue_sound(0,&sounds[0]);
  CHECK(fills==1&&filled_from[0]==0,"the byte was kept for the next sound: from %ld",filled_from[0]);
 }
 else if(!strcmp(name,"playing"))
 {
  /* a channel that plays queues the sound to follow, from its beginning as always */
  dsound_globals.channels[0].state=_sound_channel_playing;
  dsound_port_queue_sound_from(0,&sounds[1],7300);
  CHECK(fills==0&&dsound_globals.channels[0].queued_permutation==&sounds[1]&&
   dsound_globals.channels[0].state==_sound_channel_queued,"a playing channel did not queue it");
  dsound_globals.channels[0].state=_sound_channel_idle;
  dsound_virtual_queue_sound(0,&sounds[0]);
  CHECK(fills==1&&filled_from[0]==0,"the byte was kept for the next sound: from %ld",filled_from[0]);
 }
 else if(!strcmp(name,"outside"))
 {
  /* a byte outside the sound starts it at its beginning */
  dsound_port_queue_sound_from(0,&sounds[0],36*1000);
  CHECK(fills==1&&filled_from[0]==0,"a byte past the sound started from %ld",filled_from[0]);
  dsound_globals.channels[1].state=_sound_channel_idle;
  dsound_port_queue_sound_from(1,&sounds[1],-8);
  CHECK(fills==2&&filled_from[1]==0,"a byte before the sound started from %ld",filled_from[1]);
 }
 else
 {
  fprintf(stderr,"unknown case %s\n",name);
  return 2;
 }
 return 0;
}
