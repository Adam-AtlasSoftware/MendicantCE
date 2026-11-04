/*
DYNAMIC_LIGHTS.C

The enhanced dynamic lights (Video Setup > Graphics > Dynamic Lights,
display.dynamic_lights "enhanced"; "classic", the default, is the game's
own). The lights the game attaches to projectiles (plasma bolts, needles,
rockets), muzzle flashes and explosions are drawn by the renderer
(port/linux/src/d3d8_device.c, xgpu_spirv.c, xgpu_msl.c) for each pixel of
the structure and of the models, through the structure's bump maps, with a softer, brighter
falloff in linear light, and the projectiles' lights have a glow drawn about
them. This is the game's side: each frame (lights_preprocess_scene), the
lights it submits to the rasterizer, with what the renderer needs of each
that it does not have (its cone, whether it has a glow, whether the game's
own pass lights the structure with it), go to the renderer.

Where a light is drawn: the game places a light on an object's marker as
the object moves, at each tick (30 a second), and draws the objects between
ticks (port/linux/game/render_interpolation.c), so a bolt's light would step
behind its bolt; object_lights.c places it on the marker as drawn instead,
each frame. A weapon's own lights (its muzzle flash) the game moves to the
first-person weapon for its holder; an effect's light on the weapon (a
firing effect's) stays on the weapon the body holds, so it is moved to the
same marker of the first-person weapon (dynamic_lights_first_person), where
the effect is seen, and from where the first-person weapon (in the VR mode,
the weapon in the hand) is lit.

debug.dynamic_lights_log logs, each frame, where the first projectile's
light is drawn (the smoothness test: a step for each frame, not for each
tick). Only the drawing changes: nothing the game simulates, nothing sent
between machines.

Which lights are drawn (dynamic_lights_rank): a view finds up to
MAXIMUM_RENDERED_LIGHTS lights in the clusters it sees (halo_port_capacity.h:
512 on the desktop builds), but the rasterizer has room for
MAXIMUM_LIGHTS_PER_WINDOW (128) of them, the enhanced dynamic lights' grid
too (XGPU_DYNAMIC_LIGHTS_MAXIMUM), and the game gave them to the first it
found, in the clusters' order. Past 128, the lights are put in the order of
what they show first, so that the 128 drawn are those that matter most to
the view, classic or enhanced.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "interface/first_person_weapons.h"
#include "game/game.h"

#include <stdlib.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
/* port/linux/src (halo.log) */
void platform_log(const char *format, ...);

static struct
{
	/* for each rasterizer light this frame (rasterizer_lights' index) */
	long flags[MAXIMUM_LIGHTS_PER_WINDOW];
	real cone_scale[MAXIMUM_LIGHTS_PER_WINDOW];
	real cone_offset[MAXIMUM_LIGHTS_PER_WINDOW];
	/* (debug.dynamic_lights_log: where the game placed it, at the tick) */
	real_point3d tick_position[MAXIMUM_LIGHTS_PER_WINDOW];
	struct halo_dynamic_light lights[MAXIMUM_LIGHTS_PER_WINDOW];
	unsigned long frame;
} dynamic_lights;

/* a rasterizer light just submitted: its flags (HALO_DYNAMIC_LIGHT_*), its
cone, as rasterizer_set_model_lighting_point_light makes it, and where the
game placed it at the tick */
void dynamic_lights_note(
	long rasterizer_light_index,
	long flags,
	real cone_scale,
	real cone_offset,
	real_point3d const *tick_position)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW)
		return;
	dynamic_lights.flags[rasterizer_light_index] = flags;
	dynamic_lights.cone_scale[rasterizer_light_index] = cone_scale;
	dynamic_lights.cone_offset[rasterizer_light_index] = cone_offset;
	dynamic_lights.tick_position[rasterizer_light_index] = *tick_position;
}

/* an effect's light on a node of a weapon held by a unit: where it is seen, on the first-person weapon's marker of the same
name, as far from it as from the held weapon's, when the weapon is a local
player's drawn in first person; FALSE if it is not */
boolean dynamic_lights_first_person(
	long weapon_index,
	real_point3d *position)
{
	static char const *const marker_names[] = { "primary trigger", "secondary trigger" };
	real nearest = 0.5f * 0.5f;
	real_point3d moved;
	boolean found = FALSE;
	short name_index;

	for (name_index = 0; name_index < NUMBEROF(marker_names); name_index++)
	{
		struct object_marker held, first_person;
		real distance;

		if (object_get_marker_by_name(weapon_index, marker_names[name_index], &held, 1) < 1)
			continue;
		distance = distance_squared3d(&held.matrix.position, position);
		if (distance >= nearest)
			continue;
		if (first_person_weapon_get_marker_by_name_render(weapon_index, marker_names[name_index], &first_person, 1) < 1)
			continue;
		nearest = distance;
		moved.x = first_person.matrix.position.x + (position->x - held.matrix.position.x);
		moved.y = first_person.matrix.position.y + (position->y - held.matrix.position.y);
		moved.z = first_person.matrix.position.z + (position->z - held.matrix.position.z);
		found = TRUE;
	}
	if (found)
		*position = moved;
	return found;
}

