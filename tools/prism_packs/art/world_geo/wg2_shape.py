"""PRISM world geometry v2: the shaping functions (plain numpy, deterministic, functions of position only, so that any
mesh of the same surface, finer or coarser, takes the same shape).

Lengths are world units (1 wu = 3.05 m). Every function returns a signed distance along the surface's normal:
negative is into the rock, behind the collision surface; positive stands out of it.
"""
import math

import numpy as np


# ---------- hashing and noise

def _hash_u64(*ints):
    h = np.uint64(0x9E3779B97F4A7C15)
    for i, v in enumerate(ints):
        v = np.asarray(v).astype(np.int64).astype(np.uint64)
        h = h ^ (v * np.uint64((0xBF58476D1CE4E5B9 + 0x94D049BB133111EB * (i + 1)) & 0xFFFFFFFFFFFFFFFF))
        h = h ^ (h >> np.uint64(31))
        h = h * np.uint64(0x94D049BB133111EB)
        h = h ^ (h >> np.uint64(29))
    return h


def hash01(*ints, salt=0):
    """uniform [0, 1) per integer lattice point"""
    with np.errstate(over="ignore"):
        h = _hash_u64(*ints, np.full(np.shape(ints[0]), salt, np.int64))
    return (h >> np.uint64(11)).astype(np.float64) / float(1 << 53)


def value_noise(p, scale, salt=0):
    """smooth 3D value noise in [-1, 1], features about `scale` wu across"""
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
                out += w * hash01(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz, salt=salt)
    return out * 2.0 - 1.0


def fbm(p, scale, octaves=3, salt=0):
    total, amp, norm = np.zeros(len(p)), 1.0, 0.0
    for o in range(octaves):
        total += amp * value_noise(p, scale / (2 ** o), salt=salt + 17 * o)
        norm += amp
        amp *= 0.5
    return total / norm


def worley(p, scale, salt=0):
    """F1, F2 (wu), the nearest cell's id (int64) and its feature point, for jittered cells `scale` wu across"""
    q = np.floor(p / scale).astype(np.int64)
    f1 = np.full(len(p), np.inf)
    f2 = np.full(len(p), np.inf)
    cid = np.zeros(len(p), np.int64)
    cpt = np.zeros_like(p)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                c = q + np.array([dx, dy, dz])
                jit = np.stack([hash01(c[:, 0], c[:, 1], c[:, 2], salt=salt + k) for k in range(3)], 1)
                fp = (c + 0.1 + 0.8 * jit) * scale
                d = np.linalg.norm(p - fp, axis=1)
                closer = d < f1
                f2 = np.where(closer, f1, np.minimum(f2, d))
                f1 = np.where(closer, d, f1)
                ident = ((c[:, 0] * 73856093) ^ (c[:, 1] * 19349663) ^ (c[:, 2] * 83492791)).astype(np.int64)
                cid = np.where(closer, ident, cid)
                cpt = np.where(closer[:, None], fp, cpt)
    return f1, f2, cid, cpt


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------- rock

