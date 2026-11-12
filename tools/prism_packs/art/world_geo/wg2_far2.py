"""PRISM world geometry v2: a cheaper far level of detail for a written remodel (.pwg).

usage: blender -b --factory-startup --python art/world_geo/wg2_far2.py -- <in.pwg> <out.pwg> <report.json>
environment: WG_FAR_EDGES ("chain", the default, or "chord"), WG_FAR_SPACING (wu between the far level's inner
       vertices, default 1.2), WG_FAR_METAL_DISTANCE (the far distance of a part that had no far level of its own,
       default 16), WG_FAR_SKIRT_MIN and WG_FAR_SKIRT_DEPTH ("chord": the least edge deviation given skirts, default 0,
       and their depth past it, default 0.012 wu)

Run after wg2_seal.py, which makes the near level watertight (every edge two surfaces share is the same vertices on
both sides). Each surface's far level is then, with WG_FAR_EDGES:

  "chain" (the default): its near level's whole boundary loop (every vertex its neighbours have on its edges: the same
    positions on both sides) and only some of its inner vertices (one per cell of WG_FAR_SPACING, away from the loop),
    triangulated again inside the loop (Blender's constrained Delaunay) in its corners' plane, the result's open edges
    checked to be the loop's own. Near and far surfaces meet edge to edge whichever each is drawn at: no crack, no
    skirt. A surface whose loop folds seen in that plane, whose triangulation fails that check, or whose new far level
    would not be fewer triangles than its near level is drawn far as it is near (its older far level, wg2_lod.py's,
    is not kept: it dropped edge vertices its undisplaced positions put a hair off the stock edge, leaving chords
    against a neighbour's edge: the long lenses of sky through b30's and a30's far cliffs);
  "chord" (fewer triangles): its three corners' chords and inner vertices, with two skirts along each edge whose near
    level has more than its chord, back into the solid, one leaning under the neighbour and facing it, one leaning
    under this surface and facing this side (a neighbour drawn near meets this edge with all its vertices; each skirt
    a back face from the other side). A neighbour's edge that strays far from the chord still leaves a lens of sky the
    skirts do not always close.

A part that the new vertices would take past 65535 is split in two at a surface (records of one material follow each
other, as the engine reads). The vertices are the remodel's own rows (and the skirts' copies): everything derived from
the player's map; local only.
"""
import json
import os
import sys
import time

import numpy as np
from mathutils import Vector, geometry

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

T0 = time.time()
SPACING = float(os.environ.get("WG_FAR_SPACING", "1.2"))
METAL_DISTANCE = float(os.environ.get("WG_FAR_METAL_DISTANCE", "16"))
SKIRT_MIN = float(os.environ.get("WG_FAR_SKIRT_MIN", "0.0"))
SKIRT_DEPTH = float(os.environ.get("WG_FAR_SKIRT_DEPTH", "0.012"))
LEAN = 0.35
# "chain" (default): a far surface keeps its whole boundary (crack-free with any neighbour, no skirts); "chord": its
# corners' chords with skirts (fewer triangles; a neighbour's edge that strays from the chord leaves a lens of sky
# the skirts do not always close)
EDGES = os.environ.get("WG_FAR_EDGES", "chain")


def log(*a):
    print("WGF %7.1fs" % (time.time() - T0), *a, flush=True)


def pad4(n):
    return (n + 3) & ~3


def read(path):
    import struct
    b = open(path, "rb").read()
    ver, count, flags = struct.unpack_from("<III", b, 4)
    assert b[:4] == b"PWG1" and ver >= 2 and flags & 1, "a remodel with a far level (version 2) is wanted"
    off, parts = 64, []
    for _ in range(count):
        rec = b[off:off + 64]
        vc, tc = struct.unpack_from("<II", rec, 12)
        psc = struct.unpack_from("<I", rec, 36)[0]
        ntc, dist = struct.unpack_from("<If", rec, 40)
        off += 64
        p = dict(rec=rec, psc=psc, ntc=ntc, dist=dist)
        p["sft"] = np.frombuffer(b, "<u4", psc + 1, off).astype(np.int64); off += pad4((psc + 1) * 4)
        p["verts"] = np.frombuffer(b, "<f4", vc * 14, off).reshape(vc, 14).copy(); off += pad4(vc * 56)
        p["lm"] = np.frombuffer(b, "<f4", vc * 5, off).reshape(vc, 5).copy(); off += pad4(vc * 20)
        p["tri"] = np.frombuffer(b, "<u2", tc * 3, off).reshape(tc, 3).astype(np.int64); off += pad4(tc * 6)
        p["pairs"] = np.frombuffer(b, "<u4", psc * 2, off).reshape(psc, 2).astype(np.int64); off += psc * 8
        p["bounds"] = np.frombuffer(b, "<f4", psc * 4, off).reshape(psc, 4).copy(); off += psc * 16
        parts.append(p)
    return b[:64], parts


