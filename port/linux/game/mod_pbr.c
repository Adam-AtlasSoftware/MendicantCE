/*
MOD_PBR.C

PRISM's materials (display.prism_materials "pbr"; docs/mods.md, "Materials"):
a mod gives a model shader (a soso tag) a material, a file of its own,
materials\<the shader tag's path>.material.json in the mod's folder, whose
images are KTX2 files the host's texture store reads straight into the GPU's
memory (port/linux/vulkan/xgpu_texture_store.h: as display.prism_textures'
replacements, but replacing no bitmap). The renderer then lights the shader's
draws by a GGX path (xgpu.h's "PBR materials", xgpu_spirv.c's pbr_light):
the material's albedo in the base map's colour's place, its normal, its
occlusion, roughness and metal, its emissive, a clear coat (a visor's), a
detail tile, under the level's own lights and the shader's reflection cube;
CE's combiners still apply the shader's detail map, change colour,
self-illumination, specular mask and fog. Nothing simulated reads it.

	{
	  "format": 1,
	  "albedo": "textures/materials/cyborg/armor_albedo.ktx2",
	  "albedo_factor": [1, 1, 1],
	  "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 1.0,
	  "orm": "textures/materials/cyborg/armor_orm.ktx2",
	  "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
	  "emissive": null, "emissive_factor": [1, 1, 1],
	  "clearcoat": { "strength": 1.0, "roughness": 0.05, "mask": null },
	  "detail": { "map": "textures/materials/tiles/weave.ktx2", "scale": 24,
	              "normal": 0.4, "cavity": 0.3, "roughness": 0.2, "mask": "all" },
	  "reflection": 1.0, "specular": 0.5, "wrap": true, "direct_specular": 1.0,
	  "ce_reflection": 0.0,
	  "multipurpose": { "specular_mask": true, "self_illumination": true,
	                    "reflection_tint": false }
	}

The images' paths are the mod folder's (textures/... is its textures
folder, which the mods' scan lists): albedo (RGB, as the base map's
colours), normal (tangent space, RG: BC5), ORM (R occlusion, G roughness, B
metal, as glTF's: the factors multiply them, and are the values without
it), emissive (RGB, times its factor), the clear coat's mask (R) and the
detail tile (the overlay tiles' packing: RG the normal's slopes, B the
cavity, A the roughness offset, repeated scale times across the base map's
coordinates, on all, metal or dielectric texels). specular is the
dielectric's reflectance (F0 = 0.08 specular), wrap keeps CE's translucent
back light, ce_reflection mixes in the reflection cube's light as CE's shader
adds it (whole, not by the split sum: CE's art is lit by its cube), multipurpose says which of CE's masks still apply: the
specular mask (the multipurpose map's red) to the material's specular, the
self-illumination, and CE's reflection tint and brightness (its
perpendicular and parallel colours) on it. Unknown keys are ignored; a
material without images is lit by its factors. A shader the material
overlay names too (materials\overlay.json) takes the overlay's tiles as the
material's finest detail.

The files are read as the map's tags load, as the mods are found again and
as the setting changes (read once a frame: mod_pbr_frame), and their images
go to the host with the texture store's record (mod_textures.c), after
which each material is defined (xgpu_backend_material_define) and its id
armed around its shader's draws (mod_materials.c's draw_begin calls
mod_pbr_draw_begin). Stock (the default), a renderer without materials
(Metal's: the stream's materials capability 0) or no material file: nothing
is read or sent, and the command stream is as it was.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "cache/cache_files.h"
#include "main/console.h"
#include "tag_files/tag_groups.h"
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, as mod_materials.c's) */
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
int d3d8_materials_drawn(void);
void d3d8_set_material_pbr(unsigned long id, unsigned int flags);
void d3d8_materials_reset(void);
/* the platform layer's renderer's (port/linux/src/xgpu_backend.h, which the
game's units do not include: keep the two the same) */
void xgpu_backend_material_define(unsigned long id, unsigned int flags, unsigned long const *textures,
	float const *uniforms);

/* ---------- constants */

