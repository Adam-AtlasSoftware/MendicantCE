/*
MENDICANT_FIRING.C

MENDICANT AI's firing personality (the authentic layer's M1; docs/
mendicant_port.md, "The firing personality"): how each soldier with a
character (mendicant_character.c) aims and fires, read from his traits, the
fight's mood and what is happening to him. Hunters, the Flood, Sentinels,
Engineers and turrets have no character and fire as before.

Everything is written to the actor's own control fields as the game starts a
burst, a pause or the wait before a first burst (actor_combat.c's source
hooks), never to the shared firing variant tags: the one tag write left (a
marksman's charged plasma pistol, mendicant_combat.c) is put back as soon as
his combat update ends.

- Accuracy: each burst's error and the walk of its tracers onto the target
  scale with his skill, how long he has had the target (after the game's own
  new target spread, not on top of it), how hard he is being shot at against
  his nerve, his wounds, a target too close for comfort, and the mood.
- Reaction: the wait before a first burst is his own (reflex, mood, being
  shot at). Commanded soldiers, whose orders skip the game's wait, get it too,
  so an ordered squad no longer opens fire on one tick; an ambush ripples.
- Bursts: length and pause rhythm from his discipline (long and erratic to
  short and even), a magazine dump when frightened, short bursts on the move
  for Marines and Grunts and full ones for an aggressive Elite; the Marines'
  longer assault rifle bursts (ai.mendicant.marine_fire) are each Marine's
  own around the same mean.
- A pause before switching to a new target while the old one is still up, a
  Marine's beat to confirm a kill, and a flinch when a hit lands (capped, so
  steady fire cannot lock a soldier out of shooting).
- Friends in the line: a friendly ground vehicle with anyone aboard blocks
  the line of fire as a soldier on foot does (ai.mendicant.friend_vehicles).
- Search for a lost target (ai.mendicant.lost_target_search): six seconds
  after an encounter loses sight of its enemy, an Elite calls the search, up
  to two Grunts go to where the enemy was last seen, and Jackals (and steady
  Marines) hold their aim on it; a Jackal sharpshooter punishes the re-peek.

Every multiplier made from a trait has the mean 1 over the soldier's kind
(mendicant_soldier_mult), so the kind's averages stay where the layer off
has them; under a unit's command the spread is compressed (cohesion).
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "ai/ai.h"
#include "ai/actors.h"
#include "ai/actor_definitions.h"
#include "ai/actions.h"
#include "ai/props.h"
#include "ai/ai_communication.h"
#include "objects/objects.h"
#include "units/units.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define FIRING_TICKS 30.0f
/* a fire target unseen this long before a first burst is a new one */
#define FIRING_NEW_TARGET_TICKS 90
/* the settling penalty at most inside the game's new target window, which
already doubles the error (the layer off stacks its 1.4 on it; 1.2 halves
that and keeps each kind's mean within the balance band) */
#define FIRING_NEW_TARGET_SETTLE 1.2f
/* an ordered soldier is already aimed in: this share of his own reaction
(it keeps the cost of the staggered opening within the balance band) */
#define FIRING_COMMANDED_REACTION 0.75f
/* each family's aim, so that burst for burst its error keeps the layer off's
mean (lower than 1: more accurate). The factors below that are not traits
(settling inside the game's new target window, wounds, a target too close)
do not average out: measured over the trainer's frozen benchmark (50 fights
on a30, a50, b30 and c40, 3,800-6,500 bursts a family), the Marines' error
came out 9% above the layer off's and the Covenant's 2-6% below, which moved
the Marines' kills down by 20%. With these and the shake under fire halved
(below), each family's damage, kills and deaths came back within the band
(docs/mendicant_port.md, "The firing personality"). */
#define FIRING_AIM_GRUNT 1.033f
#define FIRING_AIM_JACKAL 1.059f
#define FIRING_AIM_ELITE 1.024f
#define FIRING_AIM_MARINE 0.917f
/* the longest wait before a first burst, a hesitation, a flinch (seconds) */
#define FIRING_REACTION_MAXIMUM 1.2f
#define FIRING_HESITATION_MAXIMUM 0.6f
/* a flinch: at least this long apart, and at most two in five seconds */
#define FIRING_FLINCH_COOLDOWN_TICKS 45
#define FIRING_FLINCH_WINDOW_TICKS 150
/* a burst no longer than this, unless the game's own was (ticks) */
#define FIRING_BURST_MAXIMUM_TICKS 90
/* friendly vehicles that block the line of fire: in reach, ground vehicles only */
#define FIRING_VEHICLE_RANGE 25.0f
#define FIRING_VEHICLE_RADIUS_MAXIMUM 4.0f
/* the search for a lost target */
#define FIRING_SEARCH_UPDATE_TICKS 10
#define FIRING_SEARCH_AFTER_TICKS 180    /* unseen this long: search */
#define FIRING_SEARCH_GIVE_UP_TICKS 900  /* unseen longer: the game's own search */
#define FIRING_SEARCH_LENGTH_TICKS 600
#define FIRING_SEARCH_REST_TICKS 600     /* between two searches of one encounter */
#define FIRING_SEARCH_STOP_DISTANCE 6.0f
#define FIRING_ANGLE_RANGE 30.0f         /* a held angle pre-aims within this */
#define FIRING_ANGLE_RADIUS 3.0f         /* ...on this spot */

#define firing_minimum(a, b) ((a) < (b) ? (a) : (b))
#define firing_maximum(a, b) ((a) > (b) ? (a) : (b))
#define firing_clamp(x, lo, hi) firing_minimum(firing_maximum((x), (lo)), (hi))

