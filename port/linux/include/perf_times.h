/*
PERF_TIMES.H

The last frames' times as the port measures them (port/linux/src/report.c,
from the frames Save Report keeps), for the game's code: the performance
overlay and the console's rasterizer_stats 3.
*/

#ifndef __PERF_TIMES_H
#define __PERF_TIMES_H

/* averaged over the frames counted (milliseconds a frame): the frame's, the
GPU's as the backend measures it (negative where it measures none), the
renderer's thread's and the game's waits for the renderer and the display;
frames, how many */
struct perf_times
{
	unsigned long frames;
	float frame_ms, gpu_ms, render_ms, waited_ms;
};

/* ... of the frames that ended within milliseconds of the newest's end, at
most most of them */
void perf_times_recent(unsigned long most, unsigned long milliseconds, struct perf_times *times);

#endif
