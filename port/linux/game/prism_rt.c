/*
PRISM_RT.C

display.prism_rt (docs/prism_rt.md): PRISM's hardware ray tracing, the
game's half. Off unless set, and where the renderer cannot trace rays
(halo_prism_rt_available: xgpu_backend_capabilities' ray_tracing) nothing
here runs and nothing is sent.

With it on:

- world AO's pre-pass is drawn whatever display.ambient_occlusion says
  (world_ao_mode): its inputs (each visible pixel's normal and distance,
  whether it is the level's or an object's) are the ray tracer's G-buffer,
  and its pass's place in the frame is where the backend traces;
- the structure BSP's triangles (its render geometry: the environment
  shaders' surfaces, opaque and alpha-tested, and the light cards CE's
  radiosity lit the level from) are copied once when a BSP is first
  drawn, in world space, with each one's material (its albedo, the light
  it gives as a lamp, its alpha map) and sent a slice a frame (RT_STATIC),
  never while a map loads: the backend builds its acceleration structure
  of them and traces the last whole set meanwhile;
- the BSP's light rig (docs/prism_rt.md, "The rig"): CE's own lights (the
  sky's sun, its other lights as domes, its ambient light, the lamps) at
  the scales the relight fit found its pages made of
  (<data>/prism_lightmaps/rebaked/<map>_<bsp>.scales.json, made on the
  player's machine; the defaults where there is none);
- each frame, before its windows, RT_FRAME: the settings, the rig, the
  sky's sun (its narrowest light: render_sky_sun), its colour, the sky's
  outdoor ambient, the game clock and the frame's dynamic lights
  (world_scene's list, ranked).

The objects are not described here: the backend captures them from their
draws in the pre-pass, as they are drawn (their permutations, levels of
detail and pose).

Nothing here changes what the game simulates: it draws, and reads only the
camera's sky, the structure's render geometry (read-only once loaded) and
the settings. Nothing crosses the network.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "rasterizer/rasterizer_geometry.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "scenario/sky_definitions.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "tag_files/tag_files.h"
#include "world_scene.h"
#include "prism_world_geometry.h"
#include "prism_scene_static.h"
#include "../src/xgpu_coverage.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src */
char const *config_string(char const *name);
int config_boolean(char const *name);
long config_integer(char const *name);
double config_real(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
/* d3d8_device.c's (the renderer's: xgpu_backend_rt_*) */
int halo_prism_rt_available(void);
void halo_prism_rt_static(int phase, unsigned long generation, unsigned long first, unsigned long count,
	unsigned long triangles, void const *data);
void halo_prism_rt_frame(void const *frame);
/* render_enumerate.c's (the window drawn is a map's first frame's, unseen) */
extern unsigned char render_enumerating;
/* prism_deferred.c's: display.lighting "raster" */
boolean prism_deferred_raster(void);
/* render_sky.c's (the sky's sun, as PRISM's cascaded shadows find it, and
each of its lights) */
boolean render_sky_sun(short sky_index, real_vector3d *direction, real_rgb_color *color, real *power);
boolean render_sky_light(short sky_index, short light_index, real_vector3d *direction, real_rgb_color *color,
	real *power, real *diameter);
short render_sky_light_count(short sky_index);
/* game.h's; render_interpolation.c's (the drawn frame's place between ticks) */
long game_time_get(void);
char const *platform_data_root(void);
float render_interpolation_fraction(void);

/* ---------- constants */

/* (xgpu_stream.h's: the game's sources do not see the renderer's headers) */
enum
{
	PRISM_RT_OFF,
	PRISM_RT_SHADOWS,
	PRISM_RT_GI,
	PRISM_RT_PATHTRACED,
	PRISM_RT_STATIC_BEGIN = 0,
	PRISM_RT_STATIC_TRIANGLES,
	PRISM_RT_STATIC_END,
	PRISM_RT_STATIC_RELEASE,
	PRISM_RT_STATIC_ATTRIBUTES,
	PRISM_RT_STATIC_MATERIALS,
	PRISM_RT_STATIC_ALPHA,
	PRISM_RT_SLICE = 65536,
	PRISM_RT_LIGHTS = 64,
	/* a triangle's kind (XGPU_RT_TRIANGLE_*), its attributes' words, a
	material's floats and the materials at most (XGPU_STREAM_RT_*) */
	PRISM_RT_TRIANGLE_OPAQUE = 0,
	PRISM_RT_TRIANGLE_ALPHA_TESTED,
	PRISM_RT_TRIANGLE_LIGHT,
	PRISM_RT_ATTRIBUTE_WORDS = 8,
	PRISM_RT_MATERIAL_FLOATS = 16,
	PRISM_RT_MATERIALS = 4096,
	PRISM_RT_ALPHA_WORDS = 8 << 20,
	PRISM_RT_MATERIAL_FLAG_LIGHT = 1,
	PRISM_RT_MATERIAL_FLAG_ALPHA_TESTED = 2,
	PRISM_RT_MATERIAL_FLAG_GLOW = 4,
	/* the rig's floats (XGPU_RT_RIG_*, XGPU_RT_SETTING_*) */
	PRISM_RT_RIG_FLOATS = 64,
	PRISM_RT_DOMES = 4,
	PRISM_RT_RIG_SUN = 0,
	PRISM_RT_RIG_SKY = 4,
	PRISM_RT_RIG_DOMES = 8,
	PRISM_RT_RIG_SETTINGS = 8 + 4 * 8,
	PRISM_RT_SETTING_STYLE = 0,
	PRISM_RT_SETTING_ANCHOR,
	PRISM_RT_SETTING_SUN,
	PRISM_RT_SETTING_EMISSIVE,
	PRISM_RT_SETTING_EMISSIVE_SCALE,
	PRISM_RT_SETTING_MATERIALS,
	PRISM_RT_SETTING_BOUNCES,
	PRISM_RT_SETTING_EXPOSURE,
	PRISM_RT_SETTING_EXPOSURE_RANGE,
	PRISM_RT_SETTING_EXPOSURE_SPEED,
	PRISM_RT_SETTING_EXPOSURE_BIAS,
	PRISM_RT_SETTING_TONE_CURVE,
	PRISM_RT_SETTING_PAGE_TEXEL,
	PRISM_RT_SETTING_ALPHA,
	PRISM_RT_SETTING_FITTED,
	PRISM_RT_SETTING_DYNAMIC_SHADOWS,
	/* the shaders' types (shaders.c's _shader_type_*): the environment's,
	a model's; their alpha test (an environment shader's flags' bit 0) */
	PRISM_RT_SHADER_ENVIRONMENT = 3,
	PRISM_RT_SHADER_MODEL = 4,
	PRISM_RT_ENVIRONMENT_ALPHA_TESTED = 1,
	/* RT_FRAME's flags (xgpu_stream.h's XGPU_RT_FLAG_*) */
	PRISM_RT_FLAG_NO_CACHE = 1,
	PRISM_RT_FLAG_STOCK_MODELS = 2,
	PRISM_RT_FLAG_LIGHT = 4,
	PRISM_RT_FLAG_REFERENCE = 8,
	/* debug.pt_reference: its frames at most, its path's bytes
	(XGPU_RT_REFERENCE_PATH) */
	PRISM_RT_REFERENCE_FRAMES_MOST = 1 << 20,
	PRISM_RT_REFERENCE_PATH = 512,
	/* display.prism_rt_world: the level's shape traced (prism_world_geometry.c's
	_world_proxy_*: the map's, the remodel's far level, its near level) */
	PRISM_RT_WORLD_STOCK = 0,
	PRISM_RT_WORLD_FAR,
	PRISM_RT_WORLD_REMODEL,
	/* (port: PRISM v4's SCENE_STATIC, the level as the window draws it:
	the remodel's near and far levels each where drawn; "auto") */
	PRISM_RT_WORLD_DRAWN,
	/* display.prism_rt_style */
	PRISM_RT_STYLE_FAITHFUL = 0,
	PRISM_RT_STYLE_REMIX,
	/* the rig's emitters at most, a shader's name's length */
	PRISM_RT_RIG_EMITTERS = 96,
	PRISM_RT_NAME = 160,
};

/* the share of a sunlit lightmap's light the moving casters take away (the
level's own shadows are in its lightmap), and of a model's level light,
where the BSP has no rig fitted and the surfaces read a factor alone (half
resolution); as PRISM's cascaded shadows took them (prism-world2's
PRISM_SHADOWS_STRENGTH, _MODEL_STRENGTH) */
#define PRISM_RT_SUN_STRENGTH 0.65f
#define PRISM_RT_MODEL_STRENGTH 0.55f

/* the rig where the BSP has none fitted (or the fit found the group gave
none of its light, an indoor BSP's sun): the fits' typical scales
(work/relight's BSPs: the sun's 1.2 to 2.9, the sky's 3.6 to 4.7, the
lamps' 0.3 to 0.6) */
#define PRISM_RT_DEFAULT_SUN 1.3f
#define PRISM_RT_DEFAULT_SKY 3.6f
#define PRISM_RT_DEFAULT_EMIT 0.5f
/* port: the rig in light units (display.prism_rig "units", PRISM v4's: no
map's fit, the same numbers for every map, custom ones included; docs/
prism_bake.md, "The light's units"). CE's lightmap holds the light its
radiosity found as is (linear, 1.0 the 8-bit page's white: Tool's
radiosity stops on a residual in 1/255 steps), and each light's tag gives
a colour by a power; these turn each kind of light's power into the bake's
unit (a white surface's radiance: its irradiance over pi), as Tool's
radiosity takes them. The sun's and the sky's other lights' colour by power
is the light they give a white surface facing them, CE's page's 1 its white
(pi: the rig's sun is divided by pi below; b30's pages are at white where
the sun is on them); a light card's power its radiance (1); a fixture's
intensity the irradiance it gives a surface facing it a world unit away,
inverse square (1); the sky's ambient a flat floor of light, colour by
power, unoccluded and not bounced, its outdoor values in the structure's
clusters with a sky and sky 0's indoor values in those with none (1:
docs/prism_bake.md, "The light's units") */
#define PRISM_RT_UNITS_SUN 3.14159265f
#define PRISM_RT_UNITS_SKY 1.0f
#define PRISM_RT_UNITS_EMIT 1.0f
#define PRISM_RT_UNITS_FIXTURE 1.0f
/* the real sun's angular diameter (display.prism_sun_size "real", and
"auto"'s least), "soft"'s (the bake's mask, the traces' before) */
#define PRISM_RT_SUN_REAL 0.0093f
#define PRISM_RT_PI 3.14159265f
#define PRISM_RT_SUN_SOFT 0.04f
/* the light the albedo is held within (relight_bake.py's: its bounces' mean
base maps) and the alpha maps' largest side */
#define PRISM_RT_ALBEDO_LEAST 0.03f
#define PRISM_RT_ALBEDO_MOST 0.85f
#define PRISM_RT_ALPHA_SIDE 256

/* RT_FRAME's body (xgpu_stream.h's struct xgpu_stream_rt_frame, field for
field) */
struct prism_rt_frame
{
	unsigned int header;
	unsigned int mode, quality, resolution, denoiser;
	unsigned int reflections, objects, rate, mood;
	unsigned int view, light_count, frame, ticks;
	float fraction;
	unsigned int generation;
	unsigned int flags, reference_file;
	float settings[16];
	float sun[4];
	float sun_color[4];
	float sky[4];
	float spare[4];
	float rig[PRISM_RT_RIG_FLOATS];
	float lights[PRISM_RT_LIGHTS][12];
};

/* a BSP's fitted scales (relight_calib.py's scales.json) */
struct prism_rt_rig
{
	int fitted, units;
	/* (units: the fixtures' scale) */
	float fixture;
	float sun, sky, gamma;
	float sun_rgb[3], sky_rgb[3];
	long emitter_count;
	struct
	{
		char name[PRISM_RT_NAME];
		float scale;
		float rgb[3];
	} emitters[PRISM_RT_RIG_EMITTERS];
};

/* ---------- globals */

static struct
{
	unsigned long settings_read;
	int mode, quality, resolution, denoiser, reflections, objects, rate, mood, view;
	/* display.prism_rt_world (PRISM_RT_WORLD_*, auto the far level),
	display.prism_rt_cache */
	int world, cache;
	/* display.prism_rt_style, _anchor (-1: the style's), _sun (1 all),
	_emissive (-1 the style's, 0 off, 1 lights, 2 all), _emissive_scale,
	_materials (1 average), _alpha, _bounces, display.prism_sun_size (0
	auto, 1 ce, 2 real, 3 soft), display.prism_exposure (-1 the style's, 0
	fixed, 1 ce, 2 auto), _range, _speed, _bias, display.prism_tone_curve
	(-1 the style's), whether display.prism_lightmaps is "rebaked" */
	int style, anchor, sun_mode, emissive, material_tier, alpha_traced, bounces, sun_size, exposure, tone_curve,
		rebaked;
	float emissive_scale, exposure_range, exposure_speed, exposure_bias;
	/* the level's shape the structure's triangles are of: PRISM_RT_WORLD_*
	where the remodel was drawn as they were copied */
	int copied_world;
	/* the structure's triangles: the BSP they are of (-1 none), their
	generation, the copy (9 floats each), their attributes
	(PRISM_RT_ATTRIBUTE_WORDS words each), how many; the materials
	(PRISM_RT_MATERIAL_FLOATS each, by their shaders' tags) and the alpha
	maps (bytes, a word's whole); what was sent of each */
	short bsp_index;
	unsigned long generation;
	float *positions;
	unsigned long *attributes;
	long count, sent, attributes_sent;
	float *materials;
	long *material_tags;
	long material_count, materials_sent;
	unsigned char *alpha;
	long alpha_bytes, alpha_room, alpha_sent;
	int ended;
	/* port: the set is SCENE_STATIC's (prism_scene_static.c: its
	generation), not copied here */
	int scene_set;
	/* the BSP's rig, the map and BSP it is of */
	struct prism_rt_rig rig;
	char rig_map[64];
	short rig_bsp;
	/* port: display.prism_rig (0 auto, 1 units, 2 fitted) as the rig was
	read, and debug.prism_rig_scales' (the sun's, sky's, lamps' and
	fixtures' factors on the units: the calibration's) */
	int rig_mode, rig_read_units;
	float rig_scales[4];
	/* the map's sun, from its sky as a window last saw it; its angular
	diameter; the sky's other lights (domes): direction and cosine of the
	half angle, radiance by colour and power */
	short sky_index;
	int sun_found;
	real_vector3d sun;
	real_rgb_color sun_color;
	real sun_power, sun_diameter;
	/* port: the sun's test distance (its sky light's: Tool's radiosity tests
	a ray toward a sky light that far; 0 none) and each dome's (domes'
	[8]) */
	float sun_reach, dome_reaches[PRISM_RT_DOMES];
	int dome_count;
	float domes[PRISM_RT_DOMES][8];
	unsigned long frame;
	int logged;
	int initialized;
	/* debug.pt_reference: the frames summed (0 none), the file (the
	setting's, or the data folder's pt_reference/<map>_<generation>.exr),
	two copies in turn (the host reads the one a frame named while the guest
	may write the other), the one sent; the request's generation (a new
	setting, file or game time starts the sum again) and the game's time it
	began at; whether the mode's need was logged */
	long reference_frames;
	char reference_setting[PRISM_RT_REFERENCE_PATH];
	char reference_files[2][PRISM_RT_REFERENCE_PATH];
	int reference_file_index;
	unsigned long reference_generation;
	long reference_ticks;
	int reference_logged;
} prism_rt;

int prism_rt_mode(void);
int prism_rt_full_resolution(void);

/* ---------- private code */

static int word_index(char const *value, char const *const *words, int otherwise)
{
	int index;

	for (index = 0; value && words[index]; index++)
	{
		if (!strcmp(value, words[index]))
			return index;
	}
	return otherwise;
}

static void settings_read(void)
{
	static char const *const modes[] = { "off", "shadows", "gi", "pathtraced", NULL };
	static char const *const qualities[] = { "medium", "low", "high", "ultra", NULL };
	static char const *const resolutions[] = { "auto", "half", "full", NULL };
	static char const *const denoisers[] = { "spatiotemporal", "temporal", "off", NULL };
	static char const *const reflections[] = { "off", "visor", "all", NULL };
	static char const *const objects[] = { "auto", "view_lod", "low_lod", NULL };
	static char const *const rates[] = { "full", "half", "quarter", NULL };
	static char const *const views[] = { "off", "visibility", "ao", "gi", "reflections", "albedo", "normals",
		"probes", "heat", NULL };
	static char const *const worlds[] = { "stock", "far", "remodel", "drawn", "auto", NULL };
	static char const *const styles[] = { "faithful", "remix", NULL };
	static char const *const suns[] = { "objects", "all", NULL };
	static char const *const emissives[] = { "off", "lights", "all", NULL };
	static char const *const materials[] = { "grey", "average", NULL };
	static char const *const sizes[] = { "auto", "ce", "real", "soft", NULL };
	static char const *const exposures[] = { "fixed", "ce", "auto", NULL };
	static char const *const curves[] = { "ce", "filmic", "aces", "local", NULL };
	char const *lightmaps;
	char const *reference_file;
	long mood, value;

	if (!prism_rt.initialized)
	{
		prism_rt.initialized = TRUE;
		prism_rt.mood = 100;
		prism_rt.cache = 1;
		prism_rt.bsp_index = -1;
		prism_rt.rig_bsp = -2;
		prism_rt.sky_index = NONE;
	}
	if (prism_rt.settings_read == config_changes() + 1)
		return;
	prism_rt.settings_read = config_changes() + 1;
	prism_rt.mode = word_index(config_string("display.prism_rt"), modes, PRISM_RT_OFF);
	prism_rt.quality = word_index(config_string("display.prism_rt_quality"), qualities, 0);
	prism_rt.resolution = word_index(config_string("display.prism_rt_resolution"), resolutions, 0);
	prism_rt.denoiser = word_index(config_string("display.prism_rt_denoiser"), denoisers, 0);
	prism_rt.reflections = word_index(config_string("display.prism_rt_reflections"), reflections, 0);
	prism_rt.objects = word_index(config_string("display.prism_rt_objects"), objects, 0);
	prism_rt.rate = word_index(config_string("display.prism_rt_rate"), rates, 0);
	prism_rt.view = word_index(config_string("debug.prism_rt_view"), views, 0);
	prism_rt.world = word_index(config_string("display.prism_rt_world"), worlds, 4);
	if (prism_rt.world == 4)
		prism_rt.world = PRISM_RT_WORLD_DRAWN;
	/* (display.lighting "raster" with ray tracing off: the level as drawn
	is the scene the baker traces, whatever the setting) */
	if (prism_rt.mode == PRISM_RT_OFF && prism_deferred_raster())
		prism_rt.world = PRISM_RT_WORLD_DRAWN;
	prism_rt.cache = config_boolean("display.prism_rt_cache") != 0;
	mood = config_integer("display.prism_rt_mood");
	prism_rt.mood = mood < 0 ? 0 : mood > 100 ? 100 : (int)mood;
	prism_rt.style = word_index(config_string("display.prism_rt_style"), styles, PRISM_RT_STYLE_FAITHFUL);
	value = config_integer("display.prism_rt_anchor");
	prism_rt.anchor = value < 0 ? -1 : value > 100 ? 100 : (int)value;
	prism_rt.sun_mode = word_index(config_string("display.prism_rt_sun"), suns, 1);
	prism_rt.emissive = word_index(config_string("display.prism_rt_emissive"), emissives, -1);
	prism_rt.emissive_scale = (float)config_real("display.prism_rt_emissive_scale");
	if (!(prism_rt.emissive_scale >= 0.0f))
		prism_rt.emissive_scale = 1.0f;
	if (prism_rt.emissive_scale > 4.0f)
		prism_rt.emissive_scale = 4.0f;
	prism_rt.material_tier = word_index(config_string("display.prism_rt_materials"), materials, 1);
	prism_rt.alpha_traced = config_boolean("display.prism_rt_alpha") != 0;
	value = config_integer("display.prism_rt_bounces");
	prism_rt.bounces = value < 0 ? 0 : value > 4 ? 4 : (int)value;
	prism_rt.sun_size = word_index(config_string("display.prism_sun_size"), sizes, 0);
	{
		static char const *const rigs[] = { "auto", "units", "fitted", NULL };
		char const *scales = config_string("debug.prism_rig_scales");
		float read[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		int k;

		prism_rt.rig_mode = word_index(config_string("display.prism_rig"), rigs, 0);
		if (scales && *scales)
		{
			sscanf(scales, "%f %f %f %f", &read[0], &read[1], &read[2], &read[3]);
		}
		for (k = 0; k < 4; k++)
		{
			prism_rt.rig_scales[k] = read[k] >= 0.0f && read[k] < 1000.0f ? read[k] : 1.0f;
		}
	}
	prism_rt.exposure = word_index(config_string("display.prism_exposure"), exposures, -1);
	prism_rt.exposure_range = (float)config_real("display.prism_exposure_range");
	prism_rt.exposure_range = !(prism_rt.exposure_range >= 0.0f) ? 1.5f : prism_rt.exposure_range > 6.0f ? 6.0f :
		prism_rt.exposure_range;
	prism_rt.exposure_speed = (float)config_real("display.prism_exposure_speed");
	prism_rt.exposure_speed = !(prism_rt.exposure_speed > 0.0f) ? 2.0f : prism_rt.exposure_speed > 20.0f ? 20.0f :
		prism_rt.exposure_speed;
	prism_rt.exposure_bias = (float)config_real("display.prism_exposure_bias");
	prism_rt.exposure_bias = !(prism_rt.exposure_bias >= -4.0f) ? 0.0f : prism_rt.exposure_bias > 4.0f ? 4.0f :
		prism_rt.exposure_bias;
	prism_rt.tone_curve = word_index(config_string("display.prism_tone_curve"), curves, -1);
	lightmaps = config_string("display.prism_lightmaps");
	prism_rt.rebaked = lightmaps && !strcmp(lightmaps, "rebaked");
	value = config_integer("debug.pt_reference");
	value = value < 0 ? 0 : value > PRISM_RT_REFERENCE_FRAMES_MOST ? PRISM_RT_REFERENCE_FRAMES_MOST : value;
	reference_file = config_string("debug.pt_reference_file");
	if (!reference_file)
		reference_file = "";
	/* (a new request, its frames or its file: summed again) */
	if (value != prism_rt.reference_frames || strncmp(reference_file, prism_rt.reference_setting,
		sizeof(prism_rt.reference_setting)))
	{
		prism_rt.reference_frames = value;
		snprintf(prism_rt.reference_setting, sizeof(prism_rt.reference_setting), "%s", reference_file);
		prism_rt.reference_generation++;
		prism_rt.reference_ticks = game_time_get();
		prism_rt.reference_logged = FALSE;
	}
}

/* ---------- the rig: a BSP's fitted scales (relight_calib.py's
scales.json): "sun", "sky", "gamma" (numbers), "emit" ({shader: scale}) and
"rgb" ({"sun": [r, g, b], "sky": [...], "emit": {shader: [...]}}), read by
a small reader of JSON's objects, arrays, strings and numbers alone, each
number named by its path ("emit/<shader>", "rgb/emit/<shader>/2") */

struct rig_reader
{
	char const *at, *end;
	char path[4][PRISM_RT_NAME];
	int depth;
	long index[4];
	int array[4];
	struct prism_rt_rig *rig;
};

static void rig_space(struct rig_reader *reader)
{
	while (reader->at < reader->end && (*reader->at == ' ' || *reader->at == '\n' || *reader->at == '\r' ||
		*reader->at == '\t'))
	{
		reader->at++;
	}
}

/* a string at the reader (its backslashes' escapes resolved), kept to
PRISM_RT_NAME - 1 letters; FALSE where none is */
static int rig_string(struct rig_reader *reader, char *to)
{
	long length = 0;

	rig_space(reader);
	if (reader->at >= reader->end || *reader->at != '"')
		return FALSE;
	reader->at++;
	while (reader->at < reader->end && *reader->at != '"')
	{
		char letter = *reader->at++;

		if (letter == '\\' && reader->at < reader->end)
			letter = *reader->at++;
		if (length < PRISM_RT_NAME - 1)
			to[length++] = letter;
	}
	to[length] = 0;
	if (reader->at >= reader->end)
		return FALSE;
	reader->at++;
	return TRUE;
}

/* the emitter of a name in the rig (made where it is not yet), its index;
-1 where there is no room */
static long rig_emitter(struct prism_rt_rig *rig, char const *name)
{
	long index;

	for (index = 0; index < rig->emitter_count; index++)
	{
		if (!strcmp(rig->emitters[index].name, name))
			return index;
	}
	if (rig->emitter_count == PRISM_RT_RIG_EMITTERS)
		return -1;
	index = rig->emitter_count++;
	memset(&rig->emitters[index], 0, sizeof(rig->emitters[index]));
	strncpy(rig->emitters[index].name, name, PRISM_RT_NAME - 1);
	rig->emitters[index].scale = PRISM_RT_DEFAULT_EMIT;
	rig->emitters[index].rgb[0] = rig->emitters[index].rgb[1] = rig->emitters[index].rgb[2] = 1.0f;
	return index;
}

/* a number at its path */
static void rig_number(struct rig_reader *reader, float value)
{
	struct prism_rt_rig *rig = reader->rig;
	char const *a = reader->depth > 0 ? reader->path[0] : "", *b = reader->depth > 1 ? reader->path[1] : "";
	char const *c = reader->depth > 2 ? reader->path[2] : "";
	long channel = reader->index[reader->depth - 1];

	if (!(value == value) || value > 1.0e6f || value < -1.0e6f)
		return;
	if (reader->depth == 1 && !strcmp(a, "sun"))
		rig->sun = value;
	else if (reader->depth == 1 && !strcmp(a, "sky"))
		rig->sky = value;
	else if (reader->depth == 1 && !strcmp(a, "gamma"))
		rig->gamma = value;
	else if (reader->depth == 2 && !strcmp(a, "emit"))
	{
		long emitter = rig_emitter(rig, b);

		if (emitter >= 0)
			rig->emitters[emitter].scale = value;
	}
	else if (reader->depth == 2 && reader->array[1] && !strcmp(a, "rgb") && channel >= 0 && channel < 3)
	{
		if (!strcmp(b, "sun"))
			rig->sun_rgb[channel] = value;
		else if (!strcmp(b, "sky"))
			rig->sky_rgb[channel] = value;
	}
	else if (reader->depth == 3 && reader->array[2] && !strcmp(a, "rgb") && !strcmp(b, "emit") && channel >= 0 &&
		channel < 3)
	{
		long emitter = rig_emitter(rig, c);

		if (emitter >= 0)
			rig->emitters[emitter].rgb[channel] = value;
	}
}

/* a value at the reader (FALSE: not JSON as this reads it) */
static int rig_value(struct rig_reader *reader)
{
	rig_space(reader);
	if (reader->at >= reader->end)
		return FALSE;
	if (*reader->at == '{' || *reader->at == '[')
	{
		int object = *reader->at == '{';
		char close = object ? '}' : ']';

		if (reader->depth == 4)
			return FALSE;
		reader->at++;
		reader->array[reader->depth] = !object;
		reader->index[reader->depth] = 0;
		reader->path[reader->depth][0] = 0;
		reader->depth++;
		rig_space(reader);
		if (reader->at < reader->end && *reader->at == close)
		{
			reader->at++;
			reader->depth--;
			return TRUE;
		}
		for (;;)
		{
			if (object)
			{
				if (!rig_string(reader, reader->path[reader->depth - 1]))
					return FALSE;
				rig_space(reader);
				if (reader->at >= reader->end || *reader->at != ':')
					return FALSE;
				reader->at++;
			}
			if (!rig_value(reader))
				return FALSE;
			rig_space(reader);
			if (reader->at < reader->end && *reader->at == ',')
			{
				reader->at++;
				reader->index[reader->depth - 1]++;
				continue;
			}
			if (reader->at < reader->end && *reader->at == close)
			{
				reader->at++;
				reader->depth--;
				return TRUE;
			}
			return FALSE;
		}
	}
	if (*reader->at == '"')
	{
		char ignored[PRISM_RT_NAME];

		return rig_string(reader, ignored);
	}
	{
		char number[48];
		long length = 0;

		while (reader->at < reader->end && length < (long)sizeof(number) - 1 &&
			((*reader->at >= '0' && *reader->at <= '9') || *reader->at == '-' || *reader->at == '+' ||
			*reader->at == '.' || *reader->at == 'e' || *reader->at == 'E'))
		{
			number[length++] = *reader->at++;
		}
		number[length] = 0;
		if (!length)
		{
			/* (true, false, null: passed over) */
			while (reader->at < reader->end && *reader->at >= 'a' && *reader->at <= 'z')
				reader->at++;
			return TRUE;
		}
		if (reader->depth > 0)
			rig_number(reader, (float)atof(number));
		return TRUE;
	}
}

/* the BSP's rig: its scales.json, read again when the map or BSP changes;
the defaults where it has none */
static void rig_read(void)
{
	char const *map = cache_file_loaded_map_name(), *part;
	short bsp_index = global_structure_bsp_index_get();
	char name[64], path[256];
	struct prism_rt_rig *rig = &prism_rt.rig;
	FILE *file;
	long index;
	int units;

	if (!map)
		map = "";
	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
			map = part + 1;
	}
	units = prism_rt.rig_mode == 1 || (prism_rt.rig_mode == 0 && prism_deferred_raster());
	if (prism_rt.rig_bsp == bsp_index && !strcmp(prism_rt.rig_map, map) && prism_rt.rig_read_units == units)
		return;
	prism_rt.rig_read_units = units;
	prism_rt.rig_bsp = bsp_index;
	strncpy(prism_rt.rig_map, map, sizeof(prism_rt.rig_map) - 1);
	prism_rt.rig_map[sizeof(prism_rt.rig_map) - 1] = 0;
	memset(rig, 0, sizeof(*rig));
	rig->gamma = 1.0f;
	for (index = 0; index < 3; index++)
		rig->sun_rgb[index] = rig->sky_rgb[index] = 1.0f;
	/* (the name as prism_lightmaps.c makes its pages': letters, digits and
	-, the rest _) */
	for (index = 0; map[index] && index < (long)sizeof(name) - 1; index++)
	{
		char letter = map[index];

		name[index] = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
			(letter >= '0' && letter <= '9') || letter == '-' ? letter : '_';
	}
	name[index] = 0;
	/* (port: the units' rig: no file, the same for every map) */
	if (units)
	{
		rig->units = TRUE;
		rig->sun = PRISM_RT_UNITS_SUN * prism_rt.rig_scales[0];
		rig->sky = PRISM_RT_UNITS_SKY * prism_rt.rig_scales[1];
		rig->fixture = PRISM_RT_UNITS_FIXTURE * prism_rt.rig_scales[3];
		platform_log("prism rt: bsp %d's rig: the light's units (sun %.3f, sky %.3f, lamps %.3f, fixtures %.3f)",
			(int)bsp_index, rig->sun, rig->sky, PRISM_RT_UNITS_EMIT * prism_rt.rig_scales[2], rig->fixture);
		{
			struct scenario *scenario = global_scenario_get();
			short sky;

			for (sky = 0; scenario && sky < scenario->sky_references.count; sky++)
			{
				byte const *tag = (byte const *)scenario_get_sky(sky);
				float const *f = tag ? (float const *)(tag + 0x38) : NULL;

				if (f)
					platform_log("prism rt: sky %d: indoor ambient %.3f %.3f %.3f x %.3f, outdoor %.3f %.3f %.3f x %.3f",
						(int)sky, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
			}
		}
		return;
	}
	/* (from the data root, as the game's fopen finds files) */
	snprintf(path, sizeof(path), "prism_lightmaps/rebaked/%s_%d.scales.json", name, (int)bsp_index);
	file = name[0] && bsp_index != NONE ? fopen(path, "rb") : NULL;
	if (file)
	{
		static char text[64 * 1024];
		size_t length = fread(text, 1, sizeof(text), file);
		struct rig_reader reader;

		fclose(file);
		memset(&reader, 0, sizeof(reader));
		reader.at = text;
		reader.end = text + length;
		reader.rig = rig;
		rig->fitted = length > 0 && length < sizeof(text) && rig_value(&reader);
		if (!rig->fitted)
		{
			memset(rig, 0, sizeof(*rig));
			rig->gamma = 1.0f;
			for (index = 0; index < 3; index++)
				rig->sun_rgb[index] = rig->sky_rgb[index] = 1.0f;
			platform_log("prism rt: %s is not a relight fit's scales: the default rig", path);
		}
	}
	/* (a group the fit found gave none of the BSP's light, an indoor BSP's
	sun: the defaults, so that light reaching in through a window has some) */
	if (!(rig->sun > 1.0e-3f))
		rig->sun = PRISM_RT_DEFAULT_SUN;
	if (!(rig->sky > 1.0e-3f))
		rig->sky = PRISM_RT_DEFAULT_SKY;
	if (!(rig->gamma >= 1.0f && rig->gamma <= 3.0f))
		rig->gamma = 1.0f;
	platform_log("prism rt: bsp %d's rig: %s (sun %.3f, sky %.3f, gamma %.1f, %ld lamps' scales)", (int)bsp_index,
		rig->fitted ? path : "the defaults (no relight fit)", rig->sun, rig->sky, rig->gamma, rig->emitter_count);
}

/* a shader's lamp's scale and colour gain (the fit's, else the default) */
static float rig_emitter_scale(long shader_tag, float rgb[3])
{
	char const *name = tag_get_name(shader_tag);
	long index;

	rgb[0] = rgb[1] = rgb[2] = 1.0f;
	if (prism_rt.rig.units)
	{
		return PRISM_RT_UNITS_EMIT * prism_rt.rig_scales[2];
	}
	for (index = 0; name && index < prism_rt.rig.emitter_count; index++)
	{
		if (!strcmp(prism_rt.rig.emitters[index].name, name))
		{
			memcpy(rgb, prism_rt.rig.emitters[index].rgb, sizeof(float) * 3);
			return prism_rt.rig.emitters[index].scale;
		}
	}
	return PRISM_RT_DEFAULT_EMIT;
}

/* ---------- the materials */

/* port: the bounces' albedo. In the light's units (the rig "units": PRISM
v4's) a base map's mean as stored, the byte over 255 with no curve: the
reflectance CE's frame shows (its light times the stored texel), and so the
one the light the rig is measured against (CE's lightmaps) bounced. Made
linear (a byte to the power 2.2) the light where no lamp is near came out at
0.14 to 0.43 of CE's (docs/prism_bake.md, "The light's units"). Else (the
fitted rig, PRISM RT's) made linear, as it was fitted. HALO_PRISM_ALBEDO
"stored" or "linear" overrides the rule (a diagnostic) */
int prism_rt_albedo_stored(void)
{
	char const *value = getenv("HALO_PRISM_ALBEDO");

	if (value && !strcmp(value, "stored"))
		return TRUE;
	if (value && !strcmp(value, "linear"))
		return FALSE;
	return prism_rt.rig_mode == 1 || (prism_rt.rig_mode == 0 && prism_deferred_raster());
}

/* a bitmap's first texture's mean colour (linear, or as stored where stored
is nonzero) and alpha; FALSE where it cannot be read. Its texels are loaded
as a draw loads them, and waited for (world_sdf.c's sdf_bitmap_coverage) */
static int bitmap_mean_as(long bitmap_tag, float mean[4], int stored)
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	unsigned long const *header;

	if (bitmap_tag == NONE || !(group = bitmap_group_get(bitmap_tag)) || !group->bitmaps.address ||
		group->bitmaps.count <= 0)
	{
		return FALSE;
	}
	header = texture_cache_port_load((unsigned long)bitmap_tag, 0);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (!header || !bitmap->base_address)
		return FALSE;
	return xgpu_texture_mean_color_as((unsigned int)header[3], (unsigned int)header[4],
		(unsigned char const *)bitmap->base_address, 64, mean, stored);
}

static int bitmap_mean(long bitmap_tag, float mean[4])
{
	return bitmap_mean_as(bitmap_tag, mean, 0);
}

/* ... its alpha map (at most PRISM_RT_ALPHA_SIDE a side) added to the
alpha maps: the byte it starts at, its size; FALSE where none */
static int bitmap_alpha(long bitmap_tag, float *offset, float *width, float *height)
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	unsigned long const *header;
	unsigned long w = 0, h = 0;
	long start;

	if (bitmap_tag == NONE || !(group = bitmap_group_get(bitmap_tag)) || !group->bitmaps.address ||
		group->bitmaps.count <= 0)
	{
		return FALSE;
	}
	header = texture_cache_port_load((unsigned long)bitmap_tag, 0);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (!header || !bitmap->base_address)
		return FALSE;
	start = (prism_rt.alpha_bytes + 3) & ~3L;
	if (start + PRISM_RT_ALPHA_SIDE * PRISM_RT_ALPHA_SIDE > (long)PRISM_RT_ALPHA_WORDS * 4)
		return FALSE;
	if (start + PRISM_RT_ALPHA_SIDE * PRISM_RT_ALPHA_SIDE > prism_rt.alpha_room)
	{
		long room = MAX(prism_rt.alpha_room * 2, start + PRISM_RT_ALPHA_SIDE * PRISM_RT_ALPHA_SIDE * 4);
		unsigned char *grown = realloc(prism_rt.alpha, (size_t)room);

		if (!grown)
			return FALSE;
		prism_rt.alpha = grown;
		prism_rt.alpha_room = room;
	}
	if (!xgpu_texture_alpha_map((unsigned int)header[3], (unsigned int)header[4],
		(unsigned char const *)bitmap->base_address, PRISM_RT_ALPHA_SIDE, prism_rt.alpha + start, &w, &h) || !w || !h)
	{
		return FALSE;
	}
	*offset = (float)start;
	*width = (float)w;
	*height = (float)h;
	prism_rt.alpha_bytes = start + (long)(w * h);
	return TRUE;
}

