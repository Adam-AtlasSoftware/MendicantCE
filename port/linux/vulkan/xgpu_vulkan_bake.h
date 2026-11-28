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
0 none) shown in the light's place instead; light_shadows display.
enhanced_light_shadows (0 off to 3 ultra). NULL where the bake has nothing
to give (CE's frame stays) */
struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(struct xgpu_vulkan_image *copy, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t bake_view, int materials, int objects_volume,
	uint32_t light_shadows, int *whole);

/* what lit a frame of Raster's (HALO_RASTER_FRAME_LOG, a diagnostic: its
changes logged): the bake whole or in part, or CE's frame and why, and
whether the window had its G-buffer; xgpu_vulkan_bake_raster notes its own
frames, the resolve those it is not asked for (gbuffer 0) */
enum
{
	XGPU_BAKE_RASTER_SHOWN_FULL,
	XGPU_BAKE_RASTER_SHOWN_PARTIAL,
	XGPU_BAKE_RASTER_SHOWN_NONE,
	XGPU_BAKE_RASTER_SHOWN_SET,
	XGPU_BAKE_RASTER_SHOWN_EMPTY,
	XGPU_BAKE_RASTER_SHOWN_UNTRACEABLE,
	XGPU_BAKE_RASTER_SHOWN_KINDS
};

void xgpu_vulkan_bake_raster_note(int gbuffer);

/* PRISM Path Traced's view of the bake on the GPU (xgpu_vulkan_pt.c; docs/
prism_pathtraced.md, "No baked light"): the scene's block alone, by its
device address (xgpu_vulkan_bake_glsl.inc's Scene: the bake's acceleration
structure, its materials, lamps, fixtures and rig: the geometry and the
lights, made for the BSP whether or not a bake is there), its set's
generation, and the plan's tiles (which world it is, for the log). Never the
bake's light: no lightmap page, sun mask, irradiance volume, reflection probe
or light-importance grid, a pack's or this machine's, so that Path Traced's
light is the same whether or not one is there. 0 where the bake has no
scene to trace (or only another set's) */
struct xgpu_vulkan_bake_lookup
{
	uint64_t scene;
	uint32_t generation, tiles_planned;
};
int xgpu_vulkan_bake_lookup(struct xgpu_vulkan_bake_lookup *lookup);
/* PRISM Path Traced short of the card's memory (xgpu_vulkan_pt.c): the
bake's light, which it never reads, put in the host's memory (a whole bake's:
nothing more baked, nothing being read from the file), the card's freed
after this submission; back on the card as PRISM Raster draws again. 1
where buffers were moved now; parked: 1 while they are in the host's */
int xgpu_vulkan_bake_park(void);
int xgpu_vulkan_bake_parked(void);
/* PRISM Path Traced's shortage of the card's memory told to the game
(XGPU_BAKE_STATUS_PT_MEMORY: 0 none, the light grid's scale in hundredths,
1 no grid fits), which the bake's indicator shows */
void xgpu_vulkan_bake_pt_memory(uint32_t value);
/* (T4) the loaded bake's sun, the one Raster's light pass lights by (its
direction toward the sun, w over 0 where it has one): 1 where there is one */
int xgpu_vulkan_bake_sun(float sun[4]);

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
