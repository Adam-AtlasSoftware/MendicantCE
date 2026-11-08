/*
PRISM_RT.C

display.prism_rt (docs/prism_rt.md): PRISM's hardware ray tracing, the
game's half. Off unless set, and where the renderer cannot trace rays
(halo_prism_rt_available: xgpu_backend_capabilities' ray_tracing) nothing
here runs and nothing is sent.

With it on:

- world AO's pre-pass is drawn whatever display.ambient_occlusion says
  (world_ao_mode): its inputs (each visible pixel's normal and distance,
  whether it is the level's or an object's) are the ray tracer's G-buffer,
  and its pass's place in the frame is where the backend traces;
- the structure BSP's opaque triangles (its render geometry: the
  environment shaders' surfaces, not the alpha-tested, transparent or
  water ones) are copied once when a BSP is first drawn, in world space,
  and sent a slice a frame (RT_STATIC), never while a map loads: the
  backend builds its acceleration structure of them and traces the last
  whole set meanwhile;
- each frame, before its windows, RT_FRAME: the settings, the sky's sun
  (as render_sky aims its lens flare: the brightest light of the window's
  sky), its colour, the sky's outdoor ambient, the game clock and the
  frame's dynamic lights (world_scene's list, ranked).

The objects are not described here: the backend captures them from their
draws in the pre-pass, as they are drawn (their permutations, levels of
detail and pose).

Nothing here changes what the game simulates: it draws, and reads only the
camera's sky, the structure's render geometry (read-only once loaded) and
the settings. Nothing crosses the network.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "rasterizer/rasterizer_geometry.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "scenario/sky_definitions.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "world_scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src */
char const *config_string(char const *name);
long config_integer(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
/* d3d8_device.c's (the renderer's: xgpu_backend_rt_*) */
int halo_prism_rt_available(void);
void halo_prism_rt_static(int phase, unsigned long generation, unsigned long first, unsigned long count,
	unsigned long triangles, float const *positions);
void halo_prism_rt_frame(void const *frame);
/* render_enumerate.c's (the window drawn is a map's first frame's, unseen) */
extern unsigned char render_enumerating;
/* render_sky.c's (the sky's sun, as PRISM's cascaded shadows find it) */
boolean render_sky_sun(short sky_index, real_vector3d *direction, real_rgb_color *color, real *power);
/* game.h's; render_interpolation.c's (the drawn frame's place between ticks) */
long game_time_get(void);
float render_interpolation_fraction(void);

/* ---------- constants */

/* (xgpu_stream.h's: the game's sources do not see the renderer's headers) */
enum
{
	PRISM_RT_OFF,
	PRISM_RT_SHADOWS,
	PRISM_RT_GI,
	PRISM_RT_PATHTRACED,
	PRISM_RT_STATIC_BEGIN = 0,
	PRISM_RT_STATIC_TRIANGLES,
	PRISM_RT_STATIC_END,
	PRISM_RT_STATIC_RELEASE,
	PRISM_RT_SLICE = 65536,
	PRISM_RT_LIGHTS = 64,
	/* the environment shaders (shaders.c's _shader_type_environment) and
	their alpha test (their flags' bit 0) */
	PRISM_RT_SHADER_ENVIRONMENT = 3,
	PRISM_RT_ENVIRONMENT_ALPHA_TESTED = 1,
};

/* the share of a sunlit lightmap's light the moving casters take away (the
level's own shadows are in its lightmap), and of a model's level light
(its ambient and distant lights together, which the occlusion's slot
multiplies: xgpu.h's XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX); as PRISM's
cascaded shadows take them (prism-world2's PRISM_SHADOWS_STRENGTH,
_MODEL_STRENGTH) */
#define PRISM_RT_SUN_STRENGTH 0.65f
#define PRISM_RT_MODEL_STRENGTH 0.55f

/* RT_FRAME's body (xgpu_stream.h's struct xgpu_stream_rt_frame, field for
field) */
struct prism_rt_frame
{
	unsigned int header;
	unsigned int mode, quality, resolution, denoiser;
	unsigned int reflections, objects, rate, mood;
	unsigned int view, light_count, frame, ticks;
	float fraction;
	unsigned int generation;
	unsigned int pad[2];
	float settings[16];
	float sun[4];
	float sun_color[4];
	float sky[4];
	float spare[4];
	float lights[PRISM_RT_LIGHTS][12];
};

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	int mode, quality, resolution, denoiser, reflections, objects, rate, mood, view;
	/* the structure's triangles: the BSP they are of (-1 none), their
	generation, the copy (9 floats each), how many, how many sent */
	short bsp_index;
	unsigned long generation;
	float *positions;
	long count, sent;
	int ended;
	/* the map's sun, from its sky as a window last saw it */
	short sky_index;
	int sun_found;
	real_vector3d sun;
	real_rgb_color sun_color;
	real sun_power;
	unsigned long frame;
	int logged;
} prism_rt = { 0, 0, 0, 0, 0, 0, 0, 0, 100, 0, -1, 0, NULL, 0, 0, 0, NONE };

