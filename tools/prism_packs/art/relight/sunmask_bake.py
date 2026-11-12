"""PRISM baked shadows, stage 1 (headless Blender, Cycles): the sun's visibility baked finely for
display.prism_baked_shadows (prism_lightmaps.c's .psm pages; sunmask_encode.py writes them).

Local only, from the player's own map (nothing made of the game's data leaves the machine).

For one structure BSP whose sky has a sun, and for each lightmap page the sun reaches:

  CE's own geometry (relight_bake.py's scene: the BSP's render mesh, its materials; the sun the
  game's: render_sky_sun's brightest sky light, its direction as the game logs it, --sun):
    du   1x   the sun's direct light on CE's normal, unshadowed (the light a texel would have
              from it if nothing stood in the way; CE's units at the sun's scale 1)
    vce  1x   the sun's visibility as a page at CE's size can hold it (its own diameter)
    vrb  4x   ... as the rebaked page (relight_bake.py --stage final --factor 4) holds it
  the remodel (the .pwg's near triangles where the BSP's material has one, else CE's own;
  alpha-tested surfaces cut out by their base map's alpha, transparent ones pass light):
    vhi  Fx   the sun's visibility at F times CE's density (8 or 16: the page's texels' size in
              the world over --target-wu, at most --max-factor), through a disc as wide as the
              ray tracing's (display.prism_rt's 0.02 radian radius) or the light's own, whichever
              is wider

usage: blender -b --factory-startup --python sunmask_bake.py -- --gltf <bsp.gltf>
         --lights <map_lights.json> --bsp <index> --calib <relight bake dir> --out <dir>
         [--pwg <bsp.pwg>] [--sun "x y z"] [--gpu <part of a card's name>] [--samples 64] [--penumbra 0.04]
         [--target-wu 0.03] [--max-factor 16] [--batch-texels 40000000] [--pages 1,2]
Output: <out>/sun_<page>.npz (vce, du, vrb, vhi: float32, rows from v = 0; vhi's alpha the
coverage), <out>/sun_meta.json.
"""
import json
import math
import os
import sys
import time

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import packs_env as E  # noqa: E402
from sunmask_pwg import read_pwg  # noqa: E402

# relight_bake.py's functions (its scene, materials, bakes), without running it
_source = open(os.path.join(HERE, "relight_bake.py")).read()
_source = _source[:_source.rindex("\nmain()")]
RB = {"__name__": "relight_bake", "__file__": os.path.join(HERE, "relight_bake.py")}
_argv = sys.argv
sys.argv = [a for a in sys.argv]
exec(compile(_source, "relight_bake.py", "exec"), RB)
sys.argv = _argv

LUMA = np.array([0.2126, 0.7152, 0.0722])


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = {"gpu": E.GPU, "samples": 64, "penumbra": 0.04, "target-wu": 0.03, "max-factor": 16, "min-factor": 2,
         "batch-texels": 40_000_000, "pages": None, "pwg": None, "sun": None, "bounces": 0, "share": 0.03,
         "widest": 0.1}
    i = 0
    while i < len(a):
        o[a[i][2:]] = a[i + 1]
        i += 2
    for k in ("samples", "max-factor", "min-factor", "batch-texels", "bsp"):
        o[k] = int(o[k])
    for k in ("penumbra", "target-wu", "share", "widest"):
        o[k] = float(o[k])
    o["bounces"] = 0
    return o


def calib_page(calib, page, scales):
    """the calibration bake's light at CE's size (rows from v = 0): the total in CE's linear
    units (luminance), the sky lights' share, the texel's world size"""
    d = np.load(os.path.join(calib, f"calib_{page}.npz"))
    meta = json.load(open(os.path.join(calib, "calib_meta.json")))
    total = np.zeros(d["position"].shape[:2])
    sun = d["sun"][..., :3] @ LUMA * scales["sun"]
    total += sun + d["sky"][..., :3] @ LUMA * scales["sky"]
    for key, shader in meta["groups"].items():
        if key.startswith("emit"):
            total += d[key][..., :3] @ LUMA * scales["emit"].get(shader, 0.0)
    pos = d["position"][..., :3]
    cov = d["position"][..., 3] > 0
    step = np.linalg.norm(pos[:, 1:] - pos[:, :-1], axis=2)
    ok = cov[:, 1:] & cov[:, :-1]
    near = step[ok][step[ok] < 5] if ok.any() else np.zeros(0)
    texel = float(np.median(near)) if len(near) else 0.1
    return total[::-1], sun[::-1], cov[::-1], texel


