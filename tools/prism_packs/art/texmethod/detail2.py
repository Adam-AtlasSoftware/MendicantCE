"""B2's detail stage, second version: detail above the base's own Nyquist that CE's own texture asks for,
and nothing where CE is flat, smooth or dark; one edge pass aligned to the base's edge; CE's shape kept
under small lit decals and self-illumination.

  from detail2 import detail2
  out = detail2(ce, F, base4, kind, wrap, P=None, keep=None, redrawn=False)
  -> dict(albedo=..., det=..., edge=..., amp=..., keep=...)

ce     CE map (h, w, 3), float sRGB
F      target factor (texels -> px); the base is a 4x result (ML A' or fusion), Lanczos'd to F here
base4  the 4x base (already back-projected to CE's low band)
kind   "hard" | "cov" | "organic" | "cloth" | "skin": hard and cov get the edge pass
P      soft class memberships at CE resolution (fusion.CLASSES order) for the exemplar pools; None = one pool
keep   keep-CE-shape mask at CE resolution (invented_check.keep_mask); plain bicubic there
redrawn  the base already drew its lines (fusion's line band): no straightening, only the aligned crisping
no_edge  CE-resolution weight where no edge pass is wanted (fabric and skin classes)
edge_quiet  no detail octaves on CE's strong edges and their flanks, no exemplars from there (default; the
         tier review found edge-parallel ghost lines beside plate borders without it)

What changed against mars.mars (each fixes one failure of the visual review):
- canvas on flat strips: the old exemplar was CE's own 1-texel band (pixel-art, axis-aligned, DXT residue)
  re-scaled to every octave from 1 texel down and laid on top of a base that already had those octaves,
  each patch normalised to full amplitude however flat it was, with random 90-degree turns -> an
  axis-aligned cross-hatch. Now: only the octave(s) the base lacks (above its 4x Nyquist: one at F = 8,
  two at F = 16, none at F = 4); the exemplar is the base's own top band (ML grain, not pixel art); the
  amplitude is a per-pixel map -- the base's local top-band rms there times its local octave ratio, times
  a weight from CE's own mid-band energy (no texture in CE -> no detail); patches are drawn only from
  textured source spots and turned to the destination's own orientation (structure tensor) when the
  destination is anisotropic.
- doubled contours and invented rivets: the old redraw posterised luminance between the local min and max
  of a heavily smoothed copy, added the difference to the unsmoothed one (two edges where they disagree)
  and drew closed contours around any bump. Now one pass: (straightening only for a base with no line
  band) and an unsharp mask of a quarter texel clamped to the local range within a quarter texel, weighted
  by CE's own strong edges. It cannot create a level crossing that is not already there.
- terraced gradients: CE ramps (low-band gradient, contrast still growing from a 3- to a 7-texel window,
  little mid-band texture) get neither detail nor crisping.
- speckle in dark gaps: detail scales with the local level below 15 percent luminance, and the sum is held
  inside the base's local range (half a texel) plus a quarter of the detail amplitude: no new extremes.
"""
import math
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invented_check as IC  # noqa: E402
import texlib as T  # noqa: E402

Q = 24  # exemplar patch size in 4x px (6 texels)


def gw(a, s, wrap):
    return T.gblur(a, s, wrap)


def up(a, f, wrap, interp=cv2.INTER_LINEAR):
    return IC._up(a, f, wrap, interp) if f > 1 else a


def resize_to(a, H, W, wrap, interp=cv2.INTER_LANCZOS4):
    f = H // a.shape[0]
    if f == 1:
        return a
    return up(a, f, wrap, interp)


def tensor(L, s, wrap):
    """Structure tensor orientation (edge tangent, radians) and coherence (0..1) at scale s px."""
    gx = cv2.Sobel(L, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(L, cv2.CV_32F, 0, 1, ksize=3)
    jxx, jyy, jxy = (gw(a, s, wrap) for a in (gx * gx, gy * gy, gx * gy))
    tr = jxx + jyy + 1e-12
    det = np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2)
    coh = (det / tr) ** 2
    theta = 0.5 * np.arctan2(2 * jxy, jxx - jyy) + np.pi / 2
    return theta.astype(np.float32), coh.astype(np.float32)


