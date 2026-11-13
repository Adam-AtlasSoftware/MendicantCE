"""FP arms remodel proof, headless Blender:

  blender -b --factory-startup --python art/fp_build.py -- art/config_fp_arms.json

Steps (each recorded in <out_dir>/build_report.json):
  1. import the extracted fp arms glTF (skeleton + skinned mesh), check the bind pose
     against the extracted JMS node block;
  2. weld the CE mesh by position (the reference envelope);
  3. game mesh: Catmull-Clark subdivision, then a band restore that puts every vertex back on
     the CE surface (nearest point on the CE facets), rounded band_alpha of the way toward the
     Phong surface through the CE vertices (CE corner normals, hard edges kept flat), so the
     result stays inside the allowed band between the CE polygon and the smooth curve through
     its vertices instead of shrinking inside it;
  4. weights: Data Transfer from the CE mesh (nearest face interpolated), limit to
     max_influences, normalise, on the SAME armature (no new bones);
  5. UVs: keep the CE layout as UVMap (TEXCOORD_0, Channel A) and add a unique unwrap
     uv_unique (seams from the CE islands, angle-based unwrap, pack);
  6. high-poly: two more subdivision levels, the same band restore, plus procedural inward-only displacement
     (rubber stipple/creases, armour grooves/orange peel);
  7. Cycles bakes high -> game mesh on uv_unique: tangent normal, AO, cavity (pointiness);
     CE albedo reprojected through the carried CE UVs from the 2x upscaled base map (emission
     bake, no rays); microtexture class mask (R glove, G undersuit, B plate);
  8. export glTF (rig + game mesh) and JMS 8200 (own writer, node block copied verbatim
     from the extracted JMS); JMS writer self-test on the CE mesh;
  9. geometry checks (envelope, weights, UV, normal map validity) and save .blend.
"""
import json
import math
import os
import sys
import time

import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402
import halo_formats as hf  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()
REPORT = {"steps": {}}


def P(rel):
    return E.path(rel)


def step(name, **kv):
    REPORT["steps"][name] = dict(kv, t=round(time.time() - T0, 2))
    print("STEP", name, json.dumps(kv, default=str)[:400], flush=True)


def activate(obj, others=()):
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in others:
        o.select_set(True)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def dup(obj, name):
    o = obj.copy()
    o.data = obj.data.copy()
    o.name = name
    o.data.name = name
    bpy.context.scene.collection.objects.link(o)
    return o


def mesh_np(obj):
    me = obj.data
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def set_mesh_np(obj, co):
    obj.data.vertices.foreach_set("co", co.astype(np.float32).ravel())
    obj.data.update()


# ---------------------------------------------------------------- procedural detail
def _hash(ix, iy, iz, seed):
    h = (ix.astype(np.int64) * 73856093) ^ (iy.astype(np.int64) * 19349663) ^ (iz.astype(np.int64) * 83492791) ^ seed
    h = h & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF).astype(np.float32) / float(0x1000000)


def worley_f1(p, cell, seed=1):
    q = p / cell
    base = np.floor(q).astype(np.int64)
    best = np.full(len(p), 1e9, np.float32)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                c = base + np.array([dx, dy, dz])
                j = np.stack([_hash(c[:, 0], c[:, 1], c[:, 2], seed + k) for k in range(3)], 1)
                d2 = ((q - (c + j)) ** 2).sum(1)
                best = np.minimum(best, d2)
    return np.sqrt(best)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def detail_height(p, rubber_frac, d):
    """Inward-only displacement (<= 0) in world units."""
    f1 = worley_f1(p, d["rubber_cell"], 11)
    pebble = 1.0 - smoothstep(0.15, 0.55, f1)            # 1 at cell centres
    h_rub = -d["rubber_depth"] * (1.0 - pebble)
    crease = 0.5 + 0.5 * np.sin(2 * math.pi * p[:, 0] / d["rubber_crease_spacing"])
    h_rub += -d["rubber_crease_depth"] * crease ** 4
    h_arm = np.zeros(len(p), np.float32)
    for k, dirv in enumerate(((1.0, 0.35, 0.2), (0.15, 1.0, -0.45))):
        a = np.array(dirv, np.float32)
        a /= np.linalg.norm(a)
        s = p @ a / d["armor_groove_spacing"] + 0.37 * k
        dist = np.minimum(s - np.floor(s), np.ceil(s) - s) * d["armor_groove_spacing"]
        h_arm = np.minimum(h_arm, -d["armor_groove_depth"] * (1.0 - smoothstep(0.0, d["armor_groove_width"], dist)))
    peel = worley_f1(p, d["armor_peel_cell"], 29)
    h_arm += -d["armor_peel_depth"] * smoothstep(0.0, 0.7, peel)
    return rubber_frac * h_rub + (1 - rubber_frac) * h_arm


def vertex_material_frac(obj, mat_index=0):
    me = obj.data
    n = len(me.vertices)
    acc = np.zeros(n, np.float32)
    cnt = np.zeros(n, np.float32)
    lt = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_total", lt)
    mi = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("material_index", mi)
    lv = np.empty(len(me.loops), np.int32)
    me.loops.foreach_get("vertex_index", lv)
    pm = np.repeat(mi, lt)
    np.add.at(acc, lv, (pm == mat_index).astype(np.float32))
    np.add.at(cnt, lv, 1)
    return acc / np.maximum(cnt, 1)


