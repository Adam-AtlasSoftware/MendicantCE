/*
PRISM_SCENE_LIGHTS.C

PRISM v4's SCENE_LIGHTS (overhaul_research/prism_v4_renderer/design.md,
section 1.6; xgpu_stream.h; docs/prism_scene_records.md): the frame's light
table, one for the baker and for Raster, sent each frame where SCENE_STATIC
is (display.lighting "ce_deferred", or display.prism_rt on; never with "ce"
and ray tracing off).

- The sun: the sky's narrowest light (render_sky_sun), its direction toward
  it, colour and power, its angular diameter. Static.
- The domes: the sky's other lights, each a cone toward it (its half
  angle's cosine). Static.
- The sky: its outdoor ambient radiosity (colour and power, the sky tag's).
  Static.
- The emitters: each material whose radiosity lit the level (light cards,
  lamp surfaces: SCENE_STATIC's triangles of it), its middle and reach, its
  colour by power and its area. Static.
- The fixtures: the light fixtures placed in the level, live (their device
  objects), each a point or spot light of its colour and intensity, on
  where its device has power. Switchable (a script turns them on and off).
- The dynamic lights: the lights the rasterizer drew the frame before
  (world_scene's: flashlight, plasma, muzzle flashes, projectiles, vehicle
  lights), each under its datum handle. Dynamic, never baked.

Ids are stable across frames (xgpu_stream.h's XGPU_SCENE_LIGHT_*_ID, a
datum handle for the live ones). Nothing here changes what the game
simulates: it reads the sky's tag, the structure's materials and the live
objects' places.
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
#include "objects/objects.h"
#include "devices/devices.h"
#include "devices/device_light_fixtures.h"
#include "tag_files/tag_files.h"
#include "world_scene.h"
#include "prism_deferred.h"
#include "prism_scene_lights.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src: the stream's (xgpu_stream_encode.c) */
void xgpu_backend_scene_lights(unsigned long first, unsigned long total, const void *lights, unsigned long count);
void platform_log(const char *format, ...);
/* render_enumerate.c's */
extern unsigned char render_enumerating;
/* render_sky.c's */
boolean render_sky_sun(short sky_index, real_vector3d *direction, real_rgb_color *color, real *power);
boolean render_sky_light(short sky_index, short light_index, real_vector3d *direction, real_rgb_color *color,
	real *power, real *diameter);
short render_sky_light_count(short sky_index);
/* prism_rt.c's, world_ao.c's, world_scene.c's */
int prism_rt_mode(void);
int world_ao_mode(void);
/* port/linux/src/port_config.c */
int config_boolean(const char *name);


/* ---------- constants */

/* (xgpu_stream.h's: the game's sources do not see the renderer's headers) */
enum
{
	LIGHT_SUN,
	LIGHT_DOME,
	LIGHT_SKY,
	LIGHT_POINT,
	LIGHT_SPOT,
	LIGHT_EMITTER,
	CLASS_STATIC = 0,
	CLASS_ANIMATED,
	CLASS_SWITCHABLE,
	CLASS_DYNAMIC,
	FLAG_ON = 1,
	FLAG_CASTS_SHADOWS = 2,
	RECORD_LIGHTS = 256,
	SUN_ID = 1,
	SKY_ID = 2,
	DOME_ID = 16,
	EMITTER_ID = 0x10000,
	/* the table at most (fixtures, emitters and dynamic lights together) */
	LIGHTS_MOST = 2048,
	EMITTERS_MOST = 1024
};

/* a light as struct xgpu_stream_scene_light lays it out (20 words) */
struct scene_light
{
	unsigned long id, type, light_class, flags;
	float position[4];
	float direction[4];
	float radiance[4];
	float shape[4];
};

/* an emitter: a material's emitting triangles together */
struct emitter
{
	unsigned long material;
	float sum[3];
	float least[3], most[3];
	float area;
	float radiance[3];
};

/* ---------- globals */

static struct
{
	struct scene_light lights[LIGHTS_MOST];
	/* the emitters of the BSP (found once a BSP) */
	struct structure_bsp *emitters_bsp;
	short emitters_bsp_index;
	long emitter_count;
	struct emitter emitters[EMITTERS_MOST];
	long logged_count;
	/* (debug.dynamic_lights_log: the counts last logged) */
	long logged_drawn, logged_listed, logged_sent;
} scene_lights = { { { 0 } }, NULL, NONE, 0 };

/* ---------- private code */

