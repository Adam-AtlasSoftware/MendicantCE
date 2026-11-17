/*
XGPU_VULKAN_PT.H

PRISM Path Traced (overhaul_research/prism_v4_renderer/design.md, sections
1.5 and 3.8; docs/prism_pathtraced.md): display.lighting "pathtraced", the
deferred core's frame lit by the path tracer from the G-buffer and the bake,
every CE light off (xgpu_vulkan_pt.c). The deferred resolve asks it for the
frame as Raster's relight is asked (xgpu_vulkan_stream.c).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_PT_H
#define __HALO_LINUX_XGPU_VULKAN_PT_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* the frame's opaque pixels lit by the path tracer, into an image of its
own (RGBA16F, the copy's size) that the resolve draws as it is (*whole):
light, base and additions the resolve's (the light's sampled copy: CE's
frame where no surface is, the sky), words and extras the G-buffer's
targets, view SCENE_VIEW's cameras of the pass, phase the resolve's value
(XGPU_GBUFFER_PHASE_PATHTRACED's profile and denoiser bits, its view).
The denoise step's one call site is here (the plugins' denoise entry points
go in its place). NULL where it cannot trace yet (no device, no bake's
scene): the resolve then draws nothing new over the target */
struct xgpu_vulkan_image *xgpu_vulkan_pt_frame(struct xgpu_vulkan_image *light, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t phase, int *whole);

/* the device going away: everything let go of */
void xgpu_vulkan_pt_shutdown(void);

#endif
