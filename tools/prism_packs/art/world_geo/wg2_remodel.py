"""PRISM world geometry v2: a structure BSP remodelled with real new geometry, in headless Blender, written as the
.pwg the engine draws in the BSP's place (display.prism_world_geometry = "remodel";
port/linux/game/prism_world_geometry.c). Collision, pathing and visibility keep the map's BSP.

usage: blender -b --factory-startup --python art/world_geo/wg2_remodel.py -- <bsp.gltf> <extract root> <recipes.json>
       <out.pwg> <report.json>
environment: WG_VARIANT (recipe key suffix), WG_GPU ("OPTIX" or "NONE"), WG_GPU_NAME (substring of the device),
       WG_DUMP (a .blend written for inspection)

What it does, after v1 (wg_remodel.py):
  1. The BSP's opaque materials become one welded mesh, every face tagged with its material and stock surface.
  2. Topology (bmesh): deep bevels on the sharp edges CE's metal mesh already has; for metal, grooves cut along the
     panel lines of the material's own texture (iso-u and iso-v lines found in its luminance or bump profile); then
     adaptive subdivision of the displaced classes to a target edge length (crack-free: edges are split, so both
     faces of an edge always agree), within a triangle budget.
  3. Shape (numpy, wg2_shape.py, a function of position only): rock gets strata (layers with their own set-back,
     notches whose lips make overhang undersides and ledges), chipped facets and the CE texture's painted relief at
     two scales; ground gets wind ripples, swells from the texture and small lumps; metal grooves are set in.
     Mostly into the rock, behind the collision surface; steep faces take much more than faces one stands on.
  4. Light that answers the new shape: Cycles bakes the remodel and the stock surface it stands for under the same
     sun and sky (the other geometry of the level casting shadows in both), and their ratio, per vertex, goes into
     each corner's incident radiosity (its direction and length), which CE's lightmap pass already reads:
     lightmap x lerp(1, dot(bump, L), |L|). Crevices, overhang undersides and faces turned from the light darken;
     CE's lightmap and colours are kept.
  5. Corners take base map and lightmap coordinates and tangent frames from their stock surface (barycentric).

Every output is derived from the player's map: it stays local.
"""
import json
import math
import os
import sys
import time

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.kdtree import KDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import packs_env as E  # noqa: E402
import wg_common as wg  # noqa: E402
import wg2_shape as shp  # noqa: E402
import wg2_lod  # noqa: E402

T0 = time.time()
# the classes whose surfaces are subdivided and moved (metal's are cut and bevelled in place); "smooth": rounded by
# Phong over CE's own normals alone (pipes, conduits)
DISPLACED = ("rock", "ground", "smooth")


def log(*a):
    print("WG2 %7.1fs" % (time.time() - T0), *a, flush=True)


