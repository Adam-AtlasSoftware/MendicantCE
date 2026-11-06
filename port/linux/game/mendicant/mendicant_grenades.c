/*
MENDICANT_GRENADES.C

MENDICANT AI's grenade reactions and dispersal (the authentic layer's M2;
docs/mendicant_port.md, "Grenade reactions"): how each soldier with a
character (mendicant_character.c) notices a live grenade, how long he takes to
react, what he does and which way he goes. Hunters, the Flood, Sentinels,
Engineers and turrets have no character and react exactly as before, as do
actors in a scripted play fight or a command list.

The stock game has every soldier who sees an enemy grenade shout and dive on
the same tick, to the same side (the escape is worked out from the grenade's
motion alone). Here:

- Notice (actor_perception.c): in place of the game's 30 ticks of seeing an
  enemy grenade, his own time around them, from his reflex (the kind's mean
  stays the game's), whether it came from behind him, his kind and the
  fight's mood; his clock runs only while he perceives it, and the game's
  notice chance still decides. His own or a friend's: at once, as the game.
- Reaction (actions.c, actor_action_handle_danger_avoidance): after noticing
  it, a wait from his nerve (courage and reflex), kind and mood (at most 12
  ticks; under 22 ticks to the blast he goes at once), then one of the
  reactions the engine has animations and movement for: the dive (the game's
  own dive animations, in his own direction), a run clear (free movement,
  actor_go_to), a step back to about the blast's edge, or for a frightened
  Grunt or a rookie a freeze before the run. Near the blast but not in it, a
  step back or a run where the game would take its avoid action or leave a
  fighting soldier standing.
- Dispersal: each soldier's way out is his own, scored away from the
  grenade, away from his friends, away from where the others reacting to the
  same grenade are going, toward his own side and toward cover from it, with
  his own noise; a slow, jumpy soldier sometimes picks a poor way, and a
  soldier who likes a buddy may go with him.
- A stuck plasma grenade: a Marine runs away from his friends (and the Chief)
  as often as his selflessness says (40 to 90%); a Grunt panics as the game
  has it (into his friends), unless he is the brave one; a Jackal sometimes
  turns away; an Elite keeps the game's own berserk or flight.
- The Grunt coward's signature: he bolts the farthest, screaming the flee
  line.

The escape is free movement (actor_go_to, urgency 2), which no other owner of
free movement (the drill, following the Chief, the search) can take over while
it lasts (mendicant_go_by_grenade, mendicant_grenade_evading); it ends when the
grenade is gone or after three seconds.

Logged in the fire log (both layers, so the layer off is measured the same
way): gnotice, greact, gmove, gstuck (docs/mendicant_port.md). With
ai.mendicant.grenade_test (training), live grenades are dropped among soldiers
during each fight, and some plasma grenades stuck to them, for the measures.
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_globals.h"
#include "scenario/scenario.h"
#include "ai/ai.h"
#include "ai/actors.h"
#include "ai/actor_definitions.h"
#include "ai/actions.h"
#include "ai/ai_communication.h"
#include "ai/ai_debug.h"
#include "objects/objects.h"
#include "items/projectiles.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define GRENADE_DANGER_PROJECTILE 2  /* actor_danger_zone.danger_type */
#define GRENADE_DIVE_SUCCESS 4       /* ai_debug.h's _dive_success */
#define GRENADE_EVADE_LEFT 0         /* actions.c's _actor_evade_left */

#define GRENADE_NOTICE_STOCK 30      /* the game's own acknowledgement (ticks, in a fight) */
#define GRENADE_NOTICE_UNCERTAIN 4   /* ...where the game acknowledges at once */
#define GRENADE_NOTICE_MAXIMUM 45
#define GRENADE_REACT_MAXIMUM 12     /* from noticing to moving (ticks) */
#define GRENADE_URGENT_TICKS 22      /* this close to the blast: no wait */
#define GRENADE_AT_FEET 1.2f         /* world units: no wait */
#define GRENADE_FREEZE_MINIMUM 8
#define GRENADE_FREEZE_MAXIMUM 15
#define GRENADE_ESCAPE_TICKS 90      /* the longest escape order */
#define GRENADE_STUCK_TICKS 90       /* the longest run from friends */
#define GRENADE_MOVE_SAMPLE_TICKS 30 /* the log's displacement after a reaction */
#define GRENADE_STUCK_SAMPLE_TICKS 45 /* the log's distance to friends after a stick */
#define GRENADE_FRIEND_RANGE 8.0f
#define GRENADE_STUCK_FRIEND_RANGE 12.0f
#define GRENADE_DIRECTIONS 16
#define GRENADE_TABLE 32             /* grenades with handed-out escapes */
#define GRENADE_TABLE_ESCAPES 8
#define GRENADE_STUCK_FRIENDS 8

#define grenade_minimum(a, b) ((a) < (b) ? (a) : (b))
#define grenade_maximum(a, b) ((a) > (b) ? (a) : (b))
#define grenade_clamp(x, lo, hi) grenade_minimum(grenade_maximum((x), (lo)), (hi))

#define GRENADE_KIND_GRUNT(kind) ((kind) >= MENDICANT_KIND_GRUNT_MINOR && (kind) <= MENDICANT_KIND_GRUNT_FLEE)
#define GRENADE_KIND_JACKAL(kind) ((kind) == MENDICANT_KIND_JACKAL_MINOR || (kind) == MENDICANT_KIND_JACKAL_MAJOR)
#define GRENADE_KIND_ELITE(kind) ((kind) >= MENDICANT_KIND_ELITE_MINOR && (kind) <= MENDICANT_KIND_ELITE_STEALTH)
#define GRENADE_KIND_MARINE(kind) ((kind) >= MENDICANT_KIND_MARINE && (kind) <= MENDICANT_KIND_SERGEANT)

/* the reactions, as the log numbers them (0 and 5 are the game's own) */
enum
{
	GRENADE_REACT_NONE = -1,
	GRENADE_REACT_STOCK_DIVE = 0,
	GRENADE_REACT_DIVE,
	GRENADE_REACT_RUN,
	GRENADE_REACT_BACK,
	GRENADE_REACT_FREEZE,
	GRENADE_REACT_STOCK_AVOID,
	GRENADE_REACT_COWARD
};

/* ---------- globals */

/* free movement's owner check: set around this module's own calls */
boolean mendicant_go_by_grenade = FALSE;

/* each soldier's grenade state, by absolute actor index */
static struct grenade_soldier
{
	long actor;          /* the full index this slot is for, NONE for none */
	/* the grenade he is dealing with */
	long object;         /* NONE none */
	long seen_time;      /* its danger zone first seen */
	long notice_due;     /* he notices it at this tick (the layer on) */
	long notice_time;    /* he noticed it */
	long notice_wait;
	long react_wait;     /* from noticing it to reacting (ticks) */
	long react_due;      /* he reacts at this tick, 0 until he has noticed it */
	long react_time;     /* he reacted, 0 not yet */
	int kind;            /* GRENADE_REACT_*, NONE until he reacts */
	long dive_time;      /* a dive was asked for at this tick */
	long freeze_until;
	long escape_until;   /* his escape order lasts until then */
	boolean escaping;    /* he holds an escape order of his own */
	real dir_x, dir_y;
	real_point3d react_position;
	boolean move_logged;
	/* a plasma grenade stuck to him */
	long stuck_object;
	long stuck_time;
	int stuck_choice;    /* 1 away from friends (the layer), 0 the game's own, -1 the layer off */
	boolean stuck_running; /* his escape order is the run from his friends */
	int stuck_friends;
	long stuck_friend[GRENADE_STUCK_FRIENDS];
	real stuck_before;
	boolean stuck_logged;
	real_point3d last_position;
	long last_time;
} grenade_soldiers[MENDICANT_MAX_ACTORS];

