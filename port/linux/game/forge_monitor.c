/*
FORGE_MONITOR.C

Forge's builder as the monitor (343 Guilty Spark): Home / D-pad up makes the
player's unit a monitor biped, which flies where the player looks, and again
gives the player a body of the game's back where the monitor is
(port/linux/game/forge.c). A player flies in Forge while their unit is the
monitor: that is the game's state, the host's in a system link game, so
every machine sees every builder's monitor as it sees any unit.

The monitor is no tag of a multiplayer map: it comes from The Maw (c40.map)
as a Forge game's map loads, from Forge's tag pack (port: Watanoge's tag
importer, port/linux/game/tag_import.c, called by forge_tag_import.c), on
every machine alike, so its tag has the same index on all of them. With
game.forge_monitor off (this machine's builder), or without the pack, there
is no monitor and Forge flies its camera free of the player, as it did
(camera/director.c).

The monitor moves by the unit's throttle, as Forge's free camera flew
(units/bipeds.c, a player's flying biped): forward and sideways level, the
way the player faces whatever the pitch of their view, and straight up and
down with the tools' rise and sink keys (R and F, RB and LB), faster with
their fast key (shift, LT). The keys reach the game as unit control flags
(forge_monitor_control_flags: jump, crouch and FORGE_MONITOR_FAST_BIT;
game/players.c makes the first two the throttle's third part), so a client's
host flies its monitor as it does.

In first person the monitor's own lens flares (its eye's) are not drawn:
they would fill the view (objects/object_lights.c,
forge_monitor_hides_lens_flares). While it turns what it holds (the right
trigger, or the right mouse button, forge.c), the look and the throttle are
the held object's, as the free camera's are (forge_monitor_control_input).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "camera/director.h"
#include "camera/director_forge.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "objects/objects.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "units/biped_definitions.h"
#include "units/units.h"

#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define FORGE_MONITOR_TAG_NAME "characters\\monitor\\monitor"
/* the unit control flag that is the tools' fast key: a flying biped has no
other use of it but how exactly it faces at rest (units/bipeds.c) */
#define FORGE_MONITOR_FAST_BIT _unit_control_exact_facing_bit
/* world units a second a held object comes nearer or goes further while it
is turned, and how much faster with the fast key: the free camera's
(FORGE_ZOOM_SPEED by FORGE_SPEED_SCALE, FORGE_FAST_SCALE, camera/director_forge.c) */
#define FORGE_MONITOR_ZOOM_SPEED 4.f
#define FORGE_MONITOR_FAST_SCALE 4.f

/* ---------- prototypes */

/* cache/cache_files.c's: the map's tags */
extern struct cache_file_tag_instance *global_tag_instances;
/* the platform layer's (port/linux/src/port_config.c) */
int config_boolean(const char *name);

/* ---------- globals */

static struct
{
	/* the monitor's tag in the map whose tags these are, by their address and
	the scenario's name (every map's tags are at the same address) */
	void *tag_instances;
	char scenario_name[256];
	long definition_index;
	/* local player 0's rise (1) and sink (-1) keys, and their fast key */
	int rise;
	int fast;
} forge_monitor_globals = { NULL, "", NONE, 0, 0 };

/* ---------- public code */

/* whether the map that is loading gets Forge's tags (forge_tag_import.c):
a Forge game's, on every machine of it alike (port: not a setting of this
machine's, which the tags' indices must not depend on) */
int forge_monitor_import_wanted(
	void)
{
	return game_variant_set_is_forge();
}

/* the monitor's biped in this map, or NONE */
long forge_monitor_definition(
	void)
{
	char const *scenario_name = global_tag_instances && global_scenario_index != NONE
		? tag_get_name(global_scenario_index)
		: "";

	if (forge_monitor_globals.tag_instances != (void *)global_tag_instances ||
		strcmp(forge_monitor_globals.scenario_name, scenario_name) != 0)
	{
		forge_monitor_globals.tag_instances = (void *)global_tag_instances;
		_snprintf(forge_monitor_globals.scenario_name, sizeof(forge_monitor_globals.scenario_name), "%s",
			scenario_name);
		forge_monitor_globals.scenario_name[sizeof(forge_monitor_globals.scenario_name) - 1] = 0;
		forge_monitor_globals.definition_index = global_tag_instances
			? tag_loaded(BIPED_DEFINITION_TAG, FORGE_MONITOR_TAG_NAME)
			: NONE;
	}

	return forge_monitor_globals.definition_index;
}

/* a new map (game/halo_mods.c): its tags are looked in again, even where they
are at the same address under the same scenario name */
void forge_monitor_new_map(
	void)
{
	forge_monitor_globals.tag_instances = NULL;
	forge_monitor_globals.scenario_name[0] = 0;
	forge_monitor_globals.definition_index = NONE;

	return;
}

