/*
XGPU_VULKAN_PT.C

PRISM Path Traced (overhaul_research/prism_v4_renderer/design.md, sections
1.5 and 3.8; docs/prism_pathtraced.md), in the Vulkan backend: display.
lighting "pathtraced", the deferred core's opaque pixels lit by the path
tracer, every CE light off. It traces the scene the bake was made from, by
the bake's own functions (xgpu_vulkan_pt.glsl, built on the baker's
BAKE_COMMON), and reads the bake where the bake has the light already:

  light: at each opaque pixel of the G-buffer, the sun (a ray near the eye
    and on the objects, the baked sun mask beyond), the sky's domes, the
    lamps, the fixtures and the dynamic lights (resampled importance over
    candidates from the bake's light-importance grid, one shadow ray), the
    ambient floor, the indirect light (the bake's indirect layer at the
    pixel in "performance" where the level has its texel, else a bounce
    whose hit is lit from the bake) and the reflections (a GGX ray below a
    roughness, its hit lit from the bake; the bake's probes above it);
  the denoiser (ReLAX-class, written from the published methods): temporal
    (a slow and a fast history, the fast one holding the slow one, the
    moments' variance, a firefly clamp, no history across a change of
    instance, normal or plane: none through an opening), fix (a wide gather
    where the history is short), atrous (a-trous steps stopped at the
    plane, the normal, the instance and the luminance by the variance), and
    the sun's visibility through its own history and a filter as wide as
    its penumbra;
  composite: the frame, Raster's form (the diffuse light by the base colour
    less the specular's share, the specular light, the emissive light).

debug.pt_reference in this mode (RT_FRAME's request, prism_rt.c) traces the
same at every pixel unbiased with no bake (the pass "reference"), summed
until its estimated relative MSE is under the gates' bound (0.001, docs/
prism_gates.md) or its frames are done, then written as a linear OpenEXR
file and held.

Each pass's GPU time is measured by timestamps (HALO_PT_TIMES_FILE: a line
a frame; the log: their means every 600 frames).
*/

#include "xgpu_vulkan.h"
#include "xgpu_vulkan_pt.h"
#include "xgpu_vulkan_pt_fit.h"
#include "xgpu_vulkan_pt_shaders.h"
#include "xgpu_vulkan_bake.h"
#include "xgpu_vulkan_rt.h"
#include "xgpu_vulkan_scene.h"
#include "xgpu_vulkan_upscale.h"
#include "host.h"
#include "xgpu_exr.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- constants */

/* the job blocks' ring (each a frame's: the block, then its dynamic
lights, then the recent ones (the history's), LIGHT_FLOATS each), the
descriptor sets' */
#define JOB_SLOTS 16u
#define JOB_BYTES 8192u
#define JOB_LIGHTS_OFFSET 1024u
#define LIGHTS_MOST 64u
#define LIGHT_FLOATS 12u
/* the frame's counters (xgpu_vulkan_pt.glsl's Counters: the history's
diagnostics), four words in the job's block before its lights */
#define JOB_COUNTERS_OFFSET 960u
#define SET_SLOTS 16u
/* the bindings (xgpu_vulkan_pt.glsl's) */
#define BINDINGS 46u
/* the reference's floats a pixel (the GLSL's SUMS), its noise's bound (the
gates' reference_noise, as the gates' frames measure it), the frames
between its noise's measures, the fewest it sums, the gates' frames' width
(tools/prism_gate/metrics.py's SIZE: every frame brought to 960 x 540) */
#define SUMS 16u
#define REFERENCE_NOISE 0.001
#define REFERENCE_MEASURE 32u
#define REFERENCE_LEAST 256u
#define GATE_WIDTH 960.0
/* the reference's light tree (xgpu_vulkan_pt.glsl's Tree): the lamps the
copy holds at most, a node's floats */
#define LAMPS_MOST 65536u
#define TREE_NODE_FLOATS 16u
/* the radiance cache: its cells (a power of two) and each one's words
(xgpu_vulkan_pt.glsl's CACHE_WORDS), whether it is on by default, and the
indirect light's clamp by default (0 none). On since Path Traced reads no
baked light (2026-11-27): its cells carry the bounces past the first, which
the bake's light at a bounce's hit carried before (HALO_PT_CACHE=0 off: one
bounce, its hit lit directly). Its words since the pixels' cells (the
temporal pass's prior): 20, the pixels' light beside the hits'; a short
history's cap at least this many times its cell's mean by default
(HALO_PT_PRIOR_CAP, in hundredths; 0: no pixels' cells). The blocks of
pixels a feeding one (a power of two, HALO_PT_CACHE_FEED) */
#define CACHE_CELLS (1u << 19)
#define CACHE_WORDS 20u
#define CACHE_DEFAULT 1
#define PRIOR_CAP_DEFAULT 800
#define CACHE_FEED_DEFAULT 64
/* ReSTIR DI's initial candidates taken from the light tree, the rest from
the grid's lists (HALO_PT_TREE_CANDIDATES): all 4 since the dark interiors'
second round. With the specular lobe in the tree's importance (tree_lobe)
the tree finds the lamps whose glints a semi-glossy wall shows and the grid's
lists, ranked by each lamp's light at the cell's few points, do not: at
720p against R, c10's lab still error 0.045 to 0.015 and moving 0.084 to
0.036, a10's door windows moving 0.112 to 0.030, b30's G4 moving 0.156 to
0.076 and 0.116 to 0.060, the frames' energy back to within 5% of R's. 1 to
4 measured in v4_dark_interiors_runs (U1 to U4). Its cost is a descent of
the tree a candidate in place of a cell's lookup: the light pass about 8 to
16% longer a tree candidate (whole-card Atlas runs, the light pass against
the denoiser's passes in the same run), so 4 cost it about 30 to 50% (an
estimate of +3 ms of the RTX 3070 Ti's at G4, 1080p; not yet timed on it).
2 in the performance profile were tried and not kept: b30's G4 frames
darker than prism-9's (energy 0.70 to 0.77 of R's); one descent shared by
the 4 was slower (its stack in local memory) */
#define TREE_CANDIDATES_DEFAULT 4
/* whether the tree's importance takes its nodes' emitter cones (round 6:
1.5 ms of the RTX 3070 Ti's light pass at G4, 1080p) */
#define TREE_CONES_DEFAULT 0
#define INDIRECT_CLAMP_DEFAULT 0.0f
/* the a-trous steps by default (HALO_PT_ATROUS_STEPS), and whether four of
them take strides 1, 2, 4 and 16 (HALO_PT_ATROUS_WIDE; else 1 to 8) */
#define ATROUS_STEPS_DEFAULT 4
#define ATROUS_WIDE_DEFAULT 1
/* the temporal stabilisation's history at most by default (HALO_PT_STABILIZE,
frames; 0 none); the
slow history's frames at most, a moving eye's and a
still one's (HALO_PT_HISTORY, HALO_PT_HISTORY_STILL) and ReSTIR DI's
reservoirs' confidence at most for a still eye (HALO_PT_RESTIR_M_STILL) */
#define STABILIZE_DEFAULT 0
#define HISTORY_DEFAULT 32
#define HISTORY_STILL_DEFAULT 128
#define RESTIR_M_STILL_DEFAULT 20
/* how the temporal pass holds a moving eye's glossy specular history by
default (HALO_PT_SPEC_HOLD): 0 not, 1 to this frame's 3 x 3, 2 to the 3 x 3
and the 5 x 5's four along the axes (since the dark interiors' third round:
the 3 x 3's sparse samples held a lamp's pools darker than they are while
the eye moved) */
#define SPEC_HOLD_DEFAULT 2
/* the histories' anti-firefly's bound by default, in standard deviations of
the neighbours' (HALO_PT_HIST_FIREFLY; 0 none): over the 240 moving frames
of c40's lab the bright pixels the native frame has not fell 1.8 times
(DLSS 0.33), 2.8 times (DLSS 0.67 with the light at 0.33 of the screen) and
7.7 times (DLAA with the light at 0.5), and the glows that stayed 8 frames
and more in one place to none; against the supersampled
references every pose's FLIP and edge SSIM are better, its ghosting less
(a10's corridor native: FLIP 0.087 to 0.069, ghosting 17.5 to 11.6), b30's
beach unchanged. 1 measured a little better again and 2 or 3 a little
worse; 1.5 keeps more of a small bright feature's peak than 1
(work/openh1_eval/v4_pt_clarity_runs/r3) */
#define HIST_FIREFLY_DEFAULT 1.5f
/* ReSTIR DI by default: the spatial neighbours a pixel
(HALO_PT_RESTIR_SPATIAL), the initial candidates where they are reused
(HALO_PT_CANDIDATES; 8 without them), their disc's radius in pixels
(HALO_PT_RESTIR_RADIUS) */
#define RESTIR_SPATIAL_DEFAULT 1
#define CANDIDATES_SPATIAL_DEFAULT 4
#define RESTIR_RADIUS_DEFAULT 16
/* whether ReSTIR DI's light-tree candidate takes the surface's specular lobe
by default (HALO_PT_TREE_LOBE; xgpu_vulkan_pt.glsl's tree_lobe) */
#define TREE_LOBE_DEFAULT 1
/* the lamp ray's roughness at most by default (HALO_PT_LAMP_RAY); the job
block's flag (xgpu_vulkan_pt.glsl's FLAG_LAMP_MIS): the lamps' specular light
shared between ReSTIR DI and the reflection's ray by the chance ReSTIR DI
finds each lamp (HALO_PT_LAMP_MIS, on by default; 0: by its solid angle alone,
as before) */
#define LAMP_RAY_DEFAULT 0.0
#define FLAG_LAMP_MIS 256u
/* the job block's flag of the bake's guide (xgpu_vulkan_pt.glsl's FLAG_BAKE:
display.prism_pt_bake, the drawn world's own whole bake read where the
traced light has nothing yet; HALO_PT_BAKE=0 or 1 overrides the setting, a
diagnosis) */
#define FLAG_BAKE 1024u
/* (with it, the bake's light-importance grid: HALO_PT_BAKE_GRID, off by
default, the passes' specialization constant SPEC_GRID: spec_options) */
#define BAKE_GRID_DEFAULT 0
/* the short history's cap, times its neighbours' brightest, and the light
below which it holds nothing, in eighths of the light shown as white
(xgpu_vulkan_pt_frame) */
#define SHORT_CAP_DEFAULT 4
#define SHORT_CAP_FLOOR_DEFAULT 32
/* the light pass's specialization constants (xgpu_vulkan_pt.glsl's
SPEC_*); whether half the level's 8 x 8 blocks (1) or half its pixels (2)
take the bake's indirect light as their bounce by default
(HALO_PT_BOUNCE_HALF, 0 none) */
#define SPEC_COUNT 9u
/* whether the sun's ray is left out in a tile the bake found all in shadow
by default (HALO_PT_SUN_TILES) */
#define SUN_TILES_DEFAULT 1
#define BOUNCE_HALF_DEFAULT 0
/* whether ReSTIR GI's temporal reuse is on by default (HALO_PT_RESTIR_GI) */
#define RESTIR_GI_DEFAULT 0
/* ReSTIR DI's reuse flags (xgpu_vulkan_pt.glsl's REUSE_*): the spatial
reservoirs kept for the next frame, the temporal reuse by a permutation;
the boiling filter's strength in hundredths above them, the disc's radius
above that. By default: whether the permutation is on, the boiling
filter's strength (0 none) */
/* (diagnoses, not for play: FEEDBACK brings back round 6's blobs in motion,
and PERMUTE is biased, a reservoir from another pixel counted without its
support there: G4's first pose 0.0957 to 0.1583, darker) */
#define REUSE_FEEDBACK 1u
#define REUSE_PERMUTE 2u
#define REUSE_TREE_LOBE 4u
#define RESTIR_PERMUTE_DEFAULT 0
#define RESTIR_BOIL_DEFAULT 0.0f
/* the passes timed, the timestamps a frame (a begin and each pass's end),
the frames' slots of them */
#define TIMED 5u
#define STAMPS (TIMED + 1u)
#define TIME_SLOTS 8u

enum
{
	PASS_LIGHT,
	PASS_TEMPORAL,
	PASS_FIX,
	PASS_ATROUS,
	PASS_COMPOSITE,
	PASS_REFERENCE,
	PASS_REFERENCE_STATS,
	PASS_LAMPS,
	PASS_CACHE,
	PASS_WATER,
	PASS_STABILIZE,
	PASS_UPSAMPLE,
	PASSES
};

/* the images: their bindings */
enum
{
	IMAGE_NOISY_DIFFUSE = 4,
	IMAGE_NOISY_SPECULAR,
	IMAGE_CLEAN_DIFFUSE,
	IMAGE_CLEAN_SPECULAR,
	IMAGE_SUN_RAW,
	IMAGE_GUIDE_NOW,
	IMAGE_GUIDE_LAST,
	IMAGE_DIFF_LAST,
	IMAGE_DIFF_NOW,
	IMAGE_SPEC_LAST,
	IMAGE_SPEC_NOW,
	IMAGE_MOMENTS_LAST,
	IMAGE_MOMENTS_NOW,
	IMAGE_SUN_LAST,
	IMAGE_SUN_NOW,
	IMAGE_DIFF_A,
	IMAGE_DIFF_B,
	IMAGE_SPEC_A,
	IMAGE_SPEC_B,
	IMAGE_SUN_A,
	IMAGE_SUN_B,
	IMAGE_FRAME,
	IMAGE_LIGHT_IN,
	IMAGE_SPEC_MOMENTS_LAST,
	IMAGE_SPEC_MOMENTS_NOW,
	IMAGE_RESERVOIR_POINT_LAST,
	IMAGE_RESERVOIR_POINT_NOW,
	IMAGE_RESERVOIR_INFO_LAST,
	IMAGE_RESERVOIR_INFO_NOW,
	IMAGE_GI_POINT_LAST,
	IMAGE_GI_POINT_NOW,
	IMAGE_GI_DATA_LAST,
	IMAGE_GI_DATA_NOW,
	/* the frame's exposure and curve (1x1), for a plugin that upscales */
	IMAGE_EXPOSURE,
	/* the temporal stabilisation's histories (the pass "stabilize") */
	IMAGE_STAB_DIFF_LAST,
	IMAGE_STAB_DIFF_NOW,
	IMAGE_STAB_SPEC_LAST,
	IMAGE_STAB_SPEC_NOW,
	/* the traced light's upscaling: the light brought to the G-buffer's
	grid (the pass "upsample") */
	IMAGE_UP_DIFF,
	IMAGE_UP_SPEC,
	IMAGE_UP_CLEAN_DIFFUSE,
	IMAGE_UP_CLEAN_SPECULAR,
};

/* the job block's flags (xgpu_vulkan_pt.glsl's FLAG_*) */
enum
{
	FLAG_HISTORY = 1,
	FLAG_PERFORMANCE = 2,
	FLAG_DENOISER_OFF = 4,
	FLAG_REFERENCE = 8,
	/* (HALO_PT_PROBE_NEE=1, a diagnosis: the pixels whose reflection the
	probes give keep the lamps' specular light by ReSTIR DI as well, counted
	twice) */
	FLAG_PROBE_NEE = 32,
	/* (HALO_PT_PROBE_NEE=2, a diagnosis: the lamps' specular light from the
	probes alone on their pixels, none by ReSTIR DI) */
	FLAG_PROBE_ONLY = 128,
};

#define PT_FUNCTIONS(F) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) \
	F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) F(vkCreateShaderModule) F(vkDestroyShaderModule) \
	F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
	F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) \
	F(vkCmdDispatch) F(vkCmdPushConstants) F(vkCmdPipelineBarrier) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults) \
	F(vkGetBufferDeviceAddress) F(vkCmdCopyBuffer) F(vkCmdFillBuffer)

#define DECLARE(name) static PFN_##name name;
PT_FUNCTIONS(DECLARE)
#undef DECLARE

/* ---------- structures */

struct pt_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkDeviceAddress address;
	void *mapped;
	/* what it is counted under (xgpu_vulkan_memory_counted) */
	int use;
	uint32_t type;
	VkDeviceSize counted;
};

/* the use the next buffer_make counts under (the radiance cache's sets it) */
static int pt_memory_use = XGPU_VULKAN_MEMORY_PT_OTHER;

/* the job block (xgpu_vulkan_pt.glsl's Job, field for field) */
struct pt_job
{
	VkDeviceAddress pages, charts, vpages, table, tiles, indirect, sun, masks;
	VkDeviceAddress bricks, coarse, probes, lightmaps, reflections, reflection_table, reflection_data, lights;
	VkDeviceAddress sums, stats, meter, counters, tree, lamps_out, objects, object_table;
	/* the radiance cache (its cells; 0 off); ReSTIR DI's reuse flags
	(REUSE_*); a diagnosis's parts of the light pass left out (PARTS_*) */
	VkDeviceAddress cache;
	uint32_t reuse_flags, parts_off;
	float camera[4][4];
	float last_camera[4][4];
	int32_t corners[4];
	int32_t last_corners[4];
	uint32_t counts[4];
	uint32_t more[4];
	float volume[4];
	uint32_t frame[4];
	float settings[4];
	float denoise[4];
	float extra[4];
	/* display.prism_exposure (0 fixed, 1 ce, 2 auto), display.prism_tone_curve
	(0 ce, 1 filmic, 2 aces, 3 local), 1 where the frame meters */
	float exposure[4];
	/* the indirect light's clamp (a bounce's luminance, 0 none), the radiance
	cache's cell at a unit's distance, the samples a cell needs before it is
	read, the most it keeps */
	float cache_settings[4];
	/* the traced light's upscaling: 1 where the job's passes run on the
	light's own grid, the G-buffer's width and height */
	float low[4];
};

struct pt_push
{
	VkDeviceAddress scene, job;
	uint32_t parameters[4];
	float values[4];
};

/* ---------- globals */

