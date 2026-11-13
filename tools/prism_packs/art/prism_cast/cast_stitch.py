#!/usr/bin/env python3
"""cast_stitch.py <ce jms dir> <in jms dir> <out jms dir> --base <perm> [--tol 0.015]: the remodel's cracks closed.

elite_build_r2.py builds each region apart and welds a region only within each dominant node, so where CE's mesh
was whole across two regions or two nodes (a hip, a neck, a thigh plate on its pelvis) the two sides are remodelled
apart: subdivided, clamped, cut and set out each on its own, they part by a millimetre or a centimetre and the
world shows through (the Elite's "transparent lines"). Here, for each level of detail:

  - the open edges of the character as drawn (the --base permutation's file; vertices welded at 1e-5 wu) that are
    not CE's own open edges are cracks; their vertices are moved, together with every corner at the same place,
    half way to the nearest point of the crack edges facing them (not their own edge's neighbours) within --tol,
    six times over: the two sides meet on one line;
  - every other permutation's file (a helmet, a head) is stitched against the base drawn with it: its crack
    vertices go all the way onto the base's open edges (the base is not moved again: it is drawn with all of them).

Only positions change (normals, UVs and weights stay). Prints the open edge length before and after per file.
Everything here is CE's data: local only.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import halo_formats as hf  # noqa: E402
import cast_cracks as cc  # noqa: E402

WELD = 1e-5


def welded(P, T):
    key = np.round(P / WELD).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    return inv.reshape(-1)


def boundary(inv, T):
    t = inv[T]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    e = np.sort(np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]]), axis=1)
    u, c = np.unique(e, axis=0, return_counts=True)
    return u[c == 1]


def components(inv, T, n):
    """each welded vertex's connected component (by triangles)"""
    parent = np.arange(n)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    t = inv[T]
    for a_, b_ in np.concatenate([t[:, [0, 1]], t[:, [1, 2]]]):
        ra, rb = find(a_), find(b_)
        if ra != rb:
            parent[ra] = rb
    return np.array([find(i) for i in range(n)])


def seg_dist(points, a, b):
    """distance from each point (n,3) to each segment (m,3)-(m,3): (n,m) distances and the closest points"""
    ab = b - a
    L = np.maximum((ab * ab).sum(1), 1e-20)
    ap = points[:, None, :] - a[None, :, :]
    t = np.clip((ap * ab[None]).sum(2) / L[None], 0, 1)
    c = a[None] + t[..., None] * ab[None]
    return np.linalg.norm(points[:, None, :] - c, axis=2), c


def on_polyline(points, a, b, tol=2e-4):
    if len(a) == 0:
        return np.zeros(len(points), bool)
    out = np.zeros(len(points), bool)
    for i in range(0, len(points), 256):
        d, _ = seg_dist(points[i:i + 256], a, b)
        out[i:i + 256] = d.min(1) < tol
    return out


def stitch(P, T, ce_a, ce_b, tol, fixed_a=None, fixed_b=None, rounds=16):
    """P moved (a copy): crack vertices half way to the facing crack edges, or (fixed_*) all the way onto them"""
    P = P.copy()
    inv = welded(P, T)
    nW = inv.max() + 1
    for _ in range(rounds):
        W = np.zeros((nW, 3))
        np.add.at(W, inv, P)
        W /= np.bincount(inv, minlength=nW)[:, None]
        ob = boundary(inv, T)
        if not len(ob):
            break
        a, b = W[ob[:, 0]], W[ob[:, 1]]
        mid = (a + b) * 0.5
        ce_edge = on_polyline(a, ce_a, ce_b) & on_polyline(b, ce_a, ce_b) & on_polyline(mid, ce_a, ce_b)
        crack = ob[~ce_edge]
        if not len(crack):
            break
        cv = np.unique(crack)
        comp = components(inv, T, nW)
        if fixed_a is not None:
            ta, tb = fixed_a, fixed_b
            own = None
        else:
            ta, tb = W[crack[:, 0]], W[crack[:, 1]]
            own = crack
        move = np.zeros((nW, 3))
        moved = 0
        for i in range(0, len(cv), 256):
            v = cv[i:i + 256]
            d, c = seg_dist(W[v], ta, tb)
            if own is not None:
                # (not its own side: crack edges of the same connected piece)
                d = np.where(comp[own[:, 0]][None, :] == comp[v][:, None], np.inf, d)
            k = d.argmin(1)
            dm = d[np.arange(len(v)), k]
            ok = (dm < tol) & (dm > 1e-7)
            target = c[np.arange(len(v)), k]
            f = 1.0 if own is None else 0.5
            move[v[ok]] = (target[ok] - W[v[ok]]) * f
            moved += int(ok.sum())
        if not moved:
            break
        P += move[inv]
    return P


