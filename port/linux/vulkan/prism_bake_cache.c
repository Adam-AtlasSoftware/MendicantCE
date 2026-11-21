/*
PRISM_BAKE_CACHE.C

The baker's cache file: a header and a log of records
(prism_bake_cache.h).
*/

#include "prism_bake_cache.h"
#include "prism_bake_pack.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
typedef __int64 bake_offset;
#define bake_seek _fseeki64
#define bake_tell _ftelli64
#define bake_truncate(file, size) _chsize_s(_fileno(file), (__int64)(size))
#define bake_sync(file) _commit(_fileno(file))
#else
#include <sys/types.h>
#include <unistd.h>
typedef off_t bake_offset;
#define bake_seek fseeko
#define bake_tell ftello
#define bake_truncate(file, size) ftruncate(fileno(file), (bake_offset)(size))
#define bake_sync(file) fsync(fileno(file))
#endif

static unsigned char const header_magic[8] = { 'P', 'R', 'I', 'S', 'M', 'B', 'A', 'K' };
#define RECORD_MAGIC PRISM_BAKE_CACHE_FOURCC('P', 'B', 'K', 'R')

/* ---------- hashes and checksums */

uint64_t prism_bake_hash64(uint64_t hash, void const *data, size_t size)
{
	unsigned char const *bytes = (unsigned char const *)data;
	size_t index;

	for (index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= 0x100000001B3ULL;
	}
	return hash;
}

