/*
XGPU_STREAM.H

The renderer's command stream: a frame's backend calls (xgpu_backend.h)
written by the game as plain data into a ring in its own memory, and
played back by a backend on the 64-bit host, on a thread of its own, while
the game goes on to the next frame. On the Linux arm64 build, Android and
macOS the game is ILP32 guest code: one host call a frame hands the frame
over instead of a crossing for each GL call, the backend's driver work
leaves the game's thread, and in the VR mode the host can take the newest
head pose just before it submits.

Both sides include this file, so it holds only fixed-width integers and
floats, laid out alike by the guest's ILP32 compiler and the host's LP64
one: every field is 4 bytes or smaller and aligned to its size, every
record a multiple of 4 bytes (the sizes are checked at the end). The
interface's own structures cannot cross as they are: unsigned long and
DWORD are 4 bytes in the guest and 8 on the host.

The guest writes (xgpu_stream_encode.c), the host reads
(port/android/host/host_render_stream.c, which calls the backend's
functions: host_render_stream.h). Guest data is a guest address: the
host's view of it is host_address(address) (port/android/host/host.h). Data
the game rewrites while the frame waits (streamed vertices, indices,
immediate mode, constants, uniforms, mirror writes, the lights) is copied
into the ring after its record ("inline": its address is in the ring);
what is too large for the ring (more than an eighth of it) is copied into
a buffer of its own in guest memory, which the guest frees once the host
has released the record. Textures and palettes are read where they are, as
the Xbox's GPU reads them; their changes are the write tracking's
(memory_watch.c). Refer to "The renderer's command stream" in
port/linux/README.md.
*/

#ifndef __HALO_XGPU_STREAM_H
#define __HALO_XGPU_STREAM_H

#include <stdint.h>

#define XGPU_STREAM_MAGIC 0x53504758u /* "XGPS" */
/* 38: GOAT H1's 20 to 23 (the ray traced occlusion's WORLD_AO_MESH and _INSTANCES, the render marks,
DYNAMIC_LIGHTS_CHANGE, SHARP_UPSCALE, the HDR curve, peak and gamut, PASS_TIMES, the ring's GPU and
render times, the capabilities' ray_queries to device) merged with PRISM v4's 20 to 37: PRISM's records
after GOAT H1's, the scene records still last (docs/prism_scene_records.md);
39: PRISM v4 M5's UPSCALE (display.upscaler and display.denoiser's plugin, docs/prism_plugin.md), which
was 38 on its own branch, after 38's records; 40: the terrain overhaul's PRISM_TERRAIN (T0,
docs/prism_terrain.md), 39 on its own branch, after UPSCALE, the last scene record; 41: PRISM_TERRAIN's
FRAME carries the terrain pack's folder (T1); 42: its WATER_DRAW sent (T7), three items of
xgpu_stream_terrain_water (41 on its own branch, after T1's 41) */
#define XGPU_STREAM_VERSION 42
#define XGPU_STREAM_LOADING_ART_BYTES (640u * 480u * 4u)

/* ---------- the ring

The guest's: a header and the ring's bytes, both in guest memory, the bytes
aligned to XGPU_STREAM_RING_ALIGNMENT and a power of two in size (so that a
Metal backend can wrap them in one buffer without copying:
newBufferWithBytesNoCopy). Positions are running counts of bytes written
since the stream began, modulo 2^32: byte position p is at buffer + (p &
(size - 1)). The guest writes records from committed on, at most to
released + size; it publishes them with host_render_stream_submit, which
also counts the frames; the host releases bytes once the GPU is done with
the work that read them (the backend's fence). */

#define XGPU_STREAM_RING_ALIGNMENT 0x4000u
/* records and inline data: records start at 4 bytes, inline data at 16
(vertex data's alignment in every API) */
#define XGPU_STREAM_RECORD_ALIGNMENT 4u
#define XGPU_STREAM_DATA_ALIGNMENT 16u

struct xgpu_stream_ring
{
	/* XGPU_STREAM_MAGIC, XGPU_STREAM_VERSION, sizeof(struct xgpu_stream_ring) */
	uint32_t magic, version, header_size;
	/* the ring's bytes: guest address and size (a power of two) */
	uint32_t buffer, size;
	/* the most frames submitted and not yet retired (the GPU done with
	them): 2, the frame the host draws and the one the game writes */
	uint32_t frames_in_flight;
	/* the host's answers, in guest memory the guest allocates:
	visibility: XGPU_STREAM_VISIBILITY_SLOTS counts (uint32_t), the latest
	known for each slot, each at most 0x7fffffff (the game reads them as
	longs and halts on a negative one: XGPU_VISIBILITY_COUNT_MOST), then
	XGPU_STREAM_VISIBILITY_SLOTS test numbers (uint32_t), each the number
	of the test whose count the slot's is (the visibility end record's
	test; 0: none yet), written after its count with a release store, so
	that a guest reading the number first (an acquire load) reads a count
	of that test or a later one; pipelines: XGPU_STREAM_PIPELINES states (uint8_t,
	XGPU_STREAM_PIPELINE_*) of the pipeline ids */
	uint32_t visibility, pipelines;

	/* the guest's (written before each host_render_stream_submit, read by
	the host in it) */
	uint32_t committed;
	uint32_t frames_submitted;

	/* the host's (read by the guest with acquire loads) */
	uint32_t released;
	uint32_t frames_retired;
	/* positions decoded; nonzero after a malformed record (then the host
	decodes no more: XGPU_STREAM_ERROR_*) and where it was */
	uint32_t decoded;
	uint32_t error, error_position;

	/* statistics: the guest's (bytes of the last frame, the most a frame
	has had, VERTICES records sent again for a draw in a later submit)
	and the host's (the times and nanoseconds the guest waited for room, a
	frame to retire or a synchronous record; nanoseconds the last frame
	took to play, the backend's calls included) */
	uint32_t frame_bytes, frame_bytes_most;
	uint32_t vertices_again;
	uint32_t waits, wait_nanoseconds;
	uint32_t decode_nanoseconds;
	/* the host's display (display.hdr: written by the host whenever it
	looks, read by the guest between frames): whether it shows HDR (its
	potential headroom past XGPU_HDR_DISPLAY_HEADROOM, or an HDR swapchain),
	and its headroom now (a float's bits, 1.0 for none); its SDR white in
	nits (a float's bits: the system's, or the one the encoding takes where
	it gives none) and whether the system gave it */
	uint32_t display_hdr;
	uint32_t display_headroom;
	uint32_t display_white;
	uint32_t display_white_reported;
	/* the guest's, with each frame: microseconds more than its frames take
	that the next frame will (a load zone's switch in it): the host starts
	that frame so much earlier (display_wait) */
	uint32_t frame_extra_microseconds;
	/* the host's, running counts of microseconds modulo 2^32 (read by the
	guest with acquire loads at each frame presented: a report's,
	port/linux/src/report.c): the GPU's time on the frames, at each frame's
	end (host_render_stream_gpu_time: Metal's, the time the GPU was busy;
	Vulkan's, from each frame's first work to its last), and nonzero once
	the backend measures it; the render thread's time playing records (the
	backend's calls included, but its waits for the display:
	host_render_stream_display_blocking); the guest's waits for the
	display's pacing, a part of wait_nanoseconds (display_wait's, and its
	waits for the render thread or the GPU while the display held the render
	thread) */
	uint32_t gpu_microseconds;
	uint32_t gpu_measured;
	uint32_t render_microseconds;
	uint32_t paced_microseconds;
	/* the host's, at each frame's end: the replacement textures and meshes
	(display.prism_textures, display.prism_models) drawn in its last frames
	that are still to come, read, copied in or sharpened, drawn stock or
	less sharp meanwhile (debug.camera_path_settle); 0 with no store */
	uint32_t streams_pending;
};

enum
{
	XGPU_STREAM_ERROR_NONE,
	XGPU_STREAM_ERROR_RECORD,   /* an unknown type, or a size too small for it */
	XGPU_STREAM_ERROR_BOUNDS,   /* a record past committed, or inline data outside its record */
	XGPU_STREAM_ERROR_ID,       /* a pipeline or layout id out of range or never defined */
};

/* host_render_stream_submit's flags */
enum
{
	/* the records end a frame (after XGPU_STREAM_FRAME_END): returns once
	no more than frames_in_flight frames are unretired */
	XGPU_STREAM_SUBMIT_FRAME = 1,
	/* returns once the host has played the records and the GPU has done
	their work: a synchronous record's answer is written, data it named in
	place may change again */
	XGPU_STREAM_SUBMIT_SYNC = 2,
};

#define XGPU_STREAM_VISIBILITY_SLOTS 4096
#define XGPU_STREAM_PIPELINES 16384
#define XGPU_STREAM_LAYOUTS 4096

/* a pipeline id's state, written by the host when the backend first makes
it (the front end's lighting fallback reads it from the next frame) */
enum
{
	XGPU_STREAM_PIPELINE_UNKNOWN,
	XGPU_STREAM_PIPELINE_MADE,
	XGPU_STREAM_PIPELINE_FAILED,
};

/* ---------- records

Each starts with a word: its type in the low 8 bits, its size in bytes
(header and inline data included, a multiple of 4) in the high 24. A
record's inline data follows its fixed part, at the next 16-byte aligned
ring position, and lies inside the record. A record never wraps: one that
would not fit before the ring's end is preceded by XGPU_STREAM_SKIP of size
0, which skips to the end (any other SKIP skips its size).

Records named "synchronous" are submitted with XGPU_STREAM_SUBMIT_SYNC and
carry the guest address of their answer (reply). */

#define XGPU_STREAM_HEADER(type, size) ((uint32_t)(type) | ((uint32_t)(size) << 8))
#define XGPU_STREAM_HEADER_TYPE(header) ((header) & 0xffu)
#define XGPU_STREAM_HEADER_SIZE(header) ((header) >> 8)
#define XGPU_STREAM_RECORD_MAXIMUM 0xfffffcu

enum
{
	XGPU_STREAM_SKIP,
	/* the backend's life */
	XGPU_STREAM_INITIALIZE,            /* synchronous: xgpu_stream_capabilities */
	XGPU_STREAM_INVALIDATE,
	XGPU_STREAM_FLUSH,
	XGPU_STREAM_ANTI_ALIASING_PREPARE,
	XGPU_STREAM_ANTI_ALIAS,
	/* render targets */
	XGPU_STREAM_TARGET_CREATE,
	XGPU_STREAM_TARGET_DELETE,
	XGPU_STREAM_TARGET_MULTISAMPLE,
	XGPU_STREAM_TARGET_RESOLVE,
	XGPU_STREAM_SET_TARGETS,
	XGPU_STREAM_CLEAR,
	XGPU_STREAM_CLEAR_TARGET,
	XGPU_STREAM_READ_TARGET,           /* synchronous */
	/* textures */
	XGPU_STREAM_SET_TEXTURES,
	/* pipelines */
	XGPU_STREAM_CONSTANTS,
	XGPU_STREAM_VERTEX_PROGRAM,
	XGPU_STREAM_VERTEX_LIGHTING,
	XGPU_STREAM_PIPELINE_DEFINE,
	XGPU_STREAM_PIPELINE,
	XGPU_STREAM_DRAW_STATE,
	/* vertices and draws */
	XGPU_STREAM_MIRROR_WRITE,
	XGPU_STREAM_LAYOUT_DEFINE,
	XGPU_STREAM_VERTICES,
	XGPU_STREAM_DRAW,
	XGPU_STREAM_DRAW_INDICES,
	XGPU_STREAM_DRAW_INDEXED,
	XGPU_STREAM_DRAW_IMMEDIATE,
	/* visibility tests */
	XGPU_STREAM_VISIBILITY_BEGIN,
	XGPU_STREAM_VISIBILITY_END,
	XGPU_STREAM_VISIBILITY_FRAME,
	/* frames */
	XGPU_STREAM_PRESENT,
	XGPU_STREAM_FRAME_END,
	/* the enhanced dynamic lights */
	XGPU_STREAM_DYNAMIC_LIGHTS_PREPARE,
	XGPU_STREAM_DYNAMIC_LIGHTS_UPLOAD,
	XGPU_STREAM_DYNAMIC_LIGHTS_HALOS,
	/* the VR mode */
	XGPU_STREAM_VR_SUPPORTED,          /* synchronous: uint32_t */
	XGPU_STREAM_VR_INITIALIZE,         /* synchronous: xgpu_stream_vr_info, in and out */
	XGPU_STREAM_VR_EYES,
	XGPU_STREAM_VR_PASS,
	XGPU_STREAM_VR_GROUPS_RESET,
	XGPU_STREAM_VR_TIMING,
	XGPU_STREAM_VR_COMPOSE,
	XGPU_STREAM_VR_SUBMIT,
	/* the shaders and pipelines made */
	XGPU_STREAM_PREPARED,              /* synchronous: uint32_t, the milliseconds waited */
	XGPU_STREAM_PREPARE,
	/* the VR mode's state its composition reads, and what vr.c sets in the
	backend as the frame is drawn (vr.h) */
	XGPU_STREAM_VR_STATE,
	XGPU_STREAM_VR_EYE_TRANSFORMS,
	XGPU_STREAM_VR_SCREEN_CORRECTIONS,
	XGPU_STREAM_VR_SKY,
	XGPU_STREAM_VR_EYE_FEED,
	XGPU_STREAM_VR_EYE_TINT,
	XGPU_STREAM_LOADING,
	XGPU_STREAM_LOADING_OWNED,          /* synchronous: current presenter ownership */
	XGPU_STREAM_LOADING_ART,            /* value: byte count, followed by owned 640x480 RGBA pixels */
	XGPU_STREAM_LOADING_SCENE,          /* value: byte count, followed by the menu's scene (loading_scene.h) */
	XGPU_STREAM_LOADING_PROGRESS,       /* value: the load's progress, a float's bits (0 to 1) */
	XGPU_STREAM_LOADING_MODE,           /* value: 1 the sweep follows the progress (the Xbox's), 0 it loops */
	XGPU_STREAM_LOADING_CAPTION,        /* value: byte count, followed by a width and a height (uint32_t)
	                                       and that many bytes of a line's coverage; 0: none */
	/* display.hdr (xgpu_hdr.h) */
	XGPU_STREAM_HDR,
	XGPU_STREAM_TONE_MAP,
	/* the screenshot key's picture (controls.screenshot) */
	XGPU_STREAM_SCREENSHOT,            /* synchronous */
	/* display.picture_position: where a picture of another shape than the
	window sits in it (value: 0 centered, n + 1 n pixels from the top, no
	lower than the bottom) */
	XGPU_STREAM_PICTURE_POSITION,
	/* display.gamma: an SDR frame's curve as it is presented (value: the
	setting, a float's bits; 1 none) */
	XGPU_STREAM_GAMMA,
	/* display.panini (xgpu_backend_panini) */
	XGPU_STREAM_PANINI,
	/* display.ambient_occlusion "world" (xgpu_backend_world_ao_window,
	_field; docs/world-ao.md) */
	XGPU_STREAM_WORLD_AO_WINDOW,
	XGPU_STREAM_WORLD_AO_FIELD,
	/* the pipeline bound's stand-in (xgpu_backend_pipeline_stand_in) */
	XGPU_STREAM_PIPELINE_STAND_IN,
	/* the game's renderer's category of the draws from now on, for the
	backends' timing alone (xgpu_backend_category; value: XGPU_STREAM_CATEGORY_*) */
	XGPU_STREAM_CATEGORY,
	/* the enhanced dynamic lights' world (xgpu_backend_dynamic_lights_world) */
	XGPU_STREAM_DYNAMIC_LIGHTS_WORLD,
	/* display.ambient_occlusion "world" found once a pixel
	(xgpu_backend_world_ao_prepass) */
	XGPU_STREAM_WORLD_AO_PREPASS,
	/* display.ambient_occlusion "world"'s distance-field scene: a box of
	its cascades (xgpu_backend_world_ao_cascades) */
	XGPU_STREAM_WORLD_AO_CASCADES,
	/* display.ambient_occlusion "ray_traced": a mesh of the scene the GPU's
	ray queries trace, and the frame's instances of the meshes
	(xgpu_backend_world_ao_mesh, _instances) */
	XGPU_STREAM_WORLD_AO_MESH,
	XGPU_STREAM_WORLD_AO_INSTANCES,
	/* debug.render_marks: what the draws from now on belong to, for the
	inspector's replay (port/macos/tools/halo_replay.m); the backends play
	nothing for them (xgpu_stream_mark_*) */
	XGPU_STREAM_MARK_STRING,
	XGPU_STREAM_MARK_VIEW,
	XGPU_STREAM_MARK_OWNER,
	/* the enhanced dynamic lights' cells that changed
	(xgpu_backend_dynamic_lights_upload; DYNAMIC_LIGHTS_UPLOAD's records,
	of the whole, are the stream's version 22's) */
	XGPU_STREAM_DYNAMIC_LIGHTS_CHANGE,
	/* display.render_scale: a frame smaller than the window scaled up
	sharply (value 1: the render scale's) or smoothly (0: "original"
	resolution scaling's 640x480, as it always was) */
	XGPU_STREAM_SHARP_UPSCALE,
	/* display.hdr_sdr_curve: the curve the HDR encoding takes the frame's
	values to light by (value: xgpu_hdr.h's XGPU_HDR_SDR_CURVE_*, AUTO the
	backend's platform's) */
	XGPU_STREAM_HDR_SDR_CURVE,
	/* display.hdr_peak_nits (#1122): the display's peak in nits, of the
	player's (value; 0 the system's) */
	XGPU_STREAM_HDR_PEAK_NITS,
	/* display.hdr_gamut (#1124): the highlights' colours expanded past
	sRGB's (value 1; 0 sRGB's) */
	XGPU_STREAM_HDR_GAMUT,
	/* debug.timings: each pass timed on the GPU (value: every so many
	frames, 0 none), as the setting changes (xgpu_backend_pass_times) */
	XGPU_STREAM_PASS_TIMES,
	/* display.prism_material_overlay: the overlay of the draws that follow
	(xgpu_stream_material_overlay) */
	XGPU_STREAM_MATERIAL_OVERLAY,
	/* display.prism_shadows "cascaded": the sun's shadow maps' pass
	(xgpu_backend_prism_shadows) */
	XGPU_STREAM_PRISM_SHADOWS,
	/* display.prism_textures: the map's replacement textures
	(xgpu_stream_texture_store_map) */
	XGPU_STREAM_TEXTURE_STORE_MAP,     /* synchronous */
	/* display.prism_models: the map's replacement meshes, an object's
	nodes, and the mesh a model part's draws take (xgpu_stream_mesh_*) */
	XGPU_STREAM_MESH_STORE_MAP,        /* synchronous */
	XGPU_STREAM_MESH_INSTANCE,
	XGPU_STREAM_MESH_DRAW,
	/* display.prism_materials: a material defined (xgpu_stream_material_
	define), and the material of the draws that follow (value: its id, 0
	none) */
	XGPU_STREAM_MATERIAL_DEFINE,
	XGPU_STREAM_MATERIAL_BIND,
	/* display.prism_rt: the structure's triangles for the acceleration
	structures, a slice at a time, and each frame's lights and settings
	(xgpu_stream_rt_static, xgpu_stream_rt_frame; docs/prism_rt.md) */
	XGPU_STREAM_RT_STATIC,
	XGPU_STREAM_RT_FRAME,
	/* display.lighting "ce_deferred" (PRISM v4 M1's "CE light, deferred"):
	the window's draws from now on to a phase's target, or the frame put
	together from them (xgpu_backend_gbuffer_phase; value:
	XGPU_GBUFFER_PHASE_*) */
	XGPU_STREAM_GBUFFER_PHASE,
	/* PRISM v4's scene records (below; docs/prism_scene_records.md): sent
	only in the v4 modes. M1 sends SCENE_MATERIAL (each shader of the map,
	once, before its first G-buffer pass) and GBUFFER_DRAW (the material and
	instance of the G-buffer pass's draws that follow); the host takes the
	others' sizes and nothing more yet */
	XGPU_STREAM_SCENE_MATERIAL,
	XGPU_STREAM_SCENE_MATERIAL_ANIMATE,
	XGPU_STREAM_SCENE_INSTANCE,
	XGPU_STREAM_GBUFFER_DRAW,
	XGPU_STREAM_SCENE_STATIC,
	XGPU_STREAM_SCENE_LIGHTS,
	XGPU_STREAM_SCENE_DECAL,
	XGPU_STREAM_SCENE_FOG,
	/* (the cameras the G-buffer's motion is found by: SCENE_VIEW, below) */
	XGPU_STREAM_SCENE_VIEW,
	/* (PRISM v4 M2: the in-game baker's control and inputs, PRISM_BAKE,
	below; docs/prism_bake.md) */
	XGPU_STREAM_PRISM_BAKE,
	/* (PRISM v4 M4: a material's maps where a pack or a mod gives its
	shader PRISM's material, SCENE_MATERIAL_MAPS, below;
	docs/prism_materials.md) */
	XGPU_STREAM_SCENE_MATERIAL_MAPS,
	/* (PRISM v4 M5: display.upscaler's and display.denoiser's frame,
	UPSCALE, below; docs/prism_plugin.md) */
	XGPU_STREAM_UPSCALE,
	/* (the terrain overhaul, T0: what of the ground, the weather and the
	water only the game knows, PRISM_TERRAIN, below; docs/prism_terrain.md) */
	XGPU_STREAM_PRISM_TERRAIN,
	XGPU_STREAM_RECORD_TYPES,
};
/* (the scene records' own count: they run from SCENE_MATERIAL to the end) */
#define XGPU_STREAM_SCENE_RECORD_FIRST XGPU_STREAM_SCENE_MATERIAL
#define XGPU_STREAM_SCENE_RECORD_TYPES XGPU_STREAM_RECORD_TYPES

