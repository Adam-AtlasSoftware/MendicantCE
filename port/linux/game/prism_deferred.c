/*
PRISM_DEFERRED.C

PRISM v4's deferred core, its first step (overhaul_research/prism_v4_renderer
design.md, M1): display.lighting "ce_deferred", the design's diagnostic "CE
light, deferred". The window's opaque surfaces are drawn as the game draws
them, but CE's environment passes go to targets of their own rather than
into one another, and a resolve puts the frame together from them before
the transparent geometry, the fog and the effects are drawn over it as
before:

  the light: the sky, the objects' opaque parts, the lightmap pass (with the
    self-illumination it folds in), the objects' shadows, the diffuse
    lights and the light decals: the window's own colour target, as now;
  the base: the diffuse texture pass (base map, detail maps, micro detail:
    CE's material colour) and the decals over it (GBUFFER_PHASE_BASE), into
    an image of its own that starts white, with the light's alpha;
  the additions: the specular lights, specular lightmaps, reflection masks,
    mirrors and reflections (GBUFFER_PHASE_ADDS), into an image that starts
    black, with the base's alpha (their blend reads the base pass's
    destination alpha, CE's specular mask);
  the resolve (GBUFFER_PHASE_RESOLVE): light x base + additions, the
    additions' alpha.

That is CE's own arithmetic (the lightmap pass's light, multiplied by the
texture pass's colour, the specular and reflection passes added), so the
frame is Classic's up to rounding and the decals (drawn over the base, not
the lit colour): the equivalence gate (docs/prism_gates.md) checks that the
targets carry every surface. debug.gbuffer_view shows one of them in the
resolve's place. The objects' opaque parts are drawn lit in one pass by CE
(their light is in their combiners), so they stay in the light target: their
base colour and light apart are the next step (the uber-shaders per family).

Before CE's passes the window's opaque surfaces are drawn once more, first
and alone: the G-buffer pass (walk 1, render_gbuffer_pass, as display.
ambient_occlusion "world"'s pre-pass draws them), each draw's twin into the
G-buffer (xgpu_stream.h's GBUFFER_BEGIN: an object's base colour, its
combiners with white light; the normal; roughness, metalness and specular;
the material, its source tier and the instance), with the material of the
shader it draws (GBUFFER_DRAW) from the map's material table: every shader
of the map, once, before its first G-buffer pass (SCENE_MATERIAL, the
material ids the shaders' order in the tags: alike in every run), resolved
by the host at its automatic tier (or a mod's, where display.prism_materials
gives the shader one), and after each its maps, where a pack or a mod gives
the shader PRISM's material (SCENE_MATERIAL_MAPS, prism_materials.c: the
pack and mod tiers, which the G-buffer pass's uber-shaders draw). The level's
base colour is the base target's, or the pack's where it has one: put over
the texture pass's once that is drawn (GBUFFER_PHASE_BASE_TEXTURED), so that
the decals lie over it.

Only the primary window's view, not a mirror's nor the VR mode's eyes, and
only where the host draws it (the Vulkan backend; the others ignore the
records and draw as Classic). With "ce" (the default) no record is sent and
nothing changes.
*/

#include "cseries.h"
#include "prism_deferred.h"
#include "cache/cache_files.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "objects/objects.h"
#include "bitmaps/bitmap_group.h"
#include "structures/structure_bsp_definitions.h"
#include "render/render_cameras.h"
#include "render_cameras_internal.h"
#include "mods.h"

#include <stdlib.h>

#include <string.h>

/* prism_bake.c's: an object the bake holds as static scenery */
boolean prism_bake_object_static(long object_index);

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
unsigned long config_changes(void);
void xgpu_backend_gbuffer_phase(unsigned long phase);
void xgpu_backend_scene_material(unsigned long material, const unsigned long hash[2], int family, int material_type,
	unsigned long shader_flags, unsigned long detail_functions, unsigned long flags, const float *scalars,
	int scalar_count);
void xgpu_backend_gbuffer_draw(unsigned long material, unsigned long instance, unsigned long flags,
	unsigned long lightmap);
void xgpu_backend_scene_instance(unsigned long instance, unsigned long flags, unsigned long lod, const float *now,
	const float *last, unsigned long node_count);
