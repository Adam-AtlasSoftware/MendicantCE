/*
PRISM_SCENE_STATIC.C

PRISM v4's SCENE_STATIC (overhaul_research/prism_v4_renderer/design.md,
section 1.2; xgpu_stream.h; docs/prism_scene_records.md): the level's
geometry as it is drawn, sent once a BSP for every consumer the host has:
display.prism_rt's acceleration structures now (prism_rt.c's traced level),
M2's baker, the G-buffer's shadows. It is sent with display.lighting
"ce_deferred" (the v4 modes) or display.prism_rt on, never otherwise: with
"ce" and ray tracing off nothing here runs, and Classic's frames stay byte
for byte as they were.

The set: every surface of the structure BSP's opaque, alpha-tested and
light-card materials (the environment shaders, and the others whose
radiosity lit the level), in the BSP's own order (lightmap, material,
surface). A surface the remodel draws (prism_world_geometry.c, where it is
drawn) is its remodel's triangles, its near level's then its far level's
where that differs (each marked with its level); any other is the map's own
triangle. Each triangle carries its material (SCENE_MATERIAL's id,
prism_deferred.c's), its surface, its kind, its lightmap page and the three
corners' lightmap and base map coordinates, and display.prism_rt's own
material where the ray tracer traces it. The lightmap pages' sizes come
first, so that the baker can scale CE's layout (its atlas).

Sent a slice a frame (at most XGPU_SCENE_STATIC_SLICE triangles, whole
surfaces): BEGIN and the pages on the frame the set begins, then the
triangles, then their attributes, then END. A set begins again where the
BSP, the remodel drawn or the ray tracer's traced kinds change; nothing is
kept in memory but the slice being made.

And each window, where it changed, the level every surface is drawn at
(LEVELS): the remodel's rule (prism_world_geometry_surface_level: the
camera's distance, the band, the hysteresis), for the surfaces off screen
too, so that the ray tracer's structure (folded to these levels and
refitted, xgpu_vulkan_rt.c) is the level the window draws, near and far.

Nothing here changes what the game simulates: it reads the structure's
render geometry and the remodel's.
*/

#include "cseries.h"
#include "rasterizer/rasterizer_geometry.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_internal.h"
#include "tag_files/tag_files.h"
#include "prism_world_geometry.h"
#include "prism_deferred.h"
#include "prism_scene_static.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src: the stream's (xgpu_stream_encode.c) */
void xgpu_backend_scene_static(unsigned long phase, unsigned long generation, unsigned long first,
	unsigned long count, unsigned long total, const void *data);
void platform_log(const char *format, ...);
/* prism_rt.c's */
int prism_rt_mode(void);
long prism_rt_traced_material(long shader_tag, int *kind);
int prism_rt_scene_traced(void);
/* render_enumerate.c's (the window drawn is a map's first frame's, unseen) */
extern unsigned char render_enumerating;
/* prism_deferred.c's */
unsigned long prism_deferred_material_id(void const *shader);

/* ---------- constants */

/* (xgpu_stream.h's: the game's sources do not see the renderer's headers) */
enum
{
	PHASE_BEGIN,
	PHASE_TRIANGLES,
	PHASE_ATTRIBUTES,
	PHASE_END,
	PHASE_RELEASE,
	PHASE_PAGES,
	PHASE_LEVELS,
	SLICE = 65536,
	ATTRIBUTE_WORDS = 16,
	ATTRIBUTE_MATERIAL = 0,
	ATTRIBUTE_SURFACE = 1,
	ATTRIBUTE_KIND_PAGE = 2,
	ATTRIBUTE_ATLAS = 3,
	ATTRIBUTE_BASE = 9,
	ATTRIBUTE_TRACED = 15,
	LEVEL_BOTH = 0,
	LEVEL_NEAR = 1,
	LEVEL_FAR = 2,
	LEVEL_SHIFT = 8,
	INDOOR = 1 << 10,
	PAGE_SHIFT = 16,
	PAGE_NONE = 0xffff,
	KIND_OPAQUE = 0,
	KIND_ALPHA_TESTED = 1,
	KIND_LIGHT = 2,
	/* (shader_definitions.h's types: the environment's; its flags at 0x28,
	bit 0 alpha-tested) */
	SHADER_ENVIRONMENT = 3,
	ENVIRONMENT_FLAGS = 0x28,
	ENVIRONMENT_ALPHA_TESTED = 1,
	/* the pages at most (SCENE_STATIC's slice) */
	PAGES_MOST = 4096
};
#define TRACED_NONE 0xffffffffUL

