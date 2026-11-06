/*
MENDICANT_LANES.C

MENDICANT AI's vehicle lane clearing (the authentic layer's M4;
docs/mendicant_port.md, "Vehicle lanes"): how each soldier with a character
(mendicant_character.c) gets out of the way of a moving vehicle, the Chief's
Warthog and the enemy's alike. Hunters, the Flood, Sentinels, Engineers and
turrets have no character and react exactly as before, as do actors in a
scripted play fight or a command list.

The stock game sees a vehicle only within its radius plus 10 world units,
predicts it 1.5 seconds ahead on a straight line, and dives when it is under
a second away: everyone ignores the Warthog until the last moment, then the
whole group dives on the same frame (or, failing the notice roll, nobody
moves). Here (ai.mendicant.lane_clearing, with the character on):

- Lanes: every other tick, each vehicle that does collision damage and moves
  faster than 4 world units a second, driven or rolling, has its path
  predicted 3 seconds ahead, curved by how fast it is turning. A soldier on
  foot is in its lane when the path passes within its radius plus a world
  unit of him between 0.3 and 3 seconds from now.
- Fairness: he reacts only to a vehicle he can see or hear: the game already
  perceives it as his danger, or it is within 25 world units and faster than
  8 world units a second (its engine), or he sees it (within his kind's
  vision range and cone, nothing between his eyes and it), or he fully
  perceives its driver. The Chief's own side keeps an eye on the Chief: a
  Marine sees the Chief's vehicle wherever a line of sight to it runs within
  60 world units. A vehicle behind him costs a glance first (0.4 s, his head
  turns to it).
- Timing: his own reaction time (his reflex, the kind's base, the fight's
  mood, his own roll) and his own lead, how long before it reaches him he
  goes at the earliest: Marines 2 to 3.5 s (3 to 4 s for the Chief's own
  Warthog, calm), Elites 1.8 to 3 s, Grunts 0.8 to 2 s, Jackals late, at
  1.1 to 1.4 s. Under 0.9 s with no move made, the game's own dive takes
  over: some still dive, never all of them, never in one tick (no third
  soldier starts, a move or the game's dive, within two ticks of two others
  on the same vehicle; mendicant_lanes_danger_react holds a dive that long).
- Each kind's way, at a run, straight across (a turn of 25 degrees only for
  cover), only to ground a man walks to with a straight path there: a
  Marine steps off the road on the side he is on, never across the front of
  a friend's vehicle, toward cover when there is some; a Grunt scatters to his own side, one in five
  the wrong way first, and about one in three freezes for a moment, then
  runs in a panic, screaming; a Grunt near his Elite goes a beat after him;
  a Jackal waits, then one short sidestep, his shield to the vehicle; an
  aggressive Elite (a berserker or duelist more often) facing an enemy
  vehicle under 16 world units a second stands and shoots the driver (the
  matador: the game's dive takes him at the last second), the others step
  aside early. A soldier who gets no further from the road for 0.6 s goes
  straight across instead, then the other side if there is time, else stays
  where he is (never let back into the road while it comes).
- Memory: each pass a soldier lived through, and each friend he saw run over
  (a minute each), make him quicker next time; Grunts learn the most.
- Positions: while a lane threatens him, firing positions clear of it score
  up to 8 more, and a position the lane covers for a second is let go, as the
  game does with its shorter line. A Marine keeps his position out of the
  Chief's own Warthog's lane even before it threatens him.
- The escape is free movement (actor_go_to, urgency 2) that no other owner
  (the drill, following the Chief, the spacing) can take over or end while it
  lasts (mendicant_go_by_lane, mendicant_lane_evading), and the drill does not
  re-order an evading soldier (mendicant_threat_evading); it ends when the
  vehicle has passed or turned away, or after 3.5 s. A grenade's escape goes
  first.

Logged in the fire log (both layers, so the layer off is measured the same
way): lane, lmove, lend, ldive, lhit, lstuck, lfail (docs/mendicant_port.md). The scripted
gate scene is ai.mendicant.lane_test_log (mendicant_lanes_test.c).
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "ai/ai.h"
#include "ai/actors.h"
#include "ai/props.h"
#include "ai/actor_definitions.h"
#include "ai/ai_scenario_definitions.h"
#include "ai/actions.h"
#include "ai/ai_communication.h"
#include "ai/ai_debug.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicles.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define LANE_DANGER_VEHICLE 3       /* actor_danger_zone.danger_type */
#define LANE_DIVE_SUCCESS 4         /* ai_debug.h's _dive_success */
#define LANE_UPDATE_TICKS 2
#define LANE_VEHICLES 12
#define LANE_SAMPLES 13             /* 0 to 3 seconds */
#define LANE_STEP 0.25f             /* seconds between samples */
#define LANE_HORIZON 3.0f
#define LANE_SOONEST 0.3f           /* nearer than this in time: too late for anything but the game's dive */
#define LANE_LATE 0.9f              /* no move made by now: the game's dive */
#define LANE_MIN_SPEED 4.0f         /* world units a second */
#define LANE_RANGE 60.0f
#define LANE_HEAR_RANGE 25.0f
#define LANE_HEAR_SPEED 8.0f
#define LANE_MARGIN 1.0f            /* beyond the vehicle's radius: in its lane */
#define LANE_RADIUS_MINIMUM 0.8f
#define LANE_RADIUS_MAXIMUM 4.0f
#define LANE_TURN_MAXIMUM 2.5f      /* radians a second */
#define LANE_EVADE_TICKS 105        /* the longest escape (it ends sooner when the vehicle has passed) */
#define LANE_COOLDOWN_TICKS 90      /* one early move per vehicle per three seconds */
#define LANE_GLANCE_TICKS 12        /* turning to a vehicle behind him */
#define LANE_MEMORY_TICKS 1800      /* a pass or a friend run over, remembered a minute */
#define LANE_FEAR_RANGE 25.0f
#define LANE_ELITE_LEAD_RANGE 8.0f  /* Grunts this near their Elite go after him */
#define LANE_UNISON_TICKS 2
#define LANE_POSITION_TICKS 30      /* a lane over his firing position this long: let go */
#define LANE_POSITION_BONUS 8.0f
#define LANE_MATADOR_SPEED 16.0f    /* world units a second: an Elite stands to nothing faster */
#define LANE_CHIEF_RANGE 40.0f      /* a Marine keeps his position out of the Chief's lane this near */
#define LANE_DIRECTIONS 5

#define lane_minimum(a, b) ((a) < (b) ? (a) : (b))
#define lane_maximum(a, b) ((a) > (b) ? (a) : (b))
#define lane_clamp(x, lo, hi) lane_minimum(lane_maximum((x), (lo)), (hi))

#define LANE_KIND_GRUNT(kind) ((kind) >= MENDICANT_KIND_GRUNT_MINOR && (kind) <= MENDICANT_KIND_GRUNT_FLEE)
#define LANE_KIND_JACKAL(kind) ((kind) == MENDICANT_KIND_JACKAL_MINOR || (kind) == MENDICANT_KIND_JACKAL_MAJOR)
#define LANE_KIND_ELITE(kind) ((kind) >= MENDICANT_KIND_ELITE_MINOR && (kind) <= MENDICANT_KIND_ELITE_STEALTH)
#define LANE_KIND_MARINE(kind) ((kind) >= MENDICANT_KIND_MARINE && (kind) <= MENDICANT_KIND_SERGEANT)

/* ---------- globals */

boolean mendicant_go_by_lane = FALSE;

/* each moving vehicle's lane */
static struct lane_vehicle
{
	long object;           /* NONE none */
	long driver;           /* its driver's unit, NONE none */
	short team;            /* its driver's team, NONE none */
	boolean player;        /* a player drives it */
	real speed;            /* world units a second */
	real radius;
	real_point3d path[LANE_SAMPLES];
} lane_vehicles[LANE_VEHICLES];
static int lane_vehicle_count = 0;
static long lane_update_time = 0;

