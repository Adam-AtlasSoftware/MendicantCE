"""PRISM lightmap re-bake, stage 1 (headless Blender, Cycles on the GPU).

Bakes one structure BSP's lightmap pages again from CE's own light sources, on this
machine, from the player's own map (nothing made of the game's data leaves the machine):

- geometry: the BSP's render mesh as map_extract writes it (glTF: game coordinates, Z up,
  CE's vertex normals, tangents, base-map and lightmap coordinates), read straight from
  the accessors (no importer), one object per lightmap page, CE's normals as custom normals;
- lights: the sky's lights (relight_dump: colour x power, direction from yaw and pitch,
  angular diameter as the sun lamp's angle), the sky's outdoor ambient radiosity as the
  world's colour, and every shader with radiosity power as an emitter (colour x power);
  transparent shaders (sotr, sgla, swat) pass shadow rays and add their emission;
- albedo for the bounces: the mean of each shader's base map.

Passes (all with no margin: coverage is found from the position pass, gutters are
filled later by relight_encode.py):
  calib  at 1x the page size, flat normal, one light group at a time (sun, sky, emit),
         the texel's world position, CE's flat-bump factor and whether the texel is an
         emitting shader's own (the fit leaves the lamps themselves out): relight_encode.py fits the groups' scales and
         the per-area colour field against CE's own page;
  final  at the factor times the page size, all lights scaled by the fit (read from
         --scales), the shading normal tilted 50 degrees from CE's normal along four
         tangent-space directions (0: the normal itself, 1-3: a tripod at 0/120/240
         degrees about it), and the position, from which relight_encode.py fits
         Y(n) = a + b.n per texel (the dominant direction and how directional it is).

usage: blender -b --factory-startup --python relight_bake.py -- \
         --gltf <bsp.gltf> --lights <map_lights.json> --bsp <index> --out <dir> \
         --stage calib|final [--factor 4] [--samples 128] [--gpu <part of a card's name>] [--scales scales.json]
Output: <out>/<stage>_<page>.npz (float32 arrays, rows bottom-up as Blender keeps them).
"""
import json
import math
import os
import sys
import time

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

TILT = math.radians(50.0)
# the tripod about the normal, in the tangent plane (x along T, y along B)
TRIPOD = [(1.0, 0.0), (-0.5, math.sqrt(3) / 2), (-0.5, -math.sqrt(3) / 2)]


def args():
    a = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    o = {"factor": 4, "samples": 128, "gpu": E.GPU, "scales": None, "bounces": 4, "pages": None}
    i = 0
    while i < len(a):
        k = a[i][2:]
        o[k] = a[i + 1]
        i += 2
    o["factor"] = int(o["factor"])
    o["samples"] = int(o["samples"])
    o["bounces"] = int(o["bounces"])
    o["bsp"] = int(o["bsp"])
    return o


# ---------- the glTF, read from its accessors

COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DTYPES = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}


def read_gltf(path):
    g = json.load(open(path))
    base = os.path.dirname(path)
    blob = open(os.path.join(base, g["buffers"][0]["uri"]), "rb").read()

    def accessor(i):
        a = g["accessors"][i]
        v = g["bufferViews"][a["bufferView"]]
        off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = COMPONENTS[a["type"]]
        arr = np.frombuffer(blob, dtype=DTYPES[a["componentType"]], count=a["count"] * n, offset=off)
        return arr.reshape(a["count"], n) if n > 1 else arr

    prims = []
    for p in g["meshes"][0]["primitives"]:
        at = p["attributes"]
        mat = g["materials"][p["material"]]
        ex = mat.get("extras", {})
        prims.append({
            "pos": accessor(at["POSITION"]).astype(np.float64),
            "nrm": accessor(at["NORMAL"]).astype(np.float64),
            "tan": accessor(at["TANGENT"]).astype(np.float64),
            "uv1": accessor(at["TEXCOORD_1"]).astype(np.float64) if "TEXCOORD_1" in at else None,
            "rad": accessor(at["_INCIDENT_RADIOSITY"]).astype(np.float64) if "_INCIDENT_RADIOSITY" in at else None,
            "idx": accessor(p["indices"]).astype(np.int64).reshape(-1, 3),
            "shader": ex.get("shader", mat.get("name", "")),
            "group": ex.get("shader_group", ""),
            "page": ex.get("lightmap_bitmap_index", -1),
            "lightmap_index": ex.get("lightmap_index", -1),
            "material_index": ex.get("material_index", -1),
            "base_png": (ex.get("maps", {}).get("base_map", {}) or {}).get("png"),
            "lightmap_png": ex.get("lightmap_png"),
        })
    return g, prims, base


