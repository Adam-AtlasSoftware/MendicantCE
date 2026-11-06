/*
FORGE_MVAR.C (test)

Forge's map variant files (port/linux/game/forge_mvar.c, docs/forge_mvar.md):
what is written is read back the same, a damaged or truncated file is
refused, chunks and records of later versions are skipped, nothing is read
past the end of the data; and the turn kept as a quaternion
(forge_layout.c's forge_layout_rotation_of_axes and
forge_layout_axes_of_rotation) gives back the object's forward and up.
test_forge_mvar.py takes the code (under_test.inc).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "harness.h"

#include <math.h>

/* the engine's maths, as forge_layout.c calls it */
#define square_root(value) sqrtf(value)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void set_real_vector3d(real_vector3d *vector, real i, real j, real k)
{
	vector->i = i;
	vector->j = j;
	vector->k = k;
}

static void cross_product3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *result)
{
	set_real_vector3d(result, a->j * b->k - a->k * b->j, a->k * b->i - a->i * b->k, a->i * b->j - a->j * b->i);
}

static real dot(real_vector3d const *a, real_vector3d const *b)
{
	return a->i * b->i + a->j * b->j + a->k * b->k;
}

/* network_distributed.c's: axes that are numbers and not nothing, made
unit and at right angles */
static boolean distributed_axes_make_valid(real_vector3d *forward, real_vector3d *up)
{
	real length = sqrtf(dot(forward, forward));
	real along;

	if (!(length > 0.0001f) || !isfinite(length))
		return FALSE;
	set_real_vector3d(forward, forward->i / length, forward->j / length, forward->k / length);
	along = dot(up, forward);
	set_real_vector3d(up, up->i - along * forward->i, up->j - along * forward->j, up->k - along * forward->k);
	length = sqrtf(dot(up, up));
	if (!(length > 0.0001f) || !isfinite(length))
		return FALSE;
	set_real_vector3d(up, up->i / length, up->j / length, up->k / length);
	return TRUE;
}

#include "under_test.inc"

static unsigned char buffer[FORGE_MVAR_MAXIMUM_SIZE];
static unsigned char copy[FORGE_MVAR_MAXIMUM_SIZE];