static struct
{
	int made, failed;
	/* the passes' pipelines made on a thread of their own (compile_thread:
	compile_running while it runs, compile_done set as it ends, compile_ok
	if every one was made), so that a first change to Path Traced never
	holds the game while the driver compiles them (cold, 47 s on Adam's RTX
	3070 Ti, 118 s on a busy RTX 3090, the game frozen throughout): CE's
	frame meanwhile, as before a scene is made */
	pthread_t compile_thread;
	int compile_running, compile_done, compile_ok;
	double compile_started;
	struct xgpu_vulkan_rt_device device;
	VkDescriptorSetLayout layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipelines[PASSES];
	VkDescriptorPool pool;
	VkDescriptorSet sets[SET_SLOTS];
	uint64_t set_serial[SET_SLOTS];
	uint32_t set_next;
	VkSampler sampler;
	struct pt_buffer jobs[JOB_SLOTS];
	uint64_t job_serial[JOB_SLOTS];
	uint32_t job_next;
	/* each job slot's frame, for the diagnostics' file (HALO_PT_DEBUG_FILE:
	a line a frame once it is done): its number, flags, the view's flags,
	how far the eye moved since the last frame's camera */
	uint32_t job_frame[JOB_SLOTS], job_flags[JOB_SLOTS], job_view_flags[JOB_SLOTS];
	float job_moved[JOB_SLOTS];
	FILE *debug_file;
	int debug_file_tried;
	/* the images at the window's size (each its binding's), the last
	frame's and this frame's histories by parity */
	uint32_t width, height;
	struct xgpu_vulkan_image *noisy_diffuse, *noisy_specular, *clean_diffuse, *clean_specular, *sun_raw;
	struct xgpu_vulkan_image *guide[2], *diff[2], *spec[2], *moments[2], *spec_moments[2], *sun[2];
	struct xgpu_vulkan_image *reservoir_point[2], *reservoir_info[2];
	/* ReSTIR GI's reservoirs (the bounce's sample: its point and normal; its
	radiance, confidence and weight), the last frame's and this one's */
	struct xgpu_vulkan_image *gi_point[2], *gi_data[2];
	struct xgpu_vulkan_image *diff_a, *diff_b, *spec_a, *spec_b, *sun_a, *sun_b, *out;
	/* the temporal stabilisation's histories, by parity */
	struct xgpu_vulkan_image *stab_diff[2], *stab_spec[2];
	/* the traced light's upscaling (light_scale): the light's grid's size
	(the G-buffer's where it is off), the light brought to the G-buffer's
	grid (1 x 1 where it is off), the scale last logged */
	uint32_t low_width, low_height;
	struct xgpu_vulkan_image *up_diff, *up_spec, *up_clean_diffuse, *up_clean_specular;
	float low_logged;
	/* the light's grid a shortage of the card's memory chose for the
	window's size (images_fit, xgpu_vulkan_pt_fit.h) */
	struct pt_fit_state memory;
	uint32_t memory_freed, memory_checked, frames_undrawn;
	/* the frame's exposure and curve (1x1: xgpu_vulkan_upscale_pt_frame's);
	whether the last frame gave its light raw to display.denoiser's plugin
	(the denoiser's images were then its guides: no history across a change) */
	struct xgpu_vulkan_image *exposure;
	int raw_last;
	/* (the bake's guide as last logged: 1 off, 2 wanted with no whole bake,
	3 on; 0 not yet) */
	uint32_t bake_logged;
	/* the last frame's jitter (pixels of its view's target: an upscaler's,
	xgpu_vulkan_upscale_jitter; 0 none), its camera's rays turned by it */
	float last_jitter[2];
	uint32_t parity;
	int history_valid;
	uint32_t frame;
	uint64_t scene_seen;
	/* the bake's scene's generation the last frame traced (a new set's scene,
	the world's geometry changed in play, never takes the history of
	another's, whatever its address), and whether the last frame traced took
	its history (xgpu_vulkan_pt_history_kept: the denoiser's plugin drops
	its own where this did) */
	uint32_t generation_seen;
	int history_kept;
	/* the scene's generation last logged, + 1 (0 none) */
	uint32_t generation_logged;
	uint64_t last_serial;
	/* the timing: a query pool of TIME_SLOTS frames' STAMPS, each slot's
	submission (0 free), the means' sums */
	VkQueryPool queries;
	double timestamp_period;
	uint64_t time_serial[TIME_SLOTS];
	uint32_t time_next;
	double time_sums[TIMED + 1];
	uint32_t time_frames;
	FILE *times_file;
	int times_file_tried;
	/* the reference: its sums (SUMS floats a pixel), its noise's partial
	sums and their readback, the sums' readback for the file */
	struct pt_buffer sums, stats, stats_readback, sums_readback;
	uint32_t ref_frames, ref_count, ref_generation, ref_width, ref_height, ref_paths;
	float ref_camera[4][4];
	uint64_t ref_scene;
	int ref_seen, ref_stopped, ref_converged, ref_copied;
	uint64_t ref_stats_serial, ref_sums_serial;
	uint32_t ref_stats_frames, ref_stats_groups;
	double ref_noise, ref_noise_hdr;
	/* the reference's light tree: the bake's lamps' copy for the host (its
	submission, the scene's generation it is of), the tree (its nodes) and
	the generation it was built for */
	struct pt_buffer lamps_readback, tree;
	/* the radiance cache's cells (made once; emptied for each bake's scene
	generation) */
	struct pt_buffer cache;
	uint32_t cache_generation;
	int cache_made;
	uint64_t lamps_serial;
	uint32_t lamps_generation, tree_generation, tree_nodes;
	int tree_failed;
	char ref_path[XGPU_RT_REFERENCE_PATH];
	int logged_first, logged_no_bake;
	/* the recent dynamic lights (the temporal pass's history cut where
	they light the surface otherwise than this frame's): each one of the
	frames before, by its id, where it was last frame, its colour the most
	of its last frame's and its recent colour less a history's share a frame
	(denoise.x: what the history may still hold of it), until that is gone */
	float recent_lights[LIGHTS_MOST * LIGHT_FLOATS];
	uint32_t recent_ids[LIGHTS_MOST];
	uint32_t recent_count;
	/* the terrain overhaul's water rays (T8, xgpu_vulkan_pt_water): this
	frame's job (its block's address, the scene, the images bound, 0 until
	a frame is traced and again at the frame's end), the rays' light (the
	upper and the lower ray's) and the reference's sums of them */
	VkDeviceAddress water_job;
	uint64_t water_scene;
	int water_reference;
	struct xgpu_vulkan_image *water_bound[BINDINGS];
	struct xgpu_vulkan_image *water_up, *water_down, *water_sum_up, *water_sum_down, *water_aux, *water_sum_aux;
} pt;

/* ---------- memory */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < pt.device.memory->memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (pt.device.memory->memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int buffer_make(VkDeviceSize size, int host, struct pt_buffer *made)
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
	info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(pt.device.device, &info, NULL, &made->buffer) != VK_SUCCESS)
		return 0;
	vkGetBufferMemoryRequirements(pt.device.device, made->buffer, &requirements);
	type = memory_type(requirements.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	memset(&flags, 0, sizeof(flags));
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &flags;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || vkAllocateMemory(pt.device.device, &allocate, NULL, &made->memory) != VK_SUCCESS)
	{
		vkDestroyBuffer(pt.device.device, made->buffer, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	if (vkBindBufferMemory(pt.device.device, made->buffer, made->memory, 0) != VK_SUCCESS ||
		(host && vkMapMemory(pt.device.device, made->memory, 0, VK_WHOLE_SIZE, 0, &made->mapped) != VK_SUCCESS))
	{
		vkDestroyBuffer(pt.device.device, made->buffer, NULL);
		vkFreeMemory(pt.device.device, made->memory, NULL);
		memset(made, 0, sizeof(*made));
		return 0;
	}
	made->size = size;
	made->use = pt_memory_use;
	made->type = (uint32_t)type;
	made->counted = requirements.size;
	xgpu_vulkan_memory_counted(made->use, made->type, (int64_t)made->counted);
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address.buffer = made->buffer;
	made->address = vkGetBufferDeviceAddress(pt.device.device, &address);
	return 1;
}

/* let go of once the GPU is done with every submission so far (the
reference's buffers, made again rarely: when the window's size changes) */
static void buffer_free(struct pt_buffer *buffer)
{
	if (!buffer->buffer)
		return;
	xgpu_vulkan_wait(xgpu_vulkan_serial() > 0 ? xgpu_vulkan_serial() - 1 : 0);
	xgpu_vulkan_memory_counted(buffer->use, buffer->type, -(int64_t)buffer->counted);
	vkDestroyBuffer(pt.device.device, buffer->buffer, NULL);
	vkFreeMemory(pt.device.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

/* the passes' names, for a lost device's report (xgpu_vulkan_breadcrumb:
each pass's id its index + 1; the frame's begun and done BREADCRUMB_BEGUN
and BREADCRUMB_DONE) */
static const char *const pass_names[] =
{
	"Path Traced's light pass", "Path Traced's temporal pass", "Path Traced's fix pass", "Path Traced's atrous pass",
	"Path Traced's composite pass", "Path Traced's reference pass", "Path Traced's reference statistics",
	"Path Traced's lamps copy", "Path Traced's radiance cache", "Path Traced's water rays",
	"Path Traced's stabilisation", "Path Traced's light upsampling",
};
typedef char pass_names_complete[sizeof(pass_names) / sizeof(pass_names[0]) == PASSES ? 1 : -1];
#define BREADCRUMB_BEGUN 20u
#define BREADCRUMB_DONE 21u

/* the subgroup operations the passes use in compute (xgpu_vulkan_pt.glsl's
temporal pass; BAKE_COMMON's shaders) */
#define PT_SUBGROUP_OPERATIONS (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | \
	VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT)

static void barrier(VkCommandBuffer commands)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
}

/* the frame's writes the host reads once it is done (the counters, the
reference's readbacks) made visible to it */
static void host_barrier(VkCommandBuffer commands)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory, 0, NULL, 0, NULL);
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
	if (vkCreateShaderModule(pt.device.device, &info, NULL, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return module;
}

/* an option's variable: its integer, else the default */
static int env_int(const char *name, int otherwise)
{
	const char *value = getenv(name);

	return value && value[0] ? atoi(value) : otherwise;
}

/* the passes' specialization constants (xgpu_vulkan_pt.glsl's SPEC_*):
the options whose code an off option leaves out of the light pass, read
from the same variables and defaults as the job's fields (the radiance
cache, the indirect clamp, the tree's emitter cones, the spatial
neighbours, the parts diagnosis, the boiling filter, the bounce from the
bake on half the blocks (HALO_PT_BOUNCE_HALF), the sun's ray left out in
the bake's dark tiles (HALO_PT_SUN_TILES)) */
static void spec_options(uint32_t values[SPEC_COUNT])
{
	const char *clamp = getenv("HALO_PT_INDIRECT_CLAMP");
	const char *boil = getenv("HALO_PT_RESTIR_BOIL");
	int spatial = env_int("HALO_PT_RESTIR_SPATIAL", RESTIR_SPATIAL_DEFAULT);

	values[0] = env_int("HALO_PT_CACHE", CACHE_DEFAULT) != 0;
	values[1] = (clamp ? (float)atof(clamp) : INDIRECT_CLAMP_DEFAULT) > 0.0f;
	values[2] = env_int("HALO_PT_TREE_CONES", TREE_CONES_DEFAULT) != 0;
	values[3] = (uint32_t)(spatial < 0 ? 0 : spatial > 4 ? 4 : spatial);
	values[4] = env_int("HALO_PT_PARTS_OFF", 0) != 0 || env_int("HALO_PT_PARTS_CYCLE", 0) > 0;
	values[5] = (boil ? (float)atof(boil) : RESTIR_BOIL_DEFAULT) > 0.0f;
	values[6] = (uint32_t)env_int("HALO_PT_BOUNCE_HALF", BOUNCE_HALF_DEFAULT);
	values[6] = values[6] > 2u ? 1u : values[6];
	values[7] = env_int("HALO_PT_SUN_TILES", SUN_TILES_DEFAULT) != 0;
	values[8] = env_int("HALO_PT_BAKE_GRID", BAKE_GRID_DEFAULT) != 0;
}

static int pipeline_make(int which, const uint32_t *words, size_t word_count)
{
	VkComputePipelineCreateInfo info;
	VkSpecializationMapEntry entries[SPEC_COUNT];
	VkSpecializationInfo specialization;
	uint32_t values[SPEC_COUNT];
	VkShaderModule module = shader_make(words, word_count);
	VkResult result;
	uint32_t index;

	if (!module)
	{
		host_logf(HOST_LOG_WARN, "prism pt: %s's shader module cannot be made", pass_names[which]);
		return 0;
	}
	spec_options(values);
	for (index = 0; index < SPEC_COUNT; index++)
	{
		entries[index].constantID = index;
		entries[index].offset = index * (uint32_t)sizeof(uint32_t);
		entries[index].size = sizeof(uint32_t);
	}
	specialization.mapEntryCount = SPEC_COUNT;
	specialization.pMapEntries = entries;
	specialization.dataSize = sizeof(values);
	specialization.pData = values;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.stage.pSpecializationInfo = &specialization;
	info.layout = pt.pipeline_layout;
	result = vkCreateComputePipelines(pt.device.device, xgpu_vulkan_pipeline_cache(), 1, &info, NULL, &pt.pipelines[which]);
	vkDestroyShaderModule(pt.device.device, module, NULL);
	if (result != VK_SUCCESS)
		host_logf(HOST_LOG_WARN, "prism pt: %s's pipeline cannot be made (%d)", pass_names[which], (int)result);
	return result == VK_SUCCESS;
}

static double pt_seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1.0e-9;
}

