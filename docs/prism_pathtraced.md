# PRISM Path Traced

PRISM v4's milestone M3 (`overhaul_research/prism_v4_renderer/design.md`, sections 1.5, 2.1, 3.8 and 7): with
`display.lighting` `"pathtraced"`, every CE light is off and the deferred core's opaque pixels are lit by the path
tracer, from the G-buffer (raster primary visibility) and this machine's own bake of the level (`docs/prism_bake.md`).
This document says what the path tracer traces, what it reads from the bake, which light each term carries (so that
nothing is counted twice), the denoiser, the reference it is judged against, and what is not done yet. The gates'
figures are in `docs/prism_gates.md` ("M3: Path Traced").

## What runs where

| Part | Where | What |
| --- | --- | --- |
| The mode | The game, `port/linux/game/prism_deferred.c` | `"pathtraced"` is the deferred core lit from the bake, as `"raster"` is: the same G-buffer pass, scene records, `PRISM_BAKE` control and light rig in the light's units (`prism_deferred_raster` is true for both); the resolve's value carries `XGPU_GBUFFER_PHASE_PATHTRACED`, the profile and the denoiser |
| The path tracer | The renderer, `port/linux/vulkan/xgpu_vulkan_pt.c` | Its passes each frame, in the resolve's place of Raster's relight: light, temporal, fix, four a-trous steps, composite; or the reference |
| Its shaders | `port/linux/vulkan/xgpu_vulkan_pt.glsl` | Each pass is the baker's `BAKE_COMMON` (`xgpu_vulkan_bake_glsl.inc`), then the file's common part, then its own; `tools/vulkan_pt_shaders.py` makes `xgpu_vulkan_pt_shaders.c` |
| The bake's part | `port/linux/vulkan/xgpu_vulkan_bake.c` | `xgpu_vulkan_bake_lookup`: the bake's scene (its acceleration structure, materials, lamps, light-importance grid, fixtures and rig), Raster's lookups, the indirect layer on the GPU (a word a texel beside the tiles) and each level triangle's lightmap page and coordinates |

The path tracer traces the scene the bake was made from (SCENE_STATIC's level as drawn, its near level, and the static
scenery), with the bake's materials and lights, by the bake's own functions (`lamp_pick`, `lamp_direction`,
`lamp_visible`, `domes_at`, `fixture_light`, `sky_along`, `ambient`, `glow_of`, `albedo_of`, `hit`, `blocked`): the
trace and the bake are one estimator's. None of the old `display.prism_rt` modes' passes (their anchor, hold, filter,
radiance cache, probes or checkerboard) is used, and CE's lightmap is never read: the light target CE's passes still
draw is read only where the G-buffer has no surface (the sky).

Until the bake's scene is made (the plan, a few seconds after a BSP is first drawn) there is nothing to trace: the frame
is CE's until then (logged once).

## Which light each term carries

The bake's unit is a white surface's radiance (the irradiance over pi). At an opaque pixel, its diffuse light `D` (the
base colour multiplies it) and its specular light `S`:

