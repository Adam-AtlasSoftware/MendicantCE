/*
PRISM_WORLD_GEOMETRY.C

PRISM's enhanced world geometry (World Geometry Enhanced): the structure
BSP's surfaces drawn from a denser mesh (the farm's remodel: bevelled edges,
rounded rock, relief), in the BSP's place on screen and nowhere else. The
mesh comes from the map's sidecar (maps_enhanced/<map>.prsx), decoded at the
BSP's load against the player's own map (prism_world_source.c,
prism_geom.c), and only where prism_world_source.c's ladder gives the map
its enhanced world: Enhanced or Path Traced lighting with the enhanced
lightmaps of that very geometry. Classic lighting never draws it.

The game keeps its BSP. Collision, physics, the AI's paths, visibility
(clusters, portals, the potentially visible sets), decals, the lights'
lightmap samples under objects and every other reader of the tag read the
tag as the map has it: nothing here writes to it. Only the drawing of the
surfaces the game has already chosen to draw changes.

The remodel is cut along the BSP's own surfaces: every triangle of it
belongs to one of the stock triangles (a surface), and the triangles of
each surface are consecutive in its material's index list. So whatever set
of surfaces a pass draws (the frame's visible ones, a light's, a shadow's),
the remodel draws exactly the triangles that stand for them, as runs of its
own index list: structure_render_pass hands each material's chunk of
surfaces to prism_world_geometry_chunk, which gives the pass the remodel's
vertices in place of the material's, and the draws of those vertices
(prism_world_geometry_draw, at the top of the two functions that draw
the structure: rasterizer_xbox_draw_primitives.c) draw the runs. Every
other step of the pass (its shaders, textures, the lightmap and its
dominant light direction per vertex, fog, the dynamic lights) is the
game's, because the remodel's vertices are the game's two environment
vertex formats: each corner carries the base map and lightmap coordinates
and incident radiosity of the point of its surface it lies over, so the
baked light lands where the stock light was.

The image (prism_geom.c builds it in memory from the sidecar's GEOM and LMUV
sections; the farm's remodel pipeline writes the same layout as its .pwg
intermediate, tools/prism_packs/art/world_geo's write_pwg), little endian:

  header, 64 bytes: "PWG1", version (1), material count, flags (0), the
    BSP tag's path (48 bytes, informational)
  each material, 64 bytes, then its data:
    lightmap index (16 bits), material index (16 bits),
    the material's stock surface count, stock vertex count,
    the remodel's vertex count (at most 65535), triangle count,
    topology check (32 bits), position check (64-bit float), the part's
    first surface of the material's and its surface count (32 bits each),
    24 bytes zero;
    surface_first_triangle[the part's surface count + 1] (32 bits each: the
      remodel's triangles of the part's surface s are [first[s],
      first[s + 1]));
    the vertices, uncompressed environment vertices (56 bytes);
    the lightmap vertices, uncompressed (20 bytes), one for each vertex;
    the triangles, three 16-bit indices each;
    each array padded to 4 bytes.

Version 2 (flags bit 0) adds a far level of detail: the record's bytes 40
to 47 are the part's near triangle count (its first triangles; the rest
are the far ones) and the distance (world units, a float) beyond which a
surface is drawn far; after the triangles come, for each of the part's
surfaces, the first and count of its far triangles (32 bits each: a range
of the part's triangles, which may be its near ones) and its bounding
sphere (centre and radius, 4 floats). A far surface keeps every vertex its
near triangles have on the stock triangle's edges, and every edge two
surfaces share is the same vertices on both sides (tools/prism_packs/art/
world_geo/wg2_seal.py, then wg2_far2.py), so near and far surfaces side by
side meet without a crack whichever level each is drawn at: a T-junction, or
a far level's chord against a neighbour's edge, leaves a line of the sky or
the level behind showing through (debug.crack_view finds them).
The camera's distance to the sphere, less its radius, scaled by the view's
zoom (the vertical field of view against 70 degrees), chooses: the same for
every pass of the frame, so the z-equal passes agree. Against pop-in
(prism_popin.c): display.prism_world_geometry_band gives each surface its
own distance in a band past that one (by its number), so that the surfaces
at one distance switch one by one as the camera moves, not as a ring in one
frame; display.prism_lod_hysteresis keeps a surface drawn near so much
further again before it goes far (each surface's level kept from the
camera before), so that a camera at the distance does not switch it back
and forth. Neither draws a surface far nearer than the file's distance.

A material whose remodel has more than 65535 vertices (the 16-bit indices)
comes in parts, records one after another, each of a run of its surfaces in
order; a surface's triangles are all in one part.

The checks are of the map's own material: the topology check is the sum
over its surfaces s of (3 a + 5 b + 7 c) (s + 1), modulo 2^32, where a, b,
c are its vertex indices; the position check is the sum over its vertices
of x + 2 y + 3 z. A material whose counts or checks differ (another map
revision, a modded map) stays stock, and the log says so; so does one whose
shader is transparent. The file is untrusted input: every count, offset
and index is checked against what was read.

Memory: the remodel's vertices and triangles, compressed by the game's own
rasterizer_geometry_compress_vertices, sit in the Xbox's contiguous memory
as the map's geometry does, each array inside one of the renderer's mirror
segments (MIRROR_SEGMENT_BYTES), so that the GPU draws them from its copy
and nothing is streamed per draw. They are freed when the BSP changes and
with the map.

What it costs. The passes before the surfaces do not draw the remodel as
the surfaces are drawn: the sun's shadow maps (render_prism_shadow_pass,
three cascades, both faces) draw every surface's far level
(display.prism_world_geometry_shadows "far"; "remodel" or "stock" the
map's triangles), and the world occlusion's pre-pass
(render_world_ao_prepass), whose level gives only where objects darken it
and what hides them, the surfaces as drawn
(display.prism_world_geometry_occlusion "remodel", by default: a
pixel reads its own surface's occlusion; "far", "stock" cheaper). That is
Classic lighting's, as it always was. Port: in PRISM Rebaked and Path
Traced every pass draws the geometry drawn (one geometry per surface: no
surface shadowed or occluded by another shape than its own); there those
settings' "far" and "stock" are debug overrides, drawn only with
debug.geometry_overrides (logged once). Each
surface's near and far levels have a cone of facings (the bounds' sphere,
the mean of the triangles' facings and the least cosine to it, made as the
file loads): a level that faces away from the camera whole is skipped in
the camera's passes (display.prism_world_geometry_facing), as
structure_culling.c skips the stock triangles it cannot, or drawn through
where it is a few triangles between two drawn (MAXIMUM_FILLED_TRIANGLES:
the GPU culls them all). The choices of near or far and facing are found
once a camera (a stamp), not once a pass. And a part's runs are drawn as
one draw, their indices copied one after another
(display.prism_world_geometry_batch). display.prism_world_geometry_detail
scales the far distance.

Off ("stock"), prism_world_geometry_frame reads the setting and returns,
prism_world_geometry_chunk returns the material's own vertices and
prism_world_geometry_draw returns FALSE: the game draws as before, byte for
byte.
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "errors.h"
#include "tag_files/tag_groups.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "structures/structure_bsp_definitions.h"
#include "render/render.h"
#include "structure_culling.h"
#include "prism_world_geometry.h"
#include "prism_popin.h"
#include "prism_scene_static.h"
#include "prism_deferred.h"
#include "prism_world_source.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* (the C library's allocator: the file and the tables are ours alone) */
#undef malloc
#undef free

/* the game's (port) */
char *tag_get_name(long tag_index);
/* prism_rt.c's: the shape of the level display.prism_rt traces (_world_proxy_*:
stock where it traces none, or the map's) */
short prism_rt_world_proxy(void);
/* the platform's (port/linux/src) */
char const *config_string(char const *name);
int config_boolean(char const *name);
long config_integer(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
/* (rasterizer_xbox.h) */
extern D3DDevice *global_d3d_device;

/* ---------- constants */

#define PWG_MAGIC "PWG1"
#define PWG_VERSION 2
#define PWG_FLAG_FAR 1
#define PWG_HEADER_SIZE 64
#define PWG_MATERIAL_SIZE 64

#define ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE 56
#define ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE 20

/* the most of the Xbox's contiguous memory a BSP's remodel takes (of the
port's 512 MB: platform.h's PLATFORM_CONTIGUOUS_SIZE) */
#define MAXIMUM_CONTIGUOUS_BYTES (176UL * 1024 * 1024)
/* the most surfaces one chunk names (render.h's visible structure surfaces,
and the lights' and shadows' MAXIMUM_LOCALLY_RENDERED_SURFACES) */
#define MAXIMUM_CHUNK_SURFACES 32768
/* as rasterizer_xbox_draw_primitives.c's
RASTERIZER_MAXIMUM_PRIMITIVES_PER_DRAW_COMMAND */
#define MAXIMUM_TRIANGLES_PER_DRAW 10000
/* port/linux/src/xgpu_backend.h's XGPU_MIRROR_SEGMENT_SIZE, as
mod_models.c's MIRROR_SEGMENT_BYTES */
#define MIRROR_SEGMENT_BYTES 0x400000UL
/* the frames between the log's lines of what was drawn */
#define STATISTICS_FRAMES 1800
/* the most triangles of surfaces skipped for facing away that a draw goes
on through rather than end there and start again after them (the GPU culls
every one of them: a draw fewer for some vertices more) */
#define MAXIMUM_FILLED_TRIANGLES 384
/* display.prism_world_geometry_detail's bounds (percent) */
#define MINIMUM_DETAIL 25
#define MAXIMUM_DETAIL 400

/* the shape a pass before the surfaces draws the level from
(display.prism_world_geometry_shadows and _occlusion) */
enum
{
	_world_proxy_stock,
	_world_proxy_far,
	_world_proxy_remodel
};

/* a surface's choices for the camera (world_material's choices) */
enum
{
	_surface_choice_far_bit,
	_surface_choice_near_away_bit,
	_surface_choice_far_away_bit
};

/* ---------- structures */

/* a Direct3D resource as every resource starts (D3DResource_Register):
Common, Data, Lock */
struct world_resource
{
	unsigned long common;
	unsigned long data;
	unsigned long lock;
};

struct world_material
{
	/* the remodel's vertices and lightmap vertices, side by side as a
	structure_material's are (the passes draw `vertices + 1` as the
	lightmap's) */
	struct vertex_buffer buffers[2];
	struct world_resource vertex_resources[2];
	struct world_resource index_resource;
	short lightmap_index;
	short material_index;
	/* the material's first surface in the BSP; this part's surfaces, from
	its first, of the material's; and on a material's first part, how many
	parts it has (the next ones follow it) */
	long first_surface_index;
	long part_first_surface;
	long surface_count;
	long part_count;
	long triangle_count;
	unsigned long *surface_first_triangle;
	/* the far level (version 2), NULL none: each surface's far triangles
	(first, count) and bounding sphere (x, y, z, radius); the distance */
	unsigned long *far_triangles;
	float *bounds;
	float far_distance;
	/* each surface's cones of facings (its near triangles', then its far
	ones'; NULL none: never skipped for facing away), and its choices for
	the camera (_surface_choice_*) with the camera's stamp they are of */
	struct structure_culling_cone *cones;
	unsigned long *choice_stamps;
	byte *choices;
	/* each surface's level drawn last (display.prism_lod_hysteresis: 0
	near, 1 far, 2 none yet), with the far level; port: the frame it last
	switched (HALO_GEOMETRY_LOG's near/far switches and flips) */
	byte *far_state;
	unsigned long *far_switched;
};

struct world_run
{
	long part;
	long first_triangle;
	long triangle_count;
};

struct prism_world_geometry_globals
{
	boolean enabled;
	unsigned long settings_read;

	/* the BSP the remodel is of (its tag, and where its tags are), and the
	BSP last tried and found without one */
	long loaded_tag_index;
	struct structure_bsp *loaded_bsp;
	long tried_tag_index;
	struct structure_bsp *tried_bsp;

	struct world_material *materials;
	long material_count;
	/* lightmap index -> its first entry in lookup; lookup -> the material
	of the remodel or NONE */
	long *lightmap_first;
	short *lookup;
	long lightmap_count;
	byte *geometry;

	/* the chunk prism_world_geometry_chunk gave last, and its runs */
	struct world_material *current;
	struct world_run *runs;
	long run_count;

	/* the camera of the frame (structure_render_preprocess's), and the
	zoom's scale of the far levels' distances (with the detail setting's);
	the camera's radius (VR's eyes); the stamp of the surfaces' choices,
	new whenever any of these changes */
	real_point3d camera;
	float distance_scale;
	real camera_radius;
	unsigned long stamp;

	/* display.prism_world_geometry_shadows and _occlusion (_world_proxy_*),
	_facing, and _detail as a scale */
	short shadow_proxy;
	short occlusion_proxy;
	boolean facing;
	boolean batch;
	float detail;
	/* display.prism_lod_hysteresis and _world_geometry_band, as fractions */
	float hysteresis;
	float band;

	/* display.prism_world_geometry_batch: a part's runs copied one after
	another into this, and drawn as one (the front end streams it) */
	word *batch_indices;
	unsigned long batch_capacity;
	struct world_resource batch_resource;

	/* what was drawn (the log, every STATISTICS_FRAMES frames) */
	unsigned long frames;
	unsigned long draws;
	unsigned long triangles;
	unsigned long stock_triangles;
	unsigned long far_surfaces;
	unsigned long surfaces;
	unsigned long away_surfaces;
	unsigned long filled_triangles;
	unsigned long batched_runs;
	/* port: HALO_GEOMETRY_LOG (0 none, 1 a summary each 600 frames, 2 each
	flip too): the surfaces' near/far switches and flips (a switch undone
	within GEOMETRY_FLIP_FRAMES), all told, by the frames counted here */
	int geometry_log;
	unsigned long geometry_frame;
	unsigned long near_far_switches, near_far_flips;
	/* the camera at the frame's first window, and the last frame it moved
	more than GEOMETRY_CUT_UNITS in a frame (a cut: a switch before it undone
	after is no flip), and such cuts */
	real_point3d geometry_camera;
	unsigned long cut_frame, cuts;
};

/* ---------- globals */

static struct prism_world_geometry_globals world_geometry =
{
	FALSE, 0,
	NONE, NULL, NONE, NULL,
};

/* ---------- private code */

static unsigned long read_u32(
	byte const *bytes)
{
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8) |
		((unsigned long)bytes[2] << 16) | ((unsigned long)bytes[3] << 24);
}