/* every pass's pipeline (the compile thread's: Vulkan's pipeline creation
needs no lock of ours, the layout made before the thread and the device
not let go of before it is joined, xgpu_vulkan_pt_shutdown) */
static void *pipelines_thread(void *unused)
{
	int ok;

	(void)unused;
	ok = pipeline_make(PASS_LIGHT, xgpu_vulkan_pt_light_words, xgpu_vulkan_pt_light_count) &&
		pipeline_make(PASS_TEMPORAL, xgpu_vulkan_pt_temporal_words, xgpu_vulkan_pt_temporal_count) &&
		pipeline_make(PASS_FIX, xgpu_vulkan_pt_fix_words, xgpu_vulkan_pt_fix_count) &&
		pipeline_make(PASS_ATROUS, xgpu_vulkan_pt_atrous_words, xgpu_vulkan_pt_atrous_count) &&
		pipeline_make(PASS_COMPOSITE, xgpu_vulkan_pt_composite_words, xgpu_vulkan_pt_composite_count) &&
		pipeline_make(PASS_REFERENCE, xgpu_vulkan_pt_reference_words, xgpu_vulkan_pt_reference_count) &&
		pipeline_make(PASS_LAMPS, xgpu_vulkan_pt_lamps_words, xgpu_vulkan_pt_lamps_count) &&
		pipeline_make(PASS_CACHE, xgpu_vulkan_pt_cache_words, xgpu_vulkan_pt_cache_count) &&
		pipeline_make(PASS_WATER, xgpu_vulkan_pt_water_words, xgpu_vulkan_pt_water_count) &&
		pipeline_make(PASS_STABILIZE, xgpu_vulkan_pt_stabilize_words, xgpu_vulkan_pt_stabilize_count) &&
		pipeline_make(PASS_UPSAMPLE, xgpu_vulkan_pt_upsample_words, xgpu_vulkan_pt_upsample_count) &&
		pipeline_make(PASS_REFERENCE_STATS, xgpu_vulkan_pt_reference_stats_words,
		xgpu_vulkan_pt_reference_stats_count);
	pt.compile_ok = ok;
	__atomic_store_n(&pt.compile_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

/* the compile thread, if one runs, waited for */
static void pipelines_wait(void)
{
	if (pt.compile_running)
	{
		pthread_join(pt.compile_thread, NULL);
		pt.compile_running = 0;
	}
}

/* a lost device's (xgpu_vulkan_device_lost_note): where Path Traced drew
one of the last frames, the message says so, and the next start draws
PRISM Rebaked instead (report.c's report_path_traced_fallback reads it) */
static void lost_note(char *text, size_t size)
{
	if (!pt.made || !pt.last_serial || pt.last_serial + 16 < xgpu_vulkan_serial())
		return;
	snprintf(text, size, " in PRISM Path Traced. The next start uses PRISM Rebaked; Settings can choose Path Traced "
		"again.");
}

static int make(void)
{
	VkDescriptorSetLayoutBinding bindings[BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[SET_SLOTS];
	VkSamplerCreateInfo sampler;
	VkQueryPoolCreateInfo queries;
	uint32_t index;

	if (pt.made)
		return 1;
	if (pt.failed)
		return 0;
	/* (the passes being made: CE's frame until they are) */
	if (pt.compile_running)
	{
		if (!__atomic_load_n(&pt.compile_done, __ATOMIC_ACQUIRE))
			return 0;
		pipelines_wait();
		if (!pt.compile_ok)
		{
			pt.failed = 1;
			host_logf(HOST_LOG_WARN, "prism pt: its passes cannot be made: no path tracing");
			return 0;
		}
		host_logf(HOST_LOG_INFO, "prism pt: its passes made in %.1f s", pt_seconds() - pt.compile_started);
		goto passes_made;
	}
	pt.failed = 1;
	if (!xgpu_vulkan_rt_device(&pt.device))
		return 0;
	/* (the temporal pass's ballots and minimum and maximum, the baker's
	shuffles, in compute: else no path tracing, rather than pipelines the
	device cannot run) */
	if ((pt.device.subgroup_compute & PT_SUBGROUP_OPERATIONS) != PT_SUBGROUP_OPERATIONS)
	{
		host_logf(HOST_LOG_WARN, "prism pt: the device's compute shaders lack subgroup operations (0x%x of 0x%x): "
			"no path tracing", pt.device.subgroup_compute, PT_SUBGROUP_OPERATIONS);
		return 0;
	}
#define LOAD(name) name = (PFN_##name)pt.device.get_device_proc(pt.device.device, #name); \
	if (!name) { host_logf(HOST_LOG_WARN, "prism pt: no %s: no path tracing", #name); return 0; }
	PT_FUNCTIONS(LOAD)
#undef LOAD
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < BINDINGS; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = index < 4 || index == IMAGE_LIGHT_IN ?
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = BINDINGS;
	set_info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(pt.device.device, &set_info, NULL, &pt.layout) != VK_SUCCESS)
		return 0;
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.size = sizeof(struct pt_push);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &pt.layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(pt.device.device, &layout_info, NULL, &pt.pipeline_layout) != VK_SUCCESS)
		return 0;
	/* (logged first: a driver that ends the game as it compiles them leaves
	this as halo.log's last Path Traced line. The next start does not yet
	fall back to PRISM Rebaked for that, only for a lost device: report.c) */
	host_logf(HOST_LOG_INFO, "prism pt: making its passes (on a thread of their own: CE's frame until they are made)");
	pt.compile_started = pt_seconds();
	pt.compile_done = 0;
	pt.compile_ok = 0;
	if (pthread_create(&pt.compile_thread, NULL, pipelines_thread, NULL) == 0)
	{
		pt.compile_running = 1;
		pt.failed = 0;
		return 0;
	}
	/* (no thread: made here, the game held meanwhile, as before) */
	pipelines_thread(NULL);
	if (!pt.compile_ok)
	{
		host_logf(HOST_LOG_WARN, "prism pt: its passes cannot be made: no path tracing");
		return 0;
	}
passes_made:
	pt.failed = 1;
	memset(sizes, 0, sizeof(sizes));
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = 5 * SET_SLOTS;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	sizes[1].descriptorCount = (BINDINGS - 5) * SET_SLOTS;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = SET_SLOTS;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(pt.device.device, &pool_info, NULL, &pt.pool) != VK_SUCCESS)
		return 0;
	for (index = 0; index < SET_SLOTS; index++)
		layouts[index] = pt.layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = pt.pool;
	allocate.descriptorSetCount = SET_SLOTS;
	allocate.pSetLayouts = layouts;
	if (vkAllocateDescriptorSets(pt.device.device, &allocate, pt.sets) != VK_SUCCESS)
		return 0;
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	if (vkCreateSampler(pt.device.device, &sampler, NULL, &pt.sampler) != VK_SUCCESS)
		return 0;
	for (index = 0; index < JOB_SLOTS; index++)
		if (!buffer_make(JOB_BYTES, 1, &pt.jobs[index]))
			return 0;
	pt.timestamp_period = pt.device.timestamp_period;
	if (pt.timestamp_period > 0.0)
	{
		memset(&queries, 0, sizeof(queries));
		queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queries.queryCount = TIME_SLOTS * STAMPS;
		if (vkCreateQueryPool(pt.device.device, &queries, NULL, &pt.queries) != VK_SUCCESS)
			pt.queries = VK_NULL_HANDLE;
	}
	pt.made = 1;
	pt.failed = 0;
	xgpu_vulkan_device_lost_note(lost_note);
	host_logf(HOST_LOG_INFO, "prism pt: ready");
	return 1;
}

/* ---------- images */

static struct xgpu_vulkan_image *image_make(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
	uint32_t height)
{
	struct xgpu_vulkan_image_description wanted;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	if (image)
		xgpu_vulkan_image_destroy(image);
	memset(&wanted, 0, sizeof(wanted));
	wanted.type = XGPU_VULKAN_IMAGE_2D;
	wanted.format = format;
	wanted.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_STORAGE;
	wanted.samples = 1;
	wanted.width = width;
	wanted.height = height;
	wanted.depth_or_layers = 1;
	wanted.levels = 1;
	return xgpu_vulkan_image_create(&wanted);
}

/* every image at the window's size, the light's and the denoiser's at the
light's grid's (low_width x low_height: the window's, or smaller with the
traced light's upscaling, which then has its images at the window's size):
1 if they are (the histories begun again where they were made) */
static int images_make(uint32_t width, uint32_t height, uint32_t low_width, uint32_t low_height)
{
	struct xgpu_vulkan_image **half[] =
	{
		&pt.noisy_diffuse, &pt.noisy_specular, &pt.clean_diffuse, &pt.clean_specular, &pt.sun_raw, &pt.diff[0],
		&pt.diff[1], &pt.spec[0], &pt.spec[1], &pt.moments[0], &pt.moments[1], &pt.spec_moments[0],
		&pt.spec_moments[1], &pt.sun[0], &pt.sun[1], &pt.diff_a, &pt.diff_b, &pt.spec_a, &pt.spec_b, &pt.sun_a,
		&pt.sun_b, &pt.stab_diff[0], &pt.stab_diff[1], &pt.stab_spec[0], &pt.stab_spec[1],
	};
	struct xgpu_vulkan_image **up[] = { &pt.up_diff, &pt.up_spec, &pt.up_clean_diffuse, &pt.up_clean_specular };
	int low = low_width != width || low_height != height;
	uint32_t index, up_width = low ? width : 1u, up_height = low ? height : 1u;

	if (width == pt.width && height == pt.height && low_width == pt.low_width && low_height == pt.low_height &&
		pt.out)
		return 1;
	if (!(pt.out = image_make(pt.out, XGPU_VULKAN_FORMAT_RGBA16F, width, height)))
		return 0;
	for (index = 0; index < sizeof(up) / sizeof(up[0]); index++)
		if (!(*up[index] = image_make(*up[index], XGPU_VULKAN_FORMAT_RGBA16F, up_width, up_height)))
			return 0;
	width = low_width;
	height = low_height;
	for (index = 0; index < sizeof(half) / sizeof(half[0]); index++)
		if (!(*half[index] = image_make(*half[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height)))
			return 0;
	for (index = 0; index < 2; index++)
		if (!(pt.guide[index] = image_make(pt.guide[index], XGPU_VULKAN_FORMAT_RGBA32_UINT, width, height)) ||
			!(pt.reservoir_info[index] = image_make(pt.reservoir_info[index], XGPU_VULKAN_FORMAT_RGBA32_UINT, width,
			height)) ||
			!(pt.reservoir_point[index] = image_make(pt.reservoir_point[index], XGPU_VULKAN_FORMAT_RGBA32F, width,
			height)) ||
			!(pt.gi_point[index] = image_make(pt.gi_point[index], XGPU_VULKAN_FORMAT_RGBA32F, width, height)) ||
			!(pt.gi_data[index] = image_make(pt.gi_data[index], XGPU_VULKAN_FORMAT_RGBA32F, width, height)))
			return 0;
	if (!(pt.exposure = image_make(pt.exposure, XGPU_VULKAN_FORMAT_RGBA16F, 1, 1)))
		return 0;
	pt.width = xgpu_vulkan_image_description(pt.out)->width;
	pt.height = xgpu_vulkan_image_description(pt.out)->height;
	pt.low_width = low_width;
	pt.low_height = low_height;
	pt.history_valid = 0;
	return 1;
}

/* the images of the window's and the light grid's sizes let go of (their
memory freed as the GPU is done with them), so that they are made again
whole at the next images_make */
static void images_free(void)
{
	struct xgpu_vulkan_image **images[] =
	{
		&pt.noisy_diffuse, &pt.noisy_specular, &pt.clean_diffuse, &pt.clean_specular, &pt.sun_raw, &pt.diff[0],
		&pt.diff[1], &pt.spec[0], &pt.spec[1], &pt.moments[0], &pt.moments[1], &pt.spec_moments[0],
		&pt.spec_moments[1], &pt.sun[0], &pt.sun[1], &pt.diff_a, &pt.diff_b, &pt.spec_a, &pt.spec_b, &pt.sun_a,
		&pt.sun_b, &pt.stab_diff[0], &pt.stab_diff[1], &pt.stab_spec[0], &pt.stab_spec[1], &pt.up_diff, &pt.up_spec,
		&pt.up_clean_diffuse, &pt.up_clean_specular, &pt.guide[0], &pt.guide[1], &pt.reservoir_info[0],
		&pt.reservoir_info[1], &pt.reservoir_point[0], &pt.reservoir_point[1], &pt.gi_point[0], &pt.gi_point[1],
		&pt.gi_data[0], &pt.gi_data[1], &pt.out,
	};
	uint32_t index;

	for (index = 0; index < sizeof(images) / sizeof(images[0]); index++)
	{
		if (*images[index])
			xgpu_vulkan_image_destroy(*images[index]);
		*images[index] = NULL;
	}
	pt.width = pt.height = pt.low_width = pt.low_height = 0;
	pt.history_valid = 0;
}

/* the bytes images_make's images of a window's size and a light grid's
take (the output, the grid's 25 RGBA16F and 10 RGBA32 images, with a coarser
grid 4 RGBA16F of the window's size) */
static uint64_t images_bytes(uint32_t width, uint32_t height, uint32_t low_width, uint32_t low_height)
{
	uint64_t window = (uint64_t)width * height, grid = (uint64_t)low_width * low_height;
	int low = low_width != width || low_height != height;

	return window * 8u + (low ? window * 32u : 0u) + grid * (25u * 8u + 10u * 16u);
}

/* (what the card is to keep free beside the images: the frame's own, the
drivers', the texture store's growth) */
#define IMAGES_MARGIN (256ull << 20)

/* the memory Path Traced never reads put aside for its images (the bake's
light to the host's memory, xgpu_vulkan_bake_park): 1 where some was, freed
after this submission */
static int images_room_made(void)
{
	if (!xgpu_vulkan_bake_park())
		return 0;
	pt.memory_freed++;
	return 1;
}

/* images_make of the window's size and a light grid's, for pt_fit_choose:
first, where the card's budget has no room for them, the memory Path Traced
never reads put aside (PT_FIT_FREEING: tried again once it is freed); what
a failed try made let go of, and that memory put aside before a coarser
grid is tried */
static int images_made(void *context, uint32_t low_width, uint32_t low_height)
{
	const uint32_t *size = (const uint32_t *)context;
	uint64_t budget, usage, need = images_bytes(size[0], size[1], low_width, low_height);
	int last, made;

	if (!pt.out && xgpu_vulkan_memory_budget(&budget, &usage) && usage && usage + need + IMAGES_MARGIN > budget)
	{
		if (images_room_made())
		{
			xgpu_vulkan_memory_log("the light's images need room");
			return PT_FIT_FREEING;
		}
		/* (no room in the budget at all, and nothing to put aside: refused
		without a try, which the driver would refuse) */
		if (usage + need > budget)
			return PT_FIT_REFUSED;
	}
	last = xgpu_vulkan_memory_use(XGPU_VULKAN_MEMORY_PT_IMAGES);
	made = images_make(size[0], size[1], low_width, low_height);
	xgpu_vulkan_memory_use(last);
	if (made)
		return PT_FIT_MADE;
	xgpu_vulkan_memory_log("the light's images refused");
	images_free();
	return images_room_made() ? PT_FIT_FREEING : PT_FIT_REFUSED;
}

/* the light grid's scale whose images the card has room for (the one
asked, else a coarser one: xgpu_vulkan_pt_fit.h), -1 where none this frame
(CE's frame); each grid a shortage chose, and none fitting, said once */
static float images_fit(uint32_t width, uint32_t height, float asked, int low_allowed)
{
	uint32_t size[2] = { width, height };
	int changed;
	float scale;

	/* (a coarser grid, or none, chosen before the bake's light could be put
	aside (still being loaded then): the grid asked again once it is) */
	if ((pt.memory.step || pt.memory.none) && pt.memory.width == width && pt.memory.height == height &&
		!pt.memory.wait && images_room_made())
	{
		pt_fit_retry(&pt.memory);
		host_logf(HOST_LOG_INFO, "prism pt: memory put aside: the light's grid asked tried again");
	}
	/* (the images made, but the card all but full beside them: a bake
loaded since, the light's buffers on the card as far as they went, the rest
in the host's memory; put aside too, every 30 frames at most) */
	if (pt.out && ++pt.memory_checked >= 30)
	{
		uint64_t budget, usage;

		pt.memory_checked = 0;
		if (xgpu_vulkan_memory_budget(&budget, &usage) && usage && usage + IMAGES_MARGIN * 2 > budget &&
			images_room_made())
			xgpu_vulkan_memory_log("the card all but full beside Path Traced");
	}
	scale = pt_fit_choose(&pt.memory, width, height, asked, low_allowed, images_made, size, &changed);

	if (changed && scale > 0.0f)
	{
		uint32_t low_width, low_height;

		pt_fit_low_size(width, height, scale, &low_width, &low_height);
		host_logf(HOST_LOG_WARN, "prism pt: warning: no memory on the card for the light's images at %ux%u: traced at "
			"%ux%u (a light grid of %.2f, brought up guided by depth and normals)", width, height, low_width, low_height,
			(double)scale);
	}
	else if (changed)
		host_logf(HOST_LOG_WARN, "prism pt: warning: no memory on the card for the light's images at %ux%u at any "
			"light grid: CE's frame until there is (a lower resolution or an upscaler needs less)", width, height);
	/* (the game's indicator says so: XGPU_BAKE_STATUS_PT_MEMORY) */
	xgpu_vulkan_bake_pt_memory(pt.memory.none ? 1u : pt.memory.step ?
		(uint32_t)(pt_fit_scales[pt.memory.step - 1] * 100.0f + 0.5f) : 0u);
	return scale;
}

/* ---------- the timing */

static void times_collect(void)
{
	uint32_t slot;

	if (!pt.queries)
		return;
	for (slot = 0; slot < TIME_SLOTS; slot++)
	{
		uint64_t stamps[STAMPS];
		uint32_t index;

		if (!pt.time_serial[slot] || xgpu_vulkan_completed() < pt.time_serial[slot])
			continue;
		pt.time_serial[slot] = 0;
		if (vkGetQueryPoolResults(pt.device.device, pt.queries, slot * STAMPS, STAMPS, sizeof(stamps), stamps,
			sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			continue;
		if (!pt.times_file_tried)
		{
			const char *path = getenv("HALO_PT_TIMES_FILE");

			pt.times_file_tried = 1;
			pt.times_file = path && *path ? fopen(path, "w") : NULL;
		}
		{
			double ms[TIMED + 1];

			for (index = 0; index < TIMED; index++)
			{
				ms[index] = (double)(stamps[index + 1] - stamps[index]) * pt.timestamp_period * 1.0e-6;
				pt.time_sums[index] += ms[index];
			}
			ms[TIMED] = (double)(stamps[TIMED] - stamps[0]) * pt.timestamp_period * 1.0e-6;
			pt.time_sums[TIMED] += ms[TIMED];
			pt.time_frames++;
			if (pt.times_file)
			{
				fprintf(pt.times_file, "%.4f %.4f %.4f %.4f %.4f %.4f\n", ms[TIMED], ms[0], ms[1], ms[2], ms[3], ms[4]);
				fflush(pt.times_file);
			}
		}
		if (pt.time_frames >= 600)
		{
			double n = (double)pt.time_frames;

			host_logf(HOST_LOG_INFO, "prism pt: %u frames: %.3f ms a frame (light %.3f, temporal %.3f, fix %.3f, "
				"atrous %.3f, composite %.3f)", pt.time_frames, pt.time_sums[TIMED] / n, pt.time_sums[0] / n,
				pt.time_sums[1] / n, pt.time_sums[2] / n, pt.time_sums[3] / n, pt.time_sums[4] / n);
			memset(pt.time_sums, 0, sizeof(pt.time_sums));
			pt.time_frames = 0;
			xgpu_vulkan_memory_log("path traced");
		}
	}
}

/* ---------- the reference */

/* the sums' copy written once the GPU has made it */
static void reference_write(void)
{
	/* (noise.direct, .indirect and .specular: the standard error of each
	part's luminance's mean (the direct light, the diffuse bounce's, the
	reflection's) over the whole's mean, where the noise is) */
	static const char *const names[] =
	{
		"light.R", "light.G", "light.B", "spec.R", "spec.G", "spec.B", "Z", "samples", "noise", "noise.direct",
		"noise.indirect", "noise.specular",
	};
	struct xgpu_exr_attribute attributes[3];
	const float *sums = (const float *)pt.sums_readback.mapped;
	uint64_t texel, count = (uint64_t)pt.ref_width * pt.ref_height;
	float *pixels;
	char error[256];
	uint32_t n = pt.ref_count;

	if (!pt.ref_sums_serial || xgpu_vulkan_completed() < pt.ref_sums_serial)
		return;
	pt.ref_sums_serial = 0;
	if (!sums || !pt.ref_path[0] || !(pixels = (float *)malloc((size_t)count * 12 * sizeof(float))))
	{
		host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: the sum cannot be written%s",
			pt.ref_path[0] ? "" : " (no file named)");
		return;
	}
	for (texel = 0; texel < count; texel++)
	{
		const float *s = sums + texel * SUMS;
		float *to = pixels + texel * 12;
		int channel;

		for (channel = 0; channel < 6; channel++)
			to[channel] = s[channel];
		to[6] = s[8];
		to[7] = s[8] > 0.0f ? (float)n : 0.0f;
		/* (the luminance's mean's relative standard error, and its parts') */
		for (channel = 0; channel < 4; channel++)
		{
			float m2 = s[channel ? 8 + 2 * channel : 7];

			to[8 + channel] = n > 1 && s[6] > 1.0e-6f ? sqrtf(fmaxf(m2, 0.0f) / ((float)n * (float)(n - 1))) / s[6] :
				0.0f;
		}
	}
	attributes[0].name = "prismReferenceFrames";
	attributes[0].value = (int32_t)n;
	attributes[1].name = "prismReferenceNoisePpm";
	attributes[1].value = (int32_t)(pt.ref_noise * 1.0e6 + 0.5);
	attributes[2].name = "prismReferenceGeneration";
	attributes[2].value = (int32_t)pt.ref_generation;
	if (xgpu_exr_write(pt.ref_path, pt.ref_width, pt.ref_height, 12, names, pixels, attributes, 3, error, sizeof(error)))
		host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: %u frames of %u paths summed (estimated relative "
			"MSE %.6f as the gates' frames measure it, %.6f linear), written to %s", n, pt.ref_paths, pt.ref_noise,
			pt.ref_noise_hdr, pt.ref_path);
	else
		host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: %s", error);
	free(pixels);
}

/* the share of a pixel's independent noise's variance (relative to the
mean squared) the gates' frames keep: theirs are this frame brought to
GATE_WIDTH by bilinear filtering (PIL's, a triangle as wide as the scale on
each side), each of their pixels the filter's weights' sum over the frame's
pixels, so a pixel's variance reaches it by the squared weights: the square
of their sum along an axis (half size: weights 1 3 3 1 over 8, 0.3125 an
axis, 0.098 a pixel) */
static double gate_share(uint32_t width)
{
	double scale = (double)width / GATE_WIDTH, sum = 0.0, squares = 0.0;
	int k, reach;

	if (scale <= 1.0)
		return 1.0;
	reach = (int)ceil(scale) + 1;
	for (k = -reach; k < reach; k++)
	{
		double w = 1.0 - fabs((double)k + 0.5) / scale;

		if (w > 0.0)
		{
			sum += w;
			squares += w * w;
		}
	}
	return sum > 0.0 ? (squares / (sum * sum)) * (squares / (sum * sum)) : 1.0;
}

/* its noise measured (the partial sums the GPU made): the estimated
relative MSE of the frame as the gates' frames measure it (the stopping
rule's), and linear (the whole light, unclamped: logged), and whether the
first is under the bound */
static void reference_noise_collect(void)
{
	const float *partial = (const float *)pt.stats_readback.mapped;
	double variance = 0.0, square = 0.0, shown_variance = 0.0, shown_square = 0.0;
	uint32_t index;

	if (!pt.ref_stats_serial || xgpu_vulkan_completed() < pt.ref_stats_serial || !partial)
		return;
	pt.ref_stats_serial = 0;
	for (index = 0; index < pt.ref_stats_groups; index++)
	{
		variance += partial[index * 4];
		square += partial[index * 4 + 1];
		shown_variance += partial[index * 4 + 2];
		shown_square += partial[index * 4 + 3];
	}
	pt.ref_noise_hdr = square > 0.0 ? variance / square : 1.0;
	pt.ref_noise = shown_square > 0.0 ? gate_share(pt.ref_width) * shown_variance / shown_square : 1.0;
	if ((pt.ref_stats_frames % 512u) == 0u || pt.ref_noise <= REFERENCE_NOISE)
		host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: %u frames, estimated relative MSE %.6f as the gates' "
			"frames measure it (%.6f linear)", pt.ref_stats_frames, pt.ref_noise, pt.ref_noise_hdr);
	if (pt.ref_noise <= REFERENCE_NOISE && pt.ref_stats_frames >= REFERENCE_LEAST && !pt.ref_stopped)
	{
		pt.ref_stopped = 1;
		pt.ref_converged = 1;
	}
}

/* this frame's request (RT_FRAME's, prism_rt.c's reference_frame): 1 if the
frame is the reference's, its sums made at the size and its count the
frame's number in the sum (held once it is stopped) */
/* whether this frame asks for debug.pt_reference (RT_FRAME's request), read
before the frame's images are sized: the reference traces and sums every
pixel of the G-buffer, so the light grid is the G-buffer's own while it runs */
static int reference_requested(void)
{
	uint32_t generation_unused, materials_count, alpha_words;
	const float *materials;
	const uint32_t *alpha;
	const struct xgpu_stream_rt_frame *frame = NULL;

	(void)xgpu_vulkan_rt_scene(&generation_unused, &materials, &materials_count, &alpha, &alpha_words, &frame);
	return frame && (frame->flags & XGPU_RT_FLAG_REFERENCE) && frame->spare[0] >= 1.0f;
}

static int reference_take(const struct xgpu_stream_scene_view *view, uint64_t scene, uint32_t width, uint32_t height)
{
	uint32_t generation_unused, materials_count;
	const float *materials;
	const uint32_t *alpha;
	uint32_t alpha_words, frames, generation;
	const struct xgpu_stream_rt_frame *frame = NULL;
	int restart = 0, row, column;

	(void)xgpu_vulkan_rt_scene(&generation_unused, &materials, &materials_count, &alpha, &alpha_words, &frame);
	if (!frame || !(frame->flags & XGPU_RT_FLAG_REFERENCE) || !(frame->spare[0] >= 1.0f))
	{
		pt.ref_seen = 0;
		return 0;
	}
	frames = frame->spare[0] > 1048576.0f ? 1048576u : (uint32_t)frame->spare[0];
	generation = frame->spare[1] >= 0.0f && frame->spare[1] < 16777216.0f ? (uint32_t)frame->spare[1] : 0u;
	if (!pt.ref_seen || generation != pt.ref_generation || frames != pt.ref_frames || scene != pt.ref_scene)
		restart = 1;
	if (width != pt.ref_width || height != pt.ref_height || !pt.sums.buffer)
	{
		uint32_t groups = (width * height + 255u) / 256u;

		buffer_free(&pt.sums);
		buffer_free(&pt.stats);
		buffer_free(&pt.stats_readback);
		buffer_free(&pt.sums_readback);
		if (!buffer_make((VkDeviceSize)width * height * SUMS * 4, 0, &pt.sums) ||
			!buffer_make((VkDeviceSize)groups * 16, 0, &pt.stats) ||
			!buffer_make((VkDeviceSize)groups * 16, 1, &pt.stats_readback) ||
			!buffer_make((VkDeviceSize)width * height * SUMS * 4, 1, &pt.sums_readback))
		{
			host_logf(HOST_LOG_WARN, "prism pt: debug.pt_reference: no memory for its sums");
			pt.ref_seen = 0;
			return 0;
		}
		pt.ref_width = width;
		pt.ref_height = height;
		pt.ref_stats_groups = groups;
		restart = 1;
	}
	if (pt.ref_seen && !restart)
		for (row = 0; row < 4 && !restart; row++)
			for (column = 0; column < 4 && !restart; column++)
				if (fabsf(pt.ref_camera[row][column] - view->now[row][column]) >
					1.0e-4f * fmaxf(1.0f, fabsf(pt.ref_camera[row][column])))
				{
					host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: the camera moved after %u frames: summed "
						"again", pt.ref_count);
					restart = 1;
				}
	if (restart)
	{
		const char *path = frame->reference_file ? host_pointer(frame->reference_file) : NULL;
		const char *paths = getenv("HALO_PT_REFERENCE_PATHS");

		pt.ref_generation = generation;
		pt.ref_frames = frames;
		pt.ref_scene = scene;
		pt.ref_count = 0;
		pt.ref_stopped = pt.ref_converged = pt.ref_copied = 0;
		pt.ref_noise = pt.ref_noise_hdr = 1.0;
		pt.ref_stats_serial = pt.ref_sums_serial = 0;
		pt.ref_paths = paths ? (uint32_t)atoi(paths) : 1u;
		if (pt.ref_paths < 1u || pt.ref_paths > 64u)
			pt.ref_paths = 1u;
		memcpy(pt.ref_camera, view->now, sizeof(pt.ref_camera));
		pt.ref_path[0] = 0;
		if (path && memchr(path, 0, XGPU_RT_REFERENCE_PATH))
			snprintf(pt.ref_path, sizeof(pt.ref_path), "%s", path);
	}
	pt.ref_seen = 1;
	return 1;
}

/* ---------- the reference's light tree

A binary tree of the bake's lamps (xgpu_vulkan_pt.glsl's Tree: the
reference picks a lamp from the grid's mixture or from it, half and half),
built here from a copy of the bake's lamps (the pass "lamps") once a scene's
generation: each node its lamps' box, power (luminance by area), children
and parent, a leaf its lamp and its triangle's plane; the lamps split at the
middle of their middles' widest axis, half and half. */

static const float *tree_lamps;
static int tree_axis;

static float lamp_middle(const float *lamp, int axis)
{
	return (lamp[axis] + lamp[3 + axis] + lamp[6 + axis]) / 3.0f;
}

static int tree_compare(const void *a, const void *b)
{
	uint32_t ia = *(const uint32_t *)a, ib = *(const uint32_t *)b;
	float ma = lamp_middle(tree_lamps + (size_t)ia * 16, tree_axis);
	float mb = lamp_middle(tree_lamps + (size_t)ib * 16, tree_axis);

	if (ma != mb)
		return ma < mb ? -1 : 1;
	return ia < ib ? -1 : ia > ib ? 1 : 0;
}

static void float_bits(float *to, uint32_t value)
{
	memcpy(to, &value, sizeof(value));
}

/* the node over order[first .. first + count) (lamps' indices), its parent's
index (the root's its own, unused); its index */
/* a node's cone of its emitters' normals (Conty Estevez and Kulla 2018's
orientation bounds): the smallest cone holding its children's two, its axis
at 11 to 13 and its half angle's cosine at 15 (a leaf's: its normal, 1) */
static void tree_cone(float *node, const float *a, const float *b)
{
	float theta_a = acosf(fminf(fmaxf(a[15], -1.0f), 1.0f)), theta_b = acosf(fminf(fmaxf(b[15], -1.0f), 1.0f));
	float axis[3], d, theta_d, theta_o, theta_r, length, ortho[3], dot_ab;
	int k;

	if (theta_a < theta_b)
	{
		const float *t = a;
		float tt = theta_a;

		a = b;
		b = t;
		theta_a = theta_b;
		theta_b = tt;
	}
	dot_ab = a[11] * b[11] + a[12] * b[12] + a[13] * b[13];
	theta_d = acosf(fminf(fmaxf(dot_ab, -1.0f), 1.0f));
	if (fminf(theta_d + theta_b, 3.14159265f) <= theta_a)
	{
		memcpy(node + 11, a + 11, 3 * sizeof(float));
		node[15] = a[15];
		return;
	}
	theta_o = 0.5f * (theta_a + theta_d + theta_b);
	if (theta_o >= 3.14159265f || theta_d < 1.0e-6f)
	{
		memcpy(node + 11, a + 11, 3 * sizeof(float));
		node[15] = theta_o >= 3.14159265f ? -1.0f : cosf(theta_o);
		return;
	}
	/* (a's axis turned toward b's by theta_o less theta_a) */
	theta_r = theta_o - theta_a;
	for (k = 0; k < 3; k++)
		ortho[k] = b[11 + k] - dot_ab * a[11 + k];
	length = sqrtf(ortho[0] * ortho[0] + ortho[1] * ortho[1] + ortho[2] * ortho[2]);
	for (k = 0; k < 3; k++)
		axis[k] = a[11 + k] * cosf(theta_r) + (length > 0.0f ? ortho[k] / length : 0.0f) * sinf(theta_r);
	d = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	for (k = 0; k < 3; k++)
		node[11 + k] = d > 0.0f ? axis[k] / d : a[11 + k];
	node[15] = cosf(theta_o);
}

static uint32_t tree_node(float *nodes, uint32_t *node_count, uint32_t *order, uint32_t first, uint32_t count,
	uint32_t parent)
{
	uint32_t index = (*node_count)++, k, left, right;
	float *node = nodes + (size_t)index * TREE_NODE_FLOATS;
	float lo[3], hi[3], middle_lo[3], middle_hi[3], power = 0.0f, widest = -1.0f;
	int axis, corner, split = 0;

	for (axis = 0; axis < 3; axis++)
	{
		lo[axis] = middle_lo[axis] = 1.0e30f;
		hi[axis] = middle_hi[axis] = -1.0e30f;
	}
	for (k = 0; k < count; k++)
	{
		const float *lamp = tree_lamps + (size_t)order[first + k] * 16;

		for (axis = 0; axis < 3; axis++)
		{
			float m = lamp_middle(lamp, axis);

			for (corner = 0; corner < 3; corner++)
			{
				lo[axis] = fminf(lo[axis], lamp[corner * 3 + axis]);
				hi[axis] = fmaxf(hi[axis], lamp[corner * 3 + axis]);
			}
			middle_lo[axis] = fminf(middle_lo[axis], m);
			middle_hi[axis] = fmaxf(middle_hi[axis], m);
		}
		power += (0.2126f * lamp[9] + 0.7152f * lamp[10] + 0.0722f * lamp[11]) * lamp[12];
	}
	memset(node, 0, TREE_NODE_FLOATS * sizeof(float));
	memcpy(node, lo, sizeof(lo));
	memcpy(node + 3, hi, sizeof(hi));
	node[6] = power;
	float_bits(node + 9, parent);
	if (count == 1)
	{
		const float *lamp = tree_lamps + (size_t)order[first] * 16;
		float e1[3], e2[3], n[3], length;

		for (axis = 0; axis < 3; axis++)
		{
			e1[axis] = lamp[3 + axis] - lamp[axis];
			e2[axis] = lamp[6 + axis] - lamp[axis];
		}
		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length > 0.0f)
		{
			n[0] /= length;
			n[1] /= length;
			n[2] /= length;
		}
		float_bits(node + 7, 0u);
		float_bits(node + 8, 0u);
		float_bits(node + 10, order[first]);
		memcpy(node + 11, n, sizeof(n));
		node[14] = n[0] * lamp[0] + n[1] * lamp[1] + n[2] * lamp[2];
		/* (its emitters' cone: the triangle's normal, no spread) */
		node[15] = 1.0f;
		return index;
	}
	for (axis = 0; axis < 3; axis++)
		if (middle_hi[axis] - middle_lo[axis] > widest)
		{
			widest = middle_hi[axis] - middle_lo[axis];
			split = axis;
		}
	tree_axis = split;
	qsort(order + first, count, sizeof(uint32_t), tree_compare);
	left = tree_node(nodes, node_count, order, first, count / 2, index);
	right = tree_node(nodes, node_count, order, first + count / 2, count - count / 2, index);
	node = nodes + (size_t)index * TREE_NODE_FLOATS;
	float_bits(node + 7, left);
	float_bits(node + 8, right);
	tree_cone(node, nodes + (size_t)left * TREE_NODE_FLOATS, nodes + (size_t)right * TREE_NODE_FLOATS);
	return index;
}

/* the lamps' copy read once the GPU made it: the tree built and given to
the GPU (a buffer the host writes: its nodes, then each lamp's leaf) */
static void tree_collect(void)
{
	const float *copy = (const float *)pt.lamps_readback.mapped;
	uint32_t count, node_count = 0, *order, k;
	float *nodes;

	if (!pt.lamps_serial || xgpu_vulkan_completed() < pt.lamps_serial || !copy)
		return;
	pt.lamps_serial = 0;
	memcpy(&count, copy, sizeof(count));
	/* (no lamps, or more than the copy holds: no tree, the grid alone) */
	if (count == 0 || count > LAMPS_MOST)
	{
		if (count > LAMPS_MOST)
			host_logf(HOST_LOG_INFO, "prism pt: %u lamps, more than the light tree takes: the grid alone", count);
		pt.tree_generation = pt.lamps_generation;
		pt.tree_nodes = 0;
		return;
	}
	nodes = (float *)malloc(((size_t)count * 2 - 1) * TREE_NODE_FLOATS * sizeof(float) + (size_t)count * 4);
	order = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
	if (!nodes || !order)
	{
		free(nodes);
		free(order);
		pt.tree_failed = 1;
		return;
	}
	for (k = 0; k < count; k++)
		order[k] = k;
	tree_lamps = copy + 16;
	tree_node(nodes, &node_count, order, 0, count, 0);
	/* (each lamp's leaf, after the nodes) */
	for (k = 0; k < node_count; k++)
	{
		uint32_t left, lamp;

		memcpy(&left, nodes + (size_t)k * TREE_NODE_FLOATS + 7, 4);
		memcpy(&lamp, nodes + (size_t)k * TREE_NODE_FLOATS + 10, 4);
		if (left == 0 && lamp < count)
			float_bits(nodes + (size_t)node_count * TREE_NODE_FLOATS + lamp, k);
	}
	buffer_free(&pt.tree);
	if (!buffer_make((VkDeviceSize)node_count * TREE_NODE_FLOATS * 4 + (VkDeviceSize)count * 4, 1, &pt.tree))
	{
		free(nodes);
		free(order);
		pt.tree_failed = 1;
		host_logf(HOST_LOG_WARN, "prism pt: no memory for the reference's light tree");
		return;
	}
	memcpy(pt.tree.mapped, nodes, (size_t)node_count * TREE_NODE_FLOATS * 4 + (size_t)count * 4);
	pt.tree_nodes = node_count;
	pt.tree_generation = pt.lamps_generation;
	host_logf(HOST_LOG_INFO, "prism pt: the reference's light tree: %u lamps, %u nodes", count, node_count);
	free(nodes);
	free(order);
}

/* ---------- the frame */

/* the exposure's settings from the last RT_FRAME, as Raster's frame takes
them (display.prism_exposure: 0 fixed, 1 ce, 2 auto; display.prism_tone_curve
0 ce, 1 filmic, 2 aces, 3 local; prism_rt.c's style default for the v4
modes is "ce" and the clipped curve) */
static void exposure_settings(float exposure[4])
{
	uint32_t generation, material_count, alpha_words, mode = 1, curve = 0;
	const float *materials;
	const uint32_t *alpha;
	const struct xgpu_stream_rt_frame *frame = NULL;

	(void)xgpu_vulkan_rt_scene(&generation, &materials, &material_count, &alpha, &alpha_words, &frame);
	if (frame)
	{
		mode = (uint32_t)(frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_EXPOSURE] + 0.5f);
		mode = mode > 2 ? 1 : mode;
		curve = (uint32_t)(frame->rig[XGPU_RT_RIG_SETTINGS + XGPU_RT_SETTING_TONE_CURVE] + 0.5f);
		curve = curve > 3 ? 0 : curve;
	}
	exposure[0] = (float)mode;
	exposure[1] = (float)curve;
}