def rock(p, n, r, tex_coarse, tex_fine):
    """Cliff rock: strata (layers in height, bent by noise, each layer with its own set-back, a notch between layers
    whose lower lip is an overhang's underside and whose upper lip is a ledge), chipped facets (cells with their own
    tilted planes) and the CE texture's own painted relief at two scales. Steep faces take it all; faces one can
    stand on keep only small relief. Returns (d, parts)."""
    steep = 1.0 - smoothstep(r.get("floor_cos_lo", 0.55), r.get("floor_cos_hi", 0.85), n[:, 2])
    # strata
    T0 = r["strata_thickness"]
    warp = r["strata_warp"] * fbm(p, r["strata_warp_scale"], 2, salt=11)
    phase = (p[:, 2] + warp) / T0 + 0.35 * value_noise(p, r["strata_warp_scale"] * 0.6, salt=23)
    k = np.floor(phase).astype(np.int64)
    t = phase - k
    # a layer's identity also changes along the face, so that one layer's set-back is not the same all round
    region = np.floor((p[:, 0] + 3.1 * p[:, 1]) / r["strata_region"]).astype(np.int64)
    setback = hash01(k, region, salt=31)
    notch = hash01(k, region, salt=37)
    lip_lo, lip_hi = r.get("notch_lo", 0.14), r.get("notch_hi", 0.86)
    s = smoothstep(0.0, lip_lo, t) * (1.0 - smoothstep(lip_hi, 1.0, t))
    depth = r["strata_depth"]
    d_strata = -depth * (r.get("setback_share", 0.45) * setback * s + (0.55 + 0.45 * notch) * (1.0 - s))
    # a slight lean inside each layer (weathered tops stand out more than the bottoms)
    d_strata += depth * 0.18 * (t - 0.5) * s
    # chipped facets
    f1, f2, cid, cpt = worley(p, r["chip_scale"], salt=41)
    ch = hash01(cid, salt=43)
    g = np.stack([hash01(cid, salt=47 + j) for j in range(3)], 1) * 2.0 - 1.0
    g = g - n * (g * n).sum(1, keepdims=True)
    d_chip = -r["chip_depth"] * (0.35 + 0.65 * ch) + r.get("chip_tilt", 0.35) * ((p - cpt) * g).sum(1)
    d_chip = np.minimum(d_chip, 0.0)
    crack = -r.get("crack_depth", 0.0) * (1.0 - smoothstep(0.0, r.get("crack_width", 0.05), f2 - f1))
    d_tex = r["tex_coarse"] * tex_coarse + r["tex_fine"] * tex_fine
    d_floor = r.get("floor_relief", 0.0) * fbm(p, r.get("floor_relief_scale", 0.8), 3, salt=53)
    d = steep * (d_strata + d_chip + crack) + d_tex + (1.0 - steep) * d_floor
    return d, {"steep": steep, "strata": d_strata, "chip": d_chip, "tex": d_tex}


def ground(p, n, r, tex_coarse, tex_fine):
    """Sand and earth: wind ripples (bent by noise, fading in and out in patches), low swells from the CE texture's own
    painted shading, and small lumps."""
    ang = math.radians(r.get("ripple_dir_deg", 30.0))
    dirv = np.array([math.cos(ang), math.sin(ang), 0.0])
    lam = r["ripple_wavelength"]
    bend = r.get("ripple_bend", 1.5) * fbm(p, r.get("ripple_bend_scale", 2.5), 2, salt=61)
    x = (p @ dirv) / lam + bend
    # asymmetric ripple: gentle windward slope, steeper lee
    tt = x - np.floor(x)
    prof = np.where(tt < 0.7, tt / 0.7, (1.0 - tt) / 0.3) * 2.0 - 1.0
    patch = smoothstep(-0.2, 0.5, fbm(p, r.get("ripple_patch_scale", 4.0), 2, salt=67))
    flat = smoothstep(0.75, 0.95, n[:, 2])  # ripples only where sand lies flat enough
    d = r["ripple_amp"] * prof * patch * flat
    d += r.get("lump_amp", 0.0) * fbm(p, r.get("lump_scale", 0.5), 3, salt=71)
    d += r["tex_coarse"] * tex_coarse + r["tex_fine"] * tex_fine
    return d, {"ripple": d}


def smooth(p, n, r, tex_coarse, tex_fine):
    """Rounded only (Phong over CE's normals does it): no relief of its own"""
    return np.zeros(len(p)), {}


def soft_clamp(d, lo, hi):
    """lo <= 0 <= hi: d squeezed into (lo, hi) smoothly (tanh on each side); a zero bound clamps to zero"""
    lo = np.broadcast_to(np.asarray(lo, np.float64), d.shape)
    hi = np.broadcast_to(np.asarray(hi, np.float64), d.shape)
    neg = np.where(lo < 0, lo * np.tanh(d / np.where(lo < 0, lo, -1.0)), 0.0)
    pos = np.where(hi > 0, hi * np.tanh(d / np.where(hi > 0, hi, 1.0)), 0.0)
    return np.where(d < 0, neg, pos)
