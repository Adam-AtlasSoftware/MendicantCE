/*
PRISM_TERRAIN.C

The terrain, weather and water overhaul's game half (docs/prism_terrain.md;
overhaul_research/prism_v4_renderer/terrain/design.md, sections 1, 2.2 and
12), T0: what of the ground, the weather and the water only the game knows,
sent to the renderer each frame as PRISM_TERRAIN (xgpu_stream.h):

- FRAME: a generation (a new map, a BSP switch, a revert to a checkpoint,
  a new game: the game's clock going back), the game's time step (0 while
  paused), real time, the first local player's camera, the BSP, the
  settings, the camera's weather palette entry, the wind at the eye
  (scenario_get_wind, a pure read) and each palette entry's wind there;
- EVENTS: what the hooks in source/ appended since the last frame: feet
  (bipeds.c, the ground under them from the footstep's own ray in
  material_effects.c), vehicles' mass points on the ground (vehicles.c),
  projectiles' impacts and detonations (projectiles.c), explosions
  (damage.c), within EVENT_RANGE of a local player's camera;
- WADERS: the units near the eye in water, or out of it for less than
  WADER_SECONDS (a table of this file's own: when each left).

Only with display.lighting "raster" or "pathtraced", display.terrain "on",
in a campaign (no game engine runs) being played (prism_terrain_wanted);
never in Classic, multiplayer or the menus. Every hook's first act is that
test, and none writes the game's state, calls a random function or the
seeds: the hooks read their arguments and append to this file's ring, and
the frame reads objects, the scenario and the wind. Of what the renderer
makes of it only one thing comes back, whether it loaded the terrain pack
(T1: the texture pass fades CE's detail maps only then), and that reaches
no state of the game's. HALO_PORT_NETWORK_VERSION is unchanged.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "cache/cache_files.h"
#include "camera/observer.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "scenario/scenario.h"
#include "scenario/wind.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_files.h"
#include "shaders/shader_definitions.h"
#include "prism_deferred.h"
#include "prism_terrain.h"
#include "../src/xgpu_stream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* (the data folder: port/linux/src/platform.h's) */
char const *platform_data_root(void);
/* (sdl_platform.c's: whether the renderer loaded the terrain pack of that
folder, 1 loaded, -1 not, 0 not tried yet) */
int platform_prism_terrain_pack(char const *folder);

/* ---------- prototypes */

