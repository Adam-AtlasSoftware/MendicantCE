/* A map's preparation's record (port/linux/src/d3d8_device.c's journal,
port/linux/game/render_prepared.c): the entries read back are the entries
recorded, a repeat once, in their order, with the textures and pages between
them; malformed words are refused; a record whose draws the played back ones
do not all have, or that is another form's, map's, build's or settings', is
refused. */
#include "harness.h"

#include <stdint.h>

typedef unsigned long DWORD;
typedef int BOOL;
#define D3DTSS_MAXSTAGES 4
#define D3DRS_MAX 144
#define D3DTSS_MAX 32

/* FNV-1a, 64 bits (xgpu_manifest.c's) */
static uint64_t xgpu_manifest_hash(const void *data, size_t size, uint64_t hash)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
	return hash;
}
#define XGPU_MANIFEST_HASH_START 14695981039346656037ULL

#include "journal.inc"

static struct
{
	uint32_t *words;
	unsigned long count, capacity;
	BOOL failed;
	struct journal_state last;
	uint64_t seen[JOURNAL_SEEN_SLOTS];
	unsigned long seen_count;
} journal;

#include "journal_code.inc"
#include "prepared.inc"

#define POOL 40
#define ENTRIES 3000
#define PAGE_COUNT 4096

static struct journal_state pool[POOL];
static struct { unsigned long op; int state; } expected[ENTRIES];
static int expected_count;
static uint32_t textures_expected[ENTRIES][2];
static int textures_expected_count;

static struct { unsigned long op; struct journal_state state; } read_entries[ENTRIES];
static int read_count;
static uint32_t textures_read[ENTRIES][2];
static int textures_read_count;
static unsigned long pages_read[16][2];
static int pages_read_count;

static void on_entry(unsigned long op, const struct journal_state *state)
{
	CHECK(read_count < ENTRIES, "too many entries");
	read_entries[read_count].op = op;
	read_entries[read_count].state = *state;
	read_count++;
}

static void on_texture(uint32_t tag_index, uint32_t bitmap_index)
{
	textures_read[textures_read_count][0] = tag_index;
	textures_read[textures_read_count][1] = bitmap_index;
	textures_read_count++;
}

static void on_pages(unsigned long first, unsigned long end)
{
	pages_read[pages_read_count][0] = first;
	pages_read[pages_read_count][1] = end;
	pages_read_count++;
}

static const struct journal_reader reader = { on_entry, on_texture, on_pages };

static void texture_note(uint32_t tag_index, uint32_t bitmap_index)
{
	CHECK(journal_reserve(3), "room");
	journal.words[journal.count++] = JOURNAL_TEXTURE;
	journal.words[journal.count++] = tag_index;
	journal.words[journal.count++] = bitmap_index;
	textures_expected[textures_expected_count][0] = tag_index;
	textures_expected[textures_expected_count][1] = bitmap_index;
	textures_expected_count++;
}

static int read_back(struct journal_state *state)
{
	memset(state, 0, sizeof(*state));
	read_count = textures_read_count = pages_read_count = 0;
	return journal_read(journal.words, journal.count, state, PAGE_COUNT, &reader);
}

/* states that differ in a few of their words, as one draw's from the next */
static void pool_make(void)
{
	int index, word;

	srand(7);
	for (index = 0; index < POOL; index++)
	{
		uint32_t *words = (uint32_t *)&pool[index];

		if (index)
			pool[index] = pool[rand() % index];
		for (word = 0; word < 1 + rand() % 6; word++)
			words[rand() % JOURNAL_STATE_WORDS] = (uint32_t)rand();
	}
}

static void record(void)
{
	int index, seen;

	memset(&journal, 0, sizeof(journal));
	pool_make();
	expected_count = textures_expected_count = 0;
	for (index = 0; index < ENTRIES; index++)
	{
		unsigned long op = JOURNAL_DRAW + (unsigned long)(rand() % 3);
		int state = rand() % POOL;

		if (index && rand() % 7 == 0)
			texture_note((uint32_t)rand(), (uint32_t)(rand() % 16));
		journal_append(op, &pool[state]);
		for (seen = 0; seen < expected_count && (expected[seen].op != op || expected[seen].state != state); seen++)
			;
		if (seen == expected_count)
		{
			expected[expected_count].op = op;
			expected[expected_count].state = state;
			expected_count++;
		}
	}
	/* (the pages last, as halo_gpu_journal_take writes them) */
	CHECK(journal_reserve(5), "room");
	journal.words[journal.count++] = JOURNAL_PAGES | 2u << 8;
	journal.words[journal.count++] = 3;
	journal.words[journal.count++] = 9;
	journal.words[journal.count++] = 100;
	journal.words[journal.count++] = 101;
}