static short read_s16(
	byte const *bytes)
{
	return (short)((unsigned short)bytes[0] | ((unsigned short)bytes[1] << 8));
}

static double read_f64(
	byte const *bytes)
{
	double value;

	memcpy(&value, bytes, sizeof(value));
	return value;
}

static unsigned long pad4(
	unsigned long size)
{
	return (size + 3) & ~3UL;
}

/* where a block of bytes goes at offset in memory whose start is aligned
to a mirror segment: there, or the next segment's start if it would span
two (mod_models.c's segment_place) */
static unsigned long segment_place(
	unsigned long offset,
	unsigned long bytes)
{
	/* (every array at 32 bytes: the vertices' attributes must start on
	their components' size, which an odd count of 16-bit triangles before
	them would break) */
	offset = (offset + 31) & ~31UL;
	if (bytes && bytes <= MIRROR_SEGMENT_BYTES &&
		offset / MIRROR_SEGMENT_BYTES != (offset + bytes - 1) / MIRROR_SEGMENT_BYTES)
	{
		offset = (offset / MIRROR_SEGMENT_BYTES + 1) * MIRROR_SEGMENT_BYTES;
	}

	return offset;
}

/* a pass's shape. Classic lighting: the setting's, as it always was
("stock", "far" or "remodel"; "drawn" is "remodel"; else the fallback). PRISM
Rebaked and Path Traced (one geometry for every pass): as drawn, "far" or
"stock" with debug.geometry_overrides only (else said once and drawn) */
static short proxy_setting(
	char const *name,
	short fallback,
	boolean overrides,
	boolean *told)
{
	char const *value = config_string(name);
	short proxy = !value ? fallback : !strcmp(value, "stock") ? _world_proxy_stock :
		!strcmp(value, "far") ? _world_proxy_far : !strcmp(value, "remodel") || !strcmp(value, "drawn") ?
		_world_proxy_remodel : fallback;

	if (!prism_deferred_raster())
	{
		return proxy;
	}
	if (proxy != _world_proxy_remodel && !overrides)
	{
		if (!*told)
		{
			platform_log("prism world geometry: %s \"%s\" is a debug override (debug.geometry_overrides): the pass "
				"draws the geometry drawn", name, value);
			*told = TRUE;
		}
		return _world_proxy_remodel;
	}
	if (proxy == _world_proxy_remodel)
	{
		*told = FALSE;
	}
	return proxy;
}

/* whether the enhanced geometry is drawn (prism_world_source.c's ladder:
the map latched to it and the lighting Enhanced or Path Traced), asked each
time: the map's choice changes with no setting changed */
static void enabled_update(
	void)
{
	boolean enabled = prism_world_enhanced();

	if (enabled != world_geometry.enabled)
	{
		/* (turned on again: a BSP that had no remodel is looked for again) */
		world_geometry.tried_tag_index = NONE;
		world_geometry.tried_bsp = NULL;
	}
	world_geometry.enabled = enabled;
}

static void settings_read(
	void)
{
	long detail;

	enabled_update();
	if (world_geometry.settings_read == config_changes() + 1)
	{
		return;
	}
	world_geometry.settings_read = config_changes() + 1;
	{
		static boolean told_shadows, told_occlusion;
		boolean overrides = config_boolean("debug.geometry_overrides") != 0;

		world_geometry.shadow_proxy = proxy_setting("display.prism_world_geometry_shadows", _world_proxy_far, overrides,
			&told_shadows);
		world_geometry.occlusion_proxy = proxy_setting("display.prism_world_geometry_occlusion", _world_proxy_remodel,
			overrides, &told_occlusion);
	}
	/* (port: display.prism_rt traces the remodel's level, prism_rt.c's
	world_level: the pre-pass its rays start from draws the same, so that
	they leave the surface they trace, not one inside or above it) */
	{
		short traced = prism_rt_world_proxy();

		if (traced > world_geometry.occlusion_proxy)
		{
			world_geometry.occlusion_proxy = traced;
		}
	}
	world_geometry.facing = config_boolean("display.prism_world_geometry_facing") != 0;
	world_geometry.batch = config_boolean("display.prism_world_geometry_batch") != 0;
	detail = config_integer("display.prism_world_geometry_detail");
	detail = detail < MINIMUM_DETAIL ? MINIMUM_DETAIL : detail > MAXIMUM_DETAIL ? MAXIMUM_DETAIL : detail;
	world_geometry.detail = (float)detail / 100.0f;
	world_geometry.hysteresis = prism_popin_hysteresis();
	world_geometry.band = prism_popin_band();

	return;
}

static void world_geometry_release(
	void)
{
	if (world_geometry.geometry)
	{
		XPhysicalFree(world_geometry.geometry);
	}
	if (world_geometry.materials)
	{
		long index;

		for (index = 0; index < world_geometry.material_count; index++)
		{
			free(world_geometry.materials[index].surface_first_triangle);
			free(world_geometry.materials[index].far_triangles);
			free(world_geometry.materials[index].bounds);
			free(world_geometry.materials[index].cones);
			free(world_geometry.materials[index].choice_stamps);
			free(world_geometry.materials[index].choices);
			free(world_geometry.materials[index].far_state);
			free(world_geometry.materials[index].far_switched);
		}
	}
	free(world_geometry.materials);
	free(world_geometry.lightmap_first);
	free(world_geometry.lookup);
	free(world_geometry.runs);
	if (world_geometry.batch_indices)
	{
		XPhysicalFree(world_geometry.batch_indices);
	}
	world_geometry.batch_indices = NULL;
	world_geometry.batch_capacity = 0;
	world_geometry.geometry = NULL;
	world_geometry.materials = NULL;
	world_geometry.material_count = 0;
	world_geometry.lightmap_first = NULL;
	world_geometry.lookup = NULL;
	world_geometry.lightmap_count = 0;
	world_geometry.runs = NULL;
	world_geometry.run_count = 0;
	world_geometry.current = NULL;
	world_geometry.loaded_tag_index = NONE;
	world_geometry.loaded_bsp = NULL;

	return;
}

/* the checks of the map's own material (the file's header comment) */
static boolean material_stock_checks(
	struct structure_bsp *bsp,
	struct structure_material const *material,
	unsigned long *topology,
	double *positions)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
	byte const *vertices = (byte const *)material->compressed_vertex_data.address;
	unsigned long sum = 0;
	double position_sum = 0.0;
	long index;

	if (!vertices || vertex_size < 12 || material->vertices.count <= 0 ||
		material->first_surface_index < 0 || material->surface_count <= 0 ||
		material->first_surface_index + material->surface_count > bsp->surfaces.count ||
		(unsigned long)material->compressed_vertex_data.size < (unsigned long)material->vertices.count * (unsigned long)vertex_size)
	{
		return FALSE;
	}
	for (index = 0; index < material->surface_count; index++)
	{
		struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(
			&bsp->surfaces,
			material->first_surface_index + index,
			struct structure_surface);

		sum += (3UL * (unsigned short)surface->vertex_indices[0] + 5UL * (unsigned short)surface->vertex_indices[1] +
			7UL * (unsigned short)surface->vertex_indices[2]) * (unsigned long)(index + 1);
	}
	for (index = 0; index < material->vertices.count; index++)
	{
		float point[3];

		memcpy(point, vertices + index * vertex_size, sizeof(point));
		position_sum += (double)point[0] + 2.0 * (double)point[1] + 3.0 * (double)point[2];
	}
	*topology = sum;
	*positions = position_sum;

	return TRUE;
}

struct pwg_material_view
{
	short lightmap_index;
	short material_index;
	unsigned long surface_count;
	unsigned long stock_vertex_count;
	unsigned long vertex_count;
	unsigned long triangle_count;
	unsigned long topology;
	double positions;
	unsigned long part_first_surface;
	unsigned long part_surface_count;
	byte const *surface_first_triangle;
	byte const *vertices;
	byte const *lightmap_vertices;
	byte const *triangles;
	/* version 2's far level: the near triangle count, the distance, the
	surfaces' (first, count) pairs and spheres; NULL none */
	unsigned long near_triangle_count;
	float far_distance;
	byte const *far_triangles;
	byte const *bounds;
	struct structure_material *material;
	boolean usable;
	/* the part's surfaces drawn as their stock triangles (the shape's
	checks: shape_check), by their numbers in the part, and how many */
	unsigned long *fallback;
	unsigned long fallback_count;
};

/* the file's materials read and checked against its size, the map's BSP
and themselves; FALSE if the file is not one */
static boolean pwg_parse(
	byte const *bytes,
	unsigned long size,
	struct structure_bsp *bsp,
	struct pwg_material_view **views_out,
	long *view_count_out,
	char const *path)
{
	struct pwg_material_view *views;
	unsigned long material_count;
	unsigned long offset;
	unsigned long index;
	unsigned long version;
	boolean far;

	if (size < PWG_HEADER_SIZE || memcmp(bytes, PWG_MAGIC, 4) ||
		(version = read_u32(bytes + 4)) < 1 || version > PWG_VERSION)
	{
		platform_log("prism world geometry: %s is not a remodel of version 1 to %d", path, PWG_VERSION);
		return FALSE;
	}
	far = version >= 2 && (read_u32(bytes + 12) & PWG_FLAG_FAR);
	material_count = read_u32(bytes + 8);
	if (!material_count || material_count > 4096)
	{
		platform_log("prism world geometry: %s has %lu materials", path, material_count);
		return FALSE;
	}
	views = calloc(material_count, sizeof(*views));
	if (!views)
	{
		return FALSE;
	}
	offset = PWG_HEADER_SIZE;
	for (index = 0; index < material_count; index++)
	{
		struct pwg_material_view *view = &views[index];
		byte const *record;
		unsigned long data_size;

		if (size - offset < PWG_MATERIAL_SIZE)
		{
			goto truncated;
		}
		record = bytes + offset;
		view->lightmap_index = read_s16(record);
		view->material_index = read_s16(record + 2);
		view->surface_count = read_u32(record + 4);
		view->stock_vertex_count = read_u32(record + 8);
		view->vertex_count = read_u32(record + 12);
		view->triangle_count = read_u32(record + 16);
		view->topology = read_u32(record + 20);
		view->positions = read_f64(record + 24);
		view->part_first_surface = read_u32(record + 32);
		view->part_surface_count = read_u32(record + 36);
		view->near_triangle_count = far ? read_u32(record + 40) : view->triangle_count;
		memcpy(&view->far_distance, record + 44, sizeof(view->far_distance));
		if (!far || !isfinite(view->far_distance) || view->far_distance <= 0.0f)
		{
			view->far_distance = 0.0f;
		}
		offset += PWG_MATERIAL_SIZE;
		if (view->surface_count > 0x100000 || view->vertex_count > 0xFFFF || view->triangle_count > 0x400000 ||
			view->part_first_surface > view->surface_count ||
			view->part_surface_count > view->surface_count - view->part_first_surface)
		{
			goto truncated;
		}
		data_size = pad4((view->part_surface_count + 1) * 4) +
			pad4(view->vertex_count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE) +
			pad4(view->vertex_count * ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE) +
			pad4(view->triangle_count * 6) +
			(far ? view->part_surface_count * (8 + 16) : 0);
		if (view->near_triangle_count > view->triangle_count || size - offset < data_size)
		{
			goto truncated;
		}
		view->surface_first_triangle = bytes + offset;
		offset += pad4((view->part_surface_count + 1) * 4);
		view->vertices = bytes + offset;
		offset += pad4(view->vertex_count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE);
		view->lightmap_vertices = bytes + offset;
		offset += pad4(view->vertex_count * ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE);
		view->triangles = bytes + offset;
		offset += pad4(view->triangle_count * 6);
		view->far_triangles = NULL;
		view->bounds = NULL;
		if (far)
		{
			view->far_triangles = bytes + offset;
			offset += view->part_surface_count * 8;
			view->bounds = bytes + offset;
			offset += view->part_surface_count * 16;
		}
	}
	*views_out = views;
	*view_count_out = (long)material_count;

	return TRUE;

truncated:
	platform_log("prism world geometry: %s is cut short or has a material too large (material %lu)", path, index);
	free(views);

	return FALSE;
}

