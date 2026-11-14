/*
XGPU_VULKAN_RT.H

display.prism_rt (docs/prism_rt.md): PRISM's hardware ray tracing in the
Vulkan backend (xgpu_vulkan_rt.c). The stream's decoder
(xgpu_vulkan_stream.c) hands it the structure's triangles and each
frame's settings, the objects' draws as world AO's pre-pass draws them,
and at the pre-pass's end asks it for the window's light: the image the
surfaces then read in world AO's place.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_RT_H
#define __HALO_LINUX_XGPU_VULKAN_RT_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* the device traces rays (xgpu_vulkan_rt_device) and the passes' pipelines
are made: INITIALIZE's ray_tracing */
int xgpu_vulkan_rt_available(void);

/* RT_STATIC and RT_FRAME (xgpu_stream.h) */
void xgpu_vulkan_rt_static(const struct xgpu_stream_rt_static *record, const void *data);
void xgpu_vulkan_rt_frame(const struct xgpu_stream_rt_frame *record);

/* the baker's (xgpu_vulkan_bake.c): the last whole set built (its
generation: SCENE_STATIC's where the level traced is its), its materials
(material_count of XGPU_STREAM_RT_MATERIAL_FLOATS floats) and alpha maps
(words), in host memory the set keeps until the next; and the last RT_FRAME
(its rig and sun, NULL none); 0 where no set is built */
int xgpu_vulkan_rt_scene(uint32_t *generation, const float **materials, uint32_t *material_count,
	const uint32_t **alpha, uint32_t *alpha_words, const struct xgpu_stream_rt_frame **frame);

/* this frame traces (an RT_FRAME with a mode since the last frame's end) */
int xgpu_vulkan_rt_active(void);
/* the frame traces the stock models, not the mesh store's replacements
(display.prism_rt_objects "low_lod") */
int xgpu_vulkan_rt_stock_models(void);
/* port: a replacement mesh's pieces (that many triangles, its node 0 at
origin) traced: display.prism_rt_objects "auto" within a reach of the eye
and a share of the window's triangles, "view_lod" always; else the game's
own model in their place */
int xgpu_vulkan_rt_replacement_traced(const float origin[3], uint32_t triangles);

/* a pre-pass draw of an object, as it is drawn, by where its vertices,
indices and constants are on the GPU (nothing of them is read here: a
compute pass skins them, xgpu_vulkan_rt.c's skin): its vertices' buffer
(a storage buffer, which has a device address where the device traces
rays: xgpu_vulkan.c's buffer_make), the byte of its vertex 0 there and the
vertices the draw may name from it, their stride and the stock layout's
position (float x y z at position_offset; its two nodes, bytes of three
times the node's index, at nodes_offset, and the first's weight, a signed
16-bit fraction, at weight_offset: -1 none, node 0 then); its indices
(VK_NULL_HANDLE: count vertices in order; else count indices of
index_size bytes from element index_first of the buffer), each plus
index_add the vertex; its primitive (XGPU_VULKAN_RT_*); the vertex
constants it is drawn with (192 rows, the nodes' matrices three rows each
from c[60]: on the GPU at constants_buffer's constants_offset, and the
CPU's copy; VK_NULL_HANDLE: the vertices are in the world, a mesh store's
skinning). The objects' acceleration structures are kept from frame to
frame (display.prism_rt_cache): identity and occurrence name the draw from
one frame to the next (its vertices' and indices' buffers and places; the
same draw again in a window, another object of the same model, its next
occurrence); generations change when their bytes are written (the
mirror's segments' writes; for bytes in the host's memory, a hash of a
sample of them); pose, for
vertices in the world, changes when they move (0 with constants: the rows
are compared); origin is node 0's place (a draw that jumps far is built
again, not refitted); volatile_source: vertices or indices written for
this draw alone (built again each frame) */
enum
{
	XGPU_VULKAN_RT_LIST,
	XGPU_VULKAN_RT_STRIP,
	XGPU_VULKAN_RT_FAN,
};
struct xgpu_vulkan_rt_draw
{
	VkBuffer vertex_buffer;
	uint64_t vertex_offset;
	uint32_t vertex_count, stride;
	int position_offset, nodes_offset, weight_offset;
	VkBuffer index_buffer;
	uint32_t index_size;
	uint64_t index_first;
	/* (or, with no index buffer, 16-bit indices the CPU has: copied into the
	ring where the draw is skinned) */
	const uint16_t *cpu_indices;
	uint32_t count;
	int32_t index_add;
	int primitive;
	VkBuffer constants_buffer;
	uint64_t constants_offset;
	const float (*constants)[4];
	/* an occluder of the pre-pass (no world light of its own): traced, but
	left out where it is all within reach of the eye (the first-person
	weapon and hands) */
	int occluder;
	uint64_t identity[4];
	uint32_t generation[2];
	uint64_t pose;
	float origin[3];
	int volatile_source;
	/* the submission the constants' and vertices' ranges are of */
	uint64_t serial;
	/* its base map's mean colour (linear), the traced hits' albedo; 0
	none known */
	float albedo[3];
	int has_albedo;
	/* a cutout's (foliage's) share of its texels drawn, below 1: its rays
	take it as that share solid (traced as the others, its hits kept by that
	chance); 1 opaque */
	float coverage;
	/* port: its object's slot among the window's world-occlusion owners
	(0 to 63; else none): the object's motion from the last frame, by which
	the denoisers find its point there */
	int owner;
};
void xgpu_vulkan_rt_capture(const struct xgpu_vulkan_rt_draw *draw);
/* the frame traces the cutouts (display.prism_rt_alpha) */
int xgpu_vulkan_rt_alpha_traced(void);
/* a draw the capture passed over (its layout or alpha test), counted */
void xgpu_vulkan_rt_skipped(void);

/* a window's pre-pass begins (its captures from none) */
void xgpu_vulkan_rt_window_begin(void);
/* ... and ended: the acceleration structures built and the rays traced
from its inputs (width x height, world AO's) as the pass's camera saw it;
the image the surfaces read (world AO's pass's texels' layout, at the
pass's scale), or NULL (world AO's pass then draws as before) */
struct xgpu_vulkan_image *xgpu_vulkan_rt_window_end(struct xgpu_vulkan_image *inputs, uint32_t width,
	uint32_t height, const struct xgpu_world_ao_pass *pass);

/* display.prism_tone_curve (with display.prism_rt "pathtraced"): the 3D
view's rectangle (corners: x0 y0 x1 y1) through the frame's tone curve, in
compute, before PRISM's grade and the tone map; 0 where none was drawn (no
curve, no ray tracing this frame, a view not of half floats or
multisampled) */
int xgpu_vulkan_rt_tone_curve(struct xgpu_vulkan_image *view, const int32_t corners[4]);

#endif