/* each soldier's lane state, by absolute actor index */
static struct lane_soldier
{
	long actor;            /* the full index this slot is for, NONE for none */
	/* the episode: a vehicle whose lane he is in, perceived */
	long vehicle;          /* NONE none */
	long seen_time;        /* the episode's start */
	long react_due;        /* he can move from this tick */
	long cue_time;         /* his Elite went: he goes at this tick (0 none) */
	real lead;             /* seconds: he goes once it is this near */
	real reaction_time;    /* seconds, as drawn */
	int reaction;          /* MENDICANT_LANE_* */
	real arrival;          /* seconds until it reaches him, at the last update */
	real offset;           /* his distance from the lane's middle */
	real nearest;          /* the nearest the vehicle came */
	boolean behind;        /* it came from behind him: the glance */
	boolean too_late;      /* left to the game's dive */
	boolean started;
	boolean his_own;       /* the layer moves him (else only logged) */
	long move_time;        /* his early move began, 0 none */
	long dive_time;        /* the game's dive, 0 none */
	long freeze_until;
	long wrong_until;      /* the wrong way first, until then */
	int side;              /* +1 or -1: the side of the lane he goes to */
	boolean panicked;
	boolean replanned;
	int side_switched;
	real_point3d move_from;  /* where his escape began */
	long move_checked;       /* its progress last checked */
	real move_offset;        /* ...and his distance from the vehicle's line then */
	/* his escape */
	boolean evading;
	long evade_until;
	real_point3d goal;
	/* firing positions */
	long position_ticks;
	boolean position_dropped;
	/* one early move per vehicle per three seconds */
	long last_vehicle;
	long last_move_time;
	/* memory */
	real passes;
	long passes_time;
	real fear;
	long fear_time;
} lane_soldiers[MENDICANT_MAX_ACTORS];

/* the last starts per vehicle (no three in two ticks) */
static struct
{
	long vehicle;
	long time[2];
} lane_starts[LANE_VEHICLES];

static long lane_counts[MENDICANT_LANE_REACTIONS];
static int lane_fail_reason[4]; /* (no way off: blocked, no ground, in a lane, no straight way) */

/* ---------- private code */

static boolean lane_layer_on(
	void)
{
	return mendicant_active && mendicant_config.character && mendicant_config.lane_clearing;
}

/* the lanes are worked out: the layer moves soldiers, or a log measures them */
static boolean lane_watching(
	void)
{
	return lane_layer_on() || mendicant_fire_file || mendicant_lane_test_file;
}

static struct actor_datum *lane_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static struct unit_datum *lane_unit(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_unit);
}

static struct lane_soldier *lane_soldier_get(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct lane_soldier *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return NULL;
	soldier = &lane_soldiers[slot];
	if (soldier->actor != actor_index)
	{
		memset(soldier, 0, sizeof(*soldier));
		soldier->actor = actor_index;
		soldier->vehicle = NONE;
		soldier->last_vehicle = NONE;
		soldier->reaction = MENDICANT_LANE_NONE;
	}
	return soldier;
}

static real lane_length2(
	real x,
	real y)
{
	return (real)sqrt(x * x + y * y);
}

static real lane_lerp(
	real a,
	real b,
	real t)
{
	return a + (b - a) * lane_clamp(t, 0.0f, 1.0f);
}

static real lane_rollf(
	long actor_index,
	long vehicle,
	unsigned long salt)
{
	return mendicant_soldier_rollf(actor_index, 0x1a4e0000UL + (salt << 12) + ((unsigned long)vehicle & 0xfffUL));
}

/* a trait's multiplier (mean 1 over his kind), pulled to 1 under command */
static real lane_mult(
	long actor_index,
	int trait,
	real lo,
	real hi)
{
	real value = mendicant_soldier_mult(actor_index, trait, lo, hi);

	return 1.0f + (value - 1.0f) * mendicant_soldier_cohesion(actor_index);
}

/* the layer moves him: a character, the layer on, on foot, not in a script's hands */
static boolean lane_his_own(
	long actor_index,
	struct actor_datum *actor)
{
	if (!lane_layer_on() || !actor || !mendicant_soldier_has(actor_index))
		return FALSE;
	if (actor->external_orders.playfighting || actor->state.action == _actor_action_obey ||
		actor->meta.swarm || actor->input.vehicle_index != NONE)
		return FALSE;
	return mendicant_soldier_kind(actor_index) != MENDICANT_KIND_NONE;
}

/* his kind as the log numbers it: his own, or with no character his species' (a Hunter 20) */
static int lane_log_kind(
	long actor_index,
	struct actor_datum *actor)
{
	int kind = mendicant_soldier_kind(actor_index);

	if (kind != MENDICANT_KIND_NONE || !actor)
		return kind;
	if (actor->meta.type == 3)
		return 20; /* a Hunter */
	return ACTOR_IS_GRUNT(actor->meta.type) ? MENDICANT_KIND_GRUNT_MINOR : ACTOR_IS_JACKAL(actor->meta.type) ? MENDICANT_KIND_JACKAL_MINOR :
		ACTOR_IS_ELITE(actor->meta.type) ? MENDICANT_KIND_ELITE_MINOR : ACTOR_IS_MARINE(actor->meta.type) ? MENDICANT_KIND_MARINE : 0;
}

static boolean lane_commanded(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	return order_actor[slot] == actor_index || (commanded_time[slot] != 0 && mendicant_time() - commanded_time[slot] < 60);
}

/* a record of the fire log (time,record,actor,kind,quirk,commanded,a,b,c,d,e,f) */
static void lane_log(
	char const *record,
	long actor_index,
	struct actor_datum *actor,
	real a,
	real b,
	long c,
	long d,
	long e,
	real f)
{
	if (!mendicant_fire_file)
		return;
	fprintf(mendicant_fire_file, "%ld,%s,%d,%d,%d,%d,%.3f,%.3f,%ld,%ld,%ld,%.3f\n", mendicant_time(), record,
		(int)(short)actor_index, lane_log_kind(actor_index, actor), mendicant_soldier_quirk(actor_index),
		lane_commanded(actor_index) ? 1 : 0, a, b, c, d, e, f);
}

static struct lane_vehicle *lane_vehicle_find(
	long object)
{
	int index;

	if (object == NONE)
		return NULL;
	for (index = 0; index < lane_vehicle_count; index++)
	{
		if (lane_vehicles[index].object == object)
			return &lane_vehicles[index];
	}
	return NULL;
}

/* the vehicle's path for 3 seconds: its velocity turned at its yaw rate */
static void lane_predict(
	struct unit_datum *vehicle,
	struct lane_vehicle *lane)
{
	real vx = vehicle->object.translational_velocity.i * 30.0f;
	real vy = vehicle->object.translational_velocity.j * 30.0f;
	real vz = vehicle->object.translational_velocity.k * 30.0f;
	real turn = lane_clamp(vehicle->object.angular_velocity.k * 30.0f, -LANE_TURN_MAXIMUM, LANE_TURN_MAXIMUM);
	real c = (real)cos(turn * LANE_STEP), s = (real)sin(turn * LANE_STEP);
	real_point3d at;
	int sample;

	object_get_origin(lane->object, &at);
	vz = lane_clamp(vz, -1.0f, 1.0f); /* (a bump's bounce is no climb) */
	lane->path[0] = at;
	for (sample = 1; sample < LANE_SAMPLES; sample++)
	{
		real nx = vx * c - vy * s, ny = vx * s + vy * c;

		at.x += 0.5f * (vx + nx) * LANE_STEP;
		at.y += 0.5f * (vy + ny) * LANE_STEP;
		at.z += vz * LANE_STEP;
		vx = nx;
		vy = ny;
		lane->path[sample] = at;
	}
}

/* the moving vehicles that can run a man over */
static void lane_build(
	void)
{
	struct object_iterator iterator;
	struct unit_datum *vehicle;

	lane_vehicle_count = 0;
	if (!object_header_data || !object_header_data->valid)
		return;
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while ((vehicle = (struct unit_datum *)object_iterator_next(&iterator)) != NULL && lane_vehicle_count < LANE_VEHICLES)
	{
		struct lane_vehicle *lane;
		struct unit_datum *driver;
		real speed;

		speed = 30.0f * lane_length2(vehicle->object.translational_velocity.i, vehicle->object.translational_velocity.j);
		if (speed < LANE_MIN_SPEED || !vehicle_causes_collision_damage(iterator.index))
			continue;
		lane = &lane_vehicles[lane_vehicle_count++];
		lane->object = iterator.index;
		lane->driver = vehicle->unit.driver_object_index;
		driver = lane_unit(lane->driver);
		if (!driver)
			lane->driver = NONE;
		lane->team = driver ? driver->object.owner_team_index : NONE;
		lane->player = driver && driver->unit.player_index != NONE;
		lane->speed = speed;
		lane->radius = lane_clamp(vehicle->object.bounding_sphere_radius, LANE_RADIUS_MINIMUM, LANE_RADIUS_MAXIMUM);
		lane_predict(vehicle, lane);
	}
}