/* where the slices have come to: the BSP's lightmap, material and surface
next */
struct cursor
{
	long lightmap_index, material_index, surface;
	/* (the current surface's triangles sent already, near level then far:
	a remodelled surface past a slice goes in parts) */
	long done;
};

/* ---------- globals */

static struct
{
	/* a set begun (and not released), its generation, what it is of (the
	BSP, whether the remodel is drawn, the traced kinds), its totals, the
	phase it has come to (PHASE_TRIANGLES, _ATTRIBUTES, _END: ended) and the
	slices' cursor and count sent */
	boolean begun;
	unsigned long generation;
	short bsp_index;
	struct structure_bsp *bsp;
	/* port: the BSP the last set was of, kept across a release (a lighting
	change), until the map goes; and whether the set is of it again (a
	setting changed in play: prism_scene_static_in_play) */
	struct structure_bsp *played_bsp;
	boolean in_play;
	boolean remodel;
	int traced;
	long total, surfaces;
	int phase;
	struct cursor cursor;
	long sent;
	/* the slice's room */
	float *positions;
	unsigned long *attributes;
	float *base;
	float *lightmap;
	/* the surfaces with two levels (their lightmap and material indices
	and their index among the material's, and the BSP's), the levels' bits
	and the last sent (none yet: level_sent FALSE) */
	long level_count;
	short *level_lightmaps;
	short *level_materials;
	long *level_locals;
	long *level_surfaces;
	unsigned long *levels;
	unsigned long *levels_sent;
	long level_words;
	boolean level_sent;
} scene_static;

/* ---------- private code */

/* (the game's free takes no NULL) */
static void free_kept(
	void *memory)
{
	if (memory)
	{
		free(memory);
	}
}

/* a material's kind (KIND_*), or NONE where it is not in the set (a
transparent shader that lit nothing) */
static int material_kind(
	struct structure_material const *material)
{
	struct shader const *shader;

	if (material->shader.index == NONE || !(shader = shader_definition_get(material->shader.index)))
	{
		return NONE;
	}
	if (shader->base.type == SHADER_ENVIRONMENT)
	{
		return (*(unsigned short const *)((byte const *)shader + ENVIRONMENT_FLAGS) & ENVIRONMENT_ALPHA_TESTED) ?
			KIND_ALPHA_TESTED : KIND_OPAQUE;
	}

	return shader->base.radiosity.power > 0.0f ? KIND_LIGHT : NONE;
}

/* a material's vertices, readable (its surfaces in the BSP's) */
static boolean material_readable(
	struct structure_bsp const *bsp,
	struct structure_material const *material)
{
	long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);

	return material->compressed_vertex_data.address && vertex_size >= 12 && material->vertices.count > 0 &&
		material->surface_count > 0 && material->first_surface_index >= 0 &&
		material->first_surface_index + material->surface_count <= bsp->surfaces.count &&
		(unsigned long)material->compressed_vertex_data.size >=
		(unsigned long)material->vertices.count * (unsigned long)vertex_size;
}

