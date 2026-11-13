"""Invented-texture check: structure in an upscaled CE map that CE does not have and the earlier style
checks do not see (they look at colour, the DXT grid and new strong edges more than one texel from CE's
edges). Five measures, each computed against CE itself (and a plain bicubic resample of CE where a
baseline is needed), never against another method's output:

  canvas    excess energy above CE's Nyquist where CE has little texture: per texel, the rms of the result
            above CE's Nyquist (e_hi) against CE's own mid-band energy (DoG 0.7..1.4 texels, e_mid). A
            texel fails when e_hi > CANVAS_A * e_mid + CANVAS_B. Reported as the failing share of CE's
            low-texture texels (flat strips and smooth gradients).
  period    periodic peaks in the new band: per 32-texel tile, the power spectrum of the band above CE's
            Nyquist; a local maximum PERIOD_RATIO x above the radial median at its radius, holding at least
            PERIOD_SHARE of the annulus power, that is not a harmonic of a peak in CE's own tile spectrum
            (sharp periodic CE patterns, the Elite's hex suit, have legitimate harmonics up there).
  contour   doubled contours and new closed contours (rivets, ghost lines): strong gradient ridges of the
            result (NMS, above CONTOUR_REL of the local maximum) counted within 1.25 texels of CE's own
            ridges, per 8-texel tile, divided by CE's ridge length there. A tile fails above CONTOUR_MULT.
            Plus strong ridges in low-texture regions further than 1.5 texels from any CE ridge.
  banding   terraced gradients: in CE's low-texture gradient regions, the share of pixels whose fine-scale
            gradient (sigma F/4) is below 0.3 of the coarse gradient (sigma F) -- plateaus between steps --
            minus the same share for the plain resample.
  speckle   bright specks in dark regions: white top-hat of the result (opening with a disk one texel
            across) above max(SPECK_ABS, SPECK_REL x opened level) inside CE's dark regions, counted as
            connected components per 1000 dark texels, minus the plain resample's count.
  decal     glyph strokes in small lit decals: inside the keep-CE-shape mask (small bright compact CE
            components, plus self-illumination where a multipurpose map is given), the rms of the result
            minus the plain resample (8-bit levels).

  invented_check.py calibrate      run on the outputs saved before the B2 fix and print the table
  from invented_check import check, keep_mask

Thresholds were calibrated on those outputs: every failure named in the visual review fails and the
fusion results the review judged clean pass (see calibrate()).
"""
import json
import math
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texlib as T  # noqa: E402

# calibrated thresholds (see calibrate())
E_LOW = 2.0          # CE mid-band energy (8-bit levels) below which a texel counts as low-texture
G_LOW = 1.2          # CE low-band gradient (levels per texel) above which low-texture is a gradient
EXCESS_A, EXCESS_B = 1.0, 2.5
WEAVE_RATIO = 3.0
PERIOD_RATIO, PERIOD_SHARE = 40.0, 0.04
CONTRAST_MIN = 12.0  # CE contrast (levels over 1 texel) for an edge to count
CONTOUR_MULT = 1.6
BAND_TILE = 0.12     # posterisation index above the plain resample's, per tile
RAMP_K = 0.5  # ramp: CE mid-band energy below max(E_LOW, RAMP_K * gradient)
DARK = 0.12          # CE luminance (low band) below which a region is dark
SPECK_ABS, SPECK_REL = 0.05, 0.6
GATES = {"excess": 0.10, "weave": 0.10, "period": 0.05, "contour": 0.15, "banding": 0.5,
         "speckle": 40.0, "decal": 3.5}


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def plain(ce, F, wrap):
    """Plain resample: bicubic, wrap-padded for tiling maps."""
    h, w = ce.shape[:2]
    if not wrap:
        return np.clip(cv2.resize(ce, (w * F, h * F), interpolation=cv2.INTER_CUBIC), 0, 1)
    p = 3
    a = T.pad_wrap(ce, p)
    u = cv2.resize(a, (a.shape[1] * F, a.shape[0] * F), interpolation=cv2.INTER_CUBIC)
    return np.clip(u[p * F:p * F + h * F, p * F:p * F + w * F], 0, 1)


