/*
MOD_MATERIALS.C

The material overlay of mods (mods.c): display.prism_material_overlay. A mod
names model shaders (soso tags, by their paths) in its
tags\materials\overlay.json, and gives each a micro normal, cavity and
roughness from tiles of its own (tags\materials\...\name.png), which the
renderer lights the shader's draws with for each pixel (xgpu_spirv.c's
material overlay, the Vulkan renderer's: xgpu.h's XGPU_OVERLAY_*): the fine grain of a surface (a weapon's
machining, a suit's weave) where the map's textures have none. It adds no
color: the base map, the detail map, the change color and the
self-illumination are the shader's own, and only the light on them changes.
Nothing simulated reads it, as nothing reads what a mod's bitmaps draw.

overlay.json, of the mod loaded last that has one (port_mods_file_find):

	{
	  "format": 1,
	  "classes": {
	    "gun_metal": { "tile": "tiles/gun_metal.png", "tile_wu": 0.02,
	                   "levels": { "normal": "tiles/gun_metal_n.ktx2",
	                               "cavity_rough": "tiles/gun_metal_cr.ktx2" },
	                   "normal": 1.0, "cavity": 0.5, "rough": 0.5,
	                   "fade": [1.0, 3.0], "footprint": [32, 8] }
	  },
	  "shaders": {
	    "weapons\\pistol\\fp\\shaders\\silver": {
	      "slots": ["gun_metal", "gun_polymer"], "uv_wu": 0.12,
	      "mask": { "channel": "r", "lo": 0.3, "hi": 0.6, "invert": false },
	      "exclude": { "channel": "g", "above": 0.1 },
	      "strength": 1.0, "reflection": { "bend": 1.0, "cavity": 0.5 } },
	    "characters\\cyborg\\shaders\\armor": {
	      "slots": ["plate", "undersuit", "hard_polymer", "rubber_grip"],
	      "mask": { "channel": "b", "lo": 0.35, "hi": 0.65 },
	      "class_mask": ["masks/characters_cyborg_shaders_armor_0.png"],
	      "class_mask_wins": false, ... },
	    "characters\\cyborg\\shaders\\visor": null
	  }
	}

A class is a tile, an RGBA PNG of 8 bits a channel (RG the tangent-space
normal's x and y, y up the picture, 0.5 none; B the cavity,
half of it, 0.5 none; A the roughness offset, 0.5 none: the packing
called packed_v1), its size (tile_wu, world units across: 1 is 10
feet), the strengths of its normal, cavity and roughness, the camera
distances in world units over which it fades out (fade) and the sizes of
a tile on the screen, in pixels, over which it fades out as it shrinks
(footprint: from 32 to 8 unless it says). Its levels, if it gives them,
are the tile's every mip level as the tools filtered them (two KTX2s of
BC5 blocks, one face, no supercompression, down to 1 by 1: the normal's x
and y; the cavity, half of it, and the roughness offset, which grows in the
smaller levels where the normals average out); otherwise, or if they
cannot be read, the PNG's smaller levels are averages of its texels. A shader names one or two
classes (slots): tile A where the multipurpose map's channel (r specular, g self-illumination, b change
color, a auxiliary: the Xbox's order) is above hi, tile B below lo, mixed
between; invert turns it around. Its base map's coordinates cover uv_wu
world units each (the tiles' scale is uv_wu / tile_wu tiles to the
coordinates' 1; or uv_scale, given, which wins), texels whose channel is
above exclude's are left alone (self-illuminated lights), strength scales
it, and the reflection cube's direction is bent by the micro normal (bend)
and darkened by the cavity (cavity). A shader without a multipurpose map
has tile A everywhere. null leaves a shader out. Unknown keys are ignored.

A shader may name up to four classes (A to D) and a class mask for each of
its base map's bitmaps (class_mask: one PNG, or an array by the bitmap's
index): an RGBA PNG at the base map's coordinates whose R G B A are the
weights of slots A B C D (art_pipeline/class_masks.py builds them on the
player's machine from the player's own maps, so a mod names them but does
not carry them: they are found in whichever mod has them). With its mask,
the channel rule (mask) decides slot A, as above, and the mask's other
slots share the rest in their proportions (B alone where they are all 0);
without a channel rule the mask's four weights are the classes. With
"class_mask_wins": true the mask's own slots A, C and D are kept where it
gives them, and the channel rule decides between A and B over the mask's B
alone: plate the mask names stays plate where the channel dips (a scratch
CE paints into the multipurpose map), and a class C or D the mask names (a
buckle's metal inside the plate) is not taken by the rule's A. A shader
whose class mask is not there (not built yet, or another map's), or not
ready yet, draws with slots A and B and the channel rule alone, as a shader
of two classes does.

PRISM v2 surface maps (display.prism_surface_maps, off unless set): an
RGBA PNG at a base map's coordinates (RG the normal's x and y, y up the
picture; B the ambient occlusion; A the roughness, 1 matte and without a
highlight), baked from a remodel's high-poly onto its game mesh at CE's own
UVs (art_pipeline/prism_bake_ce.py, prism_surface_pack.py: built on the
player's machine from the player's maps). A mod gives one for a bitmap by
putting it next to the bitmap's tag: tags\<bitmap>.surface.png for the
bitmap's first picture, tags\<bitmap>.surface<n>.png for its picture n.
Every model shader whose base map that is draws with it, whether or not
overlay.json names the shader and whether or not the overlay is on: with
no tiles the surface map lights the draw alone (xgpu.h's
XGPU_OVERLAY_SURFACE_ONLY), at the strengths below. overlay.json may still
name a shader's surface maps itself ("surface_map": one PNG or an array by
the bitmap's index, under tags\materials\, which wins) and its strengths
("surface": { "normal": 1.0, "occlusion": 1.0, "specular": 0.5,
"exponent": 48 }, also the defaults): xgpu.h's "The surface map".
Its "specular" and "exponent" were the prototype's highlight's; PRISM v2's
highlights are display.prism_specular's now (prism_light.c).

A surface map is decoded on a thread of its own when a draw first needs
it, its levels box-filtered and each encoded as two BC5 images (the
normal's x and y; the occlusion and roughness): a byte a texel for both,
so a 4096 by 4096 map is 22 MB with its levels (an RGBA image of it, 85
MB, is past what the renderer uploads at once: 32 MB a level). The draws
go without it until it is ready.

The overlay is taken up when the map's tags load and mods_reload; its tiles
are read then (or when it is first turned on) and decoded on a thread of
their own, a draw going without the overlay until its tiles are ready; the
settings are read again each
frame they change (display.prism_material_overlay,
display.prism_overlay_strength, display.prism_overlay_view), so the overlay
can be turned on and off in a stopped game (prism_set). Off, no draw is
armed, nothing is made for it, and every draw is the game's.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "cache/cache_files.h"
#include "main/console.h"
#include "tag_files/tag_groups.h"
#include "bitmaps/bitmap_group.h"
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, not cseries.h's checked one: the tiles are
decoded on a thread, and hud_hires_png_pixels' texels are the library's; the
images made of them free them with the library's free: xgpu_images.c) */
#undef malloc
#undef free

/* (the tiles decoded on a thread of their own where there are threads) */
#if !defined(HALO_ANDROID)
#define TILES_DECODE_AHEAD
#include <pthread.h>
#endif

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"

/* the game's (port) */
char *tag_get_name(long tag_index);
/* the platform's (port/linux/src) */
int config_boolean(char const *name);
long config_integer(char const *name);
double config_real(char const *name);
int config_write(char const *name, char const *value);
unsigned long config_changes(void);
unsigned long system_milliseconds(void);
unsigned char *hud_hires_png_pixels(void const *png, unsigned long size, unsigned long *width, unsigned long *height);
unsigned int xgpu_image_create(void *pixels, unsigned long width, unsigned long height, unsigned long levels,
	int format, int owned);
void xgpu_image_delete(unsigned int image);
int d3d8_material_overlay_drawn(void);
void d3d8_set_material_overlay(float const *uniforms, unsigned int const *tiles, unsigned int mask, int view, int mask_wins,
	unsigned int const *surface, int surface_only, int prism_lighting, unsigned int probes);

/* ---------- constants */

#define SHADER_MODEL_TAG 'soso'
#define OVERLAY_FILE "materials\\overlay.json"
#define OVERLAY_FORMAT 1

