"""PRISM's extended skeletons, the art side (docs/prism_rigs.md; the host's half is
port/linux/vulkan/xgpu_rig.c): a replacement mesh's skeleton (the CE model's nodes, then helper
bones) packed as a .pmesh's SKEL section, its rules (.rig.json) compiled to the RIG section's
bytes exactly as the host compiles them, and a vertex's influences cut to the mesh's four joints
and weights out of 255.

Pure Python, no numpy, so that Blender's Python (tools/prism_rig_blender.py) and any other can
import it.

  python tools/prism_rig.py compile <rig.json> <skeleton.json> [-o rig.bin]
  python tools/prism_rig.py skel <skeleton.json> -o skel.bin

A skeleton.json is {"ce_node_count": N, "bones": [{"name", "parent", "bind_world": [12 floats, 3x4
rows of (x, y, z, translation), world units]}...]}, the CE nodes first in the CE model's order
(what prism_rig_blender.py writes).
"""

import argparse
import json
import math
import struct
import sys

MAGIC = 0x47495250  # 'PRIG'
VERSION = 1
BONES_MAXIMUM = 1024
HELPERS_MAXIMUM = 1024
CE_NODES_MAXIMUM = 64
SPRING_STEP = 1.0 / 120.0

BONE_CE_NODE, BONE_HELPER = 1, 2
TYPES = ["follow", "copy_rotation", "twist", "aim", "look_at", "driven", "spring"]
FOLLOW, COPY_ROTATION, TWIST, AIM, LOOK_AT, DRIVEN, SPRING = range(7)
TARGET_CAMERA = -2
AXES = {"x": 0, "y": 1, "z": 2}
HELPER_FORMAT = "<HBBhhBBBBBBHf16f"  # 84 bytes, struct xgpu_rig_helper
BONE_FORMAT = "<IhHhH12f12f"  # 108 bytes, struct xgpu_rig_bone
assert struct.calcsize(HELPER_FORMAT) == 84 and struct.calcsize(BONE_FORMAT) == 108


class RigError(ValueError):
    """Why a rig or skeleton is refused; status is the host's (FORMAT, SKELETON, UNKNOWN_BONE)."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def name_hash(name):
    """FNV-1a over the name's bytes, ASCII letters lower case (xgpu_rig_name_hash)."""
    value = 2166136261
    for c in name.encode("utf-8"):
        if 65 <= c <= 90:
            c += 32
        value = ((value ^ c) * 16777619) & 0xFFFFFFFF
    return value


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def radians(degrees):
    return degrees * (math.pi / 180.0)


# ---------- 3x4 matrices: rows of (x, y, z, translation)

def matrix_multiply(a, b):
    out = [0.0] * 12
    for row in range(3):
        x = a[row * 4:row * 4 + 4]
        for column in range(4):
            out[row * 4 + column] = x[0] * b[column] + x[1] * b[4 + column] + x[2] * b[8 + column]
        out[row * 4 + 3] += x[3]
    return out


def matrix_invert(m):
    a, b, c, d, e, f, g, h, i = m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]
    A, B, C = e * i - f * h, -(d * i - f * g), d * h - e * g
    det = a * A + b * B + c * C
    if abs(det) < 1e-20:
        raise RigError("SKELETON", "a bind matrix cannot be inverted")
    r = [A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
         B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
         C / det, -(a * h - b * g) / det, (a * e - b * d) / det]
    out = []
    for row in range(3):
        rr = r[row * 3:row * 3 + 3]
        out += rr + [-(rr[0] * m[3] + rr[1] * m[7] + rr[2] * m[11])]
    return out


def matrix_from_quaternion_translation(q, t, scale=1.0):
    """Standard quaternion (w, x, y, z) and a translation as a 3x4 matrix."""
    w, x, y, z = q
    return [scale * (1 - 2 * (y * y + z * z)), scale * 2 * (x * y - z * w), scale * 2 * (x * z + y * w), t[0],
            scale * 2 * (x * y + z * w), scale * (1 - 2 * (x * x + z * z)), scale * 2 * (y * z - x * w), t[1],
            scale * 2 * (x * z - y * w), scale * 2 * (y * z + x * w), scale * (1 - 2 * (x * x + y * y)), t[2]]