/* the frame's lights (rasterizer_lights, as lights_preprocess_scene
submitted them) to the renderer */
void dynamic_lights_submit(
	void)
{
	long light_index;
	long count = MIN(rasterizer_lights.light_count, MAXIMUM_LIGHTS_PER_WINDOW);
	real_vector3d right;
	float camera[3], right_axis[3], up_axis[3];
	long logged = 0;

	dynamic_lights.frame++;
	for (light_index = 0; light_index < count; light_index++)
	{
		struct rasterizer_light_submit_parameters const *light = &rasterizer_lights.lights[light_index];
		struct halo_dynamic_light *out = &dynamic_lights.lights[light_index];

		out->position[0] = light->position.x;
		out->position[1] = light->position.y;
		out->position[2] = light->position.z;
		out->radius = light->radius;
		out->color[0] = light->color.red;
		out->color[1] = light->color.green;
		out->color[2] = light->color.blue;
		out->axis[0] = light->forward.i;
		out->axis[1] = light->forward.j;
		out->axis[2] = light->forward.k;
		out->cone_scale = dynamic_lights.cone_scale[light_index];
		out->cone_offset = dynamic_lights.cone_offset[light_index];
		out->flags = dynamic_lights.flags[light_index];
		if (logged < 4 && (out->flags & HALO_DYNAMIC_LIGHT_PROJECTILE) && config_boolean("debug.dynamic_lights_log"))
		{
			logged++;
			platform_log("dynamic lights: frame %lu tick %ld + %.3f: projectile light %ld drawn at %.4f %.4f %.4f "
				"(the tick's %.4f %.4f %.4f) radius %.3f color %.2f %.2f %.2f", dynamic_lights.frame, game_time_get(),
				(double)game_time_get_tick_fraction(), light_index, (double)out->position[0], (double)out->position[1],
				(double)out->position[2], (double)dynamic_lights.tick_position[light_index].x,
				(double)dynamic_lights.tick_position[light_index].y, (double)dynamic_lights.tick_position[light_index].z,
				(double)out->radius, (double)out->color[0], (double)out->color[1], (double)out->color[2]);
		}
	}
	if (config_boolean("debug.dynamic_lights_log") && !(dynamic_lights.frame % 72))
	{
		long projectiles = 0, models_only = 0;

		for (light_index = 0; light_index < count; light_index++)
		{
			projectiles += (dynamic_lights.lights[light_index].flags & HALO_DYNAMIC_LIGHT_PROJECTILE) != 0;
			models_only += (dynamic_lights.lights[light_index].flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) != 0;
		}
		platform_log("dynamic lights: frame %lu: %ld lights, %ld projectiles', %ld models' only; camera %.2f %.2f %.2f "
			"forward %.2f %.2f %.2f", dynamic_lights.frame, count, projectiles, models_only, render.camera.position.x,
			render.camera.position.y, render.camera.position.z, render.camera.forward.i, render.camera.forward.j,
			render.camera.forward.k);
	}
	cross_product3d(&render.camera.forward, &render.camera.up, &right);
	camera[0] = render.camera.position.x;
	camera[1] = render.camera.position.y;
	camera[2] = render.camera.position.z;
	right_axis[0] = right.i;
	right_axis[1] = right.j;
	right_axis[2] = right.k;
	up_axis[0] = render.camera.up.i;
	up_axis[1] = render.camera.up.j;
	up_axis[2] = render.camera.up.k;
	halo_dynamic_lights_set(dynamic_lights.lights, (int)count, camera, right_axis, up_axis);
	/* (each submit notes its own again; one not noted is omni) */
	for (light_index = 0; light_index < count; light_index++)
	{
		dynamic_lights.flags[light_index] = 0;
		dynamic_lights.cone_scale[light_index] = 0.0f;
		dynamic_lights.cone_offset[light_index] = 1.0f;
		dynamic_lights.tick_position[light_index] = rasterizer_lights.lights[light_index].position;
	}
}

/* ---------- which lights are drawn */

/* a light wholly behind the camera's plane by more than this (world units:
the VR eyes beside the head's centre and the late latch's turn, with room)
lights nothing in view, and goes after every light that may */
#define RANK_BEHIND_MARGIN 0.1f
#define RANK_BEHIND_SCALE (1.0f / 64.0f)