/* each grenade's handed-out escapes (directions and ends) */
static struct grenade_escapes
{
	long object;  /* NONE free */
	long time;
	int count;
	real dir[GRENADE_TABLE_ESCAPES][2];
	real end[GRENADE_TABLE_ESCAPES][2];
} grenade_escapes[GRENADE_TABLE];

/* the escape directions: 16 around, 22.5 degrees apart (no maths functions in the game) */
static real const grenade_direction[GRENADE_DIRECTIONS][2] =
{
	{ 1.0f, 0.0f }, { 0.92388f, 0.38268f }, { 0.70711f, 0.70711f }, { 0.38268f, 0.92388f },
	{ 0.0f, 1.0f }, { -0.38268f, 0.92388f }, { -0.70711f, 0.70711f }, { -0.92388f, 0.38268f },
	{ -1.0f, 0.0f }, { -0.92388f, -0.38268f }, { -0.70711f, -0.70711f }, { -0.38268f, -0.92388f },
	{ 0.0f, -1.0f }, { 0.38268f, -0.92388f }, { 0.70711f, -0.70711f }, { 0.92388f, -0.38268f },
};

/* the grenade test (training): its own numbers, the next drop, the count */
static unsigned long grenade_test_random = 1;
static long grenade_test_next = 0;
static long grenade_test_drops = 0;
static boolean grenade_test_running = FALSE;
/* counters for the log */
static long grenade_reactions[GRENADE_REACT_COWARD + 1];

/* ---------- private code */

static boolean grenade_layer_on(
	void)
{
	return mendicant_active && mendicant_config.character && mendicant_config.grenade_reactions;
}

static struct actor_datum *grenade_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static struct object_datum *grenade_object(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return object_try_and_get_and_verify_type(object_index, _object_mask_all);
}

static struct grenade_soldier *grenade_soldier_get(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct grenade_soldier *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return NULL;
	soldier = &grenade_soldiers[slot];
	if (soldier->actor != actor_index)
	{
		memset(soldier, 0, sizeof(*soldier));
		soldier->actor = actor_index;
		soldier->object = NONE;
		soldier->kind = GRENADE_REACT_NONE;
		soldier->stuck_object = NONE;
	}
	return soldier;
}

/* the layer reacts for him: a character, the layer on, not in a script's hands */
static boolean grenade_his_own(
	long actor_index,
	struct actor_datum *actor)
{
	if (!grenade_layer_on() || !actor || !mendicant_soldier_has(actor_index))
		return FALSE;
	if (actor->external_orders.playfighting || actor->state.action == _actor_action_obey ||
		actor->meta.swarm)
		return FALSE;
	return mendicant_soldier_kind(actor_index) != MENDICANT_KIND_NONE;
}

static real grenade_rollf(
	long actor_index,
	long object,
	unsigned long salt)
{
	return mendicant_soldier_rollf(actor_index, 0x47e00000UL + (salt << 12) + ((unsigned long)object & 0xfffUL));
}

static real grenade_lerp(
	real a,
	real b,
	real t)
{
	return a + (b - a) * grenade_clamp(t, 0.0f, 1.0f);
}

static real grenade_length2(
	real x,
	real y)
{
	return (real)sqrt(x * x + y * y);
}

/* his kind as the log numbers it: his own, or with no character his species' */
static int grenade_log_kind(
	long actor_index,
	struct actor_datum *actor)
{
	int kind = mendicant_soldier_kind(actor_index);

	if (kind != MENDICANT_KIND_NONE || !actor)
		return kind;
	return ACTOR_IS_GRUNT(actor->meta.type) ? MENDICANT_KIND_GRUNT_MINOR : ACTOR_IS_JACKAL(actor->meta.type) ? MENDICANT_KIND_JACKAL_MINOR :
		ACTOR_IS_ELITE(actor->meta.type) ? MENDICANT_KIND_ELITE_MINOR : ACTOR_IS_MARINE(actor->meta.type) ? MENDICANT_KIND_MARINE : 0;
}

static boolean grenade_commanded(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	return order_actor[slot] == actor_index || (commanded_time[slot] != 0 && mendicant_time() - commanded_time[slot] < 60);
}

/* a record of the fire log (time,record,actor,kind,quirk,commanded,a,b,c,d,e,f) */
static void grenade_log(
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
		(int)(short)actor_index, grenade_log_kind(actor_index, actor), mendicant_soldier_quirk(actor_index),
		grenade_commanded(actor_index) ? 1 : 0, a, b, c, d, e, f);
}

static struct grenade_escapes *grenade_escapes_get(
	long object,
	boolean create)
{
	long time = mendicant_time();
	int index, free_index = -1, oldest = 0;

	for (index = 0; index < GRENADE_TABLE; index++)
	{
		struct grenade_escapes *entry = &grenade_escapes[index];

		if (entry->object == object && time - entry->time < 300)
			return entry;
		if (entry->object == NONE || time - entry->time >= 300)
		{
			if (free_index < 0)
				free_index = index;
		}
		else if (entry->time < grenade_escapes[oldest].time)
		{
			oldest = index;
		}
	}
	if (!create)
		return NULL;
	if (free_index < 0)
		free_index = oldest;
	memset(&grenade_escapes[free_index], 0, sizeof(grenade_escapes[free_index]));
	grenade_escapes[free_index].object = object;
	grenade_escapes[free_index].time = time;
	return &grenade_escapes[free_index];
}

/* free movement for his escape: no other owner can take it while it lasts */
static boolean grenade_go(
	long actor_index,
	struct grenade_soldier *soldier,
	real_point3d const *point,
	real tolerance,
	long ticks)
{
	int result;

	mendicant_go_by_grenade = TRUE;
	result = actor_go_to(actor_index, point, tolerance, 2);
	mendicant_go_by_grenade = FALSE;
	if (result != 1)
		return FALSE;
	soldier->escaping = TRUE;
	soldier->escape_until = mendicant_time() + ticks;
	return TRUE;
}

static void grenade_go_end(
	long actor_index,
	struct grenade_soldier *soldier)
{
	if (!soldier->escaping)
		return;
	soldier->escaping = FALSE;
	mendicant_go_by_grenade = TRUE;
	actor_go_cancel(actor_index);
	mendicant_go_by_grenade = FALSE;
}

/* the soldier's species group's position of friends and their positions */
static int grenade_friends(
	long actor_index,
	struct actor_datum *actor,
	real range,
	real_point3d *positions,
	long *indices,
	int maximum,
	boolean with_players)
{
	real_point3d const *at = &actor->input.position.body_position;
	int index, count = 0;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS && count < maximum; index++)
	{
		struct actor_datum *other = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		long other_index;
		real dx, dy, dz;

		if (!other->identifier || other->meta.team_index != actor->meta.team_index || other->meta.swarm ||
			other->input.body_vitality <= 0.0f || other->input.vehicle_index != NONE)
			continue;
		other_index = ((long)(unsigned short)other->identifier << 16) | index;
		if (other_index == actor_index)
			continue;
		dx = other->input.position.body_position.x - at->x;
		dy = other->input.position.body_position.y - at->y;
		dz = other->input.position.body_position.z - at->z;
		if (dx * dx + dy * dy + dz * dz > range * range || dz * dz > 9.0f)
			continue;
		positions[count] = other->input.position.body_position;
		if (indices)
			indices[count] = other_index;
		count++;
	}
	if (with_players)
	{
		long units[MENDICANT_MAX_PLAYERS];
		int players = mendicant_player_units(units, MENDICANT_MAX_PLAYERS), player;

		for (player = 0; player < players && count < maximum; player++)
		{
			struct object_datum *unit = grenade_object(units[player]);
			real dx, dy;

			if (!unit)
				continue;
			dx = unit->object.position.x - at->x;
			dy = unit->object.position.y - at->y;
			if (dx * dx + dy * dy > range * range)
				continue;
			positions[count] = unit->object.position;
			if (indices)
				indices[count] = NONE;
			count++;
		}
	}
	return count;
}