/* the path's nearest pass by a point: the distance, when (seconds), which side
(+1 left of its way, -1 right) and its way there (a unit vector); FALSE when
it comes no nearer than where it is now behind him, or on another level */
static boolean lane_approach(
	struct lane_vehicle const *lane,
	real_point3d const *point,
	real *distance,
	real *time,
	int *side,
	real *way_x,
	real *way_y)
{
	real best = 1e30f, best_time = 0.0f, best_x = 1.0f, best_y = 0.0f, best_cross = 0.0f;
	int sample;

	for (sample = 0; sample + 1 < LANE_SAMPLES; sample++)
	{
		real_point3d const *a = &lane->path[sample], *b = &lane->path[sample + 1];
		real abx = b->x - a->x, aby = b->y - a->y, length2 = abx * abx + aby * aby;
		real px = point->x - a->x, py = point->y - a->y;
		real u = length2 > 1e-6f ? lane_clamp((px * abx + py * aby) / length2, 0.0f, 1.0f) : 0.0f;
		real qx = px - abx * u, qy = py - aby * u, d = lane_length2(qx, qy);
		real dz = point->z - (a->z + (b->z - a->z) * u);

		if (dz > 4.0f || dz < -4.0f)
			continue;
		if (d < best)
		{
			real length = (real)sqrt(length2);

			best = d;
			best_time = (sample + u) * LANE_STEP;
			if (length > 1e-3f)
			{
				best_x = abx / length;
				best_y = aby / length;
			}
			best_cross = best_x * qy - best_y * qx;
		}
	}
	if (best >= 1e29f)
		return FALSE;
	*distance = best;
	*time = best_time;
	*side = best_cross >= 0.0f ? 1 : -1;
	*way_x = best_x;
	*way_y = best_y;
	return TRUE;
}

/* the point is clear of every lane (of the episode's vehicle by its radius
plus its margin plus extra) */
static boolean lane_point_clear(
	real_point3d const *point,
	long vehicle,
	real extra)
{
	int index;

	for (index = 0; index < lane_vehicle_count; index++)
	{
		struct lane_vehicle const *lane = &lane_vehicles[index];
		real distance, time, wx, wy;
		int side;

		if (!lane_approach(lane, point, &distance, &time, &side, &wx, &wy))
			continue;
		if (time > LANE_HORIZON - 0.01f && lane->object != vehicle)
			continue;
		if (distance < lane->radius + LANE_MARGIN + (lane->object == vehicle ? extra : 0.0f))
			return FALSE;
	}
	return TRUE;
}

/* he can see or hear it: the game's danger zone perceives it, or it is near
and fast (its engine), or he sees it (in his vision's range and cone, his eyes
on it; a friend of the Chief keeps an eye on the Chief's own vehicle wherever
it is in sight), or he fully perceives its driver */
static boolean lane_perceived(
	long actor_index,
	struct actor_datum *actor,
	struct lane_vehicle const *lane)
{
	struct unit_datum *vehicle = lane_unit(lane->object);
	real dx, dy;

	if (!vehicle)
		return FALSE;
	if (actor->danger_zone.danger_type == LANE_DANGER_VEHICLE && actor->danger_zone.object_index == lane->object &&
		actor->danger_zone.currently_perceived)
		return TRUE;
	dx = vehicle->object.position.x - actor->input.position.body_position.x;
	dy = vehicle->object.position.y - actor->input.position.body_position.y;
	if (lane->speed > LANE_HEAR_SPEED && dx * dx + dy * dy < LANE_HEAR_RANGE * LANE_HEAR_RANGE)
		return TRUE;
	/* he sees it: within his vision's range and cone, nothing between his eyes and it */
	{
		struct actor_definition *definition = actor_definition_get(actor->meta.definition_index);
		real range = definition ? definition->perception.maximum_vision_distance : 0.0f;
		real half = definition ? 0.5f * definition->perception.maximum_vision_angle : 0.0f;
		real distance = lane_length2(dx, dy);

		boolean watched = lane->player && lane->team != NONE && !game_team_is_enemy(lane->team, actor->meta.team_index);

		half = lane_clamp(half, 0.785f, 1.571f);
		/* (his own side keeps an eye on the Chief: any way he faces, as far as a line of sight runs) */
		if (watched)
		{
			range = LANE_RANGE;
			half = 3.15f;
		}
		if (range > 0.0f && distance < range && distance > 0.01f &&
			(watched || (dx * actor->input.facing_vector.i + dy * actor->input.facing_vector.j) / distance >= (real)cos(half)))
		{
			real_point3d eye = actor->input.position.head_position, at = vehicle->object.position;
			int blocked;

			at.z += 0.5f;
			global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
			blocked = mendicant_line_blocked(&eye, &at);
			global_current_collision_user_depth--;
			if (blocked == 0)
				return TRUE;
		}
	}
	if (lane->driver != NONE && lane_unit(lane->driver) && prop_data && prop_data->valid)
	{
		long prop_index = prop_get_active_by_unit_index(actor_index, lane->driver);

		if (prop_index != NONE)
		{
			struct prop_datum *prop = (struct prop_datum *)datum_try_and_get(prop_data, prop_index);

			if (prop && prop->perception >= _actor_perception_full)
				return TRUE;
		}
	}
	return FALSE;
}

/* what he remembers: passes lived through and friends run over, fading over a minute */
static real lane_memory(
	struct lane_soldier const *soldier,
	long time)
{
	real memory = 0.0f;

	if (soldier->passes > 0.0f && time - soldier->passes_time < LANE_MEMORY_TICKS)
		memory += soldier->passes * (1.0f - (real)(time - soldier->passes_time) / LANE_MEMORY_TICKS);
	if (soldier->fear > 0.0f && time - soldier->fear_time < LANE_MEMORY_TICKS)
		memory += 1.5f * soldier->fear * (1.0f - (real)(time - soldier->fear_time) / LANE_MEMORY_TICKS);
	return lane_clamp(memory, 0.0f, 3.0f);
}

