/*
XGPU_SCENE_ENCODE.H

PRISM v4's scene records (xgpu_stream.h's SCENE_* and GBUFFER_DRAW;
docs/prism_scene_records.md) filled from typed values into a buffer the
caller gives (4-byte aligned), each returning the bytes written: the
record's size, its header's, or 0 where the buffer is too small or the
values are refused. Nothing calls them yet: no build sends these records
until display.lighting has its v4 modes (M1), and this file is not in any
build; tools/test_scene_records.py compiles and checks it.

What every encoder does with what it is given:
- a float that is not finite is written 0;
- a count past its capacity is cut to it (nodes, lights, planes, bitmaps,
  scalars, transforms, change colours), and the record's size follows the
  count sent;
- an id that must name something (a material's, an instance's) refuses the
  record when it is 0; an enum past its last value (a family, a phase, a
  blend) refuses it too;
- a light of an unknown type or class, or a fog plane with no normal, is
  left out, the others kept in order.
*/

#ifndef __HALO_XGPU_SCENE_ENCODE_H
#define __HALO_XGPU_SCENE_ENCODE_H

#include <stdint.h>

#include "xgpu_stream.h"

/* a shader tag's path's 64-bit FNV-1a hash (its bytes as they are), low
word first: SCENE_MATERIAL's path_hash */
void xgpu_scene_path_hash(char const *path, uint32_t hash[2]);

uint32_t xgpu_scene_encode_material(void *out, uint32_t room, uint32_t material, const uint32_t hash[2],
	uint32_t family, uint32_t material_type, uint32_t shader_flags, uint32_t detail_functions, uint32_t flags,
	const uint32_t *bitmaps, uint32_t bitmap_count, const float *scalars, uint32_t scalar_count);

/* a material's maps (PRISM v4 M4): tier XGPU_GBUFFER_TIER_PACK or _MOD,
or _AUTO (the maps forgotten; anything else refused), model 0 to 15 (past
it refused); maps: map_count of the texture store's ids by XGPU_SCENE_MAP_*
(the rest 0) */
uint32_t xgpu_scene_encode_material_maps(void *out, uint32_t room, uint32_t material, uint32_t tier,
	uint32_t model, uint32_t flags, const uint32_t *maps, uint32_t map_count, const float *scalars,
	uint32_t scalar_count);

/* transforms: NULL, or transform_count slots' two rows (the rest the
identity); change_colors: rgb each */
uint32_t xgpu_scene_encode_material_animate(void *out, uint32_t room, uint32_t material,
	const float self_illumination[3], const float (*transforms)[2][4], uint32_t transform_count,
	const float (*change_colors)[3], uint32_t change_color_count);

/* last: NULL for an instance first seen (its NO_HISTORY flag set, this
frame's matrices sent twice) */
uint32_t xgpu_scene_encode_instance(void *out, uint32_t room, uint32_t instance, uint32_t flags, uint32_t lod,
	uint32_t mesh, const float (*change_colors)[3], uint32_t change_color_count, const float (*now)[3][4],
	const float (*last)[3][4], uint32_t node_count);

uint32_t xgpu_scene_encode_gbuffer_draw(void *out, uint32_t room, uint32_t material, uint32_t instance,
	uint32_t surface, uint32_t vertex_program, uint32_t flags, uint32_t lod);

uint32_t xgpu_scene_encode_static(void *out, uint32_t room, uint32_t phase, uint32_t generation, uint32_t first,
	uint32_t count, uint32_t total, uint32_t data, uint32_t size);

/* a triangle's XGPU_SCENE_STATIC_ATTRIBUTE_WORDS words (kind 0 to 255, page
0 to 65535: past them refused, 0 returned; else 1) */
int xgpu_scene_static_attributes(uint32_t words[XGPU_SCENE_STATIC_ATTRIBUTE_WORDS], uint32_t material,
	uint32_t surface, uint32_t kind, uint32_t page, const float atlas[6], const float base[6]);

uint32_t xgpu_scene_encode_lights(void *out, uint32_t room, uint32_t first, uint32_t total,
	const struct xgpu_stream_scene_light *lights, uint32_t count);

uint32_t xgpu_scene_encode_decal(void *out, uint32_t room, uint32_t decal, uint32_t bitmap, uint32_t frame,
	uint32_t blend, uint32_t flags, const float centre[3], const float axes[3][3], const float color[4],
	const float rectangle[4]);

/* atmosphere_range: start and opaque distances */
uint32_t xgpu_scene_encode_fog(void *out, uint32_t room, uint32_t flags, const float atmosphere_color[4],
	const float atmosphere_range[2], const struct xgpu_stream_scene_fog_plane *planes, uint32_t plane_count);

#endif
