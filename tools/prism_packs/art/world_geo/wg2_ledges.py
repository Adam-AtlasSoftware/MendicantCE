"""PRISM world geometry: sharp rock ledges from a CC0 scan (a rock recipe's "ledges"), for the kit's builder
(wg2_remodel.py, wg2_shape.py). Plain numpy, deterministic, a function of position only.

The cliff is cut into layers (bedding) stacked in height: each layer a face leaning back a little, with its own
set-back (varying slowly along the face), and between two layers a riser: a hard step, out (an undercut, whose
underside shades) or in (a shelf, whose top catches the light). The layers' lines follow a CC0 scan's height
(wg2_scans.json, Marble Cliff 03 by default), stretched along the bedding (anisotropic: long along it, short
across), so they wander, thin and thicken as the scan's breaks do; a little of the scan's own height stays on the
faces. Nothing finer than the mesh carries is added: the scan is band-limited by the vertex's edge across the
bedding, and the layers fade out where the mesh has fewer than about three rows a layer.

A step is only as sharp as the mesh lets it be. So the vertices next to a riser line slide along the face (in the
stock triangle's plane, within the stock triangle, never one on a stock edge, a pinned or a fading one) onto the
riser's two edges, a riser's height apart: one row at the foot, one at the top. The ledge is then hard at any
density, and no triangle is added. A slide that would turn or collapse a triangle is undone.

Lengths in world units (1 wu = 3.05 m); d is along the surface normal, negative into the rock.
"""
import numpy as np

import wg2_natural as nat
import wg2_shape as shp

DEFAULTS = {
    "scan": "ph_marble_cliff_03",
    "tile_v": 3.77,          # the scan's tile across the bedding (wu): twice its measured 5.75 m
    "stretch_u": 2.5,        # along the bedding, this many times longer
    "layer": 2.0,            # a layer's mean thickness (wu)
    "wander": 0.35,          # the scan's pull on the layer lines (layers) ...
    "phase_band": 0.6,       # ... its broad shape only: nothing under this many layers
    "warp": 0.5,             # a slow dip of the bedding (wu) ...
    "warp_scale": 14.0,      # ... over this (wu)
    "lean": 0.12,            # a layer face's lean back, top against foot (wu)
    "setback": 0.20,         # the spread of the layers' set-backs (wu)
    "setback_drift": 0.08,   # their slow change along the face (wu) ...
    "drift_scale": 7.0,      # ... over this (wu)
    "riser": 0.07,           # a riser's height (wu)
    "scan_amp": 0.03,        # the scan's own height left on the faces (wu, its deviation)
    "bias": -0.08,           # the layers set in (wu): the lips near the collision surface, the faces behind it
    "rows_lo": 1.5,          # the layers fade in from this many mesh rows a layer ...
    "rows_hi": 2.5,          # ... to full
    "snap_band": 0.55,       # vertices within this many edges of a riser slide onto it
    "boundary_lo": 0.3,      # near another class's faces: the kit's shape kept within this (wu) ...
    "boundary_hi": 1.2,      # ... the ledges in full past this (wu)
}


def params(r):
    p = dict(DEFAULTS)
    p.update(r.get("ledges", {}) if isinstance(r.get("ledges"), dict) else {})
    return p


def _hash01(a, b, c, salt):
    return shp.hash01(a, b, c, salt=salt)


def layer_noise(xy, k, scale, salt):
    """a smooth value noise in [-1, 1] along the face (world x, y), different in every layer k"""
    q = xy / scale
    i = np.floor(q).astype(np.int64)
    f = q - i
    u = f * f * (3.0 - 2.0 * f)
    out = np.zeros(len(xy))
    for dx in (0, 1):
        for dy in (0, 1):
            w = (u[:, 0] if dx else 1 - u[:, 0]) * (u[:, 1] if dy else 1 - u[:, 1])
            out += w * _hash01(i[:, 0] + dx, i[:, 1] + dy, k, salt)
    return out * 2.0 - 1.0