char const *config_string(char const *name);
int config_boolean(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
void xgpu_backend_prism_terrain(const struct xgpu_stream_prism_terrain *terrain, const void *items,
	unsigned long count);
unsigned long long SDL_GetTicksNS(void);
/* ui_widget.c's */
boolean main_menu_is_active(void);

/* ---------- constants */

/* (events within this many world units of a local player's camera: what
the renderer can show) */
#define EVENT_RANGE 60.0f
/* (a footstep's ray's hit belongs to the foot just sent within this) */
#define GROUND_REACH 0.6f
#define WADER_SECONDS 10.0f
#define WADERS_KEPT 64
#define GAME_TICKS_PER_SECOND 30.0f

/* the weather palette entry's first fields (wind.c's layout) */
struct terrain_weather_entry
{
	char name[32];
	struct tag_reference particle_system;
};

/* ---------- globals */

static struct
{
	/* the settings (read when they change) */
	unsigned long changes;
	int read;
	boolean on, tracks, drifts, layers_hq, weather, water, view_class, view_events, detail_fade;
	unsigned long quality;
	/* the frame's events since the last frame, those left out (the ring
	full), and the foot whose ground comes next (-1 none) */
	struct xgpu_stream_terrain_event events[XGPU_TERRAIN_EVENTS_MOST];
	unsigned long event_count, dropped;
	long pending_foot;
	/* the waders: each unit's datum index and when it left the water (the
	game's ticks; -1 still in it) */
	long wader_objects[WADERS_KEPT];
	long wader_left[WADERS_KEPT];
	/* the generation and what it is of */
	unsigned long generation, map_checksum;
	short bsp_index;
	long game_time;
	unsigned long long real_ns;
	boolean active;
	/* (T1) the terrain pack's folder (display.terrain_pack_folder; the data
	folder's prism_terrain), sent with each FRAME */
	char pack_folder[XGPU_TERRAIN_PATH];
	/* (whether the renderer loaded its pack: platform_prism_terrain_pack,
	asked each frame the HQ layers are asked for; 1 loaded and its layers
	drawn in the renderer's last frame, -1 not loaded, 0 not tried yet or not
	drawn. CE's detail maps fade only where it is 1, since only then do the
	layers stand in for them; the last answer logged beside it) */
	int pack_state, pack_logged;
	boolean detail_logged;
} prism_terrain;

/* ---------- private code */

static void settings_read(void)
{
	unsigned long changes = config_changes();
	char const *value;

	if (prism_terrain.read && changes == prism_terrain.changes)
	{
		return;
	}
	prism_terrain.read = 1;
	prism_terrain.changes = changes;
	value = config_string("display.terrain");
	prism_terrain.on = !value || strcmp(value, "off");
	value = config_string("display.terrain_quality");
	prism_terrain.quality = !value ? XGPU_TERRAIN_QUALITY_AUTO : !strcmp(value, "low") ? XGPU_TERRAIN_QUALITY_LOW :
		!strcmp(value, "medium") ? XGPU_TERRAIN_QUALITY_MEDIUM : !strcmp(value, "high") ? XGPU_TERRAIN_QUALITY_HIGH :
		!strcmp(value, "ultra") ? XGPU_TERRAIN_QUALITY_ULTRA : XGPU_TERRAIN_QUALITY_AUTO;
	value = config_string("display.terrain_layers");
	prism_terrain.layers_hq = !value || strcmp(value, "ce");
	prism_terrain.tracks = config_boolean("display.terrain_tracks") != 0;
	prism_terrain.drifts = config_boolean("display.terrain_drifts") != 0;
	value = config_string("display.weather");
	prism_terrain.weather = !value || strcmp(value, "ce");
	value = config_string("display.water");
	prism_terrain.water = !value || strcmp(value, "ce");
	value = config_string("debug.gbuffer_view");
	prism_terrain.view_class = value && !strcmp(value, "terrain_class");
	prism_terrain.view_events = config_boolean("debug.terrain_events") != 0;
	prism_terrain.detail_fade = config_boolean("debug.terrain_detail_fade") != 0;
	value = config_string("display.terrain_pack_folder");
	if (value && value[0])
	{
		snprintf(prism_terrain.pack_folder, sizeof(prism_terrain.pack_folder), "%s", value);
	}
	else
	{
		snprintf(prism_terrain.pack_folder, sizeof(prism_terrain.pack_folder), "%s/prism_terrain", platform_data_root());
	}
}

/* (T1) what the renderer made of the terrain pack's folder, asked each
frame the HQ layers are: CE's detail maps fade only once it says the pack
is loaded (prism_terrain_detail_level); a change logged */
static void pack_state_read(
	void)
{
	int state = prism_terrain.layers_hq ? platform_prism_terrain_pack(prism_terrain.pack_folder) : 0;

	/* (0 also while the pack is loaded but no layers were drawn in the last
	frame, so the log names each answer once, not each return to it) */
	if (state != prism_terrain.pack_logged && state != 0)
	{
		prism_terrain.pack_logged = state;
		platform_log("prism terrain: the renderer %s the terrain pack in %s%s", state > 0 ? "loaded" : "did not load",
			prism_terrain.pack_folder, state > 0 ? ": CE's fine detail maps fade near the camera" :
			": CE's ground alone, its detail maps unfaded");
	}
	prism_terrain.pack_state = state;
}

/* a direction, octahedral, 2x16 snorm (the G-buffer's normal's encoding);
0 (straight up) where it has no length */
static unsigned long octahedral(real_vector3d const *vector)
{
	real length = (real)(fabs(vector->i) + fabs(vector->j) + fabs(vector->k));
	real x, y;
	long qx, qy;

	if (!(length > 1.0e-6f))
	{
		return 0;
	}
	x = vector->i / length;
	y = vector->j / length;
	if (vector->k < 0.0f)
	{
		real fx = (1.0f - (real)fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
		real fy = (1.0f - (real)fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);

		x = fx;
		y = fy;
	}
	qx = (long)floor(PIN(x, -1.0f, 1.0f) * 32767.0f + 0.5f);
	qy = (long)floor(PIN(y, -1.0f, 1.0f) * 32767.0f + 0.5f);
	return ((unsigned long)qx & 0xffffUL) | (((unsigned long)qy & 0xffffUL) << 16);
}

/* within EVENT_RANGE of a local player's camera */
static boolean near_a_camera(real_point3d const *point)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		if (local_player_get_player_index(local_player_index) != NONE &&
			distance_squared3d(&observer_get_camera(local_player_index)->position, point) < EVENT_RANGE * EVENT_RANGE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* a new event (NULL: the ring is full, or the place is too far) */
static struct xgpu_stream_terrain_event *event_new(unsigned long kind, real_point3d const *position, long object)
{
	struct xgpu_stream_terrain_event *event;

	if (!near_a_camera(position))
	{
		return NULL;
	}
	if (prism_terrain.event_count >= XGPU_TERRAIN_EVENTS_MOST)
	{
		prism_terrain.dropped++;
		return NULL;
	}
	event = &prism_terrain.events[prism_terrain.event_count++];
	memset(event, 0, sizeof(*event));
	event->kind = (uint32_t)(kind | ((unsigned long)XGPU_TERRAIN_MATERIAL_UNKNOWN << 16));
	event->position[0] = position->x;
	event->position[1] = position->y;
	event->position[2] = position->z;
	event->object = (uint32_t)object;
	return event;
}

static void event_material(struct xgpu_stream_terrain_event *event, long material_type)
{
	unsigned long material = material_type >= 0 && material_type < 255 ? (unsigned long)material_type :
		XGPU_TERRAIN_MATERIAL_UNKNOWN;

	event->kind = (uint32_t)((event->kind & ~0xff0000UL) | (material << 16));
}

/* a biped's species, by its tag's name */
static unsigned long species_of(long object_index)
{
	struct object_datum *object = object_get(object_index);
	char const *name = tag_get_name(object->definition_index);

	if (!name)
	{
		return XGPU_TERRAIN_SPECIES_OTHER;
	}
	if (strstr(name, "cyborg") || strstr(name, "marine") || strstr(name, "captain") || strstr(name, "crewman") ||
		strstr(name, "pilot"))
	{
		return XGPU_TERRAIN_SPECIES_HUMAN;
	}
	return strstr(name, "elite") ? XGPU_TERRAIN_SPECIES_ELITE : strstr(name, "grunt") ? XGPU_TERRAIN_SPECIES_GRUNT :
		strstr(name, "jackal") ? XGPU_TERRAIN_SPECIES_JACKAL : strstr(name, "hunter") ? XGPU_TERRAIN_SPECIES_HUNTER :
		strstr(name, "flood") ? XGPU_TERRAIN_SPECIES_FLOOD : strstr(name, "sentinel") ? XGPU_TERRAIN_SPECIES_SENTINEL :
		XGPU_TERRAIN_SPECIES_OTHER;
}

static void ring_reset(void)
{
	prism_terrain.event_count = 0;
	prism_terrain.pending_foot = -1;
}

/* the waders this frame (the units within XGPU_TERRAIN_WADER_RANGE of the
eye in water or out of it for less than WADER_SECONDS); their count */
static unsigned long waders_find(real_point3d const *eye, long now, struct xgpu_stream_terrain_wader *waders)
{
	struct object_iterator iterator;
	struct object_datum *object;
	unsigned long count = 0;
	short slot;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		boolean partially = TEST_FLAG(object->object.flags, _object_partially_under_media_bit);
		boolean wholly = TEST_FLAG(object->object.flags, _object_wholly_under_media_bit);
		short found = NONE, free_slot = NONE;
		real seconds_out = 0.0f;
		struct xgpu_stream_terrain_wader *wader;

		if (distance_squared3d(&object->object.position, eye) > XGPU_TERRAIN_WADER_RANGE * XGPU_TERRAIN_WADER_RANGE)
		{
			continue;
		}
		for (slot = 0; slot < WADERS_KEPT; slot++)
		{
			if (prism_terrain.wader_objects[slot] == iterator.index)
			{
				found = slot;
			}
			else if (free_slot == NONE && prism_terrain.wader_objects[slot] == NONE)
			{
				free_slot = slot;
			}
		}
		if (partially || wholly)
		{
			if (found == NONE)
			{
				found = free_slot;
			}
			if (found != NONE)
			{
				prism_terrain.wader_objects[found] = iterator.index;
				prism_terrain.wader_left[found] = -1;
			}
		}
		else if (found != NONE)
		{
			if (prism_terrain.wader_left[found] < 0)
			{
				prism_terrain.wader_left[found] = now;
			}
			seconds_out = (real)(now - prism_terrain.wader_left[found]) / GAME_TICKS_PER_SECOND;
			if (seconds_out > WADER_SECONDS || seconds_out < 0.0f)
			{
				prism_terrain.wader_objects[found] = NONE;
				continue;
			}
		}
		else
		{
			continue;
		}
		if (count >= XGPU_TERRAIN_WADERS_MOST)
		{
			continue;
		}
		wader = &waders[count++];
		wader->kind = (uint32_t)((partially ? XGPU_TERRAIN_WADER_PARTIALLY : 0) |
			(wholly ? XGPU_TERRAIN_WADER_WHOLLY : 0) |
			(object->object.type == _object_type_biped ? XGPU_TERRAIN_WADER_BIPED : XGPU_TERRAIN_WADER_VEHICLE) |
			((unsigned long)PIN(seconds_out * 64.0f, 0.0f, 65535.0f) << 16));
		wader->position[0] = object->object.position.x;
		wader->position[1] = object->object.position.y;
		wader->position[2] = object->object.position.z;
		wader->velocity[0] = object->object.translational_velocity.i * GAME_TICKS_PER_SECOND;
		wader->velocity[1] = object->object.translational_velocity.j * GAME_TICKS_PER_SECOND;
		wader->velocity[2] = object->object.translational_velocity.k * GAME_TICKS_PER_SECOND;
		wader->radius = object->object.bounding_sphere_radius;
	}
	return count;
}

/* the frame's fixed part: the camera, its weather and wind */
static void frame_weather(struct xgpu_stream_prism_terrain *frame)
{
	struct observer_result const *camera;
	struct structure_bsp *bsp;
	real_vector3d wind;
	short index, count;

	frame->weather_entry = -1;
	if (local_player_get_player_index(0) == NONE || global_structure_bsp_index_get() == NONE)
	{
		return;
	}
	camera = observer_get_camera(0);
	bsp = global_structure_bsp_get();
	frame->camera[0] = camera->position.x;
	frame->camera[1] = camera->position.y;
	frame->camera[2] = camera->position.z;
	if (camera->location.cluster_index != NONE && camera->location.cluster_index < bsp->clusters.count)
	{
		struct structure_cluster *cluster = TAG_BLOCK_GET_ELEMENT(&bsp->clusters, camera->location.cluster_index,
			struct structure_cluster);

		if (cluster->weather_palette_index != NONE && cluster->weather_palette_index < bsp->weather_palette.count)
		{
			struct terrain_weather_entry *entry = TAG_BLOCK_GET_ELEMENT(&bsp->weather_palette,
				cluster->weather_palette_index, struct terrain_weather_entry);

			frame->weather_entry = cluster->weather_palette_index;
			frame->weather_type = entry->particle_system.index != NONE ? 1 : 0;
			frame->weather_scale = 1.0f;
		}
		scenario_get_wind(&camera->location, &camera->position, &wind, 0);
		frame->wind[0] = wind.i;
		frame->wind[1] = wind.j;
		frame->wind[2] = wind.k;
	}
	count = (short)MIN(bsp->weather_palette.count, (long)XGPU_TERRAIN_WINDS);
	for (index = 0; index < count; index++)
	{
		scenario_get_current_from_weather_palette(&camera->position, &wind, FLAG(_scenario_current_simple_bit), index);
		frame->winds[index][0] = wind.i;
		frame->winds[index][1] = wind.j;
		frame->winds[index][2] = wind.k;
	}
	frame->wind_count = (uint32_t)(count > 0 ? count : 0);
}

/* ---------- public code */

/* (T1: the border colour that marks a ground shader's detail maps for the
renderer's near fade under the HQ layers: xgpu_shader.h's
XGPU_TERRAIN_FADE_BORDER_COLOR; their wrapped addressing never shows it) */
#define TERRAIN_FADE_BORDER_COLOR 0x01fade01L

boolean prism_terrain_detail_fades(
	void const *shader,
	long bitmap_tag_index)
{
	char const *name;
	char lower[256];
	int index;

	if (!shader || bitmap_tag_index == NONE)
	{
		return FALSE;
	}
	name = tag_get_name(bitmap_tag_index);
	for (index = 0; name && name[index] && index < (int)sizeof(lower) - 1; index++)
	{
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] + 32 : name[index]);
	}
	lower[name ? index : 0] = 0;
	return !strstr(lower, "grass") && !strstr(lower, "moss");
}