# ---------- the skeleton

def check_skeleton(bones, ce_node_count):
    """bones: dicts {name, parent, bind_world[12]} (and optionally inverse_bind); the host's rules."""
    if not bones or len(bones) > BONES_MAXIMUM:
        raise RigError("SKELETON", "%d bones (1 to %d)" % (len(bones), BONES_MAXIMUM))
    if not 0 < ce_node_count <= min(CE_NODES_MAXIMUM, len(bones)):
        raise RigError("SKELETON", "%d CE nodes of %d bones" % (ce_node_count, len(bones)))
    for index, bone in enumerate(bones):
        if not -1 <= bone["parent"] < index:
            raise RigError("SKELETON", "bone %d (%s): parent %d not an earlier bone" %
                           (index, bone["name"], bone["parent"]))
        if len(bone["bind_world"]) != 12 or not all(math.isfinite(v) for v in bone["bind_world"]):
            raise RigError("SKELETON", "bone %d (%s): its bind matrix" % (index, bone["name"]))
    hashes = {}
    for index, bone in enumerate(bones):
        hashes.setdefault(name_hash(bone["name"]), []).append(index)
    return hashes


def pack_skeleton(bones, ce_node_count):
    """The SKEL section: the entries (struct xgpu_rig_bone) then the names, NUL-terminated, the whole
    padded to 16 bytes."""
    check_skeleton(bones, ce_node_count)
    out = bytearray()
    for index, bone in enumerate(bones):
        bind = [f32(v) for v in bone["bind_world"]]
        inverse = bone.get("inverse_bind") or matrix_invert(bind)
        ce = index < ce_node_count
        out += struct.pack(BONE_FORMAT, name_hash(bone["name"]), bone["parent"],
                           BONE_CE_NODE if ce else BONE_HELPER, index if ce else -1, 0, *bind, *inverse)
    out += b"".join(bone["name"].encode("utf-8") + b"\0" for bone in bones)
    out += b"\0" * (-len(out) % 16)
    return bytes(out)


def skeleton_entries(bones, ce_node_count):
    """The entries alone (what xgpu_rig_create takes)."""
    return pack_skeleton(bones, ce_node_count)[:108 * len(bones)]


# ---------- .rig.json -> the RIG section

def _bone(bones_by_hash, name, what):
    if not isinstance(name, str):
        raise RigError("FORMAT", "%s: not a bone's name" % what)
    found = bones_by_hash.get(name_hash(name), [])
    if len(found) > 1:
        raise RigError("FORMAT", "%s: two bones named %r" % (what, name))
    if not found:
        raise RigError("UNKNOWN_BONE", "%s: no bone %r" % (what, name))
    return found[0]


def _number(value, what):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise RigError("FORMAT", "%s: not a number" % what)
    return float(value)


def _numbers(value, count, what):
    if not isinstance(value, list) or len(value) != count:
        raise RigError("FORMAT", "%s: not %d numbers" % (what, count))
    return [_number(v, what) for v in value]


def _axis(value, what):
    if value not in AXES:
        raise RigError("FORMAT", "%s: not x, y or z" % what)
    return AXES[value]


def _function(value, what):
    if not isinstance(value, str) or len(value) != 1 or value.upper() not in "ABCD":
        raise RigError("FORMAT", "%s: not a function A-D" % what)
    return "ABCD".index(value.upper())