/* the kinds by species */
#define FIRING_KIND_GRUNT(kind) ((kind) >= MENDICANT_KIND_GRUNT_MINOR && (kind) <= MENDICANT_KIND_GRUNT_FLEE)
#define FIRING_KIND_JACKAL(kind) ((kind) == MENDICANT_KIND_JACKAL_MINOR || (kind) == MENDICANT_KIND_JACKAL_MAJOR)
#define FIRING_KIND_ELITE(kind) ((kind) >= MENDICANT_KIND_ELITE_MINOR && (kind) <= MENDICANT_KIND_ELITE_STEALTH)
#define FIRING_KIND_MARINE(kind) ((kind) >= MENDICANT_KIND_MARINE && (kind) <= MENDICANT_KIND_SERGEANT)

/* ---------- globals */

/* each soldier's firing state, by absolute actor index */
static struct firing_soldier
{
	long actor;            /* the full index this slot is for, NONE for none */
	long fired_prop;       /* the prop of his last burst */
	long last_burst_time;
	long hold_prop;        /* the fire target last seen by mendicant_fire_held */
	long damage_time;
	real damage;           /* recent body and shield damage then */
	long flinch_until, flinch_next, flinch_times[2];
	real near_miss;        /* the memory of rounds coming close (0..0.8) */
	long near_miss_time;
	real moving_scale;     /* the last burst's on-the-move scale, also on the pause after it */
	unsigned long draws;   /* a counter salting his own random numbers */
	long angle_until;      /* holding his aim on where a lost enemy was */
	long preaim_until;     /* a re-peek there within this is met at once */
	real_point3d angle_point;
} firing_soldiers[MENDICANT_MAX_ACTORS];

/* each encounter's search for a lost target, by absolute encounter index */
static struct firing_search
{
	boolean active;
	long since, ended, lost_time;
	real_point3d point;
	long movers[2];
} firing_searches[MENDICANT_MAX_ENCOUNTERS];
static long firing_search_time;
/* how often a friendly vehicle was put in a line of fire test (the log) */
static long firing_vehicle_pills, firing_vehicle_tests, firing_vehicle_seen, firing_vehicle_large;

/* ---------- private code */

static boolean firing_on(
	void)
{
	return mendicant_active && mendicant_config.character && mendicant_config.fire_personality;
}

static struct actor_datum *firing_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static struct prop_datum *firing_prop(
	long prop_index)
{
	if (prop_index == NONE || !prop_data || !prop_data->valid)
		return NULL;
	return (struct prop_datum *)datum_try_and_get(prop_data, prop_index);
}

static struct unit_datum *firing_unit(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_all);
}

/* his firing state, for a soldier with a character (the slot started again
for a new actor in it); NULL for anyone else */
static struct firing_soldier *firing_soldier_get(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct firing_soldier *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS || !mendicant_soldier_has(actor_index))
		return NULL;
	soldier = &firing_soldiers[slot];
	if (soldier->actor != actor_index)
	{
		memset(soldier, 0, sizeof(*soldier));
		soldier->actor = actor_index;
		soldier->fired_prop = NONE;
		soldier->hold_prop = NONE;
		soldier->last_burst_time = -100000;
		soldier->damage_time = -100000;
		soldier->flinch_times[0] = soldier->flinch_times[1] = -100000;
		soldier->moving_scale = 1.0f;
	}
	return soldier;
}

/* his own number in 0..1, a new one at each call */
static real firing_random(
	long actor_index,
	struct firing_soldier *soldier)
{
	return mendicant_soldier_rollf(actor_index, 0x6f1e0000UL + (soldier->draws++ & 0xffffUL));
}

static real firing_lerp(
	real a,
	real b,
	real t)
{
	return a + (b - a) * firing_clamp(t, 0.0f, 1.0f);
}

