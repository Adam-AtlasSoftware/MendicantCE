/*
XGPU_TEXTURE_STORE.C

PRISM's texture store's portable core (xgpu_texture_store.h).

An entry's image holds its levels from top down (top 0: the whole chain;
top scale: from the stock bitmap's resolution down). Its levels are copied
in as the workers read them, and resident is the most detailed level from
which every level down is in the image: the entry is usable once resident
is at most its scale. A job is one level of one entry, every face, read in
pieces of the staging buffer; a piece is copied into the image the job was
queued for (its ticket), or dropped if the entry has another image since,
or the map another generation.

The staging buffer is handed out in pieces in order, as a ring, and taken
back in that order: a piece given back waits for those handed out before
it, so that the free space is always one run (or two, wrapping).

Windows's host (port/windows/host) has no pread, and opening a guest's path
there goes through its system calls' translation, not checked yet: the
store answers every query refused there (the clean disabled path).
*/

#include "xgpu_texture_store.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define STORE_FILES 1
#else
#define STORE_FILES 0
#endif

/* the pieces of the staging buffer handed out at once */
#define PIECES 2048
#define WORKERS_MAXIMUM 8
/* entries asked for in the last this many frames are streamed */
#define WANTED_FRAMES 2u
/* the levels above the stock resolution are queued while fewer jobs than
this wait */
#define QUEUE_SHORT 4u
/* the budget read again this often (frames) */
#define BUDGET_FRAMES 60u

enum
{
	_piece_free,
	_piece_reading,
	_piece_ready,
	_piece_copied,
	_piece_released,
};

struct piece
{
	uint32_t offset, size;
	uint8_t state, failed;
	uint64_t serial;
	uint32_t generation, entry, ticket, level, face;
	uint32_t y, width, height, row_bytes;
};

struct job
{
	struct job *next;
	uint32_t generation, entry, ticket, level;
	char *path;
	uint64_t offset, face_bytes;
	uint32_t faces, blocks_wide, blocks_high, block_bytes;
	uint32_t width, height;
	/* where the reading goes on from (a job put back: the tests' thread) */
	uint32_t face_from, y_from;
};

struct entry
{
	char *path;
	struct xgpu_ktx2_file file;
	struct xgpu_ktx2_stock stock;
	int failed;
	void *image;
	uint64_t image_bytes;
	uint32_t top, ticket, resident;
	uint32_t done, queued;
	uint32_t pending[XGPU_KTX2_LEVELS_MAXIMUM];
	uint64_t last_used, first_used;
	uint32_t streak;
	double evicted_at;
	uint8_t logged_usable, logged_full, was_usable;
};

struct xgpu_texture_store
{
	struct xgpu_texture_store_backend backend;
	struct xgpu_texture_store_settings settings;
	uint32_t generation, guest_generation;
	struct entry *entries;
	uint32_t entry_count;
	uint64_t frame;
	uint32_t epoch;
	int loading;
	uint32_t budget_setting;
	uint64_t budget, resident_bytes;
	double map_time;
	struct xgpu_texture_store_statistics statistics;
	int warned_memory, warned_srgb;

	/* the workers' (store->lock) */
	struct job *queue_first, *queue_last;
	uint32_t queued_jobs;
	struct piece pieces[PIECES];
	uint32_t piece_first, piece_count;
	uint32_t ready[PIECES];
	uint32_t ready_first, ready_count;
	uint64_t read_bytes;
	int stopping;
#if STORE_FILES
	pthread_mutex_t lock;
	pthread_cond_t work, room;
	pthread_t threads[WORKERS_MAXIMUM];
	uint32_t thread_count;
#endif
};

#if STORE_FILES
#define LOCK(store) pthread_mutex_lock(&(store)->lock)
#define UNLOCK(store) pthread_mutex_unlock(&(store)->lock)
#else
#define LOCK(store) ((void)(store))
#define UNLOCK(store) ((void)(store))
#endif

/* ---------- private code */

