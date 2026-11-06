/*
FORGE_MVAR.H

Forge's map variants (port/linux/game/forge_layout.c) as files: the .mvar
format's chunks, written and read a byte at a time, little-endian, never as
the structures below are laid out in memory (the builds' ABIs differ). The
format is described in docs/forge_mvar.md.

A file is a run of chunks, each a header (four letters, the size of what
follows, a version and flags) and its contents:

	_blf    "mvar", the file's length and a CRC-32 of every chunk after it
	mvhd    the variant's name, description, author, base map, times, whether
	        the map list shows it, the next free object id
	strt    the tag paths the objects name, each once
	mvob    the objects, one record of FORGE_MVAR_OBJECT_RECORD_SIZE each
	mved    the editor's notes about objects: group, comment (left out of
	        what a system link host sends its clients)
	mvst    each Forge part's settings (spawns, zones, AI, gravity, sky): its
	        name and its lines (halo_mod.h, layout_save)
	_eof    the end

A reader skips a chunk it does not know, and reads the fields of a record
it knows, skipping the rest of a longer one: later versions may add both.
Everything read is untrusted: every reader here checks the length left
before it reads, and fails from then on once one did not fit.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __FORGE_MVAR_H
#define __FORGE_MVAR_H

enum
{
	FORGE_MVAR_VERSION = 1,
	/* the most a file may be, and what a system link host sends */
	FORGE_MVAR_MAXIMUM_SIZE = 262144,
	FORGE_MVAR_CHUNK_HEADER_SIZE = 12,
	FORGE_MVAR_NAME_SIZE = 64,
	FORGE_MVAR_DESCRIPTION_SIZE = 128,
	FORGE_MVAR_AUTHOR_SIZE = 32,
	FORGE_MVAR_SCENARIO_SIZE = 256,
	FORGE_MVAR_MOD_NAME_SIZE = 16,
	FORGE_MVAR_COMMENT_SIZE = 64,
	FORGE_MVAR_OBJECT_RECORD_SIZE = 64,
	FORGE_MVAR_EDITOR_RECORD_SIZE = 72,
	/* a tag path in the string table, without its terminator */
	FORGE_MVAR_MAXIMUM_STRING_LENGTH = 255
};

/* what an object record is */
enum
{
	/* made by the tools or a mod */
	_forge_mvar_object_placed = 0,
	/* the map's own, moved (found again by its tag and original) */
	_forge_mvar_object_moved,
	/* the map's own, removed */
	_forge_mvar_object_removed,
	NUMBER_OF_FORGE_MVAR_OBJECT_KINDS
};

/* an object's spawn_flags */
enum
{
	/* made when the variant loads (else only while Forge is on, to edit) */
	FORGE_MVAR_SPAWN_AT_START = 0x01,
	/* not made again once gone (kept for respawning, not used yet) */
	FORGE_MVAR_SPAWN_NEVER_RESPAWN = 0x02
};

/* an object's editor flags (mved) */
enum
{
	FORGE_MVAR_EDITOR_LOCKED = 0x01
};

/* the variant: mvhd */
struct forge_mvar_header
{
	char name[FORGE_MVAR_NAME_SIZE];
	char description[FORGE_MVAR_DESCRIPTION_SIZE];
	char author[FORGE_MVAR_AUTHOR_SIZE];
	/* the base map's scenario ("levels\test\bloodgulch\bloodgulch") */
	char scenario[FORGE_MVAR_SCENARIO_SIZE];
	/* seconds since 1970, UTC */
	unsigned long created;
	unsigned long modified;
	int listed;
	unsigned long next_object_id;
};

/* an object: mvob */
struct forge_mvar_object
{
	unsigned long id;
	int kind;
	/* the object type of its tag (objects.h, _object_type_*) */
	int type;
	/* NONE (-1) none, else 0 to 15 */
	int team;
	unsigned int spawn_flags;
	/* the tag's group, four letters as a number ('scen'), and its path's
	index in the string table */
	unsigned long group;
	unsigned int tag;
	/* 0 the game type's */
	unsigned int respawn_seconds;
	/* a bit for each game type (game_engine.h, 1 << game_engine_ctf...),
	0 every one */
	unsigned int game_types;
	float position[3];
	/* a unit quaternion: x, y, z, w */
	float rotation[4];
	/* 0 the tag's own */
	float scale;
	/* moved and removed: where the map puts it */
	float original[3];
};

