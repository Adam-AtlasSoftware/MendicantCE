/* The real settings_volume_amplitude, settings_volume_place,
settings_volume_shown and settings_volume_typed (menu_functions.c):
Settings' volume sliders, a place evenly loud along the track over the
amplitude config.toml keeps, shown as a percentage (#1106). */
#include "harness.h"
#include <math.h>
#include "under_test.inc"
static double decibels(double amplitude) { return 20.0 * log10(amplitude); }
int main(int argc,char **argv) {
 const char *case_name;
 int step;
 CHECK(argc==2,"case name"); case_name=argv[1];
 /* every value the old spinners saved (0 to 1, the master's to 2, in
 tenths) sits at a place that writes it back, inside the slider (the
 master's 0 to 110) */
 CASE("old-values-round-trip") {
  for(step=0;step<=20;step++) {
   double amplitude=step/10.0, place=settings_volume_place(amplitude);
   CHECK(fabs(settings_volume_amplitude(place)-amplitude)<1e-9,"%g came back as %g",amplitude,
    settings_volume_amplitude(place));
   CHECK(place>=0.0 && place<=(step<=10 ? 100.0 : 110.0)+1e-9,"%g at %g",amplitude,place);
  }
  return 0;
 }
 /* shown as the old steps read: past 100% the amplitude's percentage
 (1.5 is 150%, 2.0 the master's 200%), and a percentage typed is that
 place */
 CASE("shown") {
  CHECK(fabs(settings_volume_shown(110.0)-200.0)<1e-9,"110 shown as %g%%",settings_volume_shown(110.0));
  CHECK(fabs(settings_volume_shown(settings_volume_place(1.5))-150.0)<1e-9,"1.5 shown as %g%%",
   settings_volume_shown(settings_volume_place(1.5)));
  CHECK(settings_volume_shown(100.0)==100.0 && settings_volume_shown(42.0)==42.0,"to 100, the place itself");
  for(step=0;step<=200;step++)
   CHECK(fabs(settings_volume_shown(settings_volume_typed(step))-step)<1e-9,"%d%% typed shows %g%%",step,
    settings_volume_shown(settings_volume_typed(step)));
  return 0;
 }
 /* 0 an exact mute, 100 the Xbox's level, 110 the master's 6 dB more;
 evenly loud: 50 is -30 dB, 105 +3 dB, the first step far below 20 dB under
 full, rising with no step back */
 CASE("curve") {
  double previous=-1.0;
  CHECK(settings_volume_amplitude(0.0)==0.0 && settings_volume_place(0.0)==0.0,"0 is a mute");
  CHECK(fabs(settings_volume_amplitude(100.0)-1.0)<1e-12,"100 is %g",settings_volume_amplitude(100.0));
  CHECK(fabs(settings_volume_amplitude(110.0)-2.0)<1e-12,"110 is %g",settings_volume_amplitude(110.0));
  CHECK(fabs(decibels(settings_volume_amplitude(105.0))-3.0)<0.1,"105 is %g dB",
   decibels(settings_volume_amplitude(105.0)));
  CHECK(fabs(decibels(settings_volume_amplitude(50.0))+30.0)<0.01,"50 is %g dB",
   decibels(settings_volume_amplitude(50.0)));
  CHECK(decibels(settings_volume_amplitude(1.0))<-60.0,"1 is %g dB",decibels(settings_volume_amplitude(1.0)));
  for(step=0;step<=1100;step++) {
   double amplitude=settings_volume_amplitude(step/10.0);
   CHECK(amplitude>previous,"%g is no louder than the step before",step/10.0);
   previous=amplitude;
  }
  return 0;
 }
 CHECK(FALSE,"unknown case");
 return 1;
}