def compile_helper(entry, bones, bones_by_hash, ce_node_count):
    if not isinstance(entry, dict):
        raise RigError("FORMAT", "a helper not an object")
    if entry.get("type") not in TYPES:
        raise RigError("FORMAT", "a helper of no known type")
    kind = TYPES.index(entry["type"])
    bone = _bone(bones_by_hash, entry.get("bone"), "a helper's bone")
    if bone < ce_node_count:
        raise RigError("FORMAT", "bone %d is a CE node" % bone)
    h = dict(bone=bone, type=kind, flags=0, source=-1, target=-1, axis=0, axes=7, space=0, curve=0, driver=0,
             function=0xFF, fraction=1.0, values=[0.0] * 16)
    h["values"][8] = h["values"][9] = h["values"][10] = 1.0
    if kind == SPRING:
        h["values"][0], h["values"][1], h["values"][4] = 100.0, 10.0, 0.05
    what = "helper of bone %d" % bone
    for key, value in entry.items():
        if key in ("type", "bone", "comment"):
            continue
        if key == "source" and kind in (COPY_ROTATION, TWIST):
            h["source"] = _bone(bones_by_hash, value, what)
        elif key == "child" and kind == TWIST:
            h["target"] = _bone(bones_by_hash, value, what)
        elif key == "target" and kind in (AIM, LOOK_AT):
            h["target"] = TARGET_CAMERA if value == "camera" else _bone(bones_by_hash, value, what)
        elif key == "parent" and kind == SPRING:
            if _bone(bones_by_hash, value, what) != bones[bone]["parent"]:
                raise RigError("FORMAT", "%s: \"parent\" is not its parent" % what)
        elif key == "axis":
            h["axis"] = _axis(value, what)
        elif key == "up" and kind == LOOK_AT:
            h["axes"] = _axis(value, what)
        elif key == "helper_axis" and kind == TWIST:
            h["axes"] = _axis(value, what)
        elif key == "axes" and kind == COPY_ROTATION:
            if not isinstance(value, str) or not value or len(set(value)) != len(value) or set(value) - set("xyz"):
                raise RigError("FORMAT", "%s: \"axes\"" % what)
            h["axes"] = sum(1 << AXES[c] for c in value)
        elif key == "space" and kind == COPY_ROTATION:
            if value not in ("local", "parent"):
                raise RigError("FORMAT", "%s: \"space\"" % what)
            h["space"] = 0 if value == "local" else 1
        elif key == "fraction" and kind in (COPY_ROTATION, TWIST, AIM, LOOK_AT):
            h["fraction"] = _number(value, what)
        elif key == "limit_deg" and kind in (SPRING, AIM, LOOK_AT):
            limit = _number(value, what)
            if limit < 0:
                raise RigError("FORMAT", "%s: \"limit_deg\"" % what)
            h["values"][3 if kind == SPRING else 0] = radians(limit)
        elif kind == SPRING and key in ("stiffness", "damping", "gravity", "length"):
            h["values"][{"stiffness": 0, "damping": 1, "gravity": 2, "length": 4}[key]] = _number(value, what)
        elif kind == SPRING and key == "drive":
            if not isinstance(value, dict) or set(value) - {"function", "rotate_deg"} or "function" not in value:
                raise RigError("FORMAT", "%s: \"drive\"" % what)
            h["function"] = _function(value["function"], what)
            if "rotate_deg" in value:
                h["values"][5:8] = [radians(v) for v in _numbers(value["rotate_deg"], 3, what)]
        elif kind == DRIVEN and key == "driver":
            if not isinstance(value, dict):
                raise RigError("FORMAT", "%s: \"driver\"" % what)
            angle = False
            for inner, v in value.items():
                if inner == "bone":
                    h["source"], h["driver"] = _bone(bones_by_hash, v, what), 0
                elif inner == "angle":
                    h["axis"] = _axis(v, what)
                elif inner == "function":
                    h["function"], h["driver"] = _function(v, what), 1
                elif inner == "distance":
                    if not isinstance(v, list) or len(v) != 2:
                        raise RigError("FORMAT", "%s: \"distance\"" % what)
                    h["source"] = _bone(bones_by_hash, v[0], what)
                    h["target"] = _bone(bones_by_hash, v[1], what)
                    h["driver"] = 2
                elif inner == "range_deg":
                    h["values"][0:2] = [radians(r) for r in _numbers(v, 2, what)]
                    angle = True
                elif inner == "range":
                    h["values"][0:2] = _numbers(v, 2, what)
                else:
                    raise RigError("FORMAT", "%s: \"driver\" key %r" % (what, inner))
            if angle != (h["driver"] == 0):
                raise RigError("FORMAT", "%s: an angle's range in degrees (range_deg), others' in range" % what)
        elif kind == DRIVEN and key == "translate":
            h["values"][2:5] = _numbers(value, 3, what)
        elif kind == DRIVEN and key == "rotate_deg":
            h["values"][5:8] = [radians(v) for v in _numbers(value, 3, what)]
        elif kind == DRIVEN and key == "scale":
            h["values"][8:11] = _numbers(value, 3, what)
        elif kind == DRIVEN and key == "curve":
            if value not in ("linear", "smoothstep"):
                raise RigError("FORMAT", "%s: \"curve\"" % what)
            h["curve"] = 0 if value == "linear" else 1
        else:
            raise RigError("FORMAT", "%s: \"%s\" not known for its type" % (what, key))
    if kind == TWIST and "helper_axis" not in entry:
        h["axes"] = h["axis"]
    return h


