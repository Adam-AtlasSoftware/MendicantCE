#!/usr/bin/env python3
"""Placeholder replacement textures for PRISM's texture store (display.prism_textures), for tests:
a map's extracted bitmaps (an extract's <tag>.json and PNGs) upscaled plainly (Lanczos) and written
as KTX2 files of BC7 blocks with whole chains, where a mod's textures folder has them
(<mod>/textures/<tier>/<tag path>#<bitmap index>.ktx2). Local only: the textures are the map's.

The BC7 encoder is a small one of this file's own (mode 6: one subset, RGBA endpoints of 7 bits and a
shared bit, 4-bit indices, the endpoints the block's colours' bounding box), checked against its own
decoder (--check: PSNR). It is for tests, not for shipping art.

  texstore_synth.py chief --extract <extract>/a10 --out <data>/mods/texstore_test [--grid]
      the Chief's cyborg#0 at 4096 x 4096 (a 1-pixel grid every 64 pixels and a mark stamped on its
      first level with --grid, which only its two most detailed levels show)
  texstore_synth.py set --extract <extract>/a10 --out <data>/mods/texstore_set [--prefix characters\\]
      every bitmap of the tags under the prefix that can take a replacement: 2D ones at 4096 on their
      longer side, cube maps at four times their size
  texstore_synth.py check  the encoder against its decoder on a test picture
"""

import argparse
import json
import math
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

KTX2_IDENTIFIER = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
VK_FORMAT_BC7_UNORM_BLOCK = 145
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int32)
SIDE_MAXIMUM = 4096


# ---------- BC7 mode 6