/* the episode's draws: reaction time, lead and reaction */
static void lane_episode_draw(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	struct lane_vehicle const *lane,
	long time)
{
	int kind = mendicant_soldier_kind(actor_index), quirk = mendicant_soldier_quirk(actor_index);
	real reflex = lane_mult(actor_index, MENDICANT_TRAIT_REFLEX, 1.45f, 0.55f);
	real caution = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_CAUTION);
	real courage = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_COURAGE);
	real aggression = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_AGGRESSION);
	real memory = lane_memory(soldier, time);
	real base, lead_low, lead_high, wait, lead, u;
	boolean friendly = lane->team != NONE && !game_team_is_enemy(lane->team, actor->meta.team_index);
	long vehicle = lane->object;

	if (LANE_KIND_GRUNT(kind))
	{
		base = 0.5f;
		lead_low = 0.8f;
		lead_high = 2.0f;
	}
	else if (LANE_KIND_JACKAL(kind))
	{
		base = 0.4f;
		lead_low = 1.1f;
		lead_high = 1.4f;
	}
	else if (LANE_KIND_ELITE(kind))
	{
		base = 0.25f;
		lead_low = 1.8f;
		lead_high = 3.0f;
	}
	else
	{
		base = 0.35f;
		lead_low = friendly ? 3.0f : 2.0f;
		lead_high = friendly ? 4.0f : 3.5f;
		if (friendly)
			base *= 0.8f; /* the Chief's own: no surprise in it */
	}
	/* the reaction time: his reflex, the mood, his own roll; what he remembers */
	u = 2.0f * lane_rollf(actor_index, vehicle, 1) - 1.0f;
	wait = base * reflex * mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_REACTION) * (1.0f + 0.6f * u);
	if (quirk == MENDICANT_QUIRK_ROOKIE)
		wait += 0.2f;
	else if (quirk == MENDICANT_QUIRK_VETERAN)
		wait -= 0.1f;
	wait -= 0.15f * memory;
	wait = lane_clamp(wait, 0.08f, 1.5f);
	if (soldier->behind)
		wait += (real)LANE_GLANCE_TICKS / 30.0f;
	soldier->reaction_time = wait;
	soldier->react_due = time + (long)(wait * 30.0f + 0.5f);
	/* the lead: his caution and his own roll; a Grunt learns the most */
	lead = lane_lerp(lead_low, lead_high, 0.5f * caution + 0.5f * lane_rollf(actor_index, vehicle, 2));
	if (LANE_KIND_GRUNT(kind))
		lead += 0.45f * memory;
	else if (!LANE_KIND_JACKAL(kind))
		lead += 0.15f * memory;
	soldier->lead = lead;
	soldier->cue_time = 0;
	/* the reaction */
	soldier->reaction = MENDICANT_LANE_SIDESTEP;
	if (LANE_KIND_GRUNT(kind))
	{
		real freeze = 0.33f * mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_COURAGE, 1.7f, 0.3f);

		if (quirk == MENDICANT_QUIRK_COWARD || kind == MENDICANT_KIND_GRUNT_FLEE)
			freeze = 0.9f;
		else if (quirk == MENDICANT_QUIRK_BRAVE)
			freeze = 0.0f;
		freeze *= memory >= 1.0f ? 0.5f : 1.0f; /* he has seen it before */
		if (lane_rollf(actor_index, vehicle, 3) < freeze)
			soldier->reaction = MENDICANT_LANE_FREEZE;
		else if (lane_rollf(actor_index, vehicle, 4) < 0.2f)
			soldier->reaction = MENDICANT_LANE_WRONG_WAY;
		(void)courage;
	}
	else if (LANE_KIND_JACKAL(kind))
	{
		soldier->reaction = MENDICANT_LANE_SHIELD_STEP;
	}
	else if (LANE_KIND_ELITE(kind))
	{
		/* the odd one: an aggressive Elite, a berserker or a duelist more often */
		real chance = lane_clamp((aggression - 0.6f) * 1.2f, 0.0f, 0.35f);

		if (quirk == MENDICANT_QUIRK_BERSERKER || quirk == MENDICANT_QUIRK_DUELIST)
			chance = lane_maximum(chance, 0.4f);
		if (!friendly && lane->team != NONE && lane->speed < LANE_MATADOR_SPEED && memory < 1.5f &&
			lane_rollf(actor_index, vehicle, 5) < chance)
			soldier->reaction = MENDICANT_LANE_MATADOR;
	}
}

/* free movement for his escape: no other owner can take it while it lasts */
static boolean lane_go(
	long actor_index,
	struct lane_soldier *soldier,
	real_point3d const *point,
	real tolerance)
{
	struct actor_datum *actor = lane_actor(actor_index);
	int result;

	if (!actor)
		return FALSE;
	if (mendicant_grenade_evading(actor_index))
		return FALSE; /* a grenade's escape goes first */
	mendicant_go_by_lane = TRUE;
	result = actor_go_to(actor_index, point, tolerance, 2);
	mendicant_go_by_lane = FALSE;
	if (result != 1)
		return FALSE;
	soldier->evading = TRUE;
	soldier->goal = *point;
	soldier->evade_until = mendicant_time() + LANE_EVADE_TICKS;
	soldier->move_from = actor->input.position.body_position;
	soldier->move_checked = mendicant_time();
	soldier->move_offset = soldier->offset;
	return TRUE;
}

static void lane_go_end(
	long actor_index,
	struct lane_soldier *soldier)
{
	real_point3d goal;

	if (!soldier->evading)
		return;
	soldier->evading = FALSE;
	soldier->panicked = FALSE;
	/* (only his own order: a grenade's escape may have taken over) */
	if (actor_go_goal(actor_index, &goal) && lane_length2(goal.x - soldier->goal.x, goal.y - soldier->goal.y) < 1.5f)
	{
		mendicant_go_by_lane = TRUE;
		actor_go_cancel(actor_index);
		mendicant_go_by_lane = FALSE;
	}
}

/* the ground under a point, no steeper from the ground under him than a man
walks (no ledge, no pit); the point set on it */
static boolean lane_ground_fits(
	struct actor_datum *actor,
	real_point3d *point)
{
	real_point3d ground, under, his = actor->input.position.body_position;
	real run = lane_length2(point->x - his.x, point->y - his.y);

	his.z += 0.5f;
	if (!mendicant_ground_under(point, &ground))
		return FALSE;
	if (!mendicant_ground_under(&his, &under))
		under.z = actor->input.position.body_position.z;
	if (ground.z - under.z > 0.5f + 0.35f * run || under.z - ground.z > 0.6f + 0.45f * run)
		return FALSE;
	point->z = ground.z + 0.3f;
	return TRUE;
}

/* his way off the road: to side (+1 left of the vehicle's way), clear of the
lane by clear world units, the straight way across first, then turned 25 or
45 degrees away from the vehicle or toward it; a Marine (prefer_cover) takes
the one with cover from the vehicle. FALSE: none fits. */
static boolean lane_way_off(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	struct lane_vehicle const *lane,
	int side,
	real clear,
	boolean prefer_cover,
	real_point3d *end)
{
	static real const turns[LANE_DIRECTIONS][2] = { { 1.0f, 0.0f }, { 0.906f, -0.423f }, { 0.906f, 0.423f },
		{ 0.707f, -0.707f }, { 0.707f, 0.707f } };
	real_point3d const *at = &actor->input.position.body_position;
	real distance, time, wx, wy, nx, ny, need, best_score = -1e30f;
	int lane_side, turn, found = 0;
	struct unit_datum *vehicle = lane_unit(lane->object);

	if (!lane_approach(lane, at, &distance, &time, &lane_side, &wx, &wy))
		return FALSE;
	/* across the vehicle's way, to the side asked */
	nx = -wy * (real)side;
	ny = wx * (real)side;
	need = (lane->radius + LANE_MARGIN + clear) - distance * (side == lane_side ? 1.0f : -1.0f);
	need = lane_clamp(need, 1.2f, 9.0f);
	for (turn = 0; turn < LANE_DIRECTIONS; turn++)
	{
		/* turned along the vehicle's way (with it) or against it */
		real c = turns[turn][0], s = turns[turn][1];
		real x = nx * c + wx * s, y = ny * c + wy * s;
		real length = need / lane_maximum(c, 0.5f), score;
		real_point3d from = *at, to;
		int blocked;

		to.x = at->x + x * length;
		to.y = at->y + y * length;
		to.z = at->z + 0.5f;
		from.z += 0.5f;
		blocked = mendicant_line_blocked(&from, &to);
		if (blocked == 1)
		{
			lane_fail_reason[0]++;
			continue;
		}
		to.z = at->z + 0.5f;
		if (!lane_ground_fits(actor, &to))
		{
			lane_fail_reason[1]++;
			continue;
		}
		if (!lane_point_clear(&to, lane->object, 0.3f))
		{
			lane_fail_reason[2]++;
			continue;
		}
		/* a straight way there on foot (not round by the road) */
		if (!mendicant_go_direct(actor_index, &to))
		{
			lane_fail_reason[3]++;
			continue;
		}
		/* straight across is best (soonest out of it); a turn only for cover */
		score = -(turn >= 3 ? 1.6f : turn >= 1 ? 0.45f : 0.0f) + 0.3f * (lane_rollf(actor_index, lane->object, 20 + turn) - 0.5f);
		if (prefer_cover && vehicle && blocked == 0)
		{
			real_point3d threat = vehicle->object.position;

			threat.z += 0.6f;
			if (mendicant_cover_score(&to, &threat) > 0)
				score += 0.6f;
		}
		if (!found || score > best_score)
		{
			best_score = score;
			*end = to;
			found = 1;
		}
		if (!prefer_cover && found)
			break;
	}
	(void)soldier;
	return found != 0;
}

/* the side he goes to: the side of the lane he stands on, or for a man in the
middle of it his own */
static int lane_near_side(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	int lane_side)
{
	if (soldier->offset < 0.25f)
	{
		/* in the middle: his peek side, by his facing against the vehicle's way */
		int peek = mendicant_soldier_peek_side(actor_index) == MENDICANT_SIDE_RIGHT ? -1 : 1;

		(void)actor;
		return lane_rollf(actor_index, soldier->vehicle, 6) < 0.7f ? peek : -peek;
	}
	return lane_side;
}

