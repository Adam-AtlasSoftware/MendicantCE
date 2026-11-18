/*
PROFILE_TIMINGS.C (test)

debug.timings (#1147): source/cseries/profile.c's profile_frame_start turns
the CPU sections' timing on or off at a frame's start (profile_port_timings,
perf_overlay.c's), port/linux/include/profile_sections.h's sections time
through the original's profile_enter, and profile_port_busiest lists the
busiest. Off, a section reads no clock and calls nothing. test_profile_timings.py
takes the code.
*/

#include "harness.h"

#define MAXIMUM_PROFILE_HISTORY 120
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)>(b)?(b):(a))

/* the fields the code reads of a section (its first three as profile.h's,
which PROFILE_SECTION initializes) */
struct profile_section
{
	const char *name;
	long section_index;
	boolean active;
	short stack_depth;
	long recent_call_count;
	long long recent_elapsed_timebase;
	long sample_count;
};

struct profile_timer
{
	long long start;
};

static struct
{
	long long timebase_frequency;
	short stack_depth;
	short section_count;
	struct profile_section *sections[8];
	struct
	{
		long frame_index;
		long long vertical_blank_index;
		short game_tick_count;
		struct profile_timer frame;
	} current_frame;
} profile_globals;

static boolean profile_global_enable = FALSE;
static boolean profile_timebase_ticks = FALSE;
static struct { long frame_index; } render;
static struct { long long vertical_blank_index; } rasterizer_globals;

/* the clock reads and the sections' timing calls */
static int clock_reads, enters, exits;
static boolean timings;

static boolean profile_port_timings(void)
{
	return timings;
}

static void profile_internal_step(void)
{
}

#define csmemset memset

static void profile_timesection_begin_now(struct profile_timer *timer)
{
	clock_reads++;
	timer->start = clock_reads;
}

static void profile_enter_private(struct profile_section *section)
{
	clock_reads++;
	enters++;
	profile_globals.stack_depth++;
	section->stack_depth = profile_globals.stack_depth;
}

static void profile_exit_private(struct profile_section *section)
{
	clock_reads++;
	exits++;
	profile_globals.stack_depth--;
	section->stack_depth = NONE;
}

#include "under_test.inc"

PROFILE_SECTION(test_section, "ai_update")

/* a frame: its start, then a section around some work */
static int frame(void)
{
	int work = 0;

	profile_frame_start();
	profile_scope(test_section, work++;)
	return work;
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	if (!strcmp(test, "off"))
	{
		int index, reads;

		for (index = 0; index < 100; index++)
		{
			reads = clock_reads;
			CHECK(frame() == 1, "the section's statement not run");
			/* (the frame's own timer, as before: the section none) */
			CHECK(clock_reads - reads == 1, "%d clock reads in a frame with timings off", clock_reads - reads);
		}
		CHECK(!enters && !exits, "%d enters, %d exits with timings off", enters, exits);
		CHECK(!profile_global_enable, "profile_global_enable on with timings off");
		return 0;
	}
	if (!strcmp(test, "on"))
	{
		timings = TRUE;
		CHECK(frame() == 1, "the section's statement not run");
		CHECK(enters == 1 && exits == 1, "%d enters, %d exits with timings on", enters, exits);
		timings = FALSE;
		frame();
		CHECK(enters == 1 && exits == 1, "timed after timings were turned off");
		return 0;
	}
	if (!strcmp(test, "reopened"))
	{
		/* (turned off mid-frame, the console's: the section's exit not
		made; turned on again at a frame's start, it is taken for closed) */
		timings = TRUE;
		profile_frame_start();
		profile_enter(test_section)
		profile_global_enable = FALSE;
		profile_exit(test_section)
		CHECK(test_section.stack_depth != NONE, "the fault the case makes did not happen");
		frame();
		CHECK(test_section.stack_depth == NONE && profile_globals.stack_depth == 0,
			"a section left open: depth %d, %d", test_section.stack_depth, profile_globals.stack_depth);
		return 0;
	}
	if (!strcmp(test, "busiest"))
	{
		static struct profile_section sections[4] =
		{
			{ "a", 0, TRUE, NONE, 120, 1200, 200 },
			{ "b", 1, TRUE, NONE, 240, 3600, 200 },
			{ "c", 2, FALSE, NONE, 120, 99999, 200 },
			{ "d", 3, TRUE, NONE, 30, 600, 60 },
		};
		char const *names[2];
		real milliseconds[2], calls[2];
		short index, count;

		profile_global_enable = TRUE;
		profile_globals.timebase_frequency = 1000;
		for (index = 0; index < 4; index++)
			profile_globals.sections[index] = &sections[index];
		profile_globals.section_count = 4;
		count = profile_port_busiest(names, milliseconds, calls, 2);
		/* (b: 3600 ticks over 120 frames, 30 ms; d: 600 over its 60 frames,
		10 ms; a: 1200 over 120, 10 ms, after d; c inactive) */
		CHECK(count == 2, "%d listed", count);
		CHECK(!strcmp(names[0], "b") && milliseconds[0] > 29.9f && milliseconds[0] < 30.1f && calls[0] == 2.0f,
			"first %s %f %f", names[0], milliseconds[0], calls[0]);
		CHECK(!strcmp(names[1], "d") || !strcmp(names[1], "a"), "second %s", names[1]);
		CHECK(milliseconds[1] > 9.9f && milliseconds[1] < 10.1f, "second %f ms", milliseconds[1]);
		profile_global_enable = FALSE;
		CHECK(profile_port_busiest(names, milliseconds, calls, 2) == 0, "listed with timings off");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