/* d3d8_device.c's (the window's view's pixels are the device's) */
void halo_scene_view(short x0, short y0, short x1, short y1, float const camera[4][4]);

/* xgpu_stream.h's GBUFFER_PHASE values */
#define PHASE_BASE 1ul
#define PHASE_ADDS 2ul
#define PHASE_RESOLVE 3ul
#define PHASE_GBUFFER_BEGIN 4ul
#define PHASE_GBUFFER_END 5ul
#define PHASE_BASE_TEXTURED 6ul
#define PHASE_VIEW_SHIFT 8
#define PHASE_RASTER 0x10000ul
#define PHASE_RASTER_MATERIALS 0x20000ul
#define PHASE_RASTER_OBJECTS_VOLUME 0x40000ul
/* display.lighting "pathtraced" (xgpu_stream.h's
XGPU_GBUFFER_PHASE_PATHTRACED, from the word's top down, so that Raster's
bits, from bit 16 up, never meet it), its profile (display.pt_profile) and
its denoiser off (display.denoiser "off") */
#define PHASE_PATHTRACED 0x10000000ul
#define PHASE_PT_PROFILE_SHIFT 29
#define PHASE_PT_DENOISER_OFF 0x80000000ul
/* the views the bake's relight shows (Raster's), the path tracer's */
#define VIEW_BAKE_FIRST 12ul
#define VIEW_PT_FIRST 20ul

/* xgpu_stream.h's SCENE_MATERIAL's families, flags and scalars, and
GBUFFER_DRAW's flags */
enum
{
	FAMILY_SENV, FAMILY_SOSO, FAMILY_SGLA, FAMILY_SWAT, FAMILY_SCHI, FAMILY_SOTR, FAMILY_SMET, FAMILY_SPLA,
};
#define MATERIAL_ALPHA_TESTED 1ul
#define MATERIAL_TWO_SIDED 2ul
#define MATERIAL_BUMP_ALPHA_SPECULAR 4ul
#define MATERIAL_EMITTER 16ul
#define MATERIAL_TRANSPARENT_LAYER 32ul
#define MATERIAL_NO_OCCLUSION 64ul
#define SCALAR_BUMP_SCALE 0
#define SCALAR_SPECULAR_BRIGHTNESS 5
#define SCALAR_PERPENDICULAR_COLOR 6
#define SCALAR_PARALLEL_COLOR 9
#define SCALAR_REFLECTION_PERPENDICULAR 12
#define SCALAR_REFLECTION_PARALLEL 13
#define SCALAR_RADIOSITY_POWER 20
#define SCALAR_RADIOSITY_COLOR 21
#define SCALAR_TRANSLUCENCY 24
#define SCALAR_TINT 25
#define SCALARS 32
#define DRAW_FIRST_PERSON 4ul
/* (port: xgpu_stream.h's XGPU_GBUFFER_FLAG_STATIC_SCENERY) */
#define DRAW_STATIC_SCENERY 16ul
/* GBUFFER_DRAW's instance of the first-person weapon (xgpu_stream.h's
XGPU_GBUFFER_INSTANCE_FIRST_PERSON) */
#define INSTANCE_FIRST_PERSON 0xffffUL
/* SCENE_INSTANCE's flags: xgpu_stream.h's XGPU_SCENE_INSTANCE_IN_TLAS (port:
the object in the bake's static scenery, which the path tracer has) */
#define INSTANCE_IN_TLAS 32UL

/* the shader tags' fields the material reads, by their offsets in the tag
(struct shader: radiosity power at 0x04, its colour at 0x08, its tint at
0x14, the physics' material type at 0x22, the family's own flags at 0x28;
rasterizer_xbox_models.c's and rasterizer_xbox_environment.c's checked
offsets for the rest) */
#define SHADER_RADIOSITY_POWER 0x04
#define SHADER_RADIOSITY_COLOR 0x08
#define SHADER_TINT 0x14
#define SHADER_MATERIAL_TYPE 0x22
#define SHADER_FLAGS 0x28
#define SENV_DETAIL_FUNCTION 0xB0
#define SENV_BUMP_SCALE 0x124
#define SENV_SPECULAR_BRIGHTNESS 0x290
#define SENV_PERPENDICULAR_COLOR 0x2A8
#define SENV_PARALLEL_COLOR 0x2B4
#define SENV_REFLECTION_PERPENDICULAR 0x2F4
#define SENV_REFLECTION_PARALLEL 0x2F8
#define SOSO_TRANSLUCENCY 0x38
#define SOSO_DETAIL_FUNCTION 0xD4

