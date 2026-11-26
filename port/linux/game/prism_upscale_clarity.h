/*
PRISM_UPSCALE_CLARITY.H

display.pt_clarity's mapping (prism_upscale.c's settings_read, Path Traced
with its own denoiser under an upscaler): from the setting and
display.upscaler_scale (already in its range) to the G-buffer's share of
the window each way and the share of that G-buffer the light is traced at.
Alone in this header so that tools/test_upscale.py can run it without the
game.
*/

#ifndef __PRISM_UPSCALE_CLARITY_H
#define __PRISM_UPSCALE_CLARITY_H

#include <string.h>

/* "auto": the least share each way the path tracer's G-buffer is drawn at,
the light traced at display.upscaler_scale's share of the screen below it;
"max": the light's share of a G-buffer at every pixel */
#define PT_CLARITY_GBUFFER_LEAST 0.5f
#define PT_CLARITY_MAX_LIGHT 0.5f

/* the G-buffer's scale (*scale, display.upscaler_scale's on the way in) and
the light's share of it (returned; 0: every pixel, or display.pt_light_scale
where that is set). "off", unknown or NULL: the scale as it is, no share */
static float prism_upscale_clarity(char const *clarity, float *scale)
{
	if (clarity && !strcmp(clarity, "max"))
	{
		*scale = 1.0f;
		return PT_CLARITY_MAX_LIGHT;
	}
	if (clarity && !strcmp(clarity, "auto") && *scale < PT_CLARITY_GBUFFER_LEAST)
	{
		float share = *scale / PT_CLARITY_GBUFFER_LEAST;

		*scale = PT_CLARITY_GBUFFER_LEAST;
		return share;
	}
	return 0.0f;
}

#endif
