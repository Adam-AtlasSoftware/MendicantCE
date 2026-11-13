"""PRISM v2 round 2 hero remodel: crisp terraces instead of rounding (headless Blender).

  blender -b --factory-startup --python art/hero_remodel2.py -- <recipe.json> <height2 dir> <out dir>

Round 1 (hero_remodel.py) rounded every CE facet toward a Phong surface and moved vertices by a blended
height field: it read as "CE, smoothed" (soft plates, a shallow 0.9 cm relief, ragged plate borders on UV
seams, a lumpy belt, and the collar pushed up into the helmet). Round 2 per level of detail:

  1. CE's superhigh mesh welded (the effect shells kept aside, as round 1); CE's hard creases inside the
     armour chamfered (bmesh bevel, recipe crease_bevel_mm): the small flat bevel that carries a highlight;
  2. SIMPLE subdivision, then refinement driven by the terrace layers (hero_height2.py's signed distances in
     mm): an edge is split near a layer's contour, or where the distance is not linear along it (a feature
     smaller than the edge), down to the level's near_len_mm;
  3. per vertex distance: the mean over its UV corners, except where the corners of a UV seam disagree in
     sign (the two islands' paint does not meet): there the vertex sits on the contour, so a plate border on
     a seam runs straight along CE's own edge instead of zig-zagging (round 1's ragged groin, thighs and
     boot tops); the armour's distance is relaxed a few steps along the surface near its contour;
  4. contour loops: for every layer and every break of its profile (wall foot, wall top, chamfer top) a
     vertex loop is cut exactly on the level line (edges split at the crossing, faces split along it);
  5. rounding only on the undersuit (CE band, recipe suit_alpha), never on armour: CE's flat planes and hard
     creases stay;
  6. heights: base_cm everywhere (the undersuit set back) plus each layer's profile: flat top, a 45 degree
     chamfer, a steep wall (plates 2.3 cm proud of the suit: a real plate thickness); vents, slots and
     grooves cut, bolts raised. Moves go along a smoothed normal; inward moves are capped by the local
     thickness, outward moves by the clearance in front of the vertex (so the collar cannot rise into the
     helmet, nor a plate into its neighbour);
  7. tubes (recipe tube3d): cable bundles laid on the finished surface (paths picked on hero_pick.py's views),
     skinned and textured from the surface point under them;
  8. shading: split above split_deg, face-area weighted normals (flat plates stay flat, chamfers and walls
     carry the light); weights: CE's own at each vertex's point of its CE triangle (2 per vertex);
  9. lower LODs collapse-decimated from the high level.
High-poly for the bakes: the superhigh level (without tubes) subdivided and moved by hero_height2's detail
relief (CE's painted seams, studs, folds and ribs), plus lod_sh_bake (superhigh without tubes) as the target.

Outputs (local only: CE vertices and pixels): <out>/export/__base <lod>.jms, <out>/remodel.blend
(rig, ce_ref, lod_*, lod_sh_bake, highpoly), <out>/remodel_report.json.
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
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402
import halo_formats as hf  # noqa: E402
import fp_build as FB  # noqa: E402
import hero_remodel as H1  # noqa: E402
import mesh_hygiene as MH  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()
REPORT = {"steps": {}}
MM_WU = 1.0 / 3048.0
CM_WU = 10.0 * MM_WU
MOVES = {}
DECIMATED_FROM = {}


def P(rel):
    return E.path(rel)


def step(name, **kv):
    REPORT["steps"][name] = dict(kv, t=round(time.time() - T0, 1))
    print("STEP", name, json.dumps(kv, default=float)[:1200], flush=True)


# ------------------------------------------------------------------ fields
class Field:
    def __init__(self, path):
        self.a = np.load(path).astype(np.float32)
        self.n = self.a.shape[0]

    def sample(self, uv):
        n = self.n
        x = np.clip(uv[:, 0] * n - 0.5, 0, n - 1.001)
        y = np.clip((1 - uv[:, 1]) * n - 0.5, 0, n - 1.001)
        x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
        fx, fy = x - x0, y - y0
        a = self.a
        return (a[y0, x0] * (1 - fx) * (1 - fy) + a[y0, x0 + 1] * fx * (1 - fy)
                + a[y0 + 1, x0] * (1 - fx) * fy + a[y0 + 1, x0 + 1] * fx * fy)


def layer_levels(L):
    w, b = L["wall_mm"], L["chamfer_mm"]
    return [-w, 0.0, b] if L["kind"] == "raise" else [w, 0.0, -b]


def profile(L, s):
    """the layer's height contribution (cm) at signed distance s (mm, + inside the shape)"""
    d = L["depth_cm"]
    c = min(L["chamfer_mm"] / 10.0, 0.5 * d)
    w, b = L["wall_mm"], L["chamfer_mm"]
    x = s if L["kind"] == "raise" else -s
    top = 1.0 - c / d
    p = np.where(x <= -w, 0.0,
                 np.where(x < 0, (x + w) / w * top,
                          np.where(x < b, top + (x / b) * (1 - top), 1.0)))
    return d * p if L["kind"] == "raise" else -d * (1.0 - p)


def corner_fields(bm, uvl, fields, detail_mats):
    """per vertex distance for every layer: corner mean, or 0 where a seam's corners disagree in sign;
    also a flag: the vertex has an armour corner"""
    nv = len(bm.verts)
    lv, luv, lm = [], [], []
    for f in bm.faces:
        for l in f.loops:
            lv.append(l.vert.index)
            luv.append(l[uvl].uv[:])
            lm.append(f.material_index)
    lv = np.array(lv)
    luv = np.array(luv, np.float64)
    use = np.isin(np.array(lm), list(detail_mats))
    has = np.zeros(nv, bool)
    has[lv[use]] = True
    out = {}
    for name, F in fields.items():
        v = F.sample(luv[use]).astype(np.float64)
        idx = lv[use]
        acc = np.zeros(nv)
        cnt = np.zeros(nv)
        mn = np.full(nv, np.inf)
        mx = np.full(nv, -np.inf)
        np.add.at(acc, idx, v)
        np.add.at(cnt, idx, 1)
        np.minimum.at(mn, idx, v)
        np.maximum.at(mx, idx, v)
        val = np.where(cnt > 0, acc / np.maximum(cnt, 1), -40.0)
        val = np.where((mn < 0) & (mx > 0), 0.0, val)
        out[name] = val
    return out, has


