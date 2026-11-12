#!/usr/bin/env python3
"""PRISM lightmap re-bake, stage 2: calibration against CE's own pages, and the pages
the engine reads (display.prism_lightmaps "rebaked").

  relight_encode.py fit    --gltf <bsp.gltf> --bake <dir> [--gamma auto|1|2.2]
  relight_encode.py encode --gltf <bsp.gltf> --bake <dir> --map <name> --bsp <index> --out <data root>
                           [--field-strength 1] [--previews <dir>]

fit (after relight_bake.py --stage calib): for every covered texel of every page,
  target = (CE's texel x CE's flat-bump factor) ^ gamma   (what CE shows on a flat surface,
           made linear),
  and the three light groups' bakes S (sky lights), K (sky ambient), E (emitters). It finds
  the scales a, b, c >= 0 of target ~ a S + b K + c E (luminance, weighted by relative
  error, the gamma of 1 or 2.2 that fits better), then the area's colour and exposure left
  over: log((target + e) / (a S + b K + c E + e)) per channel, splatted at the texels' world
  positions into a 3D grid and blurred (normalized convolution): a field that varies
  only from room to room. Writes <bake>/scales.json (for the final bake) and
  <bake>/field.npz.

encode (after relight_bake.py --stage final): per texel of the finer page,
  the four bakes (the shading normal tilted 50 degrees: i = 0 the normal, 1-3 a tripod
  about it) times exp(field strength x field at the texel's position) times the area's
  match to CE (--match-mode sums, the default: CE's shown light and the bake's, each
  blurred chart by chart in the world over --match-wu at the stock size, their ratio, the
  luminance's within 1/--match-clamp..--match-clamp and each channel's over it within
  --match-chroma-clamp; an emitting shader's own texels matched texel by texel; where the
  bake has next to no light, the light it falls short by added without a direction:
  CE's mood and its lamps' hue at low frequency, the bake's detail above it; --match-mode
  log, the first version: the blurred log ratio, darker), then one gain for the whole BSP
  so that the mean luminance CE's normal shows is CE's times --exposure-aim when it falls
  outside --exposure-min..--exposure-max (never darker than CE); Y_i their luminance in CE's encoding
  (^ 1/gamma) fitted to Y(n) = alpha + beta . n (tangent space), written in the form
  CE's lightmap pass combines: L (1 - a + a n . d), d = beta / |beta|,
  L = alpha + |beta|, a = |beta| / L (at most --max-directional). The page holds L's
  colour (the normal's light divided by 1 - a + a d.z: on CE's normal it shows the
  bake's light exactly). Gutters (texels no triangle covers) are filled from their
  covered neighbours, then from the stock page.
  Writes <out>/prism_lightmaps/rebaked/<map>_<bsp>_<page>.prl (local only, never shipped):
    0  "PRL1"   4 version (2)   8 stock width   12 stock height   16 factor
    20 the stock page's hash (FNV-1a of its decoded ARGB, as prism_lightmaps.c hashes it)
    24 flags (1: a direction image follows)   28 deflated size   32 inflated size
    36 CRC-32 of bytes 0..35 and the deflated bytes;  40 the deflated bytes:
    the light L (RGBA8, rows from v = 0), then the direction (RGB = d * 0.5 + 0.5 in the
    vertices' tangent frame, A = 255 a), same size.
"""
import argparse
import json
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
G_RANGE = 2.0


def gltf_pages(gltf):
    g = json.load(open(gltf))
    root = os.path.abspath(os.path.join(os.path.dirname(gltf), "..", "..", ".."))
    pages = {}
    for m in g["materials"]:
        ex = m.get("extras", {})
        p = ex.get("lightmap_bitmap_index", -1)
        if p >= 0 and ex.get("lightmap_png"):
            pages[p] = os.path.join(root, ex["lightmap_png"])
    return pages


def load_ce(path):
    im = np.asarray(Image.open(path).convert("RGBA")).astype(np.float64)
    return im[..., :3] / 255.0, im


def npz(bake, stage, page):
    d = np.load(os.path.join(bake, f"{stage}_{page}.npz"))
    # (Blender keeps rows from the bottom; the page's rows run from v = 0)
    return {k: d[k][::-1].astype(np.float64) for k in d.files}


def box_blur_axis(a, r, axis):
    if r <= 0:
        return a
    a = np.moveaxis(a, axis, 0)
    pad = np.concatenate([np.zeros((r + 1,) + a.shape[1:]), a, np.zeros((r,) + a.shape[1:])], 0)
    c = np.cumsum(pad, 0)
    out = c[2 * r + 1:] - c[:-2 * r - 1]
    return np.moveaxis(out, 0, axis)


