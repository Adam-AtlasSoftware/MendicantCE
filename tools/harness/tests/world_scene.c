/* display.ambient_occlusion "world"'s scene (port/linux/game/world_scene.c,
test_world_scene.py). The skeleton's: an empty scene. Workstream A grows
it: ids stable across frames, an object's instance following its drawn
node, a deleted object's id never given again while the map is loaded, the
static triangles' copy in world space. */

#include "harness.h"

#include "under_test.inc"

static void empty_case(void)
{
	struct world_scene_triangles triangles;

	world_scene_map_loaded();
	world_scene_frame_begin();
	CHECK(world_scene_instance_count() == 0, "instances");
	CHECK(world_scene_instance_find(WORLD_SCENE_ID_STATIC_BIT) == NULL, "found");
	CHECK(world_scene_light_count() == 0, "lights");
	CHECK(!world_scene_static_triangles(0, &triangles) && triangles.count == 0, "triangles");
	world_scene_triangles_free(&triangles);
	world_scene_map_unloaded();
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "empty"))
		empty_case();
	else
		return 2;
	return 0;
}