/* a stock surface's triangle: its corners' places and base map and
lightmap coordinates (the compressed vertices: base map's at 24, the
lightmap vertices after the vertices, their coordinates two shorts at 4;
the uncompressed: at 48, and floats at 12) */
static boolean stock_triangle(
	struct structure_bsp const *bsp,
	struct structure_material const *material,
	long surface_index,
	float positions[9],
	float base[6],
	float lightmap[6])
{
	long vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
	long lightmap_size = material->lightmap_vertices.count > 0 ?
		rasterizer_geometry_get_vertex_size(material->lightmap_vertices.type) : 0;
	byte const *vertices = (byte const *)material->compressed_vertex_data.address;
	byte const *lightmap_vertices = vertices + material->vertices.count * vertex_size;
	struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
		material->first_surface_index + surface_index, struct structure_surface);
	boolean lightmapped = lightmap_size > 0 && (unsigned long)material->compressed_vertex_data.size >=
		(unsigned long)material->vertices.count * (unsigned long)(vertex_size + lightmap_size);
	int corner;

	float normal[3] = { 0.0f, 0.0f, 0.0f };

	for (corner = 0; corner < 3; corner++)
	{
		long index = (unsigned short)surface->vertex_indices[corner];
		byte const *vertex;

		if (index >= material->vertices.count)
		{
			return FALSE;
		}
		vertex = vertices + index * vertex_size;
		memcpy(positions + corner * 3, vertex, 3 * sizeof(float));
		/* (its vertices' normals summed: the compressed vertex's at 12, packed
		11:11:10; the uncompressed's at 12, floats) */
		if (vertex_size == 32)
		{
			real_vector3d unpacked = uncompress_int32_to_real_vector3d(*(unsigned long const *)(vertex + 12));

			normal[0] += unpacked.i;
			normal[1] += unpacked.j;
			normal[2] += unpacked.k;
		}
		else if (vertex_size >= 24)
		{
			float const *n = (float const *)(vertex + 12);

			normal[0] += n[0];
			normal[1] += n[1];
			normal[2] += n[2];
		}
		if (vertex_size == 32 || vertex_size >= 56)
		{
			memcpy(base + corner * 2, vertex + (vertex_size == 32 ? 24 : 48), 2 * sizeof(float));
		}
		else
		{
			base[corner * 2] = base[corner * 2 + 1] = 0.0f;
		}
		lightmap[corner * 2] = lightmap[corner * 2 + 1] = 0.0f;
		if (lightmapped && index < material->lightmap_vertices.count)
		{
			byte const *at = lightmap_vertices + index * lightmap_size;

			if (lightmap_size == 8)
			{
				short const *uv = (short const *)(at + 4);

				lightmap[corner * 2] = ((float)uv[0] * 2.0f + 1.0f) * (1.0f / 65535.0f);
				lightmap[corner * 2 + 1] = ((float)uv[1] * 2.0f + 1.0f) * (1.0f / 65535.0f);
			}
			else if (lightmap_size >= 20)
			{
				memcpy(lightmap + corner * 2, at + 12, 2 * sizeof(float));
			}
		}
	}
	/* port: (PRISM v4 M2) the corners turned where they are wound against
	their vertices' normals, as the map's own triangles all are (D3D's
	clockwise faces: their corners' cross product points into the level), so
	that every consumer's face normal, the corners' cross product, points
	out of the level: the ray tracer's one-sided faces, the baker's texels,
	a lamp's emission */
	{
		float e1[3], e2[3], face[3], swap[2];
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			e1[axis] = positions[3 + axis] - positions[axis];
			e2[axis] = positions[6 + axis] - positions[axis];
		}
		face[0] = e1[1] * e2[2] - e1[2] * e2[1];
		face[1] = e1[2] * e2[0] - e1[0] * e2[2];
		face[2] = e1[0] * e2[1] - e1[1] * e2[0];
		if (face[0] * normal[0] + face[1] * normal[1] + face[2] * normal[2] < 0.0f)
		{
			float corner_swap[3];

			memcpy(corner_swap, positions + 3, sizeof(corner_swap));
			memcpy(positions + 3, positions + 6, sizeof(corner_swap));
			memcpy(positions + 6, corner_swap, sizeof(corner_swap));
			memcpy(swap, base + 2, sizeof(swap));
			memcpy(base + 2, base + 4, sizeof(swap));
			memcpy(base + 4, swap, sizeof(swap));
			memcpy(swap, lightmap + 2, sizeof(swap));
			memcpy(lightmap + 2, lightmap + 4, sizeof(swap));
			memcpy(lightmap + 4, swap, sizeof(swap));
		}
	}

	return TRUE;
}

