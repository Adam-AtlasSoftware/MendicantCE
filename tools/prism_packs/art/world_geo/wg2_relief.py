"""PRISM world geometry v2: the relief of a metal texture's own bump map made real geometry (wg2_remodel.py's
"relief" recipes, for the Forerunner metal of b30, a30 and c10).

CE's bump maps are normal maps of a height the artists painted: thin channels (the "traces" and industrial details of
the Forerunner floors and walls) sunk into flat plates. Here that height is taken back from the normal map (its
slopes integrated over the tile, which repeats, by FFT: Frankot and Chellappa), its slow drift taken out, and the
channels traced as two contour lines each (their lip and their floor) in the texture's own plane. The tile's mesh is
those lines triangulated (Blender's constrained Delaunay): flat plates and floors are a few large triangles, the
walls a strip between lip and floor, so the triangle count follows the length of the lines, not the area.

Each flat metal triangle of the remodel then takes the tile's mesh clipped to itself in texture space, and its
vertices are set into the surface by the channel's depth (wg2_remodel.py). The plates stay exactly on CE's surface
(where one collides); only the channels go in. Depths are the bump map's own: its slopes in texels times the
texture's size on the surface, times the recipe's relief_scale.

Plain numpy, and mathutils' delaunay_2d_cdt; everything made here is derived from the player's map: local only.
"""
import math

import numpy as np

# (the normal map's x: a normal leans away from the way the height rises; its y is the one of the two signs whose
# slopes integrate (rows run up or down the image depending on who loaded it))
SIGN_X = -1.0


def box_blur(a, r):
    if r < 1:
        return a.copy()
    k = 2 * r + 1
    pad = np.pad(a, r, mode="wrap")
    c = pad.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def height_from_bump(px, drift_px=16):
    """px: the bump map's pixels (h, w, >=3) in [0, 1]. Returns the height in texels (h, w): 0 on the plates, negative
    in the channels, with its slow drift (the slopes' non-integrable rest) taken out by a wide blur."""
    n = px[..., :3] * 2.0 - 1.0
    nz = np.maximum(n[..., 2], 0.2)
    gx = SIGN_X * n[..., 0] / nz
    h, w = gx.shape
    kx = 2j * np.pi * np.fft.fftfreq(w)[None, :]
    ky = 2j * np.pi * np.fft.fftfreq(h)[:, None]
    den = np.abs(kx) ** 2 + np.abs(ky) ** 2
    den[0, 0] = 1.0
    best = None
    for sy in (-1.0, 1.0):
        gy = sy * n[..., 1] / nz
        Hf = (np.conj(kx) * np.fft.fft2(gx) + np.conj(ky) * np.fft.fft2(gy)) / den
        Hf[0, 0] = 0.0
        # (the rest the height cannot explain)
        res = np.abs(kx * Hf - np.fft.fft2(gx)) ** 2 + np.abs(ky * Hf - np.fft.fft2(gy)) ** 2
        res = float(res.sum())
        if best is None or res < best[0]:
            best = (res, Hf)
    H = np.real(np.fft.ifft2(best[1]))
    H = H - box_blur(box_blur(H, drift_px), drift_px)
    # the plates' level: most of the tile's area
    return H - np.percentile(H, 60)


# ---------- contour lines (marching squares on the repeating grid, cut at the tile's edges)

# segments per case, as pairs of cell edges: 0 bottom (y=j), 1 right (x=i+1), 2 top (y=j+1), 3 left (x=i)
_CASES = {1: [(3, 0)], 2: [(0, 1)], 3: [(3, 1)], 4: [(1, 2)], 6: [(0, 2)], 7: [(3, 2)], 8: [(2, 3)], 9: [(2, 0)],
          11: [(2, 1)], 12: [(1, 3)], 13: [(1, 0)], 14: [(0, 3)]}