static void lane_count_start(
	long vehicle,
	long time)
{
	int index, free_index = -1;

	for (index = 0; index < LANE_VEHICLES; index++)
	{
		if (lane_starts[index].vehicle == vehicle)
		{
			lane_starts[index].time[1] = lane_starts[index].time[0];
			lane_starts[index].time[0] = time;
			return;
		}
		if (free_index < 0 && (lane_starts[index].vehicle == NONE || !lane_vehicle_find(lane_starts[index].vehicle)))
			free_index = index;
	}
	if (free_index < 0)
		free_index = 0;
	lane_starts[free_index].vehicle = vehicle;
	lane_starts[free_index].time[0] = time;
	lane_starts[free_index].time[1] = -1000;
}

/* two others on this vehicle started within the last two ticks */
static boolean lane_unison(
	long vehicle,
	long time)
{
	int index;

	for (index = 0; index < LANE_VEHICLES; index++)
	{
		if (lane_starts[index].vehicle == vehicle)
			return time - lane_starts[index].time[1] <= LANE_UNISON_TICKS;
	}
	return FALSE;
}

/* the clear distance off the lane by kind, his own */
static real lane_clear_distance(
	long actor_index,
	struct lane_soldier *soldier,
	int kind)
{
	real roll = lane_rollf(actor_index, soldier->vehicle, 7);

	if (soldier->reaction == MENDICANT_LANE_FREEZE)
		return 3.0f + 3.0f * roll; /* the panic run */
	if (LANE_KIND_JACKAL(kind))
		return 0.5f + 0.8f * roll;  /* one short step */
	if (LANE_KIND_GRUNT(kind))
		return 1.5f + 2.5f * roll;
	if (LANE_KIND_ELITE(kind))
		return 1.5f + 2.0f * roll;
	return 2.0f + 2.0f * roll;
}

/* his move off the road begins (or the freeze before it); FALSE: no way off */
static boolean lane_begin(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	struct lane_vehicle const *lane,
	int lane_side,
	long time)
{
	int kind = mendicant_soldier_kind(actor_index);
	real_point3d end;
	int side = lane_near_side(actor_index, actor, soldier, lane_side);
	/* across its way only with time to spare, never in front of a friend's bumper */
	boolean far_allowed = soldier->arrival > 2.0f && !(lane->team != NONE && !game_team_is_enemy(lane->team, actor->meta.team_index));

	soldier->side = side;
	if (soldier->reaction == MENDICANT_LANE_FREEZE)
	{
		/* stopped dead, a moment, then the panic run */
		real_point3d here = actor->input.position.body_position;

		soldier->freeze_until = time + 9 + (long)(15.0f * lane_rollf(actor_index, lane->object, 8));
		if (!lane_go(actor_index, soldier, &here, 0.5f))
			return FALSE;
		return TRUE;
	}
	if (soldier->reaction == MENDICANT_LANE_WRONG_WAY && soldier->arrival > 1.5f)
	{
		/* a step the wrong way first, then his own side */
		real distance, t, wx, wy;
		int s;

		if (lane_approach(lane, &actor->input.position.body_position, &distance, &t, &s, &wx, &wy))
		{
			real_point3d to = actor->input.position.body_position;

			to.x += -wy * (real)(-side) * 1.2f;
			to.y += wx * (real)(-side) * 1.2f;
			to.z += 0.5f;
			if (lane_ground_fits(actor, &to) && lane_go(actor_index, soldier, &to, 0.6f))
			{
				soldier->wrong_until = time + 8 + (long)(4.0f * lane_rollf(actor_index, lane->object, 9));
				return TRUE;
			}
		}
		soldier->reaction = MENDICANT_LANE_SIDESTEP;
	}
	if (!lane_way_off(actor_index, actor, soldier, lane, side, lane_clear_distance(actor_index, soldier, kind),
		LANE_KIND_MARINE(kind) || LANE_KIND_ELITE(kind), &end))
	{
		if (!far_allowed || !lane_way_off(actor_index, actor, soldier, lane, -side,
			lane_clear_distance(actor_index, soldier, kind), FALSE, &end))
			return FALSE;
		soldier->side = -side;
	}
	return lane_go(actor_index, soldier, &end, 1.0f);
}

/* the freeze or the wrong step is over: the run off the road */
static boolean lane_follow_on(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	struct lane_vehicle const *lane)
{
	int kind = mendicant_soldier_kind(actor_index);
	real_point3d end;
	boolean panic = soldier->reaction == MENDICANT_LANE_FREEZE;

	if (!lane_way_off(actor_index, actor, soldier, lane, soldier->side, lane_clear_distance(actor_index, soldier, kind),
		FALSE, &end) &&
		!lane_way_off(actor_index, actor, soldier, lane, -soldier->side, lane_clear_distance(actor_index, soldier, kind),
		FALSE, &end))
		return FALSE;
	if (!lane_go(actor_index, soldier, &end, 1.0f)) /* (replaces his own order) */
		return FALSE;
	if (panic)
	{
		soldier->panicked = TRUE;
		ai_communication_event(_ai_communication_flee, actor->meta.unit_index, NONE, NONE, NONE, NONE, NULL);
		if (mendicant_soldier_quirk(actor_index) == MENDICANT_QUIRK_COWARD && mendicant_soldier_signature_due(actor_index))
			mendicant_soldier_signature_spent(actor_index);
	}
	return TRUE;
}

static void lane_episode_end(
	long actor_index,
	struct actor_datum *actor,
	struct lane_soldier *soldier,
	long time,
	int outcome)
{
	if (soldier->vehicle == NONE)
		return;
	lane_log("lend", actor_index, actor, soldier->nearest, soldier->offset, (long)(short)soldier->vehicle, soldier->reaction,
		outcome, soldier->move_time ? (real)(soldier->move_time - soldier->seen_time) / 30.0f : -1.0f);
	/* he lived through it: quicker next time */
	if (outcome != MENDICANT_LANE_OUTCOME_HIT && soldier->nearest < 6.0f)
	{
		soldier->passes = (soldier->passes > 0.0f && time - soldier->passes_time < LANE_MEMORY_TICKS ?
			soldier->passes * (1.0f - (real)(time - soldier->passes_time) / LANE_MEMORY_TICKS) : 0.0f) + 1.0f;
		soldier->passes_time = time;
	}
	if (soldier->move_time)
	{
		soldier->last_vehicle = soldier->vehicle;
		soldier->last_move_time = soldier->move_time;
	}
	lane_go_end(actor_index, soldier);
	soldier->vehicle = NONE;
	soldier->started = FALSE;
	soldier->too_late = FALSE;
	soldier->reaction = MENDICANT_LANE_NONE;
	soldier->freeze_until = soldier->wrong_until = 0;
	soldier->replanned = FALSE;
	soldier->side_switched = 0;
	soldier->position_ticks = 0;
	soldier->position_dropped = FALSE;
	soldier->cue_time = 0;
}

/* his Elite went: Grunts near him, in the same lane, go a beat after him */
static void lane_cue_grunts(
	long elite_index,
	struct actor_datum *elite,
	long vehicle,
	long time)
{
	int index;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct lane_soldier *other = &lane_soldiers[index];
		struct actor_datum *grunt;
		real dx, dy;

		if (other->actor == NONE || other->vehicle != vehicle || other->started || other->cue_time ||
			!other->his_own)
			continue;
		grunt = lane_actor(other->actor);
		if (!grunt || !ACTOR_IS_GRUNT(grunt->meta.type) || grunt->meta.team_index != elite->meta.team_index)
			continue;
		dx = grunt->input.position.body_position.x - elite->input.position.body_position.x;
		dy = grunt->input.position.body_position.y - elite->input.position.body_position.y;
		if (dx * dx + dy * dy > LANE_ELITE_LEAD_RANGE * LANE_ELITE_LEAD_RANGE)
			continue;
		other->cue_time = time + 6 + (long)(9.0f * lane_rollf(other->actor, vehicle, 10));
	}
	(void)elite_index;
}

