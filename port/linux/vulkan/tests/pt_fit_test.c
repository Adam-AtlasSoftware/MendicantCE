/*
PT_FIT_TEST.C

xgpu_vulkan_pt_fit.h on the CPU (tools/test_prism_pt_fit.py): Path Traced's
light grid where the card has no room for its images. A made-up card's
memory: each try's images take their bytes (the window's own and the light
grid's, as images_make makes them), a failed try's held until PT_FIT_WAIT
frames later, as the renderer's garbage holds them after the submission;
what can be put aside for them (the bake's light) freed as late.
*/

#include "../xgpu_vulkan_pt_fit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

/* the card: room in bytes; images_make's bytes for a window and a grid
(25 RGBA16F and 10 RGBA32 images at the grid, the output and, with a coarser
grid, 4 RGBA16F at the window's size) */
struct card
{
	double room, made, held;
	uint32_t width, height;
	int tries;
	uint32_t last_low_width, last_low_height;
	/* what can be put aside for the images (the bake's light, which Path
	Traced never reads: xgpu_vulkan_bake_park), and what was, freed with the
	held */
	double parkable, parking;
	int parks;
};

static double bytes_of(uint32_t width, uint32_t height, uint32_t low_width, uint32_t low_height)
{
	double window = (double)width * height, grid = (double)low_width * low_height;
	int low = low_width != width || low_height != height;

	return window * 8.0 + (low ? 4.0 * window * 8.0 : 0.0) + grid * (25.0 * 8.0 + 10.0 * 16.0);
}

static int made(void *context, uint32_t low_width, uint32_t low_height)
{
	struct card *card = (struct card *)context;
	double need = bytes_of(card->width, card->height, low_width, low_height);

	card->tries++;
	card->last_low_width = low_width;
	card->last_low_height = low_height;
	/* (images_made: no room in the budget, what can be put aside is, the
	same grid tried once it is freed) */
	if (need > card->room - card->held && card->parkable > 0.0)
	{
		card->parking += card->parkable;
		card->parkable = 0.0;
		card->parks++;
		return PT_FIT_FREEING;
	}
	if (need <= card->room - card->held)
	{
		card->made = need;
		return PT_FIT_MADE;
	}
	/* (a failed try's images held until the GPU is past the submission) */
	card->held += need * 0.5;
	return PT_FIT_REFUSED;
}

/* one frame: what the renderer's garbage held let go of after a few frames */
static float frame(struct pt_fit_state *state, struct card *card, float asked, int low_allowed, int *changed, int *age)
{
	float scale;

	if ((card->held > 0.0 || card->parking > 0.0) && ++*age >= 3)
	{
		card->held = 0.0;
		card->room += card->parking;
		card->parking = 0.0;
		*age = 0;
	}
	scale = pt_fit_choose(state, card->width, card->height, asked, low_allowed, made, card, changed);
	return scale;
}

/* frames until a scale is chosen (-1 none within the frames) */
static float settle(struct pt_fit_state *state, struct card *card, float asked, int low_allowed, int frames,
	int *said)
{
	int changed, age = 0, index;
	float scale = -1.0f;

	*said = 0;
	for (index = 0; index < frames; index++)
	{
		scale = frame(state, card, asked, low_allowed, &changed, &age);
		*said += changed;
		if (scale >= 0.0f)
			return scale;
	}
	return scale;
}

