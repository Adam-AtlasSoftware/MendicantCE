/*
SPECTATE_DIRECTOR.C

The spectator's auto-director (spectate.h's DIRECTOR camera): it watches
whoever is most worth watching and cuts between them as a person directing
a broadcast would, from what every machine of the game already knows.

Each game tick every player gets an interest:
- a kill in the last seconds (the most), being in a fight (their weapon fired
  lately, or they were hurt), carrying the flag or the ball, driving fast, and
  enemies close by;
- a little more the longer since they were shown, so everyone gets a turn.

And the shot rules, so the picture never jitters:
- a shot lasts MINIMUM_SHOT seconds at least and MAXIMUM_SHOT at most;
- before its end it cuts only to someone clearly more interesting (CUT_RATIO
  and CUT_MARGIN over the one watched);
- the one watched dying holds on them KILL_HOLD seconds (their fall, the
  camera where it was), then cuts to their killer if they are alive, else
  to whoever is most worth watching;
- a cut to someone near glides (the observer's timer), to someone far is
  hard.

Every cut is logged with its reason and the game tick, which the tests read
(tools/test_spectator.py).
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "items/weapons.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "text/unicode.h"
#include "units/units.h"

#include "spectate.h"
#include "spectate_director.h"

#include <math.h>

/* ---------- constants */

/* a shot's least and most seconds */
#define MINIMUM_SHOT 5.0f
#define MAXIMUM_SHOT 15.0f
/* seconds the camera stays with the one watched once they die */
#define KILL_HOLD 2.0f
/* how much more interesting someone must be to cut to them early */
#define CUT_RATIO 1.25f
#define CUT_MARGIN 1.0f
/* a cut to someone no further than this (world units) glides */
#define GLIDE_DISTANCE 25.0f
/* ticks since a weapon fired that count as fighting */
#define FIRING_TICKS 15
/* enemies this near (world units) count */
#define NEAR_DISTANCE 12.0f
/* how fast a vehicle goes to count as driving fast (world units a tick) */
#define FAST_VEHICLE 0.15f

/* each signal's weight in a player's interest, and its decay (of 1, a second) */
#define KILL_WEIGHT 6.0f
#define KILL_DECAY 0.25f
#define FIGHT_WEIGHT 3.0f
#define FIGHT_DECAY 0.6f
#define HURT_WEIGHT 2.0f
#define HURT_DECAY 0.5f
#define OBJECTIVE_WEIGHT 2.5f
#define VEHICLE_WEIGHT 1.0f
#define NEAR_WEIGHT 0.6f
/* interest a second since they were last shown, to two minutes */
#define TURN_WEIGHT 0.02f
#define TURN_SECONDS 120.0f

enum
{
	_reason_start,
	_reason_kill,
	_reason_killer,
	_reason_fight,
	_reason_hurt,
	_reason_objective,
	_reason_vehicle,
	_reason_near,
	_reason_turn,
	_reason_time,
	_reason_left,
	_reason_died,
	NUMBER_OF_REASONS
};

static char const *const reason_names[NUMBER_OF_REASONS] =
{
	"start", "kill", "killer", "fight", "hurt", "objective", "vehicle", "near", "turn", "time", "left", "died"
};

/* ---------- globals */

static struct
{
	short identifier;
	boolean alive;
	short kills;
	short deaths;
	real body;
	real shield;
	real kill;
	real fight;
	real hurt;
	real objective;
	real vehicle;
	real near;
	real unshown_seconds;
	/* the ticks of their last kill and death (NONE: none) */
	long kill_tick;
	long death_tick;
} director_players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

static struct
{
	long last_tick;
	long watched;
	real shot_seconds;
	/* seconds left watching the one who died (0: not holding) */
	real hold_seconds;
	boolean started;
} director;

/* the platform layer's (port_config.c) */
void platform_log(char const *format, ...);

/* ---------- private code */

