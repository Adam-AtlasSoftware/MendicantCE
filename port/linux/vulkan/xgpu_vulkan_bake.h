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
#include <vulkan/vulkan_core.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* PRISM_BAKE played (host_render_stream's checks done): data its phase's
(NULL none). The control's frame runs the bake's slice of GPU work */
void xgpu_vulkan_bake_record(const struct xgpu_stream_prism_bake *record, const void *data);

/* PRISM Raster's frame: the window's opaque pixels (copy: a sampled copy of
the light target, CE's light; base and additions CE's other targets) lit
from the bake where it has them, each by its material's diffuse and
specular light (*whole set: the frame itself, CE's own where the bake has
nothing), into an image of its own (RGBA16F, the copy's size), read by the
resolve in the copy's place; words and extras the G-buffer's targets, view
SCENE_VIEW's cameras of the pass; the bake's view (XGPU_GBUFFER_VIEW_BAKE_*,
0 none) shown in the light's place instead. NULL where the bake has nothing
to give (CE's frame stays) */
struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(struct xgpu_vulkan_image *copy, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t bake_view, int *whole);

/* PRISM v4's exposure (docs/prism_bake.md, "Raster's exposure"): the meter's
buffer (made in commands the first time; 0 none), which a frame's pass fills
with its level pixels' lit light and CE's light (xgpu_vulkan_bake_glsl.inc's
EXPOSURE_COMMON: meter_add), and the step after the pass, which makes the
exposure the next frame reads (exposure_of) from display.prism_exposure's
settings and empties the meter. Raster's frame takes them; the composite to
come takes the same */
VkDeviceAddress xgpu_vulkan_bake_meter(VkCommandBuffer commands);
void xgpu_vulkan_bake_exposure_step(VkCommandBuffer commands);

/* the device going away: everything let go of */
void xgpu_vulkan_bake_shutdown(void);

#endif
