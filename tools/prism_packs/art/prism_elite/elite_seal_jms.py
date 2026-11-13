#!/usr/bin/env python3
"""PRISM v2 r2 Elite: the remodel's JMS made to show no see-through lines (run between elite_build_r2.py and
build_elite_r2_mod.sh's invader-model; plain Python, no Blender).

  python3 -I art/prism_elite/elite_seal_jms.py <in.jms> <out.jms> --ce <CE's JMS of the same permutation
      and level> --exclude "<shaders CE's backing leaves out>" [--report <json>] [--rewind] [--rim]

Lines of the level showed through the Elites' bodies, along the plates' edges. elite_build_r2.py builds the
remodel apart, node by node (CE's parts welded within each dominant node: rigid pieces stay apart), and each piece is
subdivided, clamped to CE's surface and recessed (its bodysuit) on its own: where two pieces met in CE, at the plates'
edges and the joints, their edges now stand 3 to 20 mm apart (822 open edges on the base superhigh, half of them with
no other open edge within 5.5 mm), and through each slit the body's inside, all back faces, is culled: the level shows.

The fix here (backing): CE's own mesh of the same permutation and level, set in behind the remodel's surface (each CE
vertex along its normal past the deepest the new surface goes under it, sampled at its own ray and its triangles'
middles, plus 5 mm), with CE's vertices, weights (so it bends as CE's does, its seams closed), UVs and shaders, its
triangles turned to the remodel's winding and written both ways (a slit into the body between two parts' backings
shows the backing's far side). Through a slit one sees CE's body a little deeper, as a seam, not the level behind.
Where the part is too thin (the backing deeper than 0.45 of its thickness there: mandibles, claws) a vertex is left
out with its triangles. In game (b30, 13 views, environment off, crack view): 4965 see-through pixels to 642; with
every triangle drawn both ways instead, 950; superhigh +5.7% triangles. The positions, normals, UVs, weights, regions
and shaders of the remodel are kept.

--rewind (each connected piece wound one way and turned to face as most of its shading normals say, its corners'
normals turned with a rewound triangle) and --rim (the triangles at open, non-manifold or turned edges written both
ways) are what was tried first: neither closed the lines (4612 and 4187 of 4735), since they were slits, not faces
wound the wrong way. Kept for checking.
Everything here is derived from the player's own maps: local only.
"""
import argparse
import json
import sys
from collections import deque
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import halo_formats as hf  # noqa: E402

WELD = 1e-3  # JMS units (x100 of world units): 0.01 mm