def check_helper(h, bone_count, ce_node_count):
    """The host's check_helper (xgpu_rig.c)."""
    def valid(b):
        return 0 <= b < bone_count
    bone, kind, v = h["bone"], h["type"], h["values"]
    bad = None
    if kind == COPY_ROTATION:
        bad = not valid(h["source"]) or h["source"] == bone or not 0 < h["axes"] <= 7
    elif kind == TWIST:
        bad = (not valid(h["source"]) or not valid(h["target"]) or h["source"] == h["target"] or
               bone in (h["source"], h["target"]) or h["axes"] > 2)
    elif kind in (AIM, LOOK_AT):
        bad = ((h["target"] != TARGET_CAMERA and not valid(h["target"])) or h["target"] == bone or
               (kind == LOOK_AT and (h["axes"] > 2 or h["axes"] == h["axis"])))
    elif kind == DRIVEN:
        bad = ((h["driver"] == 1 and h["function"] > 3) or
               (h["driver"] != 1 and (not valid(h["source"]) or h["source"] == bone)) or
               (h["driver"] == 2 and (not valid(h["target"]) or h["target"] == bone)) or f32(v[0]) == f32(v[1]))
    elif kind == SPRING:
        bad = (v[0] <= 0 or v[1] < 0 or v[3] < 0 or v[4] <= 0 or
               f32(v[0]) * SPRING_STEP * SPRING_STEP > 2.0 or f32(v[1]) * SPRING_STEP > 1.0)
    if bad:
        raise RigError("FORMAT", "bone %d: the fields of its %s" % (bone, TYPES[kind]))


def helper_reads(h):
    kind = h["type"]
    if kind == COPY_ROTATION:
        return [h["source"]]
    if kind == TWIST:
        return [h["source"], h["target"]]
    if kind in (AIM, LOOK_AT):
        return [h["target"]] if h["target"] >= 0 else []
    if kind == DRIVEN:
        return ([h["source"]] if h["driver"] != 1 else []) + ([h["target"]] if h["driver"] == 2 else [])
    return []


def check_order(helpers, bones, ce_node_count):
    """Every helper's rule must be workable after its parent and what it reads (no cycle)."""
    rule = {h["bone"]: h for h in helpers}
    if len(rule) != len(helpers):
        raise RigError("FORMAT", "a bone has two rules")
    done = set(range(ce_node_count))
    left = set(range(ce_node_count, len(bones)))
    while left:
        ready = [b for b in sorted(left) if (bones[b]["parent"] < 0 or bones[b]["parent"] in done) and
                 all(r in done for r in (helper_reads(rule[b]) if b in rule else []))]
        if not ready:
            raise RigError("FORMAT", "the rules read each other in a cycle")
        done.update(ready)
        left.difference_update(ready)


def compile_rig(rig, bones, ce_node_count):
    """.rig.json (a dict) against a skeleton -> the RIG section's bytes (xgpu_rig_compile_json's)."""
    bones_by_hash = check_skeleton(bones, ce_node_count)
    if not isinstance(rig, dict):
        raise RigError("FORMAT", "not an object")
    for key in rig:
        if key not in ("version", "helpers", "comment", "skeleton"):
            raise RigError("FORMAT", "%r not known" % key)
    if rig.get("version") != 1 or not isinstance(rig.get("helpers"), list):
        raise RigError("FORMAT", "not version 1, or no helpers")
    if len(rig["helpers"]) > HELPERS_MAXIMUM:
        raise RigError("FORMAT", "%d helpers" % len(rig["helpers"]))
    helpers = [compile_helper(e, bones, bones_by_hash, ce_node_count) for e in rig["helpers"]]
    for h in helpers:
        check_helper(h, len(bones), ce_node_count)
    check_order(helpers, bones, ce_node_count)
    out = bytearray(struct.pack("<IHHII", MAGIC, VERSION, len(helpers), len(bones), ce_node_count))
    for h in helpers:
        out += struct.pack(HELPER_FORMAT, h["bone"], h["type"], h["flags"], h["source"], h["target"], h["axis"],
                           h["axes"], h["space"], h["curve"], h["driver"], h["function"], 0, h["fraction"],
                           *h["values"])
    return bytes(out)


