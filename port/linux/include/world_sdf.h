/*
WORLD_SDF.H

display.ambient_occlusion "world" (docs/world-ao.md, #578): a signed
distance field of each structure BSP's collision geometry, baked on the
player's own machine from the player's own map, once, and kept in the data
folder's world_sdf (as menu_pictures keeps its pictures: never shipped).
Baked off the critical path: in the start's "Preparing menu pictures" pass,
which loads every stock map anyway, or on a low-priority thread after a map
loads without one; never during a load, and a map with no field yet loads
exactly as fast as it would without the feature (the proxies alone shade
until the field is ready, then it fades in).

Workstream B owns this interface (port/linux/game/world_sdf.c, the bake's
pure arithmetic in port/linux/game/world_sdf_bake.c).
*/

#ifndef WORLD_SDF_H
#define WORLD_SDF_H

/* ---------- constants */

/* the cache's version: bumped whenever the bake or the format changes, so
that every field is baked again */
#define WORLD_SDF_VERSION 1
/* the file's magic, "WSDF" */
#define WORLD_SDF_MAGIC 0x46445357UL

enum
{
	/* the finest voxel, in thousandths of a world unit (0.25) */
	WORLD_SDF_FINEST_VOXEL_MILLI = 250,
	/* the most voxels a field has: the voxel grows past the finest on a
	large map to stay within it (8 MB as R8) */
	WORLD_SDF_MAXIMUM_VOXELS = 8 * 1024 * 1024,
	/* the most along an axis (the smallest 3D texture limit, 2048, is the
	backends'; this keeps the slices' uploads small) */
	WORLD_SDF_MAXIMUM_DIMENSION = 1024,
	/* the distance the encoding holds each way, in voxels: past it, clamped */
	WORLD_SDF_RANGE_VOXELS = 12,
	/* the bounds padded each way, in voxels */
	WORLD_SDF_PADDING_VOXELS = 2,
};

/* a field's state (world_sdf_state) */
enum world_sdf_state
{
	_world_sdf_none, /* no map, or the feature off */
	_world_sdf_baking, /* none cached: baking in the background */
	_world_sdf_ready, /* loaded, and uploaded (or uploading: world_sdf_fade) */
	_world_sdf_failed, /* the bake failed: the proxies alone */
};

/* ---------- structures */

/* the cache file's header (little-endian, 64 bytes), followed by the
voxels: width * height * depth signed bytes, x fastest, then y, then z,
each the distance at the voxel's centre over range (127 = +range: outside;
-127 = -range: inside solid), deflated (zlib) as one stream; a CRC-32 of
the header and the deflated bytes ends the file. Named
<data>/world_sdf/<map name>_<bsp index>.wsdf */
struct world_sdf_header
{
	unsigned long magic; /* WORLD_SDF_MAGIC */
	unsigned long version; /* WORLD_SDF_VERSION */
	unsigned long map_checksum; /* the cache file header's checksum */
	unsigned long bsp_index;
	unsigned long width, height, depth;
	/* the first voxel's centre's corner (x0 y0 z0) and the voxel's size, world units */
	float origin[3];
	float voxel;
	/* the distance +-127 stands for, world units */
	float range;
	unsigned long deflated_size;
	/* milliseconds the bake took, and on how many threads (logged, measured) */
	unsigned long bake_milliseconds;
	unsigned long bake_threads;
};

/* a field in memory */
struct world_sdf_volume
{
	struct world_sdf_header header;
	signed char *voxels;
};

/* ---------- prototypes (port/linux/game/world_sdf.c) */

/* the game's hooks: a map loaded (finds its fields in the cache; starts
the background bake of those missing, at low priority, from a copy of the
geometry: world_scene_static_triangles), unloaded (a bake still running is
abandoned, its work kept for the next load of that map where it can be),
a BSP switched to (its field uploaded); each frame (a finished bake's field
uploaded, a few slices a frame so that nothing hitches) */
void world_sdf_map_loaded(void);
void world_sdf_map_unloaded(void);
void world_sdf_bsp_switched(short bsp_index);
void world_sdf_frame(void);
/* the "Preparing menu pictures" pass has the map loaded (menu_pictures_render.c):
its fields baked now if they are missing, where no one is waiting to play */
void world_sdf_startup_map_loaded(void);
/* any stock map's field missing (the pass runs for them too) */
int world_sdf_startup_wanted(void);

/* the current BSP's field: its state, and how far it has faded in (0 to 1:
the occlusion's strength takes it, so that it never pops) */
int world_sdf_state(void);
float world_sdf_fade(void);
/* its header (NULL: none) */
struct world_sdf_header const *world_sdf_current(void);

/* ---------- the bake's pure arithmetic (port/linux/game/world_sdf_bake.c;
tools/harness/tests/world_sdf.c) */

/* the field's dimensions and origin for these bounds (x0 y0 z0 x1 y1 z1),
within WORLD_SDF_MAXIMUM_VOXELS */
void world_sdf_layout(float const bounds[6], struct world_sdf_header *header);
/* a distance (world units) encoded, and decoded */
signed char world_sdf_encode(float distance, float range);
float world_sdf_decode(signed char value, float range);
/* the field of these triangles baked into volume->voxels (allocated by
the caller: width * height * depth), the slices z_first to z_end - 1 alone,
so that threads each bake a slab; inside(point) says whether a point is
in solid (the collision BSP's test), for the sign */
void world_sdf_bake_slab(struct world_sdf_volume *volume, float const *positions, long triangle_count,
	int (*inside)(void *context, float const point[3]), void *context, long z_first, long z_end);
/* the field's distance at a point, trilinear, as the shaders read it */
float world_sdf_sample(struct world_sdf_volume const *volume, float const point[3]);

#endif
