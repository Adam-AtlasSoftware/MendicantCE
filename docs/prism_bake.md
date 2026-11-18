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
included; no map's fit (`scales.json`) is read. CE's lightmap holds the light its radiosity found (1.0 is the page's
white), and CE's frame is that light times the base map, so the bake's light goes into Raster's frame as CE's lightmap
does. The units are Tool's radiosity's, as far as the shipped pages and the public accounts of Tool's lightmapper
(c20's tag pages; two re-implementations' notes, unreviewed) agree:

| Light | Its power is | In the bake's unit |
| --- | --- | --- |
| The sun (the sky's narrowest light) | the light it gives a white surface facing it (a directional light, no 1/pi) | colour × power |
| The domes (the sky's other lights) | the same | colour × power |
| The sky's ambient radiosity | the open sky's radiance (`display.prism_sky_light` `"sky"`, the default): a point under the open sky gets colour × power, one under cover less | colour × power, seen where a path leaves the level and passed on by the bounces; nothing where no sky is seen. `"flat"`, Tool's: a floor of light, unoccluded, at every texel and probe, not bounced (the outdoor values in the structure's clusters with a sky, sky 0's indoor values in those with none) |
| A light card or lamp surface (radiosity power) | its radiance (the patch's radiosity, shot once) | colour × power |
| A light fixture (intensity) | the irradiance it gives a surface facing it a world unit away | colour × intensity / d² |

Raster's frame rolls its light past white off by its tone curve (`"filmic"` by default), after the exposure and before
the base; under the `"ce"` curve it holds it at white as the page held it (a colour past white divided by its brightest
channel, its hue kept; "Raster's exposure" below). The bake's light and R's are neither: the gates compare them as they
are.

What the units rest on (November 2026):

- **The sun saturates the page.** b30's first BSP's 26 pages: 39.1% of the texels they use have their brightest channel
  at 255, 39.6% within a 5-bit step of it, and 7% between 128 and 254; the rest is shade, 8 to 60 of 255. The sun's
  colour by power there is 1.5 (0.91 0.89 0.80 by 1.70): a sun of power 1 or more whites out a face that looks at it,
  which colour × power / pi (0.48) never does. CE's frames agree: the Forerunner structure's sunlit light (the frame
  over the base) is flat at the page's white from a cosine of 0.35 up.
- **The shade needs no fitted floor.** With each kind of light baked alone at 1 (`debug.prism_rig_scales`; draft, the
  bounce by the linear albedo then), CE's light target over the bake's, the median per pixel at G2's and G3's poses:
  the sunlit pixels 0.84 to 0.87 with the sun at colour × power and the page's hold (the target is under the page's
  white there by its own factor, below), against 1.19 to 1.23 with the sun over pi; the shade 1.11 to 1.67 with the
  sun's bounce at colour × power and the flat ambient alone (b30's shade against the raw page with the rig as it now
  is: 0.93 to 1.01 at G2, 0.66 to 0.75 under G3's cliff). The floor the first fit put at 3.1 to 4.1 times the ambient
  (5.9 to 7.9 traced as a radiance from the open sky, more under cover) was the sun's and the domes' bounce missing its
  pi.
- **The sky is a light** (`display.prism_sky_light` `"sky"`, the default since the lighting is to look path traced;
  `sky_light()` in the bake, the rig's word 61 at 2): its ambient colour × power is the open sky's radiance, seen where
  a path leaves the level (a ray that meets nothing within 300 world units, the bake's and R's alike) and passed on by
  the bounces. In the light's unit (the irradiance over pi) a point under the whole open sky gets colour × power, Tool's
  ambient, and one under cover less: the contact shadows and the occlusion a path-traced frame has. There is no floor:
  a room with no sky (a10's, c40's chamber) has its lamps' light and what comes in. The sky's rays have no test
  distance (the sun's and the domes' keep theirs). Against the flat fill (draft bakes, `"auto"` exposure, the
  frames' medians, 3070 Ti): b30's sunlit beach unchanged (0.98 to 1.03), the shade under G3's cliff 0.82 to 0.83,
  c40's moonlit canyon 0.73 to 0.76 (now at Classic's, 0.11 to 0.16), its chamber 0.77 to 0.92, b30's interior 1.03
  to 1.17.
- **Tool's fill, the other choice** (`"flat"`, the word at 1): the ambient flat and by the cluster's sky (Tool's, as c20
  and both accounts give it: no visibility ray). An interior's clusters have no sky: c40's chamber takes sky 0's indoor
  values (0.066 0.088 0.128), a10's none (its sky's ambient powers are 0). `prism_scene_static.c` marks each drawn
  triangle of an indoor cluster (its middle a little off its face, the collision BSP's leaf and the leaf's cluster:
  `XGPU_SCENE_INDOOR`); a texel takes its triangle's floor, a probe the floor of the nearest structure the six axes meet
  (`floor_at`), R's pixel the floor of the structure it shows (an object's, the structure below it).
- **The cards' unit is 1:** a receiver gains the emitter's colour × power by the form factor, the bake's radiance.

Kept on purpose with the path-traced look (November 2026): the sky lights' test distance (a canyon without its moon is
not tasteful; a map's own override of the light is the later answer), CE's composition as Raster's default (the
textures' and the materials' style, which is to match Classic's), the linear albedo, and every bounce (up to 32).

**CE's light target** (`ce_deferred`'s light, `debug.gbuffer_view` `light`, which every fit here is measured against)
is CE's lightmap pass: the page's light by a mix of 1 and the bump map's dot product with the vertex's incident radiosity
direction (a normalisation cube map; the mix by the vertex's alpha), plus the self-illumination. The detail maps are
the base pass's, not in it. So the target is at or under the page's light, and its texture (a door's scratches, a
panel's mottling) is the bump map's: where the light comes from one side at a slant (a wall under ceiling lamps) the
target is under the page by that factor, a bias of every fit against it (by the pixel, not by the shader).

The sky the rig takes is the BSP's own (the one most of its clusters have), whatever the window sees, so that a bake's
rig is the same wherever its camera was.

**The bounces' albedo.** In the light's units a surface's albedo is its base map's mean **made linear** (a byte to
the power 2.2, its detail maps' factors as CE's frame applies them; 0.03 to 0.85), the reflectance of the linear light
the bake carries and the one the path tracer's materials reflect with: the bake's, R's and the drawn objects' alike
(`prism_rt_albedo_stored` false; `HALO_PRISM_ALBEDO` `stored` takes the base map's mean as stored, the byte over 255, a
diagnostic). Decided (November 2026) from the bake's light over CE's raw page (the median of the level's pixels; draft
bakes, the plain paths, 32 bounces, the doors out, the sky lights' test distances, the flat ambient by the cluster's
sky):

| Place (the bake over the page) | Linear | Stored |
| --- | --- | --- |
| a10's cryo bay | 0.95 | 1.59 |
| a10, the wall behind p1's windows | 0.51 | 0.96 |
| a10, p2's corner | 0.49 | 1.26 |
| c40's chamber | 1.49 to 1.58 | 1.64 to 1.70 |
| c40's canyon, moonlit | 1.22 to 1.33 | 1.43 to 1.56 |
| c40's canyon, where the moon is not | 1.35 to 1.40 | 1.93 to 1.99 |
| b30, G2 sunlit | 0.92 to 1.09 | 1.04 to 1.63 |
| b30, G2 shade | 0.93 to 1.01 | 1.19 to 1.26 |
| b30, G3 sunlit | 0.88 to 0.89 | 1.06 to 1.07 |
| b30, G3 shade | 0.66 to 0.75 | 1.21 to 1.28 |

The rms of log2(bake / page) over the ten places is 0.55 with the linear mean and 0.52 with the stored one; neither is
within 30% of the page everywhere. The linear mean misses a10's far corners (half the page), c40's chamber (1.5), its
canyon (1.3 to 1.4) and b30's deep shade under its cliff (0.7); the stored mean is over almost everywhere (1.2 to 2.0),
a flatter error the exposure would absorb, but a reflectance the linear light does not have, which carries the walls'
colour into the light. Tool's own pages hold far less bounce than a converged solve (c40's moonlit snow is the moon and
the ambient alone; 28% of a10's texels are black, with no floor), which the weaker linear bounce is nearer. The stored
mean was taken at first (on a10's far corners, 0.14 to 0.43 of CE's light made linear) while the bake still had the
doors in it (they shaded those corners, Tool never had them) and the old rig (the sun over pi, the floor at pi); with
both gone the comparison above undid it. The rule changes no more without a whole-BSP comparison in lightmap space.

The cards' unit stays 1: the level a place is off by is one factor, the exposure's (Raster's exposure below). The ceiling
matters little (0.95 for 0.85 moves the light 1 to 4%). The units' first check against CE left out the pixels whose
base view is white (base 0.98 and over), 70% of a10's p1, the light walls where the gap was widest; a10's skies have no
indoor ambient (0 by 0), so the gap was not that floor's.

## The estimator

The bake's light is the path tracer's reference's (`debug.pt_reference`: no reuse, no cache, no probes, up to 32 bounces,
Russian roulette past the second; 8 cut a room with walls of albedo 0.85 short), on the same scene, materials and rig, so the gate against R compares one estimator
converged against the same estimator converged:

- **The light at a point, less the sun's** (the rig's linear units: a white surface's radiance, the irradiance over pi),
  at its normal: the domes (a direction of each one's cone, its shadow traced), a lamp (below), and the sky and every
  bounce by a cosine-distributed path (the sky where it leaves the level; at each hit its glow and its albedo by the
  sun's and a lamp's light there, and the next bounce).
- **The lamps** (the emitting triangles: light cards, lamp surfaces) are picked from the **light-importance grid**: the
  level's surfaces cut into cells of 2 world units, each with two lists of up to 64 lamp triangles: the *seen*, whose
  light reaches the cell's points most with their shadows traced (weighed on the GPU from the 128 strongest by power
  and distance within 48 world units, 8 points of the cell's surfaces and 4 points of each lamp), and the *near*, the
  strongest of those 128 by their unshadowed light at the cell's middle. A point's lamp is picked from the seen by
  70% of the picks, from the near by 25% (95% where the cell saw none), and by power over the whole level by the
  rest, its probability the three's mixture: every lamp can be picked, so the bake's lamps are R's, unbiased. The
  near and the power share are the defensive part: a lamp the cell's few points did not see that the point does is
  picked with a fair probability (its near weight), and a far one with its share of the level's power, where its
  light is small. The first round had the seen alone (a cell whose points saw no lamp sampled none): on a10 the bake's
  light came out at 0.4 to 0.9 of R's (a third of its cells saw no lamp), and the light cards' unit was fitted at 2 to
  make up for it. Against a bake whose every pick is by power (`HALO_BAKE_TERMS` 32, unbiased and noisier) the
  mixture's light agrees to 0.3 to 10% on a10's poses at `high`. A lamp's point is sampled by the solid angle its
  triangle subtends where it subtends a fair one (Arvo's spherical triangle sampling, as PBRT has it: no 1/d² spike
  beside a lamp), else
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
- **The sky lights' rays** (the sun's, the domes') are tested as Tool's radiosity tests them: as far as the light's test
  distance (its tag's field: c40's moon 30 world units, b30's sun and dome 50; the rig's `rig[63]` and each dome's
  `[7]`), and a face met from its back (a one-sided face of the level seen from behind) lets them through. c40's canyon
  under G6 is roofed along the moon's direction about 105 world units up (the collision BSP agrees with the render
  geometry), and CE's page has the moon on its floor: with the rule, 43 to 50% of G6's level pixels see the moon (none
  before), in the page's colour, though the bake there is now 1.6 to 1.7 times the page's light. b30 does not change
  (G2's and G3's sunlit and shaded pixels against the page within 2%). R takes the same rule.
- **Every lamp of the cell's lists** (`lamps_all`): past the draft, a texel takes each sample the light of every lamp
  in its cell's two lists (a point of each by solid angle or area, its own shadow traced) and the level's other lamps
  by one pick by power, counted only where the picked lamp is in neither list; the draft and the paths' hits keep the
  resampled pick. Unbiased (a10 at `high`, texel by texel: the whole BSP's light equal to the pick's to 0.1%, the
  median texel to 0.1%); at `ultra` on a10 the texels' light-weighted noise between two seeds is 8.4% against 11.8%
  (rms 22.0% against 25.6%, median 5.6% against 7.8%), for about twice the direct light's cost (219 s against 110 s).
- **The final gather** (`path_gathered`, a diagnostic, off unless `HALO_BAKE_GATHER`): past the draft, a bounce's ray
  that meets the level takes the bake's own light there (the hit's texels in the tiles' pool, bilinear, at the hit's
  normal, less the floor of light) by the hit's albedo, plus the sun's there and the hit's glow, in place of the hit's
  lamp pick and the paths past it; a hit with no texel takes the path. It comes out over the paths, 0.4 to 0.6% over a
  whole BSP and about 3% at the median texel, more in dark texels (a10 at `high`), and R sides with the paths; the
  suspects are the cache's neighbour-filled texels inside the level and its bilinear reads at a chart's edge. Its noise
  gain alone was small (11.8% to 10.9%).

**When a texel stops (D4).** Each texel's samples are added until the relative standard error of its mean luminance is
at most 1% (`docs/prism_gates.md`: the standard error itself, not a confidence interval), with at least 64 samples.
The samples come in batches of 64 whose first directions are stratified (8 × 8 strata of the hemisphere, or of the
sphere for a probe), and the error is the batches' means' (8 batches at least): the samples' own variance would
overstate a stratified estimate's noise;
a texel whose mean is under 1/1000 of middle grey after 4096 samples is black; and no single sample may move the mean by
more than 1% (`prism_bake_converge.h`'s rule: a texel that has just met its first firefly is not stopped by a variance
that has not yet seen the tail). At `max` there is no sample budget. The quicker tiers stop at 64 (`draft`), 512
(`high`) and 2048 (`ultra`) samples whatever the noise.

**`max`'s noise floor (D4 amended, 2026-11-17).** At `max` the error is measured against the larger of the texel's own
mean and a tenth of middle grey (0.018): a texel stops once the standard error of its mean luminance is at most
0.01 × max(mean, 0.1 × middle grey), and the firefly rule likewise (no sample moves the mean by more than 1% of that).
The other rules are M0's (at least 64 samples; black below 1/1000 of middle grey after 4096). `debug.bake_noise_floor`
(0.1) sets the share of middle grey; 0 is the literal rule (`prism_bake_converge.h`'s `error_floor`, which the tests
keep at 0). The quicker tiers keep the literal rule under their sample caps. The log counts the texels the floor
stopped (converged with their light below it).

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
| The reflection probes | One each 8-world-unit cell of the walk's floors, a world unit above them (b30's first BSP: 923; a30's: 2,185; a10's: 25); a batch of 8 at a time | A box (how far each of its six axes reaches, 1 to 64 world units) and a cube of four levels (32, 16, 8 and 4 texels a side), each level GGX-prefiltered at its roughness ((level / 3)^2: each texel the light the bake's estimator sees along the lobe's directions about it, weighed by their cosines; 16, 64 or 256 samples a texel by the quality), RGB9E5 |
| The footprints | One job: each switchable fixture's tiles within its reach (section 2.4's relight) | The light's id, its tiles |