/* GBUFFER_PHASE's value: the diffuse texture pass's base (an image that
starts white, with the window's colour target's alpha), the specular and
reflection passes' additions (one that starts black, with the base's
alpha), or the resolve (the colour target becomes light x base + additions,
the additions' alpha, and the draws go to it again); the resolve's view from
bit 8 (0 the frame; 1 the base, 2 the light, 3 the additions, alone:
debug.gbuffer_view) */
#define XGPU_GBUFFER_PHASE_BASE 1u
#define XGPU_GBUFFER_PHASE_ADDITIONS 2u
#define XGPU_GBUFFER_PHASE_RESOLVE 3u
#define XGPU_GBUFFER_PHASE_VIEW_SHIFT 8
/* (PRISM v4 M4) in the base phase, the diffuse texture pass drawn and the
decals next: the level's pixels of a pack's or a mod's material take its
maps' base colour now (the albedo target's, GBUFFER_BEGIN below), so that
the decals lie over it; the draws still go to the base */
#define XGPU_GBUFFER_PHASE_BASE_TEXTURED 6u
/* the G-buffer pass (walk 1: the window's opaque surfaces drawn first,
alone, as display.ambient_occlusion "world"'s pre-pass draws them, before
CE's passes): GBUFFER_BEGIN (the draws from here on are the G-buffer's:
each opaque one's G-buffer twin, xgpu_stream_gbuffer_twin, into the G-buffer
target of the colour target's size and a depth target of its own, every
other draw and clear skipped) and GBUFFER_END (the draws go to their targets
again). The G-buffer target is four 32-bit words a pixel
(XGPU_FORMAT_GBUFFER: RGBA32Uint):
  [0] the base colour, RGBA8 (unorm, the base map's colour space): an
      object's, its combiners with white diffuse light, no specular light
      and no self-illumination; on the level's surfaces, whose base colour
      is the base phase's target (CE's diffuse texture pass), the lightmap
      coordinates where the draw has a page (GBUFFER_DRAW's lightmap; the
      second target's word 2 says which page), 2x16 unorm, else 0;
  [1] the shading normal in the world, octahedral, 2x16 snorm (where word 2
      has XGPU_GBUFFER_FLAG_NORMAL; else none);
  [2] roughness, metalness, specular (F0's scale, 0.5 the dielectric 4%),
      the material model and flags (XGPU_GBUFFER_MODEL_*: 4 bits each),
      RGBA8 unorm;
  [3] the material (bits 0 to 11: SCENE_MATERIAL's id, 0 none), its source
      tier (12 to 14: XGPU_GBUFFER_TIER_*), present (15, every pixel the
      pass drew), the instance (16 to 31: 0 the level, 1 + an object's datum
      index, XGPU_GBUFFER_INSTANCE_FIRST_PERSON the first-person weapon).
Beside it, the G-buffer's second target, four 32-bit words a pixel too
(RGBA32Uint, written by the same twins):
  [0] the screen motion, 2x16 half floats: where the pixel's surface was
      the frame before, in the target's pixels, less where it is now (x
      right, y down): the camera's move (SCENE_VIEW's last and this frame's
      cameras) and the object's (SCENE_INSTANCE's node the draw's part hangs
      from, last frame's and this frame's), 0 where there is no history;
  [1] [2] the emissive light, 3 half floats (red and green, then blue in
      word 2's low 16 bits): an object's combiners with no light (its
      self-illumination), the level's lightmap pass's with no lightmap; CE's
      values; word 2's bit 16 set where the motion has history, its bits 17
      to 31 the level's lightmap page + 1 (0 none: an object's, or a draw
      without a page);
  [3] the distance along the view's axis (a float's bits).
Where a material of the map has maps (PRISM v4 M4: SCENE_MATERIAL_MAPS, a
pack's or a mod's), the pass has a third target, the albedo target (RGBA8
unorm): the base colour its family's uber-shader drew of the maps (the
base map's colour space), its alpha the maps' occlusion, at least 1/255; 0
where the base colour is not the maps' (every other twin writes nothing
there: the fourth word's tier says whose a pixel is). Once the diffuse
texture pass is drawn (XGPU_GBUFFER_PHASE_BASE_TEXTURED), before the
decals, the level's pixels of the pack or mod tier take its colour in the
base target's place (xgpu_vulkan_post_albedo_over; at the resolve, where
that phase did not come), so that the decals lie over it and the resolve,
its views and PRISM Raster read the maps' base colour as they read CE's.
The resolve's views (bit 8 on, debug.gbuffer_view) 4 to 11 show the G-buffer
in the frame's place: 4 the base colour (the level's from the base phase's
target), 5 the normal, 6 the roughness, 7 the metalness, 8 the material
source (each opaque pixel coloured by its tier: XGPU_GBUFFER_SOURCE_*; a
pixel the window drew opaque that the G-buffer has not, magenta), 9 the
instances, 10 the motion (red right, green down, 8 pixels full; blue where
there is no history), 11 the emissive light; with the baker's light (M2, display.lighting "raster":
XGPU_GBUFFER_PHASE_RASTER) 12 the baked light (the light the level and the
objects take from the bake, the sun's with it), 13 the sun's visibility (the
level's from the baked sun mask, an object's from the volume), 14 the bake's
texels (each baked texel a cell of a checker, red where no bake covers the
pixel), 15 the irradiance volume alone. With a view (1 to 15) nothing more is
drawn into the window's colour target after the resolve, so the view is what
the frame shows.

XGPU_GBUFFER_PHASE_RASTER, or'd into the resolve's value: display.lighting
"raster" (PRISM Raster, design section 1.8): before the resolve, the opaque
pixels are made whole from the bake where the bake has them (docs/
prism_bake.md): the diffuse light the level's from its baked lightmap and
the sun's light by the baked sun mask, an object's from the irradiance
volume and its traced sun; the material table's specular by the
reflection probes; the resolve draws that frame as it is. Where the bake
has nothing yet, CE's own frame (light x base + additions) stays. 17 to 19
show the frame's albedo, specular and diffuse parts. */
#define XGPU_GBUFFER_PHASE_GBUFFER_BEGIN 4u
#define XGPU_GBUFFER_PHASE_GBUFFER_END 5u
#define XGPU_GBUFFER_VIEW_ALBEDO 4u
#define XGPU_GBUFFER_VIEW_NORMAL 5u
#define XGPU_GBUFFER_VIEW_ROUGHNESS 6u
#define XGPU_GBUFFER_VIEW_METAL 7u
#define XGPU_GBUFFER_VIEW_MATERIAL_SOURCE 8u
#define XGPU_GBUFFER_VIEW_INSTANCE 9u
#define XGPU_GBUFFER_VIEW_MOTION 10u
#define XGPU_GBUFFER_VIEW_EMISSIVE 11u
#define XGPU_GBUFFER_VIEW_BAKE_LIGHT 12u
#define XGPU_GBUFFER_VIEW_BAKE_SUN 13u
#define XGPU_GBUFFER_VIEW_BAKE_TEXELS 14u
#define XGPU_GBUFFER_VIEW_BAKE_VOLUME 15u
/* (the bake's whole light: the light less the sun's and the sun's, at the
pixel's normal, in CE's encoding: the light's units' calibration) */
#define XGPU_GBUFFER_VIEW_BAKE_TOTAL 16u
/* (PRISM Raster's frame's parts: its albedo, its specular light, its
diffuse light by the albedo) */
#define XGPU_GBUFFER_VIEW_RASTER_ALBEDO 17u
#define XGPU_GBUFFER_VIEW_RASTER_SPECULAR 18u
#define XGPU_GBUFFER_VIEW_RASTER_DIFFUSE 19u
#define XGPU_GBUFFER_PHASE_RASTER 0x10000u
/* (with it: display.prism_raster_composition "materials", Raster's own
materials compose the frame whole; else CE's compose Raster's light: it
goes into the light target, CE's base and additions with it) */
#define XGPU_GBUFFER_PHASE_RASTER_MATERIALS 0x20000u
/* (with it: display.prism_raster_objects "volume", an object's light the
irradiance volume's alone; else, with CE's composition, never under the
light CE gives the object's pixel) */
#define XGPU_GBUFFER_PHASE_RASTER_OBJECTS_VOLUME 0x40000u
#define XGPU_GBUFFER_PHASE_VIEW_MASK 0xff00u
/* XGPU_GBUFFER_PHASE_PATHTRACED, or'd into the resolve's value in its
place (M3, stream version 35): display.lighting "pathtraced" (PRISM Path
Traced, design section 1.5; docs/prism_pathtraced.md): every CE light off,
the opaque pixels lit by the path tracer (xgpu_vulkan_pt.c) from the G-buffer
and the bake, and the resolve draws that frame as it is; its profile
(display.pt_profile: XGPU_PT_PROFILE_*) in bits 29 and 30, bit 31 the
denoiser off (display.denoiser "off"); its views from 20 (debug.gbuffer_view
"pt_*": XGPU_GBUFFER_VIEW_PT_*). Its bits are taken from the word's top
down (PATHTRACED bit 28), Raster's (prism-v4-m2) from bit 16 up, so that
the two never meet */
#define XGPU_GBUFFER_PHASE_PATHTRACED 0x10000000u
#define XGPU_GBUFFER_PHASE_PT_PROFILE_SHIFT 29
#define XGPU_GBUFFER_PHASE_PT_PROFILE_MASK 0x60000000u
#define XGPU_GBUFFER_PHASE_PT_DENOISER_OFF 0x80000000u
/* (the traced light's upscaling, display.pt_light_scale: the share of the
G-buffer's pixels each way the light is traced and denoised at, in
hundredths, bits 20 to 26; 0 the G-buffer's own, as before. Clear of
Raster's bits, which PT's phase never has) */
#define XGPU_GBUFFER_PHASE_PT_LIGHT_SCALE_SHIFT 20
#define XGPU_GBUFFER_PHASE_PT_LIGHT_SCALE_MASK 0x07f00000u
#define XGPU_PT_PROFILE_PERFORMANCE 0u
#define XGPU_PT_PROFILE_QUALITY 1u
/* (the path tracer's views: the diffuse light (denoised, the albedo left
out), the specular light, the sun's visibility (filtered), the direct
light's and the indirect light's parts of the diffuse, the history's length,
the noisy diffuse light as traced) */
#define XGPU_GBUFFER_VIEW_PT_DIFFUSE 20u
#define XGPU_GBUFFER_VIEW_PT_SPECULAR 21u
#define XGPU_GBUFFER_VIEW_PT_SUN 22u
#define XGPU_GBUFFER_VIEW_PT_DIRECT 23u
#define XGPU_GBUFFER_VIEW_PT_INDIRECT 24u
#define XGPU_GBUFFER_VIEW_PT_HISTORY 25u
#define XGPU_GBUFFER_VIEW_PT_NOISY 26u
#define XGPU_GBUFFER_VIEW_PT_LAST 26u
/* the second target's word 2: the motion's history bit and the page's
shift (GBUFFER_BEGIN) */
#define XGPU_GBUFFER_EXTRA_HISTORY 0x10000u
#define XGPU_GBUFFER_EXTRA_PAGE_SHIFT 17
/* the source tiers (design section 5.3): none (no material: a fault), the
class default, the automatic tier (the host's, from CE's shader at load),
the pack tier, a mod's */
#define XGPU_GBUFFER_TIER_NONE 0u
#define XGPU_GBUFFER_TIER_CLASS 1u
#define XGPU_GBUFFER_TIER_AUTO 2u
#define XGPU_GBUFFER_TIER_PACK 3u
#define XGPU_GBUFFER_TIER_MOD 4u
#define XGPU_GBUFFER_PRESENT 0x8000u
#define XGPU_GBUFFER_INSTANCE_FIRST_PERSON 0xffffu
/* the material models (design section 1.3), word 2's fourth byte's low 4
bits; its high 4 bits the flags */
#define XGPU_GBUFFER_MODEL_OPAQUE 0u
#define XGPU_GBUFFER_MODEL_FOLIAGE 1u
#define XGPU_GBUFFER_MODEL_CLEARCOAT 2u
#define XGPU_GBUFFER_MODEL_SUBSURFACE 3u
#define XGPU_GBUFFER_MODEL_EMISSIVE 4u
/* (the terrain overhaul's ground (T2, T3): written only by its HQ layers'
pass (xgpu_vulkan_terrain.c) on the level's classified pixels, so no
material's own model is ever one of them; on these word 2's third byte is
the class's specular scale times the layer's occlusion, which the light
passes read back as the occlusion (docs/prism_terrain.md, T2): granular
(sand: the back-scattering rough-diffuse lobe and glints), soil (dirt and
mud: the lobe, weaker, no glints), snow (the wrapped, blue-weighted
subsurface light, its screen-space diffusion in Path Traced, sparkle) */
#define XGPU_GBUFFER_MODEL_GRANULAR 5u
#define XGPU_GBUFFER_MODEL_SOIL 6u
#define XGPU_GBUFFER_MODEL_SNOW 7u
#define XGPU_GBUFFER_FLAG_LEVEL 1u
#define XGPU_GBUFFER_FLAG_ALPHA_TESTED 2u
#define XGPU_GBUFFER_FLAG_FIRST_PERSON 4u
/* (word 1 holds a normal: the vertices gave one) */
#define XGPU_GBUFFER_FLAG_NORMAL 8u
/* (GBUFFER_DRAW's alone: the object is static scenery the bake holds, its
sun shadows baked; PRISM Raster's traced objects leave it out) */
#define XGPU_GBUFFER_FLAG_STATIC_SCENERY 16u
/* (GBUFFER_DRAW's alone: the player's own body in first person, traced as a
shadow caster (the traced objects' structure, instance mask 16: the rays of
the first-person view's pixels leave it out) but not drawn into the
G-buffer, so the camera never sees it: port/linux/game/prism_deferred.c's
prism_deferred_body) */
#define XGPU_GBUFFER_FLAG_TRACE_ONLY 32u

/* a record of no more than its header: SKIP, INVALIDATE, FLUSH,
VISIBILITY_BEGIN, VISIBILITY_FRAME, DYNAMIC_LIGHTS_PREPARE,
VR_GROUPS_RESET, PREPARE */
struct xgpu_stream_record
{
	uint32_t header;
};

/* FRAME_END: when the game read the frame's input, nanoseconds of its
monotonic clock (0 unknown; a recording's older FRAME_END is its header
alone), for the time from it to the frame on screen (frame_pacing.h) */
struct xgpu_stream_frame_end
{
	uint32_t header;
	uint32_t input_low, input_high;
};

/* a record of one value: ANTI_ALIASING_PREPARE (smaa), TARGET_DELETE,
TARGET_RESOLVE, PRESENT (the back buffer's target), PICTURE_POSITION, GAMMA, SHARP_UPSCALE, HDR_SDR_CURVE, HDR_PEAK_NITS, HDR_GAMUT, PASS_TIMES, VR_PASS, VR_TIMING
(mark), PIPELINE (the id, XGPU_STREAM_PIPELINE_DEFINE's), VR_SKY (sky) */
struct xgpu_stream_value
{
	uint32_t header;
	uint32_t value;
};

/* CATEGORY's value: the category (rasterizer.h's _rasterizer_profile_*,
XGPU_STREAM_CATEGORY_NONE outside them) */
#define XGPU_STREAM_CATEGORY_NONE 255u

/* ---------- debug.render_marks (port/linux/game/render_marks.c)

Written only with the setting on. A string a mark names is a number: the
first mark that names it is preceded by MARK_STRING, its number and its
bytes (inline, NUL-terminated); 0 names none. Numbers count from 1 since
the stream began and are never given again. A VIEW mark says where the
camera of the window drawn from now on is; an OWNER mark says what the
draws from now on, until the next OWNER mark, belong to. */

/* an owner's kind (the replay's names: halo_replay.m's mark_kind_names) */
enum
{
	XGPU_STREAM_MARK_OTHER,
	XGPU_STREAM_MARK_WORLD_OBJECT,
	XGPU_STREAM_MARK_FIRST_PERSON,
	XGPU_STREAM_MARK_BSP,
	XGPU_STREAM_MARK_SKY,
	XGPU_STREAM_MARK_DECAL,
	XGPU_STREAM_MARK_EFFECT,
	XGPU_STREAM_MARK_PARTICLE,
	XGPU_STREAM_MARK_CONTRAIL,
	XGPU_STREAM_MARK_LIGHTNING,
	XGPU_STREAM_MARK_WEATHER,
	XGPU_STREAM_MARK_WATER,
	XGPU_STREAM_MARK_HUD,
	XGPU_STREAM_MARK_TEXT,
	XGPU_STREAM_MARK_MENU_WIDGET,
	XGPU_STREAM_MARK_CINEMATIC_LETTERBOX,
	XGPU_STREAM_MARK_DEBUG,
	XGPU_STREAM_MARK_DETAIL_OBJECT,
	XGPU_STREAM_MARK_LENS_FLARE,
	XGPU_STREAM_MARK_FOG,
	XGPU_STREAM_MARK_SCREEN_EFFECT,
	XGPU_STREAM_MARK_LIGHT_VOLUME,
	XGPU_STREAM_MARK_SHADOW,
	XGPU_STREAM_MARK_CLEAR,
	XGPU_STREAM_MARK_KINDS,
};

#define XGPU_STREAM_MARK_NONE 0xffffffffu

/* MARK_STRING: then length bytes and a NUL (inline: at data) */
struct xgpu_stream_mark_string
{
	uint32_t header;
	uint32_t number, length, data;
};

/* MARK_VIEW: the map, the game's tick, the window (XGPU_STREAM_MARK_NONE:
the frame's start, no camera) and its camera: position, forward and up
(the world's units), the horizontal field of view (radians) and the
viewport in the back buffer's pixels (x, y, width, height) */
struct xgpu_stream_mark_view
{
	uint32_t header;
	uint32_t map, tick, window;
	float position[3], forward[3], up[3];
	float field_of_view;
	int32_t viewport[4];
};

/* MARK_OWNER: the kind (XGPU_STREAM_MARK_*), an id (an object's or a
widget's handle, a tag's index; XGPU_STREAM_MARK_NONE), and strings: a
label, the tags that matter (the object's definition, its model, the
shader, a bitmap), a menu widget's tag and the generator source that wrote
it (tools/menu_sources.py's) */
struct xgpu_stream_mark_owner
{
	uint32_t header;
	uint32_t kind, id;
	uint32_t label;
	uint32_t object, model, shader, bitmap;
	uint32_t widget, source;
};

/* ---------- the backend's life */

/* INITIALIZE's answer: xgpu_backend_capabilities, and the backend's own
(the enhanced dynamic lights can be made: xgpu_backend_dynamic_lights_prepare) */
struct xgpu_stream_capabilities
{
	int32_t largest_target;
	int32_t most_samples;
	int32_t most_anisotropy;
	uint8_t sample_counting, bgra_vertex_colors, base_vertex, sample_mask;
	uint8_t mixed_constant_blend, dynamic_lights, dynamic_lights_preparable, hdr;
	/* display.ambient_occlusion "ray_traced": the backend traces the
	occlusion's rays by the GPU's ray queries (xgpu_backend_world_ao_mesh,
	_instances: Vulkan's VK_KHR_ray_query) */
	/* (and xgpu_backend_capabilities' clear_alone and draw_bounds, and in
	cutout_prepass its alpha_test_drop, 1, and cutout_prepass, 2) */
	uint8_t ray_queries, cutout_prepass, clear_alone, draw_bounds;
	/* display.output_depth: the SDR back buffer can be RGBA16
	(xgpu_backend.h's XGPU_FORMAT_RGBA16) */
	uint8_t deep_sdr;
	/* display.soft_particles: an effect drawn with no views can be soft
	(the pixel key's vr_effect: xgpu_backend_capabilities' soft_effects) */
	uint8_t soft_effects, pad[2];
	/* the GPU, its API and driver, in a few words (a report's:
	port/linux/src/report.c), NUL terminated */
	char device[96];
	/* display.prism_material_overlay drawn (MATERIAL_OVERLAY); the
	lightmaps smoothed (display.prism_lightmaps: the pixel key's
	lightmap_filter); display.prism_bloom drawn (BLOOM); display.prism_shadows
	"cascaded" drawn (PRISM_SHADOWS) */
	uint8_t material_overlay, lightmap_filter, bloom, prism_shadows;
	/* PRISM's texture store (TEXTURE_STORE_MAP, SET_TEXTURES' store ids):
	its files opened and drawn by the host (0: the guest never asks);
	PRISM's mesh store (MESH_STORE_MAP, MESH_INSTANCE, MESH_DRAW:
	display.prism_models); PRISM's materials (display.prism_materials:
	MATERIAL_DEFINE, MATERIAL_BIND, the pixel key's material_pbr, the
	store's material files; 0: never sent); display.prism_rt: hardware ray
	queries and acceleration structures (RT_STATIC, RT_FRAME; 0: never
	sent, the menu's row greyed out) */
	uint8_t texture_store, mesh_store, materials, ray_tracing;
};

struct xgpu_stream_reply_record
{
	uint32_t header;
	/* where the answer goes (guest address) */
	uint32_t reply;
};

struct xgpu_stream_anti_alias
{
	uint32_t header;
	uint32_t target;
	uint32_t smaa;
	int32_t corners[4];
};

