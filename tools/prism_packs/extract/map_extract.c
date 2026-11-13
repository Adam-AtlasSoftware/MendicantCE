/*
MAP_EXTRACT.C

A map's bitmaps, models and structure bsps written out as files other tools
read: PNG and JSON, glTF 2.0 and JMS. The map is read as the game reads it,
so that what comes out is what the game draws:
- its tag data is read (and inflated) to where the game reads it, the tag
  cache at 0x803A6000, and checked by the game's tag validator
  (port/linux/game/tag_validate.c), as tools/map_validate.c does; each
  structure bsp is read where it loads and checked the same way;
- a bitmap's pixels are read from the map at the offset the texture cache
  reads them from (texture_cache_bitmap_new, texture_cache_start_loading_bitmap),
  its Xbox texture header made as texture_cache_initialize_hardware_format
  makes it, and decoded by the port's own texture decoder
  (port/linux/src/xbox_texture_decode.h, the decoding half of
  xbox_textures.c): unswizzled, DXT decoded, converted to 32-bit BGRA; a P8
  bump map with the palette the rasterizer gives the device
  (global_vector_palette, bitmaps.c);
- a model's vertices and indices are those its parts draw: the buffers of
  the map's header (rasterizer_xbox_draw_primitives.c), their vertices
  decoded as the draw's vertex declaration reads them (positions, packed
  normals as uncompress_int32_to_real_vector3d, NORMSHORT texture
  coordinates scaled by the model's base_map_scale, node indices over 3,
  NORMSHORT node weights: rasterizer_xbox_transparent_geometry.c), its
  strips as the device draws them; its nodes' bind pose and inverse
  matrices are the tag's (models.c);
- a structure bsp's render geometry is its lightmaps' materials: each
  material's vertices and lightmap vertices from the bsp header's buffers,
  its triangles the bsp's surfaces (structure_render.c,
  rasterizer_xbox_environment.c).
Which bitmap a model or bsp uses, and as what, is found by walking the
shaders with the validator's tag schemas (tag_schema.h): every bitmap
reference with its field's name (base_map, multipurpose_map, detail_map,
bump_map, ...).

Map files are untrusted: nothing is read before the validator has checked
(and where needed corrected) the tags, and every read of the file is
bounds checked.

Built by tools/x86_game_abi.py --map-extract (map_extract_tool.c, which adds
the lights command) with the validator's objects, a 32-bit program with the
tag cache at the game's address; the fork's Linux package carries it as
bin/openh1-map-extract, PRISM's pack tools' extractor (tools/prism_packs).

usage: map_extract [--data <data root>] [--out <folder>] [--tag <text>]...
	[--only bitmaps,models,bsps] [--mips] [--scale <factor>] [--list] <map>

<map> is a map's name (b30), read from <data root>/maps/<map>.map, or a
path to a .map file. Output goes to <out>/<map>/ (default out: extract):
	index.json                     what was written, and the checks' results
	bitmaps/<tag>.json             each bitmap tag: its fields, the shaders that use it, its PNGs
	bitmaps/<tag>[_<n>][_<face>|_z<slice>][_mip<m>].png
	models/<tag>.gltf, .bin        nodes, markers, regions, permutations, LODs, skinned
	models/<tag>/<perm> <lod>.jms  JMS (8200), one per permutation and level of detail
	bsps/<tag>.gltf, .bin          the render mesh, lightmap UVs as TEXCOORD_1
With --tag, only the tags whose names contain one of the texts are written
(with the bitmaps their shaders use, and an object's model); --only limits
the kinds. --list prints the map's tags and exits. glTF is Y up: a root
node turns the game's Z up into it and scales its world units (1 = 10 feet)
by --scale (default 1).
*/

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zlib_prefixed.h"

/* ---------- the Xbox texture header (port/include/xdk/xdk_d3d8.h, xdk_pdb.h),
for the port's texture decoder */

typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned long D3DCOLOR;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define D3DFORMAT_DMACHANNEL_A 0x00000001
#define D3DFORMAT_CUBEMAP 0x00000004
#define D3DFORMAT_BORDERSOURCE_COLOR 0x00000008
#define D3DFORMAT_DIMENSION_MASK 0x000000F0
#define D3DFORMAT_DIMENSION_SHIFT 4
#define D3DFORMAT_FORMAT_MASK 0x0000FF00
#define D3DFORMAT_FORMAT_SHIFT 8
#define D3DFORMAT_MIPMAP_MASK 0x000F0000
#define D3DFORMAT_MIPMAP_SHIFT 16
#define D3DFORMAT_USIZE_MASK 0x00F00000
#define D3DFORMAT_USIZE_SHIFT 20
#define D3DFORMAT_VSIZE_MASK 0x0F000000
#define D3DFORMAT_VSIZE_SHIFT 24
#define D3DFORMAT_PSIZE_MASK 0xF0000000
#define D3DFORMAT_PSIZE_SHIFT 28
#define D3DSIZE_WIDTH_MASK 0x00000FFF
#define D3DSIZE_HEIGHT_MASK 0x00FFF000
#define D3DSIZE_HEIGHT_SHIFT 12
#define D3DSIZE_PITCH_MASK 0xFF000000
#define D3DSIZE_PITCH_SHIFT 24
#define D3DTEXTURE_CUBEFACE_ALIGNMENT 128
#define D3DTEXTURE_PITCH_ALIGNMENT 64

enum
{
	D3DFMT_L8 = 0, D3DFMT_AL8 = 1, D3DFMT_A1R5G5B5 = 2, D3DFMT_A4R4G4B4 = 4, D3DFMT_R5G6B5 = 5,
	D3DFMT_A8R8G8B8 = 6, D3DFMT_X8R8G8B8 = 7, D3DFMT_P8 = 11, D3DFMT_DXT1 = 12, D3DFMT_DXT3 = 14,
	D3DFMT_DXT5 = 15, D3DFMT_LIN_A1R5G5B5 = 16, D3DFMT_LIN_R5G6B5 = 17, D3DFMT_LIN_A8R8G8B8 = 18,
	D3DFMT_LIN_L8 = 19, D3DFMT_A8 = 25, D3DFMT_A8L8 = 26, D3DFMT_LIN_AL8 = 27, D3DFMT_LIN_A4R4G4B4 = 29,
	D3DFMT_LIN_X8R8G8B8 = 30, D3DFMT_LIN_A8 = 31, D3DFMT_LIN_A8L8 = 32, D3DFMT_V16U16 = 51,
	D3DFMT_LIN_V16U16 = 54,
};

/* (xgpu.h's, the fields the decoder reads) */
struct xgpu_texture_description
{
	DWORD format;
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;
	BOOL compressed;
	unsigned long pitch;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "map_extract_texture_decode.h"
#pragma clang diagnostic pop

/* ---------- constants */

enum
{
	/* where the game reads a map's tags, and how many bytes there are room
	for (physical_memory_map.c, physical_memory_map.h) */
	TAG_CACHE_BASE_ADDRESS = 0x803A6000,
	TAG_CACHE_SIZE = 0x1600000,

	CACHE_FILE_HEADER_SIZE = 0x800,
	CACHE_FILE_HEADER_SIGNATURE = 'head',
	CACHE_FILE_FOOTER_SIGNATURE = 'foot',
	CACHE_FILE_SECTOR_SIZE = 512,
	SCENARIO_GROUP_TAG = 'scnr',
	SCENARIO_STRUCTURE_BSPS_OFFSET = 0x5A4,
	MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO = 32,

	BITMAP_GROUP_TAG = 'bitm',
	MODEL_GROUP_TAG = 'mode',
	STRUCTURE_BSP_GROUP_TAG = 'sbsp',
	SHADER_GROUP_TAG = 'shdr',
	OBJECT_GROUP_TAG = 'obje',

	NONE = -1,
	NUMBER_OF_DETAIL_LEVELS = 5,
	MAXIMUM_NODES_PER_MODEL = 64,
	NUMBER_OF_BITMAP_FORMATS = 18,

	/* bitmap_data's flags (xbox_texture_cache.c) */
	_bitmap_has_power_of_two_dimensions_bit = 0,
	_bitmap_compressed_bit,
	_bitmap_palettized_bit,
	_bitmap_swizzled_bit,
	_bitmap_linear_bit,
	_bitmap_v16u16_bit,
	_bitmap_allocated_bit,
	_bitmap_cached_bit,

	_bitmap_type_2d = 0,
	_bitmap_type_3d,
	_bitmap_type_cube_map,

	/* rasterizer_geometry.h */
	_rasterizer_vertex_type_environment_uncompressed = 0,
	_rasterizer_vertex_type_environment_compressed,
	_rasterizer_vertex_type_environment_lightmap_uncompressed,
	_rasterizer_vertex_type_environment_lightmap_compressed,
	_rasterizer_vertex_type_model_uncompressed,
	_rasterizer_vertex_type_model_compressed,
	_triangle_buffer_type_triangles = 0,
	_triangle_buffer_type_precompiled_strip,
};

/* ---------- structures (the game's, each checked against its size) */

struct cache_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	unsigned long reserved0C;
	long tag_data_offset;
	long tag_data_size;
	unsigned char reserved18[8];
	char name[0x20];
	char build[0x20];
	unsigned char reserved60[0x79C];
	unsigned long footer_signature;
};

struct tag_instance
{
	unsigned long group_tag;
	unsigned long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

/* cache_files.c's struct cache_file_tag_header (tag_validate.c) */
struct tag_header
{
	struct tag_instance *instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	unsigned char *vertex_buffers;
	long index_buffer_count;
	unsigned char *index_buffers;
	unsigned long signature;
};

/* cache_files.c's struct cache_file_structure_bsp_header: its index buffers
are its lightmap vertex buffers */
struct structure_bsp_header
{
	void *base_address;
	long vertex_buffer_count;
	unsigned char *vertex_buffers;
	long index_buffer_count;
	unsigned char *index_buffers;
	unsigned long signature;
};

struct tag_block
{
	long count;
	void *address;
	void *definition;
};

struct tag_reference
{
	unsigned long group_tag;
	char *name;
	long name_length;
	long index;
};

struct tag_data
{
	long size;
	unsigned long pad;
	long file_offset;
	void *address;
	void *definition;
};

struct scenario_structure_bsp_reference
{
	long file_offset;
	long file_size;
	void *base_address;
	unsigned char unused[4];
	struct tag_reference structure_bsp;
};

/* a Direct3D resource of the map's header (a vertex or index buffer): its
Data is the address of its bytes in the tag cache */
struct d3d_buffer
{
	unsigned long common;
	unsigned char *data;
	unsigned long lock;
};

struct real_point3d { float x, y, z; };
struct real_quaternion { float i, j, k, w; };
struct real_matrix4x3
{
	float scale;
	float forward[3], left[3], up[3], position[3];
};

/* bitmap_group.h */
struct bitmap_data
{
	unsigned long signature;
	short width, height, depth, type, format;
	unsigned short flags;
	short registration_point[2];
	short mipmap_count;
	short mipmap_pad;
	long pixels_offset;
	long pixels_size;
	long tag_index;
	long cache_block_index;
	void *hardware_format;
	void *base_address;
};

struct bitmap_group
{
	short type, format, usage;
	unsigned short flags;
	float detail_fade, sharpen_amount, bump_height;
	short sprite_budget_size;
	unsigned short sprite_budget_count;
	short import_width, import_height;
	struct tag_data import_bitmap;
	struct tag_data pixel_data;
	float smoothing_filter_size, alpha_bias;
	short mipmap_count, sprite_usage, sprite_spacing;
	unsigned short unused;
	struct tag_block sequences;
	struct tag_block bitmaps;
};

/* model_definitions.h, tag_schema_models.c */
struct model_marker_instance
{
	unsigned char region_index, permutation_index, node_index, pad;
	struct real_point3d translation;
	struct real_quaternion rotation;
};

struct model_marker
{
	char name[32];
	long unused[5];
	struct tag_block instances;
};

struct model_node
{
	char name[32];
	short next_sibling_node_index, first_child_node_index, parent_node_index;
	unsigned short pad;
	struct real_point3d default_translation;
	struct real_quaternion default_rotation;
	float node_distance_from_parent;
	long unused[8];
	struct real_matrix4x3 runtime_default_inverse_matrix;
};

struct model_region_permutation
{
	char name[32];
	long flags;
	short variant_number;
	unsigned short pad;
	long unused[6];
	short geometry_indices[NUMBER_OF_DETAIL_LEVELS];
	unsigned short pad2;
	struct tag_block markers;
};

struct model_region
{
	char name[32];
	long unused[8];
	struct tag_block permutations;
};

struct model
{
	unsigned long flags;
	unsigned long node_list_checksum;
	float detail_cutoff_pixels[NUMBER_OF_DETAIL_LEVELS];
	short node_counts[NUMBER_OF_DETAIL_LEVELS];
	unsigned short pad;
	long unused1[2];
	float base_map_scale[2];
	long unused2[29];
	struct tag_block markers;
	struct tag_block nodes;
	struct tag_block regions;
	struct tag_block geometries;
	struct tag_block shaders;
};

struct model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	unsigned short pad;
	long unused[3];
};

struct triangle_buffer
{
	short type;
	unsigned short pad;
	long count;
	void *base_address;
	struct d3d_buffer *hardware_format;
};

struct vertex_buffer
{
	short type;
	unsigned short pad;
	long count;
	long offset;
	void *base_address;
	struct d3d_buffer *hardware_format;
};

struct model_geometry
{
	unsigned char reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index, next_part_index;
	short centroid_primary_node_index, centroid_secondary_node_index;
	float centroid_primary_node_weight, centroid_secondary_node_weight;
	struct real_point3d centroid;
	struct tag_block uncompressed_vertices, compressed_vertices, triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

/* rasterizer_model_types.h, rasterizer_geometry.c */
struct model_vertex_uncompressed
{
	float position[3], normal[3], binormal[3], tangent[3], texcoord[2];
	short nodes[2];
	float node_weights[2];
};

struct model_vertex_compressed
{
	float position[3];
	unsigned long normal, binormal, tangent;
	short texcoord[2];
	unsigned char nodes[2];
	short node_weight;
};

struct environment_vertex_uncompressed
{
	float position[3], normal[3], binormal[3], tangent[3], texcoord[2];
};

struct environment_vertex_compressed
{
	float position[3];
	unsigned long normal, binormal, tangent;
	float texcoord[2];
};

struct environment_lightmap_vertex_uncompressed
{
	float incident_radiosity[3];
	float texcoord[2];
};

struct environment_lightmap_vertex_compressed
{
	unsigned long incident_radiosity;
	short lightmap_u, lightmap_v;
};

/* structure_bsp_definitions.h */
struct structure_material
{
	struct tag_reference shader;
	short permutation_index;
	unsigned short flags;
	long first_surface_index;
	long surface_count;
	struct real_point3d centroid;
	unsigned char lighting[0x74];
	float plane[4];
	short breakable_surface_index;
	unsigned short pad;
	struct vertex_buffer vertices;
	struct vertex_buffer lightmap_vertices;
	struct tag_data uncompressed_vertex_data;
	struct tag_data compressed_vertex_data;
};

struct structure_lightmap
{
	short bitmap_index;
	unsigned short pad;
	long unused[4];
	struct tag_block materials;
};

struct structure_surface
{
	unsigned short vertex_indices[3];
};

/* the validator's schemas (tag_schema.h) */
struct tag_schema_field
{
	short type, size, count, flags;
	long offset, maximum, target_level, target_offset;
	void const *definition;
	void *check;
	char const *name;
};

struct tag_schema_definition
{
	char const *name;
	long size;
	struct tag_schema_field const *fields;
};

struct tag_schema_group
{
	unsigned long group_tag;
	unsigned long parent_group_tags[2];
	struct tag_schema_definition const *definition;
};

enum
{
	_tag_schema_block,
	_tag_schema_data,
	_tag_schema_file_data,
	_tag_schema_reference,
	_tag_schema_tag_index,
	_tag_schema_block_index,
	_tag_schema_enum,
	_tag_schema_string,
	_tag_schema_reset,
	_tag_schema_struct,
	_tag_schema_check,
	_tag_schema_terminator,
};

#define CHECK_SIZE(type, size) typedef char verify_##type##_size[sizeof(struct type) == (size) ? 1 : -1]
CHECK_SIZE(cache_file_header, CACHE_FILE_HEADER_SIZE);
CHECK_SIZE(tag_instance, 0x20);
CHECK_SIZE(tag_header, 0x24);
CHECK_SIZE(structure_bsp_header, 0x18);
CHECK_SIZE(tag_data, 0x14);
CHECK_SIZE(scenario_structure_bsp_reference, 0x20);
CHECK_SIZE(bitmap_data, 0x30);
CHECK_SIZE(bitmap_group, 0x6C);
CHECK_SIZE(model_marker_instance, 0x20);
CHECK_SIZE(model_marker, 0x40);
CHECK_SIZE(model_node, 0x9C);
CHECK_SIZE(model_region_permutation, 0x58);
CHECK_SIZE(model_region, 0x4C);
CHECK_SIZE(model, 0xE8);
CHECK_SIZE(model_shader_reference, 0x20);
CHECK_SIZE(model_geometry, 0x30);
CHECK_SIZE(model_geometry_part, 0x68);
CHECK_SIZE(model_vertex_uncompressed, 68);
CHECK_SIZE(model_vertex_compressed, 32);
CHECK_SIZE(environment_vertex_uncompressed, 56);
CHECK_SIZE(environment_vertex_compressed, 32);
CHECK_SIZE(environment_lightmap_vertex_uncompressed, 20);
CHECK_SIZE(environment_lightmap_vertex_compressed, 8);
CHECK_SIZE(structure_material, 0x100);
CHECK_SIZE(structure_lightmap, 0x20);
CHECK_SIZE(tag_schema_field, 36);

/* ---------- the validator's (port/linux/game/tag_validate.c, tag_schema_*.c) */

unsigned char tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name);
unsigned char tag_validate_structure_bsp(long tag_index, void *base, long size);
long tag_validate_corrections(void);
extern struct tag_schema_group const *const tag_schema_group_lists[];

