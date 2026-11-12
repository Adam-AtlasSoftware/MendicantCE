#!/usr/bin/env python3
"""PRISM v2 round 2: terrace layers for a hero remodel (hero_remodel2.py), at the base map's coordinates.

Round 1 read one blended height field from CE's lines and moved vertices by it: plate borders came out soft
and ragged (a vertex row never lies on the painted line, and a border on a UV seam was averaged from two
islands that disagree). Round 2 hands the remodel a signed distance per layer instead, in millimetres on
the model's surface (positive inside the layer's shape), so the remodel can cut a vertex loop exactly on
every contour of a crisp profile (wall foot, wall top, chamfer top) and lay heights on those loops.

Layers (recipe "layers"; each: kind raise|cut, depth_cm, chamfer_mm, wall_mm, game true|false):
  raise      CE's armour: the multipurpose map's change-colour plates plus the light grey hard parts of the
             base map (light, not change-colour, not undersuit), speck-free; plus authored islands
  ...        authored shapes (recipe "features"): uv_rect, uv_circle, uv_poly, uv_island (fill the UV island
             under a point), path3d (a groove or slot along picked 3D points, projected onto CE's surface and
             drawn in every island it crosses, so a break across a UV seam is continuous), dots3d (discs);
             uv_dark (the base map's dark texels inside a rectangle: CE's own painted vent slats)
Detail (recipe "detail"; highpoly only, in cm): CE's painted seams and slots inside plates (black top-hat),
the undersuit's folds and ribs, small studs, damped near every authored feature and every lamp.

  python -I art/hero_height2.py <recipe.json> <world mesh.npz (hero_pick.py)> <out dir>

Outputs (local only: CE pixels and vertices): sdf_<layer>.npy (float32 mm, N x N, row 0 = v 1), detail.npy (cm),
mm_per_px.npy, inside.npy, masks.png (preview), height2.json.
"""
import json
import os
import sys

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hero_height import fill_outside, keep_features, smoothstep  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK


def P(rel):
    return E.path(rel)


def raster_owner(uv, tris, n):
    """owner triangle per texel (-1 outside), Blender UVs (v up)"""
    owner = np.full((n, n), -1, np.int32)
    for i in tris:
        pts = np.stack([uv[i, :, 0] * n, (1 - uv[i, :, 1]) * n], 1)
        q = np.round(pts * 16).astype(np.int32)
        m = np.zeros((n, n), np.uint8)
        cv2.fillConvexPoly(m, q, 1, lineType=cv2.LINE_8, shift=4)
        owner[m > 0] = i
    return owner


