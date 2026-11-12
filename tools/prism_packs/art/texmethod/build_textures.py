"""PRISM v2 prototype: "max" tier maps for the Chief (third person) and the first-person Assault Rifle.

  python -I build_textures.py [--only chief|ar] [--reuse]

Per texture (the texture method's recommendation for armour and hard-surface weapons):
  albedo   B2: ML 4x (general_wdn_v3) + Lanczos to 4096 + low-band back-projection (A'), straightened
           and redrawn line edges, CE's own grain carried down the octaves (no weathering: that is the
           separate Weathering switch, built as albedo_weathered.png from C, not used by default)
  surface  RGBA at 2048 (the overlay's image path uploads at most 32 MB a level: 2048^2 RGBA8 is
           16 MB): RG the tangent-space normal's x and y (y up the picture, 0.5 flat) from C's height,
           B the ambient occlusion (1 open) from the height's concavity, A the roughness (0..1)
  multi    the multipurpose map at 2048, Lanczos 4x then a guided filter (He et al.) by the redrawn
           albedo's luminance, so the change-colour and reflection masks' edges follow the redrawn
           edges instead of 512-texel bilinear steps; each channel back-projected to CE's low band
Outputs (local only: game pixels) under <work>/textures/proto/<name>/, metrics.json with the colour checks.
"""
import argparse
import json
import os
import sys
import time

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mars as M  # noqa: E402
import smart as S  # noqa: E402
import texlib as T  # noqa: E402

EX = T.E.extract("b30", "bitmaps")
OUT = T.E.work("textures", "proto")
STUDY = os.path.join(T.WORK, "out", "final")
CFG = {
    "chief": {"base": "characters/cyborg/bitmaps/cyborg.png", "multi": "characters/cyborg/bitmaps/cyborg multipurpose.png",
              "F": 8, "model": "general_wdn_v3", "kind": "hard", "study": "chief_diffuse"},
    "ar": {"base": "weapons/assault rifle/fp/bitmaps/assault rifle_fp.png",
           "multi": "weapons/assault rifle/fp/bitmaps/assault rifle multipurpose.png",
           "F": 8, "model": "general_wdn_v3", "kind": "hard", "study": None},
}
SURFACE = 2048
MULTI = 2048


def box_filter(a, r):
    return cv2.boxFilter(a, -1, (2 * r + 1, 2 * r + 1), borderType=cv2.BORDER_REFLECT)


def guided(I, p, r, eps):
    """He et al.'s guided filter: p filtered with I's edges"""
    mI, mp = box_filter(I, r), box_filter(p, r)
    cov = box_filter(I * p, r) - mI * mp
    var = box_filter(I * I, r) - mI * mI
    a = cov / (var + eps)
    b = mp - a * mI
    return box_filter(a, r) * I + box_filter(b, r)


def lowband_match(up, ce, sigma=0.8):
    """back-project each channel to CE's low band (texlib.backproject_low for any channel count)"""
    out = up.copy()
    f = up.shape[0] // ce.shape[0]
    for _ in range(4):
        down = cv2.resize(out, (ce.shape[1], ce.shape[0]), interpolation=cv2.INTER_AREA)
        err = cv2.GaussianBlur(ce - down, (0, 0), sigma)
        out = out + cv2.resize(err, (up.shape[1], up.shape[0]), interpolation=cv2.INTER_CUBIC)
    return np.clip(out, 0, 1)


def surface_map(height, rough, n_strength):
    """2048 RGBA: normal xy (from the height, y up the picture), AO from concavity, roughness"""
    H = height.shape[0]
    k = H // SURFACE
    h = cv2.resize(height, (SURFACE, SURFACE), interpolation=cv2.INTER_AREA) / k
    nrm = M.normal_from_height(h, strength=n_strength, wrap=False)
    # ambient occlusion: how far below its surroundings a texel lies (crevices, panel gaps)
    hs = [cv2.GaussianBlur(h, (0, 0), s) for s in (2, 6, 16)]
    depth = sum(np.clip(b - h, 0, None) * w for b, w in zip(hs, (0.5, 0.3, 0.2)))
    ao = np.clip(1.0 - depth / (np.percentile(depth, 99.5) + 1e-6) * 0.65, 0.2, 1.0)
    r = cv2.resize(rough, (SURFACE, SURFACE), interpolation=cv2.INTER_AREA)
    return np.dstack([nrm[..., 0], nrm[..., 1], ao, r]).astype(np.float32), ao


def save_rgba(path, a):
    Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(path)


