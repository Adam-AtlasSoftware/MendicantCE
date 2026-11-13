"""PRISM world geometry v2: a written remodel (.pwg) made watertight where its surfaces meet.

usage: python3 art/world_geo/wg2_seal.py <bsp.gltf> <in.pwg> <out.pwg> <report.json>
environment: WG_SEAL_TOLERANCE (wu: how far off an edge a neighbour's vertex may lie and still be its, default 0.03),
       WG_SEAL_ROUNDS (default 8)

The rasteriser draws two triangles that share an edge without a gap only when the edge is the same two vertices on
both sides. The remodel broke that in three ways, each a line of see-through pixels (the sky or the level behind
showing through a cliff or a wall):
  - T-junctions: one side of a stock edge cut at points the other is not (metal grooves cut along each surface's own
    texture lines, which part where the texture does not run on across the edge; bevels and the displaced classes'
    splits against a material that stays stock, the water's 'underwater' under the beach): the cut side's vertices
    lie on the other's longer edge, and the pixels along it are covered by neither;
  - small gaps: the same, the vertices a little off the edge (a bevel's clamp, a cut's rounding);
  - a material that stays stock beside one that is remodelled: its triangles keep the stock edges.

Here every near triangle of the file, and the stock (opaque environment) materials the file leaves out, are welded
by position (0.1 mm); each edge that only one triangle uses (an open edge) and that has another open edge's vertices
lying along it (within 2 mm, or within the tolerance where the other edge runs the same way) is split at them: its triangle is fanned
from its opposite corner, the new corners taking the triangle's own coordinates, normals and light at that point of
the edge and the neighbour's exact position, so both sides then share every vertex. Rounds repeat until no edge is
split. Small holes left then (a loop of at most 12 open edges and 0.6 wu, not on the stock BSP's own border: a bevel's
end, a dropped sliver) are closed by a fan. A stock material that had to be split is written into the file as a remodel of itself (its own vertices, its
surfaces split where they meet the remodel), which the engine then draws in its place. Far levels that were the near
triangles follow them; a surface's own far level (wg2_lod.py's, which keeps its near level's edge vertices) is split
at the vertices the seal gave its near level's edges, so that near and far still meet exactly. wg2_far2.py, run after
this, then thins far levels to their corners' chords, with skirts where a far surface meets a near one.

Everything it reads and writes is derived from the player's map: local only.
"""
import json
import os
import struct
import sys
import time
from collections import defaultdict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wg_common as wg  # noqa: E402

T0 = time.time()
TOL = float(os.environ.get("WG_SEAL_TOLERANCE", "0.03"))
# (a vertex this close to an edge is on it whichever way its own edges run: a T-junction)
TIGHT = 2e-3
# (open edges' vertices this close are one point)
SNAP = 6e-4
# the largest hole filled: its open edges and its perimeter (wu)
HOLE_EDGES = 12
HOLE_PERIMETER = 0.6
ROUNDS = int(os.environ.get("WG_SEAL_ROUNDS", "8"))
WELD = 1e-4


def log(*a):
    print("WGS %7.1fs" % (time.time() - T0), *a, flush=True)


def pad4(n):
    return (n + 3) & ~3


def read_pwg(path):
    b = open(path, "rb").read()
    ver, count, flags = struct.unpack_from("<III", b, 4)
    far = ver >= 2 and bool(flags & 1)
    off, parts = 64, []
    for _ in range(count):
        rec = b[off:off + 64]
        vc, tc = struct.unpack_from("<II", rec, 12)
        pfs, psc = struct.unpack_from("<II", rec, 32)
        ntc, dist = struct.unpack_from("<If", rec, 40) if far else (tc, 0.0)
        off += 64
        p = dict(rec=rec, lm=struct.unpack_from("<h", rec, 0)[0], mi=struct.unpack_from("<h", rec, 2)[0],
                 pfs=pfs, psc=psc, ntc=ntc, dist=dist)
        p["sft"] = np.frombuffer(b, "<u4", psc + 1, off).astype(np.int64); off += pad4((psc + 1) * 4)
        p["verts"] = np.frombuffer(b, "<f4", vc * 14, off).reshape(vc, 14).copy(); off += pad4(vc * 56)
        p["lmv"] = np.frombuffer(b, "<f4", vc * 5, off).reshape(vc, 5).copy(); off += pad4(vc * 20)
        p["tri"] = np.frombuffer(b, "<u2", tc * 3, off).reshape(tc, 3).astype(np.int64); off += pad4(tc * 6)
        if far:
            p["pairs"] = np.frombuffer(b, "<u4", psc * 2, off).reshape(psc, 2).astype(np.int64); off += psc * 8
            p["bounds"] = np.frombuffer(b, "<f4", psc * 4, off).reshape(psc, 4).copy(); off += psc * 16
        else:
            p["pairs"] = np.stack([p["sft"][:-1], p["sft"][1:] - p["sft"][:-1]], 1)
            p["bounds"] = None
        parts.append(p)
    return b[:64], far, parts