#define MAXIMUM_OVERLAY_FILE_SIZE (1024UL * 1024)
#define MAXIMUM_TILE_FILE_SIZE (64UL * 1024 * 1024)
#define MAXIMUM_TILE_SIZE 4096
#define TILE_MADE_INTERVAL_MILLISECONDS 8
/* (xgpu_images.h XGPU_IMAGE_RGBA and XGPU_IMAGE_RGBA_LEVELS: its every
level given, level 0 first) */
#define IMAGE_RGBA 0
#define IMAGE_RGBA_LEVELS 2
#define IMAGE_BC5_LEVELS 3
/* a surface map's file next to its bitmap's tag, and the bitmaps whose
surface maps are looked for (a power of two, at least twice the most
bitmap tags drawn) */
#define SURFACE_FILE_EXTENSION ".surface"
#define SURFACE_TABLE_SIZE 2048
/* KTX2's: VK_FORMAT_BC5_UNORM_BLOCK */
#define KTX2_BC5_UNORM 141
#define MAXIMUM_TOKENS 16384
#define MAXIMUM_CLASSES 64
#define MAXIMUM_SHADERS 512
/* (a power of two, at least twice MAXIMUM_SHADERS) */
#define SHADER_TABLE_SIZE 1024
#define MAXIMUM_NAME 256

/* as xgpu_spirv.c reads them (xgpu.h XGPU_OVERLAY_UNIFORM_COUNT) */
#define OVERLAY_UNIFORM_COUNT 20
/* the class masks a shader names, one per bitmap of its base map */
#define MAXIMUM_CLASS_MASKS 8

enum
{
	_slot_a = 0,
	_slot_b,
	_slot_c,
	_slot_d,
	NUMBER_OF_SLOTS
};

/* each slot's vec4 of the uniform (its scale and strengths), and where its
footprint fade's start and end are (xgpu.h) */
static short const slot_uniform[NUMBER_OF_SLOTS] = { 4, 5, 7, 8 };
static short const slot_footprint[NUMBER_OF_SLOTS][2] = { { 3, 2 }, { 6, 2 }, { 9, 0 }, { 9, 2 } };

/* ---------- structures */

enum
{
	_tile_none = 0,
	/* its files read, to be decoded */
	_tile_queued,
	_tile_decoding,
	/* decoded (pixels; NULL if it could not be), to be made a texture */
	_tile_decoded
};

enum
{
	_tile_file_png = 0,
	_tile_file_normal,
	_tile_file_cavity_rough,
	NUMBER_OF_TILE_FILES
};

struct overlay_class
{
	/* (a class mask: no name, its PNG alone, its levels averaged) */
	char name[64];
	boolean mask;
	/* (a surface map: tile is the mod file's whole path, its second
	image, texture2, the occlusion and roughness; pixels2 their levels) */
	boolean surface;
	unsigned int texture2;
	byte *pixels2;
	char tile[MAXIMUM_NAME];
	/* (levels) the tile's mip levels made by the tools, two KTX2s of BC5
	blocks: the normal's x and y, and the cavity (half of it) and the
	roughness offset, its levels' widened where their normals averaged
	out (Toksvig's) */
	char levels[2][MAXIMUM_NAME];
	unsigned int texture;
	boolean failed;
	/* the decoding (TILES_DECODE_AHEAD: under tiles_lock): _tile_*, the
	files as read, and the texels, every level's when levels_given */
	int state;
	byte *files[NUMBER_OF_TILE_FILES];
	unsigned long file_sizes[NUMBER_OF_TILE_FILES];
	byte *pixels;
	unsigned long width, height, level_count;
	boolean levels_given;
	/* how long its decoding took (the log's) */
	unsigned long decode_milliseconds;
	real tile_wu;
	real normal, cavity, rough;
	real fade[2];
	real footprint[2];
};

struct overlay_shader
{
	struct shader const *shader;
	short slot_count;
	short slots[NUMBER_OF_SLOTS];
	/* the class masks by the base map's bitmap (classes[] of their own,
	NONE for none) */
	short mask_count;
	short masks[MAXIMUM_CLASS_MASKS];
	/* "class_mask_wins": the mask's slots A, C and D kept over the
	channel rule (xgpu.h's XGPU_OVERLAY_CLASS_MASK_WINS) */
	boolean mask_wins;
	/* "surface_map" (display.prism_surface_maps, xgpu.h's "The surface
	map"): its images by the base map's bitmap (classes[] of their own, as
	the masks are; NONE none), and "surface": the normal's, occlusion's and
	highlight's strengths and the highlight's sharpest exponent */
	short surface_count;
	short surfaces[MAXIMUM_CLASS_MASKS];
	real surface[4];
	real tiles_per_uv[NUMBER_OF_SLOTS];
	real strength;
	/* the uniform, before each draw's fade and strength */
	float uniforms[OVERLAY_UNIFORM_COUNT][4];
};

/* ---------- globals */

static struct
{
	struct overlay_class classes[MAXIMUM_CLASSES];
	short class_count;
	struct overlay_shader shaders[MAXIMUM_SHADERS];
	short shader_count;
	/* the shaders' indices by their definitions (NONE empty) */
	short table[SHADER_TABLE_SIZE];
	/* the settings as last read (config_changes) */
	unsigned long settings_read;
	boolean enabled;
	real strength;
	int view;
	/* display.prism_surface_maps */
	boolean surface_maps;
	/* the surface maps found next to the bitmaps drawn, by the bitmap
	tag's index (tag NONE empty; surfaces[] NONE none, per picture) */
	struct
	{
		long tag;
		short surfaces[MAXIMUM_CLASS_MASKS];
	} surface_table[SURFACE_TABLE_SIZE];
	long surface_tags;
	/* a draw is armed (mod_materials_draw_end disarms it) */
	boolean armed;
} mod_materials_globals;

static boolean mod_materials_table_cleared;

/* the tiles' decoding: started (for the overlay as read), and the thread's
running and asked to stop */
static boolean tiles_started;
#ifdef TILES_DECODE_AHEAD
static pthread_mutex_t tiles_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tiles_finished = PTHREAD_COND_INITIALIZER;
static boolean tiles_running, tiles_stopping;
#endif

static void tiles_stop(void);
static short surface_add(char const *path);
static void surface_table_clear(void);

/* ---------- private code */

static void table_clear(
	void)
{
	long index;

	for (index = 0; index < SHADER_TABLE_SIZE; index++)
	{
		mod_materials_globals.table[index] = NONE;
	}
	mod_materials_table_cleared = TRUE;

	return;
}

static unsigned long pointer_hash(
	void const *pointer)
{
	unsigned long value = (unsigned long)(size_t)pointer;

	return (value >> 4) * 2654435761UL;
}

static struct overlay_shader *shader_find(
	struct shader const *shader)
{
	unsigned long index;
	long probes;

	if (!mod_materials_globals.shader_count)
	{
		return NULL;
	}
	index = pointer_hash(shader) & (SHADER_TABLE_SIZE - 1);
	for (probes = 0; probes < SHADER_TABLE_SIZE; probes++)
	{
		short entry = mod_materials_globals.table[index];

		if (entry == NONE)
		{
			return NULL;
		}
		if (mod_materials_globals.shaders[entry].shader == shader)
		{
			return &mod_materials_globals.shaders[entry];
		}
		index = (index + 1) & (SHADER_TABLE_SIZE - 1);
	}

	return NULL;
}

static void table_add(
	short entry)
{
	unsigned long index = pointer_hash(mod_materials_globals.shaders[entry].shader) & (SHADER_TABLE_SIZE - 1);

	while (mod_materials_globals.table[index] != NONE)
	{
		index = (index + 1) & (SHADER_TABLE_SIZE - 1);
	}
	mod_materials_globals.table[index] = entry;

	return;
}

/* ---------- the surface maps next to the bitmaps */

static void surface_table_clear(
	void)
{
	long index;

	for (index = 0; index < SURFACE_TABLE_SIZE; index++)
	{
		mod_materials_globals.surface_table[index].tag = NONE;
	}
	mod_materials_globals.surface_tags = 0;

	return;
}

