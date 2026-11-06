/*
MENDICANT_CONVOY.C

MENDICANT AI's convoy (authentic layer M3; ai.mendicant.convoy, on by default
with the soldier character on; docs/mendicant_port.md, "The convoy"): when the
Chief drives off in a vehicle that is full, the Marines left over take the
other vehicles nearby and follow him, rather than standing there or racing
after him on foot.

- Seats: every seat a Marine goes for is reserved for him (vehicle, seat,
  soldier, an expiry), and a Marine's own boarding (action_vehicle.c,
  mendicant_seat_withheld) does not send a second Marine to a reserved seat or
  to a seat another is already walking to, so two Marines never go for one
  seat. A script's boarding (ai_go_to_vehicle, a command list) is left alone.
- Who sits where: the vehicles are filled one at a time, nearest first, so a
  lone driver does not take the second Warthog while the first has room. The
  wheel goes to the most skilled (a born driver more often), the gun to the
  next (a born gunner more often), the rest to the passenger seats; a Marine
  who rode before takes the seat he last had when it is free ("my hog").
- When: each Marine sets off after his own delay, 0.3 + 1.5 x (1 - loyalty)
  seconds +- 0.3, so one reacts first and the rest follow, and walks to his
  seat with the game's own walk-in (actor_action_try_to_enter_vehicle).
- The column: a follower's driver waits until his crew is in, or 6 seconds,
  then the vehicles' autopilot (mendicant_vehicles.c) drives him to his own
  place behind the Chief (to the side and rear, his own gap and offset); he
  never stops in the 30 degree cone in front of the Chief's vehicle and goes
  around it, not through it. More than 60 world units behind for 20 seconds,
  the crew get out and walk.
- Getting out: when the Chief gets out, each rider of his vehicle, and of
  every follower that pulls up near him, gets out on his own delay
  (0.3-1.5 s, by initiative) with his own exit animation, the driver last,
  after at least 4 seconds aboard. A gunner stays on the gun while the Chief
  is within 25 world units and an enemy within 50.
- No seat for him: he stays where he is (no run after a Warthog) until the
  Chief comes back within 30 world units or has been on foot for 20 seconds.
  The first to stay says so to the Chief (convoy_stay_line: a hook for the
  voice lines to come; it logs the moment and says nothing yet).

The convoy runs whether a unit commands the Marines or not (the drill's gate
on vehicles is lifted for it), only for Marines no script holds (obey,
playfighting), never during a cinematic. Hunters, the Flood and the
Covenant's vehicles are unchanged. With the convoy off, the vehicles' older
rules (mendicant_vehicles.c) run exactly as before.

ai.mendicant.convoy_test_log (training): a fixed convoy scene on the current
map with its measures in that log: the Chief, six Marines and two spare
Warthogs, the Chief driving a scripted leg out and back with a stop, an exit
and a remount at each end, then a last leg with one spare Warthog gone, more
Marines than seats (convoy_test_*).
*/

#include "cseries.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "ai/ai.h"
#include "ai/ai_script.h"
#include "ai/actors.h"
#include "ai/actor_definitions.h"
#include "ai/actions.h"
#include "cutscene/cinematics.h"
#include "objects/objects.h"
#include "physics/collision_usage.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "tag_files/tag_groups.h"

#include "mendicant_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

#define CONVOY_RESERVATIONS 48
#define CONVOY_FOLLOWERS 8
#define CONVOY_SEATS 16
#define CONVOY_CANDIDATES 32
#define CONVOY_VEHICLE_CANDIDATES 12
#define CONVOY_SOLDIER_TICKS 3        /* the soldiers' states */
#define CONVOY_PLAN_TICKS 15          /* who gets in, who gets out */
#define CONVOY_REPLAN_TICKS 60        /* leftovers with no seat yet: again */
#define CONVOY_FULL_SPEED 6.0f        /* world units a second: the Chief is leaving */
#define CONVOY_SLOW_SPEED 3.0f        /* ...slow enough to climb in beside him */
#define CONVOY_CHIEF_REACH 12.0f      /* Marines this near board his slow vehicle */
#define CONVOY_LEFTOVER_REACH 30.0f   /* Marines this near his vehicle are left over */
#define CONVOY_VEHICLE_REACH 35.0f    /* the other vehicles they may take */
#define CONVOY_RESERVE_TICKS 300      /* a reservation outlives his delay by this */
#define CONVOY_BOARD_TICKS 450        /* the longest walk to a seat */
#define CONVOY_REFUSED_TICKS 300
#define CONVOY_DEPART_TICKS 180       /* a follower waits this long for his crew */
#define CONVOY_MIN_RIDE_TICKS 120
#define CONVOY_GUNNER_CHIEF 25.0f
#define CONVOY_GUNNER_ENEMY 50.0f
#define CONVOY_DISMOUNT_REACH 25.0f
#define CONVOY_CONE_COS 0.866f        /* 30 degrees either side of his heading */
#define CONVOY_CONE_RANGE 40.0f
#define CONVOY_AROUND 7.0f            /* a follower's line this near the Chief goes around him */
#define CONVOY_LOST_RANGE 60.0f
#define CONVOY_LOST_TICKS 600
#define CONVOY_HOLD_RETURN 30.0f
#define CONVOY_HOLD_ON_FOOT_TICKS 600
#define CONVOY_LINE_REACH 25.0f       /* the Chief hears the one who stays */

#define convoy_minimum(a, b) ((a) < (b) ? (a) : (b))
#define convoy_maximum(a, b) ((a) > (b) ? (a) : (b))
#define convoy_clamp(x, lo, hi) convoy_minimum(convoy_maximum((x), (lo)), (hi))

enum
{
	CONVOY_NONE,
	CONVOY_ASSIGNED, /* a seat is his; he sets off at start_at */
	CONVOY_BOARDING, /* walking to it */
	CONVOY_RIDING,
	CONVOY_HOLDING   /* no seat: stays put */
};

enum
{
	CONVOY_SEAT_PASSENGER,
	CONVOY_SEAT_GUNNER,
	CONVOY_SEAT_DRIVER
};

/* ---------- structures */

struct convoy_soldier
{
	long actor;
	unsigned char state;
	unsigned char role;
	short seat;
	long vehicle;
	long start_at;
	long seated_at;
	long exit_at;          /* riding: when he gets out, 0 not yet */
	long last_vehicle;     /* the vehicle and seat he last rode in */
	short last_seat;
	long refused_vehicle;
	long refused_until;
	long hold_since;
	boolean gunner_holding; /* stayed on the gun at this dismount */
	boolean seated_by_convoy; /* riding: the convoy sent him to this seat */
};

struct convoy_reservation
{
	long vehicle;
	long actor;
	short seat;
	long expires;
};

struct convoy_follower
{
	long vehicle;
	int slot;              /* his place in the column */
	long driver_seated_at; /* 0 until his driver is in */
	long behind_since;
	long stuck_ticks;      /* driving and not getting anywhere */
	long stuck_checked;
	real jitter_forward, jitter_right, gap;
	boolean abandoned;     /* nobody takes it for a minute */
	long abandoned_at;
};

/* ---------- globals */

FILE *mendicant_convoy_file = NULL;

static struct convoy_soldier convoy_soldiers[MENDICANT_MAX_ACTORS];
static struct convoy_reservation convoy_reservations[CONVOY_RESERVATIONS];
static struct convoy_follower convoy_followers[CONVOY_FOLLOWERS];
static long convoy_chief_vehicle = NONE;      /* the vehicle the Chief drives now */
static long convoy_chief_last_vehicle = NONE; /* ...or drove last */
static long convoy_full_time = 0;             /* his vehicle filled up (or left), 0 not now */
static long convoy_on_foot_since = 0;
static long convoy_soldier_time, convoy_plan_time, convoy_replan_time;
static real_vector3d convoy_chief_forward = { 1.0f, 0.0f, 0.0f };
static boolean convoy_line_said = FALSE;
static long convoy_counts[8]; /* assigned, boarded, seated, refused, held, exits, gunner holds, abandoned */
/* the soldiers given a seat in this planning pass, for the cascade */
static long convoy_pass[CONVOY_CANDIDATES];
static int convoy_pass_count = 0;

/* the convoy test (ai.mendicant.convoy_test_log) */
static void convoy_test_update(long time);

/* ---------- private code */

static boolean convoy_on(
	void)
{
	return mendicant_active && mendicant_config.character && mendicant_config.convoy;
}

static struct actor_datum *convoy_actor(
	long actor_index)
{
	if (actor_index == NONE || !actor_data || !actor_data->valid)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

static struct actor_datum *convoy_actor_at(
	int index)
{
	return (struct actor_datum *)((char *)actor_data->data + index * actor_data->size);
}

static long convoy_actor_index(
	struct actor_datum *actor,
	int index)
{
	return ((long)(unsigned short)actor->identifier << 16) | index;
}

/* an object of any type, read as a unit */
static struct unit_datum *convoy_unit(
	long object_index)
{
	if (object_index == NONE || !object_header_data || !object_header_data->valid)
		return NULL;
	return (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_all);
}

/* where an object is in the world (a rider's own position is his seat's) */
static real_point3d convoy_origin(
	long object_index)
{
	real_point3d point = { 0.0f, 0.0f, 0.0f };

	if (convoy_unit(object_index))
		object_get_origin(object_index, &point);
	return point;
}

static void convoy_log(
	char const *record,
	long a,
	long b,
	long c,
	real d,
	real e)
{
	FILE *file = mendicant_convoy_file ? mendicant_convoy_file : mendicant_drill_file;

	if (file)
		fprintf(file, "%ld,convoy_%s,%ld,%ld,%ld,%.2f,%.2f\n", mendicant_time(), record, a, b, c, d, e);
}

static real convoy_distance2d(
	real_point3d const *a,
	real_point3d const *b)
{
	real x = a->x - b->x, y = a->y - b->y;

	return (real)sqrt(x * x + y * y);
}

static real convoy_rollf(
	long actor_index,
	unsigned long salt)
{
	return mendicant_soldier_rollf(actor_index, 0xC0A00000UL + salt);
}

static struct convoy_soldier *convoy_soldier_get(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct convoy_soldier *soldier;

	if (actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return NULL;
	soldier = &convoy_soldiers[slot];
	if (soldier->actor != actor_index)
	{
		memset(soldier, 0, sizeof(*soldier));
		soldier->actor = actor_index;
		soldier->vehicle = NONE;
		soldier->seat = NONE;
		soldier->last_vehicle = NONE;
		soldier->last_seat = NONE;
		soldier->refused_vehicle = NONE;
	}
	return soldier;
}

/* the seat's part: driver, gunner or passenger */
static int convoy_seat_role(
	struct unit_datum *vehicle,
	short seat_index)
{
	struct unit_definition *definition = unit_definition_get(vehicle->definition_index);
	struct unit_seat *seat;

	if (!definition || seat_index < 0 || seat_index >= definition->unit.seats.count)
		return CONVOY_SEAT_PASSENGER;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat_index, struct unit_seat);
	if (TEST_FLAG(seat->flags, _unit_seat_driver_bit))
		return CONVOY_SEAT_DRIVER;
	if (TEST_FLAG(seat->flags, _unit_seat_gunner_bit))
		return CONVOY_SEAT_GUNNER;
	return CONVOY_SEAT_PASSENGER;
}

static void convoy_release(
	long actor_index)
{
	int index;

	for (index = 0; index < CONVOY_RESERVATIONS; index++)
	{
		if (convoy_reservations[index].actor == actor_index)
			convoy_reservations[index].actor = NONE;
	}
}

static boolean convoy_reserve(
	long actor_index,
	long vehicle_index,
	short seat_index,
	long expires)
{
	int index;

	convoy_release(actor_index);
	for (index = 0; index < CONVOY_RESERVATIONS; index++)
	{
		if (convoy_reservations[index].actor == NONE)
		{
			convoy_reservations[index].actor = actor_index;
			convoy_reservations[index].vehicle = vehicle_index;
			convoy_reservations[index].seat = seat_index;
			convoy_reservations[index].expires = expires;
			return TRUE;
		}
	}
	return FALSE;
}

/* someone other than the actor holds the seat: a reservation (unless only
walkers count), or another actor already walking to it */
static boolean convoy_seat_held(
	long vehicle_index,
	short seat_index,
	long actor_index,
	boolean reservations)
{
	long time = mendicant_time();
	int index;

	for (index = 0; index < CONVOY_RESERVATIONS && reservations; index++)
	{
		struct convoy_reservation *reservation = &convoy_reservations[index];

		if (reservation->actor != NONE && reservation->actor != actor_index && reservation->vehicle == vehicle_index &&
			reservation->seat == seat_index && time < reservation->expires)
			return TRUE;
	}
	if (!actor_data || !actor_data->valid)
		return FALSE;
	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *other = convoy_actor_at(index);
		struct vehicle_state_data const *state;

		if (!other->identifier || other->state.action != _actor_action_vehicle ||
			convoy_actor_index(other, index) == actor_index)
			continue;
		state = &other->state.action_data.vehicle;
		if (state->vehicle_index == vehicle_index && state->seat_index == seat_index &&
			!state->vehicle_entry_done && !state->vehicle_entry_failed)
			return TRUE;
	}
	return FALSE;
}