/* a detail map's factor on the albedo at its mean (its function: double
biased multiply 2d, multiply d, double biased add 1 + 2 (d - 0.5) about a
mean grey base), made linear as the texels are */
static float detail_factor(long bitmap_tag, short function)
{
	float mean[4], d, factor;

	if (bitmap_tag == NONE || !bitmap_mean_as(bitmap_tag, mean, prism_rt_albedo_stored()))
		return 1.0f;
	/* (back to the display's value for CE's own sum, then linear again; as
	stored, the display's value itself) */
	d = (mean[0] * 0.2126f + mean[1] * 0.7152f + mean[2] * 0.0722f);
	if (!prism_rt_albedo_stored())
		d = d > 0.0f ? (float)pow(d, 1.0 / 2.2) : 0.0f;
	factor = function == 1 ? d : function == 2 ? 1.0f + 2.0f * (d - 0.5f) : 2.0f * d;
	factor = PIN(factor, 0.25f, 2.0f);
	return prism_rt_albedo_stored() ? factor : (float)pow(factor, 2.2);
}

/* the shader's material made (once a set: by its tag) and its index;
NONE where it is not traced. kind: what its triangles are
(PRISM_RT_TRIANGLE_*) */
static long material_of(long shader_tag, int *kind)
{
	struct shader const *shader;
	byte const *bytes;
	float *record;
	long index;

	*kind = PRISM_RT_TRIANGLE_OPAQUE;
	if (shader_tag == NONE || !(shader = shader_definition_get(shader_tag)))
		return NONE;
	bytes = (byte const *)shader;
	for (index = 0; index < prism_rt.material_count; index++)
	{
		if (prism_rt.material_tags[index] == shader_tag)
		{
			int flags = (int)prism_rt.materials[index * PRISM_RT_MATERIAL_FLOATS + 3];

			*kind = shader->base.type != PRISM_RT_SHADER_ENVIRONMENT ? PRISM_RT_TRIANGLE_LIGHT :
				(flags & PRISM_RT_MATERIAL_FLAG_ALPHA_TESTED) ? PRISM_RT_TRIANGLE_ALPHA_TESTED : PRISM_RT_TRIANGLE_OPAQUE;
			return index;
		}
	}
	/* (the environment's surfaces; of the others, the lamps alone: a light
	card CE's radiosity lit the level from) */
	if (shader->base.type != PRISM_RT_SHADER_ENVIRONMENT && !(shader->base.radiosity.power > 0.0f))
		return NONE;
	if (prism_rt.material_count == PRISM_RT_MATERIALS)
		return NONE;
	if (!prism_rt.materials)
	{
		prism_rt.materials = malloc(PRISM_RT_MATERIALS * PRISM_RT_MATERIAL_FLOATS * sizeof(float));
		prism_rt.material_tags = malloc(PRISM_RT_MATERIALS * sizeof(long));
		if (!prism_rt.materials || !prism_rt.material_tags)
			return NONE;
	}
	index = prism_rt.material_count++;
	prism_rt.material_tags[index] = shader_tag;
	record = prism_rt.materials + index * PRISM_RT_MATERIAL_FLOATS;
	memset(record, 0, PRISM_RT_MATERIAL_FLOATS * sizeof(float));
	record[0] = record[1] = record[2] = 0.35f;
	if (shader->base.type == PRISM_RT_SHADER_ENVIRONMENT)
	{
		/* (rasterizer_xbox_environment.c's struct shader_environment_definition:
		its flags at 0x28 (bit 0: alpha-tested), the base map at 0x88, the
		detail maps' function at 0xB0 and the primary detail map at 0xB8, the
		micro detail map's function at 0xF4 and map at 0xFC; its
		self-illumination's on colours at 0x19C, 0x1D8 and 0x214 and its map
		at 0x254) */
		unsigned short flags = *(unsigned short const *)(bytes + 0x28);
		long base_map = ((struct tag_reference const *)(bytes + 0x88))->index;
		short detail_function = *(short const *)(bytes + 0xB0);
		long detail_map = ((struct tag_reference const *)(bytes + 0xB8))->index;
		short micro_function = *(short const *)(bytes + 0xF4);
		long micro_map = ((struct tag_reference const *)(bytes + 0xFC))->index;
		long glow_map = ((struct tag_reference const *)(bytes + 0x254))->index;
		float mean[4], factor;

		/* (port: in the light's units the base map's stored mean alone, as
		Tool's radiosity reflected it (its detail maps are the frame's, not the
		radiosity's), and none without a base map) */
		if (prism_rt_albedo_stored())
			record[0] = record[1] = record[2] = 0.0f;
		if (bitmap_mean_as(base_map, mean, prism_rt_albedo_stored()))
		{
			factor = prism_rt_albedo_stored() ? 1.0f :
				detail_factor(detail_map, detail_function) * detail_factor(micro_map, micro_function);
			record[0] = PIN(mean[0] * factor, PRISM_RT_ALBEDO_LEAST, PRISM_RT_ALBEDO_MOST);
			record[1] = PIN(mean[1] * factor, PRISM_RT_ALBEDO_LEAST, PRISM_RT_ALBEDO_MOST);
			record[2] = PIN(mean[2] * factor, PRISM_RT_ALBEDO_LEAST, PRISM_RT_ALBEDO_MOST);
		}
		if ((flags & PRISM_RT_ENVIRONMENT_ALPHA_TESTED) &&
			bitmap_alpha(base_map, &record[8], &record[9], &record[10]))
		{
			record[3] = (float)((int)record[3] | PRISM_RT_MATERIAL_FLAG_ALPHA_TESTED);
			/* (CE's environment's alpha test: past half) */
			record[7] = 0.5f;
			*kind = PRISM_RT_TRIANGLE_ALPHA_TESTED;
		}
		/* (its glow: the self-illumination map's channels by their on
		colours, red the primary's, green the secondary's, blue the
		plasma's) */
		if (glow_map != NONE && bitmap_mean(glow_map, mean))
		{
			real_rgb_color const *on[3];
			int channel, light;

			on[0] = (real_rgb_color const *)(bytes + 0x19C);
			on[1] = (real_rgb_color const *)(bytes + 0x1D8);
			on[2] = (real_rgb_color const *)(bytes + 0x214);
			for (channel = 0; channel < 3; channel++)
			{
				float sum = 0.0f;

				for (light = 0; light < 3; light++)
				{
					float const *color = &on[light]->red;

					sum += mean[light] * PIN(color[channel], 0.0f, 4.0f);
				}
				record[11 + channel] = sum;
			}
			if (record[11] + record[12] + record[13] > 1.0e-4f)
				record[3] = (float)((int)record[3] | PRISM_RT_MATERIAL_FLAG_GLOW);
		}
	}
	else
	{
		*kind = PRISM_RT_TRIANGLE_LIGHT;
	}
	/* (a lamp: CE's radiosity's emitted colour by its power, by the fit's
	scale and colour gain: Cycles' emission, which the fit matched to CE's
	pages) */
	if (shader->base.radiosity.power > 0.0f)
	{
		float rgb[3], scale = rig_emitter_scale(shader_tag, rgb);
		real power = shader->base.radiosity.power;

		record[4] = PIN(shader->base.radiosity.color_of_emitted_light.red, 0.0f, 1.0f) * power * scale * rgb[0];
		record[5] = PIN(shader->base.radiosity.color_of_emitted_light.green, 0.0f, 1.0f) * power * scale * rgb[1];
		record[6] = PIN(shader->base.radiosity.color_of_emitted_light.blue, 0.0f, 1.0f) * power * scale * rgb[2];
		if (record[4] + record[5] + record[6] > 0.0f && record[4] + record[5] + record[6] < 1.0e5f)
			record[3] = (float)((int)record[3] | PRISM_RT_MATERIAL_FLAG_LIGHT);
	}
	return index;
}

