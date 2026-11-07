/*
XGPU.H

What the renderer's parts share, in no GPU API's terms: the vertex
programs' and pixel shaders' keys and lighting (decoded by nv2a_vsh.c and
nv2a_psh.c), the enhanced dynamic lights' layout, the Xbox's texture
formats (xbox_textures.c) and generated source text. The front end
(d3d8_device.c) and every backend include it; the interface between front
end and backend is xgpu_backend.h.
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* where one of the game's model lighting programs (d3d8_device.c
halo_vertex_shader_lighting) has the normal, and the world position, that it
lights the diffuse color by: the temporary register that holds each before
the instruction given */
struct nv2a_vertex_lighting
{
	/* 1 by the ambient and distant lights, 2 by the point lights too */
	int lights;
	unsigned long normal_instruction, normal_register;
	/* (with the point lights only) */
	unsigned long position_instruction, position_register;
	/* the world position the program projects to the screen (the
	temporary its clip position is computed from, before the instruction
	given), for the enhanced dynamic lights (display.dynamic_lights): with
	the point lights, the same as theirs; world_found FALSE if none */
	BOOL world_found;
	unsigned long world_instruction, world_register;
	/* (the backends': lit for each vertex, the ambient and distant lights'
	sum and the point lights' handed on apart, for the pixel shader's
	ambient occlusion: XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX) */
	BOOL split;
};

/* the lights value of a structure's (the environment's) lightmap pass lit
by the enhanced dynamic lights: its vertices' world position, normal,
binormal and tangent (v0 to v3) handed on as they are */
#define XGPU_ENVIRONMENT_LIGHTING 3

struct xgpu_vertex_program;

/* finds them in a model lighting program (decoded: xgpu_shader.h), checking
that it lights its diffuse color as xgpu_spirv.c does for each pixel; FALSE if
it does not */
BOOL nv2a_vertex_shader_lighting(const struct xgpu_vertex_program *program, struct nv2a_vertex_lighting *lighting);