/* one soldier's update: his lane, his episode, his escape */
static void lane_soldier_update(
	long actor_index,
	struct actor_datum *actor,
	long time)
{
	struct lane_soldier *soldier = lane_soldier_get(actor_index);
	struct lane_vehicle const *lane = NULL, *best = NULL;
	real distance = 0.0f, arrival = 0.0f, best_arrival = 1e30f, wx, wy;
	int lane_side = 1, index;
	boolean own;

	if (!soldier)
		return;
	own = lane_his_own(actor_index, actor);
	/* the episode's vehicle */
	if (soldier->vehicle != NONE)
	{
		lane = lane_vehicle_find(soldier->vehicle);
		if (!lane || !lane_approach(lane, &actor->input.position.body_position, &distance, &arrival, &lane_side, &wx, &wy) ||
			arrival < 0.05f)
		{
			/* passed, stopped, or gone */
			struct unit_datum *vehicle = lane_unit(soldier->vehicle);
			real near = vehicle ? lane_length2(vehicle->object.position.x - actor->input.position.body_position.x,
				vehicle->object.position.y - actor->input.position.body_position.y) : 99.0f;

			if (near < soldier->nearest)
				soldier->nearest = near;
			lane_episode_end(actor_index, actor, soldier, time,
				soldier->move_time ? MENDICANT_LANE_OUTCOME_MOVED : soldier->dive_time ? MENDICANT_LANE_OUTCOME_DIVED :
				MENDICANT_LANE_OUTCOME_STOOD);
			lane = NULL;
		}
		else
		{
			soldier->arrival = arrival;
			soldier->offset = distance;
			if (distance < soldier->nearest)
				soldier->nearest = distance;
		}
	}
	/* a new episode: the most urgent lane he is in, perceived */
	if (soldier->vehicle == NONE && actor->input.vehicle_index == NONE && actor->input.body_vitality > 0.0f)
	{
		for (index = 0; index < lane_vehicle_count; index++)
		{
			struct lane_vehicle const *candidate = &lane_vehicles[index];
			struct unit_datum *vehicle = lane_unit(candidate->object);
			real d, t;
			int s;

			if (!vehicle || actor->meta.unit_index == candidate->driver)
				continue;
			if (lane_length2(vehicle->object.position.x - actor->input.position.body_position.x,
				vehicle->object.position.y - actor->input.position.body_position.y) > LANE_RANGE)
				continue;
			/* (walking to a seat of it) */
			if (actor->state.action == _actor_action_vehicle && actor->state.action_data.vehicle.vehicle_index == candidate->object)
				continue;
			if (!lane_approach(candidate, &actor->input.position.body_position, &d, &t, &s, &wx, &wy))
				continue;
			if (d >= candidate->radius + LANE_MARGIN || t < LANE_SOONEST || t > LANE_HORIZON - 0.01f)
				continue;
			if (t >= best_arrival)
				continue;
			if (soldier->last_vehicle == candidate->object && time - soldier->last_move_time < LANE_COOLDOWN_TICKS)
				continue;
			if (!lane_perceived(actor_index, actor, candidate))
				continue;
			best = candidate;
			best_arrival = t;
			distance = d;
			lane_side = s;
		}
		if (best)
		{
			struct unit_datum *vehicle = lane_unit(best->object);
			real dx = vehicle->object.position.x - actor->input.position.body_position.x;
			real dy = vehicle->object.position.y - actor->input.position.body_position.y;
			real length = lane_length2(dx, dy);

			lane = best;
			soldier->vehicle = best->object;
			soldier->seen_time = time;
			soldier->arrival = best_arrival;
			soldier->offset = distance;
			soldier->nearest = distance;
			soldier->move_time = 0;
			soldier->dive_time = 0;
			soldier->started = FALSE;
			soldier->too_late = FALSE;
			soldier->his_own = own;
			soldier->behind = length > 0.1f &&
				(dx * actor->input.facing_vector.i + dy * actor->input.facing_vector.j) / length < -0.3f;
			soldier->reaction = MENDICANT_LANE_NONE;
			if (own)
				lane_episode_draw(actor_index, actor, soldier, best, time);
			lane_log("lane", actor_index, actor, best_arrival, distance, (long)(short)best->object, soldier->reaction,
				best->player ? 1 : 0, own ? soldier->reaction_time : -1.0f);
		}
	}
	if (soldier->vehicle == NONE || !lane)
	{
		if (soldier->evading)
			lane_go_end(actor_index, soldier);
		return;
	}
	soldier->his_own = own;
	if (!own)
	{
		if (soldier->evading)
			lane_go_end(actor_index, soldier);
		return;
	}
	/* his escape under way */
	if (soldier->started)
	{
		real_point3d goal;
		int status;

		if (!soldier->evading)
			return;
		/* another owner (a grenade) took his order */
		if (!actor_go_goal(actor_index, &goal) || lane_length2(goal.x - soldier->goal.x, goal.y - soldier->goal.y) > 1.5f)
		{
			soldier->evading = FALSE;
			soldier->panicked = FALSE;
			return;
		}
		if (soldier->freeze_until && time >= soldier->freeze_until)
		{
			soldier->freeze_until = 0;
			if (!lane_follow_on(actor_index, actor, soldier, lane))
				lane_go_end(actor_index, soldier);
			return;
		}
		if (soldier->wrong_until && time >= soldier->wrong_until)
		{
			soldier->wrong_until = 0;
			soldier->reaction = MENDICANT_LANE_WRONG_WAY;
			if (!lane_follow_on(actor_index, actor, soldier, lane))
				lane_go_end(actor_index, soldier);
			return;
		}
		if (soldier->freeze_until || soldier->wrong_until)
			return;
		status = actor_go_status(actor_index);
		if (time >= soldier->evade_until)
		{
			lane_go_end(actor_index, soldier);
			return;
		}
		/* every 0.6 s: no further from the vehicle's line (his way
		was no way, or his path turns back into the road): straight across
		instead, then the other side if there is time; else he stays where
		he is (the game's dive is left to him), never let back into the road */
		if (time - soldier->move_checked >= 18 && distance < lane->radius + LANE_MARGIN && soldier->arrival > 0.6f &&
			status != GO_STATUS_ARRIVED)
		{
			boolean stalled = distance < soldier->move_offset + 0.1f;

			soldier->move_checked = time;
			soldier->move_offset = distance;
			if (stalled)
			{
				real_point3d end;
				int kind = mendicant_soldier_kind(actor_index);
				real clear = lane_clear_distance(actor_index, soldier, kind);
				boolean friendly = lane->team != NONE && !game_team_is_enemy(lane->team, actor->meta.team_index);

				lane_log("lstuck", actor_index, actor, soldier->arrival, distance, (long)(short)lane->object, soldier->reaction,
					soldier->replanned ? 1 : 0, 0.0f);
				if (!soldier->replanned)
				{
					soldier->replanned = TRUE;
					if (lane_way_off(actor_index, actor, soldier, lane, soldier->side, clear, FALSE, &end) &&
						lane_length2(end.x - soldier->goal.x, end.y - soldier->goal.y) > 0.5f && lane_go(actor_index, soldier, &end, 1.0f))
						return;
				}
				if (soldier->side_switched < 1 && soldier->arrival > (friendly ? 1.5f : 1.2f) &&
					lane_way_off(actor_index, actor, soldier, lane, -soldier->side, clear, FALSE, &end) && lane_go(actor_index, soldier, &end, 1.0f))
				{
					soldier->side = -soldier->side;
					soldier->side_switched++;
					return;
				}
				/* (no better way: his order holds him where he is, out of
				the drill's and the follow's hands, till it has passed) */
			}
			return;
		}
		/* there and still in it (it turned his way): once more, if there is time */
		if (status == GO_STATUS_ARRIVED && distance < lane->radius + LANE_MARGIN)
		{
			if (!soldier->replanned && soldier->arrival > LANE_LATE)
			{
				soldier->replanned = TRUE;
				if (!lane_begin(actor_index, actor, soldier, lane, lane_side, time))
					lane_go_end(actor_index, soldier);
			}
			return;
		}
		if (status != GO_STATUS_MOVING && status != GO_STATUS_YIELDING && status != GO_STATUS_ARRIVED)
			lane_go_end(actor_index, soldier);
		return;
	}
	if (soldier->too_late || soldier->reaction == MENDICANT_LANE_MATADOR)
		return;
	if (distance >= lane->radius + LANE_MARGIN)
		return; /* (out of it now: it turned) */
	if (soldier->arrival < LANE_LATE)
	{
		soldier->too_late = TRUE; /* the game's own dive from here */
		return;
	}
	if (time < soldier->react_due)
		return;
	if (soldier->arrival > soldier->lead && !(soldier->cue_time && time >= soldier->cue_time))
		return;
	/* never three at once on one vehicle */
	if (lane_unison(lane->object, time) && soldier->arrival > LANE_LATE + 0.15f)
		return;
	memset(lane_fail_reason, 0, sizeof(lane_fail_reason));
	if (!lane_begin(actor_index, actor, soldier, lane, lane_side, time))
	{
		soldier->too_late = TRUE; /* no way off: the game's own */
		lane_log("lfail", actor_index, actor, soldier->arrival, distance, lane_fail_reason[0], lane_fail_reason[1],
			lane_fail_reason[2], (real)lane_fail_reason[3]);
		return;
	}
	soldier->started = TRUE;
	soldier->move_time = time;
	lane_count_start(lane->object, time);
	if (soldier->reaction >= 0 && soldier->reaction < MENDICANT_LANE_REACTIONS)
		lane_counts[soldier->reaction]++;
	lane_log("lmove", actor_index, actor, soldier->arrival, distance, (long)(short)lane->object, soldier->reaction,
		time - soldier->seen_time, soldier->lead);
	if (ACTOR_IS_ELITE(actor->meta.type))
		lane_cue_grunts(actor_index, actor, lane->object, time);
}

