"""Minimal KTX2 (Khronos Texture 2.0) writer/reader for 2D textures with mips.

Formats: VK_FORMAT_BC5_UNORM_BLOCK (141) with a numpy BC5 encoder, and
VK_FORMAT_R8G8B8A8_UNORM (37). No supercompression. Level data is stored smallest first,
as the spec orders it; the level index lists level 0 first.
"""
import struct

import numpy as np

IDENT = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
VK_BC5_UNORM = 141
VK_RGBA8_UNORM = 37


# --- BC4 / BC5 -----------------------------------------------------------------------------
def _blocks(ch):
    """uint8 (h, w) -> (bh, bw, 16) 4x4 blocks; pads small levels by edge replication."""
    h, w = ch.shape
    H, W = max(4, (h + 3) // 4 * 4), max(4, (w + 3) // 4 * 4)
    if (H, W) != (h, w):
        ch = np.pad(ch, ((0, H - h), (0, W - w)), mode="wrap" if h >= 4 else "edge")
    return ch.reshape(H // 4, 4, W // 4, 4).transpose(0, 2, 1, 3).reshape(H // 4, W // 4, 16)


def bc4_encode(ch):
    b = _blocks(ch).astype(np.int32)
    e0 = b.max(-1)
    e1 = b.min(-1)
    rng = np.maximum(e0 - e1, 1)
    p = np.rint((e0[..., None] - b) * 7.0 / rng[..., None]).astype(np.int64)
    code = np.where(p == 0, 0, np.where(p == 7, 1, p + 1))
    code = np.where((e0 == e1)[..., None], 0, code)
    bits = np.zeros(code.shape[:2], np.uint64)
    for k in range(16):
        bits |= code[..., k].astype(np.uint64) << np.uint64(3 * k)
    out = np.zeros(code.shape[:2] + (8,), np.uint8)
    out[..., 0] = e0
    out[..., 1] = e1
    for k in range(6):
        out[..., 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def bc4_decode(blk, h, w):
    e0 = blk[..., 0].astype(np.float64)
    e1 = blk[..., 1].astype(np.float64)
    bits = np.zeros(blk.shape[:2], np.uint64)
    for k in range(6):
        bits |= blk[..., 2 + k].astype(np.uint64) << np.uint64(8 * k)
    pal = np.zeros(blk.shape[:2] + (8,))
    pal[..., 0], pal[..., 1] = e0, e1
    m8 = e0 > e1
    for i in range(1, 7):
        pal[..., i + 1] = np.where(m8, ((7 - i) * e0 + i * e1) / 7.0, 0)
    for i in range(1, 5):
        pal[..., i + 1] = np.where(m8, pal[..., i + 1], ((5 - i) * e0 + i * e1) / 5.0)
    pal[..., 6] = np.where(m8, pal[..., 6], 0)
    pal[..., 7] = np.where(m8, pal[..., 7], 255)
    idx = np.stack([((bits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int64) for k in range(16)], -1)
    v = np.take_along_axis(pal, idx, -1)
    bh, bw = blk.shape[:2]
    img = v.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    return np.rint(img[:h, :w]).astype(np.uint8)


def bc5_encode(rg):
    return np.concatenate([bc4_encode(rg[..., 0]), bc4_encode(rg[..., 1])], -1).tobytes()


def bc5_decode(data, h, w):
    bh, bw = max(1, (h + 3) // 4), max(1, (w + 3) // 4)
    blk = np.frombuffer(data, np.uint8).reshape(bh, bw, 16)
    return np.stack([bc4_decode(blk[..., :8], h, w), bc4_decode(blk[..., 8:], h, w)], -1)


# --- container -----------------------------------------------------------------------------
def _dfd(vk):
    if vk == VK_BC5_UNORM:
        model, dims, plane, samples = 132, (3, 3, 0, 0), 16, [(0, 63, 0), (64, 63, 1)]
    elif vk == VK_RGBA8_UNORM:
        model, dims, plane, samples = 1, (0, 0, 0, 0), 4, [(0, 7, 0), (8, 7, 1), (16, 7, 2), (24, 7, 15)]
    else:
        raise ValueError(vk)
    blk = struct.pack("<IHH", 0, 2, 24 + 16 * len(samples))
    blk += bytes([model, 1, 1, 0]) + bytes(dims) + bytes([plane, 0, 0, 0, 0, 0, 0, 0])
    for off, length, ch in samples:
        upper = 0xFFFFFFFF if vk == VK_BC5_UNORM else 255
        blk += struct.pack("<HBB4BII", off, length, ch, 0, 0, 0, 0, 0, upper)
    return struct.pack("<I", 4 + len(blk)) + blk


def _kvd(pairs):
    out = b""
    for k, v in pairs:
        kv = k.encode() + b"\0" + v.encode() + b"\0"
        out += struct.pack("<I", len(kv)) + kv
        out += b"\0" * ((-len(out)) % 4)
    return out


def write(path, vk, width, height, levels, writer="prism microtex"):
    """levels: list of bytes, level 0 (largest) first."""
    align = 16 if vk == VK_BC5_UNORM else 4
    n = len(levels)
    dfd = _dfd(vk)
    kvd = _kvd([("KTXorientation", "rd"), ("KTXwriter", writer)])
    hdr_len = 12 + 36 + 32 + 24 * n
    dfd_off = hdr_len
    kvd_off = dfd_off + len(dfd)
    pos = kvd_off + len(kvd)
    body = bytearray()
    offsets = [0] * n
    for li in range(n - 1, -1, -1):
        pad = (-(pos + len(body))) % align
        body += b"\0" * pad
        offsets[li] = pos + len(body)
        body += levels[li]
    out = bytearray(IDENT)
    out += struct.pack("<9I", vk, 1, width, height, 0, 0, 1, n, 0)
    out += struct.pack("<IIIIQQ", dfd_off, len(dfd), kvd_off, len(kvd), 0, 0)
    for li in range(n):
        out += struct.pack("<QQQ", offsets[li], len(levels[li]), len(levels[li]))
    out += dfd + kvd + body
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def read(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:12] != IDENT:
        raise ValueError("not KTX2")
    vk, ts, w, h, depth, layers, faces, n, sc = struct.unpack_from("<9I", d, 12)
    dfd_off, dfd_len, kvd_off, kvd_len, _, _ = struct.unpack_from("<IIIIQQ", d, 48)
    levels = []
    for li in range(max(n, 1)):
        off, ln, uln = struct.unpack_from("<QQQ", d, 80 + 24 * li)
        levels.append(d[off:off + ln])
    return {"vk": vk, "width": w, "height": h, "levels": levels, "supercompression": sc,
            "dfd": d[dfd_off:dfd_off + dfd_len], "kvd": d[kvd_off:kvd_off + kvd_len]}