def seal(j, rewind=False, rim=True):
    P = np.array([v.pos for v in j.vertices], np.float64)
    N = np.array([v.normal for v in j.vertices], np.float64)
    T = np.array([t.v for t in j.triangles], np.int64)
    n = len(T)
    _, wid = np.unique(np.round(P / WELD).astype(np.int64), axis=0, return_inverse=True)
    wid = wid.ravel()
    F = wid[T]
    degen = (F[:, 0] == F[:, 1]) | (F[:, 1] == F[:, 2]) | (F[:, 0] == F[:, 2])
    A, B, C = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    geo = np.cross(B - A, C - A)
    area = np.linalg.norm(geo, axis=1) * 0.5
    shade = N[T[:, 0]] + N[T[:, 1]] + N[T[:, 2]]
    agree = (geo * shade).sum(1)
    # the convention: which way a triangle winds about its front (the JMS's own majority)
    conv = 1.0 if (np.sign(agree) * area).sum() >= 0 else -1.0
    # edges: directed (a, b) per triangle corner; manifold ones (exactly two triangles) link triangles
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    ef = np.concatenate([np.arange(n)] * 3)
    ok = np.concatenate([~degen] * 3)
    E, ef = E[ok], ef[ok]
    key = np.sort(E, 1)
    _, inv, cnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    man = cnt[inv] == 2
    order = np.argsort(inv[man], kind="stable")
    idx = np.nonzero(man)[0][order]
    f1, f2 = ef[idx[0::2]], ef[idx[1::2]]
    # the two traverse it the same way: they disagree as they stand
    same = E[idx[0::2], 0] == E[idx[1::2], 0]
    adj = [[] for _ in range(n)]
    for a, b, s in zip(f1.tolist(), f2.tolist(), same.tolist()):
        adj[a].append((b, s))
        adj[b].append((a, s))
    flip = np.zeros(n, bool)
    seen = np.zeros(n, bool)
    comp = np.full(n, -1)
    conflict = np.zeros(n, bool)
    pieces = 0
    for seed in np.argsort(-area):
        if seen[seed] or degen[seed]:
            continue
        seen[seed] = True
        comp[seed] = pieces
        q = deque([seed])
        while q:
            f = q.popleft()
            for g, s in adj[f]:
                want = flip[f] ^ s
                if not seen[g]:
                    seen[g] = True
                    flip[g] = want
                    comp[g] = pieces
                    q.append(g)
                elif flip[g] != want:
                    conflict[f] = conflict[g] = True
        pieces += 1
    # each piece faces as most of its area's shading normals say
    signed = np.where(flip, -agree, agree) * conv
    turned_pieces = 0
    for c in range(pieces):
        sel = comp == c
        if (np.sign(signed[sel]) * area[sel]).sum() < 0:
            flip[sel] = ~flip[sel]
            turned_pieces += 1
    signed = np.where(flip, -agree, agree) * conv
    cosv = signed / np.maximum(np.linalg.norm(geo, axis=1) * np.linalg.norm(shade, axis=1), 1e-30)
    if not rewind:
        flip[:] = False
    # a triangle wound again whose shading normals still say the old way (Blender made them from the old winding):
    # its corners' normals turned with it (a corner shared with triangles that keep theirs gets a copy)
    renormal = flip & (cosv < -0.17) & ~degen & ~conflict
    users = np.bincount(T.ravel(), minlength=len(P))
    ren_users = np.bincount(T[renormal].ravel(), minlength=len(P))
    verts = list(j.vertices)
    copies = {}
    T2 = T.copy()
    for i in np.nonzero(renormal)[0]:
        for k in range(3):
            vi = int(T[i, k])
            if ren_users[vi] == users[vi]:
                if vi not in copies:
                    v = verts[vi]
                    verts[vi] = hf.JmsVertex(v.node0, v.pos, tuple(-x for x in v.normal), v.node1, v.weight, v.u, v.v)
                    copies[vi] = vi
            else:
                if vi not in copies:
                    v = verts[vi]
                    copies[vi] = len(verts)
                    verts.append(hf.JmsVertex(v.node0, v.pos, tuple(-x for x in v.normal), v.node1, v.weight, v.u,
                                              v.v))
                T2[i, k] = copies[vi]
    j.vertices = verts
    N2 = np.array([v.normal for v in verts], np.float64)
    shade2 = N2[T2[:, 0]] + N2[T2[:, 1]] + N2[T2[:, 2]]
    signed2 = np.where(flip, -(geo * shade2).sum(1), (geo * shade2).sum(1)) * conv
    cos2 = signed2 / np.maximum(np.linalg.norm(geo, axis=1) * np.linalg.norm(shade2, axis=1), 1e-30)
    both = (conflict | (cos2 < -0.17)) & ~degen if rewind else np.zeros(n, bool)
    # the rims: every triangle with an edge that is open, shared by more than two triangles (a wall's foot, a bevel
    # over a non-manifold rim) or wound against its neighbour's, written both ways
    rim_faces = np.zeros(n, bool)
    if rim:
        bad_edge = (cnt[inv] != 2)
        rim_faces[ef[bad_edge]] = True
        rim_faces[f1[same]] = True
        rim_faces[f2[same]] = True
        rim_faces &= ~degen
        both |= rim_faces
    out = []
    for i, t in enumerate(j.triangles):
        tv = tuple(int(x) for x in T2[i])
        v = (tv[0], tv[2], tv[1]) if flip[i] else tv
        out.append(hf.JmsTriangle(t.region, t.shader, v))
        if both[i]:
            out.append(hf.JmsTriangle(t.region, t.shader, (v[0], v[2], v[1])))
    rep = dict(triangles=n, pieces=pieces, rewound=int((flip & ~degen).sum()), pieces_turned=turned_pieces,
               turned_edges_before=int(same.sum()), conflicts=int(conflict.sum()), two_sided=int(both.sum()),
               normals_turned=int(renormal.sum()), vertices_added=len(verts) - len(P), rim=int(rim_faces.sum()),
               convention=conv)
    j.triangles = out
    return j, rep


MM = 100.0 / 3048.0  # a millimetre in JMS units (x100 of world units)


def ray_hits(orig, dirs, P, T, far):
    """for each ray (orig + t dir, 0 < t < far) the nearest t at which it meets a triangle of (P, T), either face;
    inf where none (Moller-Trumbore, the rays in batches)"""
    A, B, C = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    e1, e2 = B - A, C - A
    lo = np.minimum(np.minimum(A, B), C)
    hi = np.maximum(np.maximum(A, B), C)
    out = np.full(len(orig), np.inf)
    for s in range(0, len(orig), 32):
        o, d = orig[s:s + 32], dirs[s:s + 32]
        # (only the triangles whose boxes the rays' segments reach)
        a_, b_ = o, o + d * far
        slo, shi = np.minimum(a_, b_).min(0), np.maximum(a_, b_).max(0)
        sel = np.nonzero(((hi >= slo) & (lo <= shi)).all(1))[0]
        if not len(sel):
            continue
        E1, E2, AA = e1[sel][None], e2[sel][None], A[sel][None]
        D = d[:, None, :]
        pv = np.cross(D, E2)
        det = (E1 * pv).sum(2)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        tv = o[:, None, :] - AA
        u = (tv * pv).sum(2) * inv
        qv = np.cross(tv, E1)
        v = (D * qv).sum(2) * inv
        t = (E2 * qv).sum(2) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-6) & (t < far)
        out[s:s + 32] = np.where(hit, t, np.inf).min(1)
    return out