/* ---------- private code */

static int word_index(char const *value, char const *const *words, int otherwise)
{
	int index;

	for (index = 0; value && words[index]; index++)
	{
		if (!strcmp(value, words[index]))
			return index;
	}
	return otherwise;
}

static void settings_read(void)
{
	static char const *const modes[] = { "off", "shadows", "gi", "pathtraced", NULL };
	static char const *const qualities[] = { "medium", "low", "high", "ultra", NULL };
	static char const *const resolutions[] = { "auto", "half", "full", NULL };
	static char const *const denoisers[] = { "spatiotemporal", "temporal", "off", NULL };
	static char const *const reflections[] = { "off", "visor", "all", NULL };
	static char const *const objects[] = { "auto", "view_lod", "low_lod", NULL };
	static char const *const rates[] = { "full", "half", NULL };
	static char const *const views[] = { "off", "visibility", "ao", "gi", "reflections", "albedo", "normals",
		"probes", "heat", NULL };
	long mood;

	if (prism_rt.settings_read == config_changes() + 1)
		return;
	prism_rt.settings_read = config_changes() + 1;
	prism_rt.mode = word_index(config_string("display.prism_rt"), modes, PRISM_RT_OFF);
	prism_rt.quality = word_index(config_string("display.prism_rt_quality"), qualities, 0);
	prism_rt.resolution = word_index(config_string("display.prism_rt_resolution"), resolutions, 0);
	prism_rt.denoiser = word_index(config_string("display.prism_rt_denoiser"), denoisers, 0);
	prism_rt.reflections = word_index(config_string("display.prism_rt_reflections"), reflections, 0);
	prism_rt.objects = word_index(config_string("display.prism_rt_objects"), objects, 0);
	prism_rt.rate = word_index(config_string("display.prism_rt_rate"), rates, 0);
	prism_rt.view = word_index(config_string("debug.prism_rt_view"), views, 0);
	mood = config_integer("display.prism_rt_mood");
	prism_rt.mood = mood < 0 ? 0 : mood > 100 ? 100 : (int)mood;
}

static void static_release(void)
{
	/* (the game's free takes no NULL) */
	if (prism_rt.positions)
		free(prism_rt.positions);
	prism_rt.positions = NULL;
	prism_rt.count = prism_rt.sent = 0;
	prism_rt.ended = 0;
	prism_rt.bsp_index = -1;
}

/* the environment shader's surfaces that are drawn opaque: not the
alpha-tested ones (their holes are not traced yet), nor the transparent,
water or sky ones */
static int material_opaque(struct structure_material const *material)
{
	struct shader const *shader;

	if (material->shader.index == NONE)
		return FALSE;
	shader = shader_definition_get(material->shader.index);
	if (!shader || shader->base.type != PRISM_RT_SHADER_ENVIRONMENT)
		return FALSE;
	return !(*(unsigned short const *)(shader + 1) & PRISM_RT_ENVIRONMENT_ALPHA_TESTED);
}

/* the BSP's opaque render triangles, copied in world space (FALSE: none) */
static int static_copy(struct structure_bsp *bsp)
{
	long lightmap_index, total = 0, written = 0, pass;

	for (pass = 0; pass < 2; pass++)
	{
		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
				struct structure_lightmap);
			long material_index;

			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
					struct structure_material);
				long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
				byte const *vertices = (byte const *)material->compressed_vertex_data.address;
				long surface_index;

				if (!vertices || vertex_size < 12 || material->vertices.count <= 0 || material->surface_count <= 0 ||
					material->first_surface_index < 0 ||
					material->first_surface_index + material->surface_count > bsp->surfaces.count ||
					(unsigned long)material->compressed_vertex_data.size <
					(unsigned long)material->vertices.count * (unsigned long)vertex_size || !material_opaque(material))
				{
					continue;
				}
				if (!pass)
				{
					total += material->surface_count;
					continue;
				}
				for (surface_index = 0; surface_index < material->surface_count; surface_index++)
				{
					struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
						material->first_surface_index + surface_index, struct structure_surface);
					float *to = prism_rt.positions + written * 9;
					int corner, usable = TRUE;

					for (corner = 0; corner < 3; corner++)
					{
						long index = (unsigned short)surface->vertex_indices[corner];

						if (index >= material->vertices.count)
						{
							usable = FALSE;
							break;
						}
						memcpy(to + corner * 3, vertices + index * vertex_size, 3 * sizeof(float));
					}
					if (usable && written < total)
						written++;
				}
			}
		}
		if (!pass)
		{
			if (!total)
				return FALSE;
			prism_rt.positions = malloc((size_t)total * 9 * sizeof(float));
			if (!prism_rt.positions)
				return FALSE;
		}
	}
	prism_rt.count = written;
	return written > 0;
}

