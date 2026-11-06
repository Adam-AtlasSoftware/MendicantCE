/*
FORGE_AI.C

One of Forge's parts (halo_mod.h, halo_mods.c): AI characters in any map, from the dev tools'
menu (in forge mode: 1, D-pad right or X), on the "AI" tab. A keyboard and
a controller work alike.

- Character: the actor variants the map has (tags of the group actv whose
  actor and biped are there): in a multiplayer map only those the launcher
  brings in from another map (mods.json "import", the actv group; the
  launcher's Ctrl-T list), such as the grunts, hunters and elites of the
  campaign.
- Spawn at the crosshair: a character stands there as the map's own
  creatures do (the biped is made, dressed as the variant says and given an
  actor with the game's own ai_attach_free), so it sees, shoots, takes cover
  and dies as in the campaign.
- Faction: the team new characters are on: their own (a grunt's body is
  the Covenant's, a combat form's the Flood's), the player's side, or the
  human, Covenant, Flood or sentinel team of the campaign. Every team is the
  enemy of every other (game/game_allegiance.c), so characters of two
  factions fight each other as they fight the player, with the game's own
  perception and combat; those on the player's side fight the rest.
- Aggression: cautious characters defend and never charge, normal ones fight
  as their actor says, berserk ones charge once they fight.
- Squad: how many are added at once, at the crosshair. The first
  leads (it patrols the waypoints); the others follow it in a V while
  nothing draws them away, and whatever one of them fights, the rest are
  made to see (the game's magic sight), so the squad fights together.
- Attack on sight: patrolling characters break off to fight an enemy they
  see or hear (the command list's initiative and targeting), and go back to
  their patrol once the fight is over and they are idle again. Off, they
  walk their round whatever happens.
- Waypoints: "add a waypoint" puts a point at the crosshair; the points are
  drawn in the world. With two or more, characters walk from one to the next
  and round again, pausing at each, until something draws their attention:
  the game's own command list (ai_command_lists of the scenario, one made
  here for each map, with go to and pause commands and a loop) sends them, so
  the game's pathfinding, which multiplayer maps have, takes them round walls
  and up ramps.
- New characters (a squad's leader) patrol along the points by themselves, or
  "send everyone" does it for the ones already there; "remove all" takes
  them away.
- A waypoint is picked up like an object while the waypoints are shown:
  aim at it and press A / enter (flying) or F4, aim to move it, A / enter
  puts it down (on the floor there), B / escape puts it back, Y / delete
  removes it. The characters then walk the changed round.

The points and characters are forgotten when the map changes; nothing is
kept for other players: local games only.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "ai/actor_definitions.h"
#include "ai/actors.h"
#include "ai/ai_scenario_definitions.h"
#include "ai/ai.h"
#include "ai/ai_script.h"
#include "ai/props.h"
#include "cache/cache_files.h"
#include "camera/observer.h"
#include "cseries/cseries_windows.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the game's malloc and free are debugging macros; this mod's blocks belong
to the scenario for a whole map, so they are the C library's */
#undef malloc
#undef free

/* ---------- constants */

enum
{
	FORGE_AI_LOCAL_PLAYER_INDEX = 0,
	FORGE_AI_MAXIMUM_CHARACTERS = 48,
	FORGE_AI_MAXIMUM_WAYPOINTS = 24,
	FORGE_AI_MAXIMUM_UNITS = 64,
	/* a go to and a pause for each point, and one loop */
	FORGE_AI_MAXIMUM_COMMANDS = 2 * FORGE_AI_MAXIMUM_WAYPOINTS + 1,
	/* how far below a point the ground is looked for, and how far above it it starts */
	FORGE_AI_GROUND_SEARCH_UP = 1,
	FORGE_AI_GROUND_SEARCH_DOWN = 4,
	FORGE_AI_PAUSE_SECONDS = 2,
	/* how often idle characters are sent back to their patrol */
	FORGE_AI_REPATROL_MILLISECONDS = 3000,
	/* how often squads are kept together, and the aggression applied */
	FORGE_AI_SQUAD_MILLISECONDS = 1000,
	FORGE_AI_MAXIMUM_SQUAD_SIZE = 8,
	FORGE_AI_MAXIMUM_SQUAD_ID = 30000,
	/* the game's teams (game/game_allegiance.c) */
	FORGE_AI_TEAM_COUNT = 10,
	FORGE_AI_TEAM_HUMAN = 2,
	FORGE_AI_TEAM_COVENANT = 3,
	FORGE_AI_TEAM_FLOOD = 4,
	FORGE_AI_TEAM_SENTINEL = 5,
	/* the actor's combat status at which it knows of a target, and the unit
	effect of magic sight (ai/actors.c and ai/ai_script.c have them) */
	FORGE_AI_COMBAT_STATUS_WARY = 1,
	FORGE_AI_COMBAT_STATUS_DEFINITE = 3,
	FORGE_AI_UNIT_EFFECT_MAGIC_SIGHT = 3
};

/* the space between a squad's characters, and how far one may be from its
place behind the leader before it is sent there */
#define FORGE_AI_SQUAD_SPACING 2.f
#define FORGE_AI_FOLLOW_DISTANCE 3.f

/* a waypoint is aimed at within this far of the middle of its marker */
#define FORGE_AI_WAYPOINT_PICK_RADIUS 0.7f
#define FORGE_AI_WAYPOINT_PICK_HEIGHT 0.6f
/* how far from the origin a layout's line may put a waypoint or a
character (world units) */
#define FORGE_AI_WORLD_BOUND 32768.f

#define FORGE_AI_WAYPOINT_COLOR_R 1.f
#define FORGE_AI_WAYPOINT_COLOR_G 0.75f
#define FORGE_AI_WAYPOINT_COLOR_B 0.1f

enum
{
	_forge_ai_faction_own = 0,
	_forge_ai_faction_player,
	_forge_ai_faction_human,
	_forge_ai_faction_covenant,
	_forge_ai_faction_flood,
	_forge_ai_faction_sentinel,
	NUMBER_OF_FORGE_AI_FACTIONS
};

static char const *const forge_ai_faction_names[NUMBER_OF_FORGE_AI_FACTIONS] =
{
	"their own", "the player's side", "human", "covenant", "flood", "sentinel"
};

enum
{
	_forge_ai_aggression_cautious = 0,
	_forge_ai_aggression_normal,
	_forge_ai_aggression_berserk,
	NUMBER_OF_FORGE_AI_AGGRESSIONS
};

static char const *const forge_ai_aggression_names[NUMBER_OF_FORGE_AI_AGGRESSIONS] =
{
	"cautious (defend, no charging)", "normal", "berserk (charge)"
};

enum
{
	_forge_ai_row_character = 0,
	_forge_ai_row_faction,
	_forge_ai_row_aggression,
	_forge_ai_row_squad_size,
	_forge_ai_row_attack,
	_forge_ai_row_spawn,
	_forge_ai_row_add_waypoint,
	_forge_ai_row_move_waypoint,
	_forge_ai_row_show_waypoints,
	_forge_ai_row_clear_waypoints,
	_forge_ai_row_patrol,
	_forge_ai_row_send,
	_forge_ai_row_remove,
	NUMBER_OF_FORGE_AI_ROWS
};

/* ---------- globals */

static struct
{
	/* the actor variants that can be spawned, by name */
	long characters[FORGE_AI_MAXIMUM_CHARACTERS];
	short character_count;
	short character_index;

	real_point3d waypoints[FORGE_AI_MAXIMUM_WAYPOINTS];
	long waypoint_surfaces[FORGE_AI_MAXIMUM_WAYPOINTS];
	short waypoint_count;
	boolean show_waypoints;
	boolean patrol_new;
	/* new characters' faction, aggression, and how many are added at once */
	short faction;
	/* patrols break off to fight */
	boolean attack_on_sight;
	short aggression;
	short squad_size;
	unsigned long repatrol_milliseconds;
	unsigned long squad_milliseconds;
	/* the id of the next squad */
	short next_squad;

	/* what was spawned: the units (object indices), their actor variants,
	factions and aggressions, their squads (NONE alone), and where each
	follower was last sent */
	long units[FORGE_AI_MAXIMUM_UNITS];
	long unit_variants[FORGE_AI_MAXIMUM_UNITS];
	short unit_factions[FORGE_AI_MAXIMUM_UNITS];
	short unit_aggressions[FORGE_AI_MAXIMUM_UNITS];
	short unit_squads[FORGE_AI_MAXIMUM_UNITS];
	real_point3d unit_follow_points[FORGE_AI_MAXIMUM_UNITS];
	short unit_count;