/* whether a material of the file is the map's (its counts and checks), its
shader is opaque and its own tables hold together */
static boolean pwg_material_check(
	struct structure_bsp *bsp,
	struct pwg_material_view *view,
	char const *path)
{
	struct structure_lightmap *lightmap;
	struct structure_material *material;
	struct shader *shader;
	unsigned long topology;
	double positions;
	unsigned long index;
	unsigned long previous = 0;
	char const *reason = NULL;

	if (view->lightmap_index < 0 || view->lightmap_index >= bsp->lightmaps.count)
	{
		reason = "names no lightmap of the BSP";
		goto refused;
	}
	lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, view->lightmap_index, struct structure_lightmap);
	if (view->material_index < 0 || view->material_index >= lightmap->materials.count)
	{
		reason = "names no material of its lightmap";
		goto refused;
	}
	material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, view->material_index, struct structure_material);
	if ((unsigned long)material->surface_count != view->surface_count ||
		(unsigned long)material->vertices.count != view->stock_vertex_count)
	{
		reason = "is of another material (its surface or vertex count differs)";
		goto refused;
	}
	if (!material_stock_checks(bsp, material, &topology, &positions) || topology != view->topology ||
		fabs(positions - view->positions) > 1e-3 + 1e-6 * fabs(positions))
	{
		reason = "is of another material (its surfaces or vertices differ)";
		goto refused;
	}
	shader = material->shader.index == NONE ? NULL : shader_definition_get(material->shader.index);
	if (!shader || shader_type_is_transparent(shader->base.type))
	{
		reason = "is a transparent material's, which stays stock";
		goto refused;
	}
	if (!view->vertex_count || !view->triangle_count)
	{
		reason = "is empty";
		goto refused;
	}
	for (index = 0; index <= view->part_surface_count; index++)
	{
		unsigned long first = read_u32(view->surface_first_triangle + 4 * index);

		if (first < previous || first > view->near_triangle_count || (index == 0 && first != 0))
		{
			reason = "has its surfaces' triangles out of order";
			goto refused;
		}
		previous = first;
	}
	if (previous != view->near_triangle_count)
	{
		reason = "does not end its surfaces' triangles at its triangle count";
		goto refused;
	}
	if (view->far_triangles)
	{
		for (index = 0; index < view->part_surface_count; index++)
		{
			unsigned long first = read_u32(view->far_triangles + 8 * index);
			unsigned long count = read_u32(view->far_triangles + 8 * index + 4);
			int axis;

			if (first > view->triangle_count || count > view->triangle_count - first)
			{
				reason = "has a surface's far triangles outside its triangles";
				goto refused;
			}
			for (axis = 0; axis < 4; axis++)
			{
				float value;

				memcpy(&value, view->bounds + 16 * index + 4 * axis, sizeof(value));
				if (!isfinite(value) || (axis == 3 && value < 0.0f))
				{
					reason = "has a surface's bounds that are no number";
					goto refused;
				}
			}
		}
	}
	for (index = 0; index < view->triangle_count * 3; index++)
	{
		unsigned long vertex = (unsigned long)view->triangles[2 * index] | ((unsigned long)view->triangles[2 * index + 1] << 8);

		if (vertex >= view->vertex_count)
		{
			reason = "has a triangle of no vertex";
			goto refused;
		}
	}
	for (index = 0; index < view->vertex_count * (ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE / 4); index++)
	{
		float value;

		memcpy(&value, view->vertices + 4 * index, sizeof(value));
		if (!isfinite(value))
		{
			reason = "has a vertex that is no number";
			goto refused;
		}
	}
	for (index = 0; index < view->vertex_count * (ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE / 4); index++)
	{
		float value;

		memcpy(&value, view->lightmap_vertices + 4 * index, sizeof(value));
		if (!isfinite(value))
		{
			reason = "has a lightmap vertex that is no number";
			goto refused;
		}
	}
	view->material = material;
	view->usable = TRUE;

	return TRUE;

refused:
	platform_log("prism world geometry: %s lightmap %d material %d %s; it stays stock",
		path, (int)view->lightmap_index, (int)view->material_index, reason);

	return FALSE;
}

/* ---------- the shape's checks (PRISM v4, M1)

A remodel's triangles stand for its stock triangle's surface: each must lie
within the surface's offset prism, the stock triangle's plane and a relief
off it (SHAPE_RELIEF) and its edges and a margin past them in its plane
(SHAPE_MARGIN), or it is a floating facet (a spike or sheet the pack tools'
decimation or far level left: a10f's steel and a10g's panel strips have
them, metres out of slivers of stock triangles). A surface with one, at
either level, is drawn as its stock triangle instead.

A surface drawn stock among remodelled neighbours leaves a crack wherever a
neighbour's edge along it is not the stock edge (a T-junction where the
neighbour's edge has vertices between the stock corners, a gap where it
left the edge). So such surfaces fall back in seal groups, never alone: the
group grows across each stock edge to every remodelled neighbour whose
edge there is not the stock edge: a vertex between the stock corners (a
T-junction), a corner it does not reach, or vertices beside the edge (its
boundary moved off it, as a bevel's: a gap). That is the crack check, on
the CPU over the loaded file; the group grows until no edge of its border
would crack. A group past SEAL_GROUP_MOST surfaces is not let fall back (the
remodel is drawn as the file has it, and the log says so): a crack across a
level is worse than a facet. Neighbours of other materials are found by the
stock edges' corners, which the BSP's surfaces share exactly.

display.prism_world_geometry_check false skips the checks (read as a BSP's
remodel loads). */

#define SHAPE_RELIEF 1.0
#define SHAPE_MARGIN 1.0
/* a neighbour's vertex this near a stock edge is on it (wg2_cracks.py's
tolerance), this near a corner at it (world units): the remodel's
boundaries lie on the stock edges (wg2_seal.py) */
#define SEAL_NEAR 2.0e-3
#define SEAL_CORNER 1.0e-3
/* (a neighbour's vertex this near a stock edge but off it: its boundary
moved, as a bevel's inset) */
#define SEAL_NEAR_BAND 0.1
#define SEAL_GROUP_MOST 256

struct shape_edge
{
	long q[6];
	long surface;
};

struct shape_check
{
	struct structure_bsp *bsp;
	struct pwg_material_view *views;
	long view_count;
	/* the remodelled surfaces, all views' in order: the first of each view
	(view_count + 1 entries) */
	long *view_first;
	long surface_count;
	/* each surface's stock corners (9 floats), its state, its group */
	float *corners;
	byte *state;
	long *queue;
	struct shape_edge *edges;
};

enum
{
	_shape_kept = 0,
	_shape_facets,
	_shape_grouped,
	_shape_fallback,
	_shape_kept_whole
};

static void shape_vertex(
	struct pwg_material_view const *view,
	unsigned long vertex,
	double p[3])
{
	float f[3];

	memcpy(f, view->vertices + vertex * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, sizeof(f));
	p[0] = f[0];
	p[1] = f[1];
	p[2] = f[2];

	return;
}

static unsigned long shape_index(
	struct pwg_material_view const *view,
	unsigned long element)
{
	return (unsigned long)view->triangles[2 * element] | ((unsigned long)view->triangles[2 * element + 1] << 8);
}

/* a part's surface's stock corners (FALSE: an index past the material's) */
static boolean shape_stock_corners(
	struct structure_bsp *bsp,
	struct pwg_material_view const *view,
	long local,
	float corners[9])
{
	long vertex_size = rasterizer_geometry_get_vertex_size(view->material->vertices.type);
	byte const *stock = (byte const *)view->material->compressed_vertex_data.address;
	struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
		view->material->first_surface_index + (long)view->part_first_surface + local, struct structure_surface);
	int corner;

	for (corner = 0; corner < 3; corner++)
	{
		unsigned long vertex = (unsigned short)surface->vertex_indices[corner];

		if (vertex >= (unsigned long)view->material->vertices.count)
		{
			return FALSE;
		}
		memcpy(corners + 3 * corner, stock + vertex * vertex_size, 3 * sizeof(float));
	}

	return TRUE;
}

/* whether a run of a part's triangles has one outside its surface's offset
prism (their centroids against the stock triangle) */
static boolean shape_run_facets(
	struct pwg_material_view const *view,
	float const corners[9],
	unsigned long first,
	unsigned long count,
	unsigned long *facets)
{
	double a[3], b[3], c[3], n[3], inward[3][3], length;
	unsigned long triangle;
	boolean found = FALSE;
	int axis, edge;

	for (axis = 0; axis < 3; axis++)
	{
		a[axis] = corners[axis];
		b[axis] = corners[3 + axis];
		c[axis] = corners[6 + axis];
	}
	n[0] = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
	n[1] = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
	n[2] = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
	length = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (length < 1.0e-12)
	{
		/* (a stock triangle of no area: nothing to stand within) */
		return FALSE;
	}
	for (axis = 0; axis < 3; axis++)
	{
		n[axis] /= length;
	}
	/* each edge's inward direction in the plane (n x edge, the corners
	about n as they come) */
	for (edge = 0; edge < 3; edge++)
	{
		float const *u = corners + 3 * edge;
		float const *v = corners + 3 * ((edge + 1) % 3);
		double e[3] = { v[0] - u[0], v[1] - u[1], v[2] - u[2] };

		inward[edge][0] = n[1] * e[2] - n[2] * e[1];
		inward[edge][1] = n[2] * e[0] - n[0] * e[2];
		inward[edge][2] = n[0] * e[1] - n[1] * e[0];
		length = sqrt(inward[edge][0] * inward[edge][0] + inward[edge][1] * inward[edge][1] +
			inward[edge][2] * inward[edge][2]);
		for (axis = 0; axis < 3; axis++)
		{
			inward[edge][axis] = length > 1.0e-12 ? inward[edge][axis] / length : 0.0;
		}
	}
	for (triangle = first; triangle < first + count; triangle++)
	{
		double centroid[3] = { 0.0, 0.0, 0.0 }, p[3], height, outside = 0.0;
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			shape_vertex(view, shape_index(view, triangle * 3 + corner), p);
			for (axis = 0; axis < 3; axis++)
			{
				centroid[axis] += p[axis] / 3.0;
			}
		}
		height = (centroid[0] - a[0]) * n[0] + (centroid[1] - a[1]) * n[1] + (centroid[2] - a[2]) * n[2];
		for (edge = 0; edge < 3; edge++)
		{
			float const *u = corners + 3 * edge;
			double in = (centroid[0] - u[0]) * inward[edge][0] + (centroid[1] - u[1]) * inward[edge][1] +
				(centroid[2] - u[2]) * inward[edge][2];

			if (-in > outside)
			{
				outside = -in;
			}
		}
		if (fabs(height) > SHAPE_RELIEF || outside > SHAPE_MARGIN)
		{
			(*facets)++;
			found = TRUE;
		}
	}

	return found;
}

static void shape_edge_key(
	float const *u,
	float const *v,
	struct shape_edge *edge)
{
	long a[3], b[3];
	int axis, swap = 0;