/* the views a draw goes to (a vertex program's multiview variant): with
XGPU_MULTIVIEW_EYES, both of the VR mode's eyes at once (multiview): the
clip position goes through each eye's correction (xgpu_spirv.c,
halo_vr_set_eye_transforms); with XGPU_MULTIVIEW_LEFT_EYE or
XGPU_MULTIVIEW_RIGHT_EYE, that eye's layer alone */
enum
{
	XGPU_MULTIVIEW_NONE = 0,
	XGPU_MULTIVIEW_EYES,
	/* the eye pass's visibility tests: queries in a multiview pass would
	count in as many query slots as views (Vulkan), which Zink does not
	allow for, and the GPU writes past them */
	XGPU_MULTIVIEW_LEFT_EYE,
	/* a draw into each eye's layer in turn, each with its own screen
	correction (the crosshairs: halo_vr_draw_eye) */
	XGPU_MULTIVIEW_RIGHT_EYE,
};
/* The VR mode's eye pass is foveated (vr.foveation, host_vulkan_vr.c): its targets
have four layers, two for each eye (layer 2 eye + 0, the whole eye at the
periphery's resolution; 2 eye + 1, the inset about the gaze at full
resolution, as large in pixels). XGPU_MULTIVIEW_EYES then draws all four
(eye_correction[VIEW >> 1], then fovea_view[VIEW] scales and moves the
inset's views), XGPU_MULTIVIEW_LEFT_EYE and XGPU_MULTIVIEW_RIGHT_EYE that
eye's two (its visibility tests are counted by the pixel shaders instead),
and a layered target is sampled in an inset's layer through
fovea_sample[layer] (its pixels from the whole eye's), from the whole eye's
layer where the inset does not have the point. Set before the first VR
shader is made. */
extern int xgpu_foveated;

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
	/* a 2-layer render target (the VR mode's eye pass; 4 foveated,
	xgpu_foveated): the view's layer */
	_xgpu_sampler_2d_layered,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (Android) */
	unsigned char count_samples;
	/* a high-res HUD meter (hud_hires.h) drawn with the meter's blend (the
	destination kept by the source's alpha): that alpha is eased to 1 by the
	coverage texture 0's green holds, so that the meter darkens what is
	behind it only where it covers it (the Xbox's point-sampled meters stop
	at their texels' edges; filtered ones have a fringe of faint texels) */
	unsigned char coverage_alpha;
	/* Discrete meter thresholds in texture 0's red are read at level zero,
	without filtering. Coverage/brightness still use the filtered lookup. */
	unsigned char point_threshold;
	/* a model lighting program's draw lit for each pixel
	(display.per_pixel_lighting): nv2a_vertex_lighting's lights, or 0 for
	the diffuse color the vertex shader computed */
	unsigned char per_pixel_lighting;
	/* drawn into a multisampled target (display.anti_aliasing's
	multisampling), its samples a pixel: the alpha test covers samples in
	proportion to how far alpha is past the reference, not all of the pixel
	or none of it, so that cut-out edges (foliage, grates) are smoothed too */
	unsigned char alpha_test_samples;
	/* drawn into both eyes at once (the VR mode, d3d8_device.c, xgpu_vulkan.c):
	layered render targets are sampled at the view's layer
	(XGPU_MULTIVIEW_*) */
	unsigned char multiview;
	/* the source's blend factor, applied here instead (1 the blend color, 2
	one less it, 3 its alpha, 4 one less that; 0 none), for a blend of a
	constant color factor with a constant alpha one, as the meters'
	(shader_transparent_meter) */
	unsigned char source_blend;
	/* the enhanced dynamic lights (display.dynamic_lights, d3d8_device.c,
	xgpu_spirv.c) added for each pixel: XGPU_DYNAMIC_LIGHTS_ENVIRONMENT to a
	structure's lightmap pass (into the light the textures are then
	multiplied by, the bump map in texture 0), XGPU_DYNAMIC_LIGHTS_MODEL to a
	model's lighting lit for each pixel in place of its two point lights; 0
	none */
	unsigned char dynamic_lights;
	/* display.ambient_occlusion's occlusion multiplies the draw's ambient
	light alone, not the light its lights add (XGPU_AMBIENT_OCCLUSION_LIGHT_*,
	below); 0 none */
	unsigned char ambient_occlusion;
	/* the VR mode's effects (d3d8_device.c halo_vr_effect_depth, the
	backend's xgpu_spirv.h XGPU_SPIRV_EFFECT_*): an effect drawn into both
	eyes soft against the scene's depth (1), and thick (2); 0 none */
	unsigned char vr_effect;
	/* display.hdr: the draw's tag into the HDR back buffer (xgpu_hdr.h:
	XGPU_HDR_*); 0 into any other target, and without HDR */
	unsigned short hdr;
	/* display.prism_material_overlay: a model draw lit for each pixel whose
	shader a mod's overlay.json names (port/linux/game/mod_materials.c),
	lit with a micro normal, cavity and roughness from two tiles mixed by
	its multipurpose map (texture 2): 0 none, else 1 plus twice
	display.prism_overlay_view, plus XGPU_OVERLAY_CLASS_MASK with four tiles
	weighted by a class mask and the multipurpose map's rule on top, plus
	XGPU_OVERLAY_CLASS_MASK_WINS where the mask's own tiles A, C and D are
	kept over the rule, plus XGPU_OVERLAY_SURFACE with a surface map,
	plus XGPU_OVERLAY_SURFACE_ONLY with no tiles (with a surface map, or
	without one for PRISM v2's light alone: prism_lighting) (below: "The
	material overlay") */
	unsigned char material_overlay;
	/* display.prism_lightmaps: a structure's lightmap pass whose stage 2 is
	its lightmap (a 2D texture at projected coordinates) reads it smoothed
	(XGPU_LIGHTMAP_FILTER_*, below: "The lightmaps smoothed"); 0 as the
	Xbox read it */
	unsigned char lightmap_filter;
	/* PRISM v2's model light (display.prism_specular, prism_reflections,
	prism_shadows), on a draw with a material overlay alone (its uniform
	and images carry its parameters): XGPU_PRISM_* (below: "PRISM v2's
	model light"); 0 none, as the game draws */
	unsigned char prism_lighting;
};

/* ---------- the lightmaps smoothed (display.prism_lightmaps)

A lightmap pass's draw with lightmap_filter XGPU_LIGHTMAP_FILTER_SMOOTH
reads texture stage 2 (the lightmap page, R5G6B5 as the game has it, or
the finer copy rebuilt on this machine that replaces it: prism_lightmaps.c)
in place of its one bilinear sample with:

  c = the cubic B-spline of the 4 by 4 texels about the point, as four
      bilinear samples (the weights' sums at each pair's place);
  m, M = the least and most of each channel over the 2 by 2 texels the
      bilinear sample reads (three gathers), c clamped to [m, M]: the
      B-spline's wider reach never pulls light across a chart's edge or a
      shadow's into the cell (no bleed of a gutter's black);
  d = the mean of the four bilinear samples XGPU_LIGHTMAP_DEBAND_REACH
      texels away along the diagonals, each weighed by how close it is to c
      (1 within a quantum of the 16-bit format, R and B 1/31, G 1/63, 0 past
      XGPU_LIGHTMAP_DEBAND_LIMIT of them): where the light is even (a band's
      step of one quantum) c moves toward d by the weights' mean; where it
      changes (an edge, a shadow) it stays c;
  t2 = c + (n - 0.5) / 255, n interleaved gradient noise of the pixel
       (Jimenez's), so that the steps left in the 8-bit target do not band.

Its alpha is the bilinear sample's, as the combiners read it. Nothing else
changes: the bump's response, the self-illumination, the dynamic lights. */
#define XGPU_LIGHTMAP_FILTER_SMOOTH 1
#define XGPU_LIGHTMAP_DEBAND_REACH 1.5f
#define XGPU_LIGHTMAP_DEBAND_LIMIT 2.0f

