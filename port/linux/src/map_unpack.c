/*
MAP_UNPACK.C

A stock map unpacked into a cache slot (z:\cacheNNN.map, source/cache/
cache_files_windows.c) off the game's thread, on a few workers of its own,
so that the first start's menus' pictures' pass (port/linux/game/
menu_pictures_render.c) finds each map whole as it reaches it, instead of
waiting on the game's one decompressor (cache_files_decompress_windows.c,
the Xbox's, one map at a time) for each.

An Xbox map is its header (struct cache_file_header, 0x800 bytes) and one
zlib stream of the rest: the map's file_length less the header, inflated.
The slot written is the precache's, byte for byte: the header at 0, the
inflated map after it, and zeros to the slot's size (the map's length
rounded up to 64 KB); its pages put on the disk, then its header written
and put on the disk, and only then the slot's source file (xbox_files.c's
cache_slot_source_record, version 3), the mark of a whole copy. A slot cut
short anywhere before has no mark and is copied again, never played.

Each unpack has its own zlib stream and buffers: any number run at once,
each into its own slot. The game's thread chooses the slot and holds it
while a worker fills it (cache_files_unpack_begin), and takes it back
through the same check the slots get as the game starts
(cache_files_unpack_update).
*/

#include "platform.h"
#include "../../third_party/zlib/zlib_prefixed.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* (xbox_files.c's cache slots' sources) */
void cache_slot_source_record(short slot, const char *map_path, const char *name, unsigned long checksum,
	unsigned long file_length);
int cache_slot_source_matches(short slot, const char *map_path, const char *name, unsigned long checksum,
	unsigned long file_length);

/* ---------- constants */

enum
{
	MAP_UNPACK_HEADER_SIZE = 0x800,
	MAP_UNPACK_FILE_LENGTH_OFFSET = 0x8,
	MAP_UNPACK_NAME_OFFSET = 0x20,
	MAP_UNPACK_NAME_SIZE = 0x20,
	MAP_UNPACK_CHECKSUM_OFFSET = 0x64,
	MAP_UNPACK_READ_SIZE = 0x40000,
	MAP_UNPACK_WRITE_SIZE = 0x400000,
	/* (cache_files_windows.c's NUMBER_OF_CACHED_MAP_FILES) */
	MAP_UNPACK_SLOTS = HALO_PORT_CACHED_MAP_FILES,
	MAP_UNPACK_MOST_WORKERS = 4,
};

enum map_unpack_state
{
	_map_unpack_free,
	_map_unpack_queued,
	_map_unpack_running,
	_map_unpack_done,
};

/* ---------- structures */

struct map_unpack_job
{
	enum map_unpack_state state;
	int succeeded;
	/* the game's own copy (the precache's: taken only by map_unpack_take),
	the copy asked to stop, how far it has come (0 to 1) */
	int precache;
	volatile int stop;
	volatile float progress;
	long copy_size;
	char map_path[1024], slot_path[1024];
	/* (the map's path as the slot's source names it: cache_slot_source_record) */
	char map_xbox_path[256];
};

/* ---------- globals */

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t wake, finished;
	int workers;
	struct map_unpack_job jobs[MAP_UNPACK_SLOTS];
} map_unpack = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

/* ---------- private code */

