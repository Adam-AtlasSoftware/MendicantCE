/*
PRISM_BAKE_CACHE.H

The baker's cache file (PRISM v4's design, section 3.7): one per map and
structure bsp, <data>/prism_bake/<map>/<bsp>.pbk, local only, never
shipped.

The file is a log: a fixed header, then records appended as the bake
finishes its jobs. Nothing written is ever rewritten, so a crash, a quit
or a GPU reset loses at most the record being written, and the bake
resumes at the next unfinished job.

	header (PRISM_BAKE_CACHE_HEADER_SIZE bytes)
		magic "PRISMBAK", the format's version, the header's size
		the key: what the bake was made from (the map's checksum and name,
		the bsp's index and name, the hashes of the bsp, the geometry (stock
		or the .pwg file), the material table, the light table, the PRISM
		settings that change light, the baker's version, the quality and
		the convergence threshold)
		the header's checksum
	records, each:
		magic "PBKR", its type, its job (the 64-bit job id below), its
		sequence number (0, 1, 2... in the file's order), its payload's
		size, the payload's checksum, its generation (a plan's own; a
		result's, the newest plan's when it was written), the payload's
		unpacked size where it is packed (prism_bake_pack.h; 0 a plain
		one: version 3), the record header's checksum
		the payload, padded to 8 bytes

Job ids. A job's id is a 64-bit value that names the job for as long as
the bake's plan names it, however the plan is refined:

	bits 63..56	the job type (prism_bake_job_type; 0 is no job)
	bits 55..0	the type's stable key:
	lightmap tile	chart (24: bits 55..32), level (4: 31..28), tile y
			(14: 27..14), tile x (14: 13..0)
	sun mask tile	page (16: 55..40), level (4: 39..36), tile y (18: 35..18),
			tile x (18: 17..0)
	volume brick	cascade (8: 55..48), z (16: 47..32), y (16: 31..16),
			x (16: 15..0)
	probe		set (8: 55..48), index (48: 47..0)
	importance cell	level (8: 55..48), z, y, x (16 each, as a brick's)

A lightmap or sun mask tile's level is the chart's (or page's) refinement
level: its scale is the base scale doubled that many times. Its tile x and
y count tiles from the box's corner (a chart's box with its gutter, floored
to a CE texel; a page's own corner) in the tiles of that level, whose side
is half the CE texels of the level before: tile (x, y) of level L is
divided into tiles (2x, 2y), (2x + 1, 2y), (2x, 2y + 1), (2x + 1, 2y + 1) of
level L + 1, so a refined tile's id names the tile it came from
(prism_bake_job_parent). The keys do not depend on the plan's order or on
what else is planned, so an id means the same job in every plan, and ids of
different types never meet (the type is in the id). A key out of its
field's range has no id (prism_bake_job_* give PRISM_BAKE_JOB_NONE), never
a wrapped one.

Records:
- PLAN: the bake's job list: the ids of every job the bake now needs, as
  64-bit little-endian values (the first plan is generation 1; a later PLAN,
  as when the atlas is refined, is the next generation and replaces the
  one before it);
- a job's result (atlas tile, sun mask tile, volume brick, probe, the
  importance grid, the reachable set, footprints): the job's final data;
  a result is valid only while its job id is in the newest PLAN: a job the
  newest plan no longer names is superseded, and is not done, though its
  record stays in the file (a new plan keeps the finished jobs it still
  names: refinement leaves the other charts' results as they were). A superseded
  tile's result stays readable as a fallback (prism_bake_cache_fallback)
  for the tiles refined from it, each until its own result is in: a refined
  tile is drawn from its parent's (or grandparent's) texels while it is
  unfinished;
- ATLAS_AUX and ATLAS_FILTERED, the filter's (prism_bake_filter.h), of a
  lightmap tile's job: its texels' normals, charts and variances beside its
  result, then its texels filtered (its result's layout). The unfinished log
  keeps the result as baked, so the filter can run again from it; the
  finished file has the filtered texels as the tile's result and neither of
  these;
- CHECKPOINT: a job's state part-way (its accumulators and convergence
  state, so a long job resumes where it stopped); a later record of the
  same job replaces it;
- DONE: the bake is complete.

Reading validates everything: the header's checksum and every size; a
record whose header or payload does not check out ends the log there
(it is the torn write of a crash), and the writer resuming cuts the
file there before it appends. The key compared with the game's gives
the bake's freshness: a stale bake is still used, flagged, until the
rebake replaces it (written beside it as <bsp>.pbk.new and renamed over
it when DONE).

Host code with no engine dependency, written as C89; the file is little
endian.
*/

