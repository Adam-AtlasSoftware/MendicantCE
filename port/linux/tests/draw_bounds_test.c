/* The draws' bounds end with the frame (port/linux/src/draw_bounds_state.h):
a light's or a shadow's pass that sets its bounds and never ends (no
draw_bounds_none) clips nothing of the next frame. Built by
tools/test_draw_bounds.py; with DRAW_BOUNDS_TEST_NO_RESET, the present
leaves the bounds as they were, and the test must fail (the negative
control). */
#include <stdio.h>

#include "draw_bounds_state.h"

static struct draw_bounds_state bounds;

/* D3DDevice_Present's part: the frame shown, the next one begun */
static void present(void)
{
#ifndef DRAW_BOUNDS_TEST_NO_RESET
	draw_bounds_state_frame_end(&bounds);
#endif
}

int main(void)
{
	static const float light[4] = { 100.0f, 50.0f, 220.0f, 140.0f };
	int failures = 0;

	/* a pass bounded and ended as the game does: none after it */
	draw_bounds_state_set(&bounds, light);
	if (!bounds.known || bounds.rectangle[2] != 220.0f)
	{
		printf("FAIL: the bounds set are not kept\n");
		failures++;
	}
	draw_bounds_state_set(&bounds, NULL);
	if (bounds.known)
	{
		printf("FAIL: the bounds ended are still kept\n");
		failures++;
	}

	/* a pass bounded that never ends: kept for the rest of its frame... */
	draw_bounds_state_set(&bounds, light);
	if (!bounds.known)
	{
		printf("FAIL: the bounds set are not kept within the frame\n");
		failures++;
	}
	/* ...and gone in the next */
	present();
	if (bounds.known)
	{
		printf("FAIL: a pass's bounds that never ended survive into the next frame\n");
		failures++;
	}
	if (!failures)
		printf("ok\n");
	return failures ? 1 : 0;
}
