/*
WORLD_SDF.C

display.ambient_occlusion "world"'s distance field of the structure
(world_sdf.h, docs/world-ao.md): found in the data folder's world_sdf, or
baked in the background (never during a load), and uploaded a few slices a
frame. Workstream B; the bake's arithmetic is world_sdf_bake.c.

The schedule (docs/world-ao.md, "The bake's schedule"):
- a map loaded (scenario_load: world_sdf_map_loaded) or a BSP switched to
  only asks the looker thread to find its field in the cache: no file is
  read in the load;
- a field missing from the cache is baked from a copy of the BSP's
  geometry (world_scene_static_triangles) and of its collision BSP's planes
  (the sign's point test), taken on the game's thread a few frames after
  the load and handed to the baker thread, which bakes it in slabs of z
  slices on every processor at the lowest priority, writes it to the cache
  and hands it back;
- the first launch's "Preparing menu pictures" pass (menu_pictures_render.c)
  bakes the BSPs of every stock map it loads (the one each map starts in,
  and each picture's), where nobody waits to play
  (world_sdf_startup_map_loaded), and runs for them alone when only fields
  are missing (world_sdf_startup_wanted). Only the loaded BSP's tags are
  in memory, and the pass switches to no other BSP for them (repeated
  switches once leaked the device's groups, #506): a campaign map's other
  BSPs are baked in the background the first time the game switches to
  them (world_sdf_bsp_switched: a look in the cache, the copy a few frames
  later, never in the switch), the proxies alone shading them till then;
- a field found or baked is uploaded at most WORLD_SDF_UPLOAD_BYTES a frame,
  then fades in over a second (world_sdf_fade); until then the proxies
  alone shade.

A bake outlives the map it was taken from (it works on copies): its field is
written to the cache all the same, and found there the next time.

The cache (<data>/world_sdf/<map>_<bsp>.wsdf, world_sdf.h's struct
world_sdf_header): the header written field by field, little-endian, the
voxels deflated, a CRC-32 of both; written to a .tmp beside it and renamed
into place, and a file that fails any check is deleted (and baked again).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp_definitions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

#include "memory/zlib/zlib.h"

#include "menu_pictures.h"
#include "world_ao.h"
#include "world_scene.h"
#include "world_sdf.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* (the C library's allocator, which every thread may call, not the game's
debug one, cseries.h's: the bake's threads allocate, and its fields are
not the game's memory) */
#undef malloc
#undef free
#undef realloc

/* ---------- constants */

enum
{
	/* the most bytes of the field uploaded a frame */
	WORLD_SDF_UPLOAD_BYTES = 512 * 1024,
	/* the frames after a load before a missing field's geometry is copied */
	WORLD_SDF_COPY_DELAY_FRAMES = 2,
	/* the z slices a worker takes at a time */
	WORLD_SDF_SLAB_SLICES = 2,
	/* the most bake threads */
	WORLD_SDF_MAXIMUM_THREADS = 32,
	/* the most bakes waiting (the oldest waiting dropped past it) */
	WORLD_SDF_MAXIMUM_BAKES = 64,
	/* the most fields the start-up pass has tried */
	WORLD_SDF_MAXIMUM_TRIED = 64,
	/* the cache file's header, and its CRC after the deflated voxels */
	WORLD_SDF_FILE_HEADER = 64,
	WORLD_SDF_FILE_CRC = 4,
	/* the deflate's level */
	WORLD_SDF_DEFLATE_LEVEL = 6,
};

/* the fade in, in nanoseconds */
#define WORLD_SDF_FADE_NANOSECONDS 1000000000ULL

/* what a job is, and what came of it */
enum
{
	_sdf_job_look, /* the cache looked in */
	_sdf_job_bake, /* baked from the copies */
};

enum
{
	_sdf_result_found,
	_sdf_result_missing,
	_sdf_result_baked,
	_sdf_result_failed,
};

/* ---------- prototypes */

/* the platform layer's */
const char *platform_data_root(void);
void platform_log(char const *format, ...);
void halo_load_trace(char const *phase, char const *detail);
int config_boolean(const char *name);
double config_real(const char *name);
const char *config_string(const char *name);
int halo_server_mode(void);
unsigned long long SDL_GetTicksNS(void);
/* the guest runtime's system calls (port/android/guest/runtime) */
long __guest_syscall(long long number, long long a, long long b, long long c, long long d, long long e, long long f);
/* (sys/stat.h's, which the game's strict ANSI hides; not translated, so
given the folder's whole path) */
int mkdir(const char *path, unsigned int mode);
void halo_world_ao_field(unsigned long width, unsigned long height, unsigned long depth, unsigned long z_first,
	unsigned long z_count, signed char const *voxels);
/* the port's zlib (port/third_party/zlib/zlib_prefixed.h: its inflate,
for data from anywhere, whose allocator is the C library's) */
int z_uncompress(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size);

/* ---------- structures */

/* a copy of a collision BSP's bsp3d: its nodes (plane, back child, front
child) and planes (i j k d), for its point test on another thread */
struct sdf_bsp
{
	long node_count, plane_count;
	long (*nodes)[3];
	float (*planes)[4];
};

/* a field's name: its map (and that map's checksum) and BSP */
struct sdf_key
{
	char map[64];
	unsigned long checksum;
	short bsp;
};