## The bake file

`<folder>/<map>/bsp<n>.pbk` (`prism_bake_cache.h`): a log of records, each with its job's id and checksum, appended as
jobs finish. While the bake runs it is `bsp<n>.pbk.new`, and a quit, a crash or a GPU reset resumes it at the next
unfinished job (the tiles of the batch then on the GPU are begun again); once every job is done, `DONE` is appended
and the file renamed over `bsp<n>.pbk`. Its key (the header) holds what the bake was made from: the map's checksum and
name, the BSP, a hash of the level's triangles and attributes as the bake read them, the surfaces' tiers, the materials
and alpha maps, the rig (with the port's words past its settings: the indoor floor, the fixtures' scale, the units and
the sky's kind, the albedo's rule, the sun's test distance) and the lamps, the reach's bricks, the baker's version, the
quality and the threshold. A file of
another key is not resumed: the BSP is baked again (into `.new`) and replaces it when done.

**The reach's file.** The walk that finds where the player can be (the reach: the bricks the probes fill, the
surfaces' tiers) is kept beside the BSP's bake (`<map>/bsp<n>.reach`, keyed by a hash of its inputs), so a BSP drawn
again takes it at once: b30's first BSP's walk takes 16 s on the CPU, during which Raster drew CE's light. Its BSP
change (b30's beach into the structure and back, 3070 Ti) now holds the frame 0.9 and 1.9 s, Raster's light whole
again 1.5 to 3 s after it (the tiles loaded 2048 a frame), where before the frame stopped 20 s going in (the path
tracer's lamp grid, made on the frame's thread for the structure's 1871 lamps although nothing was traced: now made at
the first traced window) and the beach stayed CE's light 16 s coming out.

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
| `debug.gbuffer_view` | `"bake_light"`, `"bake_sun"`, `"bake_texels"`, `"bake_volume"`, `"bake_total"` | Raster's views |
| `display.prism_raster_composition` | `"ce"`, `"materials"` | CE's shaders compose Raster's light (default), or PRISM's own materials |
| `display.prism_raster_objects` | `"volume"`, `"classic"` | Objects by the volume's light (default), or never lit under Classic |
| `display.prism_raster_object_shadows` | `"traced"`, `"classic"` | The moving objects' sun shadows traced (default), or CE's projected ones |
| `display.prism_sky_light` | `"sky"`, `"flat"` | The sky a light, by what each point sees of it (default), or Tool's flat fill; a change bakes again |
| `display.prism_exposure`, `_range`, `_speed`, `_bias`, `display.prism_tone_curve` | as the path tracer's | Raster's exposure and tone curve (its style's: `"auto"` and `"filmic"`) |

The bake's diagnostics (environment variables): `HALO_BAKE_TERMS` leaves terms out (1 the domes, 2 the lamps, 4 the
glows, 8 the paths past the first hit, 16 the fixtures, 32 the grid, 64 the lamps' spherical sampling; a texel's 128
indirect light, 256 own lamps' direct light; 512 every lamp of the cell's lists in the draft too, 1024 the resampled
pick past it), `HALO_BAKE_SEED` another seed (two bakes' noise against each other), `HALO_BAKE_GATHER` the final
gather, `HALO_BAKE_EMITTERS_PASS` the level's emitting faces neither blocking nor reflecting, `HALO_PRISM_ALBEDO`
`stored` the base maps' stored mean; `HALO_RASTER_EXPOSURE_LOG` logs Raster's exposure and its meter's medians each
frame.

Settings > Mods > PRISM LIGHTING has Lighting (Classic or PRISM Raster), Bake (While Playing, When Asked, Never), Bake
Quality, Sky Light, Rebake Lightmaps and Progress on Screen, and Raster's look: Exposure, Highlights and Objects. The bake needs ray queries; a GPU without them draws Raster with CE's
light (D5).

## How Raster reads the bake

The G-buffer pass's level twins write each pixel's lightmap coordinates (the G-buffer's word 0, which the level does not
otherwise use) and the second target's word 2 its page (bits 17 to 23) and the lightmap pass's whole light's luminance
(bits 24 on), from `GBUFFER_DRAW`'s page, which the game sets as each lightmap's pass begins. Before the resolve a
compute pass (`raster_source`) makes the frame's opaque pixels whole, from the G-buffer, CE's base target and the bake:

- **The diffuse light.** A level pixel: its CE texel's chart map gives its virtual page; the four texels about its point
  there are read through the page's tile table (bilinear, the texels the bake has), each at the pixel's normal
  (`a0 + k · n`), and the sun's light is the rig's by the pixel's facing and the sun's visibility (the tile's state, or
  its mask, bilinear: the level's and the static scenery's shadows) and the moving objects' shadows, traced (below; with
  `display.prism_raster_object_shadows` `"classic"`, or where the frame has no structure of its objects, those CE drew
  over its lightmap pass instead: its projected shadow maps, their share of the light the G-buffer says that pass
  gave). An object pixel
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

**The moving objects' sun shadows** (`display.prism_raster_object_shadows` `"traced"`, the default): the game sets
RT_FRAME's `XGPU_RT_FLAG_RASTER_OBJECTS` with its mode off, and the RT backend (`xgpu_vulkan_rt.c`) takes a slot of its
own for the frame, captures each object draw of the G-buffer pass as the path tracer captures the pre-pass's (the same
`xgpu_vulkan_rt_capture`: the vertices where the GPU has them, each draw's entry and bottom-level structure kept from
frame to frame, skinned and refitted on the GPU only where it moved; the first-person weapon, its G-buffer instance,
left out, as CE never let it cast a shadow, and the static scenery the bake holds, whose shadows are baked: the game
marks its draws, `XGPU_GBUFFER_FLAG_STATIC_SCENERY`, by the bake's own rule, `prism_bake_object_static`; b30's G2
pose went from 703 draws to 175) and at the pass's end runs the path tracer's own `objects_record` and
`scene_build`, nothing traced there. The raster job then has the top level's address and the objects' table
(`xgpu_vulkan_rt_objects`), and `objects_visibility` traces one ray a pixel to the sun against the objects (masks 4 and 8: an
opaque one's hit blocks it, each cutout crossed lets 1 - 0.75 x its share of texels drawn through, a smooth shade
where the path tracer's stochastic hit, undenoised, was a dither) from every lit pixel the sun faces: the level's, so the vehicles,
characters, weapons, crates and the Pelican shadow the ground, and the objects', so they shadow each other and
themselves. CE's projected shadows on the level are then left out (they were the same shadows, softer and drawn in
CE's own direction). With `"classic"`, or a GPU without ray queries, nothing is captured and CE's stay.

**What composes the frame** (`display.prism_raster_composition`): by default (`"ce"`) CE's own shaders, until PRISM's
materials reach theirs: the diffuse light (exposed, through the tone curve, below) goes into CE's light target in its light's
place (the level's glow folded in as CE's lightmap pass folds it; an object's base colour, the G-buffer's, and glow
with it), and the resolve draws CE's base colour times it and CE's additions (its reflections, specular lightmaps and
specular lights) over it, as Classic's frame is drawn; the meter then takes both frames with the same base and
additions. `"materials"` (for the materials milestone) composes the frame whole from PRISM's own materials, as follows.

With `"materials"` the pixel is base × (1 − the specular's share) × the diffuse light + the specular light + its
self-illumination (the level's by its base colour, as CE's lightmap pass folds it in; an object's as its combiners give
it), the exposure on both lights and the tone curve over the whole (below), and the resolve draws it as it is: CE's
specular, reflection and object-lighting layers are not added. Under the `"ce"` curve the diffuse light (the level's
and the objects', the sun's in it) is held at white as CE's page held its light (a colour past white divided by its
brightest channel, its hue kept), after the exposure and before the base, so a sunlit wall whites out as Classic's
does, and a sunlit object beside it with it. Where the
bake has nothing (a tile not baked yet, an object outside both cascades, the sky), the pixel is CE's own (light × base +
additions). `debug.gbuffer_view` `raster_albedo`, `raster_specular` and `raster_diffuse` show the parts; the views and R
are before the exposure and the curve.

### Raster's exposure

`display.prism_exposure` (and its `_range`, `_speed` and `_bias`) and `display.prism_tone_curve` reach Raster's frame
from the path tracer's settings. Raster's `"style"` is an eye's and a film's: `"auto"` and `"filmic"` (November 2026:
the lighting is to look path traced, a place darker than Classic where its light is darker; `"ce"` and `"ce"`, the
frame's middle as bright as Classic's and the light held at white, are the other choice).

- **The meter** (`xgpu_vulkan_bake_glsl.inc`'s `EXPOSURE_COMMON`): in the frame whole, a lit pixel in 16 (every 4th
  each way) adds two luminances to two histograms (log2 from −12 to 4, a quarter of a stop a bin, in one buffer,
  `xgpu_vulkan_bake_meter`): Raster's frame there before the exposure and the curve (with CE's composition the baked
  light by CE's base, CE's additions added; with `"materials"` the bake's light by the material, the specular light,
  the glow) and Classic's (CE's light target by its base, its additions added).
