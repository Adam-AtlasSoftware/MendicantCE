/*
PERF_OVERLAY.C

The performance overlay (display.perf_overlay, off by default): "simple" is
the game's frame rate counter at the screen's bottom right (main.c's
main_framerate_render, which the Xbox version's display_framerate console
variable drew); "detailed" adds lines above it with the times the port
already measures for Save Report (port/linux/src/report.c, perf_times.h):
the frame's time, the 1% low, and a frame's milliseconds on the GPU (the
backend's timestamps), on the renderer's thread, waiting for the display
and on the game's thread (the frame less its waits).

The averages are of the last half second and the 1% low of the last 10
seconds (at most PERF_TIMES_MOST frames), read again every half second so
the numbers can be read. The setting is read when it changes
(config_changes), so a menu's change shows in the next frame.
*/

#include "cseries.h"
#include "perf_overlay.h"
#include "perf_times.h"

#include <stdio.h>
#include <string.h>

/* port_config.c's */
const char *config_string(const char *name);
unsigned long config_changes(void);
/* the engine's clock (source/cseries/cseries_windows.c) */
unsigned long system_milliseconds(void);

/* the averages' frames, the 1% low's, and how often they are read again */
#define PERF_OVERLAY_AVERAGE_MILLISECONDS 500
#define PERF_OVERLAY_LOW_MILLISECONDS 10000

short perf_overlay_mode(
	void)
{
	static short mode = _perf_overlay_off;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		const char *word = config_string("display.perf_overlay");

		read_at = config_changes();
		mode = !strcmp(word, "detailed") ? _perf_overlay_detailed :
			!strcmp(word, "simple") ? _perf_overlay_simple : _perf_overlay_off;
	}
	return mode;
}

short perf_overlay_lines(
	char lines[PERF_OVERLAY_LINES][PERF_OVERLAY_LINE_SIZE])
{
	static char kept[PERF_OVERLAY_LINES][PERF_OVERLAY_LINE_SIZE];
	static unsigned long read_ms;
	static short count;
	unsigned long now = system_milliseconds();

	if (!count || now - read_ms >= PERF_OVERLAY_AVERAGE_MILLISECONDS)
	{
		struct perf_times average, low;

		read_ms = now;
		perf_times_recent(PERF_TIMES_MOST, PERF_OVERLAY_AVERAGE_MILLISECONDS, &average);
		perf_times_recent(PERF_TIMES_MOST, PERF_OVERLAY_LOW_MILLISECONDS, &low);
		count = 0;
		/* (top to bottom, the frame rate's counter below the last) */
		snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "FRAME %.1f ms", average.frame_ms);
		snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "1%% LOW %.0f fps", low.low_fps);
		if (average.gpu_ms >= 0.0f)
			snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "GPU %.1f ms", average.gpu_ms);
		else
			snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "GPU --");
		snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "RENDER %.1f ms", average.render_ms);
		snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "WAIT %.1f ms", average.waited_ms);
		snprintf(kept[count++], PERF_OVERLAY_LINE_SIZE, "GAME %.1f ms",
			average.frame_ms > average.waited_ms ? average.frame_ms - average.waited_ms : 0.0f);
	}
	memcpy(lines, kept, sizeof(kept));
	return count;
}