/* a surface's triangles of each level (the remodel's near and far, or the
map's one: near alone); FALSE where the surface is the map's */
static boolean surface_counts(
	short lightmap_index,
	short material_index,
	long surface,
	long *near_count,
	long *far_count)
{
	long near_triangles, far_triangles;

	if (!scene_static.remodel)
	{
		return FALSE;
	}
	near_triangles = prism_world_geometry_surface_triangles(lightmap_index, material_index, surface,
		PRISM_WORLD_GEOMETRY_LEVEL_NEAR, NULL, NULL, NULL, 0);
	if (near_triangles < 0)
	{
		return FALSE;
	}
	far_triangles = prism_world_geometry_surface_triangles(lightmap_index, material_index, surface,
		PRISM_WORLD_GEOMETRY_LEVEL_FAR, NULL, NULL, NULL, 0);
	*near_count = near_triangles;
	*far_count = far_triangles > 0 ? far_triangles : 0;

	return TRUE;
}

static void set_free(
	void)
{
	free_kept(scene_static.positions);
	free_kept(scene_static.attributes);
	free_kept(scene_static.base);
	free_kept(scene_static.lightmap);
	free_kept(scene_static.level_lightmaps);
	free_kept(scene_static.level_materials);
	free_kept(scene_static.level_locals);
	free_kept(scene_static.level_surfaces);
	free_kept(scene_static.levels);
	free_kept(scene_static.levels_sent);
	scene_static.positions = NULL;
	scene_static.attributes = NULL;
	scene_static.base = NULL;
	scene_static.lightmap = NULL;
	scene_static.level_lightmaps = NULL;
	scene_static.level_materials = NULL;
	scene_static.level_locals = NULL;
	scene_static.level_surfaces = NULL;
	scene_static.levels = NULL;
	scene_static.levels_sent = NULL;
	scene_static.level_count = scene_static.level_words = 0;
	scene_static.level_sent = FALSE;
	scene_static.begun = FALSE;
	scene_static.bsp = NULL;
	scene_static.bsp_index = NONE;
}

static void release(
	void)
{
	if (scene_static.begun)
	{
		xgpu_backend_scene_static(PHASE_RELEASE, ++scene_static.generation, 0, 0, 0, NULL);
	}
	set_free();
}

/* the set's totals counted and its surfaces of two levels listed; FALSE
where it has none (or no memory) */
static boolean set_count(
	struct structure_bsp *bsp)
{
	long lightmap_index, pass;

	scene_static.total = 0;
	scene_static.level_count = 0;
	scene_static.surfaces = bsp->surfaces.count;
	for (pass = 0; pass < 2; pass++)
	{
		long levels = 0;

		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
				struct structure_lightmap);
			long material_index;

			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
					struct structure_material);
				long surface;

				if (!material_readable(bsp, material) || material_kind(material) == NONE)
				{
					continue;
				}
				for (surface = 0; surface < material->surface_count; surface++)
				{
					long near_count, far_count;

					if (!surface_counts((short)lightmap_index, (short)material_index, surface, &near_count, &far_count))
					{
						scene_static.total += !pass;
						continue;
					}
					if (!pass)
					{
						scene_static.total += near_count + far_count;
					}
					if (far_count > 0)
					{
						if (pass)
						{
							scene_static.level_lightmaps[levels] = (short)lightmap_index;
							scene_static.level_materials[levels] = (short)material_index;
							scene_static.level_locals[levels] = surface;
							scene_static.level_surfaces[levels] = material->first_surface_index + surface;
						}
						levels++;
					}
				}
			}
		}
		if (!pass)
		{
			scene_static.level_count = levels;
			scene_static.level_words = (scene_static.surfaces + 31) / 32;
			if (levels)
			{
				scene_static.level_lightmaps = malloc(levels * sizeof(short));
				scene_static.level_materials = malloc(levels * sizeof(short));
				scene_static.level_locals = malloc(levels * sizeof(long));
				scene_static.level_surfaces = malloc(levels * sizeof(long));
				scene_static.levels = malloc(scene_static.level_words * sizeof(unsigned long));
				scene_static.levels_sent = malloc(scene_static.level_words * sizeof(unsigned long));
				if (!scene_static.level_lightmaps || !scene_static.level_materials || !scene_static.level_locals ||
					!scene_static.level_surfaces || !scene_static.levels || !scene_static.levels_sent)
				{
					return FALSE;
				}
			}
			else
			{
				/* (one pass: nothing to list) */
				break;
			}
		}
	}

	return scene_static.total > 0;
}

