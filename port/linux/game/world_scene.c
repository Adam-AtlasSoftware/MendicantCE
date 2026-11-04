/*
WORLD_SCENE.C

display.ambient_occlusion "world"'s scene (world_scene.h, docs/world-ao.md):
the structure's static geometry and the frame's objects under stable ids,
and the world light list. Workstream A.

The static meshes: the loaded structure BSP's collision geometry (its
surfaces, each a ring of edges, fanned into triangles from the ring's first
corner), in world space, as the BSP's collision surfaces are, with each
triangle's collision material. world_scene_static_triangles hands out a
copy, which a bake on another thread reads after the map's tags are gone.
Only the BSP that is loaded has its tags in place; another BSP of the map
is found when the game switches to it (world_scene_bsp_switched).

debug.world_scene_dump (a folder) writes each BSP's triangles there as it
loads, <map>_bsp<n>.obj, to look at in any mesh viewer.

Nothing here runs while the setting is not "world" (world_ao.c), and
nothing here is read by the simulation.
*/

#include "cseries.h"
#include "physics/collision_bsp_definitions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "world_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src/port_config.c */
const char *config_string(const char *name);
/* port/linux/src (halo.log) */
void platform_log(const char *format, ...);

/* ---------- the static meshes */

static struct
{
	/* the BSP whose triangles debug.world_scene_dump last wrote (NONE) */
	short dumped_bsp_index;
} world_scene_static = { NONE };

/* a collision BSP's surfaces fanned into triangles (positions: 9 floats
each, materials: one each; NULL for the count alone): each surface's ring
of edges walked as the game walks it (an edge's right side is the
surface's when its second surface is), its corners fanned from the first.
A surface whose ring leaves the BSP's edges or vertices, or doesn't close
within MAXIMUM_EDGES_PER_COLLISION_SURFACE, is left out. The count */
static long world_scene_fan(
	struct collision_surface const *surfaces, long surface_count,
	struct collision_edge const *edges, long edge_count,
	struct collision_vertex const *vertices, long vertex_count,
	float *positions, short *materials)
{
	long surface_index, count = 0;

	for (surface_index = 0; surface_index < surface_count; surface_index++)
	{
		struct collision_surface const *surface = &surfaces[surface_index];
		long ring[MAXIMUM_EDGES_PER_COLLISION_SURFACE];
		long edge_index = surface->first_edge_index;
		short corners = 0, corner;

		do
		{
			struct collision_edge const *edge;
			short right;

			if (edge_index < 0 || edge_index >= edge_count || corners >= MAXIMUM_EDGES_PER_COLLISION_SURFACE)
			{
				corners = 0;
				break;
			}
			edge = &edges[edge_index];
			right = edge->surface_indices[1] == surface_index;
			if ((!right && edge->surface_indices[0] != surface_index) ||
				edge->vertex_indices[right] < 0 || edge->vertex_indices[right] >= vertex_count)
			{
				corners = 0;
				break;
			}
			ring[corners++] = edge->vertex_indices[right];
			edge_index = edge->edge_indices[right];
		}
		while (edge_index != surface->first_edge_index);
		for (corner = 1; corner + 1 < corners; corner++)
		{
			if (positions)
			{
				long const fan[3] = { ring[0], ring[corner], ring[corner + 1] };
				short vertex;

				for (vertex = 0; vertex < 3; vertex++)
				{
					real_point3d const *point = &vertices[fan[vertex]].point;

					positions[count * 9 + vertex * 3 + 0] = point->x;
					positions[count * 9 + vertex * 3 + 1] = point->y;
					positions[count * 9 + vertex * 3 + 2] = point->z;
				}
				materials[count] = surface->material_index;
			}
			count++;
		}
	}
	return count;
}

/* the triangles' bounds (zero for none) */
static void world_scene_bounds(long count, float const *positions, float bounds[6])
{
	long index;

	memset(bounds, 0, 6 * sizeof(float));
	for (index = 0; index < count * 3; index++)
	{
		short axis;

		for (axis = 0; axis < 3; axis++)
		{
			float value = positions[index * 3 + axis];

			if (!index || value < bounds[axis])
				bounds[axis] = value;
			if (!index || value > bounds[3 + axis])
				bounds[3 + axis] = value;
		}
	}
}

/* the loaded BSP's collision geometry, if it is bsp_index's (NULL) */
static struct collision_bsp const *world_scene_collision_bsp(short bsp_index)
{
	struct scenario *scenario = global_scenario_try_and_get();