/* the surface map of a bitmap tag's picture (a class), looked for next to
its tag the first time it is asked for: tags\<bitmap>.surface.png, or
tags\<bitmap>.surface<n>.png for picture n; NONE without one */
static short surface_of_bitmap(
	long tag_index,
	short bitmap)
{
	unsigned long index = ((unsigned long)tag_index * 2654435761UL) & (SURFACE_TABLE_SIZE - 1);
	long probes;

	if (tag_index == NONE || bitmap < 0 || bitmap >= MAXIMUM_CLASS_MASKS)
	{
		return NONE;
	}
	for (probes = 0; probes < SURFACE_TABLE_SIZE; probes++)
	{
		if (mod_materials_globals.surface_table[index].tag == tag_index)
		{
			return mod_materials_globals.surface_table[index].surfaces[bitmap];
		}
		if (mod_materials_globals.surface_table[index].tag == NONE)
		{
			break;
		}
		index = (index + 1) & (SURFACE_TABLE_SIZE - 1);
	}
	/* (a full table looks no further: half full at most) */
	if (probes == SURFACE_TABLE_SIZE || mod_materials_globals.surface_tags >= SURFACE_TABLE_SIZE / 2)
	{
		return NONE;
	}
	mod_materials_globals.surface_table[index].tag = tag_index;
	mod_materials_globals.surface_tags++;
	{
		char const *name = tag_get_name(tag_index);
		long count = bitmap_group_get(tag_index)->bitmaps.count;
		short picture;

		for (picture = 0; picture < MAXIMUM_CLASS_MASKS; picture++)
		{
			char path[MAXIMUM_NAME + 32];

			mod_materials_globals.surface_table[index].surfaces[picture] = NONE;
			if (!name || picture >= count)
			{
				continue;
			}
			if (picture)
				snprintf(path, sizeof(path), "%s%s%d.png", name, SURFACE_FILE_EXTENSION, picture);
			else
				snprintf(path, sizeof(path), "%s%s.png", name, SURFACE_FILE_EXTENSION);
			if (port_mods_file_find(path, mods_map_name()) != NONE)
			{
				mod_materials_globals.surface_table[index].surfaces[picture] = surface_add(path);
				error(_error_silent, "mods: the surface map %s found for the bitmap %s", path, name);
			}
		}
	}

	return mod_materials_globals.surface_table[index].surfaces[bitmap];
}

/* every tile and shader let go */
static void materials_dispose(
	void)
{
	short index;

	mod_materials_draw_end();
	tiles_stop();
	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		if (mod_materials_globals.classes[index].texture)
		{
			xgpu_image_delete(mod_materials_globals.classes[index].texture);
		}
		if (mod_materials_globals.classes[index].texture2)
		{
			xgpu_image_delete(mod_materials_globals.classes[index].texture2);
		}
	}
	mod_materials_globals.class_count = 0;
	surface_table_clear();
	mod_materials_globals.shader_count = 0;
	table_clear();

	return;
}

/* ---------- reading overlay.json */

struct json_document
{
	char const *text;
	jsmntok_t *tokens;
	int count;
};

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

