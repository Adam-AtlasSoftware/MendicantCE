# PRISM ray tracing (`display.prism_rt`)

Hardware ray tracing for the Vulkan renderer, off by default. It follows the design in the PRISM overhaul's
`raytracing_design.md`; this page says what is built and how it fits the frame.

## Principles

- **Off is off.** With `display.prism_rt = "off"` (the default, and the Classic preset's) the game sends no record
  and no shader changes: the frames are byte for byte the base build's (checked at b30 with fixed step, 46 of 46
  frames, and again with the ray tracing extensions enabled against `HALO_VULKAN_NO_RT=1`). The device enables
  `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations` (with buffer device
  addresses) where present; its buffers and pipelines are made with the first frame that traces, never for a game
  that does not. Where the card has no ray queries, every mode is off and the menus' rows are greyed out.
- **Client-side and render-only.** The game reads only the camera's sky, the structure's render geometry and its
  shaders' tags and bitmaps (read-only once loaded) and the settings; nothing is simulated, saved or sent between
  machines.
- **One light rig.** The traced light is CE's own lights at the scales the relight fit found CE's lightmaps made of
  (the bake and the trace share them: "The rig", below), so a converged trace reproduces CE's pages by calibration,
  not by clamps. The sun is split as `light = A + S·v`: `S` the rig's sun by the facing, `v` its visibility, `A`
  everything else. A shadow removes exactly `S·(1 − v)`, the same for a cliff's and a Marine's shadow.
