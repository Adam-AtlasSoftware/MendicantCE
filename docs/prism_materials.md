# PRISM v4's materials: the pack and mod tiers

PRISM v4's deferred renderer (`overhaul_research/prism_v4_renderer/design.md`, section 5) gives every shader of a map a
material. The automatic tier is made by the host from CE's own shader as the map loads (`SCENE_MATERIAL`,
`docs/prism_scene_records.md`): CE's colours, the material type's roughness and metal, CE's specular and reflection
numbers. This document is the next two tiers: materials of PRISM's own format, made on the player's machine by the
pack tools (the pack tier) or written by a modder (the mod tier), whose maps the G-buffer pass draws in place of the
automatic tier's.

| Priority | Tier | Made by | File |
| --- | --- | --- | --- |
| 1 | Mod | A mod's author (it may ship: it is the modder's own work) | `materials/<shader tag path>.material.json` in the mod's folder |
| 2 | Pack | `prism_packs.py`'s `v4_materials` step, from the player's own maps | `materials/<shader tag path>.pack.json` in `mods/prism_v4_materials` |
| 3 | Relief | The game, from the player's own bitmaps, as the map loads (below) | `prism_relief/<key>.<bump\|base>.ktx2` in the data folder (a cache) |
| 4 | Automatic | The host, from CE's shader, as the map loads | none |

Where a shader has both, the mod's is drawn. A mod's `materials/<path>.material.json` of format 1 is
`display.prism_materials`' (the forward renderer's model materials, `port/linux/game/mod_pbr.c`): where that setting
is `"pbr"` it is drawn, at the mod tier, and the shader's pack material is not; otherwise the pack's is.

The tiers apply in the deferred modes (`display.lighting` `"ce_deferred"` or `"raster"`) with `display.material_packs`
on (the default). With `"ce"` nothing is read or sent: Classic's and Enhanced's frames are as they were.
`debug.gbuffer_view` `"material_source"` colours each opaque pixel by its tier (blue the pack's, cyan a mod's, green
the automatic tier's), and the console's `prism_set "material_tiers list"` counts the map's environment and model
shaders by tier (each one's in the log).

## The format (2)

```json
{
  "format": 2,
  "family": "senv",
  "model": "opaque",
  "base": {
    "albedo": "textures/prism_v4/levels/b30/shaders/metal panels generic/albedo.ktx2",
    "albedo_factor": [1, 1, 1],
    "normal": "textures/prism_v4/levels/b30/shaders/metal panels generic/normal.ktx2",
    "normal_scale": 1.0, "normal_uv": "bump",
    "orm": "textures/prism_v4/levels/b30/shaders/metal panels generic/orm.ktx2",
    "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
    "height": "textures/prism_v4/levels/b30/shaders/metal panels generic/height.ktx2", "height_scale": 0.02
  },
  "layers": [
    { "slot": "secondary_detail", "albedo": "textures/prism_v4/details/levels/b30/bitmaps/dirt splotch detail.ktx2",
      "scale": "ce" },
    { "slot": "micro_detail", "albedo": "textures/prism_v4/details/levels/b30/bitmaps/detail metal sratches.ktx2",
      "scale": "ce", "function": "ce" }
  ],
  "type": "ce",
  "specular": { "f0": 0.06 },
  "emissive": { "map": null, "factor": [1, 1, 1] },
  "translucency": 0.0,
  "alpha": { "mode": "ce" },
  "source": "pack:prism_v4_materials@1"
}
```

- **Images** are KTX2 files of the mod's or pack's `textures/` folder (paths with either slash), read straight into
  the GPU's memory by the host's texture store as `display.prism_materials`' images are (BC1, BC3, BC4, BC5 or BC7,
  sides powers of two, every mip level, no supercompression).
  - `albedo`: the base colour, in the colour space of CE's base maps, with CE's painted light taken out; its alpha is
    CE's base map's (the detail maps' blend, the specular mask, the alpha test). Its layout is CE's base map's (the
    same coordinates), at any size.
  - `normal`: the shading normal in the surface's tangent space, RG: x along u, y along v (CE's bump maps'
    convention: down the picture), 0.5 flat. `normal_uv` `"bump"` samples it at CE's bump map's coordinates (the
    environment shader's bump map scale), `"base"` (the default) at the base map's.
  - `orm`: R the occlusion, G the roughness, B the metalness, as glTF's. `roughness` and `metal` multiply them (and
    are the values where there is no ORM); `occlusion` is the occlusion's strength.
  - `height`: R, the surface 0.5 (`height_scale` world units its 0 to 1 spans). Not drawn yet (parallax is later).
  - `emissive.map`: RGB times `emissive.factor`. Not drawn yet: CE's self-illumination stays the emissive light.
