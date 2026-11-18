#!/usr/bin/env python3
"""PRISM v4's material pack (docs/prism_materials.md; overhaul_research/prism_v4_renderer/design.md, section 5.4):
the pack tier of the deferred renderer's materials, made on the player's machine from the player's own maps (the
extract: map_extract's bitmaps and shaders) and the PRISM texture packs already built (Bold's and base_local's
upscaled characters, Bold's surface maps, the material overlay's classes). For every environment shader of a map
(senv) and every model shader of its characters (soso, characters\\...), a material of PRISM's format 2:

  materials\\<shader tag>.pack.json, its images in textures\\prism_v4\\<shader tag>\\:
    albedo.ktx2  the base colour with CE's painted light taken out (BC7, RGBA: CE's alpha kept, its detail and
                 specular masks and its alpha test)
    normal.ktx2  the shading normal in tangent space (BC5: x along u, y along v, as CE's bump maps): CE's bump map
                 where the shader has one (at its coordinates, "normal_uv": "bump"), a model's surface map's
                 where a pack made one, else (an environment shader) one found from the albedo's relief
    orm.ktx2     occlusion, roughness, metal (BC7): the material type's prior, CE's specular mask and brightness,
                 the relief's cavities and edges; a model's by the overlay's classes and its surface map
    height.ktx2  the height of CE's bump map (BC4, Frankot-Chellappa), 0.5 the surface
  and an environment shader's detail maps as CE draws them (textures\\prism_v4\\details\\<bitmap tag>.ktx2),
  blended by CE's own type, functions and scales at run time ("ce"), with CE's own mip levels where the extract
  has them (--detail-mips: a detail map's levels fade toward grey, so that a far surface keeps its base colour;
  a chain filtered from level 0 would keep the map's mean, and a bright one would lighten every far surface).

The de-lighting: an environment base map's painted relief light, regressed on its own bump map's relief (cavity,
slope, facing) above a sixteenth of the map, divided out where the fit explains a tenth of the fine detail at
least (R^2 >= 0.1); a model's broad painted shading flattened inside its islands. The low band (CE's layout and
colours) is kept: the gate takes the low-band CIEDE2000 of the composite base colour (with the detail maps' mean
by CE's detail function) against CE's, at most 1.0 on average and 3 at the 95th percentile; a shader that fails it
is left to the automatic tier (no file), and the report says so. Roughness is kept in its material type's band.

  prism_v4_materials.py build --extract <work>/extract/<map> --shaders <work>/extract_v4/<map> \\
      --out <data>/mods/prism_v4_materials [--detail-mips <work>/extract_v4_mips/<map>] [--packs <data>/mods] \\
      [--workers 4] [--side 2048] [--again]
  prism_v4_materials.py check   the encoders and the gate's maths on made-up pictures

numpy and Pillow only. Everything it writes is made from the player's maps: it stays on the player's machine.
"""

import argparse
import json
import math
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent))
import texstore_synth  # noqa: E402  (tools/: the BC7 encoder)
from microtex_lib import ktx2 as microtex_ktx2  # noqa: E402  (the BC4 and BC5 encoders)
import mod_bitmap_check  # noqa: E402  (a .bitmap tag's walk)

PACK = "prism_v4_materials"
FORMAT = 2
VERSION = 1
LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)
# KTX2's vkFormats and their data format descriptors' models (KHR_DF_MODEL_BC4, BC5, BC7)
VK_BC4, VK_BC5, VK_BC7 = 139, 141, 145
DF_MODEL = {VK_BC4: 131, VK_BC5: 132, VK_BC7: 134}
KTX2_IDENTIFIER = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


# ---------------------------------------------------------------- pictures


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), np.uint8)


def s2l(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def l2s(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def blur(x, sigma):
    """a Gaussian blur of a 2-D array, wrapped (the maps tile), by FFT"""
    if sigma <= 0:
        return x
    h, w = x.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    g = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(x) * g))


def blur_rgb(x, sigma):
    return np.stack([blur(x[..., c], sigma) for c in range(x.shape[2])], -1)


def power_of_two(side):
    return 1 << max(0, int(math.ceil(math.log2(max(1, side)))))


def resized(rgba, width, height, resample=Image.LANCZOS):
    """an RGBA uint8 picture at a size"""
    if rgba.shape[1] == width and rgba.shape[0] == height:
        return rgba
    return np.asarray(Image.fromarray(rgba, "RGBA").resize((width, height), resample), np.uint8)


def resized_float(x, width, height):
    """a 2-D float array at a size (bilinear)"""
    if x.shape[1] == width and x.shape[0] == height:
        return x
    return np.asarray(Image.fromarray(x.astype(np.float32), "F").resize((width, height), Image.BILINEAR), np.float32)


