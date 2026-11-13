"""Orthographic picking views of a model (headless Blender, Workbench) with their cameras, plus the model's
world-space triangles, corner UVs and shaders, so features can be placed by pixel (hero_pick_query.py) and
mapped to the base map (hero_height2.py).

  blender -b --factory-startup --python art/hero_pick.py -- <file.blend> <texture.png> <out dir> <object>

Writes <out>/<view>.png (1000 px), <out>/g_<view>.png (with a 50 px grid), <out>/cams.json and <out>/mesh.npz
(co, tri, uv per corner, mat per triangle, names). Local only: CE vertices and pixels.
"""
import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

VIEWS = {"front": (0, 0.5, 1.0), "back": (180, 0.5, 1.0), "left": (90, 0.5, 1.0), "right": (270, 0.5, 1.0),
         "head_f": (0, 0.9, 0.22), "head_l": (90, 0.9, 0.22), "head_r": (270, 0.9, 0.22), "head_b": (180, 0.9, 0.22),
         "head_fl": (45, 0.9, 0.22), "head_bl": (135, 0.9, 0.22),
         "torso_f": (0, 0.62, 0.4), "torso_b": (180, 0.62, 0.4), "torso_l": (90, 0.62, 0.4), "torso_fl": (40, 0.6, 0.4),
         "abs_f": (0, 0.62, 0.22), "abs_fl": (35, 0.62, 0.22),
         "legs_f": (0, 0.25, 0.5), "legs_b": (180, 0.25, 0.5), "legs_l": (90, 0.25, 0.5), "legs_r": (270, 0.25, 0.5),
         "hand_f": (0, 0.45, 0.45), "hand_b": (180, 0.45, 0.45), "hand_l": (90, 0.45, 0.22),
         "hand_r": (270, 0.45, 0.22), "belt_l": (60, 0.55, 0.3), "belt_r": (300, 0.55, 0.3),
         "belt_bl": (130, 0.55, 0.3), "belt_br": (230, 0.55, 0.3), "torso_r": (270, 0.62, 0.4)}
RES = 1000


def main():
    blend, img, out, objn = sys.argv[sys.argv.index("--") + 1:][:4]
    os.makedirs(out, exist_ok=True)
    bpy.ops.wm.open_mainfile(filepath=blend)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.render.resolution_x = sc.render.resolution_y = RES
    o = bpy.data.objects[objn]
    names = [(s.material.name if s.material else "").replace("/", "\\").split("\\")[-1].split(".")[0].lower()
             for s in o.material_slots]
    dg = bpy.context.evaluated_depsgraph_get()
    eo = o.evaluated_get(dg)
    me = eo.to_mesh()
    me.calc_loop_triangles()
    W = np.array([list(eo.matrix_world @ v.co) for v in me.vertices])
    T = np.array([list(lt.vertices) for lt in me.loop_triangles])
    TL = np.array([list(lt.loops) for lt in me.loop_triangles])
    TM = np.array([me.polygons[lt.polygon_index].material_index for lt in me.loop_triangles])
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers["UVMap"].data.foreach_get("uv", uv)
    np.savez(os.path.join(out, "mesh.npz"), co=W, tri=T, uv=uv.reshape(-1, 2)[TL], mat=TM, names=np.array(names))
    eo.to_mesh_clear()
    m = bpy.data.materials.new("pick")
    m.use_nodes = True
    t = m.node_tree.nodes.new("ShaderNodeTexImage")
    t.image = bpy.data.images.load(img)
    m.node_tree.links.new(t.outputs[0], m.node_tree.nodes["Principled BSDF"].inputs[0])
    m.node_tree.nodes.active = t
    for x in bpy.data.objects:
        if x.type == "MESH":
            x.hide_render = x is not o
    o.data.materials.clear()
    o.data.materials.append(m)
    for p in o.data.polygons:
        p.material_index = 0
    lo, hi = W.min(0), W.max(0)
    c = (lo + hi) / 2
    h = hi[2] - lo[2]
    cd = bpy.data.cameras.new("c")
    cam = bpy.data.objects.new("c", cd)
    sc.collection.objects.link(cam)
    sc.camera = cam
    cd.type = "ORTHO"
    cams = {}
    for k, (az, f, span) in VIEWS.items():
        a = math.radians(az)
        tgt = Vector((c[0], c[1], lo[2] + f * h))
        cam.location = tgt + Vector((math.cos(a), math.sin(a), 0.0)) * 3 * h
        cam.rotation_euler = (tgt - cam.location).to_track_quat("-Z", "Y").to_euler()
        cd.ortho_scale = span * h * 1.1
        bpy.context.view_layer.update()
        M = cam.matrix_world.to_3x3()
        cams[k] = {"loc": list(cam.location), "right": list(M @ Vector((1, 0, 0))), "up": list(M @ Vector((0, 1, 0))),
                   "fwd": list(M @ Vector((0, 0, -1))), "scale": cd.ortho_scale, "res": RES}
        sc.render.filepath = os.path.join(out, k + ".png")
        bpy.ops.render.render(write_still=True)
    json.dump(cams, open(os.path.join(out, "cams.json"), "w"), indent=1)
    try:
        import cv2  # noqa: F401
    except ImportError:
        pass
    print("PICK_VIEWS_OK", len(cams))


main()