def islands(uv, tri, tris):
    """UV island id per triangle: triangles joined by an edge whose UVs agree on both sides"""
    key = lambda p: (round(float(p[0]), 5), round(float(p[1]), 5))
    edges = {}
    for f in tris:
        for k in range(3):
            a, b = tri[f, k], tri[f, (k + 1) % 3]
            e = (min(a, b), max(a, b))
            edges.setdefault(e, []).append((f, {a: key(uv[f, k]), b: key(uv[f, (k + 1) % 3])}))
    par = {f: f for f in tris}

    def fd(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for lst in edges.values():
        if len(lst) == 2 and lst[0][1] == lst[1][1]:
            par[fd(lst[0][0])] = fd(lst[1][0])
    return {f: fd(f) for f in tris}


class Surface:
    """CE's world-space triangles of the baked shaders: closest point, UV and texel density"""

    def __init__(self, mesh, tris, n):
        self.co, self.tri, self.uv = mesh["co"], mesh["tri"], mesh["uv"]
        self.tris = np.asarray(tris)
        p = self.co[self.tri[self.tris]]
        self.p = p
        a3 = 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1) * 3048.0 ** 2   # mm^2
        u = self.uv[self.tris] * n
        e0, e1 = u[:, 1] - u[:, 0], u[:, 2] - u[:, 0]
        a2 = 0.5 * np.abs(e0[:, 0] * e1[:, 1] - e0[:, 1] * e1[:, 0])                                  # px^2
        self.mm_per_px = np.sqrt(a3 / np.maximum(a2, 1e-9))
        self.n = n

    def closest(self, q):
        """closest point on the triangles to q: (row in self.tris, barycentric, distance in mm)"""
        p = self.p
        a, b, c = p[:, 0], p[:, 1], p[:, 2]
        # project onto the plane, clamp to the triangle by sampling edges if outside
        ab, ac = b - a, c - a
        ap = q - a
        d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
        d00, d01, d11 = (ab * ab).sum(1), (ab * ac).sum(1), (ac * ac).sum(1)
        den = np.maximum(d00 * d11 - d01 * d01, 1e-30)
        v = (d11 * d1 - d01 * d2) / den
        w = (d00 * d2 - d01 * d1) / den
        bc = np.stack([1 - v - w, v, w], 1)
        bc = np.clip(bc, 0, None)
        bc /= bc.sum(1, keepdims=True)
        x = (bc[:, :, None] * p).sum(1)
        dist = np.linalg.norm(x - q, axis=1)
        i = int(np.argmin(dist))
        return i, bc[i], dist[i] * 3048.0

    def all_close(self, q, tol_mm):
        """every triangle within tol_mm of q (both sides of a seam), with barycentrics"""
        p = self.p
        a, b, c = p[:, 0], p[:, 1], p[:, 2]
        ab, ac = b - a, c - a
        ap = q - a
        d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
        d00, d01, d11 = (ab * ab).sum(1), (ab * ac).sum(1), (ac * ac).sum(1)
        den = np.maximum(d00 * d11 - d01 * d01, 1e-30)
        v = (d11 * d1 - d01 * d2) / den
        w = (d00 * d2 - d01 * d1) / den
        bc = np.clip(np.stack([1 - v - w, v, w], 1), 0, None)
        bc /= bc.sum(1, keepdims=True)
        x = (bc[:, :, None] * p).sum(1)
        dist = np.linalg.norm(x - q, axis=1) * 3048.0
        best = dist.min()
        sel = np.nonzero(dist <= best + tol_mm)[0]
        return [(int(i), bc[i]) for i in sel]

    def uv_px(self, row, bc):
        u = (bc[:, None] * self.uv[self.tris[row]]).sum(0)
        return np.array([u[0] * self.n, (1 - u[1]) * self.n])


def draw_feature(f, masks, surf, n, lum, inside, owner_isl, isl_mask_cache, picks):
    L = f["layer"]
    m = masks.setdefault(L, np.zeros((n, n), np.uint8))
    s = n / 1024.0
    t = f["type"]
    if t == "uv_rect":
        x0, y0, x1, y1 = [v * s for v in f["rect"]]
        r = f.get("round_px", 0) * s
        tmp = np.zeros_like(m)
        cv2.rectangle(tmp, (int(round(x0 + r)), int(round(y0))), (int(round(x1 - r)), int(round(y1))), 1, -1)
        cv2.rectangle(tmp, (int(round(x0)), int(round(y0 + r))), (int(round(x1)), int(round(y1 - r))), 1, -1)
        if r > 0:
            for cx, cy in ((x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)):
                cv2.circle(tmp, (int(round(cx)), int(round(cy))), int(round(r)), 1, -1)
        m |= tmp
    elif t == "uv_circle":
        cv2.circle(m, (int(round(f["c"][0] * s)), int(round(f["c"][1] * s))), int(round(f["r_px"] * s)), 1, -1)
    elif t == "uv_poly":
        q = np.round(np.array(f["pts"]) * s * 16).astype(np.int32)
        cv2.fillPoly(m, [q], 1, lineType=cv2.LINE_8, shift=4)
    elif t == "uv_dark":
        x0, y0, x1, y1 = [int(round(v * s)) for v in f["rect"]]
        sub = lum[y0:y1, x0:x1] < f["lum_below"]
        sub = ndi.binary_opening(sub, iterations=1)
        m[y0:y1, x0:x1] |= sub.astype(np.uint8)
    elif t == "uv_island":
        x, y = [int(round(v * s)) for v in f["at"]]
        isl = owner_isl[y, x]
        m |= ((owner_isl == isl) & (isl >= 0)).astype(np.uint8)
    elif t in ("path3d", "dots3d"):
        pts = [np.array(picks(p)) for p in f["picks"]]
        width = f.get("width_mm", 4.0)
        if t == "path3d":
            samp = []
            for a, b in zip(pts[:-1], pts[1:]):
                L3 = np.linalg.norm(b - a) * 3048.0
                k = max(2, int(L3 / 0.5))
                samp += [a + (b - a) * i / k for i in range(k + 1)]
        else:
            samp = pts
        rad = 0.5 * width if t == "path3d" else f.get("r_mm", 4.0)
        for q in samp:
            for row, bc in surf.all_close(q, 0.6):
                c = surf.uv_px(row, bc)
                rp = rad / surf.mm_per_px[row]
                cv2.circle(m, (int(round(c[0] * 16)), int(round(c[1] * 16))), max(1, int(round(rp * 16))), 1, -1,
                           lineType=cv2.LINE_8, shift=4)
    else:
        raise ValueError(t)


