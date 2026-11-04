/* display.ambient_occlusion "world"'s scene (port/linux/game/world_scene.c,
test_world_scene.py): the whole file compiled against a small fake game (its
game headers left out). The empty case; the static triangles' copy of a
hand-built collision BSP, in world space, each with its material. */

#include "harness.h"

#include <math.h>
#include <stdarg.h>

struct tag_block
{
	long count;
	void *address;
	void *definition;
};

/* (the collision BSP's elements and limits, from the game's header) */
#include "collision.inc"

/* only what world_scene.c reads of them */
struct collision_bsp
{
	struct tag_block surfaces, edges, vertices;
};

struct scenario
{
	struct tag_block structure_bsp_references;
};

struct scenario *fake_scenario;
struct collision_bsp *global_collision_bsp;
short global_structure_bsp_index = NONE;
long global_scenario_index = NONE;

struct scenario *global_scenario_try_and_get(void)
{
	return fake_scenario;
}

char *tag_get_name(long tag_index)
{
	(void)tag_index;
	return "levels\\test\\fake\\fake";
}

const char *config_string(const char *name)
{
	(void)name;
	return "";
}

void platform_log(const char *format, ...)
{
	(void)format;
}

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

/* A hand-built collision BSP: a square ABCD (surface 0, material 3) and a
triangle DCE above it sharing the edge CD (surface 1, material 5), each a
ring of edges as the game stores them; and a surface whose ring leaves the
BSP's edges (left out). */
static struct collision_vertex fake_vertices[5];
static struct collision_edge fake_edges[6];
static struct collision_surface fake_surfaces[3];
static struct collision_bsp fake_bsp;
static struct scenario fake_scenario_storage;

static void fake_edge(long index, long v0, long v1, long next0, long next1, long s0, long s1)
{
	struct collision_edge *edge = &fake_edges[index];

	edge->vertex_indices[0] = v0;
	edge->vertex_indices[1] = v1;
	edge->edge_indices[0] = next0;
	edge->edge_indices[1] = next1;
	edge->surface_indices[0] = s0;
	edge->surface_indices[1] = s1;
}

static void fake_map(void)
{
	static float const points[5][3] = {
		{ 10.0f, 20.0f, 1.0f }, /* A */
		{ 11.0f, 20.0f, 1.0f }, /* B */
		{ 11.0f, 21.0f, 1.0f }, /* C */
		{ 10.0f, 21.0f, 1.0f }, /* D */
		{ 10.5f, 22.0f, 1.5f }, /* E */
	};
	long index;

	for (index = 0; index < 5; index++)
	{
		fake_vertices[index].point.x = points[index][0];
		fake_vertices[index].point.y = points[index][1];
		fake_vertices[index].point.z = points[index][2];
		fake_vertices[index].first_edge_index = 0;
	}
	/* the square: A B C D (each edge's left side, 0) */
	fake_edge(0, 0, 1, 1, NONE, 0, NONE); /* AB */
	fake_edge(1, 1, 2, 2, NONE, 0, NONE); /* BC */
	fake_edge(2, 2, 3, 3, 4, 0, 1); /* CD: the square's left, the triangle's right (from D) */
	fake_edge(3, 3, 0, 0, NONE, 0, NONE); /* DA */
	/* the triangle: D (CD's right side) C E */
	fake_edge(4, 2, 4, 5, NONE, 1, NONE); /* from C */
	fake_edge(5, 4, 3, 2, NONE, 1, NONE); /* from E */
	memset(fake_surfaces, 0, sizeof(fake_surfaces));
	fake_surfaces[0].first_edge_index = 0;
	fake_surfaces[0].material_index = 3;
	fake_surfaces[1].first_edge_index = 2;
	fake_surfaces[1].material_index = 5;
	fake_surfaces[2].first_edge_index = 77;
	fake_surfaces[2].material_index = 9;
	fake_bsp.surfaces.count = 3;
	fake_bsp.surfaces.address = fake_surfaces;
	fake_bsp.edges.count = 6;
	fake_bsp.edges.address = fake_edges;
	fake_bsp.vertices.count = 5;
	fake_bsp.vertices.address = fake_vertices;
	fake_scenario_storage.structure_bsp_references.count = 2;
	fake_scenario = &fake_scenario_storage;
	global_collision_bsp = &fake_bsp;
	global_structure_bsp_index = 0;
	global_scenario_index = 1;
}

static int same_point(float const *position, long vertex)
{
	return position[0] == fake_vertices[vertex].point.x && position[1] == fake_vertices[vertex].point.y &&
		position[2] == fake_vertices[vertex].point.z;
}

static void triangles_case(void)
{
	static long const expected[3][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 3, 2, 4 } };
	static short const materials[3] = { 3, 3, 5 };
	struct world_scene_triangles triangles;
	long index;

	fake_map();
	world_scene_map_loaded();
	CHECK(world_scene_static_triangles(0, &triangles), "the loaded bsp");
	CHECK(triangles.count == 3, "%ld triangles", triangles.count);
	for (index = 0; index < 3; index++)
	{
		short corner;

		for (corner = 0; corner < 3; corner++)
		{
			CHECK(same_point(&triangles.positions[index * 9 + corner * 3], expected[index][corner]),
				"triangle %ld corner %d: %g %g %g", index, corner, triangles.positions[index * 9 + corner * 3],
				triangles.positions[index * 9 + corner * 3 + 1], triangles.positions[index * 9 + corner * 3 + 2]);
		}
		CHECK(triangles.materials[index] == materials[index], "triangle %ld material %d", index,
			triangles.materials[index]);
	}
	CHECK(triangles.bounds[0] == 10.0f && triangles.bounds[1] == 20.0f && triangles.bounds[2] == 1.0f &&
		triangles.bounds[3] == 11.0f && triangles.bounds[4] == 22.0f && triangles.bounds[5] == 1.5f,
		"bounds %g %g %g %g %g %g", triangles.bounds[0], triangles.bounds[1], triangles.bounds[2],
		triangles.bounds[3], triangles.bounds[4], triangles.bounds[5]);
	/* a copy: the tags changed (or gone) leave it as it was */
	fake_vertices[0].point.x = 99.0f;
	CHECK(triangles.positions[0] == 10.0f, "a copy");
	world_scene_triangles_free(&triangles);
	CHECK(!triangles.positions && !triangles.materials && !triangles.count, "freed");
	/* another bsp than the one loaded, one the map doesn't have, none */
	CHECK(!world_scene_static_triangles(1, &triangles) && !triangles.count && !triangles.positions, "bsp 1");
	CHECK(!world_scene_static_triangles(2, &triangles) && !triangles.count, "bsp 2");
	CHECK(!world_scene_static_triangles(-1, &triangles) && !triangles.count, "bsp -1");
	world_scene_map_unloaded();
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "empty"))
		empty_case();
	else if (!strcmp(name, "triangles"))
		triangles_case();
	else
		return 2;
	return 0;
}
