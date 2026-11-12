#!/usr/bin/env python3
"""PRISM lightmap re-bake, stage 2 (second version): each light's strength and colour fitted
to the pools of light CE's own page shows, and the check of a finished bake against CE.

  relight_calib.py fit   --gltf <bsp.gltf> --bake <dir> [--gamma auto|1|2.2] [--lights <map_lights.json> --bsp N]
  relight_calib.py check --gltf <bsp.gltf> --bake <dir> --map <name> --bsp N --rebaked <dir of .prl> [--json out]

fit (after relight_bake.py --stage calib): every covered texel of every page that is not an
emitting shader's own (the calib bake's "own" pass), with
  t   = (CE's texel x CE's flat-bump factor) ^ gamma, what CE shows on a flat surface made
        linear, per channel;
  G_g = the bake of light group g alone (the sky's lights, the sky's ambient, each emitting
        shader at CE's own colour x power), per channel.
Each group's pool is where it gives more than half of the light. The scales start from the
mean-absolute-log-error search of the first version (no limit on how far an emitter moves)
and are then refined pool by pool: c_g = what CE shows in g's pool, less the other groups'
light there, over g's own light there (sums, so the pool's mean brightness is CE's, not its
geometric mean, which is darker). Each emitter's colour gain (r, g, b, luminance kept) is
the same ratio channel by channel: the light's colour is CE's emitted colour turned to the
hue of the pool CE shows (an orange circuit's pool stays orange, a blue strip's light blue).
The sky's lights and its ambient keep CE's colours (--sky-gains 1 fits them too: tinted to
their pools, a50's ambient turned the hangar's fill red).
Writes <bake>/scales.json ("sun", "sky", "emit": {shader: scale}, "rgb": the gains,
"gamma") and <bake>/fit_report.json (per group: its pool's size, CE's and the fit's mean
luminance and chromaticity there). No field (field.npz all zero): the encode's match does
the area's correction, from the ratio of blurred sums.

check (after relight_encode.py encode): the .prl pages read back, the light they show on CE's
normal (L (1 - a + a d.z)) taken to CE's size (each block's covered texels' mean), against
CE's (texel x flat-bump factor), in CE's encoding (what the combiners multiply), over the
covered texels: the exposure (mean luminance ratio), and per group pool its luminance ratio
and its chromaticity distance (CIE 1976 u'v' of the pool's mean colour, made linear with the
fit's gamma). The pools are the fit's (where each group gives more than half of the light).
Also the BSP's colour balance (the mean colour in CE's encoding, its chromaticity with the
display's 2.2) and each lit texel's chromaticity distance, weighted by its light: a tint no
pool shows; and the lamps' own texels.
"""
import argparse
import json
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relight_encode import LUMA, gltf_pages, load_ce, npz  # noqa: E402

# linear sRGB (Rec. 709) to XYZ
M_XYZ = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])


def uv_prime(rgb):
    xyz = np.asarray(rgb, dtype=np.float64) @ M_XYZ.T
    d = xyz[..., 0] + 15 * xyz[..., 1] + 3 * xyz[..., 2]
    d = np.maximum(d, 1e-9)
    return np.stack([4 * xyz[..., 0] / d, 9 * xyz[..., 1] / d], -1)


def load_calib(o):
    pages = gltf_pages(o.gltf)
    meta = json.load(open(os.path.join(o.bake, "calib_meta.json")))
    groups = meta["groups"]
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
        own = d["own"][..., 0] > 0.5 if "own" in d else np.zeros(cov.shape, bool)
        rows.append({"page": page, "cov": cov, "own": own[cov], "u": ce[cov] * flat[cov][:, None],
                     "G": np.stack([np.maximum(d[nm][..., :3][cov], 0.0) for nm in names], 0)})
    return names, groups, rows


