/*
BENCHMARK.C

The built-in benchmark: tools/bench.py's scenario, run by the game itself
for a player, with no Python and no source tree. Settings > Video > Frame
Rate & Sync's RUN BENCHMARK (menu_functions.c's benchmark_request) or
halo --benchmark starts it from the main menu.

The scenario is bench.py's: a local game of Slayer on Blood Gulch
(debug.network_test "local:bloodgulch:slayer", network_test.c) with
BENCHMARK_BOTS bots, the network game's seed fixed (debug.random_seed) and
the scripted player (debug.test_input "bot:<seed>", a bot of the player's
own that runs the map and fights), each set for this run alone
(config_set_for_run) and given back at its end. It runs in real time, so
the frames are those a player sees; the game's ticks are the same each run.
display.benchmark_pacing chooses what is measured: "uncapped" turns vsync,
variable refresh and the frame cap off for the run (a cap of
BENCHMARK_UNCAPPED_FPS, not none: a GPU never left to rest can hang,
sdl_platform.c's display.max_fps), what the computer can draw; "settings" keeps them as they
are set, what the player sees as they play. The results say which, and how
much of the time the frames waited for the display all the same (a Mac's
display link, or a system's forced vsync).

The match is guarded before it is measured (benchmark_live_problem): its
game time must advance (a frozen match, or a paused one, is refused at
once), a player and bots must be in it, and someone must have been killed;
the minute begins at the first scenario second from BENCHMARK_WARMUP on
that passes, and the run fails if none has by BENCHMARK_WARMUP_MOST, or if
nobody is killed in the measured minute. The frames are the Save Report's
ring's (report.c's report_frames_mark): from the measure's first to its
last, summed as the report and the performance overlay sum theirs, and
written to benchmarks/<date>_<time>-bloodgulch.txt beside reports/
(report_benchmark_write). The game goes back to the main menu, whose
results screen (menu_functions.c's pc_menu_benchmark_results_open) shows
the average, the 1% low, the 95th and 99th percentile frame times, the
GPU's and the CPU's time a frame, the settings and the computer.
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/players.h"
#include "main/main.h"

#include "benchmark.h"
#include "bots.h"
#include "menu_table.h"

#include "../src/port_config.h" /* port: port/linux/src/port_config.c */
#include "../src/report.h" /* port: port/linux/src/report.c */

#include <stdio.h>
#include <string.h>

void platform_log(char const *format, ...);
unsigned long system_milliseconds(void);
/* network_test.c's */
void network_test_restart(void);
long network_test_cues_start_tick(void);
/* xinput_sdl.c's */
void halo_test_input_reread(void);
/* menu_functions.c's: the results screen opened over the main menu, 1 when
it was (or cannot be) */
int pc_menu_benchmark_results_open(void);
/* sdl_platform.c's */
void platform_video_drawable_size(int *width, int *height);
/* port_config.c's: the value after a command line argument (halo --benchmark <pacing>) */
int config_command_line_value(char const *name, char *value, unsigned long size);
/* xgpu_backend.c's: the GPU's name (its header is the host's, not the game's) */
char const *xgpu_backend_device(void);

#define BENCHMARK_MAP "bloodgulch"
#define BENCHMARK_BOTS "40"
#define BENCHMARK_SEED "1"
#define BENCHMARK_UNCAPPED_FPS "1000"
/* the scenario's seconds before the measure may begin, the most it may
wait for a live match, and the seconds measured */
#define BENCHMARK_WARMUP 5.0
#define BENCHMARK_WARMUP_MOST 40.0
#define BENCHMARK_SECONDS 60.0
/* real seconds a match may take to start (its map loaded), and to be found
frozen: its game time under a quarter of the real time's */
#define BENCHMARK_START_MOST_MS 180000UL
#define BENCHMARK_FROZEN_SECONDS 15.0
/* the share of the time (percent) waiting for vsync or the frame cap the
results call held back (report.c's REPORT_PACED_PERCENT) */
#define BENCHMARK_PACED_PERCENT 20.0