/* the point of the grenade's path nearest him: where it is, or where it will be */
static void grenade_centre(
	struct actor_datum *actor,
	real_point3d *centre)
{
	struct actor_danger_zone const *zone = &actor->danger_zone;
	real speed = grenade_length2(zone->velocity.i, zone->velocity.j);

	*centre = speed > 0.05f ? zone->bounding_sphere_center : zone->position;
}

/* the ground under a point near his height; FALSE off a ledge or into a pit */
static boolean grenade_ground_fits(
	struct actor_datum *actor,
	real_point3d const *point)
{
	real_point3d ground;

	if (!mendicant_ground_under(point, &ground))
		return FALSE;
	return ground.z - actor->input.position.body_position.z < 1.0f &&
		actor->input.position.body_position.z - ground.z < 1.2f;
}

/* his way out (a unit direction), scored: away from the grenade, from his
friends and from where the others reacting to it go; his own side, cover from
the blast, his own noise. distance: how far he goes along it. FALSE: none. */
static boolean grenade_choose_direction(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	real distance_minimum,
	real distance_margin,
	boolean dive,
	real *dir_x,
	real *dir_y,
	real_point3d *end)
{
	struct actor_danger_zone const *zone = &actor->danger_zone;
	real_point3d const *at = &actor->input.position.body_position;
	real_point3d centre, friends[24];
	struct grenade_escapes *escapes = grenade_escapes_get(zone->object_index, FALSE);
	real away_x, away_y, away, side_x, side_y, score[GRENADE_DIRECTIONS], reach[GRENADE_DIRECTIONS];
	real reflex = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_REFLEX);
	real buddy = mendicant_soldier_buddy(actor_index);
	boolean careless, with_buddy;
	int friend_count, index, tried, best;
	unsigned char used[GRENADE_DIRECTIONS];

	grenade_centre(actor, &centre);
	away_x = at->x - centre.x;
	away_y = at->y - centre.y;
	away = grenade_length2(away_x, away_y);
	if (away < 0.3f)
	{
		/* on top of it: away from where it came from, else his own facing */
		away_x = zone->velocity.i;
		away_y = zone->velocity.j;
		away = grenade_length2(away_x, away_y);
		if (away < 0.01f)
		{
			away_x = -actor->input.facing_vector.i;
			away_y = -actor->input.facing_vector.j;
			away = grenade_length2(away_x, away_y);
		}
		if (away < 0.01f)
		{
			away_x = 1.0f;
			away_y = 0.0f;
			away = 1.0f;
		}
	}
	away_x /= away;
	away_y /= away;
	/* his own side: the side he peeks from */
	side_x = -actor->input.facing_vector.j;
	side_y = actor->input.facing_vector.i;
	if (mendicant_soldier_peek_side(actor_index) == MENDICANT_SIDE_RIGHT)
	{
		side_x = -side_x;
		side_y = -side_y;
	}
	friend_count = grenade_friends(actor_index, actor, GRENADE_FRIEND_RANGE, friends, NULL, 24,
		GRENADE_KIND_MARINE(mendicant_soldier_kind(actor_index)));
	/* a slow, jumpy soldier now and then goes the obvious way, friends or not */
	careless = reflex < 0.35f && grenade_rollf(actor_index, zone->object_index, 1) < 0.25f;
	/* a soldier who likes a buddy may go his way */
	with_buddy = escapes && escapes->count > 0 && buddy > 0.7f && grenade_rollf(actor_index, zone->object_index, 2) < 0.35f;

	for (index = 0; index < GRENADE_DIRECTIONS; index++)
	{
		real x = grenade_direction[index][0], y = grenade_direction[index][1];
		real along = x * away_x + y * away_y, value, length, ex, ey;
		int other;

		used[index] = 0;
		/* far enough along it to be clear of the blast */
		length = distance_minimum + distance_margin - ((at->x - centre.x) * x + (at->y - centre.y) * y);
		length = grenade_clamp(length, distance_minimum, 9.0f);
		if (dive)
			length = distance_minimum;
		reach[index] = length;
		if (along < -0.15f)
		{
			score[index] = -1000.0f; /* not toward it */
			continue;
		}
		value = 2.0f * along;
		ex = at->x + x * length;
		ey = at->y + y * length;
		if (!careless)
		{
			for (other = 0; other < friend_count; other++)
			{
				real fx = friends[other].x - at->x, fy = friends[other].y - at->y;
				real fd = grenade_length2(fx, fy), toward;

				if (fd < 0.1f)
					continue;
				toward = (x * fx + y * fy) / fd;
				if (toward > 0.0f)
					value -= 1.2f * toward / (1.0f + fd * 0.5f);
			}
			if (escapes)
			{
				for (other = 0; other < escapes->count; other++)
				{
					real dx = ex - escapes->end[other][0], dy = ey - escapes->end[other][1];
					real gap = grenade_length2(dx, dy);
					real same = x * escapes->dir[other][0] + y * escapes->dir[other][1];

					if (with_buddy && other == escapes->count - 1)
					{
						value += 0.8f * same; /* together */
						continue;
					}
					if (gap < 4.0f)
						value -= 1.5f * (1.0f - gap / 4.0f);
					if (same > 0.0f)
						value -= 0.6f * same;
				}
			}
		}
		value += 0.3f * (x * side_x + y * side_y);
		value += 0.25f * (x * actor->input.facing_vector.i + y * actor->input.facing_vector.j);
		value += 0.5f * (grenade_rollf(actor_index, zone->object_index, 10 + index) - 0.5f);
		score[index] = value;
	}

	/* the best few, tested against the level: a clear run, ground at his
	height, cover from the blast a bonus */
	best = -1;
	for (tried = 0; tried < 6; tried++)
	{
		int pick = -1;
		real_point3d from, to, ground_point;
		int blocked;

		for (index = 0; index < GRENADE_DIRECTIONS; index++)
			if (!used[index] && score[index] > -500.0f && (pick < 0 || score[index] > score[pick]))
				pick = index;
		if (pick < 0)
			break;
		used[pick] = 1;
		from = *at;
		from.z += 0.5f;
		to.x = at->x + grenade_direction[pick][0] * reach[pick];
		to.y = at->y + grenade_direction[pick][1] * reach[pick];
		to.z = at->z + 0.5f;
		blocked = mendicant_line_blocked(&from, &to);
		if (blocked == 1)
		{
			score[pick] = -1000.0f;
			continue;
		}
		ground_point = to;
		if (!grenade_ground_fits(actor, &ground_point))
		{
			score[pick] = -1000.0f;
			continue;
		}
		if (blocked == 0 && tried < 4)
		{
			real_point3d blast = centre;

			blast.z += 0.3f;
			if (mendicant_cover_score(&ground_point, &blast) > 0)
				score[pick] += 0.4f;
		}
		if (best < 0 || score[pick] > score[best])
			best = pick;
	}
	if (best < 0)
		return FALSE;
	*dir_x = grenade_direction[best][0];
	*dir_y = grenade_direction[best][1];
	end->x = at->x + *dir_x * reach[best];
	end->y = at->y + *dir_y * reach[best];
	end->z = at->z + 0.3f;
	return TRUE;
}

/* the others reacting to this grenade see his way out */
static void grenade_hand_out(
	long object,
	real dir_x,
	real dir_y,
	real_point3d const *end)
{
	struct grenade_escapes *escapes = grenade_escapes_get(object, TRUE);
	int at;

	if (escapes->count < GRENADE_TABLE_ESCAPES)
		at = escapes->count++;
	else
		at = GRENADE_TABLE_ESCAPES - 1;
	escapes->dir[at][0] = dir_x;
	escapes->dir[at][1] = dir_y;
	escapes->end[at][0] = end->x;
	escapes->end[at][1] = end->y;
	escapes->time = mendicant_time();
}