/* a job slot's last frame, done: its line in the diagnostics' file
(HALO_PT_DEBUG_FILE): the frame's number, whether it took the last frame's
history, the view's flags, how far the eye moved, and the counters of the
temporal pass (the level pixels in 16 that looked for their history, those
whose place there was on their own surface, in the view, their history
taken) */
static void debug_line(uint32_t slot)
{
	const uint32_t *counters;

	if (!pt.debug_file_tried)
	{
		const char *path = getenv("HALO_PT_DEBUG_FILE");

		pt.debug_file_tried = 1;
		pt.debug_file = path && path[0] ? fopen(path, "w") : NULL;
	}
	if (!pt.debug_file || !pt.job_frame[slot] || !pt.jobs[slot].mapped)
		return;
	counters = (const uint32_t *)((const char *)pt.jobs[slot].mapped + JOB_COUNTERS_OFFSET);
	fprintf(pt.debug_file, "%u history %u view %u moved %.5f looked %u own %u in %u taken %u\n", pt.job_frame[slot],
		(pt.job_flags[slot] & FLAG_HISTORY) ? 1u : 0u, pt.job_view_flags[slot], pt.job_moved[slot], counters[0],
		counters[1], counters[2], counters[3]);
	fflush(pt.debug_file);
	pt.job_frame[slot] = 0;
}

