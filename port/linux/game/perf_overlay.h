/*
PERF_OVERLAY.H

The performance overlay at the screen's bottom right (display.perf_overlay:
perf_overlay.c), drawn by main.c's main_framerate_render.
*/

#ifndef __PERF_OVERLAY_H
#define __PERF_OVERLAY_H

enum
{
	_perf_overlay_off = 0,
	/* the frame rate's counter */
	_perf_overlay_simple,
	/* ... and the frame's times */
	_perf_overlay_detailed
};

enum
{
	/* the frame's times (6); with debug.timings the busiest CPU sections,
	the GPU's passes and the counts too */
	PERF_OVERLAY_LINES = 16,
	PERF_OVERLAY_LINE_SIZE = 32,
	PERF_OVERLAY_CPU_SECTIONS = 4,
	PERF_OVERLAY_GPU_GROUPS = 4
};

/* the setting's mode (_perf_overlay_*), read again when it changes */
short perf_overlay_mode(void);
/* "detailed"'s lines, top to bottom, above the counter: their count */
short perf_overlay_lines(char lines[PERF_OVERLAY_LINES][PERF_OVERLAY_LINE_SIZE]);
/* debug.timings (off by default), read again when it changes */
int perf_overlay_timings(void);

#endif