/* his nerve against a grenade: courage and reflex, with the mood on them */
static real grenade_nerve(
	long actor_index)
{
	return 0.55f * mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_COURAGE) +
		0.45f * mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_REFLEX);
}

/* the wait from noticing it to moving (ticks) */
static long grenade_reaction_wait(
	long actor_index,
	long object)
{
	int kind = mendicant_soldier_kind(actor_index), quirk = mendicant_soldier_quirk(actor_index);
	real wait = grenade_lerp(9.0f, 0.0f, grenade_nerve(actor_index));

	wait *= mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_REACTION);
	wait += 3.0f * grenade_rollf(actor_index, object, 3);
	if (quirk == MENDICANT_QUIRK_ROOKIE)
		wait += 4.0f;
	else if (quirk == MENDICANT_QUIRK_VETERAN)
		wait -= 2.0f;
	if (GRENADE_KIND_ELITE(kind))
		wait *= 0.6f;
	else if (GRENADE_KIND_GRUNT(kind))
		wait *= 1.2f;
	return (long)(grenade_clamp(wait, 0.0f, (real)GRENADE_REACT_MAXIMUM) + 0.5f);
}

/* which reaction: by his kind and quirk, then his traits, then his own roll */
static int grenade_choose_reaction(
	long actor_index,
	struct actor_datum *actor,
	long object)
{
	int kind = mendicant_soldier_kind(actor_index), quirk = mendicant_soldier_quirk(actor_index);
	real courage = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_COURAGE);
	real caution = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_CAUTION);
	real aggression = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_AGGRESSION);
	real distance = actor->danger_zone.current_distance_from_actor;
	real dive = 0.0f, run = 0.0f, back = 0.0f, freeze = 0.0f, total, roll;

	if (GRENADE_KIND_GRUNT(kind))
	{
		if (quirk == MENDICANT_QUIRK_COWARD && mendicant_soldier_signature_due(actor_index))
			return GRENADE_REACT_COWARD;
		dive = 0.20f;
		run = 0.45f;
		back = 0.15f;
		freeze = 0.20f * (1.4f - courage);
		if (quirk == MENDICANT_QUIRK_PANIC_THROWER)
			freeze *= 2.0f;
		else if (quirk == MENDICANT_QUIRK_BRAVE)
			freeze = 0.0f;
	}
	else if (GRENADE_KIND_JACKAL(kind))
	{
		dive = 0.15f;
		run = 0.30f;
		back = 0.55f * (quirk == MENDICANT_QUIRK_SKITTISH ? 1.5f : 1.0f);
	}
	else if (GRENADE_KIND_ELITE(kind))
	{
		dive = distance < 2.5f ? 0.55f : 0.25f;
		run = 0.25f;
		back = 0.45f * (quirk == MENDICANT_QUIRK_BERSERKER || quirk == MENDICANT_QUIRK_DUELIST ? 1.5f : 1.0f);
	}
	else
	{
		/* the game's Marines dive two times in three */
		dive = 0.55f * (quirk == MENDICANT_QUIRK_VETERAN ? 1.2f : 1.0f);
		run = 0.25f;
		back = 0.20f;
		if (quirk == MENDICANT_QUIRK_ROOKIE)
			freeze = 0.25f;
	}
	run *= 0.7f + 0.6f * caution;
	back *= 0.7f + 0.6f * aggression;
	total = dive + run + back + freeze;
	roll = grenade_rollf(actor_index, object, 4) * total;
	if (roll < dive)
		return GRENADE_REACT_DIVE;
	if (roll < dive + run)
		return GRENADE_REACT_RUN;
	if (roll < dive + run + back)
		return GRENADE_REACT_BACK;
	return GRENADE_REACT_FREEZE;
}

/* a run clear or a step back, in his own direction; FALSE: no way found */
static boolean grenade_start_move(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	int reaction)
{
	real radius = actor->danger_zone.danger_radius, margin, minimum, tolerance;
	real dir_x, dir_y;
	real_point3d end;

	switch (reaction)
	{
	case GRENADE_REACT_BACK:
		/* to about the blast's edge: a step back is not always quite enough */
		margin = -0.3f + 0.8f * grenade_rollf(actor_index, soldier->object, 5);
		minimum = 1.5f;
		tolerance = 0.8f;
		break;
	case GRENADE_REACT_COWARD:
		margin = 3.0f + 2.0f * grenade_rollf(actor_index, soldier->object, 5);
		minimum = 4.0f;
		tolerance = 1.5f;
		break;
	default:
		margin = 0.3f + 1.5f * grenade_rollf(actor_index, soldier->object, 5);
		minimum = 2.5f;
		tolerance = 1.0f;
		break;
	}
	if (!grenade_choose_direction(actor_index, actor, soldier, grenade_maximum(minimum, radius * 0.5f), radius + margin - radius * 0.5f,
		FALSE, &dir_x, &dir_y, &end))
		return FALSE;
	if (!grenade_go(actor_index, soldier, &end, tolerance, GRENADE_ESCAPE_TICKS))
		return FALSE;
	soldier->dir_x = dir_x;
	soldier->dir_y = dir_y;
	grenade_hand_out(soldier->object, dir_x, dir_y, &end);
	return TRUE;
}

static void grenade_reacted(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	int reaction)
{
	if (!soldier->react_time)
	{
		soldier->react_time = mendicant_time();
		mendicant_status.grenade_reactions++;
		soldier->react_position = actor->input.position.body_position;
		soldier->move_logged = FALSE;
		grenade_log("greact", actor_index, actor, soldier->dir_x, soldier->dir_y, (long)(short)soldier->object, reaction,
			mendicant_time() - soldier->seen_time, actor->danger_zone.current_distance_from_actor);
	}
	soldier->kind = reaction;
	if (reaction >= 0 && reaction <= GRENADE_REACT_COWARD)
		grenade_reactions[reaction]++;
}

/* a new grenade for him: the waits drawn */
static void grenade_new_object(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier)
{
	struct actor_danger_zone const *zone = &actor->danger_zone;
	long time = mendicant_time();

	if (soldier->escaping && !soldier->stuck_running)
		grenade_go_end(actor_index, soldier);
	soldier->object = zone->object_index;
	soldier->seen_time = time;
	soldier->notice_time = 0;
	soldier->notice_wait = 0;
	soldier->notice_due = time;
	soldier->react_wait = 0;
	soldier->react_due = 0;
	soldier->react_time = 0;
	soldier->kind = GRENADE_REACT_NONE;
	soldier->dive_time = 0;
	soldier->freeze_until = 0;
	soldier->dir_x = soldier->dir_y = 0.0f;
	soldier->move_logged = TRUE;
	if (grenade_his_own(actor_index, actor))
	{
		/* the game acknowledges a grenade after 30 ticks of seeing it in a
		fight (at once just after an uncertain moment), every soldier alike:
		each his own time around that, from his reflex (its mean over his kind
		is the game's), the mood, his kind, and whether it came from behind him */
		int kind = mendicant_soldier_kind(actor_index);
		real dx = zone->position.x - actor->input.position.body_position.x;
		real dy = zone->position.y - actor->input.position.body_position.y;
		real d = grenade_length2(dx, dy);
		real base = actor->state.uncertain_combat_timer != NONE && actor->state.uncertain_combat_timer < 60 ?
			(real)GRENADE_NOTICE_UNCERTAIN : (real)GRENADE_NOTICE_STOCK;
		real wait = base * mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_REFLEX, 1.45f, 0.55f) *
			mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_REACTION);

		if (d > 0.1f && (dx * actor->input.facing_vector.i + dy * actor->input.facing_vector.j) / d < 0.0f)
			wait += 2.0f + 4.0f * grenade_rollf(actor_index, soldier->object, 6);
		wait += 6.0f * (grenade_rollf(actor_index, soldier->object, 7) - 0.5f);
		if (GRENADE_KIND_ELITE(kind))
			wait *= 0.85f;
		else if (GRENADE_KIND_GRUNT(kind))
			wait *= 1.1f;
		soldier->notice_wait = (long)grenade_clamp(wait + 0.5f, 0.0f, (real)GRENADE_NOTICE_MAXIMUM);
		soldier->notice_due = time + soldier->notice_wait;
		soldier->react_wait = grenade_reaction_wait(actor_index, soldier->object);
	}
}

