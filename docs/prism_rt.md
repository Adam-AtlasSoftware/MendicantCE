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
  - the occlusion follows world AO's rules: the level is darkened only by objects (2 wu), objects by objects (their
    own creases among them, 1.5 wu) and by the structure close by (0.5 wu), floored at 0.2;
  - no auto-exposure, no grade.

## The frame

```
render_frame ─► world_ao_frame_begin ─► prism_rt_frame_begin: the BSP's triangles (once, a slice a frame: RT_STATIC),
                                        the sun, sky, lights and settings (RT_FRAME)
render_window ─► world AO's pre-pass (forced on while prism_rt is): each opaque draw's twin writes its pixel's
                 normal, distance and flags; the backend captures each object draw as drawn (xgpu_vulkan_rt_capture:
                 the stock vertex layout skinned on the CPU by the draw's own node matrices)
                 PREPASS_END ─► objects' BLAS (fast build) ─► TLAS (structure mask 1, objects mask 4)
                             ─► trace (compute, ray queries): sun rays to objects and structure, occlusion rays
                             ─► resolve (compute): 5x5 cross-bilateral, temporal blend by the last camera
                             ─► the surfaces read it in world AO's place (xgpu_vulkan_world_ao_set_pixels)
```

Files: `port/linux/game/prism_rt.c` (the game's half), `port/linux/vulkan/xgpu_vulkan_rt.c` (acceleration structures,
capture, passes), `xgpu_vulkan_rt_glsl.inc` (the compute shaders; `tools/vulkan_rt_shaders.py` compiles them to
`xgpu_vulkan_rt_shaders.c`), records `RT_STATIC` and `RT_FRAME` in `xgpu_stream.h`.

## What is traced

- **The structure:** its render geometry's opaque environment surfaces (not alpha-tested, transparent or water), in
  world space, one bottom-level structure built once (`PREFER_FAST_TRACE`) when a BSP is first drawn.
- **The objects:** every pre-pass draw with the world's light (characters, vehicles, weapons, scenery), as drawn: their
  permutations, levels of detail and pose. All of a window's go in one bottom-level structure built each frame. The
  first-person weapon is the pre-pass's occluder and is never captured. Draws whose base map is a cutout (a fifth of
  its texels under half alpha: foliage) are left out until the traces test alpha; a mesh-store mesh is traced as its
  stock part (the pre-pass draws the stock twin).
- Off-screen objects are not in the structure (they are captured from the pre-pass), so a tree behind the camera casts
  no traced shadow; off-screen structure does.

## Settings

`display.prism_rt` (`off` / `shadows` / `gi` / `pathtraced`; `gi` and `pathtraced` trace as `shadows` until built),
`display.prism_rt_quality` (rays a pixel), `display.prism_rt_resolution`, `display.prism_rt_denoiser`,
`display.prism_rt_reflections`, `display.prism_rt_mood`, `display.prism_rt_objects`, `display.prism_rt_rate`,
`debug.prism_rt_view`, `HALO_VULKAN_NO_RT=1`. Settings > Mods has Ray Tracing and its quality, greyed out where the
renderer cannot trace. `HALO_VULKAN_STATS=1` logs the passes' GPU time every 600 frames.

## Not yet

GI (probe volumes), local lights' shadows, reflections, path tracing, alpha-tested traces, remodel relief, the
compaction of the structure's BLAS, Deck and RDNA tuning, Metal.
