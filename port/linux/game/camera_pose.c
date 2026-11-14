/*
CAMERA_POSE.C

The camera put at an exact place and facing from the console
(debug_camera_set), for pictures of a map made from numbers: the script's
camera (camera_control's), set with no transition, the observer cleared
first so that it takes the camera at once as its first command (in the step
the script's camera takes on being set), so the next frame is drawn from
there, also with the game's time stopped (game_speed 0), when the observer
takes no steps of its own. The first-person weapon is not drawn from the
script's camera; the HUD is show_hud's.
*/

#include "cseries.h"
#include "camera_pose.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "main/console.h"
#include "objects/objects.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"
#include "game/game.h"
#include "game/players.h"
#include "ai/ai_scenario_definitions.h"
#include "cache/cache_files.h"
#include "tag_files/tag_files.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "hs/hs.h"
#include "cutscene/cinematics.h"
#include "prism_popin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator: config_file_read's buffer) */
#undef free

void scripted_camera_set_camera_point_absolute(real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real field_of_view, word transition_time);

#define DEGREES_TO_RADIANS (3.14159265358979f / 180.0f)

/* the script's camera at a pose: world units and degrees */
static void camera_pose_put(float x, float y, float z, float yaw, float pitch, float roll, float field_of_view)
{
	float cy, sy, cp, sp, cr, sr;
	real_point3d position;
	real_vector3d forward, level_up, right, up;

	cy = cosf(yaw * DEGREES_TO_RADIANS);
	sy = sinf(yaw * DEGREES_TO_RADIANS);
	cp = cosf(pitch * DEGREES_TO_RADIANS);
	sp = sinf(pitch * DEGREES_TO_RADIANS);
	cr = cosf(roll * DEGREES_TO_RADIANS);
	sr = sinf(roll * DEGREES_TO_RADIANS);
	position.x = x;
	position.y = y;
	position.z = z;
	forward.i = cy * cp;
	forward.j = sy * cp;
	forward.k = sp;
	/* (up before the roll, and right: forward cross it) */
	level_up.i = -cy * sp;
	level_up.j = -sy * sp;
	level_up.k = cp;
	right.i = forward.j * level_up.k - forward.k * level_up.j;
	right.j = forward.k * level_up.i - forward.i * level_up.k;
	right.k = forward.i * level_up.j - forward.j * level_up.i;
	up.i = cr * level_up.i + sr * right.i;
	up.j = cr * level_up.j + sr * right.j;
	up.k = cr * level_up.k + sr * right.k;
	director_script_camera(TRUE);
	observer_obsolete_position(0);
	scripted_camera_set_camera_point_absolute(&position, &forward, &up, field_of_view * DEGREES_TO_RADIANS, 0);
}

void camera_pose_set(char const *pose)
{
	float x, y, z, yaw, pitch, roll, field_of_view;

	if (pose && strcmp(pose, "off") == 0)
	{
		director_script_camera(FALSE);
		return;
	}
	if (!pose || sscanf(pose, "%f %f %f %f %f %f %f", &x, &y, &z, &yaw, &pitch, &roll, &field_of_view) != 7 ||
		field_of_view <= 0.0f || field_of_view >= 90.0f)
	{
		/* (the script's camera halts the game past 90 degrees: camera_scripting.c) */
		console_printf(FALSE, "debug_camera_set \"<x> <y> <z> <yaw> <pitch> <roll> <field of view under 90>\" (degrees), or \"off\"");
		return;
	}
	camera_pose_put(x, y, z, yaw, pitch, roll, field_of_view);
}