def gaps(paths, ce_paths, tol=0.02):
    """the cracks of a character as drawn: open edges not on CE's open edges, each with the width of the gap at
    its middle (the distance to the nearest open edge that does not touch it, under tol); returns the total length
    of crack edges with a gap wider than 0.0005 wu (0.15 cm) and the widths' p50, p95"""
    P, T = cc.load(paths)
    inv = welded(P, T)
    W = np.zeros((inv.max() + 1, 3))
    W[inv] = P
    ob = boundary(inv, T)
    cP, cT = cc.load(ce_paths)
    cinv = welded(cP, cT)
    cW = np.zeros((cinv.max() + 1, 3))
    cW[cinv] = cP
    cob = boundary(cinv, cT)
    a, b = W[ob[:, 0]], W[ob[:, 1]]
    m = (a + b) * 0.5
    ce_edge = on_polyline(a, cW[cob[:, 0]], cW[cob[:, 1]]) & on_polyline(b, cW[cob[:, 0]], cW[cob[:, 1]])
    widths, lengths = [], []
    for i in range(0, len(ob), 256):
        d, _ = seg_dist(m[i:i + 256], a, b)
        e = ob[i:i + 256]
        touch = np.isin(ob[None, :, 0], e) | np.isin(ob[None, :, 1], e)
        own = (ob[None, :, 0] == e[:, 0:1]) | (ob[None, :, 1] == e[:, 0:1]) | (ob[None, :, 0] == e[:, 1:2]) | \
            (ob[None, :, 1] == e[:, 1:2])
        d = np.where(own, np.inf, d)
        widths.append(d.min(1))
    w = np.concatenate(widths) if widths else np.zeros(0)
    L = np.linalg.norm(b - a, axis=1)
    crack = (~ce_edge) & (w < tol)
    vis = crack & (w > 0.0005)
    return {"crack_edges": int(crack.sum()), "visible_crack_length_wu": round(float(L[vis].sum()), 4),
            "width_p50_wu": round(float(np.median(w[crack])), 5) if crack.any() else 0.0,
            "width_p95_wu": round(float(np.percentile(w[crack], 95)), 5) if crack.any() else 0.0}


def closest_on_triangles(p, A, B, C):
    """closest points of triangles (m) to each point (n): (n, m, 3) via Ericson's regions, vectorised"""
    ab, ac, ap = B - A, C - A, p[:, None, :] - A[None]
    d1 = (ab[None] * ap).sum(2); d2 = (ac[None] * ap).sum(2)
    bp = p[:, None, :] - B[None]; d3 = (ab[None] * bp).sum(2); d4 = (ac[None] * bp).sum(2)
    cp = p[:, None, :] - C[None]; d5 = (ab[None] * cp).sum(2); d6 = (ac[None] * cp).sum(2)
    va = d3 * d6 - d5 * d4; vb = d5 * d2 - d1 * d6; vc = d1 * d4 - d3 * d2
    den = np.where(np.abs(va + vb + vc) < 1e-30, 1e-30, va + vb + vc)
    v = vb / den; w = vc / den
    out = A[None] + ab[None] * v[..., None] + ac[None] * w[..., None]
    # edges and corners
    def seg(P0, P1):
        e = P1 - P0
        t = np.clip(((p[:, None, :] - P0[None]) * e[None]).sum(2) / np.maximum((e * e).sum(1), 1e-30)[None], 0, 1)
        return P0[None] + t[..., None] * e[None]
    inside = (va >= 0) & (vb >= 0) & (vc >= 0)
    cands = [seg(A, B), seg(B, C), seg(C, A)]
    best = out
    bd = np.where(inside, np.linalg.norm(p[:, None, :] - out, axis=2), np.inf)
    for c in cands:
        d = np.linalg.norm(p[:, None, :] - c, axis=2)
        m = d < bd
        best = np.where(m[..., None], c, best)
        bd = np.where(m, d, bd)
    return best, bd


def snap_t_junctions(P, T, ce_a, ce_b, tol=0.008, near=48):
    """vertices of open edges that are CE's own open edges (CE's T-junctions: a boundary laid against another
    piece's face) put back on the nearest surface of another piece within tol, where the remodel moved that surface
    away (a plate set out, a bodysuit set in)"""
    P = P.copy()
    inv = welded(P, T)
    nW = inv.max() + 1
    W = np.zeros((nW, 3))
    np.add.at(W, inv, P)
    W /= np.bincount(inv, minlength=nW)[:, None]
    ob = boundary(inv, T)
    if not len(ob):
        return P, 0
    a, b = W[ob[:, 0]], W[ob[:, 1]]
    ce_edge = on_polyline(a, ce_a, ce_b) & on_polyline(b, ce_a, ce_b)
    cv = np.unique(ob[ce_edge])
    if not len(cv):
        return P, 0
    comp = components(inv, T, nW)
    t = inv[T]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    tcomp = comp[t[:, 0]]
    cen = W[t].mean(1)
    move = np.zeros((nW, 3))
    n = 0
    for i in range(0, len(cv), 64):
        v = cv[i:i + 64]
        dc = np.linalg.norm(W[v][:, None, :] - cen[None], axis=2)
        dc = np.where(tcomp[None, :] == comp[v][:, None], np.inf, dc)
        idx = np.argsort(dc, axis=1)[:, :near]
        for r, vv in enumerate(v):
            k = idx[r][np.isfinite(dc[r, idx[r]])]
            if not len(k):
                continue
            tri = t[k]
            c, d = closest_on_triangles(W[vv][None], W[tri[:, 0]], W[tri[:, 1]], W[tri[:, 2]])
            j = d[0].argmin()
            if 1e-6 < d[0, j] < tol:
                move[vv] = c[0, j] - W[vv]
                n += 1
    P += move[inv]
    return P, n