/* the map's shaders by their tags' addresses: an open table of a power of
two (half full at most), the material id each (1 on, in the tags' order) */
#define MATERIALS_MOST 4096
/* SCENE_INSTANCE: the objects whose nodes are kept (MAXIMUM_OBJECTS_PER_MAP),
the nodes an instance sends (xgpu_stream.h's XGPU_SCENE_INSTANCE_NODES) */
#define INSTANCES_MOST 2048
#define INSTANCE_NODES 64
#define MATERIAL_SLOTS (MATERIALS_MOST * 2)

static struct
{
	unsigned long settings_read;
	boolean wanted, raster, raster_materials, raster_objects_volume, raster_object_shadows, pathtraced;
	unsigned long pt_profile;
	boolean pt_denoiser_off;
	unsigned long view;
	/* the window's phase sent last (0 none: the light) */
	unsigned long phase;
	/* the map the table is of (its scenario's tag and address), and the
	table */
	long scenario_index;
	void const *scenario;
	boolean table_made;
	long material_count;
	/* prism_materials.c's generation the materials' maps were sent at */
	unsigned long maps_generation;
	void const *slot_shaders[MATERIAL_SLOTS];
	unsigned short slot_materials[MATERIAL_SLOTS];
	/* the G-buffer pass's draws' instance and flags now, and the record sent
	last (its material, instance and flags; none at the pass's start) */
	unsigned long instance, draw_flags, lightmap;
	unsigned long sent_material, sent_instance, sent_flags, sent_lightmap;
	boolean sent;
	/* SCENE_INSTANCE: the G-buffer passes counted; each object's (by its
	absolute index) nodes as last sent, their count, the pass they were
	sent in and the object's datum handle (another object in the slot: no
	history) */
	unsigned long pass_number;
	float *instance_nodes[INSTANCES_MOST];
	short instance_node_counts[INSTANCES_MOST];
	unsigned long instance_passes[INSTANCES_MOST];
	long instance_handles[INSTANCES_MOST];
} prism_deferred;

