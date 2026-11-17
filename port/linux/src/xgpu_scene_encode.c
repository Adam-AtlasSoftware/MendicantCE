/*
XGPU_SCENE_ENCODE.C

PRISM v4's scene records filled into a caller's buffer
(xgpu_scene_encode.h, which says what each does with what it is given;
xgpu_stream.h's layouts). Not in any build yet: tools/test_scene_records.py
compiles it and checks its bytes against the layouts.
*/

#include "xgpu_scene_encode.h"

#include <string.h>

/* (finite, else 0: a NaN's or an infinity's bits never reach the host;
compared by its exponent's bits, so that no maths library is needed) */
static float finite_or_zero(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return (bits & 0x7f800000u) == 0x7f800000u ? 0.0f : value;
}

static void copy_floats(float *to, const float *from, uint32_t count)
{
	uint32_t index;

	for (index = 0; index < count; index++)
		to[index] = finite_or_zero(from[index]);
}

static uint32_t least(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

void xgpu_scene_path_hash(char const *path, uint32_t hash[2])
{
	uint64_t value = 0xcbf29ce484222325ull;

	while (path && *path)
	{
		value ^= (uint8_t)*path++;
		value *= 0x100000001b3ull;
	}
	hash[0] = (uint32_t)value;
	hash[1] = (uint32_t)(value >> 32);
}

uint32_t xgpu_scene_encode_material(void *out, uint32_t room, uint32_t material, const uint32_t hash[2],
	uint32_t family, uint32_t material_type, uint32_t shader_flags, uint32_t detail_functions, uint32_t flags,
	const uint32_t *bitmaps, uint32_t bitmap_count, const float *scalars, uint32_t scalar_count)
{
	struct xgpu_stream_scene_material *record = out;
	uint32_t size = sizeof(*record);

	if (!out || room < size || !material || family >= XGPU_SCENE_FAMILIES)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_MATERIAL, size);
	record->material = material;
	if (hash)
	{
		record->path_hash[0] = hash[0];
		record->path_hash[1] = hash[1];
	}
	record->family = family;
	record->material_type = material_type;
	record->shader_flags = shader_flags;
	record->detail_functions = detail_functions;
	record->flags = flags;
	bitmap_count = bitmaps ? least(bitmap_count, XGPU_SCENE_MATERIAL_BITMAPS) : 0;
	if (bitmap_count)
		memcpy(record->bitmaps, bitmaps, bitmap_count * sizeof(uint32_t));
	record->bitmap_count = bitmap_count;
	if (scalars)
		copy_floats(record->scalars, scalars, least(scalar_count, XGPU_SCENE_MATERIAL_SCALARS));
	return size;
}

uint32_t xgpu_scene_encode_material_maps(void *out, uint32_t room, uint32_t material, uint32_t tier,
	uint32_t model, uint32_t flags, const uint32_t *maps, uint32_t map_count, const float *scalars,
	uint32_t scalar_count)
{
	struct xgpu_stream_scene_material_maps *record = out;
	uint32_t size = sizeof(*record);

	if (!out || room < size || !material || model > 15u ||
		(tier != XGPU_GBUFFER_TIER_AUTO && tier != XGPU_GBUFFER_TIER_PACK && tier != XGPU_GBUFFER_TIER_MOD))
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_MATERIAL_MAPS, size);
	record->material = material;
	record->tier = tier;
	record->model = model;
	record->flags = flags;
	map_count = maps ? least(map_count, XGPU_SCENE_MATERIAL_MAPS) : 0;
	if (map_count)
		memcpy(record->maps, maps, map_count * sizeof(uint32_t));
	if (scalars)
		copy_floats(record->scalars, scalars, least(scalar_count, XGPU_SCENE_MATERIAL_MAP_SCALARS));
	return size;
}

uint32_t xgpu_scene_encode_material_animate(void *out, uint32_t room, uint32_t material,
	const float self_illumination[3], const float (*transforms)[2][4], uint32_t transform_count,
	const float (*change_colors)[3], uint32_t change_color_count)
{
	struct xgpu_stream_scene_material_animate *record = out;
	uint32_t size = sizeof(*record), index;

	if (!out || room < size || !material)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_MATERIAL_ANIMATE, size);
	record->material = material;
	if (self_illumination)
		copy_floats(record->self_illumination, self_illumination, 3);
	transform_count = transforms ? least(transform_count, XGPU_SCENE_MATERIAL_BITMAPS) : 0;
	for (index = 0; index < XGPU_SCENE_MATERIAL_BITMAPS; index++)
	{
		if (index < transform_count)
		{
			copy_floats(record->transforms[index][0], transforms[index][0], 4);
			copy_floats(record->transforms[index][1], transforms[index][1], 4);
		}
		else
		{
			record->transforms[index][0][0] = 1.0f;
			record->transforms[index][1][1] = 1.0f;
		}
	}
	change_color_count = change_colors ? least(change_color_count, XGPU_SCENE_CHANGE_COLORS) : 0;
	for (index = 0; index < change_color_count; index++)
		copy_floats(record->change_colors[index], change_colors[index], 3);
	return size;
}

