"""(b)+(c) Material-aware re-synthesis ("MARS") of a CE texture to 4K, plus height/normal/roughness.

CE's low frequencies are kept exactly (the result is back-projected: its box filter to CE size is CE),
each material gets new detail above CE's Nyquist that is CE's own detail of that material carried down
the octaves (self-exemplar patch synthesis, amplitude extrapolated along the material's own spectrum and
modulated by how much detail CE painted at that spot), and hard edges (panel lines, plate borders, seams)
are redrawn as iso-contours of a smoothed upscale (a vectorisation without a vector file: straight where
CE is straight, 1.5 px wide at 4K).

  from mars import mars
  out = mars(ce_rgb, F, base, kind="hard"|"organic"|"skin", wrap=True, mp=None, bump=None)
  -> dict(albedo=..., height=..., normal=..., rough=..., labels=..., edge=...)

base: CE upscaled by F already (Lanczos or an ML model; it is back-projected here).
"""
import math
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texlib as T  # noqa: E402


def g(a, s):
    return cv2.GaussianBlur(a, (0, 0), s, borderType=cv2.BORDER_WRAP if False else cv2.BORDER_REFLECT) if s > 0 else a


def gw(a, s, wrap):
    if s <= 0:
        return a
    if not wrap:
        return cv2.GaussianBlur(a, (0, 0), s, borderType=cv2.BORDER_REFLECT)
    p = int(math.ceil(3 * s)) + 1
    pa = T.pad_wrap(a, p)
    return cv2.GaussianBlur(pa, (0, 0), s)[p:-p, p:-p]


# ---------------------------------------------------------------- materials
def materials(ce, k=4, mp=None, wrap=True, seed=1):
    """k-means at CE resolution on smoothed Lab, local detail energy and (if given) the multipurpose
    channels. Production would use the class-mask recipes for characters; this is the generic path."""
    lab = T.srgb_to_lab(ce).astype(np.float32)
    lab_s = np.stack([gw(lab[..., i], 1.0, wrap) for i in range(3)], -1)
    L = lab[..., 0]
    e = np.sqrt(gw((L - gw(L, 1.0, wrap)) ** 2, 2.0, wrap))
    feats = [lab_s[..., 0] / 10, lab_s[..., 1] / 6, lab_s[..., 2] / 6, e / 2]
    if mp is not None:
        feats += [gw(mp[..., i], 1.0, wrap) * 6 for i in range(3)]
    X = np.stack(feats, -1).reshape(-1, len(feats)).astype(np.float32)
    cv2.setRNGSeed(seed)
    _, lab_k, _ = cv2.kmeans(X, k, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.01), 4,
                             cv2.KMEANS_PP_CENTERS)
    lab_k = lab_k.reshape(ce.shape[:2])
    # majority filter 3x3 to drop speckle
    onehot = np.stack([(lab_k == i).astype(np.float32) for i in range(k)], -1)
    onehot = np.stack([cv2.blur(onehot[..., i], (3, 3)) for i in range(k)], -1)
    return onehot.argmax(-1)


# ---------------------------------------------------------------- edges
def edge_strength_ce(ce, wrap):
    L = T.lum(ce)
    Ls = gw(L, 0.7, wrap)
    gx = cv2.Sobel(Ls, cv2.CV_32F, 1, 0, borderType=cv2.BORDER_REFLECT)
    gy = cv2.Sobel(Ls, cv2.CV_32F, 0, 1, borderType=cv2.BORDER_REFLECT)
    return np.hypot(gx, gy)