def contours(F, c):
    """Polylines of F = c on the tile (node (i, j) at texel (i, j), F repeating). Returns a list of (points (n, 2) in
    node coordinates x in [0, w], y in [0, h], closed). Open lines end on the tile's edges, where the line goes on in
    the next tile: its end there is the same point as the other side's start."""
    h, w = F.shape
    v00 = F
    v10 = np.roll(F, -1, 1)
    v01 = np.roll(F, -1, 0)
    v11 = np.roll(np.roll(F, -1, 0), -1, 1)
    case = ((v00 > c).astype(np.int64) | ((v10 > c).astype(np.int64) << 1) | ((v11 > c).astype(np.int64) << 2) |
            ((v01 > c).astype(np.int64) << 3))
    pts = {}

    def edge_point(i, j, e):
        # the cell edge's key (a node and an orientation) and its point
        if e == 0:
            key = ("h", i, j)
        elif e == 2:
            key = ("h", i, j + 1)
        elif e == 3:
            key = ("v", i, j)
        else:
            key = ("v", i + 1, j)
        if key not in pts:
            o, a, b = key
            if o == "h":
                fa, fb = F[b % h, a % w], F[b % h, (a + 1) % w]
                t = (c - fa) / (fb - fa)
                pts[key] = (a + t, float(b))
            else:
                fa, fb = F[b % h, a % w], F[(b + 1) % h, a % w]
                t = (c - fa) / (fb - fa)
                pts[key] = (float(a), b + t)
        return key

    adj = {}
    for j, i in zip(*np.nonzero((case != 0) & (case != 15))):
        k = int(case[j, i])
        if k == 5 or k == 10:
            centre = 0.25 * (v00[j, i] + v10[j, i] + v01[j, i] + v11[j, i]) > c
            if k == 5:
                segs = [(3, 2), (1, 0)] if centre else [(3, 0), (1, 2)]
            else:
                segs = [(0, 1), (2, 3)] if centre else [(2, 1), (0, 3)]
        else:
            segs = _CASES[k]
        for e1, e2 in segs:
            a = edge_point(i, j, e1)
            b = edge_point(i, j, e2)
            adj.setdefault(a, []).append(b)
            adj.setdefault(b, []).append(a)
    lines = []
    seen = set()

    def walk(start):
        line = [start]
        seen.add(start)
        prev, cur = None, start
        while True:
            nxt = [n for n in adj[cur] if n != prev and n not in seen]
            if not nxt:
                # closed when it came back to its start
                closed = len(line) > 2 and start in adj[cur]
                return line, closed
            prev, cur = cur, nxt[0]
            line.append(cur)
            seen.add(cur)
    # open lines first, from their ends (a point with one neighbour: on the tile's edge)
    for key, nb in adj.items():
        if len(nb) == 1 and key not in seen:
            line, _ = walk(key)
            lines.append((np.array([pts[k] for k in line]), False))
    for key in adj:
        if key not in seen:
            line, closed = walk(key)
            if len(line) >= 3:
                lines.append((np.array([pts[k] for k in line]), closed))
    return lines


def simplify(P, tol, closed):
    """Douglas-Peucker; an open line keeps its ends, a closed one its two farthest points"""
    if len(P) < 3:
        return P
    if closed:
        d = np.linalg.norm(P - P[0], axis=1)
        far = int(np.argmax(d))
        a = simplify(np.concatenate([P[:far + 1]]), tol, False)
        b = simplify(np.concatenate([P[far:], P[:1]]), tol, False)
        return np.concatenate([a[:-1], b[:-1]])
    keep = np.zeros(len(P), bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(P) - 1)]
    while stack:
        s, e = stack.pop()
        if e - s < 2:
            continue
        a, b = P[s], P[e]
        ab = b - a
        L = np.linalg.norm(ab)
        q = P[s + 1:e] - a
        if L < 1e-12:
            d = np.linalg.norm(q, axis=1)
        else:
            d = np.abs(ab[0] * q[:, 1] - ab[1] * q[:, 0]) / L
        k = int(np.argmax(d))
        if d[k] > tol:
            m = s + 1 + k
            keep[m] = True
            stack.append((s, m))
            stack.append((m, e))
    return P[keep]


# ---------- the tile's mesh

class Tile:
    """A texture's relief over one repeat: uv (n, 2) over [u0, u0 + 1] x [v0, v0 + 1], depth (n,) in texels (<= 0),
    triangles (m, 3). u0 = 0.5 / w, v0 = 0.5 / h (the tile's corners on texel centres, where the contours' grid
    is)."""

    def __init__(self, uv, depth, tris, size):
        self.uv = uv
        self.depth = depth
        self.tris = tris
        self.size = size  # texels (w, h)
        self.origin = np.array([0.5 / size[0], 0.5 / size[1]])