def ce_regions(ce, wrap):
    """CE-resolution weights: wtex (CE has texture here), ramp (smooth gradient), edge (strong CE edge)."""
    emid, grad, Llo = IC.regions(ce, wrap)
    Lc = T.lum(ce)
    k3, k7 = np.ones((3, 3), np.uint8), np.ones((7, 7), np.uint8)
    con = (cv2.dilate(Lc, k3) - cv2.erode(Lc, k3)) * 255
    con7 = (cv2.dilate(Lc, k7) - cv2.erode(Lc, k7)) * 255
    ramp = ((grad >= 2.0) & (con7 >= 2.2 * con) & (emid < np.maximum(IC.E_LOW, 0.5 * grad))).astype(np.float32)
    ramp = np.clip(gw(cv2.dilate(ramp, k3), 0.7, wrap) * 1.5, 0, 1)
    wtex = IC.smoothstep(0.75 * IC.E_LOW, 2.5 * IC.E_LOW, emid) * (1 - ramp)
    edge = IC.smoothstep(IC.CONTRAST_MIN, 2.5 * IC.CONTRAST_MIN, con) * (1 - ramp)
    edge = np.clip(gw(cv2.dilate(edge, k3), 0.7, wrap), 0, 1)
    return wtex.astype(np.float32), ramp.astype(np.float32), edge.astype(np.float32), Llo