static long living_unit(
	struct player_datum const *player)
{
	return player && player->unit_index != NONE &&
		object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit) &&
		!TEST_FLAG(object_get(player->unit_index)->object.damage_flags, _object_dead_bit) ?
		player->unit_index : NONE;
}

static real decay(
	real value,
	real rate,
	real seconds)
{
	return MAX(0.0f, value - rate * seconds);
}

/* the interest of a player now, and its main reason */
static real interest(
	long absolute_index,
	short *reason)
{
	real terms[NUMBER_OF_REASONS];
	real total = 0.0f;
	real best = -1.0f;
	short index;

	csmemset(terms, 0, sizeof(terms));
	terms[_reason_kill] = KILL_WEIGHT * director_players[absolute_index].kill;
	terms[_reason_fight] = FIGHT_WEIGHT * director_players[absolute_index].fight;
	terms[_reason_hurt] = HURT_WEIGHT * director_players[absolute_index].hurt;
	terms[_reason_objective] = OBJECTIVE_WEIGHT * director_players[absolute_index].objective;
	terms[_reason_vehicle] = VEHICLE_WEIGHT * director_players[absolute_index].vehicle;
	terms[_reason_near] = NEAR_WEIGHT * MIN(director_players[absolute_index].near, 3.0f);
	terms[_reason_turn] = TURN_WEIGHT * MIN(director_players[absolute_index].unshown_seconds, TURN_SECONDS);
	*reason = _reason_turn;
	for (index = 0; index < NUMBER_OF_REASONS; index++)
	{
		total += terms[index];
		if (terms[index] > best)
		{
			best = terms[index];
			*reason = index;
		}
	}
	return total;
}

/* ticks around a death that a kill counts as its killer's (the kill and the
death come in messages of their own) */
#define KILLER_TICKS 30

/* the player who killed the player most likely: whoever scored a kill
nearest the death (NONE: nobody did) */
static long killer_of(
	long absolute_index)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long death_tick = director_players[absolute_index].death_tick;
	long best = NONE;
	long best_gap = KILLER_TICKS + 1;

	if (death_tick == NONE)
		return NONE;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long other = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		long gap;

		if (other == absolute_index || other < 0 || other >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ||
			director_players[other].kill_tick == NONE)
		{
			continue;
		}
		gap = director_players[other].kill_tick - death_tick;
		gap = gap < 0 ? -gap : gap;
		if (gap < best_gap)
		{
			best_gap = gap;
			best = iterator.datum_index;
		}
	}
	return best;
}