/* ---------- the material overlay (display.prism_material_overlay)

A model draw lit for each pixel (per_pixel_lighting, its world position
handed on: the point lights' or the world's transform) takes a micro
normal, a cavity and a roughness from up to four tiles of a mod's (RG the
normal's slopes along u and v, 0.5 none; B the cavity, half of it; A the
roughness, 0.5 none), repeated across texture 0's coordinates at their
scale and weighted by its multipurpose map's channel (texture 2, in the
Xbox's channel order) or a class mask. It changes the light alone: the
normal model_lighting lights by (the vertices' normal bent along the
screen-space derivatives' cotangent frame of the world position and the
coordinates, Schueler's: the vertices need nothing new), its ambient and
distant lights' sum by the cavity and raised by half the roughness, clamped
to 0..1; and texture stage 3's reflection cube, its direction bent from the
vertices' normal to the micro normal and its level of detail raised by
four times the roughness, darkened by the cavity. The tiles fade with
their texels' size on the screen (their footprint, the tiles across a
pixel, log2) and with the draw's distance (the uniform's fade, the CPU's).
The tiles are images (xgpu_images.h), repeating and filtered trilinearly,
at XGPU_OVERLAY_BINDING (Vulkan: bindings 4 to 10 of the textures' set),
tiles A to D then the class mask, then the surface map's two images (below); the uniform (vec4
overlay[XGPU_OVERLAY_UNIFORM_COUNT]): [0] the multipurpose channel that
mixes the two tiles (one-hot, Xbox order: specular, self-illumination,
change color, auxiliary); [1] lo, hi, invert, has multipurpose; [2] the
excluded channel (one-hot); [3] above which it is excluded, the draw's
fade, the footprint fade's start and end (tiles a pixel, log2); [4], [5]
tile A's and B's scale (tiles per UV unit), normal, cavity and roughness
strengths; [6] reflection bend, cavity on reflection, tile B's footprint
fade's start and end ([3]'s are tile A's); with a class mask also [7], [8]
tile C's and D's as [4] and [5], and [9] C's footprint fade's start and end,
then D's. The class mask (RGBA: the weights of tiles A to D at the base
map's coordinates) gives the weights; where the multipurpose channel's rule
is (has multipurpose, and a channel in [0]), it gives tile A, and the
mask's B, C and D share the rest (B alone where they are 0); with
XGPU_OVERLAY_CLASS_MASK_WINS the mask's A, C and D are kept where the mask
gives them and the rule decides between tiles A and B over the mask's B
alone (a texel the mask names plate stays plate where the multipurpose
channel dips, at a scratch the base map paints; its C and D, a buckle's
metal, are not taken by the rule's A). The debug
views (display.prism_overlay_view, the key's bits 1 to 3): 1 the tiles'
weights, 2 the micro normal's change, 3 the fade, 4 the light alone.

The surface map (display.prism_surface_maps, PRISM v2; off unless set):
two BC5 images at the base map's coordinates, read once (not tiled), the
images after the class mask (port/linux/game/mod_materials.c makes them
from one RGBA PNG): the first's RG the tangent-space normal's x and y (y
up the picture, 0.5 flat; its slope added to the tiles' in the same
frame), the second's R the ambient occlusion (1 open): mix(1, R, [10].y)
multiplies the ambient and distant lights alone, as
display.ambient_occlusion's occlusion does (the two multiplied: PRISM
v2's model self-occlusion), and the reflection with the cavity and PRISM
v2's highlights; its G the roughness (0 polished, 1 matte).
With XGPU_OVERLAY_SURFACE_ONLY (a shader the overlay does not name, or
the overlay off) there are no tiles: the slope is the surface map's alone,
the cavity 1, the roughness offset 0, and the uniform's [2],
[3].x and .y (the excluded channel, its threshold, the fade) and [6] (the
reflection's bend and cavity) are mod_materials.c's defaults. [10]: x the
normal's strength, y the occlusion's; z and w unused (the prototype's
highlight, now PRISM v2's model light's: below). With
XGPU_OVERLAY_SURFACE_ONLY and no surface map the overlay carries PRISM v2's
model light alone (below). */
#define XGPU_OVERLAY_BINDING 4
#define XGPU_OVERLAY_TILE_COUNT 4
#define XGPU_OVERLAY_SURFACE_IMAGE (XGPU_OVERLAY_TILE_COUNT + 1)
#define XGPU_OVERLAY_SURFACE_IMAGES 2
/* (PRISM v2's local reflections' probes: below) */
#define XGPU_OVERLAY_PROBE_IMAGE (XGPU_OVERLAY_SURFACE_IMAGE + XGPU_OVERLAY_SURFACE_IMAGES)
#define XGPU_OVERLAY_IMAGE_COUNT (XGPU_OVERLAY_PROBE_IMAGE + 1)
#define XGPU_OVERLAY_UNIFORM_COUNT 20
#define XGPU_OVERLAY_CLASS_MASK 16
#define XGPU_OVERLAY_CLASS_MASK_WINS 32
#define XGPU_OVERLAY_SURFACE 64
#define XGPU_OVERLAY_SURFACE_ONLY 128
#define XGPU_OVERLAY_VIEW(material_overlay) (((material_overlay) >> 1) & 7)