	/* the scenario's command lists with ours at the end, made for this map,
	and the previous map's (freed when the next map's is made) */
	struct ai_command_list_definition *command_lists;
	struct ai_command_list_definition *previous_command_lists;
	struct ai_command_definition commands[FORGE_AI_MAXIMUM_COMMANDS];
	struct ai_command_point_definition points[FORGE_AI_MAXIMUM_WAYPOINTS];
	short command_list_index;
	/* after ours, one list for each unit's place in units, to send a squad's
	follower behind its leader: a go to its one point */
	struct ai_command_definition follow_commands[FORGE_AI_MAXIMUM_UNITS];
	struct ai_command_point_definition follow_points[FORGE_AI_MAXIMUM_UNITS];
} forge_ai_globals = { { 0 }, 0, 0, { { 0 } }, { 0 }, 0, TRUE, TRUE, _forge_ai_faction_own, TRUE,
	_forge_ai_aggression_normal, 1 };

/* ---------- private code */

static boolean forge_ai_local_game(
	void)
{
	/* (or the host of a system link game: the AI runs there, and its
	characters reach the clients as its other units do) */
	return halo_mods_authoritative() &&
		local_player_get_player_index(FORGE_AI_LOCAL_PLAYER_INDEX) != NONE;
}

/* the last part of a tag's name */
static char const *forge_ai_short_name(
	long tag_index)
{
	char const *name = tag_get_name(tag_index);
	char const *separator = strrchr(name, '\\');

	return separator ? separator + 1 : name;
}

/* the map's actor variants that have an actor and a biped, by name */
static void forge_ai_find_characters(
	void)
{
	struct tag_iterator iterator;
	long tag_index;

	forge_ai_globals.character_count = 0;
	tag_iterator_new(&iterator, ACTOR_VARIANT_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE &&
		forge_ai_globals.character_count < FORGE_AI_MAXIMUM_CHARACTERS)
	{
		struct actor_variant_definition *variant = actor_variant_definition_get(tag_index);
		struct actor_definition *actor = variant->actor_reference.index != NONE
			? actor_definition_get(variant->actor_reference.index)
			: NULL;

		if (actor && variant->unit_reference.index != NONE &&
			!TEST_FLAG(actor->flags, _actor_definition_swarm_actor_bit))
		{
			short position = forge_ai_globals.character_count++;

			/* insertion sort by name */
			while (position > 0 &&
				_stricmp(tag_get_name(forge_ai_globals.characters[position - 1]), tag_get_name(tag_index)) > 0)
			{
				forge_ai_globals.characters[position] = forge_ai_globals.characters[position - 1];
				position--;
			}
			forge_ai_globals.characters[position] = tag_index;
		}
	}
	if (forge_ai_globals.character_index >= forge_ai_globals.character_count)
		forge_ai_globals.character_index = 0;

	return;
}

/* the units still there, packed */
static short forge_ai_living_units(
	void)
{
	short read;
	short write = 0;

	for (read = 0; read < forge_ai_globals.unit_count; read++)
	{
		if (object_try_and_get(forge_ai_globals.units[read]))
		{
			forge_ai_globals.units[write] = forge_ai_globals.units[read];
			forge_ai_globals.unit_variants[write] = forge_ai_globals.unit_variants[read];
			forge_ai_globals.unit_factions[write] = forge_ai_globals.unit_factions[read];
			forge_ai_globals.unit_aggressions[write] = forge_ai_globals.unit_aggressions[read];
			forge_ai_globals.unit_squads[write] = forge_ai_globals.unit_squads[read];
			forge_ai_globals.unit_follow_points[write] = forge_ai_globals.unit_follow_points[read];
			write++;
		}
	}
	forge_ai_globals.unit_count = write;

	return write;
}

/* the collision surface (the pathfinding surface) below a point, or NONE */
static long forge_ai_ground_surface(
	real_point3d *point)
{
	struct collision_result collision;
	real_point3d origin = *point;
	real_vector3d ray;

	origin.z += FORGE_AI_GROUND_SEARCH_UP;
	ray.i = 0.f;
	ray.j = 0.f;
	ray.k = -(real)(FORGE_AI_GROUND_SEARCH_UP + FORGE_AI_GROUND_SEARCH_DOWN);
	if (collision_test_vector(
		FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&origin,
		&ray,
		NONE,
		&collision) &&
		collision.type == _collision_result_structure)
	{
		*point = collision.point;
		return collision.surface_index;
	}

	return NONE;
}

/* ---------- the command list

The scenario of a multiplayer map has no command lists. A block cannot grow
(tag_block_resize is not supported with a cache file), but the block is only
a count and an address, so each map gets a copy of its list with one more at
the end, ours, whose commands and points are arrays here that change when
the waypoints do, and after it the followers' lists. */

/* the command list array in use is no longer this map's: it is kept a map
longer (a stale pointer to it could be in a command list still running for a
moment), and the one before it is freed */
static void forge_ai_retire_command_lists(
	void)
{
	free(forge_ai_globals.previous_command_lists);
	forge_ai_globals.previous_command_lists = forge_ai_globals.command_lists;
	forge_ai_globals.command_lists = NULL;
	forge_ai_globals.command_list_index = NONE;

	return;
}

/* makes the command lists of this map: its own plus ours. A map reset
(main/main.c, main_reset_map_private) starts the map again without loading
its tags, so its scenario has ours already: it is kept, not copied again with
another of ours at the end. */
static void forge_ai_install_command_list(
	void)
{
	struct scenario *scenario = global_scenario_get();
	long count = scenario->ai_command_lists.count;
	long ours = 1 + FORGE_AI_MAXIMUM_UNITS;
	struct ai_command_list_definition *lists;
	struct ai_command_list_definition *list;
	short index;

	if (forge_ai_globals.command_lists && count >= ours &&
		scenario->ai_command_lists.address == forge_ai_globals.command_lists)
	{
		forge_ai_globals.command_list_index = (short)(count - ours);
		return;
	}

	forge_ai_retire_command_lists();
	lists = (struct ai_command_list_definition *)calloc(count + ours, sizeof(struct ai_command_list_definition));
	if (!lists)
		return;
	if (count > 0)
		memcpy(lists, scenario->ai_command_lists.address, count * sizeof(struct ai_command_list_definition));
	for (index = 0; index < ours; index++)
	{
		list = &lists[count + index];
		strcpy(list->name, index == 0 ? "forge_ai_patrol" : "forge_ai_follow");
		list->manual_structure_bsp_reference_index = global_structure_bsp_index;
		list->runtime_structure_bsp_reference_index = global_structure_bsp_index;
		list->commands.address = index == 0 ? forge_ai_globals.commands : &forge_ai_globals.follow_commands[index - 1];
		list->points.address = index == 0 ? forge_ai_globals.points : &forge_ai_globals.follow_points[index - 1];
	}

	forge_ai_globals.command_lists = lists;
	forge_ai_globals.command_list_index = (short)count;
	scenario->ai_command_lists.count = count + ours;
	scenario->ai_command_lists.address = lists;

	return;
}

/* with initiative and targeting those sent stop to fight what they see */
static long forge_ai_command_list_flags(
	void)
{
	return forge_ai_globals.attack_on_sight
		? FLAG(_ai_command_list_allow_initiative_bit) | FLAG(_ai_command_list_allow_targeting_bit)
		: 0;
}

/* a go to the point, which must have a surface (the command list's) */
static void forge_ai_go_to_command(
	struct ai_command_definition *go_to,
	short point_index)
{
	memset(go_to, 0, sizeof(*go_to));
	go_to->atom_type = _ai_atom_go_to;
	go_to->atom_modifier = _ai_atom_go_to_modifier_stop_at_point;
	go_to->point1_index = point_index;
	go_to->point2_index = NONE;
	go_to->animation_reference_index = NONE;
	go_to->script_reference_index = NONE;
	go_to->recording_reference_index = NONE;
	go_to->command_index = NONE;
	go_to->object_name_index = NONE;

	return;
}

