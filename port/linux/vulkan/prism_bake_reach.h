/*
PRISM_BAKE_REACH.H

The baker's reachable space (PRISM v4's design, section 3.3): where in a
structure bsp the player can be, found from its collision bsp alone, and
how much each render surface matters to the bake because of it.

- Air and solid come from the collision bsp's bsp3d exactly as the game
  classifies a point (bsp3d.c's bsp3d_test_point: a leaf is air, NONE is
  solid), and a segment's first solid from the same tree, so nothing is
  voxelised from triangles.
- From the scenario's seeds (player starts, netgame flags and equipment,
  placed vehicles, bipeds, weapons and equipment, the AI's starting
  locations and firing positions) a walk is flooded over floors: a cell
  of the grid (0.25 wu) is a floor where the cast down meets a surface
  within the player's maximum slope and the player's body fits above it;
  from a floor the next cells are reached by a step or a jump, and drops
  of any height are allowed (the cast down simply finds the floor).
- A floor under deep water is not walked: CE kills a player whose head
  is under water, so a floor whose eye is below a water plane (a fog
  plane with a material) with open water up to the surface is no floor.
- The walk is dilated through air by the camera volume (the first- and
  third-person cameras, the vehicles' cameras), and, when the scenario
  has a flying vehicle, every air cell joined to it is reachable by
  flight (on a coarser grid, at a lower priority).
- Each render triangle then gets a tier: 0 within 8 wu of a floor the walk
  reached and in a cluster the reachable clusters' PVS sees, 1 seen from
  reachable space further away, 2 never seen (outside every reachable
  PVS, or facing away from every reachable eye).

Host code with no engine dependency (the bsp is passed as arrays as the
map has them), written as C89 so the guest could compile it too. The
offline checks (tools/prism_bake, tools/test_prism_bake.py) run it on the
player's own maps.
*/

#ifndef PRISM_BAKE_REACH_H
#define PRISM_BAKE_REACH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- the collision bsp */

/* a bsp3d as the map holds it: nodes of (plane, back child, front child),
a child with its top bit set a leaf (its low bits the leaf index) or NONE
(-1, solid); planes of (i, j, k, d), a point in front where
i x + j y + k z - d >= 0 */
struct prism_bake_bsp3d
{
	int32_t const *nodes;
	int32_t node_count;
	float const *planes;
	int32_t plane_count;
	/* the leaves the bsp has: a leaf index at or past it is outside the
	world, as scenario_location_from_point has it */
	int32_t leaf_count;
};

enum
{
	PRISM_BAKE_NONE = -1,
	/* bsp3d.h's MAXIMUM_BSP3D_TRAVERSAL_DEPTH */
	PRISM_BAKE_BSP3D_MAXIMUM_DEPTH = 128
};

/* the leaf a point is in, or PRISM_BAKE_NONE where it is solid */
int32_t prism_bake_bsp3d_test_point(struct prism_bake_bsp3d const *bsp, float const point[3]);

/* the first solid along the segment from a to b: 1 and its fraction along
the segment and the normal of the plane entered (facing a), or 0 where the
whole segment is air; a in solid is a hit at 0 with a zero normal */
int prism_bake_bsp3d_cast(struct prism_bake_bsp3d const *bsp, float const a[3], float const b[3],
	float *fraction, float normal[3]);

/* ---------- a sparse set of voxels */

/* voxels at integer coordinates (each within +-2^20) with a byte of flags */
struct prism_bake_voxels
{
	uint64_t *keys;
	uint8_t *flags;
	size_t capacity, count;
	float cell;
};

void prism_bake_voxels_init(struct prism_bake_voxels *set, float cell);
void prism_bake_voxels_free(struct prism_bake_voxels *set);
/* the flags of (x, y, z) or 0 */
uint8_t prism_bake_voxels_get(struct prism_bake_voxels const *set, int32_t x, int32_t y, int32_t z);
/* ors flags into (x, y, z): its flags before (0 if it was new), or -1 when
out of memory */
int prism_bake_voxels_add(struct prism_bake_voxels *set, int32_t x, int32_t y, int32_t z, uint8_t flags);
/* the coordinates and flags of the slot index (0 if the slot is empty) */
uint8_t prism_bake_voxels_slot(struct prism_bake_voxels const *set, size_t slot, int32_t xyz[3]);

/* ---------- the reachable space */

enum
{
	PRISM_BAKE_REACH_SEED = 1 << 0, /* a seed's floor */
	PRISM_BAKE_REACH_WALK = 1 << 1, /* a floor the walk reached (the voxel just above it) */
	PRISM_BAKE_REACH_BODY = 1 << 2, /* air a standing player fills */
	PRISM_BAKE_REACH_CAMERA = 1 << 3, /* air a camera reaches */
	PRISM_BAKE_REACH_VIEW = 1 << 4, /* a cutscene camera point */
	PRISM_BAKE_REACH_FLIGHT = 1 << 5 /* (the flight set's own flag) */
};