def surface_bounds(P, tris_by_surface):
    out = np.zeros((len(tris_by_surface), 4), np.float32)
    for s, t in enumerate(tris_by_surface):
        pts = P[np.unique(t)] if len(t) else np.zeros((1, 3))
        lo, hi = pts.min(0), pts.max(0)
        c = (lo + hi) * 0.5
        out[s] = [c[0], c[1], c[2], float(np.linalg.norm(pts - c, axis=1).max())]
    return out


class Mesh:
    """the near triangles of every part (and of the candidate stock materials) as one list: per triangle its part
    and surface; per part its rows (vertices and lightmap vertices, grown as splits add corners)"""

    def __init__(self, parts):
        self.parts = parts
        self.tri_part, self.tri_surf, self.tri_rows = [], [], []
        for pi, p in enumerate(parts):
            for s in range(p["psc"]):
                t = p["tri"][p["sft"][s]:p["sft"][s + 1]]
                self.tri_rows.append(t)
                self.tri_part.append(np.full(len(t), pi))
                self.tri_surf.append(np.full(len(t), s))
            p["rows_v"] = [p["verts"]]
            p["rows_l"] = [p["lmv"]]
            p["nrows"] = len(p["verts"])
        self.tri_rows = np.concatenate(self.tri_rows)
        self.tri_part = np.concatenate(self.tri_part)
        self.tri_surf = np.concatenate(self.tri_surf)

    def positions(self):
        """every triangle corner's position (n, 3, 3), float64"""
        out = np.zeros((len(self.tri_rows), 3, 3))
        for pi, p in enumerate(self.parts):
            sel = np.nonzero(self.tri_part == pi)[0]
            if not len(sel):
                continue
            V = np.concatenate(p["rows_v"])
            out[sel] = V[self.tri_rows[sel]][:, :, 0:3]
        return out

    def add_row(self, pi, v, l):
        p = self.parts[pi]
        p["rows_v"].append(v[None].astype(np.float32))
        p["rows_l"].append(l[None].astype(np.float32))
        p["nrows"] += 1
        return p["nrows"] - 1

    def row(self, pi, r):
        p = self.parts[pi]
        if r < len(p["verts"]):
            return p["verts"][r].astype(np.float64), p["lmv"][r].astype(np.float64)
        k = r - len(p["verts"])
        return p["rows_v"][1 + k][0].astype(np.float64), p["rows_l"][1 + k][0].astype(np.float64)


def lerp_row(va, la, vb, lb, t, pos):
    v = va + (vb - va) * t
    l = la + (lb - la) * t
    for a, b in ((3, 6), (6, 9), (9, 12)):
        n = np.linalg.norm(v[a:b])
        if n > 1e-9:
            v[a:b] /= n
    v[0:3] = pos
    return v, l


def set_position(mesh, pi, r, pos):
    p = mesh.parts[pi]
    if r < len(p["verts"]):
        p["verts"][r, 0:3] = pos
    else:
        p["rows_v"][1 + r - len(p["verts"])][0, 0:3] = pos


