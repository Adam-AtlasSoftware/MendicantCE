"""A vehicle's tyres as CE lays them out: which of CE's faces are the tread and which the side discs, the tread's
map of angle and width fitted from them, and the tread's relief from CE's own painted blocks. Plain numpy (no
Blender): vehicle_remodel.py builds the round tyres on it, vehicle_material.py the tyre's normal and roughness maps,
and tools/test_prism_vehicles.py checks it.

CE's faces, not its vertex normals, say what is tread: CE smooths the normals across the tread's edge (the
Warthog's tread corners lean 0.43 to 0.56 along the axle) and a dished hub's inner corners face outward, so a
choice by the normals took the hub's corners for the tread and fitted the tread to the hub disc's and the motion
blur's part of the map (the Warthog's tyres smeared into streaks where they stood still).
"""
import numpy as np


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)


def face_normals(P, T):
    # (CE's front faces wind clockwise about their outward normal; only the direction across the axle matters here)
    return unit(-np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]]))


def radii(P, c, axle):
    q = P - c
    return np.linalg.norm(q - np.outer(q @ axle, axle), axis=1)


def wheel_axle(P, T):
    """the axle of a wheel of CE's (its vertices P, its triangles T): the direction its side faces' normals share
    (their area-weighted second moment's main axis), else the vertices' least spread"""
    fn = -np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    area = np.linalg.norm(fn, axis=1)
    fu = unit(fn)
    Q = P[np.unique(T)]
    c = (Q.max(0) + Q.min(0)) * 0.5
    spread = np.linalg.eigh(np.cov((Q - c).T))[1][:, 0]
    side = np.abs(fu @ spread) > 0.7
    if side.sum() >= 4:
        m = (fu[side] * area[side, None]).T @ fu[side]
        return np.linalg.eigh(m)[1][:, -1]
    return spread


def tread_faces(P, T, c, axle, across=0.3, rim=0.9):
    """CE's tread faces: their own normal across the axle (|n . axle| < across) and every corner on the rim (at
    least rim of the wheel's radius: its own vertices', T's)"""
    r = radii(P, c, axle)
    fn = face_normals(P, T)
    return (np.abs(fn @ axle) < across) & (r[T].min(1) >= rim * r[T].max())


def side_faces(P, T, axle, along=0.7):
    """CE's side faces (the hub discs): their own normal along the axle"""
    return np.abs(face_normals(P, T) @ axle) > along


def fit_tread_map(Q, uv, axle, e1, e2, half):
    """the tread's map from CE's tread corners (Q their positions from the wheel's centre, uv theirs): u across the
    width (from CE's lowest u to its highest, CE's sign), v around, k whole repeats a turn from v0. Returns the fit
    and its rms error in v."""
    th = np.arctan2(Q @ e2, Q @ e1) / (2 * np.pi)
    u_lo, u_hi = float(uv[:, 0].min()), float(uv[:, 0].max())
    c = np.corrcoef(Q @ axle, uv[:, 0])[0, 1]
    sgn = float(np.sign(c)) if np.isfinite(c) and c != 0 else 1.0
    best = None
    for k in (-4, -3, -2, -1, 1, 2, 3, 4):
        # (v0 for this k: the circular mean of each corner's offset, refined on a fine grid about it)
        off = uv[:, 1] - k * th
        ang = np.angle(np.exp(2j * np.pi * off).mean()) / (2 * np.pi)
        for v0 in ang + np.linspace(-0.01, 0.01, 41):
            e = np.mod(off - v0 + 0.5, 1.0) - 0.5
            err = float(np.sqrt((e * e).mean()))
            if best is None or err < best[0]:
                best = (err, k, float(np.mod(v0, 1.0)))
    err, k, v0 = best
    return {"u_lo": u_lo, "u_hi": u_hi, "sign": sgn, "k": int(k), "v0": v0, "half": float(half), "rms": err}


def tread_uv(fit, Q, axle, e1, e2, width=0.8):
    """the tread's map at positions Q (from the wheel's centre): across the middle width of the tyre (its shoulders
    beyond) the strip of CE's tread, around k repeats a turn"""
    th = np.arctan2(Q @ e2, Q @ e1) / (2 * np.pi)
    frac = np.clip(((Q @ axle) / fit["half"] * fit["sign"] + width) / (2 * width), 0, 1)
    return np.stack([fit["u_lo"] + frac * (fit["u_hi"] - fit["u_lo"]), fit["v0"] + fit["k"] * th], 1)


def fit_disc_map(X, uv):
    """a side disc's map: uv an affine map of the position across the axle (X: rows of (x, y, 1)), fitted to CE's
    side corners, then again without the first fit's outliers"""
    A, *_ = np.linalg.lstsq(X, uv, rcond=None)
    keep = np.abs(X @ A - uv).max(1) < 0.03
    if 3 <= keep.sum() < len(keep):
        A, *_ = np.linalg.lstsq(X[keep], uv[keep], rcond=None)
    return A


# ---------------------------------------------------------------- the relief of CE's painted tread

def box_blur(x, r):
    """a wrapped box blur of radius r (texels)"""
    if r < 1:
        return x
    k = 2 * r + 1
    pad = np.pad(x, r, mode="wrap")
    c = np.pad(pad.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def luminance(rgb):
    return rgb[..., :3] @ np.array([0.2126, 0.7152, 0.0722])


def relief_height(lum, local_px=8, edge_px=1):
    """a height in 0..1 from a painted map's luminance (rows top first): what is lighter than its neighbourhood
    (local_px texels about it) stands up, what is darker is cut in (the tread's blocks and grooves, the block ends
    and the side wall's ribs as CE painted them), its edges softened over edge_px texels"""
    hp = lum - box_blur(lum, local_px)
    t = max(float(hp.std()) * 0.6, 1e-6)
    h = np.clip(0.5 + 0.5 * hp / t, 0.0, 1.0)
    h = h * h * (3 - 2 * h)
    for _ in range(edge_px):
        h = box_blur(h, 1)
    return np.clip(h, 0.0, 1.0)


def sample(field, uv):
    """field (rows top first: v = 0 the top row) bilinearly at uv, wrapped"""
    h, w = field.shape
    x = np.mod(uv[:, 0], 1.0) * w - 0.5
    y = np.mod(uv[:, 1], 1.0) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    x0 %= w
    y0 %= h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    return ((field[y0, x0] * (1 - fx) + field[y0, x1] * fx) * (1 - fy) +
            (field[y1, x0] * (1 - fx) + field[y1, x1] * fx) * fy)