def search_scales(X, t, names, valid, eps=0.01):
    """the first version's search: mean absolute log error, coarse to fine over log2 scales"""
    ng = len(names)
    lt = np.log(t + eps)
    idx = np.flatnonzero(valid)
    sub = np.random.default_rng(1).choice(idx, size=min(len(idx), 60000), replace=False)

    def cost(lg):
        pred = X[sub] @ np.where(np.isfinite(lg), 2.0 ** lg, 0.0)
        return np.mean(np.abs(np.log(pred + eps) - lt[sub]))
    lg = np.zeros(ng)
    for i in range(ng):
        if X[valid, i].max(initial=0) <= 0:
            lg[i] = -np.inf
    for i in range(ng):
        if np.isfinite(lg[i]) and names[i].startswith("emit"):
            lit = valid & (X[:, i] > 0.5 * X.max(1))
            if lit.sum() > 50:
                lg[i] = np.log2(max(np.median(t[lit]), 1e-4) / max(np.median(X[lit, i]), 1e-6))
    start = np.where(np.isfinite(lg), 2.0 ** lg, 0.0)
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
                c = cost(trial)
                if c < cur:
                    cur, lg, improved = c, trial, True
        if not improved:
            step /= 2
    coef = np.where(np.isfinite(lg), 2.0 ** lg, 0.0)
    # (the global error may switch off a light whose few texels it would rather leave dark,
    # as a10's red lamps; CE's page shows their pools: such a light goes back to its start,
    # the pools then fit it)
    for i in range(ng):
        if names[i].startswith("emit") and start[i] > 0 and coef[i] < start[i] / 8:
            coef[i] = start[i]
    return coef, cur


def pools_fit(G, t3, names, valid, coef, iters, min_pool, gain_clamp, sky_gains=False):
    """the pool-by-pool refinement: scales (luminance) and colour gains, alternately"""
    ng = len(names)
    gains = np.ones((ng, 3))
    t = t3 @ LUMA
    for it in range(iters):
        Gc = G * gains[:, None, :]
        P = coef[:, None, None] * Gc                     # groups x texels x 3
        pred3 = P.sum(0)
        pred = pred3 @ LUMA
        share = (P @ LUMA) / np.maximum(pred, 1e-9)[None]
        for g in range(ng):
            if coef[g] <= 0:
                continue
            pool = valid & (share[g] > 0.5)
            fixed_colour = names[g] in ("sun", "sky") and not sky_gains
            if pool.sum() < min_pool:
                continue
            own_l = (P[g][pool] @ LUMA)
            rest = pred[pool] - own_l
            want = np.maximum(t[pool] - rest, 0.0)
            new = coef[g] * want.sum() / max(own_l.sum(), 1e-12)
            # (damped, in log: the pools move as the others' light does)
            coef[g] = float(np.exp(0.5 * np.log(max(coef[g], 1e-9)) + 0.5 * np.log(max(new, 1e-9))))
            # the colour: channel by channel the same ratio, luminance kept (the sky's lights
            # and its ambient keep CE's colour: tinted to its pools, a50's sky turned the whole
            # hangar's fill red; the emitters' pools are where a hue is the light's own)
            if fixed_colour:
                continue
            own3 = P[g][pool].sum(0)
            want3 = np.maximum(t3[pool] - (pred3[pool] - P[g][pool]), 0.0).sum(0)
            ratio = np.where(own3 > 1e-9, want3 / np.maximum(own3, 1e-12), 1.0)
            base = G[g][pool].sum(0)

            def lum_kept(k):
                return k * (base @ LUMA) / max((k * base) @ LUMA, 1e-12)
            ng_ = lum_kept(np.clip(gains[g] * ratio, 1.0 / gain_clamp, gain_clamp))
            gains[g] = lum_kept(np.sqrt(gains[g] * ng_))
    return coef, gains


