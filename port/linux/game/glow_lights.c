/*
GLOW_LIGHTS.C

PRISM Path Traced's light from what glows (docs/prism_pathtraced.md, "What
glows"): beside CE's dynamic lights (world_scene's list, SCENE_LIGHTS'
dynamic class), every glowing thing in view gives the path tracer a light:

- CE's lights that light nothing in CE (a light with no dynamic flag: a
  plasma bolt's, a Banshee's or Ghost's bolt's, a plasma weapon's or the
  needler's glow, a vehicle's lamps, drawn as their lens flare alone) are
  lit in Path Traced (and Raster's "materials" composition) as the enhanced
  dynamic lights light a projectile's (object_lights.c: glow_lights_all), at
  their tag's colour and reach by display.prism_glow_lights, a weapon's at
  GLOW_WEAPON_SHARE; the shields' glow (shield_glow.c) is on;
- a projectile's light is followed as it flies and flashes where it struck
  once it is gone (glow_strike_*: CE's plasma and needle strikes are
  particles alone);
- a projectile in flight or a grenade on the ground with a self-illuminated
  shader (shader_model's) and no light of its own gives a light at its
  bounding sphere's middle, its colour the shader's self-illumination as CE
  draws it (by its object function), its reach by its size
  (glow_lights_frame). Not the bodies, vehicles and weapons: their
  self-illumination is masked to small parts by maps the game does not read
  (an Elite's and the Chief's armour have white colours).

debug.glow_inventory (HALO_GLOW_INVENTORY=1) logs, once per map, every
light, object and effect tag of the map with what it has that glows (its
lights, self-illuminated shaders, particles and contrails), and in play,
each light first seen with its owner and whether it was drawn
(work/openh1_eval/v4_glow_runs/glow_inventory.md).

Only the drawing changes: nothing the game simulates, nothing sent between
machines.
*/

#include "cseries.h"
#include "real_math.h"
#include "cache/cache_files.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "effects/effect_definitions.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "render/render.h"
#include "game/game.h"
#include "glow_lights.h"
#include "prism_deferred.h"

#include <math.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
double config_real(const char *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);

/* ---------- constants */

enum
{
	_shader_type_model = 4,
	SEEN_MOST = 512,
	/* the self-illuminated objects lit at most, the nearest */
	GLOW_OBJECTS_MOST = 24,
	/* projectiles' lights followed (their strikes' flashes) */
	STRIKES_MOST = 64,
	/* a strike's flash's length (ticks of the game's clock: 0.2 s) */
	STRIKE_TICKS = 6,
	/* object definitions remembered (their glow found once) */
	DEFINITIONS_MOST = 1024,
	/* a light's flags (tag_schema_render.c's light_definition) */
	_light_definition_dynamic_bit = 0,
};

/* objects this far from the camera and nearer give their glow's light */
#define GLOW_RANGE 40.0f
/* a strike's flash, of its projectile's light: brighter, further, and
drawn back from the surface it struck along its path (world units) */
#define STRIKE_BRIGHTER 1.5f
#define STRIKE_REACH 1.5f
#define STRIKE_BACK 0.15f
/* a weapon's lens flare's light's share (glow_lights_share) */
#define GLOW_WEAPON_SHARE 0.25f

/* ---------- structures */

/* tag_schema_render.c's shader_model_definition, its self-illumination */
struct glow_shader_model
{
	struct shader shader;
	word flags;
	short type;
	byte reserved2C[0xC];
	real translucency;
	byte reserved3C[0x10];
	short change_color_source;
	byte reserved4E[0x1E];
	word self_illumination_flags;
	short pad6E;
	short self_illumination_color_source;
	short self_illumination_animation_function;
	real self_illumination_animation_period;
	real_rgb_color self_illumination_color_lower_bound;
	real_rgb_color self_illumination_color_upper_bound;
	byte reserved90[0xC];
	real map_u_scale;
	real map_v_scale;
	struct tag_reference base_map;
	byte reservedB4[0x8];
	struct tag_reference multipurpose_map;
};

typedef char verify_glow_shader_model_self_illumination[
	offsetof(struct glow_shader_model, self_illumination_animation_function) == 0x72 ? 1 : -1];
typedef char verify_glow_shader_model_multipurpose[
	offsetof(struct glow_shader_model, multipurpose_map) == 0xBC ? 1 : -1];

