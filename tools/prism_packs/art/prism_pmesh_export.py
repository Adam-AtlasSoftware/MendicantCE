"""PRISM mesh store: a Blender scene as a .pmesh (display.prism_models), headless.

    blender -b --factory-startup --python art/prism_pmesh_export.py -- export <scene.blend> <model dump.json> <out.pmesh>
        [--scale S] [--uv UVMAP] [--max-weights 4] [--rig helpers.rig.json]
    blender -b --factory-startup --python art/prism_pmesh_export.py -- import-dump <model dump.json> <out.blend>

The scene, in Halo model space (Z up, world units; --scale for scenes in another unit, e.g. 0.01 for
JMS's centimetre-like units):
- one armature whose bones named as the model's nodes are those nodes (every node must be there);
  any other bone is a helper bone (its parent before it; rigs drive helpers, until then a helper follows
  its nearest node);
- mesh objects parented to the armature (an Armature modifier or the parent), weighted by vertex groups
  named as the bones, in collections named lod0, lod1, ... (lod0 the most detailed; a collection's
  custom property cutoff_pixels is the size it is drawn from, else the model's own cutoffs);
- each object's custom properties region and permutation (indices or the model's names; default 0, 0)
  and each material's ce_shader (the model's shader index, or its tag name's last part; else the
  material's name is looked up the same way) choose the stock part the piece replaces;
- uv0 the active UV map (or --uv), kept to the model's base map scale (the stock layout); uv1 the
  second map if there is one;
- faces wound as Blender's are (counter-clockwise about their outward normals): written reversed, as CE winds
  its front faces (clockwise), so the stock parts' culling keeps their outsides.

The model dump (debug.prism_mesh_dump's json, local only: the map's data) gives the nodes' names,
checksum and bind pose, the regions, permutations, shaders and markers. Checked before writing, as the
host checks: every node present (its bind position to 1e-3 world units, scaled), every piece a part of
the model, uv0 inside the base map scale; and warned: a marker the gameplay reads (a weapon's muzzle, an
attachment) more than 2 cm (0.0066 world units) inside the new surface, whose effects would start inside
it. Triangulated (non-destructively), split normals kept, MikkTSpace tangents on uv0, at most four
weights a vertex (the largest, normalised), identical vertices merged per piece.

PRISM_PACKS_TOOLS (packs_env.py) names the folder of the game's format tools, whose pmesh.py writes and
checks the file (the repository's tools/, or the pack tools' formats/ in a download).

import-dump makes a test scene of the model as the game draws it (its most detailed geometry, its
nodes as an armature, weights, uvs and normals): exporting it gives the stock geometry back as a
.pmesh (the exporter's check against tools/pmesh_from_dump.py's).
"""

import argparse
import json
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import packs_env as E  # noqa: E402

OPENH1_TOOLS = E.TOOLS
sys.path.insert(0, OPENH1_TOOLS)
import pmesh  # noqa: E402

DETAIL_NAMES = ("superlow", "low", "medium", "high", "superhigh")
MARKER_INSIDE = 0.02 / 3.048  # 2 cm in world units (10 feet)


def script_args():
    argv = sys.argv
    return argv[argv.index("--") + 1:] if "--" in argv else []


def node_binds(dump):
    """each node's bind pose in model space (4x4), from the dump's inverse"""
    out = []
    for node in dump["nodes"]:
        s, fi, fj, fk, li, lj, lk, ui, uj, uk, px, py, pz = node["inverse"]
        m = np.eye(4)
        m[:3, 0] = np.array([fi, fj, fk]) * s
        m[:3, 1] = np.array([li, lj, lk]) * s
        m[:3, 2] = np.array([ui, uj, uk]) * s
        m[:3, 3] = [px, py, pz]
        out.append(np.linalg.inv(m))
    return out


def shader_index(dump, value):
    if isinstance(value, (int, float)) or (isinstance(value, str) and value.isdigit()):
        return int(value)
    for index, shader in enumerate(dump["shaders"]):
        name = shader["name"]
        if value == name or value == name.split("\\")[-1]:
            return index
    raise SystemExit(f"material {value!r}: no shader of the model is named so "
                     f"({', '.join(s['name'].split(chr(92))[-1] for s in dump['shaders'])})")


