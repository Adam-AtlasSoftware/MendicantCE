"""Generator primitives for PRISM M3 (Covenant and Marines): skins, cloth weaves, webbing.

Same contract as gen.py: each takes a Ctx plus physical parameters in mm and returns
(height_mm, masks); every one tiles (integer counts on the tile, wrapped indices, periodic noise);
parameters finer than the resolution floor (4 shipped texels) are clamped up and noted.

  scales     imbricated reptile scales: jittered cells, each a dome leaning toward its trailing
             edge with a crevice at its border (Elite skin)
  wrinkles   a crease network: narrow valleys along the zero crossings of band-pass noise,
             optionally stretched along a direction (leathery Grunt skin, human skin's fine lines)
  pores      small pits scattered at a density per cm^2 (skin pores, follicles)
  woven      woven cloth: warp and weft threads crossing over and under in a plain or twill
             order, the threads wandering a little, varying in thickness (slubs) and carrying a
             fibre grain along their length, so a weave has no exact long-range period to beat
             against the pixels (BDU twill, webbing, Jackal coarse cloth)
  fibres     short fibre strokes at random angles (hair, stubble, feather barbs when angled)
"""
import math

import numpy as np

from . import core


def _worley_vec(n, cells_x, cells_y, rng, jitter=0.85):
    """Periodic Worley on a jittered grid: F1, F2 (in cell units), the offset (dx, dy) from the
    nearest feature point (cell units) and that point's cell id."""
    jit = 0.5 + (rng.random((cells_y, cells_x, 2)) - 0.5) * jitter
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    x *= cells_x / n
    y *= cells_y / n
    cx, cy = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    f1 = np.full((n, n), 1e18)
    f2 = np.full((n, n), 1e18)
    ox = np.zeros((n, n))
    oy = np.zeros((n, n))
    ids = np.zeros((n, n), np.int64)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            gx, gy = cx + dx, cy + dy
            mx, my = gx % cells_x, gy % cells_y
            j = jit[my, mx]
            ddx = x - gx - j[..., 0]
            ddy = y - gy - j[..., 1]
            d = ddx * ddx + ddy * ddy
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            ox = np.where(closer, ddx, ox)
            oy = np.where(closer, ddy, oy)
            ids = np.where(closer, my * cells_x + mx, ids)
            f1 = np.where(closer, d, f1)
    return np.sqrt(f1), np.sqrt(f2), ox, oy, ids


def scales(ctx, cell_mm, rms_mm, angle_deg=90.0, angle_var_deg=25.0, lean=0.55, crevice=0.22,
           height_var=0.25, jitter=0.85, floor=0.0):
    """Overlapping scales about cell_mm across. Each cell is a low dome that rises toward its
    trailing edge (direction angle_deg, varied per cell by angle_var_deg) and drops into a crevice at
    the border with its neighbours, which is how overlapping scales catch the light. floor (0..1)
    keeps that much of the scale's height at the crevice, so the scales stay rounded pebbles
    joined by shallow grooves rather than tiles with deep cracks between them."""
    cell_mm = ctx.clamp_min("scales cell", cell_mm, ctx.min_feat * 2)
    cells = max(4, int(round(ctx.tile_mm / cell_mm)))
    f1, f2, ox, oy, ids = _worley_vec(ctx.n, cells, cells, ctx.rng, jitter)
    k = cells * cells
    th = np.radians(angle_deg + ctx.rng.normal(0, angle_var_deg, k))
    hv = 1 + ctx.rng.uniform(-height_var, height_var, k)
    proj = ox * np.cos(th)[ids] + oy * np.sin(th)[ids]           # -0.5 (leading) .. +0.5 (trailing)
    dome = np.clip(1 - (f1 / 0.75) ** 2, 0, 1) ** 0.5
    body = (1 - lean) * dome + lean * np.clip(0.5 + proj, 0, 1.2) * dome
    edge = core.smoothstep(0.0, crevice, f2 - f1)                  # 0 at the border
    h = body * (floor + (1 - floor) * edge) * hv[ids]
    h = core.gauss_blur(h, ctx.px(ctx.min_feat) * 0.2)
    return rms_mm * core.unit(h), {"crevice": 1 - edge}


