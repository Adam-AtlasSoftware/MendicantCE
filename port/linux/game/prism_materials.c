/*
PRISM_MATERIALS.C

PRISM v4's materials of the pack and mod tiers (overhaul_research/
prism_v4_renderer/design.md, section 5; docs/prism_materials.md). Every
shader of a map has a material at the automatic tier, which the host makes
from CE's tag (SCENE_MATERIAL, prism_deferred.c); where a mod or a pack gives
a shader one of PRISM's format 2, its maps are drawn instead:

	{
	  "format": 2,
	  "family": "senv",
	  "model": "opaque",
	  "base": {
	    "albedo": "textures/materials/levels/b30/shaders/sand/albedo.ktx2",
	    "albedo_factor": [1, 1, 1],
	    "normal": "textures/materials/levels/b30/shaders/sand/normal.ktx2",
	    "normal_scale": 1.0, "normal_uv": "bump",
	    "orm": "textures/materials/levels/b30/shaders/sand/orm.ktx2",
	    "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
	    "height": "textures/materials/levels/b30/shaders/sand/height.ktx2",
	    "height_scale": 0.02
	  },
	  "layers": [
	    { "slot": "primary_detail", "albedo": "textures/...", "scale": "ce", "function": "ce" },
	    { "slot": "secondary_detail", "albedo": "textures/...", "scale": "ce" },
	    { "slot": "micro_detail", "albedo": "textures/...", "scale": "ce", "function": "ce" }
	  ],
	  "type": "ce",
	  "specular": { "f0": 0.04 },
	  "emissive": { "map": null, "factor": [1, 1, 1] },
	  "translucency": 0.0,
	  "source": "pack:prism_v4_materials@1"
	}

A mod's is materials\<the shader tag's path>.material.json in its folder
(the mod tier), a pack's (prism_packs.py's v4_materials step, made on the
player's machine from the player's maps) materials\<path>.pack.json (the
pack tier); where a shader has both, the mod's is drawn, and where a mod
gives the shader display.prism_materials' material (format 1, mod_pbr.c) and
that setting is on, that one. "ce", or a value left out, takes CE's own from
the shader's tag: the environment shader's type, detail functions and its
detail maps' scales as its texture pass computes them (rescaled by the base
map's size over the detail map's where the tag says to). Images are KTX2 files
of the mod's or pack's textures folder, which the host's texture store reads
straight into the GPU's memory (KIND_MATERIAL, as mod_pbr.c's), sent with the
store's record (mod_textures.c); their ids go to the host with each material
(SCENE_MATERIAL_MAPS) after the map's SCENE_MATERIALs, and again when the
store answers again. The G-buffer pass draws a material's opaque draws by
its family's uber-shader, which samples them (xgpu_spirv.c's gbuffer_pack).

Only in the deferred modes (display.lighting "ce_deferred" or "raster"), with
display.material_packs (the default): with "ce" nothing is read or sent, and
the command stream is as it was. Nothing simulated reads a material.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "main/console.h"
#include "tag_files/tag_groups.h"
#include "mods.h"
#include "prism_deferred.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, as mod_pbr.c's) */
#undef malloc
#undef free

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"

/* the game's (port) */
char *tag_get_name(long tag_index);
/* the platform's (port/linux/src) */
char const *config_string(char const *name);
unsigned long config_changes(void);
int config_boolean(char const *name);
/* (port/linux/src/d3d8_device.c) nonzero where the renderer draws
materials: the textures' set has their images' bindings, which a pack's
uber-shader samples */
int d3d8_materials_drawn(void);
/* the platform layer's renderer's (port/linux/src/xgpu_backend.h, which the
game's units do not include: keep the two the same) */
void xgpu_backend_scene_material_maps(unsigned long material, unsigned long tier, unsigned long model,
	unsigned long flags, const unsigned long *maps, const float *scalars);

/* ---------- constants */

#define MATERIAL_FORMAT 2
#define MAXIMUM_MATERIAL_FILE_SIZE (256UL * 1024)
#define MAXIMUM_TOKENS 1024
#define MAXIMUM_MATERIALS 1024
/* (a power of two, at least twice MAXIMUM_MATERIALS) */
#define MATERIAL_TABLE_SIZE 2048
#define MAXIMUM_PATH 1024