/* each player's signals, once a tick */
static void director_tick(
	real seconds)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long now = game_time_get();

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		long unit_index = living_unit(player);
		struct unit_datum *unit = unit_index != NONE ? unit_get(unit_index) : NULL;

		if (absolute_index < 0 || absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
			continue;
		/* (a new player in the slot starts afresh) */
		if (director_players[absolute_index].identifier != player->identifier)
		{
			csmemset(&director_players[absolute_index], 0, sizeof(director_players[absolute_index]));
			director_players[absolute_index].identifier = player->identifier;
			director_players[absolute_index].kills = player->statistics.kills[0];
			director_players[absolute_index].deaths = player->statistics.deaths;
			director_players[absolute_index].kill_tick = NONE;
			director_players[absolute_index].death_tick = NONE;
			director_players[absolute_index].unshown_seconds = TURN_SECONDS / 2.0f;
		}
		director_players[absolute_index].kill = decay(director_players[absolute_index].kill, KILL_DECAY, seconds);
		director_players[absolute_index].fight = decay(director_players[absolute_index].fight, FIGHT_DECAY, seconds);
		director_players[absolute_index].hurt = decay(director_players[absolute_index].hurt, HURT_DECAY, seconds);
		if (iterator.datum_index != director.watched)
			director_players[absolute_index].unshown_seconds += seconds;
		if (player->statistics.kills[0] > director_players[absolute_index].kills)
		{
			director_players[absolute_index].kill = 1.0f;
			director_players[absolute_index].kill_tick = now;
		}
		director_players[absolute_index].kills = player->statistics.kills[0];
		if (player->statistics.deaths > director_players[absolute_index].deaths ||
			(director_players[absolute_index].alive && !unit))
		{
			director_players[absolute_index].death_tick = now;
		}
		director_players[absolute_index].deaths = player->statistics.deaths;
		director_players[absolute_index].alive = unit != NULL;
		director_players[absolute_index].objective = 0.0f;
		director_players[absolute_index].vehicle = 0.0f;
		director_players[absolute_index].near = 0.0f;
		if (!unit)
			continue;

		/* fighting: their weapon fired lately, or they were hurt */
		if (unit->unit.current_weapon_index >= 0 && unit->unit.current_weapon_index < 4)
		{
			long weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];

			if (weapon_index != NONE && object_try_and_get_and_verify_type(weapon_index, _object_mask_weapon))
			{
				long fired = weapon_get(weapon_index)->weapon.game_time_last_fired;

				if (fired <= now && now - fired <= FIRING_TICKS)
					director_players[absolute_index].fight = 1.0f;
				if (weapon_is_flag(weapon_index))
					director_players[absolute_index].objective = 1.0f;
			}
		}
		if (unit->object.body_vitality < director_players[absolute_index].body - 0.001f ||
			unit->object.shield_vitality < director_players[absolute_index].shield - 0.001f)
		{
			director_players[absolute_index].hurt = 1.0f;
		}
		director_players[absolute_index].body = unit->object.body_vitality;
		director_players[absolute_index].shield = unit->object.shield_vitality;
		/* driving fast */
		if (unit->object.parent_object_index != NONE)
		{
			struct object_datum *vehicle = object_try_and_get(unit->object.parent_object_index);

			if (vehicle)
			{
				real_vector3d const *velocity = &vehicle->object.translational_velocity;

				if (velocity->i * velocity->i + velocity->j * velocity->j + velocity->k * velocity->k >
					FAST_VEHICLE * FAST_VEHICLE)
				{
					director_players[absolute_index].vehicle = 1.0f;
				}
			}
		}
		/* enemies close by */
		{
			struct data_iterator others;
			struct player_datum *other;

			data_iterator_new(&others, player_data);
			while ((other = data_iterator_next(&others)) != NULL)
			{
				long other_unit_index = living_unit(other);
				struct object_datum *other_unit;
				real dx, dy, dz;

				if (others.datum_index == iterator.datum_index || other_unit_index == NONE)
					continue;
				other_unit = object_get(other_unit_index);
				if (!game_team_is_enemy(unit->object.owner_team_index, other_unit->object.owner_team_index))
					continue;
				dx = other_unit->object.position.x - unit->object.position.x;
				dy = other_unit->object.position.y - unit->object.position.y;
				dz = other_unit->object.position.z - unit->object.position.z;
				if (dx * dx + dy * dy + dz * dz < NEAR_DISTANCE * NEAR_DISTANCE)
					director_players[absolute_index].near += 1.0f;
			}
		}
	}
}

/* the most interesting living player but `except`, and why (NONE: none alive) */
static long most_interesting(
	long except,
	real *best_interest,
	short *best_reason)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long best = NONE;

	*best_interest = -1.0f;
	*best_reason = _reason_turn;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		short reason;
		real value;

		if (iterator.datum_index == except || living_unit(player) == NONE ||
			absolute_index < 0 || absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		{
			continue;
		}
		value = interest(absolute_index, &reason);
		if (value > *best_interest)
		{
			*best_interest = value;
			*best_reason = reason;
			best = iterator.datum_index;
		}
	}
	return best;
}

