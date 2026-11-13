"""FFT and noise helpers. Frequencies are integer cycles per tile, so every result tiles."""
import functools
import math

import numpy as np


@functools.lru_cache(maxsize=8)
def _kgrid(n):
    f = np.fft.fftfreq(n) * n
    kx, ky = np.meshgrid(f, f)          # kx varies along columns (x), ky along rows (y)
    kx.setflags(write=False)
    ky.setflags(write=False)
    kr = np.sqrt(kx ** 2 + ky ** 2)
    kr.setflags(write=False)
    return kx, ky, kr


def kgrid(n):
    return _kgrid(n)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0 + 1e-12), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def log_band(k, kmin, kmax, soft=0.35):
    """1 inside [kmin, kmax], raised-cosine roll-off over a factor (1+soft) in log k."""
    lk = np.log(np.maximum(k, 1e-9))
    lo, hi, w = math.log(kmin), math.log(kmax), math.log(1 + soft)
    a = smoothstep(lo - w, lo, lk)
    b = 1 - smoothstep(hi, hi + w, lk)
    return a * b


def unit(a):
    a = a - a.mean()
    s = a.std()
    return a / s if s > 0 else a


def band_noise(n, tile_mm, lam_min_mm, lam_max_mm, rng, soft=0.35):
    """Isotropic band-pass noise, wavelengths [lam_min, lam_max] mm, unit RMS, zero mean."""
    _, _, kr = kgrid(n)
    filt = log_band(kr, tile_mm / lam_max_mm, tile_mm / lam_min_mm, soft)
    spec = np.fft.fft2(rng.standard_normal((n, n))) * filt
    return unit(np.real(np.fft.ifft2(spec)))


def aniso_noise(n, tile_mm, across_mm, streak_mm, angle_deg, rng, soft=0.35):
    """Streaks along `angle_deg` (0 = +x): band [across_mm] across, length ~streak_mm along."""
    kx, ky, _ = kgrid(n)
    t = math.radians(angle_deg)
    k_al = kx * math.cos(t) + ky * math.sin(t)      # frequency along the streak direction
    k_ac = -kx * math.sin(t) + ky * math.cos(t)     # frequency across
    filt = log_band(np.abs(k_ac), tile_mm / across_mm[1], tile_mm / across_mm[0], soft)
    kc = tile_mm / streak_mm
    filt = filt * np.exp(-(k_al / kc) ** 2)
    spec = np.fft.fft2(rng.standard_normal((n, n))) * filt
    return unit(np.real(np.fft.ifft2(spec)))


def gauss_blur(a, sigma_px):
    if sigma_px <= 0:
        return a
    kx, ky, _ = kgrid(a.shape[0])
    n = a.shape[0]
    g = np.exp(-2 * math.pi ** 2 * sigma_px ** 2 * ((kx / n) ** 2 + (ky / n) ** 2))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * g))


def highpass(a, kcut=4.0, taper=2.0):
    """Remove every component below kcut cycles/tile (hard zero), soft ramp to kcut+taper."""
    _, _, kr = kgrid(a.shape[0])
    w = smoothstep(kcut, kcut + taper, kr) * (kr >= kcut)
    return np.real(np.fft.ifft2(np.fft.fft2(a) * w))


def lowfreq_fraction(a, kcut=4.0):
    """Fraction of the (zero-mean) variance at 0 < k < kcut cycles/tile."""
    _, _, kr = kgrid(a.shape[0])
    p = np.abs(np.fft.fft2(a - a.mean())) ** 2
    tot = p.sum()
    return float(p[(kr > 0) & (kr < kcut)].sum() / tot) if tot > 0 else 0.0


def fine_fraction(a, k_min):
    """Fraction of the variance at k > k_min cycles/tile."""
    _, _, kr = kgrid(a.shape[0])
    p = np.abs(np.fft.fft2(a - a.mean())) ** 2
    tot = p.sum()
    return float(p[kr > k_min].sum() / tot) if tot > 0 else 0.0


def median_wavelength_mm(a, tile_mm):
    """Power-weighted median wavelength of a field (mm)."""
    _, _, kr = kgrid(a.shape[0])
    p = (np.abs(np.fft.fft2(a - a.mean())) ** 2).ravel()
    k = kr.ravel()
    m = k > 0
    p, k = p[m], k[m]
    o = np.argsort(k)
    c = np.cumsum(p[o])
    i = np.searchsorted(c, 0.5 * c[-1])
    return float(tile_mm / k[o][min(i, len(o) - 1)])


def down2(a):
    n = a.shape[0] // 2
    if a.ndim == 2:
        return a.reshape(n, 2, n, 2).mean((1, 3))
    return a.reshape(n, 2, n, 2, a.shape[2]).mean((1, 3))


def worley(n, cells_x, cells_y, rng, jitter=1.0, aspect_mm=(1.0, 1.0)):
    """Periodic Worley F1/F2 on a jittered cells_x x cells_y grid.

    Distances are in mm-proportional units (cell size scaled by aspect_mm = (cw, ch)).
    Returns (F1, F2, id_of_nearest) with F in the same units as aspect_mm.
    """
    jit = 0.5 + (rng.random((cells_y, cells_x, 2)) - 0.5) * jitter
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    x *= cells_x / n
    y *= cells_y / n
    cx, cy = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    f1 = np.full((n, n), 1e18)
    f2 = np.full((n, n), 1e18)
    ids = np.zeros((n, n), np.int64)
    cw, ch = aspect_mm
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            gx, gy = cx + dx, cy + dy
            mx, my = gx % cells_x, gy % cells_y
            j = jit[my, mx]
            d = ((x - gx - j[..., 0]) * cw) ** 2 + ((y - gy - j[..., 1]) * ch) ** 2
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            ids = np.where(closer, my * cells_x + mx, ids)
            f1 = np.where(closer, d, f1)
    return np.sqrt(f1), np.sqrt(f2), ids
