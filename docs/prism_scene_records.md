# PRISM v4's scene records

PRISM v4 (`overhaul_research/prism_v4_renderer/design.md`) gives the host a description of the scene in place of
CE's lighting passes: the materials, the objects drawn, the opaque draws of the G-buffer, the level's geometry, the
lights, the decals and the fog. These are new records of the renderer's command stream, specified in
`port/linux/src/xgpu_stream.h` ("PRISM v4's scene records"), with their encoders in
`port/linux/src/xgpu_scene_encode.c`.

## What is there now

`XGPU_STREAM_VERSION` is 36 (35: M4's `SCENE_MATERIAL_MAPS`; 36: M3's resolve's `GBUFFER_PHASE` value carries
`XGPU_GBUFFER_PHASE_PATHTRACED` in bit 28, the profile in bits 29 and 30 and the denoiser off in bit 31,
`docs/prism_pathtraced.md`: taken from the word's top down while `prism-v4-m2`'s Raster bits grow from bit 16 up; no
record was added). The records' types are in the stream's own list, after M1's first record,
`GBUFFER_PHASE` (82): `SCENE_MATERIAL` is 83 and so on to `SCENE_FOG`, 90, then `SCENE_VIEW`, 91, M2's `PRISM_BAKE`,
92, M4's `SCENE_MATERIAL_MAPS`, 93, and `XGPU_STREAM_RECORD_TYPES` is 94. The
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
pixel): the screen motion (in pixels, from where the surface was last frame: the cameras and, for an object, the
move of the `SCENE_INSTANCE` node its draw's part hangs from, the one whose matrix is the draw's first node's in the
vertex constants; the first-person weapon none on the screen), the emissive light (an object's combiners with no
light, the level's lightmap pass's with no lightmap) and the distance along the view's axis. The motion is the
part's rigid motion: a skinned part's vertices take its first node's. `debug.gbuffer_view` `motion` and `emissive`
show them.

M4 adds `SCENE_MATERIAL_MAPS` (93; `docs/prism_materials.md`), sent after the map's `SCENE_MATERIAL`s (and again as
the texture store answers again) for each shader a pack or a mod gives a material of PRISM's format 2, in the deferred
modes with `display.material_packs`: its tier (the pack's or a mod's; the automatic tier's forgets the maps it had),
its model, its flags (the normal map's coordinates, the environment shader's type and detail functions), the texture
store's ids of its images (albedo, normal, ORM, the three detail maps, emissive, height) and its numbers (the factors,
specular, translucency, the detail maps' scales). The G-buffer pass draws the material's opaque draws by its family's
uber-shader, which samples them. `GBUFFER_PHASE` gains a value, `BASE_TEXTURED` (6), sent in the deferred modes once
the diffuse texture pass is drawn, before the decals: where a material's maps drew the level's base colour, the host
puts it over the base target there, so that the decals lie over it.

M2 adds `PRISM_BAKE` (92), sent with `display.lighting` `"raster"` (`port/linux/game/prism_bake.c`): each frame the
in-game baker's control (the map and BSP, `display.bake`, `display.bake_quality`, the rebakes asked for, the bake
folder's and the status block's guest addresses, the GPU time a frame may give the bake), and once a BSP the reach's
inputs a slice a frame (`_REACH_INPUT`, words of the serialised collision BSP, clusters, PVS, water, triangles, places
and fighters, the static scenery's triangles and the light fixtures' placements; `_REACH_END`). The host writes the bake's state, progress, time and size into the status block
(`XGPU_BAKE_STATUS_*`). `docs/prism_bake.md` describes the bake.

The other three are specified and taken, not sent: `SCENE_MATERIAL_ANIMATE`, `SCENE_DECAL`, `SCENE_FOG`. `xgpu_scene_encode.c` still fills all eight from typed values for the tests; the stream writes its own
from `xgpu_stream_encode.c`.

## The byte-identity rule

The records are sent only in the v4 modes (`display.lighting` `ce_deferred`, `raster` and `pathtraced`).
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
| `SCENE_DECAL` | Per live decal | Decal id, bitmap and sequence frame, CE's framebuffer blend function (alpha, multiply, double multiply, add, subtract, min, max, alpha multiply add), flags (emissive, normal), the box (centre and three half-extent axes), colour, texture rectangle | 120 |
| `SCENE_FOG` | Each frame | Flags; the camera cluster's atmospheric fog (colour, maximum density, start and opaque distances); up to 8 fog planes seen, each its index, flags (inside), plane, colour and maximum density, opaque distance and depth | 48 + 64 per plane (at most 560) |
| `SCENE_VIEW` | At each G-buffer pass's begin | Flags (history); the camera this frame and last (eye and near plane, the axis at the screen's middle and the far plane, its change a unit of the screen's x and y); the view's corners in the target's pixels, this frame's and last | 176 |
| `SCENE_MATERIAL_MAPS` | After the map's `SCENE_MATERIAL`s, for each shader a pack or a mod gives a material (again as the texture store answers) | Material id (0 refused); tier (pack, mod; the automatic tier's forgets); model (`XGPU_GBUFFER_MODEL_*`, 15 the automatic tier's); flags (the normal at CE's bump map's coordinates, foliage, the environment shader's type and detail functions); 8 images' texture store ids (albedo, normal, ORM, primary, secondary and micro detail, emissive, height); 24 scalars (albedo and emissive factors, normal scale, roughness, metal, occlusion, specular, translucency, the detail maps' scales, height scale) | 148 |
| `PRISM_BAKE` | Each frame in Raster (the control); once a BSP (the reach's inputs, sliced) | Phase (control, reach input, reach end), generation, first, count, total, data and size; display.bake, the quality, the rebakes asked for, flags (menus, foreground, reach sent); the map's checksum, the BSP; the bake folder's and the status block's guest addresses; the frame's GPU budget, the threshold; the map's name | 144 |

### The capacities

- 8 bitmap slots: base, primary, secondary and micro detail, bump, multipurpose (or self-illumination), reflection,
  and one more (glass tint, water ripples): every bitmap a family's tag names that the material model reads.
- 8 maps a material of the pack or mod tier: the images its format names that the G-buffer reads or will.
- 32 scalars: CE's numbers for those, with room for the families' others.
- 64 nodes: CE's `MAXIMUM_NODES_PER_MODEL`.
- 256 lights a record: every fixture and dynamic light a CE map draws at once, with room; a frame with more sends
  several records.
- 8 fog planes: more than any CE cluster sees at once.
- 65536 triangles a `SCENE_STATIC` slice, as `RT_STATIC`.

### What the encoders refuse and cut

Each encoder writes into a buffer the caller gives and returns the bytes written, or 0:

- 0 when the buffer is too small, a required id is 0, or an enum is past its last value (family, phase, blend, a
  material's maps' tier or model);
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
