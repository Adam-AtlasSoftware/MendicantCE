"""PRISM v2 texture visibility, part 2: how much a tier map differs from CE, per frequency band, and how much of
that difference can reach the screen at a given magnification.

  python -I texvis_bands.py <out.json> [key ...]

Reference: CE's map Lanczos-resampled to the tier map's size (what the GPU's bilinear magnification of CE looks
like, without the block). Bands in cycles per CE texel (Nyquist 0.5): B0 < 1/8 (shapes over 8 texels), B1 1/8-1/4,
B2 1/4-1/2 (CE's finest), B3 1/2-1 (first octave above CE: visible when CE is magnified 2x or more),
B4 1-2 (second: 4x), B5 2-4 (Max only: 8x). Per band: rms of the tier's L* (CIE L*, 0-100), rms of the reference's,
and rms of their difference; for the screen: L* difference rms and CIEDE2000 mean of tier vs reference after both
are low-passed to the screen's Nyquist at magnification m (screen pixels per CE texel), the mip the GPU would use.
Local only (CE pixels): numbers and file paths only leave this machine.
"""
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texlib as T  # noqa: E402
import tier_cache as TC  # noqa: E402
import tier_recipe as R  # noqa: E402

OUT = os.path.join(T.WORK, "out/tiers")
BANDS = [(0, 1 / 8), (1 / 8, 1 / 4), (1 / 4, 1 / 2), (1 / 2, 1), (1, 2), (2, 4)]
KEYS = {"chief": "characters__cyborg__bitmaps__cyborg",
        "fp_ar": "weapons__assault_rifle__fp__bitmaps__assault_rifle_fp",
        "fp_hands": "characters__cyborg__fp__bitmaps__cyborg_hands",
        "elite_torso": "characters__elite__bitmaps__elite_torso",
        "elite_arms": "characters__elite__bitmaps__elite_arms",
        "elite_legs": "characters__elite__bitmaps__elite_legs",
        "elite_head": "characters__elite__bitmaps__elite_head",
        "marine_torso": "characters__marine_armored__bitmaps__marine_torso",
        "marine_arms": "characters__marine_armored__bitmaps__marine_arms",
        "marine_legs": "characters__marine_armored__bitmaps__marine_legs",
        "elite_legs1": "characters__elite__bitmaps__elite_legs1",
        "elite_head1": "characters__elite__bitmaps__elite_head1"}
TAG = {v: k for k, v in ((t, R.safe(t)) for t in R.BASES)}


def ce_path(key):
    return dict(TC.images(TAG[key]))[key]


def Lstar(rgb):
    return T.srgb_to_lab(np.clip(rgb, 0, 1))[..., 0]


def bandpass(L, F, lo, hi):
    """L (H, W) at F x CE; keep frequencies lo <= f < hi in cycles per CE texel (radial, hard edges)"""
    H, W = L.shape
    fy = np.fft.fftfreq(H)[:, None] * F
    fx = np.fft.fftfreq(W)[None, :] * F
    r = np.sqrt(fx ** 2 + fy ** 2)
    S = np.fft.fft2(L - L.mean())
    m = (r >= lo) & (r < hi)
    return np.real(np.fft.ifft2(S * m))


def lowpass_rgb(rgb, F, fc):
    H, W = rgb.shape[:2]
    fy = np.fft.fftfreq(H)[:, None] * F
    fx = np.fft.fftfreq(W)[None, :] * F
    r = np.sqrt(fx ** 2 + fy ** 2)
    m = np.exp(-0.5 * (r / (fc * 0.85)) ** 2)  # a mip's box-ish fall-off, -3 dB near fc
    out = np.empty_like(rgb)
    for c in range(3):
        out[..., c] = np.real(np.fft.ifft2(np.fft.fft2(rgb[..., c]) * m))
    return np.clip(out, 0, 1)


def screen_sim(img, F, m):
    """img at F x CE seen at m screen px per CE texel: what the sampled mip carries (resolution min(F, m) x CE),
    returned at CE x min(F, m)-ish for comparison (low-passed at the screen's Nyquist 0.5 * min(F, m) per CE texel)"""
    return lowpass_rgb(img, F, 0.5 * min(F, m))


def report(img, ce, mags=(0.54, 1.45, 2.9, 3.9)):
    F = img.shape[0] // ce.shape[0]
    ref = T.lanczos(ce, img.shape[0], img.shape[1])
    Li, Lr = Lstar(img), Lstar(ref)
    out = {"F": F, "bands": []}
    for lo, hi in BANDS:
        if lo >= 0.5 * F:
            break
        bi, br = bandpass(Li, F, lo, hi), bandpass(Lr, F, lo, hi)
        out["bands"].append({"band": [lo, hi], "tier_rms": float(bi.std()), "ce_rms": float(br.std()),
                             "diff_rms": float((bi - br).std())})
    tot_d = sum(b["diff_rms"] ** 2 for b in out["bands"])
    for b in out["bands"]:
        b["share_of_diff_energy"] = b["diff_rms"] ** 2 / max(tot_d, 1e-9)
    out["screen"] = {}
    for m in mags:
        si, sr = screen_sim(img, F, m), screen_sim(ref, F, m)
        # compare at a manageable size (box to 2x the screen's density or the map's)
        f2 = max(1, F // max(1, int(round(min(F, 2 * m)))))
        if f2 > 1:
            si, sr = T.boxdown(si, f2), T.boxdown(sr, f2)
        de = T.ciede2000(T.srgb_to_lab(si), T.srgb_to_lab(sr))
        dl = Lstar(si) - Lstar(sr)
        out["screen"]["m%.2f" % m] = {"dE00_mean": float(de.mean()), "dE00_p95": float(np.percentile(de, 95)),
                                      "dL_rms": float(dl.std()), "share_px_dE_over_1": float((de > 1.0).mean()),
                                      "share_px_dE_over_2": float((de > 2.0).mean())}
    return out


def main():
    out = sys.argv[1]
    keys = sys.argv[2:] or list(KEYS)
    res = {}
    for k in keys:
        key = KEYS[k]
        ce, _ = T.load(ce_path(key))
        res[k] = {"ce_size": list(ce.shape[:2])}
        for tier in ("enh", "max"):
            img, _ = T.load(os.path.join(OUT, key, tier + ".png"))
            res[k][tier] = report(img, ce)
            b = res[k][tier]["bands"]
            print(k, tier, "F", res[k][tier]["F"], " ".join("B%d ce %.2f d %.2f" % (i, x["ce_rms"], x["diff_rms"]) for i, x in enumerate(b)),
                  " | ", " ".join("%s dE %.2f >1:%.1f%%" % (mm, v["dE00_mean"], 100 * v["share_px_dE_over_1"]) for mm, v in res[k][tier]["screen"].items()), flush=True)
        json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