def jms_arrays(j):
    P = np.array([v.pos for v in j.vertices], np.float64) / 100.0
    T = np.array([t.v for t in j.triangles], np.int64)
    return P, T


def write_moved(j, P, path):
    j.vertices = [hf.JmsVertex(v.node0, tuple(p * 100.0), v.normal, v.node1, v.weight, v.u, v.v)
                  for v, p in zip(j.vertices, P)]
    hf.write_jms(path, j)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ce_dir")
    ap.add_argument("in_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--base", required=True)
    ap.add_argument("--tol", type=float, default=0.015)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    files = {}
    for f in os.listdir(a.in_dir):
        if f.endswith(".jms"):
            perm, lod = f[:-4].rsplit(" ", 1)
            files.setdefault(lod, []).append(perm)
    report = {}
    for lod, perms in sorted(files.items()):
        ce = hf.read_jms(os.path.join(a.ce_dir, "%s %s.jms" % (a.base, lod)))
        cP, cT = jms_arrays(ce)
        cinv = welded(cP, cT)
        cW = np.zeros((cinv.max() + 1, 3))
        cW[cinv] = cP
        cob = boundary(cinv, cT)
        ce_a, ce_b = cW[cob[:, 0]], cW[cob[:, 1]]
        bpath = os.path.join(a.in_dir, "%s %s.jms" % (a.base, lod))
        bj = hf.read_jms(bpath)
        bP, bT = jms_arrays(bj)
        before = cc.report([bpath])["open_length_wu"]
        bP2 = stitch(bP, bT, ce_a, ce_b, a.tol)
        bP2, nt = snap_t_junctions(bP2, bT, ce_a, ce_b)
        report["%s %s t-junctions snapped" % (a.base, lod)] = nt
        out = os.path.join(a.out_dir, "%s %s.jms" % (a.base, lod))
        write_moved(bj, bP2, out)
        report["%s %s" % (a.base, lod)] = [before, cc.report([out])["open_length_wu"],
                                           cc.report([os.path.join(a.ce_dir, "%s %s.jms" % (a.base, lod))])["open_length_wu"]]
        binv = welded(bP2, bT)
        bW = np.zeros((binv.max() + 1, 3))
        bW[binv] = bP2
        bob = boundary(binv, bT)
        fa, fb = bW[bob[:, 0]], bW[bob[:, 1]]
        for perm in perms:
            if perm == a.base:
                continue
            path = os.path.join(a.in_dir, "%s %s.jms" % (perm, lod))
            j = hf.read_jms(path)
            P, T = jms_arrays(j)
            cep = os.path.join(a.ce_dir, "%s %s.jms" % (perm, lod))
            both_before = cc.report([out, path])["open_length_wu"]
            # (its own cracks first, then onto the base)
            cj = hf.read_jms(cep)
            qP, qT = jms_arrays(cj)
            # CE's open edges of the pair as drawn together (its own and the base's)
            allP = np.concatenate([cP, qP])
            allT = np.concatenate([cT, qT + len(cP)])
            ainv = welded(allP, allT)
            aW = np.zeros((ainv.max() + 1, 3))
            aW[ainv] = allP
            aob = boundary(ainv, allT)
            P = stitch(P, T, aW[aob[:, 0]], aW[aob[:, 1]], a.tol)
            P = stitch(P, T, aW[aob[:, 0]], aW[aob[:, 1]], a.tol, fixed_a=fa, fixed_b=fb, rounds=1)
            P, nt = snap_t_junctions(P, T, aW[aob[:, 0]], aW[aob[:, 1]])
            report["%s %s t-junctions snapped" % (perm, lod)] = nt
            o = os.path.join(a.out_dir, "%s %s.jms" % (perm, lod))
            write_moved(j, P, o)
            report["%s+%s %s" % (a.base, perm, lod)] = [both_before, cc.report([out, o])["open_length_wu"],
                                                        cc.report([os.path.join(a.ce_dir, "%s %s.jms" % (a.base, lod)), cep])["open_length_wu"]]
    print(json.dumps(report, indent=0))


if __name__ == "__main__":
    main()