# ---------------------------------------------------------------- envelope
class CEBand:
    """The CE envelope band: for any point on a CE facet, the facet itself (limit 0) and the
    Phong surface through the CE vertices (limit 1, the smooth curve through the vertices).
    Rounding is allowed only in between (chord
    smoothing).

    Position normals (pn) make the Phong surface continuous and keep CE's hard edges: a CE
    vertex is 'smooth' when all its corner normals agree within smooth_deg and their mean leans
    at most max_lean_deg from every face around it; it then has ONE normal for all its
    triangles. Every other vertex is 'pinned': it uses each face's own normal, which adds zero
    offset on that face and on its edges, so hard edges and rims stay exactly CE's.
    Shading normals (cn) are CE's own corner normals, unchanged."""

    def __init__(self, ref_obj, max_lean_deg=60.0, smooth_deg=5.0):
        me = ref_obj.data
        me.calc_loop_triangles()
        self.v = mesh_np(ref_obj).astype(np.float64)
        n_lt = len(me.loop_triangles)
        tv = np.empty(n_lt * 3, np.int32)
        me.loop_triangles.foreach_get("vertices", tv)
        tl = np.empty(n_lt * 3, np.int32)
        me.loop_triangles.foreach_get("loops", tl)
        self.t = tv.reshape(-1, 3)
        cn = np.empty(len(me.loops) * 3, np.float32)
        me.corner_normals.foreach_get("vector", cn)
        cn = cn.reshape(-1, 3).astype(np.float64)[tl.reshape(-1, 3)]          # (T,3,3)
        p = self.v[self.t]
        fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
        fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-20)
        self.cn = cn
        self.fn = fn
        # position normals per welded CE vertex
        key = np.round(self.v / 1e-5).astype(np.int64)
        _, wid = np.unique(key, axis=0, return_inverse=True)
        wid = wid.ravel()
        wt = wid[self.t].ravel()                                               # (T*3,)
        cnf = cn.reshape(-1, 3)
        fnc = np.repeat(fn, 3, 0)
        nw = wid.max() + 1
        mean = np.zeros((nw, 3))
        np.add.at(mean, wt, cnf)
        mean /= np.maximum(np.linalg.norm(mean, axis=1, keepdims=True), 1e-20)
        agree = np.ones(nw)
        np.minimum.at(agree, wt, (cnf * mean[wt]).sum(1))
        lean = np.ones(nw)
        np.minimum.at(lean, wt, (fnc * mean[wt]).sum(1))
        smooth = (agree >= math.cos(math.radians(smooth_deg))) & (lean >= math.cos(math.radians(max_lean_deg)))
        self.pn = np.where(smooth[wt][:, None], mean[wt], fnc).reshape(-1, 3, 3)
        self.n_vertices = int(nw)
        self.n_pinned = int((~smooth).sum())
        self.n_hard_corners = int((~smooth[wt]).sum())
        self.tree = BVHTree.FromPolygons([tuple(x) for x in self.v], [tuple(x) for x in self.t])
        lp = np.empty(n_lt, np.int32)
        me.loop_triangles.foreach_get("polygon_index", lp)
        self.poly2tri = np.full(len(me.polygons), -1, np.int64)
        self.poly2tri[lp] = np.arange(n_lt)       # CE polygons are triangles (checked at import)

    def nearest(self, co):
        loc = np.empty_like(co, dtype=np.float64)
        idx = np.empty(len(co), np.int64)
        for i, v in enumerate(co):
            l_, _, k, _ = self.tree.find_nearest(Vector(v))
            loc[i] = l_
            idx[i] = k
        return loc, idx

    def phong(self, loc, idx):
        p = self.v[self.t[idx]]                                               # (N,3,3)
        e0, e1, d = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0], loc - p[:, 0]
        d00, d01, d11 = (e0 * e0).sum(1), (e0 * e1).sum(1), (e1 * e1).sum(1)
        d20, d21 = (d * e0).sum(1), (d * e1).sum(1)
        den = np.maximum(d00 * d11 - d01 * d01, 1e-30)
        b1 = (d11 * d20 - d01 * d21) / den
        b2 = (d00 * d21 - d01 * d20) / den
        b = np.clip(np.stack([1 - b1 - b2, b1, b2], 1), 0, 1)
        b /= b.sum(1, keepdims=True)
        n = self.pn[idx]
        proj = loc[:, None, :] - ((loc[:, None, :] - p) * n).sum(2, keepdims=True) * n
        return (b[:, :, None] * proj).sum(1)

    def corner_normals(self, obj):
        """CE shading normals for every face corner of obj: each face knows its CE triangle
        (face attribute ce_tri, carried through SIMPLE subdivision, which keeps every child face
        inside its CE triangle), and the corner takes that triangle's corner normals interpolated
        at its own facet point, so CE's smooth and hard edges both carry over."""
        me = obj.data
        co = mesh_np(obj).astype(np.float64)
        n_p = len(me.polygons)
        ce = np.empty(n_p, np.int32)
        me.attributes["ce_tri"].data.foreach_get("value", ce)
        fidx = self.poly2tri[ce]
        lt = np.empty(n_p, np.int32)
        me.polygons.foreach_get("loop_total", lt)
        lv = np.empty(len(me.loops), np.int32)
        me.loops.foreach_get("vertex_index", lv)
        tri = np.repeat(fidx, lt)
        p = self.v[self.t[tri]]
        x = co[lv]
        x = x - ((x - p[:, 0]) * self.fn[tri]).sum(1, keepdims=True) * self.fn[tri]   # onto the facet plane
        e0, e1, d = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0], x - p[:, 0]
        d00, d01, d11 = (e0 * e0).sum(1), (e0 * e1).sum(1), (e1 * e1).sum(1)
        d20, d21 = (d * e0).sum(1), (d * e1).sum(1)
        den = np.maximum(d00 * d11 - d01 * d01, 1e-30)
        b1 = (d11 * d20 - d01 * d21) / den
        b2 = (d00 * d21 - d01 * d20) / den
        b = np.clip(np.stack([1 - b1 - b2, b1, b2], 1), 0, 1)
        b /= b.sum(1, keepdims=True)
        n = (b[:, :, None] * self.cn[tri]).sum(1)
        return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20)

    def targets(self, co, alpha):
        """Facet point, Phong point and loc + alpha*(phong - loc) for every point."""
        loc, idx = self.nearest(co)
        ph = self.phong(loc, idx)
        return loc, ph, loc + alpha * (ph - loc), idx


def set_corner_normals(obj, n, split_cos=0.9998):
    """Per-corner custom normals. Blender averages custom normals inside a smooth fan, so every
    edge whose two faces want different normals at either end is marked sharp first (that is
    exactly where CE's own normals split), and sharp_face is cleared (smoothing comes from n)."""
    me = obj.data
    nl = len(me.loops)
    ls = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_start", ls)
    lt = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_total", lt)
    pidx = np.repeat(np.arange(len(lt)), lt)
    nxt = np.arange(nl) + 1
    nxt = np.where(nxt == (ls + lt)[pidx], ls[pidx], nxt)
    le = np.empty(nl, np.int32)
    me.loops.foreach_get("edge_index", le)
    order = np.argsort(le, kind="stable")
    es = le[order]
    pair = np.nonzero(es[1:] == es[:-1])[0]
    la, lb = order[pair], order[pair + 1]          # the two loops of a manifold edge
    # la runs v1->v2, lb runs v2->v1: compare at v1 (la vs nxt[lb]) and at v2 (nxt[la] vs lb)
    c1 = (n[la] * n[nxt[lb]]).sum(1)
    c2 = (n[nxt[la]] * n[lb]).sum(1)
    sharp = np.zeros(len(me.edges), bool)
    sharp[es[pair]] = (c1 < split_cos) | (c2 < split_cos)
    if "sharp_edge" not in me.attributes:
        me.attributes.new("sharp_edge", "BOOLEAN", "EDGE")
    me.attributes["sharp_edge"].data.foreach_set("value", sharp)
    if "sharp_face" in me.attributes:
        me.attributes["sharp_face"].data.foreach_set("value", np.zeros(len(me.polygons), bool))
    me.normals_split_custom_set([tuple(v) for v in n.astype(np.float32)])
    me.update()
    got = np.empty(nl * 3, np.float32)
    me.corner_normals.foreach_get("vector", got)
    err = np.degrees(np.arccos(np.clip((got.reshape(-1, 3) * n).sum(1), -1, 1)))
    return {"sharp_edges": int(sharp.sum()), "normal_err_deg_max": float(err.max()),
            "normal_err_deg_p99": float(np.percentile(err, 99))}


