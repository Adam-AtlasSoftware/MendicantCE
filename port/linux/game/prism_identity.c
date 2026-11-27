/*
PRISM_IDENTITY.C

prism_identity.h has the identities and their derivation. C89, no game
headers (the farm's tools build it: tools/geopatch/native.py).
*/

#include "prism_identity.h"

#include <stdlib.h>
#include <string.h>

#include "../../third_party/zlib/zlib_prefixed.h"

/* ---------- base_id */

void prism_base_id_begin(struct prism_base_id *context)
{
	mcc_sha256_begin(&context->sha);
}

void prism_base_id_add(struct prism_base_id *context, const void *bytes, unsigned long size)
{
	const unsigned char *p = (const unsigned char *)bytes;

	while (size > 0)
	{
		int step = size > 0x40000000UL ? 0x40000000 : (int)size;

		mcc_sha256_add(&context->sha, p, step);
		p += step;
		size -= (unsigned long)step;
	}
}

void prism_base_id_end(struct prism_base_id *context, unsigned char id[PRISM_ID_SIZE])
{
	mcc_sha256_end(&context->sha, id);
}

static uint32_t identity_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

enum
{
	IDENTITY_CHUNK = 256 * 1024,
	/* the longest map the game opens (cache_files.c: cache_file_header_verify) */
	IDENTITY_MAXIMUM_LENGTH = 0x11600000,
};

int prism_base_id_of_file(FILE *file, unsigned char id[PRISM_ID_SIZE])
{
	unsigned char header[PRISM_MAP_HEADER_SIZE];
	unsigned char *in = NULL, *out = NULL;
	struct prism_base_id context;
	unsigned long body, done = 0;
	long file_size;
	int whole = 0;

	if (!file || fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0 ||
		fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "daeh", 4) != 0 ||
		memcmp(header + 0x7FC, "toof", 4) != 0)
	{
		return 0;
	}
	if (identity_u32(header + 8) < PRISM_MAP_HEADER_SIZE || identity_u32(header + 8) > IDENTITY_MAXIMUM_LENGTH)
		return 0;
	body = (unsigned long)identity_u32(header + 8) - PRISM_MAP_HEADER_SIZE;
	in = (unsigned char *)malloc(IDENTITY_CHUNK);
	out = (unsigned char *)malloc(IDENTITY_CHUNK);
	if (!in || !out)
		goto done;
	prism_base_id_begin(&context);
	/* (a file at least its length long is inflated already, as a slot is; a
	disc map's zlib stream is always shorter than the body it inflates to) */
	if ((unsigned long)file_size < body + PRISM_MAP_HEADER_SIZE)
	{
		z_stream stream;
		int status = Z_OK;

		memset(&stream, 0, sizeof(stream));
		if (inflateInit(&stream) != Z_OK)
			goto done;
		while (done < body && status != Z_STREAM_END)
		{
			if (!stream.avail_in)
			{
				size_t count = fread(in, 1, IDENTITY_CHUNK, file);

				if (!count)
					break;
				stream.next_in = in;
				stream.avail_in = (uInt)count;
			}
			stream.next_out = out;
			stream.avail_out = (uInt)(body - done < IDENTITY_CHUNK ? body - done : IDENTITY_CHUNK);
			status = inflate(&stream, Z_NO_FLUSH);
			if (status != Z_OK && status != Z_STREAM_END)
				break;
			prism_base_id_add(&context, out, (unsigned long)(stream.next_out - out));
			done += (unsigned long)(stream.next_out - out);
		}
		inflateEnd(&stream);
	}
	else
	{
		while (done < body)
		{
			size_t want = body - done < IDENTITY_CHUNK ? (size_t)(body - done) : IDENTITY_CHUNK;

			if (fread(out, 1, want, file) != want)
				break;
			prism_base_id_add(&context, out, (unsigned long)want);
			done += (unsigned long)want;
		}
	}
	if (done == body)
	{
		prism_base_id_end(&context, id);
		whole = 1;
	}

