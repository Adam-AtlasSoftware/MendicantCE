/*
MESH_INSTANCE_VALUES.C (test)

The mesh store's instance of a replaced model (port/linux/game/mod_meshes.c's
mod_meshes_model_begin) takes the draw's own function values, and never
looks its identifier up as an object: render_model's identifier is an
object's index for an object, a deleted projectile's for a ghost and 0 for
the sky. Before, it looked the identifier up through render_interpolation.c,
which with interpolation off (HALO_INTERPOLATION=false: the fixed-clock
captures) has no record and reads the object itself: a deleted one's datum
is NULL, the b40 crash (render_interpolation_object_function_values from
mod_meshes_model_begin). test_mesh_instance_values.py takes the code
(under_test.inc): mod_meshes_model_begin, and render_interpolation.c's
lookups the old code called (the negative control puts that call back).
*/

#include "harness.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXIMUM_NODES_PER_MODEL 64
#define NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS 4
#define NUMBER_OF_OBJECT_CHANGE_COLORS 4
#define MAXIMUM_INTERPOLATED_OBJECTS 8

typedef struct { real red, green, blue; } real_rgb_color;
typedef struct
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
} real_matrix4x3;
struct render_skinning
{
	real_matrix4x3 const *node_matrices;
	short node_matrix_count;
};
struct interpolation_rotation { real q[4]; };

/* (mod_meshes.c's flags, structure and instance bits; render_interpolation.c's
record) */
#include "config.inc"

/* the fake world: the mesh store's state the code reads (mod_meshes.c's
globals), the objects alive, the instance sent */
static struct
{
	boolean secondary_motion;
	struct mesh_model *models;
	long model_count;
	long accepted;
	struct mesh_model const *drawing;
	unsigned long instance;
	unsigned long instances;
} mod_meshes_globals;

struct object_datum
{
	struct
	{
		real outgoing_function_values[NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS];
		real_rgb_color outgoing_change_colors[NUMBER_OF_OBJECT_CHANGE_COLORS];
	} object;
};

static struct object_datum objects[4];
static boolean alive[4];
static long lookups;

/* (objects.c's: a live object's datum. A deleted one's, or one never
made, is NULL in a release build, which the caller then reads: the crash,
failed here as a check) */
static struct object_datum *object_get(long object_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	lookups++;
	CHECK(object_index != NONE && absolute >= 0 && absolute < 4 && alive[absolute],
		"a datum read that is no live object's: %lx", (unsigned long)object_index);
	return &objects[absolute];
}

static boolean interpolation_rendering;
static struct interpolated_object interpolated[MAXIMUM_INTERPOLATED_OBJECTS];
static struct interpolated_object *interpolated_objects = interpolated;
static long interpolation_tick, interpolation_frame;
static real interpolation_fraction;

static real lerp(real a, real b, real t)
{
	return a + (b - a) * t;
}

static struct
{
	long calls;
	unsigned long instance, mesh, object, flags, node_count;
	float const *functions;
} sent;

static void xgpu_backend_mesh_instance(unsigned long instance, unsigned long mesh, unsigned long object,
	float level_of_detail_pixels, unsigned long flags, float const (*matrices)[3][4], unsigned long node_count,
	unsigned long game_time, float fraction, float const *functions)
{
	(void)level_of_detail_pixels;
	(void)matrices;
	(void)game_time;
	(void)fraction;
	sent.calls++;
	sent.instance = instance;
	sent.mesh = mesh;
	sent.object = object;
	sent.flags = flags;
	sent.node_count = node_count;
	sent.functions = functions;
}

static long game_time_get(void)
{
	return 100;
}

static float render_interpolation_fraction(void)
{
	return interpolation_rendering ? interpolation_fraction : 1.0f;
}

#include "under_test.inc"

static real_matrix4x3 nodes[2];
static struct mesh_model models[6];
static real draw_values[NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS] = { 0.25f, 0.5f, 0.75f, 1.0f };

/* a world with one replaced model (tag 0xE0010005, absolute index 5) and
one live object (slot 1); interpolation as asked, its records empty */
static void world(boolean interpolation)
{
	long index;

	memset(&mod_meshes_globals, 0, sizeof(mod_meshes_globals));
	memset(&sent, 0, sizeof(sent));
	memset(objects, 0, sizeof(objects));
	memset(alive, 0, sizeof(alive));
	memset(models, 0, sizeof(models));
	memset(interpolated, 0, sizeof(interpolated));
	for (index = 0; index < MAXIMUM_INTERPOLATED_OBJECTS; index++)
		interpolated[index].object_index = NONE;
	models[5].tag_index = (long)0xE0010005;
	models[5].id = 7;
	mod_meshes_globals.models = models;
	mod_meshes_globals.model_count = 6;
	mod_meshes_globals.accepted = 1;
	mod_meshes_globals.secondary_motion = TRUE;
	alive[1] = TRUE;
	interpolation_rendering = interpolation;
	interpolation_tick = 3;
	interpolation_frame = 9;
	interpolation_fraction = 0.5f;
	lookups = 0;
}

static void draw(long unique_identifier, real const *values)
{
	struct render_skinning skinning;

	skinning.node_matrices = nodes;
	skinning.node_matrix_count = 2;
	mod_meshes_model_begin((long)0xE0010005, &skinning, 120.0f, 0, unique_identifier, values);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a ghost (a projectile deleted, its identifier still the draw's) with
	interpolation off: the draw's values sent, nothing looked up */
	CASE("ghost-interpolation-off")
	{
		world(FALSE);
		draw((long)0xE0020002, draw_values);
		CHECK(sent.calls == 1, "no instance sent");
		CHECK(sent.functions == draw_values, "the instance does not take the draw's values");
		CHECK(lookups == 0, "the identifier was looked up as an object %ld times", lookups);
		CHECK(mod_meshes_globals.drawing != NULL, "the model is not being drawn");
		return 0;
	}
	/* the same with interpolation on */
	CASE("ghost-interpolation-on")
	{
		world(TRUE);
		draw((long)0xE0020002, draw_values);
		CHECK(sent.calls == 1 && sent.functions == draw_values, "the instance does not take the draw's values");
		CHECK(lookups == 0, "the identifier was looked up as an object %ld times", lookups);
		return 0;
	}
	/* the sky's draw (identifier 0): no object, no values */
	CASE("sky-interpolation-off")
	{
		world(FALSE);
		draw(0, draw_values);
		CHECK(sent.calls == 1, "no instance sent");
		CHECK(sent.functions == NULL, "the sky's draw sent function values");
		CHECK(sent.object == (unsigned long)NONE, "the sky's instance names an object: %lu", sent.object);
		CHECK(lookups == 0, "the identifier was looked up as an object %ld times", lookups);
		return 0;
	}
	/* a live object with interpolation off: its draw's values (render_model
	gave it the object's own, which the renderer read) */
	CASE("object-interpolation-off")
	{
		world(FALSE);
		draw((long)0xE0010001, draw_values);
		CHECK(sent.calls == 1 && sent.functions == draw_values, "the instance does not take the draw's values");
		CHECK(sent.object == (unsigned long)0xE0010001, "the instance names %lx", sent.object);
		CHECK(lookups == 0, "the identifier was looked up as an object %ld times", lookups);
		return 0;
	}
	printf("unknown case %s\n", case_name);
	return 2;
}
