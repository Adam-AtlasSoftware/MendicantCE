# PRISM's quality gates

Every PRISM v4 milestone (`overhaul_research/prism_v4_renderer/design.md`, section 9) is judged on the same frozen
captures, against converged references, by fixed metrics. Nothing is judged by eye or against an unconverged
reference. This document describes the capture set, the harness that makes it, the references, the metrics, the
thresholds and the checks every milestone also passes. The tools are in `tools/prism_gate`; their tests, which need
no game data, are `tools/test_prism_gate.py`.

The captures and the references are made from the player's own maps and stay on the machine they were made on:
nothing under a capture folder is ever committed.

## The capture set

`tools/prism_gate/scenes.json` holds the scenes, each a map, a structure BSP where it is not the first, the camera's
keys (`x y z yaw pitch roll field-of-view`, world units and degrees) and what it tests:

| Id | Map | Tests |
| --- | --- | --- |
| G1 | ratrace | Lamp spots, many lamps |
| G2 | b30 (sunny beach) | Sun, sand, objects' shadows |
| G3 | b30 (shade) | Sky and dome light, occlusion |
| G4 | b30, BSP 1 (interior) | Many lamps |
| G5 | a10 | Door windows, specular trim, disocclusion |
| G6 | c40 (exterior) | Night sky energy |
| G7 | c40 (interior) | Lamp strips, hologram ring |
| S1 | a30 (the canyon at night) | Cliffs and rocks of the remodel, the canyon's fog |
| S2a | b30 (the shore) | Water |
| S2b | a10 (the cryo bay) | Glass (the cryo tubes) |
| S3 | a50, BSP 2 (the hangar) | The hangar's lamps (the flashlight: not yet) |

G1 to G7 are the path tracer's gate scenes, their keys as `prism-pt`'s gate checked them (inside the level, every
probe pose seeing surfaces). S1 to S3 are the design's stills, their poses from earlier PRISM captures, checked in
play for out-of-level and sky-only frames; the design's S2 is two scenes (b30's water, a10's glass). S4 and V1 and
V2 are listed in "Not yet frozen" below.

A scene is changed only with a note here saying why: every milestone's figures are measured on these.

### What a capture is

`capture.py` runs a staged tree (`tools/linux_package.py tree`) hidden, silent and isolated, and drives it with a
camera path (`debug.camera_path`): every pose and every console command at a known frame, so that every run of the
same build draws the same frames.