/* about to go off, or at his feet: no more waiting */
static boolean grenade_urgent(
	struct actor_datum *actor)
{
	struct actor_danger_zone const *zone = &actor->danger_zone;

	return (zone->projectile.time_until_explosion != NONE && zone->projectile.time_until_explosion < GRENADE_URGENT_TICKS) ||
		zone->current_distance_from_actor < GRENADE_AT_FEET;
}

/* a plasma grenade stuck to him: away from his friends, or the game's own panic */
static void grenade_stuck_start(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	long projectile)
{
	real_point3d friends[GRENADE_STUCK_FRIENDS + 4];
	long indices[GRENADE_STUCK_FRIENDS + 4];
	real_point3d const *at = &actor->input.position.body_position;
	real sum = 0.0f, cx = 0.0f, cy = 0.0f;
	int count, index, kind = mendicant_soldier_kind(actor_index), quirk = mendicant_soldier_quirk(actor_index);
	boolean marine;

	soldier->stuck_object = projectile;
	soldier->stuck_time = mendicant_time();
	soldier->stuck_logged = FALSE;
	soldier->stuck_choice = -1;
	marine = ACTOR_IS_MARINE(actor->meta.type);
	count = grenade_friends(actor_index, actor, GRENADE_STUCK_FRIEND_RANGE, friends, indices, GRENADE_STUCK_FRIENDS + 4, marine);
	soldier->stuck_friends = 0;
	for (index = 0; index < count; index++)
	{
		real d = grenade_length2(friends[index].x - at->x, friends[index].y - at->y);

		sum += d;
		cx += friends[index].x;
		cy += friends[index].y;
		if (indices[index] != NONE && soldier->stuck_friends < GRENADE_STUCK_FRIENDS)
			soldier->stuck_friend[soldier->stuck_friends++] = indices[index];
	}
	soldier->stuck_before = count ? sum / count : -1.0f;
	if (!count || !grenade_his_own(actor_index, actor) || actor->input.vehicle_index != NONE)
		return;
	{
		real chance, selfless;
		real loyalty = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_LOYALTY);
		real courage = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_COURAGE);
		real discipline = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE);

		selfless = grenade_clamp(0.5f * loyalty + 0.3f * courage + 0.2f * discipline, 0.0f, 1.0f);
		if (GRENADE_KIND_MARINE(kind))
			chance = 0.4f + 0.5f * selfless;
		else if (GRENADE_KIND_GRUNT(kind))
			chance = quirk == MENDICANT_QUIRK_BRAVE ? 0.85f : quirk == MENDICANT_QUIRK_COWARD ? 0.0f : 0.05f + 0.15f * courage;
		else if (GRENADE_KIND_JACKAL(kind))
			chance = 0.2f + 0.3f * courage;
		else
			chance = 0.0f; /* Elites: the game's own berserk or flight */
		soldier->stuck_choice = 0;
		if (grenade_rollf(actor_index, projectile, 8) >= chance)
			return;
	}
	/* away from the middle of his friends, 6 to 9 world units, the nearest way that fits */
	{
		real ax = at->x - cx / count, ay = at->y - cy / count, a = grenade_length2(ax, ay);
		real length = 6.0f + 3.0f * grenade_rollf(actor_index, projectile, 9);
		static real const turns[5][2] = { { 1.0f, 0.0f }, { 0.866f, 0.5f }, { 0.866f, -0.5f }, { 0.5f, 0.866f }, { 0.5f, -0.866f } };
		int turn;

		if (a < 0.1f)
		{
			ax = actor->input.facing_vector.i;
			ay = actor->input.facing_vector.j;
			a = grenade_length2(ax, ay);
			if (a < 0.01f)
			{
				ax = 1.0f;
				ay = 0.0f;
				a = 1.0f;
			}
		}
		ax /= a;
		ay /= a;
		for (turn = 0; turn < 5; turn++)
		{
			real x = ax * turns[turn][0] - ay * turns[turn][1], y = ax * turns[turn][1] + ay * turns[turn][0];
			real_point3d from = *at, to;

			to.x = at->x + x * length;
			to.y = at->y + y * length;
			to.z = at->z + 0.5f;
			from.z += 0.5f;
			if (mendicant_line_blocked(&from, &to) == 1 || !grenade_ground_fits(actor, &to))
				continue;
			if (grenade_go(actor_index, soldier, &to, 1.5f, GRENADE_STUCK_TICKS))
			{
				soldier->stuck_choice = 1;
				soldier->stuck_running = TRUE;
				soldier->dir_x = x;
				soldier->dir_y = y;
				return;
			}
		}
	}
}

static void grenade_stuck_log(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier)
{
	real sum = 0.0f;
	int count = 0, index;

	soldier->stuck_logged = TRUE;
	for (index = 0; index < soldier->stuck_friends; index++)
	{
		struct actor_datum *other = grenade_actor(soldier->stuck_friend[index]);

		if (!other)
			continue;
		sum += grenade_length2(other->input.position.body_position.x - soldier->last_position.x,
			other->input.position.body_position.y - soldier->last_position.y);
		count++;
	}
	grenade_log("gstuck", actor_index, actor, soldier->stuck_before, count ? sum / count : -1.0f,
		(long)(short)soldier->stuck_object, soldier->stuck_choice, count, (real)(soldier->last_time - soldier->stuck_time));
}

/* a reaction on his feet begins: the freeze (he stops dead, then bolts), the
run, the step back, the coward's bolt; FALSE: no way out found */
static boolean grenade_begin_move(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	int reaction)
{
	long time = mendicant_time();

	soldier->kind = reaction;
	if (reaction == GRENADE_REACT_FREEZE)
	{
		real_point3d here = actor->input.position.body_position;

		soldier->freeze_until = time + GRENADE_FREEZE_MINIMUM +
			(long)((GRENADE_FREEZE_MAXIMUM - GRENADE_FREEZE_MINIMUM) * grenade_rollf(actor_index, soldier->object, 11));
		if (grenade_go(actor_index, soldier, &here, 0.5f, GRENADE_FREEZE_MAXIMUM + 2))
		{
			grenade_reacted(actor_index, actor, soldier, GRENADE_REACT_FREEZE);
			return TRUE;
		}
		reaction = GRENADE_REACT_RUN;
		soldier->kind = reaction;
	}
	if (!grenade_start_move(actor_index, actor, soldier, reaction))
	{
		soldier->kind = GRENADE_REACT_STOCK_DIVE; /* the game's own from here */
		return FALSE;
	}
	if (reaction == GRENADE_REACT_COWARD)
	{
		/* his signature: he bolts screaming the flee line */
		ai_communication_event(_ai_communication_flee, actor->meta.unit_index, NONE, NONE, NONE, NONE, NULL);
		mendicant_soldier_signature_spent(actor_index);
	}
	grenade_reacted(actor_index, actor, soldier, reaction);
	return TRUE;
}

