/*
NEAR_CLIP.H

The near clip plane of the local players' views at a wide field of view
(near_clip.c).
*/

#ifndef NEAR_CLIP_H
#define NEAR_CLIP_H

struct render_camera;

/* the near clip distance a local player's view is drawn at (world units),
from the camera (its field of view, viewport and near distance, the game's)
and its frustum's bounds (render_camera_build_frustum_bounds'): the game's
own, or nearer where the view is so wide that the game's near plane would
reach farther than NEAR_CLIP_REACH from the eye at its corners */
real render_near_clip_distance(struct render_camera const *camera, real_rectangle2d const *frustum_bounds);

#endif
