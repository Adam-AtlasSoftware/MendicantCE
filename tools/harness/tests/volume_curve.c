/* The real settings_volume_amplitude and settings_volume_percent
(menu_functions.c): Settings' volume sliders, a percentage evenly loud along
the track, over the amplitude config.toml keeps (#1106). */
#include "harness.h"
#include <math.h>
#include "under_test.inc"
static double decibels(double amplitude) { return 20.0 * log10(amplitude); }
int main(int argc,char **argv) {
 const char *case_name;
 int step;
 CHECK(argc==2,"case name"); case_name=argv[1];
 /* every value the old spinners saved (0 to 1, the master's to 2, in
 tenths) shows at a percentage that writes it back */
 CASE("old-values-round-trip") {
  for(step=0;step<=20;step++) {
   double amplitude=step/10.0, percent=settings_volume_percent(amplitude);
   CHECK(fabs(settings_volume_amplitude(percent)-amplitude)<1e-9,"%g came back as %g",amplitude,
    settings_volume_amplitude(percent));
   CHECK(percent>=0.0 && percent<=(step<=10 ? 100.0 : 110.1),"%g at %g%%",amplitude,percent);
  }
  return 0;
 }
 /* 0% an exact mute, 100% the Xbox's level, 110% the master's 6 dB more;
 evenly loud: each 10% above the knee 6 dB, the first 1% far below 20 dB
 under full, rising with no step back */
 CASE("curve") {
  double previous=-1.0;
  CHECK(settings_volume_amplitude(0.0)==0.0 && settings_volume_percent(0.0)==0.0,"0 is a mute");
  CHECK(fabs(settings_volume_amplitude(100.0)-1.0)<1e-12,"100%% is %g",settings_volume_amplitude(100.0));
  CHECK(fabs(decibels(settings_volume_amplitude(110.0))-6.0)<0.1,"110%% is %g dB",
   decibels(settings_volume_amplitude(110.0)));
  CHECK(fabs(decibels(settings_volume_amplitude(50.0))+30.0)<0.01,"50%% is %g dB",
   decibels(settings_volume_amplitude(50.0)));
  CHECK(decibels(settings_volume_amplitude(1.0))<-60.0,"1%% is %g dB",decibels(settings_volume_amplitude(1.0)));
  for(step=0;step<=1100;step++) {
   double amplitude=settings_volume_amplitude(step/10.0);
   CHECK(amplitude>previous,"%g%% is no louder than the step before",step/10.0);
   previous=amplitude;
  }
  return 0;
 }
 CHECK(FALSE,"unknown case");
 return 1;
}