def backing(j, ce, exclude, margin_mm=5.0, reach_mm=80.0, thin=0.45):
    """CE's own mesh of this permutation and level, set in behind the remodel's surface (each CE vertex along its
    normal past the deepest the new surface goes under it, by margin): its vertices, weights, UVs and shaders are
    CE's, its triangles turned to the remodel's winding. Wherever the remodel leaves a slit (the pieces it builds
    apart, node by node, each shrunk and recessed on its own: the seams between parts), CE's body shows through it,
    as it would be in CE, instead of the level behind; written both ways, so that a slit into the body's inside
    (between two parts' backings, each set in on its own) shows the backing's far side. A vertex where the part is too thin for it (deeper than
    thin x the part's thickness there) is left out with its triangles: the mandibles, the claws."""
    Pn = np.array([v.pos for v in j.vertices], np.float64)
    Tn = np.array([t.v for t in j.triangles], np.int64)
    Pc = np.array([v.pos for v in ce.vertices], np.float64)
    Nc = np.array([v.normal for v in ce.vertices], np.float64)
    Nc /= np.maximum(np.linalg.norm(Nc, axis=1, keepdims=True), 1e-12)
    shader_name = {i: m[0].lower() for i, m in enumerate(ce.materials)}
    body = [t for t in ce.triangles if shader_name[t.shader] not in exclude]
    if not body:
        return j, {"backing_triangles": 0}
    Tc = np.array([t.v for t in body], np.int64)
    used = np.unique(Tc)
    margin, reach = margin_mm * MM, reach_mm * MM
    # how deep the new surface lies under each CE vertex (the deepest of its own ray and its triangles' middles)
    start = Pc[used] + Nc[used] * reach * 0.25
    h = ray_hits(start, -Nc[used], Pn, Tn, reach * 1.25)
    depth = np.full(len(Pc), 0.0)
    depth[used] = np.where(np.isfinite(h), h - reach * 0.25, 0.0)
    cen = Pc[Tc].mean(1)
    cn = Nc[Tc].sum(1)
    cn /= np.maximum(np.linalg.norm(cn, axis=1, keepdims=True), 1e-12)
    hc = ray_hits(cen + cn * reach * 0.25, -cn, Pn, Tn, reach * 1.25)
    dc = np.where(np.isfinite(hc), hc - reach * 0.25, 0.0)
    for k in range(3):
        np.maximum.at(depth, Tc[:, k], dc)
    depth = np.maximum(depth, 0.0) + margin
    # the part's thickness under each vertex (CE's own other side)
    tc = ray_hits(Pc[used] - Nc[used] * 1e-3, -Nc[used], Pc, Tc, reach * 4)
    thick = np.full(len(Pc), np.inf)
    thick[used] = tc
    ok_v = depth < thin * thick
    keep = ok_v[Tc].all(1)
    base = len(j.vertices)
    remap = {}
    verts = list(j.vertices)
    mat_index = {m[0].lower(): i for i, m in enumerate(j.materials)}
    reg_index = {r: i for i, r in enumerate(j.regions)}
    tris = list(j.triangles)
    added = 0
    for t, k in zip(body, keep):
        if not k:
            continue
        sh = shader_name[t.shader]
        if sh not in mat_index or ce.regions[t.region] not in reg_index:
            continue
        vv = []
        for vi in t.v:
            if vi not in remap:
                v = ce.vertices[vi]
                p = Pc[vi] - Nc[vi] * depth[vi]
                remap[vi] = len(verts)
                verts.append(hf.JmsVertex(v.node0, tuple(float(x) for x in p), v.normal, v.node1, v.weight, v.u, v.v))
            vv.append(remap[vi])
        tris.append(hf.JmsTriangle(reg_index[ce.regions[t.region]], mat_index[sh], (vv[0], vv[2], vv[1])))
        # (and its inside: through a slit into the body, between the backings of two parts set in apart, the
        # backing's far side is seen from within)
        tris.append(hf.JmsTriangle(reg_index[ce.regions[t.region]], mat_index[sh], (vv[0], vv[1], vv[2])))
        added += 2
    j.vertices, j.triangles = verts, tris
    return j, {"backing_triangles": added, "backing_vertices": len(verts) - base,
               "backing_left_out_thin": int((~keep).sum()),
               "backing_depth_mm": [float(np.percentile(depth[used] / MM, q)) for q in (5, 50, 95)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--report")
    ap.add_argument("--rewind", action="store_true", help="wind each piece one way (not the default: see above)")
    ap.add_argument("--rim", action="store_true", help="write the rims' triangles both ways")
    ap.add_argument("--ce", help="CE's JMS of the same permutation and level: set in behind as a backing")
    ap.add_argument("--exclude", default="", help="shaders (comma list) CE's backing leaves out (cards, glows)")
    a = ap.parse_args()
    j = hf.read_jms(a.src)
    j, rep = seal(j, rewind=a.rewind, rim=a.rim)
    if a.ce:
        j, brep = backing(j, hf.read_jms(a.ce), {x.strip().lower() for x in a.exclude.split(",") if x.strip()})
        rep.update(brep)
    hf.write_jms(a.dst, j)
    print(Path(a.src).name, json.dumps(rep))
    if a.report:
        Path(a.report).write_text(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