#ifndef PRISM_BAKE_CACHE_H
#define PRISM_BAKE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PRISM_BAKE_CACHE_VERSION = 3, /* (1: 32-bit job indices, no plan generations: not read; 2: no packed records, read) */
	PRISM_BAKE_CACHE_HEADER_SIZE = 256,
	PRISM_BAKE_CACHE_RECORD_HEADER_SIZE = 40,
	/* a record's payload at most (a 1024 x 1024 tile of 64-byte texels) */
	PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD = 64 << 20,
	PRISM_BAKE_CACHE_NAME_LENGTH = 64
};

#define PRISM_BAKE_CACHE_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum prism_bake_record_type
{
	PRISM_BAKE_RECORD_PLAN = PRISM_BAKE_CACHE_FOURCC('P', 'L', 'A', 'N'),
	PRISM_BAKE_RECORD_CHECKPOINT = PRISM_BAKE_CACHE_FOURCC('C', 'K', 'P', 'T'),
	PRISM_BAKE_RECORD_DONE = PRISM_BAKE_CACHE_FOURCC('D', 'O', 'N', 'E'),
	PRISM_BAKE_RECORD_ATLAS_TILE = PRISM_BAKE_CACHE_FOURCC('A', 'T', 'L', 'S'),
	PRISM_BAKE_RECORD_ATLAS_AUX = PRISM_BAKE_CACHE_FOURCC('A', 'T', 'L', 'X'),
	PRISM_BAKE_RECORD_ATLAS_FILTERED = PRISM_BAKE_CACHE_FOURCC('A', 'T', 'L', 'F'),
	PRISM_BAKE_RECORD_SUN_MASK_TILE = PRISM_BAKE_CACHE_FOURCC('M', 'A', 'S', 'K'),
	PRISM_BAKE_RECORD_VOLUME_BRICK = PRISM_BAKE_CACHE_FOURCC('V', 'O', 'L', 'B'),
	PRISM_BAKE_RECORD_PROBE = PRISM_BAKE_CACHE_FOURCC('P', 'R', 'O', 'B'),
	PRISM_BAKE_RECORD_IMPORTANCE_GRID = PRISM_BAKE_CACHE_FOURCC('I', 'M', 'P', 'G'),
	PRISM_BAKE_RECORD_REACHABLE = PRISM_BAKE_CACHE_FOURCC('R', 'E', 'A', 'C'),
	PRISM_BAKE_RECORD_FOOTPRINTS = PRISM_BAKE_CACHE_FOURCC('F', 'O', 'O', 'T'),
	/* the layered bake (docs/prism_bake.md, "The layers"): the layers'
	base (the light the bake was made with: what the weights at load are
	relative to; job none), a lightmap tile's layers (its texels' shares of
	the light by kind, beside its ATLS), a volume brick's layers (beside
	its VOLB) */
	PRISM_BAKE_RECORD_LAYER_BASE = PRISM_BAKE_CACHE_FOURCC('L', 'Y', 'R', 'B'),
	PRISM_BAKE_RECORD_ATLAS_LAYERS = PRISM_BAKE_CACHE_FOURCC('A', 'T', 'L', 'Y'),
	PRISM_BAKE_RECORD_VOLUME_LAYERS = PRISM_BAKE_CACHE_FOURCC('V', 'O', 'L', 'Y'),
	/* the foliage-ready layers (v4 geopatch M4, PLAN_FINAL.md section 4.4: the
	light with the static scenery's cutouts, the leaves' cards, passed, for
	foliage drawn by the runtime): a tile's sun mask (MASK_TEXELS bytes,
	the tile's mask job), a brick's probes' sun visibility (a byte each, the
	brick's job), and the foliage classes (FCLS, no job: the format in
	xgpu_vulkan_bake.c's foliage_classes_write) */
	PRISM_BAKE_RECORD_SUN_MASK_FOLIAGE = PRISM_BAKE_CACHE_FOURCC('M', 'S', 'K', 'F'),
	PRISM_BAKE_RECORD_VOLUME_FOLIAGE = PRISM_BAKE_CACHE_FOURCC('V', 'O', 'L', 'F'),
	PRISM_BAKE_RECORD_FOLIAGE_CLASSES = PRISM_BAKE_CACHE_FOURCC('F', 'C', 'L', 'S')
};