def edge_mid_fields(bm, uvl, fields, detail_mats):
    """per edge: the distance at the midpoint of the edge's UV segment (first armour loop of the edge)"""
    ne = len(bm.edges)
    ei, a, b = [], [], []
    for e in bm.edges:
        for l in e.link_loops:
            if l.face.material_index in detail_mats:
                ei.append(e.index)
                a.append(l[uvl].uv[:])
                b.append(l.link_loop_next[uvl].uv[:])
                break
    ei = np.array(ei, np.int64)
    if not len(ei):
        return {k: np.full(ne, np.nan) for k in fields}
    m = 0.5 * (np.array(a) + np.array(b))
    q1 = 0.75 * np.array(a) + 0.25 * np.array(b)
    q3 = 0.25 * np.array(a) + 0.75 * np.array(b)
    out = {}
    for name, F in fields.items():
        r = np.full((ne, 3), np.nan)
        r[ei, 0] = F.sample(q1)
        r[ei, 1] = F.sample(m)
        r[ei, 2] = F.sample(q3)
        out[name] = r
    return out


def refine_fields(obj, fields, layers, lv, detail_mats):
    """split edges near the layers' contours (down to near_len_mm), where the distance is not linear
    along the edge (a feature between the vertices), and any edge over far_len_mm"""
    me = obj.data
    near = lv["near_len_mm"]
    far = lv.get("far_len_mm", 1e9)
    tol = lv["field_tol_mm"]
    total = 0
    for it in range(lv["refine_iters"]):
        bm = bmesh.new()
        bm.from_mesh(me)
        uvl = bm.loops.layers.uv["UVMap"]
        bm.verts.index_update()
        bm.edges.index_update()
        vf, has = corner_fields(bm, uvl, fields, detail_mats)
        mid = edge_mid_fields(bm, uvl, fields, detail_mats)
        ev = np.array([(e.verts[0].index, e.verts[1].index) for e in bm.edges], np.int64)
        co = np.array([v.co[:] for v in bm.verts])
        elen = np.linalg.norm(co[ev[:, 0]] - co[ev[:, 1]], axis=1) / MM_WU
        bad = elen > far
        why = {"far": int(bad.sum())}
        for L in layers:
            f = vf[L["name"]]
            fa, fb = f[ev[:, 0]], f[ev[:, 1]]
            m = mid[L["name"]]
            near = L.get("near_mm", lv["near_len_mm"]) * lv.get("near_scale", 1.0)
            for lev in layer_levels(L):
                cross = (fa - lev) * (fb - lev) < 0
                bad |= cross & (elen > near)
                # a crossing between the vertices that the ends do not show (a slot narrower than the edge)
                hidden = np.nanmax(np.stack([(m[:, k] - lev) * (fa - lev) < 0 for k in range(3)], 1), 1) & ~cross
                if L.get("hidden_split", L["name"] != "raise"):
                    bad |= hidden & (elen > 0.35 * near)
                # a crossing whose place the ends misjudge (the distance bends along the edge)
                lin = np.stack([0.75 * fa + 0.25 * fb, 0.5 * (fa + fb), 0.25 * fa + 0.75 * fb], 1)
                err = np.nanmax(np.abs(m - lin), 1)
                bad |= cross & (err > L.get("tol_mm", tol)) & (elen > 0.35 * near)
                why[L["name"]] = why.get(L["name"], 0) + int((cross & (elen > near)).sum()) * 1000000 \
                    + int((cross & (err > L.get("tol_mm", tol)) & (elen > 0.35 * near)).sum())
        sel = [e for e in bm.edges if bad[e.index] and any(l.face.material_index in detail_mats for l in e.link_loops)]
        if not sel:
            bm.free()
            break
        bmesh.ops.subdivide_edges(bm, edges=sel, cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
        bm.to_mesh(me)
        bm.free()
        me.update()
        total += len(sel)
        print("refine", it, "split", len(sel), "faces", len(me.polygons), json.dumps(why), flush=True)
    return {"splits": total, "faces": len(me.polygons)}


def smooth_field(bm, f, near_mm, iters, lam=0.5):
    """relax a distance along the surface near its contour (edge-length weighted Jacobi steps)"""
    ev = np.array([(e.verts[0].index, e.verts[1].index) for e in bm.edges], np.int64)
    co = np.array([v.co[:] for v in bm.verts])
    w = 1.0 / np.maximum(np.linalg.norm(co[ev[:, 0]] - co[ev[:, 1]], axis=1), 1e-9)
    border = np.zeros(len(co), bool)
    for e in bm.edges:
        if not e.is_manifold:
            border[e.verts[0].index] = border[e.verts[1].index] = True
    for _ in range(iters):
        acc = np.zeros_like(f)
        ws = np.zeros_like(f)
        np.add.at(acc, ev[:, 0], w * f[ev[:, 1]])
        np.add.at(acc, ev[:, 1], w * f[ev[:, 0]])
        np.add.at(ws, ev[:, 0], w)
        np.add.at(ws, ev[:, 1], w)
        nb = acc / np.maximum(ws, 1e-12)
        sel = (np.abs(f) < near_mm) & (ws > 0) & ~border
        f = np.where(sel, (1 - lam) * f + lam * nb, f)
    return f


def insert_contours(obj, fields, layers, detail_mats, smooth):
    """cut a vertex loop on every level of every layer; returns the per vertex distances (exact on loops)"""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    uvl = bm.loops.layers.uv["UVMap"]
    bm.verts.index_update()
    vf, has = corner_fields(bm, uvl, fields, detail_mats)
    lay = {}
    for L in layers:
        name = L["name"]
        f = vf[name]
        if L["name"] in smooth:
            f = smooth_field(bm, f, smooth[L["name"]][0], smooth[L["name"]][1])
        lay[name] = bm.verts.layers.float.new("f_" + name)
        for v in bm.verts:
            v[lay[name]] = float(f[v.index])
    armor = bm.verts.layers.int.new("armor")
    for v in bm.verts:
        v[armor] = int(has[v.index])
    stats = {}
    for L in layers:
        name = L["name"]
        ly = lay[name]
        n_cut = 0
        n_snap = 0
        for lev in layer_levels(L):
            # snap: a vertex very near the level (relative to its shortest edge) takes the level itself,
            # so no sliver is cut beside it
            for v in bm.verts:
                d = v[ly] - lev
                if d == 0 or abs(d) > 4.0 or not v[armor]:
                    continue
                emin = min((e.calc_length() for e in v.link_edges), default=0) / MM_WU
                if emin <= 0:
                    continue
                # the distance's slope is about 1 (mm per mm): within 12 % of the shortest edge
                if abs(d) < 0.12 * emin:
                    v[ly] = lev
                    n_snap += 1
            pct = {}
            for e in bm.edges:
                a, b = e.verts[0][ly] - lev, e.verts[1][ly] - lev
                if a * b < 0 and (e.verts[0][armor] or e.verts[1][armor]):
                    pct[e] = a / (a - b)
            if not pct:
                continue
            ret = bmesh.ops.subdivide_edges(bm, edges=list(pct.keys()), cuts=1, edge_percents=pct, use_grid_fill=False)
            for g in ret["geom_inner"]:
                if isinstance(g, bmesh.types.BMVert):
                    g[ly] = lev
                    g[armor] = 1
            bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
            n_cut += len(pct)
        stats[name] = {"edges_cut": n_cut, "snapped": n_snap}
    bm.verts.index_update()
    out = {L["name"]: np.array([v[lay[L["name"]]] for v in bm.verts]) for L in layers}
    has = np.array([bool(v[armor]) for v in bm.verts])
    # (the distances stay on the mesh as point attributes f_<layer>: inspection and the bake read them)
    bm.to_mesh(me)
    bm.free()
    me.update()
    return out, has, stats


# ------------------------------------------------------------------ crease bevel on the welded CE mesh
def bevel_creases(obj, fields, detail_mats, offset_mm, deg):
    """chamfer CE's hard creases inside the armour (both faces armour, dihedral over deg, the armour's
    distance positive on both sides): a flat bevel offset_mm wide"""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    uvl = bm.loops.layers.uv["UVMap"]
    F = fields["raise"]
    lim = math.radians(deg)
    sel = []
    for e in bm.edges:
        if len(e.link_faces) != 2 or any(f.material_index not in detail_mats for f in e.link_faces):
            continue
        if e.calc_face_angle(0.0) < lim:
            continue
        uvs = []
        for l in e.link_loops:
            uvs.append(0.5 * (Vector(l[uvl].uv) + Vector(l.link_loop_next[uvl].uv)))
        s = F.sample(np.array([u[:] for u in uvs], np.float64))
        if (s > 2.0).all():
            sel.append(e)
    if not sel:
        bm.free()
        return 0
    bmesh.ops.bevel(bm, geom=sel + list({v for e in sel for v in e.verts}), offset=offset_mm * MM_WU,
                    offset_type="OFFSET", segments=1, profile=0.5, affect="EDGES", clamp_overlap=True,
                    loop_slide=True, material=-1)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(me)
    bm.free()
    me.update()
    return len(sel)


# ------------------------------------------------------------------ weights
def transfer_weights2(obj, ref, arm, node_names, band):
    """CE's skin weights at each vertex's own point on CE's facets (attribute facet_co, kept through the
    planar clean-up): the nearest point of CE's mesh, barycentric in its triangle; tube vertices (no facet
    point) through their face's CE triangle and UV (hero_remodel.ce_weights). Two influences, normalised."""
    top_uv, w_uv = H1.ce_weights(obj, ref, band, node_names)
    me = obj.data
    nv = len(me.vertices)
    fc = np.zeros(nv * 3, np.float32)
    me.attributes["facet_co"].data.foreach_get("vector", fc)
    fc = fc.reshape(-1, 3).astype(np.float64)
    tube_v = np.zeros(nv, bool)
    if "tube" in me.attributes:
        tf = np.zeros(len(me.polygons), np.int32)
        me.attributes["tube"].data.foreach_get("value", tf)
        for p in me.polygons:
            if tf[p.index]:
                tube_v[list(p.vertices)] = True
    rme = ref.data
    gi = {g.index: node_names.index(g.name) for g in ref.vertex_groups if g.name in node_names}
    W = np.zeros((len(rme.vertices), len(node_names)))
    for v in rme.vertices:
        for g in v.groups:
            if g.group in gi:
                W[v.index, gi[g.group]] += g.weight
    W /= np.maximum(W.sum(1, keepdims=True), 1e-12)
    loc, idx = band.nearest(fc)
    p = band.v[band.t[idx]]
    e0, e1, d = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0], loc - p[:, 0]
    d00, d01, d11 = (e0 * e0).sum(1), (e0 * e1).sum(1), (e1 * e1).sum(1)
    d20, d21 = (d * e0).sum(1), (d * e1).sum(1)
    den = np.maximum(d00 * d11 - d01 * d01, 1e-30)
    b1 = (d11 * d20 - d01 * d21) / den
    b2 = (d00 * d21 - d01 * d20) / den
    b = np.clip(np.stack([1 - b1 - b2, b1, b2], 1), 0, 1)
    b /= b.sum(1, keepdims=True)
    Wv = (b[:, :, None] * W[band.t[idx]]).sum(1)
    top = np.argsort(-Wv, axis=1)[:, :2]
    w2 = np.take_along_axis(Wv, top, 1)
    w2 = np.where(w2 < 0.001, 0, w2)
    w2 /= np.maximum(w2.sum(1, keepdims=True), 1e-12)
    top = np.where(tube_v[:, None], top_uv, top)
    w2 = np.where(tube_v[:, None], w_uv, w2)
    obj.vertex_groups.clear()
    for n in node_names:
        obj.vertex_groups.new(name=n)
    for i in range(nv):
        for j in range(2):
            if w2[i, j] > 0:
                obj.vertex_groups[int(top[i, j])].add([i], float(w2[i, j]), "REPLACE")
    obj.parent = arm
    obj.matrix_parent_inverse = ref.matrix_parent_inverse.copy()
    if not any(m.type == "ARMATURE" for m in obj.modifiers):
        am = obj.modifiers.new("Armature", "ARMATURE")
        am.object = arm
    return {"tube_vertices": int(tube_v.sum())}


