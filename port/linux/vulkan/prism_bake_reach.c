/*
PRISM_BAKE_REACH.C

The baker's reachable space and the surfaces' importance
(prism_bake_reach.h; PRISM v4's design, section 3.3).
*/

#include "prism_bake_reach.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the collision bsp */

#define LEAF_BIT ((int32_t)0x80000000)

static int bsp3d_child_is_solid(struct prism_bake_bsp3d const *bsp, int32_t child)
{
	return child == PRISM_BAKE_NONE || (child & 0x7FFFFFFF) >= bsp->leaf_count;
}

static float plane_distance(float const *plane, float const point[3])
{
	return plane[0] * point[0] + plane[1] * point[1] + plane[2] * point[2] - plane[3];
}

int32_t prism_bake_bsp3d_test_point(struct prism_bake_bsp3d const *bsp, float const point[3])
{
	int32_t node_index = 0;
	int depth = 0;

	do
	{
		int32_t const *node;
		int32_t plane;

		/* (bsp3d_test_point: a node that is no node, or too deep, is solid) */
		if (node_index < 0 || node_index >= bsp->node_count || depth++ >= PRISM_BAKE_BSP3D_MAXIMUM_DEPTH)
			return PRISM_BAKE_NONE;
		node = bsp->nodes + node_index * 3;
		plane = node[0];
		if (plane < 0 || plane >= bsp->plane_count)
			return PRISM_BAKE_NONE;
		node_index = node[1 + (plane_distance(bsp->planes + plane * 4, point) >= 0.0f)];
	}
	while (!(node_index & LEAF_BIT));

	if (bsp3d_child_is_solid(bsp, node_index))
		return PRISM_BAKE_NONE;
	return node_index & 0x7FFFFFFF;
}

struct cast_state
{
	struct prism_bake_bsp3d const *bsp;
	float a[3], delta[3];
	float fraction;
	float normal[3];
};

static void cast_point(struct cast_state const *state, float t, float point[3])
{
	point[0] = state->a[0] + state->delta[0] * t;
	point[1] = state->a[1] + state->delta[1] * t;
	point[2] = state->a[2] + state->delta[2] * t;
}

/* the part [t0, t1] of the segment in the subtree: 1 when it meets solid
(entered through entry, a plane facing the segment's start, or NULL at the
start) */
static int cast_node(struct cast_state *state, int32_t node_index, float t0, float t1, float const *entry, int depth)
{
	int32_t const *node;
	float const *plane;
	float p0[3], p1[3], d0, d1;
	int near_side;

	if (node_index & LEAF_BIT)
	{
		if (!bsp3d_child_is_solid(state->bsp, node_index))
			return 0;
		state->fraction = t0;
		if (entry)
		{
			state->normal[0] = entry[0];
			state->normal[1] = entry[1];
			state->normal[2] = entry[2];
		}
		else
		{
			state->normal[0] = state->normal[1] = state->normal[2] = 0.0f;
		}
		return 1;
	}
	if (node_index < 0 || node_index >= state->bsp->node_count || depth >= PRISM_BAKE_BSP3D_MAXIMUM_DEPTH ||
		state->bsp->nodes[node_index * 3] < 0 || state->bsp->nodes[node_index * 3] >= state->bsp->plane_count)
	{
		return cast_node(state, PRISM_BAKE_NONE, t0, t1, entry, depth);
	}
	node = state->bsp->nodes + node_index * 3;
	plane = state->bsp->planes + node[0] * 4;
	cast_point(state, t0, p0);
	cast_point(state, t1, p1);
	d0 = plane_distance(plane, p0);
	d1 = plane_distance(plane, p1);
	if (d0 >= 0.0f && d1 >= 0.0f)
		return cast_node(state, node[2], t0, t1, entry, depth + 1);
	if (d0 < 0.0f && d1 < 0.0f)
		return cast_node(state, node[1], t0, t1, entry, depth + 1);
	near_side = d0 >= 0.0f;
	{
		float tm = t0 + (t1 - t0) * (d0 / (d0 - d1));
		float facing[3];

		if (tm < t0)
			tm = t0;
		if (tm > t1)
			tm = t1;
		if (cast_node(state, node[1 + near_side], t0, tm, entry, depth + 1))
			return 1;
		/* (the far side is entered through the plane, facing the near side) */
		facing[0] = near_side ? plane[0] : -plane[0];
		facing[1] = near_side ? plane[1] : -plane[1];
		facing[2] = near_side ? plane[2] : -plane[2];
		return cast_node(state, node[2 - near_side], tm, t1, facing, depth + 1);
	}
}

int prism_bake_bsp3d_cast(struct prism_bake_bsp3d const *bsp, float const a[3], float const b[3],
	float *fraction, float normal[3])
{
	struct cast_state state;
	int hit;

	state.bsp = bsp;
	state.a[0] = a[0];
	state.a[1] = a[1];
	state.a[2] = a[2];
	state.delta[0] = b[0] - a[0];
	state.delta[1] = b[1] - a[1];
	state.delta[2] = b[2] - a[2];
	state.fraction = 1.0f;
	state.normal[0] = state.normal[1] = state.normal[2] = 0.0f;
	hit = bsp->node_count > 0 ? cast_node(&state, 0, 0.0f, 1.0f, NULL, 0) : cast_node(&state, PRISM_BAKE_NONE,
		0.0f, 1.0f, NULL, 0);
	if (fraction)
		*fraction = hit ? state.fraction : 1.0f;
	if (normal)
	{
		normal[0] = state.normal[0];
		normal[1] = state.normal[1];
		normal[2] = state.normal[2];
	}
	return hit;
}

