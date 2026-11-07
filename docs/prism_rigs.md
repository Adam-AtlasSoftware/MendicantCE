# PRISM's extended skeletons

A replacement mesh of PRISM's host mesh store (`display.prism_models`) may be skinned to more bones
than the CE model it replaces has nodes: the CE model's nodes, then **helper bones** (a forearm's
twist, a deltoid, a knee's volume, a pouch, a mandible). The game never sees a helper. Gameplay,
markers, attachments and the network go on reading the CE model's own nodes; no animation data is
added. Each frame the guest sends the CE nodes' skin matrices as it skins with them (node matrix times
the node's default inverse: `rasterizer_set_model_skinning`'s), and the host works out each helper's
transform from them by the rules in the mod's rig, on the render thread
(`port/linux/vulkan/xgpu_rig.c`).

| Limit | CE | A replacement mesh |
| --- | --- | --- |
| Bones | 64 nodes a model (`MAXIMUM_NODES_PER_MODEL`), 44 at once in the vertex shader | 1024 (`XGPU_RIG_BONES_MAXIMUM`): the CE nodes (up to 64) and the helpers |
| Weights | 2 a vertex | 4 a vertex, out of 255 (`tools/prism_rig.py quantize_influences`) |
| Animation | the map's | the map's; helpers follow rules, springs simulate |

## The skeleton

Bones `0` to `ce_node_count - 1` are the CE model's nodes, in its order, with its names, parents and
bind pose. The bones after them are helpers, each after its parent (a CE node or an earlier helper; a
helper with no parent is carried by node 0). The `.pmesh`'s `SKEL` section holds them: 108 bytes each
(`struct xgpu_rig_bone`: the name's hash, the parent, flags, the CE node's number or -1, the bind
matrix and its inverse, 3x4 rows of x, y, z and translation in world units), then their names. A
CE node's frame is the CE model's (its x along the bone); a helper's is whatever its author gave it
(Blender's: y along the bone). The host refuses a skeleton whose CE bones are not the CE nodes in
order, whose parents come after their children, or whose inverse binds are not their binds'
inverses (`XGPU_RIG_SKELETON`).

A name's hash is FNV-1a over its bytes with ASCII letters in lower case (`xgpu_rig_name_hash`,
`prism_rig.name_hash`).

## The rig (`<model tag>.rig.json`)

```json
{ "version": 1,
  "helpers": [
    {"bone": "l forearm twist", "type": "twist", "source": "frame l forearm",
     "child": "frame l wriste", "axis": "x", "helper_axis": "y", "fraction": 0.6},
    {"bone": "l deltoid", "type": "copy_rotation", "source": "bip01 l upperarm",
     "fraction": 0.5, "axes": "xyz", "space": "local"},
    {"bone": "r knee volume", "type": "driven", "driver": {"bone": "bip01 r calf",
     "angle": "y", "range_deg": [0, 140]}, "translate": [0, 0, 0.012], "scale": [1, 1.08, 1.08],
     "curve": "smoothstep"},
    {"bone": "pouch", "type": "spring", "parent": "bip01 pelvis", "stiffness": 300,
     "damping": 14, "gravity": 1.0, "limit_deg": 35, "length": 0.05, "axis": "y"},
    {"bone": "mandible lu", "type": "spring", "stiffness": 60, "damping": 4, "limit_deg": 18,
     "drive": {"function": "A", "rotate_deg": [0, 0, 12]}},
    {"bone": "visor glint", "type": "look_at", "target": "camera", "axis": "x", "up": "z",
     "fraction": 0.25},
    {"bone": "aimer", "type": "aim", "target": "bip01 head", "axis": "y", "limit_deg": 40}
  ] }
```

Every helper starts where its parent carries it (its bind pose relative to its parent); its rule then
turns it about its own origin, or for `driven` moves, turns and scales it in its own frame:

| Type | Fields | Result |
| --- | --- | --- |
| `copy_rotation` | `source`, `fraction` (1), `axes` (`"xyz"`), `space` (`"local"` the source's own bind frame, or `"parent"` its parent's) | The source's rotation away from its bind pose relative to its parent, a fraction of it (by its shortest arc; with fewer than three axes, its XYZ Euler angles on those axes times the fraction), turned about the same axis in the world |
| `twist` | `source`, `child`, `axis` (the child's, `"x"`), `helper_axis` (the helper's own, default `axis`), `fraction` (1) | The child's rotation relative to the source split into swing and twist about `axis`; the helper turns by a fraction of the twist (in -180 to 180 degrees) about `helper_axis` |
| `aim` | `target` (a bone, or `"camera"`), `axis`, `fraction`, `limit_deg` | The helper's `axis` turned towards the target's origin by the shortest arc (Blender's Damped Track), limited, then a fraction of it |
| `look_at` | as `aim`, and `up` | As `aim`, with the `up` axis kept as near as it can be to where the parent carries it (Blender's Track To) |
| `driven` | `driver`: `{"bone", "angle": axis, "range_deg"}` (the bone's local rotation about the axis, by the swing and twist split), `{"function": "A"-"D", "range"}` (the object's function value), or `{"distance": [bone, bone], "range"}`; `translate`, `rotate_deg` (XYZ Euler), `scale`, `curve` (`"linear"`, `"smoothstep"`) | The driver's value placed in its range (0 to 1, held at the ends), through the curve, gives that part of the translation, the rotation and the scale (1 to `scale`) |
| `spring` | `stiffness` (100, 1/s^2), `damping` (10, 1/s), `gravity` (0, a fraction of the game's 9.81 m/s^2), `limit_deg` (0: none), `length` (0.05 world units), `axis` (`"x"`), `drive` (`{"function", "rotate_deg"}`) | The helper's tip (`length` along `axis`) is a damped spring towards where its parent carries it (turned by the drive times the function's value), damped against the target's own velocity: a body moving steadily carries it along, its accelerations swing it. The helper turns to point at the tip, at most `limit_deg` from where its parent carries it |

`"parent"` on a spring only names the skeleton's own parent again (a check). A `"comment"` key is
allowed anywhere; any other key a type does not have is refused, as are unknown bones (status
`UNKNOWN_BONE`), a rule on a CE node, two rules for one bone, and rules that read each other in a
cycle (`FORMAT`). A helper with no rule follows its parent. Rules are worked out in an order where
each comes after its parent and every bone it reads.

The host compiles `.rig.json` itself (`xgpu_rig_compile_json`, jsmn) or reads the `.pmesh`'s `RIG`
section, the same bytes compiled by `tools/prism_rig.py compile` (`PRIG`, version 1, 16 bytes of
header then 84 bytes a rule: `struct xgpu_rig_helper`); `tools/test_prism_rig.py` checks that the two
compilers give the same bytes.

## Springs and the game's clock

Springs move on the game's clock, never the frame's: the instance's time is the game's ticks / 30
plus the interpolation's fraction, and the springs step 1/120 of a second at a time (a tick is four
steps; at most 8 a frame, a longer gap skips time rather than piling up steps), their target between
two frames' samples interpolated, and the helper drawn between its last two steps. So:

- a paused game (the clock still) holds them still, bit for bit;
- the same frames give the same bits (a replay, a theater film);
- frames drawn between ticks (the interpolation, any frame rate) change only where between two steps
  the result is drawn;
- splitscreen and co-op views see the same state (it is kept per instance, not per view).

State is kept per instance (the object's handle and the mesh) and starts again where the parent
carries the helper on a teleport (the guest's flag: the object made, or moved more than 2 world
units), a target that jumps more than `XGPU_RIG_SPRING_JUMP` (2 world units) between frames, an
instance unseen for half a second of game time, or the clock going back (a checkpoint revert, a
film's seek); it is dropped when unseen for half a second. `display.prism_secondary_motion = false`
makes springs and driven helpers follow their parents (copy, twist, aim and look-at still turn).

## Exporting from Blender (`tools/prism_rig_blender.py`)

```sh
blender -b chief.blend -P tools/prism_rig_blender.py -- --armature rig --mesh body,pouch \
    --ce-nodes "cyborg/__base superhigh.jms" --out build/cyborg
```

The armature's bones named as the CE model's nodes are its nodes (their heads within 1 mm of the CE
model's, their parents the CE model's: checked); every other bone is a helper, its bind its Blender
rest pose. A helper's rule is taken from its pose bone: a `prism_rig` custom property (a rule's JSON),
else a Copy Rotation (local spaces, Replace: `copy_rotation`, the influence its fraction), a
Transformation constraint from the target's rotation in a Swing and Twist mode (`twist`), a Damped
Track (`aim`) or a Track To (`look_at`), else custom properties `prism_type = "spring"` and
`prism_stiffness`, `prism_damping`, `prism_gravity`, `prism_limit_deg`, `prism_length` (default: the
bone's length), `prism_axis` (default `"y"`). Axes on CE bones are carried from their Blender frames to
their CE frames. Vertex groups named as bones give each vertex its weights, cut to the largest four
and rounded to 255 (the rounding to the largest). It writes the skeleton (`.skeleton.json` and the
`SKEL` section), the rig (`.rig.json` and the `RIG` section) and the skin (`.pskin`: positions,
normals, joints, weights, triangles) that the `.pmesh` writer packs.

Because the rules are Blender's own constraints, Blender's evaluation is the rig's oracle:
`tests/rig_eval.c` runs `xgpu_rig.c` over recorded frames, and on the Chief's arms over CE's
first-person animations the host's helpers agree with Blender's to 0.0002 degrees and 0.0002 mm.

## Cost

The rules cost about 0.25 microseconds a helper on the render thread (0.31 ms for 40 instances of 30
helpers each on a loaded machine; 2 to 7 microseconds for one Chief); no memory is allocated a frame
for a rig of up to 64 bones.