def sample_wrap(picture, height, width, scale):
    """picture (h, w, c) sampled bilinearly, wrapped, at the texel centres of a height x width grid whose
    coordinates are scale (u, v) times the grid's (as CE samples a bump map at its scale over the base map's)"""
    h, w = picture.shape[:2]
    u = ((np.arange(width) + 0.5) / width * scale[0] * w - 0.5)[None, :].repeat(height, 0)
    v = ((np.arange(height) + 0.5) / height * scale[1] * h - 0.5)[:, None].repeat(width, 1)
    x0 = np.floor(u).astype(np.int64)
    y0 = np.floor(v).astype(np.int64)
    fx = (u - x0)[..., None]
    fy = (v - y0)[..., None]
    x0 %= w
    y0 %= h
    x1 = (x0 + 1) % w
    y1 = (y0 + 1) % h
    p = picture.astype(np.float32)
    top = p[y0, x0] * (1 - fx) + p[y0, x1] * fx
    bottom = p[y1, x0] * (1 - fx) + p[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def normals_of(rgb):
    """a normal map's texels (RGB 0 to 1, CE's bump maps' encoding) as unit vectors"""
    n = rgb.astype(np.float32) * 2.0 - 1.0
    return n / (np.linalg.norm(n, axis=-1, keepdims=True) + 1e-6)


def height_from_normal(n):
    """the height a field of normals is the slopes of (Frankot-Chellappa, wrapped), zero mean, unit deviation;
    x along the picture's columns, y down its rows (the extract's green: the sign whose height registers with the
    base map, materials.md section 3)"""
    nz = np.maximum(n[..., 2], 0.2)
    p = -n[..., 0] / nz
    q = -n[..., 1] / nz
    h, w = p.shape
    wx = 2 * np.pi * np.fft.fftfreq(w)[None, :]
    wy = 2 * np.pi * np.fft.fftfreq(h)[:, None]
    d = wx * wx + wy * wy
    d[0, 0] = 1
    spectrum = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / d
    spectrum[0, 0] = 0
    z = np.real(np.fft.ifft2(spectrum))
    return (z - z.mean()) / (z.std() + 1e-6)


def normal_from_height(height, strength):
    """a tangent-space normal from a height (x along u, y along v: down the rows), wrapped"""
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5
    n = np.stack([-strength * dx, -strength * dy, np.ones_like(height)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


# ---------------------------------------------------------------- CIEDE2000 (the gate)


def srgb_to_lab(rgb):
    """sRGB (0 to 1) to CIELAB (D65)"""
    lin = s2l(np.clip(rgb, 0, 1))
    m = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def ciede2000(lab1, lab2):
    """CIEDE2000 (Sharma, Wu and Dalal 2005), per texel"""
    l1, a1, b1 = lab1[..., 0], lab1[..., 1], lab1[..., 2]
    l2, a2, b2 = lab2[..., 0], lab2[..., 1], lab2[..., 2]
    c1 = np.hypot(a1, b1)
    c2 = np.hypot(a2, b2)
    cm = (c1 + c2) / 2
    g = 0.5 * (1 - np.sqrt(cm ** 7 / (cm ** 7 + 25.0 ** 7)))
    a1p, a2p = (1 + g) * a1, (1 + g) * a2
    c1p, c2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dl = l2 - l1
    dc = c2p - c1p
    dh = h2p - h1p
    dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh))
    dh = np.where(c1p * c2p == 0, 0, dh)
    dhh = 2 * np.sqrt(c1p * c2p) * np.sin(np.radians(dh) / 2)
    lm = (l1 + l2) / 2
    cmp_ = (c1p + c2p) / 2
    hs = h1p + h2p
    hm = np.where(c1p * c2p == 0, hs, np.where(np.abs(h1p - h2p) <= 180, hs / 2,
                                              np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2)))
    t = (1 - 0.17 * np.cos(np.radians(hm - 30)) + 0.24 * np.cos(np.radians(2 * hm)) +
         0.32 * np.cos(np.radians(3 * hm + 6)) - 0.20 * np.cos(np.radians(4 * hm - 63)))
    d_theta = 30 * np.exp(-(((hm - 275) / 25) ** 2))
    rc = 2 * np.sqrt(cmp_ ** 7 / (cmp_ ** 7 + 25.0 ** 7))
    sl = 1 + 0.015 * (lm - 50) ** 2 / np.sqrt(20 + (lm - 50) ** 2)
    sc = 1 + 0.045 * cmp_
    sh = 1 + 0.015 * cmp_ * t
    rt = -np.sin(np.radians(2 * d_theta)) * rc
    return np.sqrt((dl / sl) ** 2 + (dc / sc) ** 2 + (dhh / sh) ** 2 + rt * (dc / sc) * (dhh / sh))


def detail_apply(color, detail, function):
    """CE's detail function (its texture pass's combiners): double-biased multiply, multiply, double-biased add"""
    if function == 1:
        return np.clip(color * detail, -1, 1)
    if function == 2:
        return np.clip(color + 2 * detail - 1, -1, 1)
    return np.clip(2 * color * detail, -1, 1)


def low_band_delta(ce_rgb, pack_rgb, detail=None, weight=None):
    """the gate: CIEDE2000 between two base colours (sRGB 0 to 1) above a sixteenth of the map blurred away, each
    composited with the detail maps' means by CE's functions ((function, mean rgb) pairs); mean and 95th percentile
    over the texels weight keeps (an alpha-tested map's drawn ones; all without one)"""
    sigma = max(ce_rgb.shape[:2]) / 16.0
    a, b = ce_rgb, pack_rgb
    for function, mean in detail or []:
        a = detail_apply(a, np.asarray(mean, np.float64), function)
        b = detail_apply(b, np.asarray(mean, np.float64), function)
    if weight is not None and weight.any():
        # (the low band of the texels drawn alone: a masked blur)
        w = weight.astype(np.float64)
        norm = np.maximum(blur(w, sigma), 1e-6)[..., None]
        a = np.clip(blur_rgb(np.clip(a, 0, 1) * w[..., None], sigma) / norm, 0, 1)
        b = np.clip(blur_rgb(np.clip(b, 0, 1) * w[..., None], sigma) / norm, 0, 1)
        delta = ciede2000(srgb_to_lab(a), srgb_to_lab(b))[weight]
    else:
        a = np.clip(blur_rgb(np.clip(a, 0, 1), sigma), 0, 1)
        b = np.clip(blur_rgb(np.clip(b, 0, 1), sigma), 0, 1)
        delta = ciede2000(srgb_to_lab(a), srgb_to_lab(b))
    return float(delta.mean()), float(np.percentile(delta, 95))


# ---------------------------------------------------------------- KTX2


def ktx2_dfd(vk):
    """the basic data format descriptor of a block format (one sample, or two for BC5)"""
    samples = [(0, 63, 0)] if vk == VK_BC4 else [(0, 63, 0), (64, 63, 1)] if vk == VK_BC5 else [(0, 127, 0)]
    block_bytes = 8 if vk == VK_BC4 else 16
    body = struct.pack("<II", 0, ((24 + 16 * len(samples)) << 16) | 2)
    body += bytes([DF_MODEL[vk], 1, 1, 0]) + bytes([3, 3, 0, 0]) + bytes([block_bytes, 0, 0, 0, 0, 0, 0, 0])
    for offset, length, channel in samples:
        body += struct.pack("<HBB", offset, length, channel) + bytes(4) + struct.pack("<II", 0, 0xFFFFFFFF)
    return struct.pack("<I", 4 + len(body)) + body


