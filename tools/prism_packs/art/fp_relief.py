#!/usr/bin/env python3
"""Relief maps for a remodel: CE's painted detail turned into height, at the base map's own UVs.

  python -I art/fp_relief.py <recipe.json>

A recipe names the extracted JMS (UV layout, shaders, 3D scale) and the CE base map it paints, then a
list of features. Every feature cites the texel rectangle of the CE base map where CE paints it
(CE texels, x right, y down the picture, as an image editor shows them) and says what CE paints there;
the op only acts inside that rectangle, on the named shaders' islands. So the relief follows CE's own
lines: a groove lies where CE paints a dark seam, a pad where CE paints a pad, and nothing is added
where CE paints nothing ("Add geometry only where CE paints it").

Ops (sizes in millimetres on the model, converted per island through the UV layout's own scale):
  plate     the islands in the rectangle raised by depth, falling to 0 over bevel at their borders
  raise     where CE paints lighter (or darker, polarity -1) than its surroundings: raised (recessed
  recess    for recess) by depth, edge bevel, top flat / dome / pyramid
  groove    CE's painted dark lines (morphological top-hat) as grooves of width and depth
  ridge     CE's painted light lines as ridges
  screws    CE's painted round dots (Laplacian of Gaussian) as domed heads in a countersink
  relief    CE's painted shading (band-pass of the luminance) as form: folds, ribbing, padding
  shape     (round 2) authored forms, placed at CE texel coordinates where CE paints the part they belong
            to: "rect" panels and slots (rounded corners, an array of them for vents and rail teeth),
            "outline" panel breaks (a groove along a rectangle's border), "circle" lenses and holes,
            "screw" heads (domed, slotted or hex, in a countersink); crisp chamfered walls
Each op has "game": true (in the game mesh) or false (only in the high-poly that the bakes read).
Round 2 options: "edge": "chamfer" (a straight, crisp wall instead of the smooth S), groove/ridge
"shape": "box" (flat floor, chamfered walls), recipe "depth_scale" (every depth multiplied), and an
op's "game_depth" (the game mesh's share of the depth, default all of it); raise/recess "smooth" (CE
texels: the mask's outline smoothed before the wall is cut).

Heights are 0 within margin of every island border, so geometry stays closed across UV seams.

Outputs (local only: derived from CE pixels): <out>/relief.npz (h_game, h_high in world units,
coverage, mm per texel), <out>/relief_preview.png and per-op masks.
"""
import json
import math
import os
import sys
import time

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import halo_formats as hf  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
MM_PER_WU = 3048.0


def P(rel):
    return E.path(rel)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / max(e1 - e0, 1e-9), 0, 1)
    return t * t * (3 - 2 * t)


def raster_layout(jms, size, shaders):
    """coverage per shader set, and mm per texel (at `size`) from each triangle's 3D/UV area ratio.
    Triangles whose UVs are stretched far from the model's typical texel size (CE's slivers mapped
    across the sheet) are left out; smaller triangles are drawn last so they win where islands overlap."""
    rows = []
    for t in jms.triangles:
        if shaders is not None and t.shader not in shaders:
            continue
        vs = [jms.vertices[i] for i in t.v]
        uv = np.array([[v.u * size, (1 - v.v) * size] for v in vs], np.float64)
        p = np.array([v.pos for v in vs], np.float64) / 100.0 * MM_PER_WU
        a3 = np.linalg.norm(np.cross(p[1] - p[0], p[2] - p[0])) / 2
        e1, e2 = uv[1] - uv[0], uv[2] - uv[0]
        a2 = abs(e1[0] * e2[1] - e1[1] * e2[0]) / 2
        if a2 < 1e-6 or a3 < 1e-9:
            continue
        rows.append((a2, math.sqrt(a3 / a2), uv, a3))
    cov = np.zeros((size, size), np.uint8)
    scale = np.zeros((size, size), np.float32)
    if not rows:
        return cov, scale
    s_all = np.array([r[1] for r in rows])
    w_all = np.array([r[3] for r in rows])     # weighted by 3D area
    order = np.argsort(s_all)
    cw = np.cumsum(w_all[order]) / w_all.sum()
    med = s_all[order][np.searchsorted(cw, 0.5)]          # area-weighted median scale
    for a2, s, uv, _ in sorted(rows, key=lambda r: -r[0]):
        if not (med / 3.0 <= s <= med * 3.0):
            continue
        pts = np.round(uv * 16).astype(np.int32)
        cv2.fillPoly(cov, [pts], 1, lineType=cv2.LINE_8, shift=4)
        cv2.fillPoly(scale, [pts], float(s), lineType=cv2.LINE_8, shift=4)
    return cov, scale