int main(void)
{
	struct pt_fit_state state = { 0 };
	struct card card = { 0 };
	double full = bytes_of(3440, 1440, 3440, 1440);
	int said, changed, age = 0, index;
	float scale;
	uint32_t w, h;

	/* the grid's size: at least 16, at most the window's */
	pt_fit_low_size(3440, 1440, 0.5f, &w, &h);
	CHECK(w == 1720 && h == 720, "half of 3440x1440 is %ux%u", w, h);
	pt_fit_low_size(40, 30, 0.25f, &w, &h);
	CHECK(w == 16 && h == 16, "a tiny window's grid %ux%u", w, h);

	/* room for every pixel: the grid asked, nothing said */
	card.width = 3440;
	card.height = 1440;
	card.room = full * 1.2;
	scale = settle(&state, &card, 0.0f, 1, 4, &said);
	CHECK(scale == 0.0f && said == 0 && card.tries == 1, "room: scale %.2f said %d tries %d", scale, said, card.tries);

	/* Adam's 3070 Ti at 3440x1440 beside the enhanced world: 1.8 GB do not fit, the half grid does; CE's frame only
	until it is found, said once */
	memset(&state, 0, sizeof(state));
	card.room = full * 0.6;
	card.tries = 0;
	card.held = 0.0;
	scale = settle(&state, &card, 0.0f, 1, 40, &said);
	CHECK(scale == 0.5f && said == 1, "short: scale %.2f said %d", scale, said);
	CHECK(card.last_low_width == 1720 && card.last_low_height == 720, "short: grid %ux%u", card.last_low_width,
		card.last_low_height);
	/* (kept: never the full grid tried again at this size, nothing said again) */
	card.tries = 0;
	for (index = 0; index < 200; index++)
	{
		scale = frame(&state, &card, 0.0f, 1, &changed, &age);
		CHECK(scale == 0.5f && !changed, "kept: frame %d scale %.2f changed %d", index, scale, changed);
	}
	CHECK(card.last_low_width == 1720, "kept: grid %u", card.last_low_width);

	/* another window's size asks again from the grid asked */
	card.width = 1920;
	card.height = 1080;
	card.room = full;
	card.held = 0.0;
	scale = settle(&state, &card, 0.0f, 1, 4, &said);
	CHECK(scale == 0.0f && said == 0, "new size: scale %.2f said %d", scale, said);

	/* still shorter: the next coarser grids in turn, each try waiting for the failed one's images to go */
	memset(&state, 0, sizeof(state));
	card.width = 3440;
	card.height = 1440;
	card.room = bytes_of(3440, 1440, 860, 360) * 1.01;
	card.held = 0.0;
	scale = settle(&state, &card, 0.0f, 1, 200, &said);
	CHECK(scale == 0.25f && said == 1, "shorter: scale %.2f said %d", scale, said);

	/* display.pt_light_scale asked coarser than a step: the steps finer than it never tried */
	memset(&state, 0, sizeof(state));
	card.room = bytes_of(3440, 1440, 860, 360) * 1.01;
	card.held = 0.0;
	card.tries = 0;
	scale = settle(&state, &card, 0.4f, 1, 200, &said);
	CHECK(scale == 0.25f, "asked 0.4: scale %.2f", scale);
	CHECK(card.tries == 3, "asked 0.4: %d tries (0.4, 0.35, 0.25)", card.tries);

	/* no coarser grid allowed (the denoiser off, Ray Reconstruction, the reference): CE's frame, said once, tried
	again later */
	memset(&state, 0, sizeof(state));
	card.room = full * 0.6;
	card.held = 0.0;
	card.tries = 0;
	scale = settle(&state, &card, 0.0f, 0, 300, &said);
	CHECK(scale < 0.0f && said == 1, "not allowed: scale %.2f said %d", scale, said);
	CHECK(card.tries >= 2 && card.tries <= 4, "not allowed: tried %d times in 300 frames", card.tries);
	/* (room again, a lower resolution's or freed memory: it draws) */
	card.room = full * 1.5;
	card.held = 0.0;
	scale = settle(&state, &card, 0.0f, 0, 200, &said);
	CHECK(scale == 0.0f && said == 0, "room again: scale %.2f said %d", scale, said);

	/* the 3070 Ti with the bake's light put aside (2.3 GB, more than the 1.2 GB short): every pixel, never a coarser
	grid, nothing said; one park */
	memset(&state, 0, sizeof(state));
	card.width = 3440;
	card.height = 1440;
	card.room = full * 0.4;
	card.held = card.parking = 0.0;
	card.parkable = full * 1.3;
	card.parks = 0;
	card.tries = 0;
	scale = settle(&state, &card, 0.0f, 1, 40, &said);
	CHECK(scale == 0.0f && said == 0 && card.parks == 1, "parked: scale %.2f said %d parks %d", scale, said, card.parks);
	CHECK(card.last_low_width == 3440 && card.last_low_height == 1440, "parked: grid %ux%u", card.last_low_width,
		card.last_low_height);
	/* (too little to put aside: the next coarser grid after it, said once) */
	memset(&state, 0, sizeof(state));
	card.room = full * 0.4;
	card.held = card.parking = 0.0;
	card.parkable = full * 0.1;
	scale = settle(&state, &card, 0.0f, 1, 200, &said);
	CHECK(scale == 0.5f && said == 1, "parked, still short: scale %.2f said %d", scale, said);
	/* the bake's light put aside only once it is whole (still loading at first): the coarser grid meanwhile, then
	the grid asked again (pt_fit_retry) once it is put aside, and kept */
	memset(&state, 0, sizeof(state));
	card.room = full * 0.6;
	card.held = card.parking = card.parkable = 0.0;
	scale = settle(&state, &card, 0.0f, 1, 40, &said);
	CHECK(scale == 0.5f, "loading: scale %.2f", scale);
	CHECK(!pt_fit_retry(&(struct pt_fit_state){ 0 }), "retry with nothing chosen");
	card.room += full * 1.0; /* (put aside: xgpu_vulkan_bake_park, freed after the submission) */
	CHECK(pt_fit_retry(&state), "retry after a coarser grid");
	scale = settle(&state, &card, 0.0f, 1, 40, &said);
	CHECK(scale == 0.0f && said == 0, "retried: scale %.2f said %d", scale, said);
	for (index = 0; index < 50; index++)
	{
		scale = frame(&state, &card, 0.0f, 1, &changed, &age);
		CHECK(scale == 0.0f && !changed, "retried, kept: frame %d scale %.2f", index, scale);
	}

	/* nothing fits at any grid: CE's frame, said once, the coarsest tried again now and then */
	memset(&state, 0, sizeof(state));
	card.room = 1.0;
	card.held = card.parking = card.parkable = 0.0;
	scale = settle(&state, &card, 0.0f, 1, 600, &said);
	CHECK(scale < 0.0f && said == 1, "none: scale %.2f said %d", scale, said);

	if (failures)
	{
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