def blur3(a, r, passes=3):
    for _ in range(passes):
        for ax in range(3):
            a = box_blur_axis(a, r, ax)
    return a


def fit(o):
    pages = gltf_pages(o.gltf)
    groups = json.load(open(os.path.join(o.bake, "calib_meta.json")))["groups"]
    names = list(groups)
    rows = []
    for page, png in sorted(pages.items()):
        if not os.path.exists(os.path.join(o.bake, f"calib_{page}.npz")):
            continue
        d = npz(o.bake, "calib", page)
        ce, _ = load_ce(png)
        cov = d["position"][..., 3] > 0
        if ce.shape[:2] != cov.shape:
            print(f"page {page}: size differs ({ce.shape} vs {cov.shape}): skipped")
            continue
        flat = np.clip(d["flat"][..., 0], 0.0, 1.0)
        rows.append({
            "page": page, "cov": cov, "pos": d["position"][..., :3][cov],
            "u": ce[cov] * flat[cov][:, None],
            "G": np.stack([d[nm][..., :3][cov] for nm in names], 0),
        })
    u = np.concatenate([r["u"] for r in rows])
    G = np.concatenate([r["G"] for r in rows], 1)  # groups x texels x 3
    pos = np.concatenate([r["pos"] for r in rows])
    X = (G @ LUMA).T  # texels x groups
    ng = len(names)
    # the scales: the mean absolute error in log light (robust to the emitters' own texels,
    # which the bake lights far brighter than any page holds, and fair to dark rooms), by a
    # coarse-to-fine search over log2 scales; each gamma tried
    best = None
    eps = 0.01
    for gamma in ([1.0, 2.2] if o.gamma == "auto" else [float(o.gamma)]):
        t = (u ** gamma) @ LUMA
        lt = np.log(t + eps)
        sub = np.random.default_rng(1).choice(len(t), size=min(len(t), 60000), replace=False)

        def cost(lg):
            pred = X[sub] @ np.where(np.isfinite(lg), 2.0 ** lg, 0.0)
            return np.mean(np.abs(np.log(pred + eps) - lt[sub]))
        lg = np.zeros(ng)
        for i in range(ng):
            if X[:, i].max() <= 0:
                lg[i] = -np.inf
        # (a start near the answer: each emitter at the scale that brings its median to
        # the pages' median where it is the light)
        for i in range(ng):
            if np.isfinite(lg[i]) and names[i].startswith("emit"):
                lit = X[:, i] > 0.5 * X.max(1)
                if lit.sum() > 50:
                    lg[i] = np.log2(max(np.median(t[lit]), 1e-4) / max(np.median(X[lit, i]), 1e-6))
        # (each emitter kept within o.emit_stops of its start: the global error alone
        # may switch off a light whose few texels it would rather leave dark, as b30's
        # dim blue panels; CE's page shows them lit)
        start = lg.copy()
        step = 4.0
        cur = cost(lg)
        while step > 0.02:
            improved = False
            for i in range(ng):
                if not np.isfinite(lg[i]):
                    continue
                for dlt in (-step, step):
                    trial = lg.copy()
                    trial[i] += dlt
                    if names[i].startswith("emit") and abs(trial[i] - start[i]) > o.emit_stops and start[i] != 0:
                        continue
                    c = cost(trial)
                    if c < cur:
                        cur, lg, improved = c, trial, True
            if not improved:
                step /= 2
        coef = np.where(np.isfinite(lg), 2.0 ** lg, 0.0)
        if best is None or cur < best[0]:
            best = (cur, gamma, coef)
    err, gamma, coef = best
    t3 = u ** gamma
    pred3 = np.tensordot(coef, G, 1)
    eps = 0.01
    logr = np.clip(np.log((t3 + eps) / (pred3 + eps)), -np.log(10), np.log(10))
    # the area's field: splatted into a grid of h world units, blurred over about 2.5 cells
    h = o.cell
    # (the field's box: the texels' bulk; outliers, as a far cliff's foot, clamp to its edge)
    pos_lo, pos_hi = np.percentile(pos, 0.2, axis=0), np.percentile(pos, 99.8, axis=0)
    pos = np.clip(pos, pos_lo, pos_hi)
    lo = pos.min(0) - 4 * h
    idx = np.floor((pos - lo) / h).astype(np.int64)
    shape = tuple(idx.max(0) + 5)
    while np.prod(shape) > 12_000_000:
        h *= 1.5
        lo = pos.min(0) - 4 * h
        idx = np.floor((pos - lo) / h).astype(np.int64)
        shape = tuple(idx.max(0) + 5)
    flat_idx = np.ravel_multi_index(idx.T, shape)
    wsum = np.bincount(flat_idx, minlength=int(np.prod(shape))).reshape(shape).astype(np.float64)
    fields = []
    for c in range(3):
        fields.append(np.bincount(flat_idx, weights=logr[:, c], minlength=int(np.prod(shape))).reshape(shape))
    r = max(1, int(round(o.blur)))
    wb = blur3(wsum, r)
    field = np.stack([blur3(f, r) / np.maximum(wb, 1e-9) for f in fields], -1)
    field[wb < 1e-6] = 0.0
    # how well the groups alone, and with the field, match CE (in CE's values)
    def trilinear(p):
        q = (p - lo) / h - 0.5
        i0 = np.floor(q).astype(np.int64)
        f = q - i0
        out = np.zeros((len(p), 3))
        for dx in (0, 1):
            for dy in (0, 1):
                for dz in (0, 1):
                    ii = np.clip(i0 + [dx, dy, dz], 0, np.array(shape) - 1)
                    wgt = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                    out += field[ii[:, 0], ii[:, 1], ii[:, 2]] * wgt[:, None]
        return out
    corr = pred3 * np.exp(trilinear(pos))
    v_ce = u
    v_groups = np.clip(pred3, 0, None) ** (1 / gamma)
    v_field = np.clip(corr, 0, None) ** (1 / gamma)
    report = {
        "gamma": gamma, "scales": {nm + ("" if nm in ("sun", "sky") else " " + groups[nm]): float(c) for nm, c in zip(names, coef)},
        "texels": int(len(u)), "mean_abs_log_error": float(err),
        "mean_abs_ce_value_groups_only": float(np.mean(np.abs(np.clip(v_groups, 0, 1) - v_ce))),
        "mean_abs_ce_value_with_field": float(np.mean(np.abs(np.clip(v_field, 0, 1) - v_ce))),
        "mean_ce_value": float(v_ce.mean()), "mean_value_groups_only": float(np.clip(v_groups, 0, 1).mean()),
        "mean_value_with_field": float(np.clip(v_field, 0, 1).mean()),
        "field_cell_wu": float(h), "field_shape": [int(x) for x in shape], "field_blur_cells": r,
        "field_log_ratio_p5_p50_p95": [float(x) for x in np.percentile(trilinear(pos) @ LUMA, [5, 50, 95])],
    }
    json.dump({"sun": float(coef[names.index("sun")]), "sky": float(coef[names.index("sky")]),
               "emit": {groups[nm]: float(c) for nm, c in zip(names, coef) if nm.startswith("emit")}, "gamma": gamma},
              open(os.path.join(o.bake, "scales.json"), "w"), indent=1)
    np.savez_compressed(os.path.join(o.bake, "field.npz"), field=field.astype(np.float32), lo=lo, h=h)
    json.dump(report, open(os.path.join(o.bake, "fit_report.json"), "w"), indent=1)
    print(json.dumps(report, indent=1))