/* a soldier under a unit's command: its orders this drill update */
static boolean firing_commanded(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	if (slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	return order_actor[slot] == actor_index || (commanded_time[slot] != 0 && mendicant_time() - commanded_time[slot] < 60);
}

/* a multiplier's spread pulled toward 1 under command */
static real firing_blend(
	real multiplier,
	real cohesion)
{
	return 1.0f + (multiplier - 1.0f) * cohesion;
}

/* how hard he is being shot at, 0..1: the game's own smoothed danger, and a
short memory of rounds coming close */
static real firing_suppression(
	struct actor_datum *actor,
	struct firing_soldier *soldier)
{
	long time = mendicant_time();
	real danger = firing_clamp(actor->emotions.perceived_danger * 0.5f, 0.0f, 1.0f);
	real memory = soldier->near_miss - 0.5f * (time - soldier->near_miss_time) / FIRING_TICKS;

	if (memory < 0.0f)
		memory = 0.0f;
	if (actor->emotions.instantaneous_danger >= 1.2f && memory < 0.8f)
		memory = 0.8f;
	soldier->near_miss = memory;
	soldier->near_miss_time = time;
	return firing_maximum(danger, memory);
}

static boolean firing_shields_up(
	struct actor_datum *actor)
{
	return actor->input.shield_vitality >= 0.05f;
}

/* a record of the firing log (docs/mendicant_port.md, "The firing personality") */
static void firing_log(
	char const *record,
	long actor_index,
	real a,
	real b,
	long c,
	long d,
	long e,
	real f)
{
	if (!mendicant_fire_file)
		return;
	fprintf(mendicant_fire_file, "%ld,%s,%d,%d,%d,%d,%.4f,%.4f,%ld,%ld,%ld,%.4f\n",
		mendicant_time(), record, (int)(short)actor_index, mendicant_soldier_kind(actor_index),
		mendicant_soldier_quirk(actor_index), firing_commanded(actor_index) ? 1 : 0, a, b, c, d, e, f);
}

/* the firing log for a soldier whose firing is not his own (the layer off,
for comparison): his species as a kind, the layer's aim and the length */
static void firing_log_stock(
	char const *record,
	long actor_index,
	long ticks)
{
	struct actor_datum *actor;
	int kind;

	if (!mendicant_fire_file || (actor = firing_actor(actor_index)) == NULL)
		return;
	kind = ACTOR_IS_GRUNT(actor->meta.type) ? MENDICANT_KIND_GRUNT_MINOR : ACTOR_IS_JACKAL(actor->meta.type) ? MENDICANT_KIND_JACKAL_MINOR :
		ACTOR_IS_ELITE(actor->meta.type) ? MENDICANT_KIND_ELITE_MINOR : ACTOR_IS_MARINE(actor->meta.type) ? MENDICANT_KIND_MARINE : 0;
	fprintf(mendicant_fire_file, "%ld,%s,%d,%d,0,%d,%.4f,%.4f,%ld,%ld,%d,1.0\n", mendicant_time(), record, (int)(short)actor_index, kind,
		firing_commanded(actor_index) ? 1 : 0, mendicant_aim_stock_factor(actor_index), mendicant_aim_stock_factor(actor_index),
		ticks, ticks, actor->control.firing_while_moving ? 1 : 0);
}

/* ---------- private code: the search for a lost target */

static void firing_search_end(
	int encounter)
{
	struct firing_search *search = &firing_searches[encounter];
	int index;

	for (index = 0; index < 2; index++)
	{
		long mover = search->movers[index];
		int status;

		if (mover == NONE)
			continue;
		status = actor_go_status(mover);
		/* (a mover the drill has taken since is the drill's) */
		if ((status == GO_STATUS_MOVING || status == GO_STATUS_ARRIVED) && !firing_commanded(mover))
			actor_go_cancel(mover);
		search->movers[index] = NONE;
	}
	search->active = FALSE;
	search->ended = mendicant_time();
	firing_log("search_end", encounter, 0.0f, 0.0f, 0, 0, 0, 0.0f);
}

static void firing_search_start(
	int encounter,
	long time)
{
	struct firing_search *search = &firing_searches[encounter];
	long grunts[2] = { NONE, NONE };
	real grunt_courage[2] = { -1.0f, -1.0f };
	long elite = NONE;
	int index, holders = 0, movers = 0;

	search->active = TRUE;
	search->since = time;
	search->movers[0] = search->movers[1] = NONE;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		long actor_index;
		int kind;
		struct firing_soldier *soldier;
		real dx, dy, dz, distance;

		if (!actor->identifier || actor->meta.encounter_index == NONE ||
			(int)(unsigned short)actor->meta.encounter_index != encounter)
			continue;
		actor_index = ((long)(unsigned short)actor->identifier << 16) | index;
		soldier = firing_soldier_get(actor_index);
		if (!soldier || actor->meta.dormant || actor->meta.swarm || actor->input.body_vitality <= 0.0f ||
			actor->input.vehicle_index != NONE || actor->state.action == _actor_action_flee)
			continue;
		kind = mendicant_soldier_kind(actor_index);
		dx = search->point.x - actor->input.position.body_position.x;
		dy = search->point.y - actor->input.position.body_position.y;
		dz = search->point.z - actor->input.position.body_position.z;
		distance = (real)sqrt(dx * dx + dy * dy + dz * dz);

		if (FIRING_KIND_ELITE(kind))
		{
			if (elite == NONE)
				elite = actor_index;
			soldier->angle_until = time + 90; /* he looks (points) where the enemy went */
			soldier->angle_point = search->point;
			soldier->angle_point.z += 0.6f;
		}
		else if (FIRING_KIND_JACKAL(kind) && distance < 40.0f)
		{
			boolean sharpshooter = mendicant_soldier_quirk(actor_index) == MENDICANT_QUIRK_SHARPSHOOTER;

			soldier->angle_until = time + (sharpshooter ? 180 : 120);
			soldier->angle_point = search->point;
			soldier->angle_point.z += 0.6f;
			holders++;
		}
		else if (FIRING_KIND_MARINE(kind) && distance < 40.0f &&
			mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE) > 0.55f)
		{
			soldier->angle_until = time + 90;
			soldier->angle_point = search->point;
			soldier->angle_point.z += 0.6f;
			holders++;
		}
		else if (FIRING_KIND_GRUNT(kind) && kind != MENDICANT_KIND_GRUNT_FLEE &&
			order_actor[index] != actor_index && actor_role[index] == ROLE_NONE &&
			distance > FIRING_SEARCH_STOP_DISTANCE + 1.0f && distance < 40.0f && actor_go_status(actor_index) <= GO_STATUS_NONE)
		{
			real courage = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_COURAGE);
			int slot = grunt_courage[0] <= grunt_courage[1] ? 0 : 1;

			if (courage > grunt_courage[slot])
			{
				grunts[slot] = actor_index;
				grunt_courage[slot] = courage;
			}
		}
	}

	if (elite != NONE)
	{
		struct actor_datum *actor = firing_actor(elite);

		if (actor)
			ai_communication_event(_ai_communication_search_start, actor->meta.unit_index, NONE, NONE, NONE, NONE, NULL);
	}
	/* the Grunts go to where the enemy was last seen, the two bravest with an
	Elite to answer to, only a brave one without */
	for (index = 0; index < 2; index++)
	{
		if (grunts[index] == NONE || (elite == NONE && grunt_courage[index] <= 0.5f))
			continue;
		if (actor_go_to(grunts[index], &search->point, FIRING_SEARCH_STOP_DISTANCE, 0) == 1)
			search->movers[movers++] = grunts[index];
	}
	firing_log("search_start", encounter, (time - search->lost_time) / FIRING_TICKS, 0.0f,
		elite != NONE ? 1 : 0, movers, holders, 0.0f);
}