static void settings_read(
	void)
{
	char const *value;

	if (prism_deferred.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_deferred.settings_read = config_changes() + 1;
	value = config_string("display.lighting");
	/* (port: "pathtraced" (M3) is lit from the bake as Raster is: its scene,
	rig and bake are Raster's, its resolve the path tracer's) */
	prism_deferred.pathtraced = value && !strcmp(value, "pathtraced");
	prism_deferred.raster = value && (!strcmp(value, "raster") || prism_deferred.pathtraced);
	prism_deferred.wanted = value && (!strcmp(value, "ce_deferred") || prism_deferred.raster);
	value = config_string("display.pt_profile");
	prism_deferred.pt_profile = value && !strcmp(value, "performance") ? 0 : 1;
	value = config_string("display.denoiser");
	prism_deferred.pt_denoiser_off = value && !strcmp(value, "off");
	/* (display.prism_raster_composition: "ce", the default, CE's shaders
	compose Raster's light; "materials" Raster's own materials the frame
	whole, for the materials milestone) */
	value = config_string("display.prism_raster_composition");
	prism_deferred.raster_materials = value && !strcmp(value, "materials");
	/* (display.prism_raster_objects: "classic", the default, an object never
	lit under the light CE gives its pixel; "volume" the irradiance volume's
	light alone) */
	value = config_string("display.prism_raster_objects");
	prism_deferred.raster_objects_volume = !value || strcmp(value, "classic");
	/* (port: display.prism_raster_object_shadows: "traced", the default, the
	moving objects' sun shadows traced against the frame's objects
	(prism_rt.c's RT_FRAME asks the renderer for their structure); "classic"
	CE's projected shadows) */
	value = config_string("display.prism_raster_object_shadows");
	prism_deferred.raster_object_shadows = !value || strcmp(value, "classic");
	value = config_string("debug.gbuffer_view");
	prism_deferred.view = !value ? 0 : !strcmp(value, "base") ? 1 : !strcmp(value, "light") ? 2 :
		!strcmp(value, "additions") ? 3 : !strcmp(value, "albedo") ? 4 : !strcmp(value, "normal") ? 5 :
		!strcmp(value, "roughness") ? 6 : !strcmp(value, "metal") ? 7 : !strcmp(value, "material_source") ? 8 :
		!strcmp(value, "instance") ? 9 : !strcmp(value, "motion") ? 10 : !strcmp(value, "emissive") ? 11 :
		!strcmp(value, "bake_light") ? 12 : !strcmp(value, "bake_sun") ? 13 : !strcmp(value, "bake_texels") ? 14 :
		!strcmp(value, "bake_volume") ? 15 : !strcmp(value, "bake_total") ? 16 :
		!strcmp(value, "raster_albedo") ? 17 : !strcmp(value, "raster_specular") ? 18 :
		!strcmp(value, "raster_diffuse") ? 19 : !strcmp(value, "pt_diffuse") ? 20 :
		!strcmp(value, "pt_specular") ? 21 : !strcmp(value, "pt_sun") ? 22 : !strcmp(value, "pt_direct") ? 23 :
		!strcmp(value, "pt_indirect") ? 24 : !strcmp(value, "pt_history") ? 25 : !strcmp(value, "pt_noisy") ? 26 : 0;
	/* (the bake's views: Raster's alone; the path tracer's its own) */
	if (prism_deferred.view >= VIEW_BAKE_FIRST && prism_deferred.view < VIEW_PT_FIRST &&
		(!prism_deferred.raster || prism_deferred.pathtraced))
	{
		prism_deferred.view = 0;
	}
	if (prism_deferred.view >= VIEW_PT_FIRST && !prism_deferred.pathtraced)
	{
		prism_deferred.view = 0;
	}

	return;
}

static unsigned long slot_of(
	void const *shader)
{
	unsigned long value = (unsigned long)shader;

	value ^= value >> 15;
	value *= 0x2c1b3c6dUL;
	value ^= value >> 12;
	return value & (MATERIAL_SLOTS - 1);
}

static float field_real(
	void const *shader,
	long offset)
{
	float value;

	memcpy(&value, (char const *)shader + offset, sizeof(value));
	return value;
}

static short field_short(
	void const *shader,
	long offset)
{
	short value;

	memcpy(&value, (char const *)shader + offset, sizeof(value));
	return value;
}

static void field_rgb(
	void const *shader,
	long offset,
	float *rgb)
{
	memcpy(rgb, (char const *)shader + offset, 3 * sizeof(float));
}

/* a shader numbered and sent (its family's group, its tag) */
static void material_add(
	long group_tag,
	long tag_index)
{
	void const *shader = tag_get(group_tag, tag_index);
	unsigned long slot, hash[2], flags = 0, detail_functions = 0, shader_flags;
	unsigned long long value = 0xcbf29ce484222325ULL;
	char const *path;
	float scalars[SCALARS];
	int family;

	if (!shader || prism_deferred.material_count >= MATERIALS_MOST - 1)
	{
		return;
	}
	for (slot = slot_of(shader); prism_deferred.slot_shaders[slot]; slot = (slot + 1) & (MATERIAL_SLOTS - 1))
	{
		if (prism_deferred.slot_shaders[slot] == shader)
		{
			return;
		}
	}
	prism_deferred.slot_shaders[slot] = shader;
	prism_deferred.slot_materials[slot] = (unsigned short)++prism_deferred.material_count;

	switch (group_tag)
	{
	case 'senv': family = FAMILY_SENV; break;
	case 'soso': family = FAMILY_SOSO; break;
	case 'sgla': family = FAMILY_SGLA; break;
	case 'swat': family = FAMILY_SWAT; break;
	case 'sotr': family = FAMILY_SOTR; break;
	case 'smet': family = FAMILY_SMET; break;
	case 'spla': family = FAMILY_SPLA; break;
	default: family = FAMILY_SCHI; break;
	}
	/* the tag path's 64-bit FNV-1a hash (xgpu_scene_path_hash's) */
	path = tag_get_name(tag_index);
	while (path && *path)
	{
		value ^= (unsigned char)*path++;
		value *= 0x100000001b3ULL;
	}
	hash[0] = (unsigned long)(value & 0xffffffffUL);
	hash[1] = (unsigned long)(value >> 32);
	memset(scalars, 0, sizeof(scalars));
	scalars[SCALAR_RADIOSITY_POWER] = field_real(shader, SHADER_RADIOSITY_POWER);
	field_rgb(shader, SHADER_RADIOSITY_COLOR, scalars + SCALAR_RADIOSITY_COLOR);
	field_rgb(shader, SHADER_TINT, scalars + SCALAR_TINT);
	shader_flags = (unsigned short)field_short(shader, SHADER_FLAGS);
	if (scalars[SCALAR_RADIOSITY_POWER] > 0.0f)
	{
		flags |= MATERIAL_EMITTER;
	}
	if (family == FAMILY_SENV)
	{
		/* (senv's flags: alpha tested, the bump map the specular mask) */
		if (shader_flags & 1)
		{
			flags |= MATERIAL_ALPHA_TESTED;
		}
		if (shader_flags & 2)
		{
			flags |= MATERIAL_BUMP_ALPHA_SPECULAR;
		}
		detail_functions = (unsigned short)field_short(shader, SENV_DETAIL_FUNCTION);
		scalars[SCALAR_BUMP_SCALE] = field_real(shader, SENV_BUMP_SCALE);
		scalars[SCALAR_SPECULAR_BRIGHTNESS] = field_real(shader, SENV_SPECULAR_BRIGHTNESS);
		field_rgb(shader, SENV_PERPENDICULAR_COLOR, scalars + SCALAR_PERPENDICULAR_COLOR);
		field_rgb(shader, SENV_PARALLEL_COLOR, scalars + SCALAR_PARALLEL_COLOR);
		scalars[SCALAR_REFLECTION_PERPENDICULAR] = field_real(shader, SENV_REFLECTION_PERPENDICULAR);
		scalars[SCALAR_REFLECTION_PARALLEL] = field_real(shader, SENV_REFLECTION_PARALLEL);
	}
	else if (family == FAMILY_SOSO)
	{
		/* (soso's flags: two-sided, not alpha tested) */
		if (shader_flags & 2)
		{
			flags |= MATERIAL_TWO_SIDED;
		}
		if (!(shader_flags & 4))
		{
			flags |= MATERIAL_ALPHA_TESTED;
		}
		detail_functions = (unsigned short)field_short(shader, SOSO_DETAIL_FUNCTION);
		scalars[SCALAR_TRANSLUCENCY] = field_real(shader, SOSO_TRANSLUCENCY);
	}
	else if (family == FAMILY_SGLA || family == FAMILY_SWAT)
	{
		flags |= MATERIAL_TRANSPARENT_LAYER;
	}
	else if (family == FAMILY_SMET || family == FAMILY_SPLA)
	{
		flags |= MATERIAL_NO_OCCLUSION;
	}
	xgpu_backend_scene_material((unsigned long)prism_deferred.material_count, hash, family,
		field_short(shader, SHADER_MATERIAL_TYPE), shader_flags, detail_functions & 0xff, flags, scalars, SCALARS);
	/* (its maps, where a pack or a mod gives it PRISM's material) */
	prism_materials_scene(shader, (unsigned long)prism_deferred.material_count, FALSE);

	return;
}

/* the map's materials' maps sent again (prism_materials.c read them again,
or the texture store answered again): every material's, the automatic
tier's where it has none now */
static void materials_maps_again(
	void)
{
	unsigned long slot;

	for (slot = 0; slot < MATERIAL_SLOTS; slot++)
	{
		if (prism_deferred.slot_shaders[slot])
		{
			prism_materials_scene(prism_deferred.slot_shaders[slot], prism_deferred.slot_materials[slot], TRUE);
		}
	}
	prism_deferred.maps_generation = prism_materials_generation();

	return;
}

/* the map's materials, numbered and sent once (again for another map) */
static void materials_make(
	void)
{
	static long const groups[] =
	{
		'senv', 'soso', 'sotr', 'schi', 'scex', 'swat', 'sgla', 'smet', 'spla',
	};
	void const *scenario = global_scenario_index != NONE ? global_scenario_get() : NULL;
	struct tag_iterator iterator;
	long tag_index;
	int group;

	if (prism_deferred.table_made && prism_deferred.scenario_index == global_scenario_index &&
		prism_deferred.scenario == scenario)
	{
		return;
	}
	memset(prism_deferred.slot_shaders, 0, sizeof(prism_deferred.slot_shaders));
	memset(prism_deferred.slot_materials, 0, sizeof(prism_deferred.slot_materials));
	prism_deferred.material_count = 0;
	prism_deferred.scenario_index = global_scenario_index;
	prism_deferred.scenario = scenario;
	prism_deferred.table_made = TRUE;
	prism_deferred.maps_generation = prism_materials_generation();
	if (!scenario)
	{
		return;
	}
	for (group = 0; group < (int)(sizeof(groups) / sizeof(groups[0])); group++)
	{
		tag_iterator_new(&iterator, groups[group]);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			material_add(groups[group], tag_index);
		}
	}

	return;
}

