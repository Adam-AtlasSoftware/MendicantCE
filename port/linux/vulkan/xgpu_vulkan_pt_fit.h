/*
XGPU_VULKAN_PT_FIT.H

PRISM Path Traced's light grid where the card has no room for its images
(xgpu_vulkan_pt.c's images_fit): the grid asked (0, the G-buffer's pixels,
or display.pt_light_scale's), else the next coarser of
PT_FIT_SCALES where the light may be traced on a coarser grid (not with the
denoiser off, under Ray Reconstruction or for the reference), one try a
frame, the grid a shortage chose kept for the window's size (never tried
finer again, so that the frames do not hunt), -1 where none fits yet: CE's
frame. An 8 GB card at
3440x1440 had no room for the 1.8 GB of the light's images at every pixel
beside the enhanced world's scene (b30, after Classic -> Path Traced with
World Geometry Enhanced): every one refused, and the frame was CE's,
unlit. Before a coarser grid, the memory Path Traced never reads is put
aside (made() says PT_FIT_FREEING: the bake's light moved to the host's
memory) and the same grid tried again; once memory is freed after a coarser
grid was chosen, pt_fit_retry tries the grid asked again. Pure C, no
Vulkan: port/linux/vulkan/tests/pt_fit_test.c.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_PT_FIT_H
#define __HALO_LINUX_XGPU_VULKAN_PT_FIT_H

#include <stdint.h>

/* the coarser grids tried, finest first (light_scale_of's floor is 0.25) */
#define PT_FIT_STEPS 3
static const float pt_fit_scales[PT_FIT_STEPS] = { 0.5f, 0.35f, 0.25f };

/* the light grid's size of a scale (> 0) of the G-buffer's width x height:
at least 16 a side, at most the G-buffer's */
static inline void pt_fit_low_size(uint32_t width, uint32_t height, float scale, uint32_t *low_width,
	uint32_t *low_height)
{
	uint32_t w = (uint32_t)((float)width * scale + 0.5f), h = (uint32_t)((float)height * scale + 0.5f);

	*low_width = w < 16u ? 16u : w > width ? width : w;
	*low_height = h < 16u ? 16u : h > height ? height : h;
}

/* frames a failed try waits before the next (its images are let go of
after the submission being recorded, which the next try must not count on),
and before the coarsest is tried again once nothing fits */
#define PT_FIT_WAIT 8
#define PT_FIT_WAIT_NONE 120

/* what a shortage chose for which window: the coarser grid's step (0 the
grid asked, else pt_fit_scales[step - 1]), the frames to wait, the step last
said, and whether the coarsest failed too */
struct pt_fit_state
{
	uint32_t width, height;
	int step, wait, said, none;
};

/* what made() says of a try: every image of the grid made; none (a failed
try lets go of what it made); or none yet, memory put aside for them that is
freed after the submission (the bake's light, xgpu_vulkan_bake_park) */
#define PT_FIT_REFUSED 0
#define PT_FIT_MADE 1
#define PT_FIT_FREEING 2

/* the scale whose images make this frame (made(context, low_width,
low_height): PT_FIT_*), for the window and the scale asked (0, the G-buffer's
pixels), or -1 (CE's frame this frame): one try a frame, the grid asked
first, after a failure the next coarser one where the light may be traced
coarser (low_allowed), PT_FIT_WAIT frames later; the grid found kept for
the window's size (never tried finer again, so that the frames do not
hunt). *changed 1 where a shortage chose a grid not said before, or where
the coarsest failed (state->none) */
static inline float pt_fit_choose(struct pt_fit_state *state, uint32_t width, uint32_t height, float asked,
	int low_allowed, int (*made)(void *context, uint32_t low_width, uint32_t low_height), void *context, int *changed)
{
	float scale = asked;
	uint32_t low_width = width, low_height = height;
	int next, result;

	*changed = 0;
	if (state->width != width || state->height != height)
	{
		state->width = width;
		state->height = height;
		state->step = state->wait = state->said = state->none = 0;
	}
	if (state->wait > 0)
	{
		state->wait--;
		return -1.0f;
	}
	if (low_allowed && state->step > 0 && (asked <= 0.0f || pt_fit_scales[state->step - 1] < asked))
		scale = pt_fit_scales[state->step - 1];
	if (scale > 0.0f)
		pt_fit_low_size(width, height, scale, &low_width, &low_height);
	result = made(context, low_width, low_height);
	if (result == PT_FIT_MADE)
	{
		*changed = scale != asked && state->said != state->step;
		state->said = state->step;
		state->none = 0;
		return scale;
	}
	/* (memory put aside for it, freed after the submission: the same grid
	again once it is) */
	if (result == PT_FIT_FREEING)
	{
		state->wait = PT_FIT_WAIT;
		return -1.0f;
	}
	/* (the next grid coarser than the one that failed) */
	for (next = state->step + 1; low_allowed && next <= PT_FIT_STEPS && pt_fit_scales[next - 1] >= scale &&
		scale > 0.0f; next++)
		;
	if (low_allowed && next <= PT_FIT_STEPS)
	{
		state->step = next;
		state->wait = PT_FIT_WAIT;
	}
	else
	{
		*changed = !state->none;
		state->none = 1;
		state->wait = PT_FIT_WAIT_NONE;
	}
	return -1.0f;
}

/* memory freed since a shortage chose a coarser grid (or none): the grid
asked tried again, PT_FIT_WAIT frames from now (once what was freed is),
and a later shortage said again; 1 where it had chosen one */
static inline int pt_fit_retry(struct pt_fit_state *state)
{
	if (!state->step && !state->none)
		return 0;
	state->step = state->none = state->said = 0;
	state->wait = PT_FIT_WAIT;
	return 1;
}

#endif