# ------------------------------------------------------------------ offset
def offset_solve(co, ftri, h, lam, max_ratio):
    """per vertex move d with n_f . d = h for every face f around it (corner-angle weighted least squares,
    a small pull toward h along the mean normal): flat planes stay flat and each crease stays a crease (the
    corner goes where the offset planes meet) instead of being bevelled by an averaged normal; a wall's foot
    and top, which lie on one CE facet, move exactly in parallel"""
    p = co[ftri]
    fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-20)
    ang = np.zeros((len(ftri), 3))
    for k in range(3):
        a_ = p[:, (k + 1) % 3] - p[:, k]
        b_ = p[:, (k + 2) % 3] - p[:, k]
        c_ = (a_ * b_).sum(1) / np.maximum(np.linalg.norm(a_, axis=1) * np.linalg.norm(b_, axis=1), 1e-30)
        ang[:, k] = np.arccos(np.clip(c_, -1, 1))
    nv = len(co)
    A = np.zeros((nv, 3, 3))
    b = np.zeros((nv, 3))
    nm = np.zeros((nv, 3))
    W = np.zeros(nv)
    nn = fn[:, :, None] * fn[:, None, :]
    for k in range(3):
        v = ftri[:, k]
        w = ang[:, k]
        np.add.at(A, v, w[:, None, None] * nn)
        np.add.at(b, v, (w * h[v])[:, None] * fn)
        np.add.at(nm, v, w[:, None] * fn)
        np.add.at(W, v, w)
    nm /= np.maximum(np.linalg.norm(nm, axis=1, keepdims=True), 1e-20)
    reg = lam * np.maximum(W, 1e-12)
    A += reg[:, None, None] * np.eye(3)[None]
    b += (reg * h)[:, None] * nm
    d = np.linalg.solve(A, b[..., None])[..., 0]
    dl = np.linalg.norm(d, axis=1)
    lim = max_ratio * np.abs(h) + 1e-12
    d *= np.minimum(1.0, lim / np.maximum(dl, 1e-20))[:, None]
    return d