/* ---------- a path of poses (debug.camera_path)

A file of the camera's poses at frames, for a test that drives the camera
the same way each run: a frame-numbered path, not a timed one, so that two
builds draw the same poses in the same frames whatever their frame rates.

  start <frames>     the game's frames drawn after its clock's first tick
                     on the map (and after a cinematic:
                     debug.skip_cinematics skips them) before the path
                     starts (default 0)
  start_tick <ticks> the path starts at the first frame once the game's
                     clock has run that many ticks on the map (and no
                     cinematic plays): the same tick every run, which a
                     count of frames is not where a frame need not take a
                     step of the clock
  cmd <command>      a console command run as the path starts (show_hud 0),
                     in order
  at <frame> <command>
                     one run at a frame of the path (game_speed 0 a few
                     frames in: the director takes the script's camera in
                     a step of the game's)
  tick <ticks> <command>
                     one run at the first frame once the game's clock has
                     run that many ticks since the path started (a frame
                     need not take a step of the clock: game_speed 0 by
                     the clock stops the game at the same tick every run)
  key <frame> <x> <y> <z> <yaw> <pitch> <roll> <field of view>
                     a pose at a frame of the path (frames in order; world
                     units, degrees)
  save <frame>       a frame of the path saved (frames in order); with none,
                     every frame is

The poses between keys follow a Catmull-Rom spline through them (their
angles too, the yaw's the shorter way round), so that the camera's speed
changes smoothly. While the path runs, each frame (or each named by a save
line) is saved as
debug.screenshot_every saves one (camera_path_capturing: to
debug.screenshot_stream or _directory); at its end the camera stays, and
the log says so.

With debug.camera_path_settle, a frame to be saved is drawn with every
texture the stock cache has to read (camera_path_settling) and held: its pose
drawn again, the path not moved on and nothing saved, until the host has
said on frames in flight + 1 frames after its first that its PRISM texture
and mesh stores have nothing to come of what they drew (the ring's
streams_pending, read here as each frame begins: the frames in flight
before it may not have reached the host), or for at most the setting's
frames. A held frame runs no "at" line again; "tick" lines go by the game's
clock, which a stopped game (game_speed 0) does not move, nor then the
picture: the rendering's animations follow that clock (main.c), and each
held frame is drawn with the frames' seed its first had. */

/* (enough for a key every frame of a long capture's moves, and for the
commands of the quality gates' captures: tools/prism_gate) */
#define MAXIMUM_PATH_KEYS 8192
#define MAXIMUM_PATH_COMMANDS 128
#define MAXIMUM_PATH_SAVES 8192

struct camera_path_key
{
	long frame;
	float pose[7];
};

static struct
{
	boolean read, running, done;
	long start_frames, frames_seen, frame, start_ticks;
	long key_count, command_count;
	struct camera_path_key keys[MAXIMUM_PATH_KEYS];
	long command_frames[MAXIMUM_PATH_COMMANDS];
	/* (a tick line's: its ticks, run once; the clock as the path started) */
	boolean command_by_tick[MAXIMUM_PATH_COMMANDS], command_run[MAXIMUM_PATH_COMMANDS];
	long start_tick;
	char commands[MAXIMUM_PATH_COMMANDS][128];
	/* the frames saved (none: every one), in order, and the next to come */
	long save_count, save_next;
	long saves[MAXIMUM_PATH_SAVES];
	/* debug.camera_path_settle (camera_path_read's): this frame saved, or held
	(drawn again next frame); the frames it has been held, those of them
	whose answer had nothing to come, in a row, the frames' seed as it was
	first drawn, and the frames held in all */
	long settle_most;
	boolean settle_save, settle_hold;
	long held, settled, held_total;
	unsigned long held_seed;
} camera_path;

/* the platform's (port/linux/src/port_config.c) */
char const *config_string(char const *name);
long config_integer(char const *name);
char *config_file_read(char const *path, size_t *size);
void platform_log(const char *format, ...);
/* the host's (port/linux/src/xgpu_stream_encode.c) */
unsigned long xgpu_backend_streams_pending(unsigned long *frames_in_flight);

