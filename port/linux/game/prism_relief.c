/*
PRISM_RELIEF.C

The relief tier of PRISM v4's materials (docs/prism_materials.md, "The
relief tier"): the environment shaders of a relief class (RELIEF_PATTERNS:
Forerunner metal's floors, walls and panels) given a normal map of their
designs raised and sunk, which the G-buffer pass draws in place of the
vertices' normal under Enhanced and Path Traced lighting, so that the sun,
the flashlight, the dynamic lights and the bake's light (read at the
pixel's normal by its direction, docs/prism_bake.md) all show it.

Each is made on the player's machine from the player's own bitmaps as the
map loads (prism_relief_gen.c: CE's bump map's relief, else the base map's
design), kept in <data>/prism_relief/<key>.<bump|base>.ktx2 (the key every
value it is made of and PRISM_RELIEF_VERSION; a shader with no design
<key>.none) for the next load, and never shipped: Settings' Clear Caches
deletes them (game_caches.c). prism_materials.c sends it to the texture
store as a pack's image and draws it as a material of the pack tier with
the relief flag (its colour, roughness, metal and specular the automatic
tier's).

The classes are the enhancements' (display.surface_relief, Materials &
Terrain): tools/port_settings.py's RELIEF_CLASSES is RELIEF_PATTERNS' twin
(tools/test_prism_relief.py checks that they agree). A class is a list of
shader tag paths, '*' any run of characters, case not minded.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/texture_cache.h"
#include "tag_files/tag_groups.h"
#include "mods.h"
#include "prism_relief_gen.h"
#include "../src/posix.h"
#include "../src/xgpu_coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator, as prism_materials.c's) */
#undef malloc
#undef free

/* the platform's (port/linux/src) */
char const *platform_data_root(void);
unsigned long long SDL_GetTicksNS(void);
/* (sys/stat.h's, which the game's strict ANSI hides: world_sdf.c's) */
int mkdir(const char *path, unsigned int mode);

/* ---------- constants */

/* the environment shader's bump map (rasterizer_xbox_environment.c's
diffuse properties at 0x6C: its bump map scale at 0xB8, the reference
after) and base map; a tag reference's index, 12 bytes in */
#define SENV_BASE_MAP 0x88
#define SENV_BUMP_MAP 0x128
#define REFERENCE_INDEX 12
/* bitmaps.c's P8 bump format, whose texels are global_vector_palette's */
#define BITMAP_FORMAT_P8_BUMP 17
#define MAXIMUM_PATH 1024

/* the relief classes: each shader tag path a class takes (the enhancements'
registry, tools/port_settings.py's RELIEF_CLASSES, is its twin) */
static struct
{
	char const *class_name;
	char const *pattern;
} const RELIEF_PATTERNS[] =
{
	/* Forerunner metal: b30's shaders (a30, b30, b40, c10, c20 and c40 draw
	them), b40's own (b40 and c40) and c20's own; their grates, lamps,
	glass and mirrors are not metal plate */
	{ "forerunner_metal", "levels\\b30\\shaders\\metal *" },
	{ "forerunner_metal", "levels\\b30\\shaders\\decorative *" },
	{ "forerunner_metal", "levels\\b30\\shaders\\tech dense circuits" },
	{ "forerunner_metal", "levels\\b40\\shaders\\b40_metal *" },
	{ "forerunner_metal", "levels\\b40\\shaders\\b40_panels *" },
	{ "forerunner_metal", "levels\\b40\\shaders\\b40_bridgeramp*" },
	{ "forerunner_metal", "levels\\b40\\shaders\\b40_cliffmetal*" },
	{ "forerunner_metal", "levels\\b40\\shaders\\b40_generator *" },
	{ "forerunner_metal", "levels\\c20\\shaders\\metal *" },
	{ "forerunner_metal", "levels\\c20\\shaders\\c20metal *" },
	{ "forerunner_metal", "levels\\c20\\shaders\\c20column*" },
};

#define NUMBER_OF_RELIEF_PATTERNS ((long)(sizeof(RELIEF_PATTERNS) / sizeof(RELIEF_PATTERNS[0])))

/* ---------- globals */

static struct
{
	/* this map's: shaders with relief, made now, found in the cache, and
	the milliseconds the made ones took */
	long count, made, cached;
	unsigned long long nanoseconds;
} prism_relief_globals;