static void write_image(VkWriteDescriptorSet *write, VkDescriptorImageInfo *info, VkDescriptorSet set, uint32_t binding,
	struct xgpu_vulkan_image *image, int sampled)
{
	memset(info, 0, sizeof(*info));
	info->sampler = sampled ? pt.sampler : VK_NULL_HANDLE;
	info->imageView = xgpu_vulkan_image_view(image);
	info->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(write, 0, sizeof(*write));
	write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write->dstSet = set;
	write->dstBinding = binding;
	write->descriptorCount = 1;
	write->descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write->pImageInfo = info;
}

/* the a-trous steps a frame (HALO_PT_ATROUS_STEPS, 1 to 5; 4 by default) */
static int atrous_steps(void)
{
	static int steps = -1;

	if (steps < 0)
	{
		steps = env_int("HALO_PT_ATROUS_STEPS", ATROUS_STEPS_DEFAULT);
		steps = steps < 1 ? 1 : steps > 5 ? 5 : steps;
	}
	return steps;
}

/* the temporal pass's hold of the specular history (HALO_PT_SPEC_HOLD) */
static uint32_t spec_hold(void)
{
	static int hold = -1;

	if (hold < 0)
		hold = env_int("HALO_PT_SPEC_HOLD", SPEC_HOLD_DEFAULT);
	return (uint32_t)hold;
}

/* the histories' anti-firefly in the fix pass (xgpu_vulkan_pt.glsl's
history_firefly): the bound in quarters of the neighbours' standard
deviation (HALO_PT_HIST_FIREFLY, in standard deviations; 0 none) */
static uint32_t hist_firefly(void)
{
	static int quarters = -1;

	if (quarters < 0)
	{
		const char *v = getenv("HALO_PT_HIST_FIREFLY");
		float k = v ? (float)atof(v) : HIST_FIREFLY_DEFAULT;

		quarters = k <= 0.0f ? 0 : k > 63.0f ? 252 : (int)(k * 4.0f + 0.5f);
	}
	return (uint32_t)quarters;
}

/* the temporal stabilisation after the filter: its history's frames at most
(HALO_PT_STABILIZE; 0 none) */
static int stabilize_on(void)
{
	static int most = -1;

	if (most < 0)
	{
		most = env_int("HALO_PT_STABILIZE", STABILIZE_DEFAULT);
		most = most < 0 ? 0 : most > 64 ? 64 : most;
	}
	return most;
}

/* whether four a-trous steps skip stride 8 for 16 (HALO_PT_ATROUS_WIDE) */
static int atrous_wide(void)
{
	static int wide = -1;

	if (wide < 0)
		wide = env_int("HALO_PT_ATROUS_WIDE", ATROUS_WIDE_DEFAULT) != 0;
	return wide;
}

static void dispatch(VkCommandBuffer commands, int pass, VkDeviceAddress scene, VkDeviceAddress job, uint32_t p0,
	uint32_t p1, uint32_t p2, uint32_t p3, uint32_t groups_x, uint32_t groups_y)
{
	struct pt_push push;

	memset(&push, 0, sizeof(push));
	push.scene = scene;
	push.job = job;
	push.parameters[0] = p0;
	push.parameters[1] = p1;
	push.parameters[2] = p2;
	push.parameters[3] = p3;
	xgpu_vulkan_breadcrumb(commands, (uint32_t)pass + 1u, pass_names[pass]);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipelines[pass]);
	vkCmdPushConstants(commands, pt.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(commands, groups_x, groups_y, 1);
}

static void stamp(VkCommandBuffer commands, uint32_t slot, uint32_t index)
{
	if (pt.queries && slot < TIME_SLOTS)
		vkCmdWriteTimestamp(commands, index ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			pt.queries, slot * STAMPS + index);
}

/* the recent dynamic lights into the job (after this frame's, at
LIGHTS_MOST), then this frame's taken into them: each recent one's colour
less 1 / history of itself, this frame's colour where more, its place and
cone this frame's; one gone below 1/500 of white left out, and all where the history is not kept. The
count of the recent ones given to the job (those of the frames before alone: one this frame added is not
in the job's copy) */
static uint32_t recent_lights_take(float *lights, const uint32_t *ids, uint32_t count, int history, float frames)
{
	float keep = frames > 1.0f ? 1.0f - 1.0f / frames : 0.0f;
	uint32_t index, at, kept = 0, given;

	if (!history)
		pt.recent_count = 0;
	given = pt.recent_count;
	memcpy(lights + LIGHTS_MOST * LIGHT_FLOATS, pt.recent_lights, pt.recent_count * LIGHT_FLOATS * sizeof(float));
	for (index = 0; index < pt.recent_count; index++)
	{
		float *r = pt.recent_lights + index * LIGHT_FLOATS;

		r[4] *= keep;
		r[5] *= keep;
		r[6] *= keep;
	}
	for (index = 0; index < count; index++)
	{
		const float *l = lights + index * LIGHT_FLOATS;
		float *r;
		int channel;

		for (at = 0; at < pt.recent_count && pt.recent_ids[at] != ids[index]; at++);
		if (at == pt.recent_count)
		{
			if (pt.recent_count == LIGHTS_MOST)
			{
				/* (no room: the dimmest recent one's place) */
				uint32_t dimmest = 0;

				for (at = 1; at < LIGHTS_MOST; at++)
					if (pt.recent_lights[at * LIGHT_FLOATS + 4] + pt.recent_lights[at * LIGHT_FLOATS + 5] +
						pt.recent_lights[at * LIGHT_FLOATS + 6] < pt.recent_lights[dimmest * LIGHT_FLOATS + 4] +
						pt.recent_lights[dimmest * LIGHT_FLOATS + 5] + pt.recent_lights[dimmest * LIGHT_FLOATS + 6])
						dimmest = at;
				at = dimmest;
			}
			else
				pt.recent_count++;
			memset(pt.recent_lights + at * LIGHT_FLOATS, 0, LIGHT_FLOATS * sizeof(float));
			pt.recent_ids[at] = ids[index];
		}
		r = pt.recent_lights + at * LIGHT_FLOATS;
		for (channel = 4; channel < 7; channel++)
			if (l[channel] > r[channel])
				r[channel] = l[channel];
		memcpy(r, l, 4 * sizeof(float));
		memcpy(r + 7, l + 7, 5 * sizeof(float));
	}
	for (index = 0; index < pt.recent_count; index++)
	{
		const float *r = pt.recent_lights + index * LIGHT_FLOATS;

		if (r[4] + r[5] + r[6] < 0.006f)
			continue;
		if (kept != index)
		{
			memmove(pt.recent_lights + kept * LIGHT_FLOATS, r, LIGHT_FLOATS * sizeof(float));
			pt.recent_ids[kept] = pt.recent_ids[index];
		}
		kept++;
	}
	pt.recent_count = kept;
	return given;
}

/* a dynamic light the path tracer takes: on, a point or a spot, reaching
somewhere, giving light */
static int dynamic_light_taken(const struct xgpu_stream_scene_light *l)
{
	return l->light_class == XGPU_SCENE_LIGHT_CLASS_DYNAMIC && (l->flags & XGPU_SCENE_LIGHT_FLAG_ON) &&
		(l->type == XGPU_SCENE_LIGHT_POINT || l->type == XGPU_SCENE_LIGHT_SPOT) && l->position[3] > 0.0f &&
		l->radiance[3] * (l->radiance[0] + l->radiance[1] + l->radiance[2]) > 0.0f;
}

/* the dynamic lights the frame takes, by their places in the table: all of
them in the table's order where LIGHTS_MOST or fewer; past that (a fight's
bolts, glows, flashes and the shields' flares), the LIGHTS_MOST that light
the most about the eye, each by its whole light (its colour's brightest
channel by its reach squared) over its distance's square past its reach,
in the table's order again (the job's lights keep their ids' order frame to
frame where the same are kept) */
static uint32_t dynamic_lights_rank(const struct xgpu_stream_scene_light *lights, uint32_t count, const float eye[4],
	uint32_t ranked[LIGHTS_MOST])
{
	static float scores[4096];
	static uint32_t found[4096];
	uint32_t index, taken = 0, kept;

	for (index = 0; lights && index < count && taken < 4096u; index++)
	{
		const struct xgpu_stream_scene_light *l = &lights[index];
		float dx, dy, dz, r, d2, bright;

		if (!dynamic_light_taken(l))
			continue;
		dx = l->position[0] - eye[0];
		dy = l->position[1] - eye[1];
		dz = l->position[2] - eye[2];
		r = l->position[3];
		d2 = dx * dx + dy * dy + dz * dz;
		bright = l->radiance[3] * fmaxf(l->radiance[0], fmaxf(l->radiance[1], l->radiance[2]));
		scores[taken] = bright * r * r / fmaxf(d2, r * r);
		found[taken++] = index;
	}
	if (taken <= LIGHTS_MOST)
	{
		memcpy(ranked, found, taken * sizeof(uint32_t));
		return taken;
	}
	/* (the least of the kept put out by a brighter one, then the kept put
	back in the table's order) */
	for (index = 0; index < LIGHTS_MOST; index++)
		ranked[index] = index;
	for (index = LIGHTS_MOST; index < taken; index++)
	{
		uint32_t least = 0, at;

		for (at = 1; at < LIGHTS_MOST; at++)
			if (scores[ranked[at]] < scores[ranked[least]])
				least = at;
		if (scores[index] > scores[ranked[least]])
			ranked[least] = index;
	}
	for (index = 1; index < LIGHTS_MOST; index++)
	{
		uint32_t value = ranked[index], at = index;

		for (; at > 0 && ranked[at - 1] > value; at--)
			ranked[at] = ranked[at - 1];
		ranked[at] = value;
	}
	for (kept = 0; kept < LIGHTS_MOST; kept++)
		ranked[kept] = found[ranked[kept]];
	return LIGHTS_MOST;
}
/* the traced light's upscaling (display.pt_light_scale, the phase's
XGPU_GBUFFER_PHASE_PT_LIGHT_SCALE bits; HALO_PT_LIGHT_SCALE is the setting's
own variable): the share of the G-buffer's pixels each way the light pass
and the denoiser run at, 0 for all of them (as before). The light's grid
is then smaller (the job "low" of its passes: each of its pixels a whole
pixel of the G-buffer, xgpu_vulkan_pt.glsl's lo_to_hi), its denoised light
brought to the G-buffer's grid by the pass "upsample" (a joint bilateral
filter by the G-buffer's depth and normals) and composite lights every
pixel of the G-buffer with its own albedo, normal and emission: geometry
and texture edges stay the G-buffer's, which display.upscaler then
anti-aliases at 1.0 (DLAA) or 0.67, where an upscaler from a smaller
G-buffer upscales the lit frame whole (work/openh1_eval/
v4_pt_clarity_runs/r2/light). Not with the denoiser off nor with the
light given raw to a plugin (Ray Reconstruction) */
static float light_scale_of(uint32_t phase)
{
	uint32_t hundredths = (phase & XGPU_GBUFFER_PHASE_PT_LIGHT_SCALE_MASK) >> XGPU_GBUFFER_PHASE_PT_LIGHT_SCALE_SHIFT;

	if (hundredths == 0u || hundredths >= 100u)
		return 0.0f;
	return (float)(hundredths < 25u ? 25u : hundredths) / 100.0f;
}

/* a camera (SCENE_VIEW's: its eye, axis, step across and step down, the
rays ray_of in xgpu_vulkan_pt.glsl makes of them) turned so that a pixel's
centre is its ray moved by the jitter (pixels of the view's target, right
and down): the axis less the jitter's share of the steps */
static void pt_camera_jitter(float camera[4][4], const int32_t corners[4], const float jitter[2])
{
	float width = (float)(corners[2] - corners[0]), height = (float)(corners[3] - corners[1]);
	float sx, sy;
	int axis;

	if ((jitter[0] == 0.0f && jitter[1] == 0.0f) || !(width >= 1.0f) || !(height >= 1.0f))
		return;
	sx = 2.0f * jitter[0] / width;
	sy = 2.0f * jitter[1] / height;
	for (axis = 0; axis < 3; axis++)
		camera[1][axis] -= sx * camera[2][axis] + sy * camera[3][axis];
}