- **The step** (`exposure_source`, one invocation after the frame's pass, `xgpu_vulkan_bake_exposure_step`):
  - `"auto"` moves toward a key, 0.30 (b30's sunlit beach then comes out as bright as Classic's), by half the stops
    between the key and the frame's median where the frame is brighter and a quarter where it is darker, at most the
    range up (1.5 stops) and two thirds of it down (1 stop), plus the bias. An eye takes in bright light more than dim:
    a dark place stays darker than a bright one, a night a night.
  - `"ce"` aims the frame's median at Classic's, within the range.
  - `"fixed"` applies none.
  - It moves toward its aim at the speed (2 stops a second by the host's clock), at once the first time, keeps the
    exposure for the next frame, and empties the histograms. A frame with no lit pixel keeps the exposure it had.
- **The exposure** multiplies the light (with `"materials"` the diffuse and the specular light); the glow is CE's and
  stays. **The tone curve** then: under `"ce"` the light is held at white (a colour past white divided by its
  brightest channel, its hue kept) before CE's base multiplies it, as CE's page held its light; under any other curve
  the light goes to the curve unclamped, so the sun past white rolls off instead of sitting flat at white:
  `"filmic"` (and `"local"` here) the light as it is up to 0.6 of white and past it rolled toward white by its
  brightest channel, `"aces"` Narkowicz's fit. With CE's composition the curve is over the light, before the base;
  CE's additions (its reflections and specular layers), added after, are not rolled off. With `"materials"` it is over
  the frame whole.

Why a quarter up (draft bakes, the linear albedo; `HALO_RASTER_EXPOSURE_LOG`, the frames at 1280×720, 30 frames a second,
4 s a pose; the medians of the level's and the objects' pixels). The frame's median before the exposure, at G2, G3, G6
and G7's poses, against Classic's at the same pixels:

| Scene | Raster before the exposure | Classic | `"auto"`, half both ways: its exposure, the frame's median | Classic's frame | `"auto"` as chosen: its exposure |
| --- | --- | --- | --- | --- | --- |
| b30 G2, the sunlit beach | 0.55, 0.39, 0.65 | 0.32, 0.27, 0.32 | −0.43, −0.18, −0.56 stops: 0.39, 0.32, 0.36 | 0.38, 0.28, 0.32 | the same |
| b30 G3, the shade under the cliff | 0.10 | 0.11 to 0.14 | +0.82: 0.19 | 0.14 to 0.16 | +0.41 |
| c40 G6, the canyon at night | 0.19, 0.14, 0.14 | 0.16, 0.11, 0.11 | +0.32, +0.57, +0.57: 0.23, 0.19, 0.18 | 0.15, 0.11, 0.11 | +0.16, +0.28, +0.28 |
| c40 G7, the chamber's lamps | 0.16, 0.14, 0.16 | 0.10, 0.08, 0.10 | +0.44, +0.57, +0.44: 0.17, 0.17, 0.18 | 0.11, 0.10, 0.09 | +0.22, +0.28, +0.22 |

The sunlit beach's light is over Classic's before any exposure (the sun past the page's white), and half the stops
down bring it to Classic's frame. c40's canyon and chamber are already 1.2 to 1.7 times Classic's light in their own
(the bake over the page there: Tool's short bounce, "What remains"), and half the stops up put them 0.3 to 0.8 stops
over Classic's frames: the night washed toward dusk. A quarter up holds them near their own light.

Raster's lit value is last linear and unclamped in `raster_source`'s `light_`, the exposure applied, before the hold
or the tone curve; `bake.raster_light` (RGBA16F) holds it after them (with `"materials"`, the frame whole after the
curve) until the resolve draws it into the frame's target (`xgpu_vulkan_post.c`'s `DEFERRED_RESOLVE`: with CE's
composition its light × CE's base + CE's additions, with `"materials"` view 2, the frame as it is). The composite to
come takes the same meter (`meter_add`) and step.

## Measurements

See `docs/prism_gates.md`, "M2 as measured": the bake against R, Raster against R, the frame times, the bake's time
and size per map, and Classic's byte identity. In short: `ultra` bakes a BSP in one to thirty minutes (b30's first in
161 seconds on an RTX 3070 Ti, 1 GB); `max` as D4 defines it does not finish (b30's first BSP 99.6% done after 5.7
hours), its time going to the darkest texels; Raster costs 2.3 ms a frame at 1440p on the RTX 3070 Ti.

## What remains

- `max`: path guiding or a light cache between texels, or an error floor nearer the eye's (Adam's decision).
- R where lamps light a scene: at 4096 frames it is not converged (its median relative error 27 to 38% at G4,
  `docs/prism_gates.md`), and it comes out below the bake's light at a10 (0.6 to 0.9 of it on average, far darker at a
  tenth of its pixels): the bake against R there measures R's noise as much as the bake.
