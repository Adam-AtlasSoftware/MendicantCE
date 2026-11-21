"""PRISM world geometry v3: the natural surfaces of a remodel (rock, cliffs, hills, ground): tessellation, the smart
triangle budget, the smooth base surface and the band-limited relief (wg2_remodel.py's "rock" and "ground" classes).

What the v2 builder left (its shaping, wg2_shape.py, gone now): edge splits with slivers, CE's facets showing through a
light Phong rounding, stepped strata and flat Worley chips sharper than the mesh could carry (stair steps, jagged
silhouettes and facets under a pixel that sparkle as the camera moves), and rock set up to 0.5 wu behind the
collision surface. Here:

  1. Tessellation (tessellate): each stock edge next to a natural face is cut into equal segments by its length and
     the faces' spacing (the same points on both sides, so neighbours of any class meet without a crack); each natural
     stock triangle takes a regular triangular lattice of inner points and is triangulated in its plane by Blender's
     constrained Delaunay triangulation: near-equilateral triangles, every one inside its stock triangle. A face of
     another class whose edges were cut is triangulated with those points alone.
  2. The budget (Reach, spacing_mult): fine where the player can be (the bake's reach: the 1-wu bricks of air the
     walk, a body or the camera reaches; else the faces one can stand on), coarser with distance, coarsest for faces
     no reachable point is in front of.
  3. The smooth base (taubin_along): Taubin's two-step smoothing (no shrinking) of the shape along each vertex's
     normal on the fine mesh, after the Phong rounding over CE's normals: CE's facets leave the hills and rocks;
     vertices on open edges, other classes and pinned ones hold their place.
  4. The relief (rock_relief, ground_relief): band-limited by each vertex's own edge length (a band whose wavelength is
     under about four edges fades out, so nothing finer than the mesh can carry is asked for: no stair steps, no
     sparkle), and anchored in CE's texture space where the art is: the base map's painted rows as soft strata and
     ledges, its painted shading at three scales (its dark cracks deeper than its ridges are tall), erosion runnels
     along v, low boulder-scale swells in world space.

Plain numpy and mathutils (Blender's); deterministic: the same map gives the same remodel. Everything made from the
player's map stays local.
"""
import math
import struct

import numpy as np

try:
    from mathutils import Vector, geometry
    from mathutils.kdtree import KDTree
except ImportError:  # (the tests' numpy-only parts)
    Vector = geometry = KDTree = None


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


