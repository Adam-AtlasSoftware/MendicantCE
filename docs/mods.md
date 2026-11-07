# Mods

A mod is a folder of Halo PC tag files that the game plays over a map's own
tags on your machine, as a resource pack does: sounds (a high-res audio
pack), bitmaps (a texture pack, a HUD redraw), text (a translation) and
models (higher-detail weapons, vehicles and scenery). Only the machine that
has a mod plays it. Nothing of a mod is sent to another machine, and a mod
changes nothing that the machines of a network game must agree on, so you
can play with mods in any game, with players who have other mods or none. (A host's mod sounds of other lengths change how long
the host's scripts and AI wait for dialogue to end; the host decides those
for every machine, so they still agree.)

The game's code is `port/linux/game/mods.c` (with `mod_sounds.c`,
`mod_bitmaps.c`, `mod_strings.c` and `mod_models.c`, one for each kind of
tag) and `port/linux/src/port_mods.c` (the mods folder and its files).

## A mod's folder

The mods are in the `mods` folder of the data root (the folder with
`maps/`; on Android the app's data folder). Each mod is a folder in it, with a `mod.toml` and a `tags`
folder:

```
mods/
  hd_audio/
    mod.toml
    tags/
      sound/sfx/weapons/assault rifle/fire.sound
      sound/dialog/...
  german/
    mod.toml
    tags/
      ui/hud/hud_item_messages.unicode_string_list
```

The files in `tags` have the paths of the tags they replace: a tag's name,
then its group's extension, as the Halo Editing Kit and Invader keep them.
`tags/sound/sfx/weapons/assault rifle/fire.sound` replaces the sound tag
`sound\sfx\weapons\assault rifle\fire` of every map that has one. Names are
not case-sensitive. The game only looks up the names of the map's tags among
a mod's files: a mod cannot add tags, and no tag name of a map makes a path
outside the mod.

`mod.toml` says what the mod is. Every key is optional:

```toml
name = "High-res audio"       # what the Mods menu shows (else the folder's name)
id = "hd_audio"               # what other mods name it by (else the folder's name)
version = "1.0"
authors = ["Someone", "Someone else"]   # or one string
description = "The game's sounds at 44 kHz"
maps = ["a10", "bloodgulch"]  # the maps it is for, by their files' names (else every map)
load_after = ["hd_textures"]  # mods it plays over: they load before it
load_before = ["fixes"]       # mods that play over it: they load after it
incompatible = ["other_audio"]  # mods it cannot be played with
format = 1                    # the mods' format it is made for
```

`load_after`, `load_before` and `incompatible` name other mods by their `id`
or their folder's name, not case-sensitive. A mod they name that is not in
the mods folder is passed over. `maps = []` is every map.

A folder without `mod.toml` is not a mod. A mod made for a later `format`
than the build's, or whose `maps` names no map, is not played. The game
logs anything else wrong in `mod.toml` (an unknown key, a value of the
wrong type) and uses the rest; a `mod.toml` that is not TOML at all is
logged, and the mod played without what it says.

The game's log is its output: the terminal on Linux, `halo.log` beside
`halo.exe` on Windows, logcat on Android, and the page's log on the web.

Limits: 256 mods, folder names of 63 bytes at most, 262144 files in all, 24
folders deep under `tags`, and 64 maps in a mod's `maps`.

## Existing loose sound files

`audio.loose_sounds = true` (or `HALO_LOOSE_SOUNDS=1`) still loads sound
files directly from the data root's `tags/` folder; no `mod.toml` is needed.
These files take priority over mod packs. `loose_sounds_reload` reads them
again; `loose_sounds false` reveals a mod pack's sound, or the map's sound
when no mod replaces it. `loose_sounds true` enables them until the next map,
which uses the setting again. `mods_enable false` disables mod packs while
leaving loose sounds enabled. Both commands must be off to hear only the
map's original sounds.

## The load order

Every mod in the mods folder is on until you turn it off. The mods load in
an order, and where two mods have a file for the same tag, the one loaded
later plays.

- The default order is the folders' names in alphabetical order (not
  case-sensitive), except that the mods a mod must load after (those it
  asks to load after, and those that ask to load before it) are taken to
  just before it. Mods that ask in a ring (a after b, b after a) load
  alphabetically, and the game's log says so.
