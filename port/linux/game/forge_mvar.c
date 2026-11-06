/*
FORGE_MVAR.C

The .mvar format of Forge's map variants (forge_mvar.h,
docs/forge_mvar.md): chunks of little-endian fields, written into a buffer
and read from one with every length checked. What the fields mean, and
what is done with them, is forge_layout.c's.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "forge_mvar.h"

#include <string.h>

typedef char verify_forge_mvar_unsigned_long_size[sizeof(unsigned long) == 4 ? 1 : -1];
typedef char verify_forge_mvar_float_size[sizeof(float) == 4 ? 1 : -1];

/* ---------- the CRC */

/* CRC-32 (IEEE 802.3, as zip's), a bit at a time: a file is read rarely */
unsigned long forge_mvar_crc32(
	void const *data,
	unsigned long length)
{
	unsigned char const *bytes = (unsigned char const *)data;
	unsigned long crc = 0xffffffffUL;
	unsigned long index;
	int bit;

	for (index = 0; index < length; index++)
	{
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
	}

	return crc ^ 0xffffffffUL;
}

/* ---------- writing */

static void forge_mvar_put_at(
	struct forge_mvar_writer *writer,
	unsigned long offset,
	unsigned long value,
	unsigned long count)
{
	unsigned long index;

	for (index = 0; index < count; index++)
		writer->data[offset + index] = (unsigned char)(value >> (8 * index));

	return;
}

static void forge_mvar_put(
	struct forge_mvar_writer *writer,
	unsigned long value,
	unsigned long count)
{
	if (writer->overflowed || writer->size - writer->length < count)
	{
		writer->overflowed = 1;
		return;
	}
	forge_mvar_put_at(writer, writer->length, value, count);
	writer->length += count;

	return;
}

void forge_mvar_put_u8(
	struct forge_mvar_writer *writer,
	unsigned int value)
{
	forge_mvar_put(writer, value & 0xff, 1);

	return;
}

void forge_mvar_put_u16(
	struct forge_mvar_writer *writer,
	unsigned int value)
{
	forge_mvar_put(writer, value & 0xffff, 2);

	return;
}

void forge_mvar_put_u32(
	struct forge_mvar_writer *writer,
	unsigned long value)
{
	forge_mvar_put(writer, value, 4);

	return;
}

void forge_mvar_put_f32(
	struct forge_mvar_writer *writer,
	float value)
{
	unsigned long bits;

	memcpy(&bits, &value, sizeof(bits));
	forge_mvar_put(writer, bits, 4);

	return;
}

void forge_mvar_put_bytes(
	struct forge_mvar_writer *writer,
	void const *bytes,
	unsigned long count)
{
	if (writer->overflowed || writer->size - writer->length < count)
	{
		writer->overflowed = 1;
		return;
	}
	memcpy(writer->data + writer->length, bytes, count);
	writer->length += count;

	return;
}

void forge_mvar_put_text(
	struct forge_mvar_writer *writer,
	char const *text,
	unsigned long size)
{
	unsigned long length = 0;

	/* (always ended: one byte is left for the terminator) */
	while (text[length] && length < size - 1)
		length++;
	forge_mvar_put_bytes(writer, text, length);
	for (; length < size; length++)
		forge_mvar_put(writer, 0, 1);

	return;
}

void forge_mvar_put_string(
	struct forge_mvar_writer *writer,
	char const *text)
{
	unsigned long length = (unsigned long)strlen(text);

	if (length > FORGE_MVAR_MAXIMUM_STRING_LENGTH)
		length = FORGE_MVAR_MAXIMUM_STRING_LENGTH;
	forge_mvar_put_u16(writer, (unsigned int)length);
	forge_mvar_put_bytes(writer, text, length);

	return;
}

void forge_mvar_chunk_begin(
	struct forge_mvar_writer *writer,
	char const *tag,
	unsigned int version)
{
	writer->chunk_start = writer->length;
	forge_mvar_put_bytes(writer, tag, 4);
	/* (its size, once it is all written) */
	forge_mvar_put_u32(writer, 0);
	forge_mvar_put_u16(writer, version);
	forge_mvar_put_u16(writer, 0);

	return;
}

