"""PRISM v2 texture tiers from the fixed pipeline: Enhanced (fusion) and Max (fusion + detail2 where it passes the
invented-texture check), base and multipurpose maps, for the maps in tier_recipe.BASES.

  python -I tiers.py [--jobs N] [substring ...]

Per base map (each bitmap of the tag):
  Enhanced  fusion at 4x (fusion.fuse: CE low band, the line models only on long line-like edges, class grain,
            wdn only on skin, deblock, keep-CE-shape mask), capped at 2048 per side
  Max       detail2 on the fusion base at 8x (one octave above the base), capped at 4096; kept only when it
            passes the invented-texture check (invented_check.check), else the Enhanced map. Faces: Enhanced
            (ML only, at most 4x)
  Chief     the visor's region (from the Chief's own UVs) is a plain resample in both tiers (no armour grain)
  alpha     (DXT3 bases: the Marine helmet, the Jackal head) a plain bicubic resample of CE's alpha
Per multipurpose map (Xbox channel order R specular, G self-illumination, B colour change, A auxiliary), at
half the tier's base-map size (at least CE's size, at most 2048): R and B Lanczos then a guided filter by the
tier's base-map luminance (edges follow the upscaled base map), back-projected to CE's low band; G and A plain
resample (self-illumination keeps CE's shape and size). The PC channel reorder and DXT5 are done when the tag
is written (build_tier_mods.sh).
Outputs (local only: CE pixels): <work>/textures/out/tiers/<key>/ (enh.png, max.png, multi_enh.png,
multi_max.png, alpha_*.png, metrics.json, crops/) and <work>/textures/out/tiers/summary.json.
"""
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import detail2 as D  # noqa: E402
import fusion as FU  # noqa: E402
import invented_check as IC  # noqa: E402
import texlib as T  # noqa: E402
import tier_cache as TC  # noqa: E402
import tier_recipe as R  # noqa: E402

OUT = os.path.join(T.WORK, "out/tiers")
SPEC = {}   # key -> recipe entry (+ "mp" array), for the fusion hooks below
_orig_class_maps = FU.class_maps


def class_maps(name, ce, wrap):
    """fusion.class_maps for the tier maps: class masks with their live channel rule, the Covenant rules, faces."""
    if name not in SPEC:
        return _orig_class_maps(name, ce, wrap)
    r = SPEC[name]
    h, w = ce.shape[:2]
    ci = {c: i for i, c in enumerate(FU.CLASSES)}
    P = np.zeros((h, w, len(FU.CLASSES)), np.float32)
    cl = r["classes"]
    if r["domain"] == "face":
        P[..., ci["skin"]] = 1
        return P, "face: whole map skin"
    if cl and cl[0] == "cov_rule":
        return FU.cov_char_classes(ce, wrap, "char", r["mp_path"])
    if cl and cl[0] == "mask":
        _, fn, slots, rule = cl
        m = np.asarray(Image.open(os.path.join(R.MASKS, fn)).convert("RGBA")).astype(np.float32) / 255
        if m.shape[:2] != (h, w):
            m = cv2.resize(m, (w, h), interpolation=cv2.INTER_LINEAR)
        m = m[..., :len(slots)]
        if rule is not None and r["mp"] is not None:  # the renderer's rule: slot A where the channel is on
            ch, lo, hi = rule
            mp = r["mp"]
            if mp.shape[:2] != (h, w):
                mp = cv2.resize(mp, (w, h), interpolation=cv2.INTER_LINEAR)
            a = FU.smoothstep(lo, hi, mp[..., ch])
            rest = m[..., 1:]
            rest = rest / np.maximum(rest.sum(-1, keepdims=True), 1e-6)
            m = np.concatenate([a[..., None], rest * (1 - a[..., None])], -1)
        for i, s in enumerate(slots):
            P[..., ci[FU.MASK_CLASS[s]]] += m[..., i]
        cov = P.sum(-1)
        auto, _ = FU.auto_classes(ce, wrap, "char")
        P = P + auto * (1 - np.clip(cov, 0, 1))[..., None]
        return P / np.maximum(P.sum(-1, keepdims=True), 1e-6), "class mask " + fn + (" + rule %s" % (rule,) if rule else "")
    if r["domain"] == "cov":  # Grunts, Jackals: automatic paint classes, no human skin tones on a Covenant
        auto, how = FU.auto_classes(ce, wrap, "char")
        auto[..., ci["skin"]] = 0
        return auto / np.maximum(auto.sum(-1, keepdims=True), 1e-6), how + " (no skin: Covenant)"
    return FU.auto_classes(ce, wrap, r["domain"])