def straighten(L, F, wrap, n_dir=8, length=3.0, sigma_t=1.5):
    """Line-integral smoothing along the local edge direction (structure tensor at sigma_t CE texels, 8
    directions, kernels `length` CE texels long), weighted by coherence: wobbly painted edges become the
    straight lines they stand for; corners and isotropic texture (low coherence) are left alone."""
    gx = cv2.Sobel(L, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(L, cv2.CV_32F, 0, 1, ksize=3)
    jxx, jyy, jxy = (gw(a, sigma_t * F, wrap) for a in (gx * gx, gy * gy, gx * gy))
    tr = jxx + jyy + 1e-9
    det = np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2)
    coh = (det / tr) ** 2
    theta = 0.5 * np.arctan2(2 * jxy, jxx - jyy) + np.pi / 2  # edge tangent
    klen = int(length * F) | 1
    out = np.zeros_like(L)
    wsum = np.zeros_like(L)
    for i in range(n_dir):
        a = np.pi * i / n_dir
        ker = np.zeros((klen, klen), np.float32)
        c = klen // 2
        cv2.line(ker, (int(round(c - c * np.cos(a))), int(round(c - c * np.sin(a)))),
                 (int(round(c + c * np.cos(a))), int(round(c + c * np.sin(a)))), 1.0, 1, cv2.LINE_AA)
        ker /= ker.sum()
        wgt = np.cos(theta - a) ** 2
        wgt = wgt ** 8
        Lp = T.pad_wrap(L, c) if wrap else np.pad(L, c, mode="reflect")
        out += wgt * cv2.filter2D(Lp, -1, ker)[c:-c, c:-c]
        wsum += wgt
    sm = out / (wsum + 1e-9)
    wc = np.clip((coh - 0.25) / 0.5, 0, 1)
    return L + wc * (sm - L), coh


def crisp_edges(base, F, wrap, c_lo=None, c_hi=None, width=1.5, straight=True):
    """Redraw hard edges as iso-contours: within a window of +-F px the luminance is snapped to its local
    min or max across the mid level, with a transition `width` px wide; chroma kept (ratio scaling).
    Weight: local contrast (c_lo..c_hi) so flat or noisy areas are untouched."""
    L = T.lum(base)
    Ls = gw(L, 0.45 * F, wrap)
    coh = None
    if straight:
        Ls, coh = straighten(Ls, F, wrap)
    r = int(F)
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    Lmin = cv2.erode(Ls, ker, borderType=cv2.BORDER_REFLECT)
    Lmax = cv2.dilate(Ls, ker, borderType=cv2.BORDER_REFLECT)
    c = Lmax - Lmin
    mid = 0.5 * (Lmin + Lmax)
    gx = cv2.Sobel(Ls, cv2.CV_32F, 1, 0, ksize=3, scale=1 / 8)
    gy = cv2.Sobel(Ls, cv2.CV_32F, 0, 1, ksize=3, scale=1 / 8)
    gm = np.hypot(gx, gy) + 1e-5
    d = (Ls - mid) / gm  # signed distance (px) to the iso-contour
    t = np.clip(0.5 + d / width, 0, 1)
    t = t * t * (3 - 2 * t)
    Lnew = Lmin + t * c
    # keep CE's own (smoothed) structure inside the band where the profile is not a step: blend by contrast
    if c_lo is None:  # adaptive: the map's own contrast distribution (low-contrast Covenant paint too)
        c_lo = max(0.02, float(np.percentile(c[::4, ::4], 55)))
        c_hi = max(c_lo + 0.02, float(np.percentile(c[::4, ::4], 85)))
    w = np.clip((c - c_lo) / (c_hi - c_lo), 0, 1)
    if coh is not None:  # only line-like edges (coherent structure): mottling and blobs are not posterised
        w = w * np.clip((coh - 0.35) / 0.35, 0, 1)
    w = gw(w, 0.5 * F, wrap)
    Lout = L + w * (Lnew - Ls)
    ratio = (Lout + 0.02) / (L + 0.02)
    return np.clip(base * ratio[..., None], 0, 1), w


# ---------------------------------------------------------------- detail synthesis
def _patch_win(P):
    a = np.sin(np.pi * (np.arange(P) + 0.5) / P).astype(np.float32)
    return np.outer(a, a)