def aniso_scan(field, p, n, h, tile_v, stretch_u):
    """the scan's height in world space on the two side projections (rows level: v up the face), u stretched
    stretch_u times along the bedding; band-limited by the edge h across it (the stricter way). Deviation units.
    Faces turned up take the mean of both side views (the ledges are for the steep faces)."""
    w = np.maximum(np.abs(n[:, :2]) - 0.05, 0.0) ** 4
    w = w / np.maximum(w.sum(1, keepdims=True), 1e-12)
    out = np.zeros(len(p))
    acc = np.zeros(len(p))
    tile_u = tile_v * stretch_u
    for axis, a in ((0, 1), (1, 0)):
        sel = w[:, axis] > 1e-4
        if not sel.any():
            continue
        face = np.sign(n[sel, axis])
        face = np.where(face == 0, 1.0, face)
        u = face * p[sel, a] / tile_u + 0.37 * axis
        v = -p[sel, 2] / tile_v + 0.21 * axis
        hv = field.sample(np.stack([u, v], 1), h[sel] / tile_v)
        out[sel] += w[sel, axis] * hv
        acc[sel] += w[sel, axis] ** 2
    return out / np.maximum(np.sqrt(acc), 1e-6)


def phase(p, n, h, q, field):
    """the layer coordinate (layers, rising with height) and the scan's height there (band-limited by h). The layer
    lines follow the scan's broad shape only (band-limited to phase_band layers: the scan's own sharp breaks would
    turn a layer line vertical)"""
    hs = aniso_scan(field, p, n, h, q["tile_v"], q["stretch_u"])
    hb = aniso_scan(field, p, n, np.maximum(h, q["phase_band"] * q["layer"]), q["tile_v"], q["stretch_u"])
    warp = q["warp"] * shp.fbm(p * np.array([1.0, 1.0, 0.25]), q["warp_scale"], 2, salt=211)
    return (p[:, 2] + warp) / q["layer"] + q["wander"] * hb, hs


def layer_values(p, k, q):
    """a layer's set-back and lean at p (wu)"""
    xy = p[:, :2]
    z = np.zeros_like(k)
    off = q["setback"] * (_hash01(k, z + 7, z + 3, 223) - 0.5) + \
        q["setback_drift"] * layer_noise(xy, k, q["drift_scale"], 227)
    lean = q["lean"] * (0.4 + 0.6 * _hash01(k, z + 11, z + 5, 229))
    return off, lean


def riser_width(q):
    """the riser's height in layers"""
    return q["riser"] / q["layer"]


def profile(ph, p, q):
    """d (wu) of the layers at phase ph: within a layer its set-back and lean back (its foot out, its top in), then
    over the last riser_width of it the riser to the next layer's foot (smooth only across that height)"""
    k = np.floor(ph).astype(np.int64)
    t = ph - k
    wr = riser_width(q)
    off, lean = layer_values(p, k, q)
    off1, lean1 = layer_values(p, k + 1, q)
    tt = np.clip(t / (1.0 - wr), 0.0, 1.0)
    face = off + lean * (0.5 - tt)
    top = off - 0.5 * lean
    foot1 = off1 + 0.5 * lean1
    s = shp.smoothstep(1.0 - wr, 1.0, t)
    return face + s * (foot1 - top)


def relief(p, n, h, steep, q, field):
    """the ledges' d (wu) at p on faces of normal n, edge h; steep 0..1 (1 a cliff face)"""
    ph, hs = phase(p, n, h, q, field)
    rows = q["layer"] / np.maximum(h, 1e-6)
    fade = shp.smoothstep(q["rows_lo"], q["rows_hi"], rows)
    d = fade * (profile(ph, p, q) + q["bias"]) + q["scan_amp"] * hs
    return d * steep, {"ledges": d * steep, "fade": fade}