static void camera_path_read(void)
{
	char const *path = config_string("debug.camera_path");
	char line[256];
	char *text, *next;
	size_t size = 0;

	camera_path.read = TRUE;
	camera_path.settle_most = config_integer("debug.camera_path_settle");
	if (!path || !*path)
	{
		return;
	}
	text = config_file_read(path, &size);
	if (!text)
	{
		platform_log("camera path: cannot read %s", path);
		return;
	}
	for (next = text; next < text + size && *next;)
	{
		struct camera_path_key key;
		char *end = next;
		size_t length;

		while (end < text + size && *end && *end != '\n')
		{
			end++;
		}
		length = (size_t)(end - next);
		if (length >= sizeof(line))
		{
			length = sizeof(line) - 1;
		}
		memcpy(line, next, length);
		line[length] = 0;
		next = end < text + size && *end ? end + 1 : end;
		while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
		{
			line[--length] = 0;
		}
		if (strncmp(line, "start ", 6) == 0)
		{
			camera_path.start_frames = atol(line + 6);
		}
		else if (strncmp(line, "start_tick ", 11) == 0)
		{
			camera_path.start_ticks = atol(line + 11);
		}
		else if (strncmp(line, "cmd ", 4) == 0 && camera_path.command_count < MAXIMUM_PATH_COMMANDS &&
			length - 4 < sizeof(camera_path.commands[0]))
		{
			camera_path.command_frames[camera_path.command_count] = 0;
			strcpy(camera_path.commands[camera_path.command_count++], line + 4);
		}
		else if ((strncmp(line, "at ", 3) == 0 || strncmp(line, "tick ", 5) == 0) &&
			camera_path.command_count < MAXIMUM_PATH_COMMANDS)
		{
			char *command = strchr(line + (line[0] == 't' ? 5 : 3), ' ');

			if (command && strlen(command + 1) < sizeof(camera_path.commands[0]))
			{
				camera_path.command_by_tick[camera_path.command_count] = line[0] == 't';
				camera_path.command_frames[camera_path.command_count] = atol(line + (line[0] == 't' ? 5 : 3));
				strcpy(camera_path.commands[camera_path.command_count++], command + 1);
			}
		}
		else if (strncmp(line, "save ", 5) == 0 && camera_path.save_count < MAXIMUM_PATH_SAVES &&
			(!camera_path.save_count || atol(line + 5) > camera_path.saves[camera_path.save_count - 1]))
		{
			camera_path.saves[camera_path.save_count++] = atol(line + 5);
		}
		else if (sscanf(line, "key %ld %f %f %f %f %f %f %f", &key.frame, &key.pose[0], &key.pose[1], &key.pose[2],
			&key.pose[3], &key.pose[4], &key.pose[5], &key.pose[6]) == 8 && camera_path.key_count < MAXIMUM_PATH_KEYS &&
			key.pose[6] > 0.0f && key.pose[6] < 90.0f &&
			(!camera_path.key_count || key.frame > camera_path.keys[camera_path.key_count - 1].frame))
		{
			if (camera_path.key_count)
			{
				/* (the yaw the shorter way round from the key before) */
				float before = camera_path.keys[camera_path.key_count - 1].pose[3];

				while (key.pose[3] - before > 180.0f)
				{
					key.pose[3] -= 360.0f;
				}
				while (key.pose[3] - before < -180.0f)
				{
					key.pose[3] += 360.0f;
				}
			}
			camera_path.keys[camera_path.key_count++] = key;
		}
	}
	free(text);
	platform_log("camera path: %ld keys over %ld frames from %s, %ld frames after the map's first",
		camera_path.key_count, camera_path.key_count ? camera_path.keys[camera_path.key_count - 1].frame : 0L, path,
		camera_path.start_frames);
}

/* the pose at a frame of the path: a Catmull-Rom spline through the keys */
static void camera_path_pose(long frame, float pose[7])
{
	long count = camera_path.key_count, k = 0;
	struct camera_path_key const *p0, *p1, *p2, *p3;
	float t, t2, t3;
	short i;

	while (k + 1 < count && camera_path.keys[k + 1].frame <= frame)
	{
		k++;
	}
	p1 = &camera_path.keys[k];
	if (k + 1 >= count)
	{
		memcpy(pose, p1->pose, sizeof(p1->pose));
		return;
	}
	p2 = &camera_path.keys[k + 1];
	p0 = k > 0 ? &camera_path.keys[k - 1] : p1;
	p3 = k + 2 < count ? &camera_path.keys[k + 2] : p2;
	t = (float)(frame - p1->frame) / (float)(p2->frame - p1->frame);
	t2 = t * t;
	t3 = t2 * t;
	for (i = 0; i < 7; i++)
	{
		pose[i] = 0.5f * (2.0f * p1->pose[i] + (p2->pose[i] - p0->pose[i]) * t +
			(2.0f * p0->pose[i] - 5.0f * p1->pose[i] + 4.0f * p2->pose[i] - p3->pose[i]) * t2 +
			(3.0f * p1->pose[i] - p0->pose[i] - 3.0f * p2->pose[i] + p3->pose[i]) * t3);
	}
}