static void static_release(void)
{
	/* (the game's free takes no NULL) */
	if (prism_rt.positions)
		free(prism_rt.positions);
	if (prism_rt.attributes)
		free(prism_rt.attributes);
	if (prism_rt.materials)
		free(prism_rt.materials);
	if (prism_rt.material_tags)
		free(prism_rt.material_tags);
	if (prism_rt.alpha)
		free(prism_rt.alpha);
	prism_rt.positions = NULL;
	prism_rt.attributes = NULL;
	prism_rt.materials = NULL;
	prism_rt.material_tags = NULL;
	prism_rt.alpha = NULL;
	prism_rt.count = prism_rt.sent = prism_rt.attributes_sent = 0;
	prism_rt.material_count = prism_rt.materials_sent = 0;
	prism_rt.alpha_bytes = prism_rt.alpha_room = prism_rt.alpha_sent = 0;
	prism_rt.ended = 0;
	prism_rt.bsp_index = -1;
}

/* a surface's triangle: its corners' places and base map coordinates, and
its vertices' normal (the first corner's) */
static int surface_read(struct structure_material const *material, struct structure_surface const *surface,
	long vertex_size, byte const *vertices, float positions[9], float uv[6], float normal[3])
{
	int corner;

	for (corner = 0; corner < 3; corner++)
	{
		long index = (unsigned short)surface->vertex_indices[corner];
		byte const *vertex;

		if (index >= material->vertices.count)
			return FALSE;
		vertex = vertices + index * vertex_size;
		memcpy(positions + corner * 3, vertex, 3 * sizeof(float));
		/* (the compressed vertex's coordinates at 24, the uncompressed's at
		48, its normal at 12) */
		if (vertex_size == 32)
		{
			memcpy(uv + corner * 2, vertex + 24, 2 * sizeof(float));
			if (!corner)
			{
				real_vector3d unpacked = uncompress_int32_to_real_vector3d(*(unsigned long const *)(vertex + 12));

				normal[0] = unpacked.i;
				normal[1] = unpacked.j;
				normal[2] = unpacked.k;
			}
		}
		else if (vertex_size >= 56)
		{
			memcpy(uv + corner * 2, vertex + 48, 2 * sizeof(float));
			if (!corner)
				memcpy(normal, vertex + 12, 3 * sizeof(float));
		}
		else
		{
			uv[corner * 2] = uv[corner * 2 + 1] = 0.0f;
			if (!corner)
				normal[0] = normal[1] = normal[2] = 0.0f;
		}
	}
	return TRUE;
}

