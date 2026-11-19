/*
PRISM_TERRAIN.H

The terrain, weather and water overhaul's game half (prism_terrain.c;
docs/prism_terrain.md). The hooks in source/ call these; each returns at
once unless the overhaul is wanted (prism_terrain_wanted), and none writes
the game's state or draws a random number: they read their arguments and
keep what the renderer is told in this file's own ring. Includers have
cseries.h's real types.
*/

#ifndef __PRISM_TERRAIN_H
#define __PRISM_TERRAIN_H

/* display.terrain "on", display.lighting "raster" or "pathtraced", a
campaign (no game engine runs) being played (read once a frame at most) */
boolean prism_terrain_wanted(void);
/* each frame, before the windows (world_ao_frame_begin): PRISM_TERRAIN's
frame, then the events and waders since the last */
void prism_terrain_frame(void);

/* bipeds.c's biped_make_footstep: a foot's contact marker (its CE material
effect's index: walk, run, sliding, shuffle, jump, jump's landing; the
contact point 0 the left foot, 1 the right) */
void prism_terrain_footstep(long biped_index, short event_index, short contact_point_index,
	real_matrix4x3 const *marker);
/* material_effects.c's material_effect_new_from_point: the footstep's ray's
hit (the ground under the foot just sent, its normal and CE's material
type) */
void prism_terrain_footstep_ground(real_point3d const *point, real_vector3d const *normal, long material_type);
/* vehicles.c's create_slipping_effects: a mass point moving on the ground
(its contact, the ground's normal, its velocity over the ground, the
ground's material type, CE's speed scale) */
void prism_terrain_contact(long vehicle_index, real_point3d const *position, real_vector3d const *normal,
	real_vector3d const *velocity, short material_type, real scale);
/* projectiles.c: a projectile's impact on a surface (projectile_collision:
the hit's point, normal, material type, the velocity) and its detonation
(projectile_detonate: where, its last hit's material type) */
void prism_terrain_impact(long projectile_index, real_point3d const *point, real_vector3d const *normal,
	short material_type, real_vector3d const *velocity);
void prism_terrain_detonation(long projectile_index, real_point3d const *point, short material_type);
/* damage.c's area_of_effect_cause_damage: an explosion's centre and its
damage's cutoff radius */
void prism_terrain_explosion(real_point3d const *centre, real radius);
/* render.c: TRUE where the renderer draws the weather (display.weather
"prism") in CE's place; FALSE (CE draws it) until T12 */
boolean prism_terrain_weather_draw(void);
/* rasterizer_xbox_water.c (T7): TRUE where the renderer draws the water
(display.water "prism", the backend with ray tracing) in CE's colour draw's
place; then a group's steps (PRISM_TERRAIN_WATER_BEGIN, _CAPTURED, _END)
about its draws, each with its shader's flags (bits 0 to 3, and
PRISM_TERRAIN_WATER_BASE and _SKY: xgpu_stream.h's XGPU_TERRAIN_WATER_*), its
plane, CE's ripple scale, its tints seen straight on and edge on (rgb,
alpha) and the window's planar fog (colour, maximum density; maximum depth
and distance, the eye's height over its plane) */
enum
{
	PRISM_TERRAIN_WATER_BEGIN,
	PRISM_TERRAIN_WATER_CAPTURED,
	PRISM_TERRAIN_WATER_END,
	PRISM_TERRAIN_WATER_BASE = 0x100,
	PRISM_TERRAIN_WATER_SKY = 0x200,
};
boolean prism_terrain_water_wanted(void);
void prism_terrain_water_step(unsigned long step, unsigned long flags, real_plane3d const *plane, real ripple_scale,
	float const perpendicular[4], float const parallel[4], float const fog[8]);

#endif