# ------------------------------------------------------------------ rays
def ray_lengths(obj, origins, dirs, max_wu, skip_wu):
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    t = np.full(len(origins), max_wu)
    for i, (p, d) in enumerate(zip(origins, dirs)):
        o = Vector(p) + Vector(d) * skip_wu
        hit = tree.ray_cast(o, Vector(d), max_wu)
        if hit[0] is not None:
            t[i] = hit[3] + skip_wu
    return t


# ------------------------------------------------------------------ tubes
def add_tubes(obj, rc, feat, sides, mat_index):
    """cables on the finished surface: each path's pixels (hero_pick.py view) cast onto obj, centres
    lifted by 0.85 r along the normal, smoothed, swept by a ring of `sides`; UV and CE triangle of the
    surface point under each ring (so CE's skin weights and the undersuit's colour carry over)"""
    cams = json.load(open(os.path.join(P(rc["pick_dir"]), "cams.json")))
    cam = cams[feat["view"]]
    r_wu = feat["r_mm"] * MM_WU
    mwi = obj.matrix_world.inverted()
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.faces.ensure_lookup_table()
    uvl = bm.loops.layers.uv["UVMap"]
    cel = bm.faces.layers.int.get("ce_tri")
    tl = bm.faces.layers.int.get("tube") or bm.faces.layers.int.new("tube")
    tree = BVHTree.FromBMesh(bm)
    R, S = cam["res"], cam["scale"]
    org = np.array(cam["loc"])
    right, up, fwd = np.array(cam["right"]), np.array(cam["up"]), np.array(cam["fwd"])
    d_l = (mwi.to_3x3() @ Vector(fwd)).normalized()
    made = 0
    for path in feat["paths"]:
        bm.faces.ensure_lookup_table()
        px = np.array(path, np.float64)
        # dense Catmull-Rom through the pixels
        pts = []
        ext = np.vstack([px[0] * 2 - px[1], px, px[-1] * 2 - px[-2]])
        for i in range(1, len(ext) - 2):
            p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
            for t in np.linspace(0, 1, 24, endpoint=False):
                t2, t3 = t * t, t * t * t
                pts.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
        pts.append(px[-1])
        C, N, UVs, CE = [], [], [], []
        for x, y in pts:
            o = org + right * ((x / R - 0.5) * S) + up * ((0.5 - y / R) * S)
            o_l = mwi @ Vector(o)
            hit = tree.ray_cast(o_l, d_l, 10.0)
            if hit[0] is None:
                continue
            loc, nrm, fi, _ = hit
            f = bm.faces[fi]
            vs = [l.vert.co for l in f.loops]
            # barycentric UV
            a, b, c = vs
            v0, v1, v2 = b - a, c - a, loc - a
            d00, d01, d11 = v0.dot(v0), v0.dot(v1), v1.dot(v1)
            d20, d21 = v2.dot(v0), v2.dot(v1)
            den = max(d00 * d11 - d01 * d01, 1e-30)
            bb = (d11 * d20 - d01 * d21) / den
            cc = (d00 * d21 - d01 * d20) / den
            aa = 1 - bb - cc
            uv = sum((Vector(l[uvl].uv) * w for l, w in zip(f.loops, (aa, bb, cc))), Vector((0, 0)))
            C.append(np.array(loc) + np.array(nrm) * r_wu * 0.85)
            N.append(np.array(nrm))
            UVs.append((uv.x, uv.y))
            CE.append(f[cel] if cel is not None else 0)
        if len(C) < 4:
            continue
        C = np.array(C)
        for _ in range(6):
            C[1:-1] = 0.5 * C[1:-1] + 0.25 * (C[:-2] + C[2:])
        # resample at ~ 4 mm
        seg = np.linalg.norm(np.diff(C, axis=0), axis=1)
        s = np.concatenate([[0], np.cumsum(seg)])
        k = max(4, int(s[-1] / (4 * MM_WU)))
        ss = np.linspace(0, s[-1], k)
        idx = np.clip(np.searchsorted(s, ss) - 1, 0, len(s) - 2)
        tt = (ss - s[idx]) / np.maximum(seg[idx], 1e-12)
        Cs = C[idx] + (C[idx + 1] - C[idx]) * tt[:, None]
        Ns = np.array(N)[idx]
        UVs_ = [UVs[i] for i in idx]
        CEs = [CE[i] for i in idx]
        # ends sink into the surface
        Cs[0] -= Ns[0] * r_wu * 0.9
        Cs[-1] -= Ns[-1] * r_wu * 0.9
        rings = []
        for i in range(len(Cs)):
            tan = Cs[min(i + 1, len(Cs) - 1)] - Cs[max(i - 1, 0)]
            tan /= np.linalg.norm(tan) + 1e-12
            n0 = Ns[i] - tan * (Ns[i] @ tan)
            n0 /= np.linalg.norm(n0) + 1e-12
            b0 = np.cross(tan, n0)
            ring = []
            for j in range(sides):
                ang = 2 * math.pi * j / sides
                p = Cs[i] + r_wu * (math.cos(ang) * n0 + math.sin(ang) * b0)
                ring.append(bm.verts.new(p))
            rings.append(ring)
        for i in range(len(rings) - 1):
            for j in range(sides):
                a, b = rings[i][j], rings[i][(j + 1) % sides]
                c, d = rings[i + 1][(j + 1) % sides], rings[i + 1][j]
                for tri in ((a, b, c), (a, c, d)):
                    f = bm.faces.new(tri)
                    f.material_index = mat_index
                    f[tl] = 1
                    if cel is not None:
                        f[cel] = CEs[i]
                    for l in f.loops:
                        l[uvl].uv = UVs_[i]
                    f.smooth = True
                    made += 1
        for end, i in ((rings[0], 0), (rings[-1], len(rings) - 1)):
            cv = bm.verts.new(Cs[i])
            for j in range(sides):
                tri = (end[j], end[(j + 1) % sides], cv) if i else (end[(j + 1) % sides], end[j], cv)
                f = bm.faces.new(tri)
                f.material_index = mat_index
                f[tl] = 2
                if cel is not None:
                    f[cel] = CEs[i]
                for l in f.loops:
                    l[uvl].uv = UVs_[i]
                made += 1
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    me.update()
    return made