/* ---------- PRISM v2's model light (display.prism_specular,
prism_reflections, prism_shadows; off unless set)

A model draw lit for each pixel with a material overlay (the tiles, a
surface map, or neither: XGPU_OVERLAY_SURFACE_ONLY without
XGPU_OVERLAY_SURFACE, the overlay's frame and light with nothing added)
whose key's prism_lighting is not 0. With all three settings at their
first value nothing is armed and every draw is as the game draws it.

- XGPU_PRISM_SPECULAR_SUBTLE or _FULL: texture stage 3's reflection
  (a cube map: the shader's reflection, which its combiners mask by the
  multipurpose map's specular channel and weigh by CE's Fresnel and tint)
  gains the level's lights' highlights: GGX's distribution with Smith's
  visibility (Schlick-GGX, k = a / 2), a = r^2,
  r = max(the surface map's roughness, else [11].y, [11].z), along the
  micro normal toward the eye (the reflection's own direction reflected
  back), N.L-weighted, times the light's colour and [11].x, the sum
  capped at [11].w before it is added (the clamp to 1 after is the
  stage's). Subtle: the model's first distant light alone (ml[7], ml[8]:
  the lightmap's dominant light under it); full: also the two point
  lights (ml[1] to ml[6], as model_lighting weighs them). No Fresnel of
  its own: the combiners' is CE's. The light's shadow (below) darkens it.
- XGPU_PRISM_REFLECTIONS_LOCAL: the reflection read from the level's
  probes in place of the cube (image XGPU_OVERLAY_PROBE_IMAGE: an atlas of
  [17].y by [17].y octahedral cells of [17].z texels each, levels
  prefiltered by the tools; cell s at (s mod [17].y, s / [17].y)): the
  nearer of the probes [18] and [19] (xyz its place, w its cell; [17].x
  of them, 0 to 2) by the pixel's world position, the two mixed by their
  distances where both are given, at level log2([17].z) * r * [17].w
  (the roughness's blur), the octahedral cell's border kept off by half
  a texel of the level read.
- XGPU_PRISM_SHADOWS_SOFT or _TRACED (where display.ambient_occlusion
  "world" draws its window: the distance-field scene's cascades at
  XGPU_SPIRV_BINDING_WORLD_CASCADES of its set, which the backend binds
  for these draws whether or not they take the world's light, as a
  self-illuminated shader does not; their block in [13] to [16]): the
  first distant light's share of the light, and its highlight, by
  mix(1, 1 - h, [12].x), h the share of rays from the pixel pushed [12].w
  along its normal toward -ml[7] that world_trace (docs/world-ao.md) finds
  hitting the objects (its channel 1: the characters' capsules, the
  vehicles' wheels and boxes, its own among them) within [12].y, each hit
  at t counting smoothstep(0.05, 0.25, t) (a model's own shapes about its
  surface are not its shadow). Soft: four rays, the light's direction
  tilted [12].z * 0.04 four ways, out to 2 world units; traced: one ray,
  out to 6. Hardware ray queries (#602) take world_trace's place. */
#define XGPU_PRISM_SPECULAR_SUBTLE 1
#define XGPU_PRISM_SPECULAR_FULL 2
#define XGPU_PRISM_SPECULAR_OF(value) ((value) & 3)
#define XGPU_PRISM_REFLECTIONS_LOCAL 4
#define XGPU_PRISM_SHADOWS_SOFT 8
#define XGPU_PRISM_SHADOWS_TRACED 16
#define XGPU_PRISM_SHADOWS_OF(value) ((value) & 24)
/* (_SOFT or _TRACED: the distance-field scene's, not the sun's maps) */
#define XGPU_PRISM_SHADOWS_TRACES(value) (XGPU_PRISM_SHADOWS_OF(value) == XGPU_PRISM_SHADOWS_SOFT || \
	XGPU_PRISM_SHADOWS_OF(value) == XGPU_PRISM_SHADOWS_TRACED)
