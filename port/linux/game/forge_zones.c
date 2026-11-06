/*
FORGE_ZONES.C

One of Forge's parts (halo_mod.h, halo_mods.c): kill, gravity and teleport zones,
boxes placed with the dev tools' crosshair (port/linux/game/forge.c).

Everything is in the dev tools' menu (1, D-pad right or, flying, X), on the
"Zones" tab: up and down choose a row, left and right change its value,
enter or A act, LB / RB (Page Up / Page Down, T / V) change tab. So it works on a controller as it does on a
keyboard, and needs no other key. The rows:

	Show zones      draw the zones in the world (placing one shows them);
	                they are drawn only in forge mode (flying), or while
	                one is held: never on foot
	New zone        left/right: the kind of the next one; enter/A: close the
	                menu and hold a new one at the crosshair, on the surface
	                it points at, turned the way the camera looks
	Zone            which zone the rows below are about (with the zones
	                shown, aiming at one with the menu closed chooses it,
	                and X on a controller opens the menu here on it)
	Kind            kill, gravity or teleport
	Shape           a square (a box) or a circle (a cylinder, its width
	                and length the ellipse's)
	Setting         a kill zone's delay, a gravity zone's gravity (below zero
	                it lifts), a teleport zone's channel (a number)
	Direction       (teleport) two-way, entrance only, or exit only
	Width, Length, Height, Resize step
	Pick up and move
	                hold the zone as the tools hold an object: aim to move
	                it, left/right turn it about the chosen axis (T: yaw,
	                pitch or roll; V / X: free, or in steps of 15, 45, 90 or
	                180 degrees), and flying, as an object, the right stick
	                with RT held and the mouse with its right button held
	                turn its yaw and pitch; up/down raise and lower it, enter or A put
	                it down, escape or B put it back, delete, Back or Y
	                remove it. A new zone is held the same way until it is
	                put down (escape or B: no zone after all).
	                With the zones shown, aiming at one and pressing F4 (or
	                enter / A while flying) picks it up the same way, unless
	                an object is nearer.
	Move here       to the crosshair, turned the way the camera looks
	Remove

A zone is a box, or a cylinder, standing on the surface it was placed on,
turned as an object is (yaw, pitch and roll). It works whether it is shown or not:
- a kill zone kills every living biped (players and AI, also in a vehicle)
  whose middle is in it, at once or after its delay; while local player 0
  counts down, the screen says so
- a gravity zone gives every biped, vehicle and item in it (not what rides
  or is carried) that fraction of the map's gravity: 0 is weightless,
  below 0 it rises. Bipeds standing on the ground are left alone unless it
  lifts, which takes them off the ground first.
- a teleport zone sends every biped and vehicle that walks into it (not
  what rides or is carried) to another teleport zone with the same channel
  number: two zones with a number in common are a pair, as many as share
  it are a ring, each sending to the next. What arrives keeps its place
  in the box, turned by the difference of the two boxes' yaws (its
  speed too, and the player's view), and cannot go back through the zone
  it arrived in before leaving it. An entrance only zone never receives, an
  exit only zone never sends. Shown, paired teleports have the same colour
  and a line between them, and an arrow on the floor of each says which
  way is its forward: what walks into one along its arrow comes out of
  the other along that one's arrow (so a player looks that way).

The zones change the game only in the tick hook (halo_mod.h), so only in
local games and at the game's own rate. They are forgotten when the map
changes; they are not part of checkpoints, so going back to one keeps
them.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "camera/director_forge.h"
#include "camera/observer.h"
#include "cseries/cseries_windows.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/physics.h"
#include "rasterizer/rasterizer.h"
#include "units/bipeds.h"
#include "units/units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	FORGE_ZONES_LOCAL_PLAYER_INDEX = 0,
	FORGE_ZONES_MAXIMUM_ZONES = 64,
	/* the game's object data array */
	FORGE_ZONES_MAXIMUM_OBJECTS = 2048,
	FORGE_ZONES_MAXIMUM_CHANNEL = 16
};

enum forge_zone_kind
{
	_forge_zone_kill = 0,
	_forge_zone_gravity,
	_forge_zone_teleport,
	NUMBER_OF_FORGE_ZONE_KINDS
};

enum forge_zone_direction
{
	_forge_zone_two_way = 0,
	_forge_zone_entrance_only,
	_forge_zone_exit_only,
	NUMBER_OF_FORGE_ZONE_DIRECTIONS
};

enum forge_zone_shape
{
	_forge_zone_square = 0,
	_forge_zone_circle,
	NUMBER_OF_FORGE_ZONE_SHAPES
};

/* the sides a circle zone is drawn with: the game draws a frame's debug
geometry from 8,192 lines and triangles at most (rasterizer_debug.c), which
the zones share with the spawns, the waypoints and the rest */
#define FORGE_ZONES_CIRCLE_SEGMENTS 16

/* world units (a world unit is about 3 metres) */
#define FORGE_ZONES_DEFAULT_LENGTH 3.f
#define FORGE_ZONES_DEFAULT_WIDTH 3.f
#define FORGE_ZONES_DEFAULT_HEIGHT 2.f
#define FORGE_ZONES_MINIMUM_SIZE 0.25f
#define FORGE_ZONES_MAXIMUM_SIZE 400.f
/* how far from the origin a layout's line may put a zone (world units) */
#define FORGE_ZONES_WORLD_BOUND 32768.f
/* how far below the aimed surface a zone's floor goes, so that what
stands there is in it */
#define FORGE_ZONES_FLOOR_DEPTH 0.1f
/* how far the crosshair selects zones, and how far from their edge they are
drawn (the chosen one wherever it is) */
#define FORGE_ZONES_SELECT_RANGE 200.f
#define FORGE_ZONES_DRAW_RANGE 200.f
/* how much of its colour an edge of a zone that is not the chosen one has:
edges are drawn opaque, which the game draws together, all in one go, where
it draws each see-through line by itself */
#define FORGE_ZONES_DIM_EDGE 0.6f
/* the upward speed, world units a tick, a lift gives a biped standing on
the ground, to take it off */
#define FORGE_ZONES_LIFT_TAKEOFF_SPEED 0.05f
/* what a teleported object is kept from the floor and walls of the box it
arrives in */
#define FORGE_ZONES_TELEPORT_MARGIN 0.05f
/* world units a second a held zone rises and sinks, and radians a second it
turns: the tools' own speeds for an object (port/linux/game/forge.c) */
#define FORGE_ZONES_HOLD_RAISE_SPEED 2.f
#define FORGE_ZONES_HOLD_TURN_SPEED _pi

/* 0xAARRGGBB */
#define FORGE_ZONES_WARNING_COLOR 0xffff4040UL

/* ---------- structures */

struct forge_zone_setting
{
	real value;
	char const *name;
};

struct forge_zone
{
	short kind;
	/* a kill or gravity zone's setting */
	short setting_index;
	real_point3d center;
	/* turned as the tools turn an object: about the world's up, then the nose
	up, then about forward */
	real yaw;
	real pitch;
	real roll;
	/* half the length (along forward), width (along left) and height (along
	up) */
	real_vector3d half_size;
	/* a teleport zone: its channel, 1 to FORGE_ZONES_MAXIMUM_CHANNEL, and
	what it does with it (enum forge_zone_direction) */
	short channel;
	short direction;
	/* enum forge_zone_shape: a box, or a cylinder whose ellipse is the box's
	length and width */
	short shape;
};

/* ---------- globals */

/* a kill zone's delay, seconds */
static struct forge_zone_setting const forge_zones_kill_settings[] =
{
	{ 0.f, "at once" },
	{ 1.f, "after 1 s" },
	{ 2.f, "after 2 s" },
	{ 3.f, "after 3 s" },
	{ 5.f, "after 5 s" },
	{ 10.f, "after 10 s" }
};

/* a gravity zone's fraction of the map's gravity */
static struct forge_zone_setting const forge_zones_gravity_settings[] =
{
	{ 0.f, "weightless" },
	{ 0.17f, "moon (17%)" },
	{ 0.25f, "25%" },
	{ 0.5f, "50%" },
	{ 1.5f, "150%" },
	{ 2.f, "200%" },
	{ 3.f, "300%" },
	{ -0.5f, "slow lift" },
	{ -1.f, "lift" },
	{ -3.f, "fast lift" }
};

static char const *const forge_zones_direction_names[NUMBER_OF_FORGE_ZONE_DIRECTIONS] =
{
	"two-way",
	"entrance only",
	"exit only"
};

static char const *const forge_zones_shape_names[NUMBER_OF_FORGE_ZONE_SHAPES] =
{
	"square",
	"circle"
};

