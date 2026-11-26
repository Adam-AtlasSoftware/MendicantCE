/*
PRISM_PRSX.H

The enhanced world's sidecar (maps_enhanced/<map>.<id8>.prsx): our own data
for one retail map, read beside the player's unchanged map, never a copy of
it (v4_geopatch PLAN_FINAL.md sections 2.3 and 2.5). The farm writes it
(tools/geopatch/encode.py); this reader is the game's, and the farm's tools
load the same C (tools/geopatch/native.py), so both read one format.

The file, little endian:

  header, PRSX_HEADER_SIZE bytes:
    "PRSX", u32 format, u8 classic_world_id[32], u32 section_count,
    u32 flags (0)
  directory, section_count entries of PRSX_ENTRY_SIZE bytes:
    fourcc type, u16 bsp (PRSX_BSP_ALL: every BSP), u16 flags,
    u32 collv (0: not collision), u64 offset, u64 csize, u64 usize,
    u8 sha256_decoded[32]
  sections, each csize bytes at its offset

An entry's flags: PRSX_REQUIRED (a reader that does not know the type must
refuse the file), PRSX_BAKE_RELEVANT (the section enters the BSP's enhanced
world_id), the optional static set it belongs to (bits 4-7; 0 none) and its
codec (bits 12-15: stored, zlib). sha256_decoded is the SHA-256 of the
section's bytes as its codec gives them back; a section is used only once
that matches (per-section verification at load).

The file carries no release label, manifest data, patch-level hash or file
SHA: those live in the signed manifest only, so nothing in a key is
circular (D-B1). Nothing here is a copy of retail data: a section refers to
a retail value by a REF op that the engine resolves from the tags already in
memory (prism_ref_decode.h).

Reading goes through an abstract source (a read callback over a byte range),
so the same code reads a sidecar file, a buffer, or a container appended to
a whole map file (prsx_source_appended), should the decision on full map
files (A1) ever go the other way.

Every count, offset and size is checked against the source before use: the
file is untrusted input.
*/

#ifndef PRISM_PRSX_H
#define PRISM_PRSX_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

enum
{
	PRSX_HEADER_SIZE = 48,
	PRSX_ENTRY_SIZE = 68,
	/* the format this build writes and reads, and the oldest it still
	reads (a binary reads its own and the previous: section 3.7) */
	PRSX_FORMAT = 1,
	PRSX_FORMAT_OLDEST = 1,
	PRSX_MAXIMUM_SECTIONS = 4096,
	/* the most one section may inflate to (a BSP's GEOM is tens of MB) */
	PRSX_MAXIMUM_SECTION_BYTES = 1024 * 1024 * 1024,
	PRSX_BSP_ALL = 0xFFFF,
	PRSX_ID_SIZE = 32,
	/* the appended form's trailer: "PRSXTAIL", u64 container offset,
	u64 container size */
	PRSX_TRAILER_SIZE = 24,
};

/* an entry's flags */
enum
{
	PRSX_REQUIRED = 0x0001,
	PRSX_BAKE_RELEVANT = 0x0002,
	PRSX_SET_SHIFT = 4,
	PRSX_SET_MASK = 0x00F0,
	PRSX_CODEC_SHIFT = 12,
	PRSX_CODEC_MASK = 0xF000,
	PRSX_FLAGS_KNOWN = PRSX_REQUIRED | PRSX_BAKE_RELEVANT | PRSX_SET_MASK | PRSX_CODEC_MASK,
};

enum prsx_codec
{
	PRSX_CODEC_STORED = 0,
	PRSX_CODEC_ZLIB = 1,
	/* zstd: reserved; refused until the decoder is vendored */
	PRSX_CODEC_ZSTD = 2,
};

#define PRSX_FOURCC(a, b, c, d) \
	((uint32_t)(unsigned char)(a) | (uint32_t)(unsigned char)(b) << 8 | \
	 (uint32_t)(unsigned char)(c) << 16 | (uint32_t)(unsigned char)(d) << 24)

