/*
WORLD_AO_SHADING.C

display.ambient_occlusion "world"'s shading on the CPU (world_ao.h, "the
arithmetic"; docs/world-ao.md, "Shading"): the occlusion a pixel's ambient
light is multiplied by, as the shaders find it (port/linux/src/xgpu.h's
XGPU_AMBIENT_OCCLUSION_WORLD; the emitters' world_ao_*, operation for
operation), and an object's reference, which world_ao.c hands the shaders
with its draws. Pure: the field's sample is world_sdf_sample's
(world_sdf_bake.c), a proxy's occlusion and reach world_proxies.c's.
Tested against both emitters' shaders (tools/test_world_ao_light.py).
Workstream D.
*/

#include "world_ao.h"
#include "world_proxies.h"
#include "world_sdf.h"

#include <math.h>

static float saturate(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float mix(float a, float b, float t)
{
	return a + (b - a) * t;
}

float world_ao_field_occlusion(struct world_sdf_volume const *volume, float const point[3], float const normal[3])
{
	float first, occlusion = 0.0f;
	int tap, axis;

	if (!volume)
		return 1.0f;
	first = volume->header.voxel > WORLD_AO_STEP ? volume->header.voxel : WORLD_AO_STEP;
	for (tap = 0; tap < WORLD_AO_TAPS; tap++)
	{
		float h = first * (float)(1 << tap), at[3];

		for (axis = 0; axis < 3; axis++)
			at[axis] = point[axis] + normal[axis] * h;
		occlusion += 1.0f / (float)(2 << tap) * saturate((h - world_sdf_sample(volume, at)) / h);
	}
	return 1.0f - saturate(WORLD_AO_K * occlusion);
}

float world_ao_reference(struct world_sdf_volume const *volume, float const lighting_point[3])
{
	static const float up[3] = { 0.0f, 0.0f, 1.0f };
	float above[3];

	if (!volume)
		return 1.0f;
	above[0] = lighting_point[0];
	above[1] = lighting_point[1];
	above[2] = lighting_point[2] + (volume->header.voxel > WORLD_AO_STEP ? volume->header.voxel : WORLD_AO_STEP);
	return world_ao_field_occlusion(volume, above, up);
}

float world_ao_proxies_occlusion(struct world_proxy const *proxies, long count, float const point[3],
	float const normal[3], float owner)
{
	float product = 1.0f;
	long index;

	for (index = 0; index < count; index++)
	{
		struct world_proxy const *proxy = &proxies[index];
		float centre[3], offset[3], reach, away, fade;
		int axis;

		if (proxy->v[1][3] == owner)
			continue;
		/* (from its centre: a capsule's segment's middle, a sphere's or a
		box's a) */
		for (axis = 0; axis < 3; axis++)
		{
			centre[axis] = (int)proxy->v[0][3] == _world_proxy_capsule ?
				0.5f * (proxy->v[0][axis] + proxy->v[1][axis]) : proxy->v[0][axis];
			offset[axis] = point[axis] - centre[axis];
		}
		reach = world_proxy_reach(proxy);
		away = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
		if (away >= reach)
			continue;
		fade = saturate((reach - away) / (reach * WORLD_AO_REACH_FADE));
		product *= 1.0f - world_proxy_occlusion(proxy, point, normal) * fade;
	}
	return product;
}

float world_ao_occlusion(float const parameters[2][4], struct world_sdf_volume const *volume,
	struct world_proxy const *proxies, long count, float const point[3], float const normal[3], float owner,
	float reference, int lightmap)
{
	float proxied = world_ao_proxies_occlusion(proxies, count, point, normal, owner), field;

	if (lightmap)
		return mix(1.0f, proxied > WORLD_AO_FLOOR ? proxied : WORLD_AO_FLOOR, parameters[0][1]);
	field = world_ao_field_occlusion(volume, point, normal);
	field = mix(field, saturate(field / (reference > WORLD_AO_REFERENCE_LEAST ? reference : WORLD_AO_REFERENCE_LEAST)),
		parameters[1][0]);
	field = mix(1.0f, field, parameters[0][2]) * proxied;
	return mix(1.0f, field > WORLD_AO_FLOOR ? field : WORLD_AO_FLOOR, parameters[0][0]);
}
