/*
WORLD_AO.C

display.ambient_occlusion "world" (world_ao.h, docs/world-ao.md): the
renderer's side of each frame and window. The scene found again
(world_scene.c, workstream A), the field's upload continued (world_sdf.c,
B), the window's proxies chosen (world_proxies.c, C) and handed with the
field's place to the front end (halo_world_ao_*, d3d8_device.c), which the
shaders read in the ambient slot (D).

Nothing here runs unless the setting is "world": every other value (and
the Classic preset's "off") draws exactly as before.
*/

#include "world_ao.h"
#include "world_proxies.h"
#include "world_scene.h"
#include "world_sdf.h"

#include <string.h>

const char *config_string(const char *name);
unsigned long config_changes(void);

static struct
{
	int initialized;
	unsigned long read_at;
	int mode;
	/* a window's occlusion handed to the front end (world_ao_window_end
	takes it away) */
	int window;
} world_ao;

int world_ao_mode(void)
{
	unsigned long changes = config_changes();

	if (!world_ao.initialized || world_ao.read_at != changes)
	{
		const char *setting = config_string("display.ambient_occlusion");

		world_ao.initialized = 1;
		world_ao.read_at = changes;
		world_ao.mode = setting && !strcmp(setting, "world") ? WORLD_AO_PROXIES | WORLD_AO_FIELD : 0;
	}
	return world_ao.mode;
}

void world_ao_map_loaded(void)
{
	if (!world_ao_mode())
		return;
	world_scene_map_loaded();
	world_sdf_map_loaded();
}

void world_ao_bsp_switched(short bsp_index)
{
	if (!world_ao_mode())
		return;
	world_scene_bsp_switched(bsp_index);
	world_sdf_bsp_switched(bsp_index);
}

void world_ao_map_unloaded(void)
{
	world_sdf_map_unloaded();
	world_proxies_map_unloaded();
	world_scene_map_unloaded();
}

void world_ao_frame_begin(void)
{
	if (!world_ao_mode())
		return;
	world_scene_frame_begin();
	world_sdf_frame();
}

void world_ao_window_begin(short local_player_index, short rasterizer_target, float const camera_position[3])
{
	(void)local_player_index;
	(void)rasterizer_target;
	(void)camera_position;
	if (!world_ao_mode())
		return;
	/* D (with C's world_proxies_window and B's world_sdf_current): the
	window's block made and handed on (halo_world_ao_window); the skeleton
	hands nothing on, so that "world" draws as "off" until the streams land */
}

void world_ao_window_end(void)
{
	if (!world_ao.window)
		return;
	world_ao.window = 0;
	halo_world_ao_window(NULL, NULL, NULL, -1);
}

void world_ao_object_begin(long object_index)
{
	(void)object_index;
	if (!world_ao.window)
		return;
	/* D: the object's owner slot and reference (halo_world_ao_object) */
}