static boolean convoy_seat_claimed_by_other(
	long vehicle_index,
	short seat_index,
	long actor_index)
{
	return convoy_seat_held(vehicle_index, seat_index, actor_index, TRUE);
}

/* the vehicle's empty seats that nobody else holds (not the driver's unless
asked; with walkers_only, a seat merely reserved still counts as free) */
static int convoy_free_seats_counting(
	long vehicle_index,
	struct unit_datum *vehicle,
	long except_actor,
	boolean with_driver,
	boolean walkers_only,
	short *seats,
	int maximum)
{
	short found[CONVOY_SEATS];
	int count, index, free_count = 0;

	count = vehicle_scripting_find_available_seats(vehicle_index, "", NONE, found, CONVOY_SEATS);
	for (index = 0; index < count && free_count < maximum; index++)
	{
		if (!with_driver && convoy_seat_role(vehicle, found[index]) == CONVOY_SEAT_DRIVER)
			continue;
		if (convoy_seat_held(vehicle_index, found[index], except_actor, !walkers_only))
			continue;
		seats[free_count++] = found[index];
	}
	return free_count;
}

static int convoy_free_seats(
	long vehicle_index,
	struct unit_datum *vehicle,
	long except_actor,
	boolean with_driver,
	short *seats,
	int maximum)
{
	short found[CONVOY_SEATS];
	int count, index, free_count = 0;

	count = vehicle_scripting_find_available_seats(vehicle_index, "", NONE, found, CONVOY_SEATS);
	for (index = 0; index < count && free_count < maximum; index++)
	{
		if (!with_driver && convoy_seat_role(vehicle, found[index]) == CONVOY_SEAT_DRIVER)
			continue;
		if (convoy_seat_claimed_by_other(vehicle_index, found[index], except_actor))
			continue;
		seats[free_count++] = found[index];
	}
	return free_count;
}

/* an enemy of the actor's alive within range of a point */
static boolean convoy_enemy_near(
	struct actor_datum *actor,
	real_point3d const *point,
	real range)
{
	int index;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *other = convoy_actor_at(index);

		if (!other->identifier || other->meta.unit_index == NONE || other->input.body_vitality <= 0.0f ||
			!game_team_is_enemy(other->meta.team_index, actor->meta.team_index))
			continue;
		if (convoy_distance2d(&other->input.position.body_position, point) < range)
			return TRUE;
	}
	return FALSE;
}

/* a Marine the convoy may move: alive, on foot or riding, no script's, not
running from a grenade, not panicking */
static boolean convoy_marine_free(
	long actor_index,
	struct actor_datum *actor,
	struct unit_datum **unit_out)
{
	struct unit_datum *unit;

	if (!actor || !actor->identifier || !ACTOR_IS_MARINE(actor->meta.type) ||
		mendicant_side_of(actor->meta.team_index) != TEAM_HUMAN || actor->meta.swarm)
		return FALSE;
	if (actor->external_orders.playfighting || actor->state.action == _actor_action_obey ||
		(actor->state.action == _actor_action_flee && actor->state.action_data.flee.panic_type > 0))
		return FALSE;
	unit = convoy_unit(actor->meta.unit_index);
	if (!unit || unit->object.body_vitality <= 0.0f || actor->meta.unit_index == mendicant_player_unit())
		return FALSE;
	if (mendicant_grenade_evading(actor_index))
		return FALSE;
	if (unit_out)
		*unit_out = unit;
	return TRUE;
}

static struct convoy_follower *convoy_follower_get(
	long vehicle_index,
	boolean create,
	long driver_actor)
{
	int index, used = 0, free_index = -1;
	boolean taken[CONVOY_FOLLOWERS];

	memset(taken, 0, sizeof(taken));
	for (index = 0; index < CONVOY_FOLLOWERS; index++)
	{
		if (convoy_followers[index].vehicle == vehicle_index && vehicle_index != NONE)
			return &convoy_followers[index];
		if (convoy_followers[index].vehicle == NONE)
		{
			if (free_index < 0)
				free_index = index;
		}
		else if (convoy_followers[index].slot >= 0 && convoy_followers[index].slot < CONVOY_FOLLOWERS)
		{
			taken[convoy_followers[index].slot] = TRUE;
			used++;
		}
	}
	if (!create || free_index < 0)
		return NULL;
	{
		struct convoy_follower *follower = &convoy_followers[free_index];
		real skill = mendicant_soldier_trait(driver_actor, MENDICANT_TRAIT_SKILL);

		memset(follower, 0, sizeof(*follower));
		follower->vehicle = vehicle_index;
		for (follower->slot = 0; follower->slot < CONVOY_FOLLOWERS - 1 && taken[follower->slot]; follower->slot++)
			;
		/* his own gap and offset: no two columns alike */
		follower->gap = 1.3f - 0.5f * convoy_clamp(skill, 0.0f, 1.0f);
		follower->jitter_forward = (convoy_rollf(driver_actor, 11) - 0.5f) * 4.0f;
		follower->jitter_right = (convoy_rollf(driver_actor, 12) - 0.5f) * 4.0f;
		convoy_log("follower", (long)(unsigned short)driver_actor, follower->slot, used, follower->gap, 0.0f);
		return follower;
	}
}

/* his own start: 0.3 + 1.5 x (1 - loyalty) seconds +- 0.3 */
static long convoy_delay_ticks(
	long actor_index)
{
	real loyalty = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_LOYALTY);
	real seconds = 0.3f + 1.5f * (1.0f - loyalty) + (convoy_rollf(actor_index, 1) - 0.5f) * 0.6f;

	return (long)(convoy_clamp(seconds, 0.2f, 2.5f) * 30.0f + 0.5f);
}

static void convoy_assign(
	long actor_index,
	long vehicle_index,
	struct unit_datum *vehicle,
	short seat_index,
	long time,
	boolean chief_vehicle)
{
	struct convoy_soldier *soldier = convoy_soldier_get(actor_index);
	long delay = convoy_delay_ticks(actor_index);

	if (!soldier)
		return;
	soldier->state = CONVOY_ASSIGNED;
	soldier->vehicle = vehicle_index;
	soldier->seat = seat_index;
	soldier->role = (unsigned char)convoy_seat_role(vehicle, seat_index);
	soldier->start_at = time + delay;
	soldier->exit_at = 0;
	convoy_reserve(actor_index, vehicle_index, seat_index, soldier->start_at + CONVOY_RESERVE_TICKS);
	if (convoy_pass_count < CONVOY_CANDIDATES)
		convoy_pass[convoy_pass_count++] = actor_index;
	if (soldier->role == CONVOY_SEAT_DRIVER && !chief_vehicle)
		convoy_follower_get(vehicle_index, TRUE, actor_index);
	convoy_counts[0]++; mendicant_status.convoy_assigned++;
	convoy_log("assign", (long)(unsigned short)actor_index, seat_index, soldier->role, (real)delay / 30.0f,
		chief_vehicle ? 1.0f : 0.0f);
}

/* The ones given a seat together set off one after another: whoever's own
delay is shortest goes first and the rest follow him, each a quarter to
two-thirds of a second after the one before (someone reacts, the others take
it up), so a group never moves on one tick. */
static void convoy_cascade(
	long time)
{
	int index, other;
	long previous = 0;

	for (index = 1; index < convoy_pass_count; index++)
	{
		for (other = index; other > 0; other--)
		{
			struct convoy_soldier *a = convoy_soldier_get(convoy_pass[other - 1]);
			struct convoy_soldier *b = convoy_soldier_get(convoy_pass[other]);
			long swap;

			if (!a || !b || a->start_at <= b->start_at)
				break;
			swap = convoy_pass[other - 1];
			convoy_pass[other - 1] = convoy_pass[other];
			convoy_pass[other] = swap;
		}
	}
	for (index = 0; index < convoy_pass_count; index++)
	{
		struct convoy_soldier *soldier = convoy_soldier_get(convoy_pass[index]);
		long earliest;

		if (!soldier || soldier->state != CONVOY_ASSIGNED)
			continue;
		earliest = previous ? previous + 8 + (long)(convoy_rollf(convoy_pass[index], 2) * 12.0f) : 0;
		if (soldier->start_at < earliest)
		{
			soldier->start_at = earliest;
			convoy_reserve(convoy_pass[index], soldier->vehicle, soldier->seat, soldier->start_at + CONVOY_RESERVE_TICKS);
		}
		previous = soldier->start_at;
		convoy_log("start", (long)(unsigned short)convoy_pass[index], index, 0, (real)(soldier->start_at - time) / 30.0f, 0.0f);
	}
	convoy_pass_count = 0;
}