static void firing_search_update(
	long time)
{
	static boolean sees[MENDICANT_MAX_ENCOUNTERS], engaged[MENDICANT_MAX_ENCOUNTERS], members[MENDICANT_MAX_ENCOUNTERS];
	static long lost_time[MENDICANT_MAX_ENCOUNTERS];
	static real_point3d lost_point[MENDICANT_MAX_ENCOUNTERS];
	int index, encounter;

	if (!actor_data || !actor_data->valid)
		return;
	memset(sees, 0, sizeof(sees));
	memset(engaged, 0, sizeof(engaged));
	memset(members, 0, sizeof(members));
	memset(lost_time, 0, sizeof(lost_time));

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
		long actor_index;
		struct prop_datum *prop;

		if (!actor->identifier || actor->meta.encounter_index == NONE)
			continue;
		encounter = (int)(unsigned short)actor->meta.encounter_index;
		actor_index = ((long)(unsigned short)actor->identifier << 16) | index;
		if (encounter >= MENDICANT_MAX_ENCOUNTERS || !mendicant_soldier_has(actor_index) ||
			actor->meta.dormant || actor->input.body_vitality <= 0.0f)
			continue;
		members[encounter] = TRUE;
		if (actor->situation.visible_reachable_enemies > 0)
			sees[encounter] = TRUE;
		if (actor->state.combat_status >= 2)
			engaged[encounter] = TRUE;
		prop = firing_prop(actor->target.target_prop_index);
		if (prop && prop->enemy && !prop->dead && prop->last_visible_time > lost_time[encounter])
		{
			lost_time[encounter] = prop->last_visible_time;
			lost_point[encounter] = prop->last_perceived_body_position;
		}
	}

	for (encounter = 0; encounter < MENDICANT_MAX_ENCOUNTERS; encounter++)
	{
		struct firing_search *search = &firing_searches[encounter];

		if (search->active)
		{
			for (index = 0; index < 2; index++)
			{
				/* there, or no way there: the game's own search takes over */
				int status = search->movers[index] != NONE ? actor_go_status(search->movers[index]) : GO_STATUS_NONE;

				if (search->movers[index] != NONE && status != GO_STATUS_MOVING && status != GO_STATUS_YIELDING)
				{
					if (status == GO_STATUS_ARRIVED && !firing_commanded(search->movers[index]))
						actor_go_cancel(search->movers[index]);
					search->movers[index] = NONE;
				}
			}
			if (!members[encounter] || sees[encounter] || time - search->since > FIRING_SEARCH_LENGTH_TICKS)
				firing_search_end(encounter);
			continue;
		}
		if (!members[encounter] || sees[encounter] || !engaged[encounter] || lost_time[encounter] <= 0 ||
			lost_time[encounter] == search->lost_time || time - search->ended < FIRING_SEARCH_REST_TICKS ||
			time - lost_time[encounter] < FIRING_SEARCH_AFTER_TICKS || time - lost_time[encounter] > FIRING_SEARCH_GIVE_UP_TICKS)
			continue;
		search->lost_time = lost_time[encounter];
		search->point = lost_point[encounter];
		firing_search_start(encounter, time);
	}
}

/* ---------- public code */

void mendicant_firing_reset(
	void)
{
	int index;

	memset(firing_soldiers, 0, sizeof(firing_soldiers));
	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		firing_soldiers[index].actor = NONE;
	memset(firing_searches, 0, sizeof(firing_searches));
	for (index = 0; index < MENDICANT_MAX_ENCOUNTERS; index++)
	{
		firing_searches[index].movers[0] = firing_searches[index].movers[1] = NONE;
		firing_searches[index].ended = -100000;
	}
	firing_search_time = 0;
	firing_vehicle_pills = firing_vehicle_tests = firing_vehicle_seen = firing_vehicle_large = 0;
}

/* a soldier whose firing is his own (the aim no longer goes through the tags) */
boolean mendicant_firing_personal(
	long actor_index)
{
	struct actor_datum *actor;

	if (!firing_on() || !mendicant_soldier_has(actor_index))
		return FALSE;
	actor = firing_actor(actor_index);
	return actor && !actor->external_orders.playfighting;
}