/* models.c's model_shader_reference */
struct model_shader_reference_glow
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* object_lights.c's light definition, its start */
struct glow_light_definition
{
	long flags;
	real radius;
	real radius_modifier_lower_bound;
	real radius_modifier_upper_bound;
	real falloff_angle;
	real cutoff_angle;
	real lens_flare_radius;
	real runtime_cosine_falloff_angle;
	real runtime_cosine_cutoff_angle;
	real specular_radius_multiplier;
	real runtime_sine_cutoff_angle;
	byte reserved2C[0x8];
	unsigned long color_interpolation_flags;
	real_argb_color color_lower_bound;
	real_argb_color color_upper_bound;
};

/* an object definition's glow, found once: its self-illuminated model
shader's colours and the object function they follow; whether it carries
a light of its own */
struct glow_definition
{
	long definition_index;
	boolean glows;
	boolean has_light;
	short color_source;
	real_rgb_color lower, upper;
	short animation_function;
	real animation_period;
};

/* a projectile's light followed */
struct glow_strike
{
	long object_index;
	real_point3d position, last;
	real radius;
	real_rgb_color color;
	long seen, struck; /* game ticks (struck NONE: still flying) */
};

/* ---------- globals */

static struct
{
	/* debug.glow_inventory: the map's tags logged; the lights seen in play */
	boolean dumped;
	long seen_count;
	long seen[SEEN_MOST][2];
	/* the definitions' glows */
	long definition_count;
	struct glow_definition definitions[DEFINITIONS_MOST];
	/* the projectiles' lights followed */
	long strike_count;
	struct glow_strike strikes[STRIKES_MOST];
	/* the frame's glowing objects */
	long glow_count;
	struct glow_lights_light glows[GLOW_OBJECTS_MOST];
	boolean read;
	unsigned long changes;
	boolean inventory;
	real scale;
} glow_lights;

/* ---------- private code */

static char const *glow_tag_name(long tag_index)
{
	char const *name = tag_index != NONE ? tag_get_name(tag_index) : NULL;

	return name ? name : "(none)";
}

static void glow_group_text(unsigned long group, char text[5])
{
	text[0] = (char)(group >> 24);
	text[1] = (char)(group >> 16);
	text[2] = (char)(group >> 8);
	text[3] = (char)group;
	text[4] = 0;
}

static void glow_inventory_light(char const *owner, long light_index)
{
	struct glow_light_definition const *light = (struct glow_light_definition const *)tag_get('ligh', light_index);

	platform_log("glow inventory: %s: light %s: %s, radius %.2f (x %.2f..%.2f), colour %.2f %.2f %.2f..%.2f %.2f %.2f%s",
		owner, glow_tag_name(light_index), TEST_FLAG(light->flags, _light_definition_dynamic_bit) ? "dynamic" :
		"lens flare only", (double)light->radius, (double)light->radius_modifier_lower_bound,
		(double)light->radius_modifier_upper_bound, (double)light->color_lower_bound.rgb.red,
		(double)light->color_lower_bound.rgb.green, (double)light->color_lower_bound.rgb.blue,
		(double)light->color_upper_bound.rgb.red, (double)light->color_upper_bound.rgb.green,
		(double)light->color_upper_bound.rgb.blue, light->cutoff_angle > 0.0f && light->cutoff_angle < 3.1f ?
		" (spot)" : "");
}

/* a model shader's self-illumination, if it has one that shows */
static struct glow_shader_model const *glow_shader_self_illuminated(long shader_index)
{
	struct shader *shader = shader_index != NONE ? shader_definition_get(shader_index) : NULL;
	struct glow_shader_model const *model;

	if (!shader || shader->base.type != _shader_type_model)
		return NULL;
	model = (struct glow_shader_model const *)shader;
	if (model->multipurpose_map.index == NONE)
		return NULL;
	if (model->self_illumination_color_upper_bound.red + model->self_illumination_color_upper_bound.green +
		model->self_illumination_color_upper_bound.blue <= 0.0f &&
		model->self_illumination_color_lower_bound.red + model->self_illumination_color_lower_bound.green +
		model->self_illumination_color_lower_bound.blue <= 0.0f)
		return NULL;
	return model;
}

