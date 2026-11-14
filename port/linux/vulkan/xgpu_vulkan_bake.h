/*
XGPU_VULKAN_BAKE.H

PRISM v4's in-game baker (overhaul_research/prism_v4_renderer/design.md,
section 3; docs/prism_bake.md) and PRISM Raster's light (section 1.8), in
the Vulkan backend (xgpu_vulkan_bake.c). The stream's decoder hands it the
game's PRISM_BAKE records (each frame's control, the reach once a BSP);
the bake runs a slice of each frame on the GPU, from SCENE_STATIC's level as
drawn and display.prism_rt's materials and rig, into the bake file; the
deferred resolve asks it for the light of display.lighting "raster".
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_BAKE_H
#define __HALO_LINUX_XGPU_VULKAN_BAKE_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* PRISM_BAKE played (host_render_stream's checks done): data its phase's
(NULL none). The control's frame runs the bake's slice of GPU work */
void xgpu_vulkan_bake_record(const struct xgpu_stream_prism_bake *record, const void *data);

/* PRISM Raster's light: the window's light target's pixels (copy: a
sampled copy of them, CE's light) lit from the bake where it has them,
into an image of its own (RGBA16F, the copy's size), read by the resolve in
the copy's place; words and extras the G-buffer's targets, view SCENE_VIEW's
cameras of the pass, which the bake's view (XGPU_GBUFFER_VIEW_BAKE_*, 0
none) shows in the light's place. NULL where the bake has nothing to give
(CE's light stays) */
struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(struct xgpu_vulkan_image *copy, struct xgpu_vulkan_image *words,
	struct xgpu_vulkan_image *extras, const struct xgpu_stream_scene_view *view, uint32_t bake_view);

/* the device going away: everything let go of */
void xgpu_vulkan_bake_shutdown(void);

#endif