def set_shading2(obj, split_deg):
    """smooth, split above split_deg, face-area weighted normals; a tube's own edges never split"""
    me = obj.data
    for p in me.polygons:
        p.use_smooth = True
    bm = bmesh.new()
    bm.from_mesh(me)
    lim = math.radians(split_deg)
    tl = bm.faces.layers.int.get("tube")
    sharp_layer = bm.edges.layers.bool.get("sharp_edge") or bm.edges.layers.bool.new("sharp_edge")
    n = 0
    for e in bm.edges:
        s_ = len(e.link_faces) == 2 and e.calc_face_angle(0.0) > lim
        if s_ and tl is not None and all(f[tl] == 1 for f in e.link_faces):
            s_ = False
        e[sharp_layer] = s_
        n += s_
    bm.to_mesh(me)
    bm.free()
    if me.has_custom_normals:
        FB.activate(obj)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    FB.activate(obj)
    m = obj.modifiers.new("wn", "WEIGHTED_NORMAL")
    m.mode = "FACE_AREA"
    m.weight = 50
    m.keep_sharp = True
    bpy.ops.object.modifier_apply(modifier=m.name)
    return n


# ------------------------------------------------------------------ ground contact
def ground_pin(co_w, n_w, gp, fade_key="fade_cm"):
    """per vertex share of the move to drop (1 = stays on CE's facet): the band above CE's lowest point (the
    sole and the tread strip) fully, fading to nothing at fade_cm; faces that look down (the sole, under the
    toe and heel) fully up to down_cm. Rest pose, world z up; positions in world units."""
    if not gp:
        return np.zeros(len(co_w))
    z = co_w[:, 2] - REPORT["ground_z"]
    lo, hi = gp.get("band_cm", 3.0) * CM_WU, gp.get(fade_key, gp.get("fade_cm", 6.0)) * CM_WU
    t = np.clip((hi - z) / max(hi - lo, 1e-12), 0.0, 1.0)
    w = t * t * (3 - 2 * t)
    down = (n_w[:, 2] < -gp.get("down_cos", 0.5)) & (z < gp.get("down_cm", 8.0) * CM_WU)
    return np.where(down, 1.0, w)