/* how much a Width, Length or Height press changes a size */
static real const forge_zones_resize_steps[] = { 0.25f, 1.f, 4.f };
static char const *const forge_zones_resize_step_names[] = { "small (0.25)", "medium (1)", "large (4)" };

static struct
{
	struct forge_zone_setting const *settings;
	short setting_count;
	short default_setting_index;
	char const *name;
	real_argb_color color;
} const forge_zone_kinds[NUMBER_OF_FORGE_ZONE_KINDS] =
{
	{
		forge_zones_kill_settings,
		(short)(sizeof(forge_zones_kill_settings) / sizeof(forge_zones_kill_settings[0])),
		0,
		"kill zone",
		{ { 1.f, 1.f, 0.25f, 0.2f } }
	},
	{
		forge_zones_gravity_settings,
		(short)(sizeof(forge_zones_gravity_settings) / sizeof(forge_zones_gravity_settings[0])),
		2,
		"gravity zone",
		{ { 1.f, 0.3f, 0.55f, 1.f } }
	},
	{
		/* the setting is the channel; its colour is by channel */
		NULL,
		0,
		0,
		"teleport zone",
		{ { 1.f, 0.7f, 0.3f, 1.f } }
	}
};

/* a teleport channel's colour (red, green, blue), by channel number */
static real const forge_zones_channel_colors[8][3] =
{
	{ 0.75f, 0.30f, 1.00f }, { 0.20f, 0.90f, 0.90f }, { 1.00f, 0.55f, 0.15f }, { 0.35f, 1.00f, 0.35f },
	{ 1.00f, 0.35f, 0.70f }, { 0.35f, 0.55f, 1.00f }, { 0.95f, 0.95f, 0.30f }, { 1.00f, 1.00f, 1.00f }
};

static struct
{
	/* the "Zones" tab */
	boolean showing;
	short new_kind;
	short resize_step_index;

	short zone_count;
	struct forge_zone zones[FORGE_ZONES_MAXIMUM_ZONES];
	short selected_zone_index;

	/* ticks each biped (by absolute object index) has been in a kill zone,
	and the object it was, so a new object in the same slot starts over */
	long kill_object_indices[FORGE_ZONES_MAXIMUM_OBJECTS];
	short kill_ticks[FORGE_ZONES_MAXIMUM_OBJECTS];
	/* whether the object was in a teleport zone that sends last tick (it
	goes only when it walks in, not each tick it stays) */
	boolean teleport_inside[FORGE_ZONES_MAXIMUM_OBJECTS];
	long game_time;
	/* a tick has counted towards a kill since the zones were last all gone */
	boolean kill_counting;

	/* local player 0's countdown, seconds; below zero when there is none */
	real player_kill_seconds;

	/* this frame's drawing, for the render hook */
	boolean drawing;
} forge_zones_globals = { 0 };

/* for a tick (forge_zones_tick_prepare): each zone's axes, and how many
zones there are of each kind. A tick asks of every object whether each zone
holds it; working the axes out again each time (cosines and sines) was most
of what the zones cost. */
static struct
{
	real_vector3d forward[FORGE_ZONES_MAXIMUM_ZONES];
	real_vector3d left[FORGE_ZONES_MAXIMUM_ZONES];
	real_vector3d up[FORGE_ZONES_MAXIMUM_ZONES];
	short kind_counts[NUMBER_OF_FORGE_ZONE_KINDS];
} forge_zones_tick_globals;

/* ---------- private code */

static void forge_zones_forget_kill_timers(
	void)
{
	short index;

	for (index = 0; index < FORGE_ZONES_MAXIMUM_OBJECTS; index++)
	{
		forge_zones_globals.kill_object_indices[index] = NONE;
		forge_zones_globals.kill_ticks[index] = 0;
		forge_zones_globals.teleport_inside[index] = FALSE;
	}
	forge_zones_globals.player_kill_seconds = -1.f;

	return;
}

/* the zone's axes: yaw, then pitch, then roll, as the tools turn an object
(forge_orientation_from_angles, port/linux/game/forge.c) */
static void forge_zones_axes(
	struct forge_zone const *zone,
	real_vector3d *forward,
	real_vector3d *left,
	real_vector3d *up)
{
	set_real_vector3d(forward, cosine(zone->pitch) * cosine(zone->yaw), cosine(zone->pitch) * sine(zone->yaw),
		sine(zone->pitch));
	set_real_vector3d(up, -sine(zone->pitch) * cosine(zone->yaw), -sine(zone->pitch) * sine(zone->yaw),
		cosine(zone->pitch));
	rotate_vector_about_axis(up, forward, sine(zone->roll), cosine(zone->roll));
	cross_product3d(up, forward, left);

	return;
}

/* the point in the zone's own axes: forward, left, up from its center */
static void forge_zones_to_local(
	struct forge_zone const *zone,
	real_point3d const *point,
	real_vector3d *local)
{
	real_vector3d forward;
	real_vector3d left;
	real_vector3d up;
	real_vector3d offset;

	forge_zones_axes(zone, &forward, &left, &up);
	vector_from_points3d(&zone->center, point, &offset);
	local->i = dot_product3d(&offset, &forward);
	local->j = dot_product3d(&offset, &left);
	local->k = dot_product3d(&offset, &up);

	return;
}

static void forge_zones_tick_prepare(
	void)
{
	short zone_index;

	csmemset(forge_zones_tick_globals.kind_counts, 0, sizeof(forge_zones_tick_globals.kind_counts));
	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];

		forge_zones_axes(zone, &forge_zones_tick_globals.forward[zone_index],
			&forge_zones_tick_globals.left[zone_index], &forge_zones_tick_globals.up[zone_index]);
		forge_zones_tick_globals.kind_counts[zone->kind]++;
	}

	return;
}

/* whether the zone holds the point, in a tick, after
forge_zones_tick_prepare */
static boolean forge_zones_tick_contains(
	short zone_index,
	real_point3d const *point)
{
	struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];
	real_vector3d offset;
	real along;
	real across;

	vector_from_points3d(&zone->center, point, &offset);
	if ((real)fabs(dot_product3d(&offset, &forge_zones_tick_globals.up[zone_index])) > zone->half_size.k)
		return FALSE;
	along = dot_product3d(&offset, &forge_zones_tick_globals.forward[zone_index]) / zone->half_size.i;
	across = dot_product3d(&offset, &forge_zones_tick_globals.left[zone_index]) / zone->half_size.j;

	return zone->shape == _forge_zone_circle
		? along * along + across * across <= 1.f
		: (real)fabs(along) <= 1.f && (real)fabs(across) <= 1.f;
}

static real forge_zones_setting(
	struct forge_zone const *zone)
{
	return forge_zone_kinds[zone->kind].settings[zone->setting_index].value;
}

/* where the ray from origin along direction (a unit vector) enters the
zone, 0 when origin is in it; FALSE when it misses it within range */
static boolean forge_zones_ray_hits(
	struct forge_zone const *zone,
	real_point3d const *origin,
	real_vector3d const *direction,
	real range,
	real *distance)
{
	real_vector3d forward;
	real_vector3d left;
	real_vector3d up;
	real_vector3d local_origin;
	real local_direction[3];
	real start[3];
	real extent[3];
	real enter = 0.f;
	real leave = range;
	short axis;

	forge_zones_axes(zone, &forward, &left, &up);
	forge_zones_to_local(zone, origin, &local_origin);
	local_direction[0] = dot_product3d(direction, &forward);
	local_direction[1] = dot_product3d(direction, &left);
	local_direction[2] = dot_product3d(direction, &up);
	start[0] = local_origin.i;
	start[1] = local_origin.j;
	start[2] = local_origin.k;
	extent[0] = zone->half_size.i;
	extent[1] = zone->half_size.j;
	extent[2] = zone->half_size.k;

	for (axis = 0; axis < 3; axis++)
	{
		if ((real)fabs(local_direction[axis]) < 1e-6f)
		{
			if ((real)fabs(start[axis]) > extent[axis])
				return FALSE;
		}
		else
		{
			real near_distance = (-extent[axis] - start[axis]) / local_direction[axis];
			real far_distance = (extent[axis] - start[axis]) / local_direction[axis];

			if (near_distance > far_distance)
			{
				real swap = near_distance;

				near_distance = far_distance;
				far_distance = swap;
			}
			enter = MAX(enter, near_distance);
			leave = MIN(leave, far_distance);
			if (enter > leave)
				return FALSE;
		}
	}
	if (zone->shape == _forge_zone_circle)
	{
		/* and the cylinder in the box: in the ellipse's own units, a circle
		of radius 1 */
		real origin_along = start[0] / extent[0];
		real origin_across = start[1] / extent[1];
		real direction_along = local_direction[0] / extent[0];
		real direction_across = local_direction[1] / extent[1];
		real a = direction_along * direction_along + direction_across * direction_across;
		real b = origin_along * direction_along + origin_across * direction_across;
		real c = origin_along * origin_along + origin_across * origin_across - 1.f;

		if (a < 1e-12f)
		{
			if (c > 0.f)
				return FALSE;
		}
		else
		{
			real discriminant = b * b - a * c;
			real root;

			if (discriminant < 0.f)
				return FALSE;
			root = square_root(discriminant);
			enter = MAX(enter, (-b - root) / a);
			leave = MIN(leave, (-b + root) / a);
			if (enter > leave)
				return FALSE;
		}
	}
	*distance = enter;

	return TRUE;
}