int forge_unit_is_monitor(
	long unit_index)
{
	struct object_datum *object = unit_index != NONE ? object_try_and_get(unit_index) : NULL;

	return object && forge_monitor_definition() != NONE && object->definition_index == forge_monitor_definition();
}

int forge_player_is_monitor(
	long player_index)
{
	return player_index != NONE && forge_unit_is_monitor(player_get(player_index)->unit_index);
}

int forge_flying(
	short local_player_index)
{
	return director_forge_flying(local_player_index) ||
		forge_player_is_monitor(local_player_get_player_index(local_player_index));
}

/* the game's machine only: makes the player the monitor, where their head
is, or gives them the game's body back, where the monitor is; FALSE when
that cannot be now (dead, riding a vehicle, no monitor in this map) */
int forge_monitor_set(
	long player_index,
	int monitor)
{
	struct player_datum *player = player_get(player_index);
	long unit_index = NONE;
	real_point3d position;

	if (player->unit_index == NONE || !unit_try_and_get(player->unit_index) ||
		forge_monitor_definition() == NONE || !forge_player_is_monitor(player_index) == !monitor)
	{
		return FALSE;
	}
	if (monitor)
	{
		unit_get_head_position(player->unit_index, &position);
		unit_index = player_replace_unit(player_index, forge_monitor_definition(), &position);
		/* (a builder is no target) */
		if (unit_index != NONE)
			SET_FLAG(object_get(unit_index)->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
	}
	else
	{
		struct game_globals_multiplayer_information *multiplayer_information = TAG_BLOCK_GET_ELEMENT(
			&scenario_get_game_globals()->multiplayer_information, 0, struct game_globals_multiplayer_information);
		struct game_globals_player_information *player_information = TAG_BLOCK_GET_ELEMENT(
			&scenario_get_game_globals()->player_information, 0, struct game_globals_player_information);
		long definition_index = game_engine_running()
			? multiplayer_information->unit.index
			: player_information->player_unit.index;

		position = object_get(player->unit_index)->object.position;
		if (definition_index != NONE)
			unit_index = player_replace_unit(player_index, definition_index, &position);
		/* the weapons a player of the game starts with, as when they spawn */
		if (unit_index != NONE && game_engine_running())
			game_engine_postspawn_player_update(player_index);
	}

	return unit_index != NONE;
}

/* local player 0's rise and sink keys and fast key, as held now (forge.c) */
void forge_monitor_set_keys(
	int rise,
	int fast)
{
	forge_monitor_globals.rise = rise;
	forge_monitor_globals.fast = fast;
}

/* a local player's unit control flags (game/player_control.c): a monitor's
jump and crouch are the tools' rise and sink keys, not the game's (A, which
also takes what the crosshair points at), and FORGE_MONITOR_FAST_BIT their
fast key */
unsigned long forge_monitor_control_flags(
	short local_player_index,
	unsigned long flags)
{
	if (forge_player_is_monitor(local_player_get_player_index(local_player_index)))
	{
		int rise = local_player_index == 0 ? forge_monitor_globals.rise : 0;

		SET_FLAG(flags, _unit_control_jump_bit, rise > 0);
		SET_FLAG(flags, _unit_control_crouch_modifier_bit, rise < 0);
		SET_FLAG(flags, FORGE_MONITOR_FAST_BIT, local_player_index == 0 && forge_monitor_globals.fast);
	}

	return flags;
}

/* a local player's look and throttle of this frame (game/player_control.c):
while the monitor turns what it holds (the right trigger, or the right mouse
button, forge.c) the view stays and the monitor with it, as the free camera
does (camera/director_forge.c): the mouse turns the object (the right stick
does in forge.c), and forward and back bring it nearer and take it further */
void forge_monitor_control_input(
	short local_player_index,
	float facing_delta[2],
	float throttle[2],
	float seconds)
{
	if (local_player_index == 0 && forge_camera_turning() &&
		forge_player_is_monitor(local_player_get_player_index(local_player_index)))
	{
		forge_camera_turn_held(facing_delta[0], facing_delta[1]);
		forge_camera_zoom(throttle[0] * FORGE_MONITOR_ZOOM_SPEED *
			(forge_monitor_globals.fast ? FORGE_MONITOR_FAST_SCALE : 1.f) * seconds);
		facing_delta[0] = facing_delta[1] = 0.f;
		throttle[0] = throttle[1] = 0.f;
	}

	return;
}

/* whether a unit's lens flares are left out of the view being drawn: the
monitor's own, of the player who looks out of it (objects/object_lights.c) */
int forge_monitor_hides_lens_flares(
	long unit_index)
{
	long player_index = local_player_get_player_index(render.local_player_index);

	return player_index != NONE && player_get(player_index)->unit_index == unit_index &&
		director_get_perspective(render.local_player_index) == _director_perspective_first_person &&
		forge_unit_is_monitor(unit_index);
}
