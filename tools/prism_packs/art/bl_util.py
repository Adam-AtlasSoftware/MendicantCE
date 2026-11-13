"""Blender (bpy) helpers shared by the art pipeline scripts. Import only inside Blender."""
import json
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import halo_formats as hf  # noqa: E402
import packs_env as E  # noqa: E402


def script_args():
    """Arguments after `--` on the blender command line."""
    argv = sys.argv
    return argv[argv.index("--") + 1:] if "--" in argv else []


def load_config(path):
    with open(path) as f:
        return json.load(f)


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_ref_gltf(path, keep_lod="superhigh"):
    """Import the map_extract glTF. Keep the armature and one LOD mesh.

    Returns (armature_object, mesh_object). World space == Halo model space (Z up, world
    units); the armature object carries the root node's Y-up -> Z-up rotation.
    """
    bpy.ops.import_scene.gltf(filepath=path, bone_heuristic="TEMPERANCE")
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    keep = None
    for o in list(bpy.data.objects):
        if o.type == "MESH" and o.name.endswith("/" + keep_lod):
            keep = o
    for o in list(bpy.data.objects):
        if o is arm or o is keep:
            continue
        bpy.data.objects.remove(o, do_unlink=True)
    return arm, keep


def jms_rest_world(jms):
    """Halo-space rest (bind) matrices per node name from a JMS node list (world units)."""
    par = jms.parents()
    out = {}
    mats = [None] * len(jms.nodes)
    for i, n in enumerate(jms.nodes):  # JMS lists parents before children
        local = Matrix.Translation(Vector(n.pos) / 100.0) @ Quaternion(hf.std_quat(n.rot)).to_matrix().to_4x4()
        mats[i] = local if par[i] < 0 else mats[par[i]] @ local
        out[n.name] = mats[i]
    return out


def jma_frame_world(jma, frame):
    """Halo-space node matrices per node name for one JMA frame."""
    par = jma.parents()
    fr = jma.frames[frame]
    mats = [None] * len(jma.node_names)
    order = list(range(len(jma.node_names)))
    done = set()
    while order:  # resolve parents first regardless of listing order
        rest = []
        for i in order:
            if par[i] >= 0 and par[i] not in done:
                rest.append(i)
                continue
            pos, rot, sc = fr[i]
            local = (Matrix.Translation(Vector(pos) / 100.0)
                     @ Quaternion(hf.std_quat(rot)).to_matrix().to_4x4()
                     @ Matrix.Scale(sc, 4))
            mats[i] = local if par[i] < 0 else mats[par[i]] @ local
            done.add(i)
        order = rest
    return {n: mats[i] for i, n in enumerate(jma.node_names)}


def bone_corrections(arm, rest_world):
    """Per bone: C = H_rest^-1 @ W_bone so that any Halo node pose P gives bone world P @ C.

    Also returns the max rest-position error between JMS bind and the imported bones.
    """
    corr, err = {}, 0.0
    for b in arm.data.bones:
        W = arm.matrix_world @ b.matrix_local
        H = rest_world[b.name]
        corr[b.name] = H.inverted() @ W
        err = max(err, (W.translation - H.translation).length)
    return corr, err


def apply_pose(arm, node_world, corr):
    """Set pose bones so each bone follows the given Halo-space node matrix."""
    aw_inv = arm.matrix_world.inverted()
    target = {}
    for b in arm.data.bones:
        if b.name in node_world:
            target[b.name] = aw_inv @ node_world[b.name] @ corr[b.name]
        else:  # node absent from the animation: keep rest relative to parent
            target[b.name] = None
    pose_arm = {}
    for b in arm.data.bones:  # bones are ordered parent-first in bpy
        pb = arm.pose.bones[b.name]
        rest = b.matrix_local
        if target[b.name] is None:
            if b.parent:
                pose_arm[b.name] = pose_arm[b.parent.name] @ (b.parent.matrix_local.inverted() @ rest)
            else:
                pose_arm[b.name] = rest.copy()
            pb.matrix_basis = Matrix.Identity(4)
            continue
        P = target[b.name]
        if b.parent:
            basis = (rest.inverted() @ b.parent.matrix_local) @ pose_arm[b.parent.name].inverted() @ P
        else:
            basis = rest.inverted() @ P
        pb.matrix_basis = basis
        pose_arm[b.name] = P
    bpy.context.view_layer.update()


def clear_pose(arm):
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()


def evaluated_world_coords(obj):
    """Evaluated (modifiers/armature applied) vertex world coordinates as a list of Vectors."""
    dg = bpy.context.evaluated_depsgraph_get()
    eo = obj.evaluated_get(dg)
    me = eo.to_mesh()
    mw = eo.matrix_world
    pts = [mw @ v.co for v in me.vertices]
    eo.to_mesh_clear()
    return pts


def mesh_bvh(obj, evaluated=True):
    from mathutils.bvhtree import BVHTree
    dg = bpy.context.evaluated_depsgraph_get()
    eo = obj.evaluated_get(dg) if evaluated else obj
    bm = bmesh.new()
    bm.from_object(eo, dg) if evaluated else bm.from_mesh(obj.data)
    bm.transform(obj.matrix_world)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.normal_update()
    tree = BVHTree.FromBMesh(bm)
    return tree, bm


def set_cycles_gpu(scene, device_name_substr=None, samples=32):
    """Cycles on the graphics cards named by part of their name (PRISM_PACKS_GPU's by default: packs_env.py,
    OptiX, CUDA, HIP, oneAPI or Metal); the processor when there is none."""
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    picked, _cards = E.cycles_gpu(prefs, device_name_substr)
    scene.cycles.device = "GPU" if picked else "CPU"
    scene.cycles.samples = samples
    return picked or "CPU"
