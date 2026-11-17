/*
DEMO_RECORDING_LISTENER.C (test)

The listener demo.c tells as a recording starts and stops
(demo_set_recording_listener): demo_record_stop's own code, with the
recorder faked, tells it the last tick; nothing is told with none set.
test_demo_recording_listener.py takes the code (under_test.inc) and checks
that demo_record_start tells it the demo's name and first tick.
*/

#include "harness.h"

typedef void (*demo_recording_listener)(int started, const char *name, long tick);

struct demo_buffer { unsigned long size; boolean failed; };

static struct
{
	boolean recording;
	void *writer;
	struct demo_buffer records;
	long last_tick;
	unsigned long long written;
	char name[64];
	long ticks, patches, structural_snapshots, structural_too_often;
	unsigned long patch_bytes;
	unsigned long long work_microseconds;
	boolean shadow_valid;
} demo_recorder;

static struct { boolean read; } demo_settings;

#define demo_record_begin(buffer, type) 0UL
#define demo_buffer_u32(buffer, value) ((void)(value))
#define demo_record_end(buffer, start) ((void)(start))
#define demo_records_flush() ((void)0)
#define demo_writer_close(writer) 0ULL
#define platform_log(...) ((void)0)

#include "under_test.inc"

static int told, told_started;
static long told_tick;
static char told_name[64];

static void listener(int started, const char *name, long tick)
{
	told++;
	told_started = started;
	told_tick = tick;
	snprintf(told_name, sizeof(told_name), "%s", name ? name : "(null)");
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* set: a start told with its name and tick (as demo_record_start
	tells it), then the stop with the recording's last tick */
	if (!strcmp(case_name, "start_stop"))
	{
		demo_set_recording_listener(listener);
		demo_recorder.recording = TRUE;
		strcpy(demo_recorder.name, "20261117-120000-a10.demo");
		demo_recording_tell(1, demo_recorder.name, 450);
		CHECK(told == 1 && told_started == 1 && told_tick == 450, "start: %d %d %ld", told, told_started, told_tick);
		CHECK(!strcmp(told_name, "20261117-120000-a10.demo"), "name %s", told_name);
		demo_recorder.last_tick = 1234;
		demo_record_stop("a test");
		CHECK(told == 2 && told_started == 0 && told_tick == 1234, "stop: %d %d %ld", told, told_started, told_tick);
		CHECK(!strcmp(told_name, "(null)"), "stop's name %s", told_name);
		/* (a stop with nothing recording tells nothing) */
		demo_record_stop("again");
		CHECK(told == 2, "a second stop told: %d", told);
		return 0;
	}
	/* unset (never, or set and taken away): nothing told */
	if (!strcmp(case_name, "unset"))
	{
		demo_recorder.recording = TRUE;
		demo_record_stop("a test");
		demo_set_recording_listener(listener);
		demo_set_recording_listener(NULL);
		demo_recorder.recording = TRUE;
		demo_recording_tell(1, "x.demo", 1);
		demo_record_stop("a test");
		CHECK(told == 0, "told %d times with none set", told);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