/* ---------- PRISM's sun shadows (display.prism_shadows "cascaded")

The level's sun (its sky's brightest light that lights the outside, a
lens flare's direction) casts real shadows: the window's casters drawn
from it into shadow maps (cascades fitted about the camera, out to a few
dozen world units; xgpu_stream.h's PRISM_SHADOWS, its block) before the
window's surfaces, the objects into one atlas and the structure into
another. A draw whose key has XGPU_PRISM_SHADOWS_CASCADED reads them
(XGPU_SPIRV_SET_SHADOWS) at its pixel's world position, pushed along its
normal 1.5 texels (facing the sun) to 4 (edge on) of the cascade it falls
in:

  v_m = the moving casters' visibility (PCSS: the mean depth of the
        blockers among 8 taps about the point, the penumbra by the distance
        from them to it times light.y, then 12 compared taps over it, each a
        2x2 bilinear test),
  v_s = the structure's (4 compared taps, a texel apart),
  both 1 past the last cascade (faded over its last fade.x of it);

- a structure's lightmap pass (its light before the textures multiply it,
  the enhanced dynamic lights added after): light times
  1 - light.x * (1 - v_m) * v_s * smoothstep(0, 0.25, N.sun): the moving
  casters take light.x of the light where the sun reached it (the
  structure's own shadows, already in the lightmap, are never darkened
  again, and a face turned away from the sun has none to lose);
- a model whose lighting has its world position (lit for each pixel
  then, with or without a material overlay): its first distant light's
  share by
  mix(1, v_m * mix(1, v_s, fade.y), fade.z) (its own limbs, other
  characters, vehicles, scenery, and the structure where the lightmap
  sample under it is sunlit: a Marine half under a ledge).

Hardware filtering does the comparisons (a depth compare sampler); the
draws without the key never read them. */
#define XGPU_PRISM_SHADOWS_CASCADED 24

/* the vertex constants the per-pixel model lighting reads, in a uniform of
their own (the vertex shader's 192 would pass the least fragment uniform
space a GPU must have): [0] c[-82] (the translucency in z), [1] to [11] c[-79] to
c[-69] (rasterizer_set_model_lighting's two point lights, two distant
lights and the ambient light), [12] c[-57] (with the enhanced dynamic
lights, the lights the model is not lit by: its object's own that say so,
as four light indices, -1 none: halo_dynamic_lights.h) */
#define XGPU_MODEL_LIGHT_COUNT 13
#define XGPU_MODEL_LIGHT_OWN_LIGHTS 12

/* ---------- display.ambient_occlusion in the light

The occlusion (found in the world at the pixel: below) is the share of
the ambient light that reaches a pixel, so it multiplies that light alone,
in the shaders that light the surfaces, never what a light adds (the
flashlight, a plasma bolt's glow, the enhanced dynamic lights): the pixel
key's ambient_occlusion, the light it multiplies:

- XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL: a model lit for each pixel
  (per_pixel_lighting): its ambient and distant lights (the level's light
  about it, sampled from the lightmap) by the occlusion, its two point
  lights or the enhanced dynamic lights added after;
- XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX: a model lit for each vertex, as the
  Xbox lit it: the vertex shader lights it as its program does, but hands
  on the ambient and distant lights' sum and the point lights' apart
  (XGPU_LIGHTING_MODEL_SPLIT), and the pixel shader adds them, the first by
  the occlusion; the program's own sum clamps each vertex's light, which
  no pixel could take apart again;
- XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP: a structure's lightmap pass, its
  light (the lightmap, before the textures multiply it) by the occlusion,
  the enhanced dynamic lights added after. */
enum
{
	XGPU_AMBIENT_OCCLUSION_LIGHT_NONE,
	XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL,
	XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX,
	XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP,
};

