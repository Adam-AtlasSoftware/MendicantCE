/*
MENDICANT_SPACING.C

MENDICANT AI's spacing and threat dispersal (the authentic layer's M6;
docs/mendicant_port.md, "Spacing"): where each soldier with a character
(mendicant_character.c) stands relative to his friends. Hunters, the Flood,
Sentinels, Engineers and turrets have no character and stand exactly as
before, as do actors in a scripted play fight or a command list.

The stock game's only spacing rule is one friend avoidance radius per actor
definition, given to the friends' bodies (never where they are going), worth
at most 10 points of a firing position's score, in the fight, uncover and
avoid searches only. Two soldiers can pick neighbouring positions in the
same tick, and a squad on a ring of the drill ends up a world unit apart.
Here (ai.mendicant.spacing, with the character on):

- S1 personal space (actor_firing_position.c, in place of the friend
  avoidance): the nearest friends' claimed positions (where they stand, or
  where they are going) become avoid points with the soldier's own radius,
  from his kind and caution (a Grunt 1.4 to 2.2 world units, a Jackal 1.8 to
  2.6, a Marine 2.6 to 3.6, an Elite 3.0 to 4.5) and the fight's mood, or the
  room the friend's kind wants when that is more (a Grunt gives his Elite
  room), and a position clear of them scores up to 10 more; also a
  Marine's Chief (0.7 of it), the friendly vehicles in use near him (a
  vehicle draws fire and runs men over) and a friend with a plasma grenade
  stuck to him. The fight, cover, uncover, guard and avoid searches; at most
  28 entries of the game's 32, so its guard search keeps room for its own.
- S2 buddies: two Marines, or two Grunts, near each other who both like
  working in a pair (M0's buddy preference) pair up while both live and stay
  near; a position 0.8 to 1.6 of his radius from his buddy's scores more, so
  pairs stand as fire teams, not as a lattice.
- S3 species: a Jackal scores a position beside a Jackal friend across the
  line to the target, so they form a shield wall; Grunts' small radius keeps
  them in a herd until a grenade scatters them.
- S4 threat: the radius grows with what threatens the squad, perceived only:
  a live grenade he or a friend within 12 world units has noticed (x1.6, a
  Grunt x2), a grenade blast within 8 world units in the last 6 seconds
  (half of that), a target in sight 6 to 20 world units off who carries
  grenades (+0.4), a target with a heavy weapon or in a vehicle (+0.3), at
  most x2.2. It reaches each soldier after his own short delay (up to half a
  second, from his reflex), so a squad does not move in one tick, and falls
  back over 8 to 15 seconds (his caution) as the squad regroups.
- S5: the fight action asks for a position each time the soldier thinks, so
  a grown radius moves the soldiers it crowds at their next thought, each
  after his own delay; S6 (a stuck plasma grenade) is M2's run from friends,
  and the stuck friend is an avoid point here.
- The drill (mendicant_units.c's move_near, mendicant_drill_position): a
  member's candidate positions within his radius of a position already given
  this step, or of a friend's claimed position, are marked down by
  3 x (radius - distance), so a drill's ring no longer stands a world unit
  apart; its free movement to a point goes beside the point when friends
  are already there.

Logged in the fire log (both layers, so the layer off is measured the same
way): "space" each second for each soldier on foot in a fight, and "sstat"
at a fight's end (docs/mendicant_port.md).
*/

#include "cseries.h"
#include "memory/data.h"
#include "math/real_math.h"
#include "game/game.h"
#include "ai/ai.h"
#include "ai/actors.h"
#include "ai/props.h"
#include "ai/actor_definitions.h"
#include "ai/ai_scenario_definitions.h"
#include "objects/objects.h"
#include "units/units.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define SPACING_DANGER_PROJECTILE 2  /* actor_danger_zone.danger_type */
#define SPACING_UPDATE_TICKS 10      /* each soldier's threat and buddy, every third of a second */
#define SPACING_SAMPLE_TICKS 30      /* the log's sample, each second */
#define SPACING_FRIEND_RANGE 30.0f
#define SPACING_FRIENDS 12           /* the nearest friends made avoid points */
#define SPACING_AVOID_LIMIT 28       /* of the game's 32: room for the guard search's own */
#define SPACING_RADIUS_MINIMUM 0.75f /* (a 0 radius is a NaN in the game's score) */
#define SPACING_THREAT_MAXIMUM 2.2f
#define SPACING_EXPLOSIVE_RANGE 12.0f
#define SPACING_BLAST_RANGE 8.0f
#define SPACING_BLAST_TICKS 180
#define SPACING_BLASTS 16
#define SPACING_BUDDY_RANGE 12.0f    /* a buddy is found this near */
#define SPACING_BUDDY_KEEP 20.0f     /* ... and kept while this near */
#define SPACING_VEHICLE_RANGE 15.0f
#define SPACING_VEHICLES 3
#define SPACING_CHIEF_RANGE 20.0f
#define SPACING_STUCK_RADIUS 4.5f
#define SPACING_BONUS_MAXIMUM 30.0f
#define SPACING_CROWD_WEIGHT 10.0f   /* Marines and Elites: clear of his friends by his radius */
#define SPACING_CROWD_JACKAL 7.0f
#define SPACING_CROWD_GRUNT 5.0f     /* (a herd) */
#define SPACING_GRENADE_CATEGORY 3   /* the damage category of a grenade's blast */
#define SPACING_STEP_TICKS 75        /* the longest step apart */
#define SPACING_STEP_STILL 0.02f     /* standing (world units a tick) */
#define SPACING_DIRECTIONS 8
#define SPACING_DRILL_MARK 3.0f      /* a drill candidate's mark per world unit inside his radius of a claim */

#define spacing_minimum(a, b) ((a) < (b) ? (a) : (b))
#define spacing_maximum(a, b) ((a) > (b) ? (a) : (b))
#define spacing_clamp(x, lo, hi) spacing_minimum(spacing_maximum((x), (lo)), (hi))

#define SPACING_KIND_GRUNT(kind) ((kind) >= MENDICANT_KIND_GRUNT_MINOR && (kind) <= MENDICANT_KIND_GRUNT_FLEE)
#define SPACING_KIND_JACKAL(kind) ((kind) == MENDICANT_KIND_JACKAL_MINOR || (kind) == MENDICANT_KIND_JACKAL_MAJOR)
#define SPACING_KIND_ELITE(kind) ((kind) >= MENDICANT_KIND_ELITE_MINOR && (kind) <= MENDICANT_KIND_ELITE_STEALTH)
#define SPACING_KIND_MARINE(kind) ((kind) >= MENDICANT_KIND_MARINE && (kind) <= MENDICANT_KIND_SERGEANT)

