/*
NETWORK_REPLAY_STORE.C

A replay's records, kept in memory for the playback to read a window of at
a time (network_replay_playback.c). A long match's replay inflates to more
than one piece of memory should be: a 46 MB file of 22 minutes inflated to
152 MB.

The bytes are kept in chunks, so that no one allocation is large and growing
never copies what is already kept. One replay at a time, on the game's
thread alone.

Ported from SlipstreamCE's host_replay.c (thelinkin3000/SlipstreamCE@f8dbc4411,
by thelinkin3000; AGPL-3.0-or-later), where the Switch's guest kept them in
its host's memory: OpenH1's guests keep them in their own heap, so the store
is the guest's (it uses the C library alone).
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE (4u * 1024 * 1024)

static struct
{
	unsigned char **chunks;
	uint32_t chunk_count;
	uint32_t chunk_room;
	uint32_t size;
} store;

/* the replay kept before let go (none kept: nothing done) */
void network_replay_store_free(void)
{
	uint32_t index;

	for (index = 0; index < store.chunk_count; index++)
		free(store.chunks[index]);
	free(store.chunks);
	memset(&store, 0, sizeof(store));
}

/* size bytes at data added to the replay's end; 0 if there
is not the memory for them (what was kept before is kept) */
int network_replay_store_append(void const *data, uint32_t size)
{
	const unsigned char *from = (const unsigned char *)data;

	if (size > UINT32_MAX - store.size)
		return 0;
	while (size)
	{
		uint32_t used = store.size % CHUNK_SIZE;
		uint32_t part;

		if (!used && store.size / CHUNK_SIZE == store.chunk_count)
		{
			unsigned char *chunk;

			if (store.chunk_count == store.chunk_room)
			{
				uint32_t room = store.chunk_room ? store.chunk_room * 2 : 64;
				unsigned char **chunks = realloc(store.chunks, room * sizeof(*chunks));

				if (!chunks)
					return 0;
				store.chunks = chunks;
				store.chunk_room = room;
			}
			if (!(chunk = malloc(CHUNK_SIZE)))
				return 0;
			store.chunks[store.chunk_count++] = chunk;
		}
		part = CHUNK_SIZE - used;
		if (part > size)
			part = size;
		memcpy(store.chunks[store.size / CHUNK_SIZE] + used, from, part);
		from += part;
		size -= part;
		store.size += part;
	}
	return 1;
}

/* up to size bytes of the replay from offset copied to
destination; how many there were */
uint32_t network_replay_store_read(uint32_t offset, void *destination, uint32_t size)
{
	unsigned char *to = (unsigned char *)destination;
	uint32_t copied = 0;

	if (offset >= store.size)
		return 0;
	if (size > store.size - offset)
		size = store.size - offset;
	while (copied < size)
	{
		uint32_t at = offset + copied;
		uint32_t part = CHUNK_SIZE - at % CHUNK_SIZE;

		if (part > size - copied)
			part = size - copied;
		memcpy(to + copied, store.chunks[at / CHUNK_SIZE] + at % CHUNK_SIZE, part);
		copied += part;
	}
	return copied;
}
