/*
XGPU_VULKAN_TERRAIN.H

The terrain, weather and water overhaul's host half (docs/prism_terrain.md;
overhaul_research/prism_v4_renderer/terrain/design.md). T0: the surface
classes of the map's materials, PRISM_TERRAIN's frame, events and waders
kept, and the debug views drawn over the resolved frame
(debug.gbuffer_view "terrain_class", debug.terrain_events). Nothing here
runs without PRISM_TERRAIN's FRAME this frame: never in Classic.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_TERRAIN_H
#define __HALO_LINUX_XGPU_VULKAN_TERRAIN_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* the surface classes (tools/prism_terrain_classes.py's CLASSES, in their
order) */
enum
{
	XGPU_TERRAIN_CLASS_NONE,
	XGPU_TERRAIN_CLASS_SAND,
	XGPU_TERRAIN_CLASS_DIRT,
	XGPU_TERRAIN_CLASS_MUD,
	XGPU_TERRAIN_CLASS_ROCK,
	XGPU_TERRAIN_CLASS_SNOW,
	XGPU_TERRAIN_CLASS_ICE,
	XGPU_TERRAIN_CLASS_WATER,
	XGPU_TERRAIN_CLASSES,
};

/* a material's classes as SCENE_MATERIAL gives it (material 1 starts the
map's set again): CE's material type for an environment shader, the
curated table's entry where its path hash has one */
void xgpu_vulkan_terrain_material(uint32_t material, const uint32_t hash[2], uint32_t family,
	uint32_t material_type);
/* a material's class (which: 0 the first, 1 the second), its CE material
type (255 none known) */
uint8_t xgpu_vulkan_terrain_class(uint32_t material, int which);
uint8_t xgpu_vulkan_terrain_material_type(uint32_t material);
/* PRISM_TERRAIN (the decoder checked its count) */
void xgpu_vulkan_terrain_record(const struct xgpu_stream_prism_terrain *record);
/* whether this frame's FRAME asked for debug.gbuffer_view "terrain_class"
(the frame is then the view: nothing drawn over it after the resolve) */
int xgpu_vulkan_terrain_view_held(void);
/* the debug views over a window's resolved frame (its colour target), from
its G-buffer's words and its camera (SCENE_VIEW); nothing where this frame
had no FRAME or asks for neither */
void xgpu_vulkan_terrain_debug(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *words,
	const struct xgpu_stream_scene_view *view);
/* (T1) the HQ layers over a window's level pixels: CE's base colour (the
base target) times the terrain pack's detail, the G-buffer's normal and
roughness (its words) the pack's, the distance from its second target's
words (extras), the point from its camera (SCENE_VIEW); at the texture
pass's end (BASE_TEXTURED), else before the resolve; once a window a frame,
only where this frame's FRAME asks for display.terrain_layers "hq" and the
pack is there */
void xgpu_vulkan_terrain_layers(struct xgpu_vulkan_image *base, struct xgpu_vulkan_image *words,
	struct xgpu_vulkan_image *extras, const struct xgpu_stream_scene_view *view);
/* (a window's G-buffer begun: the pass may be drawn for it) */
void xgpu_vulkan_terrain_window(void);
/* the frame's end: a FRAME is this frame's alone */
void xgpu_vulkan_terrain_frame_end(void);

#endif