/* the BSP's traced triangles, copied in world space with their
attributes (FALSE: none): the environment's opaque and alpha-tested
surfaces (the map's, or the remodel's level, world: PRISM_RT_WORLD_*, of
each opaque material it remodels) and the light cards */
static int static_copy(struct structure_bsp *bsp, int world)
{
	long lightmap_index, total = 0, written = 0, pass;

	for (pass = 0; pass < 2; pass++)
	{
		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
				struct structure_lightmap);
			long material_index;

			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
					struct structure_material);
				long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
				byte const *vertices = (byte const *)material->compressed_vertex_data.address;
				long surface_index, traced;
				int kind;

				if (!vertices || vertex_size < 12 || material->vertices.count <= 0 || material->surface_count <= 0 ||
					material->first_surface_index < 0 ||
					material->first_surface_index + material->surface_count > bsp->surfaces.count ||
					(unsigned long)material->compressed_vertex_data.size <
					(unsigned long)material->vertices.count * (unsigned long)vertex_size)
				{
					continue;
				}
				traced = material_of(material->shader.index, &kind);
				if (traced == NONE || (kind == PRISM_RT_TRIANGLE_ALPHA_TESTED && !prism_rt.alpha_traced))
					continue;
				/* (the remodel's level where it is drawn and display.prism_rt_world
				traces it: prism_world_geometry.c's triangles of the material) */
				if (world != PRISM_RT_WORLD_STOCK && kind == PRISM_RT_TRIANGLE_OPAQUE)
				{
					long remodelled = prism_world_geometry_triangles((short)lightmap_index, (short)material_index,
						world == PRISM_RT_WORLD_FAR, NULL, 0);

					if (remodelled >= 0)
					{
						if (!pass)
							total += remodelled;
						else
						{
							long made = prism_world_geometry_triangles((short)lightmap_index, (short)material_index,
								world == PRISM_RT_WORLD_FAR, prism_rt.positions + written * 9, total - written), at;

							for (at = written; at < written + made; at++)
							{
								memset(prism_rt.attributes + at * PRISM_RT_ATTRIBUTE_WORDS, 0,
									PRISM_RT_ATTRIBUTE_WORDS * sizeof(unsigned long));
								prism_rt.attributes[at * PRISM_RT_ATTRIBUTE_WORDS] = (unsigned long)traced;
							}
							written += made;
						}
						continue;
					}
				}
				if (!pass)
				{
					total += material->surface_count;
					continue;
				}
				for (surface_index = 0; surface_index < material->surface_count && written < total; surface_index++)
				{
					struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
						material->first_surface_index + surface_index, struct structure_surface);
					float *to = prism_rt.positions + written * 9;
					unsigned long *attributes = prism_rt.attributes + written * PRISM_RT_ATTRIBUTE_WORDS;
					float uv[6], normal[3];

					if (!surface_read(material, surface, vertex_size, vertices, to, uv, normal))
						continue;
					/* (a lamp lights along its vertices' normal, as the bake's
					one-sided emission: its corners turned so that its face's
					normal is that way) */
					if (kind == PRISM_RT_TRIANGLE_LIGHT)
					{
						float e1[3], e2[3], face[3], swap[3];
						int axis;

						for (axis = 0; axis < 3; axis++)
						{
							e1[axis] = to[3 + axis] - to[axis];
							e2[axis] = to[6 + axis] - to[axis];
						}
						face[0] = e1[1] * e2[2] - e1[2] * e2[1];
						face[1] = e1[2] * e2[0] - e1[0] * e2[2];
						face[2] = e1[0] * e2[1] - e1[1] * e2[0];
						if (face[0] * normal[0] + face[1] * normal[1] + face[2] * normal[2] < 0.0f)
						{
							memcpy(swap, to + 3, sizeof(swap));
							memcpy(to + 3, to + 6, sizeof(swap));
							memcpy(to + 6, swap, sizeof(swap));
							swap[0] = uv[2];
							swap[1] = uv[3];
							uv[2] = uv[4];
							uv[3] = uv[5];
							uv[4] = swap[0];
							uv[5] = swap[1];
						}
					}
					attributes[0] = (unsigned long)traced | ((unsigned long)kind << 16);
					memcpy(attributes + 1, uv, sizeof(uv));
					attributes[7] = 0;
					written++;
				}
			}
		}
		if (!pass)
		{
			if (!total)
				return FALSE;
			prism_rt.positions = malloc((size_t)total * 9 * sizeof(float));
			prism_rt.attributes = malloc((size_t)total * PRISM_RT_ATTRIBUTE_WORDS * sizeof(unsigned long));
			if (!prism_rt.positions || !prism_rt.attributes)
				return FALSE;
		}
	}
	prism_rt.count = written;
	return written > 0;
}