char const *benchmark_live_problem(
	struct benchmark_live const *live,
	int measure)
{
	if (live->real_seconds >= BENCHMARK_FROZEN_SECONDS &&
		(double)live->ticks < live->real_seconds * (double)TICKS_PER_SECOND / 4.0)
	{
		return "the match is frozen: its game time does not advance";
	}
	if (!measure)
		return NULL;
	if (live->players < 1)
		return "no player is in the match";
	if (live->bots < 1)
		return "no bots joined the match";
	if (live->kills < 1)
		return "nobody was killed: the match is not being played";
	return NULL;
}

enum
{
	_benchmark_idle,
	/* asked for, waiting for the main menu */
	_benchmark_asked,
	/* the match being set up and loaded */
	_benchmark_starting,
	/* in it, waiting for the minute's start */
	_benchmark_warming,
	_benchmark_measuring,
	/* over: the main menu's results screen waiting */
	_benchmark_done
};

/* the settings set for the run, and what they were */
#define BENCHMARK_SETTINGS 16
static struct
{
	int state;
	int command_line_checked;
	char pacing[16];
	struct
	{
		char const *name;
		char previous[256];
	} settings[BENCHMARK_SETTINGS];
	short setting_count;
	/* when it was begun (system_milliseconds), when the match began (and
	its tick), and the measure's: its scenario second, its first frame and
	the kills before it */
	unsigned long started_ms, playing_ms;
	long playing_tick;
	int playing;
	double measure_second;
	unsigned long first_frame;
	long kills_before;
	/* the results, or why it failed */
	char failure[160];
	struct report_measure measure;
	char verdict[96];
	char file[160];
	char window[32];
	char processor[160];
} benchmark;

static void benchmark_set(
	char const *name,
	char const *value)
{
	if (benchmark.setting_count < BENCHMARK_SETTINGS &&
		config_text(name, benchmark.settings[benchmark.setting_count].previous,
			sizeof(benchmark.settings[benchmark.setting_count].previous)))
	{
		benchmark.settings[benchmark.setting_count++].name = name;
	}
	config_set_for_run(name, value);
}

static void benchmark_restore(
	void)
{
	short index;

	for (index = benchmark.setting_count; index-- > 0;)
		config_set_for_run(benchmark.settings[index].name, benchmark.settings[index].previous);
	benchmark.setting_count = 0;
	network_test_restart();
	halo_test_input_reread();
}

static void benchmark_begin(
	void)
{
	benchmark.setting_count = 0;
	benchmark_set("debug.network_test", "local:" BENCHMARK_MAP ":slayer");
	benchmark_set("debug.network_test_start", "3");
	benchmark_set("debug.network_test_score", "1000");
	benchmark_set("debug.network_test_players", "1");
	benchmark_set("bots.count", BENCHMARK_BOTS);
	benchmark_set("bots.make_room", "false");
	benchmark_set("bots.fill_to", "0");
	benchmark_set("bots.skill", "normal");
	benchmark_set("debug.test_input", "bot:" BENCHMARK_SEED);
	benchmark_set("debug.random_seed", BENCHMARK_SEED);
	benchmark_set("debug.skip_cinematics", "true");
	if (!strcmp(benchmark.pacing, "uncapped"))
	{
		benchmark_set("display.vsync", "false");
		benchmark_set("display.vrr", "off");
		benchmark_set("display.max_fps", BENCHMARK_UNCAPPED_FPS);
	}
	network_test_restart();
	halo_test_input_reread();
	benchmark.state = _benchmark_starting;
	benchmark.started_ms = system_milliseconds();
	benchmark.playing = FALSE;
	benchmark.failure[0] = 0;
	memset(&benchmark.measure, 0, sizeof(benchmark.measure));
	benchmark.file[0] = benchmark.verdict[0] = 0;
	platform_log("benchmark: begun (%s, pacing %s)", BENCHMARK_MAP, benchmark.pacing);
}

/* the run over: its settings given back and the main menu asked for, its
results (or failure) shown there */
static void benchmark_end(
	char const *failure)
{
	if (failure)
	{
		snprintf(benchmark.failure, sizeof(benchmark.failure), "%s", failure);
		platform_log("benchmark: failed: %s", failure);
	}
	benchmark_restore();
	benchmark.state = _benchmark_done;
	if (!main_menu_is_loaded())
		main_goto_main_menu();
}

