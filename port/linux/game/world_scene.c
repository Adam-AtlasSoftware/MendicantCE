/*
WORLD_SCENE.C

display.ambient_occlusion "world"'s scene (world_scene.h, docs/world-ao.md):
the structure's static geometry and the frame's objects under stable ids,
and the world light list. Workstream A.

The skeleton: an empty scene. Nothing here runs while the setting is not
"world" (world_ao.c).
*/

#include "world_scene.h"

#include <stdlib.h>

void world_scene_map_loaded(void)
{
	/* A: the BSPs' meshes found (their collision geometry), ids given */
}

void world_scene_map_unloaded(void)
{
	/* A: every instance and mesh forgotten */
}

void world_scene_bsp_switched(short bsp_index)
{
	(void)bsp_index;
}

int world_scene_static_triangles(short bsp_index, struct world_scene_triangles *triangles)
{
	(void)bsp_index;
	triangles->count = 0;
	triangles->positions = NULL;
	triangles->materials = NULL;
	return 0;
}

void world_scene_triangles_free(struct world_scene_triangles *triangles)
{
	free(triangles->positions);
	free(triangles->materials);
	triangles->positions = NULL;
	triangles->materials = NULL;
	triangles->count = 0;
}

void world_scene_frame_begin(void)
{
	/* A: the objects as drawn (their nodes, render_interpolation.c's pose) */
}

long world_scene_instance_count(void)
{
	return 0;
}

struct world_scene_instance const *world_scene_instances(void)
{
	return NULL;
}

struct world_scene_instance const *world_scene_instance_find(long id)
{
	(void)id;
	return NULL;
}

long world_scene_light_count(void)
{
	return 0;
}

struct world_scene_light const *world_scene_lights(void)
{
	return NULL;
}