/* the shape of the level traced now: display.prism_rt_world's where the
remodel is drawn for the BSP, else the map's */
static int world_level(void)
{
	return prism_world_geometry_drawn() ? prism_rt.world : PRISM_RT_WORLD_STOCK;
}

/* port: "drawn" (and "auto"): the level traced is SCENE_STATIC's, the level
as drawn (prism_scene_static.c sends its triangles and their attributes,
each with its traced material: prism_rt_traced_material), and the set here
is begun with it, its materials and alpha maps sent once it is whole, then
its end. FALSE where the level is copied here (the other shapes) */
static int static_scene_frame(void)
{
	unsigned long generation;

	if (prism_rt.world != PRISM_RT_WORLD_DRAWN)
	{
		if (prism_rt.scene_set)
		{
			static_release();
			prism_rt.scene_set = 0;
		}
		return FALSE;
	}
	rig_read();
	generation = prism_scene_static_generation();
	if (!prism_rt.scene_set || generation != prism_rt.generation)
	{
		static_release();
		prism_rt.scene_set = 1;
		prism_rt.generation = generation;
		prism_rt.bsp_index = global_structure_bsp_index_get();
		prism_rt.copied_world = world_level();
		if (!generation)
		{
			prism_rt.ended = 1;
			return TRUE;
		}
		halo_prism_rt_static(PRISM_RT_STATIC_BEGIN, prism_rt.generation, 0, 0, 0, NULL);
	}
	if (prism_rt.ended || !prism_scene_static_ended())
		return TRUE;
	if (prism_rt.materials_sent < prism_rt.material_count)
	{
		halo_prism_rt_static(PRISM_RT_STATIC_MATERIALS, prism_rt.generation, 0, (unsigned long)prism_rt.material_count,
			(unsigned long)prism_rt.material_count, prism_rt.materials);
		prism_rt.materials_sent = prism_rt.material_count;
		return TRUE;
	}
	if (prism_rt.alpha_sent * 4 < prism_rt.alpha_bytes)
	{
		long words = (prism_rt.alpha_bytes + 3) / 4, count = MIN(words - prism_rt.alpha_sent, PRISM_RT_SLICE);

		halo_prism_rt_static(PRISM_RT_STATIC_ALPHA, prism_rt.generation, (unsigned long)prism_rt.alpha_sent,
			(unsigned long)count, (unsigned long)words, prism_rt.alpha + prism_rt.alpha_sent * 4);
		prism_rt.alpha_sent += count;
		return TRUE;
	}
	halo_prism_rt_static(PRISM_RT_STATIC_END, prism_rt.generation, 0, 0, 0, NULL);
	prism_rt.ended = 1;
	platform_log("prism rt: bsp %d: the level as drawn (scene set %lu), %ld materials, %ld KB of alpha maps",
		(int)prism_rt.bsp_index, prism_rt.generation, prism_rt.material_count, prism_rt.alpha_bytes / 1024);
	if (prism_rt.alpha)
		free(prism_rt.alpha);
	prism_rt.alpha = NULL;
	return TRUE;
}

