/* prism_shadows.c: PRISM's sun shadows (display.prism_shadows "cascaded";
off unless set), the game's half. The renderer's is xgpu_spirv.c's
prism_sun_* and the backends' pass (xgpu.h's "PRISM's sun shadows",
xgpu_stream.h's PRISM_SHADOWS).

Each primary window whose clusters see the sky (render.visible_sky_model)
draws its casters from the sky's sun (render_sky_sun: its brightest light,
as its lens flare is aimed) before its surfaces: render.c calls
prism_shadows_pass_begin, draws the objects again (render_objects, with
render_prism_shadow_pass: their opaque parts alone, no first-person
weapon, nothing queued), then prism_shadows_pass_structure and the
structure's lightmap pass again, then prism_shadows_pass_end. The backend
draws each opaque draw of the pass into each cascade (its shadow twin) and
skips the others. The cascades are spheres about slices of the camera's
view (out to PRISM_SHADOWS_REACH world units), their maps' texels fixed in
the light's plane so that a still shadow does not shimmer as the camera
turns or moves. The surfaces drawn after read them (the lightmap pass and
the models with PRISM v2's light: d3d8_device.c, prism_light.c) until
prism_shadows_window_end.

Nothing here changes what the game simulates: it draws, and reads only
the camera, the sky's tag and the settings. */

#include "cseries.h"
#include "render/render.h"
#include "scenario/scenario.h"

#include <math.h>
#include <string.h>

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
long config_integer(char const *name);
double config_real(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
int halo_prism_shadows_pass(int phase, int resolution, int cascades, float const *block);
/* render_sky.c's */
boolean render_sky_sun(short sky_index, real_vector3d *direction, real_rgb_color *color, real *power);

/* ---------- constants */

/* (xgpu_stream.h's XGPU_PRISM_SHADOWS_*: the game's sources do not see the
renderer's headers) */
enum
{
	PRISM_SHADOWS_NONE,
	PRISM_SHADOWS_BEGIN,
	PRISM_SHADOWS_STRUCTURE,
	PRISM_SHADOWS_END,
};
#define PRISM_SHADOWS_CASCADES 3
#define PRISM_SHADOWS_CASCADES_MOST 4
/* (struct xgpu_prism_shadows_block's floats: four cascades' matrices, then
sun, light, texel, depth, fade, tune and two spare) */
#define PRISM_SHADOWS_BLOCK_FLOATS (PRISM_SHADOWS_CASCADES_MOST * 16 + 8 * 4)
#define BLOCK_SUN (PRISM_SHADOWS_CASCADES_MOST * 16)
#define BLOCK_LIGHT (BLOCK_SUN + 4)
#define BLOCK_TEXEL (BLOCK_SUN + 8)
#define BLOCK_DEPTH (BLOCK_SUN + 12)
#define BLOCK_FADE (BLOCK_SUN + 16)
#define BLOCK_TUNE (BLOCK_SUN + 20)

/* the cascades' far ends in world units (a Spartan is 0.7 tall): the first
holds the characters near the camera at a few millimetres a texel, the last
the scene out to about 150 metres */
static real const cascade_ends[PRISM_SHADOWS_CASCADES] = { 4.0f, 14.0f, 48.0f };
#define PRISM_SHADOWS_REACH 48.0f
/* how far toward the sun past a cascade's sphere its casters are kept (a
tall tree, a Pelican, the structure's cliffs above the scene) */
#define PRISM_SHADOWS_TOWARD_SUN 60.0f

/* the receivers' light (struct xgpu_prism_shadows_block's light and fade):
the share of a lightmap's sunlit light the moving casters take away; the
penumbra's growth a world unit from the blocker (the sun's apparent size,
widened: about 1.4 degrees); where the last cascade starts to fade; the
structure's share on a model; a model's sunlit share the casters take */
#define PRISM_SHADOWS_STRENGTH 0.85f
#define PRISM_SHADOWS_SOFTNESS 0.035f
#define PRISM_SHADOWS_FADE 0.8f
#define PRISM_SHADOWS_STRUCTURE_ON_MODELS 0.35f
#define PRISM_SHADOWS_MODEL_STRENGTH 0.85f
/* the bake's sun visibility (struct xgpu_prism_shadows_block's tune): a
lightmap texel's share of the sun, its dominant incident direction's
nearness to the sun from tune.x to tune.y (cosines) times its directional
share times tune.z (at most 1): where the bake already shadowed it, the
casters take nothing; and the cascades' blend's band (the share of a
cascade's half width short of its edge where its pixels start to take the
next) */
#define PRISM_SHADOWS_BAKED_FROM 0.3f
#define PRISM_SHADOWS_BAKED_TO 0.8f
#define PRISM_SHADOWS_BAKED_GAIN 1.25f
#define PRISM_SHADOWS_BLEND_BAND 0.15f
/* a dim sun (a moon, a hazy sky: its light's power) casts weaker and
softer shadows: the strength by PIN((power - FROM) / RANGE, LEAST, 1), the
softness by 1 + SOFTER (1 - that) */
#define PRISM_SHADOWS_POWER_FROM 0.4f
#define PRISM_SHADOWS_POWER_RANGE 1.2f
#define PRISM_SHADOWS_POWER_LEAST 0.35f
#define PRISM_SHADOWS_POWER_SOFTER 1.5f

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	boolean wanted;
	int resolution;
	/* display.prism_shadow_softness and _strength */
	real softness;
	real strength;
	/* the map's sun (from its sky when a window last saw it), found */
	short sky_index;
	boolean sun_found;
	real_vector3d sun;
	real sun_power;
	/* a pass was drawn for the window being drawn: its surfaces read it */
	boolean receiving;
	boolean logged;
	float block[PRISM_SHADOWS_BLOCK_FLOATS];
} prism_shadows = { 0 };

