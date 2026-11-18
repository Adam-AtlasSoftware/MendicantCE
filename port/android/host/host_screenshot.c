/*
HOST_SCREENSHOT.C

The screenshot key's files (controls.screenshot): the frame the guest asks
for (XGPU_STREAM_SCREENSHOT, which host_render_stream.c reads from the
backend, tone mapped) written as a PNG with SDL into the folder the guest
names (its data folder's screenshots), named for the local date and time,
YYYY-MM-DD_HH.MM.SS.png; another in the same second YYYY-MM-DD_HH.MM.SS_2.png,
_3 and on, so that none is written over.

The PNG's deflate takes a 4K frame most of a second, and the record that
asks for it holds the render thread and the game's (which waits for its
answer): the name is chosen and the pixels copied at once, and the file is
written on a thread of its own, as <name>.part, renamed to <name> once
whole, so that a reader that waits for <name> (report.c's zip) never reads
half a picture. host_screenshot_finish waits for those being written.
*/

#include "host_render_stream.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (pictures in one second, at most) */
#define SCREENSHOT_COLLISIONS 1000
/* (pictures written on their threads at once, at most: each holds a copy
of the frame; past them, written on the caller's) */
#define SCREENSHOT_WRITERS 2

/* a picture written on its thread */
struct screenshot_job
{
	unsigned char *bgra;
	uint32_t width, height;
	char *path, *part;
	SDL_Thread *thread;
};

static struct
{
	SDL_Mutex *lock;
	struct screenshot_job *jobs[SCREENSHOT_WRITERS];
	int at_exit;
} writers;

void host_screenshot_name(char *name, uint32_t name_size, int year, int month, int day, int hour, int minute,
	int second, unsigned collision)
{
	if (collision)
		snprintf(name, name_size, "%04d-%02d-%02d_%02d.%02d.%02d_%u.png", year, month, day, hour, minute, second,
			collision + 1);
	else
		snprintf(name, name_size, "%04d-%02d-%02d_%02d.%02d.%02d.png", year, month, day, hour, minute, second);
}

/* (a failure's reason in name, for the guest's message) */
static int failed(char *name, uint32_t name_size, const char *reason)
{
	snprintf(name, name_size, "%s", reason);
	return 0;
}

/* the pixels written as a PNG at path: through part (renamed to path once
whole) when part is given; 1 if written */
static int png_write(unsigned char *bgra, uint32_t width, uint32_t height, const char *path, const char *part)
{
	SDL_Surface *surface;
	size_t pixel;
	int written;

	/* (the frame's alpha is the game's scratch, not transparency) */
	for (pixel = 0; pixel < (size_t)width * height; pixel++)
		bgra[pixel * 4 + 3] = 0xff;
	surface = SDL_CreateSurfaceFrom((int)width, (int)height, SDL_PIXELFORMAT_BGRA32, bgra, (int)(width * 4));
	written = surface && SDL_SavePNG(surface, part ? part : path);
	SDL_DestroySurface(surface);
	if (part)
	{
		written = written && SDL_RenamePath(part, path);
		if (!written)
			SDL_RemovePath(part);
	}
	return written;
}

static int SDLCALL job_run(void *data)
{
	struct screenshot_job *job = data;

	if (!png_write(job->bgra, job->width, job->height, job->path, job->part))
		SDL_Log("screenshot: %s could not be written", job->path);
	return 0;
}

static void job_free(struct screenshot_job *job)
{
	SDL_free(job->bgra);
	SDL_free(job->path);
	SDL_free(job->part);
	SDL_free(job);
}

/* the jobs' lock, made once (the first picture is asked for on the render
thread, before any job) */
static SDL_Mutex *writers_lock(void)
{
	if (!writers.lock)
		writers.lock = SDL_CreateMutex();
	return writers.lock;
}