def trilinear_field(fd, p):
    field, lo, h = fd["field"], fd["lo"], float(fd["h"])
    shape = np.array(field.shape[:3])
    q = (p - lo) / h - 0.5
    i0 = np.floor(q).astype(np.int64)
    f = q - i0
    out = np.zeros((len(p), 3))
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                ii = np.clip(i0 + [dx, dy, dz], 0, shape - 1)
                wgt = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                out += field[ii[:, 0], ii[:, 1], ii[:, 2]] * wgt[:, None]
    return out


def fill_gutters(img, cov, iterations):
    """each uncovered texel the mean of its covered (or already filled) 8 neighbours"""
    img = img.copy()
    have = cov.copy()
    H, W = cov.shape
    for _ in range(iterations):
        if have.all():
            break
        acc = np.zeros_like(img)
        cnt = np.zeros((H, W))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if not dx and not dy:
                    continue
                sh = np.roll(np.roll(img * have[..., None], dy, 0), dx, 1)
                sc = np.roll(np.roll(have.astype(np.float64), dy, 0), dx, 1)
                # (no wrap across the page's edges)
                if dy == 1:
                    sh[0] = 0; sc[0] = 0
                if dy == -1:
                    sh[-1] = 0; sc[-1] = 0
                if dx == 1:
                    sh[:, 0] = 0; sc[:, 0] = 0
                if dx == -1:
                    sh[:, -1] = 0; sc[:, -1] = 0
                acc += sh
                cnt += sc
        new = (~have) & (cnt > 0)
        img[new] = acc[new] / cnt[new][:, None]
        have = have | new
    return img, have


