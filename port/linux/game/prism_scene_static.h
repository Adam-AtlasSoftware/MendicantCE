/*
PRISM_SCENE_STATIC.H

PRISM v4's SCENE_STATIC: the level's geometry as drawn, once for every
consumer (prism_scene_static.c).
*/

#ifndef __PRISM_SCENE_STATIC_H
#define __PRISM_SCENE_STATIC_H

/* each frame, before its windows (world_ao_frame_begin): the set begun
again where the BSP, the remodel drawn or the traced kinds changed, and a
slice of it sent */
void prism_scene_static_frame(void);
/* each window's camera (prism_world_geometry_frame): the surfaces' levels
sent where they changed */
void prism_scene_static_levels(void);
/* the map's tags are about to go */
void prism_scene_static_map_unloaded(void);
/* the set being sent or whole: its generation (0 none), whether it is
whole (its _END sent) */
unsigned long prism_scene_static_generation(void);
int prism_scene_static_ended(void);
/* the set of the BSP drawn begun with nothing in it: the BSP has no
lightmapped surface (a cinematic's), so there is nothing to bake */
int prism_scene_static_empty(void);
/* port: the set is of the BSP the last one was of, begun again in play (the
world geometry, the lighting or the traced kinds changed): no bake is made
for it, the bake of its key loaded if there is one, else its own scene made,
nothing baked, as a load with no bake makes it: never the bake drawn before
it, another set's (xgpu_vulkan_bake.c) */
boolean prism_scene_static_in_play(void);

#endif