static unsigned long map_unpack_word(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static int map_unpack_write_all(int descriptor, const void *buffer, size_t size, off_t offset)
{
	const unsigned char *bytes = buffer;

	while (size)
	{
		ssize_t written = pwrite(descriptor, bytes, size, offset);

		if (written <= 0)
			return 0;
		bytes += written;
		size -= (size_t)written;
		offset += written;
	}
	return 1;
}

/* ---------- public code */

int map_unpack_slot(const char *map_path, const char *slot_path, long copy_size, char *name, unsigned long *checksum,
	unsigned long *file_length, volatile int *stop, volatile float *progress)
{
	unsigned char header[MAP_UNPACK_HEADER_SIZE];
	unsigned char *in = NULL, *out = NULL;
	z_stream stream;
	int source = -1, slot = -1, inflating = 0, whole = 0;
	unsigned long length, body, done = 0;
	off_t read_offset = MAP_UNPACK_HEADER_SIZE;

	source = open(map_path, O_RDONLY | O_CLOEXEC);
	if (source < 0 || pread(source, header, sizeof(header), 0) != (ssize_t)sizeof(header))
		goto finished;
	length = map_unpack_word(header + MAP_UNPACK_FILE_LENGTH_OFFSET);
	if (length < MAP_UNPACK_HEADER_SIZE || (long)length > copy_size || !memchr(header + MAP_UNPACK_NAME_OFFSET, 0,
			MAP_UNPACK_NAME_SIZE))
	{
		goto finished;
	}
	body = length - MAP_UNPACK_HEADER_SIZE;
	in = malloc(MAP_UNPACK_READ_SIZE);
	out = malloc(MAP_UNPACK_WRITE_SIZE);
	memset(&stream, 0, sizeof(stream));
	if (!in || !out || inflateInit(&stream) != Z_OK)
		goto finished;
	inflating = 1;
	/* (emptied, then its size: zeros past the map, as the precache's
	cached_map_file_resize leaves them, and no header until the end) */
	slot = open(slot_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (slot < 0 || ftruncate(slot, 0) != 0 || ftruncate(slot, (off_t)copy_size) != 0)
		goto finished;
	while (done < body)
	{
		int result;

		if (stop && *stop)
			goto finished;
		if (progress)
			*progress = (float)done / (float)body;

		if (!stream.avail_in)
		{
			ssize_t count = pread(source, in, MAP_UNPACK_READ_SIZE, read_offset);

			if (count <= 0)
				goto finished;
			read_offset += count;
			stream.next_in = in;
			stream.avail_in = (uInt)count;
		}
		stream.next_out = out;
		stream.avail_out = (uInt)(body - done < MAP_UNPACK_WRITE_SIZE ? body - done : MAP_UNPACK_WRITE_SIZE);
		result = inflate(&stream, Z_NO_FLUSH);
		if (result != Z_OK && result != Z_STREAM_END)
			goto finished;
		{
			unsigned long produced = (unsigned long)(stream.next_out - out);

			if (produced && !map_unpack_write_all(slot, out, produced, (off_t)(MAP_UNPACK_HEADER_SIZE + done)))
				goto finished;
			done += produced;
		}
		/* (a stream that ends before the map's length is a bad file, as the
		precache has it; one that goes on past it is read no further) */
		if (result == Z_STREAM_END && done != body)
			goto finished;
	}
	/* (its pages on the disk, then its header, itself on the disk) */
	if (fsync(slot) != 0 || !map_unpack_write_all(slot, header, sizeof(header), 0) || fsync(slot) != 0)
		goto finished;
	if (name)
		memcpy(name, header + MAP_UNPACK_NAME_OFFSET, MAP_UNPACK_NAME_SIZE);
	if (checksum)
		*checksum = map_unpack_word(header + MAP_UNPACK_CHECKSUM_OFFSET);
	if (file_length)
		*file_length = length;
	whole = 1;

finished:
	if (inflating)
		inflateEnd(&stream);
	free(in);
	free(out);
	if (slot >= 0 && close(slot) != 0)
		whole = 0;
	if (source >= 0)
		close(source);
	return whole;
}

/* the job's slot unpacked, and only if it is whole, its source written:
the mark, last */
static int map_unpack_job_run(short slot, struct map_unpack_job *job)
{
	char name[MAP_UNPACK_NAME_SIZE];
	unsigned long checksum = 0, file_length = 0;

	if (!map_unpack_slot(job->map_path, job->slot_path, job->copy_size, name, &checksum, &file_length, &job->stop,
			&job->progress))
		return 0;
	cache_slot_source_record(slot, job->map_xbox_path, name, checksum, file_length);
	return cache_slot_source_matches(slot, job->map_xbox_path, name, checksum, file_length);
}

static void *map_unpack_worker(void *context)
{
	(void)context;
	pthread_mutex_lock(&map_unpack.lock);
	for (;;)
	{
		short slot;

		/* (the game's own copy first: a load waits for it) */
		for (slot = 0; slot < MAP_UNPACK_SLOTS &&
			!(map_unpack.jobs[slot].state == _map_unpack_queued && map_unpack.jobs[slot].precache); slot++)
			;
		if (slot >= MAP_UNPACK_SLOTS)
		{
			for (slot = 0; slot < MAP_UNPACK_SLOTS && map_unpack.jobs[slot].state != _map_unpack_queued; slot++)
				;
		}
		if (slot >= MAP_UNPACK_SLOTS)
		{
			pthread_cond_wait(&map_unpack.wake, &map_unpack.lock);
			continue;
		}
		map_unpack.jobs[slot].state = _map_unpack_running;
		pthread_mutex_unlock(&map_unpack.lock);
		{
			int succeeded = map_unpack_job_run(slot, &map_unpack.jobs[slot]);

			pthread_mutex_lock(&map_unpack.lock);
			map_unpack.jobs[slot].succeeded = succeeded;
		}
		map_unpack.jobs[slot].state = _map_unpack_done;
		pthread_cond_broadcast(&map_unpack.finished);
	}
	return NULL;
}

int map_unpack_worker_count(void)
{
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	long count = cores / 2;

	if (count < 1)
		count = 1;
	if (count > MAP_UNPACK_MOST_WORKERS)
		count = MAP_UNPACK_MOST_WORKERS;
	return (int)count;
}

/* (the lock held) the pool started, once */
static int map_unpack_start(void)
{
	int wanted = map_unpack_worker_count();

	while (map_unpack.workers < wanted)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, map_unpack_worker, NULL) != 0)
			break;
		pthread_detach(thread);
		map_unpack.workers++;
	}
	return map_unpack.workers > 0;
}