#define SHADER_MODEL_TAG 'soso'
#define MATERIAL_FORMAT 1
#define MAXIMUM_MATERIAL_FILE_SIZE (256UL * 1024)
#define MAXIMUM_TOKENS 1024
#define MAXIMUM_MATERIALS 512
/* (a power of two, at least twice MAXIMUM_MATERIALS) */
#define MATERIAL_TABLE_SIZE 1024
#define MAXIMUM_PATH 1024

/* xgpu.h's: the images, the uniform, the pixel key's bits */
enum
{
	_image_albedo,
	_image_normal,
	_image_orm,
	_image_emissive,
	_image_clearcoat,
	_image_detail,
	NUMBER_OF_IMAGES
};
#define UNIFORM_COUNT 9
#define PBR_ON 1u
#define PBR_IMAGE(image) (2u << (image))
#define PBR_CLEARCOAT_LOBE 128u

/* ---------- definitions */

struct json_document
{
	char const *text;
	jsmntok_t *tokens;
	int count;
};

struct material
{
	struct shader const *shader;
	/* its images' files (port_mods_file_find's, NONE none), as the store's
	queries (mod_pbr_texture_file: NONE none), and their ids (0 none) */
	long files[NUMBER_OF_IMAGES];
	short textures[NUMBER_OF_IMAGES];
	unsigned long ids[NUMBER_OF_IMAGES];
	float uniforms[UNIFORM_COUNT][4];
	/* its pixel key's bits (xgpu.h's XGPU_PBR_*), as defined */
	unsigned int flags;
	boolean clearcoat;
	/* defined on the host (its id: its index plus one) */
	boolean defined;
};

/* ---------- globals */

static struct
{
	/* display.prism_materials is "pbr" (as read: config_changes + 1) */
	unsigned long settings_read;
	boolean enabled;
	boolean read_enabled;
	boolean armed;
	struct material *materials;
	long material_count;
	short table[MATERIAL_TABLE_SIZE];
	/* the files of the images, each once (the store's queries) */
	long *files;
	long file_count;
} mod_pbr_globals;

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
		mod_pbr_globals.table[index] = NONE;
	}

	return;
}

static void table_add(
	short entry)
{
	unsigned long index = pointer_hash(mod_pbr_globals.materials[entry].shader) & (MATERIAL_TABLE_SIZE - 1);

	while (mod_pbr_globals.table[index] != NONE)
	{
		index = (index + 1) & (MATERIAL_TABLE_SIZE - 1);
	}
	mod_pbr_globals.table[index] = entry;

	return;
}

static short material_find(
	struct shader const *shader)
{
	unsigned long index;
	long probes;

	if (!mod_pbr_globals.material_count)
	{
		return NONE;
	}
	index = pointer_hash(shader) & (MATERIAL_TABLE_SIZE - 1);
	for (probes = 0; probes < MATERIAL_TABLE_SIZE; probes++)
	{
		short entry = mod_pbr_globals.table[index];

		if (entry == NONE)
		{
			return NONE;
		}
		if (mod_pbr_globals.materials[entry].shader == shader)
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
	if (mod_pbr_globals.armed)
	{
		d3d8_set_material_pbr(0, 0);
		mod_pbr_globals.armed = FALSE;
	}
	free(mod_pbr_globals.materials);
	free(mod_pbr_globals.files);
	mod_pbr_globals.materials = NULL;
	mod_pbr_globals.files = NULL;
	mod_pbr_globals.material_count = 0;
	mod_pbr_globals.file_count = 0;
	table_clear();

	return;
}

static void settings_read(
	void)
{
	char const *value;

	if (mod_pbr_globals.settings_read == config_changes() + 1)
	{
		return;
	}
	mod_pbr_globals.settings_read = config_changes() + 1;
	value = config_string("display.prism_materials");
	mod_pbr_globals.enabled = value && !strcmp(value, "pbr");

	return;
}

/* ---------- JSON (jsmn's tokens) */

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

static boolean json_boolean(
	struct json_document const *json,
	int token,
	boolean otherwise)
{
	if (token < 0 || token >= json->count || json->tokens[token].type != JSMN_PRIMITIVE)
	{
		return otherwise;
	}
	if (json->text[json->tokens[token].start] == 't')
	{
		return TRUE;
	}
	if (json->text[json->tokens[token].start] == 'f')
	{
		return FALSE;
	}

	return otherwise;
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
		error(_error_silent, "mods: %s names %s, which no mod has", material_path, path);
	}