void forge_mvar_chunk_end(
	struct forge_mvar_writer *writer)
{
	if (!writer->overflowed)
	{
		forge_mvar_put_at(writer, writer->chunk_start + 4,
			writer->length - writer->chunk_start - FORGE_MVAR_CHUNK_HEADER_SIZE, 4);
	}

	return;
}

void forge_mvar_writer_new(
	struct forge_mvar_writer *writer,
	unsigned char *data,
	unsigned long size)
{
	writer->data = data;
	writer->size = size;
	writer->length = 0;
	writer->chunk_start = 0;
	writer->overflowed = 0;

	/* the file's length and CRC are written when it is finished */
	forge_mvar_chunk_begin(writer, "_blf", FORGE_MVAR_VERSION);
	forge_mvar_put_bytes(writer, "mvar", 4);
	forge_mvar_put_u32(writer, 0);
	forge_mvar_put_u32(writer, 0);
	forge_mvar_chunk_end(writer);

	return;
}

void forge_mvar_seal(
	unsigned char *data,
	unsigned long length)
{
	unsigned long blf_size = FORGE_MVAR_CHUNK_HEADER_SIZE + 12;
	unsigned long crc = forge_mvar_crc32(data + blf_size, length - blf_size);
	short index;

	for (index = 0; index < 4; index++)
	{
		data[FORGE_MVAR_CHUNK_HEADER_SIZE + 4 + index] = (unsigned char)(length >> (8 * index));
		data[FORGE_MVAR_CHUNK_HEADER_SIZE + 8 + index] = (unsigned char)(crc >> (8 * index));
	}

	return;
}

unsigned long forge_mvar_writer_finish(
	struct forge_mvar_writer *writer)
{
	forge_mvar_chunk_begin(writer, "_eof", FORGE_MVAR_VERSION);
	forge_mvar_chunk_end(writer);
	if (writer->overflowed)
		return 0;
	forge_mvar_seal(writer->data, writer->length);

	return writer->length;
}

void forge_mvar_put_header(
	struct forge_mvar_writer *writer,
	struct forge_mvar_header const *header)
{
	forge_mvar_put_text(writer, header->name, FORGE_MVAR_NAME_SIZE);
	forge_mvar_put_text(writer, header->description, FORGE_MVAR_DESCRIPTION_SIZE);
	forge_mvar_put_text(writer, header->author, FORGE_MVAR_AUTHOR_SIZE);
	forge_mvar_put_text(writer, header->scenario, FORGE_MVAR_SCENARIO_SIZE);
	forge_mvar_put_u32(writer, header->created);
	forge_mvar_put_u32(writer, header->modified);
	forge_mvar_put_u8(writer, header->listed ? 1 : 0);
	forge_mvar_put_u8(writer, 0);
	forge_mvar_put_u16(writer, 0);
	forge_mvar_put_u32(writer, header->next_object_id);

	return;
}

void forge_mvar_put_object(
	struct forge_mvar_writer *writer,
	struct forge_mvar_object const *object)
{
	short index;

	forge_mvar_put_u32(writer, object->id);
	forge_mvar_put_u8(writer, (unsigned int)object->kind);
	forge_mvar_put_u8(writer, (unsigned int)object->type);
	forge_mvar_put_u8(writer, (unsigned int)(object->team < 0 ? 0xff : object->team));
	forge_mvar_put_u8(writer, object->spawn_flags);
	forge_mvar_put_u32(writer, object->group);
	forge_mvar_put_u16(writer, object->tag);
	forge_mvar_put_u16(writer, object->respawn_seconds);
	forge_mvar_put_u16(writer, object->game_types);
	forge_mvar_put_u16(writer, 0);
	for (index = 0; index < 3; index++)
		forge_mvar_put_f32(writer, object->position[index]);
	for (index = 0; index < 4; index++)
		forge_mvar_put_f32(writer, object->rotation[index]);
	forge_mvar_put_f32(writer, object->scale);
	for (index = 0; index < 3; index++)
		forge_mvar_put_f32(writer, object->original[index]);

	return;
}

void forge_mvar_put_editor(
	struct forge_mvar_writer *writer,
	struct forge_mvar_editor const *editor)
{
	forge_mvar_put_u32(writer, editor->id);
	forge_mvar_put_u16(writer, editor->group);
	forge_mvar_put_u8(writer, editor->flags);
	forge_mvar_put_u8(writer, 0);
	forge_mvar_put_text(writer, editor->comment, FORGE_MVAR_COMMENT_SIZE);