- **`layers`**: an environment shader's detail maps (`primary_detail`, `secondary_detail`, `micro_detail`), each
  `albedo` the detail map as CE draws it, `scale` a number or `[u, v]` or `"ce"`, `function`
  (`double_biased_multiply`, `multiply`, `double_biased_add` or `"ce"`) for the primary and micro layers.
- **`"ce"`**, or a value left out, takes CE's own from the shader's tag at run time: the environment shader's `type`
  (`normal`, `blended`, `blended_base_specular`), its detail functions, and its detail maps' scales as its texture
  pass computes them (the scale, times the base map's size over the detail map's where the tag says to rescale).
  This keeps CE's behaviour without copying the game's numbers into a file a mod ships.
- **`model`**: `opaque`, `alpha_tested` (CE's alpha test, as `opaque`), `foliage`, `clearcoat`, `subsurface` or
  `emissive`; left out, the automatic tier's (from CE's flags and material type).
- **`specular.f0`**: the dielectric's reflectance (0.04 the usual 4%).
- **`translucency`**: a foliage or subsurface model's diffuse transmission (0 to 1).
- **`source`**: who made it (`pack:<pack>@<version>` for a pack).
- Unknown keys are ignored. A file of another format is left out (the log says why).

## How it is drawn

The guest (`port/linux/game/prism_materials.c`) reads each environment and model shader's file as the map loads (and
again as the mods are found again or the settings change), sends its images to the texture store with the store's
record (`port/linux/game/mod_textures.c`, `KIND_MATERIAL` queries), and after the map's `SCENE_MATERIAL`s sends each
material's maps: `SCENE_MATERIAL_MAPS` (`xgpu_stream.h`; its tier, model, flags, the texture store's ids of its images
and its numbers, CE's where the file says `"ce"`).

The Vulkan backend (`port/linux/vulkan/xgpu_vulkan_stream.c`) keeps them in the material table. In the G-buffer pass a
draw whose material has maps is drawn by its family's uber-shader (`port/linux/src/xgpu_spirv.c`'s `gbuffer_pack`), a
G-buffer twin of the draw with CE's vertex program as it is:

- **an environment shader's** (the level's draws: CE's lightmap pass's twin): the base colour CE's texture pass
  draws, the albedo in the base map's place: the detail maps blended by the shader's type (normal: the secondary's
  alpha mixes the secondary and the primary; blended and blended base specular: the base's alpha does), its detail
  function and its micro detail's function, each step clamped as the NV2A's combiners clamp, CE's default detail map
  (grey) where the material has no layer; the normal map, at the base map's coordinates (the vertices' own, handed on
  by the vertex shader) or at CE's bump map's (the lightmap pass's texture 0), in the vertices' tangent frame (CE's
  tangent along u, its binormal along v), CE's bump map where the normal map is not resident yet;
- **a model shader's** (an object's draws): the albedo in texture 0's place (its alpha CE's base map's), CE's
  combiners making the base colour of it as before (the detail map, the change colour); the normal map at the base
  map's coordinates in the cotangent frame of the pixel's world position (from its distance along the view's axis
  and `SCENE_VIEW`'s camera) and its coordinates;
- **both**: roughness and metalness the ORM's times their factors (the automatic tier's until it is resident), the
  occlusion its red, specular the material's.

The maps' base colour also goes to the G-buffer's albedo target (`GBUFFER_BEGIN`'s third target, RGBA8: the colour,
its alpha the occlusion; 0 where the albedo is not the maps'), which the pass has only where a map's material has
maps. Once CE's diffuse texture pass is drawn into the base target, before the decals (`GBUFFER_PHASE`'s
`BASE_TEXTURED`, which `render_window` sends after `structure_render_diffuse_texture`), the level's pixels of a pack's
or mod's material take its colour in the base target's place (`xgpu_vulkan_post_albedo_over`): the decals (dirt,
scorch marks, bullet holes) lie over it as over CE's, and the resolve, its views and PRISM Raster's light read it as
they read CE's. An object's base colour is in the G-buffer's first word, as before. The albedo target is written by a
pack's twins alone, its write mask not the other targets', so it needs the device's `independentBlend`; and the
uber-shaders sample the textures' set's material bindings (12 to 17 of set 1), which the set has only where the device
draws `display.prism_materials`' materials (block-compressed images, 24 samplers and images a stage, 32 samplers a set,
`HALO_MATERIALS_OFF` unset). Where either is missing, the materials are drawn at the automatic tier: the guest reads no
pack or mod material and queues none of their images where the renderer draws no materials, and the host binds no
albedo target, so no pack's twin is made.

The images are streamed as `display.prism_materials`' are: each is usable once its levels from 256 texels down are
resident, the uber-shader drawing the automatic tier's values and CE's bump map until then.