def chart_denoise(img, pos, nrm_like, cov, radius, sigma_p):
    """a small edge-aware blur in the page: neighbours weigh by how near they are in the
    world (charts side by side in the page are far apart there), so a chart never takes
    another's light"""
    H, W = cov.shape
    acc = img * cov[..., None]
    wsum = cov.astype(np.float64).copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if not dx and not dy:
                continue
            sp = np.roll(np.roll(pos, dy, 0), dx, 1)
            si = np.roll(np.roll(img, dy, 0), dx, 1)
            sc = np.roll(np.roll(cov, dy, 0), dx, 1)
            d2 = ((sp - pos) ** 2).sum(-1)
            w = np.exp(-d2 / (2 * sigma_p ** 2)) * sc * cov * np.exp(-(dx * dx + dy * dy) / (2.0 * radius * radius))
            acc += si * w[..., None]
            wsum += w
    out = acc / np.maximum(wsum, 1e-9)[..., None]
    return np.where(cov[..., None], out, img)


def chart_blur_sep(img, pos, cov, radius, sigma_p, passes=1):
    """chart_denoise's edge-aware blur, separable (along u, then v; each pass): wider
    reach for its cost; neighbours weigh by their distance in the page and in the world
    (another chart's texels are far in the world: none of their light)"""
    out = img * cov[..., None]
    for _ in range(passes):
        for axis in (1, 0):
            acc = out * cov[..., None]
            wsum = cov.astype(np.float64).copy()
            for k in range(-radius, radius + 1):
                if not k:
                    continue
                sp = np.roll(pos, k, axis)
                si = np.roll(out, k, axis)
                sc = np.roll(cov, k, axis).astype(np.float64)
                # (no wrap across the page's edges)
                if axis == 1:
                    if k > 0:
                        sc[:, :k] = 0
                    else:
                        sc[:, k:] = 0
                else:
                    if k > 0:
                        sc[:k] = 0
                    else:
                        sc[k:] = 0
                d2 = ((sp - pos) ** 2).sum(-1)
                w = np.exp(-d2 / (2 * sigma_p ** 2)) * sc * cov * np.exp(-(k * k) / (2.0 * (radius / 2.0) ** 2))
                acc += si * w[..., None]
                wsum += w
            out = np.where(cov[..., None], acc / np.maximum(wsum, 1e-9)[..., None], out)
    return np.where(cov[..., None], out, img)


def fnv_argb(rgba8):
    """prism_lightmaps.c's hash: FNV-1a over each texel's ARGB dword's bytes, lowest first
    (B, G, R, A)"""
    b = rgba8[..., [2, 1, 0, 3]].astype(np.uint8).reshape(-1)
    h = 2166136261
    # (vectorised in chunks would be faster; the pages are small)
    for byte in b.tobytes():
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def pool_share(cal, scales, groups):
    """how much of each texel's light (the calib bake's groups at the fit's scales and
    colours) the emitters give, 0..1"""
    rgb = scales.get("rgb") or {}
    tot = np.zeros(cal["position"].shape[:2])
    emit = np.zeros_like(tot)
    for nm, what in (groups or {}).items():
        if nm not in cal:
            continue
        if nm in ("sun", "sky"):
            c, g = scales[nm], rgb.get(nm, [1, 1, 1])
        else:
            c, g = scales["emit"].get(what, 0.0), rgb.get("emit", {}).get(what, [1, 1, 1])
        y = (np.maximum(cal[nm][..., :3], 0.0) * np.array(g)) @ LUMA * c
        tot += y
        if nm not in ("sun", "sky"):
            emit += y
    return np.clip(emit / np.maximum(tot, 1e-9), 0.0, 1.0)


def target_stock(ce, o, page, gamma):
    """what CE shows on a flat surface (encoded), at CE's size, and which texels it covers"""
    cal = npz(o.bake, "calib", page)
    ccov = cal["position"][..., 3] > 0
    flat = np.clip(cal["flat"][..., 0], 0.0, 1.0)
    return ce * flat[..., None], ccov