/* the BSP's lightmap pages' sizes sent (a bitmap missing: 0 by 0) */
static void pages_send(
	struct structure_bsp *bsp)
{
	static unsigned long sizes[PAGES_MOST * 2];
	struct bitmap_group *group;
	long count, index;

	if (bsp->lightmap_group.index == NONE || !(group = tag_get('bitm', bsp->lightmap_group.index)))
	{
		return;
	}
	count = MIN(group->bitmaps.count, PAGES_MOST);
	for (index = 0; index < count; index++)
	{
		struct bitmap_data *bitmap = bitmap_group_try_and_get_bitmap(bsp->lightmap_group.index, (short)index);

		sizes[index * 2] = bitmap ? (unsigned long)MAX(bitmap->width, 0) : 0;
		sizes[index * 2 + 1] = bitmap ? (unsigned long)MAX(bitmap->height, 0) : 0;
	}
	if (count > 0)
	{
		xgpu_backend_scene_static(PHASE_PAGES, scene_static.generation, 0, (unsigned long)count, (unsigned long)count,
			sizes);
	}
}

/* port: whether a triangle is in one of the structure's indoor clusters
(a cluster with no sky: xgpu_stream.h's XGPU_SCENE_INDOOR): the cluster of
its middle a little off its face, on one side and then the other (a side in
solid has none); FALSE where neither side is in a cluster */
static boolean triangle_indoor(
	struct structure_bsp *bsp,
	float const *corners)
{
	struct collision_bsp *collision = bsp->collision_bsp.count > 0 && bsp->collision_bsp.address ?
		TAG_BLOCK_GET_ELEMENT(&bsp->collision_bsp, 0, struct collision_bsp) : NULL;
	float e1[3], e2[3], n[3], length;
	int axis, side;

	if (!collision || !bsp->clusters.address || !bsp->leaves.address)
	{
		return FALSE;
	}
	for (axis = 0; axis < 3; axis++)
	{
		e1[axis] = corners[3 + axis] - corners[axis];
		e2[axis] = corners[6 + axis] - corners[axis];
	}
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	length = (float)sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (!(length > 1.0e-12f))
	{
		return FALSE;
	}
	for (side = 0; side < 2; side++)
	{
		float offset = (side ? -0.05f : 0.05f) / length;
		real_point3d point;
		long leaf;
		short cluster;

		point.x = (corners[0] + corners[3] + corners[6]) / 3.0f + n[0] * offset;
		point.y = (corners[1] + corners[4] + corners[7]) / 3.0f + n[1] * offset;
		point.z = (corners[2] + corners[5] + corners[8]) / 3.0f + n[2] * offset;
		leaf = bsp3d_test_point(&collision->bsp3d, 0, &point);
		if (leaf == NONE || (leaf & LONG_MAX) >= bsp->leaves.count)
		{
			continue;
		}
		cluster = TAG_BLOCK_GET_ELEMENT(&bsp->leaves, leaf & LONG_MAX, struct structure_leaf)->cluster_index;
		if (cluster < 0 || cluster >= bsp->clusters.count)
		{
			continue;
		}
		return TAG_BLOCK_GET_ELEMENT(&bsp->clusters, cluster, struct structure_cluster)->sky_index == NONE;
	}
	return FALSE;
}