def keep_ce(name, ce, wrap):
    if name not in SPEC:
        return IC.keep_mask(ce, wrap, FU.mp_for(name), decals=FU.CFG[name][2] != "env")
    return SPEC[name]["keep"]


FU.class_maps = class_maps
FU.keep_ce = keep_ce


def mp_resized(mp, h, w):
    if mp is None or mp.shape[:2] == (h, w):
        return mp
    return cv2.resize(mp, (w, h), interpolation=cv2.INTER_LINEAR)


def guided(I, p, r, eps):
    """He et al.'s guided filter: p filtered with I's edges"""
    bf = lambda a: cv2.boxFilter(a, -1, (2 * r + 1, 2 * r + 1), borderType=cv2.BORDER_REFLECT)  # noqa: E731
    mI, mp = bf(I), bf(p)
    a = (bf(I * p) - mI * mp) / (bf(I * I) - mI * mI + eps)
    b = mp - a * mI
    return bf(a) * I + bf(b)


def lowband_match(up, ce, sigma=0.8):
    out = up.copy()
    for _ in range(4):
        down = cv2.resize(out, (ce.shape[1], ce.shape[0]), interpolation=cv2.INTER_AREA)
        err = cv2.GaussianBlur(ce - down, (0, 0), sigma)
        out = out + cv2.resize(err, (up.shape[1], up.shape[0]), interpolation=cv2.INTER_CUBIC)
    return np.clip(out, 0, 1)


def multi_size(mpw, mph, base_w):
    """half the base map's width, a power-of-two factor of the CE multipurpose map, at least 1, at most 2048"""
    f = 1
    while mpw * f * 2 <= base_w // 2 and mpw * f * 2 <= R.MULTI_CAP and mph * f * 2 <= R.MULTI_CAP:
        f *= 2
    return mpw * f, mph * f, f


def build_multi(mp_rgba, albedo):
    """multipurpose (Xbox order RGBA, CE) -> upscaled (Xbox order RGBA) guided by the tier's base map"""
    h, w = mp_rgba.shape[:2]
    W, H, f = multi_size(w, h, albedo.shape[1])
    if f == 1:
        return mp_rgba.copy(), f
    lum = cv2.resize(T.lum(albedo), (W, H), interpolation=cv2.INTER_AREA).astype(np.float32)
    lan = np.clip(cv2.resize(mp_rgba, (W, H), interpolation=cv2.INTER_LANCZOS4), 0, 1).astype(np.float32)
    out = lan.copy()
    for c in (0, 2):  # specular, colour change: edges follow the base map
        g = guided(lum, lan[..., c], r=max(2, f), eps=2e-3)
        out[..., c] = lowband_match(np.clip(g, 0, 1), mp_rgba[..., c])
    # self-illumination and auxiliary: plain (CE's shape and size)
    return np.clip(out, 0, 1), f


def visor_weight(H, W):
    v = np.asarray(Image.open(R.VISOR).convert("L"), np.float32) / 255
    return cv2.resize(v, (W, H), interpolation=cv2.INTER_AREA)


def apply_visor(img, ce):
    """plain bicubic resample (lightly smoothed against ringing) under the visor: no armour grain"""
    H, W = img.shape[:2]
    w = visor_weight(H, W)[..., None]
    sm = cv2.GaussianBlur(np.clip(cv2.resize(ce, (W, H), interpolation=cv2.INTER_CUBIC), 0, 1), (0, 0), 0.2 * W / ce.shape[1])
    return img * (1 - w) + sm * w