	for (axis = 0; axis < 3; axis++)
	{
		a[axis] = (long)floor(u[axis] / SEAL_CORNER + 0.5);
		b[axis] = (long)floor(v[axis] / SEAL_CORNER + 0.5);
	}
	for (axis = 0; axis < 3; axis++)
	{
		if (a[axis] != b[axis])
		{
			swap = a[axis] > b[axis];
			break;
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		edge->q[axis] = swap ? b[axis] : a[axis];
		edge->q[3 + axis] = swap ? a[axis] : b[axis];
	}

	return;
}

static int shape_edge_compare(
	void const *x,
	void const *y)
{
	struct shape_edge const *a = (struct shape_edge const *)x;
	struct shape_edge const *b = (struct shape_edge const *)y;
	int i;

	for (i = 0; i < 6; i++)
	{
		if (a->q[i] != b->q[i])
		{
			return a->q[i] < b->q[i] ? -1 : 1;
		}
	}

	return a->surface < b->surface ? -1 : a->surface > b->surface;
}

/* the view and part's surface of a remodelled surface's number */
static struct pwg_material_view *shape_surface(
	struct shape_check const *check,
	long surface,
	long *local)
{
	long low = 0, high = check->view_count - 1;

	while (low < high)
	{
		long middle = (low + high + 1) / 2;

		if (check->view_first[middle] <= surface)
		{
			low = middle;
		}
		else
		{
			high = middle - 1;
		}
	}
	*local = surface - check->view_first[low];

	return &check->views[low];
}

/* whether a neighbour's edge along the stock edge u-v is that edge: a
vertex of its triangles at each corner, and none on the edge between them
(a T-junction) nor beside it (within SEAL_NEAR_BAND: a boundary moved off
the edge, a bevel's, which a stock triangle beside it leaves a gap against) */
static boolean shape_edge_sealed(
	struct shape_check const *check,
	long neighbour,
	float const *u,
	float const *v)
{
	long local;
	struct pwg_material_view const *view = shape_surface(check, neighbour, &local);
	unsigned long ranges[2][2];
	double e[3] = { v[0] - u[0], v[1] - u[1], v[2] - u[2] };
	double length2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
	int range;
	boolean at_u = FALSE, at_v = FALSE;

	if (length2 < 1.0e-12)
	{
		return TRUE;
	}
	ranges[0][0] = read_u32(view->surface_first_triangle + 4 * local);
	ranges[0][1] = read_u32(view->surface_first_triangle + 4 * (local + 1)) - ranges[0][0];
	ranges[1][0] = view->far_triangles ? read_u32(view->far_triangles + 8 * local) : 0;
	ranges[1][1] = view->far_triangles ? read_u32(view->far_triangles + 8 * local + 4) : 0;
	for (range = 0; range < 2; range++)
	{
		unsigned long element;

		for (element = ranges[range][0] * 3; element < (ranges[range][0] + ranges[range][1]) * 3; element++)
		{
			double p[3], t, d[3], distance2, corner2;
			int axis;

			shape_vertex(view, shape_index(view, element), p);
			t = ((p[0] - u[0]) * e[0] + (p[1] - u[1]) * e[1] + (p[2] - u[2]) * e[2]) / length2;
			t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
			for (axis = 0; axis < 3; axis++)
			{
				d[axis] = p[axis] - (u[axis] + t * e[axis]);
			}
			distance2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
			if (distance2 > SEAL_NEAR_BAND * SEAL_NEAR_BAND)
			{
				continue;
			}
			corner2 = (p[0] - u[0]) * (p[0] - u[0]) + (p[1] - u[1]) * (p[1] - u[1]) + (p[2] - u[2]) * (p[2] - u[2]);
			if (corner2 <= SEAL_CORNER * SEAL_CORNER)
			{
				at_u = TRUE;
				continue;
			}
			corner2 = (p[0] - v[0]) * (p[0] - v[0]) + (p[1] - v[1]) * (p[1] - v[1]) + (p[2] - v[2]) * (p[2] - v[2]);
			if (corner2 <= SEAL_CORNER * SEAL_CORNER)
			{
				at_v = TRUE;
				continue;
			}
			/* (on the edge between its corners, or beside it within the band
			and so a boundary moved off it: either cracks against it) */
			if (distance2 <= SEAL_NEAR * SEAL_NEAR || (t > 0.0 && t < 1.0))
			{
				return FALSE;
			}
		}
	}

	return at_u && at_v;
}

/* the remodel's shape checked (above): each usable view's fallback list
made; a view whose fallbacks would take it past 16-bit indices stays stock */
static void shape_check(
	struct structure_bsp *bsp,
	struct pwg_material_view *views,
	long view_count,
	char const *path)
{
	struct shape_check check;
	long index, surface, edge_count = 0;
	unsigned long facets = 0, facet_surfaces = 0, groups = 0, kept_groups = 0, fallbacks = 0, largest = 0;

	memset(&check, 0, sizeof(check));
	if (!config_boolean("display.prism_world_geometry_check"))
	{
		return;
	}
	check.bsp = bsp;
	check.views = views;
	check.view_count = view_count;
	check.view_first = malloc((view_count + 1) * sizeof(long));
	if (!check.view_first)
	{
		return;
	}
	for (index = 0; index < view_count; index++)
	{
		check.view_first[index] = check.surface_count;
		if (views[index].usable)
		{
			check.surface_count += (long)views[index].part_surface_count;
		}
	}
	check.view_first[view_count] = check.surface_count;
	check.corners = malloc(check.surface_count * 9 * sizeof(float) + 1);
	check.state = calloc(check.surface_count + 1, 1);
	check.queue = malloc(check.surface_count * sizeof(long) + 1);
	check.edges = malloc(check.surface_count * 3 * sizeof(struct shape_edge) + 1);
	if (!check.corners || !check.state || !check.queue || !check.edges)
	{
		goto done;
	}

	/* the facets */
	for (index = 0; index < view_count; index++)
	{
		struct pwg_material_view *view = &views[index];
		long local;

		if (!view->usable)
		{
			continue;
		}
		for (local = 0; local < (long)view->part_surface_count; local++)
		{
			float *corners = check.corners + 9 * (check.view_first[index] + local);
			unsigned long first = read_u32(view->surface_first_triangle + 4 * local);
			unsigned long count = read_u32(view->surface_first_triangle + 4 * (local + 1)) - first;
			boolean found;
			int corner;

			if (!shape_stock_corners(bsp, view, local, corners))
			{
				memset(corners, 0, 9 * sizeof(float));
				continue;
			}
			found = shape_run_facets(view, corners, first, count, &facets);
			if (view->far_triangles &&
				shape_run_facets(view, corners, read_u32(view->far_triangles + 8 * local),
					read_u32(view->far_triangles + 8 * local + 4), &facets))
			{
				found = TRUE;
			}
			if (found)
			{
				check.state[check.view_first[index] + local] = _shape_facets;
				facet_surfaces++;
			}
			for (corner = 0; corner < 3; corner++)
			{
				struct shape_edge *edge = &check.edges[edge_count++];

				shape_edge_key(corners + 3 * corner, corners + 3 * ((corner + 1) % 3), edge);
				edge->surface = check.view_first[index] + local;
			}
		}
	}
	if (!facet_surfaces)
	{
		goto done;
	}
	qsort(check.edges, edge_count, sizeof(*check.edges), shape_edge_compare);

	/* the seal groups, one from each surface with facets not yet in one */
	for (surface = 0; surface < check.surface_count; surface++)
	{
		long head = 0, tail = 0, member;
		boolean too_large = FALSE;

		if (check.state[surface] != _shape_facets)
		{
			continue;
		}
		groups++;
		check.state[surface] = _shape_grouped;
		check.queue[tail++] = surface;
		while (head < tail && !too_large)
		{
			long g = check.queue[head++];
			float const *corners = check.corners + 9 * g;
			int corner;

			for (corner = 0; corner < 3 && !too_large; corner++)
			{
				float const *u = corners + 3 * corner;
				float const *v = corners + 3 * ((corner + 1) % 3);
				struct shape_edge key;
				long low = 0, high = edge_count;

				shape_edge_key(u, v, &key);
				key.surface = -1;
				while (low < high)
				{
					long middle = (low + high) / 2;

					if (shape_edge_compare(&check.edges[middle], &key) < 0)
					{
						low = middle + 1;
					}
					else
					{
						high = middle;
					}
				}
				for (; low < edge_count && !memcmp(check.edges[low].q, key.q, sizeof(key.q)); low++)
				{
					long n = check.edges[low].surface;

					if (n == g || check.state[n] == _shape_grouped || check.state[n] == _shape_fallback)
					{
						continue;
					}
					if (check.state[n] == _shape_facets || !shape_edge_sealed(&check, n, u, v))
					{
						if (tail >= SEAL_GROUP_MOST)
						{
							too_large = TRUE;
							break;
						}
						check.state[n] = _shape_grouped;
						check.queue[tail++] = n;
					}
				}
			}
		}
		if ((unsigned long)tail > largest)
		{
			largest = (unsigned long)tail;
		}
		for (member = 0; member < tail; member++)
		{
			/* (a group too large keeps its remodel: its members are marked
			apart from facets so that no other group takes them up) */
			check.state[check.queue[member]] = too_large ? _shape_kept_whole : _shape_fallback;
		}
		if (too_large)
		{
			kept_groups++;
		}
		else
		{
			fallbacks += (unsigned long)tail;
		}
	}

	/* each view's fallbacks */
	for (index = 0; index < view_count; index++)
	{
		struct pwg_material_view *view = &views[index];
		long local;
		unsigned long count = 0;

		if (!view->usable)
		{
			continue;
		}
		for (local = 0; local < (long)view->part_surface_count; local++)
		{
			count += check.state[check.view_first[index] + local] == _shape_fallback;
		}
		if (!count)
		{
			continue;
		}
		if (view->vertex_count + 3 * count > 0xFFFF)
		{
			platform_log("prism world geometry: %s lightmap %d material %d: %lu surfaces to draw stock would "
				"take it past 65535 vertices; it stays stock", path, (int)view->lightmap_index,
				(int)view->material_index, count);
			view->usable = FALSE;
			continue;
		}
		view->fallback = malloc(count * sizeof(unsigned long));
		if (!view->fallback)
		{
			view->usable = FALSE;
			continue;
		}
		for (local = 0; local < (long)view->part_surface_count; local++)
		{
			if (check.state[check.view_first[index] + local] == _shape_fallback)
			{
				view->fallback[view->fallback_count++] = (unsigned long)local;
			}
		}
	}

done:
	if (facet_surfaces)
	{
		platform_log("prism world geometry: %s: %lu floating facets on %lu surfaces; %lu seal groups (the largest "
			"%lu surfaces), %lu surfaces drawn as their stock triangles, %lu groups kept whole (over %d surfaces)",
			path, facets, facet_surfaces, groups, largest, fallbacks, kept_groups, SEAL_GROUP_MOST);
	}
	else if (check.state)
	{
		platform_log("prism world geometry: %s: no floating facet (%ld surfaces checked)", path, check.surface_count);
	}
	free(check.view_first);
	free(check.corners);
	free(check.state);
	free(check.queue);
	free(check.edges);

	return;
}

/* the cone of facings of a run of a part's triangles (the file's vertices
and triangles), as structure_culling_cone_build's: the bounds' sphere, the
mean of the triangles' unit normals (structure_culling.c's sign: the
stock surfaces' front) and the least cosine of any of them to it. A
triangle of no area draws nothing and is left out; a run of none, or one
whose facings span a hemisphere, gets a cutoff of -1 (never facing away).
Doubles: the cutoff of a run of thousands of nearly flat triangles must
not round past the truth */
static void run_cone(
	byte const *vertices,
	byte const *triangles,
	unsigned long first,
	unsigned long count,
	struct structure_culling_cone *cone)
{
	double low[3] = { 0.0, 0.0, 0.0 }, high[3] = { 0.0, 0.0, 0.0 }, sum[3] = { 0.0, 0.0, 0.0 };
	double centre[3], radius = 0.0, length, cutoff = 1.0;
	unsigned long triangle;
	long normals = 0;
	int axis, pass;

	cone->center.x = cone->center.y = cone->center.z = 0.0f;
	cone->axis.i = cone->axis.j = cone->axis.k = 0.0f;
	cone->radius = 0.0f;
	cone->cutoff = -1.0f;
	if (!count)
	{
		return;
	}
	/* the bounds, then the sphere about their middle; then the normals'
	mean (pass 0) and their least cosine to it (pass 1) */
	for (triangle = first; triangle < first + count; triangle++)
	{
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			unsigned long vertex = (unsigned long)triangles[6 * triangle + 2 * corner] |
				((unsigned long)triangles[6 * triangle + 2 * corner + 1] << 8);
			float point[3];

			memcpy(point, vertices + vertex * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, sizeof(point));
			for (axis = 0; axis < 3; axis++)
			{
				if ((triangle == first && corner == 0) || point[axis] < low[axis])
				{
					low[axis] = point[axis];
				}
				if ((triangle == first && corner == 0) || point[axis] > high[axis])
				{
					high[axis] = point[axis];
				}
			}
		}
	}
	for (axis = 0; axis < 3; axis++)
	{
		centre[axis] = 0.5 * (low[axis] + high[axis]);
	}
	for (pass = 0; pass < 2; pass++)
	{
		for (triangle = first; triangle < first + count; triangle++)
		{
			double p[3][3], e0[3], e1[3], n[3], squared;
			int corner;

			for (corner = 0; corner < 3; corner++)
			{
				unsigned long vertex = (unsigned long)triangles[6 * triangle + 2 * corner] |
					((unsigned long)triangles[6 * triangle + 2 * corner + 1] << 8);
				float point[3];

				memcpy(point, vertices + vertex * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, sizeof(point));
				for (axis = 0; axis < 3; axis++)
				{
					p[corner][axis] = point[axis];
				}
				if (pass == 0)
				{
					double dx = p[corner][0] - centre[0], dy = p[corner][1] - centre[1], dz = p[corner][2] - centre[2];
					double distance = sqrt(dx * dx + dy * dy + dz * dz);

					radius = distance > radius ? distance : radius;
				}
			}
			for (axis = 0; axis < 3; axis++)
			{
				e0[axis] = p[1][axis] - p[0][axis];
				e1[axis] = p[2][axis] - p[0][axis];
			}
			n[0] = e0[1] * e1[2] - e0[2] * e1[1];
			n[1] = e0[2] * e1[0] - e0[0] * e1[2];
			n[2] = e0[0] * e1[1] - e0[1] * e1[0];
			squared = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
			if (!(squared > 1.0e-18))
			{
				continue;
			}
			length = -1.0 / sqrt(squared); /* (structure_culling.c's STRUCTURE_CULLING_FRONT_SIGN) */
			if (pass == 0)
			{
				for (axis = 0; axis < 3; axis++)
				{
					sum[axis] += n[axis] * length;
				}
				normals++;
			}
			else
			{
				double cosine = (n[0] * sum[0] + n[1] * sum[1] + n[2] * sum[2]) * length;

				cutoff = cosine < cutoff ? cosine : cutoff;
			}
		}
		if (pass == 0)
		{
			length = sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
			if (!normals || !(length > 1.0e-9))
			{
				return;
			}
			for (axis = 0; axis < 3; axis++)
			{
				sum[axis] /= length;
			}
		}
	}
	cone->center.x = (float)centre[0];
	cone->center.y = (float)centre[1];
	cone->center.z = (float)centre[2];
	/* (a little more than the truth: the vertices are compressed for
	drawing, and the float sphere must hold every one) */
	cone->radius = (float)(radius * 1.0001 + 1.0e-3);
	cone->axis.i = (float)sum[0];
	cone->axis.j = (float)sum[1];
	cone->axis.k = (float)sum[2];
	/* (a margin for the compression of the normals' positions, too) */
	cone->cutoff = cutoff > 0.0 ? (float)(cutoff - 1.0e-3) : -1.0f;