/* a triangle's attributes */
static void attributes_write(
	unsigned long *to,
	unsigned long material,
	long surface,
	int kind,
	int level,
	short page,
	float const lightmap[6],
	float const base[6],
	unsigned long traced,
	boolean indoor)
{
	memset(to, 0, ATTRIBUTE_WORDS * sizeof(unsigned long));
	to[ATTRIBUTE_MATERIAL] = material;
	to[ATTRIBUTE_SURFACE] = (unsigned long)surface;
	to[ATTRIBUTE_KIND_PAGE] = (unsigned long)kind | ((unsigned long)level << LEVEL_SHIFT) |
		((unsigned long)(page < 0 ? PAGE_NONE : page) << PAGE_SHIFT) | (indoor ? INDOOR : 0);
	memcpy(to + ATTRIBUTE_ATLAS, lightmap, 6 * sizeof(float));
	memcpy(to + ATTRIBUTE_BASE, base, 6 * sizeof(float));
	to[ATTRIBUTE_TRACED] = traced;
}

/* the next slice made (whole surfaces from the cursor, at most SLICE
triangles: their places, or their attributes); how many */
static long slice_make(
	struct structure_bsp *bsp,
	boolean attributes)
{
	struct cursor *cursor = &scene_static.cursor;
	long made = 0;

	while (cursor->lightmap_index < bsp->lightmaps.count)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, cursor->lightmap_index,
			struct structure_lightmap);
		struct structure_material *material;
		unsigned long id = 0, traced = TRACED_NONE;
		int kind;

		if (cursor->material_index >= lightmap->materials.count)
		{
			cursor->lightmap_index++;
			cursor->material_index = 0;
			cursor->surface = 0;
			continue;
		}
		material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, cursor->material_index, struct structure_material);
		kind = material_kind(material);
		if (!material_readable(bsp, material) || kind == NONE || cursor->surface >= material->surface_count)
		{
			cursor->material_index++;
			cursor->surface = 0;
			continue;
		}
		if (attributes)
		{
			int traced_kind;
			long index;

			id = prism_deferred_material_id(shader_definition_get(material->shader.index));
			if (scene_static.traced && (index = prism_rt_traced_material(material->shader.index, &traced_kind)) != NONE)
			{
				traced = (unsigned long)index;
			}
		}
		while (cursor->surface < material->surface_count)
		{
			long surface = cursor->surface, near_count, far_count, level;

			if (!surface_counts((short)cursor->lightmap_index, (short)cursor->material_index, surface, &near_count,
				&far_count))
			{
				float base[6], uv[6];

				if (made + 1 > SLICE)
				{
					return made;
				}
				if (!stock_triangle(bsp, material, surface, scene_static.positions + made * 9, base, uv))
				{
					/* (a surface out of its vertices: a triangle of no area, kept
					so that the totals hold) */
					memset(scene_static.positions + made * 9, 0, 9 * sizeof(float));
					memset(base, 0, sizeof(base));
					memset(uv, 0, sizeof(uv));
				}
				if (attributes)
				{
					attributes_write(scene_static.attributes + made * ATTRIBUTE_WORDS, id,
						material->first_surface_index + surface, kind, LEVEL_BOTH, lightmap->bitmap_index, uv, base,
						traced, triangle_indoor(bsp, scene_static.positions + made * 9));
				}
				made++;
				cursor->surface++;
				continue;
			}
			/* (a surface whole in a slice where it fits; one past a slice (a
			remodelled cliff's, 100,000 triangles and more) begun on a slice of
			its own and sent in parts, near level then far, so that every
			triangle counted is sent: a surface left out had left the set
			unwhole, and the bake waiting for it) */
			if (made > 0 && made + near_count + far_count - cursor->done > SLICE)
			{
				return made;
			}
			{
				long take = MIN(near_count + far_count - cursor->done, SLICE - made), from = cursor->done;

				for (level = 0; level < 2; level++)
				{
					long start = level ? near_count : 0, whole = level ? far_count : near_count, written, index;
					long low = MAX(from, start), high = MIN(from + take, start + whole), count = high - low;

					if (count <= 0)
					{
						continue;
					}
					written = prism_world_geometry_surface_triangles_from((short)cursor->lightmap_index,
						(short)cursor->material_index, surface, level ? PRISM_WORLD_GEOMETRY_LEVEL_FAR :
						PRISM_WORLD_GEOMETRY_LEVEL_NEAR, scene_static.positions + made * 9, scene_static.base + made * 6,
						scene_static.lightmap + made * 6, low - start, count);
					for (index = MAX(written, 0); index < count; index++)
					{
						memset(scene_static.positions + (made + index) * 9, 0, 9 * sizeof(float));
						memset(scene_static.base + (made + index) * 6, 0, 6 * sizeof(float));
						memset(scene_static.lightmap + (made + index) * 6, 0, 6 * sizeof(float));
					}
					if (attributes)
					{
						for (index = 0; index < count; index++)
						{
							attributes_write(scene_static.attributes + (made + index) * ATTRIBUTE_WORDS, id,
								material->first_surface_index + surface, kind,
								!far_count ? LEVEL_BOTH : level ? LEVEL_FAR : LEVEL_NEAR, lightmap->bitmap_index,
								scene_static.lightmap + (made + index) * 6, scene_static.base + (made + index) * 6, traced,
								triangle_indoor(bsp, scene_static.positions + (made + index) * 9));
						}
					}
					made += count;
				}
				cursor->done += take;
			}
			if (cursor->done < near_count + far_count)
			{
				/* (the slice full in the middle of the surface: the rest next) */
				return made;
			}
			cursor->done = 0;
			cursor->surface++;
		}
	}

	return made;
}

