/*
XGPU_VULKAN_TERRAIN_PRINTS.H

The terrain overhaul's prints (T4: docs/prism_terrain.md; overhaul_research/
prism_v4_renderer/terrain/design.md sections 2.3, 2.4 and 4): the feet of
PRISM_TERRAIN's events pressed into a sparse pool of world-space pages of
the ground's height (a page 126 texels of 2 cm with a texel of apron about
it), relaxed toward the ground's angle of repose and filled in over minutes
in the wind, and handed to the terrain pass (xgpu_vulkan_terrain.c), which
draws them by parallax with the sun's micro-shadow. Render only: nothing
here reaches the game. Its own time is the game's (FRAME's step, 0 while
paused), so the same frames make the same prints.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_TERRAIN_PRINTS_H
#define __HALO_LINUX_XGPU_VULKAN_TERRAIN_PRINTS_H

#include <stdint.h>

#include "../src/xgpu_stream.h"

struct xgpu_vulkan_image;

/* a page's side in texels (an apron texel each side: PRINT_PAGE_INNER of
its own), the texel's metres, the window of the page table about the
camera (pages a side) */
#define PRINT_PAGE_TEXELS 128u
#define PRINT_PAGE_INNER 126u
#define PRINT_TEXEL_METRES 0.02f
#define PRINT_WINDOW 64u
#define PRINT_LEVELS 3u

/* what the terrain pass is given: the pool (a 2D array, RGBA16F: the
displacement in world units, the compaction, the ground's height over the
page's own times the coverage, the coverage), the page table (PRINT_WINDOW
squared RGBA32_UINT: the page's pool layer + 1, its x and y, its height's
float bits) and the pass's four vec4s (xgpu_vulkan_post_uniforms' prints) */
struct terrain_prints_draw
{
	struct xgpu_vulkan_image *pool, *table;
	float values[4][4];
};

/* every page forgotten (a new generation) */
void terrain_prints_forget(void);
/* an event of the frame (feet are stamped; the rest wait for T5) */
void terrain_prints_event(const struct xgpu_stream_terrain_event *event);
/* the frame's waders (a wet foot's print is a wet print) */
void terrain_prints_waders(const struct xgpu_stream_terrain_wader *waders, uint32_t count);
/* the frame: the game's step (seconds, 0 paused), the camera, the wind at
the eye; the pages relaxed and filled in */
void terrain_prints_frame(float game_seconds, const float camera[3], const float wind[3]);
/* the pool and table made and brought up to date about the eye; 1 where a
page lies in the window (draw filled), 0 where nothing is to draw */
int terrain_prints_prepare(const float eye[3], const float sun[4], struct terrain_prints_draw *draw);
/* (for the log and the tests) the pages in use, the stamps made */
void terrain_prints_counts(uint32_t *pages, uint32_t *stamps);

#endif
