# PRISM's in-game baker and PRISM Raster

PRISM v4's milestone M2 (`overhaul_research/prism_v4_renderer/design.md`, sections 3 and 1.8): each level's light is
baked on the player's own machine, by the GPU, from the level the game draws, and `display.lighting` `"raster"` draws
the game lit from that bake, with no ray tracing while playing. This document describes what is baked and how, the
bake file, the settings, how Raster reads the bake, and what was measured. The gates' figures are in
`docs/prism_gates.md` ("M2: the bake and PRISM Raster").

**Players never bake (v4 geopatch M3, `docs/prism_settings.md`).** The baker below is the farm's and the developers':
`display.bake`, `_quality`, `_rebake`, `_indicator`, `_budget`, `_folder` and `display.prism_rig` are no player's settings
(never read from config.toml; `HALO_BAKE=auto` and the rest on the farm), so the main menu's bake, its questions
(PLAY NOW, REBAKE) and the Light Bake page described below never run or show for a player, and Enhanced lighting
(`display.lighting` `"enhanced"`, Raster below) and Path Traced draw the shipped MAX lightmaps alone. The enhanced
world's bakes are keyed to each BSP's `world_id` (`docs/prism_geometry.md`).

A release ships every campaign level's light baked at MAX for both worlds, lighting only ("The shipped bake", below):
the player's machine uses it wherever its key is that machine's own BSP's, and bakes only what it does not cover (a
mod, a custom map). A bake made on the player's machine stays there (`<data>/prism_bake`, or `display.bake_folder`).

## What runs where

