"""PRISM v4 world geometry: a sealed level remodel (.pwg) thinned where its triangles buy no visible shape, without
opening a crack.

usage: blender -b --factory-startup --python art/world_geo/wg3_thin.py -- <bsp.gltf> <in.pwg> <out.pwg> <report.json>
environment (wu; 1 wu = 3.048 m; angles in degrees; light as a share of the corner's incident radiosity):
  WG3_NEAR_TOL (0.009), WG3_NEAR_ANGLE (15), WG3_NEAR_LIGHT (0.15): rock, ground and the rest
  WG3_NEAR_TOL_SMOOTH (0.005), WG3_NEAR_ANGLE_SMOOTH (6), WG3_NEAR_LIGHT_SMOOTH (0.08): the recipes' "smooth" kind
  WG3_NEAR_TOL_METAL (0.003), WG3_NEAR_ANGLE_METAL (2.5), WG3_NEAR_LIGHT_METAL (0.05): the "metal" kind
  WG3_FAR_TOL (0.15), WG3_FAR_TOL_METAL (0.05: smooth and metal), WG3_FAR_ANGLE (90: off), WG3_FAR_LIGHT (100: off)
  WG3_FAR_SCALE (1.0: each part's far distance scaled), WG3_ONLY (a comma list of shaders' last words to thin; all)
  WG3_SKIP_TIERS ("2": tiers kept whole, 0 rock and ground, 1 smooth, 2 metal), WG3_JOBS (8: processes; one where the
  system cannot fork, as on Windows), WG3_KEEP_POINTS (an .npz from wg3_fix.py: surfaces near them kept)

Why. wg2_seal.py made every edge two surfaces share the same vertices on both sides, and wg2_far2.py's far levels
keep each surface's whole boundary loop so near and far meet edge to edge. Both are right and both cost: the sealed
b30a draws about 2.1 million near and 0.6 million far triangles (the far level is also what the sun's three shadow
cascades draw, both faces), much of it where the near level's corners lie within a centimetre of a coarser surface
(flat metal fanned by the seal, sand and the broad faces of the rock strata) and where the boundary chains carry
every vertex the finest neighbour gave them.

How (no vertex is moved and none is made: every kept corner is the remodel's own row, so its position, texture and
lightmap coordinates, normal and baked light are exactly what they were):
  1. Every row of every part is welded by position (1e-4 wu, exactly as the seal left them). Each stock surface's
     near triangles are one loop seen in its corners' plane, or the surface is "kept": its triangles and all its
     boundary vertices stay as they are.
  2. The boundary chains. An edge of a loop that two surfaces share runs, between corners (a vertex where three or
     more loops meet, an edge only one surface has: the BSP's border, a transparent or stock neighbour, a kept
     surface), as a chain of vertices both sides have. Each chain is thinned once, Douglas-Peucker in space to the
     stricter tolerance of its two surfaces, and the choice is by position: both sides drop the same vertices, so
     the edge stays the same vertices on both sides (no T-junction, no gap).
  3. Each surface is triangulated again inside its thinned loop (Blender's constrained Delaunay, in its corners'
     plane), adding back its inner vertices greedily (the worst of each triangle at a time) while any lies further
     than the tolerance from the triangles, or its normal turns more than the angle, or its baked light differs by
     more than the light share. Near level: the near tolerances. Far level: the same loop with the far tolerances
     (so near and far still meet edge to edge, whichever each surface is drawn at). A surface whose triangulation
     fails is kept, and its loop's vertices are put back for its neighbours too (a second round).
  4. Written as version 2 with the same records, far distances (WG3_FAR_SCALE), surface bounds and rows (unused rows
     dropped). A corner whose position has several rows in a surface (a bevel's split normals) takes, in each new
     triangle, the row whose normal is nearest the triangle's.

Checked by wg3_fix.py (near, far and the two mixed modes: no open edge the input lacks) and the game's
debug.crack_view.
Everything it reads and writes is derived from the player's map: local only.
"""
import json
import math
import os
import sys
import time

import numpy as np
from mathutils import Vector, geometry

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wg_common as wg  # noqa: E402
from wg2_seal import read_pwg, write as write_pwg  # noqa: E402

T0 = time.time()
E = os.environ.get
NEAR = dict(tol=float(E("WG3_NEAR_TOL", "0.009")), metal=float(E("WG3_NEAR_TOL_METAL", "0.003")),
            cos=math.cos(math.radians(float(E("WG3_NEAR_ANGLE", "15")))), light=float(E("WG3_NEAR_LIGHT", "0.15")))