	return;
}

/* the part's surfaces' cones of facings. A surface whose mean facing is
not its stock triangle's front is marked in turned (bit 0 its near
triangles, bit 1 its far ones; NULL: left at -1, never facing away): the
remodel wound its triangles to its vertices' normals where CE's triangle
is wound against its own (d40's steel, a10's panels: CE draws such a
triangle by its winding, and its culling skips it by that front), so the
GPU culled the remodel exactly where CE showed the stock face, and the sky
showed through (d40g's ramp). Such a run is turned to the stock's winding
(turn_triangles) and its cone with it. The count of those returned */
static long part_cones(
	struct structure_bsp *bsp,
	struct pwg_material_view const *view,
	struct world_material *made,
	byte *turned)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(view->material->vertices.type);
	byte const *stock_vertices = (byte const *)view->material->compressed_vertex_data.address;
	long local, disagree = 0;

	made->cones = malloc(made->surface_count * 2 * sizeof(*made->cones) + 1);
	made->choice_stamps = calloc(made->surface_count + 1, sizeof(*made->choice_stamps));
	made->choices = calloc(made->surface_count + 1, 1);
	if (!made->cones || !made->choice_stamps || !made->choices)
	{
		free(made->cones);
		made->cones = NULL;
		return 0;
	}
	for (local = 0; local < made->surface_count; local++)
	{
		struct structure_culling_cone *near = &made->cones[2 * local];
		struct structure_culling_cone *far = &made->cones[2 * local + 1];
		struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
			made->first_surface_index + made->part_first_surface + local, struct structure_surface);
		real_vector3d stock = { 0.0f, 0.0f, 0.0f };
		float p[3][3];
		int corner;

		run_cone(view->vertices, view->triangles, made->surface_first_triangle[local],
			made->surface_first_triangle[local + 1] - made->surface_first_triangle[local], near);
		if (made->far_triangles)
		{
			run_cone(view->vertices, view->triangles, made->far_triangles[2 * local], made->far_triangles[2 * local + 1],
				far);
		}
		else
		{
			*far = *near;
		}
		/* the stock triangle's front (the checks have bounded its indices'
		material: material_stock_checks) */
		for (corner = 0; corner < 3; corner++)
		{
			unsigned long vertex = (unsigned short)surface->vertex_indices[corner];

			if (vertex >= (unsigned long)view->material->vertices.count)
			{
				break;
			}
			memcpy(p[corner], stock_vertices + vertex * vertex_size, sizeof(p[corner]));
		}
		if (corner == 3)
		{
			float e0[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
			float e1[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };

			stock.i = -(e0[1] * e1[2] - e0[2] * e1[1]);
			stock.j = -(e0[2] * e1[0] - e0[0] * e1[2]);
			stock.k = -(e0[0] * e1[1] - e0[1] * e1[0]);
		}
		/* (a run with no facing, no area, has a zero axis: never turned) */
		if (dot_product3d(&near->axis, &stock) < 0.0f)
		{
			if (turned)
			{
				turned[local] |= 1;
				near->axis.i = -near->axis.i;
				near->axis.j = -near->axis.j;
				near->axis.k = -near->axis.k;
			}
			else
			{
				near->cutoff = -1.0f;
			}
			disagree++;
		}
		if (made->far_triangles && dot_product3d(&far->axis, &stock) < 0.0f)
		{
			if (turned)
			{
				turned[local] |= 2;
				far->axis.i = -far->axis.i;
				far->axis.j = -far->axis.j;
				far->axis.k = -far->axis.k;
			}
			else
			{
				far->cutoff = -1.0f;
			}
			disagree++;
		}
		else if (!made->far_triangles)
		{
			*far = *near;
		}
	}

	return disagree;
}

/* a part's fallback surfaces' stock corners as vertices after the file's
(stride bytes each, in the file's uncompressed layout, into to): each a copy
of the surface's own vertex nearest that corner (its base map and lightmap
coordinates, normal and incident radiosity are those of the point of the
surface it lies over), put at the corner. The vertices' pass finds the
copied vertices (sources); the lightmap vertices' pass, after it, reuses
them */
static void fallback_vertices(
	struct structure_bsp *bsp,
	struct pwg_material_view const *view,
	byte *to,
	unsigned long stride,
	byte const *from,
	unsigned long *sources)
{
	unsigned long element;

	for (element = 0; element < view->fallback_count; element++)
	{
		long local = (long)view->fallback[element];
		float corners[9];
		int corner;

		if (!shape_stock_corners(bsp, view, local, corners))
		{
			memset(corners, 0, sizeof(corners));
		}
		for (corner = 0; corner < 3; corner++)
		{
			unsigned long target = view->vertex_count + 3 * element + corner;

			if (stride == ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE)
			{
				unsigned long first = read_u32(view->surface_first_triangle + 4 * local);
				unsigned long last = read_u32(view->surface_first_triangle + 4 * (local + 1));
				unsigned long best = 0, item;
				double best_distance = -1.0;

				for (item = first * 3; item < last * 3; item++)
				{
					unsigned long vertex = shape_index(view, item);
					double p[3], d;

					shape_vertex(view, vertex, p);
					d = (p[0] - corners[3 * corner]) * (p[0] - corners[3 * corner]) +
						(p[1] - corners[3 * corner + 1]) * (p[1] - corners[3 * corner + 1]) +
						(p[2] - corners[3 * corner + 2]) * (p[2] - corners[3 * corner + 2]);
					if (best_distance < 0.0 || d < best_distance)
					{
						best_distance = d;
						best = vertex;
					}
				}
				sources[3 * element + corner] = best;
			}
			memcpy(to + target * stride, from + sources[3 * element + corner] * stride, stride);
			if (stride == ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE)
			{
				memcpy(to + target * stride, corners + 3 * corner, 3 * sizeof(float));
			}
		}
	}

	return;
}

/* a part's fallback surfaces' triangles: the first of each one's near run
its stock triangle (the stock surface's corners in their order: its front),
the rest of no area (the GPU draws nothing of them), and its far level that
same run (its own far triangles, if any, are left undrawn) */
/* the runs part_cones marked turned wound as their stock triangles are
(corners 1 and 2 swapped), each triangle once: a surface with no far level
of its own shares its near run */
static void turn_triangles(
	struct world_material const *made,
	byte const *turned,
	word *indices)
{
	byte *mark;
	long local;
	unsigned long triangle;

	if (!turned || made->triangle_count <= 0)
	{
		return;
	}
	mark = calloc((unsigned long)made->triangle_count + 1, 1);
	if (!mark)
	{
		return;
	}
	for (local = 0; local < made->surface_count; local++)
	{
		unsigned long first, count;

		if (turned[local] & 1)
		{
			first = made->surface_first_triangle[local];
			count = made->surface_first_triangle[local + 1] - first;
			if (first <= (unsigned long)made->triangle_count && count <= (unsigned long)made->triangle_count - first)
			{
				memset(mark + first, 1, count);
			}
		}
		if ((turned[local] & 2) && made->far_triangles)
		{
			first = made->far_triangles[2 * local];
			count = made->far_triangles[2 * local + 1];
			if (first <= (unsigned long)made->triangle_count && count <= (unsigned long)made->triangle_count - first)
			{
				memset(mark + first, 1, count);
			}
		}
	}
	for (triangle = 0; triangle < (unsigned long)made->triangle_count; triangle++)
	{
		if (mark[triangle])
		{
			word swap = indices[3 * triangle + 1];

			indices[3 * triangle + 1] = indices[3 * triangle + 2];
			indices[3 * triangle + 2] = swap;
		}
	}
	free(mark);

	return;
}

static void fallback_triangles(
	struct pwg_material_view const *view,
	struct world_material *made,
	word *indices)
{
	unsigned long element;

	for (element = 0; element < view->fallback_count; element++)
	{
		long local = (long)view->fallback[element];
		word first_vertex = (word)(view->vertex_count + 3 * element);
		unsigned long first = made->surface_first_triangle[local];
		unsigned long count = made->surface_first_triangle[local + 1] - first;
		unsigned long triangle;

		for (triangle = first; triangle < first + count; triangle++)
		{
			int corner;

			for (corner = 0; corner < 3; corner++)
			{
				indices[3 * triangle + corner] = triangle == first ? (word)(first_vertex + corner) : first_vertex;
			}
		}
		if (made->far_triangles)
		{
			made->far_triangles[2 * local] = first;
			made->far_triangles[2 * local + 1] = count;
		}
	}

	return;
}