def snap_open(mesh, report):
    """open edges' vertices closer than SNAP to each other (a bevel's clamp, a cut's rounding: two corners that are
    one point) moved onto one; returns how many moved"""
    X = mesh.positions()
    n = len(X)
    flat = X.reshape(-1, 3)
    _, first, wid = np.unique(np.round(flat / WELD).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    wid = wid.ravel()
    W = flat[first]
    F = wid.reshape(n, 3)
    E = np.sort(np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]]), 1)
    E = E[E[:, 0] != E[:, 1]]
    u, c = np.unique(E, axis=0, return_counts=True)
    ov = np.unique(u[c == 1])
    if not len(ov):
        return 0
    grid = defaultdict(list)
    for v, cl in zip(ov, map(tuple, np.floor(W[ov] / SNAP).astype(np.int64))):
        grid[cl].append(int(v))
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            x = parent[x]
        return x
    for cl, vs in grid.items():
        near = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    near += grid.get((cl[0] + dx, cl[1] + dy, cl[2] + dz), [])
        for v in vs:
            for w in near:
                if w > v and np.linalg.norm(W[w] - W[v]) < SNAP:
                    a, b = find(v), find(w)
                    if a != b:
                        parent[max(a, b)] = min(a, b)
    moved = {v: find(v) for v in list(parent)}
    moved = {v: r for v, r in moved.items() if r != v}
    if not moved:
        return 0
    corners = np.nonzero(np.isin(wid, np.array(sorted(moved), np.int64)))[0]
    for ci in corners:
        t, k = divmod(int(ci), 3)
        set_position(mesh, int(mesh.tri_part[t]), int(mesh.tri_rows[t, k]), W[moved[int(wid[ci])]])
    report["snapped"] = report.get("snapped", 0) + len(moved)
    return len(moved)


def seal_round(mesh, report):
    X = mesh.positions()
    n = len(X)
    flat = X.reshape(-1, 3)
    key = np.round(flat / WELD).astype(np.int64)
    _, first, wid = np.unique(key, axis=0, return_index=True, return_inverse=True)
    wid = wid.ravel()
    W = flat[first]
    F = wid.reshape(n, 3)
    good = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    et = np.concatenate([np.arange(n)] * 3)
    ek = np.concatenate([np.zeros(n, np.int64), np.ones(n, np.int64), np.full(n, 2)])
    ok = np.concatenate([good] * 3)
    E, et, ek = E[ok], et[ok], ek[ok]
    Es = np.sort(E, 1)
    _, inv, cnt = np.unique(Es, axis=0, return_inverse=True, return_counts=True)
    once = cnt[inv.ravel()] == 1
    oE, ot, ok_ = E[once], et[once], ek[once]
    # the open edges' vertices, with the directions of their open edges
    dirs = defaultdict(list)
    for (a, b) in oE:
        d = W[b] - W[a]
        ln = np.linalg.norm(d)
        if ln > 1e-9:
            dirs[int(a)].append(d / ln)
            dirs[int(b)].append(d / ln)
    ov = np.array(sorted(dirs), np.int64)
    if not len(ov):
        return 0
    cell = TOL * 4
    grid = defaultdict(list)
    for v, c in zip(ov, map(tuple, np.floor(W[ov] / cell).astype(np.int64))):
        grid[c].append(int(v))
    inserts = defaultdict(list)  # (triangle, corner k) -> [(t, welded vertex)]
    for (a, b), t_, k_ in zip(oE, ot, ok_):
        A, B = W[a], W[b]
        ab = B - A
        ll = float(ab @ ab)
        if ll < 1e-12:
            continue
        L = ll ** 0.5
        u = ab / L
        steps = int(L / (cell * 0.5)) + 2
        cand = set()
        for c in set(map(tuple, np.floor((A[None] + np.linspace(0, 1, steps)[:, None] * ab[None]) / cell).astype(np.int64))):
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        cand.update(grid.get((c[0] + dx, c[1] + dy, c[2] + dz), ()))
        cand.discard(int(a))
        cand.discard(int(b))
        if not cand:
            continue
        cv = np.array(sorted(cand))
        Pv = W[cv]
        t = ((Pv - A) @ ab) / ll
        d = np.linalg.norm(A + t[:, None] * ab - Pv, axis=1)
        sel = (t > 1e-4) & (t < 1 - 1e-4) & (d < TOL) & (t * L > WELD * 2) & ((1 - t) * L > WELD * 2)
        for v, tv, dv in zip(cv[sel], t[sel], d[sel]):
            # (the vertex's own open edges run along this one: a neighbour's chain, not a stray point)
            if dv < TIGHT or any(abs(float(dd @ u)) > 0.98 for dd in dirs[int(v)]):
                inserts[(int(t_), int(k_))].append((float(tv), int(v)))
    if not inserts:
        return 0
    # one edge a triangle a round: the one with most points; fanned from the opposite corner
    best = {}
    for (t_, k_), pts in inserts.items():
        if t_ not in best or len(pts) > len(best[t_][1]):
            best[t_] = (k_, pts)
    new_rows, new_part, new_surf = [], [], []
    drop = np.zeros(n, bool)
    made = 0
    for t_, (k_, pts) in best.items():
        pts = sorted(set(pts))
        pi = int(mesh.tri_part[t_])
        rows = mesh.tri_rows[t_]
        ra, rb, rc = int(rows[k_]), int(rows[(k_ + 1) % 3]), int(rows[(k_ + 2) % 3])
        va, la = mesh.row(pi, ra)
        vb, lb = mesh.row(pi, rb)
        chain = [ra]
        for tv, v in pts:
            vv, lv = lerp_row(va, la, vb, lb, tv, W[v])
            chain.append(mesh.add_row(pi, vv, lv))
        chain.append(rb)
        for i in range(len(chain) - 1):
            new_rows.append((chain[i], chain[i + 1], rc))
            new_part.append(pi)
            new_surf.append(int(mesh.tri_surf[t_]))
        drop[t_] = True
        made += len(pts)
    mesh.tri_rows = np.concatenate([mesh.tri_rows[~drop], np.array(new_rows, np.int64)])
    mesh.tri_part = np.concatenate([mesh.tri_part[~drop], np.array(new_part, np.int64)])
    mesh.tri_surf = np.concatenate([mesh.tri_surf[~drop], np.array(new_surf, np.int64)])
    report["edges_split"] += len(best)
    report["points_inserted"] += made
    return len(best)


