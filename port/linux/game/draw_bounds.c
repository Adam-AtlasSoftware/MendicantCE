/*
DRAW_BOUNDS.C

The part of a view that a pass over the level can change.

A dynamic light and an object's shadow are drawn onto the level as the
level's own surfaces, whole: every surface the light's sphere or the
shadow's volume touches (structure_render.c). A surface of the level is
often many times the size of the light or the shadow on it, and what the
pass draws beyond them changes nothing: the light's distance attenuation
and the shadow's map are sampled by their border there, which adds no light
and takes none away. Those pixels are still shaded and blended, and in a
fight, where every plasma bolt is a light and every body casts a shadow,
they were a quarter of a Raspberry Pi 5's frame.

So the pass says what it is bounded by (the light's reach as a cube, the
shadow's volume as a box) and its draws are scissored to the box's place in
the view (halo_draw_bounds, port/linux/src/d3d8_device.c): the pixels drawn
are the same ones, only fewer are shaded. A box that reaches behind the
eye bounds nothing, and its pass is drawn whole as before.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "draw_bounds.h"
#include "rasterizer/rasterizer.h"
#include "render/render_cameras.h"

/* ---------- constants */

enum
{
	NUMBER_OF_BOX_CORNERS = 8
};

enum
{
	/* (the rasterizer's files' own, each: rasterizer_xbox.c) */
	_rasterizer_target_render_primary = 0
};

/* how near the eye's plane a corner of a box may be for its place in the
view to bound anything (the projection's w, world units) */
#define DRAW_BOUNDS_NEAREST_CORNER 0.001f

/* ---------- public code */

void draw_bounds_box(
	real_point3d const *centre,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real const low[3],
	real const high[3])
{
	real const width = (real)(global_window_parameters.camera.viewport_bounds.x1 -
		global_window_parameters.camera.viewport_bounds.x0);
	real const height = (real)(global_window_parameters.camera.viewport_bounds.y1 -
		global_window_parameters.camera.viewport_bounds.y0);
	float rectangle[4];
	short corner;

	/* (the window's view of the world alone, not a mirror's: the others
	are drawn through projections of their own) */
	if (global_window_parameters.rasterizer_target != _rasterizer_target_render_primary ||
		global_window_parameters.camera.mirrored)
	{
		halo_draw_bounds(NULL);
		return;
	}

	rectangle[0] = width;
	rectangle[1] = height;
	rectangle[2] = 0.0f;
	rectangle[3] = 0.0f;
	for (corner = 0; corner < NUMBER_OF_BOX_CORNERS; corner++)
	{
		real const along_forward = (corner & 1) ? high[0] : low[0];
		real const along_left = (corner & 2) ? high[1] : low[1];
		real const along_up = (corner & 4) ? high[2] : low[2];
		real_point3d point;
		real_point3d view_point;
		real w;
		real x;
		real y;

		point.x = centre->x + forward->i*along_forward + left->i*along_left + up->i*along_up;
		point.y = centre->y + forward->j*along_forward + left->j*along_left + up->j*along_up;
		point.z = centre->z + forward->k*along_forward + left->k*along_left + up->k*along_up;
		matrix4x3_transform_point(
			&global_window_parameters.frustum.world_to_view,
			&point,
			&view_point);
		w =
			global_window_parameters.frustum.projection_matrix[0][3] * view_point.x +
			global_window_parameters.frustum.projection_matrix[1][3] * view_point.y +
			global_window_parameters.frustum.projection_matrix[2][3] * view_point.z +
			global_window_parameters.frustum.projection_matrix[3][3];
		if (!(w > DRAW_BOUNDS_NEAREST_CORNER))
		{
			halo_draw_bounds(NULL);
			return;
		}

		/* (the viewport's pixels from its left and top, by their edges; as
		rasterizer_project_billboard has a point's, by their middles) */
		x = ((
			global_window_parameters.frustum.projection_matrix[0][0] * view_point.x +
			global_window_parameters.frustum.projection_matrix[1][0] * view_point.y +
			global_window_parameters.frustum.projection_matrix[2][0] * view_point.z +
			global_window_parameters.frustum.projection_matrix[3][0]) / w + 1.0f) * width * 0.5f;
		y = (1.0f - (
			global_window_parameters.frustum.projection_matrix[0][1] * view_point.x +
			global_window_parameters.frustum.projection_matrix[1][1] * view_point.y +
			global_window_parameters.frustum.projection_matrix[2][1] * view_point.z +
			global_window_parameters.frustum.projection_matrix[3][1]) / w) * height * 0.5f;
		rectangle[0] = MIN(rectangle[0], x);
		rectangle[1] = MIN(rectangle[1], y);
		rectangle[2] = MAX(rectangle[2], x);
		rectangle[3] = MAX(rectangle[3], y);
	}

	/* (the part of it in the viewport and a pixel around it, which the
	front end's own placing of the pixels may still bring in: none, if the
	right is not past the left or the bottom past the top) */
	rectangle[0] = MAX(rectangle[0], -1.0f);
	rectangle[1] = MAX(rectangle[1], -1.0f);
	rectangle[2] = MIN(rectangle[2], width + 1.0f);
	rectangle[3] = MIN(rectangle[3], height + 1.0f);
	halo_draw_bounds(rectangle);

	return;
}

void draw_bounds_cube(
	real_point3d const *centre,
	real half_width)
{
	real low[3];
	real high[3];

	low[0] = low[1] = low[2] = -half_width;
	high[0] = high[1] = high[2] = half_width;
	draw_bounds_box(centre, global_forward3d, global_left3d, global_up3d, low, high);

	return;
}

void draw_bounds_none(
	void)
{
	halo_draw_bounds(NULL);

	return;
}