/* the BSP drawn: its triangles copied once (again when the shape traced
or the traced kinds change: the remodel loaded, display.prism_rt_world or
_alpha set), then sent a slice a frame: the triangles, their attributes,
the materials, the alpha maps */
static void static_frame(void)
{
	static int copied_alpha = -1;
	short bsp_index = global_structure_bsp_index_get();
	int world = world_level();
	struct structure_bsp *bsp;

	if (static_scene_frame())
		return;
	rig_read();
	if (bsp_index != prism_rt.bsp_index || world != prism_rt.copied_world || prism_rt.alpha_traced != copied_alpha)
	{
		static_release();
		prism_rt.bsp_index = bsp_index;
		prism_rt.copied_world = world;
		copied_alpha = prism_rt.alpha_traced;
		prism_rt.generation++;
		bsp = bsp_index != NONE ? global_structure_bsp_get() : NULL;
		if (!bsp || !static_copy(bsp, world))
		{
			halo_prism_rt_static(PRISM_RT_STATIC_RELEASE, prism_rt.generation, 0, 0, 0, NULL);
			prism_rt.ended = 1;
			return;
		}
		platform_log("prism rt: bsp %d: %ld triangles for the acceleration structure (%s), %ld materials, %ld KB of "
			"alpha maps", (int)bsp_index, prism_rt.count, world == PRISM_RT_WORLD_FAR ? "the remodel's far level" :
			world == PRISM_RT_WORLD_REMODEL ? "the remodel" : "the map's", prism_rt.material_count,
			prism_rt.alpha_bytes / 1024);
		halo_prism_rt_static(PRISM_RT_STATIC_BEGIN, prism_rt.generation, 0, 0, (unsigned long)prism_rt.count, NULL);
	}
	if (prism_rt.ended)
		return;
	if (prism_rt.sent < prism_rt.count)
	{
		long count = MIN(prism_rt.count - prism_rt.sent, PRISM_RT_SLICE);

		halo_prism_rt_static(PRISM_RT_STATIC_TRIANGLES, prism_rt.generation, (unsigned long)prism_rt.sent,
			(unsigned long)count, (unsigned long)prism_rt.count, prism_rt.positions + prism_rt.sent * 9);
		prism_rt.sent += count;
		return;
	}
	if (prism_rt.attributes_sent < prism_rt.count)
	{
		long count = MIN(prism_rt.count - prism_rt.attributes_sent, PRISM_RT_SLICE);

		halo_prism_rt_static(PRISM_RT_STATIC_ATTRIBUTES, prism_rt.generation, (unsigned long)prism_rt.attributes_sent,
			(unsigned long)count, (unsigned long)prism_rt.count,
			prism_rt.attributes + prism_rt.attributes_sent * PRISM_RT_ATTRIBUTE_WORDS);
		prism_rt.attributes_sent += count;
		return;
	}
	if (prism_rt.materials_sent < prism_rt.material_count)
	{
		halo_prism_rt_static(PRISM_RT_STATIC_MATERIALS, prism_rt.generation, 0, (unsigned long)prism_rt.material_count,
			(unsigned long)prism_rt.material_count, prism_rt.materials);
		prism_rt.materials_sent = prism_rt.material_count;
		return;
	}
	if (prism_rt.alpha_sent * 4 < prism_rt.alpha_bytes)
	{
		long words = (prism_rt.alpha_bytes + 3) / 4, count = MIN(words - prism_rt.alpha_sent, PRISM_RT_SLICE);

		/* (the last word's bytes past the maps: whatever the room held) */
		halo_prism_rt_static(PRISM_RT_STATIC_ALPHA, prism_rt.generation, (unsigned long)prism_rt.alpha_sent,
			(unsigned long)count, (unsigned long)words, prism_rt.alpha + prism_rt.alpha_sent * 4);
		prism_rt.alpha_sent += count;
		return;
	}
	halo_prism_rt_static(PRISM_RT_STATIC_END, prism_rt.generation, 0, 0, (unsigned long)prism_rt.count, NULL);
	prism_rt.ended = 1;
	/* (the copy is the backend's now) */
	if (prism_rt.positions)
		free(prism_rt.positions);
	if (prism_rt.attributes)
		free(prism_rt.attributes);
	if (prism_rt.alpha)
		free(prism_rt.alpha);
	prism_rt.positions = NULL;
	prism_rt.attributes = NULL;
	prism_rt.alpha = NULL;
}

/* the sun of the window's sky (or the one last found for this map), and
its other lights, the domes */

/* port: a sky light's test distance (its tag's lights' field at 0x60, world
units: Tool's radiosity tests a ray toward the light that far, so that a
roof further along it does not shade it); 0 where it has none */
static float sky_light_reach(short sky_index, short light_index)
{
	byte const *sky = sky_index >= 0 ? (byte const *)scenario_get_sky(sky_index) : NULL;
	struct tag_block const *lights = sky ? (struct tag_block const *)(sky + 0xC4) : NULL;
	float reach = 0.0f;

	if (!lights || !lights->address || light_index < 0 || light_index >= lights->count)
		return 0.0f;
	memcpy(&reach, (byte const *)lights->address + light_index * 0x74 + 0x60, sizeof(reach));
	return reach > 0.0f && reach < 100000.0f ? reach : 0.0f;
}