def fill_holes(mesh, stock_open, report):
    """the small holes left (a bevel's end where it met a pinned corner, a cut's sliver dropped): each loop of open
    edges at most HOLE_EDGES long and HOLE_PERIMETER around, none of them on the stock BSP's own border, closed by a
    fan of triangles in the winding its neighbours have, on the rows of the part whose triangle owns its first edge
    (a corner it lacks copied from the part that has it)"""
    X = mesh.positions()
    n = len(X)
    flat = X.reshape(-1, 3)
    _, first, wid = np.unique(np.round(flat / WELD).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    wid = wid.ravel()
    W = flat[first]
    F = wid.reshape(n, 3)
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    et = np.concatenate([np.arange(n)] * 3)
    ek = np.concatenate([np.zeros(n, np.int64), np.ones(n, np.int64), np.full(n, 2)])
    ok = E[:, 0] != E[:, 1]
    E, et, ek = E[ok], et[ok], ek[ok]
    _, inv, cnt = np.unique(np.sort(E, 1), axis=0, return_inverse=True, return_counts=True)
    once = cnt[inv.ravel()] == 1
    nxt, owner = {}, {}
    bad = set()
    for (a, b), t_, k_ in zip(E[once], et[once], ek[once]):
        a, b = int(a), int(b)
        if a in nxt:
            bad.add(a)
        nxt[a] = b
        owner[a] = (int(t_), int(k_))
    seen = set()
    new_rows, new_part, new_surf = [], [], []
    filled = 0
    for start in list(nxt):
        if start in seen or start in bad:
            continue
        loop = [start]
        while len(loop) <= HOLE_EDGES:
            v = nxt.get(loop[-1])
            if v is None or v in bad:
                loop = None
                break
            if v == start:
                break
            if v in loop:
                loop = None
                break
            loop.append(v)
        seen.update(loop or [start])
        if not loop or len(loop) < 3 or len(loop) > HOLE_EDGES:
            continue
        P = W[loop]
        if np.linalg.norm(P - np.roll(P, -1, 0), axis=1).sum() > HOLE_PERIMETER:
            continue
        mids = 0.5 * (P + np.roll(P, -1, 0))
        if stock_open is not None and len(stock_open[0]):
            A_, B_ = stock_open
            on = False
            for m in mids:
                ab = B_ - A_
                t = np.clip(((m - A_) * ab).sum(1) / np.maximum((ab * ab).sum(1), 1e-18), 0, 1)
                if (np.linalg.norm(A_ + t[:, None] * ab - m, axis=1) < TIGHT).any():
                    on = True
                    break
            if on:
                continue
        t0, _ = owner[loop[0]]
        pi, si = int(mesh.tri_part[t0]), int(mesh.tri_surf[t0])
        rows = []
        for v in loop:
            t_, k_ = owner[v]
            r = int(mesh.tri_rows[t_, k_])
            if int(mesh.tri_part[t_]) != pi:
                vv, lv = mesh.row(int(mesh.tri_part[t_]), r)
                r = mesh.add_row(pi, vv, lv)
            rows.append(r)
        # (the loop runs as its neighbours wind their open edges; the fill winds the other way along them)
        for i in range(1, len(rows) - 1):
            new_rows.append((rows[0], rows[i + 1], rows[i]))
            new_part.append(pi)
            new_surf.append(si)
        filled += 1
    if new_rows:
        mesh.tri_rows = np.concatenate([mesh.tri_rows, np.array(new_rows, np.int64)])
        mesh.tri_part = np.concatenate([mesh.tri_part, np.array(new_part, np.int64)])
        mesh.tri_surf = np.concatenate([mesh.tri_surf, np.array(new_surf, np.int64)])
    report["holes_filled"] = filled
    report["hole_triangles"] = len(new_rows)
    return filled


def open_edge_stats(mesh):
    X = mesh.positions()
    flat = X.reshape(-1, 3)
    wid = np.unique(np.round(flat / WELD).astype(np.int64), axis=0, return_inverse=True)[1].ravel()
    F = wid.reshape(-1, 3)
    good = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    F = F[good]
    E = np.sort(np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]]), 1)
    _, c = np.unique(E, axis=0, return_counts=True)
    return int((c == 1).sum())


