#!/usr/bin/env python3
"""The terrain overhaul's HQ layers (T1; docs/prism_terrain.md, "T1: the HQ layers"): the CC0 material pack the
terrain pass draws, built from public CC0 scans into the game's data folder.

    prism_terrain_layers.py list                                  # the layers, their sources and licences
    prism_terrain_layers.py build --data <data folder> [--size 1024] [--work <folder>] [--yes]
    prism_terrain_layers.py check --data <data folder>            # is the pack there and whole

Every source is a CC0 1.0 scan (manifest.json beside this file: the asset, its page, the archive's URL and SHA-256,
the licence, the date it was checked, and our numbers). Nothing is fetched until the player says yes (--yes, or the
question answered); each archive is checked against its SHA-256 before it is used. The pack is:

    <data>/prism_terrain/layers.ptl     the layers (format below)
    <data>/prism_terrain/manifest.json  this tool's manifest as it was built, with what was made of each source
    <data>/prism_terrain/LICENSES.txt   each source, its licence and its page

What is made of a scan (design section 9.2), per layer, at --size (a power of two; 1024 by default):
- the detail: the albedo in linear light divided by its own low-pass (a Gaussian about an eighth of the tile, on the
  torus: the scans repeat), so that its mean is 1 and it holds only the grain, the pebbles and the cracks. The
  terrain pass multiplies CE's own base colour by it (detail transfer): CE's colour and large shapes stay CE's.
  Stored as half of it (0.5 is 1) in RGB;
- the height (the scan's displacement, its range stretched to 0..1) in A;
- the normal (OpenGL's: y up the picture) x and y in R and G of the second image (0.5 flat);
- the roughness divided by its mean (half of it, 0.5 is 1) in B, the occlusion in A.
A scan whose albedo still holds light (the high-passed luminance following the normal map's shading from some
direction: |correlation| over the manifest's limit) is refused: its shadows would be drawn twice.

The macro map is ours: four octaves of tileable value noise (R 1, G 2, B 4, A 8 cells across), mean 0.5.

layers.ptl, little-endian:
    char magic[4] = "PTL1"; uint32 version = 1, size, layers, macro_size, reserved[3]
    per layer: char name[16]; float tile_metres, strength, chroma, normal, roughness, height_contrast, spare[2]
    the detail images (layers x size x size x RGBA8), the surface images (the same), the macro map
    (macro_size x macro_size x RGBA8); the levels below are the renderer's to make.

The scans are CC0: anything may be done with them, including shipping them in the game; the manifest's credits
are a courtesy to their authors.
"""

import argparse
import datetime
import hashlib
import io
import json
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "manifest.json"
MAGIC = b"PTL1"
VERSION = 1
MACRO_SIZE = 512


def manifest():
    return json.loads(MANIFEST.read_text())


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def resize(picture, size, mode):
    return np.asarray(picture.convert(mode).resize((size, size), Image.LANCZOS), dtype=np.float32) / 255.0


def gaussian_torus(image, sigma):
    """A Gaussian blur of a repeating image (each channel), in the frequency domain."""
    h, w = image.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    kernel = np.exp(-2.0 * (np.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2))
    if image.ndim == 2:
        return np.real(np.fft.ifft2(np.fft.fft2(image) * kernel))
    return np.stack([np.real(np.fft.ifft2(np.fft.fft2(image[..., c]) * kernel)) for c in range(image.shape[2])],
                    axis=-1)


def lit_correlation(luminance, normal):
    """The largest |correlation| of the high-passed luminance with the normal map's Lambert shading from eight
    directions 45 degrees up: a de-lit albedo follows none of them."""
    high = luminance - gaussian_torus(luminance, luminance.shape[0] / 16.0)
    n = normal * 2.0 - 1.0
    nz = np.sqrt(np.clip(1.0 - n[..., 0] ** 2 - n[..., 1] ** 2, 0.0, 1.0))
    best = 0.0
    for k in range(8):
        a = k * np.pi / 4.0
        light = np.array([np.cos(a) * 0.7071, np.sin(a) * 0.7071, 0.7071])
        shade = n[..., 0] * light[0] + n[..., 1] * light[1] + nz * light[2]
        c = np.corrcoef(high.ravel(), shade.ravel())[0, 1]
        best = max(best, abs(float(c)))
    return best


