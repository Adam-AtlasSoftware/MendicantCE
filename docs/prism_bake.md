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

The bake reads only what the renderer already has: SCENE_STATIC's level as drawn (its near level), display.prism_rt's
materials (each environment shader's mean albedo, its alpha map, its glow; the light cards' and lamp surfaces'
emission) and its light rig (RT_FRAME's: the sun, the sky's ambient, the domes, the lamps, at the BSP's fitted scales
where `prism_lightmaps/rebaked/<map>_<bsp>.scales.json` has them). In Raster the game sends display.prism_rt's scene
and rig whatever `display.prism_rt` says, with nothing traced in the frame (RT_FRAME's mode off).

The map's own triangles are wound clockwise, as D3D draws them: their corners' cross product points into the level.
SCENE_STATIC now turns each of them to its vertices' normals (`prism_scene_static.c`), so that every consumer's face
normal points out of the level; before M2, display.prism_rt's one-sided faces were inside out with the stock world
traced as drawn (the remodel, two-sided, hid it).

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
| The irradiance volume | 64 probes a brick (a world unit, 0.25 apart), the bricks where the player stands (the walk's floors and the two world units of air above them); a batch of 262144 probes at a time | Per probe: SH L1 of the light from every direction (RGB, half floats), the sun's visibility, whether it is inside the level |
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
compute pass (`raster_source`) lights the light target:

- **A level pixel:** its CE texel's chart map gives its virtual page; the four texels about its point there are read
  through the page's tile table (bilinear, the texels the bake has), each at the pixel's normal (`a0 + k · n`); the sun's
  light is the rig's by the pixel's facing and the sun's visibility (the tile's state, or its mask, bilinear); the
  self-illumination CE folds into its lightmap pass is added; and what CE drew over its lightmap pass's light (the
  objects' shadows, the dynamic lights: the light target's pixel against that light) is kept, the shadows as a share of
  the baked light and the lights added.
- **An object pixel:** the volume's eight probes about its point (those inside the level left out), SH at its normal,
  and the sun's light by the probes' visibility, by the pixel's base colour (CE draws an object whole into the light
  target, its base target white).
- Where the bake has nothing (a tile not baked yet, a pixel outside the volume), CE's light stays.

The resolve then puts the frame together as `ce_deferred` does: light × base + CE's specular and reflection additions.

## Measurements

See `docs/prism_gates.md`, "M2 as measured": the bake against R, Raster against R, the frame times, the bake's time
and size per map, and Classic's byte identity. In short: `ultra` bakes a BSP in one to thirty minutes (b30's first in
161 seconds on an RTX 3070 Ti, 1 GB); `max` as D4 defines it does not finish (b30's first BSP 99.6% done after 5.7
hours), its time going to the darkest texels; Raster costs 2.3 ms a frame at 1440p on the RTX 3070 Ti.

## What remains

- `max`: path guiding or a light cache between texels, or an error floor nearer the eye's (Adam's decision).
- The static scenery (trees, rocks) traced by the bake as R traces it, and their sun shadows drawn by Raster.
- The rig's lamps calibrated where no fitted scales exist (a10, c40: Raster darker than Classic there, as R would be).
- The reflection probes (not baked yet), the indirect layer's use by M3's path tracer, the remodel's atlas, BC6H.