/* the commands for the waypoints: go to each and pause there, then again from the first */
static void forge_ai_update_command_list(
	void)
{
	struct ai_command_list_definition *list = forge_ai_globals.command_lists
		? &forge_ai_globals.command_lists[forge_ai_globals.command_list_index]
		: NULL;
	short index;
	short command_count = 0;

	if (!list)
		return;
	list->flags = forge_ai_command_list_flags();
	for (index = 0; index < forge_ai_globals.waypoint_count; index++)
	{
		struct ai_command_definition *go_to = &forge_ai_globals.commands[command_count++];
		struct ai_command_definition *pause = &forge_ai_globals.commands[command_count++];

		forge_ai_go_to_command(go_to, index);
		*pause = *go_to;
		pause->atom_type = _ai_atom_pause;
		pause->atom_modifier = 0;
		pause->parameter1 = FORGE_AI_PAUSE_SECONDS;
		pause->point1_index = NONE;
		forge_ai_globals.points[index].position = forge_ai_globals.waypoints[index];
		forge_ai_globals.points[index].surface_index = forge_ai_globals.waypoint_surfaces[index];
		forge_ai_globals.points[index].unused = 0;
	}
	if (index > 0)
	{
		struct ai_command_definition *loop = &forge_ai_globals.commands[command_count++];

		memset(loop, 0, sizeof(*loop));
		loop->atom_type = _ai_atom_loop;
		loop->atom_modifier = 0;
		loop->point1_index = NONE;
		loop->point2_index = NONE;
		loop->animation_reference_index = NONE;
		loop->script_reference_index = NONE;
		loop->recording_reference_index = NONE;
		loop->command_index = 0;
		loop->object_name_index = NONE;
	}
	list->commands.count = command_count;
	list->points.count = forge_ai_globals.waypoint_count;

	return;
}

/* ---------- squads */

/* the actor of a living character in units, or NULL */
static struct actor_datum *forge_ai_unit_actor(
	short slot)
{
	struct unit_datum *unit = unit_try_and_get(forge_ai_globals.units[slot]);

	if (!unit || unit->object.body_vitality <= 0.f || unit->unit.actor_index == NONE)
		return NULL;

	return actor_get(unit->unit.actor_index);
}

/* the squad's leader: its first living character in units, or NONE */
static short forge_ai_squad_leader(
	short squad)
{
	short slot;

	for (slot = 0; slot < forge_ai_globals.unit_count; slot++)
	{
		if (forge_ai_globals.unit_squads[slot] == squad && forge_ai_unit_actor(slot))
			return slot;
	}

	return NONE;
}

/* a character that follows its squad's leader rather than leading or
patrolling */
static boolean forge_ai_is_follower(
	short slot)
{
	short squad = forge_ai_globals.unit_squads[slot];

	return squad != NONE && forge_ai_squad_leader(squad) != slot;
}

/* an actor with nothing to fight and nothing else to do than its orders */
static boolean forge_ai_actor_idle(
	struct actor_datum *actor,
	boolean obeying)
{
	short action = actor->state.action;

	return actor->state.combat_status <= FORGE_AI_COMBAT_STATUS_WARY &&
		(action == _actor_action_none || action == _actor_action_alert || action == _actor_action_guard ||
			action == _actor_action_wait || (obeying && action == _actor_action_obey));
}

/* sends the character at the slot to the point, with its own command list;
FALSE when there is no floor there */
static boolean forge_ai_follow(
	short slot,
	real_point3d const *point)
{
	short list_index = (short)(forge_ai_globals.command_list_index + 1 + slot);
	struct ai_command_list_definition *list;
	real_point3d floor = *point;
	long surface_index;

	if (!forge_ai_globals.command_lists || (surface_index = forge_ai_ground_surface(&floor)) == NONE)
		return FALSE;
	list = &forge_ai_globals.command_lists[list_index];
	list->flags = forge_ai_command_list_flags();
	forge_ai_go_to_command(&forge_ai_globals.follow_commands[slot], 0);
	forge_ai_globals.follow_points[slot].position = floor;
	forge_ai_globals.follow_points[slot].surface_index = surface_index;
	forge_ai_globals.follow_points[slot].unused = 0;
	list->commands.count = 1;
	list->points.count = 1;
	forge_ai_globals.unit_follow_points[slot] = floor;
	ai_scripting_command_list_by_unit(forge_ai_globals.units[slot], list_index);

	return TRUE;
}

/* the rank'th follower's place behind a leader at the position, facing
forward: in a V, two by two, each row a step further back and out (a
follower is a pathfinding obstacle: a ring of them round the leader would
wall it in) */
static void forge_ai_squad_place(
	real_point3d const *position,
	real_vector3d const *leader_forward,
	short rank,
	real_point3d *place)
{
	real_vector3d forward = *leader_forward;
	real row = (real)((rank + 1) / 2);
	real side = (rank & 1) ? 1.f : -1.f;

	forward.k = 0.f;
	if (forward.i * forward.i + forward.j * forward.j < 0.0001f)
		forward = *global_forward3d;
	else
		normalize3d(&forward);
	*place = *position;
	/* (left of forward is (-j, i)) */
	place->x += -forward.i * FORGE_AI_SQUAD_SPACING * row - forward.j * FORGE_AI_SQUAD_SPACING * row * side;
	place->y += -forward.j * FORGE_AI_SQUAD_SPACING * row + forward.i * FORGE_AI_SQUAD_SPACING * row * side;

	return;
}

/* whatever a squad's character fights, the others of its squad are made to
see (magic sight lasts five seconds: this comes round every second) */
static void forge_ai_squad_share_target(
	short squad)
{
	long target_unit_index = NONE;
	short slot;

	for (slot = 0; slot < forge_ai_globals.unit_count && target_unit_index == NONE; slot++)
	{
		struct actor_datum *actor = forge_ai_globals.unit_squads[slot] == squad ? forge_ai_unit_actor(slot) : NULL;
		struct prop_datum *prop;

		if (actor && actor->state.combat_status >= FORGE_AI_COMBAT_STATUS_DEFINITE &&
			actor->target.target_prop_index != NONE &&
			(prop = prop_try_and_get(actor->target.target_prop_index)) != NULL &&
			prop->unit_index != NONE && unit_try_and_get(prop->unit_index) &&
			unit_get(prop->unit_index)->object.body_vitality > 0.f)
		{
			target_unit_index = prop->unit_index;
		}
	}
	if (target_unit_index == NONE)
		return;
	for (slot = 0; slot < forge_ai_globals.unit_count; slot++)
	{
		struct actor_datum *actor = forge_ai_globals.unit_squads[slot] == squad ? forge_ai_unit_actor(slot) : NULL;
		long actor_index;
		long prop_index;

		if (!actor || actor->state.combat_status >= FORGE_AI_COMBAT_STATUS_DEFINITE)
			continue;
		actor_index = unit_get(forge_ai_globals.units[slot])->unit.actor_index;
		prop_index = prop_get_base_by_unit_index(actor_index, target_unit_index, TRUE, FALSE);
		if (prop_index != NONE)
			actor_handle_unit_effect(actor_index, prop_index, FORGE_AI_UNIT_EFFECT_MAGIC_SIGHT);
	}

	return;
}

/* cautious: defending, never charging; normal: neither; berserk: charging
once it fights (the game ends a berserk when the fight does) */
static void forge_ai_apply_aggression(
	short slot)
{
	struct actor_datum *actor = forge_ai_unit_actor(slot);
	short aggression = forge_ai_globals.unit_aggressions[slot];

	if (!actor)
		return;
	actor->external_orders.defending = aggression == _forge_ai_aggression_cautious;
	actor->external_orders.disable_charging = aggression == _forge_ai_aggression_cautious;
	if (aggression == _forge_ai_aggression_cautious && actor->emotions.berserk)
	{
		actor_berserk(unit_get(forge_ai_globals.units[slot])->unit.actor_index, FALSE);
	}
	else if (aggression == _forge_ai_aggression_berserk && !actor->emotions.berserk &&
		actor->state.combat_status >= FORGE_AI_COMBAT_STATUS_DEFINITE)
	{
		actor_berserk(unit_get(forge_ai_globals.units[slot])->unit.actor_index, TRUE);
	}

	return;
}