def build_tile(H, r):
    """The relief's mesh from the height H (texels, plates at 0). The channels' depth d is the height's 1st
    percentile (how deep CE's bump map goes); recipe: relief_lip (the share of d below the plates where a channel's
    wall starts, default 0.2), relief_floor (where its floor starts, default 0.55; the floor itself lies at the
    median height inside that line), relief_tolerance (the lines' simplification, texels, default 0.45; raised until
    the tile has at most relief_tile_triangles, default 6000), relief_min_loop (closed lines shorter than this many
    texels are dropped: specks, default 3)."""
    d = max(-float(np.percentile(H, 1)), 1e-3)
    lip = -abs(r.get("relief_lip", 0.2)) * d
    floor_ = -abs(r.get("relief_floor", 0.55)) * d
    below = H < floor_
    floor_depth = float(np.median(H[below])) if below.any() else floor_
    tol = r.get("relief_tolerance", 0.45)
    most = r.get("relief_tile_triangles", 6000)
    min_loop = r.get("relief_min_loop", 3.0)
    lines = [(P, closed, val) for val in (lip, floor_) for P, closed in contours(H, val)]
    for _ in range(6):
        tile = _triangulate(H, lines, lip, floor_, floor_depth, tol, min_loop)
        if len(tile.tris) <= most:
            break
        tol *= 1.6
        min_loop *= 1.6
    tile.levels = {"d": d, "lip": lip, "floor": floor_, "floor_depth": floor_depth, "tolerance": tol}
    return tile


def _triangulate(H, lines, lip, floor_, floor_depth, tol, min_loop):
    from mathutils import Vector, geometry

    h, w = H.shape
    pts, kinds, edges = [], [], []
    for P, closed, level in lines:
        if closed:
            per = np.linalg.norm(np.diff(np.concatenate([P, P[:1]]), axis=0), axis=1).sum()
            if per < min_loop:
                continue
        Q = simplify(P, tol, closed)
        if len(Q) < 2 or (closed and len(Q) < 3):
            continue
        base = len(pts)
        for q in Q:
            pts.append((float(q[0]), float(q[1])))
            kinds.append(0.0 if level == lip else floor_depth)
        n = len(Q)
        for k in range(n - 1):
            edges.append((base + k, base + k + 1))
        if closed:
            edges.append((base + n - 1, base))
    # the tile's edges: its corners and every line's end on them, so that the next tile meets the same points
    on_edge = [i for i, (x, y) in enumerate(pts) if x <= 1e-9 or x >= w - 1e-9 or y <= 1e-9 or y >= h - 1e-9]
    corners = [(0.0, 0.0), (float(w), 0.0), (float(w), float(h)), (0.0, float(h))]
    ncontour = len(pts)
    for cx, cy in corners:
        pts.append((cx, cy))
        kinds.append(None)
    cidx = list(range(len(pts) - 4, len(pts)))
    for side in range(4):
        along = []
        for i in on_edge:
            x, y = pts[i]
            if side == 0 and y <= 1e-9:
                along.append((x, i))
            elif side == 1 and x >= w - 1e-9:
                along.append((y, i))
            elif side == 2 and y >= h - 1e-9:
                along.append((-x, i))
            elif side == 3 and x <= 1e-9:
                along.append((-y, i))
        chain = [cidx[side]] + [i for _, i in sorted(along)] + [cidx[(side + 1) % 4]]
        for k in range(len(chain) - 1):
            if chain[k] != chain[k + 1]:
                edges.append((chain[k], chain[k + 1]))
    out = geometry.delaunay_2d_cdt([Vector(p) for p in pts], edges, [], 0, 1e-6, True)
    verts, faces, orig_v = out[0], out[2], out[3]
    uv = np.array([(v[0], v[1]) for v in verts], np.float64)
    depth = np.zeros(len(uv))
    cpts = np.array(pts[:ncontour]) if ncontour else np.zeros((0, 2))
    ckind = np.array(kinds[:ncontour]) if ncontour else np.zeros(0)
    for k, ov in enumerate(orig_v):
        lv = [kinds[o] for o in ov if o < len(kinds) and kinds[o] is not None]
        if lv:
            depth[k] = min(lv)
            continue
        # (a corner, or where two lines crossed: the nearest line's level when one is near, else its own region's)
        x, y = uv[k]
        if len(cpts):
            dd = np.hypot(cpts[:, 0] - x, cpts[:, 1] - y)
            j = int(np.argmin(dd))
            if dd[j] < 1.5:
                depth[k] = ckind[j]
                continue
        hv = H[int(round(y)) % h, int(round(x)) % w]
        depth[k] = 0.0 if hv > lip else (floor_depth if hv < floor_ else 0.0)
    tris = []
    for f in faces:
        f = list(f)
        for k in range(1, len(f) - 1):
            tris.append((f[0], f[k], f[k + 1]))
    tris = np.array(tris, np.int64).reshape(-1, 3)
    uv_t = (uv + 0.5) / np.array([w, h], np.float64)
    return Tile(uv_t, depth, tris, (w, h))