def smoothstep(a, b, x):
    t = np.clip((np.asarray(x, np.float64) - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------- the reach (where the player can be)

def read_reach(path, walk_only=False):
    """The bake's reach file (xgpu_vulkan_bake.c's reach_file_write: "PRCH", version 1, the inputs' hash, the flight
    flag, the summary, the bricks and the tiers). Returns the bricks' centres (n, 3) in world units, or None;
    walk_only: the bricks the walk reached (XGPU_BAKE_BRICK_WALK, bit 0: where the player's feet can be) alone, not
    the camera's and bodies' air above them."""
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    if len(b) < 60 or b[:4] != b"PRCH" or struct.unpack("<I", b[4:8])[0] != 1:
        return None
    count = struct.unpack("<2I", b[52:60])[0]
    if count <= 0 or 60 + count * 8 > len(b):
        return None
    br = np.frombuffer(b, np.int16, count=count * 4, offset=60).reshape(-1, 4)
    if walk_only and ((br[:, 3] & 1) == 1).any():
        br = br[(br[:, 3] & 1) == 1]
    # (each brick a 1-wu cell by its integer corner)
    return br[:, :3].astype(np.float64) + 0.5


class Reach:
    """Distances to where the player can be: the reach's bricks (each a point at a cell's centre) or, without them, the
    walkable faces' samples."""

    def __init__(self, points, cell=1.0):
        self.points = np.asarray(points, np.float64).reshape(-1, 3)
        self.cell = cell
        self.tree = None
        if len(self.points):
            self.tree = KDTree(len(self.points))
            for i, p in enumerate(self.points):
                self.tree.insert(p, i)
            self.tree.balance()

    def distance(self, P):
        """(n,) world units from each point to the nearest reach point less half a cell (0 inside a brick)"""
        P = np.asarray(P, np.float64).reshape(-1, 3)
        if self.tree is None:
            return np.zeros(len(P))
        f = self.tree.find
        d = np.fromiter((f(p)[2] for p in P), np.float64, len(P))
        return np.maximum(d - 0.5 * self.cell, 0.0)

    def seen(self, C, N, radius=40.0, most=48):
        """(n,) whether some reach point within `radius` is in front of the face at C with normal N (either side when
        the normal is unknown): a face no one can be in front of is never seen up close"""
        C = np.asarray(C, np.float64).reshape(-1, 3)
        N = np.asarray(N, np.float64).reshape(-1, 3)
        out = np.ones(len(C), bool)
        if self.tree is None:
            return out
        for i in range(len(C)):
            hits = [h for h in self.tree.find_n(C[i], most) if h[2] <= radius]
            if not hits:
                out[i] = False
                continue
            pts = np.array([h[0] for h in hits])
            out[i] = bool((((pts - C[i]) @ N[i]) > -0.25).any())
        return out


def spacing_mult(dist, seen, near, far, far_mult, unseen_mult):
    """the target edge's factor: 1 within `near` world units of the reach, rising smoothly to far_mult at `far`;
    unseen faces take unseen_mult"""
    t = smoothstep(near, far, dist)
    m = 1.0 + (far_mult - 1.0) * t
    return np.where(seen, m, np.maximum(m, unseen_mult))


# ---------- tessellation

def _edge_table(T):
    """unique undirected edges (m, 2), and for each face its three edges' ids and whether each runs the face's way"""
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    s = np.sort(e, 1)
    uniq, inv = np.unique(s, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    n = len(T)
    fe = np.stack([inv[:n], inv[n:2 * n], inv[2 * n:]], 1)
    fwd = np.stack([e[:n, 0] == uniq[fe[:, 0], 0], e[n:2 * n, 0] == uniq[fe[:, 1], 0],
                    e[2 * n:, 0] == uniq[fe[:, 2], 0]], 1)
    return uniq, fe, fwd


def _lattice(a, b, c, s):
    """points (3D) of a triangular lattice of spacing s inside the triangle (a, b, c), at least 0.55 s from its edges,
    in the triangle's plane"""
    e1 = b - a
    L = np.linalg.norm(e1)
    nrm = np.cross(b - a, c - a)
    if L < 1e-9 or np.linalg.norm(nrm) < 1e-12:
        return np.zeros((0, 3))
    x = e1 / L
    y = np.cross(normalize(nrm), x)
    pts = np.stack([(np.array([a, b, c]) - a) @ x, (np.array([a, b, c]) - a) @ y], 1)
    lo, hi = pts.min(0), pts.max(0)
    dy = s * math.sqrt(3.0) / 2.0
    rows = np.arange(lo[1] + dy * 0.5, hi[1], dy)
    out = []
    for k, yy in enumerate(rows):
        off = (0.5 * s) if (k % 2) else 0.0
        xs = np.arange(lo[0] + off + 0.25 * s, hi[0], s)
        if len(xs):
            out.append(np.stack([xs, np.full(len(xs), yy)], 1))
    if not out:
        return np.zeros((0, 3))
    q = np.concatenate(out)
    keep = np.ones(len(q), bool)
    for i in range(3):
        p0, p1 = pts[i], pts[(i + 1) % 3]
        d = p1 - p0
        ln = np.linalg.norm(d)
        nn = np.array([-d[1], d[0]]) / ln
        if (pts[(i + 2) % 3] - p0) @ nn < 0:
            nn = -nn
        keep &= ((q - p0) @ nn) > 0.55 * s
    q = q[keep]
    return a + q[:, :1] * x + q[:, 1:2] * y


def tessellate(V, T, spacing, log=print):
    """V (n, 3), T (m, 3) triangles; spacing (m,) the target edge per face (inf: a face not to subdivide).
    Returns V2, T2, parent (each new triangle's face in T), edge_of (each new vertex's edge id or -1 for a corner or
    an inner point), edges (the edge table) and the count of faces that fell back to a fan."""
    V = np.asarray(V, np.float64)
    T = np.asarray(T, np.int64)
    edges, fe, fwd = _edge_table(T)
    elen = np.linalg.norm(V[edges[:, 1]] - V[edges[:, 0]], axis=1)
    es = np.full(len(edges), np.inf)
    for k in range(3):
        np.minimum.at(es, fe[:, k], spacing)
    cuts = np.where(np.isfinite(es), np.maximum(np.ceil(elen / np.maximum(es, 1e-6) - 1e-6).astype(np.int64) - 1, 0),
                    0)
    # the edge points (each edge's from its lower vertex id up: the same points for both faces)
    first = np.zeros(len(edges) + 1, np.int64)
    first[1:] = np.cumsum(cuts)
    nE = int(first[-1])
    t_all = np.zeros(nE)
    e_all = np.repeat(np.arange(len(edges)), cuts)
    k_all = np.arange(nE) - first[e_all]
    t_all = (k_all + 1) / (cuts[e_all] + 1)
    EP = V[edges[e_all, 0]] * (1 - t_all)[:, None] + V[edges[e_all, 1]] * t_all[:, None]
    base = len(V)
    newV = [V, EP]
    edge_of = np.concatenate([np.full(len(V), -1), e_all])
    nxt = base + nE
    outT, parent = [], []
    plain = np.ones(len(T), bool)
    fans = 0
    work = np.nonzero(np.isfinite(spacing) | (cuts[fe].sum(1) > 0))[0]
    plain[work] = False
    for f in work:
        tri = T[f]
        # the boundary loop: corner, its edge's points in the face's direction, the next corner ...
        loop = []
        for k in range(3):
            loop.append(int(tri[k]))
            e = fe[f, k]
            ids = base + first[e] + np.arange(cuts[e])
            if not fwd[f, k]:
                ids = ids[::-1]
            loop.extend(int(i) for i in ids)
        a, b, c = V[tri[0]], V[tri[1]], V[tri[2]]
        nrm = np.cross(b - a, c - a)
        ln = np.linalg.norm(nrm)
        if ln < 1e-12:
            # (a degenerate face: a fan from its first corner keeps its edges' points)
            for i in range(1, len(loop) - 1):
                outT.append((loop[0], loop[i], loop[i + 1]))
                parent.append(f)
            continue
        nrm = nrm / ln
        x = normalize(b - a)
        y = np.cross(nrm, x)
        Lp = np.array([V[i] if i < base else EP[i - base] for i in loop])
        q = np.stack([(Lp - a) @ x, (Lp - a) @ y], 1)
        inner = np.zeros((0, 3))
        if np.isfinite(spacing[f]):
            inner = _lattice(a, b, c, float(spacing[f])) if spacing[f] > 0 else np.zeros((0, 3))
        qi = np.stack([(inner - a) @ x, (inner - a) @ y], 1) if len(inner) else np.zeros((0, 2))
        allq = np.concatenate([q, qi])
        nb = len(q)
        tris = None
        if nb + len(qi) > 3:
            try:
                pts = [Vector((float(u), float(v))) for u, v in allq]
                scale = max(float(np.abs(q).max()), 1e-6)
                res = geometry.delaunay_2d_cdt(pts, [], [list(range(nb))], 1, 1e-9 * scale, True)
                ov, faces, orig = res[0], res[2], res[3]
                if len(ov) == len(pts) and all(len(o) == 1 for o in orig):
                    back = np.array([o[0] for o in orig], np.int64)
                    F = np.array([[back[i] for i in fc] for fc in faces if len(fc) == 3], np.int64)
                    if len(F):
                        # each the face's winding, none degenerate, the face covered once
                        Q = allq[F]
                        sgn = ((Q[:, 1, 0] - Q[:, 0, 0]) * (Q[:, 2, 1] - Q[:, 0, 1]) -
                               (Q[:, 1, 1] - Q[:, 0, 1]) * (Q[:, 2, 0] - Q[:, 0, 0]))
                        F = F[np.abs(sgn) > 1e-14 * scale * scale]
                        sgn = sgn[np.abs(sgn) > 1e-14 * scale * scale]
                        F[sgn < 0] = F[sgn < 0][:, [0, 2, 1]]
                        area = 0.5 * np.abs(sgn).sum()
                        full = 0.5 * ln
                        if abs(area - full) <= 1e-5 * max(full, 1e-9):
                            tris = F
            except Exception:
                tris = None
        elif nb == 3 and not len(qi):
            tris = np.array([[0, 1, 2]])
        ids = np.array(loop + list(range(nxt, nxt + len(qi))), np.int64)
        if tris is None:
            # (the fan from the boundary's centre: always valid for a convex loop)
            fans += 1
            cen = Lp.mean(0)
            ids = np.array(loop + [nxt], np.int64)
            newV.append(cen[None])
            edge_of = np.concatenate([edge_of, [-1]])
            nxt += 1
            for i in range(nb):
                outT.append((loop[i], loop[(i + 1) % nb], int(ids[-1])))
                parent.append(f)
            continue
        if len(qi):
            newV.append(inner)
            edge_of = np.concatenate([edge_of, np.full(len(qi), -1)])
            nxt += len(qi)
        for t_ in ids[tris]:
            outT.append(tuple(int(i) for i in t_))
            parent.append(f)
    V2 = np.concatenate(newV)
    T_new = np.array(outT, np.int64).reshape(-1, 3)
    # (the faces' own winding: the corners' order as T has it)
    pa = np.array(parent, np.int64)
    if len(T_new):
        fn0 = np.cross(V[T[pa, 1]] - V[T[pa, 0]], V[T[pa, 2]] - V[T[pa, 0]])
        fn1 = np.cross(V2[T_new[:, 1]] - V2[T_new[:, 0]], V2[T_new[:, 2]] - V2[T_new[:, 0]])
        flip = (fn0 * fn1).sum(1) < 0
        T_new[flip] = T_new[flip][:, [0, 2, 1]]
    keep = np.nonzero(plain)[0]
    T2 = np.concatenate([T[keep], T_new])
    parent2 = np.concatenate([keep, pa])
    log("tessellation: %d faces subdivided, %d edges cut into %d points, %d inner points, %d triangles (%d fans)" %
        (len(work), int((cuts > 0).sum()), nE, len(V2) - base - nE, len(T2), fans))
    return V2, T2, parent2, edge_of, edges, fans


def edges_of(T):
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    e.sort(1)
    return np.unique(e, axis=0)


def vertex_spacing(X, E, n):
    """each vertex's mean edge length"""
    ln = np.linalg.norm(X[E[:, 1]] - X[E[:, 0]], axis=1)
    s = np.bincount(E[:, 0], ln, n) + np.bincount(E[:, 1], ln, n)
    c = np.bincount(E[:, 0], None, n) + np.bincount(E[:, 1], None, n)
    return s / np.maximum(c, 1)


# ---------- the smooth base

def taubin_along(X, N, E, movable, iters=12, lam=0.5, mu=-0.53, weight=None):
    """Taubin smoothing (lam, mu steps: no shrinking) of X, each vertex moving only along its normal N, the vertices
    not movable held (and still pulling their neighbours). weight: per edge (default 1)."""
    X = X.copy()
    n = len(X)
    i, j = E[:, 0], E[:, 1]
    w = np.ones(len(E)) if weight is None else weight
    deg = np.bincount(i, w, n) + np.bincount(j, w, n)
    deg = np.maximum(deg, 1e-12)
    mv = movable.astype(np.float64)
    for _ in range(iters):
        for f in (lam, mu):
            L = np.empty_like(X)
            for k in range(3):
                L[:, k] = (np.bincount(i, w * X[j, k], n) + np.bincount(j, w * X[i, k], n)) / deg - X[:, k]
            dn = (L * N).sum(1)
            X += (f * dn * mv)[:, None] * N
    return X


def smooth_scalar(d, E, movable, iters=2):
    """a few Laplacian passes of a per-vertex value over the mesh's edges (held where not movable)"""
    n = len(d)
    i, j = E[:, 0], E[:, 1]
    deg = np.maximum(np.bincount(i, None, n) + np.bincount(j, None, n), 1)
    for _ in range(iters):
        m = (np.bincount(i, d[j], n) + np.bincount(j, d[i], n)) / deg
        d = np.where(movable, 0.5 * d + 0.5 * m, d)
    return d


def limit_slope(d, X, E, movable, gmax=0.8, iters=6):
    """a per-vertex offset whose difference along any edge is at most gmax times the edge's length (each excess
    shared between the edge's two ends, held ones not moving): no spike, no wall steeper than the mesh can carry"""
    n = len(d)
    i, j = E[:, 0], E[:, 1]
    L = np.linalg.norm(X[j] - X[i], axis=1)
    mv = movable.astype(np.float64)
    for _ in range(iters):
        diff = d[i] - d[j]
        ex = np.sign(diff) * np.maximum(np.abs(diff) - gmax * L, 0.0)
        if not ex.any():
            break
        wi, wj = mv[i], mv[j]
        tot = np.maximum(wi + wj, 1e-12)
        di = np.bincount(i, -ex * wi / tot, n)
        dj = np.bincount(j, ex * wj / tot, n)
        cnt = np.bincount(i, (ex != 0) * 1.0, n) + np.bincount(j, (ex != 0) * 1.0, n)
        d = d + (di + dj) / np.maximum(cnt, 1.0)
    return d


# ---------- noise (deterministic, a function of position or of texture coordinates alone)

def _hash01(ix, iy, iz, salt):
    with np.errstate(over="ignore"):
        h = (ix.astype(np.int64) * 73856093) ^ (iy.astype(np.int64) * 19349663) ^ (iz.astype(np.int64) * 83492791) ^ \
            np.int64(salt * 2654435761 & 0x7FFFFFFF)
        h = h.astype(np.uint64)
        h ^= h >> np.uint64(13)
        h *= np.uint64(0x5BD1E995)
        h ^= h >> np.uint64(15)
    return (h & np.uint64(0xFFFFFF)).astype(np.float64) / float(0xFFFFFF)


def value_noise3(p, scale, salt=0):
    """smooth 3D value noise in [-1, 1], features about `scale` across"""
    q = p / scale
    i = np.floor(q).astype(np.int64)
    f = q - i
    u = f * f * (3.0 - 2.0 * f)
    out = np.zeros(len(p))
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = ((u[:, 0] if dx else 1 - u[:, 0]) * (u[:, 1] if dy else 1 - u[:, 1]) *
                     (u[:, 2] if dz else 1 - u[:, 2]))
                out += w * _hash01(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz, salt)
    return out * 2.0 - 1.0


def periodic_noise2(uv, cells_u, cells_v, salt=0):
    """smooth 2D value noise in [-1, 1] over texture space, cells_u by cells_v cells a repeat, repeating in u (and v)
    with the texture"""
    x = uv[:, 0] * cells_u
    y = uv[:, 1] * cells_v
    ix = np.floor(x).astype(np.int64)
    iy = np.floor(y).astype(np.int64)
    fx, fy = x - ix, y - iy
    ux, uy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    out = np.zeros(len(uv))
    zero = np.zeros(len(uv), np.int64)
    for dx in (0, 1):
        for dy in (0, 1):
            w = (ux if dx else 1 - ux) * (uy if dy else 1 - uy)
            out += w * _hash01(np.mod(ix + dx, cells_u), np.mod(iy + dy, cells_v), zero, salt)
    return out * 2.0 - 1.0


def worley2(p, scale, salt=0):
    """jittered cells `scale` across: F1 and F2 (wu) and the two nearest cells' ids and feature points"""
    q = np.floor(p / scale).astype(np.int64)
    f1 = np.full(len(p), np.inf)
    f2 = np.full(len(p), np.inf)
    c1 = np.zeros(len(p), np.int64)
    c2 = np.zeros(len(p), np.int64)
    p1 = np.zeros_like(p)
    p2 = np.zeros_like(p)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                c = q + np.array([dx, dy, dz])
                jit = np.stack([_hash01(c[:, 0], c[:, 1], c[:, 2], salt + k) for k in range(3)], 1)
                fp = (c + 0.1 + 0.8 * jit) * scale
                d = np.linalg.norm(p - fp, axis=1)
                ident = ((c[:, 0] * 73856093) ^ (c[:, 1] * 19349663) ^ (c[:, 2] * 83492791)).astype(np.int64)
                first = d < f1
                second = ~first & (d < f2)
                f2 = np.where(first, f1, np.where(second, d, f2))
                c2 = np.where(first, c1, np.where(second, ident, c2))
                p2 = np.where(first[:, None], p1, np.where(second[:, None], fp, p2))
                f1 = np.where(first, d, f1)
                c1 = np.where(first, ident, c1)
                p1 = np.where(first[:, None], fp, p1)
    return f1, f2, c1, p1, c2, p2


def soft_facets(p, n, h, scale, depth, tilt, salt=41):
    """fractured rock: each jittered cell a plane of its own (set in by a depth of its own, tilted from the surface),
    blended with the next cell's over a band a few edges wide, so the ridges between facets are crisp only as far as
    the mesh can carry them (no step at a cell's edge)"""
    f1, f2, c1, p1, c2, p2 = worley2(p, scale, salt)

    def plane(cid, cpt):
        z = np.zeros(len(cid), np.int64)
        ch = _hash01(cid, z, z, salt + 2)
        g = np.stack([_hash01(cid, z, z, salt + 3 + j) for j in range(3)], 1) * 2.0 - 1.0
        g = g - n * (g * n).sum(1, keepdims=True)
        return -depth * (0.35 + 0.65 * ch) + tilt * ((p - cpt) * g).sum(1)
    o1 = plane(c1, p1)
    o2 = plane(c2, p2)
    soft = np.maximum(2.5 * h, 0.03)
    t = smoothstep(0.0, 1.0, (f2 - f1) / soft)
    d = (0.5 * (o1 + o2)) * (1.0 - t) + o1 * t
    return np.minimum(d, 0.0)


# ---------- texture bands

def box_blur_wrap(a, r):
    if r < 1:
        return a.copy()
    k = 2 * r + 1
    pad = np.pad(a, r, mode="wrap")
    c = np.pad(pad.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def box_blur_1d(a, r):
    if r < 1:
        return a.copy()
    k = 2 * r + 1
    pad = np.pad(a, r, mode="edge")
    c = np.concatenate([[0.0], np.cumsum(pad)])
    return (c[k:] - c[:-k]) / k


class TextureBands:
    """A base map's painted shading as height bands in [-1, 1] (luminance band-passes between box blurs, each with its
    wavelength in texels), and its rows' mean luminance as a strata profile along v (band-passed the same way)."""

    RADII = (1, 3, 8, 20)

    def __init__(self, rgb):
        lum = rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722
        self.h, self.w = lum.shape
        blurs = [lum] + [box_blur_wrap(lum, r) for r in self.RADII]
        self.bands = []
        for k in range(len(self.RADII)):
            b = blurs[k] - blurs[k + 1]
            self.bands.append(np.clip(b / max(float(b.std()) * 2.5, 1e-6), -1.0, 1.0))
        # (wavelength of band k: about twice its finer blur's width, at least two texels)
        self.lambda_px = [max(2.0, 2.0 * (2 * r0 + 1)) for r0 in (0,) + self.RADII[:-1]]
        rows = lum.mean(1)
        rb = [rows] + [box_blur_1d(rows, r) for r in self.RADII]
        self.rows = []
        for k in range(len(self.RADII)):
            b = rb[k] - rb[k + 1]
            self.rows.append(np.clip(b / max(float(b.std()) * 2.0, 1e-6), -1.0, 1.0))

    def sample(self, k, uv):
        return _bilinear_wrap(self.bands[k], uv)

    def sample_row(self, k, v):
        f = self.rows[k]
        y = np.clip(v, 0.0, 1.0) * self.h - 0.5
        y0 = np.clip(np.floor(y).astype(np.int64), 0, self.h - 1)
        y1 = np.clip(y0 + 1, 0, self.h - 1)
        t = np.clip(y - y0, 0.0, 1.0)
        return f[y0] * (1 - t) + f[y1] * t


def _bilinear_wrap(field, uv):
    h, w = field.shape
    x = uv[:, 0] * w - 0.5
    y = uv[:, 1] * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    x0 %= w
    y0 %= h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    return ((field[y0, x0] * (1 - fx) + field[y0, x1] * fx) * (1 - fy) +
            (field[y1, x0] * (1 - fx) + field[y1, x1] * fx) * fy)


def band_weight(lambda_wu, h, lo=3.0, hi=6.0):
    """how much of a band of wavelength lambda_wu a mesh of edge h carries: none under lo edges, all past hi"""
    return smoothstep(lo, hi, lambda_wu / np.maximum(h, 1e-6))


# ---------- the relief

def rock_relief(p, n, uv, texel_wu, h, steep, r, tex):
    """Rock and cliffs (distance along the normal, wu; negative into the rock). p, n positions and smooth normals; uv
    the base map's coordinates; texel_wu (n, 2) a texel's size on the surface along u and v; h each vertex's edge
    length; steep 1 on faces too steep to stand on, 0 on floors; tex: TextureBands (or None)."""
    m = len(p)
    d = np.zeros(m)
    parts = {}
    tw = np.sqrt(np.maximum(texel_wu[:, 0] * texel_wu[:, 1], 1e-12))
    if tex is not None:
        # the painted shading, at four scales; its dark (cracks, crevices) sunk deeper than its light stands out
        amps = r.get("tex_amps", [0.006, 0.014, 0.03, 0.05])
        dt = np.zeros(m)
        for k in range(len(tex.bands)):
            lam = tex.lambda_px[k] * tw
            b = tex.sample(k, uv)
            b = np.where(b < 0, b * r.get("crack_gain", 1.6), b)
            dt += amps[k] * band_weight(lam, h) * b
        # the painted rows: strata along v (CE's cliff strips run v up the cliff), warped a little along u
        warp = r.get("strata_warp_v", 0.012) * periodic_noise2(uv, r.get("warp_cells_u", 12), 4, salt=5)
        ds = np.zeros(m)
        samps = r.get("strata_amps", [0.0, 0.02, 0.05, 0.09])
        tv = texel_wu[:, 1]
        for k in range(len(tex.rows)):
            lam = tex.lambda_px[k] * tv
            ds += samps[k] * band_weight(lam, h) * tex.sample_row(k, uv[:, 1] + warp)
        # ledges: the strata's profile sharpened into soft steps (a terrace's flat top, a rounded lip)
        ledge = r.get("ledge", 0.4)
        ds = ds + ledge * (np.tanh(ds / max(r.get("ledge_soft", 0.03), 1e-6)) * np.abs(ds).mean() - ds) * 0.5
        d += dt + steep * ds
        parts["tex"] = dt
        parts["strata"] = steep * ds
    # erosion runnels down the face (along v), periodic in u: grooves where the noise crosses zero
    cu, cv = r.get("runnel_cells_u", 24), r.get("runnel_cells_v", 3)
    nz = periodic_noise2(uv, cu, cv, salt=17) + 0.5 * periodic_noise2(uv, 2 * cu, 2 * cv, salt=19)
    width = r.get("runnel_width", 0.18)
    run = 1.0 - smoothstep(0.0, width, np.abs(nz))
    lam_r = np.where(np.isfinite(texel_wu[:, 0]), (1.0 / max(cu, 1)) * texel_wu[:, 0] * (tex.w if tex else 256), 1.0)
    dr = -r.get("runnel_depth", 0.05) * run * band_weight(lam_r * width, h, 2.0, 4.0) * steep
    d += dr
    parts["runnels"] = dr
    # fractured facets (world space), band-limited by their size
    if r.get("facet_depth", 0.0) > 0:
        fs = r.get("facet_scale", 1.1)
        dfc = soft_facets(p, n, h, fs, r["facet_depth"], r.get("facet_tilt", 0.3))
        dfc *= band_weight(fs, h, 3.0, 6.0) * steep
        d += dfc
        parts["facets"] = dfc
    # boulder-scale swells (world space, smooth)
    sw = r.get("swell_scale", 2.6)
    dsw = r.get("swell_amp", 0.07) * (value_noise3(p, sw, salt=23) + 0.5 * value_noise3(p, sw * 0.5, salt=29)) / 1.5
    dsw *= band_weight(sw * 0.5, h, 2.0, 4.0)
    d += dsw * (0.35 + 0.65 * steep)
    parts["swell"] = dsw
    # floors keep only a little of it all
    floor_keep = r.get("floor_keep", 0.35)
    d *= floor_keep + (1.0 - floor_keep) * steep
    return d, parts


def ground_relief(p, n, uv, texel_wu, h, flatness, r, tex):
    """Sand, earth and grass: low swells from the base map's painted shading, wind ripples where it lies flat, small
    lumps; all band-limited."""
    m = len(p)
    d = np.zeros(m)
    parts = {}
    tw = np.sqrt(np.maximum(texel_wu[:, 0] * texel_wu[:, 1], 1e-12))
    if tex is not None:
        amps = r.get("tex_amps", [0.003, 0.008, 0.02, 0.035])
        dt = np.zeros(m)
        for k in range(len(tex.bands)):
            lam = tex.lambda_px[k] * tw
            dt += amps[k] * band_weight(lam, h) * tex.sample(k, uv)
        d += dt
        parts["tex"] = dt
    lam = r.get("ripple_wavelength", 1.2)
    if r.get("ripple_amp", 0.0) > 0:
        ang = math.radians(r.get("ripple_dir_deg", 30.0))
        dirv = np.array([math.cos(ang), math.sin(ang), 0.0])
        bend = r.get("ripple_bend", 1.2) * value_noise3(p, r.get("ripple_bend_scale", 6.0), salt=61)
        x = (p @ dirv) / lam + bend
        tt = x - np.floor(x)
        # a smooth asymmetric ripple (gentle windward, steeper lee), no corner at its crest
        prof = np.sin(2 * np.pi * tt) * 0.8 + 0.2 * np.sin(4 * np.pi * tt + 0.6)
        patch = smoothstep(-0.2, 0.5, value_noise3(p, r.get("ripple_patch_scale", 8.0), salt=67))
        dr = r["ripple_amp"] * prof * patch * flatness * band_weight(lam, h)
        d += dr
        parts["ripple"] = dr
    if r.get("lump_amp", 0.0) > 0:
        ls = r.get("lump_scale", 0.7)
        dl = r["lump_amp"] * value_noise3(p, ls, salt=71) * band_weight(ls, h, 2.0, 4.0)
        d += dl
    sw = r.get("swell_scale", 5.0)
    if r.get("swell_amp", 0.0) > 0:
        d += r["swell_amp"] * value_noise3(p, sw, salt=73)
    return d, parts


def soft_clamp(d, lo, hi):
    """lo <= 0 <= hi: d squeezed into (lo, hi) smoothly (tanh on each side); a zero bound clamps to zero"""
    lo = np.broadcast_to(np.asarray(lo, np.float64), d.shape)
    hi = np.broadcast_to(np.asarray(hi, np.float64), d.shape)
    neg = np.where(lo < 0, lo * np.tanh(d / np.where(lo < 0, lo, -1.0)), 0.0)
    pos = np.where(hi > 0, hi * np.tanh(d / np.where(hi > 0, hi, 1.0)), 0.0)
    return np.where(d < 0, neg, pos)


def texel_size(P, UV, tex_w, tex_h):
    """(m, 2) a texel's size in world units along u and v on each triangle (P (m, 3, 3), UV (m, 3, 2))"""
    e1, e2 = P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]
    d1, d2 = UV[:, 1] - UV[:, 0], UV[:, 2] - UV[:, 0]
    nn = np.cross(e1, e2)
    A2 = np.maximum((nn * nn).sum(1), 1e-18)
    gu = (d1[:, :1] * np.cross(e2, nn) + d2[:, :1] * np.cross(nn, e1)) / A2[:, None]
    gv = (d1[:, 1:2] * np.cross(e2, nn) + d2[:, 1:2] * np.cross(nn, e1)) / A2[:, None]
    lu = 1.0 / np.maximum(np.linalg.norm(gu, axis=1), 1e-9)
    lv = 1.0 / np.maximum(np.linalg.norm(gv, axis=1), 1e-9)
    return np.stack([lu / tex_w, lv / tex_h], 1)