static void benchmark_count(
	long *players,
	long *bots,
	long *kills)
{
	struct data_iterator iterator;
	struct player_datum *player;

	*players = *bots = *kills = 0;
	if (!player_data)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (bots_player_is_bot(iterator.datum_index))
			(*bots)++;
		else
			(*players)++;
		*kills += player->statistics.kills[0];
	}
}

/* the results' lines above the frames' (report_benchmark_write's about) */
static void benchmark_about(
	char *text,
	unsigned long size,
	long kills,
	long ticks)
{
	snprintf(text, size,
		"Scenario: %s, Slayer, %s bots, seed %s, the scripted player; %.0f seconds measured from %.1f s of "
		"the match (%ld game ticks, %ld kills in them)\n"
		"Frame pacing: %s\n",
		BENCHMARK_MAP, BENCHMARK_BOTS, BENCHMARK_SEED, BENCHMARK_SECONDS, benchmark.measure_second, ticks, kills,
		!strcmp(benchmark.pacing, "uncapped") ?
			"uncapped (vsync, variable refresh and the frame cap off for the run; a cap of " BENCHMARK_UNCAPPED_FPS
			" fps)" :
			"your settings (vsync, variable refresh and the frame cap as set)");
}

static void benchmark_finish(
	long kills,
	long ticks)
{
	char about[512];
	int width = 0, height = 0;

	benchmark_about(about, sizeof(about), kills, ticks);
	platform_video_drawable_size(&width, &height);
	snprintf(benchmark.window, sizeof(benchmark.window), "%dx%d", width, height);
	report_processor_text(benchmark.processor, sizeof(benchmark.processor));
	if (!report_benchmark_write(benchmark.first_frame, report_frames_mark(), BENCHMARK_MAP, about, &benchmark.measure,
		benchmark.verdict, sizeof(benchmark.verdict), benchmark.file, sizeof(benchmark.file)))
	{
		if (!benchmark.measure.frames)
		{
			benchmark_end("no frames were measured");
			return;
		}
		snprintf(benchmark.file, sizeof(benchmark.file), "(the results could not be saved)");
	}
	platform_log("benchmark: %lu frames, %.1f fps average, %.1f fps 1%% low, p95 %.2f ms, p99 %.2f ms, GPU %.2f ms, "
		"game %.2f ms, %s", benchmark.measure.frames, benchmark.measure.average_fps, benchmark.measure.low_1_fps,
		benchmark.measure.p95_ms, benchmark.measure.p99_ms, benchmark.measure.gpu_ms, benchmark.measure.game_ms,
		benchmark.verdict);
	benchmark_end(NULL);
}

void benchmark_request(
	char const *pacing)
{
	if (benchmark.state != _benchmark_idle && benchmark.state != _benchmark_done)
		return;
	if (!pacing || (strcmp(pacing, "uncapped") && strcmp(pacing, "settings")))
		pacing = config_string("display.benchmark_pacing");
	snprintf(benchmark.pacing, sizeof(benchmark.pacing), "%s", strcmp(pacing, "settings") ? "uncapped" : "settings");
	benchmark.state = _benchmark_asked;
}