/* the threat's parts, as the log's flags */
enum
{
	SPACING_THREAT_LIVE = 1,      /* a live grenade noticed near */
	SPACING_THREAT_BLAST = 2,     /* a grenade went off near, lately */
	SPACING_THREAT_GRENADIER = 4, /* a target in sight with grenades, in range */
	SPACING_THREAT_HEAVY = 8,     /* a target with a heavy weapon or in a vehicle */
	/* (the sample only) */
	SPACING_SAMPLE_POSITION = 16, /* he has a firing position */
	SPACING_SAMPLE_ARRIVED = 32,  /* at his destination; his action x 256, combat status x 8192 above */
};

/* ---------- globals */

/* each soldier's spacing state, by absolute actor index */
static struct spacing_soldier
{
	long actor;          /* the full index this slot is for, NONE for none */
	real threat;         /* his radius's multiplier now (1 none) */
	real threat_peak;    /* where its fall started */
	real rise_to;        /* a rise on its way to him */
	long rise_due;       /* ...reaching him at this tick, 0 none */
	int flags;           /* SPACING_THREAT_* at his last update */
	long buddy;          /* his buddy's actor index, NONE none */
	short position;      /* his firing position at the last sample (both layers) */
	boolean sampled;
	/* a step apart (no firing position, a friend too close) */
	boolean stepping;
	real_point3d step_goal;
	long step_until;
	long step_next;      /* no new step before this tick */
} spacing_soldiers[MENDICANT_MAX_ACTORS];

/* the grenade blasts that hurt someone lately */
static struct spacing_blast
{
	real_point3d point;
	long time;           /* 0 none */
} spacing_blasts[SPACING_BLASTS];
static int spacing_blast_next = 0;

/* this fight's counts, for the log's sstat row */
static struct spacing_stats
{
	long selections;     /* searches given the soldier's own avoid points */
	long points;         /* avoid points given */
	long most;           /* the most entries any search had after ours */
	long over;           /* searches found with more than the game's 32 (never) */
	long bad;            /* bonuses not written: not a finite number in range (never) */
	long bonuses;        /* positions given a buddy or shield-wall bonus */
	long drill_marked;   /* drill candidates marked down */
	long changes;        /* soldiers' firing position changes seen (both layers) */
	long samples;        /* soldier samples (both layers) */
	long pairs;          /* buddy pairs made */
	long steps;          /* steps apart ordered */
	long repicks;        /* positions given up when the threat jumped */
} spacing_stats;

/* a friend near the soldier */
struct spacing_friend
{
	long actor_index;
	struct actor_datum *actor;
	real distance;
};

/* ---------- private code */

static boolean spacing_layer_on(
	void)
{
	return mendicant_active && mendicant_config.character && mendicant_config.spacing;
}

static struct actor_datum *spacing_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static struct object_datum *spacing_object(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return object_try_and_get_and_verify_type(object_index, _object_mask_all);
}

static struct prop_datum *spacing_prop(
	long prop_index)
{
	if (prop_index == NONE || !prop_data || !prop_data->valid)
		return NULL;
	return (struct prop_datum *)datum_try_and_get(prop_data, prop_index);
}

static struct spacing_soldier *spacing_soldier_get(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct spacing_soldier *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return NULL;
	soldier = &spacing_soldiers[slot];
	if (soldier->actor != actor_index)
	{
		memset(soldier, 0, sizeof(*soldier));
		soldier->actor = actor_index;
		soldier->threat = 1.0f;
		soldier->threat_peak = 1.0f;
		soldier->rise_to = 1.0f;
		soldier->buddy = NONE;
		soldier->position = NONE;
	}
	return soldier;
}

/* his slot when it is his (no reset: a slot another actor has taken since
is that actor's, never cleared for an old index) */
static struct spacing_soldier *spacing_soldier_find(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS || spacing_soldiers[slot].actor != actor_index)
		return NULL;
	return &spacing_soldiers[slot];
}

/* the layer spaces him: a character, the layer on, on foot, not in a script's hands */
static boolean spacing_his_own(
	long actor_index,
	struct actor_datum *actor)
{
	if (!spacing_layer_on() || !actor || !mendicant_soldier_has(actor_index))
		return FALSE;
	if (actor->external_orders.playfighting || actor->state.action == _actor_action_obey ||
		actor->meta.swarm || actor->input.vehicle_index != NONE)
		return FALSE;
	return mendicant_soldier_kind(actor_index) != MENDICANT_KIND_NONE;
}

/* his kind as the log numbers it: his own, or with no character his species' */
static int spacing_log_kind(
	long actor_index,
	struct actor_datum *actor)
{
	int kind = mendicant_soldier_kind(actor_index);

	if (kind != MENDICANT_KIND_NONE || !actor)
		return kind;
	return ACTOR_IS_GRUNT(actor->meta.type) ? MENDICANT_KIND_GRUNT_MINOR : ACTOR_IS_JACKAL(actor->meta.type) ? MENDICANT_KIND_JACKAL_MINOR :
		ACTOR_IS_ELITE(actor->meta.type) ? MENDICANT_KIND_ELITE_MINOR : ACTOR_IS_MARINE(actor->meta.type) ? MENDICANT_KIND_MARINE : 0;
}

static boolean spacing_commanded(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	return order_actor[slot] == actor_index || (commanded_time[slot] != 0 && mendicant_time() - commanded_time[slot] < 60);
}

/* an actor's full index from its datum */
static long spacing_full_index(
	struct actor_datum *actor)
{
	return ((long)(unsigned short)actor->identifier << 16) |
		(long)(((char *)actor - (char *)actor_data->data) / actor_data->size);
}

static real spacing_distance_squared(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return dx * dx + dy * dy + dz * dz;
}

static boolean spacing_finite(
	real value)
{
	return value == value && value > -1e30f && value < 1e30f;
}

/* where a friend stands for spacing: the firing position he holds or is going
to, else where free movement takes him, else where he is */
static void spacing_claim_point(
	struct actor_datum *other,
	real_point3d *point)
{
	short index = other->firing_positions.current_position_index;
	struct firing_position_definition *positions;
	int count = 0;
	real_point3d goal;

	*point = other->input.position.body_position;
	if (index == NONE && actor_go_goal(spacing_full_index(other), &goal))
	{
		*point = goal;
		return;
	}
	if (index == NONE || other->meta.encounter_index == NONE)
		return;
	positions = mendicant_encounter_firing_positions(other->meta.encounter_index, &count);
	if (positions && index >= 0 && index < count)
		*point = positions[index].position;
}

