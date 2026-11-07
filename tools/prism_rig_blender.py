"""A replacement mesh's extended skeleton out of Blender (docs/prism_rigs.md): the CE model's nodes and
the helper bones, the helpers' rules from their Blender constraints, and every vertex's four joints
and weights. Run headless:

  blender -b <scene.blend> -P tools/prism_rig_blender.py -- --armature <name> --mesh <name>[,<name>...]
      --ce-nodes <model.jms | nodes.json> --out <folder/prefix>

The armature's bones named as the CE model's nodes are the CE nodes (their bind pose must be the CE
model's: every head within 1 mm of the node's, every parent the node's parent); every other bone is a
helper. Writes:
  <prefix>.skeleton.json  the bones in the mesh's order: the CE nodes in the CE model's order, then the
                          helpers parent first; each bone's bind matrix (3x4 rows, world units, the CE
                          model's space) - tools/prism_rig.py's input
  <prefix>.skel           the .pmesh's SKEL section (tools/prism_rig.py pack_skeleton)
  <prefix>.rig.json       the helpers' rules
  <prefix>.rig            the .pmesh's RIG section (compiled as the host compiles .rig.json)
  <prefix>.pskin          the meshes' vertices in the CE model's space, four joints and weights out of
                          255 each, triangles (tools/prism_rig.py write_pskin)

A CE node's frame is the CE model's (x along the bone); a helper's is its Blender bone's (y along the
bone): axes in the rules are each bone's own. A helper's rule, the first that applies:
- a custom property "prism_rig" on the pose bone: a .rig.json helper entry as JSON text (its "bone" is
  filled in);
- a Copy Rotation constraint (the armature's own bone, local spaces, Replace): copy_rotation, the
  influence its fraction, its axes;
- a Transformation constraint from the target's rotation in a Swing and X/Y/Z Twist mode to a rotation:
  twist (child: the target, source: the target's parent), its fraction the ratio of the ranges;
- a Damped Track constraint: aim; a Track To constraint: look_at (the influence the fraction);
- custom properties prism_type "spring" (prism_stiffness, prism_damping, prism_gravity,
  prism_limit_deg, prism_length (default the bone's length), prism_axis (default "y")) or "driven"
  (needs "prism_rig");
- else it follows its parent.
"""

import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import prism_rig  # noqa: E402

AXIS_NAMES = "xyz"


# ---------- the CE model's nodes

def quaternion_to_matrix(q, t):
    """CE's node rotation (i, j, k, w, the conjugate of the standard) and translation as 3x4."""
    i, j, k, w = q
    return prism_rig.matrix_from_quaternion_translation((w, -i, -j, -k), t)


def read_jms_nodes(path):
    """[(name, parent, bind_world 3x4 in world units)] from a JMS (8200: names, first child, next
    sibling, rotation i j k w, translation x100)."""
    lines = [line.strip() for line in open(path, encoding="utf-8", errors="replace")]
    lines = [line for line in lines if line != ""]
    at = 2  # version, checksum
    count = int(lines[at])
    at += 1
    names, first_child, next_sibling, local = [], [], [], []
    for _ in range(count):
        names.append(lines[at])
        first_child.append(int(lines[at + 1]))
        next_sibling.append(int(lines[at + 2]))
        rotation = [float(v) for v in lines[at + 3].split()]
        translation = [float(v) / 100.0 for v in lines[at + 4].split()]
        local.append(quaternion_to_matrix(rotation, translation))
        at += 5
    parents = [-1] * count
    for index in range(count):
        child = first_child[index]
        while child != -1:
            parents[child] = index
            child = next_sibling[child]
    worlds = []
    for index in range(count):  # a JMS lists parents first
        worlds.append(local[index] if parents[index] < 0 else prism_rig.matrix_multiply(worlds[parents[index]],
                                                                                         local[index]))
    return [(names[i], parents[i], worlds[i]) for i in range(count)]


