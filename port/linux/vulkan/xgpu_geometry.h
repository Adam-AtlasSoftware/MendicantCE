/*
XGPU_GEOMETRY.H

PRISM's resolved geometry of the drawn objects (display.prism_models): one
decision per object and frame of the shape every pass draws and traces, so
that the view, the shadow maps and the traced scene (display.prism_rt,
Rebaked's object sun, Path Traced) never disagree and never flip.

For each object (its datum and its mesh: a real hash map, aged, not a
direct-mapped table) the resolver keeps:
- its variant: stock until the store has its mesh, then the remodel for the
  rest of the map (the store keeps a mesh drawn once: a budget never sends
  it back to stock);
- its level of detail, decided once a frame by the first of its draws: the
  view's own size where the view comes first, else the size the view would
  have (its last size in the view times its distance then, over its
  distance now), with the hysteresis (display.prism_lod_hysteresis) against
  the level it had, however many frames ago. The shadows draw that level;
- its traced level: the drawn one, or a coarser level of the same remodel
  where the traced triangles' budget needs it, the farthest objects first,
  with a band so that the choice does not hunt (never stock, never left
  out). Levels past the ladder's drawable floor are traced only beyond the
  coarse reach.
Under the skinned vertices' budget (the objects skinned again each frame,
the rigs': a full skin ring drew them stock), the farthest of them are drawn
at a coarser level of their ladder, with the same band.
The ladder's drawable floor: a level of fewer than an eighth of the
triangles of the level before it is not drawn (a ladder that drops 16x in a
step, the old rocks' 1984 / 124 / 31, reads as the remodel flipping to CE's
shape); the ladder stops at the level before.

The consumers also note what each pass actually used (xgpu_geometry_note),
which the statistics and the change log (HALO_GEOMETRY_LOG: 1 a summary
each 600 frames, 2 every change too) count: switches, flips (a change undone
within XGPU_GEOMETRY_FLIP_FRAMES), and frames where the shadow or the traced
shape was not the view's. Pure C: tools/test_geometry_resolver.py runs it.
*/

#ifndef XGPU_GEOMETRY_H
#define XGPU_GEOMETRY_H

#include <stdint.h>

#define XGPU_GEOMETRY_LODS 8u
/* a change undone within this many frames is a flip */
#define XGPU_GEOMETRY_FLIP_FRAMES 90u
/* frames an object not drawn is remembered */
#define XGPU_GEOMETRY_AGE_FRAMES 1800u
/* the drawable floor: a level of fewer than 1/RATIO of the level before's
triangles is not drawn */
#define XGPU_GEOMETRY_LADDER_RATIO 8u

enum
{
	XGPU_GEOMETRY_STOCK = 0,
	XGPU_GEOMETRY_REMODEL = 1,
	/* a note only: the pass left the object out */
	XGPU_GEOMETRY_NONE = 2
};

/* the passes that note what they drew */
enum
{
	XGPU_GEOMETRY_VIEW = 0,
	XGPU_GEOMETRY_SHADOW = 1,
	XGPU_GEOMETRY_TRACED = 2,
	XGPU_GEOMETRY_CHANNELS = 3
};

/* a mesh's levels as the store has them: triangles, and the size in pixels
from which each is drawn (the last's 0) */
struct xgpu_geometry_ladder
{
	uint32_t lod_count;
	uint32_t triangles[XGPU_GEOMETRY_LODS];
	float cutoff[XGPU_GEOMETRY_LODS];
	/* each level's vertices, and whether the object is skinned again each
	frame (a rig's: a still object's skinning is kept) */
	uint32_t vertices[XGPU_GEOMETRY_LODS];
	int per_frame;
};

struct xgpu_geometry_settings
{
	/* display.prism_lod_hysteresis as a fraction (0 to 0.5) */
	float hysteresis;
	/* the remodels' traced triangles at most a frame (0: no budget) */
	uint32_t traced_budget;
	/* beyond it a level past the drawable floor may be traced (world units) */
	float coarse_reach;
	/* the vertices the objects skinned each frame may take a frame (the skin
	ring's share; 0 no budget): past it the farthest are drawn at a coarser
	level of their ladder, never stock */
	uint32_t skinned_budget;
};

struct xgpu_geometry_choice
{
	int variant;
	uint32_t lod, traced_lod;
	/* the ladder's last drawable level */
	uint32_t drawable_last;
};

struct xgpu_geometry_statistics
{
	uint64_t frames, decisions, objects;
	/* by channel: what the passes used, changed and undone */
	uint64_t switches[XGPU_GEOMETRY_CHANNELS], flips[XGPU_GEOMETRY_CHANNELS];
	/* stock to remodel (once a mesh is in), remodel to stock (never, with
	the resolver: the store keeps it) */
	uint64_t to_remodel, to_stock;
	/* object-frames whose shadow drew another level than the view, or
	whose traced shape was stock or left out under a drawn remodel, or
	another level than the view's within the coarse reach */
	uint64_t shadow_mismatch, traced_stock, traced_mismatch;
	/* traced levels made coarser for the budget (object-frames), frames
	still over it, levels the ladder's floor held back */
	uint64_t traced_degraded, over_budget, floor_held;
	/* drawn levels made coarser for the skinned vertices' budget
	(object-frames), the frames still over it, the vertices last frame */
	uint64_t drawn_degraded, skinned_over_budget, skinned_vertices_last;
	uint64_t traced_triangles_last;
	uint32_t entries;
};

typedef void (*xgpu_geometry_log_function)(void *context, const char *line);

struct xgpu_geometry;

struct xgpu_geometry *xgpu_geometry_create(void);
void xgpu_geometry_destroy(struct xgpu_geometry *geometry);
/* the log (level 0 none, 1 summaries, 2 every change) */
void xgpu_geometry_log(struct xgpu_geometry *geometry, int level, xgpu_geometry_log_function log, void *context);
void xgpu_geometry_settings_set(struct xgpu_geometry *geometry, const struct xgpu_geometry_settings *settings);
/* a new map (or a new set of meshes): every object forgotten */
void xgpu_geometry_map(struct xgpu_geometry *geometry);
/* a new frame: the eye (NULL unknown), the objects not drawn lately let go
of, the traced levels shared out within the budget */
void xgpu_geometry_frame(struct xgpu_geometry *geometry, const float eye[3]);
/* a draw of an object's mesh (object 0xffffffff: none, keyed by the mesh
alone), from the view or a shadow, at its size in pixels, its node 0 at
origin (NULL unknown); ladder NULL where the store does not have the mesh
in (drawn stock). The choice every pass of the frame draws */
void xgpu_geometry_resolve(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int shadow,
	const struct xgpu_geometry_ladder *ladder, float lod_pixels, const float origin[3],
	struct xgpu_geometry_choice *choice);
/* what a pass drew of an object (variant, level) */
void xgpu_geometry_note(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int channel, int variant,
	uint32_t lod);
/* the ladder's last drawable level */
uint32_t xgpu_geometry_drawable_last(const struct xgpu_geometry_ladder *ladder);
void xgpu_geometry_statistics(const struct xgpu_geometry *geometry, struct xgpu_geometry_statistics *statistics);
uint64_t xgpu_geometry_frame_number(const struct xgpu_geometry *geometry);

#endif