	return file;
}

/* the material's file of a shader, read into material; FALSE if there is
none or it is not one */
static boolean material_read(
	char const *tag_name,
	struct material *material)
{
	char key[MAXIMUM_PATH];
	struct json_document json;
	jsmn_parser parser;
	unsigned long size = 0, stamp[2];
	long file;
	char *text = NULL;
	boolean read = FALSE;
	short image;

	if (snprintf(key, sizeof(key), "materials\\%s.material.json", tag_name) >= (int)sizeof(key))
	{
		return FALSE;
	}
	file = port_mods_file_find(key, mods_map_name());
	if (file == NONE)
	{
		return FALSE;
	}
	if (!port_mods_file_stamp(file, &size, stamp) || !size || size > MAXIMUM_MATERIAL_FILE_SIZE)
	{
		error(_error_silent, "mods: %s left out: it is not the size of a material", port_mods_file_path(file));
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
			error(_error_silent, "mods: %s left out: it is not JSON (or has more than %d values)", path, MAXIMUM_TOKENS);
		}
		else if (json_real(&json, json_member(&json, 0, "format"), 0.0f) != MATERIAL_FORMAT)
		{
			error(_error_silent, "mods: %s left out: it is not of format %d", path, MATERIAL_FORMAT);
		}
		else
		{
			int clearcoat = json_member(&json, 0, "clearcoat");
			int detail = json_member(&json, 0, "detail");
			int multipurpose = json_member(&json, 0, "multipurpose");
			char mask[32] = "all";

			memset(material->uniforms, 0, sizeof(material->uniforms));
			material->files[_image_albedo] = image_file(&json, json_member(&json, 0, "albedo"), path);
			material->files[_image_normal] = image_file(&json, json_member(&json, 0, "normal"), path);
			material->files[_image_orm] = image_file(&json, json_member(&json, 0, "orm"), path);
			material->files[_image_emissive] = image_file(&json, json_member(&json, 0, "emissive"), path);
			material->files[_image_clearcoat] = image_file(&json, json_member(&json, clearcoat, "mask"), path);
			material->files[_image_detail] = image_file(&json, json_member(&json, detail, "map"), path);
			/* the uniform (xgpu.h's "PBR materials"; [5] and [6] the host's) */
			material->uniforms[0][0] = material->uniforms[0][1] = material->uniforms[0][2] = 1.0f;
			json_rgb(&json, json_member(&json, 0, "albedo_factor"), material->uniforms[0]);
			material->uniforms[1][0] = material->uniforms[1][1] = material->uniforms[1][2] = 1.0f;
			json_rgb(&json, json_member(&json, 0, "emissive_factor"), material->uniforms[1]);
			material->uniforms[1][3] = json_number(&json, json_member(&json, 0, "normal_scale"), 1.0f, 0.0f, 8.0f);
			material->uniforms[2][0] = json_number(&json, json_member(&json, 0, "roughness"), 1.0f, 0.0f, 1.0f);
			material->uniforms[2][1] = json_number(&json, json_member(&json, 0, "metal"), 1.0f, 0.0f, 1.0f);
			material->uniforms[2][2] = json_number(&json, json_member(&json, 0, "occlusion"), 1.0f, 0.0f, 1.0f);
			material->uniforms[2][3] = json_number(&json, json_member(&json, 0, "reflection"), 1.0f, 0.0f, 4.0f);
			material->uniforms[3][0] = json_number(&json, json_member(&json, clearcoat, "strength"),
				clearcoat >= 0 && json.tokens[clearcoat].type == JSMN_OBJECT ? 1.0f : 0.0f, 0.0f, 1.0f);
			material->uniforms[3][1] = json_number(&json, json_member(&json, clearcoat, "roughness"), 0.05f, 0.0f, 1.0f);
			material->uniforms[3][2] = 0.08f * json_number(&json, json_member(&json, 0, "specular"), 0.5f, 0.0f, 1.0f);
			material->uniforms[3][3] = json_boolean(&json, json_member(&json, 0, "wrap"), TRUE) ? 1.0f : 0.0f;
			material->uniforms[4][0] = json_number(&json, json_member(&json, detail, "scale"), 16.0f, 0.0f, 4096.0f);
			material->uniforms[4][1] = json_number(&json, json_member(&json, detail, "normal"), 0.5f, 0.0f, 4.0f);
			material->uniforms[4][2] = json_number(&json, json_member(&json, detail, "cavity"), 0.5f, 0.0f, 4.0f);
			material->uniforms[4][3] = json_number(&json, json_member(&json, detail, "roughness"), 0.5f, 0.0f, 4.0f);
			material->uniforms[7][0] = json_boolean(&json, json_member(&json, multipurpose, "specular_mask"), TRUE) ? 1.0f : 0.0f;
			material->uniforms[7][1] = json_boolean(&json, json_member(&json, multipurpose, "self_illumination"), TRUE) ?
				1.0f : 0.0f;
			material->uniforms[7][2] = json_boolean(&json, json_member(&json, multipurpose, "reflection_tint"), FALSE) ?
				1.0f : 0.0f;
			material->uniforms[7][3] = json_number(&json, json_member(&json, 0, "direct_specular"), 1.0f, 0.0f, 8.0f);
			material->uniforms[8][3] = json_number(&json, json_member(&json, 0, "ce_reflection"), 0.0f, 0.0f, 1.0f);
			json_string(&json, json_member(&json, detail, "mask"), mask, sizeof(mask));
			material->uniforms[8][!strcmp(mask, "metal") ? 1 : !strcmp(mask, "dielectric") ? 2 : 0] = 1.0f;
			material->clearcoat = material->uniforms[3][0] > 0.0f;
			read = TRUE;
		}
	}
	free(json.tokens);
	free(text);
	for (image = 0; image < NUMBER_OF_IMAGES; image++)
	{
		material->textures[image] = NONE;
		material->ids[image] = 0;
		if (!read)
		{
			material->files[image] = NONE;
		}
	}

	return read;
}

