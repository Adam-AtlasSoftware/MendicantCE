"""(b)/(c) approach C: "smart material" re-authoring driven by CE's own structure.

What a modern texture artist does on top of a clean base (Substance-style layering), with every mask
derived from the CE map itself, every colour taken from CE's own palette of that material, and CE's
lower band restored at the end (back-projection), so the map still averages to CE:

  height   = CE's bump map integrated (Frankot-Chellappa) + a plate/groove term from the redrawn edges
             + a fine procedural micro layer (fBm in the octaves CE could not hold)
  wear     = convex edges of that height x breakup noise -> towards the material's own bright colour
             (CE's 97th percentile of that material), lower roughness (bare paint edge / metal)
  grime    = concave creases x breakup noise -> towards the material's own dark colour (3rd percentile)
  scratches= sparse anti-aliased strokes (hard surfaces), axis-biased on BSP panels, slight lighten
  normal / roughness / height written alongside.

  smart(ce, F, base, kind, wrap, bump=None, mp=None, ...) -> dict like mars()
"""
import math
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mars as M  # noqa: E402
import texlib as T  # noqa: E402

gw = M.gw


def fbm(H, W, lam_min, lam_max, rng, wrap=True):
    """Band-limited noise with wavelengths lam_min..lam_max px (log-uniform octaves), unit std."""
    out = np.zeros((H, W), np.float32)
    lam = lam_max
    amp = 1.0
    while lam >= lam_min:
        n = rng.standard_normal((H, W)).astype(np.float32)
        b = gw(n, lam / 6, wrap) - gw(n, lam / 3, wrap)
        out += amp * b / (b.std() + 1e-6)
        lam /= 2
        amp *= 0.7
    return out / (out.std() + 1e-6)


def scratch_field(H, W, F, rng, count, axis_bias=0.6, len_px=(1.5, 12), width=1.0):
    """Anti-aliased strokes, lengths in CE texels (x F px). Half of them axis-aligned (+-6 deg) if axis_bias."""
    img = np.zeros((H * 1, W * 1), np.float32)
    for _ in range(count):
        L = math.exp(rng.uniform(math.log(len_px[0] * F), math.log(len_px[1] * F)))
        if rng.random() < axis_bias:
            th = rng.choice([0, math.pi / 2]) + rng.normal(0, math.radians(4))
        else:
            th = rng.uniform(0, math.pi)
        x0, y0 = rng.uniform(0, W), rng.uniform(0, H)
        x1, y1 = x0 + L * math.cos(th), y0 + L * math.sin(th)
        v = float(rng.uniform(0.3, 1.0))
        cv2.line(img, (int(x0 * 16), int(y0 * 16)), (int(x1 * 16), int(y1 * 16)), v, max(1, int(width)),
                 cv2.LINE_AA, shift=4)
    return img


def material_palette(ce, labels, k):
    """Per material: CE's own bright (97th pct L) and dark (3rd pct L) colours."""
    lab = T.srgb_to_lab(ce)
    pal = {}
    for m in range(k):
        sel = labels == m
        if sel.sum() < 8:
            continue
        L = lab[..., 0][sel]
        rgb = ce[sel]
        hi = rgb[L >= np.percentile(L, 97)].mean(0)
        lo = rgb[L <= np.percentile(L, 3)].mean(0)
        pal[m] = (hi.astype(np.float32), lo.astype(np.float32))
    return pal


PARAMS = {
    #           wear  grime  scratches/MPx  micro_amp  height_from_L  rough0 rough_wear
    "hard":    (0.35, 0.25, 250, 0.25, 0.6, 0.45, 0.25),
    "cov":     (0.30, 0.25, 250, 0.15, 0.5, 0.30, 0.18),
    "organic": (0.25, 0.45, 0, 0.9, 1.2, 0.85, 0.75),
    "cloth":   (0.15, 0.35, 0, 0.5, 0.7, 0.80, 0.75),
    "skin":    (0.05, 0.15, 0, 0.3, 0.3, 0.50, 0.50),
}