def edge_pass(a, F, wrap, edge_t, straighten):
    """One edge pass on the luminance (grey offset): optional straightening (structure tensor line
    integral, mars.straighten) then an unsharp mask of F/4 px clamped to the local range within F/4 px."""
    import mars as M
    L = T.lum(a)
    L1 = L
    if straighten:
        Ls, _ = M.straighten(L, F, wrap)
        L1 = L + edge_t * (Ls - L)
    s = max(0.7, F / 4)
    # aligned to the base's own edge: only where its gradient (at F/4) is near the local maximum (within one
    # texel), so the grain beside an edge is not sharpened into extra ridges
    Lg = gw(L1, s, wrap)
    gm = np.hypot(cv2.Sobel(Lg, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(Lg, cv2.CV_32F, 0, 1, ksize=3))
    loc = cv2.dilate(gm, np.ones((2 * F + 1, 2 * F + 1), np.uint8))
    edge_t = edge_t * IC.smoothstep(0.25, 0.6, gm / (loc + 1e-6))
    edge_t = gw(edge_t, 0.5, wrap)
    Lu = L1 + 1.0 * (L1 - Lg)
    r = max(1, F // 4)
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    Lu = np.clip(Lu, cv2.erode(L1, ker, borderType=cv2.BORDER_REFLECT), cv2.dilate(L1, ker, borderType=cv2.BORDER_REFLECT))
    L2 = L1 + edge_t * (Lu - L1)
    return np.clip(a + (L2 - L)[..., None], 0, 1), edge_t


def synth(base4, F, wrap, labels4, keep4, seed=7, edge4=None):
    """Unit-rms texture for each octave above the 4x base's Nyquist (list, target resolution), the base's
    top-band rms (4x) and its local octave ratio (4x)."""
    rng = np.random.default_rng(seed)
    L4 = T.lum(base4)
    h4, w4 = L4.shape
    H, W = h4 * F // 4, w4 * F // 4
    b1 = gw(L4, 0.6, wrap) - gw(L4, 1.2, wrap)
    b2 = gw(L4, 1.2, wrap) - gw(L4, 2.4, wrap)
    e1 = np.sqrt(gw(b1 * b1, 3.0, wrap) + 1e-10)
    e2 = np.sqrt(gw(b2 * b2, 3.0, wrap) + 1e-10)
    ratio = gw(np.clip(e1 / e2, 0.3, 0.75), 4.0, wrap)
    theta, coh = tensor(gw(L4, 0.6, wrap), 3.0, wrap)
    n_oct = max(0, int(round(math.log2(F / 4))))
    octs = []
    if n_oct == 0:
        return octs, e1, ratio
    # source pools per material: textured spots (top 60 percent of e1 in the material), away from keep
    st = Q // 4
    ys, xs = np.mgrid[Q:h4 - 2 * Q:st, Q:w4 - 2 * Q:st]
    cy, cx = ys + Q // 2, xs + Q // 2
    pools = {}
    for m in np.unique(labels4):
        selm = (labels4[cy, cx] == m) & (keep4[cy, cx] < 0.3)
        if edge4 is not None:  # no exemplar from an edge: its own ridge, turned along the destination's
            selm &= edge4[cy, cx] < 0.2  # edge, draws parallel ghost lines beside it
        if selm.sum() < 8:
            continue
        thr = np.quantile(e1[cy, cx][selm], 0.4)
        ok = selm & (e1[cy, cx] >= thr)
        c = coh[cy, cx][ok]
        pools[int(m)] = {"coh": np.stack([ys[ok][c > 0.4], xs[ok][c > 0.4]], -1),
                         "iso": np.stack([ys[ok][c <= 0.4], xs[ok][c <= 0.4]], -1),
                         "all": np.stack([ys[ok], xs[ok]], -1)}
    if not pools:
        return octs, e1, ratio
    fallback = max(pools, key=lambda m: len(pools[m]["all"]))
    big = int(Q * 1.5) | 1  # crop for rotation, centre-cropped to Q afterwards
    pb = big // 2 + 1
    Lp = np.pad(b1, pb, mode="wrap" if wrap else "reflect")
    e1p = np.pad(e1, pb, mode="wrap" if wrap else "reflect")
    for j in range(1, n_oct + 1):
        s = (F / 4) / 2 ** j  # target px per 4x px for octave j
        Pt = int(round(Q * s))
        Pt += Pt % 2
        a = np.sin(np.pi * (np.arange(Pt) + 0.5) / Pt).astype(np.float32)
        win = np.outer(a, a)
        acc = np.zeros((H + 2 * Pt, W + 2 * Pt), np.float32)
        w2 = np.zeros_like(acc)
        npatch = int(20 * (H + Pt) * (W + Pt) / (Pt * Pt))
        cys = rng.integers(-Pt // 2, H + Pt // 2, npatch)
        cxs = rng.integers(-Pt // 2, W + Pt // 2, npatch)
        f4 = 4.0 / F
        for yc0, xc0 in zip(cys, cxs):
            y4 = min(max(int(yc0 * f4), 0), h4 - 1)
            x4 = min(max(int(xc0 * f4), 0), w4 - 1)
            m = int(labels4[y4, x4])
            pool = pools.get(m, pools[fallback])
            cd = coh[y4, x4]
            if cd > 0.4 and len(pool["coh"]):
                sy, sx = pool["coh"][rng.integers(len(pool["coh"]))]
                ysc, xsc = sy + Q // 2, sx + Q // 2
                crop = Lp[ysc + pb - big // 2:ysc + pb + big // 2 + 1, xsc + pb - big // 2:xsc + pb + big // 2 + 1]
                ang = np.degrees(theta[y4, x4] - theta[ysc, xsc])
                R = cv2.getRotationMatrix2D((big // 2, big // 2), -ang, 1.0)
                crop = cv2.warpAffine(crop, R, (big, big), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT)
                o = big // 2 - Q // 2
                patch = crop[o:o + Q, o:o + Q]
            else:
                src = pool["iso"] if len(pool["iso"]) else pool["all"]
                sy, sx = src[rng.integers(len(src))]
                ysc, xsc = sy + Q // 2, sx + Q // 2
                patch = Lp[sy + pb:sy + pb + Q, sx + pb:sx + pb + Q]
                k = rng.integers(4)
                patch = np.rot90(patch, k) if k else patch
                if rng.random() < 0.5:
                    patch = patch[:, ::-1]
            patch = patch / max(float(e1p[ysc + pb, xsc + pb]), 1e-4)
            if Pt != Q:
                patch = cv2.resize(np.ascontiguousarray(patch), (Pt, Pt), interpolation=cv2.INTER_CUBIC)
            ya, xa = yc0 - Pt // 2 + Pt, xc0 - Pt // 2 + Pt
            acc[ya:ya + Pt, xa:xa + Pt] += patch * win
            w2[ya:ya + Pt, xa:xa + Pt] += win * win
        u = acc / np.sqrt(w2 + 0.05)
        u = u[Pt:Pt + H, Pt:Pt + W]
        # band-limit to the octave (the rotation and cubic resampling leak a little below it)
        lam = 2.0 * (F / 4) / 2 ** j  # px: the octave's centre wavelength is ~2x this
        u = u - gw(u, 1.2 * lam, wrap)
        u /= float(np.sqrt((u ** 2).mean()) + 1e-6)
        octs.append(u)
    return octs, e1, ratio


def detail2(ce, F, base4, kind, wrap, P=None, keep=None, redrawn=False, seed=7, gain=1.0, no_edge=None,
            edge_quiet=True):
    h, w = ce.shape[:2]
    H, W = h * F, w * F
    if keep is None:
        keep = np.zeros((h, w), np.float32)
    wtex, ramp, edge, Llo = ce_regions(ce, wrap)
    base_t = np.clip(resize_to(base4, H, W, wrap), 0, 1)
    a = T.backproject_low(base_t, ce, 4, wrap=wrap)
    # one edge pass, on CE's strong edges, never on ramps or under the keep mask
    edge_t = np.zeros((H, W), np.float32)
    if kind in ("hard", "cov"):
        ne = 0 if no_edge is None else np.clip(no_edge, 0, 1)
        edge_t = up(edge * (1 - keep) * (1 - ne), F, wrap)
        a, edge_t = edge_pass(a, F, wrap, edge_t, straighten=not redrawn)
    # octaves above the base's Nyquist
    labels4 = np.zeros((h * 4, w * 4), np.int32)
    if P is not None:
        labels4 = up(P.argmax(-1).astype(np.uint8), 4, wrap, cv2.INTER_NEAREST).astype(np.int32)
    keep4 = up(keep, 4, wrap)
    # CE's strong edges and their flanks (about 1.5 texels each side): the base's top-band energy there is
    # the edge's own, not texture, and detail laid there along the edge reads as ripples beside it
    flank = np.clip(gw(cv2.dilate(edge, np.ones((3, 3), np.uint8)), 0.7, wrap) * 1.5, 0, 1) if edge_quiet else np.zeros_like(edge)
    octs, e1, ratio = synth(base4, F, wrap, labels4, keep4, seed=seed, edge4=up(flank, 4, wrap) if edge_quiet else None)
    det = np.zeros((H, W), np.float32)
    amp0 = up(e1, F // 4, wrap) if F > 4 else e1
    rat = up(ratio, F // 4, wrap) if F > 4 else ratio
    wt = up(wtex * (1 - keep) * (1 - flank), F, wrap) * (1 - 0.6 * edge_t)
    La = T.lum(a)
    dark = np.clip(gw(La, F / 2, wrap) / 0.15, 0, 1)
    amp = amp0 * wt * dark * gain
    for j, u in enumerate(octs, 1):
        det += u * amp * rat ** j
    # no new extremes: the sum stays inside the base's local range (F/2 px) plus a quarter of the amplitude
    r = max(1, F // 2)
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    lo = cv2.erode(La, ker, borderType=cv2.BORDER_REFLECT) - 0.25 * amp
    hi = cv2.dilate(La, ker, borderType=cv2.BORDER_REFLECT) + 0.25 * amp
    Ln = np.clip(La + det, lo, hi)
    alb = np.clip(a + (Ln - La)[..., None], 0, 1)
    alb = T.backproject_low(alb, ce, 3, wrap=wrap)
    # keep CE's shape: plain bicubic under small lit decals and self-illumination
    kt = up(keep, F, wrap)[..., None]
    if kt.max() > 0:
        alb = kt * IC.plain(ce, F, wrap) + (1 - kt) * alb
    return {"albedo": np.clip(alb, 0, 1), "det": Ln - La, "edge": edge_t, "amp": amp, "keep": keep}