	return;
}

/* ---------- reading */

static unsigned long forge_mvar_get(
	struct forge_mvar_reader *reader,
	unsigned long count)
{
	unsigned long value = 0;
	unsigned long index;

	if (reader->failed || reader->length - reader->offset < count)
	{
		reader->failed = 1;
		return 0;
	}
	for (index = 0; index < count; index++)
		value |= (unsigned long)reader->data[reader->offset + index] << (8 * index);
	reader->offset += count;

	return value;
}

unsigned int forge_mvar_get_u8(
	struct forge_mvar_reader *reader)
{
	return (unsigned int)forge_mvar_get(reader, 1);
}

unsigned int forge_mvar_get_u16(
	struct forge_mvar_reader *reader)
{
	return (unsigned int)forge_mvar_get(reader, 2);
}

unsigned long forge_mvar_get_u32(
	struct forge_mvar_reader *reader)
{
	return forge_mvar_get(reader, 4);
}

float forge_mvar_get_f32(
	struct forge_mvar_reader *reader)
{
	unsigned long bits = forge_mvar_get(reader, 4);
	float value;

	memcpy(&value, &bits, sizeof(value));

	return value;
}

unsigned char const *forge_mvar_get_bytes(
	struct forge_mvar_reader *reader,
	unsigned long count)
{
	unsigned char const *bytes;

	if (reader->failed || reader->length - reader->offset < count)
	{
		reader->failed = 1;
		return NULL;
	}
	bytes = reader->data + reader->offset;
	reader->offset += count;

	return bytes;
}

void forge_mvar_get_text(
	struct forge_mvar_reader *reader,
	char *result,
	unsigned long size)
{
	unsigned char const *bytes = forge_mvar_get_bytes(reader, size);

	if (bytes)
		memcpy(result, bytes, size);
	else
		memset(result, 0, size);
	result[size - 1] = 0;

	return;
}

int forge_mvar_get_string(
	struct forge_mvar_reader *reader,
	char *result,
	unsigned long size)
{
	unsigned long length = forge_mvar_get_u16(reader);
	unsigned char const *bytes;

	result[0] = 0;
	if (length >= size)
	{
		reader->failed = 1;
		return 0;
	}
	if ((bytes = forge_mvar_get_bytes(reader, length)) == NULL)
		return 0;
	memcpy(result, bytes, length);
	result[length] = 0;

	return 1;
}

int forge_mvar_open(
	struct forge_mvar_reader *reader,
	void const *data,
	unsigned long length)
{
	unsigned long blf_size = FORGE_MVAR_CHUNK_HEADER_SIZE + 12;
	unsigned char const *magic;
	unsigned long size;
	unsigned int version;

	reader->data = (unsigned char const *)data;
	reader->length = length;
	reader->offset = 0;
	reader->failed = 0;

	if (length < blf_size + FORGE_MVAR_CHUNK_HEADER_SIZE || length > FORGE_MVAR_MAXIMUM_SIZE ||
		memcmp(reader->data, "_blf", 4) != 0)
	{
		reader->failed = 1;
		return 0;
	}
	reader->offset = 4;
	size = forge_mvar_get_u32(reader);
	version = forge_mvar_get_u16(reader);
	forge_mvar_get_u16(reader);
	magic = forge_mvar_get_bytes(reader, 4);
	/* (a newer game's file may mean what this one cannot read) */
	if (size < 12 || version < 1 || version > FORGE_MVAR_VERSION || !magic || memcmp(magic, "mvar", 4) != 0 ||
		forge_mvar_get_u32(reader) != length ||
		forge_mvar_get_u32(reader) != forge_mvar_crc32(reader->data + blf_size, length - blf_size))
	{
		reader->failed = 1;
		return 0;
	}
	/* (the rest of a longer _blf) */
	reader->offset = FORGE_MVAR_CHUNK_HEADER_SIZE;
	if (!forge_mvar_get_bytes(reader, size))
		return 0;

	return !reader->failed;
}