static int verbose;
static long validator_messages;

void tag_validate_report(char const *message)
{
	validator_messages++;
	if (verbose)
		fprintf(stderr, "validator: %s\n", message);
}

/* (the game's printf family and string functions, which the validator calls
by these names: port/linux/include/stdio.h, cseries.h) */
int halo_linux_vsnprintf(char *buffer, size_t count, char const *format, va_list arguments)
{
	return vsnprintf(buffer, count, format, arguments);
}

int halo_linux_snprintf(char *buffer, size_t count, char const *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

void *csmemset(void *buffer, long c, unsigned long size) { return memset(buffer, (int)c, size); }
void *csmemcpy(void *destination, void const *source, unsigned long size) { return memcpy(destination, source, size); }
unsigned long csstrlen(char const *s) { return strlen(s); }
char *csstrcat(char *s1, char const *s2) { return strcat(s1, s2); }
long csstrcmp(char const *s1, char const *s2) { return strcmp(s1, s2); }

/* ---------- globals */

/* the palette of a P8 bump map (bitmaps.c's global_vector_palette, which
rasterizer_xbox.c gives the device) */
static unsigned long const global_vector_palette[256] =
{
#include "map_extract_vector_palette.h"
};

static struct
{
	struct cache_file_header header;
	unsigned char *bytes; /* the map as the game sees it: inflated after its header */
	unsigned char *tag_cache;
	struct tag_header *tags;
	char const *name;
	char out[1024];
	int mips;
	float scale;
	long corrections_tags;
	long loaded_bsp; /* the tag index of the bsp in the tag cache, or NONE */
	/* what is written */
	unsigned char *selected; /* by tag index: 1 asked for, 2 a dependency */
	int want_bitmaps, want_models, want_bsps;
	/* each bitmap's uses: "<group>.<field>" by "<user>" */
	struct bitmap_use { long bitmap; long user; char field[96]; } *uses;
	long use_count, use_capacity;
	FILE *index;
	int index_first;
	long errors;
} globals;

/* ---------- helpers */

static void fail(char const *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fprintf(stderr, "map_extract: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
	exit(1);
}

static void warn(char const *format, ...)
{
	va_list arguments;

	globals.errors++;
	va_start(arguments, format);
	fprintf(stderr, "map_extract: warning: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

static void *allocate(size_t size)
{
	void *p = calloc(1, size ? size : 1);

	if (!p)
		fail("out of memory");
	return p;
}

static char const *group_string(unsigned long group, char string[5])
{
	int i;

	for (i = 0; i < 4; i++)
	{
		unsigned char c = (unsigned char)(group >> (24 - 8 * i));

		string[i] = c >= 32 && c < 127 ? (char)c : '?';
	}
	string[4] = 0;
	return string;
}

static struct tag_instance *instance_get(long tag_index)
{
	long index = (short)(tag_index & 0xFFFF);

	if (tag_index == NONE || index < 0 || index >= globals.tags->tag_count ||
		globals.tags->instances[index].tag_index != tag_index)
	{
		return NULL;
	}
	return &globals.tags->instances[index];
}

static char const *tag_name(long tag_index)
{
	struct tag_instance *instance = instance_get(tag_index);

	return instance && instance->name ? instance->name : "";
}

static long instance_tag_index(struct tag_instance const *instance)
{
	return instance->tag_index;
}

static int tag_is(struct tag_instance const *instance, unsigned long group)
{
	return instance->group_tag == group || instance->parent_group_tags[0] == group ||
		instance->parent_group_tags[1] == group;
}

/* a tag's name as a relative path: backslashes made slashes, no "..", no
leading slash */
static void tag_path(long tag_index, char *path, size_t size)
{
	char const *name = tag_name(tag_index);
	size_t length = 0;
	char const *s;

	for (s = name; *s && length + 1 < size; s++)
	{
		char c = *s == '\\' ? '/' : *s;

		if (c == '/' && (length == 0 || path[length - 1] == '/'))
			continue;
		if ((unsigned char)c < 32 || c == ':')
			c = '_';
		path[length++] = c;
	}
	path[length] = 0;
	while (strstr(path, ".."))
		memcpy(strstr(path, ".."), "__", 2);
	if (!length)
		snprintf(path, size, "tag_%08lx", (unsigned long)tag_index);
}

static void make_directories(char const *path)
{
	char buffer[2048];
	char *p;

	snprintf(buffer, sizeof(buffer), "%s", path);
	for (p = buffer + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = 0;
			if (mkdir(buffer, 0755) && errno != EEXIST)
				fail("cannot make %s: %s", buffer, strerror(errno));
			*p = '/';
		}
	}
}

static FILE *open_output(char const *path, char const *mode)
{
	FILE *file;

	make_directories(path);
	file = fopen(path, mode);
	if (!file)
		fail("cannot write %s: %s", path, strerror(errno));
	return file;
}

/* a path relative to the output folder of `from` (a file in it) to `to` */
static void relative_path(char const *from, char const *to, char *result, size_t size)
{
	size_t length = 0;
	char const *s;

	result[0] = 0;
	for (s = from; *s; s++)
	{
		if (*s == '/' && length + 4 < size)
		{
			memcpy(result + length, "../", 3);
			length += 3;
			result[length] = 0;
		}
	}
	snprintf(result + length, size - length, "%s", to);
}

static void json_string(FILE *file, char const *s)
{
	fputc('"', file);
	for (; s && *s; s++)
	{
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			fprintf(file, "\\%c", c);
		else if (c < 32)
			fprintf(file, "\\u%04x", c);
		else if (c >= 128)
			fprintf(file, "\\u%04x", c); /* (the maps' names are ASCII; Latin-1 otherwise) */
		else
			fputc(c, file);
	}
	fputc('"', file);
}

/* a float that JSON can hold */
static void json_float(FILE *file, double value)
{
	if (!isfinite(value))
		value = 0;
	fprintf(file, "%.9g", value);
}

/* (a string of a name, terminated within its field) */
static void field_string(char *result, char const *field, size_t field_size)
{
	size_t length = strnlen(field, field_size);

	memcpy(result, field, length);
	result[length] = 0;
}

/* the bytes of a tag cache address range, if they are in the tag cache */
static void *cache_bytes(void const *address, unsigned long size)
{
	unsigned long offset = (unsigned long)address - TAG_CACHE_BASE_ADDRESS;

	if ((unsigned long)address < TAG_CACHE_BASE_ADDRESS || offset > TAG_CACHE_SIZE || size > TAG_CACHE_SIZE - offset)
		return NULL;
	return (void *)address;
}

/* ---------- the map */

/* the map's bytes as the game sees them (its tag data and bsps at their file
offsets): the file, inflated after its header if it is compressed
(tools/map_validate.c's map_read) */
static unsigned char *map_read(char const *path, struct cache_file_header *header)
{
	FILE *file = fopen(path, "rb");
	unsigned char *bytes = NULL;
	unsigned char *compressed = NULL;
	long size;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < CACHE_FILE_HEADER_SIZE || fseek(file, 0, SEEK_SET) ||
		fread(header, CACHE_FILE_HEADER_SIZE, 1, file) != 1 ||
		header->header_signature != CACHE_FILE_HEADER_SIGNATURE ||
		header->footer_signature != CACHE_FILE_FOOTER_SIGNATURE ||
		header->file_length < CACHE_FILE_HEADER_SIZE || header->file_length > 0x40000000)
	{
		fclose(file);
		return NULL;
	}
	bytes = malloc(header->file_length);
	compressed = malloc(size - CACHE_FILE_HEADER_SIZE);
	if (bytes && compressed && fread(compressed, size - CACHE_FILE_HEADER_SIZE, 1, file) == 1)
	{
		memcpy(bytes, header, CACHE_FILE_HEADER_SIZE);
		if (size == header->file_length)
		{
			memcpy(bytes + CACHE_FILE_HEADER_SIZE, compressed, size - CACHE_FILE_HEADER_SIZE);
		}
		else
		{
			uLongf inflated = header->file_length - CACHE_FILE_HEADER_SIZE;

			if (uncompress(bytes + CACHE_FILE_HEADER_SIZE, &inflated, compressed, size - CACHE_FILE_HEADER_SIZE) != Z_OK ||
				inflated != (uLongf)(header->file_length - CACHE_FILE_HEADER_SIZE))
			{
				free(bytes);
				bytes = NULL;
			}
		}
	}
	else
	{
		free(bytes);
		bytes = NULL;
	}
	free(compressed);
	fclose(file);

	return bytes;
}

static void map_load(char const *path)
{
	struct cache_file_header *header = &globals.header;
	struct tag_instance *scenario;

	globals.bytes = map_read(path, header);
	if (!globals.bytes)
		fail("%s is not a map this build reads (an Xbox map)", path);
	if (header->tag_data_offset < 0 || header->tag_data_size <= 0 || header->tag_data_size > TAG_CACHE_SIZE ||
		header->tag_data_offset > header->file_length - header->tag_data_size)
	{
		fail("%s: its tag data is outside it", path);
	}
	memset(globals.tag_cache, 0xCD, TAG_CACHE_SIZE);
	memcpy(globals.tag_cache, globals.bytes + header->tag_data_offset, header->tag_data_size);
	if (!tag_validate_tags(globals.tag_cache, header->tag_data_size, header->file_length, globals.name))
		fail("%s: the tag validator refuses it (run map_validate on it to see why)", path);
	globals.corrections_tags = tag_validate_corrections();
	globals.tags = (struct tag_header *)globals.tag_cache;
	globals.loaded_bsp = NONE;
	scenario = instance_get(globals.tags->scenario_tag_index);
	if (!scenario || scenario->group_tag != SCENARIO_GROUP_TAG)
		fail("%s: its tag header names no scenario", path);
}

/* the scenario's structure bsps block */
static struct tag_block *scenario_bsps(void)
{
	struct tag_instance *scenario = instance_get(globals.tags->scenario_tag_index);

	return (struct tag_block *)((unsigned char *)scenario->base_address + SCENARIO_STRUCTURE_BSPS_OFFSET);
}

/* the bsp of the scenario's index read where it loads and checked, as
scenario_structure_bsp_load and the validator have it: its header, or NULL */
static struct structure_bsp_header *bsp_load(long index)
{
	struct tag_block *bsps = scenario_bsps();
	struct scenario_structure_bsp_reference *reference;
	unsigned long load, read_size;
	long tag_data_size = globals.header.tag_data_size;

	if (index < 0 || index >= bsps->count || index >= MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO)
		return NULL;
	reference = (struct scenario_structure_bsp_reference *)bsps->address + index;
	load = (unsigned long)reference->base_address - TAG_CACHE_BASE_ADDRESS;
	read_size = ((unsigned long)reference->file_size + CACHE_FILE_SECTOR_SIZE - 1) &
		~(unsigned long)(CACHE_FILE_SECTOR_SIZE - 1);
	if (reference->file_offset < 0 || reference->file_size <= 0 ||
		reference->file_offset > globals.header.file_length - reference->file_size ||
		load < (unsigned long)tag_data_size || load > TAG_CACHE_SIZE || read_size > TAG_CACHE_SIZE - load)
	{
		warn("structure bsp %ld is outside the map", index);
		return NULL;
	}
	memset(globals.tag_cache + tag_data_size, 0xCD, TAG_CACHE_SIZE - tag_data_size);
	memcpy(reference->base_address, globals.bytes + reference->file_offset, reference->file_size);
	globals.loaded_bsp = NONE;
	if (!tag_validate_structure_bsp(reference->structure_bsp.index, reference->base_address, reference->file_size))
	{
		warn("structure bsp %ld (%s) is refused by the validator", index, tag_name(reference->structure_bsp.index));
		return NULL;
	}
	globals.loaded_bsp = reference->structure_bsp.index;
	return (struct structure_bsp_header *)reference->base_address;
}

/* ---------- the schemas */

static struct tag_schema_group const *schema_group(unsigned long group_tag)
{
	long list;

	for (list = 0; tag_schema_group_lists[list]; list++)
	{
		struct tag_schema_group const *group;

		for (group = tag_schema_group_lists[list]; group->group_tag; group++)
		{
			if (group->group_tag == group_tag)
				return group;
		}
	}
	return NULL;
}

/* a field of a definition by name */
static struct tag_schema_field const *schema_field(struct tag_schema_definition const *definition, char const *name)
{
	struct tag_schema_field const *field;

	if (!definition)
		return NULL;
	for (field = definition->fields; field->type != _tag_schema_terminator; field++)
	{
		if (field->name && !strcmp(field->name, name))
			return field;
	}
	return NULL;
}

typedef void (*schema_reference_proc)(void *context, char const *field, unsigned long group, long tag_index);

/* every tag reference and tag index in an element of a definition, with the
path of its field */
static void schema_walk(struct tag_schema_definition const *definition, unsigned char *base, char *path,
	size_t path_length, int depth, schema_reference_proc proc, void *context)
{
	struct tag_schema_field const *field;

	if (!definition || !base || depth > 16)
		return;
	for (field = definition->fields; field->type != _tag_schema_terminator; field++)
	{
		short element;

		for (element = 0; element < (field->count > 0 ? field->count : 1); element++)
		{
			unsigned char *p = base + field->offset + element * field->size;
			size_t length = path_length;

			if (path_length < 400)
			{
				length += (size_t)snprintf(path + path_length, 512 - path_length, "%s%s", path_length ? "." : "",
					field->name ? field->name : "?");
				if (field->count > 1)
					length += (size_t)snprintf(path + length, 512 - length, "[%d]", element);
			}
			switch (field->type)
			{
			case _tag_schema_reference:
			{
				struct tag_reference *reference = (struct tag_reference *)p;

				if (reference->index != NONE && instance_get(reference->index))
					proc(context, path, instance_get(reference->index)->group_tag, reference->index);
				break;
			}
			case _tag_schema_tag_index:
			{
				long tag_index = *(long *)p;

				if (tag_index != NONE && instance_get(tag_index))
					proc(context, path, instance_get(tag_index)->group_tag, tag_index);
				break;
			}
			case _tag_schema_block:
			{
				struct tag_block *block = (struct tag_block *)p;
				struct tag_schema_definition const *element_definition = field->definition;
				long index;

				if (!element_definition || block->count <= 0 ||
					!cache_bytes(block->address, (unsigned long)block->count * element_definition->size))
				{
					break;
				}
				for (index = 0; index < block->count; index++)
				{
					size_t element_length = length;

					if (length < 400)
						element_length += (size_t)snprintf(path + length, 512 - length, "[%ld]", index);
					schema_walk(element_definition, (unsigned char *)block->address + index * element_definition->size,
						path, element_length, depth + 1, proc, context);
				}
				break;
			}
			case _tag_schema_struct:
				schema_walk(field->definition, p, path, length, depth + 1, proc, context);
				break;
			default:
				break;
			}
			path[path_length] = 0;
		}
	}
}

static void tag_walk(long tag_index, void *base, schema_reference_proc proc, void *context)
{
	struct tag_instance *instance = instance_get(tag_index);
	struct tag_schema_group const *group;
	char path[512];

	if (!instance)
		return;
	group = schema_group(instance->group_tag);
	if (!group || !group->definition)
		return;
	path[0] = 0;
	schema_walk(group->definition, base ? base : instance->base_address, path, 0, 0, proc, context);
}

/* (field paths without their block indices: "maps[2].map" is "maps.map") */
static void field_without_indices(char const *field, char *result, size_t size)
{
	size_t length = 0;
	int skipping = 0;

	for (; *field && length + 1 < size; field++)
	{
		if (*field == '[')
			skipping = 1;
		else if (*field == ']')
			skipping = 0;
		else if (!skipping)
			result[length++] = *field;
	}
	result[length] = 0;
}

static void use_add(long bitmap, long user, char const *field)
{
	char group[5];
	long index;

	for (index = 0; index < globals.use_count; index++)
	{
		if (globals.uses[index].bitmap == bitmap && globals.uses[index].user == user &&
			!strncmp(globals.uses[index].field + 5, field, sizeof(globals.uses[index].field) - 5))
		{
			return;
		}
	}
	if (globals.use_count == globals.use_capacity)
	{
		globals.use_capacity = globals.use_capacity ? globals.use_capacity * 2 : 1024;
		globals.uses = realloc(globals.uses, globals.use_capacity * sizeof(*globals.uses));
		if (!globals.uses)
			fail("out of memory");
	}
	globals.uses[globals.use_count].bitmap = bitmap;
	globals.uses[globals.use_count].user = user;
	snprintf(globals.uses[globals.use_count].field, sizeof(globals.uses[0].field), "%s.%s",
		group_string(instance_get(user)->group_tag, group), field);
	globals.use_count++;
}

struct uses_context
{
	long user;
};

static void uses_proc(void *data, char const *field, unsigned long group, long tag_index)
{
	struct uses_context *context = data;
	char plain[96];

	/* (a bitmap's own tag index, in each of its bitmaps, is no use) */
	if (group != BITMAP_GROUP_TAG || tag_index == context->user)
		return;
	field_without_indices(field, plain, sizeof(plain));
	use_add(tag_index, context->user, plain);
}

/* every bitmap reference of every tag (and each bsp's lightmaps) */
static void uses_find(void)
{
	long index;
	struct tag_block *bsps = scenario_bsps();

	for (index = 0; index < globals.tags->tag_count; index++)
	{
		struct tag_instance *instance = &globals.tags->instances[index];
		struct uses_context context;

		if (instance->group_tag == STRUCTURE_BSP_GROUP_TAG || !instance->base_address)
			continue;
		context.user = instance_tag_index(instance);
		tag_walk(context.user, NULL, uses_proc, &context);
	}
	for (index = 0; index < bsps->count && index < MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO; index++)
	{
		struct structure_bsp_header *header = bsp_load(index);
		struct uses_context context;

		if (!header)
			continue;
		context.user = globals.loaded_bsp;
		tag_walk(context.user, header->base_address, uses_proc, &context);
	}
}

/* what a bitmap is used as, from its uses: diffuse, normal, multipurpose,
detail, cube, lightmap, self_illumination, ... */
static char const *bitmap_role(char const *field)
{
	char const *name = strchr(field, '.') ? strchr(field, '.') + 1 : field;
	char const *last = strrchr(name, '.') ? strrchr(name, '.') + 1 : name;

	if (!strcmp(field, "sbsp.lightmap_group"))
		return "lightmap";
	if (strstr(field, "predicted_resources"))
		return "predicted_resource";
	if (!strcmp(last, "base_map") || !strcmp(last, "diffuse_map"))
		return "diffuse";
	if (strstr(last, "bump_map"))
		return "normal";
	if (!strcmp(last, "multipurpose_map"))
		return "multipurpose";
	if (strstr(last, "detail_map"))
		return "detail";
	if (strstr(last, "cube_map") || strstr(last, "reflection_map"))
		return "cube";
	if (strstr(last, "self_illumination"))
		return "self_illumination";
	if (strstr(last, "noise_map"))
		return "noise";
	return last;
}

/* ---------- PNG (a deflate of fixed Huffman codes and LZ77 matches: the
port's zlib inflates only) */

struct bit_writer
{
	unsigned char *bytes;
	size_t length, capacity;
	unsigned long bits;
	int count;
};

static void bits_put(struct bit_writer *writer, unsigned long value, int count)
{
	writer->bits |= value << writer->count;
	writer->count += count;
	while (writer->count >= 8)
	{
		if (writer->length == writer->capacity)
		{
			writer->capacity = writer->capacity ? writer->capacity * 2 : 65536;
			writer->bytes = realloc(writer->bytes, writer->capacity);
			if (!writer->bytes)
				fail("out of memory");
		}
		writer->bytes[writer->length++] = (unsigned char)writer->bits;
		writer->bits >>= 8;
		writer->count -= 8;
	}
}

static unsigned long bits_reverse(unsigned long value, int count)
{
	unsigned long result = 0;
	int i;

	for (i = 0; i < count; i++)
		result |= ((value >> i) & 1) << (count - 1 - i);
	return result;
}

/* a literal or length symbol's fixed code (RFC 1951 3.2.6) */
static void fixed_symbol(struct bit_writer *writer, int symbol)
{
	if (symbol < 144)
		bits_put(writer, bits_reverse(0x30 + symbol, 8), 8);
	else if (symbol < 256)
		bits_put(writer, bits_reverse(0x190 + symbol - 144, 9), 9);
	else if (symbol < 280)
		bits_put(writer, bits_reverse(symbol - 256, 7), 7);
	else
		bits_put(writer, bits_reverse(0xC0 + symbol - 280, 8), 8);
}

static void fixed_match(struct bit_writer *writer, int length, int distance)
{
	static short const length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
		67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static unsigned char const length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
		4, 4, 5, 5, 5, 5, 0 };
	static unsigned short const distance_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
		385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
	static unsigned char const distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
		10, 10, 11, 11, 12, 12, 13, 13 };
	int code = 28, d = 29;

	while (length_base[code] > length)
		code--;
	fixed_symbol(writer, 257 + code);
	bits_put(writer, (unsigned long)(length - length_base[code]), length_extra[code]);
	while (distance_base[d] > distance)
		d--;
	bits_put(writer, bits_reverse((unsigned long)d, 5), 5);
	bits_put(writer, (unsigned long)(distance - distance_base[d]), distance_extra[d]);
}