/* his reaction on his feet goes on: TRUE while it does (the freeze ending in
the run), FALSE when it is over or failed (the game's own from there) */
static boolean grenade_continue_move(
	long actor_index,
	struct actor_datum *actor,
	struct grenade_soldier *soldier,
	boolean inside)
{
	long time = mendicant_time();
	int status;

	if (soldier->kind == GRENADE_REACT_FREEZE)
	{
		if (time < soldier->freeze_until)
			return TRUE;
		if (grenade_start_move(actor_index, actor, soldier, GRENADE_REACT_RUN))
		{
			soldier->kind = GRENADE_REACT_RUN;
			return TRUE;
		}
		grenade_go_end(actor_index, soldier);
		soldier->kind = GRENADE_REACT_STOCK_DIVE;
		return FALSE;
	}
	status = actor_go_status(actor_index);
	if (soldier->escaping && (status == GO_STATUS_MOVING || status == GO_STATUS_YIELDING))
		return TRUE;
	if (soldier->escaping && status == GO_STATUS_ARRIVED && !inside)
		return TRUE;
	/* there and still inside it (it rolled after him): once more */
	if (soldier->escaping && status == GO_STATUS_ARRIVED && grenade_start_move(actor_index, actor, soldier, GRENADE_REACT_RUN))
	{
		soldier->kind = GRENADE_REACT_RUN;
		return TRUE;
	}
	grenade_go_end(actor_index, soldier);
	soldier->kind = GRENADE_REACT_STOCK_DIVE;
	return FALSE;
}

/* ---------- private code: the grenade test (training) */

static unsigned long grenade_test_rand(
	void)
{
	grenade_test_random ^= (grenade_test_random << 13) & 0xFFFFFFFFUL;
	grenade_test_random ^= grenade_test_random >> 17;
	grenade_test_random ^= (grenade_test_random << 5) & 0xFFFFFFFFUL;
	grenade_test_random &= 0xFFFFFFFFUL;
	return grenade_test_random;
}

static real grenade_test_randf(
	void)
{
	return (real)(grenade_test_rand() & 0xffffUL) / 65536.0f;
}

/* the nearest unit of the soldier's enemies (an actor's or a player's): the grenade's owner */
static long grenade_test_owner(
	struct actor_datum *target)
{
	real_point3d const *at = &target->input.position.body_position;
	long best = NONE, units[MENDICANT_MAX_PLAYERS];
	real best_distance = 1e30f;
	int index, players;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *other = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		real dx, dy, d;

		if (!other->identifier || other->meta.unit_index == NONE || other->input.body_vitality <= 0.0f ||
			!game_team_is_enemy(other->meta.team_index, target->meta.team_index))
			continue;
		dx = other->input.position.body_position.x - at->x;
		dy = other->input.position.body_position.y - at->y;
		d = dx * dx + dy * dy;
		if (d < best_distance)
		{
			best_distance = d;
			best = other->meta.unit_index;
		}
	}
	players = mendicant_player_units(units, MENDICANT_MAX_PLAYERS);
	for (index = 0; index < players; index++)
	{
		struct object_datum *unit = grenade_object(units[index]);
		real dx, dy, d;

		if (!unit || !game_team_is_enemy(unit->object.owner_team_index, target->meta.team_index))
			continue;
		dx = unit->object.position.x - at->x;
		dy = unit->object.position.y - at->y;
		d = dx * dx + dy * dy;
		if (d < best_distance)
		{
			best_distance = d;
			best = units[index];
		}
	}
	return best;
}

/* a live grenade of the owner's side near a soldier with friends about, or stuck to one */
static void grenade_test_drop(
	long time)
{
	long candidates[MENDICANT_MAX_ACTORS];
	int count = 0, index, crowded = 0;
	struct actor_datum *target;
	struct object_datum *owner;
	struct object_placement_data placement;
	struct game_globals *globals = scenario_get_game_globals();
	struct game_globals_grenade *grenade;
	long owner_index, object_index;
	int type;
	boolean stick;
	real_vector3d velocity;

	if (!globals || !actor_data || !actor_data->valid)
		return;
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		real_point3d near[2];

		if (!actor->identifier || actor->meta.dormant || actor->meta.swarm || actor->meta.unit_index == NONE ||
			actor->input.body_vitality <= 0.0f || actor->input.vehicle_index != NONE ||
			!(ACTOR_IS_GRUNT(actor->meta.type) || ACTOR_IS_JACKAL(actor->meta.type) || ACTOR_IS_ELITE(actor->meta.type) ||
				ACTOR_IS_MARINE(actor->meta.type)))
			continue;
		/* soldiers with a friend close by first */
		if (grenade_friends(((long)(unsigned short)actor->identifier << 16) | index, actor, 5.0f, near, NULL, 1, FALSE) > 0)
		{
			candidates[count++] = candidates[crowded];
			candidates[crowded++] = ((long)(unsigned short)actor->identifier << 16) | index;
		}
		else
		{
			candidates[count++] = ((long)(unsigned short)actor->identifier << 16) | index;
		}
	}
	if (!count)
		return;
	index = crowded && grenade_test_randf() < 0.85f ? (int)(grenade_test_rand() % crowded) : (int)(grenade_test_rand() % count);
	target = grenade_actor(candidates[index]);
	if (!target)
		return;
	owner_index = grenade_test_owner(target);
	owner = grenade_object(owner_index);
	if (!owner)
		return;
	/* the owner's side's grenade: plasma from the Covenant, a frag from the humans */
	type = owner->object.owner_team_index == TEAM_COVENANT ? 1 : 0;
	stick = (grenade_test_drops % 3) == 2 && (ACTOR_IS_MARINE(target->meta.type) || ACTOR_IS_GRUNT(target->meta.type));
	if (stick)
		type = 1;
	if (type >= globals->grenades.count)
		return;
	grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, type, struct game_globals_grenade);
	if (!grenade || grenade->projectile.index == NONE)
		return;
	object_placement_data_new(&placement, grenade->projectile.index, owner_index);
	SET_FLAG(placement.flags, _new_object_never_automatically_delete_bit, TRUE);
	if (stick)
	{
		/* in front of his chest, coming at him */
		placement.position.x = target->input.position.body_position.x + target->input.facing_vector.i * 0.9f;
		placement.position.y = target->input.position.body_position.y + target->input.facing_vector.j * 0.9f;
		placement.position.z = target->input.position.body_position.z + 0.5f;
		velocity.i = -target->input.facing_vector.i * 0.4f;
		velocity.j = -target->input.facing_vector.j * 0.4f;
		velocity.k = 0.0f;
	}
	else
	{
		/* thrown from the owner's side: it arrives over 20 ticks, aimed 0.5 to 2
		world units from him, and bounces on the way he is */
		int pick = (int)(grenade_test_rand() % GRENADE_DIRECTIONS);
		real distance = 0.5f + 1.5f * grenade_test_randf();
		real_point3d landing;
		real fx = owner->object.position.x - target->input.position.body_position.x;
		real fy = owner->object.position.y - target->input.position.body_position.y;
		real f = grenade_length2(fx, fy);

		if (f < 0.1f)
		{
			fx = 1.0f;
			fy = 0.0f;
			f = 1.0f;
		}
		fx /= f;
		fy /= f;
		landing.x = target->input.position.body_position.x + grenade_direction[pick][0] * distance;
		landing.y = target->input.position.body_position.y + grenade_direction[pick][1] * distance;
		landing.z = target->input.position.body_position.z;
		placement.position.x = landing.x + fx * 5.0f;
		placement.position.y = landing.y + fy * 5.0f;
		placement.position.z = landing.z + 2.0f;
		velocity.i = (landing.x - placement.position.x) / 20.0f;
		velocity.j = (landing.y - placement.position.y) / 20.0f;
		velocity.k = -0.064f;
		/* not through a wall: from above him instead */
		if (mendicant_line_blocked(&placement.position, &landing) == 1)
		{
			placement.position = landing;
			placement.position.z += 0.8f;
			velocity.i = -fx * 0.04f;
			velocity.j = -fy * 0.04f;
			velocity.k = -0.05f;
		}
	}
	placement.forward.i = velocity.i;
	placement.forward.j = velocity.j;
	placement.forward.k = 0.0f;
	if (normalize3d(&placement.forward) == 0.0f)
		set_real_vector3d(&placement.forward, 1.0f, 0.0f, 0.0f);
	set_real_vector3d(&placement.up, 0.0f, 0.0f, 1.0f);
	object_index = object_new(&placement);
	if (object_index == NONE)
		return;
	projectile_accelerate(object_index, &velocity);
	grenade_test_drops++;
	grenade_log("gtest", candidates[index], target, placement.position.x, placement.position.y, (long)(short)object_index,
		stick ? 1 : 0, type, 0.0f);
	(void)time;
}