# ---------- scene

def setup_cycles(o):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    kind, cards = E.cycles_gpu(prefs, o["gpu"])
    chosen = (kind, cards) if kind else None
    for d in prefs.devices:
        if d.type == "CPU":
            d.use = False
    scene.cycles.device = "GPU" if chosen else "CPU"
    print("relight: device", chosen or "CPU", flush=True)
    scene.cycles.samples = o["samples"]
    scene.cycles.use_denoising = False
    scene.cycles.max_bounces = o["bounces"] + 2
    scene.cycles.diffuse_bounces = o["bounces"]
    scene.cycles.glossy_bounces = 0
    scene.cycles.transmission_bounces = 2
    scene.cycles.transparent_max_bounces = 8
    scene.cycles.caustics_reflective = False
    scene.cycles.caustics_refractive = False
    scene.cycles.sample_clamp_indirect = float(o.get("clamp", 1.0))
    scene.render.bake.margin = 0
    scene.render.bake.use_clear = True
    return scene


def base_albedo(base, png, cache):
    if not png:
        return (0.35, 0.35, 0.35)
    if png in cache:
        return cache[png]
    path = os.path.join(base, "..", "..", "..", png) if not os.path.exists(os.path.join(base, png)) else os.path.join(base, png)
    col = (0.35, 0.35, 0.35)
    try:
        im = bpy.data.images.load(os.path.abspath(path))
        px = np.empty(im.size[0] * im.size[1] * 4, dtype=np.float32)
        im.pixels.foreach_get(px)
        px = px.reshape(-1, 4)
        # (Blender gives linear values for an sRGB image)
        col = tuple(float(np.clip(px[:, c].mean(), 0.03, 0.85)) for c in range(3))
        bpy.data.images.remove(im)
    except Exception as e:
        print("relight: base map not read:", path, e, flush=True)
    cache[png] = col
    return col


def make_material(name, albedo, emit_rgb, transparent):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    diff = nt.nodes.new("ShaderNodeBsdfDiffuse")
    diff.inputs["Color"].default_value = (*albedo, 1.0)
    diff.name = "diffuse"
    emi = nt.nodes.new("ShaderNodeEmission")
    emi.name = "emission"
    emi.inputs["Color"].default_value = (*emit_rgb, 1.0)
    emi.inputs["Strength"].default_value = 0.0
    # one-sided emission, toward CE's normal (a light panel on a ceiling lights the room,
    # not the roof above it): strength x (CE normal . incoming > 0)
    es = nt.nodes.new("ShaderNodeValue")
    es.name = "emit_strength"
    es.outputs[0].default_value = 0.0
    an = nt.nodes.new("ShaderNodeAttribute")
    an.attribute_name = "ce_normal"
    gin = nt.nodes.new("ShaderNodeNewGeometry")
    dp = nt.nodes.new("ShaderNodeVectorMath")
    dp.operation = "DOT_PRODUCT"
    nt.links.new(an.outputs["Vector"], dp.inputs[0])
    nt.links.new(gin.outputs["Incoming"], dp.inputs[1])
    gt = nt.nodes.new("ShaderNodeMath")
    gt.operation = "GREATER_THAN"
    nt.links.new(dp.outputs["Value"], gt.inputs[0])
    gt.inputs[1].default_value = 0.0
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    nt.links.new(gt.outputs[0], mul.inputs[0])
    nt.links.new(es.outputs[0], mul.inputs[1])
    nt.links.new(mul.outputs[0], emi.inputs["Strength"])
    add = nt.nodes.new("ShaderNodeAddShader")
    add.name = "add"
    nt.links.new(diff.outputs[0], add.inputs[0])
    nt.links.new(emi.outputs[0], add.inputs[1])
    if transparent:
        # (a transparent shader's surface: diffuse to the bake's own ray, transparent with
        # its emission to every other ray)
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        add2 = nt.nodes.new("ShaderNodeAddShader")
        nt.links.new(tr.outputs[0], add2.inputs[0])
        nt.links.new(emi.outputs[0], add2.inputs[1])
        lp = nt.nodes.new("ShaderNodeLightPath")
        mix = nt.nodes.new("ShaderNodeMixShader")
        nt.links.new(lp.outputs["Is Camera Ray"], mix.inputs[0])
        nt.links.new(add2.outputs[0], mix.inputs[1])
        nt.links.new(diff.outputs[0], mix.inputs[2])
        nt.links.new(mix.outputs[0], out.inputs[0])
    else:
        nt.links.new(add.outputs[0], out.inputs[0])
    out.name = "out"
    # the shading normal: CE's normal tilted toward (tx, ty) in its tangent frame
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    at = nt.nodes.new("ShaderNodeAttribute")
    at.attribute_name = "ce_tangent"
    ab = nt.nodes.new("ShaderNodeAttribute")
    ab.attribute_name = "ce_binormal"
    vals = []
    for nm in ("cos", "tx", "ty"):
        v = nt.nodes.new("ShaderNodeValue")
        v.name = nm
        v.outputs[0].default_value = 1.0 if nm == "cos" else 0.0
        vals.append(v)

    def vm(op, a, b=None, scale_socket=None):
        n = nt.nodes.new("ShaderNodeVectorMath")
        n.operation = op
        nt.links.new(a, n.inputs[0])
        if b is not None:
            nt.links.new(b, n.inputs[1])
        if scale_socket is not None:
            nt.links.new(scale_socket, n.inputs["Scale"])
        return n.outputs["Vector"]

    N = geo.outputs["Normal"]
    a = vm("SCALE", N, scale_socket=vals[0].outputs[0])
    b = vm("SCALE", at.outputs["Vector"], scale_socket=vals[1].outputs[0])
    c = vm("SCALE", ab.outputs["Vector"], scale_socket=vals[2].outputs[0])
    s = vm("ADD", vm("ADD", a, b), c)
    nn = vm("NORMALIZE", s)
    nt.links.new(nn, diff.inputs["Normal"])
    # the bake's target
    img = nt.nodes.new("ShaderNodeTexImage")
    img.name = "bake"
    nt.nodes.active = img
    return m