/* the zone the crosshair points into, nearest first, or NONE */
static short forge_zones_aimed_zone(
	real *aimed_distance)
{
	struct observer_result const *camera = observer_get_camera(FORGE_ZONES_LOCAL_PLAYER_INDEX);
	real nearest_distance = FORGE_ZONES_SELECT_RANGE;
	short aimed_zone_index = NONE;
	short zone_index;

	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		real distance;

		if (forge_zones_ray_hits(&forge_zones_globals.zones[zone_index],
				&camera->position, &camera->forward, FORGE_ZONES_SELECT_RANGE, &distance) &&
			distance <= nearest_distance)
		{
			nearest_distance = distance;
			aimed_zone_index = zone_index;
		}
	}
	if (aimed_distance)
		*aimed_distance = nearest_distance;

	return aimed_zone_index;
}

/* the zone stands on the crosshair's surface, turned the way the camera
looks; FALSE when the camera is outside the map */
static boolean forge_zones_place_at_crosshair(
	struct forge_zone *zone)
{
	struct observer_result const *camera = observer_get_camera(FORGE_ZONES_LOCAL_PLAYER_INDEX);
	float point[3];
	float normal[3];

	if (!forge_point_at_crosshair(point, normal))
		return FALSE;

	zone->center.x = point[0];
	zone->center.y = point[1];
	zone->center.z = point[2] + zone->half_size.k - FORGE_ZONES_FLOOR_DEPTH;
	zone->yaw = arctangent(camera->forward.j, camera->forward.i);

	return TRUE;
}


static boolean forge_zones_local_game(
	void)
{
	/* a local game, or the host of a system link game, whose layout takes
	the zones to its clients (forge_layout.c) */
	boolean local = halo_mods_authoritative();

	if (!local)
		terminal_printf(global_real_argb_orange, "forge_zones: the zones are the host's");

	return local;
}

/* the teleport zones sharing the zone's channel, itself included */
static short forge_zones_channel_count(
	short channel)
{
	short zone_index;
	short count = 0;

	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];

		if (zone->kind == _forge_zone_teleport && zone->channel == channel)
			count++;
	}

	return count;
}

/* where a teleport zone sends: the next zone of its channel after it (round
the list) that receives, or NONE */
static short forge_zones_teleport_destination(
	short zone_index)
{
	struct forge_zone const *from = &forge_zones_globals.zones[zone_index];
	short offset;

	for (offset = 1; offset < forge_zones_globals.zone_count; offset++)
	{
		short candidate_index = (short)((zone_index + offset) % forge_zones_globals.zone_count);
		struct forge_zone const *candidate = &forge_zones_globals.zones[candidate_index];

		if (candidate->kind == _forge_zone_teleport &&
			candidate->channel == from->channel &&
			candidate->direction != _forge_zone_entrance_only)
		{
			return candidate_index;
		}
	}

	return NONE;
}

/* the teleport zone that sends and has the point in it, or NONE (in a tick) */
static short forge_zones_teleport_entrance(
	real_point3d const *point)
{
	short zone_index;

	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];

		if (zone->kind == _forge_zone_teleport &&
			zone->direction != _forge_zone_exit_only &&
			forge_zones_tick_contains(zone_index, point))
		{
			return zone_index;
		}
	}

	return NONE;
}

static void forge_zones_describe(
	struct forge_zone const *zone,
	char *text,
	size_t size)
{
	char setting[160];

	switch (zone->kind)
	{
	case _forge_zone_teleport:
		_snprintf(setting, sizeof(setting), "channel %d, %s", zone->channel,
			forge_zones_direction_names[zone->direction]);
		break;
	default:
		_snprintf(setting, sizeof(setting), "%s",
			forge_zone_kinds[zone->kind].settings[zone->setting_index].name);
		break;
	}
	_snprintf(text, size, "%s %s, %s, %.2f x %.2f x %.2f",
		forge_zones_shape_names[zone->shape], forge_zone_kinds[zone->kind].name, setting,
		2.f * zone->half_size.i, 2.f * zone->half_size.j, 2.f * zone->half_size.k);

	return;
}

/* the kind's own first setting (a change of kind starts over) */
static void forge_zones_reset_setting(
	struct forge_zone *zone)
{
	zone->setting_index = forge_zone_kinds[zone->kind].default_setting_index;
	zone->channel = 1;
	zone->direction = _forge_zone_two_way;

	return;
}

static void forge_zones_resize(
	struct forge_zone *zone,
	real *half_size,
	real step)
{
	real bottom = zone->center.z - zone->half_size.k;

	*half_size = PIN(*half_size + step / 2.f, FORGE_ZONES_MINIMUM_SIZE / 2.f, FORGE_ZONES_MAXIMUM_SIZE / 2.f);
	/* the floor stays where it is */
	zone->center.z = bottom + zone->half_size.k;

	return;
}

static boolean forge_zones_pick_up(
	short zone_index,
	boolean spawned);

/* a new zone at the crosshair, held as a spawned object is until it is put
down */
static void forge_zones_new(
	void)
{
	struct forge_zone zone;
	char description[128];

	if (forge_zones_globals.zone_count >= FORGE_ZONES_MAXIMUM_ZONES)
	{
		terminal_printf(global_real_argb_orange, "forge_zones: at most %d zones", FORGE_ZONES_MAXIMUM_ZONES);
		return;
	}

	memset(&zone, 0, sizeof(zone));
	zone.kind = forge_zones_globals.new_kind;
	forge_zones_reset_setting(&zone);
	set_real_vector3d(&zone.half_size,
		FORGE_ZONES_DEFAULT_LENGTH / 2.f, FORGE_ZONES_DEFAULT_WIDTH / 2.f, FORGE_ZONES_DEFAULT_HEIGHT / 2.f);
	if (zone.kind == _forge_zone_teleport)
	{
		/* the next teleport joins the last channel that has only one zone,
		else starts a new one */
		short channel;

		for (channel = 1; channel <= FORGE_ZONES_MAXIMUM_CHANNEL; channel++)
		{
			if (forge_zones_channel_count(channel) < 2)
			{
				zone.channel = channel;
				break;
			}
		}
	}
	if (!forge_zones_place_at_crosshair(&zone))
	{
		terminal_printf(global_real_argb_orange, "forge_zones: the camera is outside the map");
		return;
	}

	forge_zones_globals.selected_zone_index = forge_zones_globals.zone_count;
	forge_zones_globals.zones[forge_zones_globals.zone_count++] = zone;
	forge_zones_globals.showing = TRUE;
	forge_zones_describe(&zone, description, sizeof(description));
	terminal_printf(global_real_argb_green, "forge_zones: new %s", description);
	forge_zones_pick_up(forge_zones_globals.selected_zone_index, TRUE);

	return;
}

static void forge_zones_remove(
	short zone_index)
{
	short index;

	for (index = zone_index; index < forge_zones_globals.zone_count - 1; index++)
		forge_zones_globals.zones[index] = forge_zones_globals.zones[index + 1];
	forge_zones_globals.zone_count--;
	forge_zones_globals.selected_zone_index = (short)MIN(zone_index, forge_zones_globals.zone_count - 1);
	forge_zones_forget_kill_timers();
	terminal_printf(global_real_argb_green, "forge_zones: zone removed");

	return;
}

/* whether the zones are drawn and can be aimed at: shown, and only in forge
mode (the tools' flying camera) or while the tools hold something or have
their menu open; never on foot, so a game can be played with them shown */
static boolean forge_zones_visible(
	void)
{
	return forge_zones_globals.showing && forge_mode_on() &&
		(forge_flying(FORGE_ZONES_LOCAL_PLAYER_INDEX) || forge_busy());
}