static double seconds_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void say(struct xgpu_texture_store *store, int warning, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
static void say(struct xgpu_texture_store *store, int warning, const char *format, ...)
{
	char text[512];
	va_list arguments;

	if (!store->backend.log)
		return;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	store->backend.log(store->backend.context, warning, text);
}

/* an entry's levels from top down: their bytes */
static uint64_t levels_bytes(const struct entry *entry, uint32_t top)
{
	uint64_t bytes = 0;
	uint32_t level;

	for (level = top; level < entry->file.levels; level++)
		bytes += entry->file.level[level].length;
	return bytes;
}

static int entry_usable(const struct entry *entry)
{
	return entry->image && entry->resident <= entry->file.scale;
}

static void epoch_bump(struct xgpu_texture_store *store)
{
	store->epoch++;
}

/* the image let go of (after the work recorded so far) */
static void entry_release(struct xgpu_texture_store *store, struct entry *entry)
{
	int was_usable = entry_usable(entry);

	if (entry->image)
	{
		store->backend.image_destroy(store->backend.context, entry->image);
		store->resident_bytes -= entry->image_bytes;
	}
	entry->image = NULL;
	entry->image_bytes = 0;
	entry->top = 0;
	entry->resident = entry->file.levels;
	entry->done = entry->queued = 0;
	memset(entry->pending, 0, sizeof(entry->pending));
	entry->ticket++;
	if (was_usable)
		epoch_bump(store);
}

/* the entry's image made with its levels from top down, keeping the levels
it holds (resident and below) */
static int entry_image_make(struct xgpu_texture_store *store, struct entry *entry, uint32_t top)
{
	struct xgpu_texture_store_image_description description;
	uint64_t bytes = 0;
	void *image;
	uint32_t level, keep_from;
	int was_usable = entry_usable(entry);

	memset(&description, 0, sizeof(description));
	description.format = entry->file.format;
	description.cube = entry->file.faces == 6;
	description.width = entry->file.width >> top ? entry->file.width >> top : 1;
	description.height = entry->file.height >> top ? entry->file.height >> top : 1;
	description.levels = entry->file.levels - top;
	image = store->backend.image_create(store->backend.context, &description, &bytes);
	if (!image)
	{
		store->statistics.allocation_failures++;
		/* (the budget lowered to what the store has, less a tenth) */
		store->budget = store->resident_bytes - store->resident_bytes / 10;
		if (!store->warned_memory)
			say(store, 1, "texstore: the device's memory is out: the budget is lowered to %llu MB",
				(unsigned long long)(store->budget >> 20));
		store->warned_memory = 1;
		return 0;
	}
	keep_from = entry->resident > top ? entry->resident : top;
	if (entry->image)
	{
		for (level = keep_from; level < entry->file.levels; level++)
			store->backend.image_copy_level(store->backend.context, entry->image, level - entry->top, image, level - top);
		store->backend.image_destroy(store->backend.context, entry->image);
		store->resident_bytes -= entry->image_bytes;
	}
	else
	{
		keep_from = entry->file.levels;
	}
	entry->image = image;
	entry->image_bytes = bytes;
	store->resident_bytes += bytes;
	entry->top = top;
	entry->resident = keep_from;
	entry->done = 0;
	for (level = keep_from; level < entry->file.levels; level++)
		entry->done |= 1u << level;
	entry->queued = 0;
	memset(entry->pending, 0, sizeof(entry->pending));
	entry->ticket++;
	if (was_usable != entry_usable(entry) || was_usable)
		epoch_bump(store);
	return 1;
}

static struct xgpu_texture_store *sort_store;

static int older_first(const void *a, const void *b)
{
	const struct entry *first = &sort_store->entries[*(const uint32_t *)a];
	const struct entry *second = &sort_store->entries[*(const uint32_t *)b];

	return first->last_used < second->last_used ? -1 : first->last_used > second->last_used;
}

/* room for bytes more in the budget, from entries not drawn lately (but
keep): cut back to their stock resolution, then let go of, the least
recently drawn first; 1 if there is */
static int room_make(struct xgpu_texture_store *store, uint64_t bytes, const struct entry *keep)
{
	uint32_t *order, count = 0, index;
	int pass;

	if (store->resident_bytes + bytes <= store->budget)
		return 1;
	order = malloc((size_t)store->entry_count * sizeof(*order));
	if (!order)
		return 0;
	for (index = 0; index < store->entry_count; index++)
	{
		const struct entry *entry = &store->entries[index];

		if (entry->image && entry != keep && entry->last_used + XGPU_TEXTURE_STORE_KEEP_FRAMES < store->frame)
			order[count++] = index;
	}
	sort_store = store;
	qsort(order, count, sizeof(*order), older_first);
	for (pass = 0; pass < 2 && store->resident_bytes + bytes > store->budget; pass++)
	{
		for (index = 0; index < count && store->resident_bytes + bytes > store->budget; index++)
		{
			struct entry *entry = &store->entries[order[index]];

			if (!entry->image)
				continue;
			if (pass == 0)
			{
				/* (cut back to the stock resolution, where it has more and is
				usable: its levels from scale down copied) */
				if (entry->top < entry->file.scale && entry_usable(entry) &&
					entry_image_make(store, entry, entry->file.scale))
					store->statistics.demotions++;
			}
			else
			{
				entry_release(store, entry);
				entry->evicted_at = seconds_now();
				store->statistics.evictions++;
			}
		}
	}
	free(order);
	return store->resident_bytes + bytes <= store->budget;
}

static void budget_read(struct xgpu_texture_store *store)
{
	uint64_t budget = 0, usage = 0, chosen;

	if (store->budget_setting)
	{
		chosen = (uint64_t)store->budget_setting << 20;
		/* (no more than nine tenths of the device's budget) */
		if (store->backend.memory && store->backend.memory(store->backend.context, &budget, &usage) && budget &&
			chosen > budget - budget / 10)
			chosen = budget - budget / 10;
	}
	else if (store->backend.memory && store->backend.memory(store->backend.context, &budget, &usage) && budget)
	{
		uint64_t others = usage > store->resident_bytes ? usage - store->resident_bytes : 0;

		chosen = usage ? (budget > others ? (budget - others) / 10 * 6 : 0) : budget / 4;
		if (chosen < store->settings.budget_floor)
			chosen = store->settings.budget_floor;
		if (chosen > store->settings.budget_ceiling)
			chosen = store->settings.budget_ceiling;
	}
	else
	{
		chosen = store->settings.budget_floor;
	}
	/* (after the device's memory ran out: no more than it held then) */
	if (store->warned_memory && store->budget && chosen > store->budget)
		chosen = store->budget;
	store->budget = chosen;
}

/* ---------- the staging buffer's pieces (store->lock) */

/* a piece of size bytes, or -1 if there is no room now */
static int piece_take(struct xgpu_texture_store *store, uint32_t size)
{
	uint32_t offset, index;

	if (store->piece_count == PIECES || size > store->backend.staging_size)
		return -1;
	if (!store->piece_count)
	{
		offset = 0;
	}
	else
	{
		const struct piece *oldest = &store->pieces[store->piece_first];
		const struct piece *newest = &store->pieces[(store->piece_first + store->piece_count - 1) % PIECES];
		uint32_t head = newest->offset + newest->size;

		if (head > oldest->offset)
		{
			if (store->backend.staging_size - head >= size)
				offset = head;
			else if (oldest->offset >= size)
				offset = 0;
			else
				return -1;
		}
		else
		{
			if (oldest->offset - head >= size)
				offset = head;
			else
				return -1;
		}
	}
	index = (store->piece_first + store->piece_count) % PIECES;
	store->piece_count++;
	memset(&store->pieces[index], 0, sizeof(store->pieces[index]));
	store->pieces[index].offset = offset;
	store->pieces[index].size = size;
	store->pieces[index].state = _piece_reading;
	return (int)index;
}

/* the pieces given back, oldest first, as far as they all are */
static void pieces_trim(struct xgpu_texture_store *store)
{
	while (store->piece_count && store->pieces[store->piece_first].state == _piece_released)
	{
		store->pieces[store->piece_first].state = _piece_free;
		store->piece_first = (store->piece_first + 1) % PIECES;
		store->piece_count--;
	}
}

/* ---------- the workers */

static void job_free(struct job *job)
{
	if (!job)
		return;
	free(job->path);
	free(job);
}

/* (store->lock held) the next job, or NULL */
static struct job *job_next(struct xgpu_texture_store *store)
{
	struct job *job = store->queue_first;

	if (!job)
		return NULL;
	store->queue_first = job->next;
	if (!store->queue_first)
		store->queue_last = NULL;
	store->queued_jobs--;
	return job;
}

/* reads a job's level into pieces; wait: wait for room (a worker), else
stop where there is none (the tests' thread, which copies them): 1 if it
stopped there, its place kept in the job */
static int job_read(struct xgpu_texture_store *store, struct job *job, int wait)
{
#if STORE_FILES
	uint32_t row_bytes = job->blocks_wide * job->block_bytes;
	uint32_t rows_a_piece = row_bytes ? store->settings.piece_maximum / row_bytes : 0;
	uint32_t face, y;
	int file;

	if (!rows_a_piece)
		rows_a_piece = 1;
	file = open(job->path, O_RDONLY | O_CLOEXEC);
	for (face = job->face_from; face < job->faces; face++)
	{
		for (y = face == job->face_from ? job->y_from : 0; y < job->blocks_high; y += rows_a_piece)
		{
			uint32_t rows = job->blocks_high - y < rows_a_piece ? job->blocks_high - y : rows_a_piece;
			uint32_t size = rows * row_bytes;
			uint64_t at = job->offset + face * job->face_bytes + (uint64_t)y * row_bytes;
			struct piece *piece;
			int index, failed = 0;
			ssize_t got = 0;

			LOCK(store);
			while ((index = piece_take(store, size)) < 0 && wait && !store->stopping &&
				job->generation == store->generation)
				pthread_cond_wait(&store->room, &store->lock);
			if (index < 0)
			{
				int stopped = !wait && job->generation == store->generation;

				UNLOCK(store);
				if (file >= 0)
					close(file);
				job->face_from = face;
				job->y_from = y;
				return stopped;
			}
			piece = &store->pieces[index];
			piece->generation = job->generation;
			piece->entry = job->entry;
			piece->ticket = job->ticket;
			piece->level = job->level;
			piece->face = face;
			piece->y = y * 4;
			piece->width = job->width;
			piece->height = job->height - y * 4 < rows * 4 ? job->height - y * 4 : rows * 4;
			piece->row_bytes = row_bytes;
			UNLOCK(store);
			if (file < 0)
			{
				failed = 1;
			}
			else
			{
				uint32_t done = 0;

				while (done < size)
				{
					got = pread(file, store->backend.staging + piece->offset + done, size - done, (off_t)(at + done));
					if (got < 0 && errno == EINTR)
						continue;
					if (got <= 0)
						break;
					done += (uint32_t)got;
				}
				failed = done != size;
			}
			LOCK(store);
			piece->failed = (uint8_t)failed;
			piece->state = _piece_ready;
			store->ready[(store->ready_first + store->ready_count) % PIECES] = (uint32_t)index;
			store->ready_count++;
			if (!failed)
				store->read_bytes += size;
			UNLOCK(store);
		}
	}
	if (file >= 0)
		close(file);
	return 0;
#else
	(void)store;
	(void)job;
	(void)wait;
	return 0;
#endif
}

#if STORE_FILES
static void *worker_main(void *argument)
{
	struct xgpu_texture_store *store = argument;

	LOCK(store);
	for (;;)
	{
		struct job *job;

		while (!store->stopping && !store->queue_first)
			pthread_cond_wait(&store->work, &store->lock);
		if (store->stopping)
			break;
		job = job_next(store);
		if (job->generation != store->generation)
		{
			job_free(job);
			continue;
		}
		UNLOCK(store);
		job_read(store, job, 1);
		job_free(job);
		LOCK(store);
	}
	UNLOCK(store);
	return NULL;
}
#endif

/* (render thread) a level of an entry queued */
static void job_queue(struct xgpu_texture_store *store, uint32_t index, uint32_t level)
{
	struct entry *entry = &store->entries[index];
	const struct xgpu_ktx2_level *from = &entry->file.level[level];
	uint32_t row_bytes = from->blocks_wide * entry->file.block_bytes;
	uint32_t rows_a_piece = row_bytes ? store->settings.piece_maximum / row_bytes : 1;
	struct job *job = calloc(1, sizeof(*job));

	if (!rows_a_piece)
		rows_a_piece = 1;
	if (!job || !(job->path = strdup(entry->path)))
	{
		free(job);
		return;
	}
	job->generation = store->generation;
	job->entry = index;
	job->ticket = entry->ticket;
	job->level = level;
	job->offset = from->offset;
	job->face_bytes = from->face_bytes;
	job->faces = entry->file.faces;
	job->blocks_wide = from->blocks_wide;
	job->blocks_high = from->blocks_high;
	job->block_bytes = entry->file.block_bytes;
	job->width = entry->file.width >> level ? entry->file.width >> level : 1;
	job->height = entry->file.height >> level ? entry->file.height >> level : 1;
	entry->pending[level] = entry->file.faces * ((from->blocks_high + rows_a_piece - 1) / rows_a_piece);
	entry->queued |= 1u << level;
	LOCK(store);
	if (store->queue_last)
		store->queue_last->next = job;
	else
		store->queue_first = job;
	store->queue_last = job;
	store->queued_jobs++;
#if STORE_FILES
	pthread_cond_signal(&store->work);
#endif
	UNLOCK(store);
}

static void entry_log(struct xgpu_texture_store *store, struct entry *entry, const char *what)
{
	const char *name = strrchr(entry->path, '/');

	say(store, 0, "texstore: %s %s %ux%u %s %u levels (resident %ux%u, %.2f s after the map)", name ? name + 1 : entry->path,
		what, entry->file.width, entry->file.height, xgpu_ktx2_format_name(entry->file.format), entry->file.levels,
		entry->file.width >> entry->resident ? entry->file.width >> entry->resident : 1,
		entry->file.height >> entry->resident ? entry->file.height >> entry->resident : 1,
		seconds_now() - store->map_time);
}

/* a level copied whole: resident follows */
static void level_done(struct xgpu_texture_store *store, struct entry *entry, uint32_t level)
{
	int was_usable = entry_usable(entry);
	uint32_t before = entry->resident;

	entry->done |= 1u << level;
	entry->queued &= ~(1u << level);
	while (entry->resident > entry->top && (entry->done & (1u << (entry->resident - 1))))
		entry->resident--;
	if (entry->resident != before)
		epoch_bump(store);
	if (!was_usable && entry_usable(entry))
	{
		store->statistics.last_usable_seconds = seconds_now() - store->map_time;
		if (!entry->logged_usable)
			entry_log(store, entry, "usable");
		entry->logged_usable = 1;
	}
	if (entry->resident == 0 && before != 0)
	{
		store->statistics.last_full_seconds = seconds_now() - store->map_time;
		if (!entry->logged_full)
			entry_log(store, entry, "full");
		entry->logged_full = 1;
	}
}

/* the pieces read copied, in the order they were read, up to the frame's
bytes; the pieces copied whose submission is done given back */
static void pieces_copy(struct xgpu_texture_store *store)
{
	uint64_t completed = store->backend.completed(store->backend.context);
	uint64_t serial = store->backend.serial(store->backend.context);
	uint64_t cap = store->loading ? store->settings.loading_bytes : store->settings.frame_bytes, copied = 0;
	uint32_t index;
	int released = 0;

	LOCK(store);
	for (index = 0; index < store->piece_count; index++)
	{
		struct piece *piece = &store->pieces[(store->piece_first + index) % PIECES];

		if (piece->state == _piece_copied && piece->serial <= completed)
		{
			piece->state = _piece_released;
			released = 1;
		}
	}
	while (store->ready_count)
	{
		struct piece *piece = &store->pieces[store->ready[store->ready_first]];
		struct entry *entry = piece->generation == store->generation && piece->entry < store->entry_count ?
			&store->entries[piece->entry] : NULL;

		if (entry && entry->ticket == piece->ticket && entry->image && !piece->failed)
		{
			struct xgpu_texture_store_copy copy;

			if (copied && copied + piece->size > cap)
				break;
			memset(&copy, 0, sizeof(copy));
			copy.image = entry->image;
			copy.level = piece->level - entry->top;
			copy.face = piece->face;
			copy.y = piece->y;
			copy.width = piece->width;
			copy.height = piece->height;
			copy.staging_offset = piece->offset;
			copy.bytes = piece->size;
			copy.row_bytes = piece->row_bytes;
			UNLOCK(store);
			store->backend.copy(store->backend.context, &copy);
			LOCK(store);
			copied += piece->size;
			piece->state = _piece_copied;
			piece->serial = serial;
			store->statistics.pieces++;
			if (entry->pending[piece->level] && !--entry->pending[piece->level])
			{
				UNLOCK(store);
				level_done(store, entry, piece->level);
				LOCK(store);
			}
		}
		else
		{
			if (entry && entry->ticket == piece->ticket && piece->failed && !entry->failed)
			{
				entry->failed = 1;
				say(store, 1, "texstore: %s cannot be read: drawn as it is now", entry->path);
			}
			piece->state = _piece_released;
			released = 1;
		}
		store->ready_first = (store->ready_first + 1) % PIECES;
		store->ready_count--;
	}
	if (released)
	{
		pieces_trim(store);
#if STORE_FILES
		pthread_cond_broadcast(&store->room);
#endif
	}
	store->statistics.read_bytes = store->read_bytes;
	store->statistics.queued_jobs = store->queued_jobs;
	UNLOCK(store);
	store->statistics.uploaded_bytes += copied;
	if (copied > store->statistics.frame_bytes_most)
		store->statistics.frame_bytes_most = copied;
}

/* the work the entries drawn lately need queued: images made, the levels
up to the stock resolution of those not usable, then one level above it
at a time while the queue is short; images of entries drawn in a row made
whole where the budget has room */
static void work_queue(struct xgpu_texture_store *store)
{
	uint32_t index;
	double now = 0.0;

	for (index = 0; index < store->entry_count; index++)
	{
		struct entry *entry = &store->entries[index];
		uint32_t level, bottom;

		if (entry->failed || !entry->last_used || entry->last_used + WANTED_FRAMES < store->frame || entry_usable(entry))
			continue;
		if (!entry->image)
		{
			if (!now)
				now = seconds_now();
			if (entry->evicted_at && now - entry->evicted_at < XGPU_TEXTURE_STORE_THRASH_SECONDS &&
				entry->streak < XGPU_TEXTURE_STORE_THRASH_FRAMES)
				continue;
			/* (the whole chain where it has room, else from the stock
			resolution down, else it waits, drawn stock) */
			if (room_make(store, levels_bytes(entry, 0), entry))
			{
				if (!entry_image_make(store, entry, 0))
					continue;
			}
			else if (room_make(store, levels_bytes(entry, entry->file.scale), entry))
			{
				if (!entry_image_make(store, entry, entry->file.scale))
					continue;
			}
			else
			{
				continue;
			}
		}
		bottom = entry->file.scale > entry->top ? entry->file.scale : entry->top;
		for (level = entry->file.levels; level-- > bottom;)
		{
			if (!(entry->done & (1u << level)) && !(entry->queued & (1u << level)))
				job_queue(store, index, level);
		}
	}
	for (index = 0; index < store->entry_count; index++)
	{
		struct entry *entry = &store->entries[index];
		uint32_t level;

		uint32_t queued;

		LOCK(store);
		queued = store->queued_jobs;
		UNLOCK(store);
		if (queued >= QUEUE_SHORT)
			break;
		if (entry->failed || !entry->image || !entry_usable(entry) ||
			entry->last_used + WANTED_FRAMES < store->frame)
			continue;
		/* (made whole, where it was made from the stock resolution) */
		if (entry->top && entry->resident == entry->top)
		{
			if (!now)
				now = seconds_now();
			if (entry->evicted_at && now - entry->evicted_at < XGPU_TEXTURE_STORE_THRASH_SECONDS &&
				entry->streak < XGPU_TEXTURE_STORE_THRASH_FRAMES)
				continue;
			if (!room_make(store, levels_bytes(entry, 0) > entry->image_bytes ?
				levels_bytes(entry, 0) - entry->image_bytes : 0, entry) ||
				!entry_image_make(store, entry, 0))
				continue;
			store->statistics.promotions++;
		}
		if (entry->resident > entry->top)
		{
			level = entry->resident - 1;
			if (!(entry->queued & (1u << level)) && !(entry->done & (1u << level)))
				job_queue(store, index, level);
		}
	}
}

/* (store->lock) the queue's jobs let go of */
static void queue_clear(struct xgpu_texture_store *store)
{
	struct job *job;

	while ((job = job_next(store)) != NULL)
		job_free(job);
}

static void entries_free(struct xgpu_texture_store *store)
{
	uint32_t index;

	for (index = 0; index < store->entry_count; index++)
	{
		entry_release(store, &store->entries[index]);
		free(store->entries[index].path);
	}
	free(store->entries);
	store->entries = NULL;
	store->entry_count = 0;
	store->resident_bytes = 0;
}

static int ends_with(const char *text, const char *end)
{
	size_t length = strlen(text), end_length = strlen(end), index;

	if (length < end_length)
		return 0;
	for (index = 0; index < end_length; index++)
	{
		char character = text[length - end_length + index];

		if (character >= 'A' && character <= 'Z')
			character = (char)(character - 'A' + 'a');
		if (character != end[index])
			return 0;
	}
	return 1;
}

/* a query's file opened and its header checked: XGPU_KTX2_*, and the file */
static int query_check(struct xgpu_texture_store *store, const struct xgpu_store_query *query,
	struct xgpu_ktx2_file *file, char **path)
{
#if STORE_FILES
	unsigned char header[XGPU_KTX2_INDEX_BYTES];
	char resolved[PATH_MAX];
	struct stat information;
	ssize_t got;
	int descriptor, status;

	*path = NULL;
	if (!query->path || !query->path[0] || !ends_with(query->path, ".ktx2"))
		return XGPU_KTX2_PATH;
	if (!realpath(query->path, resolved))
		return XGPU_KTX2_UNREADABLE;
	/* (a file in a mods folder alone, whatever links lead there) */
	if (!strstr(resolved, "/mods/") || !ends_with(resolved, ".ktx2"))
		return XGPU_KTX2_PATH;
	descriptor = open(resolved, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return XGPU_KTX2_UNREADABLE;
	if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode))
	{
		close(descriptor);
		return XGPU_KTX2_UNREADABLE;
	}
	if ((uint64_t)information.st_size != query->file_size || (uint32_t)information.st_mtime != query->stamp_seconds)
	{
		close(descriptor);
		return XGPU_KTX2_CHANGED;
	}
	do
		got = pread(descriptor, header, sizeof(header), 0);
	while (got < 0 && errno == EINTR);
	close(descriptor);
	if (got < (ssize_t)XGPU_KTX2_HEADER_BYTES)
		return XGPU_KTX2_UNREADABLE;
	status = xgpu_ktx2_parse(header, (uint32_t)got, (uint64_t)information.st_size, &query->stock,
		store->settings.side_maximum, file);
	if (status != XGPU_KTX2_ACCEPTED)
		return status;
	if (store->backend.format_supported && !store->backend.format_supported(store->backend.context, file->format))
		return XGPU_KTX2_DEVICE;
	if (file->srgb && !store->warned_srgb)
	{
		say(store, 0, "texstore: %s is sRGB: sampled as UNORM, as the stock textures are", resolved);
		store->warned_srgb = 1;
	}
	*path = strdup(resolved);
	return *path ? XGPU_KTX2_ACCEPTED : XGPU_KTX2_UNREADABLE;
#else
	(void)store;
	(void)query;
	(void)file;
	*path = NULL;
	return XGPU_KTX2_DEVICE;
#endif
}

