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

With debug.timings (off by default) the game's CPU sections are timed: the
original's profile_enter sections and the port's PROFILE_SECTIONs
(profile_sections.h), from the next frame's start (profile.c's
profile_frame_start). "detailed" then adds the sections that took the most
of the last 120 frames, a line each, and the counts of the objects,
effects and particles alive and of the pipelines defined. Off, nothing is
timed: the sections test profile_global_enable, which stays FALSE, as the
original's did.
*/

#include "cseries.h"
#include "cseries/profile.h"
#include "memory/data.h"
#include "perf_overlay.h"
#include "perf_times.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port_config.c's */
const char *config_string(const char *name);
int config_boolean(const char *name);
unsigned long config_changes(void);
/* the engine's clock (source/cseries/cseries_windows.c) */
unsigned long system_milliseconds(void);
/* (debug.timings' counts) the game's data arrays, and the pipelines the
renderer has defined (port/linux/src/xgpu_stream_encode.c) */
extern struct data_array *object_header_data;
extern struct data_array *effect_data;
extern struct data_array *particle_data;
unsigned long xgpu_backend_pipelines_defined(void);
#ifdef HALO_GUEST
/* the host's last lines (port/android/guest/runtime/guest_host.h) */
unsigned int host_log_recent(char *buffer, unsigned int size);
#endif

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

int perf_overlay_timings(
	void)
{
	static int timings;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		timings = config_boolean("debug.timings") != 0;
	}
	return timings;
}

boolean profile_port_timings(
	void)
{
	return perf_overlay_timings() ? TRUE : FALSE;
}

/* a data array's datums alive (0 before it is made) */
static long perf_overlay_alive(
	struct data_array const *array)
{
	return array && array->valid ? array->actual_count : 0;
}

/* (debug.timings) the GPU's passes by the graphics setting that owns them,
from the last of the Vulkan backend's summaries in the host's lines
("vulkan: pass groups: draws N; SMAA 0.093; OTHER 2.100", the most first:
xgpu_vulkan.c's pass_times_groups_log): at most most of them, their labels
and milliseconds a frame, and the draws a frame; how many (0: none found,
as the Metal backend logs none) */
static short perf_overlay_pass_groups(
	char const *log,
	char labels[][PERF_OVERLAY_LINE_SIZE],
	real *milliseconds,
	short most,
	long *draws)
{
	static char const prefix[] = "vulkan: pass groups: draws ";
	char const *found = NULL, *at = log;
	short count = 0;

	while ((at = strstr(at, prefix)) != NULL)
		found = at++;
	if (!found)
		return 0;
	at = found + sizeof(prefix) - 1;
	*draws = strtol(at, NULL, 10);
	while (count < most && (at = strstr(at, "; ")) != NULL)
	{
		char const *end = at + 2;
		char const *space = NULL;
		size_t length;

		/* (a label of words, then its milliseconds, to the next ; or the
		line's end) */
		while (*end && *end != ';' && *end != '\n')
		{
			if (*end == ' ')
				space = end;
			end++;
		}
		if (!space)
			break;
		length = (size_t)(space - (at + 2));
		if (length >= PERF_OVERLAY_LINE_SIZE)
			length = PERF_OVERLAY_LINE_SIZE - 1;
		memcpy(labels[count], at + 2, length);
		labels[count][length] = 0;
		milliseconds[count] = (real)strtod(space + 1, NULL);
		count++;
		if (*end != ';')
			break;
		at = end;
	}
	return count;
}

/* (debug.timings) the busiest CPU sections, a line each, the GPU's passes
by owner, and the counts: the lines written from line on; the next line */
static short perf_overlay_timing_lines(
	char lines[PERF_OVERLAY_LINES][PERF_OVERLAY_LINE_SIZE],
	short line)
{
	char const *names[PERF_OVERLAY_CPU_SECTIONS];
	real milliseconds[PERF_OVERLAY_CPU_SECTIONS], calls[PERF_OVERLAY_CPU_SECTIONS];
	short count = profile_port_busiest(names, milliseconds, calls, PERF_OVERLAY_CPU_SECTIONS);
	short index;

	for (index = 0; index < count && line < PERF_OVERLAY_LINES; index++)
	{
		char *text = lines[line++];
		char *letter;

		/* (the section's name in capitals, as the other lines' words) */
		snprintf(text, PERF_OVERLAY_LINE_SIZE, "%.22s", names[index]);
		for (letter = text; *letter; letter++)
			*letter = (char)toupper((unsigned char)*letter);
		snprintf(letter, (size_t)(PERF_OVERLAY_LINE_SIZE - (letter - text)), " %.2f ms", milliseconds[index]);
	}
	if (line < PERF_OVERLAY_LINES)
		snprintf(lines[line++], PERF_OVERLAY_LINE_SIZE, "OBJ %ld FX %ld PRT %ld", perf_overlay_alive(object_header_data),
			perf_overlay_alive(effect_data), perf_overlay_alive(particle_data));
	{
		static char log[16 * 1024];
		char labels[PERF_OVERLAY_GPU_GROUPS][PERF_OVERLAY_LINE_SIZE];
		real pass_milliseconds[PERF_OVERLAY_GPU_GROUPS];
		long draws = -1;
		short groups;

#ifdef HALO_GUEST
		host_log_recent(log, sizeof(log));
#endif
		groups = perf_overlay_pass_groups(log, labels, pass_milliseconds, PERF_OVERLAY_GPU_GROUPS, &draws);
		for (index = 0; index < groups && line < PERF_OVERLAY_LINES; index++)
			snprintf(lines[line++], PERF_OVERLAY_LINE_SIZE, "GPU %.18s %.2f ms", labels[index], pass_milliseconds[index]);
		/* (the Metal backend's, or none timed yet) */
		if (!groups && line < PERF_OVERLAY_LINES)
			snprintf(lines[line++], PERF_OVERLAY_LINE_SIZE, "GPU PASSES --");
		if (line < PERF_OVERLAY_LINES && draws >= 0)
			snprintf(lines[line++], PERF_OVERLAY_LINE_SIZE, "PIPELINES %lu DRAWS %ld", xgpu_backend_pipelines_defined(),
				draws);
		else if (line < PERF_OVERLAY_LINES)
			snprintf(lines[line++], PERF_OVERLAY_LINE_SIZE, "PIPELINES %lu", xgpu_backend_pipelines_defined());
	}
	return line;
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
		if (perf_overlay_timings())
			count = perf_overlay_timing_lines(kept, count);
	}
	memcpy(lines, kept, sizeof(kept));
	return count;
}