static void forge_zones_update(
	void)
{
	forge_zones_globals.drawing = FALSE;
	if (local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX) == NONE ||
		game_connection() == _game_connection_film_playback)
	{
		return;
	}
	forge_zones_globals.drawing = TRUE;

	/* with the zones shown, aiming at one chooses it, unless the tools' menu
	is in use (its rows say which zone) or a cutscene plays */
	if (forge_zones_visible() && !forge_busy() && !cinematic_in_progress())
	{
		short aimed_zone_index = forge_zones_aimed_zone(NULL);

		if (aimed_zone_index != NONE)
			forge_zones_globals.selected_zone_index = aimed_zone_index;
	}

	return;
}

/* a zone's colour: its kind's, a lift's green, a teleport's by channel */
static real_argb_color forge_zones_zone_color(
	struct forge_zone const *zone)
{
	real_argb_color color = forge_zone_kinds[zone->kind].color;

	if (zone->kind == _forge_zone_gravity && forge_zones_setting(zone) < 0.f)
	{
		color.red = 0.3f;
		color.green = 1.f;
		color.blue = 0.4f;
	}
	else if (zone->kind == _forge_zone_teleport)
	{
		real const *channel_color = forge_zones_channel_colors[(zone->channel - 1) % 8];

		color.red = channel_color[0];
		color.green = channel_color[1];
		color.blue = channel_color[2];
	}

	return color;
}

/* the zone's point at forward, left and up from its center */
static void forge_zones_zone_point(
	struct forge_zone const *zone,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real along,
	real across,
	real above,
	real_point3d *point)
{
	point->x = zone->center.x + forward->i * along + left->i * across + up->i * above;
	point->y = zone->center.y + forward->j * along + left->j * across + up->j * above;
	point->z = zone->center.z + forward->k * along + left->k * across + up->k * above;

	return;
}

static void forge_zones_draw_box(
	struct forge_zone const *zone,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real_argb_color const *edge_color,
	real_argb_color const *face_color)
{
	real_point3d corners[8];
	short corner_index;
	short index;
	/* the four corners of a face, in order, by corner number */
	static short const faces[6][4] =
	{
		{ 0, 1, 3, 2 }, { 4, 5, 7, 6 },
		{ 0, 1, 5, 4 }, { 2, 3, 7, 6 },
		{ 0, 2, 6, 4 }, { 1, 3, 7, 5 }
	};
	static short const edges[12][2] =
	{
		{ 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
		{ 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
	};

	/* corner bits: 1 forward, 2 left, 4 up */
	for (corner_index = 0; corner_index < 8; corner_index++)
	{
		forge_zones_zone_point(zone, forward, left, up,
			(corner_index & 1) ? zone->half_size.i : -zone->half_size.i,
			(corner_index & 2) ? zone->half_size.j : -zone->half_size.j,
			(corner_index & 4) ? zone->half_size.k : -zone->half_size.k,
			&corners[corner_index]);
	}

	for (index = 0; index < 6; index++)
	{
		rasterizer_debug_triangle(&corners[faces[index][0]], &corners[faces[index][1]],
			&corners[faces[index][2]], face_color);
		rasterizer_debug_triangle(&corners[faces[index][0]], &corners[faces[index][2]],
			&corners[faces[index][3]], face_color);
	}
	for (index = 0; index < 12; index++)
		rasterizer_debug_line_shaded(&corners[edges[index][0]], &corners[edges[index][1]], edge_color, edge_color);

	return;
}

static void forge_zones_draw_cylinder(
	struct forge_zone const *zone,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real_argb_color const *edge_color,
	real_argb_color const *face_color,
	boolean fill_sides)
{
	real_point3d bottom[FORGE_ZONES_CIRCLE_SEGMENTS];
	real_point3d top[FORGE_ZONES_CIRCLE_SEGMENTS];
	real_point3d bottom_center;
	real_point3d top_center;
	short index;

	for (index = 0; index < FORGE_ZONES_CIRCLE_SEGMENTS; index++)
	{
		real angle = 2.f * _pi * (real)index / FORGE_ZONES_CIRCLE_SEGMENTS;
		real along = cosine(angle) * zone->half_size.i;
		real across = sine(angle) * zone->half_size.j;

		forge_zones_zone_point(zone, forward, left, up, along, across, -zone->half_size.k, &bottom[index]);
		forge_zones_zone_point(zone, forward, left, up, along, across, zone->half_size.k, &top[index]);
	}
	forge_zones_zone_point(zone, forward, left, up, 0.f, 0.f, -zone->half_size.k, &bottom_center);
	forge_zones_zone_point(zone, forward, left, up, 0.f, 0.f, zone->half_size.k, &top_center);

	for (index = 0; index < FORGE_ZONES_CIRCLE_SEGMENTS; index++)
	{
		short next = (short)((index + 1) % FORGE_ZONES_CIRCLE_SEGMENTS);

		rasterizer_debug_triangle(&bottom_center, &bottom[index], &bottom[next], face_color);
		rasterizer_debug_triangle(&top_center, &top[index], &top[next], face_color);
		/* (the sides only of the chosen zone: each is drawn by itself) */
		if (fill_sides)
		{
			rasterizer_debug_triangle(&bottom[index], &bottom[next], &top[next], face_color);
			rasterizer_debug_triangle(&bottom[index], &top[next], &top[index], face_color);
		}
		rasterizer_debug_line_shaded(&bottom[index], &bottom[next], edge_color, edge_color);
		rasterizer_debug_line_shaded(&top[index], &top[next], edge_color, edge_color);
		/* (a few of the sides, not all: it would be a blur) */
		if (index % (FORGE_ZONES_CIRCLE_SEGMENTS / 4) == 0)
			rasterizer_debug_line_shaded(&bottom[index], &top[index], edge_color, edge_color);
	}

	return;
}

/* a teleport zone's forward: an arrow on its floor, from the middle to the
front, with its head */
static void forge_zones_draw_arrow(
	struct forge_zone const *zone,
	real_vector3d const *forward,
	real_vector3d const *left,
	real_vector3d const *up,
	real_argb_color const *color)
{
	real above = -zone->half_size.k + FORGE_ZONES_FLOOR_DEPTH + 0.02f;
	real length = zone->half_size.i * 0.9f;
	real head = MIN(zone->half_size.i, zone->half_size.j) * 0.35f;
	real_point3d tail;
	real_point3d tip;
	real_point3d head_left;
	real_point3d head_right;

	forge_zones_zone_point(zone, forward, left, up, -length * 0.5f, 0.f, above, &tail);
	forge_zones_zone_point(zone, forward, left, up, length, 0.f, above, &tip);
	forge_zones_zone_point(zone, forward, left, up, length - head, head, above, &head_left);
	forge_zones_zone_point(zone, forward, left, up, length - head, -head, above, &head_right);
	rasterizer_debug_line_shaded(&tail, &tip, color, color);
	rasterizer_debug_line_shaded(&tip, &head_left, color, color);
	rasterizer_debug_line_shaded(&tip, &head_right, color, color);
	rasterizer_debug_triangle(&tip, &head_left, &head_right, color);

	return;
}

static void forge_zones_draw_zone(
	struct forge_zone const *zone,
	boolean selected)
{
	real_vector3d forward;
	real_vector3d left;
	real_vector3d up;
	real_argb_color edge_color = forge_zones_zone_color(zone);
	real_argb_color face_color = edge_color;

	edge_color.alpha = 1.f;
	if (!selected)
	{
		edge_color.red *= FORGE_ZONES_DIM_EDGE;
		edge_color.green *= FORGE_ZONES_DIM_EDGE;
		edge_color.blue *= FORGE_ZONES_DIM_EDGE;
	}
	face_color.alpha = selected ? 0.25f : 0.12f;

	forge_zones_axes(zone, &forward, &left, &up);
	if (zone->shape == _forge_zone_circle)
		forge_zones_draw_cylinder(zone, &forward, &left, &up, &edge_color, &face_color, selected);
	else
		forge_zones_draw_box(zone, &forward, &left, &up, &edge_color, &face_color);
	if (zone->kind == _forge_zone_teleport)
	{
		real_argb_color arrow_color = edge_color;

		arrow_color.alpha = 1.f;
		forge_zones_draw_arrow(zone, &forward, &left, &up, &arrow_color);
	}

	return;
}


static void forge_zones_render(
	void)
{
	short x0;
	short y0;
	short x1;
	short y1;

	if (forge_zones_globals.drawing && forge_zones_globals.player_kill_seconds >= 0.f &&
		halo_mod_screen(&x0, &y0, &x1, &y1))
	{
		long font = halo_mod_font(TRUE);

		if (font != NONE)
		{
			char warning[64];
			short line_height = halo_mod_line_height(font);
			short top = (short)((y0 + y1) / 2 - 3 * line_height);

			_snprintf(warning, sizeof(warning), "Leave the kill zone: %d",
				(int)ceil(forge_zones_globals.player_kill_seconds));
			halo_mod_draw_text(font, x0, top, x1, (short)(top + line_height),
				HALO_MOD_TEXT_CENTER, FORGE_ZONES_WARNING_COLOR, warning);
		}
	}

	/* drawn only after an update: nothing is left over when the game stops
	updating the mods (the console, menus, loading) */
	forge_zones_globals.drawing = FALSE;

	return;
}

/* the zones, in each player's view (the world drawing hook): the game
draws the queued debug geometry at the end of the view's window
(rasterizer_debug_draw) */
static void forge_zones_render_world(
	void)
{
	struct observer_result const *camera;
	short zone_index;

	if (!forge_zones_visible() || game_connection() == _game_connection_film_playback)
		return;

	camera = observer_get_camera(FORGE_ZONES_LOCAL_PLAYER_INDEX);
	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];
		boolean selected = zone_index == forge_zones_globals.selected_zone_index;
		real extent = MAX(MAX(zone->half_size.i, zone->half_size.j), zone->half_size.k);

		if (!selected && distance3d(&camera->position, &zone->center) - extent > FORGE_ZONES_DRAW_RANGE)
			continue;
		forge_zones_draw_zone(zone, selected);
		if (zone->kind == _forge_zone_teleport)
		{
			short destination_index = forge_zones_teleport_destination(zone_index);

			if (destination_index != NONE && zone->direction != _forge_zone_exit_only)
			{
				real_argb_color color = forge_zones_zone_color(zone);

				color.alpha = 1.f;
				rasterizer_debug_line_shaded(&zone->center, &forge_zones_globals.zones[destination_index].center,
					&color, &color);
			}
		}
	}

	return;
}