uint32_t xgpu_scene_encode_instance(void *out, uint32_t room, uint32_t instance, uint32_t flags, uint32_t lod,
	uint32_t mesh, const float (*change_colors)[3], uint32_t change_color_count, const float (*now)[3][4],
	const float (*last)[3][4], uint32_t node_count)
{
	struct xgpu_stream_scene_instance *record = out;
	uint32_t size, index;

	node_count = now ? least(node_count, XGPU_SCENE_INSTANCE_NODES) : 0;
	size = (uint32_t)(sizeof(*record) - sizeof(record->nodes) + node_count * sizeof(record->nodes[0]));
	if (!out || room < size || !instance)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_INSTANCE, size);
	record->instance = instance;
	record->flags = last ? flags & ~(uint32_t)XGPU_SCENE_INSTANCE_NO_HISTORY :
		flags | XGPU_SCENE_INSTANCE_NO_HISTORY;
	record->lod = lod;
	record->mesh = mesh;
	record->node_count = node_count;
	change_color_count = change_colors ? least(change_color_count, XGPU_SCENE_CHANGE_COLORS) : 0;
	for (index = 0; index < change_color_count; index++)
		copy_floats(record->change_colors[index], change_colors[index], 3);
	for (index = 0; index < node_count; index++)
	{
		copy_floats(&record->nodes[index].now[0][0], &now[index][0][0], 12);
		copy_floats(&record->nodes[index].last[0][0], last ? &last[index][0][0] : &now[index][0][0], 12);
	}
	return size;
}

uint32_t xgpu_scene_encode_gbuffer_draw(void *out, uint32_t room, uint32_t material, uint32_t instance,
	uint32_t surface, uint32_t vertex_program, uint32_t flags, uint32_t lod)
{
	struct xgpu_stream_gbuffer_draw *record = out;
	uint32_t size = sizeof(*record);

	if (!out || room < size || !material)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_GBUFFER_DRAW, size);
	record->material = material;
	record->instance = instance;
	record->surface = surface;
	record->vertex_program = vertex_program;
	record->flags = flags;
	record->lod = lod;
	return size;
}

uint32_t xgpu_scene_encode_static(void *out, uint32_t room, uint32_t phase, uint32_t generation, uint32_t first,
	uint32_t count, uint32_t total, uint32_t data, uint32_t size_)
{
	struct xgpu_stream_scene_static *record = out;
	uint32_t size = sizeof(*record);

	if (!out || room < size || phase >= XGPU_SCENE_STATIC_PHASES)
		return 0;
	/* (a slice: at most XGPU_SCENE_STATIC_SLICE triangles) */
	if ((phase == XGPU_SCENE_STATIC_TRIANGLES || phase == XGPU_SCENE_STATIC_ATTRIBUTES) &&
		count > XGPU_SCENE_STATIC_SLICE)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_STATIC, size);
	record->phase = phase;
	record->generation = generation;
	record->first = first;
	record->count = count;
	record->total = total;
	record->data = data;
	record->size = size_;
	return size;
}

int xgpu_scene_static_attributes(uint32_t words[XGPU_SCENE_STATIC_ATTRIBUTE_WORDS], uint32_t material,
	uint32_t surface, uint32_t kind, uint32_t page, const float atlas[6], const float base[6])
{
	float floats[12];
	uint32_t index;

	if (kind > 0xffu || page > 0xffffu)
		return 0;
	memset(words, 0, XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * sizeof(uint32_t));
	memset(floats, 0, sizeof(floats));
	words[XGPU_SCENE_ATTRIBUTE_MATERIAL] = material;
	words[XGPU_SCENE_ATTRIBUTE_SURFACE] = surface;
	words[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] = kind | (page << 16);
	if (atlas)
		copy_floats(floats, atlas, 6);
	if (base)
		copy_floats(floats + 6, base, 6);
	for (index = 0; index < 12; index++)
		memcpy(&words[XGPU_SCENE_ATTRIBUTE_ATLAS + index], &floats[index], sizeof(float));
	return 1;
}

