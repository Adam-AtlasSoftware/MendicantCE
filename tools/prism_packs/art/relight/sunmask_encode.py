#!/usr/bin/env python3
"""PRISM baked shadows, stage 2: the sun masks the engine reads (display.prism_baked_shadows; prism_lightmaps.c's
.psm pages), from sunmask_bake.py's bakes.

  sunmask_encode.py --gltf <bsp.gltf> --bake <sunmask bake dir> --map <name> --bsp <index> --out <data root>
                    [--calib <relight calib dir>] [--previews <dir>] [--floor 0.15] [--trust 0.15]

Per page the sun reaches, at each texel of the fine bake (F times CE's density):

  A  = what is not the sun's direct light: the calibration's light at CE's size (relight_bake.py's groups at the
       fit's scales, CE's linear units) less the sun's direct light as CE's page holds it (du x vce), at least
       --floor of it (the shadow's side never darker than CE's sky and bounce allow);
  S  = the sun's direct light unshadowed (du at the fit's sun scale), A and S bilinear from CE's size, gutters
       filled from their charts;
  f  = (A + S vhi) / (A + S v_page): the sun's light as the fine bake sees it over the light the page holds where
       it is drawn, v_page the visibility the page itself has (vce bilinear at CE's size for the stock page and its
       smooth copies; vrb bilinear at 4x for the rebaked page); as the page's encoding has it, f ^ (1 / gamma)
       (the fit's gamma: the pages hold light ^ (1 / gamma)).

f is 1 wherever the page holds the sun's light as finely as the bake (all lit, all shadowed, no sun): only near a
shadow's edge does it move light from one side to the other, so the area's brightness, colour and mood stay the
page's. Coded log2(f) x 127 / 3 + 128 (1/8 to 8; 128 exactly 1).

The page is cut into tiles of 4 x 4 CE texels (4F x 4F fine texels); a tile whose f is 1 throughout (both
channels within a code) and whose visibility is even is "uniform" (its visibility alone, in its entry); the rest
are stored whole.

<out>/prism_lightmaps/sun/<map>_<bsp>_<page>.psm (local only, never shipped), little endian:
   0 "PSM1"   4 version (1)   8 CE's page width   12 height   16 the factor F   20 tile size in CE texels (4)
  24 the stock page's hash (prism_lightmaps.c's FNV-1a of its decoded ARGB)   28 flags (0)
  32 tiles across   36 tiles down   40 tiles stored   44 deflated size   48 inflated size
  52 CRC-32 of bytes 0..51 and the deflated bytes;  56 the deflated bytes:
    the entries, tiles across x down, 4 bytes each: the stored tile's number + 1 (16 bits; 0 uniform), the
      visibility (0-255), 0;
    the stored tiles, each 4F x 4F texels of RGBA: R f against the stock page, G f against the rebaked page,
      B the sun's visibility (0-255), A 255; rows from v = 0.
"""
import argparse
import json
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relight_encode import fill_gutters, fnv_argb, gltf_pages  # noqa: E402

LUMA = np.array([0.2126, 0.7152, 0.0722])
LOG_RANGE = 3.0
TILE_CE = 4


