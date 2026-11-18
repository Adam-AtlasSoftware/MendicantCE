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
| Sun | The sky's sun, direct | A ray to a point of its disc at every pixel (since round 3: the mask's texels missed thin sunlit edges far off, round 2's G3). `D` += sun × cos × v, `S` += GGX × the sun × v | A ray within 60 world units of the eye and on every object pixel; past 60 the baked sun mask (the level's pixels) | A ray to its disc at every pixel |
| Domes | The sky's other lights, direct | One direction of each one's cone, its shadow traced (`domes_at`): `D` | The same | The same |
| Lamps | The emitting triangles (light cards, lamp surfaces), direct | ReSTIR DI: a reservoir a pixel, its initial candidates 8 lamp points from the light-importance grid (`lamp_pick`: the cell's seen and near lists, the rest by power: every lamp can be picked), each by its unshadowed diffuse and specular light; the last frame's reservoir at the surface's place reused (below); the kept sample's shadow traced (`lamp_visible`: any surface before the lamp's point blocks it, another lamp's too, since `prism-v4-m2` b2e459b8b): `D` and `S`; on a pixel whose reflection is traced (roughness up to 0.35) the lamps' `S` shared with the reflection's GGX ray meeting a lamp's face (`lamp_met`) by multiple importance sampling: the power heuristic between the ray's density (GGX's visible normals) and 1 over the lamp's solid angle (`lamp_mis_nee`), the two weights summing to 1 at every direction | The same | Resampled importance over the same 8 candidates, each one's shadow traced and its visibility in its target (`lights_ris_seen`), no reservoir reused |
| Fixtures | The scenario's light fixtures, direct | In the same reservoir (all of them, or 8 by count) | The same | The same (16) |
| Dynamic lights | SCENE_LIGHTS' dynamic class (flashlight, plasma, muzzle flashes), direct | In the same reservoir (4), CE's falloff, its shadow traced; never reused from the last frame | The same | The same (16) |
| The sky's floor | The units' rig's ambient, unoccluded, as Tool's: the sky's outdoor value in the structure's clusters with a sky, sky 0's indoor value in those with none (`prism-v4-m2` 4acaa7892, `XGPU_SCENE_INDOOR`) | A constant, `D`: the flag of the structure's triangle the pixel shows (a short stretch of the eye's ray about its point), an object's the structure below it (a ray down past the object) | The same | The same |
| Indirect | Every bounce of every static light, the sun's bounce included | One cosine bounce, its hit lit from the bake: the hit's glow and albedo by its texel's light less the sun's and less the floor its triangle took (indoor or outdoor; the volume's less the floor where its probes stand), the sun there by the mask (or the volume's visibility); a miss the sky without the domes (the pixel took them as lights) | The bake's indirect layer at the pixel where the level has its texel; else (the objects, a pixel with no texel) `quality`'s bounce | A cosine path of up to 32 bounces (prism-v4-m2 bb2a6b1a4, the bake's alike), next-event estimation of the sun, lamps and fixtures at each hit, the domes at each hit by multiple importance sampling of a direction of their cone and the path's own cosine direction where it leaves (Veach's power heuristic: the bake's light, the quieter estimate for a cone of any width) |
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

An object's normal turned from the eye (its vertices' normal at a silhouette, a two-sided card seen from behind) is
mirrored about the plane square to the eye before anything is lit (the level's, one-sided, is turned to the eye as
before): left turned away, GGX's terms go negative, and the reference summed negative reflections there (round 1's
black fringes about b30's rocks and over its trees' leaves in R, 3,000 to 21,000 pixels a pose of G2 and G3).

The resolve: the frame is `base colour × (1 − the specular's share) × D + S + emission`, the specular's share GGX's
split sum (Karis's fit), F0 4% at the material table's specular 0.5, tinted half way to the base colour by the
metalness (Raster's form). The sky's pixels are CE's frame there.

**The exposure, the tone curve, the white hold** (Raster's meter and step, shared: `xgpu_vulkan_bake_meter` and
`xgpu_vulkan_bake_exposure_step`; Adam's direction of 21:34 on November 17: the light modern, path traced and
physically lit by default, darker than Classic where it is, only the characters' art and the textures' style
matching Classic). `display.prism_exposure` multiplies `D` and `S` (the mode's style default `"auto"`, an eye's, the
shared step's as Raster has it since `prism-v4-m2` 82546a690: a share of the stops between the frame's median and a
key of 0.30, half of them where the frame is brighter and a quarter where it is darker, so a dark place stays darker,
at most the range up and two thirds of it down, moving at `display.prism_exposure_speed`; `"ce"` pins it to Classic's,
`"fixed"` is the light's own scale); `display.prism_tone_curve` takes what is past white (the mode's style default
`"filmic"`, Raster's too: `prism_deferred_raster` is true for both modes); the diffuse light is held at white as CE's page held it (a colour past white divided by its
brightest channel, its hue kept) only under the `"ce"` curve, the display's clip. The emission is CE's and stays. The
`pt_*` views are the light's own values (no exposure, no curve, no hold).

**Why `"filmic"`.** The frame is CE's: its base colours are the textures as stored, multiplied by the light, written as
they are (no transfer curve), so a curve that changes the values under white changes the textures' look, which must
stay Classic's. `"filmic"` is the identity up to 0.6 of white and rolls off past it by the brightest channel (the hue
kept): nothing under 0.6 moves, so a dark place stays dark and there is no grey wash, and the sunlit ground about
three times white comes down to white smoothly instead of clipping. Narkowicz's `"aces"` fit lifts a middle grey by a
half (0.18 to 0.27) and darkens the deepest shadows, both under white, and shifts bright colours toward white channel
by channel; `"local"` is `"filmic"` here (no local operator in this pass). The frames each gives at G2, G3 and an
interior are taken by `jobs/dev_scene.sh`'s look stills (`look/`); see `docs/prism_gates.md`.

**Where the gates measure.** The gates compare the frame as drawn, the moving frames and the reference's alike through
this composite, at a fixed exposure (`display.prism_exposure "fixed"`, the light in CE's scale) and the default curve
(`"filmic"`, no hold): `capture.py`'s `v4pt`, `v4perf`, `v4raw`, `v4sun`, `v4hist` and `refs_v4`. No exposure that
adapts between them differs; the eye's exposure would put a moving camera's frames and a still reference at different
levels.

## ReSTIR DI

The lamps, fixtures and dynamic lights are sampled by ReSTIR DI (Bitterli et al. 2020) with the reuse `[lessons]`
found safe:

- **The initial candidates** come from the bake's light-importance grid (each cell's lamps by their light with shadows
  traced, so hidden lamps are rarely picked) and from a light tree of the bake's lamps, half and half, each lamp's
  probability the two's mean (one sample of a mixture), resampled by their unshadowed light at the pixel. Each kind of
  light is its own domain, its candidates' weight over its count, so the initial reservoir is unbiased for any count.
- **The light tree** (built on the host once a bake's scene generation, from a copy of its lamps: the pass `lamps`):
  a binary tree split at the middle of its lamps' middles' widest axis; a node's importance at a point its lamps'
  power (luminance by area) by the receiver's cosine bound over the cone to its bounding sphere, over the distance
  squared (after Conty Estevez and Kulla 2018's, without the emitters' cone); a leaf's nought where the point is behind
  its triangle. Every lamp that can light a point keeps a probability above nought. The grid knows which lamps a
  cell's few points see; the tree knows which are near: a cell's lists can miss a lamp near one of its surfaces and
  leave it to the level's power, one pick in thousands (round 1's a10: the references' dense fireflies, the live
  frames' noise in motion). `HALO_PT_LAMP_TREE=0` gives the live frames the grid alone.
- **Temporal reuse only.** The last frame's reservoir at the surface's place there (the G-buffer's motion), reused only
  where its guide is this surface's (the same instance, the normal, the plane: as the denoiser's history, so no sample
  crosses an edge or an opening), its sample's target evaluated again at this pixel, its confidence capped at 8
  frames. A dynamic light's sample is not reused (their order changes from frame to frame). There is no spatial
  reuse: in `prism-pt` it spread a pick's choice over patches.
- **The target holds the shadow.** The initial reservoir is resampled by the unshadowed light (its candidates' shadows
  are not traced); its kept sample's shadow is then traced, and the last frame's sample's from this pixel, and the two
  are combined by generalized resampled importance (Lin et al. 2022) with the shadowed light at this pixel as the
  target of both: the targets alike, the generalized balance heuristic is each one's share of the confidences, and the
  light is unbiased. The kept sample's shadow is known, so its light is given with no further ray; the reservoir is
  kept with the weight of that target (an empty one, both hidden, a valid estimate of nought at its confidence). Two
  shadow rays a pixel where there is a last reservoir, one where there is none. Round 2 combined by the unshadowed
  target and traced the kept sample's shadow after: in a room of many lamps most of a pixel's picks were hidden, so
  its reservoir held a hidden lamp most frames and a seen one in a few, each frame's light nought or several times its
  mean (G4's and G5's speckled walls, darkened further by the firefly hold). `tools/test_prism_pt.py` models a still
  pixel of 40 lamps, 6 seen: both unbiased, the shadowed target's frame-to-frame spread a fifth of the unshadowed
  one's. (Round 1's first attempt, a hidden sample's weight set to nought after a choice made by the unshadowed target
  with its confidence counted, is not this: it took a ninth of a sparsely seen light at G5's first pose.)
- `HALO_PT_RESTIR=0` turns the reuse off (each pixel's own candidates alone), for comparison.

## ReSTIR GI

The quality profile's bounce is reused by ReSTIR GI's temporal reuse (Ouyang et al. 2021). A bounce sample is its hit
point (far along its direction where it leaves the level), the hit's normal and the radiance it sends back (the bake at
the hit, diffuse: the same toward any point near the pixel). A pixel's reservoir is its own sample (one candidate of
the cosine density, its target the radiance's luminance by the cosine at the pixel) and the last frame's reservoir at
the surface's place there (the denoiser's guide test, its confidence at most 20 frames), merged by generalized
resampled importance (Lin et al. 2022): each sample weighed by its MIS weight (the generalized balance heuristic over
the two points, each by its confidence and its target there, the reconnection's Jacobian taking a sample from one
point's measure to the other's) by its target here by its contribution weight. The last reservoir's confidence counts
whatever its sample (a miss's, a dark one's): left out, the light grows as it is reused, as ReSTIR DI's did in
`prism-pt` (`tools/test_prism_pt.py`: the merge unbiased across a moved point, its noise far less). The kept sample's
segment to the pixel is not traced again (the point moves by a pixel's footprint on its own surface between frames).
There is no spatial reuse yet. `HALO_PT_RESTIR_GI=0` turns it off (each frame's own bounce).

## The denoiser

The light's noisy parts (the domes, lamps, fixtures and dynamic lights, the traced bounce, the traced reflections) go
through the denoiser; the rest (the floor, the bake's indirect layer at the pixel, the probes' reflections) is added
after it. The specular is denoised as the light the lobe takes in (divided by its split sum's share, put back by
composite). `display.denoiser` `"off"` shows the light as traced (one sample a pixel). The passes, written from the
published methods (ReLAX's and SVGF's; no NVIDIA SDK code):

- **temporal**: each pixel's last-frame histories found where its surface was: a level pixel's point projected into
  the last frame's camera (the level stands still: its geometry, not a motion vector), an object's along the
  G-buffer's motion; each of the 2 × 2 taps kept only
  where its guide (the last frame's: instance, normal, distance) is this surface's: the same instance, the normal
  within 25 degrees, and on the level its point on this pixel's plane (the last frame's camera; an object's distance
  within 5%). So no history is taken across an edge, nor through an opening onto the surface behind it (a10's door
  windows). Where the whole 4 × 4 about the place passes that test, the diffuse and specular light are resampled by
  Catmull-Rom, held to the range of the 2 × 2 taps (bilinear taps at every move blurred the history along the move:
  round 2's moving G4 floor streaked a lamp's sparse samples toward the point the eye moved to); the moments, the
  lengths and the sun's visibility keep the bilinear taps. A slow history (up to 32 frames; the specular's shorter on glossy surfaces where the eye moves: its length at least
  the lobe's width (alpha, the roughness squared) over twice the parallax, the angle the eye turned about the point
  since the last frame, as ReLAX's specular accumulation, and 4 frames for a mirror under a moving eye, the whole 32
  for a still one) and a fast one (4) of the
  diffuse and specular light; the fast one holds the slow one (its luminance within the fast mean ± 2 standard
  deviations of that mean, the deviation the samples' spread the slow moments know over the fast count, the slow
  history's length cut to 4 where it is held); the luminance's moments give the variance; once the history has 16
  frames, a new sample's luminance is held to the history's mean and 8 times its spread (the firefly clamp; the
  moments are of the samples as traced, the light's history of the held ones: moments of held samples held the next
  ones lower still, so a light one frame in twenty sees was held to about half of itself under a still eye, and a
  moving eye's resampled histories loosened the hold: round 2's interiors darker than R held still, brighter moving); while it
  is shorter (a moving camera, a disocclusion, the first frames), to 4 times the brightest of its eight neighbours two (the specular to 16 times)
  pixels away on the same surface, so that one sample of a small bright light (a keypad's glow met by the bounce) is
  not spread by the fix and the a-trous steps into a blob (round 1's moving a10 frames: green dots, smears). Once the
  history is long, no light is held to its neighbours: where a lamp is seen by few samples (through a gap) a sample
  far above them is the light itself, and round 1's clamp at every frame (4 times the brightest neighbour) took a10's
  such light away from a still camera (G5's first pose: the pillar lit in R, dark here); now a still camera takes it
  once its history passes 16 frames.
- **fix**: where the history is shorter than 4 frames (a disocclusion, the first frames), the light gathered wide from
  the neighbours on the same surface (instance, normal, plane), weighed by their history's length, and written into
  the history; the variance from the neighbourhood's own spread where the moments do not have it yet (as SVGF takes
  it; round 1's 4 / n times it let the a-trous steps blur a moving camera's noise into smears).
- **atrous**: five steps of the a-trous filter (a 3 × 3 kernel, 1/4 1/2 1/4 each way, at strides 1 to 16: the reach
  of SVGF's five 5 × 5 steps with a third of their taps), each tap weighed by its
  plane's distance, its normal, the same instance (never across an object's or the level's edge) and its luminance
  against the pixel's by the variance (the diffuse's prefiltered over the pixel's cross, as SVGF prefilters it); the
  specular's kernel narrowed by the roughness and stopped after three steps (none on a mirror).
- **the sun's visibility**: its own slow and fast history, held to the range the frame's own visibility spans over the
  pixel's 5 × 5 on the same surface (a lit pixel among lit neighbours is lit, whatever the reprojection's bilinear taps
  brought from a shadow beside it: round 1's G2 lost up to 8% of its foreground's light over its first leg's forward
  move, measured block by block against the light as traced; in a penumbra the 25 samples span 0 to 1 and the history
  accumulates), the fix's gather, and the a-trous steps only at a pixel
  whose visibility is neither all lit nor all dark (above 0.02, below 0.98, with a variance) and only as wide as the
  penumbra; a lit pixel is never filtered, so a shadow beside it cannot darken it (in G2's moves of round 1, before
  this rule, the foreground's sunlight lost up to a third where the variance of a moving camera's short histories
  let the filter spread the rocks' shadows) (the blocker's distance by the disc's
  angle over the pixel's footprint). In "performance" pixels past 60 world units read the baked mask, which has no noise. The
  FidelityFX Shadows denoiser was not vendored: this filter does its work here (its tile classification, temporal
  accumulation and penumbra-sized spatial passes) in the same frame as the other signals.

The denoise step has one call site in `xgpu_vulkan_pt_frame`: the plugins' denoise and denoise-and-upscale entry points
(`prism_plugin.h`) go there.

## The reference

`debug.pt_reference N` in this mode (the request comes as with `display.prism_rt`, RT_FRAME's, `prism_rt.c`) traces at
every pixel the same scene, materials and lights unbiased with no bake, no reuse and no denoiser (the table's last
column). Its lamps are reached by next-event estimation alone: at the pixel and at every hit a lamp's point is
resampled from 8 (4 at a hit) candidates of the grid's and the light tree's mixture (above), which can pick every lamp,
by their unshadowed light there (at the pixel the GGX lobe's with the diffuse), and a ray that meets a lamp's triangle
takes only its glow; but a glossy pixel (roughness under 0.3) takes its lamps' specular light by its reflection's
own direction instead: its GGX ray's path meeting a lamp's face from the front takes that lamp's radiance, and its
next-event estimate takes the lamps' diffuse light alone (a narrow lobe's highlight is met by few of a lamp's candidate
points: round 1's references were noisiest at a10's glossy trim, a pixel's relative standard error near 1 at 1536
frames, its specular light most of its light). Each lamp's specular light is taken by one of the two, so nothing is
counted twice; the live frames keep next-event estimation for every roughness (their GGX reflectance clamped at 64,
as Raster's: a very glossy highlight of a lamp is a little brighter in the reference). That
is unbiased without weighing a lamp met by a sampled direction against its next-event estimate (multiple importance
sampling): the surfaces a path meets are diffuse (the bake's albedo), where next-event estimation is the better
sampler, and the pixel's glossy lobe is in the resampling's target. It `HALO_PT_REFERENCE_PATHS` paths a pixel a frame (1 by default; each path the diffuse lobe's or the specular's, picked
by their shares, at least a tenth each, its light over its chance), and sums them as running means (Welford's,
so a long sum keeps its precision). Every 32 frames it measures its own noise as the gates' frames measure the error:
each pixel's variance of its luminance's mean by the exposure squared, none where its drawn value is held at white or
clipped (it does not move with its noise), over the drawn luminances squared, by the share of a pixel's variance the
gates' frames keep (`metrics.load` brings every frame to 960 × 540 by bilinear filtering: 0.098 of a pixel's
independent noise at 1920 × 1080; R's pixels are independent, a seed a pixel and no reuse), the frame's estimated
relative MSE. The linear estimate (the whole light, unclamped, at full size: round 1's rule) is logged beside it; in
round 1 it was dominated by pixels far past white (G3's first pose: 0.23 linear, 5e-6 as the gates see it) and by
the resize's share. Its EXR has each part's noise beside the whole's: `noise.direct` (the floor, sun, domes, lamps,
fixtures and dynamic lights at the pixel), `noise.indirect` (the diffuse bounce), `noise.specular` (the reflection),
each part's luminance's standard error over the whole's mean. Its diffuse bounce from the pixel is guided: 8 cosine-distributed directions, each traced to its hit and weighed by
the light the bake says that hit gives back (plus a floor), one kept by its weight (resampled importance) and its path
traced; its light over its weight by the candidates' mean weight. The bake steers only which direction is traced: the
estimate's expectation is the path's, whatever the bake holds, so R stays the unbiased estimate of the same scene with
no bake in its result; its noise is far less where a dark pixel's light comes from a lit place few directions reach
(a10's corridors: R's first round, unguided, was at an estimated relative MSE of 0.04 to 0.29 after 1536 frames). It
stops once that is at most 0.001 (the gates' `reference_noise`) with 256 frames
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

## The moving objects

The traced scene is the bake's (the level and its static scenery: placements of scenery, controls and light fixtures
that nothing moves or destroys, `prism_bake.c`'s `scenery_static`) plus this frame's moving objects (c7218cff2):

- **What is captured.** Every object draw of the G-buffer pass (`xgpu_vulkan_stream.c`'s `gbuffer_draw`) but the
  first-person view's (its weapon and arms would stand in front of the eye in the world's rays) and those of objects
  the bake's static scenery already holds (the game marks them `XGPU_SCENE_INSTANCE_IN_TLAS` in SCENE_INSTANCE,
  `prism_bake_object_static`: a placement of the static kinds at its place, by `scenery_static`'s rule). So the
  vehicles, the characters, weapons on the ground and in hands, the Pelican, projectiles with a model, the machines
  (doors and lifts, which the bake leaves out since prism-v4-m2 c50d3b3d3), equipment and scenery that can move or
  break are traced as drawn, in their pose and level of detail. A draw is passed over (counted) where its vertices are
  not the stock layout, or it is alpha-tested with no cutout coverage; a cutout (foliage on an object) is traced by its
  share of texels drawn. Transparent draws (effects, particles, shields, glass) are not in the G-buffer pass and so not
  traced.
- **How.** RT_FRAME carries `XGPU_RT_FLAG_OBJECTS` under "pathtraced"; the old tracer's object capture
  (`xgpu_vulkan_rt.c`: a draw's vertices, indices and node matrices where they already are on the GPU, skinned by a
  compute pass into a triangle soup, a bottom-level structure an entry, kept from frame to frame and refitted as it
  moves) is fed by the G-buffer pass's draws, and one top level is built as the pass ends
  (`xgpu_vulkan_rt_objects_end`). Nothing else of that file runs in this mode, and the old `display.prism_rt` modes
  never see the flag: they are as they were (D3). The plan was an instance manager in the path tracer's own files;
  reusing the frozen module's capture got the objects traced now, at the cost of that module's limits (2.25 million
  object triangles a window, the far ones past `display.prism_rt_objects`' reach traced by the game's own model).
- **What sees them.** `job.objects` (the top level: object instances mask 4, cutouts 8) and `job.object_table` (a
  row an object: its soup, its base map's mean albedo, its coverage). Every visibility test of BAKE_COMMON's the
  shaders make (`blocked`, `sky_blocked`, wrapped by `tools/vulkan_pt_shaders.py` so the baker's text stays as it
  is): the lamps', fixtures', dynamic lights', domes' and the sun's at the pixel and at every path vertex; the sun's
  ray (`sun_ray`) and the far sun mask (an object's shadow over it, by the sun's centre); a bounce's or a reflection's
  hit on an object, lit by the irradiance volume at its point and a sun ray from it; the reference's paths, which hit
  them with their albedo and go on from them. `HALO_PT_OBJECTS=0` leaves them out (the frame and the reference).
- **Not yet.** An object hit's own glow and specular, its texture's colour at the hit (the mean only); the baked
  indirect light (the texels' and the volume's, at the pixel and at a hit) has no object in it, so an object takes no
  share of the indirect light the bake carries beyond the traced bounce (`performance`'s short occlusion rays are the
  plan); the first-person view's shadow on the world.

## What is not done yet

- **ReSTIR's spatial reuse** (left out on purpose, above) and the unbiased (ray-traced) reuse's correction.
- **ReSTIR GI's spatial reuse** (its temporal reuse is in), the dynamic objects' occlusion of the baked light
  (`performance`'s short AO rays), traced-light upscaling and the async queue (M5, M6).
- **The transparent layer** (water and glass, traced reflection and refraction): after the seven gate scenes pass.
- **A lamp surface seen directly** shows its G-buffer emission (its self-illumination), not its radiosity's radiance.
