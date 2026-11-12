"""2x or 4x upscale of a CE base (diffuse) map with the permissive Real-ESRGAN x4plus model.

  python -I art/upscale_base.py --in <base.png> --out <base_up.png> [--scale 2|4|auto]

Colour (RGB) only: x4plus at 4x on an edge-padded copy; at --scale 2 a Lanczos downsample to 2x
follows (the model is 4x-only; 4x then 2x down also averages away most of its invented texture).
--scale auto is 4 for a map whose larger side is 256 or less, else 2. --wrap pads with the
opposite edge instead of the edge's own pixels (a tiling map). Alpha is resized on its own with
Lanczos. Never use this on normal, bump, detail, cube or multipurpose maps.
Back-projection (--backproject N, default 6): the model alone smooths CE's painted mottling and
shifts hues (x4plus at 4x on elite arms: hue 5.9 deg, skin mottling gone), so the result is then
corrected N times by the error between CE and its own box-filtered copy, spread back up with
Lanczos (after --model-weight, if below 1, has blended the model's result back toward a Lanczos
upscale: for maps where the model draws letter-like shapes). CE's pixels are then what the upscale averages to (its colours and texture kept), and the
model adds only what lies above CE's resolution: edge sharpness. --backproject 0 is the model alone.
Fidelity check: the 2x result box-filtered back to CE size
vs the CE pixels, CIEDE2000 mean/p95/p99 and mean hue shift. Writes <out>.json next to it.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
ESR = E.upscaler("realesrgan") if E.UPSCALERS else ""


def srgb_to_lab(rgb):
    c = rgb.astype(np.float64) / 255.0
    c = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = c @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz), xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def ciede2000(l1, l2):
    L1, a1, b1 = l1[..., 0], l1[..., 1], l1[..., 2]
    L2, a2, b2 = l2[..., 0], l2[..., 1], l2[..., 2]
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cb = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cb ** 7 / (Cb ** 7 + 25.0 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p
    dh = np.where(C1p * C2p == 0, 0, np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh)))
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lbp, Cbp = (L1 + L2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hbp = np.where(C1p * C2p == 0, hs, np.where(np.abs(h1p - h2p) <= 180, hs / 2,
                                                np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2)))
    T = (1 - 0.17 * np.cos(np.radians(hbp - 30)) + 0.24 * np.cos(np.radians(2 * hbp))
         + 0.32 * np.cos(np.radians(3 * hbp + 6)) - 0.20 * np.cos(np.radians(4 * hbp - 63)))
    dth = 30 * np.exp(-(((hbp - 275) / 25) ** 2))
    Rc = 2 * np.sqrt(Cbp ** 7 / (Cbp ** 7 + 25.0 ** 7))
    Sl = 1 + 0.015 * (Lbp - 50) ** 2 / np.sqrt(20 + (Lbp - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cbp, 1 + 0.015 * Cbp * T
    Rt = -np.sin(np.radians(2 * dth)) * Rc
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh)), dh, np.minimum(C1p, C2p)


def backproject(rgb, up, k, n):
    """n rounds of: out += Lanczos-up(CE - box-down(out)), per channel in float, clipped to 0-255"""
    if n <= 0:
        return up
    h, w = rgb.shape[:2]
    src = rgb.astype(np.float32)
    out = np.asarray(up).astype(np.float32)
    for _ in range(n):
        down = np.stack([np.asarray(Image.fromarray(out[..., c]).resize((w, h), Image.BOX)) for c in range(3)], -1)
        err = src - down
        out += np.stack([np.asarray(Image.fromarray(err[..., c]).resize((k * w, k * h), Image.LANCZOS))
                         for c in range(3)], -1)
        out = np.clip(out, 0, 255)
    return Image.fromarray(np.round(out).astype(np.uint8))


def added_detail(rgb, up, k):
    h, w = rgb.shape[:2]
    lz = np.asarray(Image.fromarray(rgb).resize((k * w, k * h), Image.LANCZOS)).astype(np.float64)
    y = lambda c: c[..., 0] * 0.299 + c[..., 1] * 0.587 + c[..., 2] * 0.114
    d = np.abs(y(up.astype(np.float64)) - y(lz))
    return {"added_luma_mean": float(d.mean()), "added_luma_p99": float(np.percentile(d, 99))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--gpu", default=E.VULKAN_GPU or "auto")
    ap.add_argument("--pad", type=int, default=16)
    ap.add_argument("--scale", default="auto", choices=["2", "4", "auto"])
    ap.add_argument("--wrap", action="store_true")
    ap.add_argument("--backproject", type=int, default=6)
    ap.add_argument("--model-weight", type=float, default=1.0,
                    help="how much of the model's own detail over a Lanczos upscale is kept (0: Lanczos alone)")
    ap.add_argument("--raw-out", help="also keep the model's own result (before back-projection) here")
    a = ap.parse_args()
    src = Image.open(a.inp).convert("RGBA")
    w, h = src.size
    k = (4 if max(w, h) <= 256 else 2) if a.scale == "auto" else int(a.scale)
    rgb = np.asarray(src)[..., :3]
    alpha = src.getchannel("A")
    work = E.scratch("upscale")
    pin = os.path.join(work, "esr_in_%d.png" % os.getpid())
    pout = os.path.join(work, "esr_out_%d.png" % os.getpid())
    Image.fromarray(np.pad(rgb, ((a.pad, a.pad), (a.pad, a.pad), (0, 0)), mode="wrap" if a.wrap else "edge")).save(pin)
    cmd = [os.path.join(ESR, "realesrgan-ncnn-vulkan" + E.EXE), "-i", pin, "-o", pout, "-n", a.model, "-s", "4",
           "-m", os.path.join(ESR, "models"), "-t", "256"]
    if a.gpu != "auto":
        cmd += ["-g", a.gpu]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(pout):
        sys.exit("upscaler failed: " + r.stderr[-2000:])
    up4 = Image.open(pout).convert("RGB")
    p4 = a.pad * 4
    up4 = up4.crop((p4, p4, p4 + 4 * w, p4 + 4 * h))
    up2 = up4 if k == 4 else up4.resize((2 * w, 2 * h), Image.LANCZOS)
    if a.raw_out:
        up2.save(a.raw_out)
    if a.model_weight != 1.0:
        lz = np.asarray(Image.fromarray(rgb).resize((k * w, k * h), Image.LANCZOS)).astype(np.float32)
        up2 = Image.fromarray(np.clip(lz + a.model_weight * (np.asarray(up2).astype(np.float32) - lz), 0, 255)
                              .round().astype(np.uint8))
    up2 = backproject(rgb, up2, k, a.backproject)
    a2 = alpha.resize((k * w, k * h), Image.LANCZOS)
    out = up2.copy()
    out.putalpha(a2)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    out.save(a.out)
    # fidelity: the upscale box-filtered back to CE size vs CE
    back = np.asarray(up2.resize((w, h), Image.BOX))
    de, dh, cmin = ciede2000(srgb_to_lab(rgb), srgb_to_lab(back))
    chroma = cmin > 10  # hue only meaningful on coloured pixels
    rep = {"input": os.path.relpath(a.inp, ROOT), "output": os.path.relpath(a.out, ROOT), "model": a.model,
           "backproject": a.backproject, "model_weight": a.model_weight,
           "licence": "Real-ESRGAN code and weights BSD-3-Clause / ncnn-vulkan MIT (<upscalers>/realesrgan/LICENSE-*)",
           "scale": k, "size_in": [w, h], "size_out": [k * w, k * h],
           "de2000_mean": float(de.mean()), "de2000_p95": float(np.percentile(de, 95)),
           "de2000_p99": float(np.percentile(de, 99)),
           "hue_shift_mean_deg_chromatic": float(np.abs(dh[chroma]).mean()) if chroma.any() else 0.0,
           "chromatic_px": int(chroma.sum()),
           # added detail: the upscale against a plain Lanczos upscale of CE (luma, 0-255); what the model
           # drew that interpolation did not (sharpened edges, and anything it invented)
           **added_detail(rgb, np.asarray(up2), k),
           "limits": {"de2000_mean": 3, "de2000_p95": 6, "de2000_p99": 8, "hue_deg": 4}}
    rep["pass"] = bool(rep["de2000_mean"] <= 3 and rep["de2000_p95"] <= 6 and rep["de2000_p99"] <= 8
                       and rep["hue_shift_mean_deg_chromatic"] <= 4)
    with open(os.path.splitext(a.out)[0] + ".json", "w") as f:
        json.dump(rep, f, indent=1)
    for p in (pin, pout):
        os.remove(p)
    print("UPSCALE", json.dumps(rep))


if __name__ == "__main__":
    main()