/* PANINI: a window's 3D view drawn again through the Panini projection
(xgpu_backend_panini): its target, corners, and the projection (d, s, the
magnification at the middle, 0, and the rectilinear view's tangents at the
corners' left, right, top and bottom) */
struct xgpu_stream_panini
{
	uint32_t header;
	uint32_t target;
	int32_t corners[4];
	float projection[8];
};

/* HDR: display.hdr's state (xgpu_backend_hdr): drawn in HDR, the mode
(XGPU_HDR_MODE_*), paper white (in nits; 0: the system's SDR white, in
percent of it: xgpu_hdr_paper_white_nits), the UI's white (in nits; 0:
paper white's) */
struct xgpu_stream_hdr
{
	uint32_t header;
	uint32_t active, mode, paper_white_nits, paper_white, ui_nits;
};

/* TONE_MAP: a window's 3D view tone mapped (xgpu_backend_tone_map) */
struct xgpu_stream_tone_map
{
	uint32_t header;
	uint32_t target;
	int32_t corners[4];
	/* display.prism_bloom first (XGPU_BLOOM_*: xgpu_backend.h; 0 none) in
	the low 8 bits, display.prism_tone's grade (XGPU_PRISM_TONE_*) from
	XGPU_PRISM_TONE_SHIFT */
	uint32_t bloom;
};

/* ---------- display.ambient_occlusion "world" (docs/world-ao.md, #578)

As the GPU sees it, alike in the guest's front end and the hosts' backends.

- The field: one 3D texture of the current structure BSP's signed distance
  (world_sdf.h), R8 signed normalized (VK_FORMAT_R8_SNORM,
  MTLPixelFormatR8Snorm), linear filtering, clamped to its edge; a texel's
  value times the block's range is the distance in world units at the
  voxel's centre.
- The block: the field's place and the window's proxies (world_proxies.h's
  struct world_proxy), a uniform buffer (std140: vec4s only).

Bindings (the shaders: workstream D; the backends' descriptors: B):
- SPIR-V: XGPU_SPIRV_SET_OCCLUSION (a set of its own, so that a draw
  still binds at most four sets), bindings XGPU_SPIRV_BINDING_WORLD_SDF and
  XGPU_SPIRV_BINDING_WORLD_AO (xgpu_spirv.h);
- MSL: [[texture(XGPU_MSL_TEXTURE_WORLD_SDF)]] with a constexpr sampler in
  the shader, [[buffer(XGPU_MSL_BUFFER_WORLD_AO)]] (xgpu_msl.h).
*/

#define XGPU_WORLD_AO_PROXIES_MAXIMUM 64
/* a proxy's kind (its [0].w) */
#define XGPU_WORLD_AO_PROXY_SPHERE 1
#define XGPU_WORLD_AO_PROXY_CAPSULE 2
#define XGPU_WORLD_AO_PROXY_BOX 3
/* (a vehicle's occlusion field, over this proxy and the next two: [0]
the reach's centre, kind; [1] row 0 of the world to its voxels' place,
owner; [2] the three rows' translations, its reach (world units); then [0]
row 1, 0; [1] row 2, owner; [2] its last voxel's place (its sides less
one), 0; then [0] its place in the objects' atlas plus a half texel, 0;
[1] one over the atlas's size, owner; [2] its geometry's box's half
extents (voxels, about the grid's middle), 0: the box's bound
(world_proxy_box_fade's U) bounds the field's occlusion) */
#define XGPU_WORLD_AO_PROXY_FIELD 4

/* the uniform block: [0] the field's origin (its first voxel's centre) and
one over the voxel's size; [1] its width, height, depth (in voxels) and
range (world units); [2] the strength on objects, the strength on the level,
the field's fade (0 none yet: the proxies alone), the proxies' count; [3]
the reference's weight (how much of the occlusion at the object's lighting
point its ambient sample already holds) and three spare; then the
proxies, three vec4s each (world_proxies.h's struct world_proxy); then
their grid about the camera (world_ao.h's world_ao_grid): [0] the camera,
[1] each axis's scale and one over the cell, and the cells' 64-bit masks of
the proxies that can reach them, two cells a uvec4 */
#define XGPU_WORLD_AO_GRID_CELLS_MAXIMUM (16 * 16 * 8)
struct xgpu_world_ao_block
{
	float field[2][4];
	float parameters[2][4];
	float proxies[XGPU_WORLD_AO_PROXIES_MAXIMUM][3][4];
	float grid[2][4];
	uint32_t cells[XGPU_WORLD_AO_GRID_CELLS_MAXIMUM / 2][4];
};

/* WORLD_AO_WINDOW: display.ambient_occlusion "world"'s block for the
window's opaque surfaces from here on (xgpu_backend_world_ao_window):
active 0 none (the block's contents unused) */
struct xgpu_stream_world_ao_window
{
	uint32_t header;
	uint32_t active;
	uint32_t pad[2];
	struct xgpu_world_ao_block block;
};

/* ---------- display.ambient_occlusion "world" found once a pixel

The window's opaque surfaces are drawn twice (source/render/render.c): first
their inputs to the occlusion alone (the pre-pass: each draw with the
world's light, XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED, drawn by the backend
with its twin, the same pipeline but for its pixel key's
XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS and its target: xgpu_world_ao_twin),
into a target of the inputs (XGPU_FORMAT_WORLD_AO_INPUTS: four 32-bit
words a pixel, xgpu.h's) and a depth target of its own; then the
occlusion found once from them, at each 2x2 block's nearest pixel (a
quarter of the pixels: the pass's camera[3].w 2; 1 each pixel's), into a
texture of four floats a texel (the occlusion, the pixel's distance from
the eye, its normal octahedral); then the surfaces drawn, each reading its
pixel's (XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED), upsampled by distance and
normal. Nothing else is drawn in the pre-pass: the draws without the
world's light are skipped, its clears too.

WORLD_AO_PREPASS: phase XGPU_WORLD_AO_PREPASS_BEGIN (the draws from here
on are the pre-pass's, into targets of target's size), _END (the pre-pass
done: the occlusion found over the window's corners (left, top, right,
bottom, in target's pixels) as the camera saw it (struct
xgpu_world_ao_pass)), or _NONE (the occlusion found no more: the surfaces
read 1 from here on). While a map is prepared (the others then ignored):
_TWINS (the draws from here on would be drawn in the pre-pass in play:
each one's twin made with it, xgpu_stream_world_ao_twin) and _NONE (no
more) */
enum
{
	XGPU_WORLD_AO_PREPASS_NONE,
	XGPU_WORLD_AO_PREPASS_BEGIN,
	XGPU_WORLD_AO_PREPASS_END,
	XGPU_WORLD_AO_PREPASS_TWINS,
};
/* the camera, as the occlusion's pass finds a pixel's world position
(world_ao.h's world_ao_pass_position): [0] its position and its near
plane; [1] the direction (one unit along the view's axis) at the screen's
middle, and its far plane; [2] and [3] the direction's change a unit of
the screen's x (-1 its left edge, 1 its right) and y (-1 its top, 1 its
bottom), [2].w 0, [3].w the pixels a side of the blocks the occlusion is
found once for (1 or 2); the window's corners in its target's pixels
(left, top, right, bottom). A pixel's position: the eye (the block's
grid[0]) + its distance (from the pre-pass) along normalize([1] + x [2] +
y [3]). Then the distance-field scene's cascades, which the dynamic
objects' rays step through (world_cascades.h: each the first voxel's
centre and the voxel, 0 not ready); the window's previous frame's camera
and corners as these, where its occlusion is reprojected from; and history:
[0] 1 where the previous frame's occlusion is this window's (one window a
frame, its corners alike: the backend's previous texture of it), else 0;
[1] the frame's number (the rays' turn); [2] the share of the new frame's
occlusion blended into what is reprojected (1: none of the history); [3] 1
where the dynamic objects' occlusion is traced, 0 where their shapes are
walked instead (HALO_WORLD_AO_NO_TRACE, for measuring against), 2 where
it is traced by the GPU's ray queries ("ray_traced": WORLD_AO_MESH, below;
a backend without them, or before its scene is built, traces it as 1) */
struct xgpu_world_ao_pass
{
	float camera[4][4];
	int32_t corners[4];
	float cascades[4][4];
	float previous[4][4];
	int32_t previous_corners[4];
	float history[4];
};
struct xgpu_stream_world_ao_prepass
{
	uint32_t header;
	uint32_t phase;
	uint32_t target;
	uint32_t pad;
	struct xgpu_world_ao_pass pass;
};

/* ---------- display.prism_shadows "cascaded" (xgpu.h's "PRISM's sun shadows")

PRISM_SHADOWS: phase XGPU_PRISM_SHADOWS_BEGIN (the draws from here on are
the casters' that move: each opaque one's shadow twin,
xgpu_stream_prism_shadow_twin, drawn once a cascade into the moving
casters' atlas, the others and the clears skipped), _STRUCTURE (the draws
from here on are the structure's: into the structure's atlas), _END (the
pass done: the block the receivers read from here on, until the next pass
or _NONE), _NONE (no shadow: the receivers read a sun that is nowhere).
The atlases are resolution * cascades by resolution texels of depth, cascade
k's tile the k-th; a caster's twin reaches its tile's clip space by
cascades[k] times the world position its vertex program's clip position
is of (the backend undoes the camera's projection, the vertex constants'
first four rows, for each draw), its depth reversed (1 nearest the sun). */
enum
{
	XGPU_PRISM_SHADOWS_NONE,
	XGPU_PRISM_SHADOWS_BEGIN,
	XGPU_PRISM_SHADOWS_STRUCTURE,
	XGPU_PRISM_SHADOWS_END,
};
#define XGPU_PRISM_SHADOWS_CASCADES_MOST 4
#define XGPU_PRISM_SHADOWS_RESOLUTION_LEAST 512
#define XGPU_PRISM_SHADOWS_RESOLUTION_MOST 4096
/* the receivers' block (std140, a uniform buffer): cascades[k] the world
to cascade k's clip (rows x, y, z, w: x and y -1 to 1 across its tile, z
reversed); sun: xyz the unit direction toward the sun, w the cascades' count
(0: no shadow); light: x the share of a lightmap's light the moving casters
take away, y the penumbra's growth a world unit from blocker to receiver,
z the atlas's width's reciprocal, w the cascades' tile's texels; texel: each
cascade's texel in world units; depth: each cascade's depth range in world
units; fade: x where the last cascade's shadow starts to fade (the share of
its radius), y the structure's share for a model (1: as the moving
casters'), z a model's share of its first distant light the casters take, w
unused; tune: the bake's sun visibility (a lightmap texel's dominant
incident direction's nearness to the sun from x to y, cosines, times its
directional share times z, at most 1: the share of its light the casters
may take) and w the cascades' blend's band (the share of a cascade's half
width short of its edge where its pixels start to take the next) */
struct xgpu_prism_shadows_block
{
	float cascades[XGPU_PRISM_SHADOWS_CASCADES_MOST][4][4];
	float sun[4];
	float light[4];
	float texel[4];
	float depth[4];
	float fade[4];
	float tune[4];
	float spare[2][4];
};
struct xgpu_stream_prism_shadows
{
	uint32_t header;
	uint32_t phase;
	uint32_t resolution;
	uint32_t cascades;
	struct xgpu_prism_shadows_block block;
};

/* PIPELINE_STAND_IN: pipeline id (one with the world's light) has a stand-in
(an id, 0 none), the pipeline the draw would have without the world: the
backend may draw with it until id's is made, and never makes id's while a
map is prepared (xgpu_backend_pipeline_stand_in) */
struct xgpu_stream_pipeline_stand_in
{
	uint32_t header;
	uint32_t id, stand_in;
};

/* WORLD_AO_FIELD: slices z_first to z_first + z_count - 1 of a part of
the field (a width * height * depth volume, x fastest:
xgpu_backend_world_ao_field): its table of cells (part 0, four bytes a
texel: an atlas brick's x, y, z and the cell's kind, read as RGBA8) or its
atlas of bricks (part 1, signed bytes: R8 snorm), or the static
scenery's pages of the level's occlusion (part 2, bytes, R8 unorm: a 2D
array, depth its layers, sent before the table), or the vehicles'
occlusion fields' atlas (part 3, four half floats a texel: RGBA16F, a 3D
volume, sent before the table), its bytes at data (size:
width * height * z_count * the texel's); width 0 releases the field, every
part */
#define XGPU_STREAM_WORLD_AO_TABLE 0
#define XGPU_STREAM_WORLD_AO_ATLAS 1
#define XGPU_STREAM_WORLD_AO_SCENERY 2
#define XGPU_STREAM_WORLD_AO_OBJECTS 3
#define XGPU_STREAM_WORLD_AO_TEXEL(part) ((part) == XGPU_STREAM_WORLD_AO_TABLE ? 4u : \
	(part) == XGPU_STREAM_WORLD_AO_OBJECTS ? 8u : 1u)
struct xgpu_stream_world_ao_field
{
	uint32_t header;
	uint32_t part;
	uint32_t width, height, depth;
	uint32_t z_first, z_count;
	uint32_t data, size;
	uint32_t pad;
};

/* WORLD_AO_CASCADES: a box of the distance-field scene's cascades'
texture (xgpu.h's XGPU_WORLD_CASCADE*: XGPU_WORLD_CASCADE_SPAN texels
square, XGPU_WORLD_CASCADE_SPAN * XGPU_WORLD_CASCADES deep, two signed bytes
a texel, RG8 snorm; xgpu_backend_world_ao_cascades): its first texel x, y,
z and its size, its bytes at data (size: width * height * depth * 2, x
fastest); width 0 releases the texture */
#define XGPU_STREAM_WORLD_AO_CASCADES_SIDE 65u
#define XGPU_STREAM_WORLD_AO_CASCADES_DEPTH (65u * 4u)
struct xgpu_stream_world_ao_cascades
{
	uint32_t header;
	uint32_t x, y, z;
	uint32_t width, height, depth;
	uint32_t data, size;
	uint32_t pad;
};

/* ---------- render targets */

struct xgpu_stream_target_create
{
	uint32_t header;
	uint32_t target;
	uint32_t width, height;
	/* (format: XGPU_FORMAT_* of a colour target: BGRA8, RGBA16F the back
	buffer with display.hdr, or RGBA16 with display.output_depth) */
	uint8_t depth, layers, format, pad;
};

struct xgpu_stream_target_multisample
{
	uint32_t header;
	uint32_t target;
	int32_t samples;
};

struct xgpu_stream_set_targets
{
	uint32_t header;
	uint32_t color, depth;
	/* vr.h's HALO_VR_HUD_GROUP_* or -1; the layers drawn at once (1, 2, 4,
	or -1, -2 one layer alone) from base; their multisampled attachments' samples */
	int8_t hud_group, views;
	uint8_t base, samples;
};

/* the rectangles (left, top, width, height) follow, count of them */
struct xgpu_stream_clear
{
	uint32_t header;
	uint32_t flags;
	float color[4];
	float depth;
	uint32_t stencil;
	uint32_t count;
	int32_t rectangles[][4];
};

struct xgpu_stream_clear_target
{
	uint32_t header;
	uint32_t target;
	float color[4];
};

/* synchronous: the host writes the target's (or its layer's, -1 none)
pixels as BGRA rows from its first into pixels (guest memory of
width * height * 4 bytes, as the target was made), then the reply.
display.hdr's RGBA16F back buffer is clamped to [0, 1], as an SDR display
shows it, or with tone_map, tone mapped into it (xgpu_hdr_sdr_pixel) */
struct xgpu_stream_read_target
{
	uint32_t header;
	uint32_t target;
	int32_t layer;
	uint32_t tone_map;
	uint32_t pixels, size;
	/* (guest address of a uint32_t: 1 written, 0 failed) */
	uint32_t reply;
};

/* synchronous: the screenshot key's picture. The host reads the target as
READ_TARGET does, tone mapped, into pixels (guest memory of width * height
* 4 bytes), and writes it as a PNG file in the folder the record ends with
(a UTF-8 path and its NUL, made if it is missing), named for the local date
and time; then the file's name (without its folder, NUL-terminated) into
name (XGPU_STREAM_SCREENSHOT_NAME bytes of guest memory), and the reply (a
uint32_t: 1 written, 0 failed) */
#define XGPU_STREAM_SCREENSHOT_NAME 64u
struct xgpu_stream_screenshot
{
	uint32_t header;
	uint32_t target;
	uint32_t width, height;
	uint32_t pixels, size;
	uint32_t name;
	uint32_t reply;
	char directory[];
};

/* ---------- textures */

/* xgpu_backend.h's struct xgpu_sampler, which is these fields */
struct xgpu_stream_sampler
{
	uint8_t minify, mip, magnify;
	uint8_t address[3];
	uint8_t pad[2];
	float lod_bias;
	uint32_t first_level;
	uint32_t anisotropy;
	uint32_t border_color;
};

#define XGPU_STREAM_MIP_LEVELS 16

/* a stage's xgpu_texture_source; resource_format and resource_size are
the texture header's Format and Size words as they were when the call was
made (the header is at resource). For an Xbox texture (XGPU_SOURCE_TEXTURE)
target is the channels its texels are sampled from: 0 as they are, else
0x100 and two bits for each of red, green, blue and alpha (red's lowest),
the channel of the texels it is taken from (a Custom Edition map's model
masks and meters: xbox_textures.c).

A stage may instead draw an image (XGPU_STREAM_SOURCE_IMAGE: a high-res
replacement for the Xbox texture, xgpu_images.h): resource is its number,
resource_format its pixels' kind (XGPU_IMAGE_*: RGBA, 4 bytes each),
resource_size a serial that
changes with its pixels (no two images have the same), data its first
level's pixels, width by height, rows from the first; the host makes its
levels (levels of them) from the first. */
#define XGPU_STREAM_SOURCE_IMAGE 3

struct xgpu_stream_texture_stage
{
	uint8_t source, sampler_type, sampled;
	/* the level_targets and level_written that follow it in the record
	(rendered_levels of them, at most XGPU_STREAM_MIP_LEVELS) */
	uint8_t level_count;
	uint32_t resource, palette;
	uint32_t resource_format, resource_size;
	uint32_t target, drawn;
	uint32_t data, width, height, levels, rendered_levels;
	struct xgpu_stream_sampler sampler;
};

struct xgpu_stream_texture_level
{
	uint32_t target, written;
};

/* the stages whose bits are set in changed follow, in order, each an
xgpu_stream_texture_stage and its level_count levels; the others are as
they were. With XGPU_STREAM_TEXTURES_STORE in changed, four uint32_t
follow them: each stage's texture store id from now on (0 none; the
store's TEXTURE_STORE_MAP), sent only when they change, which they never
do without a store (display.prism_textures stock: the record is as it was
before the store). No other bit may be set. */
#define XGPU_STREAM_TEXTURES_STAGES 0x0fu
#define XGPU_STREAM_TEXTURES_STORE 0x100u
struct xgpu_stream_set_textures
{
	uint32_t header;
	uint32_t changed;
};

/* ---------- PRISM's texture store (display.prism_textures; the host's
port/linux/vulkan/xgpu_texture_store.h)

TEXTURE_STORE_MAP: the map's replacement textures, as the guest found them
in the mods' textures folders (port/linux/game/mod_textures.c), sent at a
map's load, as the mods are found again and as the setting changes. The
host checks each file's header against the stock bitmap it replaces and
answers each query (query_count of them, at queries, their paths' bytes at
strings) with an id, into reply (query_count answers). generation is new
at each record; an id carries its low 12 bits (XGPU_TEXTURE_STORE_ID), and
a stage naming an id of an older generation is drawn stock. query_count 0
closes the store (its images let go of). The queries, paths and answers are
in a buffer of the guest's own, freed once the record is answered. */
struct xgpu_stream_texture_store_map
{
	uint32_t header, reply;
	uint32_t generation;
	/* 1 enhanced, 2 max (for the host's log: the guest chose the files) */
	uint32_t tier;
	/* display.prism_texture_budget_mb (0: auto) */
	uint32_t budget_megabytes;
	uint32_t query_count, queries;
	uint32_t strings, strings_size;
	/* XGPU_STREAM_TEXTURE_STORE_LOADING: a map is loading (the uploads'
	larger rate, until the first frame drawn after it) */
	uint32_t flags;
};
#define XGPU_STREAM_TEXTURE_STORE_LOADING 1u
/* display.prism_prefetch: the map's files read as it loads, within the
budget, not each when first drawn */
#define XGPU_STREAM_TEXTURE_STORE_PREFETCH 2u
/* display.prism_texture_fade: the milliseconds a level takes to sharpen to
the next as it comes in (bits 16 to 31; 0 at once) */
#define XGPU_STREAM_TEXTURE_STORE_FADE_SHIFT 16
/* a query: its file's path (offset into strings and length, NUL-terminated
there; a host path, as the guest opens it), the size and modification
time the guest found (a file changed since is refused), and the stock
bitmap: its kind (XGPU_STREAM_TEXTURE_STORE_KIND_*), type (0 2D, 1 cube),
levels and first level's size. A material's image (KIND_MATERIAL:
display.prism_materials) replaces no bitmap: its stock fields are 0, its
side may be up to XGPU_STREAM_TEXTURE_STORE_MATERIAL_SIDE, it is 2D, and it
is usable once its levels from 256 texels down are resident */
#define XGPU_STREAM_TEXTURE_STORE_KIND_BITMAP 0
#define XGPU_STREAM_TEXTURE_STORE_KIND_MATERIAL 1
#define XGPU_STREAM_TEXTURE_STORE_MATERIAL_SIDE 8192u
struct xgpu_stream_texture_store_query
{
	uint32_t path, path_length;
	uint32_t second_path, second_path_length;
	uint32_t file_size, stamp_seconds;
	uint8_t kind, stock_type, stock_levels, unused;
	uint16_t stock_width, stock_height;
};
/* an answer: the id (0 refused), the file's size and levels, and why it was
refused (XGPU_STREAM_TEXTURE_STORE_*), its format (the store's
XGPU_KTX2_FORMAT_*) */
enum
{
	XGPU_STREAM_TEXTURE_STORE_ACCEPTED,
	XGPU_STREAM_TEXTURE_STORE_UNREADABLE,
	XGPU_STREAM_TEXTURE_STORE_CHANGED,
	XGPU_STREAM_TEXTURE_STORE_FORMAT,
	XGPU_STREAM_TEXTURE_STORE_SIZE,
	XGPU_STREAM_TEXTURE_STORE_DEVICE,
	XGPU_STREAM_TEXTURE_STORE_PATH,
};
struct xgpu_stream_texture_store_answer
{
	uint32_t id;
	uint16_t width, height;
	uint8_t levels, status, format, unused;
};
/* the most queries a record has */
#define XGPU_STREAM_TEXTURE_STORE_QUERIES 65536u