/* the living friends on foot near him (his team, not swarms), nearest first,
at most maximum; the counts within 2, 4 and 12 world units of him over all of
them (any of which may be NULL) */
static int spacing_friends(
	long actor_index,
	struct actor_datum *actor,
	real range,
	struct spacing_friend *friends,
	int maximum,
	int *within2,
	int *within4,
	int *within12,
	real *nearest)
{
	real_point3d const *at = &actor->input.position.body_position;
	int index, count = 0, n2 = 0, n4 = 0, n12 = 0;
	real best = -1.0f;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *other = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		long other_index;
		real dx, dy, dz, d2, d;
		int slot;

		if (!other->identifier || other->meta.team_index != actor->meta.team_index || other->meta.swarm ||
			other->meta.unit_index == NONE || other->input.body_vitality <= 0.0f || other->input.vehicle_index != NONE)
			continue;
		other_index = ((long)(unsigned short)other->identifier << 16) | index;
		if (other_index == actor_index)
			continue;
		dx = other->input.position.body_position.x - at->x;
		dy = other->input.position.body_position.y - at->y;
		dz = other->input.position.body_position.z - at->z;
		if (dz * dz > 9.0f)
			continue;
		d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > range * range)
			continue;
		d = (real)sqrt(d2);
		if (best < 0.0f || d < best)
			best = d;
		n2 += d < 2.0f;
		n4 += d < 4.0f;
		n12 += d < 12.0f;
		if (!friends)
			continue;
		/* nearest first, the farthest dropped */
		slot = count < maximum ? count++ : maximum;
		if (slot == maximum && d >= friends[maximum - 1].distance)
			continue;
		if (slot == maximum)
			slot = maximum - 1;
		while (slot > 0 && friends[slot - 1].distance > d)
		{
			friends[slot] = friends[slot - 1];
			slot--;
		}
		friends[slot].actor_index = other_index;
		friends[slot].actor = other;
		friends[slot].distance = d;
	}
	if (within2)
		*within2 = n2;
	if (within4)
		*within4 = n4;
	if (within12)
		*within12 = n12;
	if (nearest)
		*nearest = best;
	return count;
}

/* his radius before the threat: from his kind and caution, and the fight's mood */
static real spacing_base_radius(
	long actor_index)
{
	int kind = mendicant_soldier_kind(actor_index);
	real low = 2.6f, high = 3.6f, radius;

	if (SPACING_KIND_GRUNT(kind))
	{
		low = 1.4f;
		high = 2.2f;
	}
	else if (SPACING_KIND_JACKAL(kind))
	{
		low = 1.8f;
		high = 2.6f;
	}
	else if (SPACING_KIND_ELITE(kind))
	{
		low = 3.0f;
		high = 4.5f;
	}
	radius = low + (high - low) * mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_CAUTION);
	radius *= mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_SPACING);
	return spacing_maximum(radius, SPACING_RADIUS_MINIMUM);
}

/* the distance he keeps from one friend: his own radius, or the room the
friend's kind wants (0.75 of the friend's own radius before the threat, so a
Grunt gives his Elite room) when that is more */
static real spacing_pair_radius(
	real radius,
	long other_index)
{
	struct actor_datum *other = spacing_actor(other_index);

	if (!spacing_his_own(other_index, other))
		return radius;
	return spacing_maximum(radius, 0.75f * spacing_base_radius(other_index));
}

/* what threatens him now (perceived only): the multiplier it calls for, and
the SPACING_THREAT_* flags */
static real spacing_threat_target(
	long actor_index,
	struct actor_datum *actor,
	struct spacing_friend const *friends,
	int friend_count,
	long time,
	int *flags_out)
{
	int flags = 0, index, kind = spacing_log_kind(actor_index, actor);
	real explosive = 0.0f, target;
	struct prop_datum *prop;

	/* a live grenade he or a friend near him has noticed */
	if (actor->danger_zone.danger_type == SPACING_DANGER_PROJECTILE && actor->danger_zone.noticed_danger)
		flags |= SPACING_THREAT_LIVE;
	for (index = 0; index < friend_count && !(flags & SPACING_THREAT_LIVE); index++)
	{
		struct actor_datum *other = friends[index].actor;

		if (friends[index].distance > SPACING_EXPLOSIVE_RANGE)
			break;
		if ((other->danger_zone.danger_type == SPACING_DANGER_PROJECTILE && other->danger_zone.noticed_danger) ||
			other->input.delayed_attached_projectile_index != NONE)
			flags |= SPACING_THREAT_LIVE;
	}
	if (flags & SPACING_THREAT_LIVE)
		explosive = 1.0f;
	/* a blast near him lately */
	for (index = 0; index < SPACING_BLASTS; index++)
	{
		struct spacing_blast const *blast = &spacing_blasts[index];

		if (blast->time && time - blast->time < SPACING_BLAST_TICKS && time >= blast->time &&
			spacing_distance_squared(&blast->point, &actor->input.position.body_position) <
				SPACING_BLAST_RANGE * SPACING_BLAST_RANGE)
		{
			flags |= SPACING_THREAT_BLAST;
			explosive = spacing_maximum(explosive, 0.5f);
			break;
		}
	}
	/* his target, in sight lately */
	prop = spacing_prop(actor->target.target_prop_index);
	if (prop && prop->enemy && !prop->dead && prop->last_visible_time != NONE && time - prop->last_visible_time < 90)
	{
		struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(prop->unit_index, _object_mask_unit);

		if (unit)
		{
			if (prop->distance >= 6.0f && prop->distance <= 20.0f && prop->vehicle_index == NONE &&
				(unit->unit.grenade_counts[0] > 0 || unit->unit.grenade_counts[1] > 0))
				flags |= SPACING_THREAT_GRENADIER;
			if (prop->distance <= 40.0f && (prop->vehicle_index != NONE || mendicant_weapon_class(unit) == 3))
				flags |= SPACING_THREAT_HEAVY;
		}
	}
	target = 1.0f + (SPACING_KIND_GRUNT(kind) ? 1.0f : 0.6f) * explosive +
		((flags & SPACING_THREAT_GRENADIER) ? 0.4f : 0.0f) + ((flags & SPACING_THREAT_HEAVY) ? 0.3f : 0.0f);
	*flags_out = flags;
	return spacing_minimum(target, SPACING_THREAT_MAXIMUM);
}

/* S5: the threat jumped (by 0.4 or more) and a friend is now inside his new
radius: a fighting soldier at his position gives it up at his next thought
(the fight action's position timer, as when his time there runs out), so the
search picks again with the wider radius */
static void spacing_repick(
	long actor_index,
	real jump,
	real radius)
{
	struct actor_datum *actor = spacing_actor(actor_index);
	struct spacing_friend friends[1];

	if (jump < 0.4f || !actor || actor->state.action != _actor_action_fight || spacing_commanded(actor_index) ||
		actor->firing_positions.current_position_index == NONE || !actor->control.path.at_destination)
		return;
	if (!spacing_friends(actor_index, actor, radius, friends, 1, NULL, NULL, NULL, NULL))
		return;
	actor->state.action_data.fight.firing_position_timer = 1;
	spacing_stats.repicks++;
	mendicant_status.spacing_repicks++;
}