/* ---------- private code */

static int lower(int character)
{
	return character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character;
}

/* text matched by a pattern ('*' any run of characters), case not minded */
static boolean pattern_matches(char const *pattern, char const *text)
{
	while (*pattern)
	{
		if (*pattern == '*')
		{
			pattern++;
			for (;;)
			{
				if (pattern_matches(pattern, text))
					return TRUE;
				if (!*text)
					return FALSE;
				text++;
			}
		}
		if (!*text || lower((unsigned char)*pattern) != lower((unsigned char)*text))
			return FALSE;
		pattern++;
		text++;
	}

	return !*text;
}

/* a bitmap reference's bitmap tag, NONE none */
static long reference_tag(void const *shader, long offset)
{
	long index;

	memcpy(&index, (char const *)shader + offset + REFERENCE_INDEX, sizeof(index));
	return index;
}

/* a bitmap tag's first bitmap's texels (malloc's, ARGB words; a P8 bump
map's by its palette), at most PRISM_RELIEF_SIDE_MAXIMUM a side; NULL where
it cannot be read. Its texels are loaded as a draw loads them, and waited
for (prism_rt.c's bitmap_mean_as) */
static unsigned int *bitmap_texels(long bitmap_tag, unsigned long *width, unsigned long *height)
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	unsigned long const *header;
	unsigned int palette[256], *texels;
	int index;

	if (bitmap_tag == NONE || !(group = bitmap_group_get(bitmap_tag)) || !group->bitmaps.address ||
		group->bitmaps.count <= 0)
	{
		return NULL;
	}
	header = texture_cache_port_load((unsigned long)bitmap_tag, 0);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (!header || !bitmap->base_address)
	{
		return NULL;
	}
	for (index = 0; index < 256; index++)
	{
		palette[index] = (unsigned int)global_vector_palette[index];
	}
	texels = malloc((size_t)PRISM_RELIEF_SIDE_MAXIMUM * PRISM_RELIEF_SIDE_MAXIMUM * sizeof(*texels));
	if (!texels || !xgpu_texture_argb_map((unsigned int)header[3], (unsigned int)header[4],
		(unsigned char const *)bitmap->base_address, bitmap->format == BITMAP_FORMAT_P8_BUMP ? palette : NULL,
		PRISM_RELIEF_SIDE_MAXIMUM, texels, width, height))
	{
		free(texels);
		return NULL;
	}

	return texels;
}

static boolean file_size(char const *path, unsigned long *size)
{
	struct posix_file_information information;

	if (posix_stat(path, &information) != 0 || (information.flags & _posix_file_is_directory))
	{
		return FALSE;
	}
	*size = (unsigned long)information.size_low;

	return TRUE;
}

/* bytes written whole to path (the game's: from the data folder, as
port/linux/include/stdio.h's fopen finds it; a .tmp beside it renamed over
it); FALSE if not */
static boolean file_write(char const *path, unsigned char const *bytes, unsigned long size)
{
	char temporary[MAXIMUM_PATH + 8];
	FILE *file;
	boolean written;

	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
	{
		return FALSE;
	}
	written = fwrite(bytes, 1, size, file) == size;
	written = fclose(file) == 0 && written;
	/* (Windows renames over nothing) */
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
	{
		remove(temporary);
	}

	return written;
}

/* ---------- public code */

/* the relief class a shader's tag path is of; NULL none */
char const *prism_relief_class(
	char const *tag_name)
{
	long index;

	for (index = 0; tag_name && index < NUMBER_OF_RELIEF_PATTERNS; index++)
	{
		if (pattern_matches(RELIEF_PATTERNS[index].pattern, tag_name))
		{
			return RELIEF_PATTERNS[index].class_name;
		}
	}

	return NULL;
}

/* (prism_materials.c, as a map's materials are read) this map's counts begun */
void prism_relief_begin(
	void)
{
	memset(&prism_relief_globals, 0, sizeof(prism_relief_globals));

	return;
}

/* ... and said */
void prism_relief_end(
	void)
{
	if (prism_relief_globals.count)
	{
		error(_error_silent, "relief: %ld of the map's shaders have relief (%ld made now in %lu ms, %ld from the cache)",
			prism_relief_globals.count, prism_relief_globals.made,
			(unsigned long)(prism_relief_globals.nanoseconds / 1000000ULL), prism_relief_globals.cached);
	}

	return;
}