/* ---------- display.ambient_occlusion "world" in the light

The same three lights (XGPU_AMBIENT_OCCLUSION_LIGHT_*, the key's low bits:
XGPU_AMBIENT_OCCLUSION_LIGHT_OF) multiplied by the occlusion found in the
world at the pixel, where the key has
XGPU_AMBIENT_OCCLUSION_WORLD (docs/world-ao.md, "Shading";
port/linux/game/world_ao.c). The pixel's world position p and normal n
(the lighting's: a model's, or the lightmap pass's own, XGPU_LIGHTING_
ENVIRONMENT) read the window's block (xgpu_stream.h's struct
xgpu_world_ao_block: XGPU_SPIRV_BINDING_WORLD_AO, XGPU_MSL_BUFFER_WORLD_AO)
and the structure's distance field (XGPU_SPIRV_BINDING_WORLD_SDF,
XGPU_MSL_TEXTURE_WORLD_SDF), and the draw's object its uniforms' world_ao
(its owner slot among the proxies', -1 none; its reference):

  A_sdf   = 1 - clamp(K sum_i 2^-i clamp((h_i - d(p + n h_i)) / h_i, 0, 1), 0, 1),
            h_i = max(STEP, voxel) 2^(i-1), i = 1 to TAPS
  A_obj   = mix(A_sdf, clamp(A_sdf / max(reference, REFERENCE_LEAST), 0, 1), reference_weight)
  A_prox  = prod over the proxies of other owners within reach (1 - occlusion_i fade_i),
            fade_i = clamp((reach_i - |p - c_i|) / (REACH_FADE reach_i), 0, 1), c_i its
            centre (a sphere's and a box's a, a capsule's segment's middle); a box's
            fade_i its bound's (world_proxy_box_fade), and none where that is 0;
            the proxies those of the pixel's cell of the block's grid alone
            (XGPU_WORLD_AO_GRID_*), lowest first: the same product
  models:   A = mix(1, max(mix(1, A_obj, field_fade) A_prox, FLOOR), strength_objects)
  lightmap: A = mix(1, max(A_prox, FLOOR), strength_level)          (never the field)

d is the field's distance, read as world_sdf_sample reads it
(world_sdf_bake.c): the field is sparse (world_sdf.h), a table of cells
(XGPU_SPIRV_BINDING_WORLD_SDF_TABLE, XGPU_MSL_TEXTURE_WORLD_SDF_TABLE: RGBA8,
an atlas brick's x, y, z and the cell's kind, XGPU_WORLD_SDF_CELL_*) of
XGPU_WORLD_SDF_BRICK voxels' steps a side, and an atlas of bricks of
XGPU_WORLD_SDF_BRICK_SPAN voxels a side, XGPU_WORLD_SDF_ATLAS by
XGPU_WORLD_SDF_ATLAS a layer (the block's field[1].w layers); at p, in
voxels c = clamp((p - field[0].xyz) field[0].w, 0, 8 field[1].xyz), its cell
min(floor(c / 8), field[1].xyz - 1): a cell outside is +range, inside
-range, a brick its texel at its place times 9 plus c - 8 cell plus a half,
filtered, times range = RANGE_VOXELS / field[0].w. occlusion_i is
world_proxy_occlusion's (world_proxies.c), reach_i world_proxy_reach's;
world_ao.c's world_ao_occlusion is the whole of it on the CPU, line for
line. With XGPU_AMBIENT_OCCLUSION_WORLD_VIEW too (debug.world_ao_view) the
draw shows A in grey. */
#define XGPU_AMBIENT_OCCLUSION_WORLD 0x10
#define XGPU_AMBIENT_OCCLUSION_WORLD_VIEW 0x20
/* found once a pixel (xgpu_stream.h's WORLD_AO_PREPASS): with
XGPU_AMBIENT_OCCLUSION_WORLD, the draw reads its pixel's occlusion from the
backend's texture of it (XGPU_SPIRV_BINDING_WORLD_AO_PIXELS,
XGPU_MSL_TEXTURE_WORLD_AO_PIXELS) where it would find it, upsampled where it
was found at a quarter of the pixels (the block's parameters[1].w 2: each
2x2 block's at its nearest pixel; world_ao.h's world_ao_upsample); its twin
in the pre-pass (_INPUTS in its place) writes its pixel's inputs to the
occlusion instead of its colour, its alpha test as it is: four words, the
normal octahedral, two signed 16-bit halves (x low); the owner slot plus 2
in its low byte, the XGPU_WORLD_AO_INPUT_* bits, and in its high half the
reference (a model's) or the static scenery's page's texel (the lightmap
pass's: 1 where it has none) as a half float; its distance from the eye
(the block's grid[0]), a float's bits; and a spare word. The occlusion's
pass reads them (world_ao_shading.c's world_ao_pass_*) */
#define XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED 0x40
#define XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS 0x80
#define XGPU_WORLD_AO_INPUT_LIGHTMAP 0x100u
#define XGPU_WORLD_AO_INPUT_ALL_SHAPES 0x200u
#define XGPU_WORLD_AO_INPUT_PRESENT 0x400u
/* (the surfaces' upsampling of the occlusion found at a quarter of the
pixels: world_ao.h's WORLD_AO_UPSAMPLE_*) */
#define XGPU_WORLD_AO_UPSAMPLE_DEPTH 0.02f
#define XGPU_WORLD_AO_UPSAMPLE_FACING 0.2f
#define XGPU_WORLD_AO_UPSAMPLE_LEAST 1.0e-4f
#define XGPU_AMBIENT_OCCLUSION_LIGHT_OF(value) ((value) & 0x0f)
#define XGPU_WORLD_AO_K 1.6f
#define XGPU_WORLD_AO_STEP 0.15f
#define XGPU_WORLD_AO_TAPS 5
#define XGPU_WORLD_AO_REFERENCE_LEAST 0.25f
#define XGPU_WORLD_AO_REACH_FADE 0.25f
#define XGPU_WORLD_AO_FLOOR 0.2f
/* the field's (world_sdf.h's WORLD_SDF_*: d3d8_device.c checks they agree) */
#define XGPU_WORLD_SDF_BRICK 8
#define XGPU_WORLD_SDF_BRICK_SPAN 9
#define XGPU_WORLD_SDF_ATLAS 16
#define XGPU_WORLD_SDF_RANGE_VOXELS 12
#define XGPU_WORLD_SDF_CELL_OUTSIDE 0
#define XGPU_WORLD_SDF_CELL_INSIDE 1
#define XGPU_WORLD_SDF_CELL_BRICK 2
/* (world_proxies.c's WORLD_PROXIES_PI) */
#define XGPU_WORLD_AO_PI 3.14159265358979f
/* the block as the emitters declare it (xgpu_stream.h's struct
xgpu_world_ao_block, its XGPU_WORLD_AO_PROXIES_MAXIMUM and _PROXY_*: the
emitters' copies of these headers have not that one; d3d8_device.c checks
they agree): its members' offsets, the most proxies, their kinds */
#define XGPU_WORLD_AO_BLOCK_FIELD 0
#define XGPU_WORLD_AO_BLOCK_PARAMETERS 32
#define XGPU_WORLD_AO_BLOCK_PROXIES 64
#define XGPU_WORLD_AO_BLOCK_GRID 3136
#define XGPU_WORLD_AO_BLOCK_CELLS 3168
#define XGPU_WORLD_AO_SHAPES 64
/* the proxies' grid (world_ao.h's WORLD_AO_GRID_*: world_ao_grid) and the
least box occlusion computed, one over (world_proxies.c's
WORLD_PROXIES_LEAST: world_proxy_box_fade) */
#define XGPU_WORLD_AO_GRID_X 16
#define XGPU_WORLD_AO_GRID_Y 16
#define XGPU_WORLD_AO_GRID_Z 8
#define XGPU_WORLD_AO_GRID_CELLS (XGPU_WORLD_AO_GRID_X * XGPU_WORLD_AO_GRID_Y * XGPU_WORLD_AO_GRID_Z)
#define XGPU_WORLD_AO_LEAST 510.0f
/* (world_proxies.c's WORLD_PROXIES_FAR: a box as its faces' patches past
it, in half diagonals) */
#define XGPU_WORLD_AO_FAR 6.0f
#define XGPU_WORLD_AO_SPHERE 1
#define XGPU_WORLD_AO_CAPSULE 2
#define XGPU_WORLD_AO_BOX 3
/* (a vehicle's occlusion field: its shape and the next two, read from the
objects' atlas) */
#define XGPU_WORLD_AO_FIELD 4