uint32_t xgpu_scene_encode_lights(void *out, uint32_t room, uint32_t first, uint32_t total,
	const struct xgpu_stream_scene_light *lights, uint32_t count)
{
	struct xgpu_stream_scene_lights *record = out;
	uint32_t kept = 0, index, size;

	if (!out || room < sizeof(*record) - sizeof(record->lights))
		return 0;
	count = lights ? least(count, XGPU_SCENE_LIGHTS) : 0;
	for (index = 0; index < count; index++)
	{
		const struct xgpu_stream_scene_light *light = &lights[index];
		struct xgpu_stream_scene_light *to;

		if (light->type >= XGPU_SCENE_LIGHT_TYPES || light->light_class >= XGPU_SCENE_LIGHT_CLASSES)
			continue;
		if (room < sizeof(*record) - sizeof(record->lights) + (kept + 1) * sizeof(*to))
			return 0;
		to = &record->lights[kept++];
		to->id = light->id;
		to->type = light->type;
		to->light_class = light->light_class;
		to->flags = light->flags;
		copy_floats(to->position, light->position, 4);
		copy_floats(to->direction, light->direction, 4);
		copy_floats(to->radiance, light->radiance, 4);
		copy_floats(to->shape, light->shape, 4);
	}
	size = (uint32_t)(sizeof(*record) - sizeof(record->lights) + kept * sizeof(record->lights[0]));
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_LIGHTS, size);
	record->first = first;
	record->count = kept;
	record->total = total;
	return size;
}

uint32_t xgpu_scene_encode_decal(void *out, uint32_t room, uint32_t decal, uint32_t bitmap, uint32_t frame,
	uint32_t blend, uint32_t flags, const float centre[3], const float axes[3][3], const float color[4],
	const float rectangle[4])
{
	struct xgpu_stream_scene_decal *record = out;
	uint32_t size = sizeof(*record), index;

	if (!out || room < size || blend >= XGPU_SCENE_DECAL_BLENDS)
		return 0;
	memset(record, 0, size);
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_DECAL, size);
	record->decal = decal;
	record->bitmap = bitmap;
	record->frame = frame;
	record->blend = blend;
	record->flags = flags;
	if (centre)
		copy_floats(record->centre, centre, 3);
	for (index = 0; axes && index < 3; index++)
		copy_floats(record->axes[index], axes[index], 3);
	if (color)
		copy_floats(record->color, color, 4);
	if (rectangle)
		copy_floats(record->rectangle, rectangle, 4);
	return size;
}

uint32_t xgpu_scene_encode_fog(void *out, uint32_t room, uint32_t flags, const float atmosphere_color[4],
	const float atmosphere_range[2], const struct xgpu_stream_scene_fog_plane *planes, uint32_t plane_count)
{
	struct xgpu_stream_scene_fog *record = out;
	uint32_t kept = 0, index, size;

	if (!out || room < sizeof(*record) - sizeof(record->planes))
		return 0;
	memset(record, 0, sizeof(*record) - sizeof(record->planes));
	plane_count = planes ? least(plane_count, XGPU_SCENE_FOG_PLANES) : 0;
	for (index = 0; index < plane_count; index++)
	{
		const struct xgpu_stream_scene_fog_plane *plane = &planes[index];
		struct xgpu_stream_scene_fog_plane *to;
		float normal[3];

		copy_floats(normal, plane->plane, 3);
		if (normal[0] == 0.0f && normal[1] == 0.0f && normal[2] == 0.0f)
			continue;
		if (room < sizeof(*record) - sizeof(record->planes) + (kept + 1) * sizeof(*to))
			return 0;
		to = &record->planes[kept++];
		memset(to, 0, sizeof(*to));
		to->id = plane->id;
		to->flags = plane->flags;
		copy_floats(to->plane, plane->plane, 4);
		copy_floats(to->color, plane->color, 4);
		copy_floats(to->shape, plane->shape, 4);
	}
	size = (uint32_t)(sizeof(*record) - sizeof(record->planes) + kept * sizeof(record->planes[0]));
	record->header = XGPU_STREAM_HEADER(XGPU_STREAM_SCENE_FOG, size);
	record->flags = flags;
	record->plane_count = kept;
	if (atmosphere_color)
		copy_floats(record->atmosphere_color, atmosphere_color, 4);
	if (atmosphere_range)
		copy_floats(record->atmosphere_range, atmosphere_range, 2);
	return size;
}
