/*
WORLD_PROXIES.H

display.ambient_occlusion "world" (docs/world-ao.md, #578): the dynamic
things' occluders, a few analytic shapes for each object (spheres, capsules,
boxes), made once per object definition from its skeleton and collision
model (a biped's limbs from its nodes, a vehicle's body from its collision
geometry's pieces), placed each frame on the object's nodes as drawn
(render_interpolation.c) and handed to the renderer for a window. Their
occlusion is analytic: soft, exact, no noise, nothing on screen needed.

Workstream C owns this interface (port/linux/game/world_proxies.c); D's
shaders read the same shapes on the GPU (xgpu_stream.h), and
world_proxy_occlusion is their CPU reference.
*/

#ifndef WORLD_PROXIES_H
#define WORLD_PROXIES_H

/* ---------- constants */

enum
{
	/* the most shapes a definition has */
	WORLD_PROXY_MAXIMUM_PER_DEFINITION = 16,
	/* the most a window hands the renderer (XGPU_WORLD_AO_PROXIES_MAXIMUM) */
	WORLD_PROXY_MAXIMUM_PER_WINDOW = 64,
};

enum world_proxy_kind
{
	_world_proxy_none,
	_world_proxy_sphere,
	_world_proxy_capsule,
	_world_proxy_box,
	NUMBER_OF_WORLD_PROXY_KINDS
};

/* ---------- structures */

/* a shape in its node's space (a definition's) */
struct world_proxy_shape
{
	short kind; /* world_proxy_kind */
	short node_index; /* the node it rides on (-1: the object's root) */
	/* sphere: a (centre), radius; capsule: a, b (its segment's ends),
	radius; box: a (centre), axes[0..2] (its half extents along each, in
	the node's space), radius unused */
	float a[3], b[3];
	float radius;
	float axes[3][3];
};

/* a definition's shapes (made once, kept by definition: its tag index) */
struct world_proxy_set
{
	long definition;
	short count;
	struct world_proxy_shape shapes[WORLD_PROXY_MAXIMUM_PER_DEFINITION];
};

/* a shape in the world, as the renderer takes it (the GPU's layout:
xgpu_stream.h's three vec4s): [0] a.xyz, kind; [1] b.xyz (a box's x
half axis), owner (the window's object slot: an object's own shapes do not
shade it); [2] radius (a box's y half axis xyz and z half extent in w) */
struct world_proxy
{
	float v[3][4];
};

/* ---------- prototypes (port/linux/game/world_proxies.c) */

/* a definition's shapes (made the first time it is asked for: from the
model's nodes and the collision model's pieces, or a sphere of its bound
radius); NULL none (an object that occludes nothing: a projectile, a
light-weight effect object) */
struct world_proxy_set const *world_proxies_definition(long definition);
/* the map unloaded: every definition's shapes forgotten */
void world_proxies_map_unloaded(void);
/* a window's shapes: those of the frame's instances (world_scene.h) that
can shade what this camera sees (their spheres of influence in its
frustum, nearest first), at most maximum; returns how many. owners[i] is
the instance's id whose shape proxies[i] is */
long world_proxies_window(float const camera_position[3], struct world_proxy *proxies, long *owners,
	long maximum);

/* ---------- the arithmetic (shared with D's shaders: their reference;
tools/harness/tests/world_proxies.c) */

/* the share of a point's cosine-weighted hemisphere (normal) one shape
blocks, 0 to 1 */
float world_proxy_occlusion(struct world_proxy const *proxy, float const point[3], float const normal[3]);
/* how far from a shape its occlusion is worth computing (the shaders'
cull; past it, under 1/255), from a sphere's or box's centre (a) or a
capsule's midpoint ((a + b) / 2) */
float world_proxy_reach(struct world_proxy const *proxy);

#endif
