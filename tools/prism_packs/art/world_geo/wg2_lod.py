"""PRISM world geometry v2: the far level of detail of a remodel, surface by surface.

Each stock surface's remodelled triangles keep every vertex on the stock triangle's edges (so a surface drawn far
meets a neighbour drawn near with the very same edge vertices: no crack, whichever level each is drawn at) and only
some of its inner vertices (one per cell of a grid `spacing` wu across), triangulated again by Blender's constrained
Delaunay triangulation in the stock triangle's plane. Vertices are the remodel's own (the same rows), so the far
level costs indices only.
"""
import numpy as np
from mathutils import Vector, geometry


def _normalize(v):
    return v / max(np.linalg.norm(v), 1e-12)


def surface_lod1(tri_rows, topo_corners, topo_pos, a, b, c, spacing, min_saved=12):
    """tri_rows (n, 3) the surface's triangles as row ids, topo_corners (n, 3) the same corners' topology vertex ids,
    topo_pos the topology's undisplaced positions (on the stock plane), a b c the stock triangle. Returns the far
    triangles as row ids (m, 3), or None when it would not save at least min_saved triangles."""
    verts, first = np.unique(topo_corners.ravel(), return_index=True)
    rows_of = tri_rows.ravel()[first]
    if len(verts) < 16:
        return None
    p = topo_pos[verts]
    e1 = _normalize(b - a)
    nrm = _normalize(np.cross(b - a, c - a))
    e2 = np.cross(nrm, e1)
    q = np.stack([(p - a) @ e1, (p - a) @ e2], 1)
    qa = np.zeros(2)
    qb = np.array([(b - a) @ e1, (b - a) @ e2])
    qc = np.array([(c - a) @ e1, (c - a) @ e2])

    def side(u, v, x):
        return (v[0] - u[0]) * (x[:, 1] - u[1]) - (v[1] - u[1]) * (x[:, 0] - u[0])

    scale = max(np.linalg.norm(qb), np.linalg.norm(qc), 1e-9)
    tol = 1e-5 * scale
    lens = [np.linalg.norm(qb - qa), np.linalg.norm(qc - qb), np.linalg.norm(qa - qc)]
    d = np.stack([np.abs(side(qa, qb, q)) / max(lens[0], 1e-12), np.abs(side(qb, qc, q)) / max(lens[1], 1e-12),
                  np.abs(side(qc, qa, q)) / max(lens[2], 1e-12)], 1)
    edge = (d < tol).any(1)
    inner = np.nonzero(~edge)[0]
    if len(inner) < 4:
        return None
    # one inner vertex per grid cell, away from the edges by half a cell
    far_enough = d[inner].min(1) > 0.5 * spacing
    inner = inner[far_enough]
    if len(inner):
        cell = np.floor(q[inner] / spacing).astype(np.int64)
        _, pick = np.unique(cell, axis=0, return_index=True)
        inner = inner[np.sort(pick)]
    bnd = np.nonzero(edge)[0]
    cen = (qa + qb + qc) / 3.0
    ang = np.arctan2(q[bnd, 1] - cen[1], q[bnd, 0] - cen[0])
    bnd = bnd[np.argsort(ang)]
    order = np.concatenate([bnd, inner])
    pts = [Vector((float(x), float(y))) for x, y in q[order]]
    try:
        out = geometry.delaunay_2d_cdt(pts, [], [list(range(len(bnd)))], 1, 1e-9 * scale, True)
    except Exception:
        return None
    out_verts, _, faces, orig_verts = out[0], out[1], out[2], out[3]
    if len(out_verts) != len(pts):
        return None
    back = np.full(len(out_verts), -1, np.int64)
    for i, ov in enumerate(orig_verts):
        if len(ov) != 1:
            return None
        back[i] = ov[0]
    F = np.array([[back[f[0]], back[f[1]], back[f[2]]] for f in faces if len(f) == 3], np.int64)
    if len(F) == 0 or (F < 0).any() or len(F) + min_saved > len(tri_rows):
        return None
    # the stock surface's winding (the remodel's triangles all share it)
    t0 = topo_corners[0]
    loc = {v: i for i, v in enumerate(verts)}
    w0 = np.array([loc[v] for v in t0])
    p0 = q[w0]
    s0 = np.sign((p0[1, 0] - p0[0, 0]) * (p0[2, 1] - p0[0, 1]) - (p0[1, 1] - p0[0, 1]) * (p0[2, 0] - p0[0, 0]))
    Fq = q[order][F]
    s = np.sign((Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) -
                (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0]))
    flip = s != s0
    F[flip] = F[flip][:, [0, 2, 1]]
    keep = s != 0
    F = F[keep]
    # it must cover the stock triangle exactly once (a misordered edge would leave a hole)
    Fq = q[order][F]
    area = 0.5 * np.abs((Fq[:, 1, 0] - Fq[:, 0, 0]) * (Fq[:, 2, 1] - Fq[:, 0, 1]) -
                        (Fq[:, 1, 1] - Fq[:, 0, 1]) * (Fq[:, 2, 0] - Fq[:, 0, 0])).sum()
    full = 0.5 * abs(qb[0] * qc[1] - qb[1] * qc[0])
    if full <= 0 or abs(area - full) > 1e-4 * full:
        return None
    return rows_of[order][F]