# ---------- weights

def quantize_influences(influences, limit=4):
    """[(joint, weight), ...] -> (joints[4], weights[4]): the largest four, normalised, out of 255,
    the rounding given to the largest; zero weights' joints are 0."""
    merged = {}
    for joint, weight in influences:
        if weight > 0:
            merged[joint] = merged.get(joint, 0.0) + weight
    top = sorted(merged.items(), key=lambda item: (-item[1], item[0]))[:limit]
    total = sum(w for _, w in top)
    if total <= 0:
        return [0, 0, 0, 0], [255, 0, 0, 0]
    scaled = [(j, w * 255.0 / total) for j, w in top]
    weights = [int(math.floor(w + 0.5)) for _, w in scaled]
    weights[0] += 255 - sum(weights)
    joints = [j for j, _ in scaled]
    pairs = [(j, w) for j, w in zip(joints, weights) if w > 0]
    while len(pairs) < 4:
        pairs.append((0, 0))
    return [j for j, _ in pairs], [w for _, w in pairs]


# ---------- the skin interchange (.pskin), what prism_rig_blender.py writes beside the rig

PSKIN_MAGIC = 0x4E4B5350  # 'PSKN'


def write_pskin(path, positions, normals, joints, weights, triangles):
    """Positions and normals (model space, world units), four joints (u16) and weights (u8) a
    vertex, triangles (u32 x 3)."""
    with open(path, "wb") as f:
        f.write(struct.pack("<IIII", PSKIN_MAGIC, 1, len(positions), len(triangles)))
        for p in positions:
            f.write(struct.pack("<3f", *p))
        for n in normals:
            f.write(struct.pack("<3f", *n))
        for j in joints:
            f.write(struct.pack("<4H", *j))
        for w in weights:
            f.write(struct.pack("<4B", *w))
        for t in triangles:
            f.write(struct.pack("<3I", *t))


def read_pskin(path):
    data = open(path, "rb").read()
    magic, version, vertices, triangles = struct.unpack_from("<IIII", data, 0)
    if magic != PSKIN_MAGIC or version != 1:
        raise ValueError("%s: not a .pskin" % path)
    at = 16
    out = {}
    for name, fmt, count in (("positions", "<3f", vertices), ("normals", "<3f", vertices),
                             ("joints", "<4H", vertices), ("weights", "<4B", vertices),
                             ("triangles", "<3I", triangles)):
        size = struct.calcsize(fmt)
        out[name] = [struct.unpack_from(fmt, data, at + i * size) for i in range(count)]
        at += size * count
    return out


def load_skeleton(path):
    data = json.load(open(path))
    return data["bones"], data["ce_node_count"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("compile", help="a .rig.json to the RIG section's bytes")
    c.add_argument("rig")
    c.add_argument("skeleton")
    c.add_argument("-o", "--output")
    s = sub.add_parser("skel", help="a skeleton.json to the SKEL section's bytes")
    s.add_argument("skeleton")
    s.add_argument("-o", "--output", required=True)
    a = parser.parse_args(argv)
    bones, ce_node_count = load_skeleton(a.skeleton)
    try:
        if a.command == "compile":
            data = compile_rig(json.load(open(a.rig)), bones, ce_node_count)
        else:
            data = pack_skeleton(bones, ce_node_count)
    except RigError as error:
        print("%s: %s" % (error.status, error), file=sys.stderr)
        return 1
    if getattr(a, "output", None):
        open(a.output, "wb").write(data)
    print("%d bytes, %d bones (%d CE nodes)" % (len(data), len(bones), ce_node_count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