/* data as a zlib stream */
static unsigned char *zlib_deflate(unsigned char const *data, size_t size, size_t *result_size)
{
	enum { HASH_BITS = 15, WINDOW = 32768, CHAIN = 32 };
	struct bit_writer writer = { 0 };
	long *head = malloc(sizeof(long) << HASH_BITS);
	long *previous = malloc(sizeof(long) * (size ? size : 1));
	size_t i = 0;
	unsigned long adler = adler32(1L, data, (uInt)size);
	unsigned char *result;

	if (!head || !previous)
		fail("out of memory");
	memset(head, 0xFF, sizeof(long) << HASH_BITS);
	bits_put(&writer, 0x78, 8);
	bits_put(&writer, 0x01, 8);
	bits_put(&writer, 1, 1); /* the final block */
	bits_put(&writer, 1, 2); /* of fixed codes */
	while (i < size)
	{
		int best_length = 0, best_distance = 0;

		if (i + 3 <= size)
		{
			unsigned long hash = ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & ((1 << HASH_BITS) - 1);
			long candidate = head[hash];
			int chain = CHAIN;

			while (candidate >= 0 && (long)i - candidate <= WINDOW - 1 && chain-- > 0)
			{
				int length = 0;
				int maximum = (int)(size - i < 258 ? size - i : 258);

				while (length < maximum && data[candidate + length] == data[i + length])
					length++;
				if (length > best_length)
				{
					best_length = length;
					best_distance = (int)(i - candidate);
					if (length == maximum)
						break;
				}
				candidate = previous[candidate];
			}
			previous[i] = head[hash];
			head[hash] = (long)i;
		}
		if (best_length >= 3)
		{
			size_t end = i + best_length;

			fixed_match(&writer, best_length, best_distance);
			for (i++; i < end; i++)
			{
				if (i + 3 <= size)
				{
					unsigned long hash = ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & ((1 << HASH_BITS) - 1);

					previous[i] = head[hash];
					head[hash] = (long)i;
				}
			}
		}
		else
		{
			fixed_symbol(&writer, data[i]);
			i++;
		}
	}
	fixed_symbol(&writer, 256);
	if (writer.count)
		bits_put(&writer, 0, 8 - writer.count);
	bits_put(&writer, (adler >> 24) & 0xFF, 8);
	bits_put(&writer, (adler >> 16) & 0xFF, 8);
	bits_put(&writer, (adler >> 8) & 0xFF, 8);
	bits_put(&writer, adler & 0xFF, 8);
	free(head);
	free(previous);
	result = writer.bytes;
	*result_size = writer.length;
	return result;
}

static void png_chunk(FILE *file, char const *type, unsigned char const *data, size_t size)
{
	unsigned char word[4];
	unsigned long crc;

	word[0] = (unsigned char)(size >> 24);
	word[1] = (unsigned char)(size >> 16);
	word[2] = (unsigned char)(size >> 8);
	word[3] = (unsigned char)size;
	fwrite(word, 4, 1, file);
	fwrite(type, 4, 1, file);
	if (size)
		fwrite(data, size, 1, file);
	crc = crc32(0L, (unsigned char const *)type, 4);
	if (size)
		crc = crc32(crc, data, (uInt)size);
	word[0] = (unsigned char)(crc >> 24);
	word[1] = (unsigned char)(crc >> 16);
	word[2] = (unsigned char)(crc >> 8);
	word[3] = (unsigned char)crc;
	fwrite(word, 4, 1, file);
}

/* ARGB words as an RGBA PNG */
static void png_write(char const *path, unsigned long const *pixels, unsigned long width, unsigned long height)
{
	static unsigned char const signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	size_t row_size = width * 4 + 1;
	unsigned char *raw = malloc(row_size * height);
	unsigned char header[13];
	unsigned char *compressed;
	size_t compressed_size;
	unsigned long x, y;
	FILE *file;

	if (!raw)
		fail("out of memory");
	for (y = 0; y < height; y++)
	{
		unsigned char *row = raw + y * row_size;

		row[0] = 1; /* the Sub filter */
		for (x = 0; x < width; x++)
		{
			unsigned long pixel = pixels[y * width + x];
			unsigned char rgba[4];
			int c;

			rgba[0] = (unsigned char)(pixel >> 16);
			rgba[1] = (unsigned char)(pixel >> 8);
			rgba[2] = (unsigned char)pixel;
			rgba[3] = (unsigned char)(pixel >> 24);
			for (c = 0; c < 4; c++)
			{
				unsigned char left = 0;

				if (x)
				{
					unsigned long previous = pixels[y * width + x - 1];

					left = (unsigned char)(c == 0 ? previous >> 16 : c == 1 ? previous >> 8 : c == 2 ? previous :
						previous >> 24);
				}
				row[1 + x * 4 + c] = (unsigned char)(rgba[c] - left);
			}
		}
	}
	compressed = zlib_deflate(raw, row_size * height, &compressed_size);
	header[0] = (unsigned char)(width >> 24);
	header[1] = (unsigned char)(width >> 16);
	header[2] = (unsigned char)(width >> 8);
	header[3] = (unsigned char)width;
	header[4] = (unsigned char)(height >> 24);
	header[5] = (unsigned char)(height >> 16);
	header[6] = (unsigned char)(height >> 8);
	header[7] = (unsigned char)height;
	header[8] = 8;  /* bits per channel */
	header[9] = 6;  /* RGBA */
	header[10] = 0;
	header[11] = 0;
	header[12] = 0;
	file = open_output(path, "wb");
	fwrite(signature, 8, 1, file);
	png_chunk(file, "IHDR", header, 13);
	png_chunk(file, "IDAT", compressed, compressed_size);
	png_chunk(file, "IEND", NULL, 0);
	fclose(file);
	free(raw);
	free(compressed);
}

/* ---------- bitmaps */

static char const *const bitmap_format_names[NUMBER_OF_BITMAP_FORMATS] =
{
	"a8", "y8", "ay8", "a8y8", "unused1", "unused2", "r5g6b5", "unused3", "a1r5g5b5", "a4r4g4b4",
	"x8r8g8b8", "a8r8g8b8", "unused4", "unused5", "dxt1", "dxt3", "dxt5", "p8_bump",
};
static unsigned char const bitmap_format_bits_per_pixel[NUMBER_OF_BITMAP_FORMATS] =
{
	8, 8, 8, 16, 0, 0, 16, 0, 16, 16, 32, 32, 0, 0, 4, 8, 8, 8,
};
/* xbox_texture_cache.c's bitmap_d3d_format_tables */
static long const bitmap_d3d_formats[2][NUMBER_OF_BITMAP_FORMATS] =
{
	{ D3DFMT_A8, D3DFMT_L8, D3DFMT_AL8, D3DFMT_A8L8, NONE, NONE, D3DFMT_R5G6B5, NONE, D3DFMT_A1R5G5B5,
		D3DFMT_A4R4G4B4, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, NONE, NONE, D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5,
		D3DFMT_P8 },
	{ D3DFMT_LIN_A8, D3DFMT_LIN_L8, D3DFMT_LIN_AL8, D3DFMT_LIN_A8L8, NONE, NONE, D3DFMT_LIN_R5G6B5, NONE,
		D3DFMT_LIN_A1R5G5B5, D3DFMT_LIN_A4R4G4B4, D3DFMT_LIN_X8R8G8B8, D3DFMT_LIN_A8R8G8B8, NONE, NONE, NONE, NONE,
		NONE, NONE },
};
static char const *const bitmap_type_names[] = { "2d", "3d", "cube_map" };
static char const *const bitmap_group_type_names[] =
	{ "2d_textures", "3d_textures", "cube_maps", "sprites", "interface_bitmaps" };
static char const *const bitmap_group_format_names[] =
	{ "compressed_with_color_key_transparency", "compressed_with_explicit_alpha",
		"compressed_with_interpolated_alpha", "16_bit_color", "32_bit_color", "monochrome" };
static char const *const bitmap_group_usage_names[] =
	{ "alpha_blend", "default", "height_map", "detail_map", "light_map", "vector_map" };
/* the hardware's cube faces, in the order of their bytes (D3DCUBEMAP_FACES) */
static char const *const cube_face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };

#define NAME_OF(names, index) \
	((index) >= 0 && (unsigned long)(index) < sizeof(names) / sizeof((names)[0]) ? (names)[index] : "unknown")

static int floor_log2_int(long value)
{
	int result = 0;

	while (value > 1)
	{
		value >>= 1;
		result++;
	}
	return result;
}

/* rasterizer_swizzle.c's rasterizer_xbox_bitmap_get_max_mipmap_count */
static short xbox_max_mipmap_count(struct bitmap_data const *bitmap)
{
	short count = 0;

	if ((bitmap->flags & (1 << _bitmap_has_power_of_two_dimensions_bit)) &&
		!(bitmap->flags & (1 << _bitmap_linear_bit)))
	{
		long w = bitmap->width, h = bitmap->height, d = bitmap->depth;
		long largest;

		if (bitmap->flags & (1 << _bitmap_compressed_bit))
		{
			w /= 4;
			h /= 4;
		}
		largest = w > h ? w : h;
		largest = largest > d ? largest : d;
		count = (short)floor_log2_int(largest);
		if (bitmap->mipmap_count < count)
			count = bitmap->mipmap_count;
	}
	return count;
}

/* the bitmap's Xbox texture header, as texture_cache_initialize_hardware_format
makes it, described as the decoder reads it: FALSE if the texture cache
would not load it (texture_cache_bitmap_valid) */
static int bitmap_describe(struct bitmap_data const *bitmap, struct xgpu_texture_description *description,
	unsigned long *format_word, unsigned long *size_word)
{
	int linear = (bitmap->flags & (1 << _bitmap_linear_bit)) != 0;
	long d3d_format;

	if (bitmap->format < 0 || bitmap->format >= NUMBER_OF_BITMAP_FORMATS || bitmap->type < 0 || bitmap->type > 2 ||
		bitmap->width <= 0 || bitmap->height <= 0 || bitmap->depth <= 0)
	{
		return FALSE;
	}
	d3d_format = bitmap_d3d_formats[linear][bitmap->format];
	if (d3d_format == NONE)
		return FALSE;
	if ((bitmap->flags & (1 << _bitmap_v16u16_bit)) && (bitmap->format == 10 || bitmap->format == 11))
		d3d_format = linear ? D3DFMT_LIN_V16U16 : D3DFMT_V16U16;
	if (linear)
	{
		long row_pitch = (long)bitmap->width * bitmap_format_bits_per_pixel[bitmap->format] / 8;

		*format_word = ((unsigned long)d3d_format << D3DFORMAT_FORMAT_SHIFT) | (1 << D3DFORMAT_MIPMAP_SHIFT) |
			(2 << D3DFORMAT_DIMENSION_SHIFT) | D3DFORMAT_BORDERSOURCE_COLOR | D3DFORMAT_DMACHANNEL_A;
		*size_word = ((unsigned long)((row_pitch + D3DTEXTURE_PITCH_ALIGNMENT - 1) / D3DTEXTURE_PITCH_ALIGNMENT - 1)
			<< D3DSIZE_PITCH_SHIFT) | ((unsigned long)(bitmap->height - 1) << D3DSIZE_HEIGHT_SHIFT) |
			(unsigned long)(bitmap->width - 1);
	}
	else
	{
		*format_word = ((unsigned long)floor_log2_int(bitmap->depth) << D3DFORMAT_PSIZE_SHIFT) |
			((unsigned long)floor_log2_int(bitmap->height) << D3DFORMAT_VSIZE_SHIFT) |
			((unsigned long)floor_log2_int(bitmap->width) << D3DFORMAT_USIZE_SHIFT) |
			((unsigned long)d3d_format << D3DFORMAT_FORMAT_SHIFT) |
			((bitmap->type == _bitmap_type_3d ? 3UL : 2UL) << D3DFORMAT_DIMENSION_SHIFT) |
			((unsigned long)(xbox_max_mipmap_count(bitmap) + 1) << D3DFORMAT_MIPMAP_SHIFT) |
			(bitmap->type == _bitmap_type_cube_map ? D3DFORMAT_CUBEMAP : 0) |
			D3DFORMAT_BORDERSOURCE_COLOR | D3DFORMAT_DMACHANNEL_A;
		*size_word = 0;
	}
	xbox_texture_describe(*format_word, *size_word, description);
	return TRUE;
}