- **The run.** No window (SDL's dummy video driver, `debug.hidden_window`), no sound (`HALO_NO_AUDIO=1`,
  `SDL_AUDIODRIVER=dummy`), no controllers (`SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000`,
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`, `SDL_JOYSTICK_HIDAPI=0`), nothing online (the run's own empty brokers,
  relays and masters files), its own data, save and config folders, the clock fixed (`HALO_FIXED_STEP=1`: a tick a
  frame) at the game's own 30 ticks a second (`HALO_SIM_RATE=30`) and interpolation off (`HALO_INTERPOLATION=false`),
  MENDICANT AI's models off (`HALO_AI_MENDICANT=false`: their decisions differ from run to run). The data folder links the maps and PRISM's folders
  read only; the mods are linked one by one beside a copy of their load order. Every run takes the same
  `config.toml` (the kit's, with `display.prism_rt` off: the path turns it on).
- **The kit.** Before every run the harness looks for a process whose executable is in the kit (the game the person
  this works for plays) and stops if one runs.
- **The harness's rules.** The path starts at the game's tick 600 (`start_tick`). At its first frame: the
  console's lines and the HUD off, the player deathless (`cheat_deathless_player 1`), the weather off (`weather 0`),
  no particles, particle systems, weather systems or contrails drawn (they move frame by frame at a stopped clock,
  and are not the light's) and the AI erased (`ai_erase_all`), again every 60 ticks (a level's script places its AI
  as its opening ends). The game is stopped (`game_speed 0`) by its clock, 420 ticks into the path, the same tick in
  every run (a tick line: a path's frame need not take a step of the clock), and the camera is held at the first
  key for 900 frames in all. These let a level's opening play out (b30's insertion: the director takes the path's
  camera only once it has, and only in a tick), projectiles land and a hit's flash fade. As the path starts, the
  frames' own randomness (particles, a light's flicker) is set to a constant.
- **The frames.** Only the frames the path names (its `save` lines) are written to the stream
  (`debug.screenshot_stream`), each at the end of a hold of its pose (a pose set in a frame is drawn by the next).
  They are saved as PNG with the SHA-256 of their pixels in `manifest.json`.
- **moves.** In each state, RT off, path traced (`faithful`, the denoiser `spatiotemporal`) and raw (the denoiser
  off, a diagnostic), the camera held 150 frames at the first key, then moved along the keys 40 steps a leg, each
  step held 3 frames and its last saved: `moves/m_<state>_<step>.png`.
- **refs.** At the three probe poses (a quarter, a half and three quarters of the move), the camera still: RT off's
  frame after 150 frames (`refs/p<n>_off.png`); then path traced for 300 frames; then the reference
  (`debug.pt_reference 4096`), its file `refs/p<n>.exr` and the frame it holds (`refs/p<n>_ref.png`).

```sh
python3 tools/prism_gate/capture.py all G1 G2 --stage <tree> --data <data> --config <config.toml> --out <dir>
python3 tools/prism_gate/gate.py measure <dir>          # the table and <dir>/results.json
python3 tools/prism_gate/gate.py repro <dir a> <dir b>  # two runs' frames and references byte for byte
```

## The reference: `debug.pt_reference`

`debug.pt_reference N` (`prism_set "pt_reference 4096"`, with `display.prism_rt` `pathtraced`) traces the scene
unbiased and sums it over N frames at a stopped camera and game, then writes it once as a linear OpenEXR file
(`debug.pt_reference_file`) and holds the converged image on screen:

- every pixel traced every frame, one path a pixel;
- no reuse: ReSTIR DI's lamps and dynamic lights keep only the frame's own candidates (resampled importance over
  them is unbiased), no reservoir from the last frame or a neighbour;
- no cache: the radiance cache neither read nor written, the probes not read at a path's end;
- up to 8 bounces (`XGPU_RT_REFERENCE_BOUNCES`), Russian roulette past the second, a path ending in nothing past the
  last;
- no denoiser, no clamp: the samples summed in 32-bit floats (`xgpu_vulkan_rt_glsl.inc`'s reference pass, in the
  temporal pass's place) and their mean passed through;
- the same scene, materials, lights and style as the frames it is compared with: the style's own resolve (the
  faithful anchor, the exposure) still turns the converged light into the frame, so that R is the converged
  expectation of the estimator the frames are drawn with;
- seeded by the frame's number in the sum, with the exposure's clock a 60th of a second a frame: every run sums the
  same, byte for byte;
- started again where the camera moves (by more than a ten-thousandth of its place's size: a stopped camera moves in
  its last bits), the game's time moves (the game must be stopped with `game_speed 0`) or the level's triangles are
  built again.

The file's channels, each a 32-bit float per pixel: `light.R/G/B` (the light less the sun's, the rig's linear units),
`sun.V` (the sun's visibility), `dynamic.R/G/B` (the dynamic lights' change), `structure.V` (the sun's visibility
past the structure alone), `Z` (the distance; 0 where no surface: the sky) and `samples`; its header's
`prismReferenceFrames`, `prismReferenceBounces` and `prismReferenceGeneration`. Today's tracer lights CE's forward
pass rather than a frame of its own, so the linear image of today's references is the light; the frame drawn from it
is `p<n>_ref.png`. M1's deferred resolve adds the whole frame, linear, as a channel set of its own.

## Metrics

All on frames brought to 960 by 540 (`metrics.py`), at the three probe poses, the worst of them:

- **error**: the relative MSE of the path traced moving frame at the pose against R.
- **blotch**: the mean of the 4–24 px band (blur 2 less blur 12) of (moving − R), over R's mean: error in patches
  the size of a reservoir's or a filter's footprint.
- **flip**: mean FLIP against R (`flip.py`, LDR FLIP written from Andersson et al. 2020, 67 pixels a degree).
- **flicker**: the mean change from step to step of the moving path traced frames, over RT off's.
- **trail**: for a still camera as an object crosses it (V1), the frames its ghost lasts behind it
  (`metrics.ghost_trail`: the residual against the frame long after, where RT off shows the object left k frames
  ago).
- Left out of error, blotch and flip: where RT off's moving frame and its still frame at the pose differ (water's
  waves, a projectile, a body: what moved of itself is not the light's), widened by a blur of 4.
- A harness fault, not a pass or a fail: a reference whose `Z` shows no surface at more than 80% of its pixels (the
  pose outside the level, or the sky alone).

## Thresholds

`tools/prism_gate/thresholds.json`, fixed on M0's first measurements (the RTX 3070 Ti, `capture.py all`, the
references at 4096 frames; "Measurements" below):

| Measure | Threshold | Basis |
| --- | --- | --- |
| error | ≤ 0.01 | `prism-pt`'s, kept: G2 (sunlit) measures 0.0006, the scenes the path tracer fails today 0.02 to 0.07 |
| blotch | ≤ 0.015 | as error: G2 0.007, the failing scenes 0.03 to 0.06 |
| flicker | ≤ 1.30 | `prism-pt`'s, kept: every scene measures 0.95 to 1.10 |
| flip, still camera | ≤ 0.10 | G2 0.04; the failing scenes 0.05 to 0.19: FLIP passes some scenes error fails, so both gate |
| flip, V1 and V2 | ≤ 0.15 | *initial*: V1 and V2 are not frozen yet |
| trail | ≤ 4 frames | *initial*: V1 is not frozen yet |
| harness fault (sky) | > 0.8 of a reference's pixels | the frozen poses' references show no surface at 0 to 0.3 of their pixels |
| a reference's own noise | its frame's estimated relMSE ≤ 0.001 | a tenth of the error threshold (below) |
| the bake's noise (D4) | a texel's relative standard error ≤ 0.01 | below |

**A reference's own noise.** R is the gate's truth only where its own noise is well under what the gate measures.
Measured by summing the same reference over 1024 and over 4096 frames (`debug.pt_reference`'s first 1024 frames are
the same in both: the difference is the last 3072 frames' noise): at G2 (sunlit) R at 4096 frames has a median
relative standard error of 0.3% in its light (95th percentile 3%), its frame an estimated relMSE of 1e-5 to 4e-5;
at G4 (b30's interior, 700 lamps) a median of 27 to 38% (95th percentile 73 to 107%), its frame an estimated relMSE
of 0.005 to 0.012, as large as the error threshold. So 4096 frames are not a converged reference wherever lamps light
the scene: a reference sums until its estimated relMSE is under 0.001 (at G4, of the order of 50,000 frames with
today's lamp sampling), and today's G1, G4, G5 and G7 verdicts against R at 4096 frames are within R's own noise.

**The bake's noise (D4).** A texel's samples are added until the relative standard error of its mean luminance is
under 1% (at least 64 samples; a texel whose mean is under 1/1000 of the frame's middle grey stops there: black
stays black). No texel stops on a sample budget. At 1% a bake's noise adds an relMSE of about 1e-4 to a frame, a
hundredth of the gate's error threshold. Measured with today's tracer (one path a pixel, ReSTIR's candidates, no
reuse): a sunlit texel needs a median of about 400 samples (95th percentile 20,000 to 50,000), a lamp-lit interior's
millions. The baker therefore needs far better lamp sampling than today's (the design's light-importance grid with
baked visibility) for the `max` bake to finish.

## Reproducibility, as measured

Two full captures by the same tree (M0's; `gate.py repro`), G1 to G7 on the RTX 3070 Ti and S1 to S3 on the RTX
2060, each saved frame and reference file compared byte for byte:

| What | G1 to G7 alike | S1 to S3 alike |
| --- | --- | --- |
| RT off, the moves | 683 of 760 frames (G1, G2, G3, G4 and G6 all; G5 63 of 80; G7 60 of 120) | 240 of 240 |
| RT off, the still at each probe | 18 of 21 | 11 of 12 |
| The references' linear files (`p<n>.exr`) | 18 of 21 (all but G3's three) | 9 of 12 (all but S1's three) |
| The references' frames (`p<n>_ref.png`) | 15 of 21 | 7 of 12 |
| Path traced, the moves (live, denoised) | 90 of 760 | 61 of 240 |
| Raw, the moves (live, the denoiser off) | 106 of 760 | 80 of 240 |

What the harness fixed (each found by a capture differing from its twin, and checked by the determinism log):
commands at path frames, which fall in different ticks from run to run (now by the clock: the path's `start_tick`
and tick lines); particles and the frames' randomness; MENDICANT AI's models; c40's opening played with its AI.

What still differs, and why:

- **The PRISM asset streams.** Where a move uncovers a room, a10's (G5) RT-off frames show the stock texture cache's
  or the remodel's not-yet-read data for a few frames (blocks of a texture through a door window); c40's hologram
  ring (G7) animates by a clock of the frames drawn since the map loaded. `display.prism_prefetch`, no texture fade
  and the longest texture wait do not change it (a10: 45 to 48 of 80 frames alike).
- **The traced objects.** G3's and S1's references differ in the sun's visibility over regions of the ground: the
  objects in the acceleration structure differ (the replacement meshes are traced while resident and under the budget, both a
  matter of timing).
- **The live path tracer.** The radiance cache claims its cells with atomic compare-and-swap, in whatever order the
  GPU runs, and a transient as the mode turns on decays through the temporal history: without the cache (G1, Rat Race,
  on the RTX 2060), the moves part only in their first 24 steps.

Two fixes for the first of those are in (M1's determinism work):

- **The streams settled before a save** (`debug.camera_path_settle`; the captures' `PRISM_GATE_SETTLE`, 0 by
  default): each frame a path saves is drawn with every texture the stock texture cache has to read (as while a
  map's shaders are made), and held, its pose drawn again and nothing saved, until the host's texture and mesh stores
  have said on frames in flight + 1 frames after its first that nothing they drew is still to come (the ring's
  `streams_pending`, written at each frame's end), or for that many frames (logged). Each held frame is drawn with the
  frames' seed its first had, and runs no `at` line again. What the renderer keeps from frame to frame by its own
  count (a temporal history, a jitter) still moves on with each held frame, so a held capture is no longer byte for
  byte a build's before it (the stills of G2, G4 and G5 with the kit's settings then differ in their noise; with no
  hold they are alike). Measured (RT off's moves, G5 and G7, two runs of each tree, the RTX 2060, with a hold of up to
  300 frames): 44 of 80 and 59 of 120 frames alike, against 41 and 54 for the tree before. So the harness holds
  none by default.
- **The rendering's animations by the game's clock.** The frame's time the glows, the skies' animations and weather
  advance by is scaled by the game's speed, so they stand still while the game is stopped, as its shaders'
  animations already did; a glow's trailing particles are aged and made by the ticks since its last update, not the
  game's whole time on each draw, at any speed but 1; the skies' animations begin again with each map. At speed 1 (as
  games are played) nothing of it changes but that last.

What still parts G7's RT-off moves after both: the lamps' glows on the floor (a light's flare, its brightness by a
visibility test whose answer comes a frame or two later, by the GPU's timing, most likely), not the hologram ring. G7's still
(identity.py stills) differs from run to run the same way, in this tree and the one before it.

So the byte-for-byte check covers RT off and the references of the scenes above; the live path-traced frames are
judged by the metrics only, until M1's deferred core replaces the tracer's inputs with records it can make
deterministic (the materials resolved at load, the instances by their ids, the cache claimed in a fixed order).

## M1: the deferred core's equivalence

`display.lighting` `ce_deferred` is the design's "CE light, deferred" diagnostic (`port/linux/game/prism_deferred.c`):
CE's environment passes drawn into targets of their own (the light, the base colour, the specular and reflection
additions) and the frame resolved from them before the transparent geometry. `capture.py equivalence` holds the camera at each probe pose and saves Classic's
frame (`display.lighting` `ce`), the diagnostic's, and its targets alone (`debug.gbuffer_view`); `gate.py
equivalence` takes the mean FLIP of the diagnostic's frame against Classic's at each pose, the worst a scene's,
against `equivalence_flip` (0.05, the design's).

```sh
python3 tools/prism_gate/capture.py equivalence G2 --stage <tree> --data <data> --config <toml> --out <dir>
python3 tools/prism_gate/gate.py equivalence <dir>
```

The equivalence is taken with the Classic preset (`display.graphics` `classic`): it is what the resolve reproduces.
With the kit's settings (PRISM's forward extras) the same holds, as those extras draw inside CE's passes.

Measured on the RTX 3070 Ti (2026-11-13, M1's build, every frozen scene; the worst pose's mean FLIP):

| Scene | Classic preset | Kit's settings |
| --- | --- | --- |
| G1 ratrace | 0.0025 | |
| G2 b30 sunny | 0.0035 | 0.0046 |
| G3 b30 shade | 0.0024 | |
| G4 b30 interior | 0.0018 | 0.0011 |
| G5 a10 | 0.0020 | 0.0109 |
| G6 c40 exterior | 0.0027 | |
| G7 c40 interior | 0.0026 | |
| S1 a30 canyon | 0.0026 | 0.0033 |
| S2a b30 water | 0.0010 | |
| S2b a10 cryo glass | 0.0028 | |
| S3 a50 hangar | 0.0029 | 0.0021 |

With the Classic preset no pixel differs by more than 2 of 255 except at G2 (up to 55, on 0.015% of the pixels: the
decals, drawn over the base rather than the lit colour). Every scene is within 0.05.

### The G-buffer pass

Before CE's passes, `ce_deferred` draws the window's opaque surfaces once more, first and alone, into the G-buffer (the
design's walk 1; `xgpu_stream.h`'s `GBUFFER_BEGIN`): an object's base colour (its combiners with white light, in the
same draw as its geometry), the shading normal, roughness, metalness and specular, the material and its source tier,
and the instance. The level's base colour is the base target's (CE's diffuse texture pass). The map's materials are
resolved at the automatic tier as its first G-buffer pass begins (`SCENE_MATERIAL`, `docs/prism_scene_records.md`).
`debug.gbuffer_view` shows each (`albedo`, `normal`, `roughness`, `metal`, `material_source`, `instance`), and
`capture.py equivalence` saves them at each probe pose with Classic's and the deferred frame.

The equivalence above holds with the G-buffer pass in: the frame is drawn by the same passes, which the G-buffer pass
does not touch. Measured again on the RTX 3070 Ti (2026-11-13, M1's G-buffer build, the Classic preset), the worst pose's
mean FLIP is the same at every scene: G1 0.0025, G2 0.0035, G3 0.0024, G4 0.0018, G5 0.0020, G6 0.0027, G7 0.0026, S1
0.0026, S2a 0.0010, S2b 0.0028, S3 0.0029. With the kit's settings on the RTX 2060: G2 0.0046, G4 0.0011, G5 0.0087, S1
0.0039, S3 0.0021. With `ce`, the frames stay byte for byte M0's (`identity.py frames`, b30 and a10, 46 of 46 frames
each), and the simulation alike (`identity.py determinism`, a10).

### The material-source gate

`gate.py material_source` reads the `material_source` view at each pose: each opaque pixel (one the window drew: its
depth is not the cleared depth) coloured by its material's tier, green the automatic tier, blue a pack's, cyan a mod's,
yellow the class default, orange none, magenta a pixel the window drew that the G-buffer missed, red one the
G-buffer has that the window did not draw (the design's per-pixel comparison of the surfaces). The coverage is the
share of the opaque pixels at the automatic tier or better; the gate is 1.0 (`material_source_coverage`), every one.

```sh
python3 tools/prism_gate/gate.py material_source <dir>
```

Measured (2026-11-13, M1's G-buffer build): every frozen scene's three probe poses (G1 to G7, S1 to S3: ratrace,
b30, a10, c40, a30 and a50) and, for the other campaign maps, four headings at the player's start
(`debug_object_place "player pose"`: b40, c10, c20, d20 and d40): every one 1.000000, no pixel missed, extra or
without a material. That is the ten campaign maps and Rat Race. Every material there is at the automatic tier (none of
the shaders seen has a mod's material with the Classic preset).

### The G-buffer pass's time

The G-buffer pass is timed apart (`HALO_VULKAN_GPU_TIMES_FILE`'s third figure: the GPU's time from its begin to its
end), against the design's budget for it on the stock world: 2.5 ms on the RTX 3070 Ti at 1080p, 4.0 ms on the RTX 2060
at 720p. The medians of 600-frame blocks (the first 60 of each dropped) at the three probe poses, `ce` and `ce_deferred`
alternated three times in one run; the frame's GPU time beside it (`ce` against `ce_deferred`, which adds the G-buffer
pass, the base and additions targets and the resolve). "Classic" is the Classic preset (the stock world); "kit" the
kit's settings (the remodel, the replacement meshes and PRISM's forward extras), recorded, gated at M7.

| Scene | Settings | 3070 Ti 1080p: G-buffer pass median (p99), ms | frame `ce` / `ce_deferred`, ms | 2060 720p: G-buffer pass median (p99), ms | frame `ce` / `ce_deferred`, ms |
| --- | --- | --- | --- | --- | --- |
| G2 | classic | 0.222 (0.233) | 0.38 / 0.86 | 0.915 (0.961) | 1.12 / 2.99 |
| G2 | kit | 0.525 (1.427) | 5.84 / 6.84 | 1.620 (2.185) | 10.46 / 12.30 |
| G4 | classic | 0.165 (0.179) | 0.30 / 0.71 | 0.336 (0.340) | 0.57 / 1.70 |
| G4 | kit | 0.168 (0.189) | 1.92 / 2.53 | 0.364 (0.372) | 6.80 / 9.25 |
| G5 | classic | 0.170 (0.176) | 0.28 / 0.69 | 0.485 (0.492) | 0.73 / 1.91 |
| G5 | kit | 0.244 (0.251) | 2.64 / 3.38 | 0.814 (0.942) | 8.66 / 10.49 |

Every figure is well within its budget (the stock world's worst: 0.22 ms of 2.5 on the 3070 Ti, 0.92 ms of 4.0 on the 2060; with the remodel 0.53 and 1.62 ms). The 2060's figures are of the build before the normal's flag (`XGPU_GBUFFER_FLAG_NORMAL`), which adds one instruction to the twins' pixel shaders.

### The a30 rock fog shell (S1's bug class)

Inside a30's planar fog the game drew the fog screen's layer over each object in fog with its stock parts, so
a replacement mesh (the v3 rocks) showed a fog-coloured shell in the stock rock's shape. M1 draws that layer with the
part each queued group's own draw named (`rasterizer_xbox_environment_fog.c`, `mod_meshes_part_name_again`). Found again
by listing a30's rocks (`debug_object_place "list rock"`) and holding the camera inside the fog by each: with the
kit's settings, BSP 0, at `-68.54 -81.83 39.30 120 -15 0 70`, `-88.11 -63.71 39.53 120 -15 0 70`,
`-85.38 -50.75 39.96 240 -15 0 70` and `-45.47 -63.22 41.32 240 -15 0 70`, M0's tree draws the shell (2 to 3% of the
frame's pixels differ) and M1's does not. The design's fog as a screen pass (no geometry drawn for fog at all) is
still to come.

### The scene records: SCENE_STATIC, SCENE_LIGHTS, SCENE_INSTANCE and the motion

M1's scene plumbing (`docs/prism_scene_records.md`) was measured after each step: c1, SCENE_STATIC and SCENE_LIGHTS
(the ray tracer's level as drawn); c2, with SCENE_INSTANCE, SCENE_VIEW and the G-buffer's second target, and the
determinism work merged.

- **Equivalence** (Classic preset, RTX 3070 Ti, all eleven frozen scenes): the worst pose's mean FLIP is the same as
  before at every scene, c1 and c2: G1 0.0025, G2 0.0035, G3 0.0024, G4 0.0018, G5 0.0020, G6 0.0027, G7 0.0026, S1
  0.0026, S2a 0.0010, S2b 0.0028, S3 0.0029. With the kit's settings (RTX 2060): G2 0.0046, G4 0.0011, G5 0.0087 (c2
  0.0082), S1 0.0040 (c2 0.0039), S3 0.0021.
- **Material source**: 1.000000 at every pose of every scene, both settings, c1 and c2.
- **Classic byte identity** against M1's G-buffer build: `frames` (a10, b30) 46 of 46 alike each; `stills` (the kit's
  settings, RT off) G2, G4 and G5 30 of 30 alike (c2's with the harness holding no frame: see above), G7 alike in no
  run of either tree (its floor lamps' glows). **Determinism** (`--simulation`): all alike, each tree with itself and
  the new with the base.
- **Vulkan validation** with synchronisation validation (G2's probes; `ce`, `ce_deferred`, path traced with the kit's
  settings and the level as drawn): c2 0 hazards, 0 VUIDs. c1 had 10 of one VUID (`vkCmdDraw-magFilter-04553`) in
  `ce_deferred` and path traced: the resolve sampled the G-buffer's integer words with a linear sampler since the
  G-buffer pass came in; c2 fetches them with a nearest one.
- **The ray tracer's level as drawn** (b30 with the sealed remodel, path traced, the kit's settings, at G2's probes):
  2.07 million triangles (125 MB of structure) against 1.68 million for the near level whole (112 MB); the frame's
  GPU time 16.7 ms against 17.8 on the RTX 3070 Ti at 1080p, 40.0 against 44.5 on the RTX 2060 at 720p (the levels
  not drawn are folded to points and cost the rays little; the structure is refitted only as surfaces change level).
  With the far level traced (the old default), b30's Forerunner structure showed blotches of shadow where a near
  surface's rays started under the far level; as drawn they are gone.
- **The motion** (a10, the camera panned 0.08 degrees a frame, RTX 2060): each frame warped back by its motion view
  against the frame before, a mean absolute error of 0.35 of 255, against 1.63 unwarped, least at the motion's own
  scale.

The frame times (medians of 600-frame blocks, `ce` against `ce_deferred`, the G-buffer pass alone; ms):

| Scene | Settings | 3070 Ti 1080p c1: frame / G-buffer pass | c2 | 2060 720p c1 | c2 |
| --- | --- | --- | --- | --- | --- |
| G2 | classic | 0.38 / 0.85, 0.22 | 0.38 / 1.04, 0.37 | 1.13 / 3.02, 0.94 | 1.13 / 3.51, 1.44 |
| G2 | kit | 5.55 / 6.60, 0.52 | 5.83 / 7.03, 0.75 | 11.13 / 12.58, 1.62 | 11.23 / 12.57, 1.71 |
| G4 | classic | 0.30 / 0.70, 0.17 | 0.30 / 0.89, 0.35 | 0.57 / 1.70, 0.33 | 0.57 / 2.21, 0.88 |
| G4 | kit | 1.92 / 2.53, 0.17 | 1.92 / 2.71, 0.36 | 7.17 / 9.31, 0.39 | 7.18 / 9.82, 0.91 |
| G5 | classic | 0.27 / 0.69, 0.17 | 0.27 / 0.86, 0.34 | 0.73 / 1.91, 0.49 | 0.73 / 2.39, 0.95 |
| G5 | kit | 2.64 / 3.38, 0.24 | 2.64 / 3.56, 0.42 | 8.69 / 10.47, 0.81 | 8.69 / 11.01, 1.22 |

c1 is M1's G-buffer build's to the rounding (SCENE_STATIC and SCENE_LIGHTS cost the frame nothing measurable). c2's
second target (16 bytes a pixel more written, the motion's arithmetic) adds 0.15 to 0.2 ms to the G-buffer pass on
the 3070 Ti and 0.5 ms on the 2060, the worst on the stock world 0.37 ms of 2.5 and 1.44 ms of 4.0: within budget.

## M2: the bake and PRISM Raster

M2 (`docs/prism_bake.md`) is judged on bakes made in the game by its own baker (`debug.bake_exit`, the measurements'
runs) and on the frozen scenes captured with `capture.py bake`:

```sh
python3 tools/prism_gate/capture.py refs G2 G3 --stage <tree> --data <data> --config <classic toml> --out <refs>
python3 tools/prism_gate/capture.py bake G2 G3 --stage <tree> --data <data> --config <classic toml> --out <dir> \
    --env HALO_BAKE_FOLDER=<bakes> --env HALO_BAKE_QUALITY=max
python3 tools/prism_gate/gate.py bake <dir> <refs>
```

`capture.py bake` holds the camera at each probe pose with `display.lighting` `"raster"` reading a bake made before
(`display.bake` `"off"`: nothing is baked in the run) and saves PRISM Raster's frame, the bake's light less the sun's
(`debug.gbuffer_view` `bake_light`), the sun's visibility (`bake_sun`), the base colour, the normal and the instances,
then Classic's frame. `gate.py bake` puts the bake's static frame together the way Raster's resolve does (the base
colour by the light in CE's encoding, the sun's light by the pixel's normal and the visibility; CE's additions left
out) and the reference's the same way from R's linear file (`light.R/G/B`, `structure.V`: the static light, the
objects' shadows left out), over the level's pixels with a surface (the objects left out: static-only frames), and
takes the mean FLIP of one against the other, the worst pose a scene's, against `bake_flip` (0.05); the light alone and
the visibility alone are reported beside it.

PRISM Raster's frame against R's (`refs/p<n>_ref.png`, the path tracer's converged frame) is the design's other bound,
`raster_flip` (0.15): Raster is an approximation (the objects lit from the volume, CE's specular and reflections).

### M2 as measured

On the stock world (`config.classic.toml`'s settings), the bakes made by the game itself (`debug.bake_exit`), the
captures by `capture.py bake` against R captured by the same tree (`capture.py refs`, its reference frames), November
2026.

**The bake's time and size per map.** `ultra` (2048 samples a texel or probe at most, else D4's rule):

| Map, BSP | GPU | Time | Lightmap texels | Sun masks | Probes | Samples | File |
| --- | --- | --- | --- | --- | --- | --- | --- |
| b30, 0 | RTX 3070 Ti | 161 s | 48.4 M | 2,750 | 8.0 M | 35.0 G | 1,008 MB |
| b30, 1 | RTX 3070 Ti | 104 s | 21.4 M | 11 | 0.4 M | 14.1 G | 353 MB |
| a10, 0 | RTX 2060 | 83 s | 8.9 M | 237 | 0.08 M | 2.6 G | 141 MB |
| a30, 0 | RTX 2060 | 1,717 s | 80.9 M | 6,125 | 17.4 M | 78.6 G | 1,818 MB |
| c40, 2 | RTX 3070 Ti | 480 s | 40.7 M | 0 | 1.8 M | 37.3 G | 719 MB |

`max` (D4: no sample budget) did not finish. b30's first BSP on the RTX 3070 Ti: 5.7 hours of lightmap work reached
188,210 of its 188,934 tiles (99.6%), the masks and the volume not begun, the file 815 MB; the tiles left held texels
past 14 million samples each. a10 on the RTX 2060: 6,489 of 34,631 tiles in 2.4 hours. Where the samples went, from
b30's finished tiles: 69% of all samples to the 9% of texels whose light is under 0.01 (a nineteenth of middle grey),
35% to the 3% under 0.002; a sunlit texel stops in a few thousand. Those texels are lit through narrow openings (the
light reaching them a rare path's), which a path tracer without guiding meets once in hundreds of samples; their 1%
is of a light the eye barely sees. So `max` as D4 defines it needs either far better sampling of those paths (path
guiding, or the light cached between texels) or an error measured against a floor nearer the eye's (1% of the
texel's light or of a tenth of middle grey, whichever is more): Adam's decision. The deliverables below are `ultra`'s.

**The bake against R** (`bake_flip` 0.05; `ultra`):

| Scene | Worst pose | Poses | The light alone | The sun's visibility alone | |
| --- | --- | --- | --- | --- | --- |
| G2 b30 sunny | 0.0416 | 0.0328, 0.0416, 0.0350 | 0.058 to 0.077 | 0.015 to 0.025 | pass |
| G3 b30 shade | 0.1016 | 0.1016, 0.0397, 0.0431 | 0.085 to 0.175 | 0.018 to 0.032 | fail |

G3's first pose fails on the ground under trees: R's paths meet the objects (the trees, the rocks: mask 4 and 8 of
its scene), the bake's only the level, so the bake's light there is the open sky's (1.85 times R's on that ground).
The bake should trace the level's static scenery too (M3).

**PRISM Raster against R** (`raster_flip` 0.15): G2 0.235 (poses 0.177, 0.235, 0.230), G3 0.299 (0.299, 0.191,
0.181): fail. Classic against the same frames: G2 0.069 to 0.090, G3 0.049 to 0.077. Raster's frame is lit from the
bake as the gate composes it (which passes on G2), and differs from R by what the composition leaves out: the objects'
sun shadows (R's are traced, Raster keeps CE's, which the scenery does not cast), the objects lit from the volume, and
CE's additions on the bake's light (11% brighter than R's frame on G2's level).

**The frame time** (`m2time`'s three poses of G2, 600 frames each, the GPU's time, the bake loaded): PRISM Raster at
2560 × 1440 on the RTX 3070 Ti 2.26 ms median (99th percentile 2.34); at 1920 × 1080 on the RTX 2060 10.4 ms (99th
percentile 10.5), Classic there 1.68 ms: both within 16.7 ms.

**Classic's identity.** `identity.py frames` against M1's tree (031cd33a1): b30's and a10's 46 frames each
byte-identical (and each tree's two runs alike); `determinism`: the simulation alike tick for tick (1,475 ticks).

**The Cycles cross-check** was not run: the scene's export to Blender (the materials, the rig) is not written yet, and
R is the gate's truth.

## M3: Path Traced

M3 (`docs/prism_pathtraced.md`) is judged on the frozen G scenes with `display.lighting` `"pathtraced"` reading a bake
made before by the game itself (`ultra`, `debug.bake_exit`; the run's `HALO_BAKE_FOLDER`, `display.bake` `"off"`),
against its own reference: `debug.pt_reference` in that mode traces the same scene, materials and lights unbiased at
every pixel with no bake, summed until its estimated relative MSE is under `reference_noise` (0.001) or its frames
are done, its file `p<n>.exr` (`light.R/G/B`, `spec.R/G/B`, `Z`, `samples`, `noise`) and its frame `p<n>_ref.png`.
The moves' RT off is Classic's frame (`v4off`); the moving frames measured are the `quality` profile's (`v4pt`), the
`performance` profile's (`v4perf`) beside them; `v4raw` is the light as traced (the denoiser off, a diagnostic).

```sh
python3 tools/prism_gate/capture.py moves G5 --stage <tree> --data <data> --config <classic toml> --out <dir> \
    --states v4off v4pt v4perf v4raw --env HALO_BAKE_FOLDER=<bakes> --env HALO_BAKE=off --env HALO_BAKE_QUALITY=ultra
python3 tools/prism_gate/capture.py refs_v4 G5 --stage <tree> --data <data> --config <classic toml> --out <dir> \
    --reference-frames 4096 --env HALO_BAKE_FOLDER=<bakes> --env HALO_BAKE=off --env HALO_BAKE_QUALITY=ultra \
    --env HALO_PT_REFERENCE_PATHS=4
python3 tools/prism_gate/gate.py measure <dir> G5 --state v4pt --off v4off
python3 tools/prism_gate/evidence.py sheet <dir> G5      # the still sheet
python3 tools/prism_gate/evidence.py clip <dir> G5       # the moving camera's clip (H.264)
```

The bake's quality is part of its key (`docs/prism_bake.md`): a run reads a bake only with the quality it was made
at (`HALO_BAKE_QUALITY`), else the bake is stale and, with `display.bake` `"off"`, there is none. The thresholds are
M0's (error 0.01, blotch 0.015, flicker 1.30, still FLIP 0.10). The frame time is the GPU's
(`HALO_VULKAN_GPU_TIMES_FILE`) and each pass's (`HALO_PT_TIMES_FILE`: the light, the temporal, fix and a-trous passes,
the composite). V1 and V2 are being frozen on another branch: their gates are not taken here.

### M3 as measured

Round 1 (November 2026). The bakes are `ultra` (2048 samples a texel at most), made by the game itself.

**The frame time on the RTX 3070 Ti** (`quality`, native 1920 × 1080, the stock world (the Classic preset), a10's
three probe poses (G5), 600 frames each, three times: nine blocks, each block's first 60 frames dropped; the GPU's
time, `HALO_VULKAN_GPU_TIMES_FILE`; the path tracer's passes, `HALO_PT_TIMES_FILE`; a10's `ultra` bake loaded):

| Tree | Frame median | Frame 99th percentile (median of the blocks') | The blocks' 99th percentiles | The path tracer's passes (median, ms) |
| --- | --- | --- | --- | --- |
| d8bd5f053 (four 5 × 5 a-trous steps) | 13.84 ms | 19.18 ms | 17.6 to 24.7 | 13.35: light 6.85, temporal 0.76, fix 0.32, atrous 4.77, composite 0.30 |
| a32e190a4 (five 3 × 3 steps, the fast lamp pick) | 11.68 ms | 15.58 ms | 14.3 to 20.7 | 11.04: light 7.03, temporal 0.76, fix 0.32, atrous 2.43, composite 0.30 |

Within the gate (median at most 16.7 ms, 99th percentile at most 20 ms) on G5's poses: one block of nine has its 99th
percentile at 20.7 ms. The light pass is two thirds of the path tracer's time.

**Classic's stills** (`identity.py stills`, the kit's settings, RT off, against the base 0950596f5): G2, G4 and G5 30 of
30 frames alike (each tree with itself, and the new with the base); G7 alike in no pair, the base with itself
included: its floor lamps' glows, as M0 and M2 found.

## Every milestone also passes

`tools/prism_gate/identity.py` checks the milestone's tree against the tree it started from:

- **frames**: each tree run twice on b30 and a10 with the game's defaults (Classic), the clock fixed and
  interpolation off, every frame from the simulation's 40th second to its 41.5th saved: each tree alike with
  itself, and the new alike with the base, byte for byte.
- **stills**: each tree run twice on G2, G4, G5 and G7 with the kit's settings and RT off (`display.lighting`
  `ce`), by the capture harness (a path an older build also reads), the last 30 frames of a hold at each scene's
  first probe pose: alike byte for byte, as `frames`.
- **determinism**: each tree twice on a10 with the determinism log, the frames' time fixed and a scripted player;
  `tools/determinism_compare.py --simulation`: each tree with itself and the new with the base.

```sh
python3 tools/prism_gate/identity.py frames <base tree> <new tree> --data <data> --out <dir>
python3 tools/prism_gate/identity.py stills <base tree> <new tree> --data <data> --out <dir> --config <toml>
python3 tools/prism_gate/identity.py determinism <base tree> <new tree> --data <data> --out <dir>
```

and the design's others: no change to `HALO_PORT_NETWORK_VERSION`, Vulkan validation clean, `pytest tools/harness`.

## Not yet frozen

S4 (c10's swamp: the pose tried is an interior), S3's flashlight (the player's light is where the player is, not
the scene's camera; `debug_object_place "player at <x> <y> <z> <yaw>"` now stands the player there, and
`units_set_desired_flashlight_state (players) true` turns the light on, but in a first try at S3's pose the light was
not seen and a hint of the HUD was drawn over the frame), V1 (a Warthog driven across b30's beach before a still camera: the Warthog placed by name was
not made) and V2 (a50's firefight with the AI kept: the poses tried show no fight) need their poses or setup found
in play before they join `scenes.json`. The harness has what they need: a live scene (`"live": true`: the AI kept, the
clock running during its moves, each state its own run so that every state sees the same world; its references
made by replaying the run to the probe's step and stopping there), a scene's own setup commands and scripted
input (`"setup"`, `"input"`: `debug_object_place "player seat W-driver"` puts the player's own unit in a placed
vehicle for `debug.test_input` to drive), and `metrics.ghost_trail`.