/* (with the lock held) the jobs whose files are written, let go of; wait:
every one waited for */
static void jobs_reap(int wait)
{
	int index, status;

	for (index = 0; index < SCREENSHOT_WRITERS; index++)
	{
		struct screenshot_job *job = writers.jobs[index];

		if (!job)
			continue;
		if (!wait && SDL_GetThreadState(job->thread) != SDL_THREAD_COMPLETE)
			continue;
		SDL_WaitThread(job->thread, &status);
		job_free(job);
		writers.jobs[index] = NULL;
	}
}

void host_screenshot_finish(void)
{
	SDL_Mutex *lock = writers_lock();

	SDL_LockMutex(lock);
	jobs_reap(1);
	SDL_UnlockMutex(lock);
}

static void at_exit(void)
{
	host_screenshot_finish();
}

/* the picture handed to a thread of its own; 0 if none can take it (too
many being written, or no memory or thread), for the caller to write it */
static int job_start(const unsigned char *bgra, uint32_t width, uint32_t height, const char *path)
{
	SDL_Mutex *lock = writers_lock();
	struct screenshot_job *job;
	size_t bytes = (size_t)width * height * 4, part_size = strlen(path) + 6;
	SDL_IOStream *reserve;
	int index, started = 0;

	if (!lock)
		return 0;
	SDL_LockMutex(lock);
	jobs_reap(0);
	for (index = 0; index < SCREENSHOT_WRITERS && writers.jobs[index]; index++)
		;
	job = index < SCREENSHOT_WRITERS ? SDL_calloc(1, sizeof(*job)) : NULL;
	if (job)
	{
		job->bgra = SDL_malloc(bytes);
		job->path = SDL_strdup(path);
		job->part = SDL_malloc(part_size);
		job->width = width;
		job->height = height;
	}
	if (job && job->bgra && job->path && job->part)
	{
		memcpy(job->bgra, bgra, bytes);
		snprintf(job->part, part_size, "%s.part", path);
		/* (the .part made now: the name taken, for the next picture of
		this second, before its file is written) */
		reserve = SDL_IOFromFile(job->part, "wb");
		if (reserve && SDL_CloseIO(reserve))
		{
			job->thread = SDL_CreateThread(job_run, "screenshot", job);
			started = job->thread != NULL;
			if (!started)
				SDL_RemovePath(job->part);
		}
	}
	if (started)
	{
		writers.jobs[index] = job;
		if (!writers.at_exit)
			writers.at_exit = atexit(at_exit) == 0;
	}
	else if (job)
		job_free(job);
	SDL_UnlockMutex(lock);
	return started;
}

int host_screenshot_write(unsigned char *bgra, uint32_t width, uint32_t height, const char *directory, char *name,
	uint32_t name_size)
{
	SDL_Time now;
	SDL_DateTime date;
	size_t path_size;
	unsigned collision;
	char *path;
	int written;

	if (!name_size)
		return 0;
	if (!width || !height || width > INT_MAX / 4 || height > INT_MAX / width)
		return failed(name, name_size, "no picture");
	if (!SDL_CreateDirectory(directory))
		return failed(name, name_size, "the screenshots folder cannot be made");
	if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true))
		return failed(name, name_size, "no clock");
	path_size = strlen(directory) + 1 + name_size + 5;
	path = SDL_malloc(path_size);
	if (!path)
		return failed(name, name_size, "out of memory");
	for (collision = 0; collision < SCREENSHOT_COLLISIONS; collision++)
	{
		host_screenshot_name(name, name_size, date.year, date.month, date.day, date.hour, date.minute, date.second,
			collision);
		snprintf(path, path_size, "%s/%s.part", directory, name);
		if (SDL_GetPathInfo(path, NULL))
			continue;
		path[strlen(path) - 5] = 0;
		if (!SDL_GetPathInfo(path, NULL))
			break;
	}
	if (collision == SCREENSHOT_COLLISIONS)
	{
		SDL_free(path);
		return failed(name, name_size, "too many in one second");
	}
	/* (on a thread of its own, or, if none takes it, here) */
	written = job_start(bgra, width, height, path) || png_write(bgra, width, height, path, NULL);
	SDL_free(path);
	if (!written)
		return failed(name, name_size, "the file cannot be written");
	return 1;
}