- **`shadows` and `gi` keep CE's lightmaps** and change only the sun's share of them (`display.prism_rt_sun`):
  - `all` (the default): the surfaces read the traced texel as a factor and a light added
    (`XGPU_AMBIENT_OCCLUSION_RT_LIGHT`, as `pathtraced`'s): the rig's sun as the rays see it (one ray through the
    level, its alpha-tested surfaces and the objects) less as the lightmap's texel held it (through the structure,
    from about the point over a texel's width: the page's soft edge), `S·(v − v')`, by the occlusion. The level's
    own shadows come out as sharp and as dark as the objects', from the same rays; where the rays and the page agree
    nothing changes. A level pixel never loses more than the sun's share of the light CE's lightmap gave it (the
    rig's sky and domes over theirs and the sun's: the pre-pass gives the lightmap's own light, below). An object
    only loses light (CE lit it by one sample under it). At half resolution the factor alone,
    `(A + S·v) / (A + S·v')` with the rig's `A` and `S`, the bake's sharp mask's form;
  - `objects`: the rig's sun taken only where an object blocks it and the structure does not (the structure's own
    shadows are in the lightmap: never darkened again), as before; with no rig fitted, `0.65` of a sunlit
    lightmap's light (`0.55` of a model's) by `smoothstep` facing terms, as before;
  - the occlusion follows world AO's rules: the level is darkened only by objects (2 wu, 0.7 strength), objects by
    objects (their own creases among them, 1 wu) and by the structure close by (0.5 wu), at 0.6 strength;
  - `gi`: an object's light is shaded by the probes' light (now the rig's, off real albedo, every bounce) along its
    normal against their mean over the sphere, within 1/k to k (k = 1.25 at `display.prism_rt_mood` 100).
- **`pathtraced` replaces the lightmaps' light** with the rig's, traced (below), and then keeps CE's look as far as
  `display.prism_rt_style` asks: `faithful` holds each region to CE's light, at CE's exposure; `remix` does not.
- **CE's own object shadows are left out while the rays trace** (any mode on: `prism_rt_sun_shadows`, render.c),
  and so are `display.prism_shadows = "cascaded"`'s (`prism_shadows_pass_begin`): drawn as well, two shadows of one
  object stacked (the kit ran `pathtraced` with cascaded). With no sun, the traced occlusion stands for CE's shadow.
- **The sun is the sky's narrowest light** (`render_sky_sun`): the narrowest under 0.1 rad across whose power times
  its brightest channel is at least a quarter of the sky's strongest. a50's moon, not its brighter 50 degree dome
  (whose shadows fell straight down from the zenith); c10 (a 20 degree dome alone) has none. The other sky lights
  are the rig's domes.

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
                             ─► TLAS (structure mask 1, its alpha-tested surfaces 2, each object draw an
                                 instance, mask 4, the cutouts' 8)
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

- **The structure:** its render geometry's environment surfaces, in world space, with each triangle's material and
  base map coordinates: the opaque ones in one bottom-level structure (`PREFER_FAST_TRACE`, mask 1), the
  alpha-tested ones (grates, fences, leaf cards; `display.prism_rt_alpha`) in another (mask 2, not opaque: each
  candidate kept where its material's alpha map's texel is past half), built once when a BSP is first drawn; the
  light cards (transparent shaders with radiosity power) only as lamps, never traced. Water and other transparent
  surfaces are not traced.
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
  its texels under half alpha: foliage) are traced as solid by the share of their texels drawn (mask 8, not opaque:
  each candidate kept by that chance), with `display.prism_rt_alpha`; a part the mesh store's
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

## The rig

The lights of the bake and the trace (art_pipeline's `relight_bake.py` and `relight_calib.py`), per BSP, in the
units the relight fit made CE's pages of: a lightmap texel's light is the rig's light at that point, to `1 / gamma`
(`gamma` 1, or 2.2 where the fit chose it). Read by the game from
`<data>/prism_lightmaps/rebaked/<map>_<bsp>.scales.json` (the fit's `scales.json`: `sun`, `sky`, `gamma`, `emit`
{shader: scale}, `rgb` gains; made on the player's machine, never shipped). Where a BSP has none, or the fit found a
group gave none of its light (an indoor BSP's sun), the fits' typical scales stand in (sun 1.3, sky 3.6, lamps 0.5).

| Light | From | Traced as |
| --- | --- | --- |
| Sun | The sky's narrowest light (`render_sky_sun`) | Its colour by power, `scale.sun` and `rgb.sun`, over pi (the bake's sun lamp of strength 1 lights a white surface to 1 / pi), by the facing, through a disc (`display.prism_sun_size`) |
| Domes | The sky's other lights (b30's and a30's 50 degree zenith dome, a10's back light, a50's dome, c10's) | Cone lights at their diameter, by the sun's scale as the fit scaled them: a direction of the cone each (next events) at a pixel, their radiance where a path's ray leaves the level within one |
| Sky | The sky tag's outdoor ambient by power, `scale.sky`, `rgb.sky` | The radiance of every ray that leaves the level |
| Lamps | Every BSP shader with radiosity power: the light cards (transparent, not traced, never casting shadows) and the lamps' own surfaces | One-sided emitters along CE's normal (as the bake's): their emitted colour by power, `scale.emit[shader]`, `rgb.emit[shader]`, `display.prism_rt_emissive_scale`; ReSTIR DI at each pixel, one candidate at a path's hits |
| Glows | Environment self-illumination maps (`display.prism_rt_emissive "all"`) | Their mean light by their on colours, at the surfaces the paths meet |
| Dynamic lights | `world_scene`'s (flashlight, plasma, muzzle flashes) | Added by CE unshadowed; their bounce traced, and their traced shadows' share of their light taken (ReSTIR DI over them) |

Light fixtures (`scenario->light_fixtures`) are not in the rig: the bake has none, so their scale is not known.
`faithful`'s fill (below) puts back the light CE's pages show that the rig lacks.

**Materials.** Each BSP shader traced has a material (`RT_STATIC`'s `_MATERIALS`): its albedo (its base map's mean,
made linear, by its detail and micro detail maps' functions at their means, within 0.03 to 0.85, as the bake's), its
alpha map (alpha-tested environment shaders: the base map's alpha at 256 a side at most), its lamp's light and its
glow. The game reads the bitmaps as a draw loads them, once a BSP. An object's albedo is its base map's mean (the
host's texture cache, once a texture's generation). `display.prism_rt_materials "grey"`: 0.45 for all.

## Path tracing (`pathtraced`)

CE's frame is still drawn as CE draws it: what each pixel sees comes from the raster G-buffer (world AO's pre-pass,
at every pixel in this mode), the textures, specular and fog are CE's, and the transparent surfaces, effects,
particles, decals, water, the first-person weapon and the HUD are drawn exactly as before. What changes is the light
the opaque surfaces are given: the rig's, traced, in place of the lightmap's and of a model's ambient and distant
lights (`light × x + yzw` with `x` = 1 − the mood, 0 at `display.prism_rt_mood` 100).

```
PREPASS_END ─► objects' BLAS ─► TLAS (opaque structure 1, alpha-tested structure 2, objects 4, cutouts 8)
            ─► probes (a quarter a frame): their rays in the rig's light, each hit's albedo by the sun, a lamp,
               and the probes' last irradiance there (every bounce, kept from frame to frame)
            ─► path (every pixel; half in a checkerboard at low quality or display.prism_rt_rate half):
                 the sun's visibility (its disc, through everything)
                 the domes (a direction of each cone) and the lamps (ReSTIR DI: candidates by power, the last
                 frame's reservoirs at the point and about it, the one kept traced for its shadow)
                 the sky and the bounces: 1 to 4 (display.prism_rt_bounces; by quality), cosine rays, each hit
                 its material's albedo by the sun, a lamp and its glow, the last ending in the probes;
                 Russian roulette past the second
                 the dynamic lights (ReSTIR DI over them, as before)
            ─► accumulate (temporal: the light by 1 / its length, at most 32 frames; the sun's visibility at most 8,
               held within its neighbours' reach this frame; a sample past 32 times its history held to it)
            ─► tiles (8x8: CE's light and the traced light, summed; their histograms for the exposure)
            ─► exposure (display.prism_exposure)
            ─► atrous x3 (5x5, steps 1, 2, 4: by distance and normal; the sun's visibility also by its nearness
               over its spread), the last writing x and yzw: the rest + the rig's sun by the facing and the
               visibility, in CE's encoding, held to its region by the anchor, exposed
the 3D view's end ─► display.prism_tone_curve (compute, over a copy of the view) ─► prism_tone, bloom, tone map
```

The pre-pass's lightmap twins (those of draws reading the traced light) write the light their lightmap gives in the
inputs' fourth word (its square root, 8 bits a channel): CE's light at each level pixel, the anchor's and the
exposure's reference (`debug.prism_rt_view albedo`).

**Styles** (`display.prism_rt_style`; any of their settings set explicitly wins):

| | `faithful` (default) | `remix` |
| --- | --- | --- |
| `display.prism_rt_anchor` | 100: each region (a tent 40 pixels wide) held to CE's light within 1/1.5 to 1.5, and the light CE's region has past that added evenly in CE's colour there | 0: the rig alone |
| `display.prism_exposure` | `fixed` (CE's) | `ce`: the traced light's median over the level's pixels pinned to CE's, within 1.5 EV, 2 EV a second |
| `display.prism_tone_curve` | `ce` (clipped) | `local`: each region compressed past half white, its detail kept, rolled off |
| `display.prism_rt_emissive` | `lights` | `all` (the glows too) |
| dynamic lights' traced shadows | 0.6 of their light | all of it |

The exposure applies to the traced light alone, as it is given to the surfaces, so effects, transparent surfaces and
the HUD stay in CE's encoding over it (pre-exposed). The keyed lightmap pass adds its light past the combiners'
clamp, so the level's light goes past white in `display.hdr`'s half floats; a model's light is still clamped at
CE's white. The tone curve needs the half floats (`display.hdr` on; none over 8 bits or a multisampled view).

**Methods** written from their papers (no code taken): ReSTIR DI (Bitterli, Wyman, Pharr, Shirley, Lefohn,
Jarosz, 2020) over the lamps and the dynamic lights, resampled importance sampling (Talbot, Cline, Egbert, 2005);
the visibility's moments and variance-guided weights after SVGF (Schied et al., 2017); the edge-avoiding a-trous
wavelet (Dammertz et al., 2010); Narkowicz's ACES fit (2016); the anchor after `raytracing_design.md` §8.5.

## Settings

`display.prism_rt` (`off` / `shadows` / `gi` / `pathtraced`),
`display.prism_rt_quality` (rays a pixel), `display.prism_rt_resolution`, `display.prism_rt_denoiser`,
`display.prism_rt_reflections`, `display.prism_rt_mood`, `display.prism_rt_objects`, `display.prism_rt_rate`,
`display.prism_rt_world`, `display.prism_rt_cache`, `display.prism_rt_sun`, `debug.prism_rt_view`,
`HALO_VULKAN_NO_RT=1`; the path tracer's look: `display.prism_rt_style`, `display.prism_rt_anchor`,
`display.prism_sun_size`, `display.prism_rt_emissive`, `display.prism_rt_emissive_scale`,
`display.prism_rt_materials`, `display.prism_rt_alpha`, `display.prism_rt_bounces`, `display.prism_exposure`
(`_range`, `_speed`, `_bias`), `display.prism_tone_curve` (port/linux/README.md has each);
`display.vulkan_device` chooses the graphics card (auto, the loader's index, `0x` and a PCI device id, or a part of its
name; the log lists them: `vulkan: device 0: ...`). Settings > Mods > PRISM Ray Tracing has Traced Level, Traced Models,
Keep Shapes and Traced Sun too; Settings > Mods > PRISM Path Tracing the look's. Settings > Mods has Ray Tracing and
its quality, greyed out where the renderer cannot trace. `HALO_VULKAN_STATS=1` logs the passes' GPU time every 600
frames.

## Not yet

RT reflections (`display.prism_rt_reflections` is read, not traced), motion vectors (the denoisers reproject by the
camera alone: a moving object's light trails until its history is let go of), textured materials at hits (each
material's mean), the painted sky as a light (the sky's ambient alone), light fixtures in the rig, a world radiance
cache (the probes about the camera stand for it), the objects' cutouts' own alpha (their share of texels drawn
stands for it: a canopy casts a solid, soft shadow), the in-engine visibility baker for RT off, Metal (and Metal's
emitter of the traced light), Deck and RDNA tuning. Models' traced light is clamped at CE's white.

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
