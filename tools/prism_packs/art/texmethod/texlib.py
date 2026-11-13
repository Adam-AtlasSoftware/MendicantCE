"""Shared helpers for the PRISM v2 texture-method study: resampling, ML upscalers, fidelity metrics.

Run with a Python that has numpy, scipy, scikit-image, opencv-python-headless, torch and spandrel
(prism_packs.py's requirements), the folders of ../packs_env.py set.
All images are float32 sRGB in [0,1], HxWxC.
"""
import os
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

ESR = E.upscaler("realesrgan") if E.UPSCALERS else ""
WEIGHTS = E.upscaler("weights") if E.UPSCALERS else ""
WORK = E.work("textures") if E.WORK else ""


def load(path):
    im = Image.open(path)
    a = np.asarray(im.convert("RGBA")).astype(np.float32) / 255.0
    return a[..., :3], a[..., 3]


def save(path, rgb):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    a = np.clip(rgb, 0, 1)
    if a.ndim == 2:
        Image.fromarray((a * 255 + 0.5).astype(np.uint8), "L").save(path)
    else:
        Image.fromarray((a * 255 + 0.5).astype(np.uint8)).save(path)


# ---------------------------------------------------------------- resampling
def nearest(rgb, s):
    return np.repeat(np.repeat(rgb, s, 0), s, 1)


def lanczos(rgb, h, w):
    return np.clip(cv2.resize(rgb, (w, h), interpolation=cv2.INTER_LANCZOS4), 0, 1)