def conform_far(V, near, far, report):
    """a surface's own far level (wg2_lod.py's: its near level's edge vertices and a coarser inside) given every
    vertex its sealed near level has on its edges: each open edge of the far triangles split at the near level's
    edge rows lying on it (the same part's rows: the same positions, so near and far of neighbours meet exactly).
    Returns the far triangles."""
    def open_edges(T):
        E = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
        key = np.sort(E, 1)
        _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
        once = cnt[inv.ravel()] == 1
        return E[once], np.concatenate([np.arange(len(T))] * 3)[once], np.concatenate(
            [np.zeros(len(T), np.int64), np.ones(len(T), np.int64), np.full(len(T), 2)])[once]
    nE, _, _ = open_edges(near)
    cand = np.unique(nE)
    if not len(cand):
        return far
    T = far.copy()
    for _ in range(4):
        fE, ft, fk = open_edges(T)
        Pc = V[cand, 0:3].astype(np.float64)
        best = {}
        for (a, b), t_, k_ in zip(fE, ft, fk):
            A, B = V[a, 0:3].astype(np.float64), V[b, 0:3].astype(np.float64)
            ab = B - A
            ll = float(ab @ ab)
            if ll < 1e-12:
                continue
            t = ((Pc - A) @ ab) / ll
            d = np.linalg.norm(A + t[:, None] * ab - Pc, axis=1)
            sel = (t > 1e-4) & (t < 1 - 1e-4) & (d < TIGHT) & (cand != a) & (cand != b)
            if sel.any():
                pts = sorted(zip(t[sel].tolist(), cand[sel].tolist()))
                if t_ not in best or len(pts) > len(best[t_][1]):
                    best[int(t_)] = (int(k_), pts)
        if not best:
            break
        new, drop = [], np.zeros(len(T), bool)
        for t_, (k_, pts) in best.items():
            ra, rb, rc = int(T[t_, k_]), int(T[t_, (k_ + 1) % 3]), int(T[t_, (k_ + 2) % 3])
            chain = [ra] + [r for _, r in pts] + [rb]
            for i in range(len(chain) - 1):
                new.append((chain[i], chain[i + 1], rc))
            drop[t_] = True
            report["far_points_inserted"] = report.get("far_points_inserted", 0) + len(pts)
        T = np.concatenate([T[~drop], np.array(new, np.int64)])
    return T