/* a shader's material id (0 none) */
static unsigned long material_of(
	void const *shader)
{
	unsigned long slot;

	if (!shader)
	{
		return 0;
	}
	for (slot = slot_of(shader); prism_deferred.slot_shaders[slot]; slot = (slot + 1) & (MATERIAL_SLOTS - 1))
	{
		if (prism_deferred.slot_shaders[slot] == shader)
		{
			return prism_deferred.slot_materials[slot];
		}
	}

	return 0;
}

unsigned long prism_deferred_material_id(
	void const *shader)
{
	materials_make();

	return material_of(shader);
}

/* a node's matrix as three rows of four (scaled forward, left and up as
columns, then its position: rows . (x y z 1) is a point of the node's space
in the world), as the renderer's constants and SCENE_INSTANCE have it */
static void node_rows(
	real_matrix4x3 const *matrix,
	float *rows)
{
	float scale = matrix->scale;

	rows[0] = matrix->forward.i * scale;
	rows[1] = matrix->left.i * scale;
	rows[2] = matrix->up.i * scale;
	rows[3] = matrix->position.x;
	rows[4] = matrix->forward.j * scale;
	rows[5] = matrix->left.j * scale;
	rows[6] = matrix->up.j * scale;
	rows[7] = matrix->position.y;
	rows[8] = matrix->forward.k * scale;
	rows[9] = matrix->left.k * scale;
	rows[10] = matrix->up.k * scale;
	rows[11] = matrix->position.z;
}

