# PRISM's terrain, weather and water overhaul

The plan is `overhaul_research/prism_v4_renderer/terrain/design.md` (with its surveys `inventory.md`,
`engine_hooks.md` and `techniques.md` beside it): high-quality sand, dirt, mud, rock, snow and ice in CE's style,
footprints and tracks, wetness that dries, drifts at walls' feet, wind with dust and snow in the air, and water with
real reflection, refraction and motion. It is built in milestones T0 to T15 (the design's section 13). This file says
what is built and how it fits; the design says what comes.

Scope: campaign only, PRISM Rebaked (`display.lighting` `"raster"`) and PRISM Path Traced (`"pathtraced"`) only.
Classic is untouched and byte-identical; nothing here reaches the game's simulation, its determinism or the network.

## T0: the scaffolding

| Part | Where |
| --- | --- |
| `PRISM_TERRAIN`, stream version 39 | `port/linux/src/xgpu_stream.h` (record, phases, flags, event kinds, sizes), `xgpu_stream_encode.c` (the guest's writer), `xgpu_scene_encode.c` (the tests' encoder), `port/android/host/host_render_stream.{h,c}` (decoded and checked), `port/linux/vulkan/xgpu_vulkan_stream.c` (registered) |
| The game's half | `port/linux/game/prism_terrain.{h,c}`: gating, the event ring, the waders, the wind and weather, the frame's records; called from `world_ao_frame_begin` |
| The seven hooks | `source/units/bipeds.c` (`biped_make_footstep`), `source/effects/material_effects.c` (the footstep's ray), `source/units/vehicles.c` (`create_slipping_effects`), `source/items/projectiles.c` (`projectile_collision`'s impact, `projectile_detonate`), `source/objects/damage.c` (`area_of_effect_cause_damage`), `source/render/render.c` (the weather's guard), `source/rasterizer/xbox/rasterizer_xbox_water.c` (the water's guard) |
| CE's material type kept | `xgpu_vulkan_stream.c`'s `struct gbuffer_material` (`material_type`), and the terrain's own table |
| Surface classes | `port/linux/vulkan/xgpu_vulkan_terrain.{h,c}`; the curated table `tools/prism_terrain_classes.py`, written as `port/assets/prism_terrain/classes.json` and `xgpu_vulkan_terrain_classes.inc` |
| Settings | `port/linux/src/port_config.c`, `port/linux/README.md` |
| Debug views | `debug.gbuffer_view` `"terrain_class"`, `debug.terrain_events`: the post program `TERRAIN_DEBUG` (`tools/vulkan_post_shaders.py`, `xgpu_vulkan_post.c`) |

### The record

Each frame, in a campaign in Rebaked or Path Traced with `display.terrain` `"on"` (`prism_terrain_wanted`: also a game
in progress and no main menu), the game sends:

- `FRAME`: a generation (a new map, a BSP switch, a revert to a checkpoint or a new game, found by the game's clock
  going back: the renderer then forgets every event and every dynamic thing), the game's time step (0 while paused),
  real seconds, the first local player's camera, the BSP, the settings and debug views as flags,
  `display.terrain_quality`, the camera's weather palette entry and whether it has a particle system, the wind at the
  eye (`scenario_get_wind`) and each palette entry's wind there, the map's checksum;
- `EVENTS`: the frame's events within 60 world units of a local player's camera, at most 256: feet (CE's material
  effects of a biped: walk, run, slide, shuffle, jump, landing; the side; the contact marker's facing; the ground
  under it from the footstep's own ray: its hit, normal and CE's material type), a vehicle's mass point moving on the
  ground (its contact, normal, motion and CE's speed scale), a projectile's impact (point, normal, material, velocity)
  and detonation, an explosion (centre, cutoff radius). Sub-steps and co-op prediction may send an event twice; what
  the renderer makes of them must be the same made twice (a stamp's `min` and `max`);
- `WADERS`: units within 25 world units of the eye in water, or out of it for less than 10 seconds (the game's ticks
  since they left it, a table of `prism_terrain.c`'s own);
- `WEATHER_DRAW` and `WATER_DRAW`: specified for T12 and T7, never sent yet (their guards return false: CE draws the
  weather and the water).

A frame's records are 176 bytes each plus 32 an item: at most 8,368 bytes for the events (the design's bound is
12 KB a frame). `tools/test_scene_records.py` checks the layout on ILP32 and LP64, the encoder's bytes and refusals,
the type's place and the decoder.

### What the hooks may do

Every hook's first act is `prism_terrain_wanted()`; with `"ce"`, in multiplayer, in the menus or with
`display.terrain` `"off"` it returns before anything. They read their arguments (and a biped's tag name for its
species) and append to `prism_terrain.c`'s ring. None writes the game's state, calls a random function or touches a
seed. The frame reads objects (the units' positions, media flags and velocities), the scenario (the cluster's weather
palette entry) and the wind (pure reads). The renderer sends nothing back.

The footstep hook sits where CE finds the foot's marker, inside CE's own gate (a foot within 10 world units of a
local player's camera: `material_effect_visible`). T4's prints will need feet up to 40 world units away; moving the
hook out of that gate (and finding the marker and the ground there) is part of T4.

### Surface classes

Each material of the map (`SCENE_MATERIAL`) gets two classes (none, sand, dirt, mud, rock, snow, ice, water: CE's
blended environment shaders mix two detail maps by the base map's alpha, so a shader can be two grounds), an HQ
layer, one HQ tile's metres, a porosity and whether drifts pile on it:

- an environment shader's from CE's material type: dirt, sand, stone (rock), snow, ice; a model shader typed stone (scenery's rocks) rock, every other none;
- the curated table replaces them by the shader's path hash (`SCENE_MATERIAL`'s `path_hash`): the ten campaign maps'
  ground shaders from `inventory.md` sections 2 to 4 (b30's beach sand over CE's grass detail and its sea bed; b40 and
  c40's snow, snow over rock, ice and cliffs; c10's swamp dirt over mud and its beds; a30, a50 and d20's dirt and
  rock, d20's two cliffs CE typed dirt; d40's sand and its opaque water; the Autumn's engine pieces on d40, which CE
  typed stone, are none).

`classes.json` is what the terrain data pack will ship: path hashes and our numbers, no tag names and nothing of CE's
assets. T0 builds the same table into the host (`xgpu_vulkan_terrain_classes.inc`); reading a pack's or a mod's
`classes.json` at run time comes with the pack (T1). `python3 tools/prism_terrain_classes.py --check` checks both
files against the curated list.

In T0 only the level's pixels show their classes; an object shows rock only (the design's rule), and static scenery
counts as an object in the view (its G-buffer pixels carry no static-scenery flag yet).

### Settings

| Setting | Values (default) | T0 |
| --- | --- | --- |
| `display.terrain` | `"on"`, `"off"` | Sends the record (nothing visible) |
| `display.terrain_quality` | `"auto"`, `"low"`, `"medium"`, `"high"`, `"ultra"` | Sent, unused |
| `display.terrain_layers` | `"hq"`, `"ce"` | Sent, unused |
| `display.terrain_tracks` | `true` | Sent, unused |
| `display.terrain_drifts` | `true` | Sent, unused |
| `display.weather` | `"prism"`, `"ce"` | Sent; CE's weather is drawn either way |
| `display.water` | `"prism"`, `"ce"` | Sent; CE's water is drawn either way |
| `debug.gbuffer_view` | `"terrain_class"` | The class view (a view: nothing drawn over it) |
| `debug.terrain_events` | `false` | The events drawn over the frame for 30 seconds, and logged |

Each has an environment variable (the README's table) and the console's `prism_set "<key> <value>"`. They are not in
the settings menu yet (they change nothing to see); they join the Rebaked and Path Traced pages with T1.

### Debug views

`debug.gbuffer_view` `"terrain_class"`: each opaque pixel by its material's classes (sand gold, dirt brown, mud dark
olive, rock blue-grey, snow white, ice cyan, water blue), striped 8 pixels wide where its second class differs, a
little of the frame's lightness kept; every other pixel the frame's lightness, dark.

`debug.terrain_events`: the last 128 feet, impacts and explosions and the last 128 wheel contacts, each a disc with a
solid rim where it happened (projected by the window's `SCENE_VIEW` camera), fading over 30 seconds: a left foot green,
a right foot red, a landing magenta, a wheel orange, an impact cyan, a detonation or an explosion white. Each event
but the wheels' is logged (`prism terrain: event ...`; one wheel contact in 16), and every 10 seconds the counts by kind and the largest
record's size.

### The capture poses

Recorded in T0 for every later milestone (`tools/prism_gate/scenes.json`, `docs/prism_gates.md`); poses are x y z yaw
pitch roll field of view:

| Pose | Map, BSP | Pose | What |
| --- | --- | --- | --- |
| B1 | b30, 0 | -60.0 18.3 4.5 180 -15 0 70 | The first beach from inland, the shoreline (x = -73) across the frame |
| N1 | b40, 0 (b40_a) | 150.0 -136.0 2.5 -90 -12 0 70 | The snowfield of the canyon floor, its cliff walls |
| D1 | d40, 8 (d40_terrain) | -6.0 20.0 29.0 90 -8 0 70 | The sand valley between dunes |
| R1 | a30, 0 (a30_a) | 27.0 -103.0 60.5 0 -12 0 70 | The dirt by the first player start, rock beyond |
| M1 | c10, 0 (c10_exteriora) | -138.0 -44.0 42.5 180 -15 0 70 | The swamp's dirt and mud |

Scripted actions (the game live, the AI erased, the camera held at a pose, the player played by
`debug.test_input` `"script:..."` on the game's clock in seconds from the scenario's start; the path starts at tick
1800, after b30's landing, and puts the player at its first pose by `cheat_teleport_to_camera`):

- **A1**: from -67.0 15.0 2.2 on B1's sand, aim north and walk 3 s, turn and walk 3 s back, seen from B1's pose:
  `script:61-61.1 aim=90,0;61.2-64.2 move=0,1;64.4-64.5 aim=-90,0;64.6-67.6 move=0,1`.
- **A2**: the scenario's Warthog `jeep` made on the sand plateau above the first beach (`object_create_anew jeep`),
  the player put in its seat (`unit_enter_vehicle (unit (list_get (players) 0)) jeep "W-driver"`), then driven round
  with the stick held (`script:62.5-71 move=0,1;63.2-71 look=0.45,0`), seen from -24.0 12.0 30.0 90 -42 0 70. (The
  design's loop through the surf wants a vehicle on the beach itself: the scenario's beach Warthog has no name to
  script it by; T5 can teleport `jeep` with a cutscene flag once one is found.)

The run folder's `tools/tcap.py` and `jobs/actions.sh` make them (`work/openh1_eval/v4_terrain_runs`).