# (the tiers: 0 rock, ground and the rest; 1 smooth, the rounded classes whose highlights run along them (hulls,
# pipes, bark); 2 metal, flat panels whose reflections show any turn of the normal across a long triangle)
TIERS = [dict(tol=NEAR["tol"], cos=NEAR["cos"], light=NEAR["light"]),
         dict(tol=float(E("WG3_NEAR_TOL_SMOOTH", "0.005")), cos=math.cos(math.radians(float(E("WG3_NEAR_ANGLE_SMOOTH", "6")))),
              light=float(E("WG3_NEAR_LIGHT_SMOOTH", "0.08"))),
         dict(tol=NEAR["metal"], cos=math.cos(math.radians(float(E("WG3_NEAR_ANGLE_METAL", "2.5")))),
              light=float(E("WG3_NEAR_LIGHT_METAL", "0.05")))]
FAR = dict(tol=float(E("WG3_FAR_TOL", "0.15")), metal=float(E("WG3_FAR_TOL_METAL", "0.05")),
           cos=math.cos(math.radians(float(E("WG3_FAR_ANGLE", "90")))), light=float(E("WG3_FAR_LIGHT", "100")))
FAR_SCALE = float(E("WG3_FAR_SCALE", "1.0"))
SKIP_TIERS = {int(x) for x in E("WG3_SKIP_TIERS", "2").split(",") if x.strip()}
ONLY = [x.strip() for x in E("WG3_ONLY", "").split(",") if x.strip()]
WELD = 1e4


def log(*a):
    print("WG3 %7.1fs" % (time.time() - T0), *a, flush=True)