/* each second: the squads together, each fighting what one of it fights,
the followers behind their leaders while nothing else draws them, and
everyone fighting as their aggression says */
static void forge_ai_update_squads(
	void)
{
	short slot;
	/* (counted once: counting packs the list) */
	short unit_count = forge_ai_living_units();

	for (slot = 0; slot < unit_count; slot++)
	{
		short squad = forge_ai_globals.unit_squads[slot];
		struct actor_datum *actor;
		short leader_slot;
		short rank;
		short other;
		real_point3d place;
		real_point3d const *position;

		forge_ai_apply_aggression(slot);
		if (squad == NONE || !(actor = forge_ai_unit_actor(slot)))
			continue;
		leader_slot = forge_ai_squad_leader(squad);
		if (leader_slot == slot)
		{
			/* (once a squad, by its leader) */
			if (forge_ai_globals.attack_on_sight)
				forge_ai_squad_share_target(squad);
			continue;
		}
		if (!forge_ai_actor_idle(actor, TRUE))
			continue;
		for (other = 0, rank = 1; other < slot; other++)
		{
			if (other != leader_slot && forge_ai_globals.unit_squads[other] == squad && forge_ai_unit_actor(other))
				rank++;
		}
		forge_ai_squad_place(&object_get(forge_ai_globals.units[leader_slot])->object.position,
			&object_get(forge_ai_globals.units[leader_slot])->object.forward, rank, &place);
		position = &object_get(forge_ai_globals.units[slot])->object.position;
		/* sent again when it is away from its place and not already going
		there */
		if (distance3d(position, &place) > FORGE_AI_FOLLOW_DISTANCE &&
			(actor->state.action != _actor_action_obey ||
				distance3d(&forge_ai_globals.unit_follow_points[slot], &place) > FORGE_AI_FOLLOW_DISTANCE / 2.f))
		{
			forge_ai_follow(slot, &place);
		}
	}

	return;
}

/* sends the living characters along the waypoints (not the followers of a
squad: they follow) */
static short forge_ai_send_patrols(
	void)
{
	short index;
	/* (counted once: counting packs the list) */
	short unit_count;
	short sent = 0;

	if (forge_ai_globals.waypoint_count < 2 || !forge_ai_globals.command_lists)
		return 0;
	for (index = 0, unit_count = forge_ai_living_units(); index < unit_count; index++)
	{
		if (forge_ai_is_follower(index))
			continue;
		ai_scripting_command_list_by_unit(forge_ai_globals.units[index], forge_ai_globals.command_list_index);
		sent++;
	}

	return sent;
}

/* the team of local player 0's unit, 0 without one */
static short forge_ai_player_team(
	void)
{
	long player_index = local_player_get_player_index(FORGE_AI_LOCAL_PLAYER_INDEX);
	long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;
	struct unit_datum *unit = unit_index != NONE ? unit_try_and_get(unit_index) : NULL;

	return unit ? unit->object.owner_team_index : 0;
}

/* characters that fought and are idle again walk their patrol again */
static void forge_ai_repatrol(
	void)
{
	short index;
	/* (counted once: counting packs the list) */
	short unit_count;

	if (forge_ai_globals.waypoint_count < 2 || !forge_ai_globals.command_lists || !forge_ai_globals.patrol_new)
		return;
	for (index = 0, unit_count = forge_ai_living_units(); index < unit_count; index++)
	{
		struct actor_datum *actor = forge_ai_unit_actor(index);

		if (actor && !forge_ai_is_follower(index) && forge_ai_actor_idle(actor, FALSE))
		{
			ai_scripting_command_list_by_unit(forge_ai_globals.units[index], forge_ai_globals.command_list_index);
		}
	}

	return;
}

/* ---------- the actions */

/* the team of a faction; NONE for the character's own */
static short forge_ai_faction_team(
	short faction)
{
	switch (faction)
	{
	case _forge_ai_faction_player:
		return forge_ai_player_team();
	case _forge_ai_faction_human:
		return FORGE_AI_TEAM_HUMAN;
	case _forge_ai_faction_covenant:
		return FORGE_AI_TEAM_COVENANT;
	case _forge_ai_faction_flood:
		return FORGE_AI_TEAM_FLOOD;
	case _forge_ai_faction_sentinel:
		return FORGE_AI_TEAM_SENTINEL;
	}

	return NONE;
}

/* makes a character of the variant, standing at the position, facing
forward, of the faction, the aggression and the squad (NONE alone); NONE
when the game cannot */
static long forge_ai_create(
	long variant_index,
	real_point3d const *position,
	real_vector3d const *forward,
	short faction,
	short aggression,
	short squad)
{
	struct actor_variant_definition *variant = actor_variant_definition_get(variant_index);
	struct object_placement_data data;
	long unit_index;

	if (forge_ai_living_units() >= FORGE_AI_MAXIMUM_UNITS)
	{
		terminal_printf(global_real_argb_orange, "forge_ai: too many characters");
		return NONE;
	}
	object_placement_data_new(&data, variant->unit_reference.index, NONE);
	data.position = *position;
	data.forward = *forward;
	data.forward.k = 0.f;
	if (data.forward.i * data.forward.i + data.forward.j * data.forward.j < 0.0001f)
		data.forward = *global_forward3d;
	else
		normalize3d(&data.forward);
	data.up = *global_up3d;
	unit_index = object_new(&data);
	if (unit_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_ai: %s could not be made", forge_ai_short_name(variant_index));
		return NONE;
	}
	actor_customize_unit(variant_index, unit_index);
	{
		/* the actor takes the unit's team when it is attached */
		struct unit_datum *unit = unit_get(unit_index);
		short player_team = forge_ai_player_team();
		short team = forge_ai_faction_team(faction);

		if (team != NONE)
			unit->object.owner_team_index = team;
		else if (unit->object.owner_team_index == player_team)
			/* its own team, which is the player's in this game: an enemy still */
			unit->object.owner_team_index = (short)((player_team + 1) % FORGE_AI_TEAM_COUNT);
	}
	ai_scripting_attach_free(unit_index, variant_index);
	forge_ai_globals.units[forge_ai_globals.unit_count] = unit_index;
	forge_ai_globals.unit_variants[forge_ai_globals.unit_count] = variant_index;
	forge_ai_globals.unit_factions[forge_ai_globals.unit_count] = faction;
	forge_ai_globals.unit_aggressions[forge_ai_globals.unit_count] = aggression;
	forge_ai_globals.unit_squads[forge_ai_globals.unit_count] = squad;
	forge_ai_globals.unit_follow_points[forge_ai_globals.unit_count] = *position;
	forge_ai_globals.unit_count++;
	forge_ai_apply_aggression((short)(forge_ai_globals.unit_count - 1));
	/* (a squad's first is its leader: the others follow it) */
	if (forge_ai_globals.patrol_new && forge_ai_globals.waypoint_count >= 2 && forge_ai_globals.command_lists &&
		!forge_ai_is_follower((short)(forge_ai_globals.unit_count - 1)))
	{
		ai_scripting_command_list_by_unit(unit_index, forge_ai_globals.command_list_index);
	}

	return unit_index;
}

/* a new squad's id; NONE for one character alone */
static short forge_ai_new_squad(
	short size)
{
	short squad = forge_ai_globals.next_squad;

	if (size <= 1)
		return NONE;
	forge_ai_globals.next_squad = (short)((squad + 1) % FORGE_AI_MAXIMUM_SQUAD_ID);

	return squad;
}

/* makes size characters of the variant, the first at the point, the others
in their places behind it on the floor there, all facing forward and of one
squad; how many were made */
static short forge_ai_create_squad(
	long variant_index,
	real_point3d const *point,
	real_vector3d const *forward,
	short faction,
	short aggression,
	short size)
{
	short squad = forge_ai_new_squad(size);
	real_point3d floor = *point;
	/* how far above the floor the point is (the placement's) */
	real lift = forge_ai_ground_surface(&floor) != NONE ? point->z - floor.z : 0.f;
	short made = 0;
	short index;
	short player_team = forge_ai_player_team();
	short team = forge_ai_faction_team(faction);

	for (index = 0; index < size; index++)
	{
		real_point3d position = *point;

		if (index > 0)
		{
			forge_ai_squad_place(point, forward, index, &position);
			floor = position;
			/* on the floor there, or (none) where the first stands */
			if (forge_ai_ground_surface(&floor) != NONE)
				position.z = floor.z + lift;
			else
				position = *point;
		}
		if (forge_ai_create(variant_index, &position, forward, faction, aggression, squad) != NONE)
			made++;
	}
	if (made > 0)
	{
		/* (the actors that know these already: they see their teams again) */
		ai_update_team_status();
		if (faction != _forge_ai_faction_player && team != NONE && team == player_team)
		{
			terminal_printf(global_real_argb_orange, "forge_ai: the %s team is the player's in this game: friends",
				forge_ai_faction_names[faction]);
		}
	}

	return made;
}

