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
	_prism_deferred_resolve,
	/* (in the base phase: the diffuse texture pass drawn, the decals next;
	PRISM v4's materials' base colour put over it) */
	_prism_deferred_base_textured
};

/* display.lighting "ce_deferred", "raster" or "pathtraced" (read once a frame at
most) */
boolean prism_deferred_wanted(void);
/* display.lighting "raster" or "pathtraced" (PRISM Raster or Path Traced:
the deferred core lit from the bake, port/linux/game/prism_bake.c; Path
Traced's scene, rig and bake are Raster's) */
boolean prism_deferred_raster(void);
/* display.lighting "pathtraced" (PRISM Path Traced, M3: every CE light off,
the path tracer's light, xgpu_vulkan_pt.c) */
boolean prism_deferred_pathtraced(void);
/* display.lighting "raster" with display.prism_raster_composition "materials" */
boolean prism_deferred_raster_materials(void);
/* port: whether an object is one the bake's static scenery holds
(prism_bake.c's scenery_static: a placement of scenery, a control or a light
fixture, where it was placed), which the path tracer's moving objects leave
out (xgpu_stream.h's XGPU_SCENE_INSTANCE_IN_TLAS) */
boolean prism_bake_object_static(long object_index);
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
/* the player's own body in first person, traced as a shadow caster in the
traced modes' G-buffer pass (render_objects.c), and the mode of its draws
(1 traced alone, 2 its legs drawn alone, 0 neither) */
boolean prism_deferred_body_traced(long object_index);
void prism_deferred_body(int mode);
/* port: display.player_shadow off under display.lighting "raster" or
"pathtraced": no shadow of the player's own body at all (its legs drawn and
not traced, no CE or cascaded shadow of it) */
boolean prism_deferred_player_shadowless(void);
void prism_deferred_shader(void const *shader);
/* ... and the lightmap page the level's draws that follow are lit from (its
bitmap of the BSP's lightmap bitmaps; NULL none), which their G-buffer
twins write with their lightmap coordinates
(rasterizer_xbox_environment.c's lightmap pass's begin) */
void prism_deferred_lightmap(void const *lightmap_bitmap);
/* (v4 geopatch M4) the draws that follow (a remodelled material's, until the
next material) lit from the enhanced world's own lightmap page own_page
(prism_world_geometry_own_pages: the BSP's lightmap bitmap count +
own_page), or, NONE, from the lightmap pass's own page again */
void prism_deferred_page_override(long own_page);
/* a shader's material id in the map's table (0 none; the table made
where it is not: SCENE_STATIC's, prism_scene_static.c) */
unsigned long prism_deferred_material_id(void const *shader);

#endif