def build_scene(o, prims, base, lights):
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    bsp = lights["bsps"][o["bsp"]]
    mats_info = {}
    for mi in bsp["materials"]:
        mats_info[(mi["lightmap_index"], mi["material_index"])] = mi
    albedo_cache = {}
    materials = {}
    emitters = []
    page_prims = {}
    for p in prims:
        page_prims.setdefault(p["page"], []).append(p)
    objects = {}
    for page, plist in sorted(page_prims.items()):
        verts, faces, nrm, tan, bin_, uv, slots, face_slot, flat = [], [], [], [], [], [], [], [], []
        offset = 0
        for p in plist:
            key = p["shader"]
            info = mats_info.get((p["lightmap_index"], p["material_index"]), {})
            rad = info.get("radiosity", {})
            power = rad.get("power", 0.0) or 0.0
            emit = [c * power for c in rad.get("color", [0, 0, 0])] if power > 0 else [0, 0, 0]
            transparent = p["group"] != "senv"
            mkey = (key, transparent, tuple(emit))
            if mkey not in materials:
                m = make_material(key.split("\\")[-1][:50], base_albedo(base, p["base_png"], albedo_cache), emit, transparent)
                m["shader"] = key
                m["emits"] = bool(power > 0)
                m["emit_base"] = [float(x) for x in emit]
                materials[mkey] = m
                if power > 0:
                    emitters.append(m)
            m = materials[mkey]
            if m.name not in slots:
                slots.append(m.name)
            si = slots.index(m.name)
            n = len(p["pos"])
            verts.append(p["pos"])
            faces.append(p["idx"] + offset)
            nrm.append(p["nrm"])
            t = p["tan"][:, :3]
            w = p["tan"][:, 3:4]
            b = np.cross(p["nrm"], t) * w
            tan.append(t)
            bin_.append(b)
            uv.append(p["uv1"] if p["uv1"] is not None else np.zeros((n, 2)))
            # CE's lightmap pass on a flat bump: lightmap x mix(1, max(0, R^.N), |R|), R the
            # vertex's incident radiosity (its direction in the cube, its length in D0's alpha)
            if p["rad"] is not None:
                k = np.clip(np.linalg.norm(p["rad"], axis=1), 0.0, 1.0)
                cosr = (p["rad"] * p["nrm"]).sum(1) / np.maximum(np.linalg.norm(p["rad"], axis=1), 1e-9)
                flat.append(1.0 - k + k * np.maximum(cosr, 0.0))
            else:
                flat.append(np.ones(n))
            face_slot.append(np.full(len(p["idx"]), si))
            offset += n
        V = np.concatenate(verts)
        F = np.concatenate(faces)
        me = bpy.data.meshes.new(f"page_{page}")
        me.vertices.add(len(V))
        me.vertices.foreach_set("co", V.astype(np.float32).ravel())
        me.loops.add(F.size)
        me.loops.foreach_set("vertex_index", F.astype(np.int32).ravel())
        me.polygons.add(len(F))
        me.polygons.foreach_set("loop_start", (np.arange(len(F)) * 3).astype(np.int32))
        me.polygons.foreach_set("loop_total", np.full(len(F), 3, dtype=np.int32))
        me.polygons.foreach_set("material_index", np.concatenate(face_slot).astype(np.int32))
        me.polygons.foreach_set("use_smooth", np.ones(len(F), dtype=bool))
        me.update(calc_edges=True)
        me.validate(clean_customdata=False)
        for nm in slots:
            me.materials.append(bpy.data.materials[nm])
        loopv = F.ravel()
        U = np.concatenate(uv)
        uvl = me.uv_layers.new(name="lm")
        # (D3D's v runs down the page; Blender's up)
        uvl.data.foreach_set("uv", np.stack([U[loopv, 0], 1.0 - U[loopv, 1]], 1).astype(np.float32).ravel())
        for name, arr in (("ce_tangent", np.concatenate(tan)), ("ce_binormal", np.concatenate(bin_))):
            a = me.attributes.new(name, "FLOAT_VECTOR", "POINT")
            a.data.foreach_set("vector", arr.astype(np.float32).ravel())
        a = me.attributes.new("ce_normal", "FLOAT_VECTOR", "POINT")
        nn_ = np.concatenate(nrm)
        a.data.foreach_set("vector", (nn_ / np.maximum(np.linalg.norm(nn_, axis=1, keepdims=True), 1e-9)).astype(np.float32).ravel())
        a = me.attributes.new("ce_flat", "FLOAT", "POINT")
        a.data.foreach_set("value", np.concatenate(flat).astype(np.float32))
        N = np.concatenate(nrm)
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
        me.normals_split_custom_set_from_vertices(N.astype(np.float32).tolist())
        ob = bpy.data.objects.new(f"page_{page}", me)
        bpy.context.scene.collection.objects.link(ob)
        objects[page] = ob
    # the sky: the one most of the bsp's clusters see (else the first)
    skies = lights["skies"]
    counts = {}
    for c in bsp["clusters"]:
        if c["sky"] >= 0:
            counts[c["sky"]] = counts.get(c["sky"], 0) + c["surfaces"]
    sky_index = max(counts, key=counts.get) if counts else 0
    sky = skies[sky_index] if sky_index < len(skies) and skies[sky_index].get("tag") else None
    suns = []
    if sky:
        for i, l in enumerate(sky["lights"]):
            if l["power"] <= 0:
                continue
            ld = bpy.data.lights.new(f"sky_light_{i}", "SUN")
            ld.color = tuple(l["color"])
            ld.angle = max(float(l["diameter"]), 0.0005)
            ld.cycles.cast_shadow = True
            lo = bpy.data.objects.new(f"sky_light_{i}", ld)
            bpy.context.scene.collection.objects.link(lo)
            d = np.array([math.cos(l["pitch"]) * math.cos(l["yaw"]), math.cos(l["pitch"]) * math.sin(l["yaw"]),
                          math.sin(l["pitch"])])
            # (a sun lamp shines along its -Z: point it from the light toward the level)
            z = d / np.linalg.norm(d)
            up = np.array([0, 0, 1.0]) if abs(z[2]) < 0.99 else np.array([1.0, 0, 0])
            x = np.cross(up, z)
            x /= np.linalg.norm(x)
            y = np.cross(z, x)
            import mathutils
            lo.matrix_world = mathutils.Matrix(((x[0], y[0], z[0], 0), (x[1], y[1], z[1], 0), (x[2], y[2], z[2], 0), (0, 0, 0, 1)))
            ld["base_color"] = [float(x) for x in l["color"]]
            suns.append((ld, float(l["power"])))
    world = bpy.data.worlds.new("relight_world")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    amb = [c * sky["outdoor_power"] for c in sky["outdoor_ambient"]] if sky else [0, 0, 0]
    bg.inputs["Color"].default_value = (*amb, 1.0)
    world["base_color"] = [float(x) for x in amb]
    bpy.context.scene.world = world
    print(f"relight: {len(objects)} page objects, {len(materials)} materials, {len(emitters)} emitters, "
          f"sky {sky_index} ({sky['tag'] if sky else None}), {len(suns)} sky lights, ambient {amb}", flush=True)
    return objects, materials, emitters, suns, bg, sky_index


