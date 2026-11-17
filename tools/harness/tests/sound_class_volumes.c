/* The real sound_manager_port_volume, with the game's own sound class table:
which of config.toml's volumes each class plays at (#1107). */
#include "harness.h"
#include "types.inc"
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
static unsigned long changes;
static double music = 0.2, dialogue = 0.9, effects = 0.5;
static double config_real(const char *name) {
 if(!strcmp(name,"audio.music_volume")) return music;
 if(!strcmp(name,"audio.dialogue_volume")) return dialogue;
 if(!strcmp(name,"audio.effects_volume")) return effects;
 CHECK(FALSE,"unknown setting %s",name);
 return 0;
}
static unsigned long config_changes(void) { return changes; }
#define sound_class_get(i) (&sound_classes[i])
#include "classes.inc"
#include "under_test.inc"
static short class_named(const char *name) {
 short i;
 for(i=0;i<NUMBER_OF_SOUND_CLASSES;i++) if(!strcmp(sound_class_names[i],name)) return i;
 CHECK(FALSE,"no class %s",name);
 return NONE;
}
static void expect(const char *name, double volume) {
 real gain=sound_manager_port_volume(class_named(name));
 CHECK(gain==(real)volume,"%s plays at %g, not %g",name,gain,volume);
}
int main(int argc,char **argv) {
 const char *case_name;
 CHECK(argc==2,"case name"); case_name=argv[1];
 /* the units' lines and the scripts' dialogue at the dialogue volume; the
 scripts' effects, weapons and engines at the effects volume, music at its own */
 CASE("classes") {
  expect("unit_dialog",0.9); expect("scripted_dialog_player",0.9); expect("scripted_dialog_other",0.9);
  expect("scripted_dialog_force_unspatialized",0.9);
  expect("scripted_effect",0.5); expect("weapon_fire",0.5); expect("vehicle_engine",0.5);
  expect("music",0.2);
  return 0;
 }
 /* read again when Settings changes it */
 CASE("changed") {
  expect("unit_dialog",0.9);
  dialogue=0.3; changes++;
  expect("unit_dialog",0.3); expect("weapon_fire",0.5);
  return 0;
 }
 CHECK(FALSE,"unknown case");
 return 1;
}