/* xgpu_stream.h's SCENE_MATERIAL_MAPS: its maps and scalars, its tiers,
models and flags */
#define MAPS 8
#define SCALARS 24
enum
{
	_map_albedo,
	_map_normal,
	_map_orm,
	_map_primary_detail,
	_map_secondary_detail,
	_map_micro_detail,
	_map_emissive,
	_map_height
};
enum
{
	_scalar_albedo = 0,
	_scalar_normal_scale = 3,
	_scalar_roughness = 4,
	_scalar_metal = 5,
	_scalar_occlusion = 6,
	_scalar_specular = 7,
	_scalar_emissive = 8,
	_scalar_translucency = 11,
	_scalar_primary_detail = 12,
	_scalar_secondary_detail = 14,
	_scalar_micro_detail = 16,
	_scalar_height_scale = 18
};
#define TIER_AUTO 2UL
#define TIER_PACK 3UL
#define TIER_MOD 4UL
/* (XGPU_GBUFFER_MODEL_*, and the automatic tier's kept) */
#define MODEL_OPAQUE 0UL
#define MODEL_FOLIAGE 1UL
#define MODEL_CLEARCOAT 2UL
#define MODEL_SUBSURFACE 3UL
#define MODEL_EMISSIVE 4UL
#define MODEL_AUTO 15UL
#define FLAG_NORMAL_AT_BUMP 1UL
#define FLAG_FOLIAGE 2UL
#define TYPE_SHIFT 4
#define DETAIL_SHIFT 8
#define MICRO_SHIFT 12

/* the environment shader's fields (rasterizer_xbox_environment.c's struct
shader_environment_definition: the shader at 0x00, its own flags at 0x28,
its type at 0x2A, the diffuse properties from 0x6C) */
#define SENV_TYPE 0x2A
#define SENV_DIFFUSE_FLAGS 0x6C
#define SENV_BASE_MAP 0x88
#define SENV_DETAIL_FUNCTION 0xB0
#define SENV_PRIMARY_DETAIL_SCALE 0xB4
#define SENV_PRIMARY_DETAIL_MAP 0xB8
#define SENV_SECONDARY_DETAIL_SCALE 0xC8
#define SENV_SECONDARY_DETAIL_MAP 0xCC
#define SENV_MICRO_DETAIL_FUNCTION 0xF4
#define SENV_MICRO_DETAIL_SCALE 0xF8
#define SENV_MICRO_DETAIL_MAP 0xFC
/* (a tag reference's index, 12 bytes in) */
#define REFERENCE_INDEX 12
#define DIFFUSE_RESCALE_DETAIL_MAPS 1

/* ---------- definitions */

struct json_document
{
	char const *text;
	jsmntok_t *tokens;
	int count;
};

struct material
{
	void const *shader;
	unsigned long tier, model, flags;
	/* its images' files (port_mods_file_find's, NONE none), as the store's
	queries (prism_materials_texture_file's: NONE none), and their ids (0
	none) */
	long files[MAPS];
	short textures[MAPS];
	unsigned long ids[MAPS];
	float scalars[SCALARS];
};

/* ---------- globals */

static struct
{
	/* display.material_packs in a deferred mode (as read: config_changes
	+ 1), and as the materials were last read */
	unsigned long settings_read;
	boolean setting;
	boolean wanted;
	boolean read_wanted;
	struct material *materials;
	long material_count;
	short table[MATERIAL_TABLE_SIZE];
	/* the images' files, each once (the store's queries) */
	long *files;
	long file_count;
	/* moved as the store's answers come (the maps sent again) */
	unsigned long generation;
	long pack_count;
	long mod_count;
} prism_materials_globals;

/* ---------- private code */

static unsigned long pointer_hash(
	void const *pointer)
{
	unsigned long value = (unsigned long)(size_t)pointer;

	return (value >> 4) * 2654435761UL;
}

static void table_clear(
	void)
{
	long index;

	for (index = 0; index < MATERIAL_TABLE_SIZE; index++)
	{
		prism_materials_globals.table[index] = NONE;
	}

	return;
}

static void table_add(
	short entry)
{
	unsigned long index = pointer_hash(prism_materials_globals.materials[entry].shader) & (MATERIAL_TABLE_SIZE - 1);

	while (prism_materials_globals.table[index] != NONE)
	{
		index = (index + 1) & (MATERIAL_TABLE_SIZE - 1);
	}
	prism_materials_globals.table[index] = entry;

	return;
}