/* ---------- the distance-field scene (port/linux/include/world_cascades.h)

The cascades about the camera, one 3D texture of two signed bytes a texel
(RG8 snorm: the structure's distance, the objects'), each cascade
XGPU_WORLD_CASCADE_SPAN texels a side (its side and a guard), one above the
other along z; a cascade's block vec4 is its first voxel's centre and its
voxel (0: not ready). world_trace, the emitters' ray step through them
(xgpu_spirv.c's, xgpu_msl.c's), is world_cascades.c's world_trace_ray
operation for operation; these are world_cascades.h's WORLD_CASCADE* and
WORLD_TRACE_* */
#define XGPU_WORLD_CASCADES 4
#define XGPU_WORLD_CASCADE_SIDE 64
#define XGPU_WORLD_CASCADE_SPAN 65
#define XGPU_WORLD_CASCADE_RANGE_VOXELS 4
#define XGPU_WORLD_TRACE_STEPS 24
#define XGPU_WORLD_TRACE_HIT 0.25f
#define XGPU_WORLD_TRACE_LEAST_STEP 0.5f
#define XGPU_WORLD_TRACE_STRUCTURE 1
#define XGPU_WORLD_TRACE_OBJECTS 2
/* World AO's rays through it (the pass's world_ao_traced; world_ao.h's
WORLD_AO_RAYS and so on): rays a pixel, cosine-weighted about its normal;
their reach and their start off the surface (world units); and the share of
a frame's occlusion blended into the history reprojected (temporal
accumulation), and the history's tests (the distance's relative
difference, the normals' least cosine) */
#define XGPU_WORLD_AO_RAYS 4
#define XGPU_WORLD_AO_RAY_REACH 2.0f
/* (a pixel no object is within the reach of casts none: the objects'
distance at it read first in the finest cascade whose range, 4 voxels,
holds the reach, cascade 1's 2 wu) */
#define XGPU_WORLD_AO_RAY_CASCADE 1
#define XGPU_WORLD_AO_RAY_BIAS 0.1f
#define XGPU_WORLD_AO_HISTORY_NEW 0.2f
#define XGPU_WORLD_AO_HISTORY_DISTANCE 0.05f
#define XGPU_WORLD_AO_HISTORY_NORMAL 0.8f

/* ---------- enhanced dynamic lights

display.dynamic_lights "enhanced" (d3d8_device.c, halo_dynamic_lights_set):
the frame's dynamic lights (up to the game's 128 a window) light the
structure's lightmap pass and the models lit for each pixel, each pixel by
the lights near it alone. A grid about the camera, its cells boxes in the
world (eye-independent: the VR mode's eyes share it), holds for each cell a
128-bit mask of the lights whose spheres reach it (an integer 3D texture at
XGPU_DYNAMIC_LIGHTS_UNIT); the lights are a uniform block at
XGPU_DYNAMIC_LIGHTS_BINDING. The cells grow away from the camera: a cell's
index along each axis is log2(1 + |offset| / XGPU_DYNAMIC_LIGHTS_CELL) times
that axis's scale, from the middle, so that the grid reaches
XGPU_DYNAMIC_LIGHTS_REACH world units each way with small cells near the
camera, where lights are seen large. */
#define XGPU_DYNAMIC_LIGHTS_MAXIMUM 128
#define XGPU_DYNAMIC_LIGHTS_GRID_X 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Y 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Z 16
#define XGPU_DYNAMIC_LIGHTS_CELL 0.5f
#define XGPU_DYNAMIC_LIGHTS_REACH 128.0f
/* the largest soft core of a light's falloff, world units (xgpu_spirv.c) */
#define XGPU_DYNAMIC_LIGHTS_CORE 0.6
#define XGPU_DYNAMIC_LIGHTS_UNIT 12
#define XGPU_DYNAMIC_LIGHTS_BINDING 1