/* SCENE_INSTANCE for an object the G-buffer pass draws, once a pass: its
nodes as drawn (object_get_node_matrices: the pose between the ticks) and
as last sent, where that was the pass before and the same object */
static void instance_send(
	long object_index,
	unsigned long absolute)
{
	struct object_datum *object = object_try_and_get_and_verify_type(object_index, _object_mask_all);
	real_matrix4x3 const *nodes;
	float now[INSTANCE_NODES * 12];
	float *kept;
	short node_count, node;
	boolean history;

	if (!object || absolute >= INSTANCES_MOST || prism_deferred.instance_passes[absolute] == prism_deferred.pass_number)
	{
		return;
	}
	nodes = object_get_node_matrices(object_index);
	node_count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
	if (!nodes || node_count <= 0)
	{
		return;
	}
	if (node_count > INSTANCE_NODES)
	{
		node_count = INSTANCE_NODES;
	}
	for (node = 0; node < node_count; node++)
	{
		node_rows(&nodes[node], now + node * 12);
	}
	kept = prism_deferred.instance_nodes[absolute];
	history = kept && prism_deferred.instance_passes[absolute] + 1 == prism_deferred.pass_number &&
		prism_deferred.instance_handles[absolute] == object_index &&
		prism_deferred.instance_node_counts[absolute] == node_count;
	/* (port: an object the bake's static scenery holds marked, which the
	path tracer's moving objects leave out: xgpu_stream.h's
	XGPU_SCENE_INSTANCE_IN_TLAS) */
	xgpu_backend_scene_instance(absolute + 1, prism_deferred_pathtraced() && prism_bake_object_static(object_index) ?
		INSTANCE_IN_TLAS : 0, 0, now, history ? kept : NULL, (unsigned long)node_count);
	if (!kept)
	{
		kept = malloc(INSTANCE_NODES * 12 * sizeof(float));
		prism_deferred.instance_nodes[absolute] = kept;
	}
	if (kept)
	{
		memcpy(kept, now, node_count * 12 * sizeof(float));
	}
	prism_deferred.instance_node_counts[absolute] = node_count;
	prism_deferred.instance_passes[absolute] = prism_deferred.pass_number;
	prism_deferred.instance_handles[absolute] = object_index;
}

