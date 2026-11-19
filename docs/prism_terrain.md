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
| `PRISM_TERRAIN`, stream version 40 (type 106, after `UPSCALE`) | `port/linux/src/xgpu_stream.h` (record, phases, flags, event kinds, sizes), `xgpu_stream_encode.c` (the guest's writer), `xgpu_scene_encode.c` (the tests' encoder), `port/android/host/host_render_stream.{h,c}` (decoded and checked), `port/linux/vulkan/xgpu_vulkan_stream.c` (registered) |
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
- `WEATHER_DRAW`: specified for T12, never sent yet (its guard returns false: CE draws the weather);
- `WATER_DRAW` (T7, below): the steps about each CE water group the renderer draws.

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
| `display.water` | `"prism"`, `"ce"` | T7: `"prism"` PRISM's water layer in CE's colour draw's place (below) |
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

## T7: the water layer (Rebaked, and Path Traced's frame)

With `display.water` `"prism"` (the default) in a campaign in Rebaked or Path Traced, on a backend with ray tracing
(`xgpu_backend_ray_tracing`: the layer's pass runs on its device), CE's water is drawn by PRISM in place of CE's colour
draw. CE's depth is kept, so CE's fog planes, effects and particles still depth-test against the water; a sky's water
stays CE's. Classic and `"ce"` draw CE's water as before (feature-off identity: the frames are byte for byte the
previous build's).

### How a group is drawn

The design (section 8.1) groups SCENE_STATIC's water triangles into bodies; T7 does not: what the layer needs of CE's
water (its own ripple map at CE's own texture coordinates and animation, its base map, the reflection cube) only the
game's draws can give, so the game draws them into the frame for the host, and the host composites. In
`rasterizer_water_draw` (`rasterizer_xbox_water.c`, `rasterizer_water_draw_prism`):

| Step | The game | The host (`xgpu_vulkan_stream.c`, `stream_water`) |
| --- | --- | --- |
| `BEGIN` | CE's ripple bump map built as CE builds it; the step | At the next draw (the game's targets are bound with it): the colour copied (what is behind the water); the colour's alpha emptied |
| | The ripple map drawn at the water's pixels as CE's reflection pass samples it (its vertex program, ripple scale, drift and mip bias), the reflection cube bound beside it; alpha 1 | |
| `CAPTURED` | | The ripples copied, the cube kept |
| | The base map drawn there: its colour where it modulates what is behind, else its alpha where that modulates the reflection, else white; alpha 1; the water's depth written | |
| | CE's own depth-only draw | |
| `END` | | The base map and the mask (the alpha) copied; the layer composited (`xgpu_vulkan_water.c`, a compute pass) and copied into the colour target, what was behind put back wherever the captures drew outside the water. Where it cannot run (no G-buffer of the window's), what was behind is put back and the water is not drawn (logged) |

A water CE draws before the fog (c10's swamps) has its colour draw as the environment is drawn, before PRISM's frame
is resolved: there its depth alone, and the layer at its queued draw after the resolve, over the fog CE drew on it.
`WATER_DRAW`'s item carries the step, the shader's flags, the group's plane, CE's ripple scale and tints (straight on and
edge on) and the window's planar fog (its colour and density, depth and distance, the eye's height over it).

### The layer (`xgpu_vulkan_water.glsl`)

Each water pixel: the plane met by the pixel's ray (the G-buffer's distances give what is behind), the normal bent by
CE's ripples (flattened edge on and far off, where the 8-bit map would alias), in light (the frame's values to 2.2):

- **Fresnel** at an IOR of 1.33 (Schlick, F0 0.02).
- **Reflection**: marched along the screen against the G-buffer's distances (refined by halving, faded at the view's
  edges, only surfaces above the water); else what lies far along the reflected ray where the frame shows it (the sky,
  or far surfaces in their fog); else CE's reflection cube in CE's tint. Blurred with distance (the ripples finer than a
  pixel as roughness). CE's base map alpha, where it modulates CE's reflection (b30's mask by the shore), modulates it.
- **Refraction**: the floor the refracted ray reaches (Snell, found on the screen, the pixel's own where it is not), its
  colour absorbed over the path by Beer and Lambert, in CE's tint's colours (the base map's where it tints the
  background, else the straight-on tint's hue), in-scattered toward the deep colour (CE's planar fog's colour where the
  window has one, else the tint's). Where the frame has no floor (CE's fog beyond the world), what CE drew there.
- **The sun's highlight** off the ripples (GGX, rougher far off), from SCENE_LIGHTS' sun.
- **The shore**: the water fades into the ground over its first 0.05 world units of depth.
- **From below**: Snell's window (CE's cube, the sky) and total internal reflection outside it (the deep colour, and
  what the screen has under the surface along the reflected ray), with a little of the deep colour over long paths.
  CE's own planar fog (b30's: opaque in 0.15 world units) never fogged CE's water's surface, drawn after it, and
  would hide the window.

**An opaque water** (d40's: an environment shader of the water class, `classes.json`): no capture and no refraction;
after the resolve, its pixels (the level's of a material of the water class) take reflection, Fresnel and the sun's
highlight over CE's own colour of them, the normal from its own bump.

Tuning, for one run (the defaults are the look judged against Classic by pictures): `HALO_WATER_RIPPLES` (0.35),
`HALO_WATER_ABSORB` (0.25 a world unit), `HALO_WATER_SUN` (1.5), `HALO_WATER_DEEP` (1), `HALO_WATER_CUBE` (1), and
`HALO_WATER_VIEW` (1 the ripples, 2 the base map, 3 the mask, distance and side, 4 the reflection, 5 the refraction).

The bake is untouched: water stays out of SCENE_STATIC and the bake's scene, and a bake made by the build before loads
whole. In Path Traced the same layer is drawn over the traced frame; its traced reflection and refraction rays are T8.

What T7 leaves for later: the waves' motion and foam (T9: the surface is CE's plane, its motion CE's ripples), the
shore's run-up and caustics (T10), water in the acceleration structures and traced water rays (T8, T14), a sun shadow
on the water's highlight (the cliffs' shade does not dim it yet).