static short material_find(
	void const *shader)
{
	unsigned long index;
	long probes;

	if (!prism_materials_globals.material_count || !shader)
	{
		return NONE;
	}
	index = pointer_hash(shader) & (MATERIAL_TABLE_SIZE - 1);
	for (probes = 0; probes < MATERIAL_TABLE_SIZE; probes++)
	{
		short entry = prism_materials_globals.table[index];

		if (entry == NONE)
		{
			return NONE;
		}
		if (prism_materials_globals.materials[entry].shader == shader)
		{
			return entry;
		}
		index = (index + 1) & (MATERIAL_TABLE_SIZE - 1);
	}

	return NONE;
}

static void materials_dispose(
	void)
{
	free(prism_materials_globals.materials);
	free(prism_materials_globals.files);
	prism_materials_globals.materials = NULL;
	prism_materials_globals.files = NULL;
	prism_materials_globals.material_count = 0;
	prism_materials_globals.file_count = 0;
	prism_materials_globals.pack_count = 0;
	prism_materials_globals.mod_count = 0;
	table_clear();

	return;
}

static void settings_read(
	void)
{
	if (prism_materials_globals.settings_read != config_changes() + 1)
	{
		prism_materials_globals.settings_read = config_changes() + 1;
		prism_materials_globals.setting = prism_deferred_wanted() && config_boolean("display.material_packs");
	}
	/* (only where the renderer draws materials: without the textures' set's
	material bindings, a device without block compression or with
	HALO_MATERIALS_OFF, no pack's images are queued and no maps sent, every
	shader at the automatic tier) */
	prism_materials_globals.wanted = prism_materials_globals.setting && d3d8_materials_drawn();

	return;
}

/* ---------- JSON (jsmn's tokens, as mod_pbr.c reads them) */

/* the token after a value and everything in it */
static int json_skip(
	struct json_document const *json,
	int token)
{
	int end = json->tokens[token].end;

	token++;
	while (token < json->count && json->tokens[token].start < end)
	{
		token++;
	}

	return token;
}

/* a string token's text (\\ \" \/ unescaped), into text; FALSE if it is not
one or does not fit */
static boolean json_string(
	struct json_document const *json,
	int token,
	char *text,
	unsigned long size)
{
	char const *character;
	char const *end;
	unsigned long length = 0;

	if (token < 0 || token >= json->count || json->tokens[token].type != JSMN_STRING)
	{
		return FALSE;
	}
	character = json->text + json->tokens[token].start;
	end = json->text + json->tokens[token].end;
	while (character < end)
	{
		char value = *character++;

		if (value == '\\' && character < end)
		{
			value = *character++;
			if (value != '\\' && value != '"' && value != '/')
			{
				return FALSE;
			}
		}
		if (length + 1 >= size)
		{
			return FALSE;
		}
		text[length++] = value;
	}
	text[length] = 0;

	return TRUE;
}

/* the value of an object's member, or NONE */
static int json_member(
	struct json_document const *json,
	int object,
	char const *name)
{
	int token;
	int member;

	if (object < 0 || object >= json->count || json->tokens[object].type != JSMN_OBJECT)
	{
		return NONE;
	}
	token = object + 1;
	for (member = 0; member < json->tokens[object].size && token + 1 < json->count; member++)
	{
		char key[64];

		if (json_string(json, token, key, sizeof(key)) && !strcmp(key, name))
		{
			return token + 1;
		}
		token = json_skip(json, token + 1);
	}

	return NONE;
}

/* a number, or `otherwise` */
static real json_real(
	struct json_document const *json,
	int token,
	real otherwise)
{
	char text[64];
	char *end;
	double value;
	int length;

	if (token < 0 || token >= json->count || json->tokens[token].type != JSMN_PRIMITIVE)
	{
		return otherwise;
	}
	length = json->tokens[token].end - json->tokens[token].start;
	if (length <= 0 || length >= (int)sizeof(text))
	{
		return otherwise;
	}
	memcpy(text, json->text + json->tokens[token].start, (size_t)length);
	text[length] = 0;
	value = strtod(text, &end);
	if (*end || !mods_finite_real((real)value))
	{
		return otherwise;
	}

	return (real)value;
}

/* a number in lo to hi, or `otherwise` */
static float json_number(
	struct json_document const *json,
	int token,
	real otherwise,
	real lo,
	real hi)
{
	return (float)PIN(json_real(json, token, otherwise), lo, hi);
}