void benchmark_frame(
	int main_menu_loaded)
{
	long players, bots, kills, cues_start;
	double scenario;
	struct benchmark_live live;
	char const *problem;

	/* (halo --benchmark [uncapped|settings], or debug.benchmark (a test
	run's, which takes no arguments): as RUN BENCHMARK, at once) */
	if (!benchmark.command_line_checked)
	{
		char value[32];

		benchmark.command_line_checked = TRUE;
		if (config_command_line_value("--benchmark", value, sizeof(value)) &&
			(!strcmp(value, "uncapped") || !strcmp(value, "settings")))
		{
			benchmark_request(value);
		}
		else if (config_command_line_flag("--benchmark"))
			benchmark_request(NULL);
		else if (config_string("debug.benchmark")[0])
			benchmark_request(config_string("debug.benchmark"));
	}
	switch (benchmark.state)
	{
	case _benchmark_asked:
		if (main_menu_loaded)
			benchmark_begin();
		return;
	case _benchmark_done:
		if (main_menu_loaded && pc_menu_benchmark_results_open())
			benchmark.state = _benchmark_idle;
		return;
	case _benchmark_idle:
		return;
	default:
		break;
	}

	/* (the match left by the player, or never begun) */
	if (benchmark.playing && main_menu_loaded)
	{
		benchmark_end("the match ended before the minute was measured");
		return;
	}
	if (!benchmark.playing)
	{
		if (main_menu_loaded || !game_time_initialized() || !game_in_progress())
		{
			if (system_milliseconds() - benchmark.started_ms > BENCHMARK_START_MOST_MS)
				benchmark_end("the match did not start (is bloodgulch.map in the maps folder?)");
			return;
		}
		benchmark.playing = TRUE;
		benchmark.playing_ms = system_milliseconds();
		benchmark.playing_tick = game_time_get();
		platform_log("benchmark: the match began");
	}

	benchmark_count(&players, &bots, &kills);
	live.real_seconds = (double)(system_milliseconds() - benchmark.playing_ms) / 1000.0;
	live.ticks = game_time_get() - benchmark.playing_tick;
	live.players = players;
	live.bots = bots;
	live.kills = kills;
	problem = benchmark_live_problem(&live, FALSE);
	if (problem)
	{
		benchmark_end(problem);
		return;
	}
	/* (the scenario's clock: from the tick its player had a unit) */
	cues_start = network_test_cues_start_tick();
	if (cues_start == NONE)
		return;
	scenario = (double)(game_time_get() - cues_start) / TICKS_PER_SECOND;

	if (benchmark.state == _benchmark_warming || benchmark.state == _benchmark_starting)
	{
		benchmark.state = _benchmark_warming;
		if (scenario < BENCHMARK_WARMUP)
			return;
		problem = benchmark_live_problem(&live, TRUE);
		if (problem)
		{
			if (scenario >= BENCHMARK_WARMUP_MOST)
				benchmark_end(problem);
			return;
		}
		benchmark.state = _benchmark_measuring;
		benchmark.measure_second = scenario;
		benchmark.first_frame = report_frames_mark();
		benchmark.kills_before = kills;
		benchmark.playing_tick = game_time_get();
		benchmark.playing_ms = system_milliseconds();
		platform_log("benchmark: measuring from %.1f s of the match (%ld players, %ld bots, %ld kills)", scenario,
			players, bots, kills);
		return;
	}
	/* _benchmark_measuring */
	if (scenario < benchmark.measure_second + BENCHMARK_SECONDS)
		return;
	live.kills = kills - benchmark.kills_before;
	problem = benchmark_live_problem(&live, TRUE);
	if (problem)
	{
		benchmark_end(problem);
		return;
	}
	benchmark_finish(live.kills, live.ticks);
}

/* ---------- the results screen (port/assets/menus/screens/benchmark.xml):
its table's rows (the source "benchmark.results") and its bindings */

enum
{
	_benchmark_row_average,
	_benchmark_row_low_1,
	_benchmark_row_low_01,
	_benchmark_row_p50,
	_benchmark_row_p95,
	_benchmark_row_p99,
	_benchmark_row_gpu,
	_benchmark_row_game,
	_benchmark_row_render,
	_benchmark_row_frames,
	NUMBER_OF_BENCHMARK_ROWS
};

static void benchmark_row(
	short index,
	char *label,
	char *value,
	unsigned long size)
{
	struct report_measure const *measure = &benchmark.measure;

	label[0] = value[0] = 0;
	switch (index)
	{
	case _benchmark_row_average:
		snprintf(label, size, "Average");
		snprintf(value, size, "%.1f fps (%.2f ms)", measure->average_fps, measure->average_ms);
		break;
	case _benchmark_row_low_1:
		snprintf(label, size, "1%% low");
		snprintf(value, size, "%.1f fps", measure->low_1_fps);
		break;
	case _benchmark_row_low_01:
		snprintf(label, size, "0.1%% low");
		snprintf(value, size, "%.1f fps", measure->low_01_fps);
		break;
	case _benchmark_row_p50:
		snprintf(label, size, "Frame time, median");
		snprintf(value, size, "%.2f ms", measure->p50_ms);
		break;
	case _benchmark_row_p95:
		snprintf(label, size, "Frame time, 95th percentile");
		snprintf(value, size, "%.2f ms", measure->p95_ms);
		break;
	case _benchmark_row_p99:
		snprintf(label, size, "Frame time, 99th percentile");
		snprintf(value, size, "%.2f ms", measure->p99_ms);
		break;
	case _benchmark_row_gpu:
		snprintf(label, size, "GPU, a frame");
		if (measure->gpu_ms >= 0.0)
			snprintf(value, size, "%.2f ms", measure->gpu_ms);
		else
			snprintf(value, size, "not measured");
		break;
	case _benchmark_row_game:
		snprintf(label, size, "CPU, game thread");
		snprintf(value, size, "%.2f ms", measure->game_ms);
		break;
	case _benchmark_row_render:
		snprintf(label, size, "CPU, render thread");
		snprintf(value, size, "%.2f ms", measure->render_ms);
		break;
	case _benchmark_row_frames:
		snprintf(label, size, "Frames measured");
		snprintf(value, size, "%lu in %.1f s", measure->frames, measure->seconds);
		break;
	}
}