def read_ce_nodes(path):
    if path.lower().endswith(".jms"):
        return read_jms_nodes(path)
    data = json.load(open(path))
    return [(n["name"], n["parent"], n["bind_world"]) for n in data["nodes"]]


# ---------- Blender

def matrix_to_rows(m):
    """A mathutils 4x4 as 3x4 rows."""
    return [m[r][c] for r in range(3) for c in range(4)]


def rows_to_matrix(rows):
    from mathutils import Matrix
    return Matrix((rows[0:4], rows[4:8], rows[8:12], (0.0, 0.0, 0.0, 1.0)))


class Skeleton:
    """The mesh's bones out of an armature: CE nodes first, then the helpers."""

    def __init__(self, armature, ce_nodes, tolerance=1e-3):
        self.armature = armature
        bones = {b.name.lower(): b for b in armature.data.bones}
        self.order = []  # Blender bone names in the mesh's order
        self.bones = []  # prism_rig bones
        self.correction = {}  # per CE bone: C, its Blender frame in its CE frame (W_blender = H C)
        self.ce_count = len(ce_nodes)
        errors = []
        for index, (name, parent, bind) in enumerate(ce_nodes):
            bone = bones.get(name.lower())
            if bone is None:
                errors.append("CE node %r has no bone" % name)
                continue
            world = armature.matrix_world @ bone.matrix_local
            H = rows_to_matrix(bind)
            if (world.translation - H.translation).length > tolerance:
                errors.append("bone %r: its head %.4f world units from the CE node's" %
                              (bone.name, (world.translation - H.translation).length))
            expected_parent = ce_nodes[parent][0].lower() if parent >= 0 else None
            if (bone.parent.name.lower() if bone.parent else None) != expected_parent:
                errors.append("bone %r: parent %r, the CE node's %r" %
                              (bone.name, bone.parent.name if bone.parent else None, expected_parent))
            self.correction[bone.name] = H.inverted() @ world
            self.order.append(bone.name)
            self.bones.append({"name": name, "parent": parent, "bind_world": list(bind)})
        if errors:
            raise prism_rig.RigError("SKELETON", "; ".join(errors))
        ce_names = set(self.order)
        for bone in armature.data.bones:  # parents first
            if bone.name in ce_names:
                continue
            parent = self.order.index(bone.parent.name) if bone.parent else -1
            world = armature.matrix_world @ bone.matrix_local
            self.order.append(bone.name)
            self.bones.append({"name": bone.name, "parent": parent, "bind_world": matrix_to_rows(world)})
        self.index = {name: i for i, name in enumerate(self.order)}

    def is_ce(self, name):
        return self.index[name] < self.ce_count

    def axis_in_frame(self, bone_name, blender_axis):
        """A Blender bone axis (0 x, 1 y, 2 z) as the axis of the bone's frame in the mesh: a CE node's
        through its correction (it must land on an axis, positive), a helper's as it is."""
        if not self.is_ce(bone_name):
            return blender_axis
        c = self.correction[bone_name].to_3x3()
        column = [c[r][blender_axis] for r in range(3)]
        best = max(range(3), key=lambda r: abs(column[r]))
        if abs(column[best]) < 0.999 or column[best] < 0:
            raise prism_rig.RigError("FORMAT", "bone %r: its Blender axis %s is not one of its CE frame's" %
                                     (bone_name, AXIS_NAMES[blender_axis]))
        return best

    def ce_world(self, name):
        """A CE bone's posed world matrix in its CE frame (the armature's pose evaluated)."""
        bone = self.armature.pose.bones[name]
        return self.armature.matrix_world @ bone.matrix @ self.correction[name].inverted()

    def helper_world(self, name):
        bone = self.armature.pose.bones[name]
        return self.armature.matrix_world @ bone.matrix

    def ce_skin_rows(self):
        """Every CE node's skin matrix of the current pose (world times inverse bind): what the guest
        sends."""
        out = []
        for index in range(self.ce_count):
            name = self.order[index]
            bind = rows_to_matrix(self.bones[index]["bind_world"])
            out.append(matrix_to_rows(self.ce_world(name) @ bind.inverted()))
        return out