/* ---------- public code */

void mendicant_lanes_reset(
	void)
{
	int index;

	memset(lane_soldiers, 0, sizeof(lane_soldiers));
	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
	{
		lane_soldiers[index].actor = NONE;
		lane_soldiers[index].vehicle = NONE;
		lane_soldiers[index].last_vehicle = NONE;
	}
	for (index = 0; index < LANE_VEHICLES; index++)
	{
		lane_vehicles[index].object = NONE;
		lane_starts[index].vehicle = NONE;
	}
	lane_vehicle_count = 0;
	lane_update_time = 0;
	memset(lane_counts, 0, sizeof(lane_counts));
	mendicant_go_by_lane = FALSE;
}

/* free movement asks: does a lane escape of his own hold him now */
boolean mendicant_lane_evading(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS || !lane_layer_on())
		return FALSE;
	return lane_soldiers[slot].actor == actor_index && lane_soldiers[slot].evading;
}

/* the drill and free movement: a safety move of his own (a grenade's, a lane's) holds him */
boolean mendicant_threat_evading(
	long actor_index)
{
	return mendicant_grenade_evading(actor_index) || mendicant_lane_evading(actor_index);
}

/* each tick after the AI's update: the lanes, every other tick */
void mendicant_lanes_update(
	long time)
{
	int index;

	mendicant_lanes_test_update(time);
	if (!actor_data || !actor_data->valid)
		return;
	/* the layer turned off (the settings menu) with no log watching: any
	escape still under way ends here, its order with it (else it would hold
	its soldier at its point) */
	if (!lane_watching())
	{
		for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		{
			struct lane_soldier *soldier = &lane_soldiers[index];

			if (soldier->actor == NONE || (soldier->vehicle == NONE && !soldier->evading))
				continue;
			lane_go_end(soldier->actor, soldier);
			soldier->vehicle = NONE;
			soldier->started = FALSE;
			soldier->too_late = FALSE;
			soldier->reaction = MENDICANT_LANE_NONE;
			soldier->freeze_until = soldier->wrong_until = 0;
		}
		return;
	}
	if (time < lane_update_time)
		lane_update_time = 0; /* (the clock went back) */
	if (time - lane_update_time < LANE_UPDATE_TICKS && lane_update_time)
		return;
	lane_update_time = time;
	lane_build();
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		struct lane_soldier *soldier = &lane_soldiers[index];
		long actor_index;

		if (!actor->identifier)
		{
			if (soldier->actor != NONE && soldier->vehicle != NONE)
			{
				soldier->vehicle = NONE;
				soldier->evading = FALSE;
			}
			continue;
		}
		actor_index = ((long)(unsigned short)actor->identifier << 16) | index;
		if (soldier->actor != actor_index && !lane_vehicle_count)
			continue;
		if (actor->meta.swarm || actor->meta.dormant || actor->meta.unit_index == NONE)
			continue;
		if (actor->input.body_vitality <= 0.0f)
		{
			if (soldier->actor == actor_index && soldier->vehicle != NONE)
				lane_episode_end(actor_index, actor, soldier, time, MENDICANT_LANE_OUTCOME_DIED);
			continue;
		}
		if (!lane_vehicle_count && (soldier->actor != actor_index || (soldier->vehicle == NONE && !soldier->evading)))
			continue;
		lane_soldier_update(actor_index, actor, time);
	}
}

/* each decision, after it: the game's own dives from vehicles (logged), and a
lane over his firing position a second let go */
void mendicant_lanes_after_decide(
	long actor_index)
{
	struct actor_datum *actor = lane_actor(actor_index);
	struct lane_soldier *soldier;
	struct actor_debug_info *debug;
	long time = mendicant_time();
	int slot = (int)(unsigned short)actor_index;

	if (!actor || slot >= MENDICANT_MAX_ACTORS || lane_soldiers[slot].actor != actor_index)
		return;
	soldier = &lane_soldiers[slot];
	debug = &actor_debug_array[DATUM_INDEX_TO_ABSOLUTE_INDEX(actor_index)];
	if (debug->dive_decision_time == time && debug->dive_decision == LANE_DIVE_SUCCESS &&
		actor->danger_zone.danger_type == LANE_DANGER_VEHICLE)
	{
		if (soldier->vehicle != NONE && !soldier->dive_time)
			soldier->dive_time = time;
		if (soldier->his_own)
			lane_count_start(actor->danger_zone.object_index, time); /* (a dive is a start too) */
		lane_log("ldive", actor_index, actor, actor->danger_zone.current_distance_from_actor,
			soldier->vehicle != NONE ? soldier->arrival : -1.0f, (long)(short)actor->danger_zone.object_index,
			soldier->reaction, soldier->move_time ? 1 : 0, soldier->seen_time ? (real)(time - soldier->seen_time) / 30.0f : -1.0f);
	}
	if (soldier->vehicle == NONE || !soldier->his_own || soldier->position_dropped)
		return;
	{
		struct lane_vehicle const *lane = lane_vehicle_find(soldier->vehicle);
		short position = actor->firing_positions.current_position_index;
		struct firing_position_definition *positions;
		int count = 0;
		real distance, t, wx, wy;
		int side;

		if (!lane || position == NONE || actor->meta.encounter_index == NONE)
			return;
		positions = mendicant_encounter_firing_positions(actor->meta.encounter_index, &count);
		if (!positions || position < 0 || position >= count)
			return;
		if (!lane_approach(lane, &positions[position].position, &distance, &t, &side, &wx, &wy) ||
			distance >= lane->radius + LANE_MARGIN || t < LANE_SOONEST)
		{
			soldier->position_ticks = 0;
			return;
		}
		if (!soldier->position_ticks)
			soldier->position_ticks = time;
		if (time - soldier->position_ticks >= LANE_POSITION_TICKS)
		{
			actor_discard_firing_position(actor_index, position, TRUE);
			soldier->position_dropped = TRUE;
		}
	}
}

/* actions.c, the danger avoidance once the game has found its way out: a
vehicle's dive waits a tick or three when two others on the same vehicle
started (moved or dived) within the last two ticks and it is not yet on him,
so a group never goes down on one frame. _mendicant_danger_moved: wait;
_stock: the game's own. */
short mendicant_lanes_danger_react(
	long actor_index)
{
	struct actor_datum *actor = lane_actor(actor_index);
	struct lane_soldier *soldier;
	int slot = (int)(unsigned short)actor_index;

	if (!actor || actor->danger_zone.danger_type != LANE_DANGER_VEHICLE || slot >= MENDICANT_MAX_ACTORS ||
		lane_soldiers[slot].actor != actor_index || !lane_his_own(actor_index, actor))
		return _mendicant_danger_stock;
	soldier = &lane_soldiers[slot];
	if (soldier->vehicle != actor->danger_zone.object_index || soldier->arrival < 0.35f)
		return _mendicant_danger_stock;
	if (lane_unison(soldier->vehicle, mendicant_time()))
		return _mendicant_danger_moved;
	return _mendicant_danger_stock;
}