static int segment_clear(struct prism_bake_bsp3d const *bsp, float ax, float ay, float az, float bx, float by,
	float bz)
{
	float a[3], b[3];

	a[0] = ax;
	a[1] = ay;
	a[2] = az;
	b[0] = bx;
	b[1] = by;
	b[2] = bz;
	return !prism_bake_bsp3d_cast(bsp, a, b, NULL, NULL);
}

static int point_air(struct prism_bake_bsp3d const *bsp, float x, float y, float z)
{
	float p[3];

	p[0] = x;
	p[1] = y;
	p[2] = z;
	return prism_bake_bsp3d_test_point(bsp, p) != PRISM_BAKE_NONE;
}

/* ---------- the voxel set */

#define VOXEL_BIAS 0x100000
#define VOXEL_MASK 0x1FFFFF

static uint64_t voxel_key(int32_t x, int32_t y, int32_t z)
{
	/* (+1: the key 0 is an empty slot) */
	return ((((uint64_t)((x + VOXEL_BIAS) & VOXEL_MASK)) << 42) |
		(((uint64_t)((y + VOXEL_BIAS) & VOXEL_MASK)) << 21) |
		((uint64_t)((z + VOXEL_BIAS) & VOXEL_MASK))) + 1;
}

static size_t voxel_hash(uint64_t key, size_t mask)
{
	key ^= key >> 33;
	key *= 0xFF51AFD7ED558CCDULL;
	key ^= key >> 33;
	key *= 0xC4CEB9FE1A85EC53ULL;
	key ^= key >> 33;
	return (size_t)key & mask;
}

void prism_bake_voxels_init(struct prism_bake_voxels *set, float cell)
{
	memset(set, 0, sizeof(*set));
	set->cell = cell;
}

void prism_bake_voxels_free(struct prism_bake_voxels *set)
{
	free(set->keys);
	free(set->flags);
	set->keys = NULL;
	set->flags = NULL;
	set->capacity = set->count = 0;
}

uint8_t prism_bake_voxels_get(struct prism_bake_voxels const *set, int32_t x, int32_t y, int32_t z)
{
	uint64_t key = voxel_key(x, y, z);
	size_t mask, slot;

	if (!set->capacity)
		return 0;
	mask = set->capacity - 1;
	for (slot = voxel_hash(key, mask); set->keys[slot]; slot = (slot + 1) & mask)
	{
		if (set->keys[slot] == key)
			return set->flags[slot];
	}
	return 0;
}

static int voxels_grow(struct prism_bake_voxels *set)
{
	size_t capacity = set->capacity ? set->capacity * 2 : 4096;
	uint64_t *keys = (uint64_t *)calloc(capacity, sizeof(uint64_t));
	uint8_t *flags = (uint8_t *)calloc(capacity, 1);
	size_t index;

	if (!keys || !flags)
	{
		free(keys);
		free(flags);
		return -1;
	}
	for (index = 0; index < set->capacity; index++)
	{
		if (set->keys[index])
		{
			size_t slot = voxel_hash(set->keys[index], capacity - 1);

			while (keys[slot])
				slot = (slot + 1) & (capacity - 1);
			keys[slot] = set->keys[index];
			flags[slot] = set->flags[index];
		}
	}
	free(set->keys);
	free(set->flags);
	set->keys = keys;
	set->flags = flags;
	set->capacity = capacity;
	return 0;
}

int prism_bake_voxels_add(struct prism_bake_voxels *set, int32_t x, int32_t y, int32_t z, uint8_t flags)
{
	uint64_t key = voxel_key(x, y, z);
	size_t mask, slot;
	uint8_t before;

	if ((set->count + 1) * 4 > set->capacity * 3 && voxels_grow(set))
		return -1;
	mask = set->capacity - 1;
	for (slot = voxel_hash(key, mask); set->keys[slot]; slot = (slot + 1) & mask)
	{
		if (set->keys[slot] == key)
		{
			before = set->flags[slot];
			set->flags[slot] = (uint8_t)(before | flags);
			return before;
		}
	}
	set->keys[slot] = key;
	set->flags[slot] = flags;
	set->count++;
	return 0;
}

uint8_t prism_bake_voxels_slot(struct prism_bake_voxels const *set, size_t slot, int32_t xyz[3])
{
	uint64_t key;

	if (slot >= set->capacity || !set->keys[slot])
		return 0;
	key = set->keys[slot] - 1;
	xyz[0] = (int32_t)((key >> 42) & VOXEL_MASK) - VOXEL_BIAS;
	xyz[1] = (int32_t)((key >> 21) & VOXEL_MASK) - VOXEL_BIAS;
	xyz[2] = (int32_t)(key & VOXEL_MASK) - VOXEL_BIAS;
	return set->flags[slot];
}

/* ---------- the walk */