int forge_mvar_next_chunk(
	struct forge_mvar_reader *reader,
	char tag[5],
	unsigned int *version,
	struct forge_mvar_reader *chunk)
{
	unsigned char const *letters = forge_mvar_get_bytes(reader, 4);
	unsigned long size = forge_mvar_get_u32(reader);
	unsigned char const *contents;

	*version = forge_mvar_get_u16(reader);
	forge_mvar_get_u16(reader);
	contents = forge_mvar_get_bytes(reader, size);
	if (!letters || !contents)
	{
		/* (a file must end with _eof) */
		reader->failed = 1;
		return 0;
	}
	memcpy(tag, letters, 4);
	tag[4] = 0;
	chunk->data = contents;
	chunk->length = size;
	chunk->offset = 0;
	chunk->failed = 0;

	return memcmp(tag, "_eof", 4) != 0;
}

int forge_mvar_get_header(
	struct forge_mvar_reader *reader,
	struct forge_mvar_header *header)
{
	forge_mvar_get_text(reader, header->name, FORGE_MVAR_NAME_SIZE);
	forge_mvar_get_text(reader, header->description, FORGE_MVAR_DESCRIPTION_SIZE);
	forge_mvar_get_text(reader, header->author, FORGE_MVAR_AUTHOR_SIZE);
	forge_mvar_get_text(reader, header->scenario, FORGE_MVAR_SCENARIO_SIZE);
	header->created = forge_mvar_get_u32(reader);
	header->modified = forge_mvar_get_u32(reader);
	header->listed = (forge_mvar_get_u8(reader) & 1) != 0;
	forge_mvar_get_u8(reader);
	forge_mvar_get_u16(reader);
	header->next_object_id = forge_mvar_get_u32(reader);

	return !reader->failed;
}

int forge_mvar_get_table(
	struct forge_mvar_reader *reader,
	unsigned long minimum_record_size,
	unsigned long *count,
	unsigned long *record_size)
{
	*count = forge_mvar_get_u32(reader);
	*record_size = forge_mvar_get_u16(reader);
	forge_mvar_get_u16(reader);
	if (reader->failed || *record_size < minimum_record_size ||
		*count > (reader->length - reader->offset) / *record_size)
	{
		reader->failed = 1;
		*count = 0;
		return 0;
	}

	return 1;
}

int forge_mvar_get_object(
	struct forge_mvar_reader *reader,
	unsigned long record_size,
	struct forge_mvar_object *object)
{
	unsigned long start = reader->offset;
	unsigned int team;
	short index;

	object->id = forge_mvar_get_u32(reader);
	object->kind = (int)forge_mvar_get_u8(reader);
	object->type = (int)forge_mvar_get_u8(reader);
	team = forge_mvar_get_u8(reader);
	object->team = team == 0xff ? -1 : (int)team;
	object->spawn_flags = forge_mvar_get_u8(reader);
	object->group = forge_mvar_get_u32(reader);
	object->tag = forge_mvar_get_u16(reader);
	object->respawn_seconds = forge_mvar_get_u16(reader);
	object->game_types = forge_mvar_get_u16(reader);
	forge_mvar_get_u16(reader);
	for (index = 0; index < 3; index++)
		object->position[index] = forge_mvar_get_f32(reader);
	for (index = 0; index < 4; index++)
		object->rotation[index] = forge_mvar_get_f32(reader);
	object->scale = forge_mvar_get_f32(reader);
	for (index = 0; index < 3; index++)
		object->original[index] = forge_mvar_get_f32(reader);
	/* (what a later version adds) */
	if (!reader->failed && record_size > reader->offset - start)
		forge_mvar_get_bytes(reader, record_size - (reader->offset - start));

	return !reader->failed;
}

int forge_mvar_get_editor(
	struct forge_mvar_reader *reader,
	unsigned long record_size,
	struct forge_mvar_editor *editor)
{
	unsigned long start = reader->offset;

	editor->id = forge_mvar_get_u32(reader);
	editor->group = forge_mvar_get_u16(reader);
	editor->flags = forge_mvar_get_u8(reader);
	forge_mvar_get_u8(reader);
	forge_mvar_get_text(reader, editor->comment, FORGE_MVAR_COMMENT_SIZE);
	if (!reader->failed && record_size > reader->offset - start)
		forge_mvar_get_bytes(reader, record_size - (reader->offset - start));

	return !reader->failed;
}
