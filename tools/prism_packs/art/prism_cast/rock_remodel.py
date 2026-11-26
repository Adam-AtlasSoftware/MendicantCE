#!/usr/bin/env python3
"""PRISM: a scenery rock or boulder remodelled for the mesh store (display.prism_models "max"), numpy only.

    python3 rock_remodel.py <model dump.json> <base map.png> <out.pmesh> [--seed N] [--levels auto|4,3,2]
        [--cutoffs auto|200,100,0] [--target-triangles 12000] [--relief 1.0] [--report out.json]

From the model as the game draws it (debug.prism_mesh_dump), each permutation's pieces:
  1. split into four at the edges' midpoints, the level's number of times, each midpoint set on the PN curve of its
     edge (Phong rounding on CE's own normals, welded at each position, so a UV seam or a hard edge never opens):
     CE's facets become the rounded mass the shading already drew;
  2. smoothed along the normal (Taubin's two steps, no shrinking) over the welded mesh, so no stock facet shows;
  3. relief, as the level remodel's natural surfaces have it (art/world_geo/wg2_natural.py): the base map's painted
     shading at four scales (its dark cracks sunk deeper than its ridges stand), and low swells in the rock's own
     space; each band only where the level's edges can carry it (none finer than about four edges), so every level
     is the same rock at its own detail and a level switch changes only the finest grain;
  4. never more than 0.6 % of its size outside CE's surface, nor 3 % inside it (collision is CE's); normals per
     corner, smooth up to 60 degrees; tangents from the uvs.
The levels (--levels auto): as many subdivisions as keep level 0 within --target-triangles, then one fewer a level,
each a quarter of the one before, down to one subdivision: never the bare cage (CE's own facets, or fewer triangles
than CE's rock), which drawn far off read as the remodel flipping back to CE's shape; at most --most-levels. The
cutoffs (auto): 200 px for level 0, half for each level after, so that a level's triangles keep their size on the
screen. The renderer draws no level of fewer than an eighth of the triangles of the level before it
(port/linux/vulkan/xgpu_geometry.h): the old ladders' 16x steps (1,984 / 124 / 31) were drawn at level 0 alone.
Every displacement is a function of the position and the base map's coordinates (a seam's two sides move together:
welded). The last level is the smooth mass once subdivided (not CE's own geometry, whose flat facets would show as a
different shell when it switches in). Collision, physics and markers are the stock model's (the game's): nothing here
is simulated. Everything is derived from the player's maps: local only.
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
sys.path.insert(0, str(HERE.parent / "world_geo"))
import wg2_natural as nat  # noqa: E402
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


def load_bands(png):
    """the base map's painted shading as wg2_natural's TextureBands (rows from the top: the game's v)"""
    im = np.asarray(Image.open(png).convert("RGB")).astype(np.float64) / 255.0
    return nat.TextureBands(im)


def welded_edges(T, w):
    """the welded mesh's unique edges (by welded vertex id)"""
    e = np.concatenate([w[T[:, [0, 1]]], w[T[:, [1, 2]]], w[T[:, [2, 0]]]])
    e.sort(1)
    e = e[e[:, 0] != e[:, 1]]
    return np.unique(e, axis=0)


def displace(P, N, UV, T, bands, size, opts):
    """the smooth mass (Taubin along the normal over the welded mesh) and the band-limited relief, welded per
    position and capped against CE's surface (P is on CE's rounded surface; the caps are against the unmoved P)"""
    w = weld_keys(P, 1e-6)
    nW = w.max() + 1
    first = np.zeros(nW, np.int64)
    first[w[::-1]] = np.arange(len(P))[::-1]
    PW = P[first]
    NW = norm(welded_mean(N, w)[first])
    E = welded_edges(T, w)
    h = nat.vertex_spacing(PW, E, nW)
    movable = np.ones(nW, bool)
    X = nat.taubin_along(PW, NW, E, movable, opts.taubin)
    # the texel's size on the surface (world units along u and v), per corner from its triangle, welded
    move = np.zeros(nW)
    if bands is not None:
        tx = nat.texel_size(P[T], UV[T], bands.w, bands.h)
        texel = np.zeros((len(P), 2))
        cnt = np.zeros(len(P))
        for k in range(3):
            np.add.at(texel, T[:, k], tx)
            np.add.at(cnt, T[:, k], 1)
        texel = texel / np.maximum(cnt, 1)[:, None]
        tw = np.sqrt(np.maximum(texel[:, 0] * texel[:, 1], 1e-12))
        rel = np.zeros(len(P))
        hp = h[w]
        for k, amp in enumerate(opts.tex_amps):
            b = bands.sample(k, UV)
            b = np.where(b < 0, b * opts.crack_gain, b)
            rel += amp * size * nat.band_weight(bands.lambda_px[k] * tw, hp) * b
        move += welded_mean(rel, w)[first]
    sw = opts.swell_scale * size
    move += opts.swell * size * nat.value_noise3(X, sw, salt=23) * nat.band_weight(sw * 0.5, h, 2.0, 4.0)
    move = nat.smooth_scalar(move, E, movable, 1)
    X = X + NW * move[:, None]
    off = ((X - PW) * NW).sum(1)
    off_c = nat.soft_clamp(off, -opts.cap_in * size, opts.cap_out * size)
    X = X + NW * (off_c - off)[:, None]
    return X[w]


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
            P = displace(P, N, UV, T, relief, size, opts)
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


def auto_levels(cage, target, most):
    """subdivisions per level: level 0 the most within target (at least one), one fewer each level after (each a
    quarter of the one before), never 0 (the cage); at most `most` levels"""
    top = 1
    while cage * 4 ** (top + 1) <= target:
        top += 1
    return list(range(top, 0, -1))[:max(1, most)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("base_map")
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--levels", default="auto",
                    help="subdivisions per level (4,3,2), or auto: within --target-triangles, a quarter a step")
    ap.add_argument("--cutoffs", default="auto", help="pixels from which each level is drawn, or auto")
    ap.add_argument("--target-triangles", type=int, default=12000)
    ap.add_argument("--most-levels", type=int, default=4)
    ap.add_argument("--phong", type=float, default=0.75)
    ap.add_argument("--taubin", type=int, default=6)
    ap.add_argument("--relief", type=float, default=1.0, help="the relief's scale (1: the amplitudes below)")
    ap.add_argument("--tex-amps", default="0.0025,0.005,0.009,0.013",
                    help="the base map's four bands, each a share of the rock's size")
    ap.add_argument("--crack-gain", type=float, default=1.6)
    ap.add_argument("--swell", type=float, default=0.012)
    ap.add_argument("--swell-scale", type=float, default=0.35)
    ap.add_argument("--cap-in", type=float, default=0.03)
    ap.add_argument("--cap-out", type=float, default=0.006)
    ap.add_argument("--split", type=float, default=60.0)
    ap.add_argument("--report", default="")
    o = ap.parse_args()
    o.tex_amps = [float(x) * o.relief for x in o.tex_amps.split(",")]
    o.swell *= o.relief
    dump = json.loads(Path(o.dump).read_text())
    blob = Path(o.dump).with_suffix(".bin").read_bytes()
    relief = load_bands(o.base_map) if o.base_map != "-" else None
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
    cage = sum(len(pfd.part_arrays(dump, blob, p)[1]) for ps in groups.values() for p in ps)
    subs = auto_levels(cage, o.target_triangles, o.most_levels) if o.levels == "auto" else \
        [int(x) for x in o.levels.split(",")]
    cut = [200.0 / (2 ** i) for i in range(len(subs) - 1)] + [0.0] if o.cutoffs == "auto" else \
        [float(x) for x in o.cutoffs.split(",")]
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