def wrinkles(ctx, rms_mm, wavelength_mm=(2.0, 6.0), width=0.3, angle_deg=None, stretch_mm=None):
    """Creases: valleys where band-pass noise crosses zero (width: of the valley, in the noise's
    units), so they join into a network as leathery skin does. With angle_deg and stretch_mm the
    noise is stretched along that direction (fine lines that run one way)."""
    lo = ctx.clamp_min("wrinkles wavelength_min", wavelength_mm[0], ctx.min_feat * 2)
    hi = max(wavelength_mm[1], lo * 1.3)
    if angle_deg is not None and stretch_mm:
        nz = core.aniso_noise(ctx.n, ctx.tile_mm, (lo, hi), stretch_mm, angle_deg, ctx.rng)
    else:
        nz = core.band_noise(ctx.n, ctx.tile_mm, lo, hi, ctx.rng)
    crease = np.exp(-(nz / width) ** 2)
    h = -crease
    h = core.gauss_blur(h, ctx.px(ctx.min_feat) * 0.2)
    return rms_mm * core.unit(h), {"crease": crease}


def pores(ctx, per_cm2, radius_mm=(0.12, 0.25), depth_mm=0.01, scales_n=3):
    """Pits of radius radius_mm scattered at per_cm2 a square centimetre, depths varied."""
    rmin = ctx.clamp_min("pores radius_min", radius_mm[0], ctx.min_feat / 2)
    rmax = max(radius_mm[1], rmin * 1.2)
    n = ctx.n
    kx, ky, _ = core.kgrid(n)
    f2 = (kx / n) ** 2 + (ky / n) ** 2
    count = per_cm2 * (ctx.tile_mm / 10.0) ** 2
    h = np.zeros((n, n))
    mask = np.zeros((n, n))
    for r in np.geomspace(rmin, rmax, scales_n):
        c = int(count / scales_n)
        imp = np.zeros((n, n))
        ys = ctx.rng.integers(0, n, c)
        xs = ctx.rng.integers(0, n, c)
        np.add.at(imp, (ys, xs), ctx.rng.uniform(0.4, 1.0, c))
        sp = ctx.px(r) / 1.5
        g = np.real(np.fft.ifft2(np.fft.fft2(imp) * np.exp(-2 * math.pi ** 2 * sp ** 2 * f2)))
        g /= max(g.max(), 1e-12)
        h -= g
        mask = np.maximum(mask, np.clip(g * 2, 0, 1))
    return depth_mm * h / max(np.abs(h).max(), 1e-12), {"pore": mask}


def _state_along(prev, here, nxt, f, soft):
    """a thread's over/under state (0/1 per crossing) as a smooth function of its position f in the
    crossing's cell: the crossing's state in the middle, easing to the neighbours' at the cell's ends"""
    a = core.smoothstep(0.0, soft, f)
    b = core.smoothstep(1.0 - soft, 1.0, f)
    left = 0.5 * (prev + here)
    right = 0.5 * (here + nxt)
    return np.where(f < 0.5, left + (here - left) * a, here + (right - here) * b)


def woven(ctx, warp, weft, amp_mm, pattern="plain", float_over=2, wander_mm=0.05, wander_wl_mm=(3.0, 12.0),
          slub=0.2, slub_wl_mm=(2.0, 8.0), fibre=0.12, fibre_mm=(0.1, 0.3), crossing_soft=0.35,
          thread_power=0.7, lift=0.8):
    """Woven cloth. warp threads run along y (warp of them across x), weft along x (weft of them
    down y). pattern "plain" (1 over 1 under) or "twill" (float_over over, 1 under, stepped by one
    each thread: the diagonal wale). The threads wander by wander_mm (smooth noise), swell and
    thin by slub (a fraction of their thickness, along their length), and carry a fibre grain
    (fibre: of the height, wavelength fibre_mm across, long along the thread)."""
    period = 2 if pattern == "plain" else float_over + 1
    if warp % period or weft % period:
        raise ValueError("woven counts must be multiples of the pattern period (%d) to tile" % period)
    ctx.clamp_min("woven warp pitch", ctx.tile_mm / warp, ctx.min_feat)
    ctx.clamp_min("woven weft pitch", ctx.tile_mm / weft, ctx.min_feat)
    n = ctx.n
    y, x = np.mgrid[0:n, 0:n] / n
    wx = core.band_noise(n, ctx.tile_mm, wander_wl_mm[0], wander_wl_mm[1], ctx.rng) * (wander_mm / ctx.tile_mm)
    wy = core.band_noise(n, ctx.tile_mm, wander_wl_mm[0], wander_wl_mm[1], ctx.rng) * (wander_mm / ctx.tile_mm)
    u, v = (x + wx) * warp, (y + wy) * weft
    i = np.floor(u).astype(np.int64)
    j = np.floor(v).astype(np.int64)
    fu, fv = u - i, v - j
    # the over/under table: st[i, j] = 1 where warp thread i is over weft thread j
    ii, jj = np.meshgrid(np.arange(warp), np.arange(weft), indexing="ij")
    if pattern == "plain":
        st = ((ii + jj) % 2 == 0).astype(np.float64)
    else:
        st = (((jj - ii) % period) < float_over).astype(np.float64)
    # warp thread i: its state along y (by weft row); weft thread j: the complement along x
    iw, jw = i % warp, j % weft
    warp_state = _state_along(st[iw, (j - 1) % weft], st[iw, jw], st[iw, (j + 1) % weft], fv, crossing_soft)
    weft_state = _state_along(1 - st[(i - 1) % warp, jw], 1 - st[iw, jw], 1 - st[(i + 1) % warp, jw], fu, crossing_soft)
    sl_w = 1 + slub * core.band_noise(n, ctx.tile_mm, slub_wl_mm[0], slub_wl_mm[1], ctx.rng)
    sl_f = 1 + slub * core.band_noise(n, ctx.tile_mm, slub_wl_mm[0], slub_wl_mm[1], ctx.rng)
    prof_w = np.sin(math.pi * np.clip(fu, 0, 1)) ** thread_power
    prof_f = np.sin(math.pi * np.clip(fv, 0, 1)) ** thread_power
    hw = prof_w * sl_w * ((1 - lift) + lift * warp_state)
    hf = prof_f * sl_f * ((1 - lift) + lift * weft_state)
    top_warp = hw >= hf
    h = np.maximum(hw, hf)
    if fibre > 0:
        lo = max(fibre_mm[0], ctx.min_feat)
        hi = max(fibre_mm[1], lo * 1.3)
        g_w = core.aniso_noise(n, ctx.tile_mm, (lo, hi), 4 * ctx.tile_mm / weft, 90.0, ctx.rng)
        g_f = core.aniso_noise(n, ctx.tile_mm, (lo, hi), 4 * ctx.tile_mm / warp, 0.0, ctx.rng)
        h = h + fibre * np.where(top_warp, g_w, g_f) * h
    h = core.gauss_blur(h, 0.6)
    return amp_mm * h, {"weave_gap": 1 - core.smoothstep(0.1, 0.5, h / max(h.max(), 1e-9))}