static void benchmark_widen(
	char const *text,
	unsigned short *wide,
	short size)
{
	short index;

	for (index = 0; index < size - 1 && text[index]; index++)
		wide[index] = (unsigned char)text[index];
	wide[index] = 0;
}

static void benchmark_source_update(
	struct menu_table_view *view)
{
	view->count = benchmark.failure[0] || !benchmark.measure.frames ? 0 : NUMBER_OF_BENCHMARK_ROWS;
	view->first = 0;
	view->chosen = NONE;
	view->focus = NONE;
	view->focus_titles = FALSE;
}

static int benchmark_source_heading(
	short index,
	unsigned short *words,
	short size)
{
	(void)index;
	(void)words;
	(void)size;
	return FALSE;
}

static void benchmark_source_cell(
	short index,
	char const *column,
	int focused,
	struct menu_table_cell *cell)
{
	char label[64], value[64];

	(void)focused;
	benchmark_row(index, label, value, sizeof(label));
	benchmark_widen(strcmp(column, "value") ? label : value, cell->text, MENU_TABLE_TEXT_LENGTH);
}

struct menu_table_source const benchmark_results_source =
{
	benchmark_source_update,
	benchmark_source_heading,
	benchmark_source_cell,
	NULL,
	NULL
};

int benchmark_value(
	char const *name,
	char *value,
	unsigned long size)
{
	struct report_measure const *measure = &benchmark.measure;
	int failed = benchmark.failure[0] || !measure->frames;

	value[0] = 0;
	if (!strcmp(name, "benchmark.failure") && failed)
		snprintf(value, size, "The benchmark stopped: %s. Nothing was measured.",
			benchmark.failure[0] ? benchmark.failure : "it has not run");
	else if (!strcmp(name, "benchmark.verdict") && !failed)
		snprintf(value, size, "%s", benchmark.verdict[0] ? benchmark.verdict : "No verdict");
	else if (!strcmp(name, "benchmark.held") && !failed && measure->paced_percent >= BENCHMARK_PACED_PERCENT)
		snprintf(value, size, "The frames waited for vsync or the frame cap %.0f%% of the time.",
			measure->paced_percent);
	else if (!strcmp(name, "benchmark.pacing"))
		snprintf(value, size, "%s", strcmp(benchmark.pacing, "uncapped") ?
			"your settings (vsync and frame cap as set)" : "uncapped (vsync, VRR and frame cap off)");
	else if (!strcmp(name, "benchmark.settings") && !failed)
		snprintf(value, size, "%s, %s graphics", benchmark.window, config_string("display.graphics"));
	else if (!strcmp(name, "benchmark.gpu") && !failed)
		snprintf(value, size, "%s", xgpu_backend_device()[0] ? xgpu_backend_device() : "unknown");
	else if (!strcmp(name, "benchmark.cpu") && !failed)
		snprintf(value, size, "%s", benchmark.processor);
	else if (!strcmp(name, "benchmark.file") && !failed)
		snprintf(value, size, "%s", benchmark.file);
	else if (!strcmp(name, "benchmark.scenario"))
		snprintf(value, size, "Blood Gulch, %s bots, %.0f seconds", BENCHMARK_BOTS, BENCHMARK_SECONDS);
	return value[0] != 0;
}
