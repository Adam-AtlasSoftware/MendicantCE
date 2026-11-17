# PRISM v4's scene records

PRISM v4 (`overhaul_research/prism_v4_renderer/design.md`) gives the host a description of the scene in place of
CE's lighting passes: the materials, the objects drawn, the opaque draws of the G-buffer, the level's geometry, the
lights, the decals and the fog. These are new records of the renderer's command stream, specified in
`port/linux/src/xgpu_stream.h` ("PRISM v4's scene records"), with their encoders in
`port/linux/src/xgpu_scene_encode.c`.

## What is there now

`XGPU_STREAM_VERSION` is 35. The records' types are in the stream's own list, after M1's first record,
`GBUFFER_PHASE` (82): `SCENE_MATERIAL` is 83 and so on to `SCENE_FOG`, 90, then `SCENE_VIEW`, 91, M2's `PRISM_BAKE`,
92, and `XGPU_STREAM_RECORD_TYPES` is 93. The
host's decoder takes every one, with its fixed part's size as its minimum.

M1 sends six of them, and `SCENE_VIEW`. With `display.lighting` `ce_deferred` (`port/linux/game/prism_deferred.c`), in its G-buffer pass
(walk 1: the window's opaque surfaces drawn first into the G-buffer, `GBUFFER_PHASE`'s `GBUFFER_BEGIN` and
`GBUFFER_END`):