def set_lights(materials, suns, bg, sun_scale, sky_scale, emit_scale, rgb=None):
    """emit_scale: one scale for every emitter, or {shader: scale} (others 0); rgb (the
    fit's colour gains, relight_calib.py): {"sun": [r, g, b], "sky": [...], "emit": {shader:
    [...]}}, each group's colour times its gain (CE's own colour, matched to the hue of
    the pools CE's page shows)"""
    rgb = rgb or {}
    for ld, power in suns:
        ld.energy = power * sun_scale
        g = rgb.get("sun", [1, 1, 1])
        ld.color = tuple(c * k for c, k in zip(ld["base_color"], g))
    bg.inputs["Strength"].default_value = sky_scale
    g = rgb.get("sky", [1, 1, 1])
    w = bpy.context.scene.world
    bg.inputs["Color"].default_value = (*[c * k for c, k in zip(w["base_color"], g)], 1.0)
    for m in materials.values():
        sc = emit_scale.get(m.get("shader", ""), 0.0) if isinstance(emit_scale, dict) else emit_scale
        m.node_tree.nodes["emit_strength"].outputs[0].default_value = sc
        if "emit_base" in m:
            g = rgb.get("emit", {}).get(m.get("shader", ""), [1, 1, 1])
            m.node_tree.nodes["emission"].inputs["Color"].default_value = (
                *[c * k for c, k in zip(m["emit_base"], g)], 1.0)