/* the PNG of a bitmap of a group: <tag>[_<n>] */
static void bitmap_png_name(long tag_index, long bitmap_index, long bitmap_count, char *path, size_t size)
{
	char tag[1024];

	tag_path(tag_index, tag, sizeof(tag));
	if (bitmap_count > 1)
		snprintf(path, size, "bitmaps/%s_%ld", tag, bitmap_index);
	else
		snprintf(path, size, "bitmaps/%s", tag);
}

/* the output-relative path of the PNG of bitmap 0 (its first face, its
first level) of a bitmap tag */
static void bitmap_first_png(long tag_index, char *path, size_t size)
{
	struct tag_instance *instance = instance_get(tag_index);
	struct bitmap_group *group;
	char base[1100];

	path[0] = 0;
	if (!instance || instance->group_tag != BITMAP_GROUP_TAG)
		return;
	group = instance->base_address;
	if (group->bitmaps.count <= 0)
		return;
	bitmap_png_name(tag_index, 0, group->bitmaps.count, base, sizeof(base));
	{
		struct bitmap_data *bitmap = group->bitmaps.address;

		if (bitmap->type == _bitmap_type_cube_map)
			snprintf(path, size, "%s_%s.png", base, cube_face_names[0]);
		else if (bitmap->type == _bitmap_type_3d && bitmap->depth > 1)
			snprintf(path, size, "%s_z0.png", base);
		else
			snprintf(path, size, "%s.png", base);
	}
}

static unsigned char *bitmaps_done;

static void bitmap_export(long tag_index)
{
	struct tag_instance *instance = instance_get(tag_index);
	struct bitmap_group *group;
	char tag[1024], path[2200];
	FILE *json;
	long bitmap_index, use;

	if (!instance || instance->group_tag != BITMAP_GROUP_TAG || bitmaps_done[(short)tag_index])
		return;
	bitmaps_done[(short)tag_index] = 1;
	group = instance->base_address;
	tag_path(tag_index, tag, sizeof(tag));
	snprintf(path, sizeof(path), "%s/bitmaps/%s.json", globals.out, tag);
	json = open_output(path, "w");
	fprintf(json, "{\n  \"tag\": ");
	json_string(json, tag_name(tag_index));
	fprintf(json, ",\n  \"group\": \"bitm\",\n  \"type\": \"%s\",\n  \"format\": \"%s\",\n  \"usage\": \"%s\",\n",
		NAME_OF(bitmap_group_type_names, group->type), NAME_OF(bitmap_group_format_names, group->format),
		NAME_OF(bitmap_group_usage_names, group->usage));
	fprintf(json, "  \"flags\": %u,\n  \"detail_fade\": ", group->flags);
	json_float(json, group->detail_fade);
	fprintf(json, ",\n  \"bump_height\": ");
	json_float(json, group->bump_height);
	fprintf(json, ",\n  \"mipmap_count\": %d,\n  \"sprite_usage\": %d,\n", group->mipmap_count, group->sprite_usage);
	/* its uses */
	fprintf(json, "  \"used_as\": [");
	{
		int first = 1;

		for (use = 0; use < globals.use_count; use++)
		{
			if (globals.uses[use].bitmap != tag_index)
				continue;
			fprintf(json, "%s\n    {\"role\": \"%s\", \"field\": ", first ? "" : ",", bitmap_role(globals.uses[use].field));
			json_string(json, globals.uses[use].field);
			fprintf(json, ", \"by\": ");
			json_string(json, tag_name(globals.uses[use].user));
			fprintf(json, "}");
			first = 0;
		}
		fprintf(json, "%s],\n", first ? "" : "\n  ");
		/* (the roles, most used first, those of predicted resources not
		counted) */
		fprintf(json, "  \"roles\": [");
		{
			char const *roles[32];
			int counts[32], role_count = 0, r, printed = 0;

			for (use = 0; use < globals.use_count; use++)
			{
				char const *role;

				if (globals.uses[use].bitmap != tag_index)
					continue;
				role = bitmap_role(globals.uses[use].field);
				if (!strcmp(role, "predicted_resource"))
					continue;
				for (r = 0; r < role_count && strcmp(roles[r], role); r++)
					;
				if (r == role_count && role_count < 32)
				{
					roles[role_count] = role;
					counts[role_count++] = 0;
				}
				if (r < role_count)
					counts[r]++;
			}
			while (printed < role_count)
			{
				int best = -1;

				for (r = 0; r < role_count; r++)
				{
					if (counts[r] >= 0 && (best < 0 || counts[r] > counts[best]))
						best = r;
				}
				fprintf(json, "%s\"%s\"", printed ? ", " : "", roles[best]);
				counts[best] = -1;
				printed++;
			}
		}
		fprintf(json, "],\n");
	}
	/* its sequences */
	fprintf(json, "  \"sequences\": [");
	if (cache_bytes(group->sequences.address, (unsigned long)group->sequences.count * 0x40))
	{
		long index;

		for (index = 0; index < group->sequences.count; index++)
		{
			unsigned char *sequence = (unsigned char *)group->sequences.address + index * 0x40;
			char name[33];

			field_string(name, (char const *)sequence, 32);
			fprintf(json, "%s\n    {\"name\": ", index ? "," : "");
			json_string(json, name);
			fprintf(json, ", \"first_bitmap_index\": %d, \"bitmap_count\": %d}", *(short *)(sequence + 32),
				*(short *)(sequence + 34));
		}
		fprintf(json, "%s", group->sequences.count ? "\n  " : "");
	}
	fprintf(json, "],\n  \"bitmaps\": [");
	for (bitmap_index = 0; bitmap_index < group->bitmaps.count; bitmap_index++)
	{
		struct bitmap_data *bitmap = (struct bitmap_data *)group->bitmaps.address + bitmap_index;
		struct xgpu_texture_description description;
		unsigned long format_word = 0, size_word = 0;
		/* (where texture_cache_start_loading_bitmap reads its pixels: an Xbox
		map's bitmaps are cached already (_bitmap_cached_bit), their offsets in
		the file; texture_cache_bitmap_new, which adds the group's pixel data
		offset, is only for a bitmap not yet cached) */
		long file_offset = bitmap->pixels_offset +
			((bitmap->flags & (1 << _bitmap_cached_bit)) ? 0 : group->pixel_data.file_offset);
		unsigned long face_size = 0, faces, read_size = 0;
		char base[1100];
		int described = bitmap_describe(bitmap, &description, &format_word, &size_word);
		int written = 0;

		bitmap_png_name(tag_index, bitmap_index, group->bitmaps.count, base, sizeof(base));
		fprintf(json, "%s\n    {\n      \"index\": %ld, \"width\": %d, \"height\": %d, \"depth\": %d,\n",
			bitmap_index ? "," : "", bitmap_index, bitmap->width, bitmap->height, bitmap->depth);
		fprintf(json, "      \"type\": \"%s\", \"format\": \"%s\", \"flags\": %u,\n",
			NAME_OF(bitmap_type_names, bitmap->type), NAME_OF(bitmap_format_names, bitmap->format), bitmap->flags);
		fprintf(json, "      \"swizzled\": %s, \"linear\": %s, \"compressed\": %s, \"palettized\": %s, \"cached\": %s,"
			" \"power_of_two\": %s,\n",
			bitmap->flags & (1 << _bitmap_swizzled_bit) ? "true" : "false",
			bitmap->flags & (1 << _bitmap_linear_bit) ? "true" : "false",
			bitmap->flags & (1 << _bitmap_compressed_bit) ? "true" : "false",
			bitmap->flags & (1 << _bitmap_palettized_bit) ? "true" : "false",
			bitmap->flags & (1 << _bitmap_cached_bit) ? "true" : "false",
			bitmap->flags & (1 << _bitmap_has_power_of_two_dimensions_bit) ? "true" : "false");
		fprintf(json, "      \"registration_point\": [%d, %d], \"mipmap_count\": %d, \"hardware_levels\": %lu,\n",
			bitmap->registration_point[0], bitmap->registration_point[1], bitmap->mipmap_count,
			described ? description.levels : 0);
		fprintf(json, "      \"d3d_format\": %lu, \"format_word\": \"%08lx\", \"size_word\": \"%08lx\",\n",
			described ? description.format : 0, format_word, size_word);
		fprintf(json, "      \"pixels_file_offset\": %ld, \"pixels_offset\": %ld, \"pixel_data_file_offset\": %ld, "
			"\"pixels_size\": %ld,\n", file_offset, bitmap->pixels_offset, group->pixel_data.file_offset, bitmap->pixels_size);
		fprintf(json, "      \"png\": [");
		if (described)
		{
			face_size = xbox_texture_face_size(&description);
			faces = description.cube_map ? 6 : 1;
			read_size = face_size * faces;
		}
		if (!described)
		{
			warn("%s bitmap %ld (%dx%d format %d type %d) is not one the texture cache loads", tag_name(tag_index),
				bitmap_index, bitmap->width, bitmap->height, bitmap->format, bitmap->type);
		}
		else if (file_offset < 0 || file_offset > globals.header.file_length ||
			read_size > (unsigned long)(globals.header.file_length - file_offset))
		{
			warn("%s bitmap %ld: its pixels are outside the map", tag_name(tag_index), bitmap_index);
		}
		else
		{
			unsigned char const *pixels = globals.bytes + file_offset;
			unsigned long face, level;
			unsigned long levels = globals.mips ? description.levels : 1;

			for (face = 0; face < faces; face++)
			{
				for (level = 0; level < levels; level++)
				{
					unsigned long width = level_dimension(description.width, level);
					unsigned long height = level_dimension(description.height, level);
					unsigned long depth = level_dimension(description.depth, level);
					unsigned long *argb_pixels = malloc(width * height * depth * sizeof(unsigned long));
					unsigned char const *source = pixels + face * face_size + xbox_texture_level_offset(&description, level);
					struct format_information information = format_information(description.format);
					unsigned long slice;
					BOOL decoded;

					if (!argb_pixels)
						fail("out of memory");
					if (description.compressed)
					{
						dxt_decode_level(information.kind, source, width, height, depth, argb_pixels);
						decoded = TRUE;
					}
					else
					{
						decoded = decode_level(&description, level, source,
							information.kind == _texel_p8 ? global_vector_palette : NULL, argb_pixels);
					}
					for (slice = 0; decoded && slice < depth; slice++)
					{
						char name[1300], full[2600];
						size_t length = (size_t)snprintf(name, sizeof(name), "%s", base);

						if (faces > 1)
							length += (size_t)snprintf(name + length, sizeof(name) - length, "_%s", cube_face_names[face]);
						if (description.depth > 1)
							length += (size_t)snprintf(name + length, sizeof(name) - length, "_z%lu", slice);
						if (level)
							length += (size_t)snprintf(name + length, sizeof(name) - length, "_mip%lu", level);
						snprintf(name + length, sizeof(name) - length, ".png");
						snprintf(full, sizeof(full), "%s/%s", globals.out, name);
						png_write(full, argb_pixels + slice * width * height, width, height);
						fprintf(json, "%s\n        ", written ? "," : "");
						json_string(json, name);
						written++;
					}
					free(argb_pixels);
				}
			}
		}
		fprintf(json, "%s]\n    }", written ? "\n      " : "");
	}
	fprintf(json, "%s]\n}\n", group->bitmaps.count ? "\n  " : "");
	fclose(json);

	fprintf(globals.index, "%s\n    {\"kind\": \"bitmap\", \"tag\": ", globals.index_first ? "" : ",");
	globals.index_first = 0;
	json_string(globals.index, tag_name(tag_index));
	fprintf(globals.index, ", \"json\": \"bitmaps/");
	{
		char const *s;

		for (s = tag; *s; s++)
		{
			if (*s == '"' || *s == '\\')
				fputc('\\', globals.index);
			fputc(*s, globals.index);
		}
	}
	fprintf(globals.index, ".json\", \"bitmaps\": %ld}", group->bitmaps.count);
}

/* the bitmaps a tag (a shader) references, exported */
static void bitmaps_of_proc(void *context, char const *field, unsigned long group, long tag_index)
{
	(void)context;
	(void)field;
	if (group == BITMAP_GROUP_TAG && globals.want_bitmaps)
		bitmap_export(tag_index);
}

/* ---------- glTF */

struct gltf
{
	unsigned char *bin;
	size_t bin_length, bin_capacity;
	/* the JSON's arrays, built as text */
	char *views, *accessors, *nodes, *meshes, *materials, *textures, *images;
	size_t views_length, accessors_length, nodes_length, meshes_length, materials_length, textures_length, images_length;
	long view_count, accessor_count, node_count, mesh_count, material_count, texture_count, image_count;
};

static void text_append(char **text, size_t *length, char const *format, ...)
{
	va_list arguments;
	int added;

	va_start(arguments, format);
	added = vsnprintf(NULL, 0, format, arguments);
	va_end(arguments);
	if (added < 0)
		return;
	*text = realloc(*text, *length + added + 1);
	if (!*text)
		fail("out of memory");
	va_start(arguments, format);
	vsnprintf(*text + *length, added + 1, format, arguments);
	va_end(arguments);
	*length += added;
}

/* a JSON string's text, escaped, in a buffer */
static char const *json_escaped(char const *s, char *buffer, size_t size)
{
	size_t length = 0;

	for (; s && *s && length + 8 < size; s++)
	{
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
		{
			buffer[length++] = '\\';
			buffer[length++] = (char)c;
		}
		else if (c < 32 || c >= 128)
		{
			length += (size_t)snprintf(buffer + length, size - length, "\\u%04x", c);
		}
		else
		{
			buffer[length++] = (char)c;
		}
	}
	buffer[length] = 0;
	return buffer;
}

static size_t gltf_bin(struct gltf *gltf, void const *data, size_t size)
{
	size_t offset;

	while (gltf->bin_length % 4)
	{
		if (gltf->bin_length == gltf->bin_capacity)
			break;
		gltf->bin[gltf->bin_length++] = 0;
	}
	if (gltf->bin_length + size + 4 > gltf->bin_capacity)
	{
		gltf->bin_capacity = (gltf->bin_length + size + 4) * 2;
		gltf->bin = realloc(gltf->bin, gltf->bin_capacity);
		if (!gltf->bin)
			fail("out of memory");
		while (gltf->bin_length % 4)
			gltf->bin[gltf->bin_length++] = 0;
	}
	offset = gltf->bin_length;
	memcpy(gltf->bin + offset, data, size);
	gltf->bin_length += size;
	return offset;
}

enum { GLTF_BYTE = 5120, GLTF_UNSIGNED_BYTE = 5121, GLTF_UNSIGNED_SHORT = 5123, GLTF_UNSIGNED_INT = 5125,
	GLTF_FLOAT = 5126 };

/* an accessor of count elements of type ("VEC3") of component type, whose
data is copied to the buffer; min and max (of components components) if
given */
static long gltf_accessor(struct gltf *gltf, void const *data, long count, int component_type, char const *type,
	int components, float const *minimum, float const *maximum, int target)
{
	int component_size = component_type == GLTF_FLOAT || component_type == GLTF_UNSIGNED_INT ? 4 :
		component_type == GLTF_UNSIGNED_SHORT ? 2 : 1;
	size_t size = (size_t)count * components * component_size;
	size_t offset = gltf_bin(gltf, data, size);
	int i;

	text_append(&gltf->views, &gltf->views_length, "%s\n    {\"buffer\": 0, \"byteOffset\": %lu, \"byteLength\": %lu%s}",
		gltf->view_count ? "," : "", (unsigned long)offset, (unsigned long)size,
		target == 34963 ? ", \"target\": 34963" : target == 34962 ? ", \"target\": 34962" : "");
	text_append(&gltf->accessors, &gltf->accessors_length,
		"%s\n    {\"bufferView\": %ld, \"componentType\": %d, \"count\": %ld, \"type\": \"%s\"",
		gltf->accessor_count ? "," : "", gltf->view_count, component_type, count, type);
	if (minimum && maximum)
	{
		text_append(&gltf->accessors, &gltf->accessors_length, ", \"min\": [");
		for (i = 0; i < components; i++)
			text_append(&gltf->accessors, &gltf->accessors_length, "%s%.9g", i ? ", " : "", minimum[i]);
		text_append(&gltf->accessors, &gltf->accessors_length, "], \"max\": [");
		for (i = 0; i < components; i++)
			text_append(&gltf->accessors, &gltf->accessors_length, "%s%.9g", i ? ", " : "", maximum[i]);
		text_append(&gltf->accessors, &gltf->accessors_length, "]");
	}
	text_append(&gltf->accessors, &gltf->accessors_length, "}");
	gltf->view_count++;
	return gltf->accessor_count++;
}

