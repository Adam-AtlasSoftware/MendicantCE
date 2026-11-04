/* display.ambient_occlusion "world"'s game side: the setting read, the
frame's and the window's hooks doing nothing unless it is "world"; a
window's block made from the field and the proxies; each object named with
its owner slot and reference, the first-person weapon left unlit; the
made-up window of debug.world_ao_test (port/linux/game/world_ao.c;
test_world_ao.py). The parts' own tests are test_world_scene.py,
test_world_sdf.py and test_world_proxies.py; the shading's arithmetic,
tools/test_world_ao_light.py. */

#include "harness.h"
#include "world_ao.h"
#include "world_proxies.h"
#include "world_scene.h"
#include "world_sdf.h"

static const char *setting = "off";
static const char *test_setting = "";
static unsigned long changes = 1;
static int scene_frames, sdf_frames, windows_handed, fields_handed;
static struct
{
	float field[2][4], parameters[2][4], proxies[8][3][4], grid[2][4];
	unsigned int cells;
	int count;
} handed;
static int object_owner = -100;
unsigned char render_enumerating;
static float object_reference = -100.0f;

const char *config_string(const char *name)
{
	if (!strcmp(name, "debug.world_ao_test"))
		return test_setting;
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

/* the field: a volume whose header world_sdf_current hands on, half faded in */
static struct world_sdf_volume volume;
static int field_ready;

struct world_sdf_header const *world_sdf_current(void)
{
	return field_ready ? &volume.header : NULL;
}

float world_sdf_fade(void)
{
	return 0.5f;
}

signed char world_sdf_encode(float distance, float range)
{
	(void)range;
	return distance > 0.0f ? 1 : -1;
}

/* three shapes: two of object 100 (slot 0), one of object 200 (slot 1) */
long world_proxies_window(float const camera_position[3], struct world_proxy *proxies, long *owners, long maximum)
{
	static const long ids[3] = { 100, 100, 200 };
	long index;

	(void)camera_position;
	for (index = 0; index < 3 && index < maximum; index++)
	{
		memset(&proxies[index], 0, sizeof(proxies[index]));
		proxies[index].v[0][0] = (float)index;
		proxies[index].v[0][3] = _world_proxy_sphere;
		proxies[index].v[1][3] = ids[index] == 100 ? 0.0f : 1.0f;
		proxies[index].v[2][0] = 0.5f;
		owners[index] = ids[index];
	}
	return index;
}

/* object 200's lighting point; the others not in the scene */
struct world_scene_instance const *world_scene_instance_find(long id)
{
	static struct world_scene_instance instance;

	if (id != 200)
		return NULL;
	memset(&instance, 0, sizeof(instance));
	instance.id = id;
	instance.lighting_point[2] = 7.0f;
	return &instance;
}

/* (world_ao_shading.c's, tested apart: here, which point; asked without a
field, a value that shows it was) */
float world_ao_reference(struct world_sdf_volume const *reference_volume, float const lighting_point[3])
{
	return reference_volume ? lighting_point[2] / 10.0f : 0.3f;
}

/* (world_ao_shading.c's grid, tested apart: here, about which camera, of
how many proxies) */
void world_ao_grid(struct world_proxy const *proxies, long count, float const camera[3], float grid[2][4],
	unsigned int cells[WORLD_AO_GRID_CELLS / 2][4])
{
	(void)proxies;
	memset(grid, 0, sizeof(float) * 8);
	memcpy(grid[0], camera, sizeof(float) * 3);
	cells[0][0] = (unsigned int)count;
}

void halo_world_ao_window(float const field[2][4], float const parameters[2][4], float const (*proxies)[3][4],
	int count, float const grid[2][4], unsigned int const (*cells)[4])
{
	windows_handed++;
	handed.count = count;
	if (count < 0)
		return;
	memcpy(handed.field, field, sizeof(handed.field));
	memcpy(handed.parameters, parameters, sizeof(handed.parameters));
	memcpy(handed.proxies, proxies, (size_t)(count < 8 ? count : 8) * sizeof(handed.proxies[0]));
	memcpy(handed.grid, grid, sizeof(handed.grid));
	handed.cells = cells ? cells[0][0] : 0xffffffffu;
}

void halo_world_ao_object(int owner, float reference)
{
	object_owner = owner;
	object_reference = reference;
}

/* (the made-up field made sparse and handed on: world_sdf_bake.c's and
world_sdf.c's, tested apart) */
int world_sdf_pack(struct world_sdf_volume *packed, signed char const *voxels)
{
	(void)voxels;
	packed->cells = NULL;
	packed->bricks = NULL;
	return 1;
}

void world_sdf_upload(struct world_sdf_volume const *uploaded)
{
	(void)uploaded;
	fields_handed++;
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
		world_ao_object_begin(200);
		world_ao_first_person(1);
		world_ao_window_end();
	}
	CHECK(scene_frames == 0 && sdf_frames == 0 && windows_handed == 0, "%d %d %d", scene_frames, sdf_frames,
		windows_handed);
	CHECK(object_owner == -100, "an object named while off: %d", object_owner);
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

static void window_case(void)
{
	float const camera[3] = { 1.0f, 2.0f, 3.0f };

	setting = "world";
	changes++;
	volume.header.width = 8;
	volume.header.height = 4;
	volume.header.depth = 2;
	volume.header.voxel = 0.5f;
	volume.header.range = 6.0f;
	volume.header.origin[0] = -1.0f;
	field_ready = 1;
	/* (none while a map's pipelines are prepared) */
	render_enumerating = 1;
	world_ao_window_begin(0, 0, camera);
	CHECK(windows_handed == 0, "while enumerating: %d", windows_handed);
	render_enumerating = 0;
	world_ao_window_begin(0, 0, camera);
	CHECK(windows_handed == 1 && handed.count == 3, "%d %d", windows_handed, handed.count);
	CHECK(handed.field[0][0] == -1.0f && handed.field[0][3] == 2.0f && handed.field[1][0] == 8.0f &&
		handed.field[1][2] == 2.0f && handed.field[1][3] == 1.0f, "the field's place (its atlas's layer)");
	CHECK(handed.parameters[0][0] == WORLD_AO_STRENGTH_OBJECTS && handed.parameters[0][1] == WORLD_AO_STRENGTH_LEVEL &&
		handed.parameters[0][2] == 0.5f && handed.parameters[0][3] == 3.0f &&
		handed.parameters[1][0] == WORLD_AO_REFERENCE_WEIGHT, "the parameters");
	CHECK(handed.proxies[2][1][3] == 1.0f, "the proxies' slots handed on as they were");
	CHECK(handed.grid[0][0] == 1.0f && handed.grid[0][2] == 3.0f && handed.cells == 3,
		"the proxies' grid about the camera");
	/* each object's slot (its first shape's) and reference */
	world_ao_object_begin(200);
	CHECK(object_owner == 1 && object_reference == 0.7f, "%d %g", object_owner, object_reference);
	world_ao_object_begin(100);
	CHECK(object_owner == 0 && object_reference == 1.0f, "%d %g", object_owner, object_reference);
	world_ao_object_begin(300);
	CHECK(object_owner == WORLD_AO_OWNER_NONE && object_reference == 1.0f, "%d %g", object_owner, object_reference);
	world_ao_first_person(1);
	CHECK(object_owner == WORLD_AO_OWNER_UNLIT, "the first-person weapon: %d", object_owner);
	world_ao_first_person(0);
	CHECK(object_owner == WORLD_AO_OWNER_NONE, "after it: %d", object_owner);
	world_ao_object_begin(-1);
	CHECK(object_owner == WORLD_AO_OWNER_NONE && object_reference == 1.0f, "the structure");
	world_ao_window_end();
	CHECK(windows_handed == 2 && handed.count == -1, "taken away: %d %d", windows_handed, handed.count);
	object_owner = -100;
	world_ao_object_begin(200);
	CHECK(object_owner == -100, "an object named with no window");
	/* without a field: none faded in, every reference 1 */
	field_ready = 0;
	world_ao_window_begin(0, 0, camera);
	CHECK(handed.parameters[0][2] == 0.0f && handed.field[1][0] == 1.0f, "no field");
	world_ao_object_begin(200);
	CHECK(object_owner == 1 && object_reference == 1.0f, "%d %g", object_owner, object_reference);
	world_ao_window_end();
}

static void test_case(void)
{
	float const camera[3] = { 0.0f, 0.0f, 0.0f };

	setting = "world";
	test_setting = "plane 0 0 1 0; sphere 1 2 3 0.5; box 0 0 1 2 3 4; nonsense";
	changes++;
	world_ao_window_begin(0, 0, camera);
	CHECK(handed.count == 2 && fields_handed == 1, "%d %d", handed.count, fields_handed);
	CHECK(handed.proxies[0][0][3] == _world_proxy_sphere && handed.proxies[0][2][0] == 0.5f &&
		handed.proxies[0][1][3] == 200.0f, "the sphere");
	CHECK(handed.proxies[1][0][3] == _world_proxy_box && handed.proxies[1][1][0] == 2.0f &&
		handed.proxies[1][2][1] == 3.0f && handed.proxies[1][2][3] == 4.0f && handed.proxies[1][1][3] == 201.0f,
		"the box");
	CHECK(handed.parameters[0][2] == 1.0f && handed.field[0][3] == 4.0f, "its field, faded in");
	world_ao_window_end();
	world_ao_window_begin(0, 0, camera);
	CHECK(fields_handed == 1, "its field uploaded once: %d", fields_handed);
	world_ao_window_end();
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "off"))
		off_case();
	else if (!strcmp(name, "world"))
		world_case();
	else if (!strcmp(name, "window"))
		window_case();
	else if (!strcmp(name, "test"))
		test_case();
	else
		return 2;
	return 0;
}