def main_sun(lights, bsp_index, sky_index):
    """render_sky_sun's choice: the sky light of most power times its brightest channel"""
    sky = lights["skies"][sky_index]
    best, score_best = None, -1.0
    for i, l in enumerate(sky["lights"]):
        s = l["power"] * max(l["color"]) if l["power"] > 0 and max(l["color"]) > 0 else (
            0.0 if l.get("lens_flare") else -1.0)
        if s > score_best:
            best, score_best = i, s
    return best, sky["lights"][best]


def point_sun(lo, d):
    import mathutils
    z = np.asarray(d, float)
    z = z / np.linalg.norm(z)
    up = np.array([0, 0, 1.0]) if abs(z[2]) < 0.99 else np.array([1.0, 0, 0])
    x = np.cross(up, z)
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    lo.matrix_world = mathutils.Matrix(((x[0], y[0], z[0], 0), (x[1], y[1], z[1], 0), (x[2], y[2], z[2], 0),
                                        (0, 0, 0, 1)))


def set_shadow(ld, on):
    for owner, attr in ((ld.cycles, "cast_shadow"), (ld, "use_shadow")):
        if hasattr(owner, attr):
            setattr(owner, attr, on)


def image_read(img):
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    # (Blender keeps rows from the bottom; a page's rows run from v = 0)
    return px.reshape(h, w, 4)[::-1].copy()


def bake_pages(objects, pages, sizes, factor_of, kind, label, prefill=None):
    """one bake of the pages' objects (each its own image at its factor): {page: rows from v = 0}"""
    images = {}
    for page in pages:
        w, h = sizes[page]
        f = factor_of(page)
        img = bpy.data.images.new(f"{label}_{page}", w * f, h * f, alpha=True, float_buffer=True)
        img.colorspace_settings.name = "Non-Color"
        if prefill is not None:
            img.pixels.foreach_set(np.full(w * f * h * f * 4, prefill, np.float32))
        images[page] = img
        for slot in objects[page].material_slots:
            nt = slot.material.node_tree
            nt.nodes["bake"].image = img
            nt.nodes.active = nt.nodes["bake"]
    bpy.ops.object.select_all(action="DESELECT")
    for page in pages:
        objects[page].select_set(True)
    bpy.context.view_layer.objects.active = objects[pages[0]]
    t = time.time()
    if kind == "SHADOW":
        bpy.ops.object.bake(type="SHADOW", margin=0, use_clear=prefill is None)
    else:
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT"}, margin=0, use_clear=True)
    out = {page: image_read(img) for page, img in images.items()}
    print(f"sunmask: bake {label} ({len(pages)} pages, {sum(i.size[0] * i.size[1] for i in images.values()) / 1e6:.1f} "
          f"Mtexels) {time.time() - t:.1f} s", flush=True)
    for img in images.values():
        bpy.data.images.remove(img)
    return out


def ce_page_materials(objects):
    """relight_bake.py's scene: each page's object its own copies of its materials (a bake target each)"""
    for page, ob in objects.items():
        for i, slot in enumerate(ob.material_slots):
            m = slot.material
            key = f"{m.name}@{page}"
            mc = bpy.data.materials.get(key) or m.copy()
            mc.name = key
            ob.material_slots[i].material = mc


def cutout_alpha(base, png, cache):
    """the base map's alpha (H x W float, rows from v = 0) where a fifth or more of its texels are under
    half: an alpha-tested surface's holes; else None"""
    if not png:
        return None
    if png in cache:
        return cache[png]
    path = os.path.join(base, "..", "..", "..", png)
    res = None
    try:
        im = bpy.data.images.load(os.path.abspath(path))
        if im.channels == 4:
            px = np.empty(im.size[0] * im.size[1] * 4, dtype=np.float32)
            im.pixels.foreach_get(px)
            a = px.reshape(-1, 4)[:, 3]
            if (a < 0.5).mean() >= 0.05 and (a < 0.5).mean() <= 0.95:
                res = im
                im = None
        if im is not None:
            bpy.data.images.remove(im)
    except Exception as e:
        print("sunmask: base map not read:", path, e, flush=True)
    cache[png] = res
    return res