static void case_roundtrip(void)
{
	struct journal_state state;
	int index;

	record();
	CHECK(read_back(&state), "the words read");
	CHECK(read_count == expected_count, "%d entries read, %d recorded apart", read_count, expected_count);
	for (index = 0; index < read_count; index++)
	{
		CHECK(read_entries[index].op == expected[index].op, "entry %d's type", index);
		CHECK(!memcmp(&read_entries[index].state, &pool[expected[index].state], sizeof(state)), "entry %d's state",
			index);
	}
	CHECK(textures_read_count == textures_expected_count, "%d textures read of %d", textures_read_count,
		textures_expected_count);
	CHECK(!memcmp(textures_read, textures_expected, sizeof(textures_read[0]) * textures_read_count), "the textures");
	CHECK(pages_read_count == 2 && pages_read[0][0] == 3 && pages_read[0][1] == 9 && pages_read[1][0] == 100,
		"the pages");
}

static void case_malformed(void)
{
	struct journal_state state;
	uint32_t saved;
	unsigned long count;

	record();
	count = journal.count;
	/* (cut short: in the first entry's pairs) */
	journal.count = 2;
	CHECK(!read_back(&state), "a journal cut short is refused");
	journal.count = count;
	/* (a type no entry has) */
	saved = journal.words[0];
	journal.words[0] = 0x33;
	journal.count = 1;
	CHECK(!read_back(&state), "an unknown type is refused");
	journal.words[0] = saved;
	journal.count = count;
	/* (a word past the state's) */
	saved = journal.words[1];
	journal.words[1] = JOURNAL_STATE_WORDS;
	CHECK(!read_back(&state), "a word past the state is refused");
	journal.words[1] = saved;
	/* (pages past the mirror's) */
	journal.words[count - 1] = PAGE_COUNT + 1;
	CHECK(!read_back(&state), "pages past the mirror are refused");
	journal.words[count - 1] = 101;
	CHECK(read_back(&state), "the journal as recorded is read");
}

static void case_missing(void)
{
	unsigned long long recorded[] = { 1, 3, 5, 7 };
	unsigned long long more[] = { 1, 2, 3, 5, 7, 9 };
	unsigned long long fewer[] = { 1, 3, 7 };
	unsigned long long other[] = { 2, 4, 6, 8 };

	CHECK(render_prepared_draws_missing(recorded, 4, recorded, 4) == 0, "the same draws");
	CHECK(render_prepared_draws_missing(recorded, 4, more, 6) == 0, "more draws than the record's");
	CHECK(render_prepared_draws_missing(recorded, 4, fewer, 3) == 1, "a draw not made");
	CHECK(render_prepared_draws_missing(recorded, 4, other, 4) == 4, "none of the record's draws");
	CHECK(render_prepared_draws_missing(recorded, 4, NULL, 0) == 4, "no draws");
}

static void case_stale(void)
{
	struct prepared_header header, changed;

	memset(&header, 0, sizeof(header));
	header.magic = PREPARED_MAGIC;
	header.version = PREPARED_VERSION;
	header.header_size = sizeof(header);
	header.checksum = 0x12345678;
	header.key = 0x1122334455667788ULL;
	header.word_count = 100;
	header.draw_count = 10;
	CHECK(prepared_header_fits(&header, 0x12345678, 0x1122334455667788ULL), "the record's own");
	changed = header;
	changed.version++;
	CHECK(!prepared_header_fits(&changed, 0x12345678, 0x1122334455667788ULL), "another form's");
	CHECK(!prepared_header_fits(&header, 0x12345679, 0x1122334455667788ULL), "another map's");
	CHECK(!prepared_header_fits(&header, 0x12345678, 0x1122334455667789ULL), "another build's or settings'");
	changed = header;
	changed.magic = 0;
	CHECK(!prepared_header_fits(&changed, 0x12345678, 0x1122334455667788ULL), "not a record");
	changed = header;
	changed.word_count = 0;
	CHECK(!prepared_header_fits(&changed, 0x12345678, 0x1122334455667788ULL), "an empty record");
	changed = header;
	changed.word_count = PREPARED_MAXIMUM_WORDS + 1;
	CHECK(!prepared_header_fits(&changed, 0x12345678, 0x1122334455667788ULL), "a record too large");
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "roundtrip"))
		case_roundtrip();
	else if (!strcmp(which, "malformed"))
		case_malformed();
	else if (!strcmp(which, "missing"))
		case_missing();
	else if (!strcmp(which, "stale"))
		case_stale();
	else
		CHECK(0, "no case %s", which);
	return 0;
}