def position_map(jms, size, shaders):
    """the model's rest position (mm) of every texel of the named shaders' triangles (later triangles
    win where islands overlap), NaN elsewhere: lets a form be laid out in 3D (straight across the
    model) where CE's UVs are skewed"""
    pos = np.full((size, size, 3), np.nan, np.float32)
    for t in jms.triangles:
        if shaders is not None and t.shader not in shaders:
            continue
        vs = [jms.vertices[i] for i in t.v]
        uv = np.array([[v.u * size, (1 - v.v) * size] for v in vs], np.float64)
        p = np.array([v.pos for v in vs], np.float64) / 100.0 * MM_PER_WU
        x0, y0 = np.floor(uv.min(0)).astype(int)
        x1, y1 = np.ceil(uv.max(0)).astype(int)
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, size - 1), min(y1, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1].astype(np.float64) + 0.5
        a, b, c = uv
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-12:
            continue
        w0 = ((b[1] - c[1]) * (xx - c[0]) + (c[0] - b[0]) * (yy - c[1])) / den
        w1 = ((c[1] - a[1]) * (xx - c[0]) + (a[0] - c[0]) * (yy - c[1])) / den
        w2 = 1 - w0 - w1
        inside = (w0 >= -1e-3) & (w1 >= -1e-3) & (w2 >= -1e-3)
        if not inside.any():
            continue
        q = w0[..., None] * p[0] + w1[..., None] * p[1] + w2[..., None] * p[2]
        sub = pos[y0:y1 + 1, x0:x1 + 1]
        sub[inside] = q[inside]
    return pos


def rect_mask(size, F, rect):
    m = np.zeros((size, size), bool)
    x0, y0, x1, y1 = [int(round(c * F)) for c in rect]
    m[max(y0, 0):min(y1, size), max(x0, 0):min(x1, size)] = True
    return m


def dist_mm(mask, scale):
    """distance inside mask to its border, in mm (texel distance times the local mm per texel)"""
    return ndi.distance_transform_edt(mask) * scale


def clean(mask, F, r_open, r_close, min_area, fill):
    if r_close > 0:
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * int(r_close * F) + 1,) * 2)
        mask = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_CLOSE, k) > 0
    if r_open > 0:
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * int(r_open * F) + 1,) * 2)
        mask = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_OPEN, k) > 0
    if fill:
        mask = ndi.binary_fill_holes(mask)
    if min_area > 0:
        lab, n = ndi.label(mask)
        if n:
            areas = ndi.sum(mask, lab, np.arange(1, n + 1)) / (F * F)
            keep = np.zeros(n + 1, bool)
            keep[1:] = areas >= min_area
            mask = keep[lab]
    return mask


def profile(mask, scale, depth, bevel, top, F, edge_kind="smooth"):
    """height of a raised region: bevel at its edge, then flat, dome or pyramid"""
    d = dist_mm(mask, scale)
    edge = np.clip(d / max(bevel, 1e-6), 0, 1) if edge_kind == "chamfer" else smoothstep(0, bevel, d)
    if top == "flat":
        return depth * edge
    lab, n = ndi.label(mask)
    out = np.zeros(mask.shape, np.float32)
    if not n:
        return out
    dmax = ndi.maximum(d, lab, np.arange(1, n + 1))
    dm = np.zeros(n + 1, np.float32)
    dm[1:] = np.maximum(dmax, 1e-6)
    rel = np.clip(d / dm[lab], 0, 1)
    if top == "dome":
        shape = np.sqrt(np.clip(1 - (1 - rel) ** 2, 0, 1))
    elif top == "pyramid":
        shape = 0.55 * edge + 0.45 * rel
    else:
        raise ValueError(top)
    return depth * np.where(mask, shape, 0)


def _box_sdf(xx, yy, x0, y0, x1, y1, r):
    """signed distance (texels, negative inside) to a rectangle with round corners of radius r"""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx, qy = np.abs(xx - cx) - hx, np.abs(yy - cy) - hy
    out = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
    return out + np.minimum(np.maximum(qx, qy), 0) - r