	if (!scenario || bsp_index < 0 || bsp_index >= scenario->structure_bsp_references.count ||
		bsp_index != global_structure_bsp_index || !global_collision_bsp)
	{
		return NULL;
	}
	return global_collision_bsp;
}

int world_scene_static_triangles(short bsp_index, struct world_scene_triangles *triangles)
{
	struct collision_bsp const *bsp = world_scene_collision_bsp(bsp_index);
	struct collision_surface const *surfaces;
	struct collision_edge const *edges;
	struct collision_vertex const *vertices;
	long count;

	memset(triangles, 0, sizeof(*triangles));
	if (!bsp)
		return FALSE;
	surfaces = (struct collision_surface const *)bsp->surfaces.address;
	edges = (struct collision_edge const *)bsp->edges.address;
	vertices = (struct collision_vertex const *)bsp->vertices.address;
	count = world_scene_fan(surfaces, bsp->surfaces.count, edges, bsp->edges.count, vertices, bsp->vertices.count,
		NULL, NULL);
	if (count > 0)
	{
		triangles->positions = malloc((size_t)count * 9 * sizeof(float));
		triangles->materials = malloc((size_t)count * sizeof(short));
		if (!triangles->positions || !triangles->materials)
		{
			world_scene_triangles_free(triangles);
			return FALSE;
		}
		world_scene_fan(surfaces, bsp->surfaces.count, edges, bsp->edges.count, vertices, bsp->vertices.count,
			triangles->positions, triangles->materials);
	}
	triangles->count = count;
	world_scene_bounds(count, triangles->positions, triangles->bounds);
	return TRUE;
}

void world_scene_triangles_free(struct world_scene_triangles *triangles)
{
	free(triangles->positions);
	free(triangles->materials);
	triangles->positions = NULL;
	triangles->materials = NULL;
	triangles->count = 0;
}

/* debug.world_scene_dump: the loaded BSP's triangles written to the
folder as <map>_bsp<n>.obj (once each while the map is loaded), each
collision material a group */
static void world_scene_dump(short bsp_index)
{
	char const *folder = config_string("debug.world_scene_dump");
	char const *scenario_name, *map_name;
	struct world_scene_triangles triangles;
	char path[1024];
	FILE *file;
	long index;
	short material = -2;

	if (!folder || !*folder || bsp_index == world_scene_static.dumped_bsp_index ||
		!world_scene_static_triangles(bsp_index, &triangles))
	{
		return;
	}
	world_scene_static.dumped_bsp_index = bsp_index;
	scenario_name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	map_name = scenario_name ? scenario_name : "map";
	if (strrchr(map_name, '\\'))
		map_name = strrchr(map_name, '\\') + 1;
	snprintf(path, sizeof(path), "%s/%s_bsp%d.obj", folder, map_name, (int)bsp_index);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "# %s bsp %d: %ld collision triangles, world units\n", map_name, (int)bsp_index,
			triangles.count);
		for (index = 0; index < triangles.count * 3; index++)
		{
			fprintf(file, "v %.4f %.4f %.4f\n", (double)triangles.positions[index * 3],
				(double)triangles.positions[index * 3 + 1], (double)triangles.positions[index * 3 + 2]);
		}
		for (index = 0; index < triangles.count; index++)
		{
			if (triangles.materials[index] != material)
			{
				material = triangles.materials[index];
				fprintf(file, "g material_%d\n", (int)material);
			}
			fprintf(file, "f %ld %ld %ld\n", index * 3 + 1, index * 3 + 2, index * 3 + 3);
		}
		fclose(file);
	}
	platform_log("world scene: %s bsp %d: %ld triangles, bounds %.1f %.1f %.1f to %.1f %.1f %.1f%s%s",
		map_name, (int)bsp_index, triangles.count, (double)triangles.bounds[0], (double)triangles.bounds[1],
		(double)triangles.bounds[2], (double)triangles.bounds[3], (double)triangles.bounds[4],
		(double)triangles.bounds[5], file ? ", written to " : ", not written to ", path);
	world_scene_triangles_free(&triangles);
}

/* ---------- the map */

void world_scene_map_loaded(void)
{
	world_scene_static.dumped_bsp_index = NONE;
	world_scene_dump(global_structure_bsp_index);
}

void world_scene_map_unloaded(void)
{
	world_scene_static.dumped_bsp_index = NONE;
}

void world_scene_bsp_switched(short bsp_index)
{
	world_scene_dump(bsp_index);
}

/* ---------- the frame */

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
