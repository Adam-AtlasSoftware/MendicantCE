"""PRISM v2 "Bold" texture prototype: the Enhanced fusion map (4x) given material definition that reads at play
distance, CE's layout, colours and designs kept. Each stage is measured on its own, cumulatively, against the
faithfulness checks (fusion.checks / passes, invented_check.check, the palette per material class) and against
visibility (texvis_bands.report: the difference that survives the screen's mip at 0.54x, 1.45x, 2.9x, 3.9x).

  python -I texvis_bold.py <key> [...]        (keys of texvis_bands.KEYS)

Stages (all in CIE L* unless noted, hue and chroma untouched except S3's own colours):
  S0  Enhanced (tiers.py, fusion 4x): the reference.
  S1  release: the upscaler's own detail above CE's band at gain (1 + g0), and the 1-4 texel band (edge and
      highlight definition, B2/B3) at gain (1 + g1), per material class; the keep-CE-shape mask (glyphs, lights)
      untouched.
  S2  material contrast: the 4-16 texel band (B1: the value step between plate and suit, painted form
      shading) at gain (1 + g2) per class: crisper value contrast between materials, same colours.
  S3  weathering (behind the Weathering switch): convex plate edges (from the class map's plate borders and the
      L*-derived height's curvature) move toward the material's own bright colour (CE's 97th percentile of that
      class), creases toward its dark (3rd percentile); 1-1.5 texels wide, breakup noise.
  surface map (engine: <bitmap>.surface.png, RG normal, B occlusion, A roughness): height from the L* bands of
      S2 per class (the painted relief and the upscaler's fine detail), normal at the 4x map's frequency;
      roughness per class (paint 0.5, smooth polymer 0.42, fabric 0.85, skin 0.6) less CE's specular mask and
      worn edges, plus creases; occlusion from the height's cavities.
Outputs (local only: CE pixels): <work>/textures/out/texvis/bold/<key>/{s1,s2,s3}.png, surface.png, metrics.json.
"""
import json
import os
import sys
import time

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fusion as FU  # noqa: E402
import invented_check as IC  # noqa: E402
import texlib as T  # noqa: E402
import texvis_bands as TB  # noqa: E402
import tier_cache as TC  # noqa: E402
import tier_recipe as R  # noqa: E402
import tiers as TI  # noqa: E402

OUT = os.path.join(T.WORK, "out/texvis/bold")
CI = {c: i for i, c in enumerate(FU.CLASSES)}
# per class: g0 (above CE), g1 (1-4 texels), g2 (4-16 texels), wear, grime, roughness, height gain
CLS = {"smooth_paint": (1.0, 0.55, 0.30, 0.45, 0.35, 0.42, 1.0),
       "mottled_paint": (1.0, 0.60, 0.30, 0.50, 0.35, 0.50, 1.0),
       "stone": (0.8, 0.45, 0.25, 0.30, 0.35, 0.80, 1.0),
       "grass": (0.5, 0.30, 0.15, 0.0, 0.25, 0.85, 0.6),
       "fabric": (0.7, 0.40, 0.20, 0.0, 0.40, 0.85, 1.4),
       "skin": (0.3, 0.15, 0.08, 0.0, 0.10, 0.60, 0.4)}
GAIN = float(os.environ.get("BOLD_GAIN", "1.0"))
HSCALE = float(os.environ.get("BOLD_HEIGHT", "0.5"))  # height gain of the texture-derived normal (1.0: tilt mean 9-19 deg; 0.5: about half)


def class_P(key, tag, ce, mp_png):
    r = dict(R.BASES[tag])
    h, w = ce.shape[:2]
    mp = None
    if mp_png:
        mp = np.asarray(Image.open(mp_png).convert("RGBA")).astype(np.float32)[..., :3] / 255
        if mp.shape[:2] != (h, w):
            mp = cv2.resize(mp, (w, h), interpolation=cv2.INTER_LINEAR)
    r["mp"], r["mp_path"] = mp, mp_png
    if mp is not None and mp_png and np.asarray(Image.open(mp_png)).shape[:2] != (h, w):
        p = os.path.join(OUT, key, "mp_at_base_size.png")
        T.save(p, mp)
        r["mp_path"] = p
    keep = IC.keep_mask(ce, False, mp, decals=True)
    if r["visor"]:
        keep = np.maximum(keep, TI.visor_weight(h, w))
    r["keep"] = keep
    TI.SPEC[key] = r
    FU.CFG[key] = (R.MAX_F, False, "char" if r["domain"] == "cov" else r["domain"], r["kind"], FU.GENTLE, None)
    P, how = TI.class_maps(key, ce, False)
    return P, keep, mp, how


def per_class(P4, col):
    v = np.array([CLS[c][col] for c in FU.CLASSES], np.float32)
    return (P4 * v).sum(-1)


