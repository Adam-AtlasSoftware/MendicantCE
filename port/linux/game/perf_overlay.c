/*
PERF_OVERLAY.C

The performance overlay (display.perf_overlay, off by default): "simple" is
the game's frame rate counter at the screen's bottom right (main.c's
main_framerate_render, which the Xbox version's display_framerate console
variable drew); "detailed" adds cells above it with the times the port
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
of the last 120 frames, by short labels (perf_overlay_section_label; the
developer console keeps their names), the GPU's passes by the setting that
owns them, and the counts of the objects, effects and particles alive, the
pipelines defined and the draws. Off, nothing is timed: the sections test
profile_global_enable, which stays FALSE, as the original's did. The Vulkan
backend times the passes from the frame after the setting turns on (the
render stream's PASS_TIMES record: d3d8_device.c's pass_times_send) until
it turns off.

The cells are drawn two to a row, each group from a row of its own, a label
at the left and its value at the right, every time in milliseconds with two
decimals (perf_overlay_milliseconds). perf_overlay_place puts them in the
view's bottom right corner above the counter, scaled to be compact and
still readable from 720p to 4K and in split screen.
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

/* ---------- the cells */

/* the CPU sections' labels: a name, or a name's start ending in '*'; any
other is its last part (after a '.') in capitals, '_' as spaces, cut */
static struct
{
	char const *name;
	char const *label;
} const perf_overlay_section_labels[] =
{
	{ "game_time_update", "GAME TICK" },
	{ "game_update", "SIMULATION" },
	{ "game_frame", "GAME FRAME" },
	{ "render_objects", "OBJECTS" },
	{ "render_object_shadows", "OBJ SHADOWS" },
	{ "render_model", "MODELS" },
	{ "render_lights", "LIGHTS" },
	{ "render_interpolation_tick", "INTERPOLATE" },
	{ "render_structure_visibility_*", "VISIBILITY" },
	{ "render_structure_shadows*", "BSP SHADOWS" },
	{ "render_structure_lightmaps", "LIGHTMAPS" },
	{ "render_structure_fog", "FOG" },
	{ "render_structure_reflections", "REFLECTIONS" },
	{ "render_structure_transparent_geometry", "TRANSPARENT" },
	{ "render_structure_*", "LEVEL" },
	{ "present", "PRESENT" },
	{ "input", "INPUT" },
	{ "throttle", "THROTTLE" },
	{ "ui_update", "MENUS" },
	{ "platform_events", "EVENTS" },
	{ "main_update_time", "CLOCK" },
	{ "non_deterministic_update", "LOCAL UPDATE" },
	{ "network_start_frame", "NET RECEIVE" },
	{ "network_end_frame", "NET SEND" },
	{ "network_distributed_tick", "NET TICK" },
	{ "network_*", "NETWORK" },
	{ "sound_render", "SOUND" },
	{ "ai_update", "AI" },
	{ "game_tick.ai", "AI" },
	{ "game_tick.actors_drive", "AI DRIVING" },
	{ "objects_update", "OBJECT TICK" },
	{ "game_tick.objects", "OBJECT TICK" },
	{ "effects_update", "EFFECTS" },
	{ "game_tick.effects", "EFFECTS" },
	{ "particles_update", "PARTICLES" },
	{ "unit_update", "UNITS" },
	{ "game_tick.units", "UNITS" },
	{ "biped_update", "BIPEDS" },
	{ "vehicle_update", "VEHICLES" },
	{ "weapon_update", "WEAPONS" },
	{ "item_update", "ITEMS" },
	{ "projectile_update", "PROJECTILES" },
	{ "player_control_update", "CONTROLS" },
	{ "players_update_*", "PLAYERS" },
	{ "game_tick.players_*", "PLAYERS" },
	{ "game_tick.hs", "SCRIPTS" },
	{ "game_tick.hud", "HUD" },
	{ "game_tick.game_engine", "GAME RULES" },
	{ "game_tick.first_person", "FIRST PERSON" },
};

/* (a label made of a name the table has not: its letters at most) */
#define PERF_OVERLAY_SECTION_LETTERS 12

void perf_overlay_section_label(
	char const *name,
	char *label,
	short size)
{
	char const *last;
	short index, length = 0;

	if (size <= 0)
		return;
	if (!name)
		name = "";
	for (index = 0; index < (short)NUMBEROF(perf_overlay_section_labels); index++)
	{
		char const *pattern = perf_overlay_section_labels[index].name;
		size_t pattern_length = strlen(pattern);
		int found = pattern[pattern_length - 1] == '*' ?
			!strncmp(name, pattern, pattern_length - 1) : !strcmp(name, pattern);

		if (found)
		{
			snprintf(label, (size_t)size, "%s", perf_overlay_section_labels[index].label);
			return;
		}
	}
	last = strrchr(name, '.');
	last = last ? last + 1 : name;
	for (; *last && length < size - 1 && length < PERF_OVERLAY_SECTION_LETTERS; last++)
		label[length++] = *last == '_' ? ' ' : (char)toupper((unsigned char)*last);
	while (length > 0 && label[length - 1] == ' ')
		length--;
	label[length] = 0;
}

