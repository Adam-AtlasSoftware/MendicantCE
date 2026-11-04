/*
NEAR_CLIP.C

The near clip plane of the local players' views at a wide field of view
(display.fov, display.vehicle_fov). The game draws the world clipped at a
plane 0.0625 world units ahead of the eye (rasterizer_near_clip_distance,
rasterizer.c's rasterizer_global_defaults), a plane across the view, not a
sphere about the eye: a point at an angle a off the view's axis is clipped
nearer than 0.0625 / cos a. At the stock view the near plane's corners are
0.085 units from the eye (16:9), and nothing the player stands against is
cut. At a wide one the sides are nearly beside the eye: a wall the player
stands against is 0.2 units (the cyborg's collision radius) from it, and
the plane cuts it wherever it is more than atan(0.2 / 0.0625), 72.6
degrees, off the axis, so from about 145 degrees across 16:9 a wall
beside the player is drawn cut along a line near the screen's side, the
world behind it showing (#604). The Panini projection (panini.c) redraws
the rectilinear view as it was drawn, cut walls and all.

So the plane is brought nearer as the view widens, to where its farthest
point, a corner of the view, is NEAR_CLIP_REACH from the eye: then nothing
NEAR_CLIP_REACH or more from the eye is clipped, at any angle the view
draws. The corners are the view's as the game's frustum takes them
(render_camera_build_frustum): its vertical field of view, its viewport's
shape and its bounds (split screen's windows are off their middles, so a
corner of one is farther out than the other side's), which the culling
(render_cameras.c's frustum, structure_culling.c) and the projection share.
The lowered reticle's view is turned up, not skewed (lowered_reticle.c),
and the Panini pass's supersampling draws the same frustum at more pixels.
The plane is never moved farther than the game's: the stock view, and any
whose corners are within NEAR_CLIP_REACH, are drawn exactly as before.

The depth buffer (Vulkan's and Metal's reversed 32-bit floats: far_clip.c)
keeps its precision as a share of the distance whatever the near distance
is. The first-person weapon has a near plane of its own (rasterizer_first_
person_weapon_near_clip_distance, 0.0117), which it keeps. It is drawing
alone: nothing a tick reads sees the render camera.
*/

#include "cseries.h"
#include "render/render_cameras.h"
#include "near_clip.h"

#include <math.h>

/* how far from the eye the near plane may reach at the view's corners
(world units): the cyborg's collision radius, as near as the world comes
to the eye beside it (a wall the player stands against) */
#define NEAR_CLIP_REACH 0.2f

real render_near_clip_distance(struct render_camera const *camera, real_rectangle2d const *frustum_bounds)
{
	real viewport_width = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0);
	real viewport_height = (real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real tangent = (real)tan(camera->vertical_field_of_view * 0.5f);
	real across, up, corner, reached;

	/* (the half tangents at the view's farther side and its farther top or
	bottom: render_camera_build_frustum's planes, its bounds as across the
	viewport's height) */
	across = MAX(fabsf(frustum_bounds->x0), fabsf(frustum_bounds->x1)) * viewport_width / viewport_height * tangent;
	up = MAX(fabsf(frustum_bounds->y0), fabsf(frustum_bounds->y1)) * tangent;
	/* (a corner at depth 1 is this far from the eye) */
	corner = (real)sqrt(1.0 + (double)across * across + (double)up * up);
	reached = NEAR_CLIP_REACH / corner;
	return reached < camera->z_near ? reached : camera->z_near;
}
