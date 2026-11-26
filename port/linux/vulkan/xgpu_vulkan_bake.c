/*
XGPU_VULKAN_BAKE.C

PRISM v4's in-game baker and PRISM Raster's light (overhaul_research/
prism_v4_renderer/design.md, sections 3 and 1.8; docs/prism_bake.md), in
the Vulkan backend. A map's structure BSP is baked from what the game
draws: SCENE_STATIC's level as drawn (its near level: what the player sees
close by), display.prism_rt's materials (the mean albedo, the alpha maps,
the glows) and its light rig (RT_FRAME's: the sun, the sky, the domes, the
lamps), with the path tracer's reference estimator (xgpu_vulkan_bake_glsl.
inc), into one bake file a BSP (prism_bake_cache.h). It is a list of jobs:

  the plan (on a thread of its own): the atlas (prism_bake_atlas.h: CE's
    lightmap pages scaled chart by chart, the density of each chart by its
    surfaces' importance tier, which the game's reach gave: PRISM_BAKE's
    tiers), its tiles of 16 x 16 texels in each virtual page (a CE page at
    a chart's scale), each tile's triangles; the light-importance grid's
    cells (the level's surfaces' points, a cell of world units each) and
    each cell's lamps to weigh; the irradiance volume's probes (the reach's
    bricks, 4 x 4 x 4 probes a world unit);
  the importance grid: each cell's candidate lamps weighed on the GPU by
    the light each gives the cell's points with its shadow traced, the 64
    most kept (its seen), beside the 64 strongest unshadowed at its middle
    (its near): a lamp is picked from the seen, the near or by power over
    the level, a mixture in which every lamp can be picked (unbiased);
  the lightmap's tiles: a batch of tiles at a time, each texel's samples
    added until its noise is under the threshold (prism_bake_converge.h's
    rule: D4's 1%, at least 64 samples, black below a floor; no budget at
    `max`), then the tile written (the light less the sun's, its direction,
    the indirect light alone, the sun's visibility) and drawn;
  the sun mask: each tile whose texels are neither all lit nor all dark
    baked again at 4 times its density, the sun's visibility alone;
  the volume: each brick's probes (SH L1 of the light from every direction,
    the sun's visibility, whether the probe is inside the level);
  the footprints: each switchable fixture's tiles within its reach.

Each finished job is appended to the bake file (<folder>/<map>/<bsp>.pbk.new)
with its checksum; a quit or a crash resumes at the next unfinished job.
Once all are done, the live records are copied into <bsp>.pbk and the log
is let go of. A frame gives the bake the control's budget of GPU time (a
few milliseconds while playing, most of the frame in the menus or with
debug.bake_exit), measured by its own timestamps.

PRISM Raster reads the bake from the GPU: the virtual pages' tile tables,
the tiles' texels, the sun's table and masks, the volume's two cascades'
bricks, the reflection probes, each level pixel finding its texel through
its CE lightmap coordinates and page (the G-buffer's), each object pixel
its probes about its point, and makes the opaque frame whole: diffuse
light by CE's base colour, the material table's specular by the probes.

Besides the level the bake traces the static scenery (the reach's inputs:
the scenario's unmoving placements' drawn triangles, with their albedo and
coverage) and lights the scenario's light fixtures (also the reach's
inputs); the rig is in the light's units (prism_rt.c's PRISM_RT_UNITS_*).

The scene the frame traces (Rebaked's light pass, Path Traced: the lookup's
scene block) is the bake's but for the static scenery's runs a mod's mesh
draws (the reach's last section, prism_bake.c's scenery_copy): those
placements' stock triangles are uploaded after the others of their kind,
the bake's structures hold them all, and the frame's (scene_runtime) are
built over the others alone, so that the bake's light and hash stay the
stock scenery's while the frame traces each such rock as it is drawn (the
game sends it as a moving object). The two blocks are alike but for their
top level.
*/

#include "xgpu_vulkan.h"
#include "xgpu_vulkan_bake.h"
#include "xgpu_vulkan_bake_shaders.h"
#include "xgpu_vulkan_rt.h"
#include "xgpu_vulkan_scene.h"
#include "xgpu_vulkan_terrain.h"
#include "host.h"
#include "prism_bake_atlas.h"
#include "prism_bake_batch.h"
#include "prism_bake_cache.h"
#include "prism_bake_converge.h"
#include "prism_bake_filter.h"
#include "prism_bake_layers.h"
#include "prism_bake_reach.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- constants */

/* the bake file's maker (a change makes every bake stale): xgpu_stream.h's */
#define BAKE_VERSION XGPU_BAKE_VERSION
/* a tile's texels, a mask's */
#define TILE 16u
#define TILE_TEXELS (TILE * TILE)
#define MASK_FACTOR 4u
#define MASK_SIDE (TILE * MASK_FACTOR)
#define MASK_TEXELS (MASK_SIDE * MASK_SIDE)
/* the accumulators' floats a probe; a texel's (the probe's and its 8
groups' sums of luminance: the robust mean's, prism_bake_filter.h) */
#define ACCUMULATOR_FLOATS 32u
#define TEXEL_ACCUMULATOR_FLOATS 40u
/* a batch's tiles at most (its texels' working set: 2048 x 256 x 64 bytes,
32 MB, and the records' 16 MB) */
#define BATCH_TILES 4096u
/* the masks' tiles a batch */
#define MASK_BATCH 256u
/* the probes a batch */
#define VOLUME_BATCH (64u * 4096u)
/* a raster tile's words (2 a texel), the bake file's tile (4 a texel), the
filter's words of a tile (4 a texel: the normal, the chart + 1, the
variance of the level, the robust mean's factor) */
#define RASTER_TILE_WORDS (TILE_TEXELS * 2u)
#define FILE_TILE_WORDS (TILE_TEXELS * 4u)
#define AUX_TILE_WORDS (TILE_TEXELS * 4u)
/* the layered bake's shares (prism_bake_layers.h): a tile's, a brick's */
#define ATLAS_LAYER_BYTES (TILE_TEXELS * PRISM_BAKE_LAYER_BYTES)
#define VOLUME_LAYER_BYTES (BRICK_PROBES * PRISM_BAKE_LAYER_BYTES)
/* the filtered tiles waiting to be written and drawn at most, and those a
frame writes */
#define FILTER_RING 2048u
#define FILTER_FRAME 1024u
/* the importance grid: a cell's side, its candidates, the words a cell */
#define GRID_CELL 2.0f
#define GRID_CANDIDATES 128u
#define GRID_POINTS 8u
#define GRID_LIST 64u
#define GRID_WORDS 3u
#define GRID_NEAR 64u
#define GRID_REACH 48.0f
/* the volume's probes: a brick's (a world unit), their spacing */
#define BRICK_PROBES 64u
#define PROBE_SPACING 0.25f
#define PROBE_WORDS 8u
/* the fine cascade's bricks a BSP at most: 12.6 M probes, 403 MB of results
on the GPU (and 201 MB of their places), so that the volume fits an 8 GB
card beside the lightmap's working set. Every campaign BSP but one holds
under 5.3 M probes in all; d40's eighth, its open terrain, would hold 397 M
(12.7 GB), the reach's floors walked over the whole valley. Past the budget
the bricks farthest from the level's shapes are left to the coarse cascade
(Unreal's volumetric lightmap drops "the high-density bricks farthest from
geometry first"), which the game reads where the fine has no brick */
#define VOLUME_FINE_BRICKS_MOST 196608u
/* the staging ring's slots (frames), each's bytes */
#define STAGING_SLOTS 4u
#define STAGING_BYTES (24u << 20)
#define READBACK_SLOTS 4u
/* the static scenery's triangle: its corners, albedo and coverage (the
game's reach inputs': prism_bake.c's SCENERY_FLOATS) */
#define SCENERY_FLOATS 13
#define FIXTURE_FLOATS 12
/* the static scenery's materials at most (each albedo and coverage alike
shares one) */
#define SCENERY_MATERIALS 4096u
/* PRISM Raster's dynamic lights a frame at most, and where they are in
the raster's job buffer; the volume's coarse cascade's probe spacing (its
bricks 4 probes a side: 8 world units) */
#define RASTER_LIGHTS 64u
#define RASTER_LIGHTS_OFFSET 1024u
#define COARSE_SPACING 2.0f
/* the reflection probes: a cell's side (world units) each has one, their
height above the floor, the most, their cubes' side and levels (the texels
of all six faces of every level), the probes a frame's batch and the grid's
probes a cell lists */
#define REFLECTION_CELL 8.0f
#define REFLECTION_HEIGHT 1.0f
#define REFLECTIONS_MOST 4096u
#define REFLECTION_SIDE 32u
#define REFLECTION_LEVELS 4u
#define REFLECTION_TEXELS (6u * (32u * 32u + 16u * 16u + 8u * 8u + 4u * 4u))
#define REFLECTION_BATCH 8u
#define REFLECTION_LIST 8u
#define COARSE_HEIGHT 12
/* the spread of a texel's samples: the most invocations a texel, and the
invocations the running texels are spread to fill */
#define SPREAD_MOST 16u
/* the samples a dispatch begins with, at a step's start and a probe batch's;
and the most samples of all its texels or probes one dispatch takes (16.8
million paths: a fraction of a second on an RTX 2060), whatever the budget's
measure: the measure is of the last frames' work, and a step or batch that
begins with far more texels or probes than the last frames had would
otherwise run for seconds in one dispatch (an RTX 2060 under GPU-assisted
validation: 11 s, the probes' first dispatch at 4096 samples after the
grid's light work; Windows's limit is 2 s, then the device is lost) */
#define SAMPLES_FIRST 4u
#define SAMPLES_WORK_MOST (1u << 24)
#define SPREAD_INVOCATIONS 65536u
/* the fixtures the bake takes at most */
#define FIXTURES_MOST 4096u
/* the bounces (the reference's: XGPU_RT_REFERENCE_BOUNCES) */
#define BOUNCES 32
/* the black floor: 1/1000 of middle grey (D4, docs/prism_gates.md) */
#define BLACK_FLOOR (0.18f * 1.0e-3f)
/* `max`'s hard cap on a texel's or probe's samples: the 1% rule under the
noise floor finishes almost every texel far below it (c10's most about
50,000), but a handful of near-black texels lit by rare paths never meet it
(a50's first BSP: 5 texels still running after 13 half-hour slices, one at
242,944 samples). A texel stopped by the cap closes at its running mean; the
robust mean and the filter take its residual noise. Counted in the log
("capped"). Samples, not seconds, so the bake stays the same on every card */
#define MAX_SAMPLES_CAP 65536.0f

#define BAKE_FUNCTIONS(F) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) \
	F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) \
	F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
	F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) \
	F(vkCmdDispatch) F(vkCmdDispatchIndirect) F(vkCmdUpdateBuffer) F(vkCmdPushConstants) F(vkCmdPipelineBarrier) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkCmdFillBuffer) \
	F(vkGetQueryPoolResults) F(vkGetBufferDeviceAddress) F(vkCreateAccelerationStructureKHR) \
	F(vkDestroyAccelerationStructureKHR) F(vkGetAccelerationStructureBuildSizesKHR) \
	F(vkCmdBuildAccelerationStructuresKHR) F(vkGetAccelerationStructureDeviceAddressKHR) F(vkCmdCopyBuffer)

#define DECLARE(name) static PFN_##name name;
BAKE_FUNCTIONS(DECLARE)
#undef DECLARE

/* a brick's flags (the reach's: its walk's floors, a body's or camera's
air, flight's) */
enum
{
	XGPU_BAKE_BRICK_WALK = 1,
	XGPU_BAKE_BRICK_CAMERA = 2,
	XGPU_BAKE_BRICK_FLIGHT = 4,
};

/* ---------- structures */

struct bake_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkDeviceAddress address;
	void *mapped;
};

/* the scene's block (xgpu_vulkan_bake_glsl.inc's Scene, field for field) */
struct bake_scene_block
{
	VkDeviceAddress tlas, vertices, info, materials, alpha, lamps, grid, unused;
	uint32_t counts[4];
	float sun[4];
	float rig[16][4];
	float lamp[4];
	uint32_t settings[4];
};

struct bake_push
{
	VkDeviceAddress scene, job;
	uint32_t parameters[4];
	float values[4];
};

enum
{
	PIPELINE_SETUP,
	PIPELINE_LIGHTMAP,
	PIPELINE_MASK,
	PIPELINE_IMPORTANCE,
	PIPELINE_VOLUME,
	PIPELINE_RASTER,
	PIPELINE_COMPACT,
	PIPELINE_REFLECTION,
	PIPELINE_EXPOSURE,
	PIPELINES
};

/* a virtual page: a CE page at a scale (prism_bake_atlas.h's), its tiles'
first in the table */
struct bake_vpage
{
	int32_t page, scale;
	int32_t tiles_x, tiles_y;
	uint32_t table_first;
};

/* a tile of the lightmap: its virtual page and place, its triangles' range
in the tile lists, its state, its physical tile in the raster pool (0
none), its sun's state (0 unknown, 1 dark, 2 lit, 3 + mask) */
enum
{
	TILE_PENDING,
	TILE_BATCHED,
	TILE_DONE,
};
struct bake_tile
{
	uint32_t vpage;
	uint16_t x, y;
	uint32_t triangle_first, triangle_count;
	uint8_t state, mask_state, masked;
	uint32_t physical;
	uint32_t sun;
	uint64_t job;
};

/* the batch: its slots' tiles (index into the tiles, or none) */
struct bake_batch
{
	uint32_t tiles[BATCH_TILES];
	uint32_t count;
	/* the slots whose results were asked for (a readback), by slot */
	uint8_t asked[BATCH_TILES];
};

struct bake_readback
{
	struct bake_buffer buffer;
	uint64_t serial;
	int busy;
	/* what it holds: the running counts (BATCH_TILES words), then the
	finished slots' accumulators and states */
	uint32_t slots[BATCH_TILES];
	uint32_t tiles[BATCH_TILES];
	uint32_t finished;
	int kind;
	/* the volume's batch it is of (prism_bake_batch.h) */
	uint32_t batch;
};

enum
{
	STEP_NONE,
	STEP_PLAN,
	STEP_UPLOAD,
	STEP_LOAD,
	STEP_IMPORTANCE,
	STEP_LIGHTMAP,
	STEP_MASK,
	STEP_VOLUME,
	STEP_REFLECTIONS,
	STEP_FOOTPRINTS,
	STEP_COMMIT,
	STEP_DONE,
	STEP_FAILED,
};

/* the plan's inputs and outputs (the thread's) */
struct bake_plan
{
	/* in: the scene's triangles kept (their SCENE_STATIC indices), their
	positions, attributes, tiers; the lamps; the bricks */
	uint32_t triangle_count;
	float *positions;
	uint32_t *attributes;
	uint8_t *tiers;
	uint32_t *pages; /* width, height each */
	uint32_t page_count;
	float *lamps;
	uint32_t lamp_count;
	int16_t *bricks;
	uint32_t brick_count;
	int quality;
	/* out */
	struct prism_bake_atlas atlas;
	struct bake_vpage *vpages;
	uint32_t vpage_count;
	struct bake_tile *tiles;
	uint32_t tile_count, table_words;
	uint32_t *tile_triangles;
	size_t tile_triangle_count;
	uint32_t *atlas_scene; /* each atlas triangle's kept triangle */
	uint16_t *charts; /* each page's CE texels: virtual page + 1 */
	uint32_t *chart_offsets; /* each page's first texel in charts */
	size_t chart_texels;
	/* the grid's cells: their keys, points (GRID_POINTS x 4 floats), candidates */
	uint32_t cell_count;
	int32_t *cell_keys; /* x y z */
	float *cell_points;
	uint32_t *cell_candidates;
	/* the probes: the fine cascade's (fine_count, 0.25 world units apart),
	then the coarse one's (COARSE_SPACING apart, over the air the fine
	cascade does not reach: tree tops, the far side of a canyon) */
	uint32_t probe_count, fine_count;
	/* the fine cascade's bricks the budget left to the coarse cascade
	(VOLUME_FINE_BRICKS_MOST), of fine_wanted */
	uint32_t fine_dropped, fine_wanted;
	float *probes;
	/* the reflection probes: one each REFLECTION_CELL cell of the walk's
	floors, above them (4 floats each) */
	uint32_t reflection_count;
	float *reflections;
	int failed;
	double seconds;
	/* set (released) as the plan's thread ends: its join then waits for
	nothing (pthread_tryjoin_np is glibc's alone; the Windows host's
	pthreads have none) */
	int done;
};

/* a tile the filter has made: its index, its file's words, its raster's */
struct filter_result
{
	uint32_t tile;
	uint32_t file[FILE_TILE_WORDS];
	uint32_t raster[RASTER_TILE_WORDS];
	/* (the tile's layers, the log's, where it has them) */
	uint8_t layers[ATLAS_LAYER_BYTES];
	int has_layers;
};

/* a BSP's bake */
struct bake_state
{
	int step;
	/* (the step a failure stopped it at: what it had made by then stays
	traced) */
	int failed_at;
	uint32_t generation;
	int32_t bsp;
	uint32_t quality, request;
	char map[XGPU_BAKE_NAME];
	char folder[XGPU_BAKE_PATH];
	char path[XGPU_BAKE_PATH + 160], new_path[XGPU_BAKE_PATH + 164];
	struct prism_bake_cache_key key;
	struct prism_bake_cache cache;
	struct prism_bake_cache_writer writer;
	int writing;
	double started, seconds;
	uint64_t rays;
	/* the plan, on its thread */
	struct bake_plan plan;
	pthread_t thread;
	int thread_running, plan_ready;
	/* the scene on the GPU */
	struct bake_buffer vertices, info, materials, alpha, lamps, grid, scene;
	struct bake_buffer blas_storage[4], tlas_storage, instances;
	VkAccelerationStructureKHR blas[4], tlas;
	/* the frame's traced scene where it differs (the static scenery a mod's
	mesh draws left out): its scenery's two structures, its top level, and
	its scene block (no buffer: the bake's own serves) */
	struct bake_buffer runtime_blas_storage[2], runtime_tlas_storage, runtime_instances, scene_runtime;
	VkAccelerationStructureKHR runtime_blas[2], runtime_tlas;
	VkDeviceAddress runtime_tlas_address;
	uint32_t opaque_count, alpha_count, material_count, lamp_count, grid_entries;
	/* the static scenery's triangles (the reach's, copied: SCENERY_FLOATS
	each), and its opaque and cutout counts as uploaded, and of them those
	the frame traces (the first of each kind: the rest a mod's mesh draws);
	the runs a mod's mesh draws (first and count each) */
	float *scenery;
	uint32_t scenery_count, scenery_opaque, scenery_cutout, scenery_opaque_kept, scenery_cutout_kept;
	int32_t *scenery_replaced;
	uint32_t scenery_replaced_count;
	/* per scenery triangle its base map coordinates (6 floats) and alpha
	map (-1 none), and the maps (each width, height, then its texels a byte
	each padded to a word), the reach's copied (NULL where it sent none) */
	float *scenery_uv;
	int32_t *scenery_map;
	uint32_t *scenery_maps;
	uint32_t scenery_map_count, scenery_map_words;
	/* the fixtures (16 floats each: xgpu_vulkan_bake_glsl.inc's
	fixture_light) and their buffer */
	float *fixtures;
	uint32_t fixture_count, fixtures_seen;
	struct bake_buffer fixtures_buffer;
	float lamp_power;
	struct bake_scene_block block;
	/* the raster lookup's */
	struct bake_buffer pages, charts, vpages, table, pool, sun, masks, bricks, probes, raster_job;
	/* PRISM Path Traced's (xgpu_vulkan_pt.c): the indirect light alone, a
	word a texel (RGB9E5) in the pool's physical tiles' order; each level
	triangle's lightmap page and coordinates (4 words, by its kept index:
	the info's word 7) */
	struct bake_buffer pool_indirect, lightmaps;
	/* the final gather's: each kept triangle's atlas triangle (+1; 0 none), and
	the cache's block (the virtual pages, the tile table, the pool, the atlas
	triangles, that map) the lightmap's and the volume's passes read */
	struct bake_buffer plan_atlas, cache_block;
	uint32_t mask_capacity, mask_count, physical_count, brick_entries, coarse_entries;
	struct bake_buffer bricks_coarse;
	uint32_t *table_host, *sun_host;
	/* the batch's working buffers */
	struct bake_buffer ranges, triangles, texels, accumulators, states, running, tile_origins, job;
	/* the running texels' or probes' indices, packed, and their dispatch's
	groups (x, 1, 1, then their count) */
	struct bake_buffer compact_list, compact_args;
	struct bake_batch batch;
	uint32_t tiles_done, tiles_planned, frame;
	double texels_done, texels_planned, texels_baked;
	uint32_t samples_most, running_texels;
	/* (the finished texels whose last samples the firefly rule asked for:
	their largest sample over their mean wanted more than their noise did) */
	double texels_firefly_bound;
	/* HALO_BAKE_SEED's (0 none) */
	uint32_t seed;
	/* the `max` tier's noise floor (radiance; 0 none) and the texels it
	stopped (converged with their light below it) */
	float noise_floor;
	double texels_floor, texels_converged, texels_capped;
	/* the GPU time this run gave the bake (its timestamps') */
	double gpu_seconds;
	/* the importance step: the cells weighed so far */
	uint32_t cells_done;
	struct bake_buffer cell_points, cell_candidates, cell_weights;
	float *weights_host;
	int grid_ready;
	/* the masks: the tiles to mask, the next */
	uint32_t *mask_tiles;
	uint32_t mask_tile_count, masks_done, masks_batched;
	struct bake_buffer mask_ranges, mask_out;
	/* the volume: its batches of probes (done: those written) */
	struct prism_bake_batches volume;
	int volume_loaded;
	/* display.bake "off" (or "ask" and not asked) when the file was opened:
	nothing written, nothing baked (a whole bake, or an unfinished log of
	this key read, is drawn); begun again once a bake is wanted */
	int idle;
	/* the reflection probes: their places, info, texels and hashed grid on
	the GPU, the grid's entries, those done and in the batch running */
	struct bake_buffer reflection_points, reflection_info, reflection_data, reflection_table;
	uint32_t reflection_entries, reflections_done, reflections_batched;
	int reflections_made, reflections_loaded;
	struct bake_buffer probe_points, probe_accumulators, probe_states, probe_statistics;
	float *probe_host;
	/* how the light drawn stands (XGPU_BAKE_SHOWN_*) */
	int shown, complete;
	/* the draft (a background bake's first pass: every tile at the draft's
	64 samples, drawn at once, written as its tile's checkpoint) */
	int drafting;
	uint32_t drafted;
	uint64_t file_bytes;
	int logged_resumed;
	/* the noise (prism_bake_filter.h): the robust mean's rule and the texels
	it took to their groups' median; the filter's parameters (passes 0:
	none), its thread (state 0 not begun, 1 running, 2 done), the tiles it
	has filtered waiting to be written and drawn (a ring, filter_lock's),
	the tiles the file had filtered, the filter's time */
	struct prism_bake_robust_parameters robust;
	int robust_on;
	double texels_robust;
	struct prism_bake_filter_parameters filter;
	pthread_t filter_thread;
	int filter_state, filter_stop, filter_finished, filter_failed;
	pthread_mutex_t filter_lock;
	pthread_cond_t filter_room;
	struct filter_result *filter_ring;
	uint32_t filter_tail, filter_count, filter_tiles, tiles_filtered;
	double filter_started, filter_seconds;
	/* the layered bake (prism_bake_layers.h, docs/prism_bake.md, "The
	layers"): the light the bake was made with (its base: known once made
	or read from the file), whether a texel baked was a remodel's, the
	weights what is drawn was recombined by; once they change, everything
	drawn recombined again from the file (relighting: its reader, the next
	tile, brick and reflection probe); the volume's light-weighted mean
	ratio a channel, the reflection probes' */
	struct prism_bake_layer_light base;
	int base_known, saw_remodel;
	/* (the filter's passes a diagnostic asked for, -1 none: the key's
	settings at another quality, key_settings_hash) */
	int filter_asked;
	/* the bake drawn is the shipped one (a release's, read only: <shipped>/
	<stock|remodel>/<map>/bsp<n>.pbk): its quality, at least the one asked */
	int shipped;
	uint32_t loaded_quality;
	/* a whole bake found before the plan (file_choose: 1 the shipped one,
	2 this machine's), loaded into cache: the plan is made at its quality */
	int chosen;
	struct prism_bake_layer_weights weights;
	int relighting, relight_open, relight_logged;
	struct prism_bake_cache relight_reader;
	uint32_t relight_tile, relight_probe, relight_reflection, relights;
	double relight_started;
	double volume_ratio_sum[3], volume_ratio_weight[3];
};

/* ---------- globals */

static struct
{
	int made, failed;
	struct xgpu_vulkan_rt_device device;
	VkDescriptorSetLayout layouts[PIPELINES];
	VkPipelineLayout pipeline_layouts[PIPELINES];
	VkPipeline pipelines[PIPELINES];
	VkDescriptorPool pool;
	VkDescriptorSet raster_sets[4];
	uint32_t raster_set_next;
	VkSampler sampler;
	VkQueryPool queries;
	uint64_t query_serial[8];
	int query_written[8];
	uint32_t query_next;
	double timestamp_period;
	VkDeviceSize scratch_alignment;
	/* the staging ring */
	struct bake_buffer staging[STAGING_SLOTS];
	uint64_t staging_serial[STAGING_SLOTS];
	uint32_t staging_slot, staging_used;
	struct bake_readback readbacks[READBACK_SLOTS];
	/* garbage after a serial */
	struct { uint64_t serial; struct bake_buffer buffer; VkAccelerationStructureKHR structure; } garbage[256];
	uint32_t garbage_count;
	/* the control and the reach: its inputs as they come (words), its
	thread */
	struct xgpu_stream_prism_bake control;
	int control_known;
	uint32_t *reach_input;
	uint32_t reach_words, reach_received;
	pthread_t reach_thread;
	int reach_running, reach_flight;
	uint32_t reach_summary[8];
	uint32_t reach_generation;
	int32_t reach_bsp;
	uint8_t *tiers;
	uint32_t tier_count;
	int16_t *bricks;
	uint32_t brick_count, brick_total;
	int reach_whole;
	/* the reach's file (reach_file_path: <folder>/<map>/bsp<n>.reach, made
	as its inputs end, the thread's to read and write) */
	char reach_path[XGPU_BAKE_PATH + 200];
	/* the static scenery's triangles (SCENERY_FLOATS each: corners, albedo,
	coverage), the reach's inputs' */
	float *scenery;
	uint32_t scenery_count;
	/* of them the runs a mod's mesh draws (first triangle and count each) */
	int32_t *scenery_replaced;
	uint32_t scenery_replaced_count;
	/* their base map coordinates, cutouts' alpha maps and the maps (the
	bake state's scenery_uv, scenery_map, scenery_maps) */
	float *scenery_uv;
	int32_t *scenery_map;
	uint32_t *scenery_maps;
	uint32_t scenery_map_count, scenery_map_words;
	/* the scenario's light fixtures in the BSP (FIXTURE_FLOATS each), the
	reach's inputs' */
	float *fixtures_input;
	uint32_t fixtures_input_count;
	/* the bake */
	struct bake_state *bake;
	/* the samples a batch's dispatch takes, as the budget allows; the step
	they were measured on (another step's work a dispatch differs: they
	begin again from SAMPLES_FIRST) */
	uint32_t samples;
	int samples_step;
	double gpu_milliseconds;
	struct xgpu_vulkan_image *raster_light;
	/* (the terrain's micro-shadows' stand-in: a white texel) */
	struct xgpu_vulkan_image *micro_white;
	uint32_t raster_frames;
	/* the exposure's meter (xgpu_vulkan_bake_glsl.inc's EXPOSURE_COMMON: 128
	bins, the exposure's log2, whether it is made, the last medians), kept
	from frame to frame; with HALO_RASTER_EXPOSURE_LOG its last four words
	copied each step to a host buffer and logged (a diagnostic) */
	struct bake_buffer meter, meter_log;
	double exposure_clock;
	int meter_logging;

	uint32_t status_serial;
	double last_status;
} bake;

/* ---------- time */

static double now_seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1.0e-9;
}

/* ---------- memory */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < bake.device.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (bake.device.memory->memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int buffer_make(VkDeviceSize size, VkBufferUsageFlags usage, int host, struct bake_buffer *made)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateFlagsInfo flags;
	VkMemoryAllocateInfo allocate;
	VkBufferDeviceAddressInfo address;
	int type;

	memset(made, 0, sizeof(*made));
	if (size < 16)
		size = 16;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(bake.device.device, &info, NULL, &made->buffer) != VK_SUCCESS)
		return 0;
	vkGetBufferMemoryRequirements(bake.device.device, made->buffer, &requirements);
	type = memory_type(requirements.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT :
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0 && host)
		type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	memset(&flags, 0, sizeof(flags));
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &flags;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || vkAllocateMemory(bake.device.device, &allocate, NULL, &made->memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(bake.device.device, made->buffer, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	if (vkBindBufferMemory(bake.device.device, made->buffer, made->memory, 0) != VK_SUCCESS ||
		(host && vkMapMemory(bake.device.device, made->memory, 0, VK_WHOLE_SIZE, 0, &made->mapped) != VK_SUCCESS))
	{
		vkDestroyBuffer(bake.device.device, made->buffer, NULL);
		vkFreeMemory(bake.device.device, made->memory, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	made->size = size;
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address.buffer = made->buffer;
	made->address = vkGetBufferDeviceAddress(bake.device.device, &address);
	return 1;
}

static void buffer_free_now(struct bake_buffer *buffer)
{
	if (buffer->buffer)
		vkDestroyBuffer(bake.device.device, buffer->buffer, NULL);
	if (buffer->memory)
		vkFreeMemory(bake.device.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

/* let go of after the submission being recorded */
static void garbage_add(struct bake_buffer *buffer, VkAccelerationStructureKHR structure)
{
	if (bake.garbage_count == sizeof(bake.garbage) / sizeof(bake.garbage[0]))
	{
		xgpu_vulkan_wait(bake.garbage[0].serial);
		if (bake.garbage[0].structure)
			vkDestroyAccelerationStructureKHR(bake.device.device, bake.garbage[0].structure, NULL);
		buffer_free_now(&bake.garbage[0].buffer);
		bake.garbage[0] = bake.garbage[--bake.garbage_count];
	}
	bake.garbage[bake.garbage_count].serial = xgpu_vulkan_serial();
	if (buffer)
		bake.garbage[bake.garbage_count].buffer = *buffer;
	else
		memset(&bake.garbage[bake.garbage_count].buffer, 0, sizeof(struct bake_buffer));
	bake.garbage[bake.garbage_count].structure = structure;
	bake.garbage_count++;
	if (buffer)
		memset(buffer, 0, sizeof(*buffer));
}

static void garbage_collect(void)
{
	uint64_t done = xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < bake.garbage_count)
	{
		if (bake.garbage[index].serial <= done)
		{
			if (bake.garbage[index].structure)
				vkDestroyAccelerationStructureKHR(bake.device.device, bake.garbage[index].structure, NULL);
			buffer_free_now(&bake.garbage[index].buffer);
			bake.garbage[index] = bake.garbage[--bake.garbage_count];
			continue;
		}
		index++;
	}
}

static void barrier(VkCommandBuffer commands)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
		&memory, 0, NULL, 0, NULL);
	/* (the readbacks' host reads: their own stage, which ALL_COMMANDS does
	not take) */
	memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory, 0, NULL,
		0, NULL);
}

/* bytes of the staging ring for this frame's copies: their place (host)
and the buffer and offset (NULL: no room this frame) */
static void *staging_take(uint32_t size, VkBuffer *buffer, VkDeviceSize *offset)
{
	struct bake_buffer *slot;
	uint32_t at;

	size = (size + 15u) & ~15u;
	if (size > STAGING_BYTES)
		return NULL;
	slot = &bake.staging[bake.staging_slot];
	if (!slot->buffer)
		return NULL;
	if (bake.staging_serial[bake.staging_slot] != xgpu_vulkan_serial())
	{
		/* (a new frame: the next slot, once the GPU is done with it) */
		uint32_t next = (bake.staging_slot + 1) % STAGING_SLOTS;

		if (bake.staging_serial[bake.staging_slot] != 0)
		{
			if (bake.staging_serial[next] > xgpu_vulkan_completed())
				return NULL;
			bake.staging_slot = next;
			slot = &bake.staging[next];
		}
		bake.staging_serial[bake.staging_slot] = xgpu_vulkan_serial();
		bake.staging_used = 0;
	}
	if (bake.staging_used + size > STAGING_BYTES)
		return NULL;
	at = bake.staging_used;
	bake.staging_used += size;
	*buffer = slot->buffer;
	*offset = at;
	return (char *)slot->mapped + at;
}

/* data copied into a device buffer at offset, in this frame's commands:
0 where the staging ring has no room (try again next frame) */
static int upload(VkCommandBuffer commands, struct bake_buffer *to, VkDeviceSize offset, const void *data, uint32_t size)
{
	VkBuffer from;
	VkDeviceSize at;
	VkBufferCopy copy;
	void *place = staging_take(size, &from, &at);

	if (!place)
		return 0;
	memcpy(place, data, size);
	copy.srcOffset = at;
	copy.dstOffset = offset;
	copy.size = size;
	vkCmdCopyBuffer(commands, from, to->buffer, 1, &copy);
	return 1;
}

/* a device buffer made and filled from host data (through a host buffer
let go of after the submission) */
static int buffer_filled(VkCommandBuffer commands, const void *data, VkDeviceSize size, struct bake_buffer *made,
	VkBufferUsageFlags usage)
{
	struct bake_buffer host;
	VkBufferCopy copy;

	if (!buffer_make(size, usage, 0, made))
		return 0;
	if (!data)
	{
		vkCmdFillBuffer(commands, made->buffer, 0, VK_WHOLE_SIZE, 0);
		return 1;
	}
	if (!buffer_make(size, 0, 1, &host))
	{
		buffer_free_now(made);
		return 0;
	}
	memcpy(host.mapped, data, (size_t)size);
	copy.srcOffset = copy.dstOffset = 0;
	copy.size = size;
	vkCmdCopyBuffer(commands, host.buffer, made->buffer, 1, &copy);
	garbage_add(&host, VK_NULL_HANDLE);
	return 1;
}

/* ---------- made once */

static VkShaderModule shader_make(const uint32_t *words, size_t count)
{
	VkShaderModuleCreateInfo info;
	VkShaderModule module = VK_NULL_HANDLE;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = count * sizeof(uint32_t);
	info.pCode = words;
	if (vkCreateShaderModule(bake.device.device, &info, NULL, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

static int pipeline_make(int which, const VkDescriptorType *types, uint32_t count, const uint32_t *words,
	size_t word_count)
{
	VkPushConstantRange push;
	VkDescriptorSetLayoutBinding bindings[8];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkComputePipelineCreateInfo info;
	VkShaderModule module;
	uint32_t index;
	VkResult result;

	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < count; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = types[index];
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = count;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(bake.device.device, &set_info, NULL, &bake.layouts[which]) != VK_SUCCESS)
		return 0;
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.size = sizeof(struct bake_push);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = count ? 1 : 0;
	layout_info.pSetLayouts = &bake.layouts[which];
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(bake.device.device, &layout_info, NULL, &bake.pipeline_layouts[which]) != VK_SUCCESS)
		return 0;
	module = shader_make(words, word_count);
	if (!module)
		return 0;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = bake.pipeline_layouts[which];
	result = vkCreateComputePipelines(bake.device.device, VK_NULL_HANDLE, 1, &info, NULL, &bake.pipelines[which]);
	vkDestroyShaderModule(bake.device.device, module, NULL);
	return result == VK_SUCCESS;
}

static int make(void)
{
	static const VkDescriptorType raster_types[] =
	{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	};
	VkPhysicalDeviceAccelerationStructurePropertiesKHR structure_properties;
	VkPhysicalDeviceProperties2 properties;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[4];
	VkSamplerCreateInfo sampler;
	VkQueryPoolCreateInfo queries;
	uint32_t index;

	if (bake.made)
		return 1;
	if (bake.failed)
		return 0;
	bake.failed = 1;
	if (!xgpu_vulkan_rt_device(&bake.device))
		return 0;
#define LOAD(name) name = (PFN_##name)bake.device.get_device_proc(bake.device.device, #name); \
	if (!name) { host_logf(HOST_LOG_WARN, "prism bake: no %s: no bake", #name); return 0; }
	BAKE_FUNCTIONS(LOAD)
#undef LOAD
	bake.scratch_alignment = 256;
	memset(&structure_properties, 0, sizeof(structure_properties));
	structure_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	memset(&properties, 0, sizeof(properties));
	properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties.pNext = &structure_properties;
	if (bake.device.get_physical_properties2)
	{
		bake.device.get_physical_properties2(bake.device.physical, &properties);
		if (structure_properties.minAccelerationStructureScratchOffsetAlignment > bake.scratch_alignment)
			bake.scratch_alignment = structure_properties.minAccelerationStructureScratchOffsetAlignment;
	}
	bake.timestamp_period = bake.device.timestamp_period;
	if (!pipeline_make(PIPELINE_SETUP, NULL, 0, xgpu_vulkan_bake_setup_words, xgpu_vulkan_bake_setup_count) ||
		!pipeline_make(PIPELINE_LIGHTMAP, NULL, 0, xgpu_vulkan_bake_lightmap_words, xgpu_vulkan_bake_lightmap_count) ||
		!pipeline_make(PIPELINE_MASK, NULL, 0, xgpu_vulkan_bake_mask_words, xgpu_vulkan_bake_mask_count) ||
		!pipeline_make(PIPELINE_IMPORTANCE, NULL, 0, xgpu_vulkan_bake_importance_words,
		xgpu_vulkan_bake_importance_count) ||
		!pipeline_make(PIPELINE_VOLUME, NULL, 0, xgpu_vulkan_bake_volume_words, xgpu_vulkan_bake_volume_count) ||
		!pipeline_make(PIPELINE_RASTER, raster_types, 7, xgpu_vulkan_bake_raster_words, xgpu_vulkan_bake_raster_count) ||
		!pipeline_make(PIPELINE_COMPACT, NULL, 0, xgpu_vulkan_bake_compact_words, xgpu_vulkan_bake_compact_count) ||
		!pipeline_make(PIPELINE_REFLECTION, NULL, 0, xgpu_vulkan_bake_reflection_words,
		xgpu_vulkan_bake_reflection_count) ||
		!pipeline_make(PIPELINE_EXPOSURE, NULL, 0, xgpu_vulkan_bake_exposure_words, xgpu_vulkan_bake_exposure_count))
	{
		host_logf(HOST_LOG_WARN, "prism bake: its passes cannot be made: no bake");
		return 0;
	}
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = 24;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = 4;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 4;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(bake.device.device, &pool_info, NULL, &bake.pool) != VK_SUCCESS)
		return 0;
	for (index = 0; index < 4; index++)
		layouts[index] = bake.layouts[PIPELINE_RASTER];
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = bake.pool;
	allocate.descriptorSetCount = 4;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(bake.device.device, &allocate, bake.raster_sets) != VK_SUCCESS)
		return 0;
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	if (vkCreateSampler(bake.device.device, &sampler, NULL, &bake.sampler) != VK_SUCCESS)
		return 0;
	if (bake.timestamp_period > 0.0)
	{
		memset(&queries, 0, sizeof(queries));
		queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queries.queryCount = 16;
		if (vkCreateQueryPool(bake.device.device, &queries, NULL, &bake.queries) != VK_SUCCESS)
			bake.queries = VK_NULL_HANDLE;
	}
	for (index = 0; index < STAGING_SLOTS; index++)
		if (!buffer_make(STAGING_BYTES, 0, 1, &bake.staging[index]))
			return 0;
	bake.samples = SAMPLES_FIRST;
	bake.samples_step = -1;
	bake.reach_bsp = -1;
	bake.made = 1;
	bake.failed = 0;
	host_logf(HOST_LOG_INFO, "prism bake: ready");
	return 1;
}

/* ---------- the plan (its thread: nothing of the GPU's) */

/* a brick's key in the volume's table (xgpu_vulkan_bake_glsl.inc's raster
pass packs it alike) */
static uint32_t brick_key(int32_t x, int32_t y, int32_t z)
{
	return (((uint32_t)(x + 32768)) & 2047u) | ((((uint32_t)(y + 32768)) & 2047u) << 11) |
		((((uint32_t)(z + 32768)) & 1023u) << 22);
}

static uint32_t plan_rng(uint32_t *state)
{
	*state = *state * 747796405u + 2891336453u;
	{
		uint32_t w = ((*state >> ((*state >> 28u) + 4u)) ^ *state) * 277803737u;

		return (w >> 22u) ^ w;
	}
}

static float plan_random(uint32_t *state)
{
	return (float)(plan_rng(state) & 0xffffffu) / 16777216.0f;
}

static void face_normal(const float *p, float n[3], float *area)
{
	float e1[3], e2[3], length;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		e1[axis] = p[3 + axis] - p[axis];
		e2[axis] = p[6 + axis] - p[axis];
	}
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	*area = 0.5f * length;
	if (length > 1.0e-12f)
	{
		n[0] /= length;
		n[1] /= length;
		n[2] /= length;
	}
	else
	{
		n[0] = n[1] = 0.0f;
		n[2] = 1.0f;
	}
}

static uint32_t octahedral(const float n[3])
{
	float s = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]), x, y;
	int32_t ix, iy;

	if (s < 1.0e-12f)
		s = 1.0f;
	x = n[0] / s;
	y = n[1] / s;
	if (n[2] < 0.0f)
	{
		float fx = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f);
		float fy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);

		x = fx;
		y = fy;
	}
	ix = (int32_t)lrintf((x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x) * 32767.0f);
	iy = (int32_t)lrintf((y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y) * 32767.0f);
	return ((uint32_t)ix & 0xffffu) | (((uint32_t)iy & 0xffffu) << 16);
}

/* a 2D point's distance from a triangle (0 inside) */
static float triangle_distance(const float *t, float x, float y)
{
	float best = 1.0e30f, d0, d1, d2;
	int e;

	d0 = (t[2] - t[0]) * (y - t[1]) - (t[3] - t[1]) * (x - t[0]);
	d1 = (t[4] - t[2]) * (y - t[3]) - (t[5] - t[3]) * (x - t[2]);
	d2 = (t[0] - t[4]) * (y - t[5]) - (t[1] - t[5]) * (x - t[4]);
	if ((d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f) || (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f))
		return 0.0f;
	for (e = 0; e < 3; e++)
	{
		float ax = t[e * 2], ay = t[e * 2 + 1], dx = t[((e + 1) % 3) * 2] - ax, dy = t[((e + 1) % 3) * 2 + 1] - ay;
		float len2 = dx * dx + dy * dy, h = len2 > 1.0e-20f ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.0f, px, py, d;

		h = h < 0.0f ? 0.0f : h > 1.0f ? 1.0f : h;
		px = ax + h * dx - x;
		py = ay + h * dy - y;
		d = px * px + py * py;
		if (d < best)
			best = d;
	}
	return sqrtf(best);
}

/* the grid's cells: an open table of their keys */
struct cell_table
{
	uint64_t *keys;
	uint32_t *cells;
	uint32_t capacity, count;
};

/* the importance grid's points on a triangle of an area: 8 a cell's area
(every cell the triangle crosses met), 65536 at most */
static uint32_t grid_points_of(float area)
{
	float points = area / (GRID_CELL * GRID_CELL * 0.125f) + 1.0f;

	return points > 65536.0f ? 65536u : (uint32_t)points;
}

static uint64_t cell_key(int32_t x, int32_t y, int32_t z)
{
	return (((uint64_t)(uint32_t)(x + (1 << 20)) & 0x1fffffu) | ((((uint64_t)(uint32_t)(y + (1 << 20))) & 0x1fffffu) << 21) |
		((((uint64_t)(uint32_t)(z + (1 << 20))) & 0x1fffffu) << 42)) + 1u;
}

static uint32_t hash32(uint32_t v)
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

	return (word >> 22u) ^ word;
}

/* the volume's budget (VOLUME_FINE_BRICKS_MOST): of the count fine bricks
listed (the reach's indices, ascending), those nearest the level's shapes
kept, the rest left to the coarse cascade. A brick's nearness is how many of
its 17 neighbours beside and above it the reach does not hold (solid: a
wall, a rock, an overhang, the slope of a hill; the 9 below are its floor on
open ground too, so they are not counted), then whether it is a floor's
(walked) rather than the air's; ties keep the reach's order. On open terrain
the walls, slopes and props keep their fine bricks and the flat ground's
far from them goes coarse. The same bricks whatever the card: no float, no
time. 0 out of memory */
static int volume_budget(struct bake_plan *plan, uint32_t *fine, uint32_t *count)
{
	uint32_t capacity = 1024, *all, i, b, score, threshold, most = VOLUME_FINE_BRICKS_MOST, kept = 0, histogram[64];
	uint8_t *scores;

	while (capacity < plan->brick_count * 2u + 2u)
		capacity <<= 1;
	all = (uint32_t *)calloc(capacity, sizeof(uint32_t));
	scores = (uint8_t *)malloc((size_t)*count + 1);
	if (!all || !scores)
	{
		free(all);
		free(scores);
		return 0;
	}
	for (b = 0; b < plan->brick_count; b++)
	{
		int16_t const *brick = plan->bricks + b * 4;
		uint32_t key = brick_key(brick[0], brick[1], brick[2]) + 1u, h = hash32(key) & (capacity - 1);

		while (all[h] && all[h] != key)
			h = (h + 1) & (capacity - 1);
		all[h] = key;
	}
	memset(histogram, 0, sizeof(histogram));
	for (i = 0; i < *count; i++)
	{
		int16_t const *brick = plan->bricks + fine[i] * 4;
		int dx, dy, dz;

		score = 0;
		for (dz = 0; dz <= 1; dz++)
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++)
				{
					uint32_t key, h;

					if (!dx && !dy && !dz)
						continue;
					key = brick_key(brick[0] + dx, brick[1] + dy, brick[2] + dz) + 1u;
					h = hash32(key) & (capacity - 1);
					while (all[h] && all[h] != key)
						h = (h + 1) & (capacity - 1);
					score += all[h] != key;
				}
		score = score * 2u + ((brick[3] & XGPU_BAKE_BRICK_WALK) ? 1u : 0u);
		scores[i] = (uint8_t)score;
		histogram[score]++;
	}
	free(all);
	/* (the lowest score kept whole, and how many of the next lower are
	kept, in the reach's order) */
	for (threshold = 63; threshold > 0 && histogram[threshold] <= most; threshold--)
		most -= histogram[threshold];
	for (i = 0; i < *count; i++)
	{
		if (scores[i] > threshold || (scores[i] == threshold && most))
		{
			if (scores[i] == threshold)
				most--;
			fine[kept++] = fine[i];
		}
	}
	free(scores);
	plan->fine_dropped = *count - kept;
	*count = kept;
	return 1;
}

static void *plan_thread(void *context)
{
	struct bake_plan *plan = (struct bake_plan *)context;
	struct prism_bake_atlas_parameters parameters;
	struct prism_bake_atlas_page *pages = NULL;
	struct prism_bake_atlas_triangle *triangles = NULL;
	uint32_t *atlas_scene = NULL, atlas_count = 0, index, tile, *counts = NULL;
	float *best = NULL;
	double started = now_seconds();
	struct cell_table table;
	uint32_t rng = 0x1234567u;

	memset(&table, 0, sizeof(table));
	plan->failed = 1;
	/* the atlas: the triangles with lightmap coordinates on a page */
	pages = (struct prism_bake_atlas_page *)calloc(plan->page_count + 1, sizeof(*pages));
	triangles = (struct prism_bake_atlas_triangle *)calloc(plan->triangle_count + 1, sizeof(*triangles));
	atlas_scene = (uint32_t *)calloc(plan->triangle_count + 1, sizeof(uint32_t));
	if (!pages || !triangles || !atlas_scene)
		goto done;
	for (index = 0; index < plan->page_count; index++)
	{
		pages[index].width = (int32_t)plan->pages[index * 2];
		pages[index].height = (int32_t)plan->pages[index * 2 + 1];
	}
	for (index = 0; index < plan->triangle_count; index++)
	{
		const uint32_t *a = plan->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
		uint32_t page = a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_PAGE_SHIFT;

		if (page == XGPU_SCENE_PAGE_NONE || page >= plan->page_count)
			continue;
		triangles[atlas_count].page = (int32_t)page;
		memcpy(triangles[atlas_count].uv, a + XGPU_SCENE_ATTRIBUTE_ATLAS, 6 * sizeof(float));
		memcpy(triangles[atlas_count].position, plan->positions + (size_t)index * 9, 9 * sizeof(float));
		triangles[atlas_count].tier = plan->tiers ? plan->tiers[index] : 0;
		atlas_scene[atlas_count] = index;
		atlas_count++;
	}
	prism_bake_atlas_defaults(&parameters, (enum prism_bake_quality)plan->quality);
	parameters.tile = (int32_t)TILE;
	if (prism_bake_atlas_build(&plan->atlas, pages, (int32_t)plan->page_count, triangles, atlas_count, &parameters))
		goto done;
	/* the virtual pages and their tiles */
	plan->vpage_count = (uint32_t)plan->atlas.virtual_page_count;
	plan->vpages = (struct bake_vpage *)calloc(plan->vpage_count + 1, sizeof(struct bake_vpage));
	if (!plan->vpages)
		goto done;
	plan->table_words = 0;
	for (index = 0; index < plan->vpage_count; index++)
	{
		struct prism_bake_virtual_page const *vp = &plan->atlas.virtual_pages[index];

		plan->vpages[index].page = vp->page;
		plan->vpages[index].scale = vp->scale;
		plan->vpages[index].tiles_x = vp->tiles_x;
		plan->vpages[index].tiles_y = vp->tiles_y;
		plan->vpages[index].table_first = plan->table_words;
		plan->table_words += (uint32_t)(vp->tiles_x * vp->tiles_y);
		plan->tile_count += (uint32_t)vp->tile_count;
	}
	plan->tiles = (struct bake_tile *)calloc(plan->tile_count + 1, sizeof(struct bake_tile));
	/* (each table entry's tile index + 1, while the lists are made) */
	counts = (uint32_t *)calloc(plan->table_words + 1, sizeof(uint32_t));
	if (!plan->tiles || !counts)
		goto done;
	tile = 0;
	for (index = 0; index < plan->vpage_count; index++)
	{
		struct prism_bake_virtual_page const *vp = &plan->atlas.virtual_pages[index];
		int32_t x, y;

		for (y = 0; y < vp->tiles_y; y++)
			for (x = 0; x < vp->tiles_x; x++)
			{
				if (!vp->tiles[(size_t)y * vp->tiles_x + x])
					continue;
				plan->tiles[tile].vpage = index;
				plan->tiles[tile].x = (uint16_t)x;
				plan->tiles[tile].y = (uint16_t)y;
				plan->tiles[tile].job = prism_bake_job_lightmap_tile(index, 0, (uint32_t)x, (uint32_t)y);
				counts[plan->vpages[index].table_first + (uint32_t)(y * vp->tiles_x + x)] = tile + 1;
				tile++;
			}
	}
	plan->tile_count = tile;
	/* each tile's triangles: those whose box, grown by a texel and a half,
	touches it (two passes: counted, then listed) */
	{
		int pass;
		uint32_t *cursor = (uint32_t *)calloc(plan->tile_count + 1, sizeof(uint32_t));

		if (!cursor)
			goto done;
		for (pass = 0; pass < 2; pass++)
		{
			for (index = 0; index < atlas_count; index++)
			{
				int32_t chart = plan->atlas.triangle_charts[index];
				const float *t = plan->atlas.triangle_texels + (size_t)index * 6;
				struct bake_vpage const *vp;
				float x0, x1, y0, y1, scale;
				int32_t tx0, tx1, ty0, ty1, tx, ty;

				if (chart < 0 || plan->atlas.charts[chart].virtual_page < 0)
					continue;
				vp = &plan->vpages[plan->atlas.charts[chart].virtual_page];
				scale = (float)vp->scale;
				x0 = fminf(t[0], fminf(t[2], t[4])) * scale - 1.5f;
				x1 = fmaxf(t[0], fmaxf(t[2], t[4])) * scale + 1.5f;
				y0 = fminf(t[1], fminf(t[3], t[5])) * scale - 1.5f;
				y1 = fmaxf(t[1], fmaxf(t[3], t[5])) * scale + 1.5f;
				tx0 = (int32_t)floorf(x0 / (float)TILE);
				tx1 = (int32_t)floorf(x1 / (float)TILE);
				ty0 = (int32_t)floorf(y0 / (float)TILE);
				ty1 = (int32_t)floorf(y1 / (float)TILE);
				for (ty = ty0 < 0 ? 0 : ty0; ty <= ty1 && ty < vp->tiles_y; ty++)
					for (tx = tx0 < 0 ? 0 : tx0; tx <= tx1 && tx < vp->tiles_x; tx++)
					{
						uint32_t entry = counts[vp->table_first + (uint32_t)(ty * vp->tiles_x + tx)];

						if (!entry)
							continue;
						if (!pass)
							plan->tiles[entry - 1].triangle_count++;
						else
							plan->tile_triangles[plan->tiles[entry - 1].triangle_first + cursor[entry - 1]++] = index;
					}
			}
			if (!pass)
			{
				size_t total = 0;

				for (tile = 0; tile < plan->tile_count; tile++)
				{
					plan->tiles[tile].triangle_first = (uint32_t)total;
					total += plan->tiles[tile].triangle_count;
				}
				plan->tile_triangle_count = total;
				plan->tile_triangles = (uint32_t *)malloc((total + 1) * sizeof(uint32_t));
				if (!plan->tile_triangles)
				{
					free(cursor);
					goto done;
				}
			}
		}
		free(cursor);
	}
	/* the chart maps: each page's CE texels by the virtual page of the
	chart nearest within three quarters of a texel */
	plan->chart_offsets = (uint32_t *)calloc(plan->page_count + 1, sizeof(uint32_t));
	if (!plan->chart_offsets)
		goto done;
	plan->chart_texels = 0;
	for (index = 0; index < plan->page_count; index++)
	{
		plan->chart_offsets[index] = (uint32_t)plan->chart_texels;
		plan->chart_texels += (size_t)plan->pages[index * 2] * plan->pages[index * 2 + 1];
		/* (each page's texels start on a word: two uint16 a word) */
		plan->chart_texels = (plan->chart_texels + 1) & ~(size_t)1;
	}
	plan->charts = (uint16_t *)calloc(plan->chart_texels + 2, sizeof(uint16_t));
	best = (float *)malloc((plan->chart_texels + 2) * sizeof(float));
	if (!plan->charts || !best)
		goto done;
	for (index = 0; index < plan->chart_texels; index++)
		best[index] = 1.0e30f;
	for (index = 0; index < atlas_count; index++)
	{
		int32_t chart = plan->atlas.triangle_charts[index], page, width, height, x, y;
		const float *t = plan->atlas.triangle_texels + (size_t)index * 6;

		if (chart < 0 || plan->atlas.charts[chart].virtual_page < 0)
			continue;
		page = plan->atlas.charts[chart].page;
		width = (int32_t)plan->pages[page * 2];
		height = (int32_t)plan->pages[page * 2 + 1];
		for (y = (int32_t)floorf(fminf(t[1], fminf(t[3], t[5])) - 1.0f); y <= (int32_t)ceilf(fmaxf(t[1], fmaxf(t[3],
			t[5])) + 1.0f); y++)
			for (x = (int32_t)floorf(fminf(t[0], fminf(t[2], t[4])) - 1.0f); x <= (int32_t)ceilf(fmaxf(t[0], fmaxf(t[2],
				t[4])) + 1.0f); x++)
			{
				size_t at;
				float d;

				if (x < 0 || y < 0 || x >= width || y >= height)
					continue;
				d = triangle_distance(t, (float)x + 0.5f, (float)y + 0.5f);
				at = plan->chart_offsets[page] + (size_t)y * width + x;
				if (d <= 0.75f && d < best[at])
				{
					best[at] = d;
					plan->charts[at] = (uint16_t)(plan->atlas.charts[chart].virtual_page + 1);
				}
			}
	}
	/* the importance grid's cells: points on the level's surfaces (by area,
	at most GRID_POINTS a cell, chosen alike in every run) */
	/* (the table twice the points at most: every cell a point lands in kept,
	a large triangle's as much as a small one's) */
	{
		uint64_t total = 0;

		for (index = 0; index < plan->triangle_count && plan->lamp_count; index++)
		{
			float n[3], area;

			face_normal(plan->positions + (size_t)index * 9, n, &area);
			if (area >= 1.0e-6f)
				total += grid_points_of(area);
		}
		table.capacity = 1u << 16;
		while (table.capacity < total * 2u && table.capacity < (1u << 24))
			table.capacity <<= 1;
	}
	table.keys = (uint64_t *)calloc(table.capacity, sizeof(uint64_t));
	table.cells = (uint32_t *)calloc(table.capacity, sizeof(uint32_t));
	plan->cell_keys = (int32_t *)malloc((size_t)table.capacity / 2 * 3 * sizeof(int32_t));
	plan->cell_points = (float *)calloc((size_t)table.capacity / 2 * GRID_POINTS * 4, sizeof(float));
	if (!table.keys || !table.cells || !plan->cell_keys || !plan->cell_points)
		goto done;
	{
		uint32_t *seen = (uint32_t *)calloc(table.capacity / 2, sizeof(uint32_t));

		if (!seen)
			goto done;
		for (index = 0; index < plan->triangle_count && plan->lamp_count; index++)
		{
			const float *p = plan->positions + (size_t)index * 9;
			float n[3], area;
			uint32_t points, k, packed;

			face_normal(p, n, &area);
			if (area < 1.0e-6f)
				continue;
			packed = octahedral(n);
			points = grid_points_of(area);
			for (k = 0; k < points; k++)
			{
				float r1 = sqrtf(plan_random(&rng)), r2 = plan_random(&rng), q[3];
				int32_t c[3], axis;
				uint64_t key;
				uint32_t slot, cell;

				for (axis = 0; axis < 3; axis++)
				{
					q[axis] = (1.0f - r1) * p[axis] + r1 * (1.0f - r2) * p[3 + axis] + r1 * r2 * p[6 + axis];
					c[axis] = (int32_t)floorf(q[axis] / GRID_CELL);
				}
				key = cell_key(c[0], c[1], c[2]);
				slot = hash32((uint32_t)key ^ hash32((uint32_t)(key >> 32))) & (table.capacity - 1);
				while (table.keys[slot] && table.keys[slot] != key)
					slot = (slot + 1) & (table.capacity - 1);
				if (!table.keys[slot])
				{
					if (table.count >= table.capacity / 2)
						continue;
					table.keys[slot] = key;
					table.cells[slot] = table.count;
					plan->cell_keys[table.count * 3] = c[0];
					plan->cell_keys[table.count * 3 + 1] = c[1];
					plan->cell_keys[table.count * 3 + 2] = c[2];
					table.count++;
				}
				cell = table.cells[slot];
				/* (a reservoir of the cell's points) */
				seen[cell]++;
				{
					uint32_t at = seen[cell] <= GRID_POINTS ? seen[cell] - 1 : plan_rng(&rng) % seen[cell];

					if (at < GRID_POINTS)
					{
						float *to = plan->cell_points + ((size_t)cell * GRID_POINTS + at) * 4;
						uint32_t bits = packed ? packed : 1u;

						to[0] = q[0];
						to[1] = q[1];
						to[2] = q[2];
						memcpy(&to[3], &bits, 4);
					}
				}
			}
		}
		free(seen);
	}
	plan->cell_count = table.count;
	/* each cell's candidates: the lamps whose light at its middle is most
	(power over the squared distance, a cell's width added), facing it,
	within GRID_REACH (a bucket grid of the lamps' middles) */
	plan->cell_candidates = (uint32_t *)malloc(((size_t)plan->cell_count + 1) * GRID_CANDIDATES * sizeof(uint32_t));
	if (!plan->cell_candidates)
		goto done;
	{
		float bucket = GRID_REACH * 0.5f, least[3] = { 1e30f, 1e30f, 1e30f }, most[3] = { -1e30f, -1e30f, -1e30f };
		int32_t dims[3], axis;
		uint32_t *heads = NULL, *next = NULL, l, buckets;

		for (l = 0; l < plan->lamp_count; l++)
			for (axis = 0; axis < 3; axis++)
			{
				const float *lp = plan->lamps + (size_t)l * 16;
				float m = (lp[axis] + lp[3 + axis] + lp[6 + axis]) / 3.0f;

				least[axis] = fminf(least[axis], m);
				most[axis] = fmaxf(most[axis], m);
			}
		for (axis = 0; axis < 3; axis++)
		{
			dims[axis] = plan->lamp_count ? (int32_t)((most[axis] - least[axis]) / bucket) + 1 : 1;
			if (dims[axis] > 512)
				dims[axis] = 512;
		}
		buckets = (uint32_t)(dims[0] * dims[1] * dims[2]);
		heads = (uint32_t *)malloc((buckets + 1) * sizeof(uint32_t));
		next = (uint32_t *)malloc((plan->lamp_count + 1) * sizeof(uint32_t));
		if (!heads || !next)
		{
			free(heads);
			free(next);
			goto done;
		}
		for (index = 0; index < buckets; index++)
			heads[index] = 0xffffffffu;
		for (l = 0; l < plan->lamp_count; l++)
		{
			const float *lp = plan->lamps + (size_t)l * 16;
			int32_t b[3];

			for (axis = 0; axis < 3; axis++)
			{
				b[axis] = (int32_t)(((lp[axis] + lp[3 + axis] + lp[6 + axis]) / 3.0f - least[axis]) / bucket);
				b[axis] = b[axis] < 0 ? 0 : b[axis] >= dims[axis] ? dims[axis] - 1 : b[axis];
			}
			index = (uint32_t)((b[2] * dims[1] + b[1]) * dims[0] + b[0]);
			next[l] = heads[index];
			heads[index] = l;
		}
		for (index = 0; index < plan->cell_count; index++)
		{
			float middle[3], weight[GRID_CANDIDATES];
			uint32_t kept = 0, k, *to = plan->cell_candidates + (size_t)index * GRID_CANDIDATES;
			int32_t b0[3], b1[3], bx, by, bz;

			for (axis = 0; axis < 3; axis++)
			{
				middle[axis] = ((float)plan->cell_keys[index * 3 + axis] + 0.5f) * GRID_CELL;
				b0[axis] = (int32_t)floorf((middle[axis] - GRID_REACH - least[axis]) / bucket);
				b1[axis] = (int32_t)floorf((middle[axis] + GRID_REACH - least[axis]) / bucket);
				b0[axis] = b0[axis] < 0 ? 0 : b0[axis];
				b1[axis] = b1[axis] >= dims[axis] ? dims[axis] - 1 : b1[axis];
			}
			for (bz = b0[2]; bz <= b1[2]; bz++)
				for (by = b0[1]; by <= b1[1]; by++)
					for (bx = b0[0]; bx <= b1[0]; bx++)
						for (l = heads[(bz * dims[1] + by) * dims[0] + bx]; l != 0xffffffffu; l = next[l])
						{
							const float *lp = plan->lamps + (size_t)l * 16;
							float c[3], ne[3], area, d2 = 0.0f, w, facing = -1.0f;
							int corner;

							face_normal(lp, ne, &area);
							for (axis = 0; axis < 3; axis++)
							{
								c[axis] = (lp[axis] + lp[3 + axis] + lp[6 + axis]) / 3.0f;
								d2 += (middle[axis] - c[axis]) * (middle[axis] - c[axis]);
							}
							if (d2 > GRID_REACH * GRID_REACH)
								continue;
							/* (one-sided: a corner of the cell in front of it) */
							for (corner = 0; corner < 8; corner++)
							{
								float f = 0.0f;

								for (axis = 0; axis < 3; axis++)
									f += ne[axis] * (middle[axis] + ((corner >> axis) & 1 ? 0.5f : -0.5f) * GRID_CELL - c[axis]);
								facing = fmaxf(facing, f);
							}
							if (facing <= 0.0f)
								continue;
							w = (0.2126f * lp[9] + 0.7152f * lp[10] + 0.0722f * lp[11]) * lp[12] / (d2 + GRID_CELL * GRID_CELL);
							if (!(w > 0.0f))
								continue;
							if (kept < GRID_CANDIDATES)
							{
								to[kept] = l;
								weight[kept++] = w;
							}
							else
							{
								uint32_t smallest = 0;

								for (k = 1; k < kept; k++)
									if (weight[k] < weight[smallest])
										smallest = k;
								if (w > weight[smallest])
								{
									to[smallest] = l;
									weight[smallest] = w;
								}
							}
						}
			for (k = kept; k < GRID_CANDIDATES; k++)
				to[k] = 0xffffffffu;
		}
		free(heads);
		free(next);
	}
	/* the probes: each brick's 64, of the bricks where the player and what
	he fights stand (the walk's floors, and the air two world units above
	them that a body or a camera fills): the camera's and flight's farther
	air is lit by the bake's lightmap where it meets the level, and the
	objects in it by CE's light, until the volume's coarser level */
	{
		uint32_t b, k, kept_bricks = 0, walk_capacity = 1024, *walk, *fine, fine_count = 0;

		while (walk_capacity < plan->brick_count * 2u + 2u)
			walk_capacity <<= 1;
		walk = (uint32_t *)calloc(walk_capacity, sizeof(uint32_t));
		fine = (uint32_t *)malloc(((size_t)plan->brick_count + 1) * sizeof(uint32_t));
		if (!fine || !walk)
		{
			free(walk);
			free(fine);
			goto done;
		}
		for (b = 0; b < plan->brick_count; b++)
		{
			int16_t const *brick = plan->bricks + b * 4;
			uint32_t key = brick_key(brick[0], brick[1], brick[2]) + 1u, h = hash32(key) & (walk_capacity - 1);

			if (!(brick[3] & XGPU_BAKE_BRICK_WALK))
				continue;
			while (walk[h] && walk[h] != key)
				h = (h + 1) & (walk_capacity - 1);
			walk[h] = key;
		}
		for (b = 0; b < plan->brick_count; b++)
		{
			int16_t const *brick = plan->bricks + b * 4;
			uint32_t below;
			int near = (brick[3] & XGPU_BAKE_BRICK_WALK) != 0;

			for (below = 1; below <= 2 && !near && (brick[3] & XGPU_BAKE_BRICK_CAMERA); below++)
			{
				uint32_t key = brick_key(brick[0], brick[1], brick[2] - (int32_t)below) + 1u;
				uint32_t h = hash32(key) & (walk_capacity - 1);

				while (walk[h] && walk[h] != key)
					h = (h + 1) & (walk_capacity - 1);
				near = walk[h] == key;
			}
			if (!near)
				continue;
			fine[fine_count++] = b;
		}
		free(walk);
		plan->fine_wanted = fine_count;
		plan->fine_dropped = 0;
		if (fine_count > VOLUME_FINE_BRICKS_MOST && !volume_budget(plan, fine, &fine_count))
		{
			free(fine);
			goto done;
		}
		plan->probes = (float *)malloc(((size_t)fine_count + 1) * BRICK_PROBES * 4 * sizeof(float));
		if (!plan->probes)
		{
			free(fine);
			goto done;
		}
		for (kept_bricks = 0; kept_bricks < fine_count; kept_bricks++)
		{
			int16_t const *brick;

			b = fine[kept_bricks];
			brick = plan->bricks + b * 4;
			for (k = 0; k < BRICK_PROBES; k++)
			{
				float *to = plan->probes + ((size_t)kept_bricks * BRICK_PROBES + k) * 4;

				to[0] = (float)brick[0] + ((float)(k & 3) + 0.5f) * PROBE_SPACING;
				to[1] = (float)brick[1] + ((float)((k >> 2) & 3) + 0.5f) * PROBE_SPACING;
				to[2] = (float)brick[2] + ((float)(k >> 4) + 0.5f) * PROBE_SPACING;
				memcpy(&to[3], &b, 4);
			}
		}
		free(fine);
		plan->probe_count = plan->fine_count = kept_bricks * BRICK_PROBES;
		/* (the coarse cascade: its bricks of COARSE_SPACING x 4 world units,
		each over the reach's bricks and the air COARSE_HEIGHT above its
		floors) */
		{
			uint32_t capacity = 1024, *seen;
			float *grown;

			while (capacity < plan->brick_count * 4u + 4u)
				capacity <<= 1;
			seen = (uint32_t *)calloc(capacity, sizeof(uint32_t));
			for (b = 0; seen && b < plan->brick_count; b++)
			{
				int16_t const *brick = plan->bricks + b * 4;
				int32_t span = (int32_t)(COARSE_SPACING * 4.0f), z;
				int32_t top = (brick[3] & XGPU_BAKE_BRICK_WALK) ? brick[2] + COARSE_HEIGHT : brick[2];

				for (z = brick[2]; z <= top; z += span / 2)
				{
					int32_t cx = (int32_t)floorf((float)brick[0] / (float)span), cy = (int32_t)floorf((float)brick[1] /
						(float)span), cz = (int32_t)floorf((float)z / (float)span);
					uint32_t key = brick_key(cx, cy, cz) + 1u, h = hash32(key) & (capacity - 1);

					while (seen[h] && seen[h] != key)
						h = (h + 1) & (capacity - 1);
					if (seen[h])
						continue;
					seen[h] = key;
					grown = (float *)realloc(plan->probes, ((size_t)plan->probe_count + BRICK_PROBES + 1) * 4 *
						sizeof(float));
					if (!grown)
						break;
					plan->probes = grown;
					for (k = 0; k < BRICK_PROBES; k++)
					{
						float *to = plan->probes + ((size_t)plan->probe_count + k) * 4;

						to[0] = (float)(cx * span) + ((float)(k & 3) + 0.5f) * COARSE_SPACING;
						to[1] = (float)(cy * span) + ((float)((k >> 2) & 3) + 0.5f) * COARSE_SPACING;
						to[2] = (float)(cz * span) + ((float)(k >> 4) + 0.5f) * COARSE_SPACING;
						memcpy(&to[3], &b, 4);
					}
					plan->probe_count += BRICK_PROBES;
				}
			}
			free(seen);
		}
		/* (the reflection probes: each REFLECTION_CELL cell of the walk's
		floors one, at its floors' middle, REFLECTION_HEIGHT above them) */
		{
			uint32_t capacity = 1024, *keys, *slots;
			double *sums;

			while (capacity < plan->brick_count * 2u + 2u)
				capacity <<= 1;
			keys = (uint32_t *)calloc(capacity, sizeof(uint32_t));
			slots = (uint32_t *)calloc(capacity, sizeof(uint32_t));
			sums = (double *)calloc((size_t)REFLECTIONS_MOST * 4, sizeof(double));
			plan->reflections = (float *)calloc((size_t)REFLECTIONS_MOST * 4 + 4, sizeof(float));
			for (b = 0; keys && slots && sums && plan->reflections && b < plan->brick_count; b++)
			{
				int16_t const *brick = plan->bricks + b * 4;
				int32_t c[3];
				uint32_t key, h;
				int axis;

				if (!(brick[3] & XGPU_BAKE_BRICK_WALK))
					continue;
				for (axis = 0; axis < 3; axis++)
					c[axis] = (int32_t)floorf(((float)brick[axis] + 0.5f) / REFLECTION_CELL);
				key = brick_key(c[0], c[1], c[2]) + 1u;
				h = hash32(key) & (capacity - 1);
				while (keys[h] && keys[h] != key)
					h = (h + 1) & (capacity - 1);
				if (!keys[h])
				{
					if (plan->reflection_count >= REFLECTIONS_MOST)
						continue;
					keys[h] = key;
					slots[h] = plan->reflection_count++;
				}
				for (axis = 0; axis < 3; axis++)
					sums[slots[h] * 4 + axis] += (double)brick[axis] + 0.5;
				sums[slots[h] * 4 + 3] += 1.0;
			}
			for (b = 0; sums && plan->reflections && b < plan->reflection_count; b++)
			{
				int axis;

				for (axis = 0; axis < 3; axis++)
					plan->reflections[b * 4 + axis] = (float)(sums[b * 4 + axis] / sums[b * 4 + 3]);
				plan->reflections[b * 4 + 2] += REFLECTION_HEIGHT - 0.5f;
			}
			if (!plan->reflections)
				plan->reflection_count = 0;
			free(keys);
			free(slots);
			free(sums);
		}
	}
	plan->failed = 0;
	/* (HALO_BAKE_DUMP: each page's chart map and its triangles' UVs as
	images, for checking the atlas) */
	if (getenv("HALO_BAKE_DUMP"))
	{
		uint32_t page;

		for (page = 0; page < plan->page_count; page++)
		{
			char path[1024];
			FILE *file;
			uint32_t w = plan->pages[page * 2], h = plan->pages[page * 2 + 1], x, y;

			snprintf(path, sizeof(path), "%s/page%02u.pgm", getenv("HALO_BAKE_DUMP"), page);
			file = fopen(path, "wb");
			if (!file)
				continue;
			fprintf(file, "P5 %u %u 255\n", w, h);
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
				{
					uint16_t v = plan->charts[plan->chart_offsets[page] + (size_t)y * w + x];

					fputc(v ? 64 + (int)(v * 37u % 192u) : 0, file);
				}
			fclose(file);
		}
	}

done:
	free(pages);
	free(triangles);
	if (plan->failed)
		free(atlas_scene);
	else
		plan->atlas_scene = atlas_scene;
	free(counts);
	free(best);
	free(table.keys);
	free(table.cells);
	plan->seconds = now_seconds() - started;
	__atomic_store_n(&plan->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void plan_free(struct bake_plan *plan)
{
	free(plan->positions);
	free(plan->attributes);
	free(plan->tiers);
	free(plan->pages);
	free(plan->lamps);
	free(plan->bricks);
	prism_bake_atlas_free(&plan->atlas);
	free(plan->vpages);
	free(plan->tiles);
	free(plan->tile_triangles);
	free(plan->atlas_scene);
	free(plan->charts);
	free(plan->chart_offsets);
	free(plan->cell_keys);
	free(plan->cell_points);
	free(plan->cell_candidates);
	free(plan->probes);
	free(plan->reflections);
	memset(plan, 0, sizeof(*plan));
}

/* ---------- a BSP's bake begun */

static void status_set(int word, uint32_t value);
static void status_real(int word, float value);
static void reach_wait(void);
static void filter_stop(struct bake_state *b);

static void folder_make(const char *path)
{
	char partial[XGPU_BAKE_PATH + 160];
	size_t index, length = strlen(path);

	if (length >= sizeof(partial))
		return;
	for (index = 1; index <= length; index++)
	{
		/* (Windows's folders end at either slash) */
		if (path[index] == '/' || path[index] == '\\' || path[index] == 0)
		{
			memcpy(partial, path, index);
			partial[index] = 0;
			mkdir(partial, 0755);
		}
	}
}

/* the map's name as a file's: letters, digits and - (the rest _) */
static void name_clean(const char *from, char *to, size_t size)
{
	size_t index;

	for (index = 0; from[index] && index + 1 < size; index++)
	{
		char c = from[index];

		to[index] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
	}
	to[index] = 0;
	if (!to[0] && size > 4)
		strcpy(to, "map");
}

static void bake_free(struct bake_state *b)
{
	uint32_t index;
	struct bake_buffer *buffers[] =
	{
		&b->vertices, &b->info, &b->materials, &b->alpha, &b->lamps, &b->grid, &b->scene, &b->blas_storage[0],
		&b->blas_storage[1], &b->blas_storage[2], &b->blas_storage[3], &b->fixtures_buffer, &b->tlas_storage, &b->instances, &b->pages, &b->charts, &b->vpages, &b->table, &b->pool,
		&b->pool_indirect, &b->lightmaps, &b->sun, &b->masks, &b->bricks, &b->bricks_coarse, &b->reflection_points, &b->reflection_info,
		&b->reflection_data, &b->reflection_table, &b->probes, &b->raster_job, &b->ranges, &b->triangles, &b->texels,
		&b->accumulators, &b->states, &b->running, &b->tile_origins, &b->job, &b->cell_points, &b->cell_candidates,
		&b->cell_weights, &b->mask_ranges, &b->mask_out, &b->probe_points, &b->probe_accumulators, &b->probe_states,
		&b->probe_statistics, &b->compact_list, &b->compact_args, &b->plan_atlas, &b->cache_block,
		&b->runtime_blas_storage[0], &b->runtime_blas_storage[1], &b->runtime_tlas_storage, &b->runtime_instances,
		&b->scene_runtime,
	};

	if (!b)
		return;
	if (b->thread_running)
		pthread_join(b->thread, NULL);
	filter_stop(b);
	for (index = 0; index < sizeof(buffers) / sizeof(buffers[0]); index++)
		if (buffers[index]->buffer)
			garbage_add(buffers[index], VK_NULL_HANDLE);
	for (index = 0; index < 4; index++)
		if (b->blas[index])
			garbage_add(NULL, b->blas[index]);
	for (index = 0; index < 2; index++)
		if (b->runtime_blas[index])
			garbage_add(NULL, b->runtime_blas[index]);
	if (b->runtime_tlas)
		garbage_add(NULL, b->runtime_tlas);
	free(b->scenery);
	free(b->scenery_replaced);
	free(b->scenery_uv);
	free(b->scenery_map);
	free(b->scenery_maps);
	free(b->fixtures);
	if (b->tlas)
		garbage_add(NULL, b->tlas);
	if (b->writing)
		prism_bake_cache_writer_close(&b->writer);
	prism_bake_cache_close(&b->cache);
	if (b->relight_open)
		prism_bake_cache_close(&b->relight_reader);
	plan_free(&b->plan);
	free(b->table_host);
	free(b->sun_host);
	free(b->weights_host);
	free(b->mask_tiles);
	free(b->probe_host);
	free(b);
}

static void bake_fail(struct bake_state *b, const char *why)
{
	host_logf(HOST_LOG_WARN, "prism bake: %s bsp %d: %s", b->map, (int)b->bsp, why);
	if (b->step != STEP_FAILED)
		b->failed_at = b->step;
	b->step = STEP_FAILED;
	status_set(XGPU_BAKE_STATUS_STATE, XGPU_BAKE_STATE_FAILED);
}

/* whether the bake's scene is on the GPU to be traced and looked up: past
its upload, or failed past it (port: a failure after the upload, a card
shared with the desktop out of memory for the importance grid's update, the
sun mask, the volume or the probes, left the frame Classic's for good with a
whole bake loaded (the demo's build of 4692c0483 under its launcher, "no bake's
scene to trace yet" after "the bake is whole"); what it had made by then is
traced, as a bake still being made is, its missing parts the shaders'
fallbacks) */
static int bake_traceable(const struct bake_state *b)
{
	return b && b->scene.address && (b->step == STEP_FAILED ? b->failed_at > STEP_UPLOAD : b->step > STEP_UPLOAD);
}

/* ---------- the layered bake's light (prism_bake_layers.h) */

/* the largest emission of the lamps' materials (the light cards and lamp
surfaces), 1 where there are none */
static float emission_most(const float *materials, uint32_t count)
{
	float most = 0.0f;
	uint32_t index;
	int k;

	for (index = 0; materials && index < count; index++)
	{
		const float *m = materials + (size_t)index * XGPU_STREAM_RT_MATERIAL_FLOATS;

		if (!((int)m[XGPU_RT_MATERIAL_FLAGS] & XGPU_RT_MATERIAL_FLAG_LIGHT))
			continue;
		for (k = 0; k < 3; k++)
			if (m[XGPU_RT_MATERIAL_EMISSION + k] > most && m[XGPU_RT_MATERIAL_EMISSION + k] < 1.0e9f)
				most = m[XGPU_RT_MATERIAL_EMISSION + k];
	}
	return most > 0.0f ? most : 1.0f;
}

/* an emission as a share of the largest, to 1/65536 (so that a scale of all
the lamps hashes the same) */
static float emission_share(float emission, float most)
{
	return floorf(emission / most * 65536.0f + 0.5f);
}

/* the light's colours and powers in a rig (RT_FRAME's, or the bake's) */
static void layer_light_of(const float *rig, const float *materials, uint32_t material_count,
	struct prism_bake_layer_light *light)
{
	int k, dome;

	for (k = 0; k < 3; k++)
	{
		light->sun[k] = rig[XGPU_RT_RIG_SUN + k];
		light->sky[k] = rig[XGPU_RT_RIG_SKY + k];
		light->domes[k] = 0.0f;
		for (dome = 0; dome < XGPU_RT_DOMES; dome++)
			light->domes[k] += rig[XGPU_RT_RIG_DOMES + dome * 8 + 4 + k];
		/* (Tool's flat fill: the sky's outdoor ambient, sky 0's indoor) */
		light->floor_outdoor[k] = rig[XGPU_RT_RIG_SKY + k];
		light->floor_indoor[k] = rig[57 + k];
	}
	light->fixtures = rig[60];
	light->emission = emission_most(materials, material_count);
	light->emissive_scale = rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EMISSIVE_SCALE];
	/* (rig[61]: 1 the units rig with Tool's fill, 2 with the sky a light, 0
	the fitted rig, whose sky is a light too) */
	light->flat = rig[61] > 0.5f && rig[61] < 1.5f;
}

/* the materials' hash, their emission as shares of the largest */
static uint64_t materials_hash(const float *materials, uint32_t count, float most)
{
	uint64_t hash = PRISM_BAKE_HASH64_SEED;
	float copy[XGPU_STREAM_RT_MATERIAL_FLOATS];
	uint32_t index;
	int k;

	for (index = 0; index < count; index++)
	{
		memcpy(copy, materials + (size_t)index * XGPU_STREAM_RT_MATERIAL_FLOATS, sizeof(copy));
		for (k = 0; k < 3; k++)
			copy[XGPU_RT_MATERIAL_EMISSION + k] = emission_share(copy[XGPU_RT_MATERIAL_EMISSION + k], most);
		hash = prism_bake_hash64(hash, copy, sizeof(copy));
	}
	return hash;
}

/* the light's transport (the key's light hash, the layered bake's) */
static uint64_t light_transport_hash(const float block_rig[16][4], const struct xgpu_stream_rt_frame *frame,
	const float *lamps, uint32_t lamp_count, float most)
{
	const float *rig = (const float *)block_rig;
	uint64_t hash = PRISM_BAKE_HASH64_SEED;
	float words[16];
	uint32_t index;
	int dome, k;

	/* (the sun: its direction, whether there is one, its disc, whether it
	gives light; the sky's: whether it gives light) */
	memcpy(words, frame->sun, 3 * sizeof(float));
	words[3] = frame->sun[3] > 0.0f ? 1.0f : 0.0f;
	words[4] = rig[XGPU_RT_RIG_SUN + 3];
	words[5] = rig[XGPU_RT_RIG_SUN] + rig[XGPU_RT_RIG_SUN + 1] + rig[XGPU_RT_RIG_SUN + 2] > 0.0f ? 1.0f : 0.0f;
	words[6] = rig[XGPU_RT_RIG_SKY] + rig[XGPU_RT_RIG_SKY + 1] + rig[XGPU_RT_RIG_SKY + 2] > 0.0f ? 1.0f : 0.0f;
	/* (the albedo's rule, the sun's test distance, the units or fitted rig) */
	words[7] = frame->rig[62];
	words[8] = frame->rig[63];
	words[9] = frame->rig[61] > 0.5f ? 1.0f : 0.0f;
	hash = prism_bake_hash64(hash, words, 10 * sizeof(float));
	for (dome = 0; dome < XGPU_RT_DOMES; dome++)
	{
		const float *d = rig + XGPU_RT_RIG_DOMES + dome * 8;

		memcpy(words, d, 4 * sizeof(float));
		words[4] = d[4] + d[5] + d[6] > 0.0f ? 1.0f : 0.0f;
		words[5] = d[7];
		hash = prism_bake_hash64(hash, words, 6 * sizeof(float));
	}
	for (index = 0; index < lamp_count; index++)
	{
		const float *l = lamps + (size_t)index * 16;

		memcpy(words, l, 9 * sizeof(float));
		for (k = 0; k < 3; k++)
			words[9 + k] = emission_share(l[9 + k], most);
		words[12] = l[12];
		hash = prism_bake_hash64(hash, words, 13 * sizeof(float));
	}
	return hash;
}

/* the key's settings (the reach's bricks, the `max` tier's noise floor and
sample cap, HALO_BAKE_SEED, the noise's steps where a diagnostic changed
them) as a bake of this quality has them: the bake's own, or a better one's,
which is good for this bake too (a release's shipped MAX bake for an ultra
one) */
static uint64_t key_settings_hash(const struct bake_state *b, uint32_t quality)
{
	struct prism_bake_filter_parameters standard;
	float noise_floor = quality == PRISM_BAKE_QUALITY_MAX && bake.control.noise_floor > 0.0f &&
		bake.control.noise_floor <= 10.0f ? bake.control.noise_floor * 0.18f : 0.0f;
	uint64_t hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, bake.bricks, (size_t)bake.brick_count * 8);
	int passes;

	hash = prism_bake_hash64(hash, &noise_floor, sizeof(noise_floor));
	if (quality == PRISM_BAKE_QUALITY_MAX)
	{
		/* (`max`'s cap: a bake begun with no cap is not resumed under one) */
		float cap = MAX_SAMPLES_CAP;

		hash = prism_bake_hash64(hash, &cap, sizeof(cap));
	}
	if (b->seed)
		hash = prism_bake_hash64(hash, &b->seed, sizeof(b->seed));
	prism_bake_filter_defaults(&standard, (int)quality);
	passes = b->filter_asked >= 0 ? b->filter_asked : standard.passes;
	if (!b->robust_on)
		hash = prism_bake_hash64(hash, "no robust", 9);
	if (passes != standard.passes)
		hash = prism_bake_hash64(hash, &passes, sizeof(passes));
	return hash;
}

static void file_choose(struct bake_state *b);

/* the bake's inputs taken (the render thread's: the scene, the materials,
the rig, the reach), its key made and its plan begun on its thread: NULL
where they are not all here yet */
static struct bake_state *bake_begin(void)
{
	const struct xgpu_scene_static_set *set = xgpu_vulkan_scene_static_set();
	const struct xgpu_stream_rt_frame *frame;
	const float *materials;
	const uint32_t *alpha;
	uint32_t generation, material_count, alpha_words, index, kept = 0, lamps = 0, indoor = 0;
	struct bake_state *b;
	uint64_t hash;
	float power = 0.0f;
	char name[XGPU_BAKE_NAME];
	int alpha_traced;
	double entered = now_seconds();

	if (!set || set->generation != bake.control.generation || !set->triangle_count)
		return NULL;
	if (!xgpu_vulkan_rt_scene(&generation, &materials, &material_count, &alpha, &alpha_words, &frame) ||
		generation != set->generation || !frame || !materials)
		return NULL;
	if (__atomic_load_n(&bake.reach_whole, __ATOMIC_ACQUIRE) != 1 || bake.reach_generation != set->generation)
		return NULL;
	reach_wait();
	b = (struct bake_state *)calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	prism_bake_layer_weights_identity(&b->weights);
	b->generation = set->generation;
	b->bsp = bake.control.bsp_index;
	b->quality = bake.control.quality > PRISM_BAKE_QUALITY_MAX ? PRISM_BAKE_QUALITY_MAX : bake.control.quality;
	b->request = bake.control.request;
	memcpy(b->map, bake.control.map_name, sizeof(b->map));
	b->map[sizeof(b->map) - 1] = 0;
	{
		const char *folder = bake.control.folder ? (const char *)host_pointer(bake.control.folder) : NULL;

		snprintf(b->folder, sizeof(b->folder), "%s", folder && *folder ? folder : "prism_bake");
	}
	/* the fixtures: the scenario's light fixtures in the BSP as placed (CE's
	radiosity's point and spot lights, every one whether its device has
	power or not), at a scale of 1 (the layered bake: the rig's fixtures'
	scale is the fixtures layer's weight at load, 0 where the rig is not in
	units); one of no light left out */
	if (bake.fixtures_input_count)
	{
		uint32_t k;

		b->fixtures = (float *)calloc((size_t)bake.fixtures_input_count + 1, 16 * sizeof(float));
		for (k = 0; b->fixtures && k < bake.fixtures_input_count && b->fixture_count < FIXTURES_MOST; k++)
		{
			const float *f = bake.fixtures_input + (size_t)k * FIXTURE_FLOATS;
			float *to = b->fixtures + (size_t)b->fixture_count * 16, cutoff = f[11], falloff = f[10];
			int channel;

			b->fixtures_seen++;
			if (!(f[9] > 0.0f) || !(f[9] < 1.0e6f))
				continue;
			memset(to, 0, 64);
			memcpy(to, f, 3 * sizeof(float));
			for (channel = 0; channel < 3; channel++)
				to[4 + channel] = (f[6 + channel] < 0.0f ? 0.0f : f[6 + channel] > 1.0f ? 1.0f : f[6 + channel]) * f[9];
			/* (a spot where its cutoff is a cone's: its axis and cones' cosines) */
			if (cutoff > 0.0f && cutoff < 3.14159265f)
			{
				memcpy(to + 8, f + 3, 3 * sizeof(float));
				to[11] = cosf(cutoff);
				to[12] = falloff > 0.0f && falloff < cutoff ? cosf(falloff) : to[11];
			}
			else
				to[11] = to[12] = 2.0f;
			if (to[4] + to[5] + to[6] > 0.0f)
				b->fixture_count++;
		}
	}
	alpha_traced = alpha && alpha_words > 4;
	/* the triangles kept: the opaque and alpha-tested, of the near level */
	for (index = 0; index < set->triangle_count; index++)
	{
		const uint32_t *a = set->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
		uint32_t kind = a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & 0xffu;
		uint32_t level = (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_LEVEL_SHIFT) & 3u;
		uint32_t traced = a[XGPU_SCENE_ATTRIBUTE_TRACED];

		if (level == XGPU_SCENE_LEVEL_FAR || traced == XGPU_SCENE_TRACED_NONE)
			continue;
		if (traced < material_count && ((int)materials[(size_t)traced * XGPU_STREAM_RT_MATERIAL_FLOATS +
			XGPU_RT_MATERIAL_FLAGS] & XGPU_RT_MATERIAL_FLAG_LIGHT))
			lamps++;
		if (kind == XGPU_RT_TRIANGLE_OPAQUE || kind == XGPU_RT_TRIANGLE_ALPHA_TESTED)
			kept++;
	}
	b->plan.triangle_count = kept;
	b->plan.positions = (float *)malloc(((size_t)kept + 1) * 9 * sizeof(float));
	b->plan.attributes = (uint32_t *)malloc(((size_t)kept + 1) * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * 4);
	b->plan.tiers = (uint8_t *)malloc((size_t)kept + 1);
	b->plan.pages = (uint32_t *)malloc(((size_t)set->page_count + 1) * 8);
	b->plan.lamps = (float *)malloc(((size_t)lamps + 1) * 16 * sizeof(float));
	b->plan.bricks = (int16_t *)malloc(((size_t)bake.brick_count + 1) * 8);
	if (!b->plan.positions || !b->plan.attributes || !b->plan.tiers || !b->plan.pages || !b->plan.lamps ||
		!b->plan.bricks)
	{
		bake_free(b);
		return NULL;
	}
	kept = 0;
	lamps = 0;
	indoor = 0;
	for (index = 0; index < set->triangle_count; index++)
	{
		const uint32_t *a = set->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
		const float *p = set->positions + (size_t)index * 9;
		uint32_t kind = a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & 0xffu;
		uint32_t level = (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_LEVEL_SHIFT) & 3u;
		uint32_t traced = a[XGPU_SCENE_ATTRIBUTE_TRACED], surface = a[XGPU_SCENE_ATTRIBUTE_SURFACE];
		const float *material = traced < material_count ? materials + (size_t)traced * XGPU_STREAM_RT_MATERIAL_FLOATS :
			NULL;

		/* (the level the path tracer traces: its near level, its traced
		surfaces) */
		if (level == XGPU_SCENE_LEVEL_FAR || traced == XGPU_SCENE_TRACED_NONE)
			continue;
		if (material && ((int)material[XGPU_RT_MATERIAL_FLAGS] & XGPU_RT_MATERIAL_FLAG_LIGHT))
		{
			float n[3], area, luminance;
			float *to = b->plan.lamps + (size_t)lamps * 16;

			face_normal(p, n, &area);
			luminance = 0.2126f * material[XGPU_RT_MATERIAL_EMISSION] + 0.7152f * material[XGPU_RT_MATERIAL_EMISSION + 1] +
				0.0722f * material[XGPU_RT_MATERIAL_EMISSION + 2];
			if (area > 1.0e-7f && luminance > 0.0f)
			{
				memcpy(to, p, 36);
				memcpy(to + 9, material + XGPU_RT_MATERIAL_EMISSION, 12);
				to[12] = area;
				power += luminance * area;
				to[13] = power;
				to[14] = to[15] = 0.0f;
				lamps++;
			}
		}
		if (kind != XGPU_RT_TRIANGLE_OPAQUE && kind != XGPU_RT_TRIANGLE_ALPHA_TESTED)
			continue;
		memcpy(b->plan.positions + (size_t)kept * 9, p, 36);
		memcpy(b->plan.attributes + (size_t)kept * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS, a,
			XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * 4);
		indoor += (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & XGPU_SCENE_INDOOR) != 0;
		b->plan.tiers[kept] = bake.tiers && surface < bake.tier_count ? bake.tiers[surface] : 0;
		kept++;
	}
	b->plan.lamp_count = lamps;
	b->lamp_count = lamps;
	b->lamp_power = power;
	b->plan.page_count = set->page_count;
	memcpy(b->plan.pages, set->pages, (size_t)set->page_count * 8);
	memcpy(b->plan.bricks, bake.bricks, (size_t)bake.brick_count * 8);
	b->plan.brick_count = bake.brick_count;
	b->plan.quality = (int)b->quality;
	/* the rig (the frame's: the sun, the sky, the domes, the lamps' scale) */
	memcpy(b->block.rig, frame->rig, sizeof(b->block.rig));
	/* (the map's light, not the path tracer's look: the sun's disc
	display.prism_sun_size "auto"'s (RT_FRAME's spare[2]), the lamps
	display.prism_rt_emissive "lights"' (the light cards and lamp surfaces,
	no glows: Tool's radiosity's), at display.prism_rt_emissive_scale 1, the
	albedo display.prism_rt_materials "average"'s, whatever those settings
	are, so that a bake is the same in every session of the same light
	settings) */
	{
		float *rig = (float *)b->block.rig;

		if (frame->spare[2] > 0.0f && frame->spare[2] < 3.0f)
			rig[XGPU_RT_RIG_SUN + 3] = frame->spare[2];
		rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EMISSIVE] = 1.0f;
		rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EMISSIVE_SCALE] = 1.0f;
		rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_MATERIALS] = 1.0f;
		/* (the layered bake: the sky always a light (rig[61] 2), its layer
		weighed by the rig at load, Tool's flat fill (display.prism_sky_light
		"flat") added there; the fixtures at 1 (rig[60])) */
		rig[61] = 2.0f;
		rig[60] = 1.0f;
		/* (its base: the light it is made with) */
		memset(&b->base, 0, sizeof(b->base));
		layer_light_of(rig, materials, material_count, &b->base);
		b->base.fixtures = 1.0f;
		b->base.emissive_scale = 1.0f;
		b->base.stock = 1;
		b->base_known = 1;
	}
	/* (D4 amended: the `max` tier's noise floor, a share of middle grey; the
	converge rule's error floor, rig[15].z) */
	b->noise_floor = b->quality == PRISM_BAKE_QUALITY_MAX && bake.control.noise_floor > 0.0f &&
		bake.control.noise_floor <= 10.0f ? bake.control.noise_floor * 0.18f : 0.0f;
	b->block.rig[15][2] = b->noise_floor;
	/* (HALO_BAKE_SEED: the samples from another seed, a diagnostic: the
	scene block's spare word, mixed into each texel's and probe's seed; 0 as
	before. rig[15].w is the sun's test distance) */
	{
		const char *seed_text = getenv("HALO_BAKE_SEED");
		uint32_t seed = seed_text ? (uint32_t)strtoul(seed_text, NULL, 10) : 0u;

		b->block.unused = seed;
		b->seed = seed;
	}
	/* (the noise's steps, prism_bake_filter.h: the robust mean, and the
	filter by the quality; HALO_BAKE_NO_ROBUST and HALO_BAKE_NO_FILTER leave
	them out, HALO_BAKE_FILTER_PASSES sets the passes: diagnostics) */
	prism_bake_robust_defaults(&b->robust);
	b->robust_on = getenv("HALO_BAKE_NO_ROBUST") == NULL;
	prism_bake_filter_defaults(&b->filter, (int)b->quality);
	if (getenv("HALO_BAKE_FILTER_PASSES"))
		b->filter.passes = atoi(getenv("HALO_BAKE_FILTER_PASSES"));
	if (getenv("HALO_BAKE_NO_FILTER"))
		b->filter.passes = 0;
	b->filter.passes = b->filter.passes < 0 ? 0 : b->filter.passes > PRISM_BAKE_FILTER_PASSES_MOST ?
		PRISM_BAKE_FILTER_PASSES_MOST : b->filter.passes;
	memcpy(b->block.sun, frame->sun, sizeof(b->block.sun));
	b->material_count = material_count;
	/* the key */
	memset(&b->key, 0, sizeof(b->key));
	b->key.map_checksum = bake.control.map_checksum;
	snprintf(b->key.map_name, sizeof(b->key.map_name), "%s", b->map);
	b->key.bsp_index = b->bsp;
	snprintf(b->key.bsp_name, sizeof(b->key.bsp_name), "bsp%d", (int)b->bsp);
	hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, b->plan.positions, (size_t)kept * 36);
	hash = prism_bake_hash64(hash, b->plan.attributes, (size_t)kept * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS * 4);
	b->key.bsp_hash = hash;
	b->key.geometry_hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, b->plan.tiers, kept);
	/* (the static scenery the bake traces with the level: its triangles,
	albedo and coverage in the geometry's hash) */
	if (bake.scenery_count && (b->scenery = (float *)malloc((size_t)bake.scenery_count * SCENERY_FLOATS * 4)) != NULL)
	{
		memcpy(b->scenery, bake.scenery, (size_t)bake.scenery_count * SCENERY_FLOATS * 4);
		b->scenery_count = bake.scenery_count;
		b->key.geometry_hash = prism_bake_hash64(b->key.geometry_hash, b->scenery,
			(size_t)b->scenery_count * SCENERY_FLOATS * 4);
		/* (the runs a mod's mesh draws: not in the hash, the frame's alone) */
		if (bake.scenery_replaced_count &&
			(b->scenery_replaced = (int32_t *)malloc((size_t)bake.scenery_replaced_count * 8)) != NULL)
		{
			memcpy(b->scenery_replaced, bake.scenery_replaced, (size_t)bake.scenery_replaced_count * 8);
			b->scenery_replaced_count = bake.scenery_replaced_count;
		}
		/* (the cutouts' coordinates and alpha maps: in the geometry's hash,
		so that a bake whose cards blocked by their coverage alone is made
		again) */
		if (bake.scenery_uv && bake.scenery_map && bake.scenery_maps && bake.scenery_map_words &&
			(b->scenery_uv = (float *)malloc((size_t)b->scenery_count * 24)) != NULL &&
			(b->scenery_map = (int32_t *)malloc((size_t)b->scenery_count * 4)) != NULL &&
			(b->scenery_maps = (uint32_t *)malloc((size_t)bake.scenery_map_words * 4)) != NULL)
		{
			memcpy(b->scenery_uv, bake.scenery_uv, (size_t)b->scenery_count * 24);
			memcpy(b->scenery_map, bake.scenery_map, (size_t)b->scenery_count * 4);
			memcpy(b->scenery_maps, bake.scenery_maps, (size_t)bake.scenery_map_words * 4);
			b->scenery_map_count = bake.scenery_map_count;
			b->scenery_map_words = bake.scenery_map_words;
			b->key.geometry_hash = prism_bake_hash64(b->key.geometry_hash, b->scenery_uv, (size_t)b->scenery_count * 24);
			b->key.geometry_hash = prism_bake_hash64(b->key.geometry_hash, b->scenery_map, (size_t)b->scenery_count * 4);
			b->key.geometry_hash = prism_bake_hash64(b->key.geometry_hash, b->scenery_maps,
				(size_t)b->scenery_map_words * 4);
		}
		else
		{
			free(b->scenery_uv);
			free(b->scenery_map);
			b->scenery_uv = NULL;
			b->scenery_map = NULL;
		}
	}
	/* (the materials, their emission as a share of the level's largest: the
	lamps' overall power is the lamps layer's weight at load, not the key's) */
	hash = materials_hash(materials, material_count, b->base.emission);
	if (alpha_traced)
		hash = prism_bake_hash64(hash, alpha, (size_t)alpha_words * 4);
	b->key.material_hash = hash;
	/* (the light's transport alone, the layered bake's key: the sun's
	direction and disc, the domes' directions, cones and test distances, the
	sun's test distance, the albedo's rule, the units or the fitted rig,
	which kinds of light there are; the lamps' places and their emission's
	shares, the fixtures as placed. Not the lights' colours and powers, the
	sky's kind (display.prism_sky_light), the fixtures' scale or the lamps'
	brightness: those are the layers' weights at load) */
	hash = light_transport_hash(b->block.rig, frame, b->plan.lamps, lamps, b->base.emission);
	if (b->fixture_count)
		hash = prism_bake_hash64(hash, b->fixtures, (size_t)b->fixture_count * 16 * sizeof(float));
	b->key.light_hash = hash;
	b->filter_asked = getenv("HALO_BAKE_FILTER_PASSES") || getenv("HALO_BAKE_NO_FILTER") ? b->filter.passes : -1;
	b->key.settings_hash = key_settings_hash(b, b->quality);
	b->key.baker_version = BAKE_VERSION;
	b->key.quality = b->quality;
	b->key.convergence_threshold = bake.control.threshold > 0.0f ? bake.control.threshold : 0.01f;
	name_clean(b->map, name, sizeof(name));
	snprintf(b->path, sizeof(b->path), "%s/%s/bsp%d.pbk", b->folder, name, (int)b->bsp);
	snprintf(b->new_path, sizeof(b->new_path), "%s.new", b->path);
	{
		char folder[XGPU_BAKE_PATH + 160];

		snprintf(folder, sizeof(folder), "%s/%s", b->folder, name);
		folder_make(folder);
	}
	/* (a whole bake of this BSP: the shipped one, or this machine's; the
	plan at its quality) */
	file_choose(b);
	/* the materials and alpha maps copied for the GPU (the set's may go) */
	b->started = now_seconds();
	b->step = STEP_PLAN;
	b->materials.mapped = malloc((size_t)material_count * XGPU_STREAM_RT_MATERIAL_FLOATS * 4 + 16);
	b->alpha.mapped = malloc((size_t)alpha_words * 4 + 16);
	if (!b->materials.mapped || !b->alpha.mapped)
	{
		free(b->materials.mapped);
		free(b->alpha.mapped);
		b->materials.mapped = b->alpha.mapped = NULL;
		bake_free(b);
		return NULL;
	}
	memcpy(b->materials.mapped, materials, (size_t)material_count * XGPU_STREAM_RT_MATERIAL_FLOATS * 4);
	if (alpha_traced)
		memcpy(b->alpha.mapped, alpha, (size_t)alpha_words * 4);
	b->alpha.size = alpha_traced ? (VkDeviceSize)alpha_words * 4 : 0;
	if (pthread_create(&b->thread, NULL, plan_thread, &b->plan) == 0)
		b->thread_running = 1;
	else
		plan_thread(&b->plan);
	host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: begun (quality %u): %u triangles (%u in indoor clusters), %u lamps "
		"(power %.2f), %u fixtures (of %u: those of no light left out), %u triangles of static scenery, %u bricks; %s",
		b->map, (int)b->bsp, b->quality, kept, indoor, lamps, (double)power, b->fixture_count, b->fixtures_seen,
		b->scenery_count, bake.brick_count, b->path);
	if (getenv("HALO_RASTER_EXPOSURE_LOG"))
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: begun at %.3f s (%.3f s in its start)", b->map, (int)b->bsp,
			now_seconds(), now_seconds() - entered);
	host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the rig: sun %.5f %.5f %.5f toward %.5f %.5f %.5f (%s, disc %.4f), sky "
		"%.4f %.4f %.4f (%s), gamma %.2f, emissive scale %.3f", b->map, (int)b->bsp, (double)b->block.rig[0][0],
		(double)b->block.rig[0][1], (double)b->block.rig[0][2], (double)b->block.sun[0], (double)b->block.sun[1],
		(double)b->block.sun[2], b->block.sun[3] > 0.0f ? "present" : "none", (double)b->block.rig[0][3],
		(double)b->block.rig[1][0], (double)b->block.rig[1][1], (double)b->block.rig[1][2],
		b->block.rig[15][1] > 1.5f ? "a light" : b->block.rig[15][1] > 0.5f ? "a flat fill" : "the fitted rig's",
		(double)b->block.rig[1][3], (double)b->block.rig[11][0]);
	return b;
}

/* a bottom-level structure of count triangles at vertices' address (in
commands): 0 where it cannot be made */
static int structure_build(VkCommandBuffer commands, VkDeviceAddress vertices, uint32_t count, int opaque,
	struct bake_buffer *storage, VkAccelerationStructureKHR *made, VkDeviceAddress *address)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkAccelerationStructureCreateInfoKHR create;
	VkAccelerationStructureDeviceAddressInfoKHR where;
	struct bake_buffer scratch;

	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexData.deviceAddress = vertices;
	geometry.geometry.triangles.vertexStride = 12;
	geometry.geometry.triangles.maxVertex = count * 3 - 1;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(bake.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&count, &sizes);
	if (!buffer_make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0,
		storage))
		return 0;
	if (!buffer_make(sizes.buildScratchSize + bake.scratch_alignment, 0, 0, &scratch))
	{
		buffer_free_now(storage);
		return 0;
	}
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = storage->buffer;
	create.size = sizes.accelerationStructureSize;
	create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHR(bake.device.device, &create, NULL, made) != VK_SUCCESS)
	{
		buffer_free_now(storage);
		buffer_free_now(&scratch);
		return 0;
	}
	info.dstAccelerationStructure = *made;
	info.scratchData.deviceAddress = (scratch.address + bake.scratch_alignment - 1) & ~(bake.scratch_alignment - 1);
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
	garbage_add(&scratch, VK_NULL_HANDLE);
	memset(&where, 0, sizeof(where));
	where.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	where.accelerationStructure = *made;
	*address = vkGetAccelerationStructureDeviceAddressKHR(bake.device.device, &where);
	return 1;
}

/* the top-level structure of the four (the level's opaque: mask 1, custom
index 0; its alpha-tested: mask 2, custom index 1; the static scenery's
opaque: mask 4, custom index 2; its cutouts: mask 8, custom index 3), into
the buffers and structure given (the bake's, or the frame's) */
static int scene_structure_into(VkCommandBuffer commands, const VkDeviceAddress addresses[4],
	struct bake_buffer *instances_buffer, struct bake_buffer *storage, VkAccelerationStructureKHR *tlas,
	VkDeviceAddress *tlas_address)
{
	VkAccelerationStructureInstanceKHR instances[4];
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkAccelerationStructureCreateInfoKHR create;
	VkAccelerationStructureDeviceAddressInfoKHR where;
	struct bake_buffer scratch;
	uint32_t count = 0, index;

	memset(instances, 0, sizeof(instances));
	for (index = 0; index < 4; index++)
	{
		VkDeviceAddress address = addresses[index];

		if (!address)
			continue;
		instances[count].transform.matrix[0][0] = instances[count].transform.matrix[1][1] =
			instances[count].transform.matrix[2][2] = 1.0f;
		instances[count].instanceCustomIndex = index;
		instances[count].mask = 1u << index;
		instances[count].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
			(index == 0 || index == 2 ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
		instances[count].accelerationStructureReference = address;
		count++;
	}
	if (!buffer_filled(commands, instances, sizeof(instances), instances_buffer,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
		return 0;
	barrier(commands);
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = instances_buffer->address;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(bake.device.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
		&count, &sizes);
	if (!buffer_make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0,
		storage) || !buffer_make(sizes.buildScratchSize + bake.scratch_alignment, 0, 0, &scratch))
		return 0;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = storage->buffer;
	create.size = sizes.accelerationStructureSize;
	create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	if (vkCreateAccelerationStructureKHR(bake.device.device, &create, NULL, tlas) != VK_SUCCESS)
	{
		buffer_free_now(&scratch);
		return 0;
	}
	info.dstAccelerationStructure = *tlas;
	info.scratchData.deviceAddress = (scratch.address + bake.scratch_alignment - 1) & ~(bake.scratch_alignment - 1);
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	vkCmdBuildAccelerationStructuresKHR(commands, 1, &info, &ranges);
	garbage_add(&scratch, VK_NULL_HANDLE);
	memset(&where, 0, sizeof(where));
	where.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	where.accelerationStructure = *tlas;
	*tlas_address = vkGetAccelerationStructureDeviceAddressKHR(bake.device.device, &where);
	return 1;
}

static int scene_structure(VkCommandBuffer commands, struct bake_state *b, const VkDeviceAddress addresses[4])
{
	VkDeviceAddress address = 0;

	if (!scene_structure_into(commands, addresses, &b->instances, &b->tlas_storage, &b->tlas, &address))
		return 0;
	b->block.tlas = address;
	return 1;
}

/* the scene's block uploaded (its own, and the frame's: the same but for
its top level, where the frame traces another) */
static int scene_block_upload(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_scene_block runtime;

	if (!upload(commands, &b->scene, 0, &b->block, sizeof(b->block)))
		return 0;
	if (!b->scene_runtime.buffer)
		return 1;
	runtime = b->block;
	runtime.tlas = b->runtime_tlas_address;
	return upload(commands, &b->scene_runtime, 0, &runtime, sizeof(runtime));
}

/* the scene block the frame traces: the bake's, or its own where the static
scenery a mod's mesh draws is left out */
static VkDeviceAddress scene_runtime_address(const struct bake_state *b)
{
	return b->scene_runtime.buffer ? b->scene_runtime.address : b->scene.address;
}

/* the scene put on the GPU (the plan done): the triangles traced (the
opaque, then the alpha-tested, each its info), their structures, the
materials, alpha maps and lamps, the scene's block; the raster lookup's
tables; the batch's buffers */
/* HALO_BAKE_EMITTERS_PASS (a diagnostic): the level's emitting faces (a
material with a lamp's light) neither block nor reflect, as Tool's
radiosity's emitters "block nothing"; their light is the lamps' still */
static int emitter_passes(const float *materials, uint32_t traced, uint32_t material_count)
{
	static int pass = -1;

	if (pass < 0)
		pass = getenv("HALO_BAKE_EMITTERS_PASS") != NULL;
	return pass && materials && traced < material_count &&
		((int)materials[(size_t)traced * XGPU_STREAM_RT_MATERIAL_FLOATS + XGPU_RT_MATERIAL_FLAGS] &
		XGPU_RT_MATERIAL_FLAG_LIGHT);
}

static int bake_upload(struct bake_state *b)
{
	VkCommandBuffer commands = xgpu_vulkan_compute_commands();
	struct bake_plan *plan = &b->plan;
	const float *materials = (const float *)b->materials.mapped;
	float *vertices = NULL, *tridata = NULL;
	uint32_t *info = NULL, *pages = NULL, *vpages = NULL, *plan_atlas = NULL, index, opaque = 0, tested = 0, at[4];
	uint32_t triangles, scenery_materials = 0, gone[2];
	uint8_t *replaced = NULL;
	VkDeviceAddress opaque_address = 0, tested_address = 0, addresses[4] = { 0, 0, 0, 0 };
	void *host_materials = b->materials.mapped, *host_alpha = b->alpha.mapped;
	VkDeviceSize alpha_bytes = b->alpha.size, level_alpha_bytes = b->alpha.size;
	int ok = 0, k_ok;

	b->materials.mapped = b->alpha.mapped = NULL;
	b->alpha.size = 0;
	for (index = 0; index < plan->triangle_count; index++)
	{
		const uint32_t *a = plan->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
		uint32_t traced = a[XGPU_SCENE_ATTRIBUTE_TRACED];

		if (emitter_passes(materials, traced, b->material_count))
			continue;
		int tests = (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & 0xffu) == XGPU_RT_TRIANGLE_ALPHA_TESTED && alpha_bytes &&
			traced < b->material_count && ((int)materials[(size_t)traced * XGPU_STREAM_RT_MATERIAL_FLOATS +
			XGPU_RT_MATERIAL_FLAGS] & XGPU_RT_MATERIAL_FLAG_ALPHA_TESTED);

		if (tests)
			tested++;
		else
			opaque++;
	}
	b->opaque_count = opaque;
	b->alpha_count = tested;
	/* (the static scenery's after the level's: its opaque, then its cutouts,
	each its own material: its albedo, and its coverage in the alpha test's
	place; the cutouts two-sided, cards) */
	scenery_materials = 0;
	/* (each kind's triangles of placements a mod's mesh draws after the
	others: the frame's structures are built over the others, the first) */
	if (b->scenery_replaced_count && (replaced = (uint8_t *)calloc(b->scenery_count + 1, 1)) != NULL)
	{
		for (index = 0; index < b->scenery_replaced_count; index++)
			memset(replaced + b->scenery_replaced[index * 2], 1, (size_t)b->scenery_replaced[index * 2 + 1]);
	}
	for (index = 0; index < b->scenery_count; index++)
	{
		int cutout = b->scenery[(size_t)index * SCENERY_FLOATS + 12] < 0.999f;

		if (cutout)
			b->scenery_cutout++;
		else
			b->scenery_opaque++;
		if (!replaced || !replaced[index])
		{
			if (cutout)
				b->scenery_cutout_kept++;
			else
				b->scenery_opaque_kept++;
		}
	}
	triangles = plan->triangle_count + b->scenery_count;
	vertices = (float *)malloc(((size_t)triangles + 1) * 36);
	info = (uint32_t *)malloc(((size_t)triangles + 1) * 32);
	if (!vertices || !info)
		goto done;
	if (b->scenery_count)
	{
		float *grown = (float *)realloc(host_materials, ((size_t)b->material_count + SCENERY_MATERIALS + 1) *
			XGPU_STREAM_RT_MATERIAL_FLOATS * 4);
		/* (the cutouts' alpha maps after the level's: each map's offset in
		bytes, width and height; -1 where its words do not add up) */
		float map_place[256 * 3];
		uint32_t map, map_total = b->scenery_uv && b->scenery_map && b->scenery_maps ? b->scenery_map_count : 0;
		uint32_t mapped = 0;

		if (!grown)
			goto done;
		host_materials = grown;
		materials = grown;
		if (map_total > 256)
			map_total = 256;
		if (map_total)
		{
			uint32_t word = 0, texel_words = 0;
			unsigned char *alpha_grown;

			for (map = 0; map < map_total; map++)
			{
				uint32_t w, h, n;

				map_place[map * 3] = -1.0f;
				if (word + 2 > b->scenery_map_words)
					break;
				w = b->scenery_maps[word];
				h = b->scenery_maps[word + 1];
				n = w && h && w <= 4096 && h <= 4096 ? (w * h + 3) / 4 : 0;
				if (!n || word + 2 + n > b->scenery_map_words)
					break;
				map_place[map * 3] = (float)(alpha_bytes + (VkDeviceSize)texel_words * 4);
				map_place[map * 3 + 1] = (float)w;
				map_place[map * 3 + 2] = (float)h;
				texel_words += n;
				word += 2 + n;
			}
			for (; map < map_total; map++)
				map_place[map * 3] = -1.0f;
			alpha_grown = texel_words ? (unsigned char *)realloc(host_alpha, (size_t)alpha_bytes +
				(size_t)texel_words * 4 + 16) : NULL;
			if (alpha_grown)
			{
				host_alpha = alpha_grown;
				for (word = 0, map = 0; map < map_total && map_place[map * 3] >= 0.0f; map++)
				{
					uint32_t n = (b->scenery_maps[word] * b->scenery_maps[word + 1] + 3) / 4;

					memcpy(alpha_grown + (size_t)map_place[map * 3], b->scenery_maps + word + 2, (size_t)n * 4);
					word += 2 + n;
				}
				alpha_bytes += (VkDeviceSize)texel_words * 4;
			}
			else
				map_total = 0;
		}
		at[2] = opaque + tested;
		at[3] = opaque + tested + b->scenery_opaque;
		gone[0] = at[2] + b->scenery_opaque_kept;
		gone[1] = at[3] + b->scenery_cutout_kept;
		for (index = 0; index < b->scenery_count; index++)
		{
			const float *t = b->scenery + (size_t)index * SCENERY_FLOATS;
			int cutout = t[12] < 0.999f;
			uint32_t slot = replaced && replaced[index] ? gone[cutout]++ : at[cutout ? 3 : 2]++, m;
			uint32_t *to = info + (size_t)slot * 8;
			/* (a cutout's alpha map, where the reach sent one: its card
			tested texel by texel as the level's alpha-tested faces are, not
			kept by its coverage) */
			int32_t own = cutout && map_total ? b->scenery_map[index] : -1;
			float place = own >= 0 && (uint32_t)own < map_total ? map_place[own * 3] : -1.0f;

			/* (its material: one alike already made, else a new one) */
			for (m = 0; m < scenery_materials; m++)
			{
				const float *had = grown + ((size_t)b->material_count + m) * XGPU_STREAM_RT_MATERIAL_FLOATS;

				if (had[0] == t[9] && had[1] == t[10] && had[2] == t[11] && had[7] == t[12] &&
					(place >= 0.0f ? had[3] == (float)XGPU_RT_MATERIAL_FLAG_ALPHA_TESTED && had[8] == place :
					had[3] == 0.0f))
					break;
			}
			if (m == scenery_materials && scenery_materials < SCENERY_MATERIALS)
			{
				float *made = grown + ((size_t)b->material_count + m) * XGPU_STREAM_RT_MATERIAL_FLOATS;

				memset(made, 0, XGPU_STREAM_RT_MATERIAL_FLOATS * 4);
				made[0] = t[9];
				made[1] = t[10];
				made[2] = t[11];
				made[7] = t[12];
				if (place >= 0.0f)
				{
					made[3] = (float)XGPU_RT_MATERIAL_FLAG_ALPHA_TESTED;
					made[8] = place;
					made[9] = map_place[own * 3 + 1];
					made[10] = map_place[own * 3 + 2];
				}
				scenery_materials++;
			}
			if (m >= scenery_materials)
				m = 0;
			memcpy(vertices + (size_t)slot * 9, t, 36);
			memset(to, 0, 32);
			to[0] = (b->material_count + m) | (cutout ? 0x10000u : 0u);
			if (place >= 0.0f)
			{
				memcpy(to + 1, b->scenery_uv + (size_t)index * 6, 24);
				mapped++;
			}
			to[7] = 0xffffffffu;
		}
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the static scenery's cutouts: %u of %u triangles tested by "
			"their alpha maps (%u maps), the rest kept by their coverage", b->map, (int)b->bsp, mapped,
			b->scenery_cutout, map_total);
	}
	at[0] = 0;
	at[1] = opaque;
	for (index = 0; index < plan->triangle_count; index++)
	{
		const uint32_t *a = plan->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
		uint32_t traced = a[XGPU_SCENE_ATTRIBUTE_TRACED];

		if (emitter_passes(materials, traced, b->material_count))
			continue;
		uint32_t level = (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_LEVEL_SHIFT) & 3u;
		/* (the level's alpha maps alone decide, as when the triangles were
		counted: the scenery's are added after them) */
		int tests = (a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & 0xffu) == XGPU_RT_TRIANGLE_ALPHA_TESTED && level_alpha_bytes &&
			traced < b->material_count && ((int)materials[(size_t)traced * XGPU_STREAM_RT_MATERIAL_FLOATS +
			XGPU_RT_MATERIAL_FLAGS] & XGPU_RT_MATERIAL_FLAG_ALPHA_TESTED);
		uint32_t slot = at[tests ? 1 : 0]++;
		uint32_t *to = info + (size_t)slot * 8;

		memcpy(vertices + (size_t)slot * 9, plan->positions + (size_t)index * 9, 36);
		to[0] = (traced < b->material_count ? traced : b->material_count) | (level != XGPU_SCENE_LEVEL_BOTH ? 0x10000u : 0u) |
			((a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] & XGPU_SCENE_INDOOR) ? 0x20000u : 0u);
		memcpy(to + 1, a + XGPU_SCENE_ATTRIBUTE_BASE, 6 * sizeof(float));
		to[7] = index;
	}
	/* (PRISM Path Traced's: each kept triangle's lightmap page + 1 (0 none)
	and its corners' lightmap coordinates, 2 x 16 unorm each, so that a ray's
	hit on the level reads the bake there) */
	{
		uint32_t *lightmaps = (uint32_t *)calloc((size_t)plan->triangle_count + 1, 16);

		if (!lightmaps)
			goto done;
		for (index = 0; index < plan->triangle_count; index++)
		{
			const uint32_t *a = plan->attributes + (size_t)index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS;
			uint32_t page = a[XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_PAGE_SHIFT, k;

			if (page == XGPU_SCENE_PAGE_NONE || page >= plan->page_count)
				continue;
			lightmaps[index * 4] = page + 1;
			for (k = 0; k < 3; k++)
			{
				float u, v;

				memcpy(&u, a + XGPU_SCENE_ATTRIBUTE_ATLAS + k * 2, 4);
				memcpy(&v, a + XGPU_SCENE_ATTRIBUTE_ATLAS + k * 2 + 1, 4);
				u = u > 0.0f ? (u < 1.0f ? u : 1.0f) : 0.0f;
				v = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
				lightmaps[index * 4 + 1 + k] = (uint32_t)(u * 65535.0f + 0.5f) | ((uint32_t)(v * 65535.0f + 0.5f) << 16);
			}
		}
		k_ok = buffer_filled(commands, lightmaps, (VkDeviceSize)plan->triangle_count * 16 + 16, &b->lightmaps, 0);
		free(lightmaps);
		if (!k_ok)
			goto done;
	}
	if (!buffer_filled(commands, vertices, (VkDeviceSize)triangles * 36, &b->vertices,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) ||
		!buffer_filled(commands, info, (VkDeviceSize)triangles * 32, &b->info, 0) ||
		!buffer_filled(commands, host_materials, ((VkDeviceSize)b->material_count + scenery_materials) *
		XGPU_STREAM_RT_MATERIAL_FLOATS * 4 + 16, &b->materials, 0) ||
		!buffer_filled(commands, alpha_bytes ? host_alpha : NULL, alpha_bytes + 16, &b->alpha, 0) ||
		!buffer_filled(commands, plan->lamps, (VkDeviceSize)plan->lamp_count * 64 + 64, &b->lamps, 0) ||
		!buffer_filled(commands, b->fixtures, (VkDeviceSize)b->fixture_count * 64 + 64, &b->fixtures_buffer, 0))
		goto done;
	barrier(commands);
	if (opaque && !structure_build(commands, b->vertices.address, opaque, 1, &b->blas_storage[0], &b->blas[0],
		&opaque_address))
		goto done;
	if (tested && !structure_build(commands, b->vertices.address + (VkDeviceSize)opaque * 36, tested, 0,
		&b->blas_storage[1], &b->blas[1], &tested_address))
		goto done;
	if (b->scenery_opaque && !structure_build(commands, b->vertices.address + (VkDeviceSize)(opaque + tested) * 36,
		b->scenery_opaque, 1, &b->blas_storage[2], &b->blas[2], &addresses[2]))
		goto done;
	if (b->scenery_cutout && !structure_build(commands, b->vertices.address + (VkDeviceSize)(opaque + tested +
		b->scenery_opaque) * 36, b->scenery_cutout, 0, &b->blas_storage[3], &b->blas[3], &addresses[3]))
		goto done;
	barrier(commands);
	addresses[0] = opaque_address;
	addresses[1] = tested_address;
	if (!scene_structure(commands, b, addresses))
		goto done;
	/* (the frame's: the level's and the static scenery's but for the
	placements a mod's mesh draws, the first of each kind's triangles) */
	if (b->scenery_opaque_kept != b->scenery_opaque || b->scenery_cutout_kept != b->scenery_cutout)
	{
		VkDeviceAddress runtime[4];

		runtime[0] = opaque_address;
		runtime[1] = tested_address;
		runtime[2] = runtime[3] = 0;
		if (b->scenery_opaque_kept && !structure_build(commands, b->vertices.address + (VkDeviceSize)(opaque + tested) *
			36, b->scenery_opaque_kept, 1, &b->runtime_blas_storage[0], &b->runtime_blas[0], &runtime[2]))
			goto done;
		if (b->scenery_cutout_kept && !structure_build(commands, b->vertices.address + (VkDeviceSize)(opaque + tested +
			b->scenery_opaque) * 36, b->scenery_cutout_kept, 0, &b->runtime_blas_storage[1], &b->runtime_blas[1],
			&runtime[3]))
			goto done;
		barrier(commands);
		if (!scene_structure_into(commands, runtime, &b->runtime_instances, &b->runtime_tlas_storage, &b->runtime_tlas,
			&b->runtime_tlas_address) || !buffer_make(sizeof(b->block), 0, 0, &b->scene_runtime))
			goto done;
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the frame traces the static scenery but %u placements' %u "
			"triangles a mod's mesh draws (traced as drawn)", b->map, (int)b->bsp, b->scenery_replaced_count,
			(b->scenery_opaque - b->scenery_opaque_kept) + (b->scenery_cutout - b->scenery_cutout_kept));
	}
	barrier(commands);
	/* the scene's block (the grid's address and entries once it is made) */
	b->block.vertices = b->vertices.address;
	b->block.info = b->info.address;
	b->block.materials = b->materials.address;
	b->block.alpha = b->alpha.address;
	b->block.lamps = b->lamps.address;
	b->block.counts[0] = opaque;
	b->block.counts[1] = tested;
	b->block.counts[2] = b->lamp_count;
	b->block.counts[3] = b->material_count + scenery_materials;
	b->block.settings[3] = b->scenery_opaque;
	b->block.lamp[0] = b->lamp_power;
	b->block.lamp[1] = GRID_CELL;
	b->block.lamp[2] = (float)b->fixture_count;
	b->block.unused = b->fixtures_buffer.address;
	b->block.lamp[3] = 0.0f;
	b->block.settings[0] = BOUNCES;
	/* (HALO_BAKE_TERMS: terms left out, for diagnosis: xgpu_vulkan_bake_glsl.inc's term_off) */
	b->block.settings[1] = getenv("HALO_BAKE_TERMS") ? (uint32_t)strtoul(getenv("HALO_BAKE_TERMS"), NULL, 0) : 0u;
	if (!buffer_make(sizeof(b->block), 0, 0, &b->scene) || !scene_block_upload(commands, b))
		goto done;
	/* the raster lookup's tables: the pages (their chart maps' first words
	and sizes), the chart maps, the virtual pages, the tile table, the
	tiles' pool, the sun's table and masks */
	pages = (uint32_t *)calloc((size_t)plan->page_count + 1, 16);
	vpages = (uint32_t *)calloc((size_t)plan->vpage_count + 1, 32);
	tridata = (float *)malloc(((size_t)plan->atlas.triangle_count + 1) * 64);
	plan_atlas = (uint32_t *)calloc((size_t)plan->triangle_count + 1, 4);
	b->table_host = (uint32_t *)calloc((size_t)plan->table_words + 1, 4);
	b->sun_host = (uint32_t *)calloc((size_t)plan->table_words + 1, 4);
	if (!pages || !vpages || !tridata || !b->table_host || !b->sun_host)
		goto done;
	for (index = 0; index < plan->page_count; index++)
	{
		pages[index * 4] = plan->chart_offsets[index];
		pages[index * 4 + 1] = plan->pages[index * 2];
		pages[index * 4 + 2] = plan->pages[index * 2 + 1];
	}
	for (index = 0; index < plan->vpage_count; index++)
	{
		vpages[index * 8] = (uint32_t)plan->vpages[index].page;
		vpages[index * 8 + 1] = (uint32_t)plan->vpages[index].scale;
		vpages[index * 8 + 2] = (uint32_t)plan->vpages[index].tiles_x;
		vpages[index * 8 + 3] = (uint32_t)plan->vpages[index].tiles_y;
		vpages[index * 8 + 4] = plan->vpages[index].table_first;
	}
	/* (each atlas triangle: its corners in its virtual page's texels, its
	world corners, its flags: 1 a remodel's, 2 in an indoor cluster, from bit
	8 its virtual page + 1 (0 none): the final gather's) */
	for (index = 0; index < (uint32_t)plan->atlas.triangle_count; index++)
	{
		int32_t chart = plan->atlas.triangle_charts[index];
		float scale = chart >= 0 && plan->atlas.charts[chart].virtual_page >= 0 ?
			(float)plan->vpages[plan->atlas.charts[chart].virtual_page].scale : 1.0f;
		float *to = tridata + (size_t)index * 16;
		uint32_t k, flags, scene_index = plan->atlas_scene[index];

		for (k = 0; k < 6; k++)
			to[k] = plan->atlas.triangle_texels[(size_t)index * 6 + k] * scale;
		memcpy(to + 6, plan->positions + (size_t)scene_index * 9, 36);
		flags = ((plan->attributes[(size_t)scene_index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS +
			XGPU_SCENE_ATTRIBUTE_KIND_PAGE] >> XGPU_SCENE_LEVEL_SHIFT) & 3u) != XGPU_SCENE_LEVEL_BOTH ? 1u : 0u;
		if (plan->attributes[(size_t)scene_index * XGPU_SCENE_STATIC_ATTRIBUTE_WORDS + XGPU_SCENE_ATTRIBUTE_KIND_PAGE] &
			XGPU_SCENE_INDOOR)
			flags |= 2u;
		if (chart >= 0 && plan->atlas.charts[chart].virtual_page >= 0)
			flags |= ((uint32_t)plan->atlas.charts[chart].virtual_page + 1u) << 8;
		memcpy(to + 15, &flags, 4);
		if (plan_atlas && scene_index < plan->triangle_count)
			plan_atlas[scene_index] = index + 1u;
	}
	b->mask_capacity = plan->tile_count / 4 + 64;
	if (!buffer_filled(commands, pages, (VkDeviceSize)plan->page_count * 16 + 16, &b->pages, 0) ||
		!buffer_filled(commands, plan->charts, (VkDeviceSize)plan->chart_texels * 2 + 16, &b->charts, 0) ||
		!buffer_filled(commands, vpages, (VkDeviceSize)plan->vpage_count * 32 + 32, &b->vpages, 0) ||
		!buffer_filled(commands, NULL, (VkDeviceSize)plan->table_words * 4 + 16, &b->table, 0) ||
		!buffer_filled(commands, NULL, (VkDeviceSize)plan->table_words * 4 + 16, &b->sun, 0) ||
		!buffer_make((VkDeviceSize)plan->tile_count * RASTER_TILE_WORDS * 4 + 64, 0, 0, &b->pool) ||
		!buffer_make((VkDeviceSize)plan->tile_count * TILE_TEXELS * 4 + 64, 0, 0, &b->pool_indirect) ||
		!buffer_filled(commands, NULL, (VkDeviceSize)b->mask_capacity * MASK_TEXELS + 64, &b->masks, 0) ||
		!buffer_filled(commands, tridata, (VkDeviceSize)plan->atlas.triangle_count * 64 + 64, &b->triangles, 0) ||
		!buffer_filled(commands, plan->tile_triangles, (VkDeviceSize)plan->tile_triangle_count * 4 + 16, &b->ranges, 0) ||
		!plan_atlas || !buffer_filled(commands, plan_atlas, (VkDeviceSize)plan->triangle_count * 4 + 16, &b->plan_atlas, 0))
		goto done;
	/* (the final gather's cache block: the addresses the lightmap's and the
	volume's passes read the bake's own light through) */
	{
		VkDeviceAddress cache[8];

		memset(cache, 0, sizeof(cache));
		cache[0] = b->vpages.address;
		cache[1] = b->table.address;
		cache[2] = b->pool.address;
		cache[3] = b->triangles.address;
		cache[4] = b->plan_atlas.address;
		if (!buffer_filled(commands, cache, sizeof(cache), &b->cache_block, 0))
			goto done;
	}
	/* the batch's: the texels' records, accumulators, states, running counts,
	the setup's entries and the job blocks */
	if (!buffer_make((VkDeviceSize)BATCH_TILES * TILE_TEXELS * 32, 0, 0, &b->texels) ||
		!buffer_make((VkDeviceSize)BATCH_TILES * TILE_TEXELS * TEXEL_ACCUMULATOR_FLOATS * 4, 0, 0, &b->accumulators) ||
		!buffer_make((VkDeviceSize)BATCH_TILES * TILE_TEXELS * 4, 0, 0, &b->states) ||
		!buffer_make((VkDeviceSize)BATCH_TILES * 4, 0, 0, &b->running) ||
		!buffer_make((VkDeviceSize)BATCH_TILES * 32, 0, 0, &b->tile_origins) ||
		!buffer_make(4096, 0, 0, &b->job) ||
		!buffer_make((VkDeviceSize)(BATCH_TILES * TILE_TEXELS > VOLUME_BATCH ? BATCH_TILES * TILE_TEXELS : VOLUME_BATCH) * 4, 0, 0,
		&b->compact_list) ||
		!buffer_make(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 0, &b->compact_args) ||
		!buffer_make((VkDeviceSize)MASK_BATCH * MASK_TEXELS * 32, 0, 0, &b->mask_ranges) ||
		!buffer_make((VkDeviceSize)MASK_BATCH * MASK_TEXELS, 0, 0, &b->mask_out) ||
		!buffer_make(RASTER_LIGHTS_OFFSET + RASTER_LIGHTS * 12 * 4, 0, 0, &b->raster_job))
		goto done;
	vkCmdFillBuffer(commands, b->states.buffer, 0, VK_WHOLE_SIZE, 4u);
	vkCmdFillBuffer(commands, b->running.buffer, 0, VK_WHOLE_SIZE, 0u);
	barrier(commands);
	for (index = 0; index < BATCH_TILES; index++)
		b->batch.tiles[index] = 0xffffffffu;
	b->tiles_planned = plan->tile_count;
	ok = 1;

done:
	free(replaced);
	free(host_materials);
	free(host_alpha);
	free(vertices);
	free(info);
	free(pages);
	free(vpages);
	free(tridata);
	free(plan_atlas);
	return ok;
}

/* ---------- encodings */

static uint32_t rgb9e5(const float rgb[3])
{
	float most = 0.0f, r, g, b, scale;
	int exponent, shared;
	uint32_t ri, gi, bi;

	r = rgb[0] > 0.0f ? (rgb[0] < 65408.0f ? rgb[0] : 65408.0f) : 0.0f;
	g = rgb[1] > 0.0f ? (rgb[1] < 65408.0f ? rgb[1] : 65408.0f) : 0.0f;
	b = rgb[2] > 0.0f ? (rgb[2] < 65408.0f ? rgb[2] : 65408.0f) : 0.0f;
	most = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (!(most > 1.0e-30f))
		return 0;
	exponent = (int)floorf(log2f(most)) + 1;
	shared = exponent + 15;
	if (shared < 0)
		shared = 0;
	if (shared > 31)
		shared = 31;
	scale = exp2f((float)(shared - 15 - 9));
	if ((uint32_t)floorf(most / scale + 0.5f) == 512u && shared < 31)
	{
		shared++;
		scale *= 2.0f;
	}
	ri = (uint32_t)floorf(r / scale + 0.5f);
	gi = (uint32_t)floorf(g / scale + 0.5f);
	bi = (uint32_t)floorf(b / scale + 0.5f);
	ri = ri > 511u ? 511u : ri;
	gi = gi > 511u ? 511u : gi;
	bi = bi > 511u ? 511u : bi;
	return ri | (gi << 9) | (bi << 18) | ((uint32_t)shared << 27);
}

static float luma3(const float c[3])
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

static uint8_t unorm8(float v)
{
	v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	return (uint8_t)(v * 255.0f + 0.5f);
}

/* ---------- the status the game reads */

static void status_set(int word, uint32_t value)
{
	uint32_t *status;

	if (!bake.control_known || !bake.control.status)
		return;
	status = (uint32_t *)host_pointer(bake.control.status);
	if (status)
		__atomic_store_n(&status[word], value, __ATOMIC_RELEASE);
}

static void status_real(int word, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	status_set(word, bits);
}

/* ---------- readbacks */

static struct bake_readback *readback_take(VkDeviceSize size, int kind)
{
	uint32_t index;

	for (index = 0; index < READBACK_SLOTS; index++)
	{
		struct bake_readback *r = &bake.readbacks[index];

		if (r->busy)
			continue;
		if (r->buffer.size < size)
		{
			if (r->buffer.buffer)
				garbage_add(&r->buffer, VK_NULL_HANDLE);
			if (!buffer_make(size, 0, 1, &r->buffer))
				return NULL;
		}
		r->busy = 1;
		r->serial = xgpu_vulkan_serial();
		r->kind = kind;
		r->finished = 0;
		r->batch = 0;
		return r;
	}
	return NULL;
}

static void copy_region(VkCommandBuffer commands, struct bake_buffer *from, VkDeviceSize from_offset,
	struct bake_buffer *to, VkDeviceSize to_offset, VkDeviceSize size)
{
	VkBufferCopy copy;

	copy.srcOffset = from_offset;
	copy.dstOffset = to_offset;
	copy.size = size;
	vkCmdCopyBuffer(commands, from->buffer, to->buffer, 1, &copy);
}

/* ---------- the file */

static void file_append(struct bake_state *b, uint32_t type, uint64_t job, const void *payload, size_t size)
{
	if (!b->writing)
		return;
	if (prism_bake_cache_append(&b->writer, type, job, payload, size))
	{
		host_logf(HOST_LOG_WARN, "prism bake: %s: cannot be written (%s): the bake goes on unsaved", b->new_path,
			strerror(errno));
		prism_bake_cache_writer_close(&b->writer);
		b->writing = 0;
		return;
	}
	b->file_bytes = b->writer.end;
}

/* ---------- a tile finished: its texels' results (the accumulators, states
and records of its 256 texels), written and drawn */

/* a texel's results: light (less the sun's), indirect, direction (k, a0), sun */
struct texel_result
{
	float total[3], indirect[3], k[3], a0, sun;
	/* (the layered bake's: the sun's bounce, the sky, the domes, the
	fixtures; whether its cluster is indoor) */
	float layers[PRISM_BAKE_LAYER_SHARES][3];
	uint32_t samples;
	uint8_t state, valid, facing, indoor;
};

/* the raster's and the file's words of a tile from its results; its sun's
state (1 dark, 2 lit, 3 mixed) */
static int tile_words(struct texel_result *texels, uint32_t *raster, uint32_t *file, const float sun[4])
{
	int pass, lit = 0, dark = 0, facing = 0;
	uint32_t index;

	/* (the empty texels and those inside the level from their valid
	neighbours, ring by ring, within the tile) */
	for (pass = 0; pass < 16; pass++)
	{
		int changed = 0;
		uint8_t valid[TILE_TEXELS];

		for (index = 0; index < TILE_TEXELS; index++)
			valid[index] = texels[index].valid;
		for (index = 0; index < TILE_TEXELS; index++)
		{
			int x = (int)(index % TILE), y = (int)(index / TILE), dx, dy, count = 0, k, layer, indoor = 0;
			struct texel_result sum;

			if (valid[index])
				continue;
			memset(&sum, 0, sizeof(sum));
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++)
				{
					int nx = x + dx, ny = y + dy;
					struct texel_result const *n;

					if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= (int)TILE || ny >= (int)TILE ||
						!valid[ny * TILE + nx])
						continue;
					n = &texels[ny * TILE + nx];
					for (k = 0; k < 3; k++)
					{
						sum.total[k] += n->total[k];
						sum.indirect[k] += n->indirect[k];
						sum.k[k] += n->k[k];
						for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
							sum.layers[layer][k] += n->layers[layer][k];
					}
					indoor += n->indoor;
					sum.a0 += n->a0;
					sum.sun += n->sun;
					count++;
				}
			if (!count)
				continue;
			for (k = 0; k < 3; k++)
			{
				texels[index].total[k] = sum.total[k] / (float)count;
				texels[index].indirect[k] = sum.indirect[k] / (float)count;
				texels[index].k[k] = sum.k[k] / (float)count;
				for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
					texels[index].layers[layer][k] = sum.layers[layer][k] / (float)count;
			}
			texels[index].indoor = (uint8_t)(indoor * 2 >= count);
			texels[index].a0 = sum.a0 / (float)count;
			texels[index].sun = sum.sun / (float)count;
			texels[index].valid = 2;
			changed = 1;
		}
		if (!changed)
			break;
	}
	for (index = 0; index < TILE_TEXELS; index++)
	{
		struct texel_result *t = &texels[index];
		uint32_t samples_log = 0;

		if (!t->valid)
		{
			raster[index * 2] = raster[index * 2 + 1] = 0;
			memset(file + index * 4, 0, 16);
			continue;
		}
		raster[index * 2] = rgb9e5(t->total);
		raster[index * 2 + 1] = (uint32_t)unorm8(t->k[0] * 0.25f + 0.5f) | ((uint32_t)unorm8(t->k[1] * 0.25f + 0.5f) << 8) |
			((uint32_t)unorm8(t->k[2] * 0.25f + 0.5f) << 16) | ((uint32_t)(unorm8((t->a0 + 1.0f) * 0.25f) | 1u) << 24);
		while (samples_log < 255 && (1u << (samples_log / 8)) < t->samples)
			samples_log += 8;
		file[index * 4] = raster[index * 2];
		file[index * 4 + 1] = raster[index * 2 + 1];
		file[index * 4 + 2] = rgb9e5(t->indirect);
		file[index * 4 + 3] = (uint32_t)unorm8(t->sun) | ((uint32_t)t->state << 8) | (samples_log << 16) |
			((uint32_t)(t->valid | (t->facing ? 4u : 0u)) << 24);
		/* (the sun's state: of the texels on the level facing the sun) */
		if (t->valid == 1 && t->facing)
		{
			if (t->sun >= 0.999f)
				lit++;
			else if (t->sun <= 0.001f)
				dark++;
			facing++;
		}
	}
	if (!facing || (dark == facing))
		return 1;
	if (lit == facing)
		return 2;
	return 3;
}

/* the raster's: a tile's words into the pool (a physical tile taken), its
table entry; its sun's state (and a mask's coarse first content from the
texels' visibility) */
static int tile_draw(VkCommandBuffer commands, struct bake_state *b, uint32_t tile_index, const uint32_t *raster,
	int sun_state, const uint8_t *sun_texels, const uint8_t *fine_mask)
{
	struct bake_tile *tile = &b->plan.tiles[tile_index];
	struct bake_vpage const *vp = &b->plan.vpages[tile->vpage];
	uint32_t entry = vp->table_first + (uint32_t)tile->y * (uint32_t)vp->tiles_x + tile->x, sun_value;

	if (!tile->physical)
		tile->physical = ++b->physical_count;
	if (!upload(commands, &b->pool, (VkDeviceSize)(tile->physical - 1) * RASTER_TILE_WORDS * 4, raster,
		RASTER_TILE_WORDS * 4))
		return 0;
	sun_value = (uint32_t)sun_state;
	if (sun_state >= 3)
	{
		uint8_t mask[MASK_TEXELS];
		uint32_t x, y;

		if (tile->sun < 3)
		{
			if (b->mask_count >= b->mask_capacity)
			{
				/* (the masks' buffer grown, its contents kept) */
				struct bake_buffer grown;
				uint32_t capacity = b->mask_capacity + b->mask_capacity / 2 + 64;

				if (!buffer_make((VkDeviceSize)capacity * MASK_TEXELS + 64, 0, 0, &grown))
					return 0;
				barrier(commands);
				copy_region(commands, &b->masks, 0, &grown, 0, (VkDeviceSize)b->mask_capacity * MASK_TEXELS);
				garbage_add(&b->masks, VK_NULL_HANDLE);
				b->masks = grown;
				b->mask_capacity = capacity;
			}
			tile->sun = 3 + b->mask_count++;
		}
		/* (the texels' visibility, bilinear, at the mask's density; or the
		fine mask itself where the caller has it, so that the slot is written
		once: two copies to it would race) */
		if (fine_mask)
			memcpy(mask, fine_mask, MASK_TEXELS);
		else
		for (y = 0; y < MASK_SIDE; y++)
			for (x = 0; x < MASK_SIDE; x++)
			{
				float fx = ((float)x + 0.5f) / (float)MASK_FACTOR - 0.5f, fy = ((float)y + 0.5f) / (float)MASK_FACTOR - 0.5f;
				int x0 = (int)floorf(fx), y0 = (int)floorf(fy), x1, y1;
				float tx = fx - (float)x0, ty = fy - (float)y0, v;

				x0 = x0 < 0 ? 0 : x0;
				y0 = y0 < 0 ? 0 : y0;
				x1 = x0 + 1 >= (int)TILE ? (int)TILE - 1 : x0 + 1;
				y1 = y0 + 1 >= (int)TILE ? (int)TILE - 1 : y0 + 1;
				v = ((float)sun_texels[y0 * TILE + x0] * (1.0f - tx) + (float)sun_texels[y0 * TILE + x1] * tx) * (1.0f - ty) +
					((float)sun_texels[y1 * TILE + x0] * (1.0f - tx) + (float)sun_texels[y1 * TILE + x1] * tx) * ty;
				mask[y * MASK_SIDE + x] = (uint8_t)(v + 0.5f);
			}
		if (!upload(commands, &b->masks, (VkDeviceSize)(tile->sun - 3) * MASK_TEXELS, mask, MASK_TEXELS))
			return 0;
		sun_value = tile->sun;
	}
	else
		tile->sun = sun_value;
	b->table_host[entry] = tile->physical;
	b->sun_host[entry] = sun_value;
	if (!upload(commands, &b->table, (VkDeviceSize)entry * 4, &tile->physical, 4) ||
		!upload(commands, &b->sun, (VkDeviceSize)entry * 4, &sun_value, 4))
		return 0;
	return 1;
}

/* PRISM Path Traced's: a drawn tile's indirect light (its file words' third
of each texel) into the indirect pool, at its physical tile */
static int tile_indirect(VkCommandBuffer commands, struct bake_state *b, uint32_t tile_index, const uint32_t *words)
{
	struct bake_tile *tile = &b->plan.tiles[tile_index];

	if (!tile->physical || !b->pool_indirect.buffer)
		return 0;
	return upload(commands, &b->pool_indirect, (VkDeviceSize)(tile->physical - 1) * TILE_TEXELS * 4, words,
		TILE_TEXELS * 4);
}

/* the variance of a texel's luminance's mean from its accumulators: its
stratified batches' (4 or more), else its samples' */
static float texel_variance(const float *a, uint32_t count)
{
	uint32_t batches;

	memcpy(&batches, &a[17], 4);
	if (batches >= 4)
		return (a[19] > 0.0f ? a[19] : 0.0f) / (float)(batches - 1) / (float)batches;
	if (count > 1)
		return (a[14] > 0.0f ? a[14] : 0.0f) / (float)(count - 1) / (float)count;
	return 0.0f;
}

/* a tile's results from its readback: written to the file (with the
filter's words of its texels, where it filters: their robust means'
levels, prism_bake_filter.h), drawn, its mask asked for where its sun is
mixed */
static void tile_finish(VkCommandBuffer commands, struct bake_state *b, uint32_t tile_index, const float *accumulators,
	const uint32_t *states, const float *records)
{
	struct texel_result texels[TILE_TEXELS];
	uint32_t raster[RASTER_TILE_WORDS], file[FILE_TILE_WORDS], aux[AUX_TILE_WORDS], index;
	uint32_t drawn[RASTER_TILE_WORDS], indirect[TILE_TEXELS];
	uint8_t sun_texels[TILE_TEXELS], layers[ATLAS_LAYER_BYTES];
	int sun_state;
	float const *sun = b->block.sun;
	int filtered = b->filter.passes > 0 && !b->drafting;

	memset(aux, 0, sizeof(aux));
	memset(layers, 0, sizeof(layers));

	for (index = 0; index < TILE_TEXELS; index++)
	{
		const float *a = accumulators + (size_t)index * TEXEL_ACCUMULATOR_FLOATS;
		const float *r = records + (size_t)index * 8;
		struct texel_result *t = &texels[index];
		uint32_t flags, count, kept;
		float lum, n[3], k, factor = 1.0f, variance;
		int axis;
		int32_t chart;

		memset(t, 0, sizeof(*t));
		memcpy(&flags, &r[6], 4);
		memcpy(&count, &a[11], 4);
		t->state = (uint8_t)states[index];
		t->samples = count;
		if (count > 1 && a[13] > BLACK_FLOOR)
		{
			double rse_samples = (double)a[14] / (double)(count - 1) / ((double)a[13] * a[13] * 1.0e-4);
			double firefly_samples = (double)a[15] / ((double)a[13] * 0.01);

			if (firefly_samples > rse_samples)
				b->texels_firefly_bound += 1.0;
		}
		if (flags && states[index] == 1)
		{
			b->texels_converged += 1.0;
			if (b->noise_floor > 0.0f && a[13] < b->noise_floor)
				b->texels_floor += 1.0;
		}
		if (flags && b->quality == PRISM_BAKE_QUALITY_MAX && (float)count >= MAX_SAMPLES_CAP)
			b->texels_capped += 1.0;
		if (!flags || (states[index] != 1 && states[index] != 2))
			continue;
		/* (the robust mean: the level the filter compares, the groups'
		median where a firefly put their mean far over it; the light as
		sampled) */
		variance = texel_variance(a, count);
		if (b->robust_on && !b->drafting)
		{
			factor = prism_bake_robust_factor(a + 20, count, a[15], &variance, &b->robust);
			if (factor != 1.0f)
				b->texels_robust += 1.0;
		}
		for (axis = 0; axis < 3; axis++)
		{
			int layer;

			t->total[axis] = a[axis] + a[3 + axis];
			t->indirect[axis] = a[3 + axis];
			n[axis] = r[3 + axis];
			for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
				t->layers[layer][axis] = a[28 + layer * 3 + axis];
		}
		t->indoor = (flags & 8u) != 0;
		b->saw_remodel |= (flags & 4u) != 0;
		lum = luma3(t->total);
		/* (the direction: the moment's luminance over 2 pi, as a share of
		the light at the texel's normal; at most 2) */
		for (axis = 0; axis < 3; axis++)
			t->k[axis] = lum > BLACK_FLOOR ? a[6 + axis] / (6.2831853f * lum) : 0.0f;
		k = sqrtf(t->k[0] * t->k[0] + t->k[1] * t->k[1] + t->k[2] * t->k[2]);
		if (k > 1.9f)
			for (axis = 0; axis < 3; axis++)
				t->k[axis] *= 1.9f / k;
		t->a0 = 1.0f - (t->k[0] * n[0] + t->k[1] * n[1] + t->k[2] * n[2]);
		t->sun = a[9];
		/* (on a triangle and facing the sun: its visibility counts for the
		tile's state; a texel facing away has none to give) */
		t->facing = sun[3] > 0.0f && (flags & 2u) && n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2] > 0.0f;
		if (!t->facing)
			t->sun = 0.0f;
		t->valid = 1;
		/* (the filter's words: the normal, the chart (the texel's atlas
		triangle's) + 1, the variance of the level, the robust mean's
		factor) */
		memcpy(&kept, &r[7], 4);
		chart = kept && kept - 1u < b->plan.atlas.triangle_count ? b->plan.atlas.triangle_charts[kept - 1u] : -1;
		aux[index * 4] = prism_bake_normal_pack(n);
		aux[index * 4 + 1] = chart >= 0 ? (uint32_t)chart + 1u : 0u;
		memcpy(&aux[index * 4 + 2], &variance, 4);
		memcpy(&aux[index * 4 + 3], &factor, 4);
	}
	sun_state = tile_words(texels, raster, file, sun);
	for (index = 0; index < TILE_TEXELS; index++)
	{
		sun_texels[index] = unorm8(texels[index].sun);
		/* (the layers' shares of the light, the filled texels' too) */
		if (texels[index].valid)
			prism_bake_layer_shares_put(texels[index].total, (const float (*)[3])texels[index].layers,
				texels[index].indoor, layers, index, TILE_TEXELS);
	}
	/* (the light drawn: recombined by the weights now, prism_bake_layers.h) */
	prism_bake_layers_tile(&b->weights, file, layers, TILE_TEXELS, drawn, indirect);
	if (b->drafting)
	{
		/* (the draft: drawn and kept as the tile's checkpoint, the tile baked
		again from nothing by the pass after) */
		uint8_t draft[sizeof(raster) + TILE_TEXELS + 4];

		memcpy(draft, raster, sizeof(raster));
		memcpy(draft + sizeof(raster), sun_texels, TILE_TEXELS);
		memcpy(draft + sizeof(raster) + TILE_TEXELS, &sun_state, 4);
		file_append(b, PRISM_BAKE_RECORD_CHECKPOINT, b->plan.tiles[tile_index].job, draft, sizeof(draft));
		if (!tile_draw(commands, b, tile_index, drawn, sun_state < 3 ? sun_state : 3, sun_texels, NULL))
			return;
		b->plan.tiles[tile_index].state = TILE_PENDING;
		b->drafted++;
		return;
	}
	file_append(b, PRISM_BAKE_RECORD_ATLAS_TILE, b->plan.tiles[tile_index].job, file, sizeof(file));
	file_append(b, PRISM_BAKE_RECORD_ATLAS_LAYERS, b->plan.tiles[tile_index].job, layers, sizeof(layers));
	if (filtered)
		file_append(b, PRISM_BAKE_RECORD_ATLAS_AUX, b->plan.tiles[tile_index].job, aux, sizeof(aux));
	if (!tile_draw(commands, b, tile_index, drawn, sun_state, sun_texels, NULL) ||
		!tile_indirect(commands, b, tile_index, indirect))
		return;
	b->plan.tiles[tile_index].state = TILE_DONE;
	b->plan.tiles[tile_index].mask_state = sun_state >= 3 ? 1 : 0;
	b->tiles_done++;
	for (index = 0; index < TILE_TEXELS; index++)
	{
		b->rays += texels[index].samples;
		if (texels[index].samples)
			b->texels_baked += 1.0;
		if (texels[index].samples > b->samples_most)
			b->samples_most = texels[index].samples;
	}
}

/* ---------- the filter in chart space (prism_bake_filter.h): once every
tile is baked, on a thread, a virtual page at a time and its tiles row by
row, each tile from the file's results of it and its eight neighbours
(their texels as baked and the filter's words), handed back to the frame,
which writes it (ATLAS_FILTERED) and draws it */

static void rgb9e5_decode(uint32_t v, float rgb[3])
{
	float e = exp2f((float)((int)(v >> 27) - 15 - 9));

	rgb[0] = (float)(v & 511u) * e;
	rgb[1] = (float)((v >> 9) & 511u) * e;
	rgb[2] = (float)((v >> 18) & 511u) * e;
}

/* a tile's texels in the filter's terms from its file words and filter
words (NULL: the tile is not there, every texel chart -1) */
static void filter_texels(const uint32_t *file, const uint32_t *aux, struct prism_bake_filter_texel *texels)
{
	uint32_t index;

	for (index = 0; index < TILE_TEXELS; index++)
	{
		struct prism_bake_filter_texel *t = &texels[index];
		uint32_t w1, axis;
		float lum, factor;

		memset(t, 0, sizeof(*t));
		t->chart = -1;
		if (!file || !aux || ((file[index * 4 + 3] >> 24) & 3u) != 1u || !aux[index * 4 + 1])
			continue;
		rgb9e5_decode(file[index * 4], t->light);
		rgb9e5_decode(file[index * 4 + 2], t->indirect);
		lum = luma3(t->light);
		w1 = file[index * 4 + 1];
		for (axis = 0; axis < 3; axis++)
			t->moment[axis] = ((float)((w1 >> (axis * 8)) & 255u) / 255.0f - 0.5f) * 4.0f * lum;
		memcpy(&t->variance, &aux[index * 4 + 2], 4);
		memcpy(&factor, &aux[index * 4 + 3], 4);
		t->level = lum * factor;
		prism_bake_normal_unpack(aux[index * 4], t->normal);
		t->chart = (int32_t)aux[index * 4 + 1] - 1;
	}
}

/* a filtered tile's words: its baked texels' light, indirect light and
direction the filter's, the rest (the sun, the state, the samples) as
baked, the empty texels filled again from the filtered ones */
static void filter_words(const uint32_t *baked, struct prism_bake_filter_texel const *filtered, uint32_t *file,
	uint32_t *raster, const float sun[4])
{
	struct texel_result texels[TILE_TEXELS];
	uint32_t index;

	for (index = 0; index < TILE_TEXELS; index++)
	{
		struct texel_result *t = &texels[index];
		struct prism_bake_filter_texel const *f = &filtered[index];
		uint32_t w3 = baked[index * 4 + 3], axis;
		float lum, k;

		memset(t, 0, sizeof(*t));
		if (((w3 >> 24) & 3u) != 1u)
			continue;
		t->valid = 1;
		t->state = (uint8_t)((w3 >> 8) & 255u);
		t->samples = 1u << (((w3 >> 16) & 255u) / 8u);
		t->facing = (w3 >> 24) & 4u ? 1 : 0;
		t->sun = (float)(w3 & 255u) / 255.0f;
		if (f->chart < 0)
		{
			/* (a texel the filter did not take: as baked) */
			uint32_t w1 = baked[index * 4 + 1];

			rgb9e5_decode(baked[index * 4], t->total);
			rgb9e5_decode(baked[index * 4 + 2], t->indirect);
			for (axis = 0; axis < 3; axis++)
				t->k[axis] = ((float)((w1 >> (axis * 8)) & 255u) / 255.0f - 0.5f) * 4.0f;
			t->a0 = (float)(w1 >> 24) / 255.0f * 4.0f - 1.0f;
			continue;
		}
		memcpy(t->total, f->light, sizeof(t->total));
		memcpy(t->indirect, f->indirect, sizeof(t->indirect));
		lum = luma3(f->light);
		for (axis = 0; axis < 3; axis++)
			t->k[axis] = lum > BLACK_FLOOR ? f->moment[axis] / lum : 0.0f;
		k = sqrtf(t->k[0] * t->k[0] + t->k[1] * t->k[1] + t->k[2] * t->k[2]);
		if (k > 1.9f)
			for (axis = 0; axis < 3; axis++)
				t->k[axis] *= 1.9f / k;
		t->a0 = 1.0f - (t->k[0] * f->normal[0] + t->k[1] * f->normal[1] + t->k[2] * f->normal[2]);
	}
	tile_words(texels, raster, file, sun);
	/* (the sun's words exactly as baked: the tile's sun state and mask stand) */
	for (index = 0; index < TILE_TEXELS; index++)
		file[index * 4 + 3] = baked[index * 4 + 3];
}

/* a row of a virtual page's tiles, held while the rows beside it are
filtered: each tile's file words and its texels in the filter's terms */
struct filter_row
{
	int32_t y;
	uint8_t *have;
	uint32_t *file;
	struct prism_bake_filter_texel *texels;
};

static void filter_row_load(struct bake_state *b, struct prism_bake_cache *reader, const int32_t *map,
	struct bake_vpage const *vp, int32_t y, struct filter_row *row)
{
	int32_t x;
	uint32_t aux[AUX_TILE_WORDS];

	row->y = y;
	for (x = 0; x < vp->tiles_x; x++)
	{
		int32_t tile = y >= 0 && y < vp->tiles_y ? map[y * vp->tiles_x + x] : -1;
		struct prism_bake_record const *result, *words;
		uint32_t *file = row->file + (size_t)x * FILE_TILE_WORDS;

		row->have[x] = 0;
		if (tile < 0)
			continue;
		result = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_ATLAS_TILE, b->plan.tiles[tile].job);
		words = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_ATLAS_AUX, b->plan.tiles[tile].job);
		if (!result || !words || result->size != FILE_TILE_WORDS * 4 || words->size != sizeof(aux) ||
			words->sequence < result->sequence || prism_bake_cache_read(reader, result, file, FILE_TILE_WORDS * 4) ||
			prism_bake_cache_read(reader, words, aux, sizeof(aux)))
			continue;
		filter_texels(file, aux, row->texels + (size_t)x * TILE_TEXELS);
		row->have[x] = 1;
	}
}

/* a result handed to the frame: waits while the ring is full; 0 once the
filter is to stop */
static int filter_hand(struct bake_state *b, uint32_t tile, const uint32_t *file, const uint32_t *raster,
	const uint8_t *layers)
{
	struct filter_result *r;

	pthread_mutex_lock(&b->filter_lock);
	while (b->filter_count >= FILTER_RING && !b->filter_stop)
		pthread_cond_wait(&b->filter_room, &b->filter_lock);
	if (b->filter_stop)
	{
		pthread_mutex_unlock(&b->filter_lock);
		return 0;
	}
	/* (the slot past the frame's, this thread's until counted: the frame
	moves the tail and the count together) */
	r = &b->filter_ring[(b->filter_tail + b->filter_count) % FILTER_RING];
	pthread_mutex_unlock(&b->filter_lock);
	r->tile = tile;
	memcpy(r->file, file, sizeof(r->file));
	memcpy(r->raster, raster, sizeof(r->raster));
	r->has_layers = layers != NULL;
	if (layers)
		memcpy(r->layers, layers, sizeof(r->layers));
	pthread_mutex_lock(&b->filter_lock);
	b->filter_count++;
	pthread_mutex_unlock(&b->filter_lock);
	return 1;
}

static void *filter_main(void *context)
{
	struct bake_state *b = (struct bake_state *)context;
	struct bake_plan *plan = &b->plan;
	struct prism_bake_cache reader;
	struct prism_bake_filter_texel *window = NULL, *scratch = NULL, out[TILE_TEXELS], empty;
	struct filter_row rows[3];
	uint32_t first = 0, vpage, file[FILE_TILE_WORDS], raster[RASTER_TILE_WORDS];
	uint8_t layers[ATLAS_LAYER_BYTES];
	int32_t *map = NULL;
	int32_t widest = 1, k;
	float sun[4];

	memset(&reader, 0, sizeof(reader));
	memset(rows, 0, sizeof(rows));
	memset(&empty, 0, sizeof(empty));
	empty.chart = -1;
	memcpy(sun, b->block.sun, sizeof(sun));
	for (vpage = 0; vpage < plan->vpage_count; vpage++)
		if (plan->vpages[vpage].tiles_x > widest)
			widest = plan->vpages[vpage].tiles_x;
	window = (struct prism_bake_filter_texel *)malloc(PRISM_BAKE_FILTER_WINDOW * PRISM_BAKE_FILTER_WINDOW *
		sizeof(*window));
	scratch = (struct prism_bake_filter_texel *)malloc(2 * PRISM_BAKE_FILTER_WINDOW * PRISM_BAKE_FILTER_WINDOW *
		sizeof(*scratch));
	for (k = 0; k < 3; k++)
	{
		rows[k].have = (uint8_t *)calloc((size_t)widest, 1);
		rows[k].file = (uint32_t *)malloc((size_t)widest * FILE_TILE_WORDS * 4);
		rows[k].texels = (struct prism_bake_filter_texel *)malloc((size_t)widest * TILE_TEXELS * sizeof(*rows[k].texels));
		if (!rows[k].have || !rows[k].file || !rows[k].texels)
			b->filter_failed = 1;
	}
	if (!window || !scratch || b->filter_failed ||
		prism_bake_cache_open(&reader, b->new_path, &b->key, 0) != PRISM_BAKE_CACHE_CURRENT)
	{
		b->filter_failed = 1;
		goto done;
	}
	for (vpage = 0; vpage < plan->vpage_count && !b->filter_stop; vpage++)
	{
		struct bake_vpage const *vp = &plan->vpages[vpage];
		uint32_t last = first, index;
		int32_t y, x;

		/* (the plan's tiles are its virtual pages' in turn, row by row) */
		while (last < plan->tile_count && plan->tiles[last].vpage == vpage)
			last++;
		if (last == first)
			continue;
		free(map);
		map = (int32_t *)malloc((size_t)vp->tiles_x * vp->tiles_y * sizeof(int32_t));
		if (!map)
		{
			b->filter_failed = 1;
			break;
		}
		for (index = 0; index < (uint32_t)(vp->tiles_x * vp->tiles_y); index++)
			map[index] = -1;
		for (index = first; index < last; index++)
			map[plan->tiles[index].y * vp->tiles_x + plan->tiles[index].x] = (int32_t)index;
		for (k = 0; k < 3; k++)
			rows[k].y = -1000;
		for (index = first; index < last && !b->filter_stop; index++)
		{
			struct bake_tile const *tile = &plan->tiles[index];
			struct filter_row *middle;
			int32_t dy, dx, line;

			y = tile->y;
			x = tile->x;
			/* (rows y - 1, y and y + 1 held: row r in rows[r mod 3]) */
			for (dy = -1; dy <= 1; dy++)
			{
				struct filter_row *row = &rows[(y + dy + 3) % 3];

				if (row->y != y + dy)
					filter_row_load(b, &reader, map, vp, y + dy, row);
			}
			middle = &rows[y % 3];
			if (!middle->have[x])
				continue;
			for (dy = -1; dy <= 1; dy++)
			{
				struct filter_row const *row = &rows[(y + dy + 3) % 3];

				for (dx = -1; dx <= 1; dx++)
				{
					int there = x + dx >= 0 && x + dx < vp->tiles_x && row->have[x + dx];
					struct prism_bake_filter_texel const *from = there ? row->texels + (size_t)(x + dx) * TILE_TEXELS :
						NULL;

					for (line = 0; line < (int32_t)TILE; line++)
					{
						struct prism_bake_filter_texel *to = window + ((dy + 1) * TILE + line) * PRISM_BAKE_FILTER_WINDOW +
							(dx + 1) * TILE;
						int32_t c;

						if (from)
							memcpy(to, from + line * TILE, TILE * sizeof(*to));
						else
							for (c = 0; c < (int32_t)TILE; c++)
								to[c] = empty;
					}
				}
			}
			prism_bake_filter_tile(window, out, scratch, &b->filter);
			filter_words(middle->file + (size_t)x * FILE_TILE_WORDS, out, file, raster, sun);
			{
				/* (its layers, for the light drawn: recombined by the frame) */
				struct prism_bake_record const *shares = prism_bake_cache_find(&reader, PRISM_BAKE_RECORD_ATLAS_LAYERS,
					tile->job);
				int have = shares && shares->size == ATLAS_LAYER_BYTES &&
					prism_bake_cache_read(&reader, shares, layers, ATLAS_LAYER_BYTES) == 0;

				if (!filter_hand(b, index, file, raster, have ? layers : NULL))
					break;
			}
		}
		first = last;
	}
done:
	prism_bake_cache_close(&reader);
	free(map);
	free(window);
	free(scratch);
	for (k = 0; k < 3; k++)
	{
		free(rows[k].have);
		free(rows[k].file);
		free(rows[k].texels);
	}
	pthread_mutex_lock(&b->filter_lock);
	b->filter_finished = 1;
	pthread_mutex_unlock(&b->filter_lock);
	return NULL;
}

/* the filter begun once every tile is baked: 1 where it runs */
static int filter_start(struct bake_state *b)
{
	if (b->filter.passes <= 0 || !b->writing || b->filter_state)
		return 0;
	if (b->tiles_filtered >= b->plan.tile_count)
	{
		/* (the file had every tile filtered) */
		b->filter_state = 2;
		return 0;
	}
	b->filter_ring = (struct filter_result *)malloc(FILTER_RING * sizeof(struct filter_result));
	if (!b->filter_ring || fflush(b->writer.file) != 0)
	{
		free(b->filter_ring);
		b->filter_ring = NULL;
		host_logf(HOST_LOG_WARN, "prism bake: %s bsp %d: the filter cannot run (no memory): its texels as baked", b->map,
			(int)b->bsp);
		b->filter_state = 2;
		return 0;
	}
	pthread_mutex_init(&b->filter_lock, NULL);
	pthread_cond_init(&b->filter_room, NULL);
	b->filter_tail = b->filter_count = b->filter_tiles = 0;
	b->filter_stop = b->filter_finished = b->filter_failed = 0;
	b->filter_started = now_seconds();
	if (pthread_create(&b->filter_thread, NULL, filter_main, b) != 0)
	{
		pthread_mutex_destroy(&b->filter_lock);
		pthread_cond_destroy(&b->filter_room);
		free(b->filter_ring);
		b->filter_ring = NULL;
		b->filter_state = 2;
		return 0;
	}
	b->filter_state = 1;
	return 1;
}

/* the filtered tiles waiting written and drawn (FILTER_FRAME a frame at
most); the filter's end */
static void filter_drain(VkCommandBuffer commands, struct bake_state *b)
{
	uint32_t count, done = 0;
	int finished;

	if (b->filter_state != 1)
		return;
	pthread_mutex_lock(&b->filter_lock);
	count = b->filter_count;
	finished = b->filter_finished;
	pthread_mutex_unlock(&b->filter_lock);
	while (done < count && done < FILTER_FRAME)
	{
		struct filter_result const *r = &b->filter_ring[(b->filter_tail + done) % FILTER_RING];
		struct bake_tile const *tile = &b->plan.tiles[r->tile];
		uint32_t drawn[RASTER_TILE_WORDS], indirect[TILE_TEXELS];

		/* (recombined by the weights now) */
		prism_bake_layers_tile(&b->weights, r->file, r->has_layers ? r->layers : NULL, TILE_TEXELS, drawn, indirect);
		if (tile->physical && !upload(commands, &b->pool, (VkDeviceSize)(tile->physical - 1) * RASTER_TILE_WORDS * 4,
			drawn, RASTER_TILE_WORDS * 4))
			break;
		/* (PRISM Path Traced's indirect pool the filtered tile's too, as a
		loaded bake's is) */
		if (tile->physical && !tile_indirect(commands, b, r->tile, indirect))
			break;
		file_append(b, PRISM_BAKE_RECORD_ATLAS_FILTERED, tile->job, r->file, sizeof(r->file));
		done++;
	}
	pthread_mutex_lock(&b->filter_lock);
	b->filter_tail = (b->filter_tail + done) % FILTER_RING;
	b->filter_count -= done;
	b->filter_tiles += done;
	pthread_cond_signal(&b->filter_room);
	pthread_mutex_unlock(&b->filter_lock);
	if (finished && done == count)
	{
		pthread_join(b->filter_thread, NULL);
		pthread_mutex_destroy(&b->filter_lock);
		pthread_cond_destroy(&b->filter_room);
		free(b->filter_ring);
		b->filter_ring = NULL;
		b->filter_state = 2;
		b->filter_seconds = now_seconds() - b->filter_started;
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the filter: %u of %u tiles in %.1f s (%d passes)%s", b->map,
			(int)b->bsp, b->filter_tiles, b->plan.tile_count, b->filter_seconds, b->filter.passes,
			b->filter_failed ? ", stopped short (no memory or no file): the rest as baked" : "");
	}
}

/* the filter let go of (the bake given up): its thread stopped */
static void filter_stop(struct bake_state *b)
{
	if (b->filter_state != 1)
		return;
	pthread_mutex_lock(&b->filter_lock);
	b->filter_stop = 1;
	pthread_cond_signal(&b->filter_room);
	pthread_mutex_unlock(&b->filter_lock);
	pthread_join(b->filter_thread, NULL);
	pthread_mutex_destroy(&b->filter_lock);
	pthread_cond_destroy(&b->filter_room);
	free(b->filter_ring);
	b->filter_ring = NULL;
	b->filter_state = 0;
}

/* ---------- the passes */

static void run(VkCommandBuffer commands, int pipeline, struct bake_state *b, VkDeviceAddress job, uint32_t p0,
	uint32_t p1, uint32_t p2, uint32_t p3, float v0, float v1, float v2, float v3, uint32_t groups)
{
	struct bake_push push;

	if (!groups)
		return;
	memset(&push, 0, sizeof(push));
	push.scene = b ? b->scene.address : 0;
	push.job = job;
	push.parameters[0] = p0;
	push.parameters[1] = p1;
	push.parameters[2] = p2;
	push.parameters[3] = p3;
	push.values[0] = v0;
	push.values[1] = v1;
	push.values[2] = v2;
	push.values[3] = v3;
	xgpu_vulkan_breadcrumb(commands, 60, "the bake's passes");
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, bake.pipelines[pipeline]);
	vkCmdPushConstants(commands, bake.pipeline_layouts[pipeline], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(commands, groups, 1, 1);
}

static VkDeviceAddress job_block(VkCommandBuffer commands, struct bake_state *b, uint32_t slot,
	const VkDeviceAddress *addresses, uint32_t count);

/* the running ones of count states (0 running) packed into compact_list,
and compact_args made their dispatch's groups (64 a group): then pipeline
run over them alone, its job block's slot `at` the list and the next the
arguments. A batch whose texels are mostly done no longer runs a group for
each of its few running ones' 64 slots: its running ones fill whole groups */
/* the samples a dispatch of items texels or probes takes: the budget's,
no more than SAMPLES_WORK_MOST paths in all */
static uint32_t samples_for(uint64_t items)
{
	uint64_t most = items ? SAMPLES_WORK_MOST / items : bake.samples;

	if (most < 1)
		most = 1;
	return bake.samples < most ? bake.samples : (uint32_t)most;
}

static void run_compacted(VkCommandBuffer commands, int pipeline, struct bake_state *b, VkDeviceAddress states,
	uint32_t count, VkDeviceAddress *addresses, uint32_t at, uint32_t slot, uint32_t p1, uint32_t p2, uint32_t p3, float v0,
	float v1, float v2, float v3, uint32_t lanes)
{
	static const uint32_t reset[4] = { 0u, 1u, 1u, 0u };
	VkDeviceAddress compact[3];
	VkDeviceAddress job, cjob;
	struct bake_push push;

	if (!count)
		return;
	compact[0] = states;
	compact[1] = b->compact_list.address;
	compact[2] = b->compact_args.address;
	cjob = job_block(commands, b, 7, compact, 3);
	addresses[at] = b->compact_list.address;
	addresses[at + 1] = b->compact_args.address;
	job = job_block(commands, b, slot, addresses, 8);
	if (!cjob || !job)
		return;
	vkCmdUpdateBuffer(commands, b->compact_args.buffer, 0, sizeof(reset), reset);
	barrier(commands);
	run(commands, PIPELINE_COMPACT, b, cjob, count, lanes, 0, 0, 0, 0, 0, 0, (count + 255) / 256);
	barrier(commands);
	memset(&push, 0, sizeof(push));
	push.scene = b->scene.address;
	push.job = job;
	push.parameters[0] = count;
	push.parameters[1] = p1;
	push.parameters[2] = p2 | (lanes > 1 ? lanes << 16 : 0u);
	push.parameters[3] = p3;
	push.values[0] = v0;
	push.values[1] = v1;
	push.values[2] = v2;
	push.values[3] = v3;
	xgpu_vulkan_breadcrumb(commands, 61, "the bake's texels");
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, bake.pipelines[pipeline]);
	vkCmdPushConstants(commands, bake.pipeline_layouts[pipeline], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatchIndirect(commands, b->compact_args.buffer, 0);
}

/* a job block (addresses, 8 of them) written for this frame's passes, at
a slot of the job buffer: its address (0 no room) */
static VkDeviceAddress job_block(VkCommandBuffer commands, struct bake_state *b, uint32_t slot,
	const VkDeviceAddress *addresses, uint32_t count)
{
	VkDeviceAddress words[8];

	memset(words, 0, sizeof(words));
	memcpy(words, addresses, count * sizeof(VkDeviceAddress));
	if (!upload(commands, &b->job, (VkDeviceSize)slot * 64, words, sizeof(words)))
		return 0;
	return b->job.address + (VkDeviceSize)slot * 64;
}

/* the final gather's cache block for the lightmap's and the volume's
passes (docs/prism_bake.md, "The estimator"): the bake's own light, read
where a bounce's path meets the level, once the draft has given every tile
its first light (every bounce in it); 0 while it drafts. Off unless
HALO_BAKE_GATHER (a diagnostic: it comes out a few percent over the paths
in dark texels, not yet explained) */
static VkDeviceAddress gather_cache(struct bake_state *b)
{
	static int on = -1;

	if (on < 0)
		on = getenv("HALO_BAKE_GATHER") != NULL && getenv("HALO_BAKE_NO_GATHER") == NULL;
	return !on || b->drafting || !b->cache_block.buffer ? 0 : b->cache_block.address;
}

/* the samples a texel or probe stops at whatever its noise (the quicker
tiers': section 3.6's counts; `max` none, D4) */
static float cap_of(struct bake_state *b)
{
	static const float caps[NUMBER_OF_PRISM_BAKE_QUALITIES] = { 64.0f, 512.0f, 2048.0f, MAX_SAMPLES_CAP };

	return b->quality < NUMBER_OF_PRISM_BAKE_QUALITIES ? caps[b->quality] : 0.0f;
}

/* the invocations a running texel's samples are spread over: one while
the texels running fill the GPU, then more as they become few (the
tail: the darkest texels, lit through narrow openings, whose samples a
single invocation each would take far longer to finish), a power of 2
up to SPREAD_MOST */
static uint32_t lanes_of(struct bake_state *b)
{
	uint32_t lanes = 1;

	if (b->drafting || !b->running_texels || getenv("HALO_BAKE_NO_SPREAD") ||
		!(bake.device.subgroup_compute & VK_SUBGROUP_FEATURE_SHUFFLE_BIT))
	{
		return 1;
	}
	/* (a texel's lanes in one subgroup, whatever size the driver gives the
	pass: no more than the device's fewest) */
	while (lanes < SPREAD_MOST && lanes * 2u <= bake.device.subgroup_least &&
		b->running_texels * lanes * 2u <= SPREAD_INVOCATIONS)
	{
		lanes <<= 1;
	}
	return lanes;
}

static float threshold_of(struct bake_state *b)
{
	return b->key.convergence_threshold > 0.0f ? b->key.convergence_threshold : 0.01f;
}

/* ---------- the importance grid */

static void importance_frame(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_plan *plan = &b->plan;
	uint32_t total = plan->cell_count * GRID_CANDIDATES, chunk, first;
	VkDeviceAddress job, addresses[3];

	if (!plan->cell_count || !b->lamp_count)
	{
		b->grid_ready = 1;
		return;
	}
	if (!b->cell_points.buffer)
	{
		if (!buffer_filled(commands, plan->cell_points, (VkDeviceSize)plan->cell_count * GRID_POINTS * 16, &b->cell_points,
			0) || !buffer_filled(commands, plan->cell_candidates, (VkDeviceSize)total * 4, &b->cell_candidates, 0) ||
			!buffer_make((VkDeviceSize)total * 4, 0, 0, &b->cell_weights))
		{
			bake_fail(b, "no memory for the importance grid");
			return;
		}
		barrier(commands);
		return;
	}
	if (b->cells_done < total)
	{
		/* (about 64 thousand candidates a frame, 4 points of each lamp) */
		chunk = bake.control.flags & (XGPU_BAKE_FLAG_FOREGROUND | XGPU_BAKE_FLAG_MENU) ? 262144u : 65536u;
		first = b->cells_done;
		if (chunk > total - first)
			chunk = total - first;
		addresses[0] = b->cell_points.address;
		addresses[1] = b->cell_candidates.address;
		addresses[2] = b->cell_weights.address;
		job = job_block(commands, b, 0, addresses, 3);
		if (!job)
			return;
		barrier(commands);
		run(commands, PIPELINE_IMPORTANCE, b, job, first + chunk, 4, GRID_CANDIDATES, first, 0, 0, 0, 0,
			(chunk + 63) / 64);
		b->cells_done += chunk;
		if (b->cells_done >= total)
		{
			struct bake_readback *r = readback_take((VkDeviceSize)total * 4, STEP_IMPORTANCE);

			barrier(commands);
			if (!r)
			{
				b->cells_done = first;
				return;
			}
			copy_region(commands, &b->cell_weights, 0, &r->buffer, 0, (VkDeviceSize)total * 4);
		}
	}
}

/* the weights read back: each cell's 16 most kept (with their cumulative
weights), the grid's table made and put on the GPU */
static void importance_finish(VkCommandBuffer commands, struct bake_state *b, const float *weights)
{
	struct bake_plan *plan = &b->plan;
	uint32_t entries = 1024, index, *table, kept_cells = 0, used, room;
	size_t table_words;

	while (entries < plan->cell_count * 2u)
		entries <<= 1;
	/* (the table, GRID_WORDS a cell: its key and its lists' first word; the
	lists after it, each cell's as long as its lamps: one word for the
	entries' count first, for the file) */
	table_words = (size_t)entries * GRID_WORDS;
	room = (uint32_t)(table_words + 1 + (size_t)plan->cell_count * 64 + 1024);
	table = (uint32_t *)calloc(room, 4);
	if (!table)
	{
		bake_fail(b, "no memory for the importance grid");
		return;
	}
	table[0] = entries;
	used = (uint32_t)(table_words + 1);
	for (index = 0; index < plan->cell_count; index++)
	{
		const float *w = weights + (size_t)index * GRID_CANDIDATES;
		const uint32_t *c = plan->cell_candidates + (size_t)index * GRID_CANDIDATES;
		uint32_t order[GRID_CANDIDATES], near_order[GRID_CANDIDATES], count = 0, near_count = 0, k, j, key0, key1, h, at;
		uint32_t cx, cy, cz, *list;
		float sum = 0.0f, near_w[GRID_CANDIDATES], middle[3];
		int axis;

		for (k = 0; k < GRID_CANDIDATES; k++)
			if (c[k] != 0xffffffffu && w[k] > 0.0f)
				order[count++] = k;
		/* (the most first) */
		for (k = 1; k < count; k++)
			for (j = k; j > 0 && w[order[j]] > w[order[j - 1]]; j--)
			{
				uint32_t swap = order[j];

				order[j] = order[j - 1];
				order[j - 1] = swap;
			}
		if (count > GRID_LIST)
			count = GRID_LIST;
		/* (the kept by their lamps' ids, for the GPU's binary searches) */
		for (k = 1; k < count; k++)
			for (j = k; j > 0 && c[order[j]] < c[order[j - 1]]; j--)
			{
				uint32_t swap = order[j];

				order[j] = order[j - 1];
				order[j - 1] = swap;
			}
		/* (the near: the candidates by their unshadowed light at the cell's
		middle, the most first: the lamps the cell's few points did not see
		that its other points may, the defensive part of the picks, so that
		every lamp near the cell can be picked whatever its points saw) */
		for (axis = 0; axis < 3; axis++)
			middle[axis] = ((float)plan->cell_keys[index * 3 + axis] + 0.5f) * GRID_CELL;
		for (k = 0; k < GRID_CANDIDATES; k++)
		{
			const float *lp;
			float d2 = 0.0f, centre;

			if (c[k] == 0xffffffffu || c[k] >= plan->lamp_count)
				continue;
			lp = plan->lamps + (size_t)c[k] * 16;
			for (axis = 0; axis < 3; axis++)
			{
				centre = (lp[axis] + lp[3 + axis] + lp[6 + axis]) / 3.0f;
				d2 += (middle[axis] - centre) * (middle[axis] - centre);
			}
			near_w[k] = (0.2126f * lp[9] + 0.7152f * lp[10] + 0.0722f * lp[11]) * lp[12] / (d2 + GRID_CELL * GRID_CELL);
			if (near_w[k] > 0.0f)
				near_order[near_count++] = k;
		}
		for (k = 1; k < near_count; k++)
			for (j = k; j > 0 && near_w[near_order[j]] > near_w[near_order[j - 1]]; j--)
			{
				uint32_t swap = near_order[j];

				near_order[j] = near_order[j - 1];
				near_order[j - 1] = swap;
			}
		if (near_count > GRID_NEAR)
			near_count = GRID_NEAR;
		for (k = 1; k < near_count; k++)
			for (j = k; j > 0 && c[near_order[j]] < c[near_order[j - 1]]; j--)
			{
				uint32_t swap = near_order[j];

				near_order[j] = near_order[j - 1];
				near_order[j - 1] = swap;
			}
		cx = (uint32_t)(plan->cell_keys[index * 3] + (1 << 20));
		cy = (uint32_t)(plan->cell_keys[index * 3 + 1] + (1 << 20));
		cz = (uint32_t)(plan->cell_keys[index * 3 + 2] + (1 << 20));
		key0 = cx | (cy << 21);
		key1 = (cy >> 11) | (cz << 10) | 0x80000000u;
		h = hash32(key0 ^ hash32(key1)) & (entries - 1);
		for (j = 0; j < 32; j++)
		{
			at = 1 + ((h + j) & (entries - 1)) * GRID_WORDS;
			if (!table[at + 1])
				break;
		}
		if (j == 32)
			continue;
		if (used + 2 + 2 * (count + near_count) > room)
		{
			uint32_t *grown;

			room = room * 2 + 2 + 2 * (count + near_count);
			grown = (uint32_t *)realloc(table, (size_t)room * 4);
			if (!grown)
			{
				free(table);
				bake_fail(b, "no memory for the importance grid");
				return;
			}
			table = grown;
		}
		/* (the lists' first word: the buffer's, which starts at the file's
		second) */
		table[at] = key0;
		table[at + 1] = key1;
		table[at + 2] = used - 1;
		list = table + used;
		/* (a cell none of whose candidates' light reaches its points is kept
		with no lamps seen: its points pick from its near list and by power) */
		list[0] = count;
		for (k = 0; k < count; k++)
		{
			sum += w[order[k]];
			list[1 + 2 * k] = c[order[k]];
			memcpy(&list[2 + 2 * k], &sum, 4);
		}
		list += 1 + 2 * count;
		sum = 0.0f;
		list[0] = near_count;
		for (k = 0; k < near_count; k++)
		{
			sum += near_w[near_order[k]];
			list[1 + 2 * k] = c[near_order[k]];
			memcpy(&list[2 + 2 * k], &sum, 4);
		}
		used += 2 + 2 * (count + near_count);
		kept_cells += count > 0;
	}
	if (!buffer_filled(commands, table + 1, (VkDeviceSize)(used - 1) * 4, &b->grid, 0))
	{
		free(table);
		bake_fail(b, "no memory for the importance grid");
		return;
	}
	file_append(b, PRISM_BAKE_RECORD_IMPORTANCE_GRID, prism_bake_job_importance_cell(255, 0, 0, 0), table,
		(size_t)used * 4 > PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD ? 0 : (size_t)used * 4);
	free(table);
	b->block.grid = b->grid.address;
	b->block.settings[2] = entries;
	b->grid_entries = entries;
	barrier(commands);
	scene_block_upload(commands, b);
	barrier(commands);
	b->grid_ready = 1;
	host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the light-importance grid: %u of %u cells with lamps seen (%u lamps; "
		"%.1f MB)", b->map, (int)b->bsp, kept_cells, plan->cell_count, b->lamp_count, (double)used * 4.0 / 1048576.0);
}

/* ---------- the lightmap's batches */

static void lightmap_frame(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_plan *plan = &b->plan;
	uint32_t slot, refilled = 0, pending_left = 0, entries[BATCH_TILES], entry_count = 0, index;
	static uint32_t next_pending;
	VkDeviceAddress job, addresses[8];
	struct bake_readback *r;
	float entry_words[8];
	uint32_t asked = 0, lanes;

	/* the free slots refilled with tiles still to do */
	if (b->frame == 0)
		next_pending = 0;
	for (slot = 0; slot < BATCH_TILES; slot++)
	{
		if (b->batch.tiles[slot] != 0xffffffffu)
			continue;
		while (next_pending < plan->tile_count && (plan->tiles[next_pending].state != TILE_PENDING ||
			(b->drafting && plan->tiles[next_pending].physical)))
			next_pending++;
		if (next_pending >= plan->tile_count)
			break;
		b->batch.tiles[slot] = next_pending;
		b->batch.asked[slot] = 0;
		plan->tiles[next_pending].state = TILE_BATCHED;
		entries[entry_count++] = slot;
		next_pending++;
		if (entry_count >= 1024)
			break;
	}
	for (index = next_pending; index < plan->tile_count && !pending_left; index++)
		pending_left = plan->tiles[index].state == TILE_PENDING;
	if (entry_count)
	{
		for (index = 0; index < entry_count; index++)
		{
			struct bake_tile const *tile = &plan->tiles[b->batch.tiles[entries[index]]];
			uint32_t words[3] = { entries[index], tile->triangle_first, tile->triangle_count };

			entry_words[0] = (float)tile->x * (float)TILE;
			entry_words[1] = (float)tile->y * (float)TILE;
			memcpy(&entry_words[2], words, 12);
			entry_words[5] = entry_words[6] = entry_words[7] = 0.0f;
			if (!upload(commands, &b->tile_origins, (VkDeviceSize)index * 32, entry_words, 32))
			{
				/* (no room: the rest next frame) */
				uint32_t k;

				for (k = index; k < entry_count; k++)
				{
					plan->tiles[b->batch.tiles[entries[k]]].state = TILE_PENDING;
					b->batch.tiles[entries[k]] = 0xffffffffu;
				}
				next_pending = 0;
				entry_count = index;
				break;
			}
			vkCmdFillBuffer(commands, b->running.buffer, (VkDeviceSize)entries[index] * 4, 4, 0u);
		}
		refilled = entry_count;
	}
	barrier(commands);
	if (refilled)
	{
		addresses[0] = b->tile_origins.address;
		addresses[1] = b->ranges.address;
		addresses[2] = b->triangles.address;
		addresses[3] = b->texels.address;
		addresses[4] = b->states.address;
		addresses[5] = b->accumulators.address;
		addresses[6] = b->running.address;
		job = job_block(commands, b, 1, addresses, 7);
		if (job)
		{
			run(commands, PIPELINE_SETUP, b, job, refilled, 0, 0, TILE_TEXELS, 0, 0, 0, 0,
				(refilled * TILE_TEXELS + 255) / 256);
			barrier(commands);
		}
	}
	/* the samples */
	addresses[0] = b->texels.address;
	addresses[1] = b->accumulators.address;
	addresses[2] = b->states.address;
	addresses[3] = b->running.address;
	addresses[6] = gather_cache(b);
	/* (every lamp of the cell's lists at a texel, past the draft) */
	addresses[7] = b->drafting ? 0 : 1;
	/* (tiles refilled: their texels' samples at the rate the running ones
	had, the dispatch's work as before) */
	if (refilled)
	{
		uint64_t before = b->running_texels ? b->running_texels : 1u;
		uint64_t after = before + (uint64_t)refilled * TILE_TEXELS;

		bake.samples = (uint32_t)((uint64_t)bake.samples * before / after);
		if (bake.samples < 1)
			bake.samples = 1;
	}
	/* (spread over lanes, each texel's samples are its lanes' shares: the
	paths a dispatch takes are its texels' lanes' samples) */
	lanes = lanes_of(b);
	run_compacted(commands, PIPELINE_LIGHTMAP, b, b->states.address, BATCH_TILES * TILE_TEXELS, addresses, 4, 2,
		samples_for(((uint64_t)b->running_texels + (uint64_t)refilled * TILE_TEXELS) * lanes), 64, b->frame,
		threshold_of(b), BLACK_FLOOR, b->drafting ? 64.0f : cap_of(b), (float)BOUNCES, lanes);
	b->frame++;
	barrier(commands);
	/* the readback: the running counts, and the asked slots' results */
	for (slot = 0; slot < BATCH_TILES; slot++)
		if (b->batch.tiles[slot] != 0xffffffffu && b->batch.asked[slot] == 1)
			asked++;
	if (asked > 512)
		asked = 512;
	r = readback_take(BATCH_TILES * 4 + (VkDeviceSize)(asked ? asked : 1) * TILE_TEXELS * (TEXEL_ACCUMULATOR_FLOATS * 4 + 4 +
		32),
		STEP_LIGHTMAP);
	if (!r)
		return;
	copy_region(commands, &b->running, 0, &r->buffer, 0, BATCH_TILES * 4);
	{
		VkDeviceSize at = BATCH_TILES * 4;

		for (slot = 0; slot < BATCH_TILES; slot++)
		{
			r->tiles[slot] = b->batch.tiles[slot];
			if (b->batch.tiles[slot] == 0xffffffffu || b->batch.asked[slot] != 1 || r->finished >= asked)
				continue;
			copy_region(commands, &b->accumulators, (VkDeviceSize)slot * TILE_TEXELS * TEXEL_ACCUMULATOR_FLOATS * 4,
				&r->buffer, at, TILE_TEXELS * TEXEL_ACCUMULATOR_FLOATS * 4);
			at += TILE_TEXELS * TEXEL_ACCUMULATOR_FLOATS * 4;
			copy_region(commands, &b->states, (VkDeviceSize)slot * TILE_TEXELS * 4, &r->buffer, at, TILE_TEXELS * 4);
			at += TILE_TEXELS * 4;
			copy_region(commands, &b->texels, (VkDeviceSize)slot * TILE_TEXELS * 32, &r->buffer, at, TILE_TEXELS * 32);
			at += TILE_TEXELS * 32;
			r->slots[r->finished++] = slot;
			b->batch.asked[slot] = 2;
		}
	}
	(void)pending_left;
}

/* a lightmap readback done: its finished slots' tiles written and drawn,
and the slots whose texels have all stopped asked for */
static void lightmap_collect(VkCommandBuffer commands, struct bake_state *b, struct bake_readback *r)
{
	const uint32_t *running = (const uint32_t *)r->buffer.mapped;
	const char *data = (const char *)r->buffer.mapped + BATCH_TILES * 4;
	uint32_t index, slot;

	for (index = 0; index < r->finished; index++)
	{
		const float *accumulators = (const float *)data;
		const uint32_t *states = (const uint32_t *)(data + TILE_TEXELS * TEXEL_ACCUMULATOR_FLOATS * 4);
		const float *records = (const float *)(data + TILE_TEXELS * (TEXEL_ACCUMULATOR_FLOATS * 4 + 4));

		slot = r->slots[index];
		if (b->batch.tiles[slot] == r->tiles[slot] && b->batch.tiles[slot] != 0xffffffffu)
		{
			tile_finish(commands, b, b->batch.tiles[slot], accumulators, states, records);
			b->batch.tiles[slot] = 0xffffffffu;
			b->batch.asked[slot] = 0;
		}
		data += TILE_TEXELS * (TEXEL_ACCUMULATOR_FLOATS * 4 + 4 + 32);
	}
	b->running_texels = 0;
	for (slot = 0; slot < BATCH_TILES; slot++)
	{
		if (r->tiles[slot] != 0xffffffffu)
			b->running_texels += running[slot];
		if (b->batch.tiles[slot] != 0xffffffffu && b->batch.tiles[slot] == r->tiles[slot] && !b->batch.asked[slot] &&
			running[slot] == 0)
			b->batch.asked[slot] = 1;
	}
}

/* ---------- the sun mask */

static void mask_frame(VkCommandBuffer commands, struct bake_state *b)
{
	uint32_t count = 0, index;
	VkDeviceAddress job, addresses[8];
	struct bake_readback *r;
	float entry_words[8];

	if (!b->mask_tiles)
	{
		b->mask_tiles = (uint32_t *)malloc(((size_t)b->plan.tile_count + 1) * 4);
		if (!b->mask_tiles)
		{
			bake_fail(b, "no memory for the sun mask");
			return;
		}
		for (index = 0; index < b->plan.tile_count; index++)
			if (b->plan.tiles[index].mask_state == 1)
				b->mask_tiles[b->mask_tile_count++] = index;
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the sun mask: %u of %u tiles", b->map, (int)b->bsp,
			b->mask_tile_count, b->plan.tile_count);
	}
	if (b->masks_batched >= b->mask_tile_count)
		return;
	for (index = 0; index < MASK_BATCH && b->masks_batched + index < b->mask_tile_count; index++)
	{
		struct bake_tile const *tile = &b->plan.tiles[b->mask_tiles[b->masks_batched + index]];
		uint32_t words[3] = { index, tile->triangle_first, tile->triangle_count };

		entry_words[0] = (float)tile->x * (float)TILE;
		entry_words[1] = (float)tile->y * (float)TILE;
		memcpy(&entry_words[2], words, 12);
		entry_words[5] = entry_words[6] = entry_words[7] = 0.0f;
		if (!upload(commands, &b->tile_origins, (VkDeviceSize)index * 32, entry_words, 32))
			break;
		count++;
	}
	if (!count)
		return;
	r = readback_take((VkDeviceSize)count * MASK_TEXELS, STEP_MASK);
	if (!r)
		return;
	vkCmdFillBuffer(commands, b->mask_out.buffer, 0, (VkDeviceSize)count * MASK_TEXELS, 0u);
	barrier(commands);
	addresses[0] = b->tile_origins.address;
	addresses[1] = b->ranges.address;
	addresses[2] = b->triangles.address;
	addresses[3] = b->mask_ranges.address;
	addresses[4] = b->states.address;
	addresses[5] = 0;
	addresses[6] = 0;
	job = job_block(commands, b, 3, addresses, 7);
	if (!job)
		return;
	run(commands, PIPELINE_SETUP, b, job, count, 0, 0, MASK_TEXELS, 0, 0, 0, 0, (count * MASK_TEXELS + 255) / 256);
	barrier(commands);
	addresses[0] = b->mask_ranges.address;
	addresses[1] = b->mask_out.address;
	job = job_block(commands, b, 4, addresses, 2);
	if (!job)
		return;
	run(commands, PIPELINE_MASK, b, job, count * MASK_TEXELS, 16, 256, b->frame++, 0, 0, 0, 0,
		(count * MASK_TEXELS + 63) / 64);
	barrier(commands);
	copy_region(commands, &b->mask_out, 0, &r->buffer, 0, (VkDeviceSize)count * MASK_TEXELS);
	for (index = 0; index < count; index++)
		r->tiles[index] = b->mask_tiles[b->masks_batched + index];
	r->finished = count;
	b->masks_batched += count;
}

static void mask_collect(VkCommandBuffer commands, struct bake_state *b, struct bake_readback *r)
{
	uint32_t index;

	for (index = 0; index < r->finished; index++)
	{
		struct bake_tile *tile = &b->plan.tiles[r->tiles[index]];
		const uint8_t *mask = (const uint8_t *)r->buffer.mapped + (size_t)index * MASK_TEXELS;

		if (tile->sun >= 3)
			upload(commands, &b->masks, (VkDeviceSize)(tile->sun - 3) * MASK_TEXELS, mask, MASK_TEXELS);
		file_append(b, PRISM_BAKE_RECORD_SUN_MASK_TILE, prism_bake_job_sun_mask_tile(tile->vpage, 0, tile->x, tile->y),
			mask, MASK_TEXELS);
		tile->mask_state = 2;
		b->masks_done++;
	}
}

/* the volume's brick of the plan's probes from index (BRICK_PROBES of
them): its cascade (0 fine, 1 coarse) and its place (in its bricks: world
units for the fine, COARSE_SPACING x 4 for the coarse) */
static void brick_of(const struct bake_plan *plan, uint32_t index, uint32_t *cascade, int32_t xyz[3])
{
	const float *p = plan->probes + (size_t)index * 4;
	float span = index >= plan->fine_count ? COARSE_SPACING * 4.0f : 1.0f;
	int axis;

	*cascade = index >= plan->fine_count ? 1u : 0u;
	for (axis = 0; axis < 3; axis++)
		xyz[axis] = (int32_t)floorf(p[axis] / span);
}

static uint64_t brick_job(const struct bake_plan *plan, uint32_t index)
{
	uint32_t cascade;
	int32_t xyz[3];

	brick_of(plan, index, &cascade, xyz);
	return prism_bake_job_volume_brick(cascade, (uint32_t)(xyz[0] + 32768), (uint32_t)(xyz[1] + 32768),
		(uint32_t)(xyz[2] + 32768));
}

/* ---------- the irradiance volume */

static float half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h >> 15) << 31, exponent = (h >> 10) & 31u, mantissa = h & 1023u, bits;
	float value;

	if (!exponent)
		value = (float)mantissa * (1.0f / 16777216.0f);
	else if (exponent == 31)
		value = 65504.0f;
	else
	{
		bits = ((exponent + 112u) << 23) | (mantissa << 13);
		memcpy(&value, &bits, 4);
	}
	return sign ? -value : value;
}

static uint16_t float_to_half(float f)
{
	uint32_t bits, sign, exponent, mantissa;

	memcpy(&bits, &f, 4);
	sign = (bits >> 16) & 0x8000u;
	exponent = (bits >> 23) & 0xffu;
	mantissa = bits & 0x7fffffu;
	if (exponent >= 143)
		return (uint16_t)(sign | 0x7bffu);
	if (exponent <= 112)
		return (uint16_t)sign;
	return (uint16_t)(sign | ((exponent - 112u) << 10) | (mantissa >> 13));
}


static void volume_frame(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_plan *plan = &b->plan;
	uint32_t count;
	VkDeviceAddress addresses[8];
	struct bake_readback *r;

	if (!plan->probe_count)
		return;
	if (!b->probe_points.buffer)
	{
		uint32_t entries = 1024, *table, index, brick_index, cascade;

		table = (uint32_t *)calloc(2, 4);
		b->probe_host = (float *)calloc((size_t)plan->probe_count, PROBE_WORDS * 4);
		if (!table || !b->probe_host ||
			!buffer_filled(commands, plan->probes, (VkDeviceSize)plan->probe_count * 16, &b->probe_points, 0) ||
			!buffer_make((VkDeviceSize)VOLUME_BATCH * ACCUMULATOR_FLOATS * 4, 0, 0, &b->probe_accumulators) ||
			!buffer_make((VkDeviceSize)VOLUME_BATCH * 4 + 16, 0, 0, &b->probe_states) ||
			!buffer_make((VkDeviceSize)VOLUME_BATCH * 36, 0, 0, &b->probe_statistics) ||
			!buffer_filled(commands, NULL, (VkDeviceSize)plan->probe_count * PROBE_WORDS * 4, &b->probes, 0))
		{
			free(table);
			bake_fail(b, "no memory for the irradiance volume");
			return;
		}
		/* (each cascade's bricks' table: each brick's key + 1 and its first
		probe) */
		for (cascade = 0; cascade < 2; cascade++)
		{
			uint32_t from = cascade ? plan->fine_count : 0, to = cascade ? plan->probe_count : plan->fine_count;

			entries = 1024;
			while (entries < (to - from) / BRICK_PROBES * 2u)
				entries <<= 1;
			free(table);
			table = (uint32_t *)calloc((size_t)entries * 2, 4);
			if (!table)
				break;
			for (brick_index = from / BRICK_PROBES; brick_index < to / BRICK_PROBES; brick_index++)
			{
				uint32_t which, key, h, probe;
				int32_t xyz[3];

				brick_of(plan, brick_index * BRICK_PROBES, &which, xyz);
				key = brick_key(xyz[0], xyz[1], xyz[2]);
				h = hash32(key ^ 0x68e31da4u) & (entries - 1);
				for (probe = 0; probe < 16; probe++)
				{
					index = ((h + probe) & (entries - 1)) * 2;
					if (!table[index])
					{
						table[index] = key + 1;
						table[index + 1] = brick_index * BRICK_PROBES;
						break;
					}
				}
			}
			if (!buffer_filled(commands, table, (VkDeviceSize)entries * 8, cascade ? &b->bricks_coarse : &b->bricks, 0))
				break;
			if (cascade)
				b->coarse_entries = to > from ? entries : 0;
			else
				b->brick_entries = entries;
		}
		if (cascade < 2)
		{
			free(table);
			bake_fail(b, "no memory for the irradiance volume");
			return;
		}
		free(table);
		prism_bake_batches_start(&b->volume, plan->probe_count, VOLUME_BATCH);
		barrier(commands);
		return;
	}
	if (b->volume.batched > b->volume.done)
	{
		uint32_t number = prism_bake_batches_counting(&b->volume);

		/* (the batch running: its count of running probes read back, until
		its results are asked for) */
		if (!number)
			return;
		r = readback_take(16, STEP_VOLUME);
		if (!r)
			return;
		r->batch = number;
		addresses[0] = b->probe_points.address + (VkDeviceSize)b->volume.done * 16;
		addresses[1] = b->probe_accumulators.address;
		addresses[2] = b->probe_states.address;
		addresses[3] = b->probe_statistics.address;
		addresses[4] = b->probe_states.address + (VkDeviceSize)VOLUME_BATCH * 4;
		addresses[7] = gather_cache(b);
		count = b->volume.batched - b->volume.done;
		run_compacted(commands, PIPELINE_VOLUME, b, b->probe_states.address, count, addresses, 5, 5,
			samples_for(count), 64,
			b->frame++, threshold_of(b), BLACK_FLOOR, cap_of(b), (float)BOUNCES, 1u);
		barrier(commands);
		copy_region(commands, &b->probe_states, (VkDeviceSize)VOLUME_BATCH * 4, &r->buffer, 0, 4);
		r->finished = 0;
		return;
	}
	/* the next batch begun (all its probes running: the samples from the
	start again) */
	count = prism_bake_batches_begin(&b->volume);
	if (!count)
		return;
	bake.samples = SAMPLES_FIRST;
	vkCmdFillBuffer(commands, b->probe_accumulators.buffer, 0, VK_WHOLE_SIZE, 0u);
	vkCmdFillBuffer(commands, b->probe_states.buffer, 0, (VkDeviceSize)VOLUME_BATCH * 4, 0u);
	vkCmdFillBuffer(commands, b->probe_states.buffer, (VkDeviceSize)VOLUME_BATCH * 4, 4, count);
	vkCmdFillBuffer(commands, b->probe_statistics.buffer, 0, VK_WHOLE_SIZE, 0u);
	barrier(commands);
}

/* the running probes' count read: the batch's results asked for where it is
0 (once: several counts of a batch are in flight while the game draws), or
its results written (each brick's record, the raster's probes) */
static void volume_collect(VkCommandBuffer commands, struct bake_state *b, struct bake_readback *r)
{
	struct bake_plan *plan = &b->plan;
	uint32_t count, index;

	if (r->finished == 0)
	{
		uint32_t running;
		struct bake_readback *data;

		memcpy(&running, r->buffer.mapped, 4);
		count = prism_bake_batches_counted(&b->volume, r->batch, running);
		if (!count)
			return;
		data = readback_take((VkDeviceSize)count * (ACCUMULATOR_FLOATS * 4 + 4 + 36), STEP_VOLUME);
		if (!data)
		{
			/* (asked again by the next count) */
			b->volume.asked = 0;
			return;
		}
		data->batch = r->batch;
		copy_region(commands, &b->probe_accumulators, 0, &data->buffer, 0, (VkDeviceSize)count * ACCUMULATOR_FLOATS * 4);
		copy_region(commands, &b->probe_states, 0, &data->buffer, (VkDeviceSize)count * ACCUMULATOR_FLOATS * 4,
			(VkDeviceSize)count * 4);
		copy_region(commands, &b->probe_statistics, 0, &data->buffer, (VkDeviceSize)count * (ACCUMULATOR_FLOATS * 4 + 4),
			(VkDeviceSize)count * 36);
		data->finished = count;
		/* (no more samples meanwhile: the batch is whole) */
		return;
	}
	{
		const float *accumulators = (const float *)r->buffer.mapped;
		const uint32_t *states = (const uint32_t *)((const char *)r->buffer.mapped +
			(size_t)r->finished * ACCUMULATOR_FLOATS * 4);
		const uint32_t *statistics = (const uint32_t *)((const char *)r->buffer.mapped +
			(size_t)r->finished * (ACCUMULATOR_FLOATS * 4 + 4));
		uint32_t first, words[BRICK_PROBES * PROBE_WORDS], k;
		int64_t at = prism_bake_batches_finished(&b->volume, r->batch, r->finished);

		if (at < 0)
			return;
		first = (uint32_t)at;
		for (index = 0; index < r->finished; index++)
		{
			const float *a = accumulators + (size_t)index * ACCUMULATOR_FLOATS;
			uint32_t *to = (uint32_t *)(b->probe_host + (size_t)(first + index) * PROBE_WORDS);
			float valid = states[index] == 1 || states[index] == 2 ? 1.0f : 0.0f;

			for (k = 0; k < 6; k++)
				to[k] = (uint32_t)float_to_half(a[k * 2]) | ((uint32_t)float_to_half(a[k * 2 + 1]) << 16);
			memcpy(&to[6], &a[12], 4);
			memcpy(&to[7], &valid, 4);
			b->rays += statistics[index * 9];
		}
		/* (the bricks' records, as baked, and their layers (each probe's
		shares of its SH L0); then the light drawn recombined by the weights
		now) */
		for (index = 0; index + BRICK_PROBES <= r->finished; index += BRICK_PROBES)
		{
			uint8_t layers[VOLUME_LAYER_BYTES];
			uint32_t *at = (uint32_t *)(b->probe_host + (size_t)(first + index) * PROBE_WORDS);

			for (k = 0; k < BRICK_PROBES; k++)
			{
				const float *a = accumulators + (size_t)(index + k) * ACCUMULATOR_FLOATS;
				float total[3], shares[PRISM_BAKE_LAYER_SHARES][3];
				int layer, channel;

				for (channel = 0; channel < 3; channel++)
				{
					total[channel] = a[channel * 4];
					for (layer = 0; layer < PRISM_BAKE_LAYER_SHARES; layer++)
						shares[layer][channel] = a[14 + layer * 3 + channel];
				}
				prism_bake_layer_shares_put(total, (const float (*)[3])shares, a[26] < 0.5f, layers, k, BRICK_PROBES);
			}
			memcpy(words, at, sizeof(words));
			file_append(b, PRISM_BAKE_RECORD_VOLUME_BRICK, brick_job(plan, first + index), words, sizeof(words));
			file_append(b, PRISM_BAKE_RECORD_VOLUME_LAYERS, brick_job(plan, first + index), layers, sizeof(layers));
			prism_bake_layers_probes_mean(&b->weights, words, layers, BRICK_PROBES, b->volume_ratio_sum,
				b->volume_ratio_weight);
			prism_bake_layers_probes(&b->weights, words, layers, BRICK_PROBES, at);
		}
		upload(commands, &b->probes, (VkDeviceSize)first * PROBE_WORDS * 4, b->probe_host + (size_t)first * PROBE_WORDS,
			r->finished * PROBE_WORDS * 4 > STAGING_BYTES ? 0 : r->finished * PROBE_WORDS * 4);
		(void)half_to_float;
	}
}

/* ---------- the reflection probes */

/* the reflection probes' ratio a channel (the layered bake: the volume's
light-weighted mean ratio by the weights now; 1 where it has no light) */
static void reflection_ratio(struct bake_state *b, float ratio[3])
{
	int k;

	for (k = 0; k < 3; k++)
		ratio[k] = prism_bake_layers_identity(&b->weights) || !(b->volume_ratio_weight[k] > 0.0) ? 1.0f :
			(float)(b->volume_ratio_sum[k] / b->volume_ratio_weight[k]);
}

/* the samples a reflection probe's texel takes (by the quality) */
static uint32_t reflection_samples(struct bake_state *b)
{
	return b->quality == PRISM_BAKE_QUALITY_DRAFT ? 16u : b->quality == PRISM_BAKE_QUALITY_HIGH ? 64u : 256u;
}

/* the probes' buffers and hashed grid made (each REFLECTION_CELL cell
within a cell of a probe's: its key + 1, 0, its nearest REFLECTION_LIST
probes, none 0xffffffff); FALSE if no memory */
static int reflections_make(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_plan *plan = &b->plan;
	uint32_t entries = 1024, *table, index, cells = plan->reflection_count * 27u;
	int ok;

	while (entries < cells * 2u)
		entries <<= 1;
	table = (uint32_t *)calloc((size_t)entries * 10, 4);
	if (!table)
		return 0;
	for (index = 0; index < plan->reflection_count; index++)
	{
		const float *p = plan->reflections + (size_t)index * 4;
		int32_t c0[3], dx, dy, dz;
		int axis;

		for (axis = 0; axis < 3; axis++)
			c0[axis] = (int32_t)floorf(p[axis] / REFLECTION_CELL);
		for (dz = -1; dz <= 1; dz++)
			for (dy = -1; dy <= 1; dy++)
				for (dx = -1; dx <= 1; dx++)
				{
					int32_t c[3] = { c0[0] + dx, c0[1] + dy, c0[2] + dz };
					uint32_t key = brick_key(c[0], c[1], c[2]), h = hash32(key ^ 0x2c1b3c6du) & (entries - 1);
					uint32_t *at = NULL, k, probe;
					float middle[3], d, worst = -1.0f;
					int replace = -1;

					for (probe = 0; probe < 32; probe++)
					{
						uint32_t *e = table + (size_t)((h + probe) & (entries - 1)) * 10;

						if (!e[0] || e[0] == key + 1u)
						{
							at = e;
							break;
						}
					}
					if (!at)
						continue;
					if (!at[0])
					{
						at[0] = key + 1u;
						for (k = 0; k < REFLECTION_LIST; k++)
							at[2 + k] = 0xffffffffu;
					}
					for (axis = 0; axis < 3; axis++)
						middle[axis] = ((float)c[axis] + 0.5f) * REFLECTION_CELL;
					d = (p[0] - middle[0]) * (p[0] - middle[0]) + (p[1] - middle[1]) * (p[1] - middle[1]) +
						(p[2] - middle[2]) * (p[2] - middle[2]);
					/* (the nearest kept: a free place, else the farthest replaced) */
					for (k = 0; k < REFLECTION_LIST; k++)
					{
						float dk;
						const float *q;

						if (at[2 + k] == 0xffffffffu)
						{
							replace = (int)k;
							worst = 1e30f;
							break;
						}
						q = plan->reflections + (size_t)at[2 + k] * 4;
						dk = (q[0] - middle[0]) * (q[0] - middle[0]) + (q[1] - middle[1]) * (q[1] - middle[1]) +
							(q[2] - middle[2]) * (q[2] - middle[2]);
						if (dk > worst)
						{
							worst = dk;
							replace = (int)k;
						}
					}
					if (replace >= 0 && d < worst)
						at[2 + replace] = index;
				}
	}
	ok = buffer_filled(commands, plan->reflections, (VkDeviceSize)plan->reflection_count * 16 + 16, &b->reflection_points,
		0) && buffer_filled(commands, NULL, (VkDeviceSize)plan->reflection_count * 48 + 48, &b->reflection_info, 0) &&
		buffer_filled(commands, NULL, (VkDeviceSize)plan->reflection_count * REFLECTION_TEXELS * 4 + 16,
		&b->reflection_data, 0) && buffer_filled(commands, table, (VkDeviceSize)entries * 40, &b->reflection_table, 0);
	free(table);
	b->reflection_entries = entries;
	barrier(commands);
	return ok;
}

/* the probes the file has, drawn (a frame's staging's worth at a time):
how many more */
static uint32_t reflections_load(VkCommandBuffer commands, struct bake_state *b)
{
	uint32_t loaded = 0, words = 12 + REFLECTION_TEXELS;
	uint32_t *payload = (uint32_t *)malloc((size_t)words * 4);

	while (payload && b->cache.record_count && b->reflections_done < b->plan.reflection_count && loaded < 256)
	{
		uint64_t job = prism_bake_job_probe(1, b->reflections_done);
		struct prism_bake_record const *record;

		if (!prism_bake_cache_job_done(&b->cache, PRISM_BAKE_RECORD_PROBE, job))
			break;
		record = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_PROBE, job);
		if (!record || record->size != (size_t)words * 4 || prism_bake_cache_read(&b->cache, record, payload, (size_t)words * 4))
			break;
		{
			/* (the BSP's ratio a channel, the volume's light-weighted mean) */
			float ratio[3];

			reflection_ratio(b, ratio);
			prism_bake_layers_scale(payload + 12, REFLECTION_TEXELS, ratio);
		}
		if (!upload(commands, &b->reflection_info, (VkDeviceSize)b->reflections_done * 48, payload, 48) ||
			!upload(commands, &b->reflection_data, (VkDeviceSize)b->reflections_done * REFLECTION_TEXELS * 4, payload + 12,
			REFLECTION_TEXELS * 4))
			break;
		b->reflections_done++;
		b->reflections_batched = b->reflections_done;
		loaded++;
	}
	free(payload);
	return loaded;
}

/* a batch of probes baked: their boxes, then their texels, then read back */
static void reflections_frame(VkCommandBuffer commands, struct bake_state *b)
{
	uint32_t first = b->reflections_done, count = b->plan.reflection_count - first, index;
	VkDeviceAddress addresses[3], job;
	struct bake_readback *r;

	if (b->reflections_batched > b->reflections_done || !count)
		return;
	if (count > REFLECTION_BATCH)
		count = REFLECTION_BATCH;
	r = readback_take((VkDeviceSize)count * (48 + REFLECTION_TEXELS * 4), STEP_REFLECTIONS);
	if (!r)
		return;
	addresses[0] = b->reflection_points.address;
	addresses[1] = b->reflection_info.address;
	addresses[2] = b->reflection_data.address;
	job = job_block(commands, b, 6, addresses, 3);
	if (!job)
	{
		r->busy = 0;
		return;
	}
	run(commands, PIPELINE_REFLECTION, b, job, first, count, 0, b->frame, 0.0f, (float)REFLECTION_SIDE,
		(float)REFLECTION_LEVELS, (float)BOUNCES, (count + 63) / 64);
	barrier(commands);
	run(commands, PIPELINE_REFLECTION, b, job, first, count, reflection_samples(b), b->frame++, 1.0f,
		(float)REFLECTION_SIDE, (float)REFLECTION_LEVELS, (float)BOUNCES, (count * REFLECTION_TEXELS + 63) / 64);
	barrier(commands);
	for (index = 0; index < count; index++)
	{
		copy_region(commands, &b->reflection_info, (VkDeviceSize)(first + index) * 48, &r->buffer,
			(VkDeviceSize)index * (48 + REFLECTION_TEXELS * 4), 48);
		copy_region(commands, &b->reflection_data, (VkDeviceSize)(first + index) * REFLECTION_TEXELS * 4, &r->buffer,
			(VkDeviceSize)index * (48 + REFLECTION_TEXELS * 4) + 48, REFLECTION_TEXELS * 4);
		r->tiles[index] = first + index;
	}
	r->finished = count;
	b->reflections_batched = first + count;
}

static void reflections_collect(VkCommandBuffer commands, struct bake_state *b, struct bake_readback *r)
{
	uint32_t index;
	float ratio[3];

	reflection_ratio(b, ratio);
	for (index = 0; index < r->finished; index++)
	{
		const uint8_t *payload = (const uint8_t *)r->buffer.mapped + (size_t)index * (48 + REFLECTION_TEXELS * 4);

		file_append(b, PRISM_BAKE_RECORD_PROBE, prism_bake_job_probe(1, r->tiles[index]), payload,
			48 + REFLECTION_TEXELS * 4);
		b->rays += (uint64_t)REFLECTION_TEXELS * reflection_samples(b);
		/* (the cube drawn by the weights now, where they are not the bake's) */
		if (!prism_bake_layers_identity(&b->weights))
		{
			uint32_t *texels = (uint32_t *)malloc(REFLECTION_TEXELS * 4);

			if (texels)
			{
				memcpy(texels, payload + 48, REFLECTION_TEXELS * 4);
				prism_bake_layers_scale(texels, REFLECTION_TEXELS, ratio);
				upload(commands, &b->reflection_data, (VkDeviceSize)r->tiles[index] * REFLECTION_TEXELS * 4, texels,
					REFLECTION_TEXELS * 4);
				free(texels);
			}
		}
	}
	b->reflections_done = b->reflections_batched;
}

/* ---------- the footprints: each switchable light's tiles in its reach
(design section 2.4: what the lightmap-space relight of a fixture switched
on or off redoes): each light's id, its tiles' count and the tiles (their
indices in the plan), the tiles whose triangles' box meets its sphere */

static void footprints_make(struct bake_state *b)
{
	const struct xgpu_stream_scene_light *lights;
	uint32_t count = 0, serial, index, tile, *payload, words = 1, kept = 0, capacity;
	struct bake_plan *plan = &b->plan;
	float *boxes;

	lights = xgpu_vulkan_scene_light_table(&count, &serial);
	capacity = plan->tile_count * 4u + 4096u;
	boxes = (float *)malloc(((size_t)plan->tile_count + 1) * 6 * sizeof(float));
	payload = (uint32_t *)malloc((size_t)capacity * 4);
	if (!boxes || !payload)
	{
		free(boxes);
		free(payload);
		return;
	}
	for (tile = 0; tile < plan->tile_count; tile++)
	{
		float *box = boxes + (size_t)tile * 6;
		uint32_t k, c, axis;

		box[0] = box[1] = box[2] = 1e30f;
		box[3] = box[4] = box[5] = -1e30f;
		for (k = 0; k < plan->tiles[tile].triangle_count; k++)
		{
			const float *p = plan->positions + (size_t)plan->atlas_scene[plan->tile_triangles[plan->tiles[tile].triangle_first +
				k]] * 9;

			for (c = 0; c < 3; c++)
				for (axis = 0; axis < 3; axis++)
				{
					box[axis] = fminf(box[axis], p[c * 3 + axis]);
					box[3 + axis] = fmaxf(box[3 + axis], p[c * 3 + axis]);
				}
		}
	}
	for (index = 0; lights && index < count; index++)
	{
		const struct xgpu_stream_scene_light *light = &lights[index];
		float radius = light->position[3] > 0.0f ? light->position[3] : 8.0f;
		uint32_t head = words, axis;

		if (light->light_class != XGPU_SCENE_LIGHT_CLASS_SWITCHABLE || words + 2 >= capacity)
			continue;
		payload[words++] = light->id;
		payload[words++] = 0;
		for (tile = 0; tile < plan->tile_count && words < capacity; tile++)
		{
			const float *box = boxes + (size_t)tile * 6;
			float d2 = 0.0f;

			for (axis = 0; axis < 3; axis++)
			{
				float c = light->position[axis] < box[axis] ? box[axis] : light->position[axis] > box[3 + axis] ?
					box[3 + axis] : light->position[axis];

				d2 += (c - light->position[axis]) * (c - light->position[axis]);
			}
			if (d2 <= radius * radius)
			{
				payload[words++] = tile;
				payload[head + 1]++;
			}
		}
		kept++;
	}
	payload[0] = kept;
	file_append(b, PRISM_BAKE_RECORD_FOOTPRINTS, prism_bake_job_probe(255, 0), payload, (size_t)words * 4);
	host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: %u switchable lights' footprints", b->map, (int)b->bsp, kept);
	free(boxes);
	free(payload);
}

/* ---------- a bake's file opened: resumed, or loaded whole */

/* the plan's job ids (plan 1: the tiles, the grid, the bricks, the
footprints; with masks: their tiles too) */
static int plan_write(struct bake_state *b, int with_masks)
{
	struct bake_plan *plan = &b->plan;
	size_t count = (size_t)plan->tile_count + 2 + plan->probe_count / BRICK_PROBES + plan->reflection_count +
		(with_masks ? plan->tile_count : 0), at = 0, index;
	uint64_t *jobs = (uint64_t *)malloc((count + 1) * sizeof(uint64_t));
	int result;

	if (!jobs)
		return -1;
	for (index = 0; index < plan->tile_count; index++)
		jobs[at++] = plan->tiles[index].job;
	jobs[at++] = prism_bake_job_importance_cell(255, 0, 0, 0);
	jobs[at++] = prism_bake_job_probe(255, 0);
	for (index = 0; index + BRICK_PROBES <= plan->probe_count; index += BRICK_PROBES)
		jobs[at++] = brick_job(plan, (uint32_t)index);
	for (index = 0; index < plan->reflection_count; index++)
		jobs[at++] = prism_bake_job_probe(1, index);
	for (index = 0; with_masks && index < plan->tile_count && at < count; index++)
	{
		struct bake_tile const *tile = &plan->tiles[index];

		if (tile->mask_state >= 1)
			jobs[at++] = prism_bake_job_sun_mask_tile(tile->vpage, 0, tile->x, tile->y);
	}
	result = b->writing ? prism_bake_cache_append_plan(&b->writer, jobs, at) : 0;
	free(jobs);
	return result;
}

/* the layers' base the file has (the light it was baked with), where it has
one: the light the weights are of. 1 where read */
static int base_read(struct bake_state *b, struct prism_bake_cache *cache)
{
	struct prism_bake_record const *record = prism_bake_cache_find(cache, PRISM_BAKE_RECORD_LAYER_BASE,
		PRISM_BAKE_JOB_NONE);
	uint32_t words[PRISM_BAKE_LAYER_BASE_WORDS];
	struct prism_bake_layer_light base;

	if (!record || record->size != sizeof(words) || prism_bake_cache_read(cache, record, words, sizeof(words)) ||
		prism_bake_layer_base_read(words, PRISM_BAKE_LAYER_BASE_WORDS, &base))
		return 0;
	b->base = base;
	b->base_known = 1;
	b->saw_remodel |= !base.stock;
	return 1;
}

/* whether two lights' colours and powers differ */
static int layer_light_differs(const struct prism_bake_layer_light *a, const struct prism_bake_layer_light *b)
{
	int k;

	for (k = 0; k < 3; k++)
		if (fabsf(a->sun[k] - b->sun[k]) > 1.0e-4f * (fabsf(b->sun[k]) + 1.0e-4f) ||
			fabsf(a->sky[k] - b->sky[k]) > 1.0e-4f * (fabsf(b->sky[k]) + 1.0e-4f) ||
			fabsf(a->domes[k] - b->domes[k]) > 1.0e-4f * (fabsf(b->domes[k]) + 1.0e-4f))
			return 1;
	return fabsf(a->emission - b->emission) > 1.0e-4f * (fabsf(b->emission) + 1.0e-4f) ||
		fabsf(a->fixtures - b->fixtures) > 1.0e-4f || fabsf(a->emissive_scale - b->emissive_scale) > 1.0e-4f;
}

/* the layers' base written (the bake's light, and whether it is of the
stock world so far) */
static void base_write(struct bake_state *b)
{
	uint32_t words[PRISM_BAKE_LAYER_BASE_WORDS];

	b->base.stock = !b->saw_remodel;
	prism_bake_layer_base_write(&b->base, words);
	file_append(b, PRISM_BAKE_RECORD_LAYER_BASE, PRISM_BAKE_JOB_NONE, words, sizeof(words));
}

/* the file: a whole bake of this key loaded (DONE), the log of an unfinished
one resumed, or a new one made; where no bake is wanted (wanted 0), nothing
made or resumed */
/* a whole bake at path that is good for this one: of its key, or of the
same inputs at a better quality (a release's shipped MAX bake for an
ultra one; the key's settings as that quality has them). 1 where it is,
loaded into b->cache, b->key then the file's; else 0, b->cache closed, the
fields that differed in *differences (0 no file or not a bake) */
static int file_whole(struct bake_state *b, const char *path, uint32_t *differences)
{
	enum prism_bake_cache_status status = prism_bake_cache_open(&b->cache, path, &b->key, 0);
	struct prism_bake_cache_key better;

	*differences = status == PRISM_BAKE_CACHE_STALE ? b->cache.differences : 0;
	if (status == PRISM_BAKE_CACHE_CURRENT && b->cache.done)
	{
		b->loaded_quality = b->key.quality;
		return 1;
	}
	if (status == PRISM_BAKE_CACHE_STALE && b->cache.done && b->cache.key.quality > b->key.quality &&
		b->cache.key.quality <= PRISM_BAKE_QUALITY_MAX &&
		!(b->cache.differences & ~(uint32_t)(PRISM_BAKE_KEY_QUALITY | PRISM_BAKE_KEY_SETTINGS)))
	{
		better = b->key;
		better.quality = b->cache.key.quality;
		better.settings_hash = key_settings_hash(b, better.quality);
		if (prism_bake_key_compare(&better, &b->cache.key) == 0)
		{
			prism_bake_cache_close(&b->cache);
			if (prism_bake_cache_open(&b->cache, path, &better, 0) == PRISM_BAKE_CACHE_CURRENT && b->cache.done)
			{
				b->key = better;
				b->loaded_quality = better.quality;
				return 1;
			}
			*differences = b->cache.differences;
		}
	}
	prism_bake_cache_close(&b->cache);
	memset(&b->cache, 0, sizeof(b->cache));
	return 0;
}

static const char *const quality_names[] = { "draft", "high", "ultra", "max" };

/* the shipped bake's note beside the BSP's local bake (<folder>/<map>/
shipped.txt, a line a BSP and world: "bsp<n> <stock|remodel> used <quality>"
or "bsp<n> <stock|remodel> mismatch <the key's fields that differ, hex>"),
which the main menu reads (prism_bake_menu.c): a BSP whose shipped bake
does not match this machine's maps (a mod, a custom map, a remodel built
otherwise) is baked here, that BSP alone */
static void shipped_note(struct bake_state *b, const char *world, const char *state, uint32_t value)
{
	char path[XGPU_BAKE_PATH + 200], temporary[XGPU_BAKE_PATH + 208], name[96], line[256], prefix[64];
	char text[8192];
	size_t length = 0;
	FILE *file;

	name_clean(b->map, name, sizeof(name));
	snprintf(path, sizeof(path), "%s/%s/shipped.txt", b->folder, name);
	snprintf(prefix, sizeof(prefix), "bsp%d %s ", (int)b->bsp, world);
	text[0] = 0;
	if ((file = fopen(path, "rb")) != NULL)
	{
		while (fgets(line, sizeof(line), file))
		{
			size_t size = strlen(line);

			if (!strncmp(line, prefix, strlen(prefix)) || size == 0 || line[size - 1] != '\n' ||
				length + size + 1 >= sizeof(text))
				continue;
			memcpy(text + length, line, size + 1);
			length += size;
		}
		fclose(file);
	}
	if (!strcmp(state, "used"))
		snprintf(line, sizeof(line), "%s%s %s\n", prefix, state, value < 4 ? quality_names[value] : "?");
	else
		snprintf(line, sizeof(line), "%s%s 0x%x\n", prefix, state, value);
	if (length + strlen(line) + 1 < sizeof(text))
	{
		memcpy(text + length, line, strlen(line) + 1);
		length += strlen(line);
	}
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	if ((file = fopen(temporary, "wb")) == NULL)
		return;
	if (fwrite(text, 1, length, file) != length)
	{
		fclose(file);
		remove(temporary);
		return;
	}
	fclose(file);
	if (rename(temporary, path) != 0)
	{
		remove(path);
		if (rename(temporary, path) != 0)
			remove(temporary);
	}
}

/* before the plan: a whole bake that is good for this one, the world
drawn's shipped one first (a release's, read only: wherever its key is
this BSP's (the player's maps, their geometry, materials and lights: the
per-BSP input hashes) it is drawn, nothing baked), else this machine's;
loaded into b->cache, b->chosen 1 or 2, the plan then made at its quality
(a better quality's plan is finer: a lesser plan's tiles are not the
file's). Else 0, and this BSP alone is baked here as before */
static void file_choose(struct bake_state *b)
{
	uint32_t differences;
	const char *shipped = bake.control.shipped ? (const char *)host_pointer(bake.control.shipped) : NULL;

	b->chosen = 0;
	if (shipped && *shipped)
	{
		char path[XGPU_BAKE_PATH + 200], name[96];
		const char *world = (bake.control.flags & XGPU_BAKE_FLAG_REMODEL) ? "remodel" : "stock";
		struct stat info;

		name_clean(b->map, name, sizeof(name));
		snprintf(path, sizeof(path), "%s/%s/%s/bsp%d.pbk", shipped, world, name, (int)b->bsp);
		if (stat(path, &info) == 0)
		{
			if (file_whole(b, path, &differences))
			{
				snprintf(b->path, sizeof(b->path), "%s", path);
				b->shipped = 1;
				b->chosen = 1;
				shipped_note(b, world, "used", b->loaded_quality);
			}
			else
			{
				host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the shipped bake (%s) is not this machine's BSP (the "
					"key's fields 0x%x differ: 1 the map, 2 the BSP, 4 the geometry, 8 the materials, 16 the lights, 32 "
					"the settings, 64 the baker, 128 the quality): this BSP's own bake used or made", b->map, (int)b->bsp,
					path, differences);
				shipped_note(b, world, "mismatch", differences);
			}
		}
	}
	if (!b->chosen && file_whole(b, b->path, &differences))
		b->chosen = 2;
	if (b->chosen)
		b->plan.quality = (int)b->loaded_quality;
}

static void file_open(struct bake_state *b, int wanted)
{
	enum prism_bake_cache_status status;
	uint32_t differences;

	if (b->chosen == 1)
	{
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the shipped bake (%s, %s) is this BSP's: loaded, nothing "
			"baked (at %.3f s)%s", b->map, (int)b->bsp, b->path, quality_names[b->loaded_quality & 3u], now_seconds(),
			base_read(b, &b->cache) ? "" : ", no layers' base: drawn as baked");
		b->writing = 0;
		b->complete = 1;
		return;
	}
	if (b->chosen == 2)
	{
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the bake is whole (%s%s%s): loaded (at %.3f s)%s", b->map,
			(int)b->bsp, b->path, b->loaded_quality != b->quality ? ", " : "",
			b->loaded_quality != b->quality ? quality_names[b->loaded_quality & 3u] : "", now_seconds(),
			base_read(b, &b->cache) ? "" : ", no layers' base: drawn as baked");
		b->writing = 0;
		b->complete = 1;
		return;
	}
	status = prism_bake_cache_open(&b->cache, b->path, &b->key, 0);
	/* (the differences read before the cache is let go: closing clears it) */
	differences = b->cache.differences;
	prism_bake_cache_close(&b->cache);
	if (!wanted)
	{
		/* (display.bake "off", or "ask" and not asked: no file made or
		resumed, nothing baked; an unfinished log of this key drawn as far as
		it goes) */
		b->idle = 1;
		b->writing = 0;
		if (prism_bake_cache_open(&b->cache, b->new_path, &b->key, 0) != PRISM_BAKE_CACHE_CURRENT)
		{
			prism_bake_cache_close(&b->cache);
			memset(&b->cache, 0, sizeof(b->cache));
		}
		else
			base_read(b, &b->cache);
		if (status == PRISM_BAKE_CACHE_STALE)
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the bake there is of something else (differences 0x%x): "
				"not baked (display.bake %s)", b->map, (int)b->bsp, differences,
				bake.control.mode == XGPU_BAKE_MODE_OFF ? "\"off\"" : "\"ask\", not asked");
		else
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: no whole bake: not baked (display.bake %s)%s", b->map,
				(int)b->bsp, bake.control.mode == XGPU_BAKE_MODE_OFF ? "\"off\"" : "\"ask\", not asked",
				b->cache.record_count ? ", its unfinished log drawn" : "");
		return;
	}
	if (status == PRISM_BAKE_CACHE_STALE)
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the bake there is of something else (differences 0x%x): "
			"baked again", b->map, (int)b->bsp, differences);
	if (prism_bake_cache_resume(&b->writer, &b->cache, b->new_path, &b->key, 0) == 0)
	{
		struct prism_bake_layer_light session = b->base;

		/* (the base the log was begun with: its transport is the key's, but a
		log begun with other colours (debug.prism_rig_scales, the fitted rig)
		is begun again, so that its texels' layers are all of one light) */
		if (base_read(b, &b->cache) && layer_light_differs(&session, &b->base))
		{
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: its log was begun with other lights' colours: begun again",
				b->map, (int)b->bsp);
			prism_bake_cache_writer_close(&b->writer);
			prism_bake_cache_close(&b->cache);
			memset(&b->cache, 0, sizeof(b->cache));
			b->base = session;
			remove(b->new_path);
		}
		else
		{
			b->writing = 1;
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: resumed (%lu records%s)", b->map, (int)b->bsp,
				(unsigned long)b->cache.record_count, b->cache.torn ? ", its torn last cut" : "");
			if (b->cache.plan_generation == 0)
				plan_write(b, 0);
			if (!b->base_known || !prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_LAYER_BASE, PRISM_BAKE_JOB_NONE))
				base_write(b);
			return;
		}
	}
	else
	{
		prism_bake_cache_close(&b->cache);
		memset(&b->cache, 0, sizeof(b->cache));
	}
	if (prism_bake_cache_create(&b->writer, b->new_path, &b->key, 0) == 0)
	{
		b->writing = 1;
		plan_write(b, 0);
		base_write(b);
	}
	else
		host_logf(HOST_LOG_WARN, "prism bake: %s cannot be made (%s): the bake is drawn unsaved", b->new_path,
			strerror(errno));
}

/* the finished log made the bake file: its live records alone (each job's
last result, the newest plan; not the drafts' checkpoints, nor a result a
later one replaced) copied into <bsp>.pbk, the log let go of: 0, or -1 */
static int file_compact(struct bake_state *b)
{
	struct prism_bake_cache log;
	struct prism_bake_cache_writer writer;
	char temporary[XGPU_BAKE_PATH + 168];
	void *payload = NULL;
	size_t index, capacity = 0;
	int failed = 0;

	memset(&log, 0, sizeof(log));
	if (prism_bake_cache_open(&log, b->new_path, &b->key, 0) != PRISM_BAKE_CACHE_CURRENT)
	{
		prism_bake_cache_close(&log);
		return prism_bake_cache_commit(b->new_path, b->path);
	}
	snprintf(temporary, sizeof(temporary), "%s.tmp", b->path);
	if (prism_bake_cache_create(&writer, temporary, &b->key, 0))
	{
		prism_bake_cache_close(&log);
		return prism_bake_cache_commit(b->new_path, b->path);
	}
	if (log.plan_count)
		failed |= prism_bake_cache_append_plan(&writer, log.plan_jobs, log.plan_count) != 0;
	for (index = 0; index < log.record_count && !failed; index++)
	{
		struct prism_bake_record const *record = &log.records[index], *source = record;

		if (record->type == PRISM_BAKE_RECORD_PLAN || record->type == PRISM_BAKE_RECORD_CHECKPOINT ||
			record->type == PRISM_BAKE_RECORD_DONE || record->type == PRISM_BAKE_RECORD_ATLAS_AUX ||
			record->type == PRISM_BAKE_RECORD_ATLAS_FILTERED ||
			prism_bake_cache_find(&log, record->type, record->job) != record ||
			(record->job != PRISM_BAKE_JOB_NONE && log.plan_count && !prism_bake_cache_in_plan(&log, record->job) &&
			record->type != PRISM_BAKE_RECORD_FOOTPRINTS && record->type != PRISM_BAKE_RECORD_IMPORTANCE_GRID))
			continue;
		/* (a tile's filtered texels as its result) */
		if (record->type == PRISM_BAKE_RECORD_ATLAS_TILE)
		{
			struct prism_bake_record const *filtered = prism_bake_cache_find(&log, PRISM_BAKE_RECORD_ATLAS_FILTERED,
				record->job);

			if (filtered && filtered->size == record->size && filtered->sequence > record->sequence)
				source = filtered;
		}
		if (record->size > capacity)
		{
			void *grown = realloc(payload, record->size);

			if (!grown)
			{
				failed = 1;
				break;
			}
			payload = grown;
			capacity = record->size;
		}
		/* (the large records packed, prism_bake_pack.h: the tiles' and probes'
		words by 4 bytes, the layers' and masks' planar bytes by 1) */
		if (prism_bake_cache_read(&log, source, payload, record->size))
			failed = 1;
		else if (record->type == PRISM_BAKE_RECORD_ATLAS_TILE || record->type == PRISM_BAKE_RECORD_VOLUME_BRICK ||
			record->type == PRISM_BAKE_RECORD_PROBE)
			failed |= prism_bake_cache_append_packed(&writer, record->type, record->job, payload, record->size, 4) != 0;
		else if (record->type == PRISM_BAKE_RECORD_ATLAS_LAYERS || record->type == PRISM_BAKE_RECORD_VOLUME_LAYERS ||
			record->type == PRISM_BAKE_RECORD_SUN_MASK_TILE)
			failed |= prism_bake_cache_append_packed(&writer, record->type, record->job, payload, record->size, 1) != 0;
		else if (prism_bake_cache_append(&writer, record->type, record->job, payload, record->size))
			failed = 1;
	}
	failed |= prism_bake_cache_append(&writer, PRISM_BAKE_RECORD_DONE, PRISM_BAKE_JOB_NONE, NULL, 0) != 0;
	failed |= prism_bake_cache_writer_close(&writer) != 0;
	prism_bake_cache_close(&log);
	free(payload);
	if (failed)
	{
		remove(temporary);
		return prism_bake_cache_commit(b->new_path, b->path);
	}
	if (prism_bake_cache_commit(temporary, b->path))
		return -1;
	/* (HALO_BAKE_KEEP_LOG keeps the log as <bsp>.pbk.log: its tiles as baked
	and the filter's words, a diagnostic) */
	if (getenv("HALO_BAKE_KEEP_LOG"))
	{
		snprintf(temporary, sizeof(temporary), "%s.log", b->path);
		if (rename(b->new_path, temporary) == 0)
			return 0;
	}
	remove(b->new_path);
	return 0;
}

/* the finished jobs the file has, drawn (a frame's staging's worth at a
time): 1 once all are */
static int file_load(VkCommandBuffer commands, struct bake_state *b)
{
	static uint32_t cursor;
	struct bake_plan *plan = &b->plan;
	uint32_t file[FILE_TILE_WORDS], raster[RASTER_TILE_WORDS], indirect[TILE_TEXELS];
	uint8_t layers[ATLAS_LAYER_BYTES];
	int loaded = 0;

	if (!b->cache.record_count)
		return 1;
	if (b->frame == 0 && !b->logged_resumed)
	{
		cursor = 0;
		b->logged_resumed = 1;
	}
	/* the importance grid */
	if (!b->grid_ready)
	{
		struct prism_bake_record const *record = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_IMPORTANCE_GRID,
			prism_bake_job_importance_cell(255, 0, 0, 0));

		if (record && record->size >= (1 + GRID_WORDS) * 4)
		{
			uint32_t *table = (uint32_t *)malloc(record->size);

			/* (its first word the table's entries, the buffer after it) */
			if (table && prism_bake_cache_read(&b->cache, record, table, record->size) == 0 && table[0] >= 1024 &&
				!(table[0] & (table[0] - 1)) && (size_t)table[0] * GRID_WORDS * 4 + 4 <= record->size &&
				buffer_filled(commands, table + 1, record->size - 4, &b->grid, 0))
			{
				b->block.grid = b->grid.address;
				b->block.settings[2] = table[0];
				b->grid_entries = b->block.settings[2];
				barrier(commands);
				scene_block_upload(commands, b);
				b->grid_ready = 1;
				b->cells_done = plan->cell_count * GRID_CANDIDATES;
			}
			free(table);
		}
	}
	for (; cursor < plan->tile_count; cursor++)
	{
		struct bake_tile *tile = &plan->tiles[cursor];
		struct prism_bake_record const *record;
		uint32_t index;
		int lit = 0, dark = 0, facing = 0, sun_state;
		uint8_t sun_texels[TILE_TEXELS];

		if (!prism_bake_cache_job_done(&b->cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile->job))
		{
			/* (its draft, where the file has it: drawn until the tile is done) */
			struct prism_bake_record const *draft = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_CHECKPOINT,
				tile->job);
			uint8_t payload[sizeof(raster) + TILE_TEXELS + 4];
			int draft_sun;

			if (draft && draft->size == sizeof(payload) && prism_bake_cache_read(&b->cache, draft, payload,
				sizeof(payload)) == 0)
			{
				if (loaded >= 2048)
					return 0;
				memcpy(raster, payload, sizeof(raster));
				memcpy(&draft_sun, payload + sizeof(raster) + TILE_TEXELS, 4);
				if (!tile_draw(commands, b, cursor, raster, draft_sun >= 1 && draft_sun <= 3 ? draft_sun : 2,
					payload + sizeof(raster), NULL))
					return 0;
				b->drafted++;
				loaded++;
			}
			continue;
		}
		record = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_ATLAS_TILE, tile->job);
		if (!record || record->size != sizeof(file))
			continue;
		if (loaded >= 2048)
			return 0;
		{
			/* (its filtered texels, where the log has them) */
			struct prism_bake_record const *filtered = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_ATLAS_FILTERED,
				tile->job);

			if (filtered && filtered->size == sizeof(file) && filtered->sequence > record->sequence &&
				prism_bake_cache_read(&b->cache, filtered, file, sizeof(file)) == 0)
				b->tiles_filtered++;
			else if (prism_bake_cache_read(&b->cache, record, file, sizeof(file)))
				continue;
		}
		{
			/* (the light drawn: recombined from its layers by the weights now) */
			struct prism_bake_record const *shares = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_ATLAS_LAYERS,
				tile->job);
			int have = shares && shares->size == sizeof(layers) &&
				prism_bake_cache_read(&b->cache, shares, layers, sizeof(layers)) == 0;

			prism_bake_layers_tile(&b->weights, file, have ? layers : NULL, TILE_TEXELS, raster, indirect);
		}
		for (index = 0; index < TILE_TEXELS; index++)
		{
			uint32_t w3 = file[index * 4 + 3], valid = (w3 >> 24) & 3u;

			sun_texels[index] = (uint8_t)(w3 & 255u);
			if (valid == 1 && ((w3 >> 24) & 4u))
			{
				facing++;
				lit += (w3 & 255u) >= 255u;
				dark += (w3 & 255u) == 0u;
			}
		}
		sun_state = !facing || dark == facing ? 1 : lit == facing ? 2 : 3;
		if (sun_state >= 3)
		{
			/* (the fine mask where the file has it) */
			struct prism_bake_record const *mask_record = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_SUN_MASK_TILE,
				prism_bake_job_sun_mask_tile(tile->vpage, 0, tile->x, tile->y));

			uint8_t mask[MASK_TEXELS];
			int fine = mask_record && mask_record->size == MASK_TEXELS &&
				prism_bake_cache_read(&b->cache, mask_record, mask, MASK_TEXELS) == 0;

			if (!tile_draw(commands, b, cursor, raster, sun_state, sun_texels, fine ? mask : NULL))
				return 0;
			tile->mask_state = fine ? 2 : 1;
			b->masks_done += fine;
		}
		else if (!tile_draw(commands, b, cursor, raster, sun_state, sun_texels, NULL))
			return 0;
		if (!tile_indirect(commands, b, cursor, indirect))
			return 0;
		tile->state = TILE_DONE;
		b->tiles_done++;
		loaded++;
	}
	return 1;
}

/* the volume's bricks the file has (once the volume's buffers are made) */
static void volume_load(VkCommandBuffer commands, struct bake_state *b)
{
	struct bake_plan *plan = &b->plan;
	uint32_t index, words[BRICK_PROBES * PROBE_WORDS], first = b->volume.done;
	uint8_t layers[VOLUME_LAYER_BYTES];

	if (!b->cache.record_count || !b->probe_host)
		return;
	for (index = first; index + BRICK_PROBES <= plan->probe_count && index < first + 65536; index += BRICK_PROBES)
	{
		uint64_t job = brick_job(plan, index);
		struct prism_bake_record const *record;

		if (!prism_bake_cache_job_done(&b->cache, PRISM_BAKE_RECORD_VOLUME_BRICK, job))
			break;
		record = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_VOLUME_BRICK, job);
		if (!record || record->size != sizeof(words) || prism_bake_cache_read(&b->cache, record, words, sizeof(words)))
			break;
		{
			/* (recombined from its layers by the weights now) */
			struct prism_bake_record const *shares = prism_bake_cache_find(&b->cache, PRISM_BAKE_RECORD_VOLUME_LAYERS, job);
			int have = shares && shares->size == sizeof(layers) &&
				prism_bake_cache_read(&b->cache, shares, layers, sizeof(layers)) == 0;

			prism_bake_layers_probes_mean(&b->weights, words, have ? layers : NULL, BRICK_PROBES, b->volume_ratio_sum,
				b->volume_ratio_weight);
			prism_bake_layers_probes(&b->weights, words, have ? layers : NULL, BRICK_PROBES, words);
		}
		memcpy(b->probe_host + (size_t)index * PROBE_WORDS, words, sizeof(words));
		if (!upload(commands, &b->probes, (VkDeviceSize)index * PROBE_WORDS * 4, words, sizeof(words)))
			break;
		prism_bake_batches_skip(&b->volume, index + BRICK_PROBES);
	}
}

/* ---------- the layered bake's light recombined (prism_bake_layers.h): the
weights the live rig gives against the bake's base, and, when they change
(a setting that scales or tints a light: display.prism_sky_light, the
rig's scales, display.prism_rt_emissive_scale), everything drawn
recombined again from the file, with no rebake */

/* the file read again for the recombining (the finished file, or the log so
far, flushed): 1 where it is open */
static int relight_open(struct bake_state *b)
{
	const char *path = b->complete || (!b->writing && !b->idle) ? b->path : b->new_path;

	if (b->relight_open)
		return 1;
	if (b->writing && b->writer.file)
		fflush(b->writer.file);
	if (prism_bake_cache_open(&b->relight_reader, path, &b->key, 0) != PRISM_BAKE_CACHE_CURRENT)
	{
		prism_bake_cache_close(&b->relight_reader);
		if (prism_bake_cache_open(&b->relight_reader, b->complete ? b->new_path : b->path, &b->key, 0) !=
			PRISM_BAKE_CACHE_CURRENT)
		{
			prism_bake_cache_close(&b->relight_reader);
			return 0;
		}
	}
	b->relight_open = 1;
	return 1;
}

static void relight_close(struct bake_state *b)
{
	if (b->relight_open)
		prism_bake_cache_close(&b->relight_reader);
	b->relight_open = 0;
}

/* a frame's share of the recombining: the drawn tiles (2048 a frame), then
the volume's bricks, then the reflection probes; 1 once all are */
static int relight_step(VkCommandBuffer commands, struct bake_state *b)
{
	struct prism_bake_cache *reader = &b->relight_reader;
	struct bake_plan *plan = &b->plan;
	uint32_t file[FILE_TILE_WORDS], raster[RASTER_TILE_WORDS], indirect[TILE_TEXELS];
	uint8_t layers[ATLAS_LAYER_BYTES];
	int done = 0;

	if (!relight_open(b))
		return 1;
	for (; b->relight_tile < plan->tile_count && done < 2048; b->relight_tile++)
	{
		struct bake_tile *tile = &plan->tiles[b->relight_tile];
		struct prism_bake_record const *record, *filtered, *shares;

		if (tile->state != TILE_DONE || !tile->physical)
			continue;
		record = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_ATLAS_TILE, tile->job);
		shares = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_ATLAS_LAYERS, tile->job);
		if (!record || record->size != sizeof(file) || !shares || shares->size != sizeof(layers))
			continue;
		filtered = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_ATLAS_FILTERED, tile->job);
		if (filtered && (filtered->size != sizeof(file) || filtered->sequence < record->sequence))
			filtered = NULL;
		if (prism_bake_cache_read(reader, filtered ? filtered : record, file, sizeof(file)) ||
			prism_bake_cache_read(reader, shares, layers, sizeof(layers)))
			continue;
		prism_bake_layers_tile(&b->weights, file, layers, TILE_TEXELS, raster, indirect);
		if (!upload(commands, &b->pool, (VkDeviceSize)(tile->physical - 1) * RASTER_TILE_WORDS * 4, raster,
			RASTER_TILE_WORDS * 4))
			return 0;
		tile_indirect(commands, b, b->relight_tile, indirect);
		done++;
		b->relights++;
	}
	if (b->relight_tile < plan->tile_count)
		return 0;
	/* (the volume's bricks drawn: their mean ratio summed again) */
	if (b->probe_host && b->probes.buffer)
	{
		uint32_t words[BRICK_PROBES * PROBE_WORDS], index;
		uint8_t shares_bytes[VOLUME_LAYER_BYTES];

		if (b->relight_probe == 0)
		{
			memset(b->volume_ratio_sum, 0, sizeof(b->volume_ratio_sum));
			memset(b->volume_ratio_weight, 0, sizeof(b->volume_ratio_weight));
		}
		for (index = b->relight_probe; index + BRICK_PROBES <= b->volume.done && index < b->relight_probe + 65536;
			index += BRICK_PROBES)
		{
			uint64_t job = brick_job(plan, index);
			struct prism_bake_record const *record = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_VOLUME_BRICK, job);
			struct prism_bake_record const *shares = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_VOLUME_LAYERS, job);

			if (!record || record->size != sizeof(words) || !shares || shares->size != sizeof(shares_bytes) ||
				prism_bake_cache_read(reader, record, words, sizeof(words)) ||
				prism_bake_cache_read(reader, shares, shares_bytes, sizeof(shares_bytes)))
				continue;
			prism_bake_layers_probes_mean(&b->weights, words, shares_bytes, BRICK_PROBES, b->volume_ratio_sum,
				b->volume_ratio_weight);
			prism_bake_layers_probes(&b->weights, words, shares_bytes, BRICK_PROBES, words);
			memcpy(b->probe_host + (size_t)index * PROBE_WORDS, words, sizeof(words));
			if (!upload(commands, &b->probes, (VkDeviceSize)index * PROBE_WORDS * 4, words, sizeof(words)))
				return 0;
		}
		b->relight_probe = index;
		if (index + BRICK_PROBES <= b->volume.done)
			return 0;
	}
	/* (the reflection probes drawn, by the BSP's ratio) */
	if (b->reflection_data.buffer)
	{
		uint32_t words = 12 + REFLECTION_TEXELS, loaded = 0;
		uint32_t *payload = (uint32_t *)malloc((size_t)words * 4);
		float ratio[3];

		reflection_ratio(b, ratio);
		while (payload && b->relight_reflection < b->reflections_done && loaded < 256)
		{
			struct prism_bake_record const *record = prism_bake_cache_find(reader, PRISM_BAKE_RECORD_PROBE,
				prism_bake_job_probe(1, b->relight_reflection));

			if (record && record->size == (size_t)words * 4 &&
				prism_bake_cache_read(reader, record, payload, (size_t)words * 4) == 0)
			{
				prism_bake_layers_scale(payload + 12, REFLECTION_TEXELS, ratio);
				if (!upload(commands, &b->reflection_data, (VkDeviceSize)b->relight_reflection * REFLECTION_TEXELS * 4,
					payload + 12, REFLECTION_TEXELS * 4))
					break;
			}
			b->relight_reflection++;
			loaded++;
		}
		free(payload);
		if (b->relight_reflection < b->reflections_done)
			return 0;
	}
	return 1;
}

/* each frame: the weights the live rig gives (where the bake's base is
known), the recombining begun when they change, and a frame's share of it */
static void layers_frame(VkCommandBuffer commands, struct bake_state *b)
{
	const struct xgpu_stream_rt_frame *frame;
	const float *materials;
	const uint32_t *alpha;
	uint32_t generation, material_count, alpha_words;
	struct prism_bake_layer_light live;
	struct prism_bake_layer_weights weights;

	if (!b->base_known || b->step < STEP_UPLOAD)
		return;
	if (xgpu_vulkan_rt_scene(&generation, &materials, &material_count, &alpha, &alpha_words, &frame) &&
		generation == b->generation && frame)
	{
		memset(&live, 0, sizeof(live));
		layer_light_of(frame->rig, materials, material_count, &live);
		prism_bake_layer_weights_make(&b->base, &live, &weights);
		if (prism_bake_layer_weights_differ(&weights, &b->weights))
		{
			int anything = b->tiles_done || b->drafted || b->volume.done || b->reflections_done;

			b->weights = weights;
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the light's weights now sun %.3f, sky %.3f, domes %.3f, "
				"fixtures %.3f, lamps %.3f%s: %s", b->map, (int)b->bsp, luma3(weights.w[PRISM_BAKE_LAYER_SUN]),
				luma3(weights.w[PRISM_BAKE_LAYER_SKY]), luma3(weights.w[PRISM_BAKE_LAYER_DOMES]),
				luma3(weights.w[PRISM_BAKE_LAYER_FIXTURES]), luma3(weights.w[PRISM_BAKE_LAYER_LAMPS]),
				weights.flat ? ", Tool's flat fill" : "", anything ? "what is drawn recombined, no rebake" :
				"the bake drawn so");
			if (anything)
			{
				relight_close(b);
				b->relighting = 1;
				b->relight_tile = b->relight_probe = b->relight_reflection = 0;
				b->relight_started = now_seconds();
				b->relights = 0;
			}
		}
	}
	if (b->relighting && relight_step(commands, b))
	{
		b->relighting = 0;
		relight_close(b);
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: recombined %u tiles, the volume and %u reflection probes in "
			"%.2f s", b->map, (int)b->bsp, b->relights, b->reflections_done, now_seconds() - b->relight_started);
	}
}

/* ---------- each frame's slice */

/* the bake's GPU time measured: its frames' timestamps read where done, the
samples a dispatch takes moved toward the budget */
static void timing_collect(void)
{
	uint32_t pair;
	uint64_t done = xgpu_vulkan_completed();

	if (!bake.queries)
		return;
	for (pair = 0; pair < 8; pair++)
	{
		uint64_t times[2];
		double milliseconds, budget, ratio;

		if (!bake.query_written[pair] || bake.query_serial[pair] > done)
			continue;
		bake.query_written[pair] = 0;
		if (vkGetQueryPoolResults(bake.device.device, bake.queries, pair * 2, 2, sizeof(times), times, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || times[1] < times[0])
			continue;
		milliseconds = (double)(times[1] - times[0]) * bake.timestamp_period * 1.0e-6;
		bake.gpu_milliseconds = milliseconds;
		if (bake.bake)
			bake.bake->gpu_seconds += milliseconds * 1.0e-3;
		budget = bake.control.budget_milliseconds > 0.1f ? bake.control.budget_milliseconds : 2.0;
		if (milliseconds <= 0.01)
			continue;
		ratio = budget / milliseconds;
		ratio = ratio < 0.5 ? 0.5 : ratio > 1.5 ? 1.5 : ratio;
		bake.samples = (uint32_t)((double)bake.samples * ratio + 0.5);
		if (bake.samples < 1)
			bake.samples = 1;
		/* (a batch whose texels are mostly done gives its last running ones
		the frame's time: many samples a dispatch) */
		if (bake.samples > 4096)
			bake.samples = 4096;
	}
}

static void timestamp(VkCommandBuffer commands, int end)
{
	uint32_t pair = bake.query_next % 8;

	if (!bake.queries)
		return;
	if (!end)
	{
		if (bake.query_written[pair])
			return;
		vkCmdResetQueryPool(commands, bake.queries, pair * 2, 2);
		vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, bake.queries, pair * 2);
		bake.query_written[pair] = 2;
	}
	else if (bake.query_written[pair] == 2)
	{
		vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, bake.queries, pair * 2 + 1);
		bake.query_written[pair] = 1;
		bake.query_serial[pair] = xgpu_vulkan_serial();
		bake.query_next++;
	}
}

static void readbacks_collect(VkCommandBuffer commands, struct bake_state *b)
{
	uint64_t done = xgpu_vulkan_completed();
	uint32_t index;

	for (index = 0; index < READBACK_SLOTS; index++)
	{
		struct bake_readback *r = &bake.readbacks[index];

		if (!r->busy || r->serial > done)
			continue;
		if (b)
		{
			switch (r->kind)
			{
			case STEP_IMPORTANCE:
				if (b->step == STEP_IMPORTANCE)
					importance_finish(commands, b, (const float *)r->buffer.mapped);
				break;
			case STEP_LIGHTMAP:
				if (b->step == STEP_LIGHTMAP)
					lightmap_collect(commands, b, r);
				break;
			case STEP_MASK:
				if (b->step == STEP_MASK)
					mask_collect(commands, b, r);
				break;
			case STEP_VOLUME:
				if (b->step == STEP_VOLUME)
					volume_collect(commands, b, r);
				break;
			case STEP_REFLECTIONS:
				if (b->step == STEP_REFLECTIONS)
					reflections_collect(commands, b, r);
				break;
			}
		}
		r->busy = 0;
	}
}

static void status_write(struct bake_state *b)
{
	double planned, done, seconds, left = -1.0;
	int state, step;
	float progress = 0.0f;

	if (!b)
	{
		status_set(XGPU_BAKE_STATUS_STATE, XGPU_BAKE_STATE_WAITING);
		status_set(XGPU_BAKE_STATUS_SERIAL, ++bake.status_serial);
		return;
	}
	step = b->step;
	state = step == STEP_DONE ? XGPU_BAKE_STATE_DONE : step == STEP_FAILED ? XGPU_BAKE_STATE_FAILED :
		XGPU_BAKE_STATE_BAKING;
	/* (the whole's progress: the lightmap's tiles 80%, the masks 10%, the
	volume 10%) */
	planned = b->plan.tile_count ? (double)b->plan.tile_count : 1.0;
	done = (double)b->tiles_done / planned * 0.8;
	if (b->mask_tile_count)
		done += (double)b->masks_done / (double)b->mask_tile_count * 0.1;
	else if (step > STEP_MASK)
		done += 0.1;
	if (b->plan.probe_count)
		done += (double)b->volume.done / (double)b->plan.probe_count * 0.1;
	else if (step > STEP_VOLUME)
		done += 0.1;
	progress = step == STEP_DONE ? 1.0f : (float)done;
	seconds = step == STEP_DONE ? b->seconds : now_seconds() - b->started;
	if (progress > 0.01f && step != STEP_DONE)
		left = seconds / progress * (1.0 - progress);
	status_set(XGPU_BAKE_STATUS_STATE, (uint32_t)state);
	status_set(XGPU_BAKE_STATUS_BSP, (uint32_t)b->bsp);
	status_set(XGPU_BAKE_STATUS_GENERATION, b->generation);
	status_set(XGPU_BAKE_STATUS_QUALITY, b->complete ? b->loaded_quality : b->quality);
	status_set(XGPU_BAKE_STATUS_SOURCE, b->shipped ? XGPU_BAKE_SOURCE_SHIPPED : XGPU_BAKE_SOURCE_HERE);
	status_real(XGPU_BAKE_STATUS_PROGRESS, progress);
	status_set(XGPU_BAKE_STATUS_STEP, step == STEP_IMPORTANCE ? XGPU_BAKE_STEP_IMPORTANCE : step == STEP_LIGHTMAP ?
		XGPU_BAKE_STEP_LIGHTMAP : step == STEP_MASK ? XGPU_BAKE_STEP_SUN_MASK : step == STEP_VOLUME ? XGPU_BAKE_STEP_VOLUME :
		step == STEP_FOOTPRINTS ? XGPU_BAKE_STEP_FOOTPRINTS : step >= STEP_COMMIT ? XGPU_BAKE_STEP_DONE : XGPU_BAKE_STEP_PLAN);
	status_real(XGPU_BAKE_STATUS_STEP_PROGRESS, (float)((double)b->tiles_done / planned));
	status_real(XGPU_BAKE_STATUS_TEXELS_DONE, (float)((double)b->tiles_done * TILE_TEXELS / 1.0e6));
	status_real(XGPU_BAKE_STATUS_TEXELS, (float)(planned * TILE_TEXELS / 1.0e6));
	status_real(XGPU_BAKE_STATUS_RAYS, seconds > 0.0 ? (float)((double)b->rays / seconds / 1.0e6) : 0.0f);
	status_real(XGPU_BAKE_STATUS_SECONDS, (float)seconds);
	status_real(XGPU_BAKE_STATUS_SECONDS_LEFT, (float)left);
	status_real(XGPU_BAKE_STATUS_MEGABYTES, (float)((double)b->file_bytes / 1048576.0));
	status_set(XGPU_BAKE_STATUS_SHOWN, (uint32_t)(step == STEP_DONE ? XGPU_BAKE_SHOWN_FINAL : b->tiles_done ?
		XGPU_BAKE_SHOWN_PARTIAL : XGPU_BAKE_SHOWN_NONE));
	status_set(XGPU_BAKE_STATUS_SERIAL, ++bake.status_serial);
}

static void bake_log_progress(struct bake_state *b)
{
	double t = now_seconds();

	if (t - bake.last_status < 10.0)
		return;
	bake.last_status = t;
	host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: step %d, tiles %u of %u, masks %u of %u, probes %u of %u, "
		"%.0f s, %u samples a dispatch (%.1f ms of GPU), %u texels running, %.0f MB written; %.0f samples a texel (the "
		"most %u; %.0f%% by the firefly rule; %.0f%% of those converged by the noise floor; %.0f capped), %.1f M samples "
		"a second, %.1f s of GPU", b->map, (int)b->bsp, b->step, b->tiles_done, b->plan.tile_count, b->masks_done,
		b->mask_tile_count, b->volume.done, b->plan.probe_count, t - b->started, bake.samples, bake.gpu_milliseconds,
		b->running_texels, (double)b->file_bytes / 1048576.0, b->texels_baked > 0.0 ? (double)b->rays / b->texels_baked : 0.0,
		b->samples_most, b->texels_baked > 0.0 ? 100.0 * b->texels_firefly_bound / b->texels_baked : 0.0,
		b->texels_converged > 0.0 ? 100.0 * b->texels_floor / b->texels_converged : 0.0, b->texels_capped,
		(double)b->rays / (t - b->started + 1.0e-3) / 1.0e6, b->gpu_seconds);
}

static void bake_frame(void)
{
	struct bake_state *b = bake.bake;
	VkCommandBuffer commands;
	int wanted;

	garbage_collect();
	timing_collect();
	wanted = bake.control.mode == XGPU_BAKE_MODE_AUTO || bake.control.request != 0;
	/* (a bake of another BSP, scene or quality, a rebake asked, or one
	opened while none was wanted that now is: let go) */
	if (b && (b->generation != bake.control.generation || b->bsp != bake.control.bsp_index ||
		b->quality != (bake.control.quality > PRISM_BAKE_QUALITY_MAX ? PRISM_BAKE_QUALITY_MAX : bake.control.quality) ||
		b->request != bake.control.request || (b->idle && wanted)))
	{
		int rebake = b->request != bake.control.request && b->generation == bake.control.generation;
		char path[XGPU_BAKE_PATH + 164];

		snprintf(path, sizeof(path), "%s", b->new_path);
		bake_free(b);
		bake.bake = b = NULL;
		if (rebake)
		{
			remove(path);
			path[strlen(path) - 4] = 0;
			remove(path);
		}
	}
	if (!b)
	{
		b = bake.bake = bake_begin();
		if (!b)
		{
			status_write(NULL);
			return;
		}
	}
	commands = xgpu_vulkan_compute_commands();
	/* (the layered bake's weights, and what is drawn recombined when they
	change) */
	layers_frame(commands, b);
	readbacks_collect(commands, b);
	/* (the filter's tiles, while it runs beside the steps after the lightmap) */
	filter_drain(commands, b);
	/* (display.bake "off", or "ask" and not asked: what the file has is
	drawn, nothing more baked) */
	if (!wanted && !b->complete && b->step > STEP_LOAD && b->step < STEP_COMMIT)
	{
		status_write(b);
		status_set(XGPU_BAKE_STATUS_STATE, XGPU_BAKE_STATE_PAUSED);
		return;
	}
	/* (HALO_BAKE_DEBUG_FAIL=<step>: the bake failed as it reaches that step
(4 the importance grid, 6 the sun mask, 7 the volume, 8 the reflection
probes), as a card out of memory fails it; for the tests of what a failed
bake still traces) */
	{
		const char *fail = getenv("HALO_BAKE_DEBUG_FAIL");

		if (fail && b->step == atoi(fail))
		{
			bake_fail(b, "failed as HALO_BAKE_DEBUG_FAIL asks");
			return;
		}
	}
	if (b->step != bake.samples_step)
	{
		bake.samples_step = b->step;
		bake.samples = SAMPLES_FIRST;
	}
	timestamp(commands, 0);
	switch (b->step)
	{
	case STEP_PLAN:
		if (b->thread_running)
		{
			/* (the thread's end: its plan whole) */
			if (!__atomic_load_n(&b->plan.done, __ATOMIC_ACQUIRE))
				break;
			pthread_join(b->thread, NULL);
			b->thread_running = 0;
		}
		if (b->plan.failed)
		{
			bake_fail(b, "its plan cannot be made (no memory)");
			break;
		}
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: planned in %.1f s: %u charts, %u virtual pages, %u tiles (%.1f M "
			"texels), %u triangle lists' entries; %u grid cells; %u probes (at %.3f s)", b->map, (int)b->bsp,
			b->plan.seconds, (uint32_t)b->plan.atlas.chart_count, b->plan.vpage_count, b->plan.tile_count,
			(double)b->plan.tile_count * TILE_TEXELS / 1.0e6, (uint32_t)b->plan.tile_triangle_count, b->plan.cell_count,
			b->plan.probe_count, now_seconds());
		if (b->plan.fine_dropped)
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the volume's budget: %u of the fine cascade's %u bricks "
				"kept (the nearest the level's shapes), %u left to the coarse cascade; %u coarse bricks", b->map,
				(int)b->bsp, b->plan.fine_wanted - b->plan.fine_dropped, b->plan.fine_wanted, b->plan.fine_dropped,
				(b->plan.probe_count - b->plan.fine_count) / BRICK_PROBES);
		b->step = STEP_UPLOAD;
		break;
	case STEP_UPLOAD:
		if (!bake_upload(b))
		{
			bake_fail(b, "its scene cannot be put on the GPU (no memory)");
			break;
		}
		file_open(b, wanted);
		b->step = STEP_LOAD;
		break;
	case STEP_LOAD:
		/* (the file's finished jobs drawn first) */
		if (file_load(commands, b))
			b->step = b->grid_ready ? STEP_LIGHTMAP : STEP_IMPORTANCE;
		break;
	case STEP_IMPORTANCE:
		importance_frame(commands, b);
		if (b->grid_ready)
			b->step = STEP_LIGHTMAP;
		break;
	case STEP_LIGHTMAP:
		/* (a background bake of more than the draft's samples: the draft
		first, every tile at 64 samples, so the level is lit by its own bake
		within seconds) */
		if (!b->drafted && !b->drafting && !(bake.control.flags & XGPU_BAKE_FLAG_FOREGROUND) && cap_of(b) != 64.0f &&
			b->tiles_done < b->plan.tile_count)
		{
			uint32_t index, pending = 0;

			for (index = 0; index < b->plan.tile_count; index++)
				pending += b->plan.tiles[index].state == TILE_PENDING && !b->plan.tiles[index].physical;
			if (pending)
			{
				b->drafting = 1;
				b->frame = 0;
				host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the draft first (%u tiles)", b->map, (int)b->bsp, pending);
			}
		}
		if (b->drafting)
		{
			uint32_t index, left = 0;

			for (index = 0; index < BATCH_TILES; index++)
				left += b->batch.tiles[index] != 0xffffffffu;
			if (!left && b->drafted)
			{
				for (index = 0; index < b->plan.tile_count && !left; index++)
					left = b->plan.tiles[index].state == TILE_PENDING && !b->plan.tiles[index].physical;
				if (!left)
				{
					b->drafting = 0;
					b->frame = 0;
					b->shown = XGPU_BAKE_SHOWN_DRAFT;
					host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the draft drawn in %.1f s", b->map, (int)b->bsp,
						now_seconds() - b->started);
					break;
				}
			}
		}
		if (!b->drafting && b->tiles_done >= b->plan.tile_count)
		{
			filter_start(b);
			b->step = STEP_MASK;
			break;
		}
		lightmap_frame(commands, b);
		break;
	case STEP_MASK:
		if (!b->mask_tiles)
		{
			mask_frame(commands, b);
			plan_write(b, 1);
			break;
		}
		if (b->masks_done >= b->mask_tile_count)
		{
			b->step = STEP_VOLUME;
			break;
		}
		mask_frame(commands, b);
		break;
	case STEP_VOLUME:
		/* (the bricks the file has first, a frame's staging at a time) */
		if (!b->volume_loaded && b->probe_host)
		{
			uint32_t before = b->volume.done;

			volume_load(commands, b);
			if (b->volume.done == before)
				b->volume_loaded = 1;
		}
		else
			volume_frame(commands, b);
		if (b->volume.done >= b->plan.probe_count)
			b->step = STEP_REFLECTIONS;
		break;
	case STEP_REFLECTIONS:
		if (!b->reflections_made)
		{
			/* (port: no memory for them (a card shared with the desktop, the
			path tracer's objects): the bake goes on without them, the
			surfaces' reflections the volume's, rather than failing whole and
			leaving the frame Classic's) */
			if (b->plan.reflection_count && !reflections_make(commands, b))
			{
				host_logf(HOST_LOG_WARN, "prism bake: %s bsp %d: no memory for the %u reflection probes: none used",
					b->map, (int)b->bsp, b->plan.reflection_count);
				b->plan.reflection_count = 0;
			}
			b->reflections_made = 1;
			host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: %u reflection probes", b->map, (int)b->bsp,
				b->plan.reflection_count);
			break;
		}
		if (!b->reflections_loaded)
		{
			if (!reflections_load(commands, b))
				b->reflections_loaded = 1;
		}
		else
			reflections_frame(commands, b);
		if (b->reflections_done >= b->plan.reflection_count)
			b->step = STEP_FOOTPRINTS;
		break;
	case STEP_FOOTPRINTS:
		/* (the filter's last tiles first) */
		if (b->filter_state == 1)
			break;
		footprints_make(b);
		b->step = STEP_COMMIT;
		break;
	case STEP_COMMIT:
		b->seconds = now_seconds() - b->started;
		if (b->writing)
		{
			base_write(b);
			file_append(b, PRISM_BAKE_RECORD_DONE, PRISM_BAKE_JOB_NONE, NULL, 0);
			prism_bake_cache_writer_close(&b->writer);
			b->writing = 0;
			if (file_compact(b))
				host_logf(HOST_LOG_WARN, "prism bake: %s cannot be put in place (%s)", b->path, strerror(errno));
		}
		{
			struct stat info;

			if (stat(b->path, &info) == 0)
				b->file_bytes = (uint64_t)info.st_size;
		}
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: done in %.1f s: %u tiles (%.1f M texels), %u masks, %u probes, "
			"%.1f G samples, %.1f MB (%s); this run %.1f s of GPU, %.0f texels converged, %.0f of them by the noise floor",
			b->map, (int)b->bsp, b->seconds, b->plan.tile_count, (double)b->plan.tile_count * TILE_TEXELS / 1.0e6,
			b->mask_tile_count, b->plan.probe_count, (double)b->rays / 1.0e9, (double)b->file_bytes / 1048576.0, b->path,
			b->gpu_seconds, b->texels_converged, b->texels_floor);
		host_logf(HOST_LOG_INFO, "prism bake: %s bsp %d: the noise: %.0f texels taken to their groups' median (the robust "
			"mean%s); the filter %d passes, %u tiles in %.1f s%s", b->map, (int)b->bsp, b->texels_robust,
			b->robust_on ? "" : " off", b->filter.passes, b->filter_tiles, b->filter_seconds,
			b->tiles_filtered ? ", the rest filtered in the file" : "");
		b->step = STEP_DONE;
		break;
	default:
		break;
	}
	timestamp(commands, 1);
	if (b->step != STEP_DONE && b->step != STEP_FAILED)
		bake_log_progress(b);
	status_write(b);
}

/* ---------- the reach (the game's inputs, port/linux/game/prism_bake.c's
reach_serialise; found here, on a thread of its own: a large BSP's walk is
millions of cells) */

struct reach_reader
{
	const uint32_t *words;
	size_t count, at;
	int failed;
};

static const void *reach_take(struct reach_reader *r, size_t words)
{
	const void *at;

	if (r->failed || words > r->count - r->at)
	{
		r->failed = 1;
		return NULL;
	}
	at = r->words + r->at;
	r->at += words;
	return at;
}

static int32_t reach_count(struct reach_reader *r, size_t words_each)
{
	const int32_t *value = (const int32_t *)reach_take(r, 1);

	if (!value || *value < 0 || (words_each && (size_t)*value > (r->count - r->at) / words_each))
	{
		r->failed = 1;
		return 0;
	}
	return *value;
}

/* whether a fighter stands where the walk reached (within two world units
of a floor or a body's air): a vehicle the player can reach and fly */
static int fighter_reached(struct prism_bake_reach const *reach, const float *fighters, int32_t count)
{
	int32_t index;

	for (index = 0; index < count; index++)
	{
		const float *at = fighters + index * 3;
		float cell = reach->voxels.cell;
		int32_t x0 = (int32_t)floorf(at[0] / cell), y0 = (int32_t)floorf(at[1] / cell), z0 = (int32_t)floorf(at[2] / cell);
		int32_t reach_cells = (int32_t)(2.0f / cell), x, y, z;

		for (z = z0 - reach_cells; z <= z0 + reach_cells; z++)
			for (y = y0 - reach_cells; y <= y0 + reach_cells; y++)
				for (x = x0 - reach_cells; x <= x0 + reach_cells; x++)
					if (prism_bake_voxels_get(&reach->voxels, x, y, z) & (PRISM_BAKE_REACH_WALK | PRISM_BAKE_REACH_BODY))
						return 1;
	}
	return 0;
}

/* the reach's file: what the walk found for one BSP's inputs (their
hash), kept beside its bake so that a BSP drawn again (b30's beach after its
structure, a session after) takes it at once: "PRCH", its version, the
inputs' hash, the flight flag, the summary, the bricks and the tiers */
#define REACH_FILE_VERSION 1u
static int reach_file_read(uint64_t hash)
{
	FILE *f = bake.reach_path[0] ? fopen(bake.reach_path, "rb") : NULL;
	uint32_t head[2], counts[2];
	uint64_t stored;
	int32_t flight;
	uint32_t summary[8];
	int16_t *bricks = NULL;
	uint8_t *tiers = NULL;

	if (!f)
		return 0;
	if (fread(head, 4, 2, f) != 2 || memcmp(head, "PRCH", 4) || head[1] != REACH_FILE_VERSION ||
		fread(&stored, 8, 1, f) != 1 || stored != hash || fread(&flight, 4, 1, f) != 1 ||
		fread(summary, 4, 8, f) != 8 || fread(counts, 4, 2, f) != 2 || counts[0] > (1u << 26) || counts[1] > (1u << 26))
	{
		fclose(f);
		return 0;
	}
	bricks = (int16_t *)malloc(((size_t)counts[0] + 1) * 8);
	tiers = (uint8_t *)malloc((size_t)counts[1] + 1);
	if (!bricks || !tiers || fread(bricks, 8, counts[0], f) != counts[0] || fread(tiers, 1, counts[1], f) != counts[1])
	{
		free(bricks);
		free(tiers);
		fclose(f);
		return 0;
	}
	fclose(f);
	bake.bricks = bricks;
	bake.brick_count = bake.brick_total = counts[0];
	bake.tiers = tiers;
	bake.tier_count = counts[1];
	bake.reach_flight = flight;
	memcpy(bake.reach_summary, summary, sizeof(summary));
	return 1;
}

static void reach_file_write(uint64_t hash)
{
	char temporary[sizeof(bake.reach_path) + 8];
	uint32_t head[2] = { 0, REACH_FILE_VERSION }, counts[2] = { bake.brick_count, bake.tier_count };
	int32_t flight = bake.reach_flight;
	FILE *f;
	int ok;

	if (!bake.reach_path[0] || !bake.bricks)
		return;
	memcpy(head, "PRCH", 4);
	snprintf(temporary, sizeof(temporary), "%s.new", bake.reach_path);
	f = fopen(temporary, "wb");
	if (!f)
		return;
	ok = fwrite(head, 4, 2, f) == 2 && fwrite(&hash, 8, 1, f) == 1 && fwrite(&flight, 4, 1, f) == 1 &&
		fwrite(bake.reach_summary, 4, 8, f) == 8 && fwrite(counts, 4, 2, f) == 2 &&
		fwrite(bake.bricks, 8, counts[0], f) == counts[0] && (!counts[1] || fwrite(bake.tiers, 1, counts[1], f) == counts[1]);
	ok = fclose(f) == 0 && ok;
	/* (prism_bake_cache_commit: Windows's rename takes no file's place) */
	if (!ok || prism_bake_cache_commit(temporary, bake.reach_path))
		remove(temporary);
}

static void *reach_thread(void *unused)
{
	struct reach_reader r;
	struct prism_bake_bsp3d bsp;
	struct prism_bake_clusters clusters;
	struct prism_bake_reach_parameters parameters;
	struct prism_bake_importance_parameters importance;
	struct prism_bake_importance_counts counts;
	struct prism_bake_reach reach;
	struct prism_bake_voxels bricks;
	struct prism_bake_seed *seeds = NULL;
	int16_t *leaf_clusters = NULL, *leaf_counts = NULL;
	int32_t *leaf_first = NULL, *surfaces = NULL, fighter_count, water_count, seed_count, surface_count, index;
	float *positions = NULL;
	const float *bounds, *fighters, *water;
	uint8_t *tiers = NULL, *surface_tiers = NULL;
	size_t triangle_count = 0, slot, t;
	int built = 0, failed = 1, from_file = 0;
	uint64_t input_hash = 0;
	size_t hashed_words = 0;
	double started = now_seconds();

	(void)unused;
	memset(&bsp, 0, sizeof(bsp));
	memset(&clusters, 0, sizeof(clusters));
	memset(&counts, 0, sizeof(counts));
	memset(&bricks, 0, sizeof(bricks));
	memset(bake.reach_summary, 0, sizeof(bake.reach_summary));
	bake.reach_flight = 0;
	r.words = bake.reach_input;
	r.count = bake.reach_words;
	r.at = 0;
	r.failed = 0;
	{
		const uint32_t *head = (const uint32_t *)reach_take(&r, 2);

		if (!head || memcmp(head, "PBSC", 4) || head[1] != 4)
			r.failed = 1;
	}
	bounds = (const float *)reach_take(&r, 6);
	fighter_count = reach_count(&r, 3);
	fighters = (const float *)reach_take(&r, (size_t)fighter_count * 3);
	water_count = reach_count(&r, 4);
	water = (const float *)reach_take(&r, (size_t)water_count * 4);
	bsp.node_count = reach_count(&r, 3);
	bsp.nodes = (const int32_t *)reach_take(&r, (size_t)bsp.node_count * 3);
	bsp.plane_count = reach_count(&r, 4);
	bsp.planes = (const float *)reach_take(&r, (size_t)bsp.plane_count * 4);
	bsp.leaf_count = reach_count(&r, 0);
	clusters.leaf_count = reach_count(&r, 2);
	leaf_clusters = (int16_t *)malloc(((size_t)clusters.leaf_count + 1) * 2);
	leaf_counts = (int16_t *)malloc(((size_t)clusters.leaf_count + 1) * 2);
	leaf_first = (int32_t *)malloc(((size_t)clusters.leaf_count + 1) * 4);
	for (index = 0; leaf_clusters && leaf_counts && leaf_first && index < clusters.leaf_count; index++)
	{
		const uint32_t *leaf = (const uint32_t *)reach_take(&r, 2);

		if (!leaf)
			break;
		memcpy(&leaf_clusters[index], leaf, 2);
		memcpy(&leaf_counts[index], (const char *)leaf + 2, 2);
		memcpy(&leaf_first[index], leaf + 1, 4);
	}
	clusters.leaf_clusters = leaf_clusters;
	clusters.leaf_reference_count = leaf_counts;
	clusters.leaf_first_reference = leaf_first;
	clusters.reference_count = reach_count(&r, 1);
	clusters.reference_surfaces = (const int32_t *)reach_take(&r, (size_t)clusters.reference_count);
	clusters.cluster_count = reach_count(&r, 0);
	clusters.pvs_words = (size_t)reach_count(&r, 0);
	if (clusters.cluster_count > 65536 || clusters.pvs_words > 4096)
		r.failed = 1;
	clusters.pvs = !r.failed ? (const uint32_t *)reach_take(&r, (size_t)clusters.cluster_count * clusters.pvs_words) : NULL;
	if (clusters.pvs_words < (size_t)((clusters.cluster_count + 31) / 32))
		clusters.pvs = NULL;
	seed_count = reach_count(&r, 4);
	seeds = (struct prism_bake_seed *)malloc(((size_t)seed_count + 1) * sizeof(*seeds));
	for (index = 0; seeds && index < seed_count; index++)
	{
		const uint32_t *seed = (const uint32_t *)reach_take(&r, 4);

		if (!seed)
			break;
		seeds[index].kind = (int32_t)seed[0];
		memcpy(seeds[index].position, seed + 1, 12);
	}
	surface_count = reach_count(&r, 0);
	triangle_count = (size_t)reach_count(&r, 10);
	positions = (float *)malloc((triangle_count + 1) * 36);
	surfaces = (int32_t *)malloc((triangle_count + 1) * 4);
	for (t = 0; positions && surfaces && t < triangle_count; t++)
	{
		const uint32_t *triangle = (const uint32_t *)reach_take(&r, 10);

		if (!triangle)
			break;
		surfaces[t] = (int32_t)triangle[0];
		memcpy(positions + t * 9, triangle + 1, 36);
	}
	{
		int32_t count = reach_count(&r, SCENERY_FLOATS);
		const float *scenery = (const float *)reach_take(&r, (size_t)count * SCENERY_FLOATS);

		free(bake.scenery);
		bake.scenery = NULL;
		bake.scenery_count = 0;
		if (scenery && count > 0 && (bake.scenery = (float *)malloc((size_t)count * SCENERY_FLOATS * 4)) != NULL)
		{
			memcpy(bake.scenery, scenery, (size_t)count * SCENERY_FLOATS * 4);
			bake.scenery_count = (uint32_t)count;
		}
		count = reach_count(&r, FIXTURE_FLOATS);
		scenery = (const float *)reach_take(&r, (size_t)count * FIXTURE_FLOATS);
		free(bake.fixtures_input);
		bake.fixtures_input = NULL;
		bake.fixtures_input_count = 0;
		if (scenery && count > 0 && (bake.fixtures_input = (float *)malloc((size_t)count * FIXTURE_FLOATS * 4)) != NULL)
		{
			memcpy(bake.fixtures_input, scenery, (size_t)count * FIXTURE_FLOATS * 4);
			bake.fixtures_input_count = (uint32_t)count;
		}
		/* (the static scenery's runs a mod's mesh draws: after what the
		reach's file and the bake's key are made of, none from an older
		game; a run outside the scenery's triangles left out) */
		hashed_words = r.at;
		free(bake.scenery_replaced);
		bake.scenery_replaced = NULL;
		bake.scenery_replaced_count = 0;
		if (!r.failed && r.at < r.count)
		{
			const int32_t *runs;
			int32_t run;

			count = reach_count(&r, 2);
			runs = (const int32_t *)reach_take(&r, (size_t)count * 2);
			if (runs && count > 0 && (bake.scenery_replaced = (int32_t *)malloc((size_t)count * 8)) != NULL)
			{
				for (run = 0; run < count; run++)
				{
					if (runs[run * 2] < 0 || runs[run * 2 + 1] <= 0 ||
						(uint32_t)runs[run * 2] + (uint32_t)runs[run * 2 + 1] > bake.scenery_count)
						continue;
					bake.scenery_replaced[bake.scenery_replaced_count * 2] = runs[run * 2];
					bake.scenery_replaced[bake.scenery_replaced_count * 2 + 1] = runs[run * 2 + 1];
					bake.scenery_replaced_count++;
				}
			}
		}
		/* (the scenery's base map coordinates and cutouts' alpha maps: after
		the runs, none from an older game's; a map index past the maps, or
		maps that do not add up, left as none) */
		free(bake.scenery_uv);
		free(bake.scenery_map);
		free(bake.scenery_maps);
		bake.scenery_uv = NULL;
		bake.scenery_map = NULL;
		bake.scenery_maps = NULL;
		bake.scenery_map_count = bake.scenery_map_words = 0;
		if (!r.failed && r.at < r.count)
		{
			int32_t uv_count = reach_count(&r, 7), map_count, map_words;
			const float *uv = (const float *)reach_take(&r, (size_t)uv_count * 6);
			const int32_t *maps = (const int32_t *)reach_take(&r, (size_t)uv_count);
			const uint32_t *texels;

			map_count = reach_count(&r, 0);
			map_words = reach_count(&r, 1);
			texels = (const uint32_t *)reach_take(&r, (size_t)map_words);
			if (!r.failed && uv && maps && texels && uv_count > 0 && (uint32_t)uv_count == bake.scenery_count &&
				map_count > 0 && map_words > 0 &&
				(bake.scenery_uv = (float *)malloc((size_t)uv_count * 24)) != NULL &&
				(bake.scenery_map = (int32_t *)malloc((size_t)uv_count * 4)) != NULL &&
				(bake.scenery_maps = (uint32_t *)malloc((size_t)map_words * 4)) != NULL)
			{
				int32_t index;

				memcpy(bake.scenery_uv, uv, (size_t)uv_count * 24);
				memcpy(bake.scenery_maps, texels, (size_t)map_words * 4);
				bake.scenery_map_count = (uint32_t)map_count;
				bake.scenery_map_words = (uint32_t)map_words;
				for (index = 0; index < uv_count; index++)
					bake.scenery_map[index] = maps[index] >= 0 && maps[index] < map_count ? maps[index] : -1;
			}
			r.failed = 0;
		}
	}
	if (r.failed || !leaf_clusters || !leaf_counts || !leaf_first || !seeds || !positions || !surfaces)
	{
		host_logf(HOST_LOG_WARN, "prism bake: bsp %d's reach's inputs do not read", (int)bake.reach_bsp);
		goto done;
	}
	/* (the walk these inputs made before, from its file: none to make) */
	input_hash = prism_bake_hash64(PRISM_BAKE_HASH64_SEED, bake.reach_input, hashed_words * 4);
	if (reach_file_read(input_hash))
	{
		failed = 0;
		from_file = 1;
		goto done;
	}
	prism_bake_reach_defaults(&parameters);
	memcpy(parameters.bounds, bounds, sizeof(parameters.bounds));
	parameters.flight = 0;
	parameters.water_planes = water;
	parameters.water_plane_count = water_count;
	parameters.maximum_walk = 50000000;
	parameters.maximum_flight = 20000000;
	prism_bake_voxels_init(&bricks, 1.0f);
	built = prism_bake_reach_build(&reach, &bsp, seeds, (size_t)seed_count, &parameters) == 0;
	if (built && fighter_reached(&reach, fighters, fighter_count))
	{
		/* (a fighter the player can reach: flight, its air at a lower
		priority) */
		prism_bake_reach_free(&reach);
		parameters.flight = 1;
		bake.reach_flight = 1;
		built = prism_bake_reach_build(&reach, &bsp, seeds, (size_t)seed_count, &parameters) == 0;
	}
	if (!built)
	{
		host_logf(HOST_LOG_WARN, "prism bake: bsp %d's reach: no memory for its walk", (int)bake.reach_bsp);
		goto done;
	}
	tiers = (uint8_t *)malloc(triangle_count + 1);
	surface_tiers = (uint8_t *)malloc((size_t)surface_count + 1);
	prism_bake_importance_defaults(&importance);
	if (!tiers || !surface_tiers || prism_bake_importance(&reach, &bsp, &clusters, positions, surfaces, triangle_count,
		&importance, tiers, &counts) != 0)
	{
		prism_bake_reach_free(&reach);
		goto done;
	}
	/* (a surface no stock triangle lists: the tier of nothing seen; each
	listed one the best of its triangles') */
	memset(surface_tiers, PRISM_BAKE_TIER_UNSEEN, (size_t)surface_count + 1);
	for (t = 0; t < triangle_count; t++)
		if (surfaces[t] >= 0 && surfaces[t] < surface_count && tiers[t] < surface_tiers[surfaces[t]])
			surface_tiers[surfaces[t]] = tiers[t];
	/* the bricks: each 1-world-unit cell with reachable air (the walk's
	floors, a body's or a camera's), or flight's */
	failed = 0;
	for (slot = 0; slot < reach.voxels.capacity && !failed; slot++)
	{
		int32_t xyz[3];
		uint8_t flags = prism_bake_voxels_slot(&reach.voxels, slot, xyz), brick = 0;
		float cell = reach.voxels.cell;

		if (flags & (PRISM_BAKE_REACH_WALK | PRISM_BAKE_REACH_SEED))
			brick |= XGPU_BAKE_BRICK_WALK;
		if (flags & (PRISM_BAKE_REACH_BODY | PRISM_BAKE_REACH_CAMERA | PRISM_BAKE_REACH_VIEW))
			brick |= XGPU_BAKE_BRICK_CAMERA;
		if (brick && prism_bake_voxels_add(&bricks, (int32_t)floorf(((float)xyz[0] + 0.5f) * cell),
			(int32_t)floorf(((float)xyz[1] + 0.5f) * cell), (int32_t)floorf(((float)xyz[2] + 0.5f) * cell), brick) < 0)
			failed = 1;
	}
	for (slot = 0; slot < reach.flight.capacity && !failed; slot++)
	{
		int32_t xyz[3];
		uint8_t flags = prism_bake_voxels_slot(&reach.flight, slot, xyz);
		float cell = reach.flight.cell;

		if (flags && prism_bake_voxels_add(&bricks, (int32_t)floorf(((float)xyz[0] + 0.5f) * cell),
			(int32_t)floorf(((float)xyz[1] + 0.5f) * cell), (int32_t)floorf(((float)xyz[2] + 0.5f) * cell),
			XGPU_BAKE_BRICK_FLIGHT) < 0)
			failed = 1;
	}
	bake.reach_summary[0] = (uint32_t)reach.walk_count;
	bake.reach_summary[1] = (uint32_t)reach.body_count;
	bake.reach_summary[2] = (uint32_t)reach.camera_count;
	bake.reach_summary[3] = (uint32_t)reach.flight_count;
	bake.reach_summary[4] = (uint32_t)reach.seeds_used;
	bake.reach_summary[5] = (uint32_t)seed_count;
	bake.reach_summary[6] = (uint32_t)counts.triangles[0];
	bake.reach_summary[7] = (uint32_t)counts.triangles[1];
	prism_bake_reach_free(&reach);
	if (!failed)
	{
		size_t made = 0;

		bake.bricks = (int16_t *)malloc((bricks.count + 1) * 8);
		for (slot = 0; bake.bricks && slot < bricks.capacity; slot++)
		{
			int32_t xyz[3];
			uint8_t flags = prism_bake_voxels_slot(&bricks, slot, xyz);

			if (!flags || xyz[0] < -32768 || xyz[0] > 32767 || xyz[1] < -32768 || xyz[1] > 32767 || xyz[2] < -32768 ||
				xyz[2] > 32767)
				continue;
			bake.bricks[made * 4] = (int16_t)xyz[0];
			bake.bricks[made * 4 + 1] = (int16_t)xyz[1];
			bake.bricks[made * 4 + 2] = (int16_t)xyz[2];
			bake.bricks[made * 4 + 3] = (int16_t)flags;
			made++;
		}
		bake.brick_count = bake.brick_total = (uint32_t)made;
		bake.tiers = surface_tiers;
		bake.tier_count = (uint32_t)surface_count;
		surface_tiers = NULL;
		failed = !bake.bricks;
		if (!failed)
			reach_file_write(input_hash);
	}

done:
	prism_bake_voxels_free(&bricks);
	free(leaf_clusters);
	free(leaf_counts);
	free(leaf_first);
	free(seeds);
	free(positions);
	free(surfaces);
	free(tiers);
	free(surface_tiers);
	if (!failed)
		host_logf(HOST_LOG_INFO, "prism bake: bsp %d's reach (%.1f s%s): %u floors walked, %u body and %u camera cells, %u "
			"flight cells (%s), %u of %u seeds used; %u triangles near, %u seen further; %u bricks", (int)bake.reach_bsp,
			now_seconds() - started, from_file ? ", from its file" : "", bake.reach_summary[0], bake.reach_summary[1], bake.reach_summary[2],
			bake.reach_summary[3], bake.reach_flight ? "a fighter the player reaches" : "no fighter the player reaches",
			bake.reach_summary[4], bake.reach_summary[5], bake.reach_summary[6], bake.reach_summary[7], bake.brick_count);
	__atomic_store_n(&bake.reach_whole, failed ? -1 : 1, __ATOMIC_RELEASE);
	return NULL;
}

/* the reach's thread, if one runs, waited for */
static void reach_wait(void)
{
	if (bake.reach_running)
	{
		pthread_join(bake.reach_thread, NULL);
		bake.reach_running = 0;
	}
}

/* ---------- the stream's record */

void xgpu_vulkan_bake_record(const struct xgpu_stream_prism_bake *record, const void *data)
{
	if (!make())
		return;
	switch (record->phase)
	{
	case XGPU_BAKE_CONTROL:
		bake.control = *record;
		bake.control_known = 1;
		bake_frame();
		break;
	case XGPU_BAKE_REACH_INPUT:
		if (record->first == 0)
		{
			/* (the diagnostic log's clock: when a BSP's inputs begin) */
			if (getenv("HALO_RASTER_EXPOSURE_LOG"))
				host_logf(HOST_LOG_INFO, "prism bake: bsp %d's reach's inputs begun at %.3f s", (int)record->bsp_index,
					now_seconds());
			reach_wait();
			free(bake.reach_input);
			free(bake.tiers);
			free(bake.bricks);
			bake.tiers = NULL;
			bake.bricks = NULL;
			bake.tier_count = bake.brick_count = bake.brick_total = 0;
			bake.reach_whole = 0;
			bake.reach_generation = record->generation;
			bake.reach_bsp = record->bsp_index;
			bake.reach_words = record->total;
			bake.reach_received = 0;
			bake.reach_input = (uint32_t *)calloc((size_t)record->total + 1, 4);
		}
		if (bake.reach_input && data && record->generation == bake.reach_generation && record->bsp_index == bake.reach_bsp &&
			record->first <= bake.reach_words && record->count <= bake.reach_words - record->first)
		{
			memcpy(bake.reach_input + record->first, data, (size_t)record->count * 4);
			bake.reach_received += record->count;
		}
		break;
	case XGPU_BAKE_REACH_END:
		if (bake.reach_input && record->generation == bake.reach_generation && record->bsp_index == bake.reach_bsp &&
			bake.reach_received == bake.reach_words && !bake.reach_running)
		{
			/* (the reach's file: beside the BSP's bake) */
			{
				const char *folder = bake.control.folder ? (const char *)host_pointer(bake.control.folder) : NULL;
				char name[96], map[sizeof(bake.control.map_name) + 1], directory[sizeof(bake.reach_path)];

				memcpy(map, bake.control.map_name, sizeof(bake.control.map_name));
				map[sizeof(bake.control.map_name)] = 0;
				name_clean(map, name, sizeof(name));
				snprintf(directory, sizeof(directory), "%s/%s", folder && *folder ? folder : "prism_bake", name);
				folder_make(directory);
				snprintf(bake.reach_path, sizeof(bake.reach_path), "%s/bsp%d.reach", directory, (int)bake.reach_bsp);
				if (!bake.control_known || !map[0])
					bake.reach_path[0] = 0;
			}
			if (pthread_create(&bake.reach_thread, NULL, reach_thread, NULL) == 0)
				bake.reach_running = 1;
			else
				reach_thread(NULL);
		}
		break;
	default:
		break;
	}
}

/* ---------- PRISM Raster's light */

struct bake_raster_job
{
	VkDeviceAddress pages, charts, vpages, table, tiles, sun, masks, bricks;
	VkDeviceAddress probes, coarse, objects, object_table;
	float camera[4][4];
	int32_t corners[4];
	float volume[4];
	uint32_t counts[4];
	VkDeviceAddress lights, reflections, reflection_table, reflection_data;
	uint32_t more[4];
	/* the exposure's meter, and [0] display.prism_exposure (0 fixed, 1 ce, 2
	auto), [1] display.prism_tone_curve (0 ce, 1 filmic, 2 aces, 3 local), [2]
	1 where this frame meters (the frame whole) */
	VkDeviceAddress meter, meter_pad;
	float exposure[4];
};

/* PRISM v4's exposure's settings from the last RT_FRAME (display.
prism_exposure and its range, speed and bias; display.prism_tone_curve);
"ce" and the clipped curve where none has come */
static void exposure_settings(uint32_t *mode, float values[4], uint32_t *curve)
{
	uint32_t generation, material_count, alpha_words;
	const float *materials;
	const uint32_t *alpha;
	const struct xgpu_stream_rt_frame *frame = NULL;

	xgpu_vulkan_rt_scene(&generation, &materials, &material_count, &alpha, &alpha_words, &frame);
	*mode = 1;
	*curve = 0;
	values[0] = 1.5f;
	values[1] = 2.0f;
	values[2] = 0.0f;
	values[3] = 1.0f / 60.0f;
	if (!frame)
		return;
	*mode = (uint32_t)(frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EXPOSURE] + 0.5f);
	*mode = *mode > 2 ? 1 : *mode;
	*curve = (uint32_t)(frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_TONE_CURVE] + 0.5f);
	*curve = *curve > 3 ? 0 : *curve;
	values[0] = frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EXPOSURE_RANGE];
	values[1] = frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EXPOSURE_SPEED];
	values[2] = frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EXPOSURE_BIAS];
}

VkDeviceAddress xgpu_vulkan_bake_meter(VkCommandBuffer commands)
{
	if (!bake.made)
		return 0;
	if (!bake.meter.buffer && !buffer_filled(commands, NULL, 1024, &bake.meter, 0))
		return 0;
	return bake.meter.address;
}

void xgpu_vulkan_bake_exposure_step(VkCommandBuffer commands)
{
	struct bake_push push;
	uint32_t mode, curve;

	if (!bake.made || !bake.meter.buffer)
		return;
	memset(&push, 0, sizeof(push));
	exposure_settings(&mode, push.values, &curve);
	/* (the seconds since the last step, by the host's clock: at most a
	quarter, a 60th the first time) */
	{
		double now = now_seconds(), since = bake.exposure_clock > 0.0 ? now - bake.exposure_clock : 1.0 / 60.0;

		push.values[3] = (float)(since < 0.0 ? 0.0 : since > 0.25 ? 0.25 : since);
		bake.exposure_clock = now;
	}
	push.scene = bake.meter.address;
	push.parameters[0] = mode;
	barrier(commands);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, bake.pipelines[PIPELINE_EXPOSURE]);
	vkCmdPushConstants(commands, bake.pipeline_layouts[PIPELINE_EXPOSURE], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
		&push);
	vkCmdDispatch(commands, 1, 1, 1);
	barrier(commands);
	/* (the diagnostic log: the exposure (stops) and the medians (log2) an
	earlier step left, the host buffer's, then this step's copied to it) */
	if (!bake.meter_logging)
		bake.meter_logging = getenv("HALO_RASTER_EXPOSURE_LOG") ? 1 : -1;
	if (bake.meter_logging > 0 && (bake.meter_log.buffer || buffer_make(16, 0, 1, &bake.meter_log)))
	{
		const float *words = (const float *)bake.meter_log.mapped;
		VkBufferCopy copy;

		host_logf(HOST_LOG_INFO, "prism raster: frame %u, %.3f s: the exposure %+.2f stops, the lit frame's median "
			"%.4f, Classic's %.4f", bake.raster_frames, bake.exposure_clock, words[0], words[2] < 90.0f ?
			exp2f(words[2]) : 0.0f, words[3] < 90.0f ? exp2f(words[3]) : 0.0f);
		copy.srcOffset = 512;
		copy.dstOffset = 0;
		copy.size = 16;
		vkCmdCopyBuffer(commands, bake.meter.buffer, bake.meter_log.buffer, 1, &copy);
		barrier(commands);
	}
}

struct xgpu_vulkan_image *xgpu_vulkan_bake_raster(struct xgpu_vulkan_image *copy, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t bake_view, int materials, int objects_volume, int *whole)
{
	struct bake_state *b = bake.bake;
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_image_description wanted;
	struct bake_raster_job job;
	VkDescriptorImageInfo images[7];
	VkWriteDescriptorSet writes[7];
	VkDescriptorSet set;
	struct xgpu_vulkan_image *micro;
	VkCommandBuffer commands;
	uint32_t index, flags = 0, light_count = 0, serial = 0, dynamic = 0;
	const struct xgpu_stream_scene_light *lights;
	float dynamic_lights[RASTER_LIGHTS * 12];

	if (whole)
		*whole = 0;
	if (!bake.made || !b || !copy || !base || !additions || !words || !extras || !view || !bake_traceable(b) ||
		(!b->physical_count && !b->volume.done))
		return NULL;
	description = xgpu_vulkan_image_description(copy);
	if (!bake.raster_light || xgpu_vulkan_image_description(bake.raster_light)->width != description->width ||
		xgpu_vulkan_image_description(bake.raster_light)->height != description->height)
	{
		if (bake.raster_light)
			xgpu_vulkan_image_destroy(bake.raster_light);
		memset(&wanted, 0, sizeof(wanted));
		wanted.type = XGPU_VULKAN_IMAGE_2D;
		wanted.format = XGPU_VULKAN_FORMAT_RGBA16F;
		wanted.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
		wanted.samples = 1;
		wanted.width = description->width;
		wanted.height = description->height;
		wanted.depth_or_layers = 1;
		wanted.levels = 1;
		bake.raster_light = xgpu_vulkan_image_create(&wanted);
		if (!bake.raster_light)
			return NULL;
	}
	commands = xgpu_vulkan_compute_commands();
	memset(&job, 0, sizeof(job));
	job.pages = b->pages.address;
	job.charts = b->charts.address;
	job.vpages = b->vpages.address;
	job.table = b->table.address;
	job.tiles = b->pool.address;
	job.sun = b->sun.address;
	job.masks = b->masks.address;
	job.bricks = b->bricks.address;
	job.probes = b->probes.address;
	memcpy(job.camera, view->now, sizeof(job.camera));
	memcpy(job.corners, view->corners, sizeof(job.corners));
	job.volume[0] = PROBE_SPACING;
	job.volume[1] = COARSE_SPACING;
	job.counts[0] = b->probes.buffer && b->volume.done ? b->brick_entries : 0;
	job.counts[1] = description->width;
	job.counts[2] = description->height;
	job.coarse = b->bricks_coarse.address;
	job.counts[3] = b->probes.buffer && b->volume.done ? b->coarse_entries : 0;
	/* (this frame's dynamic lights: SCENE_LIGHTS' dynamic class, those on;
	in the frame whole alone (display.prism_raster_composition "materials"):
	CE's composition takes CE's own dynamic lights' passes (its additions),
	which would count them twice) */
	lights = materials ? xgpu_vulkan_scene_light_table(&light_count, &serial) : NULL;
	for (index = 0; lights && index < light_count && dynamic < RASTER_LIGHTS; index++)
	{
		const struct xgpu_stream_scene_light *l = &lights[index];
		float *to = dynamic_lights + dynamic * 12;

		if (l->light_class != XGPU_SCENE_LIGHT_CLASS_DYNAMIC || !(l->flags & XGPU_SCENE_LIGHT_FLAG_ON) ||
			(l->type != XGPU_SCENE_LIGHT_POINT && l->type != XGPU_SCENE_LIGHT_SPOT) || !(l->position[3] > 0.0f))
			continue;
		memcpy(to, l->position, 4 * sizeof(float));
		to[4] = l->radiance[0] * l->radiance[3];
		to[5] = l->radiance[1] * l->radiance[3];
		to[6] = l->radiance[2] * l->radiance[3];
		to[7] = 0.0f;
		memcpy(to + 8, l->direction, 3 * sizeof(float));
		to[11] = l->type == XGPU_SCENE_LIGHT_SPOT ? l->direction[3] : 2.0f;
		dynamic++;
	}
	job.volume[2] = (float)dynamic;
	/* (the reflection probes, once their buffers are: each used once baked,
	its box empty before) */
	if (b->reflections_made && b->plan.reflection_count && b->reflections_done && !getenv("HALO_RASTER_NO_PROBES"))
	{
		job.reflections = b->reflection_info.address;
		job.reflection_table = b->reflection_table.address;
		job.reflection_data = b->reflection_data.address;
		job.more[0] = b->reflections_done;
		job.more[1] = b->reflection_entries;
		job.more[2] = REFLECTION_LEVELS;
		job.more[3] = REFLECTION_SIDE;
	}
	job.lights = dynamic ? b->raster_job.address + RASTER_LIGHTS_OFFSET : 0;
	/* (port: the frame's moving objects' structure, display.prism_raster_
	object_shadows "traced": their sun shadows traced against it in the frame
	whole, CE's projected ones on the level left out; none in the bake's
	views) */
	if (!bake_view)
		(void)xgpu_vulkan_rt_objects(&job.objects, &job.object_table, NULL);
	/* (the exposure: metered and applied in the frame whole, not in the bake's
	views, the gates' measures) */
	{
		uint32_t mode, curve;
		float values[4];

		exposure_settings(&mode, values, &curve);
		job.meter = xgpu_vulkan_bake_meter(commands);
		job.exposure[0] = (float)mode;
		job.exposure[1] = (float)curve;
		job.exposure[2] = !bake_view && job.meter ? 1.0f : 0.0f;
	}
	if (!upload(commands, &b->raster_job, 0, &job, sizeof(job)) ||
		(dynamic && !upload(commands, &b->raster_job, RASTER_LIGHTS_OFFSET, dynamic_lights, dynamic * 12 * sizeof(float))))
		return NULL;
	xgpu_vulkan_image_compute_use(copy, 0);
	xgpu_vulkan_image_compute_use(words, 0);
	xgpu_vulkan_image_compute_use(extras, 0);
	xgpu_vulkan_image_compute_use(base, 0);
	xgpu_vulkan_image_compute_use(additions, 0);
	/* (the terrain's prints' micro-shadows where the terrain pass drew them
	this frame, else a white texel: xgpu_vulkan_terrain.c) */
	micro = xgpu_vulkan_terrain_micro(description->width, description->height);
	if (!micro)
	{
		if (!bake.micro_white)
		{
			static const uint8_t white[4] = { 255, 255, 255, 255 };
			struct xgpu_vulkan_image_description one;

			memset(&one, 0, sizeof(one));
			one.type = XGPU_VULKAN_IMAGE_2D;
			one.format = XGPU_VULKAN_FORMAT_RGBA8;
			one.usage = XGPU_VULKAN_USAGE_SAMPLED;
			one.width = one.height = 1;
			one.depth_or_layers = 1;
			one.levels = 1;
			bake.micro_white = xgpu_vulkan_image_create(&one);
			if (!bake.micro_white)
				return NULL;
			xgpu_vulkan_image_upload(bake.micro_white, 0, 0, 0, 0, 0, 1, 1, 1, white, 4, 4);
		}
		micro = bake.micro_white;
	}
	xgpu_vulkan_image_compute_use(micro, 0);
	xgpu_vulkan_image_compute_use(bake.raster_light, 1);
	set = bake.raster_sets[bake.raster_set_next];
	bake.raster_set_next = (bake.raster_set_next + 1) & 3;
	memset(images, 0, sizeof(images));
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < 7; index++)
	{
		struct xgpu_vulkan_image *image = index == 0 ? words : index == 1 ? extras : index == 2 ? copy : index == 3 ?
			bake.raster_light : index == 4 ? base : index == 5 ? additions : micro;

		images[index].sampler = index != 3 ? bake.sampler : VK_NULL_HANDLE;
		images[index].imageView = xgpu_vulkan_image_view(image);
		images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = index != 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[index].pImageInfo = &images[index];
	}
	vkUpdateDescriptorSets(bake.device.device, 7, writes, 0, NULL);
	if (b->block.rig[1][3] > 1.001f)
		flags |= 1u;
	/* (the frame whole, its own materials composing it (display.
	prism_raster_composition "materials"), but for the bake's views; else
	the light alone, CE's composition) */
	if (materials && (!bake_view || bake_view >= XGPU_GBUFFER_VIEW_RASTER_ALBEDO))
		flags |= 2u;
	/* (display.prism_raster_objects "volume": an object's light the volume's
	alone, not held at Classic's) */
	if (objects_volume)
		flags |= 4u;
	barrier(commands);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, bake.pipelines[PIPELINE_RASTER]);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, bake.pipeline_layouts[PIPELINE_RASTER], 0, 1, &set, 0,
		NULL);
	{
		struct bake_push push;

		memset(&push, 0, sizeof(push));
		push.scene = scene_runtime_address(b);
		push.job = b->raster_job.address;
		push.parameters[0] = bake_view;
		push.parameters[1] = b->plan.page_count;
		push.parameters[2] = bake.raster_frames++;
		push.parameters[3] = flags;
		vkCmdPushConstants(commands, bake.pipeline_layouts[PIPELINE_RASTER], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
			&push);
	}
	xgpu_vulkan_breadcrumb(commands, 62, "Rebaked's light pass");
	vkCmdDispatch(commands, (description->width + 7) / 8, (description->height + 7) / 8, 1);
	barrier(commands);
	if (!bake_view)
		xgpu_vulkan_bake_exposure_step(commands);
	if (whole)
		*whole = !bake_view && materials;
	return bake.raster_light;
}

int xgpu_vulkan_bake_lookup(struct xgpu_vulkan_bake_lookup *lookup)
{
	struct bake_state *b = bake.bake;

	memset(lookup, 0, sizeof(*lookup));
	if (!bake.made || !bake_traceable(b) || !b->block.tlas)
		return 0;
	lookup->scene = scene_runtime_address(b);
	lookup->pages = b->pages.address;
	lookup->charts = b->charts.address;
	lookup->vpages = b->vpages.address;
	lookup->table = b->table.address;
	lookup->tiles = b->pool.address;
	lookup->indirect = b->pool_indirect.address;
	lookup->sun = b->sun.address;
	lookup->masks = b->masks.address;
	lookup->lightmaps = b->lightmaps.address;
	lookup->page_count = b->plan.page_count;
	lookup->tiles_done = b->tiles_done;
	lookup->tiles_planned = b->tiles_planned;
	if (b->probes.buffer && b->volume.done)
	{
		lookup->bricks = b->bricks.address;
		lookup->coarse = b->bricks_coarse.address;
		lookup->probes = b->probes.address;
		lookup->brick_entries = b->brick_entries;
		lookup->coarse_entries = b->coarse_entries;
	}
	lookup->probe_spacing = PROBE_SPACING;
	lookup->coarse_spacing = COARSE_SPACING;
	if (b->reflections_made && b->plan.reflection_count && b->reflections_done)
	{
		lookup->reflections = b->reflection_info.address;
		lookup->reflection_table = b->reflection_table.address;
		lookup->reflection_data = b->reflection_data.address;
		lookup->reflection_count = b->reflections_done;
		lookup->reflection_entries = b->reflection_entries;
		lookup->reflection_levels = REFLECTION_LEVELS;
		lookup->reflection_side = REFLECTION_SIDE;
	}
	lookup->generation = b->generation;
	lookup->complete = b->step == STEP_DONE;
	lookup->gamma = b->block.rig[1][3] > 1.001f ? b->block.rig[1][3] : 1.0f;
	return 1;
}

int xgpu_vulkan_bake_sun(float sun[4])
{
	struct bake_state *b = bake.bake;

	if (!bake.made || !b || !(b->block.sun[3] > 0.0f))
		return 0;
	memcpy(sun, b->block.sun, 4 * sizeof(float));
	return 1;
}

void xgpu_vulkan_bake_shutdown(void)
{
	uint32_t index;

	if (!bake.made)
		return;
	if (bake.bake)
		bake_free(bake.bake);
	bake.bake = NULL;
	for (index = 0; index < READBACK_SLOTS; index++)
		buffer_free_now(&bake.readbacks[index].buffer);
	for (index = 0; index < STAGING_SLOTS; index++)
		buffer_free_now(&bake.staging[index]);
	buffer_free_now(&bake.meter);
	buffer_free_now(&bake.meter_log);
	while (bake.garbage_count)
	{
		bake.garbage_count--;
		if (bake.garbage[bake.garbage_count].structure)
			vkDestroyAccelerationStructureKHR(bake.device.device, bake.garbage[bake.garbage_count].structure, NULL);
		buffer_free_now(&bake.garbage[bake.garbage_count].buffer);
	}
	if (bake.raster_light)
		xgpu_vulkan_image_destroy(bake.raster_light);
	bake.raster_light = NULL;
	for (index = 0; index < PIPELINES; index++)
	{
		if (bake.pipelines[index])
			vkDestroyPipeline(bake.device.device, bake.pipelines[index], NULL);
		if (bake.pipeline_layouts[index])
			vkDestroyPipelineLayout(bake.device.device, bake.pipeline_layouts[index], NULL);
		if (bake.layouts[index])
			vkDestroyDescriptorSetLayout(bake.device.device, bake.layouts[index], NULL);
	}
	if (bake.pool)
		vkDestroyDescriptorPool(bake.device.device, bake.pool, NULL);
	if (bake.sampler)
		vkDestroySampler(bake.device.device, bake.sampler, NULL);
	if (bake.queries)
		vkDestroyQueryPool(bake.device.device, bake.queries, NULL);
	free(bake.tiers);
	free(bake.bricks);
	memset(&bake, 0, sizeof(bake));
}