- `SCENE_MATERIAL`: every shader of the map (the `senv`, `soso`, `sotr`, `schi`, `scex`, `swat`, `sgla`, `smet` and `spla`
  tags, in that order and the tags' own order within each: the same ids in every run), once, as the map's first
  G-buffer pass begins: its family, CE's material type, its flags and detail function, its radiosity, tint, bump
  scale, specular and reflection numbers (no bitmaps yet). The Vulkan backend resolves each at the automatic tier as
  it arrives (the design's section 5.5: the material type's roughness and metal prior, smoother where CE's specular is
  brighter, metal only where CE reflected; the model from the family and flags) into its material table.
- `GBUFFER_DRAW`: before the G-buffer pass's draws, where the material, the instance, the flags or the lightmap page
  change: the shader's material (`_rasterizer_model_draw`'s and `_rasterizer_environment_lightmap_draw`'s shader), the
  instance (1 + the object's datum index, 0 the level, 0xffff the first-person weapon), the first-person flag, and
  (M2) the level's draws' lightmap page + 1 (`lightmap`, the field that was padding: the page whose lightmap pass has
  begun, `world_ao_lightmap_begin`'s bitmap's index among the BSP's lightmap bitmaps; 0 for an object). The level's
  twins write their lightmap coordinates in the G-buffer's word 0 and the page in the second target's word 2
  (`xgpu_stream.h`'s `GBUFFER_BEGIN`), which PRISM Raster finds each pixel's baked texel by.

And with `ce_deferred` or `display.prism_rt` on, each frame before its windows (`world_ao_frame_begin`):

- `SCENE_STATIC` (`port/linux/game/prism_scene_static.c`): the level's geometry as drawn, once a BSP, for every
  consumer: every surface of the BSP's opaque, alpha-tested and light-card materials in the BSP's order (lightmap,
  material, surface); a surface the remodel draws is its remodel's near triangles, then its far ones where they
  differ, each marked with its level; any other is the map's own triangle, wound (M2) so that its corners' cross
  product points out of the level, as its vertices' normals do (the map's own are all wound the other way, D3D's
  clockwise faces: display.prism_rt's one-sided faces were inside out with the stock world traced as drawn). Each triangle carries its material
  (`SCENE_MATERIAL`'s id), surface, kind, level, lightmap page, its corners' lightmap coordinates (0 to 1 on the page:
  the baker's atlas scales CE's pages, `prism_bake_atlas.h`) and base map coordinates, and `display.prism_rt`'s own
  material. `_PAGES` gives the pages' sizes. A slice a frame, whole surfaces, at most 65536 triangles (b30 with the
  sealed remodel: 2.07 million triangles, 35105 surfaces, 26 pages, in 64 frames). `_LEVELS`, each window where it
  changed: the level every surface is drawn at, by the remodel's own rule for the camera, for the surfaces off
  screen too. The Vulkan backend keeps the last whole set (`xgpu_vulkan_scene.c`): `display.prism_rt` builds its
  structure from it (`display.prism_rt_world` `auto`), both levels in, the level not drawn folded to points and the
  structure refitted as the levels change, so that the rays meet the surfaces the window draws, near and far.
- `SCENE_LIGHTS` (`port/linux/game/prism_scene_lights.c`): the light table: the sky's sun (static, casts shadows), its
  other lights as domes and its ambient light; each emitting material's triangles together (its middle and reach, CE's
  radiosity colour by power, its area: `XGPU_SCENE_LIGHT_EMITTER`); the level's light fixtures, live, switchable, on
  where their devices have power; and the dynamic lights the rasterizer drew (world_scene's: flashlight, plasma,
  muzzle flashes, projectiles), dynamic. Ids are stable (`XGPU_SCENE_LIGHT_*_ID`, a datum handle for the live ones).
  b30: 31 lights (the sun, a dome, the sky, 6 emitters, 10 fixtures, 12 dynamic). The backend keeps the last whole
  table.

And in the G-buffer pass, for its motion:

- `SCENE_VIEW` (91, after `SCENE_FOG`): as the pass begins, the window's camera this frame and last (as world AO's
  pass has it: the eye, the axis at the screen's middle, its change a unit of the screen's x and y) and the view's
  corners in the target's pixels; history where the last frame's was the same window's.
- `SCENE_INSTANCE`: each object the pass draws, once, before its draws: its id (1 + its datum index, as
  `GBUFFER_DRAW`'s instance), its nodes' matrices as drawn (the pose between the ticks) and as last sent where that
  was the pass before and the same object (else `NO_HISTORY`).

The Vulkan backend draws each G-buffer twin into a second target too (`xgpu_stream.h`'s `GBUFFER_BEGIN`: four words a
pixel): the screen motion (in pixels, from where the surface was last frame), the emissive light (an object's
combiners with no light, the level's lightmap pass's with no lightmap) and the distance along the view's axis.
`debug.gbuffer_view` `motion` and `emissive` show them.

The motion is each vertex's own. The level's is the cameras' (`SCENE_VIEW`'s last and this frame's). An object's twin
runs its vertex program a second time on the last frame's constants (`xgpu_spirv.h`'s
`XGPU_SPIRV_BINDING_LAST_CONSTANTS`, `xgpu_spirv_options`' `gbuffer_previous`), which the host makes from the draw's own:
the camera's projection (`c[-96]` to `c[-93]`) the pass before's, and each node's matrix (`c[-36]` on: a model's nodes
in its own order, each its world matrix by its default pose's inverse, `models.c`'s relative node matrices) moved back
as its `SCENE_INSTANCE` node moved (last frame's world matrix by this frame's inverse). The two clip positions go to
the pixel shader, whose motion is their difference: a skinned limb moves as its own nodes and weights move it, not as
the part's first node. A model of more nodes than the constants hold (44: a Custom Edition map's, skinned a part's own
nodes at a time) moves as its root. The first-person weapon has none on the screen.

M2 adds `PRISM_BAKE` (92), sent with `display.lighting` `"raster"` (`port/linux/game/prism_bake.c`): each frame the
in-game baker's control (the map and BSP, `display.bake`, `display.bake_quality`, the rebakes asked for, the bake
folder's and the status block's guest addresses, the GPU time a frame may give the bake), and once a BSP the reach's
inputs a slice a frame (`_REACH_INPUT`, words of the serialised collision BSP, clusters, PVS, water, triangles, places
and fighters, the static scenery's triangles and the light fixtures' placements; `_REACH_END`). The host writes the bake's state, progress, time and size into the status block
(`XGPU_BAKE_STATUS_*`). `docs/prism_bake.md` describes the bake.

And in the v4 modes where the host draws the window deferred (`prism_deferred_screen_passes`: the Vulkan backend, a
target of one sample), the fog and the decals are screen passes over the G-buffer, not CE's draws:

- `SCENE_FOG` (`port/linux/game/prism_deferred.c`): in CE's fog pass's place (after the resolve, before the
  transparent geometry), the window's fog: the atmosphere (its colour, maximum density, start and opaque distances,
  the eye's share of it) and the camera's cluster's fog plane (its plane, colour, maximum density, opaque distance and
  depth, the eye's depth in it). The host (`xgpu_vulkan_post_fog`) finds each opaque pixel's point from its distance
  and the camera and gives it the fog CE gives its surface there: the level's as CE's fog pass drew it (the density
  tables' curves: the atmosphere's a ramp, the plane's `(1 - cos(pi x)) / 2` along the view and under the plane,
  as its 16-texel tables are sampled), an object's as its draws' combiners had it (their vertex programs' curves),
  blended as CE's fog pass blends. The level is not drawn again for its fog, and the opaque objects' draws have no fog
  of their own (`rasterizer_xbox_models.c`: no fog constants, the planar fog's vertex program its lights' alike but
  for the fog, no true atmospheric fog); the sky's models keep theirs, and the transparent geometry its own. With
  `XGPU_SCENE_FOG_FLAG_SCREEN`, in the fog screen's place for its second pass (`rasterizer_xbox_environment_fog.c`),
  the fog screen's density (its near and far distances, its near density over its far, whether the level's and the
  objects' pixels take it) written into the colour target's alpha, which the fog screen's layers then blend by as
  CE's do: the fog screen draws neither the level nor its queued models (the a30 rocks' "shell" of M0: nothing draws
  the stock rock for the fog any more).