/* fills one vehicle's free seats from the candidates, the wheel first, the
gun next; TRUE when it took anyone */
static int convoy_fill_vehicle(
	long vehicle_index,
	struct unit_datum *vehicle,
	long *candidates,
	int candidate_count,
	boolean *used,
	long time,
	boolean chief_vehicle)
{
	short seats[CONVOY_SEATS];
	int seat_count, order, index, taken = 0;

	seat_count = convoy_free_seats(vehicle_index, vehicle, NONE, !chief_vehicle, seats, CONVOY_SEATS);
	if (!seat_count)
		return 0;
	if (!chief_vehicle && vehicle->unit.driver_object_index == NONE)
	{
		/* a vehicle nobody can drive is no use to them */
		boolean driver_free = FALSE;

		for (index = 0; index < seat_count; index++)
			driver_free |= convoy_seat_role(vehicle, seats[index]) == CONVOY_SEAT_DRIVER;
		if (!driver_free)
			return 0;
	}
	/* the wheel, the gun, then the passenger seats */
	for (order = CONVOY_SEAT_DRIVER; order >= CONVOY_SEAT_PASSENGER; order--)
	{
		for (index = 0; index < seat_count; index++)
		{
			short seat = seats[index];
			int best = -1, candidate;
			real best_score = -1e30f;

			if (seat == NONE || convoy_seat_role(vehicle, seat) != order)
				continue;
			for (candidate = 0; candidate < candidate_count; candidate++)
			{
				long actor_index = candidates[candidate];
				struct actor_datum *actor = convoy_actor(actor_index);
				struct convoy_soldier *soldier = convoy_soldier_get(actor_index);
				real score, skill;
				int preference;

				if (used[candidate] || !actor || !soldier)
					continue;
				if (soldier->refused_vehicle == vehicle_index && time < soldier->refused_until)
					continue;
				if (!unit_has_animation_to_enter_seat(actor->meta.unit_index, vehicle_index, seat))
					continue;
				skill = mendicant_soldier_trait(actor_index, MENDICANT_TRAIT_SKILL);
				preference = mendicant_soldier_seat(actor_index);
				score = -convoy_distance2d(&actor->input.position.body_position, &vehicle->object.position) / 20.0f;
				if (soldier->last_vehicle == vehicle_index)
					score += soldier->last_seat == seat ? 3.0f : 1.0f;
				if (order == CONVOY_SEAT_DRIVER)
					score += 1.2f * skill + (preference == MENDICANT_SEAT_DRIVER ? 0.6f : 0.0f) -
						(preference == MENDICANT_SEAT_GUNNER ? 0.3f : 0.0f);
				else if (order == CONVOY_SEAT_GUNNER)
					score += 0.8f * skill + (preference == MENDICANT_SEAT_GUNNER ? 0.6f : 0.0f) -
						(preference == MENDICANT_SEAT_DRIVER ? 0.3f : 0.0f);
				score += 0.25f * convoy_rollf(actor_index, 20 + (unsigned long)seat);
				if (score > best_score)
				{
					best_score = score;
					best = candidate;
				}
			}
			if (best < 0)
			{
				if (order == CONVOY_SEAT_DRIVER && !chief_vehicle)
					return taken; /* nobody for the wheel: no one else either */
				continue;
			}
			used[best] = TRUE;
			seats[index] = NONE;
			convoy_assign(candidates[best], vehicle_index, vehicle, seat, time, chief_vehicle);
			taken++;
		}
	}
	return taken;
}

/* the vehicles near a point the Marines may take, nearest first */
static int convoy_vehicles_near(
	real_point3d const *centre,
	long chief_vehicle,
	long *vehicles,
	int maximum)
{
	struct object_iterator iterator;
	struct unit_datum *vehicle;
	real distances[CONVOY_VEHICLE_CANDIDATES];
	int count = 0;

	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while ((vehicle = (struct unit_datum *)object_iterator_next(&iterator)) != NULL && maximum > 0)
	{
		long vehicle_index = iterator.index;
		long driver = vehicle->unit.driver_object_index;
		real distance;
		int at;

		if (vehicle_index == chief_vehicle || !mendicant_vehicle_for_marines(vehicle) ||
			mendicant_vehicle_speed(vehicle) > CONVOY_SLOW_SPEED)
			continue;
		distance = convoy_distance2d(&vehicle->object.position, centre);
		if (distance > CONVOY_VEHICLE_REACH)
			continue;
		if (driver != NONE)
		{
			/* someone else's: only a Marine follower the convoy already drives */
			struct convoy_follower *follower = convoy_follower_get(vehicle_index, FALSE, NONE);
			struct unit_datum *driver_unit = convoy_unit(driver);

			if (!follower || follower->abandoned || !driver_unit ||
				game_team_is_enemy(driver_unit->object.owner_team_index, TEAM_HUMAN))
				continue;
		}
		else
		{
			struct convoy_follower *follower = convoy_follower_get(vehicle_index, FALSE, NONE);

			if (follower && follower->abandoned)
				continue;
		}
		for (at = count; at > 0 && distances[at - 1] > distance; at--)
		{
			if (at < maximum)
			{
				vehicles[at] = vehicles[at - 1];
				distances[at] = distances[at - 1];
			}
		}
		if (at < maximum)
		{
			vehicles[at] = vehicle_index;
			distances[at] = distance;
			if (count < maximum)
				count++;
		}
	}
	return count;
}

/* the first of those who stay says so to the Chief: the hook for the voice
lines ("We'll stay here and keep the area secure, Chief"); the convoy has no
line of its own to play yet, so it is only logged */
static void convoy_stay_line(
	long actor_index)
{
	convoy_log("stay_line", (long)(unsigned short)actor_index, 0, 0, 0.0f, 0.0f);
}

/* leftovers with a seat somewhere take it; the rest stay put once he goes */
static void convoy_plan_leftovers(
	long time,
	struct unit_datum *chief_vehicle,
	boolean leaving)
{
	long candidates[CONVOY_CANDIDATES], vehicles[CONVOY_VEHICLE_CANDIDATES];
	boolean used[CONVOY_CANDIDATES];
	real_point3d centre = { 0.0f, 0.0f, 0.0f };
	int count = 0, index, vehicle_count;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS && count < CONVOY_CANDIDATES; index++)
	{
		struct actor_datum *actor = convoy_actor_at(index);
		long actor_index;
		struct convoy_soldier *soldier;

		if (!actor->identifier)
			continue;
		actor_index = convoy_actor_index(actor, index);
		if (!convoy_marine_free(actor_index, actor, NULL) || actor->input.vehicle_index != NONE ||
			actor->state.action == _actor_action_vehicle)
			continue;
		soldier = convoy_soldier_get(actor_index);
		if (!soldier || (soldier->state != CONVOY_NONE && soldier->state != CONVOY_HOLDING))
			continue;
		if (convoy_distance2d(&actor->input.position.body_position, &chief_vehicle->object.position) > CONVOY_LEFTOVER_REACH)
			continue;
		candidates[count] = actor_index;
		used[count] = FALSE;
		centre.x += actor->input.position.body_position.x;
		centre.y += actor->input.position.body_position.y;
		centre.z += actor->input.position.body_position.z;
		count++;
	}
	if (!count)
		return;
	centre.x /= count;
	centre.y /= count;
	centre.z /= count;
	vehicle_count = convoy_vehicles_near(&centre, convoy_chief_vehicle, vehicles, CONVOY_VEHICLE_CANDIDATES);
	for (index = 0; index < vehicle_count; index++)
	{
		struct unit_datum *vehicle = convoy_unit(vehicles[index]);

		if (vehicle)
			convoy_fill_vehicle(vehicles[index], vehicle, candidates, count, used, time, FALSE);
	}
	if (!leaving)
		return;
	/* no seat for them: they stay */
	{
		long speaker = NONE;
		real loudest = -1.0f;

		for (index = 0; index < count; index++)
		{
			struct convoy_soldier *soldier = convoy_soldier_get(candidates[index]);
			struct actor_datum *actor = convoy_actor(candidates[index]);

			if (used[index] || !soldier || !actor || soldier->state == CONVOY_HOLDING)
				continue;
			soldier->state = CONVOY_HOLDING;
			soldier->hold_since = time;
			mendicant_follow_cancel(candidates[index]);
			convoy_counts[4]++;
			convoy_log("hold", (long)(unsigned short)candidates[index], 0, 0, 0.0f, 0.0f);
			if (convoy_distance2d(&actor->input.position.body_position, &chief_vehicle->object.position) < CONVOY_LINE_REACH &&
				mendicant_soldier_trait(candidates[index], MENDICANT_TRAIT_VOCAL) > loudest)
			{
				loudest = mendicant_soldier_trait(candidates[index], MENDICANT_TRAIT_VOCAL);
				speaker = candidates[index];
			}
		}
		if (speaker != NONE && !convoy_line_said)
		{
			convoy_line_said = TRUE;
			convoy_stay_line(speaker);
		}
	}
}

/* Marines near the Chief's slow vehicle climb in beside him */
static void convoy_plan_chief_vehicle(
	long time,
	long vehicle_index,
	struct unit_datum *vehicle)
{
	long candidates[CONVOY_CANDIDATES];
	boolean used[CONVOY_CANDIDATES];
	int count = 0, index;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS && count < CONVOY_CANDIDATES; index++)
	{
		struct actor_datum *actor = convoy_actor_at(index);
		long actor_index;
		struct convoy_soldier *soldier;
		real reach;

		if (!actor->identifier)
			continue;
		actor_index = convoy_actor_index(actor, index);
		if (!convoy_marine_free(actor_index, actor, NULL) || actor->input.vehicle_index != NONE ||
			actor->state.action == _actor_action_vehicle)
			continue;
		soldier = convoy_soldier_get(actor_index);
		if (!soldier || (soldier->state != CONVOY_NONE && soldier->state != CONVOY_HOLDING))
			continue;
		/* a man who rode with him before comes from further */
		reach = soldier->last_vehicle == vehicle_index ? CONVOY_CHIEF_REACH * 1.5f : CONVOY_CHIEF_REACH;
		if (convoy_distance2d(&actor->input.position.body_position, &vehicle->object.position) > reach)
			continue;
		candidates[count] = actor_index;
		used[count] = FALSE;
		count++;
	}
	if (count)
		convoy_fill_vehicle(vehicle_index, vehicle, candidates, count, used, time, TRUE);
}

/* the riders of a vehicle get out one by one, the driver last */
static void convoy_dismount(
	long vehicle_index,
	long time,
	char const *why)
{
	long last = time;
	int index, riders = 0;
	long driver_slot = -1;

	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
	{
		struct convoy_soldier *soldier = &convoy_soldiers[index];
		real initiative, seconds;

		if (soldier->state != CONVOY_RIDING || soldier->vehicle != vehicle_index || soldier->exit_at)
			continue;
		if (soldier->role == CONVOY_SEAT_DRIVER)
		{
			driver_slot = index;
			continue;
		}
		initiative = mendicant_soldier_trait(soldier->actor, MENDICANT_TRAIT_INITIATIVE);
		seconds = 0.3f + 0.9f * (1.0f - initiative) + 0.3f * convoy_rollf(soldier->actor, 30);
		soldier->exit_at = time + (long)(seconds * 30.0f);
		if (strcmp(why, "flipped"))
			soldier->exit_at = convoy_maximum(soldier->exit_at, soldier->seated_at + CONVOY_MIN_RIDE_TICKS);
		soldier->gunner_holding = FALSE;
		last = convoy_maximum(last, soldier->exit_at);
		riders++;
	}
	if (driver_slot >= 0)
	{
		struct convoy_soldier *soldier = &convoy_soldiers[driver_slot];

		soldier->exit_at = last + 9 + (long)(convoy_rollf(soldier->actor, 31) * 15.0f);
		if (strcmp(why, "flipped"))
			soldier->exit_at = convoy_maximum(soldier->exit_at, soldier->seated_at + CONVOY_MIN_RIDE_TICKS);
		riders++;
	}
	if (riders)
		convoy_log("dismount", riders, driver_slot >= 0, !strcmp(why, "flipped"), 0.0f, 0.0f);
}

/* the soldiers' own states, every few ticks */
static void convoy_soldiers_update(
	long time,
	struct unit_datum *chief)
{
	real_point3d chief_at = convoy_origin(mendicant_player_unit());
	int index;