static void sun_find(void)
{
	short sky_index = render.visible_sky_model ? render.visible_sky_index : NONE;
	short count, index;

	/* (port: the rig in the light's units: the BSP's own sky, the one most
	of its clusters have, whatever the window sees, so that the bake's rig
	is the same wherever the camera is; an indoor BSP's, the scenario's
	first) */
	if (prism_rt.rig.units)
	{
		struct structure_bsp *bsp = global_structure_bsp_get();
		struct scenario *scenario = global_scenario_get();
		short most = NONE, votes[16];

		memset(votes, 0, sizeof(votes));
		for (index = 0; bsp && bsp->clusters.address && index < bsp->clusters.count; index++)
		{
			short sky = TAG_BLOCK_GET_ELEMENT(&bsp->clusters, index, struct structure_cluster)->sky_index;

			if (sky >= 0 && sky < 16 && ++votes[sky] > (most == NONE ? 0 : votes[most]))
				most = sky;
		}
		if (most == NONE && scenario && scenario->sky_references.count > 0)
			most = 0;
		if (most != NONE)
			sky_index = most;
	}
	if (sky_index == NONE || (prism_rt.sky_index == sky_index))
		return;
	prism_rt.sky_index = sky_index;
	prism_rt.sun_found = render_sky_sun(sky_index, &prism_rt.sun, &prism_rt.sun_color, &prism_rt.sun_power);
	prism_rt.sun_diameter = 0.0f;
	prism_rt.dome_count = 0;
	prism_rt.sun_reach = 0.0f;
	count = render_sky_light_count(sky_index);
	for (index = 0; index < count; index++)
	{
		real_vector3d direction;
		real_rgb_color color;
		real power, diameter;
		float *dome;

		if (!render_sky_light(sky_index, index, &direction, &color, &power, &diameter) || !(power > 0.0f))
			continue;
		/* (the sun: its diameter kept; the others, domes) */
		if (prism_rt.sun_found && direction.i == prism_rt.sun.i && direction.j == prism_rt.sun.j &&
			direction.k == prism_rt.sun.k && power == prism_rt.sun_power)
		{
			prism_rt.sun_diameter = diameter;
			prism_rt.sun_reach = sky_light_reach(sky_index, index);
			continue;
		}
		if (prism_rt.dome_count == PRISM_RT_DOMES)
			continue;
		prism_rt.dome_reaches[prism_rt.dome_count] = sky_light_reach(sky_index, index);
		dome = prism_rt.domes[prism_rt.dome_count++];
		dome[0] = direction.i;
		dome[1] = direction.j;
		dome[2] = direction.k;
		dome[3] = (float)cos(PIN(diameter, 0.0005f, 3.0f) * 0.5f);
		dome[4] = color.red * power;
		dome[5] = color.green * power;
		dome[6] = color.blue * power;
		dome[7] = diameter;
	}
	if (prism_rt.sun_found)
		platform_log("prism rt: sky %d's sun toward (%.3f %.3f %.3f), colour %.2f %.2f %.2f, power %.2f, diameter "
			"%.4f; %d domes", (int)sky_index, prism_rt.sun.i, prism_rt.sun.j, prism_rt.sun.k, prism_rt.sun_color.red,
			prism_rt.sun_color.green, prism_rt.sun_color.blue, prism_rt.sun_power, prism_rt.sun_diameter,
			prism_rt.dome_count);
	else
		platform_log("prism rt: sky %d has no sun; %d domes", (int)sky_index, prism_rt.dome_count);
}

/* the frame's rig (RT_FRAME's rig: XGPU_RT_RIG_*) */
static void rig_frame(float *rig)
{
	struct prism_rt_rig const *scales = &prism_rt.rig;
	int remix = prism_rt.style == PRISM_RT_STYLE_REMIX, index;
	float *settings = rig + PRISM_RT_RIG_SETTINGS, diameter;

	/* (the sun's and the domes' light on a surface facing them squarely:
the bake's sun lamp's strength over pi, as Cycles' sun of strength 1
lights a white surface to 1 / pi; the sky's and the lamps' radiance as
they are) */
	if (prism_rt.sun_found)
	{
		rig[PRISM_RT_RIG_SUN + 0] = prism_rt.sun_color.red * prism_rt.sun_power * scales->sun * scales->sun_rgb[0] /
			PRISM_RT_PI;
		rig[PRISM_RT_RIG_SUN + 1] = prism_rt.sun_color.green * prism_rt.sun_power * scales->sun * scales->sun_rgb[1] /
			PRISM_RT_PI;
		rig[PRISM_RT_RIG_SUN + 2] = prism_rt.sun_color.blue * prism_rt.sun_power * scales->sun * scales->sun_rgb[2] /
			PRISM_RT_PI;
	}
	/* (display.prism_sun_size: "auto" the tag's or the real sun's, the
	wider; "ce" the tag's; "real"; "soft") */
	diameter = prism_rt.sun_diameter > 0.0f ? prism_rt.sun_diameter : 0.0035f;
	rig[PRISM_RT_RIG_SUN + 3] = prism_rt.sun_size == 1 ? diameter : prism_rt.sun_size == 2 ? PRISM_RT_SUN_REAL :
		prism_rt.sun_size == 3 ? PRISM_RT_SUN_SOFT : MAX(diameter, PRISM_RT_SUN_REAL);
	if (prism_rt.sky_index != NONE)
	{
		struct sky *sky = scenario_get_sky(prism_rt.sky_index);

		if (sky)
		{
			float ambient[3], power;

			/* (the sky's outdoor ambient radiosity: its colour at 0x48 and
			power at 0x54 of the tag) */
			memcpy(ambient, (byte const *)sky + 0x48, sizeof(ambient));
			memcpy(&power, (byte const *)sky + 0x54, sizeof(power));
			if (power >= 0.0f && power < 1000.0f)
			{
				for (index = 0; index < 3; index++)
					rig[PRISM_RT_RIG_SKY + index] = PIN(ambient[index], 0.0f, 1.0f) * power * scales->sky *
						scales->sky_rgb[index];
			}
		}
	}
	rig[PRISM_RT_RIG_SKY + 3] = scales->gamma;
	/* (port: rig[14]'s y, z and w: the floor in the structure's indoor
	clusters, sky 0's indoor ambient radiosity (its colour at 0x38, its power
	at 0x44), where the rig is in the light's units; x is the host's) */
	rig[57] = rig[58] = rig[59] = 0.0f;
	if (scales->units)
	{
		struct scenario *scenario = global_scenario_get();
		byte const *sky = scenario && scenario->sky_references.count > 0 ? (byte const *)scenario_get_sky(0) : NULL;

		if (sky)
		{
			float ambient[3], power;

			memcpy(ambient, sky + 0x38, sizeof(ambient));
			memcpy(&power, sky + 0x44, sizeof(power));
			if (power >= 0.0f && power < 1000.0f)
			{
				for (index = 0; index < 3; index++)
					rig[57 + index] = PIN(ambient[index], 0.0f, 1.0f) * power * scales->sky * scales->sky_rgb[index];
			}
		}
	}
	/* (the domes: by the sun's scale, as the fit scaled every sky light) */
	for (index = 0; index < prism_rt.dome_count; index++)
	{
		float *dome = rig + PRISM_RT_RIG_DOMES + index * 8;
		int channel;

		memcpy(dome, prism_rt.domes[index], 4 * sizeof(float));
		for (channel = 0; channel < 3; channel++)
			dome[4 + channel] = prism_rt.domes[index][4 + channel] * scales->sun * scales->sun_rgb[channel] / PRISM_RT_PI;
		/* (port: dome[7], its test distance where the rig is in the light's
		units: its rays tested that far, as Tool's; 0 the whole level) */
		dome[7] = scales->units ? prism_rt.dome_reaches[index] : 0.0f;
	}
	settings[PRISM_RT_SETTING_STYLE] = (float)prism_rt.style;
	settings[PRISM_RT_SETTING_ANCHOR] = (prism_rt.anchor >= 0 ? (float)prism_rt.anchor : remix ? 0.0f : 100.0f) /
		100.0f;
	settings[PRISM_RT_SETTING_SUN] = (float)prism_rt.sun_mode;
	settings[PRISM_RT_SETTING_EMISSIVE] = (float)(prism_rt.emissive >= 0 ? prism_rt.emissive : remix ? 2 : 1);
	settings[PRISM_RT_SETTING_EMISSIVE_SCALE] = prism_rt.emissive_scale;
	settings[PRISM_RT_SETTING_MATERIALS] = (float)prism_rt.material_tier;
	settings[PRISM_RT_SETTING_BOUNCES] = (float)prism_rt.bounces;
	/* (the style's exposure: remix's and PRISM Raster's "ce", the frame's
	middle as bright as Classic's; faithful's fixed) */
	settings[PRISM_RT_SETTING_EXPOSURE] = (float)(prism_rt.exposure >= 0 ? prism_rt.exposure :
		remix || prism_deferred_raster() ? 1 : 0);
	settings[PRISM_RT_SETTING_EXPOSURE_RANGE] = prism_rt.exposure_range;
	settings[PRISM_RT_SETTING_EXPOSURE_SPEED] = prism_rt.exposure_speed;
	settings[PRISM_RT_SETTING_EXPOSURE_BIAS] = prism_rt.exposure_bias;
	settings[PRISM_RT_SETTING_TONE_CURVE] = (float)(prism_rt.tone_curve >= 0 ? prism_rt.tone_curve : remix ? 3 : 0);
	/* (a lightmap's texel: the rebaked pages' a quarter of CE's) */
	settings[PRISM_RT_SETTING_PAGE_TEXEL] = prism_rt.rebaked ? 0.12f : 0.4f;
	settings[PRISM_RT_SETTING_ALPHA] = prism_rt.alpha_traced ? 1.0f : 0.0f;
	settings[PRISM_RT_SETTING_FITTED] = scales->fitted ? 1.0f : 0.0f;
	/* (port: rig[15]: the fixtures' scale where the rig is in units (0: the
	fixtures are CE's lightmap's, not lights), and 1 there) */
	rig[60] = scales->units ? scales->fixture : 0.0f;
	rig[61] = scales->units ? 1.0f : 0.0f;
	/* (rig[62]: 1 where the bounces' albedo is the base maps' stored mean,
	prism_rt_albedo_stored: the host's drawn objects' alike) */
	rig[62] = prism_rt_albedo_stored() ? 1.0f : 0.0f;
	/* (rig[63]: the sun's test distance where the rig is in the light's
	units: its rays tested that far, as Tool's (c40's moon: 30 world units,
	under a cliff 105 up along it); 0 the whole level) */
	rig[63] = scales->units ? prism_rt.sun_reach : 0.0f;
	settings[PRISM_RT_SETTING_DYNAMIC_SHADOWS] = remix ? 1.0f : 0.6f;
}

/* the surfaces read the traced texel as a factor and a light added
(xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT): "pathtraced" always; "shadows"
and "gi" where the sun's light is traced as a whole (display.prism_rt_sun
"all") and the texel is each pixel's */
static int light_keyed(void)
{
	if (prism_rt_mode() == PRISM_RT_PATHTRACED)
		return TRUE;
	return prism_rt_mode() != PRISM_RT_OFF && prism_rt.sun_mode == 1 && prism_rt_full_resolution();
}

/* ---------- public code */

/* the mode drawn (PRISM_RT_*: off where the renderer cannot trace) */
int prism_rt_mode(void)
{
	settings_read();
	if (prism_rt.mode == PRISM_RT_OFF || !halo_prism_rt_available())
		return PRISM_RT_OFF;
	return prism_rt.mode;
}