/* the usable materials' vertices compressed into one block of contiguous
memory, each array inside a mirror segment, and their buffers made */
static boolean world_geometry_make(
	struct structure_bsp *bsp,
	struct pwg_material_view *views,
	long view_count,
	char const *path)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	unsigned long total = 0, alignment, offset;
	unsigned long triangles = 0, stock_triangles = 0, vertices = 0;
	long usable = 0, index, lightmap_index, entry_count = 0, disagree = 0, coned = 0, facing = 0;
	byte *scratch = NULL;
	unsigned long scratch_size = 0;
	/* (a fallback surface's corners' vertices of the file: fallback_vertices) */
	unsigned long *corner_sources = NULL;
	unsigned long most_fallbacks = 0;
	long fallen = 0;
	/* (a part's surfaces turned to their stock triangles' winding: part_cones) */
	byte *turned = NULL;

	for (index = 0; index < view_count; index++)
	{
		struct pwg_material_view const *view = &views[index];
		unsigned long count;

		if (!view->usable)
		{
			continue;
		}
		/* (with a fallback surface's three corners each: shape_check) */
		count = view->vertex_count + 3 * view->fallback_count;
		usable++;
		total = segment_place(total, count * vertex_size) + count * vertex_size;
		total = segment_place(total, count * lightmap_vertex_size) + count * lightmap_vertex_size;
		total = segment_place(total, view->triangle_count * 6) + view->triangle_count * 6;
		if (count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE > scratch_size)
		{
			scratch_size = count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE;
		}
	}
	if (!usable)
	{
		platform_log("prism world geometry: %s has no material of this map that can be drawn", path);
		return FALSE;
	}
	if (total > MAXIMUM_CONTIGUOUS_BYTES)
	{
		platform_log("prism world geometry: %s needs %lu KB of contiguous memory, more than the %lu KB it may take",
			path, total / 1024, MAXIMUM_CONTIGUOUS_BYTES / 1024);
		return FALSE;
	}
	for (alignment = 0x1000; alignment < MIRROR_SEGMENT_BYTES && alignment < total + 1; alignment <<= 1)
	{
	}
	world_geometry.geometry = XPhysicalAlloc(total + 1, (unsigned long)-1, alignment, PAGE_READWRITE);
	world_geometry.materials = calloc(usable, sizeof(*world_geometry.materials));
	world_geometry.lightmap_first = calloc(bsp->lightmaps.count + 1, sizeof(*world_geometry.lightmap_first));
	world_geometry.runs = malloc(MAXIMUM_CHUNK_SURFACES * sizeof(*world_geometry.runs));
	scratch = malloc(scratch_size + 1);
	for (index = 0; index < view_count; index++)
	{
		if (views[index].usable && views[index].fallback_count > most_fallbacks)
		{
			most_fallbacks = views[index].fallback_count;
		}
	}
	corner_sources = malloc(most_fallbacks * 3 * sizeof(unsigned long) + 1);
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count && world_geometry.lightmap_first; lightmap_index++)
	{
		world_geometry.lightmap_first[lightmap_index] = entry_count;
		entry_count += TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index, struct structure_lightmap)->materials.count;
	}
	if (world_geometry.lightmap_first)
	{
		world_geometry.lightmap_first[bsp->lightmaps.count] = entry_count;
	}
	world_geometry.lookup = malloc((entry_count + 1) * sizeof(*world_geometry.lookup));
	if (!world_geometry.geometry || !world_geometry.materials || !world_geometry.lightmap_first ||
		!world_geometry.runs || !scratch || !world_geometry.lookup || !corner_sources)
	{
		platform_log("prism world geometry: %s: out of memory (%lu KB of contiguous memory)", path, total / 1024);
		free(scratch);
		free(corner_sources);
		return FALSE;
	}
	world_geometry.lightmap_count = bsp->lightmaps.count;
	for (index = 0; index < entry_count; index++)
	{
		world_geometry.lookup[index] = NONE;
	}

	offset = 0;
	for (index = 0; index < view_count; index++)
	{
		struct pwg_material_view const *view = &views[index];
		struct world_material *made = &world_geometry.materials[world_geometry.material_count];
		long entry;
		byte *compressed;
		word *indices;
		unsigned long element, vertex_count;

		if (!view->usable)
		{
			continue;
		}
		entry = world_geometry.lightmap_first[view->lightmap_index] + view->material_index;
		if (world_geometry.lookup[entry] != NONE)
		{
			/* (a material's next part: right after its last, and after its
			surfaces) */
			struct world_material *first_part = &world_geometry.materials[world_geometry.lookup[entry]];
			struct world_material *last_part = made - 1;

			if (first_part + first_part->part_count != made ||
				(long)view->part_first_surface < last_part->part_first_surface + last_part->surface_count)
			{
				platform_log("prism world geometry: %s names lightmap %d material %d again out of order; "
					"the first is drawn", path, (int)view->lightmap_index, (int)view->material_index);
				continue;
			}
		}
		made->lightmap_index = view->lightmap_index;
		made->material_index = view->material_index;
		made->first_surface_index = view->material->first_surface_index;
		made->part_first_surface = (long)view->part_first_surface;
		made->surface_count = (long)view->part_surface_count;
		made->part_count = 1;
		made->triangle_count = (long)view->triangle_count;
		made->surface_first_triangle = malloc((view->part_surface_count + 1) * sizeof(unsigned long));
		if (!made->surface_first_triangle)
		{
			free(scratch);
			free(corner_sources);
			return FALSE;
		}
		for (element = 0; element <= view->part_surface_count; element++)
		{
			made->surface_first_triangle[element] = read_u32(view->surface_first_triangle + 4 * element);
		}
		made->far_triangles = NULL;
		made->bounds = NULL;
		made->far_distance = view->far_distance;
		if (view->far_triangles && view->far_distance > 0.0f)
		{
			made->far_triangles = malloc(view->part_surface_count * 2 * sizeof(unsigned long) + 1);
			made->bounds = malloc(view->part_surface_count * 4 * sizeof(float) + 1);
			if (!made->far_triangles || !made->bounds)
			{
				free(made->far_triangles);
				free(made->bounds);
				made->far_triangles = NULL;
				made->bounds = NULL;
			}
			else
			{
				for (element = 0; element < view->part_surface_count * 2; element++)
				{
					made->far_triangles[element] = read_u32(view->far_triangles + 4 * element);
				}
				memcpy(made->bounds, view->bounds, view->part_surface_count * 4 * sizeof(float));
			}
		}
		made->cones = NULL;
		made->choice_stamps = NULL;
		made->choices = NULL;
		made->far_state = NULL;
		made->far_switched = NULL;
		if (made->far_triangles)
		{
			made->far_state = malloc(view->part_surface_count + 1);
			made->far_switched = calloc(view->part_surface_count + 1, sizeof(unsigned long));
			if (made->far_state)
			{
				memset(made->far_state, 2, view->part_surface_count + 1);
			}
		}
		turned = calloc(view->part_surface_count + 1, 1);
		disagree += part_cones(bsp, view, made, turned);
		if (made->cones)
		{
			/* (a surface drawn as its stock triangle: its cones are the
			file's triangles', never skipped for them) */
			for (element = 0; element < view->fallback_count; element++)
			{
				made->cones[2 * view->fallback[element]].cutoff = -1.0f;
				made->cones[2 * view->fallback[element] + 1].cutoff = -1.0f;
			}
		}
		if (made->cones)
		{
			long local;

			for (local = 0; local < made->surface_count * 2; local++)
			{
				coned++;
				facing += made->cones[local].cutoff > 0.0f;
			}
		}

		/* the vertices, then the lightmap vertices (compressed from a copy:
		the file's bytes need not be aligned) */
		vertex_count = view->vertex_count + 3 * view->fallback_count;
		offset = segment_place(offset, vertex_count * vertex_size);
		compressed = world_geometry.geometry + offset;
		memcpy(scratch, view->vertices, view->vertex_count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE);
		fallback_vertices(bsp, view, scratch, ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, view->vertices, corner_sources);
		rasterizer_geometry_compress_vertices(
			_rasterizer_vertex_type_environment_uncompressed,
			(long)vertex_count,
			compressed,
			(long)vertex_count * vertex_size,
			scratch,
			(long)vertex_count * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE);
		made->vertex_resources[0].common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		made->vertex_resources[0].data = 0;
		D3DResource_Register((D3DResource *)&made->vertex_resources[0], compressed);
		made->buffers[0].type = _rasterizer_vertex_type_environment_compressed;
		made->buffers[0].count = (long)vertex_count;
		made->buffers[0].offset = 0;
		made->buffers[0].base_address = compressed;
		made->buffers[0].hardware_format = &made->vertex_resources[0];
		offset += vertex_count * vertex_size;

		offset = segment_place(offset, vertex_count * lightmap_vertex_size);
		compressed = world_geometry.geometry + offset;
		memcpy(scratch, view->lightmap_vertices, view->vertex_count * ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE);
		fallback_vertices(bsp, view, scratch, ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE, view->lightmap_vertices,
			corner_sources);
		rasterizer_geometry_compress_vertices(
			_rasterizer_vertex_type_environment_lightmap_uncompressed,
			(long)vertex_count,
			compressed,
			(long)vertex_count * lightmap_vertex_size,
			scratch,
			(long)vertex_count * ENVIRONMENT_LIGHTMAP_VERTEX_UNCOMPRESSED_SIZE);
		made->vertex_resources[1].common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		made->vertex_resources[1].data = 0;
		D3DResource_Register((D3DResource *)&made->vertex_resources[1], compressed);
		made->buffers[1].type = _rasterizer_vertex_type_environment_lightmap_compressed;
		made->buffers[1].count = (long)vertex_count;
		made->buffers[1].offset = 0;
		made->buffers[1].base_address = compressed;
		made->buffers[1].hardware_format = &made->vertex_resources[1];
		offset += vertex_count * lightmap_vertex_size;

		/* the triangles: an index buffer's data is ordinary (virtual)
		memory at its Data address (xdk_d3d8.h) */
		offset = segment_place(offset, view->triangle_count * 6);
		indices = (word *)(world_geometry.geometry + offset);
		for (element = 0; element < view->triangle_count * 3; element++)
		{
			indices[element] = (word)((unsigned long)view->triangles[2 * element] |
				((unsigned long)view->triangles[2 * element + 1] << 8));
		}
		turn_triangles(made, turned, indices);
		free(turned);
		turned = NULL;
		fallback_triangles(view, made, indices);
		made->index_resource.common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
		made->index_resource.data = (unsigned long)indices;
		offset += view->triangle_count * 6;

		if (world_geometry.lookup[entry] == NONE)
		{
			world_geometry.lookup[entry] = (short)world_geometry.material_count;
		}
		else
		{
			world_geometry.materials[world_geometry.lookup[entry]].part_count++;
		}
		world_geometry.material_count++;
		triangles += view->triangle_count;
		stock_triangles += view->part_surface_count;
		vertices += vertex_count;
		fallen += (long)view->fallback_count;
	}
	free(scratch);
	free(corner_sources);
	/* (the batches' indices: a part's runs copied one after another, never
	more than its triangles; in physical memory, as the parts' own indices
	are, made once with the remodel and let go with it. A buffer of the
	heap grown as the frames asked broke musl's heap in a10's remodel) */
	{
		long most = 0;

		for (index = 0; index < world_geometry.material_count; index++)
		{
			most = MAX(most, world_geometry.materials[index].triangle_count);
		}
		world_geometry.batch_capacity = 0;
		world_geometry.batch_indices = most > 0 ?
			XPhysicalAlloc((unsigned long)most * 3 * sizeof(word) + 64, (unsigned long)-1, 0x1000, PAGE_READWRITE) : NULL;
		if (world_geometry.batch_indices)
		{
			world_geometry.batch_capacity = (unsigned long)most * 3;
		}
	}
	platform_log("prism world geometry: %s: %ld of %ld material parts remodelled, %lu triangles for %lu (%.1fx), "
		"%lu vertices, %lu KB of contiguous memory; %.1f%% of the surfaces' levels can face away whole "
		"(%ld levels wound against their stock triangles, turned to them); %ld surfaces drawn as their stock triangles",
		path, world_geometry.material_count, view_count, triangles, stock_triangles,
		stock_triangles ? (double)triangles / (double)stock_triangles : 0.0, vertices, total / 1024,
		coned ? 100.0 * (double)facing / (double)coned : 0.0, disagree, fallen);

	return TRUE;
}

/* the remodel of the BSP whose tag is tag_index, when there is one */
static void world_geometry_load(
	struct structure_bsp *bsp,
	long tag_index)
{
	char path[600];
	char const *name = tag_get_name(tag_index);
	struct pwg_material_view *views = NULL;
	long view_count = 0, index;
	unsigned char *bytes = NULL;
	unsigned long size = 0;
	short bsp_index = global_structure_bsp_index_get();

	world_geometry.tried_tag_index = tag_index;
	world_geometry.tried_bsp = bsp;
	if (!name || !bsp)
	{
		return;
	}
	/* (the BSP's enhanced geometry, from the map's sidecar: prism_world_source.c) */
	if (!prism_world_source_image(bsp, bsp_index, name, &bytes, &size))
	{
		return;
	}
	snprintf(path, sizeof(path), "%s's sidecar geometry (%s)", name, prism_world_source_label(bsp_index));
	if (pwg_parse(bytes, size, bsp, &views, &view_count, path))
	{
		for (index = 0; index < view_count; index++)
		{
			pwg_material_check(bsp, &views[index], path);
		}
		shape_check(bsp, views, view_count, path);
		if (world_geometry_make(bsp, views, view_count, path))
		{
			world_geometry.loaded_tag_index = tag_index;
			world_geometry.loaded_bsp = bsp;
		}
		else
		{
			world_geometry_release();
		}
	}
	for (index = 0; views && index < view_count; index++)
	{
		free(views[index].fallback);
	}
	free(views);
	free(bytes);

	return;
}

static void statistics_frame(
	void)
{
	if (!world_geometry.loaded_bsp)
	{
		return;
	}
	world_geometry.frames++;
	if (world_geometry.frames >= STATISTICS_FRAMES)
	{
		platform_log("prism world geometry: %lu frames: %.1f draws, %.0f triangles a frame for %.0f stock, "
			"%.1f%% of the surfaces drawn far, %.1f%% skipped facing away (%.0f of their triangles a frame drawn "
			"through), %.1f runs a frame drawn with another",
			world_geometry.frames, (double)world_geometry.draws / world_geometry.frames,
			(double)world_geometry.triangles / world_geometry.frames,
			(double)world_geometry.stock_triangles / world_geometry.frames,
			world_geometry.surfaces ? 100.0 * (double)world_geometry.far_surfaces / world_geometry.surfaces : 0.0,
			world_geometry.stock_triangles ? 100.0 * (double)world_geometry.away_surfaces / world_geometry.stock_triangles : 0.0,
			(double)world_geometry.filled_triangles / world_geometry.frames,
			(double)world_geometry.batched_runs / world_geometry.frames);
		world_geometry.batched_runs = 0;
		world_geometry.away_surfaces = 0;
		world_geometry.filled_triangles = 0;
		world_geometry.far_surfaces = 0;
		world_geometry.surfaces = 0;
		world_geometry.frames = 0;
		world_geometry.draws = 0;
		world_geometry.triangles = 0;
		world_geometry.stock_triangles = 0;
	}

	return;
}

/* a surface's choices for the frame's camera (_surface_choice_*: drawn
far, its near and far levels facing away whole), found once for each
camera (the passes of a frame, the frames of a still camera ask again) */
/* (port: HALO_GEOMETRY_LOG's flip window, as the objects' resolver's,
xgpu_geometry.h's XGPU_GEOMETRY_FLIP_FRAMES) */
#define GEOMETRY_FLIP_FRAMES 90UL
/* (a camera moved more than this in a frame: a cut, a teleport, a path's
start, not a flip's cause) */
#define GEOMETRY_CUT_UNITS 5.0f

