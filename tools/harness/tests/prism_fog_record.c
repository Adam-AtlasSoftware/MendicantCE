#include "harness.h"

/* PRISM v4's fog record (port/linux/game/prism_deferred.c's fog_record):
the window's fog as SCENE_FOG takes it, the eye's densities CE's fog pass
and the objects' draws take (the atmosphere's share by the camera's height
over the plane, the plane's by its depth under it), the camera's cluster's
plane, none without planar fog */
#include "config.inc"
#define CEILING(n,ceiling) ((n)>(ceiling)?(ceiling):(n))
#define PIN(n,floor,ceiling) ((n)<(floor) ? (floor) : CEILING((n),(ceiling)))
typedef unsigned short word;
typedef struct real_plane3d { real_vector3d n; real d; } real_plane3d;
typedef union real_rgb_color { real n[3]; struct { real red, green, blue; }; } real_rgb_color;
struct fog_screen;
#include "render_fog.inc"
static struct { struct { real_point3d position; } camera; struct render_fog fog; } global_window_parameters;
static struct { long cluster_index; } render;
#include "under_test.inc"

static boolean near(float a, float b)
{
	return a - b < 1.0e-5f && b - a < 1.0e-5f;
}

static void fog_make(float camera_z)
{
	struct render_fog *fog = &global_window_parameters.fog;

	memset(&global_window_parameters, 0, sizeof(global_window_parameters));
	global_window_parameters.camera.position.z = camera_z;
	fog->atmospheric_color.red = 0.5f;
	fog->atmospheric_maximum_density = 0.8f;
	fog->atmospheric_minimum_distance = 10.0f;
	fog->atmospheric_maximum_distance = 100.0f;
	fog->planar_mode = 1;
	/* (the plane z = 4, its normal up out of the fog) */
	fog->plane.n.k = 1.0f;
	fog->plane.d = 4.0f;
	fog->planar_color.blue = 0.25f;
	fog->planar_maximum_density = 1.5f;
	fog->planar_maximum_distance = 40.0f;
	fog->planar_maximum_depth = 2.0f;
	render.cluster_index = 6;
}

int main(int argc, char **argv)
{
	struct fog_record record;
	char const *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "above"))
	{
		/* (the camera 54 over the plane: the atmosphere's share 54 / 100, none
		of the plane's depth) */
		fog_make(58.0f);
		fog_record(&record);
		CHECK(record.flags == FOG_ATMOSPHERE && record.plane_count == 1, "flags %lu planes %lu", record.flags,
			record.plane_count);
		CHECK(near(record.atmosphere_range[0], 10.0f) && near(record.atmosphere_range[1], 100.0f) &&
			near(record.atmosphere_range[2], 0.54f), "range %f %f %f", record.atmosphere_range[0],
			record.atmosphere_range[1], record.atmosphere_range[2]);
		CHECK(near(record.atmosphere_color[0], 0.5f) && near(record.atmosphere_color[3], 0.8f), "atmosphere");
		CHECK(record.planes[0].id == 7 && record.planes[0].flags == 0, "plane id %lu flags %lu", record.planes[0].id,
			record.planes[0].flags);
		CHECK(near(record.planes[0].plane[2], 1.0f) && near(record.planes[0].plane[3], 4.0f), "the plane");
		CHECK(near(record.planes[0].color[2], 0.25f) && near(record.planes[0].color[3], 1.0f), "colour, density pinned");
		CHECK(near(record.planes[0].shape[0], 40.0f) && near(record.planes[0].shape[1], 2.0f) &&
			near(record.planes[0].shape[2], 0.0f), "shape %f", record.planes[0].shape[2]);
		return 0;
	}
	if (!strcmp(which, "inside"))
	{
		/* (the camera 1 under the plane: half its opaque depth; none of the
		atmosphere) */
		fog_make(3.0f);
		fog_record(&record);
		CHECK(record.planes[0].flags == FOG_INSIDE, "inside");
		CHECK(near(record.planes[0].shape[2], 0.5f), "the eye's depth %f", record.planes[0].shape[2]);
		CHECK(near(record.atmosphere_range[2], 0.0f), "the atmosphere's share %f", record.atmosphere_range[2]);
		return 0;
	}
	if (!strcmp(which, "dominant"))
	{
		/* (the atmosphere dominant: its share whole, wherever the camera is) */
		fog_make(3.0f);
		global_window_parameters.fog.fog_definition_flags = FLAG(FOG_ATMOSPHERE_DOMINANT);
		fog_record(&record);
		CHECK(near(record.atmosphere_range[2], 1.0f), "the atmosphere's share %f", record.atmosphere_range[2]);
		return 0;
	}
	if (!strcmp(which, "no_plane"))
	{
		/* (no planar fog: no plane sent; no atmosphere: no flag) */
		fog_make(58.0f);
		global_window_parameters.fog.planar_mode = 0;
		global_window_parameters.fog.atmospheric_maximum_density = 0.0f;
		fog_record(&record);
		CHECK(record.plane_count == 0 && record.flags == 0, "planes %lu flags %lu", record.plane_count, record.flags);
		return 0;
	}
	fprintf(stderr, "no case %s\n", which);
	return 2;
}