def exposure_gain(held, o, gamma):
    """one gain (on the encoded light) for the BSP, so that the mean luminance CE's normal
    shows, at CE's size over the covered texels, is CE's times --exposure-aim when it falls
    outside --exposure-min..--exposure-max"""
    def mean_ratio(g):
        num = den = 0.0
        for hp in held:
            f, w0, h0 = hp["factor"], hp["w0"], hp["h0"]
            # (on CE's normal the page shows the light exactly, up to 1: a is cut where it would pass it)
            shown = np.clip(hp["vn"].astype(np.float64) * g, 0, 1)
            cov = hp["cov"]
            s = (shown * cov[..., None]).reshape(h0, f, w0, f, 3).sum((1, 3))
            n = cov.reshape(h0, f, w0, f).sum((1, 3))
            tgt, ccov = hp["target"]
            both = ccov & (n > 0)
            num += float(((s[both] / n[both][:, None]) @ LUMA).sum())
            den += float((tgt[both] @ LUMA).sum())
        return num / max(den, 1e-9)
    r1 = mean_ratio(1.0)
    print(f"exposure: the rebaked light's mean luminance x{r1:.3f} CE's (gain 1)", flush=True)
    if not o.exposure or (o.exposure_min <= r1 <= o.exposure_max):
        return 1.0
    lo, hi = 0.25, 4.0
    for _ in range(30):
        mid = np.sqrt(lo * hi)
        if mean_ratio(mid) < o.exposure_aim:
            lo = mid
        else:
            hi = mid
    g = float(np.sqrt(lo * hi))
    print(f"exposure: gain {g:.3f} (encoded) brings it to x{mean_ratio(g):.3f}", flush=True)
    return g