static byte surface_choice(
	struct world_material *of,
	long local)
{
	byte choice = 0;

	if (of->choice_stamps && of->choice_stamps[local] == world_geometry.stamp)
	{
		return of->choices[local];
	}
	if (of->far_triangles)
	{
		float const *sphere = of->bounds + 4 * local;
		float dx = sphere[0] - world_geometry.camera.x;
		float dy = sphere[1] - world_geometry.camera.y;
		float dz = sphere[2] - world_geometry.camera.z;
		float beyond = sqrtf(dx * dx + dy * dy + dz * dz) - sphere[3];
		float distance = of->far_distance * world_geometry.distance_scale;
		byte state = of->far_state ? of->far_state[local] : 2;

		/* (display.prism_world_geometry_band: the surface's own distance in
		the band past the file's, by its number; display.prism_lod_hysteresis:
		a surface drawn near goes far only that much further again, and
		comes back near at its distance: never drawn far nearer than the
		file's distance) */
		if (world_geometry.band > 0.0f)
		{
			unsigned long number = (unsigned long)(of->first_surface_index + of->part_first_surface + local);
			float place = (float)(((number * 2654435761u) >> 8) & 0xffff) / 65536.0f;

			distance *= 1.0f + world_geometry.band * place;
		}
		if (state == 0 && world_geometry.hysteresis > 0.0f)
		{
			distance *= 1.0f + world_geometry.hysteresis;
		}
		if (beyond > distance)
		{
			choice |= FLAG(_surface_choice_far_bit);
		}
		if (of->far_state)
		{
			byte now = beyond > distance;

			if (state != 2 && state != now)
			{
				prism_popin_world_switched();
				/* (port: counted, and a switch undone within the flip window
				a flip: the resolver's measure of the objects', for the
				world's levels) */
				world_geometry.near_far_switches++;
				if (of->far_switched && of->far_switched[local] && of->far_switched[local] > world_geometry.cut_frame &&
					world_geometry.geometry_frame - of->far_switched[local] <= GEOMETRY_FLIP_FRAMES)
				{
					world_geometry.near_far_flips++;
					if (world_geometry.geometry_log >= 2)
					{
						platform_log("geometry: world frame %lu surface %ld of lightmap %d material %d near/far flip (%s, "
							"%.1f wu past its distance)", world_geometry.geometry_frame,
							(long)(of->first_surface_index + of->part_first_surface + local), (int)of->lightmap_index,
							(int)of->material_index, now ? "far" : "near", (double)(beyond - distance));
					}
				}
			}
			/* (a switch's frame; a surface's first level, 2 before, is no
			switch, so a switch after it is no flip) */
			if (of->far_switched && state != 2 && state != now)
			{
				of->far_switched[local] = world_geometry.geometry_frame ? world_geometry.geometry_frame : 1;
			}
			of->far_state[local] = now;
		}
	}
	if (of->cones)
	{
		if (structure_culling_cone_facing(&of->cones[2 * local], &world_geometry.camera, world_geometry.camera_radius) ==
			_structure_culling_facing_away)
		{
			choice |= FLAG(_surface_choice_near_away_bit);
		}
		if (structure_culling_cone_facing(&of->cones[2 * local + 1], &world_geometry.camera,
			world_geometry.camera_radius) == _structure_culling_facing_away)
		{
			choice |= FLAG(_surface_choice_far_away_bit);
		}
		of->choice_stamps[local] = world_geometry.stamp;
		of->choices[local] = choice;
	}

	return choice;
}

/* ---------- public code */

/* port: the BSP's remodel taken up (or let go of) for the frame: by the
first of the frame's readers, SCENE_STATIC's (world_ao_frame_begin, before
the structure is drawn), so that its first set, the bake's key and the
frame's draws are all of the remodel from the BSP's first frame, not stock
for a frame and then begun again; TRUE where the setting asks for it */
boolean prism_world_geometry_prepare(
	void)
{
	struct structure_bsp *bsp;
	long tag_index;

	settings_read();
	if (!world_geometry.enabled)
	{
		return FALSE;
	}
	bsp = global_structure_bsp_get();
	tag_index = global_structure_bsp_tag_index_get();
	if (world_geometry.loaded_bsp && (world_geometry.loaded_bsp != bsp || world_geometry.loaded_tag_index != tag_index))
	{
		world_geometry_release();
	}
	if (!world_geometry.loaded_bsp && bsp && tag_index != NONE &&
		(world_geometry.tried_bsp != bsp || world_geometry.tried_tag_index != tag_index))
	{
		world_geometry_load(bsp, tag_index);
	}

	return TRUE;
}

void prism_world_geometry_frame(
	void)
{
	world_geometry.current = NULL;
	/* (port: HALO_GEOMETRY_LOG's frames, the renderer's; its summary of the
	surfaces' near/far switches and flips each 600) */
	{
		static int log_read;

		if (!log_read)
		{
			char const *log = getenv("HALO_GEOMETRY_LOG");

			world_geometry.geometry_log = log && *log >= '1' && *log <= '9' ? *log - '0' : 0;
			log_read = TRUE;
		}
		if (world_geometry.geometry_frame != (unsigned long)render.frame_index)
		{
			float dx = render.camera.position.x - world_geometry.geometry_camera.x;
			float dy = render.camera.position.y - world_geometry.geometry_camera.y;
			float dz = render.camera.position.z - world_geometry.geometry_camera.z;

			world_geometry.geometry_frame = (unsigned long)render.frame_index;
			if (dx * dx + dy * dy + dz * dz > GEOMETRY_CUT_UNITS * GEOMETRY_CUT_UNITS)
			{
				world_geometry.cut_frame = world_geometry.geometry_frame;
				world_geometry.cuts++;
			}
			world_geometry.geometry_camera = render.camera.position;
			if (world_geometry.geometry_log >= 1 && world_geometry.geometry_frame % 600UL == 0)
			{
				platform_log("geometry: world: frame %lu: %s; near/far switches %lu, flips %lu; camera cuts %lu",
					world_geometry.geometry_frame, world_geometry.enabled && world_geometry.loaded_bsp ? "remodel" : "stock",
					world_geometry.near_far_switches, world_geometry.near_far_flips, world_geometry.cuts);
			}
		}
	}
	if (!prism_world_geometry_prepare())
	{
		return;
	}
	/* (the frame's camera, which every pass of the structure's draws this
	frame shares; a narrower view, a zoom, draws near surfaces from further) */
	{
		float fov = render.camera.vertical_field_of_view;
		float scale = isfinite(fov) && fov > 0.01f && fov < 3.1f ?
			(float)(tan(0.5 * 70.0 * 3.14159265358979 / 180.0) / tan(0.5 * (double)fov)) : 1.0f;
		real radius = structure_culling_get_camera_radius();

		scale *= world_geometry.detail;
		/* (the surfaces' choices are kept while the camera stays: every pass
		of a frame, and the frames of a still camera) */
		if (!world_geometry.stamp || memcmp(&world_geometry.camera, &render.camera.position, sizeof(real_point3d)) ||
			world_geometry.distance_scale != scale || world_geometry.camera_radius != radius)
		{
			world_geometry.stamp++;
			if (!world_geometry.stamp)
			{
				world_geometry.stamp = 1;
			}
		}
		world_geometry.camera = render.camera.position;
		world_geometry.distance_scale = scale;
		world_geometry.camera_radius = radius;
	}
	statistics_frame();
	/* (PRISM v4's SCENE_STATIC: each surface's level for this camera, for the
	ray tracer's structure to follow) */
	prism_scene_static_levels();

	return;
}

struct vertex_buffer const *prism_world_geometry_chunk(
	short lightmap_index,
	short material_index,
	long const *surface_indices,
	long surface_count,
	struct vertex_buffer const *stock)
{
	struct world_material *made;
	long entry, index, part, gap;
	short proxy;
	boolean facing;

	world_geometry.current = NULL;
	if (!world_geometry.enabled || !world_geometry.loaded_bsp || !world_geometry.lookup ||
		lightmap_index < 0 || lightmap_index >= world_geometry.lightmap_count || surface_count <= 0 ||
		surface_count > MAXIMUM_CHUNK_SURFACES)
	{
		return stock;
	}
	entry = world_geometry.lightmap_first[lightmap_index] + material_index;
	if (material_index < 0 || entry >= world_geometry.lightmap_first[lightmap_index + 1] ||
		world_geometry.lookup[entry] == NONE)
	{
		return stock;
	}
	made = &world_geometry.materials[world_geometry.lookup[entry]];
	/* (the passes before the surfaces: the sun's shadow maps and the
	occlusion's pre-pass draw the shape their settings choose) */
	proxy = render_prism_shadow_pass ? world_geometry.shadow_proxy :
		render_world_ao_prepass ? world_geometry.occlusion_proxy : _world_proxy_remodel;
	if (proxy == _world_proxy_stock)
	{
		return stock;
	}
	/* (the sun's casters face it, not the camera: none is skipped there) */
	facing = world_geometry.facing && !render_prism_shadow_pass;

	/* each surface's triangles, in the part that has them, joined into runs
	where they meet; a surface whose triangles all face away is left out,
	or drawn through where it is few triangles between two of a run */
	world_geometry.run_count = 0;
	gap = MAXIMUM_FILLED_TRIANGLES + 1;
	part = 0;
	for (index = 0; index < surface_count; index++)
	{
		long surface = surface_indices[index] - made->first_surface_index;
		struct world_material *of;
		struct world_run *last;
		long begin, end, local;
		byte choice;
		boolean far;

		while (part < made->part_count - 1 && surface >= made[part].part_first_surface + made[part].surface_count)
		{
			part++;
		}
		of = &made[part];
		if (surface < of->part_first_surface || surface >= of->part_first_surface + of->surface_count)
		{
			continue;
		}
		local = surface - of->part_first_surface;
		choice = surface_choice(of, local);
		far = of->far_triangles && (proxy == _world_proxy_far || TEST_FLAG(choice, _surface_choice_far_bit));
		if (far)
		{
			begin = (long)of->far_triangles[2 * local];
			end = begin + (long)of->far_triangles[2 * local + 1];
		}
		else
		{
			begin = (long)of->surface_first_triangle[local];
			end = (long)of->surface_first_triangle[local + 1];
		}
		if (of->far_triangles)
		{
			world_geometry.surfaces++;
			world_geometry.far_surfaces += far;
		}
		if (end <= begin)
		{
			continue;
		}
		last = world_geometry.run_count ? &world_geometry.runs[world_geometry.run_count - 1] : NULL;
		if (facing && TEST_FLAG(choice, far ? _surface_choice_far_away_bit : _surface_choice_near_away_bit))
		{
			world_geometry.away_surfaces++;
			/* (a run may go on through it, if the next surface drawn follows) */
			if (last && last->part == part && last->first_triangle + last->triangle_count + gap == begin)
			{
				gap += end - begin;
			}
			else
			{
				gap = MAXIMUM_FILLED_TRIANGLES + 1;
			}
			continue;
		}
		if (last && last->part == part && gap <= MAXIMUM_FILLED_TRIANGLES &&
			last->first_triangle + last->triangle_count + gap == begin)
		{
			world_geometry.filled_triangles += (unsigned long)gap;
			last->triangle_count += gap + end - begin;
		}
		else
		{
			world_geometry.runs[world_geometry.run_count].part = part;
			world_geometry.runs[world_geometry.run_count].first_triangle = begin;
			world_geometry.runs[world_geometry.run_count].triangle_count = end - begin;
			world_geometry.run_count++;
		}
		gap = 0;
	}
	world_geometry.current = made;
	world_geometry.stock_triangles += (unsigned long)surface_count;

	return made->buffers;
}

int prism_world_geometry_draw(
	struct vertex_buffer const *vertex_buffer0,
	struct vertex_buffer const *vertex_buffer1)
{
	struct world_material *made = world_geometry.current;
	long run, next;

