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

Round 3 (all off by default, so a recipe without them builds round 2's A): breaks in the bedding (a layer line
jogs up or down by a share of a layer over a short run along the face, where a slow noise crosses zero), blocks
(each layer cut into jittered cells in plan, some of which set in or out, so short vertical fractures cross a layer
and stop at the next), and fine detail on the faces between ledges (a second, smaller and more stretched copy of the
scan, band-limited by the edge like the first, so it adds only what the mesh carries near the player). PRESETS name
the variants compared in round 3 ("a2", "a3"); a recipe's "preset" picks one, its other keys override it.

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
    # round 3
    "break": 0.0,            # a bedding break's jog (layers; 0 none) ...
    "break_len": 3.5,        # ... where a noise this long along the face (wu) crosses zero ...
    "break_sharp": 0.12,     # ... over this share of the noise (smaller: a shorter jog)
    "block": 0.0,            # the blocks' cells in plan (wu across; 0 none) ...
    "block_share": 0.4,      # ... this share of them set in or out ...
    "block_setback": 0.0,    # ... by up to this (wu)
    "fine_amp": 0.0,         # fine detail on the faces (wu, deviation) ...
    "fine_tile": 1.3,        # ... the scan's tile across the bedding (wu) ...
    "fine_stretch": 4.0,     # ... this many times longer along it ...
    "fine_band": 1.0,        # ... band-limited by the edge times this (1: the v3 rule, 3 to 6 edges a wavelength)
    # round 4
    "cut": False,            # the risers' two edges cut into the mesh along their lines (cut_risers) ...
    "cut_tmin": 0.2,         # ... an edge cut only between this share of it and 1 - it (no needle; nearer, the
                             #     vertex itself stands for the line)
    "block_soft": 0.0,       # the blocks' walls blended over this (wu; 0: a hard jump, round 3)
    "chunk_amp": 0.0,        # the kit's chunks again, low frequency only: soft-creased tilted facets (times) ...
    "chunk_scale": 1.6,      # ... cells this many wu across ...
    "chunk_tilt": 0.14,      # ... each facet's tilt (wu a wu) ...
    "chunk_soft": 0.08,      # ... its creases blended over this (wu)
}

# round 3's variants (b30a): A2 the round-2 layers with steps about 1.9 times deeper; A3 thinner layers (1.25 wu)
# with steps about 1.5 times deeper. Both: breaks, blocks (short vertical fractures), fine detail on the faces.
PRESETS = {
    "a2": {"layer": 2.0, "setback": 0.38, "setback_drift": 0.12, "lean": 0.20, "riser": 0.14, "bias": -0.11,
           "scan_amp": 0.035, "break": 0.22, "break_len": 3.5, "block": 2.6, "block_share": 0.4,
           "block_setback": 0.10, "fine_amp": 0.028, "fine_tile": 1.3, "fine_stretch": 4.0},
    "a3": {"layer": 1.25, "setback": 0.30, "setback_drift": 0.10, "lean": 0.15, "riser": 0.11, "bias": -0.09,
           "scan_amp": 0.03, "break": 0.28, "break_len": 3.0, "block": 2.0, "block_share": 0.45,
           "block_setback": 0.09, "fine_amp": 0.028, "fine_tile": 1.1, "fine_stretch": 4.0},
}
# round 4: A4 is A3 with the risers cut into the mesh along their lines (no sawtooth), soft block walls (no jump for
# a triangle to stretch over: the slivers), and the kit's chunks back at low frequency under the ledges
PRESETS["a4"] = dict(PRESETS["a3"], cut=True, block_soft=0.10, chunk_amp=1.0, chunk_scale=1.6, chunk_tilt=0.14,
                     chunk_soft=0.08)