void prism_bake_reach_defaults(struct prism_bake_reach_parameters *parameters)
{
	memset(parameters, 0, sizeof(*parameters));
	parameters->cell = 0.25f;
	/* (the cyborg crouched is about 0.4 wu tall, standing about 0.7, its
	eye about 0.62 above its feet; its jump clears about 0.4 wu: a little
	more is allowed, an extra floor costs texels, a missing one quality) */
	parameters->body_height = 0.4f;
	parameters->eye_height = 0.62f;
	parameters->radius = 0.12f;
	parameters->step_height = 0.1f;
	parameters->jump_height = 0.5f;
	parameters->jump_distance = 1.25f;
	parameters->walkable_normal_z = 0.64f; /* (about 50 degrees) */
	parameters->seed_lift = 0.5f;
	/* (the third-person and vehicle cameras sit up to about 1.5 wu from
	the body) */
	parameters->camera_distance = 1.5f;
	parameters->flight_cell = 2.0f;
	parameters->flight = 0;
	parameters->bounds[0] = parameters->bounds[2] = parameters->bounds[4] = -5000.0f;
	parameters->bounds[1] = parameters->bounds[3] = parameters->bounds[5] = 5000.0f;
	parameters->maximum_walk = 0;
	parameters->maximum_flight = 0;
}

struct floor_node
{
	float x, y, z;
};

struct walk_state
{
	struct prism_bake_bsp3d const *bsp;
	struct prism_bake_reach *reach;
	struct prism_bake_reach_parameters const *p;
	struct floor_node *queue;
	size_t queue_count, queue_capacity, queue_head;
	float step;
	int failed;
};

static int32_t cell_of(float value, float cell)
{
	return (int32_t)floorf(value / cell);
}

static int inside_bounds(struct prism_bake_reach_parameters const *p, float x, float y, float z)
{
	return x >= p->bounds[0] && x <= p->bounds[1] && y >= p->bounds[2] && y <= p->bounds[3] && z >= p->bounds[4] &&
		z <= p->bounds[5];
}

/* the floor below (x, y, top): 1 and its height and the surface's normal z */
static int find_floor(struct walk_state *state, float x, float y, float top, float *floor_z, float *normal_z)
{
	float a[3], b[3], fraction, normal[3];

	a[0] = b[0] = x;
	a[1] = b[1] = y;
	a[2] = top;
	b[2] = state->p->bounds[4] - 1.0f;
	if (b[2] >= top)
		return 0;
	if (!prism_bake_bsp3d_cast(state->bsp, a, b, &fraction, normal) || fraction <= 0.0f)
		return 0;
	*floor_z = top + (b[2] - top) * fraction;
	*normal_z = normal[2];
	return 1;
}

/* whether the eye over the floor at (x, y, z) is under open water */
static int drowned(struct walk_state *state, float x, float y, float z)
{
	struct prism_bake_reach_parameters const *p = state->p;
	float eye[3];
	int32_t k;

	eye[0] = x;
	eye[1] = y;
	eye[2] = z + p->eye_height;
	for (k = 0; k < p->water_plane_count; k++)
	{
		float const *plane = p->water_planes + k * 4;
		float depth = -plane_distance(plane, eye);

		/* (below the surface, and the water open up to it) */
		if (depth > 0.0f && segment_clear(state->bsp, eye[0], eye[1], eye[2], eye[0] + plane[0] * depth,
			eye[1] + plane[1] * depth, eye[2] + plane[2] * depth))
		{
			return 1;
		}
	}
	return 0;
}

/* whether a player fits on the floor at (x, y, z) */
static int standable(struct walk_state *state, float x, float y, float z)
{
	struct prism_bake_reach_parameters const *p = state->p;
	float mid = z + p->body_height * 0.5f;

	if (p->water_plane_count && drowned(state, x, y, z))
		return 0;

	if (!segment_clear(state->bsp, x, y, z + 0.02f, x, y, z + p->body_height))
		return 0;
	return segment_clear(state->bsp, x, y, mid, x + p->radius, y, mid) &&
		segment_clear(state->bsp, x, y, mid, x - p->radius, y, mid) &&
		segment_clear(state->bsp, x, y, mid, x, y + p->radius, mid) &&
		segment_clear(state->bsp, x, y, mid, x, y - p->radius, mid);
}

static void walk_push(struct walk_state *state, float x, float y, float z, uint8_t extra)
{
	struct prism_bake_reach *reach = state->reach;
	float cell = state->p->cell;
	int before;

	if (state->failed || !inside_bounds(state->p, x, y, z))
		return;
	if (state->p->maximum_walk && reach->walk_count >= state->p->maximum_walk)
	{
		reach->walk_truncated = 1;
		return;
	}
	before = prism_bake_voxels_add(&reach->voxels, cell_of(x, cell), cell_of(y, cell), cell_of(z + 0.01f, cell),
		(uint8_t)(PRISM_BAKE_REACH_WALK | extra));
	if (before < 0)
	{
		state->failed = 1;
		return;
	}
	if (before & PRISM_BAKE_REACH_WALK)
		return;
	reach->walk_count++;
	if (state->queue_count == state->queue_capacity)
	{
		size_t capacity = state->queue_capacity ? state->queue_capacity * 2 : 4096;
		struct floor_node *queue = (struct floor_node *)realloc(state->queue, capacity * sizeof(*queue));

		if (!queue)
		{
			state->failed = 1;
			return;
		}
		state->queue = queue;
		state->queue_capacity = capacity;
	}
	state->queue[state->queue_count].x = x;
	state->queue[state->queue_count].y = y;
	state->queue[state->queue_count].z = z;
	state->queue_count++;
}

