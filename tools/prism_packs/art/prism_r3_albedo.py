#!/usr/bin/env python3
"""PRISM r3 Chief albedo: the Max tier base map with CE's painted shading reduced, so the PBR path's lights read.

    python -I art/prism_r3_albedo.py <base.png> <height2 dir> <out.png> [--sigma 40] [--strength 0.65]
        [--preview out_preview.png]

CE painted light into the base map: plates darken toward their rims and brighten on their tops, the undersuit
carries broad soft gradients. Under real lighting (and with the geometry bake's occlusion in the ORM image)
that shading is counted twice. Per material class (plates: the remodel's plate layer, sdf_raise > 0; the
undersuit: the rest of the islands), the broad luminance (a masked Gaussian, --sigma texels at 4096) is pulled
toward the class's regional level (a masked Gaussian ten times wider) by --strength (0 keeps CE's, 1 removes
the broad shading). Detail finer than sigma (scratches, painted seams, decals, lamps) is kept; the gain is
clamped to [0.7, 1.5] and left at 1 outside the islands. Colour ratios are kept (luminance gain on RGB).
Local only: made from the player's own maps.
"""
import argparse
import json
import os

import cv2
import numpy as np
from PIL import Image


def masked_blur(x, m, sigma):
    k = int(sigma * 3) | 1
    num = cv2.GaussianBlur(x * m, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)
    den = cv2.GaussianBlur(m, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)
    _ = k
    return num / np.maximum(den, 1e-4), den


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("height2")
    ap.add_argument("out")
    ap.add_argument("--sigma", type=float, default=40.0)
    ap.add_argument("--strength", type=float, default=0.65)
    ap.add_argument("--preview")
    a = ap.parse_args()
    img = Image.open(a.base).convert("RGBA")
    rgba = np.asarray(img, dtype=np.float32) / 255.0
    n = rgba.shape[0]
    inside = np.load(os.path.join(a.height2, "inside.npy"))
    plate = np.load(os.path.join(a.height2, "sdf_raise.npy")) > 0
    if inside.shape[0] != n:
        inside = cv2.resize(inside.astype(np.uint8), (n, n), interpolation=cv2.INTER_NEAREST) > 0
        plate = cv2.resize(plate.astype(np.uint8), (n, n), interpolation=cv2.INTER_NEAREST) > 0
    rgb = rgba[..., :3]
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    s = a.sigma * n / 4096.0
    gain = np.ones_like(lum)
    stats = {}
    for name, m in (("plate", inside & plate), ("suit", inside & ~plate)):
        mf = m.astype(np.float32)
        broad, _ = masked_blur(lum, mf, s)
        level, _ = masked_blur(lum, mf, s * 10)
        g = (np.maximum(level, 1e-3) / np.maximum(broad, 1e-3)) ** a.strength
        g = np.clip(g, 0.7, 1.5)
        gain = np.where(m, g, gain)
        stats[name] = {"texels": int(m.sum()), "gain_p5_p95": [float(np.percentile(g[m], 5)), float(np.percentile(g[m], 95))],
                       "lum_std_before": float(lum[m].std())}
    # (the gain itself smoothed across class borders: no seam at a plate's rim)
    gain = cv2.GaussianBlur(gain, (0, 0), max(1.0, s / 20))
    out = np.clip(rgb * gain[..., None], 0, 1)
    lum2 = out @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    for name, m in (("plate", inside & plate), ("suit", inside & ~plate)):
        stats[name]["lum_std_after"] = float(lum2[m].std())
    res = np.concatenate([out, rgba[..., 3:]], axis=2)
    Image.fromarray((res * 255 + 0.5).astype(np.uint8), "RGBA").save(a.out)
    if a.preview:
        small = lambda x: Image.fromarray((x * 255 + 0.5).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS)  # noqa: E731
        p = Image.new("RGB", (2048, 1024))
        p.paste(small(rgb), (0, 0))
        p.paste(small(out), (1024, 0))
        p.save(a.preview)
    print("ALBEDO_OK", json.dumps(stats))


if __name__ == "__main__":
    main()