- PRISM's own materials (`"materials"`) leave the frame darker than Classic's at equal light: Raster's frame (the
  exposure fixed) over Classic's, over the bake's light over CE's light target (the frames' and the lights' medians,
  draft bakes): a10 0.42 to 0.47, c40's chamber 0.63 to 0.75, b30 0.86 to 1.01 (G2) and 0.89 to 0.91 (G3). The
  specular's share is taken from the diffuse (a metal's most), and CE's additions (its reflection cube maps, specular
  lightmaps and specular lights) are not drawn: a debt of the materials milestone, which `"ce"` leaves out.
- **Objects by the volume** (`display.prism_raster_objects` `"volume"`, the default since the lighting is to look
  path traced): an object is lit by the volume's light alone, and so darker than Classic draws it where CE's object
  lighting is generous: the Chief in c40's chamber at 0.022 to 0.024 against Classic's 0.122. The volume there holds
  0.26 to 0.27 (the light's unit, bluish), 0.4 of the lightmap's light on the floor under him (0.65): his sides face the
  walls, the lamps are above. CE lights him at about 1.36 (his Classic colour over his base colour), four times its own
  floor light there (0.31): CE's object lighting is generous, not the volume wrong. `"classic"`, the other choice:
  an object's pixel lit by the volume under the light CE gives that pixel (CE's light target there, the object's lit
  colour) is raised to it, its colour kept, so what the game shows stays seen.
