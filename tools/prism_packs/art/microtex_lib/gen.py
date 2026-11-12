"""Generator primitives. Each takes a Ctx plus physical parameters in mm and returns
(height_mm, masks) where masks is a dict of named [0,1] fields (for roughness layers and
optional extra channels). Every primitive tiles: integer frequencies or wrapped indices.

Resolution rule: the smallest feature spans at least 4 texels at the shipped
size. Parameters finer than that are clamped up to it and the clamp is noted in ctx.notes.
"""
import math

import numpy as np

from . import core


class Ctx:
    def __init__(self, n, tile_mm, ship_px, rng):
        self.n = n
        self.tile_mm = float(tile_mm)
        self.ship_px = ship_px
        self.rng = rng
        self.texel = self.tile_mm / n
        self.ship_texel = self.tile_mm / ship_px
        self.min_feat = 4.0 * self.ship_texel
        self.notes = []

    def clamp_min(self, what, v, lo):
        if v < lo:
            self.notes.append(f"{what}: {v:.3f} mm clamped to {lo:.3f} mm (resolution floor at the shipped size)")
            return lo
        return v

    def px(self, mm):
        return mm / self.texel


def _wrapped_dist_px(n):
    i = np.arange(n)
    d = np.minimum(i, n - i).astype(np.float64)
    return np.sqrt(d[None, :] ** 2 + d[:, None] ** 2)


def _conv_kernel(field, kernel_img):
    return np.real(np.fft.ifft2(np.fft.fft2(field) * np.fft.fft2(kernel_img)))


# 4. ceramic / paint orange peel ------------------------------------------------------------
def orange_peel(ctx, rms_mm, wavelength_mm=(0.5, 2.0), cell_mix=0.35):
    lo = ctx.clamp_min("orange_peel wavelength_min", wavelength_mm[0], ctx.min_feat * 1.25)
    hi = max(wavelength_mm[1], lo * 1.5)
    a = core.band_noise(ctx.n, ctx.tile_mm, lo, hi, ctx.rng)
    cells = max(4, int(round(ctx.tile_mm / math.sqrt(lo * hi))))
    f1, _, _ = core.worley(ctx.n, cells, cells, ctx.rng, jitter=0.9)
    peel = core.unit(-(f1 ** 2))                       # soft dimpled cells
    peel = core.unit(core.gauss_blur(peel, ctx.px(lo) * 0.18))
    h = core.unit((1 - cell_mix) * a + cell_mix * peel)
    return rms_mm * h, {}


# 9. bead-blast / cast grain ----------------------------------------------------------------
def bead_blast(ctx, rms_mm, crater_mm=(0.2, 0.5), coverage=1.6, scales=3):
    dmin = ctx.clamp_min("bead_blast crater_min", crater_mm[0], ctx.min_feat)
    dmax = max(crater_mm[1], dmin * 1.2)
    n = ctx.n
    dist = _wrapped_dist_px(n)
    h = np.zeros((n, n))
    area = ctx.tile_mm ** 2
    for d in np.geomspace(dmin, dmax, scales):
        r = d / 2
        count = int(coverage * area / (math.pi * r * r) / scales)
        imp = np.zeros((n, n))
        ys = ctx.rng.integers(0, n, count)
        xs = ctx.rng.integers(0, n, count)
        np.add.at(imp, (ys, xs), ctx.rng.uniform(0.5, 1.0, count))
        rp = ctx.px(r)
        s = np.clip(dist / rp, 0, 1)
        cap = -(1 - s ** 2) ** 1.5                     # spherical-ish dimple, zero slope rim
        h += _conv_kernel(imp, cap)
    return rms_mm * core.unit(h), {}


