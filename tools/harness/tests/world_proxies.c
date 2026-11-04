/* display.ambient_occlusion "world"'s object proxies: their analytic
occlusion, the shaders' reference (port/linux/game/world_proxies.c,
test_world_proxies.py). Workstream C grows the capsule's and the box's
cases here, each against a numerical integral of the hemisphere. */

#include "harness.h"

#include <math.h>

#include "under_test.inc"

static struct world_proxy sphere(float x, float y, float z, float radius)
{
	struct world_proxy proxy;

	memset(&proxy, 0, sizeof(proxy));
	proxy.v[0][0] = proxy.v[1][0] = x;
	proxy.v[0][1] = proxy.v[1][1] = y;
	proxy.v[0][2] = proxy.v[1][2] = z;
	proxy.v[0][3] = (float)_world_proxy_sphere;
	proxy.v[2][0] = radius;
	return proxy;
}

static void sphere_case(void)
{
	float const origin[3] = { 0.0f, 0.0f, 0.0f };
	float const up[3] = { 0.0f, 0.0f, 1.0f };
	float const down[3] = { 0.0f, 0.0f, -1.0f };
	struct world_proxy above = sphere(0.0f, 0.0f, 2.0f, 1.0f);
	struct world_proxy far = sphere(0.0f, 0.0f, 20.0f, 1.0f);
	float near_occlusion = world_proxy_occlusion(&above, origin, up);

	/* straight above at twice its radius: (r / l)^2 = 1/4 */
	CHECK(fabsf(near_occlusion - 0.25f) < 1.0e-5f, "%g", near_occlusion);
	/* behind the surface: none */
	CHECK(world_proxy_occlusion(&above, origin, down) == 0.0f, "behind");
	/* farther: less, and none past its reach */
	CHECK(world_proxy_occlusion(&far, origin, up) < near_occlusion, "farther");
	CHECK(world_proxy_reach(&far) < 20.0f || world_proxy_occlusion(&far, origin, up) < 1.0f / 255.0f, "reach");
}

static void none_case(void)
{
	float const origin[3] = { 0.0f, 0.0f, 0.0f };
	float const up[3] = { 0.0f, 0.0f, 1.0f };
	struct world_proxy proxy;

	memset(&proxy, 0, sizeof(proxy));
	CHECK(world_proxy_occlusion(&proxy, origin, up) == 0.0f, "none");
	CHECK(world_proxies_window(origin, NULL, NULL, 0) == 0, "an empty window");
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "sphere"))
		sphere_case();
	else if (!strcmp(name, "none"))
		none_case();
	else
		return 2;
	return 0;
}
