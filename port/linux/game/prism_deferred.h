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

/* display.lighting "ce_deferred" or "raster" (read once a frame at most) */
boolean prism_deferred_wanted(void);
/* display.lighting "raster" (PRISM Raster: the deferred core lit from the
bake, port/linux/game/prism_bake.c) */
boolean prism_deferred_raster(void);
/* port: ... with display.prism_raster_object_shadows "traced" (the moving
objects' sun shadows traced: prism_rt.c's RT_FRAME asks for their structure) */
boolean prism_deferred_raster_object_shadows(void);
/* the window's draws from now on to a phase's target (render_window) */
void prism_deferred_phase(short phase);
/* the G-buffer pass (render_window's walk 1, render_gbuffer_pass): begun
(the map's materials sent first, once) and ended */
void prism_deferred_gbuffer_begin(void);
/* ... and its window's camera (render_window's struct render_camera and
render_frustum): SCENE_VIEW, which the G-buffer's motion is found by */
void prism_deferred_gbuffer_view(void const *camera, void const *frustum);
void prism_deferred_gbuffer_end(void);
/* in the G-buffer pass: the object drawn from now on (NONE the level's:
world_ao.c's hooks), the first-person weapon's draws begun or ended, and a
draw's shader (rasterizer_xbox_models.c's and _environment.c's draws), whose
material the draws that follow take */
void prism_deferred_object(long object_index);
void prism_deferred_first_person(int drawing);
void prism_deferred_shader(void const *shader);
/* ... and the lightmap page the level's draws that follow are lit from (its
bitmap of the BSP's lightmap bitmaps; NULL none), which their G-buffer
twins write with their lightmap coordinates
(rasterizer_xbox_environment.c's lightmap pass's begin) */
void prism_deferred_lightmap(void const *lightmap_bitmap);
/* a shader's material id in the map's table (0 none; the table made
where it is not: SCENE_STATIC's, prism_scene_static.c) */
unsigned long prism_deferred_material_id(void const *shader);

#endif