def regions(ce, wrap):
    """CE-resolution maps: mid-band energy (levels), low-band gradient (levels/texel), low-band luminance."""
    L = T.lum(ce)
    b = T.gblur(L, 0.7, wrap) - T.gblur(L, 1.4, wrap)
    emid = np.sqrt(T.gblur(b * b, 1.5, wrap)) * 255
    Ls = T.gblur(L, 1.5, wrap)
    gx = cv2.Sobel(Ls, cv2.CV_32F, 1, 0, ksize=3, scale=1 / 8)
    gy = cv2.Sobel(Ls, cv2.CV_32F, 0, 1, ksize=3, scale=1 / 8)
    return emid, np.hypot(gx, gy) * 255, T.gblur(L, 1.0, wrap)


def keep_mask(ce, wrap, mp=None, decals=True):
    """Keep-CE-shape mask (CE resolution, soft 0..1): small lit decals (bright compact components found by a
    white top-hat 7 texels across: the a50 trim's lit slots, the Elite's glyph lights) and, with a
    multipurpose map (Xbox order: G = self-illumination), the self-illuminated texels. These get a plain
    resample: every ML model draws glyph-like strokes into a 2x5-texel lit blob at 16x."""
    L = T.lum(ce)
    keep = np.zeros(L.shape, np.float32)
    if not decals:  # environment detail maps (grass, rock): every blade tip is an isolated bright island
        if mp is not None:
            keep = (mp[..., 1] > 0.1).astype(np.float32)
        return np.clip(T.gblur(cv2.dilate(keep, np.ones((3, 3), np.uint8)), 0.7, wrap) * 1.5, 0, 1)
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
    pad = 8
    Lp = np.pad(L, pad, mode="wrap" if wrap else "reflect")
    th = (Lp - cv2.morphologyEx(Lp, cv2.MORPH_OPEN, k))[pad:-pad, pad:-pad]
    lab = T.srgb_to_lab(ce)
    chroma = np.hypot(lab[..., 1], lab[..., 2])
    op = L - th
    lit = (th > 0.2) & (L > 1.8 * op + 0.05) & ((L > 0.4) | ((chroma > 25) & (L > 0.25)))
    n, cc, st, _ = cv2.connectedComponentsWithStats(lit.astype(np.uint8), connectivity=8)
    keep = np.zeros(L.shape, np.float32)
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < 3 or a > 400 or max(w, h) > 40:
            continue
        # lit decal = an isolated bright island: its surroundings (a ring 1..3 texels out) are much darker
        x0, y0, x1, y1 = max(x - 4, 0), max(y - 4, 0), min(x + w + 4, L.shape[1]), min(y + h + 4, L.shape[0])
        comp = (cc[y0:y1, x0:x1] == i).astype(np.uint8)
        ring = cv2.dilate(comp, np.ones((5, 5), np.uint8)) - cv2.dilate(comp, np.ones((3, 3), np.uint8))
        Lw = L[y0:y1, x0:x1]
        lim = 0.6 if (a <= 120 and max(w, h) <= 16) else 0.3  # larger glyph blocks: on near-black only
        if ring.sum() == 0 or Lw[ring > 0].mean() > lim * Lw[comp > 0].mean():
            continue
        # and not the tip of a larger bright shape (a plate corner the opening cut off): the bright region
        # that holds it, at 60 percent of its level, stays small and inside a 20-texel margin
        X0, Y0, X1, Y1 = max(x - 20, 0), max(y - 20, 0), min(x + w + 20, L.shape[1]), min(y + h + 20, L.shape[0])
        nb, cb = cv2.connectedComponents((L[Y0:Y1, X0:X1] > 0.6 * Lw[comp > 0].mean()).astype(np.uint8), connectivity=8)
        ids = np.unique(cb[(cc[Y0:Y1, X0:X1] == i) & (cb > 0)])
        reg = np.isin(cb, ids)
        touch = ((Y0 > 0 and reg[0].any()) or (Y1 < L.shape[0] and reg[-1].any())
                 or (X0 > 0 and reg[:, 0].any()) or (X1 < L.shape[1] and reg[:, -1].any()))
        if reg.sum() > max(2 * a, a + 24) or touch:
            continue
        keep[cc == i] = 1
    # lit glyph panels: saturated bright islands on near-black (the Elite's green glyph blocks), whole
    isl = ((L > 0.3) & (chroma > 20)).astype(np.uint8)
    n2, c2, st2, _ = cv2.connectedComponentsWithStats(isl, connectivity=8)
    for i in range(1, n2):
        x, y, w, h, a = st2[i]
        if a < 12 or a > 1500 or max(w, h) > 64:
            continue
        x0, y0, x1, y1 = max(x - 5, 0), max(y - 5, 0), min(x + w + 5, L.shape[1]), min(y + h + 5, L.shape[0])
        comp = (c2[y0:y1, x0:x1] == i).astype(np.uint8)
        hull = cv2.morphologyEx(comp, cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8))
        ring = cv2.dilate(hull, np.ones((7, 7), np.uint8)) - cv2.dilate(hull, np.ones((3, 3), np.uint8))
        Lw = L[y0:y1, x0:x1]
        if ring.sum() and Lw[ring > 0].mean() < min(0.12, 0.25 * Lw[comp > 0].mean()):
            keep[y0:y1, x0:x1] = np.maximum(keep[y0:y1, x0:x1], hull.astype(np.float32))
    if mp is not None:
        keep = np.maximum(keep, (mp[..., 1] > 0.1).astype(np.float32))
    keep = cv2.dilate(keep, np.ones((3, 3), np.uint8))
    return np.clip(T.gblur(keep, 0.7, wrap) * 1.5, 0, 1)


