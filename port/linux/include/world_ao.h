/*
WORLD_AO.H

display.ambient_occlusion "world" (docs/world-ao.md, #578): ambient
occlusion found in world space, from the structure's baked distance field
(world_sdf.h) and the objects' analytic proxies (world_proxies.h), applied
to the ambient term alone through #574's slot in the shaders
(xgpu.h's XGPU_AMBIENT_OCCLUSION_LIGHT_*). Render-only and deterministic: no
noise, no history, nothing from the screen. Off (and "off", "subtle",
"strong", and Classic's preset, which sets "off") draws exactly as before.

The renderer's side of a frame (this file's game hooks, called from
source/render/render.c), and the front end's (halo_world_ao_*,
port/linux/src/d3d8_device.c, which hands it to the backend:
xgpu_backend_world_ao_*). Workstream D owns the shading and this file's
setting; A, B and C fill what it hands on.
*/

#ifndef WORLD_AO_H
#define WORLD_AO_H

/* ---------- constants */

/* display.ambient_occlusion's world mode's parts (world_ao_mode) */
enum
{
	WORLD_AO_PROXIES = 1, /* the objects' proxies */
	WORLD_AO_FIELD = 2, /* the structure's distance field (on objects alone) */
};

/* ---------- prototypes (port/linux/game/world_ao.c) */

/* display.ambient_occlusion is "world": WORLD_AO_* parts on; 0 off (any
other value, the VR mode's eye passes until they are done, the Classic
preset) */
int world_ao_mode(void);

/* the map loaded (scenario_load: its scene found, its fields looked for in
the cache; a missing one baked in the background, never here), a BSP
switched to (scenario_switch_structure_bsp), the map unloaded
(scenario_unload: everything kept of it let go, the bake abandoned). Off,
nothing but the unload, which frees what an earlier "world" kept */
void world_ao_map_loaded(void);
void world_ao_bsp_switched(short bsp_index);
void world_ao_map_unloaded(void);

/* each frame (render_frame): the scene's objects found again
(world_scene_frame_begin), a field's upload continued (world_sdf_frame) */
void world_ao_frame_begin(void);
/* a window's 3D view (render_window): its proxies and the field's place
handed to the renderer before its opaque surfaces draw, and taken away
after the lightmap pass (the later passes never read it) */
void world_ao_window_begin(short local_player_index, short rasterizer_target, float const camera_position[3]);
void world_ao_window_end(void);
/* which object is drawn now (render_objects: its own proxies do not shade
it; -1 none, the structure) */
void world_ao_object_begin(long object_index);

/* ---------- the front end's (port/linux/src/d3d8_device.c) */

/* the window's world occlusion: the field's place and the parameters
(xgpu_stream.h's struct xgpu_world_ao_block: field, parameters) and the
proxies (three vec4s each: world_proxies.h's struct world_proxy). count < 0:
none (the window draws as without it, from here on) */
void halo_world_ao_window(float const field[2][4], float const parameters[2][4], float const (*proxies)[3][4],
	int count);
/* the object drawn next: its slot among the window's proxies' owners (-1:
none, the structure) and its reference, the field's occlusion at its
lighting point, which its ambient sample already holds (1: none) */
void halo_world_ao_object(int owner, float reference);
/* the field's voxels uploaded, slices z_first to z_first + z_count - 1 of
a width * height * depth volume (signed bytes: world_sdf.h); width 0
releases it */
void halo_world_ao_field(unsigned long width, unsigned long height, unsigned long depth, unsigned long z_first,
	unsigned long z_count, signed char const *voxels);

#endif