/* the frame of the path is one it saves */
static int camera_path_saves_frame(void)
{
	if (!camera_path.save_count)
		return 1;
	while (camera_path.save_next < camera_path.save_count &&
		camera_path.saves[camera_path.save_next] < camera_path.frame)
		camera_path.save_next++;
	return camera_path.save_next < camera_path.save_count &&
		camera_path.saves[camera_path.save_next] == camera_path.frame;
}

/* debug.camera_path_settle: this frame, one to be saved, saved or held
(the path's comment above) */
static void camera_path_settle(void)
{
	unsigned long pending, frames_in_flight;

	camera_path.settle_save = camera_path.settle_hold = FALSE;
	if (camera_path.settle_most <= 0 || !camera_path_saves_frame())
	{
		camera_path.held = 0;
		return;
	}
	pending = xgpu_backend_streams_pending(&frames_in_flight);
	/* (the first frame of the pose: the answer is of the frames before it) */
	if (!camera_path.held)
	{
		camera_path.settled = 0;
		camera_path.held_seed = random_math_port_get_frame_seed();
	}
	else
	{
		camera_path.settled = pending ? 0 : camera_path.settled + 1;
		random_math_port_set_frame_seed(camera_path.held_seed);
	}
	if (camera_path.settled > (long)frames_in_flight || camera_path.held >= camera_path.settle_most)
	{
		if (camera_path.settled <= (long)frames_in_flight)
			platform_log("camera path: frame %ld saved after %ld frames held, the most (debug.camera_path_settle), "
				"%lu textures and meshes still to come", camera_path.frame, camera_path.held, pending);
		else if (camera_path.held > (long)frames_in_flight + 1)
			platform_log("camera path: frame %ld saved after %ld frames held for the streams", camera_path.frame,
				camera_path.held);
		camera_path.settle_save = TRUE;
		camera_path.held = 0;
		return;
	}
	camera_path.settle_hold = TRUE;
	camera_path.held++;
	camera_path.held_total++;
}