/* the BSP drawn: its triangles copied once, then sent a slice a frame */
static void static_frame(void)
{
	short bsp_index = global_structure_bsp_index_get();
	struct structure_bsp *bsp;

	if (bsp_index != prism_rt.bsp_index)
	{
		static_release();
		prism_rt.bsp_index = bsp_index;
		prism_rt.generation++;
		bsp = bsp_index != NONE ? global_structure_bsp_get() : NULL;
		if (!bsp || !static_copy(bsp))
		{
			halo_prism_rt_static(PRISM_RT_STATIC_RELEASE, prism_rt.generation, 0, 0, 0, NULL);
			prism_rt.ended = 1;
			return;
		}
		platform_log("prism rt: bsp %d: %ld opaque triangles for the acceleration structure", (int)bsp_index,
			prism_rt.count);
		halo_prism_rt_static(PRISM_RT_STATIC_BEGIN, prism_rt.generation, 0, 0, (unsigned long)prism_rt.count, NULL);
	}
	if (prism_rt.ended)
		return;
	if (prism_rt.sent < prism_rt.count)
	{
		long count = MIN(prism_rt.count - prism_rt.sent, PRISM_RT_SLICE);

		halo_prism_rt_static(PRISM_RT_STATIC_TRIANGLES, prism_rt.generation, (unsigned long)prism_rt.sent,
			(unsigned long)count, (unsigned long)prism_rt.count, prism_rt.positions + prism_rt.sent * 9);
		prism_rt.sent += count;
		return;
	}
	halo_prism_rt_static(PRISM_RT_STATIC_END, prism_rt.generation, 0, 0, (unsigned long)prism_rt.count, NULL);
	prism_rt.ended = 1;
	/* (the copy is the backend's now) */
	if (prism_rt.positions)
		free(prism_rt.positions);
	prism_rt.positions = NULL;
}

/* the sun of the window's sky (or the one last found for this map) */
static void sun_find(void)
{
	short sky_index = render.visible_sky_model ? render.visible_sky_index : NONE;

	if (sky_index == NONE || (prism_rt.sky_index == sky_index))
		return;
	prism_rt.sky_index = sky_index;
	prism_rt.sun_found = render_sky_sun(sky_index, &prism_rt.sun, &prism_rt.sun_color, &prism_rt.sun_power);
	if (prism_rt.sun_found)
		platform_log("prism rt: sky %d's sun toward (%.3f %.3f %.3f), colour %.2f %.2f %.2f, power %.2f",
			(int)sky_index, prism_rt.sun.i, prism_rt.sun.j, prism_rt.sun.k, prism_rt.sun_color.red,
			prism_rt.sun_color.green, prism_rt.sun_color.blue, prism_rt.sun_power);
	else
		platform_log("prism rt: sky %d has no sun", (int)sky_index);
}

/* ---------- public code */

/* the mode drawn (PRISM_RT_*: off where the renderer cannot trace) */
int prism_rt_mode(void)
{
	settings_read();
	if (prism_rt.mode == PRISM_RT_OFF || !halo_prism_rt_available())
		return PRISM_RT_OFF;
	return prism_rt.mode;
}

/* the pre-pass's pass found at every pixel (world AO's scale 1) */
int prism_rt_full_resolution(void)
{
	if (prism_rt_mode() == PRISM_RT_OFF)
		return FALSE;
	/* ("pathtraced": always, its surfaces reading their own pixel's
	factor and added light: xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT) */
	if (prism_rt_mode() == PRISM_RT_PATHTRACED)
		return TRUE;
	/* (auto: full but at low quality) */
	return prism_rt.resolution == 2 || (prism_rt.resolution == 0 && prism_rt.quality != 1);
}

/* the sun's shadows traced in this frame (a mode on and a sun found): the
game's own shadow under each object is not drawn then (render.c), as
prism-world2's cascaded shadows leave it out, or the sand under a Pelican
is darkened twice, by CE's shadow and by the traced one */
int prism_rt_sun_shadows(void)
{
	return prism_rt_mode() != PRISM_RT_OFF && prism_rt.sun_found && !render_enumerating;
}