def fetch(source, work, yes):
    """The source's archive, from the work folder or fetched (asked first), checked against its SHA-256."""
    path = work / Path(source["archive"]).name.split("=")[-1]
    if not path.exists():
        if not yes:
            answer = input(f"fetch {source['asset']} ({source['licence']}) from {source['archive']}? [y/N] ")
            if answer.strip().lower() not in ("y", "yes"):
                raise SystemExit("nothing fetched")
        work.mkdir(parents=True, exist_ok=True)
        print(f"fetching {source['archive']}")
        request = urllib.request.Request(source["archive"], headers={"User-Agent": "prism_terrain_layers"})
        with urllib.request.urlopen(request, timeout=300) as response:
            data = response.read()
        partial = path.with_suffix(".part")
        partial.write_bytes(data)
        partial.rename(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != source["sha256"]:
        raise SystemExit(f"{path}: SHA-256 {digest}, not the manifest's {source['sha256']}: refused")
    return path


def member(archive, suffix):
    names = [n for n in archive.namelist() if n.endswith(suffix)]
    if not names:
        return None
    return Image.open(io.BytesIO(archive.read(names[0])))


def make_layer(layer, archive_path, size):
    """A layer's detail and surface images (RGBA8 each) and what was found of it."""
    with zipfile.ZipFile(archive_path) as archive:
        color = member(archive, "_Color.jpg")
        normal = member(archive, "_NormalGL.jpg")
        height = member(archive, "_Displacement.jpg")
        rough = member(archive, "_Roughness.jpg")
        occlusion = member(archive, "_AmbientOcclusion.jpg")
        if color is None or normal is None or height is None or rough is None:
            raise SystemExit(f"{archive_path}: a map is missing (colour, normal, displacement, roughness)")
        c = srgb_to_linear(resize(color, size, "RGB"))
        n = resize(normal, size, "RGB")
        h = resize(height, size, "L")
        r = resize(rough, size, "L")
        o = resize(occlusion, size, "L") if occlusion is not None else np.ones((size, size), np.float32)
    luminance = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    lit = lit_correlation(luminance, n)
    if lit > layer["lit_limit"]:
        raise SystemExit(f"{layer['name']}: the albedo follows the normal map's shading (|r| {lit:.2f} > "
                         f"{layer['lit_limit']}): not de-lit, refused")
    low = gaussian_torus(c, size * layer["lowpass"])
    detail = np.clip(c / np.maximum(low, 1.0e-4), 0.0, 2.0)
    detail /= detail.reshape(-1, 3).mean(axis=0)
    h = (h - h.min()) / max(float(h.max() - h.min()), 1.0e-6)
    r_mean = max(float(r.mean()), 1.0e-3)
    a = np.empty((size, size, 4), np.float32)
    a[..., :3] = np.clip(detail * 0.5, 0.0, 1.0)
    a[..., 3] = h
    b = np.empty((size, size, 4), np.float32)
    b[..., 0] = n[..., 0]
    b[..., 1] = n[..., 1]
    b[..., 2] = np.clip(r / r_mean * 0.5, 0.0, 1.0)
    b[..., 3] = o
    found = {
        "lit_correlation": round(lit, 3),
        "detail_std": [round(float(v), 4) for v in detail.reshape(-1, 3).std(axis=0)],
        "albedo_mean_linear": [round(float(v), 4) for v in c.reshape(-1, 3).mean(axis=0)],
        "roughness_mean": round(r_mean, 4),
    }
    return (np.round(a * 255.0).astype(np.uint8), np.round(b * 255.0).astype(np.uint8), found)


def value_noise(size, cells, seed):
    rng = np.random.default_rng(seed)
    grid = rng.random((cells, cells)).astype(np.float32)
    t = (np.arange(size, dtype=np.float32) + 0.5) / size * cells
    i0 = np.floor(t).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * (3.0 - 2.0 * f)
    rows = grid[i0][:, i0] * (1 - f)[None, :] + grid[i0][:, i1] * f[None, :]
    rows1 = grid[i1][:, i0] * (1 - f)[None, :] + grid[i1][:, i1] * f[None, :]
    return rows * (1 - f)[:, None] + rows1 * f[:, None]


def make_macro(size, seed):
    out = np.empty((size, size, 4), np.float32)
    for channel, cells in enumerate((4, 8, 16, 32)):
        v = value_noise(size, cells, seed + channel) * 0.6 + value_noise(size, cells * 2, seed + 10 + channel) * 0.4
        out[..., channel] = (v - v.mean()) / max(float(v.std()), 1.0e-6) * 0.17 + 0.5
    return np.round(np.clip(out, 0.0, 1.0) * 255.0).astype(np.uint8)


def write_pack(path, size, layers, images, macro):
    with open(path, "wb") as f:
        f.write(MAGIC + struct.pack("<7I", VERSION, size, len(layers), MACRO_SIZE, 0, 0, 0))
        for layer in layers:
            f.write(struct.pack("<16s8f", layer["name"].encode()[:15], layer["tile_metres"], layer["strength"],
                                layer["chroma"], layer["normal"], layer["roughness"], layer["height_contrast"],
                                0.0, 0.0))
        for which in (0, 1):
            for image in images:
                f.write(image[which].tobytes())
        f.write(macro.tobytes())


def licences_text(m, built):
    lines = ["PRISM terrain HQ layers: sources and licences", "",
             "Every scan below is CC0 1.0 Universal (public domain dedication):",
             "https://creativecommons.org/publicdomain/zero/1.0/", "",
             "The macro map and the processing are this tool's own (tools/prism_terrain_layers).", ""]
    for layer in m["layers"]:
        s = layer["source"]
        lines.append(f"{layer['name']}: {s['asset']} by {s['author']} ({s['page']}), {s['licence']}, "
                     f"checked {s['checked']}")
    lines += ["", f"Built {built}."]
    return "\n".join(lines) + "\n"


def build(args):
    m = manifest()
    size = args.size
    if size & (size - 1) or not 256 <= size <= 4096:
        raise SystemExit("--size is a power of two from 256 to 4096")
    data = Path(args.data)
    if not data.is_dir():
        raise SystemExit(f"{data}: no such data folder")
    work = Path(args.work) if args.work else data / "prism_terrain" / "work"
    images, made = [], {}
    for layer in m["layers"]:
        archive = fetch(layer["source"], work, args.yes)
        a, b, found = make_layer(layer, archive, size)
        images.append((a, b))
        made[layer["name"]] = found
        print(f"{layer['name']}: {layer['source']['asset']}, |r| {found['lit_correlation']}, detail std "
              f"{found['detail_std']}")
    macro = make_macro(MACRO_SIZE, m.get("macro_seed", 1))
    out = data / "prism_terrain"
    out.mkdir(parents=True, exist_ok=True)
    partial = out / "layers.ptl.part"
    write_pack(partial, size, m["layers"], images, macro)
    partial.replace(out / "layers.ptl")
    built = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    record = dict(m, built=built, size=size, made=made)
    (out / "manifest.json").write_text(json.dumps(record, indent=1) + "\n")
    (out / "LICENSES.txt").write_text(licences_text(m, built))
    print(f"{out / 'layers.ptl'}: {len(m['layers'])} layers at {size}, {(out / 'layers.ptl').stat().st_size} bytes")


def check(args):
    path = Path(args.data) / "prism_terrain" / "layers.ptl"
    if not path.exists():
        raise SystemExit(f"{path}: not built (prism_terrain_layers.py build --data {args.data})")
    with open(path, "rb") as f:
        head = f.read(32)
    magic, (version, size, layers, macro) = head[:4], struct.unpack("<4I", head[4:20])
    expected = 32 + layers * 48 + 2 * layers * size * size * 4 + macro * macro * 4
    if magic != MAGIC or version != VERSION or path.stat().st_size != expected:
        raise SystemExit(f"{path}: not a whole pack (magic {magic!r}, version {version}, "
                         f"{path.stat().st_size} bytes, {expected} expected)")
    print(f"{path}: {layers} layers at {size}, whole")


def list_layers(args):
    for layer in manifest()["layers"]:
        s = layer["source"]
        print(f"{layer['name']:6} {s['asset']:10} {s['licence']:8} {layer['tile_metres']} m  {s['page']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("--data", required=True)
    p.add_argument("--size", type=int, default=1024)
    p.add_argument("--work")
    p.add_argument("--yes", action="store_true", help="fetch the manifest's CC0 sources without asking")
    p.set_defaults(run=build)
    p = sub.add_parser("check")
    p.add_argument("--data", required=True)
    p.set_defaults(run=check)
    p = sub.add_parser("list")
    p.set_defaults(run=list_layers)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    sys.exit(main())