/* ---------- public code */

struct xgpu_texture_store *xgpu_texture_store_create(const struct xgpu_texture_store_backend *backend,
	const struct xgpu_texture_store_settings *settings)
{
	struct xgpu_texture_store *store = calloc(1, sizeof(*store));

	if (!store)
		return NULL;
	store->backend = *backend;
	store->settings = *settings;
	if (!store->settings.piece_maximum || store->settings.piece_maximum > store->backend.staging_size / 2)
		store->settings.piece_maximum = store->backend.staging_size / 2;
	if (!store->settings.side_maximum)
		store->settings.side_maximum = XGPU_KTX2_SIDE_MAXIMUM;
	if (!store->settings.budget_ceiling)
		store->settings.budget_ceiling = 6ull << 30;
	if (store->settings.workers > WORKERS_MAXIMUM)
		store->settings.workers = WORKERS_MAXIMUM;
	store->budget = store->settings.budget_floor;
#if STORE_FILES
	pthread_mutex_init(&store->lock, NULL);
	pthread_cond_init(&store->work, NULL);
	pthread_cond_init(&store->room, NULL);
	for (store->thread_count = 0; store->thread_count < store->settings.workers; store->thread_count++)
	{
		if (pthread_create(&store->threads[store->thread_count], NULL, worker_main, store))
			break;
	}
#endif
	return store;
}