/* ---------- public code */

void mendicant_grenades_reset(
	void)
{
	int index;

	memset(grenade_soldiers, 0, sizeof(grenade_soldiers));
	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
	{
		grenade_soldiers[index].actor = NONE;
		grenade_soldiers[index].object = NONE;
		grenade_soldiers[index].stuck_object = NONE;
	}
	for (index = 0; index < GRENADE_TABLE; index++)
		grenade_escapes[index].object = NONE;
	memset(grenade_reactions, 0, sizeof(grenade_reactions));
	grenade_test_running = FALSE;
	mendicant_go_by_grenade = FALSE;
}

void mendicant_grenades_episode_start(
	unsigned long seed)
{
	grenade_test_random = (seed ^ 0x6a09e667UL) & 0xFFFFFFFFUL;
	if (!grenade_test_random)
		grenade_test_random = 2463534242UL;
	grenade_test_next = mendicant_time() + 90;
	grenade_test_drops = 0;
	grenade_test_running = TRUE;
}

/* free movement asks: does a grenade escape of his own hold him now */
boolean mendicant_grenade_evading(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	return grenade_soldiers[slot].actor == actor_index && grenade_soldiers[slot].escaping;
}

/* actor_perception.c, in place of the game's acknowledgement timer: -1 he
has not taken the grenade in yet, 1 now (an enemy's: the game's notice chance
still decides), 0 the game's own. Only while he perceives it, as the game. */
short mendicant_grenades_notice(
	long actor_index)
{
	struct actor_datum *actor = grenade_actor(actor_index);
	struct grenade_soldier *soldier;
	struct actor_danger_zone const *zone;

	if (!actor || actor->danger_zone.danger_type != GRENADE_DANGER_PROJECTILE || actor->danger_zone.attached_to_us ||
		!grenade_his_own(actor_index, actor))
		return 0;
	soldier = grenade_soldier_get(actor_index);
	if (!soldier)
		return 0;
	zone = &actor->danger_zone;
	if (soldier->object != zone->object_index)
		grenade_new_object(actor_index, actor, soldier);
	/* his own or a friend's: he saw it thrown (the game's own, at once) */
	if (zone->hostility != 0)
		return 0;
	if (!zone->currently_perceived)
	{
		/* unseen: his own clock runs only while he can perceive it */
		soldier->notice_due++;
		return -1;
	}
	/* (no hurry here: the game notices nothing early either; once he has
	noticed it, a blast close at hand cuts his wait short) */
	if (mendicant_time() >= soldier->notice_due)
		return 1;
	return -1;
}

/* actions.c, the danger avoidance's start: TRUE while he takes it in (no
avoidance, no avoid action yet) */
boolean mendicant_grenades_hold(
	long actor_index)
{
	struct actor_datum *actor = grenade_actor(actor_index);
	struct grenade_soldier *soldier;

	if (!actor || actor->danger_zone.danger_type != GRENADE_DANGER_PROJECTILE || !actor->danger_zone.noticed_danger ||
		actor->danger_zone.attached_to_us || !grenade_his_own(actor_index, actor))
		return FALSE;
	soldier = grenade_soldier_get(actor_index);
	if (!soldier)
		return FALSE;
	if (soldier->object != actor->danger_zone.object_index)
		grenade_new_object(actor_index, actor, soldier);
	if (soldier->kind != GRENADE_REACT_NONE || soldier->react_time)
		return FALSE;
	/* the wait runs from the tick he noticed it (the game may take longer to
	acknowledge it than his own notice) */
	if (!soldier->react_due)
		soldier->react_due = mendicant_time() + soldier->react_wait;
	if (mendicant_time() >= soldier->react_due || grenade_urgent(actor))
		return FALSE;
	return TRUE;
}

/* actions.c, near the blast but not in it, standing or at his position:
where the game takes the avoid action (passive or pursuing) and where it
leaves a fighting soldier standing. TRUE when he steps clear his own way
instead. (Fleeing, berserk or charging: the game's own.) */
boolean mendicant_grenades_avoid(
	long actor_index,
	short action_class)
{
	struct actor_datum *actor = grenade_actor(actor_index);
	struct grenade_soldier *soldier;
	int reaction;

	if (!actor || actor->danger_zone.danger_type != GRENADE_DANGER_PROJECTILE || !actor->danger_zone.noticed_danger ||
		actor->danger_zone.attached_to_us || actor->input.vehicle_index != NONE || !grenade_his_own(actor_index, actor))
		return FALSE;
	if (action_class != _action_class_passive && action_class != _action_class_pursuit &&
		action_class != _action_class_active)
		return FALSE;
	if (actor->state.action == _actor_action_flee || actor->state.action == _actor_action_charge)
		return FALSE;
	soldier = grenade_soldier_get(actor_index);
	if (!soldier)
		return FALSE;
	if (soldier->object != actor->danger_zone.object_index)
		grenade_new_object(actor_index, actor, soldier);
	switch (soldier->kind)
	{
	case GRENADE_REACT_NONE:
	case GRENADE_REACT_DIVE: /* (he was in it; now out of it) */
		break;
	case GRENADE_REACT_FREEZE:
	case GRENADE_REACT_RUN:
	case GRENADE_REACT_BACK:
	case GRENADE_REACT_COWARD:
		return grenade_continue_move(actor_index, actor, soldier, FALSE);
	default:
		return FALSE;
	}
	/* out of the blast: no dive, a step back serves */
	reaction = grenade_choose_reaction(actor_index, actor, soldier->object);
	if (reaction == GRENADE_REACT_DIVE)
		reaction = GRENADE_REACT_BACK;
	return grenade_begin_move(actor_index, actor, soldier, reaction);
}