## The relief tier

Under Enhanced and Path Traced the automatic tier draws a level's surfaces with their vertices' normal: CE's bump
maps, which Classic's lightmap pass reads, are in no G-buffer pixel of theirs, so Forerunner metal's designs (the
floors' plates and grooves, the walls' inlays and glyphs) were flat paint under the rebaked light. The relief tier
(`port/linux/game/prism_relief.c`, `display.surface_relief`, the Materials & Terrain group's SURFACE RELIEF, on in
Remastered and Realistic) gives each environment shader of a relief class a normal map of its designs raised and sunk:

- **The classes** are the enhancements' registry's (`tools/port_settings.py`'s `RELIEF_CLASSES`, whose twin is
  `prism_relief.c`'s `RELIEF_PATTERNS`, checked by `tools/test_prism_relief.py`): shader tag paths, `*` any run of
  characters, case not minded. `forerunner_metal` is b30's `metal *`, `decorative *` and `tech dense circuits` (a30,
  b30, b40, c10, c20 and c40 draw them), b40's `b40_metal *`, `b40_panels *`, `b40_bridgeramp*`, `b40_cliffmetal*` and
  `b40_generator *` (b40 and c40), and c20's `metal *`, `c20metal *` and `c20column*`; not their grates, lamps, glass
  or mirrors. a10, a50, d20 and d40 draw no Forerunner metal (the Autumn's, the Truth and Reconciliation's). A class
  joins by its patterns in both.