/* an image and a texture of a PNG (output-relative path), from the glTF
at gltf_path: the texture's index */
static long gltf_texture(struct gltf *gltf, char const *gltf_path, char const *png)
{
	char relative[2400], escaped[4800];

	relative_path(gltf_path, png, relative, sizeof(relative));
	text_append(&gltf->images, &gltf->images_length, "%s\n    {\"uri\": \"%s\"}", gltf->image_count ? "," : "",
		json_escaped(relative, escaped, sizeof(escaped)));
	text_append(&gltf->textures, &gltf->textures_length, "%s\n    {\"source\": %ld, \"sampler\": 0}",
		gltf->texture_count ? "," : "", gltf->image_count);
	gltf->image_count++;
	return gltf->texture_count++;
}

struct material_maps_context
{
	char text[4096];
	size_t length;
	long base_map;
	long bump_map;
	int first;
};

static void material_maps_proc(void *data, char const *field, unsigned long group, long tag_index)
{
	struct material_maps_context *context = data;
	char escaped[600], png[1200];

	if (group != BITMAP_GROUP_TAG)
		return;
	bitmap_first_png(tag_index, png, sizeof(png));
	if (context->length < sizeof(context->text) - 1200)
	{
		char escaped_png[2400];

		context->length += (size_t)snprintf(context->text + context->length, sizeof(context->text) - context->length,
			"%s\"%s\": {\"tag\": \"%s\", \"png\": \"%s\"}", context->first ? "" : ", ",
			json_escaped(field, escaped, sizeof(escaped)),
			json_escaped(tag_name(tag_index), escaped + 300, sizeof(escaped) - 300),
			json_escaped(png, escaped_png, sizeof(escaped_png)));
		context->first = 0;
	}
	if (!strcmp(field, "base_map") || (!strcmp(field, "diffuse_map") && context->base_map == NONE))
		context->base_map = tag_index;
	if (!strcmp(field, "bump_map"))
		context->bump_map = tag_index;
}

/* a material of a shader: its base map as the base color, its maps (each
bitmap reference of the shader, by field) as extras; the bitmaps are
exported */
static long gltf_material(struct gltf *gltf, char const *gltf_path, long shader_index, char const *extra)
{
	struct material_maps_context context;
	char escaped[600];
	char group[5];
	struct tag_instance *instance = instance_get(shader_index);

	memset(&context, 0, sizeof(context));
	context.base_map = NONE;
	context.bump_map = NONE;
	context.first = 1;
	if (instance)
	{
		tag_walk(shader_index, NULL, bitmaps_of_proc, NULL);
		tag_walk(shader_index, NULL, material_maps_proc, &context);
	}
	text_append(&gltf->materials, &gltf->materials_length, "%s\n    {\"name\": \"%s\"",
		gltf->material_count ? "," : "", json_escaped(instance ? tag_name(shader_index) : "none", escaped,
			sizeof(escaped)));
	if (context.base_map != NONE && globals.want_bitmaps)
	{
		char png[1200];

		bitmap_first_png(context.base_map, png, sizeof(png));
		if (png[0])
		{
			long texture = gltf_texture(gltf, gltf_path, png);

			text_append(&gltf->materials, &gltf->materials_length,
				", \"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": %ld}, \"metallicFactor\": 0,"
				" \"roughnessFactor\": 1}", texture);
		}
	}
	else
	{
		text_append(&gltf->materials, &gltf->materials_length,
			", \"pbrMetallicRoughness\": {\"metallicFactor\": 0, \"roughnessFactor\": 1}");
	}
	text_append(&gltf->materials, &gltf->materials_length, ", \"extras\": {\"shader\": \"%s\", \"shader_group\": \"%s\", "
		"\"maps\": {%s}%s%s}}",
		json_escaped(instance ? tag_name(shader_index) : "", escaped, sizeof(escaped)),
		instance ? group_string(instance->group_tag, group) : "", context.text, extra ? ", " : "", extra ? extra : "");
	return gltf->material_count++;
}

static void gltf_write(struct gltf *gltf, char const *relative, char const *nodes_scene, char const *extra_top)
{
	char path[2200], bin_name[1200], escaped[2400];
	char const *slash;
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s.bin", globals.out, relative);
	file = open_output(path, "wb");
	if (gltf->bin_length)
		fwrite(gltf->bin, gltf->bin_length, 1, file);
	fclose(file);
	slash = strrchr(relative, '/');
	snprintf(bin_name, sizeof(bin_name), "%s.bin", slash ? slash + 1 : relative);
	snprintf(path, sizeof(path), "%s/%s.gltf", globals.out, relative);
	file = open_output(path, "w");
	fprintf(file, "{\n  \"asset\": {\"version\": \"2.0\", \"generator\": \"OpenCE map_extract\"},\n");
	fprintf(file, "  \"scene\": 0,\n  \"scenes\": [{\"nodes\": [%s]}],\n", nodes_scene);
	fprintf(file, "  \"nodes\": [%s\n  ],\n", gltf->nodes ? gltf->nodes : "");
	if (gltf->mesh_count)
		fprintf(file, "  \"meshes\": [%s\n  ],\n", gltf->meshes);
	if (gltf->material_count)
		fprintf(file, "  \"materials\": [%s\n  ],\n", gltf->materials);
	if (gltf->texture_count)
	{
		fprintf(file, "  \"samplers\": [{\"magFilter\": 9729, \"minFilter\": 9987, \"wrapS\": 10497, \"wrapT\": 10497}],\n");
		fprintf(file, "  \"textures\": [%s\n  ],\n  \"images\": [%s\n  ],\n", gltf->textures, gltf->images);
	}
	if (extra_top)
		fprintf(file, "%s", extra_top);
	if (gltf->accessor_count)
	{
		fprintf(file, "  \"accessors\": [%s\n  ],\n", gltf->accessors);
		fprintf(file, "  \"bufferViews\": [%s\n  ],\n", gltf->views);
	}
	fprintf(file, "  \"buffers\": [{\"uri\": \"%s\", \"byteLength\": %lu}]\n}\n",
		json_escaped(bin_name, escaped, sizeof(escaped)), (unsigned long)gltf->bin_length);
	fclose(file);
	free(gltf->bin);
	free(gltf->views);
	free(gltf->accessors);
	free(gltf->nodes);
	free(gltf->meshes);
	free(gltf->materials);
	free(gltf->textures);
	free(gltf->images);
	memset(gltf, 0, sizeof(*gltf));
}

/* ---------- geometry helpers */

/* rasterizer_geometry.c's uncompress_int32_to_real_vector3d */
static void uncompress_vector(unsigned long compressed, float *v)
{
	float value = (float)(long)(compressed << 21);

	v[0] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (float)(long)(compressed << 21);
	v[1] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (float)(long)(compressed << 22);
	v[2] = (value * (1.0f / 2097152.0f) + 1.0f) * (1.0f / 1023.0f);
}

static void normalize3(float *v)
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 1e-12f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
	else
	{
		v[0] = 0;
		v[1] = 0;
		v[2] = 1;
	}
}