/* in actor_start_burst, before the sweep limit: his accuracy (the burst's
error, and the radius its tracers start from), how fast they walk in, and the
burst's length (the game then fits the sweep to it) */
void mendicant_firing_burst_geometry(
	long actor_index,
	real *origin_radius,
	real *return_radius)
{
	struct actor_datum *actor = firing_actor(actor_index);
	struct firing_soldier *soldier;
	struct actor_variant_definition *variant;
	struct unit_datum *unit;
	int kind, quirk;
	real suppression, courage, discipline, cohesion, maximum_range, range;
	real skill, settle, pace, seconds, nerves, wounds, close, error, global, motion, walk, burst;
	real moving = 1.0f, marine = 1.0f;
	long ticks, new_ticks;

	if (actor && mendicant_fire_file && !mendicant_firing_personal(actor_index))
		firing_log_stock("burst", actor_index, actor->control.fire_state_timer);
	if (!actor || !mendicant_firing_personal(actor_index) || (soldier = firing_soldier_get(actor_index)) == NULL)
		return;
	variant = actor_combat_get_firing_variant_definition(actor_index);
	if (!variant)
		return;
	mendicant_status.bursts++;
	kind = mendicant_soldier_kind(actor_index);
	quirk = mendicant_soldier_quirk(actor_index);
	unit = firing_unit(actor->meta.unit_index);
	suppression = firing_suppression(actor, soldier);
	if (FIRING_KIND_ELITE(kind) && firing_shields_up(actor))
		suppression *= 0.3f; /* an Elite behind his shields barely notices */
	courage = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_COURAGE);
	discipline = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE);
	cohesion = mendicant_soldier_cohesion(actor_index);
	maximum_range = variant->ranged_combat.maximum_firing_range;
	range = maximum_range > 0.0f ? actor->control.current_fire_target_range / maximum_range : 0.5f;

	/* accuracy: skill, settling on the target (the layer's own curve on the
	same clock, run faster by a steady hand, and no extra spread inside the
	game's own new target window, which already doubles it), nerve under
	fire, wounds, a target too close, the fight's mood */
	skill = firing_blend(mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_SKILL, 1.4f, 0.65f), cohesion);
	seconds = mendicant_aim_target_seconds(actor_index);
	pace = mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_SKILL, 1.5f, 0.6f);
	settle = mendicant_aim_settle(seconds / firing_maximum(pace, 0.2f));
	if (actor->control.firing_at_new_target && settle > FIRING_NEW_TARGET_SETTLE)
		settle = FIRING_NEW_TARGET_SETTLE;
	nerves = mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_COURAGE, 1.0f + 1.2f * suppression, 1.0f);
	wounds = 1.0f + 0.6f * firing_clamp(0.5f - actor->input.body_vitality, 0.0f, 0.5f); /* badly hurt: up to 1.3 */
	close = range < 0.2f ? firing_blend(mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_COURAGE, 1.5f, 1.0f), cohesion) : 1.0f;
	error = firing_clamp(skill * settle * nerves * wounds * close * mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_AIM) *
		(FIRING_KIND_MARINE(kind) ? FIRING_AIM_MARINE : FIRING_KIND_ELITE(kind) ? FIRING_AIM_ELITE :
		FIRING_KIND_GRUNT(kind) ? FIRING_AIM_GRUNT : FIRING_AIM_JACKAL), 0.35f, 3.0f);
	global = mendicant_aim_global(actor_index);
	motion = mendicant_aim_motion(actor_index,
		mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_SKILL, 1.3f, 0.7f));
	/* a steady hand starts his tracers nearer and walks them in faster */
	walk = firing_blend(mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_SKILL, 1.25f, 0.8f), cohesion);
	actor->control.burst_error *= global * motion * error;
	if (variant->ranged_combat.projectile_error_angle <= 0.0f && suppression > 0.0f)
		actor->control.burst_error += 0.013f * suppression * (1.0f - courage); /* up to 0.75 degrees: even a Jackal major shakes */
	*origin_radius *= global * motion * error * walk;
	*return_radius *= walk;

	/* the burst's length: discipline (shorter at long range for the
	disciplined), a frightened soldier's magazine dump, on the move */
	burst = mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_DISCIPLINE, 1.5f, 0.75f);
	if (range > 0.7f)
		burst *= mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_DISCIPLINE, 1.15f, 0.6f);
	if (suppression > 0.5f && courage < 0.4f && discipline < 0.45f)
		burst *= 1.4f;
	burst = firing_blend(burst, cohesion);
	if (actor->control.firing_while_moving)
	{
		real aggressive = mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_AGGRESSION, 0.8f, 1.2f);

		if (FIRING_KIND_ELITE(kind))
			moving = quirk == MENDICANT_QUIRK_BERSERKER || quirk == MENDICANT_QUIRK_DUELIST ? 1.15f : firing_minimum(aggressive, 1.15f);
		else if (FIRING_KIND_MARINE(kind))
			moving = quirk == MENDICANT_QUIRK_HOTHEAD ? 1.0f : 0.85f * aggressive;
		else if (FIRING_KIND_GRUNT(kind))
			moving = 0.8f;
		else
			moving = 0.9f;
	}
	/* (the pause after it is scaled alike: short bursts and short pauses on the
	move, the same share of time firing) */
	soldier->moving_scale = moving;
	/* the Marines' longer assault rifle bursts, each Marine's own around the
	same mean (the layer off writes ai.mendicant.marine_fire into the tags) */
	if (FIRING_KIND_MARINE(kind) && actor->orders.combat.override_burst_duration <= 0.0f && unit &&
		mendicant_carries_assault_rifle(unit))
		marine = firing_maximum(0.1f, mendicant_config.marine_fire);
	ticks = (long)(actor->control.fire_state_timer * marine + 0.5f);
	new_ticks = ticks;
	if (ticks > 0)
	{
		new_ticks = (long)(ticks * burst * moving + 0.5f);
		new_ticks = firing_clamp(new_ticks, 2, firing_maximum(FIRING_BURST_MAXIMUM_TICKS, ticks));
		actor->control.fire_state_timer = (short)new_ticks;
	}
	if (actor->control.current_fire_target_type == 1 /* _actor_fire_target_prop */)
		soldier->fired_prop = actor->control.current_fire_target_prop_index;
	soldier->last_burst_time = mendicant_time();
	firing_log("burst", actor_index, global * motion * error, mendicant_aim_stock_factor(actor_index),
		ticks, new_ticks, actor->control.firing_while_moving ? 1 : 0, error);
}

/* actor_start_pause's end: his rhythm, erratic to even */
void mendicant_firing_pause_started(
	long actor_index)
{
	struct actor_datum *actor = firing_actor(actor_index);
	struct firing_soldier *soldier;
	struct unit_datum *unit;
	int kind;
	real discipline, low, high, rhythm, nerves, marine = 1.0f;
	long ticks, new_ticks;

	if (actor && mendicant_fire_file && !mendicant_firing_personal(actor_index))
		firing_log_stock("pause", actor_index, actor->control.fire_state_timer);
	if (!actor || !mendicant_firing_personal(actor_index) || (soldier = firing_soldier_get(actor_index)) == NULL)
		return;
	kind = mendicant_soldier_kind(actor_index);
	discipline = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE);
	low = firing_lerp(0.4f, 0.9f, discipline);
	high = firing_lerp(1.6f, 1.1f, discipline);
	rhythm = firing_blend(low + (high - low) * firing_random(actor_index, soldier), mendicant_soldier_cohesion(actor_index));
	nerves = mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_COURAGE, 1.0f + 0.6f * firing_suppression(actor, soldier), 1.0f);
	ticks = actor->control.fire_state_timer;
	rhythm *= nerves * soldier->moving_scale;
	soldier->moving_scale = 1.0f;
	unit = firing_unit(actor->meta.unit_index);
	if (FIRING_KIND_MARINE(kind) && unit && mendicant_carries_assault_rifle(unit))
		marine = firing_maximum(0.1f, mendicant_config.marine_fire);
	rhythm /= marine;
	new_ticks = (long)(ticks * rhythm + 0.5f);
	/* a berserker behind his shields barely stops */
	if (mendicant_soldier_quirk(actor_index) == MENDICANT_QUIRK_BERSERKER && firing_shields_up(actor))
		new_ticks = firing_minimum(new_ticks, 12);
	new_ticks = firing_maximum(new_ticks, 2);
	actor->control.fire_state_timer = (short)new_ticks;
	firing_log("pause", actor_index, rhythm * marine, 0.0f, (long)(ticks / marine + 0.5f), new_ticks, 0, 0.0f);
}