/* an array of three numbers (0 to 64), into rgb; else as it was */
static void json_rgb(
	struct json_document const *json,
	int token,
	float *rgb)
{
	int index;

	if (token < 0 || token >= json->count || json->tokens[token].type != JSMN_ARRAY || json->tokens[token].size < 3)
	{
		return;
	}
	for (index = 0; index < 3; index++)
	{
		rgb[index] = json_number(json, token + 1 + index, rgb[index], 0.0f, 64.0f);
	}

	return;
}

/* a string member's text compared with `word` */
static boolean json_is(
	struct json_document const *json,
	int token,
	char const *word)
{
	char text[64];

	return json_string(json, token, text, sizeof(text)) && !strcmp(text, word);
}

/* ---------- the shader's own values ("ce") */

static short field_short(
	void const *shader,
	long offset)
{
	short value;

	memcpy(&value, (char const *)shader + offset, sizeof(value));
	return value;
}

static float field_real(
	void const *shader,
	long offset)
{
	float value;

	memcpy(&value, (char const *)shader + offset, sizeof(value));
	return value;
}

/* a bitmap reference's first bitmap's size; FALSE if it has none */
static boolean reference_size(
	void const *shader,
	long offset,
	short *width,
	short *height)
{
	long index;
	struct bitmap_group const *group;
	struct bitmap_data const *bitmap;

	memcpy(&index, (char const *)shader + offset + REFERENCE_INDEX, sizeof(index));
	if (index == NONE || !(group = bitmap_group_get(index)) || group->bitmaps.count <= 0)
	{
		return FALSE;
	}
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (bitmap->width <= 0 || bitmap->height <= 0)
	{
		return FALSE;
	}
	*width = bitmap->width;
	*height = bitmap->height;

	return TRUE;
}

/* a detail map's scale over the base map's as the texture pass computes it
(its scale, times the base map's size over the detail map's where the tag
says to rescale), u and v */
static void detail_scale(
	void const *shader,
	long scale_offset,
	long map_offset,
	float scale[2])
{
	float value = field_real(shader, scale_offset);
	short base_width, base_height, width, height;

	scale[0] = scale[1] = value;
	if ((field_short(shader, SENV_DIFFUSE_FLAGS) & DIFFUSE_RESCALE_DETAIL_MAPS) &&
		reference_size(shader, SENV_BASE_MAP, &base_width, &base_height) &&
		reference_size(shader, map_offset, &width, &height))
	{
		scale[0] = value * (float)base_width / (float)width;
		scale[1] = value * (float)base_height / (float)height;
	}

	return;
}

/* ---------- the materials' files */

/* an image's file, a path of the mod folder's (either slash), found as the
mods' scan lists it (the mod loaded last that has it); NONE none */
static long image_file(
	struct json_document const *json,
	int token,
	char const *material_path)
{
	char path[MAXIMUM_PATH];
	long file;
	char *character;

	if (token < 0 || !json_string(json, token, path, sizeof(path)) || !path[0])
	{
		return NONE;
	}
	for (character = path; *character; character++)
	{
		if (*character == '/')
		{
			*character = '\\';
		}
	}
	file = port_mods_file_find(path, mods_map_name());
	if (file == NONE)
	{
		error(_error_silent, "materials: %s names %s, which no mod has", material_path, path);
	}

	return file;
}

/* a layer's slot ("primary_detail", "secondary_detail", "micro_detail"):
its map; NONE another */
static short layer_map(
	struct json_document const *json,
	int layer)
{
	int slot = json_member(json, layer, "slot");

	return json_is(json, slot, "primary_detail") ? _map_primary_detail :
		json_is(json, slot, "secondary_detail") ? _map_secondary_detail :
		json_is(json, slot, "micro_detail") ? _map_micro_detail : NONE;
}

/* a detail function, CE's numbering ("ce": the tag's) */
static unsigned long detail_function(
	struct json_document const *json,
	int token,
	unsigned long ce)
{
	return json_is(json, token, "double_biased_multiply") ? 0 : json_is(json, token, "multiply") ? 1 :
		json_is(json, token, "double_biased_add") ? 2 : ce;
}