def band_restore(obj, band, alpha, owned=True):
    """Round the subdivided mesh inside the CE band: every vertex goes alpha of the way from its
    CE facet point toward the Phong surface through the CE vertices (alpha in [0, 1]).
    owned=True (SIMPLE subdivision): vertices already lie on the CE facets and every face knows
    its CE triangle (ce_tri), so the Phong offset is taken from the vertex's own CE triangles
    (averaged where it sits on a CE edge or vertex): no nearest-point search, no folds from a
    vertex snapping to the wrong triangle. owned=False: nearest point on the CE surface."""
    co = mesh_np(obj).astype(np.float64)
    if owned:
        me = obj.data
        ce = np.empty(len(me.polygons), np.int32)
        me.attributes["ce_tri"].data.foreach_get("value", ce)
        lt = np.empty(len(me.polygons), np.int32)
        me.polygons.foreach_get("loop_total", lt)
        lv = np.empty(len(me.loops), np.int32)
        me.loops.foreach_get("vertex_index", lv)
        tri = band.poly2tri[np.repeat(ce, lt)]
        p0 = band.v[band.t[tri, 0]]
        fnl = band.fn[tri]
        x = co[lv]
        x = x - ((x - p0) * fnl).sum(1, keepdims=True) * fnl          # on the facet plane already
        off_l = band.phong(x, tri) - x
        off = np.zeros_like(co)
        cnt = np.zeros(len(co))
        np.add.at(off, lv, off_l)
        np.add.at(cnt, lv, 1)
        off /= np.maximum(cnt, 1)[:, None]
        facet_err = float(np.abs(((co[lv] - p0) * fnl).sum(1)).max())
        nf = np.zeros_like(co)
        np.add.at(nf, lv, fnl)
        nf /= np.maximum(np.linalg.norm(nf, axis=1, keepdims=True), 1e-20)
        bulge = (off * nf).sum(1)
        tgt = co + alpha * off
    else:
        loc, ph, tgt, idx = band.targets(co, alpha)
        bulge = ((ph - loc) * band.fn[idx]).sum(1)
        facet_err = None
    moved = np.linalg.norm(tgt - co, axis=1)
    set_mesh_np(obj, tgt)
    return {"alpha": alpha, "mode": "owned CE triangle" if owned else "nearest CE point",
            "input_off_facet_max": facet_err,
            "moved_mean": float(moved.mean()), "moved_max": float(moved.max()),
            "band_width_mean": float(np.abs(bulge).mean()), "band_width_max": float(np.abs(bulge).max()),
            "outward_of_facet_max": float(max((alpha * bulge).max(), 0)),
            "inward_of_facet_max": float(max(-(alpha * bulge).min(), 0)),
            "ce_vertices": band.n_vertices, "ce_vertices_pinned": band.n_pinned,
            "pinned_corners": band.n_hard_corners}


def fold_check(obj, band):
    """Faces whose normal turned against (or far from) their CE triangle's normal."""
    me = obj.data
    fn = np.empty(len(me.polygons) * 3, np.float32)
    me.polygons.foreach_get("normal", fn)
    ce = np.empty(len(me.polygons), np.int32)
    me.attributes["ce_tri"].data.foreach_get("value", ce)
    d = (fn.reshape(-1, 3) * band.fn[band.poly2tri[ce]]).sum(1)
    return {"faces": len(d), "flipped": int((d < 0).sum()), "tilt_over_60deg": int((d < 0.5).sum()),
            "tilt_over_25deg": int((d < 0.906).sum())}


def band_check(obj, band, tol=2e-6):
    """Where each vertex sits in the band: t = 0 on the facet, 1 on the Phong curve.
    Outside the band = beyond the Phong curve or past the facet on the wrong side."""
    co = mesh_np(obj).astype(np.float64)
    loc, ph, _, idx = band.targets(co, 1.0)
    fn = band.fn[idx]
    w = ((ph - loc) * fn).sum(1)
    s = ((co - loc) * fn).sum(1)
    beyond_curve = np.where(w >= 0, s - np.maximum(w, 0), np.minimum(w, 0) - s)
    wrong_side = np.where(w >= 0, -s, s)
    off = np.maximum(beyond_curve, wrong_side)
    return {"outside_band_verts": int((off > tol).sum()), "outside_band_max": float(max(off.max(), 0)),
            "dist_to_facet_mean": float(np.abs(s).mean()), "dist_to_facet_max": float(np.abs(s).max())}


def signed_distances(obj, ref_tree):
    co = mesh_np(obj)
    s = np.empty(len(co), np.float32)
    for i, v in enumerate(co):
        loc, nrm, _, _ = ref_tree.find_nearest(Vector(v))
        s[i] = float((Vector(v) - loc) @ nrm)
    return s


