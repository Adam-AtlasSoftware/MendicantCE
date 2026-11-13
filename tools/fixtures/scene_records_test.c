/* tools/test_scene_records.py's driver: PRISM v4's scene records
(xgpu_stream.h) and their encoders (port/linux/src/xgpu_scene_encode.c).
"layout" prints every record's fields' offsets and sizes (compared between
ILP32 and LP64); "encode" prints the bytes of the encoders' records for set
values (compared with the layouts packed in Python); "constants" prints the
stream's version and the record types. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "xgpu_scene_encode.h"

#define FIELD(type, field) printf("%s.%s %u %u\n", #type, #field, (unsigned)offsetof(struct type, field), \
	(unsigned)sizeof(((struct type *)0)->field))
#define SIZE(type) printf("%s %u\n", #type, (unsigned)sizeof(struct type))

static void layout(void)
{
	SIZE(xgpu_stream_scene_material);
	FIELD(xgpu_stream_scene_material, header);
	FIELD(xgpu_stream_scene_material, material);
	FIELD(xgpu_stream_scene_material, path_hash);
	FIELD(xgpu_stream_scene_material, family);
	FIELD(xgpu_stream_scene_material, material_type);
	FIELD(xgpu_stream_scene_material, shader_flags);
	FIELD(xgpu_stream_scene_material, detail_functions);
	FIELD(xgpu_stream_scene_material, flags);
	FIELD(xgpu_stream_scene_material, bitmap_count);
	FIELD(xgpu_stream_scene_material, bitmaps);
	FIELD(xgpu_stream_scene_material, scalars);
	SIZE(xgpu_stream_scene_material_animate);
	FIELD(xgpu_stream_scene_material_animate, header);
	FIELD(xgpu_stream_scene_material_animate, material);
	FIELD(xgpu_stream_scene_material_animate, self_illumination);
	FIELD(xgpu_stream_scene_material_animate, transforms);
	FIELD(xgpu_stream_scene_material_animate, change_colors);
	SIZE(xgpu_stream_scene_node);
	FIELD(xgpu_stream_scene_node, now);
	FIELD(xgpu_stream_scene_node, last);
	SIZE(xgpu_stream_scene_instance);
	FIELD(xgpu_stream_scene_instance, header);
	FIELD(xgpu_stream_scene_instance, instance);
	FIELD(xgpu_stream_scene_instance, flags);
	FIELD(xgpu_stream_scene_instance, lod);
	FIELD(xgpu_stream_scene_instance, mesh);
	FIELD(xgpu_stream_scene_instance, node_count);
	FIELD(xgpu_stream_scene_instance, change_colors);
	FIELD(xgpu_stream_scene_instance, nodes);
	SIZE(xgpu_stream_gbuffer_draw);
	FIELD(xgpu_stream_gbuffer_draw, header);
	FIELD(xgpu_stream_gbuffer_draw, material);
	FIELD(xgpu_stream_gbuffer_draw, instance);
	FIELD(xgpu_stream_gbuffer_draw, surface);
	FIELD(xgpu_stream_gbuffer_draw, vertex_program);
	FIELD(xgpu_stream_gbuffer_draw, flags);
	FIELD(xgpu_stream_gbuffer_draw, lod);
	FIELD(xgpu_stream_gbuffer_draw, pad);
	SIZE(xgpu_stream_scene_static);
	FIELD(xgpu_stream_scene_static, header);
	FIELD(xgpu_stream_scene_static, phase);
	FIELD(xgpu_stream_scene_static, generation);
	FIELD(xgpu_stream_scene_static, first);
	FIELD(xgpu_stream_scene_static, count);
	FIELD(xgpu_stream_scene_static, total);
	FIELD(xgpu_stream_scene_static, data);
	FIELD(xgpu_stream_scene_static, size);
	SIZE(xgpu_stream_scene_light);
	FIELD(xgpu_stream_scene_light, id);
	FIELD(xgpu_stream_scene_light, type);
	FIELD(xgpu_stream_scene_light, light_class);
	FIELD(xgpu_stream_scene_light, flags);
	FIELD(xgpu_stream_scene_light, position);
	FIELD(xgpu_stream_scene_light, direction);
	FIELD(xgpu_stream_scene_light, radiance);
	FIELD(xgpu_stream_scene_light, shape);
	SIZE(xgpu_stream_scene_lights);
	FIELD(xgpu_stream_scene_lights, header);
	FIELD(xgpu_stream_scene_lights, first);
	FIELD(xgpu_stream_scene_lights, count);
	FIELD(xgpu_stream_scene_lights, total);
	FIELD(xgpu_stream_scene_lights, lights);
	SIZE(xgpu_stream_scene_decal);
	FIELD(xgpu_stream_scene_decal, header);
	FIELD(xgpu_stream_scene_decal, decal);
	FIELD(xgpu_stream_scene_decal, bitmap);
	FIELD(xgpu_stream_scene_decal, frame);
	FIELD(xgpu_stream_scene_decal, blend);
	FIELD(xgpu_stream_scene_decal, flags);
	FIELD(xgpu_stream_scene_decal, centre);
	FIELD(xgpu_stream_scene_decal, axes);
	FIELD(xgpu_stream_scene_decal, color);
	FIELD(xgpu_stream_scene_decal, rectangle);
	SIZE(xgpu_stream_scene_fog_plane);
	FIELD(xgpu_stream_scene_fog_plane, id);
	FIELD(xgpu_stream_scene_fog_plane, flags);
	FIELD(xgpu_stream_scene_fog_plane, pad);
	FIELD(xgpu_stream_scene_fog_plane, plane);
	FIELD(xgpu_stream_scene_fog_plane, color);
	FIELD(xgpu_stream_scene_fog_plane, shape);
	SIZE(xgpu_stream_scene_fog);
	FIELD(xgpu_stream_scene_fog, header);
	FIELD(xgpu_stream_scene_fog, flags);
	FIELD(xgpu_stream_scene_fog, plane_count);
	FIELD(xgpu_stream_scene_fog, pad);
	FIELD(xgpu_stream_scene_fog, atmosphere_color);
	FIELD(xgpu_stream_scene_fog, atmosphere_range);
	FIELD(xgpu_stream_scene_fog, planes);
}

/* (the bytes, 4-byte aligned) */
static uint32_t buffer[(sizeof(struct xgpu_stream_scene_lights) + 64) / 4];