/* the format 2 file of a material, read into material (its tier given);
FALSE if it is not one */
static boolean material_parse(
	struct json_document const *json,
	char const *path,
	void const *shader,
	boolean environment,
	struct material *material)
{
	int base = json_member(json, 0, "base");
	int layers = json_member(json, 0, "layers");
	int specular = json_member(json, 0, "specular");
	int emissive = json_member(json, 0, "emissive");
	int model = json_member(json, 0, "model");
	int type = json_member(json, 0, "type");
	unsigned long ce_type = 0, ce_detail = 0, ce_micro = 0;
	unsigned long detail = 0, micro = 0, kind = 0;
	short image;

	if (json_real(json, json_member(json, 0, "format"), 0.0f) != MATERIAL_FORMAT)
	{
		error(_error_silent, "materials: %s left out: it is not of format %d", path, MATERIAL_FORMAT);
		return FALSE;
	}
	if (base < 0 || json->tokens[base].type != JSMN_OBJECT)
	{
		error(_error_silent, "materials: %s left out: it has no base", path);
		return FALSE;
	}
	for (image = 0; image < MAPS; image++)
	{
		material->files[image] = NONE;
		material->textures[image] = NONE;
		material->ids[image] = 0;
	}
	memset(material->scalars, 0, sizeof(material->scalars));
	material->files[_map_albedo] = image_file(json, json_member(json, base, "albedo"), path);
	material->files[_map_normal] = image_file(json, json_member(json, base, "normal"), path);
	material->files[_map_orm] = image_file(json, json_member(json, base, "orm"), path);
	material->files[_map_height] = image_file(json, json_member(json, base, "height"), path);
	material->files[_map_emissive] = image_file(json, json_member(json, emissive, "map"), path);
	material->scalars[_scalar_albedo] = material->scalars[_scalar_albedo + 1] = material->scalars[_scalar_albedo + 2] =
		1.0f;
	json_rgb(json, json_member(json, base, "albedo_factor"), material->scalars + _scalar_albedo);
	material->scalars[_scalar_normal_scale] = json_number(json, json_member(json, base, "normal_scale"), 1.0f, 0.0f,
		8.0f);
	material->scalars[_scalar_roughness] = json_number(json, json_member(json, base, "roughness"), 1.0f, 0.0f, 1.0f);
	material->scalars[_scalar_metal] = json_number(json, json_member(json, base, "metal"), 1.0f, 0.0f, 1.0f);
	material->scalars[_scalar_occlusion] = json_number(json, json_member(json, base, "occlusion"), 1.0f, 0.0f, 1.0f);
	/* (F0, the dielectric's reflectance, as the G-buffer's specular: 0.5
	is 4%) */
	material->scalars[_scalar_specular] = json_number(json, json_member(json, specular, "f0"), 0.04f, 0.0f, 0.08f) /
		0.08f;
	material->scalars[_scalar_emissive] = material->scalars[_scalar_emissive + 1] =
		material->scalars[_scalar_emissive + 2] = 1.0f;
	json_rgb(json, json_member(json, emissive, "factor"), material->scalars + _scalar_emissive);
	material->scalars[_scalar_translucency] = json_number(json, json_member(json, 0, "translucency"), 0.0f, 0.0f,
		1.0f);
	material->scalars[_scalar_height_scale] = json_number(json, json_member(json, base, "height_scale"), 0.0f, 0.0f,
		16.0f);
	material->flags = json_is(json, json_member(json, base, "normal_uv"), "bump") ? FLAG_NORMAL_AT_BUMP : 0;
	material->model = json_is(json, model, "opaque") || json_is(json, model, "alpha_tested") ? MODEL_OPAQUE :
		json_is(json, model, "foliage") ? MODEL_FOLIAGE : json_is(json, model, "clearcoat") ? MODEL_CLEARCOAT :
		json_is(json, model, "subsurface") ? MODEL_SUBSURFACE : json_is(json, model, "emissive") ? MODEL_EMISSIVE :
		MODEL_AUTO;
	if (material->model == MODEL_FOLIAGE)
	{
		material->flags |= FLAG_FOLIAGE;
	}
	/* the environment shader's layers: its detail maps, CE's functions and
	scales where the file says "ce" or nothing */
	if (environment)
	{
		ce_type = (unsigned long)field_short(shader, SENV_TYPE) & 3;
		ce_detail = (unsigned long)field_short(shader, SENV_DETAIL_FUNCTION) & 3;
		ce_micro = (unsigned long)field_short(shader, SENV_MICRO_DETAIL_FUNCTION) & 3;
		detail_scale(shader, SENV_PRIMARY_DETAIL_SCALE, SENV_PRIMARY_DETAIL_MAP,
			material->scalars + _scalar_primary_detail);
		detail_scale(shader, SENV_SECONDARY_DETAIL_SCALE, SENV_SECONDARY_DETAIL_MAP,
			material->scalars + _scalar_secondary_detail);
		detail_scale(shader, SENV_MICRO_DETAIL_SCALE, SENV_MICRO_DETAIL_MAP, material->scalars + _scalar_micro_detail);
		kind = json_is(json, type, "normal") ? 0 : json_is(json, type, "blended") ? 1 :
			json_is(json, type, "blended_base_specular") ? 2 : ce_type;
		detail = ce_detail;
		micro = ce_micro;
	}
	if (environment && layers >= 0 && json->tokens[layers].type == JSMN_ARRAY)
	{
		int layer = layers + 1;
		int index;

		for (index = 0; index < json->tokens[layers].size && layer < json->count; index++)
		{
			short map = layer_map(json, layer);

			if (map != NONE)
			{
				int scale = json_member(json, layer, "scale");
				float *scalars = material->scalars + (map == _map_primary_detail ? _scalar_primary_detail :
					map == _map_secondary_detail ? _scalar_secondary_detail : _scalar_micro_detail);

				material->files[map] = image_file(json, json_member(json, layer, "albedo"), path);
				/* (a number, or [u, v]; else CE's) */
				if (scale >= 0 && json->tokens[scale].type == JSMN_PRIMITIVE)
				{
					scalars[0] = scalars[1] = json_number(json, scale, scalars[0], 0.0f, 4096.0f);
				}
				else if (scale >= 0 && json->tokens[scale].type == JSMN_ARRAY && json->tokens[scale].size >= 2)
				{
					scalars[0] = json_number(json, scale + 1, scalars[0], 0.0f, 4096.0f);
					scalars[1] = json_number(json, scale + 2, scalars[1], 0.0f, 4096.0f);
				}
				if (map == _map_primary_detail)
				{
					detail = detail_function(json, json_member(json, layer, "function"), ce_detail);
				}
				else if (map == _map_micro_detail)
				{
					micro = detail_function(json, json_member(json, layer, "function"), ce_micro);
				}
			}
			layer = json_skip(json, layer);
		}
	}
	material->flags |= (kind << TYPE_SHIFT) | (detail << DETAIL_SHIFT) | (micro << MICRO_SHIFT);