uint32_t prism_bake_crc32(uint32_t crc, void const *data, size_t size)
{
	static uint32_t table[256];
	static int ready;
	unsigned char const *bytes = (unsigned char const *)data;
	size_t index;

	if (!ready)
	{
		uint32_t n, k, c;

		for (n = 0; n < 256; n++)
		{
			c = n;
			for (k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
		ready = 1;
	}
	crc = ~crc;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ bytes[index]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* ---------- little-endian fields */

static void put32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static void put64(unsigned char *p, uint64_t value)
{
	put32(p, (uint32_t)value);
	put32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t get32(unsigned char const *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(unsigned char const *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* ---------- the key */

/* the header's layout */
enum
{
	HEADER_VERSION = 8,
	HEADER_SIZE_FIELD = 12,
	HEADER_MAP_CHECKSUM = 16,
	HEADER_BSP_INDEX = 20,
	HEADER_MAP_NAME = 24,
	HEADER_BSP_NAME = HEADER_MAP_NAME + PRISM_BAKE_CACHE_NAME_LENGTH,
	HEADER_BSP_HASH = HEADER_BSP_NAME + PRISM_BAKE_CACHE_NAME_LENGTH,
	HEADER_GEOMETRY_HASH = HEADER_BSP_HASH + 8,
	HEADER_MATERIAL_HASH = HEADER_GEOMETRY_HASH + 8,
	HEADER_LIGHT_HASH = HEADER_MATERIAL_HASH + 8,
	HEADER_SETTINGS_HASH = HEADER_LIGHT_HASH + 8,
	HEADER_BAKER_VERSION = HEADER_SETTINGS_HASH + 8,
	HEADER_QUALITY = HEADER_BAKER_VERSION + 4,
	HEADER_THRESHOLD = HEADER_QUALITY + 4,
	HEADER_CRC = PRISM_BAKE_CACHE_HEADER_SIZE - 4
};

typedef char header_fits[HEADER_THRESHOLD + 4 <= HEADER_CRC ? 1 : -1];

static uint32_t float_bits(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	return bits;
}

static float bits_float(uint32_t bits)
{
	float value;

	memcpy(&value, &bits, 4);
	return value;
}

static void name_copy(char *to, char const *from)
{
	size_t length = 0;

	memset(to, 0, PRISM_BAKE_CACHE_NAME_LENGTH);
	while (length < PRISM_BAKE_CACHE_NAME_LENGTH - 1 && from[length])
	{
		to[length] = from[length];
		length++;
	}
}

static void header_write(unsigned char *h, struct prism_bake_cache_key const *key)
{
	char name[PRISM_BAKE_CACHE_NAME_LENGTH];

	memset(h, 0, PRISM_BAKE_CACHE_HEADER_SIZE);
	memcpy(h, header_magic, 8);
	put32(h + HEADER_VERSION, PRISM_BAKE_CACHE_VERSION);
	put32(h + HEADER_SIZE_FIELD, PRISM_BAKE_CACHE_HEADER_SIZE);
	put32(h + HEADER_MAP_CHECKSUM, key->map_checksum);
	put32(h + HEADER_BSP_INDEX, (uint32_t)key->bsp_index);
	name_copy(name, key->map_name);
	memcpy(h + HEADER_MAP_NAME, name, PRISM_BAKE_CACHE_NAME_LENGTH);
	name_copy(name, key->bsp_name);
	memcpy(h + HEADER_BSP_NAME, name, PRISM_BAKE_CACHE_NAME_LENGTH);
	put64(h + HEADER_BSP_HASH, key->bsp_hash);
	put64(h + HEADER_GEOMETRY_HASH, key->geometry_hash);
	put64(h + HEADER_MATERIAL_HASH, key->material_hash);
	put64(h + HEADER_LIGHT_HASH, key->light_hash);
	put64(h + HEADER_SETTINGS_HASH, key->settings_hash);
	put32(h + HEADER_BAKER_VERSION, key->baker_version);
	put32(h + HEADER_QUALITY, key->quality);
	put32(h + HEADER_THRESHOLD, float_bits(key->convergence_threshold));
	put32(h + HEADER_CRC, prism_bake_crc32(0, h, HEADER_CRC));
}

/* the header read into key: 0, or -1 when it is not a valid header of this
version */
static int header_read(unsigned char const *h, struct prism_bake_cache_key *key)
{
	if (memcmp(h, header_magic, 8) || get32(h + HEADER_VERSION) != PRISM_BAKE_CACHE_VERSION ||
		get32(h + HEADER_SIZE_FIELD) != PRISM_BAKE_CACHE_HEADER_SIZE ||
		get32(h + HEADER_CRC) != prism_bake_crc32(0, h, HEADER_CRC))
	{
		return -1;
	}
	memset(key, 0, sizeof(*key));
	key->map_checksum = get32(h + HEADER_MAP_CHECKSUM);
	key->bsp_index = (int32_t)get32(h + HEADER_BSP_INDEX);
	memcpy(key->map_name, h + HEADER_MAP_NAME, PRISM_BAKE_CACHE_NAME_LENGTH);
	key->map_name[PRISM_BAKE_CACHE_NAME_LENGTH - 1] = 0;
	memcpy(key->bsp_name, h + HEADER_BSP_NAME, PRISM_BAKE_CACHE_NAME_LENGTH);
	key->bsp_name[PRISM_BAKE_CACHE_NAME_LENGTH - 1] = 0;
	key->bsp_hash = get64(h + HEADER_BSP_HASH);
	key->geometry_hash = get64(h + HEADER_GEOMETRY_HASH);
	key->material_hash = get64(h + HEADER_MATERIAL_HASH);
	key->light_hash = get64(h + HEADER_LIGHT_HASH);
	key->settings_hash = get64(h + HEADER_SETTINGS_HASH);
	key->baker_version = get32(h + HEADER_BAKER_VERSION);
	key->quality = get32(h + HEADER_QUALITY);
	key->convergence_threshold = bits_float(get32(h + HEADER_THRESHOLD));
	return 0;
}

uint32_t prism_bake_key_compare(struct prism_bake_cache_key const *a, struct prism_bake_cache_key const *b)
{
	uint32_t differences = 0;

	if (a->map_checksum != b->map_checksum || strncmp(a->map_name, b->map_name, PRISM_BAKE_CACHE_NAME_LENGTH))
		differences |= PRISM_BAKE_KEY_MAP;
	if (a->bsp_index != b->bsp_index || a->bsp_hash != b->bsp_hash ||
		strncmp(a->bsp_name, b->bsp_name, PRISM_BAKE_CACHE_NAME_LENGTH))
	{
		differences |= PRISM_BAKE_KEY_BSP;
	}
	if (a->geometry_hash != b->geometry_hash)
		differences |= PRISM_BAKE_KEY_GEOMETRY;
	if (a->material_hash != b->material_hash)
		differences |= PRISM_BAKE_KEY_MATERIALS;
	if (a->light_hash != b->light_hash)
		differences |= PRISM_BAKE_KEY_LIGHTS;
	if (a->settings_hash != b->settings_hash)
		differences |= PRISM_BAKE_KEY_SETTINGS;
	if (a->baker_version != b->baker_version)
		differences |= PRISM_BAKE_KEY_BAKER;
	if (a->quality != b->quality || float_bits(a->convergence_threshold) != float_bits(b->convergence_threshold))
		differences |= PRISM_BAKE_KEY_QUALITY;
	return differences;
}

/* ---------- job ids */

#define JOB_TYPE_SHIFT 56

static uint64_t job_make(uint32_t type, uint64_t key)
{
	return ((uint64_t)type << JOB_TYPE_SHIFT) | key;
}

uint64_t prism_bake_job_lightmap_tile(uint32_t chart, uint32_t level, uint32_t x, uint32_t y)
{
	if (chart >= (1u << 24) || level >= 16 || x >= (1u << 14) || y >= (1u << 14))
		return PRISM_BAKE_JOB_NONE;
	return job_make(PRISM_BAKE_JOB_TYPE_LIGHTMAP_TILE, (uint64_t)chart << 32 | (uint64_t)level << 28 |
		(uint64_t)y << 14 | x);
}

uint64_t prism_bake_job_sun_mask_tile(uint32_t page, uint32_t level, uint32_t x, uint32_t y)
{
	if (page >= (1u << 16) || level >= 16 || x >= (1u << 18) || y >= (1u << 18))
		return PRISM_BAKE_JOB_NONE;
	return job_make(PRISM_BAKE_JOB_TYPE_SUN_MASK_TILE, (uint64_t)page << 40 | (uint64_t)level << 36 |
		(uint64_t)y << 18 | x);
}

uint64_t prism_bake_job_volume_brick(uint32_t cascade, uint32_t x, uint32_t y, uint32_t z)
{
	if (cascade >= 256 || x >= (1u << 16) || y >= (1u << 16) || z >= (1u << 16))
		return PRISM_BAKE_JOB_NONE;
	return job_make(PRISM_BAKE_JOB_TYPE_VOLUME_BRICK, (uint64_t)cascade << 48 | (uint64_t)z << 32 |
		(uint64_t)y << 16 | x);
}

uint64_t prism_bake_job_probe(uint32_t set, uint64_t index)
{
	if (set >= 256 || index >= (1ULL << 48))
		return PRISM_BAKE_JOB_NONE;
	return job_make(PRISM_BAKE_JOB_TYPE_PROBE, (uint64_t)set << 48 | index);
}

uint64_t prism_bake_job_importance_cell(uint32_t level, uint32_t x, uint32_t y, uint32_t z)
{
	if (level >= 256 || x >= (1u << 16) || y >= (1u << 16) || z >= (1u << 16))
		return PRISM_BAKE_JOB_NONE;
	return job_make(PRISM_BAKE_JOB_TYPE_IMPORTANCE_CELL, (uint64_t)level << 48 | (uint64_t)z << 32 |
		(uint64_t)y << 16 | x);
}

uint32_t prism_bake_job_type_of(uint64_t job)
{
	return (uint32_t)(job >> JOB_TYPE_SHIFT);
}

uint64_t prism_bake_job_parent(uint64_t job)
{
	uint64_t level;

	switch (prism_bake_job_type_of(job))
	{
	case PRISM_BAKE_JOB_TYPE_LIGHTMAP_TILE:
		level = (job >> 28) & 15;
		if (!level)
			return PRISM_BAKE_JOB_NONE;
		return prism_bake_job_lightmap_tile((uint32_t)((job >> 32) & 0xFFFFFF), (uint32_t)level - 1,
			(uint32_t)((job & 0x3FFF) >> 1), (uint32_t)(((job >> 14) & 0x3FFF) >> 1));
	case PRISM_BAKE_JOB_TYPE_SUN_MASK_TILE:
		level = (job >> 36) & 15;
		if (!level)
			return PRISM_BAKE_JOB_NONE;
		return prism_bake_job_sun_mask_tile((uint32_t)((job >> 40) & 0xFFFF), (uint32_t)level - 1,
			(uint32_t)((job & 0x3FFFF) >> 1), (uint32_t)(((job >> 18) & 0x3FFFF) >> 1));
	default:
		return PRISM_BAKE_JOB_NONE;
	}
}

/* ---------- records */

enum
{
	RECORD_MAGIC_FIELD = 0,
	RECORD_TYPE = 4,
	RECORD_JOB = 8,
	RECORD_SEQUENCE = 16,
	RECORD_SIZE = 20,
	RECORD_PAYLOAD_CRC = 24,
	RECORD_GENERATION = 28,
	/* (version 3: a packed payload's unpacked size, prism_bake_pack.h; 0 a
	plain payload) */
	RECORD_UNPACKED = 32,
	RECORD_CRC = PRISM_BAKE_CACHE_RECORD_HEADER_SIZE - 4
};

static uint64_t padded(uint64_t size)
{
	return (size + 7) & ~(uint64_t)7;
}

static size_t index_hash(uint32_t type, uint64_t job, size_t mask)
{
	uint64_t key = ((uint64_t)type * 0xFF51AFD7ED558CCDULL ^ job) * 0x9E3779B97F4A7C15ULL;

	return (size_t)(key >> 17) & mask;
}

static int index_insert(struct prism_bake_cache *cache, size_t record)
{
	struct prism_bake_record const *r = &cache->records[record];
	size_t mask, slot;

	if ((cache->record_count + 1) * 2 > cache->index_capacity)
	{
		size_t capacity = cache->index_capacity ? cache->index_capacity * 2 : 1024, k;
		size_t *index = (size_t *)calloc(capacity, sizeof(size_t));

		if (!index)
			return -1;
		free(cache->index);
		cache->index = index;
		cache->index_capacity = capacity;
		/* (every record before this one again) */
		for (k = 0; k < record; k++)
		{
			struct prism_bake_record const *o = &cache->records[k];

			slot = index_hash(o->type, o->job, capacity - 1);
			while (cache->index[slot] && (cache->records[cache->index[slot] - 1].type != o->type ||
				cache->records[cache->index[slot] - 1].job != o->job))
			{
				slot = (slot + 1) & (capacity - 1);
			}
			cache->index[slot] = k + 1;
		}
	}
	mask = cache->index_capacity - 1;
	slot = index_hash(r->type, r->job, mask);
	while (cache->index[slot] && (cache->records[cache->index[slot] - 1].type != r->type ||
		cache->records[cache->index[slot] - 1].job != r->job))
	{
		slot = (slot + 1) & mask;
	}
	cache->index[slot] = record + 1;
	return 0;
}

struct prism_bake_record const *prism_bake_cache_find(struct prism_bake_cache const *cache, uint32_t type,
	uint64_t job)
{
	size_t mask, slot;

	if (!cache->index_capacity)
		return NULL;
	mask = cache->index_capacity - 1;
	for (slot = index_hash(type, job, mask); cache->index[slot]; slot = (slot + 1) & mask)
	{
		struct prism_bake_record const *r = &cache->records[cache->index[slot] - 1];

		if (r->type == type && r->job == job)
			return r;
	}
	return NULL;
}

/* the finished result of a job, whatever the plan says */
static struct prism_bake_record const *result_find(struct prism_bake_cache const *cache, uint32_t type,
	uint64_t job)
{
	struct prism_bake_record const *result = prism_bake_cache_find(cache, type, job);
	struct prism_bake_record const *checkpoint = prism_bake_cache_find(cache, PRISM_BAKE_RECORD_CHECKPOINT, job);

	return result && (!checkpoint || checkpoint->sequence < result->sequence) ? result : NULL;
}

int prism_bake_cache_in_plan(struct prism_bake_cache const *cache, uint64_t job)
{
	size_t low = 0, high = cache->plan_count;

	while (low < high)
	{
		size_t middle = low + (high - low) / 2;

		if (cache->plan_jobs[middle] == job)
			return 1;
		if (cache->plan_jobs[middle] < job)
			low = middle + 1;
		else
			high = middle;
	}
	return 0;
}

int prism_bake_cache_job_done(struct prism_bake_cache const *cache, uint32_t type, uint64_t job)
{
	return prism_bake_cache_in_plan(cache, job) && result_find(cache, type, job) != NULL;
}

struct prism_bake_record const *prism_bake_cache_fallback(struct prism_bake_cache const *cache, uint32_t type,
	uint64_t job)
{
	if (prism_bake_cache_job_done(cache, type, job))
		return NULL;
	for (job = prism_bake_job_parent(job); job != PRISM_BAKE_JOB_NONE; job = prism_bake_job_parent(job))
	{
		struct prism_bake_record const *result = result_find(cache, type, job);

		if (result)
			return result;
	}
	return NULL;
}

static int payload_check(FILE *file, uint64_t offset, uint32_t size, uint32_t expected, void *buffer)
{
	unsigned char chunk[65536];
	uint32_t crc = 0, left = size;
	unsigned char *out = (unsigned char *)buffer;

	if (bake_seek(file, (bake_offset)offset, SEEK_SET))
		return -1;
	while (left)
	{
		size_t n = left < sizeof(chunk) ? left : sizeof(chunk);

		if (fread(out ? (void *)out : (void *)chunk, 1, n, file) != n)
			return -1;
		crc = prism_bake_crc32(crc, out ? (void *)out : (void *)chunk, n);
		if (out)
			out += n;
		left -= (uint32_t)n;
	}
	return crc == expected ? 0 : -1;
}

static int job_compare(void const *a, void const *b)
{
	uint64_t x = *(uint64_t const *)a, y = *(uint64_t const *)b;

	return x < y ? -1 : x > y;
}

/* the newest plan's job ids read, checked and sorted: 0, or -1 */
static int plan_read(struct prism_bake_cache *cache, struct prism_bake_record const *record)
{
	unsigned char *bytes;
	size_t count = record->size / 8, k;

	cache->plan_jobs = (uint64_t *)malloc((count ? count : 1) * sizeof(uint64_t));
	bytes = (unsigned char *)malloc(record->size ? record->size : 1);
	if (!cache->plan_jobs || !bytes || payload_check(cache->file, record->offset, record->size, record->payload_crc, bytes))
	{
		free(bytes);
		return -1;
	}
	for (k = 0; k < count; k++)
		cache->plan_jobs[k] = get64(bytes + k * 8);
	free(bytes);
	qsort(cache->plan_jobs, count, sizeof(uint64_t), job_compare);
	cache->plan_count = count;
	return 0;
}

/* the log read from after the header: every record whose header checks
out (and payload, verify_payloads), up to the first that does not */
static int log_read(struct prism_bake_cache *cache, int verify_payloads)
{
	uint64_t offset = PRISM_BAKE_CACHE_HEADER_SIZE;
	size_t plan = 0;

	cache->valid_end = offset;
	while (offset + PRISM_BAKE_CACHE_RECORD_HEADER_SIZE <= cache->file_size)
	{
		unsigned char h[PRISM_BAKE_CACHE_RECORD_HEADER_SIZE];
		struct prism_bake_record record;

		if (bake_seek(cache->file, (bake_offset)offset, SEEK_SET) || fread(h, sizeof(h), 1, cache->file) != 1 ||
			get32(h + RECORD_MAGIC_FIELD) != RECORD_MAGIC || get32(h + RECORD_CRC) != prism_bake_crc32(0, h, RECORD_CRC))
		{
			break;
		}
		record.type = get32(h + RECORD_TYPE);
		record.job = get64(h + RECORD_JOB);
		record.generation = get32(h + RECORD_GENERATION);
		record.sequence = get32(h + RECORD_SEQUENCE);
		record.stored = get32(h + RECORD_SIZE);
		record.size = get32(h + RECORD_UNPACKED) ? get32(h + RECORD_UNPACKED) : record.stored;
		record.payload_crc = get32(h + RECORD_PAYLOAD_CRC);
		record.offset = offset + PRISM_BAKE_CACHE_RECORD_HEADER_SIZE;
		if (record.size > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD || record.stored > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD ||
			record.offset + padded(record.stored) > cache->file_size ||
			(record.stored != record.size && record.type == PRISM_BAKE_RECORD_PLAN) ||
			record.sequence != cache->next_sequence ||
			(record.type == PRISM_BAKE_RECORD_PLAN && (record.generation != cache->plan_generation + 1 ||
			record.size % 8)))
		{
			break;
		}
		if (verify_payloads && payload_check(cache->file, record.offset, record.stored, record.payload_crc, NULL))
			break;
		if (cache->record_count == cache->record_capacity)
		{
			size_t capacity = cache->record_capacity ? cache->record_capacity * 2 : 256;
			struct prism_bake_record *records = (struct prism_bake_record *)realloc(cache->records,
				capacity * sizeof(*records));

			if (!records)
				return -1;
			cache->records = records;
			cache->record_capacity = capacity;
		}
		cache->records[cache->record_count] = record;
		if (index_insert(cache, cache->record_count))
			return -1;
		cache->record_count++;
		cache->next_sequence++;
		if (record.type == PRISM_BAKE_RECORD_DONE)
			cache->done = 1;
		if (record.type == PRISM_BAKE_RECORD_PLAN)
		{
			cache->plan_generation = record.generation;
			plan = cache->record_count - 1;
		}
		offset = record.offset + padded(record.stored);
		cache->valid_end = offset;
	}
	cache->torn = cache->valid_end != cache->file_size;
	if (cache->plan_generation)
		return plan_read(cache, &cache->records[plan]);
	return 0;
}

enum prism_bake_cache_status prism_bake_cache_open(struct prism_bake_cache *cache, char const *path,
	struct prism_bake_cache_key const *key, int verify_payloads)
{
	unsigned char h[PRISM_BAKE_CACHE_HEADER_SIZE];
	int64_t size;

	memset(cache, 0, sizeof(*cache));
	cache->file = fopen(path, "rb");
	if (!cache->file)
		return cache->status = PRISM_BAKE_CACHE_MISSING;
	if (bake_seek(cache->file, 0, SEEK_END) || (size = (int64_t)bake_tell(cache->file)) < PRISM_BAKE_CACHE_HEADER_SIZE ||
		bake_seek(cache->file, 0, SEEK_SET) || fread(h, sizeof(h), 1, cache->file) != 1 ||
		header_read(h, &cache->key))
	{
		return cache->status = PRISM_BAKE_CACHE_INVALID;
	}
	cache->file_size = (uint64_t)size;
	if (log_read(cache, verify_payloads))
		return cache->status = PRISM_BAKE_CACHE_INVALID;
	cache->differences = prism_bake_key_compare(&cache->key, key);
	return cache->status = cache->differences ? PRISM_BAKE_CACHE_STALE : PRISM_BAKE_CACHE_CURRENT;
}

void prism_bake_cache_close(struct prism_bake_cache *cache)
{
	if (cache->file)
		fclose(cache->file);
	free(cache->records);
	free(cache->index);
	free(cache->plan_jobs);
	memset(cache, 0, sizeof(*cache));
}

int prism_bake_cache_read(struct prism_bake_cache const *cache, struct prism_bake_record const *record, void *buffer,
	size_t size)
{
	void *packed;
	int result;

	if (!cache->file || !record || size < record->size)
		return -1;
	if (record->stored == record->size)
		return payload_check(cache->file, record->offset, record->size, record->payload_crc, buffer);
	/* (a packed payload: checked as stored, then unpacked) */
	packed = malloc(record->stored ? record->stored : 1);
	if (!packed)
		return -1;
	result = payload_check(cache->file, record->offset, record->stored, record->payload_crc, packed);
	if (!result && prism_bake_unpack(packed, record->stored, buffer, size) != record->size)
		result = -1;
	free(packed);
	return result;
}

/* ---------- writing */

int prism_bake_cache_create(struct prism_bake_cache_writer *writer, char const *path,
	struct prism_bake_cache_key const *key, int sync)
{
	unsigned char h[PRISM_BAKE_CACHE_HEADER_SIZE];

	memset(writer, 0, sizeof(*writer));
	writer->file = fopen(path, "wb");
	if (!writer->file)
		return -1;
	writer->sync = sync;
	header_write(h, key);
	if (fwrite(h, sizeof(h), 1, writer->file) != 1 || fflush(writer->file))
	{
		fclose(writer->file);
		writer->file = NULL;
		return -1;
	}
	if (sync)
		bake_sync(writer->file);
	writer->end = PRISM_BAKE_CACHE_HEADER_SIZE;
	return 0;
}

int prism_bake_cache_resume(struct prism_bake_cache_writer *writer, struct prism_bake_cache *cache, char const *path,
	struct prism_bake_cache_key const *key, int sync)
{
	memset(writer, 0, sizeof(*writer));
	if (prism_bake_cache_open(cache, path, key, 1) != PRISM_BAKE_CACHE_CURRENT)
		return -1;
	writer->file = fopen(path, "r+b");
	if (!writer->file)
		return -1;
	if (cache->torn && bake_truncate(writer->file, cache->valid_end))
	{
		fclose(writer->file);
		writer->file = NULL;
		return -1;
	}
	cache->file_size = cache->valid_end;
	cache->torn = 0;
	if (bake_seek(writer->file, (bake_offset)cache->valid_end, SEEK_SET))
	{
		fclose(writer->file);
		writer->file = NULL;
		return -1;
	}
	writer->end = cache->valid_end;
	writer->sequence = cache->next_sequence;
	writer->generation = cache->plan_generation;
	writer->sync = sync;
	return 0;
}

static int record_append(struct prism_bake_cache_writer *writer, uint32_t type, uint64_t job, uint32_t generation,
	void const *payload, size_t size, size_t unpacked)
{
	unsigned char h[PRISM_BAKE_CACHE_RECORD_HEADER_SIZE];
	static unsigned char const zeros[8] = { 0 };
	uint64_t pad = padded(size) - size;

	if (!writer->file || size > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD || (size && !payload))
		return -1;
	memset(h, 0, sizeof(h));
	put32(h + RECORD_MAGIC_FIELD, RECORD_MAGIC);
	put32(h + RECORD_TYPE, type);
	put64(h + RECORD_JOB, job);
	put32(h + RECORD_GENERATION, generation);
	put32(h + RECORD_SEQUENCE, writer->sequence);
	put32(h + RECORD_SIZE, (uint32_t)size);
	put32(h + RECORD_UNPACKED, (uint32_t)unpacked);
	put32(h + RECORD_PAYLOAD_CRC, prism_bake_crc32(0, payload, size));
	put32(h + RECORD_CRC, prism_bake_crc32(0, h, RECORD_CRC));
	if (bake_seek(writer->file, (bake_offset)writer->end, SEEK_SET) || fwrite(h, sizeof(h), 1, writer->file) != 1 ||
		(size && fwrite(payload, size, 1, writer->file) != 1) || (pad && fwrite(zeros, (size_t)pad, 1, writer->file) != 1))
	{
		return -1;
	}
	if (writer->sync)
	{
		if (fflush(writer->file))
			return -1;
		bake_sync(writer->file);
	}
	writer->end += sizeof(h) + size + pad;
	writer->sequence++;
	return 0;
}

int prism_bake_cache_append(struct prism_bake_cache_writer *writer, uint32_t type, uint64_t job, void const *payload,
	size_t size)
{
	/* (a plan has a generation of its own: prism_bake_cache_append_plan) */
	if (type == PRISM_BAKE_RECORD_PLAN)
		return -1;
	return record_append(writer, type, job, writer->generation, payload, size, 0);
}

int prism_bake_cache_append_packed(struct prism_bake_cache_writer *writer, uint32_t type, uint64_t job,
	void const *payload, size_t size, uint32_t stride)
{
	size_t bound = prism_bake_pack_bound(size), packed_size;
	unsigned char *packed;
	int result;

	if (type == PRISM_BAKE_RECORD_PLAN || size < 64 || size > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD)
		return prism_bake_cache_append(writer, type, job, payload, size);
	packed = (unsigned char *)malloc(bound);
	if (!packed)
		return prism_bake_cache_append(writer, type, job, payload, size);
	packed_size = prism_bake_pack(payload, size, stride, packed, bound);
	/* (one that does not pack is kept plain) */
	if (!packed_size || packed_size >= size)
		result = prism_bake_cache_append(writer, type, job, payload, size);
	else
		result = record_append(writer, type, job, writer->generation, packed, packed_size, size);
	free(packed);
	return result;
}

int prism_bake_cache_append_plan(struct prism_bake_cache_writer *writer, uint64_t const *jobs, size_t count)
{
	unsigned char *bytes;
	size_t k;
	int result;

	if (count > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD / 8 || (count && !jobs))
		return -1;
	bytes = (unsigned char *)malloc(count ? count * 8 : 1);
	if (!bytes)
		return -1;
	for (k = 0; k < count; k++)
		put64(bytes + k * 8, jobs[k]);
	result = record_append(writer, PRISM_BAKE_RECORD_PLAN, 0, writer->generation + 1, bytes, count * 8, 0);
	free(bytes);
	if (!result)
		writer->generation++;
	return result;
}

int prism_bake_cache_writer_close(struct prism_bake_cache_writer *writer)
{
	int result = 0;

	if (writer->file)
	{
		result = fflush(writer->file) ? -1 : 0;
		if (writer->sync)
			bake_sync(writer->file);
		if (fclose(writer->file))
			result = -1;
	}
	writer->file = NULL;
	return result;
}

int prism_bake_cache_commit(char const *new_path, char const *path)
{
#ifdef _WIN32
	remove(path);
#endif
	return rename(new_path, path) ? -1 : 0;
}