/* port: display.lighting "raster" (PRISM v4 M2) with ray tracing off: the
level as drawn, its materials and the rig still sent, for the baker
(prism_bake.c), nothing traced in the frame (RT_FRAME's mode off) */
static int raster_scene(void)
{
	return prism_rt_mode() == PRISM_RT_OFF && halo_prism_rt_available() && prism_deferred_raster();
}

/* port: SCENE_STATIC's traced material of a shader (prism_scene_static.c):
its index in the set's materials (made once a set), NONE where the level
traced is not SCENE_STATIC's or the shader is not traced (an alpha-tested
one without display.prism_rt_alpha) */
long prism_rt_traced_material(long shader_tag, int *kind)
{
	long index;

	*kind = PRISM_RT_TRIANGLE_OPAQUE;
	if ((prism_rt_mode() == PRISM_RT_OFF && !raster_scene()) || prism_rt.world != PRISM_RT_WORLD_DRAWN)
		return NONE;
	index = material_of(shader_tag, kind);
	if (index != NONE && *kind == PRISM_RT_TRIANGLE_ALPHA_TESTED && !prism_rt.alpha_traced)
		return NONE;
	return index;
}

/* port: what SCENE_STATIC's traced materials are of: 0 none (ray tracing
off, or its level copied here), 1 the level as drawn, 2 with its
alpha-tested surfaces */
int prism_rt_scene_traced(void)
{
	if ((prism_rt_mode() == PRISM_RT_OFF && !raster_scene()) || prism_rt.world != PRISM_RT_WORLD_DRAWN)
		return 0;
	return prism_rt.alpha_traced ? 2 : 1;
}



/* the shape of the level the pre-pass draws for the rays to start from
(prism_world_geometry.c's _world_proxy_*): the one traced, the map's where
nothing is */
short prism_rt_world_proxy(void)
{
	if (prism_rt_mode() == PRISM_RT_OFF)
		return PRISM_RT_WORLD_STOCK;
	/* ("drawn": the remodel as the window draws it, near and far, which is
	the remodel's proxy) */
	return (short)(prism_rt.world == PRISM_RT_WORLD_DRAWN ? PRISM_RT_WORLD_REMODEL : prism_rt.world);
}

/* the pre-pass's pass found at every pixel (world AO's scale 1) */
int prism_rt_full_resolution(void)
{
	if (prism_rt_mode() == PRISM_RT_OFF)
		return FALSE;
	/* ("pathtraced": always, its surfaces reading their own pixel's
	factor and added light: xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT) */
	if (prism_rt_mode() == PRISM_RT_PATHTRACED)
		return TRUE;
	/* (auto: full but at low quality) */
	return prism_rt.resolution == 2 || (prism_rt.resolution == 0 && prism_rt.quality != 1);
}

/* the sun's shadows and the objects' occlusion traced in this frame (a
mode on): the game's own shadow under each object is not drawn then
(render.c), as prism-world2's cascaded shadows leave it out, or the sand
under a Pelican is darkened twice, by CE's shadow and by the traced one.
With no sun (c10's dome, a50's), or rooms the sun does not reach, the
traced occlusion stands for it */
int prism_rt_sun_shadows(void)
{
	return prism_rt_mode() != PRISM_RT_OFF && !render_enumerating;
}

/* the surfaces read the traced light (d3d8_device.c's world_ao_key: xgpu.h's
XGPU_AMBIENT_OCCLUSION_RT_LIGHT) */
int prism_rt_light(void)
{
	return light_keyed();
}

/* debug.pt_reference: the request in the frame (path traced only), its file
named; started again if the game's time moved (the sum is of one stopped
moment: the game is stopped with game_speed 0 first) */
static void reference_frame(struct prism_rt_frame *frame)
{
	char *file;
	long ticks;

	if (prism_rt.reference_frames <= 0)
		return;
	if (prism_rt.mode != PRISM_RT_PATHTRACED)
	{
		if (!prism_rt.reference_logged)
			platform_log("prism rt: debug.pt_reference needs display.prism_rt \"pathtraced\"");
		prism_rt.reference_logged = TRUE;
		return;
	}
	ticks = game_time_get();
	if (ticks != prism_rt.reference_ticks)
	{
		prism_rt.reference_ticks = ticks;
		prism_rt.reference_generation++;
		if (!prism_rt.reference_logged)
			platform_log("prism rt: debug.pt_reference: the game's time moves, so the sum starts again each tick (stop "
				"it with game_speed 0)");
		prism_rt.reference_logged = TRUE;
	}
	/* (the copy the last frame did not name: that frame may still be read) */
	prism_rt.reference_file_index ^= 1;
	file = prism_rt.reference_files[prism_rt.reference_file_index];
	if (prism_rt.reference_setting[0])
		snprintf(file, PRISM_RT_REFERENCE_PATH, "%s", prism_rt.reference_setting);
	else
		snprintf(file, PRISM_RT_REFERENCE_PATH, "%s/pt_reference/%s_%lu.exr", platform_data_root(),
			prism_rt.rig_map[0] ? prism_rt.rig_map : "map", prism_rt.reference_generation);
	frame->flags |= PRISM_RT_FLAG_REFERENCE;
	frame->spare[0] = (float)prism_rt.reference_frames;
	frame->spare[1] = (float)(prism_rt.reference_generation & 0xffffffUL);
	frame->reference_file = (unsigned int)(unsigned long)file;
}

void prism_rt_map_unloaded(void)
{
	if (prism_rt.bsp_index != -1 || prism_rt.positions)
		halo_prism_rt_static(PRISM_RT_STATIC_RELEASE, ++prism_rt.generation, 0, 0, 0, NULL);
	static_release();
	prism_rt.sky_index = NONE;
	prism_rt.sun_found = FALSE;
	prism_rt.rig_bsp = -2;
}

/* each frame, before its windows (world_ao_frame_begin: the objects stand
where they are drawn) */
void prism_rt_frame_begin(void)
{
	struct prism_rt_frame frame;
	struct world_scene_light const *lights;
	long light_count, index;

	if ((prism_rt_mode() == PRISM_RT_OFF && !raster_scene()) || render_enumerating)
		return;
	prism_rt.frame++;
	static_frame();
	sun_find();
	memset(&frame, 0, sizeof(frame));
	/* (display.lighting "raster" alone: the scene and the rig, nothing traced) */
	frame.mode = (unsigned int)prism_rt_mode();
	frame.quality = (unsigned int)prism_rt.quality;
	frame.resolution = (unsigned int)prism_rt.resolution;
	frame.denoiser = (unsigned int)prism_rt.denoiser;
	frame.reflections = (unsigned int)prism_rt.reflections;
	frame.objects = (unsigned int)prism_rt.objects;
	frame.rate = (unsigned int)prism_rt.rate;
	frame.mood = (unsigned int)prism_rt.mood;
	frame.view = (unsigned int)prism_rt.view;
	frame.frame = (unsigned int)prism_rt.frame;
	frame.ticks = (unsigned int)game_time_get();
	frame.fraction = render_interpolation_fraction();
	frame.generation = (unsigned int)prism_rt.generation;
	frame.flags = (prism_rt.cache ? 0 : PRISM_RT_FLAG_NO_CACHE) | (prism_rt.objects == 2 ? PRISM_RT_FLAG_STOCK_MODELS : 0) |
		(light_keyed() ? PRISM_RT_FLAG_LIGHT : 0);
	reference_frame(&frame);
	frame.settings[0] = PRISM_RT_SUN_STRENGTH;
	frame.settings[1] = PRISM_RT_MODEL_STRENGTH;
	/* (the level traced is a remodel: its faces wound either way) */
	frame.settings[2] = prism_rt.copied_world != PRISM_RT_WORLD_STOCK ? 1.0f : 0.0f;
	rig_frame(frame.rig);
	/* (the sky's outdoor ambient radiosity: its colour at 0x48 and power at
0x54 of the tag, before its fogs) */
	if (prism_rt.sky_index != NONE)
	{
		struct sky *sky = scenario_get_sky(prism_rt.sky_index);

		if (sky)
		{
			memcpy(frame.sky, (byte const *)sky + 0x48, 3 * sizeof(float));
			memcpy(&frame.sky[3], (byte const *)sky + 0x54, sizeof(float));
			if (!(frame.sky[3] >= 0.0f && frame.sky[3] < 1000.0f))
				memset(frame.sky, 0, sizeof(frame.sky));
		}
	}
	if (prism_rt.sun_found)
	{
		frame.sun[0] = prism_rt.sun.i;
		frame.sun[1] = prism_rt.sun.j;
		frame.sun[2] = prism_rt.sun.k;
		frame.sun[3] = 1.0f;
		frame.sun_color[0] = prism_rt.sun_color.red;
		frame.sun_color[1] = prism_rt.sun_color.green;
		frame.sun_color[2] = prism_rt.sun_color.blue;
		frame.sun_color[3] = prism_rt.sun_power;
	}
	/* (the frame's lights, ranked: world_scene's, where world AO keeps it) */
	lights = world_scene_lights();
	light_count = lights ? world_scene_light_count() : 0;
	for (index = 0; index < light_count && frame.light_count < PRISM_RT_LIGHTS; index++)
	{
		struct world_scene_light const *light = &lights[index];
		float *to = frame.lights[frame.light_count];

		if (light->type == 2)
			continue;
		to[0] = light->position[0];
		to[1] = light->position[1];
		to[2] = light->position[2];
		to[3] = light->radius;
		to[4] = light->color[0];
		to[5] = light->color[1];
		to[6] = light->color[2];
		to[7] = light->intensity;
		/* (a point light's direction and cone are not set: none) */
		to[8] = light->type == 1 ? light->direction[0] : 0.0f;
		to[9] = light->type == 1 ? light->direction[1] : 0.0f;
		to[10] = light->type == 1 ? light->direction[2] : 0.0f;
		to[11] = light->type == 1 ? light->cone_cosine : 2.0f;
		{
			int value;

			for (value = 0; value < 12; value++)
				if (!(to[value] == to[value]) || to[value] > 1.0e30f || to[value] < -1.0e30f)
					to[value] = 0.0f;
		}
		frame.light_count++;
	}
	halo_prism_rt_frame(&frame);
	if (!prism_rt.logged)
	{
		prism_rt.logged = 1;
		platform_log("prism rt: on (mode %d, quality %d, resolution %d, denoiser %d)", prism_rt.mode, prism_rt.quality,
			prism_rt.resolution, prism_rt.denoiser);
	}
}