long prism_terrain_detail_level(
	void const *shader)
{
	short material_type;

	if (!shader || !prism_terrain_wanted() || !prism_terrain.layers_hq || prism_terrain.pack_state != 1 ||
		!prism_terrain.detail_fade)
	{
		return 0;
	}
	/* (CE's material types: dirt 0, sand 1, stone 2, snow 3, ice 31) */
	material_type = ((struct shader const *)shader)->base.physics.material_type;
	if (!prism_terrain.detail_logged)
	{
		prism_terrain.detail_logged = TRUE;
		platform_log("prism terrain: CE's detail maps faded near the camera under the HQ layers");
	}
	return material_type == 0 || material_type == 1 || material_type == 2 || material_type == 3 ||
		material_type == 31 ? TERRAIN_FADE_BORDER_COLOR : 0;
}

boolean prism_terrain_wanted(
	void)
{
	settings_read();

	return prism_terrain.on && (prism_deferred_raster() || prism_deferred_pathtraced()) &&
		!game_engine_running() && game_in_progress() && !main_menu_is_active();
}

void prism_terrain_frame(
	void)
{
	static struct xgpu_stream_terrain_wader waders[XGPU_TERRAIN_WADERS_MOST];
	struct xgpu_stream_prism_terrain frame;
	unsigned long long now_ns = SDL_GetTicksNS();
	unsigned long checksum, wader_count;
	real_point3d eye;
	short bsp_index;
	long game_time;
	short slot;

	if (!prism_terrain_wanted())
	{
		if (prism_terrain.active)
		{
			platform_log("prism terrain: off");
		}
		prism_terrain.active = FALSE;
		ring_reset();
		return;
	}
	checksum = cache_files_get_checksum();
	bsp_index = global_structure_bsp_index_get();
	game_time = game_time_get();
	/* (a new generation: the first frame, another map or BSP, the game's
	clock gone back: a revert, a new game) */
	if (!prism_terrain.active || checksum != prism_terrain.map_checksum || bsp_index != prism_terrain.bsp_index ||
		game_time < prism_terrain.game_time)
	{
		prism_terrain.generation++;
		prism_terrain.map_checksum = checksum;
		prism_terrain.bsp_index = bsp_index;
		prism_terrain.game_time = game_time;
		prism_terrain.real_ns = now_ns;
		for (slot = 0; slot < WADERS_KEPT; slot++)
		{
			prism_terrain.wader_objects[slot] = NONE;
		}
		if (!prism_terrain.active)
		{
			platform_log("prism terrain: on (%s, display.terrain_quality %lu, tracks %d, drifts %d, layers %s)",
				prism_deferred_pathtraced() ? "Path Traced" : "Rebaked", prism_terrain.quality,
				prism_terrain.tracks, prism_terrain.drifts, prism_terrain.layers_hq ? "hq" : "ce");
		}
		prism_terrain.active = TRUE;
	}
	memset(&frame, 0, sizeof(frame));
	frame.phase = XGPU_TERRAIN_FRAME;
	frame.generation = (uint32_t)prism_terrain.generation;
	frame.game_seconds = game_time_get_paused() ? 0.0f :
		(real)(game_time - prism_terrain.game_time) / GAME_TICKS_PER_SECOND;
	frame.real_seconds = (float)((double)(now_ns - prism_terrain.real_ns) * 1.0e-9);
	frame.bsp_index = bsp_index;
	frame.flags = XGPU_TERRAIN_FLAG_CAMPAIGN |
		(prism_deferred_pathtraced() ? XGPU_TERRAIN_FLAG_PATHTRACED : XGPU_TERRAIN_FLAG_RASTER) |
		(prism_terrain.tracks ? XGPU_TERRAIN_FLAG_TRACKS : 0) | (prism_terrain.drifts ? XGPU_TERRAIN_FLAG_DRIFTS : 0) |
		(prism_terrain.layers_hq ? XGPU_TERRAIN_FLAG_LAYERS_HQ : 0) |
		(prism_terrain.weather ? XGPU_TERRAIN_FLAG_WEATHER : 0) | (prism_terrain.water ? XGPU_TERRAIN_FLAG_WATER : 0) |
		(game_time_get_paused() ? XGPU_TERRAIN_FLAG_PAUSED : 0) |
		(prism_terrain.view_class ? XGPU_TERRAIN_FLAG_VIEW_CLASS : 0) |
		(prism_terrain.view_events ? XGPU_TERRAIN_FLAG_VIEW_EVENTS : 0);
	frame.quality = (uint32_t)prism_terrain.quality;
	frame.map_checksum = (uint32_t)checksum;
	frame.pack_folder = (uint32_t)(unsigned long)prism_terrain.pack_folder;
	pack_state_read();
	frame_weather(&frame);
	prism_terrain.game_time = game_time;
	prism_terrain.real_ns = now_ns;
	xgpu_backend_prism_terrain(&frame, NULL, 0);

	frame.phase = XGPU_TERRAIN_EVENTS;
	if (prism_terrain.event_count)
	{
		xgpu_backend_prism_terrain(&frame, prism_terrain.events, prism_terrain.event_count);
	}
	if (prism_terrain.dropped)
	{
		platform_log("prism terrain: %lu events left out (more than %u a frame)", prism_terrain.dropped,
			(unsigned)XGPU_TERRAIN_EVENTS_MOST);
		prism_terrain.dropped = 0;
	}
	ring_reset();

	eye.x = frame.camera[0];
	eye.y = frame.camera[1];
	eye.z = frame.camera[2];
	wader_count = waders_find(&eye, game_time, waders);
	if (wader_count)
	{
		frame.phase = XGPU_TERRAIN_WADERS;
		xgpu_backend_prism_terrain(&frame, waders, wader_count);
	}
}