- **The normal map** (`prism_relief_gen.c`) is made from the player's own bitmaps as the map loads: where the shader
  has a bump map with relief in it (a texel in a hundred an eighth steep or more), its height is found again from its
  slopes (a Poisson solve, multigrid, the tile's edges wrapping), the drift of the solve taken out (twice a box of a
  quarter of the tile), the P8 palette's speckle with two medians, its plateaus flattened and its walls steepened (3t²
  - 2t³ between its 2nd and 98th percentiles), and the normal made of it at CE's bump map's coordinates, its depth the
  bump map's own (CE's tool's slopes) times 2, a quarter to 4 texels. CE's bump maps carry the designs where they have
  any (b30's floors, panels and strips: the relief deepens them 1.4 to 1.9 times on average); the base map's painted
  cracks and mottling raised beside them were noise. Where there is no such bump map, the base map's design: its
  luminance's median of 3 × 3, the same band and shaping, its dark seams and grooves sunk and its light plates raised,
  shallower (0.56 of a texel at 256), at the base map's coordinates; where the base map's band spans under 20 levels
  of 255 (b30's `metal flat generic` and `metal panels generic ext clean`, 16 and 17: mottling), none. Every step is
  integer arithmetic in a fixed order (no floating point, no platform maths), so every machine makes the same bytes (the test pins them, built with GCC and Clang, 32 and 64
  bits). b30's 512 × 512 maps take about 0.15 s each.
- **The cache**: `<data>/prism_relief/<key>.bump.ktx2` or `.base.ktx2` (BC5, every level, the box average of the
  one above), `<key>.none` for a shader with no design; the key is FNV-1a over `PRISM_RELIEF_VERSION`, the gain and
  every texel of both bitmaps. A later load reads it; Settings' Clear Caches deletes it (`game_caches.c`). Made from
  the player's maps, it never leaves the player's machine.
- **How it is drawn**: `prism_materials.c` sends a shader of a relief class that no mod or pack gives a material one
  of the pack tier with the relief flag (`XGPU_SCENE_MAPS_FLAG_RELIEF`): its only image the normal map (sent to the
  texture store from the cache, which accepts `/prism_relief/` paths for a material's image beside `/mods/`), CE's
  bump map until it is resident, its colour, roughness, metal and specular the automatic tier's (the uber-shader takes
  the automatic tier's specular for it); a pack's material of such a shader takes the relief's normal map in place of
  its own (CE's bump map); a mod's is drawn as it is. `debug.gbuffer_view` `"material_source"` shows it as a pack's
  (blue); the console's `prism_set "material_tiers list"` counts it apart, and the log says what was made and what
  was read from the cache (`relief: ...`).
- **The light it meets**: every light reads the G-buffer's normal, so the relief shows under the sun (Raster's N·L, its
  sun visibility the bake's), the flashlight and the dynamic lights (`dynamic_at`), the reflections, Path Traced's
  every ray from the first hit, and the bake's own light: each texel keeps the direction its light comes from (an SH
  L1 band, `k` and `a0`, docs/prism_bake.md "The estimator"), read at the pixel's normal, `B(n) = B(n0) (a0 + k · n)`.
  The bake is made at the surfaces' own normals (16 texels a world unit at MAX against 128 to 256 of a Forerunner
  map's texels), never the relief's: **no bake changes, its key does not hold the relief's version, and no pack is
  baked again** for it.
- **Not yet**: parallax (POM) and the relief's self-shadowing. CE's base colour is drawn by CE's texture pass at the
  unshifted coordinates, so a parallax shift would move the light's relief off the paint's (a pack's albedo, drawn by
  the twin, could shift both); the shadow needs the light's direction, which the G-buffer pass does not have (the light
  passes could march the height along the sun's, the flashlight's and the bake's direction, with a height image beside
  the normal map).

## The pack (`prism_packs.py`'s `v4_materials`)

`prism_packs.py build v4_materials --levels b30,a10 --data <data folder>` builds `mods/prism_v4_materials` from each
level's extract (the extractor's bitmaps and shaders: `shaders/<tag>.json`, each environment and model shader's fields)
and the texture packs built before it (Bold's and base_local's upscaled characters, Bold's surface maps, the material
overlay's classes). `tools/prism_packs/art/prism_v4_materials.py` (numpy and Pillow alone) makes, for every
environment shader of the level and every model shader of its characters (`characters\...`):

1. **The base colour.** An environment shader's: CE's base map; its painted relief light regressed on its own bump
   map's relief (the height Frankot-Chellappa integrates from the bump map at its scale over the base map's: cavity at
   4 and 16 texels, slope, the normal's x and y) above a sixteenth of the map, divided out where the fit explains a
   tenth of the fine detail at least (R² ≥ 0.1), the gain 0.6 to 1.6, the luminance's low band given back. A
   character's: Bold's base map (else base_local's, else CE's), at most 2048 texels a side; the broad shading painted
   at the scale of its features (a fiftieth of the map) flattened against its region's level inside its islands (the
   self-illuminated texels kept), the gain 0.7 to 1.5, its low band taken out again.
2. **The gate.** The low-band (a sixteenth of the map) CIEDE2000 of the composite base colour (with the detail maps'
   means by CE's functions; an alpha-tested map's drawn texels alone) against its source: at most 1.0 on average and
   3 at the 95th percentile. Where the de-lighting fails it, it is tried at 0.6 and 0.3 of its strength, then none
   (the source's colours as they are). A character's source against CE's base map (Bold's grade) is reported beside
   it.
3. **The normal.** CE's bump map where the environment shader has one (at its coordinates), else one found from the
   albedo's fine relief (weak); a character's from Bold's surface map (a remodel's bake: its y turned to CE's
   convention) where there is one, else none (the vertices' normal).
4. **Roughness, metal and occlusion.** An environment shader's: the material type's prior (`recipes/prism_v4_materials.json`,
   the automatic tier's) made smoother by CE's specular brightness, smoother where CE's specular mask is bright (the
   base map's alpha, or the bump map's where the tag says), rougher in the relief's cavities and smoother on its
   edges, kept in the type's band; metal only for metal types where CE also reflected, by the mask; the occlusion
   from the cavities. A character's: the material overlay's classes (its channel rule and class masks: plate,
   undersuit, polymer, skin, ...), CE's specular mask (the multipurpose map's red) smoother, Bold's surface map's
   roughness and occlusion mixed in.
5. **The files**: `materials/<tag>.pack.json`, `textures/prism_v4/<tag>/{albedo,normal,orm,height}.ktx2` (BC7, BC5,
   BC7, BC4, every level), the environment shaders' detail maps once each (`textures/prism_v4/details/<bitmap>.ktx2`,
   BC7, with CE's own mip levels: the step extracts the detail maps with every level, `map_extract --only bitmaps
   --mips`, into `extract_v4_mips`; CE's levels fade toward grey, so that a far surface keeps its base colour, where a
   chain halved from the first level would keep the map's mean: b30's metal scratches, 0.91 on average, would
   lighten every far metal wall by up to 1.8), and a report per level (`reports/<level>.json`: each shader's gate, R², strength, roughness spread, what was
   skipped and why). A shader whose texture moves (CE's texture animation) or that has no base map is left to the
   automatic tier.

It runs on the processor (b30's 73 shaders in 90 s, a10's 89 in 79 s, four workers, as measured). Everything it writes is made from the
player's maps: it stays on the player's machine (`LOCAL_ONLY.txt`).

`prism_v4_materials.py check` tests its encoders and the gate's maths on made-up pictures (`tools/test_prism_v4_materials.py`).

## Not yet

The pack and mod tiers for the transparent families (`sgla`, `swat`: the transparent layer is M4's second part), the
emissive and height maps in the G-buffer, the path tracer's and the baker's hits reading the maps (their materials are
still `display.prism_rt`'s), upscaling the environment's base maps, and per-layer detail normals.
