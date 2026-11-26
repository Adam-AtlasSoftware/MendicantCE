# PRISM's resolved geometry

One geometry per surface and object, which every pass draws: the view, the sun's shadow maps, the world occlusion's
pre-pass, the traced scene (display.prism_rt, PRISM Rebaked's object sun, every ray of PRISM Path Traced) and the
bake's frame scene. Before it, each of these chose a shape of its own, and the remodels flipped between levels and
between the remodel and CE's shape, and were lit by shadows of other shapes.

## Objects (`display.prism_models`)

The resolver is `port/linux/vulkan/xgpu_geometry.c` (pure C: `tools/test_geometry_resolver.py`). The mesh store
(`xgpu_vulkan_meshes.c`) asks it for each object drawn (`MESH_INSTANCE`):

| Decision | Rule |
|---|---|
| Key | The object (its datum) and its mesh, in a hash map aged after 1,800 frames unseen. Before, a 256-slot direct-mapped table of last frame's levels, which b30's 230 rocks and the cast shared: objects evicted each other, and a frame not drawn forgot the level. |
| Variant | Stock until the store has the mesh in, then the remodel for the rest of the map: the store keeps a mesh drawn once (`xgpu_mesh_store_pin`), so a budget never sends an object back to stock. A new set of meshes (the tier or the mods changed) forgets every choice. |
| Level | Once a frame per object, by the first of its draws: the view's size, or for a shadow drawn first or an object off screen, the size the view would have (its last size in the view times its distance then, over its distance now). The hysteresis (`display.prism_lod_hysteresis`) holds against the level it had, however many frames ago. Every shadow cascade draws that level. |
| Drawable floor | A level of fewer than an eighth of the triangles of the level before it is not drawn; the ladder stops at the level before. The kit's rocks (built 3, 1, 0 subdivisions: 1,984 / 124 / 31 triangles, 16x a step, the last CE's own facets) are drawn at level 0 alone; `rock_remodel.py` now builds quarter steps that never reach the cage. |
| Traced level | The drawn level, or a coarser level of the same remodel where the traced triangles' budget (75% of the window's 2.25 million) needs it, shared out at the frame's start over the objects drawn the frame before: the farthest made coarser first, the nearest made finer first while the total stays under 85% of the budget (so the choice does not hunt). Never stock, never left out. A level past the drawable floor is traced only beyond 40 world units. Before, each draw was traced in draw order as the remodel, its cage, CE's model or nothing, by a running triangle count. `display.prism_rt_objects` `"view_lod"` is no budget, `"low_lod"` the most coarse level each may take. |
| Skinning | A still object (no rig) whose skinning (mesh, level, matrices, wide stream) is seen again on a later frame is skinned once into a 160 MB keep region and read from there frame after frame (`xgpu_vulkan_mesh_skin_kept`). The rest are skinned each frame into the 256 MB ring, and the resolver holds them within a quarter of it: past that budget the farthest are drawn a level coarser of their drawable ladder (one level a pass, the objects whose level has not changed within the flip window first; a level let go finer only 150 frames after the budget took it and twice the flip window after its last change). A draw that still finds the ring full is skinned at a coarser level before it would be drawn stock. Before, every object was skinned into a 128 MB ring every submission, and a draw that found it full was drawn stock: on b30's beach about one draw in five of the remodels (rocks, the cast, their weapons, the vehicles) was CE's model, a different few each frame. This was the main "original vs remodel" flipping, in every world geometry setting. |

The bake's static scenery (`prism_bake.c`) names the runs of every placement whose model a mod has a mesh for in any
tier, whatever `display.prism_models` says. While remodels are drawn the frame's traced scene leaves those runs out and
the objects are traced as drawn (`prism_bake_object_static` FALSE; on the host `xgpu_vulkan_meshes_drawn` picks the
frame's scene); with `"stock"` the frame traces the bake's whole scene. A change of the setting switches the traced
scene on the frame it switches the drawn one; before, CE's rocks stayed in the traced scene (or no rock was there)
until the next BSP.

## The level (`display.prism_world_geometry`)

Each surface's near or far level is `prism_world_geometry.c`'s `surface_choice` (distance, band, hysteresis), which
the view, the shadow maps, the occlusion's pre-pass and SCENE_STATIC's LEVELS all read.
`display.prism_world_geometry_shadows` and `_occlusion` default to `"drawn"`; their `"far"` and `"stock"`, and
`display.prism_rt_world`'s `"remodel"`, `"far"` and `"stock"`, are debug overrides drawn only with
`debug.geometry_overrides` (their menu rows hidden without it). The BSP's remodel is taken up by the frame's first
reader (`prism_world_geometry_prepare`, from SCENE_STATIC), so a new BSP's first set and the bake key it opens are the
remodel's, not stock begun again a frame later.

## Seeing it

`HALO_GEOMETRY_LOG=1` logs every 600 frames: what the view, the shadows and the traced scene drew of the objects,
their switches and flips (a change undone within 90 frames), stock-to-remodel and remodel-to-stock changes, the
object-frames whose shadow or traced shape was not the view's, the traced levels made coarser for the budget.
`HALO_GEOMETRY_LOG=2` logs each change too. `HALO_GEOMETRY_RESOLVER=legacy` puts back the choices before the resolver
(each pass's own), for comparisons on the same camera path (`debug.camera_path`, `tools/popin_measure.py`).

## Not yet the resolved geometry

- The bake's light (lightmaps, sun masks, probes) holds CE's rock under a remodel, and one bake per BSP holds one world
  variant: the shipped bake's work (`prism-v4-ship-bake`) owns the bake's scene, key and both variants. The runtime
  needs from it a variant field (stock or remodel world) addressable beside the other, and, for remodelled scenery,
  the remodel's triangles in the bake's scene (the store's level 0 for each candidate placement) under a key that
  names them. The rock remodels are built within 0.6% outside and 3% inside CE's surface, so a stock rock's baked
  shadow nearly coincides with the remodel's.
- The world occlusion's distance field (`world_sdf.c`) is built from the collision BSP and stock scenery; decals and
  footprints follow the collision's height. They know only the stock shapes.
- A level change is one frame's step; no dither across frames yet.