void prism_terrain_footstep(
	long biped_index,
	short event_index,
	short contact_point_index,
	real_matrix4x3 const *marker)
{
	static const unsigned long kinds[] = { XGPU_TERRAIN_EVENT_WALK, XGPU_TERRAIN_EVENT_RUN, XGPU_TERRAIN_EVENT_SLIDE,
		XGPU_TERRAIN_EVENT_SHUFFLE, XGPU_TERRAIN_EVENT_JUMP, XGPU_TERRAIN_EVENT_LAND };
	struct xgpu_stream_terrain_event *event;
	unsigned long kind;

	if (!prism_terrain_wanted())
	{
		return;
	}
	prism_terrain.pending_foot = -1;
	kind = event_index >= 0 && event_index < (short)NUMBEROF(kinds) ? kinds[event_index] : XGPU_TERRAIN_EVENT_WALK;
	event = event_new(kind | (contact_point_index == 1 ? XGPU_TERRAIN_EVENT_RIGHT : 0), &marker->position,
		biped_index);
	if (!event)
	{
		return;
	}
	event->kind |= (uint32_t)(species_of(biped_index) << 24);
	event->facing = (uint32_t)octahedral(&marker->forward);
	event->magnitude = 1.0f;
	prism_terrain.pending_foot = (long)(event - prism_terrain.events);
}