static void show(const char *name, uint32_t size)
{
	const unsigned char *bytes = (const unsigned char *)buffer;
	uint32_t index;

	printf("%s %u ", name, (unsigned)size);
	for (index = 0; index < size; index++)
		printf("%02x", bytes[index]);
	printf("\n");
}

static void encode(void)
{
	static const float inf = 1.0f / 0.0f;
	float nan = 0.0f / 0.0f;
	uint32_t hash[2], bitmaps[10], index, size;
	float scalars[40], si[3] = { 0.25f, 0.5f, 1.0f }, colors[5][3], transforms[9][2][4];
	float now[70][3][4], last[70][3][4];
	struct xgpu_stream_scene_light lights[300];
	struct xgpu_stream_scene_fog_plane planes[10];
	float centre[3] = { 1.0f, 2.0f, 3.0f }, axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 0.5f } };
	float color[4] = { 1.0f, 0.5f, 0.25f, 0.75f }, rectangle[4] = { 0.0f, 0.0f, 0.5f, 0.5f };
	float fog_color[4] = { 0.1f, 0.2f, 0.3f, 0.9f }, fog_range[2] = { 10.0f, 200.0f };
	float atlas[6] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f }, base[6] = { 1, 2, 3, 4, 5, 6 };
	uint32_t words[XGPU_SCENE_STATIC_ATTRIBUTE_WORDS];

	xgpu_scene_path_hash("levels\\b30\\shaders\\sand", hash);
	printf("hash %08x%08x\n", hash[1], hash[0]);
	xgpu_scene_path_hash("", hash);
	printf("hash_empty %08x%08x\n", hash[1], hash[0]);
	for (index = 0; index < 10; index++)
		bitmaps[index] = 0xe0000000u + index;
	for (index = 0; index < 40; index++)
		scalars[index] = (float)index * 0.5f;
	scalars[1] = nan;
	scalars[2] = inf;
	xgpu_scene_path_hash("levels\\b30\\shaders\\sand", hash);
	show("material", xgpu_scene_encode_material(buffer, sizeof(buffer), 7, hash, XGPU_SCENE_FAMILY_SENV, 12, 0x5,
		0x010200, XGPU_SCENE_MATERIAL_FLAG_ALPHA_TESTED, bitmaps, 10, scalars, 40));
	show("material_none", xgpu_scene_encode_material(buffer, sizeof(buffer), 0, hash, 0, 0, 0, 0, 0, NULL, 0,
		NULL, 0));
	show("material_family", xgpu_scene_encode_material(buffer, sizeof(buffer), 7, hash, XGPU_SCENE_FAMILIES, 0, 0,
		0, 0, NULL, 0, NULL, 0));
	show("material_room", xgpu_scene_encode_material(buffer, sizeof(struct xgpu_stream_scene_material) - 4, 7, hash,
		0, 0, 0, 0, 0, NULL, 0, NULL, 0));
	for (index = 0; index < 5; index++)
	{
		colors[index][0] = (float)index;
		colors[index][1] = 0.5f;
		colors[index][2] = 0.25f;
	}
	colors[1][1] = nan;
	memset(transforms, 0, sizeof(transforms));
	for (index = 0; index < 9; index++)
	{
		transforms[index][0][0] = 2.0f;
		transforms[index][0][3] = (float)index;
		transforms[index][1][1] = 3.0f;
	}
	show("animate", xgpu_scene_encode_material_animate(buffer, sizeof(buffer), 7, si, transforms, 2, colors, 5));
	show("animate_identity", xgpu_scene_encode_material_animate(buffer, sizeof(buffer), 9, NULL, NULL, 0, NULL, 0));
	for (index = 0; index < 70; index++)
	{
		uint32_t row, column;

		for (row = 0; row < 3; row++)
			for (column = 0; column < 4; column++)
			{
				now[index][row][column] = (float)(index * 100 + row * 10 + column);
				last[index][row][column] = -(float)(index * 100 + row * 10 + column);
			}
	}
	now[1][2][3] = nan;
	show("instance", xgpu_scene_encode_instance(buffer, sizeof(buffer), 0x80010002u, XGPU_SCENE_INSTANCE_IN_TLAS |
		XGPU_SCENE_INSTANCE_NO_HISTORY, 3, 11, colors, 2, now, last, 3));
	show("instance_first", xgpu_scene_encode_instance(buffer, sizeof(buffer), 5, 0, 4, 0, NULL, 0, now, NULL, 2));
	size = xgpu_scene_encode_instance(buffer, sizeof(buffer), 5, 0, 4, 0, NULL, 0, now, last, 70);
	printf("instance_many %u %u\n", (unsigned)size, (unsigned)((struct xgpu_stream_scene_instance *)buffer)->node_count);
	show("instance_level", xgpu_scene_encode_instance(buffer, sizeof(buffer), 0, 0, 0, 0, NULL, 0, now, last, 1));
	show("gbuffer", xgpu_scene_encode_gbuffer_draw(buffer, sizeof(buffer), 7, 0, (2u << 16) | 3u, 41,
		XGPU_SCENE_DRAW_REMODEL, 4));
	show("gbuffer_none", xgpu_scene_encode_gbuffer_draw(buffer, sizeof(buffer), 0, 0, 0, 0, 0, 0));
	show("static", xgpu_scene_encode_static(buffer, sizeof(buffer), XGPU_SCENE_STATIC_TRIANGLES, 3, 65536, 1000,
		200000, 0x12345670u, 36000));
	show("static_phase", xgpu_scene_encode_static(buffer, sizeof(buffer), XGPU_SCENE_STATIC_PHASES, 0, 0, 0, 0, 0,
		0));
	show("static_slice", xgpu_scene_encode_static(buffer, sizeof(buffer), XGPU_SCENE_STATIC_TRIANGLES, 0, 0,
		XGPU_SCENE_STATIC_SLICE + 1, 0, 0, 0));
	atlas[3] = nan;
	printf("attributes %d", xgpu_scene_static_attributes(words, 7, 99, XGPU_RT_TRIANGLE_ALPHA_TESTED, 12, atlas,
		base));
	for (index = 0; index < XGPU_SCENE_STATIC_ATTRIBUTE_WORDS; index++)
		printf(" %08x", words[index]);
	printf("\n");
	printf("attributes_page %d\n", xgpu_scene_static_attributes(words, 7, 99, 0, 0x10000, atlas, base));
	memset(lights, 0, sizeof(lights));
	for (index = 0; index < 300; index++)
	{
		lights[index].id = 1000 + index;
		lights[index].type = index % 5;
		lights[index].light_class = index % 4;
		lights[index].flags = XGPU_SCENE_LIGHT_FLAG_ON;
		lights[index].position[0] = (float)index;
		lights[index].position[3] = 2.0f;
		lights[index].direction[3] = 2.0f;
		lights[index].radiance[0] = 1.0f;
		lights[index].radiance[3] = 3.0f;
		lights[index].shape[0] = 0.01f;
	}
	lights[1].type = XGPU_SCENE_LIGHT_TYPES;
	lights[2].light_class = XGPU_SCENE_LIGHT_CLASSES;
	lights[3].radiance[1] = inf;
	show("lights", xgpu_scene_encode_lights(buffer, sizeof(buffer), 0, 5, lights, 5));
	size = xgpu_scene_encode_lights(buffer, sizeof(buffer), 256, 600, lights, 300);
	printf("lights_many %u %u\n", (unsigned)size, (unsigned)((struct xgpu_stream_scene_lights *)buffer)->count);
	show("lights_none", xgpu_scene_encode_lights(buffer, sizeof(buffer), 0, 0, NULL, 0));
	axes[2][2] = nan;
	show("decal", xgpu_scene_encode_decal(buffer, sizeof(buffer), 77, 0xe0001234u, 2,
		XGPU_SCENE_DECAL_BLEND_DOUBLE_MULTIPLY, XGPU_SCENE_DECAL_FLAG_NORMAL, centre, axes, color, rectangle));
	show("decal_blend", xgpu_scene_encode_decal(buffer, sizeof(buffer), 77, 0, 0, XGPU_SCENE_DECAL_BLENDS, 0, NULL,
		NULL, NULL, NULL));
	memset(planes, 0, sizeof(planes));
	for (index = 0; index < 10; index++)
	{
		planes[index].id = index;
		planes[index].flags = index & 1 ? XGPU_SCENE_FOG_FLAG_INSIDE : 0;
		planes[index].pad[0] = 0xdeadbeefu;
		planes[index].plane[2] = 1.0f;
		planes[index].plane[3] = (float)index;
		planes[index].color[0] = 0.5f;
		planes[index].color[3] = 0.8f;
		planes[index].shape[0] = 30.0f;
		planes[index].shape[1] = 4.0f;
	}
	planes[1].plane[2] = 0.0f;
	show("fog", xgpu_scene_encode_fog(buffer, sizeof(buffer), XGPU_SCENE_FOG_FLAG_ATMOSPHERE, fog_color, fog_range,
		planes, 3));
	size = xgpu_scene_encode_fog(buffer, sizeof(buffer), 0, NULL, NULL, planes, 10);
	printf("fog_many %u %u\n", (unsigned)size, (unsigned)((struct xgpu_stream_scene_fog *)buffer)->plane_count);
}

