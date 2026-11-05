/*
PREPARED_SHIPPED.C

The shipped records found and inflated (prepared_shipped.h).
*/

#include "prepared_shipped.h"

#include "zlib_prefixed.h"


/* the largest record inflated (render_prepared.c's own limit is larger: a
shipped record is a stock map's, under 1 MB) */
#define PREPARED_SHIPPED_MAXIMUM_SIZE (64U << 20)

static const struct prepared_shipped *prepared_shipped_find(unsigned int checksum, unsigned int key_high,
	unsigned int key_low)
{
	unsigned int index;

	for (index = 0; index < prepared_shipped_count; index++)
	{
		const struct prepared_shipped *record = &prepared_shipped[index];

		if (record->data && record->checksum == checksum && record->key_high == key_high &&
			record->key_low == key_low)
			return record;
	}
	return NULL;
}

unsigned int prepared_shipped_size(unsigned int checksum, unsigned int key_high, unsigned int key_low)
{
	const struct prepared_shipped *record = prepared_shipped_find(checksum, key_high, key_low);

	return record && record->size <= PREPARED_SHIPPED_MAXIMUM_SIZE ? record->size : 0;
}

int prepared_shipped_inflate(unsigned int checksum, unsigned int key_high, unsigned int key_low, void *into,
	unsigned int size)
{
	const struct prepared_shipped *record = prepared_shipped_find(checksum, key_high, key_low);
	uLongf inflated = size;

	return record && size == record->size && uncompress(into, &inflated, (const unsigned char *)record->data,
		record->compressed_size) == Z_OK && inflated == record->size;
}