- **Manage Mods** in Settings' Mods tab (in the PC version's menus, `display.menus =
  "pc"`, the default) is laid out as the game lobby is: the mods on the
  left as a resource pack list has them, the one at the top playing over
  all those below it (the load order upside down: the mod loaded last is at
  the top), and the chosen mod's details in the frame on the right (its
  name, what is wrong with it, whether it is on, its priority, 1 the top,
  and its version, authors, folder, maps and description). Choosing a mod
  (A, or a click on the chosen one: the first click chooses it) turns it on
  or off (ON, OFF); left and right, or **Move up** and **Move down**, move
  the chosen mod (up: over one more mod). **OK** keeps the order and plays
  it at once (on a map that is loaded, too); **Cancel** leaves it as it was.
- A warning sign beside a mod means it is on the wrong side of a mod it
  asks to load before or after (both on): a mod that loads after another is
  above it in the menu. A red cross means a mod that is on says it cannot be
  played with this one, or that it cannot be played at all (made for a later
  build, or its `maps` names none): it stays off. The details say which.

The menu keeps the order, and the mods turned off, in `mods/load_order.toml`,
by the mods' folders' names:

```toml
order = ["german", "hd_audio", "hd_textures"]
disabled = ["hd_textures"]
```

A mod added to the folder later is put where the default order puts it,
after the mod before it there (but on the side it asks for of the mods it
names), and is on. A mod gone from the folder is left out. When the file
cannot be written, the game says so in its log and keeps the order until it
quits.

### Mods that cannot be played together

When a mod that is on names another that is on as `incompatible`, the game
asks as it starts, before the window opens:

- **Play anyway** plays them all (closing the box does too);
- **Turn off ...** turns off the mods named as incompatible (if mod A says
  it cannot be played with mod B, B is turned off), and keeps that in
  `load_order.toml`;
- **Quit** closes the game.

The Mods menu turns them on again. On the web, **Quit** starts the page
again. A run nobody watches (`debug.hidden_window`, `debug.null_renderer`,
`debug.exit_after` or `debug.network_test`) is not asked: it plays them all,
and the game's log says which conflict.

At the console:

| Command | What it does |
| --- | --- |
| `mods_list` | Lists the mods in the mods folder in their load order, and which are on, then how many of the map's string lists, bitmaps, sounds and models the mods play. |
| `mods_reload` | Finds the mods again and reads their files again: a changed file is read again (one that now fails a check leaves what was played before), a new one is played, and the map's tag comes back for a file that is gone. If a sound changed, all sounds stop. It does nothing while `mods_enable false`. |
| `mods_enable false` | Disables mod packs (loose sounds remain independent) until `mods_enable true` or the next map. |

These commands work in any game, a client's too: they change nothing of the
game.

### Getting a mod onto each platform

- **Linux, Windows**: put the mod's folder in `mods/` next to `maps/`.
- **macOS app**: put it in `~/Library/Application Support/OpenH1/mods/`,
  next to the app's `maps` link. With `HALO_DATA_ROOT`, use `mods/` in that
  data root instead.
- **Android**: put it in `mods/` in the app's data folder,
  `/sdcard/Android/data/com.openh1.openh1/files/mods`, for example with
  `adb push hd_audio /sdcard/Android/data/com.openh1.openh1/files/mods/`.

## What a mod can replace

The game plays these groups from a mod. It does not read the files of any
other group (a `.weapon`, `.vehicle`, `.model_collision_geometry` or
`.scenario`, for example): they would change what the machines of a network
game must agree on.

| Group | Extension | What it is |
| --- | --- | --- |
| Sound | `.sound` | Sounds, music and dialogue |
| Bitmap | `.bitmap` | Textures, sprites, the HUD's pictures |
| Unicode string list | `.unicode_string_list` | The text the game shows |
| String list | `.string_list` | Other text |
| Model | `.gbxmodel` (Halo PC's), `.model` (the Xbox's) | What objects look like: weapons, first-person weapons and arms, vehicles, scenery, characters |