def region_index(dump, value):
    if isinstance(value, (int, float)) or (isinstance(value, str) and value.isdigit()):
        return int(value)
    for index, region in enumerate(dump["regions"]):
        if region["name"] == value:
            return index
    raise SystemExit(f"region {value!r} is not the model's")


def permutation_index(dump, region, value):
    if isinstance(value, (int, float)) or (isinstance(value, str) and value.isdigit()):
        return int(value)
    for index, permutation in enumerate(dump["regions"][region]["permutations"]):
        if permutation["name"] == value:
            return index
    raise SystemExit(f"permutation {value!r} is not region {region}'s")


# ---------- export

def lod_collections():
    found = []
    for collection in bpy.data.collections:
        name = collection.name.lower()
        if name.startswith("lod") and name[3:].isdigit():
            found.append((int(name[3:]), collection))
    return [c for _, c in sorted(found)]


def object_triangles(obj, depsgraph, scale, uv_name, bone_index, warnings):
    """an object's evaluated mesh as corner arrays: positions, normals, tangents and signs, uv0, uv1,
    four joints and weights per corner, its material index per triangle"""
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(mesh)
    bm.free()
    uv_layer = mesh.uv_layers.get(uv_name) if uv_name else mesh.uv_layers.active
    if uv_layer is None:
        raise SystemExit(f"{obj.name}: no uv map")
    uv_name = uv_layer.name
    other_names = [layer.name for layer in mesh.uv_layers if layer.name != uv_name]
    mesh.calc_tangents(uvmap=uv_name)
    # (the layers looked up again after the tangents: their data may have moved)
    uv_layer = mesh.uv_layers[uv_name]
    second = mesh.uv_layers[other_names[0]] if other_names else None
    to_model = Matrix.Scale(scale, 4) @ obj.matrix_world
    normal_matrix = to_model.to_3x3().inverted_safe().transposed()
    corners = len(mesh.loops)
    positions = np.zeros((corners, 3))
    normals = np.zeros((corners, 3))
    tangents = np.zeros((corners, 3))
    signs = np.zeros(corners)
    uv0 = np.zeros((corners, 2))
    uv1 = np.zeros((corners, 2))
    vertex_positions = [to_model @ v.co for v in mesh.vertices]
    groups = {g.index: g.name for g in obj.vertex_groups}
    vertex_bones = []
    for v in mesh.vertices:
        pairs = [(bone_index[groups[g.group]], g.weight) for g in v.groups
                 if g.group in groups and groups[g.group] in bone_index and g.weight > 0]
        if not pairs:
            warnings.add(f"{obj.name}: vertices without a bone's weight (given the root's)")
            pairs = [(0, 1.0)]
        vertex_bones.append(sorted(pairs, key=lambda p: -p[1])[:4])
    joints = np.zeros((corners, 4), dtype=np.int64)
    weights = np.zeros((corners, 4))
    for loop in mesh.loops:
        i = loop.index
        positions[i] = vertex_positions[loop.vertex_index]
        n = normal_matrix @ Vector(mesh.corner_normals[i].vector)
        t = to_model.to_3x3() @ loop.tangent
        normals[i] = n.normalized()
        tangents[i] = t.normalized()
        signs[i] = loop.bitangent_sign
        uv0[i] = uv_layer.data[i].uv
        if second is not None:
            uv1[i] = second.data[i].uv
        for k, (bone, weight) in enumerate(vertex_bones[loop.vertex_index]):
            joints[i, k] = bone
            weights[i, k] = weight
    materials = np.array([p.material_index for p in mesh.polygons], dtype=np.int64)
    # (Blender's front faces wind counter-clockwise about their outward normals, CE's clockwise: each triangle
    # written reversed, so the stock part's culling keeps the outside)
    triangles = np.array([[p.loop_start, p.loop_start + 2, p.loop_start + 1] for p in mesh.polygons], dtype=np.int64)
    material_names = [slot.material for slot in obj.material_slots]
    evaluated.to_mesh_clear()
    # (Blender's uv v runs up; Halo's down, as the extractors flip it)
    uv0[:, 1] = 1.0 - uv0[:, 1]
    uv1[:, 1] = 1.0 - uv1[:, 1]
    return {"positions": positions, "normals": normals, "tangents": tangents, "signs": signs, "uv0": uv0,
            "uv1": uv1, "joints": joints, "weights": weights, "triangles": triangles, "materials": materials,
            "material_slots": material_names}