/* his multiplier moves toward what the threat calls for: up after his own
delay, down slowly (the regroup) */
static void spacing_threat_step(
	long actor_index,
	struct spacing_soldier *soldier,
	real target,
	long time)
{
	if (target > soldier->threat + 0.01f)
	{
		if (!soldier->rise_due || target > soldier->rise_to + 0.01f)
		{
			if (!soldier->rise_due)
			{
				real reflex = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_REFLEX);

				soldier->rise_due = time + 1 + (long)((1.0f - reflex) * 9.0f +
					mendicant_soldier_rollf(actor_index, 0x5bace001UL) * 6.0f);
			}
			soldier->rise_to = spacing_maximum(soldier->rise_to, target);
		}
		if (time >= soldier->rise_due)
		{
			spacing_repick(actor_index, soldier->rise_to - soldier->threat, spacing_base_radius(actor_index) * soldier->rise_to);
			soldier->threat = soldier->rise_to;
			soldier->threat_peak = soldier->threat;
			soldier->rise_due = 0;
		}
		return;
	}
	soldier->rise_due = 0;
	if (soldier->threat > target)
	{
		real seconds = 8.0f + 7.0f * mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_CAUTION);

		soldier->threat -= (soldier->threat_peak - 1.0f) * (real)SPACING_UPDATE_TICKS / (seconds * 30.0f);
		if (soldier->threat < target)
			soldier->threat = target;
	}
	soldier->rise_to = soldier->threat;
}

/* the buddy groups: Marines with Marines, Grunts with Grunts */
static int spacing_buddy_group(
	int kind)
{
	return SPACING_KIND_MARINE(kind) ? 1 : SPACING_KIND_GRUNT(kind) ? 2 : 0;
}

static boolean spacing_buddy_valid(
	long actor_index,
	struct actor_datum *actor,
	struct spacing_soldier *soldier)
{
	struct actor_datum *buddy;
	struct spacing_soldier *other;

	if (soldier->buddy == NONE)
		return FALSE;
	buddy = spacing_actor(soldier->buddy);
	if (!buddy || buddy->input.body_vitality <= 0.0f || buddy->input.vehicle_index != NONE ||
		buddy->meta.team_index != actor->meta.team_index || !mendicant_soldier_has(soldier->buddy) ||
		spacing_distance_squared(&buddy->input.position.body_position, &actor->input.position.body_position) >
			SPACING_BUDDY_KEEP * SPACING_BUDDY_KEEP)
		return FALSE;
	other = spacing_soldier_find(soldier->buddy);
	return other && other->buddy == actor_index;
}

/* a buddy for him if he has none: the nearest friend of his group without one,
when the two of them like working in a pair */
static void spacing_buddy_update(
	long actor_index,
	struct actor_datum *actor,
	struct spacing_soldier *soldier,
	struct spacing_friend const *friends,
	int friend_count)
{
	int group = spacing_buddy_group(mendicant_soldier_kind(actor_index)), index;
	real like = mendicant_soldier_buddy(actor_index);

	if (spacing_buddy_valid(actor_index, actor, soldier))
		return;
	if (soldier->buddy != NONE)
	{
		struct spacing_soldier *old = spacing_soldier_find(soldier->buddy);

		if (old && old->buddy == actor_index)
			old->buddy = NONE;
		soldier->buddy = NONE;
	}
	if (!group)
		return;
	for (index = 0; index < friend_count; index++)
	{
		long other_index = friends[index].actor_index;
		struct spacing_soldier *other;

		if (friends[index].distance > SPACING_BUDDY_RANGE)
			break;
		if (!spacing_his_own(other_index, friends[index].actor) ||
			spacing_buddy_group(mendicant_soldier_kind(other_index)) != group ||
			(like + mendicant_soldier_buddy(other_index)) * 0.5f < 0.5f)
			continue;
		other = spacing_soldier_get(other_index);
		if (!other || (other->buddy != NONE && spacing_buddy_valid(other_index, friends[index].actor, other)))
			continue;
		other->buddy = actor_index;
		soldier->buddy = other_index;
		spacing_stats.pairs++;
		return;
	}
}

/* a record of the fire log (time,record,actor,kind,quirk,commanded,a,b,c,d,e,f) */
static void spacing_log(
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
		(int)(short)actor_index, spacing_log_kind(actor_index, actor), mendicant_soldier_quirk(actor_index),
		spacing_commanded(actor_index) ? 1 : 0, a, b, c, d, e, f);
}

/* the space sample (both layers): nearest friend, speed, friends within 2,
4 (+1000 x within 12), the threat's flags and his multiplier */
static void spacing_sample(
	long actor_index,
	struct actor_datum *actor,
	struct spacing_soldier *soldier,
	int flags)
{
	int n2 = 0, n4 = 0, n12 = 0;
	real nearest = -1.0f, speed = 0.0f;
	struct object_datum *unit = spacing_object(actor->meta.unit_index);

	spacing_friends(actor_index, actor, SPACING_FRIEND_RANGE, NULL, 0, &n2, &n4, &n12, &nearest);
	if (unit)
		speed = (real)sqrt(unit->object.translational_velocity.i * unit->object.translational_velocity.i +
			unit->object.translational_velocity.j * unit->object.translational_velocity.j);
	spacing_stats.samples++;
	spacing_log("space", actor_index, actor, nearest, speed, n2, n4 + 1000L * n12, flags,
		spacing_his_own(actor_index, actor) ? soldier->threat : 1.0f);
}

/* the soldiers the log and the layer look at: the four species, on foot */
static boolean spacing_soldier_kind(
	struct actor_datum *actor)
{
	return ACTOR_IS_GRUNT(actor->meta.type) || ACTOR_IS_JACKAL(actor->meta.type) || ACTOR_IS_ELITE(actor->meta.type) ||
		ACTOR_IS_MARINE(actor->meta.type);
}

/* eight ways around, 45 degrees apart */
static real const spacing_direction[SPACING_DIRECTIONS][2] =
{
	{ 1.0f, 0.0f }, { 0.70711f, 0.70711f }, { 0.0f, 1.0f }, { -0.70711f, 0.70711f },
	{ -1.0f, 0.0f }, { -0.70711f, -0.70711f }, { 0.0f, -1.0f }, { 0.70711f, -0.70711f },
};