def build(name, cfg, reuse):
    t0 = time.time()
    od = os.path.join(OUT, name)
    os.makedirs(od, exist_ok=True)
    ce, _ = T.load(os.path.join(EX, cfg["base"]))
    mp, _ = T.load(os.path.join(EX, cfg["multi"]))
    F = cfg["F"]
    h, w = ce.shape[:2]
    H, W = h * F, w * F
    res = {"ce_size": [w, h], "out_size": [W, H], "factor": F, "model": cfg["model"]}
    if cfg["study"] and reuse:
        sd = os.path.join(STUDY, cfg["study"], "maps")
        alb = T.load(os.path.join(sd, "B2_albedo.png"))[0]
        calb = T.load(os.path.join(sd, "C_albedo.png"))[0]
        hgt = np.asarray(Image.open(os.path.join(sd, "C_height.png")), np.float32) / 255.0
        rough = np.asarray(Image.open(os.path.join(sd, "C_rough.png")), np.float32) / 255.0
        res["source"] = "texture_method study maps (final.py), " + sd
        # (the study saved the height normalised to 0..1; its span in texels is not kept: the
        # strength below matches the study C_normal's slope RMS at 2048, 0.106)
        hscale = 53.0
    else:
        up4 = T.upscale4(cfg["model"], ce, False)
        base = np.clip(cv2.resize(up4, (W, H), interpolation=cv2.INTER_LANCZOS4), 0, 1)
        clean = T.boxdown(up4, 4)
        r = M.mars(ce, F, base, kind="hard", wrap=False, mp=mp, bump=None, source="exemplar", crisp=True,
                   ce_clean=clean)
        alb = r["albedo"]
        s = S.smart(ce, F, base, kind=cfg["kind"], wrap=False, bump=None, mp=mp, ce_clean=clean)
        calb, rough = s["albedo"], s["rough"]
        hh = s["height"]
        hgt = (hh - hh.min()) / (np.ptp(hh) + 1e-6)
        hscale = float(np.ptp(hh))
        res["source"] = "built here: A' + B2 (mars exemplar) + C (smart)"
        T.save(os.path.join(od, "albedo_A_prime.png"), T.backproject_low(base.copy(), ce, 6, wrap=False))
    T.save(os.path.join(od, "albedo.png"), alb)
    T.save(os.path.join(od, "albedo_weathered.png"), calb)
    res["albedo_fidelity"] = T.fidelity(alb, ce)
    res["albedo_weathered_fidelity"] = T.fidelity(calb, ce)
    # surface map (height in 4K texels -> slopes at 2048)
    surf, ao = surface_map(hgt * hscale, rough, n_strength=0.25)
    save_rgba(os.path.join(od, "surface.png"), surf)
    n = surf[..., :2] * 2 - 1
    res["surface"] = {"size": SURFACE, "slope_rms": float(np.sqrt((n ** 2).sum(-1).mean())),
                      "slope_p99": float(np.percentile(np.sqrt((n ** 2).sum(-1)), 99)),
                      "ao_mean": float(ao.mean()), "ao_p1": float(np.percentile(ao, 1)),
                      "rough_mean": float(surf[..., 3].mean())}
    # multipurpose at 2048, guided by the redrawn albedo
    lum = cv2.resize(T.lum(alb), (MULTI, MULTI), interpolation=cv2.INTER_AREA).astype(np.float32)
    mpl = cv2.resize(mp, (MULTI, MULTI), interpolation=cv2.INTER_LANCZOS4).astype(np.float32)
    k = MULTI // w
    g = np.dstack([guided(lum, mpl[..., c], r=max(2, k), eps=2e-3) for c in range(3)])
    g = lowband_match(np.clip(g, 0, 1), mp)
    T.save(os.path.join(od, "multi.png"), g)
    dm = cv2.resize(g, (w, h), interpolation=cv2.INTER_AREA) - mp
    res["multi"] = {"size": MULTI, "mean_abs_err_at_ce_res": [float(np.abs(dm[..., c]).mean()) for c in range(3)],
                    "max_abs_err_at_ce_res": [float(np.abs(dm[..., c]).max()) for c in range(3)]}
    res["seconds"] = round(time.time() - t0, 1)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(name, json.dumps({k: res[k] for k in ("seconds", "surface", "multi")}, default=float)[:600], flush=True)
    print(name, "albedo", {k: round(v, 4) for k, v in res["albedo_fidelity"].items() if isinstance(v, float)}, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--reuse", action="store_true")
    a = ap.parse_args()
    for name, cfg in CFG.items():
        if a.only and name not in a.only:
            continue
        build(name, cfg, a.reuse)


if __name__ == "__main__":
    main()
