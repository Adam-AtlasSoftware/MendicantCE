# Forge map variants (`.mvar`)

SaftiGlas's format, from his Forge
([halo-ce-universal](https://github.com/SaftiGlas/halo-ce-universal), main
`173fc93ae`), which OpenH1 reads and writes unchanged.

Forge (`port/linux/game/forge_layout.c`) saves what it changed in a map as a
**map variant**, as Halo 3's Forge does: a file of its own in the
`forge_maps` folder beside `custom_maps`, in the data root (`d:\forge_maps\`
to the game). The format is written and read by
`port/linux/game/forge_mvar.c` (`forge_mvar.h`).

Only the game's machine writes variants: a local game, or the host of a
system link or internet game. The host sends the variant to each machine that
joins, and again whenever it changes, as the same format (without the
editor's notes). The other machines apply it and save nothing.

## Files

- `forge_maps/<map>_<name>.mvar`: `<map>` is the base map's short name
  (`bloodgulch`), and `<name>` is the variant's name when it was first saved,
  with characters other than letters, digits and `-` written as `_`. A number
  is added when that file exists already. Renaming a variant changes its
  `mvhd`, not its file name.
- A map's Map tab lists the variants whose `mvhd` names that map, up to 64, in
  the order of their file names.
- The layouts of before, `u/forge/<map>/layout_NN.txt` in the save root, are
  converted to variants once, when the variants are first listed. The text
  files are left where they are, and `u/forge/<map>/converted.txt` records
  which slots were converted (a line `NN file` each).
- A file is written next to its destination as `<file>.new` first, then
  renamed into place, so a game stopped partway through leaves the old file
  intact.

## Layout

All numbers are little-endian: `u8`, `u16`, `u32`, `s8`, and `f32` (IEEE 754
single). Text fields have a fixed size, are padded with zeros, and always
end within their field. Every field is written separately, never a C
structure as it sits in memory, so the 32-bit x86, ARM64 (ILP32) and
WebAssembly builds all write the same bytes.

A file is a sequence of chunks. Each chunk is a 12-byte header followed by
its contents:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 4 chars | tag |
| 4 | u32 | size of the contents (not counting this header) |
| 8 | u16 | version (1) |
| 10 | u16 | flags (0) |

Chunks appear in this order:

| Tag | Contents |
| --- | --- |
| `_blf` | `"mvar"`, u32 length of the whole file, u32 CRC-32 (IEEE, as zip's) of every byte after this chunk |
| `mvhd` | the variant (below) |
| `strt` | u32 count, then each string: u16 length, its characters (no terminator, at most 255): the paths of the tags the objects name, each path once |
| `mvob` | u32 count, u16 record size (64), u16 0, then the object records (below) |
| `mved` | u32 count, u16 record size (72), u16 0, then the editor's records (below); left out of what a host sends |
| `mvst` | u32 count, then for each Forge part: name (16 chars), u32 length, its settings as lines of text |
| `_eof` | nothing |

`strt` must come before `mvob`, and `mvob` before `mved`, since each refers
to the one before it. A reader skips chunks it does not know. If a record is longer than the
record size this version writes, the reader reads the fields it knows and
skips the rest, so later versions can add fields. A file is refused as a
whole when its `_blf` is wrong: the magic, a version newer than 1, the
length, or the CRC. A file is also refused when it is larger than 256 KB, or
when a chunk runs past the end of the file. Every count, index, number and
text is checked before it is used, the same way the game checks what another
machine sends. A position or turn that cannot be valid leaves out only that
object.

### `mvhd` (496 bytes)

| Type | Field |
| --- | --- |
| 64 chars | name |
| 128 chars | description (the map list shows it; empty: "A Forge map made on ...") |
| 32 chars | author (local player 1's name when the variant was first saved) |
| 256 chars | base map's scenario (`levels\test\bloodgulch\bloodgulch`) |
| u32 | created, seconds since 1970 (UTC) |
| u32 | modified, the same |
| u8 | 1: shown in the multiplayer map list |
| u8, u16 | 0 |
| u32 | the id the next object gets |

### Object records (`mvob`, 64 bytes each)

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | **id**: unique within the variant, never reused while the variant is edited |
| 4 | u8 | kind: 0 placed (made by Forge or a mod), 1 moved (one of the map's own), 2 removed (one of the map's own) |
| 5 | u8 | **type**: the object type of the tag: 0 biped, 1 vehicle, 2 weapon, 3 equipment, 4 garbage, 5 projectile, 6 scenery, 7 machine, 8 control, 9 light fixture, 10 placeholder, 11 sound scenery (255 unknown, in a converted layout). For information only: a reader uses the tag's own type |
| 6 | s8 | **team**: −1 (0xff) none, 0 red, 1 blue, up to 15 |
| 7 | u8 | **spawn flags**: bit 0 placed at start, bit 1 never respawn; the other bits are 0 |
| 8 | u32 | the tag's group, four letters as a number (`'scen'`) |
| 12 | u16 | **tag**: the index of its path in `strt` |
| 14 | u16 | respawn time in seconds (0: the game type's); stored, not used yet |
| 16 | u16 | game types: bit *n* for game engine *n* (1 CTF, 2 Slayer, 3 Oddball, 4 King of the Hill, 5 Race); 0 every one |
| 18 | u16 | 0 |
| 20 | 3 × f32 | **position**: world x, y, z |
| 32 | 4 × f32 | **rotation**: a unit quaternion x, y, z, w (below) |
| 48 | f32 | **scale**: 0 for the tag's own (a moved object's: the map's own); 0.01 to 100 |
| 52 | 3 × f32 | original: for moved and removed objects, where the map places them; the object is found again by its tag and this place (within 0.05 world units) |

When a variant loads:

- **Placed** objects are created with the variant's tag, position, rotation
  and scale. While Forge is off, an object with "placed at start" off, or
  whose game types do not include the current game type, is not created.
  Saving the variant again keeps it as it was.
- **Moved** objects (the map's own) are put at the variant's place and turn,
  and given its scale when it is not 0.
- **Removed** objects (the map's own) are deleted. A game type's flag or
  ball is never deleted.

### Rotation

The rotation is the unit quaternion (x, y, z, w) that turns the world's axes
x, y and z onto the object's forward, left and up vectors, in the engine's
right-handed axes (z up). It is written with w ≥ 0. Converting back:

```
forward = (1 − 2(y² + z²),  2(xy + wz),       2(xz − wy))
left    = (2(xy − wz),      1 − 2(x² + z²),   2(yz + wx))
up      = (2(xz + wy),      2(yz − wx),       1 − 2(x² + y²))
```

A quaternion is used rather than Euler angles because it has no gimbal lock,
no question of angle order, and stores any orientation in four numbers. The
popup's Turn, Tilt and Roll rows still show and change angles. A reader
normalizes the quaternion and refuses one whose squared length is not
between 0.5 and 2 (or that is not made of numbers).

### Forge settings (`mvst`)

Each Forge part (`halo_mod.h`: `layout_save` and `layout_load`) keeps its
settings as lines of text, as it did in the text layouts. The parts are
`forge_spawns` (player and item spawns), `forge_zones`, `forge_ai`,
`gravity` and `sky` (SaftiGlas's build's; OpenH1 has no sky part, and
skips it). A part the game does not have is skipped. Each line is
read with that part's own checks.

### Editor's notes (`mved`, 72 bytes each)

| Type | Field |
| --- | --- |
| u32 | the object's id |
| u16 | group (0 none, up to 99) |
| u8 | flags: bit 0 locked (kept, not used yet) |
| u8 | 0 |
| 64 chars | comment |

The editor's notes describe the variant as it is edited. A host does not
send them to its clients.

## Limits

| | |
| --- | --- |
| File | 256 KB |
| Objects created by Forge and mods | 512 |
| The map's own objects tracked | 1024 |
| Tag path | 255 characters |
| Variants a map's Map tab lists | 64 |
| Variants in the map list | 64 |
| A Forge part's settings | 64 KB |