def assemble(p, verts, lmv, near, far, report, first=0, count=None):
    """records for surfaces [first, first + count) of a part (split in two while past 65535 vertices)"""
    count = p["psc"] if count is None else count
    nr = near[first:first + count]
    fr = far[first:first + count]
    used = np.unique(np.concatenate([np.concatenate(nr).ravel(), np.concatenate([f for f in fr]).ravel()]))
    if len(used) > 0xFFFF:
        if count < 2:
            raise SystemExit("a surface needs more than 65535 vertices")
        report["split_parts"] += 1
        h = count // 2
        return (assemble(p, verts, lmv, near, far, report, first, h) +
                assemble(p, verts, lmv, near, far, report, first + h, count - h))
    remap = np.full(len(verts), -1, np.int64)
    remap[used] = np.arange(len(used))
    nt = np.concatenate(nr)
    lengths = np.array([len(x) for x in nr])
    sft = np.concatenate([[0], np.cumsum(lengths)])
    pairs = np.zeros((count, 2), np.int64)
    extra = []
    cursor = len(nt)
    for j, f in enumerate(fr):
        if f is nr[j]:
            pairs[j] = [sft[j], lengths[j]]
            continue
        pairs[j] = [cursor, len(f)]
        cursor += len(f)
        extra.append(f)
    alltri = np.concatenate([nt] + extra) if extra else nt
    V = verts[used]
    bounds = p["bounds"][first:first + count] if p.get("bounds") is not None else \
        surface_bounds(V[:, 0:3].astype(np.float64), [remap[x] for x in nr])
    return [dict(rec=p["rec"], pfs=p["pfs"] + first, psc=count, ntc=len(nt), dist=p["dist"], sft=sft, verts=V,
                 lmv=lmv[used], tri=remap[alltri], pairs=pairs, bounds=bounds)]


def write(path, header, parts):
    out = bytearray(header)
    struct.pack_into("<III", out, 4, 2, len(parts), 1)

    def arr(x):
        bts = x.tobytes()
        out.extend(bts + b"\0" * (pad4(len(bts)) - len(bts)))
    for p in parts:
        rec = bytearray(p["rec"])
        assert len(p["verts"]) <= 0xFFFF
        struct.pack_into("<II", rec, 12, len(p["verts"]), len(p["tri"]))
        struct.pack_into("<II", rec, 32, p["pfs"], p["psc"])
        struct.pack_into("<If", rec, 40, p["ntc"], p["dist"])
        out += rec
        arr(p["sft"].astype("<u4"))
        arr(p["verts"].astype("<f4"))
        arr(p["lmv"].astype("<f4"))
        arr(p["tri"].astype("<u2"))
        out += p["pairs"].astype("<u4").tobytes()
        out += p["bounds"].astype("<f4").tobytes()
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return len(out)


def stock_part(m):
    """a stock material as a part of its own (its vertices, one triangle a surface), written only if split"""
    rows = np.concatenate([m.position, m.normal, m.binormal, m.tangent, m.uv0], 1).astype(np.float32)
    lmv = np.concatenate([m.radiosity, m.uv1], 1).astype(np.float32)
    rec = bytearray(64)
    struct.pack_into("<hhIIIII", rec, 0, m.lightmap_index, m.material_index, m.surface_count, m.vertex_count, 0, 0,
                     wg.topology_check(m.triangles))
    struct.pack_into("<dII", rec, 24, wg.position_check(m.position), 0, m.surface_count)
    return dict(rec=bytes(rec), lm=m.lightmap_index, mi=m.material_index, pfs=0, psc=m.surface_count,
                ntc=m.surface_count, dist=0.0, sft=np.arange(m.surface_count + 1, dtype=np.int64), verts=rows,
                lmv=lmv, tri=m.triangles.astype(np.int64),
                pairs=np.stack([np.arange(m.surface_count), np.ones(m.surface_count, np.int64)], 1), bounds=None,
                stock=True)


