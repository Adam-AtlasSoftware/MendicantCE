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

## Path tracing (`pathtraced`)

A prototype of the ultra mode. CE's frame is still drawn as CE draws it: what each pixel sees comes from the raster
G-buffer (world AO's pre-pass, at every pixel in this mode), the textures and specular are CE's, and the transparent
surfaces, effects, particles, decals, water, the first-person weapon and the HUD are drawn exactly as before (they are
not lit through the traced slot). What the path tracer changes is the light those surfaces are given:

```
PREPASS_END ─► objects' BLAS ─► TLAS ─► gi's probes (a quarter a frame)
            ─► path (compute, every pixel; half of them in a checkerboard at low quality or display.prism_rt_rate half)
                 the sun through the structure alone and through everything
                 1 to 3 bounces (low and medium 1, high 2, ultra 3; 1 to 4 paths a pixel) of a grey dielectric (diffuse + a GGX lobe, F0 0.04), next events to the sun and to one
                 dynamic light, Russian roulette past the second, the last bounce ending in gi's probes, the sky past
                 the level; the first ray traced again through the structure alone where an object took it
                 ReSTIR DI over the dynamic lights: candidates, the last frame's reservoirs at the reprojected point and
                 about it (reweighed here), the one kept traced for its shadow
            ─► accumulate (temporal: blended into the reprojected history by 1 / its length, at most 32 frames)
            ─► atrous x3 (spatial: edge-avoiding 5x5 a-trous, steps 1, 2, 4, weighed by distance, normal and ratio)
            ─► the surfaces read it in world AO's place: x a factor, yzw a light added
```

**Keeping CE's look (the exposure).** The paths give two estimates of the static light (the sun and the sky, bounced)
at each pixel: with everything in the scene, and as CE baked it (the sun through the structure alone, the first ray
through the structure alone). Their ratio multiplies CE's own light (a lightmap pass's lightmap, a model's ambient and
distant lights), so where nothing CE did not bake is in the way the factor is 1 and the frame is CE's, byte for byte in
its colours; objects take away sun and sky as they should (contact shadows, the sky's occlusion under a Pelican),
held within 0.4 to 1.1 on the level and 0.6 to 1.25 on a model (CE lit a model's insides by one flat ambient). No auto-exposure and no grade: the units cancel. The dynamic lights (the flashlight, plasma,
muzzle flashes; not the light fixtures, whose light is in the lightmap) are added by CE unshadowed; the path tracer adds
their bounce and takes away the share of their light its shadow rays find blocked (0.6 of it, held within -0.35 to
+0.35 of the slot's light), in CE's own units (its model point light's falloff). `display.prism_rt_mood` scales all of
it (0 is CE's light).

**The surfaces.** With `pathtraced` the opaque surfaces' keys have `XGPU_AMBIENT_OCCLUSION_RT_LIGHT` (xgpu.h, a bit of
the occlusion's light nibble that was free): their pixel's texel is read as a factor and an added light
(`light * x + yzw`) in the lightmap pass and in a model's ambient light (per pixel, per vertex). No other key changes,
so with the setting anything else every shader is as before.

**Not yet:** CE's materials at the hits (a grey 0.45; coloured bounce comes from the lights' colours alone), the
lightmap's radiance at hits, emissive surfaces as lights (the self-illuminated maps), primary specular (CE's cube maps
are kept), alpha-tested foliage in the structure, the probes' own update from the paths, the PBR and overlay
materials' paths (they keep the factor alone), a traced primary (for a remodel that is not drawn).

## Settings

`display.prism_rt` (`off` / `shadows` / `gi` / `pathtraced`),
`display.prism_rt_quality` (rays a pixel), `display.prism_rt_resolution`, `display.prism_rt_denoiser`,
`display.prism_rt_reflections`, `display.prism_rt_mood`, `display.prism_rt_objects`, `display.prism_rt_rate`,
`debug.prism_rt_view`, `HALO_VULKAN_NO_RT=1`. Settings > Mods has Ray Tracing and its quality, greyed out where the
renderer cannot trace. `HALO_VULKAN_STATS=1` logs the passes' GPU time every 600 frames.

## Not yet

Colour in `gi` (the traced colour path exists now, `pathtraced`'s; `gi` does not use it yet), CE's lightmap radiance
at probe hits, local lights' shadows outside `pathtraced`, reflections, alpha-tested traces, remodel relief, the
compaction of the structure's BLAS, Deck and RDNA tuning, Metal (and Metal's emitter of the traced light).

## Measured (b30, the game stopped at tick 1350, the game's camera; GPU ms a frame, `debug.gpu_stats`)

Each state against stock measured just before it in the same run (lower is better). The GPUs were shared with other
work during these runs (Blender bakes and other game instances, logged beside the results), so the absolute times are
high and noisy; the deltas are the better guide, and a clean run should be taken before any of them is quoted as final.

| GPU, size | stock | world AO | RT shadows (medium) | RT GI (medium) | RT shadows (low, half res) |
| --- | --- | --- | --- | --- | --- |
| RTX 3070 Ti, 1080p | 2.09 | +0.64 | +2.60 (passes 1.88) | +2.64 (1.85) | +1.59 (0.64) |
| RTX 3070 Ti, 1440p | 3.7-4.7 | +0.98 | +3.85 (2.82) | +4.02 (3.03) | +2.25 (1.43) |
| RTX 2060, 1080p | 8.6-9.0 (shared) | +0.34 | +3.75 (4.96) | +4.65 (5.56) | +2.51 (3.30) |
| RTX 2060, 1440p | 8.2-10.6 (shared, 96% busy before) | +2.4 | +7.5 (6.25) | +9.6 (7.25) | +5.4 (3.68) |

"passes" is the ray tracing's own GPU time (its structures' builds, the probes, the trace and the resolve), from its
timestamps.

### `pathtraced` (b30 at tick 1350, the game's camera, no dynamic lights in view; added GPU ms against stock in the same run, 2 rounds' median; "passes" the ray tracing's own timestamps)

| GPU, size | stock | world AO | RT shadows | RT GI | PT low (checkerboard) | PT medium | PT high | PT ultra |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTX 3070 Ti, 1080p | 2.46 | +0.56 | +2.75 | +2.53 | +5.64 (5.97) | +7.34 (7.06) | +9.61 (9.62) | +14.44 (13.94) |
| RTX 3070 Ti, 1440p | 3.23 | +0.94 | +4.28 | +4.30 | +11.09 (10.76) | +11.34 (11.46) | +13.09 (12.89) | +13.67 (13.98) |
| RTX 2060, 1080p (shared) | 6.43 | +3.01 | +8.85 | +8.36 | +11.04 (12.80) | +11.49 (13.39) | +12.58 (15.13) | +16.32 (18.13) |
| RTX 2060, 1440p (shared, 84% busy before) | 4.98 | +4.59 | +7.15 | +9.62 | +17.18 (14.90) | +18.63 (14.45) | +26.36 (23.16) | +45.07 (34.15) |

Another process kept the 2060 busy throughout (30 to 84% before each run): its figures are upper bounds. On the
3070 Ti, medium at 1080p is within the design's 9 ms for `pathtraced`; 1440p is not (14 ms target: medium 11.3, ultra
13.7). Low's checkerboard saves little at 1440p: the fixed passes (the object BLAS, the probes, three a-trous passes
over full-resolution half-float images) dominate, the next thing to cut. Memory: ten full-resolution RGBA16F images and
one RGBA32F (about 170 MB at 1080p, 300 MB at 1440p).

Mood gate (frame mean against stock, 15% allowed): b30 Chief -5.9%, Elites -1.1%; a50 at night with the flashlight,
first person +7.3%, Elites -9.3%; a10 0.0% (no sun indoors: the factor is 1, the a10 window leak of `shadows` is gone);
c10 -0.7% to +1.0%. The p10 check fails where the flashlight's traced shadows fall (a50: they are the added light's,
outside the factor view's mask) and under the b30 Pelican. Vulkan validation: no messages in any capture. Off: 46 of
46 frames identical to the base build at b30, with world AO on too, and with `HALO_VULKAN_NO_RT=1`.
