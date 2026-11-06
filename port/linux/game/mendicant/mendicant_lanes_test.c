/*
MENDICANT_LANES_TEST.C

The vehicle lanes' gate scene (ai.mendicant.lane_test_log, training only;
docs/mendicant_port.md, "Vehicle lanes"): its measures written for both
layers alike, so the layer off is the stock baseline.

Once the player has control, the clearest straight way is found from the
Chief or any of the level's firing positions in the loaded part of it (level
ground with no lip that would throw a vehicle, room for it at its wheels and
its roof, no machine in it, and the soldiers' line able to see its start);
the scenery in it is taken away, every actor is erased, and the Chief is put
at the wheel of a Warthog (a Ghost on a level with none) at its start. Then,
for each species the level has (Grunts, Jackals, Elites, Hunters, Marines), a
line of them (four, two Elites or Hunters) is placed across his way 40 to 75
world units ahead, facing him, well inside his vehicle's width; they are told
he is there (as a level's script tells its encounters) and he drives through
them twice at 15 world units a second, held by script along the line (a
Warthog's own top speed is near 7; HALO_AI_MENDICANT_LANE_TEST_SPEED sets
another, 0 the throttle alone), kept on its wheels and to the ground. The
second pass has the same soldiers put back in their places (a fresh one for
each the first pass killed), so the memory of the first pass shows. The
Chief and his vehicle take no damage.

Per pass ("pass" rows): its speed at the line, the vehicle's radius, its
widest drift off the line, how often it was set upright, and whether it is
valid (at the line faster than 10, within 1.5 of it). Per soldier and pass
("soldier" rows): whether he was in the lane when the vehicle was 3 seconds
off (within its radius plus 0.3 of its line), how far from the line he stood
then and a second before it reached him, whether it hit or killed him, and
the ticks it reached him, he began his own move, the game dived him, the
layer saw him perceive it, and he could first see or hear it (an unblocked
line from his head, or within 25 world units); his reaction, reaction time,
lead and memory. tools: build/authentic_test/bin/m4_stats.py.
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/players.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "ai/ai.h"
#include "ai/ai_script.h"
#include "ai/actors.h"
#include "ai/actor_definitions.h"
#include "ai/ai_debug.h"
#include "ai/props.h"
#include "cutscene/cinematics.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "physics/collisions.h"
#include "ai/ai_scenario_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "tag_files/tag_groups.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define TEST_SPECIES 5
#define TEST_SOLDIERS 4
#define TEST_PASSES 2
#define TEST_SPEED 15.0f       /* world units a second */
#define TEST_SETTLE_TICKS 75
#define TEST_DRIVE_TICKS 420
#define TEST_STOP_TICKS 60
#define TEST_BEYOND 12.0f      /* the pass ends this far past the line */
#define TEST_HEAR 25.0f
#define TEST_DANGER_VEHICLE 3
#define TEST_DIVE_SUCCESS 4
#define TEST_MAGIC_SIGHT 3     /* actors.c's _ai_unit_effect_magic_sight */

enum { TEST_GRUNT, TEST_JACKAL, TEST_ELITE, TEST_HUNTER, TEST_MARINE };

enum
{
	TEST_WAIT,
	TEST_PLACE,
	TEST_SETTLE,
	TEST_DRIVE,
	TEST_STOP,
	TEST_END,
	TEST_FAILED
};

static char const *const test_species_names[TEST_SPECIES] = { "grunt", "jackal", "elite", "hunter", "marine" };
static char const *const test_species_not[TEST_SPECIES] = { NULL, NULL, "stealth", NULL, "armored" };
static int const test_species_count[TEST_SPECIES] = { 4, 4, 2, 2, 4 };

/* ---------- globals */

FILE *mendicant_lane_test_file = NULL;

static struct lane_test_soldier
{
	long actor;
	long unit;
	boolean fresh;          /* placed for this pass (the first, or a replacement) */
	real_point3d spot;
	real along;             /* his place along the line */
	boolean lane_checked, one_checked, arrived;
	boolean in_lane;
	real lat3, lat1;
	boolean hit, killed;
	long arrival_tick, move_tick, dive_tick, seen_tick, cue_tick;
	int reaction;
	real reaction_time, lead, memory;
	long last_dive_check;
} lane_test_soldiers[TEST_SOLDIERS];

static struct
{
	int phase;
	long phase_time;
	long last_time;
	long variants[TEST_SPECIES];
	long vehicle_definition;
	long vehicle;
	int species;            /* the pass's species */
	int pass;               /* 0 or 1 */
	int count;
	real_point3d start;
	real yaw, fx, fy, rx, ry;
	real line;              /* the soldiers' line, this far along */
	real line_z;            /* the ground's height there */
	real radius;            /* the vehicle's */
	real line_speed;        /* its speed as it reached the line */
	real speed;             /* the scripted speed (world units a second), 0: the throttle's own */
	real drift;             /* its widest drift off the line */
	int uprightings;        /* times it was set back upright this pass */
	unsigned long random;
	boolean braking;
} lane_test;

/* ---------- private code */