/* from the floor at (x, y, z) into the column (nx, ny) by a climb of up to
climb: 1 and the floor reached, or 0 */
static int walk_move(struct walk_state *state, float x, float y, float z, float nx, float ny, float climb,
	float *reached)
{
	struct prism_bake_reach_parameters const *p = state->p;
	float h = z + climb + 0.02f, top = h + p->body_height - 0.04f, f, normal_z;

	/* (room above the start for the climb, then the way across at the
	climb's height and at the top of the body) */
	if (!segment_clear(state->bsp, x, y, z + 0.02f, x, y, top))
		return 0;
	if (!segment_clear(state->bsp, x, y, h, nx, ny, h) || !segment_clear(state->bsp, x, y, top, nx, ny, top))
		return 0;
	if (!find_floor(state, nx, ny, h, &f, &normal_z) || normal_z < p->walkable_normal_z)
		return 0;
	if (!standable(state, nx, ny, f))
		return 0;
	*reached = f;
	return 1;
}

static void walk_expand(struct walk_state *state, struct floor_node node)
{
	static int const directions[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 },
		{ -1, -1 } };
	struct prism_bake_reach_parameters const *p = state->p;
	float cell = p->cell;
	int32_t ix = cell_of(node.x, cell), iy = cell_of(node.y, cell);
	int jump_cells = (int)ceilf(p->jump_distance / cell), d;

	for (d = 0; d < 8; d++)
	{
		float nx = ((float)(ix + directions[d][0]) + 0.5f) * cell;
		float ny = ((float)(iy + directions[d][1]) + 0.5f) * cell;
		float f;
		int stepped = 0, k;

		if (walk_move(state, node.x, node.y, node.z, nx, ny, state->step, &f))
		{
			walk_push(state, nx, ny, f, 0);
			stepped = f >= node.z - p->jump_height;
		}
		if (walk_move(state, node.x, node.y, node.z, nx, ny, p->jump_height, &f))
		{
			walk_push(state, nx, ny, f, 0);
			stepped = stepped || f >= node.z - p->jump_height;
		}
		if (stepped)
			continue;
		/* (a gap: jumped across, landing no higher than the jump) */
		for (k = 2; k <= jump_cells; k++)
		{
			float jx = ((float)(ix + directions[d][0] * k) + 0.5f) * cell;
			float jy = ((float)(iy + directions[d][1] * k) + 0.5f) * cell;

			if (walk_move(state, node.x, node.y, node.z, jx, jy, p->jump_height, &f) && f >= node.z - p->jump_height)
			{
				walk_push(state, jx, jy, f, 0);
				break;
			}
		}
	}
}

/* a walk seed: lifted out of solid as the game lifts a point into the world
(scenario_ensure_point_within_world's 0.05 wu steps, for at most
seed_lift), dropped to its floor; 1 when it is in this bsp */
static int walk_seed(struct walk_state *state, float const position[3])
{
	struct prism_bake_reach_parameters const *p = state->p;
	float cell = p->cell, z = position[2], f, normal_z;
	int steps = (int)(p->seed_lift / 0.05f), k;
	int32_t ix, iy;

	for (k = 0; k <= steps; k++, z += 0.05f)
	{
		if (point_air(state->bsp, position[0], position[1], z))
			break;
	}
	if (k > steps)
		return 0;
	if (!find_floor(state, position[0], position[1], z, &f, &normal_z))
		return 0;
	/* (a point far above its floor is a flier's: a view, not a walk) */
	if (z - f > 2.0f)
	{
		prism_bake_voxels_add(&state->reach->voxels, cell_of(position[0], cell), cell_of(position[1], cell),
			cell_of(z, cell), PRISM_BAKE_REACH_VIEW);
		return 1;
	}
	/* (on the walk's lattice: the seed's column, else the first of its
	neighbours a player fits in; else the seed as it is) */
	ix = cell_of(position[0], cell);
	iy = cell_of(position[1], cell);
	for (k = 0; k < 9; k++)
	{
		int order = (k + 4) % 9; /* (the seed's own column first) */
		float nx = ((float)(ix + (order % 3) - 1) + 0.5f) * cell;
		float ny = ((float)(iy + (order / 3) - 1) + 0.5f) * cell;
		float g, gz;

		if (find_floor(state, nx, ny, f + p->step_height + 0.02f, &g, &gz) && gz >= p->walkable_normal_z &&
			standable(state, nx, ny, g))
		{
			walk_push(state, nx, ny, g, PRISM_BAKE_REACH_SEED);
			return 1;
		}
	}
	if (p->water_plane_count && drowned(state, position[0], position[1], f))
		return 1;
	walk_push(state, position[0], position[1], f, PRISM_BAKE_REACH_SEED);
	return 1;
}

/* ---------- the camera volume and flight */

struct cell_queue
{
	int32_t *cells; /* x, y, z, level */
	size_t count, capacity, head;
};

static int cell_queue_push(struct cell_queue *queue, int32_t x, int32_t y, int32_t z, int32_t level)
{
	if (queue->count == queue->capacity)
	{
		size_t capacity = queue->capacity ? queue->capacity * 2 : 4096;
		int32_t *cells = (int32_t *)realloc(queue->cells, capacity * 4 * sizeof(int32_t));

		if (!cells)
			return -1;
		queue->cells = cells;
		queue->capacity = capacity;
	}
	queue->cells[queue->count * 4] = x;
	queue->cells[queue->count * 4 + 1] = y;
	queue->cells[queue->count * 4 + 2] = z;
	queue->cells[queue->count * 4 + 3] = level;
	queue->count++;
	return 0;
}

