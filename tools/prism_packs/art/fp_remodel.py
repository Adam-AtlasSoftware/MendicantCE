"""PRISM v2 remodel of a first-person model from CE's own mesh, headless Blender:

  blender -b --factory-startup --python art/fp_remodel.py -- <config.json>

Unlike fp_build.py (rounding only), this models: it bevels CE's hard edges, gives the armour plates
thickness, rounds every smooth surface, and adds as geometry the detail CE only paints (fp_relief.py's
relief, which follows CE's own texture lines), with a density that goes where the shape needs it.

  1. import the extracted glTF (rig + mesh) and its JMS (node block, markers, regions: kept verbatim);
     shaders listed in keep_shaders (the ammo counter's decals) are copied from CE's JMS unchanged;
  2. cage: CE's triangles welded (per node for a rigid model, so parts the animations move apart stay
     apart); plates: the plate shader's faces extruded outward (thickness) where they meet the suit;
  3. hard edges: edges whose CE corner normals split (CE's own smoothing intent) and that fold more
     than hard_min_deg are bevelled (segments, profile); everything else is a smooth surface;
  4. limit surface: Phong tessellation through the cage with face-area weighted normals (flat panels
     stay flat, bevels and CE's smooth surfaces round), plus the relief along the surface normal;
  5. adaptive refinement: edges are split (longest-edge bisection) where the straight edge misses that
     limit surface by more than tol_mm or bends the normal more than normal_deg, until max_tris;
  6. shading: face-area weighted normals, split above split_deg; UVs: CE's own, linear in every CE
     triangle (bevel faces interpolated from their face), so CE's maps and masks line up;
  7. weights: rigid models take their CE triangle's node; skinned ones Data Transfer from CE's mesh on
     the same rig (2 influences, normalised);
  8. high-poly for bakes: the refined mesh subdivided further, on the same limit surface with the full
     relief (h_high); bakes in fp_bake.py;
  9. JMS 8200 (template node block, materials, markers, regions; kept CE triangles appended) and a
     .blend (objects rig, ce_ref, new, high) for renders and checks.
Round 2 options (all off unless the config names them; round 1 configs build as before):
  crease: {"deg": d, "alpha": a}  rounding only away from creases: every cage vertex on an edge that folds
          more than d degrees (the bevels' own segments, CE's hard edges, the plates' rims) or on a shader
          border keeps its place (Phong alpha a, default 0): flat panels stay flat, bevels stay crisp, and
          the rounding is left to CE's genuinely curved surfaces
  alpha_by_shader: {shader: alpha}  how much each shader's curved surfaces round (hard surfaces less)
  hard_shading: [shaders]  those shaders shade by their geometry (face-area weighted normals, split above
          split_deg) instead of the limit surface's normal field: a bevel reads as a crisp highlight line
  high_refine: {max_tris, tol_mm, normal_deg, min_edge_mm, max_edge_mm}  the high-poly refined adaptively on
          the full relief (crisp walls get the triangles) instead of a uniform subdivision
  untangle_strict: true  after the easing passes, any triangle still folded is put back on its cage face
  high_untangle_iters / high_fold / high_ease_relief  the high-poly's own easing (0: none; its crisp walls
          stand steep, and easing there dents the surface); the high-poly shades by face-area weighted
          normals, so the refinement's slivers do not speckle the bake
Every output is derived from CE vertices: local only, never shipped. This script and its config are.
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
import mesh_hygiene as MH  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
MM = 1.0 / 3048.0          # one millimetre in world units
T0 = time.time()
REPORT = {"steps": {}}


def P(rel):
    return E.path(rel)


def step(name, **kv):
    REPORT["steps"][name] = dict(kv, t=round(time.time() - T0, 2))
    print("STEP", name, json.dumps(kv, default=float)[:700], flush=True)


# ------------------------------------------------------------------ mesh <-> numpy
def mesh_arrays(me):
    nv, nl, nf = len(me.vertices), len(me.loops), len(me.polygons)
    co = np.empty(nv * 3, np.float64)
    me.vertices.foreach_get("co", co)
    lv = np.empty(nl, np.int64)
    me.loops.foreach_get("vertex_index", lv)
    le = np.empty(nl, np.int64)
    me.loops.foreach_get("edge_index", le)
    ls = np.empty(nf, np.int64)
    me.polygons.foreach_get("loop_start", ls)
    lt = np.empty(nf, np.int64)
    me.polygons.foreach_get("loop_total", lt)
    uv = np.empty(nl * 2, np.float64)
    me.uv_layers["UVMap"].data.foreach_get("uv", uv)
    ev = np.empty(len(me.edges) * 2, np.int64)
    me.edges.foreach_get("vertices", ev)
    return dict(co=co.reshape(-1, 3), lv=lv, le=le, ls=ls, lt=lt, uv=uv.reshape(-1, 2), ev=ev.reshape(-1, 2))


def vertex_normals(co, A):
    """face-area weighted vertex normals of triangles"""
    tri = A["lv"].reshape(-1, 3)
    fn = np.cross(co[tri[:, 1]] - co[tri[:, 0]], co[tri[:, 2]] - co[tri[:, 0]])
    n = np.zeros_like(co)
    for i in range(3):
        np.add.at(n, tri[:, i], fn)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-30)


def untangle(Tlin, Tflat, Tfull, ref_n, A, iters, fold=0.05, ease_relief=True):
    """Folded triangles (facing against their cage face: steep relief on a fine tessellation, or
    Phong over CE's tight folds) are eased back: around them first the relief, then the rounding,
    is taken down a step at a time (to the cage's own flat triangles at worst, never folded), until
    no triangle is folded."""
    tri = A["lv"].reshape(-1, 3)
    ev = A["ev"]
    nv = len(Tlin)
    wr = np.ones(nv)
    wp = np.ones(nv)
    hist = []
    T = Tfull
    for it in range(iters):
        T = Tlin + wp[:, None] * (Tflat - Tlin) + wr[:, None] * (Tfull - Tflat)
        fn = np.cross(T[tri[:, 1]] - T[tri[:, 0]], T[tri[:, 2]] - T[tri[:, 0]])
        fl = np.linalg.norm(fn, axis=1)
        # (degenerate triangles, zero area on the cage too, do not count)
        bad = ((fn * ref_n).sum(1) < -fold * fl) & (fl > 1e-14)
        hist.append(int(bad.sum()))
        if not bad.any():
            break
        mv = np.zeros(nv, bool)
        mv[tri[bad].ravel()] = True
        dr = mv & (wr > 0) if ease_relief else np.zeros(nv, bool)
        wr[dr] = np.maximum(wr[dr] - 0.2, 0)
        dp = mv & ~dr
        wp[dp] = np.maximum(wp[dp] - 0.2, 0)
    info = {"folded_before": hist[0] if hist else 0, "folded_after": hist[-1] if hist else 0,
            "passes": len(hist), "relief_eased_verts": int((wr < 1).sum()), "rounding_eased_verts": int((wp < 1).sum())}
    if untangle.strict:
        # whatever is still folded goes back to its cage face (linear), with its neighbours, until none is
        for k in range(20):
            fn = np.cross(T[tri[:, 1]] - T[tri[:, 0]], T[tri[:, 2]] - T[tri[:, 0]])
            fl = np.linalg.norm(fn, axis=1)
            bad = ((fn * ref_n).sum(1) < -0.05 * fl) & (fl > 1e-14)
            if not bad.any():
                break
            mv = np.zeros(nv, bool)
            mv[tri[bad].ravel()] = True
            if k:
                # grow the ring
                e = A["ev"]
                g = mv[e[:, 0]] | mv[e[:, 1]]
                mv[e[g].ravel()] = True
            T[mv] = Tlin[mv]
        fn = np.cross(T[tri[:, 1]] - T[tri[:, 0]], T[tri[:, 2]] - T[tri[:, 0]])
        fl = np.linalg.norm(fn, axis=1)
        info["strict_rounds"] = k
        info["folded_final"] = int((((fn * ref_n).sum(1) < -0.05 * fl) & (fl > 1e-14)).sum())
    return T, info


untangle.strict = False


def face_attr(me, name):
    a = np.empty(len(me.polygons), np.int64)
    me.attributes[name].data.foreach_get("value", a)
    return a


# ------------------------------------------------------------------ the limit surface
class Cage:
    """Triangles with per-vertex position normals and rounding weights; Phong tessellation."""

    def __init__(self, V, F, N, alpha, hmask):
        self.V, self.F, self.N, self.alpha, self.hmask = V, F, N, alpha, hmask

    def face_normals(self):
        a, b, c = (self.V[self.F[:, i]] for i in range(3))
        n = np.cross(b - a, c - a)
        return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-30)

    def bary(self, p, f):
        a, b, c = (self.V[self.F[f, i]] for i in range(3))
        v0, v1, v2 = b - a, c - a, p - a
        d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
        d20 = (v2 * v0).sum(1); d21 = (v2 * v1).sum(1)
        den = d00 * d11 - d01 * d01
        den = np.where(np.abs(den) < 1e-30, 1e-30, den)
        w1 = (d11 * d20 - d01 * d21) / den
        w2 = (d00 * d21 - d01 * d20) / den
        w = np.stack([1 - w1 - w2, w1, w2], 1)
        w = np.clip(w, 0, 1)
        return w / w.sum(1, keepdims=True)

    def eval(self, p, f, h):
        """limit position and normal for points p (on cage face f) with relief height h (world units)"""
        w = self.bary(p, f)
        q = np.zeros_like(p)
        n = np.zeros_like(p)
        hm = np.zeros(len(p))
        for i in range(3):
            vi = self.F[f, i]
            Vi, Ni = self.V[vi], self.N[vi]
            d = ((p - Vi) * Ni).sum(1) * self.alpha[vi]
            q += w[:, i:i + 1] * (p - d[:, None] * Ni)
            n += w[:, i:i + 1] * Ni
            hm += w[:, i] * self.hmask[vi]
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20)
        return q + n * (h * hm)[:, None], n


class Relief:
    def __init__(self, path, key):
        d = np.load(path)
        self.h = d[key].astype(np.float64)
        self.S = self.h.shape[0]

    def sample(self, uv):
        S = self.S
        x = np.clip(uv[:, 0] * S - 0.5, 0, S - 1.001)
        y = np.clip((1 - uv[:, 1]) * S - 0.5, 0, S - 1.001)
        x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
        fx, fy = x - x0, y - y0
        h = self.h
        return (h[y0, x0] * (1 - fx) * (1 - fy) + h[y0, x0 + 1] * fx * (1 - fy) +
                h[y0 + 1, x0] * (1 - fx) * fy + h[y0 + 1, x0 + 1] * fx * fy)


def vertex_targets(A, cf_face, cage, relief):
    """per vertex: the cage face it lies on (via any of its faces), mean relief over its corners
    (seams stay closed), limit position and normal"""
    nv = len(A["co"])
    loop_face = np.repeat(np.arange(len(A["ls"])), A["lt"])
    vface = np.full(nv, -1, np.int64)
    vface[A["lv"]] = loop_face            # any face of the vertex
    hl = relief.sample(A["uv"]) if relief is not None else np.zeros(len(A["lv"]))
    hs = np.bincount(A["lv"], hl, nv)
    hc = np.bincount(A["lv"], None, nv)
    h = hs / np.maximum(hc, 1)
    T, N = cage.eval(A["co"], cf_face[vface], h)
    return T, N, h


# ------------------------------------------------------------------ cage construction
def ce_corner_normals(ref):
    me = ref.data
    cn = np.empty(len(me.loops) * 3, np.float64)
    me.corner_normals.foreach_get("vector", cn)
    return cn.reshape(-1, 3)


def build_cage(ref, tmpl, cfg, keep_idx):
    me = ref.data
    bm = bmesh.new()
    bm.from_mesh(me)
    cn = ce_corner_normals(ref)
    cnl = bm.loops.layers.float_vector.new("ce_n")
    for f in bm.faces:
        for l in f.loops:
            l[cnl] = Vector(cn[l.index])
    ctl = bm.faces.layers.int.get("ce_tri")
    gone = [f for f in bm.faces if f.material_index in keep_idx]
    bmesh.ops.delete(bm, geom=gone, context="FACES")
    # weld (per node for rigid models)
    dl = bm.verts.layers.deform.active
    if cfg["kind"] == "rigid":
        groups = {}
        for v in bm.verts:
            n = max(v[dl].items(), key=lambda kv: kv[1])[0] if v[dl] else -1
            groups.setdefault(n, []).append(v)
        for vs in groups.values():
            bmesh.ops.remove_doubles(bm, verts=vs, dist=cfg.get("weld_distance", 1e-5))
    else:
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=cfg.get("weld_distance", 1e-5))
    info = {"welded_verts": len(bm.verts), "faces": len(bm.faces)}
    names = [s.material.name.split("\\")[-1].split(".")[0] for s in ref.material_slots]

    # plates: the plate shader's faces extruded outward where they meet another shader
    pl = cfg.get("plate")
    rim = set()
    if pl:
        pidx = {i for i, n in enumerate(names) if n in pl["shaders"]}
        pf = [f for f in bm.faces if f.material_index in pidx]
        bnd = {e for f in pf for e in f.edges if any(g.material_index not in pidx for g in e.link_faces)}
        r = bmesh.ops.extrude_face_region(bm, geom=pf, use_keep_orig=False, use_normal_flip=False,
                                          use_select_history=False)
        newv = [g for g in r["geom"] if isinstance(g, bmesh.types.BMVert)]
        newe = [g for g in r["geom"] if isinstance(g, bmesh.types.BMEdge)]
        bm.normal_update()
        t = pl["thickness_mm"] * MM
        # region normals: area-weighted over the plate faces at each new vertex
        moves = {}
        for v in newv:
            n = Vector()
            for f in v.link_faces:
                if f.material_index in pidx and len(f.verts) >= 3 and f.normal.length > 0:
                    n += f.normal * f.calc_area()
            if n.length > 0:
                moves[v] = n.normalized() * t
        for v, d in moves.items():
            v.co += d
        wall = [f for f in bm.faces if f.material_index in pidx and any(
            v not in moves for v in f.verts) and any(v in moves for v in f.verts)]
        # the wall's rims are hard: bevel them too
        for f in wall:
            for e in f.edges:
                if len(e.link_faces) == 2:
                    rim.add(e)
        info["plate"] = {"faces": len(pf), "boundary_edges": len(bnd), "walls": len(wall),
                         "moved_verts": len(moves), "thickness_mm": pl["thickness_mm"]}
    # hard edges by CE's own split normals
    hb = cfg["hard"]
    split_cos = math.cos(math.radians(hb.get("split_deg", 5.0)))
    hard_shaders = {i for i, n in enumerate(names) if n in hb["shaders"]}
    hard = []
    soft = []
    small = 0
    small_edges = []
    for e in bm.edges:
        if len(e.link_faces) != 2:
            continue
        f0, f1 = e.link_faces
        if f0.material_index not in hard_shaders or f1.material_index not in hard_shaders:
            continue
        if math.degrees(e.calc_face_angle(0.0)) < hb.get("min_deg", 20.0):
            continue
        split = False
        for v in e.verts:
            n0 = next(l[cnl] for l in f0.loops if l.vert is v)
            n1 = next(l[cnl] for l in f1.loops if l.vert is v)
            if n0.dot(n1) < split_cos:
                split = True
        ang = e.calc_face_angle(0.0)
        # a smooth-shaded fold whose faces are wide is a box's corner, not a curve: CE smoothed
        # boxes (Gouraud) that are meant to be hard; the radius such a fold would have as a curve
        L = max(e.calc_length(), 1e-12)
        w = (f0.calc_area() + f1.calc_area()) / L          # the faces' mean width across the edge
        radius_mm = w / (2 * max(math.sin(ang / 2), 1e-6)) / MM
        crisp = split or math.degrees(ang) >= hb.get("always_deg", 75.0)
        if crisp or radius_mm >= hb.get("box_radius_mm", 1e9):
            # CE smoothed it (Gouraud over a box's corner): a wide, round bevel; CE split it: a crisp one
            wmm = cfg["bevel"].get("width_mm", 0) if crisp else cfg["bevel"].get("soft_width_mm", cfg["bevel"].get("width_mm", 0))
            # a bevel needs room: on small parts (CE's knobs and slivers) the faces stay smooth instead
            room = wmm * MM * cfg["bevel"].get("room", 3.0)
            if L < room or min(2 * f0.calc_area() / L, 2 * f1.calc_area() / L) < room:
                if crisp and cfg["bevel"].get("width_mm", 0) * MM * cfg["bevel"].get("room", 3.0) <= min(L, 2 * f0.calc_area() / L, 2 * f1.calc_area() / L):
                    hard.append(e)
                else:
                    small += 1
                    small_edges.append(e)
                continue
            (hard if crisp else soft).append(e)
    info["hard_edges"] = len(hard)
    info["hard_too_small_to_bevel"] = small

    hard = list(set(hard) | {e for e in rim if e.is_valid})
    info["hard_edges"] = len(hard)

    # bevel: the soft (wide, round) ones first, then the crisp ones that are still there
    crl = bm.verts.layers.int.new("crease")
    bv = cfg["bevel"]
    info["soft_edges"] = len(soft)
    info["bevel_faces"] = 0
    for edges, w, seg in ((soft, bv.get("soft_width_mm", 0), bv.get("soft_segments", 5)),
                          (hard, bv.get("width_mm", 0), bv.get("segments", 3))):
        edges = [e for e in edges if e.is_valid]
        if w <= 0 or not edges:
            continue
        r = bmesh.ops.bevel(bm, geom=edges + list({v for e in edges for v in e.verts}),
                            offset=w * MM, offset_type="OFFSET", profile_type="SUPERELLIPSE",
                            segments=seg, profile=bv.get("profile", 0.5), affect="EDGES",
                            clamp_overlap=True, material=-1, loop_slide=True, mark_seam=False,
                            mark_sharp=False, harden_normals=False, face_strength_mode="NONE",
                            miter_outer="ARC", miter_inner="SHARP", spread=0.1, vmesh_method="ADJ")
        info["bevel_faces"] += len(r["faces"])
        for f in r["faces"]:
            for v in f.verts:
                v[crl] = 1
    # (CE's own hard edges too small to bevel stay exactly as CE has them)
    for e in small_edges:
        if e.is_valid:
            for v in e.verts:
                v[crl] = 1
    for e in rim:
        if e.is_valid:
            for v in e.verts:
                v[crl] = 1
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    # drop degenerate faces the bevel may leave
    bmesh.ops.dissolve_degenerate(bm, dist=1e-9, edges=bm.edges)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.normal_update()
    if dl is not None:
        bm.verts.layers.deform.remove(dl)
    info["cage_tris"] = len(bm.faces)
    return bm, info


def cage_arrays(bm, cfg, protect_tree, shader_names=None):
    bm.verts.index_update()
    bm.faces.index_update()
    V = np.array([v.co[:] for v in bm.verts])
    F = np.array([[v.index for v in f.verts] for f in bm.faces])
    FM = np.array([f.material_index for f in bm.faces])
    a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    fn = np.cross(b - a, c - a)                       # length = 2 area: area weighting built in
    N = np.zeros_like(V)
    for i in range(3):
        np.add.at(N, F[:, i], fn)
    fa = np.zeros(len(V))
    for i in range(3):
        np.add.at(fa, F[:, i], np.linalg.norm(fn, axis=1))
    coherence = np.linalg.norm(N, axis=1) / np.maximum(fa, 1e-30)
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-30)
    alpha = np.full(len(V), cfg.get("alpha", 1.0))
    if cfg.get("alpha_by_shader") and shader_names is not None:
        # a vertex takes the smallest alpha of the shaders of its faces
        av = np.full(len(V), np.inf)
        for i in range(3):
            fa = np.array([cfg["alpha_by_shader"].get(shader_names[m], cfg.get("alpha", 1.0)) for m in FM])
            np.minimum.at(av, F[:, i], fa)
        alpha = np.where(np.isfinite(av), av, alpha)
    cage_arrays.creased = 0
    cage_arrays.crease_v = np.ones(len(V), bool)
    if cfg.get("crease"):
        # edges: dihedral from the triangles' normals; an edge of one face, or between two shaders, is a crease
        fnu = fn / np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-30)
        ekey = {}
        crease_v = np.zeros(len(V), bool)
        cdeg = math.cos(math.radians(cfg["crease"].get("deg", 20.0)))
        for fi, (a_, b_, c_) in enumerate(F):
            for u, w in ((a_, b_), (b_, c_), (c_, a_)):
                k = (min(u, w), max(u, w))
                ekey.setdefault(k, []).append(fi)
        by_ce = cfg["crease"].get("mode", "angle") == "ce"
        for (u, w), fs in ekey.items():
            if len(fs) != 2 or FM[fs[0]] != FM[fs[1]] or (not by_ce and (fnu[fs[0]] * fnu[fs[1]]).sum() < cdeg):
                crease_v[u] = crease_v[w] = True
        if by_ce:
            # CE's intent: the bevels, CE's split (hard) edges and the plates' rims; CE's smooth folds round
            crl = bm.verts.layers.int.get("crease")
            if crl is not None:
                crease_v |= np.array([v[crl] == 1 for v in bm.verts])
        alpha[crease_v] = cfg["crease"].get("alpha", 0.0)
        cage_arrays.creased = int(crease_v.sum())
        cage_arrays.crease_v = crease_v
    # CE's double-sided bits (two shells on the same vertices, facing away from each other): their
    # normals cancel; they are left exactly as CE has them (no rounding, no relief), or they would part
    folded = coherence < cfg.get("coherence_min", 0.35)
    alpha[folded] = 0.0
    # bulge limiter: Phong may not push an edge's middle out by more than bulge_ratio of the edge
    # (on CE's small, sharply folded knobs full rounding folds the surface over itself)
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    p0, p1 = V[E[:, 0]], V[E[:, 1]]
    m = (p0 + p1) / 2
    bul = np.linalg.norm(0.5 * ((m - p0) * N[E[:, 0]]).sum(1)[:, None] * N[E[:, 0]] +
                         0.5 * ((m - p1) * N[E[:, 1]]).sum(1)[:, None] * N[E[:, 1]], axis=1)
    ratio = bul / np.maximum(np.linalg.norm(p1 - p0, axis=1), 1e-12)
    # and never more than max_bulge_mm in absolute terms (CE's long slivers)
    ratio = np.maximum(ratio, bul / (cfg.get("max_bulge_mm", 3.0) * MM) * cfg.get("bulge_ratio", 0.12))
    worst = np.zeros(len(V))
    np.maximum.at(worst, E[:, 0], ratio)
    np.maximum.at(worst, E[:, 1], ratio)
    lim = cfg.get("bulge_ratio", 0.12)
    alpha *= np.clip(lim / np.maximum(worst, 1e-12), 0, 1)
    hmask = np.ones(len(V))
    hmask[folded] = 0.0
    cage_arrays.folded = int(folded.sum())
    if protect_tree is not None:
        r = cfg["protect"]["radius_mm"] * MM
        for i, p in enumerate(V):
            loc, nrm, idx, dist = protect_tree.find_nearest(Vector(p), r * 3)
            if dist is not None:
                k = min(1.0, max(0.0, (dist - r) / (2 * r)))
                alpha[i] = min(alpha[i], k)
                hmask[i] = min(hmask[i], k)
    c = Cage(V, F, N, alpha, hmask)
    c.crease = cage_arrays.crease_v.astype(np.float64)
    return c


# ------------------------------------------------------------------ refinement
def refine(bm, cage, relief, cfg, tmp_me):
    rf = cfg["refine"]
    tol = rf["tol_mm"] * MM
    ncos = math.cos(math.radians(rf["normal_deg"]))
    lmax = rf.get("max_edge_mm", 1e9) * MM
    max_tris = rf["max_tris"]
    cfl = bm.faces.layers.int.get("cf")
    hist = []
    for it in range(rf.get("iterations", 14)):
        bm.verts.index_update(); bm.edges.index_update(); bm.faces.index_update()
        bm.to_mesh(tmp_me)
        A = mesh_arrays(tmp_me)
        cf = face_attr(tmp_me, "cf")
        T, Nn, h = vertex_targets(A, cf, cage, relief)
        # one corner per edge: (loop l: vertex a, next loop: vertex b) in triangle faces
        nl = len(A["lv"])
        loop_face = np.repeat(np.arange(len(A["ls"])), A["lt"])
        nxt = A["ls"][loop_face] + (np.arange(nl) - A["ls"][loop_face] + 1) % A["lt"][loop_face]
        first = np.full(len(A["ev"]), -1, np.int64)
        first[A["le"][::-1]] = np.arange(nl)[::-1]
        l0 = first
        va, vb = A["lv"][l0], A["lv"][nxt[l0]]
        uvm = (A["uv"][l0] + A["uv"][nxt[l0]]) / 2
        pm = (A["co"][va] + A["co"][vb]) / 2
        hm = relief.sample(uvm) if relief is not None else np.zeros(len(pm))
        # midpoint relief as the corners' mean would give it across a seam
        Tm, Nm = cage.eval(pm, cf[loop_face[l0]], hm)
        err = np.linalg.norm(Tm - (T[va] + T[vb]) / 2, axis=1)
        ndot = (Nn[va] * Nn[vb]).sum(1)
        elen = np.linalg.norm(T[va] - T[vb], axis=1)
        bad = (err > tol) | ((ndot < ncos) & (elen > rf.get("min_edge_mm", 0.15) * MM)) | (elen > lmax)
        bad &= elen > rf.get("min_edge_mm", 0.15) * MM
        # a shader's part holds at most 65535 strip elements (invader strips about 1.15 per triangle,
        # and merges every material of one shader into one part): cap each shader's triangles
        mi = np.empty(len(tmp_me.polygons), np.int64)
        tmp_me.polygons.foreach_get("material_index", mi)
        per = np.bincount(mi, minlength=64)
        full = per >= rf.get("max_tris_per_shader", 52000)
        bad &= ~full[mi[loop_face[l0]]]
        ntri = len(bm.faces)
        hist.append({"it": it, "tris": ntri, "split": int(bad.sum()), "err_max_mm": float(err.max() / MM),
                     "err_p99_mm": float(np.percentile(err, 99) / MM)})
        print("REFINE", hist[-1], flush=True)
        if not bad.any() or ntri >= max_tris:
            break
        # budget: worst first
        # worst first, a share per pass, so the budget goes where the error is largest
        room = max(0, (max_tris - ntri) // 4)
        idx = np.nonzero(bad)[0]
        score = err[idx] / tol + (1 - ndot[idx]) / (1 - ncos) + elen[idx] / lmax
        take = max(int(len(idx) * rf.get("share", 0.45)), min(len(idx), 2000))
        idx = idx[np.argsort(-score)][:min(room, take)]
        bm.edges.ensure_lookup_table()
        sel = {bm.edges[i] for i in idx}
        # longest-edge bisection keeps triangles well shaped: a face with a split edge also splits its longest
        for e in list(sel):
            for f in e.link_faces:
                sel.add(max(f.edges, key=lambda x: x.calc_length()))
        bmesh.ops.subdivide_edges(bm, edges=list(sel), cuts=1, use_grid_fill=False, use_single_edge=False,
                                  use_only_quads=False, use_smooth_even=False)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    return hist


# ------------------------------------------------------------------ JMS
def build_jms(obj, tmpl, keep_tris, max_part_tris=20000):
    names = [n.name for n in tmpl.nodes]
    idx = {n: i for i, n in enumerate(names)}
    vg_name = {g.index: g.name for g in obj.vertex_groups}
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.update()
    uv = me.uv_layers["UVMap"].data
    mw = obj.matrix_world
    nrm3 = mw.to_3x3().inverted().transposed()
    corner_n = me.corner_normals
    verts, key2i, tris = [], {}, []
    for poly in me.polygons:
        ids = []
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index]
            gw = sorted(((g.weight, vg_name[g.group]) for g in v.groups
                         if g.weight > 0 and vg_name.get(g.group) in idx), reverse=True)[:2]
            if not gw:
                gw = [(1.0, names[0])]
            tot = sum(w for w, _ in gw)
            n0 = idx[gw[0][1]]
            n1, w1 = (idx[gw[1][1]], gw[1][0] / tot) if len(gw) > 1 else (-1, 0.0)
            pw = mw @ v.co * 100.0
            nw = (nrm3 @ corner_n[li].vector).normalized()
            u, vv = uv[li].uv
            key = (round(pw.x, 4), round(pw.y, 4), round(pw.z, 4), round(nw.x, 3), round(nw.y, 3), round(nw.z, 3),
                   round(u, 6), round(vv, 6), n0, n1, round(w1, 4))
            if key not in key2i:
                key2i[key] = len(verts)
                verts.append(hf.JmsVertex(n0, tuple(pw), tuple(nw), n1, w1, u, vv))
            ids.append(key2i[key])
        tris.append(hf.JmsTriangle(0, poly.material_index, (ids[0], ids[2], ids[1])))
    bpy.data.meshes.remove(me)
    remap = {}
    for t in keep_tris:
        vv = []
        for i in t.v:
            if i not in remap:
                remap[i] = len(verts)
                verts.append(tmpl.vertices[i])
            vv.append(remap[i])
        tris.append(hf.JmsTriangle(t.region, t.shader, tuple(vv)))
    # a part's strip may hold at most 65535 elements: big shaders are cut into several parts of the
    # same shader (extra material entries of the same name, as CE's own AR lists "numbers" twice),
    # in spatially coherent chunks (sorted along the model's long axis)
    mats = list(tmpl.materials)
    by = {}
    for t in tris:
        by.setdefault(t.shader, []).append(t)
    out = []
    for sh, ts in by.items():
        if len(ts) <= max_part_tris:
            out += ts
            continue
        cen = np.array([[sum(verts[i].pos[k] for i in t.v) / 3 for k in range(3)] for t in ts])
        ax = int(np.argmax(cen.max(0) - cen.min(0)))
        order = np.argsort(cen[:, ax], kind="stable")
        n = int(math.ceil(len(ts) / max_part_tris))
        size = int(math.ceil(len(ts) / n))
        for c in range(n):
            idx = sh if c == 0 else len(mats)
            if c:
                mats.append(tmpl.materials[sh])
            out += [hf.JmsTriangle(ts[i].region, idx, ts[i].v) for i in order[c * size:(c + 1) * size]]
    return hf.Jms(8200, tmpl.checksum, tmpl.nodes, mats, tmpl.markers, tmpl.regions, verts, out)


def new_object(name, me, like):
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    o.matrix_world = like.matrix_world.copy()
    if len(me.materials) == 0:
        for s in like.material_slots:
            me.materials.append(s.material)
    return o


def shade(obj, split_deg):
    me = obj.data
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(split_deg))
    bpy.context.view_layer.objects.active = obj
    for o in bpy.context.view_layer.objects:
        o.select_set(o is obj)
    m = obj.modifiers.new("wn", "WEIGHTED_NORMAL")
    m.mode = "FACE_AREA"
    m.keep_sharp = True
    m.weight = 50
    bpy.ops.object.modifier_apply(modifier=m.name)


def main():
    cfg = U.load_config(U.script_args()[0])
    out = P(cfg["out_dir"])
    os.makedirs(os.path.join(out, "export"), exist_ok=True)
    U.reset_scene()
    arm, ref = U.import_ref_gltf(P(cfg["ref_gltf"]), cfg.get("keep_lod", "superhigh"))
    arm.name, ref.name = "rig", "ce_ref"
    tmpl = hf.read_jms(P(cfg["ref_jms"]))
    slot_names = [s.material.name.split("\\")[-1].split(".")[0].lower() for s in ref.material_slots]
    jms_names = [m[0].lower() for m in tmpl.materials]
    assert slot_names == jms_names, (slot_names, jms_names)
    keep_idx = {i for i, n in enumerate(jms_names) if n in [s.lower() for s in cfg.get("keep_shaders", [])]}
    keep_tris = [t for t in tmpl.triangles if t.shader in keep_idx]
    me = ref.data
    if "ce_tri" not in me.attributes:
        me.attributes.new("ce_tri", "INT", "FACE")
    me.attributes["ce_tri"].data.foreach_set("value", np.arange(len(me.polygons), dtype=np.int32))
    step("import", ref_verts=len(me.vertices), ref_tris=len(me.polygons), shaders=jms_names,
         kept_ce_tris=len(keep_tris), nodes=len(tmpl.nodes), markers=len(tmpl.markers))

    # protect zone around the kept decals (the ammo counter): no rounding, no relief there
    protect_tree = None
    if keep_tris and cfg.get("protect"):
        pv = [Vector(v.pos) / 100.0 for v in tmpl.vertices]
        kv, kf = [], []
        for t in keep_tris:
            kf.append([len(kv), len(kv) + 1, len(kv) + 2])
            kv += [pv[i] for i in t.v]
        protect_tree = BVHTree.FromPolygons([ref.matrix_world.inverted() @ (ref.matrix_world @ p) for p in kv], kf)

    bm, info = build_cage(ref, tmpl, cfg, keep_idx)
    cfl = bm.faces.layers.int.new("cf")
    bm.faces.index_update()
    for f in bm.faces:
        f[cfl] = f.index
    cage = cage_arrays(bm, cfg, protect_tree, [s.material.name.split("\\")[-1].split(".")[0] for s in ref.material_slots])
    step("cage", **info, protected_verts=int((cage.hmask < 1).sum()), folded_verts=cage_arrays.folded,
         creased_verts=cage_arrays.creased, alpha_mean=float(cage.alpha.mean()))
    cage_me = bpy.data.meshes.new("cage")
    bm.to_mesh(cage_me)
    cage_obj = new_object("cage", cage_me, ref)
    cage_obj.hide_render = True

    untangle.strict = bool(cfg.get("untangle_strict", False))
    relief_g = Relief(P(cfg["relief"]), "h_game") if cfg.get("relief") else None
    relief_h = Relief(P(cfg["relief"]), "h_high") if cfg.get("relief") else None
    tmp_me = bpy.data.meshes.new("tmp")
    hist = refine(bm, cage, relief_g, cfg, tmp_me)
    lin_me = bpy.data.meshes.new("new_lin")
    bm.to_mesh(lin_me)
    bm.free()
    lin = new_object("new_lin", lin_me, ref)
    lin.hide_render = True

    # final game mesh
    new_me = lin_me.copy()
    new_me.name = "new"
    A = mesh_arrays(new_me)
    cf = face_attr(new_me, "cf")
    T, Nn, h = vertex_targets(A, cf, cage, relief_g)
    Tflat0, _, _ = vertex_targets(A, cf, cage, None)
    T, unfold = untangle(A["co"], Tflat0, T, cage.face_normals()[cf], A, cfg.get("untangle_iters", 60))
    new_me.vertices.foreach_set("co", T.astype(np.float32).ravel())
    new_me.update()
    new = new_object("new", new_me, ref)
    # shading: the limit surface's own normal field (smooth across the cage's edges, where Phong
    # patches meet with a slight crease), bent by the relief: + (geometric normal with relief -
    # geometric normal without it)
    Tflat, _, _ = vertex_targets(A, cf, cage, None)
    G1 = vertex_normals(T, A)
    G0 = vertex_normals(Tflat, A)
    ns = Nn + (G1 - G0)
    ns /= np.maximum(np.linalg.norm(ns, axis=1, keepdims=True), 1e-20)
    # where the surface turns away from the limit normal (the plates' walls, CE's tight folds) the
    # geometry's own normal shades it
    off = (ns * G1).sum(1) < math.cos(math.radians(cfg.get("normal_trust_deg", 50.0)))
    ns[off] = G1[off]
    new_me.shade_smooth()
    if cfg.get("hard_shading"):
        # hard surfaces shade by their geometry: face-area weighted corner normals, split above split_deg
        # (a bevel's segments share normals, a panel stays flat, the bevel reads as a crisp line); the
        # other shaders keep the limit surface's field
        names_ = [s.name.split("\\")[-1].split(".")[0] for s in new_me.materials]
        hard_idx = {i for i, n in enumerate(names_) if n in cfg["hard_shading"]}
        tri = A["lv"].reshape(-1, 3)
        fn_ = np.cross(T[tri[:, 1]] - T[tri[:, 0]], T[tri[:, 2]] - T[tri[:, 0]])     # 2 x area
        fu = fn_ / np.maximum(np.linalg.norm(fn_, axis=1, keepdims=True), 1e-30)
        mi_ = np.empty(len(new_me.polygons), np.int64)
        new_me.polygons.foreach_get("material_index", mi_)
        # corner normals: the area-weighted sum over the faces around the corner's vertex whose normal is
        # within split_deg of the corner's own face (a simple, robust smoothing-group rule)
        cs = math.cos(math.radians(cfg.get("hard_split_deg", 40.0)))
        nv_ = len(T)
        order = np.argsort(tri.ravel(), kind="stable")
        vflat = tri.ravel()[order]
        fflat = order // 3
        starts = np.searchsorted(vflat, np.arange(nv_ + 1))
        loopn = np.zeros((len(tri) * 3, 3))
        smooth_v = ns
        # a refined face shades hard only where its cage face is creased at all three corners (a panel
        # between bevels, a bevel itself); faces on CE's rounded surfaces keep the limit surface's field
        loop_face_ = np.repeat(np.arange(len(A["ls"])), A["lt"])
        vface_ = np.full(len(T), -1, np.int64)
        vface_[A["lv"]] = loop_face_
        cfv = cf[vface_]
        w_ = cage.bary(A["co"], cfv)
        vcrease = (w_ * cage.crease[cage.F[cfv]]).sum(1)
        face_hard = (vcrease[tri] > 0.999).all(1) & (cage.crease[cage.F[cf]] > 0.5).all(1)
        for f_ in range(len(tri)):
            for c in range(3):
                v_ = tri[f_, c]
                if mi_[f_] not in hard_idx or not face_hard[f_]:
                    loopn[3 * f_ + c] = smooth_v[v_]
                    continue
                fs = fflat[starts[v_]:starts[v_ + 1]]
                ok = (fu[fs] @ fu[f_] >= cs) & np.isin(mi_[fs], list(hard_idx)) & face_hard[fs]
                n_ = fn_[fs[ok]].sum(0)
                loopn[3 * f_ + c] = n_ / max(np.linalg.norm(n_), 1e-30)
        new_me.normals_split_custom_set([tuple(x) for x in loopn])
    else:
        new_me.normals_split_custom_set_from_vertices([tuple(x) for x in ns])
    ntri = len(new_me.polygons)
    move = np.linalg.norm(T - A["co"], axis=1)
    step("game_mesh", tris=ntri, verts=len(new_me.vertices), refine=hist, untangle=unfold,
         relief_mm=[float(h.min() / MM), float(h.max() / MM)], move_from_cage_max_mm=float(move.max() / MM),
         move_from_cage_mean_mm=float(move.mean() / MM))

    # weights
    for n in tmpl.nodes:
        new.vertex_groups.new(name=n.name)
    if cfg["kind"] == "rigid":
        ce_tri = me.attributes["ce_tri"]
        # CE triangle -> node (CE's AR is rigid: one node per triangle)
        tri_node = []
        names = [n.name for n in tmpl.nodes]
        vgn = {g.index: g.name for g in ref.vertex_groups}
        for p in me.polygons:
            v = me.vertices[p.vertices[0]]
            g = max(v.groups, key=lambda g: g.weight)
            tri_node.append(vgn[g.group])
        # new face -> cage face -> CE triangle (the cage keeps ce_tri through bevel and refinement)
        ct = face_attr(new_me, "ce_tri")
        votes = {}
        for p in new_me.polygons:
            nd = tri_node[ct[p.index]]
            for vi in p.vertices:
                votes.setdefault(vi, {}).setdefault(nd, 0)
                votes[vi][nd] += 1
        groups = {g.name: g for g in new.vertex_groups}
        bynode = {}
        for vi, vs in votes.items():
            nd = max(vs.items(), key=lambda kv: kv[1])[0]
            bynode.setdefault(nd, []).append(vi)
        for nd, vis in bynode.items():
            groups[nd].add(vis, 1.0, "REPLACE")
        mixed = sum(1 for vs in votes.values() if len(vs) > 1)
        step("weights", kind="rigid", verts_per_node={k: len(v) for k, v in bynode.items()}, mixed_verts=mixed)
    else:
        bpy.ops.object.select_all(action="DESELECT")
        new.select_set(True)
        bpy.context.view_layer.objects.active = new
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
        bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=2)
        bpy.ops.object.vertex_group_normalize_all(group_select_mode="ALL", lock_active=False)
        infl = [len([g for g in v.groups if g.weight > 0]) for v in new.data.vertices]
        step("weights", kind="skinned", max_influences=max(infl), unweighted=int(sum(1 for i in infl if i == 0)),
             two_bone=int(sum(1 for i in infl if i == 2)))
    new.parent = arm
    new.matrix_parent_inverse = ref.matrix_parent_inverse.copy()
    new.matrix_basis = ref.matrix_basis.copy()
    am = new.modifiers.new("Armature", "ARMATURE")
    am.object = arm

    # coincident twins and zero-thickness fins and zero-area faces (the back-face slits under the forearm and
    # at the wrist) removed before export; corner normals and weights ride along. No crumple collapse unless
    # the recipe asks (hygiene_crumple): the sleeves' plate rims are knife edges on purpose and a collapse
    # eats them (tried: 3,119 collapses, crumpled area up from 0.26% to 0.39%)
    if cfg.get("hygiene", True):
        step("hygiene", **MH.clean(new, crumple=cfg.get("hygiene_crumple", False)))

    # export
    jnew = build_jms(new, tmpl, keep_tris, cfg.get("max_part_tris", 10 ** 9))
    jpath = os.path.join(out, "export", cfg["jms_name"])
    hf.write_jms(jpath, jnew)
    by_shader = {}
    for t in jnew.triangles:
        by_shader[jnew.materials[t.shader][0]] = by_shader.get(jnew.materials[t.shader][0], 0) + 1
    vby = {}
    for t in jnew.triangles:
        vby.setdefault("%d %s" % (t.shader, jnew.materials[t.shader][0]), set()).update(t.v)
    step("export_jms", path=jpath, verts=len(jnew.vertices), tris=len(jnew.triangles), by_shader=by_shader,
         verts_by_shader={k: len(v) for k, v in vby.items()}, ce_tris=len(tmpl.triangles),
         ce_verts=len(tmpl.vertices), max_part_verts_ok=max(len(v) for v in vby.values()) < 65535)

    # high-poly (bake source): the linear mesh subdivided, then the same limit surface with the full relief
    if cfg.get("high_refine"):
        bmh = bmesh.new()
        bmh.from_mesh(lin_me)
        hcfg = dict(cfg, refine=dict(cfg["refine"], **cfg["high_refine"], max_tris_per_shader=10 ** 9))
        hhist = refine(bmh, cage, relief_h, hcfg, tmp_me)
        hi_me = bpy.data.meshes.new("high")
        bmh.to_mesh(hi_me)
        bmh.free()
        hi = new_object("high", hi_me, ref)
        HA = mesh_arrays(hi.data)
        hcf = face_attr(hi.data, "cf")
        HT, HN, hh = vertex_targets(HA, hcf, cage, relief_h)
        HT0, _, _ = vertex_targets(HA, hcf, cage, None)
        # (the high-poly's crisp walls stand steep or slightly over: only a real fold is eased there)
        untangle.strict = bool(cfg.get("high_untangle_strict", untangle.strict))
        HT, hunfold = untangle(HA["co"], HT0, HT, cage.face_normals()[hcf], HA, cfg.get("high_untangle_iters", cfg.get("untangle_iters", 60)),
                               fold=cfg.get("high_fold", 0.05), ease_relief=cfg.get("high_ease_relief", True))
        hi.data.vertices.foreach_set("co", HT.astype(np.float32).ravel())
        hi.data.update()
        hi.data.shade_smooth()
        # shading normals weighted by face area: the refinement's slivers (a corner near 180 degrees, no area)
        # would dominate Blender's angle-weighted normals and speckle the bake
        hvn = vertex_normals(HT, HA)
        hi.data.normals_split_custom_set_from_vertices(hvn.astype(np.float32).tolist())
        hi.hide_render = True
        step("high_poly", tris=len(hi.data.polygons), verts=len(hi.data.vertices), refine=hhist[-3:], untangle=hunfold,
             relief_mm=[float(hh.min() / MM), float(hh.max() / MM)])
    elif cfg.get("high_levels", 0) > 0:
        hi_me = lin_me.copy()
        hi_me.name = "high"
        hi = new_object("high", hi_me, ref)
        bpy.ops.object.select_all(action="DESELECT")
        hi.select_set(True)
        bpy.context.view_layer.objects.active = hi
        m = hi.modifiers.new("sub", "SUBSURF")
        m.subdivision_type = "SIMPLE"
        m.levels = m.render_levels = cfg["high_levels"]
        m.uv_smooth = "NONE"
        bpy.ops.object.modifier_apply(modifier=m.name)
        HA = mesh_arrays(hi.data)
        hcf = face_attr(hi.data, "cf")
        HT, HN, hh = vertex_targets(HA, hcf, cage, relief_h)
        hi.data.vertices.foreach_set("co", HT.astype(np.float32).ravel())
        hi.data.update()
        hi.data.shade_smooth()
        hi.hide_render = True
        step("high_poly", tris=len(hi.data.polygons), verts=len(hi.data.vertices),
             relief_mm=[float(hh.min() / MM), float(hh.max() / MM)])

    # envelope numbers (FP: silhouettes may change; recorded, not gated)
    def pts(o):
        a = np.empty(len(o.data.vertices) * 3)
        o.data.vertices.foreach_get("co", a)
        a = a.reshape(-1, 3)
        M = np.array(o.matrix_world)
        return a @ M[:3, :3].T + M[:3, 3]
    pr, pn = pts(ref), pts(new)
    c = (pr.min(0) + pr.max(0)) / 2
    tree = BVHTree.FromObject(ref, bpy.context.evaluated_depsgraph_get())
    step("envelope", bbox_ce=[pr.min(0).tolist(), pr.max(0).tolist()], bbox_new=[pn.min(0).tolist(), pn.max(0).tolist()],
         radius_ce=float(np.linalg.norm(pr - c, axis=1).max()), radius_new=float(np.linalg.norm(pn - c, axis=1).max()))
    REPORT["counts"] = {"ce_tris": len(tmpl.triangles), "new_tris": len(jnew.triangles), "new_verts": len(jnew.vertices)}
    REPORT["seconds_total"] = round(time.time() - T0, 1)
    with open(os.path.join(out, "build_report.json"), "w") as f:
        json.dump(REPORT, f, indent=1, default=float)
    bpy.data.meshes.remove(tmp_me)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "remodel.blend"))
    print("BUILD_OK", REPORT["seconds_total"], flush=True)


if __name__ == "__main__":
    main()