def encode(o):
    pages = gltf_pages(o.gltf)
    scales = json.load(open(os.path.join(o.bake, "scales.json")))
    groups = json.load(open(os.path.join(o.bake, "calib_meta.json"))).get("groups")
    gamma = scales["gamma"]
    fd = dict(np.load(os.path.join(o.bake, "field.npz")))
    meta = json.load(open(os.path.join(o.bake, "final_meta.json")))
    tilt = meta["tilt"]
    tripod = np.array(meta["tripod"])
    outdir = os.path.join(o.out, "prism_lightmaps", "rebaked")
    os.makedirs(outdir, exist_ok=True)
    if o.previews:
        os.makedirs(o.previews, exist_ok=True)
    summary = []
    held = []
    for page, png in sorted(pages.items()):
        f = os.path.join(o.bake, f"final_{page}.npz")
        if not os.path.exists(f):
            continue
        d = npz(o.bake, "final", page)
        ce, ce8 = load_ce(png)
        h0, w0 = ce.shape[:2]
        cov = d["position"][..., 3] > 0
        H, W = cov.shape
        factor = W // w0
        pos = d["position"][..., :3]
        n = [d[f"n{i}"][..., :3] for i in range(4)]
        # the light: a small edge-aware blur (shadows' edges kept); its direction, from
        # a much wider one (the direction varies slowly; the four bakes' differences are
        # what the path tracer's noise shows most)
        n_dir = [chart_blur_sep(x, pos, cov, o.dir_radius * factor // 4, o.dir_wu, 2) for x in n] if o.dir_radius else n
        if o.denoise:
            n = [chart_blur_sep(x, pos, cov, o.denoise, o.denoise_wu, 1) for x in n]
        # the irradiance on CE's normal, times the area's field (linear)
        E = np.maximum(n[0], 0.0)
        corr = np.zeros((H, W, 3))
        if o.field_strength:
            corr[cov] = trilinear_field(fd, pos[cov])
        k = np.exp(o.field_strength * corr)
        # the area's exposure and colour matched to CE's page at low frequency
        # (what CE shows on a flat surface, blurred chart by chart in the world):
        # the bake keeps its shadows' edges, its bounce and its direction; the
        # mood of each area stays CE's
        ratio = np.ones((H, W, 3))
        fill = np.zeros((H, W, 3))
        if o.match:
            cal = npz(o.bake, "calib", page)
            ccov = cal["position"][..., 3] > 0
            cpos = cal["position"][..., :3]
            flat = np.clip(cal["flat"][..., 0], 0.0, 1.0)
            target = (ce * flat[..., None]) ** gamma
            # the bake at the stock size: the mean of each block's covered texels
            Ek = (E * k * cov[..., None]).reshape(h0, factor, w0, factor, 3).sum((1, 3))
            cnt = cov.reshape(h0, factor, w0, factor).sum((1, 3))
            Ek = Ek / np.maximum(cnt, 1)[..., None]
            both = ccov & (cnt > 0)
            eps = 0.004
            lr = np.zeros((h0, w0, 3))
            if o.match_mode == "log":
                lr[both] = np.log((target[both] + eps) / (Ek[both] + eps))
                lr = chart_denoise(lr, cpos, None, both, o.match_radius, o.match_wu)
                lr = np.clip(lr * o.match, -np.log(o.match_clamp), np.log(o.match_clamp))
            else:
                # the ratio of the blurred sums (CE's and the bake's light, each blurred chart
                # by chart in the world): the area's mean brightness is CE's (a blurred log
                # ratio is a geometric mean, darker), its luminance and its colour corrected
                # apart: the luminance within --match-clamp, the colour (each channel's ratio
                # over the luminance's) within --match-chroma-clamp, so a lamp's pool keeps
                # the hue CE gives it without the bake's own detail being painted over
                # (an emitting shader's own texels, the lamps themselves, are matched texel by
                # texel and kept out of their neighbours' blur: CE's page holds them lit, the
                # bake only lights them from the room, and blurred together a strip of lamp
                # light was lost into the wall about it)
                own = (cal["own"][..., 0] > 0.5) if "own" in cal else np.zeros_like(both)
                bm = both & ~own
                # (a small epsilon: a dark level's light made linear is tiny, 0.05 in CE's
                # encoding is 0.0014, and the first version's 0.004 held its ratio near 1, so
                # a50's dim walls kept the bake's own bounce colour)
                eps = o.match_eps
                tb = chart_denoise(np.where(both[..., None], target, 0.0), cpos, None, bm, o.match_radius, o.match_wu)
                eb = chart_denoise(np.where(both[..., None], Ek, 0.0), cpos, None, bm, o.match_radius, o.match_wu)
                # (in a lamp's pool the match is CE's texel by texel: its pools are sharp, a
                # bright rim along the lamp, and the blur spread it into the wall; the weight
                # is how much of the texel's light the emitters give, at most --match-pool-detail)
                w = np.maximum(pool_share(cal, scales, groups) * o.match_pool_detail, o.match_detail)[..., None]
                tb = w * np.where(both[..., None], target, 0.0) + (1.0 - w) * tb
                eb = w * np.where(both[..., None], Ek, 0.0) + (1.0 - w) * eb
                rl = (tb @ LUMA + eps) / (eb @ LUMA + eps)
                r3 = (tb + eps) / (eb + eps)
                chroma = np.clip(r3 / rl[..., None], 1.0 / o.match_chroma_clamp, o.match_chroma_clamp)
                rl = np.clip(rl, 1.0 / o.match_clamp, o.match_clamp)
                lr = o.match * np.log(rl)[..., None] + o.match_chroma * np.log(chroma)
                # (where the bake has next to no light and CE's page has some, as small
                # charts the path tracer finds shut in, the clamped ratio cannot bring it:
                # the light it falls short by is added, without a direction)
                short = np.maximum(tb - eb * np.exp(lr), 0.0) * o.match_fill
                short[~both] = 0.0
                short, _ = fill_gutters(short, both, 4)
                fill = np.stack([np.asarray(Image.fromarray(short[..., c].astype(np.float32), mode="F").resize(
                    (W, H), Image.BILINEAR)) for c in range(3)], -1).astype(np.float64)
            lr[~both] = 0.0
            lr, _ = fill_gutters(lr, both, 4)
            up = np.stack([np.asarray(Image.fromarray(lr[..., c].astype(np.float32), mode="F").resize((W, H), Image.BILINEAR))
                           for c in range(3)], -1)
            ratio = np.exp(up)
        lin = [np.maximum(x, 0.0) * k * ratio + fill for x in n]
        # Y(n) = alpha + beta . n fitted in CE's encoding (the combiners multiply the
        # encoded texel by the bump's factor): from the normal's bake and the tripod's
        # (the direction's bakes: their ratios to the normal's, smooth, applied to the
        # sharp normal's light)
        Ys = [np.maximum((np.maximum(x, 0.0) * k * ratio + fill) @ LUMA, 0.0) ** (1.0 / gamma) for x in n_dir]
        Y0 = np.maximum(lin[0] @ LUMA, 0.0) ** (1.0 / gamma)
        rel = Y0 / np.maximum(Ys[0], 1e-6)
        Y = [Y0] + [Ys[i] * rel for i in range(1, 4)]
        c, s = np.cos(tilt), np.sin(tilt)
        M = (Y[1] + Y[2] + Y[3]) / 3.0
        bz = (Y[0] - M) / (1.0 - c)
        bx = (2.0 / (3.0 * s)) * sum((Y[i + 1] - M) * tripod[i, 0] for i in range(3))
        by = (2.0 / (3.0 * s)) * sum((Y[i + 1] - M) * tripod[i, 1] for i in range(3))
        alpha = np.maximum(Y[0] - bz, 0.0)
        beta = np.stack([bx, by, bz], -1)
        blen = np.linalg.norm(beta, axis=-1)
        # CE's own form: L (1 - a + a n.d), d a unit tangent-space direction (the
        # vertices' incident radiosity's place), a how much of the light comes from it
        L = alpha + blen
        a = np.where(L > 1e-5, blen / np.maximum(L, 1e-5), 0.0)
        a = np.clip(a, 0.0, o.max_directional)
        d = np.where(blen[..., None] > 1e-6, beta / np.maximum(blen, 1e-6)[..., None], np.array([0.0, 0.0, 1.0]))
        shown_n = 1.0 - a + a * d[..., 2]
        # the page's colour: the normal's light (encoded), lifted so that on CE's
        # normal the combiners show exactly it
        v_n = np.clip(lin[0], 0, None) ** (1.0 / gamma)
        faint = Y[0] < 1e-3
        a[faint] = 0.0
        d[faint] = (0.0, 0.0, 1.0)
        clipped = float(((v_n / np.maximum(shown_n, 0.05)[..., None])[cov].max(-1) > 1.0).mean()) if cov.any() else 0.0
        # gutters: from covered neighbours, then the stock page (bilinear up)
        vn, have = fill_gutters(v_n, cov, 3 * factor)
        d, _ = fill_gutters(d, cov, 3 * factor)
        a2, _ = fill_gutters(a[..., None], cov, 3 * factor)
        if not have.all():
            up = np.asarray(Image.fromarray((ce * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR)).astype(np.float64) / 255
            vn[~have] = up[~have]
            d[~have] = (0.0, 0.0, 1.0)
            a2[~have] = 0.0
        d = d / np.maximum(np.linalg.norm(d, axis=-1, keepdims=True), 1e-6)
        # (kept: the light on CE's normal, encoded; the page's L and a are made from it once
        # the whole BSP's exposure is known)
        held.append({"page": page, "png": png, "ce": ce, "ce8": ce8, "vn": vn.astype(np.float16),
                     "d": d.astype(np.float16), "a2": a2[..., 0].astype(np.float16), "cov": cov,
                     "factor": factor, "w0": w0, "h0": h0, "W": W, "H": H, "clipped": clipped,
                     "target": target_stock(ce, o, page, gamma),
                     "ratio_pct": [float(x) for x in np.percentile(np.log(ratio[cov] @ LUMA), [5, 50, 95])] if cov.any() else None})
    # the BSP's exposure: what CE's normal shows, at CE's size, against CE's own, over the
    # covered texels; one gain for the BSP (never darker than CE: --exposure-min..max,
    # brought to --exposure-aim when outside; a little above 1, since the texel's own
    # direction, more directional than CE's vertices', shows a bumped surface a few percent
    # darker than its flat normal: a10's corridor 3 % at 1.03)
    gain = exposure_gain(held, o, gamma)
    for hp in held:
        page, png, ce, ce8, cov = hp["page"], hp["png"], hp["ce"], hp["ce8"], hp["cov"]
        W, H, w0, h0, factor = hp["W"], hp["H"], hp["w0"], hp["h0"], hp["factor"]
        vg = hp["vn"].astype(np.float64) * gain
        d = hp["d"].astype(np.float64)
        a2 = hp["a2"].astype(np.float64)
        clipped = hp["clipped"]
        # L = (the light on CE's normal) / (1 - a + a d.z) must stay within the page's 8 bits
        # (the combiners clamp it to 1): where it would not, a is cut until it does, so a
        # bright texel keeps its brightness and its colour (clipped, the green of a10's
        # fluorescent light went first) and gives up only some of its directionality
        dz = d[..., 2]
        m = vg.max(-1)
        cap = np.where(dz < 1.0 - 1e-3, (1.0 - m) / np.maximum(1.0 - dz, 1e-3), 1.0)
        cap = np.clip(cap, 0.0, 1.0)
        capped = float((a2[cov] > cap[cov] + 1e-6).mean()) if cov.any() else 0.0
        a2 = np.minimum(a2, cap)
        sn = 1.0 - a2 + a2 * dz
        v = np.clip(vg / np.maximum(sn, 0.05)[..., None], 0.0, 1.0)
        a2 = a2[..., None]
        irr8 = np.concatenate([np.clip(v * 255 + 0.5, 0, 255), np.full((H, W, 1), 255.0)], -1).astype(np.uint8)
        dir8 = np.concatenate([np.clip((d * 0.5 + 0.5) * 255 + 0.5, 0, 255),
                               np.clip(a2 * 255 + 0.5, 0, 255)], -1).astype(np.uint8)
        payload = irr8.tobytes() + dir8.tobytes()
        deflated = zlib.compress(payload, 6)
        stock_hash = fnv_argb(ce8.astype(np.uint8))
        head = struct.pack("<4sIIIIIIII", b"PRL1", 2, w0, h0, factor, stock_hash, 1, len(deflated), len(payload))
        crc = zlib.crc32(head + deflated) & 0xFFFFFFFF
        name = f"{o.map}_{o.bsp}_{page}.prl"
        with open(os.path.join(outdir, name), "wb") as fh:
            fh.write(head + struct.pack("<I", crc) + deflated)
        summary.append({"page": page, "stock": [w0, h0], "factor": factor, "coverage": float(cov.mean()),
                        "exposure_gain_encoded": gain,
                        "mean_value_stock": float(ce.mean()), "mean_value_rebaked": float(v.mean()),
                        "mean_directional": float(a2[cov].mean()) if cov.any() else 0.0,
                        "directional_cut_fraction": capped,
                        "clipped_fraction": clipped,
                        "match_ratio_p5_p50_p95": hp["ratio_pct"],
                        "bytes": 40 + len(deflated)})
        if o.previews:
            up = np.asarray(Image.fromarray((ce * 255).astype(np.uint8)).resize((W, H), Image.NEAREST))
            row = np.concatenate([up, irr8[..., :3], dir8[..., :3]], 1)
            Image.fromarray(row).save(os.path.join(o.previews, f"page_{page}.png"))
    json.dump(summary, open(os.path.join(o.bake, "encode_report.json"), "w"), indent=1)
    tot = sum(s["bytes"] for s in summary)
    print(f"encoded {len(summary)} pages, {tot / 1e6:.1f} MB, into {outdir}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["fit", "encode"])
    ap.add_argument("--gltf", required=True)
    ap.add_argument("--bake", required=True)
    ap.add_argument("--gamma", default="auto")
    ap.add_argument("--cell", type=float, default=1.0)
    ap.add_argument("--emit-stops", type=float, default=2.0)
    ap.add_argument("--blur", type=float, default=2.0)
    ap.add_argument("--map")
    ap.add_argument("--bsp", type=int)
    ap.add_argument("--out")
    ap.add_argument("--field-strength", type=float, default=1.0)
    ap.add_argument("--denoise", type=int, default=3)
    ap.add_argument("--denoise-wu", type=float, default=0.08)
    ap.add_argument("--dir-radius", type=int, default=10)
    ap.add_argument("--dir-wu", type=float, default=0.4)
    ap.add_argument("--previews")
    ap.add_argument("--match", type=float, default=1.0)
    ap.add_argument("--match-radius", type=int, default=4)
    ap.add_argument("--match-wu", type=float, default=1.0)
    ap.add_argument("--match-clamp", type=float, default=32.0)
    ap.add_argument("--max-directional", type=float, default=0.9)
    ap.add_argument("--match-mode", choices=["sums", "log"], default="sums")
    ap.add_argument("--match-chroma", type=float, default=1.0)
    ap.add_argument("--match-fill", type=float, default=1.0)
    ap.add_argument("--match-pool-detail", type=float, default=1.0)
    ap.add_argument("--match-detail", type=float, default=0.0)
    ap.add_argument("--match-eps", type=float, default=1e-5)
    ap.add_argument("--match-chroma-clamp", type=float, default=4.0)
    ap.add_argument("--exposure", type=int, default=1)
    ap.add_argument("--exposure-min", type=float, default=1.04)
    ap.add_argument("--exposure-max", type=float, default=1.10)
    ap.add_argument("--exposure-aim", type=float, default=1.06)
    o = ap.parse_args()
    fit(o) if o.command == "fit" else encode(o)


if __name__ == "__main__":
    main()