/* a string token's text, unescaped (\\ \" \/), into text; FALSE if it does not fit */
static boolean json_string(
	struct json_document const *json,
	int token,
	char *text,
	unsigned long size)
{
	jsmntok_t const *string;
	char const *character;
	char const *end;
	unsigned long length = 0;

	/* (a member that is not there: json_member's NONE) */
	if (token < 0 || token >= json->count)
	{
		return FALSE;
	}
	string = &json->tokens[token];
	character = json->text + string->start;
	end = json->text + string->end;
	if (string->type != JSMN_STRING)
	{
		return FALSE;
	}
	while (character < end)
	{
		char value = *character++;

		if (value == '\\' && character < end)
		{
			value = *character++;
			if (value == 'n' || value == 't' || value == 'r' || value == 'b' || value == 'f' || value == 'u')
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

	if (object < 0 || json->tokens[object].type != JSMN_OBJECT)
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

static boolean json_is_null(
	struct json_document const *json,
	int token)
{
	return json->tokens[token].type == JSMN_PRIMITIVE && json->text[json->tokens[token].start] == 'n';
}

/* a number (a primitive that is one), or `otherwise` */
static real json_real(
	struct json_document const *json,
	int token,
	real otherwise)
{
	char text[64];
	char *end;
	double value;
	int length;

	if (token < 0 || json->tokens[token].type != JSMN_PRIMITIVE)
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

static boolean json_boolean(
	struct json_document const *json,
	int token,
	boolean otherwise)
{
	if (token < 0 || json->tokens[token].type != JSMN_PRIMITIVE)
	{
		return otherwise;
	}

	return json->text[json->tokens[token].start] == 't';
}

/* the two numbers of an array of two, or the defaults */
static void json_pair(
	struct json_document const *json,
	int token,
	real *pair)
{
	if (token >= 0 && json->tokens[token].type == JSMN_ARRAY && json->tokens[token].size == 2)
	{
		pair[0] = json_real(json, token + 1, pair[0]);
		pair[1] = json_real(json, token + 2, pair[1]);
	}

	return;
}

/* a multipurpose channel's index in the Xbox's order, or NONE */
static short channel_index(
	char const *name)
{
	if (!strcmp(name, "r") || !strcmp(name, "specular"))
		return 0;
	if (!strcmp(name, "g") || !strcmp(name, "self_illumination"))
		return 1;
	if (!strcmp(name, "b") || !strcmp(name, "change_color"))
		return 2;
	if (!strcmp(name, "a") || !strcmp(name, "auxiliary"))
		return 3;

	return NONE;
}

/* tag paths compared as port_mods does: any case, either slash */
static boolean names_equal(
	char const *a,
	char const *b)
{
	while (*a && *b)
	{
		char x = *a == '/' ? '\\' : *a;
		char y = *b == '/' ? '\\' : *b;

		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
		{
			return FALSE;
		}
		a++;
		b++;
	}

	return *a == *b;
}

/* log2 of x, at least 1, to within a hundredth (the footprint's ends) */
static real log2_rough(
	real x)
{
	real exponent = 0.0f;

	x = MAX(x, 1.0f);
	while (x >= 2.0f && exponent < 64.0f)
	{
		x *= 0.5f;
		exponent += 1.0f;
	}

	return exponent + (x - 1.0f) * (1.3466f - 0.3466f * x);
}

static short class_find(
	char const *name)
{
	short index;

	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		if (!mod_materials_globals.classes[index].mask && !strcmp(mod_materials_globals.classes[index].name, name))
		{
			return index;
		}
	}

	return NONE;
}

/* a class mask's entry in classes[] (one for each file, however many
shaders name it), or NONE if there is no room */
static short mask_add(
	char const *file)
{
	struct overlay_class *overlay_class;
	short index;

	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		if ((mod_materials_globals.classes[index].mask || mod_materials_globals.classes[index].surface) &&
			!strcmp(mod_materials_globals.classes[index].tile, file))
		{
			return index;
		}
	}
	if (mod_materials_globals.class_count >= MAXIMUM_CLASSES)
	{
		error(_error_silent, "mods: more than %d material overlay classes and masks: the mask %s is left out",
			MAXIMUM_CLASSES, file);
		return NONE;
	}
	overlay_class = &mod_materials_globals.classes[mod_materials_globals.class_count];
	memset(overlay_class, 0, sizeof(*overlay_class));
	overlay_class->mask = TRUE;
	strncpy(overlay_class->tile, file, sizeof(overlay_class->tile) - 1);

	return mod_materials_globals.class_count++;
}

/* a class of a surface map (its mod file's path), once for every shader
or bitmap that names it, or NONE if there is no room */
static short surface_add(
	char const *path)
{
	short index = mask_add(path);

	if (index != NONE)
	{
		mod_materials_globals.classes[index].mask = FALSE;
		mod_materials_globals.classes[index].surface = TRUE;
	}

	return index;
}

/* ---------- the tiles

Read when the overlay is (the map's tags load, mods_reload) with the setting
on, or at the first draw it is turned on for, and decoded on a thread of
their own, so that no draw waits for them: a draw whose tiles are not ready
goes without the overlay, and the first after they are makes them textures.
A class's levels (two KTX2s) give its every mip level as the tools filtered
them; without them, or if they cannot be read, its PNG is level 0 and the
renderer box-filters the rest. */

static unsigned long read_u32(
	byte const *data)
{
	return (unsigned long)data[0] | (unsigned long)data[1] << 8 | (unsigned long)data[2] << 16 |
		(unsigned long)data[3] << 24;
}

/* a mod's file, all of it (malloc), or NULL */
static byte *tile_file_read(
	char const *name,
	boolean whole_path,
	unsigned long *size)
{
	char path[MAXIMUM_NAME + 16];
	unsigned long stamp[2];
	long file;
	byte *data;
	char *character;

	*size = 0;
	snprintf(path, sizeof(path), whole_path ? "%s" : "materials\\%s", name);
	for (character = path; *character; character++)
	{
		if (*character == '/')
			*character = '\\';
	}
	file = port_mods_file_find(path, mods_map_name());
	if (file == NONE || !port_mods_file_stamp(file, size, stamp) || !*size || *size > MAXIMUM_TILE_FILE_SIZE)
	{
		error(_error_silent, "mods: the material overlay's tile %s is not in the mods", path);
		*size = 0;
		return NULL;
	}
	data = malloc(*size);
	if (data && !port_mods_file_read(file, 0, data, *size))
	{
		free(data);
		data = NULL;
	}
	if (!data)
		*size = 0;
	return data;
}

/* a BC4 block's 16 values, every 4th byte of out from its row 0 (out's
rows stride bytes apart) */
static void bc4_block_decode(
	byte const *block,
	byte *out,
	unsigned long stride,
	unsigned long columns,
	unsigned long rows)
{
	unsigned long palette[8];
	unsigned long long bits = 0;
	unsigned long e0 = block[0], e1 = block[1];
	unsigned long row, column;
	int index;

	for (index = 0; index < 6; index++)
		bits |= (unsigned long long)block[2 + index] << (8 * index);
	palette[0] = e0;
	palette[1] = e1;
	if (e0 > e1)
	{
		for (index = 1; index < 7; index++)
			palette[index + 1] = ((7 - index) * e0 + index * e1 + 3) / 7;
	}
	else
	{
		for (index = 1; index < 5; index++)
			palette[index + 1] = ((5 - index) * e0 + index * e1 + 2) / 5;
		palette[6] = 0;
		palette[7] = 255;
	}
	for (row = 0; row < rows; row++)
	{
		for (column = 0; column < columns; column++)
		{
			out[row * stride + column * 4] = (byte)palette[(bits >> (3 * (row * 4 + column))) & 7];
		}
	}
}

/* a KTX2 of BC5 blocks (as the tools write it: one face, one layer, no
supercompression, every level down to 1 by 1) into two channels of the
RGBA levels at pixels (first is 0 or 2); FALSE if it is not one of the
size given */
static boolean ktx2_bc5_decode(
	byte const *data,
	unsigned long size,
	unsigned long width,
	unsigned long height,
	unsigned long level_count,
	byte *pixels,
	int first)
{
	static byte const identifier[12] = { 0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, '\r', '\n', 0x1a, '\n' };
	unsigned long level;

	if (size < 80 + 24 * level_count || memcmp(data, identifier, sizeof(identifier)) ||
		read_u32(data + 12) != KTX2_BC5_UNORM || read_u32(data + 20) != width || read_u32(data + 24) != height ||
		read_u32(data + 28) > 1 || read_u32(data + 32) > 1 || read_u32(data + 36) != 1 ||
		read_u32(data + 40) != level_count || read_u32(data + 44) != 0)
	{
		return FALSE;
	}
	for (level = 0; level < level_count; level++)
	{
		byte const *entry = data + 80 + 24 * level;
		unsigned long level_width = width >> level ? width >> level : 1;
		unsigned long level_height = height >> level ? height >> level : 1;
		unsigned long blocks_across = (level_width + 3) / 4, blocks_down = (level_height + 3) / 4;
		unsigned long offset = read_u32(entry), length = read_u32(entry + 8);
		unsigned long x, y;

		if (read_u32(entry + 4) || read_u32(entry + 12) || length != blocks_across * blocks_down * 16 ||
			offset > size || length > size - offset)
		{
			return FALSE;
		}
		for (y = 0; y < blocks_down; y++)
		{
			for (x = 0; x < blocks_across; x++)
			{
				byte const *block = data + offset + (y * blocks_across + x) * 16;
				byte *out = pixels + ((y * 4) * level_width + x * 4) * 4 + first;
				unsigned long columns = level_width - x * 4 < 4 ? level_width - x * 4 : 4;
				unsigned long rows = level_height - y * 4 < 4 ? level_height - y * 4 : 4;

				bc4_block_decode(block, out, level_width * 4, columns, rows);
				bc4_block_decode(block + 8, out + 1, level_width * 4, columns, rows);
			}
		}
		pixels += level_width * level_height * 4;
	}
	return TRUE;
}

/* a BC4 block of 16 values (each 4 bytes apart in values, rows stride
bytes apart; columns and rows of them, the rest repeating the last):
its two ends the least and most, every value the nearest of the eight
between them */
static void bc4_block_encode(
	byte const *values,
	unsigned long stride,
	unsigned long columns,
	unsigned long rows,
	byte *block)
{
	byte v[16];
	unsigned long long bits = 0;
	int low = 255, high = 0, index;

	for (index = 0; index < 16; index++)
	{
		unsigned long row = (unsigned long)index / 4 < rows ? (unsigned long)index / 4 : rows - 1;
		unsigned long column = (unsigned long)index % 4 < columns ? (unsigned long)index % 4 : columns - 1;

		v[index] = values[row * stride + column * 4];
		low = v[index] < low ? v[index] : low;
		high = v[index] > high ? v[index] : high;
	}
	block[0] = (byte)high;
	block[1] = (byte)low;
	if (high > low)
	{
		for (index = 0; index < 16; index++)
		{
			/* (palette 0 the high end, 1 the low, 2 to 7 between from high
			to low) */
			int step = ((high - v[index]) * 14 + (high - low)) / (2 * (high - low));
			unsigned long long code = step == 0 ? 0 : step == 7 ? 1 : (unsigned long long)(step + 1);

			bits |= code << (3 * index);
		}
	}
	for (index = 0; index < 6; index++)
		block[2 + index] = (byte)(bits >> (8 * index));
}

/* an RGBA level's channels first and first + 1 as BC5 blocks at out */
static void bc5_level_encode(
	byte const *pixels,
	unsigned long width,
	unsigned long height,
	int first,
	byte *out)
{
	unsigned long x, y;

	for (y = 0; y < height; y += 4)
	{
		for (x = 0; x < width; x += 4)
		{
			byte const *at = pixels + (y * width + x) * 4 + first;
			unsigned long columns = width - x < 4 ? width - x : 4, rows = height - y < 4 ? height - y : 4;

			bc4_block_encode(at, width * 4, columns, rows, out);
			bc4_block_encode(at + 1, width * 4, columns, rows, out + 8);
			out += 16;
		}
	}
}

/* a surface map's PNG (RGBA: the normal's x and y, the occlusion, the
roughness) decoded, its levels box-filtered, every level encoded as two
BC5 images: pixels the normal's, pixels2 the occlusion's and roughness's */
static void surface_decode(
	struct overlay_class *overlay_class)
{
	byte *level = NULL, *normal = NULL, *occlusion = NULL;
	unsigned long width = 0, height = 0, level_count = 1, bytes = 0, at = 0, largest;
	unsigned long start = system_milliseconds();

	if (overlay_class->files[_tile_file_png])
	{
		level = hud_hires_png_pixels(overlay_class->files[_tile_file_png], overlay_class->file_sizes[_tile_file_png],
			&width, &height);
	}
	if (level && width && height && width <= MAXIMUM_TILE_SIZE && height <= MAXIMUM_TILE_SIZE)
	{
		unsigned long index;

		for (largest = width > height ? width : height; largest > 1; largest >>= 1)
			level_count++;
		for (index = 0; index < level_count; index++)
		{
			unsigned long w = width >> index ? width >> index : 1, h = height >> index ? height >> index : 1;

			bytes += ((w + 3) / 4) * ((h + 3) / 4) * 16;
		}
		normal = malloc(bytes);
		occlusion = malloc(bytes);
	}
	if (normal && occlusion)
	{
		unsigned long index, w = width, h = height;

		for (index = 0; index < level_count; index++)
		{
			bc5_level_encode(level, w, h, 0, normal + at);
			bc5_level_encode(level, w, h, 2, occlusion + at);
			at += ((w + 3) / 4) * ((h + 3) / 4) * 16;
			if (index + 1 < level_count)
			{
				/* (the next level: each texel the mean of the 2 by 2 under
				it, or of the 2 by 1 at an edge one texel wide) */
				unsigned long nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y;
				byte *next = malloc(nw * nh * 4);

				if (!next)
				{
					free(normal);
					free(occlusion);
					normal = occlusion = NULL;
					break;
				}
				for (y = 0; y < nh; y++)
				{
					for (x = 0; x < nw; x++)
					{
						unsigned long x0 = x * 2, y0 = y * 2, x1 = w > 1 ? x0 + 1 : x0, y1 = h > 1 ? y0 + 1 : y0;
						int channel;

						for (channel = 0; channel < 4; channel++)
						{
							next[(y * nw + x) * 4 + channel] = (byte)((level[(y0 * w + x0) * 4 + channel] +
								level[(y0 * w + x1) * 4 + channel] + level[(y1 * w + x0) * 4 + channel] +
								level[(y1 * w + x1) * 4 + channel] + 2) / 4);
						}
					}
				}
				free(level);
				level = next;
				w = nw;
				h = nh;
			}
		}
	}
	else
	{
		free(normal);
		free(occlusion);
		normal = occlusion = NULL;
	}
	free(level);
	overlay_class->levels_given = normal != NULL;
	overlay_class->width = width;
	overlay_class->height = height;
	overlay_class->level_count = level_count;
	overlay_class->pixels = normal;
	overlay_class->pixels2 = occlusion;
	overlay_class->decode_milliseconds = system_milliseconds() - start;
	free(overlay_class->files[_tile_file_png]);
	overlay_class->files[_tile_file_png] = NULL;
	overlay_class->file_sizes[_tile_file_png] = 0;

	return;
}

/* a class's tile decoded from its files (which it frees): its levels, or
its PNG's level 0 */
static void tile_decode(
	struct overlay_class *overlay_class)
{
	byte *normal = overlay_class->files[_tile_file_normal];
	byte *cavity_rough = overlay_class->files[_tile_file_cavity_rough];
	byte *pixels = NULL;
	unsigned long width = 0, height = 0, level_count = 0, texels = 0;
	unsigned long start = system_milliseconds();
	boolean levels_given = FALSE;

	if (overlay_class->surface)
	{
		surface_decode(overlay_class);
		return;
	}
	if (normal && cavity_rough && overlay_class->file_sizes[_tile_file_normal] >= 80)
	{
		unsigned long largest;

		width = read_u32(normal + 20);
		height = read_u32(normal + 24);
		level_count = 1;
		for (largest = width > height ? width : height; largest > 1; largest >>= 1)
			level_count++;
		if (width && height && width <= MAXIMUM_TILE_SIZE && height <= MAXIMUM_TILE_SIZE)
		{
			unsigned long level;

			for (level = 0; level < level_count; level++)
				texels += (width >> level ? width >> level : 1) * (height >> level ? height >> level : 1);
			pixels = malloc(texels * 4);
		}
		if (pixels &&
			ktx2_bc5_decode(normal, overlay_class->file_sizes[_tile_file_normal], width, height, level_count, pixels, 0) &&
			ktx2_bc5_decode(cavity_rough, overlay_class->file_sizes[_tile_file_cavity_rough], width, height, level_count,
				pixels, 2))
		{
			levels_given = TRUE;
		}
		else
		{
			free(pixels);
			pixels = NULL;
		}
	}
	if (!pixels && overlay_class->files[_tile_file_png])
	{
		pixels = hud_hires_png_pixels(overlay_class->files[_tile_file_png], overlay_class->file_sizes[_tile_file_png],
			&width, &height);
		level_count = 1;
		if (pixels)
		{
			unsigned long largest;

			for (largest = width > height ? width : height; largest > 1; largest >>= 1)
				level_count++;
		}
	}
	overlay_class->levels_given = levels_given;
	overlay_class->width = width;
	overlay_class->height = height;
	overlay_class->level_count = level_count;
	overlay_class->pixels = pixels;
	overlay_class->decode_milliseconds = system_milliseconds() - start;
	{
		int file;

		for (file = 0; file < NUMBER_OF_TILE_FILES; file++)
		{
			free(overlay_class->files[file]);
			overlay_class->files[file] = NULL;
			overlay_class->file_sizes[file] = 0;
		}
	}

	return;
}

#ifdef TILES_DECODE_AHEAD
static void *tiles_thread_main(
	void *parameter)
{
	short index;

	(void)parameter;
	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		struct overlay_class *overlay_class = &mod_materials_globals.classes[index];

		pthread_mutex_lock(&tiles_lock);
		if (tiles_stopping)
		{
			pthread_mutex_unlock(&tiles_lock);
			break;
		}
		if (overlay_class->state != _tile_queued)
		{
			pthread_mutex_unlock(&tiles_lock);
			continue;
		}
		overlay_class->state = _tile_decoding;
		pthread_mutex_unlock(&tiles_lock);
		tile_decode(overlay_class);
		pthread_mutex_lock(&tiles_lock);
		overlay_class->state = _tile_decoded;
		pthread_mutex_unlock(&tiles_lock);
	}
	pthread_mutex_lock(&tiles_lock);
	tiles_running = FALSE;
	pthread_cond_broadcast(&tiles_finished);
	pthread_mutex_unlock(&tiles_lock);
	return NULL;
}
#endif

/* every class's files read and their decoding begun, once for the overlay
as read */
static void tiles_start(
	void)
{
	short index;

	if (tiles_started || !mod_materials_globals.class_count)
	{
		return;
	}
	tiles_started = TRUE;
	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		struct overlay_class *overlay_class = &mod_materials_globals.classes[index];

		if (overlay_class->levels[0][0] && overlay_class->levels[1][0])
		{
			overlay_class->files[_tile_file_normal] = tile_file_read(overlay_class->levels[0], FALSE,
				&overlay_class->file_sizes[_tile_file_normal]);
			overlay_class->files[_tile_file_cavity_rough] = tile_file_read(overlay_class->levels[1], FALSE,
				&overlay_class->file_sizes[_tile_file_cavity_rough]);
		}
		overlay_class->files[_tile_file_png] = tile_file_read(overlay_class->tile, overlay_class->surface,
			&overlay_class->file_sizes[_tile_file_png]);
		overlay_class->state = _tile_queued;
	}
#ifdef TILES_DECODE_AHEAD
	{
		pthread_t thread;

		tiles_stopping = FALSE;
		tiles_running = TRUE;
		if (pthread_create(&thread, NULL, tiles_thread_main, NULL) == 0)
		{
			pthread_detach(thread);
			return;
		}
		tiles_running = FALSE;
	}
#endif
	/* (no thread: decoded here, as the map loads) */
	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		tile_decode(&mod_materials_globals.classes[index]);
		mod_materials_globals.classes[index].state = _tile_decoded;
	}

	return;
}