	if (!world_geometry.materials || !vertex_buffer0)
	{
		return FALSE;
	}
	if (!made || vertex_buffer0 != made->buffers)
	{
		/* (another material's remodel, which no pass should draw now:
		nothing; any other vertices are the game's) */
		return (byte const *)vertex_buffer0 >= (byte const *)world_geometry.materials &&
			(byte const *)vertex_buffer0 < (byte const *)(world_geometry.materials + world_geometry.material_count);
	}
	for (run = 0; run < world_geometry.run_count; run = next)
	{
		struct world_material *part = &made[world_geometry.runs[run].part];
		struct vertex_buffer const *second = vertex_buffer1 == vertex_buffer0 ? &part->buffers[0] : &part->buffers[1];
		struct world_resource *indices = &part->index_resource;
		long first = world_geometry.runs[run].first_triangle;
		long count = world_geometry.runs[run].triangle_count;
		long total = count;

		/* (the part's runs: drawn one by one from its own triangles, or
		copied one after another and drawn as one) */
		for (next = run + 1; next < world_geometry.run_count && world_geometry.runs[next].part == world_geometry.runs[run].part;
			next++)
		{
			total += world_geometry.runs[next].triangle_count;
		}
		if (world_geometry.batch && next - run > 1)
		{
			unsigned long wanted = (unsigned long)total * 3;

			if (world_geometry.batch_indices && wanted <= world_geometry.batch_capacity)
			{
				word const *from = (word const *)part->index_resource.data;
				unsigned long at = 0;
				long each;

				for (each = run; each < next; each++)
				{
					memcpy(world_geometry.batch_indices + at, from + 3 * world_geometry.runs[each].first_triangle,
						(size_t)world_geometry.runs[each].triangle_count * 3 * sizeof(word));
					at += (unsigned long)world_geometry.runs[each].triangle_count * 3;
				}
				world_geometry.batch_resource.common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
				world_geometry.batch_resource.data = (unsigned long)world_geometry.batch_indices;
				indices = &world_geometry.batch_resource;
				first = 0;
				count = total;
				world_geometry.batched_runs += (unsigned long)(next - run - 1);
			}
			else
			{
				next = run + 1;
			}
		}
		else
		{
			next = run + 1;
		}

		/* (the lightmap's vertices where the pass draws them: vertices + 1,
		or the vertices again when the lightmap is missing) */
		IDirect3DDevice8_SetStreamSource(global_d3d_device, 0,
			(IDirect3DVertexBuffer8 *)part->buffers[0].hardware_format,
			rasterizer_geometry_get_vertex_size(part->buffers[0].type));
		if (vertex_buffer1)
		{
			IDirect3DDevice8_SetStreamSource(global_d3d_device, 1,
				(IDirect3DVertexBuffer8 *)second->hardware_format,
				rasterizer_geometry_get_vertex_size(second->type));
		}
		IDirect3DDevice8_SetIndices(global_d3d_device, (IDirect3DIndexBuffer8 *)indices, 0);
		while (count > 0)
		{
			long local = MIN(count, MAXIMUM_TRIANGLES_PER_DRAW);

			IDirect3DDevice8_DrawIndexedPrimitive(global_d3d_device, D3DPT_TRIANGLELIST, 0,
				part->buffers[0].count, (UINT)(3 * first), (UINT)local);
			world_geometry.draws++;
			world_geometry.triangles += (unsigned long)local;
			first += local;
			count -= local;
		}
	}

	return TRUE;
}

long prism_world_geometry_triangles(
	short lightmap_index,
	short material_index,
	int far_level,
	float *to,
	long most)
{
	long entry, part, written = 0;
	struct world_material *first_part;

	if (!world_geometry.materials || !world_geometry.lookup || lightmap_index < 0 ||
		lightmap_index >= world_geometry.lightmap_count)
	{
		return -1;
	}
	entry = world_geometry.lightmap_first[lightmap_index] + material_index;
	if (material_index < 0 || entry >= world_geometry.lightmap_first[lightmap_index + 1] ||
		world_geometry.lookup[entry] == NONE)
	{
		return -1;
	}
	first_part = &world_geometry.materials[world_geometry.lookup[entry]];
	for (part = 0; part < first_part->part_count; part++)
	{
		struct world_material const *made = first_part + part;
		long vertex_size = rasterizer_geometry_get_vertex_size(made->buffers[0].type);
		byte const *vertices = (byte const *)made->buffers[0].base_address;
		word const *indices = (word const *)made->index_resource.data;
		long surface;

		if (!vertices || !indices || vertex_size < 12)
		{
			continue;
		}
		for (surface = 0; surface < made->surface_count; surface++)
		{
			unsigned long first, count, triangle;

			if (far_level && made->far_triangles)
			{
				first = made->far_triangles[surface * 2];
				count = made->far_triangles[surface * 2 + 1];
			}
			else
			{
				first = made->surface_first_triangle[surface];
				count = made->surface_first_triangle[surface + 1] - first;
			}
			if (first > (unsigned long)made->triangle_count || count > (unsigned long)made->triangle_count - first)
			{
				continue;
			}
			for (triangle = first; triangle < first + count; triangle++)
			{
				int corner;

				if (to && written >= most)
				{
					return written;
				}
				if (to)
				{
					for (corner = 0; corner < 3; corner++)
					{
						unsigned long vertex = indices[triangle * 3 + corner];

						if (vertex >= (unsigned long)made->buffers[0].count)
						{
							vertex = 0;
						}
						memcpy(to + written * 9 + corner * 3, vertices + vertex * vertex_size, 3 * sizeof(float));
					}
				}
				written++;
			}
		}
	}

	return written;
}

/* the remodel's part of a material that has a surface (its index among the
material's), and the surface's index in the part; NULL where none */
static struct world_material *surface_part(
	short lightmap_index,
	short material_index,
	long surface,
	long *local)
{
	long entry, part;
	struct world_material *first_part;

	if (!world_geometry.materials || !world_geometry.lookup || lightmap_index < 0 ||
		lightmap_index >= world_geometry.lightmap_count || surface < 0)
	{
		return NULL;
	}
	entry = world_geometry.lightmap_first[lightmap_index] + material_index;
	if (material_index < 0 || entry >= world_geometry.lightmap_first[lightmap_index + 1] ||
		world_geometry.lookup[entry] == NONE)
	{
		return NULL;
	}
	first_part = &world_geometry.materials[world_geometry.lookup[entry]];
	for (part = 0; part < first_part->part_count; part++)
	{
		struct world_material *made = first_part + part;

		if (surface >= made->part_first_surface && surface < made->part_first_surface + made->surface_count)
		{
			*local = surface - made->part_first_surface;
			return made;
		}
	}

	return NULL;
}

/* a remodelled surface's triangles of a level: [*first, *first + *count)
of its part's; FALSE where out of the part's */
static boolean surface_range(
	struct world_material const *made,
	long local,
	int far_level,
	unsigned long *first,
	unsigned long *count)
{
	if (far_level && made->far_triangles)
	{
		*first = made->far_triangles[local * 2];
		*count = made->far_triangles[local * 2 + 1];
	}
	else
	{
		*first = made->surface_first_triangle[local];
		*count = made->surface_first_triangle[local + 1] - *first;
	}

	return *first <= (unsigned long)made->triangle_count && *count <= (unsigned long)made->triangle_count - *first;
}

long prism_world_geometry_surface_triangles(
	short lightmap_index,
	short material_index,
	long surface,
	int level,
	float *positions,
	float *base,
	float *lightmap,
	long most)
{
	return prism_world_geometry_surface_triangles_from(lightmap_index, material_index, surface, level, positions, base,
		lightmap, 0, most);
}

long prism_world_geometry_surface_triangles_from(
	short lightmap_index,
	short material_index,
	long surface,
	int level,
	float *positions,
	float *base,
	float *lightmap,
	long skip,
	long most)
{
	long local, vertex_size, lightmap_size, written = 0;
	struct world_material const *made = surface_part(lightmap_index, material_index, surface, &local);
	unsigned long first, count, near_first, near_count, triangle;
	byte const *vertices, *lightmap_vertices;
	word const *indices;

	if (!made)
	{
		return -1;
	}
	vertex_size = rasterizer_geometry_get_vertex_size(made->buffers[0].type);
	lightmap_size = rasterizer_geometry_get_vertex_size(made->buffers[1].type);
	vertices = (byte const *)made->buffers[0].base_address;
	lightmap_vertices = (byte const *)made->buffers[1].base_address;
	indices = (word const *)made->index_resource.data;
	if (!vertices || !indices || vertex_size < 12 || !surface_range(made, local, FALSE, &near_first, &near_count))
	{
		return -1;
	}
	if (level == PRISM_WORLD_GEOMETRY_LEVEL_FAR)
	{
		/* (none apart: no far level, or its far level its near one) */
		if (!made->far_triangles || !surface_range(made, local, TRUE, &first, &count) ||
			(first == near_first && count == near_count))
		{
			return 0;
		}
	}
	else
	{
		first = near_first;
		count = near_count;
	}
	/* (NULL: counted) */
	if (!positions)
	{
		return (long)count;
	}
	if (skip < 0 || (unsigned long)skip >= count)
	{
		return 0;
	}
	for (triangle = first + (unsigned long)skip; triangle < first + count; triangle++)
	{
		float normal[3] = { 0.0f, 0.0f, 0.0f };
		int corner;

		if (written >= most)
		{
			break;
		}
		for (corner = 0; corner < 3; corner++)
		{
			unsigned long vertex = indices[triangle * 3 + corner];
			byte const *at;

			if (vertex >= (unsigned long)made->buffers[0].count)
			{
				vertex = 0;
			}
			at = vertices + vertex * vertex_size;
			memcpy(positions + written * 9 + corner * 3, at, 3 * sizeof(float));
			/* (its vertices' normals summed: the compressed vertex's at 12,
			packed 11:11:10; the uncompressed's at 12, floats) */
			if (vertex_size == 32)
			{
				real_vector3d unpacked = uncompress_int32_to_real_vector3d(*(unsigned long const *)(at + 12));

				normal[0] += unpacked.i;
				normal[1] += unpacked.j;
				normal[2] += unpacked.k;
			}
			else if (vertex_size >= 24)
			{
				float const *n = (float const *)(at + 12);

				normal[0] += n[0];
				normal[1] += n[1];
				normal[2] += n[2];
			}
			/* (the base map's coordinates: the compressed vertex's at 24, the
			uncompressed's at 48) */
			if (vertex_size == 32 || vertex_size >= 56)
			{
				memcpy(base + written * 6 + corner * 2, at + (vertex_size == 32 ? 24 : 48), 2 * sizeof(float));
			}
			else
			{
				base[written * 6 + corner * 2] = base[written * 6 + corner * 2 + 1] = 0.0f;
			}
			lightmap[written * 6 + corner * 2] = lightmap[written * 6 + corner * 2 + 1] = 0.0f;
			if (lightmap_vertices && vertex < (unsigned long)made->buffers[1].count)
			{
				at = lightmap_vertices + vertex * lightmap_size;
				if (lightmap_size == 8)
				{
					/* (the compressed lightmap vertex's coordinates: two shorts at 4,
					as environment_lightmap_vertex_compressed_get_texcoord reads them) */
					short const *uv = (short const *)(at + 4);

					lightmap[written * 6 + corner * 2] = ((float)uv[0] * 2.0f + 1.0f) * (1.0f / 65535.0f);
					lightmap[written * 6 + corner * 2 + 1] = ((float)uv[1] * 2.0f + 1.0f) * (1.0f / 65535.0f);
				}
				else if (lightmap_size >= 20)
				{
					memcpy(lightmap + written * 6 + corner * 2, at + 12, 2 * sizeof(float));
				}
			}
		}
		/* (wound to its vertices' normals, as SCENE_STATIC winds the map's
		own triangles: the remodel's are D3D's clockwise faces too, whose
		corners' cross product points into the level, and the baker's texels
		took their normals, the side their rays leave from, from it) */
		{
			float const *p = positions + written * 9;
			float e1[3], e2[3], face[3];
			int axis;

			for (axis = 0; axis < 3; axis++)
			{
				e1[axis] = p[3 + axis] - p[axis];
				e2[axis] = p[6 + axis] - p[axis];
			}
			face[0] = e1[1] * e2[2] - e1[2] * e2[1];
			face[1] = e1[2] * e2[0] - e1[0] * e2[2];
			face[2] = e1[0] * e2[1] - e1[1] * e2[0];
			if (face[0] * normal[0] + face[1] * normal[1] + face[2] * normal[2] < 0.0f)
			{
				float swap[3];

				memcpy(swap, positions + written * 9 + 3, 3 * sizeof(float));
				memcpy(positions + written * 9 + 3, positions + written * 9 + 6, 3 * sizeof(float));
				memcpy(positions + written * 9 + 6, swap, 3 * sizeof(float));
				memcpy(swap, base + written * 6 + 2, 2 * sizeof(float));
				memcpy(base + written * 6 + 2, base + written * 6 + 4, 2 * sizeof(float));
				memcpy(base + written * 6 + 4, swap, 2 * sizeof(float));
				memcpy(swap, lightmap + written * 6 + 2, 2 * sizeof(float));
				memcpy(lightmap + written * 6 + 2, lightmap + written * 6 + 4, 2 * sizeof(float));
				memcpy(lightmap + written * 6 + 4, swap, 2 * sizeof(float));
			}
		}
		written++;
	}

	return written;
}

int prism_world_geometry_surface_level(
	short lightmap_index,
	short material_index,
	long surface)
{
	long local;
	struct world_material *made = surface_part(lightmap_index, material_index, surface, &local);

	if (!made || !made->far_triangles || !world_geometry.enabled || !world_geometry.stamp)
	{
		return -1;
	}

	return TEST_FLAG(surface_choice(made, local), _surface_choice_far_bit) ? 1 : 0;
}

int prism_world_geometry_drawn(
	void)
{
	settings_read();
	enabled_update();

	return world_geometry.enabled && world_geometry.loaded_bsp && world_geometry.loaded_bsp == global_structure_bsp_get();
}

void prism_world_geometry_map_unloaded(
	void)
{
	world_geometry_release();
	prism_world_source_map_unloaded();
	world_geometry.tried_tag_index = NONE;
	world_geometry.tried_bsp = NULL;

	return;
}