/* floods air from the queue's cells through the set's neighbours (26, or
6), each joined by a clear segment between centres, up to levels steps
(0: no limit) or maximum cells (0: no limit): -1 out of memory, 1 when
stopped by maximum */
static int flood_air(struct prism_bake_bsp3d const *bsp, struct prism_bake_reach_parameters const *p,
	struct prism_bake_voxels *set, struct cell_queue *queue, uint8_t flag, int neighbours, int32_t levels,
	size_t maximum, size_t *count)
{
	float cell = set->cell;

	while (queue->head < queue->count)
	{
		int32_t *c = queue->cells + queue->head * 4;
		int32_t x = c[0], y = c[1], z = c[2], level = c[3];
		float cx = ((float)x + 0.5f) * cell, cy = ((float)y + 0.5f) * cell, cz = ((float)z + 0.5f) * cell;
		int dx, dy, dz;

		queue->head++;
		if (levels && level >= levels)
			continue;
		for (dz = -1; dz <= 1; dz++)
		{
			for (dy = -1; dy <= 1; dy++)
			{
				for (dx = -1; dx <= 1; dx++)
				{
					float nx = ((float)(x + dx) + 0.5f) * cell, ny = ((float)(y + dy) + 0.5f) * cell;
					float nz = ((float)(z + dz) + 0.5f) * cell;
					int before;

					if (!dx && !dy && !dz)
						continue;
					if (neighbours == 6 && (dx != 0) + (dy != 0) + (dz != 0) != 1)
						continue;
					if (prism_bake_voxels_get(set, x + dx, y + dy, z + dz) & flag)
						continue;
					if (!inside_bounds(p, nx, ny, nz) || !point_air(bsp, nx, ny, nz) ||
						!segment_clear(bsp, cx, cy, cz, nx, ny, nz))
					{
						continue;
					}
					if (maximum && *count >= maximum)
						return 1;
					before = prism_bake_voxels_add(set, x + dx, y + dy, z + dz, flag);
					if (before < 0)
						return -1;
					if (!(before & flag))
						(*count)++;
					if (cell_queue_push(queue, x + dx, y + dy, z + dz, level + 1))
						return -1;
				}
			}
		}
	}
	return 0;
}

/* ---------- the reachable space */

int prism_bake_reach_build(struct prism_bake_reach *reach, struct prism_bake_bsp3d const *bsp,
	struct prism_bake_seed const *seeds, size_t seed_count, struct prism_bake_reach_parameters const *parameters)
{
	struct walk_state state;
	struct cell_queue queue;
	float cell = parameters->cell, slope_step;
	size_t index, slot;
	int result = 0;

	memset(reach, 0, sizeof(*reach));
	reach->parameters = *parameters;
	prism_bake_voxels_init(&reach->voxels, cell);
	prism_bake_voxels_init(&reach->flight, parameters->flight_cell);
	memset(&state, 0, sizeof(state));
	memset(&queue, 0, sizeof(queue));
	state.bsp = bsp;
	state.reach = reach;
	state.p = &reach->parameters;
	/* (a step at least climbs the steepest floor across a cell) */
	slope_step = cell * sqrtf(1.0f - parameters->walkable_normal_z * parameters->walkable_normal_z) /
		(parameters->walkable_normal_z > 0.01f ? parameters->walkable_normal_z : 0.01f);
	state.step = parameters->step_height > slope_step ? parameters->step_height : slope_step + 0.02f;

	/* the seeds */
	for (index = 0; index < seed_count; index++)
	{
		struct prism_bake_seed const *seed = &seeds[index];

		if (seed->kind == PRISM_BAKE_SEED_VIEW)
		{
			if (point_air(bsp, seed->position[0], seed->position[1], seed->position[2]) &&
				inside_bounds(state.p, seed->position[0], seed->position[1], seed->position[2]))
			{
				if (prism_bake_voxels_add(&reach->voxels, cell_of(seed->position[0], cell),
					cell_of(seed->position[1], cell), cell_of(seed->position[2], cell), PRISM_BAKE_REACH_VIEW) < 0)
				{
					state.failed = 1;
				}
				reach->seeds_used++;
			}
			else
			{
				reach->seeds_outside++;
			}
			continue;
		}
		if (walk_seed(&state, seed->position))
			reach->seeds_used++;
		else
			reach->seeds_outside++;
	}

	/* the walk */
	while (!state.failed && state.queue_head < state.queue_count)
	{
		struct floor_node node = state.queue[state.queue_head++];

		walk_expand(&state, node);
	}
	if (state.failed)
	{
		result = -1;
		goto done;
	}

	/* the body over each floor */
	for (index = 0; index < state.queue_count; index++)
	{
		struct floor_node const *node = &state.queue[index];
		int32_t x = cell_of(node->x, cell), y = cell_of(node->y, cell);
		int32_t z0 = cell_of(node->z + 0.01f, cell), z1 = cell_of(node->z + parameters->eye_height, cell), z;

		for (z = z0; z <= z1; z++)
		{
			int before = prism_bake_voxels_add(&reach->voxels, x, y, z, PRISM_BAKE_REACH_BODY);

			if (before < 0)
			{
				result = -1;
				goto done;
			}
			if (!(before & PRISM_BAKE_REACH_BODY))
				reach->body_count++;
		}
	}