def write(path, header, parts):
    import struct
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
        arr(p["lm"].astype("<f4"))
        arr(p["tri"].astype("<u2"))
        out += p["pairs"].astype("<u4").tobytes()
        out += p["bounds"].astype("<f4").tobytes()
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)


def boundary_loop(T):
    """the near triangles' boundary as one loop of rows in their winding, or None"""
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    key = np.sort(e, 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    b = e[cnt[inv.ravel()] == 1]
    if len(b) < 3:
        return None
    nxt = {}
    for u, v in b:
        if u in nxt:
            return None
        nxt[int(u)] = int(v)
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
    return np.array(loop) if len(loop) == len(b) else None


def surface_far(P, Nrm, T, spacing):
    """a surface's new far level: (triangles as rows, -1 - k for the k-th skirt vertex), the skirt vertices as
    (corner row, position), or None"""
    # (the near level's rows are welded by position: a seam of its coordinates or a bevel's split normals gives a
    # corner several rows, which must not cut its boundary)
    rows_used = np.unique(T)
    _, weld = np.unique(np.round(P[rows_used] * 1e4).astype(np.int64), axis=0, return_inverse=True)
    weld = weld.ravel()
    rep = np.zeros(weld.max() + 1, np.int64)
    rep[weld[::-1]] = rows_used[::-1]
    to_weld = np.full(len(P), -1, np.int64)
    to_weld[rows_used] = weld
    T = rep[to_weld[T]]
    T = T[(T[:, 0] != T[:, 1]) & (T[:, 1] != T[:, 2]) & (T[:, 2] != T[:, 0])]
    if len(T) < 2:
        return "welded away"
    loop = boundary_loop(T)
    if loop is None or len(loop) < 3:
        return "loop is None or len(loop) < 3"
    q = P[loop]
    n = len(loop)
    din = q - q[np.arange(n) - 1]
    dout = q[(np.arange(n) + 1) % n] - q
    ln_in = np.linalg.norm(din, axis=1)
    ln_out = np.linalg.norm(dout, axis=1)
    if (ln_in < 1e-9).any():
        return "(ln_in < 1e-9).any()"
    # the corners: the three loop vertices spanning the most (the farthest from the middle, the farthest from it,
    # the farthest from their line): a bevelled corner's several small turns never split one corner in two
    k0 = int(np.argmax(np.linalg.norm(q - q.mean(0), axis=1)))
    k1 = int(np.argmax(np.linalg.norm(q - q[k0], axis=1)))
    line = q[k1] - q[k0]
    k2 = int(np.argmax(np.linalg.norm(np.cross(q - q[k0], line), axis=1)))
    ci = np.sort(np.array([k0, k1, k2]))
    if len(set(ci.tolist())) < 3:
        return "no three corners"
    corners = loop[ci]
    c = P[corners]
    face = np.cross(c[1] - c[0], c[2] - c[0])
    area = 0.5 * np.linalg.norm(face)
    if area < 1e-6:
        return "area < 1e-6"
    unit = face / (2 * area)
    tn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    if (tn @ unit).sum() < 0:  # (the corners in the order the near triangles wind about their front)
        ci = ci[[0, 2, 1]]
        corners = loop[ci]
        c = P[corners]
        unit = -unit
        ci_reversed = True
    else:
        ci_reversed = False
    proj = 0.5 * (tn @ unit).sum()
    if EDGES == "chain":
        return surface_far_chain(P, T, loop, c, unit, proj, spacing)
    if abs(proj - area) > 0.08 * area:
        return "abs(proj - area) > 0.03 * area"
    outward = Nrm[np.unique(T)].mean(0)
    if outward @ unit < 0:
        outward = -unit
    else:
        outward = unit
    # each edge's chain and how far it strays from its chord
    chains, dev = [], []
    for k in range(3):
        a, b = ci[k], ci[(k + 1) % 3]
        if ci_reversed:
            a, b = b, a
        idx = np.arange(a, b + 1) if b > a else np.concatenate([np.arange(a, n), np.arange(0, b + 1)])
        pts = P[loop[idx]]
        ab = pts[-1] - pts[0]
        t = np.clip(((pts - pts[0]) @ ab) / max(ab @ ab, 1e-12), 0, 1)
        d = np.linalg.norm(pts - (pts[0] + t[:, None] * ab), axis=1)
        chains.append(idx)
        dev.append(float(d.max()))
    # inner vertices, away from the edges, one per cell
    e1 = (c[1] - c[0]) / np.linalg.norm(c[1] - c[0])
    e2 = np.cross(unit, e1)
    on_loop = np.zeros(len(P), bool)
    on_loop[loop] = True
    inner = np.setdiff1d(np.unique(T), loop)
    uv = np.stack([(P[inner] - c[0]) @ e1, (P[inner] - c[0]) @ e2], 1)
    cu = np.stack([(c - c[0]) @ e1, (c - c[0]) @ e2], 1)
    if len(inner):
        dists = []
        for k in range(3):
            a, b = cu[k], cu[(k + 1) % 3]
            ab = b - a
            dists.append(np.abs(ab[0] * (uv[:, 1] - a[1]) - ab[1] * (uv[:, 0] - a[0])) / np.linalg.norm(ab))
        keep = np.min(dists, 0) > 0.5 * spacing
        inner, uv = inner[keep], uv[keep]
    if len(inner):
        cell = np.floor(uv / spacing).astype(np.int64)
        centre = (cell + 0.5) * spacing
        order = np.lexsort((np.linalg.norm(uv - centre, axis=1), cell[:, 1], cell[:, 0]))
        cs = cell[order]
        first = np.ones(len(order), bool)
        first[1:] = (cs[1:] != cs[:-1]).any(1)
        pick = order[first]
        inner, uv = inner[pick], uv[pick]
    rows = np.concatenate([corners, inner])
    pts2 = [Vector((float(x), float(y))) for x, y in np.concatenate([cu, uv])]
    scale = max(np.abs(cu).max(), 1e-6)
    try:
        out = geometry.delaunay_2d_cdt(pts2, [], [[0, 1, 2]], 1, 1e-9 * scale, True)
    except Exception:
        return "except Exception"
    ov, faces, orig = out[0], out[2], out[3]
    if len(ov) != len(pts2):
        return "len(ov) != len(pts2)"
    back = np.full(len(ov), -1, np.int64)
    for i, o in enumerate(orig):
        if len(o) != 1:
            return "len(o) != 1"
        back[i] = o[0]
    F = np.array([[back[f[0]], back[f[1]], back[f[2]]] for f in faces if len(f) == 3], np.int64)
    if not len(F) or (F < 0).any():
        return "not len(F) or (F < 0).any()"
    allp = np.concatenate([cu, uv])
    Fq = allp[F]
    s = np.sign((Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) -
                (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0]))
    F[s < 0] = F[s < 0][:, [0, 2, 1]]
    F = F[s != 0]
    Fq = allp[F]
    got = 0.5 * np.abs((Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) -
                       (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0])).sum()
    full = 0.5 * abs((cu[1, 0] - cu[0, 0]) * (cu[2, 1] - cu[0, 1]) - (cu[1, 1] - cu[0, 1]) * (cu[2, 0] - cu[0, 0]))
    if abs(got - full) > 1e-4 * full:
        return "abs(got - full) > 1e-4 * full"
    tris = rows[F]
    # the 2D triangulation's winding (counter-clockwise in e1, e2 about `unit`) is the near triangles'
    skirt_verts, skirt_tris = [], []
    centroid = c.mean(0)
    for k in range(3):
        # (every edge whose near level is more than its chord gets skirts: a neighbour drawn near meets this edge
        # with all of those vertices, and even where they lie on the chord, a T-junction leaves pixels uncovered)
        if dev[k] < SKIRT_MIN and len(chains[k]) <= 2:
            continue
        a, b = corners[k], corners[(k + 1) % 3]
        pa, pb = P[a], P[b]
        mid = 0.5 * (pa + pb)
        edge = pb - pa
        out_dir = np.cross(edge, unit)
        out_dir /= max(np.linalg.norm(out_dir), 1e-12)
        if out_dir @ (mid - centroid) < 0:
            out_dir = -out_dir
        depth = dev[k] * 1.25 + SKIRT_DEPTH
        # two skirts from the chord straight back into the solid behind the surface: one leaning under the
        # neighbour, its front toward the neighbour's side, and one leaning back under this surface, its front
        # toward this side, so that the crack is closed seen from either (each a back face from the other side,
        # culled). Both lean LEAN back from upright, so their facings are within the surface's (cosine
        # LEAN / sqrt(1 + LEAN^2) at least): the far level can still be skipped whole facing away
        for side in (1.0, -1.0):
            shift = (LEAN * side * out_dir - outward) * depth
            ia = len(skirt_verts)
            skirt_verts.append((a, pa + shift))
            skirt_verts.append((b, pb + shift))
            if side > 0:
                # (a, b) runs as the surface winds; the strip beyond it winds the same way about its own facing
                skirt_tris.append([a, -1 - (ia + 1), b])
                skirt_tris.append([a, -1 - ia, -1 - (ia + 1)])
            else:
                skirt_tris.append([a, b, -1 - (ia + 1)])
                skirt_tris.append([a, -1 - (ia + 1), -1 - ia])
    if skirt_tris:
        tris = np.concatenate([tris, np.array(skirt_tris, np.int64)])
    return tris, skirt_verts


