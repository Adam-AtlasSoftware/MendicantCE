/* prism_light.c: PRISM v2's model light (display.prism_specular,
display.prism_reflections, display.prism_shadows; all off unless set), the
game's half. The renderer's is xgpu_spirv.c's prism_* (xgpu.h's "PRISM v2's
model light"); mod_materials.c arms a model shader's draws with a material
overlay that carries it (with the overlay's tiles, a surface map, or neither)
and this file fills the overlay's uniform from [11] on:

- the highlights' strength, roughness, least roughness and cap
  (display.prism_specular "subtle" or "full");
- the shadows' strength, reach, softness and push off the surface, and the
  distance-field scene's cascades (world_ao.c's, as the window has them:
  display.ambient_occlusion "world" builds them; without it, none);
- the two probes nearest the object drawn (its bounding sphere's centre,
  world_ao.c's world_ao_object_begin; the camera for the first-person
  weapon), from the map's probes file (display.prism_reflections "local").

The probes file (<data>/prism/probes/<map>.probes, little-endian) is made
on this machine by tools/prism_probes.py from the game's own renderer (each
probe's six faces captured in a hidden run, folded octahedrally and their
levels blurred): "PRBS", version 1, the probes' count, each cell's side in
texels, the cells a side of the atlas, the atlas's levels; then each probe's
place (three floats) and reach (a float); then the atlas, RGBA, every level,
level 0 first, each half the one before. It is read as the map loads and
whenever display.prism_reflections turns "local"; a file that does not
match is ignored (and the map drawn with its own cube maps). Derived from
the player's maps: never shipped. */

#include "cseries.h"
#include "errors.h"
#include "cache/cache_files.h"
#include "objects/objects.h"
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator: the atlas is an image the renderer frees
with the library's free, xgpu_images.c) */
#undef malloc
#undef free

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
int config_boolean(char const *name);
unsigned long config_changes(void);
const char *platform_data_root(void);
void platform_log(const char *format, ...);
unsigned int xgpu_image_create(void *pixels, unsigned long width, unsigned long height, unsigned long levels,
	int format, int owned);
void xgpu_image_delete(unsigned int image);
/* the game's (port/linux/game/world_ao.c) */
int world_ao_window_cascades(float cascades[4][4]);

/* ---------- constants */

/* (xgpu.h's XGPU_PRISM_*, XGPU_OVERLAY_UNIFORM_COUNT and XGPU_IMAGE_RGBA_LEVELS:
the game's sources do not see the renderer's headers) */
#define PRISM_SPECULAR_SUBTLE 1
#define PRISM_SPECULAR_FULL 2
#define PRISM_REFLECTIONS_LOCAL 4
#define PRISM_SHADOWS_SOFT 8
#define PRISM_SHADOWS_TRACED 16
#define PRISM_UNIFORM_FIRST 11
#define PRISM_UNIFORM_COUNT 20
#define PRISM_IMAGE_RGBA_LEVELS 2

#define PROBES_MAGIC 0x53425250 /* "PRBS" */
#define PROBES_VERSION 1
#define PROBES_MAXIMUM 256
#define PROBES_CELL_MAXIMUM 512
#define PROBES_ATLAS_MAXIMUM 4096

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	int specular, reflections, shadows;
	/* the map's probes (count 0: none), their atlas's image and layout */
	char map[64];
	int probes_tried;
	long probe_count;
	float probes[PROBES_MAXIMUM][4];
	unsigned int atlas;
	unsigned long cell, cells;
	/* where the draws that follow are: the object's centre or the camera's */
	float camera[3], at[3];
	int at_valid;
} prism_light;

/* ---------- private code */

static int setting_is(
	char const *name,
	char const *value)
{
	char const *setting = config_string(name);

	return setting && !strcmp(setting, value);
}