void xgpu_texture_store_destroy(struct xgpu_texture_store *store)
{
	if (!store)
		return;
	LOCK(store);
	store->stopping = 1;
	store->generation++;
	queue_clear(store);
#if STORE_FILES
	pthread_cond_broadcast(&store->work);
	pthread_cond_broadcast(&store->room);
#endif
	UNLOCK(store);
#if STORE_FILES
	{
		uint32_t index;

		for (index = 0; index < store->thread_count; index++)
			pthread_join(store->threads[index], NULL);
	}
	pthread_mutex_destroy(&store->lock);
	pthread_cond_destroy(&store->work);
	pthread_cond_destroy(&store->room);
#endif
	entries_free(store);
	free(store);
}

void xgpu_texture_store_map(struct xgpu_texture_store *store, uint32_t generation, uint32_t budget_megabytes,
	int loading, uint32_t count, const struct xgpu_store_query *queries,
	struct xgpu_store_answer *answers)
{
	uint32_t index, accepted = 0, refused[XGPU_KTX2_PATH + 1];
	double started = seconds_now();

	memset(refused, 0, sizeof(refused));
	LOCK(store);
	store->generation++;
	queue_clear(store);
#if STORE_FILES
	pthread_cond_broadcast(&store->room);
#endif
	UNLOCK(store);
	entries_free(store);
	epoch_bump(store);
	store->guest_generation = generation;
	store->loading = loading;
	store->budget_setting = budget_megabytes;
	store->map_time = started;
	memset(&store->statistics, 0, sizeof(store->statistics));
	store->warned_memory = 0;
	budget_read(store);
	if (count > XGPU_TEXTURE_STORE_ENTRIES_MAXIMUM)
		count = XGPU_TEXTURE_STORE_ENTRIES_MAXIMUM;
	store->entries = count ? calloc(count, sizeof(*store->entries)) : NULL;
	if (count && !store->entries)
		count = 0;
	for (index = 0; index < count; index++)
	{
		struct xgpu_store_answer *answer = &answers[index];
		struct xgpu_ktx2_file file;
		char *path = NULL;
		int status = query_check(store, &queries[index], &file, &path);

		memset(answer, 0, sizeof(*answer));
		answer->status = (uint32_t)status;
		if (status != XGPU_KTX2_ACCEPTED)
		{
			refused[status]++;
			say(store, 1, "texstore: %s refused (%s)", queries[index].path ? queries[index].path : "(none)",
				xgpu_ktx2_status_name(status));
			continue;
		}
		{
			struct entry *entry = &store->entries[store->entry_count];

			entry->path = path;
			entry->file = file;
			entry->stock = queries[index].stock;
			entry->resident = file.levels;
			answer->id = XGPU_TEXTURE_STORE_ID(generation, store->entry_count);
			answer->width = file.width;
			answer->height = file.height;
			answer->levels = file.levels;
			answer->format = file.format;
			store->entry_count++;
			accepted++;
		}
	}
	if (count)
		say(store, 0, "texstore: generation %u: %u files, %u accepted, %u refused (%u size, %u format, %u unreadable, "
			"%u changed, %u device, %u path) in %.1f ms; budget %llu MB", generation, count, accepted, count - accepted,
			refused[XGPU_KTX2_SIZE], refused[XGPU_KTX2_INVALID], refused[XGPU_KTX2_UNREADABLE],
			refused[XGPU_KTX2_CHANGED], refused[XGPU_KTX2_DEVICE], refused[XGPU_KTX2_PATH],
			(seconds_now() - started) * 1000.0, (unsigned long long)(store->budget >> 20));
}

