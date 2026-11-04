/*
WORLD_SDF.C

display.ambient_occlusion "world"'s distance field of the structure
(world_sdf.h, docs/world-ao.md): found in the data folder's world_sdf, or
baked in the background (never during a load), and uploaded a few slices a
frame. Workstream B; the bake's arithmetic is world_sdf_bake.c.

The skeleton: no field, ever (the proxies alone would shade).
*/

#include "world_sdf.h"

#include <stdlib.h>

void world_sdf_map_loaded(void)
{
	/* B: the cache looked in; the missing fields' bake begun in the
	background, from world_scene_static_triangles' copy */
}

void world_sdf_map_unloaded(void)
{
	/* B: a running bake abandoned; the field released (halo_world_ao_field) */
}

void world_sdf_bsp_switched(short bsp_index)
{
	(void)bsp_index;
}

void world_sdf_frame(void)
{
	/* B: a finished bake's field uploaded, a few slices a frame */
}

void world_sdf_startup_map_loaded(void)
{
	/* B: the "Preparing menu pictures" pass's map: its fields baked now */
}

int world_sdf_startup_wanted(void)
{
	return 0;
}

int world_sdf_state(void)
{
	return _world_sdf_none;
}

float world_sdf_fade(void)
{
	return 0.0f;
}

struct world_sdf_header const *world_sdf_current(void)
{
	return NULL;
}