static void settings_read(
	void)
{
	if (prism_light.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_light.settings_read = config_changes() + 1;
	prism_light.specular = setting_is("display.prism_specular", "full") ? PRISM_SPECULAR_FULL :
		setting_is("display.prism_specular", "subtle") ? PRISM_SPECULAR_SUBTLE : 0;
	prism_light.reflections = setting_is("display.prism_reflections", "local");
	prism_light.shadows = setting_is("display.prism_shadows", "traced") ? PRISM_SHADOWS_TRACED :
		setting_is("display.prism_shadows", "soft") ? PRISM_SHADOWS_SOFT : 0;

	return;
}

static unsigned long read_u32(
	unsigned char const *at)
{
	return (unsigned long)at[0] | ((unsigned long)at[1] << 8) | ((unsigned long)at[2] << 16) |
		((unsigned long)at[3] << 24);
}

static float read_float(
	unsigned char const *at)
{
	unsigned long bits = read_u32(at);
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void probes_release(
	void)
{
	if (prism_light.atlas)
	{
		xgpu_image_delete(prism_light.atlas);
	}
	prism_light.atlas = 0;
	prism_light.probe_count = 0;
	prism_light.probes_tried = FALSE;
	prism_light.map[0] = 0;

	return;
}

/* the map's probes file read, checked and its atlas made an image */
static void probes_load(
	void)
{
	char path[512];
	char const *map = cache_file_loaded_map_name();
	FILE *file;
	unsigned char header[24];
	unsigned long count, cell, cells, levels, side, bytes, level;
	unsigned char *places = NULL, *pixels = NULL;
	long index;

	prism_light.probes_tried = TRUE;
	if (!map || !*map || strlen(map) >= sizeof(prism_light.map))
	{
		return;
	}
	strcpy(prism_light.map, map);
	/* (relative to the data root: the game's files are opened by Xbox path,
	port/linux/src/xbox_files.c) */
	snprintf(path, sizeof(path), "prism/probes/%s.probes", map);
	file = fopen(path, "rb");
	if (!file)
	{
		platform_log("prism: no probes for %s (%s/%s: tools/prism_probes.py makes them): drawn with its cube maps", map,
			platform_data_root(), path);
		return;
	}
	if (fread(header, 1, sizeof(header), file) != sizeof(header) || read_u32(header) != PROBES_MAGIC ||
		read_u32(header + 4) != PROBES_VERSION)
	{
		platform_log("prism: %s is not a probes file (version %d): drawn with the maps' cube maps", path,
			PROBES_VERSION);
		fclose(file);
		return;
	}
	count = read_u32(header + 8);
	cell = read_u32(header + 12);
	cells = read_u32(header + 16);
	levels = read_u32(header + 20);
	side = cell * cells;
	if (!count || count > PROBES_MAXIMUM || cell < 8 || cell > PROBES_CELL_MAXIMUM || (cell & (cell - 1)) ||
		!cells || cells * cells < count || side > PROBES_ATLAS_MAXIMUM || !levels || (cell >> (levels - 1)) < 1)
	{
		platform_log("prism: %s: %lu probes of %lu texels, %lu a side, %lu levels: not drawn", path, count, cell,
			cells, levels);
		fclose(file);
		return;
	}
	for (bytes = 0, level = 0; level < levels; level++)
	{
		unsigned long level_side = side >> level ? side >> level : 1;

		bytes += level_side * level_side * 4;
	}
	places = malloc(count * 16);
	pixels = malloc(bytes);
	if (!places || !pixels || fread(places, 1, count * 16, file) != count * 16 || fread(pixels, 1, bytes, file) != bytes)
	{
		platform_log("prism: %s is short: not drawn", path);
		free(places);
		free(pixels);
		fclose(file);
		return;
	}
	fclose(file);
	for (index = 0; index < (long)count; index++)
	{
		prism_light.probes[index][0] = read_float(places + index * 16);
		prism_light.probes[index][1] = read_float(places + index * 16 + 4);
		prism_light.probes[index][2] = read_float(places + index * 16 + 8);
		prism_light.probes[index][3] = read_float(places + index * 16 + 12);
	}
	free(places);
	prism_light.atlas = xgpu_image_create(pixels, side, side, levels, PRISM_IMAGE_RGBA_LEVELS, TRUE);
	if (!prism_light.atlas)
	{
		return;
	}
	prism_light.probe_count = (long)count;
	prism_light.cell = cell;
	prism_light.cells = cells;
	platform_log("prism: %s: %lu probes, %lu-texel cells, %lu levels", path, count, cell, levels);

	return;
}

/* the two probes nearest the point (indices; -1 none) */
static void probes_nearest(
	float const at[3],
	long nearest[2])
{
	float best[2] = { 1.0e30f, 1.0e30f };
	long index;

	nearest[0] = nearest[1] = -1;
	for (index = 0; index < prism_light.probe_count; index++)
	{
		float dx = prism_light.probes[index][0] - at[0];
		float dy = prism_light.probes[index][1] - at[1];
		float dz = prism_light.probes[index][2] - at[2];
		float distance = dx * dx + dy * dy + dz * dz;

		if (distance < best[0])
		{
			best[1] = best[0];
			nearest[1] = nearest[0];
			best[0] = distance;
			nearest[0] = index;
		}
		else if (distance < best[1])
		{
			best[1] = distance;
			nearest[1] = index;
		}
	}

	return;
}

/* ---------- public code */

void prism_light_map_loaded(
	void)
{
	probes_release();
	settings_read();
	if (prism_light.reflections && !config_boolean("debug.null_renderer"))
	{
		probes_load();
	}

	return;
}

void prism_light_map_unloaded(
	void)
{
	probes_release();

	return;
}

void prism_light_window_begin(
	float const camera[3])
{
	prism_light.camera[0] = camera[0];
	prism_light.camera[1] = camera[1];
	prism_light.camera[2] = camera[2];
	prism_light.at_valid = FALSE;

	return;
}

void prism_light_object_begin(
	long object_index)
{
	real_point3d center;
	real radius;

	prism_light.at_valid = FALSE;
	if (object_index == NONE || !prism_light.probe_count)
	{
		return;
	}
	object_get_bounding_sphere(object_index, &center, &radius);
	prism_light.at[0] = center.x;
	prism_light.at[1] = center.y;
	prism_light.at[2] = center.z;
	prism_light.at_valid = TRUE;

	return;
}

int prism_light_draw(
	float uniforms[][4],
	unsigned int *probes)
{
	int lighting = 0;

	settings_read();
	*probes = 0;
	if (prism_light.specular)
	{
		/* subtle: softer and dimmer, its sharpest at a roughness of 0.35, at
		most half the light again; full: at most the light again */
		static float const parameters[2][4] = { { 0.6f, 0.55f, 0.35f, 0.5f }, { 1.0f, 0.45f, 0.15f, 1.0f } };

		lighting |= prism_light.specular;
		memcpy(uniforms[11], parameters[prism_light.specular - 1], sizeof(uniforms[11]));
	}
	if (prism_light.shadows && world_ao_window_cascades((float (*)[4])uniforms[13]))
	{
		/* soft: out to 2 world units, four rays tilted 0.12 about the light;
		traced: one ray out to 6 (each pushed 0.04 off the surface) */
		static float const parameters[2][4] = { { 0.7f, 2.0f, 3.0f, 0.04f }, { 0.85f, 6.0f, 0.0f, 0.04f } };

		lighting |= prism_light.shadows;
		memcpy(uniforms[12], parameters[prism_light.shadows == PRISM_SHADOWS_TRACED], sizeof(uniforms[12]));
	}
	if (prism_light.reflections)
	{
		long nearest[2];

		/* (the map's probes, read when the setting turned on in play) */
		if (!prism_light.probes_tried || strcmp(prism_light.map, cache_file_loaded_map_name() ?
			cache_file_loaded_map_name() : ""))
		{
			probes_release();
			if (!config_boolean("debug.null_renderer"))
			{
				probes_load();
			}
		}
		if (prism_light.probe_count)
		{
			probes_nearest(prism_light.at_valid ? prism_light.at : prism_light.camera, nearest);
			uniforms[17][0] = nearest[1] >= 0 ? 2.0f : 1.0f;
			uniforms[17][1] = (float)prism_light.cells;
			uniforms[17][2] = (float)prism_light.cell;
			uniforms[17][3] = 1.0f;
			memcpy(uniforms[18], prism_light.probes[nearest[0]], sizeof(uniforms[18]));
			uniforms[18][3] = (float)nearest[0];
			memcpy(uniforms[19], prism_light.probes[nearest[1] >= 0 ? nearest[1] : nearest[0]], sizeof(uniforms[19]));
			uniforms[19][3] = (float)(nearest[1] >= 0 ? nearest[1] : nearest[0]);
			lighting |= PRISM_REFLECTIONS_LOCAL;
			*probes = prism_light.atlas;
		}
	}

	return lighting;
}

/* whether any of PRISM v2's model light is set (the material overlay is
armed for every model shader's draw then) */
int prism_light_wanted(
	void)
{
	settings_read();
	return prism_light.specular || prism_light.reflections || prism_light.shadows;
}
