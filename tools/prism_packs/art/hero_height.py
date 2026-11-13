#!/usr/bin/env python3
"""UV-space relief for a hero model, read from CE's own texture lines (PRISM v2 models).

CE painted a model's shape detail into its base and multipurpose maps: where an armour plate
ends (the multipurpose map's change-colour channel marks the plate), the seams, slots and vents
drawn as dark lines inside a plate, the undersuit's folds and ribs drawn as light and dark bands.
This builds a height field at the base map's coordinates from those lines, so the remodel
(hero_remodel.py) can turn them into geometry where CE drew them and nowhere else:

  plate     the change-colour mask -> raised plates with a rounded edge (a real thickness and
            bevel at every plate border CE drew), the undersuit set back behind them
  grooves   a black top-hat of the base map's luminance inside the plates (seams, slots, vents)
  folds     a band-pass of the luminance on the undersuit (folds, ribbing), signed
  studs     small light round blobs inside the plates (difference of Gaussians), domed

UV islands: every filter runs on maps whose texels outside the islands are filled from the
nearest island texel, so an island's border is never read as a plate edge (no false ridge at
a UV seam).

  python -I art/hero_height.py <recipe.json> <ce_mesh.npz> <out_dir>

Outputs (local only: derived from the player's CE pixels): height.npy (float32, N x N, in cm,
row 0 = the top of the picture = v 1 in Blender), layers.npz, preview PNGs, height.json.
"""
import json
import os
import sys

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK


def P(rel):
    return E.path(rel)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def raster_islands(uv, tri_ok, n):
    """coverage mask and owner triangle per texel (Blender UVs: v up)"""
    cov = np.zeros((n, n), np.uint8)
    owner = np.full((n, n), -1, np.int32)
    for i in np.nonzero(tri_ok)[0]:
        pts = np.stack([uv[i, :, 0] * n, (1 - uv[i, :, 1]) * n], 1)
        q = np.round(pts * 16).astype(np.int32)
        m = np.zeros((n, n), np.uint8)
        cv2.fillConvexPoly(m, q, 1, lineType=cv2.LINE_8, shift=4)
        cov |= m
        owner[m > 0] = i
    return cov > 0, owner


def keep_features(w, min_px):
    """a feature weight with its specks gone: connected parts (above half) smaller than min_px
    pixels are dropped, so CE's mottled paint never becomes dents"""
    lab, nl = ndi.label(w > 0.5)
    if nl == 0:
        return w * 0
    sizes = ndi.sum(np.ones_like(lab), lab, np.arange(1, nl + 1))
    keep = np.isin(lab, 1 + np.nonzero(sizes >= min_px)[0])
    keep = ndi.binary_dilation(keep, iterations=3)
    return w * keep


def fill_outside(a, inside):
    """texels outside the islands take the nearest island texel's value"""
    idx = ndi.distance_transform_edt(~inside, return_distances=False, return_indices=True)
    return a[tuple(idx)]