def main():
    recipe, mesh_npz, out = sys.argv[1:4]
    rc = json.load(open(recipe))
    hp = rc["height2"]
    n = hp.get("size", 4096)
    k = n / 512.0
    os.makedirs(out, exist_ok=True)
    mesh = np.load(mesh_npz)
    names = [str(x) for x in mesh["names"]]
    bake_sh = [names.index(s) for s in rc["bake_shaders"]]
    det_sh = names.index(rc["detail_shader"])
    tris = np.nonzero(np.isin(mesh["mat"], bake_sh))[0]
    arm = np.nonzero(mesh["mat"] == det_sh)[0]
    owner = raster_owner(mesh["uv"], arm, n)
    inside = owner >= 0
    isl = islands(mesh["uv"], mesh["tri"], list(arm))
    ids = np.full(int(mesh["tri"].shape[0]), -1, np.int64)
    for t_, r_ in isl.items():
        ids[t_] = r_
    owner_isl = np.where(owner >= 0, ids[np.maximum(owner, 0)], -1)
    surf = Surface(mesh, arm, n)
    row_of = np.full(int(mesh["tri"].shape[0]), -1, np.int64)
    row_of[arm] = np.arange(len(arm))
    mmpp = np.where(owner >= 0, surf.mm_per_px[np.maximum(row_of[np.maximum(owner, 0)], 0)], 0).astype(np.float32)
    # (a triangle whose UVs collapse to a line has no texel size: clamped to the model's usual range)
    lo_, hi_ = np.percentile(mmpp[inside], [1, 99])
    mmpp = np.clip(mmpp, 0.8 * lo_, 1.25 * hi_)
    mmpp = fill_outside(mmpp, inside)

    alb = np.asarray(Image.open(P(rc["albedo"])).convert("RGB"), np.float32) / 255
    alb = cv2.resize(alb, (n, n), interpolation=cv2.INTER_CUBIC)
    mp = np.asarray(Image.open(P(rc["multipurpose"])).convert("RGBA"), np.float32) / 255
    mp = cv2.resize(mp, (n, n), interpolation=cv2.INTER_LINEAR)
    lum = np.clip(alb @ np.array([0.2126, 0.7152, 0.0722], np.float32), 0, 1)
    lum_raw = lum.copy()
    lum = cv2.GaussianBlur(fill_outside(lum, inside), (0, 0), hp.get("lum_blur_texels", 0.6) * k)

    # ---- the armour: change-colour plates + light grey hard parts
    cc = cv2.GaussianBlur(mp[..., hp.get("plate_channel", 2)], (0, 0), hp.get("plate_blur_texels", 1.1) * k)
    cc = fill_outside(cc, inside)
    plate = cc > hp.get("plate_cut", 0.32)
    lumh = cv2.GaussianBlur(lum, (0, 0), hp.get("hard_blur_texels", 1.2) * k)
    hard = (lumh > hp.get("hard_lum", 0.42)) & (cc < hp.get("plate_cut", 0.32))
    hard = ndi.binary_opening(hard, iterations=int(round(1.0 * k)))
    glow = mp[..., hp.get("glow_channel", 1)] > hp.get("glow_above", 0.3)
    glow = ndi.binary_dilation(glow, iterations=int(round(hp.get("glow_margin_texels", 1.5) * k))) & inside
    raise_m = plate | hard
    raise_m |= glow & ndi.binary_dilation(raise_m, iterations=int(round(3 * k)))
    min_px = int((hp.get("min_feature_texels", 3.0) * k) ** 2)
    for _ in range(2):
        lab, nl = ndi.label(raise_m)
        sizes = ndi.sum(np.ones_like(lab), lab, np.arange(1, nl + 1))
        raise_m = np.isin(lab, 1 + np.nonzero(sizes >= min_px)[0])
        lab, nl = ndi.label(~raise_m)
        sizes = ndi.sum(np.ones_like(lab), lab, np.arange(1, nl + 1))
        raise_m = ~np.isin(lab, 1 + np.nonzero(sizes >= min_px)[0])
    # (smooth the painted line before taking its distance: CE's border wobbles by a texel)
    rs = cv2.GaussianBlur(raise_m.astype(np.float32), (0, 0), hp.get("border_blur_texels", 1.0) * k)
    rm_ = rs > 0.5
    # a CE triangle the paint almost fills (or almost leaves) is taken whole: a plate border CE painted a few
    # texels beside one of its own edges (a crease, a UV seam) then runs exactly along that edge, straight,
    # instead of wobbling beside it (round 1's ragged groin, thigh and boot-top edges)
    snap_hi, snap_lo = hp.get("tri_fill_above", 0.85), hp.get("tri_clear_below", 0.15)
    own = owner[inside]
    cov = np.bincount(own, weights=rm_[inside].astype(np.float64), minlength=int(mesh["tri"].shape[0]))
    tot = np.bincount(own, minlength=int(mesh["tri"].shape[0]))
    frac = cov / np.maximum(tot, 1)
    fill = np.zeros_like(rm_)
    clear = np.zeros_like(rm_)
    fill[inside] = frac[own] >= snap_hi
    clear[inside] = frac[own] <= snap_lo
    rm_ = (rm_ | fill) & ~clear
    masks = {"raise": rm_.astype(np.uint8)}

    # ---- authored features
    from hero_pick_query import Picker
    pk = Picker(rc["pick_dir"] if os.path.isabs(rc["pick_dir"]) else P(rc["pick_dir"]))

    def picks(p):
        if isinstance(p[0], str):
            r = pk.pick(p[0], p[1], p[2])
            if r is None:
                raise ValueError("pick missed: %s" % (p,))
            return r["p"]
        return p
    for f in rc.get("features", []):
        if f["type"] == "tube3d":
            continue
        draw_feature(f, masks, surf, n, lum_raw, inside, owner_isl, None, picks)
    # a layer may be clipped to another (a vent inside its recess, a bolt on its plate)
    for L in rc["layers"]:
        if L.get("clip_to") and L["name"] in masks:
            mg = L.get("clip_margin_texels", -0.5 if L["clip_to"] != "raise" else 1.5)
            if mg < 0:
                masks[L["name"]] &= ndi.binary_dilation(masks[L["clip_to"]], iterations=int(round(-mg * k)))
            else:
                masks[L["name"]] &= ndi.binary_erosion(masks[L["clip_to"]], iterations=int(round(mg * k)))
        if L.get("clip_out") and L["name"] in masks:
            for o in L["clip_out"]:
                if o in masks:
                    masks[L["name"]] &= ~masks[o].astype(bool)

    # ---- signed distance in mm per layer (positive inside), on the surface: texel distance times the
    # texel's mm (the owner triangle's), the outside filled from the nearest island texel first
    info = {"size": n, "layers": {}}
    for L in rc["layers"]:
        name = L["name"]
        if name not in masks:
            continue
        mk = masks[name].astype(bool)
        mk_f = fill_outside(mk.astype(np.float32), inside) > 0.5 if name == "raise" else mk
        sdf = ndi.distance_transform_edt(mk_f) - ndi.distance_transform_edt(~mk_f)
        sdf = sdf.astype(np.float32) * mmpp
        sdf = cv2.GaussianBlur(sdf, (0, 0), 0.35 * k)
        sdf = np.clip(sdf, -40, 40).astype(np.float32)
        np.save(os.path.join(out, "sdf_%s.npy" % name), sdf)
        info["layers"][name] = {"texels": int(mk.sum()), "share_of_islands": float(mk[inside].mean())}

    # ---- detail relief (highpoly only, cm): CE's painted seams, studs, folds, ribs
    dp = rc["detail"]
    sraise = np.load(os.path.join(out, "sdf_raise.npy"))
    deep = smoothstep(dp.get("inside_mm_lo", 3.0), dp.get("inside_mm_hi", 6.0), sraise)
    suitw = smoothstep(1.0, 4.0, -sraise)
    r = int(round(dp["groove_texels"] * k))
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    tophat = cv2.morphologyEx(lum, cv2.MORPH_BLACKHAT, ker)
    g = smoothstep(dp["groove_lo"], dp["groove_hi"], tophat)
    g = keep_features(g, dp.get("groove_min_texels2", 2.5) * k * k)
    g = cv2.GaussianBlur(g, (0, 0), 0.3 * k)
    h_groove = -dp["groove_cm"] * g * deep
    bp = cv2.GaussianBlur(lum, (0, 0), dp["fold_sigma_lo"] * k) - cv2.GaussianBlur(lum, (0, 0), dp["fold_sigma_hi"] * k)
    h_fold = np.clip(bp * dp["fold_gain_cm"], -dp["fold_max_cm"], dp["fold_max_cm"]) * suitw
    rr = int(round(dp["rib_texels"] * k))
    ker2 = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * rr + 1, 2 * rr + 1))
    th2 = cv2.morphologyEx(lum, cv2.MORPH_BLACKHAT, ker2)
    h_rib = -dp["rib_cm"] * keep_features(smoothstep(dp["rib_lo"], dp["rib_hi"], th2), 2.5 * k * k) * suitw
    s1 = dp["stud_sigma_texels"] * k
    Lb = cv2.GaussianBlur(lum, (0, 0), s1)
    lyy, lyx = np.gradient(np.gradient(Lb, axis=0), axis=(0, 1))
    lxx = np.gradient(np.gradient(Lb, axis=1), axis=1)
    det = (lxx * lyy - lyx * lyx) * s1 ** 4
    tr = (lxx + lyy) * s1 ** 2
    blob = np.where((det > 0) & (tr < 0), np.sqrt(np.maximum(det, 0)), 0.0)
    stud = keep_features(smoothstep(dp["stud_lo"], dp["stud_hi"], blob) * deep, 0.8 * k * k)
    h_stud = dp["stud_cm"] * cv2.GaussianBlur(stud.astype(np.float32), (0, 0), 0.6 * s1)
    # calm near lamps and authored features (they are geometry already)
    auth = np.zeros((n, n), bool)
    for name, mk in masks.items():
        if name != "raise":
            auth |= mk.astype(bool)
    calm = 1.0 - cv2.GaussianBlur((glow | ndi.binary_dilation(auth, iterations=int(round(1.0 * k)))).astype(np.float32),
                                  (0, 0), 0.5 * k)
    D = (h_groove + h_fold + h_rib + h_stud) * calm
    D = cv2.GaussianBlur(D.astype(np.float32), (0, 0), 0.25 * k)
    D = fill_outside(D, inside).astype(np.float32)
    np.save(os.path.join(out, "detail.npy"), D)
    np.save(os.path.join(out, "inside.npy"), inside)
    np.save(os.path.join(out, "mm_per_px.npy"), mmpp.astype(np.float32))
    info["detail_cm"] = [float(D.min()), float(D.max())]
    info["mm_per_px_median"] = float(np.median(mmpp[inside]))
    info["plate_share"] = float(plate[inside].mean())
    info["hard_share"] = float(hard[inside].mean())

    # ---- preview: albedo with the layers' borders
    sm = 2048
    base = cv2.resize(alb, (sm, sm), interpolation=cv2.INTER_AREA)
    pv = (base * 0.6 * 255).astype(np.uint8)[:, :, ::-1].copy()
    cols = [(0, 255, 0), (0, 0, 255), (255, 128, 0), (255, 0, 255), (0, 255, 255), (255, 255, 0), (128, 128, 255), (255, 255, 255)]
    for i, L in enumerate(rc["layers"]):
        f = os.path.join(out, "sdf_%s.npy" % L["name"])
        if not os.path.exists(f):
            continue
        s_ = cv2.resize(np.load(f), (sm, sm), interpolation=cv2.INTER_AREA)
        edge = (s_ > 0) & ~ndi.binary_erosion(s_ > 0)
        pv[edge] = cols[i % len(cols)]
    dv = cv2.resize(D, (sm, sm), interpolation=cv2.INTER_AREA)
    pv2 = np.clip(128 + dv * 300, 0, 255).astype(np.uint8)
    cv2.imwrite(os.path.join(out, "masks.png"), pv)
    cv2.imwrite(os.path.join(out, "detail.png"), pv2)
    json.dump(info, open(os.path.join(out, "height2.json"), "w"), indent=1)
    print("HEIGHT2_OK", json.dumps(info)[:800])


if __name__ == "__main__":
    main()
