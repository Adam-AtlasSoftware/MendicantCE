/*
CHASE_CAMERA.C

The campaign's third-person camera on foot (camera.view "third_person"):
the view from behind and over the shoulder of the player's body, which is
then drawn whole (the first-person weapon and arms are not), at a distance
the player sets (camera.third_person_distance, changed as they play by
controls.camera_closer and controls.camera_further, and the mouse wheel
while controls.camera_wheel_modifier is held).

Only the view changes. The director follows the player with the following
camera's proc, as it follows a vehicle's driver (director_choose_game_
perspective), but where the seat's track gave that camera's command this
file gives it (chase_camera_command). The deterministic camera the game
aims and fires from (director_camera_deterministic: the seat's, on foot the
eye's) is not changed, nor the player's aim, nor anything saved: the
simulation, a saved game and a demo are the same in either view.

So that the reticle, which stays in the middle of the screen, is where the
shots go, the camera looks from its point over the shoulder through the
point the game's own aim reaches (aim_assist.c's trace from the eye along
the aim, 128 world units, the player left out): the camera, that point over
the shoulder and the target are on one line, and the observer, which keeps
the camera out of walls by shortening its distance along that line
(observer_check_penetration), keeps them so. The point over the shoulder is
found by a sweep from the eye toward it, stopped short of a wall, so that
the camera never looks from the other side of one. The target's distance is
eased (a near wall swept over does not jerk the view), and held at least
CHASE_TARGET_NEAREST out (a target at the muzzle would swing the view
round).

Zoomed with a scope, the view is the first person's until the zoom ends;
in a vehicle's seat, its own; dead, the dead camera's; in a cinematic or a
scripted camera, theirs. Not in multiplayer games unless
camera.third_person_netgames (seeing round corners is an advantage), never
in the VR mode, nor in Forge's.
*/

#include "cseries.h"
#include "chase_camera.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "camera/static_camera.h"
#include "camera/first_person_camera.h"
#include "cutscene/cinematics.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "physics/collision_usage.h"
#include "units/units.h"

#include "player_settings.h"
#include "halo_keyboard.h"
#include "halo_forge.h"

#ifdef HALO_VR
/* port/linux/src/vr.c */
int halo_vr_running(void);
#endif
/* port/linux/src/port_config.c */
int config_write(const char *name, const char *value);
int config_boolean(const char *name);
void platform_log(const char *format, ...);
unsigned long system_milliseconds(void);

/* the distance's range (world units behind the eye) */
#define CHASE_DISTANCE_LEAST 1.0f
#define CHASE_DISTANCE_MOST 6.0f
/* the shoulder's (right of the eye; negative left) */
#define CHASE_SHOULDER_MOST 0.5f
/* how far above the eye the camera looks from */
#define CHASE_HEIGHT 0.06f
/* how far short of a wall the sweep to the shoulder stops */
#define CHASE_WALL_MARGIN 0.06f
/* the target held at least this far out along the aim */
#define CHASE_TARGET_NEAREST 1.5f
/* the game's aim's reach (aim_assist.c's) */
#define CHASE_TARGET_REACH 128.0f
/* the distance's change a second while a key is held, and a notch of the
wheel's (world units) */
#define CHASE_KEY_RATE 2.5f
#define CHASE_WHEEL_STEP 0.25f
/* the target's distance eased this much of the way a second (exponential) */
#define CHASE_TARGET_EASE 12.0f
/* a distance changed by the keys or the wheel written to config.toml this
long after the last change (milliseconds) */
#define CHASE_WRITE_AFTER 1500