/* debug.glow_inventory: the map's lights, objects (their lights,
contrails, particle systems and self-illuminated shaders) and effects
(their lights and particles), each logged */
static void glow_inventory_dump(void)
{
	static unsigned long const object_groups[] = { 'proj', 'weap', 'eqip', 'vehi', 'bipd', 'scen', 'mach', 'garb' };
	struct tag_iterator iterator;
	long tag_index;
	short group;

	platform_log("glow inventory: the map's tags");
	tag_iterator_new(&iterator, 'ligh');
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		glow_inventory_light("light tag", tag_index);
	for (group = 0; group < (short)NUMBEROF(object_groups); group++)
	{
		tag_iterator_new(&iterator, object_groups[group]);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			struct object_definition const *definition =
				(struct object_definition const *)tag_get(object_groups[group], tag_index);
			char owner[300], group_text[5];
			long index;

			glow_group_text(object_groups[group], group_text);
			snprintf(owner, sizeof(owner), "%s %s", group_text, glow_tag_name(tag_index));
			for (index = 0; index < definition->object.attachments.count; index++)
			{
				struct object_attachment_definition const *attachment = TAG_BLOCK_GET_ELEMENT(
					&definition->object.attachments, index, struct object_attachment_definition);
				char type[5];

				if (attachment->type.index == NONE)
					continue;
				if (attachment->type.group_tag == 'ligh')
				{
					glow_inventory_light(owner, attachment->type.index);
					continue;
				}
				glow_group_text(attachment->type.group_tag, type);
				platform_log("glow inventory: %s: attachment %s %s at %s", owner, type,
					glow_tag_name(attachment->type.index), attachment->marker_name);
			}
			if (definition->object.model.index != NONE)
			{
				struct model const *model = (struct model const *)tag_get(definition->object.model.group_tag,
					definition->object.model.index);

				for (index = 0; model && index < model->shaders.count; index++)
				{
					long shader_index = TAG_BLOCK_GET_ELEMENT(&model->shaders, index,
						struct model_shader_reference_glow)->shader.index;
					struct glow_shader_model const *glow = glow_shader_self_illuminated(shader_index);
					struct shader *shader = shader_index != NONE ? shader_definition_get(shader_index) : NULL;

					if (glow)
						platform_log("glow inventory: %s: self-illuminated shader %s: %.2f %.2f %.2f..%.2f %.2f %.2f "
							"by function %d, animation %d over %.2f s", owner, glow_tag_name(shader_index),
							(double)glow->self_illumination_color_lower_bound.red,
							(double)glow->self_illumination_color_lower_bound.green,
							(double)glow->self_illumination_color_lower_bound.blue,
							(double)glow->self_illumination_color_upper_bound.red,
							(double)glow->self_illumination_color_upper_bound.green,
							(double)glow->self_illumination_color_upper_bound.blue,
							(int)glow->self_illumination_color_source, (int)glow->self_illumination_animation_function,
							(double)glow->self_illumination_animation_period);
					else if (shader && shader->base.type >= 5)
						platform_log("glow inventory: %s: transparent shader %s (type %d)", owner,
							glow_tag_name(shader_index), (int)shader->base.type);
				}
			}
		}
	}
	tag_iterator_new(&iterator, 'effe');
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct effect_definition const *effect = (struct effect_definition const *)tag_get('effe', tag_index);
		char owner[300];
		long event_index, lights = 0, particles = 0, systems = 0, others = 0;

		snprintf(owner, sizeof(owner), "effe %s", glow_tag_name(tag_index));
		for (event_index = 0; event_index < effect->events.count; event_index++)
		{
			struct effect_event_definition const *event = TAG_BLOCK_GET_ELEMENT(&effect->events, event_index,
				struct effect_event_definition);
			long part_index;

			particles += event->particles.count;
			for (part_index = 0; part_index < event->parts.count; part_index++)
			{
				struct effect_part_definition const *part = TAG_BLOCK_GET_ELEMENT(&event->parts, part_index,
					struct effect_part_definition);

				if (part->reference.index == NONE)
					continue;
				if (part->reference.group_tag == 'ligh')
				{
					glow_inventory_light(owner, part->reference.index);
					lights++;
				}
				else if (part->reference.group_tag == 'pctl')
					systems++;
				else
					others++;
			}
		}
		platform_log("glow inventory: %s: %ld lights, %ld particles, %ld particle systems, %ld other parts", owner,
			lights, particles, systems, others);
	}
	platform_log("glow inventory: the map's tags end");
}