void xgpu_texture_store_budget(struct xgpu_texture_store *store, uint32_t budget_megabytes)
{
	store->budget_setting = budget_megabytes;
	budget_read(store);
}

void xgpu_texture_store_loading(struct xgpu_texture_store *store, int loading)
{
	store->loading = loading;
}

int xgpu_texture_store_use(struct xgpu_texture_store *store, uint32_t id, struct xgpu_texture_store_view *view)
{
	uint32_t index = (id & 0xfffffu) - 1;
	struct entry *entry;

	store->statistics.uses++;
	if (!id || (id >> 20) != (store->guest_generation & 0xfffu) || index >= store->entry_count)
	{
		store->statistics.stale_uses++;
		return 0;
	}
	entry = &store->entries[index];
	if (entry->last_used != store->frame)
	{
		entry->streak = entry->last_used + 1 == store->frame ? entry->streak + 1 : 1;
		if (!entry->first_used)
			entry->first_used = store->frame;
		entry->last_used = store->frame;
	}
	if (!entry_usable(entry))
		return 0;
	view->image = entry->image;
	view->top = entry->top;
	view->resident = entry->resident;
	view->scale = entry->file.scale;
	view->levels = entry->file.levels;
	view->format = entry->file.format;
	view->cube = entry->file.faces == 6;
	view->stock = entry->stock;
	return 1;
}