struct sdf_job
{
	struct sdf_job *next;
	int kind;
	struct sdf_key key;
	/* a bake's copies */
	float *positions;
	long triangle_count;
	float bounds[6];
	struct sdf_bsp bsp;
	/* the bake's field, and the next slab a worker takes */
	struct world_sdf_volume volume;
	long next_slice;
	/* what came of it; the game's thread's time copying the geometry */
	int result;
	double copy_milliseconds;
};

/* ---------- globals */

/* (the threads' and the game's) */
static pthread_mutex_t world_sdf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t world_sdf_look_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t world_sdf_bake_wake = PTHREAD_COND_INITIALIZER;

static struct
{
	/* (under world_sdf_lock) the threads running, the jobs waiting, the
	bake running (its key), and the results for the game */
	int looker_running, baker_running;
	struct sdf_job *looks, *bakes;
	int baking;
	/* the start-up pass runs (menu_pictures_preparing, to the main menu
	shown): bakes wait, so that the pass and its main menu come exactly when
	they would without them */
	int hold;
	struct sdf_key baking_key;
	struct sdf_job *results;

	/* (the game's) the map and BSP drawn, and its field */
	int map_known;
	struct sdf_key current;
	int state;
	/* its geometry to be copied for a bake, so many frames from now */
	int copy_wanted;
	long copy_frames;
	struct world_sdf_volume field;
	unsigned long uploaded;
	int field_sent;
	unsigned long long faded_from;
	float fade;
	/* the start-up pass: the fields it has tried, and whether any is
	missing (once looked) */
	struct sdf_key tried[WORLD_SDF_MAXIMUM_TRIED];
	long tried_count;
	int startup_looked, startup_wanted;
} world_sdf;

/* ---------- the cache file (tools/harness/tests/world_sdf.c takes these) */