/* the lowest delay, in ticks, of the kill zones the point is in, or NONE */
static long forge_zones_kill_delay(
	real_point3d const *point)
{
	long delay = NONE;
	short zone_index;

	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];

		if (zone->kind == _forge_zone_kill && forge_zones_tick_contains(zone_index, point))
		{
			long ticks = (long)(forge_zones_setting(zone) * TICKS_PER_SECOND);

			if (delay == NONE || ticks < delay)
				delay = ticks;
		}
	}

	return delay;
}

static void forge_zones_tick_kill(
	void)
{
	long player_unit_index = NONE;
	long player_index = local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX);
	struct object_iterator iterator;
	struct object_datum *object;

	if (player_index != NONE)
		player_unit_index = player_get(player_index)->unit_index;
	forge_zones_globals.player_kill_seconds = -1.f;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		long delay;

		if (absolute_index >= FORGE_ZONES_MAXIMUM_OBJECTS)
			continue;
		if (forge_zones_globals.kill_object_indices[absolute_index] != iterator.index)
		{
			forge_zones_globals.kill_object_indices[absolute_index] = iterator.index;
			forge_zones_globals.kill_ticks[absolute_index] = 0;
		}

		delay = TEST_FLAG(object->object.damage_flags, _object_dead_bit)
			? NONE
			: forge_zones_kill_delay(&object->object.bounding_sphere_center);
		if (delay == NONE)
		{
			forge_zones_globals.kill_ticks[absolute_index] = 0;
		}
		else if (forge_zones_globals.kill_ticks[absolute_index] >= delay)
		{
			unit_kill(iterator.index);
			forge_zones_globals.kill_ticks[absolute_index] = 0;
		}
		else
		{
			forge_zones_globals.kill_ticks[absolute_index]++;
			if (iterator.index == player_unit_index)
			{
				forge_zones_globals.player_kill_seconds =
					(real)(delay - forge_zones_globals.kill_ticks[absolute_index] + 1) / TICKS_PER_SECOND;
			}
		}
	}

	return;
}

/* the gravity scale of the gravity zones the point is in (the first one),
or FALSE when it is in none */
static boolean forge_zones_gravity_scale(
	real_point3d const *point,
	real *scale)
{
	short zone_index;

	for (zone_index = 0; zone_index < forge_zones_globals.zone_count; zone_index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[zone_index];

		if (zone->kind == _forge_zone_gravity && forge_zones_tick_contains(zone_index, point))
		{
			*scale = forge_zones_setting(zone);
			return TRUE;
		}
	}

	return FALSE;
}

static void forge_zones_tick_gravity(
	void)
{
	struct object_iterator iterator;
	struct object_datum *object;

	object_iterator_new(&iterator, _object_mask_biped | _object_mask_vehicle | _object_mask_item, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		real scale;
		real_vector3d *velocity = &object->object.translational_velocity;

		/* what rides or is carried moves with what holds it */
		if (object->object.parent_object_index != NONE ||
			!forge_zones_gravity_scale(&object->object.bounding_sphere_center, &scale))
		{
			continue;
		}

		if (object->object.type == _object_type_biped)
		{
			struct biped_datum *biped = (struct biped_datum *)object;

			if (!TEST_FLAG(biped->biped.flags, _biped_airborne_bit))
			{
				/* the ground holds a biped up; only a lift takes it off */
				if (scale >= 0.f)
					continue;
				velocity->k = MAX(velocity->k, FORGE_ZONES_LIFT_TAKEOFF_SPEED);
			}
		}
		else if (TEST_FLAG(object->object.flags, _object_at_rest_bit))
		{
			/* resting stays resting, unless it lifts */
			if (scale >= 0.f)
				continue;
			SET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);
		}

		/* the game takes the map's gravity off this tick; give back what
		the zone does not want */
		velocity->k += (1.f - scale) * global_gravity;
	}

	return;
}


/* sends what walked into a teleport zone to the next zone of its channel */
static void forge_zones_teleport(
	long object_index,
	struct object_datum *object,
	short from_index,
	short to_index)
{
	struct forge_zone const *from = &forge_zones_globals.zones[from_index];
	struct forge_zone const *to = &forge_zones_globals.zones[to_index];
	real_vector3d local;
	real_vector3d to_forward;
	real_vector3d to_left;
	real_vector3d to_up;
	real_point3d position;
	real_vector3d forward = object->object.forward;
	real_vector3d *velocity = &object->object.translational_velocity;
	real turn = to->yaw - from->yaw;
	real cosine_turn = cosine(turn);
	real sine_turn = sine(turn);
	real velocity_i = velocity->i;
	real velocity_j = velocity->j;
	long player_index = local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX);

	/* the same place in the box it arrives in, kept clear of its walls */
	forge_zones_to_local(from, &object->object.position, &local);
	local.i = PIN(local.i / from->half_size.i, -1.f, 1.f) * MAX(to->half_size.i - FORGE_ZONES_TELEPORT_MARGIN, 0.f);
	local.j = PIN(local.j / from->half_size.j, -1.f, 1.f) * MAX(to->half_size.j - FORGE_ZONES_TELEPORT_MARGIN, 0.f);
	local.k = MAX(local.k + from->half_size.k, FORGE_ZONES_TELEPORT_MARGIN) - to->half_size.k;
	if (to->shape == _forge_zone_circle)
	{
		/* (a corner of a square is outside a circle) */
		real along = to->half_size.i > FORGE_ZONES_TELEPORT_MARGIN
			? local.i / (to->half_size.i - FORGE_ZONES_TELEPORT_MARGIN)
			: 0.f;
		real across = to->half_size.j > FORGE_ZONES_TELEPORT_MARGIN
			? local.j / (to->half_size.j - FORGE_ZONES_TELEPORT_MARGIN)
			: 0.f;
		real distance_squared = along * along + across * across;

		if (distance_squared > 1.f)
		{
			real scale = reciprocal_square_root(distance_squared);

			local.i *= scale;
			local.j *= scale;
		}
	}
	forge_zones_axes(to, &to_forward, &to_left, &to_up);
	position.x = to->center.x + to_forward.i * local.i + to_left.i * local.j + to_up.i * local.k;
	position.y = to->center.y + to_forward.j * local.i + to_left.j * local.j + to_up.j * local.k;
	position.z = to->center.z + to_forward.k * local.i + to_left.k * local.j + to_up.k * local.k;

	/* turned by the difference of the boxes' directions about the vertical
	(their yaws: a tilted box does not tilt what goes through it) */
	forward.i = object->object.forward.i * cosine_turn - object->object.forward.j * sine_turn;
	forward.j = object->object.forward.i * sine_turn + object->object.forward.j * cosine_turn;
	velocity->i = velocity_i * cosine_turn - velocity_j * sine_turn;
	velocity->j = velocity_i * sine_turn + velocity_j * cosine_turn;

	object_set_position(object_index, &position, &forward, NULL);
	if (player_index != NONE && player_get(player_index)->unit_index == object_index)
	{
		struct observer_result const *camera = observer_get_camera(FORGE_ZONES_LOCAL_PLAYER_INDEX);
		real_vector3d facing = camera->forward;

		facing.i = camera->forward.i * cosine_turn - camera->forward.j * sine_turn;
		facing.j = camera->forward.i * sine_turn + camera->forward.j * cosine_turn;
		player_control_set_facing(FORGE_ZONES_LOCAL_PLAYER_INDEX, &facing);
	}

	return;
}