def _swing_twist_axis(mode):
    return {"SWING_TWIST_X": 0, "SWING_TWIST_Y": 1, "SWING_TWIST_Z": 2}.get(mode)


def helper_rule(skeleton, pose_bone):
    """The .rig.json entry of a helper's pose bone, or None (it follows)."""
    name = pose_bone.name
    if "prism_rig" in pose_bone.keys():
        entry = json.loads(pose_bone["prism_rig"])
        entry["bone"] = name
        return entry
    armature = skeleton.armature
    for constraint in pose_bone.constraints:
        if constraint.mute or getattr(constraint, "target", None) is not armature:
            continue
        kind = constraint.type
        if kind == "COPY_ROTATION":
            if constraint.invert_x or constraint.invert_y or constraint.invert_z or constraint.mix_mode != "REPLACE":
                raise prism_rig.RigError("FORMAT", "%s: Copy Rotation inverted or not Replace" % name)
            source = constraint.subtarget
            axes = "".join(AXIS_NAMES[skeleton.axis_in_frame(source, a)]
                           for a, used in enumerate((constraint.use_x, constraint.use_y, constraint.use_z)) if used)
            axes = "".join(sorted(axes))
            return {"bone": name, "type": "copy_rotation", "source": source, "fraction": constraint.influence,
                    "axes": axes, "space": "local"}
        if kind == "TRANSFORM" and constraint.map_from == "ROTATION" and constraint.map_to == "ROTATION":
            axis = _swing_twist_axis(constraint.from_rotation_mode)
            if axis is None:
                continue
            child = constraint.subtarget
            parent = armature.data.bones[child].parent
            if parent is None:
                raise prism_rig.RigError("FORMAT", "%s: the twist's child has no parent" % name)
            letter = AXIS_NAMES[axis]
            from_max = getattr(constraint, "from_max_%s_rot" % letter)
            destination = None
            for to in AXIS_NAMES:
                if getattr(constraint, "map_to_%s_from" % to) == letter.upper():
                    to_max = getattr(constraint, "to_max_%s_rot" % to)
                    if abs(to_max) > 1e-9:
                        destination = (AXIS_NAMES.index(to), to_max)
            if destination is None or abs(from_max) < 1e-9:
                raise prism_rig.RigError("FORMAT", "%s: the twist's ranges" % name)
            return {"bone": name, "type": "twist", "source": parent.name, "child": child,
                    "axis": AXIS_NAMES[skeleton.axis_in_frame(child, axis)],
                    "helper_axis": AXIS_NAMES[destination[0]],
                    "fraction": constraint.influence * destination[1] / from_max}
        if kind in ("DAMPED_TRACK", "TRACK_TO"):
            track = constraint.track_axis
            if track.startswith("TRACK_NEGATIVE"):
                raise prism_rig.RigError("FORMAT", "%s: a negative track axis" % name)
            entry = {"bone": name, "type": "aim" if kind == "DAMPED_TRACK" else "look_at",
                     "target": constraint.subtarget, "axis": track[-1].lower(), "fraction": constraint.influence}
            if kind == "TRACK_TO":
                entry["up"] = constraint.up_axis[-1].lower()
            return entry
    kind = pose_bone.get("prism_type")
    if kind == "spring":
        entry = {"bone": name, "type": "spring"}
        for key in ("stiffness", "damping", "gravity", "limit_deg", "length"):
            if "prism_" + key in pose_bone.keys():
                entry[key] = float(pose_bone["prism_" + key])
        entry.setdefault("length", pose_bone.bone.length * armature.matrix_world.to_scale()[0])
        entry["axis"] = str(pose_bone.get("prism_axis", "y"))
        return entry
    if kind is not None:
        raise prism_rig.RigError("FORMAT", "%s: prism_type %r needs a prism_rig entry" % (name, kind))
    return None