def pool_stats(G, gains, coef, t3, valid, names, gamma, min_pool):
    P = coef[:, None, None] * (G * gains[:, None, :])
    pred3 = P.sum(0)
    share = (P @ LUMA) / np.maximum(pred3 @ LUMA, 1e-9)[None]
    out = {}
    for g, nm in enumerate(names):
        pool = valid & (share[g] > 0.5)
        if pool.sum() < min_pool:
            continue
        ce = t3[pool].mean(0)
        fit = pred3[pool].mean(0)
        out[nm] = {"texels": int(pool.sum()),
                   "ce_luma_encoded": float(np.mean((t3[pool] @ LUMA) ** (1 / gamma))),
                   "fit_luma_encoded": float(np.mean((pred3[pool] @ LUMA) ** (1 / gamma))),
                   "ce_uv": [round(float(x), 4) for x in uv_prime(ce)],
                   "fit_uv": [round(float(x), 4) for x in uv_prime(fit)],
                   "duv": float(np.linalg.norm(uv_prime(ce) - uv_prime(fit)))}
    return out, share


def fit(o):
    names, groups, rows = load_calib(o)
    u = np.concatenate([r["u"] for r in rows])
    own = np.concatenate([r["own"] for r in rows])
    G = np.concatenate([r["G"] for r in rows], 1)
    X = (G @ LUMA).T
    best = None
    for gamma in ([1.0, 2.2] if o.gamma == "auto" else [float(o.gamma)]):
        t3 = u ** gamma
        t = t3 @ LUMA
        valid = ~own & ((t > 1e-5) | (X.sum(1) > 1e-6))
        coef0, err0 = search_scales(X, t, names, valid)
        coef, gains = pools_fit(G, t3, names, valid, coef0.copy(), o.iters, o.min_pool, o.gain_clamp, o.sky_gains)
        pred = (coef[:, None, None] * G * gains[:, None, :]).sum(0)
        err = float(np.mean(np.abs(np.log(pred[valid] @ LUMA + 0.01) - np.log(t[valid] + 0.01))))
        print(f"gamma {gamma}: log error search {err0:.4f}, after the pools {err:.4f}", flush=True)
        if best is None or err < best[0]:
            best = (err, gamma, coef, gains, coef0, valid)
    err, gamma, coef, gains, coef0, valid = best
    t3 = u ** gamma
    stats, _ = pool_stats(G, gains, coef, t3, valid, names, gamma, o.min_pool)
    stats0, _ = pool_stats(G, np.ones_like(gains), coef0, t3, valid, names, gamma, o.min_pool)
    pred = (coef[:, None, None] * G * gains[:, None, :]).sum(0)
    v_fit = np.clip(pred[valid], 0, None) ** (1 / gamma)
    report = {
        "method": "pools", "gamma": gamma, "texels": int(valid.sum()), "own_texels_left_out": int(own.sum()),
        "mean_abs_log_error": err,
        "mean_ce_value": float(u[valid].mean()), "mean_value_fit": float(np.clip(v_fit, 0, 1).mean()),
        "groups": {nm + ("" if nm in ("sun", "sky") else " " + groups[nm]): {
            "scale": float(c), "scale_first_search": float(c0), "rgb_gain": [round(float(x), 4) for x in gn],
            "pool": stats.get(nm), "pool_first_search": stats0.get(nm)}
            for nm, c, c0, gn in zip(names, coef, coef0, gains)},
    }
    if o.lights:
        # (CE's own data beside the fit: each emitter's radiosity colour and its self-illumination)
        L = json.load(open(o.lights))
        info = {}
        for mi in L["bsps"][o.bsp]["materials"]:
            info.setdefault(mi["shader"], {"radiosity": mi.get("radiosity"), "self_illumination": mi.get("self_illumination")})
        for nm in names:
            if nm.startswith("emit"):
                report["groups"][nm + " " + groups[nm]]["ce"] = info.get(groups[nm])
    scales = {"sun": float(coef[names.index("sun")]), "sky": float(coef[names.index("sky")]),
              "emit": {groups[nm]: float(c) for nm, c in zip(names, coef) if nm.startswith("emit")},
              "rgb": {"sun": [float(x) for x in gains[names.index("sun")]],
                      "sky": [float(x) for x in gains[names.index("sky")]],
                      "emit": {groups[nm]: [float(x) for x in gn] for nm, gn in zip(names, gains) if nm.startswith("emit")}},
              "gamma": gamma, "method": "pools"}
    json.dump(scales, open(os.path.join(o.bake, "scales.json"), "w"), indent=1)
    np.savez_compressed(os.path.join(o.bake, "field.npz"), field=np.zeros((2, 2, 2, 3), np.float32),
                        lo=np.zeros(3), h=1.0)
    json.dump(report, open(os.path.join(o.bake, "fit_report.json"), "w"), indent=1)
    for k, g in report["groups"].items():
        p, p0 = g["pool"], g["pool_first_search"]
        line = f"  {k[:60]:60s} scale {g['scale']:.4g} (search {g['scale_first_search']:.4g}) gain {g['rgb_gain']}"
        if p:
            line += f"  pool {p['texels']}: CE {p['ce_luma_encoded']:.3f} fit {p['fit_luma_encoded']:.3f} duv {p['duv']:.4f}"
        if p0:
            line += f" (search: {p0['fit_luma_encoded']:.3f}, duv {p0['duv']:.4f})"
        print(line)
    print(json.dumps({k: v for k, v in report.items() if k != "groups"}, indent=1))


