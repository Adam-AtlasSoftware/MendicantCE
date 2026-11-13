/*
REPLAY_RECORD.C (test)

A playback's walk of a replay's records, port/linux/game/
network_replay_playback.c's playback_record, over a store of the test's own:
each record is found by its size, a record of no bytes too (it was read for
ever), a record that crosses the window, and a file cut short ends the
walk. test_replay_record.py takes the function (under_test.inc) and its
constants (config.inc).
*/

#include "harness.h"

#include <signal.h>
#include <stdint.h>
#include <unistd.h>

typedef unsigned short word;

#include "config.inc"

static struct
{
	byte *window;
	long size;
	long window_start;
	long window_length;
} playback;

static byte *store;
static long store_size;

static uint32_t network_replay_store_read(uint32_t offset, byte *out, uint32_t most)
{
	uint32_t count = offset >= (uint32_t)store_size ? 0 : (uint32_t)store_size - offset;

	if (count > most)
		count = most;
	memcpy(out, store + offset, count);
	return count;
}

#include "under_test.inc"

static void alarmed(int number)
{
	static const char text[] = "playback_record did not return\n";

	(void)number;
	if (write(2, text, sizeof(text) - 1) < 0)
		_exit(1);
	_exit(1);
}

static long put(long at, byte kind, word size)
{
	store[at] = kind;
	store[at + 1] = 0;
	store[at + 2] = (byte)size;
	store[at + 3] = (byte)(size >> 8);
	memset(store + at + 4, 0, 4);
	memset(store + at + 8, 0xAB, size);
	return at + 8 + size;
}

/* the records walked from the first: their count */
static long walk(void)
{
	long next = 32, count = 0;
	word size = 0;
	byte const *record;

	playback.window_start = 0;
	playback.window_length = 0;
	while ((record = playback_record(next, &size)) != NULL)
	{
		CHECK(!memcmp(record, store + next, 8 + (size_t)size), "the record at %ld is not the store's", next);
		count++;
		next += REPLAY_RECORD_HEADER_SIZE + size;
	}
	return count;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	long at = 32;

	store = calloc(1, (size_t)16 * REPLAY_WINDOW_SIZE);
	playback.window = malloc(REPLAY_WINDOW_SIZE);
	CHECK(store && playback.window, "memory");
	signal(SIGALRM, alarmed);
	alarm(3);

	/* records of every size, each found */
	CASE("sizes")
	{
		at = put(at, 5, 300);
		at = put(at, 1, 40);
		at = put(at, 2, 65535);
		at = put(at, 1, 1);
		store_size = playback.size = at;
		CHECK(walk() == 4, "4 records");
		return 0;
	}

	/* a record of no bytes is its header alone: returned, and passed */
	CASE("empty")
	{
		at = put(at, 5, 300);
		at = put(at, 1, 0);
		at = put(at, 1, 40);
		at = put(at, 2, 0);
		store_size = playback.size = at;
		CHECK(walk() == 4, "4 records, two of no bytes");
		return 0;
	}

	/* records past the window's end are read into it again */
	CASE("window")
	{
		int index;

		for (index = 0; index < 40; index++)
			at = put(at, (byte)(1 + index % 3), (word)(index % 7 ? 60000 : 0));
		store_size = playback.size = at;
		CHECK(at > REPLAY_WINDOW_SIZE, "the records are more than a window");
		CHECK(walk() == 40, "40 records across the window's end");
		return 0;
	}

	/* a file cut short ends at its last whole record */
	CASE("cut")
	{
		at = put(at, 5, 300);
		at = put(at, 1, 40);
		at = put(at, 2, 500);
		store_size = playback.size = at - 100;
		CHECK(walk() == 2, "2 whole records");
		store_size = playback.size = 32 + 4;
		CHECK(walk() == 0, "no header whole");
		return 0;
	}
	return 2;
}