# ------------------------------------------------------------------ one level
def build_level(name, weld, band, fields, layers_all, rc, lv, detail_mats, visor_mats, with_tubes=True, pretube_copy=None):
    rm = rc["remodel"]
    new = FB.dup(weld, name)
    new.hide_render = False
    new.vertex_groups.clear()
    for m_ in list(new.modifiers):
        new.modifiers.remove(m_)
    FB.activate(new)
    if lv["subdiv"] > 0:
        m = new.modifiers.new("subd", "SUBSURF")
        m.subdivision_type = "SIMPLE"
        m.levels = m.render_levels = lv["subdiv"]
        m.uv_smooth = "NONE"
        bpy.ops.object.modifier_apply(modifier=m.name)
    sel = lv.get("layers", "game")
    layers = [L for L in layers_all if L["name"] in fields and (L.get("game", True) if sel == "game" else L["name"] in sel)]
    fl = {L["name"]: fields[L["name"]] for L in layers}
    ref_info = refine_fields(new, fl, layers, lv, detail_mats)
    sm = {L["name"]: (L.get("smooth_near_mm", 8.0), L.get("smooth_iters", 0)) for L in layers if L.get("smooth_iters", 0) > 0}
    vf, has, cstats = insert_contours(new, fl, layers, detail_mats, sm)
    print("contours", name, json.dumps(cstats), "faces", len(new.data.polygons), round(time.time() - T0, 1), flush=True)
    mw = np.array(new.matrix_world)
    co0 = FB.mesh_np(new).astype(np.float64)
    on_facets = co0 @ mw[:3, :3].T + mw[:3, 3]
    if "facet_co" not in new.data.attributes:
        new.data.attributes.new("facet_co", "FLOAT_VECTOR", "POINT")
    new.data.attributes["facet_co"].data.foreach_set("vector", co0.astype(np.float32).ravel())
    # rounding: undersuit only
    restore = FB.band_restore(new, band, 1.0, owned=True)
    co1 = FB.mesh_np(new).astype(np.float64)
    d = co1 - co0
    dl = np.linalg.norm(d, axis=1)
    cap = rm["cap_pct"] / 100.0 * REPORT["extent_wu"]
    k = np.minimum(1.0, cap / np.maximum(dl, 1e-12))
    Lr = next(L for L in layers if L["name"] == "raise")
    praise = profile(Lr, vf["raise"]) / Lr["depth_cm"]
    k = k * (rm["suit_alpha"] * (1 - praise) + rm.get("plate_alpha", 0.0) * praise)
    k = np.where(has, k, 1.0 if rm.get("round_visor", False) else 0.0)
    base = co0 + d * k[:, None]
    FB.set_mesh_np(new, base)
    # heights (cm)
    # move direction: smoothed vertex normals
    nrm = H1.smooth_vertex_normals(new)
    # the ground contact stays CE's (recipe ground_pin): the sole and the tread band keep CE's position, so
    # the boot neither floats (base_cm set the sole 1.4 cm back into the boot) nor grows a ledge where the
    # sole meets the tread; the pin fades out above the band. The undersuit set-back (base_cm) fades over a
    # longer run (base_fade_cm): 1.4 cm over the 3 cm fade folded the boot's side walls over the arch and the
    # toe into teeth. Plates (the raise layer) keep the short fade and their old net height, so the toe cap
    # stands no prouder than before: only the undersuit between them is lifted back toward CE
    pin = ground_pin(on_facets, nrm @ mw[:3, :3].T, rc.get("ground_pin"))
    pin_base = ground_pin(on_facets, nrm @ mw[:3, :3].T, rc.get("ground_pin"), "base_fade_cm")
    b_cm = rc.get("base_cm", 0.0)
    h = np.full(len(base), b_cm)
    for L in layers:
        h += profile(L, vf[L["name"]])
    suit = 1.0 - np.clip(praise, 0.0, 1.0)
    h = h * (1.0 - pin) - b_cm * np.maximum(pin_base - pin, 0.0) * suit
    h = np.where(has, h, 0.0)
    ev = H1.edge_graph(new.data)
    # move direction: vertex normals relaxed over neighbours that face the same way (never across a thin
    # edge such as the brim's lip, whose two sides face apart), so a wall's foot and top move in parallel
    dn = nrm.copy()
    agree = (nrm[ev[:, 0]] * nrm[ev[:, 1]]).sum(1) > rm.get("normal_agree_cos", 0.5)
    ea = ev[agree]
    for _ in range(rm.get("normal_smooth_iters", 12)):
        acc = dn.copy()
        np.add.at(acc, ea[:, 0], dn[ea[:, 1]])
        np.add.at(acc, ea[:, 1], dn[ea[:, 0]])
        dn = acc / np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-12)
    disp = h * CM_WU
    th = ray_lengths(new, base, -nrm, 8 * CM_WU, 2e-5)
    inward_cap = rm["inward_thickness_share"] * th
    clr = ray_lengths(new, base, nrm, rm.get("clearance_max_cm", 4.0) * CM_WU, 2 * MM_WU)
    outward_cap = rm["outward_clearance_share"] * clr
    n_in = int((disp < -inward_cap).sum())
    n_out = int((disp > outward_cap).sum())
    # a cap scales the vertex's whole move; the scale is spread (a minimum over two rings, then relaxed)
    # so a capped terrace sinks evenly instead of tearing at single vertices
    cap = np.where(disp < 0, inward_cap, outward_cap)
    r = np.minimum(1.0, cap / np.maximum(np.abs(disp), 1e-9))
    raw = r.copy()
    for _ in range(rm.get("cap_spread_rings", 3)):
        m_ = r.copy()
        np.minimum.at(m_, ev[:, 0], r[ev[:, 1]])
        np.minimum.at(m_, ev[:, 1], r[ev[:, 0]])
        r = m_
    for _ in range(rm.get("cap_relax_iters", 6)):
        acc = r.copy()
        cnt = np.ones_like(r)
        np.add.at(acc, ev[:, 0], r[ev[:, 1]])
        np.add.at(acc, ev[:, 1], r[ev[:, 0]])
        np.add.at(cnt, ev[:, 0], 1)
        np.add.at(cnt, ev[:, 1], 1)
        r = acc / cnt
    disp = disp * r
    # fold relaxation (only true flips: walls stand at ~90 degrees by design)
    me_ = new.data
    lvx, _ = H1.mesh_arrays(me_)
    ftri = lvx.reshape(-1, 3)

    def fnorm(c):
        f = np.cross(c[ftri[:, 1]] - c[ftri[:, 0]], c[ftri[:, 2]] - c[ftri[:, 0]])
        return f / np.maximum(np.linalg.norm(f, axis=1, keepdims=True), 1e-20)
    fn0 = fnorm(base)
    D = offset_solve(base, ftri, disp, rm.get("offset_lambda", 0.08), rm.get("offset_max_ratio", 2.5))
    folds0 = None
    fold_cos = rm.get("fold_cos", -0.25)
    # the fall-back move of a folded corner goes along the relaxed normal, stretched so the faces around
    # the vertex still move by h (a mitre: at a 90 degree crease the diagonal needs sqrt(2) h), so a corner
    # keeps up with its faces instead of lagging behind both and leaving a ledge
    cw = np.zeros(len(base))
    cs = np.zeros(len(base))
    for k_ in range(3):
        np.add.at(cs, ftri[:, k_], (dn[ftri[:, k_]] * fn0).sum(1))
        np.add.at(cw, ftri[:, k_], 1.0)
    mitre = 1.0 / np.clip(cs / np.maximum(cw, 1), rm.get("mitre_min_cos", 0.5), 1.0)
    for it in range(rm.get("fold_relax_iters", 12)):
        co = base + D
        bad = (fnorm(co) * fn0).sum(1) < fold_cos
        if folds0 is None:
            folds0 = int(bad.sum())
        if not bad.any():
            break
        vb = np.unique(ftri[bad].ravel())
        # a folded face's corners fall back toward the smoothed-normal move, then toward their mean
        D[vb] = 0.5 * D[vb] + 0.5 * dn[vb] * (disp[vb] * mitre[vb])[:, None]
        if it >= 6:
            acc = np.zeros_like(D)
            cnt = np.zeros(len(D))
            np.add.at(acc, ftri[bad].ravel(), np.repeat(D[ftri[bad]].mean(1), 3, 0))
            np.add.at(cnt, ftri[bad].ravel(), 1)
            sel_ = cnt > 0
            D[sel_] = 0.5 * D[sel_] + 0.5 * acc[sel_] / cnt[sel_, None]
        D[pin > 0.999] = 0.0
    D[pin > 0.999] = 0.0
    co = base + D
    badf = (fnorm(co) * fn0).sum(1) < fold_cos
    folds_left = int(badf.sum())
    if "folded" not in new.data.attributes:
        new.data.attributes.new("folded", "INT", "FACE")
    new.data.attributes["folded"].data.foreach_set("value", badf.astype(np.int32))
    FB.set_mesh_np(new, co)
    MOVES[name] = (on_facets, co @ mw[:3, :3].T + mw[:3, 3] - on_facets)
    # folds the relaxation left (slivers turned over on steep terrace walls) collapsed before the planar
    # clean-up and the decimation can fold them into sawtooth and coincident twins
    hyg_offset = MH.clean(new) if rm.get("hygiene", True) else None
    pre = len(new.data.polygons)
    if rm.get("dissolve_deg", 0) > 0:
        # planar clean-up: faces left coplanar by the refinement merge (UV seams, shaders and every crease
        # kept), then a beauty triangulation; the loops survive (they are creases now)
        bm = bmesh.new()
        bm.from_mesh(new.data)
        bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(rm["dissolve_deg"]), use_dissolve_boundaries=False,
                                 verts=bm.verts, edges=bm.edges, delimit={"MATERIAL", "SEAM", "UV"})
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3], quad_method="BEAUTY",
                              ngon_method="BEAUTY")
        bm.to_mesh(new.data)
        bm.free()
        new.data.update()
    dissolved = (pre, len(new.data.polygons))
    tgt = lv.get("target_tris", 0)
    if tgt and len(new.data.polygons) > tgt:
        # quadric collapse to the level's budget: flat terraces and long straight walls carry few
        # triangles; the creases, loops and UV borders cost the error and stay
        FB.activate(new)
        m = new.modifiers.new("dec", "DECIMATE")
        m.decimate_type = "COLLAPSE"
        m.ratio = tgt / len(new.data.polygons)
        m.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=m.name)
    dissolved = dissolved + (len(new.data.polygons),)
    # what the collapse left: coincident twins and zero-thickness fins, zero-area faces, crumpled teeth
    hyg_dec = MH.clean(new) if rm.get("hygiene", True) else None
    if pretube_copy:
        cp = FB.dup(new, pretube_copy)
        for m_ in list(cp.modifiers):
            cp.modifiers.remove(m_)
        set_shading2(cp, rm["split_deg"])
        cp.hide_render = True
    tubes = 0
    if with_tubes:
        detail_slot = sorted(detail_mats)[0]
        for f in rc.get("features", []):
            if f["type"] == "tube3d":
                tubes += add_tubes(new, rc, f, lv.get("tube_sides", rm.get("tube_sides", 8)), detail_slot)
    sharp = set_shading2(new, rm["split_deg"])
    ntri = sum(len(p.vertices) - 2 for p in new.data.polygons)
    step("level_" + name, refine=ref_info, contours=cstats, layers=[L["name"] for L in layers],
         restore={k_: restore[k_] for k_ in ("moved_max", "outward_of_facet_max")},
         height_cm=[float(h.min()), float(h.max())], inward_capped=n_in, outward_capped=n_out,
         folds_before_relax=folds0, folds_left=folds_left, dissolve_faces=dissolved, tube_tris=tubes, sharp_edges=sharp, tris=ntri,
         verts=len(new.data.vertices), ground_pinned=int((pin > 0.999).sum()), ground_faded=int(((pin > 0) & (pin <= 0.999)).sum()),
         hygiene_offset=hyg_offset, hygiene_decimated=hyg_dec)
    return new