static void forge_zones_tick_teleport(
	void)
{
	struct object_iterator iterator;
	struct object_datum *object;

	object_iterator_new(&iterator, _object_mask_biped | _object_mask_vehicle, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		short from_index;

		if (absolute_index >= FORGE_ZONES_MAXIMUM_OBJECTS)
			continue;
		if (forge_zones_globals.kill_object_indices[absolute_index] != iterator.index)
		{
			forge_zones_globals.kill_object_indices[absolute_index] = iterator.index;
			forge_zones_globals.kill_ticks[absolute_index] = 0;
			forge_zones_globals.teleport_inside[absolute_index] = FALSE;
		}

		/* what rides or is carried goes with what holds it, the dead stay */
		if (object->object.parent_object_index != NONE ||
			TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		{
			continue;
		}

		from_index = forge_zones_teleport_entrance(&object->object.position);
		if (from_index == NONE)
		{
			forge_zones_globals.teleport_inside[absolute_index] = FALSE;
		}
		else if (!forge_zones_globals.teleport_inside[absolute_index])
		{
			short to_index = forge_zones_teleport_destination(from_index);

			/* (an entrance with no other zone of its channel keeps things
			as they are) */
			forge_zones_globals.teleport_inside[absolute_index] = TRUE;
			if (to_index != NONE)
			{
				forge_zones_teleport(iterator.index, object, from_index, to_index);
				/* arrived in a zone that sends: it must be left before it
				sends again */
				forge_zones_globals.teleport_inside[absolute_index] =
					forge_zones_globals.zones[to_index].direction != _forge_zone_exit_only;
			}
		}
	}

	return;
}

/* local and system link games (halo_mods_tick): kill and teleport zones
where the game state is decided (a local game, the host), gravity zones on
every machine, whose physics moves its own player */
static void forge_zones_tick(
	void)
{
	long game_time = game_time_get();

	/* back at a checkpoint: count again */
	if (game_time < forge_zones_globals.game_time)
		forge_zones_forget_kill_timers();
	forge_zones_globals.game_time = game_time;

	/* only the kinds there are zones of: each looks at every object */
	forge_zones_globals.player_kill_seconds = -1.f;
	if (forge_zones_globals.kill_counting && forge_zones_globals.zone_count == 0)
	{
		/* the last zone is gone: nothing is counted towards a kill any more */
		csmemset(forge_zones_globals.kill_ticks, 0, sizeof(forge_zones_globals.kill_ticks));
		forge_zones_globals.kill_counting = FALSE;
	}
	if (forge_zones_globals.zone_count > 0)
	{
		forge_zones_tick_prepare();
		if (forge_zones_tick_globals.kind_counts[_forge_zone_kill] > 0 && halo_mods_authoritative())
		{
			forge_zones_tick_kill();
			forge_zones_globals.kill_counting = TRUE;
		}
		if (forge_zones_tick_globals.kind_counts[_forge_zone_gravity] > 0)
			forge_zones_tick_gravity();
		if (forge_zones_tick_globals.kind_counts[_forge_zone_teleport] > 0 && halo_mods_authoritative())
			forge_zones_tick_teleport();
	}

	return;
}

static void forge_zones_new_map(
	void)
{
	forge_zones_globals.zone_count = 0;
	forge_zones_globals.selected_zone_index = NONE;
	forge_zones_globals.game_time = 0;
	forge_zones_forget_kill_timers();

	return;
}

/* ---------- picking a zone up */

/* as the tools hold an object (port/linux/game/forge.c): the axis left and
right turn the zone about (T), and how it turns in steps (V / X): free, then
these degrees */
enum
{
	_forge_zones_hold_axis_yaw = 0,
	_forge_zones_hold_axis_pitch,
	_forge_zones_hold_axis_roll,
	NUMBER_OF_FORGE_ZONES_HOLD_AXES
};

static char const *const forge_zones_hold_axis_names[NUMBER_OF_FORGE_ZONES_HOLD_AXES] = { "yaw", "pitch", "roll" };
static real const forge_zones_hold_snaps[] = { 0.f, 15.f, 45.f, 90.f, 180.f };
static char const *const forge_zones_hold_snap_names[] =
{
	"free", "15 degrees", "45 degrees", "90 degrees", "180 degrees"
};

static struct
{
	short zone_index;
	struct forge_zone original;
	real height;
	short axis;
	short snap_index;
	/* the mouse turns it (its right button, flying): the yaw and pitch it
	has turned to, before the steps */
	boolean mouse_turning;
	real mouse_yaw;
	real mouse_pitch;
	/* a new zone, not yet put down: cancelling removes it */
	boolean spawned;
} forge_zones_hold_globals = { NONE };

static void forge_zones_hold_describe(
	char *line,
	unsigned long size)
{
	_snprintf(line, size, "turning: %s, step: %s, raised %.2f",
		forge_zones_hold_axis_names[forge_zones_hold_globals.axis],
		forge_zones_hold_snap_names[forge_zones_hold_globals.snap_index],
		forge_zones_hold_globals.height);

	return;
}

static int forge_zones_hold_update(
	struct halo_forge_hold_input const *input)
{
	struct forge_zone *zone;
	real *angle;
	float point[3];
	float normal[3];

	/* the zone is gone (a new map, a removal): nothing to hold */
	if (forge_zones_hold_globals.zone_index < 0 ||
		forge_zones_hold_globals.zone_index >= forge_zones_globals.zone_count)
	{
		return TRUE;
	}
	zone = &forge_zones_globals.zones[forge_zones_hold_globals.zone_index];

	if (input && input->cancel && forge_zones_hold_globals.spawned)
	{
		/* as a spawned object: gone when it is not put down */
		forge_zones_remove(forge_zones_hold_globals.zone_index);
		return TRUE;
	}
	if (!input || input->cancel)
	{
		*zone = forge_zones_hold_globals.original;
		return TRUE;
	}
	if (input->delete_pressed)
	{
		forge_zones_remove(forge_zones_hold_globals.zone_index);
		return TRUE;
	}
	if (input->place)
		return TRUE;

	if (input->axis_pressed)
	{
		forge_zones_hold_globals.axis = (short)((forge_zones_hold_globals.axis + 1) %
			NUMBER_OF_FORGE_ZONES_HOLD_AXES);
	}
	if (input->snap_pressed)
	{
		forge_zones_hold_globals.snap_index = (short)((forge_zones_hold_globals.snap_index + 1) %
			(sizeof(forge_zones_hold_snaps) / sizeof(forge_zones_hold_snaps[0])));
	}
	angle = forge_zones_hold_globals.axis == _forge_zones_hold_axis_yaw
		? &zone->yaw
		: forge_zones_hold_globals.axis == _forge_zones_hold_axis_pitch ? &zone->pitch : &zone->roll;
	if (forge_zones_hold_snaps[forge_zones_hold_globals.snap_index] == 0.f)
	{
		/* half a circle a second, left is anticlockwise */
		if (input->left != input->right)
			*angle += (input->left ? 1.f : -1.f) * input->seconds * FORGE_ZONES_HOLD_TURN_SPEED;
	}
	else if (input->left_pressed != input->right_pressed)
	{
		/* to the next whole step, as an object */
		real step = DEGREES_TO_RADIANS(forge_zones_hold_snaps[forge_zones_hold_globals.snap_index]);

		*angle = (real)floor(*angle / step + 0.5f) * step + (input->left_pressed ? step : -step);
	}

	/* the right stick turns it freely, the mouse by the step, as an object
	(forge_camera_turn_held, port/linux/game/forge.c) */
	if (input->mouse_turning)
	{
		real step = DEGREES_TO_RADIANS(forge_zones_hold_snaps[forge_zones_hold_globals.snap_index]);

		if (!forge_zones_hold_globals.mouse_turning)
		{
			forge_zones_hold_globals.mouse_yaw = zone->yaw;
			forge_zones_hold_globals.mouse_pitch = zone->pitch;
		}
		forge_zones_hold_globals.mouse_yaw += input->turn_yaw;
		forge_zones_hold_globals.mouse_pitch += input->turn_pitch;
		if (input->turn_yaw != 0.f)
		{
			zone->yaw = step == 0.f
				? forge_zones_hold_globals.mouse_yaw
				: (real)floor(forge_zones_hold_globals.mouse_yaw / step + 0.5f) * step;
		}
		if (input->turn_pitch != 0.f)
		{
			zone->pitch = step == 0.f
				? forge_zones_hold_globals.mouse_pitch
				: (real)floor(forge_zones_hold_globals.mouse_pitch / step + 0.5f) * step;
		}
	}
	else
	{
		zone->yaw += input->turn_yaw;
		zone->pitch += input->turn_pitch;
	}
	forge_zones_hold_globals.mouse_turning = input->mouse_turning;
	forge_zones_hold_globals.height += ((input->up ? 1.f : 0.f) - (input->down ? 1.f : 0.f)) * input->seconds *
		FORGE_ZONES_HOLD_RAISE_SPEED;

	/* the box stands where the crosshair points, raised as asked */
	if (forge_point_at_crosshair(point, normal))
	{
		zone->center.x = point[0];
		zone->center.y = point[1];
		zone->center.z = point[2] + zone->half_size.k - FORGE_ZONES_FLOOR_DEPTH + forge_zones_hold_globals.height;
	}

	return FALSE;
}

static struct halo_forge_hold const forge_zones_hold =
{
	"a zone",
	forge_zones_hold_describe,
	forge_zones_hold_update,
	TRUE
};

/* holds the zone (spawned: a new one, gone when it is not put down); FALSE
when the tools cannot take it now */
static boolean forge_zones_pick_up(
	short zone_index,
	boolean spawned)
{
	boolean picked_up = FALSE;

	if (forge_zones_local_game())
	{
		forge_zones_hold_globals.zone_index = zone_index;
		forge_zones_hold_globals.original = forge_zones_globals.zones[zone_index];
		forge_zones_hold_globals.height = 0.f;
		forge_zones_hold_globals.axis = _forge_zones_hold_axis_yaw;
		forge_zones_hold_globals.spawned = spawned;
		forge_zones_hold_globals.mouse_turning = FALSE;
		picked_up = forge_mod_hold_begin(&forge_zones_hold);
		if (picked_up)
		{
			forge_zones_globals.selected_zone_index = zone_index;
			forge_zones_globals.showing = TRUE;
		}
	}

	return picked_up;
}

/* the tools' pick up: a zone under the crosshair, when no object is nearer */
static int forge_zones_grab(
	void)
{
	real zone_distance;
	long object_index;
	short zone_index;

	if (!forge_zones_visible() || !halo_mods_authoritative() ||
		local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX) == NONE)
	{
		return FALSE;
	}
	zone_index = forge_zones_aimed_zone(&zone_distance);
	if (zone_index == NONE)
		return FALSE;

	object_index = forge_object_at_crosshair();
	if (object_index != NONE && object_try_and_get(object_index))
	{
		struct observer_result const *camera = observer_get_camera(FORGE_ZONES_LOCAL_PLAYER_INDEX);
		real_vector3d to_object;

		vector_from_points3d(&camera->position, &object_get(object_index)->object.bounding_sphere_center, &to_object);
		if (magnitude3d(&to_object) < zone_distance)
			return FALSE;
	}

	return forge_zones_pick_up(zone_index, FALSE);
}