def set_normal(materials, cos, tx, ty):
    for m in materials.values():
        nodes = m.node_tree.nodes
        nodes["cos"].outputs[0].default_value = cos
        nodes["tx"].outputs[0].default_value = tx
        nodes["ty"].outputs[0].default_value = ty


def set_emit_mode(materials, source):
    """the material's output: CE's position ("position") or its flat-bump factor ("flat")
    as emission (the EMIT passes), or its light (None)"""
    for m in materials.values():
        nt = m.node_tree
        out = nt.nodes["out"]
        if "saved_link" not in m:
            m["saved_link"] = out.inputs[0].links[0].from_node.name
        if source is None:
            nt.links.new(nt.nodes[m["saved_link"]].outputs[0], out.inputs[0])
            continue
        if "posemit" not in nt.nodes:
            geo = nt.nodes.new("ShaderNodeNewGeometry")
            geo.name = "posgeo"
            fl = nt.nodes.new("ShaderNodeAttribute")
            fl.attribute_name = "ce_flat"
            fl.name = "flatattr"
            e = nt.nodes.new("ShaderNodeEmission")
            e.name = "posemit"
            e.inputs["Strength"].default_value = 1.0
        e = nt.nodes["posemit"]
        if source == "position":
            nt.links.new(nt.nodes["posgeo"].outputs["Position"], e.inputs["Color"])
        elif source == "own":
            # (1 on an emitting shader's own surface, else 0: the fit leaves the lamps' own
            # texels out, the light they cast is what it matches)
            for l in list(e.inputs["Color"].links):
                nt.links.remove(l)
            v = 1.0 if m.get("emits") else 0.0
            e.inputs["Color"].default_value = (v, v, v, 1.0)
        else:
            nt.links.new(nt.nodes["flatattr"].outputs["Fac"], e.inputs["Color"])
        nt.links.new(e.outputs[0], out.inputs[0])


def page_sizes(prims, base):
    sizes = {}
    for p in prims:
        if p["page"] < 0 or p["page"] in sizes or not p["lightmap_png"]:
            continue
        path = os.path.abspath(os.path.join(base, "..", "..", "..", p["lightmap_png"]))
        im = bpy.data.images.load(path)
        sizes[p["page"]] = tuple(im.size)
        bpy.data.images.remove(im)
    return sizes