def read_prl(path):
    b = open(path, "rb").read()
    magic, ver, w0, h0, factor, h, flags, dlen, ilen = struct.unpack("<4sIIIIIIII", b[:36])
    raw = zlib.decompress(b[40:40 + dlen])
    W, H = w0 * factor, h0 * factor
    irr = np.frombuffer(raw[:W * H * 4], np.uint8).reshape(H, W, 4).astype(np.float64) / 255
    dr = np.frombuffer(raw[W * H * 4:], np.uint8).reshape(H, W, 4).astype(np.float64) / 255
    return irr[..., :3], dr[..., :3] * 2 - 1, dr[..., 3], factor


def check(o):
    """the finished pages against CE: exposure, and each pool's luminance and hue"""
    names, groups, rows = load_calib(o)
    scales = json.load(open(os.path.join(o.bake, "scales.json")))
    gamma = scales["gamma"]
    coef = np.array([scales["sun"], scales["sky"]] + [scales["emit"].get(groups[n], 0.0) for n in names[2:]])
    rgb = scales.get("rgb") or {}
    gains = np.array([rgb.get("sun", [1, 1, 1]), rgb.get("sky", [1, 1, 1])] +
                     [rgb.get("emit", {}).get(groups[n], [1, 1, 1]) for n in names[2:]])
    pages = gltf_pages(o.gltf)
    ce_all, rb_all, share_all, valid_all = [], [], [], []
    per_page = []
    for r in rows:
        page = r["page"]
        f = os.path.join(o.rebaked, f"{o.map}_{o.bsp}_{page}.prl")
        if not os.path.exists(f):
            continue
        v, d, a, factor = read_prl(f)
        shown = v * (1 - a + a * d[..., 2])[..., None]
        H, W = shown.shape[:2]
        h0, w0 = H // factor, W // factor
        fin = npz(o.bake, "final", page) if os.path.exists(os.path.join(o.bake, f"final_{page}.npz")) else None
        if fin is not None and fin["position"].shape[:2] == (H, W):
            fcov = fin["position"][..., 3] > 0
        else:
            fcov = np.ones((H, W), bool)
        s = (shown * fcov[..., None]).reshape(h0, factor, w0, factor, 3).sum((1, 3))
        n = fcov.reshape(h0, factor, w0, factor).sum((1, 3))
        s = s / np.maximum(n, 1)[..., None]
        cov = r["cov"]
        both = n[cov] > 0
        rb = s[cov][both]
        ce = r["u"][both]
        Gp = r["G"][:, both]
        P = coef[:, None, None] * (Gp * gains[:, None, :])
        share = (P @ LUMA) / np.maximum(P.sum(0) @ LUMA, 1e-9)[None]
        ce_all.append(ce)
        rb_all.append(rb)
        share_all.append(share)
        valid_all.append(~r["own"][both])
        per_page.append({"page": page, "ce_mean_luma": float((ce @ LUMA).mean()), "rebaked_mean_luma": float((rb @ LUMA).mean())})
    ce = np.concatenate(ce_all)
    rb = np.concatenate(rb_all)
    share = np.concatenate(share_all, 1)
    valid = np.concatenate(valid_all)
    out = {"map": o.map, "bsp": o.bsp, "rebaked": o.rebaked, "gamma": gamma, "texels": int(len(ce)),
           "exposure_ratio": float((rb @ LUMA).mean() / max((ce @ LUMA).mean(), 1e-9)),
           "ce_mean_luma": float((ce @ LUMA).mean()), "rebaked_mean_luma": float((rb @ LUMA).mean()),
           "pools": {}, "pages": per_page}
    for g, nm in enumerate(names):
        pool = valid & (share[g] > 0.5)
        if pool.sum() < o.min_pool:
            continue
        lce, lrb = (ce[pool] ** gamma).mean(0), (rb[pool] ** gamma).mean(0)
        # (the pool's chroma: the colourfulness of its mean colour, u'v' distance from D65's white)
        white = uv_prime(np.ones(3))
        out["pools"][nm if nm in ("sun", "sky") else groups[nm]] = {
            "texels": int(pool.sum()),
            "luma_ratio": float((rb[pool] @ LUMA).mean() / max((ce[pool] @ LUMA).mean(), 1e-9)),
            "duv": float(np.linalg.norm(uv_prime(lce) - uv_prime(lrb))),
            "ce_chroma": float(np.linalg.norm(uv_prime(lce) - white)),
            "rebaked_chroma": float(np.linalg.norm(uv_prime(lrb) - white))}
    # the whole BSP's colour balance (the mean colour, linear) and each texel's hue
    # against CE's, weighted by its light (catches a tint no pool shows)
    # (in CE's encoding, what the screen averages: dim texels count as they show; their
    # chromaticity from the values made linear with 2.2, the display's)
    out["balance_duv"] = float(np.linalg.norm(uv_prime(ce.mean(0) ** 2.2) - uv_prime(rb.mean(0) ** 2.2)))
    wt = ce @ LUMA
    lit = wt > 0.02
    out["texel_duv_weighted"] = float((np.linalg.norm(uv_prime(ce[lit] ** 2.2) - uv_prime(rb[lit] ** 2.2), axis=-1)
                                       * wt[lit]).sum() / max(wt[lit].sum(), 1e-9))
    lamps = ~valid
    if lamps.sum() >= o.min_pool:
        # (the lamps themselves: the emitting shaders' own texels)
        lce, lrb = (ce[lamps] ** gamma).mean(0), (rb[lamps] ** gamma).mean(0)
        out["lamp_texels"] = {"texels": int(lamps.sum()),
                              "luma_ratio": float((rb[lamps] @ LUMA).mean() / max((ce[lamps] @ LUMA).mean(), 1e-9)),
                              "duv": float(np.linalg.norm(uv_prime(lce) - uv_prime(lrb)))}
    if o.json:
        json.dump(out, open(o.json, "w"), indent=1)
    print(json.dumps({k: v for k, v in out.items() if k != "pages"}, indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["fit", "check"])
    ap.add_argument("--gltf", required=True)
    ap.add_argument("--bake", required=True)
    ap.add_argument("--gamma", default="auto")
    ap.add_argument("--iters", type=int, default=12)
    ap.add_argument("--min-pool", type=int, default=40)
    ap.add_argument("--gain-clamp", type=float, default=4.0)
    ap.add_argument("--sky-gains", type=int, default=0)
    ap.add_argument("--lights")
    ap.add_argument("--bsp", type=int, default=0)
    ap.add_argument("--map")
    ap.add_argument("--rebaked")
    ap.add_argument("--json")
    o = ap.parse_args()
    fit(o) if o.command == "fit" else check(o)


if __name__ == "__main__":
    main()