def main():
    recipe, mesh_npz, out = sys.argv[1:4]
    rc = json.load(open(recipe))
    hp = rc["height"]
    n = hp.get("size", 4096)
    k = n / 512.0                       # pixels per CE texel
    os.makedirs(out, exist_ok=True)
    d = np.load(mesh_npz)
    names = [str(x) for x in d["names"]]
    sh = names.index(rc["detail_shader"])
    tri_ok = d["mat"] == sh
    inside, owner = raster_islands(d["uv"], tri_ok, n)

    alb_src = P(hp["albedo_hi"]) if hp.get("albedo_hi") and os.path.exists(P(hp["albedo_hi"])) else P(rc["albedo"])
    alb = np.asarray(Image.open(alb_src).convert("RGB"), np.float32) / 255
    alb = cv2.resize(alb, (n, n), interpolation=cv2.INTER_CUBIC if alb.shape[0] < n else cv2.INTER_AREA)
    mp = np.asarray(Image.open(P(rc["multipurpose"])).convert("RGBA"), np.float32) / 255
    mp = cv2.resize(mp, (n, n), interpolation=cv2.INTER_LINEAR)
    lum = np.clip(alb @ np.array([0.2126, 0.7152, 0.0722], np.float32), 0, 1)

    # classes: the PRISM class mask (built locally by class_masks.py: undersuit, hard polymer,
    # rubber grip weights at the base map's coordinates), if the recipe names one
    cls = {}
    if hp.get("class_mask") and os.path.exists(P(hp["class_mask"])):
        cm = np.asarray(Image.open(P(hp["class_mask"])).convert("RGBA"), np.float32) / 255
        cm = cv2.resize(cm, (n, n), interpolation=cv2.INTER_LINEAR)
        for role, ch in hp["class_channels"].items():
            cls[role] = fill_outside(cv2.GaussianBlur(cm[..., ch], (0, 0), 0.5 * k), inside)
    # plate: the change-colour channel (multipurpose blue in the extracted PNG), smoothed so its
    # border follows CE's painted line without the 512 grid's steps
    cc = cv2.GaussianBlur(mp[..., hp.get("plate_channel", 2)], (0, 0), hp.get("plate_blur_texels", 0.6) * k)
    plate_soft = smoothstep(hp["plate_lo"], hp["plate_hi"], cc)
    plate_soft = fill_outside(plate_soft, inside)
    lum_f = fill_outside(lum, inside)
    # (CE's mottled paint and the DXT grid are not shape: blur them out before reading lines)
    lum_f = cv2.GaussianBlur(lum_f, (0, 0), hp.get("lum_blur_texels", 0.6) * k)
    plate = plate_soft > 0.5
    # lights (the self-illumination channel): kept flush with the plate around them and never
    # read as a groove or stud, so a lamp CE painted stays visible
    glow = mp[..., hp.get("glow_channel", 1)] > hp.get("glow_above", 0.3)
    glow = ndi.binary_dilation(glow, iterations=int(round(hp.get("glow_margin_texels", 1.5) * k)))
    glow &= inside
    near_plate = ndi.binary_dilation(plate, iterations=int(round(3 * k)))
    plate |= glow & near_plate
    # drop specks: plate islands and holes below a few CE texels
    min_px = int((hp.get("min_feature_texels", 3.0) * k) ** 2)
    lab, nl = ndi.label(plate)
    sizes = ndi.sum(np.ones_like(lab), lab, np.arange(1, nl + 1))
    plate = np.isin(lab, 1 + np.nonzero(sizes >= min_px)[0])
    lab, nl = ndi.label(~plate)
    sizes = ndi.sum(np.ones_like(lab), lab, np.arange(1, nl + 1))
    plate = ~np.isin(lab, 1 + np.nonzero(sizes >= min_px)[0])

    # signed distance to the plate border (pixels; + inside a plate)
    sdf = ndi.distance_transform_edt(plate) - ndi.distance_transform_edt(~plate)
    sdf = cv2.GaussianBlur(sdf.astype(np.float32), (0, 0), 0.5 * k)
    bev = hp["bevel_texels"] * k
    s = np.clip(sdf / bev, 0, 1)
    rim = np.sqrt(np.clip(1 - (1 - s) ** 2, 0, 1))            # quarter-round bevel
    h_plate = np.where(sdf > 0, hp["plate_cm"] * rim, 0.0)
    # undersuit set back, with a short fillet where it meets the plate wall
    fil = smoothstep(0, hp.get("fillet_texels", 0.5) * k, -sdf)
    h_suit = np.where(sdf <= 0, -hp["suit_cm"] * fil, 0.0)
    # undersuit weight (folds, ribs) and hard-part weight (no folds; grooves like a plate's)
    w_suit = cls.get("undersuit", np.ones_like(sdf)) * (sdf <= 0)
    w_hard = (cls.get("hard", np.zeros_like(sdf)) + cls.get("grip", np.zeros_like(sdf)) * hp.get("grip_groove_share", 0.5)) * (sdf <= 0)

    # grooves (seams, slots, vents CE drew dark inside a plate)
    r = int(round(hp["groove_texels"] * k))
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    tophat = cv2.morphologyEx(lum_f, cv2.MORPH_BLACKHAT, ker)
    g = smoothstep(hp["groove_lo"], hp["groove_hi"], tophat)
    g = keep_features(g, hp.get("groove_min_texels2", 2.0) * k * k)
    g = cv2.GaussianBlur(g, (0, 0), 0.35 * k)
    deep_inside = smoothstep(hp.get("inside_lo", 1.0) * bev, hp.get("inside_hi", 2.5) * bev, sdf)
    hard_inside = smoothstep(0.5 * k, 2.0 * k, -sdf) * np.clip(w_hard, 0, 1)
    h_groove = -hp["groove_cm"] * g * np.maximum(deep_inside, hard_inside * hp.get("hard_groove_share", 0.8))

    # undersuit folds and ribs: band-pass luminance, and the suit's own dark slots (top-hat)
    bp = cv2.GaussianBlur(lum_f, (0, 0), hp["fold_sigma_lo"] * k) - cv2.GaussianBlur(lum_f, (0, 0), hp["fold_sigma_hi"] * k)
    suit_w = smoothstep(0.5 * k, 2.0 * k, -sdf) * np.clip(w_suit, 0, 1)
    h_fold = np.clip(bp * hp["fold_gain_cm"], -hp["fold_max_cm"], hp["fold_max_cm"]) * suit_w
    rs = int(round(hp["rib_texels"] * k))
    ker2 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * rs + 1, 2 * rs + 1))
    th2 = cv2.morphologyEx(lum_f, cv2.MORPH_BLACKHAT, ker2)
    h_rib = -hp["rib_cm"] * keep_features(smoothstep(hp["rib_lo"], hp["rib_hi"], th2),
                                           hp.get("groove_min_texels2", 2.0) * k * k) * suit_w

    # studs: small light round blobs inside the plates (scale-normalised Hessian: both curvatures
    # negative, so a plate edge's highlight line is not taken for a stud)
    s1 = hp["stud_sigma_texels"] * k
    L = cv2.GaussianBlur(lum_f, (0, 0), s1)
    lyy, lyx = np.gradient(np.gradient(L, axis=0), axis=(0, 1))
    lxx = np.gradient(np.gradient(L, axis=1), axis=1)
    det = (lxx * lyy - lyx * lyx) * s1 ** 4
    tr = (lxx + lyy) * s1 ** 2
    blob = np.where((det > 0) & (tr < 0), np.sqrt(np.maximum(det, 0)), 0.0)
    stud = smoothstep(hp["stud_lo"], hp["stud_hi"], blob) * deep_inside
    stud = keep_features(stud, hp.get("stud_min_texels2", 0.7) * k * k)
    stud = cv2.GaussianBlur(stud.astype(np.float32), (0, 0), 0.6 * s1)
    h_stud = hp["stud_cm"] * stud

    calm = 1.0 - cv2.GaussianBlur(glow.astype(np.float32), (0, 0), 0.5 * k)
    h_groove *= calm
    h_stud *= calm
    h_fold *= calm
    h_rib *= calm
    H = h_plate + h_suit + h_groove + h_fold + h_rib + h_stud
    H = cv2.GaussianBlur(H.astype(np.float32), (0, 0), hp.get("final_blur_texels", 0.25) * k)
    H = fill_outside(H, inside).astype(np.float32)
    np.save(os.path.join(out, "height.npy"), H)
    # the plate border's signed distance (pixels) and its snap levels: the remodel puts a ring of
    # vertices on each, so a plate's wall and bevel run along CE's painted line, not across edges
    np.save(os.path.join(out, "plate_sdf.npy"), sdf.astype(np.float32))
    json.dump({"levels_px": [-hp.get("fillet_texels", 0.5) * k, 0.0, bev], "px_per_texel": k},
              open(os.path.join(out, "snap.json"), "w"))
    small = lambda a: cv2.resize(a.astype(np.float32), (1024, 1024), interpolation=cv2.INTER_AREA)
    np.savez_compressed(os.path.join(out, "layers.npz"), plate=small(plate), sdf=small(sdf), groove=small(h_groove),
                        fold=small(h_fold), rib=small(h_rib), stud=small(h_stud), inside=small(inside))

    # previews: height (grey), relief shaded, and the layers over the CE albedo
    lo, hi = float(H.min()), float(H.max())
    gy, gx = np.gradient(H)
    sh_ = np.clip(0.6 + 1.2 * (-gx * 0.7 + gy * 0.7) * (k / 2), 0, 1)
    prev = np.concatenate([(H - lo) / (hi - lo + 1e-9), sh_], 1)
    Image.fromarray((small(prev[:, :n]) * 255).astype(np.uint8)).save(os.path.join(out, "height_grey.png"))
    Image.fromarray((small(sh_) * 255).astype(np.uint8)).save(os.path.join(out, "height_relief.png"))
    a1 = small(alb.mean(2) if alb.ndim == 3 else alb)
    ins = small(inside)
    lay = np.stack([a1 * 0.5 + 0.5 * (small(h_groove + h_rib) < -0.05) * ins,
                    a1 * 0.5 + 0.5 * small(plate.astype(np.float32)) * 0.6 * ins,
                    a1 * 0.5 + 0.5 * (small(h_stud) > 0.05) * ins], 2)
    Image.fromarray((np.clip(lay, 0, 1) * 255).astype(np.uint8)).save(os.path.join(out, "layers.png"))
    info = {"size": n, "albedo_source": os.path.relpath(alb_src, ROOT), "cm_min": lo, "cm_max": hi,
            "plate_share_of_islands": float(plate[inside].mean()), "island_share": float(inside.mean()),
            "params": hp}
    json.dump(info, open(os.path.join(out, "height.json"), "w"), indent=1)
    print("HEIGHT_OK", json.dumps({k_: v for k_, v in info.items() if k_ != "params"}))


if __name__ == "__main__":
    main()