def synth_octaves(ce, F, labels, emask, wrap, source="exemplar", seed=7, chroma=0.35, gain=1.0,
                  ratio_clip=(0.45, 0.85), amp_floor=0.0, ce_clean=None):
    """Detail above CE's Nyquist, per material.
    Exemplar band: CE's DoG(0.7, 1.4) texels (features ~3-4 texels: the painted grain, pitting, mottling;
    the 1-texel band is left out, it is mostly DXT block noise and dither). Octave j (1..log2 F, the output
    octave just above CE's Nyquist is j=1) is that band re-sampled to s = F / 2^(j+1) output px per CE texel
    (its ~4-texel wavelength lands at 2F/2^j px) and laid down as random windowed patches drawn from the
    same material's interior (edges excluded; sum of squared windows == 1 keeps the variance). Amplitude:
    rms(band) * r^j, r = rms(DoG 0.7..1.4) / rms(DoG 1.4..2.8) of that material (its own spectral slope,
    clipped to ratio_clip), times CE's local detail level at that spot.
    source="noise": same amplitudes and envelope, Gaussian noise (spectral extension) instead of patches."""
    rng = np.random.default_rng(seed)
    h, w = ce.shape[:2]
    H, W = h * F, w * F
    nb = int(round(math.log2(F)))
    k = labels.max() + 1
    # bands from a de-blocked CE (the ML 4x result box-filtered back to CE size) when given: CE's DXT
    # 4x4 block grid in the exemplar otherwise comes back as a fine cross-hatch ("canvas") at 4K
    lin = (ce if ce_clean is None else ce_clean).astype(np.float32)
    g07 = np.stack([gw(lin[..., i], 0.7, wrap) for i in range(3)], -1)
    g14 = np.stack([gw(lin[..., i], 1.4, wrap) for i in range(3)], -1)
    g28 = np.stack([gw(lin[..., i], 2.8, wrap) for i in range(3)], -1)
    b1, b2 = g07 - g14, g14 - g28
    lb1, lb2 = T.lum(b1), T.lum(b2)
    loc = np.sqrt(gw(lb1 ** 2, 3.0, wrap))
    exl = lb1[..., None]
    ex = exl + chroma * (b1 - exl)
    interior = emask < 0.5
    out = np.zeros((H, W, 3), np.float32)
    stats = {}
    for m in range(k):
        sel = (labels == m) & interior
        if sel.sum() < 64:
            sel = labels == m
        if sel.sum() < 16:
            continue
        r1 = float(np.sqrt((lb1[sel] ** 2).mean()))
        r2 = float(np.sqrt((lb2[sel] ** 2).mean())) + 1e-6
        stats[int(m)] = {"band_rms": r1 * 255, "octave_ratio": float(np.clip(r1 / r2, *ratio_clip)),
                         "raw_ratio": r1 / r2, "texels": int(sel.sum())}
    lab_up = cv2.resize(labels.astype(np.uint8), (W, H), interpolation=cv2.INTER_NEAREST)
    loc_up = cv2.resize(loc, (W, H), interpolation=cv2.INTER_LINEAR)
    for j in range(1, nb + 1):
        s = F / 2 ** (j + 1)
        P = int(max(24, 12 * s))
        P += P % 2
        win = _patch_win(P)
        stride = P // 2
        ws, hs = max(int(round(w * s)), P + 2), max(int(round(h * s)), P + 2)
        exs = cv2.resize(ex, (ws, hs), interpolation=cv2.INTER_CUBIC if s >= 1 else cv2.INTER_AREA)
        labs = cv2.resize(labels.astype(np.uint8), (ws, hs), interpolation=cv2.INTER_NEAREST)
        ints = cv2.resize(interior.astype(np.uint8), (ws, hs), interpolation=cv2.INTER_NEAREST)
        cands = {}
        st = max(1, P // 4)
        ys, xs = np.mgrid[0:hs - P:st, 0:ws - P:st]
        cy, cx = ys + P // 2, xs + P // 2
        for m in stats:
            ok = (labs[cy, cx] == m) & (ints[cy, cx] > 0)
            if ok.sum() == 0:
                ok = labs[cy, cx] == m
            cands[m] = np.stack([ys[ok], xs[ok]], -1)
        # random (Poisson) patch centres, ~5 patches over each pixel, normalised by sqrt(sum w^2): a regular
        # stride grid prints its own period as a fine cross-hatch ("canvas") at 4K
        oct_ = np.zeros((H + 2 * P, W + 2 * P, 3), np.float32)
        w2 = np.zeros((H + 2 * P, W + 2 * P), np.float32)
        npatch = int(5 * (H + P) * (W + P) / (P * P * 0.25))
        cys = rng.integers(-P // 2, H + P // 2, npatch)
        cxs = rng.integers(-P // 2, W + P // 2, npatch)
        for yc0, xc0 in zip(cys, cxs):
            yc, xc = min(max(yc0, 0), H - 1), min(max(xc0, 0), W - 1)
            m = int(lab_up[yc, xc])
            if m not in stats:
                continue
            a = stats[m]["band_rms"] / 255 * stats[m]["octave_ratio"] ** j
            if source == "exemplar" and len(cands[m]):
                sy, sx = cands[m][rng.integers(len(cands[m]))]
                patch = exs[sy:sy + P, sx:sx + P]
                rot = rng.integers(4) if s <= 1 else 0
                patch = np.rot90(patch, rot) if rot else patch
                if rng.random() < 0.5:
                    patch = patch[:, ::-1]
                pr = np.sqrt((T.lum(patch) ** 2).mean()) + 1e-6
                patch = patch / pr * a
            else:
                n = rng.standard_normal((P, P)).astype(np.float32)
                lam = 2 * F / 2 ** j
                n = gw(n, lam / 6, False) - gw(n, lam / 3, False)
                n = n / (n.std() + 1e-6) * a
                patch = np.repeat(n[..., None], 3, -1)
            ya, xa = yc0 - P // 2 + P, xc0 - P // 2 + P
            oct_[ya:ya + P, xa:xa + P] += patch * win[..., None]
            w2[ya:ya + P, xa:xa + P] += win * win
        oct_ /= np.sqrt(w2 + 0.05)[..., None]
        stride = P
        out += oct_[stride:stride + H, stride:stride + W]
    mrms = np.zeros(k, np.float32)
    for m in stats:
        mrms[m] = stats[m]["band_rms"] / 255
    mod = np.clip(loc_up / (mrms[lab_up] + 1e-4), amp_floor, 2.0)
    return out * mod[..., None] * gain, stats


# ---------------------------------------------------------------- height / normal / roughness
def height_from_normal(nrm):
    """Frankot-Chellappa: integrate a tangent-space normal map (periodic) to height (texel units)."""
    n = nrm * 2 - 1
    nz = np.clip(n[..., 2], 0.2, 1)
    p, q = -n[..., 0] / nz, n[..., 1] / nz  # dz/dx, dz/dy (green up -> y down flips)
    h, w = p.shape
    wx = np.fft.fftfreq(w) * 2 * np.pi
    wy = np.fft.fftfreq(h) * 2 * np.pi
    WX, WY = np.meshgrid(wx, wy)
    P, Q = np.fft.fft2(p), np.fft.fft2(q)
    den = WX ** 2 + WY ** 2
    den[0, 0] = 1
    Z = (-1j * WX * P - 1j * WY * Q) / den
    Z[0, 0] = 0
    return np.real(np.fft.ifft2(Z)).astype(np.float32)


def normal_from_height(hgt, strength=1.0, wrap=True):
    p = 1
    hp = T.pad_wrap(hgt, p) if wrap else np.pad(hgt, p, mode="edge")
    gx = (hp[1:-1, 2:] - hp[1:-1, :-2]) * 0.5 * strength
    gy = (hp[2:, 1:-1] - hp[:-2, 1:-1]) * 0.5 * strength
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def lit_preview(albedo, nrm, rough, light=(-0.55, -0.45, 0.70)):
    """Single directional light, Lambert + GGX-ish spec, for side-by-sides only."""
    n = nrm * 2 - 1
    l = np.array(light, np.float32); l /= np.linalg.norm(l)
    v = np.array([0, 0, 1], np.float32)
    hv = (l + v) / np.linalg.norm(l + v)
    ndl = np.clip((n * l).sum(-1), 0, 1)
    ndh = np.clip((n * hv).sum(-1), 0, 1)
    a2 = np.clip(rough, 0.05, 1) ** 4
    D = a2 / (np.pi * (ndh ** 2 * (a2 - 1) + 1) ** 2)
    spec = 0.04 * D * ndl * 0.25
    lin = np.where(albedo <= 0.04045, albedo / 12.92, ((albedo + 0.055) / 1.055) ** 2.4)
    c = lin * (0.25 + 0.95 * ndl[..., None]) + spec[..., None]
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


ROUGH = {"hard": (0.38, 0.6), "organic": (0.85, 0.15), "skin": (0.55, 0.2), "cloth": (0.8, 0.1)}


def mars(ce, F, base, kind="hard", wrap=True, mp=None, bump=None, k=4, source="exemplar", seed=7,
         detail_gain=1.0, crisp=None, height_scale=None, ce_clean=None):
    h, w = ce.shape[:2]
    H, W = h * F, w * F
    labels = materials(ce, k, mp, wrap)
    es = edge_strength_ce(ce, wrap)
    emask = (es > np.quantile(es, 0.88)).astype(np.float32)
    emask = cv2.dilate(emask, np.ones((3, 3), np.uint8))
    a = T.backproject_low(base.copy(), ce, 4, wrap=wrap)
    if crisp is None:
        crisp = kind == "hard"
    ew = np.zeros((H, W), np.float32)
    if crisp:
        a, ew = crisp_edges(a, F, wrap)
    det, stats = synth_octaves(ce, F, labels, emask, wrap, source=source, seed=seed, gain=detail_gain,
                               amp_floor=0.35 if kind == "organic" else 0.15, ce_clean=ce_clean)
    det *= (1 - 0.8 * ew)[..., None]  # no grain across the redrawn lines
    alb = np.clip(a + det, 0, 1)
    alb = T.backproject_low(alb, ce, 3, wrap=wrap)
    # height: CE bump (integrated) + line grooves (dark crisp lines) + detail luminance
    hs = height_scale if height_scale is not None else {"hard": 0.35, "organic": 1.0, "skin": 0.25, "cloth": 0.5}[kind]
    hgt = np.zeros((H, W), np.float32)
    if bump is not None:
        hb = height_from_normal(bump)
        hb = cv2.resize(hb, (W, H), interpolation=cv2.INTER_CUBIC) * (W / bump.shape[1])
        hgt += hb
    Ld = T.lum(det)
    hgt += Ld * 255 * hs * 0.6
    if crisp:
        La = T.lum(alb)
        groove = ew * np.clip(gw(La, 2 * F, wrap) - La, 0, 1)
        hgt -= groove * 255 * 0.12 * F / 4
    nrm = normal_from_height(hgt, strength=0.5 * 4 / F, wrap=wrap)
    r0, rv = ROUGH[kind]
    rough = np.clip(r0 + rv * np.tanh(-Ld * 30) + 0.1 * (1 - T.lum(gw(alb, F, wrap))), 0.05, 1)
    return {"albedo": alb, "height": hgt, "normal": nrm, "rough": rough, "labels": labels, "edge": ew,
            "stats": stats}