def main():
    gltf, src, dst, report_path = sys.argv[1:5]
    info, mats = wg.load_bsp(gltf)
    header, far, parts = read_pwg(src)
    log("read", src, len(parts), "parts")
    done = {(p["lm"], p["mi"]) for p in parts}
    for p in parts:
        p["stock"] = False
    cands = [stock_part(m) for m in mats if m.shader_group == "senv" and (m.lightmap_index, m.material_index) not in done
             and m.surface_count > 0]
    log("stock materials beside the remodel", len(cands))
    allp = parts + cands
    mesh = Mesh(allp)
    report = {"tolerance": TOL, "edges_split": 0, "points_inserted": 0, "split_parts": 0, "rounds": [],
              "open_edges_before": open_edge_stats(mesh)}
    log("open edges before", report["open_edges_before"])
    for r in range(ROUNDS):
        k = snap_open(mesh, report)
        log("round", r, "vertices snapped", k)
        k += seal_round(mesh, report)
        report["rounds"].append(k)
        log("round", r, "edges split", k)
        if not k:
            break
    # (the stock BSP's own open edges: a hole there is the level's, left open)
    sp, st, sb = [], [], 0
    for m in mats:
        if m.shader_group == "senv":
            sp.append(m.position.astype(np.float64))
            st.append(m.triangles + sb)
            sb += m.vertex_count
    SP = np.concatenate(sp)
    sw_first, sw = np.unique(np.round(SP / WELD).astype(np.int64), axis=0, return_index=True, return_inverse=True)[1:]
    sw = sw.ravel()
    SF = sw[np.concatenate(st)]
    SE = np.sort(np.concatenate([SF[:, [0, 1]], SF[:, [1, 2]], SF[:, [2, 0]]]), 1)
    su, sc = np.unique(SE, axis=0, return_counts=True)
    so = su[sc == 1]
    SW = SP[sw_first]
    fill_holes(mesh, (SW[so[:, 0]], SW[so[:, 1]]), report)
    log("holes filled", report["holes_filled"], "triangles", report["hole_triangles"])
    report["open_edges_after"] = open_edge_stats(mesh)
    log("open edges after", report["open_edges_after"])
    out_parts = []
    added = []
    for pi, p in enumerate(allp):
        sel = np.nonzero(mesh.tri_part == pi)[0]
        changed = len(p["rows_v"]) > 1 or len(sel) != p["sft"][-1]
        if p["stock"] and not changed:
            continue
        verts = np.concatenate(p["rows_v"])
        lmv = np.concatenate(p["rows_l"])
        order = sel[np.argsort(mesh.tri_surf[sel], kind="stable")]
        surf = mesh.tri_surf[order]
        bnd = np.searchsorted(surf, np.arange(p["psc"] + 1))
        near = [mesh.tri_rows[order[bnd[s]:bnd[s + 1]]] for s in range(p["psc"])]
        farl = []
        for s in range(p["psc"]):
            a, c = p["pairs"][s]
            if a == p["sft"][s] and c == p["sft"][s + 1] - p["sft"][s]:
                farl.append(near[s])
            else:
                farl.append(conform_far(verts, near[s], p["tri"][a:a + c], report))
        if p["stock"]:
            added.append("%d/%d" % (p["lm"], p["mi"]))
        out_parts.extend(assemble(p, verts, lmv, near, farl, report))
    size = write(dst, header, out_parts)
    report["stock_materials_added"] = added
    report["parts"] = len(out_parts)
    report["bytes"] = size
    report["seconds"] = round(time.time() - T0, 1)
    json.dump(report, open(report_path, "w"), indent=1)
    log("wrote", dst, json.dumps(report))


if __name__ == "__main__":
    main()