def highpoly_from(src, rc, detail, detail_mats):
    hpc = rc["highpoly"]
    o = FB.dup(src, "highpoly")
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    o.vertex_groups.clear()
    FB.activate(o)
    m = o.modifiers.new("subd", "SUBSURF")
    m.subdivision_type = "SIMPLE"
    m.levels = m.render_levels = hpc["subdiv"]
    m.uv_smooth = "NONE"
    bpy.ops.object.modifier_apply(modifier=m.name)
    h, has = H1.vertex_height(o, detail, skip_mat=set(range(len(o.material_slots))) - set(detail_mats))
    h *= hpc.get("detail_scale", 1.0)
    nrm = H1.smooth_vertex_normals(o)
    co = FB.mesh_np(o).astype(np.float64)
    FB.set_mesh_np(o, co + nrm * (h * CM_WU)[:, None])
    H1.set_shading(o, rc["remodel"]["split_deg"])
    step("highpoly", tris=sum(len(p.vertices) - 2 for p in o.data.polygons), detail_cm=[float(h.min()), float(h.max())])
    return o


def main():
    recipe, hdir, out = U.script_args()[:3]
    quick = "--quick" in U.script_args()
    rc = U.load_config(recipe)
    rm = rc["remodel"]
    os.makedirs(os.path.join(out, "export"), exist_ok=True)
    fields = {}
    for L in rc["layers"]:
        p = os.path.join(hdir, "sdf_%s.npy" % L["name"])
        if os.path.exists(p):
            fields[L["name"]] = Field(p)
    detail = H1.Relief(os.path.join(hdir, "detail.npy"))
    U.reset_scene()
    arm, ref = U.import_ref_gltf(P(rc["ref_gltf"]), "superhigh")
    arm.name, ref.name = "rig", "ce_ref"
    jdir = P(rc["ref_jms_dir"])
    tmpl = {lod: hf.read_jms(os.path.join(jdir, "__base %s.jms" % lod))
            for lod in ("superhigh", "high", "medium", "low", "superlow")}
    t0 = tmpl["superhigh"]
    slot_map = H1.shader_index_map(ref, t0)
    keep_names = [s.lower() for s in rc.get("keep_shaders", [])]
    mats = [m[0].lower() for m in t0.materials]
    detail_mats = {i for i, j in slot_map.items() if mats[j] == rc["detail_shader"].lower()}
    visor_mats = {i for i, j in slot_map.items() if mats[j] in [s.lower() for s in rc.get("smooth_shaders", [])]}
    keep_slots = {i for i, j in slot_map.items() if mats[j] in keep_names}
    step("import", ce_verts=len(ref.data.vertices), ce_tris=len(ref.data.polygons), slot_map=slot_map,
         detail_slots=sorted(detail_mats), smooth_slots=sorted(visor_mats), layers=sorted(fields))
    if "ce_tri" not in ref.data.attributes:
        ref.data.attributes.new("ce_tri", "INT", "FACE")
    ref.data.attributes["ce_tri"].data.foreach_set("value", np.arange(len(ref.data.polygons), dtype=np.int32))
    weld = FB.dup(ref, "ce_weld")
    bm = bmesh.new()
    bm.from_mesh(weld.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index in keep_slots], context="FACES")
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(weld.data)
    bm.free()
    if weld.data.has_custom_normals:
        FB.activate(weld)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    weld.hide_render = True
    co = FB.mesh_np(weld)
    REPORT["extent_wu"] = float((co.max(0) - co.min(0)).max())
    mww = np.array(weld.matrix_world)
    REPORT["ground_z"] = float((co @ mww[:3, :3].T + mww[:3, 3])[:, 2].min())
    band = FB.CEBand(ref, rm.get("band_max_lean_deg", 60.0), rm.get("band_smooth_deg", 5.0))
    if rm.get("pinned_use_corner_normals", True):
        fnc = np.repeat(band.fn[:, None, :], 3, 1)
        pinned = (np.abs(band.pn - fnc).sum(2) < 1e-12)
        band.pn = np.where(pinned[..., None], band.cn, band.pn)
    nb = bevel_creases(weld, fields, detail_mats, rm["crease_bevel_mm"], rm["crease_deg"]) if rm.get("crease_bevel_mm", 0) > 0 else 0
    step("weld", verts=len(weld.data.vertices), faces=len(weld.data.polygons), extent_wu=REPORT["extent_wu"],
         ce_pinned=band.n_pinned, crease_edges_bevelled=nb)

    node_names = [n.name for n in t0.nodes]
    built = {}
    for lv in rm["lods"]:
        if quick and lv["name"] != rm["lods"][0]["name"]:
            continue
        if lv.get("from"):
            o = H1.decimate_level("lod_" + lv["name"], built[lv["from"]], lv["ratio"])
            DECIMATED_FROM[lv["name"]] = lv["from"]
            hyg = MH.clean(o) if rm.get("hygiene", True) else None
            mx, unw = H1.limit_weights(o)
            set_shading2(o, rm["split_deg"])
            step("level_lod_" + lv["name"], decimated_from=lv["from"], ratio=lv["ratio"],
                 tris=sum(len(p.vertices) - 2 for p in o.data.polygons), verts=len(o.data.vertices),
                 max_influences=mx, unweighted=unw, hygiene=hyg)
        else:
            hpf = lv["name"] == rc["highpoly"]["from"]
            if quick:
                hpf = True
            o = build_level("lod_" + lv["name"], weld, band, fields, rc["layers"], rc, lv, detail_mats, visor_mats,
                            pretube_copy=("lod_%s_bake" % lv["name"]) if hpf else None)
            transfer_weights2(o, ref, arm, node_names, band)
            if hpf and not quick:
                # the bake target (this level without tubes) and the high-poly made from it
                hp = highpoly_from(bpy.data.objects["lod_%s_bake" % lv["name"]], rc, detail, detail_mats)
                hp.hide_render = True
        built[lv["name"]] = o

    if quick:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "remodel_quick.blend"))
        print("REMODEL2_OK quick", round(time.time() - T0, 1))
        return
    counts = {}
    for lod in ("superhigh", "high", "medium", "low", "superlow"):
        src = rm["lod_map"].get(lod)
        tm = tmpl[lod]
        if src is None:
            continue
        keep_idx = {i for i, m in enumerate(tm.materials) if m[0].lower() in keep_names}
        keep_tris = [t for t in tm.triangles if t.shader in keep_idx]
        lvl = src
        while lvl in DECIMATED_FROM:
            lvl = DECIMATED_FROM[lvl]
        km = H1.follow_surface(tm, keep_tris, MOVES["lod_" + lvl], rm.get("keep_lift_mm", 0.8) / 3048.0) \
            if ("lod_" + lvl) in MOVES and keep_tris else None
        j = H1.build_jms(built[src], tm, keep_tris, km)
        hf.write_jms(os.path.join(out, "export", "__base %s.jms" % lod), j)
        by = {}
        for t in j.triangles:
            by[tm.materials[t.shader][0]] = by.get(tm.materials[t.shader][0], 0) + 1
        counts[lod] = {"from": src, "tris": len(j.triangles), "verts": len(j.vertices), "tris_by_shader": by,
                       "ce_tris": len(tm.triangles)}
    step("export", lods=counts)
    REPORT["seconds_total"] = round(time.time() - T0, 1)
    with open(os.path.join(out, "remodel_report.json"), "w") as f:
        json.dump(REPORT, f, indent=1, default=float)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "remodel.blend"))
    print("REMODEL2_OK", REPORT["seconds_total"])


if __name__ == "__main__":
    main()