A file must be a Halo PC tag file of its group, as the Halo Editing Kit's
tools and Invader write them (a model also as `invader-extract` writes an
Xbox map's: `.model`). The game checks every count, size and offset
in it against the file, and every value against what the game reads, before
it uses the file. A file that fails a check is left out (the map's tag
plays), and `debug.txt` and the console say why.

A mod may also carry MENDICANT AI's models, `tags/mendicant/models.aibw`
(the file `ai.mendicant.weights` names, written by
`tools/mendicant/export_models.py`). It is no tag: only the
host of a campaign or co-op game with `ai.mendicant.enabled` reads it, and
it changes only that host's AI.

### Sounds

- The samples can be 16-bit PCM, Xbox ADPCM or Ogg Vorbis, mono or stereo,
  at 22 or 44 kHz. The game converts them to the Xbox ADPCM that it plays,
  as it does for Custom Edition maps: mono at 22 kHz, stereo at its own rate.
- Empty values are filled in as a map's build fills them in: the defaults
  of the sound's class, the playback rates and the bend.
- A sound must have permutations: a file with an empty pitch range (some
  Halo PC translations have them) is left out.
- A permutation must fit in a block of the sound cache (one sound's samples:
  `MAXIMUM_SOUND_DATA_SIZE`).

### Bitmaps

- A bitmap's types and formats are those the game draws: 2D, 3D and cube
  maps; A8, Y8, AY8, A8Y8, R5G6B5, A1R5G5B5, A4R4G4B4, X8R8G8B8, A8R8G8B8,
  DXT1, DXT3, DXT5 and P8 bump. Sides are 4096 at most (512 for a 3D
  bitmap). A bitmap that is not a power of two on each side is drawn
  linear, as a Custom Edition map's is: it must be an uncompressed 2D
  bitmap.
- One bitmap's pixels may take a quarter of the texture cache a map with
  mods' bitmaps has: 64 MB on Linux, Windows and Macs; on Android, iOS and
  64-bit ARM Linux, from 16 MB to 64 MB by the device's memory (a 2048 x
  2048 DXT5 with its mipmaps fits on all of them).
- A bitmap replaces the map's bitmap of its name, and the map's other tags
  use it as the map's: it must have at least as many bitmaps, of the same
  types, and as many sequences, each with at least as many sprites. It can
  be larger: a redraw at twice the size is drawn at the map's size, sharper.
- The HUD draws a bitmap at its own size. So for each HUD element that draws
  a mod's bitmap, the game scales the element by the map's size of what it
  draws (its sprite, or with no sprites its bitmap) to the mod's, when that
  is the same ratio for all of them. A bitmap the HUD draws that is not the
  map's redrawn so (Halo PC's HUD bitmaps lay their sprites out to other
  sizes) is left out. The HUD's digits can be the map's size or twice it.
- The menus draw a bitmap in their widgets' bounds, whatever its size.
- Halo PC keeps the channels of a model shader's multipurpose maps and of
  the HUD's meters in another order than the Xbox. The game finds those
  bitmaps among the mod's and draws them in the Xbox's order, as it does for
  Custom Edition maps (`custom_edition_bitmaps.c`).
- The pixels are read from the file when the texture cache loads them, not
  kept in memory.

### Text

- A list keeps the map's number of strings: a mod's strings past it are
  left out, and the map's own strings stay where the mod's list is shorter.
- The game uses many strings as the format of what it prints
  (`%s was killed by %s`). A mod's string replaces the map's only if it has
  the same conversions in the same order (`%s`, `%d`, ...); otherwise the
  map's string stays. A translation keeps the conversions of the original.
- Unicode strings are UTF-16, as the tool writes them.
- The main menu is the port's own (the PC version's menus, `port/assets/menus`),
  whose text is in tags named `pc\...`. A mod's `ui\shell` lists change the
  Xbox's menus (`display.menus = "xbox"`) and the text shown while playing
  (HUD messages, the multiplayer messages, the pause menu).

### Models

A model is more than what is drawn: its nodes (its skeleton) are what its
animations move, its markers are where projectiles leave a weapon, where a
unit sits and where things attach, and its regions and permutations are what
damage and an object's variant choose among. The machines of a network game
agree on those, so a mod's model must have the map's:

