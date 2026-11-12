/*
PRISM_POPIN.H

What PRISM does against pop-in on the game's side (prism_popin.c): the
textures a draw waits for (display.prism_texture_wait), the levels of
detail's hysteresis and blend band (display.prism_lod_hysteresis,
display.prism_world_geometry_band), and the counts of what switched.
*/

#ifndef PRISM_POPIN_H
#define PRISM_POPIN_H

/* the frame's start (main_game_render): the settings read again where they
changed, the frame's waiting time given back, the counts logged now and
then */
void prism_popin_frame(void);

/* display.prism_texture_wait: whether a texture a draw needs that is still
being read may be waited for now, the frame's milliseconds of waiting not
spent (the clock started at the first); and whether that time is not spent
yet (each look of the wait's loop) */
int prism_popin_texture_wait_begin(void);
int prism_popin_texture_waiting(void);
/* a draw's texture waited for and in, or not in (its draw left out, as the
Xbox did) */
void prism_popin_texture_waited(int loaded);
void prism_popin_texture_skipped(void);

/* display.prism_lod_hysteresis, as a fraction (0 none) */
float prism_popin_hysteresis(void);
/* display.prism_world_geometry_band, as a fraction (0 none) */
float prism_popin_band(void);
/* a surface of the remodel switched between its near and far levels */
void prism_popin_world_switched(void);

/* the counts since the map's start (the camera path's log) */
void prism_popin_counts(unsigned long *skipped, unsigned long *waited, unsigned long *world_switches,
	double *waited_milliseconds);

#endif