static void test_log(
	char const *format,
	...)
{
	va_list arguments;

	if (!mendicant_lane_test_file)
		return;
	fprintf(mendicant_lane_test_file, "%ld,", mendicant_time());
	va_start(arguments, format);
	vfprintf(mendicant_lane_test_file, format, arguments);
	va_end(arguments);
	fputc('\n', mendicant_lane_test_file);
	fflush(mendicant_lane_test_file);
}

static void test_phase(
	int phase,
	long time)
{
	lane_test.phase = phase;
	lane_test.phase_time = time;
}

static unsigned long test_rand(
	void)
{
	lane_test.random ^= (lane_test.random << 13) & 0xFFFFFFFFUL;
	lane_test.random ^= lane_test.random >> 17;
	lane_test.random ^= (lane_test.random << 5) & 0xFFFFFFFFUL;
	lane_test.random &= 0xFFFFFFFFUL;
	return lane_test.random;
}

static real test_randf(
	void)
{
	return (real)(test_rand() & 0xffffUL) / 65536.0f;
}

static struct unit_datum *test_unit(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_unit);
}

static struct actor_datum *test_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static long test_palette_find(
	struct tag_block *palette,
	size_t element_size,
	char const *part,
	char const *not_part)
{
	int index;

	for (index = 0; index < palette->count; index++)
	{
		struct tag_reference *reference = (struct tag_reference *)tag_block_get_element_fast(palette, index, element_size);
		char const *name;

		if (reference->index == NONE)
			continue;
		name = tag_get_name(reference->index);
		if (name && strstr(name, part) && (!not_part || !strstr(name, not_part)))
			return reference->index;
	}
	return NONE;
}

static boolean test_ground(
	real x,
	real y,
	real z,
	real_point3d *ground)
{
	real_point3d point;
	boolean found;

	point.x = x;
	point.y = y;
	point.z = z + 3.0f;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
	found = mendicant_ground_under(&point, ground);
	global_current_collision_user_depth--;
	return found && fabs(ground->z - z) < 4.0f;
}

static boolean test_clear(
	real_point3d const *from,
	real_point3d const *to,
	real lift)
{
	real_point3d a = *from, b = *to;
	int blocked;

	a.z += lift;
	b.z += lift;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
	blocked = mendicant_line_blocked(&a, &b);
	global_current_collision_user_depth--;
	return blocked == 0;
}

/* the way is clear for a vehicle: the level and its scenery, machines and
vehicles, at two heights, along both its sides */
/* the way is clear for a vehicle: the level along its middle (the scenery in
the chosen way is taken away: test_clear_scenery) */
static boolean test_way_clear(
	real_point3d const *from,
	real_point3d const *to)
{
	real dx = to->x - from->x, dy = to->y - from->y, length = (real)sqrt(dx * dx + dy * dy);
	real_point3d a = *from, b = *to;

	if (!test_clear(from, to, 1.0f) || !test_clear(from, to, 0.5f) || length < 0.01f)
		return length < 0.01f;
	/* both its sides, at its wheels' height */
	a.x += -dy / length * 0.8f;
	a.y += dx / length * 0.8f;
	b.x += -dy / length * 0.8f;
	b.y += dx / length * 0.8f;
	if (!test_clear(&a, &b, 0.6f))
		return FALSE;
	a.x -= -dy / length * 1.6f;
	a.y -= dx / length * 1.6f;
	b.x -= -dy / length * 1.6f;
	b.y -= dx / length * 1.6f;
	if (!test_clear(&a, &b, 0.6f))
		return FALSE;
	/* no machine (a door, a lift) in its way */
	{
		real_point3d m = *from;
		real_vector3d vector;
		struct collision_result collision;
		boolean blocked;

		m.z += 0.9f;
		vector.i = dx;
		vector.j = dy;
		vector.k = to->z - from->z;
		global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
		blocked = collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_objects_bit) |
			FLAG(_collision_test_objects_machines_bit), &m, &vector, NONE, &collision);
		global_current_collision_user_depth--;
		return !blocked;
	}
}

/* the scenery in the vehicle's way (trees, rocks) is taken away for the scene */
static void test_clear_scenery(
	real length)
{
	struct object_iterator iterator;
	struct object_datum *object;
	int removed = 0;

	object_iterator_new(&iterator, _object_mask_scenery, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real dx = object->object.position.x - lane_test.start.x, dy = object->object.position.y - lane_test.start.y;
		real along = dx * lane_test.fx + dy * lane_test.fy, across = dx * lane_test.rx + dy * lane_test.ry;
		real room = object->object.bounding_sphere_radius + 4.0f;

		if (along > -8.0f && along < length + 8.0f && across > -room && across < room)
		{
			object_delete(iterator.index);
			removed++;
		}
	}
	test_log("scenery_removed,%d", removed);
}

