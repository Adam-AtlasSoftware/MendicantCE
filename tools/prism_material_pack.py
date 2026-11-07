#!/usr/bin/env python3
"""PRISM's materials (display.prism_materials "pbr"; docs/mods.md, "Materials"): a material's images
as the texture store reads them (KTX2 files of BC7 blocks with whole chains, tools/texstore_synth.py's
encoder and writer), made from pictures, and the material files that name them
(<mod>/materials/<shader tag path>.material.json; port/linux/game/mod_pbr.c reads them).

  prism_material_pack.py albedo --in albedo.png --out <mod>/textures/materials/x_albedo.ktx2 [--size 4096]
      an RGB picture as the albedo (its colours as the base map's)
  prism_material_pack.py normal --surface surface.png --out x_normal.ktx2 [--size N]
      a surface map's normal (RG: x and y, y up the picture, 0.5 flat) as the normal image
  prism_material_pack.py orm --surface surface.png [--multipurpose multi.png --metal 0.35] --out x_orm.ktx2
      the ORM image: R the surface map's occlusion (B), G its roughness (A), B the metal (the
      multipurpose map's red, CE's specular mask, times --metal; 0 without it)
  prism_material_pack.py detail --out tile.ktx2 [--size 512] [--seed 1]
      a repeating detail tile of this file's own making (fine weave and pits: RG the slopes, B the
      cavity, A the roughness offset, as the material overlay's tiles), no game data
  prism_material_pack.py chief --albedo albedo.png --surface surface.png --multipurpose multi.png
      --out <mod folder>
      the Chief's armour and visor materials (characters\\cyborg\\shaders\\armor and visor): their
      images and material files, local only where the pictures are the game's

The pictures a mod makes from the game's maps stay on the player's machine.
"""

import argparse
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import texstore_synth  # noqa: E402  (the BC7 encoder and the KTX2 writer)

CHUNK_BLOCKS = 1 << 14


def _encode_rows(rgba):
    return texstore_synth.bc7_encode(rgba)