/* actor_start_first_burst_delay's end: the wait before he opens fire, his
own; TRUE fires at once. Orders that skip the game's wait (a unit's roles)
get it too (BUG B: a commanded squad fired in unison). */
boolean mendicant_firing_first_burst_delay(
	long actor_index,
	boolean fire_immediately)
{
	struct actor_datum *actor = firing_actor(actor_index);
	struct firing_soldier *soldier;
	struct prop_datum *prop = NULL;
	boolean override, new_target, preaimed = FALSE;
	real reaction, suppression;
	long time = mendicant_time(), ticks, stock;
	int quirk;

	if (!actor || !mendicant_firing_personal(actor_index) || (soldier = firing_soldier_get(actor_index)) == NULL)
		return fire_immediately;
	mendicant_status.first_shots++;
	override = actor->orders.combat.override_firing_restrictions;
	if (actor->control.current_fire_target_type == 1 /* _actor_fire_target_prop */)
	{
		prop = firing_prop(actor->control.current_fire_target_prop_index);
		/* the game's at-once shot at a searched-for orphan stays at once */
		if (prop && prop->state >= _prop_state_uninspected_orphan && prop->state <= _prop_state_inspected_orphan)
			return fire_immediately;
	}
	if (fire_immediately && !override)
		return fire_immediately;
	quirk = mendicant_soldier_quirk(actor_index);
	/* a new target, or his target back in sight after he stopped firing at it */
	new_target = (prop && actor->control.current_fire_target_prop_index != soldier->fired_prop) ||
		(time - soldier->last_burst_time > FIRING_NEW_TARGET_TICKS && (!prop || prop->visible_ticks < FIRING_NEW_TARGET_TICKS));
	suppression = firing_suppression(actor, soldier);
	reaction = (0.08f + 0.5f * (1.0f - mendicant_soldier_base(actor_index, MENDICANT_TRAIT_REFLEX))) *
		mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_REACTION) * (1.0f + 0.6f * suppression) *
		(0.8f + 0.4f * firing_random(actor_index, soldier));
	/* a held angle: a re-peek on the same spot is met at once */
	if (time <= firing_maximum(soldier->angle_until, soldier->preaim_until) &&
		actor->control.current_fire_target_range < FIRING_ANGLE_RANGE)
	{
		real dx = actor->control.current_fire_target_position.x - soldier->angle_point.x;
		real dy = actor->control.current_fire_target_position.y - soldier->angle_point.y;

		if (dx * dx + dy * dy < FIRING_ANGLE_RADIUS * FIRING_ANGLE_RADIUS)
		{
			preaimed = TRUE;
			reaction = quirk == MENDICANT_QUIRK_SHARPSHOOTER ? 0.25f : 0.3f;
			if (quirk == MENDICANT_QUIRK_SHARPSHOOTER && mendicant_soldier_signature_due(actor_index))
				mendicant_soldier_signature_spent(actor_index);
		}
	}
	/* the rookie's first contact: a beat late, and yelling */
	if (!preaimed && new_target && quirk == MENDICANT_QUIRK_ROOKIE && mendicant_soldier_signature_due(actor_index))
	{
		reaction += 0.6f;
		mendicant_soldier_signature_spent(actor_index);
		ai_communication_event(_ai_communication_surprise, actor->meta.unit_index, NONE, NONE, NONE, NONE, NULL);
	}
	reaction = firing_minimum(reaction, FIRING_REACTION_MAXIMUM + (quirk == MENDICANT_QUIRK_ROOKIE ? 0.6f : 0.0f));
	stock = fire_immediately ? 0 : actor->control.fire_state_timer;
	if (override)
	{
		/* commanded: his reaction to a new target (picking up again on the
		same one is at once, as the orders have it), and an ambush that ripples
		out instead of one volley */
		ticks = new_target || preaimed ? (long)(reaction * FIRING_COMMANDED_REACTION * FIRING_TICKS + 0.5f) : 0;
		if (new_target && !preaimed && actor_role[(int)(unsigned short)actor_index] == ROLE_HIDDEN)
			ticks += (long)(0.35f * (1.0f - mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE)) *
				firing_random(actor_index, soldier) * FIRING_TICKS);
	}
	else if (preaimed)
	{
		ticks = firing_minimum(stock, (long)(reaction * FIRING_TICKS + 0.5f));
	}
	else
	{
		/* on his own: the game's wait, scaled by his reflexes around the same mean */
		ticks = (long)(stock * firing_blend(mendicant_soldier_mult(actor_index, MENDICANT_TRAIT_REFLEX, 1.5f, 0.6f),
			mendicant_soldier_cohesion(actor_index)) * mendicant_soldier_mood_scale(actor_index, MENDICANT_MOOD_REACTION) + 0.5f);
		if (quirk == MENDICANT_QUIRK_ROOKIE && reaction > FIRING_REACTION_MAXIMUM)
			ticks += (long)(0.6f * FIRING_TICKS);
	}
	ticks = firing_clamp(ticks, 0, (long)((FIRING_REACTION_MAXIMUM + 0.6f) * FIRING_TICKS));
	actor->control.fire_state_timer = (short)ticks;
	firing_log("react", actor_index, reaction, new_target ? 1.0f : 0.0f, stock, ticks, (override ? 1 : 0) | (preaimed ? 2 : 0), 0.0f);
	return ticks <= 0;
}