def remodel_material(name, kind, alpha_image=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    diff = nt.nodes.new("ShaderNodeBsdfDiffuse")
    diff.inputs["Color"].default_value = (0.4, 0.4, 0.4, 1.0)
    if kind == "opaque":
        nt.links.new(diff.outputs[0], out.inputs[0])
    else:
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        lp = nt.nodes.new("ShaderNodeLightPath")
        mix = nt.nodes.new("ShaderNodeMixShader")
        if kind == "cutout":
            # (opaque where the base map's alpha passes half, to every ray; the bake's own ray off
            # its own surface sees the surface)
            uvn = nt.nodes.new("ShaderNodeUVMap")
            uvn.uv_map = "base"
            tex = nt.nodes.new("ShaderNodeTexImage")
            tex.image = alpha_image
            tex.interpolation = "Closest"
            nt.links.new(uvn.outputs[0], tex.inputs[0])
            gt = nt.nodes.new("ShaderNodeMath")
            gt.operation = "GREATER_THAN"
            gt.inputs[1].default_value = 0.5
            nt.links.new(tex.outputs["Alpha"], gt.inputs[0])
            mx = nt.nodes.new("ShaderNodeMath")
            mx.operation = "MAXIMUM"
            nt.links.new(gt.outputs[0], mx.inputs[0])
            nt.links.new(lp.outputs["Is Camera Ray"], mx.inputs[1])
            nt.links.new(mx.outputs[0], mix.inputs[0])
        else:
            nt.links.new(lp.outputs["Is Camera Ray"], mix.inputs[0])
        nt.links.new(tr.outputs[0], mix.inputs[1])
        nt.links.new(diff.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], out.inputs[0])
    img = nt.nodes.new("ShaderNodeTexImage")
    img.name = "bake"
    nt.nodes.active = img
    return m


def build_remodel(prims, base, pwg):
    """the pages' objects of the remodel: the .pwg's near triangles in place of each remodelled material's,
    CE's own for the rest (transparent ones passing light, alpha-tested ones cut out)"""
    for ob in list(bpy.data.objects):
        if ob.type == "MESH":
            bpy.data.objects.remove(ob, do_unlink=True)
    pages = {}
    for p in prims:
        pages.setdefault(p["page"], []).append(p)
    alpha_cache = {}
    objects = {}
    used = set()
    remodelled = kept = 0
    for page, plist in sorted(pages.items()):
        V, F, U, U0, slots, face_slot = [], [], [], [], [], []
        offset = 0
        for p in plist:
            key = (p["lightmap_index"], p["material_index"])
            transparent = p["group"] != "senv"
            parts = pwg.get(key) if (pwg and not transparent) else None
            if parts:
                used.add(key)
                remodelled += 1
                # (the .pwg keeps the game's clockwise triangles; the glTF's, and Blender's, run counter-clockwise)
                geo = [(q["pos"], q["tri"][:, [0, 2, 1]], q["uv1"], np.zeros((len(q["pos"]), 2))) for q in parts]
                kind, img = "opaque", None
            else:
                kept += 1
                img = None if transparent else cutout_alpha(base, p["base_png"], alpha_cache)
                kind = "transparent" if transparent else ("cutout" if img is not None else "opaque")
                uv0 = p.get("uv0")
                geo = [(p["pos"], p["idx"], p["uv1"] if p["uv1"] is not None else np.zeros((len(p["pos"]), 2)),
                        uv0 if uv0 is not None else np.zeros((len(p["pos"]), 2)))]
            mname = f"{kind}:{img.name if img else ''}@{page}"
            if mname not in slots:
                slots.append(mname)
                if not bpy.data.materials.get(mname):
                    remodel_material(mname, kind, img)
            si = slots.index(mname)
            for pos, tri, uv1, uv0 in geo:
                V.append(pos)
                F.append(tri + offset)
                U.append(uv1)
                U0.append(uv0)
                face_slot.append(np.full(len(tri), si))
                offset += len(pos)
        V = np.concatenate(V)
        F = np.concatenate(F)
        me = bpy.data.meshes.new(f"remodel_{page}")
        me.vertices.add(len(V))
        me.vertices.foreach_set("co", V.astype(np.float32).ravel())
        me.loops.add(F.size)
        me.loops.foreach_set("vertex_index", F.astype(np.int32).ravel())
        me.polygons.add(len(F))
        me.polygons.foreach_set("loop_start", (np.arange(len(F)) * 3).astype(np.int32))
        me.polygons.foreach_set("loop_total", np.full(len(F), 3, dtype=np.int32))
        me.polygons.foreach_set("material_index", np.concatenate(face_slot).astype(np.int32))
        me.update(calc_edges=True)
        me.validate(clean_customdata=False)
        for nm in slots:
            me.materials.append(bpy.data.materials[nm])
        loopv = F.ravel()
        U = np.concatenate(U)
        U0 = np.concatenate(U0)
        uvl = me.uv_layers.new(name="lm")
        uvl.data.foreach_set("uv", np.stack([U[loopv, 0], 1.0 - U[loopv, 1]], 1).astype(np.float32).ravel())
        uvb = me.uv_layers.new(name="base")
        uvb.data.foreach_set("uv", np.stack([U0[loopv, 0], 1.0 - U0[loopv, 1]], 1).astype(np.float32).ravel())
        me.uv_layers.active = uvl
        uvl.active_render = True
        ob = bpy.data.objects.new(f"remodel_{page}", me)
        bpy.context.scene.collection.objects.link(ob)
        objects[page] = ob
    unused = len(set(pwg or {}) - used)
    print(f"sunmask: remodel scene: {len(objects)} pages, {remodelled} materials remodelled, {kept} CE's own, "
          f"{unused} remodel materials not in the glTF", flush=True)
    return objects