void perf_overlay_milliseconds(
	real milliseconds,
	char *value,
	short size)
{
	if (milliseconds < 0.0f)
		snprintf(value, (size_t)size, "--");
	else
		snprintf(value, (size_t)size, "%.2f ms", milliseconds);
}

/* the next cell, labeled, its value empty (NULL past the last) */
static struct perf_overlay_cell *perf_overlay_cell(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS],
	short *count,
	char const *label)
{
	struct perf_overlay_cell *cell;

	if (*count >= PERF_OVERLAY_CELLS)
		return NULL;
	cell = &cells[(*count)++];
	snprintf(cell->label, sizeof(cell->label), "%s", label);
	cell->value[0] = 0;
	return cell;
}

/* a group's end: its last row's other half left empty, so that the next
group starts a row of its own */
static void perf_overlay_group_end(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS],
	short *count)
{
	if (*count % PERF_OVERLAY_COLUMNS)
		perf_overlay_cell(cells, count, "");
}

static void perf_overlay_time_cell(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS],
	short *count,
	char const *label,
	real milliseconds)
{
	struct perf_overlay_cell *cell = perf_overlay_cell(cells, count, label);

	if (cell)
		perf_overlay_milliseconds(milliseconds, cell->value, (short)sizeof(cell->value));
}

static void perf_overlay_count_cell(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS],
	short *count,
	char const *label,
	long number)
{
	struct perf_overlay_cell *cell = perf_overlay_cell(cells, count, label);

	if (cell)
		snprintf(cell->value, sizeof(cell->value), "%ld", number);
}

/* (debug.timings) the busiest CPU sections, the GPU's passes by owner and
the counts, from cell count on: the next cell */
static short perf_overlay_timing_cells(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS],
	short count)
{
	static char log[16 * 1024];
	char const *names[PERF_OVERLAY_CPU_SECTIONS];
	real milliseconds[PERF_OVERLAY_CPU_SECTIONS], calls[PERF_OVERLAY_CPU_SECTIONS];
	char labels[PERF_OVERLAY_GPU_GROUPS][PERF_OVERLAY_LINE_SIZE];
	real pass_milliseconds[PERF_OVERLAY_GPU_GROUPS];
	char label[PERF_OVERLAY_LABEL_SIZE];
	short sections = profile_port_busiest(names, milliseconds, calls, PERF_OVERLAY_CPU_SECTIONS);
	long draws = -1;
	short groups, index;

	for (index = 0; index < sections; index++)
	{
		perf_overlay_section_label(names[index], label, (short)sizeof(label));
		perf_overlay_time_cell(cells, &count, label, milliseconds[index]);
	}
	perf_overlay_group_end(cells, &count);
#ifdef HALO_GUEST
	host_log_recent(log, sizeof(log));
#endif
	groups = perf_overlay_pass_groups(log, labels, pass_milliseconds, PERF_OVERLAY_GPU_GROUPS, &draws);
	for (index = 0; index < groups; index++)
	{
		snprintf(label, sizeof(label), "GPU %.15s", labels[index]);
		perf_overlay_time_cell(cells, &count, label, pass_milliseconds[index]);
	}
	/* (the Metal backend's, or none timed yet: the first summary comes 300
	frames after the passes are first timed) */
	if (!groups)
		perf_overlay_time_cell(cells, &count, "GPU PASSES", -1.0f);
	perf_overlay_group_end(cells, &count);
	perf_overlay_count_cell(cells, &count, "LIVE OBJECTS", perf_overlay_alive(object_header_data));
	perf_overlay_count_cell(cells, &count, "LIVE EFFECTS", perf_overlay_alive(effect_data));
	perf_overlay_count_cell(cells, &count, "PARTICLES", perf_overlay_alive(particle_data));
	perf_overlay_count_cell(cells, &count, "PIPELINES", (long)xgpu_backend_pipelines_defined());
	if (draws >= 0)
		perf_overlay_count_cell(cells, &count, "DRAWS", draws);
	perf_overlay_group_end(cells, &count);
	return count;
}

short perf_overlay_cells(
	struct perf_overlay_cell cells[PERF_OVERLAY_CELLS])
{
	static struct perf_overlay_cell kept[PERF_OVERLAY_CELLS];
	static unsigned long read_ms;
	static short count;
	unsigned long now = system_milliseconds();