/* a cut to the player: logged, and whether the camera glides there */
static long cut(
	long player_index,
	short reason,
	boolean *glide)
{
	char name[16];
	long from_unit = director.watched != NONE ? living_unit(player_try_and_get(director.watched)) : NONE;
	long to_unit = player_index != NONE ? living_unit(player_get(player_index)) : NONE;

	*glide = FALSE;
	if (from_unit != NONE && to_unit != NONE)
	{
		real_point3d from, to;

		object_get_origin(from_unit, &from);
		object_get_origin(to_unit, &to);
		*glide = (from.x - to.x) * (from.x - to.x) + (from.y - to.y) * (from.y - to.y) +
			(from.z - to.z) * (from.z - to.z) <= GLIDE_DISTANCE * GLIDE_DISTANCE;
	}
	name[0] = 0;
	if (player_index != NONE)
	{
		wide_to_ascii(player_get(player_index)->name, name, sizeof(name));
		director_players[DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index)].unshown_seconds = 0.0f;
	}
	platform_log("spectate: director cut to %s (player %ld, %s) at tick %ld after %.1f s, %s", name,
		player_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), reason_names[reason],
		game_time_get(), director.shot_seconds, *glide ? "glide" : "hard cut");
	director.watched = player_index;
	director.shot_seconds = 0.0f;
	director.hold_seconds = 0.0f;
	return player_index;
}

/* ---------- public code */

void spectate_director_reset(
	void)
{
	csmemset(director_players, 0, sizeof(director_players));
	csmemset(&director, 0, sizeof(director));
	director.watched = NONE;
	director.last_tick = NONE;
}

long spectate_director_update(
	real seconds,
	long watched,
	boolean *glide)
{
	long now = game_time_get();
	real best_interest;
	short best_reason;
	long best;

	*glide = FALSE;
	/* (someone else chose whom to watch, by hand: the shot starts from them) */
	if (watched != director.watched)
	{
		director.watched = watched;
		director.shot_seconds = 0.0f;
		director.hold_seconds = 0.0f;
	}
	if (director.last_tick != now)
	{
		real tick_seconds = director.last_tick == NONE || now < director.last_tick ? 0.0f :
			(real)(now - director.last_tick) / (real)TICKS_PER_SECOND;

		director.last_tick = now;
		director_tick(tick_seconds);
	}
	director.shot_seconds += seconds;

	/* the one watched has left the game */
	if (director.watched != NONE && !player_try_and_get(director.watched))
	{
		director.watched = NONE;
		best = most_interesting(NONE, &best_interest, &best_reason);
		return best != NONE ? cut(best, _reason_left, glide) : NONE;
	}
	/* nobody watched yet (the game's start, or nobody was alive) */
	if (director.watched == NONE)
	{
		director.shot_seconds = 0.0f;
		best = most_interesting(NONE, &best_interest, &best_reason);
		return best != NONE ? cut(best, director.started ? best_reason : _reason_start, glide) :
			NONE;
	}
	director.started = TRUE;
	/* the one watched died: their fall held a moment, then their killer */
	if (living_unit(player_get(director.watched)) == NONE)
	{
		long killer;

		if (director.hold_seconds <= 0.0f)
			director.hold_seconds = KILL_HOLD;
		director.hold_seconds -= seconds;
		if (director.hold_seconds > 0.0f)
			return director.watched;
		killer = killer_of(DATUM_INDEX_TO_ABSOLUTE_INDEX(director.watched));
		if (killer != NONE && player_try_and_get(killer) && living_unit(player_get(killer)) != NONE)
			return cut(killer, _reason_killer, glide);
		best = most_interesting(director.watched, &best_interest, &best_reason);
		return best != NONE ? cut(best, _reason_died, glide) : director.watched;
	}
	if (director.shot_seconds < MINIMUM_SHOT)
		return director.watched;
	best = most_interesting(director.watched, &best_interest, &best_reason);
	if (best == NONE)
		return director.watched;
	if (director.shot_seconds >= MAXIMUM_SHOT)
		return cut(best, _reason_time, glide);
	{
		short reason;
		real current = interest(DATUM_INDEX_TO_ABSOLUTE_INDEX(director.watched), &reason);

		if (best_interest > current * CUT_RATIO + CUT_MARGIN)
			return cut(best, best_reason, glide);
	}
	return director.watched;
}