void prism_terrain_footstep_ground(
	real_point3d const *point,
	real_vector3d const *normal,
	long material_type)
{
	struct xgpu_stream_terrain_event *event;
	real_point3d marker;

	if (prism_terrain.pending_foot < 0 || (unsigned long)prism_terrain.pending_foot >= prism_terrain.event_count ||
		!prism_terrain_wanted())
	{
		return;
	}
	event = &prism_terrain.events[prism_terrain.pending_foot];
	prism_terrain.pending_foot = -1;
	marker.x = event->position[0];
	marker.y = event->position[1];
	marker.z = event->position[2];
	if (distance_squared3d(&marker, point) > GROUND_REACH * GROUND_REACH)
	{
		return;
	}
	event->position[0] = point->x;
	event->position[1] = point->y;
	event->position[2] = point->z;
	event->normal = (uint32_t)octahedral(normal);
	event_material(event, material_type);
}

void prism_terrain_contact(
	long vehicle_index,
	real_point3d const *position,
	real_vector3d const *normal,
	real_vector3d const *velocity,
	short material_type,
	real scale)
{
	struct xgpu_stream_terrain_event *event;

	if (!prism_terrain_wanted())
	{
		return;
	}
	event = event_new(XGPU_TERRAIN_EVENT_CONTACT, position, vehicle_index);
	if (!event)
	{
		return;
	}
	event->normal = (uint32_t)octahedral(normal);
	event->facing = (uint32_t)octahedral(velocity);
	event->magnitude = PIN(scale, 0.0f, 1.0f);
	event_material(event, material_type);
}