def bilinear(img, out_h, out_w):
    """img (h x w [x c]) sampled at the centres of an out_h x out_w grid over the same page, clamped at its
    edges (a texture sampler's bilinear)"""
    h, w = img.shape[:2]
    y = (np.arange(out_h) + 0.5) * h / out_h - 0.5
    x = (np.arange(out_w) + 0.5) * w / out_w - 0.5
    y0 = np.clip(np.floor(y).astype(int), 0, h - 1)
    x0 = np.clip(np.floor(x).astype(int), 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    fy = np.clip(y - np.floor(y), 0, 1)
    fx = np.clip(x - np.floor(x), 0, 1)
    if img.ndim == 3:
        fy = fy[:, None, None]
        fx = fx[None, :, None]
    else:
        fy = fy[:, None]
        fx = fx[None, :]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bottom = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return top * (1 - fy) + bottom * fy


def filled(a, cov, iterations=6):
    img, have = fill_gutters(a[..., None] if a.ndim == 2 else a, cov, iterations)
    img = img[..., 0] if a.ndim == 2 else img
    rest = ~have
    if rest.any():
        img[rest] = img[have].mean() if have.any() else 0.0
    return img


def fast_fill(img, have, iterations):
    """each uncovered texel the mean of its covered (or already filled) 4 neighbours, iterations times (slices, no
    copies of the whole image a step: the fine pages are large)"""
    img = img.copy()
    have = have.copy()
    for _ in range(iterations):
        acc = np.zeros_like(img)
        cnt = np.zeros(have.shape, np.float32)
        hv = have.astype(np.float32)
        wi = img * hv[..., None]
        acc[1:] += wi[:-1]
        cnt[1:] += hv[:-1]
        acc[:-1] += wi[1:]
        cnt[:-1] += hv[1:]
        acc[:, 1:] += wi[:, :-1]
        cnt[:, 1:] += hv[:, :-1]
        acc[:, :-1] += wi[:, 1:]
        cnt[:, :-1] += hv[:, 1:]
        new = (~have) & (cnt > 0)
        if not new.any():
            break
        img[new] = acc[new] / cnt[new][:, None]
        have |= new
    return img, have


def blur2(a, weight, r):
    """normalized convolution of a over the texels weight marks, a (2r+1) box twice; 1 where none is near"""
    def box(x):
        for axis in (0, 1):
            x = np.moveaxis(x, axis, 0)
            pad = np.concatenate([np.zeros((r + 1,) + x.shape[1:]), x, np.zeros((r,) + x.shape[1:])], 0)
            c = np.cumsum(pad, 0)
            x = np.moveaxis(c[2 * r + 1:] - c[:-2 * r - 1], 0, axis)
        return x
    w = weight.astype(np.float64)
    num, den = box(box(a * w)), box(box(w))
    return np.where(den > 1e-6, num / np.maximum(den, 1e-6), 1.0)


def page_visibility(y, A, S, v_model, cov, r=6, edge_reach=2):
    """the sun's visibility a page's light y (CE's linear units, luminance) holds: (y / k - A) / S, k the page's
    gain over the bake about the texel (the ratio of the two blurred over r texels); the bake's own visibility where
    the sun is under a fifth of the light"""
    model = A + S * v_model
    k = blur2(y, cov, r) / np.maximum(blur2(model, cov, r), 1e-6)
    v = np.clip((y / np.maximum(k, 1e-3) - A) / np.maximum(S, 1e-6), 0.0, 1.0)
    sunny = cov & (S > 0.2 * (A + S))
    # (only about the bake's own shadows' edges, a few texels each side: elsewhere the page's light is its own
    # (its detail, its noise), never taken for the sun's)
    gy, gx = np.gradient(blur2(v_model, cov, 1))
    edge = blur2((np.hypot(gx, gy) > 0.12).astype(np.float64), np.ones_like(cov), edge_reach) > 1e-3
    return np.where(sunny & edge, v, v_model)


def read_prl(path, shape):
    """a rebaked page's light as CE's flat normal shows it ((L (1 - a + a d.z)), CE's encoding, luminance), rows
    from v = 0; None without one of this size"""
    if not os.path.exists(path):
        return None
    b = open(path, "rb").read()
    w0, h0, factor = struct.unpack_from("<III", b, 8)
    W, H = w0 * factor, h0 * factor
    if (H, W) != tuple(shape):
        return None
    p = zlib.decompress(b[40:])
    L = np.frombuffer(p[:W * H * 4], np.uint8).reshape(H, W, 4)[..., :3].astype(np.float64) / 255.0
    D = np.frombuffer(p[W * H * 4:], np.uint8).reshape(H, W, 4).astype(np.float64) / 255.0
    dz = D[..., 2] * 2 - 1
    a = D[..., 3]
    return (L * (1 - a + a * dz)[..., None]) @ LUMA


def code_of(f):
    return np.clip(np.round(128 + np.log2(np.maximum(f, 1e-6)) * 127 / LOG_RANGE), 1, 255)


def encode_page(o, meta, page, gamma, s_sun):
    d = np.load(os.path.join(o.bake, f"sun_{page}.npz"))
    pm = meta["pages"][str(page)]
    F = pm["factor"]
    W, H = pm["size"]
    total, cov1 = d["total"].astype(np.float64), d["cov"].astype(bool)
    du = d["du"][..., :3].astype(np.float64) @ LUMA * s_sun
    vce_raw = d["vce"].astype(np.float64)
    vce_cov = vce_raw[..., 3] > 0
    vce = np.clip(vce_raw[..., 0], 0, 1)
    vrb_raw = d["vrb"].astype(np.float64)
    vrb_cov = vrb_raw[..., 3] > 0
    vrb = np.clip(vrb_raw[..., 0], 0, 1)
    if "vhi8" in d.files:
        hcov = d["vhi_cov"].astype(bool)
        vhi = d["vhi8"].astype(np.float64) / 255.0
    else:
        vhi_raw = d["vhi"].astype(np.float64)
        hcov = vhi_raw[..., 3] > 0
        vhi = np.clip(vhi_raw[..., 0], 0, 1)
    cov = cov1 & vce_cov
    # (where the page holds less light than the sun the bake finds there would give it, the sun's share is cut to
    # what the page holds: a ratio of the page's light to the sun's, over the sunlit texels, spread over a few
    # texels; 1 where none is near)
    lit1 = cov & (vce > 0.5) & (du > 1e-4)
    ratio = np.where(lit1, (1.0 - o.floor) * total / np.maximum(du * vce, 1e-6), 0.0)
    cut = blur2(np.minimum(ratio, 1.0), lit1, 4)
    du = du * np.where(cov, cut, 1.0)
    S = filled(du, cov)
    A = filled(np.maximum(total - du * vce, o.floor * total), cov)
    # (the visibility each page actually holds, read from its own light where the sun is much of it: the stock
    # page's (its leaks, its blur) and the rebaked page's (its encoding's smoothing), else the bake's)
    flat = None
    if o.calib and os.path.exists(os.path.join(o.calib, f"calib_{page}.npz")):
        flat = np.clip(np.load(os.path.join(o.calib, f"calib_{page}.npz"))["flat"][::-1][..., 0], 0.0, 1.0)
    ce = np.asarray(Image.open(o.pages[page]).convert("RGB")).astype(np.float64) / 255.0
    if flat is not None and ce.shape[:2] == cov.shape:
        y_ce = ((ce * flat[..., None]) ** gamma) @ LUMA
        vce = page_visibility(y_ce, A, S, vce, cov)
    Vce = filled(vce, cov)
    rb = read_prl(os.path.join(o.out, "prism_lightmaps", "rebaked", f"{o.map}_{o.bsp}_{page}.prl"), vrb.shape)
    if rb is not None:
        r4, c4 = vrb.shape
        A4, S4 = bilinear(A, r4, c4), bilinear(S, r4, c4)
        vrb = page_visibility(rb ** gamma, A4, S4, vrb, vrb_cov, edge_reach=4)
    Vrb = filled(vrb, vrb_cov)
    hh, hw = vhi.shape
    vhi = vhi.astype(np.float32)
    S_h = bilinear(S.astype(np.float32), hh, hw)
    A_h = bilinear(A.astype(np.float32), hh, hw)
    lit = A_h + S_h * vhi
    vlo_ce = bilinear(Vce.astype(np.float32), hh, hw)
    vlo_rb = bilinear(Vrb.astype(np.float32), hh, hw)
    f_ce = (lit + 1e-5) / (A_h + S_h * vlo_ce + 1e-5)
    f_rb = (lit + 1e-5) / (A_h + S_h * vlo_rb + 1e-5)
    # (light added only where the page holds some of the sun: where it holds none at all the fine bake and the
    # page's geometry disagree (a wall the remodel moved, a window), and the page is trusted)
    f_ce = np.where(f_ce > 1, 1 + (f_ce - 1) * np.clip(vlo_ce / o.trust, 0, 1), f_ce)
    f_rb = np.where(f_rb > 1, 1 + (f_rb - 1) * np.clip(vlo_rb / o.trust, 0, 1), f_rb)
    f_ce = f_ce ** (1.0 / gamma)
    f_rb = f_rb ** (1.0 / gamma)
    # (the fine texels no triangle covers: their covered neighbours' log f, so that the bilinear read at a chart's
    # edge does not take the neutral 1 in; the rest neutral)
    lf = np.stack([np.log2(f_ce), np.log2(f_rb), vhi], -1).astype(np.float32)
    lf, have = fast_fill(np.where(hcov[..., None], lf, 0.0).astype(np.float32), hcov, 4)
    lf[~have] = [0.0, 0.0, 1.0]
    rgba = np.empty((hh, hw, 4), np.uint8)
    rgba[..., 0] = code_of(2.0 ** lf[..., 0])
    rgba[..., 1] = code_of(2.0 ** lf[..., 1])
    rgba[..., 2] = np.clip(np.round(lf[..., 2] * 255), 0, 255)
    rgba[..., 3] = 255
    TS = TILE_CE * F
    tx, ty = (W + TILE_CE - 1) // TILE_CE, (H + TILE_CE - 1) // TILE_CE
    pad = np.zeros((ty * TS, tx * TS, 4), np.uint8)
    pad[..., :2] = 128
    pad[..., 2:] = 255
    pad[:hh, :hw] = rgba
    tiles = pad.reshape(ty, TS, tx, TS, 4).transpose(0, 2, 1, 3, 4)
    entries = np.zeros((ty, tx, 4), np.uint8)
    stored = []
    for j in range(ty):
        for i in range(tx):
            t = tiles[j, i]
            c = t[..., :2].astype(int)
            v = t[..., 2].astype(int)
            if np.abs(c - 128).max() <= 1 and v.max() - v.min() <= 8:
                entries[j, i] = [0, 0, int(round(v.mean())), 0]
            else:
                n = len(stored) + 1
                if n > 65535:
                    raise SystemExit(f"page {page}: more than 65535 tiles")
                entries[j, i] = [n & 255, n >> 8, int(round(v.mean())), 0]
                stored.append(t)
    payload = entries.tobytes() + b"".join(t.tobytes() for t in stored)
    deflated = zlib.compress(payload, 9)
    ce8 = np.asarray(Image.open(o.pages[page]).convert("RGBA"))
    head = struct.pack("<4sIIIIIIIIIIII", b"PSM1", 1, W, H, F, TILE_CE, fnv_argb(ce8), 0, tx, ty, len(stored),
                       len(deflated), len(payload))
    crc = zlib.crc32(head + deflated) & 0xFFFFFFFF
    folder = os.path.join(o.out, "prism_lightmaps", "sun")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"{o.map}_{o.bsp}_{page}.psm")
    with open(path + ".part", "wb") as fh:
        fh.write(head + struct.pack("<I", crc) + deflated)
    os.replace(path + ".part", path)
    edge = hcov & ((np.abs(lf[..., 0]) > 0.05) | (np.abs(lf[..., 1]) > 0.05))
    stats = {"page": page, "factor": F, "fine": [hw, hh], "tiles": tx * ty, "stored": len(stored),
             "stored_share": len(stored) / float(tx * ty), "bytes": len(deflated) + 56,
             "gpu_bytes_atlas": len(stored) * TS * TS * 4,
             "f_ce_p1_p99": [float(np.percentile(2 ** lf[..., 0][hcov], 1)), float(np.percentile(2 ** lf[..., 0][hcov], 99))]
             if hcov.any() else None,
             "f_rb_p1_p99": [float(np.percentile(2 ** lf[..., 1][hcov], 1)), float(np.percentile(2 ** lf[..., 1][hcov], 99))]
             if hcov.any() else None,
             "edge_share": float(edge.sum() / max(hcov.sum(), 1)),
             # (the mood: the mean f over the covered texels, linear light, against the page)
             "mean_f_ce_linear": float(np.mean((2 ** lf[..., 0][hcov]) ** gamma)) if hcov.any() else 1.0,
             "mean_f_rb_linear": float(np.mean((2 ** lf[..., 1][hcov]) ** gamma)) if hcov.any() else 1.0}
    if o.previews:
        os.makedirs(o.previews, exist_ok=True)
        Image.fromarray((np.clip(lf[..., 2], 0, 1) * 255).astype(np.uint8)).save(
            os.path.join(o.previews, f"{o.map}_{o.bsp}_{page}_vhi.png"))
        Image.fromarray((np.clip(Vrb, 0, 1) * 255).astype(np.uint8)).save(
            os.path.join(o.previews, f"{o.map}_{o.bsp}_{page}_vrb.png"))
        Image.fromarray(np.stack([rgba[..., 0], rgba[..., 1], rgba[..., 2]], -1)).save(
            os.path.join(o.previews, f"{o.map}_{o.bsp}_{page}_codes.png"))
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gltf", required=True)
    ap.add_argument("--bake", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--bsp", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--previews")
    ap.add_argument("--floor", type=float, default=0.15)
    ap.add_argument("--trust", type=float, default=0.15)
    ap.add_argument("--calib", help="relight_bake.py's calibration bake (CE's flat-bump factor per texel)")
    o = ap.parse_args()
    o.pages = gltf_pages(o.gltf)
    meta = json.load(open(os.path.join(o.bake, "sun_meta.json")))
    gamma = float(meta.get("gamma", 1.0))
    s_sun = float(meta["scales"]["sun"])
    report = []
    for page in sorted(int(p) for p in meta["pages"]):
        st = encode_page(o, meta, page, gamma, s_sun)
        report.append(st)
        print(json.dumps(st), flush=True)
    total = {"pages": len(report), "bytes": sum(r["bytes"] for r in report),
             "gpu_bytes_atlas": sum(r["gpu_bytes_atlas"] for r in report),
             "stored": sum(r["stored"] for r in report), "tiles": sum(r["tiles"] for r in report)}
    print("total", json.dumps(total), flush=True)
    json.dump({"pages": report, "total": total}, open(os.path.join(o.bake, "encode_report.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