/* a variant of the header, two strings, three objects and a note */
static unsigned long write_variant(unsigned char *data, unsigned long size)
{
	struct forge_mvar_writer writer;
	struct forge_mvar_header header;
	struct forge_mvar_object object;
	struct forge_mvar_editor editor;
	int index;

	memset(&header, 0, sizeof(header));
	strcpy(header.name, "Two forts");
	strcpy(header.description, "A bridge between them");
	strcpy(header.author, "tester");
	strcpy(header.scenario, "levels\\test\\bloodgulch\\bloodgulch");
	header.created = 1700000000UL;
	header.modified = 1700000500UL;
	header.listed = TRUE;
	header.next_object_id = 4;

	forge_mvar_writer_new(&writer, data, size);
	forge_mvar_chunk_begin(&writer, "mvhd", FORGE_MVAR_VERSION);
	forge_mvar_put_header(&writer, &header);
	forge_mvar_chunk_end(&writer);
	forge_mvar_chunk_begin(&writer, "strt", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(&writer, 2);
	forge_mvar_put_string(&writer, "scenery\\rocks\\boulder\\boulder");
	forge_mvar_put_string(&writer, "vehicles\\warthog\\mp_warthog");
	forge_mvar_chunk_end(&writer);
	forge_mvar_chunk_begin(&writer, "mvob", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(&writer, 3);
	forge_mvar_put_u16(&writer, FORGE_MVAR_OBJECT_RECORD_SIZE);
	forge_mvar_put_u16(&writer, 0);
	for (index = 0; index < 3; index++)
	{
		memset(&object, 0, sizeof(object));
		object.id = (unsigned long)(index + 1);
		object.kind = index;
		object.type = index == 1 ? 1 : 6;
		object.team = index == 2 ? -1 : index;
		object.spawn_flags = FORGE_MVAR_SPAWN_AT_START | (index == 1 ? FORGE_MVAR_SPAWN_NEVER_RESPAWN : 0);
		object.group = index == 1 ? 0x76656869UL : 0x7363656eUL;
		object.tag = (unsigned int)(index == 1);
		object.respawn_seconds = (unsigned int)(index * 30);
		object.game_types = index == 0 ? 0 : 1u << 2;
		object.position[0] = 12.5f + (float)index;
		object.position[1] = -40.25f;
		object.position[2] = 1.0f / 3.0f;
		object.rotation[0] = 0.f;
		object.rotation[1] = 0.f;
		object.rotation[2] = 0.70710678f;
		object.rotation[3] = 0.70710678f;
		object.scale = index == 0 ? 2.5f : 0.f;
		object.original[0] = 1.f;
		object.original[1] = 2.f;
		object.original[2] = -3.f;
		forge_mvar_put_object(&writer, &object);
	}
	forge_mvar_chunk_end(&writer);
	forge_mvar_chunk_begin(&writer, "mved", FORGE_MVAR_VERSION);
	forge_mvar_put_u32(&writer, 1);
	forge_mvar_put_u16(&writer, FORGE_MVAR_EDITOR_RECORD_SIZE);
	forge_mvar_put_u16(&writer, 0);
	memset(&editor, 0, sizeof(editor));
	editor.id = 2;
	editor.group = 7;
	editor.flags = FORGE_MVAR_EDITOR_LOCKED;
	strcpy(editor.comment, "the red team's hog");
	forge_mvar_put_editor(&writer, &editor);
	forge_mvar_chunk_end(&writer);

	return forge_mvar_writer_finish(&writer);
}

/* every chunk of the data read; FALSE when the data is refused or a chunk
did not read */
static boolean read_all(unsigned char const *data, unsigned long length)
{
	struct forge_mvar_reader reader;
	struct forge_mvar_reader chunk;
	char tag[5];
	unsigned int version;

	if (!forge_mvar_open(&reader, data, length))
		return FALSE;
	while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
	{
	}
	return !reader.failed;
}

/* a chunk's header at data */
static void put_chunk_header(unsigned char *data, char const *tag, unsigned long size)
{
	int index;

	memcpy(data, tag, 4);
	for (index = 0; index < 4; index++)
		data[4 + index] = (unsigned char)(size >> (8 * index));
	data[8] = 1;
	data[9] = data[10] = data[11] = 0;
}

static void check_axes(real_vector3d forward, real_vector3d up, char const *what)
{
	float rotation[4];
	real_vector3d forward_back;
	real_vector3d up_back;

	CHECK(distributed_axes_make_valid(&forward, &up), "%s: not axes", what);
	forge_layout_rotation_of_axes(&forward, &up, rotation);
	CHECK(fabsf(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] +
		rotation[3] * rotation[3] - 1.f) < 1e-5f, "%s: not a unit quaternion", what);
	CHECK(rotation[3] >= 0.f, "%s: w below 0", what);
	CHECK(forge_layout_axes_of_rotation(rotation, &forward_back, &up_back), "%s: refused", what);
	CHECK(fabsf(forward_back.i - forward.i) < 1e-5f && fabsf(forward_back.j - forward.j) < 1e-5f &&
		fabsf(forward_back.k - forward.k) < 1e-5f, "%s: forward %f %f %f came back %f %f %f", what,
		forward.i, forward.j, forward.k, forward_back.i, forward_back.j, forward_back.k);
	CHECK(fabsf(up_back.i - up.i) < 1e-5f && fabsf(up_back.j - up.j) < 1e-5f && fabsf(up_back.k - up.k) < 1e-5f,
		"%s: up %f %f %f came back %f %f %f", what, up.i, up.j, up.k, up_back.i, up_back.j, up_back.k);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	unsigned long length = write_variant(buffer, sizeof(buffer));

	CHECK(length > 0, "the variant was not written");

	/* what is written is read back the same */
	CASE("round-trip")
	{
		struct forge_mvar_reader reader;
		struct forge_mvar_reader chunk;
		struct forge_mvar_header header;
		struct forge_mvar_object object;
		struct forge_mvar_editor editor;
		unsigned long count;
		unsigned long record_size;
		char tag[5];
		char path[256];
		unsigned int version;
		int chunks = 0;

		CHECK(forge_mvar_open(&reader, buffer, length), "the variant was refused");
		while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
		{
			chunks++;
			CHECK(version == FORGE_MVAR_VERSION, "a chunk of version %u", version);
			if (!strcmp(tag, "mvhd"))
			{
				CHECK(chunk.length == 496, "mvhd is %lu bytes (docs/forge_mvar.md says 496)", chunk.length);
				CHECK(forge_mvar_get_header(&chunk, &header), "mvhd did not read");
				CHECK(!strcmp(header.name, "Two forts") && !strcmp(header.description, "A bridge between them") &&
					!strcmp(header.author, "tester") && !strcmp(header.scenario, "levels\\test\\bloodgulch\\bloodgulch"),
					"mvhd's texts came back as %s / %s", header.name, header.scenario);
				CHECK(header.created == 1700000000UL && header.modified == 1700000500UL && header.listed &&
					header.next_object_id == 4, "mvhd's numbers came back wrong");
			}
			else if (!strcmp(tag, "strt"))
			{
				CHECK(forge_mvar_get_u32(&chunk) == 2, "two strings were written");
				CHECK(forge_mvar_get_string(&chunk, path, sizeof(path)) && !strcmp(path, "scenery\\rocks\\boulder\\boulder"),
					"the first string came back as %s", path);
				CHECK(forge_mvar_get_string(&chunk, path, sizeof(path)) && !strcmp(path, "vehicles\\warthog\\mp_warthog"),
					"the second string came back as %s", path);
			}
			else if (!strcmp(tag, "mvob"))
			{
				int index;

				CHECK(forge_mvar_get_table(&chunk, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size) && count == 3 &&
					record_size == 64, "the table came back as %lu of %lu", count, record_size);
				CHECK(chunk.length == 8 + 3 * 64, "the objects are %lu bytes", chunk.length);
				for (index = 0; index < 3; index++)
				{
					CHECK(forge_mvar_get_object(&chunk, record_size, &object), "object %d did not read", index);
					CHECK(object.id == (unsigned long)(index + 1) && object.kind == index &&
						object.type == (index == 1 ? 1 : 6) && object.team == (index == 2 ? -1 : index) &&
						object.tag == (unsigned int)(index == 1) && object.respawn_seconds == (unsigned int)(index * 30) &&
						object.game_types == (index == 0 ? 0u : 4u) &&
						object.spawn_flags == (index == 1 ? 3u : 1u) &&
						object.group == (index == 1 ? 0x76656869UL : 0x7363656eUL),
						"object %d's numbers came back wrong", index);
					CHECK(object.position[0] == 12.5f + (float)index && object.position[1] == -40.25f &&
						object.position[2] == 1.0f / 3.0f && object.rotation[2] == 0.70710678f &&
						object.scale == (index == 0 ? 2.5f : 0.f) && object.original[2] == -3.f,
						"object %d's floats came back changed", index);
				}
			}
			else if (!strcmp(tag, "mved"))
			{
				CHECK(forge_mvar_get_table(&chunk, FORGE_MVAR_EDITOR_RECORD_SIZE, &count, &record_size) && count == 1,
					"one note was written");
				CHECK(forge_mvar_get_editor(&chunk, record_size, &editor) && editor.id == 2 && editor.group == 7 &&
					editor.flags == FORGE_MVAR_EDITOR_LOCKED && !strcmp(editor.comment, "the red team's hog"),
					"the note came back wrong");
			}
			CHECK(!chunk.failed && chunk.offset == chunk.length, "the chunk %s was not read to its end", tag);
		}
		CHECK(!reader.failed && chunks == 4 && reader.offset == length, "the file did not end with _eof (%d chunks)",
			chunks);
		return 0;
	}
	/* a changed byte, a file cut short or made longer, a newer version: refused */
	CASE("refuses-damage")
	{
		unsigned long index;

		CHECK(read_all(buffer, length), "the variant was refused");
		for (index = 0; index < length; index += 7)
		{
			memcpy(copy, buffer, length);
			copy[index] ^= 0x10;
			CHECK(!read_all(copy, length), "a file with byte %lu changed was taken", index);
		}
		for (index = 0; index < length; index++)
			CHECK(!read_all(buffer, index), "a file cut to %lu bytes was taken", index);
		memcpy(copy, buffer, length);
		copy[length] = 0;
		CHECK(!read_all(copy, length + 1), "a file with a byte after it was taken");
		memcpy(copy, buffer, length);
		copy[8] = 2;
		forge_mvar_seal(copy, length);
		CHECK(!read_all(copy, length), "a file of a newer version was taken");
		CHECK(!read_all(buffer, FORGE_MVAR_MAXIMUM_SIZE + 1), "a file over the most a variant may be was taken");
		return 0;
	}
	/* a chunk that runs past the end, or a file without _eof, stops the reader */
	CASE("chunk-bounds")
	{
		struct forge_mvar_reader reader;
		struct forge_mvar_reader chunk;
		char tag[5];
		unsigned int version;

		/* _eof (the last 12 bytes) said to hold a byte more than there is */
		memcpy(copy, buffer, length);
		put_chunk_header(copy + length - 12, "zzzz", 1);
		forge_mvar_seal(copy, length);
		CHECK(forge_mvar_open(&reader, copy, length), "the file's _blf was refused");
		while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
			CHECK(chunk.data + chunk.length <= copy + length, "a chunk ran past the end of the file");
		CHECK(reader.failed, "a chunk past the end was not seen");
		/* no _eof */
		CHECK(forge_mvar_open(&reader, buffer, length - 12) == FALSE || !read_all(buffer, length - 12),
			"a file without _eof was taken");
		return 0;
	}
	/* a chunk this version does not know is skipped */
	CASE("unknown-chunks")
	{
		struct forge_mvar_reader reader;
		struct forge_mvar_reader chunk;
		struct forge_mvar_header header;
		char tag[5];
		unsigned int version;
		unsigned long blf = FORGE_MVAR_CHUNK_HEADER_SIZE + 12;
		boolean found = FALSE;

		/* a chunk of 5 bytes put before the rest */
		memcpy(copy, buffer, blf);
		put_chunk_header(copy + blf, "newc", 5);
		memset(copy + blf + 12, 0xee, 5);
		memcpy(copy + blf + 17, buffer + blf, length - blf);
		forge_mvar_seal(copy, length + 17);
		CHECK(forge_mvar_open(&reader, copy, length + 17), "a file with a new chunk was refused");
		while (forge_mvar_next_chunk(&reader, tag, &version, &chunk))
		{
			if (!strcmp(tag, "mvhd"))
				found = forge_mvar_get_header(&chunk, &header) && !strcmp(header.name, "Two forts");
		}
		CHECK(!reader.failed && found, "the chunks after a new one were not read");
		return 0;
	}
	/* a record longer than this version's: its fields read, the rest skipped */
	CASE("longer-records")
	{
		static unsigned char table[8 + 2 * 80];
		struct forge_mvar_writer writer;
		struct forge_mvar_reader reader;
		struct forge_mvar_object object;
		unsigned long count;
		unsigned long record_size;
		int index;

		writer.data = table;
		writer.size = sizeof(table);
		writer.length = 0;
		writer.chunk_start = 0;
		writer.overflowed = FALSE;
		forge_mvar_put_u32(&writer, 2);
		forge_mvar_put_u16(&writer, 80);
		forge_mvar_put_u16(&writer, 0);
		for (index = 0; index < 2; index++)
		{
			memset(&object, 0, sizeof(object));
			object.id = (unsigned long)(10 + index);
			object.position[0] = 5.f;
			forge_mvar_put_object(&writer, &object);
			forge_mvar_put_bytes(&writer, "sixteen more....", 16);
		}
		CHECK(!writer.overflowed, "the table did not fit");
		reader.data = table;
		reader.length = writer.length;
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(forge_mvar_get_table(&reader, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size) && count == 2 &&
			record_size == 80, "the table was refused");
		for (index = 0; index < 2; index++)
		{
			CHECK(forge_mvar_get_object(&reader, record_size, &object) && object.id == (unsigned long)(10 + index) &&
				object.position[0] == 5.f, "record %d of a later version came back as id %lu", index, object.id);
		}
		CHECK(reader.offset == reader.length && !reader.failed, "the records' ends were not skipped");
		return 0;
	}
	/* a table that says it holds more than its chunk does, or shorter records: refused */
	CASE("table-bounds")
	{
		static unsigned char table[8 + 64];
		struct forge_mvar_reader reader;
		unsigned long count;
		unsigned long record_size;

		memset(table, 0, sizeof(table));
		table[0] = 2;
		table[4] = 64;
		reader.data = table;
		reader.length = sizeof(table);
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_table(&reader, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size) && count == 0,
			"a table of 2 records in room for 1 was taken");
		table[0] = 0xff;
		table[1] = 0xff;
		table[2] = 0xff;
		table[3] = 0xff;
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_table(&reader, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size),
			"a table of 4294967295 records was taken");
		table[0] = 1;
		table[1] = table[2] = table[3] = 0;
		table[4] = 32;
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_table(&reader, FORGE_MVAR_OBJECT_RECORD_SIZE, &count, &record_size),
			"records shorter than this version's were taken");
		return 0;
	}
	/* a string longer than its room, or than what is left */
	CASE("strings")
	{
		static unsigned char data[16] = { 20, 0, 'a', 'b', 'c' };
		static unsigned char eight[10] = { 8, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
		struct forge_mvar_reader reader;
		char result[8];
		char room[16];

		reader.data = data;
		reader.length = sizeof(data);
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_string(&reader, result, sizeof(result)) && reader.failed && result[0] == 0,
			"a string of 20 was taken into 8");
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_string(&reader, (char *)copy, 64) && reader.failed,
			"a string of 20 was taken from 14 bytes");
		/* (its terminator needs room too) */
		reader.data = eight;
		reader.length = sizeof(eight);
		reader.offset = 0;
		reader.failed = FALSE;
		CHECK(!forge_mvar_get_string(&reader, room, 8) && reader.failed, "a string of 8 was taken into 8");
		return 0;
	}
	/* what does not fit is not written */
	CASE("overflow")
	{
		unsigned long size;

		for (size = 0; size < length; size += 13)
			CHECK(write_variant(copy, size) == 0, "a variant of %lu bytes was written into %lu", length, size);
		CHECK(write_variant(copy, length) == length && !memcmp(copy, buffer, length),
			"the variant did not fit its own size");
		return 0;
	}
	/* the turn as a quaternion gives back the object's axes */
	CASE("quaternion")
	{
		real_vector3d forward;
		real_vector3d up;
		float rotation[4];
		int index;

		set_real_vector3d(&forward, 1.f, 0.f, 0.f);
		set_real_vector3d(&up, 0.f, 0.f, 1.f);
		forge_layout_rotation_of_axes(&forward, &up, rotation);
		CHECK(rotation[0] == 0.f && rotation[1] == 0.f && rotation[2] == 0.f && rotation[3] == 1.f,
			"the world's own axes are %f %f %f %f", rotation[0], rotation[1], rotation[2], rotation[3]);
		/* a quarter turn left about z: forward along y */
		set_real_vector3d(&forward, 0.f, 1.f, 0.f);
		forge_layout_rotation_of_axes(&forward, &up, rotation);
		CHECK(fabsf(rotation[2] - 0.70710678f) < 1e-6f && fabsf(rotation[3] - 0.70710678f) < 1e-6f,
			"a quarter turn left is %f %f %f %f", rotation[0], rotation[1], rotation[2], rotation[3]);
		/* each branch: half turns about each axis, and upside down */
		set_real_vector3d(&forward, -1.f, 0.f, 0.f);
		check_axes(forward, up, "a half turn about z");
		set_real_vector3d(&up, 0.f, 0.f, -1.f);
		check_axes(forward, up, "a half turn about y");
		set_real_vector3d(&forward, 1.f, 0.f, 0.f);
		check_axes(forward, up, "a half turn about x");
		set_real_vector3d(&forward, 0.f, 0.f, 1.f);
		set_real_vector3d(&up, -1.f, 0.f, 0.f);
		check_axes(forward, up, "nose straight up");
		srand(1);
		for (index = 0; index < 20000; index++)
		{
			set_real_vector3d(&forward, (real)(rand() % 20001) / 20000.f - 0.5f, (real)(rand() % 20001) / 20000.f - 0.5f,
				(real)(rand() % 20001) / 20000.f - 0.5f);
			set_real_vector3d(&up, (real)(rand() % 20001) / 20000.f - 0.5f, (real)(rand() % 20001) / 20000.f - 0.5f,
				(real)(rand() % 20001) / 20000.f - 0.5f);
			cross_product3d(&forward, &up, &up);
			cross_product3d(&up, &forward, &up);
			if (distributed_axes_make_valid(&forward, &up))
				check_axes(forward, up, "a turn");
		}
		return 0;
	}
	/* what cannot be a turn is refused */
	CASE("bad-quaternion")
	{
		static float const bad[][4] =
		{
			{ 0.f, 0.f, 0.f, 0.f },
			{ 3.f, 0.f, 0.f, 0.f },
			{ 0.1f, 0.1f, 0.1f, 0.1f },
		};
		float not_a_number[4] = { 0.f, 0.f, 0.f, 1.f };
		real_vector3d forward;
		real_vector3d up;
		float slightly_off[4] = { 0.f, 0.f, 0.f, 1.1f };
		int index;

		for (index = 0; index < (int)(sizeof(bad) / sizeof(bad[0])); index++)
			CHECK(!forge_layout_axes_of_rotation(bad[index], &forward, &up), "quaternion %d was taken", index);
		not_a_number[1] = NAN;
		CHECK(!forge_layout_axes_of_rotation(not_a_number, &forward, &up), "a quaternion with NaN was taken");
		not_a_number[1] = INFINITY;
		CHECK(!forge_layout_axes_of_rotation(not_a_number, &forward, &up), "a quaternion with infinity was taken");
		CHECK(forge_layout_axes_of_rotation(slightly_off, &forward, &up) && fabsf(forward.i - 1.f) < 1e-5f &&
			fabsf(up.k - 1.f) < 1e-5f, "a quaternion a little long was not made unit");
		return 0;
	}

	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
