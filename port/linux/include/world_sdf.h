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

The field is sparse: a grid of cells, each either a brick of voxels (where
a surface is within range of it) or a single kind (far outside every
surface, or far inside solid), so that its memory follows the surfaces'
area and not the bounds' volume; and it covers the BSP where objects can
be (its placements' reach), not the sky's box or a sea floor out to the
horizon.

Workstream B owns this interface (port/linux/game/world_sdf.c, the bake's
pure arithmetic in port/linux/game/world_sdf_bake.c).
*/

#ifndef WORLD_SDF_H
#define WORLD_SDF_H

/* ---------- constants */

/* the cache's version: bumped whenever the bake or the format changes, so
that every field is baked again (2: sparse bricks, within the placements'
reach; 3: the static scenery in the field, and its occlusion of the level
in lightmap pages) */
#define WORLD_SDF_VERSION 3
/* the file's magic, "WSDF" */
#define WORLD_SDF_MAGIC 0x46445357UL

enum
{
	/* the finest voxel, in thousandths of a world unit (0.25) */
	WORLD_SDF_FINEST_VOXEL_MILLI = 250,
	/* a brick: a cell of WORLD_SDF_BRICK voxels' steps along each axis,
	its WORLD_SDF_BRICK_SPAN voxels a side at the steps' ends (its far
	faces' voxels are the next cell's near faces' too, so that a linear
	filter within one brick never reads another's) */
	WORLD_SDF_BRICK = 8,
	WORLD_SDF_BRICK_SPAN = WORLD_SDF_BRICK + 1,
	WORLD_SDF_BRICK_VOXELS = WORLD_SDF_BRICK_SPAN * WORLD_SDF_BRICK_SPAN * WORLD_SDF_BRICK_SPAN,
	/* the GPU's atlas of bricks: WORLD_SDF_ATLAS by WORLD_SDF_ATLAS bricks a
	layer, as many layers as the bricks fill */
	WORLD_SDF_ATLAS = 16,
	WORLD_SDF_ATLAS_LAYER = WORLD_SDF_ATLAS * WORLD_SDF_ATLAS,
	WORLD_SDF_ATLAS_LAYER_BYTES = WORLD_SDF_ATLAS_LAYER * WORLD_SDF_BRICK_VOXELS,
	/* a cell of the field's table: the atlas's brick x, y, z, and its kind
	(WORLD_SDF_CELL_*) */
	WORLD_SDF_CELL_BYTES = 4,
	/* the most GPU memory a field takes, its table and its atlas: the voxel
	grows past the finest (x1.125 a step) until it fits */
	WORLD_SDF_MAXIMUM_BYTES = 8 * 1024 * 1024,
	/* the most cells along an axis */
	WORLD_SDF_MAXIMUM_DIMENSION = 256,
	/* the distance the encoding holds each way, in voxels: past it, clamped */
	WORLD_SDF_RANGE_VOXELS = 12,
	/* the bounds padded each way, in voxels */
	WORLD_SDF_PADDING_VOXELS = 2,
	/* the field's bounds: the BSP's, cut to its placements' (the players',
	vehicles', units', items', devices' and the AI's) grown by this many
	world units each way: where objects can be */
	WORLD_SDF_PLACEMENTS_REACH = 32,
	/* the static scenery's occlusion of the level (struct
	world_sdf_scenery): the most bytes its pages take, the most and the
	least texels along a page's side, the most lightmap pages a BSP has,
	and the most placements baked */
	WORLD_SDF_SCENERY_MAXIMUM_BYTES = 4 * 1024 * 1024,
	WORLD_SDF_SCENERY_MOST_SIDE = 512,
	WORLD_SDF_SCENERY_LEAST_SIDE = 16,
	WORLD_SDF_SCENERY_MAXIMUM_PAGES = 1024,
	WORLD_SDF_SCENERY_MAXIMUM_PLACEMENTS = 4096,
	/* a page's layer where it has none: the lightmap pass walks the
	static scenery's shapes itself there (no page: a lightmap missing, a
	page whose texels two surfaces share) */
	WORLD_SDF_SCENERY_NO_LAYER = -1,
};

/* a cell's kind (struct world_sdf_volume's cells[i][3]): far outside every
surface (+range all through it), far inside solid (-range), or a brick of
the atlas (cells[i][0..2] its place there) */
enum
{
	WORLD_SDF_CELL_OUTSIDE = 0,
	WORLD_SDF_CELL_INSIDE = 1,
	WORLD_SDF_CELL_BRICK = 2,
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

/* a field: width x height x depth cells of WORLD_SDF_BRICK voxels' steps a
side. Its voxels' centres are at origin + voxel * (x y z), x from 0 to
WORLD_SDF_BRICK * width (and y, z so); each holds the distance at its
centre over range (127 = +range: outside; -127 = -range: inside solid).

The cache file's header (little-endian, 72 bytes), followed by the cells
(4 bytes each, x fastest, then y, then z), the bricks and the static
scenery's part (struct world_sdf_scenery, scenery_bytes of it), deflated
(zlib) as one stream; a CRC-32 of the header and the deflated bytes ends
the file. Named <data>/world_sdf/<map name>_<bsp index>.wsdf */
struct world_sdf_header
{
	unsigned long magic; /* WORLD_SDF_MAGIC */
	unsigned long version; /* WORLD_SDF_VERSION */
	unsigned long map_checksum; /* the cache file header's checksum */
	unsigned long bsp_index;
	/* the cells along x, y, z */
	unsigned long width, height, depth;
	/* the first voxel's centre (x0 y0 z0) and the voxel's size, world units */
	float origin[3];
	float voxel;
	/* the distance +-127 stands for, world units */
	float range;
	unsigned long deflated_size;
	/* milliseconds the bake took, and on how many threads (logged, measured) */
	unsigned long bake_milliseconds;
	unsigned long bake_threads;
	/* the bricks */
	unsigned long brick_count;
	/* the static scenery's part's bytes, and the milliseconds its pages
	took of the bake */
	unsigned long scenery_bytes;
	unsigned long scenery_milliseconds;
};

/* a static scenery placement baked (its shapes in the level's pages, its
collision geometry in the field): its definition (a tag index) and where it
stands, which the window finds its object by (world_sdf_scenery_baked) */
struct world_sdf_placement
{
	long definition;
	float position[3];
};

/* the static scenery's occlusion of the level, in the BSP's lightmaps'
space (docs/world-ao.md, "Static scenery"): for each of its lightmap pages,
the layer of an array of width x height pages of bytes (255 none: the
product of the static shapes' 1 - occlusion x fade at each texel's place
and normal) that the lightmap pass reads at the lightmap's own coordinates,
or WORLD_SDF_SCENERY_NO_LAYER; and the placements baked. Its part of the
file: page_count, layer_count, width, height, placement_count (4 bytes
each), each page's layer (2 bytes, signed), each placement (its definition,
its position: 4 bytes each), then the layers' texels */
struct world_sdf_scenery
{
	unsigned long page_count, layer_count, width, height, placement_count;
	short *layers;
	struct world_sdf_placement *placements;
	unsigned char *texels;
};

/* a field in memory: its cells (width * height * depth, x fastest: a
brick's place in the atlas and the kind, WORLD_SDF_CELL_*) and its bricks
(brick_count * WORLD_SDF_BRICK_VOXELS, each x fastest; brick n is at the
atlas's n % WORLD_SDF_ATLAS, n / WORLD_SDF_ATLAS % WORLD_SDF_ATLAS,
n / WORLD_SDF_ATLAS_LAYER) */
struct world_sdf_volume
{
	struct world_sdf_header header;
	unsigned char (*cells)[WORLD_SDF_CELL_BYTES];
	signed char *bricks;
	struct world_sdf_scenery scenery;
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
/* the frame drawn now prepares a map's pipelines (world_ao.c: its time is
the load's, not the frames' that HALO_WORLD_SDF_FRAMES measures) */
void world_sdf_frame_prepared(void);
/* a field handed to the backend whole, its table and its atlas, at once
(debug.world_ao_test's made-up one) */
void world_sdf_upload(struct world_sdf_volume const *volume);

/* the current BSP's field: its state, and how far it has faded in (0 to 1:
the occlusion's strength takes it, so that it never pops) */
int world_sdf_state(void);
float world_sdf_fade(void);
/* its header (NULL: none) */
struct world_sdf_header const *world_sdf_current(void);
/* whether an object of this definition standing here is a static scenery
placement that the drawn field and its pages hold (the window's proxies
put it apart: the lightmap pass reads its pages, the objects the field) */
int world_sdf_scenery_baked(long definition, float const position[3]);
/* the layer in the drawn field's pages of a page of the loaded BSP's
lightmaps (its bitmap data, as the lightmap pass binds it), or
WORLD_SDF_SCENERY_NO_LAYER (none drawn, not one of its pages, or none baked
for it) */
int world_sdf_scenery_layer(void const *lightmap_bitmap);

/* ---------- the bake's pure arithmetic (port/linux/game/world_sdf_bake.c;
tools/harness/tests/world_sdf.c) */

/* the field's cells and origin for these bounds (x0 y0 z0 x1 y1 z1), padded,
at this voxel (world units) */
void world_sdf_layout(float const bounds[6], float voxel, struct world_sdf_header *header);
/* the cells of a layout that may hold a voxel within range of a triangle
(a triangle within range and half the cell's diagonal of its middle):
marks[cell] 1, the others 0; their count */
long world_sdf_mark(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char *marks);
/* the same for slices z_first to z_end - 1 alone (marks of the others
untouched: threads each mark some), their count */
long world_sdf_mark_slices(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char *marks, long z_first, long z_end);
/* the triangles that may be within range of each marked cell (the same
test): indices into positions' triangles, cell by cell, those of cell c
from firsts[c] to firsts[c + 1] - 1 (both malloc'd, firsts width * height *
depth + 1 long); NULL if no memory */
long *world_sdf_bin(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char const *marks, long **firsts);
/* its parts, for threads each taking some slices: the cells' triangles of
slices z_first to z_end - 1 counted (indices NULL: firsts[cell + 1]++, firsts
zeroed before) or put (indices[firsts[cell]++]); between the two passes the
counts made places (world_sdf_bin_place: the indices malloc'd), after the
second the places put back (world_sdf_bin_placed) */
void world_sdf_bin_slices(struct world_sdf_header const *header, float const *positions, long triangle_count,
	unsigned char const *marks, long *firsts, long *indices, long z_first, long z_end);
long *world_sdf_bin_place(struct world_sdf_header const *header, long *firsts);
void world_sdf_bin_placed(struct world_sdf_header const *header, long *firsts);
/* the finest layout (from WORLD_SDF_FINEST_VOXEL_MILLI, the voxel x1.125
a step) of these triangles within bounds whose table and marked cells'
bricks fit WORLD_SDF_MAXIMUM_BYTES; its marks (malloc'd), NULL if no
memory. Each layout tried is marked by mark (NULL: world_sdf_mark; threads
may share it), which returns the count */
typedef long (*world_sdf_marker)(void *context, struct world_sdf_header const *header, float const *positions,
	long triangle_count, unsigned char *marks);
unsigned char *world_sdf_plan(float const bounds[6], float const *positions, long triangle_count,
	struct world_sdf_header *header, world_sdf_marker mark, void *context);
/* the GPU memory a layout's table and this many bricks take */
unsigned long world_sdf_bytes(struct world_sdf_header const *header, unsigned long bricks);
/* a distance (world units) encoded, and decoded */
signed char world_sdf_encode(float distance, float range);
float world_sdf_decode(signed char value, float range);
/* a cell's voxels baked into brick (WORLD_SDF_BRICK_VOXELS, x fastest):
each the least distance to these triangles (indices into positions'),
signed by inside(point), the collision BSP's point test (whether a point
is in solid); its kind: WORLD_SDF_CELL_BRICK if a voxel is within range,
else the side all of them are on */
int world_sdf_bake_cell(struct world_sdf_header const *header, unsigned long cell, float const *positions,
	long const *triangles, long triangle_count, int (*inside)(void *context, float const point[3]),
	void *context, signed char *brick);
/* the kind of a cell no surface is within range of: its middle's side */
int world_sdf_cell_kind(struct world_sdf_header const *header, unsigned long cell,
	int (*inside)(void *context, float const point[3]), void *context);
/* a field (header's cells, origin, voxel and range set) of a whole grid of
voxels, (WORLD_SDF_BRICK * width + 1) x ... x (WORLD_SDF_BRICK * depth + 1),
x fastest: its cells and bricks malloc'd; FALSE if no memory */
int world_sdf_pack(struct world_sdf_volume *volume, signed char const *voxels);
/* the field's distance at a point, trilinear, as the shaders read it */
float world_sdf_sample(struct world_sdf_volume const *volume, float const point[3]);
/* the GPU's atlas of the bricks: its layers, and a layer's voxels
(WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN square, WORLD_SDF_BRICK_SPAN deep,
x fastest: WORLD_SDF_ATLAS_LAYER_BYTES; the places past the last brick 0) */
unsigned long world_sdf_atlas_layers(struct world_sdf_header const *header);
void world_sdf_atlas_layer(struct world_sdf_volume const *volume, unsigned long layer, signed char *voxels);

#endif