def bc7_level(rgba, pool):
    """A level's BC7 blocks, its rows of blocks encoded in parallel (bands of whole block rows)."""
    height = rgba.shape[0]
    if height < 64 or pool is None:
        return texstore_synth.bc7_encode(rgba)
    band = max(4, (CHUNK_BLOCKS // max(1, rgba.shape[1] // 4)) * 4)
    bands = [rgba[y:y + band] for y in range(0, height, band)]
    return b"".join(pool.map(_encode_rows, bands))


def write_chain(picture, out, size, pool):
    """An RGBA PIL picture at size (a power of two on its longer side) and its whole chain, as BC7."""
    width, height = picture.size
    if size:
        scale = size / max(width, height)
        width, height = max(1, int(round(width * scale))), max(1, int(round(height * scale)))
        picture = picture.resize((width, height), Image.LANCZOS)
    for side in (width, height):
        if side & (side - 1):
            raise SystemExit(f"{out}: {width} x {height} is not a power of two on each side")
    levels = texstore_synth.chain(picture.convert("RGBA"))
    data = [[bc7_level(np.asarray(level, dtype=np.uint8), pool)] for level in levels]
    texstore_synth.ktx2_write(Path(out), width, height, 1, data)
    print(f"{out}: {width} x {height}, {len(levels)} levels, BC7, {os.path.getsize(out)} bytes")


def normal_picture(surface):
    rgba = np.asarray(surface.convert("RGBA"), dtype=np.uint8).copy()
    out = np.zeros_like(rgba)
    out[..., 0] = rgba[..., 0]
    out[..., 1] = rgba[..., 1]
    out[..., 2] = 255
    out[..., 3] = 255
    return Image.fromarray(out, "RGBA")


def orm_picture(surface, multipurpose, metal):
    rgba = np.asarray(surface.convert("RGBA"), dtype=np.float32) / 255.0
    out = np.zeros(rgba.shape, dtype=np.float32)
    out[..., 0] = rgba[..., 2]
    out[..., 1] = rgba[..., 3]
    if multipurpose is not None:
        mask = np.asarray(multipurpose.convert("RGB").resize(surface.size, Image.BILINEAR), dtype=np.float32) / 255.0
        out[..., 2] = np.clip(mask[..., 0] * metal, 0.0, 1.0)
    out[..., 3] = 1.0
    return Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA")


def detail_picture(size, seed):
    """A seamless tile: a fine woven grid's slopes, small pits (cavity) and a mottled roughness."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / size
    threads = 16
    # (a twill weave's height: crossing ridges, each band raised alternately)
    u = np.sin(2 * np.pi * threads * x)
    v = np.sin(2 * np.pi * threads * y)
    over = np.sign(np.sin(np.pi * threads * (x + y)))
    height = 0.5 * (np.where(over > 0, u, v) * 0.6 + 0.4 * u * v)
    # (pits: a few soft wells, wrapped)
    for _ in range(size // 8):
        cx, cy, r = rng.random(), rng.random(), rng.uniform(0.004, 0.012)
        dx = (x - cx + 0.5) % 1.0 - 0.5
        dy = (y - cy + 0.5) % 1.0 - 0.5
        height -= 1.5 * np.exp(-(dx * dx + dy * dy) / (r * r))
    gx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size / (4.0 * threads)
    gy = (np.roll(height, 1, axis=0) - np.roll(height, -1, axis=0)) * size / (4.0 * threads)
    cavity = np.clip(0.5 + 0.25 * (height - height.mean()), 0.0, 1.0)
    noise = np.fft.ifft2(np.fft.fft2(rng.standard_normal((size, size))) *
                         np.exp(-((np.fft.fftfreq(size)[None, :] ** 2 + np.fft.fftfreq(size)[:, None] ** 2) * 400.0))).real
    rough = 0.5 + 0.35 * noise / (np.abs(noise).max() + 1e-6)
    out = np.stack([np.clip(0.5 + 0.5 * gx, 0, 1), np.clip(0.5 + 0.5 * gy, 0, 1), cavity, np.clip(rough, 0, 1)], axis=-1)
    return Image.fromarray((out * 255.0 + 0.5).astype(np.uint8), "RGBA")


def material_file(out, shader, material):
    path = Path(out) / "materials" / (shader.replace("\\", "/") + ".material.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(material, indent=2) + "\n")
    print(f"{path}")


def chief(arguments, pool):
    out = Path(arguments.out)
    images = out / "textures" / "materials" / "cyborg"
    surface = Image.open(arguments.surface)
    write_chain(Image.open(arguments.albedo).convert("RGBA"), images / "armor_albedo.ktx2", arguments.size, pool)
    write_chain(normal_picture(surface), images / "armor_normal.ktx2", arguments.size, pool)
    write_chain(orm_picture(surface, Image.open(arguments.multipurpose) if arguments.multipurpose else None,
                            arguments.metal), images / "armor_orm.ktx2", arguments.size, pool)
    write_chain(detail_picture(512, 1), out / "textures" / "materials" / "tiles" / "weave_detail.ktx2", 512, pool)
    material_file(out, "characters\\cyborg\\shaders\\armor", {
        "format": 1,
        "albedo": "textures/materials/cyborg/armor_albedo.ktx2",
        "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 1.0,
        "orm": "textures/materials/cyborg/armor_orm.ktx2",
        "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
        "detail": {"map": "textures/materials/tiles/weave_detail.ktx2", "scale": 64,
                   "normal": 0.12, "cavity": 0.2, "roughness": 0.2, "mask": "all"},
        "reflection": 1.0, "specular": 0.5, "wrap": True, "direct_specular": 1.0, "ce_reflection": 1.0,
        "multipurpose": {"specular_mask": True, "self_illumination": True, "reflection_tint": True},
    })
    material_file(out, "characters\\cyborg\\shaders\\visor", {
        "format": 1,
        "normal": "textures/materials/cyborg/armor_normal.ktx2", "normal_scale": 0.25,
        "roughness": 0.12, "metal": 1.0,
        "clearcoat": {"strength": 1.0, "roughness": 0.03},
        "reflection": 1.0, "specular": 0.5, "wrap": True, "direct_specular": 1.0, "ce_reflection": 1.0,
        "multipurpose": {"specular_mask": True, "self_illumination": True, "reflection_tint": True},
    })
    (out / "mod.toml").write_text('name = "PRISM materials: the Chief"\nversion = "1"\n')
    (out / "LOCAL_ONLY.txt").write_text("Made from the player's own maps' pictures: not for sharing.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("albedo")
    p.add_argument("--in", dest="source", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--size", type=int, default=0)
    p = sub.add_parser("normal")
    p.add_argument("--surface", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--size", type=int, default=0)
    p = sub.add_parser("orm")
    p.add_argument("--surface", required=True)
    p.add_argument("--multipurpose")
    p.add_argument("--metal", type=float, default=0.35)
    p.add_argument("--out", required=True)
    p.add_argument("--size", type=int, default=0)
    p = sub.add_parser("detail")
    p.add_argument("--out", required=True)
    p.add_argument("--size", type=int, default=512)
    p.add_argument("--seed", type=int, default=1)
    p = sub.add_parser("chief")
    p.add_argument("--albedo", required=True)
    p.add_argument("--surface", required=True)
    p.add_argument("--multipurpose")
    p.add_argument("--metal", type=float, default=0.15)
    p.add_argument("--size", type=int, default=0)
    p.add_argument("--out", required=True)
    p.add_argument("--workers", type=int, default=8)
    arguments = parser.parse_args()
    with ProcessPoolExecutor(max_workers=getattr(arguments, "workers", 8)) as pool:
        if arguments.command == "albedo":
            write_chain(Image.open(arguments.source).convert("RGBA"), arguments.out, arguments.size, pool)
        elif arguments.command == "normal":
            write_chain(normal_picture(Image.open(arguments.surface)), arguments.out, arguments.size, pool)
        elif arguments.command == "orm":
            write_chain(orm_picture(Image.open(arguments.surface),
                                    Image.open(arguments.multipurpose) if arguments.multipurpose else None,
                                    arguments.metal), arguments.out, arguments.size, pool)
        elif arguments.command == "detail":
            write_chain(detail_picture(arguments.size, arguments.seed), arguments.out, arguments.size, pool)
        else:
            chief(arguments, pool)


if __name__ == "__main__":
    main()