	if (!count || now - read_ms >= PERF_OVERLAY_AVERAGE_MILLISECONDS)
	{
		struct perf_times average, low;
		struct perf_overlay_cell *cell;

		read_ms = now;
		perf_times_recent(PERF_TIMES_MOST, PERF_OVERLAY_AVERAGE_MILLISECONDS, &average);
		perf_times_recent(PERF_TIMES_MOST, PERF_OVERLAY_LOW_MILLISECONDS, &low);
		count = 0;
		perf_overlay_time_cell(kept, &count, "FRAME", average.frame_ms);
		if ((cell = perf_overlay_cell(kept, &count, "1% LOW")) != NULL)
			snprintf(cell->value, sizeof(cell->value), "%.0f fps", low.low_fps);
		perf_overlay_time_cell(kept, &count, "GPU", average.gpu_ms >= 0.0f ? average.gpu_ms : -1.0f);
		perf_overlay_time_cell(kept, &count, "RENDER", average.render_ms);
		perf_overlay_time_cell(kept, &count, "WAIT", average.waited_ms);
		perf_overlay_time_cell(kept, &count, "GAME",
			average.frame_ms > average.waited_ms ? average.frame_ms - average.waited_ms : 0.0f);
		if (perf_overlay_timings())
			count = perf_overlay_timing_cells(kept, count);
	}
	memcpy(cells, kept, sizeof(kept));
	return count;
}

/* ---------- the cells' place */

/* the text's scale in one view and in a split screen's; larger where the
capitals would be fewer display pixels tall than this (720p, a split
screen's small views); the least it is made to fit, under which the bottom
rows are left out instead */
#define PERF_OVERLAY_SCALE 0.45f
#define PERF_OVERLAY_SPLIT_SCALE 0.45f
#define PERF_OVERLAY_CAPITAL_PIXELS 9.0f
#define PERF_OVERLAY_SCALE_LEAST 0.3f
/* the most of the view's width and height the box takes */
#define PERF_OVERLAY_WIDTH_SHARE 0.45f
#define PERF_OVERLAY_HEIGHT_SHARE 0.6f
/* units (before the scale, but the corner's): from the view's right edge
and its bottom (the counter's room), between rows' capitals, between the
columns */
#define PERF_OVERLAY_RIGHT 14
#define PERF_OVERLAY_BOTTOM 52
#define PERF_OVERLAY_ROW_GAP 7
#define PERF_OVERLAY_COLUMN_GAP 16

void perf_overlay_place(
	short x0,
	short y0,
	short x1,
	short y1,
	real pixels,
	boolean split,
	short cells,
	short cell_width,
	short capital_height,
	struct perf_overlay_place *place)
{
	real scale = split ? PERF_OVERLAY_SPLIT_SCALE : PERF_OVERLAY_SCALE;
	real room_width, room_height, fit;
	short rows = (short)((cells + PERF_OVERLAY_COLUMNS - 1) / PERF_OVERLAY_COLUMNS);

	if (capital_height < 1)
		capital_height = 1;
	if (cell_width < 1)
		cell_width = 1;
	if (pixels > 0.0f && scale * capital_height * pixels < PERF_OVERLAY_CAPITAL_PIXELS)
		scale = PERF_OVERLAY_CAPITAL_PIXELS / (capital_height * pixels);
	if (scale > 1.0f)
		scale = 1.0f;
	place->anchor_x = (short)(x1 - PERF_OVERLAY_RIGHT);
	place->anchor_y = (short)(y1 - PERF_OVERLAY_BOTTOM);
	place->row_height = (short)(capital_height + PERF_OVERLAY_ROW_GAP);
	place->cell_width = cell_width;
	place->column_gap = PERF_OVERLAY_COLUMN_GAP;
	place->width = (short)(PERF_OVERLAY_COLUMNS * cell_width + (PERF_OVERLAY_COLUMNS - 1) * PERF_OVERLAY_COLUMN_GAP);
	room_width = (x1 - x0) * PERF_OVERLAY_WIDTH_SHARE;
	room_height = (y1 - y0) * PERF_OVERLAY_HEIGHT_SHARE;
	if (room_height > place->anchor_y - y0 - 4)
		room_height = (real)(place->anchor_y - y0 - 4);
	if (room_height < 0.0f)
		room_height = 0.0f;
	fit = room_width / place->width;
	if (rows > 0 && room_height / (rows * place->row_height) < fit)
		fit = room_height / (rows * place->row_height);
	if (fit < scale)
		scale = fit;
	/* (too many rows to fit even at the least: the top's that do) */
	if (scale < PERF_OVERLAY_SCALE_LEAST)
	{
		scale = PERF_OVERLAY_SCALE_LEAST;
		if (rows * place->row_height * scale > room_height)
			rows = (short)(room_height / (place->row_height * scale));
	}
	place->scale = scale;
	place->rows = rows;
	place->height = (short)(rows * place->row_height);
}