	/* the camera volume: air within camera_distance of the body (and of the
	views) */
	for (slot = 0; slot < reach->voxels.capacity; slot++)
	{
		int32_t xyz[3];
		uint8_t flags = prism_bake_voxels_slot(&reach->voxels, slot, xyz);

		if (flags & (PRISM_BAKE_REACH_BODY | PRISM_BAKE_REACH_VIEW))
		{
			if (cell_queue_push(&queue, xyz[0], xyz[1], xyz[2], 0))
			{
				result = -1;
				goto done;
			}
		}
	}
	{
		size_t before_count = 0;
		int flood;

		for (index = 0; index < queue.count; index++)
		{
			int32_t *c = queue.cells + index * 4;

			if (prism_bake_voxels_add(&reach->voxels, c[0], c[1], c[2], PRISM_BAKE_REACH_CAMERA) < 0)
			{
				result = -1;
				goto done;
			}
			before_count++;
		}
		reach->camera_count = before_count;
		flood = flood_air(bsp, state.p, &reach->voxels, &queue, PRISM_BAKE_REACH_CAMERA, 26,
			(int32_t)ceilf(parameters->camera_distance / cell), 0, &reach->camera_count);
		if (flood < 0)
		{
			result = -1;
			goto done;
		}
	}
	for (slot = 0; slot < reach->voxels.capacity; slot++)
	{
		int32_t xyz[3];

		if (prism_bake_voxels_slot(&reach->voxels, slot, xyz) & PRISM_BAKE_REACH_VIEW)
			reach->view_count++;
	}

	/* flight: the air joined to the reachable space, on its own grid */
	if (parameters->flight)
	{
		float fcell = parameters->flight_cell;
		int flood;

		queue.count = queue.head = 0;
		for (slot = 0; slot < reach->voxels.capacity; slot++)
		{
			int32_t xyz[3], f[3];
			uint8_t flags = prism_bake_voxels_slot(&reach->voxels, slot, xyz);
			float cx, cy, cz;
			int before;

			if (!(flags & (PRISM_BAKE_REACH_CAMERA | PRISM_BAKE_REACH_VIEW)))
				continue;
			cx = ((float)xyz[0] + 0.5f) * cell;
			cy = ((float)xyz[1] + 0.5f) * cell;
			cz = ((float)xyz[2] + 0.5f) * cell;
			f[0] = cell_of(cx, fcell);
			f[1] = cell_of(cy, fcell);
			f[2] = cell_of(cz, fcell);
			/* (a flight cell starts the flood where its centre is air) */
			if (!point_air(bsp, ((float)f[0] + 0.5f) * fcell, ((float)f[1] + 0.5f) * fcell,
				((float)f[2] + 0.5f) * fcell))
			{
				continue;
			}
			before = prism_bake_voxels_add(&reach->flight, f[0], f[1], f[2], PRISM_BAKE_REACH_FLIGHT);
			if (before < 0 || (!before && cell_queue_push(&queue, f[0], f[1], f[2], 0)))
			{
				result = -1;
				goto done;
			}
			if (!before)
				reach->flight_count++;
		}
		flood = flood_air(bsp, state.p, &reach->flight, &queue, PRISM_BAKE_REACH_FLIGHT, 6, 0,
			parameters->maximum_flight, &reach->flight_count);
		if (flood < 0)
		{
			result = -1;
			goto done;
		}
		reach->flight_truncated = flood == 1;
	}

done:
	free(state.queue);
	free(queue.cells);
	return result;
}

void prism_bake_reach_free(struct prism_bake_reach *reach)
{
	prism_bake_voxels_free(&reach->voxels);
	prism_bake_voxels_free(&reach->flight);
}

/* ---------- importance */

void prism_bake_importance_defaults(struct prism_bake_importance_parameters *parameters)
{
	parameters->near_distance = 8.0f;
	parameters->backfaces_unseen = 1;
}