def rig_json(skeleton):
    helpers = []
    for name in skeleton.order[skeleton.ce_count:]:
        entry = helper_rule(skeleton, skeleton.armature.pose.bones[name])
        if entry is not None:
            helpers.append(entry)
    return {"version": 1, "helpers": helpers}


def mesh_skin(skeleton, objects):
    """The meshes at rest in the CE model's space: positions, normals, four joints and weights,
    triangles. Vertex groups named as bones are their weights (others are ignored)."""
    positions, normals, joints, weights, triangles = [], [], [], [], []
    unweighted = 0
    for obj in objects:
        mesh = obj.data
        mesh.calc_loop_triangles()
        world = obj.matrix_world
        normal_matrix = world.to_3x3().inverted().transposed()
        group_bone = {g.index: skeleton.index.get(g.name) for g in obj.vertex_groups}
        first = len(positions)
        for vertex in mesh.vertices:
            p = world @ vertex.co
            n = (normal_matrix @ vertex.normal).normalized()
            positions.append((p.x, p.y, p.z))
            normals.append((n.x, n.y, n.z))
            influences = [(group_bone[g.group], g.weight) for g in vertex.groups
                          if group_bone.get(g.group) is not None and g.weight > 0]
            if not influences:
                unweighted += 1
            j, w = prism_rig.quantize_influences(influences)
            joints.append(j)
            weights.append(w)
        for triangle in mesh.loop_triangles:
            triangles.append(tuple(first + v for v in triangle.vertices))
    return {"positions": positions, "normals": normals, "joints": joints, "weights": weights,
            "triangles": triangles, "unweighted": unweighted}


def export(armature, objects, ce_nodes_path, prefix):
    """Writes the five files (see the module's text); returns (skeleton, rig dict, skin)."""
    skeleton = Skeleton(armature, read_ce_nodes(ce_nodes_path))
    rig = rig_json(skeleton)
    compiled = prism_rig.compile_rig(rig, skeleton.bones, skeleton.ce_count)
    skin = mesh_skin(skeleton, objects)
    folder = os.path.dirname(prefix)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(prefix + ".skeleton.json", "w") as f:
        json.dump({"ce_node_count": skeleton.ce_count, "bones": skeleton.bones}, f, indent=1)
    with open(prefix + ".skel", "wb") as f:
        f.write(prism_rig.pack_skeleton(skeleton.bones, skeleton.ce_count))
    with open(prefix + ".rig.json", "w") as f:
        json.dump(rig, f, indent=1)
    with open(prefix + ".rig", "wb") as f:
        f.write(compiled)
    prism_rig.write_pskin(prefix + ".pskin", skin["positions"], skin["normals"], skin["joints"], skin["weights"],
                          skin["triangles"])
    return skeleton, rig, skin


def main():
    import argparse
    import bpy
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="prism_rig_blender.py")
    parser.add_argument("--armature", required=True)
    parser.add_argument("--mesh", required=True, help="mesh objects, comma separated")
    parser.add_argument("--ce-nodes", required=True, help="the CE model's nodes: a .jms or nodes.json")
    parser.add_argument("--out", required=True, help="the output files' prefix")
    a = parser.parse_args(argv)
    armature = bpy.data.objects[a.armature]
    objects = [bpy.data.objects[name] for name in a.mesh.split(",")]
    try:
        skeleton, rig, skin = export(armature, objects, a.ce_nodes, a.out)
    except prism_rig.RigError as error:
        print("prism_rig_blender: %s: %s" % (error.status, error), file=sys.stderr)
        sys.exit(1)
    print("prism_rig_blender: %d bones (%d CE nodes, %d helpers), %d rules, %d vertices (%d unweighted), "
          "%d triangles" % (len(skeleton.bones), skeleton.ce_count, len(skeleton.bones) - skeleton.ce_count,
                            len(rig["helpers"]), len(skin["positions"]), skin["unweighted"], len(skin["triangles"])))


if __name__ == "__main__":
    main()