/* his step apart is over (arrived, failed or too long): ended, unless another
owner has given him an order since */
static void spacing_step_end(
	long actor_index,
	struct spacing_soldier *soldier,
	long time)
{
	real_point3d goal;
	int status;

	if (!soldier->stepping)
		return;
	if (!spacing_actor(actor_index))
	{
		soldier->stepping = FALSE;
		return;
	}
	status = actor_go_status(actor_index);
	if (!actor_go_goal(actor_index, &goal) || spacing_distance_squared(&goal, &soldier->step_goal) > 0.04f)
	{
		soldier->stepping = FALSE;
		return;
	}
	/* (still on his way, the layer on and he on foot: it goes on) */
	if (status == GO_STATUS_MOVING && time < soldier->step_until && spacing_layer_on() &&
		spacing_actor(actor_index)->input.vehicle_index == NONE && spacing_actor(actor_index)->input.body_vitality > 0.0f)
		return;
	actor_go_cancel(actor_index);
	soldier->stepping = FALSE;
}

/* A soldier standing with no firing position of his own (the fight with none
in reach, the wait after a search), a friend inside about half his radius:
he steps apart, 1 to 3 world units, away from the friends near him and across
the line to his target rather than toward it, on ground at his height with a
clear line; each after his own pause, at most every 4 to 8 seconds. */
static void spacing_step_apart(
	long actor_index,
	struct actor_datum *actor,
	struct spacing_soldier *soldier,
	struct spacing_friend const *friends,
	int friend_count,
	long time)
{
	real_point3d const *at = &actor->input.position.body_position;
	struct object_datum *unit;
	struct prop_datum *prop;
	real radius, trigger, speed, length, best_score = -1e30f, target_x = 0.0f, target_y = 0.0f;
	short action = actor->state.action;
	int index, best = -1;
	real_point3d goal;

	if (soldier->stepping || time < soldier->step_next || !friend_count)
		return;
	if (action != _actor_action_fight && action != _actor_action_wait && action != _actor_action_guard)
		return;
	if (actor->state.combat_status < 3 || actor->firing_positions.current_position_index != NONE ||
		spacing_commanded(actor_index) || mendicant_convoy_owns(actor_index) || mendicant_grenade_evading(actor_index) ||
		actor_go_status(actor_index) != GO_STATUS_NONE || actor->input.delayed_attached_projectile_index != NONE)
		return;
	radius = mendicant_spacing_radius(actor_index);
	trigger = spacing_maximum(0.55f * radius, 1.0f);
	if (friends[0].distance >= trigger)
		return;
	unit = spacing_object(actor->meta.unit_index);
	if (!unit)
		return;
	speed = (real)sqrt(unit->object.translational_velocity.i * unit->object.translational_velocity.i +
		unit->object.translational_velocity.j * unit->object.translational_velocity.j);
	if (speed >= SPACING_STEP_STILL)
		return;
	prop = spacing_prop(actor->target.target_prop_index);
	if (prop && prop->enemy && !prop->dead)
	{
		target_x = prop->last_perceived_body_position.x - at->x;
		target_y = prop->last_perceived_body_position.y - at->y;
		length = (real)sqrt(target_x * target_x + target_y * target_y);
		if (length > 0.5f)
		{
			target_x /= length;
			target_y /= length;
		}
		else
		{
			target_x = target_y = 0.0f;
		}
	}
	length = spacing_clamp(1.1f * radius - friends[0].distance, 1.0f, 3.0f);
	for (index = 0; index < SPACING_DIRECTIONS; index++)
	{
		real dx = spacing_direction[index][0], dy = spacing_direction[index][1];
		real score = 0.0f, toward;
		real_point3d point, ground;
		int other;

		for (other = 0; other < friend_count; other++)
		{
			real fx, fy, d;

			if (friends[other].distance > 2.0f * radius)
				break;
			fx = at->x - friends[other].actor->input.position.body_position.x;
			fy = at->y - friends[other].actor->input.position.body_position.y;
			d = (real)sqrt(fx * fx + fy * fy);
			if (d < 0.05f)
				continue;
			score += (dx * fx + dy * fy) / d * (2.0f * radius - friends[other].distance);
		}
		/* across the line to his target, not toward it or back */
		toward = dx * target_x + dy * target_y;
		score -= 1.5f * (toward > 0.0f ? toward : -0.5f * toward);
		score += 0.8f * mendicant_soldier_rollf(actor_index, 0x5bace100UL + (unsigned long)index);
		if (score <= best_score)
			continue;
		point.x = at->x + dx * length;
		point.y = at->y + dy * length;
		point.z = at->z + 0.3f;
		if (!mendicant_ground_under(&point, &ground) || ground.z - at->z > 1.0f || at->z - ground.z > 1.2f ||
			mendicant_line_blocked(at, &point) == 1)
			continue;
		best_score = score;
		best = index;
		goal = ground;
	}
	soldier->step_next = time + 120 + (long)(120.0f * mendicant_soldier_rollf(actor_index, 0x5bace0ffUL));
	if (best < 0 || best_score <= 0.0f || actor_go_to(actor_index, &goal, 0.5f, 1) != 1)
		return;
	if (!actor_go_goal(actor_index, &soldier->step_goal))
		return;
	soldier->stepping = TRUE;
	soldier->step_until = time + SPACING_STEP_TICKS;
	spacing_stats.steps++;
	mendicant_status.spacing_steps++;
}

/* ---------- public code */

void mendicant_spacing_reset(
	void)
{
	int index;

	memset(spacing_soldiers, 0, sizeof(spacing_soldiers));
	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		spacing_soldiers[index].actor = NONE;
	memset(spacing_blasts, 0, sizeof(spacing_blasts));
	spacing_blast_next = 0;
	memset(&spacing_stats, 0, sizeof(spacing_stats));
}

void mendicant_spacing_episode_start(
	void)
{
	memset(&spacing_stats, 0, sizeof(spacing_stats));
}

/* the fight's counts, before its episode_end row (both layers) */
void mendicant_spacing_report(
	void)
{
	if (!mendicant_fire_file)
		return;
	/* samples, position changes, searches spaced, the most avoid entries, over 32, bad bonuses */
	fprintf(mendicant_fire_file, "%ld,sstat,-1,0,0,0,%ld,%ld,%ld,%ld,%ld,%ld\n", mendicant_time(),
		spacing_stats.samples, spacing_stats.changes, spacing_stats.selections, spacing_stats.most,
		spacing_stats.over, spacing_stats.bad);
	/* avoid points given, drill candidates marked, bonuses given, buddy pairs made, steps apart, re-picks */
	fprintf(mendicant_fire_file, "%ld,sstat2,-1,0,0,0,%ld,%ld,%ld,%ld,%ld,%ld\n", mendicant_time(),
		spacing_stats.points, spacing_stats.drill_marked, spacing_stats.bonuses, spacing_stats.pairs, spacing_stats.steps,
		spacing_stats.repicks);
}