/* ---------- public code */

void prism_scene_static_frame(
	void)
{
	short bsp_index;
	struct structure_bsp *bsp;
	boolean remodel;
	int traced;
	long count;

	if (render_enumerating)
	{
		return;
	}
	if (!prism_deferred_wanted() && !prism_rt_mode())
	{
		release();
		return;
	}
	bsp_index = global_structure_bsp_index_get();
	bsp = bsp_index != NONE ? global_structure_bsp_get() : NULL;
	/* (port: the BSP's remodel taken up first, so that a new BSP's first set
	is the remodel's where it is drawn, not the stock one begun again a
	frame later, with the bake's key it opened; in PRISM Rebaked and Path
	Traced, Classic's ray tracing as it was) */
	if (prism_deferred_raster())
	{
		prism_world_geometry_prepare();
	}
	remodel = prism_world_geometry_drawn() != 0;
	/* (the traced materials: none, or the level's, with its alpha-tested
	surfaces or not) */
	traced = prism_rt_scene_traced();
	if (!bsp)
	{
		release();
		return;
	}
	if (!scene_static.begun || bsp_index != scene_static.bsp_index || bsp != scene_static.bsp ||
		remodel != scene_static.remodel || traced != scene_static.traced)
	{
		set_free();
		scene_static.generation++;
		/* (port: a set of the BSP the last one was of: begun again in play,
		by a setting; the bake then is never made, prism_bake.c) */
		scene_static.in_play = bsp == scene_static.played_bsp;
		scene_static.played_bsp = bsp;
		scene_static.bsp_index = bsp_index;
		scene_static.bsp = bsp;
		scene_static.remodel = remodel;
		scene_static.traced = traced;
		scene_static.begun = TRUE;
		scene_static.phase = PHASE_TRIANGLES;
		memset(&scene_static.cursor, 0, sizeof(scene_static.cursor));
		scene_static.sent = 0;
		scene_static.positions = malloc(SLICE * 9 * sizeof(float));
		scene_static.attributes = malloc(SLICE * ATTRIBUTE_WORDS * sizeof(unsigned long));
		scene_static.base = malloc(SLICE * 6 * sizeof(float));
		scene_static.lightmap = malloc(SLICE * 6 * sizeof(float));
		if (!scene_static.positions || !scene_static.attributes || !scene_static.base || !scene_static.lightmap ||
			!set_count(bsp))
		{
			xgpu_backend_scene_static(PHASE_RELEASE, scene_static.generation, 0, 0, 0, NULL);
			set_free();
			/* (begun, with nothing: not tried again until something changes) */
			scene_static.begun = TRUE;
			scene_static.bsp_index = bsp_index;
			scene_static.bsp = bsp;
			scene_static.remodel = remodel;
			scene_static.traced = traced;
			scene_static.phase = PHASE_END + 1;
			return;
		}
		platform_log("prism scene: bsp %d: %ld triangles (%s), %ld surfaces of two levels", (int)bsp_index,
			scene_static.total, remodel ? "the remodel's near and far levels" : "the map's", scene_static.level_count);
		xgpu_backend_scene_static(PHASE_BEGIN, scene_static.generation, 0, (unsigned long)scene_static.surfaces,
			(unsigned long)scene_static.total, NULL);
		pages_send(bsp);
		return;
	}
	switch (scene_static.phase)
	{
	case PHASE_TRIANGLES:
	case PHASE_ATTRIBUTES:
		count = slice_make(bsp, scene_static.phase == PHASE_ATTRIBUTES);
		if (count > 0)
		{
			xgpu_backend_scene_static((unsigned long)scene_static.phase, scene_static.generation,
				(unsigned long)scene_static.sent, (unsigned long)count, (unsigned long)scene_static.total,
				scene_static.phase == PHASE_ATTRIBUTES ? (void const *)scene_static.attributes :
				(void const *)scene_static.positions);
			scene_static.sent += count;
		}
		if (count <= 0 || scene_static.sent >= scene_static.total)
		{
			scene_static.phase = scene_static.phase == PHASE_TRIANGLES ? PHASE_ATTRIBUTES : PHASE_END;
			memset(&scene_static.cursor, 0, sizeof(scene_static.cursor));
			scene_static.sent = 0;
		}
		break;
	case PHASE_END:
		xgpu_backend_scene_static(PHASE_END, scene_static.generation, 0, 0, (unsigned long)scene_static.total, NULL);
		scene_static.phase = PHASE_END + 1;
		/* (the slices' room let go of; the levels' kept) */
		free_kept(scene_static.positions);
		free_kept(scene_static.attributes);
		free_kept(scene_static.base);
		free_kept(scene_static.lightmap);
		scene_static.positions = NULL;
		scene_static.attributes = NULL;
		scene_static.base = NULL;
		scene_static.lightmap = NULL;
		/* (and the levels again with the whole set) */
		scene_static.level_sent = FALSE;
		prism_scene_static_levels();
		break;
	default:
		break;
	}
}