static void cross3(float const *a, float const *b, float *result)
{
	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(float const *a, float const *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* a NORMSHORT component as the device reads it */
static float normshort(short value)
{
	float result = (float)value / 32767.0f;

	return result < -1.0f ? -1.0f : result;
}

/* a decoded vertex */
struct vertex
{
	float position[3], normal[3], tangent[4], binormal[3], texcoord[2], texcoord1[2], radiosity[3];
	unsigned short joints[4];
	float weights[4];
};

/* triangles from a part's indices (a list, or a strip as the device draws
it: every other triangle turned, degenerate ones skipped) */
static long triangles_from_indices(unsigned short const *indices, long index_count, int strip, unsigned long *result)
{
	long count = 0, i;

	if (!strip)
	{
		for (i = 0; i + 2 < index_count + 0 && i + 2 < index_count; i += 3)
		{
			result[count * 3] = indices[i];
			result[count * 3 + 1] = indices[i + 1];
			result[count * 3 + 2] = indices[i + 2];
			count++;
		}
		return count;
	}
	for (i = 0; i + 2 < index_count; i++)
	{
		unsigned long a = indices[i], b = indices[i + 1], c = indices[i + 2];

		if (a == b || b == c || a == c)
			continue;
		if (i & 1)
		{
			unsigned long t = a;

			a = b;
			b = t;
		}
		result[count * 3] = a;
		result[count * 3 + 1] = b;
		result[count * 3 + 2] = c;
		count++;
	}
	return count;
}

/* how many triangles face the way their vertices' normals do (with the
device's order), and how many face away */
static void winding_count(struct vertex const *vertices, unsigned long const *triangles, long triangle_count,
	long *with, long *against)
{
	long t;

	for (t = 0; t < triangle_count; t++)
	{
		struct vertex const *a = &vertices[triangles[t * 3]], *b = &vertices[triangles[t * 3 + 1]],
			*c = &vertices[triangles[t * 3 + 2]];
		float e1[3], e2[3], n[3], average[3];
		int k;

		for (k = 0; k < 3; k++)
		{
			e1[k] = b->position[k] - a->position[k];
			e2[k] = c->position[k] - a->position[k];
			average[k] = a->normal[k] + b->normal[k] + c->normal[k];
		}
		cross3(e1, e2, n);
		if (dot3(n, average) > 0)
			(*with)++;
		else if (dot3(n, average) < 0)
			(*against)++;
	}
}

/* how well the tag's tangent and binormal follow the texture's u and v
(summed cosines, of the triangles' uv derivatives) */
struct tangent_check
{
	double tangent_u, binormal_u, tangent_v, binormal_v;
	long count;
};

static void tangent_check_add(struct tangent_check *check, struct vertex const *vertices, unsigned long const *triangles,
	long triangle_count)
{
	long t;

	for (t = 0; t < triangle_count; t++)
	{
		struct vertex const *a = &vertices[triangles[t * 3]], *b = &vertices[triangles[t * 3 + 1]],
			*c = &vertices[triangles[t * 3 + 2]];
		float e1[3], e2[3], du1 = b->texcoord[0] - a->texcoord[0], dv1 = b->texcoord[1] - a->texcoord[1],
			du2 = c->texcoord[0] - a->texcoord[0], dv2 = c->texcoord[1] - a->texcoord[1];
		float determinant = du1 * dv2 - du2 * dv1, u[3], v[3];
		int k;

		if (fabsf(determinant) < 1e-12f)
			continue;
		for (k = 0; k < 3; k++)
		{
			e1[k] = b->position[k] - a->position[k];
			e2[k] = c->position[k] - a->position[k];
			u[k] = (e1[k] * dv2 - e2[k] * dv1) / determinant;
			v[k] = (e2[k] * du1 - e1[k] * du2) / determinant;
		}
		normalize3(u);
		normalize3(v);
		check->tangent_u += dot3(a->tangent, u);
		check->binormal_u += dot3(a->binormal, u);
		check->tangent_v += dot3(a->tangent, v);
		check->binormal_v += dot3(a->binormal, v);
		check->count++;
	}
}

/* a primitive's vertex attributes and indices added: its JSON */
static void gltf_primitive(struct gltf *gltf, char **text, size_t *length, struct vertex const *vertices,
	long vertex_count, unsigned long const *triangles, long triangle_count, int flip, long material, int skinned,
	int lightmap, int first)
{
	float *positions = allocate(vertex_count * 12), *normals = allocate(vertex_count * 12);
	float *tangents = allocate(vertex_count * 16), *texcoords = allocate(vertex_count * 8);
	float *texcoords1 = lightmap ? allocate(vertex_count * 8) : NULL;
	float *radiosity = lightmap ? allocate(vertex_count * 12) : NULL;
	unsigned short *joints = skinned ? allocate(vertex_count * 8) : NULL;
	float *weights = skinned ? allocate(vertex_count * 16) : NULL;
	unsigned long *indices = allocate(triangle_count * 12);
	float minimum[3] = { 1e30f, 1e30f, 1e30f }, maximum[3] = { -1e30f, -1e30f, -1e30f };
	long i, position, normal, tangent, texcoord, texcoord1 = NONE, radiosity_accessor = NONE, joint = NONE,
		weight = NONE, index;
	int k;

	for (i = 0; i < vertex_count; i++)
	{
		for (k = 0; k < 3; k++)
		{
			positions[i * 3 + k] = vertices[i].position[k];
			normals[i * 3 + k] = vertices[i].normal[k];
			if (vertices[i].position[k] < minimum[k])
				minimum[k] = vertices[i].position[k];
			if (vertices[i].position[k] > maximum[k])
				maximum[k] = vertices[i].position[k];
		}
		for (k = 0; k < 4; k++)
			tangents[i * 4 + k] = vertices[i].tangent[k];
		texcoords[i * 2] = vertices[i].texcoord[0];
		texcoords[i * 2 + 1] = vertices[i].texcoord[1];
		if (lightmap)
		{
			texcoords1[i * 2] = vertices[i].texcoord1[0];
			texcoords1[i * 2 + 1] = vertices[i].texcoord1[1];
			for (k = 0; k < 3; k++)
				radiosity[i * 3 + k] = vertices[i].radiosity[k];
		}
		if (skinned)
		{
			for (k = 0; k < 4; k++)
			{
				joints[i * 4 + k] = vertices[i].joints[k];
				weights[i * 4 + k] = vertices[i].weights[k];
			}
		}
	}
	for (i = 0; i < triangle_count; i++)
	{
		indices[i * 3] = triangles[i * 3];
		indices[i * 3 + 1] = triangles[i * 3 + (flip ? 2 : 1)];
		indices[i * 3 + 2] = triangles[i * 3 + (flip ? 1 : 2)];
	}
	position = gltf_accessor(gltf, positions, vertex_count, GLTF_FLOAT, "VEC3", 3, minimum, maximum, 34962);
	normal = gltf_accessor(gltf, normals, vertex_count, GLTF_FLOAT, "VEC3", 3, NULL, NULL, 34962);
	tangent = gltf_accessor(gltf, tangents, vertex_count, GLTF_FLOAT, "VEC4", 4, NULL, NULL, 34962);
	texcoord = gltf_accessor(gltf, texcoords, vertex_count, GLTF_FLOAT, "VEC2", 2, NULL, NULL, 34962);
	if (lightmap)
	{
		texcoord1 = gltf_accessor(gltf, texcoords1, vertex_count, GLTF_FLOAT, "VEC2", 2, NULL, NULL, 34962);
		radiosity_accessor = gltf_accessor(gltf, radiosity, vertex_count, GLTF_FLOAT, "VEC3", 3, NULL, NULL, 34962);
	}
	if (skinned)
	{
		joint = gltf_accessor(gltf, joints, vertex_count, GLTF_UNSIGNED_SHORT, "VEC4", 4, NULL, NULL, 34962);
		weight = gltf_accessor(gltf, weights, vertex_count, GLTF_FLOAT, "VEC4", 4, NULL, NULL, 34962);
	}
	index = gltf_accessor(gltf, indices, triangle_count * 3, GLTF_UNSIGNED_INT, "SCALAR", 1, NULL, NULL, 34963);
	text_append(text, length, "%s{\"attributes\": {\"POSITION\": %ld, \"NORMAL\": %ld, \"TANGENT\": %ld, "
		"\"TEXCOORD_0\": %ld", first ? "" : ", ", position, normal, tangent, texcoord);
	if (lightmap)
		text_append(text, length, ", \"TEXCOORD_1\": %ld, \"_INCIDENT_RADIOSITY\": %ld", texcoord1, radiosity_accessor);
	if (skinned)
		text_append(text, length, ", \"JOINTS_0\": %ld, \"WEIGHTS_0\": %ld", joint, weight);
	text_append(text, length, "}, \"indices\": %ld, \"material\": %ld, \"mode\": 4}", index, material);
	free(positions);
	free(normals);
	free(tangents);
	free(texcoords);
	free(texcoords1);
	free(radiosity);
	free(joints);
	free(weights);
	free(indices);
}

/* the tangent's handedness (glTF's w) from the tag's binormal */
static void vertex_finish(struct vertex *vertex)
{
	float b[3];

	normalize3(vertex->normal);
	normalize3(vertex->tangent);
	normalize3(vertex->binormal);
	cross3(vertex->normal, vertex->tangent, b);
	vertex->tangent[3] = dot3(b, vertex->binormal) < 0 ? -1.0f : 1.0f;
}

/* ---------- models */

static char const *const detail_level_names[NUMBER_OF_DETAIL_LEVELS] =
	{ "superlow", "low", "medium", "high", "superhigh" };

/* the quaternion as glTF's: the game's turn the other way
(matrix_math.c's matrix4x3_rotation_from_quaternion makes the transpose of
the usual matrix) */
static void quaternion_to_gltf(struct real_quaternion const *q, float *result)
{
	float length = sqrtf(q->i * q->i + q->j * q->j + q->k * q->k + q->w * q->w);

	if (length < 1e-12f)
	{
		result[0] = result[1] = result[2] = 0;
		result[3] = 1;
		return;
	}
	result[0] = -q->i / length;
	result[1] = -q->j / length;
	result[2] = -q->k / length;
	result[3] = q->w / length;
}

/* the game's matrix of a point and quaternion (matrix4x3_from_point_and_quaternion),
column-major 4x4 */
static void matrix_from_point_and_quaternion(struct real_point3d const *p, struct real_quaternion const *q, float *m)
{
	float scale = q->i * q->i + q->j * q->j + q->k * q->k + q->w * q->w;
	float x, y, z, wx, wy, wz, xx, xy, xz, yy, yz, zz;

	scale = scale != 0.f ? 2.f / scale : 0.f;
	x = scale * q->i;
	y = scale * q->j;
	z = scale * q->k;
	wx = q->w * x;
	wy = q->w * y;
	wz = q->w * z;
	xx = q->i * x;
	xy = q->i * y;
	xz = q->i * z;
	yy = q->j * y;
	yz = q->j * z;
	zz = q->k * z;
	/* forward, left, up (the columns) */
	m[0] = 1.f - (yy + zz);
	m[1] = xy - wz;
	m[2] = xz + wy;
	m[3] = 0;
	m[4] = xy + wz;
	m[5] = 1.f - (xx + zz);
	m[6] = yz - wx;
	m[7] = 0;
	m[8] = xz - wy;
	m[9] = yz + wx;
	m[10] = 1.f - (xx + yy);
	m[11] = 0;
	m[12] = p->x;
	m[13] = p->y;
	m[14] = p->z;
	m[15] = 1;
}

static void matrix_multiply(float const *a, float const *b, float *result)
{
	float r[16];
	int i, j, k;

	for (i = 0; i < 4; i++)
	{
		for (j = 0; j < 4; j++)
		{
			float sum = 0;

			for (k = 0; k < 4; k++)
				sum += a[k * 4 + j] * b[i * 4 + k];
			r[i * 4 + j] = sum;
		}
	}
	memcpy(result, r, sizeof(r));
}

static void matrix_from_matrix4x3(struct real_matrix4x3 const *matrix, float *m)
{
	int k;

	for (k = 0; k < 3; k++)
	{
		m[0 + k] = matrix->forward[k] * matrix->scale;
		m[4 + k] = matrix->left[k] * matrix->scale;
		m[8 + k] = matrix->up[k] * matrix->scale;
		m[12 + k] = matrix->position[k];
	}
	m[3] = m[7] = m[11] = 0;
	m[15] = 1;
}

/* a part's vertices decoded: their count, or 0 */
static long model_part_vertices(struct model const *model, struct model_geometry_part const *part, long node_count,
	struct vertex **result)
{
	struct vertex_buffer const *buffer = &part->vertex_buffer;
	long count = buffer->count, i;
	long size = buffer->type == _rasterizer_vertex_type_model_compressed ? 32 :
		buffer->type == _rasterizer_vertex_type_model_uncompressed ? 68 : 0;
	unsigned char const *data;
	struct vertex *vertices;
	float u_scale = model->base_map_scale[0], v_scale = model->base_map_scale[1];

	*result = NULL;
	if (!buffer->hardware_format || !size || count <= 0 ||
		!cache_bytes(buffer->hardware_format, sizeof(struct d3d_buffer)))
	{
		return 0;
	}
	data = cache_bytes(buffer->hardware_format->data, (unsigned long)count * size);
	if (!data)
		return 0;
	if (u_scale == 0 && v_scale == 0)
		u_scale = v_scale = 1;
	vertices = allocate(count * sizeof(struct vertex));
	for (i = 0; i < count; i++)
	{
		struct vertex *v = &vertices[i];
		int nodes[2];
		float weights[2];
		int k;

		if (buffer->type == _rasterizer_vertex_type_model_compressed)
		{
			struct model_vertex_compressed const *c = (void const *)(data + i * size);

			memcpy(v->position, c->position, 12);
			uncompress_vector(c->normal, v->normal);
			uncompress_vector(c->binormal, v->binormal);
			uncompress_vector(c->tangent, v->tangent);
			v->texcoord[0] = normshort(c->texcoord[0]) * u_scale;
			v->texcoord[1] = normshort(c->texcoord[1]) * v_scale;
			nodes[0] = c->nodes[0] / 3;
			nodes[1] = c->nodes[1] / 3;
			weights[0] = normshort(c->node_weight);
			if (weights[0] < 0)
				weights[0] = 0;
			weights[1] = 1.0f - weights[0];
		}
		else
		{
			struct model_vertex_uncompressed const *u = (void const *)(data + i * size);

			memcpy(v->position, u->position, 12);
			memcpy(v->normal, u->normal, 12);
			memcpy(v->binormal, u->binormal, 12);
			memcpy(v->tangent, u->tangent, 12);
			v->texcoord[0] = u->texcoord[0] * u_scale;
			v->texcoord[1] = u->texcoord[1] * v_scale;
			nodes[0] = u->nodes[0];
			nodes[1] = u->nodes[1];
			weights[0] = u->node_weights[0];
			weights[1] = u->node_weights[1];
		}
		for (k = 0; k < 2; k++)
		{
			if (nodes[k] < 0 || nodes[k] >= node_count || weights[k] <= 0)
			{
				nodes[k] = 0;
				weights[k] = 0;
			}
			v->joints[k] = (unsigned short)nodes[k];
			v->weights[k] = weights[k];
		}
		/* (two influences on one node are one) */
		if (v->joints[0] == v->joints[1] && v->weights[1] > 0)
		{
			v->weights[0] += v->weights[1];
			v->weights[1] = 0;
		}
		if (v->weights[0] + v->weights[1] <= 0)
			v->weights[0] = 1;
		else
		{
			float sum = v->weights[0] + v->weights[1];

			v->weights[0] /= sum;
			v->weights[1] /= sum;
		}
		vertex_finish(v);
	}
	*result = vertices;
	return count;
}

/* a part's triangles: their count (the indices, 3 a triangle, in *result) */
static long model_part_triangles(struct model_geometry_part const *part, unsigned long **result)
{
	struct triangle_buffer const *buffer = &part->triangle_buffer;
	long index_count;
	unsigned short const *indices;

	*result = NULL;
	if (!buffer->hardware_format || buffer->count <= 0 ||
		!cache_bytes(buffer->hardware_format, sizeof(struct d3d_buffer)))
	{
		return 0;
	}
	index_count = buffer->type == _triangle_buffer_type_triangles ? buffer->count * 3 :
		buffer->type == _triangle_buffer_type_precompiled_strip ? buffer->count + 2 : 0;
	indices = cache_bytes(buffer->hardware_format->data, (unsigned long)index_count * 2);
	if (!indices || !index_count)
		return 0;
	*result = allocate(index_count * 3 * sizeof(unsigned long));
	return triangles_from_indices(indices, index_count, buffer->type == _triangle_buffer_type_precompiled_strip, *result);
}

struct model_statistics
{
	long with, against;
	struct tangent_check tangents;
	double bind_error;
	long vertex_count, triangle_count, part_count, geometry_count;
};

/* a model's JMS (8200) for one permutation (by name) and level of detail:
every region's permutation of that name */
static void model_jms(long tag_index, struct model const *model, char const *permutation_name, int level,
	char const *directory, int flip)
{
	struct model_node *nodes = model->nodes.address;
	struct model_region *regions = model->regions.address;
	struct model_geometry *geometries = model->geometries.address;
	struct model_shader_reference *shaders = model->shaders.address;
	char path[2600];
	FILE *file;
	long r, i, total_vertices = 0, total_triangles = 0;
	long node_count = model->nodes.count;
	struct region_part { long region; struct vertex *vertices; long vertex_count; unsigned long *triangles;
		long triangle_count; long shader; } *parts = NULL;
	long part_count = 0;

	/* the parts of the permutation's geometry in each region */
	for (r = 0; r < model->regions.count; r++)
	{
		struct model_region_permutation *permutations = regions[r].permutations.address;
		long p;

		for (p = 0; p < regions[r].permutations.count; p++)
		{
			char name[33];
			short geometry_index;
			struct model_geometry *geometry;
			long part_index;

			field_string(name, permutations[p].name, 32);
			if (strcmp(name, permutation_name))
				continue;
			geometry_index = permutations[p].geometry_indices[level];
			if (geometry_index < 0 || geometry_index >= model->geometries.count)
				continue;
			geometry = &geometries[geometry_index];
			for (part_index = 0; part_index < geometry->parts.count; part_index++)
			{
				struct model_geometry_part *part = (struct model_geometry_part *)geometry->parts.address + part_index;

				parts = realloc(parts, (part_count + 1) * sizeof(*parts));
				if (!parts)
					fail("out of memory");
				parts[part_count].region = r;
				parts[part_count].vertex_count = model_part_vertices(model, part, node_count, &parts[part_count].vertices);
				parts[part_count].triangle_count = parts[part_count].vertex_count ?
					model_part_triangles(part, &parts[part_count].triangles) : 0;
				parts[part_count].shader = part->shader_index;
				total_vertices += parts[part_count].vertex_count;
				total_triangles += parts[part_count].triangle_count;
				part_count++;
			}
		}
	}
	if (!part_count)
		return;
	snprintf(path, sizeof(path), "%s/%s/%s %s.jms", globals.out, directory, permutation_name, detail_level_names[level]);
	file = open_output(path, "w");
	fprintf(file, "8200\n%lu\n%ld\n", model->node_list_checksum, node_count);
	for (i = 0; i < node_count; i++)
	{
		char name[33];

		field_string(name, nodes[i].name, 32);
		fprintf(file, "%s\n%d\n%d\n%.10g\t%.10g\t%.10g\t%.10g\n%.10g\t%.10g\t%.10g\n", name,
			nodes[i].first_child_node_index, nodes[i].next_sibling_node_index,
			nodes[i].default_rotation.i, nodes[i].default_rotation.j, nodes[i].default_rotation.k,
			nodes[i].default_rotation.w, nodes[i].default_translation.x * 100, nodes[i].default_translation.y * 100,
			nodes[i].default_translation.z * 100);
	}
	fprintf(file, "%ld\n", model->shaders.count);
	for (i = 0; i < model->shaders.count; i++)
	{
		char const *name = tag_name(shaders[i].shader.index);
		char const *last = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;

		fprintf(file, "%s\n<none>\n", last[0] ? last : "none");
	}
	/* (the markers of the permutation's regions) */
	{
		long marker_count = 0, m;
		struct model_marker *markers = model->markers.address;

		for (m = 0; m < model->markers.count; m++)
		{
			long n;

			for (n = 0; n < markers[m].instances.count; n++)
			{
				struct model_marker_instance *instance = (struct model_marker_instance *)markers[m].instances.address + n;
				struct model_region_permutation *permutations;
				char name[33];

				if (instance->region_index >= model->regions.count ||
					instance->permutation_index >= regions[instance->region_index].permutations.count)
				{
					continue;
				}
				permutations = regions[instance->region_index].permutations.address;
				field_string(name, permutations[instance->permutation_index].name, 32);
				if (!strcmp(name, permutation_name))
					marker_count++;
			}
		}
		fprintf(file, "%ld\n", marker_count);
		for (m = 0; m < model->markers.count; m++)
		{
			long n;

			for (n = 0; n < markers[m].instances.count; n++)
			{
				struct model_marker_instance *instance = (struct model_marker_instance *)markers[m].instances.address + n;
				struct model_region_permutation *permutations;
				char name[33], marker_name[33];

				if (instance->region_index >= model->regions.count ||
					instance->permutation_index >= regions[instance->region_index].permutations.count)
				{
					continue;
				}
				permutations = regions[instance->region_index].permutations.address;
				field_string(name, permutations[instance->permutation_index].name, 32);
				if (strcmp(name, permutation_name))
					continue;
				field_string(marker_name, markers[m].name, 32);
				fprintf(file, "%s\n%d\n%d\n%.10g\t%.10g\t%.10g\t%.10g\n%.10g\t%.10g\t%.10g\n%.10g\n", marker_name,
					instance->region_index, instance->node_index, instance->rotation.i, instance->rotation.j,
					instance->rotation.k, instance->rotation.w, instance->translation.x * 100,
					instance->translation.y * 100, instance->translation.z * 100, 1.0);
			}
		}
	}
	fprintf(file, "%ld\n", model->regions.count);
	for (r = 0; r < model->regions.count; r++)
	{
		char name[33];

		field_string(name, regions[r].name, 32);
		fprintf(file, "%s\n", name);
	}
	fprintf(file, "%ld\n", total_vertices);
	for (i = 0; i < part_count; i++)
	{
		long v;

		for (v = 0; v < parts[i].vertex_count; v++)
		{
			struct vertex *vertex = &parts[i].vertices[v];

			fprintf(file, "%d\n%.10g\t%.10g\t%.10g\n%.10g\t%.10g\t%.10g\n%d\n%.10g\n%.10g\n%.10g\n0\n",
				vertex->joints[0], vertex->position[0] * 100, vertex->position[1] * 100, vertex->position[2] * 100,
				vertex->normal[0], vertex->normal[1], vertex->normal[2],
				vertex->weights[1] > 0 ? vertex->joints[1] : -1, vertex->weights[1],
				vertex->texcoord[0], 1.0f - vertex->texcoord[1]);
		}
	}
	fprintf(file, "%ld\n", total_triangles);
	{
		long base = 0;

		for (i = 0; i < part_count; i++)
		{
			long t;

			for (t = 0; t < parts[i].triangle_count; t++)
			{
				unsigned long *tri = &parts[i].triangles[t * 3];

				/* (JMS triangles are in the tools' order, the device's: the
				same as the map's) */
				fprintf(file, "%ld\n%ld\n%ld\t%ld\t%ld\n", parts[i].region, (long)parts[i].shader,
					base + (long)tri[0], base + (long)tri[1], base + (long)tri[2]);
			}
			base += parts[i].vertex_count;
		}
	}
	(void)flip;
	(void)tag_index;
	fclose(file);
	for (i = 0; i < part_count; i++)
	{
		free(parts[i].vertices);
		free(parts[i].triangles);
	}
	free(parts);
}

static void model_export(long tag_index)
{
	struct tag_instance *instance = instance_get(tag_index);
	struct model *model;
	struct model_node *nodes;
	struct model_region *regions;
	struct model_geometry *geometries;
	struct model_shader_reference *shaders;
	struct model_marker *markers;
	struct gltf gltf;
	struct model_statistics statistics;
	char tag[1024], relative[1100], gltf_path[1200], escaped[2400];
	long node_count, i, joint_base, *geometry_mesh, *materials;
	char *scene = NULL;
	size_t scene_length = 0;
	char *root_children = NULL;
	size_t root_children_length = 0;
	char *skin = NULL;
	size_t skin_length = 0;
	float *world, *inverse;
	int flip;
	long level;

	if (!instance || instance->group_tag != MODEL_GROUP_TAG)
		return;
	model = instance->base_address;
	nodes = model->nodes.address;
	regions = model->regions.address;
	geometries = model->geometries.address;
	shaders = model->shaders.address;
	markers = model->markers.address;
	node_count = model->nodes.count;
	if (node_count <= 0 || node_count > MAXIMUM_NODES_PER_MODEL)
	{
		warn("%s has %ld nodes: not written", tag_name(tag_index), node_count);
		return;
	}
	memset(&gltf, 0, sizeof(gltf));
	memset(&statistics, 0, sizeof(statistics));
	tag_path(tag_index, tag, sizeof(tag));
	snprintf(relative, sizeof(relative), "models/%s", tag);
	snprintf(gltf_path, sizeof(gltf_path), "%s.gltf", relative);

	/* the bind pose: each node's world matrix from the tag's default
	translation and rotation (models.c, model_get_node_matrices), against the
	tag's inverse matrices */
	world = allocate(node_count * 16 * sizeof(float));
	inverse = allocate(node_count * 16 * sizeof(float));
	for (i = 0; i < node_count; i++)
	{
		float local[16], check[16];
		int k;

		matrix_from_point_and_quaternion(&nodes[i].default_translation, &nodes[i].default_rotation, local);
		if (nodes[i].parent_node_index >= 0 && nodes[i].parent_node_index < i)
			matrix_multiply(world + nodes[i].parent_node_index * 16, local, world + i * 16);
		else
			memcpy(world + i * 16, local, sizeof(local));
		matrix_from_matrix4x3(&nodes[i].runtime_default_inverse_matrix, inverse + i * 16);
		matrix_multiply(world + i * 16, inverse + i * 16, check);
		for (k = 0; k < 16; k++)
		{
			double error = fabs(check[k] - ((k % 5) == 0 ? 1.0 : 0.0));

			if (error > statistics.bind_error)
				statistics.bind_error = error;
		}
	}

	/* the materials: one of each shader */
	materials = allocate((model->shaders.count + 1) * sizeof(long));
	for (i = 0; i < model->shaders.count; i++)
	{
		char extra[64];

		snprintf(extra, sizeof(extra), "\"permutation_index\": %d", shaders[i].permutation_index);
		materials[i] = gltf_material(&gltf, gltf_path, shaders[i].shader.index, extra);
	}
	materials[model->shaders.count] = gltf_material(&gltf, gltf_path, NONE, NULL);

	/* the winding: the device's front faces are clockwise, glTF's counter-
	clockwise; which it is is checked against the vertices' normals */
	for (i = 0; i < model->geometries.count; i++)
	{
		long p;

		for (p = 0; p < geometries[i].parts.count; p++)
		{
			struct model_geometry_part *part = (struct model_geometry_part *)geometries[i].parts.address + p;
			struct vertex *vertices;
			unsigned long *triangles = NULL;
			long vertex_count = model_part_vertices(model, part, node_count, &vertices);
			long triangle_count = vertex_count ? model_part_triangles(part, &triangles) : 0;

			winding_count(vertices, triangles, triangle_count, &statistics.with, &statistics.against);
			tangent_check_add(&statistics.tangents, vertices, triangles, triangle_count);
			free(vertices);
			free(triangles);
		}
	}
	flip = statistics.against > statistics.with;

	/* node 0: the root, Z up to glTF's Y up, scaled; then the model's nodes */
	joint_base = 1;
	gltf.node_count = 1 + node_count;
	text_append(&gltf.nodes, &gltf.nodes_length, "\n    {\"name\": \"%s\", \"rotation\": [-0.70710678, 0, 0, 0.70710678], "
		"\"scale\": [%.9g, %.9g, %.9g], \"children\": [ROOT_CHILDREN]}",
		json_escaped(tag_name(tag_index), escaped, sizeof(escaped)), globals.scale, globals.scale, globals.scale);
	for (i = 0; i < node_count; i++)
	{
		char name[33];
		float rotation[4];
		long child;
		int first = 1;

		field_string(name, nodes[i].name, 32);
		quaternion_to_gltf(&nodes[i].default_rotation, rotation);
		text_append(&gltf.nodes, &gltf.nodes_length, ",\n    {\"name\": \"%s\", \"translation\": [%.9g, %.9g, %.9g], "
			"\"rotation\": [%.9g, %.9g, %.9g, %.9g]",
			json_escaped(name, escaped, sizeof(escaped)), nodes[i].default_translation.x, nodes[i].default_translation.y,
			nodes[i].default_translation.z, rotation[0], rotation[1], rotation[2], rotation[3]);
		/* its children: its child nodes and its markers (added below, by
		index; markers are listed after the nodes) */
		text_append(&gltf.nodes, &gltf.nodes_length, ", \"children\": [");
		for (child = 0; child < node_count; child++)
		{
			if (nodes[child].parent_node_index == i && child != i)
			{
				text_append(&gltf.nodes, &gltf.nodes_length, "%s%ld", first ? "" : ", ", joint_base + child);
				first = 0;
			}
		}
		/* (the markers' nodes are numbered from 1 + node_count) */
		{
			long m, marker_node = 1 + node_count;

			for (m = 0; m < model->markers.count; m++)
			{
				long n;

				for (n = 0; n < markers[m].instances.count; n++, marker_node++)
				{
					struct model_marker_instance *marker_instance =
						(struct model_marker_instance *)markers[m].instances.address + n;

					if (marker_instance->node_index == i)
					{
						text_append(&gltf.nodes, &gltf.nodes_length, "%s%ld", first ? "" : ", ", marker_node);
						first = 0;
					}
				}
			}
		}
		text_append(&gltf.nodes, &gltf.nodes_length, "], \"extras\": {\"node_index\": %ld, "
			"\"node_distance_from_parent\": %.9g}}", i, nodes[i].node_distance_from_parent);
		if (nodes[i].parent_node_index < 0 || nodes[i].parent_node_index >= node_count)
			text_append(&root_children, &root_children_length, "%s%ld", root_children_length ? ", " : "", joint_base + i);
	}
	/* the markers: a node each, a child of its node */
	{
		long m;

		for (m = 0; m < model->markers.count; m++)
		{
			char name[33];
			long n;

			field_string(name, markers[m].name, 32);
			for (n = 0; n < markers[m].instances.count; n++)
			{
				struct model_marker_instance *marker_instance =
					(struct model_marker_instance *)markers[m].instances.address + n;
				float rotation[4];
				char region_name[33] = "", permutation_name[33] = "";

				quaternion_to_gltf(&marker_instance->rotation, rotation);
				if (marker_instance->region_index < model->regions.count)
				{
					struct model_region *region = &regions[marker_instance->region_index];

					field_string(region_name, region->name, 32);
					if (marker_instance->permutation_index < region->permutations.count)
					{
						field_string(permutation_name,
							((struct model_region_permutation *)region->permutations.address)[marker_instance->permutation_index].name,
							32);
					}
				}
				text_append(&gltf.nodes, &gltf.nodes_length, ",\n    {\"name\": \"#%s\", \"translation\": [%.9g, %.9g, %.9g], "
					"\"rotation\": [%.9g, %.9g, %.9g, %.9g], \"extras\": {\"marker\": \"%s\", \"region\": \"%s\", "
					"\"permutation\": \"%s\", \"region_index\": %d, \"permutation_index\": %d, \"node_index\": %d}}",
					json_escaped(name, escaped, sizeof(escaped)), marker_instance->translation.x,
					marker_instance->translation.y, marker_instance->translation.z, rotation[0], rotation[1], rotation[2],
					rotation[3], json_escaped(name, escaped + 1200, sizeof(escaped) - 1200), region_name, permutation_name,
					marker_instance->region_index, marker_instance->permutation_index, marker_instance->node_index);
				gltf.node_count++;
			}
		}
	}

	/* the meshes: one of each geometry that draws, a primitive of each part */
	geometry_mesh = allocate((model->geometries.count + 1) * sizeof(long));
	for (i = 0; i < model->geometries.count; i++)
	{
		long p;
		int first = 1;
		char *primitives = NULL;
		size_t primitives_length = 0;

		geometry_mesh[i] = NONE;
		for (p = 0; p < geometries[i].parts.count; p++)
		{
			struct model_geometry_part *part = (struct model_geometry_part *)geometries[i].parts.address + p;
			struct vertex *vertices;
			unsigned long *triangles = NULL;
			long vertex_count = model_part_vertices(model, part, node_count, &vertices);
			long triangle_count = vertex_count ? model_part_triangles(part, &triangles) : 0;
			long material = part->shader_index >= 0 && part->shader_index < model->shaders.count ?
				materials[part->shader_index] : materials[model->shaders.count];

			if (triangle_count)
			{
				gltf_primitive(&gltf, &primitives, &primitives_length, vertices, vertex_count, triangles,
					triangle_count, flip, material, 1, 0, first);
				first = 0;
				statistics.vertex_count += vertex_count;
				statistics.triangle_count += triangle_count;
				statistics.part_count++;
			}
			free(vertices);
			free(triangles);
		}
		/* (a geometry that draws nothing has no mesh: glTF's has a primitive) */
		if (first)
		{
			warn("%s geometry %ld draws nothing", tag_name(tag_index), i);
			continue;
		}
		text_append(&gltf.meshes, &gltf.meshes_length, "%s\n    {\"name\": \"geometry %ld\", \"primitives\": [%s]}",
			gltf.mesh_count ? "," : "", i, primitives);
		free(primitives);
		geometry_mesh[i] = gltf.mesh_count++;
		statistics.geometry_count++;
	}

	/* the levels of detail: a node of each, holding a node of each region's
	permutation (its geometry's mesh, skinned) */
	for (level = NUMBER_OF_DETAIL_LEVELS - 1; level >= 0; level--)
	{
		long level_node = gltf.node_count++;
		char *children = NULL;
		size_t children_length = 0;
		long r;
		char *mesh_nodes = NULL;
		size_t mesh_nodes_length = 0;

		for (r = 0; r < model->regions.count; r++)
		{
			struct model_region_permutation *permutations = regions[r].permutations.address;
			long p;
			char region_name[33];

			field_string(region_name, regions[r].name, 32);
			for (p = 0; p < regions[r].permutations.count; p++)
			{
				short geometry_index = permutations[p].geometry_indices[level];
				char permutation_name[33];

				if (geometry_index < 0 || geometry_index >= model->geometries.count || geometry_mesh[geometry_index] == NONE)
					continue;
				field_string(permutation_name, permutations[p].name, 32);
				text_append(&mesh_nodes, &mesh_nodes_length, ",\n    {\"name\": \"%s/%s/%s\", \"mesh\": %ld, \"skin\": 0, "
					"\"extras\": {\"region\": \"%s\", \"permutation\": \"%s\", \"lod\": \"%s\", \"geometry_index\": %d, "
					"\"cannot_be_chosen_randomly\": %s}}",
					region_name, permutation_name, detail_level_names[level], geometry_mesh[geometry_index], region_name,
					permutation_name, detail_level_names[level], geometry_index, permutations[p].flags & 1 ? "true" : "false");
				text_append(&children, &children_length, "%s%ld", children_length ? ", " : "", gltf.node_count++);
			}
		}
		text_append(&gltf.nodes, &gltf.nodes_length, ",\n    {\"name\": \"lod_%s\", \"children\": [%s], \"extras\": "
			"{\"lod\": %ld, \"detail_cutoff_pixels\": %.9g, \"node_count\": %d}}",
			detail_level_names[level], children ? children : "", level, model->detail_cutoff_pixels[level],
			model->node_counts[level]);
		if (mesh_nodes)
			text_append(&gltf.nodes, &gltf.nodes_length, "%s", mesh_nodes);
		text_append(&root_children, &root_children_length, ", %ld", level_node);
		free(children);
		free(mesh_nodes);
	}

	/* the skin: the nodes as joints, the tag's inverse matrices */
	{
		long inverse_accessor = gltf_accessor(&gltf, inverse, node_count, GLTF_FLOAT, "MAT4", 16, NULL, NULL, 0);

		text_append(&skin, &skin_length, "  \"skins\": [{\"name\": \"%s\", \"inverseBindMatrices\": %ld, \"skeleton\": 1, "
			"\"joints\": [", json_escaped(tag_name(tag_index), escaped, sizeof(escaped)), inverse_accessor);
		for (i = 0; i < node_count; i++)
			text_append(&skin, &skin_length, "%s%ld", i ? ", " : "", joint_base + i);
		text_append(&skin, &skin_length, "]}],\n");
	}
	/* (the root's children, now known) */
	{
		char *marker = strstr(gltf.nodes, "ROOT_CHILDREN");
		char *nodes_text;
		size_t before = (size_t)(marker - gltf.nodes);

		nodes_text = allocate(gltf.nodes_length + root_children_length + 1);
		memcpy(nodes_text, gltf.nodes, before);
		memcpy(nodes_text + before, root_children, root_children_length);
		strcpy(nodes_text + before + root_children_length, marker + strlen("ROOT_CHILDREN"));
		free(gltf.nodes);
		gltf.nodes = nodes_text;
		gltf.nodes_length = strlen(nodes_text);
	}
	text_append(&scene, &scene_length, "0");
	/* (the model's own fields, as extras of the asset: kept on the root) */
	{
		char *extra = NULL;
		size_t extra_length = 0;

		text_append(&extra, &extra_length, "%s  \"extras\": {\"tag\": \"%s\", \"node_list_checksum\": %lu, "
			"\"base_map_scale\": [%.9g, %.9g], \"units\": \"world units (1 = 10 feet) times %.9g\", \"winding_flipped\": %s, "
			"\"bind_pose_error\": %.3g},\n",
			skin, json_escaped(tag_name(tag_index), escaped, sizeof(escaped)), model->node_list_checksum,
			model->base_map_scale[0], model->base_map_scale[1], globals.scale, flip ? "true" : "false",
			statistics.bind_error);
		gltf_write(&gltf, relative, scene, extra);
		free(extra);
	}

	/* the JMS: one of each permutation name and level of detail */
	{
		long r, p;
		char names[256][33];
		int name_count = 0, n;

		for (r = 0; r < model->regions.count; r++)
		{
			struct model_region_permutation *permutations = regions[r].permutations.address;

			for (p = 0; p < regions[r].permutations.count && name_count < 256; p++)
			{
				char name[33];

				field_string(name, permutations[p].name, 32);
				for (n = 0; n < name_count && strcmp(names[n], name); n++)
					;
				if (n == name_count)
					strcpy(names[name_count++], name);
			}
		}
		for (n = 0; n < name_count; n++)
		{
			for (level = 0; level < NUMBER_OF_DETAIL_LEVELS; level++)
				model_jms(tag_index, model, names[n], (int)level, relative, flip);
		}
	}

	fprintf(globals.index, "%s\n    {\"kind\": \"model\", \"tag\": ", globals.index_first ? "" : ",");
	globals.index_first = 0;
	json_string(globals.index, tag_name(tag_index));
	fprintf(globals.index, ", \"gltf\": ");
	json_string(globals.index, gltf_path);
	fprintf(globals.index, ", \"nodes\": %ld, \"regions\": %ld, \"markers\": %ld, \"geometries\": %ld, \"parts\": %ld, "
		"\"vertices\": %ld, \"triangles\": %ld, \"shaders\": %ld,\n      \"checks\": {\"bind_pose_error\": %.3g, "
		"\"triangles_facing_normals_device_order\": %ld, \"triangles_facing_away\": %ld, \"winding_flipped\": %s",
		node_count, model->regions.count, model->markers.count, statistics.geometry_count, statistics.part_count,
		statistics.vertex_count, statistics.triangle_count, model->shaders.count, statistics.bind_error,
		statistics.with, statistics.against, flip ? "true" : "false");
	if (statistics.tangents.count)
	{
		double n = (double)statistics.tangents.count;

		fprintf(globals.index, ", \"mean_cos_tangent_u\": %.3f, \"mean_cos_binormal_u\": %.3f, "
			"\"mean_cos_tangent_v\": %.3f, \"mean_cos_binormal_v\": %.3f",
			statistics.tangents.tangent_u / n, statistics.tangents.binormal_u / n,
			statistics.tangents.tangent_v / n, statistics.tangents.binormal_v / n);
	}
	fprintf(globals.index, "}}");
	(void)shaders;
	free(world);
	free(inverse);
	free(materials);
	free(geometry_mesh);
	free(scene);
	free(root_children);
	free(skin);
}

/* ---------- structure bsps */

static void bsp_export(long bsp_index)
{
	struct structure_bsp_header *header = bsp_load(bsp_index);
	struct tag_schema_group const *group = schema_group(STRUCTURE_BSP_GROUP_TAG);
	struct tag_schema_field const *surfaces_field, *lightmaps_field, *lightmap_group_field;
	unsigned char *bsp;
	struct tag_block *surfaces_block, *lightmaps_block;
	struct tag_reference *lightmap_group;
	struct structure_surface *surfaces;
	struct gltf gltf;
	char tag[1024], relative[1100], gltf_path[1200], escaped[2400];
	long tag_index, l;
	long with = 0, against = 0, vertex_total = 0, triangle_total = 0, primitive_count = 0, material_total = 0;
	long orphan_surfaces = 0;
	char *primitives = NULL;
	size_t primitives_length = 0;
	int flip;
	struct pass { int count; } pass;

	if (!header || !group || !group->definition)
		return;
	tag_index = globals.loaded_bsp;
	bsp = header->base_address;
	/* (the fields' offsets as the game's structure has them: the schema's) */
	surfaces_field = schema_field(group->definition, "surfaces");
	lightmaps_field = schema_field(group->definition, "lightmaps");
	lightmap_group_field = schema_field(group->definition, "lightmap_group");
	if (!surfaces_field || !lightmaps_field || !lightmap_group_field ||
		!lightmaps_field->definition || ((struct tag_schema_definition const *)lightmaps_field->definition)->size !=
			sizeof(struct structure_lightmap))
	{
		warn("the structure bsp schema is not the one this tool knows");
		return;
	}
	surfaces_block = (struct tag_block *)(bsp + surfaces_field->offset);
	lightmaps_block = (struct tag_block *)(bsp + lightmaps_field->offset);
	lightmap_group = (struct tag_reference *)(bsp + lightmap_group_field->offset);
	surfaces = surfaces_block->address;
	memset(&gltf, 0, sizeof(gltf));
	tag_path(tag_index, tag, sizeof(tag));
	snprintf(relative, sizeof(relative), "bsps/%s", tag);
	snprintf(gltf_path, sizeof(gltf_path), "%s.gltf", relative);
	if (globals.want_bitmaps && lightmap_group->index != NONE)
		bitmap_export(lightmap_group->index);

	/* (the surfaces no material has: structure_render_pass draws those
	between materials with the next; the retail maps have none) */
	{
		unsigned char *covered = allocate(surfaces_block->count > 0 ? surfaces_block->count : 1);

		for (l = 0; l < lightmaps_block->count; l++)
		{
			struct structure_lightmap *lightmap = (struct structure_lightmap *)lightmaps_block->address + l;
			long m, s;

			for (m = 0; m < lightmap->materials.count; m++)
			{
				struct structure_material *material = (struct structure_material *)lightmap->materials.address + m;

				for (s = 0; s < material->surface_count; s++)
					covered[material->first_surface_index + s] = 1;
			}
		}
		for (l = 0; l < surfaces_block->count; l++)
			orphan_surfaces += !covered[l];
		free(covered);
	}

	/* two passes: the winding, then the primitives */
	for (pass.count = 0; pass.count < 2; pass.count++)
	{
		flip = against > with;
		for (l = 0; l < lightmaps_block->count; l++)
		{
			struct structure_lightmap *lightmap = (struct structure_lightmap *)lightmaps_block->address + l;
			long m;

			for (m = 0; m < lightmap->materials.count; m++)
			{
				struct structure_material *material = (struct structure_material *)lightmap->materials.address + m;
				long vertex_count = material->vertices.count, lightmap_count = material->lightmap_vertices.count;
				long vertex_size = material->vertices.type == _rasterizer_vertex_type_environment_compressed ? 32 : 56;
				long lightmap_size = material->lightmap_vertices.type ==
					_rasterizer_vertex_type_environment_lightmap_compressed ? 8 : 20;
				unsigned char const *vertex_data = NULL, *lightmap_data = NULL;
				struct vertex *vertices;
				unsigned long *triangles;
				long triangle_count = 0, s, v;

				if (vertex_count <= 0 || material->surface_count <= 0)
					continue;
				if (material->vertices.hardware_format &&
					cache_bytes(material->vertices.hardware_format, sizeof(struct d3d_buffer)))
				{
					vertex_data = cache_bytes(material->vertices.hardware_format->data, (unsigned long)vertex_count * vertex_size);
				}
				if (lightmap_count > 0 && material->lightmap_vertices.hardware_format &&
					cache_bytes(material->lightmap_vertices.hardware_format, sizeof(struct d3d_buffer)))
				{
					lightmap_data = cache_bytes(material->lightmap_vertices.hardware_format->data,
						(unsigned long)lightmap_count * lightmap_size);
				}
				if (!vertex_data)
				{
					if (pass.count)
						warn("%s lightmap %ld material %ld: its vertices are in no buffer", tag_name(tag_index), l, m);
					continue;
				}
				vertices = allocate(vertex_count * sizeof(struct vertex));
				for (v = 0; v < vertex_count; v++)
				{
					struct vertex *vertex = &vertices[v];

					if (vertex_size == 32)
					{
						struct environment_vertex_compressed const *c = (void const *)(vertex_data + v * 32);

						memcpy(vertex->position, c->position, 12);
						uncompress_vector(c->normal, vertex->normal);
						uncompress_vector(c->binormal, vertex->binormal);
						uncompress_vector(c->tangent, vertex->tangent);
						memcpy(vertex->texcoord, c->texcoord, 8);
					}
					else
					{
						struct environment_vertex_uncompressed const *u = (void const *)(vertex_data + v * 56);

						memcpy(vertex->position, u->position, 12);
						memcpy(vertex->normal, u->normal, 12);
						memcpy(vertex->binormal, u->binormal, 12);
						memcpy(vertex->tangent, u->tangent, 12);
						memcpy(vertex->texcoord, u->texcoord, 8);
					}
					if (lightmap_data && v < lightmap_count)
					{
						if (lightmap_size == 8)
						{
							struct environment_lightmap_vertex_compressed const *c = (void const *)(lightmap_data + v * 8);

							uncompress_vector(c->incident_radiosity, vertex->radiosity);
							/* (environment_lightmap_vertex_compressed_get_texcoord) */
							vertex->texcoord1[0] = ((float)c->lightmap_u * 2.0f + 1.0f) * (1.0f / 65535.0f);
							vertex->texcoord1[1] = ((float)c->lightmap_v * 2.0f + 1.0f) * (1.0f / 65535.0f);
						}
						else
						{
							struct environment_lightmap_vertex_uncompressed const *u =
								(void const *)(lightmap_data + v * 20);

							memcpy(vertex->radiosity, u->incident_radiosity, 12);
							memcpy(vertex->texcoord1, u->texcoord, 8);
						}
					}
					vertex_finish(vertex);
				}
				triangles = allocate(material->surface_count * 3 * sizeof(unsigned long));
				for (s = 0; s < material->surface_count; s++)
				{
					struct structure_surface *surface = &surfaces[material->first_surface_index + s];

					if (surface->vertex_indices[0] >= vertex_count || surface->vertex_indices[1] >= vertex_count ||
						surface->vertex_indices[2] >= vertex_count)
					{
						continue;
					}
					triangles[triangle_count * 3] = surface->vertex_indices[0];
					triangles[triangle_count * 3 + 1] = surface->vertex_indices[1];
					triangles[triangle_count * 3 + 2] = surface->vertex_indices[2];
					triangle_count++;
				}
				if (!pass.count)
				{
					winding_count(vertices, triangles, triangle_count, &with, &against);
				}
				else if (triangle_count)
				{
					char extra[256];
					long material_index;

					snprintf(extra, sizeof(extra), "\"lightmap_index\": %ld, \"material_index\": %ld, "
						"\"lightmap_bitmap_index\": %d, \"permutation_index\": %d, \"has_lightmap_vertices\": %s",
						l, m, lightmap->bitmap_index, material->permutation_index, lightmap_data ? "true" : "false");
					material_index = gltf_material(&gltf, gltf_path, material->shader.index, extra);
					/* (its lightmap: the lightmap bitmap of the bsp's
					lightmap group, by the lightmap's index) */
					if (lightmap->bitmap_index != NONE && lightmap_group->index != NONE)
					{
						struct tag_instance *lightmaps = instance_get(lightmap_group->index);
						struct bitmap_group *bitmap_group = lightmaps ? lightmaps->base_address : NULL;

						if (bitmap_group && lightmap->bitmap_index < bitmap_group->bitmaps.count)
						{
							char base[1100];

							bitmap_png_name(lightmap_group->index, lightmap->bitmap_index, bitmap_group->bitmaps.count, base,
								sizeof(base));
							/* (recorded in the material's extras by name) */
							{
								char *last = gltf.materials + gltf.materials_length - 2;

								if (*last == '}' && last[1] == '}')
								{
									char addition[1400], escaped_base[2400];

									gltf.materials_length -= 2;
									gltf.materials[gltf.materials_length] = 0;
									snprintf(addition, sizeof(addition), ", \"lightmap_png\": \"%s.png\"}}",
										json_escaped(base, escaped_base, sizeof(escaped_base)));
									text_append(&gltf.materials, &gltf.materials_length, "%s", addition);
								}
							}
						}
					}
					gltf_primitive(&gltf, &primitives, &primitives_length, vertices, vertex_count, triangles,
						triangle_count, flip, material_index, 0, 1, primitive_count == 0);
					primitive_count++;
					vertex_total += vertex_count;
					triangle_total += triangle_count;
					material_total++;
				}
				free(vertices);
				free(triangles);
			}
		}
	}
	text_append(&gltf.meshes, &gltf.meshes_length, "\n    {\"name\": \"%s\", \"primitives\": [%s]}",
		json_escaped(tag_name(tag_index), escaped, sizeof(escaped)), primitives ? primitives : "");
	gltf.mesh_count = primitive_count ? 1 : 0;
	text_append(&gltf.nodes, &gltf.nodes_length, "\n    {\"name\": \"%s\", \"rotation\": [-0.70710678, 0, 0, 0.70710678], "
		"\"scale\": [%.9g, %.9g, %.9g]%s}", json_escaped(tag_name(tag_index), escaped, sizeof(escaped)), globals.scale,
		globals.scale, globals.scale, primitive_count ? ", \"mesh\": 0" : "");
	{
		char extra[3000], escaped_lightmaps[1200];

		snprintf(extra, sizeof(extra), "  \"extras\": {\"tag\": \"%s\", \"lightmap_group\": \"%s\", "
			"\"units\": \"world units (1 = 10 feet) times %.9g\", \"winding_flipped\": %s},\n",
			json_escaped(tag_name(tag_index), escaped, sizeof(escaped)),
			json_escaped(lightmap_group->index != NONE ? tag_name(lightmap_group->index) : "", escaped_lightmaps,
				sizeof(escaped_lightmaps)), globals.scale, flip ? "true" : "false");
		gltf_write(&gltf, relative, "0", extra);
	}
	free(primitives);

	fprintf(globals.index, "%s\n    {\"kind\": \"structure_bsp\", \"tag\": ", globals.index_first ? "" : ",");
	globals.index_first = 0;
	json_string(globals.index, tag_name(tag_index));
	fprintf(globals.index, ", \"gltf\": ");
	json_string(globals.index, gltf_path);
	fprintf(globals.index, ", \"lightmap_group\": ");
	json_string(globals.index, lightmap_group->index != NONE ? tag_name(lightmap_group->index) : "");
	fprintf(globals.index, ", \"lightmaps\": %ld, \"materials\": %ld, \"vertices\": %ld, \"triangles\": %ld, "
		"\"checks\": {\"triangles_facing_normals_device_order\": %ld, \"triangles_facing_away\": %ld, "
		"\"winding_flipped\": %s, \"surfaces\": %ld, \"surfaces_of_no_material\": %ld}}",
		lightmaps_block->count, material_total, vertex_total, triangle_total, with, against, flip ? "true" : "false",
		surfaces_block->count, orphan_surfaces);
}

/* ---------- what to write */

static void select_proc(void *context, char const *field, unsigned long group, long tag_index)
{
	int *depth = context;
	short index = (short)tag_index;

	(void)field;
	if (globals.selected[index])
		return;
	/* (an object's model and its shaders' bitmaps; not every tag an object
	names: its effects, sounds and weapons are not followed) */
	if (group == MODEL_GROUP_TAG || group == BITMAP_GROUP_TAG)
	{
		globals.selected[index] = 2;
		if (group == MODEL_GROUP_TAG && *depth < 3)
		{
			int next = *depth + 1;

			tag_walk(tag_index, NULL, select_proc, &next);
		}
	}
	else if (instance_get(tag_index) && tag_is(instance_get(tag_index), SHADER_GROUP_TAG) && *depth < 3)
	{
		int next = *depth + 1;

		globals.selected[index] = 2;
		tag_walk(tag_index, NULL, select_proc, &next);
	}
}

static int name_matches(char const *name, char **patterns, int pattern_count)
{
	int i;

	for (i = 0; i < pattern_count; i++)
	{
		if (strstr(name, patterns[i]))
			return 1;
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: map_extract [--data <data root>] [--out <folder>] [--tag <text>]... "
		"[--only bitmaps,models,bsps] [--mips] [--scale <factor>] [--list] [--verbose] <map>\n");
	exit(2);
}

int main(int argc, char **argv)
{
	char const *data = getenv("HALO_DATA_ROOT");
	char const *out = "extract";
	char const *map = NULL;
	char *patterns[64];
	int pattern_count = 0, list = 0, i;
	char path[2048];
	long index;
	char name[256];

	globals.scale = 1.0f;
	globals.want_bitmaps = globals.want_models = globals.want_bsps = 1;
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--data") && i + 1 < argc)
			data = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--tag") && i + 1 < argc && pattern_count < 64)
			patterns[pattern_count++] = argv[++i];
		else if (!strcmp(argv[i], "--only") && i + 1 < argc)
		{
			char const *kinds = argv[++i];

			globals.want_bitmaps = strstr(kinds, "bitmap") != NULL;
			globals.want_models = strstr(kinds, "model") != NULL;
			globals.want_bsps = strstr(kinds, "bsp") != NULL;
		}
		else if (!strcmp(argv[i], "--mips"))
			globals.mips = 1;
		else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
			globals.scale = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--list"))
			list = 1;
		else if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (argv[i][0] == '-' || map)
			usage();
		else
			map = argv[i];
	}
	if (!map)
		usage();
	if (!(globals.scale > 0))
		fail("the scale must be positive");

	/* the map: a path, or a name in the data root's maps */
	if (strchr(map, '/') || (strlen(map) > 4 && !strcmp(map + strlen(map) - 4, ".map")))
		snprintf(path, sizeof(path), "%s", map);
	else
		snprintf(path, sizeof(path), "%s/maps/%s.map", data ? data : ".", map);
	{
		char const *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		size_t length = strlen(base);

		if (length > 4 && !strcmp(base + length - 4, ".map"))
			length -= 4;
		if (length >= sizeof(name))
			length = sizeof(name) - 1;
		memcpy(name, base, length);
		name[length] = 0;
		globals.name = name;
	}

	globals.tag_cache = mmap((void *)TAG_CACHE_BASE_ADDRESS, TAG_CACHE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (globals.tag_cache != (unsigned char *)TAG_CACHE_BASE_ADDRESS)
		fail("the tag cache's address (%08x) is taken", TAG_CACHE_BASE_ADDRESS);
	map_load(path);

	if (list)
	{
		for (index = 0; index < globals.tags->tag_count; index++)
		{
			char group[5];

			printf("%s %s\n", group_string(globals.tags->instances[index].group_tag, group),
				globals.tags->instances[index].name ? globals.tags->instances[index].name : "");
		}
		return 0;
	}

	snprintf(globals.out, sizeof(globals.out), "%s/%s", out, name);
	globals.selected = allocate(0x10000);
	bitmaps_done = allocate(0x10000);
	for (index = 0; index < globals.tags->tag_count; index++)
	{
		struct tag_instance *instance = &globals.tags->instances[index];

		if (!pattern_count || name_matches(instance->name ? instance->name : "", patterns, pattern_count))
		{
			int depth = 0;

			globals.selected[index] = 1;
			if (pattern_count)
				tag_walk(instance->tag_index, NULL, select_proc, &depth);
		}
	}
	uses_find();

	snprintf(path, sizeof(path), "%s/index.json", globals.out);
	globals.index = open_output(path, "w");
	globals.index_first = 1;
	fprintf(globals.index, "{\n  \"map\": ");
	json_string(globals.index, name);
	fprintf(globals.index, ",\n  \"map_name\": ");
	{
		char header_name[33], build[33];

		field_string(header_name, globals.header.name, 32);
		field_string(build, globals.header.build, 32);
		json_string(globals.index, header_name);
		fprintf(globals.index, ",\n  \"build\": ");
		json_string(globals.index, build);
	}
	fprintf(globals.index, ",\n  \"cache_version\": %ld,\n  \"tag_count\": %ld,\n  \"validator_corrections\": %ld,\n",
		globals.header.version, globals.tags->tag_count, globals.corrections_tags);
	fprintf(globals.index, "  \"tool\": \"OpenCE map_extract\",\n  \"written\": [");

	if (globals.want_models)
	{
		for (index = 0; index < globals.tags->tag_count; index++)
		{
			struct tag_instance *instance = &globals.tags->instances[index];

			if (globals.selected[index] && instance->group_tag == MODEL_GROUP_TAG)
				model_export(instance->tag_index);
		}
	}
	if (globals.want_bsps)
	{
		struct tag_block *bsps = scenario_bsps();

		for (index = 0; index < bsps->count && index < MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO; index++)
		{
			struct scenario_structure_bsp_reference *reference = (struct scenario_structure_bsp_reference *)bsps->address +
				index;

			if (!pattern_count || name_matches(tag_name(reference->structure_bsp.index), patterns, pattern_count))
				bsp_export(index);
		}
	}
	if (globals.want_bitmaps)
	{
		for (index = 0; index < globals.tags->tag_count; index++)
		{
			struct tag_instance *instance = &globals.tags->instances[index];

			if (globals.selected[index] && instance->group_tag == BITMAP_GROUP_TAG)
				bitmap_export(instance->tag_index);
		}
	}
	fprintf(globals.index, "\n  ],\n  \"validator_messages\": %ld,\n  \"warnings\": %ld\n}\n", validator_messages,
		globals.errors);
	fclose(globals.index);
	printf("%s: written to %s (%ld warnings)\n", name, globals.out, globals.errors);
	return 0;
}
