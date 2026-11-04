# World-space ambient occlusion (#578)

`display.ambient_occlusion = "world"`: ambient occlusion found in world space, built for how Halo is lit.
It has two sources:

1. A **signed distance field (SDF) of the structure BSP**. It is baked on the player's machine from the player's own
   map, once, and cached.
2. **Analytic proxies** (spheres, capsules, boxes) for the dynamic objects.

Both apply to the **ambient term alone**, through the per-surface ambient slot #574 added. Nothing is read from the
screen, so there's no depth pre-pass, no stencil marks (#545), no noise and no denoiser. Off-screen occluders count,
and the result doesn't shimmer at the screen's edges. The same code serves the desktop, the Steam Deck, mobile, VR's
eyes (it doesn't depend on the eye) and Metal with multisampling.

This branch (`world-ao`) is the skeleton: the interfaces, stubs, hooks and tests. Four workstreams fill it in
parallel ([Workstreams](#workstreams)). With the setting at anything but `"world"` nothing new runs. `"world"`
shades with what of the field and the proxies is ready (D's shading).

## Principles

- **Render-only and deterministic.** Nothing here is read by the simulation, sent between machines, saved, or able to
  change a tick. The bake reads tag data, which is read-only once loaded, and works from a copy of it. The result
  depends only on the map and the frame's drawn pose: no history, no randomness.
- **The level doesn't darken itself.** Bungie's radiosity lightmaps already hold the level's own occlusion. The SDF
  never shades the level's lightmap pass. The level is darkened only by objects' proxies, and objects are darkened by
  the level's SDF and by other objects' proxies.
- **Only the ambient light.** The flashlight, the dynamic lights (classic and enhanced), specular, self-illumination,
  decals, transparents and the first-person weapon stay as they are, exactly as with #574's GTAO.
- **Never block or lengthen a load.** The owner's long-term goal is no load screens at all. A map with no field yet
  loads exactly as fast as it does today ([The bake's schedule](#the-bakes-schedule)).
- **Off is pixel-identical.** Off emits no stream records and no new shader code: the emitters write byte-identical
  shaders. The Classic preset sets `display.ambient_occlusion = "off"`, so Classic always bypasses all of this.

## The frame

```
scenario_load ─► world_ao_map_loaded ─► world_scene_map_loaded (meshes, ids)
                                     └► world_sdf_map_loaded  (cache hit: load; miss: background bake)
render_frame ──► world_ao_frame_begin ─► world_scene_frame_begin (objects as drawn: render_interpolation pose)
                                      └► world_sdf_frame (upload a finished field, a few slices a frame)
render_window ─► world_ao_window_begin ─► world_proxies_window (≤64 shapes) + field place
                                       └► halo_world_ao_window ─► XGPU_STREAM_WORLD_AO_WINDOW ─► backend block
                 render_objects (world_ao_object_begin per object: owner slot, reference)
                 structure_render_lightmaps
               ─► world_ao_window_end (block withdrawn; later passes never see it)
```

The hooks are in `source/render/render.c` (`render_frame`, `render_window`) and `source/scenario/scenario.c`
(`scenario_load`, `scenario_switch_structure_bsp`, `scenario_unload`), each marked `port:`.

## The scene (`world_scene.h`)

The world as a world-space renderer sees it, kept beside the game's state.

- **Static meshes:** each structure BSP's collision geometry (`collision_bsp` surfaces, edges and vertices, fanned
  into triangles), in world space. It has id `WORLD_SCENE_ID_STATIC_BIT | bsp_index` and the collision material
  index of each triangle. Collision geometry rather than render geometry, because it's watertight enough to sign the
  field and it's what objects stand on. `world_scene_static_triangles` hands the bake an owned copy, so a background
  thread never reads tags that a map unload frees.
- **Instances:** every drawn object, with id = its datum handle (index and salt), which is stable for the object's
  life and never reused while it could be confused. Each holds its definition (tag index, the proxies' key), its
  root transform this frame and last frame (as drawn, i.e. `render_interpolation.c`'s blended pose, so motion
  vectors come for free later), its bounding sphere as drawn, and its **lighting point** (where its ambient was
  sampled: `render_lighting`'s point below it).
- **The world light list:** the frame's dynamic lights as `dynamic_lights.c` ranks them, plus the structure's light
  fixtures. Each has a stable id, position, radius, cone and linear color. Nothing reads it yet; it's groundwork for
  RT shadows and for the "light list in world space" item.

The scene is built only while `"world"` is on, refreshed once a frame inside `render_interpolation_frame_begin/_end`
(the objects then stand where they are drawn), and freed at unload.

## The distance field (`world_sdf.h`)

### Geometry, bounds and resolution

There's one field per structure BSP, over its collision geometry's bounds padded by `WORLD_SDF_PADDING_VOXELS` (2).
The voxel is **0.25 world units** (about 0.76 m; a Spartan is about 0.7 wu tall). On a large map the voxel grows in
steps of ×1.125 until the field has at most **8 Mi voxels** (`WORLD_SDF_MAXIMUM_VOXELS`, 8 MB as R8) and at most
1024 along any axis (`world_sdf_layout`). Small and indoor maps get 0.25 wu. A big outdoor map like Blood Gulch
(about 300×300×60 wu) gets about 0.9 wu. That's still right for floors and walls, because trilinear interpolation
reproduces a plane's distance exactly; only corners soften. A sparse fine level (0.125 wu bricks near surfaces) is
a later option if captures show the outdoor maps need it. The format version covers that change.

### Encoding

The texture is **R8 signed normalized** (`VK_FORMAT_R8_SNORM`, `MTLPixelFormatR8Snorm`; linear filtering of it is
required on both APIs). It's sampled linear and clamped to the edge. `texel × range` is the signed distance at the
voxel's centre: positive is outside (air), negative is inside solid. range = `WORLD_SDF_RANGE_VOXELS` (12) × voxel,
which is 3 wu at the finest voxel and covers the longest occlusion step (2.4 wu, below). Precision is range/127,
about 0.024 wu.

### The bake

`world_sdf_bake_slab` is pure, and runs on N worker threads, each baking a slab of z slices:

1. **Narrow band, exact.** For each triangle, visit the voxels in its bounds grown by `range`, and keep the least
   exact point-triangle distance and the triangle that gives it. A triangle's work is about
   area / voxel² × 2·range / voxel.
2. **Far field.** Voxels farther than `range` from every triangle stay at ±range: the encoding clamps there anyway,
   so no propagation pass is needed.
3. **Sign:** whether the voxel's centre is inside solid, by the collision BSP's own point test (`bsp3d` leaf
   classification, the physics' answer). It's evaluated on the copy through the `inside` callback, which works on a
   copy of the bsp3d nodes and leaves. It's not done by normal orientation: Halo's BSPs aren't perfectly closed.
4. **Encode** (`world_sdf_encode`).

Slabs are independent, so threads need no locks. The bake runs at the lowest thread priority and stops as soon as
asked: at quit, and at a map unload if the map can't be re-read.

### The bake's schedule

- **First launch:** in #575's fixed start (intro → "Preparing menu pictures" → main menu). That pass already loads
  every stock map, so copying each map's geometry while it's loaded (`world_sdf_startup_map_loaded`, called from
  `menu_pictures_render.c`'s `_prepare_loading` state) costs no extra loads. The bakes themselves wait until the pass
  ends and the main menu shows (`menu_pictures_preparing`), so neither comes later than without them; they
  then run from the copies at the lowest priority behind the main menu. If the pictures are already cached but a field is
  missing (a new bake version), `world_sdf_startup_wanted` makes the pass run for the fields alone. Only the loaded
  BSP's tags are in memory, so the pass bakes the BSP each map starts in and each picture's, and switches to no
  other (repeated switches once leaked the device's groups, #506). A campaign map's other BSPs are baked in the
  background the first time the game switches to them (proxies alone until then), never in the switch.
- **Threads:** the bake runs on every processor (`HALO_WORLD_SDF_THREADS` overrides, for measuring), each thread
  lowered with `setpriority(PRIO_PROCESS, 0, 19)`, which each host applies to the calling thread (Linux and
  Android as it is, macOS as the background quality of service, Windows as the lowest thread priority).
- **Any other time:** after a map loads with no field (a custom map, a new bake version), `world_sdf_map_loaded`
  only looks in the cache. On a miss it copies the geometry (a few ms) and starts the bake on a low-priority
  background thread, after the load is done. **Never during a load:** `scenario_load` does nothing more than a cache
  lookup (test: `test_the_bake_is_never_in_a_load`).
- **While a field isn't ready,** `"world"` quietly runs with **proxies only** (the block's fade is 0). When the bake
  finishes, the field uploads a few slices a frame (at most 512 KB a frame) and then **fades in over 1 s**
  (`world_sdf_fade`), with no hitch and no pop. We chose proxies-only over falling back to GTAO: GTAO's pre-pass
  would cost more than the whole new feature on the Deck, and switching would pop.
- **Measured and stated:** each bake logs `world sdf: <map> bsp <n>: <w>x<h>x<d> voxel <v> in <ms> ms on <t>
  threads, <bytes> cached`, and the header records the time and threads. Workstream B's PR states the bake time and
  cache size for every stock map on the 5900X, the Steam Deck and one phone. Targets: under 2 s per BSP on 8
  desktop threads, under 10 s in the background on a phone, and a cache under 3 MB per BSP after deflate.

### Cache

The cache follows the menu pictures' conventions (#506/#575): the data folder's `world_sdf/`, made on this machine,
never shipped, and never trusted without checks.

- **Name:** `<data>/world_sdf/<map name>_<bsp index>.wsdf`.
- **Key:** the map's cache file header checksum (`menu_picture_map_checksum`'s source) plus `WORLD_SDF_VERSION`. A
  different map under the same name, or a new version, is baked again. A good file is reused forever.
- **Format** (little-endian, written field by field, not as a struct):
  - a 64-byte `struct world_sdf_header`: magic `WSDF`, version, map checksum, bsp index, width/height/depth, origin,
    voxel, range, deflated size, bake ms, bake threads;
  - then the voxels (x fastest, then y, then z), deflated as one zlib stream (the game's own zlib,
    `source/memory/zlib`);
  - then a CRC-32 of the header and the deflated bytes (the menu pictures' CRC).
- **Writing:** to `<name>.tmp`, then renamed into place, so a crash leaves no half file.
- **Reading:** a file with the wrong magic, version, checksum, size or CRC is deleted and baked again.
- **Bumping `WORLD_SDF_VERSION`:** whenever the bake's arithmetic, the source geometry or the layout changes.

## Proxies (`world_proxies.h`)

Each object definition gets up to 16 shapes, made the first time it's drawn and kept until the map unloads:

- **Bipeds:** a shape for each node with collision geometry (the collision model's node of the same index, its
  first permutation's BSP vertices): a capsule along the bone to its child node (or, with no child, along the
  vertices' longest spread), its radius 0.9 × the vertices' mean distance from that axis, its ends the vertices'
  extent along it less the radius; a sphere where they're shorter than they're wide (the head). The 10 largest are
  kept: upper and lower arms, thighs, calves, the torso and the head on a Spartan. With no collision geometry at
  all, a capsule from each node to its child, radius 0.12 × the bounding radius. Each shape rides on its node.
- **Vehicles:** a box for each node's collision geometry (its bounds in the node's space), the smallest merged into
  its nearest ancestor's box (or the largest) until at most 4 are left, plus a sphere for each node named as a wheel
  ("tire", "wheel"): its own geometry's bounds, else its height above the body's lowest point (0.15 to 1 wu).
- **Scenery, machines, controls, light fixtures, items (weapons and equipment on the ground, garbage):** one box,
  every node's collision geometry in node 0's space; with no collision geometry, a sphere of the bounding radius at
  its bounding centre.
- **Anything else** with a model: its bounding sphere.
- **None:** projectiles, sound scenery, a weapon or equipment held (its holder covers it, and it would shade the
  hands holding it), invisible objects, and any shape under 0.05 wu across.

**Each frame and window** (`world_proxies_window`): the shapes are placed by their node matrices as drawn, i.e. the
`render_interpolation.c` pose that `render_objects` draws with, so proxies never lag their objects. They're culled
by their reach sphere against the camera's frustum (its four sides), ordered by the distance from the camera to the
reach's centre, and the first 64 are kept. Each proxy records its owner, so an object's own proxies don't shade it
(models have no baked self-occlusion to double, but the crude shapes would look wrong on their own surface). The
**owner slot** (`[1].w`) is the order of the owner's first shape in the window: 0 for the nearest shape's object, then
1, 2... `world_proxies_window`'s `owners[i]` is the object's id (datum handle) for each shape, so `world_ao.c` gives
a draw its object's slot by finding the object there (-1 when it has no shape in the window, which matches none).

The game is read through `world_proxies_source.h` alone (`world_proxies_game.c`): the definitions' tags, the
frame's objects (A's `world_scene_instances`), their
node matrices as drawn and the window's frustum. `debug.world_proxies_view` draws the window's shapes as wireframes
(spheres yellow, capsules green, boxes cyan).

**GPU layout:** three vec4s per proxy (`struct world_proxy`, `xgpu_stream.h`):
- `[0]`: a.xyz, kind (1 sphere, 2 capsule, 3 box);
- `[1]`: b.xyz (a box's half axis x), owner slot;
- `[2]`: radius (a box's half axis y in xyz and half extent z in w).

## Shading

### Where it applies

#574's `XGPU_AMBIENT_OCCLUSION_LIGHT_*` variants gain a source: GTAO's texel, or the world term computed in the
shader. They're the same three places:

| Draw | Ambient multiplied by |
| --- | --- |
| Model lit per pixel (`_LIGHT_MODEL`) | SDF(p, n) relative to the reference × proxies of other objects |
| Model lit per vertex (`_LIGHT_VERTEX`, split vertex shader) | the same, per pixel, on the split ambient |
| Structure lightmap pass (`_LIGHT_LIGHTMAP`) | proxies alone (never the SDF) |

Self-illuminated draws and the first-person weapon stay at 1. #574 kept them with stencil marks; here the front end
simply doesn't give those pixel keys the world variant.

### SDF occlusion (objects)

At a point p with normal n (world space), d(x) is the field's trilinear sample × range:

```
h_i   = h_1 · 2^(i-1),  i = 1..5,   h_1 = max(0.15 wu, voxel)          (0.15 0.3 0.6 1.2 2.4)
occ   = Σ_i 2^-i · clamp((h_i − d(p + n·h_i)) / h_i, 0, 1)             (cone along n; Σ 2^-i < 1)
A_sdf = 1 − clamp(k · occ, 0, 1),   k = 1.6 (tuned by D against captures)
```

The 5 taps are linear 3D texture reads. On mobile/Deck tiers, D may use 4 taps (`h_1 = 0.3`).

**The reference: no double darkening.** An object's ambient light is the lightmap sampled at its lighting point, and
that radiosity already holds the enclosure there: a corridor's floor is dim because the corridor encloses it. So the
SDF term is taken relative to the occlusion the sample already holds:

```
ref   = A_sdf(lighting_point + up·h_1, up)                 (CPU, per object, world_sdf_sample; 1 if no field)
A_obj = clamp(A_sdf(p, n) / max(ref, 0.25), 0, 1)
A_obj = mix(A_sdf(p, n), A_obj, reference_weight)          (block parameters[1][0]; default 1)
```

A Spartan in a corridor isn't darker overall than the floor's light says. But its side facing a wall, its underside
and a Warthog's belly over the ground are darker, which is what the eye misses today.

### Proxies' occlusion

Each is the cosine-weighted share of the hemisphere the shape covers, computed analytically. The reference is
`world_proxy_occlusion` in `world_proxies.c` (`sphere_occlusion`, `capsule_occlusion`, `box_occlusion`); D's shaders
follow it line for line. p is the point, n its unit normal, all in world units.

- **Sphere** S(c, r) (centre c, radius r), exact, with the horizon's clipping (Lagarde and de Rousiers' form, as
  Quilez gives it):

  ```
  d = c − p;  if |d|² ≤ r²: 1
  l = |d|,  nl = n·d / l,  h2 = l² / r²,  k2 = 1 − h2·nl²
  k2 > 0.001 (it crosses the tangent plane):
      x   = clamp(−nl·sqrt((h2 − 1) / (1 − nl²)), −1, 1)
      occ = ((nl·acos(x) − sqrt(k2·(h2 − 1))) / h2 + atan(sqrt(k2 / (h2 − 1)))) / π,  clamped 0..1
  else: occ = max(nl, 0) / h2
  ```

  Within 0.01 of the numerical integral everywhere.

- **Capsule** (segment a–b, radius r): the tube as a line 2r wide, the part of it above the tangent plane integrated
  in closed form, plus the two caps' spheres weighted by how much of each shows past the tube, and never less than
  the sphere at the segment's closest point:

  ```
  L = |b − a|;  L < 1e-4: S(a, r)
  u = (b − a) / L,  q = a − p,  s0 = −q·u,  w = q + s0·u,  h = |w|
  nw = n·w,  nu = n·u,  t0 = −s0,  t1 = L − s0
  |nu| < 1e-6: (nw ≤ 0: t1 = t0)        else: th = −nw / nu;  nu > 0: t0 = max(t0, th)  else: t1 = min(t1, th)
  F(t) = nw·(t / (2h²(h² + t²)) + atan(t / h) / (2h³)) − nu / (2(h² + t²))
  line = (t1 > t0 and h > 1e-4) ? 2r·h / π · (F(t1) − F(t0)) : 0
  wa = ½(1 − u·normalize(p − a)),  wb = ½(1 + u·normalize(p − b))
  occ = line + wa·S(a, r) + wb·S(b, r)
  occ = clamp(max(occ, S(a + clamp(s0, 0, L)·u, r)), 0, 1)
  ```

  The line term is (2r h / π) ∫ n·d / |d|⁴ along the segment's part above the horizon; for a long tube facing p it's
  exactly r / h. Within 0.05 of the integral at a radius or more from the surface (0.04 measured); nearer, within
  0.15 (a line 2r wide is a tube only from afar), mean error about 0.015. (The draft's "sphere at the closest point
  times (1 + L·|sin θ| / 2l)" was off by up to 0.6.)

- **Box** (centre c, half axis vectors X and Y, half extent ez along Z = normalize(X) × normalize(Y)), exact: in box
  space (p and n projected on the unit axes), 1 inside; else each face with p outside its plane (at most 3), its 4
  corners taken relative to p and clipped by the plane n·v ≥ 0 (Sutherland–Hodgman taken edge by edge: each edge's
  part above the plane, and the chord from where the face leaves it to where it enters), and the faces' form factor
  (Lambert):

  ```
  edge = atan2(|vi × vj|, vi·vj) · n·(vi × vj) / |vi × vj|      (j = i + 1; the same for any lengths of vi, vj)
  occ  = clamp(|Σ_faces side · Σ_edges edge| / 2π, 0, 1)
  ```

  Each face is taken the same way round as seen from outside the box (its corners' order times its side), so an
  edge two front faces share is taken once each way and its terms cancel: it is left out (`box_occlusion`), and only
  the outline's edges (at most 6) and the chords are found, about half the edges of the faces taken one by one.
  Within 0.02 of the integral everywhere (the residue is the integral's own sampling).

- **Reach** (`world_proxy_reach`), measured from the sphere's and box's centre and the capsule's midpoint: past it
  the occlusion is under 1/255 in every direction (a box's under 1/510).
  - sphere: 16r;
  - capsule: L/2 + 16·sqrt(r² + 2rL/π), a sphere of its silhouette's area side on;
  - box: its half diagonal sqrt(|X|² + |Y|² + ez²) + sqrt(510 · 4|A| / π), A = (e_y e_z, e_x e_z, e_x e_y) (below).

  The shader skips a shape after that one distance test. A sphere's and a capsule's term also fades to 0 over the
  last quarter of the reach, `fade = clamp((reach − |p − centre|) / (0.25·reach), 0, 1)`, on the level and on objects
  alike, so culling never shows an edge.

- **A box's bound** (`world_proxy_box_fade`). In box space, with o the point's distance outside each pair of faces
  (o_i = max(|p_i| − e_i, 0)), D = |o| and H the box's height above the point's tangent plane (its support along n
  less n·p), the box's occlusion is never more than

  ```
  U = (4 / π) (e_y e_z o_x + e_x e_z o_y + e_x e_y o_z) · min(D, H) / D⁴
  ```

  (its solid angle is its front faces', each at most its area 4 e_j e_k times o_i / D³, since every point of it is D
  or more away; and the cosine at p of any direction to the box is at most H / D). A box's fade is
  `clamp(510 U − 1, 0, 1)`: computed whole where U ≥ 1/255, not at all (skipped) where U ≤ 1/510. U falls as 1/D³ on a
  floor beside a box (H/D is small there) and past a flat or long box's edges, where 17 half diagonals reached far: on
  the ground about a Warthog the box is computed within about 6 wu, not 30. `tools/harness/tests/world_proxies.c`
  (`bound`) checks U is never under the occlusion at 20,000 random points.

- **The window's grid** (`world_ao_grid`, `world_ao_shading.c`). As the enhanced dynamic lights' grid: 16 × 16 × 8
  cells about the camera, boxes in the world (VR's eyes share it), their size log2(1 + |offset| / 1 wu) times each
  axis's scale, the outermost reaching 128 wu and holding whatever reaches past it. Each cell has a 64-bit mask of the
  proxies that can touch it: a sphere's and a capsule's reach, a box's own bounds grown by where its U falls under
  1/510 (if less). Built on the CPU for each window (64 shapes: microseconds), uploaded with the block (16 KB). A pixel
  finds its cell and runs only the proxies of its mask, lowest first. That leaves out only proxies whose factor is
  exactly 1 there, in the same order, so the product is the brute-force loop's bit for bit
  (`world_ao_proxies_occlusion_grid` against `world_ao_proxies_occlusion`: 0 of 160,000 points differ, in random
  windows of 64 shapes, in `tools/test_world_ao_light.py`).

**Combining:**

```
A_prox = Π_shapes (1 − occ_i · fade_i)                 (shapes of other owners in the pixel's cell; ≤64)
A      = max(mix(1, A_obj, fade) · A_prox, 0.2)   (models: fade the field's)
A      = max(A_prox, 0.2)                          (lightmap pass)
A      = mix(1, A, strength_{objects|level})
ambient' = ambient · A,  then the point lights / dynamic lights are added as #574 does
```

The CPU reference of all of it is `world_ao_occlusion` (`port/linux/game/world_ao_shading.c`), and both emitters'
shaders are checked against it at a made-up scene's points (`tools/test_world_ao_light.py`: the Metal functions'
text run as C++, the SPIR-V modules run by a small interpreter; within 1e-6 of it). The draw's owner slot is its
object's first shape's `[1].w` (`world_proxies_window`'s owners are the objects' datum handles); a draw with no
shapes of its own has −1, and the first-person weapon gets no world variant at all.

The strengths are in the block (`parameters[0]`): **subtle** objects 0.6 / level 0.5, **strong** 1.0 / 0.8. Those
two levels may come back as `"world_subtle"` and `"world_strong"` if D's captures call for it; v1 ships `"world"` as
the strong preset.

## GPU binding layout

| | Vulkan (SPIR-V) | Metal (MSL) |
| --- | --- | --- |
| Field: 3D, R8 snorm, linear, clamp | set `XGPU_SPIRV_SET_OCCLUSION` (3), binding `XGPU_SPIRV_BINDING_WORLD_SDF` (1), combined image sampler | `[[texture(XGPU_MSL_TEXTURE_WORLD_SDF)]]` (5), `constexpr sampler` |
| Block `struct xgpu_world_ao_block` (std140, 19,552 B) | set 3, binding `XGPU_SPIRV_BINDING_WORLD_AO` (2), uniform buffer | `[[buffer(XGPU_MSL_BUFFER_WORLD_AO)]]` (23) |
| Per draw: owner slot, reference | the draw uniforms (`xgpu_spirv_uniforms`, 2 floats) | `xgpu_msl_uniforms`, the same |

Set 3 is #574's occlusion set, so a draw still binds at most 4 sets, the Vulkan minimum. With no field yet, the
backend binds a 1×1×1 texel of +127 (far outside).

The block: `field[0]` origin.xyz and 1/voxel; `field[1]` width, height, depth, range; `parameters[0]` strength on
objects, strength on the level, fade, proxy count; `parameters[1]` reference weight and 3 spare; then 64 × 3 vec4
proxies; then the grid: `grid[0]` the camera, `grid[1]` each axis's scale and 1 / cell; then 1,024 uvec4 cells, two
cells' 64-bit masks each (cell i's word w at `cells[i / 2][2 (i % 2) + w]`).

**Command stream (version 16):**
- `XGPU_STREAM_WORLD_AO_WINDOW` carries the block (`active` 0: withdrawn);
- `XGPU_STREAM_WORLD_AO_FIELD` carries a slab of slices (`width` 0: release). The decoder checks the slab's bounds
  and size before any backend reads it.

The backends' `world_ao_window` and `world_ao_field` callbacks are still unset, so the records are decoded and
ignored.

## Settings

- `display.ambient_occlusion`: `"off"` (default) | `"subtle"` | `"strong"` (GTAO, #574, kept until the world mode is
  at least as good, then retired or kept as a fallback) | `"world"`.
- `"world"` doesn't run GTAO's pre-pass (d3d8_device.c reads it as GTAO off). It works in VR's eye passes and with
  Metal's multisampling, where GTAO can't.
- The menu (`tools/port_settings.py`, AMBIENT OCCLUSION) has WORLD.
- Classic's preset writes `"off"`, which bypasses everything.
- `debug.world_ao_view` (D): the surfaces it shades drawn as the term alone, in grey. `debug.world_ao_test` (D): a
  made-up field (a plane about the camera) and shapes in place of B's and C's, to check the shading alone. `debug.world_proxies_view`
  (C): the shapes as wireframes.
- A dedicated server or `debug.null_renderer` never builds the scene, bakes or uploads (`world_ao_mode` returns 0
  without a renderer).

## Performance budgets

These are added GPU ms per frame with `"world"` versus `"off"` (`debug.gpu_stats`), measured at c10's swamp and a
Blood Gulch firefight (16 bipeds, 4 vehicles):

| Device | Resolution | Budget | (GTAO strong, #574) |
| --- | --- | --- | --- |
| RTX 4090 | 1440p / 4K | ≤ 0.10 / ≤ 0.25 | 0.52 / 1.17 |
| Radeon 8060S | 1440p | ≤ 0.35 | 1.31 |
| Steam Deck | 1280×800 | ≤ 0.35 | (too costly to enable) |
| Phone (Adreno 7xx / Apple A15) | native | ≤ 0.5 | n/a |
| VR (both eyes, multiview, 90 Hz) | per headset | ≤ 0.4 for both eyes | not drawn in the eyes |

There's no pre-pass, so the CPU doesn't walk the scene twice either. CPU per frame: scene ≤ 0.2 ms, proxies ≤
0.1 ms, upload ≤ 512 KB a frame and only while a field arrives. Memory: field ≤ 8 MB of GPU memory and one block
(3 KB) per window. If the level's per-pixel proxy loop exceeds the budget on the Deck, the fix is a proxy grid like
the dynamic lights' (`XGPU_DYNAMIC_LIGHTS_GRID_*`, a 64-bit mask per cell), not fewer proxies.

## Determinism

The feature only ever writes:
- its own globals;
- the cache folder;
- the command stream.

The bake reads a copy of the tags. The proxies and the scene read the drawn (interpolated) state, which the
simulation never reads back. The checks before merging into master:
- `determinism_compare.py --builds --simulation` on a10 with the bot and the null renderer: branch versus master,
  setting off and `"world"`, every tick alike;
- off pixel-identical to master (0 pixels differ) in the offscreen Vulkan captures #574 used.

## Groundwork toward ray tracing

- **Scene with stable ids** (A): the static meshes become BLASes, the instances a TLAS. Ids are the same across
  frames, so a TLAS refit is enough.
- **Separated lighting terms:** ambient is done (#574). Direct dynamic lights, specular and emissive each get their
  own slot in the emitters, and RT shadows, RTAO, GI or reflections can then replace one of them. This is follow-up
  E, below.
- **Normals, material ids, motion vectors:** the instances' previous transforms (A) and the collision material ids
  are in the scene now; the G-buffer outputs are follow-up E.
- **World light list** (A).
- **Sub-pixel camera jitter** (E, with TAA).

## Workstreams

The integration branch is `world-ao`. Each stream branches from it and opens its PRs into it, never into master. The
draft PR `world-ao` → master stays a draft until every stream lands and the checks above pass.

**The shared files.** Each stream edits only its own files and the marked points named in its brief:
- `render.c`, `scenario.c` and `d3d8_device.c`;
- `xgpu_stream.h`, `xgpu_stream.h`, `host_render_stream.c` and `host_render_stream.h`;
- the backends' stream tables.

The interfaces in `port/linux/include/world_*.h` and `port/linux/src/xgpu_stream.h` are fixed. A stream that needs
one changed says so in its PR and in the other owners' threads first.

| Stream | Owns | Implements | Consumes |
| --- | --- | --- | --- |
| A Scene | `world_scene.c`, `world_scene.h`, `tests/world_scene.c`, `test_world_scene.py` | `world_scene.h` | game state, `render_interpolation.c` |
| B Field | `world_sdf.c`, `world_sdf_bake.c`, `world_sdf.h`, the backends' world AO resources (new `xgpu_vulkan_world_ao.c`, `xgpu_metal_world_ao.c`), the `world_ao_field` and `world_ao_window` callbacks, the start-up pass hook in `menu_pictures_render.c`, `tests/world_sdf.c`, `test_world_sdf.py` | `world_sdf.h`, the field and block bindings | `world_scene_static_triangles` |
| C Proxies | `world_proxies.c`, `world_proxies.h`, `tests/world_proxies.c`, `test_world_proxies.py` | `world_proxies.h` | `world_scene_instances`, model and collision tags |
| D Shading | `world_ao.c`, `world_ao.h`, the emitters (`xgpu_spirv.c`, `xgpu_msl.c`), the pixel key and draw uniforms, `d3d8_device.c`'s world section, the `world_ao_object_begin` hook in `render_objects`, the menu entry, `tests/world_ao.c`, `test_world_ao.py` | `world_ao.h`, the shading | all three, through the block |

### A. World scene representation

Fill in `port/linux/game/world_scene.c` behind the fixed interface `port/linux/include/world_scene.h`. Read
docs/world-ao.md's "The scene" first.

- **Static meshes:** at map load (`world_scene_map_loaded`, already called from `scenario_load` only while
  `display.ambient_occlusion = "world"`), give each structure BSP the id `WORLD_SCENE_ID_STATIC_BIT | index`.
  `world_scene_static_triangles` returns an owned world-space copy of a BSP's collision geometry
  (`collision_bsp` surfaces fanned by their edge rings, `source/physics/collision_bsp_definitions.h`) with per-triangle
  material ids and bounds. The bake runs it on another thread after the tags may be gone, so it must copy.
- **Instances:** each frame (`world_scene_frame_begin`, called from `render_frame` while the objects stand at their
  `render_interpolation.c` pose), list every drawn object. Fields:
  - id = its datum handle;
  - kind and object type;
  - definition tag index;
  - root node transform as drawn this frame and last frame;
  - bounding sphere as drawn;
  - lighting point (where `render_lighting` sampled its ambient: `render_objects_port_last_lighting` is the way in).
- **The world light list:** from the frame's ranked dynamic lights (`port/linux/game/dynamic_lights.c`) and the
  structure's light fixtures, with stable ids.

You own `world_scene.c`, `world_scene.h`, `tools/harness/tests/world_scene.c` and `test_world_scene.py`. Touch no
other file except to add a read-only accessor in `render_interpolation.c` or `dynamic_lights.c`, and say so in the
PR. It must stay render-only: never write game state, never allocate from game memory, nothing when the setting is
off.

Test it in isolation with the harness:
- ids stable over frames and never reused for a live object;
- an instance following its drawn node between ticks;
- the triangle copy matching a hand-built collision BSP;
- the empty case when off.

Then in game with a `debug.world_scene_log` count line and a `debug.world_scene_dump` OBJ of a BSP's triangles,
checked by eye in any mesh viewer on the 5900X. B and C consume your output, so land the triangle copy first, then
the instances.

### B. SDF bake, cache and upload

Implement the structure's distance field: `port/linux/game/world_sdf.c` (schedule, cache, upload) and
`port/linux/game/world_sdf_bake.c` (pure arithmetic), behind the fixed `port/linux/include/world_sdf.h`. Follow
docs/world-ao.md's "The distance field" exactly:
- voxel 0.25 wu, grown to stay within 8 Mi voxels;
- R8 snorm with range 12 voxels;
- exact narrow-band point-triangle distances, signed by the collision BSP's `bsp3d` point test run on a copy;
- slabs of z slices baked on low-priority worker threads.

The owner's hard rule: **never block or lengthen a load.**
- Bake in the first-launch "Preparing menu pictures" pass (#575), which already loads every stock map: call
  `world_sdf_startup_map_loaded` from `menu_pictures_render.c`'s `_prepare_loading` state, and make the pass run when
  only fields are missing via `world_sdf_startup_wanted`.
- At any other time, `world_sdf_map_loaded` only looks in the cache. On a miss it copies the triangles
  (`world_scene_static_triangles`) and bakes in the background after the load.
- Until the field is ready the feature runs with proxies alone. Then the field uploads a few slices a frame (at most
  512 KB) and fades in over 1 s.

Cache:
- path: `<data>/world_sdf/<map>_<bsp>.wsdf`;
- key: the map's cache-file checksum plus `WORLD_SDF_VERSION`;
- contents: the 64-byte header written field by field, the zlib-deflated voxels, then a CRC-32;
- writes go to a temp file renamed into place; a bad file is deleted and baked again;
- follow menu_pictures.c's conventions.

You also own the backends' resources for the feature:
- new `port/linux/vulkan/xgpu_vulkan_world_ao.c` and `port/macos/xgpu_metal_world_ao.c`;
- the 3D texture (with a 1×1×1 far-outside stand-in) and the block's uniform buffer at the bindings fixed in
  `xgpu_spirv.h`, `xgpu_msl.h` and `xgpu_metal_bindings.h` (set 3 bindings 1–2; texture 5, buffer 23);
- the `world_ao_field` and `world_ao_window` callbacks at the marked points in `xgpu_vulkan_stream.c` and
  `xgpu_metal_stream.m`.

Test in isolation:
- harness: plane, box room, sphere and a closed BSP-like box against exact distances (±voxel/2), plus encode/sample
  round trips;
- the cache's round trip and rejection cases;
- the real stock maps baked on the 5900X with a timing log.

Your PR must state the bake time and cache size for every stock map (5900X, Steam Deck, one phone) and show that
load times are unchanged (`halo_load_trace`) with an empty cache.

### C. Object proxies

Implement `port/linux/game/world_proxies.c` behind the fixed `port/linux/include/world_proxies.h`, following
docs/world-ao.md's "Proxies" and "Proxies' occlusion".

Per object definition, at most 16 shapes, made on first use from its tags and cached until `world_proxies_map_unloaded`:
- bipeds: capsules on limb bones (node to child node), a torso capsule and a head sphere, with radii from the
  collision model's pills and spheres where present;
- vehicles: up to 4 boxes from the collision model's region BSP bounds, plus wheel spheres;
- scenery, items and machines: one box;
- otherwise: a bounding sphere;
- nothing for projectiles and effects.

Each frame, `world_proxies_window` places the shapes on the node matrices as drawn. Those are the
`render_interpolation.c` pose; take them via A's `world_scene_instances` and the object's node array. It culls by
reach against the frustum, keeps the nearest 64, and fills the 3-vec4 GPU layout with owner slots.

Also own the reference arithmetic in `world_proxy_occlusion` and `world_proxy_reach`: the sphere is done. Add the
capsule (closest-point sphere with its length factor) and the box (clipped-face Lambert form factor). D's shaders
must match it line for line, so agree the text with D.

You own `world_proxies.c`, `world_proxies.h`, `tools/harness/tests/world_proxies.c` and `test_world_proxies.py`.
Don't touch the backends or emitters: your output reaches the GPU through `halo_world_ao_window`, which
`world_ao.c` (D) calls with your array.

Test in isolation:
- harness: each kind against a numerical hemisphere integral (within 0.05);
- reach and culling;
- the owner slot;
- placement on a fake skeleton between ticks;
- in game, `debug.world_proxies_view` draws the shapes as wireframes over the models, so fit is checked by eye on
  bipeds and every vehicle.

### D. Shading

Make the shaders evaluate the world term in #574's ambient slot, on both backends. Read docs/world-ao.md's
"Shading", "GPU binding layout" and "Settings", and #574's design (`xgpu.h`'s `XGPU_AMBIENT_OCCLUSION_LIGHT_*`).

Emitters (`port/linux/src/xgpu_spirv.c` and `xgpu_msl.c`):
- add a world source to the pixel key beside GTAO's texel;
- for `_LIGHT_MODEL` and `_LIGHT_VERTEX`: the SDF cone term relative to the object's reference, times other owners'
  proxies;
- for `_LIGHT_LIGHTMAP`: proxies alone;
- then the strength and fade from the block;
- read the field at set 3 binding 1 / texture 5 and the block at set 3 binding 2 / buffer 23;
- add the per-draw owner slot and reference to the draw uniforms.

The proxies' math must match `world_proxy_occlusion` (C) line for line, and the SDF sample must match
`world_sdf_sample` (B).

Front end and game side:
- own `port/linux/game/world_ao.c` and `world_ao.h`: the setting, building the window's block from B's
  `world_sdf_current` and fade and C's `world_proxies_window`, and `halo_world_ao_window`;
- own `d3d8_device.c`'s "world" section: pixel keys, the `halo_world_ao_object` uniforms, and keeping
  self-illuminated draws and the first-person weapon at 1;
- add the `world_ao_object_begin` hook in `render_objects`;
- add the menu's WORLD choice in `tools/port_settings.py`;
- add `debug.world_ao_view`.

Off must stay byte-identical shaders and pixel-identical frames, and Classic's `"off"` bypasses everything.

Test in isolation, without A, B or C, by feeding a synthetic block (a plane field and three hand-placed proxies)
through `halo_world_ao_window`:
- harness emitter tests in the style of `tools/test_ambient_occlusion_light.py`: no world code when off; the term
  multiplies only the ambient before any light is added, for each of the three draws; `spirv-val` passes;
- offscreen Vulkan captures on the 5900X;
- Metal builds in CI.

Report `debug.gpu_stats` against the budgets table.

### E. Follow-up (not in this round): G-buffer and lighting terms

Separate the direct dynamic lights, specular and emissive into their own shader slots, as #574 did for the ambient.
Then add per-pixel normal, material id and motion vector outputs to an extended pre-pass (motion from A's previous
transforms), and sub-pixel camera jitter for TAA and upscalers. It starts once D lands, because it changes the same
emitters.