/* a file among the images' (each once), its index; NONE if there are too
many */
static short texture_add(
	long file)
{
	long index;

	for (index = 0; index < mod_pbr_globals.file_count; index++)
	{
		if (mod_pbr_globals.files[index] == file)
		{
			return (short)index;
		}
	}
	if (mod_pbr_globals.file_count >= MAXIMUM_MATERIALS * NUMBER_OF_IMAGES)
	{
		return NONE;
	}
	mod_pbr_globals.files[mod_pbr_globals.file_count] = file;

	return (short)mod_pbr_globals.file_count++;
}

/* the map's shaders' materials read; how many */
static long materials_read(
	void)
{
	struct tag_iterator iterator;
	long tag_index;

	mod_pbr_globals.materials = malloc(MAXIMUM_MATERIALS * sizeof(*mod_pbr_globals.materials));
	mod_pbr_globals.files = malloc(MAXIMUM_MATERIALS * NUMBER_OF_IMAGES * sizeof(*mod_pbr_globals.files));
	if (!mod_pbr_globals.materials || !mod_pbr_globals.files)
	{
		materials_dispose();
		return 0;
	}
	tag_iterator_new(&iterator, SHADER_MODEL_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE && mod_pbr_globals.material_count < MAXIMUM_MATERIALS)
	{
		char const *tag_name = tag_get_name(tag_index);
		struct material *material = &mod_pbr_globals.materials[mod_pbr_globals.material_count];
		short image;

		memset(material, 0, sizeof(*material));
		if (!tag_name || !material_read(tag_name, material))
		{
			continue;
		}
		material->shader = (struct shader const *)tag_get(SHADER_MODEL_TAG, tag_index);
		for (image = 0; image < NUMBER_OF_IMAGES; image++)
		{
			if (material->files[image] != NONE)
			{
				material->textures[image] = texture_add(material->files[image]);
			}
		}
		table_add((short)mod_pbr_globals.material_count);
		mod_pbr_globals.material_count++;
	}
	if (mod_pbr_globals.material_count)
	{
		error(_error_silent, "mods: %ld materials of the map's shaders, %ld images", mod_pbr_globals.material_count,
			mod_pbr_globals.file_count);
		if (mods_say())
		{
			console_printf(FALSE, "mods: %ld materials", mod_pbr_globals.material_count);
		}
	}

	return mod_pbr_globals.material_count;
}