def args():
    a = sys.argv[sys.argv.index("--") + 1:]
    return a[0], a[1], a[2], a[3], a[4]


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00 = (v0 * v0).sum(1)
    d01 = (v0 * v1).sum(1)
    d11 = (v1 * v1).sum(1)
    d20 = (v2 * v0).sum(1)
    d21 = (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    bc = np.stack([1.0 - v - w, v, w], 1)
    bc = np.clip(bc, 0.0, 1.0)
    return bc / np.maximum(bc.sum(1, keepdims=True), 1e-12)


# ---------- textures

_IMAGES = {}


def image_lum(png):
    if png not in _IMAGES:
        img = bpy.data.images.load(png, check_existing=False)
        w, h = img.size
        px = np.empty(w * h * 4, np.float32)
        img.pixels.foreach_get(px)
        bpy.data.images.remove(img)
        px = px.reshape(h, w, 4).astype(np.float64)
        _IMAGES[png] = px
    return _IMAGES[png]


def box_blur(a, r):
    if r < 1:
        return a.copy()
    k = 2 * r + 1
    pad = np.pad(a, r, mode="wrap")
    c = pad.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def relief_bands(png, fine_px, coarse_px):
    """the base map's luminance as two height bands in [-1, 1]: fine (minus a blur of fine_px) and coarse (a blur of
    fine_px minus a blur of coarse_px): CE's own painted shading of cracks, ribs and swells"""
    px = image_lum(png)
    lum = 0.2126 * px[..., 0] + 0.7152 * px[..., 1] + 0.0722 * px[..., 2]
    b1 = box_blur(lum, int(fine_px))
    b2 = box_blur(b1, int(coarse_px))
    fine = lum - b1
    coarse = b1 - b2
    fine = np.clip(fine / max(fine.std() * 2.5, 1e-6), -1, 1)
    coarse = np.clip(coarse / max(coarse.std() * 2.0, 1e-6), -1, 1)
    return fine, coarse


def sample_wrap(field, uv):
    h, w = field.shape[:2]
    x = uv[:, 0] * w - 0.5
    y = uv[:, 1] * h - 0.5  # (map_extract's texture coordinates are the game's: v down the image, row 0 at v 0)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    x0 %= w
    y0 %= h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    if field.ndim == 3:
        fx, fy = fx[:, None], fy[:, None]
    return ((field[y0, x0] * (1 - fx) + field[y0, x1] * fx) * (1 - fy) +
            (field[y1, x0] * (1 - fx) + field[y1, x1] * fx) * fy)


def panel_lines(png, r, bump_png=None):
    """Panel lines of a metal texture, as positions in u and in v (texture space, 0..1): the columns and rows where the
    texture's own profile has its strongest edges (the bump map's height edges when it has one, else the luminance's).
    Returns (us, vs, height field in [-1, 1] for the grooves' depth: low where the texture is dark)."""
    px = image_lum(png)
    lum = 0.2126 * px[..., 0] + 0.7152 * px[..., 1] + 0.0722 * px[..., 2]
    h, w = lum.shape
    src = lum
    if bump_png and os.path.exists(bump_png):
        bp = image_lum(bump_png)
        # the bump map's slopes (x, y in [-1, 1])
        gx = bp[..., 0] * 2 - 1
        gy = bp[..., 1] * 2 - 1
        if bp.shape[:2] == lum.shape:
            edges_u = np.abs(gx).mean(0)
            edges_v = np.abs(gy).mean(1)
        else:
            edges_u = np.abs(gx).mean(0)
            edges_v = np.abs(gy).mean(1)
            edges_u = np.interp(np.linspace(0, 1, w), np.linspace(0, 1, len(edges_u)), edges_u)
            edges_v = np.interp(np.linspace(0, 1, h), np.linspace(0, 1, len(edges_v)), edges_v)
    else:
        edges_u = np.abs(np.diff(src, axis=1, append=src[:, :1])).mean(0)
        edges_v = np.abs(np.diff(src, axis=0, append=src[:1, :])).mean(1)
    darkness = 1.0 - (lum - box_blur(lum, 6)) / max(lum.std() * 2.5, 1e-6)

    def peaks(prof, count, min_sep):
        prof = prof - np.convolve(np.pad(prof, 8, mode="wrap"), np.ones(17) / 17, mode="valid")
        out = []
        order = np.argsort(-prof)
        for i in order:
            if prof[i] <= 0 or len(out) >= count:
                break
            if all(min(abs(i - j), len(prof) - abs(i - j)) >= min_sep for j in out):
                out.append(int(i))
        return sorted(out)

    us = [(i + 0.5) / w for i in peaks(edges_u, r.get("lines_u", 4), max(4, w // 24))]
    vs = [(i + 0.5) / h for i in peaks(edges_v, r.get("lines_v", 4), max(4, h // 24))]
    return us, vs, np.clip(darkness, -1, 3)


# ---------- the level's sun

def estimate_sun(mats, use, extract_root, gltf_json):
    """the dominant light of the level: the incident radiosity direction where CE's lightmap is brightest"""
    dirs, wts = [], []
    for i in use:
        m = mats[i]
        lm_png = gltf_json["materials"][i]["extras"].get("lightmap_png")
        if not lm_png or not m.has_lightmap:
            continue
        path = os.path.join(extract_root, lm_png)
        if not os.path.exists(path):
            continue
        px = image_lum(path)
        rgb = sample_wrap(px[..., :3], m.uv1)
        lum = rgb @ np.array([0.2126, 0.7152, 0.0722])
        mag = np.linalg.norm(m.radiosity, axis=1)
        dirs.append(normalize(m.radiosity.astype(np.float64)))
        wts.append(lum * mag)
    D = np.concatenate(dirs)
    Wt = np.concatenate(wts)
    cut = np.percentile(Wt, 92)
    sel = Wt >= cut
    s = normalize((D[sel] * Wt[sel, None]).sum(0))
    return s


# ---------- Cycles: light on the remodel against light on the stock surface

def setup_cycles(samples):
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    if os.environ.get("WG_GPU", "") != "NONE":
        prefs = bpy.context.preferences.addons["cycles"].preferences
        backend, _cards = E.cycles_gpu(prefs, os.environ.get("WG_GPU_NAME"))
        sc.cycles.device = "GPU" if backend else "CPU"
        log("cycles devices", [(d.name, d.type, d.use) for d in prefs.devices])
    sc.cycles.samples = samples
    sc.cycles.use_denoising = False
    sc.cycles.max_bounces = 3
    sc.cycles.diffuse_bounces = 2
    sc.render.bake.use_pass_direct = True
    sc.render.bake.use_pass_indirect = True
    sc.render.bake.use_pass_color = False
    sc.render.bake.target = "VERTEX_COLORS"


def make_mesh_object(name, X, T, corner_normals=None):
    me = bpy.data.meshes.new(name)
    nV, nT = len(X), len(T)
    me.vertices.add(nV)
    me.vertices.foreach_set("co", X.astype(np.float32).ravel())
    me.loops.add(3 * nT)
    me.loops.foreach_set("vertex_index", T.astype(np.int32).ravel())
    me.polygons.add(nT)
    me.polygons.foreach_set("loop_start", (np.arange(nT, dtype=np.int32) * 3))
    me.update(calc_edges=True)
    if corner_normals is not None:
        me.normals_split_custom_set(corner_normals.reshape(-1, 3).astype(np.float32))
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def bake_light(target, others, mat, kind="DIFFUSE"):
    for ob in [target] + others:
        if not ob.data.materials:
            ob.data.materials.append(mat)
    me = target.data
    attr = me.color_attributes.new("light_" + kind.lower(), "FLOAT_COLOR", "CORNER")
    me.color_attributes.active_color = attr
    for ob in bpy.context.scene.objects:
        ob.select_set(False)
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    if kind == "AO":
        bpy.ops.object.bake(type="AO", target="VERTEX_COLORS")
    else:
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, target="VERTEX_COLORS")
    c = np.empty(len(me.loops) * 4, np.float32)
    attr.data.foreach_get("color", c)
    c = c.reshape(-1, 4)[:, :3].astype(np.float64)
    return c @ np.array([0.2126, 0.7152, 0.0722])


def light_ratio(X, T, cornerN, Xref, refN, occ_X, occ_T, sun, r):
    """per vertex: light on the remodel / light on the stock surface (both baked by Cycles, same sun and sky, the
    level's other geometry casting shadows in both)"""
    setup_cycles(int(r.get("samples", 48)))
    sc = bpy.context.scene
    # (the factory scene's cube, lamp and camera would cast and add light)
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    sky = r.get("sky_color", [0.55, 0.65, 0.85])
    bg.inputs[0].default_value = (sky[0], sky[1], sky[2], 1.0)
    bg.inputs[1].default_value = r.get("sky_strength", 1.0)
    sc.world = world
    ld = bpy.data.lights.new("sun", "SUN")
    ld.energy = r.get("sun_strength", 3.0)
    ld.angle = math.radians(r.get("sun_angle_deg", 2.0))
    sun_ob = bpy.data.objects.new("sun", ld)
    sc.collection.objects.link(sun_ob)
    sun_ob.rotation_euler = (-Vector(sun.tolist())).to_track_quat("-Z", "Y").to_euler()
    mat = bpy.data.materials.new("white")
    mat.use_nodes = True
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.8, 0.8, 0.8, 1.0)
    mat.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 1.0

    def oriented(XX, TT, NN):
        a, b, c = XX[TT[:, 0]], XX[TT[:, 1]], XX[TT[:, 2]]
        fn = np.cross(b - a, c - a)
        flip = (fn * NN.sum(1)).sum(1) < 0
        T2 = TT.copy()
        T2[flip] = TT[flip][:, [0, 2, 1]]
        N2 = NN.copy()
        N2[flip] = NN[flip][:, [0, 2, 1]]
        return T2, N2

    T_a, N_a = oriented(X, T, cornerN)
    T_b, N_b = oriented(Xref, T, refN)
    ob_new = make_mesh_object("remodel", X, T_a, N_a)
    ob_ref = make_mesh_object("reference", Xref, T_b, N_b)
    occ = [make_mesh_object("occluders", occ_X, occ_T)] if len(occ_T) else []
    world.light_settings.distance = r.get("ao_distance", 0.4)
    ao_on = r.get("ao_strength", 0.0) > 0
    ob_ref.hide_render = True
    t = time.time()
    lit_new = bake_light(ob_new, occ, mat)
    ao_new = bake_light(ob_new, occ, mat, "AO") if ao_on else None
    log("baked remodel", len(T), "triangles in %.1fs" % (time.time() - t))
    ob_ref.hide_render = False
    ob_new.hide_render = True
    t = time.time()
    lit_ref = bake_light(ob_ref, occ, mat)
    ao_ref = bake_light(ob_ref, occ, mat, "AO") if ao_on else None
    log("baked reference in %.1fs" % (time.time() - t))
    if os.environ.get("WG_DUMP"):
        bpy.ops.wm.save_as_mainfile(filepath=os.environ["WG_DUMP"])
    nV = len(X)

    def per_vertex(lit, TT):
        s = np.zeros(nV)
        n = np.zeros(nV)
        np.add.at(s, TT.ravel(), lit)
        np.add.at(n, TT.ravel(), 1)
        return s / np.maximum(n, 1)
    a = per_vertex(lit_new, T_a)
    b = per_vertex(lit_ref, T_b)
    e = r.get("ratio_floor", 0.04) * float(np.median(b[b > 0])) if (b > 0).any() else 1e-3
    ratio = ((a + e) / (b + e)) ** r.get("contrast", 1.0)
    if ao_on:
        # the occlusion the new shape makes, short range (ao_distance), as a ratio to the stock surface's: crevices,
        # notches and overhang undersides darken whether or not the sun reaches them
        an = per_vertex(ao_new, T_a)
        ar = per_vertex(ao_ref, T_b)
        aor = np.clip((an + 0.02) / (ar + 0.02), 0.0, 1.0)
        ratio = ratio * (1.0 - r["ao_strength"] * (1.0 - aor ** r.get("ao_power", 1.5)))
        log("ao ratio p5 %.3f p50 %.3f" % (np.percentile(aor, 5), np.percentile(aor, 50)))
    return ratio, a, b


# ---------- main

def main():
    gltf, extract_root, recipes_path, out_path, report_path = args()
    info, mats = wg.load_bsp(gltf)
    gltf_json = json.load(open(gltf))
    allrec = json.load(open(recipes_path))
    rec = allrec[info["tag"] + os.environ.get("WG_VARIANT", "")]
    recipes = rec["classes"]
    light = rec.get("light", {})

    def class_of(m):
        for name, r in recipes.items():
            if any(s.lower() in m.shader.lower() for s in r["match"]) and \
                    not any(s.lower() in m.shader.lower() for s in r.get("exclude", [])):
                return name
        return None

    use = [i for i, m in enumerate(mats) if m.shader_group == "senv"]
    cls = {i: class_of(mats[i]) for i in use}
    kind = {name: r["kind"] for name, r in recipes.items()}
    offsets, P, F, fmat, fsrc, PN = {}, [], [], [], [], []
    base = 0
    for i in use:
        m = mats[i]
        offsets[i] = base
        P.append(m.position.astype(np.float64))
        PN.append(m.normal.astype(np.float64))
        F.append(m.triangles + base)
        fmat.append(np.full(m.surface_count, i))
        fsrc.append(np.arange(m.surface_count))
        base += m.vertex_count
    P = np.concatenate(P)
    PN = np.concatenate(PN)
    F = np.concatenate(F)
    fmat = np.concatenate(fmat)
    fsrc = np.concatenate(fsrc)

    # metal: grooves along the texture's panel lines (each groove four cuts; its floor is set in later). The stock
    # vertices stay first; the cuts' vertices follow them.
    groove_info, groove_rep = {}, {}
    for name, r in recipes.items():
        if not r.get("grooves"):
            continue
        made = 0
        for m in sorted({i for i in use if cls[i] == name}):
            mm = mats[m]
            bpng = mm.maps.get("base_map", {}).get("png")
            if not bpng:
                continue
            bump = mm.maps.get("bump_map", {}).get("png")
            us, vs, dark = panel_lines(os.path.join(extract_root, bpng), r,
                                       os.path.join(extract_root, bump) if bump else None)
            groove_info[m] = (us, vs, dark)
            sel = np.nonzero(fmat == m)[0]
            newP, newN, newF, newsrc = groove_cut_np(mm, us, vs, r, offsets[m], len(P))
            if len(newF) == 0:
                continue
            keep = np.ones(len(F), bool)
            cut_src = np.unique(newsrc)
            keep[sel[np.isin(fsrc[sel], cut_src)]] = False
            P = np.concatenate([P, newP])
            PN = np.concatenate([PN, newN])
            F = np.concatenate([F[keep], newF])
            fmat = np.concatenate([fmat[keep], np.full(len(newF), m)])
            fsrc = np.concatenate([fsrc[keep], newsrc])
            made += len(cut_src)
        groove_rep[name] = made
        log("grooves", name, "surfaces cut", made, "faces now", len(F))

    key = np.round(P / 1e-4).astype(np.int64)
    _, first, wid = np.unique(key, axis=0, return_index=True, return_inverse=True)
    wid = wid.ravel()
    W = P[first]
    FW = wid[F]
    stockN = PN

    def is_displaced(c):
        return c is not None and kind[c] in DISPLACED

    shaped_face = np.array([is_displaced(cls[i]) for i in fmat])
    a, b, c = W[FW[:, 0]], W[FW[:, 1]], W[FW[:, 2]]
    fn = np.cross(b - a, c - a)
    ref = stockN[F[:, 0]] + stockN[F[:, 1]] + stockN[F[:, 2]]
    fn = np.where(((fn * ref).sum(1) < 0)[:, None], -fn, fn)
    Nw = np.zeros_like(W)
    for k in range(3):
        e1 = W[FW[:, (k + 1) % 3]] - W[FW[:, k]]
        e2 = W[FW[:, (k + 2) % 3]] - W[FW[:, k]]
        ang = np.arccos(np.clip((normalize(e1) * normalize(e2)).sum(1), -1, 1))
        np.add.at(Nw, FW[shaped_face, k], (normalize(fn) * ang[:, None])[shaped_face])
    Nw = normalize(Nw)

    # ---------- the triangle budget: the displaced classes' target edge lengths scaled to meet it
    report = {"bsp": info["tag"], "variant": os.environ.get("WG_VARIANT", ""), "classes": {}}
    areas = {}
    tri_area = np.linalg.norm(fn, axis=1) * 0.5
    for name in recipes:
        sel = np.array([cls[i] == name for i in fmat])
        areas[name] = float(tri_area[sel].sum())
    # nearness to where one walks: faces one can stand on (any material), sampled about every walk_sample wu
    near = rec.get("near", {})
    nd, fd, fm = near.get("near_distance", 2.5), near.get("far_distance", 15.0), near.get("far_mult", 3.0)
    up = normalize(fn)[:, 2] > 0.7
    pts = []
    step = near.get("walk_sample", 1.0)
    for f_ in np.nonzero(up)[0]:
        a_, b_, c_ = W[FW[f_, 0]], W[FW[f_, 1]], W[FW[f_, 2]]
        n_ = int(min(40, max(1, math.ceil(max(np.linalg.norm(b_ - a_), np.linalg.norm(c_ - a_)) / step))))
        for i_ in range(n_ + 1):
            for j_ in range(n_ + 1 - i_):
                pts.append(a_ + (b_ - a_) * (i_ / n_) + (c_ - a_) * (j_ / n_))
    walk = KDTree(max(1, len(pts)))
    for j_, p_ in enumerate(pts):
        walk.insert(p_, j_)
    walk.balance()
    log("walk samples", len(pts))

    def mult_at(p_):
        if not pts:
            return 1.0
        dist_ = walk.find(p_)[2]
        t_ = min(1.0, max(0.0, (dist_ - nd) / max(fd - nd, 1e-6)))
        return 1.0 + (fm - 1.0) * t_ * t_ * (3 - 2 * t_)

    # estimate (edge splitting makes about 2.5 times the triangles of an even mesh)
    cent = (a + b + c) / 3.0
    est = 0.0
    for f_ in range(len(F)):
        c_ = cls[fmat[f_]]
        if c_ in recipes and recipes[c_].get("target_edge"):
            t_ = recipes[c_]["target_edge"] * mult_at(cent[f_])
            est += 2.5 * tri_area[f_] / (0.433 * t_ * t_)
    budget = rec.get("budget_triangles", 1.5e6)
    scale = max(1.0, math.sqrt(est / budget)) if est > 0 else 1.0
    targets = {n: recipes[n]["target_edge"] * scale for n in recipes if recipes[n].get("target_edge")}
    log("areas", {k: round(v, 1) for k, v in areas.items()}, "estimate", int(est), "scale %.2f" % scale)
    report["edge_scale"] = scale
    report["target_edge"] = targets

    # ---------- Blender: the topology
    bm = bmesh.new()
    lay_src = bm.faces.layers.int.new("src")
    lay_mat = bm.faces.layers.int.new("mat")
    bverts = [bm.verts.new(p) for p in W]
    skipped = []
    for f in range(len(FW)):
        t = FW[f]
        if t[0] == t[1] or t[1] == t[2] or t[0] == t[2]:
            skipped.append(f)
            continue
        try:
            face = bm.faces.new((bverts[t[0]], bverts[t[1]], bverts[t[2]]))
        except ValueError:
            skipped.append(f)
            continue
        face[lay_src] = int(fsrc[f])
        face[lay_mat] = int(fmat[f])
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    bm.normal_update()

    def face_cls(face):
        return cls[face[lay_mat]]

    def pinned_vert(v):
        if any(not e.is_manifold for e in v.link_edges):
            return True
        return any(face_cls(f) is None for f in v.link_faces)

    # metal: deep bevels on the mesh's own sharp edges; with "bevel_across" (the level's recipe), also the edges
    # where two metal classes meet (a corridor's frame against its panels), at the narrower class's bevel, in the
    # pass of the class with the narrower bevel
    across = rec.get("bevel_across", False)
    metal_names = [n for n, r in recipes.items() if r.get("bevel")]

    def bevel_owner(e):
        cs = [face_cls(f) for f in e.link_faces]
        if cs[0] == cs[1]:
            return cs[0]
        if across and cs[0] in metal_names and cs[1] in metal_names:
            return min(cs, key=lambda c_: recipes[c_]["bevel"])
        return None

    for name, r in recipes.items():
        if not r.get("bevel"):
            continue
        lim = math.radians(r["bevel_angle"])
        edges = []
        why = {"own": 0, "sharp": 0, "pinned": 0}
        for e in bm.edges:
            if len(e.link_faces) != 2 or bevel_owner(e) != name:
                continue
            why["own"] += 1
            if e.calc_face_angle(0.0) <= lim:
                continue
            why["sharp"] += 1
            if pinned_vert(e.verts[0]) or pinned_vert(e.verts[1]):
                why["pinned"] += 1
                continue
            edges.append(e)
        log("bevel", name, why)
        if edges:
            bmesh.ops.bevel(bm, geom=edges, offset=r["bevel"], offset_type="OFFSET", segments=r["bevel_segments"],
                            profile=r.get("bevel_profile", 0.5), affect="EDGES", clamp_overlap=True)
        report["classes"].setdefault(name, {})["bevelled_edges"] = len(edges)

    for name, n_ in groove_rep.items():
        report["classes"].setdefault(name, {})["groove_cut_surfaces"] = n_

    # adaptive subdivision of the displaced classes: split every edge longer than its target, until none is
    rounds = 0
    while True:
        long_edges = []
        for e in bm.edges:
            tgt = math.inf
            for f in e.link_faces:
                c_ = face_cls(f)
                if c_ in targets:
                    tgt = min(tgt, targets[c_])
            if tgt < math.inf:
                ln_ = e.calc_length()
                if ln_ > tgt and ln_ > tgt * mult_at((e.verts[0].co + e.verts[1].co) * 0.5):
                    long_edges.append(e)
        if not long_edges or rounds >= 14:
            break
        bmesh.ops.subdivide_edges(bm, edges=long_edges, cuts=1, use_grid_fill=True, use_single_edge=True)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
        rounds += 1
        log("subdivision round", rounds, "split", len(long_edges), "faces", len(bm.faces))
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    bm.verts.index_update()
    bm.faces.index_update()

    # arrays, through a mesh (fast)
    pinned = np.array([pinned_vert(v) for v in bm.verts])
    me = bpy.data.meshes.new("topo")
    bm.to_mesh(me)
    bm.free()
    nV = len(me.vertices)
    V = np.empty(nV * 3, np.float32)
    me.vertices.foreach_get("co", V)
    V = V.reshape(-1, 3).astype(np.float64)
    # keep the exact stock positions where vertices are stock (float32 round trip)
    nP = len(me.polygons)
    lt = np.empty(nP, np.int32)
    me.polygons.foreach_get("loop_total", lt)
    assert (lt == 3).all()
    T = np.empty(nP * 3, np.int32)
    me.polygons.foreach_get("vertices", T)
    T = T.reshape(-1, 3).astype(np.int64)
    tsrc = np.empty(nP, np.int32)
    me.attributes["src"].data.foreach_get("value", tsrc)
    tmat = np.empty(nP, np.int32)
    me.attributes["mat"].data.foreach_get("value", tmat)
    tsrc = tsrc.astype(np.int64)
    tmat = tmat.astype(np.int64)
    ne = len(me.edges)
    E = np.empty(ne * 2, np.int32)
    me.edges.foreach_get("vertices", E)
    E = E.reshape(-1, 2).astype(np.int64)
    bpy.data.meshes.remove(me)
    log("topology", nV, "vertices", len(T), "triangles", rounds, "rounds")

    # parent stock triangle of each face
    stock_tri = np.stack([mats[m].triangles[s] + offsets[m] for m, s in zip(tmat, tsrc)])
    PA, PB, PC = W[wid[stock_tri[:, 0]]], W[wid[stock_tri[:, 1]]], W[wid[stock_tri[:, 2]]]
    tcls = [cls[m] for m in tmat]
    tkind = np.array([kind[c_] if c_ else "" for c_ in tcls])
    shaped = np.isin(tkind, list(DISPLACED))

    # owner face per vertex: a displaced face if it has one
    owner = np.full(nV, -1)
    for sel in (np.nonzero(~shaped)[0], np.nonzero(shaped)[0]):
        for k in range(3):
            owner[T[sel, k]] = sel
    o = owner
    bc = barycentric(V, PA[o], PB[o], PC[o])
    # Phong rounding over CE's welded normals (displaced classes) and the smooth normal field
    phong = np.zeros_like(V)
    Ns = np.zeros_like(V)
    for k in range(3):
        Pi = W[wid[stock_tri[o, k]]]
        Ni = Nw[wid[stock_tri[o, k]]]
        proj = V - ((V - Pi) * Ni).sum(1, keepdims=True) * Ni
        phong += bc[:, k:k + 1] * proj
        Ns += bc[:, k:k + 1] * Ni
    vshaped = np.zeros(nV, bool)
    vshaped[T[shaped].ravel()] = True
    plane = normalize(np.cross(PB[o] - PA[o], PC[o] - PA[o]))
    sref = stockN[stock_tri[o, 0]] + stockN[stock_tri[o, 1]] + stockN[stock_tri[o, 2]]
    plane = np.where(((plane * sref).sum(1) < 0)[:, None], -plane, plane)
    Ns = np.where(vshaped[:, None] & (np.linalg.norm(Ns, axis=1, keepdims=True) > 1e-6), normalize(Ns), plane)

    # per-vertex recipe: the most cautious class around it
    vcls_rank = {}
    alpha = np.full(nV, np.inf)
    cap_in = np.full(nV, np.inf)
    cap_out = np.full(nV, np.inf)
    vclass = np.full(nV, "", dtype=object)
    for name, r in recipes.items():
        sel = np.array([c_ == name for c_ in tcls])
        if not sel.any():
            continue
        vs_ = np.unique(T[sel])
        alpha[vs_] = np.minimum(alpha[vs_], r.get("phong", 0.0))
        cap_in[vs_] = np.minimum(cap_in[vs_], r.get("cap_in", 0.0))
        cap_out[vs_] = np.minimum(cap_out[vs_], r.get("cap_out", 0.0))
    for arr in (alpha, cap_in, cap_out):
        arr[~np.isfinite(arr)] = 0.0
    unshaped_v = np.unique(T[~shaped]) if (~shaped).any() else np.array([], np.int64)
    pin = pinned.copy()
    pin[unshaped_v] = True
    # fade near pinned vertices, by distance
    fade_r = rec.get("fade_distance", 0.25)
    weight = np.ones(nV)
    pin_ids = np.nonzero(pin)[0]
    if len(pin_ids):
        kd = KDTree(len(pin_ids))
        for j, vi in enumerate(pin_ids):
            kd.insert(V[vi], j)
        kd.balance()
        free_ids = np.nonzero(~pin)[0]
        dist = np.array([kd.find(V[vi])[2] for vi in free_ids])
        weight[free_ids] = shp.smoothstep(0.0, fade_r, dist)
        weight[pin_ids] = 0.0
    log("fade computed")

    # base: Phong-rounded position and the smooth normal there
    Xb = V + (phong - V) * (alpha * weight)[:, None]  # (none at pinned vertices: they stay on their neighbours')

    # texture bands per vertex (from the owner face's material and its stock UVs)
    tex_f = np.zeros(nV)
    tex_c = np.zeros(nV)
    for m in sorted(set(tmat.tolist())):
        c_ = cls[m]
        if not is_displaced(c_):
            continue
        r = recipes[c_]
        png = mats[m].maps.get("base_map", {}).get("png")
        if not png:
            continue
        sel = np.nonzero(tmat[o] == m)[0]
        if not len(sel):
            continue
        fine, coarse = relief_bands(os.path.join(extract_root, png), r.get("tex_fine_px", 4), r.get("tex_coarse_px", 24))
        st = mats[m].triangles[tsrc[o[sel]]]
        uv = (bc[sel, :1] * mats[m].uv0[st[:, 0]] + bc[sel, 1:2] * mats[m].uv0[st[:, 1]] +
              bc[sel, 2:3] * mats[m].uv0[st[:, 2]])
        tex_f[sel] = sample_wrap(fine, uv)
        tex_c[sel] = sample_wrap(coarse, uv)

    # displacement per class of the owner face
    d = np.zeros(nV)
    parts_rep = {}
    for name, r in recipes.items():
        if not is_displaced(name):
            continue
        sel = np.nonzero(vshaped & np.array([tcls[f_] == name for f_ in o]))[0]
        if not len(sel):
            continue
        fnc = {"rock": shp.rock, "ground": shp.ground, "smooth": shp.smooth}[kind[name]]
        dd, parts = fnc(Xb[sel], Ns[sel], r, tex_c[sel], tex_f[sel])
        d[sel] = dd
        parts_rep[name] = {k: [float(np.percentile(v, 5)), float(np.percentile(v, 95))] for k, v in parts.items()}
    d = shp.soft_clamp(d, -cap_in, cap_out) * weight

    phong_w = alpha * weight

    def place(d_):
        Xb_ = V + (phong - V) * phong_w[:, None]
        X_ = Xb_ + Ns * d_[:, None]
        X_ = np.where(vshaped[:, None], X_, V)
        # the whole offset off the stock (collision) plane, Phong's rounding included, within the class's caps: what
        # one sees stays within cap_out in front of and cap_in behind where one collides
        off = ((X_ - V) * plane).sum(1)
        off_c = shp.soft_clamp(off, -cap_in, cap_out)
        return X_ + plane * np.where(vshaped, off_c - off, 0.0)[:, None]

    # no folds: where moving along the normal turns a triangle over (an inward move at a convex crease, or outward in
    # a concave one, deeper than the crease's radius), its corners and their neighbours move less, until none does;
    # a turned triangle would be culled and show what is behind the rock
    def tri_normals(XX, sel_):
        a_, b_, c_ = XX[T[sel_, 0]], XX[T[sel_, 1]], XX[T[sel_, 2]]
        return np.cross(b_ - a_, c_ - a_)

    shaped_ids = np.nonzero(shaped)[0]
    # (against the stock triangle each stands for: facing away from it, it would be culled)
    n_pre = normalize(np.cross(PB[shaped_ids] - PA[shaped_ids], PC[shaped_ids] - PA[shaped_ids]))
    n_pre = np.where(((n_pre * tri_normals(V, shaped_ids)).sum(1) < 0)[:, None], -n_pre, n_pre)
    folds_rep = []
    for it in range(20):
        X = place(d)
        n_post = tri_normals(X, shaped_ids)
        ln = np.linalg.norm(n_post, axis=1)
        cosv = (n_post * n_pre).sum(1) / np.maximum(ln, 1e-12)
        bad = shaped_ids[(cosv < 0.25) & (ln > 1e-12)]
        folds_rep.append(int(len(bad)))
        if not len(bad):
            break
        hit = np.zeros(nV, bool)
        hit[T[bad].ravel()] = True
        # and the ring around them
        ring = hit[E[:, 0]] | hit[E[:, 1]]
        hit[E[ring].ravel()] = True
        d = np.where(hit, d * 0.6, d)
        phong_w = np.where(hit, phong_w * 0.6, phong_w)
        if it >= 14:
            # (the last rounds: those still turned go back onto the stock surface)
            d = np.where(hit, 0.0, d)
            phong_w = np.where(hit, 0.0, phong_w)
    report["folds_fixed"] = folds_rep
    log("folds by round", folds_rep)
    # metal grooves: inner groove vertices set in along the face's plane normal
    groove_depth = np.zeros(nV)
    for name, r in recipes.items():
        if not r.get("grooves"):
            continue
        for m, (us, vs, dark) in groove_info.items():
            if cls[m] != name:
                continue
            sel = np.nonzero((tmat[o] == m) & ~vshaped & ~pinned)[0]  # (pinned: open edges, unremodelled neighbours)
            if not len(sel):
                continue
            mm = mats[m]
            st = mm.triangles[tsrc[o[sel]]]
            uv = (bc[sel, :1] * mm.uv0[st[:, 0]] + bc[sel, 1:2] * mm.uv0[st[:, 1]] + bc[sel, 2:3] * mm.uv0[st[:, 2]])
            inner = groove_inner(uv, us, vs, r)
            dk = np.clip(sample_wrap(dark, uv), 0.0, 1.5) / 1.5
            groove_depth[sel] = r["groove_depth"] * inner * (0.6 + 0.4 * dk)
    X = X - plane * groove_depth[:, None]
    log("shaped")

    # ---------- normals per corner (smoothing angle per class, families kept apart)
    def face_normals(XX):
        a_, b_, c_ = XX[T[:, 0]], XX[T[:, 1]], XX[T[:, 2]]
        n = np.cross(b_ - a_, c_ - a_)
        r_ = stockN[stock_tri[:, 0]] + stockN[stock_tri[:, 1]] + stockN[stock_tri[:, 2]]
        return np.where(((n * r_).sum(1) < 0)[:, None], -n, n)

    fnr = face_normals(X)
    fnu = normalize(fnr)
    area = np.linalg.norm(fnr, axis=1) * 0.5
    family = np.array([0 if k_ in ("rock", "ground") else (3 if k_ == "smooth" else (1 if k_ else 2)) for k_ in tkind])
    smooth_cos = np.array([math.cos(math.radians(recipes[c_]["smooth_angle"])) if c_ else 1.0 for c_ in tcls])
    order = np.argsort(T.ravel(), kind="stable")
    vids = T.ravel()[order]
    fids = order // 3
    counts = np.bincount(vids, minlength=nV)
    K = counts.max()
    starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
    slot = np.arange(len(vids)) - starts[vids]
    inc = np.full((nV, K), -1)
    inc[vids, slot] = fids
    cornerN = np.zeros((len(T), 3, 3))
    log("normals: largest valence", int(K))
    # in batches of faces, so that the (faces, valence, 3) temporaries stay a few hundred MB
    step_ = max(1024, int(4e7 // max(K, 1)))
    for s0 in range(0, len(T), step_):
        s1 = min(len(T), s0 + step_)
        fb = fnu[s0:s1]
        for k in range(3):
            g = inc[T[s0:s1, k]]
            valid = g >= 0
            gg = np.where(valid, g, 0)
            fg = fnu[gg]
            dots = (fg * fb[:, None, :]).sum(2)
            ok = valid & (dots >= smooth_cos[s0:s1, None]) & (family[gg] == family[s0:s1, None])
            wsum = (fg * (area[gg] * ok)[..., None]).sum(1)
            cornerN[s0:s1, k] = normalize(np.where(np.linalg.norm(wsum, axis=1, keepdims=True) > 0, wsum, fb))
            del g, valid, gg, fg, dots, ok, wsum
    del inc
    # flat metal faces keep CE's own interpolated normals
    flat_metal = np.zeros(len(T), bool)
    for m in use:
        if cls[m] is None or is_displaced(cls[m]):
            continue
        sel = np.nonzero(tmat == m)[0]
        pl = normalize(np.cross(PB[sel] - PA[sel], PC[sel] - PA[sel]))
        flat_metal[sel] = np.abs((pl * fnu[sel]).sum(1)) > math.cos(math.radians(8.0))
    stockcorner = np.zeros((len(T), 3, 3))
    for k in range(3):
        vv = T[:, k]
        bck = barycentric(V[vv], PA, PB, PC)
        stockcorner[:, k] = normalize(sum(bck[:, j:j + 1] * stockN[stock_tri[:, j]] for j in range(3)))
    cornerN = np.where(flat_metal[:, None, None], stockcorner, cornerN)
    log("normals")

    # ---------- light on the new shape
    ratio = np.ones(nV)
    if light.get("bake", True):
        sun = estimate_sun(mats, use, extract_root, gltf_json) if not light.get("sun") else normalize(
            np.array(light["sun"], np.float64))
        report["sun"] = sun.tolist()
        log("sun", sun.round(3))
        # occluders: every senv face of the level not remodelled, plus the transparent ones' (sotr/sgla) are left out
        occ_sel = [i for i in use if cls[i] is None]
        occ_X, occ_T, base_ = [], [], 0
        for i in occ_sel:
            occ_X.append(mats[i].position.astype(np.float64))
            occ_T.append(mats[i].triangles + base_)
            base_ += mats[i].vertex_count
        occ_X = np.concatenate(occ_X) if occ_X else np.zeros((0, 3))
        occ_T = np.concatenate(occ_T) if occ_T else np.zeros((0, 3), np.int64)
        ratio, lit_new, lit_ref = light_ratio(X, T, cornerN, V, stockcorner, occ_X, occ_T, sun, light)
        # one pass of smoothing over the mesh's edges (bake noise)
        for _ in range(int(light.get("smooth_passes", 1))):
            s_ = ratio.copy()
            n_ = np.ones(nV)
            np.add.at(s_, E[:, 0], ratio[E[:, 1]])
            np.add.at(s_, E[:, 1], ratio[E[:, 0]])
            np.add.at(n_, E[:, 0], 1)
            np.add.at(n_, E[:, 1], 1)
            ratio = s_ / n_
        ratio = np.clip(ratio, light.get("ratio_min", 0.12), light.get("ratio_max", 1.25))
        # metal: only where the shape changed (bevels, grooves), so flat panels keep CE's light exactly
        moved = np.linalg.norm(X - V, axis=1) > 1e-5
        is_metal_v = ~vshaped
        changed = np.zeros(nV, bool)
        bevel_face = ~flat_metal & ~shaped
        if light.get("bevels", True):
            changed[T[bevel_face].ravel()] = True
        keep = is_metal_v & ~changed & ~moved
        ratio[keep] = 1.0
        for name in recipes:
            selv = np.zeros(nV, bool)
            selv[T[np.array([c_ == name for c_ in tcls])].ravel()] = True
            if selv.any():
                report["classes"].setdefault(name, {})["light_ratio"] = [
                    float(np.percentile(ratio[selv], q)) for q in (1, 5, 25, 50, 75, 95, 99)]
        rs = ratio[vshaped] if vshaped.any() else ratio
        report["light_ratio"] = {"p5": float(np.percentile(rs, 5)), "p50": float(np.percentile(rs, 50)),
                                 "p95": float(np.percentile(rs, 95))}
        log("ratio", report["light_ratio"])

    # ---------- corners: the stock surface's attributes, the light ratio in the incident radiosity
    remodels = []
    stats = {}
    for m in use:
        if cls[m] is None:
            continue
        mm = mats[m]
        sel = np.nonzero(tmat == m)[0]
        st = mm.triangles[tsrc[sel]]
        rows = []
        for k in range(3):
            vv = T[sel, k]
            bck = barycentric(V[vv], PA[sel], PB[sel], PC[sel])

            def lerp(attr):
                return bck[:, :1] * attr[st[:, 0]] + bck[:, 1:2] * attr[st[:, 1]] + bck[:, 2:3] * attr[st[:, 2]]
            n = cornerN[sel, k]
            t = normalize(lerp(mm.tangent))
            t = normalize(t - n * (t * n).sum(1, keepdims=True))
            bnm = normalize(lerp(mm.binormal))
            bnm = normalize(bnm - n * (bnm * n).sum(1, keepdims=True) - t * (bnm * t).sum(1, keepdims=True))
            rad = lerp(mm.radiosity)
            rr = ratio[vv]
            bshade = light.get("bevel_shade", 0.0)
            if bshade > 0 and not is_displaced(cls[m]):
                # a bevel's or groove wall's own light: CE's lightmap term for its new normal against the stock one's
                # (lerp(1, n.L, |L|), |L| at least bevel_shade), so chamfers facing the level's light brighten and
                # those turned from it darken, as a lightmap baked with them would show
                bent = ~flat_metal[sel]
                L0 = normalize(rad)
                me_ = np.maximum(np.linalg.norm(rad, axis=1), bshade)
                f_new = 1.0 - me_ + me_ * np.clip((n * L0).sum(1), 0.0, 1.0)
                f_old = 1.0 - me_ + me_ * np.clip((stockcorner[sel, k] * L0).sum(1), 0.0, 1.0)
                rr = np.where(bent, rr * np.clip(f_new / np.maximum(f_old, 0.05), 0.35, 1.6), rr)
            rad = encode_radiosity(rad, stockcorner[sel, k], n, t, rr)
            rows.append(np.concatenate([X[vv], n, bnm, t, lerp(mm.uv0), lerp(mm.uv1), rad], 1))
        rows = np.stack(rows, 1).reshape(-1, 19)
        owner_src = tsrc[sel]
        sk = [f for f in skipped if fmat[f] == m]
        # (a skipped face stands in only when its whole surface is missing: a degenerate sliver of a cut surface
        # is just dropped)
        present = set(tsrc[sel].tolist())
        sk = [f for f in sk if int(fsrc[f]) not in present]
        sk = list({int(fsrc[f]): f for f in sk}.values())
        if sk:
            s_ = fsrc[sk]
            st2 = mm.triangles[s_]
            extra = np.stack([np.concatenate([mm.position[st2[:, k]], mm.normal[st2[:, k]], mm.binormal[st2[:, k]],
                                              mm.tangent[st2[:, k]], mm.uv0[st2[:, k]], mm.uv1[st2[:, k]],
                                              mm.radiosity[st2[:, k]]], 1) for k in range(3)], 1).reshape(-1, 19)
            rows = np.concatenate([rows, extra])
            owner_src = np.concatenate([owner_src, s_])
        q = np.round(rows / 1e-6).astype(np.int64)
        uq, first_i, inv = np.unique(q, axis=0, return_index=True, return_inverse=True)
        vrows = rows[first_i]
        tri = inv.ravel().reshape(-1, 3)
        r = wg.Remodel(mm, vrows[:, 0:3], vrows[:, 3:6], vrows[:, 6:9], vrows[:, 9:12], vrows[:, 12:14],
                       vrows[:, 14:16], vrows[:, 16:19], tri, owner_src)
        # the far level of detail: displaced classes only (metal's cuts and bevels stay whole)
        r.lod1 = None
        spacing = recipes[cls[m]].get("lod_spacing", 0.0)
        if is_displaced(cls[m]) and spacing > 0 and rec.get("lod_distance", 0.0) > 0:
            r.lod1 = [None] * mm.surface_count
            nsel = len(sel)
            Tsel = T[sel]
            srt = np.argsort(owner_src[:nsel], kind="stable")
            bounds_ = np.searchsorted(owner_src[:nsel][srt], np.arange(mm.surface_count + 1))
            saved = 0
            for s_ in range(mm.surface_count):
                idx = srt[bounds_[s_]:bounds_[s_ + 1]]
                if len(idx) < 24:
                    continue
                stv = mm.triangles[s_] + offsets[m]
                f1 = wg2_lod.surface_lod1(tri[idx], Tsel[idx], V, W[wid[stv[0]]], W[wid[stv[1]]], W[wid[stv[2]]],
                                          spacing)
                if f1 is not None:
                    r.lod1[s_] = f1
                    saved += len(idx) - len(f1)
            lod_rep = report.setdefault("lod", {}).setdefault(cls[m], {"near": 0, "saved": 0})
            lod_rep["near"] += int(nsel)
            lod_rep["saved"] += int(saved)
        remodels.append(r)
        vsel = np.unique(T[sel])
        dv = ((X[vsel] - V[vsel]) * plane[vsel]).sum(1)
        s = stats.setdefault(cls[m], {"materials": 0, "stock_triangles": 0, "triangles": 0, "vertices": 0, "d": []})
        s["materials"] += 1
        s["stock_triangles"] += mm.surface_count
        s["triangles"] += len(tri)
        s["vertices"] += len(vrows)
        s["d"].append(dv)
    for name, s in stats.items():
        dv = np.concatenate(s.pop("d"))
        s["off_surface_wu"] = {"min": float(dv.min()), "p5": float(np.percentile(dv, 5)),
                               "p50": float(np.percentile(dv, 50)), "p95": float(np.percentile(dv, 95)),
                               "max": float(dv.max())}
        report["classes"].setdefault(name, {}).update(s)
    report["shape_parts"] = parts_rep
    report["skipped_stock_surfaces"] = len(skipped)
    report["triangles"] = int(sum(len(r.triangles) for r in remodels))
    report["stock_triangles"] = int(sum(r.stock.surface_count for r in remodels))
    size = wg.write_pwg(out_path, info["tag"], remodels, rec.get("lod_distance", 0.0))
    report["bytes"] = size
    report["seconds"] = round(time.time() - T0, 1)
    json.dump(report, open(report_path, "w"), indent=1)
    log("WG2_REPORT", json.dumps(report))


# ---------- metal grooves

def _split(poly, f):
    """a convex polygon (list of (pos, nrm, uv)) split by the sign of f (one value per vertex): (below, above)"""
    lo, hi = [], []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        fa, fb = f[i], f[(i + 1) % n]
        if fa <= 0:
            lo.append(a)
        if fa >= 0:
            hi.append(a)
        if (fa < 0 < fb) or (fb < 0 < fa):
            t = fa / (fa - fb)
            c = tuple(a[k] + (b[k] - a[k]) * t for k in range(3))
            lo.append(c)
            hi.append(c)
    return lo, hi


def groove_cut_np(mm, us, vs, r, vbase, nbase):
    """Each stock surface of one material cut along iso-u and iso-v lines of its texture (each groove four lines: its
    outer edges and its floor's), as convex cells fanned into triangles. Cut points on an edge two surfaces share
    are the same point when the texture runs on across it, so the welded mesh stays connected; where it does not, the
    edge opens and its vertices are pinned (never moved). Returns new vertices (positions, normals) and triangles
    (indices into the stock arrays for stock corners, nbase + i for new ones) with their surfaces."""
    width = r["groove_width"]
    floor_ = r.get("groove_floor", 0.5)
    offs = (-width / 2, -width * floor_ / 2, width * floor_ / 2, width / 2)
    newP, newN, tris, srcs = [], [], [], []
    vid = {}

    def index_of(item):
        p, n_, uv = item
        k = (round(p[0], 6), round(p[1], 6), round(p[2], 6))
        if k in vid:
            return vid[k]
        idx = nbase + len(newP)
        newP.append(p)
        newN.append(n_)
        vid[k] = idx
        return idx

    maxrep = r.get("max_repeats", 6)
    for s in range(mm.surface_count):
        tri = mm.triangles[s]
        uv = mm.uv0[tri].astype(np.float64)
        lines = []
        for axis, cs in ((0, us), (1, vs)):
            lo, hi = uv[:, axis].min(), uv[:, axis].max()
            if hi - lo > maxrep or not cs:
                continue
            for c0 in cs:
                for rep in range(int(math.floor(lo - c0)) - 1, int(math.ceil(hi - c0)) + 2):
                    for off in offs:
                        c = c0 + rep + off
                        if lo + 1e-6 < c < hi - 1e-6:
                            lines.append((axis, c))
        if not lines:
            continue
        stock_ids = {}
        poly = []
        for k in range(3):
            p = tuple(float(x) for x in mm.position[tri[k]])
            poly.append((np.array(p), mm.normal[tri[k]].astype(np.float64), uv[k]))
            stock_ids[(round(p[0], 6), round(p[1], 6), round(p[2], 6))] = vbase + int(tri[k])
        cells = [poly]
        for axis, c in sorted(lines):
            out = []
            for cell in cells:
                f = [it[2][axis] - c for it in cell]
                if min(f) >= 0 or max(f) <= 0:
                    out.append(cell)
                    continue
                a_, b_ = _split(cell, f)
                if len(a_) >= 3:
                    out.append(a_)
                if len(b_) >= 3:
                    out.append(b_)
            cells = out
        for cell in cells:
            ids = []
            for it in cell:
                k = (round(float(it[0][0]), 6), round(float(it[0][1]), 6), round(float(it[0][2]), 6))
                ids.append(stock_ids[k] if k in stock_ids else index_of(it))
            # drop repeats (cut lines through a corner)
            clean = [ids[0]] + [ids[i] for i in range(1, len(ids)) if ids[i] != ids[i - 1]]
            if len(clean) > 1 and clean[-1] == clean[0]:
                clean.pop()
            for i in range(1, len(clean) - 1):
                tris.append((clean[0], clean[i], clean[i + 1]))
                srcs.append(s)
    if not tris:
        return np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 3), np.int64), np.zeros(0, np.int64)
    newN = normalize(np.array(newN, np.float64)) if newN else np.zeros((0, 3))
    return (np.array(newP, np.float64).reshape(-1, 3), newN.reshape(-1, 3), np.array(tris, np.int64),
            np.array(srcs, np.int64))


def groove_inner(uv, us, vs, r):
    """1 for a vertex on a groove's floor (between its two inner lines), else 0"""
    width = r["groove_width"]
    floor_ = r.get("groove_floor", 0.5)
    half = width * floor_ / 2 + 1e-4
    inner = np.zeros(len(uv))
    for axis, lines in ((0, us), (1, vs)):
        for c0 in lines:
            dlt = uv[:, axis] - c0
            dlt = dlt - np.round(dlt)
            inner = np.maximum(inner, (np.abs(dlt) <= half).astype(np.float64))
    return inner


# ---------- light in the incident radiosity

def encode_radiosity(rad, n_ref, n_new, tangent, ratio):
    """CE's lightmap pass shades lightmap x lerp(1, dot(bump, L^), |L|), with L^ in the vertex's tangent frame. The
    stock surface showed F0 = lerp(1, max(0, n_ref . L^), m) here; this returns the L for the new vertex (normal n_new)
    that shows F0 x ratio with a flat bump: the same length where it can, longer only when the darkening needs it,
    turned in the plane of the stock light's direction so that the bump keeps lighting from the same side."""
    m = np.linalg.norm(rad, axis=1)
    L0 = rad / np.maximum(m, 1e-9)[:, None]
    c0 = np.clip((n_ref * L0).sum(1), 0.0, 1.0)
    F0 = 1.0 - m + m * c0
    Ft = np.clip(F0 * ratio, 0.0, 1.0)
    m1 = m.copy()
    c1 = np.where(m1 > 1e-6, 1.0 - (1.0 - Ft) / np.maximum(m1, 1e-6), 1.0)
    low = c1 < 0.0
    m1 = np.where(low, np.minimum(1.0, 1.0 - Ft), m1)
    c1 = np.where(low, 0.0, c1)
    c1 = np.clip(c1, 0.0, 1.0)
    tdir = L0 - n_new * (L0 * n_new).sum(1, keepdims=True)
    tl = np.linalg.norm(tdir, axis=1, keepdims=True)
    tdir = np.where(tl > 1e-4, tdir / np.maximum(tl, 1e-9), tangent)
    s1 = np.sqrt(np.maximum(0.0, 1.0 - c1 * c1))
    L1 = n_new * c1[:, None] + tdir * s1[:, None]
    # the game packs it in 11:11:10 bits with a clamp: keep inside the unit ball
    return L1 * (np.minimum(m1, 0.999))[:, None]


main()
