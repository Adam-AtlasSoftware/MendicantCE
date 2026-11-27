# PRISM's resolved geometry

One geometry per surface and object, which every pass draws: the view, the shadow maps, the world occlusion's
pre-pass, the traced scene (PRISM Rebaked's object sun, every ray of PRISM Path Traced) and the bake's frame scene.
Before it, each of these chose a shape of its own, and the remodels flipped between levels and between the remodel
and CE's shape, and were lit by shadows of other shapes.

The resolver runs under PRISM Rebaked and Path Traced only. Classic lighting is the original look and draws as it
always did: the game marks each object drawn under Rebaked or Path Traced (`XGPU_STREAM_MESH_INSTANCE_RESOLVED`) and
the host makes Classic's own level choices without it; Classic's world shadow and occlusion settings, their menu rows
and `display.prism_rt_world` are as they were; the skin buffer is Classic's 128 MB ring with nothing kept when the
map's set was sent under Classic (the resolver's ring otherwise, 256 MB on a card of 10 GB or more and 192 MB on a
smaller one, and a 32 MB keep region; it follows a lighting change at the next map's set).

## Where the world geometry comes from (the sidecar, v4 geopatch M2)

The enhanced world geometry ships as a sidecar of our own beside the player's retail map,
`<data root>/maps_enhanced/<map>.prsx` (`port/linux/game/prism_prsx.h`): per BSP a GEOM section (the remodel's
vertices, triangles, runs per stock surface and far levels) and an LMUV section (its lightmap coordinates on our own
pages, v2 below). The retail
map opens from `maps/` unchanged. At each BSP's load `prism_world_source.c` reads and verifies (SHA-256) the BSP's two
sections and `prism_geom.c` decodes them into the image `prism_world_geometry.c` has always drawn, resolving every REF
op from the map's own tags with the exact integer decode (`prism_ref_decode.c`); the resolver's near/far choice, the
shape checks and the draws are unchanged. The `.pwg` files are the remodel pipeline's intermediate on the farm, which
`tools/geopatch/encode.py` turns into the sidecar; the game no longer reads them. The packed vectors and lightmap
vertices are rounded as the game's compressor rounds them, so the GPU draws the same values as from the `.pwg`; the
positions move onto the 1/1024 world unit grid (b30: at most 0.005 world units, the snapped stock positions
included).

Which world a map draws is the ladder (`prism_world_ladder`) of the map's state and the frame's lighting, asked every
frame. The map's state is chosen once: World Geometry as the map loads (a change applies at the next load), and, at
its first frame under Enhanced or Path Traced (the load, or the first change from Classic in the map), its
`classic_world_id`, its sidecar opened and verified, held for the rest of the map whatever the lighting and the
packs, and its enhanced packs looked for. So every change of lighting in a map, in every direction, draws at once
what a load under that lighting would, with one exception kept from before: a map loaded under Classic and changed to
Enhanced keeps its classic geometry, its bakes keyed by its checksum (Enhanced and Classic keep their rules); only Path
Traced draws the enhanced geometry in it, and the rest of its state is chosen at its first Path Traced frame:

| Rung | Geometry | Lighting | When |
|---|---|---|---|
| 1 | Enhanced (the sidecar) | Path Traced | World Geometry Enhanced and the sidecar verified: nothing else, never a bake's or a pack's state |
| 1 | Enhanced (the sidecar) | Enhanced, from the enhanced packs | World Geometry Enhanced, the sidecar verified, and every BSP's enhanced pack shipped (`prism_bake_shipped/remodel/<map>/bsp<n>.pbk`, named in its `manifest.txt` with `geo:prsx:<world_id>`) and not refused by the renderer |
| 2 | Classic | Enhanced or Path Traced, from the classic packs | otherwise |
| 3 | Classic | Classic | Classic lighting |

Path Traced's geometry and lighting never depend on a bake. It reads no lightmap of the enhanced world but traces the
bake's scene, which the renderer makes for the BSP whether or not a pack is loaded (a player's bake is then paused,
nothing baked), lit by the bake's light where it has it and by its own bounce else. When the geometry changes in a
map (a change of lighting to or from Path Traced swaps it), SCENE_STATIC's set is begun again in play; under every
lighting a set with no whole bake of its key makes its own scene, nothing baked, and is never drawn with the bake
drawn before it (another set's, often another world's: below), and neither Path Traced
(`xgpu_vulkan_bake_lookup`) nor Enhanced (`xgpu_vulkan_bake_raster`) draws a bake of another set than the game's
now, so while the set's own scene is made each shows CE's frame of the geometry drawn, never the old scene. The path tracer's history and the denoiser plugin's are dropped across a new scene (its generation, not only its address).