/* what a bake was made from: a change to any field makes it stale */
struct prism_bake_cache_key
{
	uint32_t map_checksum; /* the cache file's tag header checksum */
	char map_name[PRISM_BAKE_CACHE_NAME_LENGTH];
	int32_t bsp_index;
	char bsp_name[PRISM_BAKE_CACHE_NAME_LENGTH];
	uint64_t bsp_hash; /* the bsp's bytes as loaded */
	uint64_t geometry_hash; /* 0 for the stock world, else the .pwg file's */
	uint64_t material_hash; /* the material table the bake read */
	uint64_t light_hash; /* the light table (and the per-map light overrides) */
	uint64_t settings_hash; /* the PRISM settings that change light (sun, sky, exposure of emitters...) */
	uint32_t baker_version;
	uint32_t quality; /* prism_bake_atlas.h's prism_bake_quality */
	float convergence_threshold; /* prism_bake_converge.h's, set in M0 */
	/* (v4 geopatch M4) the world the bake was made for: the BSP's world_id
	(port/linux/game/prism_identity.h; all zero where the game had none: a
	custom map, a developer's run without the farm's classic_world_id),
	SCENE_VERSION and the variant (0 classic, 1 enhanced). With a world_id
	the map's checksum is 0: the world_id names the bake's inputs, so two
	map versions of equal inputs share a pack */
	unsigned char world_id[32];
	uint32_t scene_version;
	uint32_t variant;
};

/* which fields of two keys differ */
enum
{
	PRISM_BAKE_KEY_MAP = 1 << 0,
	PRISM_BAKE_KEY_BSP = 1 << 1,
	PRISM_BAKE_KEY_GEOMETRY = 1 << 2,
	PRISM_BAKE_KEY_MATERIALS = 1 << 3,
	PRISM_BAKE_KEY_LIGHTS = 1 << 4,
	PRISM_BAKE_KEY_SETTINGS = 1 << 5,
	PRISM_BAKE_KEY_BAKER = 1 << 6,
	PRISM_BAKE_KEY_QUALITY = 1 << 7
};

uint32_t prism_bake_key_compare(struct prism_bake_cache_key const *a, struct prism_bake_cache_key const *b);

/* FNV-1a, 64 bits: for the key's hashes */
uint64_t prism_bake_hash64(uint64_t hash, void const *data, size_t size);
#define PRISM_BAKE_HASH64_SEED 0xCBF29CE484222325ULL
/* CRC-32 (IEEE): the file's checksums */
uint32_t prism_bake_crc32(uint32_t crc, void const *data, size_t size);

/* ---------- job ids */

#define PRISM_BAKE_JOB_NONE 0ULL

enum prism_bake_job_type
{
	PRISM_BAKE_JOB_TYPE_NONE = 0,
	PRISM_BAKE_JOB_TYPE_LIGHTMAP_TILE,
	PRISM_BAKE_JOB_TYPE_SUN_MASK_TILE,
	PRISM_BAKE_JOB_TYPE_VOLUME_BRICK,
	PRISM_BAKE_JOB_TYPE_PROBE,
	PRISM_BAKE_JOB_TYPE_IMPORTANCE_CELL,
	NUMBER_OF_PRISM_BAKE_JOB_TYPES
};

/* each: the id, or PRISM_BAKE_JOB_NONE when a field does not fit its bits */
uint64_t prism_bake_job_lightmap_tile(uint32_t chart, uint32_t level, uint32_t x, uint32_t y);
uint64_t prism_bake_job_sun_mask_tile(uint32_t page, uint32_t level, uint32_t x, uint32_t y);
uint64_t prism_bake_job_volume_brick(uint32_t cascade, uint32_t x, uint32_t y, uint32_t z);
uint64_t prism_bake_job_probe(uint32_t set, uint64_t index);
uint64_t prism_bake_job_importance_cell(uint32_t level, uint32_t x, uint32_t y, uint32_t z);

/* a job's type (prism_bake_job_type) */
uint32_t prism_bake_job_type_of(uint64_t job);
/* the tile a lightmap or sun mask tile was refined from (a level up, x and y
halved), or PRISM_BAKE_JOB_NONE for a tile of level 0 or another type */
uint64_t prism_bake_job_parent(uint64_t job);

/* ---------- reading */

enum prism_bake_cache_status
{
	PRISM_BAKE_CACHE_MISSING = 0, /* no file */
	PRISM_BAKE_CACHE_INVALID, /* not a bake file, another version, or a bad header */
	PRISM_BAKE_CACHE_STALE, /* valid, made from something else (the key's differences say what) */
	PRISM_BAKE_CACHE_CURRENT /* valid, made from this */
};

