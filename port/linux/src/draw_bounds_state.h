/*
DRAW_BOUNDS_STATE.H
The draws' bounds as the front end keeps them (d3d8_device.c's "the draws'
bounds"): the rectangle of the viewport the game last said a pass can
change (halo_draw_bounds, from port/linux/game/draw_bounds.c), which the
draws are scissored to until the game says otherwise. A pass that ends
without saying so (a light's or a shadow's cut short) must not clip what
comes after it, so the bounds also end with the frame
(draw_bounds_state_frame_end, from D3DDevice_Present). Apart from
d3d8_device.c so that port/linux/tests/draw_bounds_test.c builds it alone.
*/
#ifndef DRAW_BOUNDS_STATE_H
#define DRAW_BOUNDS_STATE_H

#include <string.h>

struct draw_bounds_state
{
	int known;
	/* (left, top, right, bottom: the viewport's pixels, in the game's units) */
	float rectangle[4];
};

/* the game's bounds for the draws from now on; NULL: none (drawn whole) */
static inline void draw_bounds_state_set(struct draw_bounds_state *state, const float *rectangle)
{
	state->known = rectangle != NULL;
	if (rectangle)
		memcpy(state->rectangle, rectangle, sizeof(state->rectangle));
}

/* the frame presented: the next frame's draws begin unbounded */
static inline void draw_bounds_state_frame_end(struct draw_bounds_state *state)
{
	state->known = 0;
}

#endif