def tree_of(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.normal_update()
    t = BVHTree.FromBMesh(bm)
    bm.free()
    return t


def mark_seams_from_uv(obj, layer):
    """Mark an edge as seam wherever the two faces sharing it disagree on its UVs."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    uvl = bm.loops.layers.uv[layer]
    n = 0
    for e in bm.edges:
        e.seam = False
        if len(e.link_loops) != 2:
            e.seam = True
            n += 1
            continue
        l1, l2 = e.link_loops
        # l1 runs v_a->v_b, l2 runs v_b->v_a
        a1, b1 = l1[uvl].uv, l1.link_loop_next[uvl].uv
        a2, b2 = l2.link_loop_next[uvl].uv, l2[uvl].uv
        if (a1 - a2).length > 1e-5 or (b1 - b2).length > 1e-5:
            e.seam = True
            n += 1
    bm.to_mesh(obj.data)
    bm.free()
    return n


# ---------------------------------------------------------------- UV metrics
def uv_stats(obj, layer, res=512):
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    uvl = bm.loops.layers.uv[layer]
    cov = np.zeros((res, res), np.uint16)
    area = 0.0
    for f in bm.faces:
        uv = np.array([l[uvl].uv[:] for l in f.loops]) * res
        a = 0.5 * ((uv[1, 0] - uv[0, 0]) * (uv[2, 1] - uv[0, 1]) - (uv[2, 0] - uv[0, 0]) * (uv[1, 1] - uv[0, 1]))
        area += abs(a) / (res * res)
        x0, y0 = np.floor(uv.min(0)).astype(int)
        x1, y1 = np.ceil(uv.max(0)).astype(int)
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, res - 1), min(y1, res - 1)
        if x1 < x0 or y1 < y0 or abs(a) < 1e-12:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        def e(p, q):
            return (q[0] - p[0]) * (ys - p[1]) - (q[1] - p[1]) * (xs - p[0])
        w0, w1, w2 = e(uv[1], uv[2]), e(uv[2], uv[0]), e(uv[0], uv[1])
        inside = ((w0 >= 0) & (w1 >= 0) & (w2 >= 0)) | ((w0 <= 0) & (w1 <= 0) & (w2 <= 0))
        cov[y0:y1 + 1, x0:x1 + 1] += inside.astype(np.uint16)
    bm.free()
    used = (cov > 0).mean()
    overl = (cov > 1).sum() / max((cov > 0).sum(), 1)
    return {"uv_area_sum": round(area, 4), "utilisation": round(float(used), 4),
            "overlap_fraction_of_used": round(float(overl), 4)}


# ---------------------------------------------------------------- JMS export
def build_jms(obj, template, uv_layer="UVMap"):
    """JMS 8200 from a rest-pose mesh, node block/materials/markers/regions from the template."""
    names = [n.name for n in template.nodes]
    idx = {n: i for i, n in enumerate(names)}
    vg_name = {g.index: g.name for g in obj.vertex_groups}
    mw = obj.matrix_world
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.calc_loop_triangles()
    me.update()
    uv = me.uv_layers[uv_layer].data
    nrm3 = mw.to_3x3().inverted().transposed()
    corner_n = me.corner_normals
    verts, key2i, tris = [], {}, []
    for poly in me.polygons:
        ids = []
        for li in poly.loop_indices:
            lp = me.loops[li]
            v = me.vertices[lp.vertex_index]
            gw = sorted(((g.weight, vg_name[g.group]) for g in v.groups if g.weight > 0 and vg_name.get(g.group) in idx), reverse=True)[:2]
            if not gw:
                gw = [(1.0, names[0])]
            tot = sum(w for w, _ in gw)
            n0 = idx[gw[0][1]]
            n1, w1 = (idx[gw[1][1]], gw[1][0] / tot) if len(gw) > 1 else (-1, 0.0)
            pw = mw @ v.co * 100.0
            nw = (nrm3 @ Vector(corner_n[li].vector)).normalized()
            u, vv = uv[li].uv
            key = (round(pw.x, 4), round(pw.y, 4), round(pw.z, 4), round(nw.x, 3), round(nw.y, 3), round(nw.z, 3),
                   round(u, 5), round(vv, 5), n0, n1, round(w1, 4))
            if key not in key2i:
                key2i[key] = len(verts)
                verts.append(hf.JmsVertex(n0, tuple(pw), tuple(nw), n1, w1, u, vv))
            ids.append(key2i[key])
        # JMS keeps the Xbox draw order: reverse of Blender winding
        tris.append(hf.JmsTriangle(0, poly.material_index, (ids[0], ids[2], ids[1])))
    bpy.data.meshes.remove(me)
    return hf.Jms(8200, template.checksum, template.nodes, template.materials, template.markers,
                  template.regions, verts, tris)


def compare_jms(a, b):
    """Semantic comparison of two JMS (vertex sets matched by position+uv)."""
    res = {"nodes_identical": [(n.name, n.first_child, n.next_sibling) for n in a.nodes] ==
           [(n.name, n.first_child, n.next_sibling) for n in b.nodes],
           "materials_identical": a.materials == b.materials, "regions_identical": a.regions == b.regions,
           "vertex_counts": [len(a.vertices), len(b.vertices)], "triangle_counts": [len(a.triangles), len(b.triangles)]}
    def k(v):
        return (round(v.pos[0], 2), round(v.pos[1], 2), round(v.pos[2], 2), round(v.u, 3), round(v.v, 3),
                round(v.normal[0], 1), round(v.normal[1], 1), round(v.normal[2], 1))
    def tri_set(j):
        out = set()
        for t in j.triangles:
            ks = [k(j.vertices[i]) for i in t.v]
            r = min(range(3), key=lambda i: ks[i])  # rotation-invariant, winding-sensitive
            out.add((t.shader,) + tuple(ks[(r + i) % 3] for i in range(3)))
        return out
    ta, tb = tri_set(a), tri_set(b)
    res["triangles_matched_fraction"] = round(len(ta & tb) / max(len(ta), 1), 4)
    va = {k(v): v for v in a.vertices}
    werr, nerr, nodes_same = 0.0, 0.0, 0
    common = 0
    for kk, v in ((kk, v) for kk, v in ((k(v), v) for v in b.vertices)):
        u = va.get(kk)
        if not u:
            continue
        common += 1
        same = (u.node0, u.node1) == (v.node0, v.node1)
        swapped = (u.node0, u.node1) == (v.node1, v.node0)
        nodes_same += same or swapped
        if same:
            werr = max(werr, abs(u.weight - v.weight))
        elif swapped:
            werr = max(werr, abs(u.weight - (1 - v.weight)))
        nerr = max(nerr, math.dist(u.normal, v.normal))
    res.update(vertices_matched=common, node_pairs_agree=nodes_same, max_weight_diff=round(werr, 5),
               max_normal_diff=round(nerr, 4))
    return res


# ---------------------------------------------------------------- rig-exact glTF export
def build_gltf(obj, template_path, out_path, textures, material_extras):
    """glTF that reuses the extracted file's node tree, skin and inverse bind matrices verbatim
    (so node names, order, hierarchy and bind pose are identical by construction) and swaps in
    the remodel's mesh. Mesh space = Halo model space (= Blender world), as in map_extract.
    TEXCOORD_0 = CE layout (Channel A), TEXCOORD_1 = unique unwrap (bakes).
    """
    import copy
    tg = json.load(open(template_path))
    tbin = open(os.path.join(os.path.dirname(template_path), tg["buffers"][0]["uri"]), "rb").read()
    skin = tg["skins"][0]
    joint_names = [tg["nodes"][j]["name"] for j in skin["joints"]]
    jidx = {n: i for i, n in enumerate(joint_names)}
    vg_name = {g.index: g.name for g in obj.vertex_groups}

    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.calc_tangents(uvmap="uv_unique")
    mw = obj.matrix_world
    r3 = mw.to_3x3().normalized()
    n3 = mw.to_3x3().inverted().transposed()
    uv0 = me.uv_layers["UVMap"].data
    uv1 = me.uv_layers["uv_unique"].data
    vweights = []
    for v in me.vertices:
        gw = sorted(((g.weight, jidx[vg_name[g.group]]) for g in v.groups
                     if g.weight > 0 and vg_name.get(g.group) in jidx), reverse=True)[:4]
        tot = sum(w for w, _ in gw) or 1.0
        js = [j for _, j in gw] + [0] * (4 - len(gw))
        ws = [w / tot for w, _ in gw] + [0.0] * (4 - len(gw))
        vweights.append((js, ws))
    prims = {}
    for poly in me.polygons:
        pr = prims.setdefault(poly.material_index, {"key": {}, "v": [], "i": []})
        for li in poly.loop_indices:
            lp = me.loops[li]
            v = me.vertices[lp.vertex_index]
            p = mw @ v.co
            n = (n3 @ lp.normal).normalized()
            t = (r3 @ lp.tangent).normalized()
            a, b = uv0[li].uv, uv1[li].uv
            key = (lp.vertex_index, round(n.x, 4), round(n.y, 4), round(n.z, 4), round(a.x, 6), round(a.y, 6),
                   round(b.x, 6), round(b.y, 6), round(t.x, 3), round(t.y, 3), lp.bitangent_sign)
            k = pr["key"].get(key)
            if k is None:
                k = len(pr["v"])
                pr["key"][key] = k
                js, ws = vweights[lp.vertex_index]
                pr["v"].append((tuple(p), tuple(n), (t.x, t.y, t.z, lp.bitangent_sign),
                                (a.x, 1.0 - a.y), (b.x, 1.0 - b.y), js, ws))
            pr["i"].append(k)
    bpy.data.meshes.remove(me)

    blob = bytearray()
    views, accs = [], []
    def add(arr, target, ctype, typ, minmax=False):
        nonlocal blob
        while len(blob) % 4:
            blob += b"\0"
        data = arr.tobytes()
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), **({"target": target} if target else {})})
        blob += data
        acc = {"bufferView": len(views) - 1, "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            acc["min"] = arr.min(0).tolist()
            acc["max"] = arr.max(0).tolist()
        accs.append(acc)
        return len(accs) - 1
    ibm = np.frombuffer(tbin, np.float32, count=16 * len(joint_names),
                        offset=tg["bufferViews"][tg["accessors"][skin["inverseBindMatrices"]]["bufferView"]].get("byteOffset", 0)
                        + tg["accessors"][skin["inverseBindMatrices"]].get("byteOffset", 0)).reshape(-1, 16).copy()
    ibm_acc = add(ibm, None, 5126, "MAT4")
    primitives = []
    mats = []
    for mi in sorted(prims):
        pr = prims[mi]
        V = pr["v"]
        a = {"POSITION": add(np.array([v[0] for v in V], np.float32), 34962, 5126, "VEC3", True),
             "NORMAL": add(np.array([v[1] for v in V], np.float32), 34962, 5126, "VEC3"),
             "TANGENT": add(np.array([v[2] for v in V], np.float32), 34962, 5126, "VEC4"),
             "TEXCOORD_0": add(np.array([v[3] for v in V], np.float32), 34962, 5126, "VEC2"),
             "TEXCOORD_1": add(np.array([v[4] for v in V], np.float32), 34962, 5126, "VEC2"),
             "JOINTS_0": add(np.array([v[5] for v in V], np.uint16), 34962, 5123, "VEC4"),
             "WEIGHTS_0": add(np.array([v[6] for v in V], np.float32), 34962, 5126, "VEC4")}
        ind = add(np.array(pr["i"], np.uint32), 34963, 5125, "SCALAR")
        primitives.append({"attributes": a, "indices": ind, "material": len(mats), "mode": 4})
        tm = tg["materials"][mi]
        mats.append({"name": tm["name"], "pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "texCoord": 1},
                     "metallicFactor": 0.0, "roughnessFactor": 1.0},
                     "normalTexture": {"index": 1, "texCoord": 1}, "occlusionTexture": {"index": 2, "texCoord": 1},
                     "extras": dict(copy.deepcopy(tm.get("extras", {})), **material_extras.get(mi, {}))})
    g = {"asset": {"version": "2.0", "generator": "PRISM pack tools fp_build.py (rig from map_extract)"},
         "scene": 0, "scenes": tg["scenes"], "nodes": copy.deepcopy(tg["nodes"]),
         "meshes": [{"name": "fp_arms_new", "primitives": primitives}], "materials": mats,
         "samplers": tg["samplers"], "images": [{"uri": u} for u in textures],
         "textures": [{"source": i, "sampler": 0} for i in range(len(textures))],
         "skins": [dict(skin, inverseBindMatrices=ibm_acc)],
         "extras": dict(tg.get("extras", {}), remodel="fp arms remodel (CE band-restored geometry, CE albedo 2x)", winding_flipped=False),
         "accessors": accs, "bufferViews": views,
         "buffers": [{"uri": os.path.basename(out_path)[:-5] + ".bin", "byteLength": len(blob)}]}
    for n in g["nodes"]:
        if "mesh" in n:
            n["mesh"] = 0
    with open(out_path[:-5] + ".bin", "wb") as f:
        f.write(blob)
    with open(out_path, "w") as f:
        json.dump(g, f, indent=1)
    return {"vertices": sum(len(p["v"]) for p in prims.values()),
            "triangles": sum(len(p["i"]) for p in prims.values()) // 3,
            "joint_order_same_as_extract": True, "max_influences_written": max(sum(1 for w in vw[1] if w > 0) for vw in vweights)}


# ---------------------------------------------------------------- bake helpers
def bake_material(obj, image, uv_name):
    """Give every material slot of obj an active image node on uv_name (bake target)."""
    for slot in obj.material_slots:
        m = slot.material
        m.use_nodes = True
        nt = m.node_tree
        for n in [n for n in nt.nodes if n.name in ("BAKE_TARGET", "BAKE_UV")]:
            nt.nodes.remove(n)
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.name = "BAKE_UV"
        uvn.uv_map = uv_name
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.name = "BAKE_TARGET"
        tex.image = image
        nt.links.new(uvn.outputs["UV"], tex.inputs["Vector"])
        for n in nt.nodes:
            n.select = False
        tex.select = True
        nt.nodes.active = tex


def new_image(name, size, non_color, float_buf=False):
    if name in bpy.data.images:
        bpy.data.images.remove(bpy.data.images[name])
    img = bpy.data.images.new(name, size, size, alpha=False, float_buffer=float_buf)
    img.colorspace_settings.name = "Non-Color" if non_color else "sRGB"
    return img


def save_image(img, path, depth="8"):
    """Raw pixel save (no view transform, so Non-Color data stays linear)."""
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()


def main():
    cfg = U.load_config(U.script_args()[0])
    out = P(cfg["out_dir"])
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    os.makedirs(os.path.join(out, "export"), exist_ok=True)
    U.reset_scene()
    sc = bpy.context.scene

    # 1. import + bind check
    arm, ref = U.import_ref_gltf(P(cfg["ref_gltf"]), cfg["keep_lod"])
    arm.name = "fp_rig"
    ref.name = "fp_ref"
    tmpl = hf.read_jms(P(cfg["ref_jms"]))
    rest = U.jms_rest_world(tmpl)
    corr, bind_err = U.bone_corrections(arm, rest)
    bone_names = [b.name for b in arm.data.bones]
    step("import", bones=len(bone_names), jms_nodes=len(tmpl.nodes), bind_pos_err=bind_err,
         ref_verts=len(ref.data.vertices), ref_tris=len(ref.data.polygons),
         names_match=set(bone_names) == {n.name for n in tmpl.nodes})

    # JMS writer self-test on the CE mesh
    jref = build_jms(ref, tmpl)
    hf.write_jms(os.path.join(out, "export", "selftest_ref_rewrite.jms"), jref)
    step("jms_writer_selftest", **compare_jms(tmpl, jref))

    # 2. weld (every face keeps the index of its CE triangle as face attribute ce_tri)
    assert all(len(pg.vertices) == 3 for pg in ref.data.polygons)
    if "ce_tri" not in ref.data.attributes:
        ref.data.attributes.new("ce_tri", "INT", "FACE")
    ref.data.attributes["ce_tri"].data.foreach_set("value", np.arange(len(ref.data.polygons), dtype=np.int32))
    weld = dup(ref, "fp_ref_weld")
    weld.modifiers.clear()
    bm = bmesh.new()
    bm.from_mesh(weld.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=cfg["weld_distance"])
    bm.to_mesh(weld.data)
    bm.free()
    if weld.data.has_custom_normals:
        activate(weld)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    weld.hide_render = True
    ref_tree = tree_of(weld)
    step("weld", verts=len(weld.data.vertices))

    # 3. game mesh
    new = dup(weld, "fp_new")
    new.hide_render = False
    new.vertex_groups.clear()
    activate(new)
    m = new.modifiers.new("subd", "SUBSURF")
    m.subdivision_type = cfg.get("subdiv_type", "CATMULL_CLARK")
    m.levels = m.render_levels = cfg["subdiv_levels"]
    m.uv_smooth = "NONE" if m.subdivision_type == "SIMPLE" else "PRESERVE_BOUNDARIES"
    m.boundary_smooth = "PRESERVE_CORNERS"
    bpy.ops.object.modifier_apply(modifier=m.name)
    s_before = signed_distances(new, ref_tree)
    band = CEBand(ref, cfg.get("band_max_lean_deg", 60.0), cfg.get("band_smooth_deg", 5.0))
    alpha = cfg["band_alpha"]
    owned = cfg.get("subdiv_type", "CATMULL_CLARK") == "SIMPLE"
    new_facet_co = mesh_np(new).copy()      # on the CE facets: the high-poly starts from these
    restore = band_restore(new, band, alpha, owned)
    restore["folds"] = fold_check(new, band)
    s_after = signed_distances(new, ref_tree)
    ce_n = band.corner_normals(new)
    nrm_info = set_corner_normals(new, ce_n)
    step("game_mesh", shading_normals=dict(nrm_info, source="CE corner normals interpolated per face corner"), verts=len(new.data.vertices), quads=len(new.data.polygons),
         tris=sum(len(p.vertices) - 2 for p in new.data.polygons),
         subdiv_shrink_mean=float(-s_before.mean()), subdiv_shrink_max=float(-s_before.min()),
         outside_before=int((s_before > 1e-6).sum()), outside_max_before=float(max(s_before.max(), 0)),
         restore=restore, band=band_check(new, band),
         signed_to_facet_mean_after=float(s_after.mean()), signed_to_facet_min_after=float(s_after.min()),
         signed_to_facet_max_after=float(s_after.max()))

    # 4. weights: Data Transfer from the CE mesh onto the same rig
    for n in tmpl.nodes:  # JMS node order
        new.vertex_groups.new(name=n.name)
    activate(new)
    dt = new.modifiers.new("wt", "DATA_TRANSFER")
    dt.object = ref
    dt.use_object_transform = True
    dt.use_vert_data = True
    dt.data_types_verts = {"VGROUP_WEIGHTS"}
    dt.vert_mapping = "POLYINTERP_NEAREST"
    dt.layers_vgroup_select_src = "ALL"
    dt.layers_vgroup_select_dst = "NAME"
    bpy.ops.object.modifier_apply(modifier=dt.name)
    bpy.ops.object.vertex_group_clean(group_select_mode="ALL", limit=0.001)
    bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=cfg["max_influences"])
    bpy.ops.object.vertex_group_normalize_all(group_select_mode="ALL", lock_active=False)
    infl = [len([g for g in v.groups if g.weight > 0]) for v in new.data.vertices]
    sums = [sum(g.weight for g in v.groups) for v in new.data.vertices]
    new.parent = arm
    new.matrix_parent_inverse = ref.matrix_parent_inverse.copy()
    am = new.modifiers.new("Armature", "ARMATURE")
    am.object = arm
    step("weights", max_influences=max(infl), unweighted=int(sum(1 for i in infl if i == 0)),
         two_bone_verts=int(sum(1 for i in infl if i == 2)),
         max_sum_err=float(max(abs(s - 1) for s in sums)),
         groups_not_bones=[g.name for g in new.vertex_groups if g.name not in bone_names],
         bones_now=len(arm.data.bones), bone_names_unchanged=[b.name for b in arm.data.bones] == bone_names)

    # 5. UVs
    me = new.data
    me.uv_layers["UVMap"].active = True
    uvu = me.uv_layers.new(name="uv_unique")
    activate(new)
    n_seams = mark_seams_from_uv(new, "UVMap")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    me.uv_layers.active = me.uv_layers["uv_unique"]
    bpy.ops.uv.unwrap(method="ANGLE_BASED", margin=0.002)
    bpy.ops.uv.pack_islands(rotate=True, margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    me.uv_layers["UVMap"].active_render = True
    step("uv", ce=uv_stats(new, "UVMap"), unique=uv_stats(new, "uv_unique"),
         seams=n_seams)

    # 6. high-poly
    high = dup(new, "fp_high")
    high.parent = None
    high.matrix_world = new.matrix_world.copy()
    high.modifiers.clear()
    high.vertex_groups.clear()
    if owned:
        set_mesh_np(high, new_facet_co)     # subdivide the facet version, then restore the same way
    activate(high)
    if cfg["high_extra_levels"] > 0:
        m = high.modifiers.new("subd", "SUBSURF")
        m.subdivision_type = cfg.get("subdiv_type", "CATMULL_CLARK")
        m.levels = m.render_levels = cfg["high_extra_levels"]
        m.uv_smooth = "NONE" if m.subdivision_type == "SIMPLE" else "PRESERVE_BOUNDARIES"
        bpy.ops.object.modifier_apply(modifier=m.name)
    if high.data.has_custom_normals:
        activate(high)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    h_restore = band_restore(high, band, alpha, owned)   # same band surface as the game mesh
    h_restore["folds"] = fold_check(high, band)
    h_ce_n = band.corner_normals(high)
    co = mesh_np(high)
    hn = np.empty(len(co) * 3, np.float32)
    high.data.vertices.foreach_get("normal", hn)
    hn = hn.reshape(-1, 3)
    frac = vertex_material_frac(high, 0)  # material 0 = rubber hands
    h = detail_height(co, frac, cfg["detail"])
    set_mesh_np(high, co + hn * h[:, None])     # inward only (h <= 0)
    hn2 = np.empty(len(co) * 3, np.float32)
    high.data.vertices.foreach_get("normal", hn2)
    hn2 = hn2.reshape(-1, 3)
    # high-poly shading = CE's smooth normal field (as on the game mesh) plus only the detail's
    # perturbation, so the tangent bake against the CE-normal game mesh carries just the detail
    hlv = np.empty(len(high.data.loops), np.int32)
    high.data.loops.foreach_get("vertex_index", hlv)
    hcn = h_ce_n + (hn2 - hn)[hlv]
    hcn /= np.maximum(np.linalg.norm(hcn, axis=1, keepdims=True), 1e-20)
    h_nrm_info = set_corner_normals(high, hcn, split_cos=0.98)
    hs = signed_distances(high, ref_tree)
    step("high_poly", verts=len(high.data.vertices), tris=2 * len(high.data.polygons),
         disp_min=float(h.min()), disp_mean=float(h.mean()), disp_max=float(h.max()), restore=h_restore,
         shading_normals=h_nrm_info,
         band=band_check(high, band), outside_ce_facet_verts=int((hs > 1e-6).sum()),
         outside_ce_facet_max=float(max(hs.max(), 0)))

    # 7. bakes (Cycles, one GPU)
    dev = U.set_cycles_gpu(sc, cfg["gpu"], cfg["bake_samples"])
    for slot in new.material_slots:  # own materials, the reference keeps the CE ones
        slot.material = slot.material.copy()
        slot.material.name = "fp_new_" + slot.material.name.split("\\")[-1].replace(" ", "_")
    size = cfg["bake_size"]
    bk = sc.render.bake
    bk.use_selected_to_active = True
    bk.cage_extrusion = cfg["bake_cage_extrusion"]
    bk.max_ray_distance = cfg["bake_max_ray"]
    bk.margin = 16
    bk.margin_type = "EXTEND"
    times = {}
    # explicit cage: the game mesh pushed out along its GEOMETRIC vertex normals. The game mesh
    # shades with CE's smooth normals, which on thin rims lean far from the faces; Cycles would
    # cast along those and hit the wrong surface. The cage keeps rays perpendicular to the surface.
    cage = dup(new, "fp_cage")
    cage.parent = None
    cage.matrix_world = new.matrix_world.copy()
    cage.modifiers.clear()
    cage.vertex_groups.clear()
    if cage.data.has_custom_normals:
        activate(cage)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    cco = mesh_np(cage)
    cvn = np.empty(len(cco) * 3, np.float32)
    cage.data.vertices.foreach_get("normal", cvn)
    set_mesh_np(cage, cco + cvn.reshape(-1, 3) * cfg["bake_cage_extrusion"])
    cage.hide_render = True
    bk.use_cage = True
    bk.cage_object = cage
    ref.hide_render = True
    high.hide_render = False
    for kind in ("NORMAL", "AO"):
        img = new_image("fp_" + kind.lower(), size, True)
        bake_material(new, img, "uv_unique")
        activate(new, [high])
        if kind == "NORMAL":
            bk.normal_space = "TANGENT"
        t = time.time()
        bpy.ops.object.bake(type=kind, use_selected_to_active=True, use_cage=True, cage_object=cage.name,
                            max_ray_distance=bk.max_ray_distance, margin=16, uv_layer="uv_unique")
        times[kind] = round(time.time() - t, 1)
        save_image(img, os.path.join(out, "textures", "fp_new_%s.png" % kind.lower()),
                   "16" if kind == "NORMAL" else "8")
    # cavity: Cycles pointiness of the high-poly (curvature incl. the microdetail), emission bake
    cav_mat = bpy.data.materials.new("bake_cavity")
    cav_mat.use_nodes = True
    cnt = cav_mat.node_tree
    cnt.nodes.clear()
    geo = cnt.nodes.new("ShaderNodeNewGeometry")
    em = cnt.nodes.new("ShaderNodeEmission")
    co_ = cnt.nodes.new("ShaderNodeOutputMaterial")
    cnt.links.new(geo.outputs["Pointiness"], em.inputs["Color"])
    cnt.links.new(em.outputs["Emission"], co_.inputs["Surface"])
    high.data.materials.clear()        # high shares the CE materials by reference: detach them
    high.data.materials.append(cav_mat)
    img = new_image("fp_cavity_raw", size, True, float_buf=True)
    bake_material(new, img, "uv_unique")
    activate(new, [high])
    t = time.time()
    bpy.ops.object.bake(type="EMIT", use_selected_to_active=True, use_cage=True, cage_object=cage.name,
                        max_ray_distance=bk.max_ray_distance, margin=16, uv_layer="uv_unique")
    times["CAVITY"] = round(time.time() - t, 1)
    px = np.empty(size * size * 4, np.float32)
    img.pixels.foreach_get(px)
    pt = px.reshape(-1, 4)[:, 0]
    covered = pt > 1e-4
    med = float(np.median(pt[covered]))
    p01, p99 = np.percentile(pt[covered], [1, 99])
    gain = 0.4 / max(med - p01, p99 - med, 1e-6)      # p1/p99 -> about 0.1/0.9, neutral 0.5
    cav = np.where(covered, np.clip(0.5 + (pt - med) * gain, 0, 1), 0.5)
    cimg = new_image("fp_cavity", size, True)
    rgba = np.ones((len(cav), 4), np.float32)
    rgba[:, :3] = cav[:, None]
    cimg.pixels.foreach_set(rgba.ravel())
    save_image(cimg, os.path.join(out, "textures", "fp_new_cavity.png"))
    cavity_stats = {"pointiness_median": med, "pointiness_p01": float(p01), "pointiness_p99": float(p99),
                    "gain": gain, "covered": float(covered.mean())}

    # CE albedo reprojected onto the unique UVs from the 2x upscaled CE base map. No ray casting:
    # the game mesh carries CE's own UVs (UVMap, linear through SIMPLE subdivision), so every texel
    # reads exactly the CE texel of its CE triangle (no cross-island hits at sharp edges).
    base2x = cfg.get("albedo_base_2x")
    base_img = bpy.data.images.load(P(base2x), check_existing=True) if base2x else \
        next(n.image for n in ref.material_slots[0].material.node_tree.nodes if n.type == "TEX_IMAGE")
    base_img.colorspace_settings.name = "sRGB"
    alb_mat = bpy.data.materials.new("bake_albedo")
    alb_mat.use_nodes = True
    ant = alb_mat.node_tree
    ant.nodes.clear()
    auv = ant.nodes.new("ShaderNodeUVMap")
    auv.uv_map = "UVMap"
    atx = ant.nodes.new("ShaderNodeTexImage")
    atx.image = base_img
    atx.interpolation = "Linear"
    atx.extension = "EXTEND"
    aem = ant.nodes.new("ShaderNodeEmission")
    aout = ant.nodes.new("ShaderNodeOutputMaterial")
    ant.links.new(auv.outputs["UV"], atx.inputs["Vector"])
    ant.links.new(atx.outputs["Color"], aem.inputs["Color"])
    ant.links.new(aem.outputs["Emission"], aout.inputs["Surface"])
    keep = [slot.material for slot in new.material_slots]
    tmp = []
    for slot in new.material_slots:
        m2 = alb_mat.copy()
        slot.material = m2
        tmp.append(m2)
    img = new_image("fp_albedo_ce", size, False)
    bake_material(new, img, "uv_unique")
    activate(new)
    t = time.time()
    bpy.ops.object.bake(type="EMIT", use_selected_to_active=False, margin=16, uv_layer="uv_unique")
    times["ALBEDO_CE"] = round(time.time() - t, 1)
    save_image(img, os.path.join(out, "textures", "fp_new_albedo_ce.png"))
    for slot, m0 in zip(new.material_slots, keep):
        slot.material = m0
    for m2 in tmp:
        bpy.data.materials.remove(m2)
    ref.hide_render = False

    # microtexture class mask on uv_unique: R glove, G undersuit, B plate (A = 1).
    # rubber hands faces: glove = skin weight on the wrist and finger bones, undersuit = the rest
    # (forearm, upper arm); armor hands faces: plate. (S + P rule, P from the rig).
    glove_bones = {g.index for g in new.vertex_groups if g.name.split(" ", 2)[-1].split(" ")[0] in
                   ("wriste", "index", "middle", "middlelow", "pinky", "ring", "thumb")}
    me = new.data
    gw = np.zeros(len(me.vertices), np.float32)
    for v in me.vertices:
        gw[v.index] = sum(g.weight for g in v.groups if g.group in glove_bones)
    gw = smoothstep(0.35, 0.65, gw)
    lv = np.empty(len(me.loops), np.int32)
    me.loops.foreach_get("vertex_index", lv)
    lt = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_total", lt)
    mi = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("material_index", mi)
    rub = np.repeat(mi == 0, lt)
    col = np.zeros((len(me.loops), 4), np.float32)
    col[:, 0] = np.where(rub, gw[lv], 0)
    col[:, 1] = np.where(rub, 1 - gw[lv], 0)
    col[:, 2] = np.where(rub, 0, 1)
    col[:, 3] = 1
    if "micro_class" in me.color_attributes:
        me.color_attributes.remove(me.color_attributes["micro_class"])
    ca = me.color_attributes.new("micro_class", "FLOAT_COLOR", "CORNER")
    ca.data.foreach_set("color", col.ravel())
    cls_mat = bpy.data.materials.new("bake_class")
    cls_mat.use_nodes = True
    knt = cls_mat.node_tree
    knt.nodes.clear()
    at = knt.nodes.new("ShaderNodeAttribute")
    at.attribute_name = "micro_class"
    em = knt.nodes.new("ShaderNodeEmission")
    ko = knt.nodes.new("ShaderNodeOutputMaterial")
    knt.links.new(at.outputs["Color"], em.inputs["Color"])
    knt.links.new(em.outputs["Emission"], ko.inputs["Surface"])
    keep = [slot.material for slot in new.material_slots]
    tmp = []
    for slot in new.material_slots:
        m2 = cls_mat.copy()
        slot.material = m2
        tmp.append(m2)
    img = new_image("fp_microclass", size, True)
    bake_material(new, img, "uv_unique")
    activate(new)
    bpy.ops.object.bake(type="EMIT", use_selected_to_active=False, margin=16, uv_layer="uv_unique")
    save_image(img, os.path.join(out, "textures", "fp_new_microclass.png"))
    for slot, m0 in zip(new.material_slots, keep):
        slot.material = m0
    for m2 in tmp:
        bpy.data.materials.remove(m2)
    rub_v = np.zeros(len(me.vertices), bool)
    rub_v[lv[rub]] = True
    class_stats = {"glove_bones": sorted(new.vertex_groups[i].name for i in glove_bones),
                   "rubber_verts": int(rub_v.sum()), "glove_verts": int((gw[rub_v] > 0.5).sum()),
                   "undersuit_verts": int((gw[rub_v] <= 0.5).sum())}
    # micro tile repeat on uv_unique: uv units per world unit (uniform pack scale)
    me.calc_loop_triangles()
    uvd = me.uv_layers["uv_unique"].data
    a3 = a2 = 0.0
    for lt_ in me.loop_triangles:
        p_ = [me.vertices[i].co for i in lt_.vertices]
        q_ = [uvd[i].uv for i in lt_.loops]
        a3 += ((p_[1] - p_[0]).cross(p_[2] - p_[0])).length / 2
        a2 += abs((q_[1] - q_[0]).cross(q_[2] - q_[0])) / 2
    uv_per_wu = math.sqrt(a2 / a3)
    class_stats["uv_unique_per_world_unit"] = uv_per_wu

    # normal map validity
    nimg = bpy.data.images["fp_normal"]
    px = np.empty(size * size * 4, np.float32)
    nimg.pixels.foreach_get(px)
    px = px.reshape(-1, 4)[:, :3] * 2 - 1
    ln = np.linalg.norm(px, axis=1)
    flat = (np.abs(px[:, 0]) < 0.004) & (np.abs(px[:, 1]) < 0.004)
    step("bake", device=dev, size=size, seconds=times, bake_shading="CE custom normals on both (high: + detail perturbation)", albedo_source=base2x or "CE base map 1x",
         cavity=cavity_stats, micro_class=class_stats,
         normal_len_mean=float(ln.mean()), normal_len_p01=float(np.percentile(ln, 1)),
         normal_len_p99=float(np.percentile(ln, 99)), normal_z_negative=float((px[:, 2] < 0).mean()),
         normal_flat_fraction=float(flat.mean()))

    # final materials for viewing: baked albedo * AO, baked normal on uv_unique
    for slot in new.material_slots:
        mat = slot.material
        nt = mat.node_tree
        nt.nodes.clear()
        o = nt.nodes.new("ShaderNodeOutputMaterial")
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.uv_map = "uv_unique"
        ta = nt.nodes.new("ShaderNodeTexImage"); ta.image = bpy.data.images["fp_albedo_ce"]; ta.name = "albedo"
        to = nt.nodes.new("ShaderNodeTexImage"); to.image = bpy.data.images["fp_ao"]; to.name = "ao"
        tn = nt.nodes.new("ShaderNodeTexImage"); tn.image = bpy.data.images["fp_normal"]; tn.name = "normal"
        mul = nt.nodes.new("ShaderNodeMix"); mul.data_type = "RGBA"; mul.blend_type = "MULTIPLY"; mul.name = "ao_mul"
        mul.inputs["Factor"].default_value = 1.0
        nm = nt.nodes.new("ShaderNodeNormalMap"); nm.uv_map = "uv_unique"; nm.name = "normal_map"
        for t_ in (ta, to, tn):
            nt.links.new(uvn.outputs["UV"], t_.inputs["Vector"])
        nt.links.new(ta.outputs["Color"], mul.inputs["A"])
        nt.links.new(to.outputs["Color"], mul.inputs["B"])
        nt.links.new(mul.outputs["Result"], bsdf.inputs["Base Color"])
        nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
        bsdf.inputs["Roughness"].default_value = 0.55 if "rubber" in mat.name else 0.35
        bsdf.inputs["Metallic"].default_value = 0.0 if "rubber" in mat.name else 0.6
        nt.links.new(bsdf.outputs["BSDF"], o.inputs["Surface"])

    # 8. export
    exp = os.path.join(out, "export")
    jnew = build_jms(new, tmpl)
    hf.write_jms(os.path.join(exp, "fp_arms_new.jms"), jnew)
    jback = hf.read_jms(os.path.join(exp, "fp_arms_new.jms"))
    w_sum_ok = all(0.0 <= v.weight <= 1.0 for v in jback.vertices)
    step("export_jms", verts=len(jback.vertices), tris=len(jback.triangles),
         nodes_identical_to_extract=[(n.name, n.first_child, n.next_sibling, n.rot, n.pos) for n in jback.nodes] ==
         [(n.name, n.first_child, n.next_sibling, n.rot, n.pos) for n in tmpl.nodes],
         checksum=jback.checksum, weights_in_range=w_sum_ok,
         two_node_verts=sum(1 for v in jback.vertices if v.node1 >= 0))
    gl = os.path.join(exp, "fp_arms_new.gltf")
    mdir = os.path.relpath(P(cfg["microtex"]["dir"]), exp)
    def tile(cls):
        meta = json.load(open(os.path.join(P(cfg["microtex"]["dir"]), cls + ".json")))
        return {"class": cls, "normal": "%s/%s_N.png" % (mdir, cls), "rough_cavity": "%s/%s_R.png" % (mdir, cls),
                "tile_world_units": meta["world_size_m"] / 3.048,
                "uv_unique_repeat": class_stats["uv_unique_per_world_unit"] * meta["world_size_m"] / 3.048}
    micro_common = {"cavity_texture": 3, "micro_class_texture": 4, "micro_class_channels": "R glove, G undersuit, B plate",
                    "ce_layout_base_2x": os.path.relpath(P(base2x), exp) if base2x else None}
    info = build_gltf(new, P(cfg["ref_gltf"]), gl,
                      ["../textures/fp_new_albedo_ce.png", "../textures/fp_new_normal.png",
                       "../textures/fp_new_ao.png", "../textures/fp_new_cavity.png",
                       "../textures/fp_new_microclass.png"],
                      {0: dict(micro_common, microtexture_class="mjolnir_glove+mjolnir_undersuit",
                               microtexture=[tile("mjolnir_glove"), tile("mjolnir_undersuit")]),
                       1: dict(micro_common, microtexture_class="mjolnir_plate")})
    step("export_gltf", path=gl, **info)

    # 9. final geometry checks and save
    mw = new.matrix_world
    def bbox(o):
        pts = np.array([list(o.matrix_world @ v.co) for v in o.data.vertices])
        return pts.min(0), pts.max(0)
    rb0, rb1 = bbox(weld)
    nb0, nb1 = bbox(new)
    c = (rb0 + rb1) / 2
    r_ref = float(np.linalg.norm(np.array([list(weld.matrix_world @ v.co) for v in weld.data.vertices]) - c, axis=1).max())
    r_new = float(np.linalg.norm(np.array([list(mw @ v.co) for v in new.data.vertices]) - c, axis=1).max())
    step("envelope_checks", bbox_inside=bool((nb0 >= rb0 - 1e-6).all() and (nb1 <= rb1 + 1e-6).all()),
         bbox_delta_min=(nb0 - rb0).tolist(), bbox_delta_max=(nb1 - rb1).tolist(),
         bound_radius_ref=r_ref, bound_radius_new=r_new, bound_sphere_ratio=r_new / r_ref)
    REPORT["counts"] = {"ce_tris": len(ref.data.polygons), "ce_verts_welded": len(weld.data.vertices),
                        "new_tris": sum(len(p.vertices) - 2 for p in new.data.polygons),
                        "new_verts": len(new.data.vertices), "high_tris": 2 * len(high.data.polygons)}
    REPORT["seconds_total"] = round(time.time() - T0, 1)
    with open(os.path.join(out, "build_report.json"), "w") as f:
        json.dump(REPORT, f, indent=1, default=float)
    for img in bpy.data.images:
        if img.name.startswith("fp_") and not img.name.endswith("_raw"):
            img.filepath = os.path.join(out, "textures", "fp_new_%s.png" % img.name[3:].replace("albedo_ce", "albedo_ce"))
            img.source = "FILE"
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "fp_build.blend"))
    print("BUILD_OK", REPORT["seconds_total"])


if __name__ == "__main__":
    main()