def shapes(f, S, F, sc, region, dscale, posmap=None):
    """authored forms at CE texel coordinates (round 2): every item's height is added, inside `region`.
    Sizes in texels are CE texels; depths, bevels, widths and radii of screws in mm (scaled by the
    local mm per texel of the layout). Items:
      {"rect": [x0,y0,x1,y1], "radius": t, "depth": mm, "bevel": mm, "array": [n, dx, dy]}
      {"outline": [x0,y0,x1,y1], "radius": t, "width": mm, "depth": mm, "bevel": mm}
      {"circle": [x, y], "r_mm": mm, "depth": mm, "bevel": mm, "top": "flat"|"dome"}
      {"screw": [x, y], "r_mm": mm, "depth": mm, "head": "slot"|"hex"|"dome", "angle": deg}
      {"slabs": [axis, pitch_mm, width_mm], "rect": [x0,y0,x1,y1], "depth": mm, "bevel": mm, "end_mm": mm}
                 crosswise teeth or slots straight in 3D: slabs across the model's axis (0 x, 1 y, 2 z) every
                 pitch mm, width mm wide, inside the texel rectangle (end_mm kept clear at its two ends)"""
    h = np.zeros((S, S), np.float32)
    for it in f["items"]:
        n, dx, dy = it.get("array", [1, 0, 0])
        for k in range(int(n)):
            ox, oy = k * dx, k * dy
            if "slabs" in it:
                ax, pitch, width = it["slabs"]
                x0, y0, x1, y1 = it["rect"]
                X0, Y0, X1, Y1 = int(x0 * F), int(y0 * F), int(x1 * F), int(y1 * F)
                pm = posmap[Y0:Y1, X0:X1, int(ax)]
                ok = np.isfinite(pm)
                if not ok.any():
                    continue
                lo, hi = np.nanmin(pm) + it.get("end_mm", 2.0), np.nanmax(pm) - it.get("end_mm", 2.0)
                n_ = int((hi - lo) // pitch)
                c0 = lo + ((hi - lo) - n_ * pitch) / 2
                d = np.abs(((pm - c0) % pitch) - pitch / 2)          # mm from the slab's middle
                inside_span = (pm >= c0 - pitch / 2 + pitch / 2 - width / 2) & (pm <= c0 + n_ * pitch)
                bev = max(it.get("bevel", 0.25), 1e-3)
                hh = it["depth"] * dscale * np.clip((width / 2 - d) / bev, 0, 1)
                hh = np.where(ok & inside_span, hh, 0)
                h[Y0:Y1, X0:X1] += hh.astype(np.float32)
                continue
            if "rect" in it or "outline" in it:
                x0, y0, x1, y1 = it.get("rect", it.get("outline"))
                x0, x1, y0, y1 = x0 + ox, x1 + ox, y0 + oy, y1 + oy
                pad = 8
                X0, Y0 = max(int((x0 - pad) * F), 0), max(int((y0 - pad) * F), 0)
                X1, Y1 = min(int((x1 + pad) * F) + 1, S), min(int((y1 + pad) * F) + 1, S)
                if X1 <= X0 or Y1 <= Y0:
                    continue
                yy, xx = np.mgrid[Y0:Y1, X0:X1].astype(np.float32) + 0.5
                d = _box_sdf(xx / F, yy / F, x0, y0, x1, y1, it.get("radius", 0.0)) * F   # texels at S
                s_ = sc[Y0:Y1, X0:X1]
                dmm = d * s_
                depth = it["depth"] * dscale
                bev = max(it.get("bevel", 0.3), 1e-3)
                if "outline" in it:
                    w = it["width"] / 2
                    hh = depth * np.clip((w - np.abs(dmm)) / bev, 0, 1)
                else:
                    hh = depth * np.clip(-dmm / bev, 0, 1)
                h[Y0:Y1, X0:X1] += hh
            elif "circle" in it or "screw" in it:
                cx, cy = it.get("circle", it.get("screw"))
                cx, cy = cx + ox, cy + oy
                s0 = float(sc[min(int(cy * F), S - 1), min(int(cx * F), S - 1)]) or float(np.median(sc))
                R = it["r_mm"]
                rr = 2.2 * R / s0
                X0, Y0 = max(int(cx * F - rr), 0), max(int(cy * F - rr), 0)
                X1, Y1 = min(int(cx * F + rr) + 1, S), min(int(cy * F + rr) + 1, S)
                yy, xx = np.mgrid[Y0:Y1, X0:X1].astype(np.float32) + 0.5
                ux, uy = (xx - cx * F) * s0, (yy - cy * F) * s0          # mm
                r = np.hypot(ux, uy)
                depth = it["depth"] * dscale
                if "circle" in it:
                    bev = max(it.get("bevel", 0.3), 1e-3)
                    edge = np.clip((R - r) / bev, 0, 1)
                    if it.get("top") == "dome":
                        edge = edge * (0.55 + 0.45 * np.sqrt(np.clip(1 - (r / R) ** 2, 0, 1)))
                    hh = depth * edge
                else:
                    # a countersink ring (a crisp recess around the head), the head (a low dome with a
                    # chamfered rim) and its drive (a slot or a hexagon socket)
                    sink = -0.45 * depth * np.clip((R * 1.32 - r) / 0.15, 0, 1)
                    head = depth * np.clip((R - r) / 0.18, 0, 1) * (0.7 + 0.3 * np.sqrt(np.clip(1 - (r / R) ** 2, 0, 1)))
                    a = math.radians(it.get("angle", 30.0))
                    ca, sa = math.cos(a), math.sin(a)
                    px, py = ux * ca + uy * sa, -ux * sa + uy * ca
                    kind = it.get("head", "slot")
                    if kind == "slot":
                        drive = (np.abs(py) < 0.13 * R) & (np.abs(px) < 0.78 * R)
                    elif kind == "hex":
                        ang = np.arctan2(py, px)
                        rh = 0.42 * R * math.cos(math.pi / 6) / np.cos((ang % (math.pi / 3)) - math.pi / 6)
                        drive = r < rh
                    else:
                        drive = np.zeros_like(r, bool)
                    hh = np.where(r < R, head, sink) - np.where(drive, 0.75 * depth, 0)
                h[Y0:Y1, X0:X1] += hh
    return np.where(region, h, 0).astype(np.float32)


def main():
    rcp = json.load(open(sys.argv[1]))
    t0 = time.time()
    out = P(rcp["out"])
    os.makedirs(out, exist_ok=True)
    S = rcp.get("size", 4096)
    jms = hf.read_jms(P(rcp["jms"]))
    base = np.asarray(Image.open(P(rcp["texture"])).convert("RGB"), np.float32) / 255.0
    F = S / base.shape[0]
    lum = (0.2126 * base[..., 0] + 0.7152 * base[..., 1] + 0.0722 * base[..., 2]).astype(np.float32)
    L = cv2.resize(lum, (S, S), interpolation=cv2.INTER_CUBIC)
    L = cv2.GaussianBlur(L, (0, 0), 0.55 * F)       # CE's texel and DXT block steps out, shapes kept
    names = [m[0].lower() for m in jms.materials]
    cov_all, scale = raster_layout(jms, S, None)
    report = {"size": S, "factor": F, "ops": []}
    h_game = np.zeros((S, S), np.float32)
    h_high = np.zeros((S, S), np.float32)
    feather_all = np.zeros((S, S), np.float32)
    cov_cache = {}

    def coverage(shaders):
        key = tuple(sorted(shaders))
        if key not in cov_cache:
            idx = {i for i, n in enumerate(names) if n in key}
            cov_cache[key] = raster_layout(jms, S, idx)[0] > 0
        return cov_cache[key]

    for k, f in enumerate(rcp["features"]):
        op = f["op"]
        cov = coverage([s.lower() for s in f["shaders"]])
        margin = f.get("margin", rcp.get("margin", 1.0))
        inner = ndi.distance_transform_edt(cov) / F       # CE texels to the island border
        region = cov & rect_mask(S, F, f["rect"])
        feather = smoothstep(margin * 0.5, margin * 1.5, inner)
        sc = np.where(scale > 0, scale, np.median(scale[scale > 0]))
        depth = f.get("depth", 0.0) * rcp.get("depth_scale", 1.0)
        if "depth" in f:
            f = dict(f, depth=depth)
        h = np.zeros((S, S), np.float32)
        if op == "plate":
            h = profile(region, sc, depth, f["bevel"], "flat", F, f.get("edge", "smooth"))
            feather = np.ones_like(feather)       # the plate's own bevel is its border
        elif op in ("raise", "recess"):
            bg = cv2.GaussianBlur(L, (0, 0), f.get("bg_sigma", 6.0) * F)
            c = (L - bg) * f.get("polarity", 1)
            if "abs" in f:
                c = (L - f["abs"]) * f.get("polarity", 1)
            m = region & (c > f["threshold"])
            m = clean(m, F, f.get("open", 0.5), f.get("close", 0.5), f.get("min_area", 4), f.get("fill", True))
            m &= inner > margin
            if f.get("smooth", 0) > 0:
                # (round 2) the mask's outline smoothed (a blurred mask cut at half): CE's texel steps and
                # DXT blocks out of the plate's border, so a crisp wall runs straight
                m = cv2.GaussianBlur(m.astype(np.float32), (0, 0), f["smooth"] * F) > 0.5
                m &= inner > margin
            h = profile(m, sc, depth, f["bevel"], f.get("top", "flat"), F, f.get("edge", "smooth"))
            if op == "recess":
                h = -h
        elif op in ("groove", "ridge"):
            r = int(f.get("line_radius", 2.0) * F)
            k_ = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
            if op == "groove":
                th = cv2.morphologyEx(L, cv2.MORPH_CLOSE, k_) - L
            else:
                th = L - cv2.morphologyEx(L, cv2.MORPH_OPEN, k_)
            m = region & (th > f["threshold"])
            m = clean(m, F, 0, f.get("close", 0.3), f.get("min_area", 3), False)
            # keep elongated components (lines, not blobs)
            lab, n = ndi.label(m)
            if n:
                objs = ndi.find_objects(lab)
                keep = np.zeros(n + 1, bool)
                for i, sl in enumerate(objs):
                    hh, ww = (sl[0].stop - sl[0].start) / F, (sl[1].stop - sl[1].start) / F
                    keep[i + 1] = max(hh, ww) >= f.get("min_length", 5.0)
                m = keep[lab]
            from skimage.morphology import skeletonize
            sk = skeletonize(m)
            dl = ndi.distance_transform_edt(~sk) * sc
            w = f["width"] / 2.0
            shp = f.get("shape", "u")
            if shp == "box":
                # flat floor, straight chamfered walls (chamfer mm), crisp lips
                prof = np.clip((w - dl) / max(f.get("chamfer", 0.25), 1e-6), 0, 1)
            else:
                prof = np.clip(1 - dl / w, 0, 1)
                prof = prof * prof * (3 - 2 * prof) if shp == "u" else prof
            h = depth * prof * (1 if op == "ridge" else -1)
        elif op == "screws":
            s = f.get("sigma", 1.6) * F
            log = -ndi.gaussian_laplace(L, s) * s * s * f.get("polarity", 1)
            mx = ndi.maximum_filter(log, size=int(2 * s) | 1)
            pk = (log == mx) & (log > f["threshold"]) & region & (inner > margin + 1)
            # round dots only: both principal curvatures of the same sign and similar (not a line's edge)
            pol = f.get("polarity", 1)
            lxx = ndi.gaussian_filter(L, s, order=(0, 2)) * pol
            lyy = ndi.gaussian_filter(L, s, order=(2, 0)) * pol
            lxy = ndi.gaussian_filter(L, s, order=(1, 1)) * pol
            tr, det = lxx + lyy, lxx * lyy - lxy * lxy
            disc = np.sqrt(np.maximum(tr * tr / 4 - det, 0))
            e1, e2 = tr / 2 - disc, tr / 2 + disc          # e1 <= e2, a bright dot: both < 0
            pk &= (e2 < 0) & (e2 / np.minimum(e1, -1e-12) > f.get("roundness", 0.45))
            ys, xs = np.nonzero(pk)
            R = f["radius"]
            for y, x in zip(ys, xs):
                rr = R / sc[y, x]
                y0, y1 = max(0, int(y - 2 * rr)), min(S, int(y + 2 * rr) + 1)
                x0, x1 = max(0, int(x - 2 * rr)), min(S, int(x + 2 * rr) + 1)
                yy, xx = np.mgrid[y0:y1, x0:x1]
                r = np.hypot(yy - y, xx - x) * sc[y, x]
                dome = depth * np.sqrt(np.clip(1 - (r / R) ** 2, 0, 1))
                sink = -f.get("sink", depth * 0.5) * np.clip(1 - np.abs(r - R * 1.15) / (0.25 * R), 0, 1)
                slot = np.where((np.abs((xx - x) * sc[y, x]) < 0.12 * R) & (r < 0.8 * R), -0.6 * depth, 0)
                h[y0:y1, x0:x1] += dome + sink + slot
            report.setdefault("screws_found", {})[str(k)] = int(len(ys))
        elif op == "relief":
            b1 = cv2.GaussianBlur(L, (0, 0), f["sigma_lo"] * F)
            b2 = cv2.GaussianBlur(L, (0, 0), f["sigma_hi"] * F)
            band = (b1 - b2) * f.get("polarity", 1)
            h = np.clip(band * f["gain"], -f.get("max", 99), f.get("max", 99))
            h = np.where(region, h, 0)
            h = cv2.GaussianBlur(h, (0, 0), 0.5 * F) * region * rcp.get("depth_scale", 1.0)
        elif op == "shape":
            pmap = None
            if any("slabs" in it for it in f["items"]):
                idx = {i for i, n in enumerate(names) if n in [s_.lower() for s_ in f["shaders"]]}
                pmap = position_map(jms, S, idx)
            h = shapes(f, S, F, sc, region, rcp.get("depth_scale", 1.0), pmap)
            report.setdefault("shapes", {})[str(k)] = len(f["items"])
        else:
            raise ValueError(op)
        h = h * feather * cov
        h_wu = h / MM_PER_WU
        h_high += h_wu
        if f.get("game", True):
            h_game += h_wu * f.get("game_depth", 1.0)
        feather_all = np.maximum(feather_all, feather * region)
        Image.fromarray((np.clip(0.5 + h / (2 * max(abs(depth), 0.5)), 0, 1) * 255).astype(np.uint8)).resize(
            (1024, 1024), Image.BILINEAR).save(os.path.join(out, "op%02d_%s.png" % (k, op)))
        report["ops"].append({"i": k, "op": op, "cite": f.get("cite", ""), "rect": f["rect"], "game": f.get("game", True),
                              "touched_texels_ce": float((np.abs(h) > 1e-3).sum() / F / F),
                              "min_mm": float(h.min()), "max_mm": float(h.max())})
        print("OP", k, op, f.get("cite", "")[:60], "texels", report["ops"][-1]["touched_texels_ce"],
              "range_mm", round(float(h.min()), 3), round(float(h.max()), 3), flush=True)
    # the game mesh's relief: no detail finer than the game mesh can carry
    gs = rcp.get("game_blur_texels", 0.6)
    if gs > 0:
        h_game = cv2.GaussianBlur(h_game, (0, 0), gs * F)
    np.savez_compressed(os.path.join(out, "relief.npz"), h_game=h_game.astype(np.float32),
                        h_high=h_high.astype(np.float32), coverage=cov_all, mm_per_texel=scale)
    # previews: CE base | high relief (shaded) | game relief (shaded)
    def shade(h):
        gy, gx = np.gradient(h * MM_PER_WU / np.maximum(np.where(scale > 0, scale, 1), 1e-3))
        n = np.dstack([-gx, gy, np.ones_like(h)])
        n /= np.linalg.norm(n, axis=2, keepdims=True)
        l = np.array([-0.5, 0.6, 0.62]); l /= np.linalg.norm(l)
        return np.clip(n @ l, 0, 1)
    tiles = [cv2.resize(base, (2048, 2048), interpolation=cv2.INTER_NEAREST)]
    for h in (h_high, h_game):
        s_ = cv2.resize(shade(h), (2048, 2048), interpolation=cv2.INTER_AREA)
        tiles.append(np.dstack([s_] * 3))
    Image.fromarray((np.hstack(tiles) * 255).astype(np.uint8)).save(os.path.join(out, "relief_preview.png"))
    report["seconds"] = round(time.time() - t0, 1)
    report["h_high_mm"] = [float(h_high.min() * MM_PER_WU), float(h_high.max() * MM_PER_WU)]
    report["h_game_mm"] = [float(h_game.min() * MM_PER_WU), float(h_game.max() * MM_PER_WU)]
    report["mm_per_ce_texel_median"] = float(np.median(scale[scale > 0]) * F)
    json.dump(report, open(os.path.join(out, "relief_report.json"), "w"), indent=1)
    print("RELIEF_OK", json.dumps({k: report[k] for k in ("seconds", "h_high_mm", "h_game_mm", "mm_per_ce_texel_median")}))


if __name__ == "__main__":
    main()