static struct scene_light *light_add(
	long *count,
	unsigned long id,
	unsigned long type,
	unsigned long light_class,
	unsigned long flags)
{
	struct scene_light *light;

	if (*count >= LIGHTS_MOST)
	{
		return NULL;
	}
	light = &scene_lights.lights[(*count)++];
	memset(light, 0, sizeof(*light));
	light->id = id;
	light->type = type;
	light->light_class = light_class;
	light->flags = flags;
	light->direction[3] = 2.0f;
	return light;
}

/* the BSP's emitters: each material of radiosity power, its triangles'
middle, bounds and area (the map's triangles: the remodel keeps them where
it remodels them) */
static void emitters_find(
	struct structure_bsp *bsp,
	short bsp_index)
{
	long lightmap_index;

	if (scene_lights.emitters_bsp == bsp && scene_lights.emitters_bsp_index == bsp_index)
	{
		return;
	}
	scene_lights.emitters_bsp = bsp;
	scene_lights.emitters_bsp_index = bsp_index;
	scene_lights.emitter_count = 0;
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		long material_index;

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
				struct structure_material);
			struct shader const *shader = material->shader.index != NONE ?
				shader_definition_get(material->shader.index) : NULL;
			long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
			byte const *vertices = (byte const *)material->compressed_vertex_data.address;
			unsigned long id;
			struct emitter *emitter;
			long surface, index;
			real power;

			if (!shader || !(shader->base.radiosity.power > 0.0f) || !vertices || vertex_size < 12 ||
				material->vertices.count <= 0 || material->first_surface_index < 0 ||
				material->first_surface_index + material->surface_count > bsp->surfaces.count ||
				(unsigned long)material->compressed_vertex_data.size <
				(unsigned long)material->vertices.count * (unsigned long)vertex_size)
			{
				continue;
			}
			id = prism_deferred_material_id(shader);
			if (!id)
			{
				continue;
			}
			/* (a material's lightmaps' parts gathered into one emitter) */
			for (index = 0; index < scene_lights.emitter_count && scene_lights.emitters[index].material != id; index++);
			if (index == scene_lights.emitter_count)
			{
				if (scene_lights.emitter_count == EMITTERS_MOST)
				{
					continue;
				}
				emitter = &scene_lights.emitters[scene_lights.emitter_count++];
				memset(emitter, 0, sizeof(*emitter));
				emitter->material = id;
				emitter->least[0] = emitter->least[1] = emitter->least[2] = 1.0e30f;
				emitter->most[0] = emitter->most[1] = emitter->most[2] = -1.0e30f;
				power = shader->base.radiosity.power;
				emitter->radiance[0] = PIN(shader->base.radiosity.color_of_emitted_light.red, 0.0f, 1.0f) * power;
				emitter->radiance[1] = PIN(shader->base.radiosity.color_of_emitted_light.green, 0.0f, 1.0f) * power;
				emitter->radiance[2] = PIN(shader->base.radiosity.color_of_emitted_light.blue, 0.0f, 1.0f) * power;
			}
			else
			{
				emitter = &scene_lights.emitters[index];
			}
			for (surface = 0; surface < material->surface_count; surface++)
			{
				struct structure_surface const *at = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
					material->first_surface_index + surface, struct structure_surface);
				float corners[3][3], e1[3], e2[3], n[3], area;
				int corner, axis;

				for (corner = 0; corner < 3; corner++)
				{
					long vertex = (unsigned short)at->vertex_indices[corner];

					if (vertex >= material->vertices.count)
					{
						break;
					}
					memcpy(corners[corner], vertices + vertex * vertex_size, 3 * sizeof(float));
				}
				if (corner < 3)
				{
					continue;
				}
				for (axis = 0; axis < 3; axis++)
				{
					e1[axis] = corners[1][axis] - corners[0][axis];
					e2[axis] = corners[2][axis] - corners[0][axis];
				}
				n[0] = e1[1] * e2[2] - e1[2] * e2[1];
				n[1] = e1[2] * e2[0] - e1[0] * e2[2];
				n[2] = e1[0] * e2[1] - e1[1] * e2[0];
				area = 0.5f * (float)sqrt((double)(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
				if (!(area > 0.0f) || area > 1.0e8f)
				{
					continue;
				}
				emitter->area += area;
				for (axis = 0; axis < 3; axis++)
				{
					emitter->sum[axis] += area * (corners[0][axis] + corners[1][axis] + corners[2][axis]) / 3.0f;
					for (corner = 0; corner < 3; corner++)
					{
						emitter->least[axis] = MIN(emitter->least[axis], corners[corner][axis]);
						emitter->most[axis] = MAX(emitter->most[axis], corners[corner][axis]);
					}
				}
			}
		}
	}
}

/* every finite, else 0 (a light's floats) */
static void light_finite(
	struct scene_light *light)
{
	float *values = light->position;
	int index;

