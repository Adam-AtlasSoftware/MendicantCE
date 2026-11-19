/*
XGPU_VULKAN_FSR3.H

display.upscaler "fsr3": AMD's FidelityFX Super Resolution 3.1 upscaler
in the Vulkan backend (xgpu_vulkan_fsr3.c; port/third_party/
fidelityfx-fsr3), the renderer's own upscaler, which xgpu_vulkan_upscale.c
runs over a view's inputs.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_FSR3_H
#define __HALO_LINUX_XGPU_VULKAN_FSR3_H

#include "xgpu_vulkan.h"

/* a frame's inputs: the commands recorded into (outside a pass; every image
in the GENERAL layout, written by earlier commands with a barrier that
makes it readable by compute shaders); the view's colour (LDR, linear
filtering possible), depth (reversed: 1 near), motion (pixels: last frame's
place less this one's, not jittered) and reactive mask (0 to 1), each at the
render size from 0, 0, sampled views; the output (four 16-bit floats at the
output size), a storage view; the sizes, the jitter (pixels), the camera's
near and far planes (world units) and vertical field of view (radians), the
frame's time, the sharpening (0 none, to 1), a reset (the history dropped)
and the world unit in metres */
struct xgpu_vulkan_fsr3_frame
{
	VkCommandBuffer commands;
	VkImageView color, depth, motion, reactive, output;
	uint32_t render_width, render_height, output_width, output_height;
	float jitter[2];
	float near_plane, far_plane, field_of_view;
	float frame_milliseconds, sharpness;
	int reset;
	float meters_per_unit;
};

/* the upscaler's state made where it is not (its passes, the first time; its
resources, whenever the sizes change) and its passes recorded: 1, or 0 where
it cannot be (logged once, the view then left as it is) */
int xgpu_vulkan_fsr3_dispatch(const struct xgpu_vulkan_fsr3_frame *frame);
/* its jitter's sequence's length for the sizes (FSR's: 8 times the area's
ratio) and its offset (pixels, each -0.5 to 0.5) at index */
uint32_t xgpu_vulkan_fsr3_jitter_phases(uint32_t render_width, uint32_t output_width);
void xgpu_vulkan_fsr3_jitter(uint32_t index, uint32_t phases, float jitter[2]);
/* everything let go of (the device idle) */
void xgpu_vulkan_fsr3_shutdown(void);

#endif