def params(r):
    p = dict(DEFAULTS)
    own = r.get("ledges", {}) if isinstance(r.get("ledges"), dict) else {}
    if own.get("preset"):
        p.update(PRESETS[own["preset"]])
    p.update({k: v for k, v in own.items() if k != "preset"})
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
    ph = (p[:, 2] + warp) / q["layer"] + q["wander"] * hb
    if q.get("break", 0.0) > 0.0:
        ph = ph + breaks(p, q)
    return ph, hs


def breaks(p, q):
    """the bedding's breaks (layers): a slow noise along the face (changing over about two layers in height), its
    zero crossings turned into short jogs of +-break/2"""
    zs = q["break_len"] / (2.0 * q["layer"])
    nz = shp.value_noise(p * np.array([1.0, 1.0, zs]), q["break_len"], salt=233)
    b = q["break_sharp"]
    return q["break"] * (shp.smoothstep(-b, b, nz) - 0.5)


def block_offset(p, k, q):
    """each layer's blocks: jittered cells in plan (their walls vertical, a different set in every layer), a share of
    them set in or out (wu)"""
    if q.get("block", 0.0) <= 0.0 or q.get("block_setback", 0.0) <= 0.0:
        return np.zeros(len(p))
    s = q["block"]
    p3 = np.stack([p[:, 0], p[:, 1], (k.astype(np.float64) * 7.31 + 0.5) * s], 1)

    def cell_value(cid):
        u = _hash01(cid, k, np.zeros_like(k) + 13, 241)
        on = u < q["block_share"]
        return np.where(on, q["block_setback"] * (2.0 * u / max(q["block_share"], 1e-6) - 1.0), 0.0)
    if q.get("block_soft", 0.0) > 0.0:
        # (round 4: the walls blended over block_soft, so no triangle spans a jump)
        return soft_cells(p3, s, 239, q["block_soft"], lambda cid, fp: cell_value(cid), plan=True)
    _, _, cid, _ = shp.worley(p3, s, salt=239)
    return cell_value(cid)


def soft_cells(p, scale, salt, soft, value, plan=False):
    """shp.worley's jittered cells, each cell's value(cid, feature point) blended by exp(-(d - F1) / soft): a
    Voronoi pattern with its walls soft over about `soft` wu (continuous, so a mesh of any density carries it
    without teeth). plan: the distance in x, y only (vertical walls; the cells of p's own z layer)"""
    q = np.floor(p / scale).astype(np.int64)
    ds, vs = [], []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in ((0,) if plan else (-1, 0, 1)):
                c = q + np.array([dx, dy, dz])
                jit = np.stack([shp.hash01(c[:, 0], c[:, 1], c[:, 2], salt=salt + j) for j in range(3)], 1)
                fp = (c + 0.1 + 0.8 * jit) * scale
                dd = p - fp
                d = np.linalg.norm(dd[:, :2] if plan else dd, axis=1)
                ident = ((c[:, 0] * 73856093) ^ (c[:, 1] * 19349663) ^ (c[:, 2] * 83492791)).astype(np.int64)
                ds.append(d)
                vs.append(value(ident, fp))
    D = np.stack(ds, 1)
    Vv = np.stack(vs, 1)
    w = np.exp(-(D - D.min(1, keepdims=True)) / soft)
    return (w * Vv).sum(1) / w.sum(1)


def chunks(p, n, q):
    """the kit's chipped facets at low frequency (round 4): tilted planes in cells chunk_scale across, set in, their
    creases soft over chunk_soft (about the kit's chips at twice their size, chunk_amp times)"""
    if q.get("chunk_amp", 0.0) <= 0.0:
        return np.zeros(len(p))

    def facet(cid, fp):
        ch = shp.hash01(cid, salt=443)
        g = np.stack([shp.hash01(cid, salt=447 + j) for j in range(3)], 1) * 2.0 - 1.0
        g = g - n * (g * n).sum(1, keepdims=True)
        return -0.06 * (0.35 + 0.65 * ch) + q["chunk_tilt"] * ((p - fp) * g).sum(1)
    return q["chunk_amp"] * soft_cells(p, q["chunk_scale"], 441, q["chunk_soft"], facet)