/* ---------- PRISM's mesh store (display.prism_models; the host's
port/linux/vulkan/xgpu_mesh_store.h, docs/mods.md "Meshes")

MESH_STORE_MAP: the map's replacement meshes, as the guest found them in
the mods' meshes folders (port/linux/game/mod_meshes.c): a .pmesh file for a
model tag, which the host reads straight into the GPU's memory. Sent at a
map's load, as the mods are found again and as the setting changes; the host
checks each file's header, skeleton and pieces against what the guest tells
of the model (its nodes' count and checksum, the region, permutation and
shader of each part it can draw) and answers each query with an id whose top
12 bits are the record's generation (as TEXTURE_STORE_MAP's), and the radius
the mesh reaches (the render-only culling radius the guest grows). query_count
0 closes the store. The queries, their tables and paths and the answers are
in a buffer of the guest's own, freed once the record is answered. */
struct xgpu_stream_mesh_store_map
{
	uint32_t header, reply;
	uint32_t generation;
	/* 1 enhanced, 2 max (for the host's log: the guest chose the files) */
	uint32_t tier;
	/* display.prism_model_budget_mb (0: auto) */
	uint32_t budget_megabytes;
	uint32_t query_count, queries;
	uint32_t strings, strings_size;
	/* XGPU_STREAM_MESH_STORE_LOADING: a map is loading */
	uint32_t flags;
};
#define XGPU_STREAM_MESH_STORE_LOADING 1u
/* display.prism_prefetch: the map's files read as it loads, within the
budget, not each when first drawn */
#define XGPU_STREAM_MESH_STORE_PREFETCH 2u
/* port: XGPU_STREAM_MESH_STORE_RESOLVED: display.lighting PRISM Rebaked or
Path Traced: the objects' levels are the resolver's (xgpu_geometry.h); its
absence (Classic) draws them as Classic always did. The game sends the set
again when the lighting changes between the two */
#define XGPU_STREAM_MESH_STORE_RESOLVED 4u
/* display.prism_lod_hysteresis: the percent a level's size is passed by
before it is switched to or from (bits 8 to 15; 0 at its cutoff) */
#define XGPU_STREAM_MESH_STORE_HYSTERESIS_SHIFT 8
#define XGPU_STREAM_MESH_STORE_HYSTERESIS_MASK 0xffu
/* a query: its file's path (offset into strings and length, NUL-terminated
there; a host path, as the guest opens it), the size and modification time
the guest found, the model's node count and checksum (mod_meshes.c's CRC-32
of the nodes' names, parents and default transforms: the .pmesh skeleton's
must match it), its table (offset into strings and count of 4-byte entries
{region, permutation, shader, 0}: each a part the model can draw) and its
base map scale (the stock layout's texture coordinates are of it) */
struct xgpu_stream_mesh_store_query
{
	uint32_t path, path_length;
	uint32_t file_size, stamp_seconds;
	uint32_t node_count, node_crc;
	uint32_t table, table_count;
	float base_map_scale[2];
};
/* an answer: the id (0 refused) and why (XGPU_STREAM_MESH_STORE_*), the
radius the mesh reaches from the model's origin (model units, every level of
detail and the bind pose), its bones and levels of detail */
enum
{
	XGPU_STREAM_MESH_STORE_ACCEPTED,
	XGPU_STREAM_MESH_STORE_UNREADABLE,
	XGPU_STREAM_MESH_STORE_CHANGED,
	XGPU_STREAM_MESH_STORE_FORMAT,
	XGPU_STREAM_MESH_STORE_NODES,
	XGPU_STREAM_MESH_STORE_TABLE,
	XGPU_STREAM_MESH_STORE_UV_RANGE,
	XGPU_STREAM_MESH_STORE_BUDGET,
	XGPU_STREAM_MESH_STORE_DEVICE,
	XGPU_STREAM_MESH_STORE_PATH,
};
struct xgpu_stream_mesh_store_answer
{
	uint32_t id;
	float render_radius;
	uint16_t bones;
	uint8_t lods, status;
	uint32_t unused;
};
/* the most queries a record has, and table entries a query has */
#define XGPU_STREAM_MESH_STORE_QUERIES 4096u
#define XGPU_STREAM_MESH_STORE_TABLE_MAXIMUM 4096u

/* MESH_INSTANCE: a model drawn with a mesh of the store (once each time
render_model draws it), its nodes' matrices as the vertex shader's
constants hold them (three rows of four: x' = row 0 . (x, y, z, 1), ...),
node_count of them following the record's fixed part. The instance is the
guest's running number (never 0); the draws that follow name it. */
struct xgpu_stream_mesh_instance
{
	uint32_t header;
	uint32_t instance;
	uint32_t mesh;
	/* the object's datum (0xffffffff: none) */
	uint32_t object;
	/* the size the level of detail is chosen by (display.model_detail's
	scale applied), in the 480-line screen's pixels */
	float lod_pixels;
	uint16_t node_count;
	/* XGPU_STREAM_MESH_INSTANCE_* */
	uint16_t flags;
	/* the game's clock (game_time_get's ticks, and the interpolation's
	fraction of the next) and the object's function values A-D: what an
	extended skeleton's springs and driven helpers move by (xgpu_rig.h) */
	uint32_t game_ticks;
	float game_fraction;
	float functions[4];
};
#define XGPU_STREAM_MESH_INSTANCE_SHADOW 1u
#define XGPU_STREAM_MESH_INSTANCE_FIRST_PERSON 2u
/* display.prism_secondary_motion off: springs and driven helpers follow
their parents */
#define XGPU_STREAM_MESH_INSTANCE_NO_SECONDARY 4u
/* port: display.lighting PRISM Rebaked or Path Traced: the object's level
is the resolver's (xgpu_geometry.h), one for its view, shadows and traced
shape; without it (Classic) chosen as Classic always did */
#define XGPU_STREAM_MESH_INSTANCE_RESOLVED 8u
/* the most nodes an instance has (model_definitions.h's MAXIMUM_NODES_PER_MODEL) */
#define XGPU_STREAM_MESH_NODES_MAXIMUM 64u

/* MESH_DRAW: the draws that follow, until the next MESH_DRAW, are a model
part's of an instance: drawn as the part's own until the instance's mesh is
in the GPU's memory, then (a carrier: XGPU_STREAM_MESH_DRAW_CARRIER, the
first part of its geometry with its shader) as every piece of the mesh's
level of detail with the part's region, permutation and shader, or (any
other part) not at all. instance 0: the draws that follow are no part's. */
struct xgpu_stream_mesh_draw
{
	uint32_t header;
	uint32_t instance;
	uint8_t region, permutation, shader, flags;
	uint16_t geometry, part;
};
#define XGPU_STREAM_MESH_DRAW_CARRIER 1u

/* display.prism_material_overlay (xgpu.h's "The material overlay"): the
uniform and images of the draws that follow whose pixel key has
material_overlay, sent as it changes. Its stages are images
(XGPU_STREAM_SOURCE_IMAGE, with no levels' targets; their format may be
XGPU_STREAM_IMAGE_RGBA_LEVELS: data holds every level, level 0 first, each
half the one before, at least 1, right after it) or none (source 0): tiles A
to D, then the class mask, then the surface map's two images (their
format XGPU_STREAM_IMAGE_BC5_LEVELS: data holds every level's BC5 blocks,
level 0 first, 16 bytes a block of 4 by 4), then PRISM v2's probes' atlas
(XGPU_STREAM_IMAGE_RGBA_LEVELS), at the backend's XGPU_OVERLAY_BINDING on */
#define XGPU_STREAM_IMAGE_RGBA_LEVELS 2
#define XGPU_STREAM_IMAGE_BC5_LEVELS 3
struct xgpu_stream_material_overlay
{
	uint32_t header;
	float uniforms[20][4];
	struct xgpu_stream_texture_stage stages[8];
};

/* ---------- PRISM's materials (display.prism_materials; xgpu.h's "PBR
materials", port/linux/game/mod_pbr.c)

MATERIAL_DEFINE: a material's images (the texture store's ids of KIND_MATERIAL
files: 0 none; an id of an older generation is none) and uniform
(XGPU_MATERIAL_UNIFORM_COUNT vec4s, xgpu.h: the backend writes [5] and
[6].x, .y and .z, what is resident, itself), id from 1 to
XGPU_STREAM_MATERIALS, sent after each TEXTURE_STORE_MAP whose answers it
names, and again as the setting changes; id 0 forgets every material.
flags: the pixel key's material_pbr the guest gives its draws (xgpu.h's
XGPU_PBR_*), which says which images the shader reads. MATERIAL_BIND (a
value record): the material of the draws that follow whose pixel key has
material_pbr (0 none: drawn with every image's fallback). Without a
material (display.prism_materials "stock", or a host without it:
capabilities' materials 0) neither is sent. */
#define XGPU_STREAM_MATERIALS 4096u
#define XGPU_STREAM_MATERIAL_IMAGES 6
#define XGPU_STREAM_MATERIAL_UNIFORMS 9
struct xgpu_stream_material_define
{
	uint32_t header;
	uint32_t id, flags;
	uint32_t textures[XGPU_STREAM_MATERIAL_IMAGES];
	float uniforms[XGPU_STREAM_MATERIAL_UNIFORMS][4];
};

/* ---------- pipelines */

/* the vertex constants first .. first + count, count vec4s following */
struct xgpu_stream_constants
{
	uint32_t header;
	uint32_t first, count;
	float values[][4];
};

/* the bytes of struct xgpu_vertex_instruction (xgpu_shader.h, all
unsigned char: alike on both sides), count of them, following */
#define XGPU_STREAM_VERTEX_INSTRUCTION_SIZE 36
#define XGPU_STREAM_VERTEX_PROGRAM_MAXIMUM 136

struct xgpu_stream_vertex_program
{
	uint32_t header;
	/* the vertex shader's number (for the logs); its microcode's hash
	(low word first), the key's vertex_program */
	uint32_t id;
	uint32_t hash[2];
	uint32_t instruction_count;
	uint32_t projects;
	uint8_t instructions[][XGPU_STREAM_VERTEX_INSTRUCTION_SIZE];
};

/* xgpu.h's struct nv2a_vertex_lighting */
struct xgpu_stream_vertex_lighting
{
	uint32_t header;
	uint32_t hash[2];
	int32_t lights;
	uint32_t normal_instruction, normal_register;
	uint32_t position_instruction, position_register;
	uint32_t world_found, world_instruction, world_register;
};

/* xgpu.h's struct nv2a_pixel_shader_key */
#define XGPU_STREAM_COMBINER_STATES 57

struct xgpu_stream_pixel_key
{
	uint32_t combiner_state[XGPU_STREAM_COMBINER_STATES];
	uint32_t texture_modes;
	uint8_t sampler_type[4];
	uint8_t alpha_kill[4];
	uint8_t color_sign[4];
	uint32_t alpha_test_function;
	uint8_t fog_enable, fog_table_mode, count_samples, coverage_alpha;
	uint8_t point_threshold, per_pixel_lighting, alpha_test_samples, multiview;
	uint8_t source_blend, dynamic_lights, vr_effect, ambient_occlusion;
	/* display.hdr's tag (xgpu_hdr.h) */
	uint16_t hdr;
	/* display.prism_material_overlay (xgpu.h); PRISM's light, the native
	key's prism_lighting and lightmap_filter in one byte (XGPU_STREAM_PRISM_*,
	below). Both in what was padding: the keys without them hash as they did */
	uint8_t material_overlay, prism;
	/* display.prism_materials (xgpu.h's material_pbr; the pixel program's
	is this), in a word of its own past the prism byte */
	uint8_t material_pbr, unused[3];
};

/* the pixel key's prism byte: PRISM v2's model light (xgpu.h's
XGPU_PRISM_*, below XGPU_STREAM_PRISM_LIGHTMAP_SHIFT: five bits) and
display.prism_lightmaps' filter (XGPU_LIGHTMAP_FILTER_*, with
display.prism_baked_shadows' XGPU_LIGHTMAP_FILTER_SUN: three bits, shifted
by it); 0 as the game draws */
#define XGPU_STREAM_PRISM_LIGHTMAP_SHIFT 5
#define XGPU_STREAM_PRISM_LIGHTING_MASK ((1 << XGPU_STREAM_PRISM_LIGHTMAP_SHIFT) - 1)
#define XGPU_STREAM_PRISM(prism_lighting, lightmap_filter) \
	((uint8_t)(((prism_lighting) & XGPU_STREAM_PRISM_LIGHTING_MASK) | ((lightmap_filter) << XGPU_STREAM_PRISM_LIGHTMAP_SHIFT)))
#define XGPU_STREAM_PRISM_LIGHTING(prism) ((prism) & XGPU_STREAM_PRISM_LIGHTING_MASK)
#define XGPU_STREAM_PRISM_LIGHTMAP_FILTER(prism) ((prism) >> XGPU_STREAM_PRISM_LIGHTMAP_SHIFT)

/* xgpu_backend.h's struct xgpu_pipeline_key */
struct xgpu_stream_pipeline_key
{
	uint32_t vertex_program[2];
	uint32_t packed_attributes;
	uint32_t stencil_read_mask, stencil_write_mask;
	uint8_t immediate, lighting, views;
	uint8_t blend, blend_source, blend_destination, blend_operation;
	uint8_t color_mask;
	uint8_t depth_test, depth_function, depth_write;
	uint8_t stencil_test, stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
	uint8_t cull, front_counter_clockwise, cull_front;
	uint8_t fill, depth_bias;
	uint8_t color_format, depth_format, samples;
	struct xgpu_stream_pixel_key pixel;
};

/* xgpu_shader.h's struct xgpu_pixel_program: its parts of unsigned chars
alone (alike on both sides) as their bytes, its unsigned longs as 32 bits */
#define XGPU_STREAM_TEXTURE_STAGE_SIZE 9
#define XGPU_STREAM_COMBINER_STAGE_SIZE 44
#define XGPU_STREAM_FINAL_COMBINER_SIZE 25

struct xgpu_stream_pixel_program
{
	uint8_t stages[4][XGPU_STREAM_TEXTURE_STAGE_SIZE];
	uint8_t mux_msb;
	uint8_t final[XGPU_STREAM_FINAL_COMBINER_SIZE];
	uint8_t fog, fog_mode;
	uint32_t combiner_count;
	uint8_t combiners[8][XGPU_STREAM_COMBINER_STAGE_SIZE];
	uint32_t alpha_test;
	uint8_t alpha_test_samples;
	uint8_t count_samples, coverage_alpha, point_threshold, per_pixel_lighting, dynamic_lights, multiview;
	uint8_t source_blend;
	uint8_t ambient_occlusion;
	/* display.prism_material_overlay (xgpu.h; in what was padding) */
	uint8_t material_overlay;
	/* display.hdr's tag (xgpu_hdr.h) */
	uint16_t hdr;
	/* display.prism_lightmaps, PRISM v2's model light (xgpu.h's
	XGPU_LIGHTMAP_FILTER_*, XGPU_PRISM_*) */
	uint8_t lightmap_filter, prism_lighting, unused[2];
};

/* a pipeline key the guest numbered (from 1, below XGPU_STREAM_PIPELINES),
sent once; XGPU_STREAM_PIPELINE names it from then on. With its pixel
shader decoded (nv2a_pixel_program_decode), so that the host needs no NV2A
decoding. */
struct xgpu_stream_pipeline_define
{
	uint32_t header;
	uint32_t id;
	struct xgpu_stream_pipeline_key key;
	struct xgpu_stream_pixel_program pixel_program;
};

/* a pipeline's twin in the pre-pass, made of its definition in place
(nonzero; 0 none, the draw not drawn in the pre-pass):
- of one with the world's light read from its pixel
  (XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED), its pixel key's occlusion
  XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS (its light kept): it writes its
  pixel's inputs;
- of another opaque one that writes the depth where it is nearer or as
  near (an occluder: the first-person weapon, a surface lighting itself),
  _INPUTS alone, its test made strictly nearer (so that it never takes the
  pixel of a surface it draws over at its own depth): it writes none, so
  that no surface it hides has its occlusion found;
into the inputs' target (xgpu_backend.h's XGPU_FORMAT_WORLD_AO_INPUTS:
RGBA32Uint), one sample, nothing blended, every channel written, no HDR tag,
no enhanced dynamic lights, no material overlay or material (their images are bound for
the colour pass's draws alone), no samples counted; its depth, stencil, raster,
vertex program and alpha test as they are. The backends, and the Mac's
library's build, make it alike */
#define XGPU_STREAM_WORLD_AO_SAMPLED 0x50u
#define XGPU_STREAM_WORLD_AO_INPUTS 0x90u
#define XGPU_STREAM_WORLD_AO_OCCLUDER 0x80u
#define XGPU_STREAM_FORMAT_WORLD_AO_INPUTS 4u
/* (xgpu_backend.h's XGPU_COMPARE_LESS and _LESS_EQUAL) */
#define XGPU_STREAM_COMPARE_LESS 1u
#define XGPU_STREAM_COMPARE_LESS_EQUAL 3u
static inline int xgpu_stream_world_ao_twin(struct xgpu_stream_pipeline_define *definition)
{
	uint8_t occlusion = definition->pixel_program.ambient_occlusion;

	if (occlusion & 0x80u)
		return 0;
	if ((occlusion & XGPU_STREAM_WORLD_AO_SAMPLED) == XGPU_STREAM_WORLD_AO_SAMPLED)
	{
		occlusion = (uint8_t)((occlusion & 0x0fu) | XGPU_STREAM_WORLD_AO_INPUTS);
	}
	else
	{
		if (occlusion || !definition->key.depth_test || !definition->key.depth_write || definition->key.blend ||
			!definition->key.color_mask || definition->key.pixel.source_blend ||
			(definition->key.depth_function != XGPU_STREAM_COMPARE_LESS &&
			definition->key.depth_function != XGPU_STREAM_COMPARE_LESS_EQUAL))
			return 0;
		occlusion = (uint8_t)XGPU_STREAM_WORLD_AO_OCCLUDER;
		definition->key.depth_function = (uint8_t)XGPU_STREAM_COMPARE_LESS;
	}
	definition->key.pixel.ambient_occlusion = definition->pixel_program.ambient_occlusion = occlusion;
	definition->key.blend = definition->key.blend_source = definition->key.blend_destination = 0;
	definition->key.blend_operation = 0;
	definition->key.color_mask = 0x0f;
	definition->key.color_format = (uint8_t)XGPU_STREAM_FORMAT_WORLD_AO_INPUTS;
	definition->key.samples = 0;
	definition->key.pixel.alpha_test_samples = definition->pixel_program.alpha_test_samples = 0;
	definition->key.pixel.hdr = definition->pixel_program.hdr = 0;
	definition->key.pixel.source_blend = definition->pixel_program.source_blend = 0;
	definition->key.pixel.count_samples = definition->pixel_program.count_samples = 0;
	definition->key.pixel.dynamic_lights = definition->pixel_program.dynamic_lights = 0;
	definition->key.pixel.material_overlay = definition->pixel_program.material_overlay = 0;
	definition->key.pixel.prism = 0;
	/* (a twin of a draw that reads display.prism_rt's traced light keeps
	its lightmap's filter: it writes the light its lightmap gives) */
	if (!(occlusion & 0x08u))
		definition->pixel_program.lightmap_filter = 0;
	definition->pixel_program.prism_lighting = 0;
	definition->key.pixel.material_pbr = 0;
	return 1;
}