struct rank_entry
{
	real score;
	short index;
};

/* each light as it was last drawn (dynamic_lights_drawn: its handle,
where, how far it reached and its brightest channel), by its index */
static struct
{
	long light;
	real_point3d position;
	real radius;
	real intensity;
} lights_drawn[MAXIMUM_LIGHTS_PER_MAP];

static int rank_compare(void const *a, void const *b)
{
	struct rank_entry const *left = a;
	struct rank_entry const *right = b;

	if (left->score != right->score)
		return left->score > right->score ? -1 : 1;
	/* (the game's order among equals: the same lights each frame) */
	return left->index - right->index;
}

/* object_lights.c lights_preprocess_scene: a light submitted to the
rasterizer, as it is drawn (between the ticks: its blended color and
radius, on its marker as drawn), which ranks it next frame */
void dynamic_lights_drawn(long light, real_point3d const *position, real radius, real_rgb_color const *color)
{
	long index = DATUM_INDEX_TO_ABSOLUTE_INDEX(light);

	if (index >= MAXIMUM_LIGHTS_PER_MAP)
		return;
	lights_drawn[index].light = light;
	lights_drawn[index].position = *position;
	lights_drawn[index].radius = radius;
	lights_drawn[index].intensity = MAX(color->red, MAX(color->green, color->blue));
}

/* port/linux/game/world_scene.c (display.ambient_occlusion "world"'s light
list): the light last drawn at an absolute index (dynamic_lights_drawn),
its handle, where and how far it reached; FALSE if none has been. It only
reads */
boolean dynamic_lights_drawn_get(long index, long *light, real_point3d *position, real *radius)
{
	if (index < 0 || index >= MAXIMUM_LIGHTS_PER_MAP || !lights_drawn[index].light)
		return FALSE;
	*light = lights_drawn[index].light;
	*position = lights_drawn[index].position;
	*radius = lights_drawn[index].radius;
	return TRUE;
}

/* object_lights.c lights_preprocess_scene: the scene's lights (light
handles, as structure_visibility_find_objects found them), most visible
first when there are more than the rasterizer has room for (none moved
otherwise: the order is the game's). A light's part of the view is its
intensity (its colour's brightest channel) times how wide its reach looks
from the camera: its radius over its distance, at most 1 (the camera in
it), so a near light and a far wide one both rank high, a small far one
last; and far lower wholly behind the camera's plane. Each as it was drawn
last frame (dynamic_lights_drawn: as the frame blends it between ticks, a
frame old), else as the game has it (describe: the tick's), and one of no
known brightness (new this frame) as bright. */
void dynamic_lights_rank(long *lights, short count,
	void (*describe)(long light, real_point3d *position, real *radius, real *intensity))
{
	static struct rank_entry entries[MAXIMUM_RENDERED_LIGHTS];
	static long ranked[MAXIMUM_RENDERED_LIGHTS];
	real_vector3d forward = render.camera.forward;
	short index;

	if (count <= MAXIMUM_LIGHTS_PER_WINDOW || count > MAXIMUM_RENDERED_LIGHTS)
		return;
	if (normalize3d(&forward) <= 0.0f)
		forward.i = forward.j = forward.k = 0.0f;
	for (index = 0; index < count; index++)
	{
		real_point3d position;
		real_vector3d offset;
		real radius = 0.0f, intensity = 0.0f, distance, score;
		long drawn = DATUM_INDEX_TO_ABSOLUTE_INDEX(lights[index]);

		if (drawn < MAXIMUM_LIGHTS_PER_MAP && lights_drawn[drawn].light == lights[index])
		{
			position = lights_drawn[drawn].position;
			radius = lights_drawn[drawn].radius;
			intensity = lights_drawn[drawn].intensity;
		}
		else
		{
			describe(lights[index], &position, &radius, &intensity);
			if (!(intensity > 0.0f))
				intensity = 1.0f;
		}
		radius = radius > 1.0e-3f ? radius : 1.0e-3f;
		vector_from_points3d(&render.camera.position, &position, &offset);
		distance = magnitude3d(&offset);
		score = intensity * radius / (distance > radius ? distance : radius);
		if (dot_product3d(&offset, &forward) < -(radius + RANK_BEHIND_MARGIN))
			score *= RANK_BEHIND_SCALE;
		entries[index].score = score;
		entries[index].index = index;
	}
	qsort(entries, count, sizeof(*entries), rank_compare);
	for (index = 0; index < count; index++)
		ranked[index] = lights[entries[index].index];
	memcpy(lights, ranked, count * sizeof(*lights));
}
