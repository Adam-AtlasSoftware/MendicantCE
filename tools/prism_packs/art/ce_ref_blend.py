"""A Blender scene of a CE model as the extractor wrote it (its glTF's armature and most detailed geometry,
the objects named rig and ce_ref), for hero_pick.py's picking views. Headless Blender:

  blender -b --factory-startup --python art/ce_ref_blend.py -- <model.gltf> <out.blend>

Local only: the scene is the map's mesh.
"""
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402


def main():
    gltf, out = U.script_args()[:2]
    U.reset_scene()
    arm, ref = U.import_ref_gltf(gltf, "superhigh")
    arm.name, ref.name = "rig", "ce_ref"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=out)
    print("CE_REF_OK", out, len(ref.data.polygons), "faces")


main()