/* an object definition's glow (found once) */
static struct glow_definition const *glow_definition_get(long definition_index)
{
	struct glow_definition *glow;
	struct object_definition const *definition;
	long index;

	for (index = 0; index < glow_lights.definition_count; index++)
		if (glow_lights.definitions[index].definition_index == definition_index)
			return &glow_lights.definitions[index];
	if (glow_lights.definition_count >= DEFINITIONS_MOST)
		return NULL;
	glow = &glow_lights.definitions[glow_lights.definition_count++];
	memset(glow, 0, sizeof(*glow));
	glow->definition_index = definition_index;
	definition = object_definition_get(definition_index);
	for (index = 0; index < definition->object.attachments.count; index++)
	{
		struct object_attachment_definition const *attachment = TAG_BLOCK_GET_ELEMENT(
			&definition->object.attachments, index, struct object_attachment_definition);

		if (attachment->type.group_tag == 'ligh' && attachment->type.index != NONE)
			glow->has_light = TRUE;
	}
	if (definition->object.model.index != NONE)
	{
		struct model const *model = (struct model const *)tag_get(definition->object.model.group_tag,
			definition->object.model.index);

		for (index = 0; model && index < model->shaders.count; index++)
		{
			struct glow_shader_model const *shader = glow_shader_self_illuminated(TAG_BLOCK_GET_ELEMENT(
				&model->shaders, index, struct model_shader_reference_glow)->shader.index);
			real brightness;

			if (!shader)
				continue;
			brightness = shader->self_illumination_color_upper_bound.red +
				shader->self_illumination_color_upper_bound.green + shader->self_illumination_color_upper_bound.blue;
			/* (the brightest of its shaders) */
			if (glow->glows && brightness <= glow->upper.red + glow->upper.green + glow->upper.blue)
				continue;
			glow->glows = TRUE;
			glow->color_source = shader->self_illumination_color_source;
			glow->lower = shader->self_illumination_color_lower_bound;
			glow->upper = shader->self_illumination_color_upper_bound;
			glow->animation_function = shader->self_illumination_animation_function;
			glow->animation_period = shader->self_illumination_animation_period;
		}
	}
	return glow;
}

/* an equipment's glow that is a lamp's, not a label's: none at its lower
bound (lit by its object function, as the plasma grenade's button) and a
saturated colour at its upper (an ammunition box's printed decal is grey,
alike at both bounds) */
static boolean glow_saturated(struct glow_definition const *glow)
{
	real most = MAX(glow->upper.red, MAX(glow->upper.green, glow->upper.blue));
	real least = MIN(glow->upper.red, MIN(glow->upper.green, glow->upper.blue));

	return glow->lower.red + glow->lower.green + glow->lower.blue <= 0.0f && most > 0.0f &&
		(most - least) / most >= 0.5f;
}

/* a projectile's light as last seen, and its strike's flash once the
projectile is gone (it struck, or burst): its light where it was last, its
colour STRIKE_BRIGHTER times and its reach STRIKE_REACH times its own, to
none over STRIKE_TICKS of the game's clock (a strike's particles' flash:
CE gives a plasma bolt's or a needle's strike no light, its particles alone,
and the bolt's own light went out at once) */
static void glow_strike_track(long object_index, real radius, real_rgb_color const *color,
	real_point3d const *position)
{
	struct glow_strike *strike = NULL;
	long index;

	for (index = 0; index < glow_lights.strike_count; index++)
		if (glow_lights.strikes[index].object_index == object_index)
			strike = &glow_lights.strikes[index];
	if (!strike)
	{
		/* (no room: the oldest's place) */
		if (glow_lights.strike_count < STRIKES_MOST)
			strike = &glow_lights.strikes[glow_lights.strike_count++];
		else
		{
			strike = &glow_lights.strikes[0];
			for (index = 1; index < STRIKES_MOST; index++)
				if (glow_lights.strikes[index].seen < strike->seen)
					strike = &glow_lights.strikes[index];
		}
		memset(strike, 0, sizeof(*strike));
		strike->object_index = object_index;
		strike->struck = NONE;
		strike->last = *position;
	}
	else if (strike->position.x != position->x || strike->position.y != position->y ||
		strike->position.z != position->z)
	{
		strike->last = strike->position;
	}
	strike->position = *position;
	strike->radius = radius;
	strike->color = *color;
	strike->seen = game_time_get();
}