def crops(od, ce, enh, mx, keyname, cs=64):
    """CE nearest | Enhanced | Max crops at the window with the most CE edge contrast, all at the Max scale"""
    os.makedirs(od, exist_ok=True)
    L = T.lum(ce)
    k = np.ones((3, 3), np.uint8)
    con = cv2.dilate(L, k) - cv2.erode(L, k)
    cs = min(cs, ce.shape[0], ce.shape[1])
    x, y = FU.pick_window(con, cs)
    Fm = mx.shape[0] // ce.shape[0]
    Fe = enh.shape[0] // ce.shape[0]
    a = T.nearest(ce[y:y + cs, x:x + cs], Fm)
    e = enh[y * Fe:(y + cs) * Fe, x * Fe:(x + cs) * Fe]
    e = T.nearest(e, Fm // Fe) if Fm > Fe else e
    m = mx[y * Fm:(y + cs) * Fm, x * Fm:(x + cs) * Fm]
    for lab, im in (("ce", a), ("enh", e), ("max", m)):
        T.save(os.path.join(od, lab + ".png"), im)
    return (x, y, cs)


def build_one(tag, idx, png, mp_png):
    r = dict(R.BASES[tag])
    key = R.safe(tag) if idx is None else "%s_%d" % (R.safe(tag), idx)
    od = os.path.join(OUT, key)
    os.makedirs(od, exist_ok=True)
    t0 = time.time()
    ce, alpha = T.load(png)
    h, w = ce.shape[:2]
    mp_rgba = None
    if mp_png:
        a = np.asarray(Image.open(mp_png).convert("RGBA")).astype(np.float32) / 255
        mp_rgba = a
    r["mp"] = None if mp_rgba is None else mp_resized(mp_rgba[..., :3], h, w)
    r["mp_path"] = mp_png
    if r["mp"] is not None and mp_rgba.shape[:2] != (h, w):  # (cov_char_classes reads the map at CE's size)
        r["mp_path"] = os.path.join(od, "mp_at_base_size.png")
        T.save(r["mp_path"], r["mp"])
    keep = IC.keep_mask(ce, False, r["mp"], decals=True)
    if r["visor"]:
        keep = np.maximum(keep, visor_weight(h, w))
    r["keep"] = keep
    SPEC[key] = r
    FU.CFG[key] = (R.MAX_F, False, "char" if r["domain"] == "cov" else r["domain"], r["kind"], FU.GENTLE, None)
    # candidates (the ones fusion reads on character and weapon maps; the stone and grass models are never
    # weighted on these domains and stand in as wdn)
    A = {}
    for m in TC.MODELS:
        A[m] = T.backproject_low(T.load(os.path.join(TC.OUT, key, m + ".png"))[0], ce, 10, wrap=False)
    for m in FU.ALL:
        A.setdefault(m, A[FU.GENTLE])
    fu, info = FU.fuse(key, ce, A, False)
    if r["visor"]:
        fu = apply_visor(fu, ce)
    res = {"tag": tag, "bitmap": idx, "ce_size": [w, h], "classes": info["how"], "deblock": info["deblock"],
           "line_px_share": float((info["gate"] > 0.5).mean()),
           "fusion_model_share": {m: float(v.mean()) for m, v in info["share"].items() if v.mean() > 0},
           "keep_texels": int((keep > 0.5).sum())}
    # Enhanced: fusion 4x (every base here is <= 512, so 4x <= 2048)
    Fe = min(R.ENH_F, R.ENH_CAP // max(w, h))
    enh = fu if Fe == 4 else T.backproject_low(cv2.resize(fu, (w * Fe, h * Fe), interpolation=cv2.INTER_AREA), ce, 4, wrap=False)
    res["enh"] = {"F": Fe, "size": [w * Fe, h * Fe], "style": FU.checks(enh, ce)}
    res["enh"]["style_fails"] = FU.passes(res["enh"]["style"])
    ic = IC.check(enh, ce, False, mp=r["mp"], keep=keep)
    res["enh"]["invented"] = ic
    if "banding" in ic["fails"]:
        # terraced CE ramps (the base models' steps in a smooth gradient): those 8-texel tiles (feathered by
        # 2 texels) take CE's plain resample, as under the keep-CE-shape mask
        mt = np.zeros((h, w), np.float32)
        for tx, ty in ic["banding_at"]:
            mt[max(ty - 2, 0):ty + 10, max(tx - 2, 0):tx + 10] = 1
        mu = cv2.resize(cv2.GaussianBlur(mt, (0, 0), 1.0), (w * Fe, h * Fe), interpolation=cv2.INTER_LINEAR)[..., None]
        enh2 = enh * (1 - mu) + IC.plain(ce, Fe, False) * mu
        if r["visor"]:
            enh2 = apply_visor(enh2, ce)
        ic2 = IC.check(enh2, ce, False, mp=r["mp"], keep=keep)
        res["enh"]["ramp_patch"] = {"tiles": len(ic["banding_at"]), "texel_share": float((mu > 0.5).mean()),
                                    "fails_before": ic["fails"], "fails_after": ic2["fails"]}
        enh, res["enh"]["invented"] = enh2, ic2
        res["enh"]["style"] = FU.checks(enh, ce)
        res["enh"]["style_fails"] = FU.passes(res["enh"]["style"])
        fu = enh if Fe == 4 else fu  # the Max base carries the same fix
    # Max: detail2 at 8x where it passes the invented-texture check
    Fm = min(R.MAX_F, R.MAX_CAP // max(w, h))
    res["max"] = {"F_tried": Fm}
    if r["face"] or Fm <= Fe:
        mx, res["max"]["source"] = enh, "enhanced (face: ML only, at most 4x)" if r["face"] else "enhanced (no larger size)"
    else:
        no_edge = info["P"][..., FU.CLASSES.index("fabric")] + info["P"][..., FU.CLASSES.index("skin")]
        t1 = time.time()
        d2 = D.detail2(ce, Fm, fu, r["kind"], False, P=info["P"], keep=keep, redrawn=True, no_edge=no_edge)
        cand = d2["albedo"]
        if r["visor"]:
            cand = apply_visor(cand, ce)
        res["max"]["detail2_s"] = round(time.time() - t1)
        ic = IC.check(cand, ce, False, mp=r["mp"], keep=keep)
        st = FU.checks(cand, ce)
        res["max"]["candidate"] = {"size": [w * Fm, h * Fm], "invented": ic, "style": st, "style_fails": FU.passes(st)}
        T.save(os.path.join(od, "max_candidate.png"), cand)
        if ic["fails"] and set(ic["fails"]) <= {"banding", "contour"}:
            # the two tile-located measures: a failing CE ramp tile (8 texels, feathered by 2) takes CE's plain
            # resample (a smooth CE gradient has nothing to add), a failing contour tile the fusion base
            # resampled to the same size (no detail stage there); then the whole map is checked again
            def tile_mask(tiles):
                m = np.zeros((h, w), np.float32)
                for tx, ty in tiles:
                    m[max(ty - 2, 0):ty + 10, max(tx - 2, 0):tx + 10] = 1
                return cv2.resize(cv2.GaussianBlur(m, (0, 0), 1.0), (w * Fm, h * Fm), interpolation=cv2.INTER_LINEAR)[..., None]
            bt = [tuple(t) for t in ic.get("banding_at", [])] if "banding" in ic["fails"] else []
            ct = [tuple(t) for t in ic.get("contour_at", [])] if "contour" in ic["fails"] else []
            cand2 = cand
            if ct:
                fuT = T.backproject_low(np.clip(cv2.resize(fu, (w * Fm, h * Fm), interpolation=cv2.INTER_LANCZOS4), 0, 1),
                                        ce, 4, wrap=False)
                fuT = FU.apply_keep(fuT, ce, keep, False, Fm)
                mu = tile_mask(ct)
                cand2 = cand2 * (1 - mu) + fuT * mu
            if bt:
                mu = tile_mask(bt)
                cand2 = cand2 * (1 - mu) + IC.plain(ce, Fm, False) * mu
            if r["visor"]:
                cand2 = apply_visor(cand2, ce)
            tiles = bt + ct
            m = tile_mask(tiles)[..., 0]
            ic2 = IC.check(cand2, ce, False, mp=r["mp"], keep=keep)
            res["max"]["patched"] = {"tiles": len(tiles), "texel_share": float((m > 0.5).mean()), "invented": ic2,
                                     "style": FU.checks(cand2, ce)}
            if not ic2["fails"]:
                cand, ic = cand2, ic2
                res["max"]["patched"]["used"] = True
        if ic["fails"]:
            mx, res["max"]["source"] = enh, "enhanced (fusion+B2 fails: %s)" % ", ".join(ic["fails"])
        else:
            mx = cand
            res["max"]["source"] = "fusion+B2" + (" (%d failing tiles replaced: ramps CE plain, contours fusion)" % res["max"]["patched"]["tiles"]
                                                  if res["max"].get("patched", {}).get("used") else "")
    res["max"]["size"] = [mx.shape[1], mx.shape[0]]
    T.save(os.path.join(od, "enh.png"), enh)
    T.save(os.path.join(od, "max.png"), mx)
    if alpha.min() < 1.0:  # explicit alpha (DXT3 in CE): plain resample
        for lab, im in (("enh", enh), ("max", mx)):
            a = np.clip(cv2.resize(alpha, (im.shape[1], im.shape[0]), interpolation=cv2.INTER_CUBIC), 0, 1)
            T.save(os.path.join(od, "alpha_%s.png" % lab), a)
        res["alpha"] = True
    if mp_rgba is not None:
        for lab, im in (("enh", enh), ("max", mx)):
            mm, f = build_multi(mp_rgba, im)
            Image.fromarray((mm * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(od, "multi_%s.png" % lab))
            dm = cv2.resize(mm, (mp_rgba.shape[1], mp_rgba.shape[0]), interpolation=cv2.INTER_AREA) - mp_rgba
            lam_ce = float((mp_rgba[..., 1] > 0.5).sum()) * f * f
            lam = float((mm[..., 1] > 0.5).sum())
            res["multi_" + lab] = {"size": [mm.shape[1], mm.shape[0]], "factor": f,
                                   "mean_abs_err_at_ce_res": [round(float(np.abs(dm[..., c]).mean()), 4) for c in range(4)],
                                   "selfillum_area_ratio": round(lam / lam_ce, 3) if lam_ce > 0 else None}
    T.save(os.path.join(od, "diag_classes_ce.png"), FU.class_img(info["P"]))
    T.save(os.path.join(od, "diag_keep_ce.png"), np.clip(ce * (1 - 0.6 * keep[..., None]) + 0.6 * keep[..., None] * np.array([1, 0, 0], np.float32), 0, 1))
    res["crop"] = crops(os.path.join(od, "crops"), ce, enh, mx, key)
    res["seconds"] = round(time.time() - t0)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(key, res["max"]["source"], res["seconds"], "s", flush=True)
    return key, res


def jobs(only):
    out = []
    for tag, r in R.BASES.items():
        if only and not any(o in tag for o in only):
            continue
        imgs = TC.images(tag)
        mps = TC.images(r["multi"]) if r["multi"] else [None] * len(imgs)
        for i, (key, png) in enumerate(imgs):
            mp = mps[i][1] if mps[i] else None
            out.append((tag, None if len(imgs) == 1 else i, png, mp))
    return out


def _run(j):
    return build_one(*j)


if __name__ == "__main__":
    args = sys.argv[1:]
    n = 1
    if "--jobs" in args:
        i = args.index("--jobs")
        n = int(args[i + 1])
        del args[i:i + 2]
    J = jobs(args)
    if n == 1:
        for j in J:
            _run(j)
    else:
        with ProcessPoolExecutor(n) as ex:
            list(ex.map(_run, J))
    summ = {}
    for j in jobs([]):
        k = R.safe(j[0]) if j[1] is None else "%s_%d" % (R.safe(j[0]), j[1])
        mp = os.path.join(OUT, k, "metrics.json")
        if os.path.exists(mp):
            summ[k] = json.load(open(mp))
    json.dump(summ, open(os.path.join(OUT, "summary.json"), "w"), indent=1, default=float)
    print("TIERS_OK", len(J), flush=True)