static struct chase_camera_globals
{
	struct
	{
		boolean read;
		unsigned long read_at;
		boolean third_person, netgames;
		real distance, shoulder;
	} settings[PLAYER_SETTINGS_SLOTS];
	/* each local player's distance as they play (the setting's, then the
	keys'), the target's eased distance, and the clock of the last call */
	real distance[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	boolean distance_known[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	real target[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	boolean target_known[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	unsigned long last_ms[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	unsigned long logged_ms;
	/* a distance the keys changed, to write (0: none) when */
	unsigned long write_at;
	real write_distance;
} chase;

static real chase_clamp(real value, real least, real most)
{
	return value < least ? least : value > most ? most : value;
}

/* the local player's settings, read again as they change */
static void chase_settings(short local_player_index)
{
	short slot = PLAYER_SETTINGS_SLOT(local_player_index);

	if (!chase.settings[slot].read || chase.settings[slot].read_at != config_changes())
	{
		char const *view = config_player_string(local_player_index, "camera.view");

		chase.settings[slot].read = TRUE;
		chase.settings[slot].read_at = config_changes();
		chase.settings[slot].third_person = view && !strcmp(view, "third_person");
		chase.settings[slot].netgames = config_player_boolean(local_player_index, "camera.third_person_netgames") != 0;
		chase.settings[slot].distance = chase_clamp((real)config_player_real(local_player_index,
			"camera.third_person_distance"), CHASE_DISTANCE_LEAST, CHASE_DISTANCE_MOST);
		chase.settings[slot].shoulder = chase_clamp((real)config_player_real(local_player_index,
			"camera.third_person_shoulder"), -CHASE_SHOULDER_MOST, CHASE_SHOULDER_MOST);
		/* (a setting changed, in the menus or the file: its distance taken
		again, but not the one the keys just wrote) */
		if (local_player_index >= 0 && local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS && !chase.write_at)
			chase.distance_known[local_player_index] = FALSE;
	}
}

static boolean chase_camera_wanted(short local_player_index);

/* (the wheel given back to the weapon switch whenever the first local
player's view is not this camera: asked every frame by the director) */
boolean chase_camera_enabled(short local_player_index)
{
	boolean enabled = chase_camera_wanted(local_player_index);

	if (!enabled && local_player_index == local_player_get_next(NONE))
		halo_camera_wheel_divert(FALSE);
	return enabled;
}

static boolean chase_camera_wanted(short local_player_index)
{
	short slot = PLAYER_SETTINGS_SLOT(local_player_index);
	long player_index, unit_index;
	struct unit_datum *unit;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_NUMBER_OF_LOCAL_PLAYERS)
		return FALSE;
#ifdef HALO_VR
	if (halo_vr_running())
		return FALSE;
#endif
	chase_settings(local_player_index);
	if (!chase.settings[slot].third_person || (game_engine_running() && !chase.settings[slot].netgames) ||
		forge_mode_on() || cinematic_in_progress() || (director_camera_scripted && *director_camera_scripted))
	{
		return FALSE;
	}
	player_index = local_player_get_player_index(local_player_index);
	if (player_index == NONE)
		return FALSE;
	unit_index = player_control_get_unit_index(local_player_index);
	if (unit_index == NONE || player_get(player_index)->unit_index != unit_index)
		return FALSE;
	unit = unit_try_and_get(unit_index);
	return unit && unit->object.type == _object_type_biped && unit->object.parent_object_index == NONE &&
		!TEST_FLAG(unit->object.damage_flags, _object_dead_bit) && unit->unit.current_zoom_level == NONE;
}

/* the keys and the wheel: the distance as the player changes it */
static real chase_distance(short local_player_index, real seconds)
{
	short slot = PLAYER_SETTINGS_SLOT(local_player_index);
	real distance, change = 0.0f;
	boolean modifier;

	if (!chase.distance_known[local_player_index])
	{
		chase.distance[local_player_index] = chase.settings[slot].distance;
		chase.distance_known[local_player_index] = TRUE;
	}
	distance = chase.distance[local_player_index];
	/* (the first local player's keyboard and mouse, as the game's) */
	if (local_player_index == local_player_get_next(NONE))
	{
		if (halo_named_binding_held("controls.camera_closer"))
			change -= CHASE_KEY_RATE * seconds;
		if (halo_named_binding_held("controls.camera_further"))
			change += CHASE_KEY_RATE * seconds;
		modifier = halo_named_binding_held("controls.camera_wheel_modifier") != 0;
		halo_camera_wheel_divert(modifier);
		if (modifier)
			change -= halo_camera_wheel_take() * CHASE_WHEEL_STEP;
	}
	if (change != 0.0f)
	{
		distance = chase_clamp(distance + change, CHASE_DISTANCE_LEAST, CHASE_DISTANCE_MOST);
		chase.write_at = system_milliseconds() + CHASE_WRITE_AFTER;
		if (!chase.write_at)
			chase.write_at = 1;
		chase.write_distance = distance;
	}
	if (chase.write_at && (long)(system_milliseconds() - chase.write_at) >= 0)
	{
		char text[32];

		snprintf(text, sizeof(text), "%.2f", (double)chase.write_distance);
		config_write("camera.third_person_distance", text);
		chase.write_at = 0;
	}
	chase.distance[local_player_index] = distance;
	return distance;
}

boolean chase_camera_command(short local_player_index, struct camera_command *command)
{
	short slot = PLAYER_SETTINGS_SLOT(local_player_index);
	long unit_index;
	real_point3d eye, focus, target;
	real_vector3d aim, right, sweep, toward;
	real_vector2d heading;
	real_euler_angles2d facing;
	real shoulder, seconds, reach, length;
	unsigned long now;
	struct collision_result collision;

	/* (a local player the arrays have no slot for: never the third person,
	and nothing of it written) */
	if (local_player_index < 0 || local_player_index >= MAXIMUM_NUMBER_OF_LOCAL_PLAYERS)
		return FALSE;
	if (!chase_camera_enabled(local_player_index))
	{
		chase.target_known[local_player_index] = FALSE;
		return FALSE;
	}
	unit_index = player_control_get_unit_index(local_player_index);
	now = system_milliseconds();
	seconds = chase.last_ms[local_player_index] ? (real)(now - chase.last_ms[local_player_index]) * 0.001f : 0.0f;
	seconds = chase_clamp(seconds, 0.0f, 0.1f);
	chase.last_ms[local_player_index] = now;

	/* the eye, as the game aims from it, and the view's aim (the player's
	facing now, which the unit's aim follows) */
	first_person_camera_deterministic(unit_index, &eye, &aim);
	facing = *player_control_get_facing_angles(local_player_index);
	facing.pitch = chase_clamp(facing.pitch, -_pi / 2.f + 0.01f, _pi / 2.f - 0.01f);
	vector3d_from_euler_angles2d(&aim, &facing);

	/* the shoulder, swept from the eye */
	heading.i = aim.i;
	heading.j = aim.j;
	if (normalize2d(&heading) == 0.0f)
	{
		heading.i = 1.0f;
		heading.j = 0.0f;
	}
	right.i = heading.j;
	right.j = -heading.i;
	right.k = 0.0f;
	shoulder = chase.settings[slot].shoulder;
	sweep.i = right.i * (shoulder + (shoulder < 0.0f ? -CHASE_WALL_MARGIN : CHASE_WALL_MARGIN));
	sweep.j = right.j * (shoulder + (shoulder < 0.0f ? -CHASE_WALL_MARGIN : CHASE_WALL_MARGIN));
	sweep.k = CHASE_HEIGHT + CHASE_WALL_MARGIN;
	if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
			FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) |
			FLAG(_collision_test_objects_vehicles_bit),
			&eye, &sweep, unit_index, &collision))
	{
		real kept = collision.t * magnitude3d(&sweep) - CHASE_WALL_MARGIN;
		real whole = magnitude3d(&sweep) - CHASE_WALL_MARGIN;
		real share = whole > 0.0f ? chase_clamp(kept / whole, 0.0f, 1.0f) : 0.0f;

		shoulder *= share;
		focus.x = eye.x + right.i * shoulder;
		focus.y = eye.y + right.j * shoulder;
		focus.z = eye.z + CHASE_HEIGHT * share;
	}
	else
	{
		focus.x = eye.x + right.i * shoulder;
		focus.y = eye.y + right.j * shoulder;
		focus.z = eye.z + CHASE_HEIGHT;
	}

	/* the target: where the game's aim reaches from the eye, eased */
	{
		real_vector3d ray;

		scale_vector3d(&aim, CHASE_TARGET_REACH, &ray);
		reach = CHASE_TARGET_REACH;
		if (collision_test_vector(_collision_test_for_projectiles_flags, &eye, &ray, unit_index, &collision))
			reach = collision.t * CHASE_TARGET_REACH;
		reach = chase_clamp(reach, CHASE_TARGET_NEAREST, CHASE_TARGET_REACH);
		if (!chase.target_known[local_player_index] || seconds <= 0.0f)
			chase.target[local_player_index] = reach;
		else
		{
			/* (eased in the logarithm: as quickly far as near) */
			real k = 1.0f - (real)exp(-CHASE_TARGET_EASE * seconds);
			real eased = (real)log(chase.target[local_player_index]) * (1.0f - k) + (real)log(reach) * k;

			chase.target[local_player_index] = (real)exp(eased);
		}
		chase.target_known[local_player_index] = TRUE;
		reach = chase.target[local_player_index];
		target.x = eye.x + aim.i * reach;
		target.y = eye.y + aim.j * reach;
		target.z = eye.z + aim.k * reach;
	}
	toward.i = target.x - focus.x;
	toward.j = target.y - focus.y;
	toward.k = target.z - focus.z;
	length = normalize3d(&toward);
	if (length <= 0.0f)
		toward = aim;

	/* the command: the observer's focus the eye, its offset (forward, right,
	up along the view's heading) the shoulder's, the distance back of that
	along forward */
	command->position = eye;
	heading.i = toward.i;
	heading.j = toward.j;
	if (normalize2d(&heading) == 0.0f)
	{
		heading.i = 1.0f;
		heading.j = 0.0f;
	}
	{
		real dx = focus.x - eye.x, dy = focus.y - eye.y;

		command->offset.i = dx * heading.i + dy * heading.j;
		command->offset.j = dx * heading.j - dy * heading.i;
		command->offset.k = focus.z - eye.z;
	}
	command->depth = chase_distance(local_player_index, seconds);
	/* (the walls along the camera's line from the shoulder point: the
	observer's own test leaves out vehicles (a Pelican hovering over the
	player, which the camera went up through), so it is made here with them
	too, the player left out, and the distance stopped short of what it
	hits; the observer then tests the structure again as ever) */
	{
		real_vector3d back;

		scale_vector3d(&toward, -(command->depth + CHASE_WALL_MARGIN), &back);
		if (collision_test_vector(FLAG(_collision_test_structure_bit) |
				FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_objects_bit) |
				FLAG(_collision_test_objects_scenery_bit) | FLAG(_collision_test_objects_vehicles_bit) |
				FLAG(_collision_test_objects_machines_bit),
				&focus, &back, unit_index, &collision))
		{
			real kept = collision.t * (command->depth + CHASE_WALL_MARGIN) - CHASE_WALL_MARGIN;

			command->depth = kept > 0.0f ? kept : 0.0f;
		}
	}
	/* (debug.camera_log: the last frame's camera, as the observer left it
	after the walls, once a second) */
	if (config_boolean("debug.camera_log") && now - chase.logged_ms >= 1000)
	{
		struct observer_result const *drawn = observer_get_camera(local_player_index);
		real dx = drawn->position.x - focus.x, dy = drawn->position.y - focus.y, dz = drawn->position.z - focus.z;

		chase.logged_ms = now;
		platform_log("chase camera: distance %.2f asked, %.2f from the shoulder point after the walls; shoulder %.2f "
			"of %.2f; target %.1f out", (double)command->depth, (double)square_root(dx * dx + dy * dy + dz * dz),
			(double)shoulder, (double)chase.settings[slot].shoulder, (double)reach);
	}
	command->forward = toward;
	observer_up_from_forward(&command->forward, &command->up);
	object_get_velocities(unit_index, &command->velocity, NULL);
	SET_FLAG(command->flags, 0, TRUE);
	return TRUE;
}
