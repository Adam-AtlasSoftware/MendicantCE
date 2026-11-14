/*
XGPU_VULKAN_SCENE.H

PRISM v4's scene records as the Vulkan backend keeps them
(xgpu_vulkan_scene.c): the level's geometry (SCENE_STATIC), one copy for
every consumer.
*/

#ifndef XGPU_VULKAN_SCENE_H
#define XGPU_VULKAN_SCENE_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

/* a whole SCENE_STATIC set: its generation, its triangles (9 floats each)
and their attributes (XGPU_SCENE_STATIC_ATTRIBUTE_WORDS words each), the
BSP's surfaces and lightmap pages (width and height each) */
struct xgpu_scene_static_set
{
	uint32_t generation;
	uint32_t triangle_count, surface_count, page_count;
	const float *positions;
	const uint32_t *attributes;
	const uint32_t *pages;
};

/* SCENE_STATIC played (host_render_stream's checks done: data inside the
stream and count items of the phase's size) */
void xgpu_vulkan_scene_static(const struct xgpu_stream_scene_static *record, const void *data);

/* the last whole set (NULL none) */
const struct xgpu_scene_static_set *xgpu_vulkan_scene_static_set(void);

/* each surface's level now (a bit a surface, set where drawn far; words of
them), and a number that changes whenever they do (0: none received) */
const uint32_t *xgpu_vulkan_scene_levels(uint32_t *words, uint32_t *serial);

/* SCENE_LIGHTS played: the frame's light table, whole once its last
record came */
void xgpu_vulkan_scene_lights(const struct xgpu_stream_scene_lights *record);

/* the last whole light table (count lights; NULL none), and a number that
moves with each table */
const struct xgpu_stream_scene_light *xgpu_vulkan_scene_light_table(uint32_t *count, uint32_t *serial);

#endif