void prism_deferred_gbuffer_view(
	void const *camera_pointer,
	void const *frustum_pointer)
{
	struct render_camera const *camera = camera_pointer;
	struct render_frustum const *frustum = frustum_pointer;
	float view[4][4];
	real_point2d screen_point;
	real_vector3d view_vector, middle, step;
	short axis;

	if (!frustum->projection_valid)
	{
		return;
	}
	/* (as render_window finds world AO's pass's camera) */
	view[0][0] = camera->position.x;
	view[0][1] = camera->position.y;
	view[0][2] = camera->position.z;
	view[0][3] = frustum->z_near;
	screen_point.x = 0.0f;
	screen_point.y = 0.0f;
	render_camera_screen_to_view(camera, frustum, &screen_point, &view_vector);
	matrix4x3_transform_vector(&frustum->view_to_world, &view_vector, &middle);
	view[1][0] = middle.i;
	view[1][1] = middle.j;
	view[1][2] = middle.k;
	view[1][3] = frustum->z_far;
	for (axis = 0; axis < 2; axis++)
	{
		screen_point.x = axis == 0 ? 1.0f : 0.0f;
		screen_point.y = axis == 1 ? 1.0f : 0.0f;
		render_camera_screen_to_view(camera, frustum, &screen_point, &view_vector);
		matrix4x3_transform_vector(&frustum->view_to_world, &view_vector, &step);
		view[2 + axis][0] = step.i - middle.i;
		view[2 + axis][1] = step.j - middle.j;
		view[2 + axis][2] = step.k - middle.k;
		view[2 + axis][3] = 0.0f;
	}
	halo_scene_view(camera->viewport_bounds.x0, camera->viewport_bounds.y0, camera->viewport_bounds.x1,
		camera->viewport_bounds.y1, (float const (*)[4])view);
}

void prism_deferred_gbuffer_begin(
	void)
{
	materials_make();
	if (prism_deferred.material_count && prism_deferred.maps_generation != prism_materials_generation())
	{
		materials_maps_again();
	}
	prism_deferred.pass_number++;
	prism_deferred.instance = 0;
	prism_deferred.draw_flags = 0;
	prism_deferred.sent = FALSE;
	xgpu_backend_gbuffer_phase(PHASE_GBUFFER_BEGIN);

	return;
}

void prism_deferred_gbuffer_end(
	void)
{
	xgpu_backend_gbuffer_phase(PHASE_GBUFFER_END);

	return;
}

void prism_deferred_object(
	long object_index)
{
	unsigned long absolute;

	if (!render_gbuffer_pass)
	{
		return;
	}
	absolute = object_index == -1 ? 0 : (unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index) + 1;
	prism_deferred.instance = absolute >= INSTANCE_FIRST_PERSON ? INSTANCE_FIRST_PERSON - 1 : absolute;
	/* (port: static scenery, whose shadows the bake has: not among the
	objects whose sun shadows PRISM Raster traces) */
	prism_deferred.draw_flags = absolute && prism_deferred.raster && prism_deferred.raster_object_shadows &&
		prism_bake_object_static(object_index) ? DRAW_STATIC_SCENERY : 0;
	prism_deferred.lightmap = 0;
	/* (SCENE_INSTANCE: the object's nodes, before its draws) */
	if (absolute)
	{
		instance_send(object_index, absolute - 1);
	}

	return;
}

void prism_deferred_first_person(
	int drawing)
{
	if (!render_gbuffer_pass)
	{
		return;
	}
	prism_deferred.instance = drawing ? INSTANCE_FIRST_PERSON : 0;
	prism_deferred.draw_flags = drawing ? DRAW_FIRST_PERSON : 0;
	prism_deferred.lightmap = 0;

	return;
}