void prism_rt_map_unloaded(void)
{
	if (prism_rt.bsp_index != -1 || prism_rt.positions)
		halo_prism_rt_static(PRISM_RT_STATIC_RELEASE, ++prism_rt.generation, 0, 0, 0, NULL);
	static_release();
	prism_rt.sky_index = NONE;
	prism_rt.sun_found = FALSE;
}

/* each frame, before its windows (world_ao_frame_begin: the objects stand
where they are drawn) */
void prism_rt_frame_begin(void)
{
	struct prism_rt_frame frame;
	struct world_scene_light const *lights;
	long light_count, index;

	if (prism_rt_mode() == PRISM_RT_OFF || render_enumerating)
		return;
	prism_rt.frame++;
	static_frame();
	sun_find();
	memset(&frame, 0, sizeof(frame));
	frame.mode = (unsigned int)prism_rt.mode;
	frame.quality = (unsigned int)prism_rt.quality;
	frame.resolution = (unsigned int)prism_rt.resolution;
	frame.denoiser = (unsigned int)prism_rt.denoiser;
	frame.reflections = (unsigned int)prism_rt.reflections;
	frame.objects = (unsigned int)prism_rt.objects;
	frame.rate = (unsigned int)prism_rt.rate;
	frame.mood = (unsigned int)prism_rt.mood;
	frame.view = (unsigned int)prism_rt.view;
	frame.frame = (unsigned int)prism_rt.frame;
	frame.ticks = (unsigned int)game_time_get();
	frame.fraction = render_interpolation_fraction();
	frame.generation = (unsigned int)prism_rt.generation;
	frame.settings[0] = PRISM_RT_SUN_STRENGTH;
	frame.settings[1] = PRISM_RT_MODEL_STRENGTH;
	/* (the sky's outdoor ambient radiosity: its colour at 0x48 and power at
0x54 of the tag, before its fogs) */
	if (prism_rt.sky_index != NONE)
	{
		struct sky *sky = scenario_get_sky(prism_rt.sky_index);

		if (sky)
		{
			memcpy(frame.sky, (byte const *)sky + 0x48, 3 * sizeof(float));
			memcpy(&frame.sky[3], (byte const *)sky + 0x54, sizeof(float));
			if (!(frame.sky[3] >= 0.0f && frame.sky[3] < 1000.0f))
				memset(frame.sky, 0, sizeof(frame.sky));
		}
	}
	if (prism_rt.sun_found)
	{
		frame.sun[0] = prism_rt.sun.i;
		frame.sun[1] = prism_rt.sun.j;
		frame.sun[2] = prism_rt.sun.k;
		frame.sun[3] = 1.0f;
		frame.sun_color[0] = prism_rt.sun_color.red;
		frame.sun_color[1] = prism_rt.sun_color.green;
		frame.sun_color[2] = prism_rt.sun_color.blue;
		frame.sun_color[3] = prism_rt.sun_power;
	}
	/* (the frame's lights, ranked: world_scene's, where world AO keeps it) */
	lights = world_scene_lights();
	light_count = lights ? world_scene_light_count() : 0;
	for (index = 0; index < light_count && frame.light_count < PRISM_RT_LIGHTS; index++)
	{
		struct world_scene_light const *light = &lights[index];
		float *to = frame.lights[frame.light_count];

		if (light->type == 2)
			continue;
		to[0] = light->position[0];
		to[1] = light->position[1];
		to[2] = light->position[2];
		to[3] = light->radius;
		to[4] = light->color[0];
		to[5] = light->color[1];
		to[6] = light->color[2];
		to[7] = light->intensity;
		/* (a point light's direction and cone are not set: none) */
		to[8] = light->type == 1 ? light->direction[0] : 0.0f;
		to[9] = light->type == 1 ? light->direction[1] : 0.0f;
		to[10] = light->type == 1 ? light->direction[2] : 0.0f;
		to[11] = light->type == 1 ? light->cone_cosine : 2.0f;
		{
			int value;

			for (value = 0; value < 12; value++)
				if (!(to[value] == to[value]) || to[value] > 1.0e30f || to[value] < -1.0e30f)
					to[value] = 0.0f;
		}
		frame.light_count++;
	}
	halo_prism_rt_frame(&frame);
	if (!prism_rt.logged)
	{
		prism_rt.logged = 1;
		platform_log("prism rt: on (mode %d, quality %d, resolution %d, denoiser %d)", prism_rt.mode, prism_rt.quality,
			prism_rt.resolution, prism_rt.denoiser);
	}
}