# ---------- clipping the tile's mesh into a triangle (texture space)

def _clip_half(polys, cnt, a, b, c):
    """keep where a*u + b*v + c >= 0; polys (N, K, 3): u, v, depth; cnt (N,). Returns the same shapes (K + 1)."""
    N, K, _ = polys.shape
    out = np.zeros((N, K + 1, 3))
    oc = np.zeros(N, np.int64)
    idx = np.arange(N)
    for k in range(K):
        live = k < cnt
        nk = np.where(k + 1 < cnt, k + 1, 0)
        p = polys[:, k]
        q = polys[idx, nk]
        fp = a * p[:, 0] + b * p[:, 1] + c
        fq = a * q[:, 0] + b * q[:, 1] + c
        inp = fp >= 0
        inq = fq >= 0
        put = live & inp
        out[idx[put], oc[put]] = p[put]
        oc = oc + put
        cross = live & (inp != inq)
        t = np.where(cross, fp / np.where(cross, fp - fq, 1.0), 0.0)
        x = p + (q - p) * t[:, None]
        out[idx[cross], oc[cross]] = x[cross]
        oc = oc + cross
    return out, oc


def clip_tile(tile, tri_uv, max_tiles=64):
    """The tile's triangles that fall in the triangle tri_uv (3, 2), clipped to it: (points (n, 3) as u, v, depth;
    triangles (m, 3) into them), or None when the triangle covers more than max_tiles repeats. Points of one
    triangle are not shared between its sub-triangles here (the caller welds them by position)."""
    lo = tri_uv.min(0) - tile.origin
    hi = tri_uv.max(0) - tile.origin
    i0, j0 = int(math.floor(lo[0])), int(math.floor(lo[1]))
    i1, j1 = int(math.floor(hi[0])), int(math.floor(hi[1]))
    if (i1 - i0 + 1) * (j1 - j0 + 1) > max_tiles:
        return None
    # the triangle's edges as half-planes (inside >= 0)
    p0, p1, p2 = tri_uv
    area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])
    if abs(area) < 1e-14:
        return None
    sgn = 1.0 if area > 0 else -1.0
    halves = []
    for a_, b_ in ((p0, p1), (p1, p2), (p2, p0)):
        # left of a_ -> b_ for a counter-clockwise triangle
        nx, ny = -(b_[1] - a_[1]) * sgn, (b_[0] - a_[0]) * sgn
        halves.append((nx, ny, -(nx * a_[0] + ny * a_[1])))
    T = tile.tris
    base = tile.uv[T]  # (m, 3, 2)
    dep = tile.depth[T]
    polys_in, polys_part = [], []
    for i in range(i0, i1 + 1):
        for j in range(j0, j1 + 1):
            P = base + np.array([i, j], np.float64)
            f = np.stack([hx * P[..., 0] + hy * P[..., 1] + hc for hx, hy, hc in halves], -1)  # (m, 3 corners, 3)
            inside = (f >= -1e-12).all(2).all(1)
            outside = (f < 1e-12).all(1).any(1)
            # (and the triangle wholly outside one of the tile triangle's own edges: left to the clip, which drops it)
            part = ~inside & ~outside
            if inside.any():
                polys_in.append(np.concatenate([P[inside], dep[inside][..., None]], -1))
            if part.any():
                polys_part.append(np.concatenate([P[part], dep[part][..., None]], -1))
    pts, tris = [], []
    n = 0
    if polys_in:
        A = np.concatenate(polys_in)
        pts.append(A.reshape(-1, 3))
        tris.append(np.arange(len(A) * 3).reshape(-1, 3))
        n = len(A) * 3
    if polys_part:
        B = np.concatenate(polys_part)
        cnt = np.full(len(B), 3, np.int64)
        for hx, hy, hc in halves:
            B, cnt = _clip_half(B, cnt, hx, hy, hc)
        keep = cnt >= 3
        B, cnt = B[keep], cnt[keep]
        for k in range(1, B.shape[1] - 1):
            sel = np.nonzero(cnt > k + 1)[0]
            if not len(sel):
                continue
            tri = np.stack([B[sel, 0], B[sel, k], B[sel, k + 1]], 1)
            pts.append(tri.reshape(-1, 3))
            tris.append(n + np.arange(len(sel) * 3).reshape(-1, 3))
            n += len(sel) * 3
    if not pts:
        return np.zeros((0, 3)), np.zeros((0, 3), np.int64)
    return np.concatenate(pts), np.concatenate(tris)
