# PRISM v4's scene records

PRISM v4 (`overhaul_research/prism_v4_renderer/design.md`) gives the host a description of the scene in place of
CE's lighting passes: the materials, the objects drawn, the opaque draws of the G-buffer, the level's geometry, the
lights, the decals and the fog. These are new records of the renderer's command stream, specified in
`port/linux/src/xgpu_stream.h` ("PRISM v4's scene records"), with their encoders in
`port/linux/src/xgpu_scene_encode.c`.

## What is there now

The layouts and the encoders, and nothing else:

- no build compiles the encoders, and nothing calls them;
- no backend reads the records. Their types follow the stream's own (`XGPU_STREAM_SCENE_MATERIAL` is
  `XGPU_STREAM_RECORD_TYPES`, 83, and so on to 90) and are not counted in `XGPU_STREAM_RECORD_TYPES`, so the host's
  decoder would stop at one as an unknown record;
- `XGPU_STREAM_VERSION` is 30: M1's first record, `GBUFFER_PHASE` (82, in the stream's own list), sends
  `display.lighting` `ce_deferred`'s phases (`port/linux/game/prism_deferred.c`), the "CE light, deferred" diagnostic
  drawn from CE's own passes put in targets of their own. It is sent only in that mode.

The milestone that first sends them (M1, the deferred core) moves their types into the stream's list, gives the
decoder their minimum sizes and raises `XGPU_STREAM_VERSION`.

## The byte-identity rule

The records are sent only with `display.lighting` `raster` or `pathtraced`. With `ce`, the default of Classic and
Enhanced, none is ever written, and those frames stay byte for byte as the build before them drew them. Every
milestone checks this.

## The records

Every field is a 4-byte integer or float, so the guest's ILP32 layout and the host's LP64 one are the same. Floats
that are not finite are written 0 by the encoders. Three records end in an array of which only the first `count`
entries are sent; the record's size is its fixed part and those entries.

| Record | Sent | Carries | Size (bytes) |
| --- | --- | --- | --- |
| `SCENE_MATERIAL` | Once per shader per map, at load (again when a mod changes it) | Material id (1 on; 0 refused); the 64-bit FNV-1a hash of the shader tag's path; CE family (`senv`, `soso`, `sgla`, `swat`, `schi`, `sotr`, `smet`, `spla`); CE's material type; the shader's own flags word; the detail functions (primary, secondary, micro, a byte each); derived flags (alpha tested, two-sided, bump-alpha specular, animated, emitter, transparent layer, never an occluder); 8 bitmap slots' texture ids; 32 scalars (bump and detail scales, specular brightness and colours, reflection brightnesses, self-illumination colours, radiosity power and colour, translucency, tint, alpha reference) | 200 |
| `SCENE_MATERIAL_ANIMATE` | Each frame, for animated materials drawn | The three self-illumination channels' animation values; each bitmap slot's texture coordinate transform (two affine rows; the identity where it does not move); 4 change colours not tied to an object | 344 |
| `SCENE_INSTANCE` | Each frame, per object drawn | Instance id (the object's datum index, stable; 0 refused); flags (first person, cinematic, shield, camouflage, casts shadow, in the TLAS, no history); CE's LOD; the mesh store's mesh; 4 change colours; up to 64 nodes' 3×4 matrices, this frame's and last frame's | 88 + 96 per node (at most 6232) |
| `GBUFFER_DRAW` | Before each opaque draw of walk 1 | Material id (0 refused), instance id (0 the level), the level's surface index or an object's geometry and part, the CE vertex program's id, flags (alpha tested, two-sided, first person, remodel), LOD. The draw's vertices and indices are bound as today | 32 |
| `SCENE_STATIC` | Per BSP, a slice at a time, as `RT_STATIC` | Phase (begin, triangles, attributes, end, release), generation, first, count (at most 65536 a slice), total, the data's address and size. A triangle is 9 floats; its attributes are 16 words: material, surface, kind and lightmap page, three corners' atlas coordinates, three corners' base map coordinates | 32 |
| `SCENE_LIGHTS` | Each frame | First, count and total of the frame's lights; up to 256 lights, each its stable id, type (sun, dome, sky, point, spot), class (static, animated, switchable, dynamic), flags (on, casts shadows, first person), position and radius (or direction toward it), spot axis and cone, radiance and power, shape (sun disc, dome half angle, inner cone). Emitters are SCENE_STATIC triangles by their materials | 16 + 80 per light (at most 20496) |
| `SCENE_DECAL` | Per live decal | Decal id, bitmap and sequence frame, CE's framebuffer blend function (alpha, multiply, double multiply, add, subtract, min, max, alpha multiply add), flags (emissive, normal), the box (centre and three half-extent axes), colour, texture rectangle | 120 |
| `SCENE_FOG` | Each frame | Flags; the camera cluster's atmospheric fog (colour, maximum density, start and opaque distances); up to 8 fog planes seen, each its index, flags (inside), plane, colour and maximum density, opaque distance and depth | 48 + 64 per plane (at most 560) |

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
encoders' bytes equal the layouts packed in Python, the refusals and cuts, that the stream's version and types are
unchanged, and that no build compiles the encoder. It needs a C compiler and no game data or GPU.