def main():
    o = args()
    lights = json.load(open(o["lights"]))
    g, prims, base = RB["read_gltf"](o["gltf"])
    # (CE's base map coordinates too, for the cutouts)
    blob = open(os.path.join(base, g["buffers"][0]["uri"]), "rb").read()
    for i, p in enumerate(g["meshes"][0]["primitives"]):
        at = p["attributes"]
        if "TEXCOORD_0" in at:
            a = g["accessors"][at["TEXCOORD_0"]]
            v = g["bufferViews"][a["bufferView"]]
            off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
            prims[i]["uv0"] = np.frombuffer(blob, np.float32, a["count"] * 2, off).reshape(-1, 2).astype(np.float64)
    del blob
    scales = json.load(open(os.path.join(o["calib"], "scales.json")))
    gamma = float(scales.get("gamma", 1.0))
    scene = RB["setup_cycles"](o)
    scene.cycles.samples = o["samples"]
    objects, materials, emitters, suns, bg, sky_index = RB["build_scene"](o, prims, base, lights)
    light_index, light = main_sun(lights, o["bsp"], sky_index)
    if o["sun"]:
        direction = np.array([float(x) for x in o["sun"].split()])
    else:
        direction = np.array([math.cos(light["pitch"]) * math.cos(light["yaw"]),
                              math.cos(light["pitch"]) * math.sin(light["yaw"]), math.sin(light["pitch"])])
    direction /= np.linalg.norm(direction)
    # the sky lights' lamps (build_scene's: those of power > 0, in order); the main one kept alone
    powered = [i for i, l in enumerate(lights["skies"][sky_index]["lights"]) if l["power"] > 0]
    main_ld = suns[powered.index(light_index)][0]
    main_power = float(light["power"])
    for ld, power in suns:
        ld.energy = 0.0
        for ob in bpy.data.objects:
            if ob.data == ld and ld != main_ld:
                ob.hide_render = True
    main_ob = [ob for ob in bpy.data.objects if ob.data == main_ld][0]
    point_sun(main_ob, direction)
    main_ld.energy = main_power
    main_ld.color = tuple(light["color"])
    bg.inputs["Strength"].default_value = 0.0
    sizes = RB["page_sizes"](prims, base)
    # the pages the sun reaches: its share of the calibration's light somewhere on them
    keep, texels, cal = [], {}, {}
    for page in sorted(sizes):
        if o["pages"] and page not in {int(x) for x in o["pages"].split(",")}:
            continue
        if not os.path.exists(os.path.join(o["calib"], f"calib_{page}.npz")):
            continue
        total, sun, cov, texel = calib_page(o["calib"], page, scales)
        share = np.where(cov, sun / np.maximum(total, 1e-6), 0.0)
        if cov.any() and np.percentile(share[cov], 99.5) > o["share"] and scales["sun"] > 1e-6:
            keep.append(page)
            texels[page] = texel
            cal[page] = (total, cov)
    # (a light wider than --widest is the sky's dome, not a sun (c10's, a50's: 20 and 50 degrees): its shadows are
    # as soft as CE baked them, and a finer bake of them would only move the pages' mood)
    if float(light["diameter"]) > o["widest"]:
        print(f"sunmask: the sky's light is {math.degrees(light['diameter']):.0f} degrees wide: no sun to sharpen",
              flush=True)
        keep = []
    factors = {}
    for page in keep:
        f = 2 ** math.ceil(math.log2(max(texels[page] / o["target-wu"], 1.0)))
        factors[page] = int(min(max(f, o["min-factor"]), o["max-factor"]))
    print(f"sunmask: sky {sky_index} light {light_index} toward {direction.round(4).tolist()} power {main_power}, "
          f"diameter {light['diameter']:.4f}; {len(keep)} of {len(sizes)} pages the sun reaches; factors "
          f"{ {p: factors[p] for p in keep} }", flush=True)
    os.makedirs(o["out"], exist_ok=True)
    meta = {"sun": direction.tolist(), "sky": sky_index, "light": light_index, "power": main_power,
            "color": light["color"], "diameter": light["diameter"], "penumbra": max(o["penumbra"], light["diameter"]),
            "gamma": gamma, "scales": {"sun": scales["sun"], "sky": scales["sky"]}, "pages": {}, "samples": o["samples"],
            "gltf": o["gltf"], "pwg": o["pwg"], "bsp": o["bsp"]}
    if not keep:
        json.dump(meta, open(os.path.join(o["out"], "sun_meta.json"), "w"), indent=1)
        print("sunmask: no page the sun reaches", flush=True)
        return
    # CE's geometry: the light and visibility the pages hold
    RB["set_emit_mode"]({m.name: m for m in bpy.data.materials if m.node_tree and "emission" in m.node_tree.nodes}, None)
    for m in bpy.data.materials:
        if m.node_tree and "emit_strength" in m.node_tree.nodes:
            m.node_tree.nodes["emit_strength"].outputs[0].default_value = 0.0
    RB["set_normal"]({m.name: m for m in bpy.data.materials if m.node_tree and "cos" in m.node_tree.nodes}, 1.0, 0.0, 0.0)
    ce_page_materials(objects)
    for page in objects:
        for slot in objects[page].material_slots:
            nt = slot.material.node_tree
            if "bake" not in nt.nodes:
                img = nt.nodes.new("ShaderNodeTexImage")
                img.name = "bake"
    main_ld.angle = max(float(light["diameter"]), 0.0005)
    set_shadow(main_ld, False)
    du = bake_pages(objects, keep, sizes, lambda p: 1, "DIFFUSE", "du")
    set_shadow(main_ld, True)
    vce = bake_pages(objects, keep, sizes, lambda p: 1, "SHADOW", "vce", prefill=-1.0)
    vrb = {}
    for batch in batches(keep, sizes, lambda p: 4, o["batch-texels"]):
        vrb.update(bake_pages(objects, batch, sizes, lambda p: 4, "SHADOW", "vrb", prefill=-1.0))
    # the remodel's: the visibility finely, through the ray tracing's disc
    pwg = read_pwg(o["pwg"]) if o["pwg"] else None
    objects = build_remodel(prims, base, pwg)
    main_ld.angle = max(o["penumbra"], float(light["diameter"]))
    scene.cycles.samples = o["samples"]
    for batch in batches(keep, sizes, lambda p: factors[p], o["batch-texels"]):
        vhi = bake_pages(objects, batch, sizes, lambda p: factors[p], "SHADOW", "vhi", prefill=-1.0)
        for page in batch:
            hi = vhi[page]
            # (the fine bake as bytes: its visibility, and where a triangle covers it)
            np.savez(os.path.join(o["out"], f"sun_{page}.npz"),
                     vhi8=np.clip(np.round(hi[..., 0] * 255), 0, 255).astype(np.uint8), vhi_cov=hi[..., 3] > 0,
                     vce=vce[page], du=du[page], vrb=vrb[page].astype(np.float16), total=cal[page][0],
                     cov=cal[page][1])
            meta["pages"][str(page)] = {"factor": factors[page], "size": list(sizes[page]), "texel_wu": texels[page]}
            print(f"sunmask: page {page} written", flush=True)
        json.dump(meta, open(os.path.join(o["out"], "sun_meta.json"), "w"), indent=1)
    print("sunmask: done", flush=True)


def batches(pages, sizes, factor_of, budget):
    out, cur, used = [], [], 0
    for page in pages:
        n = sizes[page][0] * sizes[page][1] * factor_of(page) ** 2
        if cur and used + n > budget:
            out.append(cur)
            cur, used = [], 0
        cur.append(page)
        used += n
    if cur:
        out.append(cur)
    return out


main()