/* the game's zlib's deflate (its compress2 allocates with the game's own
allocator, which only the game's thread may call: the bake's thread
deflates with the C library's) */
static voidpf sdf_zalloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void sdf_zfree(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static int sdf_deflate(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size)
{
	z_stream stream;
	int result;

	memset(&stream, 0, sizeof(stream));
	stream.zalloc = sdf_zalloc;
	stream.zfree = sdf_zfree;
	if (deflateInit(&stream, WORLD_SDF_DEFLATE_LEVEL) != Z_OK)
		return -1;
	stream.next_in = (Bytef *)source;
	stream.avail_in = (uInt)source_size;
	stream.next_out = destination;
	stream.avail_out = (uInt)*destination_size;
	result = deflate(&stream, Z_FINISH);
	*destination_size = stream.total_out;
	deflateEnd(&stream);
	return result == Z_STREAM_END ? 0 : -1;
}

static void sdf_put32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

static unsigned long sdf_get32(unsigned char const *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static void sdf_put_float(unsigned char *bytes, float value)
{
	unsigned int bits;

	memcpy(&bits, &value, sizeof(bits));
	sdf_put32(bytes, bits);
}

static float sdf_get_float(unsigned char const *bytes)
{
	unsigned int bits = (unsigned int)sdf_get32(bytes);
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

/* the CRC-32 (zlib's and PNG's, as the menu pictures check theirs) */
static unsigned long sdf_crc(unsigned long crc, unsigned char const *bytes, unsigned long size)
{
	static unsigned long table[256];
	unsigned long index;

	if (!table[1])
	{
		for (index = 0; index < 256; index++)
		{
			unsigned long value = index;
			int bit;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xEDB88320UL ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
	}
	crc ^= 0xFFFFFFFFUL;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ bytes[index]) & 0xFF] ^ (crc >> 8);
	return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

/* the header, field by field (struct world_sdf_header's order) */
static void sdf_header_write(struct world_sdf_header const *header, unsigned char *bytes)
{
	sdf_put32(bytes + 0, header->magic);
	sdf_put32(bytes + 4, header->version);
	sdf_put32(bytes + 8, header->map_checksum);
	sdf_put32(bytes + 12, header->bsp_index);
	sdf_put32(bytes + 16, header->width);
	sdf_put32(bytes + 20, header->height);
	sdf_put32(bytes + 24, header->depth);
	sdf_put_float(bytes + 28, header->origin[0]);
	sdf_put_float(bytes + 32, header->origin[1]);
	sdf_put_float(bytes + 36, header->origin[2]);
	sdf_put_float(bytes + 40, header->voxel);
	sdf_put_float(bytes + 44, header->range);
	sdf_put32(bytes + 48, header->deflated_size);
	sdf_put32(bytes + 52, header->bake_milliseconds);
	sdf_put32(bytes + 56, header->bake_threads);
	sdf_put32(bytes + 60, 0);
}

static void sdf_header_read(unsigned char const *bytes, struct world_sdf_header *header)
{
	header->magic = sdf_get32(bytes + 0);
	header->version = sdf_get32(bytes + 4);
	header->map_checksum = sdf_get32(bytes + 8);
	header->bsp_index = sdf_get32(bytes + 12);
	header->width = sdf_get32(bytes + 16);
	header->height = sdf_get32(bytes + 20);
	header->depth = sdf_get32(bytes + 24);
	header->origin[0] = sdf_get_float(bytes + 28);
	header->origin[1] = sdf_get_float(bytes + 32);
	header->origin[2] = sdf_get_float(bytes + 36);
	header->voxel = sdf_get_float(bytes + 40);
	header->range = sdf_get_float(bytes + 44);
	header->deflated_size = sdf_get32(bytes + 48);
	header->bake_milliseconds = sdf_get32(bytes + 52);
	header->bake_threads = sdf_get32(bytes + 56);
}

/* whether a header is one of this version's, of the map and BSP, of a
size a field may have */
static int sdf_header_good(struct world_sdf_header const *header, unsigned long checksum, unsigned long bsp_index)
{
	double voxels = (double)header->width * (double)header->height * (double)header->depth;

	return header->magic == WORLD_SDF_MAGIC && header->version == WORLD_SDF_VERSION &&
		header->map_checksum == checksum && header->bsp_index == bsp_index && header->width && header->height &&
		header->depth && header->width <= WORLD_SDF_MAXIMUM_DIMENSION && header->height <= WORLD_SDF_MAXIMUM_DIMENSION &&
		header->depth <= WORLD_SDF_MAXIMUM_DIMENSION && voxels <= (double)WORLD_SDF_MAXIMUM_VOXELS &&
		header->voxel > 0.0f && header->voxel < 1000.0f &&
		header->range == WORLD_SDF_RANGE_VOXELS * header->voxel;
}

/* a field's file, whole (malloc's: its size), the header's deflated size
set; NULL if it cannot be made */
static unsigned char *sdf_file_pack(struct world_sdf_volume *volume, unsigned long *size)
{
	unsigned long count = volume->header.width * volume->header.height * volume->header.depth;
	unsigned long deflated = count + count / 1000 + 64;
	unsigned char *bytes = malloc(WORLD_SDF_FILE_HEADER + deflated + WORLD_SDF_FILE_CRC);

	if (!bytes)
		return NULL;
	if (sdf_deflate(bytes + WORLD_SDF_FILE_HEADER, &deflated, (unsigned char const *)volume->voxels, count) != 0)
	{
		free(bytes);
		return NULL;
	}
	volume->header.deflated_size = deflated;
	sdf_header_write(&volume->header, bytes);
	sdf_put32(bytes + WORLD_SDF_FILE_HEADER + deflated, sdf_crc(0, bytes, WORLD_SDF_FILE_HEADER + deflated));
	*size = WORLD_SDF_FILE_HEADER + deflated + WORLD_SDF_FILE_CRC;
	return bytes;
}

/* a field from its file's bytes, if every check passes (the voxels
malloc's); FALSE if not */
static int sdf_file_unpack(unsigned char const *bytes, unsigned long size, unsigned long checksum,
	unsigned long bsp_index, struct world_sdf_volume *volume)
{
	unsigned long count, inflated;

	volume->voxels = NULL;
	if (size < WORLD_SDF_FILE_HEADER + WORLD_SDF_FILE_CRC)
		return 0;
	sdf_header_read(bytes, &volume->header);
	if (!sdf_header_good(&volume->header, checksum, bsp_index) ||
		volume->header.deflated_size != size - WORLD_SDF_FILE_HEADER - WORLD_SDF_FILE_CRC ||
		sdf_get32(bytes + size - WORLD_SDF_FILE_CRC) != sdf_crc(0, bytes, size - WORLD_SDF_FILE_CRC))
	{
		return 0;
	}
	count = volume->header.width * volume->header.height * volume->header.depth;
	volume->voxels = malloc(count);
	if (!volume->voxels)
		return 0;
	inflated = count;
	if (z_uncompress((unsigned char *)volume->voxels, &inflated, bytes + WORLD_SDF_FILE_HEADER,
		volume->header.deflated_size) != 0 || inflated != count)
	{
		free(volume->voxels);
		volume->voxels = NULL;
		return 0;
	}
	return 1;
}

/* a file written whole or not at all (a .tmp beside it, renamed over it) */
static int sdf_file_write(char const *path, unsigned char const *bytes, unsigned long size)
{
	char temporary[1100];
	FILE *file;
	int written;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fwrite(bytes, 1, size, file) == size;
	written = fclose(file) == 0 && written;
	/* (Windows renames over nothing) */
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

/* a whole file (malloc's), at most maximum bytes; NULL if there is none */
static unsigned char *sdf_file_read(char const *path, unsigned long maximum, unsigned long *size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *bytes;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || (unsigned long)length > maximum ||
		fseek(file, 0, SEEK_SET) != 0 || !(bytes = malloc((size_t)length + 1)))
	{
		fclose(file);
		return NULL;
	}
	if (fread(bytes, 1, (size_t)length, file) != (size_t)length)
	{
		fclose(file);
		free(bytes);
		return NULL;
	}
	fclose(file);
	*size = (unsigned long)length;
	return bytes;
}

/* a cached field: 1 found (the voxels malloc's), 0 none; a file there that
fails a check is deleted, to be baked again */
static int sdf_cache_load(char const *path, unsigned long checksum, unsigned long bsp_index,
	struct world_sdf_volume *volume)
{
	unsigned long size = 0;
	unsigned char *bytes = sdf_file_read(path,
		WORLD_SDF_FILE_HEADER + WORLD_SDF_MAXIMUM_VOXELS + WORLD_SDF_MAXIMUM_VOXELS / 1000 + 64 + WORLD_SDF_FILE_CRC,
		&size);
	int found;

	volume->voxels = NULL;
	if (!bytes)
	{
		/* (a file there too large to be one) */
		FILE *file = fopen(path, "rb");

		if (file)
		{
			fclose(file);
			remove(path);
			platform_log("world sdf: %s is not a field: deleted, to be baked again", path);
		}
		return 0;
	}
	found = sdf_file_unpack(bytes, size, checksum, bsp_index, volume);
	free(bytes);
	if (!found)
	{
		remove(path);
		platform_log("world sdf: %s is damaged, of another version or another map: deleted, to be baked again",
			path);
	}
	return found;
}

/* whether a cached field's header is this version's, of the map and BSP
(the voxels not read) */
static int sdf_cache_present(char const *path, unsigned long checksum, unsigned long bsp_index)
{
	unsigned char bytes[WORLD_SDF_FILE_HEADER];
	struct world_sdf_header header;
	FILE *file = fopen(path, "rb");
	int present;

	if (!file)
		return 0;
	present = fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
	fclose(file);
	if (!present)
		return 0;
	sdf_header_read(bytes, &header);
	return sdf_header_good(&header, checksum, bsp_index);
}

/* ---------- the sign: a copy of the collision BSP's point test */

/* bsp3d_test_point's walk (source/physics/bsp3d.c) on the copy: TRUE where
it ends in no leaf (solid); a node that is none, or past the walk's
depth, is solid as there */
static int sdf_bsp_inside(void *context, float const point[3])
{
	struct sdf_bsp const *bsp = context;
	long node_index = 0;
	int depth = 0;

	if (!bsp || bsp->node_count <= 0)
		return 0;
	do
	{
		long const *node;
		float const *plane;

		if (node_index < 0 || node_index >= bsp->node_count || depth++ >= 128)
			return 1;
		node = bsp->nodes[node_index];
		if (node[0] < 0 || node[0] >= bsp->plane_count)
			return 1;
		plane = bsp->planes[node[0]];
		node_index = node[1 + (plane[0] * point[0] + plane[1] * point[1] + plane[2] * point[2] - plane[3] >= 0.0f)];
	}
	while (node_index >= 0);
	return node_index == -1;
}

/* ---------- names and keys */

static int sdf_off(void)
{
	return !(world_ao_mode() & WORLD_AO_FIELD) || config_boolean("debug.null_renderer") || halo_server_mode();
}

static char const *sdf_folder(void)
{
	static char folder[1024];

	if (!folder[0])
		snprintf(folder, sizeof(folder), "%s/world_sdf", platform_data_root());
	return folder;
}

/* <data>/world_sdf/<map>_<bsp>.wsdf, named as the game names its files,
from the data root (the game's fopen, remove and rename find them there:
port/linux/include/stdio.h); the map's name kept to letters, digits, - and
_ (a Custom Edition map's path's last part) */
static void sdf_path(struct sdf_key const *key, char *path, size_t size)
{
	char name[64];
	char const *map = key->map, *part;
	size_t index;

	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
			map = part + 1;
	}
	for (index = 0; map[index] && index < sizeof(name) - 1; index++)
	{
		char letter = map[index];

		name[index] = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
			(letter >= '0' && letter <= '9') || letter == '-' ? letter : '_';
	}
	name[index] = 0;
	snprintf(path, size, "world_sdf/%s_%d.wsdf", name, (int)key->bsp);
}

static int sdf_key_same(struct sdf_key const *a, struct sdf_key const *b)
{
	return a->checksum == b->checksum && a->bsp == b->bsp && !strcmp(a->map, b->map);
}

/* the map loaded now and its BSP; FALSE if none */
static int sdf_key_loaded(struct sdf_key *key)
{
	char const *map = cache_file_loaded_map_name();
	short bsp = global_structure_bsp_index_get();

	if (!map || !map[0])
		return 0;
	memset(key, 0, sizeof(*key));
	snprintf(key->map, sizeof(key->map), "%s", map);
	key->checksum = cache_files_get_checksum();
	key->bsp = bsp < 0 ? 0 : bsp;
	return 1;
}

/* ---------- the threads */

static unsigned long long sdf_now(void)
{
	return SDL_GetTicksNS();
}

/* the calling thread at the lowest priority (the game's threads first):
setpriority(PRIO_PROCESS, 0, 19), which the guest's C library has not, as
the runtime's dispatcher takes it (port/android/include/halo_guest_syscall.h's
GUEST_SYS_setpriority, asm-generic's number; each host lowers the calling
thread: host_syscall.c) */
static void sdf_thread_background(void)
{
	__guest_syscall(140, 0, 0, 19, 0, 0, 0);
}

static long sdf_threads(void)
{
	char const *forced = getenv("HALO_WORLD_SDF_THREADS");
	long processors = forced && atol(forced) > 0 ? atol(forced) : sysconf(_SC_NPROCESSORS_ONLN);

	if (processors < 1)
		processors = 1;
	if (processors > WORLD_SDF_MAXIMUM_THREADS)
		processors = WORLD_SDF_MAXIMUM_THREADS;
	return processors;
}

static void sdf_job_free(struct sdf_job *job)
{
	if (!job)
		return;
	free(job->positions);
	free(job->bsp.nodes);
	free(job->bsp.planes);
	free(job->volume.voxels);
	free(job);
}

/* (under the lock) a job's result handed to the game */
static void sdf_result_locked(struct sdf_job *job, int result)
{
	struct sdf_job **last = &world_sdf.results;

	free(job->positions);
	free(job->bsp.nodes);
	free(job->bsp.planes);
	job->positions = NULL;
	job->bsp.nodes = NULL;
	job->bsp.planes = NULL;
	job->result = result;
	job->next = NULL;
	while (*last)
		last = &(*last)->next;
	*last = job;
}

static void *sdf_looker(void *context)
{
	(void)context;
	sdf_thread_background();
	for (;;)
	{
		struct sdf_job *job;
		char path[1100];
		int found;

		pthread_mutex_lock(&world_sdf_lock);
		while (!world_sdf.looks)
			pthread_cond_wait(&world_sdf_look_wake, &world_sdf_lock);
		job = world_sdf.looks;
		world_sdf.looks = job->next;
		pthread_mutex_unlock(&world_sdf_lock);
		sdf_path(&job->key, path, sizeof(path));
		found = sdf_cache_load(path, job->key.checksum, (unsigned long)job->key.bsp, &job->volume);
		pthread_mutex_lock(&world_sdf_lock);
		sdf_result_locked(job, found ? _sdf_result_found : _sdf_result_missing);
		pthread_mutex_unlock(&world_sdf_lock);
	}
	return NULL;
}

/* a bake's worker: slabs taken until there are none */
static void *sdf_bake_worker(void *context)
{
	struct sdf_job *job = context;
	long depth = (long)job->volume.header.depth;

	sdf_thread_background();
	for (;;)
	{
		long first;

		pthread_mutex_lock(&world_sdf_lock);
		first = job->next_slice;
		job->next_slice += WORLD_SDF_SLAB_SLICES;
		pthread_mutex_unlock(&world_sdf_lock);
		if (first >= depth)
			break;
		world_sdf_bake_slab(&job->volume, job->positions, job->triangle_count, sdf_bsp_inside, &job->bsp, first,
			first + WORLD_SDF_SLAB_SLICES);
	}
	return NULL;
}

/* a field baked on every processor, written to the cache; FALSE if it
could not be */
static int sdf_bake(struct sdf_job *job)
{
	pthread_t threads[WORLD_SDF_MAXIMUM_THREADS];
	long thread_count = sdf_threads(), started = 0, index;
	unsigned long long began = sdf_now();
	unsigned long count, size = 0;
	unsigned char *file;
	char path[1100];
	int written;

	memset(&job->volume.header, 0, sizeof(job->volume.header));
	world_sdf_layout(job->bounds, &job->volume.header);
	job->volume.header.magic = WORLD_SDF_MAGIC;
	job->volume.header.version = WORLD_SDF_VERSION;
	job->volume.header.map_checksum = job->key.checksum;
	job->volume.header.bsp_index = (unsigned long)job->key.bsp;
	count = job->volume.header.width * job->volume.header.height * job->volume.header.depth;
	job->volume.voxels = malloc(count);
	if (!job->volume.voxels)
		return 0;
	job->next_slice = 0;
	for (index = 1; index < thread_count; index++)
	{
		if (pthread_create(&threads[started], NULL, sdf_bake_worker, job) == 0)
			started++;
	}
	sdf_bake_worker(job);
	for (index = 0; index < started; index++)
		pthread_join(threads[index], NULL);
	job->volume.header.bake_milliseconds = (unsigned long)((sdf_now() - began) / 1000000ULL);
	job->volume.header.bake_threads = (unsigned long)(started + 1);
	sdf_path(&job->key, path, sizeof(path));
	mkdir(sdf_folder(), 0755);
	file = sdf_file_pack(&job->volume, &size);
	written = file && sdf_file_write(path, file, size);
	free(file);
	platform_log("world sdf: %s bsp %d: %ld triangles copied in %.1f ms; %lux%lux%lu voxel %.3f in %lu ms on %lu "
		"threads, %lu cached%s", job->key.map, (int)job->key.bsp, job->triangle_count, job->copy_milliseconds,
		job->volume.header.width, job->volume.header.height, job->volume.header.depth,
		(double)job->volume.header.voxel, job->volume.header.bake_milliseconds, job->volume.header.bake_threads,
		written ? size : 0UL, written ? "" : file ? " (not written: the file could not be)" :
		" (not written: the deflate failed)");
	return 1;
}

static void *sdf_baker(void *context)
{
	(void)context;
	sdf_thread_background();
	for (;;)
	{
		struct sdf_job *job;
		char path[1100];
		int result;

		pthread_mutex_lock(&world_sdf_lock);
		world_sdf.baking = 0;
		while (!world_sdf.bakes || world_sdf.hold)
			pthread_cond_wait(&world_sdf_bake_wake, &world_sdf_lock);
		job = world_sdf.bakes;
		world_sdf.bakes = job->next;
		world_sdf.baking = 1;
		world_sdf.baking_key = job->key;
		pthread_mutex_unlock(&world_sdf_lock);
		/* (baked meanwhile: found) */
		sdf_path(&job->key, path, sizeof(path));
		if (sdf_cache_load(path, job->key.checksum, (unsigned long)job->key.bsp, &job->volume))
			result = _sdf_result_found;
		else
			result = sdf_bake(job) ? _sdf_result_baked : _sdf_result_failed;
		pthread_mutex_lock(&world_sdf_lock);
		sdf_result_locked(job, result);
		pthread_mutex_unlock(&world_sdf_lock);
	}
	return NULL;
}

/* a job handed to its thread (made the first time; detached: it ends with
the game) */
static void sdf_queue(struct sdf_job *job)
{
	int bake = job->kind == _sdf_job_bake;
	struct sdf_job **last;
	long waiting = 0;
	int *running;

	pthread_mutex_lock(&world_sdf_lock);
	running = bake ? &world_sdf.baker_running : &world_sdf.looker_running;
	if (!*running)
	{
		pthread_t thread;
		pthread_attr_t attributes;

		pthread_attr_init(&attributes);
		pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
		*running = pthread_create(&thread, &attributes, bake ? sdf_baker : sdf_looker, NULL) == 0;
		pthread_attr_destroy(&attributes);
		if (!*running)
		{
			sdf_result_locked(job, _sdf_result_failed);
			pthread_mutex_unlock(&world_sdf_lock);
			return;
		}
	}
	last = bake ? &world_sdf.bakes : &world_sdf.looks;
	while (*last)
	{
		last = &(*last)->next;
		waiting++;
	}
	*last = job;
	job->next = NULL;
	/* (too many bakes waiting: the oldest dropped, its field baked when its
	map is played) */
	if (bake && waiting >= WORLD_SDF_MAXIMUM_BAKES)
	{
		struct sdf_job *oldest = world_sdf.bakes;

		world_sdf.bakes = oldest->next;
		sdf_job_free(oldest);
	}
	pthread_cond_signal(bake ? &world_sdf_bake_wake : &world_sdf_look_wake);
	pthread_mutex_unlock(&world_sdf_lock);
}

/* whether a field is waiting to be baked, or baking */
static int sdf_bake_queued(struct sdf_key const *key)
{
	struct sdf_job *job;
	int queued;

	pthread_mutex_lock(&world_sdf_lock);
	queued = world_sdf.baking && sdf_key_same(&world_sdf.baking_key, key);
	for (job = world_sdf.bakes; job && !queued; job = job->next)
		queued = sdf_key_same(&job->key, key);
	pthread_mutex_unlock(&world_sdf_lock);
	return queued;
}

/* ---------- the game's thread */

/* the loaded map's BSP's geometry and planes copied, and its bake queued;
FALSE if they cannot be (no geometry: the proxies alone) */
static int sdf_bake_begin(struct sdf_key const *key)
{
	struct world_scene_triangles triangles;
	struct collision_bsp const *collision = global_collision_bsp_get();
	struct sdf_job *job;
	unsigned long long began = SDL_GetTicksNS();

	if (sdf_bake_queued(key))
		return 1;
	if (!collision || !world_scene_static_triangles(key->bsp, &triangles))
		return 0;
	if (triangles.count <= 0 || !(job = calloc(1, sizeof(*job))))
	{
		world_scene_triangles_free(&triangles);
		return 0;
	}
	job->kind = _sdf_job_bake;
	job->key = *key;
	/* (copied again: world_scene.c's copy is the game's allocator's, let
	go of here, on the game's thread) */
	job->positions = malloc((size_t)triangles.count * 9 * sizeof(float));
	if (!job->positions)
	{
		world_scene_triangles_free(&triangles);
		sdf_job_free(job);
		return 0;
	}
	memcpy(job->positions, triangles.positions, (size_t)triangles.count * 9 * sizeof(float));
	job->triangle_count = triangles.count;
	memcpy(job->bounds, triangles.bounds, sizeof(job->bounds));
	world_scene_triangles_free(&triangles);
	job->bsp.node_count = collision->bsp3d.nodes.count;
	job->bsp.plane_count = collision->bsp3d.planes.count;
	if (job->bsp.node_count > 0 && job->bsp.plane_count > 0)
	{
		long index;

		job->bsp.nodes = malloc((size_t)job->bsp.node_count * sizeof(*job->bsp.nodes));
		job->bsp.planes = malloc((size_t)job->bsp.plane_count * sizeof(*job->bsp.planes));
		if (!job->bsp.nodes || !job->bsp.planes)
		{
			sdf_job_free(job);
			return 0;
		}
		for (index = 0; index < job->bsp.node_count; index++)
		{
			struct bsp3d_node const *node = TAG_BLOCK_GET_ELEMENT(&collision->bsp3d.nodes, index, struct bsp3d_node);

			job->bsp.nodes[index][0] = node->plane_designator;
			job->bsp.nodes[index][1] = node->children[0];
			job->bsp.nodes[index][2] = node->children[1];
		}
		for (index = 0; index < job->bsp.plane_count; index++)
		{
			real_plane3d const *plane = TAG_BLOCK_GET_ELEMENT(&collision->bsp3d.planes, index, real_plane3d);

			job->bsp.planes[index][0] = plane->n.i;
			job->bsp.planes[index][1] = plane->n.j;
			job->bsp.planes[index][2] = plane->n.k;
			job->bsp.planes[index][3] = plane->d;
		}
	}
	else
	{
		job->bsp.node_count = job->bsp.plane_count = 0;
	}
	job->copy_milliseconds = (double)(SDL_GetTicksNS() - began) / 1e6;
	sdf_queue(job);
	return 1;
}

/* the field let go of (and the backend's) */
static void sdf_field_release(void)
{
	if (world_sdf.field_sent)
		halo_world_ao_field(0, 0, 0, 0, 0, NULL);
	world_sdf.field_sent = 0;
	free(world_sdf.field.voxels);
	memset(&world_sdf.field, 0, sizeof(world_sdf.field));
	world_sdf.uploaded = 0;
	world_sdf.fade = 0.0f;
	world_sdf.faded_from = 0;
}

/* the map's BSP drawn from now on: looked for in the cache (by the looker:
nothing read here) */
static void sdf_current_set(struct sdf_key const *key)
{
	struct sdf_job *job;

	sdf_field_release();
	world_sdf.current = *key;
	world_sdf.map_known = 1;
	world_sdf.copy_wanted = 0;
	world_sdf.state = _world_sdf_baking;
	job = calloc(1, sizeof(*job));
	if (!job)
	{
		world_sdf.state = _world_sdf_failed;
		return;
	}
	job->kind = _sdf_job_look;
	job->key = *key;
	sdf_queue(job);
}

/* the threads' results taken: the current BSP's field kept */
static void sdf_results(void)
{
	struct sdf_job *jobs, *job;

	pthread_mutex_lock(&world_sdf_lock);
	jobs = world_sdf.results;
	world_sdf.results = NULL;
	pthread_mutex_unlock(&world_sdf_lock);
	while ((job = jobs) != NULL)
	{
		jobs = job->next;
		if (world_sdf.map_known && sdf_key_same(&job->key, &world_sdf.current) && !world_sdf.field.voxels)
		{
			switch (job->result)
			{
			case _sdf_result_found:
			case _sdf_result_baked:
				world_sdf.field = job->volume;
				job->volume.voxels = NULL;
				world_sdf.uploaded = 0;
				world_sdf.state = _world_sdf_ready;
				break;
			case _sdf_result_missing:
				/* (baked from a copy, a few frames from now: never in the load) */
				world_sdf.copy_wanted = 1;
				world_sdf.copy_frames = WORLD_SDF_COPY_DELAY_FRAMES;
				break;
			default:
				world_sdf.state = _world_sdf_failed;
				break;
			}
		}
		sdf_job_free(job);
	}
}

void world_sdf_map_loaded(void)
{
	struct sdf_key key;

	if (sdf_off() || !sdf_key_loaded(&key))
		return;
	/* (measured: debug.load_trace's world_sdf.map_loaded) */
	halo_load_trace("world_sdf.map_loaded.begin", key.map);
	sdf_current_set(&key);
	halo_load_trace("world_sdf.map_loaded.end", key.map);
}

void world_sdf_map_unloaded(void)
{
	/* (a bake keeps going: it has copies, and its field is cached) */
	sdf_field_release();
	world_sdf.map_known = 0;
	world_sdf.copy_wanted = 0;
	world_sdf.state = _world_sdf_none;
}

void world_sdf_bsp_switched(short bsp_index)
{
	struct sdf_key key;

	if (sdf_off() || !sdf_key_loaded(&key))
		return;
	key.bsp = bsp_index < 0 ? 0 : bsp_index;
	if (world_sdf.map_known && sdf_key_same(&key, &world_sdf.current))
		return;
	sdf_current_set(&key);
}

/* the frames' pacing while a bake runs and while none does, logged with
HALO_WORLD_SDF_FRAMES (for measuring: the bake's threads are the game's
lowest, and no frame should wait for them) */
static void sdf_frames_measured(void)
{
	static int wanted = -1;
	static unsigned long long last;
	static double worst[2], total[2];
	static long count[2];
	unsigned long long now = SDL_GetTicksNS();
	int baking;

	if (wanted < 0)
		wanted = getenv("HALO_WORLD_SDF_FRAMES") != NULL;
	if (!wanted)
		return;
	pthread_mutex_lock(&world_sdf_lock);
	/* (a bake running: not one waiting for the start-up pass to end) */
	baking = world_sdf.baking;
	pthread_mutex_unlock(&world_sdf_lock);
	if (last)
	{
		double milliseconds = (double)(now - last) / 1e6;

		if (milliseconds > worst[baking])
			worst[baking] = milliseconds;
		total[baking] += milliseconds;
		count[baking]++;
	}
	last = now;
	if (count[0] + count[1] >= 600)
	{
		platform_log("world sdf: frames while baking %ld (mean %.2f ms, worst %.2f ms), while not %ld (mean %.2f ms, "
			"worst %.2f ms)", count[1], count[1] ? total[1] / count[1] : 0.0, worst[1], count[0],
			count[0] ? total[0] / count[0] : 0.0, worst[0]);
		worst[0] = worst[1] = total[0] = total[1] = 0.0;
		count[0] = count[1] = 0;
	}
}

void world_sdf_frame(void)
{
	struct sdf_key key;
	int hold;

	if (sdf_off())
		return;
	/* (no bake while the start-up pass runs, up to the main menu shown:
	they begin once it is) */
	hold = menu_pictures_preparing();
	if (hold != world_sdf.hold)
	{
		pthread_mutex_lock(&world_sdf_lock);
		world_sdf.hold = hold;
		pthread_cond_signal(&world_sdf_bake_wake);
		pthread_mutex_unlock(&world_sdf_lock);
	}
	sdf_frames_measured();
	/* ("world" chosen with a map loaded) */
	if (!world_sdf.map_known && game_in_progress() && sdf_key_loaded(&key))
		sdf_current_set(&key);
	sdf_results();
	if (world_sdf.copy_wanted && --world_sdf.copy_frames <= 0)
	{
		world_sdf.copy_wanted = 0;
		if (!sdf_bake_begin(&world_sdf.current))
		{
			world_sdf.state = _world_sdf_failed;
			platform_log("world sdf: %s bsp %d has no geometry to bake: the proxies alone", world_sdf.current.map,
				(int)world_sdf.current.bsp);
		}
	}
	/* (uploaded a few slices a frame, then faded in) */
	if (world_sdf.field.voxels)
	{
		struct world_sdf_header const *header = &world_sdf.field.header;
		unsigned long slice = header->width * header->height;

		if (world_sdf.uploaded < header->depth)
		{
			unsigned long count = slice ? WORLD_SDF_UPLOAD_BYTES / slice : 1;

			if (count < 1)
				count = 1;
			if (count > header->depth - world_sdf.uploaded)
				count = header->depth - world_sdf.uploaded;
			halo_world_ao_field(header->width, header->height, header->depth, world_sdf.uploaded, count,
				world_sdf.field.voxels + slice * world_sdf.uploaded);
			world_sdf.field_sent = 1;
			world_sdf.uploaded += count;
			if (world_sdf.uploaded >= header->depth)
				world_sdf.faded_from = SDL_GetTicksNS();
		}
		else
		{
			unsigned long long now = SDL_GetTicksNS();

			world_sdf.fade = now - world_sdf.faded_from >= WORLD_SDF_FADE_NANOSECONDS ? 1.0f :
				(float)((double)(now - world_sdf.faded_from) / (double)WORLD_SDF_FADE_NANOSECONDS);
		}
	}
}

/* ---------- the start-up pass (menu_pictures_render.c) */

static int sdf_tried(struct sdf_key const *key)
{
	long index;

	for (index = 0; index < world_sdf.tried_count; index++)
	{
		if (sdf_key_same(&world_sdf.tried[index], key))
			return 1;
	}
	return 0;
}

/* runs nobody watches (tests), and those whose first screen is not the
main menu's: no pass (menu_pictures.c's picture_quiet_run) */
static int sdf_quiet_run(void)
{
	if (config_boolean("debug.menu_pictures"))
		return 0;
	return config_boolean("debug.hidden_window") || config_real("debug.exit_after") > 0.0 ||
		config_string("debug.network_test")[0] || config_string("debug.test_input")[0] ||
		config_boolean("debug.fixed_step") || config_string("debug.menu_open")[0];
}

void world_sdf_startup_map_loaded(void)
{
	struct sdf_key key;
	char path[1100];

	if (sdf_off() || !sdf_key_loaded(&key) || sdf_tried(&key))
		return;
	if (world_sdf.tried_count < WORLD_SDF_MAXIMUM_TRIED)
		world_sdf.tried[world_sdf.tried_count++] = key;
	sdf_path(&key, path, sizeof(path));
	if (sdf_cache_present(path, key.checksum, (unsigned long)key.bsp))
		return;
	if (!sdf_bake_begin(&key))
		platform_log("world sdf: %s bsp %d has no geometry to bake", key.map, (int)key.bsp);
}

/* whether a stock map (a10) is missing its first BSP's field: the pass
loads it for its fields (menu_pictures_render.c's) */
int world_sdf_startup_map_wanted(char const *map)
{
	struct sdf_key key;
	char path[1100];

	if (sdf_off())
		return 0;
	memset(&key, 0, sizeof(key));
	snprintf(key.map, sizeof(key.map), "%s", map);
	key.checksum = menu_picture_map_checksum(map);
	if (!key.checksum || sdf_tried(&key))
		return 0;
	sdf_path(&key, path, sizeof(path));
	return !sdf_cache_present(path, key.checksum, 0);
}

int world_sdf_startup_wanted(void)
{
	unsigned int pose;

	if (world_sdf.startup_looked)
		return world_sdf.startup_wanted;
	world_sdf.startup_looked = 1;
	if (sdf_off() || sdf_quiet_run())
		return 0;
	for (pose = 0; pose < menu_picture_pose_count && !world_sdf.startup_wanted; pose++)
	{
		unsigned int before;

		/* (each map once) */
		for (before = 0; before < pose && strcmp(menu_picture_poses[before].map, menu_picture_poses[pose].map);
			before++)
		{
		}
		if (before == pose)
			world_sdf.startup_wanted = world_sdf_startup_map_wanted(menu_picture_poses[pose].map);
	}
	return world_sdf.startup_wanted;
}

/* ---------- the field drawn */

int world_sdf_state(void)
{
	return world_sdf.map_known ? world_sdf.state : _world_sdf_none;
}

float world_sdf_fade(void)
{
	return world_sdf.field.voxels ? world_sdf.fade : 0.0f;
}

struct world_sdf_header const *world_sdf_current(void)
{
	/* (the volume's first member: (struct world_sdf_volume const *) this
	is the field, its voxels too, for world_sdf_sample. The game's thread's
	alone, and good until the next world_sdf_frame, BSP switch or unload:
	the threads never touch a field once it is handed over) */
	return world_sdf.field.voxels ? &world_sdf.field.header : NULL;
}