def g(a, s):
    return cv2.GaussianBlur(a, (0, 0), s, borderType=cv2.BORDER_REFLECT) if s > 0 else a


def set_L(rgb, Lnew):
    lab = T.srgb_to_lab(rgb).astype(np.float32)
    lab[..., 0] = np.clip(Lnew, 0, 100)
    return np.clip(lab_to_srgb(lab), 0, 1)


def lab_to_srgb(lab):
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    fy = (L + 16) / 116
    fx, fz = fy + a / 500, fy - b / 200
    d = 6 / 29

    def finv(t):
        return np.where(t > d, t ** 3, 3 * d * d * (t - 4 / 29))
    X, Y, Z = 0.95047 * finv(fx), finv(fy), 1.08883 * finv(fz)
    M = np.array([[3.2404542, -1.5371385, -0.4985314], [-0.9692660, 1.8760108, 0.0415560],
                  [0.0556434, -0.2040259, 1.0572252]], np.float32)
    lin = np.stack([X, Y, Z], -1) @ M.T
    lin = np.clip(lin, 0, 1)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055).astype(np.float32)


def palette_dE(img, ce, P):
    """per dominant class (>= 3 % of the map): CIEDE2000 between the class's mean Lab in CE and in the result
    boxed to CE size"""
    F = img.shape[0] // ce.shape[0]
    d = T.boxdown(img, F)
    lc, ld = T.srgb_to_lab(ce), T.srgb_to_lab(d)
    out = {}
    for i, c in enumerate(FU.CLASSES):
        w = P[..., i]
        if w.mean() < 0.03:
            continue
        mc = (lc * w[..., None]).sum((0, 1)) / w.sum()
        md = (ld * w[..., None]).sum((0, 1)) / w.sum()
        out[c] = float(T.ciede2000(mc[None, None], md[None, None])[0, 0])
    return out


def definition(img, ce, P, m=1.45):
    """what reads at magnification m (screen px per CE texel): L* rms in the 1-16 texel band of the screen-
    filtered map, and the L* step between the two largest classes (their mean L* difference)"""
    F = img.shape[0] // ce.shape[0]
    s = TB.screen_sim(img, F, m)
    L = TB.Lstar(s)
    band = g(L, 0.5 * F) - g(L, 8 * F)
    P4 = cv2.resize(P, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_LINEAR)
    big = np.argsort(-P.mean((0, 1)))[:2]
    mL = [float((L * P4[..., i]).sum() / P4[..., i].sum()) for i in big]
    return {"band_rms": float(band.std()), "class_step": abs(mL[0] - mL[1]), "classes": [FU.CLASSES[i] for i in big]}


def measure(img, ce, P, mp, keep):
    st = FU.checks(img, ce)
    ic = IC.check(img, ce, False, mp=mp, keep=keep)
    rep = TB.report(img, ce)
    return {"style": st, "style_fails": FU.passes(st), "invented_fails": ic["fails"],
            "invented": {k: ic[k] for k in ("excess", "weave", "period", "contour", "banding", "speckle", "decal")},
            "palette_dE": palette_dE(img, ce, P), "screen": rep["screen"], "bands": rep["bands"],
            "definition": {"m1.45": definition(img, ce, P, 1.45), "m2.90": definition(img, ce, P, 2.9),
                           "m0.54": definition(img, ce, P, 0.54)}}


def surface(img, P4, mp4, wear, grime, keep4, F):
    """RGBA: RG normal (y up the picture), B occlusion, A roughness"""
    L = (TB.Lstar(img) / 100).astype(np.float32)
    hgain = per_class(P4, 6)
    # height: painted relief (2-16 texels) + fine detail (< 1 texel) from the map, in texels of the 4x map
    h = (g(L, 0.5 * F) - g(L, 8 * F)) * 6 * F + (L - g(L, 0.5 * F)) * 2.5 * F
    h = HSCALE * h * hgain * (1 - keep4) - grime * 0.6 * F
    h = g(h.astype(np.float32), 0.6)
    gx = cv2.Sobel(h, cv2.CV_32F, 1, 0, ksize=3) / 8
    gy = cv2.Sobel(h, cv2.CV_32F, 0, 1, ksize=3) / 8
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)  # y up the picture: +v is up, rows go down
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    tilt = np.degrees(np.arccos(n[..., 2]))
    cav = np.clip((g(h, 2 * F) - h) / (0.8 * F), 0, 1)
    ao = np.clip(1 - 0.55 * cav, 0.35, 1)
    rough = per_class(P4, 5)
    if mp4 is not None:
        rough = rough - 0.25 * mp4[..., 0]  # CE's specular mask (Xbox R): polish where CE is shiny
    rough = np.clip(rough - 0.18 * wear + 0.15 * grime + 0.1 * cav, 0.08, 1)
    rough = rough * (1 - keep4) + 1.0 * keep4  # glyphs and lights: no highlight
    rgba = np.stack([n[..., 0] * 0.5 + 0.5, n[..., 1] * 0.5 + 0.5, ao, rough], -1)
    return np.clip(rgba, 0, 1), {"tilt_mean_deg": float(tilt.mean()), "tilt_p95_deg": float(np.percentile(tilt, 95)),
                                 "ao_mean": float(ao.mean()), "rough_mean": float(rough.mean())}