def boxdown(rgb, f):
    h, w = rgb.shape[:2]
    return rgb.reshape(h // f, f, w // f, f, *rgb.shape[2:]).mean((1, 3))


def pad_wrap(rgb, p):
    return np.pad(rgb, ((p, p), (p, p), (0, 0)) if rgb.ndim == 3 else ((p, p), (p, p)), mode="wrap")


def backproject(up, ce, n=6, wrap=True):
    """Make box-down(up) == ce: CE's pixels are what the result averages to; only detail above
    CE's Nyquist is the method's own."""
    f = up.shape[0] // ce.shape[0]
    h, w = up.shape[:2]
    for _ in range(n):
        err = ce - boxdown(up, f)
        if wrap:
            e = pad_wrap(err, 4)
            eu = cv2.resize(e, (e.shape[1] * f, e.shape[0] * f), interpolation=cv2.INTER_LANCZOS4)
            eu = eu[4 * f:4 * f + h, 4 * f:4 * f + w]
        else:
            eu = cv2.resize(err, (w, h), interpolation=cv2.INTER_LANCZOS4)
        up = up + eu
    return np.clip(up, 0, 1)


def gblur(a, s, wrap):
    if s <= 0:
        return a
    if not wrap:
        return cv2.GaussianBlur(a, (0, 0), s, borderType=cv2.BORDER_REFLECT)
    p = int(np.ceil(3 * s)) + 1
    return cv2.GaussianBlur(pad_wrap(a, p), (0, 0), s)[p:-p, p:-p]


def backproject_low(up, ce, n=6, sigma=0.8, wrap=True):
    """Back-projection on CE's lower band only: G(box-down(up)) == G(CE), sigma in CE texels. CE's top
    octave (where DXT block noise and 1-texel dither live) is not forced into the result, so the 4x4 block
    grid does not come back; CE's colours and shapes (everything above ~2 texels) are kept exactly."""
    f = up.shape[0] // ce.shape[0]
    h, w = up.shape[:2]
    tgt = gblur(ce, sigma, wrap)
    for _ in range(n):
        err = tgt - gblur(boxdown(up, f), sigma, wrap)
        e = pad_wrap(err, 4) if wrap else np.pad(err, ((4, 4), (4, 4), (0, 0)), mode="edge")
        eu = cv2.resize(e, (e.shape[1] * f, e.shape[0] * f), interpolation=cv2.INTER_CUBIC)
        up = up + eu[4 * f:4 * f + h, 4 * f:4 * f + w]
    return np.clip(up, 0, 1)


# ---------------------------------------------------------------- ML upscalers
# (the models the pack tools fetch on the player's request: prism_packs.py models; 4xNomos8kSC runs from its
# own PyTorch file, as the study converted it for ncnn)
NCNN = {"x4plus": "realesrgan-x4plus", "x4plus_anime": "realesrgan-x4plus-anime",
        "animev3": "realesr-animevideov3"}
TORCH = {"general_v3": "realesr-general-x4v3.pth", "general_wdn_v3": "realesr-general-wdn-x4v3.pth",
         "nomos8ksc": "4xNomos8kSC.pth",
         "swinir_l_gan": "003_realSR_BSRGAN_DFOWMFC_s64w8_SwinIR-L_x4_GAN.pth",
         "swinir_m_psnr": "001_classicalSR_DF2K_s64w8_SwinIR-M_x4.pth",
         "hat_s_nomos": "4xNomos8kSCHAT-S.pth", "dat2_nomos2hq": "4xNomos2_hq_dat2.pth",
         "dat2_realwebphoto": "4xRealWebPhoto_v4_dat2.safetensors"}
LICENCE = {"x4plus": "BSD-3", "x4plus_anime": "BSD-3", "animev3": "BSD-3", "nomos8ksc": "CC-BY-4.0",
           "general_v3": "BSD-3", "general_wdn_v3": "BSD-3", "swinir_l_gan": "Apache-2.0",
           "swinir_m_psnr": "Apache-2.0", "hat_s_nomos": "CC-BY-4.0", "dat2_nomos2hq": "CC-BY-4.0",
           "dat2_realwebphoto": "CC-BY-4.0"}
_models = {}


def _torch_model(name):
    import torch
    from spandrel import ModelLoader
    if name not in _models:
        m = ModelLoader().load_from_file(os.path.join(WEIGHTS, TORCH[name]))
        dev = os.environ.get("TEXM_DEV", E.TORCH_DEVICE)
        m = m.to(dev).eval()
        _models[name] = (m, dev)
    return _models[name]


def run_torch(name, rgb, tile=192, ov=16):
    import torch
    m, dev = _torch_model(name)
    h, w = rgb.shape[:2]
    s = m.scale
    out = np.zeros((h * s, w * s, 3), np.float32)
    wt = np.zeros((h * s, w * s, 1), np.float32)
    ys = list(range(0, max(h - tile, 0) + 1, tile - 2 * ov)) or [0]
    xs = list(range(0, max(w - tile, 0) + 1, tile - 2 * ov)) or [0]
    if ys[-1] + tile < h:
        ys.append(h - tile)
    if xs[-1] + tile < w:
        xs.append(w - tile)
    th, tw = min(tile, h), min(tile, w)
    ramp = np.ones((th * s, tw * s, 1), np.float32)
    r = ov * s
    lin = np.linspace(0.05, 1, r, dtype=np.float32)
    ramp[:r] *= lin[:, None, None]; ramp[-r:] *= lin[::-1, None, None]
    ramp[:, :r] *= lin[None, :, None]; ramp[:, -r:] *= lin[None, ::-1, None]
    with torch.no_grad():
        for y in ys:
            for x in xs:
                t = torch.from_numpy(rgb[y:y + th, x:x + tw].transpose(2, 0, 1)[None].copy()).to(dev)
                o = m(t).clamp(0, 1)[0].float().cpu().numpy().transpose(1, 2, 0)
                out[y * s:(y + th) * s, x * s:(x + tw) * s] += o * ramp
                wt[y * s:(y + th) * s, x * s:(x + tw) * s] += ramp
    return out / np.maximum(wt, 1e-6)


def run_ncnn(name, rgb, gpu=None):
    import tempfile
    gpu = E.VULKAN_GPU if gpu is None else gpu
    d = E.scratch("ncnn")
    fi = tempfile.NamedTemporaryFile(dir=d, suffix=".png", delete=False).name
    fo = fi[:-4] + "_o.png"
    save(fi, rgb)
    cmd = [os.path.join(ESR, "realesrgan-ncnn-vulkan" + E.EXE), "-i", fi, "-o", fo, "-n", NCNN[name], "-s", "4",
           "-m", os.path.join(ESR, "models")] + (["-g", str(gpu)] if str(gpu) != "" else [])
    subprocess.run(cmd, check=True, capture_output=True)
    o = np.asarray(Image.open(fo).convert("RGB")).astype(np.float32) / 255.0
    os.remove(fi); os.remove(fo)
    return o


def upscale4(name, rgb, wrap=True, pad=16):
    """One 4x pass, wrap-padded so a tiling map stays tiling (edge-padded otherwise)."""
    p = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if wrap else "edge") if pad else rgb
    o = run_ncnn(name, p) if name in NCNN else run_torch(name, p)
    return o[pad * 4:-pad * 4, pad * 4:-pad * 4] if pad else o


def upscale_to(name, rgb, factor, wrap=True):
    """factor 4, 8 (4x model then 4x model, Lanczos down by 2) or 16 (4x model twice)."""
    o = upscale4(name, rgb, wrap)
    if factor == 4:
        return o
    o2 = upscale4(name, o, wrap)
    if factor == 16:
        return o2
    h, w = rgb.shape[0] * factor, rgb.shape[1] * factor
    return cv2.resize(o2, (w, h), interpolation=cv2.INTER_AREA)


# ---------------------------------------------------------------- metrics
def srgb_to_lab(rgb):
    c = np.clip(rgb, 0, 1).astype(np.float64)
    c = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = c @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz), xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def ciede2000(l1, l2):
    from skimage.color import deltaE_ciede2000
    return deltaE_ciede2000(l1, l2)