/* an environment shader's relief: its normal map's file (made where the
cache has none) into path, *at_bump whether it is at the bump map's
coordinates; FALSE if the shader is of no relief class or has no design */
boolean prism_relief_image(
	void const *shader,
	char const *tag_name,
	char *path,
	unsigned long size,
	boolean *at_bump)
{
	struct prism_relief_source source;
	struct prism_relief relief;
	unsigned long width = 0, height = 0, bytes = 0;
	unsigned long long start;
	char folder[MAXIMUM_PATH], stem[MAXIMUM_PATH], none[MAXIMUM_PATH], name[64], written[MAXIMUM_PATH];
	unsigned int *base, *bump;
	unsigned char *file;
	uint64_t key;
	int status;

	if (!shader || !prism_relief_class(tag_name))
	{
		return FALSE;
	}
	start = SDL_GetTicksNS();
	memset(&source, 0, sizeof(source));
	base = bitmap_texels(reference_tag(shader, SENV_BASE_MAP), &width, &height);
	source.base = base;
	source.base_width = width;
	source.base_height = height;
	bump = bitmap_texels(reference_tag(shader, SENV_BUMP_MAP), &width, &height);
	source.bump = bump;
	source.bump_width = width;
	source.bump_height = height;
	if (!base && !bump)
	{
		return FALSE;
	}
	key = prism_relief_key(&source);
	snprintf(folder, sizeof(folder), "%s/prism_relief", platform_data_root());
	/* (the host's paths, which the texture store opens and stat reads; the
	game's own, from the data folder, which its fopen writes) */
	snprintf(name, sizeof(name), "%08lx%08lx", (unsigned long)(key >> 32), (unsigned long)(key & 0xFFFFFFFFUL));
	snprintf(stem, sizeof(stem), "%s/%s", folder, name);
	snprintf(none, sizeof(none), "%s.none", stem);
	/* (made before: the cache's) */
	if (snprintf(path, size, "%s.bump.ktx2", stem) < (int)size && file_size(path, &bytes) && bytes)
	{
		*at_bump = TRUE;
	}
	else if (snprintf(path, size, "%s.base.ktx2", stem) < (int)size && file_size(path, &bytes) && bytes)
	{
		*at_bump = FALSE;
	}
	else if (file_size(none, &bytes))
	{
		free(base);
		free(bump);
		return FALSE;
	}
	if (bytes)
	{
		free(base);
		free(bump);
		prism_relief_globals.count++;
		prism_relief_globals.cached++;
		return TRUE;
	}
	status = prism_relief_make(&source, &relief);
	free(base);
	free(bump);
	mkdir(folder, 0755);
	if (status != PRISM_RELIEF_MADE)
	{
		error(_error_silent, "relief: %s: none (%s)", tag_name, status == PRISM_RELIEF_NO_DESIGN ? "no design" :
			status == PRISM_RELIEF_SIZE ? "its bitmaps' size" : "no memory");
		if (status == PRISM_RELIEF_NO_DESIGN)
		{
			snprintf(written, sizeof(written), "prism_relief/%s.none", name);
			file_write(written, (unsigned char const *)"", 0);
		}
		return FALSE;
	}
	*at_bump = relief.at_bump != 0;
	file = prism_relief_ktx2(relief.normal, relief.width, relief.height, &bytes);
	snprintf(path, size, "%s.%s.ktx2", stem, *at_bump ? "bump" : "base");
	snprintf(written, sizeof(written), "prism_relief/%s.%s.ktx2", name, *at_bump ? "bump" : "base");
	if (!file || !file_write(written, file, bytes))
	{
		error(_error_silent, "relief: %s: %s could not be written", tag_name, path);
		free(file);
		prism_relief_free(&relief);
		return FALSE;
	}
	error(_error_silent, "relief: %s: made %lux%lu at the %s map's coordinates (the bump map's height %ld/256, "
		"the base map's design %ld, depth %ld/256 of a texel)", tag_name, relief.width, relief.height,
		*at_bump ? "bump" : "base", relief.bump_range, relief.base_range, relief.depth);
	free(file);
	prism_relief_free(&relief);
	prism_relief_globals.count++;
	prism_relief_globals.made++;
	prism_relief_globals.nanoseconds += SDL_GetTicksNS() - start;

	return TRUE;
}