- `SCENE_DECAL` (`port/linux/game/prism_deferred.c`, `rasterizer_xbox_decals.c`): in each decal's draw's place (the
  primary, secondary and light layers), the decal projected by its box: its
  projection's basis and extent as `decals.c` made it (kept in a table of the port's by the decal's datum, never in
  the game's state), its radius out of the surface, its bitmap's texture and sprite, CE's framebuffer blend function,
  its colour and intensity. The host (`xgpu_vulkan_post_decal`) draws it over the rectangle of the screen its box
  covers, into the target the window's draws go to then (the base colour's in the base phase, else the light), on the
  level's pixels inside the box whose surface faces the decal's way (the G-buffer's normal within 75 degrees), as CE's
  decal draws blend: their combiners' colour of the texel and the decal's colour and intensity, the pipeline's blend
  of the function. CE's decals have no normal map, so none is projected. A decal made without its projection kept (a
  saved game's) is drawn as CE draws it, and so are the water's layer and the alpha-tested layer: CE's texture pass
  leaves out the pixels under an alpha-tested decal by the stencil its draw writes, which a projection does not
  (`XGPU_SCENE_DECAL_FLAG_ALPHA_TESTED`, the host's alpha test, is there for when it does).

`SCENE_MATERIAL_ANIMATE` is specified and taken, not sent. `xgpu_scene_encode.c` still fills every record from typed
values for the tests; the stream writes its own from `xgpu_stream_encode.c`.

## The byte-identity rule

The records are sent only in the v4 modes (`display.lighting` `ce_deferred` now, `raster` and `pathtraced` to come).
With `ce`, the default of Classic and Enhanced, none is ever written, and those frames stay byte for byte as the build before them drew them. Every
milestone checks this.

## The records

Every field is a 4-byte integer or float, so the guest's ILP32 layout and the host's LP64 one are the same. Floats
that are not finite are written 0 by the encoders. Three records end in an array of which only the first `count`
entries are sent; the record's size is its fixed part and those entries.

| Record | Sent | Carries | Size (bytes) |
| --- | --- | --- | --- |
| `SCENE_MATERIAL` | Once per shader per map, at load (again when a mod changes it) | Material id (1 on; 0 refused); the 64-bit FNV-1a hash of the shader tag's path; CE family (`senv`, `soso`, `sgla`, `swat`, `schi`, `sotr`, `smet`, `spla`); CE's material type; the shader's own flags word; the detail functions (primary, secondary, micro, a byte each); derived flags (alpha tested, two-sided, bump-alpha specular, animated, emitter, transparent layer, never an occluder); 8 bitmap slots' texture ids; 32 scalars (bump and detail scales, specular brightness and colours, reflection brightnesses, self-illumination colours, radiosity power and colour, translucency, tint, alpha reference) | 200 |
| `SCENE_MATERIAL_ANIMATE` | Each frame, for animated materials drawn | The three self-illumination channels' animation values; each bitmap slot's texture coordinate transform (two affine rows; the identity where it does not move); 4 change colours not tied to an object | 344 |
| `SCENE_INSTANCE` | Each frame, per object drawn | Instance id (1 + the object's datum index, stable; 0 refused); flags (first person, cinematic, shield, camouflage, casts shadow, in the TLAS, no history); CE's LOD; the mesh store's mesh; 4 change colours; up to 64 nodes' 3×4 matrices, this frame's and last frame's | 88 + 96 per node (at most 6232) |
| `GBUFFER_DRAW` | Before each opaque draw of walk 1 | Material id (0 refused), instance id (0 the level), the level's surface index or an object's geometry and part, the CE vertex program's id, flags (alpha tested, two-sided, first person, remodel), LOD, the level's lightmap page + 1 (M2). The draw's vertices and indices are bound as today | 32 |
| `SCENE_STATIC` | Per BSP, a slice at a time, as `RT_STATIC`; its levels as they change | Phase (begin, triangles, attributes, end, release, pages, levels), generation, first, count (at most 65536 a slice), total, the data's address and size. A triangle is 9 floats; its attributes are 16 words: material, surface, kind, level and lightmap page, three corners' lightmap coordinates, three corners' base map coordinates, the ray tracer's material. A page is its width and height; the levels a bit set over the surfaces (set: drawn far) | 32 |
| `SCENE_LIGHTS` | Each frame | First, count and total of the frame's lights; up to 256 lights, each its stable id, type (sun, dome, sky, point, spot, emitter), class (static, animated, switchable, dynamic), flags (on, casts shadows, first person), position and radius (or direction toward it), spot axis and cone, radiance and power, shape (sun disc, dome half angle, inner cone). Emitters are SCENE_STATIC triangles by their materials | 16 + 80 per light (at most 20496) |
| `SCENE_DECAL` | Per decal of the level drawn, in its draw's place | Decal id, its bitmap's texture (its Direct3D header's guest address) and sequence frame, CE's framebuffer blend function (alpha, multiply, double multiply, add, subtract, min, max, alpha multiply add), flags (emissive, normal, alpha tested), the box (centre and three half-extent axes: across, up, out of the surface), colour (CE's vertex colour: red, green, blue and intensity), texture rectangle (the sprite's) | 120 |
| `SCENE_FOG` | Each window, after the resolve; and in the fog screen's second pass (`SCREEN`) | Flags (atmosphere; screen, the screen's level and objects); the camera cluster's atmospheric fog (colour, maximum density; start and opaque distances, the eye's share); the fog screen (near and far distances, near density over far); up to 8 fog planes (CE draws one), each its id, flags (inside), plane, colour and maximum density, opaque distance and depth, the eye's depth in it | 64 + 64 per plane (at most 576) |
| `SCENE_VIEW` | At each G-buffer pass's begin | Flags (history); the camera this frame and last (eye and near plane, the axis at the screen's middle and the far plane, its change a unit of the screen's x and y); the view's corners in the target's pixels, this frame's and last | 176 |
| `PRISM_BAKE` | Each frame in Raster (the control); once a BSP (the reach's inputs, sliced) | Phase (control, reach input, reach end), generation, first, count, total, data and size; display.bake, the quality, the rebakes asked for, flags (menus, foreground, reach sent); the map's checksum, the BSP; the bake folder's and the status block's guest addresses; the frame's GPU budget, the threshold; the map's name | 144 |

### The capacities

- 8 bitmap slots: base, primary, secondary and micro detail, bump, multipurpose (or self-illumination), reflection,
  and one more (glass tint, water ripples): every bitmap a family's tag names that the material model reads.
- 32 scalars: CE's numbers for those, with room for the families' others.
- 64 nodes: CE's `MAXIMUM_NODES_PER_MODEL`.
- 256 lights a record: every fixture and dynamic light a CE map draws at once, with room; a frame with more sends
  several records.
- 8 fog planes: more than any CE cluster sees at once.
- 65536 triangles a `SCENE_STATIC` slice, as `RT_STATIC`.

### What the encoders refuse and cut

Each encoder writes into a buffer the caller gives and returns the bytes written, or 0:

- 0 when the buffer is too small, a required id is 0, or an enum is past its last value (family, phase, blend);
- counts past their capacity are cut to it;
- a light of an unknown type or class, and a fog plane with no normal, is left out, the others kept in order;
- an instance given no last-frame matrices is marked `NO_HISTORY` and sends this frame's twice.

## Tests

`python3 -m pytest -q tools/test_scene_records.py` builds `tools/fixtures/scene_records_test.c` with the encoder as a
64-bit and a 32-bit program (the 32-bit build is skipped, with the reason, where the compiler cannot make one). It
checks that every field has the same offset in both, that every size is a multiple of 4 and as above, that the
encoders' bytes equal the layouts packed in Python, the refusals and cuts, the stream's version and that the
records' types are in its list, the decoder takes each, and the guest's stream writes `SCENE_MATERIAL` and
`GBUFFER_DRAW`. It needs a C compiler and no game data or GPU.