- the same nodes: their names, hierarchy and default positions;
- the same markers: their names, regions, permutations, nodes, positions
  and rotations (on the model's permutations, as the Halo Editing Kit keeps
  them, or on the model, as a map does);
- the same regions and permutations: their names and flags.

Positions and rotations may differ from the map's by 0.001 at most (a model
exported again is not always bit for bit the map's). Only what is drawn is
the mod's: the geometries, their parts and vertices, which geometry each
permutation draws at each detail level, the detail levels' cutoffs, the
base map's scale, and which shaders each part is drawn with. A detail level
the map draws must draw one of the mod's geometries, and one the map draws
nothing at stays so. Every shader must be one the map has (a mod cannot add
tags); a mod's bitmaps can redraw them.

The model is checked against each map's own model of its name as that map
loads. On a map whose model of that name has other nodes, markers or
regions (a custom map's own assault rifle, say), or lacks a shader the mod
names, the mod's model is left out and the map's drawn; the mod's other
files still play there. `maps` in `mod.toml` keeps a mod to the maps it is
made for.

- Where a mod has both, its `.gbxmodel` plays; otherwise the file of the mod
  loaded later, whichever its format.
- What the tool works out as it builds a map is worked out as it does: a
  part's strip is its triangles block, its centroid's nodes are its most
  weighted, its links of 0 to the parts drawn before and after it are none,
  a base map scale of 0 is 1, and a model whose tag asks for it has the
  normals of its vertices that share a position blended. The vertices are
  compressed as the game draws them.
- A part's links to the parts drawn before and after it (for transparency)
  must not go round.
- A model of 44 nodes or more (the renderer skins 43 at once; only Custom
  Edition maps' have so many) is drawn a part's own nodes at a time, as a
  Custom Edition map's is: a part's vertices may name 22 nodes at most.
- The collision geometry, the animations and the object's tag are the map's:
  a larger weapon still hits and is hit where the map's does, and an object
  is still left undrawn by the map's bounds of it (a much larger model may
  vanish at the screen's edges).
- Every model made from a file is kept until the map changes, a file edited
  and read again by `mods_reload` too, and counts against the mods' memory.

### Shaders

A model's parts can be drawn with a shader of every group Halo PC has but
one: `shader_transparent_chicago_extended`, which Gearbox added for Halo PC.
The Xbox game does not have it, so do not use it: use a
`shader_transparent_chicago` shader instead. (A Custom Edition map's
chicago extended shaders become chicago shaders as the map loads, keeping
only their four-stage maps: see
[custom_edition_caches.md](custom_edition_caches.md).) As for every
shader, it must be one the map has.

### The material overlay

A mod can also give the model shaders (`shader_model`) of a map a material
overlay, which `display.prism_material_overlay` (PRISM, the Vulkan renderer)
draws: the fine grain of a surface, a weapon's machining or a suit's weave,
lit at each pixel without changing any color. It is not a tag. The mod's
`tags/materials/overlay.json` names the shaders by their tag paths and gives
each one to four classes, and each class a tile in `tags/materials/` (an
8-bit RGBA PNG: red and green the tangent-space normal's x and y, green up
the picture; blue the cavity, half of it; alpha the roughness offset; 0.5
none in each), its size in world units, its strengths and the distances
it fades over. A class may also give its tile's every mip level, as the
tools filtered them, in two KTX2 files of BC5 blocks (`levels`: the normal,
and the cavity and roughness, whose smaller levels grow rougher where the
fine normals average out), which are drawn instead of the PNG's averages.
The tiles are read as the map loads and decoded on a thread of their own;
a draw goes without the overlay until its tiles are ready. Two classes are
mixed by a channel of the shader's multipurpose map. A shader can also name
up to four classes and a class mask for its base map (`class_mask`: an RGBA
PNG at the base map's coordinates, red to alpha the four classes' weights),
which is found in any mod, so that one built on the player's machine from
the player's own maps need not be shared: the channel still decides the
first class, and the mask shares the rest among the others. Until the mask
is there, the shader draws with its first two classes and the channel. The
file's format is described at the top of `port/linux/game/mod_materials.c`;
the overlay.json of the mod loaded last that has one is used, and
`mods_reload` reads it again. The settings are read each frame:
`prism_set "material_overlay true"` at the console turns it on in a running
game, and `prism_set "overlay_view 1"` shows which class each texel takes.
With the setting off, every draw is the game's. The Mac's Metal renderer
draws the overlaid shaders as they are, until its shaders are checked
against Vulkan's.

### Textures

`display.prism_textures` (PRISM's texture store; `stock` by default) draws
replacement textures a mod ships in its `textures` folder, beside `tags`:

    mods/<mod>/textures/<tier>/<the bitmap tag's path>#<bitmap index>.ktx2
    mods/prism_chief/textures/max/characters/cyborg/bitmaps/cyborg#0.ktx2

The tiers are `enhanced` and `max`; `max` takes a bitmap's `enhanced` file
where it has no `max` one. The mod loaded last that has a bitmap's file
wins, as for tag files, and a mod's `maps` apply.

A file is a KTX2 of BC1, BC3, BC4, BC5 or BC7 blocks (ASTC 4x4 on devices
without BC: most Android phones), with no supercompression, every level of
its chain (`levelCount` floor(log2(the longer side)) + 1), one face (six
for a cube map), no layers or depth, and the stock bitmap's size times 2^k
on both sides (the same k, 0 or more), at most 4096 on a side. `_SRGB`
formats are sampled as UNORM, as the stock textures are. A replacement is
sampled as its blocks decode, whatever the stock bitmap's format: BC4 gives
red alone (green and blue 0, alpha 1) and BC5 red and green (blue 0, alpha
1), so they suit only bitmaps whose shaders read those channels; a bump
map, whose shader reads all three of its normal's, a Y8 or A8 bitmap
(stock grey or alpha alone, which the stock decoding spreads to the other
channels) and anything with alpha need BC1, BC3 or BC7 with every channel
as the stock decodes it. The bitmaps that
can take one are 2D or cube maps whose sides are powers of two, not
palettized (P8) and not linear; the HUD's and the menus' high-res pictures
come first (`port/assets/hud`).

The game goes on loading the stock bitmap (or a mod's `.bitmap` of it):
it is drawn until the replacement's levels down from the stock
resolution are in the graphics card's memory, and it is where every size
the game computes with comes from. The host reads the files itself, straight
into the card's memory, never into the game's: the game's 256 MB texture
cache and its 64 MB desktop cache are not touched, and a file can be as
large as the card holds. Each level of the replacement stands for the stock
level of its size (level j + k for the stock's j), so a texture far away
looks as the stock does and only one near is sharper. The files are checked
as the map loads (the host's log says which were refused and why: size,
format, unreadable, changed, device, path), and read as they are first drawn,
on worker threads, the smallest levels first.

`display.prism_texture_budget_mb` (0: six tenths of what the card has free,
between 128 MB and 6 GB) bounds the card's memory they take; past it, those
not drawn in the last 10 frames go back to the stock resolution, then to the
stock texture, the least recently drawn first. Both settings apply in a
running game (`prism_set "textures max"`). Only the Vulkan renderer has the
store; Metal's (macOS, iOS) draws the stock textures. Textures are drawn,
not simulated: a network game's machines need not have the same files.
`tools/texstore_synth.py` makes placeholder files from an extract's PNGs
(upscaled, BC7), for tests.

### Meshes

`display.prism_models` (PRISM's mesh store; `stock` by default) draws
replacement meshes a mod ships in its `meshes` folder, beside `tags`:

    mods/<mod>/meshes/<tier>/<the model tag's path>.pmesh
    mods/prism_chief/meshes/max/characters/cyborg/cyborg.pmesh

The tiers, the load order and a mod's `maps` are as for textures. A `.pmesh`
(its format: `port/linux/vulkan/xgpu_mesh_store.h`; written and checked by
`tools/pmesh.py`) has none of the stock model's limits: 32-bit indices, any
number of vertices and triangles, four bone weights a vertex, up to 1024
bones, tangents and two texture coordinate sets, and levels of detail of its
own, each drawn from a size in pixels it names. Its skeleton is the model's
own nodes (the same names, parents and default pose: the file carries their
checksum, which must match the map's model) followed by any helper bones
(driven by the rig in the file's `RIG` section: `docs/prism_rigs.md`; a helper
without a rule follows its parent); its pieces each
replace the model's parts of one region, permutation and shader.

The game goes on loading, simulating and drawing the stock model: its nodes
are animated as they always are, and its collision, markers and attachments
are the stock model's. The renderer reads the file itself, straight into the
graphics card's memory (none of it in the game's memory, its 128 MB pool of
mods' geometry or the per-draw stream), and each frame skins the mesh by the
nodes' matrices the game draws the model with, then draws it in place of the
model's parts with their own shaders, passes, shadows and fog. The stock
model is drawn until the mesh is in, at a region and permutation the mesh has
nothing for, and on a renderer without the store (Metal's). The files are
checked as the map loads (the host's log says which were refused and why:
format, nodes, a piece no part of the model has, unreadable, changed, budget,
device, path) and read as they are first drawn. A mesh is skinned once a
frame: an object's shadow is drawn at the level of detail its view was, so
the two share the skinning. The stock part's vertex holds texture
coordinates over the model's base map scale (16 bits each), so a uv0 past
that scale is clamped there (the log counts them); a piece whose shader has
a material (below) is drawn by its pipeline's wide variant, which reads a
second stream the skinning writes: the mesh's uv0 as floats (any range, full
precision) in place of the stock vertex's coordinates, and, where the
shader hands on few enough values for it to stay fast (32 numbers a vertex
with it; not the model shaders lit for each pixel, which hand on 32
already), the mesh's own tangents for the normal map's frame instead of the
screen's derivatives (`HALO_MESH_WIDE_OFF=1`: not, for comparisons). The objects that draw the model are culled by the mesh's radius
where it is larger (their render-only radius; the bounding radius physics,
the AI and the network read is left as it is).

`display.prism_model_budget_mb` (0: a fifth of what the card has free,
between 128 MB and 1 GB) bounds the card's memory they take; a mesh past it
is drawn stock, and one not drawn in the last 10 frames is let go of for
another. Both settings apply in a running game (`prism_set "models max"`).
Meshes are drawn, not simulated: a network game's machines need not have the
same files.

The console's `prism_set "debug_pose <frame> <animation>"` puts the first player's unit in one of its own
graph's base animations at a frame and poses its nodes at once (no tick: a stopped game shows the pose,
without the animation's root motion), for checking a replacement mesh's deformation in captures. It changes
the game's state, so a network game refuses it.

`debug.prism_mesh_dump` writes a map's models as the game draws them (their
parts' vertices and strips, nodes, regions, permutations, shaders, markers)
to the data folder's `prism_dump`, which `tools/pmesh_from_dump.py` makes a
`.pmesh` of (the stock geometry as it is, or split into many more triangles,
for tests), and from which the art pipeline's Blender exporter
(`prism_pmesh_export.py`: `import-dump` makes a test scene, `export` writes a
scene's armature, meshes, weights, levels and materials as a `.pmesh` and
warns of markers the new surface swallows) checks a remodel against the map's
model. A model's data is the map's: keep what these write to yourself.

### Materials

`display.prism_materials` (PRISM's materials; `stock` by default) lights a
model shader by a material a mod gives it, in its `materials` folder,
beside `tags` and `textures`:

    mods/<mod>/materials/<the shader tag's path>.material.json
    mods/prism_chief/materials/characters/cyborg/shaders/armor.material.json

```json
{ "format": 1,
  "albedo": "textures/materials/cyborg/armor_albedo.ktx2", "albedo_factor": [1, 1, 1],
  "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 1.0,
  "orm": "textures/materials/cyborg/armor_orm.ktx2",
  "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
  "emissive": null, "emissive_factor": [1, 1, 1],
  "clearcoat": { "strength": 1.0, "roughness": 0.05, "mask": null },
  "detail": { "map": "textures/materials/tiles/weave.ktx2", "scale": 64,
              "normal": 0.15, "cavity": 0.2, "roughness": 0.2, "mask": "all" },
  "reflection": 1.0, "specular": 0.5, "wrap": true, "direct_specular": 1.0, "ce_reflection": 1.0,
  "multipurpose": { "specular_mask": true, "self_illumination": true, "reflection_tint": true } }
```

The images are KTX2 files of the mod's (a path in the mod's folder; the
texture store reads them, as it reads the replacement textures, straight
into the graphics card's memory, up to 8192 on a side, drawn once their
levels from 256 texels down are in): the albedo (the base map's colours:
it replaces texture 0's colour, its alpha, the stock base map's, kept),
the normal (tangent space, red and green its x and y, y up the picture),
the ORM (red the occlusion, green the roughness, blue the metal, as
glTF's: the factors multiply them, and are the values without it), the
emissive, the clear coat's mask (red) and a detail tile (red and green its
normal's slopes, blue the cavity, alpha a roughness offset: the material
overlay's packing, repeated `scale` times across the base map's
coordinates). A shader the material overlay names too (`overlay.json`)
takes the overlay's tiles as its finest detail; a surface map
(`display.prism_surface_maps`) of a shader with a material is not drawn:
the material's normal and occlusion are the shader's.

The shader's draws are lit for each pixel by a GGX path: Lambert diffuse
and GGX specular for the level's own lights as CE gives the model (its
ambient, two distant and two point lights, or the enhanced dynamic lights,
diffuse), the world's occlusion and the ORM's, and the shader's reflection
cube at the level of the roughness, by the split sum, or (`ce_reflection`
1) whole, as CE's shader adds it, so that the art's brightness holds; the
clear coat is a second lobe on the vertices' normal (a visor's). CE's
combiners still make the colour: they apply the shader's detail map,
change colour, self-illumination, specular mask (`specular_mask`), its
reflection tint and brightness (`reflection_tint`) and the fog, as the
shader says. `specular` is the dielectric's reflectance (F0 0.08 times it),
`wrap` keeps CE's translucent back light on the first distant light.

Materials are drawn, not simulated: a network game's machines need not
have the same ones, and nothing is sent between them. Only the Vulkan
renderer draws them (Metal's draws the shaders as the Xbox did); the
setting applies in a running game (`prism_set "materials pbr"`).
`tools/prism_material_pack.py` makes a material's images (BC7) from
pictures, and the Chief's armour and visor materials from the player's own
prototype pictures (local only).

## Making a mod

1. Get the tags you want to change as tag files. `invader-extract` extracts
   them from a map, and the Halo Editing Kit's tags are Halo PC's own.
2. Change them with the Halo Editing Kit or Invader (`invader-bitmap`,
   `invader-sound` and `invader-model` build bitmaps, sounds and models from
   images, audio files and JMS files). Keep each tag's name.
3. Put them in `tags/` of a folder in `mods/`, with a `mod.toml`.
4. Load a map (a new mod is on). `mods_reload` at the console reads the
   files again as you change them, without loading the map again.

`debug.txt` has a line for every file the game leaves out, and why, and a
summary of what each kind of tag plays from the mods:

```
mods: 9 string lists from their files (9 new), 0 given back, 0 left out
mods: 66 HUD placements scaled to draw larger bitmaps at the map's size
mods: 354 bitmaps from their files (354 new), 0 given back, 6 left out
mods: 313 sounds from their files (313 new), 0 given back, 2 left out
mods: 4 models from their files (4 new), 0 given back, 1 left out
```

## How it operates

- When a map's tags load, at `mods_reload` and at the Mods menu's **OK**,
  the game lists the files of the mods that are on, once, in a table of
  their tag paths. Each of the map's sound, bitmap, string list and model
  tags is looked up in it.
- A tag replaced by a mod takes the file's values in place, so everything
  that holds the tag keeps it, and the map's own values come back when the
  file goes. A sound playing or a texture in the cache is never changed
  under the game: before a reload changes anything, the sounds stop and the
  caches let go of what they hold.
- The mods' tags keep at most 512 MB in memory (192 MB on Android and the
  web): their files, their sounds' samples, their bitmaps' tags and their
  models' geometry.
- A Custom Edition map takes mods as an Xbox map does.