struct xgpu_vulkan_image *xgpu_vulkan_pt_frame(struct xgpu_vulkan_image *light, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras,
	const struct xgpu_stream_scene_view *view, uint32_t phase, int *whole)
{
	struct xgpu_vulkan_bake_lookup lookup;
	const struct xgpu_vulkan_image_description *description;
	struct pt_job *job;
	struct pt_buffer *slot_buffer;
	VkDescriptorImageInfo infos[BINDINGS];
	VkWriteDescriptorSet writes[BINDINGS];
	struct xgpu_vulkan_image *bound[BINDINGS];
	VkDescriptorSet set;
	VkCommandBuffer commands;
	uint32_t index, slot, set_slot, time_slot = TIME_SLOTS, now, last, light_count = 0, serial = 0, dynamic = 0;
	uint32_t groups_x, groups_y, pt_view, flags = 0, source;
	const struct xgpu_stream_scene_light *lights;
	float *dynamic_lights;
	int reference, denoiser_off, steps, step, raw, still;
	uint32_t stabilized = 0, tree_bits = 0;
	/* (the traced light's upscaling: its job's slot and block, its grid's
	size and groups) */
	uint32_t low_slot = 0, low_width, low_height, low_groups_x, low_groups_y;
	struct pt_buffer *low_buffer = NULL;
	VkDeviceAddress light_job;
	float light_scale;
	int low;
	static int history_moving = -1, history_still = -1;
	static float most_still = -1.0f;
	uint64_t scene;

	if (whole)
		*whole = 0;
	if (!light || !base || !additions || !words || !extras || !view || !make())
		return NULL;
	times_collect();
	reference_noise_collect();
	reference_write();
	tree_collect();
	if (!xgpu_vulkan_bake_lookup(&lookup))
	{
		if (!pt.logged_no_bake)
			host_logf(HOST_LOG_INFO, "prism pt: no bake's scene to trace yet: CE's frame until it is made");
		pt.logged_no_bake = 1;
		pt.history_valid = 0;
		pt.history_kept = 0;
		return NULL;
	}
	pt.logged_no_bake = 0;
	/* (each scene traced named once: a new set's, the world's geometry
	changed in play, its tiles planned telling which world it is) */
	if (!pt.generation_logged || lookup.generation + 1u != pt.generation_logged)
	{
		host_logf(HOST_LOG_INFO, "prism pt: tracing the bake's scene of set %u (%u tiles planned): its own light, "
			"no bake's", lookup.generation, lookup.tiles_planned);
		pt.generation_logged = lookup.generation + 1u;
	}
	scene = lookup.scene;
	description = xgpu_vulkan_image_description(light);
	denoiser_off = (phase & XGPU_GBUFFER_PHASE_PT_DENOISER_OFF) != 0;
	/* (no light grid without the denoiser, under Ray Reconstruction or while
	a reference runs: each traces at the G-buffer's pixels) */
	light_scale = denoiser_off || xgpu_vulkan_upscale_pt_raw() || reference_requested() ? 0.0f :
		light_scale_of(phase);
	/* (the images at the grid asked, else a coarser one where the card has
	no room for them: images_fit) */
	pt.frames_undrawn = 0;
	light_scale = images_fit(description->width, description->height, light_scale, !denoiser_off &&
		!xgpu_vulkan_upscale_pt_raw() && !reference_requested());
	if (light_scale < 0.0f)
	{
		pt.history_valid = 0;
		pt.history_kept = 0;
		return NULL;
	}
	low_width = description->width;
	low_height = description->height;
	if (light_scale > 0.0f)
		pt_fit_low_size(description->width, description->height, light_scale, &low_width, &low_height);
	if (light_scale != pt.low_logged)
	{
		host_logf(HOST_LOG_INFO, light_scale > 0.0f ? "prism pt: the light traced at %ux%u of the G-buffer's %ux%u "
			"(display.pt_light_scale %.2f), brought up guided by its depth and normals" :
			"prism pt: the light traced at every pixel of the G-buffer (%ux%u of %ux%u, %.2f)", low_width, low_height,
			description->width, description->height, (double)light_scale);
		pt.low_logged = light_scale;
	}
	/* (the job block's slot and the descriptor set's, each once the GPU is
	done with its last frame) */
	slot = pt.job_next;
	set_slot = pt.set_next;
	if (pt.job_serial[slot] > xgpu_vulkan_completed() || pt.set_serial[set_slot] > xgpu_vulkan_completed())
	{
		host_logf(HOST_LOG_WARN, "prism pt: the frames' ring is full: the frame waits");
		xgpu_vulkan_wait(pt.job_serial[slot] > pt.set_serial[set_slot] ? pt.job_serial[slot] : pt.set_serial[set_slot]);
	}
	pt.job_next = (slot + 1) % JOB_SLOTS;
	pt.set_next = (set_slot + 1) % SET_SLOTS;
	slot_buffer = &pt.jobs[slot];
	low = low_width != description->width || low_height != description->height;
	if (low)
	{
		/* (the light grid's job: the next slot, once the GPU is done with it) */
		low_slot = pt.job_next;
		if (pt.job_serial[low_slot] > xgpu_vulkan_completed())
			xgpu_vulkan_wait(pt.job_serial[low_slot]);
		pt.job_next = (low_slot + 1) % JOB_SLOTS;
		low_buffer = &pt.jobs[low_slot];
	}
	debug_line(slot);
	job = (struct pt_job *)slot_buffer->mapped;
	memset(job, 0, sizeof(*job));
	memset((char *)slot_buffer->mapped + JOB_COUNTERS_OFFSET, 0, 16);
	pt_view = (phase & XGPU_GBUFFER_PHASE_VIEW_MASK) >> XGPU_GBUFFER_PHASE_VIEW_SHIFT;
	if (pt_view < XGPU_GBUFFER_VIEW_PT_DIFFUSE || pt_view > XGPU_GBUFFER_VIEW_PT_LAST)
		pt_view = 0;
	if (((phase & XGPU_GBUFFER_PHASE_PT_PROFILE_MASK) >> XGPU_GBUFFER_PHASE_PT_PROFILE_SHIFT) ==
		XGPU_PT_PROFILE_PERFORMANCE)
		flags |= FLAG_PERFORMANCE;
	if (denoiser_off)
		flags |= FLAG_DENOISER_OFF;
	{
		static int probe_nee = -1;

		probe_nee = probe_nee < 0 ? env_int("HALO_PT_PROBE_NEE", 0) : probe_nee;
		if (probe_nee == 1)
			flags |= FLAG_PROBE_NEE;
		else if (probe_nee == 2)
			flags |= FLAG_PROBE_ONLY;
	}
	/* (the history: the last frame's, drawn by this, of the same window at
	the same size, from the same bake's scene; a frame is a submission or a
	few, so a gap of more than 16 is frames this mode did not draw) */
	if (pt.history_valid && (view->flags & XGPU_SCENE_VIEW_HISTORY) && scene == pt.scene_seen &&
		lookup.generation == pt.generation_seen && pt.last_serial + 16 >= xgpu_vulkan_serial())
		flags |= FLAG_HISTORY;
	reference = reference_take(view, scene, description->width, description->height);
	if (reference)
		flags |= FLAG_REFERENCE;
	/* (display.denoiser's plugin that denoises and upscales, in a view begun
	for it (DLSS Ray Reconstruction): the light given raw with the tracer's
	own guides, its own filter off; the denoiser's history dropped across a
	change, its images having held the guides) */
	raw = !reference && !denoiser_off && xgpu_vulkan_upscale_pt_raw();
	if (raw != pt.raw_last)
		flags &= ~(uint32_t)FLAG_HISTORY;
	pt.raw_last = raw;
	/* (the job's bake fields stay 0 (pages to tiles, sun, masks, lightmaps,
	the reflections and their counts) but the irradiance volume's, which
	display.prism_pt_bake (the phase's bit, HALO_PT_BAKE overriding it) reads
	of the drawn world's own whole bake alone: xgpu_vulkan_bake_lookup_light;
	docs/prism_pathtraced.md, "The bake's guide") */
	{
		static int forced = -2;
		struct xgpu_vulkan_bake_light baked;
		int wanted;

		if (forced == -2)
			forced = getenv("HALO_PT_BAKE") && getenv("HALO_PT_BAKE")[0] ? atoi(getenv("HALO_PT_BAKE")) != 0 : -1;
		wanted = forced >= 0 ? forced : (phase & XGPU_GBUFFER_PHASE_PT_BAKE) != 0;
		if (wanted && xgpu_vulkan_bake_lookup_light(&baked))
		{
			flags |= FLAG_BAKE;
			job->bricks = baked.bricks;
			job->coarse = baked.coarse;
			job->probes = baked.probes;
			job->counts[3] = baked.brick_entries;
			job->more[0] = baked.coarse_entries;
			job->volume[0] = baked.probe_spacing;
			job->volume[1] = baked.coarse_spacing;
		}
		if (((flags & FLAG_BAKE) ? 3u : wanted ? 2u : 1u) != pt.bake_logged)
		{
			host_logf(HOST_LOG_INFO, "prism pt: the bake's guide %s", (flags & FLAG_BAKE) ? "on: the drawn world's own "
				"whole bake (its irradiance volume) read where the traced light has nothing yet" : wanted ? "wanted, no whole bake of the drawn world's own: its own light alone" :
				"off (display.prism_pt_bake)");
			pt.bake_logged = (flags & FLAG_BAKE) ? 3u : wanted ? 2u : 1u;
		}
	}
	job->sums = pt.sums.address;
	job->stats = pt.stats.address;
	/* (the reprojection's counters: only for the diagnostics' file, their
	atomics a sixteenth of the pixels' on one address cost the temporal
	pass several milliseconds) */
	{
		static int counted = -1;

		if (counted < 0)
			counted = getenv("HALO_PT_DEBUG_FILE") && getenv("HALO_PT_DEBUG_FILE")[0];
		if (counted)
			job->counters = slot_buffer->address + JOB_COUNTERS_OFFSET;
	}
	/* (the light tree, where it is built for this scene: the reference's
	lamps and ReSTIR DI's candidates take it beside the grid;
	HALO_PT_LAMP_TREE=0, the live frames the grid alone) */
	{
		static int lamp_tree = -1;

		tree_bits = 0;

		if (lamp_tree < 0)
			lamp_tree = getenv("HALO_PT_LAMP_TREE") ? atoi(getenv("HALO_PT_LAMP_TREE")) != 0 : 1;
		if (pt.tree_nodes && pt.tree_generation == lookup.generation && pt.tree.buffer && (lamp_tree || reference))
		{
			job->tree = pt.tree.address;
			job->extra[3] = (float)pt.tree_nodes;
			/* (ReSTIR DI's candidates from the tree, of its 4 (8 without
			the spatial reuse): HALO_PT_TREE_CANDIDATES, the rest the grid's) */
			{
				static int from_tree = -1, cones = -1;

				if (from_tree < 0)
					from_tree = getenv("HALO_PT_TREE_CANDIDATES") ?
						atoi(getenv("HALO_PT_TREE_CANDIDATES")) : TREE_CANDIDATES_DEFAULT;
				if (cones < 0)
					cones = getenv("HALO_PT_TREE_CONES") ? atoi(getenv("HALO_PT_TREE_CONES")) != 0 : TREE_CONES_DEFAULT;
				/* (in the reuse flags, below: the count in bits 4 to 7, bit 3
				where the nodes' emitter cones bound the importance. They were
				job.volume's w, which the recent dynamic lights' count then
				took for itself: the tree's candidates were 1 + that count
				mod 16, its cones on past 16 of them) */
				tree_bits = ((uint32_t)(from_tree < 1 ? 1 : from_tree > 8 ? 8 : from_tree) << 4) | (cones ? 8u : 0u);
			}
		}
	}
	/* (the radiance cache (HALO_PT_CACHE=0 none): a world-space hash grid of
	the diffuse bounce's light at its hits, which the bounce reads where a
	cell has its samples; the live frames only, never the reference; and the
	indirect light's clamp, HALO_PT_INDIRECT_CLAMP, a bounce's luminance in
	the bake's unit (0: none)) */
	{
		static int cache_on = -1;
		static float clamp = -1.0f, cell = 0.03f, least = 8.0f, most = 256.0f;

		if (cache_on < 0)
		{
			cache_on = getenv("HALO_PT_CACHE") ? atoi(getenv("HALO_PT_CACHE")) != 0 : CACHE_DEFAULT;
			clamp = getenv("HALO_PT_INDIRECT_CLAMP") ? (float)atof(getenv("HALO_PT_INDIRECT_CLAMP")) :
				INDIRECT_CLAMP_DEFAULT;
			if (getenv("HALO_PT_CACHE_CELL"))
				cell = (float)atof(getenv("HALO_PT_CACHE_CELL"));
			if (getenv("HALO_PT_CACHE_LEAST"))
				least = (float)atof(getenv("HALO_PT_CACHE_LEAST"));
			if (getenv("HALO_PT_CACHE_MOST"))
				most = (float)atof(getenv("HALO_PT_CACHE_MOST"));
		}
		job->cache_settings[0] = clamp > 0.0f ? clamp : 0.0f;
		job->cache_settings[1] = cell;
		job->cache_settings[2] = least;
		job->cache_settings[3] = most;
		/* (the pixels' cells: a short history's cap at least that many times
		its surface's cell's mean of the pixels' light, xgpu_vulkan_pt.glsl's
		temporal pass; 0 none, and none in the reference) */
		{
			static int cap = -1;

			if (cap < 0)
				cap = env_int("HALO_PT_PRIOR_CAP", PRIOR_CAP_DEFAULT);
			job->low[3] = cache_on && !reference && cap > 0 ? (float)cap * 0.01f : 0.0f;
		}
		{
			static int feed = -1;

			if (feed < 0)
			{
				feed = env_int("HALO_PT_CACHE_FEED", CACHE_FEED_DEFAULT);
				feed = feed < 1 ? 1 : feed > 256 ? 256 : feed;
				while (feed & (feed - 1))
					feed &= feed - 1;
			}
			job->more[2] = (uint32_t)feed;
		}
		if (cache_on && !reference && !pt.cache_made)
		{
			pt.cache_made = 1;
			pt_memory_use = XGPU_VULKAN_MEMORY_PT_CACHE;
			if (!buffer_make((VkDeviceSize)CACHE_CELLS * CACHE_WORDS * 4, 0, &pt.cache))
				host_logf(HOST_LOG_WARN, "prism pt: no memory for the radiance cache: none");
			else
				host_logf(HOST_LOG_INFO, "prism pt: the radiance cache: %u cells (%u MB)", CACHE_CELLS,
					(unsigned)((size_t)CACHE_CELLS * CACHE_WORDS * 4 >> 20));
			pt_memory_use = XGPU_VULKAN_MEMORY_PT_OTHER;
			pt.cache_generation = ~lookup.generation;
		}
		if (cache_on && !reference && pt.cache.buffer)
			job->cache = pt.cache.address;
	}
	/* (the moving objects: the G-buffer pass's object draws, their
	structures built at its end, xgpu_vulkan_rt.c; HALO_PT_OBJECTS=0 none) */
	{
		uint64_t structure, table;
		uint32_t count;

		if (xgpu_vulkan_rt_objects(&structure, &table, &count))
		{
			job->objects = structure;
			job->object_table = table;
		}
	}
	memcpy(job->camera, view->now, sizeof(job->camera));
	memcpy(job->last_camera, (view->flags & XGPU_SCENE_VIEW_HISTORY) ? view->last : view->now, sizeof(job->last_camera));
	memcpy(job->corners, view->corners, sizeof(job->corners));
	memcpy(job->last_corners, (view->flags & XGPU_SCENE_VIEW_HISTORY) ? view->last_corners : view->corners,
		sizeof(job->last_corners));
	/* (an upscaler's jitter: the G-buffer was drawn moved by it, so a pixel's
	centre saw the surface the unjittered camera has at the centre less the
	jitter; each camera's axis turned by its frame's jitter, so that every
	point found again from a pixel's distance (the rays' origins, the
	reprojection) is on the surface drawn there. Before, a pixel's point was
	off its surface by up to half a pixel's footprint, by a different amount
	each of the sequence's frames: grazing sand and rock faces went in and
	out of their own shadow (the rays' offset is a centimetre or two), the
	frame's light changing with the jitter's phase, eight frames over) */
	{
		float jitter[2];

		if (!xgpu_vulkan_upscale_jitter(jitter))
			jitter[0] = jitter[1] = 0.0f;
		pt_camera_jitter(job->camera, job->corners, jitter);
		if (view->flags & XGPU_SCENE_VIEW_HISTORY)
			pt_camera_jitter(job->last_camera, job->last_corners, pt.last_jitter);
		else
			pt_camera_jitter(job->last_camera, job->last_corners, jitter);
		pt.last_jitter[0] = jitter[0];
		pt.last_jitter[1] = jitter[1];
	}
	job->counts[1] = description->width;
	job->counts[2] = description->height;
	job->frame[0] = ++pt.frame;
	job->frame[1] = flags;
	job->frame[2] = pt_view;
	/* (the lamp ray's roughness at most, in hundredths: a GGX ray of the
	pixels whose reflection the probes give, meeting the lamps' faces;
	HALO_PT_LAMP_RAY, 0.7 by default, 0 none) */
	{
		static int lamp_ray = -1, lamp_mis = -1;

		if (lamp_ray < 0)
		{
			const char *value = getenv("HALO_PT_LAMP_RAY");

			lamp_ray = (int)((value ? atof(value) : LAMP_RAY_DEFAULT) * 100.0 + 0.5);
			lamp_ray = lamp_ray < 0 ? 0 : lamp_ray > 100 ? 100 : lamp_ray;
		}
		job->frame[3] = (uint32_t)lamp_ray;
		if (lamp_mis < 0)
			lamp_mis = env_int("HALO_PT_LAMP_MIS", 1) != 0;
		if (lamp_mis)
			job->frame[1] |= FLAG_LAMP_MIS;
	}
	pt.job_frame[slot] = pt.frame;
	pt.job_flags[slot] = flags;
	pt.job_view_flags[slot] = view->flags;
	pt.job_moved[slot] = sqrtf((view->now[0][0] - view->last[0][0]) * (view->now[0][0] - view->last[0][0]) +
		(view->now[0][1] - view->last[0][1]) * (view->now[0][1] - view->last[0][1]) +
		(view->now[0][2] - view->last[0][2]) * (view->now[0][2] - view->last[0][2]));
	/* (a still eye: this frame's camera the last one's, its history kept;
	the histories may then be longer, the light being the same) */
	still = (view->flags & XGPU_SCENE_VIEW_HISTORY) && (flags & FLAG_HISTORY) &&
		!memcmp(view->now, view->last, sizeof(view->now));
	if (history_moving < 0)
	{
		history_moving = env_int("HALO_PT_HISTORY", HISTORY_DEFAULT);
		history_moving = history_moving < 8 ? 8 : history_moving > 255 ? 255 : history_moving;
		history_still = env_int("HALO_PT_HISTORY_STILL", HISTORY_STILL_DEFAULT);
		history_still = history_still < history_moving ? history_moving : history_still > 255 ? 255 : history_still;
		most_still = (float)env_int("HALO_PT_RESTIR_M_STILL", RESTIR_M_STILL_DEFAULT);
		most_still = most_still > 64.0f ? 64.0f : most_still;
	}
	/* (the exposure: the frame's, metered in the frame, not in the views) */
	exposure_settings(job->exposure);
	/* the settings: the sun's ray within 60 world units in "performance"
	(the baked mask beyond, design section 3.8), at every distance in
	"quality" (the mask's texels miss thin lit edges far off: round 2's G3,
	the overhang's sunlit rims and the far spire dark where R lights them),
	the reflections traced below a roughness of 0.35, the firefly clamp at 4
	times the brightest neighbour, the luminance's edge stop at 4 standard
	deviations */
	job->settings[0] = (flags & FLAG_PERFORMANCE) ? 60.0f : 1.0e6f;
	job->settings[1] = 0.35f;
	/* (HALO_PT_RAY_ROUGHNESS, a diagnosis: another roughness under which
	the reflection is traced, 1 at every pixel) */
	if (getenv("HALO_PT_RAY_ROUGHNESS"))
		job->settings[1] = (float)atof(getenv("HALO_PT_RAY_ROUGHNESS"));
	/* (ReSTIR DI's spatiotemporal reuse: the last frame's reservoirs at that
	many points about the place, besides its own; HALO_PT_RESTIR_SPATIAL) */
	{
		static int spatial = -1, traced_z = -1;

		if (spatial < 0)
			spatial = env_int("HALO_PT_RESTIR_SPATIAL", RESTIR_SPATIAL_DEFAULT);
		spatial = spatial < 0 ? 0 : spatial > 4 ? 4 : spatial;
		if (traced_z < 0)
			traced_z = getenv("HALO_PT_RESTIR_Z") ? atoi(getenv("HALO_PT_RESTIR_Z")) != 0 : 1;
		/* (16 more: the neighbours' confidences by the kept sample's support
		traced from each, HALO_PT_RESTIR_Z=1; else by their own sample's
		light here) */
		job->settings[2] = (float)spatial + (traced_z && spatial ? 16.0f : 0.0f);
	}
	/* (ReSTIR DI's reuse flags: the spatial reservoirs kept for the next
	frame, HALO_PT_RESTIR_FEEDBACK=1 (round 4's; by default the temporal
	reservoir is kept, so no sample walks from pixel to pixel over the
	frames); the temporal reuse by a permutation of the 4 x 4 about the
	place, HALO_PT_RESTIR_PERMUTE; the boiling filter's strength,
	HALO_PT_RESTIR_BOIL (0 to 1, 0 none)) */
	{
		static int feedback = -1, permute = -1, tree_lobe = -1;
		static float boil = -1.0f;
		int radius;

		if (feedback < 0)
			feedback = getenv("HALO_PT_RESTIR_FEEDBACK") ? atoi(getenv("HALO_PT_RESTIR_FEEDBACK")) != 0 : 0;
		if (permute < 0)
			permute = getenv("HALO_PT_RESTIR_PERMUTE") ? atoi(getenv("HALO_PT_RESTIR_PERMUTE")) != 0 :
				RESTIR_PERMUTE_DEFAULT;
		if (boil < 0.0f)
		{
			boil = getenv("HALO_PT_RESTIR_BOIL") ? (float)atof(getenv("HALO_PT_RESTIR_BOIL")) : RESTIR_BOIL_DEFAULT;
			boil = boil < 0.0f ? 0.0f : boil > 1.0f ? 1.0f : boil;
		}
		/* (HALO_PT_RESTIR_RADIUS: the spatial neighbours' disc's radius in
		pixels, 3 to 64; 16 by default) */
		radius = env_int("HALO_PT_RESTIR_RADIUS", RESTIR_RADIUS_DEFAULT);
		radius = radius < 3 ? 3 : radius > 64 ? 64 : radius;
		if (tree_lobe < 0)
			tree_lobe = env_int("HALO_PT_TREE_LOBE", TREE_LOBE_DEFAULT) != 0;
		job->reuse_flags = ((uint32_t)radius << 16) | (feedback ? REUSE_FEEDBACK : 0u) | (permute ? REUSE_PERMUTE : 0u) |
			((uint32_t)(boil * 100.0f + 0.5f) << 8) | (tree_lobe ? REUSE_TREE_LOBE : 0u) |
			tree_bits;
	}
	/* (a diagnosis of the light pass's cost: HALO_PT_PARTS_OFF, a mask of
	its parts left out (1 the sun's ray, 2 the domes, 4 the spatial reuse, 8
	the bounce, 16 the reflection's ray, 32 ReSTIR GI's reuse, 64 the spatial
	neighbours' traced Z); HALO_PT_PARTS_CYCLE=<frames>: each
	part left out alone in turn for that many frames, none first, so that
	one run's timestamps give each part's cost at one pose) */
	{
		static int parts = -1, cycle = -1;

		if (parts < 0)
			parts = getenv("HALO_PT_PARTS_OFF") ? atoi(getenv("HALO_PT_PARTS_OFF")) : 0;
		if (cycle < 0)
			cycle = getenv("HALO_PT_PARTS_CYCLE") ? atoi(getenv("HALO_PT_PARTS_CYCLE")) : 0;
		job->parts_off = (uint32_t)parts;
		if (cycle > 0)
		{
			uint32_t turn = (pt.frame / (uint32_t)cycle) % 8u;

			job->parts_off = turn ? 1u << (turn - 1u) : 0u;
		}
	}
	job->settings[3] = 4.0f;
	/* the histories: the slow one at most HALO_PT_HISTORY frames for a
	moving eye (32: 64 lagged in the dark interiors' strafes, a10's and c10's
	moving error 15 to 25% up) and HALO_PT_HISTORY_STILL for a still one
	(128: at 32 a still eye's view never passed 32 samples of a light few
	rays find; c10's lab 0.054 to 0.035 against R), the fast 4, the fast
	one's hold at 2 standard deviations of its mean (the anti-lag: a light
	that changes is followed within the fast history's frames) */
	job->denoise[0] = (float)(still ? history_still : history_moving);
	job->denoise[1] = 4.0f;
	job->denoise[3] = 2.0f;
	/* (a diagnosis: HALO_PT_FAST_GAMMA the fast history's hold in standard
	deviations) */
	if (getenv("HALO_PT_FAST_GAMMA"))
		job->denoise[3] = (float)atof(getenv("HALO_PT_FAST_GAMMA"));
	/* the short history's cap: a moving eye's new pixels held to K times
	their neighbours' brightest (the specular's 4 K), but never below F
	times the light the exposure shows as white (SHORT_CAP_DEFAULT,
	SHORT_CAP_FLOOR_DEFAULT: 4 and 4 times white). The cap alone took the
	light those pixels found brighter than their neighbours (c40's lab and
	a10's corridor darker and blurrier turning and strafing, over 90 frames
	to come back once still); no cap let single samples far past white
	through, which the denoiser and an upscaler smeared into white blobs
	(over the 240 moving frames of a10 and c40 at native and DLSS 0.67 to
	0.33: 42606 pixels past 230 where the cap had none). With the floor the
	moving frames keep about 90% of what the cap's removal gained (FLIP
	against the supersampled reference turning and strafing 0.0998 to
	0.0950, ghosting 27.3 to 24.8) and the blobs are back to the run to run
	noise (work/openh1_eval/v4_pt_clarity_runs/r2). The floor was 16 times
	white until the histories' anti-firefly (history_firefly) took the
	outliers that floor let through; with it 4 times white halves the
	bright disocclusions over a10's moving frames (DLSS 0.33 6710 to 3687
	new bright pixels a sweep, DLSS 0.67 with the light at 0.33 3607 to
	1400) and lowers c40's (8546 to 5237, 4823 to 3446), FLIP and edge SSIM
	against the references the same within 0.002, ghosting turning up to
	1.6 at c40 and 0.5 at a10 (work/openh1_eval/v4_pt_clarity_runs/r3).
	Diagnoses:
	HALO_PT_SHORT_CAP=<K> (1 to 255; 0 no cap at all), HALO_PT_SHORT_CAP_FLOOR=<F>
	in eighths of white (0 to 255; 0 none) */
	{
		static int short_cap = -1, short_floor = -1;

		if (short_cap < 0)
		{
			short_cap = env_int("HALO_PT_SHORT_CAP", SHORT_CAP_DEFAULT);
			short_cap = short_cap < 0 ? 0 : short_cap > 255 ? 255 : short_cap;
			short_floor = env_int("HALO_PT_SHORT_CAP_FLOOR", SHORT_CAP_FLOOR_DEFAULT);
			short_floor = short_floor < 0 ? 0 : short_floor > 255 ? 255 : short_floor;
		}
		if (short_cap == 0)
			job->frame[1] |= 512u;
		else
			job->frame[1] |= (uint32_t)short_cap << 16 | (uint32_t)short_floor << 24;
	}
	/* (ReSTIR GI's reservoirs' confidence at most 20 frames, the quality
	profile's bounce reused; HALO_PT_RESTIR_GI=0: none, each frame's own) */
	{
		static int restir_gi = -1;

		if (restir_gi < 0)
			restir_gi = env_int("HALO_PT_RESTIR_GI", RESTIR_GI_DEFAULT) != 0;
		job->denoise[2] = restir_gi ? 20.0f : 0.0f;
	}
	/* the lamps' candidates a pixel; the reference's paths a pixel a frame;
	ReSTIR DI's temporal reuse, its reservoirs' confidence at most 8 frames
	(HALO_PT_RESTIR 0: none, each pixel's own candidates alone) */
	{
		/* (HALO_PT_CANDIDATES: the live frames' lamp candidates a pixel, 1 to
		8; 8 by default, 4 where the spatial neighbours are reused (their
		reservoirs bring the rest); the reference keeps 8) */
		static int candidates = -1;

		if (candidates < 0)
			candidates = env_int("HALO_PT_CANDIDATES",
				env_int("HALO_PT_RESTIR_SPATIAL", RESTIR_SPATIAL_DEFAULT) > 0 ? CANDIDATES_SPATIAL_DEFAULT : 8);
		candidates = candidates < 1 ? 1 : candidates > 8 ? 8 : candidates;
		job->extra[0] = reference ? 8.0f : (float)candidates;
	}
	job->extra[1] = (float)(pt.ref_paths ? pt.ref_paths : 1u);
	{
		static int restir = -1;
		static float most = -1.0f;

		if (restir < 0)
			restir = getenv("HALO_PT_RESTIR") ? atoi(getenv("HALO_PT_RESTIR")) != 0 : 1;
		/* (HALO_PT_RESTIR_M: the reservoirs' confidence cap, 8 frames by
		default) */
		if (most < 0.0f)
		{
			most = getenv("HALO_PT_RESTIR_M") ? (float)atof(getenv("HALO_PT_RESTIR_M")) : 8.0f;
			most = most < 1.0f ? 1.0f : most > 64.0f ? 64.0f : most;
		}
		job->extra[2] = restir ? (still && most_still > most ? most_still : most) : 0.0f;
	}
	/* (this frame's dynamic lights: SCENE_LIGHTS' dynamic class, those on:
	the place and reach, the colour by its intensity, a spot's inner cone's
	cosine (2 none), its axis and outer cone's cosine (2 a point light); then
	the recent ones, volume.w of them, which the temporal pass's history is
	cut by where they lit the surface otherwise) */
	uint32_t ids[LIGHTS_MOST], recent;
	dynamic_lights = (float *)((char *)slot_buffer->mapped + JOB_LIGHTS_OFFSET);
	lights = xgpu_vulkan_scene_light_table(&light_count, &serial);
	/* (more than LIGHTS_MOST on: those that light the most of the view
	kept, dynamic_lights_rank) */
	uint32_t ranked[LIGHTS_MOST], ranked_count = dynamic_lights_rank(lights, light_count, job->camera[0], ranked);
	for (index = 0; index < ranked_count; index++)
	{
		const struct xgpu_stream_scene_light *l = &lights[ranked[index]];
		float *to = dynamic_lights + dynamic * LIGHT_FLOATS;

		memcpy(to, l->position, 4 * sizeof(float));
		to[4] = l->radiance[0] * l->radiance[3];
		to[5] = l->radiance[1] * l->radiance[3];
		to[6] = l->radiance[2] * l->radiance[3];
		to[7] = l->type == XGPU_SCENE_LIGHT_SPOT && l->shape[0] > l->direction[3] ? l->shape[0] : 2.0f;
		memcpy(to + 8, l->direction, 3 * sizeof(float));
		to[11] = l->type == XGPU_SCENE_LIGHT_SPOT ? l->direction[3] : 2.0f;
		ids[dynamic] = l->id;
		dynamic++;
	}
	recent = recent_lights_take(dynamic_lights, ids, dynamic, (flags & FLAG_HISTORY) != 0, job->denoise[0]);
	job->volume[2] = (float)dynamic;
	job->volume[3] = (float)recent;
	job->lights = dynamic || recent ? slot_buffer->address + JOB_LIGHTS_OFFSET : 0;
	/* (HALO_DYNAMIC_LIGHTS_LOG, debug.dynamic_lights_log: the frame's
	dynamic lights where their count changed) */
	{
		static int log_on = -1;
		static uint32_t logged = 0xffffffffu;

		if (log_on < 0)
		{
			const char *value = getenv("HALO_DYNAMIC_LIGHTS_LOG");

			log_on = value && (value[0] == '1' || value[0] == '2' || value[0] == 't' || value[0] == 'y');
		}
		if (log_on && dynamic != logged)
		{
			logged = dynamic;
			host_logf(HOST_LOG_INFO, "prism pt: frame %u: %u dynamic lights of the table's %u", pt.frame + 1u,
				dynamic, light_count);
		}
		/* (HALO_DYNAMIC_LIGHTS_LOG=2: the first one's numbers each frame) */
		if (log_on && dynamic && getenv("HALO_DYNAMIC_LIGHTS_LOG")[0] == '2')
			host_logf(HOST_LOG_INFO, "prism pt: frame %u: the first dynamic light at %.2f %.2f %.2f reach %.3f, "
				"colour %.3f %.3f %.3f, cones %.3f %.3f", pt.frame + 1u, dynamic_lights[0], dynamic_lights[1],
				dynamic_lights[2], dynamic_lights[3], dynamic_lights[4], dynamic_lights[5], dynamic_lights[6],
				dynamic_lights[7], dynamic_lights[11]);
	}
	/* the images: this frame's histories at its parity, the last frame's at
	the other */
	now = pt.parity;
	last = pt.parity ^ 1u;
	memset(bound, 0, sizeof(bound));
	bound[0] = words;
	bound[1] = extras;
	bound[2] = base;
	bound[3] = additions;
	bound[IMAGE_NOISY_DIFFUSE] = pt.noisy_diffuse;
	bound[IMAGE_NOISY_SPECULAR] = pt.noisy_specular;
	bound[IMAGE_CLEAN_DIFFUSE] = pt.clean_diffuse;
	bound[IMAGE_CLEAN_SPECULAR] = pt.clean_specular;
	bound[IMAGE_SUN_RAW] = pt.sun_raw;
	bound[IMAGE_GUIDE_NOW] = pt.guide[now];
	bound[IMAGE_GUIDE_LAST] = pt.guide[last];
	bound[IMAGE_DIFF_LAST] = pt.diff[last];
	bound[IMAGE_DIFF_NOW] = pt.diff[now];
	bound[IMAGE_SPEC_LAST] = pt.spec[last];
	bound[IMAGE_SPEC_NOW] = pt.spec[now];
	bound[IMAGE_MOMENTS_LAST] = pt.moments[last];
	bound[IMAGE_MOMENTS_NOW] = pt.moments[now];
	bound[IMAGE_SUN_LAST] = pt.sun[last];
	bound[IMAGE_SUN_NOW] = pt.sun[now];
	bound[IMAGE_DIFF_A] = pt.diff_a;
	bound[IMAGE_DIFF_B] = pt.diff_b;
	bound[IMAGE_SPEC_A] = pt.spec_a;
	bound[IMAGE_SPEC_B] = pt.spec_b;
	bound[IMAGE_SUN_A] = pt.sun_a;
	bound[IMAGE_SUN_B] = pt.sun_b;
	bound[IMAGE_FRAME] = pt.out;
	bound[IMAGE_LIGHT_IN] = light;
	bound[IMAGE_SPEC_MOMENTS_LAST] = pt.spec_moments[last];
	bound[IMAGE_SPEC_MOMENTS_NOW] = pt.spec_moments[now];
	bound[IMAGE_RESERVOIR_POINT_LAST] = pt.reservoir_point[last];
	bound[IMAGE_RESERVOIR_POINT_NOW] = pt.reservoir_point[now];
	bound[IMAGE_RESERVOIR_INFO_LAST] = pt.reservoir_info[last];
	bound[IMAGE_RESERVOIR_INFO_NOW] = pt.reservoir_info[now];
	bound[IMAGE_GI_POINT_LAST] = pt.gi_point[last];
	bound[IMAGE_GI_POINT_NOW] = pt.gi_point[now];
	bound[IMAGE_GI_DATA_LAST] = pt.gi_data[last];
	bound[IMAGE_GI_DATA_NOW] = pt.gi_data[now];
	bound[IMAGE_EXPOSURE] = pt.exposure;
	bound[IMAGE_STAB_DIFF_LAST] = pt.stab_diff[last];
	bound[IMAGE_STAB_DIFF_NOW] = pt.stab_diff[now];
	bound[IMAGE_STAB_SPEC_LAST] = pt.stab_spec[last];
	bound[IMAGE_STAB_SPEC_NOW] = pt.stab_spec[now];
	bound[IMAGE_UP_DIFF] = pt.up_diff;
	bound[IMAGE_UP_SPEC] = pt.up_spec;
	bound[IMAGE_UP_CLEAN_DIFFUSE] = pt.up_clean_diffuse;
	bound[IMAGE_UP_CLEAN_SPECULAR] = pt.up_clean_specular;
	set = pt.sets[set_slot];
	for (index = 0; index < BINDINGS; index++)
		write_image(&writes[index], &infos[index], set, index, bound[index], index < 4 || index == IMAGE_LIGHT_IN);
	vkUpdateDescriptorSets(pt.device.device, BINDINGS, writes, 0, NULL);
	/* (the water's rays this frame trace with this job, T8) */
	memcpy(pt.water_bound, bound, sizeof(pt.water_bound));
	pt.water_job = slot_buffer->address;
	pt.water_scene = scene;
	pt.water_reference = reference;
	commands = xgpu_vulkan_compute_commands();
	job->meter = xgpu_vulkan_bake_meter(commands);
	job->exposure[2] = !pt_view && job->meter ? 1.0f : 0.0f;
	for (index = 0; index < BINDINGS; index++)
		xgpu_vulkan_image_compute_use(bound[index], !(index < 4 || index == IMAGE_LIGHT_IN));
	groups_x = (description->width + 7) / 8;
	groups_y = (description->height + 7) / 8;
	/* (the light grid's job: this one's, its size the grid's, its corners
	and cameras the G-buffer's; the reference traces every pixel) */
	low = low && !reference;
	light_job = slot_buffer->address;
	low_groups_x = groups_x;
	low_groups_y = groups_y;
	if (low)
	{
		struct pt_job *grid = (struct pt_job *)low_buffer->mapped;

		memcpy(grid, job, sizeof(*grid));
		grid->counts[1] = low_width;
		grid->counts[2] = low_height;
		grid->low[0] = 1.0f;
		grid->low[1] = (float)description->width;
		grid->low[2] = (float)description->height;
		light_job = low_buffer->address;
		low_groups_x = (low_width + 7) / 8;
		low_groups_y = (low_height + 7) / 8;
	}
	if (pt.queries)
	{
		time_slot = pt.time_next;
		if (pt.time_serial[time_slot])
			time_slot = TIME_SLOTS;
		else
		{
			pt.time_next = (time_slot + 1) % TIME_SLOTS;
			vkCmdResetQueryPool(commands, pt.queries, time_slot * STAMPS, STAMPS);
		}
	}
	xgpu_vulkan_breadcrumb(commands, BREADCRUMB_BEGUN, "Path Traced's frame begun");
	barrier(commands);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipeline_layout, 0, 1, &set, 0, NULL);
	stamp(commands, time_slot, 0);
	/* (the light tree for this scene: the bake's lamps copied for the host to
	build it, once a generation) */
	if (!pt.tree_failed && !pt.lamps_serial && pt.tree_generation != lookup.generation &&
		(pt.lamps_readback.buffer || buffer_make((VkDeviceSize)(LAMPS_MOST + 1) * 64, 1, &pt.lamps_readback)))
	{
		job->lamps_out = pt.lamps_readback.address;
		dispatch(commands, PASS_LAMPS, scene, slot_buffer->address, LAMPS_MOST, 0, 0, 0, (LAMPS_MOST + 255) / 256, 1);
		pt.lamps_serial = xgpu_vulkan_serial();
		pt.lamps_generation = lookup.generation;
		barrier(commands);
	}
	if (reference)
	{
		/* the reference: summed while it runs, its noise measured every
		REFERENCE_MEASURE frames, its sums copied for the file once it
		stops; its means drawn */
		int adding = !pt.ref_stopped && pt.ref_count < pt.ref_frames;
		/* (HALO_PT_REFERENCE_LIVE=1, a diagnosis: the light pass's own
		estimate summed in R's place, its reservoirs reused from frame to
		frame as the live frames reuse them, no denoiser) */
		static int live = -1;

		if (live < 0)
			live = getenv("HALO_PT_REFERENCE_LIVE") ? atoi(getenv("HALO_PT_REFERENCE_LIVE")) != 0 : 0;
		if (adding)
		{
			pt.ref_count++;
			if (live)
			{
				dispatch(commands, PASS_LIGHT, scene, slot_buffer->address, 0, 0, 0, 0, groups_x, groups_y);
				barrier(commands);
			}
			dispatch(commands, PASS_REFERENCE, scene, slot_buffer->address, pt.ref_count, live ? 1u : 0u, 0, 0,
				groups_x, groups_y);
			if (pt.ref_count >= pt.ref_frames)
				pt.ref_stopped = 1;
		}
		stamp(commands, time_slot, 1);
		barrier(commands);
		if (adding && !pt.ref_stats_serial && ((pt.ref_count % REFERENCE_MEASURE) == 0u || pt.ref_stopped))
		{
			VkBufferCopy copy;

			dispatch(commands, PASS_REFERENCE_STATS, scene, slot_buffer->address, pt.ref_count,
				description->width * description->height, 0, 0, pt.ref_stats_groups, 1);
			barrier(commands);
			memset(&copy, 0, sizeof(copy));
			copy.size = (VkDeviceSize)pt.ref_stats_groups * 16;
			vkCmdCopyBuffer(commands, pt.stats.buffer, pt.stats_readback.buffer, 1, &copy);
			pt.ref_stats_serial = xgpu_vulkan_serial();
			pt.ref_stats_frames = pt.ref_count;
		}
		/* (stopped and its last noise measured: the sums copied for the file,
		once) */
		if (pt.ref_stopped && !pt.ref_copied && !pt.ref_stats_serial && pt.ref_count)
		{
			VkBufferCopy copy;

			memset(&copy, 0, sizeof(copy));
			copy.size = (VkDeviceSize)description->width * description->height * SUMS * 4;
			vkCmdCopyBuffer(commands, pt.sums.buffer, pt.sums_readback.buffer, 1, &copy);
			pt.ref_sums_serial = xgpu_vulkan_serial();
			pt.ref_copied = 1;
			host_logf(HOST_LOG_INFO, "prism pt: debug.pt_reference: stopped after %u frames (%s)", pt.ref_count,
				pt.ref_converged ? "its noise under the bound" : "its frames done");
		}
		barrier(commands);
		for (index = 2; index < TIMED; index++)
			stamp(commands, time_slot, index);
		dispatch(commands, PASS_COMPOSITE, scene, slot_buffer->address, 3, 0, 0, 0, groups_x, groups_y);
		stamp(commands, time_slot, TIMED);
		if (job->exposure[2] > 0.5f)
			xgpu_vulkan_bake_exposure_step(commands);
		pt.history_valid = live && adding;
		if (live && adding)
			pt.parity ^= 1u;
	}
	else
	{
		/* the light; the denoiser (temporal, fix, atrous: the one call site
		of the denoise step, where the plugins' denoise entry points go);
		the frame */
		if (job->cache && pt.cache_generation != lookup.generation)
		{
			/* (a new scene's cache: emptied) */
			vkCmdFillBuffer(commands, pt.cache.buffer, 0, VK_WHOLE_SIZE, 0);
			pt.cache_generation = lookup.generation;
			barrier(commands);
		}
		dispatch(commands, PASS_LIGHT, scene, light_job, 0, 0, 0, 0, low_groups_x, low_groups_y);
		if (job->cache)
		{
			/* (the cache's cells take this frame's samples) */
			barrier(commands);
			dispatch(commands, PASS_CACHE, scene, light_job, CACHE_CELLS, 0, 0, 0, CACHE_CELLS / 256u, 1);
		}
		stamp(commands, time_slot, 1);
		barrier(commands);
		if (raw)
		{
			/* (the denoiser's passes not run: composite gives the plugin its
			light and guides, source 4) */
			stamp(commands, time_slot, 2);
			stamp(commands, time_slot, 3);
			stamp(commands, time_slot, 4);
			source = 4u;
		}
		else if (!denoiser_off)
		{
			/* (the pre-blur, HALO_PT_PREBLUR=1: the fix pass's first use, the
			light as traced spread over a small disc before the histories
			take it; else the temporal pass reads the light as traced) */
			static int preblur = -1;

			if (preblur < 0)
				preblur = getenv("HALO_PT_PREBLUR") ? atoi(getenv("HALO_PT_PREBLUR")) != 0 : 0;
			if (preblur)
			{
				dispatch(commands, PASS_FIX, scene, light_job, 1u, 0, 0, 0, low_groups_x, low_groups_y);
				barrier(commands);
			}
			/* (the temporal pass reads the pre-blur's b, else the light as
			traced itself: no pass copies it) */
			dispatch(commands, PASS_TEMPORAL, scene, light_job, preblur ? 0u : 1u, spec_hold(), 0, 0, low_groups_x,
				low_groups_y);
			stamp(commands, time_slot, 2);
			barrier(commands);
			dispatch(commands, PASS_FIX, scene, light_job, 0, hist_firefly(), 0, 0, low_groups_x, low_groups_y);
			stamp(commands, time_slot, 3);
			barrier(commands);
			/* (the fix wrote b: the steps from b to a and back; the specular's
			the first three, its reach narrowed by its roughness) */
			steps = atrous_steps();
			for (step = 0; step < steps; step++)
			{
				/* (HALO_PT_ATROUS_WIDE: four steps at strides 1, 2, 4 and 16,
				the five steps' reach for four's time: after the first three
				the light is smooth over about 8 pixels, so the widest step's
				taps 16 apart do not alias) */
				uint32_t level = steps == 4 && atrous_wide() && step == 3 ? 4u : (uint32_t)step;

				dispatch(commands, PASS_ATROUS, scene, light_job, level, (step & 1) == 0 ? 1u : 0u,
					level < 3 ? 7u : 5u, 0, low_groups_x, low_groups_y);
				barrier(commands);
			}
			/* (an even count of steps ends in b, an odd one in a) */
			source = (steps & 1) == 0 ? 1u : 0u;
			/* (the temporal stabilisation of the filtered light: the
			composite then reads its result) */
			if (stabilize_on())
			{
				dispatch(commands, PASS_STABILIZE, scene, light_job, source, (uint32_t)stabilize_on(), 0, 0,
					low_groups_x, low_groups_y);
				barrier(commands);
				stabilized = 1u;
			}
			stamp(commands, time_slot, 4);
		}
		else
		{
			stamp(commands, time_slot, 2);
			stamp(commands, time_slot, 3);
			stamp(commands, time_slot, 4);
			source = 2u;
		}
		if (low)
		{
			/* (the traced light's upscaling: the denoised light brought to
			the G-buffer's grid, which composite reads as its source 5) */
			dispatch(commands, PASS_UPSAMPLE, scene, light_job, source, stabilized, 0, 0, groups_x, groups_y);
			barrier(commands);
			source = 5u;
			stabilized = 0;
		}
		dispatch(commands, PASS_COMPOSITE, scene, slot_buffer->address, source, stabilized, 0, 0, groups_x, groups_y);
		stamp(commands, time_slot, TIMED);
		/* (the exposure's step: the next frame's exposure from this one's
		meter, as Raster's frame makes it) */
		if (job->exposure[2] > 0.5f)
			xgpu_vulkan_bake_exposure_step(commands);
		/* (with the light raw the history is ReSTIR's reservoirs' alone, which
		the light pass keeps) */
		pt.history_valid = !denoiser_off;
		pt.parity ^= 1u;
	}
	/* the frame's exposure, and with the light raw its light and guides, for
	the view's plugin (xgpu_vulkan_upscale.c, at the view's END): the
	denoiser's images, free while it does not run: diff_a the light, spec_a
	and spec_b the albedos, diff_b the normal and roughness, sun_a the
	reflection ray's length */
	xgpu_vulkan_upscale_pt_frame(pt.exposure, (uint32_t)(job->exposure[1] + 0.5f), raw ? pt.diff_a : NULL,
		raw ? pt.spec_a : NULL, raw ? pt.spec_b : NULL, raw ? pt.diff_b : NULL, raw ? pt.sun_a : NULL);
	barrier(commands);
	host_barrier(commands);
	xgpu_vulkan_breadcrumb(commands, BREADCRUMB_DONE, "Path Traced's frame done");
	pt.job_serial[slot] = xgpu_vulkan_serial();
	if (low_buffer)
		pt.job_serial[low_slot] = xgpu_vulkan_serial();
	pt.set_serial[set_slot] = xgpu_vulkan_serial();
	if (time_slot < TIME_SLOTS)
		pt.time_serial[time_slot] = xgpu_vulkan_serial();
	pt.last_serial = xgpu_vulkan_serial();
	pt.scene_seen = scene;
	pt.generation_seen = lookup.generation;
	pt.history_kept = (flags & FLAG_HISTORY) != 0;
	if (!pt.logged_first)
	{
		pt.logged_first = 1;
		host_logf(HOST_LOG_INFO, "prism pt: first frame %ux%u (%s, denoiser %s): its own light (no bake's), %u "
			"dynamic lights, the radiance cache %s", description->width, description->height,
			(flags & FLAG_PERFORMANCE) ? "performance" : "quality", denoiser_off ? "off" : raw ? "the plugin's" : "prism",
			dynamic, job->cache ? "on" : "off");
		xgpu_vulkan_memory_log("path traced's first frame");
	}
	if (whole)
		*whole = 1;
	return pt.out;
}