/* X on a zone: the tools' menu opens on the tab, which chooses it. A zone
the camera is in is as far as a zone can be, so that what the crosshair
points at inside it comes first */
static int forge_zones_properties(
	float *distance)
{
	real zone_distance;

	if (!forge_zones_visible() || local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX) == NONE ||
		forge_zones_aimed_zone(&zone_distance) == NONE)
	{
		return FALSE;
	}
	*distance = zone_distance > 0.f ? zone_distance : FORGE_ZONES_SELECT_RANGE;

	return TRUE;
}

/* ---------- the tools' menu page */

/* the rows, in order; a zone's own rows come only with a zone chosen */
enum forge_zones_row
{
	_forge_zones_row_show = 0,
	_forge_zones_row_new,
	_forge_zones_row_zone,
	_forge_zones_row_kind,
	_forge_zones_row_shape,
	_forge_zones_row_setting,
	_forge_zones_row_extra,
	_forge_zones_row_width,
	_forge_zones_row_length,
	_forge_zones_row_height,
	_forge_zones_row_step,
	_forge_zones_row_pick_up,
	_forge_zones_row_move,
	_forge_zones_row_remove
};

static boolean forge_zones_has_selection(
	void)
{
	return forge_zones_globals.selected_zone_index >= 0 &&
		forge_zones_globals.selected_zone_index < forge_zones_globals.zone_count;
}

/* the row's meaning at this place in the menu; NONE past the last row */
static short forge_zones_row_kind(
	short row)
{
	short rows[_forge_zones_row_remove + 1];
	short count = 0;
	short id;

	for (id = _forge_zones_row_show; id <= _forge_zones_row_remove; id++)
	{
		if (id >= _forge_zones_row_kind && !forge_zones_has_selection())
			break;
		if (id == _forge_zones_row_extra)
		{
			short kind = forge_zones_globals.zones[forge_zones_globals.selected_zone_index].kind;

			if (kind != _forge_zone_teleport)
				continue;
		}
		rows[count++] = id;
	}

	return row >= 0 && row < count ? rows[row] : NONE;
}

static short forge_zones_menu_row_count(
	void)
{
	short count = 0;

	while (forge_zones_row_kind(count) != NONE)
		count++;

	return count;
}

static void forge_zones_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	struct forge_zone const *zone = forge_zones_has_selection()
		? &forge_zones_globals.zones[forge_zones_globals.selected_zone_index]
		: NULL;

	switch (forge_zones_row_kind(row))
	{
	case _forge_zones_row_show:
		_snprintf(label, label_size, "Show zones");
		_snprintf(value, value_size, "%s", forge_zones_globals.showing ? "on" : "off");
		break;
	case _forge_zones_row_new:
		_snprintf(label, label_size, "New zone (place at the crosshair)");
		_snprintf(value, value_size, "%s", forge_zone_kinds[forge_zones_globals.new_kind].name);
		break;
	case _forge_zones_row_zone:
		_snprintf(label, label_size, "Zone");
		if (zone)
		{
			_snprintf(value, value_size, "%d of %d", forge_zones_globals.selected_zone_index + 1,
				forge_zones_globals.zone_count);
		}
		else
		{
			_snprintf(value, value_size, "%s", forge_zones_globals.zone_count ? "aim at one" : "none yet");
		}
		break;
	case _forge_zones_row_kind:
		_snprintf(label, label_size, "Kind");
		_snprintf(value, value_size, "%s", forge_zone_kinds[zone->kind].name);
		break;
	case _forge_zones_row_shape:
		_snprintf(label, label_size, "Shape");
		_snprintf(value, value_size, "%s", forge_zones_shape_names[zone->shape]);
		break;
	case _forge_zones_row_setting:
		switch (zone->kind)
		{
		case _forge_zone_kill:
			_snprintf(label, label_size, "Kill after");
			_snprintf(value, value_size, "%s", forge_zone_kinds[zone->kind].settings[zone->setting_index].name);
			break;
		case _forge_zone_gravity:
			_snprintf(label, label_size, "Gravity");
			_snprintf(value, value_size, "%s", forge_zone_kinds[zone->kind].settings[zone->setting_index].name);
			break;
		default:
		{
			short count = forge_zones_channel_count(zone->channel);

			_snprintf(label, label_size, "Channel (zones with the same number are linked)");
			_snprintf(value, value_size, "%d (%s)", zone->channel,
				count < 2 ? "no partner yet" : count == 2 ? "linked" : "ring");
			break;
		}
		}
		break;
	case _forge_zones_row_extra:
		_snprintf(label, label_size, "Direction");
		_snprintf(value, value_size, "%s", forge_zones_direction_names[zone->direction]);
		break;
	case _forge_zones_row_width:
		_snprintf(label, label_size, "Width");
		_snprintf(value, value_size, "%.2f", 2.f * zone->half_size.j);
		break;
	case _forge_zones_row_length:
		_snprintf(label, label_size, "Length");
		_snprintf(value, value_size, "%.2f", 2.f * zone->half_size.i);
		break;
	case _forge_zones_row_height:
		_snprintf(label, label_size, "Height");
		_snprintf(value, value_size, "%.2f", 2.f * zone->half_size.k);
		break;
	case _forge_zones_row_step:
		_snprintf(label, label_size, "Resize step");
		_snprintf(value, value_size, "%s", forge_zones_resize_step_names[forge_zones_globals.resize_step_index]);
		break;
	case _forge_zones_row_pick_up:
		_snprintf(label, label_size, "Pick up and move (aim, turn, raise)");
		break;
	case _forge_zones_row_move:
		_snprintf(label, label_size, "Move here (to the crosshair)");
		break;
	case _forge_zones_row_remove:
		_snprintf(label, label_size, "Remove");
		break;
	}

	return;
}