	return TRUE;
}

/* a material's file (its key), read and parsed; FALSE if there is none or
it is not one of format 2 (*format: the file's format, 0 none) */
static boolean material_read(
	char const *key,
	void const *shader,
	boolean environment,
	struct material *material,
	int *format)
{
	struct json_document json;
	jsmn_parser parser;
	unsigned long size = 0, stamp[2];
	long file;
	char *text = NULL;
	boolean read = FALSE;

	*format = 0;
	file = port_mods_file_find(key, mods_map_name());
	if (file == NONE)
	{
		return FALSE;
	}
	if (!port_mods_file_stamp(file, &size, stamp) || !size || size > MAXIMUM_MATERIAL_FILE_SIZE)
	{
		error(_error_silent, "materials: %s left out: it is not the size of a material", port_mods_file_path(file));
		return FALSE;
	}
	text = malloc(size + 1);
	json.tokens = malloc(MAXIMUM_TOKENS * sizeof(jsmntok_t));
	if (text && json.tokens && port_mods_file_read(file, 0, text, size))
	{
		char const *path = port_mods_file_path(file);

		text[size] = 0;
		json.text = text;
		jsmn_init(&parser);
		json.count = jsmn_parse(&parser, text, size, json.tokens, MAXIMUM_TOKENS);
		if (json.count <= 0 || json.tokens[0].type != JSMN_OBJECT)
		{
			error(_error_silent, "materials: %s left out: it is not JSON (or has more than %d values)", path,
				MAXIMUM_TOKENS);
		}
		else
		{
			*format = (int)json_real(&json, json_member(&json, 0, "format"), 0.0f);
			/* (format 1: display.prism_materials', mod_pbr.c's) */
			if (*format != 1)
			{
				read = material_parse(&json, path, shader, environment, material);
			}
		}
	}
	free(json.tokens);
	free(text);

	return read;
}

