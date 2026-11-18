/*
BENCHMARK.C (test)

The built-in benchmark (port/linux/game/benchmark.c): its results' math,
port/linux/src/report.c's report_measure_frames, on a frame ring made up
here (the average, the 1% and 0.1% lows, the frame time's percentiles, the
GPU's, the game's and the renderer's thread's time, the waits for the
display); and its guard, benchmark_live_problem, which refuses a match that
is frozen, empty or not fought before it is measured. test_benchmark.py
takes the code.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

#define REPORT_HITCH_MS 50
#define REPORT_STALL_MS 100
#define REPORT_GPU_UNKNOWN 0xFFFFFFFFu
#define TICKS_PER_SECOND 30

#include "under_test.inc"

#define FRAMES 1000

static uint32_t micros[FRAMES], scratch[FRAMES];
static struct report_frame_work work[FRAMES];

static int near(double value, double expected)
{
	return fabs(value - expected) < 1e-6 * (fabs(expected) + 1.0);
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	if (!strcmp(test, "measure"))
	{
		struct report_measure measure;
		unsigned long index;

		/* 1000 frames: 980 of 10 ms, 10 of 20 ms, 9 of 40 ms and one of
		100 ms, oldest first in no order of their times; the GPU's 6 ms on
		each but the first 100 (not measured), the renderer's 3 ms, the
		game's waits 4 ms, of them 1 ms the pacing's */
		for (index = 0; index < FRAMES; index++)
		{
			micros[index] = 10000;
			if (index % 100 == 37)
				micros[index] = 20000;
			work[index].gpu = index < 100 ? REPORT_GPU_UNKNOWN : 6000;
			work[index].render = 3000;
			work[index].waited = 4000;
			work[index].paced = 1000;
		}
		for (index = 0; index < 9; index++)
			micros[500 + index * 7] = 40000;
		micros[999] = 100000;
		report_measure_frames(micros, work, FRAMES, scratch, &measure);
		CHECK(measure.frames == FRAMES, "%lu frames", measure.frames);
		/* (9800 + 200 + 360 + 100 ms) */
		CHECK(near(measure.seconds, 10.46), "%f seconds", measure.seconds);
		CHECK(near(measure.average_ms, 10.46), "average %f ms", measure.average_ms);
		CHECK(near(measure.average_fps, 1000.0 / 10.46), "average %f fps", measure.average_fps);
		/* the slowest 9 of the 999 frames but the stall of 100 ms (Save
		Report's, report_low_fps): the nine of 40 */
		CHECK(near(measure.low_1_fps, 25.0), "1%% low %f fps", measure.low_1_fps);
		/* the slowest frame alone, but the stall */
		CHECK(near(measure.low_01_fps, 25.0), "0.1%% low %f fps", measure.low_01_fps);
		CHECK(near(measure.p50_ms, 10.0), "p50 %f", measure.p50_ms);
		/* (sorted: 980 of 10 ms, then 10 of 20 (980-989), 9 of 40, one of 100;
		p95 at 949, p99 at 989) */
		CHECK(near(measure.p95_ms, 10.0), "p95 %f", measure.p95_ms);
		CHECK(near(measure.p99_ms, 20.0), "p99 %f", measure.p99_ms);
		CHECK(measure.hitches == 1, "%lu hitches", measure.hitches);
		/* the GPU's over the 900 frames measured, not counted as 0 on the rest */
		CHECK(near(measure.gpu_ms, 6.0), "GPU %f ms", measure.gpu_ms);
		CHECK(near(measure.render_ms, 3.0), "render %f ms", measure.render_ms);
		CHECK(near(measure.waited_ms, 4.0), "waited %f ms", measure.waited_ms);
		/* the game's: a frame less its waits */
		CHECK(near(measure.game_ms, 10.46 - 4.0), "game %f ms", measure.game_ms);
		CHECK(near(measure.paced_percent, 100.0 * 1.0 / 10.46), "paced %f%%", measure.paced_percent);
		/* the frames themselves left as they were (the ring's copy) */
		CHECK(micros[999] == 100000 && micros[0] == 10000, "the frames reordered");

		/* no GPU time at all: not measured, not 0 ms */
		for (index = 0; index < FRAMES; index++)
			work[index].gpu = REPORT_GPU_UNKNOWN;
		report_measure_frames(micros, work, FRAMES, scratch, &measure);
		CHECK(measure.gpu_ms < 0.0, "GPU %f ms with none measured", measure.gpu_ms);
		/* and no frames: nothing */
		report_measure_frames(micros, work, 0, scratch, &measure);
		CHECK(measure.frames == 0 && measure.average_fps == 0.0 && measure.gpu_ms < 0.0, "no frames measured");
		return 0;
	}
	if (!strcmp(test, "live"))
	{
		/* a match 20 seconds in: 600 ticks, a player, 39 bots, 12 kills */
		struct benchmark_live live = { 20.0, 600, 1, 39, 12 };
		struct benchmark_live frozen = live, slow = live, early = live, empty = live, peaceful = live;

		CHECK(!benchmark_live_problem(&live, FALSE), "a live match refused");
		CHECK(!benchmark_live_problem(&live, TRUE), "a live match refused for measuring");
		/* (its first seconds: no verdict on its time yet) */
		early.real_seconds = 2.0;
		early.ticks = 0;
		CHECK(!benchmark_live_problem(&early, FALSE), "a match just begun refused");
		/* a slow computer's: half the game's speed */
		slow.ticks = 300;
		CHECK(!benchmark_live_problem(&slow, TRUE), "a slow match refused");
		/* the negative control: its game time standing still */
		frozen.ticks = 0;
		CHECK(benchmark_live_problem(&frozen, FALSE) != NULL, "a frozen match taken");
		CHECK(benchmark_live_problem(&frozen, TRUE) != NULL, "a frozen match measured");
		frozen.ticks = 20;
		CHECK(benchmark_live_problem(&frozen, FALSE) != NULL, "a nearly frozen match taken");
		/* no bots, no fighting: not measured, but not yet a frozen match */
		empty.bots = 0;
		CHECK(!benchmark_live_problem(&empty, FALSE), "a match without bots taken for frozen");
		CHECK(benchmark_live_problem(&empty, TRUE) != NULL, "a match without bots measured");
		peaceful.kills = 0;
		CHECK(benchmark_live_problem(&peaceful, TRUE) != NULL, "a match without kills measured");
		empty = live;
		empty.players = 0;
		CHECK(benchmark_live_problem(&empty, TRUE) != NULL, "a match without its player measured");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