/* the editor's notes about an object: mved */
struct forge_mvar_editor
{
	unsigned long id;
	/* 0 none */
	unsigned int group;
	unsigned int flags;
	char comment[FORGE_MVAR_COMMENT_SIZE];
};

struct forge_mvar_writer
{
	unsigned char *data;
	unsigned long size;
	unsigned long length;
	/* where the chunk being written starts */
	unsigned long chunk_start;
	int overflowed;
};

struct forge_mvar_reader
{
	unsigned char const *data;
	unsigned long length;
	unsigned long offset;
	int failed;
};

unsigned long forge_mvar_crc32(void const *data, unsigned long length);

/* writing: a new file (its _blf), chunks of fields, and its end; finished,
the file's length, or 0 when it did not fit in size */
void forge_mvar_writer_new(struct forge_mvar_writer *writer, unsigned char *data, unsigned long size);
void forge_mvar_chunk_begin(struct forge_mvar_writer *writer, char const *tag, unsigned int version);
void forge_mvar_chunk_end(struct forge_mvar_writer *writer);
void forge_mvar_put_u8(struct forge_mvar_writer *writer, unsigned int value);
void forge_mvar_put_u16(struct forge_mvar_writer *writer, unsigned int value);
void forge_mvar_put_u32(struct forge_mvar_writer *writer, unsigned long value);
void forge_mvar_put_f32(struct forge_mvar_writer *writer, float value);
void forge_mvar_put_bytes(struct forge_mvar_writer *writer, void const *bytes, unsigned long count);
/* text in a field of size bytes, cut to fit, padded with zeros */
void forge_mvar_put_text(struct forge_mvar_writer *writer, char const *text, unsigned long size);
/* a string of the string table: its length and its characters */
void forge_mvar_put_string(struct forge_mvar_writer *writer, char const *text);
void forge_mvar_put_header(struct forge_mvar_writer *writer, struct forge_mvar_header const *header);
void forge_mvar_put_object(struct forge_mvar_writer *writer, struct forge_mvar_object const *object);
void forge_mvar_put_editor(struct forge_mvar_writer *writer, struct forge_mvar_editor const *editor);
unsigned long forge_mvar_writer_finish(struct forge_mvar_writer *writer);
/* a file whose chunks have been changed where they are (forge_mvar_open's
chunk readers point into it): its length and CRC written again */
void forge_mvar_seal(unsigned char *data, unsigned long length);

/* reading: a file whose _blf, length and CRC are right (FALSE: not a
variant, or damaged), and its chunks one at a time, each read with a
reader of its own (FALSE at _eof, or when the file is damaged: reader's
failed is then TRUE) */
int forge_mvar_open(struct forge_mvar_reader *reader, void const *data, unsigned long length);
int forge_mvar_next_chunk(struct forge_mvar_reader *reader, char tag[5], unsigned int *version,
	struct forge_mvar_reader *chunk);
unsigned int forge_mvar_get_u8(struct forge_mvar_reader *reader);
unsigned int forge_mvar_get_u16(struct forge_mvar_reader *reader);
unsigned long forge_mvar_get_u32(struct forge_mvar_reader *reader);
float forge_mvar_get_f32(struct forge_mvar_reader *reader);
/* the next count bytes, or NULL when there are fewer */
unsigned char const *forge_mvar_get_bytes(struct forge_mvar_reader *reader, unsigned long count);
/* a field of size bytes as text, always ended within result */
void forge_mvar_get_text(struct forge_mvar_reader *reader, char *result, unsigned long size);
/* a string of the string table; FALSE when it is longer than result holds */
int forge_mvar_get_string(struct forge_mvar_reader *reader, char *result, unsigned long size);
int forge_mvar_get_header(struct forge_mvar_reader *reader, struct forge_mvar_header *header);
/* a record of record_size bytes (at least the size this version writes) */
int forge_mvar_get_object(struct forge_mvar_reader *reader, unsigned long record_size, struct forge_mvar_object *object);
int forge_mvar_get_editor(struct forge_mvar_reader *reader, unsigned long record_size, struct forge_mvar_editor *editor);
/* a table's count and record size (mvob, mved): FALSE when its records
are shorter than this version's or do not all fit in what is left */
int forge_mvar_get_table(struct forge_mvar_reader *reader, unsigned long minimum_record_size, unsigned long *count,
	unsigned long *record_size);

#endif