void camera_path_frame(void)
{
	float pose[7];
	long i;
	/* (the frame held the frame before: drawn again) */
	boolean held = camera_path.settle_hold;

	if (!camera_path.read)
	{
		camera_path_read();
	}
	if (!camera_path.key_count || camera_path.done)
	{
		return;
	}
	/* (counted from the game's first tick, not its first frame with the map:
	the frames drawn before its clock starts are as many as the loading
	takes, and a command at a frame of the path would fall in another tick
	in each run) */
	if (!global_scenario_try_and_get() || (!camera_path.running && (cinematic_in_progress() || game_time_get() <= 0)))
	{
		camera_path.frames_seen = 0;
		return;
	}
	if (camera_path.frames_seen++ < camera_path.start_frames ||
		(!camera_path.running && game_time_get() < camera_path.start_ticks))
	{
		return;
	}
	if (!camera_path.running)
	{
		camera_path.running = TRUE;
		camera_path.frame = 0;
		/* (the frames' randomness from here the same in every run: the
		loading's frames took it as many times as the loading took) */
		random_math_port_set_frame_seed(0x70617468UL);
		camera_path.start_tick = game_time_get();
		platform_log("camera path: started");
	}
	else if (!held && ++camera_path.frame > camera_path.keys[camera_path.key_count - 1].frame)
	{
		unsigned long skipped, waited, world_switches;
		double waited_milliseconds;

		camera_path.done = TRUE;
		prism_popin_counts(&skipped, &waited, &world_switches, &waited_milliseconds);
		platform_log("camera path: done after %ld frames (%ld more held for the streams); since the map: %lu draws "
			"left out for a texture not in, %lu waited for (%.1f ms), %lu remodel surfaces switched near or far",
			camera_path.frame, camera_path.held_total, skipped, waited, waited_milliseconds, world_switches);
		return;
	}
	for (i = 0; i < camera_path.command_count; i++)
	{
		if (camera_path.command_by_tick[i] ? !camera_path.command_run[i] &&
			game_time_get() - camera_path.start_tick >= camera_path.command_frames[i] :
			camera_path.command_frames[i] == camera_path.frame && !held)
		{
			camera_path.command_run[i] = TRUE;
			hs_compile_and_evaluate(camera_path.commands[i]);
		}
	}
	camera_path_pose(camera_path.frame, pose);
	camera_pose_put(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
	camera_path_settle();
}

int camera_path_capturing(void)
{
	if (!camera_path.running || camera_path.done)
		return 0;
	if (camera_path.settle_most > 0)
		return camera_path.settle_save;
	return camera_path_saves_frame();
}

int camera_path_settling(void)
{
	return camera_path.running && !camera_path.done && (camera_path.settle_save || camera_path.settle_hold);
}

/* ---------- objects placed for the picture (debug_object_place) */

#define MAXIMUM_PLACED_OBJECTS 16

static long placed_objects[MAXIMUM_PLACED_OBJECTS];
static short placed_object_count;

/* the loaded tag of that name, in the first of the object groups that has it */
static long camera_pose_object_tag(char const *name)
{
	static long const groups[] = { 'vehi', 'bipd', 'weap', 'eqip', 'scen', 'mach', 'ctrl', 'lifi', 'plac', 'ssce', 'garb' };
	short i;

	for (i = 0; i < (short)NUMBEROF(groups); i++)
	{
		long tag_index = tag_loaded(groups[i], name);

		if (tag_index != NONE)
		{
			return tag_index;
		}
	}
	return NONE;
}

void camera_pose_place_object(char const *line)
{
	char name[256], seat[64];
	float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	float r, g, b;
	char const *color;
	long tag_index, object_index;
	struct object_placement_data data;

	if (line && strcmp(line, "clear") == 0)
	{
		short i;

		for (i = 0; i < placed_object_count; i++)
		{
			if (object_header_try_and_get(placed_objects[i]))
			{
				object_delete(placed_objects[i]);
			}
		}
		placed_object_count = 0;
		return;
	}
	seat[0] = 0;
	name[0] = 0;
	/* ("player pose": the first player's unit's place and facing written to
	the log as a camera path's pose, its eye 0.6 world units above its
	origin: the quality gates' poses on a map with no scene of its own) */
	if (line && !strcmp(line, "player pose"))
	{
		long player_index = data_next_index(player_data, NONE);
		struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
		real_point3d position;
		real_vector3d forward, up;

		if (!player || player->unit_index == NONE || !object_header_try_and_get(player->unit_index))
		{
			platform_log("player pose: none");
			return;
		}
		object_get_origin(player->unit_index, &position);
		object_get_orientation(player->unit_index, &forward, &up);
		platform_log("player pose: %.3f %.3f %.3f %.2f 0 0 70", position.x, position.y, position.z + 0.6f,
			atan2f(forward.j, forward.i) * 57.29578f);
		return;
	}
	/* ("player at <x> <y> <z> <yaw>": the first player's unit stood there,
facing so: the quality gates' scenes of the player's own light, its
flashlight where the camera is) */
	if (line && !strncmp(line, "player at ", 10))
	{
		long player_index = data_next_index(player_data, NONE);
		struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
		real_point3d position;
		real_vector3d forward, up;
		float px, py, pz, pyaw;

		if (!player || player->unit_index == NONE || !object_header_try_and_get(player->unit_index) ||
			sscanf(line + 10, "%f %f %f %f", &px, &py, &pz, &pyaw) != 4)
		{
			console_printf(FALSE, "debug_object_place: \"player at <x> <y> <z> <yaw>\", with a player's unit");
			return;
		}
		position.x = px;
		position.y = py;
		position.z = pz;
		forward.i = cosf(pyaw * DEGREES_TO_RADIANS);
		forward.j = sinf(pyaw * DEGREES_TO_RADIANS);
		forward.k = 0.0f;
		up.i = up.j = 0.0f;
		up.k = 1.0f;
		object_set_position(player->unit_index, &position, &forward, &up);
		platform_log("player at: %.3f %.3f %.3f %.2f", px, py, pz, pyaw);
		return;
	}
	/* ("list <words>": every object whose tag's name has those words in it,
	its place and bounding radius, written to the log: the quality gates'
	poses about a map's scenery) */
	if (line && !strncmp(line, "list ", 5))
	{
		struct object_iterator iterator;
		struct object_datum *object;

		object_iterator_new(&iterator, _object_mask_all, 0);
		while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
		{
			char const *tag_name = tag_get_name(object->definition_index);

			if (tag_name && strstr(tag_name, line + 5))
				platform_log("object list: %s at %.3f %.3f %.3f radius %.3f", tag_name, object->object.position.x,
					object->object.position.y, object->object.position.z, object->object.bounding_sphere_radius);
		}
		return;
	}
	if (line)
	{
		/* (the tag's name, which may have spaces in it ("scenery\plants\plant
		fern\plant fern"), ends at " seat " or at a space before a number) */
		char const *end = line;
		size_t length;

		while (*end && !(end[0] == ' ' && (strncmp(end, " seat ", 6) == 0 ||
			(end[1] >= '0' && end[1] <= '9') || end[1] == '-' || end[1] == '+' || end[1] == '.')))
		{
			end++;
		}
		length = (size_t)(end - line);
		if (length >= sizeof(name))
			length = 0;
		memcpy(name, line, length);
		name[length] = 0;
		line = end;
	}
	if (!line || !name[0] ||
		(sscanf(line, " seat %63s", seat) != 1 &&
		 sscanf(line, "%f %f %f %f %f %f", &x, &y, &z, &yaw, &pitch, &roll) < 4))
	{
		console_printf(FALSE, "debug_object_place \"<tag> <x> <y> <z> <yaw> [<pitch> <roll>] [color <r> <g> <b>]\", \"<tag> seat <label>\", "
			"\"player seat <label>\", \"player at <x> <y> <z> <yaw>\", or \"clear\"");
		return;
	}
	/* ("player seat <label>": the first player's own unit put in the last
	placed vehicle's seat, so that a scripted input (debug.test_input) drives
	it: the quality gates' moving vehicle) */
	if (!strcmp(name, "player") && seat[0])
	{
		long player_index = data_next_index(player_data, NONE);
		struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;

		if (!player || player->unit_index == NONE || placed_object_count == 0 ||
			!object_header_try_and_get(placed_objects[placed_object_count - 1]))
		{
			console_printf(FALSE, "debug_object_place: no player's unit, or no vehicle placed for the seat");
			return;
		}
		unit_scripting_enter_vehicle(player->unit_index, placed_objects[placed_object_count - 1], seat);
		return;
	}
	tag_index = camera_pose_object_tag(name);
	if (tag_index == NONE)
	{
		struct tag_iterator iterator;
		long other;

		/* (the map's vehicles and bipeds named, to choose from) */
		console_printf(FALSE, "debug_object_place: no object tag %s in this map", name);
		tag_iterator_new(&iterator, 'vehi');
		while ((other = tag_iterator_next(&iterator)) != NONE)
		{
			console_printf(FALSE, "  vehicle %s", tag_get_name(other));
		}
		tag_iterator_new(&iterator, 'bipd');
		while ((other = tag_iterator_next(&iterator)) != NONE)
		{
			console_printf(FALSE, "  biped %s", tag_get_name(other));
		}
		return;
	}
	if (placed_object_count >= MAXIMUM_PLACED_OBJECTS)
	{
		console_printf(FALSE, "debug_object_place: no more than %d objects", MAXIMUM_PLACED_OBJECTS);
		return;
	}
	object_placement_data_new(&data, tag_index, NONE);
	if (seat[0])
	{
		/* (made at the last placed vehicle, then put in its seat) */
		if (placed_object_count == 0 || !object_header_try_and_get(placed_objects[placed_object_count - 1]))
		{
			console_printf(FALSE, "debug_object_place: no vehicle placed for the seat");
			return;
		}
		object_get_origin(placed_objects[placed_object_count - 1], &data.position);
		data.position.z += 1.0f;
	}
	else
	{
		float cy = cosf(yaw * DEGREES_TO_RADIANS), sy = sinf(yaw * DEGREES_TO_RADIANS);
		float cp = cosf(pitch * DEGREES_TO_RADIANS), sp = sinf(pitch * DEGREES_TO_RADIANS);
		float cr = cosf(roll * DEGREES_TO_RADIANS), sr = sinf(roll * DEGREES_TO_RADIANS);
		real_vector3d level_up, right;

		data.position.x = x;
		data.position.y = y;
		data.position.z = z;
		data.forward.i = cy * cp;
		data.forward.j = sy * cp;
		data.forward.k = sp;
		level_up.i = -cy * sp;
		level_up.j = -sy * sp;
		level_up.k = cp;
		right.i = data.forward.j * level_up.k - data.forward.k * level_up.j;
		right.j = data.forward.k * level_up.i - data.forward.i * level_up.k;
		right.k = data.forward.i * level_up.j - data.forward.j * level_up.i;
		data.up.i = cr * level_up.i + sr * right.i;
		data.up.j = cr * level_up.j + sr * right.j;
		data.up.k = cr * level_up.k + sr * right.k;
	}
	color = strstr(line, " color ");
	if (color && sscanf(color, " color %f %f %f", &r, &g, &b) == 3)
	{
		short i;

		for (i = 0; i < NUMBER_OF_OBJECT_CHANGE_COLORS; i++)
		{
			data.change_colors[i].red = r;
			data.change_colors[i].green = g;
			data.change_colors[i].blue = b;
		}
	}
	object_index = object_new(&data);
	if (object_index == NONE)
	{
		console_printf(FALSE, "debug_object_place: %s was not made", name);
		return;
	}
	if (seat[0])
	{
		unit_scripting_enter_vehicle(object_index, placed_objects[placed_object_count - 1], seat);
	}
	placed_objects[placed_object_count++] = object_index;
}

/* ---------- the scenario's places in the loaded structure (debug_scenario_places) */

static void camera_pose_write_place(FILE *file, char const *kind, real_point3d const *position,
	float yaw, float pitch, float roll, float field_of_view)
{
	struct location location;

	scenario_location_from_point(&location, position);
	if (location.cluster_index != NONE)
	{
		fprintf(file, "%s %.3f %.3f %.3f %.2f %.2f %.2f %.2f\n", kind, position->x, position->y, position->z,
			yaw / DEGREES_TO_RADIANS, pitch / DEGREES_TO_RADIANS, roll / DEGREES_TO_RADIANS,
			field_of_view / DEGREES_TO_RADIANS);
	}
}

/* a cutscene's camera, also outside the structure (a cinematic's view from
the sky): "camera_outside" there */
static void camera_pose_write_camera(FILE *file, struct scenario_cutscene_camera_point const *point)
{
	struct location location;

	scenario_location_from_point(&location, &point->position);
	fprintf(file, "%s %.3f %.3f %.3f %.2f %.2f %.2f %.2f %s\n",
		location.cluster_index != NONE ? "camera" : "camera_outside",
		point->position.x, point->position.y, point->position.z,
		point->orientation.yaw / DEGREES_TO_RADIANS, point->orientation.pitch / DEGREES_TO_RADIANS,
		point->orientation.roll / DEGREES_TO_RADIANS, point->field_of_view / DEGREES_TO_RADIANS,
		point->name[0] ? point->name : "-");
}

/* the scenario's placed objects of one kind (their blocks' element sizes:
the object's placement, then the kind's own fields), with their tags' names:
"object <x> <y> <z> <yaw> <pitch> <roll> 0 <tag>" */
static void camera_pose_write_objects(FILE *file, struct tag_block const *objects, long element_size,
	struct tag_block const *palette)
{
	long i;

	for (i = 0; i < objects->count && objects->address; i++)
	{
		struct scenario_object_datum const *object =
			(struct scenario_object_datum const *)((byte const *)objects->address + i * element_size);
		char const *name = "-";

		if (object->palette_entry_index >= 0 && object->palette_entry_index < palette->count && palette->address)
		{
			struct scenario_object_palette_entry const *entry = (struct scenario_object_palette_entry const *)
				palette->address + object->palette_entry_index;

			if (entry->reference.index != NONE)
			{
				name = tag_get_name(entry->reference.index);
			}
		}
		fprintf(file, "object %.3f %.3f %.3f %.2f %.2f %.2f 0 %s\n", object->position.x, object->position.y,
			object->position.z, object->rotation.yaw / DEGREES_TO_RADIANS,
			object->rotation.pitch / DEGREES_TO_RADIANS, object->rotation.roll / DEGREES_TO_RADIANS, name);
	}
}

void camera_pose_write_places(char const *path)
{
	struct scenario *scenario = global_scenario_get();
	FILE *file;
	long i, j, k;

	if (!scenario || !path || !*path || !(file = fopen(path, "w")))
	{
		console_printf(FALSE, "debug_scenario_places \"<file>\": no scenario, or the file not written");
		return;
	}
	fprintf(file, "structure %d of %ld\n", global_structure_bsp_index_get(), scenario->structure_bsp_references.count);
	for (i = 0; i < scenario->players.count && scenario->players.address; i++)
	{
		struct player_starting_location *location = TAG_BLOCK_GET_ELEMENT(&scenario->players, i, struct player_starting_location);

		camera_pose_write_place(file, "player", &location->position, location->facing, 0.0f, 0.0f, 0.0f);
	}
	for (i = 0; i < scenario->cutscene_flags.count && scenario->cutscene_flags.address; i++)
	{
		struct scenario_cutscene_flag *flag = TAG_BLOCK_GET_ELEMENT(&scenario->cutscene_flags, i, struct scenario_cutscene_flag);

		camera_pose_write_place(file, "flag", &flag->position, flag->facing.yaw, flag->facing.pitch, 0.0f, 0.0f);
	}
	for (i = 0; i < scenario->cutscene_camera_points.count && scenario->cutscene_camera_points.address; i++)
	{
		struct scenario_cutscene_camera_point *point = TAG_BLOCK_GET_ELEMENT(&scenario->cutscene_camera_points, i,
			struct scenario_cutscene_camera_point);

		camera_pose_write_camera(file, point);
	}
	/* (the scenery, vehicles, bipeds and machines placed: 72, 120, 120 and 64
	bytes each in the scenario's blocks) */
	camera_pose_write_objects(file, &scenario->scenery, 0x48, &scenario->scenery_palette);
	camera_pose_write_objects(file, &scenario->vehicles, 0x78, &scenario->vehicle_palette);
	camera_pose_write_objects(file, &scenario->bipeds, 0x78, &scenario->biped_palette);
	camera_pose_write_objects(file, &scenario->machines, 0x40, &scenario->machine_palette);
	for (i = 0; i < scenario->ai_encounters.count && scenario->ai_encounters.address; i++)
	{
		struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, i, struct encounter_definition);

		for (j = 0; j < encounter->firing_positions.count && encounter->firing_positions.address; j++)
		{
			struct firing_position_definition *position = TAG_BLOCK_GET_ELEMENT(&encounter->firing_positions, j,
				struct firing_position_definition);

			camera_pose_write_place(file, "firing", &position->position, 0.0f, 0.0f, 0.0f, 0.0f);
		}
		for (j = 0; j < encounter->squads.count && encounter->squads.address; j++)
		{
			struct squad_definition *squad = TAG_BLOCK_GET_ELEMENT(&encounter->squads, j, struct squad_definition);

			for (k = 0; k < squad->starting_locations.count && squad->starting_locations.address; k++)
			{
				struct actor_starting_location *location = TAG_BLOCK_GET_ELEMENT(&squad->starting_locations, k,
					struct actor_starting_location);

				camera_pose_write_place(file, "actor", &location->position, location->facing, 0.0f, 0.0f, 0.0f);
			}
		}
	}
	fclose(file);
}