def _up(a, F, wrap, interp=cv2.INTER_LINEAR):
    h, w = a.shape[:2]
    if not wrap:
        return cv2.resize(a, (w * F, h * F), interpolation=interp)
    p = 2
    ap = T.pad_wrap(a, p)
    u = cv2.resize(ap, (ap.shape[1] * F, ap.shape[0] * F), interpolation=interp)
    return u[p * F:p * F + h * F, p * F:p * F + w * F]


def new_band(Lr, F, wrap):
    """Result luminance above CE's Nyquist: L - Lanczos(boxdown(L))."""
    d = T.boxdown(Lr, F)
    return Lr - _up(d, F, wrap, cv2.INTER_LANCZOS4)


def half_band(Lr, F, wrap):
    """Result luminance finer than half a CE texel (wavelength < F px): the octave(s) beyond a 4x ML base
    at F == 8, i.e. what a detail stage adds on its own."""
    h = max(1, F // 2)
    return Lr - _up(T.boxdown(Lr, h), h, wrap, cv2.INTER_LANCZOS4)


def nms_ridges(L, sigma):
    """Gradient magnitude (levels/px) and its non-maximum-suppressed ridges (4 directions)."""
    Ls = cv2.GaussianBlur(L, (0, 0), sigma) if sigma > 0 else L
    gx = cv2.Sobel(Ls, cv2.CV_32F, 1, 0, ksize=3, scale=1 / 8)
    gy = cv2.Sobel(Ls, cv2.CV_32F, 0, 1, ksize=3, scale=1 / 8)
    m = np.hypot(gx, gy) * 255
    ang = np.mod(np.arctan2(gy, gx), np.pi)
    q = (np.floor((ang + np.pi / 8) / (np.pi / 4)).astype(np.int8)) % 4
    nms = np.zeros(m.shape, bool)
    for qi, (dy, dx) in enumerate(((0, 1), (1, 1), (1, 0), (1, -1))):
        nms |= (q == qi) & (m >= np.roll(m, (-dy, -dx), (0, 1))) & (m > np.roll(m, (dy, dx), (0, 1)))
    return nms, m


def _tiles(a, k):
    h, w = a.shape
    return a[:h - h % k, :w - w % k].reshape(h // k, k, w // k, k).sum((1, 3))


def _tile_spectra(N, F, mask, tile_tex=32):
    """Yield (ty, tx, P, r, theta, tp) for every 32-texel tile of band N (mask multiplies the band)."""
    tp = tile_tex * F
    H, W = N.shape
    win = np.outer(np.hanning(tp), np.hanning(tp)).astype(np.float32)
    y, x = np.indices((tp, tp)) - tp // 2
    r = np.hypot(x, y)
    th = np.degrees(np.arctan2(y, x)) % 180
    for ty in range(0, H - tp + 1, tp):
        for tx in range(0, W - tp + 1, tp):
            t = N[ty:ty + tp, tx:tx + tp] * mask[ty:ty + tp, tx:tx + tp]
            P = np.abs(np.fft.fftshift(np.fft.fft2((t - t.mean()) * win))) ** 2
            yield ty, tx, t, P, r, th, tp


def weave(N2, F, mask, tile_tex=32):
    """Share of tiles whose half-texel band, away from CE's edges, is concentrated on the image axes
    (cross-hatch): axis (+-5 deg) over diagonal (+-5 deg) power above WEAVE_RATIO, band rms above 0.6."""
    flagged, n, worst = 0, 0, 0.0
    for ty, tx, t, P, r, th, tp in _tile_spectra(N2, F, mask, tile_tex):
        n += 1
        if np.sqrt((t ** 2).mean()) * 255 < 0.6:
            continue
        a = (r >= tile_tex * 1.25) & (r < tp / 2 * 0.95)
        ax = a & ((th < 5) | (th > 175) | (np.abs(th - 90) < 5))
        dg = a & ((np.abs(th - 45) < 5) | (np.abs(th - 135) < 5))
        ratio = P[ax].mean() / (P[dg].mean() + 1e-20)
        worst = max(worst, float(ratio))
        flagged += ratio > WEAVE_RATIO
    return flagged / max(n, 1), worst


def periodicity(N, ce, F, mask, tile_tex=32):
    """Share of tiles whose band above CE's Nyquist has an off-axis periodic peak that is not a harmonic of
    a peak in CE's own tile spectrum (axis-aligned cross-hatch is `weave`)."""
    Lc = T.lum(ce)
    winc = np.outer(np.hanning(tile_tex), np.hanning(tile_tex)).astype(np.float32)
    yc, xc = np.indices((tile_tex, tile_tex)) - tile_tex // 2
    rc = np.hypot(xc, yc)
    flagged, n, worst = 0, 0, 0.0
    for ty, tx, t, P, r, th, tp in _tile_spectra(N, F, mask, tile_tex):
        n += 1
        if np.sqrt((t ** 2).mean()) * 255 < 0.6:
            continue
        ri = r.astype(int)
        r_lo, r_hi = int(tile_tex / 2 * 1.2), int(tp / 2 * 0.9)
        ann = (r >= r_lo) & (r <= r_hi)
        offax = ~((th < 3) | (th > 177) | (np.abs(th - 90) < 3))
        med = np.zeros(ri.max() + 1)
        cnt = np.bincount(ri[ann])
        for rr in np.nonzero(cnt)[0]:
            med[rr] = np.median(P[ri == rr])
        ratio = P / (med[ri] + 1e-20)
        mx = cv2.dilate(P.astype(np.float32), np.ones((3, 3), np.uint8))
        tot = P[ann].sum() + 1e-20
        pk = ann & offax & (P >= mx) & (ratio > PERIOD_RATIO) & (P / tot > PERIOD_SHARE)
        if not pk.any():
            continue
        cy, cx = ty // F, tx // F
        c = Lc[cy:cy + tile_tex, cx:cx + tile_tex]
        Pc = np.abs(np.fft.fftshift(np.fft.fft2((c - c.mean()) * winc))) ** 2
        mxc = cv2.dilate(Pc.astype(np.float32), np.ones((3, 3), np.uint8))
        medc = np.median(Pc[rc >= 2]) + 1e-20
        f0 = [(a - tile_tex // 2, b - tile_tex // 2) for a, b in np.argwhere((Pc >= mxc) & (rc >= 2) & (Pc > 8 * medc))]
        for py, px in np.argwhere(pk):
            fy, fx = py - tp // 2, px - tp // 2
            harm = any(math.hypot(fy - m * gy, fx - m * gx) <= 1.5 or math.hypot(fy + m * gy, fx + m * gx) <= 1.5
                       for gy, gx in f0 for m in range(1, 16))
            if not harm:
                flagged += 1
                worst = max(worst, float(ratio[py, px]))
                break
    return flagged / max(n, 1), worst


def check(res, ce, wrap, mp=None, keep=None, detail=False, decals=True):
    """All measures for one result (target or 4x) against CE. Returns a dict; `fails` lists the gates
    exceeded. With detail=True also the per-texel failure maps (CE resolution) for pictures."""
    F = res.shape[0] // ce.shape[0]
    emid, grad, Llo = regions(ce, wrap)
    low = emid < E_LOW
    gradr = low & (grad >= G_LOW)
    pl = plain(ce, F, wrap)
    Lr, Lp = T.lum(res), T.lum(pl)
    if keep is None:
        keep = keep_mask(ce, wrap, mp, decals)
    kk = keep < 0.5
    out = {}
    # CE's strong edges (plain resample, ridges above a quarter of the local CE contrast per texel)
    Lc = T.lum(ce)
    k3 = np.ones((3, 3), np.uint8)
    con = (cv2.dilate(Lc, k3) - cv2.erode(Lc, k3)) * 255
    conu = _up(T.gblur(con, 0.7, wrap), F, wrap)
    rc, mc = nms_ridges(Lp, F / 4)
    strong_ce = rc & (mc > 0.25 * conu / F) & (conu > CONTRAST_MIN)
    dist = cv2.distanceTransform((~strong_ce).astype(np.uint8), cv2.DIST_L2, 3)
    # excess: energy finer than half a texel against CE's own mid-band energy
    N2 = half_band(Lr, F, wrap)
    etop = np.sqrt(T.boxdown(N2 * N2, F)) * 255
    etop = T.gblur(etop, 1.0, wrap)
    bad = (etop > EXCESS_A * emid + EXCESS_B) & kk
    out["excess"] = float(bad[kk].mean())
    out["excess_low"] = float(bad[low & kk].mean()) if (low & kk).sum() > 64 else 0.0
    out["etop_p50"] = float(np.median(etop))
    # weave and periodic peaks, away from CE's edges (an edge's own harmonics lie on the axes or on CE peaks)
    away = np.clip((dist - 1.0 * F) / (0.5 * F), 0, 1).astype(np.float32)
    out["weave"], out["weave_worst"] = weave(N2, F, away)
    N = new_band(Lr, F, wrap)
    out["period"], out["period_worst"] = periodicity(N, ce, F, away)
    # doubled contours / new contours along CE's edges
    rr, mr = nms_ridges(Lr, max(0.7, F / 4))
    sr = rr & (mr > 0.25 * conu / F) & (conu > CONTRAST_MIN)
    kt = 8 * F
    nr = _tiles((sr & (dist <= 1.25 * F)).astype(np.float32), kt)
    nc = _tiles(strong_ce.astype(np.float32), kt)
    et = nc >= 3 * F
    mult = nr / np.maximum(nc, 1)
    out["contour"] = float((mult[et] > CONTOUR_MULT).mean()) if et.any() else 0.0
    out["contour_mult_mean"] = float(mult[et].mean()) if et.any() else 0.0

    # banding in smooth gradients: CE ramps (low-band gradient, contrast still growing from a 3- to a 7-texel
    # window, little mid-band texture); per 8-texel tile, the posterisation index -- |t - 0.5| * 2 with t the
    # pixel's place between its local min and max over +-F/2 px (0 on a ramp, 1 on a plateau between steps)
    # -- minus the plain resample's; a tile fails above BAND_TILE
    k7 = np.ones((7, 7), np.uint8)
    con7 = (cv2.dilate(Lc, k7) - cv2.erode(Lc, k7)) * 255
    ramp = (grad >= 2.0) & (con7 >= 2.2 * con) & (emid < np.maximum(E_LOW, RAMP_K * grad)) & kk
    gu = _up(ramp.astype(np.uint8), F, wrap, cv2.INTER_NEAREST) > 0
    kr = np.ones((F + 1, F + 1), np.uint8)

    def post(L):
        Ls = cv2.GaussianBlur(L, (0, 0), 0.7)
        lo, hi = cv2.erode(Ls, kr), cv2.dilate(Ls, kr)
        ok = (gu & ((hi - lo) * 255 > 6)).astype(np.float32)
        v = np.abs((Ls - lo) / (hi - lo + 1e-4) - 0.5) * 2 * ok
        return _tiles(v, 8 * F), _tiles(ok, 8 * F)
    vp, okp = post(Lp)
    vr, okr = post(Lr)
    sel = okp >= 0.4 * (8 * F) ** 2
    d = vr / np.maximum(okr, 1) - vp / np.maximum(okp, 1)
    out["banding_share"] = float((d[sel] > BAND_TILE).mean()) if sel.sum() >= 4 else 0.0
    out["banding_at"] = [[int(x) * 8, int(y) * 8] for y, x in np.argwhere(sel & (d > BAND_TILE))]  # CE texels
    out["banding"] = float((d[sel] > BAND_TILE).sum()) if sel.sum() >= 4 else 0.0  # failing ramp tiles
    out["ramp_tiles"] = int(sel.sum())
    # speckle in dark regions
    dark = Llo < DARK
    du = _up(dark.astype(np.uint8), F, wrap, cv2.INTER_NEAREST) > 0
    kd = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (F + 1, F + 1))

    def specks(L):
        op = cv2.morphologyEx(L, cv2.MORPH_OPEN, kd)
        s = ((L - op) > np.maximum(SPECK_ABS, SPECK_REL * op)) & du
        nlab, _ = cv2.connectedComponents(s.astype(np.uint8), connectivity=8)
        return nlab - 1
    nd = max(int(dark.sum()), 1)
    out["speckle"] = float((specks(Lr) - specks(Lp)) / nd * 1000) if dark.sum() > 64 else 0.0
    out["dark_texels"] = int(dark.sum())
    # glyphs in small lit decals and self-illumination
    ku = _up(keep, F, wrap) > 0.5
    out["decal"] = float(np.sqrt(((Lr - Lp)[ku] ** 2).mean()) * 255) if ku.sum() > 4 * F * F else 0.0
    out["decal_texels"] = int((keep > 0.5).sum())
    contour_bad = et & (mult > CONTOUR_MULT)
    out["contour_at"] = [[int(x) * 8, int(y) * 8] for y, x in np.argwhere(contour_bad)][:200]
    out["fails"] = [g for g, v in GATES.items() if out[g] > v]
    if detail:
        out["_maps"] = {"excess": bad, "low": low, "ramp": ramp, "dark": dark, "keep": keep, "mult": mult}
    return out


# ---------------------------------------------------------------- calibration on the pre-fix outputs
MAPS = {"chief_diffuse": (8, False, "chief_multi.png"), "b30_metal_panels": (8, True, None),
        "a30_cliff_rock": (4, True, None), "a50_cov_panel": (16, True, None),
        "elite_torso": (8, False, "elite_torso_multi.png"), "marine_body": (8, False, None),
        "a30_detail_grass": (8, True, None), "marine_face": (16, False, None)}
ENV = {"a30_cliff_rock", "a30_detail_grass"}  # no decal detection on environment detail maps


def method_files(root, name, F):
    """{method: path} of the target-size results in a fusion output folder (4x is the target for F == 4)."""
    d = os.path.join(root, name)
    if F == 4:
        cand = {"fusion": "fusion_4x.png", "fusion+B2": "fusionB2_4x.png", "B2": "B2_4x.png",
                "select": "select_4x.png"}
    else:
        cand = {"fusion": "fusion_target.png", "fusion+B2": "fusionB2_target.png", "B2": "B2_target.png"}
    out = {m: os.path.join(d, f) for m, f in cand.items() if os.path.exists(os.path.join(d, f))}
    if "B2" not in out and F != 4:
        p = os.path.join(T.WORK, "out/final", name, "maps/B2_albedo.png")
        if os.path.exists(p):
            out["B2"] = p
    return out


def calibrate(root, names=None):
    rows = {}
    for name in names or MAPS:
        F, wrap, mpf = MAPS[name]
        ce = T.load(os.path.join(T.WORK, "src", name + ".png"))[0]
        mp = T.load(os.path.join(T.WORK, "src", mpf))[0] if mpf else None
        rows[name] = {}
        for m, p in method_files(root, name, F).items():
            r = check(T.load(p)[0], ce, wrap, mp=mp, decals=name not in ENV)
            rows[name][m] = r
            print(name, m, {k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()}, flush=True)
    return rows


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "calibrate":
        root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(T.WORK, "out/fusion/review/before_b2fix")
        names = sys.argv[3:] or None
        rows = calibrate(root, names)
        outp = os.path.join(root, "invented_check.json")
        json.dump(rows, open(outp, "w"), indent=1)
        print("wrote", outp)