/* how far a vehicle can drive straight from a point along a heading (5 world unit steps, level ground) */
static real test_clear_run(
	real_point3d const *origin,
	real yaw,
	real limit)
{
	real clear = 0.0f, step, last_rise = 0.0f;
	real_point3d previous = *origin;

	for (step = 2.5f; step <= limit; step += 2.5f)
	{
		real_point3d point;
		real rise;

		if (!test_ground(origin->x + (real)cos(yaw) * step, origin->y + (real)sin(yaw) * step, previous.z, &point))
			break;
		rise = point.z - previous.z;
		/* level enough, and no bump that would throw it at speed */
		if (fabs(rise) > 0.8f || (step > 2.5f && fabs(rise - last_rise) > 0.6f) || fabs(point.z - origin->z) > 8.0f ||
			!test_way_clear(&previous, &point) || !test_clear(&previous, &point, 1.6f))
			break;
		/* the soldiers there can see the start (no crest between) */
		{
			real_point3d eye = point, seat = *origin;

			eye.z += 0.6f;
			seat.z += 1.2f;
			if (!test_clear(&eye, &seat, 0.0f))
				break;
		}
		clear = step;
		previous = point;
		last_rise = rise;
	}
	return clear;
}

/* the clearest heading from a ground point, kept when it beats the best so far */
static void test_search_from(
	real_point3d const *ground,
	real *best_clear,
	real *best_yaw)
{
	int direction;

	for (direction = 0; direction < 24; direction++)
	{
		real yaw = direction * (real)(2.0f * _pi / 24.0f);
		real clear = test_clear_run(ground, yaw, 120.0f);

		if (clear > *best_clear)
		{
			*best_clear = clear;
			*best_yaw = yaw;
			lane_test.start = *ground;
		}
	}
}

static long test_place(
	long definition_index,
	real_point3d const *at,
	real yaw,
	real lift)
{
	struct object_placement_data placement;

	object_placement_data_new(&placement, definition_index, NONE);
	placement.position = *at;
	placement.position.z += lift;
	placement.forward.i = (real)cos(yaw);
	placement.forward.j = (real)sin(yaw);
	placement.forward.k = 0.0f;
	placement.up.i = 0.0f;
	placement.up.j = 0.0f;
	placement.up.k = 1.0f;
	return object_new(&placement);
}

static char const *test_driver_seat(
	struct unit_datum *vehicle)
{
	struct unit_definition *definition = unit_definition_get(vehicle->definition_index);
	int index;

	for (index = 0; index < definition->unit.seats.count; index++)
	{
		struct unit_seat *seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, index, struct unit_seat);

		if (TEST_FLAG(seat->flags, _unit_seat_driver_bit))
			return seat->label;
	}
	return NULL;
}

static long test_actor_of_unit(
	long unit_index)
{
	int index;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);

		if (actor->identifier && actor->meta.unit_index == unit_index)
			return ((long)(unsigned short)actor->identifier << 16) | index;
	}
	return NONE;
}

static boolean test_ours(
	long actor_index)
{
	int slot;

	for (slot = 0; slot < lane_test.count; slot++)
	{
		if (lane_test_soldiers[slot].actor == actor_index)
			return TRUE;
	}
	return FALSE;
}

static real test_along(
	real_point3d const *point)
{
	return (point->x - lane_test.start.x) * lane_test.fx + (point->y - lane_test.start.y) * lane_test.fy;
}

static real test_lateral(
	real_point3d const *point)
{
	return (point->x - lane_test.start.x) * lane_test.rx + (point->y - lane_test.start.y) * lane_test.ry;
}

static boolean test_chief_in(
	void)
{
	struct unit_datum *chief = test_unit(mendicant_player_unit());

	return chief && chief->object.parent_object_index == lane_test.vehicle && lane_test.vehicle != NONE;
}

/* the Chief at the wheel, his vehicle at the start, still */
static void test_vehicle_home(
	void)
{
	struct unit_datum *vehicle = test_unit(lane_test.vehicle);
	struct unit_datum *chief = test_unit(mendicant_player_unit());
	real_vector3d forward, up;
	real_point3d at = lane_test.start;

	if (!vehicle)
		return;
	forward.i = lane_test.fx;
	forward.j = lane_test.fy;
	forward.k = 0.0f;
	up.i = 0.0f;
	up.j = 0.0f;
	up.k = 1.0f;
	at.z += 0.6f;
	object_set_position(lane_test.vehicle, &at, &forward, &up);
	vehicle->object.translational_velocity.i = vehicle->object.translational_velocity.j =
		vehicle->object.translational_velocity.k = 0.0f;
	vehicle->object.angular_velocity.i = vehicle->object.angular_velocity.j = vehicle->object.angular_velocity.k = 0.0f;
	if (chief && chief->object.parent_object_index == NONE)
	{
		char const *seat = test_driver_seat(vehicle);

		if (seat)
			unit_scripting_enter_vehicle(mendicant_player_unit(), lane_test.vehicle, seat);
	}
}