def blocks_of(rgba):
    """An H x W x 4 uint8 picture (sides multiples of 4, or 1 and 2) as N x 16 x 4 blocks, rows of blocks."""
    height, width = rgba.shape[:2]
    if height < 4 or width < 4:
        padded = np.zeros((max(height, 4), max(width, 4), 4), dtype=np.uint8)
        padded[:height, :width] = rgba
        # (the rest repeats the picture's edge, as the GPU never reads it)
        padded[height:, :width] = rgba[-1:, :, :]
        padded[:, width:] = padded[:, width - 1:width]
        rgba = padded
        height, width = rgba.shape[:2]
    blocks = rgba.reshape(height // 4, 4, width // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    return blocks


def bc7_encode(rgba):
    """BC7 mode 6 blocks (bytes) of an RGBA picture."""
    blocks = blocks_of(rgba).astype(np.int32)
    out = bytearray()
    for start in range(0, len(blocks), 1 << 14):
        out += _encode_chunk(blocks[start:start + (1 << 14)])
    return bytes(out)


def _quantize(endpoint):
    """8-bit endpoints (N x 4) as 7 bits and a shared bit, the choice of the bit closer to them."""
    best = None
    for p in (0, 1):
        q = np.clip(np.rint((endpoint - p) / 2.0), 0, 127).astype(np.int32)
        error = ((q * 2 + p - endpoint) ** 2).sum(axis=1)
        if best is None:
            best = (q, np.full(len(q), p, np.int32), error)
        else:
            better = error < best[2]
            best = (np.where(better[:, None], q, best[0]), np.where(better, p, best[1]), np.where(better, error, best[2]))
    return best[0], best[1]


def _encode_chunk(blocks):
    n = len(blocks)
    pixels = blocks.astype(np.float64)
    mean = pixels.mean(axis=1)
    centred = pixels - mean[:, None, :]
    # (the colours' principal axis, by a few rounds of power iteration from the bounding box's diagonal)
    covariance = np.einsum("npi,npj->nij", centred, centred)
    axis = (blocks.max(axis=1) - blocks.min(axis=1)).astype(np.float64) + 1e-3
    for _ in range(6):
        axis = np.einsum("nij,nj->ni", covariance, axis)
        norm = np.linalg.norm(axis, axis=1)
        axis = np.where(norm[:, None] > 1e-9, axis / np.maximum(norm, 1e-9)[:, None], axis)
    projection = np.einsum("npi,ni->np", centred, axis)
    low = np.clip(mean + projection.min(axis=1)[:, None] * axis, 0, 255)
    high = np.clip(mean + projection.max(axis=1)[:, None] * axis, 0, 255)
    q0, p0 = _quantize(low)
    q1, p1 = _quantize(high)
    e0 = q0 * 2 + p0[:, None]
    e1 = q1 * 2 + p1[:, None]
    # (each texel's index: the nearest of the 16 colours between the endpoints)
    palette = ((64 - WEIGHTS)[None, :, None] * e0[:, None, :] + WEIGHTS[None, :, None] * e1[:, None, :] + 32) >> 6
    distance = ((blocks[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    indices = distance.argmin(axis=2).astype(np.int32)
    # (the anchor's index has no top bit: endpoints swapped and indices turned where it has one)
    swap = indices[:, 0] >= 8
    q0s = np.where(swap[:, None], q1, q0)
    q1s = np.where(swap[:, None], q0, q1)
    p0s = np.where(swap, p1, p0)
    p1s = np.where(swap, p0, p1)
    indices = np.where(swap[:, None], 15 - indices, indices)
    # bits, low first: mode 6 (7 bits: 0000001), R0 R1 G0 G1 B0 B1 A0 A1 (7 each), P0, P1, indices
    words = np.zeros((n, 2), dtype=np.uint64)
    fields = [(np.full(n, 1 << 6, np.int64), 7)]
    for channel in range(4):
        fields.append((q0s[:, channel], 7))
        fields.append((q1s[:, channel], 7))
    fields.append((p0s, 1))
    fields.append((p1s, 1))
    fields.append((indices[:, 0], 3))
    for pixel in range(1, 16):
        fields.append((indices[:, pixel], 4))
    position = 0
    for value, bits in fields:
        value = value.astype(np.uint64)
        if position + bits <= 64:
            words[:, 0] |= value << np.uint64(position)
        elif position >= 64:
            words[:, 1] |= value << np.uint64(position - 64)
        else:
            low_bits = 64 - position
            words[:, 0] |= (value & np.uint64((1 << low_bits) - 1)) << np.uint64(position)
            words[:, 1] |= value >> np.uint64(low_bits)
        position += bits
    assert position == 128
    return words.astype("<u8").tobytes()


def bc7_decode(data, width, height):
    """Mode 6 blocks back to an RGBA picture (this file's own blocks)."""
    words = np.frombuffer(data, dtype="<u8").reshape(-1, 2)

    def bits(position, count):
        if position + count <= 64:
            return ((words[:, 0] >> np.uint64(position)) & np.uint64((1 << count) - 1)).astype(np.int32)
        if position >= 64:
            return ((words[:, 1] >> np.uint64(position - 64)) & np.uint64((1 << count) - 1)).astype(np.int32)
        low_bits = 64 - position
        low = (words[:, 0] >> np.uint64(position)) & np.uint64((1 << low_bits) - 1)
        high = words[:, 1] & np.uint64((1 << (count - low_bits)) - 1)
        return (low | (high << np.uint64(low_bits))).astype(np.int32)

    assert (bits(0, 7) == 64).all(), "not this file's mode 6 blocks"
    e = np.zeros((len(words), 2, 4), np.int32)
    position = 7
    for channel in range(4):
        e[:, 0, channel] = bits(position, 7)
        e[:, 1, channel] = bits(position + 7, 7)
        position += 14
    p0 = bits(position, 1)
    p1 = bits(position + 1, 1)
    position += 2
    e[:, 0] = e[:, 0] * 2 + p0[:, None]
    e[:, 1] = e[:, 1] * 2 + p1[:, None]
    indices = np.zeros((len(words), 16), np.int32)
    indices[:, 0] = bits(position, 3)
    position += 3
    for pixel in range(1, 16):
        indices[:, pixel] = bits(position, 4)
        position += 4
    w = WEIGHTS[indices]
    texels = ((64 - w)[:, :, None] * e[:, None, 0, :] + w[:, :, None] * e[:, None, 1, :] + 32) >> 6
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    picture = texels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return picture[:height, :width].astype(np.uint8)


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return float("inf") if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


# ---------- KTX2

def dfd_bc7():
    """The data format descriptor of BC7 (UNORM, linear): one basic block, one sample of 128 bits."""
    block = struct.pack("<II", 0, (40 << 16) | 2)          # vendor 0, type 0; version 2, 40 bytes
    block += struct.pack("<BBBB", 128, 1, 1, 0)             # model BC7, primaries BT.709, transfer linear, flags
    block += struct.pack("<BBBB", 3, 3, 0, 0)               # texel block 4 x 4
    block += struct.pack("<8B", 16, 0, 0, 0, 0, 0, 0, 0)    # 16 bytes a block
    block += struct.pack("<HBB", 0, 127, 0) + bytes(4) + struct.pack("<II", 0, 0xFFFFFFFF)
    return struct.pack("<I", 4 + len(block)) + block


def ktx2_write(path, width, height, faces, levels_data):
    """levels_data: level 0 first, each a list of the faces' bytes."""
    level_count = len(levels_data)
    dfd = dfd_bc7()
    header_size = 80 + 24 * level_count
    dfd_offset = header_size
    data_start = dfd_offset + len(dfd)
    data_start = (data_start + 15) & ~15
    index = [None] * level_count
    chunks = []
    offset = data_start
    # (the smallest level first in the file, as the tools write them)
    for level in reversed(range(level_count)):
        level_bytes = b"".join(levels_data[level])
        pad = (-offset) & 15
        if pad:
            chunks.append(bytes(pad))
            offset += pad
        index[level] = (offset, len(level_bytes), len(level_bytes))
        chunks.append(level_bytes)
        offset += len(level_bytes)
    header = KTX2_IDENTIFIER + struct.pack("<9I", VK_FORMAT_BC7_UNORM_BLOCK, 1, width, height, 0, 0, faces, level_count, 0)
    header += struct.pack("<IIIIQQ", dfd_offset, len(dfd), 0, 0, 0, 0)
    for entry in index:
        header += struct.pack("<QQQ", *entry)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".ktx2.part")
    with open(temporary, "wb") as file:
        file.write(header)
        file.write(dfd)
        file.write(bytes(data_start - (header_size + len(dfd))))
        for chunk in chunks:
            file.write(chunk)
    os.replace(temporary, path)
    return offset


# ---------- pictures

def chain(picture):
    """A whole chain of levels from an RGBA PIL picture, each the one before halved (box)."""
    levels = [picture]
    while max(levels[-1].size) > 1:
        w, h = levels[-1].size
        levels.append(levels[-1].resize((max(1, w // 2), max(1, h // 2)), Image.BOX))
    return levels


def stamp_grid(picture):
    """A 1-pixel grid every 64 pixels and a mark ("4K" and its corners), on the picture itself."""
    draw = ImageDraw.Draw(picture)
    w, h = picture.size
    for x in range(0, w, 64):
        draw.line([(x, 0), (x, h - 1)], fill=(255, 0, 255, 255), width=1)
    for y in range(0, h, 64):
        draw.line([(0, y), (w - 1, y)], fill=(255, 0, 255, 255), width=1)
    for cx, cy in ((w // 4, h // 4), (3 * w // 4, h // 4), (w // 4, 3 * h // 4), (3 * w // 4, 3 * h // 4)):
        draw.rectangle([cx - 40, cy - 24, cx + 40, cy + 24], outline=(0, 255, 0, 255), width=2)
        draw.text((cx - 10, cy - 6), "4K", fill=(0, 255, 0, 255))
    return picture


def replacement(job):
    """One bitmap's KTX2: (pngs, scale, out path, grid) -> (path, bytes, psnr of level 0 or None)."""
    pngs, scale, out, grid, check = job
    faces = []
    for png in pngs:
        picture = Image.open(png).convert("RGBA")
        w, h = picture.size
        picture = picture.resize((w << scale, h << scale), Image.LANCZOS)
        if grid:
            stamp_grid(picture)
        faces.append(chain(picture))
    width, height = faces[0][0].size
    levels = []
    measured = None
    for level in range(len(faces[0])):
        level_faces = []
        for face in faces:
            rgba = np.asarray(face[level], dtype=np.uint8)
            blocks = bc7_encode(rgba)
            if check and level == 0 and measured is None:
                measured = psnr(bc7_decode(blocks, rgba.shape[1], rgba.shape[0]), rgba)
            level_faces.append(blocks)
        levels.append(level_faces)
    size = ktx2_write(Path(out), width, height, len(faces), levels)
    return str(out), size, measured, width, height, len(levels)


def eligible(bitmap):
    return (bitmap["type"] in ("2d", "cube_map") and not bitmap.get("palettized") and bitmap.get("power_of_two")
            and not bitmap.get("linear") and bitmap["format"] != "p8")


def scale_for(bitmap, cube_scale, long_side):
    side = max(bitmap["width"], bitmap["height"])
    if bitmap["type"] == "cube_map":
        scale = cube_scale
    else:
        scale = max(0, int(math.log2(long_side // side))) if side <= long_side else 0
    while (side << scale) > SIDE_MAXIMUM:
        scale -= 1
    return max(scale, 0)


def tag_file(out, tier, tag, index):
    return Path(out) / "textures" / tier / (tag.replace("\\", "/") + f"#{index}.ktx2")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=["chief", "set", "check"])
    parser.add_argument("--extract", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--tier", default="max")
    parser.add_argument("--grid", action="store_true")
    parser.add_argument("--prefix", default="characters\\")
    parser.add_argument("--long-side", type=int, default=4096)
    parser.add_argument("--cube-scale", type=int, default=2)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--check", action="store_true", help="each file's level 0 decoded again: its PSNR")
    arguments = parser.parse_args()

    if arguments.what == "check":
        # (a picture as a texture is: colours of 32 x 32 noise upscaled, opaque, a soft alpha ramp in a corner;
        # or a PNG given with --extract)
        if arguments.extract and arguments.extract.suffix == ".png":
            base = Image.open(arguments.extract).convert("RGBA")
        else:
            rng = np.random.default_rng(1)
            colours = rng.integers(0, 255, (32, 32, 3), dtype=np.uint8)
            alpha = np.full((32, 32, 1), 255, np.uint8)
            alpha[:8, :8, 0] = np.linspace(0, 255, 8, dtype=np.uint8)[None, :]
            base = Image.fromarray(np.concatenate([colours, alpha], axis=2), "RGBA").resize((512, 512), Image.LANCZOS)
        rgba = np.asarray(base, dtype=np.uint8)
        blocks = bc7_encode(rgba)
        value = psnr(bc7_decode(blocks, rgba.shape[1], rgba.shape[0]), rgba)
        print(f"bc7 mode 6: {len(blocks)} bytes for {rgba.shape[1]}x{rgba.shape[0]}, PSNR {value:.2f} dB")
        return 0 if value > 35.0 else 1

    root = arguments.extract / arguments.extract.name if (arguments.extract / arguments.extract.name).is_dir() else arguments.extract
    jobs = []
    if arguments.what == "chief":
        description = json.loads((root / "bitmaps/characters/cyborg/bitmaps/cyborg.json").read_text())
        bitmap = description["bitmaps"][0]
        scale = int(math.log2(SIDE_MAXIMUM // max(bitmap["width"], bitmap["height"])))
        jobs.append(([root / png for png in bitmap["png"]], scale, tag_file(arguments.out, arguments.tier,
                     description["tag"], 0), arguments.grid, True))
    else:
        prefix = arguments.prefix.lower()
        for path in sorted(root.glob("bitmaps/**/*.json")):
            description = json.loads(path.read_text())
            if description.get("group") != "bitm" or not description["tag"].lower().startswith(prefix):
                continue
            for bitmap in description["bitmaps"]:
                if not eligible(bitmap) or not bitmap.get("png"):
                    continue
                scale = scale_for(bitmap, arguments.cube_scale, arguments.long_side)
                jobs.append(([root / png for png in bitmap["png"]], scale,
                             tag_file(arguments.out, arguments.tier, description["tag"], bitmap["index"]), False,
                             arguments.check))
    total = 0
    with ProcessPoolExecutor(max_workers=arguments.jobs) as pool:
        for out, size, measured, width, height, levels in pool.map(replacement, jobs):
            total += size
            note = f" PSNR {measured:.2f} dB" if measured is not None else ""
            print(f"{out}: {width}x{height} BC7 {levels} levels, {size} bytes{note}")
    print(f"{len(jobs)} files, {total / 1048576:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