	for (index = 0; index < 16; index++)
	{
		if (!(values[index] == values[index]) || values[index] > 1.0e30f || values[index] < -1.0e30f)
		{
			values[index] = 0.0f;
		}
	}
}

/* ---------- public code */

void prism_scene_lights_frame(
	void)
{
	short bsp_index, sky_index;
	struct structure_bsp *bsp;
	long count = 0, index, light_count, first;
	struct scene_light *light;
	struct world_scene_light const *lights;

	if (render_enumerating || (!prism_deferred_wanted() && !prism_rt_mode()))
	{
		return;
	}
	bsp_index = global_structure_bsp_index_get();
	bsp = bsp_index != NONE ? global_structure_bsp_get() : NULL;
	if (!bsp)
	{
		return;
	}
	/* the sky's: its sun, its other lights, its ambient light */
	sky_index = render.visible_sky_model ? render.visible_sky_index : NONE;
	if (sky_index != NONE)
	{
		real_vector3d sun_direction, direction;
		real_rgb_color sun_color, color;
		real sun_power = 0.0f, power, diameter;
		boolean sun = render_sky_sun(sky_index, &sun_direction, &sun_color, &sun_power);
		short sky_lights = render_sky_light_count(sky_index), sky_light;
		struct sky *sky = scenario_get_sky(sky_index);

		for (sky_light = 0; sky_light < sky_lights; sky_light++)
		{
			boolean is_sun;

			if (!render_sky_light(sky_index, sky_light, &direction, &color, &power, &diameter) || !(power > 0.0f))
			{
				continue;
			}
			is_sun = sun && direction.i == sun_direction.i && direction.j == sun_direction.j &&
				direction.k == sun_direction.k && power == sun_power;
			light = light_add(&count, is_sun ? SUN_ID : DOME_ID + (unsigned long)sky_light, is_sun ? LIGHT_SUN : LIGHT_DOME,
				CLASS_STATIC, FLAG_ON | (is_sun ? FLAG_CASTS_SHADOWS : 0));
			if (!light)
			{
				break;
			}
			light->position[0] = direction.i;
			light->position[1] = direction.j;
			light->position[2] = direction.k;
			light->radiance[0] = color.red;
			light->radiance[1] = color.green;
			light->radiance[2] = color.blue;
			light->radiance[3] = power;
			/* (the sun's angular diameter; a dome's half angle's cosine) */
			light->shape[0] = is_sun ? diameter : (float)cos(PIN(diameter, 0.0005f, 3.0f) * 0.5f);
			light_finite(light);
		}
		/* (the sky's outdoor ambient radiosity: its colour at 0x48 and power at
		0x54 of the tag, before its fogs) */
		if (sky && (light = light_add(&count, SKY_ID, LIGHT_SKY, CLASS_STATIC, FLAG_ON)) != NULL)
		{
			memcpy(light->radiance, (byte const *)sky + 0x48, 3 * sizeof(float));
			memcpy(&light->radiance[3], (byte const *)sky + 0x54, sizeof(float));
			light_finite(light);
		}
	}
	/* the emitters */
	emitters_find(bsp, bsp_index);
	for (index = 0; index < scene_lights.emitter_count; index++)
	{
		struct emitter const *emitter = &scene_lights.emitters[index];
		int axis;
		float reach = 0.0f;

		if (!(emitter->area > 0.0f) || !(light = light_add(&count, EMITTER_ID + emitter->material, LIGHT_EMITTER,
			CLASS_STATIC, FLAG_ON)))
		{
			continue;
		}
		for (axis = 0; axis < 3; axis++)
		{
			float middle = emitter->sum[axis] / emitter->area;

			light->position[axis] = middle;
			reach += MAX(middle - emitter->least[axis], emitter->most[axis] - middle) *
				MAX(middle - emitter->least[axis], emitter->most[axis] - middle);
			light->radiance[axis] = emitter->radiance[axis];
		}
		light->position[3] = (float)sqrt((double)reach);
		light->radiance[3] = 1.0f;
		light->shape[0] = emitter->area;
		light_finite(light);
	}
	/* the fixtures: the light fixtures live in the level, on where their
	devices have power */
	{
		struct object_iterator iterator;
		struct light_fixture_datum *fixture;

		object_iterator_new(&iterator, _object_mask_light_fixture, 0);
		while ((fixture = (struct light_fixture_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real cutoff = fixture->light_fixture.cutoff_angle;
			boolean spot = cutoff > 0.0f && cutoff < 3.14159265f;
			real device_power = device_get_power(iterator.index);

			light = light_add(&count, (unsigned long)iterator.index, spot ? LIGHT_SPOT : LIGHT_POINT, CLASS_SWITCHABLE,
				device_power > 0.0f ? FLAG_ON : 0);
			if (!light)
			{
				break;
			}
			light->position[0] = fixture->object.position.x;
			light->position[1] = fixture->object.position.y;
			light->position[2] = fixture->object.position.z;
			if (spot)
			{
				real falloff = fixture->light_fixture.falloff_angle;

				light->direction[0] = fixture->object.forward.i;
				light->direction[1] = fixture->object.forward.j;
				light->direction[2] = fixture->object.forward.k;
				light->direction[3] = (float)cos((double)cutoff);
				light->shape[0] = falloff > 0.0f && falloff < cutoff ? (float)cos((double)falloff) : light->direction[3];
			}
			light->radiance[0] = fixture->light_fixture.color.red;
			light->radiance[1] = fixture->light_fixture.color.green;
			light->radiance[2] = fixture->light_fixture.color.blue;
			/* (its intensity whatever its device's power: on or off is its flag;
			the bake takes every fixture, as CE's radiosity did) */
			light->radiance[3] = fixture->light_fixture.intensity;
			light_finite(light);
		}
	}
	/* the dynamic lights the rasterizer drew (world_scene's: listed already
	where world AO is on, else listed here) */
	if (!world_ao_mode())
	{
		world_scene_lights_refresh();
	}
	lights = world_scene_lights();
	light_count = lights ? world_scene_light_count() : 0;
	for (index = 0; index < light_count; index++)
	{
		struct world_scene_light const *drawn = &lights[index];

		if (!(drawn->flags & WORLD_SCENE_LIGHT_DYNAMIC) || drawn->type == 2)
		{
			continue;
		}
		/* (on where it gives light: the rasterizer takes a light drawn
		reaching nowhere as a black one) */
		light = light_add(&count, (unsigned long)drawn->id, drawn->type == 1 ? LIGHT_SPOT : LIGHT_POINT, CLASS_DYNAMIC,
			(drawn->color[0] > 0.0f || drawn->color[1] > 0.0f || drawn->color[2] > 0.0f ? FLAG_ON : 0) |
			FLAG_CASTS_SHADOWS);
		if (!light)
		{
			break;
		}
		memcpy(light->position, drawn->position, 3 * sizeof(float));
		light->position[3] = drawn->radius;
		if (drawn->type == 1)
		{
			memcpy(light->direction, drawn->direction, 3 * sizeof(float));
			light->direction[3] = drawn->cone_cosine;
			light->shape[0] = drawn->cone_inner_cosine > drawn->cone_cosine ? drawn->cone_inner_cosine :
				drawn->cone_cosine;
		}
		memcpy(light->radiance, drawn->color, 3 * sizeof(float));
		light->radiance[3] = drawn->intensity * drawn->gel;
		light_finite(light);
	}
	/* (debug.dynamic_lights_log: the dynamic lights at each step, where
	they changed: the rasterizer's, world_scene's list, those sent) */
	if (config_boolean("debug.dynamic_lights_log"))
	{
		long sent = 0, listed = 0;

		for (index = 0; index < count; index++)
			sent += scene_lights.lights[index].light_class == CLASS_DYNAMIC;
		for (index = 0; index < light_count; index++)
			listed += (lights[index].flags & WORLD_SCENE_LIGHT_DYNAMIC) != 0;
		if (rasterizer_lights.light_count != scene_lights.logged_drawn || listed != scene_lights.logged_listed ||
			sent != scene_lights.logged_sent)
		{
			scene_lights.logged_drawn = rasterizer_lights.light_count;
			scene_lights.logged_listed = listed;
			scene_lights.logged_sent = sent;
			platform_log("prism scene: dynamic lights: the rasterizer's %ld, listed %ld (of %ld), sent %ld",
				rasterizer_lights.light_count, listed, light_count, sent);
		}
	}
	for (first = 0; first < count; first += RECORD_LIGHTS)
	{
		xgpu_backend_scene_lights((unsigned long)first, (unsigned long)count, scene_lights.lights + first,
			(unsigned long)MIN(count - first, RECORD_LIGHTS));
	}
	if (count != scene_lights.logged_count)
	{
		scene_lights.logged_count = count;
		platform_log("prism scene: %ld lights (%ld emitters)", count, scene_lights.emitter_count);
	}
}

void prism_scene_lights_map_unloaded(
	void)
{
	scene_lights.emitters_bsp = NULL;
	scene_lights.emitters_bsp_index = NONE;
	scene_lights.emitter_count = 0;
	scene_lights.logged_count = 0;
}
