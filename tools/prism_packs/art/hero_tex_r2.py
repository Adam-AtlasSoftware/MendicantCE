#!/usr/bin/env python3
"""PRISM v2 round 2: the prototype's max-tier Chief maps with the two fixes the prototype review asked for.

  python -I art/hero_tex_r2.py <albedo 4K> <multi 2K (Xbox order)> <CE albedo> <CE multi> <world mesh.npz> <out dir>
        [--visor-shader visor]

- visor: the base map under the visor's triangles (rasterised from the model's own UVs, feathered) is the CE
  map resampled smoothly (bicubic x8, a light blur against ringing), never B2's armour grain octaves (the
  prototype's gold mottling);
- self-illumination: the multipurpose map's self-illumination channel (green in the Xbox order) is CE's
  channel resampled plainly (Lanczos), not guided by the redrawn albedo, so the lamps keep their CE size and
  shape (the guided upscale shrank the chest light to an "eye").
Writes albedo.png, multi.png (Xbox order, RGBA), visor_mask.png, tex_r2.json. Local only: CE pixels.
"""
import argparse
import json
import os

import cv2
import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("albedo")
    ap.add_argument("multi")
    ap.add_argument("ce_albedo")
    ap.add_argument("ce_multi")
    ap.add_argument("mesh")
    ap.add_argument("out")
    ap.add_argument("--visor-shader", default="visor")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    alb = np.asarray(Image.open(a.albedo).convert("RGB"), np.float32) / 255
    N = alb.shape[0]
    ce = np.asarray(Image.open(a.ce_albedo).convert("RGB"), np.float32) / 255
    m = np.load(a.mesh)
    names = [str(x) for x in m["names"]]
    vi = names.index(a.visor_shader)
    mask = np.zeros((N, N), np.uint8)
    for t in np.nonzero(m["mat"] == vi)[0]:
        uv = m["uv"][t]
        q = np.round(np.stack([uv[:, 0] * N, (1 - uv[:, 1]) * N], 1) * 16).astype(np.int32)
        cv2.fillConvexPoly(mask, q, 1, lineType=cv2.LINE_8, shift=4)
    # the other shaders' texels win where islands touch: the visor mask stays inside the visor
    other = np.zeros((N, N), np.uint8)
    for t in np.nonzero(m["mat"] != vi)[0]:
        uv = m["uv"][t]
        q = np.round(np.stack([uv[:, 0] * N, (1 - uv[:, 1]) * N], 1) * 16).astype(np.int32)
        cv2.fillConvexPoly(other, q, 1, lineType=cv2.LINE_8, shift=4)
    vm = cv2.dilate(mask, np.ones((9, 9), np.uint8)) & ~(other & ~mask)
    w = cv2.GaussianBlur(vm.astype(np.float32), (0, 0), 3.0)
    w = np.where(mask > 0, np.maximum(w, 0.0), w)
    smooth = cv2.resize(ce, (N, N), interpolation=cv2.INTER_CUBIC)
    smooth = cv2.GaussianBlur(smooth, (0, 0), 1.5)
    out = alb * (1 - w[..., None]) + smooth * w[..., None]
    Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(a.out, "albedo.png"))
    Image.fromarray((w * 255).astype(np.uint8)).save(os.path.join(a.out, "visor_mask.png"))
    mp = np.asarray(Image.open(a.multi).convert("RGBA"), np.float32) / 255
    M = mp.shape[0]
    cem = np.asarray(Image.open(a.ce_multi).convert("RGBA"), np.float32) / 255
    g = cv2.resize(cem[..., 1], (M, M), interpolation=cv2.INTER_LANCZOS4)
    mp[..., 1] = np.clip(g, 0, 1)
    mp[..., 3] = cv2.resize(cem[..., 3], (M, M), interpolation=cv2.INTER_LANCZOS4)
    Image.fromarray((np.clip(mp, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(a.out, "multi.png"))
    # checks: the lamps' area at CE's size (texels over half, scaled), the visor's grain (high band RMS)
    lam_ce = float((cem[..., 1] > 0.5).sum()) * (M / cem.shape[0]) ** 2
    lam_new = float((mp[..., 1] > 0.5).sum())
    lam_old = float((np.asarray(Image.open(a.multi).convert("RGBA"), np.float32)[..., 1] / 255 > 0.5).sum())
    hb = lambda x: x - cv2.GaussianBlur(x, (0, 0), 4.0)
    L0 = alb.mean(2)
    L1 = out.mean(2)
    sel = mask > 0
    info = {"visor_texels": int(sel.sum()), "visor_highband_rms_before": float(np.sqrt((hb(L0)[sel] ** 2).mean())),
            "visor_highband_rms_after": float(np.sqrt((hb(L1)[sel] ** 2).mean())),
            "selfillum_area_vs_ce": {"ce_scaled": lam_ce, "prototype_guided": lam_old, "r2_plain": lam_new}}
    json.dump(info, open(os.path.join(a.out, "tex_r2.json"), "w"), indent=1)
    print("TEX_R2_OK", json.dumps(info))


if __name__ == "__main__":
    main()