def fbm(H, W, lmin, lmax, rng):
    out = np.zeros((H, W), np.float32)
    lam, amp = lmax, 1.0
    while lam >= lmin:
        n = rng.standard_normal((H, W)).astype(np.float32)
        b = g(n, lam / 6) - g(n, lam / 3)
        out += amp * b / (b.std() + 1e-6)
        lam /= 2
        amp *= 0.7
    return out / (out.std() + 1e-6)


def run(k):
    key = TB.KEYS[k]
    tag = TB.TAG[key]
    od = os.path.join(OUT, k)
    os.makedirs(od, exist_ok=True)
    t0 = time.time()
    ce, _ = T.load(TB.ce_path(key))
    r = R.BASES[tag]
    mp_png = dict(TC.images(r["multi"]))[R.safe(r["multi"])] if r["multi"] else None
    P, keep, mp, how = class_P(key, tag, ce, mp_png)
    enh, _ = T.load(os.path.join(TI.OUT, key, "enh.png"))
    F = enh.shape[0] // ce.shape[0]
    H, W = enh.shape[:2]
    P4 = cv2.resize(P, (W, H), interpolation=cv2.INTER_LINEAR)
    keep4 = cv2.resize(keep, (W, H), interpolation=cv2.INTER_LINEAR)
    mp4 = cv2.resize(mp, (W, H), interpolation=cv2.INTER_LINEAR) if mp is not None else None
    free = (1 - np.clip(keep4, 0, 1))
    # no sharpening on CE's smooth ramps (terracing: the banding check): gain where CE has texture or edges
    emid, grad, _ = IC.regions(ce, False)
    tex = cv2.resize(FU.smoothstep(1.5, 4.0, emid).astype(np.float32), (W, H), interpolation=cv2.INTER_LINEAR)
    tex = np.maximum(tex, 0.25)
    res = {"key": key, "classes": how, "class_share": {c: float(P[..., i].mean()) for i, c in enumerate(FU.CLASSES)},
           "gain": GAIN}
    res["S0"] = measure(enh, ce, P, mp, keep)
    L = TB.Lstar(enh)
    # S1: above CE (< 0.5 texel) and the 1-4 texel band
    d0 = L - g(L, 0.5 * F)
    d1 = g(L, 0.5 * F) - g(L, 2 * F)
    L1 = L + GAIN * free * tex * (per_class(P4, 0) * d0 + per_class(P4, 1) * d1)
    s1 = set_L(enh, L1)
    s1 = s1 * free[..., None] + enh * (1 - free[..., None])
    res["S1"] = measure(s1, ce, P, mp, keep)
    # S2: + the 4-16 texel band
    d2 = g(L, 2 * F) - g(L, 8 * F)
    L2 = L1 + GAIN * free * per_class(P4, 2) * d2
    s2 = set_L(enh, L2)
    s2 = s2 * free[..., None] + enh * (1 - free[..., None])
    res["S2"] = measure(s2, ce, P, mp, keep)
    # S3: weathering
    rng = np.random.default_rng(11)
    hard = P4[..., CI["smooth_paint"]] + P4[..., CI["mottled_paint"]]
    # plate borders: where the hard-paint share falls off (a step in the class map), on the hard side
    hb = g(hard, 0.5 * F)
    border = np.clip(np.hypot(cv2.Sobel(hb, cv2.CV_32F, 1, 0), cv2.Sobel(hb, cv2.CV_32F, 0, 1)) * F / 4, 0, 1) * hard
    Lh = g(TB.Lstar(s2).astype(np.float32), 0.6 * F)
    lap = cv2.Laplacian(g(Lh, 0.8 * F), cv2.CV_32F, ksize=3)
    sd = lap.std() + 1e-6
    convex = np.clip((-lap / sd - 0.8) / 1.6, 0, 1)
    concave = np.clip((lap / sd - 0.6) / 1.6, 0, 1)
    n1, n2 = fbm(H, W, F * 0.5, F * 6, rng), fbm(H, W, F, F * 10, rng)
    wear = np.clip(np.maximum(convex * hard, 0.8 * border) * 1.4 + 0.45 * n1 - 0.55, 0, 1)
    wear = wear * wear * (3 - 2 * wear)
    grime = np.clip(concave * 1.2 + 0.35 * n2 - 0.35, 0, 1)
    hi = np.zeros((H, W, 3), np.float32)
    lo = np.zeros((H, W, 3), np.float32)
    lab_ce = T.srgb_to_lab(ce)
    cls = P.argmax(-1)
    cls4 = cv2.resize(cls.astype(np.uint8), (W, H), interpolation=cv2.INTER_NEAREST)
    for i, c in enumerate(FU.CLASSES):
        sel = cls == i
        if sel.sum() < 16:
            continue
        Lc = lab_ce[..., 0][sel]
        rgb = ce[sel]
        hi[cls4 == i] = rgb[Lc >= np.percentile(Lc, 97)].mean(0)
        lo[cls4 == i] = rgb[Lc <= np.percentile(Lc, 3)].mean(0)
    wa, ga = per_class(P4, 3) * GAIN, per_class(P4, 4) * GAIN
    s3 = s2 + (wear * wa * free)[..., None] * (hi - s2)
    s3 = s3 + (grime * ga * free)[..., None] * (lo - s3)
    s3 = np.clip(s3, 0, 1)
    res["S3"] = measure(s3, ce, P, mp, keep)
    res["S3"]["wear_share"] = float((wear * wa > 0.1).mean())
    res["S3"]["grime_share"] = float((grime * ga > 0.1).mean())
    for n, im in (("s1", s1), ("s2", s2), ("s3", s3)):
        T.save(os.path.join(od, n + ".png"), im)
    for n, im in (("wear", wear * wa), ("grime", grime * ga)):
        T.save(os.path.join(od, n + ".png"), np.clip(im, 0, 1))
    srf, sinfo = surface(s2, P4, mp4, wear * wa, grime * ga, keep4, F)
    Image.fromarray((srf * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(od, "surface.png"))
    srf3, _ = surface(s3, P4, mp4, wear * wa, grime * ga, keep4, F)
    Image.fromarray((srf3 * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(od, "surface_weathered.png"))
    res["surface"] = sinfo
    res["seconds"] = round(time.time() - t0)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    for s in ("S0", "S1", "S2", "S3"):
        m = res[s]
        print(k, s, "dE %.2f deL %.3f hue %.2f ssim %.4f grid %.2f inv %.4f slope %.2f | fails %s %s | pal %s | screen dE 0.54/1.45/2.9: %.2f %.2f %.2f | band rms@1.45 %.2f step %.1f" % (
            m["style"]["de_mean"], m["style"]["deL_mean"], m["style"]["hue_deg"], m["style"]["ssim_L"], m["style"]["grid"],
            m["style"]["invented_edges"], m["style"]["slope_hi"], m["style_fails"], m["invented_fails"],
            {a: round(b, 2) for a, b in m["palette_dE"].items()},
            m["screen"]["m0.54"]["dE00_mean"], m["screen"]["m1.45"]["dE00_mean"], m["screen"]["m2.90"]["dE00_mean"],
            m["definition"]["m1.45"]["band_rms"], m["definition"]["m1.45"]["class_step"]), flush=True)
    return res


def resurface(k):
    """surface maps again from the saved S2/S3 maps (another BOLD_HEIGHT), without the measures"""
    key = TB.KEYS[k]
    tag = TB.TAG[key]
    od = os.path.join(OUT, k)
    ce, _ = T.load(TB.ce_path(key))
    r = R.BASES[tag]
    mp_png = dict(TC.images(r["multi"]))[R.safe(r["multi"])] if r["multi"] else None
    P, keep, mp, _ = class_P(key, tag, ce, mp_png)
    s2, _ = T.load(os.path.join(od, "s2.png"))
    s3, _ = T.load(os.path.join(od, "s3.png"))
    H, W = s2.shape[:2]
    F = H // ce.shape[0]
    P4 = cv2.resize(P, (W, H), interpolation=cv2.INTER_LINEAR)
    keep4 = cv2.resize(keep, (W, H), interpolation=cv2.INTER_LINEAR)
    mp4 = cv2.resize(mp, (W, H), interpolation=cv2.INTER_LINEAR) if mp is not None else None
    wear = np.asarray(Image.open(os.path.join(od, "wear.png")), np.float32) / 255
    grime = np.asarray(Image.open(os.path.join(od, "grime.png")), np.float32) / 255
    res = json.load(open(os.path.join(od, "metrics.json")))
    for n, im in (("surface", s2), ("surface_weathered", s3)):
        srf, info = surface(im, P4, mp4, wear, grime, keep4, F)
        Image.fromarray((srf * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(od, n + ".png"))
        if n == "surface":
            res["surface"] = dict(info, height_scale=HSCALE)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(k, res["surface"], flush=True)


if __name__ == "__main__":
    if sys.argv[1] == "--surface-only":
        for k in sys.argv[2:]:
            resurface(k)
    else:
        for k in sys.argv[1:]:
            run(k)
