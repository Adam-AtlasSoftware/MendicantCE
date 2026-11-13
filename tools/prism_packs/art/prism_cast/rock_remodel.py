#!/usr/bin/env python3
"""PRISM v3: a scenery rock or boulder remodelled for the mesh store (display.prism_models "max"), numpy only.

    python3 rock_remodel.py <model dump.json> <base map.png> <out.pmesh> [--seed N] [--levels 3,1]
        [--chisel 1.0] [--relief 1.0] [--report out.json]

From the model as the game draws it (debug.prism_mesh_dump), each permutation's pieces:
  1. split into four at the edges' midpoints, three times (level 0; once for level 1), each midpoint set on the
     PN curve of its edge (Phong rounding on CE's own normals, welded at each position, so a UV seam or a hard edge
     never opens): CE's facets become the rounded mass the shading already drew;
  2. chiselled: cuts by planes (a few per square world unit of surface, seeded by the model's name), each a plane
     tilted up to 28 degrees from the surface where it lands and set 0.6 to 2.5 % of the rock's size into it; a point
     under several takes the deepest: flat facets with crisp ridges between them, as granite breaks;
  3. the base map's own painted relief (its luminance less its blur, two scales) set in along the normal (a few
     millimetres): CE's cracks and grain become shape;
  4. never more than 0.6 % of its size outside CE's surface, nor 4 % inside it; normals per corner from the facets
     (split above 34 degrees: the chisel's ridges stay crisp); tangents from the uvs.
Every displacement is a function of the position alone (a seam's two sides move together). Level 2 is CE's own
geometry (from 0 px). Collision, physics and markers are the stock model's (the game's): nothing here is simulated.
Everything is derived from the player's maps: local only.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import packs_env as E  # noqa: E402

TOOLS = E.TOOLS
sys.path.insert(0, TOOLS)
sys.path.insert(0, str(HERE))
import pmesh  # noqa: E402
import pmesh_from_dump as pfd  # noqa: E402
from jms_to_pmesh import tangents  # noqa: E402

DETAILS = ("superlow", "low", "medium", "high", "superhigh")


def norm(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)


def weld_keys(P, q=1e-5):
    _, inv = np.unique(np.round(P / q).astype(np.int64), axis=0, return_inverse=True)
    return inv.reshape(-1)


def welded_mean(values, inv):
    n = inv.max() + 1
    s = np.zeros((n,) + values.shape[1:])
    np.add.at(s, inv, values)
    c = np.bincount(inv, minlength=n).reshape((n,) + (1,) * (values.ndim - 1))
    return (s / c)[inv]


def subdivide(P, N, UV, T, alpha):
    """one 1-to-4 split; midpoints on the PN edge curve (N: welded normals); returns P, N, UV, T"""
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    # (an edge is keyed by its ends' welded positions, so the two sides of a seam share its midpoint's position;
    # each side keeps its own vertex for its own uvs)
    key = np.sort(e, axis=1)
    uk, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    a, b = uk[:, 0], uk[:, 1]
    pa, pb, na, nb = P[a], P[b], N[a], N[b]
    d = pb - pa
    m = (pa + pb) * 0.5 - alpha * (((d * na).sum(1))[:, None] * na - ((d * nb).sum(1))[:, None] * nb) / 8.0
    base = len(P)
    P2 = np.concatenate([P, m])
    N2 = np.concatenate([N, norm(na + nb)])
    UV2 = np.concatenate([UV, (UV[a] + UV[b]) * 0.5])
    t = len(T)
    m01, m12, m20 = base + inv[:t], base + inv[t:2 * t], base + inv[2 * t:]
    A, B, C = T[:, 0], T[:, 1], T[:, 2]
    T2 = np.concatenate([np.stack([A, m01, m20], 1), np.stack([m01, B, m12], 1), np.stack([m20, m12, C], 1),
                         np.stack([m01, m12, m20], 1)])
    # seam midpoints: the same position from either side (welded mean)
    w = weld_keys(P2, 1e-6)
    P2 = welded_mean(P2, w)
    N2 = norm(welded_mean(N2, w))
    return P2, N2, UV2, T2


class Relief:
    def __init__(self, png, fine_px=2, coarse_px=10):
        im = np.asarray(Image.open(png).convert("RGB")).astype(np.float64) / 255.0
        lum = im @ np.array([0.2126, 0.7152, 0.0722])
        self.h, self.w = lum.shape

        def blur(x, r):
            k = 2 * r + 1
            pad = np.pad(x, r, mode="wrap")
            c = np.pad(pad.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
            return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
        b1 = blur(lum, fine_px)
        b2 = blur(b1, coarse_px)
        f = lum - b1
        c = b1 - b2
        self.fine = np.clip(f / max(f.std() * 2.5, 1e-6), -1, 1)
        self.coarse = np.clip(c / max(c.std() * 2.0, 1e-6), -1, 1)

    def sample(self, field, uv):
        x = np.mod(uv[:, 0], 1.0) * self.w - 0.5
        y = np.mod(uv[:, 1], 1.0) * self.h - 0.5
        x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - x0, y - y0
        x0 %= self.w
        y0 %= self.h
        x1, y1 = (x0 + 1) % self.w, (y0 + 1) % self.h
        return ((field[y0, x0] * (1 - fx) + field[y0, x1] * fx) * (1 - fy) +
                (field[y1, x0] * (1 - fx) + field[y1, x1] * fx) * fy)


def chisel_planes(P, N, T, size, rng, density, depth_lo, depth_hi, tilt_deg):
    A = 0.5 * np.linalg.norm(np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]]), axis=1)
    count = int(np.clip(A.sum() * density, 6, 400))
    pick = rng.choice(len(T), count, p=A / A.sum())
    r1, r2 = rng.random(count), rng.random(count)
    s = np.sqrt(r1)
    bc = np.stack([1 - s, s * (1 - r2), s * r2], 1)
    c = (P[T[pick]] * bc[:, :, None]).sum(1)
    n = norm((N[T[pick]] * bc[:, :, None]).sum(1))
    # tilt each plane's normal by up to tilt_deg about a random axis in the surface
    rnd = norm(rng.normal(size=(count, 3)))
    axis = norm(np.cross(n, rnd))
    ang = np.radians(tilt_deg) * rng.random(count)
    n = norm(n * np.cos(ang)[:, None] + np.cross(axis, n) * np.sin(ang)[:, None])
    depth = size * (depth_lo + (depth_hi - depth_lo) * rng.random(count))
    return c - n * depth[:, None], n


def displace(P, N, UV, T, relief, planes, size, opts):
    """the chisel's cut and the texture's relief (welded: per position), capped against CE's surface"""
    w = weld_keys(P, 1e-6)
    cut = np.zeros(len(P))
    if planes is not None:
        c, n = planes
        for i in range(0, len(P), 4096):
            p = P[i:i + 4096]
            d = ((p[:, None, :] - c[None]) * n[None]).sum(2)
            # (a plane cuts only near where it landed: within 3 times its depth plus a fifth of the size)
            r = np.linalg.norm(p[:, None, :] - c[None], axis=2) / size
            fall = np.clip((0.3 - r) / 0.12, 0, 1)
            d = d * fall * fall * (3 - 2 * fall)
            cut[i:i + 4096] = np.clip(d, 0, None).max(1)
    rel = np.zeros(len(P))
    if relief is not None:
        rel = (relief.sample(relief.fine, UV) * opts.relief_fine + relief.sample(relief.coarse, UV) * opts.relief_coarse)
        rel = welded_mean(rel, w)
    cut = welded_mean(cut, w)
    move = -cut * opts.chisel + rel * size * opts.relief
    move = np.clip(move, -opts.cap_in * size, opts.cap_out * size)
    return P + N * move[:, None]