static void forge_ai_spawn(
	void)
{
	struct actor_variant_definition *variant;
	long variant_index;
	float position[3];
	struct observer_result const *camera = observer_get_camera(FORGE_AI_LOCAL_PLAYER_INDEX);
	real_point3d point;
	real_vector3d forward;
	short made;

	if (!forge_ai_local_game())
	{
		terminal_printf(global_real_argb_orange, "forge_ai: characters are made only in local games");
		return;
	}
	if (forge_ai_globals.character_count == 0)
	{
		terminal_printf(global_real_argb_orange,
			"forge_ai: this map has no characters (import one: the launcher, Ctrl-T, tags of the actv group)");
		return;
	}
	variant_index = forge_ai_globals.characters[forge_ai_globals.character_index];
	variant = actor_variant_definition_get(variant_index);
	if (!forge_placement_at_crosshair(variant->unit_reference.index, position))
	{
		terminal_printf(global_real_argb_orange, "forge_ai: the camera is outside the map");
		return;
	}
	set_real_point3d(&point, position[0], position[1], position[2]);
	/* facing the player */
	set_real_vector3d(&forward, -camera->forward.i, -camera->forward.j, 0.f);
	made = forge_ai_create_squad(variant_index, &point, &forward, forge_ai_globals.faction,
		forge_ai_globals.aggression, forge_ai_globals.squad_size);
	if (made > 0)
	{
		terminal_printf(global_real_argb_green, "forge_ai: %d %s (%s, %s)", (int)made, forge_ai_short_name(variant_index),
			forge_ai_faction_names[forge_ai_globals.faction], forge_ai_aggression_names[forge_ai_globals.aggression]);
	}

	return;
}

static void forge_ai_add_waypoint(
	void)
{
	float position[3];
	float normal[3];
	real_point3d point;

	if (!forge_ai_local_game())
	{
		terminal_printf(global_real_argb_orange, "forge_ai: waypoints are for local games");
		return;
	}
	if (forge_ai_globals.waypoint_count >= FORGE_AI_MAXIMUM_WAYPOINTS)
	{
		terminal_printf(global_real_argb_orange, "forge_ai: at most %d waypoints", FORGE_AI_MAXIMUM_WAYPOINTS);
		return;
	}
	if (!forge_point_at_crosshair(position, normal))
	{
		terminal_printf(global_real_argb_orange, "forge_ai: the camera is outside the map");
		return;
	}
	point.x = position[0];
	point.y = position[1];
	point.z = position[2];
	{
		long surface_index = forge_ai_ground_surface(&point);

		if (surface_index == NONE)
		{
			terminal_printf(global_real_argb_orange, "forge_ai: no floor there for a waypoint");
			return;
		}
		forge_ai_globals.waypoints[forge_ai_globals.waypoint_count] = point;
		forge_ai_globals.waypoint_surfaces[forge_ai_globals.waypoint_count] = surface_index;
	}
	forge_ai_globals.waypoint_count++;
	forge_ai_update_command_list();
	forge_ai_send_patrols();
	terminal_printf(global_real_argb_green, "forge_ai: waypoint %d", forge_ai_globals.waypoint_count);

	return;
}

/* ---------- picking up a waypoint */

static struct
{
	short waypoint_index;
	real_point3d original;
	long original_surface;
} forge_ai_hold_globals = { NONE };

/* the waypoint the crosshair points at, or NONE */
static short forge_ai_aimed_waypoint(
	void)
{
	struct observer_result const *camera = observer_get_camera(FORGE_AI_LOCAL_PLAYER_INDEX);
	real best_along = 1000.f;
	short best_index = NONE;
	short index;

	for (index = 0; index < forge_ai_globals.waypoint_count; index++)
	{
		real_point3d middle = forge_ai_globals.waypoints[index];
		real_vector3d to_point;
		real along;

		middle.z += FORGE_AI_WAYPOINT_PICK_HEIGHT;
		vector_from_points3d(&camera->position, &middle, &to_point);
		along = dot_product3d(&to_point, &camera->forward);
		if (along > 0.f && along < best_along &&
			magnitude_squared3d(&to_point) - along * along <=
				FORGE_AI_WAYPOINT_PICK_RADIUS * FORGE_AI_WAYPOINT_PICK_RADIUS)
		{
			best_along = along;
			best_index = index;
		}
	}

	return best_index;
}

static void forge_ai_remove_waypoint(
	short waypoint_index)
{
	short index;

	for (index = waypoint_index; index + 1 < forge_ai_globals.waypoint_count; index++)
	{
		forge_ai_globals.waypoints[index] = forge_ai_globals.waypoints[index + 1];
		forge_ai_globals.waypoint_surfaces[index] = forge_ai_globals.waypoint_surfaces[index + 1];
	}
	forge_ai_globals.waypoint_count--;
	forge_ai_update_command_list();
	forge_ai_send_patrols();
	terminal_printf(global_real_argb_green, "forge_ai: waypoint %d removed, %d left", waypoint_index + 1,
		(int)forge_ai_globals.waypoint_count);

	return;
}

static void forge_ai_hold_describe(
	char *line,
	unsigned long size)
{
	_snprintf(line, size, "waypoint %d of %d", forge_ai_hold_globals.waypoint_index + 1,
		(int)forge_ai_globals.waypoint_count);

	return;
}

static int forge_ai_hold_update(
	struct halo_forge_hold_input const *input)
{
	short waypoint_index = forge_ai_hold_globals.waypoint_index;
	float position[3];
	float normal[3];

	/* gone with a new map */
	if (waypoint_index < 0 || waypoint_index >= forge_ai_globals.waypoint_count)
		return TRUE;

	if (!input || input->cancel)
	{
		forge_ai_globals.waypoints[waypoint_index] = forge_ai_hold_globals.original;
		forge_ai_globals.waypoint_surfaces[waypoint_index] = forge_ai_hold_globals.original_surface;
		forge_ai_update_command_list();
		return TRUE;
	}
	if (input->delete_pressed)
	{
		forge_ai_remove_waypoint(waypoint_index);
		return TRUE;
	}

	if (forge_point_at_crosshair(position, normal))
	{
		forge_ai_globals.waypoints[waypoint_index].x = position[0];
		forge_ai_globals.waypoints[waypoint_index].y = position[1];
		forge_ai_globals.waypoints[waypoint_index].z = position[2];
	}

	if (input->place)
	{
		real_point3d point = forge_ai_globals.waypoints[waypoint_index];
		long surface_index = forge_ai_ground_surface(&point);

		if (surface_index == NONE)
		{
			terminal_printf(global_real_argb_orange, "forge_ai: no floor there for a waypoint");
			return FALSE;
		}
		forge_ai_globals.waypoints[waypoint_index] = point;
		forge_ai_globals.waypoint_surfaces[waypoint_index] = surface_index;
		forge_ai_update_command_list();
		forge_ai_send_patrols();
		terminal_printf(global_real_argb_green, "forge_ai: waypoint %d moved", waypoint_index + 1);
		return TRUE;
	}

	return FALSE;
}

static struct halo_forge_hold const forge_ai_hold =
{
	"a waypoint",
	forge_ai_hold_describe,
	forge_ai_hold_update
};

static boolean forge_ai_pick_up_waypoint(
	short waypoint_index)
{
	if (waypoint_index == NONE || !forge_ai_local_game())
		return FALSE;
	forge_ai_hold_globals.waypoint_index = waypoint_index;
	forge_ai_hold_globals.original = forge_ai_globals.waypoints[waypoint_index];
	forge_ai_hold_globals.original_surface = forge_ai_globals.waypoint_surfaces[waypoint_index];

	return forge_mod_hold_begin(&forge_ai_hold);
}

/* the tools' pick up: a shown waypoint under the crosshair, unless an
object is nearer */
static int forge_ai_grab(
	void)
{
	short waypoint_index;
	long object_index;

	if (!forge_ai_globals.show_waypoints || !forge_ai_local_game())
		return FALSE;
	waypoint_index = forge_ai_aimed_waypoint();
	if (waypoint_index == NONE)
		return FALSE;

	object_index = forge_object_at_crosshair();
	if (object_index != NONE && object_try_and_get(object_index))
	{
		struct observer_result const *camera = observer_get_camera(FORGE_AI_LOCAL_PLAYER_INDEX);
		real_point3d middle = forge_ai_globals.waypoints[waypoint_index];

		middle.z += FORGE_AI_WAYPOINT_PICK_HEIGHT;
		if (distance3d(&camera->position, &object_get(object_index)->object.bounding_sphere_center) <
			distance3d(&camera->position, &middle))
		{
			return FALSE;
		}
	}

	return forge_ai_pick_up_waypoint(waypoint_index);
}