	for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
	{
		struct actor_datum *actor = convoy_actor_at(index);
		long actor_index;
		struct convoy_soldier *soldier;
		struct unit_datum *unit = NULL;

		if (!actor->identifier || !ACTOR_IS_MARINE(actor->meta.type))
		{
			if (convoy_soldiers[index].state != CONVOY_NONE)
			{
				convoy_release(convoy_soldiers[index].actor);
				convoy_soldiers[index].state = CONVOY_NONE;
			}
			continue;
		}
		actor_index = convoy_actor_index(actor, index);
		soldier = convoy_soldier_get(actor_index);
		if (!soldier)
			continue;
		if (!convoy_marine_free(actor_index, actor, &unit))
		{
			/* a script took him, he died, or he runs from a grenade: whatever
			he was about, the seat is free again (a rider stays aboard) */
			if (soldier->state == CONVOY_ASSIGNED || soldier->state == CONVOY_BOARDING ||
				soldier->state == CONVOY_HOLDING)
			{
				convoy_release(actor_index);
				soldier->state = CONVOY_NONE;
			}
			if (soldier->state == CONVOY_RIDING && (!unit || unit->object.parent_object_index != soldier->vehicle))
				soldier->state = CONVOY_NONE;
			continue;
		}
		switch (soldier->state)
		{
		case CONVOY_NONE:
			if (unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE)
			{
				/* aboard by the game's own boarding (beside the Chief): his ride too */
				struct unit_datum *vehicle = convoy_unit(unit->object.parent_object_index);

				soldier->state = CONVOY_RIDING;
				soldier->seated_by_convoy = FALSE;
				soldier->vehicle = unit->object.parent_object_index;
				soldier->seat = unit->unit.parent_seat_index;
				soldier->role = vehicle ? (unsigned char)convoy_seat_role(vehicle, soldier->seat) : CONVOY_SEAT_PASSENGER;
				soldier->seated_at = time;
				soldier->exit_at = 0;
				soldier->last_vehicle = soldier->vehicle;
				soldier->last_seat = soldier->seat;
			}
			break;
		case CONVOY_ASSIGNED:
			if (time < soldier->start_at)
				break;
			if (convoy_seat_claimed_by_other(soldier->vehicle, soldier->seat, actor_index) ||
				!convoy_unit(soldier->vehicle) || unit_seat_filled(soldier->vehicle, soldier->seat))
			{
				convoy_release(actor_index);
				soldier->state = CONVOY_NONE;
				break;
			}
			{
				short seat = soldier->seat;

				mendicant_follow_cancel(actor_index);
				if (actor_action_try_to_enter_vehicle(actor_index, soldier->vehicle, "", NONE, 1, &seat))
				{
					soldier->state = CONVOY_BOARDING;
					soldier->start_at = time;
					convoy_counts[1]++; mendicant_status.convoy_boarded++;
					convoy_log("board", (long)(unsigned short)actor_index, soldier->seat, soldier->role, 0.0f, 0.0f);
				}
				else
				{
					convoy_release(actor_index);
					soldier->state = CONVOY_NONE;
					soldier->refused_vehicle = soldier->vehicle;
					soldier->refused_until = time + CONVOY_REFUSED_TICKS;
					convoy_counts[3]++;
					convoy_log("refused", (long)(unsigned short)actor_index, soldier->seat, soldier->role, 0.0f, 0.0f);
				}
			}
			break;
		case CONVOY_BOARDING:
			if (unit->object.parent_object_index == soldier->vehicle && unit->unit.parent_seat_index != NONE)
			{
				struct unit_datum *vehicle = convoy_unit(soldier->vehicle);

				soldier->state = CONVOY_RIDING;
				soldier->seated_by_convoy = TRUE;
				soldier->seat = unit->unit.parent_seat_index;
				soldier->role = vehicle ? (unsigned char)convoy_seat_role(vehicle, soldier->seat) : soldier->role;
				soldier->seated_at = time;
				soldier->exit_at = 0;
				soldier->last_vehicle = soldier->vehicle;
				soldier->last_seat = soldier->seat;
				convoy_release(actor_index);
				convoy_counts[2]++; mendicant_status.convoy_seated++;
				if (soldier->role == CONVOY_SEAT_DRIVER)
				{
					struct convoy_follower *follower = convoy_follower_get(soldier->vehicle, TRUE, actor_index);

					if (follower && !follower->driver_seated_at)
						follower->driver_seated_at = time;
				}
				convoy_log("seated", (long)(unsigned short)actor_index, soldier->seat, soldier->role,
					(real)(time - soldier->start_at) / 30.0f, 0.0f);
			}
			else if (actor->state.action != _actor_action_vehicle ||
				actor->state.action_data.vehicle.vehicle_index != soldier->vehicle ||
				time - soldier->start_at > CONVOY_BOARD_TICKS)
			{
				convoy_release(actor_index);
				soldier->state = CONVOY_NONE;
				soldier->refused_vehicle = soldier->vehicle;
				soldier->refused_until = time + CONVOY_REFUSED_TICKS / 2;
				convoy_log("board_failed", (long)(unsigned short)actor_index, soldier->seat, soldier->role, 0.0f, 0.0f);
			}
			break;
		case CONVOY_RIDING:
			if (unit->object.parent_object_index != soldier->vehicle)
			{
				soldier->state = CONVOY_NONE; /* out */
				break;
			}
			if (soldier->exit_at && time >= soldier->exit_at)
			{
				struct unit_datum *vehicle = convoy_unit(soldier->vehicle);

				if (soldier->role == CONVOY_SEAT_GUNNER && vehicle && chief &&
					convoy_distance2d(&chief_at, &vehicle->object.position) < CONVOY_GUNNER_CHIEF &&
					convoy_enemy_near(actor, &vehicle->object.position, CONVOY_GUNNER_ENEMY))
				{
					/* the gun covers the Chief while an enemy is about */
					if (!soldier->gunner_holding)
					{
						soldier->gunner_holding = TRUE;
						convoy_counts[6]++;
						convoy_log("gunner_stays", (long)(unsigned short)actor_index, 0, 0, 0.0f, 0.0f);
					}
					soldier->exit_at = time + 15 + (long)(convoy_rollf(actor_index, 32) * 15.0f);
					break;
				}
				if (unit_try_and_exit_seat(actor->meta.unit_index))
				{
					actor->emotions.vehicle_ignore_index = soldier->vehicle;
					actor->emotions.vehicle_ignore_time = time + 180;
					soldier->exit_at = 0;
					convoy_counts[5]++;
					convoy_log("exit", (long)(unsigned short)actor_index, soldier->seat, soldier->role, 0.0f, 0.0f);
				}
			}
			break;
		case CONVOY_HOLDING:
			if (!chief)
				break;
			/* he came back (on foot, or stopped nearby), or has been on foot a while */
			if ((convoy_distance2d(&chief_at, &actor->input.position.body_position) < CONVOY_HOLD_RETURN &&
				time - soldier->hold_since > 90 &&
				(!convoy_unit(chief->object.parent_object_index) ||
					mendicant_vehicle_speed(convoy_unit(chief->object.parent_object_index)) < CONVOY_SLOW_SPEED)) ||
				(convoy_on_foot_since && time - convoy_on_foot_since > CONVOY_HOLD_ON_FOOT_TICKS))
			{
				soldier->state = CONVOY_NONE;
				convoy_log("rejoin", (long)(unsigned short)actor_index, 0, 0, (real)(time - soldier->hold_since) / 30.0f, 0.0f);
			}
			break;
		}
	}
	/* stale reservations */
	for (index = 0; index < CONVOY_RESERVATIONS; index++)
	{
		if (convoy_reservations[index].actor != NONE && time >= convoy_reservations[index].expires)
			convoy_reservations[index].actor = NONE;
	}
	/* followers whose vehicle is gone or driverless */
	for (index = 0; index < CONVOY_FOLLOWERS; index++)
	{
		struct convoy_follower *follower = &convoy_followers[index];
		struct unit_datum *vehicle;
		boolean pending = FALSE;
		int slot;

		if (follower->vehicle == NONE)
			continue;
		vehicle = convoy_unit(follower->vehicle);
		for (slot = 0; slot < MENDICANT_MAX_ACTORS && !pending; slot++)
		{
			pending = convoy_soldiers[slot].vehicle == follower->vehicle &&
				convoy_soldiers[slot].state >= CONVOY_ASSIGNED && convoy_soldiers[slot].state <= CONVOY_RIDING;
		}
		if (!vehicle || (!pending && vehicle->unit.driver_object_index == NONE &&
			(!follower->abandoned || time - follower->abandoned_at > 1800)))
			follower->vehicle = NONE;
	}
}

/* ---------- public code */

void mendicant_convoy_reset(
	void)
{
	int index;

	memset(convoy_soldiers, 0, sizeof(convoy_soldiers));
	for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		convoy_soldiers[index].actor = NONE;
	for (index = 0; index < CONVOY_RESERVATIONS; index++)
		convoy_reservations[index].actor = NONE;
	for (index = 0; index < CONVOY_FOLLOWERS; index++)
		convoy_followers[index].vehicle = NONE;
	convoy_chief_vehicle = NONE;
	convoy_chief_last_vehicle = NONE;
	convoy_full_time = 0;
	convoy_on_foot_since = 0;
	convoy_soldier_time = convoy_plan_time = convoy_replan_time = 0;
	convoy_chief_forward.i = 1.0f;
	convoy_chief_forward.j = 0.0f;
	convoy_chief_forward.k = 0.0f;
	convoy_line_said = FALSE;
	memset(convoy_counts, 0, sizeof(convoy_counts));

	return;
}

boolean mendicant_convoy_active(
	void)
{
	return convoy_on();
}

