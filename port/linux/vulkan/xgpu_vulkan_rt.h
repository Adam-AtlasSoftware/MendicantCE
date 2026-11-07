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

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* the device traces rays (xgpu_vulkan_rt_device) and the passes' pipelines
are made: INITIALIZE's ray_tracing */
int xgpu_vulkan_rt_available(void);

/* RT_STATIC and RT_FRAME (xgpu_stream.h) */
void xgpu_vulkan_rt_static(const struct xgpu_stream_rt_static *record, const float *positions);
void xgpu_vulkan_rt_frame(const struct xgpu_stream_rt_frame *record);

/* this frame traces (an RT_FRAME with a mode since the last frame's end) */
int xgpu_vulkan_rt_active(void);

/* a pre-pass draw of an object, as it is drawn: its vertices (the stock
layout's position, float x y z at position_offset; its two nodes, bytes of
three times the node's index, at nodes_offset, and the first's weight, a
signed 16-bit fraction, at weight_offset: -1 none, node 0 then), the bytes
readable from vertices, its indices (NULL: count vertices in order; else
count indices, each less index_base the vertex), its primitive
(XGPU_VULKAN_RT_*), and the vertex constants it is drawn with (192, the
nodes' matrices three rows each from c[60]) */
enum
{
	XGPU_VULKAN_RT_LIST,
	XGPU_VULKAN_RT_STRIP,
	XGPU_VULKAN_RT_FAN,
};
struct xgpu_vulkan_rt_draw
{
	const uint8_t *vertices;
	uint32_t stride, bytes;
	int position_offset, nodes_offset, weight_offset;
	const uint16_t *indices;
	uint32_t count;
	int32_t index_base;
	int primitive;
	const float (*constants)[4];
	/* an occluder of the pre-pass (no world light of its own): traced, but
	left out where it is all within reach of the eye (the first-person
	weapon and hands) */
	int occluder;
};
void xgpu_vulkan_rt_capture(const struct xgpu_vulkan_rt_draw *draw);
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

#endif