def boundary_loop(T):
    """the triangles' boundary as one loop of ids in their winding, or None"""
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    key = np.sort(e, 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    if (cnt > 2).any():
        return None
    b = e[cnt[inv.ravel()] == 1]
    if len(b) < 3:
        return None
    nxt = {}
    for u, v in b:
        u, v = int(u), int(v)
        if u in nxt:
            return None
        nxt[u] = v
    start = int(b[0, 0])
    loop = [start]
    while True:
        v = nxt.get(loop[-1])
        if v is None:
            return None
        if v == start:
            break
        loop.append(v)
        if len(loop) > len(b):
            return None
    return np.array(loop, np.int64) if len(loop) == len(b) else None


def frame(Q, loop, Tq):
    """the surface's plane from its loop's three spanning corners, turned to the triangles' winding: (origin, e1,
    e2, unit) or None"""
    q = Q[loop]
    k0 = int(np.argmax(np.linalg.norm(q - q.mean(0), axis=1)))
    k1 = int(np.argmax(np.linalg.norm(q - q[k0], axis=1)))
    line = q[k1] - q[k0]
    k2 = int(np.argmax(np.linalg.norm(np.cross(q - q[k0], line), axis=1)))
    if len({k0, k1, k2}) < 3:
        return None
    c0, c1, c2 = q[k0], q[k1], q[k2]
    face = np.cross(c1 - c0, c2 - c0)
    nf = np.linalg.norm(face)
    if nf < 2e-6:
        return None
    unit = face / nf
    tn = np.cross(Tq[:, 1] - Tq[:, 0], Tq[:, 2] - Tq[:, 0])
    if (tn @ unit).sum() < 0:
        unit = -unit
    e1 = (c1 - c0) / np.linalg.norm(c1 - c0)
    e2 = np.cross(unit, e1)
    return c0, e1, e2, unit


def dp_keep(pts, tol):
    """Douglas-Peucker in space: which of the chain's points stay (the ends always)"""
    n = len(pts)
    keep = np.zeros(n, bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        A, B = pts[a], pts[b]
        ab = B - A
        L = float(ab @ ab)
        mid = pts[a + 1:b]
        if L < 1e-18:
            d = np.linalg.norm(mid - A, axis=1)
        else:
            t = np.clip(((mid - A) @ ab) / L, 0, 1)
            d = np.linalg.norm(mid - (A + t[:, None] * ab), axis=1)
        k = int(np.argmax(d))
        if d[k] > tol:
            m = a + 1 + k
            keep[m] = True
            stack.append((a, m))
            stack.append((m, b))
    return keep


def cdt(uv, nloop):
    """triangles (indices into uv) inside the loop uv[:nloop], with every other point inside; None on failure"""
    pts2 = [Vector((float(x), float(y))) for x, y in uv]
    scale = max(float(np.abs(uv[:nloop]).max()), 1e-6)
    try:
        out = geometry.delaunay_2d_cdt(pts2, [], [list(range(nloop))], 1, 1e-9 * scale, True)
    except Exception:
        return None
    ov, faces, orig = out[0], out[2], out[3]
    if len(ov) != len(pts2):
        return None
    back = np.full(len(ov), -1, np.int64)
    for i, o in enumerate(orig):
        if len(o) != 1:
            return None
        back[i] = o[0]
    F = np.array([[back[f[0]], back[f[1]], back[f[2]]] for f in faces if len(f) == 3], np.int64)
    if not len(F) or (F < 0).any():
        return None
    Fq = uv[F]
    sg = (Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) - (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0])
    F[sg < 0] = F[sg < 0][:, [0, 2, 1]]
    loop_edges = {(i, (i + 1) % nloop) for i in range(nloop)}
    for _ in range(nloop + 1):
        Ee = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
        key = np.sort(Ee, 1)
        _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
        inv = inv.ravel()
        once = cnt[inv] == 1
        bad = np.zeros(len(F), bool)
        for e in np.nonzero(once)[0]:
            if (int(Ee[e, 0]), int(Ee[e, 1])) not in loop_edges:
                bad[e % len(F)] = True
        if not bad.any():
            break
        F = F[~bad]
        if not len(F):
            return None
    Ee = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    key = np.sort(Ee, 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    opened = {(int(a), int(b)) for (a, b), c in zip(Ee, cnt[inv]) if c == 1}
    if opened != loop_edges or (cnt > 2).any():
        return None
    return F


def locate(uv, F, q):
    """for each point q: its triangle in F (or -1) and barycentric weights"""
    A, B, C = uv[F[:, 0]], uv[F[:, 1]], uv[F[:, 2]]
    v0, v1 = B - A, C - A
    den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
    den = np.where(np.abs(den) < 1e-18, 1e-18, den)
    d = q[:, None, :] - A[None]
    b1 = (d[..., 0] * v1[None, :, 1] - v1[None, :, 0] * d[..., 1]) / den[None]
    b2 = (v0[None, :, 0] * d[..., 1] - d[..., 0] * v0[None, :, 1]) / den[None]
    b0 = 1 - b1 - b2
    m = np.minimum(np.minimum(b0, b1), b2)
    t = np.argmax(m, axis=1)
    i = np.arange(len(q))
    w = np.stack([b0[i, t], b1[i, t], b2[i, t]], 1)
    t = np.where(m[i, t] > -1e-6, t, -1)
    return t, np.clip(w, 0, 1)


def thin_surface(uv, h, nrm, rad, nloop, cand, lv):
    """the loop's points uv[:nloop] and inner candidates cand (indices into uv): the inner points kept (greedy) and
    the triangles (indices into uv), or None"""
    sel = []
    rest = np.array(cand, np.int64)
    for _ in range(400):
        idx = np.concatenate([np.arange(nloop), np.array(sel, np.int64)])
        F = cdt(uv[idx], nloop)
        if F is None:
            return None
        F = idx[F]
        if not len(rest):
            return sel, F
        t, w = locate(uv, F, uv[rest])
        bad_loc = t < 0
        tt = np.where(bad_loc, 0, t)
        cor = F[tt]
        hi = (w * h[cor]).sum(1)
        ni = (w[..., None] * nrm[cor]).sum(1)
        ni /= np.maximum(np.linalg.norm(ni, axis=1, keepdims=True), 1e-12)
        ri = (w[..., None] * rad[cor]).sum(1)
        eh = np.abs(hi - h[rest]) / lv["tol"]
        cosv = (ni * nrm[rest]).sum(1)
        en = np.where(cosv < lv["cos"], 1.5, 0.0)
        rmag = np.maximum(np.linalg.norm(rad[rest], axis=1), 1e-6)
        el = np.linalg.norm(ri - rad[rest], axis=1) / rmag / lv["light"]
        err = np.maximum(np.maximum(eh, en), el)
        # (a point outside the thinned loop seen in the plane lies within the chain's tolerance of its chord)
        err[bad_loc] = 0.0
        over = err > 1.0
        if not over.any():
            return sel, F
        # the worst of each triangle
        o = np.nonzero(over)[0]
        order = o[np.argsort(-err[o])]
        seen, add = set(), []
        for k in order:
            key = int(tt[k]) if not bad_loc[k] else -1 - int(k)
            if key in seen:
                continue
            seen.add(key)
            add.append(int(rest[k]))
        sel.extend(add)
        rest = np.setdiff1d(rest, np.array(add, np.int64))
    return None

W = {}
JOBS = int(os.environ.get("WG3_JOBS", "8"))


def do_chunk(chunk):
    return [(i, do_surface(i)) for i in chunk]


def chunk_results(chunks):
    """each chunk's surfaces thinned (do_chunk): in JOBS processes forked from this one, which read W as it stands; in
    this process where the system cannot fork (Windows: a spawned worker would be Blender started again) or JOBS is 1.
    The results are kept by surface, so the file is the same either way."""
    import multiprocessing as mp
    if JOBS > 1 and "fork" in mp.get_all_start_methods():
        with mp.get_context("fork").Pool(JOBS) as pool:
            yield from pool.imap_unordered(do_chunk, chunks)
    else:
        yield from map(do_chunk, chunks)


def do_surface(i):
    """a surface's thinned near and far triangles (rows of its part; far None when it is the near), or None"""
    parts, gid, offs, gpos, surf, removed = (W[k] for k in ("parts", "gid", "offs", "gpos", "surf", "removed"))
    r = surf[i]
    p = parts[r["pi"]]
    T = p["tri"][p["sft"][r["s"]]:p["sft"][r["s"] + 1]]
    Gp = gid[offs[r["pi"]]:offs[r["pi"] + 1]]
    loop = r["loop"][~removed[r["loop"]]]
    if len(loop) < 3:
        return None
    Tg = Gp[T]
    inner = np.setdiff1d(np.unique(Tg), r["loop"])
    ids = np.concatenate([loop, inner])
    c0, e1, e2, unit = r["frame"]
    X = gpos[ids] - c0
    uv = np.stack([X @ e1, X @ e2], 1)
    h = X @ unit
    # (a position's rows in this surface: the first's normal and light stand for it in the errors)
    rows_of = {}
    for t_row, t_g in zip(T.ravel().tolist(), Tg.ravel().tolist()):
        rows_of.setdefault(t_g, set()).add(t_row)
    rep = np.array([min(rows_of[int(g)]) for g in ids], np.int64)
    nrm = p["verts"][rep, 3:6].astype(np.float64)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
    rad = p["lmv"][rep, 0:3].astype(np.float64)
    cand = np.arange(len(loop), len(ids))
    res_n = thin_surface(uv, h, nrm, rad, len(loop), cand, TIERS[r["tier"]])
    if res_n is None:
        return None
    sel_n, Fn = res_n
    res_f = thin_surface(uv, h, nrm, rad, len(loop), np.array(sel_n, np.int64),
                         dict(FAR, tol=FAR["metal"] if r["tier"] else FAR["tol"]))
    # (the far level from the near's own points: it never has a point the near level lacks)
    Ff = res_f[1] if res_f is not None and len(res_f[1]) < len(Fn) else None

    def to_rows(F):
        # a position with several rows takes the row whose normal is nearest the triangle's
        g3 = ids[F]
        out = np.zeros_like(g3)
        fn = np.cross(gpos[g3[:, 1]] - gpos[g3[:, 0]], gpos[g3[:, 2]] - gpos[g3[:, 0]])
        fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
        for t in range(len(g3)):
            for k in range(3):
                rs = rows_of[int(g3[t, k])]
                if len(rs) == 1:
                    out[t, k] = next(iter(rs))
                else:
                    rl = np.array(sorted(rs))
                    out[t, k] = rl[int(np.argmax(p["verts"][rl, 3:6] @ fn[t]))]
        return out
    Rf = to_rows(Ff) if Ff is not None else None
    # (a surface whose loop lost no vertex may keep the file's own far level when that is fewer triangles: it has
    # the same loop, so it still meets its neighbours edge to edge)
    if len(loop) == len(r["loop"]):
        Fo = p["tri"][p["pairs"][r["s"], 0]:p["pairs"][r["s"], 0] + p["pairs"][r["s"], 1]]
        if len(Fo) < len(Rf if Rf is not None else Fn):
            Rf = Fo
    return to_rows(Fn), Rf, len(T)


def main():
    a = sys.argv[sys.argv.index("--") + 1:]
    gltf, src, dst, report_path = a[0], a[1], a[2], a[3]
    info, mats = wg.load_bsp(gltf)
    names = {(m.lightmap_index, m.material_index): m.shader for m in mats}
    allr = json.load(open(os.path.join(HERE, "wg2_recipes.json")))
    bsp = os.path.splitext(os.path.basename(gltf))[0]
    classes = next((v.get("classes", {}) for k, v in allr.items() if k.endswith("\\" + bsp)), {})

    def kind(shader):
        for cls, spec in classes.items():
            if any(p in shader for p in spec.get("match", [])):
                return spec.get("kind", cls)
        return "other"
    header, far_in, parts = read_pwg(src)
    log("read", src, len(parts), "parts")
    offs = np.cumsum([0] + [len(p["verts"]) for p in parts])
    allP = np.concatenate([p["verts"][:, 0:3] for p in parts]).astype(np.float64)
    _, gid = np.unique(np.round(allP * WELD).astype(np.int64), axis=0, return_inverse=True)
    gid = gid.ravel()
    G = gid.max() + 1
    gpos = np.zeros((G, 3))
    gpos[gid] = allP
    log("rows", len(allP), "positions", G)
    # (WG3_KEEP_POINTS: an .npz whose "P" are points, wg3_fix.py's ends of open edges the input did not have; every
    # surface with a vertex within 3 mm of one is kept whole, its boundary pinned)
    near_keep = np.zeros(G, bool)
    kp = E("WG3_KEEP_POINTS", "")
    if kp and os.path.exists(kp):
        K = np.load(kp)["P"].astype(np.float64).reshape(-1, 3)
        for k0 in range(0, len(K), 64):
            d = np.full(G, np.inf)
            for kk in K[k0:k0 + 64]:
                d = np.minimum(d, np.abs(gpos - kk).max(1))
            near_keep |= d < 0.003
        log("keep points", len(K), "positions near them", int(near_keep.sum()))

    # ---- 1. every surface's loop (in position ids) or kept
    surf = []  # per surface: dict(pi, s, loop(gids) or None, tier, thin)
    for pi, p in enumerate(parts):
        shader = names.get((p["lm"], p["mi"]), "?")
        k = kind(shader)
        tier = 2 if (k == "metal" or "metal" in shader) else 1 if k == "smooth" else 0
        # (WG3_SKIP_TIERS, default the metal tier: its bump-mapped reflections show the new long triangles as fans
        # of facets even inside the normal tolerance, seen in a30's Forerunner halls; those surfaces are kept whole,
        # near and far, and their boundaries pinned)
        thin = (not ONLY or any(shader.endswith(o) for o in ONLY)) and tier not in SKIP_TIERS
        Gp = gid[offs[pi]:offs[pi + 1]]
        P = p["verts"][:, 0:3].astype(np.float64)
        for s in range(p["psc"]):
            T = p["tri"][p["sft"][s]:p["sft"][s + 1]]
            rec = dict(pi=pi, s=s, loop=None, tier=tier, thin=thin, edges=None)
            if len(T):
                Tg = Gp[T]
                ok = (Tg[:, 0] != Tg[:, 1]) & (Tg[:, 1] != Tg[:, 2]) & (Tg[:, 2] != Tg[:, 0])
                e = np.concatenate([Tg[:, [0, 1]], Tg[:, [1, 2]], Tg[:, [2, 0]]])
                key = np.sort(e, 1)
                u, cnt = np.unique(key, axis=0, return_counts=True)
                rec["edges"] = u[cnt == 1]
                # (triangles whose corners are the same point, the seal's slivers, have no area and cancel in the
                # boundary: left out of the loop and of the new triangles; corners that only weld together, a hair
                # apart, keep the surface whole, since its neighbours have the hair's two points)
                Pt = P[T]
                exact = ((Pt[:, 0] == Pt[:, 1]).all(1) | (Pt[:, 1] == Pt[:, 2]).all(1) | (Pt[:, 2] == Pt[:, 0]).all(1))
                clean = bool((ok | exact).all())
                Tg = Tg[ok]
                if thin and clean and len(Tg) >= 2 and not near_keep[Gp[T]].any():
                    loop = boundary_loop(Tg)
                    if loop is not None and len(loop) >= 3:
                        fr = frame(gpos, loop, gpos[Tg])
                        if fr is not None:
                            c0, e1, e2, unit = fr
                            tn = np.cross(gpos[Tg[:, 1]] - gpos[Tg[:, 0]], gpos[Tg[:, 2]] - gpos[Tg[:, 0]]) @ unit
                            lu = np.stack([(gpos[loop] - c0) @ e1, (gpos[loop] - c0) @ e2], 1)
                            poly = 0.5 * float((lu[:, 0] * np.roll(lu[:, 1], -1) - np.roll(lu[:, 0], -1) * lu[:, 1]).sum())
                            proj = 0.5 * tn.sum()
                            if (tn > -1e-9 * max(abs(proj), 1e-9)).all() and poly > 1e-7 and abs(proj - poly) < 1e-3 * poly:
                                rec["loop"] = loop
                                rec["frame"] = fr
            surf.append(rec)
    n_loop = sum(1 for r in surf if r["loop"] is not None)
    log("surfaces", len(surf), "with a loop", n_loop)

    # ---- the boundary graph: each boundary edge's surfaces, each position's neighbours along boundaries
    E_all = np.concatenate([r["edges"] for r in surf if r["edges"] is not None and len(r["edges"])])
    owner = np.concatenate([np.full(len(r["edges"]), i) for i, r in enumerate(surf)
                            if r["edges"] is not None and len(r["edges"])])
    ekey = E_all[:, 0] * G + E_all[:, 1]
    uk, einv, ecnt = np.unique(ekey, return_inverse=True, return_counts=True)
    einv = einv.ravel()
    U = np.stack([uk // G, uk % G], 1)
    deg = np.bincount(U.ravel(), minlength=G)
    base_pin = np.zeros(G, bool)
    base_pin[U[ecnt != 2].ravel()] = True
    base_pin |= (deg != 2) & (deg > 0)
    # (an edge's stricter tolerance: the higher tier of its sides)
    etier = np.zeros(len(uk), np.int64)
    np.maximum.at(etier, einv, np.array([surf[o]["tier"] for o in owner]))
    # (the edges of a surface that is kept or not thinned: all its vertices pinned)
    def pins_of(idx):
        out = np.zeros(G, bool)
        for i in idx:
            r = surf[i]
            if r["edges"] is not None and len(r["edges"]):
                out[r["edges"].ravel()] = True
        return out
    kept = [i for i, r in enumerate(surf) if r["loop"] is None]
    pin = base_pin | pins_of(kept)
    # adjacency of the boundary graph (both ways)
    adj_u = np.concatenate([U[:, 0], U[:, 1]])
    adj_v = np.concatenate([U[:, 1], U[:, 0]])
    adj_e = np.concatenate([np.arange(len(U)), np.arange(len(U))])
    order = np.argsort(adj_u, kind="stable")
    adj_u, adj_v, adj_e = adj_u[order], adj_v[order], adj_e[order]
    start = np.searchsorted(adj_u, np.arange(G + 1))

    report = dict(src=src, near=NEAR, far=FAR, far_scale=FAR_SCALE, only=ONLY, rounds=[])
    near_out, far_out = {}, {}
    for rnd in range(4):
        # ---- 2. chains thinned by position
        removed = np.zeros(G, bool)
        seen_e = np.zeros(len(U), bool)
        chains = 0
        for v0 in np.nonzero(pin & (deg > 0))[0]:
            for j in range(start[v0], start[v0 + 1]):
                e0 = adj_e[j]
                if seen_e[e0]:
                    continue
                chain, tier_c = [v0], etier[e0]
                seen_e[e0] = True
                cur, nxt = v0, adj_v[j]
                while True:
                    chain.append(nxt)
                    if pin[nxt]:
                        break
                    # (an unpinned position has exactly two boundary neighbours)
                    k0, k1 = start[nxt], start[nxt + 1]
                    step = None
                    for jj in range(k0, k1):
                        if adj_v[jj] != cur and not seen_e[adj_e[jj]]:
                            step = jj
                            break
                    if step is None:
                        break
                    seen_e[adj_e[step]] = True
                    tier_c = max(tier_c, etier[adj_e[step]])
                    cur, nxt = nxt, adj_v[step]
                if len(chain) > 2:
                    tol = TIERS[tier_c]["tol"]
                    keep = dp_keep(gpos[np.array(chain)], tol)
                    removed[np.array(chain)[~keep]] = True
                    chains += 1
        # (cycles with no pinned vertex: left whole)
        removed &= ~pin
        log("round", rnd, "chains", chains, "boundary positions dropped", int(removed.sum()))

        # ---- 3. each surface again inside its thinned loop
        failed = []
        tn_in = tn_near = tn_far = 0
        W.update(parts=parts, gid=gid, offs=offs, gpos=gpos, surf=surf, removed=removed)
        todo = [i for i, r in enumerate(surf) if r["loop"] is not None]
        chunks = [todo[k:k + 200] for k in range(0, len(todo), 200)]
        for res in chunk_results(chunks):
            for i, out in res:
                if out is None:
                    failed.append(i)
                    continue
                Rn, Rf, n_in = out
                near_out[i] = Rn
                far_out[i] = Rf
                tn_in += n_in
                tn_near += len(Rn)
                tn_far += len(Rf if Rf is not None else Rn)
        report["rounds"].append(dict(round=rnd, dropped_boundary=int(removed.sum()), failed=len(failed),
                                     near_in=tn_in, near_out=tn_near, far_out=tn_far))
        log("round", rnd, "failed", len(failed), "near", tn_in, "->", tn_near, "far", tn_far)
        if not failed:
            break
        # the failed surfaces kept, their loops' vertices pinned for their neighbours too; again
        for i in failed:
            surf[i]["loop"] = None
            near_out.pop(i, None)
            far_out.pop(i, None)
        pin |= pins_of(failed)
        near_out.clear()
        far_out.clear()
    else:
        raise SystemExit("surfaces still failing after the last round")

    # ---- 4. written
    out_parts = []
    tot = dict(near_in=0, far_in=0, near_out=0, far_out=0)
    for pi, p in enumerate(parts):
        base = [i for i, r in enumerate(surf) if r["pi"] == pi]
        near_l, far_l = [], []
        for i in base:
            r = surf[i]
            s = r["s"]
            T = p["tri"][p["sft"][s]:p["sft"][s + 1]]
            Fo = p["tri"][p["pairs"][s, 0]:p["pairs"][s, 0] + p["pairs"][s, 1]]
            tot["near_in"] += len(T)
            tot["far_in"] += len(Fo)
            if i in near_out:
                n = near_out[i]
                f = far_out[i] if far_out[i] is not None else n
                if len(f) >= len(n):
                    f = n
            else:
                n = T
                f = Fo
                # (an untouched surface keeps its far level only if it does not lose vertices its neighbours lost: a
                # kept surface's boundary is pinned, so its own far level is still edge to edge)
            near_l.append(n)
            far_l.append(f)
            tot["near_out"] += len(n)
            tot["far_out"] += len(f)
        nt = np.concatenate(near_l) if near_l else np.zeros((0, 3), np.int64)
        lengths = np.array([len(x) for x in near_l], np.int64)
        sft = np.concatenate([[0], np.cumsum(lengths)])
        pairs = np.zeros((len(near_l), 2), np.int64)
        fars, cursor = [], len(nt)
        for j, f in enumerate(far_l):
            if f is near_l[j] or (len(f) == len(near_l[j]) and np.array_equal(f, near_l[j])):
                pairs[j] = [sft[j], lengths[j]]
                continue
            pairs[j] = [cursor, len(f)]
            cursor += len(f)
            fars.append(f)
        alltri = np.concatenate([nt] + fars) if fars else nt
        used = np.unique(alltri)
        remap = np.full(len(p["verts"]), -1, np.int64)
        remap[used] = np.arange(len(used))
        bounds = p["bounds"] if p["bounds"] is not None else np.zeros((p["psc"], 4), np.float32)
        out_parts.append(dict(rec=p["rec"], pfs=p["pfs"], psc=p["psc"], ntc=len(nt), dist=float(p["dist"]) * FAR_SCALE,
                              sft=sft, verts=p["verts"][used], lmv=p["lmv"][used], tri=remap[alltri], pairs=pairs,
                              bounds=bounds))
    size = write_pwg(dst, header, out_parts)
    report.update(tot, bytes=size, kept_surfaces=sum(1 for r in surf if r["loop"] is None), surfaces=len(surf))
    json.dump(report, open(report_path, "w"), indent=1)
    log("wrote", dst, json.dumps(tot), "kept", report["kept_surfaces"], "of", len(surf))


if __name__ == "__main__":
    main()
