/*
PRISM_DEFERRED.H

PRISM v4's deferred core, its first step: display.lighting "ce_deferred"
(prism_deferred.c).
*/

#ifndef __PRISM_DEFERRED_H
#define __PRISM_DEFERRED_H

enum
{
	/* the diffuse texture pass and the decals over it */
	_prism_deferred_base,
	/* the specular and reflection passes */
	_prism_deferred_additions,
	/* the frame put together from them, before the transparent geometry */
	_prism_deferred_resolve
};

/* display.lighting "ce_deferred" (read once a frame at most) */
boolean prism_deferred_wanted(void);
/* the window's draws from now on to a phase's target (render_window) */
void prism_deferred_phase(short phase);
/* the G-buffer pass (render_window's walk 1, render_gbuffer_pass): begun
(the map's materials sent first, once) and ended */
void prism_deferred_gbuffer_begin(void);
void prism_deferred_gbuffer_end(void);
/* in the G-buffer pass: the object drawn from now on (NONE the level's:
world_ao.c's hooks), the first-person weapon's draws begun or ended, and a
draw's shader (rasterizer_xbox_models.c's and _environment.c's draws), whose
material the draws that follow take */
void prism_deferred_object(long object_index);
void prism_deferred_first_person(int drawing);
void prism_deferred_shader(void const *shader);

#endif