/* the scene: TRUE when it is in place */
static boolean test_setup(
	void)
{
	struct unit_datum *chief = test_unit(mendicant_player_unit());
	real best_clear = 0.0f, best_yaw = 0.0f;
	int direction, species, found = 0;

	if (!chief || !global_scenario)
		return FALSE;
	lane_test.random = ((unsigned long)mendicant_config.seed * 2654435761UL) ^ 0x5bd1e995UL;
	/* the vehicle's speed: 15 world units a second, held by script (a Warthog's
	own top speed is near 7); HALO_AI_MENDICANT_LANE_TEST_SPEED sets another, 0 the throttle alone */
	lane_test.speed = TEST_SPEED;
	if (getenv("HALO_AI_MENDICANT_LANE_TEST_SPEED"))
		lane_test.speed = (real)atof(getenv("HALO_AI_MENDICANT_LANE_TEST_SPEED"));
	if (!lane_test.random)
		lane_test.random = 2463534242UL;
	for (species = 0; species < TEST_SPECIES; species++)
	{
		lane_test.variants[species] = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference),
			test_species_names[species], test_species_not[species]);
		if (lane_test.variants[species] == NONE && test_species_not[species])
			lane_test.variants[species] = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference),
				test_species_names[species], NULL);
		found += lane_test.variants[species] != NONE;
	}
	lane_test.vehicle_definition = test_palette_find(&global_scenario->vehicle_palette, sizeof(struct scenario_object_palette_entry),
		"warthog", "rocket");
	if (lane_test.vehicle_definition == NONE)
		lane_test.vehicle_definition = test_palette_find(&global_scenario->vehicle_palette,
			sizeof(struct scenario_object_palette_entry), "warthog", NULL);
	if (lane_test.vehicle_definition == NONE)
		lane_test.vehicle_definition = test_palette_find(&global_scenario->vehicle_palette,
			sizeof(struct scenario_object_palette_entry), "ghost", NULL);
	test_log("palette,%ld,%ld,%ld,%ld,%ld,%ld", lane_test.variants[0], lane_test.variants[1], lane_test.variants[2],
		lane_test.variants[3], lane_test.variants[4], lane_test.vehicle_definition);
	if (!found || lane_test.vehicle_definition == NONE)
	{
		test_log("setup_failed,species=%d,vehicle=%ld", found, lane_test.vehicle_definition);
		return FALSE;
	}
	/* the clearest straight line: from the Chief, or from any of the level's
	firing positions with ground under it (a vehicle's way: no scenery in it) */
	object_get_origin(mendicant_player_unit(), &lane_test.start);
	{
		real_point3d ground;
		int encounter_count = global_scenario->ai_encounters.count, tried = 0, grounded = 0, total = 0, pass, every = 1;

		/* the Chief's own spot first */
		if (test_ground(lane_test.start.x, lane_test.start.y, lane_test.start.z, &ground))
			test_search_from(&ground, &best_clear, &best_yaw);
		/* then the level's firing positions with ground under them (the loaded
		part of the level): a few hundred of them, evenly */
		for (pass = 0; pass < 2 && best_clear < 110.0f; pass++)
		{
			int encounter, seen = 0;

			for (encounter = 0; encounter < encounter_count && best_clear < 110.0f; encounter++)
			{
				struct encounter_definition *definition =
					TAG_BLOCK_GET_ELEMENT(&global_scenario->ai_encounters, encounter, struct encounter_definition);
				int position;

				for (position = 0; position < definition->firing_positions.count && best_clear < 110.0f; position++)
				{
					real_point3d const *origin =
						&TAG_BLOCK_GET_ELEMENT(&definition->firing_positions, position, struct firing_position_definition)->position;

					if (!pass)
						total++;
					if (!test_ground(origin->x, origin->y, origin->z, &ground))
						continue;
					if (!pass)
					{
						grounded++;
						continue;
					}
					if ((seen++ % every) != 0)
						continue;
					tried++;
					test_search_from(&ground, &best_clear, &best_yaw);
				}
			}
			every = grounded > 300 ? grounded / 300 : 1;
		}
		test_log("search,%d,%d,%d,%.0f,%.1f,%.1f,%.1f", tried, grounded, total, best_clear, lane_test.start.x,
			lane_test.start.y, lane_test.start.z);
	}
	if (best_clear < 50.0f)
	{
		test_log("setup_failed,clear=%.0f", best_clear);
		return FALSE;
	}
	lane_test.yaw = best_yaw;
	lane_test.fx = (real)cos(best_yaw);
	lane_test.fy = (real)sin(best_yaw);
	lane_test.rx = -lane_test.fy;
	lane_test.ry = lane_test.fx;
	lane_test.line = best_clear - TEST_BEYOND < 75.0f ? best_clear - TEST_BEYOND : 75.0f;
	test_clear_scenery(lane_test.line + TEST_BEYOND);
	{
		/* the ground's height along the way, to the soldiers' line */
		real step;
		real_point3d previous = lane_test.start, point;

		for (step = 2.5f; step <= lane_test.line + 2.0f; step += 2.5f)
		{
			if (test_ground(lane_test.start.x + lane_test.fx * step, lane_test.start.y + lane_test.fy * step, previous.z, &point))
				previous = point;
		}
		lane_test.line_z = previous.z;
	}
	ai_scripting_erase_all();
	{
		/* the level's own vehicles nearby go (nobody aboard) */
		struct object_iterator iterator;
		struct unit_datum *vehicle;

		object_iterator_new(&iterator, _object_mask_vehicle, 0);
		while ((vehicle = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real dx = vehicle->object.position.x - lane_test.start.x, dy = vehicle->object.position.y - lane_test.start.y;

			if (dx * dx + dy * dy < 130.0f * 130.0f && vehicle->unit.driver_object_index == NONE &&
				vehicle->unit.gunner_object_index == NONE)
				object_delete(iterator.index);
		}
	}
	lane_test.vehicle = test_place(lane_test.vehicle_definition, &lane_test.start, best_yaw, 0.4f);
	if (lane_test.vehicle == NONE)
	{
		test_log("setup_failed,no_vehicle");
		return FALSE;
	}
	{
		struct unit_datum *vehicle = test_unit(lane_test.vehicle);

		lane_test.radius = vehicle ? vehicle->object.bounding_sphere_radius : 1.5f;
		if (vehicle)
			SET_FLAG(vehicle->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
		SET_FLAG(chief->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
	}
	test_vehicle_home();
	lane_test.species = -1;
	lane_test.pass = TEST_PASSES - 1;
	lane_test.count = 0;
	test_log("setup,clear=%.0f,line=%.0f,yaw=%.2f,radius=%.2f,layer=%d,character=%d,speed=%.1f", best_clear, lane_test.line,
		best_yaw, lane_test.radius, mendicant_config.lane_clearing ? 1 : 0, mendicant_config.character ? 1 : 0, lane_test.speed);
	return TRUE;
}

/* a soldier of the pass's species at his spot, facing the Chief */
static boolean test_place_soldier(
	int slot)
{
	struct lane_test_soldier *soldier = &lane_test_soldiers[slot];
	long variant = lane_test.variants[lane_test.species];
	long unit_index;

	unit_index = test_place(actor_variant_definition_get(variant)->unit_reference.index, &soldier->spot,
		lane_test.yaw + _pi, 0.1f);
	if (unit_index == NONE)
		return FALSE;
	ai_scripting_attach_free(unit_index, variant);
	soldier->unit = unit_index;
	soldier->actor = test_actor_of_unit(unit_index);
	soldier->fresh = TRUE;
	return soldier->actor != NONE;
}

/* the next pass: its soldiers in place, the Chief back at the start; FALSE
when there are no passes left */
static boolean test_next_pass(
	void)
{
	int slot;
	real width = lane_test.radius + 0.2f;

	if (lane_test.pass + 1 < TEST_PASSES && lane_test.species >= 0)
	{
		lane_test.pass++;
	}
	else
	{
		/* the next species the level has: the last one's soldiers go */
		for (slot = 0; slot < lane_test.count; slot++)
		{
			struct actor_datum *actor = test_actor(lane_test_soldiers[slot].actor);

			if (actor)
				actor_erase(lane_test_soldiers[slot].actor, FALSE);
		}
		do
			lane_test.species++;
		while (lane_test.species < TEST_SPECIES && lane_test.variants[lane_test.species] == NONE);
		if (lane_test.species >= TEST_SPECIES)
			return FALSE;
		lane_test.pass = 0;
		lane_test.count = test_species_count[lane_test.species];
		for (slot = 0; slot < lane_test.count; slot++)
		{
			static real const four[4] = { -0.6f, -0.2f, 0.2f, 0.6f };
			static real const two[2] = { -0.3f, 0.35f };
			struct lane_test_soldier *soldier = &lane_test_soldiers[slot];
			real lateral = width * (lane_test.count == 4 ? four[slot] : two[slot]) + 0.3f * (test_randf() - 0.5f);
			real along = lane_test.line + 3.0f * (test_randf() - 0.5f);
			real x = lane_test.start.x + lane_test.fx * along + lane_test.rx * lateral;
			real y = lane_test.start.y + lane_test.fy * along + lane_test.ry * lateral;

			memset(soldier, 0, sizeof(*soldier));
			soldier->actor = NONE;
			soldier->unit = NONE;
			if (!test_ground(x, y, lane_test.line_z, &soldier->spot))
			{
				soldier->spot.x = x;
				soldier->spot.y = y;
				soldier->spot.z = lane_test.line_z;
			}
		}
	}
	for (slot = 0; slot < lane_test.count; slot++)
	{
		struct lane_test_soldier *soldier = &lane_test_soldiers[slot];
		struct actor_datum *actor = test_actor(soldier->actor);
		struct unit_datum *unit = actor ? test_unit(actor->meta.unit_index) : NULL;

		soldier->fresh = FALSE;
		if (!actor || !unit || unit->object.body_vitality <= 0.0f)
		{
			if (actor)
				actor_erase(soldier->actor, FALSE);
			soldier->actor = NONE;
			if (!test_place_soldier(slot))
				test_log("place_failed,%s,%d", test_species_names[lane_test.species], slot);
		}
		else
		{
			/* back to his spot, facing the Chief */
			real_vector3d forward, up;
			real_point3d at = soldier->spot;

			forward.i = -lane_test.fx;
			forward.j = -lane_test.fy;
			forward.k = 0.0f;
			up.i = 0.0f;
			up.j = 0.0f;
			up.k = 1.0f;
			at.z += 0.1f;
			object_set_position(actor->meta.unit_index, &at, &forward, &up);
			unit->object.translational_velocity.i = unit->object.translational_velocity.j = 0.0f;
		}
		soldier->along = test_along(&soldier->spot);
		soldier->lane_checked = soldier->one_checked = soldier->arrived = FALSE;
		soldier->in_lane = FALSE;
		soldier->lat3 = soldier->lat1 = -1.0f;
		soldier->hit = soldier->killed = FALSE;
		soldier->arrival_tick = soldier->move_tick = soldier->dive_tick = soldier->seen_tick = soldier->cue_tick = 0;
		soldier->reaction = -1;
		soldier->reaction_time = soldier->lead = soldier->memory = -1.0f;
		soldier->last_dive_check = 0;
	}
	test_vehicle_home();
	lane_test.line_speed = 0.0f;
	lane_test.drift = 0.0f;
	lane_test.uprightings = 0;
	lane_test.braking = FALSE;
	test_log("pass_start,%s,%d,%d", test_species_names[lane_test.species], lane_test.pass, lane_test.count);
	return TRUE;
}

/* each tick of a pass: where the vehicle is against each soldier */
static void test_measure(
	long time)
{
	struct unit_datum *vehicle = test_unit(lane_test.vehicle);
	real_point3d at;
	real speed, along, lateral;
	int slot;

	if (!vehicle)
		return;
	object_get_origin(lane_test.vehicle, &at);
	speed = 30.0f * (real)sqrt(vehicle->object.translational_velocity.i * vehicle->object.translational_velocity.i +
		vehicle->object.translational_velocity.j * vehicle->object.translational_velocity.j);
	along = test_along(&at);
	lateral = test_lateral(&at);
	if (time % 30 == 0)
		test_log("drive,%.1f,%.1f,%.2f,%d,%.2f", along, lateral, speed, test_chief_in() ? 1 : 0, vehicle->object.up.k);
	if ((real)fabs(lateral) > lane_test.drift && along < lane_test.line + 5.0f)
		lane_test.drift = (real)fabs(lateral);
	if (!lane_test.line_speed && along >= lane_test.line)
		lane_test.line_speed = speed;
	for (slot = 0; slot < lane_test.count; slot++)
	{
		struct lane_test_soldier *soldier = &lane_test_soldiers[slot];
		struct actor_datum *actor = test_actor(soldier->actor);
		struct mendicant_lane_view view;
		real ahead, his_lateral;

		if (!actor)
			continue;
		ahead = (soldier->along - along) / (speed > 1.0f ? speed : 1.0f);
		his_lateral = (real)fabs(test_lateral(&actor->input.position.body_position) - lateral);
		if (!soldier->lane_checked && ahead <= 3.0f)
		{
			soldier->lane_checked = TRUE;
			soldier->lat3 = his_lateral;
			soldier->in_lane = his_lateral < lane_test.radius + 0.3f;
		}
		if (!soldier->one_checked && ahead <= 1.0f)
		{
			soldier->one_checked = TRUE;
			soldier->lat1 = his_lateral;
		}
		if (!soldier->arrived && ahead <= 0.0f)
		{
			soldier->arrived = TRUE;
			soldier->arrival_tick = time;
		}
		/* when he could first see or hear it */
		if (!soldier->cue_tick && (time % 3) == 0)
		{
			real dx = at.x - actor->input.position.body_position.x, dy = at.y - actor->input.position.body_position.y;
			real_point3d head = actor->input.position.head_position, target = at;

			target.z += 0.6f;
			if (dx * dx + dy * dy < TEST_HEAR * TEST_HEAR)
			{
				soldier->cue_tick = time;
			}
			else
			{
				global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
				if (mendicant_line_blocked(&head, &target) == 0)
					soldier->cue_tick = time;
				global_current_collision_user_depth--;
			}
		}
		/* the game's dive from this vehicle */
		{
			struct actor_debug_info *debug = &actor_debug_array[DATUM_INDEX_TO_ABSOLUTE_INDEX(soldier->actor)];

			if (!soldier->dive_tick && debug->dive_decision == TEST_DIVE_SUCCESS && debug->dive_decision_time > soldier->last_dive_check &&
				debug->dive_decision_time <= time && actor->danger_zone.danger_type == TEST_DANGER_VEHICLE &&
				actor->danger_zone.object_index == lane_test.vehicle)
				soldier->dive_tick = debug->dive_decision_time;
			soldier->last_dive_check = time;
		}
		/* the layer's view */
		if (mendicant_lanes_view(soldier->actor, &view) && view.vehicle == lane_test.vehicle)
		{
			if (!soldier->seen_tick && view.seen_time)
			{
				soldier->seen_tick = view.seen_time;
				soldier->reaction = view.reaction;
				soldier->reaction_time = view.reaction_time;
				soldier->lead = view.lead;
				soldier->memory = view.memory;
			}
			if (!soldier->move_tick && view.move_time)
			{
				soldier->move_tick = view.move_time;
				soldier->reaction = view.reaction;
			}
		}
	}
}

static void test_pass_end(
	void)
{
	int slot;

	/* valid: it reached the line at speed, on its line */
	test_log("pass,%s,%d,%d,%.2f,%.2f,%.2f,%d,%d", test_species_names[lane_test.species], lane_test.pass, lane_test.count,
		lane_test.line_speed, lane_test.radius, lane_test.drift, lane_test.uprightings,
		lane_test.line_speed > 10.0f && lane_test.drift < 1.5f ? 1 : 0);
	for (slot = 0; slot < lane_test.count; slot++)
	{
		struct lane_test_soldier *soldier = &lane_test_soldiers[slot];

		test_log("soldier,%s,%d,%d,%d,%d,%d,%.2f,%.2f,%d,%d,%ld,%ld,%ld,%ld,%ld,%d,%.3f,%.3f,%.2f",
			test_species_names[lane_test.species], lane_test.pass, slot, mendicant_soldier_kind(soldier->actor),
			soldier->fresh ? 1 : 0, soldier->in_lane ? 1 : 0, soldier->lat3, soldier->lat1, soldier->hit ? 1 : 0,
			soldier->killed ? 1 : 0, soldier->arrival_tick, soldier->move_tick, soldier->dive_tick, soldier->seen_tick,
			soldier->cue_tick, soldier->reaction, soldier->reaction_time, soldier->lead, soldier->memory);
	}
}

/* ---------- public code */

void mendicant_lanes_test_reset(
	void)
{
	memset(&lane_test, 0, sizeof(lane_test));
	lane_test.vehicle = NONE;
}

/* a unit run over by a moving vehicle (mendicant_lanes.c) */
void mendicant_lanes_test_hurt(
	long unit_index,
	boolean lethal)
{
	int slot;

	if (!mendicant_lane_test_file || (lane_test.phase != TEST_DRIVE && lane_test.phase != TEST_STOP))
		return;
	for (slot = 0; slot < lane_test.count; slot++)
	{
		struct lane_test_soldier *soldier = &lane_test_soldiers[slot];
		struct actor_datum *actor = test_actor(soldier->actor);

		if (!actor || actor->meta.unit_index != unit_index)
			continue;
		soldier->hit = TRUE;
		if (lethal)
			soldier->killed = TRUE;
	}
}

/* each tick (mendicant_lanes_update) */
void mendicant_lanes_test_update(
	long time)
{
	struct unit_datum *chief;

	if (!mendicant_lane_test_file || !mendicant_active || !actor_data || !actor_data->valid)
		return;
	if (time < lane_test.last_time)
		mendicant_lanes_test_reset(); /* a new map, or the clock went back */
	lane_test.last_time = time;
	chief = test_unit(mendicant_player_unit());
	if (lane_test.phase == TEST_WAIT)
	{
		if (!chief || chief->object.parent_object_index != NONE || cinematic_in_progress() || time < 900)
			return;
		if (!test_setup())
		{
			test_phase(TEST_FAILED, time);
			test_log("phase,failed");
			return;
		}
		test_phase(TEST_PLACE, time);
		return;
	}
	if (lane_test.phase >= TEST_END)
		return;
	/* (a level's script may hold the player's controls: the scene drives him) */
	if (players_globals)
		players_globals->input_disabled = FALSE;
	/* the level's own actors stay away */
	if (time % 30 == 0)
	{
		int index;

		for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
		{
			struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
			long actor_index;

			if (!actor->identifier)
				continue;
			actor_index = ((long)(unsigned short)actor->identifier << 16) | index;
			if (!test_ours(actor_index))
				actor_erase(actor_index, FALSE);
		}
	}
	switch (lane_test.phase)
	{
	case TEST_PLACE:
		if (!test_next_pass())
		{
			test_phase(TEST_END, time);
			test_log("phase,end");
			break;
		}
		test_phase(TEST_SETTLE, time);
		break;
	case TEST_SETTLE:
		if (time - lane_test.phase_time == 15 || time - lane_test.phase_time == 60)
		{
			/* they know the Chief is there (as a level's script tells its
			encounters): the fight is on before he drives at them */
			int slot;

			for (slot = 0; slot < lane_test.count; slot++)
			{
				struct actor_datum *actor = test_actor(lane_test_soldiers[slot].actor);
				long prop_index;

				if (!actor || !test_unit(mendicant_player_unit()))
					continue;
				prop_index = prop_get_base_by_unit_index(lane_test_soldiers[slot].actor, mendicant_player_unit(), TRUE, FALSE);
				if (prop_index != NONE)
					actor_handle_unit_effect(lane_test_soldiers[slot].actor, prop_index, TEST_MAGIC_SIGHT);
			}
		}
		if (time - lane_test.phase_time < TEST_SETTLE_TICKS)
		{
			/* (the Chief kept at the start) */
			if (!test_chief_in() && time - lane_test.phase_time > 30)
				test_vehicle_home();
			break;
		}
		if (!test_chief_in())
		{
			test_log("pass_failed,%s,%d,chief_out", test_species_names[lane_test.species], lane_test.pass);
			test_phase(TEST_PLACE, time);
			break;
		}
		test_phase(TEST_DRIVE, time);
		break;
	case TEST_DRIVE:
		test_measure(time);
		{
			struct unit_datum *vehicle = test_unit(lane_test.vehicle);
			real_point3d at;

			if (!vehicle)
			{
				test_log("phase,failed");
				test_phase(TEST_FAILED, time);
				break;
			}
			object_get_origin(lane_test.vehicle, &at);
			if (lane_test.speed > 0.0f && test_chief_in() && vehicle->object.up.k > 0.8f)
			{
				/* held at the scripted speed, along the line (steered back onto
				it), working up to it in a third of a second) */
				real vx = vehicle->object.translational_velocity.i * 30.0f, vy = vehicle->object.translational_velocity.j * 30.0f;
				real now = (real)sqrt(vx * vx + vy * vy), want = now + 1.5f;
				real lateral = test_lateral(&at), dx, dy, length;

				if (want > lane_test.speed)
					want = lane_test.speed;
				dx = lane_test.fx * 12.0f - lane_test.rx * lateral;
				dy = lane_test.fy * 12.0f - lane_test.ry * lateral;
				length = (real)sqrt(dx * dx + dy * dy);
				vehicle->object.translational_velocity.i = dx / length * want / 30.0f;
				vehicle->object.translational_velocity.j = dy / length * want / 30.0f;
				/* (kept on its wheels and on its heading: no spin from a bump, a
				body or the wheels against the held speed; it turns only to face
				its way) */
				{
					real fx = vehicle->object.forward.i, fy = vehicle->object.forward.j;
					real cross = fx * dy / length - fy * dx / length;

					if (vehicle->object.up.k < 0.9f)
					{
						/* tipping (a lip of ground at speed): set back upright on its way */
						real_vector3d forward, up;

						forward.i = lane_test.fx;
						forward.j = lane_test.fy;
						forward.k = 0.0f;
						up.i = 0.0f;
						up.j = 0.0f;
						up.k = 1.0f;
						object_set_position(lane_test.vehicle, NULL, &forward, &up);
						vehicle->object.angular_velocity.i = 0.0f;
						vehicle->object.angular_velocity.j = 0.0f;
						lane_test.uprightings++;
					}
					vehicle->object.angular_velocity.k = cross * 0.1f;
				}
				/* (held to the ground: off a crest at a held speed it would fly) */
				{
					real_point3d ground;

					if (test_ground(at.x, at.y, at.z, &ground) && at.z - ground.z > 1.0f &&
						vehicle->object.translational_velocity.k > -0.2f)
						vehicle->object.translational_velocity.k = -0.2f;
				}
			}
			if (test_along(&at) > lane_test.line + TEST_BEYOND || time - lane_test.phase_time > TEST_DRIVE_TICKS)
			{
				lane_test.braking = TRUE;
				test_phase(TEST_STOP, time);
			}
		}
		break;
	case TEST_STOP:
		test_measure(time);
		{
			/* slowed by script too, 1 world unit a second each tick, on its wheels */
			struct unit_datum *vehicle = test_unit(lane_test.vehicle);

			if (vehicle && lane_test.speed > 0.0f)
			{
				real vx = vehicle->object.translational_velocity.i, vy = vehicle->object.translational_velocity.j;
				real now = 30.0f * (real)sqrt(vx * vx + vy * vy), scale = now > 1.0f ? (now - 1.0f) / now : 0.0f;

				vehicle->object.translational_velocity.i = vx * scale;
				vehicle->object.translational_velocity.j = vy * scale;
				vehicle->object.angular_velocity.i = 0.0f;
				vehicle->object.angular_velocity.j = 0.0f;
			}
		}
		if (time - lane_test.phase_time > TEST_STOP_TICKS)
		{
			test_pass_end();
			test_phase(TEST_PLACE, time);
		}
		break;
	}
}

/* the scripted Chief's driving (players.c, after the players' actions are
read): TRUE during the scene */
boolean mendicant_lanes_test_actions(
	struct player_action *actions)
{
	struct unit_datum *vehicle;
	real_point3d at;
	real speed, along, lateral, tx, ty;

	if (!mendicant_lane_test_file || !mendicant_active || !actions)
		return FALSE;
	if (lane_test.phase < TEST_PLACE || lane_test.phase >= TEST_END)
		return FALSE;
	actions[0].throttle.i = 0.0f;
	actions[0].throttle.j = 0.0f;
	actions[0].primary_trigger = 0.0f;
	actions[0].desired_facing.yaw = lane_test.yaw;
	actions[0].desired_facing.pitch = 0.0f;
	vehicle = test_unit(lane_test.vehicle);
	if (!vehicle || !test_chief_in())
		return TRUE;
	object_get_origin(lane_test.vehicle, &at);
	speed = 30.0f * (real)sqrt(vehicle->object.translational_velocity.i * vehicle->object.translational_velocity.i +
		vehicle->object.translational_velocity.j * vehicle->object.translational_velocity.j);
	if (lane_test.phase == TEST_STOP)
	{
		/* brake to a stop */
		actions[0].throttle.i = speed > 1.5f ? -1.0f : 0.0f;
		return TRUE;
	}
	if (lane_test.phase != TEST_DRIVE)
		return TRUE;
	/* steer for a point on the line 12 world units ahead; hold 15 world units a second */
	along = test_along(&at);
	lateral = test_lateral(&at);
	tx = lane_test.fx * 12.0f - lane_test.rx * lateral;
	ty = lane_test.fy * 12.0f - lane_test.ry * lateral;
	actions[0].desired_facing.yaw = (real)atan2(ty, tx);
	if (speed < TEST_SPEED - 0.5f)
		actions[0].throttle.i = speed < TEST_SPEED - 4.0f ? 1.0f : (0.45f + 0.15f * (TEST_SPEED - speed) > 1.0f ? 1.0f : 0.45f + 0.15f * (TEST_SPEED - speed));
	else if (speed > TEST_SPEED + 1.0f)
		actions[0].throttle.i = 0.0f;
	else
		actions[0].throttle.i = 0.4f;
	(void)along;
	return TRUE;
}