/* ---------- the terrain overhaul's water (T8) */

int xgpu_vulkan_pt_water(struct xgpu_vulkan_image *ripples, struct xgpu_vulkan_image *base, const float plane[4],
	float ripple_strength, struct xgpu_vulkan_image **up, struct xgpu_vulkan_image **down,
	struct xgpu_vulkan_image **aux, int *reference)
{
	const struct xgpu_vulkan_image_description *description;
	VkDescriptorImageInfo infos[BINDINGS];
	VkWriteDescriptorSet writes[BINDINGS];
	struct xgpu_vulkan_image *bound[BINDINGS];
	VkDescriptorSet set;
	VkCommandBuffer commands;
	uint32_t index, set_slot, count, groups_x, groups_y, half_width, half_height;
	union
	{
		float value;
		uint32_t bits;
	} strength;
	struct pt_push push;

	*up = *down = *aux = NULL;
	*reference = 0;
	if (!pt.made || !pt.water_job || !ripples || !base || !pt.width)
		return 0;
	description = xgpu_vulkan_image_description(base);
	if (description->width != pt.width || description->height != pt.height ||
		xgpu_vulkan_image_description(ripples)->width != pt.width ||
		xgpu_vulkan_image_description(ripples)->height != pt.height)
		return 0;
	/* (the rays at half the resolution: one a 2x2 block of the frame, the
	composite's first pass's; the sums in the format the pass declares) */
	half_width = (pt.width + 1) / 2;
	half_height = (pt.height + 1) / 2;
	if (!(pt.water_up = image_make(pt.water_up, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)) ||
		!(pt.water_down = image_make(pt.water_down, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)) ||
		!(pt.water_sum_up = image_make(pt.water_sum_up, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)) ||
		!(pt.water_sum_down = image_make(pt.water_sum_down, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)) ||
		!(pt.water_aux = image_make(pt.water_aux, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)) ||
		!(pt.water_sum_aux = image_make(pt.water_sum_aux, XGPU_VULKAN_FORMAT_RGBA16F, half_width, half_height)))
		return 0;
	set_slot = pt.set_next;
	if (pt.set_serial[set_slot] > xgpu_vulkan_completed())
		xgpu_vulkan_wait(pt.set_serial[set_slot]);
	pt.set_next = (set_slot + 1) % SET_SLOTS;
	set = pt.sets[set_slot];
	/* (the frame's images, the water's in four of their places: the ripples
	and the base map's capture sampled, the rays' light and the reference's
	sums written) */
	memcpy(bound, pt.water_bound, sizeof(bound));
	bound[2] = ripples;
	bound[3] = base;
	bound[IMAGE_NOISY_DIFFUSE] = pt.water_up;
	bound[IMAGE_NOISY_SPECULAR] = pt.water_down;
	bound[IMAGE_CLEAN_DIFFUSE] = pt.water_sum_up;
	bound[IMAGE_CLEAN_SPECULAR] = pt.water_sum_down;
	bound[IMAGE_SUN_RAW] = pt.water_aux;
	bound[IMAGE_SUN_A] = pt.water_sum_aux;
	for (index = 0; index < BINDINGS; index++)
		if (!bound[index])
			return 0;
	for (index = 0; index < BINDINGS; index++)
		write_image(&writes[index], &infos[index], set, index, bound[index], index < 4 || index == IMAGE_LIGHT_IN);
	vkUpdateDescriptorSets(pt.device.device, BINDINGS, writes, 0, NULL);
	commands = xgpu_vulkan_compute_commands();
	for (index = 0; index < BINDINGS; index++)
		xgpu_vulkan_image_compute_use(bound[index], !(index < 4 || index == IMAGE_LIGHT_IN));
	/* (the reference's frame in its sum: 0 live, its number while it sums,
	and the sums kept once it stops) */
	count = 0;
	if (pt.water_reference)
		count = pt.ref_stopped && pt.ref_copied ? 0xffffffffu : pt.ref_count ? pt.ref_count : 1u;
	strength.value = ripple_strength;
	groups_x = (half_width + 7) / 8;
	groups_y = (half_height + 7) / 8;
	barrier(commands);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipeline_layout, 0, 1, &set, 0, NULL);
	memset(&push, 0, sizeof(push));
	push.scene = pt.water_scene;
	push.job = pt.water_job;
	push.parameters[0] = strength.bits;
	push.parameters[1] = count;
	push.parameters[2] = pt.frame;
	memcpy(push.values, plane, sizeof(push.values));
	xgpu_vulkan_breadcrumb(commands, (uint32_t)PASS_WATER + 1u, pass_names[PASS_WATER]);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt.pipelines[PASS_WATER]);
	vkCmdPushConstants(commands, pt.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(commands, groups_x, groups_y, 1);
	barrier(commands);
	pt.set_serial[set_slot] = xgpu_vulkan_serial();
	*up = pt.water_up;
	*down = pt.water_down;
	*aux = pt.water_aux;
	*reference = pt.water_reference;
	return 1;
}