def ktx2_write(path, vk, width, height, levels):
    """a KTX2 file of one face and a whole chain (levels: bytes, the largest first), no supercompression"""
    count = len(levels)
    dfd = ktx2_dfd(vk)
    header_size = 80 + 24 * count
    position = (header_size + len(dfd) + 15) & ~15
    index = [None] * count
    body = bytearray()
    for level in reversed(range(count)):
        pad = (-(position + len(body))) & 15
        body += bytes(pad)
        index[level] = (position + len(body), len(levels[level]), len(levels[level]))
        body += levels[level]
    out = bytearray(KTX2_IDENTIFIER)
    out += struct.pack("<9I", vk, 1, width, height, 0, 0, 1, count, 0)
    out += struct.pack("<IIIIQQ", header_size, len(dfd), 0, 0, 0, 0)
    for entry in index:
        out += struct.pack("<QQQ", *entry)
    out += dfd
    out += bytes(((header_size + len(dfd) + 15) & ~15) - (header_size + len(dfd)))
    out += body
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # (whole or not at all: shaders share their detail maps, and the workers may write one at once)
    partial = path.with_name(f"{path.name}.{os.getpid()}.partial")
    partial.write_bytes(bytes(out))
    os.replace(partial, path)
    return len(out)


def chain_rgba(rgba):
    """an RGBA uint8 picture's whole chain, each level the one before halved (box)"""
    levels = [rgba]
    while max(levels[-1].shape[:2]) > 1:
        h, w = levels[-1].shape[:2]
        levels.append(np.asarray(Image.fromarray(levels[-1], "RGBA").resize((max(1, w // 2), max(1, h // 2)),
                                                                             Image.BOX), np.uint8))
    return levels


def write_bc7(path, rgba):
    levels = chain_rgba(rgba)
    return ktx2_write(path, VK_BC7, rgba.shape[1], rgba.shape[0], [texstore_synth.bc7_encode(level) for level in levels])


def write_bc7_chain(path, levels):
    """a chain of levels made before (RGBA uint8, the largest first: CE's own), as BC7, carried on to 1 x 1 by halving
    its last; False (nothing written) where they are not a power-of-two chain"""
    h, w = levels[0].shape[:2]
    if w & (w - 1) or h & (h - 1):
        return False
    for k, level in enumerate(levels):
        if level.shape[:2] != (max(1, h >> k), max(1, w >> k)):
            return False
    levels = list(levels[:-1]) + chain_rgba(levels[-1])
    ktx2_write(path, VK_BC7, w, h, [texstore_synth.bc7_encode(level) for level in levels])
    return True


def write_normal(path, normals):
    """unit normals (h, w, 3) as BC5 (x and y), each level the one before's mean normalized again"""
    levels = []
    n = normals.astype(np.float32)
    while True:
        rg = np.clip(np.rint((n[..., :2] * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)
        levels.append(microtex_ktx2.bc5_encode(rg))
        h, w = n.shape[:2]
        if max(h, w) <= 1:
            break
        nh, nw = max(1, h // 2), max(1, w // 2)
        n = n[:nh * (h // nh), :nw * (w // nw)].reshape(nh, h // nh, nw, w // nw, 3).mean((1, 3))
        n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-6
    return ktx2_write(path, VK_BC5, normals.shape[1], normals.shape[0], levels)


def write_bc4(path, channel):
    """a channel (h, w) 0 to 1 as BC4, its whole chain (box)"""
    levels = []
    c = channel.astype(np.float32)
    while True:
        levels.append(microtex_ktx2.bc4_encode(np.clip(np.rint(c * 255), 0, 255).astype(np.uint8)).tobytes())
        h, w = c.shape
        if max(h, w) <= 1:
            break
        nh, nw = max(1, h // 2), max(1, w // 2)
        c = c[:nh * (h // nh), :nw * (w // nw)].reshape(nh, h // nh, nw, w // nw).mean((1, 3))
    return ktx2_write(path, VK_BC4, channel.shape[1], channel.shape[0], levels)


# ---------------------------------------------------------------- the inputs


def tag_path(tag):
    return tag.replace("\\", "/")


def bitmap_png(extract, tag, index=0):
    """the PNG of a bitmap tag's bitmap in the extract (its first face, level 0); None if there is none"""
    meta = extract / "bitmaps" / (tag_path(tag) + ".json")
    if not meta.is_file():
        return None
    bitmaps = json.loads(meta.read_text()).get("bitmaps", [])
    if index >= len(bitmaps) or not bitmaps[index].get("png"):
        return None
    path = extract / bitmaps[index]["png"][0]
    return path if path.is_file() else None


def bitmap_levels(extract, tag, index=0):
    """a bitmap tag's bitmap's levels (2D: level 0, then _mip1, _mip2, ...) in an extract made with --mips, as
    paths; [] where it has no more than its first there"""
    meta = extract / "bitmaps" / (tag_path(tag) + ".json")
    if not meta.is_file():
        return []
    bitmaps = json.loads(meta.read_text()).get("bitmaps", [])
    if index >= len(bitmaps) or len(bitmaps[index].get("png", [])) < 2:
        return []
    paths = [extract / p for p in bitmaps[index]["png"]]
    if not all(p.is_file() for p in paths) or any(not p.name.endswith(f"_mip{k}.png") for k, p in enumerate(paths)
                                                  if k):
        return []
    return paths


def dxt_decode(data, width, height, kind):
    """DXT1, DXT3 or DXT5 blocks (little-endian, as a PC bitmap tag keeps them) to RGBA uint8"""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    size = 8 if kind == "dxt1" else 16
    blocks = np.frombuffer(data[:bw * bh * size], np.uint8).reshape(bh * bw, size)
    colour = blocks[:, -8:]
    c0 = colour[:, 0].astype(np.uint32) | (colour[:, 1].astype(np.uint32) << 8)
    c1 = colour[:, 2].astype(np.uint32) | (colour[:, 3].astype(np.uint32) << 8)
    bits = (colour[:, 4].astype(np.uint32) | (colour[:, 5].astype(np.uint32) << 8) |
            (colour[:, 6].astype(np.uint32) << 16) | (colour[:, 7].astype(np.uint32) << 24))

    def rgb565(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)

    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1) | (kind != "dxt1")
    palette = np.zeros((len(blocks), 4, 4), np.float32)
    palette[:, 0, :3], palette[:, 1, :3] = p0, p1
    palette[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2)
    palette[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) / 3, 0)
    palette[:, :, 3] = 255
    palette[:, 3, 3] = np.where(four, 255, 0)
    index = np.stack([(bits >> (2 * k)) & 3 for k in range(16)], -1)
    texels = np.take_along_axis(palette, index[..., None].repeat(4, -1).astype(np.int64), 1)
    if kind == "dxt3":
        alpha = blocks[:, :8]
        nibbles = np.stack([(alpha[:, k // 2] >> (4 * (k % 2))) & 15 for k in range(16)], -1).astype(np.float32)
        texels[..., 3] = nibbles * 17
    elif kind == "dxt5":
        a0, a1 = blocks[:, 0].astype(np.float32), blocks[:, 1].astype(np.float32)
        abits = np.zeros(len(blocks), np.uint64)
        for k in range(6):
            abits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        aindex = np.stack([((abits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int64) for k in range(16)], -1)
        apal = np.zeros((len(blocks), 8), np.float32)
        apal[:, 0], apal[:, 1] = a0, a1
        eight = a0 > a1
        for i in range(1, 7):
            apal[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) / 7, 0)
        for i in range(1, 5):
            apal[:, i + 1] = np.where(eight, apal[:, i + 1], ((5 - i) * a0 + i * a1) / 5)
        apal[:, 6] = np.where(eight, apal[:, 6], 0)
        apal[:, 7] = np.where(eight, apal[:, 7], 255)
        texels[..., 3] = np.take_along_axis(apal, aindex, 1)
    picture = texels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.clip(np.rint(picture[:height, :width]), 0, 255).astype(np.uint8)


def bitmap_tag_rgba(path, index=0):
    """a Halo PC bitmap tag's bitmap (level 0) as RGBA uint8, DXT or 32-bit ones; None if it is another kind"""
    tag = mod_bitmap_check.read(str(path))
    if index >= len(tag["bitmaps"]):
        return None
    bitmap = tag["bitmaps"][index]
    data = Path(path).read_bytes()
    start = mod_bitmap_check.HDR + mod_bitmap_check.GROUP + tag["color_plate_bytes"] + bitmap["pixels_offset"]
    width, height, kind = bitmap["width"], bitmap["height"], bitmap["format"]
    if kind in ("dxt1", "dxt3", "dxt5"):
        return dxt_decode(data[start:], width, height, kind)
    if kind in ("a8r8g8b8", "x8r8g8b8"):
        bgra = np.frombuffer(data[start:start + width * height * 4], np.uint8).reshape(height, width, 4)
        rgba = bgra[..., [2, 1, 0, 3]].copy()
        if kind == "x8r8g8b8":
            rgba[..., 3] = 255
        return rgba
    return None


# ---------------------------------------------------------------- the materials


class Recipe:
    def __init__(self, path):
        self.data = json.loads(Path(path).read_text())
        self.types = self.data["material_types"]

    def prior(self, material_type):
        if 0 <= material_type < len(self.types):
            return self.types[material_type]
        return {"roughness": 0.6, "metal": 0, "band": [0.1, 1.0]}


def environment_material(job):
    """an environment shader's material: (tag, json or None, report)"""
    shader, extract, out, recipe_path, again, detail_mips = job
    recipe = Recipe(recipe_path)
    env = recipe.data["environment"]
    gate = recipe.data["gate"]
    tag = shader["tag"]
    report = {"tag": tag, "family": "senv"}
    maps = shader.get("maps", {})
    folder = Path(out) / "textures" / "prism_v4" / tag_path(tag)
    json_path = Path(out) / "materials" / (tag_path(tag) + ".pack.json")
    animation = shader.get("texture_animation", [0, 1, 0, 0, 1, 0])
    moves = any(f not in (0, 1) and s != 0 for f, s in ((animation[0], animation[2]), (animation[3], animation[5])))
    shifts = any(f == 0 and abs(s - round(s)) > 1e-6 for f, s in ((animation[0], animation[2]), (animation[3],
                                                                                                    animation[5])))
    if moves or shifts:
        report["skipped"] = "its texture moves (texture animation): left to the automatic tier"
        return tag, None, report
    base_png = bitmap_png(extract, maps["base_map"]) if "base_map" in maps else None
    if not base_png:
        report["skipped"] = "no base map in the extract"
        return tag, None, report
    if json_path.is_file() and not again:
        report["kept"] = "made before"
        return tag, json.loads(json_path.read_text()), report
    base = load_rgba(base_png)
    height, width = base.shape[:2]
    pw, ph = power_of_two(width), power_of_two(height)
    rgb = s2l(base[..., :3].astype(np.float64) / 255.0)
    lum = rgb @ LUM
    prior = recipe.prior(int(shader.get("material_type", 0)))
    brightness = float(np.clip(shader.get("specular_brightness", 0.0), 0, 1))
    bump_png = bitmap_png(extract, maps["bump_map"]) if "bump_map" in maps else None
    scale = shader.get("runtime_bump_scale") or [1.0, 1.0]
    if not all(math.isfinite(s) and s > 0 for s in scale):
        scale = [1.0, 1.0]
    started = time.time()

    # the relief at the base map's texels: CE's bump map at its scale, else none
    normals_base = None
    bump = None
    if bump_png:
        bump = load_rgba(bump_png)
        normals_base = normals_of(sample_wrap(bump[..., :3], height, width, scale) / 255.0)
        relief = height_from_normal(normals_base)
    else:
        relief = None

    # de-lighting: the painted relief light regressed on the relief, divided out where it explains enough
    fit = None
    r2 = 0.0
    low = max(height, width) / 16.0
    if relief is not None:
        log_lum = np.log(lum + 1e-3)
        band = log_lum - blur(log_lum, low)
        slope = np.hypot(np.gradient(relief, axis=1), np.gradient(relief, axis=0))
        features = [relief - blur(relief, 4.0), relief - blur(relief, 16.0), slope, blur(slope, 2.0),
                    normals_base[..., 0], -normals_base[..., 1]]
        x = np.stack(features + [np.ones_like(relief)], -1).reshape(-1, len(features) + 1)
        y = band.reshape(-1)
        coefficients, *_ = np.linalg.lstsq(x, y, rcond=None)
        residual = y - x @ coefficients
        r2 = float(1 - residual.var() / max(y.var(), 1e-12))
        if r2 >= env["delight_r2"]:
            # (the fit above the low band alone: the map's layout and colours below a sixteenth of it are CE's)
            fit = (x @ coefficients).reshape(relief.shape) - coefficients[-1]
            fit = fit - blur(fit, low)
    report["painted_r2"] = round(r2, 3)

    def delight(strength):
        if fit is None or strength <= 0:
            return rgb, None
        gain = np.exp(np.clip(-strength * fit, *np.log(env["delight_gain"])))
        # (the light's low band given back: the de-lit luminance's low band CE's, so that the map's layout and
        # mean light do not move)
        gain *= blur(lum, low) / np.maximum(blur(lum * gain, low), 1e-6)
        return rgb * gain[..., None], gain

    # the gate: the composite's low band against CE's, the de-lighting made weaker until it passes (none at last)
    detail_means = []
    kind = int(shader.get("type", 0))
    for slot, function_key in (("primary_detail_map", "detail_function"), ("micro_detail_map",
                                                                           "micro_detail_function")):
        png = bitmap_png(extract, maps[slot]) if slot in maps else None
        mean = (load_rgba(png)[..., :3].reshape(-1, 3).mean(0) / 255.0) if png else np.array([128 / 255.0] * 3)
        detail_means.append((int(shader.get(function_key, 0)), mean))
    drawn = base[..., 3] >= 128 if int(shader.get("flags", 0)) & 1 else None
    for strength in (env["delight_strength"], env["delight_strength"] * 0.6, env["delight_strength"] * 0.3, 0.0):
        delit, gain = delight(strength)
        delit_srgb = l2s(delit)
        mean_delta, p95_delta = low_band_delta(base[..., :3] / 255.0, delit_srgb, detail_means, drawn)
        if mean_delta <= gate["low_band_mean"] and p95_delta <= gate["low_band_p95"]:
            break
    report["low_band_delta_e"] = [round(mean_delta, 3), round(p95_delta, 3)]
    if mean_delta > gate["low_band_mean"] or p95_delta > gate["low_band_p95"]:
        report["failed"] = "the gate: the composite's low band moved past CIEDE2000 1.0 mean or 3 at p95"
        return tag, None, report
    report["delight_strength"] = round(strength, 3) if fit is not None else None
    if gain is not None:
        report["gain_p1_p99"] = [round(float(np.percentile(gain, 1)), 3), round(float(np.percentile(gain, 99)), 3)]
    albedo = np.concatenate([np.clip(np.rint(delit_srgb * 255), 0, 255).astype(np.uint8), base[..., 3:4]], -1)

    # the specular mask CE reads: the base map's alpha (normal, blended base specular), the bump map's alpha
    # (its flag), none (blended: the detail maps' alpha)
    flags = int(shader.get("flags", 0))
    if flags & 2 and bump is not None:
        mask = sample_wrap(bump[..., 3:4], height, width, scale)[..., 0] / 255.0
    elif kind in (0, 2):
        mask = base[..., 3].astype(np.float64) / 255.0
    else:
        mask = np.ones((height, width))
    # roughness: the type's prior made smoother by CE's specular brightness (the automatic tier's), the mask
    # smoother where it is bright, rougher in the relief's cavities, smoother on its edges; kept in the band
    rough = prior["roughness"] * (1 - 0.3 * brightness) + env["mask_roughness"] * (0.5 - mask)
    occlusion = np.ones((height, width))
    if relief is not None:
        cavity = relief - blur(relief, 8.0)
        cavity /= cavity.std() + 1e-6
        concave = np.clip(-cavity / 2, 0, 1)
        convex = np.clip(cavity / 2, 0, 1)
        rough = rough + env["cavity_roughness"] * concave + env["edge_roughness"] * convex
        occlusion = np.clip(1 - 0.5 * concave, 0.5, 1)
    rough = np.clip(rough, *prior["band"])
    # metal: only for metal types where CE also reflected, where the mask says
    reflects = "reflection_cube_map" in maps and max(shader.get("reflection_perpendicular", 0),
                                                       shader.get("reflection_parallel", 0)) > 0
    metal = np.clip(mask, 0, 1) if prior["metal"] and reflects else np.zeros((height, width))
    orm = np.stack([occlusion, rough, metal, np.ones_like(rough)], -1)
    orm = np.clip(np.rint(orm * 255), 0, 255).astype(np.uint8)
    report["roughness_p5_p50_p95"] = [round(float(np.percentile(rough, q)), 3) for q in (5, 50, 95)]
    report["metal_mean"] = round(float(metal.mean()), 3)

    # the images
    files = {}
    write_bc7(folder / "albedo.ktx2", resized(albedo, pw, ph))
    files["albedo"] = "albedo.ktx2"
    write_bc7(folder / "orm.ktx2", resized(orm, pw, ph))
    files["orm"] = "orm.ktx2"
    normal_uv = "base"
    if bump is not None:
        # (CE's bump map itself, at its own coordinates: unit vectors again)
        bh, bw = bump.shape[:2]
        n = normals_of(bump[..., :3] / 255.0)
        n = normals_of((resized(np.concatenate([np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8),
                                                np.full((bh, bw, 1), 255, np.uint8)], -1),
                                power_of_two(bw), power_of_two(bh))[..., :3]) / 255.0)
        write_normal(folder / "normal.ktx2", n)
        normal_uv = "bump"
        write_bc4(folder / "height.ktx2", np.clip(0.5 + 0.15 * relief, 0, 1)[:ph, :pw] if relief.shape == (ph, pw)
                  else resized_float(np.clip(0.5 + 0.15 * relief, 0, 1), pw, ph))
        files["height"] = "height.ktx2"
    else:
        log_lum = np.log(s2l(delit_srgb) @ LUM + 1e-3)
        detail = log_lum - blur(log_lum, max(height, width) / 32.0)
        n = normal_from_height(detail / (detail.std() + 1e-6), env["derived_normal_strength"])
        n = resized(np.concatenate([np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8),
                                    np.full((height, width, 1), 255, np.uint8)], -1), pw, ph)
        write_normal(folder / "normal.ktx2", normals_of(n[..., :3] / 255.0))
    files["normal"] = "normal.ktx2"
    layers = []
    for slot, name in (("primary_detail_map", "primary_detail"), ("secondary_detail_map", "secondary_detail"),
                       ("micro_detail_map", "micro_detail")):
        if slot not in maps:
            continue
        png = bitmap_png(extract, maps[slot])
        if not png:
            continue
        detail_path = Path(out) / "textures" / "prism_v4" / "details" / (tag_path(maps[slot]) + ".ktx2")
        # (CE's own levels, which fade toward grey, where the extract has them; else a chain halved from level 0)
        if not detail_path.is_file() or again:
            levels = bitmap_levels(Path(detail_mips), maps[slot]) if detail_mips else []
            chain = "halved"
            if levels:
                chain = f"ce's {len(levels)} levels" if write_bc7_chain(detail_path, [load_rgba(p) for p in levels]) \
                    else "halved (ce's levels are not a power-of-two chain)"
            if not chain.startswith("ce's"):
                picture = load_rgba(png)
                write_bc7(detail_path, resized(picture, power_of_two(picture.shape[1]),
                                               power_of_two(picture.shape[0])))
            report.setdefault("detail_chains", {})[name] = chain
        layers.append({"slot": name, "albedo": "textures/prism_v4/details/" + tag_path(maps[slot]) + ".ktx2",
                       "scale": "ce", "function": "ce"})
    relative = "textures/prism_v4/" + tag_path(tag) + "/"
    material = {
        "format": FORMAT,
        "family": "senv",
        "base": {
            "albedo": relative + files["albedo"], "albedo_factor": [1, 1, 1],
            "normal": relative + files["normal"], "normal_scale": 1.0, "normal_uv": normal_uv,
            "orm": relative + files["orm"], "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
        },
        "layers": layers,
        "type": "ce",
        "specular": {"f0": round(0.08 * (0.5 + 0.25 * brightness), 4)},
        "translucency": prior.get("translucency", 0.0),
        "alpha": {"mode": "ce"},
        "source": f"pack:{PACK}@{VERSION}",
    }
    if "height" in files:
        material["base"]["height"] = relative + files["height"]
        material["base"]["height_scale"] = 0.02
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(material, indent=2) + "\n")
    report["written"] = True
    report["seconds"] = round(time.time() - started, 1)
    return tag, material, report


def overlay_weights(spec, multipurpose, class_mask, shape):
    """the overlay's class weights of a model shader's texels (mod_materials.c's rule): the channel rule's slot A
    over B, a class mask's four slots; (slots, weights (h, w, n))"""
    slots = spec.get("slots", [])
    h, w = shape
    n = len(slots)
    weights = np.zeros((h, w, max(n, 1)))
    rule = None
    channel = spec.get("mask")
    if channel and multipurpose is not None:
        value = multipurpose[..., "rgba".index(channel.get("channel", "r"))].astype(np.float64) / 255.0
        lo, hi = channel.get("lo", 0.3), channel.get("hi", 0.6)
        rule = np.clip((value - lo) / max(hi - lo, 1e-6), 0, 1)
        if channel.get("invert"):
            rule = 1 - rule
    if class_mask is not None and n > 1:
        mask = class_mask.astype(np.float64) / 255.0
        if rule is None:
            weights[..., :n] = mask[..., :n]
        elif spec.get("class_mask_wins"):
            weights[..., 0] = mask[..., 0] + mask[..., 1] * rule
            weights[..., 1] = mask[..., 1] * (1 - rule)
            for k in range(2, n):
                weights[..., k] = mask[..., k]
        else:
            weights[..., 0] = rule
            rest = mask[..., 1:n]
            total = rest.sum(-1, keepdims=True)
            share = np.where(total > 1e-6, rest / np.maximum(total, 1e-6), np.eye(n - 1)[0])
            weights[..., 1:n] = (1 - rule)[..., None] * share
    elif rule is not None and n > 1:
        weights[..., 0] = rule
        weights[..., 1] = 1 - rule
    else:
        weights[..., 0] = 1
    total = weights.sum(-1, keepdims=True)
    return slots, weights / np.maximum(total, 1e-6)


def model_material(job):
    """a character's model shader's material: (tag, json or None, report)"""
    shader, extract, out, recipe_path, again, packs, side = job
    recipe = Recipe(recipe_path)
    models = recipe.data["models"]
    gate = recipe.data["gate"]
    tag = shader["tag"]
    report = {"tag": tag, "family": "soso"}
    maps = shader.get("maps", {})
    folder = Path(out) / "textures" / "prism_v4" / tag_path(tag)
    json_path = Path(out) / "materials" / (tag_path(tag) + ".pack.json")
    if "base_map" not in maps:
        report["skipped"] = "no base map"
        return tag, None, report
    ce_png = bitmap_png(extract, maps["base_map"])
    if not ce_png:
        report["skipped"] = "no base map in the extract"
        return tag, None, report
    if json_path.is_file() and not again:
        report["kept"] = "made before"
        return tag, json.loads(json_path.read_text()), report
    started = time.time()
    ce = load_rgba(ce_png)
    # the base map: Bold's (the texture packs' last), else base_local's upscale, else CE's own
    base, source = None, "ce"
    packs = Path(packs) if packs else None
    for pack in ("prism_v2_tex_bold", "prism_base_local"):
        candidate = packs / pack / "tags" / (tag_path(maps["base_map"]) + ".bitmap") if packs else None
        if candidate and candidate.is_file():
            try:
                picture = bitmap_tag_rgba(candidate)
            except (ValueError, KeyError, IndexError, struct.error):
                picture = None
            if picture is not None and picture.shape[0] >= ce.shape[0]:
                base, source = picture, pack
                break
    if base is None:
        base = ce
    longest = max(base.shape[:2])
    if longest > side:
        k = side / longest
        base = resized(base, max(1, int(round(base.shape[1] * k))), max(1, int(round(base.shape[0] * k))), Image.BOX)
    height, width = base.shape[:2]
    pw, ph = power_of_two(width), power_of_two(height)
    report["base_source"] = source
    report["size"] = [width, height]
    multipurpose = None
    if "multipurpose_map" in maps:
        png = bitmap_png(extract, maps["multipurpose_map"])
        if png:
            multipurpose = resized(load_rgba(png), width, height, Image.BILINEAR)

    # de-lighting: the broad painted shading flattened inside the islands, the self-illuminated texels kept
    rgb = s2l(base[..., :3].astype(np.float64) / 255.0)
    lum = rgb @ LUM
    inside = (base[..., :3].max(-1) > 5).astype(np.float64)
    if multipurpose is not None:
        inside *= (multipurpose[..., 1] < 26)
    low = max(height, width) / 16.0

    def masked_blur(x, s):
        return blur(x * inside, s) / np.maximum(blur(inside, s), 1e-4)

    # (the shading painted at the scale of the model's features, a fiftieth of the map, against the level of its
    # region at the low band's scale: flattened; the low band itself, CE's colours and layout, kept)
    broad = masked_blur(lum, max(height, width) / 50.0)
    level = masked_blur(lum, low)
    shading = np.log(np.maximum(level, 1e-3) / np.maximum(broad, 1e-3))
    # (the gate: the de-lighting's change of its source's low band, made weaker until it passes; a pack's own grade
    # (Bold's) against CE's is reported beside it)
    drawn = base[..., 3] >= 128 if not (int(shader.get("flags", 0)) & 4) else None
    for strength in (models["delight_strength"], models["delight_strength"] * 0.6, models["delight_strength"] * 0.3,
                     0.0):
        log_gain = np.clip(strength * shading, *np.log(models["delight_gain"]))
        # (the gain's own low band taken out, inside the islands: the regions' levels kept)
        gain = np.where(inside > 0, np.exp(log_gain - masked_blur(log_gain, low)), 1.0)
        delit_srgb = l2s(rgb * gain[..., None])
        mean_delta, p95_delta = low_band_delta(base[..., :3] / 255.0, delit_srgb, None, drawn)
        if mean_delta <= gate["low_band_mean"] and p95_delta <= gate["low_band_p95"]:
            break
    report["delight_strength"] = round(strength, 3)
    albedo = np.concatenate([np.clip(np.rint(delit_srgb * 255), 0, 255).astype(np.uint8), base[..., 3:4]], -1)
    report["low_band_delta_e"] = [round(mean_delta, 3), round(p95_delta, 3)]
    if source != "ce":
        reference = resized(ce, width, height, Image.BICUBIC)[..., :3] / 255.0
        report["source_against_ce_delta_e"] = [round(v, 3) for v in low_band_delta(reference, base[..., :3] / 255.0,
                                                                                    None, drawn)]
    if mean_delta > gate["low_band_mean"] or p95_delta > gate["low_band_p95"]:
        report["failed"] = "the gate: the base colour's low band moved past CIEDE2000 1.0 mean or 3 at p95"
        return tag, None, report

    # roughness and metal: the overlay's classes (its rule and class masks), CE's specular mask smoother
    character = recipe.data["characters"]
    rough = np.full((height, width), character["roughness"])
    metal = np.full((height, width), character["metal"])
    overlay = packs / "prism_materials" / "tags" / "materials" / "overlay.json" if packs else None
    spec = None
    if overlay and overlay.is_file():
        spec = json.loads(overlay.read_text()).get("shaders", {}).get(tag)
    if spec:
        class_mask = None
        for name in (spec.get("class_mask") or [])[:1]:
            path = packs / "prism_class_masks" / "tags" / "materials" / name
            if path.is_file():
                class_mask = resized(load_rgba(path), width, height, Image.BILINEAR)
        slots, weights = overlay_weights(spec, multipurpose, class_mask, (height, width))
        classes = recipe.data["classes"]
        if slots:
            rough = sum(weights[..., k] * classes.get(slot, character)["roughness"] for k, slot in enumerate(slots))
            metal = sum(weights[..., k] * classes.get(slot, character)["metal"] for k, slot in enumerate(slots))
        report["classes"] = slots
    if multipurpose is not None:
        rough = rough - models["specular_roughness"] * (multipurpose[..., 0] / 255.0 - 0.5)
    # a surface map (a remodel's bake, the packs'): its normal, occlusion and roughness
    occlusion = np.ones((height, width))
    surface = None
    for pack in ("prism_v2_tex_bold", "prism_v2r2_chief_3p_maps", "prism_v2_elite_r2", "prism_v2r2_fp_hands_ar"):
        candidate = packs / pack / "tags" / (tag_path(maps["base_map"]) + ".surface.png") if packs else None
        if candidate and candidate.is_file():
            surface = resized(load_rgba(candidate), width, height, Image.BILINEAR)
            report["surface"] = pack
            break
    files = {}
    relative = "textures/prism_v4/" + tag_path(tag) + "/"
    if surface is not None:
        occlusion = surface[..., 2] / 255.0
        rough = 0.5 * rough + 0.5 * surface[..., 3] / 255.0
        n = surface[..., :3].astype(np.float32) / 255.0 * 2 - 1
        n[..., 1] = -n[..., 1]
        n[..., 2] = np.sqrt(np.clip(1 - n[..., 0] ** 2 - n[..., 1] ** 2, 0, 1))
        n = resized(np.concatenate([np.clip(np.rint((n * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8),
                                    np.full((height, width, 1), 255, np.uint8)], -1), pw, ph)
        write_normal(folder / "normal.ktx2", normals_of(n[..., :3] / 255.0))
        files["normal"] = relative + "normal.ktx2"
    rough = np.clip(rough, *character["band"])
    metal = np.clip(metal, 0, 1)
    report["roughness_p5_p50_p95"] = [round(float(np.percentile(rough, q)), 3) for q in (5, 50, 95)]
    report["metal_mean"] = round(float(metal.mean()), 3)
    orm = np.clip(np.rint(np.stack([occlusion, rough, metal, np.ones_like(rough)], -1) * 255), 0, 255).astype(np.uint8)
    write_bc7(folder / "albedo.ktx2", resized(albedo, pw, ph))
    write_bc7(folder / "orm.ktx2", resized(orm, pw, ph))
    files["albedo"] = relative + "albedo.ktx2"
    files["orm"] = relative + "orm.ktx2"
    material = {
        "format": FORMAT,
        "family": "soso",
        "base": {"albedo": files["albedo"], "albedo_factor": [1, 1, 1], "orm": files["orm"], "roughness": 1.0,
                 "metal": 1.0, "occlusion": 1.0},
        "specular": {"f0": 0.04},
        "translucency": float(np.clip(shader.get("translucency", 0.0), 0, 1)),
        "alpha": {"mode": "ce"},
        "source": f"pack:{PACK}@{VERSION}",
    }
    if "normal" in files:
        material["base"]["normal"] = files["normal"]
        material["base"]["normal_scale"] = 1.0
        material["base"]["normal_uv"] = "base"
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(material, indent=2) + "\n")
    report["written"] = True
    report["seconds"] = round(time.time() - started, 1)
    return tag, material, report


# ---------------------------------------------------------------- the command line


def shaders_of(folder):
    for path in sorted(Path(folder).glob("shaders/**/*.json")):
        yield json.loads(path.read_text())


def build(a):
    extract = Path(a.extract).resolve()
    shaders = Path(a.shaders).resolve() if a.shaders else extract
    out = Path(a.out).resolve()
    recipe = str(Path(a.recipe).resolve())
    jobs_environment, jobs_model = [], []
    for shader in shaders_of(shaders):
        if shader["group"] == "senv":
            jobs_environment.append((shader, extract, out, recipe, a.again,
                                     str(Path(a.detail_mips).resolve()) if a.detail_mips else None))
        elif shader["group"] == "soso" and shader["tag"].startswith("characters\\"):
            jobs_model.append((shader, extract, out, recipe, a.again, a.packs, a.side))
    results = []
    with ProcessPoolExecutor(max_workers=max(1, a.workers)) as pool:
        for result in pool.map(environment_material, jobs_environment):
            results.append(result)
            print(f"  {result[0]}: {'written' if result[2].get('written') else result[2].get('kept') or result[2].get('failed') or result[2].get('skipped')}", flush=True)
        for result in pool.map(model_material, jobs_model):
            results.append(result)
            print(f"  {result[0]}: {'written' if result[2].get('written') else result[2].get('kept') or result[2].get('failed') or result[2].get('skipped')}", flush=True)
    covered = [r for r in results if r[1] is not None]
    report = {
        "map": extract.name, "pack": PACK, "version": VERSION,
        "environment_shaders": len(jobs_environment),
        "environment_covered": sum(1 for r in covered if r[2]["family"] == "senv"),
        "model_shaders": len(jobs_model),
        "model_covered": sum(1 for r in covered if r[2]["family"] == "soso"),
        "failed_gate": [r[0] for r in results if "failed" in r[2]],
        "skipped": {r[0]: r[2]["skipped"] for r in results if "skipped" in r[2]},
        "shaders": [r[2] for r in results],
    }
    reports = out / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    (reports / f"{extract.name}.json").write_text(json.dumps(report, indent=1) + "\n")
    print(f"{extract.name}: environment {report['environment_covered']} of {report['environment_shaders']}, "
          f"characters' models {report['model_covered']} of {report['model_shaders']}, "
          f"{len(report['failed_gate'])} failed the gate, {len(report['skipped'])} skipped")
    return 0


def check(_a):
    """the encoders and the gate's maths on made-up pictures"""
    rng = np.random.default_rng(1)
    # (CIEDE2000 against Sharma et al.'s published pairs)
    pairs = [((50.0, 2.6772, -79.7751), (50.0, 0.0, -82.7485), 2.0425),
             ((50.0, -1.3802, -84.2814), (50.0, 0.0, -82.7485), 1.0),
             ((50.0, 2.5, 0.0), (73.0, 25.0, -18.0), 27.1492),
             ((50.0, 2.5, 0.0), (50.0, 3.1736, 0.5854), 1.0)]
    for first, second, expected in pairs:
        got = float(ciede2000(np.array(first), np.array(second)))
        assert abs(got - expected) < 1e-3, (first, second, got, expected)
    # (the gate: a picture against itself, and against a darker one)
    picture = rng.random((64, 64, 3))
    assert low_band_delta(picture, picture)[0] < 1e-9
    assert low_band_delta(picture, picture * 0.8)[0] > 1.0
    # (DXT1 decoding of a block of two colours)
    block = struct.pack("<HHI", 0xF800, 0x001F, 0b01010101010101010101010101010101)
    texels = dxt_decode(block, 4, 4, "dxt1")
    assert tuple(texels[0, 0]) == (0, 0, 255, 255) and texels.shape == (4, 4, 4)
    # (a height and the normals of its slopes, and back)
    y, x = np.mgrid[0:64, 0:64] / 64.0
    height = np.sin(2 * np.pi * x) + np.cos(4 * np.pi * y)
    normals = normal_from_height(height, 4.0)
    back = height_from_normal(normals)
    assert np.corrcoef(back.ravel(), height.ravel())[0, 1] > 0.99
    print("prism_v4_materials: check passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("--extract", required=True, help="a map's extract (map_extract's bitmaps)")
    p.add_argument("--shaders", help="its shaders (map_extract --only shaders: shaders/), else the extract's")
    p.add_argument("--detail-mips", help="its environment shaders' detail maps with every level (map_extract "
                                         "--only bitmaps --mips): CE's own levels kept in the pack's detail maps")
    p.add_argument("--out", required=True, help="the pack's mod folder (mods/prism_v4_materials)")
    p.add_argument("--packs", help="the mods folder with the PRISM texture packs already built (Bold, base_local, "
                                   "the overlay's classes and surface maps)")
    p.add_argument("--recipe", default=str(HERE / "recipes" / "prism_v4_materials.json"))
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--side", type=int, default=2048, help="a character's maps' longest side at most")
    p.add_argument("--again", action="store_true", help="make every shader's material again")
    sub.add_parser("check")
    a = parser.parse_args()
    return build(a) if a.command == "build" else check(a)


if __name__ == "__main__":
    sys.exit(main())
