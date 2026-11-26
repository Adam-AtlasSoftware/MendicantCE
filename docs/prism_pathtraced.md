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

**Under an upscaler** the G-buffer is drawn jittered, so every pass that finds a pixel's point again from its
distance takes the jittered draw's rays: the path tracer's cameras (`pt_camera_jitter`, in "The denoiser"
below), and Rebaked's light pass and the terrain's HQ layers (`xgpu_vulkan_upscale_jittered_camera`, the same turn of
SCENE_VIEW's axis). Before, at Ultra Performance (0.33) b30's hills went dark in one frame of three and lit in the
next with the jitter's sequence, whatever the bake (`work/openh1_eval/v4_groundflash_runs`: the hill's frames' mean
luma 44 to 164 frame to frame, steady at 138 after; the beach's small periodic flashes, +6 every ninth frame, gone
too).

## Which light each term carries

The bake's unit is a white surface's radiance (the irradiance over pi). At an opaque pixel, its diffuse light `D` (the
base colour multiplies it) and its specular light `S`:

| Term | Light | `quality` | `performance` | The reference |
| --- | --- | --- | --- | --- |
| Sun | The sky's sun, direct | A ray to a point of its disc at every pixel (since round 3: the mask's texels missed thin sunlit edges far off, round 2's G3), except where the pixel's tile of the bake's mask is all in shadow (none of its 16 × 16 texels' samples saw the sun; since round 7, `HALO_PT_SUN_TILES`). `D` += sun × cos × v, `S` += GGX × the sun × v | A ray within 60 world units of the eye and on every object pixel; past 60 the baked sun mask (the level's pixels) | A ray to its disc at every pixel |
| Domes | The sky's other lights, direct | One direction of each one's cone, its shadow traced (`domes_at`): `D` | The same | The same |
| Lamps | The emitting triangles (light cards, lamp surfaces), direct | ReSTIR DI: a reservoir a pixel, its initial candidates 8 lamp points from the light-importance grid (`lamp_pick`: the cell's seen and near lists, the rest by power: every lamp can be picked), each by its unshadowed diffuse and specular light; the last frame's reservoir at the surface's place reused (below); the kept sample's shadow traced (`lamp_visible`: any surface before the lamp's point blocks it, another lamp's too, since `prism-v4-m2` b2e459b8b): `D` and `S`; on a pixel whose reflection is traced (roughness up to 0.35) the lamps' `S` shared with the reflection's GGX ray meeting a lamp's face (`lamp_met`) by multiple importance sampling: the power heuristic between the ray's density (GGX's visible normals) and 1 over the lamp's solid angle (`lamp_mis_nee`), the two weights summing to 1 at every direction | The same | Resampled importance over the same 8 candidates, each one's shadow traced and its visibility in its target (`lights_ris_seen`), no reservoir reused |
| Fixtures | The scenario's light fixtures, direct | In the same reservoir (all of them, or 8 by count) | The same | The same (16) |
| Dynamic lights | SCENE_LIGHTS' dynamic class (flashlight, muzzle flashes, plasma and needles, grenades and explosions, the weapons' and vehicles' lights), direct | Apart from the reservoir (`dynamic_direct`): resampled importance over every one (64 at most), a windowed inverse square with CE's whole light and CE's cone, a sphere light's highlight, the kept one's shadow traced; what glows with them ("What glows"); the histories cut where they changed the light ("The dynamic lights", below) | The same | In the reference's resampled importance with the lamps (16) |
| The sky's floor | The units' rig's ambient, unoccluded, as Tool's: the sky's outdoor value in the structure's clusters with a sky, sky 0's indoor value in those with none (`prism-v4-m2` 4acaa7892, `XGPU_SCENE_INDOOR`) | A constant, `D`: the flag of the structure's triangle the pixel shows (a short stretch of the eye's ray about its point), an object's the structure below it (a ray down past the object) | The same | The same |
| Indirect | Every bounce of every static light, the sun's bounce included | One cosine bounce, its hit lit from the bake (or, with `HALO_PT_BOUNCE_HALF`, off by default, on half the pixels the bake's indirect layer at the pixel as the bounce's sample: below): the hit's glow and albedo by its texel's light less the sun's and less the floor its triangle took (indoor or outdoor; the volume's less the floor where its probes stand), the sun there by the mask (or the volume's visibility); a miss the sky without the domes (the pixel took them as lights) | The bake's indirect layer at the pixel where the level has its texel; else (the objects, a pixel with no texel) `quality`'s bounce | A cosine path of up to 32 bounces (prism-v4-m2 bb2a6b1a4, the bake's alike), next-event estimation of the sun, lamps and fixtures at each hit, the domes at each hit by multiple importance sampling of a direction of their cone and the path's own cosine direction where it leaves (Veach's power heuristic: the bake's light, the quieter estimate for a cone of any width) |
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
- **The probes** see the lamps' faces as the bake's paths do (by their glow), and ReSTIR DI gives a rough pixel the
  lamps' specular light as well, shadowed and sharp. Counted by both, c20's lamp-lit Library walls had 47% more
  specular light than R (the live estimator's own 256-frame mean against R: 0.0552 against 0.0375). Since the dark
  interiors' round the probe's light on a pixel with a texel is kept by the share of the bake's light there that is
  not the lamps' own (its indirect layer over its whole, `luma(indirect) / luma(total)`), and ReSTIR DI keeps the
  lamps' specular light: c20's still error against R 0.0576 to 0.0099, its blotch 0.0516 to 0.0285, the other poses
  unchanged. Taking the lamps' specular light from the probes alone instead (`HALO_PT_PROBE_NEE=2`) lost the lamps'
  glow pools on the walls that R has (c20 0.110); both, as before, is `HALO_PT_PROBE_NEE=1`. Since the second round
  the probe is scaled only in an indoor cluster (`XGPU_SCENE_INDOOR`, the structure's flag at the pixel), with the
  units rig's floor kept beside the indirect layer: the bake's direct light holds the domes' sky as well as the
  lamps, and outdoors that share is the sky's, which no lamp sample gives back. Measured at 720p against R, the
  scaling was never visibly darker outdoors (b30's beach and a30's cliffs, sunlit: still error and energy within 6% and
  1% with it, without it and indoor-only; a10's cryo bay within 1%); c20 keeps most of its gain (still error 0.060
  unscaled, 0.010 scaled everywhere, 0.013 indoor-only). A fixture's light on the wall still dims the probe by its
  share (only the lamps' faces are in a probe); a lamp mask in the probes' bake would end that.

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

The lamps and fixtures are sampled by ReSTIR DI (Bitterli et al. 2020; the dynamic lights apart, "The dynamic lights") with the reuse `[lessons]`
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
  Since round 6 each node also bounds its lamps' normals by a cone (its axis and half angle, merged from its children's
  as Conty Estevez and Kulla's orientation bounds; a leaf's, its triangle's normal), and a node's importance takes the
  best cosine any of its lamps can have toward the point (the angle to the cone's axis less its half angle and the
  sphere's angle from the point; nought past a right angle, the lamps being one-sided): a wall's lamps facing away
  from the point no longer draw its picks. `HALO_PT_TREE_CANDIDATES` (1 to 8, 4 by default) sets how many of the
  initial candidates come from the tree (the rest the grid's), each weighed by its own source's probability. Until the
  dark interiors' second round that count and the cones' switch sat in the job's `volume.w`, which the recent dynamic
  lights' count then overwrote: the tree gave 1 + that count (mod 16) candidates and its cones came on past 16 recent
  lights. They are the reuse flags' bits 4 to 7 and 3 now.
- **The specular lobe in the tree** (the dark interiors' second round, `HALO_PT_TREE_LOBE`, on by default): for a
  surface under a roughness of 0.9, a node's importance adds the surface's specular lobe to the cosine bound: the
  GGX distribution at the half angle nearest the node's bounding cone about the mirror direction, by the surface's F0,
  over 4 n.v (Conty Estevez and Kulla 2018's importance with a BRDF term). Only the descent's choices change; its
  probability is the product of the choices it made, so each candidate's weight stays exact
  (`tools/test_prism_pt.py`). **All 4 candidates are the tree's by default.** The grid's lists rank a cell's lamps by
  the light they give its few points: a small lamp whose glint a semi-glossy, normal-mapped wall shows but whose
  diffuse light there is little was a candidate in hundreds of frames, each find a white sparkle the denoiser held
  (b30's G4 walls, c10's lab door, a10's corridor floor through the door windows), and the frames lost that light
  (energy 0.80 to 0.90 of R's at those poses). The tree, with the lobe, finds those lamps most frames. At 720p
  against R (`v4_dark_interiors_runs`, variants T0, U1 to U4): c10's lab still error 0.045 to 0.015 and moving
  0.084 to 0.036, a10_g5k1's moving error 0.112 to 0.030, b30_g4k1's moving 0.156 to 0.076, b30_g4k2's 0.116 to
  0.060, the frames' energy back within about 5% of R's; c10's corridor, c20 and a30 unchanged. 2 tree candidates
  without the lobe left a10 at 0.083; 2 with it, 0.044; 1 with it, 0.056. The final build at 1080p against
  prism-9 (variant G): moving error at b30_g4k1 0.132 to 0.070, b30_g4k2 0.101 to 0.050, c10's lab 0.079 to 0.027,
  a10_g5k1 0.102 to 0.025, c20 0.064 to 0.023; still error at c10's lab 0.047 to 0.012 and b30_g4k2 0.053 to 0.027;
  the bright and outdoor poses unchanged within the runs' spread. b30's still frames are now slightly brighter than R
  (1.07 to 1.10 of its energy; prism-9 0.80 to 0.92). **Cost**: a descent of the tree a candidate in place of a cell's
  lookup. Whole-card Atlas runs, the light pass against the denoiser's passes of the same run (the cards' clocks
  varied threefold from run to run), put each tree candidate at 8 to 16% of the light pass, 4 at 30 to 50%: an
  estimate of about +3 ms of the RTX 3070 Ti's at G4, 1080p, not yet timed on that card. `HALO_PT_TREE_CANDIDATES=1`
  gives back the old count. 2 in the performance profile left b30's G4 darker than prism-9 (0.70 to 0.77 of R's
  energy) and were not kept; one descent shared by the 4 picks was slower than four. Under DLSS Ray Reconstruction the
  salt is gone too, but its frames are brighter than before at b30 and c10's lab (1.34 to 1.46 of R's energy, from
  1.16 to 1.19), the brightening of RR noted in prism_plugin.md.
- **Temporal reuse by default.** The last frame's reservoir at the surface's place there (the G-buffer's motion), reused only
  where its guide is this surface's (the same instance, the normal, the plane: as the denoiser's history, so no sample
  crosses an edge or an opening), its sample's target evaluated again at this pixel, its confidence capped at 8
  frames. The last frame's pixel is one of the 2 × 2 about the surface's place there, chosen by its bilinear
  weight (the nearest gave a lucky sample to every pixel that maps to it when the eye moves toward a surface: blocks
  that grew along the move, G4's floor streaking); ReSTIR GI's alike. A dynamic light is not in the reservoir ("The dynamic lights"). Spatial reuse is
  off by default (below): in `prism-pt` it spread a pick's choice over patches, and here its blobs in motion remain.
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
- **Spatiotemporal reuse, on by default since round 7** (`HALO_PT_RESTIR_SPATIAL=<0 to 4>`, 1 by default): besides
  the last frame's reservoir at the place, that many of the last frame's reservoirs at points of a disc of 2 to 16
  pixels about it (`HALO_PT_RESTIR_RADIUS`), each this surface's by the same test, its sample's shadow traced from
  this pixel. A neighbour's confidence counts only where the kept sample lights the neighbour's point, in front of it
  and seen from it (Bitterli et al. 2020's Z: a shadow ray from the neighbour, none where the kept sample is that
  neighbour's own), so the reuse is unbiased (round 6; before it, 1/M took the light of every pixel whose neighbours
  see lamps it does not: G4's floor in motion a third darker than R; `HALO_PT_RESTIR_Z=0` counts where the
  neighbour's own sample lit this pixel, no ray, measured worse than none).
  **The reservoir kept for the next frame is the temporal one** (the initial reservoir merged with the last frame's at
  the place), not the spatial result: the neighbours' samples light this frame alone. Round 6 kept the spatial
  result, as Bitterli et al. do, and so a lucky sample (a semi-glossy wall's highlight of a lamp the candidates find
  once in hundreds) was offered to the pixels about it, kept by them, offered again by them the next frame: a walk
  from pixel to pixel over the frames, its light spread over the disc and held there for the confidence's frames,
  which the a-trous steps then smoothed into blobs the size of the disc (`v4_reuse_runs/evidence/g4_p2_motion.png`,
  row 2). That is the correlation Wyman and Panteleev 2021 manage by capping the confidence and by the boiling
  filter; here it is cut at its root, at the cost of the chains' length (each pixel's own history alone builds its
  reservoir's quality). `tools/test_prism_pt.py` models a row of pixels: the two unbiased alike, the frame's errors
  of pixels six apart correlated a half as much and under 0.15 without the feedback.
  `HALO_PT_RESTIR_FEEDBACK=1` keeps the spatial result (round 6's). With the reuse the initial candidates are 4 (the
  neighbours' reservoirs bring the rest; 8 without it).
  Measured (G4, the RTX 2060's moving frames against R, error / blotch / FLIP / flicker; `v4_reuse_runs/g/`):

  | variant | G4 |
  | --- | --- |
  | no spatial reuse (round 6's defaults) | 0.1814 / 0.0823 / 0.119 / 1.34 |
  | 1 neighbour, the spatial result kept (round 6's, `FEEDBACK=1`) | 0.1578 / 0.0807 / 0.094 / 1.39 (blobs in motion) |
  | 1 neighbour, the temporal reservoir kept | 0.1462 / 0.0772 / 0.114 / 1.30 |
  | 2 neighbours, the temporal reservoir kept | 0.1374 / 0.0741 / 0.110 / 1.29 (2.0 ms more than 1) |
  | 1 neighbour, temporal permutation sampling | 0.1583 / 0.0741 / 0.113 / 1.22 (the first pose darker) |
  | 1 neighbour, temporal kept, 4 plain a-trous steps | 0.1644 / 0.0857 / 0.114 / 1.34 |
  | the same, 5 steps | 0.1530 / 0.0808 / 0.114 / 1.30 |
  | round 7's defaults: 1 neighbour, temporal kept, 4 candidates, the wide a-trous steps, the sun's dark tiles, ReSTIR GI's reuse off | 0.1549 / 0.0783 / 0.114 / 1.32 |
  | **round 8's defaults**: round 7's with the specular hold (two runs) | **0.1546 / 0.0725 / 0.116 / 1.30**; 0.1508 / 0.0745 / 0.115 / 1.30 |
  | round 8's with 8 candidates | 0.1498 / 0.0723 / 0.113 / 1.30 (17.69 ms) |

  G5 (a10), the same gate: round 6's defaults 0.1403 / 0.0773 / 0.132 / 1.01; round 7's 0.1522 / 0.0781 / 0.133 /
  1.00 (its first and third poses better, its second, the corridor's floor through the door windows, worse). Round 8
  found the cause by bisection (G5, the second pose's error): round 7 with the spatial reuse off 0.1406 (8
  candidates come back with it off), with five plain a-trous steps 0.1522 (not the a-trous change), with the reuse on
  and 8 candidates **0.1248** (every pose better than round 6's: 0.0367 / 0.1248 / 0.0686), with 6 candidates 0.1388.
  So the spatial reuse helps G5 too; what costs its second pose is the initial candidates cut from 8 to 4 to pay for
  it: the floor's reflection of the corridor's lamp strips is a lamp the grid's lists rank low, and 4 candidates find
  it less often than 8 (the reuse cannot pass on a lamp no pixel about it found). 8 candidates cost 1.45 ms of the RTX
  3070 Ti's light pass at G4 (17.69 ms a frame) and 6 cost 0.79 (16.98), both over the 16.7 ms target, so 4 stay the
  default; `HALO_PT_CANDIDATES=8` (or 6) is the setting for a faster card. A cheaper target for the candidates (the
  BRDF is evaluated whole for each) is the way to win the rest back.

  What is still seen in motion (`v4_reuse_runs/evidence/`): over a lamp's pool on the semi-glossy walls, small
  clusters of bright pixels about a lucky sample (the neighbours that took it this frame) where round 6 showed blobs
  and the base a salt over the whole wall; round 8's specular hold (below, "The denoiser") takes most of the radial
  streaks of the walls' glints along a forward move. Looking at the specular and the diffuse light alone in motion
  (`debug.gbuffer_view` `pt_specular`, `pt_diffuse`; the gate's states `v4spec`, `v4diff`): the specular's specks are
  single sharp pixels (lucky lamp samples on bump-mapped glossy walls); the diffuse's are soft blobs of a few pixels,
  a lucky sample the long diffuse history holds and the a-trous steps spread.
- **The switches that are off, and which are safe** (round 8; each is a diagnosis or an experiment, none for play):

  | switch | what | status |
  | --- | --- | --- |
  | `HALO_PT_RESTIR_FEEDBACK=1` | the spatial result kept for the next frame (round 6's) | unbiased, but blobs in motion: do not use |
  | `HALO_PT_RESTIR_PERMUTE=1` | temporal permutation sampling | **biased** (darker: G4's first pose 0.0957 to 0.1583): do not use |
  | `HALO_PT_RESTIR_BOIL=<0..1>` | RTXDI's boiling filter | unbiased in effect; let go of nothing at G4 |
  | `HALO_PT_BOUNCE_HALF=1/2` | the bake's indirect light as the bounce on half the blocks or pixels | unbiased (the same light); no time saved |
  | `HALO_PT_RESTIR_GI=1` | ReSTIR GI's temporal reuse | unbiased; 0.45 ms, no gain at G4/G5 |
  | `HALO_PT_CANDIDATES=6/8` | more initial lamp candidates | unbiased; G5 0.1388 / 0.1248, 0.8 / 1.45 ms more |
  | `HALO_PT_SPEC_HOLD=0` | the specular hold off (round 7's) | the radial glint streaks back |
  | `HALO_PT_PREBLUR=1`, `HALO_PT_CACHE=1`, `HALO_PT_INDIRECT_CLAMP` | earlier rounds' pre-blur, radiance cache, indirect clamp | see their sections |

  Round 8 also tried an anti-firefly in the fix pass (each pixel held to the second brightest of its 16 neighbours
  on the same surface, for the a-trous steps only): on the specular alone G4 0.1549 to 0.1542, the blotch 0.0783 to
  0.0780; on the diffuse too, at any history length, the error worse (0.1556) and FLIP worse (0.117), 0.18 ms more.
  Removed.
- **Reuse options tried and left off** (round 7): temporal permutation sampling (`HALO_PT_RESTIR_PERMUTE=1`, RTXDI's:
  the last frame's reservoir from a pixel of the 4 × 4 about the place by a permutation turned each frame) lowers the
  flicker (1.30 to 1.22) but darkens: a reservoir from another pixel is a spatial neighbour counted without its Z,
  the 1/M bias again (G4's first pose 0.0957 to 0.1583). The boiling filter (`HALO_PT_RESTIR_BOIL=<strength>`,
  RTXDI's, each kept reservoir's W against the mean of the others' in its subgroup of 32) let go of nothing at G4:
  a lucky sample of a bright light has a large target too, so its W is no outlier; filtering by the estimate (W by
  its target) would take such samples and the light they find with them (`tools/test_prism_pt.py`). The confidence
  Half the frame's blocks with spatial neighbours a frame saved no time. `HALO_PT_RESTIR_M` sets the reservoirs'
  confidence cap (8 frames by default).
- `HALO_PT_CANDIDATES` (1 to 8; 4 by default with the spatial reuse, else 8): the live frames' initial lamp candidates a pixel.
- `HALO_PT_RESTIR=0` turns the reuse off (each pixel's own candidates alone), for comparison.

### The lamp ray (the dark interiors' round)

A rough pixel whose reflection the probes give (roughness above the traced reflection's 0.35, up to
`HALO_PT_LAMP_RAY`, 0 by default since the second round, 0.7 as measured here) traces one GGX ray (60 units at most) that only looks for a lamp's face
(`lamp_met`); what it meets is shared with ReSTIR DI's specular light by the same power heuristic as the traced
reflection's (`lamp_mis_nee`), the rest of the reflection is the probe's. Semi-rough, normal-mapped walls (roughness
0.4 to 0.5: b30's G4 walls) showed every small lamp's highlight as white sparkles: ReSTIR's candidates are drawn by
the lamps' light at the cell, so the lamp a glint's lobe holds was met once in hundreds of frames, each time far
brighter than the pixel's mean. The GGX ray's weight is bounded (Fresnel by G2 / G1), so the lamp it meets adds its
radiance at most. **The lamps' weights** (`HALO_PT_LAMP_MIS`, on by default) take the lamp's own density as the
chance ReSTIR DI finds it: 1 over its solid angle (as before) by its unshadowed diffuse light here over the bake's
direct light at the pixel (the texel's whole less its indirect layer) by twice the candidates, at most 1; a small
lamp that adds little light but a bright glint goes to the ray, a lamp that lights the pixel stays ReSTIR's. Any
weights that sum to 1 keep the estimate unbiased. At 720p, against the probes' fix alone (single frames, whose
spread at b30 is wide): b30_g4k1's still error 0.267 to 0.163 and dark blotch 0.175 to 0.092, b30_g4k2's still
error 0.124 to 0.047; the other poses unchanged. b30's walls still sparkle in motion; their lamps' glints are not
all the ray's to find (b30 has 32 fixtures, of which ReSTIR draws 8 at random a pixel: the next thing to look at).
**Off by default since the second round**: its cost was never measured (about 1 to 1.5 ms of the RTX 3070 Ti's at G4
estimated) and the lobe-aware tree candidates find the same lamps at no ray; with them the ray changed nothing
measurable. Weighing all 32 of b30's fixtures each frame instead of 8 at random was tried and left out: the live
estimator's own 1024-frame mean fell to 0.89 of R's at b30_g4k2 (0.99 with 8), and the blotch rose.

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
There is no spatial reuse yet. Off by default since round 7 (`HALO_PT_RESTIR_GI=1` turns it on): it cost about 0.45 ms
of the RTX 3070 Ti's light pass at G4 (measured in one run with the parts left out in turn, below), where the bounce
already ends in a converged bake texel, and the frame had to fit 16.7 ms with ReSTIR DI's spatial reuse on.

## The dynamic lights

CE's runtime lights (the flashlight, muzzle flashes, plasma bolts and needles, grenades and explosions, the energy
sword's and plasma weapons' glow, the vehicles' and the Covenant's lights, the effects' lights) reach the path tracer
as SCENE_LIGHTS' dynamic class: the lights the rasterizer took the frame before (world_scene's list,
`WORLD_SCENE_LIGHT_DYNAMIC`), each a point or a spot (its axis, CE's falloff and cutoff angles), its colour as CE
draws it (a light drawn reaching nowhere is sent off). Until `prism-v4-pt-dynlights` none did: SCENE_LIGHTS read
world_scene's fixture bit (1) as the dynamic one (2), so the table's dynamic class was always empty, in Path Traced
and in Raster alike (Raster's `dynamic_at` reads the same class).

- **Shape and units.** A point light (a bolt, a grenade, a blast, a muzzle flash, a shield's flare) is a small
  glowing body whose power its reach sets: its intensity `DYNAMIC_POWER` (0.3) r times CE's colour, over the square
  of the distance from its size (`DYNAMIC_SIZE` 0.1 r, `DYNAMIC_SIZE_MOST` 0.08 units at most), windowed to none at
  CE's reach r ((1 - (d/r)^4)^2), never more than `DYNAMIC_NEAR_MOST` (2) times CE's own 1 - d^2/r^2 at that
  distance. CE's reach is the radius of a flat sphere of light: taken as a light's size (`prism-v4-pt-glow`'s
  shape: CE's 1 - d^2/r^2 within 0.6 r) a fuel rod striking the Chief lit the corridor end to end with his shield's
  flare, and a plasma grenade's 1 unit gave the floor under it a dim 0.75. Taken as its power, a blast's 10 units
  light a wall 3 units off at a third of CE's and 5 units off at a seventh, and a grenade's floor 0.3 units under it
  at twice CE's (`prism-v4-pt-glow2`; tools/test_prism_pt.py). A spot light (the flashlight) keeps the windowed
  inverse square of intensity 0.3 r^2, never more than CE's 1 - d^2/r^2: CE's within 0.6 r, the inverse square's
  tail past it (its whole light about three quarters of CE's). Held at 1.9 times CE's within 0.4 r instead, a wall 1
  to 2 units from the flashlight's mouth was a white disc, white through most of CE's fade. A spot's cone is CE's: whole
  inside the falloff angle, to none at the cutoff, linear in the cosine. A light with a gel map (the flashlight's
  projected texture) is not drawn through it: its colour is taken at the gel's share, 0.12 (`WORLD_SCENE_GEL`,
  world_scene.c), calibrated by the spot's peak, not its median: on b30's wall at 1 to 2 units, with the demo's
  fixed exposure and filmic curve, the spot's 95th percentile about CE's (200 of 255) and its share at white about
  CE's (2%), 0.10 and 0.15 either side (`work/openh1_eval/v4_int2_runs/calib4`, `tools/spotstats.py` there). The
  0.45 of a median per-pixel ratio (`work/openh1_eval/v4_dynl_runs/calib`) left a fifth of the spot at white.
- **Sampling and shadows.** Apart from ReSTIR DI's reservoir (`dynamic_direct`): resampled importance over all of
  them (`DYNAMIC_ALL`, 64; 4 at random until `prism-v4-pt-glow`, which in a fight missed the one light near the pixel
  most frames) by their unshadowed diffuse and specular light, one shadow ray for the kept
  one: by the level to the light's place, by the moving objects to 0.15 world units short of it (`DYNAMIC_OWNER`: a
  light on or in its own object, a vehicle's lamp, a weapon's glow, is not shadowed by it). Not in the reservoir
  since the last frame's reservoirs never hold a dynamic light (their order changes from frame to frame), so the
  reuse's confidences took the rest of its weight: a dynamic sample kept by the reservoir was weighed by 1 over
  1 + up to 8 reused confidences, a flashlight coming in at a ninth of itself over many frames. The reference
  (`lights_ris_seen`) keeps them with the lamps (no reuse there).
- **The denoiser.** The job block carries the recent dynamic lights beside this frame's (`JOB_BYTES` 8192: 64 of
  each, 12 floats a light; `job.volume.z` and `.w`). A recent light (`recent_lights_take`, xgpu_vulkan_pt.c) is each
  light of the frames before, by its id, where it was last frame, its colour the most of its last frame's and its
  recent colour less 1/32 of itself a frame (the slow history's own share: what the history may still hold of it),
  until that is under 1/500 of white. The temporal pass evaluates this frame's and the recent ones at the pixel's
  surface (unshadowed, diffuse) and cuts the diffuse and specular histories by the change's share of the light the
  history holds besides theirs (`dynamic_change`): the histories at most as long as keeps their lag under a tenth
  of the light (`DYNAMIC_LAG`: a change of that share a frame, `0.1 / share` frames; a cut to `(1 - 2 x share)^2`
  of `denoise.x` kept about 21 of 27 frames as the flashlight came on, the spot coming in over 12 frames, and
  stayed whole through CE's 24-frame fade), and where that keeps under a tenth of `denoise.x` (`DYNAMIC_RESET`)
  the histories start again at the frame's own pre-blurred light (four frames
  long, the variance the neighbours'; not the fix pass's wide gather, which made a flash over the level's sparse
  bright samples into blocks). So a light switched on is whole in its first frame (not held as a firefly against a
  long history, not a 32nd of itself); a muzzle flash's flicker keeps a few frames; one switched off, moved on or
  fading out (CE fades the flashlight over 24 frames, each frame's change small) leaves no trail: by the last
  frame's light alone the flashlight's spot stayed a ring on the wall for 200 frames. A steady light, or one small
  beside the room's light, keeps its history.
- **Diagnostics.** `debug.dynamic_lights_log` (`HALO_DYNAMIC_LIGHTS_LOG=1`) logs the dynamic lights at each step
  where their count changes: the guest's (the rasterizer's, world_scene's list, those sent) and the path tracer's
  (those in the frame's job).
- **As measured** (`work/openh1_eval/v4_dynl_runs`, b30 BSP 1 at G4's place, the demo's settings and bakes, the
  player put there, the flashlight on and off, a burst of the assault rifle: `tools/dyncap.py`): before, the light
  table's dynamic class held the fixtures (world AO "world") or nothing, the path tracer's frames 0 dynamic lights
  (`evidence/stage_counts.txt`); after, the flashlight and the muzzle flash light the walls and floor with the
  player's traced shadow, none left after either (`evidence/flashlight_ab.png`, `evidence/muzzle_flash_ab.png`,
  RTX 2060). Raster's "ce" composition is unchanged frame for frame. RTX 3070 Ti, 1080p, quality: the flashlight
  on costs 0.35 ms (18.32 to 18.67 ms of the path tracer's, its light pass 12.17 to 12.47); with no dynamic light
  nothing (G4: 16.90 ms before, 16.86 after). G4's gate measures as before (0.1769 / 0.0820 / 0.122 / 1.34);
  Classic and the defaults byte-identical (identity.py frames, b30).

## What glows

Until `prism-v4-pt-glow` the dynamic lights were what CE's rasterizer took: a light with CE's dynamic flag (the
flashlight, the muzzle flashes, the grenades and explosions, the energy sword), and with display.dynamic_lights
"enhanced" a projectile's light too. Most of what glows in the campaign has a light that is a lens flare's alone in
CE (no dynamic flag: the plasma pistol's and rifle's bolts, the Banshee's and Ghost's, the plasma weapons' and
needler's glow, the vehicles' cockpit and running lights, the Hunter's cannon, the doors' lock lights), and the
strikes of plasma and needles have particles alone. The inventory, tag by tag and in play, is
`work/openh1_eval/v4_glow_runs/glow_inventory.md` (`debug.glow_inventory`, HALO_GLOW_INVENTORY=1, logs it).

- **CE's lights, all of them** (`port/linux/game/glow_lights.c`, `object_lights.c`): in Path Traced (and Raster's
  "materials" composition, which reads the same class) every light with a colour is drawn and so listed, at its
  tag's colour and reach as the enhanced dynamic lights draw a projectile's, by `display.prism_glow_lights` (0 to
  4, 1 the default, 0 none: CE's own set); a weapon's at a quarter (`GLOW_WEAPON_SHARE`: the plasma pistol's flare
  is carried by every Grunt and Jackal, its glowing part small beside its flare). The shields' glow
  (`shield_glow.c`, display.shield_glow) is on in Path Traced whatever that setting says; its
  display.shield_glow_intensity "light_show" (the demo's) makes a flare full sooner but no longer three times as far
  in Path Traced (4.2 units: the Chief's flare struck by a fuel rod lit b30's corridor end to end); Classic's as it
  was.
- **The strikes.** A projectile's light is followed while it flies; once the projectile is gone (it struck or
  burst) its light flashes where it was, drawn back 0.15 units along its path off the surface, 1.5 times brighter and
  further, to none over 6 ticks (0.2 s) of the game's clock (`STRIKE_*`).
- **Needles.** A needle carries no CE light, and its model's shader is a transparent one (`needler luminous`), not
  a self-illuminated one: it had no light (the first round's "derived light" of a needle was the plasma grenade's
  button's). A projectile with no light and no glow of its colour (`glow_projectile_tint`, glow_lights.c) now lights
  with the colour of the glow of the weapon that fires it: its weapon's lens flare light's (the needler's `needle
  flare`, magenta 0.94 0.44 1), never a muzzle flash's (the bullets would glow), as a derived light (below). The
  needler's flare lights the weapon's surroundings, and the needles' burst (`needler\explosion`, dynamic) its own.
- **What glows with no light.** A projectile in flight or a grenade on the ground whose model has a self-illuminated
  shader and no light of its own gives a point light at its bounding sphere's middle, its colour the shader's
  self-illumination as CE draws it (between its bounds by its object function), reaching 3 times its size (1 to 4
  units), at half strength. Not the bodies, vehicles and weapons: their self-illumination colours are white over the
  whole armour, masked to small parts by maps the game does not read; their glowing parts carry CE's lights (the
  weapons' flares, the vehicles' lamps) where they show.
- **A light has a size.** A dynamic light's highlight is a sphere light's (`dynamic_specular`): the GGX lobe widened
  by `DYNAMIC_SIZE` (0.1) of its reach, `DYNAMIC_SIZE_MOST` (0.08 units) at most, over its distance, its energy kept
  by (a / a')^2 (Karis 2013): as a point, a bolt over b30's polished floor drew a highlight as bright as the sun's in
  a pixel or two. (Without the cap, the flashlight's reach of 12 made it a sphere of 1.2 units and its spot on the
  wall it pointed at a seventh of itself: `prism-v4-pt-glow`'s first commit, c246880f5, put the flashlight out in
  effect; `evidence/flashlight_offon_ab.png`.)
- **The look** (`dynamic_look`, prism_scene_lights.c; Path Traced and Raster's "materials" only). CE's light is its
  colour over its radius, and its radii are in world units (3 m each): a plasma pistol's muzzle flash floods a wall
  six metres across, and its colours are a channel or two whole and the others none. As CE gives them the fight's
  walls and floor were washed neon green, cyan and magenta (`evidence/fight_ab_r2.png`, the 40f2b1f95 and c246880f5
  rows). Each dynamic light now takes `display.prism_dynamic_lights` (0.4, 0 to 4) as far as its colour is a colour
  (its chroma (most - least) / most: the flashlight, white, keeps its whole light; a plasma bolt's green takes 0.4),
  its colour moved toward its own luminance by `display.prism_light_saturation` (0.6; its luminance kept), and a lens
  flare's light `GLOW_FLARE_SHARE` (0.4) of itself (a weapon's flare at that times GLOW_WEAPON_SHARE), a
  projectile's whole (`GLOW_PROJECTILE_SHARE`, `light_port_object_type`: a plasma or Banshee bolt is the light it
  flies by; its strike's flash alike). Calibrated on the setup4 fight at tick 260 (`cal260/`: 1, 0.6 and 0.4,
  saturation 1, 0.6 and 0.4); swept again with the point lights' new shape (`v4_glow2_runs/f_dev2`: 0.4 and 0.6,
  saturation 0.6, 0.8 and 1): the defaults kept, 0.8 and past it the plasma walls and needles' pools read neon.
  Both are on PRISM's Path Tracing page, under Moving Lights (Brightness, Colour).
- **The pixelated green patch** of the rejected 4K shot at tick 255 (`work/openh1_eval/v4_shot2_runs/final4k/
  crop255.jpg`): not a decal, contrail, sprite or clamp, and not Path Traced's. It is Classic's enhanced dynamic
  light (display.dynamic_lights "enhanced") on b30's floor under a plasma pistol's bolt, drawn in blocks of 2 to 8
  pixels that change from frame to frame with the game stopped; Path Traced showed it through CE's base. Found
  again (`work/openh1_eval/v4_glow2_runs`, the setup4 fight's plan `plans/t255.txt`, Classic, the same frames on
  each build, both cards): blocky at ed37aba9a (`prism-v4-pt-glow`'s stage) and at 6eafe5a1f (`prism-v4-int2`,
  before OpenH1/goath1 was merged), smooth at f30b1207d (that merge), 8ec6a3a08 (`prism-v4-int3`, the demo's) and
  bc960ae79, in Classic and in Path Traced alike (`look/t_bis.png`, `t_old.png`, `t_int3.png`, `t_pt.png`): the
  light whole, as Classic means it. The fix came with goath1's renderer changes merged at f30b1207d (#1154's
  toggles HALO_VULKAN_DRAW_BOUNDS, _ALPHA_TEST_DROP, _NARROW_BARRIERS and _PASS_TRANSFERS all 0 leave it smooth,
  so not those; the likeliest are the draws' inputs kept whole for their pipelines' keys, 127537cc3 and 596760a1a:
  a draw built with a stale input's pipeline); not narrowed to one commit. Nothing changed for it on this branch, so
  Classic stays byte-identical.
- **Where it is, when it is.** The table is sent again after each window places its lights
  (`prism_scene_lights_window`, after `lights_preprocess_scene`): the path tracer lit with the lights the rasterizer
  took the frame before, a fast bolt's light a frame behind the bolt.
- **Room.** Past the job's 64, the 64 that light the most about the eye are kept (`dynamic_lights_rank`: brightest
  channel x reach^2 over the distance^2 past the reach), in the table's order. What enters or leaves the kept set
  from frame to frame is the faintest about the eye (tools/test_prism_pt.py walks an eye through 90 lights: each
  light that pops lights under a quarter of the brightest kept one, at most 3 a frame); there is no hysteresis.
- **Cost.** The temporal pass's history cut evaluated every light and recent light at every pixel (1.8 ms in a fight
  at 1080p); each light is now tested once a subgroup against the box of its points, and only those reaching it are
  evaluated (`dynamic_lights_luma`; the subgroups' ballot and arithmetic, the path tracer's own extensions).
- **As measured** (`work/openh1_eval/v4_glow_runs`, the demo's settings and bakes; RTX 3070 Ti for every frame):
  the setup4 fight (`v4_shot2_runs`' Marines against the Covenant at b30's security building, deterministic) before
  (40f2b1f95) and after: the job's dynamic lights at most 16 before, 40 after; the weapons' glows, the bolts with
  "classic" dynamic lights too, the strikes' flashes light the floor, walls and bodies with traced shadows, the bolts'
  pools on the floor smaller and no longer white (`evidence/fight_ab.png`); the corridor's weapons and grenades
  (`evidence/corr_*`); the flashlight by the controller's button in the demo's setup lights the wall
  (`evidence/flashlight_button_after.png`, `flashlight_ab.png`). Frame time at 1080p, quality, the fight's 420 live
  frames (`jobs/time_fight.sh`): 18.71 ms with the glows, 18.25 with display.prism_glow_lights 0 (+0.46 ms), 20.20
  before (the temporal pass's history cut 3.86 ms there, 2.30 now). Classic and the defaults byte-identical to
  40f2b1f95 (identity.py frames, b30: `identity/`).
- **As measured again, with the look** (`v4_glow_runs`, second round): the fight (`evidence/fight_ab_r2.png`:
  40f2b1f95, c246880f5, now), the dark corridor with each thing placed in it and caught in flight with the game
  slowed (`tools/glowcorr.py`, `evidence/corridor2_ab.png`: plasma rifle, pistol and overcharged bolts, a needle, a
  plasma grenade, a Banshee bolt, a fuel rod, the sword and pistol lying on the floor), the flashlight off, on and off
  by the script's command (`evidence/flashlight_offon_ab.png`). RTX 3070 Ti, 1080p, the fight's live frames' mean:
  18.70 ms, 18.25 with display.prism_glow_lights 0 (+0.45 ms), 20.27 at 40f2b1f95. Classic and the defaults
  byte-identical to 40f2b1f95 (`identity_r2/`). The owner's two shots again at 3840x2160 (`shot_best.jpg`,
  `shot_alt1.jpg`): live frames held 300 frames, not converged references.
- **As measured, third round** (`work/openh1_eval/v4_glow2_runs`, `prism-v4-pt-glow2`, the demo's config, data and
  bakes, every A/B on the RTX 2060: the RTX 3070 Ti's driver refused new devices that day). The dark corridor with
  each thing placed in it (`tools/glow2.py`, `evidence/corridor_ab.png`, bc960ae79 against this branch): plasma
  rifle and Banshee bolts now light the floor and wall cyan as they cross; needles light their surroundings magenta
  (none before); the Chief's shield flared by a Banshee's fuel rod lights a pool at his feet, not the corridor end
  to end; a fuel rod's wall lit by its distance. Grenades thrown (`tools/corridor.py`, `evidence/grenade_ab.png`):
  the blast lights its surroundings by their distance, not the far wall whole. The setup4 fight at tick 260
  (`evidence/fight_ab.png`, the defaults and saturation 0.8): the needles' pools pink, the plasma wall's green held
  near its bolts. The job's dynamic lights in the fight at most 51 (40 before: the needles), within its 64. Frame
  time, 1080p, quality, the fight's live frames, RTX 2060 (`jobs/time_*.out`): 75.50 ms median, 75.18 before
  (+0.3 ms). Classic byte-identical to bc960ae79 (G2's moves, 118 of 120 frames alike in two runs; frames 4 and 5,
  the Pelican's exhaust, take one of two values in either build from run to run, as `prism-v4-int3` found).

## The radiance cache

A world-space hash grid of the diffuse bounce's light (the idea of a spatially hashed radiance cache: Binder et al.,
"Massively Parallel Path Space Filtering", 2019, and NVIDIA's SHaRC; this implementation is our own, no SDK code).
`xgpu_vulkan_pt.glsl`'s `cache_key`, `cache_add`, `cache_get` and the pass `cache`; `xgpu_vulkan_pt.c` makes its
cells once (2^19 cells of 12 words, 24 MB) and empties them for each bake's scene generation.

- **A cell** is a cube of the world and one of six normal directions (the hit normal's largest axis and its sign).
  Its side is 0.03 world units (`HALO_PT_CACHE_CELL`) by the power of two at or under the hit's distance from the eye,
  so a far hit's cell is larger, as a pixel's footprint there is. Its key (a hash of the cube, its level and its
  direction) finds it by open addressing (8 places).
- **The update.** Every fourth pixel's diffuse bounce adds its hit's light as traced (the bake's light at the hit,
  the moving objects' by the volume and the sun's ray: the bounce estimator's own value) into the hit's cell, by
  atomics. The `cache` pass then folds the frame's sums into each cell's running mean over at most 256 samples
  (`HALO_PT_CACHE_MOST`), and lets go of a cell no frame has updated for 600 frames. The cache therefore starts from
  the bake and follows whatever the live hits see.
- **The read.** The diffuse bounce reads its hit's cell where the cell holds 8 samples (`HALO_PT_CACHE_LEAST`), in
  place of its own hit's light; ReSTIR GI then reuses that sample as before. The reflection's GGX ray does not read it
  (a reflection needs its hit's detail), nor does the reference.
- **What it does to the light.** A bounce meeting a small bright patch (a lamp's glow, the pool under a small lamp)
  takes the cell's mean over its cube instead of the patch's own value: the same light spread over the cube, so the
  pixels about it keep their expected light where the cube is small beside the distance, and lose the sharp detail of
  the light at the bounce's hit (never the pixel's own surface).
- **What it measured (round 6), and why it is off.** At G4 (b30's interior, the RTX 2060's gate) the frames with and
  without it differ by 0.01 to 0.06 of a level in 255 and the gate's figures are the same to the fourth place; it costs
  about 0.6 ms of the RTX 3070 Ti's light pass at 1080p. Here the path already ends at its first hit in the bake,
  whose texels are converged: the cache's filtering of the hit's light changes almost nothing, and the bounce's noise
  is its direction's, which a cache at the hit cannot take away. The interiors' remaining speckle is in the specular
  light (G4's `pt_specular` view: the small lamps' highlights on the semi-glossy walls), not in the diffuse bounce
  (`pt_indirect` is smooth). A cache earns its place where a path goes on past its first hit (a longer path,
  the objects' bounce, the transparent layer's) or where the bake is missing; it stays in, off.
- `HALO_PT_CACHE=1` turns it on (`0`, the default, off).

**The indirect light's clamp** (`HALO_PT_INDIRECT_CLAMP=<luminance>`, off by default): a bounce's light past that
luminance (the bake's unit, a white surface's radiance) is scaled down to it, its hue kept. It takes energy away
wherever a bounce really meets a bright patch (not measured against R yet: the bounce is not where the interiors' speckle is, see above). It is not on
by default.

## The denoiser

The light's noisy parts (the domes, lamps, fixtures and dynamic lights, the traced bounce, the traced reflections) go
through the denoiser; the rest (the floor, the bake's indirect layer at the pixel, the probes' reflections) is added
after it. The specular is denoised as the light the lobe takes in (divided by its split sum's share, put back by
composite). `display.denoiser` `"off"` shows the light as traced (one sample a pixel). The passes, written from the
published methods (ReLAX's and SVGF's; no NVIDIA SDK code):

- **pre-blur** (ReLAX's pre-pass, from its published descriptions; the fix pass's first dispatch): the light as traced
  spread over a Vogel disc of 12 points within 6 pixels (the specular's radius by `smoothstep(0.1, 0.6, roughness)`,
  none on a mirror), turned each frame and pixel, each tap weighed by its plane's distance, its normal and the same
  instance only, with no luminance stop, so the light's energy is kept and a lucky sample is shared with its
  neighbours rather than shown and carried by the history alone (round 3's moving interiors: glitter, and radial
  streaks where the history dragged such samples along the move). The temporal pass takes these as the frame's samples.
  `HALO_PT_RAY_ROUGHNESS` (a diagnosis) sets the roughness under which the reflection is traced (0.35; 1 everywhere).
  Off by default: the temporal pass then reads the light as traced itself (until round 7 a pass of its own copied it
  for it).
- **temporal**: each pixel's last-frame histories found where its surface was: a level pixel's point projected into
  the last frame's camera (the level stands still: its geometry, not a motion vector), an object's along the
  G-buffer's motion; each of the 2 × 2 taps kept only
  where its guide (the last frame's: instance, normal, distance) is this surface's: the same instance, the normal
  within 25 degrees, and on the level its point on this pixel's plane (the last frame's camera; an object's distance
  within 5%). So no history is taken across an edge, nor through an opening onto the surface behind it (a10's door
  windows). Where the whole 4 × 4 about the place passes that test, the diffuse and specular light are resampled by
  Catmull-Rom, held to the range of the 2 × 2 taps (bilinear taps at every move blurred the history along the move:
  round 2's moving G4 floor streaked a lamp's sparse samples toward the point the eye moved to); the sun's visibility
  alike (G6's moonlit ground brighter than R in motion where the lit side's history bled over the shadows' edges); the
  moments and the lengths keep the bilinear taps. The specular's history on the level moves with the reflection where
  the reflection is traced (ReLAX's virtual motion, from its published descriptions): the light pass keeps the
  reflection ray's length in the guide (its `w`'s high half), and the temporal pass projects the reflection's image
  (the point along the eye's ray past the surface by the mean length over the pixel's 3 × 3 on the same surface:
  exact for a plane mirror, `tools/test_prism_pt.py`) into the last camera and takes the specular history and its
  moments there (2 × 2 taps, each this surface's), mixed with the surface's by `1 − smoothstep(0.1, 0.45, roughness)`;
  where the image's place holds no history of this surface, the specular's history is shortened in that proportion.
  Round 3's glossy floors in motion streaked toward the point the eye moved to: their history went with the surface.
  **The specular hold** (round 8, `HALO_PT_SPEC_HOLD`, on by default): while the eye moves, a glossy surface's
  (roughness under 0.6) specular history is held no brighter than the brightest of this frame's samples over the
  pixel's 3 × 3 on the same surface, as a TAA's clamp: a glint the history carries where nothing about it reflects one
  now is a reflection that has moved on, and the reprojection's taps smear it into a radial streak along a forward move
  (round 7's G4). A lamp's highlight that is still there is among the frame's samples (ReSTIR finds the lamp most
  frames) and is kept; a still eye is never held. It costs nothing measurable (the temporal pass 1.81 ms either way).
  Held to roughness under 0.6 rather than on every surface (measured the same within the runs' spread, below): a rough
  surface's history moves with the surface and is never smeared that way. With the hold the gate's moving frames
  are no longer exactly repeatable from run to run (whether a frame's camera moved depends on the frame pacing): two
  runs of the same build gave G4 0.1508 / 0.0745 / 0.115 / 1.30 and 0.1546 / 0.0725 / 0.116 / 1.30.
  A slow history (up to 32 frames; the specular's shorter on glossy surfaces where the eye moves: its length at least
  the reflection lobe's width (twice alpha, the roughness squared) over the parallax, the angle the eye turned about the point
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
  it; round 1's 4 / n times it let the a-trous steps blur a moving camera's noise into smears). The specular's
  variance is its 3 × 3 neighbourhood's spread as it is (round 3 divided it again by the history's length, though the
  history is already a mean, so its stops were a fifth as wide as the noise left and the semi-glossy walls' lamp pools
  stayed grainy held still).
  Then the histories' anti-firefly (`HALO_PT_HIST_FIREFLY`, 1.5 by default; 0 none; the clarity round's third part,
  ReLAX's history clamping's): each pixel's slow diffuse and specular history, as the temporal pass left it, held to
  the mean and 1.5 standard deviations of its neighbours' on the same surface (a 5 × 5 at a stride of 2, itself left
  out; at least 6 of them, else a thin thing is left alone), the held value written back into the history and its
  moments, and the a-trous steps' variance there the neighbours'. The cause it removes: a bright sample taken while
  the history is short (a moving eye's, a disocclusion's; the short history's cap let samples up to 16 times white
  through, 4 times since this round) enters the slow mean at 1 / its length and the moments with it, so the fast
  history's hold, whose width comes from those moments, never caught what was left of it (its share falls as 1 / n,
  the hold's width as 1 / √n): one sample of a lamp stayed a glow for a hundred frames after the eye stopped (the
  128-frame still history), and the a-trous steps, their luminance stops opened by the same moments, spread a few such
  pixels into a splotch as wide as their reach (c40's lab, its hologram glass and its floors; worse on the light's
  grid, `display.pt_light_scale`, where the reach covers more of the screen). Over the 240 moving frames of c40's lab
  the bright pixels the native frame does not have fell 1.8 times at DLSS 0.33, 2.8 times at DLSS 0.67 with the light
  at 0.33 of the screen and 7.7 times at DLAA with the light at 0.5, and glows that stay 8 frames in one place to
  none; against the supersampled references every pose's FLIP and edge SSIM are better and its ghosting less (a10's
  corridor native: FLIP 0.087 to 0.069, ghosting 17.5 to 11.6; c40's lab 0.136 to 0.113), b30's beach unchanged
  (`work/openh1_eval/v4_pt_clarity_runs/r3`). What it costs: a small highlight on a dark, even surface (the lamps'
  pools on c40's glossy floor) has neighbours with no spread to bound it by, so it is held near them. Native, that
  is about 1% of the brightest pixels' level; with `display.pt_clarity` `"auto"` at DLSS 0.33, where the light is
  traced at a third of the screen and such a pool is a pixel or two of it, the 200 brightest pixels about the
  right-hand lamps are 15% dimmer than with the bound off (198 against 233 of 255; the reference 243). A separate,
  looser bound for the specular history, a bound only while the history is short, one waived for a pixel whose own
  moments are steady, and a floor of up to 2 above the neighbours' mean were tried: none gave the pools back without
  bringing glows back at that setting (pixels lit 8 frames in one place over c40's 240-frame sweep: 0 with the
  bound, 48 to 89 with the floor or the waiver, 599 with no bound), so the bound stays as it is
  (`work/openh1_eval/v4_pt_clarity_runs/r4`).
- **atrous**: the first step holds a pixel brighter than twice the brightest of its eight neighbours on the same
  surface to that (ReLAX's anti-firefly), for the frame shown only: the histories keep the light whole, and a sample
  of a light few rays find adds its share to them without showing as a single bright pixel (round 3's G7 walls).
  Then four steps of the a-trous filter (a 3 × 3 kernel, 1/4 1/2 1/4 each way) at strides 1, 2, 4 and 16
  (`HALO_PT_ATROUS_WIDE`, since round 7: five steps' reach, the reach of SVGF's five 5 × 5 steps with a third of their
  taps, for four steps' time; after the first three the light is smooth over about 8 pixels, so the last step's taps
  16 apart do not alias; `HALO_PT_ATROUS_STEPS` 1 to 5, the plain strides 1 to 2^(n-1) with `HALO_PT_ATROUS_WIDE=0`;
  at G4 four plain steps raised the blotch 0.0808 to 0.0857 against five, the wide four 0.0822), each tap weighed by its
  plane's distance, its normal, the same instance (never across an object's or the level's edge) and its luminance
  against the pixel's by the variance (the diffuse's prefiltered over the pixel's cross, as SVGF prefilters it); the
  specular's kernel narrowed by the roughness and stopped after three steps (none on a mirror).
- **stabilize** (the dark interiors' round, `HALO_PT_STABILIZE`, 0 (none) by default since the second round, 8
  frames as measured here; ReBLUR's temporal
  stabilisation, written from Zhdan's published talks): after the a-trous steps the filtered diffuse and specular
  light as shown keep a history of their own (`stab_*`, by parity), taken from the last frame where the guide finds
  the surface (the temporal pass's test, bilinear over the kept taps), held to the filtered frame's 3 × 3 on the same
  surface (its mean ± 1.5 standard deviations, by channel), blended by 1 / its frames, at most 8 and never more than
  the light's own history (so every reset, the dynamic lights' cut and the fast history's hold shorten it too: the
  flashlight comes and goes as fast as before, `v4_dark_interiors_runs`' flash runs). The composite reads it
  (`parameters.y` 1). The light's histories are not touched. It lowered the moving frames' flicker at most dark poses
  (median 1.19 to 1.13 of Classic's at 720p); at 16 frames a10's door windows lagged (its moving error 0.101 to
  0.121), at 8 less (0.114). Off by default since the second round: alone it raised b30_g4k2's moving error 1.22
  times (the first round's 1080p regression there, 1.25 times, was its), its cost is unmeasured, and the tree's
  candidates took most of the flicker it was for.
  Tried in the second round and not kept (720p, the dark poses): ReSTIR DI's permutation (still error 1.3 to 2.8
  times), the boiling filter at 0.2 (darker, c10's lab still error 1.9 times), the pre-blur (`HALO_PT_PREBLUR=1`) and
  a pre-blur of only each sample's excess over its neighbourhood: both turned the sparkles into larger glitter the
  history kept (c10's lab still blotch 1.45 to 1.6 times); 8 grid candidates (c10's lab worse 1.65 times); ReSTIR DI
  without its spatial reuse (worse everywhere). The still eye's 128-frame history and confidence of 20 were checked
  against motion in front of a still camera (the player strafing and firing through the view, `stillfx` runs): the
  trail once the motion stops was shorter than prism-9's (its summed difference to the settled frame 172 against
  208 at b30, 221 against 276 at c10's lab), the muzzle flashes lit and gone as fast.
  **The histories' lengths**: the slow history is at most 32 frames while the eye moves (`HALO_PT_HISTORY`) and 128
  for a still eye (`HALO_PT_HISTORY_STILL`: the camera exactly the last frame's), and ReSTIR DI's reservoirs'
  confidence 20 frames for a still eye (`HALO_PT_RESTIR_M_STILL`, 8 moving): at 32 a still view never passed 32
  samples of a light few rays find (c10's lab still error 0.054 to 0.035 against R); 64 for a moving eye lagged
  (a10's and c10's moving error 15 to 25% up) and was left at 32. A rotated Poisson disc in place of the stride-16
  step (ReBLUR's blur, its radius by the history's length) and spatiotemporal blue noise (an in-tree
  void-and-cluster mask, animated by the golden ratio) were tried in that round and not kept: the disc spread the
  remaining sparkles into round blobs in motion (moving blotch up 15 to 55%), and the blue noise changed nothing
  measurable at the nine poses.
- **the sun's visibility**: its own slow and fast history, held to the range the frame's own visibility spans over the
  pixel's 5 × 5 on the same surface (a lit pixel among lit neighbours is lit, whatever the reprojection's bilinear taps
  brought from a shadow beside it: round 1's G2 lost up to 8% of its foreground's light over its first leg's forward
  move, measured block by block against the light as traced; in a penumbra the 25 samples span 0 to 1 and the history
  accumulates; where the slow and the fast history already equal this frame's visibility, an interior's 0 or open
  lit ground's 1, the range cannot move them and the 5 × 5 is not read: round 7, half a millisecond of the RTX
  3070 Ti's temporal pass at G4), the fix's gather, and the a-trous steps only at a pixel
  whose visibility is neither all lit nor all dark (above 0.02, below 0.98, with a variance) and only as wide as the
  penumbra; a lit pixel is never filtered, so a shadow beside it cannot darken it (in G2's moves of round 1, before
  this rule, the foreground's sunlight lost up to a third where the variance of a moving camera's short histories
  let the filter spread the rocks' shadows) (the blocker's distance by the disc's
  angle over the pixel's footprint). In "performance" pixels past 60 world units read the baked mask, which has no noise. The
  FidelityFX Shadows denoiser was not vendored: this filter does its work here (its tile classification, temporal
  accumulation and penumbra-sized spatial passes) in the same frame as the other signals.

The denoise step has one call site in `xgpu_vulkan_pt_frame`: the plugins' denoise and denoise-and-upscale entry points
(`prism_plugin.h`) go there.

Under an upscaler (FSR 3.1, DLSS, DLAA, Ray Reconstruction) the G-buffer is drawn jittered: every draw moved by the
sequence's sub-pixel offset (`xgpu_vulkan_set_jitter`). The tracer finds each pixel's point again from its distance
along the pixel's ray (`surface_at`, the reprojection, the filters' neighbours), so the job's two cameras are turned by
their frames' jitter (`pt_camera_jitter`, `xgpu_vulkan_upscale_jitter`): the ray of a pixel's centre passes where the
G-buffer drew it. Before, the point was off its surface by up to half a pixel's footprint, by a different amount on
each of the sequence's eight frames: grazing sand and rock faces went in and out of their own shadow (the rays' offset
is a centimetre or two) and b30's beach flickered with a still camera, 27 levels of frame-to-frame change on its rocks
where the same frames without an upscaler had 0.1 (work/openh1_eval/v4_rock_stable_runs: 0.2 after).

A level pixel's rays start on the level as traced (`level_origin`). The G-buffer draws each remodelled surface at the
level its distance picks (`prism_world_geometry.c`: the near level, the far one past its distance), while the traced
scene holds the bake's level (the near one, two-sided where a far one exists). Where the two part, a far surface drawn
under the near one's relief, the pixel's point lay a few centimetres under a traced face and every ray from it (the
sun, the domes, the bounce) met that face's back at once: b30's wet shoreline and G2's landing beach went black, and
with the jitter fixed they stayed black (the sun's blocker under half a unit over the band, `debug.gbuffer_view`
`pt_sun`). So the pixel's ray is followed again over a short stretch about its point (as far as `LEVEL_GAP`, 0.15
units of height, at its slant, at most 4 units), and the rays start at the traced face it meets there, off it by that
face's normal; nothing met, at the point as drawn. The stretch is the eye's own ray, drawn through to the point, so no
surface the pixel could have shown lies on it. G2's landing beach went from 91 to 143 (Rebaked 148, the stock world
141) and the first beach's shoreline band from 123 to 187 (Rebaked 193), 8-bit luminance over the far dollies; the
flicker figures stayed at the jitter fix's (still 0.22, cliffs 0.31 / 0.72 / 1.62). The domes' rays (`domes_objects_at`,
the bake's estimator with the moving objects' rays added within 40 units) are stopped by the moving objects too, as the
bounce's and the sun's already were. Open: the remodelled boulders, captured among the moving objects (their pieces in
the objects' structure each frame), still cast almost no sun shadow on the sand (`HALO_PT_OBJECTS=0` changes the
direct light on 1 to 2% of the pixels; the stock rock in the base build's static scenery cast them):
work/openh1_eval/v4_rock_stable_runs/HANDOVER.md.

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
| `display.pt_light_scale` | `0`, `0.25` to `1` | The traced light's upscaling: the light pass and the denoiser at that share of the G-buffer's pixels each way, the denoised light brought to the G-buffer's pixels by its depth and normals (the pass `upsample`), each pixel lit with its own albedo, normal and emission; `0` every pixel. With `display.upscaler` at 1.0 or 0.67 |
| `display.pt_clarity` | `"auto"`, `"max"`, `"off"` | Under an upscaler with this denoiser: `"auto"` the G-buffer at 0.5 each way at least, the light at `display.upscaler_scale`'s share of the screen below it; `"max"` the G-buffer at every pixel, the light at half; `"off"` the G-buffer at `display.upscaler_scale` (`prism_upscale.c`). The default `"auto"` costs a config at DLSS 0.33 (the play kit's) about 0.9 ms more on an RTX 3070 Ti and 1.6 on an RTX 2060 (b30's beach, 1080p), with no menu row: Settings still shows 0.33 |
| `display.denoiser` | `"prism"`, `"dlss_rr"`, `"off"` | The in-tree denoiser; NVIDIA DLSS Ray Reconstruction (the plugin `dlss`, built by the player: `docs/prism_plugin.md`, "PRISM Path Traced and a plugin"), which takes the traced light raw with the tracer's guides and replaces the denoiser and the upscaler; or the light as traced |
| `debug.gbuffer_view` | `"pt_diffuse"`, `"pt_specular"`, `"pt_sun"`, `"pt_direct"`, `"pt_indirect"`, `"pt_history"`, `"pt_noisy"` | The diffuse light (denoised), the specular, the sun's visibility, the direct or indirect light alone (each denoised), the history's length, the diffuse light as traced |
| `debug.pt_reference` | frames | The reference, at most that many frames |

Settings > Video > PRISM > Lighting > PRISM Path Traced; PRISM's page then opens Path Tracing › (Profile, Denoiser,
and the moving lights: Dynamic Lights, Brightness, `display.prism_dynamic_lights`: Off 0, Faint 0.25, Soft 0.4, Natural
0.6, Full 1, and Colour, `display.prism_light_saturation`: Pale 0.4, Muted 0.6, Rich 0.8, Pure 1; "What glows", the
look), Image › (Exposure, Exposure Bias, Tone Curve, Bloom, Tone Grade), Upscaling › (Upscaler, Upscale Quality), Light
Bake › (this mode reads the bake) and Materials & Textures › (Material Packs, PBR Materials); Ray Tracing shows the traced scene's rows
(Traced Models, Keep Shapes, Cutout Shadows). None of `display.prism_rt`'s settings reaches this mode
(docs/prism_settings.md).

`HALO_PT_REFERENCE_LIVE=1` (a diagnosis) makes `debug.pt_reference` sum the light pass's own estimate in R's place
(its light as traced, ReSTIR's reservoirs reused from frame to frame, no denoiser) into the same EXR: its mean against
R's says what the live estimator holds apart from what the denoiser does.

`HALO_PT_TIMES_FILE` writes each frame's GPU time and its passes' (light, temporal, fix, atrous, composite), as the log
does their means every 600 frames. `HALO_PT_PARTS_OFF=<mask>` leaves parts of the light pass out (1 the sun's ray, 2
the domes, 4 ReSTIR DI's spatial reuse, 8 the bounce, 16 the reflection's ray, 32 ReSTIR GI's reuse, 64 the spatial
neighbours' traced Z), and `HALO_PT_PARTS_CYCLE=<frames>` leaves each out alone in turn, none first, so that one run's
timestamps give each part's cost at one pose under the same conditions (`v4_reuse_runs/tools/parts.py`): the RTX 3070
Ti's frame times vary by half a millisecond from run to run, more than most single changes.

## Frame time

The target is G4 (b30's interior, many lamps) at 1080p quality under 16.7 ms median on the RTX 3070 Ti
(`time_3070.sh`: three poses, 600 frames each, three times). Its light pass's parts, measured with one spatial
neighbour (`HALO_PT_PARTS_CYCLE`, round 7): of 11.96 ms, the bounce 2.73, ReSTIR DI's spatial reuse 2.46 (its Z rays
0.93 of it; a second neighbour 1.9 more), the reflection's ray 2.01, the sun's ray 0.85, the domes 0.53, ReSTIR GI's
reuse 0.45.

The light pass's options that are off cost nothing: they are specialization constants of the passes' pipelines
(`SPEC_*` in the shader, `spec_options` on the host, read from the same variables), so their code (the radiance
cache, the indirect clamp, the tree's emitter cones, the spatial neighbours, the parts diagnosis, the boiling filter,
the half bounce) is not in the pass when off, nor its registers.

**The sun's ray left out in the bake's dark tiles** (`HALO_PT_SUN_TILES`, on by default since round 7): the bake's
mask marks a tile of 16 × 16 texels whose samples all missed the sun (state 1); there the ray (the same structure, the
same length) would miss it too, and a moving object only takes light away. In an interior that is nearly every pixel
facing the sun.

**The bounce from the bake on half the pixels** (`HALO_PT_BOUNCE_HALF`: 1 half the level's 8 × 8 blocks, 2 half its
pixels, a checkerboard turned each frame; 0, the default): a pixel with a texel takes the bake's indirect light at the
pixel as its sample of the bounce, through the denoiser like a traced one. The two estimate the same light (the traced
bounce's hit is lit from the same bake). Measured and left off: in the runs made side by side the traced bounce
everywhere was as fast (G4 16.14 against 16.13 ms by blocks, 15.87 by pixels, within the runs' spread), and at G5
it did not help (error 0.1539 by blocks, 0.1522 with the bounce traced).

Same-session timings (G4, RTX 3070 Ti, median / p99 ms; the passes light, temporal, a-trous):

| stage | frame | passes |
| --- | --- | --- |
| round 6's defaults (`v4_m3_runs/stage_demo`) | 16.80, 16.80, 16.86 / about 18.5 | 10.2, 2.40, 3.07 |
| round 7's defaults: 1 spatial neighbour, 4 candidates, the 4 wide a-trous steps, the sun's dark tiles, GI reuse off | **16.19, 16.05, 16.14** / about 17.6 | 10.6, 1.80, 2.51 |
| the same with ReSTIR GI's reuse on | 16.54, 16.68 / about 18.3 | 11.0, 1.84, 2.51 |

Round 8 (`v4_reuse_runs/time_r11_*`, made side by side the same morning): round 6's defaults 16.72 / 18.4 (the
passes 10.1, 2.40, 3.04); round 8's defaults (round 7's with the specular hold) **16.14 and 16.14** / 17.4 and 17.3
(10.6, 1.83, 2.51); the hold off 16.10. The hold costs nothing measurable; 6 candidates 16.98, 8 candidates 17.69.

The RTX 2060 (`time_r10_2060_*`, `time_r11_2060_q`; the same G4 check, 1080p quality): round 6's defaults 60.3 ms,
round 7's 58.5, round 8's 58.5 (the light pass 47.4 of it). The 16.7 ms target is the RTX 3070 Ti's; the 2060
needs `performance` or a lower resolution for 60 frames a second, and its gain from round 7's work (1.8 ms) is in
the same direction as the 3070 Ti's. The RX 7900 XTX (Adam's Windows PC) is not measured here: that needs the branch
pushed for the Windows build, which is his call.

The same card measured round 6's defaults at 17.32 and 18.27 on other days: compare only runs made side by side. Of
round 7's 0.7 ms against round 6's defaults with the spatial reuse's 2.46 ms added: the temporal pass 0.6 (no copy
pass, the visibility's range), the a-trous steps 0.55 (four wide), the sun's dark tiles about 0.6, ReSTIR GI's reuse
0.45, 4 candidates in place of 8, and the specialization constants.

No regression outdoors (the RTX 2060's gates, error / blotch / FLIP / flicker, round 6's defaults then round 7's; G2's
and G7's R are the pre-materials ones, so read them side by side only): G2 0.0016 / 0.0072 / 0.073 / 1.13 then
0.0015 / 0.0071 / 0.068 / 1.13; G7 0.0269 / 0.0370 / 0.059 / 1.29 then 0.0258 / 0.0361 / 0.057 / 1.29. Classic stays
byte-identical on b30 (46 of 46 frames, `v4_reuse_runs/identity/`) and the simulation deterministic. Round 8's defaults
(the specular hold): G2 0.0015 / 0.0071 / 0.068 / 1.13 (unchanged), G7 0.0178 / 0.0296 / 0.054 / 1.28 (better: its
walls' glints); G5 0.1531 / 0.0781 / 0.132 / 0.99 (round 7's, the candidates' cost above); Classic byte-identical
on b30 again (46 of 46) and the simulation deterministic.

## The moving objects

The traced scene is the bake's (the level and its static scenery: placements of scenery, controls and light fixtures
that nothing moves or destroys, `prism_bake.c`'s `scenery_static`) plus this frame's moving objects (c7218cff2):

- **What is captured.** Every object draw of the G-buffer pass (`xgpu_vulkan_stream.c`'s `gbuffer_draw`) but the
  first-person view's (its weapon and arms would stand in front of the eye in the world's rays) and those of objects
  the bake's static scenery already holds (the game marks them `XGPU_SCENE_INSTANCE_IN_TLAS` in SCENE_INSTANCE,
  `prism_bake_object_static`: a placement of the static kinds at its place, by `scenery_static`'s rule). So the
  vehicles, the characters, weapons on the ground and in hands, the Pelican, projectiles with a model, the machines
  (doors and lifts, which the bake leaves out since prism-v4-m2 c50d3b3d3), equipment and scenery that can move or
  break are traced as drawn, in their pose and level of detail. So is static scenery a mod's mesh draws (b30's boulders): the frame's
  traced scene leaves its stock triangles out (`docs/prism_bake.md`, the static scenery), and past
  `display.prism_rt_objects`' reach a replacement drawn at a level of at most 4096 triangles is still traced as drawn,
  never by the game's model under it. A draw is passed over (counted) where its vertices are
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
  The structure is shared with PRISM Raster since `prism-v4-raster-int` was merged: under `display.lighting`
  `"raster"` with `display.prism_raster_object_shadows` `"traced"` RT_FRAME carries `XGPU_RT_FLAG_RASTER_OBJECTS`
  (32; it had 16 on its branch, XGPU_STREAM_VERSION 37) and the same capture and top level are built, which Raster's
  resolve (`objects_blocked`) traces for the moving objects' sun shadows through `xgpu_vulkan_rt_objects`. Both
  modes leave out the first-person view and the bake's static scenery (Path Traced by SCENE_INSTANCE's
  `IN_TLAS`, Raster by GBUFFER_DRAW's `XGPU_GBUFFER_FLAG_STATIC_SCENERY`; the capture takes either).
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
  plan).
- **The player's own body (prism-v4-body).** In first person the player's whole body, with its weapon, is drawn
  in the G-buffer pass with `XGPU_GBUFFER_FLAG_TRACE_ONLY` (`prism_deferred_body`, render_objects.c): captured
  into the objects' structure as instance mask 16 and never rasterized (its vertex constants put in the ring whole,
  `constants_whole`). The objects' rays take mask 28 (`object_cull`, set by `surface_at` a pixel), but the
  first-person view's pixels take 12 (their rays start inside or in front of the body). The legs
  (`display.first_person_legs`), drawn, are flagged out of the capture so the body is not traced twice. Raster's
  `objects_visibility` takes the same masks. `display.player_shadow` turns it off.

## The water

CE's water (the terrain overhaul's T7 layer, `docs/prism_terrain.md` "T7 and T8") is composited over the traced frame
at each water group's END. Its rays are this mode's: the pass `"water"` (T8) traces, at half the resolution (one ray
pair a 2x2 block with water), the water's reflection and
refraction (from below, Snell's window's ray and the total internal reflection) through the frame's scene with the
frame's job, each hit lit from the bake as the reflections' (`hit_light`), the sun's visibility by the sun's ray, and
writes them in light (the frame's encoding to 2.2), premultiplied by the block's water share, for the layer's
composite; with `debug.pt_reference` it sums the reference's paths
(`reference_path`) over the layer's filter footprint instead. Its GPU time is the debug.pass_times group `WATER`
(with the layer's composite).

## A lost device

Path Traced was made on NVIDIA's cards. Should the driver lose the device while it draws (its timeout on a pass that
does not end, a page fault), halo.log says where the GPU was: the last step recorded (each pass is named as it is
recorded, the traced objects' skinning and builds too), and where the device can say, the last step it began and the
last before which every command had finished (AMD's buffer markers) and the device's fault (`VK_EXT_device_fault`).
The message says the device was lost in PRISM Path Traced, and the next start sets `display.lighting` to `"raster"`
(PRISM Rebaked) and tells the player, once for that crash (report.c's `report_path_traced_fallback`); Settings can
choose Path Traced again. Every ray is checked before it is traced: a NaN or infinite origin or direction, or a NaN
reach, is a miss (undefined for ray queries, and a traversal loop that never ends on AMD's drivers), and a ray's
query tests at most 1024 candidates.

## What is not done yet

- **The interiors in motion** (round 7, above): small clusters of bright pixels about a lucky lamp sample where
  the spatial neighbours took it, and the specular history's radial streaks of the walls' glints along a forward move.
- **ReSTIR GI's spatial reuse** (its temporal reuse is in), the dynamic objects' occlusion of the baked light
  (`performance`'s short AO rays), traced-light upscaling and the async queue (M5, M6).
- **The transparent layer** (water and glass, traced reflection and refraction): after the seven gate scenes pass.
- **A lamp surface seen directly** shows its G-buffer emission (its self-illumination), not its radiosity's radiance.