void xgpu_texture_store_frame(struct xgpu_texture_store *store)
{
	store->frame++;
	if (store->frame % BUDGET_FRAMES == 0)
		budget_read(store);
	if (!store->entry_count)
		return;
	pieces_copy(store);
	/* (a budget lowered: what is over it let go of, where it may be) */
	if (store->resident_bytes > store->budget)
		room_make(store, 0, NULL);
	work_queue(store);
}

uint32_t xgpu_texture_store_epoch(const struct xgpu_texture_store *store)
{
	return store->epoch;
}

void xgpu_texture_store_statistics(const struct xgpu_texture_store *store,
	struct xgpu_texture_store_statistics *statistics)
{
	uint32_t index;

	*statistics = store->statistics;
	statistics->entries = store->entry_count;
	statistics->resident_bytes = store->resident_bytes;
	statistics->budget_bytes = store->budget;
	for (index = 0; index < store->entry_count; index++)
	{
		const struct entry *entry = &store->entries[index];

		if (entry->failed)
			statistics->failed++;
		if (entry_usable(entry))
			statistics->usable++;
		if (entry->image && entry->resident == 0)
			statistics->full++;
		if (entry->queued)
			statistics->streaming++;
		if (entry->last_used && entry->last_used + XGPU_TEXTURE_STORE_KEEP_FRAMES >= store->frame)
			statistics->drawn++;
	}
}