def corner_normals(P, T, split_deg):
    """per-corner normals: the faces at a welded position whose normals are within split_deg of the corner's face,
    area-weighted; returns the unique (vertex, normal) vertices and the remapped triangles"""
    w = weld_keys(P, 1e-6)
    # (the dump's triangles are wound as CE's front faces, clockwise about their outward normals)
    fn = -np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    fu = norm(fn)
    nW = w.max() + 1
    # faces per welded vertex
    corners = np.repeat(np.arange(len(T)), 3)
    cv = w[T.reshape(-1)]
    order = np.argsort(cv, kind="stable")
    cv_s, cf_s = cv[order], corners[order]
    starts = np.searchsorted(cv_s, np.arange(nW + 1))
    cosl = np.cos(np.radians(split_deg))
    out = np.zeros((len(T) * 3, 3))
    for v in range(nW):
        fs = cf_s[starts[v]:starts[v + 1]]
        if not len(fs):
            continue
        u = fu[fs]
        agree = (u @ u.T) >= cosl
        out[order[starts[v]:starts[v + 1]]] = agree.astype(np.float64) @ fn[fs]
    return norm(out)


def build_level(dump, blob, part_groups, subdivisions, size, rng_seed, relief, opts):
    pieces = []
    for key, parts in sorted(part_groups.items()):
        raws, tris, base = [], [], 0
        for part in parts:
            raw, t = pfd.part_arrays(dump, blob, part)
            raws.append(raw)
            tris.append(t + base)
            base += len(raw)
        raw = np.concatenate(raws)
        T = np.concatenate(tris)
        P = raw[:, 0:3].copy()
        UV = raw[:, 12:14].copy()
        w = weld_keys(P)
        N = norm(welded_mean(raw[:, 3:6], w))
        size = float((P.max(0) - P.min(0)).max())  # (this piece's own: a permutation's rock)
        for _ in range(subdivisions):
            P, N, UV, T = subdivide(P, N, UV, T, opts.phong)
        if subdivisions:
            rng = np.random.default_rng(rng_seed + key[1] * 7919)
            planes = chisel_planes(P, N, T, size, rng, opts.density / (size * size), opts.depth_lo, opts.depth_hi,
                                   opts.tilt)
            P = displace(P, N, UV, T, relief, planes, size, opts)
        # corners: own vertices (normal per corner)
        if subdivisions:
            CN = corner_normals(P, T, opts.split)
        else:
            CN = norm(raw[:, 3:6])[T.reshape(-1)]
        flat = T.reshape(-1)
        k = np.round(np.concatenate([flat[:, None].astype(np.float64), CN * 1000.0], 1)).astype(np.int64)
        uk, first, inv = np.unique(k, axis=0, return_index=True, return_inverse=True)
        verts = flat[first]
        a = type("A", (), {"__len__": lambda s: len(s.position)})()
        a.position = P[verts]
        a.normal = CN[first]
        a.uv = UV[verts]
        a.joints = np.zeros((len(verts), 4), np.int64)
        a.weights = np.zeros((len(verts), 4))
        a.weights[:, 0] = 1.0
        tri = inv.reshape(-1, 3)
        a.tangent, a.sign = tangents(a.position, a.normal, a.uv, tri)
        pieces.append((key, a, tri))
    return pieces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("base_map")
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--levels", default="3,1")
    ap.add_argument("--cutoffs", default="140,45,0")
    ap.add_argument("--phong", type=float, default=0.75)
    ap.add_argument("--chisel", type=float, default=1.0)
    ap.add_argument("--density", type=float, default=36.0, help="planes per (size)^2 of surface")
    ap.add_argument("--depth-lo", type=float, default=0.006)
    ap.add_argument("--depth-hi", type=float, default=0.025)
    ap.add_argument("--tilt", type=float, default=28.0)
    ap.add_argument("--relief", type=float, default=0.004)
    ap.add_argument("--relief-fine", type=float, default=0.6)
    ap.add_argument("--relief-coarse", type=float, default=1.0)
    ap.add_argument("--cap-in", type=float, default=0.04)
    ap.add_argument("--cap-out", type=float, default=0.006)
    ap.add_argument("--split", type=float, default=34.0)
    ap.add_argument("--report", default="")
    o = ap.parse_args()
    dump = json.loads(Path(o.dump).read_text())
    blob = Path(o.dump).with_suffix(".bin").read_bytes()
    relief = Relief(o.base_map) if o.base_map != "-" else None
    seed = o.seed or int(hashlib.sha1(dump["tag"].encode()).hexdigest()[:8], 16)
    groups = {}
    for ri, region in enumerate(dump["regions"]):
        for pi, perm in enumerate(region["permutations"]):
            g = perm["geometries"][4]
            if not 0 <= g < len(dump["geometries"]):
                continue
            for part in dump["geometries"][g]["parts"]:
                if part["vertex_count"] <= 0 or part["flags"] & 1:
                    continue
                groups.setdefault((ri, pi, part["shader"]), []).append(part)
    allp = np.concatenate([pfd.part_arrays(dump, blob, p)[0][:, 0:3] for ps in groups.values() for p in ps])
    size = float((allp.max(0) - allp.min(0)).max())
    subs = [int(x) for x in o.levels.split(",")] + [0]
    cut = [float(x) for x in o.cutoffs.split(",")]
    vertices, indices, pieces, levels = [], [], [], []
    vat = iat = 0
    report = {"tag": dump["tag"], "size_wu": size, "levels": []}
    for li, s in enumerate(subs):
        lv = build_level(dump, blob, groups, s, size, seed, relief, o)
        lv0, li0, pf = vat, iat, len(pieces)
        pts = []
        for (ri, pi, sh), a, tri in lv:
            v = pfd.to_vertices(a)
            vertices.append(v)
            indices.append(tri.astype(np.uint32).reshape(-1))
            pieces.append(pmesh.Piece(ri, pi, sh, 0, 0, vat, len(v), iat, tri.size))
            vat += len(v)
            iat += tri.size
            pts.append(a.position)
        P = np.concatenate(pts)
        c = (P.max(0) + P.min(0)) * 0.5
        levels.append(pmesh.Level(cut[li] if li + 1 < len(subs) else 0.0, pf, len(pieces) - pf, lv0, vat - lv0, li0,
                                  iat - li0, tuple(float(x) for x in c), float(np.linalg.norm(P - c, axis=1).max())))
        report["levels"].append({"subdivisions": s, "triangles": int(sum(len(t) for _, _, t in lv)),
                                 "vertices": int(sum(len(a.position) for _, a, _ in lv))})
    allv = np.concatenate(vertices)
    bones = [pmesh.Bone(n["name"], n["parent"], i, b, inv) for i, (n, (b, inv)) in
             enumerate(zip(dump["nodes"], pfd.node_matrices(dump)))]
    radius = float(np.linalg.norm(allv["position"].astype(np.float64), axis=1).max()) * 1.0001
    mesh = pmesh.Mesh(len(dump["nodes"]), dump["node_crc"], bones, levels, pieces, allv, np.concatenate(indices),
                      (0.0, 0.0, 0.0), radius, {"source": "art/prism_cast/rock_remodel.py", "tag": dump["tag"],
                                                "levels": report["levels"]})
    problems = pmesh.check(mesh, dump)
    for p in problems:
        print("problem:", p)
    if problems:
        return 1
    Path(o.out).parent.mkdir(parents=True, exist_ok=True)
    size_b = pmesh.write_pmesh(Path(o.out), mesh)
    report["bytes"] = size_b
    if o.report:
        Path(o.report).write_text(json.dumps(report, indent=1))
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