enum prism_bake_seed_kind
{
	PRISM_BAKE_SEED_WALK = 0, /* a point the player (or what he fights) stands at */
	PRISM_BAKE_SEED_VIEW /* a camera point: reachable air, never walked from */
};

struct prism_bake_seed
{
	float position[3];
	int32_t kind; /* prism_bake_seed_kind */
};

struct prism_bake_reach_parameters
{
	float cell; /* the walk's grid (0.25 wu) */
	float body_height; /* the air a player needs above a floor (crouched) */
	float eye_height; /* the standing eye above the floor */
	float radius; /* the body's radius, tested at mid-height */
	float step_height; /* climbed without a jump */
	float jump_height; /* climbed with one */
	float jump_distance; /* a gap a jump clears */
	float walkable_normal_z; /* cos(the maximum slope) */
	float seed_lift; /* how far a seed in solid is lifted to find air (the game's 0.05 steps) */
	float camera_distance; /* the camera volume's reach from the body */
	float flight_cell; /* the flight set's grid */
	int flight; /* flood the flight set */
	float bounds[6]; /* x0 x1 y0 y1 z0 z1: nothing outside is reachable */
	/* the water's surfaces (the bsp's fog planes with a material: i j k d,
	the water below, behind the plane): a floor whose eye is under one,
	with open water up to it, drowns the player and is not walked */
	float const *water_planes;
	int32_t water_plane_count;
	size_t maximum_walk; /* floors before the walk stops (0: no limit) */
	size_t maximum_flight; /* flight cells before the flight stops (0: no limit) */
	/* (non-zero while the walk is still wanted: another thread setting it
	stops the build within milliseconds, prism_bake_reach_build returning
	PRISM_BAKE_REACH_CANCELLED; NULL never stopped) */
	int const *cancel;
};

struct prism_bake_reach
{
	struct prism_bake_voxels voxels; /* at parameters.cell */
	struct prism_bake_voxels flight; /* at parameters.flight_cell */
	size_t walk_count, body_count, camera_count, view_count, flight_count;
	size_t seeds_used, seeds_outside;
	int walk_truncated, flight_truncated;
	struct prism_bake_reach_parameters parameters;
};

/* the defaults: the cyborg's measures, generous where a guess is needed */
void prism_bake_reach_defaults(struct prism_bake_reach_parameters *parameters);

/* prism_bake_reach_build's return when parameters.cancel was set */
#define PRISM_BAKE_REACH_CANCELLED (-2)

/* floods the reachable space from the seeds: 0, -1 out of memory, or
PRISM_BAKE_REACH_CANCELLED (what was walked is kept, to be freed) */
int prism_bake_reach_build(struct prism_bake_reach *reach, struct prism_bake_bsp3d const *bsp,
	struct prism_bake_seed const *seeds, size_t seed_count, struct prism_bake_reach_parameters const *parameters);
void prism_bake_reach_free(struct prism_bake_reach *reach);

/* ---------- each surface's importance */

enum
{
	PRISM_BAKE_TIER_NEAR = 0,
	PRISM_BAKE_TIER_SEEN = 1,
	PRISM_BAKE_TIER_UNSEEN = 2
};

/* the clusters (structure_bsp's leaves, surface references and PVS) */
struct prism_bake_clusters
{
	int16_t const *leaf_clusters; /* each leaf's cluster */
	int32_t leaf_count;
	/* each leaf's surface references: the leaf's first and count, and the
	references' surface indices */
	int32_t const *leaf_first_reference;
	int16_t const *leaf_reference_count;
	int32_t const *reference_surfaces;
	int32_t reference_count;
	/* the PVS: a bit vector of pvs_words (at least ceil(cluster_count / 32))
	words per cluster, cluster_count of them, or NULL (every cluster sees
	only itself) */
	uint32_t const *pvs;
	size_t pvs_words;
	int32_t cluster_count;
};

struct prism_bake_importance_parameters
{
	float near_distance; /* tier 0's reach from a floor (8 wu) */
	int backfaces_unseen; /* a triangle no reachable eye is in front of is tier 2 */
};

struct prism_bake_importance_counts
{
	size_t triangles[3];
	double area[3];
	int32_t reachable_clusters, visible_clusters;
};

void prism_bake_importance_defaults(struct prism_bake_importance_parameters *parameters);

/* the tier of each triangle (positions: 9 floats a triangle; surfaces: each
triangle's render surface, its index in the clusters' references): 0, or
-1 out of memory */
int prism_bake_importance(struct prism_bake_reach const *reach, struct prism_bake_bsp3d const *bsp,
	struct prism_bake_clusters const *clusters, float const *positions, int32_t const *surfaces,
	size_t triangle_count, struct prism_bake_importance_parameters const *parameters, uint8_t *tiers,
	struct prism_bake_importance_counts *counts);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_REACH_H */
