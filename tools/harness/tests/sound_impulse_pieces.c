/* The real sound manager playing an impulse sound through the linked pieces a long permutation of 16-bit PCM is
split into (mod_sounds.c's range_split, OpenH1 issue 1167): each next piece is queued while the one before plays,
from the sound cache, and the chain ends where the permutation's did; the Xbox's own linked permutations (Xbox
ADPCM) still play only the first, as the Xbox played an impulse the_sound. Only the cache and the channels are fake. */
#include "harness.h"
#include <float.h>
#include "types.inc"
#define SET_FLAG(v,b,s) ((v)=(s)?((v)|FLAG(b)):((v)&~FLAG(b)))
#define TAG_BLOCK_GET_ELEMENT(b,i,t) (&((t *)(b)->address)[i])
#define VALID_INDEX(i,c) ((i)>=0&&(i)<(c))
enum {PIECES=4};
static struct sound_datum the_sound;
static struct sound_channel_datum the_channel;
static struct sound_definition the_definition;
static struct sound_pitch_range range;
static struct sound_permutation permutations[PIECES];
static struct sound_class_definition sound_class;
static struct sound_manager_globals sound_manager_globals;
static struct sound_permutation *queued_to_hardware[16];
static int hardware_queues, cache_misses, cache_requests;
static void channel_update(short i) { (void)i; }
static struct sound_platform_definition platform = {.channel_update=channel_update};
#define sound_get(i) (&the_sound)
#define channel_get(i) (&the_channel)
#define sound_definition_get(...) (&the_definition)
#define sound_class_get(...) (&sound_class)
#define sound_scale_value(a,...) (a)
#define sound_definition_get_minimum_distance(...) 0.f
#define sound_manager_master_gain(...) 1.f
#define sound_cache_sound_loaded(...) TRUE
#define channel_set_properties_hardware(...) ((void)0)
/* the sound cache: a piece is there unless the case says it is still loading */
static boolean _sound_cache_sound_request(struct sound_permutation *permutation, boolean block, boolean load,
 boolean reference)
{
 (void)permutation; (void)block; (void)load; (void)reference; cache_requests++;
 if (cache_misses) { cache_misses--; return FALSE; }
 return TRUE;
}
/* sound_manager.c's channel_queue_sound, its hardware's side recorded */
static void channel_queue_sound(short channel_index, struct sound_permutation *permutation)
{
 (void)channel_index;
 CHECK(hardware_queues<16,"too many queued");
 queued_to_hardware[hardware_queues++]=permutation;
 if (the_channel.playing_permutation) the_channel.queued_permutation=permutation;
 else the_channel.playing_permutation=permutation;
}
#include "sound_pcm_rate.inc"
#include "under_test.inc"

/* the hardware moved on to the queued piece (channel_get_state, after the playing one's last packet) */
static void transition(void)
{
 CHECK(the_channel.queued_permutation,"nothing queued to move on to");
 the_channel.playing_permutation=the_channel.queued_permutation; the_channel.queued_permutation=NULL;
}

/* one impulse sound on channel 0: its permutation 0 split into pieces 0 -> 2 -> 3 (1, another permutation picked
from, between them), PCM at 44.1 kHz, or Xbox ADPCM (the Xbox's own linked sound) */
static void setup(boolean pcm, boolean linked)
{
 int index;
 memset(&the_sound,0,sizeof(the_sound)); memset(&the_channel,0,sizeof(the_channel)); memset(&the_definition,0,sizeof(the_definition));
 memset(&range,0,sizeof(range)); memset(permutations,0,sizeof(permutations));
 SET_FLAG(the_definition.flags,_sound_definition_linked_permutations_bit,linked);
 the_definition.pitch_ranges.address=&range; the_definition.pitch_ranges.count=1; the_definition.gain_modifier=1;
 range.permutations.address=permutations; range.permutations.count=PIECES; range.actual_permutation_count=2;
 range.playback_rate=range.natural_pitch=1;
 for (index=0; index<PIECES; index++)
 {
  permutations[index].gain=1; permutations[index].next_permutation_index=NONE;
  permutations[index].compression=pcm?0:1; permutations[index].sample_buffer_size=pcm?44100:0;
  permutations[index].samples.size=0x80000;
 }
 permutations[0].next_permutation_index=2; permutations[2].next_permutation_index=3;
 the_sound.type=_sound_impulse; the_sound.playing_channel_index=NONE; the_sound.pitch=1; the_sound.source.gain=1;
 the_sound.permutation_index=0; the_sound.pitch_range_index=0;
 the_channel.sound_index=0;
 sound_manager_globals.platform_definition=&platform;
 hardware_queues=cache_misses=cache_requests=0;
}

int main(int argc,char **argv) {
 CHECK(argc==2,"case name"); const char *case_name=argv[1];
 CASE("pieces") {
  /* started: piece 0, and piece 2 queued after it at once; then 3 after 2; then nothing (the chain's end) */
  setup(TRUE,TRUE);
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==2&&queued_to_hardware[0]==&permutations[0]&&queued_to_hardware[1]==&permutations[2],
   "%d queued: the first and its next piece were not",hardware_queues);
  CHECK(the_sound.permutation_index==2,"the sound is at permutation %d",the_sound.permutation_index);
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==2,"a piece queued twice");
  transition(); update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==3&&queued_to_hardware[2]==&permutations[3],"the last piece was not queued");
  transition(); update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==3&&!the_channel.queued_permutation,"something queued past the chain's end");
  return 0;
 }
 CASE("cache-miss") {
  /* the next piece still loading: asked for again on the next update, then queued */
  setup(TRUE,TRUE);
  cache_misses=1;
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==1&&the_sound.permutation_index==0,"a piece not in the cache was queued");
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==2&&queued_to_hardware[1]==&permutations[2],"the piece was not queued once loaded");
  return 0;
 }
 CASE("stock-adpcm") {
  /* the Xbox's own linked sound, Xbox ADPCM: an impulse sound plays its first permutation only, as before */
  setup(FALSE,TRUE);
  update_channel_for_impulse_sound(0,1.f);
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==1&&queued_to_hardware[0]==&permutations[0]&&!cache_requests,
   "an Xbox ADPCM chain was followed (%d queued)",hardware_queues);
  return 0;
 }
 CASE("unlinked") {
  /* a sound whose permutations are not linked (as its file says, kept so: mod_sounds.c) plays a split one's pieces
  all the same, the next past its actual ones */
  setup(TRUE,FALSE);
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==2&&queued_to_hardware[1]==&permutations[2],"an unlinked sound's pieces were not followed");
  return 0;
 }
 CASE("unlinked-actual") {
  /* nor is one of its actual permutations, which follows nothing in a sound not linked */
  setup(TRUE,FALSE);
  permutations[0].next_permutation_index=1;
  update_channel_for_impulse_sound(0,1.f);
  CHECK(hardware_queues==1,"an unlinked sound's actual permutation was queued after another");
  return 0;
 }
 CHECK(FALSE,"unknown case");
}