/* boarding, about to, or staying put: following the Chief on foot and the
vehicles' older rules leave him to the convoy */
boolean mendicant_convoy_owns(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct convoy_soldier *soldier;

	if (!convoy_on() || actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	soldier = &convoy_soldiers[slot];
	return soldier->actor == actor_index &&
		(soldier->state == CONVOY_ASSIGNED || soldier->state == CONVOY_BOARDING || soldier->state == CONVOY_HOLDING);
}

/* the convoy sent this Marine to the wheel he holds (the vehicles' autopilot
takes a Scorpion or a Ghost only then: no script's driver is taken over) */
boolean mendicant_convoy_seated_driver(
	long actor_index,
	long vehicle_index)
{
	int slot = (int)(unsigned short)actor_index;
	struct convoy_soldier *soldier;

	if (!convoy_on() || actor_index == NONE || slot >= MENDICANT_MAX_ACTORS)
		return FALSE;
	soldier = &convoy_soldiers[slot];
	return soldier->actor == actor_index && soldier->state == CONVOY_RIDING && soldier->seated_by_convoy &&
		soldier->vehicle == vehicle_index && soldier->role == CONVOY_SEAT_DRIVER;
}

/* a Marine's own boarding (action_vehicle.c, the impromptu seat): he does
not go for a seat another holds or walks to (a script's boarding is not
asked) */
boolean mendicant_seat_withheld(
	long actor_index,
	long vehicle_index,
	short seat_index)
{
	struct actor_datum *actor;

	if (!convoy_on() || cinematic_in_progress())
		return FALSE;
	actor = convoy_actor(actor_index);
	if (!actor || !ACTOR_IS_MARINE(actor->meta.type))
		return FALSE;
	return convoy_seat_claimed_by_other(vehicle_index, seat_index, actor_index);
}

/* After the AI's update, each tick (mendicant_vehicles_update): the soldiers,
and every half second who gets in and out. */
void mendicant_convoy_update(
	long time)
{
	struct unit_datum *chief, *chief_ride = NULL;
	long chief_unit, chief_vehicle = NONE;
	boolean chief_drives = FALSE;

	convoy_test_update(time);
	if (!convoy_on() || !actor_data || !actor_data->valid || cinematic_in_progress())
		return;
	chief_unit = mendicant_player_unit();
	chief = convoy_unit(chief_unit);
	if (chief && chief->object.parent_object_index != NONE)
	{
		chief_ride = convoy_unit(chief->object.parent_object_index);
		if (chief_ride && chief_ride->unit.driver_object_index == chief_unit)
		{
			chief_vehicle = chief->object.parent_object_index;
			chief_drives = TRUE;
		}
	}
	if (time - convoy_soldier_time >= CONVOY_SOLDIER_TICKS || time < convoy_soldier_time)
	{
		convoy_soldier_time = time;
		convoy_soldiers_update(time, chief);
	}
	if (!(time - convoy_plan_time >= CONVOY_PLAN_TICKS || time < convoy_plan_time))
		return;
	convoy_plan_time = time;

	if (chief_drives)
	{
		real speed = mendicant_vehicle_speed(chief_ride);
		short seats[CONVOY_SEATS];
		int free_count;
		real fx = chief_ride->object.forward.i, fy = chief_ride->object.forward.j, length = (real)sqrt(fx * fx + fy * fy);

		if (length > 0.1f)
		{
			convoy_chief_forward.i = fx / length;
			convoy_chief_forward.j = fy / length;
		}
		if (chief_vehicle != convoy_chief_vehicle)
		{
			/* he got in (again): what was pending from his last ride is over */
			int index;

			for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
			{
				if (convoy_soldiers[index].state == CONVOY_RIDING)
					convoy_soldiers[index].exit_at = 0;
			}
			convoy_full_time = 0;
			convoy_line_said = FALSE;
			convoy_log("chief_in", 0, 0, 0, speed, 0.0f);
		}
		convoy_chief_vehicle = chief_vehicle;
		convoy_chief_last_vehicle = chief_vehicle;
		convoy_on_foot_since = 0;
		convoy_pass_count = 0;
		if (speed < CONVOY_SLOW_SPEED)
			convoy_plan_chief_vehicle(time, chief_vehicle, chief_ride);
		/* full once every seat beside him is taken or being walked to (a seat
		only promised does not send the rest off yet: they look for another
		ride when they see his is taken) */
		free_count = convoy_free_seats_counting(chief_vehicle, chief_ride, NONE, FALSE, TRUE, seats, CONVOY_SEATS);
		if ((free_count == 0 || speed > CONVOY_FULL_SPEED) && !convoy_full_time)
		{
			convoy_full_time = time;
			convoy_replan_time = 0;
			convoy_log("full", free_count, 0, 0, speed, 0.0f);
		}
		if (convoy_full_time && (time - convoy_replan_time >= CONVOY_REPLAN_TICKS || time < convoy_replan_time))
		{
			convoy_replan_time = time;
			convoy_plan_leftovers(time, chief_ride, speed > CONVOY_FULL_SPEED);
		}
		convoy_cascade(time);
		return;
	}

	/* aboard but not at the wheel (a script's ride): nothing changes until he is on foot */
	if (chief && chief->object.parent_object_index != NONE)
		return;
	/* on foot */
	if (convoy_chief_vehicle != NONE)
	{
		/* he just got out: his riders get out one by one */
		convoy_log("chief_out", 0, 0, 0, 0.0f, 0.0f);
		convoy_dismount(convoy_chief_vehicle, time, "chief");
		convoy_chief_vehicle = NONE;
		convoy_full_time = 0;
		convoy_on_foot_since = time;
	}
	if (!convoy_on_foot_since)
		convoy_on_foot_since = time;
	if (chief)
	{
		/* a follower that has pulled up near him: its crew get out too */
		real_point3d chief_at = convoy_origin(chief_unit);
		int index;

		for (index = 0; index < CONVOY_FOLLOWERS; index++)
		{
			struct convoy_follower *follower = &convoy_followers[index];
			struct unit_datum *vehicle = convoy_unit(follower->vehicle);

			if (follower->vehicle == NONE || !vehicle || mendicant_vehicle_speed(vehicle) > 2.0f ||
				convoy_distance2d(&vehicle->object.position, &chief_at) > CONVOY_DISMOUNT_REACH)
				continue;
			convoy_dismount(follower->vehicle, time, "follower");
		}
	}

	return;
}

/* A Marine follower's driving (the vehicles' autopilot asks each tick): his
place behind the Chief and the throttle to get there; FALSE leaves him to the
autopilot's older rule. */
boolean mendicant_convoy_drive(
	long unit_index,
	long vehicle_index,
	real_point3d *target,
	real *throttle)
{
	static real const column[][2] = { { -12.0f, 6.0f }, { -14.0f, -6.0f }, { -24.0f, 0.0f }, { -26.0f, 9.0f },
		{ -28.0f, -9.0f }, { -36.0f, 0.0f }, { -38.0f, 8.0f }, { -40.0f, -8.0f } };
	static real const parked[][2] = { { -7.0f, 9.0f }, { -8.0f, -9.0f }, { -14.0f, 3.0f }, { -15.0f, -12.0f },
		{ -18.0f, 12.0f }, { -22.0f, 0.0f }, { -24.0f, 9.0f }, { -24.0f, -9.0f } };
	struct unit_datum *vehicle, *chief, *chief_ride = NULL;
	struct convoy_follower *follower;
	long chief_unit = mendicant_player_unit(), time = mendicant_time();
	real_point3d reference, at;
	real fx, fy, rx, ry, along, side, distance, chief_speed = 0.0f, slot_forward, slot_right, dx, dy, d;
	int slot, index;
	boolean pending = FALSE, in_cone;

	if (!convoy_on())
		return FALSE;
	vehicle = convoy_unit(vehicle_index);
	chief = convoy_unit(chief_unit);
	if (!vehicle || !chief)
		return FALSE;
	{
		/* the driver's actor */
		long driver_actor = NONE;

		for (index = 0; index < MENDICANT_MAX_ACTORS; index++)
		{
			if (convoy_soldiers[index].actor != NONE && convoy_soldiers[index].state == CONVOY_RIDING &&
				convoy_soldiers[index].vehicle == vehicle_index && convoy_soldiers[index].role == CONVOY_SEAT_DRIVER)
				driver_actor = convoy_soldiers[index].actor;
		}
		follower = convoy_follower_get(vehicle_index, TRUE, driver_actor);
	}
	if (!follower)
		return FALSE;
	at = vehicle->object.position;
	if (!follower->driver_seated_at)
		follower->driver_seated_at = time;
	/* where the Chief is and which way he faces */
	if (chief->object.parent_object_index != NONE)
		chief_ride = convoy_unit(chief->object.parent_object_index);
	if (chief_ride)
	{
		reference = chief_ride->object.position;
		chief_speed = mendicant_vehicle_speed(chief_ride);
	}
	else
	{
		reference = convoy_origin(chief_unit);
	}
	fx = convoy_chief_forward.i;
	fy = convoy_chief_forward.j;
	rx = fy;
	ry = -fx;
	slot = convoy_clamp(follower->slot, 0, CONVOY_FOLLOWERS - 1);
	if (chief_ride)
	{
		slot_forward = column[slot][0] * follower->gap + follower->jitter_forward;
		slot_right = column[slot][1] * follower->gap + follower->jitter_right;
	}
	else
	{
		slot_forward = parked[slot][0] + follower->jitter_forward * 0.5f;
		slot_right = parked[slot][1] + follower->jitter_right * 0.5f;
	}
	target->x = reference.x + fx * slot_forward + rx * slot_right;
	target->y = reference.y + fy * slot_forward + ry * slot_right;
	target->z = reference.z;
	/* where he is from the Chief */
	dx = at.x - reference.x;
	dy = at.y - reference.y;
	distance = (real)sqrt(dx * dx + dy * dy) + 0.001f;
	along = dx * fx + dy * fy;
	side = dx * rx + dy * ry;
	in_cone = along > 0.0f && along / distance > CONVOY_CONE_COS && distance < CONVOY_CONE_RANGE;
	/* his crew first, or six seconds (not in front of the Chief: out of his way first) */
	for (index = 0; index < MENDICANT_MAX_ACTORS && !pending; index++)
	{
		pending = convoy_soldiers[index].vehicle == vehicle_index &&
			(convoy_soldiers[index].state == CONVOY_ASSIGNED || convoy_soldiers[index].state == CONVOY_BOARDING);
	}
	if (pending && !in_cone && !follower->abandoned && time - follower->driver_seated_at < CONVOY_DEPART_TICKS)
	{
		target->x = at.x + vehicle->object.forward.i * 10.0f;
		target->y = at.y + vehicle->object.forward.j * 10.0f;
		*throttle = 0.0f;
		return TRUE;
	}
	/* lost: too far behind for too long, the crew walk */
	if (distance > CONVOY_LOST_RANGE)
	{
		if (!follower->behind_since)
			follower->behind_since = time;
		else if (time - follower->behind_since > CONVOY_LOST_TICKS && !follower->abandoned)
		{
			follower->abandoned = TRUE;
			follower->abandoned_at = mendicant_time();
			convoy_counts[7]++;
			convoy_log("abandon", follower->slot, 0, 0, distance, 0.0f);
			convoy_dismount(vehicle_index, time, "lost");
		}
	}
	else
	{
		follower->behind_since = 0;
	}
	if (follower->abandoned)
	{
		target->x = at.x + vehicle->object.forward.i * 10.0f;
		target->y = at.y + vehicle->object.forward.j * 10.0f;
		*throttle = 0.0f;
		return TRUE;
	}
	/* ahead of him or beside him with his place behind: around, not through him */
	{
		real target_along = (target->x - reference.x) * fx + (target->y - reference.y) * fy;

		if (along > -4.0f && target_along < along && side * side < (CONVOY_AROUND + 5.0f) * (CONVOY_AROUND + 5.0f) &&
			distance < CONVOY_CONE_RANGE)
		{
			real way = side >= 0.0f ? 1.0f : -1.0f;

			target->x = reference.x + rx * way * (CONVOY_AROUND + 6.0f) + fx * convoy_maximum(along * 0.5f, -2.0f);
			target->y = reference.y + ry * way * (CONVOY_AROUND + 6.0f) + fy * convoy_maximum(along * 0.5f, -2.0f);
		}
	}
	dx = target->x - at.x;
	dy = target->y - at.y;
	d = (real)sqrt(dx * dx + dy * dy);
	{
		/* another vehicle (the Chief's among them) in the way: steer around it, slower */
		struct object_iterator iterator;
		struct unit_datum *other;
		real hx = dx / (d + 0.001f), hy = dy / (d + 0.001f);

		object_iterator_new(&iterator, _object_mask_vehicle, 0);
		while ((other = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real ox, oy, ahead, lateral;

			if (iterator.index == vehicle_index)
				continue;
			ox = other->object.position.x - at.x;
			oy = other->object.position.y - at.y;
			ahead = ox * hx + oy * hy;
			lateral = ox * -hy + oy * hx;
			if (ahead > 0.0f && ahead < convoy_minimum(d, 12.0f) + 2.0f && lateral * lateral < 16.0f)
			{
				real way = lateral >= 0.0f ? -1.0f : 1.0f;

				target->x = at.x + hx * ahead - hy * way * 6.0f;
				target->y = at.y + hy * ahead + hx * way * 6.0f;
				dx = target->x - at.x;
				dy = target->y - at.y;
				d = convoy_minimum((real)sqrt(dx * dx + dy * dy), 10.0f);
				break;
			}
		}
	}
	if (d > 15.0f)
		*throttle = 1.0f;
	else if (d > 4.0f)
		*throttle = 0.35f + 0.65f * (d - 4.0f) / 11.0f;
	else if (chief_speed > CONVOY_SLOW_SPEED)
		*throttle = convoy_clamp(chief_speed / 18.0f, 0.2f, 1.0f);
	else
		*throttle = 0.0f;
	if (chief_ride && distance < 7.0f && dx * (reference.x - at.x) + dy * (reference.y - at.y) > 0.0f)
		*throttle = 0.0f; /* closing on his Warthog: brake */
	if (in_cone && *throttle < 0.4f)
		*throttle = 0.5f; /* never stopped in front of his gun */
	/* stuck: driving and getting nowhere for eight seconds of the last stretch
	(the autopilot's reversing did not free it): the crew get out and walk */
	if (time - follower->stuck_checked >= CONVOY_PLAN_TICKS || time < follower->stuck_checked)
	{
		real speed = mendicant_vehicle_speed(vehicle);

		follower->stuck_checked = time;
		if (*throttle >= 0.3f && speed < 0.7f)
			follower->stuck_ticks += CONVOY_PLAN_TICKS;
		else if (speed > 2.0f)
			follower->stuck_ticks = 0;
		if (follower->stuck_ticks > 240)
		{
			follower->abandoned = TRUE;
			follower->abandoned_at = mendicant_time();
			convoy_counts[7]++;
			convoy_log("abandon", follower->slot, 1, 0, distance, 0.0f);
			convoy_dismount(vehicle_index, time, "stuck");
			*throttle = 0.0f;
		}
	}
	if (*throttle <= 0.0f)
	{
		/* parked: keep facing his way */
		target->x = at.x + fx * 10.0f;
		target->y = at.y + fy * 10.0f;
	}
	return TRUE;
}

/* a Marine follower's vehicle flipped (the autopilot): its crew get out one
by one, and nobody takes it again; FALSE when the convoy seated nobody in it
(the autopilot's own unload then) */
boolean mendicant_convoy_flipped(
	long vehicle_index)
{
	struct convoy_follower *follower;
	int index;
	boolean riders = FALSE;

	if (!convoy_on())
		return FALSE;
	follower = convoy_follower_get(vehicle_index, FALSE, NONE);
	if (follower)
	{
		follower->abandoned = TRUE;
		follower->abandoned_at = mendicant_time();
	}
	convoy_dismount(vehicle_index, mendicant_time(), "flipped");
	for (index = 0; index < MENDICANT_MAX_ACTORS && !riders; index++)
		riders = convoy_soldiers[index].state == CONVOY_RIDING && convoy_soldiers[index].vehicle == vehicle_index;
	return riders;
}

/* the end of a map's or test's tallies, in the drill log */
void mendicant_convoy_report(
	void)
{
	if (mendicant_drill_file && (convoy_counts[0] || convoy_counts[4]))
		fprintf(mendicant_drill_file, "%ld,convoy,assigned=%ld,boarded=%ld,seated=%ld,refused=%ld,held=%ld,exits=%ld,"
			"gunner_holds=%ld,abandoned=%ld\n", mendicant_time(), convoy_counts[0], convoy_counts[1], convoy_counts[2],
			convoy_counts[3], convoy_counts[4], convoy_counts[5], convoy_counts[6], convoy_counts[7]);
}

/* ---------- the convoy test (ai.mendicant.convoy_test_log, training)

A fixed scene, its measures written for both layers alike: once the player
has control, every actor is erased; two spare Warthogs and six Marines are
placed by the Chief and he is put at the wheel of a third. He waits for his
Warthog to fill, drives out along the clearest line, stops, gets out (an
enemy, braindead and unhurtable, stands 30 world units off for the gunner
rule), gets back in, waits again, drives back, stops and gets out; one
spare Warthog is taken away and he drives out once more. */

#define TEST_MARINES 6
#define TEST_VEHICLES 3
#define TEST_TICKS 3

enum
{
	TEST_WAIT,
	TEST_BOARD1,
	TEST_DRIVE1,
	TEST_STOP1,
	TEST_OUT1,
	TEST_BOARD2,
	TEST_DRIVE2,
	TEST_STOP2,
	TEST_OUT2,
	TEST_BOARD3,
	TEST_DRIVE3,
	TEST_STOP3,
	TEST_OUT3,
	TEST_END,
	TEST_FAILED
};

static char const *const test_phase_names[] = { "wait", "board1", "drive1", "stop1", "out1", "board2", "drive2",
	"stop2", "out2", "board3", "drive3", "stop3", "out3", "end", "failed" };

static struct
{
	int phase;
	long phase_time;
	long full_time;
	long claimed_time;
	long last_time;
	long marines[TEST_MARINES];
	long vehicles[TEST_VEHICLES];
	long enemy;
	real_point3d start, end;
	real yaw;
	long previous_vehicle[TEST_MARINES];
	short previous_seat[TEST_MARINES];
	long previous_action_vehicle[TEST_MARINES];
	short previous_action_seat[TEST_MARINES];
	unsigned char conflict_seen[TEST_MARINES][TEST_MARINES];
	unsigned char cone_stopped[TEST_VEHICLES];
	long cone_since[TEST_VEHICLES];
	real drive_yaw;
	long driver[TEST_VEHICLES];
	boolean gunner_checked;
	real throttle;
} convoy_test;

static void test_log(
	char const *format,
	...)
{
	va_list arguments;

	if (!mendicant_convoy_file)
		return;
	fprintf(mendicant_convoy_file, "%ld,", mendicant_time());
	va_start(arguments, format);
	vfprintf(mendicant_convoy_file, format, arguments);
	va_end(arguments);
	fputc('\n', mendicant_convoy_file);
	fflush(mendicant_convoy_file);
}

static void test_phase(
	int phase,
	long time)
{
	convoy_test.phase = phase;
	convoy_test.phase_time = time;
	convoy_test.gunner_checked = FALSE;
	test_log("phase,%s", test_phase_names[phase]);
}

static int test_marine_slot(
	long actor_index)
{
	int slot;

	for (slot = 0; slot < TEST_MARINES; slot++)
	{
		if (convoy_test.marines[slot] == actor_index)
			return slot;
	}
	return -1;
}

static int test_vehicle_slot(
	long vehicle_index)
{
	int slot;

	if (vehicle_index == NONE)
		return -1;
	for (slot = 0; slot < TEST_VEHICLES; slot++)
	{
		if (convoy_test.vehicles[slot] == vehicle_index)
			return slot;
	}
	return 9; /* another vehicle */
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

/* the ground at a point, or FALSE */
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
	real_point3d const *to)
{
	real_point3d a = *from, b = *to;
	int blocked;

	a.z += 1.0f;
	b.z += 1.0f;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
	blocked = mendicant_line_blocked(&a, &b);
	global_current_collision_user_depth--;
	return blocked == 0;
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

/* the scene: TRUE when it is in place */
static boolean test_setup(
	long time)
{
	struct unit_datum *chief = convoy_unit(mendicant_player_unit());
	long warthog, marine_variant, grunt_variant, marine_biped;
	real best_clear = 0.0f, best_yaw = 0.0f, fx, fy, rx, ry;
	int direction, slot;
	real_point3d ground;

	if (!chief || !global_scenario)
		return FALSE;
	warthog = test_palette_find(&global_scenario->vehicle_palette, sizeof(struct scenario_object_palette_entry), "warthog", "rocket");
	marine_variant = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference), "marine", "armored");
	if (marine_variant == NONE)
		marine_variant = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference), "marine", NULL);
	grunt_variant = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference), "grunt", NULL);
	if (warthog == NONE || marine_variant == NONE)
	{
		test_log("setup_failed,warthog=%ld,marine=%ld", warthog, marine_variant);
		return FALSE;
	}
	marine_biped = actor_variant_definition_get(marine_variant)->unit_reference.index;
	/* the clearest line from the Chief: drivable ground every 5 world units */
	convoy_test.start = convoy_origin(mendicant_player_unit());
	for (direction = 0; direction < 16; direction++)
	{
		real yaw = direction * (real)(2.0f * _pi / 16.0f), clear = 0.0f, step;
		real_point3d previous = convoy_test.start;

		for (step = 5.0f; step <= 70.0f; step += 5.0f)
		{
			real_point3d point;

			if (!test_ground(convoy_test.start.x + (real)cos(yaw) * step, convoy_test.start.y + (real)sin(yaw) * step,
				previous.z, &point) || fabs(point.z - previous.z) > 2.5f || !test_clear(&previous, &point))
				break;
			clear = step;
			previous = point;
		}
		if (clear > best_clear)
		{
			best_clear = clear;
			best_yaw = yaw;
		}
	}
	if (best_clear < 30.0f)
	{
		test_log("setup_failed,clear=%.0f", best_clear);
		return FALSE;
	}
	convoy_test.yaw = best_yaw;
	convoy_test.drive_yaw = best_yaw;
	fx = (real)cos(best_yaw);
	fy = (real)sin(best_yaw);
	rx = fy;
	ry = -fx;
	convoy_test.end.x = convoy_test.start.x + fx * best_clear * 0.85f;
	convoy_test.end.y = convoy_test.start.y + fy * best_clear * 0.85f;
	convoy_test.end.z = convoy_test.start.z;

	ai_scripting_erase_all();
	{
		/* the level's own vehicles nearby go too (nobody aboard) */
		struct object_iterator iterator;
		struct unit_datum *vehicle;

		object_iterator_new(&iterator, _object_mask_vehicle, 0);
		while ((vehicle = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
		{
			if (convoy_distance2d(&vehicle->object.position, &convoy_test.start) < 80.0f &&
				vehicle->unit.driver_object_index == NONE && vehicle->unit.gunner_object_index == NONE)
				object_delete(iterator.index);
		}
	}
	/* the Chief's Warthog ahead of him, the spares behind to each side */
	{
		static real const spots[TEST_VEHICLES][2] = { { 5.0f, 0.0f }, { -6.0f, 9.0f }, { -6.0f, -9.0f } };
		static real const nudges[][2] = { { 0.0f, 0.0f }, { 0.0f, 3.0f }, { 0.0f, -3.0f }, { -3.0f, 0.0f }, { 3.0f, 0.0f },
			{ -3.0f, 4.0f }, { -3.0f, -4.0f }, { 0.0f, 6.0f }, { 0.0f, -6.0f }, { -6.0f, 0.0f } };

		for (slot = 0; slot < TEST_VEHICLES; slot++)
		{
			int nudge;
			real x = 0.0f, y = 0.0f;

			/* flat ground with room ahead to drive off */
			for (nudge = 0; nudge < (int)(sizeof(nudges) / sizeof(nudges[0])); nudge++)
			{
				real_point3d ahead;

				x = convoy_test.start.x + fx * (spots[slot][0] + nudges[nudge][0]) + rx * (spots[slot][1] + nudges[nudge][1] * (slot == 2 ? -1.0f : 1.0f));
				y = convoy_test.start.y + fy * (spots[slot][0] + nudges[nudge][0]) + ry * (spots[slot][1] + nudges[nudge][1] * (slot == 2 ? -1.0f : 1.0f));
				if (test_ground(x, y, convoy_test.start.z, &ground) && fabs(ground.z - convoy_test.start.z) < 1.0f &&
					test_ground(x + fx * 8.0f, y + fy * 8.0f, ground.z, &ahead) && fabs(ahead.z - ground.z) < 1.0f &&
					test_clear(&ground, &ahead))
					break;
			}
			if (nudge == (int)(sizeof(nudges) / sizeof(nudges[0])) && !test_ground(x, y, convoy_test.start.z, &ground))
				ground.x = x, ground.y = y, ground.z = convoy_test.start.z;
			convoy_test.vehicles[slot] = test_place(warthog, &ground, best_yaw, 0.4f);
			test_log("vehicle,%d,%ld,%.1f,%.1f,%.1f", slot, convoy_test.vehicles[slot], ground.x, ground.y, ground.z);
		}
	}
	/* six Marines in a loose group behind him */
	for (slot = 0; slot < TEST_MARINES; slot++)
	{
		real angle = best_yaw + _pi + ((real)slot - 2.5f) * 0.35f;
		real radius = 4.0f + (slot % 3) * 1.6f;
		long unit_index;
		struct unit_datum *unit;
		real x = convoy_test.start.x + (real)cos(angle) * radius, y = convoy_test.start.y + (real)sin(angle) * radius;

		convoy_test.marines[slot] = NONE;
		convoy_test.previous_vehicle[slot] = NONE;
		convoy_test.previous_action_vehicle[slot] = NONE;
		if (!test_ground(x, y, convoy_test.start.z, &ground))
			ground.x = x, ground.y = y, ground.z = convoy_test.start.z;
		unit_index = test_place(marine_biped, &ground, best_yaw, 0.1f);
		if (unit_index == NONE)
			continue;
		ai_scripting_attach_free(unit_index, marine_variant);
		unit = convoy_unit(unit_index);
		if (unit)
		{
			int index;

			for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
			{
				struct actor_datum *actor = convoy_actor_at(index);

				if (actor->identifier && actor->meta.unit_index == unit_index)
					convoy_test.marines[slot] = convoy_actor_index(actor, index);
			}
		}
		test_log("marine,%d,%ld,%.1f,%.1f,%.1f", slot, convoy_test.marines[slot], ground.x, ground.y, ground.z);
	}
	convoy_test.enemy = NONE;
	(void)grunt_variant;
	/* the Chief at the wheel */
	{
		struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
		char const *seat = hog ? test_driver_seat(hog) : NULL;

		if (seat)
			unit_scripting_enter_vehicle(mendicant_player_unit(), convoy_test.vehicles[0], seat);
	}
	for (slot = 0; slot < TEST_VEHICLES; slot++)
		convoy_test.driver[slot] = NONE;
	test_log("setup,clear=%.0f,yaw=%.2f,team_mode=%d,convoy=%d", best_clear, best_yaw,
		mendicant_team_mode(TEAM_HUMAN), convoy_on());
	return TRUE;
}

static void test_enemy(
	boolean place)
{
	if (place)
	{
		long grunt_variant = test_palette_find(&global_scenario->ai_actor_palette, sizeof(struct tag_reference), "grunt", NULL);
		struct unit_datum *chief = convoy_unit(mendicant_player_unit());
		real_point3d ground, from = chief ? convoy_origin(mendicant_player_unit()) : convoy_test.end;
		real fx = (real)cos(convoy_test.yaw), fy = (real)sin(convoy_test.yaw);
		long unit_index;
		int direction;

		if (grunt_variant == NONE)
		{
			test_log("enemy_failed,no_grunt");
			return;
		}
		/* 30 world units off to a side with ground */
		for (direction = 0; direction < 8; direction++)
		{
			real angle = convoy_test.yaw + _pi * 0.5f + direction * (real)(_pi / 4.0f);

			if (test_ground(from.x + (real)cos(angle) * 30.0f, from.y + (real)sin(angle) * 30.0f, from.z, &ground))
				break;
		}
		if (direction == 8)
		{
			test_log("enemy_failed,no_ground,%.1f,%.1f,%.1f", from.x, from.y, from.z);
			return;
		}
		unit_index = test_place(actor_variant_definition_get(grunt_variant)->unit_reference.index, &ground, convoy_test.yaw + _pi, 0.1f);
		if (unit_index == NONE)
		{
			test_log("enemy_failed,no_unit");
			return;
		}
		ai_scripting_attach_free(unit_index, grunt_variant);
		{
			struct unit_datum *unit = convoy_unit(unit_index);

			if (unit)
				SET_FLAG(unit->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
		}
		convoy_test.enemy = unit_index;
		{
			int index;

			for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
			{
				struct actor_datum *actor = convoy_actor_at(index);

				if (actor->identifier && actor->meta.unit_index == unit_index)
				{
					actor_braindead(convoy_actor_index(actor, index), TRUE);
					test_log("enemy,%.1f,%.1f,team=%d", ground.x, ground.y, actor->meta.team_index);
				}
			}
		}
		(void)fx;
		(void)fy;
	}
	else if (convoy_test.enemy != NONE)
	{
		int index;

		for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
		{
			struct actor_datum *actor = convoy_actor_at(index);

			if (actor->identifier && actor->meta.unit_index == convoy_test.enemy)
				actor_erase(convoy_actor_index(actor, index), FALSE);
		}
		convoy_test.enemy = NONE;
		test_log("enemy_gone");
	}
}

/* the non-driver seats of the Chief's Warthog all taken */
static boolean test_chief_full(
	void)
{
	struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
	short seats[CONVOY_SEATS];
	int count, index;

	if (!hog)
		return FALSE;
	count = vehicle_scripting_find_available_seats(convoy_test.vehicles[0], "", NONE, seats, CONVOY_SEATS);
	for (index = 0; index < count; index++)
	{
		if (convoy_seat_role(hog, seats[index]) != CONVOY_SEAT_DRIVER)
			return FALSE;
	}
	return TRUE;
}

/* ...or being walked to (either layer: the game's own walk-in) */
static boolean test_chief_claimed(
	void)
{
	struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
	short seats[CONVOY_SEATS];
	int count, index, other;

	if (!hog)
		return FALSE;
	count = vehicle_scripting_find_available_seats(convoy_test.vehicles[0], "", NONE, seats, CONVOY_SEATS);
	for (index = 0; index < count; index++)
	{
		boolean walked = FALSE;

		if (convoy_seat_role(hog, seats[index]) == CONVOY_SEAT_DRIVER)
			continue;
		for (other = 0; other < actor_data->count && other < MENDICANT_MAX_ACTORS && !walked; other++)
		{
			struct actor_datum *actor = convoy_actor_at(other);

			walked = actor->identifier && actor->state.action == _actor_action_vehicle &&
				actor->state.action_data.vehicle.vehicle_index == convoy_test.vehicles[0] &&
				actor->state.action_data.vehicle.seat_index == seats[index];
		}
		if (!walked)
			return FALSE;
	}
	return TRUE;
}

/* the measures, every few ticks, for both layers */
static void test_measure(
	long time)
{
	struct unit_datum *chief = convoy_unit(mendicant_player_unit());
	struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
	real_point3d chief_at = convoy_origin(mendicant_player_unit());
	int slot, other;

	for (slot = 0; slot < TEST_MARINES; slot++)
	{
		struct actor_datum *actor = convoy_actor(convoy_test.marines[slot]);
		struct unit_datum *unit = actor ? convoy_unit(actor->meta.unit_index) : NULL;
		long vehicle = NONE, action_vehicle = NONE;
		short seat = NONE, action_seat = NONE;

		if (!actor || !unit)
			continue;
		if (unit->object.parent_object_index != NONE)
		{
			vehicle = unit->object.parent_object_index;
			seat = unit->unit.parent_seat_index;
		}
		if (actor->state.action == _actor_action_vehicle)
		{
			action_vehicle = actor->state.action_data.vehicle.vehicle_index;
			action_seat = actor->state.action_data.vehicle.seat_index;
		}
		if (action_vehicle != convoy_test.previous_action_vehicle[slot] || action_seat != convoy_test.previous_action_seat[slot])
		{
			if (action_vehicle != NONE)
				test_log("board,%d,%d,%d,%.1f", slot, test_vehicle_slot(action_vehicle), action_seat,
					convoy_distance2d(&unit->object.position, &convoy_unit(action_vehicle)->object.position));
			convoy_test.previous_action_vehicle[slot] = action_vehicle;
			convoy_test.previous_action_seat[slot] = action_seat;
		}
		if (vehicle != convoy_test.previous_vehicle[slot] || seat != convoy_test.previous_seat[slot])
		{
			if (convoy_test.previous_vehicle[slot] != NONE)
				test_log("exit,%d,%d,%d", slot, test_vehicle_slot(convoy_test.previous_vehicle[slot]),
					convoy_test.previous_seat[slot]);
			if (vehicle != NONE)
			{
				struct unit_datum *ride = convoy_unit(vehicle);
				int role = ride ? convoy_seat_role(ride, seat) : 0;

				test_log("seated,%d,%d,%d,%d", slot, test_vehicle_slot(vehicle), seat, role);
				if (role == CONVOY_SEAT_DRIVER)
					test_log("driver,%d,%d", test_vehicle_slot(vehicle), slot);
			}
			convoy_test.previous_vehicle[slot] = vehicle;
			convoy_test.previous_seat[slot] = seat;
		}
	}
	/* two going for one seat at once */
	for (slot = 0; slot < TEST_MARINES; slot++)
	{
		for (other = slot + 1; other < TEST_MARINES; other++)
		{
			struct actor_datum *a = convoy_actor(convoy_test.marines[slot]);
			struct actor_datum *b = convoy_actor(convoy_test.marines[other]);
			boolean together;

			if (!a || !b)
				continue;
			together = a->state.action == _actor_action_vehicle && b->state.action == _actor_action_vehicle &&
				a->state.action_data.vehicle.vehicle_index == b->state.action_data.vehicle.vehicle_index &&
				a->state.action_data.vehicle.seat_index == b->state.action_data.vehicle.seat_index &&
				!a->state.action_data.vehicle.vehicle_entry_done && !b->state.action_data.vehicle.vehicle_entry_done;
			if (together && !convoy_test.conflict_seen[slot][other])
				test_log("conflict,%d,%d,%d,%d", slot, other, test_vehicle_slot(a->state.action_data.vehicle.vehicle_index),
					a->state.action_data.vehicle.seat_index);
			convoy_test.conflict_seen[slot][other] = (unsigned char)together;
		}
	}
	/* a Marine-driven vehicle stopped in front of the Chief's Warthog */
	if (hog)
	{
		real fx = hog->object.forward.i, fy = hog->object.forward.j, length = (real)sqrt(fx * fx + fy * fy) + 0.001f;
		int vehicle_slot;

		fx /= length;
		fy /= length;
		for (vehicle_slot = 1; vehicle_slot < TEST_VEHICLES; vehicle_slot++)
		{
			struct unit_datum *vehicle = convoy_unit(convoy_test.vehicles[vehicle_slot]);
			struct unit_datum *driver = vehicle ? convoy_unit(vehicle->unit.driver_object_index) : NULL;
			real dx, dy, distance, cosine;
			int cone = 0;

			if (!vehicle || !driver || vehicle->unit.driver_object_index == mendicant_player_unit())
			{
				convoy_test.cone_stopped[vehicle_slot] = 0;
				continue;
			}
			dx = vehicle->object.position.x - hog->object.position.x;
			dy = vehicle->object.position.y - hog->object.position.y;
			distance = (real)sqrt(dx * dx + dy * dy) + 0.001f;
			cosine = (dx * fx + dy * fy) / distance;
			if (mendicant_vehicle_speed(vehicle) < 0.5f && distance < CONVOY_CONE_RANGE)
				cone = cosine > 0.9659f ? 2 : cosine > CONVOY_CONE_COS ? 1 : 0; /* 15 and 30 degrees */
			if (cone && cone != convoy_test.cone_stopped[vehicle_slot])
			{
				test_log("cone_touch,%d,%d,%.1f,%.1f", vehicle_slot, cone == 2 ? 15 : 30, distance,
					(real)(acos(convoy_clamp(cosine, -1.0f, 1.0f)) * 180.0f / _pi));
				convoy_test.cone_since[vehicle_slot] = time;
			}
			if (!cone)
				convoy_test.cone_since[vehicle_slot] = 0;
			else if (convoy_test.cone_since[vehicle_slot] && time - convoy_test.cone_since[vehicle_slot] >= 30)
			{
				/* stopped there a full second */
				test_log("cone,%d,%d,%.1f,%.1f", vehicle_slot, cone == 2 ? 15 : 30, distance,
					(real)(acos(convoy_clamp(cosine, -1.0f, 1.0f)) * 180.0f / _pi));
				convoy_test.cone_since[vehicle_slot] = 0;
			}
			convoy_test.cone_stopped[vehicle_slot] = (unsigned char)cone;
		}
	}
	/* the gunners, four seconds after he got out */
	if ((convoy_test.phase == TEST_OUT1 || convoy_test.phase == TEST_OUT2 || convoy_test.phase == TEST_OUT3) &&
		!convoy_test.gunner_checked &&
		time - convoy_test.phase_time >= 120)
	{
		int vehicle_slot;

		convoy_test.gunner_checked = TRUE;
		for (vehicle_slot = 0; vehicle_slot < TEST_VEHICLES; vehicle_slot++)
		{
			struct unit_datum *vehicle = convoy_unit(convoy_test.vehicles[vehicle_slot]);
			boolean enemy_near = FALSE;

			if (!vehicle)
				continue;
			if (convoy_test.enemy != NONE)
			{
				struct unit_datum *enemy = convoy_unit(convoy_test.enemy);

				enemy_near = enemy && convoy_distance2d(&enemy->object.position, &vehicle->object.position) < CONVOY_GUNNER_ENEMY;
			}
			test_log("gunner,%d,%d,%d,%.1f", vehicle_slot, vehicle->unit.gunner_object_index != NONE, enemy_near,
				chief ? convoy_distance2d(&chief_at, &vehicle->object.position) : -1.0f);
		}
	}
	/* where everyone is, each second */
	if (time % 30 < TEST_TICKS)
	{
		int vehicle_slot;

		for (vehicle_slot = 0; vehicle_slot < TEST_VEHICLES; vehicle_slot++)
		{
			struct unit_datum *vehicle = convoy_unit(convoy_test.vehicles[vehicle_slot]);

			if (vehicle)
				test_log("pos,%d,%.1f,%.1f,%.1f,%d,%.2f", vehicle_slot, vehicle->object.position.x, vehicle->object.position.y,
					mendicant_vehicle_speed(vehicle), vehicle->unit.driver_object_index != NONE, vehicle->object.up.k);
		}
		for (slot = 0; slot < TEST_MARINES; slot++)
		{
			struct actor_datum *actor = convoy_actor(convoy_test.marines[slot]);

			if (actor)
				test_log("mpos,%d,%.1f,%.1f,%d,%d", slot, actor->input.position.body_position.x,
					actor->input.position.body_position.y, actor->input.vehicle_index != NONE, (int)actor->state.action);
		}
		test_log("cpos,%.1f,%.1f", chief_at.x, chief_at.y);
	}
}

static boolean test_chief_in(
	void)
{
	struct unit_datum *chief = convoy_unit(mendicant_player_unit());

	return chief && chief->object.parent_object_index == convoy_test.vehicles[0];
}

static void convoy_test_update(
	long time)
{
	struct unit_datum *chief;

	if (!mendicant_convoy_file || !mendicant_active || !actor_data || !actor_data->valid)
		return;
	if (time < convoy_test.last_time)
		memset(&convoy_test, 0, sizeof(convoy_test)); /* a new map, or the clock went back */
	if (time - convoy_test.last_time < TEST_TICKS && convoy_test.last_time)
		return;
	convoy_test.last_time = time;
	chief = convoy_unit(mendicant_player_unit());
	if (convoy_test.phase == TEST_WAIT)
	{
		if (!chief || chief->object.parent_object_index != NONE || cinematic_in_progress() || time < 900)
			return;
		if (!test_setup(time))
		{
			test_phase(TEST_FAILED, time);
			return;
		}
		test_phase(TEST_BOARD1, time);
		return;
	}
	if (convoy_test.phase >= TEST_END)
		return;
	test_measure(time);
	{
		/* the level's own actors stay away */
		int index;

		if (time % 30 < TEST_TICKS)
		{
			for (index = 0; index < actor_data->count && index < MENDICANT_MAX_ACTORS; index++)
			{
				struct actor_datum *actor = convoy_actor_at(index);
				long actor_index;

				if (!actor->identifier || actor->meta.encounter_index == NONE)
					continue;
				actor_index = convoy_actor_index(actor, index);
				if (test_marine_slot(actor_index) < 0 && actor->meta.unit_index != convoy_test.enemy)
					actor_erase(actor_index, FALSE);
			}
		}
	}
	switch (convoy_test.phase)
	{
	case TEST_BOARD1:
	case TEST_BOARD2:
	case TEST_BOARD3:
		if (!test_chief_in())
		{
			struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
			char const *seat = hog ? test_driver_seat(hog) : NULL;

			if (seat && chief && chief->object.parent_object_index == NONE)
				unit_scripting_enter_vehicle(mendicant_player_unit(), convoy_test.vehicles[0], seat);
			if (time - convoy_test.phase_time > 300)
				test_phase(TEST_FAILED, time);
			break;
		}
		if (!convoy_test.claimed_time && test_chief_claimed())
		{
			int spare;

			for (spare = 1; spare < TEST_VEHICLES; spare++)
			{
				struct unit_datum *vehicle = convoy_unit(convoy_test.vehicles[spare]);
				struct unit_datum *hog = convoy_unit(convoy_test.vehicles[0]);
				short seats[CONVOY_SEATS];

				if (vehicle && hog)
					test_log("spare,%d,%d,%.1f,%.2f", spare,
						vehicle_scripting_find_available_seats(convoy_test.vehicles[spare], "", NONE, seats, CONVOY_SEATS),
						convoy_distance2d(&vehicle->object.position, &hog->object.position), vehicle->object.up.k);
			}
			convoy_test.claimed_time = time;
			test_log("claimed,%s", convoy_test.phase == TEST_BOARD1 ? "1" : convoy_test.phase == TEST_BOARD2 ? "2" : "3");
		}
		if (!convoy_test.full_time && test_chief_full())
		{
			convoy_test.full_time = time;
			test_log("full,%s", convoy_test.phase == TEST_BOARD1 ? "1" : convoy_test.phase == TEST_BOARD2 ? "2" : "3");
		}
		if ((convoy_test.full_time && time - convoy_test.full_time > 240) || time - convoy_test.phase_time > 750)
		{
			if (!convoy_test.full_time)
				test_log("full,never");
			convoy_test.full_time = 0;
			convoy_test.claimed_time = 0;
			test_phase(convoy_test.phase + 1, time);
		}
		break;
	case TEST_DRIVE1:
	case TEST_DRIVE2:
	case TEST_DRIVE3:
		if (chief)
		{
			real_point3d const *goal = convoy_test.phase == TEST_DRIVE2 ? &convoy_test.start : &convoy_test.end;

			real_point3d chief_at = convoy_origin(mendicant_player_unit());

			if (convoy_distance2d(&chief_at, goal) < 6.0f || time - convoy_test.phase_time > 750)
			{
				test_phase(convoy_test.phase + 1, time);
				if (convoy_test.phase == TEST_STOP1)
					test_enemy(TRUE);
			}
		}
		break;
	case TEST_STOP1:
	case TEST_STOP2:
	case TEST_STOP3:
		if (time - convoy_test.phase_time > 150)
		{
			unit_scripting_exit_vehicle(mendicant_player_unit());
			test_phase(convoy_test.phase + 1, time);
		}
		break;
	case TEST_OUT1:
		if (time - convoy_test.phase_time > 420)
		{
			test_enemy(FALSE);
			test_phase(TEST_BOARD2, time);
		}
		break;
	case TEST_OUT2:
		if (time - convoy_test.phase_time > 360)
		{
			/* the last leg with one spare Warthog: more Marines than seats */
			struct unit_datum *spare = convoy_unit(convoy_test.vehicles[2]);

			if (spare && spare->unit.driver_object_index == NONE && spare->unit.gunner_object_index == NONE)
			{
				object_delete(convoy_test.vehicles[2]);
				test_log("spare_gone,2");
			}
			test_phase(TEST_BOARD3, time);
		}
		break;
	case TEST_OUT3:
		if (time - convoy_test.phase_time > 450)
		{
			mendicant_convoy_report();
			test_phase(TEST_END, time);
		}
		break;
	}
}

/* the scripted Chief's driving (players.c, after the players' actions are
read): only during the convoy test */
void mendicant_player_actions(
	struct player_action *actions)
{
	struct unit_datum *chief;
	real_point3d const *goal;
	real dx, dy;

	if (!mendicant_convoy_file || !mendicant_active || !actions)
		return;
	if (convoy_test.phase < TEST_BOARD1 || convoy_test.phase >= TEST_END)
		return;
	chief = convoy_unit(mendicant_player_unit());
	if (!chief)
		return;
	actions[0].throttle.i = 0.0f;
	actions[0].throttle.j = 0.0f;
	actions[0].desired_facing.yaw = convoy_test.drive_yaw;
	actions[0].desired_facing.pitch = 0.0f;
	if (convoy_test.phase != TEST_DRIVE1 && convoy_test.phase != TEST_DRIVE2 && convoy_test.phase != TEST_DRIVE3)
		return;
	goal = convoy_test.phase == TEST_DRIVE2 ? &convoy_test.start : &convoy_test.end;
	{
		real_point3d chief_at = convoy_origin(mendicant_player_unit());

		dx = goal->x - chief_at.x;
		dy = goal->y - chief_at.y;
	}
	actions[0].desired_facing.yaw = (real)atan2(dy, dx);
	convoy_test.drive_yaw = actions[0].desired_facing.yaw;
	actions[0].throttle.i = sqrt(dx * dx + dy * dy) > 15.0f ? 1.0f : 0.5f;
}
