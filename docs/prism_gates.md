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

So the byte-for-byte check covers RT off and the references of the scenes above; the live path-traced frames are
judged by the metrics only, until M1's deferred core replaces the tracer's inputs with records it can make
deterministic (the materials resolved at load, the instances by their ids, the cache claimed in a fixed order).

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
the scene's camera), V1 (a Warthog driven across b30's beach before a still camera: the Warthog placed by name was
not made) and V2 (a50's firefight with the AI kept: the poses tried show no fight) need their poses or setup found
in play before they join `scenes.json`. The harness has what they need: a live scene (`"live": true`: the AI kept, the
clock running during its moves, each state its own run so that every state sees the same world; its references
made by replaying the run to the probe's step and stopping there), a scene's own setup commands and scripted
input (`"setup"`, `"input"`: `debug_object_place "player seat W-driver"` puts the player's own unit in a placed
vehicle for `debug.test_input` to drive), and `metrics.ghost_trail`.