/* ---------- public code */

long mod_pbr_update(
	boolean remove_all,
	boolean at_load)
{
	(void)at_load;
	settings_read();
	materials_dispose();
	mod_pbr_globals.read_enabled = mod_pbr_globals.enabled;
	if (remove_all || !mod_pbr_globals.enabled || !d3d8_materials_drawn())
	{
		return 0;
	}

	return materials_read();
}

void mod_pbr_tags_unloaded(
	void)
{
	materials_dispose();

	return;
}

/* (mod_textures.c, once a frame) the setting read again; TRUE if the
materials were read again (their images to send again) */
boolean mod_pbr_frame(
	void)
{
	settings_read();
	if (mod_pbr_globals.enabled == mod_pbr_globals.read_enabled)
	{
		return FALSE;
	}
	mod_pbr_update(FALSE, FALSE);

	return TRUE;
}

/* (mod_textures.c) the images' files, for the store's record */
long mod_pbr_texture_count(
	void)
{
	return mod_pbr_globals.file_count;
}

long mod_pbr_texture_file(
	long index)
{
	return index >= 0 && index < mod_pbr_globals.file_count ? mod_pbr_globals.files[index] : NONE;
}

/* (mod_textures.c) the store's ids of the images (0 refused; ids NULL: no
store), by mod_pbr_texture_file's index; the materials defined on the host */
void mod_pbr_textures_answered(
	unsigned long const *ids,
	long count)
{
	long index;

	d3d8_materials_reset();
	mod_pbr_globals.armed = FALSE;
	if (!mod_pbr_globals.material_count)
	{
		return;
	}
	/* (the host's materials forgotten, then each defined again) */
	xgpu_backend_material_define(0, 0, NULL, NULL);
	for (index = 0; index < mod_pbr_globals.material_count; index++)
	{
		struct material *material = &mod_pbr_globals.materials[index];
		unsigned int flags = PBR_ON | (material->clearcoat ? PBR_CLEARCOAT_LOBE : 0);
		short image;

		for (image = 0; image < NUMBER_OF_IMAGES; image++)
		{
			short texture = material->textures[image];

			material->ids[image] = ids && texture != NONE && texture < count ? ids[texture] : 0;
			if (material->ids[image])
			{
				flags |= PBR_IMAGE(image);
			}
		}
		/* (the clear coat's mask without its lobe: none) */
		if (!material->clearcoat)
		{
			flags &= ~PBR_IMAGE(_image_clearcoat);
		}
		xgpu_backend_material_define((unsigned long)index + 1, flags, material->ids, material->uniforms[0]);
		material->flags = flags;
		material->defined = TRUE;
	}

	return;
}

/* (mod_materials.c's draw_begin, around a model shader's draws) its
material armed for the draws that follow, if a mod has one: TRUE if it is
(its normal and occlusion then the shader's, not a surface map's); and
disarmed */
boolean mod_pbr_draw_begin(
	struct shader const *shader)
{
	short entry;

	if (!mod_pbr_globals.material_count || !mod_pbr_globals.enabled)
	{
		return FALSE;
	}
	entry = material_find(shader);
	if (entry == NONE || !mod_pbr_globals.materials[entry].defined)
	{
		return FALSE;
	}
	d3d8_set_material_pbr((unsigned long)entry + 1, mod_pbr_globals.materials[entry].flags);
	mod_pbr_globals.armed = TRUE;

	return TRUE;
}

void mod_pbr_draw_end(
	void)
{
	if (mod_pbr_globals.armed)
	{
		d3d8_set_material_pbr(0, 0);
		mod_pbr_globals.armed = FALSE;
	}

	return;
}