def merge_corners(arrays, triangles):
    """corners of identical attributes merged: the piece's vertices and its triangles' indices into them"""
    keyed = np.concatenate([np.round(arrays["positions"] * 1e6), np.round(arrays["normals"] * 1e5),
                            np.round(arrays["tangents"] * 1e3), arrays["signs"][:, None],
                            np.round(arrays["uv0"] * 1e6), np.round(arrays["uv1"] * 1e6), arrays["joints"],
                            np.round(arrays["weights"] * 1e4)], axis=1)
    used = np.unique(triangles.reshape(-1))
    unique, first, inverse = np.unique(keyed[used], axis=0, return_index=True, return_inverse=True)
    remap = np.zeros(len(keyed), dtype=np.int64)
    remap[used] = inverse.reshape(-1)
    return used[first], remap[triangles]


def piece_vertices(arrays, corners):
    out = np.zeros(len(corners), dtype=pmesh.VERTEX_DTYPE)
    out["position"] = arrays["positions"][corners]
    out["normal"] = pmesh.octahedral(arrays["normals"][corners])
    out["tangent"] = pmesh.octahedral(arrays["tangents"][corners])
    out["uv0"] = arrays["uv0"][corners]
    out["uv1"] = arrays["uv1"][corners].astype(np.float16)
    out["joints"] = arrays["joints"][corners]
    out["weights"] = pmesh.quantise_weights(arrays["weights"][corners])
    out["sign"] = np.where(arrays["signs"][corners] < 0, -1, 1)
    return out


def stock_tree(dump):
    """the model's most detailed geometry as the game draws it, for the markers' comparison"""
    blob_path = os.path.splitext(dump["_path"])[0] + ".bin"
    if not os.path.exists(blob_path):
        return None
    blob = open(blob_path, "rb").read()
    points, polygons = [], []
    for region in dump["regions"]:
        for permutation in region["permutations"]:
            geometry_index = permutation["geometries"][4]
            if not 0 <= geometry_index < len(dump["geometries"]):
                continue
            for part in dump["geometries"][geometry_index]["parts"]:
                count = part["vertex_count"]
                if count <= 0:
                    continue
                raw = np.frombuffer(blob, dtype="<f4", count=count * 17, offset=part["vertex_offset"]).reshape(count, 17)
                strip = np.frombuffer(blob, dtype="<u2", count=part["index_count"], offset=part["index_offset"])
                strip = strip.astype(np.int64)
                if part["strip"]:
                    triangles = np.stack([strip[:-2], strip[1:-1], strip[2:]], axis=1)
                else:
                    triangles = strip.reshape(-1, 3)
                base = len(points)
                points.extend(Vector(p) for p in raw[:, 0:3])
                polygons.extend([int(a) + base, int(b) + base, int(c) + base] for a, b, c in triangles
                                if a != b and b != c and a != c)
    return BVHTree.FromPolygons(points, polygons) if polygons else None


def depth_inside(trees, point):
    """how far inside the surfaces a point is (a ray's crossings odd), else None"""
    crossings, nearest = 0, None
    for tree in trees:
        origin, direction = point.copy(), Vector((0.0, 0.0, 1.0))
        for _ in range(256):
            hit = tree.ray_cast(origin, direction)
            if hit[0] is None:
                break
            crossings += 1
            origin = hit[0] + direction * 1e-6
        close = tree.find_nearest(point)
        if close[0] is not None and (nearest is None or close[3] < nearest):
            nearest = close[3]
    return nearest if crossings % 2 == 1 else None


def marker_warnings(dump, binds, level0_objects, depsgraph):
    """markers the new surface puts more than 2 cm deeper inside than the stock model's (its effects, a
    muzzle's flash, an attachment, would start inside the new surface)"""
    found = []
    trees = [BVHTree.FromObject(obj.evaluated_get(depsgraph), depsgraph) for obj in level0_objects]
    stock = stock_tree(dump)
    for marker in dump.get("markers", []):
        for instance in marker["instances"]:
            node = instance["node"]
            if not 0 <= node < len(binds):
                continue
            point = Vector((binds[node] @ np.array([*instance["translation"], 1.0]))[:3])
            depth = depth_inside(trees, point)
            if depth is None or depth <= MARKER_INSIDE:
                continue
            stock_depth = depth_inside([stock], point) if stock is not None else None
            if stock_depth is not None and depth <= stock_depth + MARKER_INSIDE:
                continue
            found.append(f"marker {marker['name']!r} (node {node}) is {depth:.4f} world units inside the new surface "
                         f"(the stock model's: {'outside' if stock_depth is None else f'{stock_depth:.4f}'}): "
                         f"its effects start inside it")
    return found