/* a pipeline's G-buffer twin in display.lighting "ce_deferred"'s G-buffer
pass (GBUFFER_BEGIN), made of its definition in place (nonzero; 0 none, the
draw not drawn in the pass): of an opaque one that writes the depth where it
is nearer or as near (its alpha test kept), into the G-buffer target
(RGBA32Uint: XGPU_STREAM_FORMAT_GBUFFER, its format's number for the
backends), one sample, nothing blended, every channel written; its depth,
stencil, raster, vertex program, alpha test, lighting and display.
prism_materials' material kept (an object's base colour is its combiners'
with its material's albedo); no HDR tag, no enhanced dynamic lights, no
world's light, no material overlay, no sun's shadows, no lightmap filter, no
samples counted (their sets and images are the colour passes'). The backend
makes its shaders with the G-buffer's outputs (xgpu_spirv_options gbuffer) */
#define XGPU_STREAM_FORMAT_GBUFFER 5u
static inline int xgpu_stream_gbuffer_twin(struct xgpu_stream_pipeline_define *definition)
{
	if (!definition->key.depth_test || !definition->key.depth_write || definition->key.blend ||
		!definition->key.color_mask || definition->key.pixel.source_blend ||
		(definition->key.depth_function != XGPU_STREAM_COMPARE_LESS &&
		definition->key.depth_function != XGPU_STREAM_COMPARE_LESS_EQUAL))
		return 0;
	definition->key.blend = definition->key.blend_source = definition->key.blend_destination = 0;
	definition->key.blend_operation = 0;
	definition->key.color_mask = 0x0f;
	definition->key.color_format = (uint8_t)XGPU_STREAM_FORMAT_GBUFFER;
	definition->key.samples = 0;
	definition->key.pixel.alpha_test_samples = definition->pixel_program.alpha_test_samples = 0;
	definition->key.pixel.hdr = definition->pixel_program.hdr = 0;
	definition->key.pixel.source_blend = definition->pixel_program.source_blend = 0;
	definition->key.pixel.count_samples = definition->pixel_program.count_samples = 0;
	definition->key.pixel.dynamic_lights = definition->pixel_program.dynamic_lights = 0;
	definition->key.pixel.ambient_occlusion = definition->pixel_program.ambient_occlusion = 0;
	definition->key.pixel.material_overlay = definition->pixel_program.material_overlay = 0;
	definition->key.pixel.prism = 0;
	definition->pixel_program.lightmap_filter = 0;
	definition->pixel_program.prism_lighting = 0;
	return 1;
}

/* a pipeline's shadow twin in display.prism_shadows "cascaded"'s pass, made
of its definition in place (nonzero; 0 none, the draw not drawn in the
pass): of an opaque one that writes the depth where it is nearer or as near
(its alpha test kept: a fence's, a tree's leaves), into the depth alone (no
colour: XGPU_STREAM_FORMAT_NONE, nothing written, nothing blended), one
sample, both faces, no stencil, no HDR tag, no lighting of its pixels, no
enhanced dynamic lights, no material overlay, no world's light, no samples
counted; its vertex program as it is (the backend draws its clip position
into each cascade: XGPU_STREAM_PRISM_SHADOWS) */
#define XGPU_STREAM_FORMAT_NONE 0u
static inline int xgpu_stream_prism_shadow_twin(struct xgpu_stream_pipeline_define *definition)
{
	if (!definition->key.depth_test || !definition->key.depth_write || definition->key.blend ||
		!definition->key.color_mask || definition->key.pixel.source_blend || definition->key.views > 1 ||
		(definition->key.depth_function != XGPU_STREAM_COMPARE_LESS &&
		definition->key.depth_function != XGPU_STREAM_COMPARE_LESS_EQUAL))
		return 0;
	definition->key.depth_function = (uint8_t)XGPU_STREAM_COMPARE_LESS;
	definition->key.blend = definition->key.blend_source = definition->key.blend_destination = 0;
	definition->key.blend_operation = 0;
	definition->key.color_mask = 0;
	definition->key.color_format = (uint8_t)XGPU_STREAM_FORMAT_NONE;
	definition->key.samples = 0;
	definition->key.stencil_test = 0;
	definition->key.stencil_write_mask = 0;
	definition->key.cull = 0;
	definition->key.depth_bias = 0;
	definition->key.pixel.alpha_test_samples = definition->pixel_program.alpha_test_samples = 0;
	definition->key.pixel.hdr = definition->pixel_program.hdr = 0;
	definition->key.pixel.source_blend = definition->pixel_program.source_blend = 0;
	definition->key.pixel.count_samples = definition->pixel_program.count_samples = 0;
	definition->key.pixel.dynamic_lights = definition->pixel_program.dynamic_lights = 0;
	definition->key.pixel.material_overlay = definition->pixel_program.material_overlay = 0;
	definition->key.pixel.ambient_occlusion = definition->pixel_program.ambient_occlusion = 0;
	definition->key.pixel.prism = 0;
	definition->key.pixel.per_pixel_lighting = definition->pixel_program.per_pixel_lighting = 0;
	definition->pixel_program.lightmap_filter = definition->pixel_program.prism_lighting = 0;
	return 1;
}

/* xgpu_backend.h's struct xgpu_dynamic_state */
struct xgpu_stream_dynamic_state
{
	int32_t viewport[4];
	int32_t scissor[4];
	float depth_range[2];
	uint32_t stencil_reference;
	float blend_color[4];
	float depth_bias[2];
};

/* reversed depth (xgpu_backend.h, "Reversed depth"): the depth test's
function that passes on reversed depth what the key's (xgpu_backend.h's
XGPU_COMPARE_*: never, less, equal, less or equal, greater, not equal,
greater or equal, always) passes on the game's */
static __inline uint8_t xgpu_reversed_depth_function(uint8_t function)
{
	static const uint8_t reversed[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

	return function < 8 ? reversed[function] : function;
}

/* a draw's constant depth bias (its dynamic state's depth_bias[1], in the
24-bit depth buffer's units; the pipeline's depth_bias set) as the shaders
add it to reversed depth: toward the camera, positive */
static __inline float xgpu_reversed_depth_bias(int biased, float offset)
{
	return biased ? -offset / 16777215.0f : 0.0f;
}

/* xgpu_backend.h's struct xgpu_draw_uniforms (floats alone: alike on both
sides) */
struct xgpu_stream_draw_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float alpha_reference;
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float screen_offset;
	float texture_lod_bias[4];
	float world_ao[4];
};

/* xgpu_backend_draw_state: what changed since the last follows, the
dynamic state (XGPU_STREAM_DRAW_DYNAMIC), then the uniforms' pieces that
changed (XGPU_STREAM_DRAW_UNIFORMS: the uniforms as 16-byte pieces, the
last of 4, those whose bits are set in pieces, low word first, in order);
a call that changes neither sends nothing */
enum
{
	XGPU_STREAM_DRAW_DYNAMIC = 1,
	XGPU_STREAM_DRAW_UNIFORMS = 2,
};

#define XGPU_STREAM_UNIFORM_PIECES ((sizeof(struct xgpu_stream_draw_uniforms) + 15) / 16)

struct xgpu_stream_draw_state
{
	uint32_t header;
	uint32_t parts;
	uint32_t uniforms_serial;
	uint32_t pieces[2];
};

/* ---------- vertices and draws */

/* inline: data is in the ring */
struct xgpu_stream_mirror_write
{
	uint32_t header;
	uint32_t segment, offset, size;
	uint32_t unused;
	uint32_t data;
};

/* xgpu_backend.h's struct xgpu_vertex_element */
struct xgpu_stream_vertex_element
{
	uint8_t reg, stream, format, bytes;
	uint16_t offset;
};

/* a vertex layout the guest numbered (from 1, below XGPU_STREAM_LAYOUTS):
xgpu_vertex_input's declaration, elements and packed mask, sent once */
struct xgpu_stream_layout_define
{
	uint32_t header;
	uint32_t id;
	uint32_t declaration;
	uint32_t element_count;
	uint32_t packed_mask;
	struct xgpu_stream_vertex_element elements[];
};

struct xgpu_stream_vertex_stream
{
	/* the mirror's segment and the offset in it, or -1 and the vertices
	(inline) at data, bytes of them */
	int32_t segment;
	uint32_t offset;
	uint32_t data;
	uint32_t bytes;
	uint32_t stride;
};

/* xgpu_backend_set_vertices: the layout, then an xgpu_stream_vertex_stream
for each stream present (bits of streams, in order), then the values of
the inputs that changed of those no stream feeds (bits of attributes, a
vec4 each, in order); then the streamed vertices' data, inline. A draw is
in the same submit as the VERTICES record whose data it reads (the guest
sends the record again before a draw in a later submit), so the data is
the backend's until the fence after that draw. */
struct xgpu_stream_vertices
{
	uint32_t header;
	uint32_t layout;
	uint16_t streams, attributes;
};

struct xgpu_stream_draw
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
};

/* the indices (inline) at indices, count of them (16 bits each) */
struct xgpu_stream_draw_indices
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint32_t indices;
};

/* indices from the mirror's segment at offset, or (segment -1) inline at
indices, temporary if the front end made them for this draw alone (a
backend that keeps index arrays must not keep them) */
struct xgpu_stream_draw_indexed
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint32_t minimum;
	int32_t segment;
	uint32_t offset;
	uint32_t indices;
	uint32_t temporary;
};

/* immediate mode: count vertices of the registers written (bits of
written), a vec4 each in register order (inline at vertices: no room for
those not written); the values of the registers not written that changed
(bits of attributes, a vec4 each following); the indices (inline, 0
none) */
struct xgpu_stream_draw_immediate
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint16_t written, attributes;
	uint32_t vertices;
	uint32_t indices, index_count;
};

/* ---------- visibility tests */

struct xgpu_stream_visibility_end
{
	uint32_t header;
	uint32_t slot;
	float area;
	int32_t views;
	int32_t samples;
	/* the guest's number for the test (never 0; one more each test, modulo
	2^32): published with its count, so that the guest knows which test a
	slot's count is of (a count reaches the guest frames after its test) */
	uint32_t test;
};

/* ---------- the enhanced dynamic lights */

/* xgpu.h's struct xgpu_dynamic_lights_block (floats alone) */
#define XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM 128