/* the decoding stopped and everything it holds let go */
static void tiles_stop(
	void)
{
	short index;

#ifdef TILES_DECODE_AHEAD
	pthread_mutex_lock(&tiles_lock);
	tiles_stopping = TRUE;
	while (tiles_running)
		pthread_cond_wait(&tiles_finished, &tiles_lock);
	pthread_mutex_unlock(&tiles_lock);
#endif
	for (index = 0; index < mod_materials_globals.class_count; index++)
	{
		struct overlay_class *overlay_class = &mod_materials_globals.classes[index];
		int file;

		for (file = 0; file < NUMBER_OF_TILE_FILES; file++)
		{
			free(overlay_class->files[file]);
			overlay_class->files[file] = NULL;
		}
		free(overlay_class->pixels);
		overlay_class->pixels = NULL;
		free(overlay_class->pixels2);
		overlay_class->pixels2 = NULL;
		overlay_class->state = _tile_none;
	}
	tiles_started = FALSE;

	return;
}

/* a class the tiles' start did not read (added after it) read and queued,
and the decoding thread started again if it has finished */
static void tiles_kick(
	struct overlay_class *overlay_class)
{
	int state;

#ifdef TILES_DECODE_AHEAD
	pthread_mutex_lock(&tiles_lock);
	state = overlay_class->state;
	pthread_mutex_unlock(&tiles_lock);
#else
	state = overlay_class->state;
#endif
	if (state == _tile_none)
	{
		overlay_class->files[_tile_file_png] = tile_file_read(overlay_class->tile, overlay_class->surface,
			&overlay_class->file_sizes[_tile_file_png]);
#ifdef TILES_DECODE_AHEAD
		pthread_mutex_lock(&tiles_lock);
		overlay_class->state = _tile_queued;
		pthread_mutex_unlock(&tiles_lock);
#else
		tile_decode(overlay_class);
		overlay_class->state = _tile_decoded;
#endif
		state = _tile_queued;
	}
#ifdef TILES_DECODE_AHEAD
	if (state == _tile_queued)
	{
		pthread_mutex_lock(&tiles_lock);
		if (!tiles_running && !tiles_stopping)
		{
			pthread_t thread;

			tiles_running = TRUE;
			if (pthread_create(&thread, NULL, tiles_thread_main, NULL) == 0)
				pthread_detach(thread);
			else
				tiles_running = FALSE;
		}
		pthread_mutex_unlock(&tiles_lock);
	}
#endif

	return;
}

/* the class's tile made a texture, once it is decoded; FALSE until then,
or if it cannot be */
static boolean class_tile_load(
	struct overlay_class *overlay_class)
{
	/* (one tile made an image a frame or so: the host uploads each as it
	is first drawn, and the frame the overlay first draws in does not wait
	for them all) */
	static unsigned long last_made;
	static boolean made_any;
	int state;