/* actions.c, after an action's control: the glance at a vehicle behind him,
the run (or the panicked run), a Jackal's shield to the vehicle */
void mendicant_lanes_action_control(
	long actor_index,
	short action)
{
	struct actor_datum *actor;
	struct lane_soldier *soldier;
	int slot = (int)(unsigned short)actor_index;
	long time = mendicant_time();

	(void)action;
	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS || lane_soldiers[slot].actor != actor_index)
		return;
	soldier = &lane_soldiers[slot];
	if (soldier->vehicle == NONE || !soldier->his_own || !lane_layer_on() || !lane_unit(soldier->vehicle))
		return;
	actor = lane_actor(actor_index);
	if (!actor)
		return;
	/* the glance: his head turns to the engine noise before he moves; the
	Jackal keeps his shield to it while he steps */
	if ((soldier->behind && !soldier->started && time - soldier->seen_time < LANE_GLANCE_TICKS + 6) ||
		(soldier->started && soldier->evading && ACTOR_IS_JACKAL(actor->meta.type)))
	{
		if (actor->orders.look.primary_priority < 5)
		{
			actor->orders.look.primary_priority = 5;
			actor->orders.look.primary_direction.type = _direction_specification_object;
			actor->orders.look.primary_direction.object_index = soldier->vehicle;
		}
	}
	/* off the road at a run (out of a fight his gait would be a walk), a
	frightened Grunt in a panic */
	if (soldier->evading && soldier->started && !soldier->freeze_until)
	{
		actor->orders.move.stationary_crouch = FALSE; /* (up off his knee first thing) */
		actor->orders.move.moving_crouch = FALSE;
		if (soldier->panicked)
			actor->orders.move.panicked = TRUE;
		else if (actor->orders.move.override_movement_type == NONE)
			actor->orders.move.override_movement_type = _actor_movement_type_combat;
	}
}

/* actor_firing_position.c, after the pre-evaluators (and the spacing's): a
position clear of the lane threatening him scores up to 8 more; a Marine's
are kept out of the Chief's own Warthog's lane (each bonus finite, 0..8) */
void mendicant_lanes_bonus(
	long actor_index,
	struct firing_position_evaluation_context *evaluation_context,
	short firing_position_count,
	struct firing_position *firing_positions)
{
	struct actor_datum *actor = lane_actor(actor_index);
	struct lane_soldier *soldier;
	struct lane_vehicle const *lanes[2];
	int lane_count = 0, index, slot = (int)(unsigned short)actor_index;

	if (!lane_his_own(actor_index, actor) || firing_position_count <= 0 || !firing_positions || slot >= MENDICANT_MAX_ACTORS)
		return;
	switch (evaluation_context->evaluation_mode)
	{
	case _firing_point_evaluation_mode_fight:
	case _firing_point_evaluation_mode_cover:
	case _firing_point_evaluation_mode_uncover:
	case _firing_point_evaluation_mode_guard:
	case _firing_point_evaluation_mode_avoid:
		break;
	default:
		return;
	}
	soldier = &lane_soldiers[slot];
	if (soldier->actor == actor_index && soldier->vehicle != NONE && (lanes[0] = lane_vehicle_find(soldier->vehicle)) != NULL)
		lane_count = 1;
	if (ACTOR_IS_MARINE(actor->meta.type))
	{
		for (index = 0; index < lane_vehicle_count && lane_count < 2; index++)
		{
			struct lane_vehicle const *lane = &lane_vehicles[index];
			struct unit_datum *vehicle = lane_unit(lane->object);

			if (!lane->player || (lane_count && lanes[0] == lane) || !vehicle ||
				lane_length2(vehicle->object.position.x - actor->input.position.body_position.x,
					vehicle->object.position.y - actor->input.position.body_position.y) > LANE_CHIEF_RANGE)
				continue;
			lanes[lane_count++] = lane;
		}
	}
	if (!lane_count)
		return;
	for (index = 0; index < firing_position_count; index++)
	{
		struct firing_position *position = &firing_positions[index];
		boolean clear = TRUE;
		int lane;

		if (!position->valid || !position->definition)
			continue;
		for (lane = 0; lane < lane_count && clear; lane++)
		{
			real distance, t, wx, wy;
			int side;

			if (lane_approach(lanes[lane], &position->definition->position, &distance, &t, &side, &wx, &wy) &&
				distance < lanes[lane]->radius + LANE_MARGIN + 0.5f && t >= 0.05f)
				clear = FALSE;
		}
		if (!clear || !(position->evaluation == position->evaluation) || position->evaluation > 1e30f)
			continue;
		position->evaluation += LANE_POSITION_BONUS;
	}
}

/* a unit was hurt: run over by a moving vehicle (its driver, or the vehicle,
the owner, the vehicle at hand)? his friends who saw it remember */
void mendicant_lanes_hurt(
	long unit_index,
	long owner_object_index,
	boolean lethal)
{
	struct unit_datum *victim = lane_unit(unit_index);
	int index, lane_index;
	long time = mendicant_time();

	if (!victim || !actor_data || !actor_data->valid || owner_object_index == NONE || !lane_vehicle_count)
		return;
	for (lane_index = 0; lane_index < lane_vehicle_count; lane_index++)
	{
		struct lane_vehicle const *lane = &lane_vehicles[lane_index];
		struct unit_datum *vehicle = lane_unit(lane->object);

		if (!vehicle || (owner_object_index != lane->driver && owner_object_index != lane->object))
			continue;
		if (lane_length2(vehicle->object.position.x - victim->object.position.x,
			vehicle->object.position.y - victim->object.position.y) > lane->radius + 3.0f)
			continue;
		break;
	}
	if (lane_index == lane_vehicle_count)
		return;
	mendicant_lanes_test_hurt(unit_index, lethal);
	if (victim->unit.actor_index != NONE)
	{
		struct actor_datum *actor = lane_actor(victim->unit.actor_index);

		lane_log("lhit", victim->unit.actor_index, actor, lethal ? 1.0f : 0.0f, 0.0f,
			(long)(short)lane_vehicles[lane_index].object, actor ? lane_soldiers[(unsigned short)victim->unit.actor_index].reaction : -1,
			lane_vehicles[lane_index].player ? 1 : 0, 0.0f);
		if (actor && (unsigned short)victim->unit.actor_index < MENDICANT_MAX_ACTORS &&
			lane_soldiers[(unsigned short)victim->unit.actor_index].actor == victim->unit.actor_index)
		{
			struct lane_soldier *soldier = &lane_soldiers[(unsigned short)victim->unit.actor_index];

			if (soldier->vehicle == lane_vehicles[lane_index].object)
				lane_episode_end(victim->unit.actor_index, actor, soldier, time, MENDICANT_LANE_OUTCOME_HIT);
		}
	}
	/* friends who saw it: fear, a minute */
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *other = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		struct lane_soldier *soldier;
		long other_index;
		real dx, dy;

		if (!other->identifier || other->meta.unit_index == unit_index || other->input.body_vitality <= 0.0f ||
			game_team_is_enemy(other->meta.team_index, victim->object.owner_team_index))
			continue;
		dx = other->input.position.body_position.x - victim->object.position.x;
		dy = other->input.position.body_position.y - victim->object.position.y;
		if (dx * dx + dy * dy > LANE_FEAR_RANGE * LANE_FEAR_RANGE)
			continue;
		other_index = ((long)(unsigned short)other->identifier << 16) | index;
		soldier = lane_soldier_get(other_index);
		if (!soldier)
			continue;
		soldier->fear = (soldier->fear > 0.0f && time - soldier->fear_time < LANE_MEMORY_TICKS ?
			soldier->fear * (1.0f - (real)(time - soldier->fear_time) / LANE_MEMORY_TICKS) : 0.0f) + (lethal ? 1.0f : 0.5f);
		soldier->fear_time = time;
	}
}

/* the scripted scene's view of a soldier (mendicant_lanes_test.c) */
boolean mendicant_lanes_view(
	long actor_index,
	struct mendicant_lane_view *view)
{
	int slot = (int)(unsigned short)actor_index;
	struct lane_soldier const *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS || lane_soldiers[slot].actor != actor_index)
		return FALSE;
	soldier = &lane_soldiers[slot];
	view->vehicle = soldier->vehicle;
	view->seen_time = soldier->seen_time;
	view->move_time = soldier->move_time;
	view->dive_time = soldier->dive_time;
	view->reaction = soldier->reaction;
	view->reaction_time = soldier->reaction_time;
	view->lead = soldier->lead;
	view->too_late = soldier->too_late;
	view->memory = lane_memory(soldier, mendicant_time());
	return TRUE;
}
