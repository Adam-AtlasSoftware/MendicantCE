/*
DEMO_PERF_LOG.C (test)

debug.demo_perf_log (#1148): port/linux/src/report.c's log beside each demo,
begun and ended by the demos' listener, a row for each frame at the game's
tick, its events and summaries, and the logs of demos that are gone
deleted. Off, the listener is never set and nothing is read or written.
test_demo_perf_log.py takes the code and a folder for the demos (argv[2]).
*/

#include "harness.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>

#define REPORT_GPU_UNKNOWN 0xFFFFFFFFu

struct report_frame_work
{
	uint32_t gpu, render, waited, paced;
};

static const char *folder_path;
static int setting, changes = 1, cpu_reads, initialized = 1, extra;
static long tick;
static unsigned long pipelines;
static long long now_ns;
static void (*listener)(int started, const char *name, long tick);
static int listeners_set;

static unsigned long config_changes(void)
{
	return (unsigned long)changes;
}

static int config_boolean(const char *name)
{
	CHECK(!strcmp(name, "debug.demo_perf_log"), "read %s", name);
	return setting;
}

static void platform_log(const char *format, ...)
{
	(void)format;
}

static long long report_thread_cpu_ns(void)
{
	cpu_reads++;
	return (long long)cpu_reads * 1000000LL;
}

static long long report_now_ns(void)
{
	return now_ns;
}

static unsigned char game_time_initialized(void)
{
	return (unsigned char)initialized;
}

static long game_time_get(void)
{
	return tick;
}

static unsigned long xgpu_backend_pipelines_defined(void)
{
	return pipelines;
}

static unsigned long xgpu_backend_frame_extra_pending(void)
{
	return (unsigned long)extra;
}

static int demo_files_folder(char *path, unsigned long size, int pinned)
{
	snprintf(path, size, "%s%s", folder_path, pinned ? "/kept" : "");
	return 1;
}

static int demo_files_path(char const *name, char *path, unsigned long size)
{
	struct stat information;
	int pinned;

	for (pinned = 0; pinned < 2; pinned++)
	{
		snprintf(path, size, "%s%s/%s", folder_path, pinned ? "/kept" : "", name);
		if (!stat(path, &information))
			return 1;
	}
	return 0;
}

static void *posix_directory_open(const char *path)
{
	return opendir(path);
}

static int posix_directory_next(void *directory, char *name, unsigned long size)
{
	struct dirent *entry = readdir((DIR *)directory);

	if (!entry)
		return 0;
	snprintf(name, size, "%s", entry->d_name);
	return 1;
}

static void posix_directory_close(void *directory)
{
	closedir((DIR *)directory);
}

#include "under_test.inc"

static void demo_set_recording_listener(demo_recording_listener set)
{
	listener = set;
	listeners_set++;
}

static void touch(const char *name)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", folder_path, name);
	file = fopen(path, "w");
	CHECK(file != NULL, "cannot write %s", path);
	fclose(file);
}

static int exists(const char *name)
{
	char path[1024];
	struct stat information;

	snprintf(path, sizeof(path), "%s/%s", folder_path, name);
	return !stat(path, &information);
}

/* a frame presented of micros, at the tick */
static void frame(long frame_tick, long long micros)
{
	struct report_frame_work work = { 2000, 800, 1000, 0 };

	tick = frame_tick;
	now_ns += micros * 1000LL;
	demo_perf_frame(micros, &work, now_ns);
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	folder_path = argc > 2 ? argv[2] : ".";
	if (!strcmp(test, "off"))
	{
		long index;

		touch("x.demo");
		for (index = 0; index < 100; index++)
			frame(100 + index / 8, 4166);
		CHECK(!listeners_set, "the listener set with the setting off");
		CHECK(!cpu_reads, "%d clock reads with the setting off", cpu_reads);
		CHECK(!exists("x.perf"), "a log written with the setting off");
		return 0;
	}
	if (!strcmp(test, "rows"))
	{
		char path[1024], line[256];
		FILE *file;
		long index, rows = 0, last_tick = -1, stalls = 0, built = 0, summaries = 0, ended = 0;

		setting = 1;
		frame(1, 4166);
		CHECK(listeners_set == 1 && listener, "the listener not set with the setting on");
		touch("x.demo");
		touch("gone.perf");
		touch("kept_one.perf");
		mkdir((snprintf(path, sizeof(path), "%s/kept", folder_path), path), 0755);
		touch("kept/kept_one.demo");
		listener(1, "x.demo", 100);
		CHECK(!exists("gone.perf"), "the log of a demo that is gone kept");
		CHECK(exists("kept_one.perf"), "the log of a pinned demo deleted");
		/* (8 frames a tick, a stall at tick 105, pipelines built at tick 110,
		a load at 120; 12 s in all) */
		for (index = 0; index < 8 * 30 * 12; index++)
		{
			long frame_tick = 100 + index / 8;

			pipelines += frame_tick == 110 && index % 8 == 0 ? 3 : 0;
			extra = frame_tick == 120;
			frame(frame_tick, (frame_tick == 105 || frame_tick == 120) && index % 8 == 0 ? 60000 : 4166);
		}
		listener(0, NULL, 100 + 30 * 12);
		frame(999, 4166);
		snprintf(path, sizeof(path), "%s/x.perf", folder_path);
		file = fopen(path, "r");
		CHECK(file != NULL, "no log beside the demo");
		CHECK(fgets(line, sizeof(line), file) && !strcmp(line, "# demo perf log 1 x.demo 100\n"), "header %s", line);
		while (fgets(line, sizeof(line), file))
		{
			long row_tick, number;
			long long micros, cpu, gpu;
			char kind[16];

			if (sscanf(line, "f %ld %ld %lld %lld %lld", &row_tick, &number, &micros, &cpu, &gpu) == 5)
			{
				rows++;
				CHECK(number == rows, "frame %ld numbered %ld", rows, number);
				/* (the frame's tick: the game's as it was presented) */
				CHECK(row_tick == 100 + (rows - 1) / 8, "frame %ld at tick %ld", rows, row_tick);
				CHECK(row_tick >= last_tick, "ticks go back: %ld after %ld", row_tick, last_tick);
				CHECK(cpu == 1000 && gpu == 2000, "frame %ld: CPU %lld, GPU %lld", rows, cpu, gpu);
				last_tick = row_tick;
			}
			else if (sscanf(line, "e %ld %15s", &row_tick, kind) == 2)
			{
				if (!strcmp(kind, "stall"))
					stalls += row_tick == 105;
				else if (!strcmp(kind, "load"))
					CHECK(row_tick == 120, "a load at tick %ld", row_tick);
				else if (!strcmp(kind, "pipelines"))
					built += row_tick == 110 && !strcmp(line, "e 110 pipelines 3\n");
			}
			else if (line[0] == 's')
				summaries++;
			else if (!strncmp(line, "# end ", 6))
				ended = atol(line + 6) == 100 + 30 * 12;
		}
		fclose(file);
		CHECK(rows == 8 * 30 * 12, "%ld rows", rows);
		CHECK(stalls == 1 && built == 1, "stalls %ld, pipelines %ld", stalls, built);
		/* (one at 10 s, one as it stopped) */
		CHECK(summaries == 2, "%ld summaries", summaries);
		CHECK(ended, "no end line, or the wrong tick");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