done:
	free(in);
	free(out);
	return whole;
}

/* ---------- world_id */

static void identity_add_u16(struct mcc_sha256 *sha, uint16_t value)
{
	unsigned char bytes[2];

	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	mcc_sha256_add(sha, bytes, 2);
}

static void identity_add_u32(struct mcc_sha256 *sha, uint32_t value)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
	mcc_sha256_add(sha, bytes, 4);
}

void prism_world_id_classic(const unsigned char classic_world_id[PRISM_ID_SIZE], uint16_t bsp,
	unsigned char id[PRISM_ID_SIZE])
{
	struct mcc_sha256 sha;

	mcc_sha256_begin(&sha);
	mcc_sha256_add(&sha, "PRSX-WORLD-C", 12);
	mcc_sha256_add(&sha, classic_world_id, PRISM_ID_SIZE);
	identity_add_u16(&sha, bsp);
	identity_add_u32(&sha, PRISM_SCENE_VERSION);
	mcc_sha256_end(&sha, id);
}

int prism_world_id_counts(const struct prsx_entry *entry, uint16_t bsp)
{
	/* (prsx_open has made a known type's flag its table's) */
	return (entry->flags & PRSX_BAKE_RELEVANT) && (entry->bsp == bsp || entry->bsp == PRSX_BSP_ALL);
}

void prism_world_id_enhanced(const struct prsx *prsx, uint16_t bsp, unsigned char id[PRISM_ID_SIZE])
{
	struct mcc_sha256 sha;
	uint32_t index;

	mcc_sha256_begin(&sha);
	mcc_sha256_add(&sha, "PRSX-WORLD-E", 12);
	mcc_sha256_add(&sha, prsx->classic_world_id, PRISM_ID_SIZE);
	identity_add_u16(&sha, bsp);
	identity_add_u32(&sha, PRISM_SCENE_VERSION);
	identity_add_u32(&sha, PRISM_RENDER_FORMAT);
	for (index = 0; index < prsx->section_count; index++)
	{
		const struct prsx_entry *entry = &prsx->entries[index];

		if (prism_world_id_counts(entry, bsp))
		{
			identity_add_u32(&sha, entry->type);
			mcc_sha256_add(&sha, entry->sha256, 32);
		}
	}
	mcc_sha256_end(&sha, id);
}

/* ---------- text */

void prism_id_hex(const unsigned char id[PRISM_ID_SIZE], char text[2 * PRISM_ID_SIZE + 1])
{
	static const char digits[] = "0123456789abcdef";
	int index;

	for (index = 0; index < PRISM_ID_SIZE; index++)
	{
		text[2 * index] = digits[id[index] >> 4];
		text[2 * index + 1] = digits[id[index] & 15];
	}
	text[2 * PRISM_ID_SIZE] = 0;
}

static int identity_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int prism_id_parse(const char *text, unsigned char id[PRISM_ID_SIZE])
{
	int index;

	for (index = 0; index < 2 * PRISM_ID_SIZE; index++)
	{
		if (identity_digit(text[index]) < 0)
			return 0;
	}
	if (text[2 * PRISM_ID_SIZE] && text[2 * PRISM_ID_SIZE] != ' ' && text[2 * PRISM_ID_SIZE] != '\n')
		return 0;
	for (index = 0; index < PRISM_ID_SIZE; index++)
		id[index] = (unsigned char)(identity_digit(text[2 * index]) << 4 | identity_digit(text[2 * index + 1]));
	return 1;
}

int prism_world_ladder(int lighting_classic, int geometry_enhanced, int sidecar_ok, int packs_ok, int unbaked,
	int path_traced)
{
	if (lighting_classic)
		return 3;
	if (geometry_enhanced && sidecar_ok && (packs_ok || unbaked || path_traced))
		return 1;
	return 2;
}