/* a unit was hurt (mendicant_unit_damaged): a grenade's blast is remembered
where it hurt him, for the threat near it */
void mendicant_spacing_hurt(
	long unit_index,
	short category)
{
	struct object_datum *unit;
	struct spacing_blast *blast;
	long time = mendicant_time();
	int index;

	if (category != SPACING_GRENADE_CATEGORY)
		return;
	unit = spacing_object(unit_index);
	if (!unit)
		return;
	/* one blast hurts several: the same tick near an earlier one is that one */
	for (index = 0; index < SPACING_BLASTS; index++)
	{
		if (spacing_blasts[index].time == time &&
			spacing_distance_squared(&spacing_blasts[index].point, &unit->object.position) < 16.0f)
			return;
	}
	blast = &spacing_blasts[spacing_blast_next];
	spacing_blast_next = (spacing_blast_next + 1) % SPACING_BLASTS;
	blast->point = unit->object.position;
	blast->time = time;
}

/* his spacing radius now (world units), 0 when the layer does not space him */
real mendicant_spacing_radius(
	long actor_index)
{
	struct actor_datum *actor = spacing_actor(actor_index);
	struct spacing_soldier *soldier;

	if (!spacing_his_own(actor_index, actor))
		return 0.0f;
	soldier = spacing_soldier_get(actor_index);
	return spacing_maximum(spacing_base_radius(actor_index) * (soldier ? soldier->threat : 1.0f), SPACING_RADIUS_MINIMUM);
}

/* actor_firing_position.c, in place of the game's friend avoidance: TRUE when
the soldier's own avoid points are in (the game's then skipped); FALSE: the
game's own. friend_radius is his definition's friend avoidance radius. */
boolean mendicant_spacing_avoid_points(
	long actor_index,
	struct firing_position_evaluation_context *evaluation_context,
	real friend_radius)
{
	struct actor_datum *actor = spacing_actor(actor_index);
	struct spacing_friend friends[SPACING_FRIENDS];
	long vehicles[SPACING_VEHICLES];
	int friend_count, vehicle_count = 0, index, kind;
	long count;
	real radius;

	if (!spacing_his_own(actor_index, actor))
		return FALSE;
	switch (evaluation_context->evaluation_mode)
	{
	case _firing_point_evaluation_mode_fight:
	case _firing_point_evaluation_mode_cover:
	case _firing_point_evaluation_mode_uncover:
	case _firing_point_evaluation_mode_guard:
	case _firing_point_evaluation_mode_avoid:
		break;
	default:
		return FALSE;
	}
	count = evaluation_context->avoid_point_count;
	if (count < 0 || count > MAXIMUM_NUMBER_OF_FIRING_POSITION_AVOID_POINTS)
	{
		spacing_stats.over++;
		return FALSE;
	}
	kind = mendicant_soldier_kind(actor_index);
	radius = spacing_maximum(mendicant_spacing_radius(actor_index), friend_radius);
	if (!spacing_finite(radius) || radius < SPACING_RADIUS_MINIMUM)
		radius = SPACING_RADIUS_MINIMUM;
	friend_count = spacing_friends(actor_index, actor, SPACING_FRIEND_RANGE, friends, SPACING_FRIENDS,
		NULL, NULL, NULL, NULL);
	/* the friends where they stand or are going; one stuck with a plasma grenade wider */
	for (index = 0; index < friend_count && count < SPACING_AVOID_LIMIT; index++)
	{
		struct actor_datum *other = friends[index].actor;
		real_point3d point;

		spacing_claim_point(other, &point);
		evaluation_context->avoid_points[count].point = point;
		evaluation_context->avoid_points[count].radius = spacing_pair_radius(radius, friends[index].actor_index);
		count++;
		if (other->input.delayed_attached_projectile_index != NONE && count < SPACING_AVOID_LIMIT)
		{
			evaluation_context->avoid_points[count].point = other->input.position.body_position;
			evaluation_context->avoid_points[count].radius = spacing_maximum(radius, SPACING_STUCK_RADIUS);
			count++;
		}
	}
	/* the friendly vehicles in use near him */
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS && vehicle_count < SPACING_VEHICLES; index++)
	{
		struct actor_datum *other = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		struct object_datum *vehicle;
		int seen;

		if (!other->identifier || other->meta.team_index != actor->meta.team_index || other->input.vehicle_index == NONE ||
			other->input.body_vitality <= 0.0f)
			continue;
		for (seen = 0; seen < vehicle_count && vehicles[seen] != other->input.vehicle_index; seen++)
			;
		if (seen < vehicle_count)
			continue;
		vehicle = spacing_object(other->input.vehicle_index);
		if (!vehicle || spacing_distance_squared(&vehicle->object.position, &actor->input.position.body_position) >
			SPACING_VEHICLE_RANGE * SPACING_VEHICLE_RANGE)
			continue;
		vehicles[vehicle_count++] = other->input.vehicle_index;
	}
	if (SPACING_KIND_MARINE(kind))
	{
		long units[MENDICANT_MAX_PLAYERS];
		int players = mendicant_player_units(units, MENDICANT_MAX_PLAYERS), player;

		for (player = 0; player < players; player++)
		{
			struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(units[player], _object_mask_unit);
			struct object_datum *object = spacing_object(units[player]);

			if (!unit || !object ||
				spacing_distance_squared(&object->object.position, &actor->input.position.body_position) >
					SPACING_CHIEF_RANGE * SPACING_CHIEF_RANGE)
				continue;
			/* the Chief, or the vehicle he drives */
			if (object->object.parent_object_index != NONE)
			{
				int seen;

				for (seen = 0; seen < vehicle_count && vehicles[seen] != object->object.parent_object_index; seen++)
					;
				if (seen == vehicle_count && vehicle_count < SPACING_VEHICLES && spacing_object(object->object.parent_object_index))
					vehicles[vehicle_count++] = object->object.parent_object_index;
				continue;
			}
			if (count < SPACING_AVOID_LIMIT)
			{
				evaluation_context->avoid_points[count].point = object->object.position;
				evaluation_context->avoid_points[count].radius = spacing_maximum(0.7f * radius, SPACING_RADIUS_MINIMUM);
				count++;
			}
		}
	}
	for (index = 0; index < vehicle_count && count < SPACING_AVOID_LIMIT; index++)
	{
		struct object_datum *vehicle = spacing_object(vehicles[index]);

		if (!vehicle)
			continue;
		evaluation_context->avoid_points[count].point = vehicle->object.position;
		evaluation_context->avoid_points[count].radius =
			spacing_clamp(vehicle->object.bounding_sphere_radius, 1.0f, 4.0f) + 0.5f * radius;
		count++;
	}
	spacing_stats.points += count - evaluation_context->avoid_point_count;
	spacing_stats.selections++;
	mendicant_status.spacing_selections++;
	evaluation_context->avoid_point_count = count;
	if (count > spacing_stats.most)
		spacing_stats.most = count;
	return TRUE;
}