def layer_values(p, k, q):
    """a layer's set-back and lean at p (wu)"""
    xy = p[:, :2]
    z = np.zeros_like(k)
    off = q["setback"] * (_hash01(k, z + 7, z + 3, 223) - 0.5) + \
        q["setback_drift"] * layer_noise(xy, k, q["drift_scale"], 227)
    lean = q["lean"] * (0.4 + 0.6 * _hash01(k, z + 11, z + 5, 229))
    off = off + block_offset(p, k, q)
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
    parts = {"fade": fade}
    if q.get("chunk_amp", 0.0) > 0.0:
        dc = chunks(p, n, q)
        d = d + dc
        parts["chunks"] = dc * steep
    if q.get("fine_amp", 0.0) > 0.0:
        hf = aniso_scan(field, p + 11.3, n, h * q["fine_band"], q["fine_tile"], q["fine_stretch"])
        d = d + q["fine_amp"] * hf
        parts["fine"] = q["fine_amp"] * hf * steep
    parts["ledges"] = d * steep
    return d * steep, parts


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


def cut_risers(V, T, Ns, h, splittable, face_ok, q, field):
    """round 4: the risers' two lines (a layer's top, phase k + 1 - riser_width, and the next one's foot, phase
    k + 1) cut into the mesh: every edge between two splittable vertices whose faces are all face_ok, that a line
    crosses, gets a vertex on the line (the crossing found on the phase by regula falsi), and its faces are split
    (one edge cut: two triangles through the opposite corner; two: the corner's triangle and the quad left by its
    shorter diagonal; three: four). A crossing within cut_tmin of an end is left to that vertex (no needle). So a
    step's edges are the lines themselves, smooth curves at any density: no tooth where a triangle spanned them.
    Returns V, T, parent (each face's old face), lerps [(a, b, t)] (each pass's new vertices, in order:
    lerp(X[a], X[b], t) of the arrays as they stood) and a report."""
    wr = riser_width(q)
    tmin = q.get("cut_tmin", 0.15)
    V = V.copy()
    Ns = Ns.copy()
    h = h.copy()
    splittable = splittable.copy()
    parent = np.arange(len(T))
    lerps = []
    rep = {}

    def key(x):
        return x[:, 0].astype(np.int64) * (1 << 31) + x[:, 1]
    for c, label in ((1.0 - wr, "top"), (0.0, "foot")):
        ph, _ = phase(V, Ns, h, q, field)
        side = np.floor(ph - c)
        Fo = np.nonzero(face_ok)[0]
        e = np.sort(np.concatenate([T[Fo][:, [0, 1]], T[Fo][:, [1, 2]], T[Fo][:, [2, 0]]]), 1)
        ko, koc = np.unique(key(e), return_counts=True)
        eu = np.stack([ko >> 31, ko & ((1 << 31) - 1)], 1)
        ea = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), 1)
        ka, kc = np.unique(key(ea), return_counts=True)
        a_, b_ = eu[:, 0], eu[:, 1]
        cand = splittable[a_] & splittable[b_] & (np.abs(side[a_] - side[b_]) == 1)
        cand &= kc[np.searchsorted(ka, ko)] == koc
        a_, b_ = a_[cand], b_[cand]
        target = np.maximum(side[a_], side[b_]) + c
        lo, hi = np.zeros(len(a_)), np.ones(len(a_))
        flo, fhi = ph[a_] - target, ph[b_] - target

        def secant(lo, hi, flo, fhi):
            den = np.where(np.abs(flo - fhi) > 1e-12, flo - fhi, 1e-12)
            return np.clip(lo + (hi - lo) * flo / den, lo, hi)
        t = secant(lo, hi, flo, fhi)
        for _ in range(4):
            pm = V[a_] + (V[b_] - V[a_]) * t[:, None]
            nm = Ns[a_] + (Ns[b_] - Ns[a_]) * t[:, None]
            nm /= np.maximum(np.linalg.norm(nm, axis=1, keepdims=True), 1e-12)
            hm = h[a_] + (h[b_] - h[a_]) * t
            fm = phase(pm, nm, hm, q, field)[0] - target
            same = np.sign(fm) == np.sign(flo)
            lo, flo = np.where(same, t, lo), np.where(same, fm, flo)
            hi, fhi = np.where(same, hi, t), np.where(same, fhi, fm)
            t = secant(lo, hi, flo, fhi)
        keep = (t > tmin) & (t < 1.0 - tmin)
        a_, b_, t = a_[keep], b_[keep], t[keep]
        newid = len(V) + np.arange(len(a_))
        V = np.concatenate([V, V[a_] + (V[b_] - V[a_]) * t[:, None]])
        nn = Ns[a_] + (Ns[b_] - Ns[a_]) * t[:, None]
        Ns = np.concatenate([Ns, nn / np.maximum(np.linalg.norm(nn, axis=1, keepdims=True), 1e-12)])
        h = np.concatenate([h, h[a_] + (h[b_] - h[a_]) * t])
        splittable = np.concatenate([splittable, np.ones(len(a_), bool)])
        lerps.append((a_, b_, t))
        ck = key(np.stack([a_, b_], 1))
        order = np.argsort(ck)
        ck, cid = ck[order], newid[order]
        mids = np.full((len(T), 3), -1, np.int64)
        if len(ck):
            for j, (i0, i1) in enumerate(((0, 1), (1, 2), (2, 0))):
                ek = key(np.sort(T[:, [i0, i1]], 1))
                at = np.clip(np.searchsorted(ck, ek), 0, len(ck) - 1)
                mids[:, j] = np.where(ck[at] == ek, cid[at], -1)
        ncut = (mids >= 0).sum(1)
        cutc = np.zeros(4, np.int64)
        out_T, out_P, out_ok = [T[ncut == 0]], [parent[ncut == 0]], [face_ok[ncut == 0]]
        for f in np.nonzero(ncut > 0)[0]:
            v = T[f]
            m = mids[f]  # m[0] on v0-v1, m[1] on v1-v2, m[2] on v2-v0
            k_ = int(ncut[f])
            cutc[k_] += 1
            if k_ == 1:
                j = int(np.nonzero(m >= 0)[0][0])
                tris = [(v[j], m[j], v[(j + 2) % 3]), (m[j], v[(j + 1) % 3], v[(j + 2) % 3])]
            elif k_ == 2:
                u = int(np.nonzero(m < 0)[0][0])  # the uncut edge v[u]-v[u+1]; the corner v[u+2]
                c0 = (u + 2) % 3
                v0, v1, v2 = v[c0], v[(c0 + 1) % 3], v[(c0 + 2) % 3]
                m1, m2 = m[c0], m[(c0 + 2) % 3]  # on v0-v1 and on v2-v0
                tris = [(v0, m1, m2)]
                if np.linalg.norm(V[m1] - V[v2]) <= np.linalg.norm(V[v1] - V[m2]):
                    tris += [(m1, v1, v2), (m1, v2, m2)]
                else:
                    tris += [(m1, v1, m2), (v1, v2, m2)]
            else:
                tris = [(v[0], m[0], m[2]), (m[0], v[1], m[1]), (m[2], m[1], v[2]), (m[0], m[1], m[2])]
            out_T.append(np.array(tris, np.int64))
            out_P.append(np.full(len(tris), parent[f]))
            out_ok.append(np.ones(len(tris), bool))
        T = np.concatenate(out_T)
        parent = np.concatenate(out_P)
        face_ok = np.concatenate(out_ok)
        rep[label] = {"edges_crossed": int(cand.sum()), "cut": int(len(a_)), "faces_cut_1_2_3": cutc[1:].tolist()}
    return V, T, parent, lerps, rep