/* the strikes: each projectile gone since it was seen flashes, then is
forgotten */
static void glow_strikes_update(void)
{
	long now = game_time_get(), index, kept = 0;

	for (index = 0; index < glow_lights.strike_count; index++)
	{
		struct glow_strike *strike = &glow_lights.strikes[index];

		if (strike->struck == NONE && !object_try_and_get(strike->object_index))
			strike->struck = now;
		if (strike->struck != NONE ? now - strike->struck >= STRIKE_TICKS : now - strike->seen > STRIKE_TICKS)
			continue;
		glow_lights.strikes[kept++] = *strike;
	}
	glow_lights.strike_count = kept;
}

/* ---------- public code */

static void glow_lights_settings(void)
{
	/* (read once at the start whatever the count of changes, then again
	at each change) */
	if (!glow_lights.read || glow_lights.changes != config_changes())
	{
		glow_lights.read = TRUE;
		glow_lights.changes = config_changes();
		glow_lights.inventory = config_boolean("debug.glow_inventory") != 0;
		glow_lights.scale = PIN((real)config_real("display.prism_glow_lights"), 0.0f, 4.0f);
	}
}

boolean glow_lights_all(void)
{
	glow_lights_settings();
	return glow_lights.scale > 0.0f && (prism_deferred_pathtraced() || prism_deferred_raster_materials());
}

/* a lens flare's light's share by what carries it: a weapon's glow (the
plasma pistol's and needler's flares, carried all the time by every Grunt
and Jackal) GLOW_WEAPON_SHARE, its glowing parts small beside its flare;
a projectile's, a vehicle's or an effect's whole */
real glow_lights_share(short object_type)
{
	return object_type == _object_type_weapon ? GLOW_WEAPON_SHARE : 1.0f;
}

real glow_lights_scale(void)
{
	glow_lights_settings();
	return glow_lights.scale;
}

boolean glow_lights_inventory_on(void)
{
	glow_lights_settings();
	return glow_lights.inventory;
}

void glow_lights_seen(long definition_index, long object_index, boolean dynamic, boolean drawn, real radius,
	real_rgb_color const *color, real_point3d const *position)
{
	struct object_datum *object;
	long owner = NONE, index;

	object = object_index != NONE ? object_try_and_get(object_index) : NULL;
	if (object && object->object.type == _object_type_projectile && glow_lights_all())
		glow_strike_track(object_index, radius, color, position);
	if (!glow_lights_inventory_on())
		return;
	if (object)
		owner = object->definition_index;
	for (index = 0; index < glow_lights.seen_count; index++)
		if (glow_lights.seen[index][0] == definition_index && glow_lights.seen[index][1] == owner)
			return;
	if (glow_lights.seen_count < SEEN_MOST)
	{
		glow_lights.seen[glow_lights.seen_count][0] = definition_index;
		glow_lights.seen[glow_lights.seen_count][1] = owner;
		glow_lights.seen_count++;
	}
	platform_log("glow inventory: in play: light %s on %s (type %d): %s, %s, radius %.2f, colour %.2f %.2f %.2f",
		glow_tag_name(definition_index), object ? glow_tag_name(owner) : "nothing", object ? (int)object->object.type : -1,
		dynamic ? "dynamic" : "lens flare only", drawn ? "drawn" : "not drawn", (double)radius,
		(double)color->red, (double)color->green, (double)color->blue);
}