struct prism_bake_record
{
	uint32_t type;
	uint64_t job;
	uint32_t generation;
	uint32_t sequence;
	uint64_t offset; /* the payload's, in the file */
	uint32_t size; /* the payload's (unpacked: what a read gives) */
	uint32_t stored; /* its bytes in the file (less than size where it is packed) */
	uint32_t payload_crc; /* (of the bytes stored) */
};

struct prism_bake_cache
{
	FILE *file;
	struct prism_bake_cache_key key; /* the file's */
	uint32_t differences; /* from the key it was opened with */
	enum prism_bake_cache_status status;
	/* the records whose header checks out, in order */
	struct prism_bake_record *records;
	size_t record_count, record_capacity;
	uint64_t valid_end; /* the end of the last whole record: where appending goes */
	uint64_t file_size;
	int torn; /* the log ended in a record that does not check out */
	int done; /* a DONE record was read */
	uint32_t next_sequence;
	/* the newest plan: its generation (0: no plan yet) and its job ids, sorted */
	uint32_t plan_generation;
	uint64_t *plan_jobs;
	size_t plan_count;
	/* (the last record of each type and job: an open-addressed table of
	record indices plus one) */
	size_t *index;
	size_t index_capacity;
};

/* opens a bake file and reads its log (checking every record's header and,
with verify_payloads, its payload): the status (also in cache->status) */
enum prism_bake_cache_status prism_bake_cache_open(struct prism_bake_cache *cache, char const *path,
	struct prism_bake_cache_key const *key, int verify_payloads);
void prism_bake_cache_close(struct prism_bake_cache *cache);

/* the last record of a type and job (the one that counts), or NULL */
struct prism_bake_record const *prism_bake_cache_find(struct prism_bake_cache const *cache, uint32_t type,
	uint64_t job);
/* whether the newest plan names the job */
int prism_bake_cache_in_plan(struct prism_bake_cache const *cache, uint64_t job);
/* whether the job is finished: the newest plan names it, and a record of its
result type came after any of its checkpoints (a superseded job is not) */
int prism_bake_cache_job_done(struct prism_bake_cache const *cache, uint32_t type, uint64_t job);
/* the result to draw an unfinished tile from: when the job is not done, the
record of its nearest ancestor (prism_bake_job_parent) of the result type
that has a finished result, whether or not the newest plan still names it;
NULL when the job is done (its own result counts) or no ancestor has one */
struct prism_bake_record const *prism_bake_cache_fallback(struct prism_bake_cache const *cache, uint32_t type,
	uint64_t job);
/* a record's payload, checked against its checksum: 0, or -1 */
int prism_bake_cache_read(struct prism_bake_cache const *cache, struct prism_bake_record const *record, void *buffer,
	size_t size);

/* ---------- writing */

struct prism_bake_cache_writer
{
	FILE *file;
	uint64_t end;
	uint32_t sequence;
	uint32_t generation; /* the newest plan's */
	int sync; /* flush (and fsync) after each record */
};

/* a new bake file with the key (an existing one is replaced): 0, or -1 */
int prism_bake_cache_create(struct prism_bake_cache_writer *writer, char const *path,
	struct prism_bake_cache_key const *key, int sync);
/* the bake file reopened to go on with it: its log read (cache, which
stays open for reading the finished jobs), the torn tail cut, appending
after the last whole record. The key must be the file's (else -1: a stale
bake is rebaked into a new file, not resumed) */
int prism_bake_cache_resume(struct prism_bake_cache_writer *writer, struct prism_bake_cache *cache, char const *path,
	struct prism_bake_cache_key const *key, int sync);
/* a record appended packed (prism_bake_pack.h: its payload words of stride
bytes; one that does not pack is appended plain): 0, or -1. A read
unpacks it: its record's size is the payload's, its stored the bytes
written */
int prism_bake_cache_append_packed(struct prism_bake_cache_writer *writer, uint32_t type, uint64_t job,
	void const *payload, size_t size, uint32_t stride);
/* a record appended: 0, or -1 */
int prism_bake_cache_append(struct prism_bake_cache_writer *writer, uint32_t type, uint64_t job, void const *payload,
	size_t size);
/* the next plan (generation 1, then each one more): the ids of the jobs it
names; a plan is written whole, and holds at most the payload limit's worth
of ids: 0, or -1 */
int prism_bake_cache_append_plan(struct prism_bake_cache_writer *writer, uint64_t const *jobs, size_t count);
int prism_bake_cache_writer_close(struct prism_bake_cache_writer *writer);

/* the finished new bake (path.new) renamed over the old (path): 0, or -1 */
int prism_bake_cache_commit(char const *new_path, char const *path);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_CACHE_H */