/* the section types this build knows (section 2.3) */
#define PRSX_GEOM PRSX_FOURCC('G', 'E', 'O', 'M')
#define PRSX_LMUV PRSX_FOURCC('L', 'M', 'U', 'V')
#define PRSX_SCEN PRSX_FOURCC('S', 'C', 'E', 'N')
#define PRSX_RHGT PRSX_FOURCC('R', 'H', 'G', 'T')
#define PRSX_DOBJ PRSX_FOURCC('D', 'O', 'B', 'J')
#define PRSX_COLL PRSX_FOURCC('C', 'O', 'L', 'L')
#define PRSX_COLM PRSX_FOURCC('C', 'O', 'L', 'M')
#define PRSX_PATH PRSX_FOURCC('P', 'A', 'T', 'H')

/* a type's class in this build's table: whether it is known, and whether it
enters the enhanced world_id (BAKE_RELEVANT) or not (runtime, sim) */
enum prsx_type_class
{
	PRSX_TYPE_UNKNOWN,
	PRSX_TYPE_BAKE,
	PRSX_TYPE_RUNTIME,
	PRSX_TYPE_SIM,
};

enum prsx_type_class prsx_type_class(uint32_t type);

/* a byte range to read from: read gives size bytes at offset (from the
range's start) and returns 1, or 0 if it could not */
struct prsx_source
{
	void *context;
	int (*read)(void *context, uint64_t offset, void *buffer, size_t size);
	uint64_t size;
	/* where this range starts in its context's own bytes */
	uint64_t base;
};

void prsx_source_memory(struct prsx_source *source, const void *bytes, uint64_t size);
/* a stdio file opened "rb" by the caller, who keeps it open while the
source is used (the sidecar is held for the session: section 3.4) */
int prsx_source_file(struct prsx_source *source, FILE *file);
/* the container appended to a whole file (a map with our data after it,
found by its trailer): 1 and container set, or 0 if there is none */
int prsx_source_appended(const struct prsx_source *whole, struct prsx_source *container);

struct prsx_entry
{
	uint32_t type;
	uint16_t bsp;
	uint16_t flags;
	uint32_t collv;
	uint64_t offset;
	uint64_t csize;
	uint64_t usize;
	unsigned char sha256[32];
};

struct prsx
{
	struct prsx_source source;
	uint32_t format;
	uint32_t flags;
	unsigned char classic_world_id[PRSX_ID_SIZE];
	uint32_t section_count;
	struct prsx_entry *entries;
};

enum prsx_result
{
	PRSX_OK,
	PRSX_ERROR_READ,
	PRSX_ERROR_SIGNATURE,
	PRSX_ERROR_FORMAT,
	PRSX_ERROR_HEADER,
	PRSX_ERROR_DIRECTORY,
	PRSX_ERROR_REQUIRED,
	PRSX_ERROR_CLASS,
	PRSX_ERROR_MEMORY,
	PRSX_ERROR_CODEC,
	PRSX_ERROR_SIZE,
	PRSX_ERROR_HASH,
	PRSX_ERROR_INDEX,
};

const char *prsx_result_string(enum prsx_result result);

/* the header and directory read and checked: every entry inside the source,
sizes in range, codec known, no two entries overlapping, every REQUIRED
type known, and every known type's BAKE_RELEVANT flag what this build's
table says. On PRSX_OK, prsx_close frees it */
enum prsx_result prsx_open(struct prsx *prsx, const struct prsx_source *source);
void prsx_close(struct prsx *prsx);

/* the entry of type for bsp and collv (bsp PRSX_BSP_ALL matches only an
all-BSP entry): its index, or -1 */
long prsx_find(const struct prsx *prsx, uint32_t type, uint16_t bsp, uint32_t collv);

/* section index's bytes, decoded into buffer (its usize bytes), checked
against its sha256_decoded */
enum prsx_result prsx_section_read(const struct prsx *prsx, uint32_t index, void *buffer, size_t buffer_size);

/* ---------- SHA-256 (the port's, mcc_sha256.c) */

void prsx_sha256(const void *bytes, size_t size, unsigned char digest[32]);

#endif