/* ---------- the tab */

static short forge_ai_menu_row_count(
	void)
{
	return NUMBER_OF_FORGE_AI_ROWS;
}

static void forge_ai_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	switch (row)
	{
	case _forge_ai_row_character:
		_snprintf(label, label_size, "Character");
		if (forge_ai_globals.character_count > 0)
			_snprintf(value, value_size, "%s", forge_ai_short_name(forge_ai_globals.characters[forge_ai_globals.character_index]));
		else
			_snprintf(value, value_size, "none: import one");
		break;
	case _forge_ai_row_faction:
		_snprintf(label, label_size, "Faction");
		_snprintf(value, value_size, "%s", forge_ai_faction_names[forge_ai_globals.faction]);
		break;
	case _forge_ai_row_aggression:
		_snprintf(label, label_size, "Aggression");
		_snprintf(value, value_size, "%s", forge_ai_aggression_names[forge_ai_globals.aggression]);
		break;
	case _forge_ai_row_squad_size:
		_snprintf(label, label_size, "Squad (added at once)");
		if (forge_ai_globals.squad_size > 1)
			_snprintf(value, value_size, "%d, following the first", (int)forge_ai_globals.squad_size);
		else
			_snprintf(value, value_size, "1, alone");
		break;
	case _forge_ai_row_attack:
		_snprintf(label, label_size, "Attack on sight");
		_snprintf(value, value_size, "%s", forge_ai_globals.attack_on_sight ? "on" : "off (just patrol)");
		break;
	case _forge_ai_row_spawn:
		_snprintf(label, label_size, "Add a character (crosshair)");
		_snprintf(value, value_size, "%d there", (int)forge_ai_living_units());
		break;
	case _forge_ai_row_add_waypoint:
		_snprintf(label, label_size, "Add a waypoint (crosshair)");
		_snprintf(value, value_size, "%d set", (int)forge_ai_globals.waypoint_count);
		break;
	case _forge_ai_row_move_waypoint:
		_snprintf(label, label_size, "Pick up a waypoint (crosshair, or A on it)");
		break;
	case _forge_ai_row_show_waypoints:
		_snprintf(label, label_size, "Show the waypoints");
		_snprintf(value, value_size, "%s", forge_ai_globals.show_waypoints ? "on" : "off");
		break;
	case _forge_ai_row_clear_waypoints:
		_snprintf(label, label_size, "Clear the waypoints");
		break;
	case _forge_ai_row_patrol:
		_snprintf(label, label_size, "New ones patrol the waypoints");
		_snprintf(value, value_size, "%s", forge_ai_globals.patrol_new ? "on" : "off");
		break;
	case _forge_ai_row_send:
		_snprintf(label, label_size, "Send all along the waypoints");
		break;
	case _forge_ai_row_remove:
		_snprintf(label, label_size, "Remove all");
		break;
	}

	return;
}

/* direction -1 or +1 changes a value; 0 acts. TRUE closes the menu. */
static int forge_ai_menu_row_change(
	short row,
	int direction)
{
	boolean close_menu = FALSE;

	switch (row)
	{
	case _forge_ai_row_character:
		if (forge_ai_globals.character_count > 0 && direction != 0)
		{
			forge_ai_globals.character_index = (short)((forge_ai_globals.character_index +
				forge_ai_globals.character_count + direction) % forge_ai_globals.character_count);
		}
		break;
	case _forge_ai_row_faction:
		forge_ai_globals.faction = (short)((forge_ai_globals.faction + NUMBER_OF_FORGE_AI_FACTIONS +
			(direction < 0 ? -1 : 1)) % NUMBER_OF_FORGE_AI_FACTIONS);
		break;
	case _forge_ai_row_aggression:
		forge_ai_globals.aggression = (short)((forge_ai_globals.aggression + NUMBER_OF_FORGE_AI_AGGRESSIONS +
			(direction < 0 ? -1 : 1)) % NUMBER_OF_FORGE_AI_AGGRESSIONS);
		break;
	case _forge_ai_row_squad_size:
		forge_ai_globals.squad_size = (short)((forge_ai_globals.squad_size - 1 + FORGE_AI_MAXIMUM_SQUAD_SIZE +
			(direction < 0 ? -1 : 1)) % FORGE_AI_MAXIMUM_SQUAD_SIZE + 1);
		break;
	case _forge_ai_row_attack:
		forge_ai_globals.attack_on_sight = (boolean)!forge_ai_globals.attack_on_sight;
		forge_ai_update_command_list();
		/* the flags are read when a list starts */
		forge_ai_send_patrols();
		break;
	case _forge_ai_row_move_waypoint:
		if (direction == 0)
		{
			forge_ai_globals.show_waypoints = TRUE;
			if (!forge_ai_pick_up_waypoint(forge_ai_aimed_waypoint()))
				terminal_printf(global_real_argb_orange, "forge_ai: aim at a waypoint first");
			close_menu = TRUE;
		}
		break;
	case _forge_ai_row_spawn:
		if (direction == 0)
		{
			forge_ai_spawn();
			close_menu = TRUE;
		}
		break;
	case _forge_ai_row_add_waypoint:
		if (direction == 0)
		{
			forge_ai_add_waypoint();
			close_menu = TRUE;
		}
		break;
	case _forge_ai_row_show_waypoints:
		forge_ai_globals.show_waypoints = (boolean)!forge_ai_globals.show_waypoints;
		break;
	case _forge_ai_row_clear_waypoints:
		if (direction == 0)
		{
			forge_ai_globals.waypoint_count = 0;
			forge_ai_update_command_list();
		}
		break;
	case _forge_ai_row_patrol:
		forge_ai_globals.patrol_new = (boolean)!forge_ai_globals.patrol_new;
		break;
	case _forge_ai_row_send:
		if (direction == 0)
		{
			short sent = forge_ai_send_patrols();

			if (forge_ai_globals.waypoint_count < 2)
				terminal_printf(global_real_argb_orange, "forge_ai: set two or more waypoints first");
			else
				terminal_printf(global_real_argb_green, "forge_ai: %d sent along %d waypoints", (int)sent,
					(int)forge_ai_globals.waypoint_count);
		}
		break;
	case _forge_ai_row_remove:
		if (direction == 0)
		{
			short index;
			/* (counted once: counting packs the list) */
			short unit_count;

			for (index = 0, unit_count = forge_ai_living_units(); index < unit_count; index++)
				object_delete(forge_ai_globals.units[index]);
			forge_ai_globals.unit_count = 0;
		}
		break;
	}

	return close_menu;
}

static void forge_ai_menu_opened(
	void)
{
	forge_ai_find_characters();

	return;
}

static struct halo_mod_menu const forge_ai_menu =
{
	"AI",
	forge_ai_menu_row_count,
	forge_ai_menu_row_text,
	forge_ai_menu_row_change,
	forge_ai_menu_opened
};

/* ---------- layouts (port/linux/game/forge_layout.c) */

static void forge_ai_layout_save(
	struct halo_layout_writer *writer)
{
	short index;
	/* (counted once: counting packs the list) */
	short unit_count;

	/* (the first is whether new characters are the player's, as before the
	factions; the last three are read only by builds that have them) */
	halo_layout_printf(writer, "settings %d %d %d %d %d %d %d",
		(int)(forge_ai_globals.faction == _forge_ai_faction_player), (int)forge_ai_globals.attack_on_sight,
		(int)forge_ai_globals.patrol_new, (int)forge_ai_globals.show_waypoints, (int)forge_ai_globals.faction,
		(int)forge_ai_globals.aggression, (int)forge_ai_globals.squad_size);
	for (index = 0; index < forge_ai_globals.waypoint_count; index++)
	{
		real_point3d const *point = &forge_ai_globals.waypoints[index];

		halo_layout_printf(writer, "waypoint %.4f %.4f %.4f", point->x, point->y, point->z);
	}
	for (index = 0, unit_count = forge_ai_living_units(); index < unit_count; index++)
	{
		struct object_datum *object = object_get(forge_ai_globals.units[index]);

		/* the dead lie where they fell: only the living are kept; and none
		for the clients of a system link game, who have the host's */
		if (object->object.body_vitality <= 0.f || forge_layout_saving_for_client())
			continue;
		halo_layout_printf(writer, "member %d %d %d %.4f %.4f %.4f %.5f %.5f %s",
			(int)forge_ai_globals.unit_factions[index], (int)forge_ai_globals.unit_aggressions[index],
			(int)forge_ai_globals.unit_squads[index],
			object->object.position.x, object->object.position.y, object->object.position.z,
			object->object.forward.i, object->object.forward.j,
			tag_get_name(forge_ai_globals.unit_variants[index]));
	}