def fibres(ctx, rms_mm, count_per_cm2, length_mm=(1.0, 3.0), width_mm=0.08, angle_deg=None, spread_deg=180.0):
    """Short strokes (hair, stubble, feather barbs): count_per_cm2 strokes of length_mm, raised
    ridges width_mm wide, at angle_deg +- spread_deg/2 (random when angle_deg is None)."""
    n = ctx.n
    kx, ky, _ = core.kgrid(n)
    f2 = (kx / n) ** 2 + (ky / n) ** 2
    sigma = ctx.clamp_min("fibres width/2.5", width_mm / 2.5, 1.6 * ctx.ship_texel)
    sp = ctx.px(sigma)
    rho = np.zeros((n, n))
    count = int(count_per_cm2 * (ctx.tile_mm / 10.0) ** 2)
    ds = 0.5
    xs_all, ys_all, ws_all = [], [], []
    for _ in range(count):
        L = math.exp(ctx.rng.uniform(math.log(length_mm[0]), math.log(length_mm[1])))
        base = ctx.rng.uniform(0, 180) if angle_deg is None else angle_deg
        th = math.radians(base + ctx.rng.uniform(-0.5, 0.5) * (spread_deg if angle_deg is not None else 0))
        m = max(2, int(ctx.px(L) / ds))
        s = np.linspace(0, L, m)
        x0, y0 = ctx.rng.uniform(0, ctx.tile_mm, 2)
        curve = ctx.rng.uniform(-0.04, 0.04)
        hd = th + curve * s
        px = x0 + np.cumsum(np.cos(hd)) * (L / m)
        py = y0 + np.cumsum(np.sin(hd)) * (L / m)
        env = np.clip(np.sin(math.pi * s / L), 0, 1) ** 0.5 * ctx.rng.uniform(0.5, 1.0)
        xs_all.append(px / ctx.texel)
        ys_all.append(py / ctx.texel)
        ws_all.append(env * ds)
    if xs_all:
        px, py, w = np.concatenate(xs_all), np.concatenate(ys_all), np.concatenate(ws_all)
        x0, y0 = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
        fx, fy = px - x0, py - y0
        for ox, oy, ww in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
            np.add.at(rho, ((y0 + oy) % n, (x0 + ox) % n), w * ww)
    h = np.real(np.fft.ifft2(np.fft.fft2(rho) * np.exp(-2 * math.pi ** 2 * sp ** 2 * f2)))
    h = np.tanh(h / max(np.percentile(h, 99.5), 1e-12))      # overlapping strokes do not pile up
    return rms_mm * core.unit(h), {"fibre": np.clip(h, 0, 1)}


GENERATORS = {
    "scales": scales,
    "wrinkles": wrinkles,
    "pores": pores,
    "woven": woven,
    "fibres": fibres,
}
