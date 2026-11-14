# PRISM's in-game baker and PRISM Raster

PRISM v4's milestone M2 (`overhaul_research/prism_v4_renderer/design.md`, sections 3 and 1.8): each level's light is
baked on the player's own machine, by the GPU, from the level the game draws, and `display.lighting` `"raster"` draws
the game lit from that bake, with no ray tracing while playing. This document describes what is baked and how, the
bake file, the settings, how Raster reads the bake, and what was measured. The gates' figures are in
`docs/prism_gates.md` ("M2: the bake and PRISM Raster").

Nothing a bake makes is shared: the bake files are made from the player's maps and stay on the machine they were made
on (`<data>/prism_bake`, or `display.bake_folder`).

## What runs where

| Part | Where | What |
| --- | --- | --- |
| The control | The game, `port/linux/game/prism_bake.c` | Each frame, `PRISM_BAKE`'s control (`xgpu_stream.h`): the map and BSP drawn, `display.bake`, `display.bake_quality`, the rebakes asked for, the bake folder, the GPU time a frame may give the bake, and where the renderer writes how the bake goes (the status block) |
| The reach's inputs | The game, the same file | Once a BSP: the collision BSP, the clusters and their PVS, the water, the render triangles, the scenario's places and the fighters it places, serialised and sent a slice a frame |
| The reach | The renderer, `port/linux/vulkan/xgpu_vulkan_bake.c` (a thread of its own) | The reachable space (`prism_bake_reach.c`): the walk over floors from the scenario's places, the camera volume, flight where a fighter the player can reach is placed; each render surface's importance tier; the volume's bricks. A large BSP's walk is millions of cells (b30's: 8 million), more than the game's memory holds, so it runs on the 64-bit side |
| The bake | The renderer, the same file and `xgpu_vulkan_bake_glsl.inc` | A slice of each frame's GPU work: the passes below, into the bake file |
| PRISM Raster | The renderer, the same files | The light target lit from the bake before the deferred resolve (`xgpu_vulkan_post_deferred_resolve`'s relight) |
| The indicator | The game, `port/linux/game/prism_bake_indicator.c` | "Baking lightmaps 37%" at the screen's top left while a level is baked |

The bake reads what the renderer already has: SCENE_STATIC's level as drawn (its near level), display.prism_rt's
materials (each environment shader's mean albedo, its alpha map, its glow; the light cards' and lamp surfaces'
emission) and its light rig (RT_FRAME's: the sun, the sky's ambient, the domes, the lamps), in the light's units (below:
`display.prism_rig` `"auto"` is `"units"` with Raster). In Raster the game sends display.prism_rt's scene and rig
whatever `display.prism_rt` says, with nothing traced in the frame (RT_FRAME's mode off). With the reach's inputs the
game sends two more things only it has:

- **The static scenery** (`prism_bake.c`'s `scenery_copy`): every placement of the scenario's scenery, machines and
  controls that nothing can move or destroy (no script names it, it is made on every difficulty, it has no animations
  and no body to damage: `world_sdf.c`'s rule), in this BSP, its model's most detailed geometry in the world, each
  triangle wound to its vertices' normals, with its shader's albedo (the base map's mean colour) and coverage (an
  alpha-tested model shader's share of its base map's texels drawn: the leaves' cards; a transparent shader's triangles
  left out). b30's first BSP: 592 placements, 90,319 triangles. The bake traces them with the level (their own
  structures in its scene: opaque, mask 4; cutouts, mask 8, a hit kept by 0.75 of its share, as R keeps an object's), so
  the ground under a tree takes its shade and the sun mask its shadow. They have no texels of their own: Raster lights
  them from the volume, as every object.
- **The light fixtures** (`fixtures_copy`): the scenario's light fixture placements in this BSP as placed (their colour,
  intensity, falloff and cutoff angles), CE's radiosity's point and spot lights. The bake takes every one that gives
  light, whether its device has power or not (the radiosity took them all), as a point light of inverse-square falloff
  (or a spot), its shadow traced. On the maps measured none lit a gate's pose: c40's thirty and a10's give no light
  (zero intensity or colour), b30's ten are far from G2 and G3. R takes the live fixtures from SCENE_LIGHTS alike.

The map's own triangles are wound clockwise, as D3D draws them: their corners' cross product points into the level.
SCENE_STATIC now turns each of them to its vertices' normals (`prism_scene_static.c`), so that every consumer's face
normal points out of the level; before M2, display.prism_rt's one-sided faces were inside out with the stock world
traced as drawn (the remodel, two-sided, hid it).

## The light's units

`display.prism_rig` `"units"` (`prism_rt.c`'s `PRISM_RT_UNITS_*`) puts every light in the bake's unit, a white surface's
radiance (its irradiance over pi), from its tag's colour and power alone, the same numbers for every map, custom ones
included; no map's fit (`scales.json`) is read. CE's lightmap holds the light its radiosity found as it is (linear: 1.0
is the 8-bit page's white, Tool's radiosity stopping on a residual in 1/255 steps), and CE's frame is that light times
the base map, so the bake's light goes into Raster's frame as CE's lightmap does.

| Light | Its power is | In the bake's unit |
| --- | --- | --- |
| The sun (the sky's narrowest light) | the irradiance it gives a surface facing it | colour × power / pi |
| The domes (the sky's other lights) | the same | colour × power / pi |
| The sky's ambient (its outdoor ambient radiosity) | CE's floor of light, unoccluded, at every point | colour × power × pi, added to every texel and probe (not passed on by the bounces; the sky beyond the level then gives no more) |
| A light card or lamp surface (radiosity power) | its radiance | colour × power × 2 |
| A light fixture (intensity) | the irradiance it gives a surface facing it a world unit away | colour × intensity / d² |

The sun's and the fixtures' are their definitions. The sky's and the cards' factors were measured against CE's own
pages: each kind of light baked alone at 1 (`debug.prism_rig_scales`), its light at the gates' poses (`debug.gbuffer_view`
`bake_total`) fitted to CE's light target there (`ce_deferred`'s light, the level's pixels). The sun came out at 0.84
to 0.92 (b30's G2 and G3: 1 kept). The sky traced as a radiance from the open sky needed 5.9 on b30's open beach and
7.9 under its overhang, and c40's canyon, CE's light there blue and even, more still: CE's outdoor ambient behaves as an
unoccluded floor, which b30's poses put at 3.1 to 4.1 times its colour by power (pi kept). The cards' exposure came out
at 1.3 to 2.3 times their power over a10's and c40's poses (2 kept), but not alike over their surfaces: CE's light on
a10's typical surface is three to fifteen times the bake's at 1, on c40's interior's 1.5 times (see "What remains"). The sky the rig takes is the BSP's own (the one most of its clusters
have), whatever the window sees, so that a bake's rig is the same wherever its camera was. The sky's indoor ambient
(CE's floor of light in its indoor clusters) is not a light of the rig: an interior lit only by its cards, a10's, stays
darker than Classic away from its lamps (its typical surface at a third to a fifth of CE's light).

## The estimator

The bake's light is the path tracer's reference's (`debug.pt_reference`: no reuse, no cache, no probes, up to 8 bounces,
Russian roulette past the second), on the same scene, materials and rig, so the gate against R compares one estimator
converged against the same estimator converged:

- **The light at a point, less the sun's** (the rig's linear units: a white surface's radiance, the irradiance over pi),
  at its normal: the domes (a direction of each one's cone, its shadow traced), a lamp (below), and the sky and every
  bounce by a cosine-distributed path (the sky where it leaves the level; at each hit its glow and its albedo by the
  sun's and a lamp's light there, and the next bounce).
- **The lamps** (the emitting triangles: light cards, lamp surfaces) are picked from the **light-importance grid**: the
  level's surfaces cut into cells of 2 world units, each listing the 64 lamp triangles whose light reaches the cell's
  points most with their shadows traced (weighed on the GPU from the 128 strongest by power and distance within 48
  world units, 8 points of the cell's surfaces and 4 points of each lamp), and a point's lamp is picked from its cell's
  list by those weights; a cell whose points no candidate's light reached samples none. This leaves out the light of
  lamps outside a cell's list (beyond its reach, hidden from all its points, or weaker than its 64th): a small bias,
  which the gate against R bounds, against what a fallback by power over the whole level costs. Measured on b30's
  first BSP, such a fallback (85% of the picks from the list, 15% by power) made the lamps the bake's whole cost: a
  lamp seen at all, picked with a probability of its share of every lamp's power, is a firefly a hundred times its
  texel's mean, which the stopping rule then waits out (200 seconds of `max` finished 24 of its first tiles with the
  lamps sampled so, 17,100 with no lamps at all). A lamp's point is sampled by the solid angle its triangle subtends
  where it subtends a fair one (Arvo's spherical triangle sampling, as PBRT has it: no 1/d² spike beside a lamp), else
  by its area. Each lamp sample is resampled (RIS, as the path tracer's ReSTIR DI takes its candidates): 8 candidate
  points (4 at a path's hits), one kept by its unshadowed light there over its candidates' density, its shadow traced,
  its light by the candidates' mean weight: unbiased, and far less noisy for its one ray where the cell's lamps light
  the point unequally.
- **The direction the light comes from** is kept beside it: the first moment of the incoming luminance (an SH L1 band),
  so the light can be read at another normal than the one it was baked at (a remodel's curved face, a bump):
  `B(n) = B(n0) (a0 + k · n)`, `k` the moment over 2 pi as a share of the light, `a0 = 1 - k · n0`.
- **The fixtures:** one picked by its unshadowed light at the point among them all (16 or fewer) or 16 candidates, its
  shadow traced, its light over its share of the candidates' (exact for 16 or fewer).
- **The sun's visibility** apart: a direction of the sun's disc (`display.prism_sun_size`'s, as R's).

**When a texel stops (D4).** Each texel's samples are added until the relative standard error of its mean luminance is
at most 1% (`docs/prism_gates.md`: the standard error itself, not a confidence interval), with at least 64 samples.
The samples come in batches of 64 whose first directions are stratified (8 × 8 strata of the hemisphere, or of the
sphere for a probe), and the error is the batches' means' (8 batches at least): the samples' own variance would
overstate a stratified estimate's noise;
a texel whose mean is under 1/1000 of middle grey after 4096 samples is black; and no single sample may move the mean by
more than 1% (`prism_bake_converge.h`'s rule: a texel that has just met its first firefly is not stopped by a variance
that has not yet seen the tail). At `max` there is no sample budget. The quicker tiers stop at 64 (`draft`), 512
(`high`) and 2048 (`ultra`) samples whatever the noise.

**The tail.** Once the texels still running are too few to fill the GPU (fewer than 32,768), each one's samples are
spread over 2 to 16 invocations side by side in a subgroup (`lanes_of`): each lane takes whole batches of 64 of its own
strata (the first lane the rest of the batch the texel had begun), and their sums, their samples' statistics and their
batches' are put together (Chan's merge, by subgroup shuffles) before the texel's accumulators are written and the same
rule is asked. The samples and the rule are the ones a single invocation would have taken; the darkest texels, which
the rule keeps longest, take them in a fraction of the frames. `HALO_BAKE_NO_SPREAD` turns it off.

A texel whose first rays meet mostly one-sided faces' backs is inside the level (the stock world's overlapping
geometry); it and a texel no triangle holds take their light from their valid neighbours.

## What is baked

Each a list of jobs (`prism_bake_cache.h`'s ids), in this order:

| Step | Jobs | Representation |
| --- | --- | --- |
| The plan | On a thread: the atlas (`prism_bake_atlas.h`: CE's pages scaled chart by chart by the quality's density at each tier: `max` 16 texels a world unit at tier 0, 8 at tier 1, 0.5 at tier 2), its tiles of 16 × 16 texels, each tile's triangles, the chart maps (each CE texel's virtual page), the grid's cells and candidates, the probes | — |
| The importance grid | One job: every cell's candidates weighed (4 points of each lamp from each of the cell's 8 points, shadows traced) | A hashed table of cells, 64 lamps and cumulative weights each |
| The lightmap | A tile each: a batch of 4096 tiles on the GPU at a time, each texel's samples added between frames until it stops (the running texels packed into whole groups each frame, so that a batch whose texels are mostly done still fills the GPU); a finished tile written, drawn and replaced in the batch by the next | Per texel: the light less the sun's (RGB9E5), the direction (`k`, `a0`: RGBA8), the indirect light alone (RGB9E5, for M3's path tracer), the sun's visibility, the state and samples |
| The sun mask | A tile each where the tile's texels facing the sun are neither all lit nor all dark: its 64 × 64 points (4 times the texels' density) found on its triangles again, each traced to the sun's disc (16 directions, stratified, up to 256 while they disagree) | A byte a point; a uniform tile is a state alone (dark, lit) |
| The irradiance volume | Two cascades: 64 probes a brick (a world unit, 0.25 apart) where the player stands (the walk's floors and the two world units of air above them), and 64 a coarse brick (8 world units, 2 apart) over every reachable brick and the 12 world units of air above the floors (tree tops, the far side of a canyon: every object Raster draws has light); a batch of 262144 probes at a time | Per probe: SH L1 of the light from every direction (RGB, half floats), the sun's visibility, whether it is inside the level |
| The reflection probes | One each 8-world-unit cell of the walk's floors, a world unit above them (b30's first BSP: TBD; a10's: 25); a batch of 8 at a time | A box (how far each of its six axes reaches, 1 to 64 world units) and a cube of four levels (32, 16, 8 and 4 texels a side), each level GGX-prefiltered at its roughness ((level / 3)^2: each texel the light the bake's estimator sees along the lobe's directions about it, weighed by their cosines; 16, 64 or 256 samples a texel by the quality), RGB9E5 |
| The footprints | One job: each switchable fixture's tiles within its reach (section 2.4's relight) | The light's id, its tiles |

## The bake file

`<folder>/<map>/bsp<n>.pbk` (`prism_bake_cache.h`): a log of records, each with its job's id and checksum, appended as
jobs finish. While the bake runs it is `bsp<n>.pbk.new`, and a quit, a crash or a GPU reset resumes it at the next
unfinished job (the tiles of the batch then on the GPU are begun again); once every job is done, `DONE` is appended
and the file renamed over `bsp<n>.pbk`. Its key (the header) holds what the bake was made from: the map's checksum and
name, the BSP, a hash of the level's triangles and attributes as the bake read them, the surfaces' tiers, the materials
and alpha maps, the rig and the lamps, the reach's bricks, the baker's version, the quality and the threshold. A file of
another key is not resumed: the BSP is baked again (into `.new`) and replaces it when done.

## The settings

| Setting | Values | |
| --- | --- | --- |
| `display.lighting` | `"ce"`, `"ce_deferred"`, `"raster"` | Raster is the third |
| `display.bake` | `"auto"`, `"ask"`, `"off"` | Bake while playing (default), only when asked, never (a whole bake is still drawn) |
| `display.bake_quality` | `"max"`, `"ultra"`, `"high"`, `"draft"` | D4's `max` the default |
| `display.bake_rebake` | an integer | Each change bakes the BSP drawn again from nothing (Settings' Rebake Lightmaps) |
| `display.bake_folder` | a path | Empty: the data folder's `prism_bake` |
| `display.bake_budget` | 0.25 to 200 ms | The GPU time a frame may give a background bake (30 in the menus, 60 with `debug.bake_exit`) |
| `display.bake_indicator` | `true`, `false` | The progress at the screen's top left |
| `debug.bake_exit` | `true`, `false` | Bake in the foreground and quit once done: the measurements' runs |
| `debug.gbuffer_view` | `"bake_light"`, `"bake_sun"`, `"bake_texels"`, `"bake_volume"` | Raster's views |

Settings > Mods > PRISM LIGHTING has Lighting (Classic or PRISM Raster), Bake (While Playing, When Asked, Never), Bake
Quality, Rebake Lightmaps and Progress on Screen. The bake needs ray queries; a GPU without them draws Raster with CE's
light (D5).

## How Raster reads the bake

The G-buffer pass's level twins write each pixel's lightmap coordinates (the G-buffer's word 0, which the level does not
otherwise use) and the second target's word 2 its page (bits 17 to 23) and the lightmap pass's whole light's luminance
(bits 24 on), from `GBUFFER_DRAW`'s page, which the game sets as each lightmap's pass begins. Before the resolve a
compute pass (`raster_source`) makes the frame's opaque pixels whole, from the G-buffer, CE's base target and the bake:

- **The diffuse light.** A level pixel: its CE texel's chart map gives its virtual page; the four texels about its point
  there are read through the page's tile table (bilinear, the texels the bake has), each at the pixel's normal
  (`a0 + k · n`), and the sun's light is the rig's by the pixel's facing and the sun's visibility (the tile's state, or
  its mask, bilinear: the level's and the static scenery's shadows), less the moving objects' shadows CE drew over its
  lightmap pass (its projected shadow maps: their share of the light the G-buffer says that pass gave). An object pixel
  (the static scenery's too): the volume's eight probes about its point in its fine cascade, else its coarse one
  (those inside the level left out), SH at its normal, and the sun's light by its shadow traced against the bake's
  scene (the level and the static scenery). Both add the frame's dynamic lights (SCENE_LIGHTS' dynamic class: CE's
  falloff, 1 − d²/r²).
- **The material.** The base colour is CE's (the level's its texture pass's, the decals with it; an object's the
  G-buffer's, its combiners with white light); roughness, metalness and specular are the material table's (the G-buffer's
  word 2). The specular's share of the light is GGX's split sum at the pixel (Karis's analytic fit), F0 4% at the
  table's specular 0.5, tinted half way to the base colour by the metalness: CE's base maps are diffuse colours, metal
  or not, so the diffuse keeps what the reflection does not take (1 − its share).
- **The specular light:** the reflection probes' (the nearest few whose boxes hold the point, their cubes read along
  the reflection parallax-corrected to the box, at the roughness's level, by inverse squared distance), else the bake's
  own light from the reflection's side (the texel's or the volume's SH read at the reflection); and the sun's highlight
  (GGX, Smith's visibility, Schlick's Fresnel) by the same visibility as its diffuse light.

The pixel is then base × (1 − the specular's share) × the diffuse light + the specular light + its self-illumination
(the level's by its base colour, as CE's lightmap pass folds it in; an object's as its combiners give it), and the
resolve draws it as it is: CE's specular, reflection and object-lighting layers are not added. Where the bake has
nothing (a tile not baked yet, an object outside both cascades, the sky), the pixel is CE's own (light × base +
additions). `debug.gbuffer_view` `raster_albedo`, `raster_specular` and `raster_diffuse` show the parts.

## Measurements

See `docs/prism_gates.md`, "M2 as measured": the bake against R, Raster against R, the frame times, the bake's time
and size per map, and Classic's byte identity. In short: `ultra` bakes a BSP in one to thirty minutes (b30's first in
161 seconds on an RTX 3070 Ti, 1 GB); `max` as D4 defines it does not finish (b30's first BSP 99.6% done after 5.7
hours), its time going to the darkest texels; Raster costs 2.3 ms a frame at 1440p on the RTX 3070 Ti.

## What remains

- `max`: path guiding or a light cache between texels, or an error floor nearer the eye's (Adam's decision).
- The light cards' unit: one factor does not fit a10 and c40 alike (a10's interior, lit by its cards alone, stays two to
  ten times darker than Classic away from its lamps; c40's, with the ambient floor, 1.2 to 1.7 times brighter). Whether
  CE's radiosity power is a card's total rather than its radiance (a small card then brighter) is the next question.
- The sky's indoor ambient (CE's floor in its indoor clusters) is not taken (a10's and c40's skies have none, or the
  outdoor's).
- The moving objects' sun shadows are CE's own projected ones (on the level) and none on the objects; traced ones from
  display.prism_rt's per-frame structure of the objects (`objects_blocked` reads it where the job block has it) need the
  RT backend to build that structure in Raster, which it does not yet.
- `max`: the tail spread over a subgroup, but no path guiding or light cache yet (Adam's decision on the floor stands).
- The indirect layer's use by M3's path tracer, the remodel's atlas, BC6H for the layers and the reflection cubes.
