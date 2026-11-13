"""PRISM v2 r2 Elite: one bake scene per base map for prism_bake_ce.py, headless Blender:

  blender -b --factory-startup --python art/prism_elite/elite_bake_prep_r2.py -- <recipe.json> <bake recipe.json> <out dir>

From <recipe out_dir>/build.blend: per entry of the bake recipe's "bitmaps" (one base map, one or more
passes, each a list of new/<part> objects whose faces of the given shaders use that map):
  game   the in-game superhigh mesh of those parts (rest pose, its CE UVs)
  high   the same mesh one Catmull-Clark level finer (its sharp edges creased: walls, rims and CE's hard
         edges stay crisp) with a material per shader that bends its normal by
           - the detail CE paints that the mesh is too coarse for (elite_bake.detail_height: the dark
             panel seams of the plates as grooves, the bodysuit's hex cells and the skin's pebbles as
             relief), a bump of bake recipe "bump_mm" depth;
           - a Bevel node of "bevel_mm" radius: every hard edge gets a narrow rounded highlight;
  occ    every other new/ part of the same permutation set (the occlusion rays see the whole Elite)
Writes <out dir>/<bitmap>__<pass>.blend and a list of the prism_bake_ce.py runs (bake_jobs.json).
Local only: CE vertices and pixels.
"""
import json
import os
import sys

import bpy
import bmesh

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import bl_util as U  # noqa: E402
import elite_bake as EBK  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

ROOT = E.WORK
MM_WU = 1 / 3048.0


def P(rel):
    return E.path(rel)


def joined_copy(objs, name):
    bm = bmesh.new()
    mats = []
    for o in objs:
        me = o.data.copy()
        # material slots unified by name
        remap = []
        for m in o.data.materials:
            if m.name not in [x.name for x in mats]:
                mats.append(m)
            remap.append([x.name for x in mats].index(m.name))
        for p in me.polygons:
            p.material_index = remap[p.material_index]
        me.transform(o.matrix_world)
        bm.from_mesh(me)
        bpy.data.meshes.remove(me)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for m in mats:
        me.materials.append(m)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    for p in me.polygons:
        p.use_smooth = True
    return ob


def main():
    args = U.script_args()
    cfg = U.load_config(args[0])
    bc = U.load_config(args[1])
    out = P(args[2])
    os.makedirs(out, exist_ok=True)
    build = os.path.join(P(cfg["out_dir"]), "build.blend")
    bmp_dir = P(cfg["bitmap_dir"])
    jobs = []
    for bitmap, spec in bc["bitmaps"].items():
        base = EBK.load_rgba(os.path.join(bmp_dir, bitmap + ".png"))
        multi = EBK.load_rgba(os.path.join(bmp_dir, bitmap + " multi.png"))
        if multi.shape[0] != base.shape[0]:   # (a half-size multipurpose map: bilinear up to the base's)
            multi = EBK.upsample(multi, base.shape[0] // multi.shape[0])
        hc = dict(cfg)
        hc["plates"] = cfg["plates_map"]
        hc["height"] = bc["height"]
        hc["height_factor"] = bc.get("height_factor", 4)
        hgt, arm, glow, seams = EBK.detail_height(base, multi, hc, spec.get("skin", False))
        for pi, parts in enumerate(spec["passes"]):
            bpy.ops.wm.open_mainfile(filepath=build)
            sc = bpy.context.scene
            hh, hw = hgt.shape
            import numpy as np
            himg = EBK.float_image("h_" + bitmap, hw, hh, np.dstack([hgt * 0.5 + 0.5] * 3 + [np.ones_like(hgt)]))
            himg.filepath_raw = os.path.join(out, "height_%s.png" % bitmap.replace(" ", "_"))
            himg.file_format = "PNG"
            himg.save()
            himg.pack()
            news = [bpy.data.objects["new/" + p] for p in parts]
            perm_parts = spec.get("occluders", {}).get(str(pi), bc.get("occluders", []))
            # (never a part of the same region: another permutation of it fills the same space and
            # would put the whole baked part in shadow)
            regions = {p.split("/")[0] for p in parts}
            occs = [bpy.data.objects["new/" + p] for p in perm_parts if "new/" + p in bpy.data.objects
                    and p.split("/")[0] not in regions]
            game = joined_copy(news, "game")
            high = joined_copy(news, "high")
            occ = joined_copy(occs, "occ") if occs else None
            # crease the high's sharp edges, then one Catmull-Clark level
            bm = bmesh.new()
            bm.from_mesh(high.data)
            cr = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
            for e in bm.edges:
                e[cr] = 1.0 if (not e.smooth or e.is_boundary) else 0.0
            bm.to_mesh(high.data)
            bm.free()
            m = high.modifiers.new("s", "SUBSURF")
            m.levels = m.render_levels = bc.get("high_levels", 1)
            m.uv_smooth = "NONE"
            m.use_creases = True
            m.use_limit_surface = True
            bpy.context.view_layer.objects.active = high
            for o in bpy.data.objects:
                o.select_set(o is high)
            bpy.ops.object.modifier_apply(modifier="s")
            # materials: the bump and bevel on the shaders that use this bitmap
            shaders = set(spec["shaders"])
            for i, slot in enumerate(high.material_slots):
                sh = slot.material.name.replace("/", "\\").split("\\")[-1].split(".")[0].lower()
                hm = bpy.data.materials.new(sh)
                hm.use_nodes = True
                nt = hm.node_tree
                bsdf = nt.nodes["Principled BSDF"]
                if sh in shaders:
                    bev = nt.nodes.new("ShaderNodeBevel")
                    bev.samples = bc.get("bevel_samples", 8)
                    bev.inputs["Radius"].default_value = bc.get("bevel_mm", 1.5) * MM_WU
                    tx = nt.nodes.new("ShaderNodeTexImage")
                    tx.image = himg
                    tx.interpolation = "Cubic"
                    uvn = nt.nodes.new("ShaderNodeUVMap")
                    uvn.uv_map = "UVMap"
                    nt.links.new(uvn.outputs[0], tx.inputs[0])
                    bump = nt.nodes.new("ShaderNodeBump")
                    bump.inputs["Strength"].default_value = 1.0
                    bump.inputs["Distance"].default_value = bc.get("bump_mm", 2.0) * MM_WU
                    nt.links.new(tx.outputs[0], bump.inputs["Height"])
                    nt.links.new(bev.outputs[0], bump.inputs["Normal"])
                    nt.links.new(bump.outputs[0], bsdf.inputs["Normal"])
                slot.material = hm
            # keep only what the bake needs
            for o in list(bpy.data.objects):
                if o not in (game, high, occ):
                    bpy.data.objects.remove(o, do_unlink=True)
            name = "%s__%d" % (bitmap.replace(" ", "_"), pi)
            path = os.path.join(out, name + ".blend")
            bpy.ops.wm.save_as_mainfile(filepath=path)
            jobs.append({"bitmap": bitmap, "pass": pi, "blend": path, "raw": os.path.join(out, "raw_" + name),
                         "shaders": sorted(shaders), "occluders": "occ" if occ else ""})
            print("PREP", name, len(game.data.polygons), len(high.data.polygons), flush=True)
    json.dump(jobs, open(os.path.join(out, "bake_jobs.json"), "w"), indent=1)
    print("PREP_OK", len(jobs))


main()
