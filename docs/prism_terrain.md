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
| `PRISM_TERRAIN`, stream version 40 (41: T1's pack folder; 42: T7's `WATER_DRAW`) (type 106, after `UPSCALE`) | `port/linux/src/xgpu_stream.h` (record, phases, flags, event kinds, sizes), `xgpu_stream_encode.c` (the guest's writer), `xgpu_scene_encode.c` (the tests' encoder), `port/android/host/host_render_stream.{h,c}` (decoded and checked), `port/linux/vulkan/xgpu_vulkan_stream.c` (registered) |
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

A frame's records are 180 bytes each plus 32 an item (176 before T1's pack folder, stream version 41; T7's
`WATER_DRAW`, version 42, is one 96-byte item): at most 8,372 bytes for the events (the design's bound is
12 KB a frame). `tools/test_scene_records.py` checks the layout on ILP32 and LP64, the encoder's bytes and refusals,
the type's place and the decoder.

### What the hooks may do

Every hook's first act is `prism_terrain_wanted()`; with `"ce"`, in multiplayer, in the menus or with
`display.terrain` `"off"` it returns before anything. They read their arguments (and a biped's tag name for its
species) and append to `prism_terrain.c`'s ring. None writes the game's state, calls a random function or touches a
seed. The frame reads objects (the units' positions, media flags and velocities), the scenario (the cluster's weather
palette entry) and the wind (pure reads). The renderer sends nothing back.

The footstep hook sits where CE finds the foot's marker. CE's own gate (a foot within 10 world units of a local
player's camera: `material_effect_visible`) still decides CE's effect; since T4 a foot past it is still found for the
prints where `prism_terrain_footstep_far` says so (display.terrain_tracks, within 40 world units of a local player's
camera; `FALSE` before anything else in Classic and wherever the hooks return), and its ground is found by the
footstep's own ray there (`prism_terrain_footstep_trace`: the structure's alone, read only). The foot's facing sent is
the body's forward (the way a print points), not the contact marker's (the skeleton's axes).

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
| `display.terrain_tracks` | `true` | T4: footprints (below) |
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

## T1: the HQ layers

The level's ground (sand, dirt, mud, rock, snow, ice) gets fine detail from CC0 scans while CE's colour, its painted
variation and its large shapes stay exactly CE's. It works in PRISM Rebaked and Path Traced alike, because both read
the same targets.

### The terrain pack

`tools/prism_terrain_layers/prism_terrain_layers.py build --data <data folder>` fetches the scans named in
`tools/prism_terrain_layers/manifest.json` (it asks first; `--yes` does not), checks each archive's SHA-256, refuses an
albedo that still holds light (its high-passed lightness following the normal map's shading), and writes
`<data folder>/prism_terrain/layers.ptl` with `manifest.json` and `LICENSES.txt` beside it. Nothing of the game's data
goes in. The sources and their licences are in `port/assets/prism_terrain/LICENSES.md`; all are CC0 1.0
(ambientCG).

| Layer | Scan | Tile | Used by |
| --- | --- | --- | --- |
| `sand` | Ground054 | 3.5 m | b30's beach and sea bed |
| `dirt` | Ground103 | 2.5 m | a30, a50, c10, d20 grounds |
| `mud` | Ground025 | 2.5 m | c10's mud |
| `rock` | Rock023 | 4.0 m | cliffs (b30, b40/c40, a30, a50, d20), scenery debris on d40 |
| `snow` | Snow005 | 3.0 m | b40/c40 snow |
| `ice` | Snow014 | 4.0 m | b40/c40 ice |
| `sand_desert` | Ground080 | 2.0 m | d40's sand (`classes.json` names it) |
| `dirt_cracked` | Ground031 | 2.5 m | d20's cracked mud (`classes.json` names it) |

Each layer is two RGBA8 images at 1024 (the renderer makes their levels): the detail (the scan's linear albedo
divided by its own low-pass, so its mean is 1, stored as half) and its height; the normal (x, y), the roughness over
its mean and the occlusion. Each layer's numbers (tile, strength, chroma, normal, roughness, height contrast, grain)
travel in the pack's header. A 512 macro map of our own noise varies the detail's strength over tens of metres. The
file is about 68 MB (eight layers); the GPU holds about 90 MB with levels. `display.terrain_pack_folder` moves it.

### The pass

At the texture pass's end (`GBUFFER_PHASE_BASE_TEXTURED`, after the pack materials' albedo and before the decals; at
the resolve where that phase did not come), `xgpu_vulkan_terrain_layers` draws `TERRAIN_LAYERS`
(`tools/vulkan_post_shaders.py`) over every level pixel whose material has a layer, into the base target and the
G-buffer's normal and roughness. Where the device reads a target where it is drawn (dynamic rendering's local reads)
the words are the pass's first target, read there and written through a mask of their normal and roughness, and only
the base is copied (`TERRAIN_LAYERS_LOCAL`); elsewhere both are copied and the words written whole. For each
pixel:

- the point is found from the G-buffer's distance and `SCENE_VIEW`'s camera, and its footprint on the surface's
  plane (no derivative instructions, so the discards cost nothing);
- the layer is sampled in world space: from above where the surface is flatter than 41 degrees, from all three axes
  (whiteout-blended normals) where it is steeper; each projection hex-tiled (Mikkelsen 2022: three offset and turned
  samples on a triangle grid, weighted by their place and height; rock is not turned, so its strata stay level);