/* the frame's glowing objects: the nearest GLOW_OBJECTS_MOST of those in
range that glow and carry no light, each a light at its bounding sphere's
middle */
void glow_lights_frame(real_point3d const *camera)
{
	struct object_iterator iterator;
	struct object_datum *object;
	real distances[GLOW_OBJECTS_MOST];

	glow_lights.glow_count = 0;
	if (glow_lights_inventory_on() && !glow_lights.dumped)
	{
		glow_lights.dumped = TRUE;
		glow_inventory_dump();
	}
	if (!glow_lights_all() || !camera)
		return;
	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		struct glow_definition const *glow;
		struct glow_lights_light *light;
		real dx = object->object.bounding_sphere_center.x - camera->x;
		real dy = object->object.bounding_sphere_center.y - camera->y;
		real dz = object->object.bounding_sphere_center.z - camera->z;
		real distance = dx * dx + dy * dy + dz * dz, value = 1.0f, radius;
		long at;

		if (distance > GLOW_RANGE * GLOW_RANGE)
			continue;
		/* (a projectile in flight, or a grenade on the ground, with a glow
		and no light of its own; not the bodies, vehicles and weapons, whose
		self-illumination is masked to small parts by their maps (an
		Elite's and the Chief's armour have white self-illumination
		colours) and whose glowing parts carry CE's lights where they show:
		the weapons' flares, the shields' (shield_glow.c)) */
		if (object->object.type != _object_type_projectile && object->object.type != _object_type_equipment)
			continue;
		glow = glow_definition_get(object->definition_index);
		if (!glow || !glow->glows || glow->has_light)
			continue;
		if (object->object.type == _object_type_equipment && !glow_saturated(glow))
			continue;
		/* (its colour as CE draws it: between the bounds by its function) */
		if (glow->color_source > 0 && glow->color_source <= NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS)
			value = PIN(object->object.outgoing_function_values[glow->color_source - 1], 0.0f, 1.0f);
		/* (the nearest kept: an insertion by distance) */
		for (at = glow_lights.glow_count; at > 0 && distances[at - 1] > distance; at--);
		if (at >= GLOW_OBJECTS_MOST)
			continue;
		if (glow_lights.glow_count < GLOW_OBJECTS_MOST)
			glow_lights.glow_count++;
		memmove(&glow_lights.glows[at + 1], &glow_lights.glows[at],
			(size_t)(glow_lights.glow_count - 1 - at) * sizeof(glow_lights.glows[0]));
		memmove(&distances[at + 1], &distances[at], (size_t)(glow_lights.glow_count - 1 - at) * sizeof(real));
		distances[at] = distance;
		light = &glow_lights.glows[at];
		radius = object->object.bounding_sphere_radius;
		light->id = iterator.index;
		light->strike = FALSE;
		light->position[0] = object->object.bounding_sphere_center.x;
		light->position[1] = object->object.bounding_sphere_center.y;
		light->position[2] = object->object.bounding_sphere_center.z;
		light->color[0] = glow->lower.red + (glow->upper.red - glow->lower.red) * value;
		light->color[1] = glow->lower.green + (glow->upper.green - glow->lower.green) * value;
		light->color[2] = glow->lower.blue + (glow->upper.blue - glow->lower.blue) * value;
		light->size = radius;
	}
	/* (the strikes' flashes, after the objects: the room left) */
	glow_strikes_update();
	{
		long now = game_time_get(), index;

		for (index = 0; index < glow_lights.strike_count && glow_lights.glow_count < GLOW_OBJECTS_MOST; index++)
		{
			struct glow_strike const *strike = &glow_lights.strikes[index];
			struct glow_lights_light *light = &glow_lights.glows[glow_lights.glow_count];
			real fade, dx, dy, dz, along;

			if (strike->struck == NONE)
				continue;
			fade = 1.0f - (real)(now - strike->struck) / (real)STRIKE_TICKS;
			fade = PIN(fade, 0.0f, 1.0f);
			fade *= fade;
			dx = strike->position.x - strike->last.x;
			dy = strike->position.y - strike->last.y;
			dz = strike->position.z - strike->last.z;
			along = (real)sqrt((double)(dx * dx + dy * dy + dz * dz));
			along = along > 1.0e-4f ? STRIKE_BACK / along : 0.0f;
			glow_lights.glow_count++;
			light->id = strike->object_index;
			light->strike = TRUE;
			light->position[0] = strike->position.x - dx * along;
			light->position[1] = strike->position.y - dy * along;
			light->position[2] = strike->position.z - dz * along;
			light->color[0] = strike->color.red * STRIKE_BRIGHTER * fade;
			light->color[1] = strike->color.green * STRIKE_BRIGHTER * fade;
			light->color[2] = strike->color.blue * STRIKE_BRIGHTER * fade;
			light->size = strike->radius * STRIKE_REACH;
		}
	}
}

long glow_lights_count(void)
{
	return glow_lights.glow_count;
}

struct glow_lights_light const *glow_lights_get(void)
{
	return glow_lights.glows;
}

void glow_lights_map_unloaded(void)
{
	glow_lights.dumped = FALSE;
	glow_lights.seen_count = 0;
	glow_lights.definition_count = 0;
	glow_lights.glow_count = 0;
	glow_lights.strike_count = 0;
}