/* a file among the images' (each once), its index; NONE if there are too
many */
static short texture_add(
	long file)
{
	long index;

	for (index = 0; index < prism_materials_globals.file_count; index++)
	{
		if (prism_materials_globals.files[index] == file)
		{
			return (short)index;
		}
	}
	if (prism_materials_globals.file_count >= MAXIMUM_MATERIALS * MAPS)
	{
		return NONE;
	}
	prism_materials_globals.files[prism_materials_globals.file_count] = file;

	return (short)prism_materials_globals.file_count++;
}

/* a shader's material of the mod or the pack tier, read; FALSE if it has
none */
static boolean shader_material(
	long group_tag,
	long tag_index,
	struct material *material)
{
	char key[MAXIMUM_PATH];
	char const *tag_name = tag_get_name(tag_index);
	void const *shader = tag_get(group_tag, tag_index);
	boolean environment = group_tag == 'senv';
	char const *pbr = config_string("display.prism_materials");
	int format;

	if (!tag_name || !shader)
	{
		return FALSE;
	}
	memset(material, 0, sizeof(*material));
	material->shader = shader;
	/* the mod's own, of format 2 (format 1 is display.prism_materials',
	mod_pbr.c's: that one is drawn where its setting is on) */
	if (snprintf(key, sizeof(key), "materials\\%s.material.json", tag_name) >= (int)sizeof(key))
	{
		return FALSE;
	}
	if (material_read(key, shader, environment, material, &format))
	{
		material->tier = TIER_MOD;
		return TRUE;
	}
	if (format == 1 && pbr && !strcmp(pbr, "pbr"))
	{
		return FALSE;
	}
	/* the pack's */
	if (snprintf(key, sizeof(key), "materials\\%s.pack.json", tag_name) >= (int)sizeof(key))
	{
		return FALSE;
	}
	memset(material, 0, sizeof(*material));
	material->shader = shader;
	if (material_read(key, shader, environment, material, &format))
	{
		material->tier = TIER_PACK;
		return TRUE;
	}

	return FALSE;
}

/* the map's shaders' materials read; how many */
static long materials_read(
	void)
{
	static long const groups[] = { 'senv', 'soso' };
	struct tag_iterator iterator;
	long tag_index;
	int group;

	prism_materials_globals.materials = malloc(MAXIMUM_MATERIALS * sizeof(*prism_materials_globals.materials));
	prism_materials_globals.files = malloc(MAXIMUM_MATERIALS * MAPS * sizeof(*prism_materials_globals.files));
	if (!prism_materials_globals.materials || !prism_materials_globals.files)
	{
		materials_dispose();
		return 0;
	}
	for (group = 0; group < (int)(sizeof(groups) / sizeof(groups[0])); group++)
	{
		tag_iterator_new(&iterator, groups[group]);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE &&
			prism_materials_globals.material_count < MAXIMUM_MATERIALS)
		{
			struct material *material = &prism_materials_globals.materials[prism_materials_globals.material_count];
			short image;

			if (!shader_material(groups[group], tag_index, material))
			{
				continue;
			}
			for (image = 0; image < MAPS; image++)
			{
				if (material->files[image] != NONE)
				{
					material->textures[image] = texture_add(material->files[image]);
				}
			}
			if (material->tier == TIER_PACK)
			{
				prism_materials_globals.pack_count++;
			}
			else
			{
				prism_materials_globals.mod_count++;
			}
			table_add((short)prism_materials_globals.material_count);
			prism_materials_globals.material_count++;
		}
	}
	if (prism_materials_globals.material_count)
	{
		error(_error_silent, "materials: %ld of the map's shaders have PRISM materials (%ld a pack's, %ld a mod's), "
			"%ld images", prism_materials_globals.material_count, prism_materials_globals.pack_count,
			prism_materials_globals.mod_count, prism_materials_globals.file_count);
		if (mods_say())
		{
			console_printf(FALSE, "materials: %ld from packs, %ld from mods", prism_materials_globals.pack_count,
				prism_materials_globals.mod_count);
		}
	}

	return prism_materials_globals.material_count;
}

/* ---------- public code */

long prism_materials_update(
	boolean remove_all,
	boolean at_load)
{
	(void)at_load;
	settings_read();
	materials_dispose();
	prism_materials_globals.read_wanted = prism_materials_globals.wanted;
	prism_materials_globals.generation++;
	if (remove_all || !prism_materials_globals.wanted)
	{
		return 0;
	}

	return materials_read();
}

void prism_materials_tags_unloaded(
	void)
{
	materials_dispose();
	prism_materials_globals.generation++;

	return;
}