- detail transfer (design 3.2): CE's base colour times the detail in linear light, its lightness and a little of its
  own hue (the layer's chroma), by the layer's strength, the macro map and the fade (whole within 15 world units,
  none past 60). Its mean is 1, so CE's colour and the bake's mean albedo (`material_hash`) do not change: no rebake;
- a strength past 1 (sand 3.0, cracked mud 1.2) would carry a scan's darkest grains past black: below 0.3 the
  detail's ratio rolls off toward 0 along an exponential that meets it there with its slope, so a pebble stays a
  dark grain;
- the grain: a layer with a `grain` weight (sand 1.0; d40's sand has none) is looked up a second time from above, 3.71
  times finer and turned (one lookup: under the hex-tiled layer a grain's repeat does not show), and its mean-1 detail multiplied in (not its normal: a millimetre's relief), whole within 1
  world unit and gone by 12. One tile of a 1024 scan at 3.5 m is 3.4 mm a texel, magnified about three times at
  1 m, where sand read as soft blotches; the finer lookup gives it grain at about 1 mm, and from mid range its
  levels average to 1, so the 10 m and 60 m views do not change;
- the normal: the HQ normal whiteout-blended over the G-buffer's (CE's bump), its lookups isotropic, faded from 1 to
  6 world units (further off, the relief of a scan's grain reads as streaks in a low sun, and CE's own light is
  the ground's); the roughness: the G-buffer's times the layer's variation;
- a material of two classes (CE's blended shaders) takes its second class by CE's colour's lightness against each
  class's (and, with rock, by the slope), then by the two layers' heights. The layer `classes.json` names is the
  first class's, and the second's too where both classes are one (d40's sand and sand, d20's dirt and dirt): until
  T1's last round the second slot fell back to the class's own layer, so d40's paler texels drew b30's beach sand at
  strength 3 with its grain (the pale crust and dark pebbles seen at D2) and d20's took plain dirt.

`debug.pass_times` labels it `TERRAIN`. `HALO_TERRAIN_LAYERS_VIEW=1` shows the second class's weight, the fade and
the layer instead.

### CE's own detail maps up close

CE's ground draws fine detail maps (b30's sand speckle, 7.8 mm texels) that turn into large blocky noise within a few
metres; with the HQ layers on, that noise and the scan's grain would fight. In the PRISM modes with the pack there,
the texture pass (`source/rasterizer/xbox/rasterizer_xbox_environment.c`, `prism_terrain_detail_level`) marks a
ground shader's fine primary and secondary detail maps (texels a base map's repeat past `PRISM_TERRAIN_FINE_DETAIL`,
so CE's grain and not its large shapes; never grass or moss, b30's beach grass included; never the micro detail
map, CE's finest relief: d40's lumps and pits stay) by a border colour their wrapped addressing
never shows. The renderer gives such a stage `XGPU_BORDER_TERRAIN_FADE` (`xgpu_vulkan_stream.c`, `border_axes`), and
the shader mixes its texel toward the mean of five lookups of its second level within 2.5 texels (CE's coarser
levels fade toward grey, so they cannot stand in), by `XGPU_TERRAIN_FADE_MOST` (0.25: most of CE's texel stays, so
its mottle, contrast and relief stay under the layers' grain while its blocks soften) where a texel covers four
pixels or more (level of detail -2) and not at all once it covers less than 1.4 (-0.5); the lookups run only where it is mixed in. From mid range
on, CE's ground is exactly its own. `debug.terrain_detail_fade` `false` turns it off, for checking.

The game fades the maps only once the renderer says it loaded the pack: `prism_terrain.c` asks each frame through
the host import `host_vulkan_terrain_pack` (`sdl_platform.c`'s `platform_prism_terrain_pack`, the renderer's
`xgpu_vulkan_terrain_pack_state`: 1 loaded and its layers drawn in the renderer's last whole frame, -1 not there
or not whole, 0 not tried yet for the folder FRAME brought or loaded but not drawn: the class view, a frame the pass
did not reach) and logs each answer once. With no pack, or a `display.terrain_pack_folder` that holds none, CE's detail maps stay
whole, so the ground is never left blurred with nothing over it; other renderers answer -1.

### What is not done in T1

- The blend weight of two-class shaders is not in `extras.z` bits 24 to 31 as the design planned: those bits are
  taken (Raster's CE light code, `bake_glsl.inc`'s `ce_code`). T1 derives the weight in the pass instead (above).
- Objects and static scenery get no layer (their pixels are not the level's).
- The quality tiers are not used yet: the layers are 1024 on every card.
- The settings are not in the settings menu yet.
- `debug.gbuffer_view` `"terrain_class"` turns the pass off (the class view shows the classes, not the layers).

### Cost and checks (RTX 2060)

At 1280x720 in Path Traced (`performance`, the 2060's tier in design section 10), alone on the card at its full
clocks, the pass takes 0.37 ms at B1's 1 m pose (every pixel sand: the layer, its grain and the normals) and 0.37 ms
at its 10 m pose, plus 0.02 ms for the base's copy (`debug.pass_times`: `TERRAIN`, `TERRAIN COPIES`). The grain is
one lookup of the detail alone: hex-tiled with its normal it cost 0.13 ms more. At native 1920x1080 with no
upscaler the pass takes 0.72 to 0.90 ms (plus 0.05 ms for the copy), over the 0.4 ms budget: that is met only at
the 2060's 720p internal resolution until the quality tiers (T15) give a lighter one.

Read PRISM Rebaked's figures with care. Offscreen, Rebaked does not load the card: the RTX 2060 stays at its idle
clocks (900 MHz, its memory at 5000 MHz, about a quarter busy), where every pass reads about eight times its time at
full clocks (the base's copy 0.16 ms against 0.02): that is where an earlier 2.4 to 2.9 ms came from. With a second
instance path-tracing on the card to hold its clocks up, the copy reads 0.02 ms again and the pass 0.49 ms (the
grain then hex-tiled with its normal, as Path Traced's 0.50 ms of the same build), and the frame grows by the pass
at the 10 m pose; at 1 m, where Rebaked's frame is about 1 ms, the frame instead grows by the other instance's time
slice (about 1.4 ms) whenever it passes about 1.05 ms, whatever is added (a copy alone does it), so that figure is
the sharing, not the pass. Diagnostic packs with no normal, no roughness or neither change nothing.

The 60 m view of every T1 pose (B1 to B3, N1 to N3, D1, D2, R1, R2) is within 1.63 ΔE (Oklab x100, the worst
32-pixel tile of one class; medians 0.04 to 0.30) of Classic;
Classic stays byte-identical, PRISM Rebaked with `display.terrain` `"off"` or `display.terrain_layers` `"ce"` is the
previous build's frame byte for byte, b30's bake still loads whole, and the game's simulation is the same with the
terrain on and off (`determinism_compare.py --simulation`, re-run on T1's final package).

d40's sand (`sand_desert`, Ground080) is at strength 1.0, chroma 0 and no grain: its scan is smooth, and with the
second slot fixed (above) d40's shoreline (D2) at 1 m reads as CE's ground with fine relief (std of Oklab L 2.1
against Classic's 2.0, was 3.9), its hue CE's.

## T2 and T3: the ground's light (sand, dirt, snow)

T1 gave the ground its detail; T2 and T3 give it the light of a granular surface and of snow, in PRISM Rebaked and
Path Traced alike (Classic and `display.terrain_layers` `"ce"` are untouched: the HQ layers' pass is what marks the
pixels). Design sections 3.3 and 3.4.

### The class table and the G-buffer's models

`TERRAIN_LAYERS` (`tools/vulkan_post_shaders.py`) now writes, besides the HQ normal, each level pixel's class's
physical row (`TERRAIN_ROUGHNESS`, `TERRAIN_POROSITY`, `TERRAIN_MODEL`; the porosity is `xgpu_vulkan_terrain.c`'s
`class_porosity`, which T6's wetness will read):

| Class | Dry roughness (the layer's variation over it) | Porosity | Model |
| --- | --- | --- | --- |
| sand | 0.85 | 0.6 | `XGPU_GBUFFER_MODEL_GRANULAR` (5) |
| dirt | 0.90 | 0.7 | `XGPU_GBUFFER_MODEL_SOIL` (6) |
| mud | 0.70 | 0.8 | the material's own |
| rock | 0.70 | 0.3 | the material's own |
| snow | 0.60 | 0.2 | `XGPU_GBUFFER_MODEL_SNOW` (7) |
| ice, water | the material's own | 0 | the material's own |

The three new models are written only by this pass and only on the level's pixels, so no material's own model is
ever one of them; on their pixels word 2's metal byte is 0 (the ground is never metal) and its specular byte is the
class's specular scale (0.5, snow 0.45) times the layer's occlusion, which the light passes divide back out
(`TERRAIN_COMMON`'s `terrain_of`). Past the HQ detail's reach (`bounds.z`) the pass no longer discards: it writes the
class's model, roughness (and the snow on rock, below) alone, so the response does not end where the detail does.

`TERRAIN_COMMON` (`xgpu_vulkan_bake_glsl.inc`) holds the response; Rebaked's frame (`raster_source`) and the path
tracer (`tools/vulkan_pt_shaders.py` appends it to `BAKE_COMMON`) both use it.

### The fix first: prism-8's cold patches on sand

At B1 1 m in Path Traced, prism-8's sand showed cold blue-grey patches: the scan's footprint craters (Ground054's
normal) turn facets from the sun, whose cosine then fell to nothing while the blue sky and bounce stayed whole. Two
changes, both physical: the sun's diffuse light on granular and soil pixels is wrapped (`terrain_sun`: sand 0.3,
dirt 0.2, snow 0.5: a facet of grains passes light on past its cosine; Lambert's cosine elsewhere), and the open
sky's, the floor's and the bounced light are scaled by the layer's occlusion (`terrain_ambient`: 85% of it, on sand
and dirt), which the crevices' share of the sky is. The sun's visibility is traced wherever the wrapped term is
not zero. A sand normal of 0.7 was tried against 1.0: the frames differ (mean 10/255) but the cold share is the
same (5.6% both), so the normal stays 1.0 and its relief with it.

B1 1 m, the share of the ground's pixels bluer than Oklab b -0.5 (`tools/cmpsheet.py`'s "cold"): Classic 5.7%
(CE's own sand has blue-white texels, lower left), prism-8 Path Traced 8.3%, now 5.6%: the patches beyond CE's own
are gone.

### T2: the granular look

- **Back-scattering lobe** (`terrain_sun`): on sand (and at 0.6 on dirt) the wrapped sun term is brightened looking
  down-sun: a broad lobe (0.2 of the cosine between the eye and the sun) and the opposition surge about the
  anti-solar point (shadow hiding, Hapke's B0 / (1 + tan(g/2) / h), B0 0.3, h 0.08). Looking into the sun it is
  0.8 of Lambert's.
- **Glints** (`terrain_glint`, sand and snow, design 3.4): a world-anchored grid of cells about twice the pixel's
  footprint (two levels blended by the footprint's octave), each holding at most one facet (a share of the cells),
  its normal drawn from the grains' GGX distribution (alpha 0.45, snow 0.7) about the shading normal; a facet
  reflects the sun to the eye within a cone that widens with the level (its cell holds more grains), the facet's
  share divided out, so the mean is the distribution's: the glints sit still where the camera does, sparkle where
  the half vector crosses a facet as it moves, and fade to the smooth lobe by an octave of 6. A facet lights a
  grain's spot in its cell (about 1.2 mm, snow 0.9 mm, or the pixel's footprint where that is larger, its light
  gathered into it up to four times), not the whole cell: whole cells read as square confetti at 1 m. Seeds are in world
  space and carry no frame's noise. In Path Traced they are added in the composite after the denoiser, by the
  filtered sun visibility (so nothing for the denoiser to smear and nothing to flicker, and none in shade); in
  Rebaked after the light, by the sun's visibility, divided by CE's base where the resolve multiplies by it. One
  model only (a discrete-facet one in the manner of Deliot and Belcour's), not the cheaper Wang-Bowles one the
  design names for `low`/`medium`: the tiers are T15's.
- **Ripples** (`ripple_tilt`): on near-level sand (normal z above 0.86 to 0.97), crests about 11 cm apart across the
  map's wind (an asymmetric profile: stoss long, lee short), their lines warped by the macro map, in patches of it,
  faded where the footprint nears the wavelength and on slopes. Their direction is set from the map's checksum at
  a new generation, then once from the first wind the records bring, and held (`ripple_set`): a turn toward the
  wind over minutes (round 4) moved the lit sand of a still frame (B3's held frames: per-pixel std 0.42 against
  prism-8's 0.012; held now 0.017). `HALO_TERRAIN_RIPPLES` scales their height (0 off), for checking.

### T3: the snow look

- **Subsurface**: snow's sun term wraps further (0.5) and what reaches a face turned from the sun is blue-weighted
  (ice absorbs the red); its back-scattering lobe is weak (0.15).
- **Diffusion** (Path Traced, `snow_diffused` in the composite): a screen-space pass of Burley's kind, an exponential
  a channel (red 0.45 and green 0.7 of the blue's 2 cm reach), two opposite taps along x on one parity of the pixels
  and along y on the other, on snow of the same plane only; none where the reach is under 0.75 pixel. Only the light
  that varies is carried (the denoised light and the sun's; the clean sky and floor are even over a snowfield), and
  the taps' sun visibility is read first: where it is the pixel's at both (inside a sunlit or a shaded field) nothing
  more is read. So the sun's light crosses a shadow's edge, a ledge's or a print's rim, and costs little elsewhere
  (four taps reading whole surfaces cost 0.22 ms at N1 1 m; this, 0.04).
- **Hue**: snow's diffuse light keeps CE's blue-white (`terrain_snow_tint`, luminance kept): the traced frame's warm
  sun took b40's snow from Classic's Oklab b -1.5 to -2.2 (x100) to -0.2 to -0.7; the tint (0.97, 1.0, 1.06 over its
  luminance) puts it back near Classic's (0.95, 1.0, 1.1 overshot to -2.5 to -3.2).
- **Sparkle**: the glints above, snow's: smaller cells (0.0025 world units), more of them held (0.3), rougher
  facets, a lower F0 (ice's 0.018).
- **Snow on the rock's tops**: on a map with snow (`gamut.x`: the class table names a snow material), the faces
  turned up (normal z 0.74 to 0.9) of a material of rock alone (b40's and c40's stone) take snow where CE's lightmap
  light (the G-buffer's code: Tool's radiosity left overhangs and ledges under cover dark) says the sky reaches,
  settling first where the rock's height is low among them (cracks, hollows); the colour is CE's snow's blue-white
  (`gamut.yzw`), the class becomes snow (its model and roughness), the normal mostly the surface's. Past the HQ reach
  it is the class and colour alone. Never on a material that blends rock with another class: there the rock's texels
  are CE's painted dirt in the snow (b40's brown streaks), which an earlier build covered in hard-edged pale patches
  (`sheets/r7_N3_60m.jpg`).

### Cost (RTX 2060, 1280x720, Path Traced `performance`, alone on the card)

Medians of 300 frames a pose (`HALO_PT_TIMES_FILE`, `debug.pass_times`), prism-8 (two runs, averaged) against
this build:

| Site, pose | Path Traced passes (ms) | of them the composite | `TERRAIN` | T2/T3's own (composite + `TERRAIN`) |
| --- | --- | --- | --- | --- |
| B1 sand 1 m | 12.18 -> 12.11 | 0.29 -> 0.31 | 0.37 -> 0.44 | +0.08 |
| B1 sand 10 m | 12.23 -> 12.35 | 0.29 -> 0.30 | 0.37 -> 0.44 | +0.08 |
| B3 sand 1 m | 13.49 -> 13.52 | 0.29 -> 0.30 | 0.37 -> 0.41 | +0.06 |
| B3 sand 10 m | 12.27 -> 12.42 | 0.28 -> 0.30 | 0.36 -> 0.41 | +0.07 |
| N1 snow 1 m | 20.56 -> 20.63 | 0.28 -> 0.31 | 0.31 -> 0.36 | +0.08 |
| N1 snow 10 m | 16.72 -> 16.95 | 0.28 -> 0.31 | 0.33 -> 0.39 | +0.09 |

The light pass (the sun's wrapped term, the ground's ambient share) adds up to 0.15 ms at the 10 m poses, but it
moves by about 0.1 ms between runs of builds that differ only in the HQ layers' pass (N1 10 m: 12.73 and 12.87), so
the whole frame's change is +0.12 to +0.24 ms at 10 m and within the noise at 1 m: T2 within its +0.25 ms, T3's own
work +0.09 ms (the frame's +0.07 at 1 m, up to +0.24 at 10 m with the light pass's noise).

### What is not done

- The glints are not written to the upscaler's reactive mask (`xgpu_vulkan_upscale.c` finds the mask from the
  transparent surfaces' change): they are world-anchored and stable, and the held and panned frames show no
  flicker without it.
- Wet sand's and snow's changes (roughness 0.30, packed snow) wait for T6's wetness and T4's prints.
- The ripples do not drift downwind in a strong wind.
- One glint model on every tier (T15's tiers).

## T4: the prints (PRISM Rebaked)

With `display.terrain_tracks` on (the default) and the HQ layers drawn (`display.terrain_layers` `"hq"`, the pack
there: the prints are drawn by the layers' pass), every biped's feet press prints into sand, snow, dirt and mud, and
they stay for the level. Render only: nothing of it reaches the game.

### The feet

`EVENTS`' feet (T0's hook in `biped_make_footstep`): walk, run, slide, shuffle, jump and landing, the left or right
foot, the species (by the biped's tag name), the ground under the foot (the footstep's own ray: its hit, its normal
and CE's material type) and the way the body faced. CE makes a footstep's effect only within 10 world units of a
local player's camera; T4 finds the feet out to 40 (above, "What the hooks may do"), so the Marines' and the
Covenant's prints are made where the player is not looking. Only sand (CE's material type 1), dirt (0) and snow (3)
take prints (a curated mud class has no material type of its own on the collision: it takes dirt's); stone, ice,
metal and water none, nor a slope past 40 degrees, nor a Sentinel.

### The pages (`port/linux/vulkan/xgpu_vulkan_terrain_prints.c`)

A sparse pool of world-space pages (design 2.3): a page is 128 squared texels of 2 cm, 126 of its own and a texel of
its neighbours' about it (so the pass's linear lookups meet across a page's edge), 0.827 world units a side, made
where a foot first presses. Each texel holds the displacement (world units, negative down, a rim positive), the
compaction, and the ground's height (over the page's own; with the coverage): a pixel takes a print only where its
own height is within 0.15 to 0.3 world units of the texel's ground, so a bridge over a print, or a floor under it,
shows none. The pool holds 384 pages (b30's opening battle on the first beach presses about 250 in a minute); when it
is full the page farthest from the camera that has not been pressed in 5 seconds is let go. A new generation (a new
map or BSP, a revert to a checkpoint) forgets every page: the beach is clean again, as the game's state says it is.

The host keeps the pages (the displacement as floats, the compaction and coverage as bytes, the ground's height in
1024ths of a world unit: 144 KB a page, 54 MB for a full pool) and sends a page's changed rectangle of its first
level, and its two smaller levels whole, as half floats into a 2D array (`RGBA16F`, 384 layers, 3 levels: 64 MB of
the card's). A page table (64 x 64 pages about the camera, toroidal: a page at x, y modulo 64, its x, y and height
beside its layer, so the pass checks it is the page it wants) is sent again when the camera's page or the pages
change.

### The stamps

Our own, made in code at start (`stamp_make`), 64 squared each, one a species, each a dent (0 to 1) and a broad, low
rim about it (the dent spread 6 cm, the outside alone, its highest 1):

| Species | The sole | Length | Depth's scale |
| --- | --- | --- | --- |
| Human (the Chief, Marines, crew) | A boot: heel, shank and ball, lugs as chevrons 4.5 cm apart | 33 cm | 1.15 |
| Elite | Two long toes splayed forward, the pad behind them, a heel spur | 42 cm | 1.3 |
| Grunt | A broad pad and three stubby toes | 24 cm | 0.85 |
| Jackal | A bird's: three thin toes fanned forward, one behind | 22 cm | 0.7 |
| Hunter | A great flat foot, its front ridged | 62 cm | 1.7 |
| Flood | A ragged human sole | 30 cm | 0.95 |
| Sentinel | None (it floats) | | |
| Another | An oval | 28 cm | 1.0 |

A right foot's stamp is the left's mirror across the foot's axis. The ground sets the depth and the rim (a share of
the depth): dry sand 3.5 cm and a rim 0.35 of it, wet sand 1.5 cm and 0.15, snow 10 cm
and 0.06 (it packs), dirt 0.8 cm and 0.2, mud 3 cm and 0.35 (design 4.1). The walk sets the rest: a run digs its
toe 15% deeper, a jump's push-off its toe more, a landing goes half again as deep and scuffs (the stamp 20% longer),
a slide stretches it to twice its length, a shuffle presses half as deep. A foot is wet sand's (shallow, crisp) when
a wader (a unit in water, or out of it for less than 10 seconds: `WADERS`) stands within 1.5 world units of it.
A stamp takes the lower of what is there and its dent and the higher where it raises a rim, so a foot sent twice
(sub-steps, co-op's prediction) makes the same print.

### Relax and infill

For a second and a half of the game's time after a page is pressed, the pressed region slumps toward the ground's
angle of repose (thermal erosion, three passes a frame, each moving a fifth of a step past the angle downhill): sand
56 degrees, wet sand 72, snow 68, dirt 50, mud 60. Sand is not loose dry sand's 34 degrees: a beach's sand is damp
under its surface and a boot packs it, so a print's wall stands steep. At 34 degrees every print rounded flat under
b30's sun (35 degrees up) and cast no shadow at all; at 42 a boot's 10 cm sole, five texels of 2 cm, slumped into a
V-shaped trough with no floor (a thin dark slit in the frames). So a dry print's walls round off a little and a wet
print stays sharp.

Then, about every two seconds of the game's time while a page is within 70 world units of the camera, it is filled
in: its displacement sinks toward flat and a blur softens its edges, by the ground and the wind at the eye
(FRAME's): dry sand in b30's wind (0.8 world units a second) keeps about two thirds of its depth after two minutes
(its walls softened: an old trail still reads as one; at half the depth and twice the blur the first frames lost the
trail entirely), dirt and mud keep nearly all of theirs, snow fills in only while it snows (T12); wet sand at 0.4 of dry's rate; the
compaction outlives the displacement. A page out of range catches up when it comes back (its sinking by the time
gone). While the game is paused nothing moves (FRAME's step is 0).

### Drawing them (the HQ layers' pass, `TERRAIN_PRINTS`)

Where a page lies in the table's window, the pass is the prints' variant (`tools/vulkan_post_shaders.py`: the
layers' source with the parts marked `PRINTS`; without them its words are exactly T1's, so with no print about,
or `display.terrain_tracks` off, the frame is T1's). For each pixel of a ground that takes prints (sand, dirt, mud,
snow) within 25 world units:

- **Parallax**: the view ray is marched through the field from the highest rim to the deepest floor (snow's prints
  to 18 cm, the rest's to 9 cm), in linear steps of about a pixel's footprint (4 to 24), the crossing halved thrice
  between the last two steps and then refined by the secant; the layers are looked up there
  (CE's own colour stays the pixel's), so the scan's grain lies down in the print. Three lookups (the pixel, and the
  ray's two ends) decide first whether there is any print about: a ground with none costs three texel fetches.
- **The surface**: the normal is bent by the field's slope at the point met (over the layers' normal, which a
  pressed floor smooths), the floor darker (the damp sand under the dry, to 0.8; snow packed greyer and smoother),
  the rim's kicked-out grains a little lighter (to 1.07), the layers' grain and relief a quarter where the foot packed
  it.
- **The micro-shadow**: the field is marched toward the sun (the loaded bake's sun, the one Rebaked's light pass
  lights by; SCENE_LIGHTS' where no bake is) over 27 cm (0.09 world units) in 8 steps; what stands
  over the ray shades the point, softly over three quarters of a texel. With the cavity's occlusion (the floor under
  the field's mean about it, up to 30%) it is written into a third target, the micro-shadow target (RGBA8, the base's
  size, cleared to lit each frame the prints are drawn), which PRISM Rebaked's light pass reads
  (`xgpu_vulkan_bake_glsl.inc`'s `raster_source`, binding 6): the sun's visibility of a level's pixel times red, the
  rest of its light times green. Where no print was drawn this frame the pass reads a white texel.
- The lookups take a level of the pages by the pixel's footprint (2 cm, 4 cm, 8 cm), so far prints do not shimmer.

`HALO_TERRAIN_PRINTS_VIEW=1` shows the micro-shadow (red), the cavity (green) and the depth (blue) over the lit
frame; `HALO_TERRAIN_NO_PRINTS=1` draws the plain pass; `HALO_TERRAIN_PRINTS_LOG=1` logs every stamp (the first four
are always logged, and every 20 seconds of the game's the stamps and pages).

### Path Traced

The pass is the same in Path Traced, which takes its base colour and normals: the prints' parallax, floors and bent
normals show, lit by the trace. Their shadows are not traced yet (Path Traced does not read the micro-shadow target):
the ray-origin offset and the short march of the field for every light are T5's.

### What is not done in T4

- Wet against dry comes only from waders (a foot near a unit in or just out of water): T6's wetness writes the rest.
- No vehicles' tracks, no craters, no resting bodies (T5); impacts and explosions press nothing yet.
- No prints without the HQ layers (the pack missing, or `display.terrain_layers` `"ce"`): the prints are the layers'
  pass's.
- The ground grid (design 2.3: the level rasterised top-down into layers at BSP load) is not built: a page belongs
  to the ground its feet stood on, and each texel keeps its ground's height, which does the grid's work for prints
  (a bridge over a print shows none); the drifts (T11) will need the grid itself.
- A page under a bridge and one on it share their texels where both are pressed (the later foot's ground wins
  there): rare, and only where both are walked.
- The quality tiers are not used: 2 cm texels and 384 pages on every card.

## T7 and T8: the water layer (Rebaked; Path Traced's traced rays)

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
| `END` | | The base map and the mask (the alpha) read where the game drew them (the colour target, no copy); the layer composited (`xgpu_vulkan_water.c`, a compute pass) and copied into the colour target, what was behind put back wherever the captures drew outside the water. Where it cannot run (no G-buffer of the window's), what was behind is put back and the water is not drawn (logged) |

A water CE draws before the fog (c10's swamps) has its colour draw as the environment is drawn, before PRISM's frame
is resolved: there nothing of it, not even its depth (CE's colour draw writes none, and CE's environment fog then
fogs the floor under the water as Classic's does: a depth there would keep that fog off the water's pixels, and the
swamp's milky lavender with it), and the layer at its queued draw after the resolve, which lays CE's fog over its
surface (below).
`WATER_DRAW`'s item carries the step, the shader's flags, the group's plane, CE's ripple scale and tints (straight on and
edge on) and the window's planar fog (its colour and density, depth and distance, the eye's height over it).

### The layer (`xgpu_vulkan_water.glsl`)

Two dispatches of one compute shader. The first runs at half the resolution, one texel a 2x2 block of the frame (at
its water pixels' mean position, with CE's ripples' mean there): the reflection and the sun's visibility, the
expensive part, written premultiplied by the block's water share so that the second pass's linear filter of them
takes the water's texels alone. The second runs at every pixel. In light (the frame's values to 2.2):

- **The normal**: the plane's, bent by CE's ripples (flattened edge on and far off, where the 8-bit map would alias).
- **Fresnel** at an IOR of 1.33 (Schlick, F0 0.02), a little less edge on far off (a rough sea's facets shadow and
  mask the grazing reflection).
- **Reflection** (first pass): marched along the screen against the G-buffer's distances (refined by halving, faded
  at the view's edges, only surfaces above the water; 18 steps near, down to 10 longer ones far off, each reading the distances alone, where the
  footprint's blur hides what the march misses); else what lies far along the reflected ray where the frame shows it
  (the sky, or far surfaces in their fog); else CE's reflection cube in CE's tint. Blurred over the roughness's
  footprint: an ellipse (nine taps, seventeen when wide) growing with distance and most edge on, up to 2.4 times as
  tall as wide; far off and edge on half of what the screen gives is CE's cube at its widest blur. The second pass
  puts CE's ripples' own pattern back over the half resolution's reflection: CE's cube along the pixel's reflected
  ray against the block's (so every one of CE's ripples still shows). CE's base map alpha, where it modulates CE's
  reflection (b30's mask by the shore), modulates it.
- **The water's body: CE's own colour of its water.** Under the reflection, what a deep water shows is what Classic's
  draw gives the pixel (`ce_water`): what is behind (by CE's base map's colour where that modulates the background)
  plus CE's cube along the pixel's reflected ray in CE's tint (CE's group tint, between edge-on and straight-on by the
  camera's forward against the plane, as `rasterizer_xbox_water.c` sets it; by CE's view alpha where the base's alpha
  modulates the reflection), held at white as CE's frame was. So b30's open sea keeps CE's blue and CE's ripple
  pattern, and the reflection lies over it. T7 and T8 used CE's planar fog's colour under the water instead, and the
  sunlit sea read 28% darker and grey-green.
- **Refraction**: the floor the refracted ray reaches (Snell, found on the screen, the pixel's own where it is not), its
  colour absorbed over the path by Beer and Lambert, in CE's tint's colours (the base map's where it tints the
  background, else the straight-on tint's hue), toward the body. Where the frame has no floor (the far sea, past the
  level), the body alone (it was what CE drew there: the dark slate strip under b30's horizon).
- **Far off and edge on, CE's own colour of its water** (rising from a grazing roughness of 0.04 to 90% of it at
  0.6): a far rough sea reflects the sky's broad light, which CE's art gives as its flat pale teal, not the clouds'
  stretched band.
- **The sun's highlight** off the ripples (GGX, a wide lobe: roughness 0.22, 0.38 far off, at 0.3 of the sun's
  tuning: CE's ripple facets glint softly, not as white plates), from SCENE_LIGHTS' sun, in the shade of what stands
  between the sun and the surface: the sun's ray marched along the screen against the G-buffer's distances (Rebaked;
  in the first pass, and only where the block's highlight, its lobe widened, can be lit), traced in Path Traced.
- **CE's fog over a water drawn before the fog** (c10): the frame behind already has CE's planar fog over the floor
  (Classic's own, of the right shape: it lies over the banks and thins toward them). The share it took of each pixel
  is estimated (CE's fog colour's part of the frame there: the floor under a swamp is dark), the floor without it is
  what the water is composed over (refraction, body, reflection), and CE's fog is laid back over the water by that
  share. The fog's shape is then Classic's per pixel, not only its mean (`tools/c10shape.py` in the run folder: the
  shape's check, by gradients, the shore's edge and a windowed structural similarity).
- **The shore**: the layer fades into the ground by the water's depth over it (the G-buffer's floor under the plane):
  over 0.3 world units, 0.8 under CE's fog (T7's 0.05 left c10's water a hard-edged sheet).
- **From below**: Snell's window and total internal reflection outside it. The window's edge is decided by CE's
  ripples smoothed over a few pixels and drawn toward the plane's normal, Fresnel's reflectance (exact, water to air)
  rising to 1 at the critical angle and blended a little past it: a soft, gently moving ring. The window's ray is bent
  by the pixel's ripples (mostly), and shows CE's cube in its tint sharp away from the zenith and blurred toward it
  (the cube's zenith, which Classic never shows from below, smeared by the ripples into a dark streak) in Rebaked, the
  traced sky in Path Traced, with 60% of CE's cube along the reflected ray over it (Classic's surface from below: its
  blue and white ripple pattern). Outside it, the water's body as the frame shows it along the reflected ray (CE's
  fog under the surface; off the view, the frame's bottom edge where that is not the surface; else the deep colour),
  80% CE's cube sharp in its edge-on tint and CE's ripples' pattern over it; a little of the water's body over long
  paths. CE's own planar fog (b30's: opaque in 0.15 world units) never fogged CE's water's surface, drawn after it,
  and would hide the window.

**An opaque water** (d40's: an environment shader of the water class, `classes.json`): no capture and no refraction;
after the resolve, its pixels (the level's of a material of the water class) take reflection, Fresnel and the sun's
highlight over CE's own colour of them, the normal from its own bump. A muddy water reflects little and tints what it
does: the Fresnel at 0.55 of a clear water's (at most 0.3), the reflection half in CE's colour's hue (its luminance
kept), the bump flattened most edge on (where its texels streaked the reflection vertically) and the footprint wider
than tall edge on, and the reflection held under 1.35 times the body's light (the bright clouds' reflection near
the shore stood as pale vertical columns of haze, D2a; Classic gives this water no reflection at all); the
sun's highlight faint and wide (d40's overcast); the sun's highlight in the screen's shade; and the shore: the layer fades out where the water's own pixels
thin out on two rings about the pixel (no hard line where the water class meets the sand). In Path Traced it is drawn
as in Rebaked (no traced rays yet).

Tuning, for one run (the defaults are the look judged against Classic by pictures): `HALO_WATER_RIPPLES` (0.35),
`HALO_WATER_ABSORB` (0.25 a world unit), `HALO_WATER_SUN` (1.5), `HALO_WATER_DEEP` (1), `HALO_WATER_CUBE` (1), and
`HALO_WATER_VIEW` (1 the ripples, 2 the base map, 3 the mask, distance and side, 4 the reflection, 5 the refraction).

The bake is untouched: water stays out of SCENE_STATIC and the bake's scene, and a bake made by the build before loads
whole.

### T8: water in Path Traced

In Path Traced, at a group's `END`, before the composite, Path Traced's pass `"water"` (`xgpu_vulkan_pt.glsl`,
`xgpu_vulkan_pt_water`) traces two rays for each 2x2 block of the frame with water (at half the resolution, the
composite's first pass's texels: at the block's water pixels' mean position, with CE's ripples' mean there) through
the frame's own scene (the frame's job: its camera, exposure and curve; its own descriptor set, the frame's images
with the water's in four places), from the surface the composite computes (the plane, CE's ripples at the
composite's strength):

- from above, the reflection (the hit lit from the bake as Path Traced's reflections are, `hit_light`) and the sun's
  visibility at the surface (the sun's ray, the structure's and the moving objects'); the refraction into the water,
  to the floor, with its length;
- from below, Snell's window's ray up through the surface (the real sky, the cliffs, the ring) and the reflection back
  into the water (total internal reflection outside the window), with its length.

Both are written in light (the frame's encoding, with the exposure and the tone curve of the frame's composite, to
2.2), premultiplied by the block's water share (the upper ray's by the share where it met the level), with the sun's
visibility, the upper ray's share that left the level and the lower ray's length beside them, and the composite's
first pass takes them in the screen march's place: the reflection filtered over the roughness's footprint on the
water's own texels (premultiplied: nothing of the shore or of what stands in the water is taken into it), where the ray
left the level the sky as the frame shows it (CE's sky, else CE's cube: the scene's sky is the light's radiance, not
the picture of the sky, and read as a dark purple sea); the highlight by the traced visibility; from below the window's
light (the sky where it left) and the internal reflection absorbed toward the water's body. The refraction keeps the
frame's floor found along the refracted ray on the screen, which Path Traced lit at its own pixels, with CE's planar
fog under the surface and what CE draws there (b30's surf: a traced floor has neither, and showed a bare sunlit sand
bed CE's art hides), and takes the traced floor where the frame has none (CE's fog beyond the world), CE's planar fog
put over it by its depth. One ray pair a block and no jitter:
the hits are lit from the bake, so the rays carry no noise of their own beyond the sun's penumbra, and the water needs
no history (CE's ripples move every frame). With `debug.pt_reference` the pass traces the reference's paths
(`reference_path`: no bake) instead, over the same footprint's directions (the normal turned by a Gaussian of the
filter's angular size), summed in linear light while the reference sums, and the composite takes the means unfiltered:
the water's reference.

**What the S2 gate checks, plainly**: it is a self-consistency check. The reference is the same layer's composite
with the same rays converged and unbiased (`reference_path` in place of the bake's light at the hits, the footprint's
directions sampled in place of filtered); it shows that the live water's filter and bake-lit hits do not stray from
what their own converged rays give. It does not compare the water with Classic, with a real ocean, or with a wave
model beyond CE's ripples: the look against Classic is judged by the pictures and the checks named above.

**Cost**: the first pass at half the resolution (a quarter of the rays, marches and filter taps), the screen march's
steps fewer far off, the sun's march only where its highlight can be lit, the base map's capture read in place (one
copy of the colour target fewer). The figures are in the run folder's handover (`v4_water_runs/HANDOVER.md`).