/* (a cycle through count values, direction -1, 0 or 1) */
static short forge_zones_cycle(
	short value,
	short count,
	int direction)
{
	return (short)((value + count + (direction < 0 ? -1 : 1)) % count);
}

static int forge_zones_menu_row_change(
	short row,
	int direction)
{
	struct forge_zone *zone = forge_zones_has_selection()
		? &forge_zones_globals.zones[forge_zones_globals.selected_zone_index]
		: NULL;
	real step = forge_zones_resize_steps[forge_zones_globals.resize_step_index] * (direction < 0 ? -1.f : 1.f);
	int close_menu = FALSE;

	switch (forge_zones_row_kind(row))
	{
	case _forge_zones_row_show:
		forge_zones_globals.showing = !forge_zones_globals.showing;
		break;
	case _forge_zones_row_new:
		if (direction == 0)
		{
			if (forge_zones_local_game())
			{
				forge_zones_new();
				close_menu = TRUE;
			}
		}
		else
		{
			forge_zones_globals.new_kind = forge_zones_cycle(forge_zones_globals.new_kind,
				NUMBER_OF_FORGE_ZONE_KINDS, direction);
		}
		break;
	case _forge_zones_row_zone:
		if (direction != 0 && forge_zones_globals.zone_count > 0)
		{
			forge_zones_globals.selected_zone_index = zone
				? forge_zones_cycle(forge_zones_globals.selected_zone_index, forge_zones_globals.zone_count, direction)
				: 0;
			forge_zones_globals.showing = TRUE;
		}
		break;
	case _forge_zones_row_kind:
		if (direction != 0)
		{
			zone->kind = forge_zones_cycle(zone->kind, NUMBER_OF_FORGE_ZONE_KINDS, direction);
			forge_zones_reset_setting(zone);
			forge_zones_forget_kill_timers();
		}
		break;
	case _forge_zones_row_shape:
		zone->shape = forge_zones_cycle(zone->shape, NUMBER_OF_FORGE_ZONE_SHAPES, direction);
		break;
	case _forge_zones_row_setting:
		if (direction == 0)
			break;
		if (zone->kind == _forge_zone_teleport)
		{
			zone->channel = (short)(1 + forge_zones_cycle((short)(zone->channel - 1),
				FORGE_ZONES_MAXIMUM_CHANNEL, direction));
		}
		else
		{
			short count = forge_zone_kinds[zone->kind].setting_count;

			if (count > 0)
				zone->setting_index = forge_zones_cycle(zone->setting_index, count, direction);
		}
		break;
	case _forge_zones_row_extra:
		if (direction == 0)
			break;
		zone->direction = forge_zones_cycle(zone->direction, NUMBER_OF_FORGE_ZONE_DIRECTIONS, direction);
		break;
	case _forge_zones_row_width:
		if (direction != 0)
			forge_zones_resize(zone, &zone->half_size.j, step);
		break;
	case _forge_zones_row_length:
		if (direction != 0)
			forge_zones_resize(zone, &zone->half_size.i, step);
		break;
	case _forge_zones_row_height:
		if (direction != 0)
			forge_zones_resize(zone, &zone->half_size.k, step);
		break;
	case _forge_zones_row_step:
		forge_zones_globals.resize_step_index = forge_zones_cycle(forge_zones_globals.resize_step_index,
			(short)(sizeof(forge_zones_resize_steps) / sizeof(forge_zones_resize_steps[0])), direction);
		break;
	case _forge_zones_row_pick_up:
		if (direction == 0 && forge_zones_pick_up(forge_zones_globals.selected_zone_index, FALSE))
			close_menu = TRUE;
		break;
	case _forge_zones_row_move:
		if (direction == 0 && forge_zones_local_game())
		{
			if (forge_zones_place_at_crosshair(zone))
				close_menu = TRUE;
			else
				terminal_printf(global_real_argb_orange, "forge_zones: the camera is outside the map");
		}
		break;
	case _forge_zones_row_remove:
		if (direction == 0 && forge_zones_local_game())
			forge_zones_remove(forge_zones_globals.selected_zone_index);
		break;
	}

	return close_menu;
}

/* the tab is shown: the zone the crosshair points into is the one to edit */
static void forge_zones_menu_opened(
	void)
{
	if (local_player_get_player_index(FORGE_ZONES_LOCAL_PLAYER_INDEX) != NONE &&
		halo_mods_authoritative())
	{
		short aimed_zone_index = forge_zones_aimed_zone(NULL);

		if (aimed_zone_index != NONE)
			forge_zones_globals.selected_zone_index = aimed_zone_index;
	}

	return;
}

static struct halo_mod_menu const forge_zones_menu =
{
	"Zones",
	forge_zones_menu_row_count,
	forge_zones_menu_row_text,
	forge_zones_menu_row_change,
	forge_zones_menu_opened
};

/* ---------- the mod */

/* ---------- layouts (port/linux/game/forge_layout.c) */

static void forge_zones_layout_save(
	struct halo_layout_writer *writer)
{
	short index;

	for (index = 0; index < forge_zones_globals.zone_count; index++)
	{
		struct forge_zone const *zone = &forge_zones_globals.zones[index];

		halo_layout_printf(writer, "zone %d %d %.4f %.4f %.4f %.5f %.4f %.4f %.4f %d %d %.5f %.5f %d",
			zone->kind, zone->setting_index, zone->center.x, zone->center.y, zone->center.z, zone->yaw,
			zone->half_size.i, zone->half_size.j, zone->half_size.k, zone->channel, zone->direction,
			zone->pitch, zone->roll, zone->shape);
	}

	return;
}

static void forge_zones_layout_clear(
	void)
{
	forge_zones_new_map();

	return;
}

static void forge_zones_layout_load(
	char const *line)
{
	struct forge_zone zone;
	int kind;
	int setting_index;
	int channel;
	int direction;
	/* (a layout saved before zones had shapes has none: square) */
	int shape = _forge_zone_square;
	/* (a layout saved before zones could tilt has no pitch and roll) */
	float values[9] = { 0 };
	short index;

	if (forge_zones_globals.zone_count < FORGE_ZONES_MAXIMUM_ZONES &&
		sscanf(line, "zone %d %d %f %f %f %f %f %f %f %d %d %f %f %d", &kind, &setting_index,
			&values[0], &values[1], &values[2], &values[3], &values[4], &values[5], &values[6],
			&channel, &direction, &values[7], &values[8], &shape) >= 11 &&
		kind >= 0 && kind < NUMBER_OF_FORGE_ZONE_KINDS)
	{
		/* (a number that is none fails every comparison) */
		for (index = 0; index < NUMBEROF(values); index++)
		{
			if (!(values[index] > -FORGE_ZONES_WORLD_BOUND && values[index] < FORGE_ZONES_WORLD_BOUND))
				return;
		}
		for (index = 4; index <= 6; index++)
			values[index] = PIN(values[index], FORGE_ZONES_MINIMUM_SIZE / 2.f, FORGE_ZONES_MAXIMUM_SIZE / 2.f);
		memset(&zone, 0, sizeof(zone));
		zone.kind = (short)kind;
		zone.setting_index = (short)setting_index;
		zone.center.x = values[0];
		zone.center.y = values[1];
		zone.center.z = values[2];
		zone.yaw = values[3];
		zone.pitch = values[7];
		zone.roll = values[8];
		zone.half_size.i = values[4];
		zone.half_size.j = values[5];
		zone.half_size.k = values[6];
		zone.channel = (short)PIN(channel, 1, FORGE_ZONES_MAXIMUM_CHANNEL);
		zone.direction = (short)PIN(direction, 0, NUMBER_OF_FORGE_ZONE_DIRECTIONS - 1);
		zone.shape = (short)PIN(shape, 0, NUMBER_OF_FORGE_ZONE_SHAPES - 1);
		zone.setting_index = (short)PIN(setting_index, 0, MAX(forge_zone_kinds[kind].setting_count - 1, 0));
		forge_zones_globals.zones[forge_zones_globals.zone_count++] = zone;
	}

	return;
}

struct halo_mod const forge_zones_mod =
{
	"forge_zones",
	forge_zones_update,
	forge_zones_render,
	forge_zones_tick,
	forge_zones_new_map,
	forge_zones_render_world,
	&forge_zones_menu,
	forge_zones_grab,
	NULL,
	forge_zones_layout_save,
	forge_zones_layout_clear,
	forge_zones_layout_load,
	forge_zones_properties
};