	return;
}

static void forge_ai_layout_clear(
	void)
{
	short index;
	/* (counted once: counting packs the list) */
	short unit_count;

	for (index = 0, unit_count = forge_ai_living_units(); index < unit_count; index++)
		object_delete(forge_ai_globals.units[index]);
	forge_ai_globals.unit_count = 0;
	forge_ai_globals.waypoint_count = 0;
	forge_ai_hold_globals.waypoint_index = NONE;
	forge_ai_update_command_list();

	return;
}

/* whether the numbers of a layout's line can be a place (a number that is
none fails every comparison) */
static boolean forge_ai_layout_numbers_valid(
	float const *numbers,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		if (!(numbers[index] > -FORGE_AI_WORLD_BOUND && numbers[index] < FORGE_AI_WORLD_BOUND))
			return FALSE;
	}

	return TRUE;
}

/* a character of a layout's line: its faction, aggression and squad (from
the line, checked), at the place; a squad's id is kept, and the next new
squad's is after it */
static void forge_ai_layout_character(
	char const *name,
	float const *numbers,
	int faction,
	int aggression,
	int squad)
{
	long variant_index = tag_loaded(ACTOR_VARIANT_DEFINITION_TAG, name);
	real_point3d point;
	real_vector3d forward;

	if (variant_index == NONE)
	{
		terminal_printf(global_real_argb_orange, "forge_ai: %s is not in this map, left out", name);
		return;
	}
	if (faction < 0 || faction >= NUMBER_OF_FORGE_AI_FACTIONS)
		faction = _forge_ai_faction_own;
	if (aggression < 0 || aggression >= NUMBER_OF_FORGE_AI_AGGRESSIONS)
		aggression = _forge_ai_aggression_normal;
	if (squad < 0 || squad >= FORGE_AI_MAXIMUM_SQUAD_ID)
		squad = NONE;
	else if (squad >= forge_ai_globals.next_squad)
		forge_ai_globals.next_squad = (short)((squad + 1) % FORGE_AI_MAXIMUM_SQUAD_ID);
	set_real_point3d(&point, numbers[0], numbers[1], numbers[2]);
	set_real_vector3d(&forward, numbers[3], numbers[4], 0.f);
	if (forge_ai_create(variant_index, &point, &forward, (short)faction, (short)aggression, (short)squad) != NONE)
		ai_update_team_status();

	return;
}

static void forge_ai_layout_load(
	char const *line)
{
	int values[7];
	float numbers[5];
	int name_offset = 0;
	int count;

	if ((count = sscanf(line, "settings %d %d %d %d %d %d %d", &values[0], &values[1], &values[2], &values[3],
		&values[4], &values[5], &values[6])) >= 4)
	{
		forge_ai_globals.faction = values[0] != 0 ? _forge_ai_faction_player : _forge_ai_faction_own;
		forge_ai_globals.attack_on_sight = values[1] != 0;
		forge_ai_globals.patrol_new = values[2] != 0;
		forge_ai_globals.show_waypoints = values[3] != 0;
		if (count == 7)
		{
			if (values[4] >= 0 && values[4] < NUMBER_OF_FORGE_AI_FACTIONS)
				forge_ai_globals.faction = (short)values[4];
			if (values[5] >= 0 && values[5] < NUMBER_OF_FORGE_AI_AGGRESSIONS)
				forge_ai_globals.aggression = (short)values[5];
			if (values[6] >= 1 && values[6] <= FORGE_AI_MAXIMUM_SQUAD_SIZE)
				forge_ai_globals.squad_size = (short)values[6];
		}
		forge_ai_update_command_list();
	}
	else if (sscanf(line, "waypoint %f %f %f", &numbers[0], &numbers[1], &numbers[2]) == 3 &&
		forge_ai_layout_numbers_valid(numbers, 3) &&
		forge_ai_globals.waypoint_count < FORGE_AI_MAXIMUM_WAYPOINTS)
	{
		real_point3d point;
		long surface_index;

		set_real_point3d(&point, numbers[0], numbers[1], numbers[2]);
		if ((surface_index = forge_ai_ground_surface(&point)) != NONE)
		{
			forge_ai_globals.waypoints[forge_ai_globals.waypoint_count] = point;
			forge_ai_globals.waypoint_surfaces[forge_ai_globals.waypoint_count] = surface_index;
			forge_ai_globals.waypoint_count++;
			forge_ai_update_command_list();
		}
	}
	else if (sscanf(line, "member %d %d %d %f %f %f %f %f %n", &values[0], &values[1], &values[2], &numbers[0],
		&numbers[1], &numbers[2], &numbers[3], &numbers[4], &name_offset) >= 8 && name_offset > 0 &&
		forge_ai_layout_numbers_valid(numbers, 5) &&
		/* a system link client has the host's characters */
		!forge_layout_loading_for_client())
	{
		forge_ai_layout_character(line + name_offset, numbers, values[0], values[1], values[2]);
	}
	/* (before the factions: whether it was the player's friend) */
	else if (sscanf(line, "character %d %f %f %f %f %f %n", &values[0], &numbers[0], &numbers[1], &numbers[2],
		&numbers[3], &numbers[4], &name_offset) >= 6 && name_offset > 0 &&
		forge_ai_layout_numbers_valid(numbers, 5) &&
		!forge_layout_loading_for_client())
	{
		forge_ai_layout_character(line + name_offset, numbers,
			values[0] != 0 ? _forge_ai_faction_player : _forge_ai_faction_own, _forge_ai_aggression_normal, NONE);
	}

	return;
}

/* ---------- the hooks */

/* port: SaftiGlas's test hook, kept (forge-dev-tools), off unless set.
debug.forge_test_ai (port_config.c): a few seconds into a local game,
once, two squads of that many characters stand ten units in front of the
player, five to each side, facing each other: the first two characters of
the map (by name) whose own teams differ, each on its own team; then every
two seconds for thirty, how many of each live and fight is logged, for
automated tests of factions and squads. With characters of one team only,
one squad of the first, on the player's side, patrols two waypoints twelve
units apart, and how far its followers are from the leader is logged. 0
never. */
long config_integer(char const *name);
void platform_log(char const *format, ...);