void prism_terrain_impact(
	long projectile_index,
	real_point3d const *point,
	real_vector3d const *normal,
	short material_type,
	real_vector3d const *velocity)
{
	struct xgpu_stream_terrain_event *event;

	if (!prism_terrain_wanted())
	{
		return;
	}
	event = event_new(XGPU_TERRAIN_EVENT_IMPACT, point, projectile_index);
	if (!event)
	{
		return;
	}
	event->normal = (uint32_t)octahedral(normal);
	event->facing = (uint32_t)octahedral(velocity);
	event_material(event, material_type);
}

void prism_terrain_detonation(
	long projectile_index,
	real_point3d const *point,
	short material_type)
{
	struct xgpu_stream_terrain_event *event;

	if (!prism_terrain_wanted())
	{
		return;
	}
	event = event_new(XGPU_TERRAIN_EVENT_DETONATION, point, projectile_index);
	if (event)
	{
		event_material(event, material_type);
	}
}

void prism_terrain_explosion(
	real_point3d const *centre,
	real radius)
{
	struct xgpu_stream_terrain_event *event;

	if (!prism_terrain_wanted())
	{
		return;
	}
	event = event_new(XGPU_TERRAIN_EVENT_EXPLOSION, centre, NONE);
	if (event)
	{
		event->magnitude = radius;
	}
}

boolean prism_terrain_weather_draw(
	void)
{
	/* (T12: with display.weather "prism", PRISM_TERRAIN's WEATHER_DRAW here
	and TRUE; nothing draws it yet, so CE's weather is drawn) */
	return FALSE;
}

boolean prism_terrain_water_draw(
	void const *group)
{
	/* (T7: with display.water "prism", CE's depth alone and PRISM_TERRAIN's
	WATER_DRAW for the group; nothing draws it yet, so CE's water is
	drawn) */
	(void)group;
	return FALSE;
}