const char *xgpu_texture_store_describe(const struct xgpu_texture_store *store, uint32_t id,
	struct xgpu_texture_store_view *view, uint32_t *state)
{
	uint32_t index = (id & 0xfffffu) - 1;
	const struct entry *entry;

	if (!id || (id >> 20) != (store->guest_generation & 0xfffu) || index >= store->entry_count)
		return NULL;
	entry = &store->entries[index];
	memset(view, 0, sizeof(*view));
	view->image = entry->image;
	view->top = entry->top;
	view->resident = entry->resident;
	view->scale = entry->file.scale;
	view->levels = entry->file.levels;
	view->format = entry->file.format;
	view->cube = entry->file.faces == 6;
	view->stock = entry->stock;
	*state = entry->failed ? XGPU_TEXTURE_STORE_FAILED : entry->image && entry->resident == 0 ? XGPU_TEXTURE_STORE_FULL :
		entry_usable(entry) ? (entry->top ? XGPU_TEXTURE_STORE_DEMOTED : XGPU_TEXTURE_STORE_USABLE) :
		entry->queued ? XGPU_TEXTURE_STORE_STREAMING : XGPU_TEXTURE_STORE_INDEXED;
	return entry->path;
}

int xgpu_texture_store_work(struct xgpu_texture_store *store)
{
	struct job *job;

	LOCK(store);
	job = job_next(store);
	UNLOCK(store);
	if (!job)
		return 0;
	if (job->generation == store->generation && job_read(store, job, 0))
	{
		/* (no room: put back first, read on once pieces are given back) */
		LOCK(store);
		job->next = store->queue_first;
		store->queue_first = job;
		if (!store->queue_last)
			store->queue_last = job;
		store->queued_jobs++;
		UNLOCK(store);
		return 0;
	}
	job_free(job);
	return 1;
}