/* ---------- private code */

static void settings_read(
	void)
{
	char const *value;
	long resolution;

	if (prism_shadows.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_shadows.settings_read = config_changes() + 1;
	value = config_string("display.prism_shadows");
	prism_shadows.wanted = value && !strcmp(value, "cascaded");
	resolution = config_integer("display.prism_shadow_resolution");
	prism_shadows.resolution = resolution >= 4096 ? 4096 : resolution >= 2048 ? 2048 : resolution >= 1024 ? 1024 : 2048;
	prism_shadows.softness = PIN(config_real("display.prism_shadow_softness"), 0.25f, 4.f);
	prism_shadows.strength = PIN(config_real("display.prism_shadow_strength"), 0.f, 1.f);

	return;
}

/* the sun of the window's sky (or the one last found for this map) */
static boolean sun_find(
	void)
{
	short sky_index = render.visible_sky_model ? render.visible_sky_index : NONE;
	real_rgb_color color;
	real power;

	if (sky_index == NONE)
	{
		return FALSE;
	}
	if (prism_shadows.sun_found && prism_shadows.sky_index == sky_index)
	{
		return TRUE;
	}
	prism_shadows.sun_found = render_sky_sun(sky_index, &prism_shadows.sun, &color, &power);
	prism_shadows.sun_power = prism_shadows.sun_found ? power : 0.f;
	prism_shadows.sky_index = sky_index;
	if (prism_shadows.sun_found)
	{
		platform_log("prism: the sun's shadows: sky %d's sun toward (%.3f %.3f %.3f), colour %.2f %.2f %.2f, power %.2f",
			sky_index, prism_shadows.sun.i, prism_shadows.sun.j, prism_shadows.sun.k, color.red, color.green, color.blue,
			power);
	}
	else
	{
		platform_log("prism: the sun's shadows: sky %d has no light: none", sky_index);
	}
	return prism_shadows.sun_found;
}

/* the block (the cascades about the camera) */
static void block_build(
	struct render_camera const *camera,
	struct render_frustum const *frustum)
{
	float *block = prism_shadows.block;
	real_vector3d const *sun = &prism_shadows.sun;
	real_vector3d right, up, reference;
	real tangent_x = MAX(fabs(frustum->frustum_bounds.x0), fabs(frustum->frustum_bounds.x1));
	real tangent_y = MAX(fabs(frustum->frustum_bounds.y0), fabs(frustum->frustum_bounds.y1));
	real corner = (real)sqrt(tangent_x * tangent_x + tangent_y * tangent_y);
	real start = MAX(camera->z_near, 0.01f);
	real dim = PIN((prism_shadows.sun_power - PRISM_SHADOWS_POWER_FROM) / PRISM_SHADOWS_POWER_RANGE,
		PRISM_SHADOWS_POWER_LEAST, 1.f);
	short cascade;

	memset(block, 0, sizeof(prism_shadows.block));
	/* (the light's plane: right and up across the sun's direction) */
	if (fabs(sun->k) < 0.95f)
	{
		reference.i = 0.f;
		reference.j = 0.f;
		reference.k = 1.f;
	}
	else
	{
		reference.i = 1.f;
		reference.j = 0.f;
		reference.k = 0.f;
	}
	cross_product3d(&reference, sun, &right);
	normalize3d(&right);
	cross_product3d(sun, &right, &up);
	normalize3d(&up);
	for (cascade = 0; cascade < PRISM_SHADOWS_CASCADES; cascade++)
	{
		float *matrix = block + cascade * 16;
		real near_end = cascade ? cascade_ends[cascade - 1] : start;
		real far_end = cascade_ends[cascade];
		/* the smallest sphere about the slice (its centre on the axis) */
		real middle = MIN(far_end, 0.5f * (near_end + far_end) * (1.f + corner * corner));
		real radius = (real)sqrt((far_end - middle) * (far_end - middle) + corner * far_end * corner * far_end);
		real radius_near = (real)sqrt((middle - near_end) * (middle - near_end) + corner * near_end * corner * near_end);
		real texel, x, y, z, toward, away, range;
		real_point3d centre;

		radius = MAX(radius, radius_near);
		/* (a radius of whole sixteenths: the texel's size steps rarely) */
		radius = (real)ceil(radius * 16.f) / 16.f;
		centre.x = camera->position.x + camera->forward.i * middle;
		centre.y = camera->position.y + camera->forward.j * middle;
		centre.z = camera->position.z + camera->forward.k * middle;
		texel = 2.f * radius / (real)prism_shadows.resolution;
		/* (the centre moved to a texel's corner in the light's plane) */
		x = (real)floor(dot_product3d((real_vector3d *)&centre, &right) / texel) * texel;
		y = (real)floor(dot_product3d((real_vector3d *)&centre, &up) / texel) * texel;
		z = dot_product3d((real_vector3d *)&centre, sun);
		toward = z + radius + PRISM_SHADOWS_TOWARD_SUN;
		away = z - radius - 1.f;
		range = toward - away;
		/* x and y -1 to 1 across the tile; z reversed: 1 at the end
		toward the sun, 0 at the other */
		matrix[0] = right.i / radius;
		matrix[1] = right.j / radius;
		matrix[2] = right.k / radius;
		matrix[3] = -x / radius;
		matrix[4] = up.i / radius;
		matrix[5] = up.j / radius;
		matrix[6] = up.k / radius;
		matrix[7] = -y / radius;
		matrix[8] = sun->i / range;
		matrix[9] = sun->j / range;
		matrix[10] = sun->k / range;
		matrix[11] = -away / range;
		matrix[15] = 1.f;
		block[BLOCK_TEXEL + cascade] = texel;
		block[BLOCK_DEPTH + cascade] = range;
	}
	block[BLOCK_SUN + 0] = sun->i;
	block[BLOCK_SUN + 1] = sun->j;
	block[BLOCK_SUN + 2] = sun->k;
	block[BLOCK_SUN + 3] = (float)PRISM_SHADOWS_CASCADES;
	block[BLOCK_LIGHT + 0] = PRISM_SHADOWS_STRENGTH * prism_shadows.strength * dim;
	block[BLOCK_LIGHT + 1] = PRISM_SHADOWS_SOFTNESS * prism_shadows.softness *
		(1.f + PRISM_SHADOWS_POWER_SOFTER * (1.f - dim));
	block[BLOCK_LIGHT + 2] = 1.f / (float)(prism_shadows.resolution * PRISM_SHADOWS_CASCADES);
	block[BLOCK_LIGHT + 3] = (float)prism_shadows.resolution;
	block[BLOCK_FADE + 0] = PRISM_SHADOWS_FADE;
	block[BLOCK_FADE + 1] = PRISM_SHADOWS_STRUCTURE_ON_MODELS;
	block[BLOCK_FADE + 2] = PRISM_SHADOWS_MODEL_STRENGTH * prism_shadows.strength * dim;
	block[BLOCK_TUNE + 0] = PRISM_SHADOWS_BAKED_FROM;
	block[BLOCK_TUNE + 1] = PRISM_SHADOWS_BAKED_TO;
	block[BLOCK_TUNE + 2] = PRISM_SHADOWS_BAKED_GAIN;
	block[BLOCK_TUNE + 3] = PRISM_SHADOWS_BLEND_BAND;

	return;
}

/* ---------- public code */

/* whether display.prism_shadows is "cascaded" (the pass is drawn where the
window sees the sky) */
boolean prism_shadows_wanted(
	void)
{
	settings_read();
	return prism_shadows.wanted;
}

/* (render.c, before a primary window's objects) the pass begun: TRUE where
it is drawn (the setting, a sun, the backend), the objects to be drawn
again with render_prism_shadow_pass */
boolean prism_shadows_pass_begin(
	struct render_camera const *camera,
	struct render_frustum const *frustum)
{
	prism_shadows.receiving = FALSE;
	settings_read();
	if (!prism_shadows.wanted || camera->mirrored || !frustum->projection_valid || !sun_find())
	{
		return FALSE;
	}
	block_build(camera, frustum);
	if (!halo_prism_shadows_pass(PRISM_SHADOWS_BEGIN, prism_shadows.resolution, PRISM_SHADOWS_CASCADES,
		prism_shadows.block))
	{
		return FALSE;
	}
	if (!prism_shadows.logged)
	{
		platform_log("prism: the sun's shadows: %d cascades of %d texels, out to %.0f world units",
			PRISM_SHADOWS_CASCADES, prism_shadows.resolution, PRISM_SHADOWS_REACH);
		prism_shadows.logged = TRUE;
	}
	return TRUE;
}

/* (render.c) the structure's casters from here on */
void prism_shadows_pass_structure(
	void)
{
	halo_prism_shadows_pass(PRISM_SHADOWS_STRUCTURE, prism_shadows.resolution, PRISM_SHADOWS_CASCADES, NULL);

	return;
}

/* (render.c) the pass done: the window's surfaces read it */
void prism_shadows_pass_end(
	void)
{
	prism_shadows.receiving = halo_prism_shadows_pass(PRISM_SHADOWS_END, prism_shadows.resolution,
		PRISM_SHADOWS_CASCADES, prism_shadows.block) != 0;

	return;
}

/* (render.c, after a window's surfaces) none read it from here on */
void prism_shadows_window_end(
	void)
{
	if (prism_shadows.receiving)
	{
		halo_prism_shadows_pass(PRISM_SHADOWS_NONE, prism_shadows.resolution, PRISM_SHADOWS_CASCADES, NULL);
	}
	prism_shadows.receiving = FALSE;

	return;
}

/* whether the window's surfaces read the sun's shadows (prism_light.c's
models, d3d8_device.c's lightmap pass) */
int prism_shadows_receiving(
	void)
{
	return prism_shadows.receiving;
}

/* (a map loaded or unloaded) its sun found again */
void prism_shadows_map_changed(
	void)
{
	prism_shadows.sun_found = FALSE;
	prism_shadows.sky_index = NONE;
	prism_shadows.receiving = FALSE;

	return;
}
