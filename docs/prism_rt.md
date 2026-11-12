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
    the structure's own shadows are as sharp as the lightmap holds them, unless `display.prism_baked_shadows` is
    `"sharp"` (`docs/prism_baked_shadows.md`: the sun's visibility baked finely, the same disc as the traced sun's);
  - the occlusion follows world AO's rules: the level is darkened only by objects (2 wu, 0.7 strength), objects by
    objects (their own creases among them, 1 wu) and by the structure close by (0.5 wu), at 0.6 strength;
  - `gi`: an object's light is shaded by the probes' indirect light along its normal against their mean over the
    sphere, so its brightness stays CE's and only how the light falls across it changes, within 1/k to k
    (k = 1.25 at `display.prism_rt_mood` 100, 1 at 0); the level's light is untouched (its bounce is in the lightmap);
  - no auto-exposure, no grade.
- **CE's own object shadows are left out while the sun's are traced** (any mode on, a sun above the horizon found:
  `prism_rt_sun_shadows`, render.c), as prism-world2's cascaded shadows leave them out. Drawn as well, CE's soft
  shadow under each object and the traced one stacked: the sand under the b30 Pelican went to a tenth of its light
  (the darkest tenth of that corner 24 in stock, 8 in `gi`, 29 with CE's shadow left out). With no sun, or one under the
  horizon (a10's ship), CE's shadows stay; in a sunlit level's rooms they are lost (the traced sun does not reach
  there), as with world2's.

## The frame

```
render_frame ─► world_ao_frame_begin ─► prism_rt_frame_begin: the BSP's triangles (once, a slice a frame: RT_STATIC),
                                        the sun, sky, lights and settings (RT_FRAME)
render_window ─► world AO's pre-pass (forced on while prism_rt is): each opaque draw's twin writes its pixel's
                 normal, distance and flags; the backend captures each object draw as drawn (xgpu_vulkan_rt_capture:
                 where its vertices, indices and node matrices are on the GPU; the mesh store's skinned pieces
                 where a replacement mesh draws the part), and finds its entry (its acceleration structure, kept)
                 PREPASS_END ─► the moved draws skinned (compute: skin) ─► their BLAS built or refitted (one call)
                             ─► the first-person weapon masked out (compute: mask)
                             ─► TLAS (structure mask 1, each object draw an instance, mask 4)
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
  permutations, levels of detail and pose. Nothing of them is read on the CPU: the capture names where the draw's
  vertices (a mirror segment, the ring), indices (a segment; the CPU's, copied into the ring where they are needed)
  and constants (the ring's copy the draw used) already are on the GPU, which has device addresses for every storage
  buffer where it traces rays (xgpu_vulkan.c's `buffer_make`). Each draw has an **entry** (`display.prism_rt_cache`):
  its own triangle soup and bottom-level structure (`PREFER_FAST_TRACE | ALLOW_UPDATE`) in a pool of device memory,
  named from frame to frame by its vertices' and indices' places, layout, counts and occurrence in the window (another
  object of the same model is the next occurrence). A draw whose bytes (the segments' writes counted; for the host's
  bytes, a hash of a sample of them), node matrices (the rows its vertices name, learnt from the GPU a few frames on)
  and pose are as they were is neither skinned nor built: its structure is traced again. One that moved is skinned on
  the GPU (`skin`: a triangle an invocation, the stock vertex program's two-node blend, the draw's own vertices and
  rows) and its structure refitted (an update), built again every 48 refits or where node 0 jumped 3 wu; entries
  untraced for 300 frames are let go of. The top level has an instance a draw (identity transforms: the soups are in
  the world), whose custom index names its soup in a table the hits' normals are read from. The first-person weapon
  (an occluder whose skinned bounds lie all within 1 wu of the eye: `mask`, on the GPU, from the bounds `skin` gathers)
  is left out. With `display.prism_rt` on, a model whose
  pass adds self-illumination (the Elites) takes the world's light too (its occlusion slot is its vertex light's,
  before the self-illumination is added), so it receives the traced shadows and occlusion; world AO alone still leaves
  it out. Draws whose base map is a cutout (a fifth of
  its texels under half alpha: foliage) are left out until the traces test alpha; a part the mesh store's
  replacement mesh draws (`display.prism_models`) is traced as that mesh: its pieces' skinned vertices (the mesh
  store's own skinning of the frame, shared with its draw) and 32-bit indices, copied into its entry's soup where its
  bones moved (`display.prism_rt_objects` `low_lod`: the stock part instead).
- **The level's remodel** (`display.prism_world_geometry` `remodel`, `display.prism_rt_world`): the remodel's far
  level is traced where it is drawn (b30: 282 thousand triangles for the map's 33 thousand, 19 MB of structure; 592
  thousand with the sealed remodel of combined2's data, whose far levels keep every edge vertex), or its
  near level whole (`remodel`, about 2 million), and world AO's pre-pass draws the same level, so that the rays leave
  the surface they trace (from the map's triangles, a ray started under the remodel's rock and was shadowed by it).
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
`display.prism_rt_world`, `display.prism_rt_cache`, `debug.prism_rt_view`, `HALO_VULKAN_NO_RT=1`;
`display.vulkan_device` chooses the graphics card (auto, the loader's index, `0x` and a PCI device id, or a part of its
name; the log lists them: `vulkan: device 0: ...`). Settings > Mods > PRISM Ray Tracing has Traced Level, Traced Models
and Keep Shapes too. Settings > Mods has Ray Tracing and its quality, greyed out where the
renderer cannot trace. `HALO_VULKAN_STATS=1` logs the passes' GPU time every 600 frames.

## Not yet

Colour in `gi` (the traced colour path exists now, `pathtraced`'s; `gi` does not use it yet), CE's lightmap radiance
at probe hits, local lights' shadows outside `pathtraced`, reflections, alpha-tested traces, remodel relief, the
compaction of the structure's BLAS, Deck and RDNA tuning, Metal (and Metal's emitter of the traced light).

## Measured: the objects on the GPU, kept from frame to frame (b30 at tick 1350, the kit's settings, uncapped)

The owner's kit's config.toml (every PRISM setting on: remodelled level, rebaked lightmaps, cascaded shadows,
replacement models and materials, world AO; the 3D view drawn larger than the window for its Panini projection),
`HALO_MAX_FPS=-1 HALO_NO_VSYNC=1 HALO_INTERPOLATION=true`, the base build (e33939f45) and this one run one after the
other, each state measured over 1200 frames (`debug.gpu_stats`; the render thread's CPU from /proc over the same
frames, now named "openh1 render"). "Still": the game stopped (AI erased, the game's camera); "live": the game
playing on from tick 1350 (the beach fight, the Pelican), so its states are not the same frames from one build to
the other. Other work shared both GPUs and the CPU throughout (other games and a Blender bake; the load average near
35 on 32 threads): the GPU milliseconds of one run against another move by a millisecond or two, the render thread's
share of the frame is the steadier measure. Render ms is the render thread's CPU a frame (its share times the time
between frames).

| GPU, size, scene | state | GPU ms base → new | ms between frames base → new | render thread base → new (ms a frame) |
| --- | --- | --- | --- | --- |
| 3070 Ti 1080p still | off | 5.51 → 5.76 | 8.42 → 8.03 | 64% → 61% (5.4 → 4.9) |
| | shadows | 5.57 → 7.57 | 10.31 → 8.98 | 81% → 57% (8.4 → 5.1) |
| | gi | 5.80 → 7.74 | 11.71 → 9.49 | 82% → 59% (9.6 → 5.6) |
| | pathtraced medium | 10.61 → 11.14 | 13.52 → 11.21 | 66% → 45% (8.9 → 5.0) |
| | pathtraced ultra | 16.90 → 14.94 | 16.99 → 14.95 | 55% → 27% (9.4 → 4.0) |
| 3070 Ti 1440p still | shadows | 8.42 → 9.21 | 11.30 → 13.31 | 69% → 61% (7.8 → 8.1) |
| | pathtraced medium | 14.60 → 15.85 | 14.73 → 15.89 | 60% → 31% (8.9 → 4.9) |
| | pathtraced ultra | 23.01 → 26.23 | 23.09 → 26.25 | 47% → 19% (10.8 → 5.0) |
| 2060 1080p still | off | 13.75 → 13.90 | 13.92 → 13.91 | 41% → 20% (5.7 → 2.8) |
| | shadows | 15.67 → 16.21 | 19.60 → 16.22 | 65% → 18% (12.6 → 2.9) |
| | gi | 15.85 → 16.71 | 19.94 → 16.73 | 64% → 17% (12.8 → 2.9) |
| | pathtraced medium | 22.98 → 24.53 | 23.17 → 24.55 | 44% → 12% (10.2 → 3.0) |
| | pathtraced ultra | 34.31 → 38.89 | 34.33 → 38.91 | 21% → 8% (7.1 → 3.0) |
| 2060 1440p still | shadows | 19.99 → 20.45 | 20.01 → 20.47 | 39% → 15% (7.8 → 3.0) |
| | pathtraced ultra | 54.46 → 61.23 | 54.48 → 61.25 | 12% → 6% (6.4 → 3.4) |
| 3070 Ti 1080p live | shadows | 19.33 → 18.38 | 23.52 → 18.40 | 83% → 34% (19.5 → 6.2) |
| | pathtraced medium | 37.90 → 24.97 | 37.93 → 24.98 | 50% → 24% (19.0 → 6.0) |
| | pathtraced ultra | 48.20 → 38.60 | 48.21 → 38.61 | 40% → 16% (19.0 → 6.3) |
| 2060 1080p live | shadows | 55.36 → 30.17 | 55.39 → 30.27 | 36% → 21% (19.8 → 6.2) |
| | pathtraced medium | 61.79 → 37.41 | 61.83 → 37.64 | 31% → 16% (19.0 → 6.0) |
| | pathtraced ultra | 73.80 → 48.81 | 73.82 → 49.12 | 26% → 12% (19.0 → 6.0) |

- **The render thread:** ray tracing no longer costs it anything to speak of. With the base build each mode added 3 to
  7 ms of the render thread a frame on a still scene and 13 ms in play (render ms 19 against 6 with RT off); now the
  render ms with any mode is RT off's (2.8 to 3.4 on the 2060's runs, 5 to 6.3 on the 3070 Ti's). The frames that
  waited on the render thread (shadows and gi: 19.6 ms apart for 15.7 ms of GPU work on the 2060) now follow the GPU.
- **In play** (characters moving, the Pelican), the objects were 300 thousand triangles a frame skinned on the CPU and
  built into one structure each frame; now 160 to 210 draws a frame are skinned on the GPU and refitted, 6 to 9 built
  again, 150 to 170 kept: the 2060's GPU frame at 1080p went from 55 to 30 ms with shadows and from 74 to 49 ms
  path traced at ultra.
- **Still**, every object's structure is kept (0 skinned, 140 to 150 kept a frame). The GPU's frame is a little
  longer than the base build's where the remodelled level is traced in the map's place (`display.prism_rt_world`
  `auto`): on the 2060 at 1080p, measured in one run, shadows 19.2 ms against 18.3 with `stock`, path traced at ultra
  41.8 against 35.8 (its rays cross 282 thousand triangles where they crossed 33 thousand, and the pre-pass draws
  them). Traced Level: Game's gives that back, with the shadows of the map's flat shapes where the rocks are drawn
  round. With the sealed remodel (592 thousand far triangles at b30) the difference grows: on the 2060 at 1080p with
  the kit's settings, path traced at ultra 59.6 and 59.8 ms against 38.1 and 38.9 with `stock` (b30 still, two
  alternated runs), 70.3 against 50.2 in play.
- Memory: the entries' pool is at most 320 MB (128 MB on b30 still, about 250 in play); a frame's slot is 64 MB
  where it was 136 (three slots: 216 MB less).
- Vulkan validation (`HALO_VULKAN_VALIDATION=1`, shadows, gi, pathtraced, the cache off): no messages.

## Measured, uncapped, before the objects moved to the GPU (b30 at tick 1350, the game's camera; idle GPUs at full clocks)

The tables further down were taken with the game rendering at its 30 Hz tick (interpolation off, a fixed frame
time). That leaves the GPUs so lightly loaded that the drivers hold them near 900 MHz (P3 on the 2060, logged), so
every figure there is 2 to 2.5 times high and the 2060's are not "shared GPU" noise but mostly its low clock. These
are taken with `HALO_INTERPOLATION=true HALO_FRAME_TIME=0` (both GPUs at 1.8-1.9 GHz, P0, logged beside the
results in `rt_runs/review_uncap_*`), after the capture's cached copy of the mirror (below). Added GPU ms against
stock in the same run, 2 rounds' median; in brackets the whole game's frames a second (CPU included). Lower ms is
better, higher fps is better.

| GPU, size | stock | world AO | RT shadows | RT GI | PT low | PT medium | PT high | PT ultra |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTX 3070 Ti, 1080p | 1.53 (484) | +0.30 (389) | +2.59 (107) | +2.67 (105) | +2.89 (106) | +3.39 (103) | +4.59 (97) | +6.86 (94) |
| RTX 3070 Ti, 1440p | 2.24 (371) | +0.49 (302) | +2.47 (101) | +2.43 (102) | +4.92 (92) | +5.76 (88) | +7.91 (90) | +11.85 (66) |
| RTX 2060, 1080p | 3.08 (293) | +0.85 (230) | +2.15 (101) | +2.35 (96) | +7.14 (90) | +8.25 (81) | +11.68 (64) | +16.80 (48) |
| RTX 2060, 1440p | 4.35 (212) | +1.26 (165) | +3.49 (89) | +3.71 (88) | +12.73 (56) | +14.63 (50) | +20.26 (39) | +29.12 (29) |

Against the design's targets: the 3070 Ti is inside every one (GI 3 / 4.5 ms; `pathtraced` 9 / 14 ms, even at
ultra), the 2060 inside its GI targets (3.5 / 5.5 ms). Any RT mode held the game near 100 frames a second on this
machine's CPU however light the GPU's share: the objects were still skinned on the CPU each frame (now on the GPU and
kept: the section above). Before the cached copy the capture read the mirror's write-combined memory, and
RT frames came 45-47 ms apart at the 30 Hz tick (about 21 frames a second) against stock's 33.3 ms; the earlier
tables' "between" column showed it.

**The capture's reads** (then): a captured draw's vertices and indices were read from a copy of the mirror's segment
in the host's cached memory. Now nothing is read: the segments' writes are counted instead.

## Measured at the 30 Hz tick (b30, the game stopped at tick 1350, the game's camera; GPU ms a frame, `debug.gpu_stats`)

(GPUs held near 900 MHz by the light load: see above; kept for the record.)

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
3070 Ti, medium at 1080p is within the design's 9 ms for `pathtraced` and 1440p within its 14 ms (medium 11.3,
ultra 13.7), though high at 1080p (9.6) and ultra at 1080p (14.4) are over. Low's checkerboard saves little at 1440p: the fixed passes (the object BLAS, the probes, three a-trous passes
over full-resolution half-float images) dominate, the next thing to cut. Memory: ten full-resolution RGBA16F images and
one RGBA32F (about 170 MB at 1080p, 300 MB at 1440p).

Mood gate (frame mean against stock, 15% allowed): b30 Chief -5.9%, Elites -1.1%; a50 at night with the flashlight,
first person +7.3%, Elites -9.3%; a10 0.0% (no sun indoors: the factor is 1, the a10 window leak of `shadows` is gone);
c10 -0.7% to +1.0%. The p10 check fails where the flashlight's traced shadows fall (a50: they are the added light's,
outside the factor view's mask) and under the b30 Pelican. Vulkan validation: no messages in any capture. Off: 46 of
46 frames identical to the base build at b30, with world AO on too, and with `HALO_VULKAN_NO_RT=1`.