static void constants(void)
{
	printf("version %u\n", (unsigned)XGPU_STREAM_VERSION);
	printf("record_types %u\n", (unsigned)XGPU_STREAM_RECORD_TYPES);
	printf("rt_frame %u\n", (unsigned)XGPU_STREAM_RT_FRAME);
	printf("scene_material %u\n", (unsigned)XGPU_STREAM_SCENE_MATERIAL);
	printf("scene_material_animate %u\n", (unsigned)XGPU_STREAM_SCENE_MATERIAL_ANIMATE);
	printf("scene_instance %u\n", (unsigned)XGPU_STREAM_SCENE_INSTANCE);
	printf("gbuffer_draw %u\n", (unsigned)XGPU_STREAM_GBUFFER_DRAW);
	printf("scene_static %u\n", (unsigned)XGPU_STREAM_SCENE_STATIC);
	printf("scene_lights %u\n", (unsigned)XGPU_STREAM_SCENE_LIGHTS);
	printf("scene_decal %u\n", (unsigned)XGPU_STREAM_SCENE_DECAL);
	printf("scene_fog %u\n", (unsigned)XGPU_STREAM_SCENE_FOG);
	printf("scene_record_types %u\n", (unsigned)XGPU_STREAM_SCENE_RECORD_TYPES);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "layout") == 0)
		layout();
	else if (argc == 2 && strcmp(argv[1], "encode") == 0)
		encode();
	else if (argc == 2 && strcmp(argv[1], "constants") == 0)
		constants();
	else
		return 2;
	return 0;
}
