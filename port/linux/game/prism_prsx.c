/*
PRISM_PRSX.C

The sidecar reader (prism_prsx.h has the format). C89, no game headers, so
that the farm's tools build it as it is (tools/geopatch/native.py).
*/

#include "prism_prsx.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "mcc_sha256.h"
/* the port's zlib, the one that unpacks the maps */
#include "../../third_party/zlib/zlib_prefixed.h"

/* ---------- little-endian fields */

static uint32_t prsx_u16(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t prsx_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t prsx_u64(const unsigned char *p)
{
	return (uint64_t)prsx_u32(p) | (uint64_t)prsx_u32(p + 4) << 32;
}

/* ---------- the type table (section 2.5: a CI table marks each type
BAKE_RELEVANT or not; tools/geopatch/prsx.py has its twin, and
tools/test_prism_world_ids.py checks they agree) */

static const struct
{
	uint32_t type;
	enum prsx_type_class type_class;
} prsx_types[] =
{
	{ PRSX_GEOM, PRSX_TYPE_BAKE },
	{ PRSX_LMUV, PRSX_TYPE_BAKE },
	{ PRSX_SCEN, PRSX_TYPE_BAKE },
	{ PRSX_RHGT, PRSX_TYPE_RUNTIME },
	{ PRSX_DOBJ, PRSX_TYPE_RUNTIME },
	{ PRSX_COLL, PRSX_TYPE_SIM },
	{ PRSX_COLM, PRSX_TYPE_SIM },
	{ PRSX_PATH, PRSX_TYPE_SIM },
};

enum prsx_type_class prsx_type_class(uint32_t type)
{
	size_t index;

	for (index = 0; index < sizeof(prsx_types) / sizeof(prsx_types[0]); index++)
	{
		if (prsx_types[index].type == type)
			return prsx_types[index].type_class;
	}
	return PRSX_TYPE_UNKNOWN;
}

/* ---------- sources */

struct prsx_memory
{
	const unsigned char *bytes;
};

static int prsx_memory_read(void *context, uint64_t offset, void *buffer, size_t size)
{
	memcpy(buffer, (const unsigned char *)context + offset, size);
	return 1;
}

void prsx_source_memory(struct prsx_source *source, const void *bytes, uint64_t size)
{
	source->context = (void *)bytes;
	source->read = prsx_memory_read;
	source->size = size;
	source->base = 0;
}

static int prsx_file_read(void *context, uint64_t offset, void *buffer, size_t size)
{
	FILE *file = (FILE *)context;

	/* (stdio's long offsets, as the game's other readers: a sidecar is far
	under 2 GB, and prsx_source_file refuses a file that is not) */
	if (offset > (uint64_t)LONG_MAX || fseek(file, (long)offset, SEEK_SET) != 0)
		return 0;
	return fread(buffer, 1, size, file) == size;
}

int prsx_source_file(struct prsx_source *source, FILE *file)
{
	long size;

	if (!file || fseek(file, 0, SEEK_END) != 0)
		return 0;
	size = ftell(file);
	if (size < 0 || size == LONG_MAX)
		return 0;
	source->context = file;
	source->read = prsx_file_read;
	source->size = (uint64_t)size;
	source->base = 0;
	return 1;
}

/* size bytes at offset of the range (checked inside it) */
static int prsx_read(const struct prsx_source *source, uint64_t offset, void *buffer, size_t size)
{
	if (offset > source->size || (uint64_t)size > source->size - offset)
		return 0;
	if (!size)
		return 1;
	return source->read(source->context, source->base + offset, buffer, size);
}

int prsx_source_appended(const struct prsx_source *whole, struct prsx_source *container)
{
	unsigned char trailer[PRSX_TRAILER_SIZE];
	uint64_t offset, size;

	if (whole->size < PRSX_TRAILER_SIZE + PRSX_HEADER_SIZE ||
		!prsx_read(whole, whole->size - PRSX_TRAILER_SIZE, trailer, sizeof(trailer)) ||
		memcmp(trailer, "PRSXTAIL", 8) != 0)
	{
		return 0;
	}
	offset = prsx_u64(trailer + 8);
	size = prsx_u64(trailer + 16);
	/* (the container ends where the trailer starts) */
	if (offset > whole->size - PRSX_TRAILER_SIZE || size != whole->size - PRSX_TRAILER_SIZE - offset ||
		size < PRSX_HEADER_SIZE)
	{
		return 0;
	}
	*container = *whole;
	container->base = whole->base + offset;
	container->size = size;
	return 1;
}

/* ---------- the header and directory */

const char *prsx_result_string(enum prsx_result result)
{
	switch (result)
	{
		case PRSX_OK: return "ok";
		case PRSX_ERROR_READ: return "could not be read";
		case PRSX_ERROR_SIGNATURE: return "not a PRSX file";
		case PRSX_ERROR_FORMAT: return "a format this build does not read";
		case PRSX_ERROR_HEADER: return "a malformed header";
		case PRSX_ERROR_DIRECTORY: return "a malformed directory";
		case PRSX_ERROR_REQUIRED: return "a required section this build does not know";
		case PRSX_ERROR_CLASS: return "a section whose bake class disagrees with this build's";
		case PRSX_ERROR_MEMORY: return "out of memory";
		case PRSX_ERROR_CODEC: return "a section in a codec this build does not read";
		case PRSX_ERROR_SIZE: return "a section of the wrong size";
		case PRSX_ERROR_HASH: return "a section whose SHA-256 differs";
		case PRSX_ERROR_INDEX: return "no such section";
	}
	return "?";
}

/* the directory's order: type (as its little-endian u32), BSP, collision
version */
static int prsx_key_compare(const struct prsx_entry *x, const struct prsx_entry *y)
{
	if (x->type != y->type)
		return x->type < y->type ? -1 : 1;
	if (x->bsp != y->bsp)
		return x->bsp < y->bsp ? -1 : 1;
	if (x->collv != y->collv)
		return x->collv < y->collv ? -1 : 1;
	return 0;
}

static int prsx_entry_compare(const void *a, const void *b)
{
	const struct prsx_entry *x = *(const struct prsx_entry *const *)a;
	const struct prsx_entry *y = *(const struct prsx_entry *const *)b;

	return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

enum prsx_result prsx_open(struct prsx *prsx, const struct prsx_source *source)
{
	unsigned char header[PRSX_HEADER_SIZE];
	unsigned char *directory = NULL;
	const struct prsx_entry **order = NULL;
	enum prsx_result result = PRSX_OK;
	uint64_t directory_end;
	uint32_t index;

	memset(prsx, 0, sizeof(*prsx));
	prsx->source = *source;
	if (!prsx_read(source, 0, header, sizeof(header)))
		return source->size < PRSX_HEADER_SIZE ? PRSX_ERROR_SIGNATURE : PRSX_ERROR_READ;
	if (memcmp(header, "PRSX", 4) != 0)
		return PRSX_ERROR_SIGNATURE;
	prsx->format = prsx_u32(header + 4);
	if (prsx->format < PRSX_FORMAT_OLDEST || prsx->format > PRSX_FORMAT)
		return PRSX_ERROR_FORMAT;
	memcpy(prsx->classic_world_id, header + 8, PRSX_ID_SIZE);
	prsx->section_count = prsx_u32(header + 40);
	prsx->flags = prsx_u32(header + 44);
	if (prsx->flags != 0 || prsx->section_count > PRSX_MAXIMUM_SECTIONS)
		return PRSX_ERROR_HEADER;
	directory_end = PRSX_HEADER_SIZE + (uint64_t)prsx->section_count * PRSX_ENTRY_SIZE;
	if (directory_end > source->size)
		return PRSX_ERROR_DIRECTORY;
	if (!prsx->section_count)
		return PRSX_OK;
	directory = (unsigned char *)malloc((size_t)prsx->section_count * PRSX_ENTRY_SIZE);
	prsx->entries = (struct prsx_entry *)calloc(prsx->section_count, sizeof(struct prsx_entry));
	order = (const struct prsx_entry **)malloc(prsx->section_count * sizeof(*order));
	if (!directory || !prsx->entries || !order)
	{
		result = PRSX_ERROR_MEMORY;
		goto done;
	}
	if (!prsx_read(source, PRSX_HEADER_SIZE, directory, (size_t)prsx->section_count * PRSX_ENTRY_SIZE))
	{
		result = PRSX_ERROR_READ;
		goto done;
	}
	for (index = 0; index < prsx->section_count; index++)
	{
		const unsigned char *p = directory + (size_t)index * PRSX_ENTRY_SIZE;
		struct prsx_entry *entry = &prsx->entries[index];
		enum prsx_type_class type_class;
		uint32_t codec;

		entry->type = prsx_u32(p);
		entry->bsp = (uint16_t)prsx_u16(p + 4);
		entry->flags = (uint16_t)prsx_u16(p + 6);
		entry->collv = prsx_u32(p + 8);
		entry->offset = prsx_u64(p + 12);
		entry->csize = prsx_u64(p + 20);
		entry->usize = prsx_u64(p + 28);
		memcpy(entry->sha256, p + 36, 32);
		codec = (entry->flags & PRSX_CODEC_MASK) >> PRSX_CODEC_SHIFT;
		type_class = prsx_type_class(entry->type);
		if ((entry->flags & ~PRSX_FLAGS_KNOWN) || entry->offset < directory_end ||
			entry->offset > source->size || entry->csize > source->size - entry->offset ||
			entry->usize > PRSX_MAXIMUM_SECTION_BYTES ||
			(codec == PRSX_CODEC_STORED && entry->csize != entry->usize))
		{
			result = PRSX_ERROR_DIRECTORY;
			goto done;
		}
		if (type_class == PRSX_TYPE_UNKNOWN)
		{
			if (entry->flags & PRSX_REQUIRED)
			{
				result = PRSX_ERROR_REQUIRED;
				goto done;
			}
			if (entry->flags & PRSX_BAKE_RELEVANT)
			{
				/* (a section this build skips can never enter its world_id:
				packs baked with it in place would light geometry this build
				does not draw, so the file falls to the classic world) */
				result = PRSX_ERROR_CLASS;
				goto done;
			}
		}
		else if ((type_class == PRSX_TYPE_BAKE) != ((entry->flags & PRSX_BAKE_RELEVANT) != 0) ||
			(type_class == PRSX_TYPE_SIM) != (entry->collv != 0))
		{
			/* (a known type is bake relevant exactly when the table says so,
			and a collision version is set exactly on the sim sections) */
			result = PRSX_ERROR_CLASS;
			goto done;
		}
		if (codec != PRSX_CODEC_STORED && codec != PRSX_CODEC_ZLIB && codec != PRSX_CODEC_ZSTD)
		{
			result = PRSX_ERROR_DIRECTORY;
			goto done;
		}
		if (codec == PRSX_CODEC_ZSTD && type_class != PRSX_TYPE_UNKNOWN)
		{
			/* (a section this build would read in a codec it cannot: refused
			here, at map open, not half way through the load) */
			result = PRSX_ERROR_CODEC;
			goto done;
		}
		/* (the directory is sorted by type, BSP and collision version, each
		key once: one file for one content, so the world_id, hashed in
		directory order, is canonical) */
		if (index > 0 && prsx_key_compare(&prsx->entries[index - 1], entry) >= 0)
		{
			result = PRSX_ERROR_DIRECTORY;
			goto done;
		}
		order[index] = entry;
	}
	/* (no two sections share bytes) */
	qsort((void *)order, prsx->section_count, sizeof(*order), prsx_entry_compare);
	for (index = 1; index < prsx->section_count; index++)
	{
		if (order[index - 1]->offset + order[index - 1]->csize > order[index]->offset)
		{
			result = PRSX_ERROR_DIRECTORY;
			goto done;
		}
	}

done:
	free(directory);
	free((void *)order);
	if (result != PRSX_OK)
	{
		free(prsx->entries);
		prsx->entries = NULL;
		prsx->section_count = 0;
	}
	return result;
}

void prsx_close(struct prsx *prsx)
{
	free(prsx->entries);
	memset(prsx, 0, sizeof(*prsx));
}

long prsx_find(const struct prsx *prsx, uint32_t type, uint16_t bsp, uint32_t collv)
{
	uint32_t index;

	for (index = 0; index < prsx->section_count; index++)
	{
		const struct prsx_entry *entry = &prsx->entries[index];

		if (entry->type == type && entry->bsp == bsp && entry->collv == collv)
			return (long)index;
	}
	return -1;
}

/* ---------- sections */

static enum prsx_result prsx_inflate(const unsigned char *in, uint64_t in_size, unsigned char *out, uint64_t out_size)
{
	z_stream stream;
	int status;

	memset(&stream, 0, sizeof(stream));
	if (inflateInit(&stream) != Z_OK)
		return PRSX_ERROR_MEMORY;
	stream.next_in = (Bytef *)in;
	stream.avail_in = (uInt)in_size;
	stream.next_out = out;
	stream.avail_out = (uInt)out_size;
	status = inflate(&stream, Z_FINISH);
	/* (exactly usize bytes, and the stream ends there with nothing after) */
	if (status != Z_STREAM_END || stream.total_out != out_size || stream.avail_in != 0)
	{
		inflateEnd(&stream);
		return PRSX_ERROR_SIZE;
	}
	inflateEnd(&stream);
	return PRSX_OK;
}

enum prsx_result prsx_section_read(const struct prsx *prsx, uint32_t index, void *buffer, size_t buffer_size)
{
	const struct prsx_entry *entry;
	unsigned char digest[32];
	enum prsx_result result = PRSX_OK;
	uint32_t codec;

	if (index >= prsx->section_count)
		return PRSX_ERROR_INDEX;
	entry = &prsx->entries[index];
	if ((uint64_t)buffer_size != entry->usize)
		return PRSX_ERROR_SIZE;
	codec = (entry->flags & PRSX_CODEC_MASK) >> PRSX_CODEC_SHIFT;
	if (codec == PRSX_CODEC_STORED)
	{
		if (!prsx_read(&prsx->source, entry->offset, buffer, (size_t)entry->usize))
			return PRSX_ERROR_READ;
	}
	else if (codec == PRSX_CODEC_ZLIB)
	{
		unsigned char *packed;

		if (entry->csize > 0xFFFFFFFFu || entry->usize > 0xFFFFFFFFu)
			return PRSX_ERROR_SIZE;
		packed = (unsigned char *)malloc(entry->csize ? (size_t)entry->csize : 1);
		if (!packed)
			return PRSX_ERROR_MEMORY;
		if (!prsx_read(&prsx->source, entry->offset, packed, (size_t)entry->csize))
			result = PRSX_ERROR_READ;
		else
			result = prsx_inflate(packed, entry->csize, (unsigned char *)buffer, entry->usize);
		free(packed);
		if (result != PRSX_OK)
			return result;
	}
	else
	{
		return PRSX_ERROR_CODEC;
	}
	prsx_sha256(buffer, buffer_size, digest);
	if (memcmp(digest, entry->sha256, 32) != 0)
		return PRSX_ERROR_HASH;
	return PRSX_OK;
}

void prsx_sha256(const void *bytes, size_t size, unsigned char digest[32])
{
	struct mcc_sha256 context;
	const unsigned char *p = (const unsigned char *)bytes;

	mcc_sha256_begin(&context);
	while (size > 0)
	{
		int step = size > 0x40000000u ? 0x40000000 : (int)size;

		mcc_sha256_add(&context, p, step);
		p += step;
		size -= (size_t)step;
	}
	mcc_sha256_end(&context, digest);
}