def ssim_l(a, b):
    from skimage.metrics import structural_similarity
    la, lb = srgb_to_lab(a)[..., 0], srgb_to_lab(b)[..., 0]
    return float(structural_similarity(la, lb, data_range=100.0))


def hue_shift(a, b):
    la, lb = srgb_to_lab(a), srgb_to_lab(b)
    ca, cb = np.hypot(la[..., 1], la[..., 2]), np.hypot(lb[..., 1], lb[..., 2])
    m = (ca > 8) & (cb > 8)
    if m.sum() < 16:
        return 0.0
    d = np.degrees(np.arctan2(lb[..., 2], lb[..., 1]) - np.arctan2(la[..., 2], la[..., 1]))
    d = (d + 180) % 360 - 180
    return float(np.abs(d[m]).mean())


def lum(rgb):
    return rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def detail_above_nyquist(up, f):
    """RMS (in 8-bit levels) of what lies above CE's Nyquist: up - Lanczos(boxdown(up))."""
    l = lum(up)
    d = boxdown(l, f)
    back = cv2.resize(d, (l.shape[1], l.shape[0]), interpolation=cv2.INTER_LANCZOS4)
    return float(np.sqrt(((l - back) ** 2).mean()) * 255)


def spectral_slope(l):
    """Radially averaged log power spectrum: slope below and above half-band (natural textures ~ -2..-3).
    Returns (slope over the lower octaves, slope over the upper octaves)."""
    n = min(l.shape)
    l = l[:n, :n] - l[:n, :n].mean()
    win = np.outer(np.hanning(n), np.hanning(n))
    P = np.abs(np.fft.fftshift(np.fft.fft2(l * win))) ** 2
    y, x = np.indices(P.shape) - n // 2
    r = np.hypot(x, y).astype(int)
    prof = np.bincount(r.ravel(), P.ravel()) / np.maximum(np.bincount(r.ravel()), 1)
    k = np.arange(len(prof))
    sel_lo = (k >= 4) & (k < n // 8)
    sel_hi = (k >= n // 8) & (k < n // 2)
    s_lo = np.polyfit(np.log(k[sel_lo]), np.log(prof[sel_lo] + 1e-12), 1)[0]
    s_hi = np.polyfit(np.log(k[sel_hi]), np.log(prof[sel_hi] + 1e-12), 1)[0]
    return float(s_lo), float(s_hi)


def invented_edges(up, ce, f, mid=4):
    """Invented-structure check at mid x CE resolution: share of the result's strong edges (top 10 percent
    of gradient magnitude) further than 1 CE texel from any edge of the CE map Lanczos-upsampled to the
    same grid. ML 'letter-like' shapes and new panel lines show up here; fine grain below 1 CE texel does
    not count (it is meant to be new)."""
    lm = lum(cv2.resize(up, (ce.shape[1] * mid, ce.shape[0] * mid), interpolation=cv2.INTER_AREA))
    lc = lum(lanczos(ce, ce.shape[0] * mid, ce.shape[1] * mid))

    def edges(l):
        g = cv2.GaussianBlur(l, (0, 0), 1.0)
        gx, gy = cv2.Sobel(g, cv2.CV_32F, 1, 0), cv2.Sobel(g, cv2.CV_32F, 0, 1)
        return np.hypot(gx, gy)

    em, ec = edges(lm), edges(lc)
    sm = em > np.quantile(em, 0.9)
    sc = ec > np.quantile(ec, 0.8)
    dist = cv2.distanceTransform((~sc).astype(np.uint8), cv2.DIST_L2, 3)
    return float((dist[sm] > mid).mean())


def fidelity(up, ce, alpha_mask=None):
    """The checks of (d): box-down of the result vs CE at CE's resolution, plus the lift measures."""
    f = up.shape[0] // ce.shape[0]
    d = boxdown(up, f)
    de = ciede2000(srgb_to_lab(ce), srgb_to_lab(d))
    del_ = ciede2000(srgb_to_lab(gblur(ce, 0.8, False)), srgb_to_lab(gblur(d, 0.8, False)))
    m = np.ones(ce.shape[:2], bool) if alpha_mask is None else alpha_mask
    lo, hi = spectral_slope(lum(up))
    return {"de_mean": float(de[m].mean()), "de_p95": float(np.percentile(de[m], 95)),
            "de_p99": float(np.percentile(de[m], 99)),
            "deL_mean": float(del_[m].mean()), "deL_p95": float(np.percentile(del_[m], 95)), "hue_deg": hue_shift(ce, d), "ssim_L": ssim_l(ce, d),
            "detail_rms": detail_above_nyquist(up, f), "invented_edges": invented_edges(up, ce, f),
            "slope_lo": lo, "slope_hi": hi}