static float dot3(float const *a, float const *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* the squared distance from p to the triangle abc (Ericson's closest point) */
static float triangle_distance_squared(float const *p, float const *a, float const *b, float const *c)
{
	float ab[3], ac[3], ap[3], bp[3], cp[3], q[3], d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denominator;
	int i;

	for (i = 0; i < 3; i++)
	{
		ab[i] = b[i] - a[i];
		ac[i] = c[i] - a[i];
		ap[i] = p[i] - a[i];
	}
	d1 = dot3(ab, ap);
	d2 = dot3(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
		return dot3(ap, ap);
	for (i = 0; i < 3; i++)
		bp[i] = p[i] - b[i];
	d3 = dot3(ab, bp);
	d4 = dot3(ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
		return dot3(bp, bp);
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
	{
		v = d1 / (d1 - d3);
		for (i = 0; i < 3; i++)
			q[i] = ap[i] - v * ab[i];
		return dot3(q, q);
	}
	for (i = 0; i < 3; i++)
		cp[i] = p[i] - c[i];
	d5 = dot3(ab, cp);
	d6 = dot3(ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
		return dot3(cp, cp);
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
	{
		w = d2 / (d2 - d6);
		for (i = 0; i < 3; i++)
			q[i] = ap[i] - w * ac[i];
		return dot3(q, q);
	}
	va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
	{
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		for (i = 0; i < 3; i++)
			q[i] = bp[i] - w * (c[i] - b[i]);
		return dot3(q, q);
	}
	denominator = 1.0f / (va + vb + vc);
	v = vb * denominator;
	w = vc * denominator;
	for (i = 0; i < 3; i++)
		q[i] = ap[i] - ab[i] * v - ac[i] * w;
	return dot3(q, q);
}

/* the eyes for the backface test: the cells of EYE_CELL wu that hold one,
each cell's box a bound of the eyes in it */
#define EYE_CELL 8.0f

/* whether some eye may be in front of the plane n.x = d */
static int eyes_in_front(struct prism_bake_voxels const *eyes, float const *n, float d)
{
	size_t slot;

	for (slot = 0; slot < eyes->capacity; slot++)
	{
		int32_t xyz[3];
		float best = 0.0f;
		int i;

		if (!prism_bake_voxels_slot(eyes, slot, xyz))
			continue;
		for (i = 0; i < 3; i++)
			best += n[i] * ((float)xyz[i] + (n[i] > 0.0f ? 1.0f : 0.0f)) * EYE_CELL;
		if (best > d)
			return 1;
	}
	return 0;
}

static void mark_cluster(struct prism_bake_bsp3d const *bsp, struct prism_bake_clusters const *clusters,
	uint8_t *reached, float x, float y, float z)
{
	float p[3];
	int32_t leaf;

	p[0] = x;
	p[1] = y;
	p[2] = z;
	leaf = prism_bake_bsp3d_test_point(bsp, p);
	if (leaf != PRISM_BAKE_NONE && leaf < clusters->leaf_count)
	{
		int16_t cluster = clusters->leaf_clusters[leaf];

		if (cluster >= 0 && cluster < clusters->cluster_count)
			reached[cluster] = 1;
	}
}

int prism_bake_importance(struct prism_bake_reach const *reach, struct prism_bake_bsp3d const *bsp,
	struct prism_bake_clusters const *clusters, float const *positions, int32_t const *surfaces,
	size_t triangle_count, struct prism_bake_importance_parameters const *parameters, uint8_t *tiers,
	struct prism_bake_importance_counts *counts)
{
	int32_t cluster_count = clusters->cluster_count, c, surface_count = 0;
	uint8_t *reached = NULL, *seen = NULL, *surface_seen = NULL, *surface_referenced = NULL;
	struct prism_bake_voxels near;
	struct prism_bake_voxels eyes;
	float cell = reach->parameters.cell, fcell = reach->parameters.flight_cell, near_cell = 1.0f;
	float reach_distance = parameters->near_distance + 0.8661f * near_cell;
	size_t slot, t;
	int result = -1;

	memset(counts, 0, sizeof(*counts));
	prism_bake_voxels_init(&near, near_cell);
	prism_bake_voxels_init(&eyes, EYE_CELL);
	for (t = 0; t < triangle_count; t++)
	{
		if (surfaces[t] + 1 > surface_count)
			surface_count = surfaces[t] + 1;
	}
	reached = (uint8_t *)calloc((size_t)(cluster_count > 0 ? cluster_count : 1) * 2, 1);
	seen = (uint8_t *)calloc((size_t)(cluster_count > 0 ? cluster_count : 1) * 2, 1);
	surface_seen = (uint8_t *)calloc((size_t)(surface_count > 0 ? surface_count : 1), 1);
	surface_referenced = (uint8_t *)calloc((size_t)(surface_count > 0 ? surface_count : 1), 1);
	if (!reached || !seen || !surface_seen || !surface_referenced)
		goto done;

	/* the clusters the reachable space is in (the walk's and cameras' in the
	first half, flight's in the second), the floors near which tier 0 is,
	and the eyes */
	for (slot = 0; slot < reach->voxels.capacity; slot++)
	{
		int32_t xyz[3];
		uint8_t flags = prism_bake_voxels_slot(&reach->voxels, slot, xyz);
		float x = ((float)xyz[0] + 0.5f) * cell, y = ((float)xyz[1] + 0.5f) * cell, z = ((float)xyz[2] + 0.5f) * cell;

		if (!flags)
			continue;
		if (flags & (PRISM_BAKE_REACH_WALK | PRISM_BAKE_REACH_CAMERA | PRISM_BAKE_REACH_VIEW))
			mark_cluster(bsp, clusters, reached, x, y, z);
		if ((flags & PRISM_BAKE_REACH_WALK) &&
			prism_bake_voxels_add(&near, cell_of(x, near_cell), cell_of(y, near_cell), cell_of(z, near_cell), 1) < 0)
		{
			goto done;
		}
		if ((flags & (PRISM_BAKE_REACH_CAMERA | PRISM_BAKE_REACH_VIEW | PRISM_BAKE_REACH_BODY)) &&
			prism_bake_voxels_add(&eyes, cell_of(x, EYE_CELL), cell_of(y, EYE_CELL), cell_of(z, EYE_CELL), 1) < 0)
		{
			goto done;
		}
	}
	for (slot = 0; slot < reach->flight.capacity; slot++)
	{
		int32_t xyz[3];
		float x, y, z;

		if (!prism_bake_voxels_slot(&reach->flight, slot, xyz))
			continue;
		x = ((float)xyz[0] + 0.5f) * fcell;
		y = ((float)xyz[1] + 0.5f) * fcell;
		z = ((float)xyz[2] + 0.5f) * fcell;
		mark_cluster(bsp, clusters, reached + cluster_count, x, y, z);
		if (prism_bake_voxels_add(&eyes, cell_of(x, EYE_CELL), cell_of(y, EYE_CELL), cell_of(z, EYE_CELL), 1) < 0)
			goto done;
	}

	/* what their PVS sees */
	for (c = 0; c < cluster_count; c++)
	{
		int half;

		for (half = 0; half < 2; half++)
		{
			uint32_t const *row;
			int32_t other;

			if (!reached[half * cluster_count + c])
				continue;
			seen[half * cluster_count + c] = 1;
			if (!clusters->pvs || clusters->pvs_words < (size_t)((cluster_count + 31) >> 5))
				continue;
			row = clusters->pvs + (size_t)c * clusters->pvs_words;
			for (other = 0; other < cluster_count; other++)
			{
				if (row[other >> 5] & (1u << (other & 31)))
					seen[half * cluster_count + other] = 1;
			}
		}
	}
	for (c = 0; c < cluster_count; c++)
	{
		counts->reachable_clusters += reached[c] || reached[cluster_count + c];
		counts->visible_clusters += seen[c] || seen[cluster_count + c];
	}

	/* each surface: seen near (1), seen by flight only (2), by the clusters
	of the leaves that reference it */
	{
		int32_t leaf;

		for (leaf = 0; leaf < clusters->leaf_count; leaf++)
		{
			int16_t cluster = clusters->leaf_clusters[leaf];
			int32_t first = clusters->leaf_first_reference[leaf], r;
			int16_t count = clusters->leaf_reference_count[leaf];

			if (count <= 0 || first < 0 || first > clusters->reference_count - count)
				continue;
			for (r = first; r < first + count; r++)
			{
				int32_t s = clusters->reference_surfaces[r];

				if (s < 0 || s >= surface_count)
					continue;
				surface_referenced[s] = 1;
				if (cluster < 0 || cluster >= cluster_count)
					continue;
				if (seen[cluster])
					surface_seen[s] |= 1;
				if (seen[cluster_count + cluster])
					surface_seen[s] |= 2;
			}
		}
	}

	/* each triangle */
	for (t = 0; t < triangle_count; t++)
	{
		float const *a = positions + t * 9, *b = a + 3, *cc = a + 6;
		float e1[3], e2[3], n[3], length, area, minimum[3], maximum[3];
		int32_t s = surfaces[t], i;
		int ground, flight, front = 1, is_near = 0;
		uint8_t tier;

		for (i = 0; i < 3; i++)
		{
			e1[i] = b[i] - a[i];
			e2[i] = cc[i] - a[i];
			minimum[i] = a[i] < b[i] ? (a[i] < cc[i] ? a[i] : cc[i]) : (b[i] < cc[i] ? b[i] : cc[i]);
			maximum[i] = a[i] > b[i] ? (a[i] > cc[i] ? a[i] : cc[i]) : (b[i] > cc[i] ? b[i] : cc[i]);
		}
		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		length = sqrtf(dot3(n, n));
		area = length * 0.5f;
		/* (a surface no leaf references is kept seen) */
		ground = s < 0 || s >= surface_count || !surface_referenced[s] || (surface_seen[s] & 1);
		flight = s >= 0 && s < surface_count && (surface_seen[s] & 2);
		if (parameters->backfaces_unseen && length > 0.0f)
		{
			n[0] /= length;
			n[1] /= length;
			n[2] /= length;
			front = eyes_in_front(&eyes, n, dot3(n, a));
		}
		if (ground && front)
		{
			/* (the floors' cells within reach of the triangle: the box's
			cells looked up, or every floor cell when there are fewer) */
			int32_t lo[3], hi[3], x, y, z;
			double volume = 1.0;

			for (i = 0; i < 3; i++)
			{
				lo[i] = cell_of(minimum[i] - reach_distance, near_cell);
				hi[i] = cell_of(maximum[i] + reach_distance, near_cell);
				volume *= (double)(hi[i] - lo[i] + 1);
			}
			if (volume <= (double)near.count)
			{
				for (z = lo[2]; z <= hi[2] && !is_near; z++)
				{
					for (y = lo[1]; y <= hi[1] && !is_near; y++)
					{
						for (x = lo[0]; x <= hi[0] && !is_near; x++)
						{
							float p[3];

							if (!prism_bake_voxels_get(&near, x, y, z))
								continue;
							p[0] = ((float)x + 0.5f) * near_cell;
							p[1] = ((float)y + 0.5f) * near_cell;
							p[2] = ((float)z + 0.5f) * near_cell;
							is_near = triangle_distance_squared(p, a, b, cc) <= reach_distance * reach_distance;
						}
					}
				}
			}
			else
			{
				for (slot = 0; slot < near.capacity && !is_near; slot++)
				{
					int32_t xyz[3];
					float p[3];

					if (!prism_bake_voxels_slot(&near, slot, xyz))
						continue;
					p[0] = ((float)xyz[0] + 0.5f) * near_cell;
					p[1] = ((float)xyz[1] + 0.5f) * near_cell;
					p[2] = ((float)xyz[2] + 0.5f) * near_cell;
					is_near = triangle_distance_squared(p, a, b, cc) <= reach_distance * reach_distance;
				}
			}
		}
		if (!front || (!ground && !flight))
			tier = PRISM_BAKE_TIER_UNSEEN;
		else if (ground && is_near)
			tier = PRISM_BAKE_TIER_NEAR;
		else
			tier = PRISM_BAKE_TIER_SEEN;
		tiers[t] = tier;
		counts->triangles[tier]++;
		counts->area[tier] += area;
	}
	result = 0;

done:
	free(reached);
	free(seen);
	free(surface_seen);
	free(surface_referenced);
	prism_bake_voxels_free(&near);
	prism_bake_voxels_free(&eyes);
	return result;
}