	if (overlay_class->texture || overlay_class->failed)
	{
		return overlay_class->texture != 0;
	}
	tiles_start();
	/* (a class added after the tiles were started: a surface map found
	next to a bitmap as it is first drawn) */
	tiles_kick(overlay_class);
#ifdef TILES_DECODE_AHEAD
	pthread_mutex_lock(&tiles_lock);
	state = overlay_class->state;
	pthread_mutex_unlock(&tiles_lock);
#else
	state = overlay_class->state;
#endif
	if (state != _tile_decoded ||
		(made_any && overlay_class->pixels && system_milliseconds() - last_made < TILE_MADE_INTERVAL_MILLISECONDS))
	{
		return FALSE;
	}
	overlay_class->failed = TRUE;
	if (overlay_class->pixels)
	{
		unsigned long start = system_milliseconds();

		/* (an image of the texels, which it takes: the host reads them where
		they are, and makes its levels past the first unless they are given) */
		overlay_class->texture = xgpu_image_create(overlay_class->pixels, overlay_class->width, overlay_class->height,
			overlay_class->level_count, overlay_class->surface ? IMAGE_BC5_LEVELS :
			overlay_class->levels_given ? IMAGE_RGBA_LEVELS : IMAGE_RGBA, TRUE);
		overlay_class->pixels = NULL;
		if (overlay_class->surface)
		{
			overlay_class->texture2 = overlay_class->pixels2 ? xgpu_image_create(overlay_class->pixels2,
				overlay_class->width, overlay_class->height, overlay_class->level_count, IMAGE_BC5_LEVELS, TRUE) : 0;
			overlay_class->pixels2 = NULL;
			if (!overlay_class->texture2 && overlay_class->texture)
			{
				xgpu_image_delete(overlay_class->texture);
				overlay_class->texture = 0;
			}
		}
		made_any = TRUE;
		last_made = system_milliseconds();
		if (overlay_class->texture)
		{
			error(_error_silent, "mods: the material overlay's %s %s: %lu levels (%s), decoded in %lu ms on the "
				"tiles' thread, made an image in %lu ms", overlay_class->surface ? "surface map (two BC5 images)" :
				overlay_class->mask ? "class mask" : "tile",
				overlay_class->tile, overlay_class->level_count,
				overlay_class->levels_given ? "its own" : "averaged from the PNG", overlay_class->decode_milliseconds,
				system_milliseconds() - start);
		}
	}
	if (!overlay_class->texture)
	{
		if (overlay_class->surface)
		{
			error(_error_silent, "mods: the surface map %s cannot be read (an 8-bit RGBA PNG of at most %d by %d): "
				"its draws go without it", overlay_class->tile, MAXIMUM_TILE_SIZE, MAXIMUM_TILE_SIZE);
		}
		else if (overlay_class->mask)
		{
			error(_error_silent, "mods: the material overlay's class mask %s is not built or cannot be read (an "
				"8-bit RGBA PNG): its shaders draw with their two classes' rule", overlay_class->tile);
		}
		else
		{
			error(_error_silent, "mods: the material overlay's tile %s cannot be drawn (an 8-bit RGBA PNG, or its "
				"levels)", overlay_class->tile);
		}
		return FALSE;
	}
	if (!overlay_class->surface && !overlay_class->levels_given && overlay_class->levels[0][0])
	{
		error(_error_silent, "mods: the material overlay's levels of %s cannot be read: its PNG's are drawn",
			overlay_class->tile);
	}
	overlay_class->failed = FALSE;

	return TRUE;
}

static void classes_read(
	struct json_document const *json,
	int classes)
{
	int token;
	int member;

	if (classes < 0 || json->tokens[classes].type != JSMN_OBJECT)
	{
		return;
	}
	token = classes + 1;
	for (member = 0; member < json->tokens[classes].size && token + 1 < json->count; member++)
	{
		struct overlay_class *overlay_class;
		int value = token + 1;

		if (mod_materials_globals.class_count >= MAXIMUM_CLASSES)
		{
			error(_error_silent, "mods: more than %d material overlay classes: the rest are left out", MAXIMUM_CLASSES);
			return;
		}
		overlay_class = &mod_materials_globals.classes[mod_materials_globals.class_count];
		memset(overlay_class, 0, sizeof(*overlay_class));
		if (json_string(json, token, overlay_class->name, sizeof(overlay_class->name)) &&
			json->tokens[value].type == JSMN_OBJECT &&
			json_string(json, json_member(json, value, "tile"), overlay_class->tile, sizeof(overlay_class->tile)))
		{
			int levels = json_member(json, value, "levels");

			if (levels >= 0 && json->tokens[levels].type == JSMN_OBJECT &&
				(!json_string(json, json_member(json, levels, "normal"), overlay_class->levels[0],
					sizeof(overlay_class->levels[0])) ||
				!json_string(json, json_member(json, levels, "cavity_rough"), overlay_class->levels[1],
					sizeof(overlay_class->levels[1]))))
			{
				overlay_class->levels[0][0] = overlay_class->levels[1][0] = 0;
			}
			overlay_class->tile_wu = json_real(json, json_member(json, value, "tile_wu"), 0.0f);
			overlay_class->normal = json_real(json, json_member(json, value, "normal"), 1.0f);
			overlay_class->cavity = json_real(json, json_member(json, value, "cavity"), 1.0f);
			overlay_class->rough = json_real(json, json_member(json, value, "rough"), 1.0f);
			overlay_class->fade[0] = 1.0f;
			overlay_class->fade[1] = 4.0f;
			json_pair(json, json_member(json, value, "fade"), overlay_class->fade);
			overlay_class->footprint[0] = 32.0f;
			overlay_class->footprint[1] = 8.0f;
			json_pair(json, json_member(json, value, "footprint"), overlay_class->footprint);
			if (overlay_class->fade[1] <= overlay_class->fade[0])
				overlay_class->fade[1] = overlay_class->fade[0] + 0.001f;
			/* (as xgpu_spirv.c compares them: tiles a pixel, log2) */
			overlay_class->footprint[0] = -log2_rough(overlay_class->footprint[0]);
			overlay_class->footprint[1] = -log2_rough(overlay_class->footprint[1]);
			if (overlay_class->footprint[1] <= overlay_class->footprint[0])
				overlay_class->footprint[1] = overlay_class->footprint[0] + 0.001f;
			mod_materials_globals.class_count++;
		}
		else
		{
			error(_error_silent, "mods: a material overlay class has no tile: left out");
		}
		token = json_skip(json, value);
	}

	return;
}

/* the uniform of a shader's entry (xgpu_spirv.c's overlay, xgpu.h), but for
each draw's fade and strength */
static boolean shader_read(
	struct json_document const *json,
	int value,
	struct overlay_shader *entry)
{
	int slots = json_member(json, value, "slots");
	int mask = json_member(json, value, "mask");
	int exclude = json_member(json, value, "exclude");
	int reflection = json_member(json, value, "reflection");
	int class_mask = json_member(json, value, "class_mask");
	int surface_map = json_member(json, value, "surface_map");
	int surface = json_member(json, value, "surface");
	real uv_wu = json_real(json, json_member(json, value, "uv_wu"), 0.0f);
	real uv_scale = json_real(json, json_member(json, value, "uv_scale"), 0.0f);
	short slot;

