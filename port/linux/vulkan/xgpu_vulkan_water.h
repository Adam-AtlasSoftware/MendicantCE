/*
XGPU_VULKAN_WATER.H

The terrain overhaul's water layer (T7; docs/prism_terrain.md, overhaul_research/prism_v4_renderer/terrain/design.md
section 8): a CE water group composited by a compute pass (xgpu_vulkan_water.glsl) in CE's colour draw's place, from
what the game drew for it (PRISM_TERRAIN's WATER_DRAW steps: xgpu_vulkan_stream.c keeps their images). Its device is
ray tracing's (xgpu_vulkan_rt_device), which PRISM's lighting modes need already; the game sends its steps only
where the backend has them.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_WATER_H
#define __HALO_LINUX_XGPU_VULKAN_WATER_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* a group's inputs: what was behind the water (its colour), CE's ripples and base map as the game drew them at the
water's pixels (the base's alpha 1 there, 0 elsewhere: the water's mask), the depth with the water, the G-buffer's words and second target (the distances), CE's
reflection cube (NULL: none), the window's camera, the WATER_DRAW's item, the sun (its direction toward it and
colour; sun 0: none), the frame's power (2.2: an encoded frame) and Path Traced's flag */
struct xgpu_vulkan_water_job
{
	struct xgpu_vulkan_image *behind, *ripples, *base, *depth, *words, *extras, *cube;
	const struct xgpu_stream_scene_view *view;
	const struct xgpu_stream_terrain_water *water;
	float sun_direction[3], sun_color[3];
	int sun;
	float seconds, power;
	int pathtraced;
	/* (an opaque water's layer, over the resolved frame: the classes' table
	in the base's place, no ripples or water item; the water's pixels those
	of the level's materials of the water class) */
	struct xgpu_vulkan_image *classes;
	/* (T8, Path Traced: the reflection's and the refraction's light traced
	(xgpu_vulkan_pt_water: the upper ray's, its alpha the sun's visibility;
	the lower ray's, its alpha its length in the water), NULL where not
	traced; reference 1 where they are debug.pt_reference's sums) */
	struct xgpu_vulkan_image *traced_up, *traced_down, *traced_aux;
	int reference;
};

/* the group's frame (an RGBA16F image of the colour target's size: the water at its pixels, what was behind it
elsewhere, the alpha behind's), which the caller copies into the colour target; NULL where it cannot be made */
struct xgpu_vulkan_image *xgpu_vulkan_water_composite(const struct xgpu_vulkan_water_job *job);
/* CE's ripples' strength in the normal (HALO_WATER_RIPPLES), the composite's and Path Traced's water rays' (T8) */
float xgpu_vulkan_water_ripples(void);
/* the frame's end (the descriptor sets of a frame four frames back used again) */
void xgpu_vulkan_water_frame_end(void);

#endif