def smart(ce, F, base, kind="hard", wrap=True, bump=None, mp=None, k=4, seed=11, with_exemplar=True,
          axis_bias=0.6, strength=1.0, ce_clean=None):
    rng = np.random.default_rng(seed)
    h, w = ce.shape[:2]
    H, W = h * F, w * F
    wear_a, grime_a, n_scr, micro, hL, r0, rw = PARAMS[kind]
    wear_a, grime_a = wear_a * strength, grime_a * strength
    # 1. clean base + redrawn edges (+ the exemplar octaves of approach B, at reduced gain)
    mk = "hard" if kind in ("hard", "cov") else "organic"
    r = M.mars(ce, F, base, kind=mk if kind != "skin" else "skin", wrap=wrap, mp=mp, bump=None, k=k,
               detail_gain=0.6 if with_exemplar else 0.0, crisp=kind in ("hard", "cov"), ce_clean=ce_clean)
    alb, labels, ew = r["albedo"], r["labels"], r["edge"]
    lab_up = cv2.resize(labels.astype(np.uint8), (W, H), interpolation=cv2.INTER_NEAREST)
    La = T.lum(alb)
    # 2. structure height (texel units of the output map)
    hgt = np.zeros((H, W), np.float32)
    if bump is not None:
        hb = M.height_from_normal(bump)
        hgt += cv2.resize(hb, (W, H), interpolation=cv2.INTER_CUBIC) * (W / bump.shape[1])
    hgt += hL * F * 4 * (gw(La, 0.6 * F, wrap) - gw(La, 6 * F, wrap))
    # 3. curvature
    hs = gw(hgt, 0.6 * F, wrap)
    lap = cv2.Laplacian(hs, cv2.CV_32F, ksize=3)
    sd = lap.std() + 1e-6
    convex = np.clip((-lap / sd - 0.6) / 1.6, 0, 1)
    concave = np.clip((lap / sd - 0.4) / 1.6, 0, 1)
    if kind in ("hard", "cov"):
        convex = np.maximum(convex, ew * 0.6)  # redrawn plate edges wear first
    # 4. breakup noise
    n1 = fbm(H, W, F * 0.5, F * 6, rng, wrap)
    n2 = fbm(H, W, F * 1.0, F * 10, rng, wrap)
    wear = np.clip(convex * 1.4 + 0.45 * n1 - 0.55, 0, 1)
    wear = wear * wear * (3 - 2 * wear)
    grime = np.clip(concave * 1.2 + 0.35 * n2 - 0.2, 0, 1)
    pal = material_palette(ce, labels, k)
    hi = np.zeros((H, W, 3), np.float32)
    lo = np.zeros((H, W, 3), np.float32)
    for m, (ph, pl) in pal.items():
        sel = lab_up == m
        hi[sel] = ph
        lo[sel] = pl
    out = alb + (wear * wear_a)[..., None] * (hi - alb)
    out = out + (grime * grime_a)[..., None] * (lo - out)
    # 5. scratches (hard surfaces)
    scr = np.zeros((H, W), np.float32)
    if n_scr:
        count = int(n_scr * H * W / 1e6)
        scr = scratch_field(H, W, F, rng, count, axis_bias=axis_bias)
        scr *= np.clip(0.5 + 0.5 * n1, 0, 1) * (1 - 0.7 * grime)
        out = out + (0.35 * scr)[..., None] * (hi - out)
    # 6. micro height (octaves CE could not hold), modulated by CE's own local detail level
    mh = fbm(H, W, 2.0, max(4.0, F * 0.75), rng, wrap) * micro
    hgt = hgt + mh * F * 0.25 - scr * F * 0.4 - grime * F * 0.3
    out = T.backproject_low(np.clip(out, 0, 1), ce, 4, wrap=wrap)
    nrm = M.normal_from_height(hgt, strength=0.5 * 4 / F, wrap=wrap)
    rough = np.clip(r0 - rw * wear * 0.6 + 0.2 * grime - 0.15 * scr + 0.05 * mh, 0.05, 1)
    return {"albedo": out, "height": hgt, "normal": nrm, "rough": rough, "labels": labels, "edge": ew,
            "wear": wear, "grime": grime, "scratch": scr, "stats": r["stats"]}