static void forge_ai_test(
	void)
{
	static long size = -1;
	static long started = NONE;
	static long logged = NONE;
	static long variants[2] = { NONE, NONE };
	static short teams[2];
	long time = game_time_get();
	short side;

	if (size < 0)
		size = PIN(config_integer("debug.forge_test_ai"), 0, FORGE_AI_MAXIMUM_SQUAD_SIZE);
	/* port: (in a Forge game, its player spawned: not the main menu's) */
	if (size <= 0 || time < 150 || !forge_ai_local_game() || !forge_mode_on())
		return;
	if (started == NONE)
	{
		long player_index = local_player_get_player_index(FORGE_AI_LOCAL_PLAYER_INDEX);
		struct unit_datum *player_unit = unit_try_and_get(player_get(player_index)->unit_index);
		short index;

		if (!player_unit)
			return;
		started = time;
		forge_ai_find_characters();
		for (index = 0; index < forge_ai_globals.character_count && variants[1] == NONE; index++)
		{
			long variant_index = forge_ai_globals.characters[index];
			struct unit_definition *unit_definition =
				unit_definition_get(actor_variant_definition_get(variant_index)->unit_reference.index);
			short team = unit_definition->unit.default_team;

			if (variants[0] == NONE)
			{
				variants[0] = variant_index;
				teams[0] = team;
			}
			else if (team != teams[0])
			{
				variants[1] = variant_index;
				teams[1] = team;
			}
		}
		if (variants[1] == NONE && variants[0] != NONE)
		{
			real_vector3d forward = player_unit->object.forward;
			short made;

			forward.k = 0.f;
			normalize3d(&forward);
			for (index = 0; index < 2; index++)
			{
				real_point3d point = player_unit->object.position;

				point.x += forward.i * (6.f + 12.f * index);
				point.y += forward.j * (6.f + 12.f * index);
				point.z += 1.f;
				if ((forge_ai_globals.waypoint_surfaces[index] = forge_ai_ground_surface(&point)) == NONE)
					break;
				forge_ai_globals.waypoints[index] = point;
			}
			forge_ai_globals.waypoint_count = index;
			forge_ai_update_command_list();
			made = forge_ai_create_squad(variants[0], &forge_ai_globals.waypoints[0], &forward,
				_forge_ai_faction_player, _forge_ai_aggression_normal, (short)size);
			platform_log("forge ai test: one squad of %d %s on the player's side, %d waypoints", made,
				tag_get_name(variants[0]), forge_ai_globals.waypoint_count);
			return;
		}
		if (variants[1] == NONE)
		{
			platform_log("forge ai test: the map has no characters");
			return;
		}
		for (side = 0; side < 2; side++)
		{
			real_vector3d forward = player_unit->object.forward;
			real_point3d point = player_unit->object.position;
			real_vector3d facing;
			real_point3d floor;
			short made;

			forward.k = 0.f;
			normalize3d(&forward);
			/* ten in front, five to the left and the right, facing the other */
			point.x += forward.i * 10.f + (side ? 1.f : -1.f) * -forward.j * 5.f;
			point.y += forward.j * 10.f + (side ? 1.f : -1.f) * forward.i * 5.f;
			point.z += 1.f;
			floor = point;
			if (forge_ai_ground_surface(&floor) != NONE)
				point.z = floor.z + 0.1f;
			set_real_vector3d(&facing, (side ? 1.f : -1.f) * forward.j, (side ? -1.f : 1.f) * forward.i, 0.f);
			made = forge_ai_create_squad(variants[side], &point, &facing, _forge_ai_faction_own,
				_forge_ai_aggression_normal, (short)size);
			platform_log("forge ai test: squad %d: %d %s (team %d) at %.2f %.2f %.2f", side, made,
				tag_get_name(variants[side]), teams[side], point.x, point.y, point.z);
		}
	}
	if (variants[1] == NONE && variants[0] != NONE && time - started <= 30 * 30 &&
		(logged == NONE || time - logged >= 60))
	{
		short leader_slot = forge_ai_living_units() > 0 ? forge_ai_squad_leader(forge_ai_globals.unit_squads[0]) : NONE;
		real farthest = 0.f;
		real total = 0.f;
		short followers = 0;
		short slot;

		logged = time;
		if (leader_slot == NONE)
			return;
		for (slot = 0; slot < forge_ai_globals.unit_count; slot++)
		{
			if (slot != leader_slot && forge_ai_unit_actor(slot))
			{
				real distance = distance3d(&object_get(forge_ai_globals.units[slot])->object.position,
					&object_get(forge_ai_globals.units[leader_slot])->object.position);

				farthest = MAX(farthest, distance);
				total += distance;
				followers++;
			}
		}
		platform_log("forge ai test: %.0f s: leader at %.1f %.1f (%s), %d followers, %.1f apart on average, "
			"%.1f at most", (time - started) / 30.f,
			object_get(forge_ai_globals.units[leader_slot])->object.position.x,
			object_get(forge_ai_globals.units[leader_slot])->object.position.y,
			forge_ai_unit_actor(leader_slot)->state.action == _actor_action_obey ? "walking" : "not walking",
			followers, followers ? total / followers : 0.f, farthest);
	}
	if (variants[1] != NONE && time - started <= 30 * 30 && (logged == NONE || time - logged >= 60))
	{
		short slot;
		short living[2] = { 0, 0 };
		short fighting[2] = { 0, 0 };
		short following[2] = { 0, 0 };
		short unit_count = forge_ai_living_units();

		logged = time;
		for (slot = 0; slot < unit_count; slot++)
		{
			struct actor_datum *actor = forge_ai_unit_actor(slot);

			side = forge_ai_globals.unit_variants[slot] == variants[1];
			if (!actor)
				continue;
			living[side]++;
			if (actor->state.combat_status >= FORGE_AI_COMBAT_STATUS_DEFINITE)
				fighting[side]++;
			if (forge_ai_is_follower(slot) && actor->state.action == _actor_action_obey)
				following[side]++;
		}
		platform_log("forge ai test: %.0f s: %s %d alive, %d fighting, %d following; %s %d alive, %d fighting, "
			"%d following", (time - started) / 30.f, forge_ai_short_name(variants[0]), living[0], fighting[0],
			following[0], forge_ai_short_name(variants[1]), living[1], fighting[1], following[1]);
	}

	return;
}

/* once a frame: each second the squads keep together and fight together,
and now and then idle characters go back to their patrol */
static void forge_ai_update(
	void)
{
	unsigned long milliseconds = system_milliseconds();

	/* where the AI runs: a local game, the host of a system link game */
	if (!halo_mods_authoritative())
		return;
	forge_ai_test();
	if ((long)(milliseconds - forge_ai_globals.squad_milliseconds) >= FORGE_AI_SQUAD_MILLISECONDS)
	{
		forge_ai_globals.squad_milliseconds = milliseconds;
		forge_ai_update_squads();
	}
	if (!forge_ai_globals.attack_on_sight ||
		(long)(milliseconds - forge_ai_globals.repatrol_milliseconds) < FORGE_AI_REPATROL_MILLISECONDS)
	{
		return;
	}
	forge_ai_globals.repatrol_milliseconds = milliseconds;
	forge_ai_repatrol();

	return;
}

/* a new map: nothing of the last is left, and this one gets its command list */
static void forge_ai_new_map(
	void)
{
	forge_ai_globals.waypoint_count = 0;
	forge_ai_globals.unit_count = 0;
	forge_ai_hold_globals.waypoint_index = NONE;
	forge_ai_find_characters();
	if (!halo_mods_authoritative())
	{
		/* (a system link client's AI is the host's: no list of ours) */
		forge_ai_retire_command_lists();
	}
	else
	{
		forge_ai_install_command_list();
		forge_ai_update_command_list();
	}

	return;
}

/* the waypoints, in each player's view */
static void forge_ai_render_world(
	void)
{
	short index;
	real_argb_color color;

	if (!forge_ai_globals.show_waypoints || forge_ai_globals.waypoint_count == 0 ||
		game_connection() == _game_connection_film_playback)
		return;

	/* (opaque: the game draws opaque debug lines together, and each
	see-through one by itself) */
	color.alpha = 1.f;
	color.red = FORGE_AI_WAYPOINT_COLOR_R;
	color.green = FORGE_AI_WAYPOINT_COLOR_G;
	color.blue = FORGE_AI_WAYPOINT_COLOR_B;
	for (index = 0; index < forge_ai_globals.waypoint_count; index++)
	{
		real_point3d const *point = &forge_ai_globals.waypoints[index];
		real_point3d top = *point;
		real_point3d next;
		real_point3d corners[4];
		short corner;

		top.z += 1.2f;
		rasterizer_debug_line_shaded(point, &top, &color, &color);
		/* a small diamond on the point, so it is easy to find */
		for (corner = 0; corner < 4; corner++)
		{
			corners[corner] = *point;
			corners[corner].z += 0.2f;
			corners[corner].x += (corner == 0 ? 0.25f : corner == 2 ? -0.25f : 0.f);
			corners[corner].y += (corner == 1 ? 0.25f : corner == 3 ? -0.25f : 0.f);
		}
		for (corner = 0; corner < 4; corner++)
		{
			rasterizer_debug_line_shaded(&corners[corner], &corners[(corner + 1) % 4], &color, &color);
			rasterizer_debug_line_shaded(&corners[corner], &top, &color, &color);
		}
		/* the way round */
		next = forge_ai_globals.waypoints[(index + 1) % forge_ai_globals.waypoint_count];
		if (forge_ai_globals.waypoint_count > 1 && (index + 1 < forge_ai_globals.waypoint_count || forge_ai_globals.waypoint_count > 2))
		{
			real_point3d from = *point;
			real_point3d to = next;

			from.z += 0.3f;
			to.z += 0.3f;
			rasterizer_debug_line_shaded(&from, &to, &color, &color);
		}
	}

	return;
}

struct halo_mod const forge_ai_mod =
{
	"forge_ai",
	forge_ai_update,
	NULL,
	NULL,
	forge_ai_new_map,
	forge_ai_render_world,
	&forge_ai_menu,
	forge_ai_grab,
	NULL,
	forge_ai_layout_save,
	forge_ai_layout_clear,
	forge_ai_layout_load
};