/* actor_firing_position.c, after the pre-evaluators: the buddy's and the
shield wall's bonuses and the clearance from his friends on the valid candidates
(each in 0..30, finite) */
void mendicant_spacing_bonus(
	long actor_index,
	struct firing_position_evaluation_context *evaluation_context,
	short firing_position_count,
	struct firing_position *firing_positions)
{
	struct actor_datum *actor = spacing_actor(actor_index);
	struct spacing_soldier *soldier;
	struct spacing_friend friends[SPACING_FRIENDS];
	real_point3d buddy_point, walls[4];
	real wall_axis[4][2];
	real_point3d crowd[SPACING_FRIENDS];
	real crowd_r2[SPACING_FRIENDS];
	real radius, buddy_weight = 0.0f, low2, high2, crowd_weight;
	int wall_count = 0, index, kind, crowd_count;
	boolean has_buddy = FALSE;

	if (!spacing_his_own(actor_index, actor) || firing_position_count <= 0 || !firing_positions)
		return;
	switch (evaluation_context->evaluation_mode)
	{
	case _firing_point_evaluation_mode_fight:
	case _firing_point_evaluation_mode_cover:
	case _firing_point_evaluation_mode_uncover:
	case _firing_point_evaluation_mode_guard:
		break;
	default:
		return;
	}
	soldier = spacing_soldier_get(actor_index);
	kind = mendicant_soldier_kind(actor_index);
	radius = mendicant_spacing_radius(actor_index);
	if (!soldier || !spacing_finite(radius) || radius < SPACING_RADIUS_MINIMUM)
		return;
	if (spacing_buddy_valid(actor_index, actor, soldier))
	{
		struct actor_datum *buddy = spacing_actor(soldier->buddy);

		spacing_claim_point(buddy, &buddy_point);
		buddy_weight = 4.0f * mendicant_soldier_buddy(actor_index);
		has_buddy = buddy_weight > 0.0f;
	}
	/* his friends where they stand or are going: a position clear of them by
	his radius scores up to crowd_weight more (a penalty kept as a missing bonus) */
	crowd_count = spacing_friends(actor_index, actor, SPACING_FRIEND_RANGE, friends, SPACING_FRIENDS, NULL, NULL, NULL, NULL);
	for (index = 0; index < crowd_count; index++)
	{
		real pair = spacing_pair_radius(radius, friends[index].actor_index);

		spacing_claim_point(friends[index].actor, &crowd[index]);
		crowd_r2[index] = pair * pair;
	}
	crowd_weight = SPACING_KIND_GRUNT(kind) ? SPACING_CROWD_GRUNT : SPACING_KIND_JACKAL(kind) ? SPACING_CROWD_JACKAL :
		SPACING_CROWD_WEIGHT;
	if (SPACING_KIND_JACKAL(kind) && evaluation_context->has_target)
	{
		int friend_count = spacing_friends(actor_index, actor, 6.0f, friends, SPACING_FRIENDS, NULL, NULL, NULL, NULL);

		for (index = 0; index < friend_count && wall_count < 4; index++)
		{
			real x, y, length;

			if (!ACTOR_IS_JACKAL(friends[index].actor->meta.type))
				continue;
			spacing_claim_point(friends[index].actor, &walls[wall_count]);
			x = evaluation_context->target_point.x - walls[wall_count].x;
			y = evaluation_context->target_point.y - walls[wall_count].y;
			length = (real)sqrt(x * x + y * y);
			if (length < 1.0f)
				continue;
			wall_axis[wall_count][0] = x / length;
			wall_axis[wall_count][1] = y / length;
			wall_count++;
		}
	}
	if (!has_buddy && !wall_count && !crowd_count)
		return;
	low2 = 0.8f * radius * 0.8f * radius;
	high2 = 1.6f * radius * 1.6f * radius;
	for (index = 0; index < firing_position_count; index++)
	{
		struct firing_position *position = &firing_positions[index];
		real_point3d const *at;
		real bonus = 0.0f;
		int wall;

		if (!position->valid || !position->definition)
			continue;
		at = &position->definition->position;
		if (crowd_count)
		{
			real nearest = 1.0f;
			int other;

			/* the nearest friend for his room, d^2 / r^2 */
			for (other = 0; other < crowd_count; other++)
			{
				real ratio = spacing_distance_squared(at, &crowd[other]) / crowd_r2[other];

				if (ratio < nearest)
					nearest = ratio;
			}
			bonus += crowd_weight * (real)sqrt(nearest);
		}
		if (has_buddy)
		{
			real d2 = spacing_distance_squared(at, &buddy_point);

			if (d2 >= low2 && d2 <= high2)
				bonus += buddy_weight;
		}
		for (wall = 0; wall < wall_count; wall++)
		{
			real x = at->x - walls[wall].x, y = at->y - walls[wall].y;
			real along = x * wall_axis[wall][0] + y * wall_axis[wall][1];
			real across = (real)fabs(x * wall_axis[wall][1] - y * wall_axis[wall][0]);

			if (across >= 1.5f && across <= 3.0f && (real)fabs(along) < 1.0f && (real)fabs(at->z - walls[wall].z) < 1.0f)
			{
				bonus += 3.0f;
				break;
			}
		}
		if (bonus <= 0.0f)
			continue;
		if (!spacing_finite(bonus) || bonus > SPACING_BONUS_MAXIMUM || !spacing_finite(position->evaluation))
		{
			spacing_stats.bad++;
			continue;
		}
		position->evaluation += bonus;
		spacing_stats.bonuses++;
	}
}

/* the drill's positions (mendicant_units.c's move_near,
mendicant_drill_position): the points a member keeps his radius from, the
positions given this step (used[p] == his encounter) and his friends' claimed
positions; FALSE when the layer does not space him (no marks) */
boolean mendicant_spacing_claims_begin(
	long actor_index,
	struct actor_datum *actor,
	long const *used,
	struct firing_position_definition const *positions,
	int position_count,
	struct mendicant_spacing_claims *claims)
{
	struct spacing_friend friends[SPACING_FRIENDS];
	int index, friend_count;
	long encounter_index;