/* actions.c, after the game found its way out (he is inside the blast, has
noticed it, is on foot and free to move): MENDICANT_DANGER_STOCK the game's
own, _DIVE the game's dive in the direction written here, _MOVED his own
reaction under way (no dive, no avoid action) */
short mendicant_grenades_react(
	long actor_index,
	boolean *escape_found,
	short *escape_direction,
	real_vector2d *alignment_vector)
{
	struct actor_datum *actor = grenade_actor(actor_index);
	struct grenade_soldier *soldier;
	long time = mendicant_time();
	int reaction;

	if (!actor || actor->danger_zone.danger_type != GRENADE_DANGER_PROJECTILE || actor->input.vehicle_index != NONE ||
		!grenade_his_own(actor_index, actor))
		return _mendicant_danger_stock; /* (a driver: the game's own) */
	soldier = grenade_soldier_get(actor_index);
	if (!soldier)
		return _mendicant_danger_stock;
	if (soldier->object != actor->danger_zone.object_index)
		grenade_new_object(actor_index, actor, soldier);

	switch (soldier->kind)
	{
	case GRENADE_REACT_NONE:
		break;
	case GRENADE_REACT_FREEZE:
	case GRENADE_REACT_RUN:
	case GRENADE_REACT_BACK:
	case GRENADE_REACT_COWARD:
		return grenade_continue_move(actor_index, actor, soldier, TRUE) ? _mendicant_danger_moved : _mendicant_danger_stock;
	case GRENADE_REACT_DIVE:
		if (soldier->dive_time == time)
			break;
		/* the dive did not happen (no animation, no room): he runs */
		if (grenade_start_move(actor_index, actor, soldier, GRENADE_REACT_RUN))
		{
			grenade_reacted(actor_index, actor, soldier, GRENADE_REACT_RUN);
			return _mendicant_danger_moved;
		}
		soldier->kind = GRENADE_REACT_STOCK_DIVE;
		return _mendicant_danger_stock;
	default:
		return _mendicant_danger_stock;
	}

	if (soldier->kind == GRENADE_REACT_NONE)
	{
		reaction = grenade_choose_reaction(actor_index, actor, soldier->object);
		/* the dive needs a moment to land: too close to the blast, a run is no use */
		if (reaction == GRENADE_REACT_FREEZE && grenade_urgent(actor))
			reaction = GRENADE_REACT_DIVE;
		if (reaction != GRENADE_REACT_DIVE)
			return grenade_begin_move(actor_index, actor, soldier, reaction) ? _mendicant_danger_moved : _mendicant_danger_stock;
		soldier->kind = GRENADE_REACT_DIVE;
	}

	/* the dive: the game's own animations, in his own direction (the game's
	left of an alignment turned so that left is his way out) */
	{
		real unit_dive = 2.5f;
		real dir_x, dir_y;
		real_point3d end;
		struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(actor->meta.unit_index, _object_mask_unit);

		if (unit)
		{
			struct unit_definition *definition = unit_definition_get(unit->definition_index);

			if (definition && definition->unit.dive_distance > 0.5f)
				unit_dive = definition->unit.dive_distance;
		}
		if (!grenade_choose_direction(actor_index, actor, soldier, unit_dive, 0.0f, TRUE, &dir_x, &dir_y, &end))
		{
			/* no room to dive his way: the game's own */
			soldier->kind = GRENADE_REACT_STOCK_DIVE;
			return _mendicant_danger_stock;
		}
		soldier->dir_x = dir_x;
		soldier->dir_y = dir_y;
		soldier->dive_time = time;
		/* his own choice to dive stands in for the variant's dive chance */
		actor->danger_zone.allow_dive_evasion = TRUE;
		grenade_hand_out(soldier->object, dir_x, dir_y, &end);
		alignment_vector->i = dir_y;
		alignment_vector->j = -dir_x;
		*escape_direction = GRENADE_EVADE_LEFT;
		*escape_found = TRUE;
		return _mendicant_danger_dive;
	}
}

/* each decision, after it (both layers): the log's notices and reactions,
the stuck soldier */
void mendicant_grenades_after_decide(
	long actor_index)
{
	struct actor_datum *actor = grenade_actor(actor_index);
	struct grenade_soldier *soldier;
	struct actor_danger_zone const *zone;
	long time = mendicant_time();

	if (!actor || actor->meta.swarm || actor->meta.unit_index == NONE)
		return;
	zone = &actor->danger_zone;
	soldier = grenade_soldier_get(actor_index);
	if (!soldier)
		return;
	soldier->last_position = actor->input.position.body_position;
	soldier->last_time = time;

	/* a plasma grenade stuck to him */
	if (actor->input.delayed_attached_projectile_index != NONE &&
		actor->input.delayed_attached_projectile_index != soldier->stuck_object &&
		actor->input.body_vitality > 0.0f)
		grenade_stuck_start(actor_index, actor, soldier, actor->input.delayed_attached_projectile_index);

	if (zone->danger_type != GRENADE_DANGER_PROJECTILE || zone->attached_to_us)
		return;
	if (soldier->object != zone->object_index)
	{
		/* the layer off, or a grenade first seen in his decision */
		if (grenade_his_own(actor_index, actor))
			return; /* (the perception hook draws it: the next tick) */
		grenade_new_object(actor_index, actor, soldier);
	}
	if (zone->noticed_danger && !soldier->notice_time)
	{
		soldier->notice_time = time;
		grenade_log("gnotice", actor_index, actor, zone->current_distance_from_actor,
			(real)zone->projectile.time_until_explosion, (long)(short)zone->object_index, zone->hostility,
			time - soldier->seen_time, zone->danger_radius);
	}
	if (!zone->noticed_danger || soldier->react_time)
		return;
	/* the game's own reactions (the layer off, or his own fell back to it) */
	{
		struct actor_debug_info *debug = &actor_debug_array[DATUM_INDEX_TO_ABSOLUTE_INDEX(actor_index)];

		if (soldier->kind == GRENADE_REACT_DIVE && soldier->dive_time == time)
			grenade_log("gdive", actor_index, actor, soldier->dir_x, soldier->dir_y, (long)(short)zone->object_index,
				debug->dive_decision, debug->danger_decision, zone->current_distance_from_actor);

		if (debug->dive_decision_time == time && debug->dive_decision == GRENADE_DIVE_SUCCESS)
		{
			if (soldier->kind == GRENADE_REACT_DIVE)
			{
				grenade_reacted(actor_index, actor, soldier, GRENADE_REACT_DIVE);
			}
			else
			{
				soldier->dir_x = soldier->dir_y = 0.0f;
				grenade_reacted(actor_index, actor, soldier, GRENADE_REACT_STOCK_DIVE);
			}
		}
		else if (actor->state.action == _actor_action_avoid && soldier->kind == GRENADE_REACT_NONE)
		{
			grenade_reacted(actor_index, actor, soldier, GRENADE_REACT_STOCK_AVOID);
		}
	}
}

/* each tick after the AI's update: escapes ended, the log's displacements and
stuck runs, the grenade test */
void mendicant_grenades_update(
	long time)
{
	int slot;

	if (!actor_data || !actor_data->valid)
		return;
	for (slot = 0; slot < MENDICANT_MAX_ACTORS; slot++)
	{
		struct grenade_soldier *soldier = &grenade_soldiers[slot];
		struct actor_datum *actor;

		if (soldier->actor == NONE)
			continue;
		actor = grenade_actor(soldier->actor);
		if (soldier->escaping)
		{
			boolean end = !actor || actor->input.body_vitality <= 0.0f || time >= soldier->escape_until;

			if (!end && soldier->stuck_running)
				end = actor->input.delayed_attached_projectile_index == NONE && time - soldier->stuck_time > 5;
			else if (!end)
				end = !grenade_object(soldier->object) || actor->danger_zone.danger_type != GRENADE_DANGER_PROJECTILE ||
					actor->danger_zone.object_index != soldier->object;
			if (end)
			{
				if (actor)
					grenade_go_end(soldier->actor, soldier);
				soldier->escaping = FALSE;
				soldier->stuck_running = FALSE;
			}
		}
		if (soldier->react_time && !soldier->move_logged &&
			(time - soldier->react_time >= GRENADE_MOVE_SAMPLE_TICKS || !actor))
		{
			soldier->move_logged = TRUE;
			grenade_log("gmove", soldier->actor, actor, soldier->last_position.x - soldier->react_position.x,
				soldier->last_position.y - soldier->react_position.y, (long)(short)soldier->object, soldier->kind,
				soldier->react_time, (real)(soldier->last_time - soldier->react_time));
		}
		if (soldier->stuck_object != NONE && !soldier->stuck_logged &&
			(time - soldier->stuck_time >= GRENADE_STUCK_SAMPLE_TICKS || !actor || actor->input.body_vitality <= 0.0f))
		{
			grenade_stuck_log(soldier->actor, actor, soldier);
		}
	}
	if (mendicant_training && mendicant_config.grenade_test > 0.0f && grenade_test_running && !mendicant_episode_over &&
		time >= grenade_test_next)
	{
		grenade_test_next = time + (long)(mendicant_config.grenade_test * 30.0f);
		grenade_test_drop(time);
	}
	if (mendicant_episode_over)
		grenade_test_running = FALSE;
}