	if (slots < 0 || json->tokens[slots].type != JSMN_ARRAY || json->tokens[slots].size < 1 ||
		json->tokens[slots].size > NUMBER_OF_SLOTS)
	{
		return FALSE;
	}
	memset(entry->uniforms, 0, sizeof(entry->uniforms));
	/* (the slots past the shader's are its slot A again, which no weight
	reaches) */
	entry->slot_count = (short)json->tokens[slots].size;
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
	{
		char name[64];
		int element = slots + 1 + (slot < entry->slot_count ? slot : 0);
		struct overlay_class *overlay_class;

		/* (an array's elements are its next tokens: the slots are strings) */
		if (!json_string(json, element, name, sizeof(name)) || (entry->slots[slot] = class_find(name)) == NONE)
		{
			return FALSE;
		}
		overlay_class = &mod_materials_globals.classes[entry->slots[slot]];
		entry->tiles_per_uv[slot] = uv_scale > 0.0f ? uv_scale :
			uv_wu > 0.0f && overlay_class->tile_wu > 0.0f ? uv_wu / overlay_class->tile_wu : 1.0f;
		entry->uniforms[slot_uniform[slot]][0] = entry->tiles_per_uv[slot];
		entry->uniforms[slot_footprint[slot][0]][slot_footprint[slot][1]] = overlay_class->footprint[0];
		entry->uniforms[slot_footprint[slot][0]][slot_footprint[slot][1] + 1] = overlay_class->footprint[1];
	}
	/* the class masks, by the base map's bitmap */
	entry->mask_count = 0;
	if (class_mask >= 0 && (json->tokens[class_mask].type == JSMN_STRING ||
		json->tokens[class_mask].type == JSMN_ARRAY))
	{
		boolean one = json->tokens[class_mask].type == JSMN_STRING;
		int count = one ? 1 : json->tokens[class_mask].size;
		int element = one ? class_mask : class_mask + 1;
		int index;

		for (index = 0; index < count && index < MAXIMUM_CLASS_MASKS; index++, element++)
		{
			char file[MAXIMUM_NAME];

			entry->masks[index] = json_string(json, element, file, sizeof(file)) && file[0] ? mask_add(file) : NONE;
			entry->mask_count = (short)(index + 1);
		}
	}
	/* the surface maps, by the base map's bitmap, as the masks */
	entry->surface_count = 0;
	if (surface_map >= 0 && (json->tokens[surface_map].type == JSMN_STRING ||
		json->tokens[surface_map].type == JSMN_ARRAY))
	{
		boolean one = json->tokens[surface_map].type == JSMN_STRING;
		int count = one ? 1 : json->tokens[surface_map].size;
		int element = one ? surface_map : surface_map + 1;
		int index;

		for (index = 0; index < count && index < MAXIMUM_CLASS_MASKS; index++, element++)
		{
			char file[MAXIMUM_NAME];

			char path[MAXIMUM_NAME + 16];

			entry->surfaces[index] = json_string(json, element, file, sizeof(file)) && file[0] &&
				snprintf(path, sizeof(path), "materials\\%s", file) > 0 ? surface_add(path) : NONE;
			entry->surface_count = (short)(index + 1);
		}
	}
	entry->surface[0] = json_real(json, json_member(json, surface, "normal"), 1.0f);
	entry->surface[1] = json_real(json, json_member(json, surface, "occlusion"), 1.0f);
	entry->surface[2] = json_real(json, json_member(json, surface, "specular"), 0.5f);
	entry->surface[3] = json_real(json, json_member(json, surface, "exponent"), 48.0f);
	/* (one class: tile A everywhere, the mask's step below any channel) */
	entry->uniforms[1][0] = -2.0f;
	entry->uniforms[1][1] = -1.0f;
	if (mask >= 0 && json->tokens[mask].type == JSMN_OBJECT && json->tokens[slots].size > 1)
	{
		char channel[32];

		if (json_string(json, json_member(json, mask, "channel"), channel, sizeof(channel)) &&
			channel_index(channel) != NONE)
		{
			entry->uniforms[0][channel_index(channel)] = 1.0f;
		}
		entry->uniforms[1][0] = json_real(json, json_member(json, mask, "lo"), 0.4f);
		entry->uniforms[1][1] = json_real(json, json_member(json, mask, "hi"), 0.6f);
		if (entry->uniforms[1][1] <= entry->uniforms[1][0])
			entry->uniforms[1][1] = entry->uniforms[1][0] + 0.001f;
		entry->uniforms[1][2] = json_boolean(json, json_member(json, mask, "invert"), FALSE) ? 1.0f : 0.0f;
	}
	/* (no exclusion: above 2, which no channel is) */
	entry->uniforms[3][0] = 2.0f;
	if (exclude >= 0 && json->tokens[exclude].type == JSMN_OBJECT)
	{
		char channel[32];

		if (json_string(json, json_member(json, exclude, "channel"), channel, sizeof(channel)) &&
			channel_index(channel) != NONE)
		{
			entry->uniforms[2][channel_index(channel)] = 1.0f;
			entry->uniforms[3][0] = json_real(json, json_member(json, exclude, "above"), 0.1f);
		}
	}
	entry->uniforms[6][0] = json_real(json, json_member(json, reflection, "bend"), 1.0f);
	entry->uniforms[6][1] = json_real(json, json_member(json, reflection, "cavity"), 0.5f);
	entry->strength = json_real(json, json_member(json, value, "strength"), 1.0f);
	entry->mask_wins = json_boolean(json, json_member(json, value, "class_mask_wins"), FALSE);

	return TRUE;
}

/* the map's model shaders that the overlay names; how many */
static long shaders_read(
	struct json_document const *json,
	int shaders)
{
	struct tag_iterator iterator;
	long tag_index;
	long left_out = 0;

	if (shaders < 0 || json->tokens[shaders].type != JSMN_OBJECT)
	{
		return 0;
	}
	tag_iterator_new(&iterator, SHADER_MODEL_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *tag_name = tag_get_name(tag_index);
		int token = shaders + 1;
		int member;

		if (!tag_name)
		{
			continue;
		}
		for (member = 0; member < json->tokens[shaders].size && token + 1 < json->count; member++)
		{
			char name[MAXIMUM_NAME];
			int value = token + 1;

			if (json_string(json, token, name, sizeof(name)) && names_equal(name, tag_name))
			{
				struct overlay_shader *entry;

				if (json_is_null(json, value) || json->tokens[value].type != JSMN_OBJECT)
				{
					break;
				}
				if (mod_materials_globals.shader_count >= MAXIMUM_SHADERS)
				{
					left_out++;
					break;
				}
				entry = &mod_materials_globals.shaders[mod_materials_globals.shader_count];
				memset(entry, 0, sizeof(*entry));
				entry->shader = (struct shader const *)tag_get(SHADER_MODEL_TAG, tag_index);
				if (entry->shader && shader_read(json, value, entry))
				{
					table_add(mod_materials_globals.shader_count);
					mod_materials_globals.shader_count++;
				}
				else
				{
					error(_error_silent, "mods: the material overlay of %s left out: its slots name no class",
						tag_name);
					left_out++;
				}
				break;
			}
			token = json_skip(json, value);
		}
	}

	return mod_materials_globals.shader_count;
}

/* overlay.json of the mods played on the map read again; how many of its
shaders it overlays */
static long overlay_read(
	void)
{
	struct json_document json;
	jsmn_parser parser;
	unsigned long stamp[2];
	unsigned long size = 0;
	long file = port_mods_file_find(OVERLAY_FILE, mods_map_name());
	char *text;
	long count = 0;

	if (file == NONE)
	{
		return 0;
	}
	if (!port_mods_file_stamp(file, &size, stamp) || !size || size > MAXIMUM_OVERLAY_FILE_SIZE)
	{
		error(_error_silent, "mods: %s left out: it is not the size of an overlay", port_mods_file_path(file));
		return 0;
	}
	text = malloc(size + 1);
	json.tokens = malloc(MAXIMUM_TOKENS * sizeof(jsmntok_t));
	if (text && json.tokens && port_mods_file_read(file, 0, text, size))
	{
		text[size] = 0;
		json.text = text;
		jsmn_init(&parser);
		json.count = jsmn_parse(&parser, text, size, json.tokens, MAXIMUM_TOKENS);
		if (json.count <= 0 || json.tokens[0].type != JSMN_OBJECT)
		{
			error(_error_silent, "mods: %s left out: it is not JSON (or has more than %d values)",
				port_mods_file_path(file), MAXIMUM_TOKENS);
		}
		else if (json_real(&json, json_member(&json, 0, "format"), 0.0f) != OVERLAY_FORMAT)
		{
			error(_error_silent, "mods: %s left out: it is not of format %d", port_mods_file_path(file),
				OVERLAY_FORMAT);
		}
		else
		{
			classes_read(&json, json_member(&json, 0, "classes"));
			count = shaders_read(&json, json_member(&json, 0, "shaders"));
			error(_error_silent, "mods: %s overlays %ld shaders of the map with %d classes", port_mods_file_path(file),
				count, mod_materials_globals.class_count);
			if (mods_say())
			{
				console_printf(FALSE, "mods: the material overlay of %s covers %ld shaders", port_mods_file_mod(file),
					count);
			}
		}
	}
	free(json.tokens);
	free(text);

	return count;
}

/* the settings, read again if they have changed */
static void settings_read(
	void)
{
	if (mod_materials_globals.settings_read == config_changes() + 1)
	{
		return;
	}
	mod_materials_globals.settings_read = config_changes() + 1;
	mod_materials_globals.enabled = config_boolean("display.prism_material_overlay") != 0;
	mod_materials_globals.strength = (real)config_real("display.prism_overlay_strength");
	if (!mods_finite_real(mod_materials_globals.strength) || mod_materials_globals.strength < 0.0f)
	{
		mod_materials_globals.strength = 0.0f;
	}
	/* (0 to 2, as config.toml's comment says) */
	if (mod_materials_globals.strength > 2.0f)
	{
		mod_materials_globals.strength = 2.0f;
	}
	mod_materials_globals.view = (int)config_integer("display.prism_overlay_view");
	if (mod_materials_globals.view < 0 || mod_materials_globals.view > 4)
	{
		mod_materials_globals.view = 0;
	}
	mod_materials_globals.surface_maps = config_boolean("display.prism_surface_maps") != 0;

	return;
}

