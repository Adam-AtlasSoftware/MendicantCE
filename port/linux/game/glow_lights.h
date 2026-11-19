/*
GLOW_LIGHTS.H

PRISM Path Traced's light from what glows (glow_lights.c).
*/

#ifndef GLOW_LIGHTS_H
#define GLOW_LIGHTS_H

/* a glowing object's light: where, its colour (CE's self-illumination, as
drawn) and the object's size (its bounding sphere's radius) */
struct glow_lights_light
{
	long id; /* the object's datum handle (a strike's: its projectile's) */
	boolean strike; /* a projectile's strike's flash (its reach is size) */
	float position[3];
	float color[3];
	float size;
};

/* Path Traced: CE's lights that light nothing in CE (no dynamic flag: a
lens flare's) are drawn too (object_lights.c) */
boolean glow_lights_all(void);
/* a lens flare's light's share by its owner's object type (NONE: an effect's) */
real glow_lights_share(short object_type);
/* display.prism_glow_lights (0 to 4) */
real glow_lights_scale(void);
/* debug.glow_inventory */
boolean glow_lights_inventory_on(void);
/* a light found for the scene (object_lights.c's lights_preprocess_scene):
a projectile's kept (its strike's flash), and each logged once with its
owner under debug.glow_inventory */
void glow_lights_seen(long definition_index, long object_index, boolean dynamic, boolean drawn, real radius,
	real_rgb_color const *color, real_point3d const *position);
/* each frame (SCENE_LIGHTS'): the glowing objects found about the camera */
void glow_lights_frame(real_point3d const *camera);
long glow_lights_count(void);
struct glow_lights_light const *glow_lights_get(void);
void glow_lights_map_unloaded(void);

#endif
