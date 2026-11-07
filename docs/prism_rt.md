# PRISM ray tracing (`display.prism_rt`)

Hardware ray tracing for the Vulkan renderer, off by default. It follows the design in the PRISM overhaul's
`raytracing_design.md`; this page says what is built and how it fits the frame.

## Principles

- **Off is off.** With `display.prism_rt = "off"` (the default, and the Classic preset's) the game sends no record
  and no shader changes: the frames are byte for byte the base build's (checked at b30 with fixed step, 46 of 46
  frames, and again with the ray tracing extensions enabled against `HALO_VULKAN_NO_RT=1`). The device enables
  `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations` (with buffer device
  addresses) where present; its buffers and pipelines are made with the first frame that traces, never for a game
  that does not.
- **Client-side and render-only.** The game reads only the camera's sky, the structure's render geometry (read-only
  once loaded) and the settings; nothing is simulated, saved or sent between machines.
- **CE's light is the reference.** The traced terms only take light away, through world AO's occlusion slot (the
  light the occlusion multiplies: a lightmap pass's lightmap, a model's ambient and distant lights), never add it:
  - the sun's shadows take `0.65` of a sunlit lightmap's light (`0.55` of a model's) where an object blocks the sun
    and the structure does not (the structure's own shadows are in the lightmap: never darkened again), weighted by
    `smoothstep(0, 0.25, N·sun)` (a face turned away has no sun to lose): world2's cascaded-shadow combine;
  - the occlusion follows world AO's rules: the level is darkened only by objects (2 wu, 0.7 strength), objects by
    objects (their own creases among them, 1 wu) and by the structure close by (0.5 wu), at 0.6 strength;
  - `gi`: an object's light is shaded by the probes' indirect light along its normal against their mean over the
    sphere, so its brightness stays CE's and only how the light falls across it changes, within 1/k to k
    (k = 1.25 at `display.prism_rt_mood` 100, 1 at 0); the level's light is untouched (its bounce is in the lightmap);
  - no auto-exposure, no grade.

## The frame

```
render_frame ─► world_ao_frame_begin ─► prism_rt_frame_begin: the BSP's triangles (once, a slice a frame: RT_STATIC),
                                        the sun, sky, lights and settings (RT_FRAME)
render_window ─► world AO's pre-pass (forced on while prism_rt is): each opaque draw's twin writes its pixel's
                 normal, distance and flags; the backend captures each object draw as drawn (xgpu_vulkan_rt_capture:
                 the stock vertex layout skinned on the CPU by the draw's own node matrices)
                 PREPASS_END ─► objects' BLAS (fast build) ─► TLAS (structure mask 1, objects mask 4)
                             ─► gi: a quarter of the probes updated (DDGI-like, L1 spherical harmonics a channel)
                             ─► trace (compute, ray queries): sun rays to objects and structure, occlusion rays,
                                 gi's probes read at an object's pixel
                             ─► resolve (compute): 5x5 cross-bilateral, temporal blend by the last camera
                             ─► the surfaces read it in world AO's place (xgpu_vulkan_world_ao_set_pixels)
```

Files: `port/linux/game/prism_rt.c` (the game's half), `port/linux/vulkan/xgpu_vulkan_rt.c` (acceleration structures,
capture, passes), `xgpu_vulkan_rt_glsl.inc` (the compute shaders; `tools/vulkan_rt_shaders.py` compiles them to
`xgpu_vulkan_rt_shaders.c`), records `RT_STATIC` and `RT_FRAME` in `xgpu_stream.h`.

## What is traced

- **The structure:** its render geometry's opaque environment surfaces (not alpha-tested, transparent or water), in
  world space, one bottom-level structure built once (`PREFER_FAST_TRACE`) when a BSP is first drawn.
- **The objects:** every pre-pass draw with the world's light (characters, vehicles, weapons, scenery), and its
  occluders (models lighting themselves, the Elites' armour), as drawn: their
  permutations, levels of detail and pose. All of a window's go in one bottom-level structure built each frame. The
  first-person weapon (an occluder all within 1 wu of the eye) is left out. With `display.prism_rt` on, a model whose
  pass adds self-illumination (the Elites) takes the world's light too (its occlusion slot is its vertex light's,
  before the self-illumination is added), so it receives the traced shadows and occlusion; world AO alone still leaves
  it out. Draws whose base map is a cutout (a fifth of
  its texels under half alpha: foliage) are left out until the traces test alpha; a mesh-store mesh is traced as its
  stock part (the pre-pass draws the stock twin).
- Off-screen objects are not in the structure (they are captured from the pre-pass), so a tree behind the camera casts
  no traced shadow; off-screen structure does.

## GI's probes

A volume of probes about the camera (`medium` 24x12x24 at 1.5 wu, `low` 16x8x16 at 2, `high` 32x16x32 at 1.5, `ultra`
32x16x32 at 1), toroidal, a quarter updated a frame with 64 to 256 rays each (spherical Fibonacci, turned each frame),
blended at 0.9. A ray that leaves the level takes the sky's outdoor ambient radiosity (the sky tag's colour times
power); one that hits takes `0.3 ×` (the sun at the hit, if it sees it, plus the last probes' irradiance there):
indirect light only, as CE's own model light carries the sun directly. The hit's face normal is read from the
triangles through buffer device addresses. A probe that sees the back of the level's one-sided faces with a quarter of
its rays or more is inside it and not read. The probes hold no CE lightmap yet (the design's radiance pages): their
light is the sky's and the sun's bounce, which the per-pixel ratio keeps from changing CE's brightness.

## Settings

`display.prism_rt` (`off` / `shadows` / `gi` / `pathtraced`; `pathtraced` draws as `gi` until built),
`display.prism_rt_quality` (rays a pixel), `display.prism_rt_resolution`, `display.prism_rt_denoiser`,
`display.prism_rt_reflections`, `display.prism_rt_mood`, `display.prism_rt_objects`, `display.prism_rt_rate`,
`debug.prism_rt_view`, `HALO_VULKAN_NO_RT=1`. Settings > Mods has Ray Tracing and its quality, greyed out where the
renderer cannot trace. `HALO_VULKAN_STATS=1` logs the passes' GPU time every 600 frames.

## Not yet

Colour in GI and the dynamic lights' bounce on the level (both need the surfaces' shaders to add a traced colour, a new
binding and key bit), CE's lightmap radiance at probe hits, local lights' shadows, reflections, path tracing,
alpha-tested traces, remodel relief, the compaction of the structure's BLAS, Deck and RDNA tuning, Metal.