static real smooth_step(
	real lo,
	real hi,
	real x)
{
	real t = PIN((x - lo) / (hi - lo), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

/* ---------- public code */

long mod_materials_update(
	boolean remove_all,
	boolean at_load)
{
	long count;

	if (!mod_materials_table_cleared)
	{
		table_clear();
	}

	materials_dispose();
	if (remove_all)
	{
		return 0;
	}
	count = overlay_read();
	/* (the tiles decoded as the map loads with the overlay on; otherwise
	when it is first turned on) */
	settings_read();
	/* (PRISM v2's probes, as the map loads) */
	if (at_load)
	{
		prism_light_map_loaded();
	}
	if (count && mod_materials_globals.enabled && !config_boolean("debug.null_renderer") && d3d8_material_overlay_drawn())
	{
		tiles_start();
	}

	return count;
}

void mod_materials_tags_unloaded(
	void)
{
	materials_dispose();
	prism_light_map_unloaded();

	return;
}

void mod_materials_draw_begin(
	struct shader const *shader,
	real camera_distance,
	boolean has_multipurpose,
	long base_map_index,
	short permutation_index)
{
	struct overlay_shader const *entry = NULL;
	float uniforms[OVERLAY_UNIFORM_COUNT][4];
	unsigned int tiles[NUMBER_OF_SLOTS];
	unsigned int mask_texture = 0;
	unsigned int surface_textures[2] = { 0, 0 };
	unsigned int probes = 0;
	boolean overlay = FALSE;
	boolean prism = FALSE;
	int prism_lighting;
	short surface = NONE;
	short bitmap = 0;
	short slot;
	short slot_count = 2;

	settings_read();
	/* (a renderer without the overlay: Metal's, until its shaders match
	Vulkan's) */
	/* (PRISM v2's model light, prism_light.c: every model shader's draws,
	with or without an overlay) */
	prism = prism_light_wanted() != 0;
	if ((!mod_materials_globals.enabled && !mod_materials_globals.surface_maps && !prism) || !d3d8_material_overlay_drawn())
	{
		return;
	}
	/* the base map's bitmap the draw's permutation picks, as
	rasterizer_set_texture does */
	if (base_map_index != NONE && permutation_index > 0)
	{
		long count = bitmap_group_get(base_map_index)->bitmaps.count;

		bitmap = count > 0 ? (short)(permutation_index % count) : 0;
	}
	if (mod_materials_globals.shader_count)
	{
		entry = shader_find(shader);
	}
	/* the overlay's tiles, where overlay.json names the shader and they are
	ready (they are made the first time they are drawn: with the setting off
	the renderer makes nothing for the overlay) */
	if (mod_materials_globals.enabled && entry)
	{
		overlay = class_tile_load(&mod_materials_globals.classes[entry->slots[_slot_a]]) &&
			class_tile_load(&mod_materials_globals.classes[entry->slots[_slot_b]]);
	}
	/* the surface map of the base map's bitmap (display.prism_surface_maps):
	the one overlay.json names for the shader, else the one next to the
	bitmap's tag (whether or not overlay.json names the shader); once it is
	ready */
	if (mod_materials_globals.surface_maps)
	{
		if (entry && entry->surface_count)
		{
			surface = bitmap < entry->surface_count ? entry->surfaces[bitmap] : NONE;
		}
		else
		{
			surface = surface_of_bitmap(base_map_index, bitmap);
		}
		if (surface != NONE && class_tile_load(&mod_materials_globals.classes[surface]))
		{
			surface_textures[0] = mod_materials_globals.classes[surface].texture;
			surface_textures[1] = mod_materials_globals.classes[surface].texture2;
		}
	}
	if (!overlay && !surface_textures[0] && !prism)
	{
		return;
	}
	if (overlay)
	{
		/* the class mask, once it and the other tiles are ready; until
		then, or without one, two classes */
		if (entry->mask_count)
		{
			short mask = bitmap < entry->mask_count ? entry->masks[bitmap] : NONE;

			if (mask != NONE && class_tile_load(&mod_materials_globals.classes[mask]) &&
				class_tile_load(&mod_materials_globals.classes[entry->slots[_slot_c]]) &&
				class_tile_load(&mod_materials_globals.classes[entry->slots[_slot_d]]))
			{
				mask_texture = mod_materials_globals.classes[mask].texture;
				slot_count = NUMBER_OF_SLOTS;
			}
		}
		memcpy(uniforms, entry->uniforms, sizeof(uniforms));
	}
	else
	{
		/* (the surface map alone: xgpu.h's XGPU_OVERLAY_SURFACE_ONLY. The
		self-illuminated texels, the multipurpose map's green above a half,
		left alone as overlay.json's exclude leaves them; the reflection bent
		fully by the surface map's normal and darkened by half its
		occlusion) */
		memset(uniforms, 0, sizeof(uniforms));
		uniforms[1][0] = -2.0f;
		uniforms[1][1] = -1.0f;
		uniforms[2][1] = 1.0f;
		uniforms[3][0] = 0.5f;
		uniforms[6][0] = 1.0f;
		uniforms[6][1] = 0.5f;
	}
	if (entry)
	{
		uniforms[10][0] = (float)entry->surface[0];
		uniforms[10][1] = (float)entry->surface[1];
	}
	else
	{
		/* (overlay.json's defaults) */
		uniforms[10][0] = 1.0f;
		uniforms[10][1] = 1.0f;
	}
	/* (PRISM v2's model light: [11] on) */
	prism_lighting = prism ? prism_light_draw(uniforms, &probes) : 0;
	/* (no multipurpose map: the default white texture, which is no mask) */
	uniforms[1][3] = has_multipurpose ? 1.0f : 0.0f;
	if (!has_multipurpose)
	{
		uniforms[3][0] = 2.0f;
	}
	uniforms[3][1] = 1.0f;
	if (!overlay)
	{
		d3d8_set_material_overlay(uniforms[0], NULL, 0, mod_materials_globals.view, FALSE, surface_textures, TRUE,
			prism_lighting, probes);
		mod_materials_globals.armed = TRUE;
		return;
	}
	for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
	{
		struct overlay_class const *overlay_class = &mod_materials_globals.classes[entry->slots[slot]];
		real fade = (1.0f - smooth_step(overlay_class->fade[0], overlay_class->fade[1], camera_distance)) *
			entry->strength * mod_materials_globals.strength;

		if (slot >= slot_count)
		{
			/* (two classes: C and D are not drawn) */
			tiles[slot] = 0;
			continue;
		}
		tiles[slot] = overlay_class->texture;
		uniforms[slot_uniform[slot]][1] = overlay_class->normal * fade;
		uniforms[slot_uniform[slot]][2] = overlay_class->cavity * fade;
		uniforms[slot_uniform[slot]][3] = overlay_class->rough * fade;
	}
	d3d8_set_material_overlay(uniforms[0], tiles, mask_texture, mod_materials_globals.view, entry->mask_wins,
		surface_textures[0] ? surface_textures : NULL, FALSE, prism_lighting, probes);
	mod_materials_globals.armed = TRUE;

	return;
}

void mod_materials_draw_end(
	void)
{
	if (mod_materials_globals.armed)
	{
		d3d8_set_material_overlay(NULL, NULL, 0, 0, 0, NULL, 0, 0, 0);
		mod_materials_globals.armed = FALSE;
	}

	return;
}

/* (the script function prism_set) "<setting> <value>": one of the
display.prism_* settings, set as config.toml's line is (config_write) */
void mod_materials_prism_set(
	char const *text)
{
	char name[96];
	char const *space = text ? strchr(text, ' ') : NULL;

	if (!space || space == text || (size_t)(space - text) >= sizeof(name) - 16)
	{
		console_warning("prism_set \"<setting> <value>\": material_overlay, overlay_strength, overlay_view, surface_maps, "
			"specular, reflections or shadows");
		return;
	}
	snprintf(name, sizeof(name), "display.prism_%.*s", (int)(space - text), text);
	if (!config_write(name, space + 1))
	{
		console_warning("%s cannot be set to %s", name, space + 1);
		return;
	}
	console_printf(FALSE, "%s = %s", name, space + 1);

	return;
}
