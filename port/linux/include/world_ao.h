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

/* a draw's owner (halo_world_ao_object), past the window's proxies' owner
slots (0 up): none (the structure, an object with no proxies: every proxy
shades it), and drawn without the occlusion (the first-person weapon) */
enum
{
	WORLD_AO_OWNER_NONE = -1,
	WORLD_AO_OWNER_UNLIT = -2,
};

/* the shading's constants (docs/world-ao.md, "Shading"; the emitters' are
xgpu.h's XGPU_WORLD_AO_*, which port/linux/src/d3d8_device.c checks are
these): the cone's gain, its first step's least (world units) and taps; the
least reference divided by; the share of a proxy's reach it fades out
over; the least the occlusion leaves; the strengths on objects and on the
level ("world" is the strong preset); the reference's weight */
#define WORLD_AO_K 1.6f
#define WORLD_AO_STEP 0.15f
#define WORLD_AO_TAPS 5
#define WORLD_AO_REFERENCE_LEAST 0.25f
#define WORLD_AO_REACH_FADE 0.25f
#define WORLD_AO_FLOOR 0.2f
#define WORLD_AO_STRENGTH_OBJECTS 1.0f
#define WORLD_AO_STRENGTH_LEVEL 0.8f
#define WORLD_AO_REFERENCE_WEIGHT 1.0f

struct world_proxy;
struct world_sdf_volume;

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
it; -1 none, the structure); the first-person weapon drawn (TRUE) and done
(FALSE: none drawn now), which the occlusion leaves as it is */
void world_ao_object_begin(long object_index);
void world_ao_first_person(int drawing);

/* ---------- the arithmetic (the shaders' reference: xgpu.h's
XGPU_AMBIENT_OCCLUSION_WORLD, the emitters' world_ao_*; tools/harness/
tests/world_ao.c) */

/* A_sdf: the share of the ambient light the field leaves a point with this
normal (1 none: no volume) */
float world_ao_field_occlusion(struct world_sdf_volume const *volume, float const point[3], float const normal[3]);
/* an object's reference: A_sdf where its ambient light was sampled, above
its lighting point, facing up */
float world_ao_reference(struct world_sdf_volume const *volume, float const lighting_point[3]);
/* A_prox: the share the proxies (their owner slots in v[1][3]) of other
owners than owner leave */
float world_ao_proxies_occlusion(struct world_proxy const *proxies, long count, float const point[3],
	float const normal[3], float owner);
/* the occlusion a draw's pixel multiplies its ambient light by: a model's
(lightmap FALSE: the field relative to its reference, faded in, times the
proxies) or the lightmap pass's (the proxies alone), the parameters as the
block's (xgpu_stream.h's struct xgpu_world_ao_block) */
float world_ao_occlusion(float const parameters[2][4], struct world_sdf_volume const *volume,
	struct world_proxy const *proxies, long count, float const point[3], float const normal[3], float owner,
	float reference, int lightmap);

/* ---------- the front end's (port/linux/src/d3d8_device.c) */

/* the window's world occlusion: the field's place and the parameters
(xgpu_stream.h's struct xgpu_world_ao_block: field, parameters) and the
proxies (three vec4s each: world_proxies.h's struct world_proxy). count < 0:
none (the window draws as without it, from here on) */
void halo_world_ao_window(float const field[2][4], float const parameters[2][4], float const (*proxies)[3][4],
	int count);
/* the object drawn next: its slot among the window's proxies' owners
(WORLD_AO_OWNER_NONE: none, the structure; WORLD_AO_OWNER_UNLIT: drawn
without the occlusion) and its reference, the field's occlusion at its
lighting point, which its ambient sample already holds (1: none) */
void halo_world_ao_object(int owner, float reference);
/* a part of the field uploaded (WORLD_AO_FIELD_*: world_sdf.h's table of
cells, 4 bytes each, or its atlas of bricks' voxels, signed bytes), slices
z_first to z_first + z_count - 1 of a width * height * depth volume; width
0 releases the field, both parts. The field is drawn with once both have
had their last slices */
enum
{
	WORLD_AO_FIELD_TABLE = 0,
	WORLD_AO_FIELD_ATLAS = 1,
};
void halo_world_ao_field(int part, unsigned long width, unsigned long height, unsigned long depth,
	unsigned long z_first, unsigned long z_count, void const *data);

#endif