struct xgpu_stream_dynamic_lights_block
{
	float grid[3][4];
	float position[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
	float color[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
	float cone[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
};

/* the block's lights past count are zero; the cells (inline) at cells,
size bytes */
struct xgpu_stream_dynamic_lights_upload
{
	uint32_t header;
	uint32_t count;
	uint32_t cells, size;
	struct xgpu_stream_dynamic_lights_block block;
};

/* the frame's lights: the block (its lights past count zero), and the
cells' bytes from offset that changed since the stream's last change (size
bytes, inline at cells; none: 0), the others as they were. The offset and
size are multiples of 16, inside XGPU_STREAM_DYNAMIC_LIGHTS_CELLS_BYTES;
the stream's first change is of all of them. */
struct xgpu_stream_dynamic_lights_change
{
	uint32_t header;
	uint32_t count;
	uint32_t cells, offset, size;
	struct xgpu_stream_dynamic_lights_block block;
};

/* the world's count texels (inline) at texels, count * 16 bytes; count
at most xgpu.h's XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS */
#define XGPU_STREAM_DYNAMIC_LIGHTS_WORLD_TEXELS (240u * 1024u)
/* the cells' texture's slices of 32 x 32 texels (xgpu.h): the cells and the
clusters' masks, uploaded each frame, then the world's */
#define XGPU_STREAM_DYNAMIC_LIGHTS_CELL_SLICES 17u
/* their bytes (xgpu.h's XGPU_DYNAMIC_LIGHTS_CELLS_BYTES) */
#define XGPU_STREAM_DYNAMIC_LIGHTS_CELLS_BYTES (32u * 32u * XGPU_STREAM_DYNAMIC_LIGHTS_CELL_SLICES * 16u)
#define XGPU_STREAM_DYNAMIC_LIGHTS_WORLD_SLICES 240u
struct xgpu_stream_dynamic_lights_world
{
	uint32_t header;
	uint32_t count;
	uint32_t texels;
};

/* xgpu_backend.h's struct xgpu_dynamic_lights_halos but its halos, which
follow (count * 2 vec4s) */
struct xgpu_stream_dynamic_lights_halos
{
	uint32_t header;
	uint32_t color, depth;
	int32_t layers;
	int32_t count;
	float camera_right[3], camera_up[3];
	float projection[6][4];
	float viewport_scale[4], viewport_offset[4];
	float camera[3];
	float softness;
	float screen_offset;
	float halos[][4];
};

/* ---------- the VR mode

The backend composes each frame (the eyes' images, the HUD's and its
panels', the scope's) and ends it with OpenXR, on the host's render thread:
OpenXR's calls that use the device (the session made, the swapchains'
images acquired and released, the frame begun and ended) are made there
(port/linux/host/host_vr_stream.c); the game waits for the runtime's frames and
locates the views (vr.c). What the composition reads of the game's VR mode
(vr.h's and vr_visor.h's functions the guest's backend calls) comes in the
records: VR_STATE before each VR_COMPOSE, the gaze with VR_EYES, the layers
the game would submit with VR_SUBMIT; and what vr.c sets in the backend as
it draws (the eyes' transforms and corrections, the sky's depth clamping, the
feed and the tint) as records of their own, in their order. vr.h's and
vr_visor.h's structures are of 4-byte fields, alike on both sides, and are
carried as their words, but the visor image's noise textures (pointers:
guest addresses here). */

/* xgpu_backend.h's struct xgpu_vr_info (ints alone), in and out; and
vr.foveation's inset (halo_vr_foveation_inset, 0 off) and its name (for
the log), in; and out, the visibility tests counted by the pixel shaders
from now on (xgpu_backend_capabilities' sample_counting, which the VR
mode's eye pass needs) */
/* (statistics' bits: debug.gpu_stats, and with it debug.vr_gpu_time) */
#define XGPU_STREAM_VR_STATISTICS 1
#define XGPU_STREAM_VR_GPU_TIME 2
struct xgpu_stream_vr_info
{
	int32_t eye_width, eye_height;
	int32_t hud_width, hud_height;
	int32_t scope_size;
	int32_t statistics;
	int32_t foveated;
	int32_t fovea_width, fovea_height;
	float foveation_inset;
	char foveation_name[64];
	int32_t sample_counting;
};

/* vr_host.h's struct vr_host_latch, in words */
#define XGPU_STREAM_VR_LATCH_WORDS 52

/* the eye pass begins; with its insets' middles where foveated
(halo_vr_foveation: foveation 0, none this frame), and the poses the eyes
are drawn from for the late latch (halo_vr_latch's; latched 0: none) */
struct xgpu_stream_vr_eyes
{
	uint32_t header;
	uint32_t eye_target;
	uint32_t image;
	int32_t foveation;
	float centres[2][2];
	uint32_t latched;
	uint32_t latch[XGPU_STREAM_VR_LATCH_WORDS];
};

/* xgpu_backend.h's struct xgpu_vr_frame */
struct xgpu_stream_vr_frame
{
	int32_t eyes_drawn, hud_drawn, scope_drawn, hud_panels;
	uint32_t eye_target, eye_depth, scope, hud;
	int32_t eye_depth_layers;
};

/* VR_COMPOSE */
struct xgpu_stream_vr_frame_record
{
	uint32_t header;
	struct xgpu_stream_vr_frame frame;
	uint32_t screenshot;
};

/* vr_host.h's struct vr_host_layers and struct vr_host_pose, in words */
#define XGPU_STREAM_VR_LAYERS_WORDS 116
#define XGPU_STREAM_VR_POSE_WORDS 8

/* VR_SUBMIT: the frame record (screenshot: the eyes', HUD's and scope's
images written, numbered shot); the game waited for a frame of the
runtime's for it (host_vr_frame_wait: the host begins and ends one), and the
layers it would submit (halo_vr_present's, with every image taken as
acquired: the host leaves out those it did not draw) and the head they
were located with (halo_vr_submitted's) */
struct xgpu_stream_vr_submit
{
	struct xgpu_stream_vr_frame_record record;
	uint32_t waited;
	uint32_t shot;
	uint32_t layers[XGPU_STREAM_VR_LAYERS_WORDS];
	uint32_t head[XGPU_STREAM_VR_POSE_WORDS];
};

/* vr.h's and vr_visor.h's structures, in words (the visor image's up to its
noise textures, which the 64-bit host's lie 4 bytes further, aligned) */
#define XGPU_STREAM_VR_SCOPE_WINDOW_WORDS 32
#define XGPU_STREAM_VR_COCKPIT_WINDOW_WORDS 34
#define XGPU_STREAM_VR_COCKPIT_DISPLAY_WORDS 19
#define XGPU_STREAM_VR_HUD_IMAGE_WORDS 50
#define XGPU_STREAM_VR_PANEL_WORDS 20
#define XGPU_STREAM_VR_VISOR_IMAGE_WORDS 71
#define XGPU_STREAM_VR_RIM_WORDS 39

/* VR_STATE's answered: the functions whose result says whether there is
one (XGPU_STREAM_VR_STATE_*: the scope's smart-link, its window, a cockpit's
display, the HUD's panels, the helmet's rim, the eyes' depth wanted), and
whether a frame of the runtime's is being drawn (halo_vr_frame_waited: its
images may be acquired) */
enum
{
	XGPU_STREAM_VR_STATE_SMARTLINK = 1,
	XGPU_STREAM_VR_STATE_SCOPE_WINDOW = 2,
	XGPU_STREAM_VR_STATE_COCKPIT = 4,
	XGPU_STREAM_VR_STATE_PANELS = 8,
	XGPU_STREAM_VR_STATE_RIM = 16,
	XGPU_STREAM_VR_STATE_DEPTH = 32,
	XGPU_STREAM_VR_STATE_FRAME = 64,
};

/* what the frame's composition asks of vr.c and vr_visor.c (vr.h,
vr_visor.h), as they answered when the game composed it: the scope's
(halo_vr_scope_zoom, _smartlink, _window, _readout, _animation, _shape,
_gun), a
cockpit's display (halo_vr_cockpit_window), the vignette
(halo_vr_vignette), the visor's (vr_visor_hud_image, _panels, _image,
_rim) */
struct xgpu_stream_vr_state
{
	uint32_t header;
	uint32_t answered;
	float scope_zoom, scope_zoom_height;
	float scope_reticle[2];
	uint32_t scope_window[XGPU_STREAM_VR_SCOPE_WINDOW_WORDS];
	float scope_range;
	int32_t scope_rounds_loaded, scope_rounds_carried;
	float scope_animation[3];
	float scope_shape[3];
	int32_t scope_gun;
	uint32_t cockpit_window[XGPU_STREAM_VR_COCKPIT_WINDOW_WORDS];
	uint32_t cockpit_display[XGPU_STREAM_VR_COCKPIT_DISPLAY_WORDS];
	float vignette;
	float vignette_fov[2][4];
	uint32_t hud_image[XGPU_STREAM_VR_HUD_IMAGE_WORDS];
	uint32_t panels[4][XGPU_STREAM_VR_PANEL_WORDS];
	uint32_t visor_image[XGPU_STREAM_VR_VISOR_IMAGE_WORDS];
	uint32_t visor_noise[2];
	float visor_time;
	uint32_t rim[XGPU_STREAM_VR_RIM_WORDS];
};

/* halo_vr_eye_feed's (7) and halo_vr_eye_tint's (3) values */
struct xgpu_stream_vr_values
{
	uint32_t header;
	float values[];
};

/* The late latch. The host locates the head and the controllers again as
it begins the eye pass (VR_EYES), and moves what the frame draws by how far
they have moved since the game located them: the eyes' transforms
(VR_EYE_TRANSFORMS) for each projection the game sets, by the head's move
(the world), its turn alone (the sky, drawn about the eye), or the head's
and the weapon's controller's (the first-person weapon and the hands), and
the eyes' layers' poses, and the scope's with the weapon. */
enum
{
	XGPU_STREAM_VR_LATCH_NONE = 0,
	XGPU_STREAM_VR_LATCH_WORLD,
	XGPU_STREAM_VR_LATCH_SKY,
	XGPU_STREAM_VR_LATCH_HELD,
};

/* VR_EYE_TRANSFORMS: halo_vr_set_eye_transforms' (2 row-major 4x4s, the
centre's clip position to each eye's), what they move with in the late
latch (XGPU_STREAM_VR_LATCH_*), and each eye's clip transform from its own
space (row-major: OpenXR's axes, metres) for the projection
(halo_vr_eye_latch's) */
struct xgpu_stream_vr_eye_transforms
{
	uint32_t header;
	int32_t latch;
	float transforms[2][16];
	float clip_from_eye[2][16];
};

/* VR_SCREEN_CORRECTIONS: halo_vr_set_screen_corrections' (2 vec4s), and the
point they are for (halo_vr_screen_point's: its clip position in the
centre's view, w 0 none, and the viewport's width and height), from which
the late latch makes them again */
struct xgpu_stream_vr_screen_corrections
{
	uint32_t header;
	float corrections[2][4];
	float point[4];
	float viewport[2];
};

/* ---------- display.prism_rt (docs/prism_rt.md)

RT_STATIC: the structure BSP's triangles in world space, for the
acceleration structures the backend traces (its own copy: the host never
reads the game's tags). Phase XGPU_RT_STATIC_BEGIN (a BSP's set begins:
generation names it, triangles is its total, the data none),
_TRIANGLES (count triangles from first: 9 floats each, three corners'
x y z in world units, at data), _END (the set is whole: the backend builds
it, then traces it in place of the last), or _RELEASE (none: the map is
gone). The guest sends at most XGPU_STREAM_RT_SLICE_TRIANGLES a frame,
never while a map loads. With them, for display.prism_rt's materials and
light rig (docs/prism_rt.md, "The rig"), sent before _END:
_ATTRIBUTES (count triangles' attributes from first,
XGPU_STREAM_RT_ATTRIBUTE_WORDS words each: the material and the
triangle's kind (XGPU_RT_TRIANGLE_*) in the first, the three corners' base
map coordinates as six floats after it, a word unused); _MATERIALS (count
materials from first, XGPU_STREAM_RT_MATERIAL_FLOATS floats each:
XGPU_RT_MATERIAL_*; triangles is their total); _ALPHA (count words from
first of the alpha-tested materials' alpha maps, a byte a texel, at most
XGPU_STREAM_RT_ALPHA_WORDS; triangles is their total). */
enum
{
	XGPU_RT_STATIC_BEGIN,
	XGPU_RT_STATIC_TRIANGLES,
	XGPU_RT_STATIC_END,
	XGPU_RT_STATIC_RELEASE,
	XGPU_RT_STATIC_ATTRIBUTES,
	XGPU_RT_STATIC_MATERIALS,
	XGPU_RT_STATIC_ALPHA,
};
/* a triangle's kind (its attributes' first word, bits 16 and 17): drawn
opaque, alpha-tested (traced with its material's alpha map), or a light
alone (a transparent light card CE's radiosity lit from: sampled as a
light, never traced) */
enum
{
	XGPU_RT_TRIANGLE_OPAQUE,
	XGPU_RT_TRIANGLE_ALPHA_TESTED,
	XGPU_RT_TRIANGLE_LIGHT,
};
#define XGPU_STREAM_RT_ATTRIBUTE_WORDS 8u
#define XGPU_STREAM_RT_MATERIAL_FLOATS 16u
#define XGPU_STREAM_RT_MATERIALS 4096u
#define XGPU_STREAM_RT_ALPHA_WORDS (8u << 20)
/* a material's floats: its albedo (linear rgb), its flags
(XGPU_RT_MATERIAL_FLAG_*, a whole number), the light it gives as a lamp
(its radiance, linear, in the rig's units), its alpha test's reference
(0 to 1), its alpha map (the byte its texels start at, width, height),
its glow (a self-illumination map's mean light, linear), one unused */
enum
{
	XGPU_RT_MATERIAL_ALBEDO = 0,
	XGPU_RT_MATERIAL_FLAGS = 3,
	XGPU_RT_MATERIAL_EMISSION = 4,
	XGPU_RT_MATERIAL_ALPHA_REFERENCE = 7,
	XGPU_RT_MATERIAL_ALPHA_MAP = 8,
	XGPU_RT_MATERIAL_GLOW = 11,
};
enum
{
	XGPU_RT_MATERIAL_FLAG_LIGHT = 1,
	XGPU_RT_MATERIAL_FLAG_ALPHA_TESTED = 2,
	XGPU_RT_MATERIAL_FLAG_GLOW = 4,
};
#define XGPU_STREAM_RT_SLICE_TRIANGLES 65536u
struct xgpu_stream_rt_static
{
	uint32_t header;
	uint32_t phase;
	uint32_t generation;
	uint32_t first, count;
	uint32_t triangles;
	uint32_t data, size;
};

/* RT_FRAME: each frame's ray tracing, before its first window (its pre-
pass reads it): the mode (XGPU_RT_*: 0 none, the backend's passes
skipped, the occlusion as world AO found it), quality, resolution,
denoiser, reflections, objects and rate (display.prism_rt_*: 0 the first
value each lists), the mood (0 to 100), the debug view
(XGPU_RT_VIEW_*); settings: the sun's share of a sunlit lightmap's and of a
model's light the objects take where no rig is fitted, and whether the
level traced is a remodel (1, its faces wound either way); then sun: the direction toward the sky's sun (x y z,
0 none) and its strength's share of a sunlit lightmap's light (w), its
colour (linear) and power, the sky's outdoor ambient (radiosity colour and
power); the game clock (ticks, fraction) and the frame's number; then the
lights (light_count of XGPU_STREAM_RT_LIGHTS: position and radius, colour
and intensity, direction and cone cosine (w 2 for none), id and type) */
#define XGPU_STREAM_RT_LIGHTS 64u
/* the light rig (docs/prism_rt.md, "The rig": CE's own lights at the
relight fit's scales), linear, in the units the fit made CE's lightmaps
of: the sun's light (colour by power, scale and colour gain over pi: its
light on a surface facing it squarely, as the bake's sun lamp gives it) and
its angular diameter (radians: the
disc traced, display.prism_sun_size); the sky's radiance (its outdoor
ambient by power, scale and gain) and the power the fit found CE's pages
in (1, or 2.2: a lightmap's texel is the light to 1 / that); the domes (the
sky's other lights, up to XGPU_RT_DOMES: the direction toward each and the
cosine of its half angle, then its radiance as the sun's and a 0); the
settings (XGPU_RT_SETTING_*) */
#define XGPU_RT_DOMES 4
#define XGPU_STREAM_RT_RIG_FLOATS 64u
enum
{
	XGPU_RT_RIG_SUN = 0,
	XGPU_RT_RIG_SKY = 4,
	XGPU_RT_RIG_DOMES = 8,
	XGPU_RT_RIG_SETTINGS = 8 + XGPU_RT_DOMES * 8,
};
/* the rig's settings (from rig + XGPU_RT_RIG_SETTINGS):
display.prism_rt_style (0 faithful, 1 remix), _anchor (0 to 1), _sun (0
objects, 1 all), _emissive (0 off, 1 lights, 2 all), _emissive_scale,
_materials (0 grey, 1 average), _bounces (0 by quality),
display.prism_exposure (0 fixed, 1 ce, 2 auto), _range, _speed, _bias
(EV, EV a second, EV), display.prism_tone_curve (0 ce, 1 filmic, 2 aces,
3 local), a lightmap texel's size (world units: the blur CE's own sun
shadows have), whether alpha-tested surfaces are traced
(display.prism_rt_alpha), whether the rig was fitted for the BSP (1) or is
the defaults (0), the share of the dynamic lights' light their traced
shadows take (0 to 1) */
enum
{
	XGPU_RT_SETTING_STYLE,
	XGPU_RT_SETTING_ANCHOR,
	XGPU_RT_SETTING_SUN,
	XGPU_RT_SETTING_EMISSIVE,
	XGPU_RT_SETTING_EMISSIVE_SCALE,
	XGPU_RT_SETTING_MATERIALS,
	XGPU_RT_SETTING_BOUNCES,
	XGPU_RT_SETTING_EXPOSURE,
	XGPU_RT_SETTING_EXPOSURE_RANGE,
	XGPU_RT_SETTING_EXPOSURE_SPEED,
	XGPU_RT_SETTING_EXPOSURE_BIAS,
	XGPU_RT_SETTING_TONE_CURVE,
	XGPU_RT_SETTING_PAGE_TEXEL,
	XGPU_RT_SETTING_ALPHA,
	XGPU_RT_SETTING_FITTED,
	XGPU_RT_SETTING_DYNAMIC_SHADOWS,
	XGPU_RT_SETTING_COUNT
};
enum
{
	XGPU_RT_OFF,
	XGPU_RT_SHADOWS,
	XGPU_RT_GI,
	XGPU_RT_PATHTRACED,
};
enum
{
	XGPU_RT_VIEW_OFF,
	XGPU_RT_VIEW_VISIBILITY,
	XGPU_RT_VIEW_AO,
	XGPU_RT_VIEW_GI,
	XGPU_RT_VIEW_REFLECTIONS,
	XGPU_RT_VIEW_ALBEDO,
	XGPU_RT_VIEW_NORMALS,
	XGPU_RT_VIEW_PROBES,
	XGPU_RT_VIEW_HEAT,
};
/* the frame's flags: display.prism_rt_cache false (every object skinned and
its acceleration structure built again each frame), display.prism_rt_objects
"low_lod" (the stock models traced, not the mesh store's replacements) */
enum
{
	XGPU_RT_FLAG_NO_CACHE = 1,
	XGPU_RT_FLAG_STOCK_MODELS = 2,
	/* the surfaces read the traced texel as a factor and a light added
	(xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT) in "shadows" and "gi" too */
	XGPU_RT_FLAG_LIGHT = 4,
	/* debug.pt_reference ("pathtraced" only): the path tracer unbiased (no
	reuse, no cache, no probes, no denoiser, up to XGPU_RT_REFERENCE_BOUNCES
	bounces), its samples summed over spare[0] frames at the camera and
	game time it began with, then written once as a linear OpenEXR file
	(xgpu_vulkan_rt.c) to the path at reference_file and held; spare[1] the
	request's generation (a new one starts the sum again) */
	XGPU_RT_FLAG_REFERENCE = 8,
	/* port: display.lighting "pathtraced" (PRISM v4 M3), the mode off: the
	moving objects' draws of the G-buffer pass captured and their
	acceleration structures built (xgpu_vulkan_rt.c's
	xgpu_vulkan_rt_objects_end) for the path tracer, nothing else traced */
	XGPU_RT_FLAG_OBJECTS = 16,
	/* display.lighting "raster" with display.prism_raster_object_shadows
	"traced" (mode XGPU_RT_OFF): the objects of the G-buffer pass captured
	and their structure built as XGPU_RT_FLAG_OBJECTS builds them, nothing
	traced by the RT backend itself; Raster's resolve traces the moving
	objects' sun shadows against it (32 here; prism-v4-raster-int had 16,
	which M3's objects took first: XGPU_STREAM_VERSION 37) */
	XGPU_RT_FLAG_RASTER_OBJECTS = 32,
	/* port: the game draws remodels this frame (display.prism_models: the
	mesh store answered a mesh of the set sent last), so its static scenery
	a mesh may draw is traced as drawn, not as the bake's placement
	(prism_bake_object_static); spare[3] the set's number (its 24 low bits),
	which the host pairs with its store's on the same frame
	(xgpu_vulkan_meshes_pair, HALO_GEOMETRY_LOG) */
	XGPU_RT_FLAG_REMODELS_DRAWN = 64,
};
#define XGPU_RT_REFERENCE_BOUNCES 32
/* the reference file's path: a NUL-terminated UTF-8 string of at most this
many bytes, its NUL included, in guest memory the guest keeps */
#define XGPU_RT_REFERENCE_PATH 512u
struct xgpu_stream_rt_frame
{
	uint32_t header;
	uint32_t mode, quality, resolution, denoiser;
	uint32_t reflections, objects, rate, mood;
	uint32_t view, light_count, frame, ticks;
	float fraction;
	uint32_t generation;
	/* XGPU_RT_FLAG_*; with XGPU_RT_FLAG_REFERENCE, the guest address of
	the reference file's path (XGPU_RT_REFERENCE_PATH), else 0 */
	uint32_t flags, reference_file;
	float settings[16];
	float sun[4];
	float sun_color[4];
	float sky[4];
	/* spare[0] and [1]: XGPU_RT_FLAG_REFERENCE's; spare[2]: the sun's
	angular diameter the baker takes (display.prism_sun_size "auto"'s, the
	tag's or the real sun's, whatever the setting: xgpu_vulkan_bake.c);
	spare[3]: XGPU_RT_FLAG_REMODELS_DRAWN's set number (port) */
	float spare[4];
	/* the light rig (XGPU_RT_RIG_*) */
	float rig[XGPU_STREAM_RT_RIG_FLOATS];
	float lights[XGPU_STREAM_RT_LIGHTS][12];
};

/* ---------- PRISM v4's scene records (docs/prism_scene_records.md)

The scene as the host's deferred renderer, its path tracer and its baker
read it (overhaul_research/prism_v4_renderer/design.md, section 1.2): the
materials, the objects drawn, the opaque draws of the G-buffer, the level's
geometry once for every consumer, the lights, the decals and the fog. They
are sent only in the v4 modes (display.lighting "ce_deferred", "raster" and
"pathtraced"); with "ce" (the default, Classic and Enhanced)
none is ever written, so those frames stay byte for byte as they were. Their
types are in the list above (XGPU_STREAM_VERSION 38: after GOAT H1's own) and the host's decoder
takes every one; M1 sends SCENE_MATERIAL and GBUFFER_DRAW (the guest's
xgpu_backend_scene_material and _gbuffer_draw) and SCENE_STATIC (also with
display.prism_rt on: the traced level is its), the others are specified and
taken, not sent yet. port/linux/src/xgpu_scene_encode.c fills each from
typed values (tools/test_scene_records.py checks the layouts and the
encoders).

Every record is fixed-width 4-byte fields, as the rest of the stream.
SCENE_INSTANCE's nodes, SCENE_LIGHTS' lights and SCENE_FOG's planes are
arrays at a record's end of which only the first count are sent: the
record's size is the fixed part and those, never the whole capacity. */


/* a shader's CE family (its tag group): environment, model, glass, water,
chicago, generic, meter, plasma */
enum
{
	XGPU_SCENE_FAMILY_SENV,
	XGPU_SCENE_FAMILY_SOSO,
	XGPU_SCENE_FAMILY_SGLA,
	XGPU_SCENE_FAMILY_SWAT,
	XGPU_SCENE_FAMILY_SCHI,
	XGPU_SCENE_FAMILY_SOTR,
	XGPU_SCENE_FAMILY_SMET,
	XGPU_SCENE_FAMILY_SPLA,
	XGPU_SCENE_FAMILIES,
};

/* SCENE_MATERIAL: a shader's material, once per shader per map as it loads
(and again when a mod changes it): its id (1 on, stable for the map; 0
none), the 64-bit FNV-1a hash of its tag's path (low word first; the host
resolves the PRISM material, mod, pack or auto, from it), its family
(XGPU_SCENE_FAMILY_*), CE's material type (the 33 classes), the shader's
own flags word as the tag has it, its detail functions (XGPU_SCENE_DETAIL_*:
8 bits each, CE's numbering), the flags the guest derives
(XGPU_SCENE_MATERIAL_FLAG_*), its bitmaps' texture ids by slot
(XGPU_SCENE_BITMAP_*; 0 none; bitmap_count the highest slot used plus one)
and its scalars by XGPU_SCENE_SCALAR_* (linear colours). Eight bitmap slots
are every bitmap a family's tag names that the material model reads;
thirty-two scalars are CE's numbers for them with room for the families'
odd ones. */
#define XGPU_SCENE_MATERIAL_BITMAPS 8u
#define XGPU_SCENE_MATERIAL_SCALARS 32u
enum
{
	XGPU_SCENE_BITMAP_BASE,
	XGPU_SCENE_BITMAP_PRIMARY_DETAIL,
	XGPU_SCENE_BITMAP_SECONDARY_DETAIL,
	XGPU_SCENE_BITMAP_MICRO_DETAIL,
	XGPU_SCENE_BITMAP_BUMP,
	/* senv's self-illumination map, soso's multipurpose map */
	XGPU_SCENE_BITMAP_MULTIPURPOSE,
	XGPU_SCENE_BITMAP_REFLECTION,
	/* sgla's tint map, swat's ripples */
	XGPU_SCENE_BITMAP_EXTRA,
};
/* the detail functions' word: primary (bits 0 to 7), secondary (8 to 15),
micro (16 to 23), each CE's (double-biased multiply, multiply,
double-biased add) */
#define XGPU_SCENE_DETAIL_PRIMARY_SHIFT 0
#define XGPU_SCENE_DETAIL_SECONDARY_SHIFT 8
#define XGPU_SCENE_DETAIL_MICRO_SHIFT 16
enum
{
	XGPU_SCENE_MATERIAL_FLAG_ALPHA_TESTED = 1,
	XGPU_SCENE_MATERIAL_FLAG_TWO_SIDED = 2,
	/* the specular mask is the bump map's alpha, not the base map's */
	XGPU_SCENE_MATERIAL_FLAG_BUMP_ALPHA_SPECULAR = 4,
	/* self-illumination animated (SCENE_MATERIAL_ANIMATE follows each frame) */
	XGPU_SCENE_MATERIAL_FLAG_ANIMATED = 8,
	/* a light card or lamp surface: radiosity power past zero */
	XGPU_SCENE_MATERIAL_FLAG_EMITTER = 16,
	/* the transparent layer's (water and glass) */
	XGPU_SCENE_MATERIAL_FLAG_TRANSPARENT_LAYER = 32,
	/* never an occluder of rays (shields, meters, plasma) */
	XGPU_SCENE_MATERIAL_FLAG_NO_OCCLUSION = 64,
};
enum
{
	XGPU_SCENE_SCALAR_BUMP_SCALE = 0,
	XGPU_SCENE_SCALAR_MAP_SCALE = 1,
	XGPU_SCENE_SCALAR_PRIMARY_DETAIL_SCALE = 2,
	XGPU_SCENE_SCALAR_SECONDARY_DETAIL_SCALE = 3,
	XGPU_SCENE_SCALAR_MICRO_DETAIL_SCALE = 4,
	XGPU_SCENE_SCALAR_SPECULAR_BRIGHTNESS = 5,
	XGPU_SCENE_SCALAR_PERPENDICULAR_COLOR = 6,     /* rgb */
	XGPU_SCENE_SCALAR_PARALLEL_COLOR = 9,          /* rgb */
	XGPU_SCENE_SCALAR_REFLECTION_PERPENDICULAR = 12,
	XGPU_SCENE_SCALAR_REFLECTION_PARALLEL = 13,
	XGPU_SCENE_SCALAR_SELF_ILLUMINATION_ON = 14,   /* rgb */
	XGPU_SCENE_SCALAR_SELF_ILLUMINATION_OFF = 17,  /* rgb */
	XGPU_SCENE_SCALAR_RADIOSITY_POWER = 20,
	XGPU_SCENE_SCALAR_RADIOSITY_COLOR = 21,        /* rgb */
	XGPU_SCENE_SCALAR_TRANSLUCENCY = 24,
	XGPU_SCENE_SCALAR_TINT = 25,                   /* rgb */
	XGPU_SCENE_SCALAR_ALPHA_REFERENCE = 28,
	/* 29 to 31 unused, 0 */
};
struct xgpu_stream_scene_material
{
	uint32_t header;
	uint32_t material;
	uint32_t path_hash[2];
	uint32_t family, material_type, shader_flags, detail_functions;
	uint32_t flags, bitmap_count;
	uint32_t bitmaps[XGPU_SCENE_MATERIAL_BITMAPS];
	float scalars[XGPU_SCENE_MATERIAL_SCALARS];
};

/* SCENE_MATERIAL_MAPS (PRISM v4 M4; docs/prism_materials.md): a material's
maps, where a pack or a mod gives its shader PRISM's material (a
materials\<shader tag>.pack.json or .material.json of format 2), after the
map's SCENE_MATERIALs and again as the texture store's answers change: its
id (SCENE_MATERIAL's; 0 refused); its tier (XGPU_GBUFFER_TIER_PACK or
_MOD; XGPU_GBUFFER_TIER_AUTO forgets the maps it had); its model
(XGPU_GBUFFER_MODEL_*); its flags (XGPU_SCENE_MAPS_FLAG_*, with the
environment shader's type and detail functions as CE's tag has them, the
guest's where the file says "ce"); its images by XGPU_SCENE_MAP_*, the
texture store's ids of KIND_MATERIAL entries (0 none); its numbers by
XGPU_SCENE_MAPS_SCALAR_* (the detail maps' scales CE's texture pass takes,
its base map's and its detail maps' sizes in them where the tag says to
rescale). The G-buffer pass draws the material's opaque draws with its
family's uber-shader, which samples them (xgpu_spirv.c's gbuffer_pack). */
#define XGPU_SCENE_MATERIAL_MAPS 8u
#define XGPU_SCENE_MATERIAL_MAP_SCALARS 24u
enum
{
	/* RGBA: the de-lit base colour (the colour space of CE's base map), and
	CE's base map's alpha */
	XGPU_SCENE_MAP_ALBEDO,
	/* RG: the shading normal in the surface's tangent space, x along u and
	y along v (CE's bump maps' convention), 0.5 flat */
	XGPU_SCENE_MAP_NORMAL,
	/* R occlusion, G roughness, B metalness (glTF's) */
	XGPU_SCENE_MAP_ORM,
	/* RGBA: an environment shader's detail maps, as CE's texture pass
	reads them */
	XGPU_SCENE_MAP_PRIMARY_DETAIL,
	XGPU_SCENE_MAP_SECONDARY_DETAIL,
	XGPU_SCENE_MAP_MICRO_DETAIL,
	/* RGB */
	XGPU_SCENE_MAP_EMISSIVE,
	/* R: the height, 0.5 the surface */
	XGPU_SCENE_MAP_HEIGHT,
};
enum
{
	/* the normal map is at CE's bump map's coordinates (the environment
	shader's bump map scale), not the base map's */
	XGPU_SCENE_MAPS_FLAG_NORMAL_AT_BUMP = 1,
	/* the material is two-sided foliage (its translucency scalar its
	diffuse transmission) */
	XGPU_SCENE_MAPS_FLAG_FOLIAGE = 2,
};
/* the model's value that keeps the automatic tier's (XGPU_GBUFFER_MODEL_*
the others) */
#define XGPU_SCENE_MAPS_MODEL_AUTO 15u
/* the environment shader's type (normal, blended, blended base specular),
detail function and micro detail function (CE's numbering: double-biased
multiply, multiply, double-biased add), two bits each */
#define XGPU_SCENE_MAPS_TYPE_SHIFT 4
#define XGPU_SCENE_MAPS_DETAIL_SHIFT 8
#define XGPU_SCENE_MAPS_MICRO_SHIFT 12
enum
{
	XGPU_SCENE_MAPS_SCALAR_ALBEDO = 0,            /* rgb, times the albedo */
	XGPU_SCENE_MAPS_SCALAR_NORMAL_SCALE = 3,      /* the normal's slope's scale */
	XGPU_SCENE_MAPS_SCALAR_ROUGHNESS = 4,         /* the ORM's factors (the values alone */
	XGPU_SCENE_MAPS_SCALAR_METAL = 5,             /* where it has none) */
	XGPU_SCENE_MAPS_SCALAR_OCCLUSION = 6,         /* the ORM occlusion's strength */
	XGPU_SCENE_MAPS_SCALAR_SPECULAR = 7,          /* F0's scale, 0.5 the dielectric 4% */
	XGPU_SCENE_MAPS_SCALAR_EMISSIVE = 8,          /* rgb, times the emissive map */
	XGPU_SCENE_MAPS_SCALAR_TRANSLUCENCY = 11,
	XGPU_SCENE_MAPS_SCALAR_PRIMARY_DETAIL = 12,   /* u, v: the detail map's scale over the base map's */
	XGPU_SCENE_MAPS_SCALAR_SECONDARY_DETAIL = 14, /* u, v */
	XGPU_SCENE_MAPS_SCALAR_MICRO_DETAIL = 16,     /* u, v */
	XGPU_SCENE_MAPS_SCALAR_HEIGHT_SCALE = 18,     /* world units the height's 0 to 1 spans */
	/* 19 to 23 unused, 0 */
};
struct xgpu_stream_scene_material_maps
{
	uint32_t header;
	uint32_t material, tier, model, flags;
	uint32_t maps[XGPU_SCENE_MATERIAL_MAPS];
	float scalars[XGPU_SCENE_MATERIAL_MAP_SCALARS];
};

/* SCENE_MATERIAL_ANIMATE: each frame, for each animated material drawn
(its FLAG_ANIMATED), the values CE computes for its draw: the
self-illumination's three channels' animation (primary, secondary, plasma;
0 to 1) and a 0; each bitmap slot's texture coordinates' transform this
frame (two rows of an affine map, u' = row 0 . (u, v, 0, 1), v' = row 1 .
(u, v, 0, 1); the identity where it does not move); the change colours not
tied to an object (linear rgb and a 0) */
#define XGPU_SCENE_CHANGE_COLORS 4u
struct xgpu_stream_scene_material_animate
{
	uint32_t header;
	uint32_t material;
	float self_illumination[4];
	float transforms[XGPU_SCENE_MATERIAL_BITMAPS][2][4];
	float change_colors[XGPU_SCENE_CHANGE_COLORS][4];
};

/* SCENE_INSTANCE: each frame, each object drawn: its id (stable across
frames: 1 + the object's datum index, as GBUFFER_DRAW's; never 0, the
level's), its flags
(XGPU_SCENE_INSTANCE_*), the level of detail the game chose (CE's, 0 super
low to 4 super high), the mesh store's mesh where one replaces the model (0
none), its change colours (linear rgb and a 0), and node_count nodes' world
matrices (three rows of four), this frame's then last frame's (with
NO_HISTORY, the same twice). CE's models have at most 64 nodes
(model_definitions.h's MAXIMUM_NODES_PER_MODEL); only node_count are sent. */
#define XGPU_SCENE_INSTANCE_NODES 64u
enum
{
	XGPU_SCENE_INSTANCE_FIRST_PERSON = 1,
	XGPU_SCENE_INSTANCE_CINEMATIC = 2,
	XGPU_SCENE_INSTANCE_SHIELD = 4,
	XGPU_SCENE_INSTANCE_CAMOUFLAGE = 8,
	XGPU_SCENE_INSTANCE_CASTS_SHADOW = 16,
	/* port (PRISM v4 M3): the object is in the bake's static scenery (and so
	in the scene the bake and the path tracer trace): display.lighting
	"pathtraced" leaves it out of its moving objects */
	XGPU_SCENE_INSTANCE_IN_TLAS = 32,
	/* first seen this frame: no last frame's matrices, no motion */
	XGPU_SCENE_INSTANCE_NO_HISTORY = 64,
};
struct xgpu_stream_scene_node
{
	float now[3][4];
	float last[3][4];
};
struct xgpu_stream_scene_instance
{
	uint32_t header;
	uint32_t instance, flags, lod, mesh, node_count;
	float change_colors[XGPU_SCENE_CHANGE_COLORS][4];
	struct xgpu_stream_scene_node nodes[XGPU_SCENE_INSTANCE_NODES];
};

/* GBUFFER_DRAW: before each opaque draw of walk 1, in place of CE's pass
state for it: the draw that follows (its vertices and indices bound as
today: mirror segments, the ring, the mesh store, the remodel's buffers;
CE's vertex program for skinning and texture coordinates) goes into the
G-buffer with this material and instance (0: the level). surface: the
level's surface index (SCENE_STATIC's) or, for an object, its geometry (high
16 bits) and part (low 16); vertex_program: the CE vertex program's id as
the pipelines name it; flags XGPU_SCENE_DRAW_* */
enum
{
	XGPU_SCENE_DRAW_ALPHA_TESTED = 1,
	XGPU_SCENE_DRAW_TWO_SIDED = 2,
	XGPU_SCENE_DRAW_FIRST_PERSON = 4,
	/* the remodel's geometry, not the stock BSP's */
	XGPU_SCENE_DRAW_REMODEL = 8,
};
struct xgpu_stream_gbuffer_draw
{
	uint32_t header;
	uint32_t material, instance, surface;
	uint32_t vertex_program, flags, lod;
	/* (M2) the level's draws: their lightmap page + 1 (the BSP's lightmap
	bitmap's index, SCENE_STATIC's page; 0 none, an object's), which the
	level's twins write with their lightmap coordinates (GBUFFER_BEGIN) */
	uint32_t lightmap;
};

/* SCENE_STATIC: the level's geometry as drawn (stock or remodel, the
remodel's near and far levels both), once for every consumer (the
acceleration structures, the baker, the G-buffer's shadows), a slice at a
time as RT_STATIC's: phase XGPU_SCENE_STATIC_BEGIN (a BSP's set begins:
generation names it, total its triangles, count its surfaces), _PAGES
(the BSP's lightmap pages: count pages from first, two words each, the
page's width and height in texels; total the pages), _TRIANGLES (count
triangles from first, 9 floats each: three corners' x y z in world units,
at data, size bytes), _ATTRIBUTES (count triangles' attributes from first,
XGPU_SCENE_STATIC_ATTRIBUTE_WORDS words each: XGPU_SCENE_ATTRIBUTE_*), _END
(the set is whole), _RELEASE (the map is gone, or no v4 mode or ray tracing
wants it), _LEVELS (the level of detail each surface is drawn at now: count
words from first of a bit set over the BSP's surfaces, total its words, a
bit set where the surface is drawn far; sent when it changes, after _END
and as the camera moves: the surfaces off screen by the same rule as the
drawn). At most XGPU_SCENE_STATIC_SLICE triangles (or pages, or words) a
record, never while a map loads. */
enum
{
	XGPU_SCENE_STATIC_BEGIN,
	XGPU_SCENE_STATIC_TRIANGLES,
	XGPU_SCENE_STATIC_ATTRIBUTES,
	XGPU_SCENE_STATIC_END,
	XGPU_SCENE_STATIC_RELEASE,
	XGPU_SCENE_STATIC_PAGES,
	XGPU_SCENE_STATIC_LEVELS,
	XGPU_SCENE_STATIC_PHASES,
};
#define XGPU_SCENE_STATIC_SLICE 65536u
#define XGPU_SCENE_STATIC_ATTRIBUTE_WORDS 16u
/* a triangle's attributes: its material (SCENE_MATERIAL's id; 0 none),
its surface (the BSP's surface index), its kind (XGPU_RT_TRIANGLE_*) in
bits 0 to 7, its level (XGPU_SCENE_LEVEL_*) in bits 8 and 9, bit 10
(XGPU_SCENE_INDOOR) where it is in one of the structure's indoor clusters
(a cluster with no sky, found from its middle a little off its face) and its
lightmap page in 16 to 31 (XGPU_SCENE_PAGE_NONE: no lightmap), its three
corners' lightmap coordinates (six floats, 0 to 1 across the page: the
baker's atlas is CE's pages scaled), their base map coordinates (six
floats), and display.prism_rt's own material (RT_STATIC's _MATERIALS index;
XGPU_SCENE_TRACED_NONE: not traced). A surface's triangles are consecutive,
its near level's before its far level's. */
enum
{
	XGPU_SCENE_ATTRIBUTE_MATERIAL = 0,
	XGPU_SCENE_ATTRIBUTE_SURFACE = 1,
	XGPU_SCENE_ATTRIBUTE_KIND_PAGE = 2,
	XGPU_SCENE_ATTRIBUTE_ATLAS = 3,
	XGPU_SCENE_ATTRIBUTE_BASE = 9,
	XGPU_SCENE_ATTRIBUTE_TRACED = 15,
};
/* a triangle's level: drawn whichever level its surface is drawn at (the
map's own triangles, or a remodel whose far level is its near one), its
near level's, or its far level's */
enum
{
	XGPU_SCENE_LEVEL_BOTH,
	XGPU_SCENE_LEVEL_NEAR,
	XGPU_SCENE_LEVEL_FAR,
};
#define XGPU_SCENE_LEVEL_SHIFT 8
#define XGPU_SCENE_INDOOR (1u << 10)
#define XGPU_SCENE_PAGE_SHIFT 16
#define XGPU_SCENE_PAGE_NONE 0xffffu
#define XGPU_SCENE_TRACED_NONE 0xffffffffu
struct xgpu_stream_scene_static
{
	uint32_t header;
	uint32_t phase, generation;
	uint32_t first, count, total;
	uint32_t data, size;
};

/* SCENE_LIGHTS: each frame, the light table (design section 1.6): the
sun, the domes and the sky (from the sky's lights and tag), the fixtures
and the dynamic lights; the emitters are triangles of SCENE_STATIC by their
materials. Lights first to first + count of the frame's total (a frame of
more than XGPU_SCENE_LIGHTS sends several records). A light: its id
(stable across frames), its type (XGPU_SCENE_LIGHT_*), its class
(XGPU_SCENE_LIGHT_CLASS_*), its flags (XGPU_SCENE_LIGHT_FLAG_*); position
(x y z and its radius; a sun's or dome's: the direction toward it and 0),
direction (a spot's axis and its cone's cosine, else 0 0 0 and 2), radiance
(linear rgb and its power, CE's colour by power), shape (a sun's angular
diameter in radians, a dome's half angle's cosine, a spot's inner cone's
cosine; then 0s). 256 a record holds every fixture and dynamic light a CE
map has drawn at once with room to spare. */
#define XGPU_SCENE_LIGHTS 256u
enum
{
	XGPU_SCENE_LIGHT_SUN,
	XGPU_SCENE_LIGHT_DOME,
	XGPU_SCENE_LIGHT_SKY,
	XGPU_SCENE_LIGHT_POINT,
	XGPU_SCENE_LIGHT_SPOT,
	/* a material's emitting triangles of SCENE_STATIC together (its id
	XGPU_SCENE_LIGHT_EMITTER_ID + its SCENE_MATERIAL id): position their
	middle and the radius about it that holds them, radiance CE's
	radiosity colour by power, shape their area */
	XGPU_SCENE_LIGHT_EMITTER,
	XGPU_SCENE_LIGHT_TYPES,
};
enum
{
	XGPU_SCENE_LIGHT_CLASS_STATIC,
	XGPU_SCENE_LIGHT_CLASS_ANIMATED,
	XGPU_SCENE_LIGHT_CLASS_SWITCHABLE,
	XGPU_SCENE_LIGHT_CLASS_DYNAMIC,
	XGPU_SCENE_LIGHT_CLASSES,
};
enum
{
	XGPU_SCENE_LIGHT_FLAG_ON = 1,
	XGPU_SCENE_LIGHT_FLAG_CASTS_SHADOWS = 2,
	XGPU_SCENE_LIGHT_FLAG_FIRST_PERSON = 4,
};
/* the lights' ids by their source: the sun, the sky, the sky's other
lights (domes: + the sky light's index), the emitters (+ the material's
id); a fixture's or a dynamic light's is its object's or light's datum
handle (never below 0x80000000: a handle's salt has its high bit) */
#define XGPU_SCENE_LIGHT_SUN_ID 1u
#define XGPU_SCENE_LIGHT_SKY_ID 2u
#define XGPU_SCENE_LIGHT_DOME_ID 16u
#define XGPU_SCENE_LIGHT_EMITTER_ID 0x10000u
struct xgpu_stream_scene_light
{
	uint32_t id, type, light_class, flags;
	float position[4];
	float direction[4];
	float radiance[4];
	float shape[4];
};
struct xgpu_stream_scene_lights
{
	uint32_t header;
	uint32_t first, count, total;
	struct xgpu_stream_scene_light lights[XGPU_SCENE_LIGHTS];
};

/* SCENE_DECAL: each live decal, projected into the G-buffer (design
section 1.7): its id (stable while it lives), its bitmap's texture id and
the bitmap's sequence frame, its blend (XGPU_SCENE_DECAL_BLEND_*, CE's
framebuffer blend functions in their order), its flags
(XGPU_SCENE_DECAL_FLAG_*), its box (the centre and three axes, each
scaled by its half extent: across, up, and out of the surface), its colour
(linear rgb and alpha) and its texture coordinates' rectangle in the bitmap
(u0 v0 u1 v1) */
enum
{
	XGPU_SCENE_DECAL_BLEND_ALPHA,
	XGPU_SCENE_DECAL_BLEND_MULTIPLY,
	XGPU_SCENE_DECAL_BLEND_DOUBLE_MULTIPLY,
	XGPU_SCENE_DECAL_BLEND_ADD,
	XGPU_SCENE_DECAL_BLEND_SUBTRACT,
	XGPU_SCENE_DECAL_BLEND_MIN,
	XGPU_SCENE_DECAL_BLEND_MAX,
	XGPU_SCENE_DECAL_BLEND_ALPHA_MULTIPLY_ADD,
	XGPU_SCENE_DECAL_BLENDS,
};
enum
{
	XGPU_SCENE_DECAL_FLAG_EMISSIVE = 1,
	XGPU_SCENE_DECAL_FLAG_NORMAL = 2,
};
struct xgpu_stream_scene_decal
{
	uint32_t header;
	uint32_t decal, bitmap, frame, blend, flags;
	float centre[4];
	float axes[3][4];
	float color[4];
	float rectangle[4];
};

/* SCENE_FOG: each frame, the fog as a screen pass over the G-buffer's
depth (design section 1.7): the camera's cluster's atmospheric fog (its
colour, linear, and maximum density; its start and opaque distances, then
0s), and the fog planes seen (count of XGPU_SCENE_FOG_PLANES): each its id
(the BSP's fog plane index), flags (XGPU_SCENE_FOG_FLAG_*), the plane (a
unit normal out of the fog and its distance: n . p = d), its colour and
maximum density, and its shape (opaque distance, opaque depth, then 0s).
Eight is more than any CE cluster sees at once. */
#define XGPU_SCENE_FOG_PLANES 8u
enum
{
	XGPU_SCENE_FOG_FLAG_ATMOSPHERE = 1,
	/* the camera is under the plane */
	XGPU_SCENE_FOG_FLAG_INSIDE = 2,
};
struct xgpu_stream_scene_fog_plane
{
	uint32_t id, flags, pad[2];
	float plane[4];
	float color[4];
	float shape[4];
};
struct xgpu_stream_scene_fog
{
	uint32_t header;
	uint32_t flags, plane_count, pad;
	float atmosphere_color[4];
	float atmosphere_range[4];
	struct xgpu_stream_scene_fog_plane planes[XGPU_SCENE_FOG_PLANES];
};

/* SCENE_VIEW: at each G-buffer pass's begin, the window's camera this frame
and last frame (as world AO's pass has it, xgpu_world_ao_pass's camera):
[0] the eye and the near plane, [1] the direction one unit along the view's
axis at the screen's middle and the far plane, [2] and [3] its change a
unit of the screen's x (-1 the view's left edge, 1 its right) and y (-1 its
top, 1 its bottom); the view's corners in the colour target's pixels (left,
top, right, bottom), this frame's and last's; flags XGPU_SCENE_VIEW_* */
enum
{
	/* last frame's camera given (the same window, a frame before) */
	XGPU_SCENE_VIEW_HISTORY = 1,
};
struct xgpu_stream_scene_view
{
	uint32_t header;
	uint32_t flags, pad[2];
	float now[4][4];
	float last[4][4];
	int32_t corners[4];
	int32_t last_corners[4];
};

/* PRISM_BAKE (M2, docs/prism_bake.md): the in-game baker's control and the
inputs only the game has, sent in the v4 modes that read a bake
(display.lighting "raster"; never with "ce"). Phase XGPU_BAKE_CONTROL each
frame: the map (its name and cache checksum) and the structure BSP drawn,
the SCENE_STATIC generation the bake is of, display.bake (mode), _quality,
the count of rebakes asked for (request: each change starts the BSP's bake
again from nothing), XGPU_BAKE_FLAG_*, the GPU time a frame may give the
bake (milliseconds), the bake folder's path (folder: a guest address of a
NUL-terminated UTF-8 string of at most XGPU_BAKE_PATH bytes, the guest's to
keep) and the status block the host writes (status: a guest address of
XGPU_BAKE_STATUS_WORDS uint32_t, XGPU_BAKE_STATUS_*, which the guest reads).
_REACH_INPUT (count words from first of total, data their bytes): what the
BSP's reachable space is found from, serialised (port/linux/game/
prism_bake.c's reach_serialise: the collision BSP, the clusters and PVS,
the water, the render triangles, the scenario's places and fighters), once
a generation after SCENE_STATIC's end; _REACH_END once it is whole. The host
finds the reach from it (prism_bake_reach.c, on a thread of its own: a large
BSP's walk is millions of cells, more than the guest's memory holds). */
enum
{
	XGPU_BAKE_CONTROL,
	XGPU_BAKE_REACH_INPUT,
	XGPU_BAKE_REACH_END,
	XGPU_BAKE_PHASES,
};
enum
{
	XGPU_BAKE_MODE_OFF,
	XGPU_BAKE_MODE_ASK,
	XGPU_BAKE_MODE_AUTO,
};
enum
{
	/* the menus are up (the game is not played): the bake may take the
	frame's GPU time whole (the control's budget) */
	XGPU_BAKE_FLAG_MENU = 1,
	/* debug.bake_exit: bake in the foreground (the budget's time a frame),
	the game quits once the BSP's bake is done */
	XGPU_BAKE_FLAG_FOREGROUND = 2,
	/* the reach's inputs were all sent */
	XGPU_BAKE_FLAG_REACH = 4,
	/* display.prism_world_geometry "remodel": the shipped bake looked for
	is the remodel's set (shipped's "remodel" folder), else the stock one's */
	XGPU_BAKE_FLAG_REMODEL = 8,
	/* port: the set was begun again in play (a setting changed: the world
	geometry, the lighting): nothing is baked for it; the bake of its key is
	loaded where there is a whole one, else the bake drawn is kept until the
	next load (prism_scene_static_in_play, xgpu_vulkan_bake.c) */
	XGPU_BAKE_FLAG_IN_PLAY = 16,
};
/* the bake file's maker (xgpu_vulkan_bake.c's BAKE_VERSION: a change makes
every bake stale), which the game's note of the BSPs baked holds too
(port/linux/game/prism_bake.c's prism_bake_key) */
#define XGPU_BAKE_VERSION 19u
#define XGPU_BAKE_PATH 512u
#define XGPU_BAKE_SLICE 65536u
#define XGPU_BAKE_NAME 64u
/* the status block (the host's, written as the bake goes; uint32_t each,
floats as their bits): the state (XGPU_BAKE_STATE_*), the BSP and the
generation it is of, the quality, progress (a float, 0 to 1, of the whole
bake), the step (XGPU_BAKE_STEP_*) and its progress (a float), texels done
and planned (millions, floats), rays a second (millions, a float), seconds
taken and seconds left (floats, the latter -1 unknown), the bake file's
bytes (MB, a float), whether the light drawn is the bake's
(XGPU_BAKE_SHOWN_*), a number that moves with each write, and where the
bake drawn is from (XGPU_BAKE_SOURCE_*: this machine's, or the release's
shipped one; the quality then the file's, at least the one asked) */
enum
{
	XGPU_BAKE_STATUS_STATE,
	XGPU_BAKE_STATUS_BSP,
	XGPU_BAKE_STATUS_GENERATION,
	XGPU_BAKE_STATUS_QUALITY,
	XGPU_BAKE_STATUS_PROGRESS,
	XGPU_BAKE_STATUS_STEP,
	XGPU_BAKE_STATUS_STEP_PROGRESS,
	XGPU_BAKE_STATUS_TEXELS_DONE,
	XGPU_BAKE_STATUS_TEXELS,
	XGPU_BAKE_STATUS_RAYS,
	XGPU_BAKE_STATUS_SECONDS,
	XGPU_BAKE_STATUS_SECONDS_LEFT,
	XGPU_BAKE_STATUS_MEGABYTES,
	XGPU_BAKE_STATUS_SHOWN,
	XGPU_BAKE_STATUS_SERIAL,
	XGPU_BAKE_STATUS_SOURCE,
	XGPU_BAKE_STATUS_WORDS = 16,
};
enum
{
	XGPU_BAKE_SOURCE_HERE, /* baked (or being baked) on this machine */
	XGPU_BAKE_SOURCE_SHIPPED, /* the release's shipped bake (display.bake_shipped_folder) */
};
enum
{
	XGPU_BAKE_STATE_NONE, /* no bake (off, no ray queries, nothing sent) */
	XGPU_BAKE_STATE_WAITING, /* for the level's geometry, the reach or ray tracing's scene */
	XGPU_BAKE_STATE_BAKING,
	XGPU_BAKE_STATE_PAUSED, /* display.bake "ask" and not asked, or "off" with a bake part done */
	XGPU_BAKE_STATE_DONE,
	XGPU_BAKE_STATE_FAILED, /* (the log says why) */
};
enum
{
	XGPU_BAKE_STEP_PLAN,
	XGPU_BAKE_STEP_IMPORTANCE,
	XGPU_BAKE_STEP_DRAFT,
	XGPU_BAKE_STEP_LIGHTMAP,
	XGPU_BAKE_STEP_SUN_MASK,
	XGPU_BAKE_STEP_VOLUME,
	XGPU_BAKE_STEP_PROBES,
	XGPU_BAKE_STEP_FOOTPRINTS,
	XGPU_BAKE_STEP_DONE,
};
enum
{
	XGPU_BAKE_SHOWN_NONE, /* CE's light */
	XGPU_BAKE_SHOWN_DRAFT,
	XGPU_BAKE_SHOWN_PARTIAL, /* the finished tiles, the draft elsewhere */
	XGPU_BAKE_SHOWN_FINAL,
	XGPU_BAKE_SHOWN_STALE, /* a bake of something else (its key differs), until the new one is done */
};
struct xgpu_stream_prism_bake
{
	uint32_t header;
	uint32_t phase, generation;
	uint32_t first, count, total;
	uint32_t data, size;
	uint32_t mode, quality, request, flags;
	uint32_t map_checksum;
	int32_t bsp_index;
	uint32_t folder, status;
	float budget_milliseconds, threshold;
	/* (the `max` tier's noise floor: the share of middle grey (0.18) below
	which a texel's error is measured against it, not its own light; 0 the
	literal rule) */
	float noise_floor;
	/* (the shipped bake's folder, a release's light: a guest address of a
	NUL-terminated UTF-8 string of at most XGPU_BAKE_PATH bytes, as folder;
	0 none. Read only: <shipped>/<stock|remodel>/<map>/bsp<n>.pbk, used
	before the bake folder's wherever its key is the BSP's (docs/
	prism_bake.md, "The shipped bake")) */
	uint32_t shipped;
	char map_name[XGPU_BAKE_NAME];
};

/* PRISM_TERRAIN (the terrain, weather and water overhaul, T0;
docs/prism_terrain.md): what of the ground, the weather and the water only
the game knows, sent in the PRISM modes (display.lighting "raster" or
"pathtraced") of a campaign (no game engine runs) with display.terrain
"on"; never with "ce", never in multiplayer. Nothing the host makes of it
comes back to the game. Every record is the fixed part below and count
items of 32 bytes after it (the record's size: offsetof(items) + count *
32), by phase:
- XGPU_TERRAIN_FRAME, once a frame before the windows (count 0): the
  generation (it moves on with a new map, a BSP switch, a revert to a
  checkpoint or a new game: the host forgets every dynamic thing then), the
  game's time step in seconds (0 while it is paused), real seconds since
  the last FRAME, the first local player's camera, the BSP, the settings
  (XGPU_TERRAIN_FLAG_*, display.terrain_quality XGPU_TERRAIN_QUALITY_*),
  the camera's weather palette entry (NONE: -1), whether it has a
  particle system (weather_type 1, 0 none) and its scale, the wind at the
  eye (scenario_get_wind: world units a second) and each palette entry's
  wind (at most XGPU_TERRAIN_WINDS, at the eye; wind_count of them), the
  map's cache checksum;
- XGPU_TERRAIN_EVENTS: the frame's events, xgpu_stream_terrain_event each
  (at most XGPU_TERRAIN_EVENTS_MOST): a foot's step (its kind, the side,
  the contact marker's place and facing, the ground under it: the
  footstep's ray's hit, its normal and CE's material type, 255 where the
  ray found none), a vehicle's mass point on the ground (its contact,
  ground normal, material, the motion's direction and CE's speed scale), a
  projectile's impact and detonation, an explosion (its centre and the
  damage's cutoff radius); sub-steps and co-op prediction may send one
  twice: what the host makes of each is the same made twice;
- XGPU_TERRAIN_WADERS: the units within XGPU_TERRAIN_WADER_RANGE world
  units of the eye that are in water or left it in the last 10 seconds,
  xgpu_stream_terrain_wader each (at most XGPU_TERRAIN_WADERS_MOST);
- XGPU_TERRAIN_WEATHER_DRAW (count 0): where the host draws the weather
  in CE's place (display.weather "prism": not drawn yet, T12; never sent);
- XGPU_TERRAIN_WATER_DRAW (T7, version 42; count XGPU_TERRAIN_WATER_ITEMS: one
  xgpu_stream_terrain_water, 96 bytes): a CE water group drawn by the host in CE's
  place (display.water "prism"), in steps about the group's draws in
  rasterizer_water_draw: BEGIN (before them: the colour and the depth as
  they are, what is behind the water), CAPTURED (the water's base map drawn
  into the colour target at its pixels: CE's colour of the water), END (its
  ripple map drawn there at CE's own coordinates, then CE's depth: the host
  composites the water layer over its pixels, what CE's draws left elsewhere
  put back).
The fixed part's frame fields are FRAME's; the other phases send the
generation and leave them 0. */
enum
{
	XGPU_TERRAIN_FRAME,
	XGPU_TERRAIN_EVENTS,
	XGPU_TERRAIN_WADERS,
	XGPU_TERRAIN_WEATHER_DRAW,
	XGPU_TERRAIN_WATER_DRAW,
	XGPU_TERRAIN_PHASES,
};
#define XGPU_TERRAIN_EVENTS_MOST 256u
#define XGPU_TERRAIN_WATER_ITEMS 3u
#define XGPU_TERRAIN_WADERS_MOST 64u
#define XGPU_TERRAIN_WINDS 8u
#define XGPU_TERRAIN_WADER_RANGE 25.0f
#define XGPU_TERRAIN_PATH 1024u
/* FRAME's flags: the mode, campaign, the settings (display.terrain_tracks,
_drifts, _layers "hq", display.weather and display.water "prism"), the
game paused, and the debug views (debug.gbuffer_view "terrain_class",
debug.terrain_events) */
enum
{
	XGPU_TERRAIN_FLAG_CAMPAIGN = 1u << 0,
	XGPU_TERRAIN_FLAG_RASTER = 1u << 1,
	XGPU_TERRAIN_FLAG_PATHTRACED = 1u << 2,
	XGPU_TERRAIN_FLAG_TRACKS = 1u << 3,
	XGPU_TERRAIN_FLAG_DRIFTS = 1u << 4,
	XGPU_TERRAIN_FLAG_LAYERS_HQ = 1u << 5,
	XGPU_TERRAIN_FLAG_WEATHER = 1u << 6,
	XGPU_TERRAIN_FLAG_WATER = 1u << 7,
	XGPU_TERRAIN_FLAG_PAUSED = 1u << 8,
	XGPU_TERRAIN_FLAG_VIEW_CLASS = 1u << 16,
	XGPU_TERRAIN_FLAG_VIEW_EVENTS = 1u << 17,
};
enum
{
	XGPU_TERRAIN_QUALITY_AUTO,
	XGPU_TERRAIN_QUALITY_LOW,
	XGPU_TERRAIN_QUALITY_MEDIUM,
	XGPU_TERRAIN_QUALITY_HIGH,
	XGPU_TERRAIN_QUALITY_ULTRA,
};
/* an event's kind (its kind word's bits 0 to 7): a biped's foot (CE's
material effects of a biped: walk, run, sliding, shuffle, jump, jump's
landing), a vehicle's mass point on the ground (a tyre, a track, a hover),
a projectile's impact on a surface and its detonation, an explosion's
damage. The kind word's bits 8 to 15 are flags (XGPU_TERRAIN_EVENT_RIGHT:
the right foot), 16 to 23 the ground's material type (CE's, as
SCENE_MATERIAL's; 255 none known), 24 to 31 a biped's species
(XGPU_TERRAIN_SPECIES_*, 0 another) */
enum
{
	XGPU_TERRAIN_EVENT_NONE,
	XGPU_TERRAIN_EVENT_WALK,
	XGPU_TERRAIN_EVENT_RUN,
	XGPU_TERRAIN_EVENT_SLIDE,
	XGPU_TERRAIN_EVENT_SHUFFLE,
	XGPU_TERRAIN_EVENT_JUMP,
	XGPU_TERRAIN_EVENT_LAND,
	XGPU_TERRAIN_EVENT_CONTACT,
	XGPU_TERRAIN_EVENT_IMPACT,
	XGPU_TERRAIN_EVENT_DETONATION,
	XGPU_TERRAIN_EVENT_EXPLOSION,
	XGPU_TERRAIN_EVENT_KINDS,
};
#define XGPU_TERRAIN_EVENT_RIGHT 0x100u
#define XGPU_TERRAIN_MATERIAL_UNKNOWN 255u
enum
{
	XGPU_TERRAIN_SPECIES_OTHER,
	XGPU_TERRAIN_SPECIES_HUMAN,
	XGPU_TERRAIN_SPECIES_ELITE,
	XGPU_TERRAIN_SPECIES_GRUNT,
	XGPU_TERRAIN_SPECIES_JACKAL,
	XGPU_TERRAIN_SPECIES_HUNTER,
	XGPU_TERRAIN_SPECIES_FLOOD,
	XGPU_TERRAIN_SPECIES_SENTINEL,
};
/* an event: its kind word (above), its place (a foot's: the ground's hit,
or the contact marker's where the ray found none), the ground's normal and
the facing (a foot's marker's forward; a mass point's motion; a
projectile's: its velocity), each octahedral, 2x16 snorm (0 is straight up), the
magnitude (a foot's 1; a mass point's speed scale, 0 to 1, CE's; an
explosion's cutoff radius, world units; a projectile's 0), and the object
(its datum index; NONE: 0xffffffff) */
struct xgpu_stream_terrain_event
{
	uint32_t kind;
	float position[3];
	uint32_t normal, facing;
	float magnitude;
	uint32_t object;
};
/* a wader: its flags (bits 0 to 7: XGPU_TERRAIN_WADER_*) and the seconds
since it left the water (bits 16 to 31, 1/64 s each, 0 while in it), its
centre, velocity (world units a second) and bounding radius */
enum
{
	XGPU_TERRAIN_WADER_PARTIALLY = 1u << 0,
	XGPU_TERRAIN_WADER_WHOLLY = 1u << 1,
	XGPU_TERRAIN_WADER_BIPED = 1u << 2,
	XGPU_TERRAIN_WADER_VEHICLE = 1u << 3,
};
struct xgpu_stream_terrain_wader
{
	uint32_t kind;
	float position[3];
	float velocity[3];
	float radius;
};
/* WATER_DRAW's step, its flags (bits 0 to 3 CE's water shader's: the base
map's alpha modulates the reflection, its colour the background, the
atmospheric fog, drawn before the fog; XGPU_TERRAIN_WATER_BASE: the
CAPTURED step's base colour was drawn, XGPU_TERRAIN_WATER_SKY: a sky's
water), the group's plane (its normal and its distance: CE's, the side the
water's surface faces), the group's number this frame, and CE's tints
(rgb and alpha) seen straight on (perpendicular) and edge on (parallel);
the window's planar fog (CE's colour, its maximum density: 0 none) and its
shape (its maximum depth and distance, the eye's height over its plane) */
enum
{
	XGPU_TERRAIN_WATER_BEGIN,
	XGPU_TERRAIN_WATER_CAPTURED,
	XGPU_TERRAIN_WATER_END,
	XGPU_TERRAIN_WATER_STEPS,
};
enum
{
	XGPU_TERRAIN_WATER_ALPHA_MODULATES_REFLECTION = 1u << 0,
	XGPU_TERRAIN_WATER_COLOR_MODULATES_BACKGROUND = 1u << 1,
	XGPU_TERRAIN_WATER_ATMOSPHERIC_FOG = 1u << 2,
	XGPU_TERRAIN_WATER_BEFORE_FOG = 1u << 3,
	XGPU_TERRAIN_WATER_BASE = 1u << 8,
	XGPU_TERRAIN_WATER_SKY = 1u << 9,
};
struct xgpu_stream_terrain_water
{
	uint32_t step, flags;
	float plane[4];
	uint32_t group;
	float ripple_scale;
	float perpendicular[4];
	float parallel[4];
	float fog[4];
	float fog_shape[4];
};
struct xgpu_stream_prism_terrain
{
	uint32_t header;
	uint32_t phase, generation, count;
	float game_seconds, real_seconds;
	float camera[3];
	int32_t bsp_index;
	uint32_t flags, quality;
	int32_t weather_entry;
	uint32_t weather_type;
	float weather_scale;
	float wind[3];
	uint32_t wind_count;
	float winds[XGPU_TERRAIN_WINDS][3];
	uint32_t map_checksum;
	/* (T1, version 41) the terrain pack's folder (display.terrain_pack_folder;
	<data>/prism_terrain): a guest address of a NUL-terminated UTF-8 string of
	at most XGPU_TERRAIN_PATH bytes, the guest's to keep; 0 none. FRAME's */
	uint32_t pack_folder;
	struct xgpu_stream_terrain_event items[];
};

/* ---------- display.ambient_occlusion "ray_traced" (docs/world-ao.md,
"Hardware ray queries")

The scene the occlusion's pass traces its rays through with the GPU's ray
queries, where the backend has them (xgpu_stream_capabilities' ray_queries):
the dynamic objects' occlusion is then found by the hardware's rays against
their collision meshes, the structure's collision mesh stopping them, in
place of the distance-field scene's step (the pass's history[3] 2).

WORLD_AO_MESH: a mesh (a bottom-level acceleration structure, built once):
its id (1 to XGPU_STREAM_WORLD_AO_MESHES - 1) and count triangles, nine
floats each (each corner's x, y, z in the mesh's own space, the front faces
counterclockwise seen from outside), at data (size: count * 36); id 0 lets
go of every mesh (a map's end). An id is given once a map */
#define XGPU_STREAM_WORLD_AO_MESHES 4096u
#define XGPU_STREAM_WORLD_AO_MESH_TRIANGLES (1u << 20)
struct xgpu_stream_world_ao_mesh
{
	uint32_t header;
	uint32_t id, count;
	uint32_t data, size;
	uint32_t pad[3];
};

/* WORLD_AO_INSTANCES: the frame's instances of the meshes (a top-level
acceleration structure of them, built before the next pass), count of
them at data (size: count * sizeof(struct xgpu_world_ao_instance)): each
its transform (a row-major 3x4, its mesh's space into the world), its
mesh's id, its owner (its object's datum index plus one, the instance's
custom index: a pixel of that object's own leaves it out; 0 the structure,
which stops a ray and occludes nothing: the lightmaps and the field hold
its occlusion) and its kind (XGPU_WORLD_AO_INSTANCE_*, the instance's mask:
the structure's facing either way, an object's front faces alone) */
#define XGPU_STREAM_WORLD_AO_INSTANCES_MAXIMUM 4096u
#define XGPU_WORLD_AO_INSTANCE_STRUCTURE 1u
#define XGPU_WORLD_AO_INSTANCE_OBJECT 2u
#define XGPU_WORLD_AO_INSTANCE_OWNERS (1u << 24)
struct xgpu_world_ao_instance
{
	float transform[3][4];
	uint32_t mesh, owner, kind, pad;
};
struct xgpu_stream_world_ao_instances
{
	uint32_t header;
	uint32_t count;
	uint32_t data, size;
};

/* UPSCALE (M5, docs/prism_plugin.md): display.upscaler's frame upscaling of
a player's window's 3D view, and display.denoiser's plugin. The view is drawn
smaller (display.render_scale) into the window's targets of their own at
that scale, the HUD and the menus over it at the screen's. Phase
XGPU_UPSCALE_BEGIN as the view's drawing begins (the view's targets named:
the backend jitters the draws of the view's size from here on, by a sequence
of its own); _OPAQUE once its opaque surfaces are drawn and before its
transparent ones (the view's colour kept, which the reactive mask is found
from); _END once the view is whole and tone mapped, before the HUD (the
view upscaled into the output's targets, its colour and its depth, and the
jitter stopped). The targets: the view's colour and depth (view, view_depth)
and the output's (output, output_depth), each a TARGET_CREATE's number; the
corners (left, top, right, bottom) of the window's view in the view's
targets' pixels and in the output's. upscaler: XGPU_UPSCALE_UPSCALER_*;
denoiser: XGPU_UPSCALE_DENOISER_*; flags: XGPU_UPSCALE_FLAG_*. END's camera
this frame and last (SCENE_VIEW's layout: the eye and the near plane, the
axis at the screen's middle and the far plane, its change a unit of the
screen's x and y), sharpness (0 to 1), the frame's time (milliseconds) */
enum
{
	XGPU_UPSCALE_BEGIN,
	XGPU_UPSCALE_OPAQUE,
	XGPU_UPSCALE_END,
	XGPU_UPSCALE_PHASES,
};
enum
{
	/* (none: drawn at the screen's scale, not upscaled, the denoiser's alone) */
	XGPU_UPSCALE_UPSCALER_NONE,
	/* FidelityFX Super Resolution 3.1, the renderer's own */
	XGPU_UPSCALE_UPSCALER_FSR3,
	/* the plugin named as the game started (display.upscaler "plugin:<name>") */
	XGPU_UPSCALE_UPSCALER_PLUGIN,
	XGPU_UPSCALE_UPSCALERS,
};
enum
{
	/* the path tracer's own filter (display.prism_rt_denoiser's) */
	XGPU_UPSCALE_DENOISER_PRISM,
	/* the plugin named as the game started (display.denoiser "plugin:<name>") */
	XGPU_UPSCALE_DENOISER_PLUGIN,
	/* none (the traced light as it is: a diagnostic) */
	XGPU_UPSCALE_DENOISER_OFF,
	XGPU_UPSCALE_DENOISERS,
};
enum
{
	/* last frame's camera given, and the same view a frame before: the
	upscaler's history kept */
	XGPU_UPSCALE_FLAG_HISTORY = 1,
};
struct xgpu_stream_upscale
{
	uint32_t header;
	uint32_t phase, upscaler, denoiser, flags;
	uint32_t view, view_depth, output, output_depth;
	int32_t view_corners[4];
	int32_t output_corners[4];
	float now[4][4];
	float last[4][4];
	float sharpness, frame_milliseconds, pad[2];
};

/* ---------- the layouts' sizes, alike on both sides */

#define XGPU_STREAM_ASSERT_SIZE(type, size) _Static_assert(sizeof(type) == (size), #type "'s size")

XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_ring, 124);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_frame_end, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_capabilities, 132);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_rt_static, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_rt_frame, 4 + 32 * 4 + 16 * 4 + XGPU_STREAM_RT_RIG_FLOATS * 4 +
	XGPU_STREAM_RT_LIGHTS * 48);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_material_overlay, 4 + 320 + 8 * 72);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_texture_store_map, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_texture_store_query, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_texture_store_answer, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mesh_store_map, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mesh_store_query, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mesh_store_answer, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mesh_instance, 48);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mesh_draw, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_material_define, 12 + 6 * 4 + 9 * 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_hdr, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_tone_map, 28);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_panini, 56);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_window, 16 + 32 + 32 + XGPU_WORLD_AO_PROXIES_MAXIMUM * 48 + 32 +
	XGPU_WORLD_AO_GRID_CELLS_MAXIMUM * 8);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_field, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_prepass, 96 + 64 + 64 + 16 + 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_cascades, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_mesh, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_ao_instance, 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_instances, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_prism_shadows_block, 4 * 64 + 8 * 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_prism_shadows, 16 + 4 * 64 + 8 * 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_stand_in, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_set_targets, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_string, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_view, 72);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_owner, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_clear, 36);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_read_target, 28);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_screenshot, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_sampler, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_texture_stage, 72);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pixel_key, 268);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_key, 312);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pixel_program, 440);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_define, 760);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_state, 68);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_draw_uniforms, 588);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertex_element, 6);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_layout_define, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertex_stream, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertices, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_draw_immediate, 28);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_block, 6192);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_halos, 192);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_world, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_info, 108);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_eyes, 36 + 4 * 52);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_eye_transforms, 8 + 4 * 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_screen_corrections, 4 + 4 * 14);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_frame_record, 44);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_submit, 44 + 8 + 4 * (116 + 8));
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_state, 4 * (2 + 4 + 32 + 3 + 3 + 3 + 1 + 34 + 19 + 1 + 8 + 50 + 80 + 71 + 3 + 39));

XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_material, 40 + 4 * XGPU_SCENE_MATERIAL_BITMAPS +
	4 * XGPU_SCENE_MATERIAL_SCALARS);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_material_animate, 24 + XGPU_SCENE_MATERIAL_BITMAPS * 32 +
	XGPU_SCENE_CHANGE_COLORS * 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_node, 96);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_instance, 24 + XGPU_SCENE_CHANGE_COLORS * 16 +
	XGPU_SCENE_INSTANCE_NODES * 96);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_gbuffer_draw, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_static, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_light, 80);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_lights, 16 + XGPU_SCENE_LIGHTS * 80);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_decal, 120);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_fog_plane, 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_fog, 48 + XGPU_SCENE_FOG_PLANES * 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_view, 16 + 128 + 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_prism_bake, 80 + XGPU_BAKE_NAME);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_scene_material_maps, 20 + 4 * XGPU_SCENE_MATERIAL_MAPS +
	4 * XGPU_SCENE_MATERIAL_MAP_SCALARS);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_upscale, 36 + 32 + 128 + 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_terrain_event, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_terrain_wader, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_prism_terrain, 84 + 12 * XGPU_TERRAIN_WINDS);
/* (the scene records' types still fit the header's 8 bits) */
_Static_assert(XGPU_STREAM_SCENE_RECORD_TYPES <= 256, "the scene records' types fit a header");

#endif