def compile_rig_section(rig_path, bones, node_count):
    """a .rig.json compiled against the mesh's skeleton: the RIG section's bytes (tools/prism_rig.py's,
    the host's xgpu_rig_compile_json)"""
    import prism_rig  # (OPENH1_TOOLS)
    skeleton = [{"name": b.name, "parent": int(b.parent),
                 "bind_world": [float(v) for v in np.asarray(b.bind, dtype=np.float32).reshape(12)],
                 "inverse_bind": [float(v) for v in np.asarray(b.inverse, dtype=np.float32).reshape(12)]}
                for b in bones]
    return prism_rig.compile_rig(json.loads(open(rig_path).read()), skeleton, node_count)


def export(blend, dump_path, out, scale, uv_name, rig_path=None):
    bpy.ops.wm.open_mainfile(filepath=blend)
    dump = json.loads(open(dump_path).read())
    dump["_path"] = os.path.abspath(dump_path)
    binds = node_binds(dump)
    armatures = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if len(armatures) != 1:
        raise SystemExit(f"{len(armatures)} armatures: one is wanted")
    armature = armatures[0]
    node_names = [n["name"] for n in dump["nodes"]]
    bone_names = [b.name for b in armature.data.bones]
    missing = [n for n in node_names if n not in bone_names]
    if missing:
        raise SystemExit(f"the armature lacks the model's nodes: {', '.join(missing)}")
    to_model = Matrix.Scale(scale, 4) @ armature.matrix_world
    problems = []
    for index, name in enumerate(node_names):
        head = to_model @ armature.data.bones[name].head_local
        expected = binds[index][:3, 3]
        if np.linalg.norm(np.array(head) - expected) > 1e-3:
            problems.append(f"node {name!r} is at {tuple(round(x, 5) for x in head)}, the model's at "
                            f"{tuple(round(float(x), 5) for x in expected)}")
    if problems:
        raise SystemExit("the armature's bind pose is not the model's:\n  " + "\n  ".join(problems))
    # bones: the nodes in the model's order, then helpers parents first
    bones = []
    bone_index = {}
    for index, name in enumerate(node_names):
        bind = binds[index]
        bones.append(pmesh.Bone(name, dump["nodes"][index]["parent"], index, bind[:3, :], np.linalg.inv(bind)[:3, :]))
        bone_index[name] = index
    helpers = [b for b in armature.data.bones if b.name not in bone_index]
    while helpers:
        progressed = False
        for bone in list(helpers):
            if bone.parent is None or bone.parent.name in bone_index:
                matrix = np.array(to_model @ bone.matrix_local)
                parent = bone_index.get(bone.parent.name, 0) if bone.parent else 0
                bone_index[bone.name] = len(bones)
                bones.append(pmesh.Bone(bone.name, parent, -1, matrix[:3, :], np.linalg.inv(matrix)[:3, :]))
                helpers.remove(bone)
                progressed = True
        if not progressed:
            raise SystemExit("helper bones in a cycle")
    if len(bones) > pmesh.BONES_MAXIMUM:
        raise SystemExit(f"{len(bones)} bones: at most {pmesh.BONES_MAXIMUM}")
    depsgraph = bpy.context.evaluated_depsgraph_get()
    collections = lod_collections()
    if not collections:
        raise SystemExit("no lod0 collection")
    warnings = set()
    vertices, indices, pieces, levels = [], [], [], []
    vertex_at = index_at = 0
    level0_objects = []
    for level_number, collection in enumerate(collections):
        objects = [o for o in collection.all_objects if o.type == "MESH"]
        if level_number == 0:
            level0_objects = objects
        groups = {}
        for obj in objects:
            region = region_index(dump, obj.get("region", 0))
            permutation = permutation_index(dump, region, obj.get("permutation", 0))
            arrays = object_triangles(obj, depsgraph, scale, uv_name, bone_index, warnings)
            for slot_index, material in enumerate(arrays["material_slots"] or [None]):
                value = material.get("ce_shader", material.name) if material is not None else 0
                shader = shader_index(dump, value)
                chosen = arrays["triangles"][arrays["materials"] == slot_index] if arrays["material_slots"] else \
                    arrays["triangles"]
                if len(chosen):
                    groups.setdefault((region, permutation, shader), []).append((arrays, chosen))
        level_vertex_first, level_index_first, piece_first = vertex_at, index_at, len(pieces)
        points = []
        for (region, permutation, shader), parts in sorted(groups.items()):
            piece_v, piece_i, base = [], [], 0
            for arrays, chosen in parts:
                corners, remapped = merge_corners(arrays, chosen)
                piece_v.append(piece_vertices(arrays, corners))
                piece_i.append(remapped + base)
                base += len(corners)
            v = np.concatenate(piece_v)
            i = np.concatenate(piece_i).astype(np.uint32).reshape(-1)
            vertices.append(v)
            indices.append(i)
            points.append(v["position"])
            pieces.append(pmesh.Piece(region, permutation, shader, 0, 0, vertex_at, len(v), index_at, len(i)))
            vertex_at += len(v)
            index_at += len(i)
        if len(pieces) == piece_first:
            raise SystemExit(f"{collection.name}: no triangles")
        cutoff = collection.get("cutoff_pixels")
        if cutoff is None:
            cutoff = dump["detail_cutoff_pixels"][max(0, 4 - level_number)] if level_number + 1 < len(collections) \
                else 0.0
        allp = np.concatenate(points).astype(np.float64)
        centre = (allp.max(axis=0) + allp.min(axis=0)) * 0.5
        levels.append(pmesh.Level(float(cutoff), piece_first, len(pieces) - piece_first, level_vertex_first,
                                  vertex_at - level_vertex_first, level_index_first, index_at - level_index_first,
                                  tuple(float(x) for x in centre), float(np.linalg.norm(allp - centre, axis=1).max())))
    all_vertices = np.concatenate(vertices)
    radius = float(np.linalg.norm(all_vertices["position"].astype(np.float64), axis=1).max()) * 1.0001
    mesh = pmesh.Mesh(len(node_names), dump["node_crc"], bones, levels, pieces, all_vertices, np.concatenate(indices),
                      (0.0, 0.0, 0.0), radius,
                      {"source": "art/prism_pmesh_export.py", "blend": os.path.basename(blend),
                       "tag": dump["tag"], "blender": bpy.app.version_string,
                       "levels": [{"cutoff": l.cutoff, "pieces": l.piece_count, "vertices": l.vertex_count,
                                   "triangles": l.index_count // 3} for l in levels]})
    if rig_path:
        mesh.rig = compile_rig_section(rig_path, bones, len(node_names))
    problems = pmesh.check(mesh, dump)
    if problems:
        raise SystemExit("not written:\n  " + "\n  ".join(problems))
    for warning in sorted(warnings) + marker_warnings(dump, binds, level0_objects, depsgraph):
        print(f"warning: {warning}")
    size = pmesh.write_pmesh(__import__("pathlib").Path(out), mesh)
    print(f"{out}: {size} bytes")
    print(pmesh.summary(mesh))


# ---------- import-dump: a test scene of the model as the game draws it

def import_dump(dump_path, out):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    dump = json.loads(open(dump_path).read())
    blob = open(os.path.splitext(dump_path)[0] + ".bin", "rb").read()
    binds = node_binds(dump)
    armature_data = bpy.data.armatures.new("nodes")
    armature = bpy.data.objects.new("nodes", armature_data)
    bpy.context.scene.collection.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = []
    for index, node in enumerate(dump["nodes"]):
        bone = armature_data.edit_bones.new(node["name"])
        bind = Matrix([list(row) for row in binds[index]])
        bone.head = bind.to_translation()
        bone.tail = bind.to_translation() + bind.to_3x3() @ Vector((0.0, 0.0, 0.02))
        edit_bones.append(bone)
    for index, node in enumerate(dump["nodes"]):
        if 0 <= node["parent"] < len(edit_bones):
            edit_bones[index].parent = edit_bones[node["parent"]]
    bpy.ops.object.mode_set(mode="OBJECT")
    lod0 = bpy.data.collections.new("lod0")
    bpy.context.scene.collection.children.link(lod0)
    lod0["cutoff_pixels"] = dump["detail_cutoff_pixels"][4]
    materials = []
    for index, shader in enumerate(dump["shaders"]):
        material = bpy.data.materials.new(f"{index}_{shader['name'].split(chr(92))[-1]}")
        material["ce_shader"] = index
        materials.append(material)
    for region_index_, region in enumerate(dump["regions"]):
        for permutation_index_, permutation in enumerate(region["permutations"]):
            geometry = dump["geometries"][permutation["geometries"][4]]
            for part_index, part in enumerate(geometry["parts"]):
                count = part["vertex_count"]
                if count <= 0:
                    continue
                raw = np.frombuffer(blob, dtype="<f4", count=count * 17, offset=part["vertex_offset"]).reshape(count, 17)
                strip = np.frombuffer(blob, dtype="<u2", count=part["index_count"], offset=part["index_offset"])
                strip = strip.astype(np.int64)
                if part["strip"]:
                    a, b, c = strip[:-2], strip[1:-1], strip[2:]
                    odd = (np.arange(len(a)) & 1) == 1
                    triangles = np.stack([a, np.where(odd, c, b), np.where(odd, b, c)], axis=1)
                else:
                    triangles = strip.reshape(-1, 3)
                keep = (triangles[:, 0] != triangles[:, 1]) & (triangles[:, 1] != triangles[:, 2]) & \
                    (triangles[:, 0] != triangles[:, 2])
                # (CE's front faces wind clockwise: reversed into Blender's counter-clockwise, normals outward;
                # the exporter reverses them back)
                triangles = triangles[keep][:, [0, 2, 1]]
                mesh = bpy.data.meshes.new(f"r{region_index_}p{permutation_index_}part{part_index}")
                mesh.from_pydata(raw[:, 0:3].tolist(), [], triangles.tolist())
                mesh.materials.append(materials[part["shader"]])
                uv = mesh.uv_layers.new(name="uv0")
                for loop in mesh.loops:
                    u, v = raw[loop.vertex_index, 12:14]
                    uv.data[loop.index].uv = (float(u), 1.0 - float(v))
                normals = [Vector(raw[loop.vertex_index, 3:6]).normalized() for loop in mesh.loops]
                mesh.normals_split_custom_set(normals)
                obj = bpy.data.objects.new(mesh.name, mesh)
                obj["region"] = region_index_
                obj["permutation"] = permutation_index_
                lod0.objects.link(obj)
                obj.parent = armature
                modifier = obj.modifiers.new("nodes", "ARMATURE")
                modifier.object = armature
                for node_index, node in enumerate(dump["nodes"]):
                    obj.vertex_groups.new(name=node["name"])
                for vertex_index in range(count):
                    first, second, weight = int(raw[vertex_index, 14]), int(raw[vertex_index, 15]), raw[vertex_index, 16]
                    obj.vertex_groups[first].add([vertex_index], float(weight), "ADD")
                    if second != first and weight < 1.0:
                        obj.vertex_groups[second].add([vertex_index], float(1.0 - weight), "ADD")
    bpy.ops.wm.save_as_mainfile(filepath=out)
    print(f"{out}: the model's most detailed geometry, {len(dump['nodes'])} nodes")


def main():
    parser = argparse.ArgumentParser(prog="prism_pmesh_export.py")
    sub = parser.add_subparsers(dest="command", required=True)
    e = sub.add_parser("export")
    e.add_argument("blend")
    e.add_argument("dump")
    e.add_argument("out")
    e.add_argument("--scale", type=float, default=1.0)
    e.add_argument("--uv", default=None)
    e.add_argument("--rig", default=None)
    i = sub.add_parser("import-dump")
    i.add_argument("dump")
    i.add_argument("out")
    arguments = parser.parse_args(script_args())
    if arguments.command == "export":
        export(os.path.abspath(arguments.blend), arguments.dump, arguments.out, arguments.scale, arguments.uv,
               arguments.rig)
    else:
        import_dump(arguments.dump, os.path.abspath(arguments.out))


main()