def snap_targets(V, up, h, eligible, q, field, n):
    """for the eligible vertices: the slide along up (wu) that puts each one next to a riser on the riser's foot or
    top (0 where none is near)"""
    slide = np.zeros(len(V))
    ids = np.nonzero(eligible)[0]
    if not len(ids):
        return slide
    eps = 0.02
    p0 = V[ids]
    nn = n[ids]
    hh = h[ids]
    ph0, _ = phase(p0, nn, hh, q, field)
    php, _ = phase(p0 + up[ids] * eps, nn, hh, q, field)
    phm, _ = phase(p0 - up[ids] * eps, nn, hh, q, field)
    s = (php - phm) / (2.0 * eps)  # layers a wu along up
    ok = s > 0.25 / q["layer"]
    wr = riser_width(q)
    k = np.floor(ph0)
    t = ph0 - k
    band = q["snap_band"] * hh * np.maximum(s, 1e-6)  # (in layers)
    # below a riser (the layer's top): onto the riser's foot, phase k + 1 - wr
    to_foot = (k + 1.0 - wr) - ph0
    below = ok & (t < 1.0 - wr) & ((1.0 - wr - t) < band)
    # inside a riser: to whichever edge is nearer
    inside = ok & (t >= 1.0 - wr)
    to_top = (k + 1.0 + 1e-4) - ph0
    # above a riser (the next layer's foot): onto the riser's top, phase k + small
    to_prev_top = (k + 1e-4) - ph0
    above = ok & (t < band) & ~below
    out = np.zeros(len(ids))
    out = np.where(below, to_foot / np.maximum(s, 1e-6), out)
    out = np.where(above, to_prev_top / np.maximum(s, 1e-6), out)
    near_top = inside & (to_top < (t - (1.0 - wr)))
    out = np.where(inside & near_top, to_top / np.maximum(s, 1e-6), out)
    out = np.where(inside & ~near_top, to_foot / np.maximum(s, 1e-6), out)
    # never more than the band (an edge's share) in wu
    lim = q["snap_band"] * hh * 1.2
    out = np.where(np.abs(out) <= lim, out, 0.0)
    slide[ids] = out
    return slide


def slide_vertices(V, T, faces, up, slide, owner_tri, max_rounds=6):
    """V moved by slide along up, but no face of `faces` (rock faces) turned over or collapsed to under a tenth of its
    area within its plane, and no vertex outside its stock triangle (owner_tri: PA, PB, PC per vertex); returns the
    new V and which vertices moved"""
    moving = slide != 0.0
    PA, PB, PC = owner_tri
    Vn = V.copy()
    for _ in range(max_rounds):
        Vn = V + up * np.where(moving, slide, 0.0)[:, None]
        # inside the stock triangle
        bc = barycentric(Vn, PA, PB, PC)
        out = moving & (bc.min(1) < 1e-4)
        moving &= ~out
        Vn = V + up * np.where(moving, slide, 0.0)[:, None]
        sel = faces[moving[T[faces]].any(1)]
        if not len(sel):
            break
        a0, b0, c0 = V[T[sel, 0]], V[T[sel, 1]], V[T[sel, 2]]
        a1, b1, c1 = Vn[T[sel, 0]], Vn[T[sel, 1]], Vn[T[sel, 2]]
        n0 = np.cross(b0 - a0, c0 - a0)
        n1 = np.cross(b1 - a1, c1 - a1)
        a0n = np.linalg.norm(n0, axis=1)
        bad = ((n0 * n1).sum(1) <= 0.1 * a0n * a0n) | (np.linalg.norm(n1, axis=1) < 0.1 * a0n)
        if not bad.any():
            break
        moving[T[sel[bad]].ravel()] = False
    Vn = V + up * np.where(moving, slide, 0.0)[:, None]
    return Vn, moving


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00 = (v0 * v0).sum(1)
    d01 = (v0 * v1).sum(1)
    d11 = (v1 * v1).sum(1)
    d20 = (v2 * v0).sum(1)
    d21 = (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return np.stack([1.0 - v - w, v, w], 1)


def field_for(q):
    return nat.scan_field(q["scan"])


def up_tangent(plane):
    """the face's up direction within its plane (zero on faces turned up or down)"""
    z = np.array([0.0, 0.0, 1.0])
    u = z[None, :] - plane * plane[:, 2:3]
    ln = np.linalg.norm(u, axis=1, keepdims=True)
    return np.where(ln > 0.3, u / np.maximum(ln, 1e-12), 0.0)


def boundary_weight(dist, q):
    return shp.smoothstep(q["boundary_lo"], q["boundary_hi"], dist)