/* in actor_combat_update's firing tests: TRUE while he flinches from a hit
(no burst). A switch to a new target while the old one still stands, or a
Marine confirming a kill, lengthens the wait before his next burst. */
boolean mendicant_firing_held(
	long actor_index)
{
	struct actor_datum *actor = firing_actor(actor_index);
	struct firing_soldier *soldier;
	long time = mendicant_time();
	int kind;
	real damage;

	if (!actor || !mendicant_firing_personal(actor_index) || (soldier = firing_soldier_get(actor_index)) == NULL)
		return FALSE;
	kind = mendicant_soldier_kind(actor_index);

	/* the target switch */
	if (actor->control.current_fire_target_type == 1 /* _actor_fire_target_prop */)
	{
		long target = actor->control.current_fire_target_prop_index;

		if (soldier->hold_prop != NONE && target != soldier->hold_prop && time - soldier->last_burst_time < 60 &&
			(actor->control.fire_state == _actor_fire_state_holding || actor->control.fire_state == _actor_fire_state_pausing))
		{
			struct prop_datum *old = firing_prop(soldier->hold_prop);
			real skill = mendicant_soldier_base(actor_index, MENDICANT_TRAIT_SKILL);
			real hesitation = 0.0f;

			if (old && !old->dead && old->line_of_sight == _ai_line_of_sight_clear)
			{
				if (!(FIRING_KIND_ELITE(kind) && mendicant_soldier_base(actor_index, MENDICANT_TRAIT_AGGRESSION) > 0.7f))
					hesitation = firing_lerp(0.5f, 0.12f, skill) * (1.0f + 0.5f * firing_suppression(actor, soldier));
			}
			else if (FIRING_KIND_MARINE(kind))
			{
				hesitation = (0.3f + 0.5f * firing_random(actor_index, soldier)) *
					mendicant_soldier_base(actor_index, MENDICANT_TRAIT_DISCIPLINE);
			}
			hesitation = firing_minimum(hesitation, FIRING_HESITATION_MAXIMUM);
			if (hesitation > 0.0f)
			{
				long ticks = (long)(hesitation * FIRING_TICKS + 0.5f);

				if (ticks > actor->control.fire_state_timer)
					actor->control.fire_state_timer = (short)ticks;
				firing_log("hesitate", actor_index, hesitation, old && !old->dead ? 1.0f : 0.0f, 0, ticks, 0, 0.0f);
			}
		}
		soldier->hold_prop = target;
	}

	/* the flinch: a hit of a tenth of his health or more, an Elite's only
	once his shields are down */
	damage = actor->input.recent_body_damage + actor->input.recent_shield_damage;
	if (time - soldier->damage_time <= 2 && damage - soldier->damage > 0.1f &&
		(!FIRING_KIND_ELITE(kind) || !firing_shields_up(actor)) && time >= soldier->flinch_next &&
		time - soldier->flinch_times[1] > FIRING_FLINCH_WINDOW_TICKS)
	{
		real chance = (1.0f - mendicant_soldier_base(actor_index, MENDICANT_TRAIT_COURAGE)) * 0.6f;

		if (firing_random(actor_index, soldier) < chance)
		{
			long ticks = 6 + (long)(6.0f * firing_random(actor_index, soldier));

			soldier->flinch_until = time + ticks;
			soldier->flinch_next = time + FIRING_FLINCH_COOLDOWN_TICKS;
			soldier->flinch_times[1] = soldier->flinch_times[0];
			soldier->flinch_times[0] = time;
			firing_log("flinch", actor_index, damage - soldier->damage, 0.0f, 0, ticks, 0, 0.0f);
		}
	}
	soldier->damage = damage;
	soldier->damage_time = time;
	return time < soldier->flinch_until;
}

/* after an actor's action control: a soldier on his own holding his aim on
where a lost enemy was seen */
void mendicant_firing_action_control(
	long actor_index,
	short action)
{
	struct actor_datum *actor;
	struct firing_soldier *soldier;
	long time = mendicant_time();
	int slot = (int)(unsigned short)actor_index, role;

	if (!firing_on() || !mendicant_config.lost_target_search || slot >= MENDICANT_MAX_ACTORS ||
		firing_soldiers[slot].actor != actor_index || firing_soldiers[slot].angle_until < time)
		return;
	actor = firing_actor(actor_index);
	soldier = &firing_soldiers[slot];
	role = actor_role[slot];
	/* (roles that aim somewhere of their own keep it: the base of fire at
	the last-seen point, the sectors, the ambush) */
	if (!actor || role == ROLE_SUPPORT || role == ROLE_SHIFT_FIRE || role == ROLE_SECURITY ||
		role == ROLE_OVERWATCH || role == ROLE_HIDDEN)
		return;
	if (actor->situation.visible_reachable_enemies > 0)
	{
		/* found: the moment he sees it again counts as pre-aimed */
		soldier->preaim_until = time + 15;
		soldier->angle_until = 0;
		return;
	}
	if (action != _actor_action_fight && action != _actor_action_guard && action != _actor_action_search &&
		action != _actor_action_uncover && action != _actor_action_alert)
		return;
	actor->orders.look.primary_priority = 5;
	actor->orders.look.primary_direction.type = _direction_specification_point;
	actor->orders.look.primary_direction.point = soldier->angle_point;
	if (role == ROLE_NONE)
		actor->orders.combat.shoot_at_target = FALSE;
}