/* (mod_textures.c, once a frame) the settings read again; TRUE if the
materials were read again (their images to send again) */
boolean prism_materials_frame(
	void)
{
	settings_read();
	if (prism_materials_globals.wanted == prism_materials_globals.read_wanted)
	{
		return FALSE;
	}
	prism_materials_update(FALSE, FALSE);

	return TRUE;
}

/* (mod_textures.c) the images' files, for the store's record */
long prism_materials_texture_count(
	void)
{
	return prism_materials_globals.file_count;
}

long prism_materials_texture_file(
	long index)
{
	return index >= 0 && index < prism_materials_globals.file_count ? prism_materials_globals.files[index] : NONE;
}

/* (mod_textures.c) the store's ids of the images (0 refused; ids NULL: no
store), by prism_materials_texture_file's index */
void prism_materials_textures_answered(
	unsigned long const *ids,
	long count)
{
	long index;

	for (index = 0; index < prism_materials_globals.material_count; index++)
	{
		struct material *material = &prism_materials_globals.materials[index];
		short image;

		for (image = 0; image < MAPS; image++)
		{
			short texture = material->textures[image];

			material->ids[image] = ids && texture != NONE && texture < count ? ids[texture] : 0;
		}
	}
	/* (no materials read, none to send again: with the packs off, a later
	store record sends no maps) */
	if (prism_materials_globals.material_count)
	{
		prism_materials_globals.generation++;
	}

	return;
}

/* (prism_deferred.c) moves as the materials are read and answered: their
maps to send again */
unsigned long prism_materials_generation(
	void)
{
	return prism_materials_globals.generation;
}

/* (prism_deferred.c, after a shader's SCENE_MATERIAL) its maps, where a
pack or a mod gives it a material; else, where `forget`, the automatic
tier's (any maps the host had of it forgotten) */
void prism_materials_scene(
	void const *shader,
	unsigned long material_id,
	boolean forget)
{
	short entry = material_find(shader);
	struct material const *material;

	if (!material_id)
	{
		return;
	}
	if (entry == NONE)
	{
		if (forget)
		{
			xgpu_backend_scene_material_maps(material_id, TIER_AUTO, MODEL_AUTO, 0, NULL, NULL);
		}
		return;
	}
	material = &prism_materials_globals.materials[entry];
	xgpu_backend_scene_material_maps(material_id, material->tier, material->model, material->flags, material->ids,
		material->scalars);

	return;
}

/* (the console's prism_set "material_tiers list") the map's environment and
model shaders by their materials' tiers: how many of each at the console,
each shader's in the log */
void prism_materials_describe(
	void)
{
	static long const groups[] = { 'senv', 'soso' };
	static char const *const names[] = { "automatic", "pack", "mod" };
	struct tag_iterator iterator;
	long tag_index, counts[3] = { 0, 0, 0 };
	char const *pbr = config_string("display.prism_materials");
	int group;

	settings_read();
	for (group = 0; group < (int)(sizeof(groups) / sizeof(groups[0])); group++)
	{
		tag_iterator_new(&iterator, groups[group]);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			char const *tag_name = tag_get_name(tag_index);
			short entry = material_find(tag_get(groups[group], tag_index));
			int tier = entry == NONE ? 0 : prism_materials_globals.materials[entry].tier == TIER_PACK ? 1 : 2;
			char key[MAXIMUM_PATH];

			/* (a model shader's display.prism_materials material, mod_pbr.c's:
			the mod's tier where that is on) */
			if (!tier && groups[group] == 'soso' && pbr && !strcmp(pbr, "pbr") && tag_name &&
				snprintf(key, sizeof(key), "materials\\%s.material.json", tag_name) < (int)sizeof(key) &&
				port_mods_file_find(key, mods_map_name()) != NONE)
			{
				tier = 2;
			}
			counts[tier]++;
			error(_error_silent, "materials: %s %s", names[tier], tag_name ? tag_name : "?");
		}
	}
	console_printf(FALSE, "materials (%s): %ld environment and model shaders, %ld from packs, %ld from mods, %ld at "
		"the automatic tier (each in the log)", prism_materials_globals.wanted ? "on" :
		prism_materials_globals.setting ? "off: the renderer draws no materials" :
		"off: display.lighting \"ce\" or display.material_packs false", counts[0] + counts[1] + counts[2], counts[1],
		counts[2], counts[0]);

	return;
}