| Part | Where | What |
| --- | --- | --- |
| The control | The game, `port/linux/game/prism_bake.c` | Each frame, `PRISM_BAKE`'s control (`xgpu_stream.h`): the map and BSP drawn, `display.bake`, `display.bake_quality`, the rebakes asked for, the bake folder, the GPU time a frame may give the bake, and where the renderer writes how the bake goes (the status block) |
| The reach's inputs | The game, the same file | Once a BSP: the collision BSP, the clusters and their PVS, the water, the render triangles, the scenario's places and the fighters it places, serialised and sent a slice a frame |
| The reach | The renderer, `port/linux/vulkan/xgpu_vulkan_bake.c` (a thread of its own) | The reachable space (`prism_bake_reach.c`): the walk over floors from the scenario's places, the camera volume, flight where a fighter the player can reach is placed; each render surface's importance tier; the volume's bricks. A large BSP's walk is millions of cells (b30's: 8 million), more than the game's memory holds, so it runs on the 64-bit side |
| The bake | The renderer, the same file and `xgpu_vulkan_bake_glsl.inc` | A slice of each frame's GPU work: the passes below, into the bake file |
| PRISM Raster | The renderer, the same files | The light target lit from the bake before the deferred resolve (`xgpu_vulkan_post_deferred_resolve`'s relight) |
| The indicator | The game, `port/linux/game/prism_bake_indicator.c` | "Baking lightmaps 37%" at the screen's top left while a level is baked |
| The main menu's bake | The game, `port/linux/game/prism_bake_menu.c` | The campaign's levels baked on the main menu by a worker (the game again, hidden, `debug.bake_worker`), its progress under the menu, the note of the BSPs baked, the questions ("The bake on the main menu") |

The bake reads what the renderer already has: SCENE_STATIC's level as drawn (its near level), display.prism_rt's
materials (each environment shader's mean albedo, its alpha map, its glow; the light cards' and lamp surfaces'
emission) and its light rig (RT_FRAME's: the sun, the sky's ambient, the domes, the lamps), in the light's units (below:
`display.prism_rig` `"auto"` is `"units"` with Raster). In Raster the game sends display.prism_rt's scene and rig
whatever `display.prism_rt` says, with nothing traced in the frame (RT_FRAME's mode off): its older modes (`"shadows"`,
`"gi"`, `"pathtraced"`) are off under `display.lighting` `"raster"` (`prism_rt.c`'s `settings_read`, logged once), and
the level traced is the level as drawn whatever `display.prism_rt_world` says. Before, a config with `"gi"` and
`display.prism_rt_world` `"remodel"` gave the bake a scene of no triangles, and Raster drew Classic's light. With the
reach's inputs the game sends two more things only it has:

- **The static scenery** (`prism_bake.c`'s `scenery_copy`): every placement of the scenario's scenery, machines and
  controls that nothing can move or destroy (no script names it, it is made on every difficulty, it has no animations
  and no body to damage: `world_sdf.c`'s rule), in this BSP, its model's most detailed geometry in the world, each
  triangle wound to its vertices' normals, with its shader's albedo (the base map's mean colour) and coverage (an
  alpha-tested model shader's share of its base map's texels drawn: the leaves' cards; a transparent shader's triangles
  left out). b30's first BSP: 592 placements, 90,319 triangles. The bake traces them with the level (their own
  structures in its scene: opaque, mask 4; cutouts, mask 8, a hit kept by 0.75 of its share, as R keeps an object's), so
  the ground under a tree takes its shade and the sun mask its shadow. They have no texels of their own: Raster lights
  them from the volume, as every object.
  A placement whose model a mod's mesh draws (`display.prism_models`, the mesh store: b30's boulders) is sent and baked
  as the others are (the bake's light and its geometry hash stay the stock scenery's, so a bake never depends on the
  remodels), and its run of triangles is named in the reach's last section, which neither the reach's file nor the
  bake's key covers. The scene the frame traces (Raster's light pass, Path Traced's lookup: `scene_runtime`) is the
  bake's but for those runs: their triangles are uploaded after the others of their kind and the frame's two scenery
  structures are built over the others alone. The game sends such an object as a moving one
  (`prism_bake_object_static` answers FALSE), so it is captured from the G-buffer pass as drawn, the mesh's level.
  Before, the stock rock stayed in the traced scene inside and around the rounded mesh drawn in its place, and
  shadowed it in patches of CE's facets that flickered with the frame's jitter and sun samples.
- **The light fixtures** (`fixtures_copy`): the scenario's light fixture placements in this BSP as placed (their colour,
  intensity, falloff and cutoff angles), CE's radiosity's point and spot lights. The bake takes every one that gives
  light, whether its device has power or not (the radiosity took them all), as a point light of inverse-square falloff
  (or a spot), its shadow traced. On the maps measured none lit a gate's pose: c40's thirty and a10's give no light
  (zero intensity or colour), b30's ten are far from G2 and G3. R takes the live fixtures from SCENE_LIGHTS alike.

The map's own triangles are wound clockwise, as D3D draws them: their corners' cross product points into the level.
SCENE_STATIC now turns each of them to its vertices' normals (`prism_scene_static.c`), so that every consumer's face
normal points out of the level; before M2, display.prism_rt's one-sided faces were inside out with the stock world
traced as drawn (the remodel, two-sided, hid it). The remodel's triangles (`display.prism_world_geometry` `"remodel"`)
are wound the same way (b30's first BSP: 99.96% of their area against their vertices' normals) and are turned alike
(`prism_world_geometry_surface_triangles`). Until then the baker took a remodel texel's normal from the cross product,
into the level, and turned it toward whichever side its first rays travelled further on, a guess that open ground over
the void below a level loses: b30's beach baked dark, its sun mask facing away from the sun, and Raster drew it so.
The remodel's faces are still traced two-sided.

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
- **The sun's visibility** apart: a direction of the sun's disc (`display.prism_sun_size` `"auto"`'s, the tag's or the
  real sun's, the wider, whatever the setting: R's at the default).
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
that has not yet seen the tail). At `max` a texel (or probe) stops at 32,768 samples at most (below). The quicker
tiers stop at 64 (`draft`), 512 (`high`) and 2048 (`ultra`) samples whatever the noise.

**`max`'s noise floor (D4 amended, 2026-11-17).** At `max` the error is measured against the larger of the texel's own
mean and a tenth of middle grey (0.018): a texel stops once the standard error of its mean luminance is at most
0.01 × max(mean, 0.1 × middle grey), and the firefly rule likewise (no sample moves the mean by more than 1% of that).
The other rules are M0's (at least 64 samples; black below 1/1000 of middle grey after 4096). `debug.bake_noise_floor`
(0.1) sets the share of middle grey; 0 is the literal rule (`prism_bake_converge.h`'s `error_floor`, which the tests
keep at 0). The quicker tiers keep the literal rule under their sample caps. The log counts the texels the floor
stopped (converged with their light below it).

**`max`'s cap (2026-11-25).** With no budget, `max` never finished: a handful of near-black texels lit by rare paths
never met the rule (a50's first BSP held 5 texels through 13 half-hour slices, one of them at 242,944 samples), and
each slice began them again. A texel or probe now closes at its running mean once it has 32,768 samples
(`MAX_SAMPLES_CAP`, 16 times `ultra`'s); the robust mean and the filter take what noise it has left. 32,768 was chosen
against a 262,144-sample bake of a10's first BSP (2026-11-26): 65,536 came no closer to it (mean difference 1.24 against
1.35 levels, with as many bright texels, which differ from bake to bake), at 1.8 times the time; 16,384 has clearly
more and larger bright blobs. It counts samples,
not seconds, so a bake is the same on every card; it is in the bake's key, and the log counts the texels capped. The
shipped bakes are made at `max` this way.

**The tail.** Once the texels still running are too few to fill the GPU (fewer than 32,768), each one's samples are
spread over 2 to 16 invocations side by side in a subgroup (`lanes_of`): each lane takes whole batches of 64 of its own
strata (the first lane the rest of the batch the texel had begun), and their sums, their samples' statistics and their
batches' are put together (Chan's merge, by subgroup shuffles) before the texel's accumulators are written and the same
rule is asked. The samples and the rule are the ones a single invocation would have taken; the darkest texels, which
the rule keeps longest, take them in a fraction of the frames. `HALO_BAKE_NO_SPREAD` turns it off.

**The noise** (`prism_bake_filter.h`). The tiers under `max` stop on a sample budget, not on their noise: a10's texels at
`ultra` kept a grain between two seeds (26.6% rms per texel, 9.8% light-weighted, 5.8% at the median), the mottling on
c40's chamber walls and a10's, which the auto exposure lifts. Two steps between the samples and the file:

- **The robust mean.** A texel's batches of 64 are dealt to 8 groups in turn (batch b to group b mod 8), and the
  lightmap pass keeps each group's sum of the samples' luminance beside the statistics (28 floats a texel). Where the
  groups disagree badly, the texel's level is the groups' median, a median of means, and its noise the median's: its
  mean over the median by more than three of the median's standard errors (the groups' spread by their median absolute
  deviation), and its largest sample alone a quarter or more of that excess (a firefly, not a skewed but well-sampled
  light). Each group's mean tends to the texel's light as its samples grow, so the median does too: unbiased in the
  limit. The level steers the filter's weights; the texel's light itself is not scaled. Scaling it to the median (the
  first version) took the level of a10's and b30's second BSP down 0.9% in all, but half the light of b30's interior
  walls (G4's poses 1 and 2, their light view's mean 0.43 to 0.57 of before) and a fifth of a10's darkest corners: there
  the light comes by rare paths to small bright lamps, light the texel's neighbours should have had too. Taken as the
  filter's level, such a texel's neighbours read it as their equal and take its light in, so the light is spread, not
  lost (15% of a10's lit texels and 6% of b30's second BSP's are taken to their medians). Not in the draft's
  checkpoints; at `max` alike (a converged texel's groups agree).
- **The filter in chart space**, past the lightmap step, on a thread beside the mask and volume steps: three passes
  of an a-trous filter (3 x 3 taps of 1/4, 1/2, 1/4, their spacing 1, 2, 4 texels) over each virtual page, a texel
  averaged with its neighbours on the same chart (its atlas triangle's: the setup pass keeps it in the record's last
  word) whose faces turn less than 26 degrees from its own (a cosine of 0.9; the weight falls as the square of the
  turn's share of that) and whose level differs from its own by little against the two texels' measured noise: a weight
  exp(-d^2 / (2 x 3^2 x (v1 + v2))), d the levels' difference, v each texel's variance of its level (the 1% rule's: its
  stratified batches' means', or the median's where the robust mean took it), the levels and variances carried through
  the passes. An edge the samples resolve (a lamp's pool, a shadow) stays; a difference the noise explains is averaged.
  The three passes reach 7 texels, under a tile, so each tile is filtered from a window of its 3 x 3 tiles' texels,
  the same result whatever tile a texel is in (no seams; the tests check it). The light, the indirect light and the
  direction's moment are filtered alike, a weighted mean (the level kept: 1.000 to 1.001 of before); the sun's
  visibility, the state and the samples stay as baked, and the empty texels are filled again from the filtered ones.
  `max`, which runs to its noise floor, takes one pass (the mildest). The tile as baked and the filter's words of its
  texels (the normal, the chart, the variance, the robust mean's factor: `ATLX`) are in the log, so a bake resumed runs
  the filter again from them; each filtered tile is written (`ATLF`) and drawn as it comes, and the finished file holds
  the filtered tiles as the tiles' results. It takes 2.4 to 2.6 s of one CPU core for a10's 8.9 M texels, 8 s for b30's
  second BSP's 21.4 M and 58 s for c40's third's 40.7 M, beside the GPU's steps: the bakes' times did not grow (a10 61 s
  against 69, b30 392 s against 423 to 430, c40 1006 s against 1058, 3070 Ti).

Two bakes from two seeds (`HALO_BAKE_SEED` 1 and 2) at `ultra`, texel by texel (each lit texel's luminance, the
difference over the pair's mean; light-weighted: the root of its square weighted by the light; the share of texels
one bake has at twice the other's light; the level: the texels' light summed against the bake before, 3070 Ti):

| BSP, `ultra` | Before: rms, light-weighted, median, at 2x | The filter alone | The filter on the robust levels | The level |
| --- | --- | --- | --- | --- |
| a10's first | 26.6%, 9.8%, 5.8%, 3.7% | 17.4%, 8.2%, 2.6%, 1.5% | 12.2%, 4.7%, 2.6%, 0.52% | 1.001 |
| b30's second | 13.5%, 10.3%, 3.2%, 0.76% | 9.3%, 8.4%, 0.8%, 0.48% | 4.4%, 4.4%, 0.8%, 0.05% | 1.000 |

(The before is `prism-v4-m2` 036f858b1's. The robust mean alone changes no texel's light. The filter alone is the
filter replayed on the bake's log with each texel's level its plain mean. A fourth pass took nothing more; a weight of
5 standard errors in place of 3, 4.5% light-weighted on a10.)

A texel whose first rays meet mostly one-sided faces' backs is inside the level (the stock world's overlapping
geometry); it and a texel no triangle holds take their light from their valid neighbours.

## The layers

The bake is layered (BAKE_VERSION 19, `prism_bake_layers.h`): settings that only scale or tint a light are applied as
the bake loads, with no rebake, as a game applies a setting after compiling its shaders once. Every term of the
estimator is linear in its light's colour x power, channel by channel, so a texel's light (less the sun's direct
light, which Raster and Path Traced take live from the rig and the sun mask) is the sum of five layers, each with
every bounce it starts:

| Layer | Its weight at load (per channel: the live light over the one baked) |
| --- | --- |
| The sun's bounce | the rig's sun (colour x power x the rig's scale) |
| The sky (its radiance where a path leaves the level) | the rig's sky; 0 under `display.prism_sky_light` `"flat"` |
| The domes (the sky's other lights) | the domes' colour x power, summed |
| The fixtures | the rig's fixtures' scale (0 in the fitted rig) |
| The lamps and glows (light cards, lamp surfaces) | the lamps' materials' largest emission x `display.prism_rt_emissive_scale` |

The lightmap pass and the volume pass keep the layers' means beside the light (`path_layers`, the lightmap's
accumulators 28 to 39, the volume's 14 to 26). Each finished tile keeps, beside its words as before (`ATLS`), its
texels' **shares** (`ATLY`: per channel, the sun's bounce, the sky, the domes and the fixtures as bytes of 255, the
lamps the rest; a flag for an indoor cluster's texel): 13 bytes a texel; each brick its probes' shares of their SH L0
(`VOLY`). At load, and again whenever the weights change:

- a texel's light is its light as filtered times the sum of the weights by its shares, plus Tool's floor under
  `"flat"` (the outdoor one, or sky 0's indoor one by the texel's flag; not in the indirect);
- its indirect light (Path Traced's pool) is scaled by the sun's bounce's and the sky's weights (wholly indirect) and
  the rest's (the domes', fixtures' and lamps' bounces, by their weights over their shares);
- a probe's four SH coefficients by its ratio a channel, the flat floor added to its L0;
- the reflection probes by the BSP's ratio a channel (the volume's light-weighted mean).

At the bake's own light every weight is 1 and the words are copied: a layered bake drawn at the settings it was made
with is the bake as it was. The direction (`k`, `a0`) stays the bake's. When a weight changes while a level is drawn
(the Sky Light row, the lamps' brightness, a rig's scale), the log says so ("the light's weights now ...: what is
drawn recombined, no rebake") and the host recombines every drawn tile (2048 a frame), brick and reflection probe
from the file, on the CPU, with no GPU work. The bake itself is always made with the sky a light (rig[61] 2) and the
fixtures at a scale of 1; its base (the light it was made with, `LYRB`) is in the file. A log begun with other
colours (a changed `debug.prism_rig_scales`) is begun again rather than resumed.

The key keeps only what changes the light's transport: the baker's version, the quality, the level's triangles,
tiers and static scenery, the materials' albedo and alpha maps and their emission as shares of the largest, the
cutouts, the sun's direction and disc, the domes' directions, cones and test distances, the sun's test distance,
which kinds of light there are, the units or the fitted rig (their test distances differ), the lamps' and fixtures'
places. The colours and powers, `display.prism_sky_light`, the rig's scales and the lamps' brightness are not in it.

**A distributable bake.** The file holds lighting only: the texels' light, direction, indirect light, sun visibility
and shares, the probes' SH and the reflection cubes (radiance at the mean albedo), the masks, the job ids, the lamp
grid's lamp indices; never a triangle, an albedo map or the remodel. `LYRB` says whether the bake was of the stock
world: a pack for a release is the files of a stock bake, keyed by the per-BSP hashes of its key, which the player's
machine checks against its own maps before it uses the light (the plan is rebuilt from the player's map).

## The shipped bake

Players never bake for hours and never bake again when they change a setting (Adam, 2026-11-25). As games do (Unreal's
Lightmass and volumetric lightmaps, Unity's lighting scenarios, Frostbite, Call of Duty's UberBake: the light baked
once, offline, at final quality, shipped, and recombined at runtime by its layers), each release ships every campaign
level's light baked at MAX for both worlds (the stock and the remodel), and the settings a player has change only the
runtime light. The plan and its sources: `work/openh1_eval/v4_ship_bake_runs/PLAN.md`.

**What ships** (`display.bake_shipped_folder`, the data folder's `prism_bake_shipped` by default):
`<stock|remodel>/<map>/bsp<n>.pbk`, each a finished bake file as the baker writes it, and `manifest.txt` (the game's:
`bsp <world> <map> <index> <of> <key>`, the key as the main menu's note keeps it, its geometry by kind,
`geo:stock` or the enhanced world's `geo:prsx:<world_id>`, with the BSP's tag where the export knew it, and each
level's `world <map> <base_id> <classic_world_id>`), the same lines a level in `<world>/<map>/manifest.txt` (what the
ladder and the Lightmap Downloads rows read), beside `manifest.json` (each file's size and sha256, each BSP's per-BSP
input hashes and world_id). Lighting only: the plan's job ids, the texels' light, direction, indirect
light, sun visibility and layers' shares, the sun mask, the volume's probes and their shares, the reflection probes,
the probes' placement (the reach's bricks), the lamp grid's lamp indices, the footprints and the layers' base. No
triangle, albedo, texture or remodel geometry: the player's machine rebuilds the plan from its own maps (and builds
its own remodel) and checks the key before it uses the light.

**The tool** (`tools/prism_bake_ship.py`): `export <bake folder> <stock|remodel> <shipped folder>` copies each finished
BSP bake after checking it is whole (DONE), of the quality asked (`--quality`, max for a release) or better, of this
file version and lighting only (a record of a kind not known to be light refuses the file), of the world named (its
`LYRB` record's: a stock bake is never exported as the remodel's, nor the reverse, and a file with no `LYRB` is
refused), and writes both manifests. A level ships whole or not at all, since players never bake: its BSP count is the
map's (`--maps-folder`), the worker's note's (`baked.txt`'s `bsps N`) or `--bsps map:N`, which must agree, and a level
with any BSP's file missing is refused, nothing of the export written. A BSP with nothing to bake (no lightmapped
surface: d40's BSP 9) is named `--empty map:index` and listed in the manifests with no file. The rig is always the units
rig (no `--rig`: a note that says a BSP was baked with the fitted rig is refused).
`--archives` writes a release asset a level and world (`prism_bake_<map>_<world>_v<baker>.ppk`, an uncompressed tar:
the records are packed already), each under GitHub's 2 GiB asset limit (a level past it splits by BSP, and a BSP's
file past it into parts: byte ranges of the file, `..._bsp<n>_part<i>of<k>.ppk`, each part's sha256 and the whole
file's in every part's manifest). `install` unpacks a release's archives into a shipped folder (nothing outside it): a
split BSP's parts wait beside it (`bsp<n>.pbk.part<i>of<k>`, `bsp<n>.pbk.parts.json`) until all are in, from one install
or several, then are joined and checked against the manifest's size and sha256 before the BSP is listed. `verify`
checks a shipped folder (sizes, sha256, every file a whole lighting-only bake of its manifest's key; a BSP whose parts
are not all in).
`--reach` carries each BSP's reach (the farm's walk, `<map>/bsp<n>.reach` beside its bake) in its bake as a `REAC`
record where the bake has none (a bake committed by this baker writes its own), always as the bake's first record
(sequence 0, every other record after it renumbered, payloads unchanged), and `--require-reach` refuses a BSP without
one: a player's machine then walks no reach and finds the walk from the header and one record (below). `plancheck <shipped folder> <plans folder>` checks every
shipped BSP against a player's plan of it (`<plans>/<map>/plan_bsp<n>.txt`, `HALO_BAKE_PLAN_ONLY`'s): the whole key,
field by field, the tiles, and the `REAC` record's walk against the player's own (its inputs' hash, the bricks' and
tiers' counts and CRC-32s). A BSP with no plan or any difference fails it: the farm's packaging runs it before anything
is released, so that no pack a player's game would refuse, or would plan otherwise, ships.

**A player's plan without a bake** (`HALO_BAKE_PLAN_ONLY=<file>`, `{bsp}` in the name for a file a BSP,
`HALO_BAKE_PLAN_ONLY_BSPS` BSPs before the game quits, 1 by default): once the BSP is planned, its key (every field, at
the quality asked: `HALO_BAKE_QUALITY=max` for a release's), the plan's counts, the key's BSP hash by part (the
triangles' places and each attribute field) and the walk the game made itself are written, and the game quits. No bake
is chosen, loaded or made and the reach is always walked here. In play (a player's path; `debug.bsp_switch_test` for the
other BSPs) or by the main menu's worker (`debug.bake_worker`, `debug.bake_bsp`), with a fresh config: both now give the
same key. (They did not before prism-v4-plan: the map's first frame's stand-ins, `render_enumerate.c`, left a lightmap
of the structure without its picture when no material stood in, and SCENE_STATIC's set, the key and the plan lost that
lightmap's page wherever an enumeration had left it so; the farm's worker and a player's game enumerate differently.)

**The baker** (`xgpu_vulkan_bake.c`'s `file_open`): the world drawn's shipped file first (the game sends the folder and
the world: `XGPU_BAKE_FLAG_REMODEL`), read only. Where its key is this BSP's it is loaded and drawn and nothing is
baked ("the shipped bake (...) is this BSP's: loaded, nothing baked"); the status block's `XGPU_BAKE_STATUS_SOURCE` says
so. Where it is not (a mod, a custom map, a remodel built otherwise), the log names the key's fields that differ; this BSP
is baked here only on the farm (the bake lock, "The frozen bake" below: a player's game draws its light without the
bake). Either way the baker notes it beside the local bake (`<folder>/<map>/
shipped.txt`: `bsp<n> <world> used <quality>`, or `mismatch` and the fields), for the main menu. **A better bake is good
for a lesser quality**: a whole bake of the same inputs whose quality is better than the one asked is taken (the key's
settings made again at the file's quality, the `max` tier's noise floor among them: `key_settings_hash`), so a shipped
MAX bake serves every quality, and `display.bake_quality` (config.toml's) only sets what a bake made here aims at.

**What no longer bakes again:**

| Setting | Before | Now |
| --- | --- | --- |
| Bake Quality | every level baked again | no menu row; a release ships MAX, which every quality takes |
| Sky Light | (the layered bake) | recombined from the layers at once |
| Cutout Shadows | every level baked again | the level's cutouts are always in the bake under both PRISM lightings (`prism_rt.c`'s `level_alpha_traced`); the setting chooses the moving objects' cutouts |
| World Geometry | the BSPs it changes baked again | both worlds ship: an instant switch; asks REBAKE only where the shipped bake lacks that world |
| The rig's class (`display.prism_rig`) | baked again | config.toml's alone (the units rig is every player's) |
| Exposure, tone curve, Path Traced's quality, dynamic lights, shadows, denoiser, upscaler | never read by the baker | runtime alone: the performance settings |

**The main menu** reads `manifest.txt` for the world drawn: a level whose every BSP it has (and that the baker has not
found to differ) is SHIPPED, ready, with no PLAY NOW question. Once every level that needs a bake has one, the worker
checks each shipped level not yet seen used against this machine's maps (CHECKING; "Checking PRISM light" under the
menu, playable meanwhile): it loads the level, the baker takes each BSP's shipped bake at once and bakes any BSP that
does not match, that BSP alone. A check is made once a start at most; Rebake Local (the old Rebake All) deletes the
bakes made here and the notes, keeps the shipped light and checks it again.

Not yet (the plan's other steps): the farm's MAX bake of every level and both worlds, the remodel's bit-identical
build (its geometry hash checked in the manifest), the BC6H/BC4/BC7 release encoding (about 4.5 bytes a texel against
about 18), the baked sun masks and rebaked pages of Classic's PRISM shipped alongside, and the installer's download.

## A BSP switch under Enhanced (prism-v4-drops)

Adam's play test of the 58449a4a9 kit (Enhanced lighting): "occasionally the entire lighting jumps back to classic",
and "light has to reload when the game loads new level areas". What made Enhanced draw CE's light, and what does now:

- **A whole bake's card memory.** Every bake state made the baking's buffers, a shipped bake too, though a player's
  game only loads and draws it: on b30's enhanced first BSP about 1.6 GB (Path Traced's indirect pool, unread since
  Path Traced takes no baked light; masks for a quarter of the tiles where its file has 6,440; the atlas's triangles,
  the tiles' lists, the final gather's tables, the batch's buffers, the volume's probe places and batch). On a 3070
  Ti with the enhanced geometry its volume found no room, the bake failed, and the game dropped the map to its
  classic geometry (Adam's log: "no memory for the irradiance volume", then rung 2). `b->bakes` (the farm's run with
  no whole bake chosen) now decides what is made: a whole bake makes the frame's tables, the pool, the masks its file
  has, the volume's probes and the reflection probes, and its steps after the load bake nothing its file lacks.
- **The verdict on a failed bake.** A whole bake that failed after its tiles were drawn still lights the geometry:
  only a failure before any of its light was drawn (the status's `SHOWN` none) drops the map to rung 2.
- **MENDICANT AI's maneuver slots in the reach's seeds.** The AI gives each encounter of the loaded part 48 slots
  after its firing positions while the game ticks; the reach took them for the level's, so a set begun again in play
  (a lighting change) sent other inputs than the shipped reach's (b30: 5415 seeds against the farm's 4983), the
  reach was walked again (16 s), its bricks and the key's settings moved, and the shipped bake was refused (0xa0):
  CE's light for the rest of the map. The seeds are the level's own firing positions now
  (`mendicant_encounter_own_firing_position_count`).
- **The light let go at every switch.** A BSP's bake was freed when the game drew another, and the next one made from
  its set: until its set, reach and plan were in and its file opened, CE's frame; then its tiles read and decoded on
  the render thread, 2,048 a frame, the volume after every tile, so that its objects and every texel not yet loaded
  were lit by CE or the open sky. Now:
  - **kept bakes** (`bake_keep`, `kept_take`): a whole bake loaded in full is kept on the card when the game draws
    another BSP of the map (the kept bakes at most a third of the card's budget, a quarter of it free; the longest
    kept let go of first, and before a new bake's upload where the card lacks room). When the game draws its BSP
    again (a load zone back, a checkpoint loaded, a cinematic's BSP and back) it is drawn at once, and once the BSP's
    set is in, its key is checked against the set's (`bake_begin` compares the bake drawn before it opens any file):
    kept where it is the set's, else let go of for the set's own;
  - **the loader** (`load_main`): a whole bake's file is read and decoded on a thread of its own, the volume's bricks
    first, then the tiles; the frame draws 16 MB of them at a time (`LOAD_FRAME_BYTES`);
  - **opened ahead** (`prefetch_main`): as a map is drawn its other BSPs' shipped bakes of the world drawn are opened
    on a thread (the log read, the records' index made) and read once for the disk's cache, the nearest BSPs first;
    `file_whole` takes the opened file.

A BSP drawn for the first time still waits for its set, its reach and its plan before any of its light can be drawn:
the plan is made from the player's own map (the shipped pack holds no geometry), so it cannot be made before the
game draws the BSP. `HALO_RASTER_FRAME_LOG` logs what lit each frame where it changes (the bake whole or in part, or
CE's frame and why), with the window's G-buffer; `HALO_BAKE_KEEP_NONE`, `HALO_BAKE_LOAD_INLINE` and
`HALO_BAKE_PREFETCH_NONE` turn the three off (diagnostics).

## The frozen bake: BAKE_VERSION 20 (v4 geopatch M4)

`XGPU_BAKE_VERSION` 20 is the first frozen baker: the farm bakes every level for both worlds with it (geopatch M6), and
a later change that moves it re-farms every pack of both worlds (a planned, batched content release, decision A13).
What M4 put into it before the freeze (`work/openh1_eval/v4_geopatch_runs/PLAN_FINAL.md`, M4):

- **Keyed to the world_id.** Every bake's key (its header, `prism_bake_cache.h`) holds the BSP's `world_id`, the
  engine's `SCENE_VERSION` (`XGPU_BAKE_SCENE_VERSION`, `PRISM_SCENE_VERSION`: one number) and the variant (0 classic,
  1 enhanced). The enhanced world_id comes from the sidecar the game opened (`docs/prism_geometry.md`); the classic one
  from the map's `classic_world_id`, the farm's hash of a map version's bake inputs (`tools/geopatch/classic_world.py`:
  its structure BSPs, and the tag data of its scenario, shaders, bitmaps (and their pixels), static scenery, devices,
  light fixtures, models, skies and lights), which the game looks up by the player's map's `base_id` in the shipped
  manifests' `world <map> <base_id> <classic_world_id>` lines (M7's signed manifest replaces them) and the farm passes
  as `HALO_CLASSIC_WORLD_ID`. With a world_id the key's map checksum is 0, so two map versions of equal bake inputs
  share one pack. Without one (a custom map) the key is the map's checksum, as before.
- **Foliage-ready layers** (decision A5, PLAN_FINAL section 4.4): beside the bake's own light, the light with the stock
  foliage passed, for a runtime that draws its own leaves. The foliage is the static scenery's cutouts (alpha-tested
  model shaders, the leaves' cards: mask 8 in the bake's structure); the level's own alpha-tested surfaces (fences,
  grates) are not foliage and block in both. Three records: `MSKF`, a tile's sun mask with the cutouts passed (every
  tile some of whose texels face the sun and are not lit whole: removing occluders only lights a texel more, so a tile
  lit whole needs none), `VOLF`, a brick's probes' sun visibility with them passed (a byte a probe, traced along the
  same direction as the bake's own, `sun_visible_layers`), and `FCLS`, the foliage classes (a bit a static scenery
  triangle: which of them the layers leave out; classes, no geometry). The sky's and the bounces' light under the
  canopy keep the stock foliage (the runtime's canopy term handles them, as the plan says); only the sun's visibility
  is layered. Nothing reads the layers yet: no foliage ships before the FOLIAGE rows.
- **The bake lock** (PLAN_FINAL section 3.5): a bake is made or resumed only where `HALO_BAKE_FARM=1` is set (the
  farm's runs); everywhere else the baker only loads whole bakes, and writes no `.pbk`, `.pbk.new` or `.reach` (and makes
  no bake folder). A build made with `PRISM_BAKER_COMPILED_OUT` never bakes (M7's release builds, once the farm half
  is split off). `display.bake` and `HALO_BAKE=auto` alone no longer start a bake.
- **The enhanced world's own lightmap layout** (LMUV v2, decision A4): the enhanced bake's atlas is laid out on the
  sidecar's own pages (`docs/prism_geometry.md`), which follow the BSP's own lightmap pages in SCENE_STATIC's page list
  and in the G-buffer's page. The baker's code is the same for both worlds: its atlas takes any page's size and any
  chart's coordinates.

**What forces a re-farm, and what does not:**

| Change | Re-farms |
| --- | --- |
| The baker's estimator, its records or their layout, the layers, the rig's units, the atlas's densities, the quality's rules (anything that moves `XGPU_BAKE_VERSION`) | every pack of both worlds |
| The engine's derivation of the bake's scene from the map (static scenery rules, alpha coverage, triangle order and filtering: `SCENE_VERSION`, in every world_id) | every pack of the world it changes |
| A level's enhanced geometry (a BAKE_RELEVANT sidecar section: GEOM, LMUV, SCEN) | that BSP's enhanced pack |
| A map version with other bake inputs (a new `classic_world_id`) | that version's packs |
| `RENDER_FORMAT` (GEOM's meaning to the renderer) | every enhanced pack |
| Collision (COLL, ZSOL, COLM, PATO), runtime sections (RHGT, DOBJ), the PRSX format number, the sidecar's file bytes, the pack's encoding (PACK_FORMAT: re-exported from the archived masters) | nothing |
| Any runtime setting (exposure, tone curve, sky light kind, lamp brightness: the layers' weights) | nothing |

**The pilot** (b30, both worlds, MAX, BAKE_VERSION 20, the farm's RTX 3090s, each card shared with one to three other
runs; `work/openh1_eval/v4_geopatch_runs/m4`):

| BSP | World | Tiles (M texels) | Bake (s) | Pack |
| --- | --- | --- | --- | --- |
| 0 | classic | 188,934 (48.4) | 4,186 | 736 MB |
| 1 | classic | 83,429 (21.4) | 3,759 | 201 MB |
| 0 | enhanced | 646,021 (165.4) | 4,828 | 1,168 MB |
| 1 | enhanced | 191,177 (48.9) | 3,356 | 236 MB |

The level's classic bake took 2.2 card-hours and its enhanced one 2.3 (shared cards, so these are not timings of a card
alone): the enhanced world's own layout has 3.4 times the classic texels on the beach BSP and 2.3 times in the
interior, baked at fewer samples a texel (b30's first BSP: 1,186 against 3,426). The masks step, both layers' masks
together, finished within one 10 s progress line of each bake's log. The foliage-ready records (MSKF, VOLF, FCLS) are
0.29 to 0.77% of a BSP's file. The canopy check (`v4_geopatch_runs/m4/scripts/foliage_check.py` on the release
archives' files; b30's first BSP, whose static scenery has 46,099 cutout triangles of 90,319): the stock foliage alone
shades 3.0% (classic) and 2.1% (enhanced) of the foliage layer's mask points, and 2.2% and 1.9% of the valid probes;
the interior BSP, with no cutouts, differs by the masks' noise alone (0.01% of the points, no probe). The player's run
loaded both worlds' packs from their release archives (`prism_bake_ship.py install`), keyed by the map's
classic_world_id and the sidecar's world_ids, and wrote no bake file. A shipped pack then streamed in at 2,048 tiles a
frame: on a 3090 the whole bake (tiles, then the volume) took 25 s from its start for the enhanced first BSP (646,021
tiles), 11 s for the classic one, 7 and 3 s for the interior's, and Raster lit a texel not yet loaded from the volume
or the sky (`docs/prism_geometry.md`); it is loaded off the render thread now, its volume first ("A BSP switch under
Enhanced").

**Bakes are not bit-identical run to run** (the GPU's float order and the converge rule's batch timing:
`work/openh1_eval/v4_ship_bake_runs/cap/numbers.txt`), and need not be: a pack's key is its world_id (the geometry and
inputs it is for), never its bytes; the release's manifest names each produced file by its sha256 (download and
install integrity only), and the frozen packs are archived by content hash and never baked again for the same release
(PLAN_FINAL section 2.1: the bake masters are archived because GPU bakes are not reproducible). A re-bake of the same
world_id is a different file of the same light, within the bake's noise.

## What is baked

Each a list of jobs (`prism_bake_cache.h`'s ids), in this order:

| Step | Jobs | Representation |
| --- | --- | --- |
| The plan | On a thread: the atlas (`prism_bake_atlas.h`: CE's pages scaled chart by chart by the quality's density at each tier: `max` 16 texels a world unit at tier 0, 8 at tier 1, 0.5 at tier 2), its tiles of 16 × 16 texels, each tile's triangles, the chart maps (each CE texel's virtual page), the grid's cells and candidates, the probes | — |
| The importance grid | One job: every cell's candidates weighed (4 points of each lamp from each of the cell's 8 points, shadows traced) | A hashed table of cells, 64 lamps and cumulative weights each |
| The lightmap | A tile each: a batch of 4096 tiles on the GPU at a time, each texel's samples added between frames until it stops (the running texels packed into whole groups each frame, so that a batch whose texels are mostly done still fills the GPU); a finished tile written, drawn and replaced in the batch by the next | Per texel: the light less the sun's (RGB9E5), the direction (`k`, `a0`: RGBA8), the indirect light alone (RGB9E5, for M3's path tracer), the sun's visibility, the state and samples |
| The sun mask | A tile each where the tile's texels facing the sun are neither all lit nor all dark: its 64 × 64 points (4 times the texels' density) found on its triangles again, each traced to the sun's disc (16 directions, stratified, up to 256 while they disagree) | A byte a point; a uniform tile is a state alone (dark, lit) |
| The irradiance volume | Two cascades: 64 probes a brick (a world unit, 0.25 apart) where the player stands (the walk's floors and the two world units of air above them), and 64 a coarse brick (8 world units, 2 apart) over every reachable brick and the 12 world units of air above the floors (tree tops, the far side of a canyon: every object Raster draws has light); a batch of 262144 probes at a time (its count of running probes read back each frame until it is 0, its results then asked for once: while the game draws, two or three of those counts are in flight, `prism_bake_batch.c`) | Per probe: SH L1 of the light from every direction (RGB, half floats), the sun's visibility, whether it is inside the level. The fine cascade holds 196,608 bricks a BSP at most (12.6 M probes, 403 MB on the GPU): past that, the bricks with the most solid neighbours beside and above them (walls, rocks, slopes, overhangs) are kept, then floors before air, and the rest are left to the coarse cascade, which the game reads where the fine one has no brick. The budget is in the bake's key at every quality. Only d40's eighth BSP (open terrain: the reach walks the valley, 397 M fine probes) is over it |
| The reflection probes | One each 8-world-unit cell of the walk's floors, a world unit above them (b30's first BSP: 923; a30's: 2,185; a10's: 25); a batch of 8 at a time | A box (how far each of its six axes reaches, 1 to 64 world units) and a cube of four levels (32, 16, 8 and 4 texels a side), each level GGX-prefiltered at its roughness ((level / 3)^2: each texel the light the bake's estimator sees along the lobe's directions about it, weighed by their cosines; 16, 64 or 256 samples a texel by the quality), RGB9E5 |
| The footprints | One job: each switchable fixture's tiles within its reach (section 2.4's relight) | The light's id, its tiles |

## The bake file

`<folder>/<map>/bsp<n>.pbk` (`prism_bake_cache.h`, version 3): a log of records, each with its job's id and checksum, appended as
jobs finish. While the bake runs it is `bsp<n>.pbk.new`, and a quit, a crash or a GPU reset resumes it at the next
unfinished job (the tiles of the batch then on the GPU are begun again); once every job is done, `DONE` is appended
and the file renamed over `bsp<n>.pbk`. Its key (the header) holds what the bake was made from: the map's checksum and
name, the BSP, a hash of the level's triangles and attributes as the bake read them, the surfaces' tiers, the materials
and alpha maps, the rig (with the port's words past its settings: the indoor floor, the fixtures' scale, the units and
the sky's kind, the albedo's rule, the sun's test distance) and the lamps, the reach's bricks, the baker's version, the
quality and the threshold. While the bake runs a tile's result is as baked, with the filter's words of its texels
(`ATLX`) after it and its filtered texels (`ATLF`) once the filter has run; the finished file keeps the filtered texels
as the tile's result and drops both. A file of
another key is not resumed: the BSP is baked again (into `.new`) and replaces it when done (with `display.bake` `"off"`,
nothing is made or baked: a whole bake of the key is drawn, else an unfinished log of it as
far as it goes).

The light is the map's, not the path tracer's look: the bake takes the sun's disc of `display.prism_sun_size` `"auto"`,
the lamps of `display.prism_rt_emissive` `"lights"` (the light cards and lamp surfaces; no glows, as Tool's radiosity),
at `display.prism_rt_emissive_scale` 1, and the albedo of `display.prism_rt_materials` `"average"`, whatever those
settings (and `display.prism_rt_style`, which picks their defaults) are, so a bake loads in every session of the same
light settings: `display.prism_sky_light`, `display.prism_rig`, the quality. Those values are the settings' defaults
with the faithful style: a bake made before at the defaults keeps its key.

**Packed records** (version 3, `prism_bake_pack.h`): the finished file's large records (the tiles, their layers,
the bricks, their layers, the reflection probes, the masks) are packed: a byte shuffle by the payload's word size, a
byte delta per plane, then an LZ77 in LZ4's block format, no library needed. A record's header says its unpacked size
and reading unpacks it, so every reader sees the payload as written; the log while baking stays plain.

**The reach's file.** The walk that finds where the player can be (the reach: the bricks the probes fill, the
surfaces' tiers) is kept beside the BSP's bake (`<map>/bsp<n>.reach`, keyed by a hash of its inputs), so a BSP drawn
again takes it at once: b30's first BSP's walk takes 16 s on the CPU, during which Raster drew CE's light. Its BSP
change (b30's beach into the structure and back, 3070 Ti) now holds the frame 0.9 and 1.9 s, Raster's light whole
again 1.5 to 3 s after it (the tiles loaded 2048 a frame), where before the frame stopped 20 s going in (the path
tracer's lamp grid, made on the frame's thread for the structure's 1871 lamps although nothing was traced: now made at
the first traced window) and the beach stayed CE's light 16 s coming out.
Only the farm writes the file; a player's machine takes the shipped bake's `REAC` record instead (the same bytes:
`PRCH`, its version, the inputs' hash, the flight flag, the summary, the bricks, the tiers; read whole, refused if cut
short or grown) where its inputs' hash is the BSP's own, so a BSP's shipped light is used as soon as its bake loads
(d40's eighth BSP's walk is 696 s on an RTX 2060 desktop's CPU, 1206 s on the farm's: its light now in use 5 to 6 s
after the BSP's load, a30's first BSP's 8 s against 52). Where the shipped bake is refused all the same, the reach is
walked here and the key made again from it.
A walk still running when another BSP's inputs begin (or the same BSP's new scene's) is stopped, not waited for: the
render thread joins the walk's thread there, and waited out its whole walk before, so with no shipped bake d40's
switch from its ninth BSP (nothing to bake) back to its first stopped the game for the rest of the eighth BSP's
696 s walk, in Enhanced and Path Traced alike (`prism bake: bsp <n>'s reach stopped after <s> s: its inputs were
replaced`; a BSP entered again walks again).

## The settings

| Setting | Values | |
| --- | --- | --- |
| `display.lighting` | `"ce"`, `"ce_deferred"`, `"raster"` | Raster is the third |
| `display.bake` | `"auto"`, `"off"` | Bake on the main menu and while playing (default), never (a whole bake is still drawn); the older `"ask"` is read as `"auto"` |
| `display.bake_quality` | `"max"`, `"ultra"`, `"high"`, `"draft"` | The quality of a bake made on this machine (config.toml's and the console's; no menu row): `ultra` the default (minutes a level on the main menu); D4's `max` (hours a level) where asked for. A bake of a better quality is good for a lesser one (a release's shipped MAX bake for every quality) |
| `display.bake_rebake` | an integer | Each change bakes the BSP drawn again from nothing (the console's; Settings' Rebake Local deletes every level's bake made here, the shipped light kept) |
| `display.bake_folder` | a path | Empty: the data folder's `prism_bake` |
| `display.bake_shipped_folder` | a path | A release's shipped bake, read only ("The shipped bake"); empty: the data folder's `prism_bake_shipped` |
| `display.bake_budget` | 0.25 to 200 ms | The GPU time a frame may give a background bake (30 in the menus, 60 with `debug.bake_exit`, 40 in the main menu's worker) |
| `display.bake_indicator` | `true`, `false` | The progress at the screen's top left |
| `debug.bake_exit` | `true`, `false` | Bake in the foreground and quit once done: the measurements' runs |
| `debug.bake_worker` | a map's name | The main menu's worker (below): that map loaded at once, each BSP baked in turn, then quit |
| `debug.gbuffer_view` | `"bake_light"`, `"bake_sun"`, `"bake_texels"`, `"bake_volume"`, `"bake_total"` | Raster's views |
| `display.prism_raster_composition` | `"ce"`, `"materials"` | CE's shaders compose Raster's light (default), or PRISM's own materials |
| `display.prism_raster_objects` | `"volume"`, `"classic"` | Objects by the volume's light (default), or never lit under Classic |
| `display.prism_raster_object_shadows` | `"traced"`, `"classic"` | The moving objects' sun shadows traced (default), or CE's projected ones |
| `display.prism_sky_light` | `"sky"`, `"flat"` | The sky a light, by what each point sees of it (default), or Tool's flat fill; recombined from the layers, no rebake |
| `display.prism_exposure`, `_range`, `_speed`, `_bias`, `display.prism_tone_curve` | as the path tracer's | Raster's exposure and tone curve (its style's: `"auto"` and `"filmic"`) |

The bake's diagnostics (environment variables): `HALO_BAKE_TERMS` leaves terms out (1 the domes, 2 the lamps, 4 the
glows, 8 the paths past the first hit, 16 the fixtures, 32 the grid, 64 the lamps' spherical sampling; a texel's 128
indirect light, 256 own lamps' direct light; 512 every lamp of the cell's lists in the draft too, 1024 the resampled
pick past it), `HALO_BAKE_SEED` another seed (two bakes' noise against each other), `HALO_BAKE_GATHER` the final
gather, `HALO_BAKE_EMITTERS_PASS` the level's emitting faces neither blocking nor reflecting, `HALO_PRISM_ALBEDO`
`stored` the base maps' stored mean; `HALO_BAKE_NO_ROBUST` and `HALO_BAKE_NO_FILTER` leave the noise's steps out,
`HALO_BAKE_FILTER_PASSES` sets the filter's passes (0 to 4), `HALO_BAKE_KEEP_LOG` keeps the finished bake's log beside
it (`bsp<n>.pbk.log`: the tiles as baked and the filter's words); `HALO_RASTER_EXPOSURE_LOG` logs Raster's exposure and its meter's medians each
frame.

Settings > Video > PRISM > Lighting chooses Classic, PRISM Rebaked (`"raster"`) or PRISM Path Traced. PRISM's page
then opens, under PRISM Rebaked, Rebaked Lighting › (Composition, Object Light, Object Shadows, and Shadow Quality while
the object shadows are Classic's, Dynamic Lights); under both PRISM modes, Image › (Exposure, Exposure Bias and Tone
Curve), Upscaling › and Light Bake ›: Sky Light, each campaign level's bake and Rebake Local (Path Traced
reads the bake too; "The bake on the main menu", below). Ray Tracing has the traced scene's rows
(docs/prism_settings.md). The bake
needs ray queries; a GPU without them draws Raster with CE's
light (D5).

## The bake on the main menu

The player never asks for a bake (`port/linux/game/prism_bake_menu.c`). With either PRISM lighting chosen, a card that
traces rays and `display.bake` `"auto"`, the main menu bakes the campaign's levels whose maps there are and that lack
a bake of the settings chosen, before they are played, as other games compile their shaders.

- **A worker.** The baker bakes the level drawn, and the main menu draws none, so a level is baked by the game started
  again as a process of its own (`update.h`'s `update_child_start`: fork and exec on Linux, `CreateProcessW` in a job
  on Windows; ended with the game by Linux's parent death signal or the job), with `debug.bake_worker` its map. It is
  hidden and silent: no sound, controllers, network, updates, crash reports, movies or menu pictures; its saves, log
  (`debug.txt`) and pipeline cache its own, under the saves folder's `bake_worker`; it reads the same config.toml, so
  its bake is of the same settings. It loads the map at once in place of `init.txt` (`console.c`), stops the game's
  clock (`game_speed 0`), bakes each BSP in turn in the foreground at 40 ms of GPU time a frame (`scenario_switch_
  structure_bsp` between frames, once the BSP before is `DONE`), writes its progress to the bake folder's
  `menu_worker.txt` every half second (its BSP, of how many, the share and the seconds left, the baker's estimate),
  and quits. A BSP that fails is passed over and the rest are baked; the worker then quits with 3. A BSP with no lightmapped
  surface (d40's tenth, a cinematic's) has nothing to bake: the scene says so (`prism_scene_static_empty`), the note
  counts it baked and the worker goes on. With
  `debug.bake_bsp` it bakes that BSP alone and quits (a farm's job a BSP). One worker at a time; the main menu's frames are held to 30 a second while it bakes, so the GPU is the
  bake's, and the bake goes on with the window out of focus.
- **The order.** The level WAIT chose (below), the one played last (the bake folder's `last_played.txt`), the saved
  game's, the one after the last finished (player 1's profile), then the campaign's.
- **What is baked** is noted beside each map's bake, `<folder>/<map>/baked.txt`: the map's BSPs, and each BSP baked
  with its key (`prism_bake_key`: the baker's version, `XGPU_BAKE_VERSION`, `display.bake_quality`, the rig's class
  (`units`, which `"auto"` is, or `fitted`), `cutouts` (always: the level's cutouts are in every bake); a noted quality
  better than the one asked is good) and its own geometry (`geo:stock:<tag>`, or
  `geo:pwg:<tag>:<size>:<time>` where the world is the remodel and the BSP has a `.pwg`), written by whichever process
  saw that BSP's bake `DONE` (the worker, or a game played). A level is baked when all its BSPs carry the settings'
  key and their geometry is still what was noted: switching the world's geometry, or one BSP's remodel changing,
  bakes again only the BSPs it changes. The
  baker checks the whole key as it loads a bake (the map, its geometry, its lights), so a bake the note calls good that
  is not (a map or a remodel changed) is baked again as the level is played.
- **Resumed.** The bake file is a log the baker resumes at its next unfinished job, so a worker ended (the game quit
  or killed, a level started) loses only the work in flight; the next start goes on where it stopped. A worker that
  fails, or writes no progress for four minutes, leaves its level until the next start (FAILED on Light Bake).
- **On the main menu,** under the menu: "Baking PRISM light: The Silent Cartographer 37%", a bar, then "About 4 min
  left - Level 1 of 10 - best played once it is done"; "PRISM light baked: every level is ready" for a few seconds once
  done.
- **A level started before its bake is done** (Campaign's Choose Mission and Choose Difficulty, Continue, Load Game:
  `menu_functions.c`'s `campaign_start`) asks first (`screens/bake_play.xml`): PLAY NOW plays it with Classic's light
  where its bake has nothing yet, the game's own baker going on with it in the background as it is played (the worker
  is ended as the level starts); WAIT (and B) goes back, the main menu baking that level next.
- **A setting the bake holds changed** (World Geometry where the shipped bake lacks the world chosen, the rig's class:
  config.toml's) under a PRISM lighting asks once it is written (`screens/bake_rebake.xml`): REBAKE (and B) keeps it,
  and the main menu bakes the levels again; UNDO writes the settings back. Bake Quality, Cutout Shadows and Sky Light
  never ask ("The shipped bake").
- **Light Bake** has Sky Light, each campaign level's bake (SHIPPED, CHECKING, BAKED, BAKING 37%, LOADING, PART BAKED,
  NOT BAKED, NO MAP, FAILED) and REBAKE LOCAL, which asks (`screens/bake_rebake_all.xml`), then deletes every level's
  bake and note for the main menu to bake them again from nothing. Its old Bake (While Playing, When Asked, Never),
  Rebake Lightmaps and Progress on Screen rows are gone: `display.bake` `"ask"` is read as `"auto"` and written so;
  `"off"`, `display.bake_rebake` and `display.bake_indicator` stay config.toml's (and the console's).

The default quality is `ultra` (since prism-9): it bakes a level in minutes, so the main menu's bake feels like a
game's shader compilation. `max`, whose time is the darkest texels' (below, "Measurements"), can take hours a level
and stays for whoever asks for it; a config.toml that names a quality keeps it.

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
  (those inside the level left out), SH at its normal (never under the probe's ambient part, L0 less half its L1's
  length: the linear reading of a side turned from a probe's main light goes below it, to nothing where the light
  is all from one side, as a door the bake has not, lit through the doorway from behind, was on c20's tp_sec4 back
  wall), and the sun's light by its shadow traced against the bake's
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

**The moving objects' occlusion** (`display.prism_raster_object_occlusion` `"traced"`, the default, with Ray Tracing
on). The bake is made without the moving objects, so neither the lightmaps nor the volume hold the shade a vehicle or
a character casts about it: under a Warthog, about a Marine's feet, in an object's own creases and on its neighbours.
Path Traced has it (every ray meets the objects), Classic CE's projected shadow under each object, and before this
Enhanced had none at all: its traced sun shadows replace CE's projected ones, and in shade, indoors or under a sky
with no sun nothing stood in for them. With Ray Tracing off CE's projected shadow came back only as a share of the
sun's light, so not in shade either: it now darkens the rest of the light too (its whole texel, as Classic's does)
wherever the objects' occlusion is not traced. The game sets RT_FRAME's `XGPU_RT_FLAG_RASTER_OCCLUSION`, which
captures the objects as the traced sun shadows do (with them or alone: with Object Shadows Classic the sun shadows
stay CE's), and before the light pass a compute pass (`occlusion_source`, `xgpu_vulkan_bake.c`'s `raster_occlusion`)
traces, from each pixel the G-buffer has a normal for, cosine-distributed rays against the objects' structure (masks
4, 8 and the player's body, 16): a hit at t counts 1 - (t / reach)^2, as world AO's traced objects count, and the
ambient light left is 1 - strength x the mean.

- **A level pixel** is darkened by the objects alone, within 2.5 world units: the bake already has the level's and the
  static scenery's own occlusion (the static scenery is left out of the capture, `XGPU_GBUFFER_FLAG_STATIC_SCENERY`),
  so nothing is darkened twice.
- **An object's pixel** by the objects, itself among them, within 1 world unit (a Warthog's wheel wells, a Marine's
  arms over his chest): the volume's light is the place's, without the object, and its probes (0.25 wu apart) hold
  the level about it.
- **What it darkens:** the light less the sun's (the bake's texel or the volume's SH, the light the sun's shadows do
  not already take) and, with PRISM's materials, the reflections; never the sun's light (its own traced shadow) or the
  dynamic lights.
- **No noise, no history.** Each pixel traces two rays, a sixteenth of its 4 x 4 block's 32 directions (a spiral, by
  a 4 x 4 ordered dither); the light pass (`occlusion_at`) gathers the 4 x 4 texels about the pixel, each weighed by
  its nearness to the pixel's plane and its normal, and only those of the same surface (the level's, or the same
  object's), so every pixel sees every direction once and a Warthog's shade never bleeds onto the ground beside it or
  the ground's onto it. `tools/test_prism_object_occlusion.py` checks that any 4 x 4 window holds every direction and
  that the block's estimate is the converged integral's within 3% for a ceiling over a floor and a wall beside it.
- `HALO_RASTER_OCCLUSION="<rays> <level reach> <object reach> <level strength> <object strength>"` tries another set
  (a diagnostic, for the calibration against Path Traced); `HALO_RASTER_TIMING` logs the occlusion pass's and the light
  pass's GPU time every 300 frames.

**The lights' shadows** (`display.enhanced_light_shadows`: `"ultra"`, the default, `"high"`, `"low"`, `"off"`).
Classic and Enhanced lit the level with the moving lights unshadowed: the flashlight, a Warthog's headlights, muzzle
flashes, plasma bolts and the other lights CE's world_scene gives the frame (SCENE_LIGHTS' dynamic class) shone
through walls and from behind cover. The light pass now lights the level with them under either composition (with
CE's, CE's diffuse light passes' light is replaced, in CE's encoding, after the exposure and its curve; a gelled
light, the flashlight, at `RASTER_CE_GEL` of its spot) and traces their shadows as Path Traced's `light_seen` does:

- **Which:** of the lights reaching a pixel, those giving it the most (by luminance, at least 1/500 of white): 8 at
  ultra (the brightest two with two rays each, a soft edge), 4 at high, 2 at low; the rest light it unshadowed.
- **By what:** the level and the static scenery (the bake's scene) to the light's place, and, while Ray Tracing
  captures the moving objects (Object Shadows or Object Occlusion traced), the objects to `DYNAMIC_OWNER` short of it,
  so a lamp on or in its own object is not shadowed by it; a light within a world unit of the eye (the player's
  flashlight and muzzle flash) is not shadowed by the player's own body (mask 16).
- **Its edge:** the point aimed at is spread over the light's size (`DYNAMIC_SIZE` of its reach, `DYNAMIC_SIZE_MOST`
  at most, as Path Traced's) by the pixel's fixed pattern: soft as far as the caster is from the surface, no flicker.
- **The baked lamps and fixtures:** while the objects are captured, the 2 (1 at low) lamps of the pixel's importance
  cell and fixtures that light it most, in the bake's units, are taken from the baked light where an object blocks the
  ray and the level does not, never under a tenth of it: an object casts a shadow from a room's own lights.

Not in the bake's views (the gates' measures). Push flag bits 8 and 9 of the light pass, the resolve phase's bits 19
and 20 (`XGPU_GBUFFER_PHASE_RASTER_LIGHT_SHADOWS_*`).

**What composes the frame** (`display.prism_raster_composition`): by default (`"ce"`) CE's own shaders, until PRISM's
materials reach theirs: the diffuse light (exposed, through the tone curve, below) goes into CE's light target in its light's
place (the level's glow folded in as CE's lightmap pass folds it; an object's base colour, the G-buffer's, and glow
with it), and the resolve draws CE's base colour times it and CE's additions (its reflections, specular lightmaps and
specular lights) over it, as Classic's frame is drawn; the meter then takes both frames with the same base and
additions. `"materials"` (for the materials milestone) composes the frame whole from PRISM's own materials, as follows.

**What an object draws over its own light stays** (`render_deferred_window`, render.c). The relight (and the composite
that takes its place in Path Traced) gives an object's pixel the volume's light times the G-buffer's base colour plus
its emissive light, and it cannot see anything else CE drew into the light target there. A model's decal (a
`shader_transparent_generic` or `_chicago` with the decal flag, or a model shader that is an alpha-blended decal) is
drawn by CE at once, among the model's own passes, into the light target: the assault rifle's ammo counter and the
displays on weapons are such, so the relit pixel took them away (Classic and `ce_deferred`, which keep the light
target's pixels, showed them; Raster and Path Traced did not). From the G-buffer pass's end to the resolve
(`render_deferred_window`) a model's decal is queued as the model's other transparent parts are, drawn over the
resolved frame with them: after the relight and any composite, before the fog screens, with no exposure or curve on it
(a display is lit by its own). Classic is unchanged (the flag is set only where the window is drawn for the resolve).
Anything else an object drew at once into the light target belongs to the same rule: a pass of its own that the G-buffer
has no twin for (a blended draw) is lost to the relight.

**The level's alpha-tested decals go into the base** (`render_window`, render.c). CE draws the decal layer
`_decal_layer_alpha_tested` (a sign's paint, a floor's markings: a10's LIFEPODS arrows and star signs) over the light,
before the texture pass, multiplying it, and marks the stencil so that the texture pass leaves those pixels alone: the
frame there is the light by the decal, no base map. In the deferred core the light target held the decal and the base
target stayed white there, so once Raster (or Path Traced) put its own light in the light target's place the decal was
gone and the shape came out as the bake's light alone (white in a10's corridors). Under `prism_deferred_raster` the
layer is drawn as the base phase begins instead, before the texture pass, with the same stencil: the base is white by
the decal, the light target the light, and the resolve gives Classic's frame with the bake's light in its place (and
Classic's own where the bake has nothing). Classic and `ce_deferred` keep CE's order. A pack or mod material's albedo
(`xgpu_vulkan_post_albedo_over`) still lies over such a decal where the level's surface under it has one.

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

## Path Traced short of the card's memory

Path Traced traces the bake's scene (its structure and the buffers its scene block points into) and never reads its
light. Where the card has no room for Path Traced's images beside it (docs/prism_pathtraced.md, "No room on the
card"), `xgpu_vulkan_bake_park` moves the light's buffers (the virtual pages, charts, tile table and pool, the
indirect pool, the sun mask's table and masks, the volume's probes and bricks, the reflection probes, the final
gather's atlas and map, the batches' working buffers: `park_buffer`) to the host's memory: each copied on the GPU,
the card's let go of after the submission, the final gather's cache block written again with the new addresses
(every pass reads the buffers by their addresses, so a pass that ran meanwhile would only be slower). Only a bake
that bakes nothing more is parked: a whole bake's (the shipped one or this machine's, perhaps still being read from
its file), an idle one's, a done or failed one's; never one being baked, relit or filtered (`park_allowed`). Raster
moves them back before it draws (`bake_unpark`); where the card has no room for them yet it lets go of Path Traced's
images (`xgpu_vulkan_pt_release`) and draws from the host's memory meanwhile. The scene's buffers and structures
never move. And where the card has no room for a light's buffer as it is made (the steps after the scene's upload),
it is made in the host's memory rather than the bake failing ("no memory for the irradiance volume" on the 6 GB RTX
2060 once Path Traced's images were made first), said once, and moved to the card by the next `bake_unpark`.

## Measurements

See `docs/prism_gates.md`, "M2 as measured": the bake against R, Raster against R, the frame times, the bake's time
and size per map, and Classic's byte identity. In short: `ultra` bakes a BSP in one to thirty minutes (b30's first in
161 seconds on an RTX 3070 Ti, 1 GB); `max` as D4 defined it did not finish (b30's first BSP 99.6% done after 5.7
hours), its time going to the darkest texels, until its cap of 32,768 samples a texel (above); Raster costs 2.3 ms a frame at 1440p on the RTX 3070 Ti.

## What remains

- `max`: path guiding or a light cache between texels, or an error floor nearer the eye's (Adam's decision).
- The bake against R on equal scenes (`bake_flip`, 0.05) still fails after the noise's steps: a10 G5 0.0818 to 0.0716,
  c40 G7 0.0623 to 0.0584 (high, the structure alone, seed 1; 3070 Ti, R's frames of `v4_m2_runs/r2/eq/refs`). What is
  left is not the bake's grain: at G5's first pose the light alone is at 0.18 against R, the bake's pillar and floor
  darker than R's, and R's own speckle (unconverged where lamps light the scene) in every pose.
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
- The indirect layer's use by M3's path tracer; BC6H for the reflection cubes. The layered bake's indirect for the
  domes', fixtures' and lamps' bounces is scaled by their mix (exact at the defaults), its direction kept as baked,
  the reflection probes by one ratio for the BSP; the final gather (a diagnostic) is not layered.
- The remodel needs no atlas of its own: its corners carry CE's lightmap coordinates of the point of their surface, so
  CE's chart map finds a remodel pixel's texels (b30's G2: every level pixel). Its charts are split finer than CE's
  (b30's first BSP: 17,224 against 8,378: its corners on a chart's inner edges need not meet in the same
  coordinates), each with its own density and so its own scale; a CE texel where two of them meet takes the nearer's
  virtual page. No seam was seen at G2 and G3; a whole-BSP check of those borders is not made.