void xgpu_vulkan_pt_water_frame_end(void)
{
	pt.water_job = 0;
	/* (Path Traced not drawn for 4 s, another lighting chosen: its images of
	the window's size, 1.8 GB at 3440x1440, let go of for the lighting drawn;
	made again as it draws again) */
	if (pt.out && ++pt.frames_undrawn >= 120 && xgpu_vulkan_pt_release())
		host_logf(HOST_LOG_INFO, "prism pt: not drawn for 120 frames: its images let go of");
}

int xgpu_vulkan_pt_release(void)
{
	if (!pt.made || !pt.out)
		return 0;
	images_free();
	memset(&pt.memory, 0, sizeof(pt.memory));
	pt.low_logged = -1.0f;
	return 1;
}

int xgpu_vulkan_pt_history_kept(void)
{
	return pt.history_kept;
}

void xgpu_vulkan_pt_shutdown(void)
{
	uint32_t index;
	struct xgpu_vulkan_image **images[] =
	{
		&pt.noisy_diffuse, &pt.noisy_specular, &pt.clean_diffuse, &pt.clean_specular, &pt.sun_raw, &pt.guide[0],
		&pt.guide[1], &pt.diff[0], &pt.diff[1], &pt.spec[0], &pt.spec[1], &pt.moments[0], &pt.moments[1],
		&pt.spec_moments[0], &pt.spec_moments[1], &pt.sun[0], &pt.sun[1], &pt.diff_a, &pt.diff_b, &pt.spec_a, &pt.spec_b,
		&pt.sun_a, &pt.sun_b, &pt.out, &pt.reservoir_point[0], &pt.reservoir_point[1], &pt.reservoir_info[0],
		&pt.reservoir_info[1], &pt.gi_point[0], &pt.gi_point[1], &pt.gi_data[0], &pt.gi_data[1], &pt.exposure,
		&pt.water_up, &pt.water_down, &pt.water_sum_up, &pt.water_sum_down, &pt.water_aux, &pt.water_sum_aux,
		&pt.stab_diff[0], &pt.stab_diff[1], &pt.stab_spec[0], &pt.stab_spec[1],
	};
	struct pt_buffer *buffers[] = { &pt.sums, &pt.stats, &pt.stats_readback, &pt.sums_readback, &pt.lamps_readback,
		&pt.tree, &pt.cache };

	/* (a compile still running: done before the device can go; its
	pipelines then let go of with the rest, or alone where none was made) */
	pipelines_wait();
	if (!pt.made)
	{
		for (index = 0; index < PASSES; index++)
			if (pt.pipelines[index])
				vkDestroyPipeline(pt.device.device, pt.pipelines[index], NULL);
		memset(pt.pipelines, 0, sizeof(pt.pipelines));
		return;
	}
	for (index = 0; index < sizeof(images) / sizeof(images[0]); index++)
	{
		if (*images[index])
			xgpu_vulkan_image_destroy(*images[index]);
		*images[index] = NULL;
	}
	for (index = 0; index < sizeof(buffers) / sizeof(buffers[0]); index++)
		if (buffers[index]->buffer)
		{
			xgpu_vulkan_memory_counted(buffers[index]->use, buffers[index]->type, -(int64_t)buffers[index]->counted);
			vkDestroyBuffer(pt.device.device, buffers[index]->buffer, NULL);
			vkFreeMemory(pt.device.device, buffers[index]->memory, NULL);
			memset(buffers[index], 0, sizeof(*buffers[index]));
		}
	for (index = 0; index < JOB_SLOTS; index++)
		if (pt.jobs[index].buffer)
		{
			xgpu_vulkan_memory_counted(pt.jobs[index].use, pt.jobs[index].type, -(int64_t)pt.jobs[index].counted);
			vkDestroyBuffer(pt.device.device, pt.jobs[index].buffer, NULL);
			vkFreeMemory(pt.device.device, pt.jobs[index].memory, NULL);
			memset(&pt.jobs[index], 0, sizeof(pt.jobs[index]));
		}
	for (index = 0; index < PASSES; index++)
		if (pt.pipelines[index])
			vkDestroyPipeline(pt.device.device, pt.pipelines[index], NULL);
	if (pt.pipeline_layout)
		vkDestroyPipelineLayout(pt.device.device, pt.pipeline_layout, NULL);
	if (pt.layout)
		vkDestroyDescriptorSetLayout(pt.device.device, pt.layout, NULL);
	if (pt.pool)
		vkDestroyDescriptorPool(pt.device.device, pt.pool, NULL);
	if (pt.sampler)
		vkDestroySampler(pt.device.device, pt.sampler, NULL);
	if (pt.queries)
		vkDestroyQueryPool(pt.device.device, pt.queries, NULL);
	if (pt.times_file)
		fclose(pt.times_file);
	if (pt.debug_file)
		fclose(pt.debug_file);
	memset(&pt, 0, sizeof(pt));
}
