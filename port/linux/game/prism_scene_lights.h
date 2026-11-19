/*
PRISM_SCENE_LIGHTS.H

PRISM v4's SCENE_LIGHTS: the frame's light table (prism_scene_lights.c).
*/

#ifndef __PRISM_SCENE_LIGHTS_H
#define __PRISM_SCENE_LIGHTS_H

/* each frame, before its windows and after world_scene's lights are
listed (world_ao_frame_begin) */
void prism_scene_lights_frame(void);
/* display.lighting "pathtraced" (and Raster's "materials" composition): after a window's lights are placed
(lights_preprocess_scene), the table sent again with them as placed */
void prism_scene_lights_window(void);
/* the map's tags are about to go */
void prism_scene_lights_map_unloaded(void);

#endif