# 5. brushed / machined ---------------------------------------------------------------------
def brushed(ctx, rms_mm, across_mm=(0.2, 0.5), streak_mm=20.0, angle_deg=0.0):
    lo = ctx.clamp_min("brushed across_min", across_mm[0], ctx.min_feat)
    hi = max(across_mm[1], lo * 1.3)
    a = core.aniso_noise(ctx.n, ctx.tile_mm, (lo, hi), streak_mm, angle_deg, ctx.rng)
    return rms_mm * a, {}


# 6. multi-scale scratches (raised lips, roughness mask) -------------------------------------
def scratches(ctx, scales):
    n = ctx.n
    kx, ky, _ = core.kgrid(n)
    f2 = (kx / n) ** 2 + (ky / n) ** 2
    h = np.zeros((n, n))
    mask = np.zeros((n, n))
    for si, sc in enumerate(scales):
        sigma_mm = ctx.clamp_min(f"scratches[{si}] width/2.5", sc["width_mm"] / 2.5, 1.6 * ctx.ship_texel)
        sp = ctx.px(sigma_mm)
        rho = np.zeros((n, n))
        ds = 0.5                                       # px step along the scratch
        pts_y, pts_x, pts_w = [], [], []
        for _ in range(int(sc["count"])):
            L = math.exp(ctx.rng.uniform(math.log(sc["length_mm"][0]), math.log(sc["length_mm"][1])))
            if sc.get("angle_deg") is None:
                th = ctx.rng.uniform(0, math.pi)
            else:
                th = math.radians(sc["angle_deg"] + ctx.rng.normal(0, sc.get("spread_deg", 10)))
            kappa = ctx.rng.uniform(-1, 1) * sc.get("curvature_per_mm", 0.0)
            depth = sc["depth_mm"] * ctx.rng.uniform(0.5, 1.0)
            m = max(2, int(ctx.px(L) / ds))
            s = np.linspace(0, L, m)
            heading = th + kappa * s
            dx = np.cumsum(np.cos(heading)) * (L / m)
            dy = np.cumsum(np.sin(heading)) * (L / m)
            x0, y0 = ctx.rng.uniform(0, ctx.tile_mm, 2)
            t = s / L
            env = np.sin(math.pi * t) ** 0.6 * (0.75 + 0.25 * np.sin(2 * math.pi * (t * ctx.rng.uniform(1, 4) + ctx.rng.random())))
            pts_x.append((x0 + dx) / ctx.texel)
            pts_y.append((y0 + dy) / ctx.texel)
            pts_w.append(depth * env * ds)
        if pts_x:
            px = np.concatenate(pts_x)
            py = np.concatenate(pts_y)
            w = np.concatenate(pts_w)
            x0 = np.floor(px).astype(np.int64)
            y0 = np.floor(py).astype(np.int64)
            fx, fy = px - x0, py - y0
            for ox, oy, ww in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                               (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
                np.add.at(rho, ((y0 + oy) % n, (x0 + ox) % n), w * ww)
        F = np.fft.fft2(rho)
        g1 = np.exp(-2 * math.pi ** 2 * sp ** 2 * f2) * math.sqrt(2 * math.pi) * sp
        sl = 2.5 * sp
        g2 = np.exp(-2 * math.pi ** 2 * sl ** 2 * f2) * math.sqrt(2 * math.pi) * sl
        groove = np.real(np.fft.ifft2(F * g1))
        lip = np.real(np.fft.ifft2(F * g2))
        h += -groove + sc.get("lip", 0.3) * lip
        gm = np.real(np.fft.ifft2(F * np.exp(-2 * math.pi ** 2 * (1.5 * sp) ** 2 * f2) * math.sqrt(2 * math.pi) * 1.5 * sp))
        mask = np.maximum(mask, np.clip(gm / sc["depth_mm"], 0, 1))
    return h, {"scratch": mask}


# 7. fine paint chips (step-down in coating, no raised rim) ----------------------------------
def chips(ctx, count, radius_mm=(0.25, 1.2), depth_mm=0.03, wall_mm=0.1, irregular=0.35):
    n = ctx.n
    rmin = ctx.clamp_min("chips radius_min", radius_mm[0], ctx.min_feat)
    rmax = max(radius_mm[1], rmin * 1.5)
    wall = ctx.clamp_min("chips wall", wall_mm, 2 * ctx.ship_texel)
    nf = core.band_noise(n, ctx.tile_mm, max(rmin, 2 * ctx.min_feat), max(rmax * 2, 3 * ctx.min_feat), ctx.rng)
    mask = np.zeros((n, n))
    for _ in range(int(count)):
        r = math.exp(ctx.rng.uniform(math.log(rmin), math.log(rmax)))
        cx, cy = ctx.rng.uniform(0, n, 2)
        half = int(math.ceil(1.7 * r / ctx.texel)) + 2
        xs = np.arange(int(cx) - half, int(cx) + half + 1)
        ys = np.arange(int(cy) - half, int(cy) + half + 1)
        X, Y = np.meshgrid(xs, ys)
        d = np.sqrt((X - cx) ** 2 + (Y - cy) ** 2) * ctx.texel / r
        iy, ix = np.ix_(ys % n, xs % n)
        val = 1 - d + irregular * nf[iy, ix]
        m = core.smoothstep(0, wall / r, val) * (1 - core.smoothstep(1.0, 1.3, d))   # no satellites
        mask[iy, ix] = np.maximum(mask[iy, ix], m)
    mask = np.clip(core.gauss_blur(mask, ctx.px(wall) / 3), 0, 1)   # band-limit the walls
    return -depth_mm * mask, {"chip": mask}


# 3. grip pads: hex dot lattice --------------------------------------------------------------
def hex_dots(ctx, cols, rows, dot_diam_mm, height_mm, profile=1.5, jitter_mm=0.0, height_var=0.05):
    if rows % 2:
        raise ValueError("hex_dots rows must be even to tile")
    n = ctx.n
    pxmm, pymm = ctx.tile_mm / cols, ctx.tile_mm / rows
    R = ctx.clamp_min("hex_dots dot_diam", dot_diam_mm, ctx.min_feat) / 2
    jit = ctx.rng.normal(0, 1, (rows, cols, 2)) * np.array([jitter_mm / pxmm, jitter_mm / pymm])
    hv = 1 + ctx.rng.uniform(-height_var, height_var, (rows, cols))
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    u, v = x * cols / n, y * rows / n
    best = np.full((n, n), 1e9)
    bh = np.ones((n, n))
    for dj in (-1, 0, 1):
        j = np.floor(v).astype(np.int64) + dj
        o = 0.5 * (j % 2)
        for di in (-1, 0, 1):
            i = np.floor(u - o).astype(np.int64) + di
            jm, im = j % rows, i % cols
            cu = i + o + jit[jm, im, 0]
            cv = j + 0.5 + jit[jm, im, 1]
            d = np.sqrt(((u - cu) * pxmm) ** 2 + ((v - cv) * pymm) ** 2)
            c = d < best
            best = np.where(c, d, best)
            bh = np.where(c, hv[jm, im], bh)
    s = np.clip(best / R, 0, 1)
    dome = (1 - s ** 2) ** profile
    return height_mm * dome * bh, {"dot": dome}


# stipple (rubberised polymer grip): random domed bumps on a jittered grid -------------------
def stipple(ctx, cell_mm, dome_frac=0.8, height_mm=0.08, size_var=0.3, jitter=0.8, profile=1.5):
    """Domes on a jittered grid; each pixel takes the max over the 3x3 neighbouring domes, so
    overlapping domes merge smoothly instead of being cut at Voronoi borders."""
    cell_mm = ctx.clamp_min("stipple cell", cell_mm, ctx.min_feat * 1.5)
    cells = max(4, int(round(ctx.tile_mm / cell_mm)))
    cw = ctx.tile_mm / cells
    n = ctx.n
    jit = 0.5 + (ctx.rng.random((cells, cells, 2)) - 0.5) * jitter
    rad = dome_frac * cw / 2 * (1 + size_var * ctx.rng.uniform(-1, 1, (cells, cells)))
    rad = np.maximum(rad, ctx.min_feat / 2)
    y, x = np.mgrid[0:n, 0:n].astype(np.float64) * (cells / n)
    cx, cy = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    dome = np.zeros((n, n))
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            gx, gy = cx + dx, cy + dy
            mx, my = gx % cells, gy % cells
            j = jit[my, mx]
            d = np.sqrt((x - gx - j[..., 0]) ** 2 + (y - gy - j[..., 1]) ** 2) * cw
            s = np.clip(d / rad[my, mx], 0, 1)
            dome = np.maximum(dome, (1 - s ** 2) ** profile)
    return height_mm * dome, {"dot": dome}


# 8. pebble / leather grain / scale cells ----------------------------------------------------
def pebble(ctx, cell_mm, rms_mm, mode="dome"):
    cell_mm = ctx.clamp_min(f"pebble({mode}) cell", cell_mm, ctx.min_feat)
    cells = max(4, int(round(ctx.tile_mm / cell_mm)))
    cw = ctx.tile_mm / cells
    f1, f2, _ = core.worley(ctx.n, cells, cells, ctx.rng, jitter=0.9, aspect_mm=(cw, cw))
    if mode == "dome":
        h = np.clip(1 - (f1 / (0.62 * cw)) ** 2, 0, 1) ** 0.6
    elif mode == "leather":                            # crevices along the cell borders
        h = core.smoothstep(0.0, 0.35 * cw, f2 - f1)
    else:
        raise ValueError(mode)
    h = core.gauss_blur(h, ctx.px(ctx.min_feat) * 0.15)
    return rms_mm * core.unit(h), {"pebble": core.smoothstep(-1, 2, core.unit(h))}


# 2. rib / knit (whole number of ribs per tile) ----------------------------------------------
def rib_knit(ctx, ribs, amp_mm, power=0.7, axis="y"):
    n = ctx.n
    y, x = np.mgrid[0:n, 0:n] / n
    t = y if axis == "y" else x
    pitch = ctx.tile_mm / ribs
    ctx.clamp_min("rib_knit pitch", pitch, ctx.min_feat)
    rib = (0.5 + 0.5 * np.cos(2 * math.pi * ribs * t)) ** power
    return amp_mm * rib, {"rib_groove": 1 - rib}


# 2b. knit rib: wales of stitches, irregular (no long-range period to beat against the pixels) ----
def knit_rib(ctx, ribs, stitches, amp_mm, power=0.8, axis="y", wander_mm=0.35, wander_wl_mm=(6.0, 20.0),
             amp_var=0.25, stitch_depth=0.35, slant=0.6, stitch_jitter=0.5):
    """`ribs` rib rows a tile (whole number: tiles) along `axis`, each a chain of `stitches` V-shaped
    knit stitches a tile across it. The rows wander (a smooth noise displacement of wander_mm), each
    row's height varies (amp_var) and each row's stitches have their own phase (stitch_jitter of a
    stitch), so the lattice has no exact period beyond one rib: at a few pixels a rib it averages into
    an even grain instead of a moire."""
    n = ctx.n
    y, x = np.mgrid[0:n, 0:n] / n
    t, u = (y, x) if axis == "y" else (x, y)
    pitch = ctx.tile_mm / ribs
    ctx.clamp_min("knit_rib pitch", pitch, ctx.min_feat)
    ctx.clamp_min("knit_rib stitch pitch", ctx.tile_mm / stitches, ctx.min_feat)
    w = core.band_noise(n, ctx.tile_mm, wander_wl_mm[0], wander_wl_mm[1], ctx.rng) * (wander_mm / ctx.tile_mm)
    phase = ribs * (t + w)
    row = np.floor(phase).astype(np.int64) % ribs
    f = phase - np.floor(phase)
    row_amp = 1 + amp_var * ctx.rng.uniform(-1, 1, ribs)
    row_shift = ctx.rng.uniform(0, 1, ribs) * stitch_jitter
    rib = (0.5 - 0.5 * np.cos(2 * math.pi * f)) ** power           # 0 at the grooves, 1 mid-row
    s = stitches * u + row_shift[row] + slant * np.abs(f - 0.5)     # the V: the two legs lean apart
    stitch = 0.5 + 0.5 * np.cos(2 * math.pi * s)
    along = 1 + 0.5 * amp_var * core.band_noise(n, ctx.tile_mm, 2 * pitch, 6 * pitch, ctx.rng)
    h = rib * (1 - stitch_depth + stitch_depth * stitch) * row_amp[row] * along
    h = core.gauss_blur(h, 0.6)
    return amp_mm * h, {"rib_groove": 1 - rib}


# 1. weave: over/under threads ---------------------------------------------------------------
def weave(ctx, warp, weft, amp_mm, pattern="plain"):
    """warp threads run along y (count across x), weft along x (count along y)."""
    per = {"plain": 2, "twill": 3}[pattern]
    if warp % per or weft % per:
        raise ValueError("weave counts must be multiples of the pattern period to tile")
    ctx.clamp_min("weave warp pitch", ctx.tile_mm / warp, ctx.min_feat)
    ctx.clamp_min("weave weft pitch", ctx.tile_mm / weft, ctx.min_feat)
    n = ctx.n
    y, x = np.mgrid[0:n, 0:n] / n
    fx, fy = (x * warp) % 1.0, (y * weft) % 1.0
    i, j = np.floor(x * warp), np.floor(y * weft)
    prof_x = np.sin(math.pi * fx) ** 0.8               # warp thread cross-section
    prof_y = np.sin(math.pi * fy) ** 0.8
    if pattern == "plain":
        s = np.cos(math.pi * (y * weft) + math.pi * i)     # warp over/under along y
    else:
        s = np.cos(2 * math.pi * (y * weft + i) / 3)
    hw = prof_x * (0.55 + 0.45 * s)
    hf = prof_y * (0.55 - 0.45 * s)
    h = np.maximum(hw, hf)
    h = core.gauss_blur(h, 0.6)
    return amp_mm * h, {"weave_gap": 1 - core.smoothstep(0.1, 0.5, h)}


# anodised alloy grain: very fine isotropic grain with a faint crystalline cell ---------------
def anodised_grain(ctx, rms_mm, grain_mm=(0.3, 0.8), cell_mix=0.4):
    lo = ctx.clamp_min("anodised_grain min", grain_mm[0], ctx.min_feat)
    hi = max(grain_mm[1], lo * 1.5)
    a = core.band_noise(ctx.n, ctx.tile_mm, lo, hi, ctx.rng)
    cells = max(4, int(round(ctx.tile_mm / hi)))
    f1, f2, _ = core.worley(ctx.n, cells, cells, ctx.rng, jitter=0.9)
    c = core.unit(core.gauss_blur(core.smoothstep(0, 0.25, f2 - f1), ctx.px(lo) * 0.3))
    return rms_mm * core.unit((1 - cell_mix) * a + cell_mix * c), {}


GENERATORS = {
    "orange_peel": orange_peel,
    "bead_blast": bead_blast,
    "brushed": brushed,
    "scratches": scratches,
    "chips": chips,
    "hex_dots": hex_dots,
    "stipple": stipple,
    "pebble": pebble,
    "rib_knit": rib_knit,
    "knit_rib": knit_rib,
    "weave": weave,
    "anodised_grain": anodised_grain,
}
# 10. dust is roughness-only: see maps.rough_layers ("noise" layer with a wavelength band).

# PRISM M3 (Covenant and Marines): skins, weaves, webbing, fibres
from .gen_organic import GENERATORS as _ORGANIC  # noqa: E402
GENERATORS.update(_ORGANIC)