int map_unpack_queue(short slot, const char *map_xbox_path, const char *slot_xbox_path, long copy_size, int precache)
{
	struct map_unpack_job *job;
	int queued = 0;

	if (slot < 0 || slot >= MAP_UNPACK_SLOTS)
		return 0;
	pthread_mutex_lock(&map_unpack.lock);
	job = &map_unpack.jobs[slot];
	if (job->state == _map_unpack_free && map_unpack_start())
	{
		platform_translate_path(map_xbox_path, job->map_path, sizeof(job->map_path));
		platform_translate_path(slot_xbox_path, job->slot_path, sizeof(job->slot_path));
		snprintf(job->map_xbox_path, sizeof(job->map_xbox_path), "%s", map_xbox_path);
		job->copy_size = copy_size;
		job->succeeded = 0;
		job->precache = precache;
		job->stop = 0;
		job->progress = 0.0f;
		if (job->map_path[0] && job->slot_path[0])
		{
			job->state = _map_unpack_queued;
			pthread_cond_signal(&map_unpack.wake);
			queued = 1;
		}
	}
	pthread_mutex_unlock(&map_unpack.lock);
	return queued;
}

int map_unpack_take_finished(short *slot, int *succeeded)
{
	short index;
	int found = 0;

	pthread_mutex_lock(&map_unpack.lock);
	for (index = 0; index < MAP_UNPACK_SLOTS; index++)
	{
		if (map_unpack.jobs[index].state == _map_unpack_done && !map_unpack.jobs[index].precache)
		{
			*slot = index;
			*succeeded = map_unpack.jobs[index].succeeded;
			map_unpack.jobs[index].state = _map_unpack_free;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&map_unpack.lock);
	return found;
}

void map_unpack_wait(short slot)
{
	if (slot < 0 || slot >= MAP_UNPACK_SLOTS)
		return;
	pthread_mutex_lock(&map_unpack.lock);
	while (map_unpack.jobs[slot].state == _map_unpack_queued || map_unpack.jobs[slot].state == _map_unpack_running)
		pthread_cond_wait(&map_unpack.finished, &map_unpack.lock);
	pthread_mutex_unlock(&map_unpack.lock);
}

int map_unpack_status(short slot, float *progress)
{
	int status;

	if (slot < 0 || slot >= MAP_UNPACK_SLOTS)
		return -1;
	pthread_mutex_lock(&map_unpack.lock);
	if (progress)
		*progress = map_unpack.jobs[slot].progress;
	status = map_unpack.jobs[slot].state == _map_unpack_done ? (map_unpack.jobs[slot].succeeded ? 1 : -1) :
		map_unpack.jobs[slot].state == _map_unpack_free ? -1 : 0;
	pthread_mutex_unlock(&map_unpack.lock);
	return status;
}

void map_unpack_stop(short slot)
{
	if (slot >= 0 && slot < MAP_UNPACK_SLOTS)
		map_unpack.jobs[slot].stop = 1;
}

int map_unpack_take(short slot)
{
	int succeeded = 0;

	if (slot < 0 || slot >= MAP_UNPACK_SLOTS)
		return 0;
	map_unpack_wait(slot);
	pthread_mutex_lock(&map_unpack.lock);
	if (map_unpack.jobs[slot].state == _map_unpack_done)
	{
		succeeded = map_unpack.jobs[slot].succeeded;
		map_unpack.jobs[slot].state = _map_unpack_free;
	}
	pthread_mutex_unlock(&map_unpack.lock);
	return succeeded;
}