- Objects in the sun come out 15 to 30% brighter beside the sunlit ground than Classic draws them (Raster over Classic
  at G2's poses: sunlit objects 1.14 to 1.33, the sunlit level 0.88 to 1.16; in the shade alike, 0.90 to 1.04): the
  volume's light and the traced sun against CE's object lighting.
- **The bake against CE's raw page** (the lightmap pass without the bump's incident radiosity,
  `rasterizer_lightmaps_incident_radiosity 0`: 0 to 11% over CE's light target): the table under "The bounces'
  albedo" (the linear column). Tool's pages hold far less bounce than a converged solve: c40's moonlit snow is the moon
  at colour × power × cos and the flat ambient alone (0.33 to 0.39 against 0.36), where the bake has its bounce too
  (0.55 to 0.60), and in the moon's shade the page is half the bake's light; 28% of a10's texels in its own pages are
  black, with no floor (the darkest values fall away from 0 smoothly). So the lamp-lit rooms and the canyon over the
  page are Tool's short bounce, which the rig does not copy; the exposure carries the level in the frame. Ruled out on
  the way: the target's bump term, the emitting faces' blocking (letting them pass makes a10 brighter), the sky's floor
  in indoor clusters. A whole-BSP comparison in lightmap space is where it would be looked at again.
- The sun's half-lit band (partial shadows, low cosines) comes out over CE's light (CE/bake 0.5 to 0.8 at G2 and G3):
  Tool's sun is sixteen directions spread over its disc, on the vertices, so its shadows' edges are softer and coarser
  than the bake's traced disc: a place where the bake is meant to differ.
- The moving objects' traced sun shadows are one ray a pixel to the sun's centre: hard edged (CE's projected ones were
  soft blobs), and an object off screen casts none, as it is not drawn in the G-buffer pass.
- `max`: the tail spread over a subgroup, but no path guiding or light cache yet (Adam's decision on the floor stands).
- The indirect layer's use by M3's path tracer, the remodel's atlas, BC6H for the layers and the reflection cubes.