def surface_far_chain(P, T, loop, c, unit, proj, spacing):
    """WG_FAR_EDGES "chain": the far level keeps its near level's whole boundary loop (every vertex its neighbours
    have on its edges, near or far: after wg2_seal.py the same rows' positions on both sides) and only some of its
    inner vertices (one per cell of spacing, away from the loop by half a cell), triangulated again inside the loop
    (Blender's constrained Delaunay) in the corners' plane. Near and far surfaces meet edge to edge, whichever each is
    drawn at: no skirts. Refused where the loop folds over itself seen in that plane (displaced rock that overhangs
    it), which keeps the old far level."""
    e1 = (c[1] - c[0]) / np.linalg.norm(c[1] - c[0])
    e2 = np.cross(unit, e1)
    lu = np.stack([(P[loop] - c[0]) @ e1, (P[loop] - c[0]) @ e2], 1)
    poly = 0.5 * abs(float((lu[:, 0] * np.roll(lu[:, 1], -1) - np.roll(lu[:, 0], -1) * lu[:, 1]).sum()))
    if poly < 1e-6 or abs(proj - poly) > 1e-3 * poly:
        return "the loop folds seen in its plane"
    inner = np.setdiff1d(np.unique(T), loop)
    uv = np.stack([(P[inner] - c[0]) @ e1, (P[inner] - c[0]) @ e2], 1) if len(inner) else np.zeros((0, 2))
    if len(inner):
        a, b = lu, np.roll(lu, -1, 0)
        ab = b - a
        ll = np.maximum((ab * ab).sum(1), 1e-18)
        d = np.full(len(uv), np.inf)
        for s0 in range(0, len(a), 256):
            A_, AB, L_ = a[s0:s0 + 256], ab[s0:s0 + 256], ll[s0:s0 + 256]
            t = np.clip(((uv[:, None, :] - A_[None]) * AB[None]).sum(2) / L_[None], 0, 1)
            q = A_[None] + t[..., None] * AB[None]
            d = np.minimum(d, np.linalg.norm(uv[:, None, :] - q, axis=2).min(1))
        keep = d > 0.5 * spacing
        inner, uv = inner[keep], uv[keep]
    if len(inner):
        cell = np.floor(uv / spacing).astype(np.int64)
        centre = (cell + 0.5) * spacing
        order = np.lexsort((np.linalg.norm(uv - centre, axis=1), cell[:, 1], cell[:, 0]))
        cs = cell[order]
        first = np.ones(len(order), bool)
        first[1:] = (cs[1:] != cs[:-1]).any(1)
        pick = order[first]
        inner, uv = inner[pick], uv[pick]
    rows = np.concatenate([loop, inner])
    allp = np.concatenate([lu, uv])
    pts2 = [Vector((float(x), float(y))) for x, y in allp]
    scale = max(np.abs(lu).max(), 1e-6)
    try:
        out = geometry.delaunay_2d_cdt(pts2, [], [list(range(len(loop)))], 1, 1e-9 * scale, True)
    except Exception:
        return "chain: the triangulation failed"
    ov, faces, orig = out[0], out[2], out[3]
    if len(ov) != len(pts2):
        return "chain: the triangulation added points"
    back = np.full(len(ov), -1, np.int64)
    for i, o in enumerate(orig):
        if len(o) != 1:
            return "chain: the triangulation merged points"
        back[i] = o[0]
    F = np.array([[back[f[0]], back[f[1]], back[f[2]]] for f in faces if len(f) == 3], np.int64)
    if not len(F) or (F < 0).any():
        return "chain: no triangles"
    Fq = allp[F]
    sg = np.sign((Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) -
                 (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0]))
    F[sg < 0] = F[sg < 0][:, [0, 2, 1]]
    # (a triangle of three of the loop's points on one line in this plane is no sliver in space: turned by its
    # facing there)
    flat = np.nonzero(sg == 0)[0]
    if len(flat):
        R = rows[F[flat]]
        fn = np.cross(P[R[:, 1]] - P[R[:, 0]], P[R[:, 2]] - P[R[:, 0]])
        back = (fn @ unit) < 0
        F[flat[back]] = F[flat[back]][:, [0, 2, 1]]
    # the triangles' open edges must be the loop's own edges, each once, and every other edge shared by two: along a
    # run of the loop's points on one line the triangulation's "inside" can add slivers outside the loop (taken
    # away, while they leave an open edge that is not the loop's) or leave one out (refused)
    nl = len(loop)
    loop_edges = {(i, (i + 1) % nl) for i in range(nl)}
    for _ in range(nl + 1):
        E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
        key = np.sort(E, 1)
        _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
        inv = inv.ravel()
        once = cnt[inv] == 1
        bad = np.zeros(len(F), bool)
        for e, (a_, b_) in enumerate(E):
            if once[e] and (int(a_), int(b_)) not in loop_edges:
                bad[e % len(F)] = True
        if not bad.any():
            break
        F = F[~bad]
        if not len(F):
            return "chain: no triangles"
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    key = np.sort(E, 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    opened = {(int(a_), int(b_)) for (a_, b_), c_ in zip(E, cnt[inv]) if c_ == 1}
    if opened != loop_edges or (cnt > 2).any():
        return "chain: the triangles' edges are not the loop's"
    # (counter-clockwise in e1, e2 about `unit`: the near triangles' winding)
    return rows[F], []


def main():
    a = sys.argv[sys.argv.index("--") + 1:]
    src, dst, report_path = a[0], a[1], a[2]
    header, parts = read(src)
    log("read", src, len(parts), "parts")
    report = {"spacing": SPACING, "metal_distance": METAL_DISTANCE, "skirt_min": SKIRT_MIN, "old_far": 0, "new_far": 0,
              "surfaces": 0, "replaced": 0, "kept": 0, "skirts": 0, "split_parts": 0, "refused": {}}
    out_parts = []
    import struct
    for pi, p in enumerate(parts):
        P, Nrm = p["verts"][:, 0:3].astype(np.float64), p["verts"][:, 3:6].astype(np.float64)
        sft, tri, pairs = p["sft"], p["tri"], p["pairs"]
        had_own = any(not (pairs[s, 0] == sft[s] and pairs[s, 1] == sft[s + 1] - sft[s]) for s in range(p["psc"]))
        dist = p["dist"] if had_own else METAL_DISTANCE
        far_list, extra_rows, extra_pos = [], [], []
        vc = len(P)
        for s in range(p["psc"]):
            report["surfaces"] += 1
            nb, ne = sft[s], sft[s + 1]
            old = tri[pairs[s, 0]:pairs[s, 0] + pairs[s, 1]]
            report["old_far"] += len(old)
            res = surface_far(P, Nrm, tri[nb:ne], SPACING) if ne - nb >= 4 else "few triangles"
            if isinstance(res, str):
                report["refused"][res] = report["refused"].get(res, 0) + 1
                res = None
            near = tri[nb:ne]
            if EDGES == "chain":
                # (the old far level is not kept: wg2_lod.py's dropped edge vertices that its undisplaced positions
                # put a hair off the stock edge, leaving chords against a neighbour's edge; the new one is used if
                # it is fewer triangles than the near level, else the near level is drawn far too)
                if res is not None and len(res[0]) >= len(near):
                    res = None
                    report["refused"]["no fewer than near"] = report["refused"].get("no fewer than near", 0) + 1
                if res is None:
                    far_list.append(near)
                    report["kept"] += 1
                    report["new_far"] += len(near)
                    continue
            if EDGES != "chain" and res is not None and len(res[0]) * 3 > len(old) * 2:
                res = None
                report["refused"]["saves too little"] = report["refused"].get("saves too little", 0) + 1
            if res is None:
                # (its old far level)
                far_list.append(old)
                report["kept"] += 1
                report["new_far"] += len(old)
                continue
            ftris, sv = res
            base = vc + len(extra_rows)
            ftris = np.where(ftris < 0, base + (-1 - ftris), ftris)
            for row, pos in sv:
                extra_rows.append(row)
                extra_pos.append(pos)
            far_list.append(ftris)
            report["replaced"] += 1
            report["skirts"] += len(sv) // 2
            report["new_far"] += len(ftris)
        verts = np.concatenate([p["verts"], p["verts"][extra_rows]]) if extra_rows else p["verts"]
        lm = np.concatenate([p["lm"], p["lm"][extra_rows]]) if extra_rows else p["lm"]
        if extra_rows:
            verts[vc:, 0:3] = np.array(extra_pos, np.float32)
        pfs = struct.unpack_from("<I", p["rec"], 32)[0]
        out_parts.extend(assemble(p, verts, lm, far_list, pfs, dist, report))
        if pi % 10 == 0:
            log("part", pi, "of", len(parts), "far triangles", report["old_far"], "->", report["new_far"])
    write(dst, header, out_parts)
    report["parts"] = len(out_parts)
    json.dump(report, open(report_path, "w"), indent=1)
    log("wrote", dst, json.dumps(report))


def assemble(p, verts, lm, far_list, pfs, dist, report, first=0, count=None):
    """the part's records for surfaces [first, first + count): one, or split in two while its vertices are too
    many for 16-bit indices"""
    sft, tri = p["sft"], p["tri"]
    count = p["psc"] if count is None else count
    near = [tri[sft[s]:sft[s + 1]] for s in range(first, first + count)]
    far = far_list[first:first + count]
    used = np.unique(np.concatenate([np.concatenate(near).ravel(), np.concatenate(far).ravel()]))
    if len(used) > 0xFFFF:
        if count < 2:
            raise SystemExit("a surface needs more than 65535 vertices")
        report["split_parts"] += 1
        h = count // 2
        return (assemble(p, verts, lm, far_list, pfs, dist, report, first, h) +
                assemble(p, verts, lm, far_list, pfs, dist, report, first + h, count - h))
    remap = np.full(len(verts), -1, np.int64)
    remap[used] = np.arange(len(used))
    nt = np.concatenate(near)
    lengths = np.array([len(x) for x in near])
    new_sft = np.concatenate([[0], np.cumsum(lengths)])
    pairs = np.zeros((count, 2), np.int64)
    fars = []
    cursor = len(nt)
    for j, f in enumerate(far):
        if f is near[j] or (len(f) == len(near[j]) and np.array_equal(f, near[j])):
            pairs[j] = [new_sft[j], lengths[j]]
            continue
        pairs[j] = [cursor, len(f)]
        cursor += len(f)
        fars.append(f)
    alltri = np.concatenate([nt] + fars) if fars else nt
    return [dict(rec=p["rec"], pfs=pfs + first, psc=count, ntc=len(nt), dist=dist, sft=new_sft,
                 verts=verts[used], lm=lm[used], tri=remap[alltri], pairs=pairs,
                 bounds=p["bounds"][first:first + count])]


if __name__ == "__main__":
    main()
