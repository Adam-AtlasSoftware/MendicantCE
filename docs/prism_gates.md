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

G1 to G7 are the path tracer's gate scenes, their keys as `prism-pt`'s gate checked them (inside the level, every
probe pose seeing surfaces). The design's S1 to S4 and V1 and V2 are listed in "Not yet frozen" below.

A scene is changed only with a note here saying why: every milestone's figures are measured on these.

### What a capture is

`capture.py` runs a staged tree (`tools/linux_package.py tree`) hidden, silent and isolated, and drives it with a
camera path (`debug.camera_path`): every pose and every console command at a known frame, so that every run of the
same build draws the same frames.

- **The run.** No window (SDL's dummy video driver, `debug.hidden_window`), no sound (`HALO_NO_AUDIO=1`,
  `SDL_AUDIODRIVER=dummy`), no controllers (`SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000`,
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`, `SDL_JOYSTICK_HIDAPI=0`), nothing online (the run's own empty brokers,
  relays and masters files), its own data, save and config folders, the clock fixed (`HALO_FIXED_STEP=1`: a tick a
  frame) and interpolation off (`HALO_INTERPOLATION=false`). The data folder links the maps and PRISM's folders
  read only; the mods are linked one by one beside a copy of their load order. Every run takes the same
  `config.toml` (the kit's, with `display.prism_rt` off: the path turns it on).
- **The kit.** Before every run the harness looks for a process whose executable is in the kit (the game the person
  this works for plays) and stops if one runs.
- **The harness's rules.** At the path's first frame: the console's lines and the HUD off, the player deathless
  (`cheat_deathless_player 1`), the weather off (`weather 0`) and the AI erased (`ai_erase_all`). The game then
  runs 900 frames with the camera at the first key and is stopped (`game_speed 0`). The frames let a level's opening
  play out (b30's insertion: the director takes the path's camera only once it has, and only in a tick),
  projectiles land and a hit's flash fade.
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

`tools/prism_gate/thresholds.json`, fixed on M0's first measurements (below):

| Measure | Threshold |
| --- | --- |
| error | ≤ 0.01 |
| blotch | ≤ 0.015 |
| flicker | ≤ 1.30 |
| flip, still camera | ≤ 0.10 |
| flip, V1 and V2 | ≤ 0.15 |
| trail | ≤ 4 frames |
| harness fault (sky) | > 0.8 of a reference's pixels |
| the bake's noise (D4) | 0.01 |

The bake's noise threshold (design 10.1, D4: a texel's samples are added until it converges, never cut by a
budget) is the relative standard error of a texel's mean luminance: a texel stops once its standard error is under
1% of its mean, over at least 64 samples, or once its mean is under 1/1000 of the frame's middle grey (black stays
black without a million samples).

## Every milestone also passes

`tools/prism_gate/identity.py` checks the milestone's tree against the tree it started from:

- **frames**: each tree run twice on b30 and a10, the clock fixed and interpolation off, every frame from the
  simulation's 40th second to its 41.5th saved, with the game's defaults (Classic) and again with the kit's
  settings and RT off (`display.lighting` `ce`): each tree alike with itself, and the new alike with the base,
  byte for byte.
- **determinism**: each tree twice on a10 with the determinism log, the frames' time fixed and a scripted player;
  `tools/determinism_compare.py --simulation`: each tree with itself and the new with the base.

```sh
python3 tools/prism_gate/identity.py frames <base tree> <new tree> --data <data> --out <dir> [--config <toml>]
python3 tools/prism_gate/identity.py determinism <base tree> <new tree> --data <data> --out <dir>
```

and the design's others: no change to `HALO_PORT_NETWORK_VERSION`, Vulkan validation clean, `pytest tools/harness`.

## Not yet frozen

The design's S1 to S4 (a30's cliffs under a fog plane with the remodel, b30's water and a10's cryo glass, a50's
hangar with the flashlight, c10's swamp) and V1 and V2 (a Warthog driven across b30's beach before a still camera,
a50's firefight with the AI kept) need their poses found in play and checked for sky-only and out-of-level frames
before they join `scenes.json`. The harness has what they need: a live scene (`"live": true`: the AI kept, the
clock running during its moves, each state its own run so that every state sees the same world; its references
made by replaying the run to the probe's step and stopping there), a scene's own setup commands and scripted
input (`"setup"`, `"input"`: `debug_object_place "player seat W-driver"` puts the player's own unit in a placed
vehicle for `debug.test_input` to drive), and `metrics.ghost_trail`.