/* Which lights reach which of the structure's clusters: a light reaches the
clusters its sphere floods into through the structure's portals from where
it is (structure_clusters_in_sphere, as the game gathers a light's surfaces
for its own pass), and no other, so that a light on the far side of a wall
does not light this side of it. Past the cells, at z = GRID_Z, the cells'
texture has a slice of each cluster's 128-bit mask of the lights that reach
it (XGPU_DYNAMIC_LIGHTS_CLUSTERS of them: the game's most), uploaded with
the cells. Past it are the world's slices (XGPU_DYNAMIC_LIGHTS_WORLD_SLICES),
uploaded when the structure changes: the structure's 3D BSP, which says in
which leaf, and so cluster, a point is (bsp3d_test_point), as four words a
texel, texel t at x = t % 32, y = t / 32 % 32, z = GRID_Z + 1 + t / 1024:
texel 0 the header (the node count, the planes' first texel, the leaves'
first texel, the leaf count), from texel 1 a node each (its plane, its
children behind and in front: a child with the top bit set is a leaf, all
bits set none), then the planes (their floats' bits: the normal, and its
distance along it), then the leaves' clusters, four a texel (all bits set:
none). A pixel finds its cluster from its point a little off its surface,
along its normal, and its cell's lights are those its cluster's mask has; a
world with no nodes (none uploaded, or too large) leaves the cells' lights as
they are, as does a point the BSP puts in no cluster. */
#define XGPU_DYNAMIC_LIGHTS_CLUSTERS 512
#define XGPU_DYNAMIC_LIGHTS_WORLD_SLICES 240
#define XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS (XGPU_DYNAMIC_LIGHTS_WORLD_SLICES * 1024)
/* the bytes uploaded each frame: the cells and the clusters' masks */
#define XGPU_DYNAMIC_LIGHTS_CELLS_BYTES \
	(XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y * (XGPU_DYNAMIC_LIGHTS_GRID_Z + 1) * 16)
/* how far off its surface a pixel's point is put, world units */
#define XGPU_DYNAMIC_LIGHTS_CLUSTER_OFFSET 0.02f
/* the most nodes a pixel walks down the BSP (bsp3d.c's
MAXIMUM_BSP3D_TRAVERSAL_DEPTH) */
#define XGPU_DYNAMIC_LIGHTS_WORLD_DEPTH 128
enum
{
	XGPU_DYNAMIC_LIGHTS_ENVIRONMENT = 1,
	XGPU_DYNAMIC_LIGHTS_MODEL,
};

/* the uniform block, as std140 lays it out: [0] the grid's middle (the
camera) and the light count, [1] each axis's cells a doubling of the offset
and 1 / XGPU_DYNAMIC_LIGHTS_CELL, [2] the look (xgpu_spirv.c); then each
light's position and 1 / radius (less than 0: models only, the structure
lit by the game's own pass: a spotlight's gel), its color (linear, the
look's brightness in it) and its cone's offset, its cone's axis and scale
(omni: 0, 0 and an offset of 1) */
struct xgpu_dynamic_lights_block
{
	float grid[3][4];
	float position[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float color[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float cone[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
};

/* writes BGRA pixels as a BMP named <name><frame>.bmp in the screenshot
folder (debug.screenshot_directory): rows from the top, or from the bottom
(d3d8_device.c) */
void xgpu_write_bmp(const char *name, const unsigned char *pixels, unsigned long width, unsigned long height,
	BOOL bottom_up);
/* the <frame> of this frame's screenshots (where the host writes the VR
mode's, it is told it: xgpu_stream_encode.c) */
unsigned long xgpu_screenshot_number(void);

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
	BOOL hires;         /* a high-res HUD texture drawn in the texture's place (hud_hires.h) */
	BOOL hires_coverage; /* ... whose green is its coverage (a meter's) */
	BOOL hires_point_threshold; /* ... whose red holds discrete segment data */
};

/* (xbox_texture_decode.c) */
void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);
/* bytes of a level (all of a volume's slices), and a level's width, height
or depth from the first level's */
unsigned long xgpu_texture_level_size(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_dimension(unsigned long base, unsigned long level);
/* the size is one D3DDevice_GetDeviceCaps allows (d3d8_device.c) */
BOOL xgpu_texture_size_supported(const struct xgpu_texture_description *description);
/* a level of a texture that is not compressed (a volume's slices one after
another), from its texels at source, as 32-bit BGRA (a DWORD each: ARGB),
rows from the first; FALSE when out of memory. palette: a P8 texture's 256
D3DCOLORs (NULL: grey) */
BOOL xgpu_texture_decode_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, DWORD *destination);
/* a level of a DXT texture, as xgpu_texture_decode_level */
void xgpu_texture_decode_dxt(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, DWORD *destination);

#endif