def assign_images(objects, materials, sizes, factor):
    images = {}
    for page, ob in objects.items():
        if page not in sizes:
            continue
        w, h = sizes[page]
        img = bpy.data.images.new(f"bake_{page}", w * factor, h * factor, alpha=True, float_buffer=True)
        img.colorspace_settings.name = "Non-Color"
        images[page] = img
        for slot in ob.material_slots:
            pass
    # (a material may be on several pages: each page's object gets its own copies)
    for page, ob in objects.items():
        if page not in images:
            continue
        for i, slot in enumerate(ob.material_slots):
            m = slot.material
            key = f"{m.name}@{page}"
            mc = bpy.data.materials.get(key)
            if mc is None:
                mc = m.copy()
                mc.name = key
                mc["origin"] = m.name
            mc.node_tree.nodes["bake"].image = images[page]
            mc.node_tree.nodes.active = mc.node_tree.nodes["bake"]
            ob.material_slots[i].material = mc
    return images


def bake(objects, images, kind, label):
    bpy.ops.object.select_all(action="DESELECT")
    sel = [ob for page, ob in objects.items() if page in images]
    for ob in sel:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = sel[0]
    t = time.time()
    if kind == "EMIT":
        bpy.ops.object.bake(type="EMIT", margin=0, use_clear=True)
    else:
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=0, use_clear=True)
    print(f"relight: bake {label} {time.time() - t:.1f} s", flush=True)
    out = {}
    for page, img in images.items():
        w, h = img.size
        px = np.empty(w * h * 4, dtype=np.float32)
        img.pixels.foreach_get(px)
        out[page] = px.reshape(h, w, 4)
    return out


def main():
    o = args()
    lights = json.load(open(o["lights"]))
    g, prims, base = read_gltf(o["gltf"])
    scene = setup_cycles(o)
    objects, materials, emitters, suns, bg, sky_index = build_scene(o, prims, base, lights)
    sizes = page_sizes(prims, base)
    if o["pages"]:
        keep = set(int(x) for x in o["pages"].split(","))
        sizes = {k: v for k, v in sizes.items() if k in keep}
    os.makedirs(o["out"], exist_ok=True)
    factor = 1 if o["stage"] == "calib" else o["factor"]
    images = assign_images(objects, materials, sizes, factor)
    # (every material: the page copies, and the originals that pages without an image keep,
    # such as the emitting transparent shaders')
    allm = {m.name: m for m in bpy.data.materials if m.node_tree and "emission" in m.node_tree.nodes}
    results = {}
    set_emit_mode(allm, "position")
    results["position"] = bake(objects, images, "EMIT", "position")
    if o["stage"] == "calib":
        set_emit_mode(allm, "flat")
        results["flat"] = bake(objects, images, "EMIT", "flat")
        set_emit_mode(allm, "own")
        results["own"] = bake(objects, images, "EMIT", "own")
    set_emit_mode(allm, None)
    if o["stage"] == "calib":
        set_normal(allm, 1.0, 0.0, 0.0)
        emitting = sorted({m["shader"] for m in allm.values() if m.get("emits")})
        groups = [("sun", (1, 0, {})), ("sky", (0, 1, {}))]
        groups += [(f"emit{i}", (0, 0, {sh: 1.0})) for i, sh in enumerate(emitting)]
        for label, sc in groups:
            set_lights(allm, suns, bg, *sc)
            results[label] = bake(objects, images, "DIFFUSE", label)
        meta_groups = {"sun": "sky lights", "sky": "sky ambient"}
        meta_groups.update({f"emit{i}": sh for i, sh in enumerate(emitting)})
    else:
        scales = json.load(open(o["scales"])) if o["scales"] else {"sun": 1, "sky": 1, "emit": {}}
        set_lights(allm, suns, bg, scales["sun"], scales["sky"], scales["emit"], scales.get("rgb"))
        dirs = [(1.0, 0.0, 0.0)] + [(math.cos(TILT), math.sin(TILT) * tx, math.sin(TILT) * ty) for tx, ty in TRIPOD]
        for i, d in enumerate(dirs):
            set_normal(allm, *d)
            results[f"n{i}"] = bake(objects, images, "DIFFUSE", f"n{i}")
    meta = {"groups": meta_groups if o["stage"] == "calib" else None, "stage": o["stage"], "factor": factor, "sky": sky_index, "tilt": TILT, "tripod": TRIPOD,
            "samples": o["samples"], "sizes": {str(k): v for k, v in sizes.items()}}
    for page in images:
        np.savez_compressed(os.path.join(o["out"], f"{o['stage']}_{page}.npz"),
                            **{k: v[page] for k, v in results.items()})
    json.dump(meta, open(os.path.join(o["out"], f"{o['stage']}_meta.json"), "w"), indent=1)
    print("relight: done", flush=True)


main()