Enhanced lighting needs the packs: a map without them draws its classic geometry under Enhanced (and the enhanced
under Path Traced, back and forth as the lighting changes). A pack the ladder found but the renderer did not load
whole (its manifest names the BSP's world_id but its own key is another geometry's, or the file is damaged: the
player's bake of the BSP is then paused or failed) drops Enhanced lighting to rung 2 for the rest of the map, with a
warning in the log (`prism_world_source_bake_refused`, from `prism_bake_frame`); Path Traced still draws the enhanced
geometry. The ladder reads the packs and manifests by their host paths, as the renderer and the menu do (an absolute
`display.bake_shipped_folder` was read under the data folder). The log says what is drawn each time it changes
(`prism world: <map>: drawn now: ... geometry, ... lighting (rung n)`).

Classic lighting never draws the enhanced geometry (its lightmaps on the remodel are the rock bug), and a map loaded
under it opens no sidecar until the lighting changes to Path Traced; switching to Classic in a map draws the map's own geometry at
once. Each BSP's enhanced `world_id` (`prism_identity.h`) is computed from the opened sidecar's GEOM and LMUV
sections and sent with the bake's control (`xgpu_stream_prism_bake.world_id`); the baker keys the bake to it (its key's
world_id, `docs/prism_bake.md`, "The frozen bake"), so an enhanced bake is used only for the geometry it was made from.
The classic world's bakes are keyed alike, to the classic world_id of the map's `classic_world_id` (the farm's hash of
the map version's bake inputs, `tools/geopatch/classic_world.py`), which the game looks up by the player's map's
`base_id` in the shipped manifests (`world <map> <base_id> <classic_world_id>`, until M7's signed manifest); a
sidecar whose header names another `classic_world_id` than the map's is refused (rung 2). `debug.world_unbaked`
(`HALO_WORLD_UNBAKED=1`) takes rung 1 under Enhanced without the packs: the farm's (to bake them) and the developers',
never a player's.

### The enhanced world's own lightmap layout (LMUV v2, geopatch M4)

M2's LMUV (v1) carried each remodel vertex's CE lightmap coordinates, so the enhanced bake was laid out through CE's
charts: retail-derived data, with no room for an overhang, a cliff's underside or a trunk. v2 is our own layout,
made on the farm from the remodel's own triangles (`tools/geopatch/lmuv.py`, written by `encode.py` by default):

- Each near triangle faces the way its stock surface's remodel faces on the whole (the nearest of 26 directions to its
  near triangles' area-weighted normal; a facet turned more than about 72 degrees from that way takes its own), and the
  near triangles of a part that share a direction and a 32-world-unit cell of its plane and touch (a shared corner
  position: the remodel's vertices are split along CE's old seams) are one chart, laid flat along the direction. A far
  triangle takes the chart of the nearest near triangle of its own surface facing its way, so a surface's near and far
  levels read the same texels and its level switch changes no light. b30: 28,809 and 27,723 charts, area-weighted
  stretch 1.03 and 1.02, 0.3% and 0.007% of the area stretched past 2.
- A vertex used by triangles of several charts is copied (a seam), and vertices equal in all their drawn attributes
  are one within a chart; a part past 65,535 vertices is cut along its surfaces (its far ranges kept with them).
- The charts are packed on pages of 1024 texels a side at one texel a world unit, two texels apart, each material's
  charts on one page (b30: 3 and 2 pages; the last page's height cut to what it uses). The bake's atlas measures each
  chart and scales it to its quality's density (MAX's tier 0: 16 texels a world unit, a virtual page of 16384, its
  largest).
- The incident radiosity a lightmap vertex carries (CE's lightmap pass's bump term) is not stored: each vertex takes
  its own normal. Enhanced and Path Traced light the enhanced world from its bake.

The game (`prism_geom.c`) decodes v2 into the image with flags bit 1: each part's page (+ 1) in its record's bytes 48
to 51 and a page table after the parts. A material's parts share a page (the decoder refuses otherwise). The own pages
follow the BSP's own lightmap pages: SCENE_STATIC sends their sizes after the BSP's (`pages_send`) and gives a
remodelled triangle page (bitmap count + own page) (`page_of`), and the G-buffer's page for a remodelled material's
draws is the same (`prism_world_geometry_chunk` sets it through `prism_deferred_page_override` before the material's
draws, and puts the lightmap pass's own page back for the next stock material), so the bake's atlas, Raster's chart
maps and Path Traced's lookups all see one layout. A material whose shape check fell back to stock surfaces (whose stock
corners carry CE's coordinates) stays stock whole under v2 (b30: none). The G-buffer's page is 7 bits: the BSP's pages
and the own pages together stay under 127 (b30: 26 + 3 and 29 + 2). The sidecar shrank from 89 to 80 MB.

CE's own lightmap pass still draws the enhanced world, but under v2 it reads the BSP's stock page at our coordinates
with each vertex's normal as its bump term: its light there is no light at all (b30's beach comes out orange, red and
yellow). Enhanced lighting therefore never falls back to it on a texel the bake has a chart for but no texel: such a
pixel (the whole of b30's first BSP's 646,021 tiles is not loaded until 25 s after its bake starts on a 3090,
the shipped pack streaming in 2,048 tiles a frame) is lit as an object is, by the bake's volume, or, while the volume is
not loaded either, by the open sky's light (the rig's sky, by the share of the sky its normal faces), with the sun
traced (`raster_source`'s `unbaked`). Stock geometry takes the same fallback for such texels.

Where no bake lights the enhanced world at all (`debug.world_unbaked`'s rung 1, a shipped pack not yet loaded, Path
Traced before the bake's scene is made), the frame is CE's: its lightmap pass draws a remodelled material on an own page
with the fullbright mode's combiner and a neutral grey (`PRISM_OWN_PAGE_NEUTRAL_LIGHT`), and the specular lightmap and
reflection mask passes skip it (`prism_world_geometry_own_page_draw`, `rasterizer_xbox_environment.c`): flat, never the
stock page read at our coordinates.

## Objects (`display.prism_models`)

The resolver is `port/linux/vulkan/xgpu_geometry.c` (pure C: `tools/test_geometry_resolver.py`). The mesh store
(`xgpu_vulkan_meshes.c`) asks it for each object drawn (`MESH_INSTANCE`):

| Decision | Rule |
|---|---|
| Key | The object (its datum) and its mesh, in a hash map aged after 1,800 frames unseen. Before, a 256-slot direct-mapped table of last frame's levels, which b30's 230 rocks and the cast shared: objects evicted each other, and a frame not drawn forgot the level. |
| Key | (Objects of no datum, the game's 0xffffffff: their mesh and the 2 wu cell of their place; one moved into a cell beside takes its decision along. Before, they shared one entry per mesh.) |
| Variant | Stock until the store has the mesh in, then the remodel for the rest of the map: the store keeps a mesh drawn once (`xgpu_mesh_store_pin`), so a budget never sends an object back to stock. A new set of meshes (the tier or the mods changed) forgets every choice. Nothing waits for the skin ring within a frame: a draw that finds it full is skinned at a coarser level of its remodel, never drawn stock (Skinning, below); a traced part whose skinning fails is traced at a coarser level of its remodel, else left out, never as CE's part; the instances' 4096 slots by id keep a frame's instances apart, so a part's instance is not lost. Each of these is counted (stock fallbacks by channel, ring waits and fills, lost parts, traced left out). |
| Level | Once a frame per object, by the first of its draws: the view's size, or for a shadow drawn first or an object off screen, the size the view would have (its last size in the view times its distance then, over its distance now). The hysteresis (`display.prism_lod_hysteresis`) holds against the level it had, however many frames ago. Every shadow cascade draws that level. |
| Drawable floor | A level of fewer than an eighth of the triangles of the level before it is not drawn; the ladder stops at the level before. The kit's rocks (built 3, 1, 0 subdivisions: 1,984 / 124 / 31 triangles, 16x a step, the last CE's own facets) are drawn at level 0 alone; `rock_remodel.py` now builds quarter steps that never reach the cage. |
| Traced level | The drawn level within 40 world units of the eye (or where the distance is unknown), on every frame, whatever the budget. Beyond 40 world units, a coarser level of the same remodel where the traced triangles' budget (75% of the window's 2.25 million) needs it, shared out at the frame's start over the objects drawn the frame before: the farthest made coarser first, the nearest made finer first while the total stays under 85% of the budget (so the choice does not hunt). Never stock, never left out. Before, each draw was traced in draw order as the remodel, its cage, CE's model or nothing, by a running triangle count. `display.prism_rt_objects` `"view_lod"` is no budget, `"low_lod"` the most coarse level each may take. |
| Skinning | A still object (no rig) whose skinning (mesh, level, matrices, wide stream) is seen again on a later frame is skinned once into a 32 MB keep region (b30's pans kept at most 3 MB) and read from there frame after frame (`xgpu_vulkan_mesh_skin_kept`). The rest are skinned each frame into the ring (256 MB, or 192 MB on a card under 10 GB: a 6 GB card's), and a frame takes at most a quarter of it, the room for the frames the GPU may still be drawing; the resolver holds the frame's objects within that share: past that budget the farthest are drawn a level coarser of their drawable ladder (one level a pass, the objects whose level has not changed within the flip window first; a level let go finer only 150 frames after the budget took it and twice the flip window after its last change). A frame past its share cuts the resolver's skinned budget (by a sixth, grown back a tenth of a percent a frame), so the levels step coarser with their hysteresis, never draw by draw (refusing the draws past the share stepped them draw by draw, and flipped 14 objects on b30's pan). Nothing within a frame waits for the ring: the next frame's share is made free at the frame's end, by waiting there for the oldest frame's work only where the GPU is more than three frames behind, and each such wait is logged with its frame and time (`meshstore: skin ring waited for at frame N's start`); a draw that still finds the ring full is skinned at a coarser level, never waited for. Before (fix1), a full ring submitted the frame's recording mid-frame and waited for it. Before, every object was skinned into a 128 MB ring every submission, and a draw that found it full was drawn stock: on b30's beach about one draw in five of the remodels (rocks, the cast, their weapons, the vehicles) was CE's model, a different few each frame. This was the main "original vs remodel" flipping, in every world geometry setting. |

The game's remodels-drawn signal and its set's number travel in RT_FRAME (`XGPU_RT_FLAG_REMODELS_DRAWN`, spare[3]);
the host pairs them with its store's own on the same frame of the stream (`xgpu_vulkan_meshes_pair`): a frame whose
traced scene's choice of the bake's runs (below) would differ between the game and the host is counted and logged.

The bake's static scenery (`prism_bake.c`) names the runs of every placement whose model a mod has a mesh for in any
tier, whatever `display.prism_models` says. While remodels are drawn the frame's traced scene leaves those runs out and
the objects are traced as drawn (`prism_bake_object_static` FALSE; on the host `xgpu_vulkan_meshes_drawn` picks the
frame's scene); with `"stock"` the frame traces the bake's whole scene. A change of the setting switches the traced
scene on the frame it switches the drawn one; before, CE's rocks stayed in the traced scene (or no rock was there)
until the next BSP.

## The level (`display.prism_world_geometry`)

Each surface's near or far level is `prism_world_geometry.c`'s `surface_choice` (distance, band, hysteresis), which
the view, the shadow maps, the occlusion's pre-pass and SCENE_STATIC's LEVELS all read.
Under PRISM Rebaked and Path Traced every pass draws the level drawn: `display.prism_world_geometry_shadows`' and
`_occlusion`'s `"far"` and `"stock"` are drawn there only with `debug.geometry_overrides`. Classic keeps them as they
were (defaults `"far"` and `"remodel"`, their menu rows shown), and `display.prism_rt_world` as it was. The BSP's remodel is taken up by the frame's first
reader (`prism_world_geometry_prepare`, from SCENE_STATIC), so a new BSP's first set and the bake key it opens are the
remodel's, not stock begun again a frame later.

## A setting changed in play

A set of SCENE_STATIC begun again for the BSP the last set was of (`display.prism_world_geometry` or the lighting
changed in play) is marked `XGPU_BAKE_FLAG_IN_PLAY`. Nothing is baked for it: the host loads the whole bake of its
key if the folder has one, else makes the set's own scene, nothing baked, and logs which. No lighting lights a
geometry with a bake of another set or world: the bake drawn before the change is kept only where it is of the new
set's key already (the setting changed back). Under Enhanced the set's own unbaked scene draws what a load of the map
with no bake draws for that geometry (`display.bake` `"off"`: the bake paused with nothing in it, so Raster gives CE's
frame), where before 2026-11-27 the bake drawn was kept until the next load: after Path Traced -> Enhanced on a map
loaded under Classic with the enhanced packs, the enhanced world's light on the classic geometry (the boulders
orange). Players never bake.

## Seeing it

`HALO_GEOMETRY_LOG=1` logs every 600 frames: what the view, the shadows and the traced scene drew of the objects,
their switches and flips (a change undone within 90 frames), stock-to-remodel and remodel-to-stock changes, the
object-frames whose shadow or traced shape was not the view's, the traced levels made coarser for the budget, the
stock fallbacks by channel; the meshstore's mode (resolved or legacy), ring waits, lost parts, traced parts left out
and the remodels-drawn pairing; the skin ring's line (its sizes, the most a frame took and in use, skinnings past
the share and let past it, fills, frame-start waits, the upload ring's mid-frame submissions); the world's near/far switches and flips. `HALO_GEOMETRY_LOG=2` logs each change too
(and every frame's pairing). With `HALO_GEOMETRY_LOG` set, each render-thread wait for the GPU of 30 ms or more is
logged with its frame and place (`vulkan: waited N ms for the GPU`, the first 400), so a stall can be pinned to it. `HALO_GEOMETRY_RESOLVER=legacy` puts back the choices before the resolver
(each pass's own), for comparisons on the same camera path (`debug.camera_path`, `tools/popin_measure.py`).

## Not yet the resolved geometry

- The bake's light (lightmaps, sun masks, probes) still holds CE's rock under a remodel (and every other replaced
  scenery model) until the enhanced bake, built from the enhanced geometry (the geometry plan's M4), lands; one bake
  per BSP holds one world variant: the shipped bake's work (`prism-v4-ship-bake`) owns the bake's scene, key and both
  variants.
- PRISM Rebaked and Path Traced trace the level from the bake's scene, which is the remodel's near level whole: a
  surface drawn at its far level is traced at its near level (the enhanced bake's scene, M4, is the place to follow
  the drawn level). Classic's ray tracing follows the drawn level (SCENE_STATIC's LEVELS, sent before the frame's
  rays and refitted in the same submission). The runtime
  needs from it a variant field (stock or remodel world) addressable beside the other, and, for remodelled scenery,
  the remodel's triangles in the bake's scene (the store's level 0 for each candidate placement) under a key that
  names them. The rock remodels are built within 0.6% outside and 3% inside CE's surface, so a stock rock's baked
  shadow nearly coincides with the remodel's.
- The world occlusion's distance field (`world_sdf.c`) is built from the collision BSP and stock scenery; decals and
  footprints follow the collision's height. They know only the stock shapes.
- A level change is one frame's step; no dither across frames yet.