/* ai_find_line_of_fire_friend_pills' end: friendly ground vehicles with
someone aboard, as spheres; the count written */
short mendicant_firing_vehicle_pills(
	long actor_index,
	short maximum,
	real_point3d *centers,
	real *radii,
	long *prop_indices)
{
	struct actor_datum *actor;
	struct prop_iterator iterator;
	struct prop_datum *prop;
	long vehicles[8];
	short count = 0;

	if (!firing_on() || !mendicant_config.friend_vehicles || maximum <= 0 || !mendicant_soldier_has(actor_index))
		return 0;
	actor = firing_actor(actor_index);
	if (!actor || !prop_data || !prop_data->valid)
		return 0;
	if (maximum > 8)
		maximum = 8;
	firing_vehicle_tests++;
	prop_iterator_new(&iterator, actor_index);
	while ((prop = prop_iterator_next(&iterator)) != NULL && count < maximum)
	{
		struct unit_datum *vehicle;
		real radius, dx, dy, dz, distance_squared;
		short index;

		if (prop->enemy || prop->dead || prop->state != _prop_state_acknowledged || prop->vehicle_index == NONE ||
			prop->vehicle_index == actor->input.vehicle_index)
			continue;
		for (index = 0; index < count && vehicles[index] != prop->vehicle_index; index++)
			;
		if (index < count)
			continue;
		vehicle = firing_unit(prop->vehicle_index);
		if (!vehicle)
			continue;
		firing_vehicle_seen++;
		radius = vehicle->object.bounding_sphere_radius * 0.8f;
		if (radius <= 0.0f || vehicle->object.bounding_sphere_radius > FIRING_VEHICLE_RADIUS_MAXIMUM)
		{
			firing_vehicle_large++;
			continue; /* a dropship is not a Warthog */
		}
		dx = vehicle->object.bounding_sphere_center.x - actor->input.position.head_position.x;
		dy = vehicle->object.bounding_sphere_center.y - actor->input.position.head_position.y;
		dz = vehicle->object.bounding_sphere_center.z - actor->input.position.head_position.z;
		distance_squared = dx * dx + dy * dy + dz * dz;
		if (distance_squared > FIRING_VEHICLE_RANGE * FIRING_VEHICLE_RANGE ||
			distance_squared < (radius + 0.5f) * (radius + 0.5f))
			continue; /* out of reach, or he stands beside it */
		vehicles[count] = prop->vehicle_index;
		centers[count] = vehicle->object.bounding_sphere_center;
		radii[count] = radius;
		prop_indices[count] = iterator.index;
		count++;
	}
	firing_vehicle_pills += count;
	return count;
}

/* each tick after the AI's update */
void mendicant_firing_update(
	long time)
{
	if (mendicant_fire_file && time % 900 == 0)
		fflush(mendicant_fire_file);
	if (!firing_on())
		return;
	if (mendicant_config.lost_target_search && (time < firing_search_time || time - firing_search_time >= FIRING_SEARCH_UPDATE_TICKS))
	{
		firing_search_time = time;
		firing_search_update(time);
	}
	if (mendicant_fire_file && time % 900 == 0)
	{
		fprintf(mendicant_fire_file, "%ld,vehicles,-1,0,0,0,%ld,%ld,%ld,%ld,0,0\n", time,
			firing_vehicle_seen, firing_vehicle_large, firing_vehicle_tests, firing_vehicle_pills);
		fflush(mendicant_fire_file);
	}
}

/* ---------- the fire log's hits */

/* who is behind a unit's damage or in its body: an actor (his index and
actor type), the player (type 99), else nobody (-1); a vehicle stands for
whoever works its gun, else its driver */
static void firing_hit_party(
	long object_index,
	long *actor_index,
	int *type)
{
	struct unit_datum *unit = object_index != NONE && object_header_data && object_header_data->valid ?
		(struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_unit) : NULL;
	struct actor_datum *actor;

	*actor_index = NONE;
	*type = -1;
	if (unit && unit->unit.actor_index == NONE && unit->unit.player_index == NONE)
	{
		long seat = unit->unit.gunner_object_index != NONE ? unit->unit.gunner_object_index : unit->unit.driver_object_index;

		unit = seat != NONE ? (struct unit_datum *)object_try_and_get_and_verify_type(seat, _object_mask_unit) : NULL;
	}
	if (!unit)
		return;
	if (unit->unit.player_index != NONE)
	{
		*type = 99;
		return;
	}
	if ((actor = firing_actor(unit->unit.actor_index)) != NULL)
	{
		*actor_index = unit->unit.actor_index;
		*type = actor->meta.type;
	}
}

/* a unit was hurt (units.c's unit_damage_aftermath): a "hit" row of the fire
log (actor and kind: the attacker's actor and actor type; quirk: the
victim's actor type; commanded: it killed; a, b: shield and body damage; c,
d: the victim's and the attacker's team; e: the victim's actor), for the
balance measures. Nothing without the log. */
void mendicant_unit_damaged(
	long unit_index,
	long owner_object_index,
	short owner_team,
	real shield_damage,
	real body_damage,
	boolean lethal,
	short category)
{
	struct unit_datum *victim;
	long victim_actor, owner_actor;
	int victim_type, owner_type;

	if (mendicant_active)
		mendicant_spacing_hurt(unit_index, category); /* a grenade's blast, for the spacing's threat */
	if (!mendicant_fire_file || !mendicant_active || !object_header_data || !object_header_data->valid)
		return;
	victim = (struct unit_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit);
	if (!victim)
		return;
	firing_hit_party(unit_index, &victim_actor, &victim_type);
	firing_hit_party(owner_object_index, &owner_actor, &owner_type);
	fprintf(mendicant_fire_file, "%ld,hit,%d,%d,%d,%d,%.4f,%.4f,%d,%d,%d,%d\n", mendicant_time(), (int)(short)owner_actor,
		owner_type, victim_type, lethal ? 1 : 0, shield_damage, body_damage, (int)victim->object.owner_team_index,
		(int)owner_team, (int)(short)victim_actor, (int)category);
}
