/* display.ambient_occlusion "world"'s game side: the setting read, the
frame's and the window's hooks doing nothing unless it is "world"
(port/linux/game/world_ao.c, world_scene.c; test_world_ao.py). The parts'
own tests are test_world_sdf.py and test_world_proxies.py; workstreams A
and D grow theirs here. */

#include "harness.h"

static const char *setting = "off";
static unsigned long changes = 1;
static int scene_frames, sdf_frames, windows_handed;

const char *config_string(const char *name)
{
	(void)name;
	return setting;
}

unsigned long config_changes(void)
{
	return changes;
}

/* the parts (their stubs' behaviour: the real ones are tested apart) */
void world_scene_map_loaded(void) {}
void world_scene_map_unloaded(void) {}
void world_scene_bsp_switched(short bsp_index) { (void)bsp_index; }
void world_scene_frame_begin(void) { scene_frames++; }
void world_sdf_map_loaded(void) {}
void world_sdf_map_unloaded(void) {}
void world_sdf_bsp_switched(short bsp_index) { (void)bsp_index; }
void world_sdf_frame(void) { sdf_frames++; }
void world_proxies_map_unloaded(void) {}
void halo_world_ao_window(float const field[2][4], float const parameters[2][4], float const (*proxies)[3][4],
	int count)
{
	(void)field;
	(void)parameters;
	(void)proxies;
	(void)count;
	windows_handed++;
}

#include "under_test.inc"

static void off_case(void)
{
	float const camera[3] = { 0.0f, 0.0f, 0.0f };
	static const char *const others[] = { "off", "subtle", "strong", "" };
	int index;

	for (index = 0; index < 4; index++)
	{
		setting = others[index];
		changes++;
		CHECK(world_ao_mode() == 0, "%s", setting);
		world_ao_frame_begin();
		world_ao_window_begin(0, 0, camera);
		world_ao_window_end();
	}
	CHECK(scene_frames == 0 && sdf_frames == 0 && windows_handed == 0, "%d %d %d", scene_frames, sdf_frames,
		windows_handed);
}

static void world_case(void)
{
	setting = "world";
	changes++;
	CHECK(world_ao_mode() == (WORLD_AO_PROXIES | WORLD_AO_FIELD), "%d", world_ao_mode());
	world_ao_frame_begin();
	CHECK(scene_frames == 1 && sdf_frames == 1, "%d %d", scene_frames, sdf_frames);
	/* (and back off once the setting changes) */
	setting = "off";
	changes++;
	CHECK(world_ao_mode() == 0, "off again");
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "off"))
		off_case();
	else if (!strcmp(name, "world"))
		world_case();
	else
		return 2;
	return 0;
}