| Term | Light | `quality` | `performance` | The reference |
| --- | --- | --- | --- | --- |
| Sun | The sky's sun, direct | A ray to a point of its disc within 60 world units of the eye and on every object pixel; past 60 the baked sun mask (the level's pixels). `D` += sun × cos × v, `S` += GGX × the sun × v | The same | A ray to its disc at every pixel |
| Domes | The sky's other lights, direct | One direction of each one's cone, its shadow traced (`domes_at`): `D` | The same | The same |
| Lamps | The emitting triangles (light cards, lamp surfaces), direct | ReSTIR DI: a reservoir a pixel, its initial candidates 8 lamp points from the light-importance grid (`lamp_pick`: the cell's seen and near lists, the rest by power: every lamp can be picked), each by its unshadowed diffuse and specular light; the last frame's reservoir at the surface's place reused (below); the kept sample's shadow traced: `D` and `S` | The same | Resampled importance over the same 8 candidates, no reservoir reused |
| Fixtures | The scenario's light fixtures, direct | In the same reservoir (all of them, or 8 by count) | The same | The same (16) |
| Dynamic lights | SCENE_LIGHTS' dynamic class (flashlight, plasma, muzzle flashes), direct | In the same reservoir (4), CE's falloff, its shadow traced; never reused from the last frame | The same | The same (16) |
| The sky's floor | The units' rig's ambient: CE's outdoor ambient, unoccluded | A constant, `D` | The same | The same |
| Indirect | Every bounce of every static light, the sun's bounce included | One cosine bounce, its hit lit from the bake: the hit's glow and albedo by its texel's light less the sun's and less the floor (or the volume's), the sun there by the mask (or the volume's visibility); a miss the sky without the domes (the pixel took them as lights) | The bake's indirect layer at the pixel where the level has its texel; else (the objects, a pixel with no texel) `quality`'s bounce | A cosine path of up to 8 bounces, next-event estimation of the sun, lamps and fixtures at each hit, the domes where it leaves |
| Reflections | The light the surface's specular lobe sees, less the lights taken directly | Below a roughness of 0.35, one GGX ray (the visible normals' distribution, Heitz 2018) lit as the bounce's hit, the domes where it leaves; above it, the bake's reflection probes by the split sum | The same | A GGX ray and the same path, the domes where it leaves |
| Emission | The surface's own (the G-buffer's emissive: an object's self-illumination; the level's, by its base colour, as CE's lightmap pass folds it in) | Added | Added | Added |

Why nothing is counted twice:

- **The bake's layers.** The bake keeps, at each texel, the light less the sun's (the domes, lamps and fixtures direct,
  the floor, and every bounce) and, apart, the indirect light alone (every bounce, the sun's included). The pixel's
  direct terms are traced; the bake gives the pixel only its indirect layer (`performance`), never the light less the
  sun's.
- **A bounce's hit** gives back its glow and its albedo by the hit's whole light less the floor: the bake's own path
  estimator does not pass the floor on, so neither does the bounce. The hit's light includes the lamps' direct light
  there, which is the pixel's indirect light.
- **The domes.** The pixel takes them as lights (`domes_at`), so a diffuse bounce's miss returns the sky without them
  (the bake's first ray's rule). A reflection's ray does not take them as lights, so its miss returns them.
- **The lamps.** A lamp's light reaches a point by next-event estimation only: a ray that meets a lamp's triangle takes
  only its glow (a self-illumination map's), never its radiosity's light, in the bake, the reference and the live trace
  alike.
- **The probes** see the lamps as the bake's paths do (by their glow), so a rough surface's reflection of a lamp is its
  RIS's specular, once.

The resolve: the frame is `base colour × (1 − the specular's share) × D + S + emission`, the specular's share GGX's
split sum (Karis's fit), F0 4% at the material table's specular 0.5, tinted half way to the base colour by the
metalness (Raster's form). The sky's pixels are CE's frame there.

## ReSTIR DI

The lamps, fixtures and dynamic lights are sampled by ReSTIR DI (Bitterli et al. 2020) with the reuse `[lessons]`
found safe:

- **The initial candidates** come from the bake's light-importance grid (each cell's lamps by their light with shadows
  traced, so hidden lamps are rarely picked), resampled by their unshadowed light at the pixel. Each kind of light is
  its own domain, its candidates' weight over its count, so the initial reservoir is unbiased for any count.
- **Temporal reuse only.** The last frame's reservoir at the surface's place there (the G-buffer's motion), reused only
  where its guide is this surface's (the same instance, the normal, the plane: as the denoiser's history, so no sample
  crosses an edge or an opening), its sample's target evaluated again at this pixel, its confidence capped at 8
  frames. A dynamic light's sample is not reused (their order changes from frame to frame). There is no spatial
  reuse: in `prism-pt` it spread a pick's choice over patches.
- **A hidden sample still counts.** The kept sample's shadow is traced; where it is hidden, the reservoir kept for the
  next frame has weight 0 but keeps its confidence, so a reused reservoir whose lamp was hidden still counts its
  candidates and the light does not brighten as it is reused.
- `HALO_PT_RESTIR=0` turns the reuse off (each pixel's own candidates alone), for comparison.

## The denoiser

The light's noisy parts (the domes, lamps, fixtures and dynamic lights, the traced bounce, the traced reflections) go
through the denoiser; the rest (the floor, the bake's indirect layer at the pixel, the probes' reflections) is added
after it. The specular is denoised as the light the lobe takes in (divided by its split sum's share, put back by
composite). `display.denoiser` `"off"` shows the light as traced (one sample a pixel). The passes, written from the
published methods (ReLAX's and SVGF's; no NVIDIA SDK code):

- **temporal**: each pixel's last-frame histories found along the G-buffer's motion, each of the 2 × 2 taps kept only
  where its guide (the last frame's: instance, normal, distance) is this surface's: the same instance, the normal
  within 25 degrees, and on the level its point on this pixel's plane (the last frame's camera; an object's distance
  within 5%). So no history is taken across an edge, nor through an opening onto the surface behind it (a10's door
  windows). A slow history (up to 32 frames; the specular's shorter on glossy surfaces) and a fast one (4) of the
  diffuse and specular light; the fast one holds the slow one (its luminance within the fast mean ± 2 standard
  deviations of that mean, the deviation the samples' spread the slow moments know over the fast count, the slow
  history's length cut to 4 where it is held); the luminance's moments give the variance; once the history has 16
  frames, a new sample's luminance is held to the history's mean and 8 times its spread (the firefly clamp). Neither
  holds a light to what the pixel's neighbours or its last few frames saw: where a lamp is seen by few samples (through
  a gap) a sample far above them is the light itself, and the first round's clamps (4 times the brightest neighbour,
  the fast spread alone) took a10's such light away (G5's first pose, round 1: the pillar lit in R, dark here).
- **fix**: where the history is shorter than 4 frames (a disocclusion, the first frames), the light gathered wide from
  the neighbours on the same surface (instance, normal, plane), weighed by their history's length, and written into
  the history; the variance from the neighbourhood where the moments do not have it yet.
- **atrous**: five steps of the a-trous filter (a 3 × 3 kernel, 1/4 1/2 1/4 each way, at strides 1 to 16: the reach
  of SVGF's five 5 × 5 steps with a third of their taps), each tap weighed by its
  plane's distance, its normal, the same instance (never across an object's or the level's edge) and its luminance
  against the pixel's by the variance; the specular's kernel narrowed by the roughness and stopped after three steps
  (none on a mirror).
- **the sun's visibility**: its own slow and fast history, the fix's gather, and the a-trous steps only where it is
  neither all lit nor all dark (its variance) and only as wide as the penumbra (the blocker's distance by the disc's
  angle over the pixel's footprint). Pixels past 60 world units read the baked mask, which has no noise. The
  FidelityFX Shadows denoiser was not vendored: this filter does its work here (its tile classification, temporal
  accumulation and penumbra-sized spatial passes) in the same frame as the other signals.

The denoise step has one call site in `xgpu_vulkan_pt_frame`: the plugins' denoise and denoise-and-upscale entry points
(`prism_plugin.h`) go there.

## The reference

`debug.pt_reference N` in this mode (the request comes as with `display.prism_rt`, RT_FRAME's, `prism_rt.c`) traces at
every pixel the same scene, materials and lights unbiased with no bake, no reuse and no denoiser (the table's last
column). Its lamps are reached by next-event estimation alone: at the pixel and at every hit a lamp's point is
resampled from 8 (4 at a hit) candidates of the grid's mixture, which can pick every lamp, by their unshadowed light
there (at the pixel the GGX lobe's with the diffuse), and a ray that meets a lamp's triangle takes only its glow. That
is unbiased without weighing a lamp met by a sampled direction against its next-event estimate (multiple importance
sampling): the surfaces a path meets are diffuse (the bake's albedo), where next-event estimation is the better
sampler, and the pixel's glossy lobe is in the resampling's target. It `HALO_PT_REFERENCE_PATHS` paths a pixel a frame (1 by default; each path the diffuse lobe's or the specular's, picked
by their shares, at least a tenth each, its light over its chance), and sums them as running means (Welford's,
so a long sum keeps its precision). Every 32 frames it measures its own noise: each pixel's variance of its
luminance's mean over the sum of the means squared, the frame's estimated relative MSE (linear, before the tone curve:
stricter than the 8-bit frame's). Its diffuse bounce from the pixel is guided: 8 cosine-distributed directions, each traced to its hit and weighed by
the light the bake says that hit gives back (plus a floor), one kept by its weight (resampled importance) and its path
traced; its light over its weight by the candidates' mean weight. The bake steers only which direction is traced: the
estimate's expectation is the path's, whatever the bake holds, so R stays the unbiased estimate of the same scene with
no bake in its result; its noise is far less where a dark pixel's light comes from a lit place few directions reach
(a10's corridors: R's first round, unguided, was at an estimated relative MSE of 0.04 to 0.29 after 1536 frames). It
stops once that is at most 0.001 (the gates' `reference_noise`) with 64 frames
at least, or at N frames, and writes a linear OpenEXR file: `light.R/G/B` (the diffuse light's mean), `spec.R/G/B`
(the specular light's), `Z` (the distance; 0 the sky), `samples` and `noise` (each pixel's relative standard error),
with `prismReferenceFrames` and `prismReferenceNoisePpm` (the estimate, in millionths). The frame it holds is the
means through the same composite as the live frames. It starts again where the camera, the game's time or the bake's
scene changes.

## The settings

| Setting | Values | |
| --- | --- | --- |
| `display.lighting` | `"pathtraced"` | Every CE light off, the path tracer's light |
| `display.pt_profile` | `"quality"`, `"performance"` | The indirect light: a bounce at every pixel, or the bake's where the level has it |
| `display.denoiser` | `"prism"`, `"off"` | The in-tree denoiser, or the light as traced |
| `debug.gbuffer_view` | `"pt_diffuse"`, `"pt_specular"`, `"pt_sun"`, `"pt_direct"`, `"pt_indirect"`, `"pt_history"`, `"pt_noisy"` | The diffuse light (denoised), the specular, the sun's visibility, the direct or indirect light alone (each denoised), the history's length, the diffuse light as traced |
| `debug.pt_reference` | frames | The reference, at most that many frames |

`HALO_PT_TIMES_FILE` writes each frame's GPU time and its passes' (light, temporal, fix, atrous, composite), as the log
does their means every 600 frames.

## What is not done yet

- **Moving objects in the trace.** The traced scene is the bake's: the level and the static scenery. The moving
  objects (vehicles, doors and other devices, weapons on the ground, bodies) are lit, but cast no traced shadow, are
  not hit by rays and do not occlude the bake's light; the reference has the same scene, so the gates compare like
  with like. The design (section 8.1) keeps `prism-pt`'s GPU object capture, skinning and per-draw acceleration
  structures as the scene's instance manager; they live in `xgpu_vulkan_rt.c` and are fed by world AO's pre-pass,
  which this mode does not draw. Feeding them from the G-buffer pass's object draws (an RT_FRAME mode that builds the
  objects' structure and traces nothing) is the short way; a new instance manager the long one. The first touches the
  frozen RT backend's file (not its modes), so it waits for a decision.
- **ReSTIR's spatial reuse** (left out on purpose, above) and the unbiased (ray-traced) reuse's correction.
- **ReSTIR GI** (`quality`'s bounce reuse), the dynamic objects' occlusion of the baked light (`performance`'s short
  AO rays), traced-light upscaling and the async queue (M5, M6).
- **The transparent layer** (water and glass, traced reflection and refraction): after the seven gate scenes pass.
- **A lamp surface seen directly** shows its G-buffer emission (its self-illumination), not its radiosity's radiance.