	claims->count = 0;
	claims->radius = 0.0f;
	if (!actor || !spacing_his_own(actor_index, actor))
		return FALSE;
	claims->radius = mendicant_spacing_radius(actor_index);
	if (claims->radius < SPACING_RADIUS_MINIMUM || !spacing_finite(claims->radius))
		return FALSE;
	encounter_index = actor->meta.encounter_index;
	for (index = 0; used && positions && index < position_count && claims->count < MENDICANT_SPACING_CLAIMS / 2; index++)
	{
		if (used[index] == encounter_index &&
			spacing_distance_squared(&positions[index].position, &actor->input.position.body_position) < 1600.0f)
		{
			claims->point[claims->count] = positions[index].position;
			claims->room[claims->count++] = claims->radius;
		}
	}
	friend_count = spacing_friends(actor_index, actor, 40.0f, friends, SPACING_FRIENDS, NULL, NULL, NULL, NULL);
	for (index = 0; index < friend_count && claims->count < MENDICANT_SPACING_CLAIMS; index++)
	{
		spacing_claim_point(friends[index].actor, &claims->point[claims->count]);
		claims->room[claims->count++] = spacing_pair_radius(claims->radius, friends[index].actor_index);
	}
	return claims->count > 0;
}

/* a drill candidate's mark: 3 x (radius - distance) for each claim within its
radius (score units), at most 6 x his radius */
real mendicant_spacing_claims_penalty(
	struct mendicant_spacing_claims const *claims,
	real_point3d const *point)
{
	real penalty = 0.0f;
	int index;

	for (index = 0; index < claims->count; index++)
	{
		real d2 = spacing_distance_squared(point, &claims->point[index]), room = claims->room[index];

		if (d2 < room * room)
			penalty += SPACING_DRILL_MARK * (room - (real)sqrt(d2));
	}
	if (penalty > 0.0f)
		spacing_stats.drill_marked++;
	return spacing_minimum(penalty, 2.0f * SPACING_DRILL_MARK * claims->radius);
}

/* the drill's free movement to a point (mendicant_go_fallback): for a member
the layer spaces, the point or one of sixteen around it at his radius and 1.6 times it, the
one least near his friends' claimed positions (the point itself on a tie) */
void mendicant_spacing_fallback_point(
	long actor_index,
	struct actor_datum *actor,
	real_point3d *point)
{
	struct mendicant_spacing_claims claims;
	real best_penalty, radius;
	real_point3d best;
	int index;

	if (!mendicant_spacing_claims_begin(actor_index, actor, NULL, NULL, 0, &claims))
		return;
	radius = claims.radius;
	best = *point;
	best_penalty = mendicant_spacing_claims_penalty(&claims, point);
	for (index = 0; index < 2 * SPACING_DIRECTIONS && best_penalty > 0.0f; index++)
	{
		real_point3d candidate, ground;
		real penalty, out = index < SPACING_DIRECTIONS ? radius : 1.6f * radius;

		candidate.x = point->x + spacing_direction[index % SPACING_DIRECTIONS][0] * out;
		candidate.y = point->y + spacing_direction[index % SPACING_DIRECTIONS][1] * out;
		candidate.z = point->z + 0.3f;
		penalty = mendicant_spacing_claims_penalty(&claims, &candidate);
		if (penalty >= best_penalty - 0.01f || !mendicant_ground_under(&candidate, &ground) ||
			(real)fabs(ground.z - point->z) > 1.2f || mendicant_line_blocked(point, &candidate) == 1)
			continue;
		best_penalty = penalty;
		best = ground;
	}
	*point = best;
}

/* each tick after the AI's update: each soldier's threat and buddy (the layer
on), and the log's samples (both layers) */
void mendicant_spacing_update(
	long time)
{
	boolean on = spacing_layer_on();
	int index;

	if (!actor_data || !actor_data->valid)
		return;
	/* the steps apart end here, whatever became of the soldier (an arrived
	order would otherwise hold him at its point) */
	if ((time % SPACING_UPDATE_TICKS) == 0)
	{
		for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		{
			if (spacing_soldiers[index].stepping)
				spacing_step_end(spacing_soldiers[index].actor, &spacing_soldiers[index], time);
		}
	}
	if (!on && !mendicant_fire_file)
		return;
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		struct spacing_friend friends[SPACING_FRIENDS];
		struct spacing_soldier *soldier;
		long actor_index;
		boolean update = (time % SPACING_UPDATE_TICKS) == (index % SPACING_UPDATE_TICKS);
		boolean sample = mendicant_fire_file && (time % SPACING_SAMPLE_TICKS) == (index % SPACING_SAMPLE_TICKS);
		int friend_count, flags = 0;
		real target;

		if (!update && !sample)
			continue;
		if (!actor->identifier || actor->meta.swarm || actor->meta.dormant || actor->meta.unit_index == NONE ||
			actor->input.body_vitality <= 0.0f || actor->input.vehicle_index != NONE || !spacing_soldier_kind(actor))
			continue;
		actor_index = ((long)(unsigned short)actor->identifier << 16) | index;
		soldier = spacing_soldier_get(actor_index);
		if (!soldier)
			continue;
		if (update)
		{
			friend_count = spacing_friends(actor_index, actor, SPACING_FRIEND_RANGE, friends, SPACING_FRIENDS,
				NULL, NULL, NULL, NULL);
			target = spacing_threat_target(actor_index, actor, friends, friend_count, time, &flags);
			soldier->flags = flags;
			if (spacing_his_own(actor_index, actor))
			{
				spacing_threat_step(actor_index, soldier, target, time);
				spacing_step_apart(actor_index, actor, soldier, friends, friend_count, time);
				if ((time / SPACING_UPDATE_TICKS) % 3 == 0)
					spacing_buddy_update(actor_index, actor, soldier, friends, friend_count);
			}
			else
			{
				soldier->threat = 1.0f;
				soldier->threat_peak = 1.0f;
				soldier->rise_to = 1.0f;
				soldier->rise_due = 0;
			}
		}
		if (sample && actor->state.combat_status >= 2)
		{
			short position = actor->firing_positions.current_position_index;
			int extra = (position != NONE ? SPACING_SAMPLE_POSITION : 0) |
				(actor->control.path.at_destination ? SPACING_SAMPLE_ARRIVED : 0) |
				((int)spacing_clamp(actor->state.action, 0, 31) << 8) |
				((int)spacing_clamp(actor->state.combat_status, 0, 7) << 13);

			if (soldier->sampled && position != NONE && position != soldier->position)
				spacing_stats.changes++;
			soldier->position = position;
			soldier->sampled = TRUE;
			spacing_sample(actor_index, actor, soldier, soldier->flags | extra);
		}
	}
}