void prism_scene_static_levels(
	void)
{
	long index;

	if (!scene_static.begun || !scene_static.level_count || !scene_static.levels || scene_static.phase <= PHASE_TRIANGLES)
	{
		return;
	}
	memset(scene_static.levels, 0, scene_static.level_words * sizeof(unsigned long));
	for (index = 0; index < scene_static.level_count; index++)
	{
		long surface = scene_static.level_surfaces[index];

		if (surface >= 0 && surface < scene_static.surfaces &&
			prism_world_geometry_surface_level(scene_static.level_lightmaps[index], scene_static.level_materials[index],
			scene_static.level_locals[index]) == 1)
		{
			scene_static.levels[surface >> 5] |= 1UL << (surface & 31);
		}
	}
	if (scene_static.level_sent &&
		!memcmp(scene_static.levels, scene_static.levels_sent, scene_static.level_words * sizeof(unsigned long)))
	{
		return;
	}
	memcpy(scene_static.levels_sent, scene_static.levels, scene_static.level_words * sizeof(unsigned long));
	scene_static.level_sent = TRUE;
	xgpu_backend_scene_static(PHASE_LEVELS, scene_static.generation, 0, (unsigned long)scene_static.level_words,
		(unsigned long)scene_static.level_words, scene_static.levels);
}

void prism_scene_static_map_unloaded(
	void)
{
	release();
	scene_static.played_bsp = NULL;
	scene_static.in_play = FALSE;
}

boolean prism_scene_static_in_play(
	void)
{
	return scene_static.begun && scene_static.in_play;
}

unsigned long prism_scene_static_generation(
	void)
{
	return scene_static.begun ? scene_static.generation : 0;
}

int prism_scene_static_ended(
	void)
{
	return scene_static.begun && scene_static.phase > PHASE_END && scene_static.total > 0;
}
