/*
DRAW_BOUNDS.H

The part of a view that a pass over the level can change (draw_bounds.c).
*/

#ifndef DRAW_BOUNDS_H
#define DRAW_BOUNDS_H

#include "math/real_math.h"

/* the draws from now on change no pixel of the view outside a box (world
units): its middle, its three axes (unit vectors), and how far it reaches
along each of them, from low to high */
void draw_bounds_box(
	real_point3d const *centre,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real const low[3],
	real const high[3]);

/* ... outside a cube about a point, the world's axes its own */
void draw_bounds_cube(
	real_point3d const *centre,
	real half_width);

/* the draws from now on are not bounded */
void draw_bounds_none(
	void);

#endif
