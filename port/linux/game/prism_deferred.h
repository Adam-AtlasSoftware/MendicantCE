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

#endif