void prism_deferred_shader(
	void const *shader)
{
	unsigned long material;

	if (!render_gbuffer_pass)
	{
		return;
	}
	material = material_of(shader);
	if (prism_deferred.sent && material == prism_deferred.sent_material &&
		prism_deferred.instance == prism_deferred.sent_instance && prism_deferred.draw_flags == prism_deferred.sent_flags &&
		prism_deferred.lightmap == prism_deferred.sent_lightmap)
	{
		return;
	}
	prism_deferred.sent = TRUE;
	prism_deferred.sent_material = material;
	prism_deferred.sent_instance = prism_deferred.instance;
	prism_deferred.sent_flags = prism_deferred.draw_flags;
	prism_deferred.sent_lightmap = prism_deferred.lightmap;
	xgpu_backend_gbuffer_draw(material, prism_deferred.instance, prism_deferred.draw_flags, prism_deferred.lightmap);

	return;
}

boolean prism_deferred_wanted(
	void)
{
	settings_read();

	return prism_deferred.wanted;
}

boolean prism_deferred_raster(
	void)
{
	settings_read();

	return prism_deferred.raster;
}

boolean prism_deferred_pathtraced(
	void)
{
	settings_read();

	return prism_deferred.pathtraced;
}

boolean prism_deferred_raster_materials(
	void)
{
	settings_read();

	/* (Raster's frame whole by its own materials: it reads SCENE_LIGHTS'
	dynamic class, which CE's composition draws by CE's own passes) */
	return prism_deferred.raster && !prism_deferred.pathtraced && prism_deferred.raster_materials;
}

boolean prism_deferred_raster_object_shadows(
	void)
{
	settings_read();

	/* (Path Traced traces every moving object itself: its own flag) */
	return prism_deferred.raster && !prism_deferred.pathtraced && prism_deferred.raster_object_shadows;
}

void prism_deferred_lightmap(
	void const *lightmap_bitmap)
{
	struct structure_bsp *bsp;
	struct bitmap_group *group;
	char const *first;

	if (!render_gbuffer_pass)
	{
		return;
	}
	/* (its page: its index among the BSP's lightmap bitmaps, + 1) */
	prism_deferred.lightmap = 0;
	bsp = global_structure_bsp_index_get() != NONE ? global_structure_bsp_get() : NULL;
	if (!lightmap_bitmap || !bsp || bsp->lightmap_group.index == NONE ||
		!(group = (struct bitmap_group *)tag_get('bitm', bsp->lightmap_group.index)) || !group->bitmaps.address)
	{
		return;
	}
	first = (char const *)group->bitmaps.address;
	if ((char const *)lightmap_bitmap >= first &&
		(char const *)lightmap_bitmap < first + group->bitmaps.count * (long)sizeof(struct bitmap_data) &&
		((char const *)lightmap_bitmap - first) % (long)sizeof(struct bitmap_data) == 0)
	{
		prism_deferred.lightmap = (unsigned long)(((char const *)lightmap_bitmap - first) /
			(long)sizeof(struct bitmap_data)) + 1;
	}

	return;
}

void prism_deferred_phase(
	short phase)
{
	unsigned long value;

	/* (a moment of the base phase, not a phase of its own: the maps' base
	colour under the decals) */
	if (phase == _prism_deferred_base_textured)
	{
		xgpu_backend_gbuffer_phase(PHASE_BASE_TEXTURED);
		return;
	}
	switch (phase)
	{
	case _prism_deferred_base:
		value = PHASE_BASE;
		break;
	case _prism_deferred_additions:
		value = PHASE_ADDS;
		break;
	default:
		value = PHASE_RESOLVE | (prism_deferred.view << PHASE_VIEW_SHIFT);
		if (prism_deferred.pathtraced)
		{
			value |= PHASE_PATHTRACED | (prism_deferred.pt_profile << PHASE_PT_PROFILE_SHIFT) |
				(prism_deferred.pt_denoiser_off ? PHASE_PT_DENOISER_OFF : 0);
		}
		else if (prism_deferred.raster)
		{
			value |= PHASE_RASTER | (prism_deferred.raster_materials ? PHASE_RASTER_MATERIALS : 0) |
				(prism_deferred.raster_objects_volume ? PHASE_RASTER_OBJECTS_VOLUME : 0);
		}
		break;
	}
	prism_deferred.phase = value;
	xgpu_backend_gbuffer_phase(value);

	return;
}
