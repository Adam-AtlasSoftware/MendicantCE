/*
WORLD_PROXIES.C

display.ambient_occlusion "world"'s object proxies (world_proxies.h,
docs/world-ao.md): each object definition's spheres, capsules and boxes,
placed on its nodes as drawn, and their analytic occlusion (the shaders'
reference). Workstream C.

The skeleton: no definition has shapes; a sphere's occlusion is the
reference the others follow (docs/world-ao.md, "Proxies' occlusion").
*/

#include "world_proxies.h"

#include <math.h>

struct world_proxy_set const *world_proxies_definition(long definition)
{
	(void)definition;
	return NULL;
}

void world_proxies_map_unloaded(void)
{
}

long world_proxies_window(float const camera_position[3], struct world_proxy *proxies, long *owners,
	long maximum)
{
	(void)camera_position;
	(void)proxies;
	(void)owners;
	(void)maximum;
	return 0;
}

float world_proxy_occlusion(struct world_proxy const *proxy, float const point[3], float const normal[3])
{
	switch ((int)proxy->v[0][3])
	{
	case _world_proxy_sphere:
	{
		/* a sphere's cosine-weighted share of the hemisphere, (n.d / l) (r / l)^2,
		the horizon's clipping left out (docs/world-ao.md) */
		float d[3], length_squared, length, cosine, radius = proxy->v[2][0];
		int axis;

		for (axis = 0; axis < 3; axis++)
			d[axis] = proxy->v[0][axis] - point[axis];
		length_squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (length_squared <= radius * radius)
			return 1.0f;
		length = sqrtf(length_squared);
		cosine = (normal[0] * d[0] + normal[1] * d[1] + normal[2] * d[2]) / length;
		return cosine > 0.0f ? cosine * radius * radius / length_squared : 0.0f;
	}
	default:
		/* C: capsules and boxes */
		return 0.0f;
	}
}

float world_proxy_reach(struct world_proxy const *proxy)
{
	/* (r / l)^2 < 1/255 past about 16 radii) */
	return 16.0f * proxy->v[2][0];
}
