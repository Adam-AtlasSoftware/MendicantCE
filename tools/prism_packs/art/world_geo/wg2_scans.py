#!/usr/bin/env python3
"""wg2_scans.py: the CC0 scans whose height maps give the natural remodel's rock its relief (wg2_scans.json).

  wg2_scans.py fetch --work <folder> [--yes] [names...]   each source fetched into the folder (asked first unless
                                                          --yes) and checked against its SHA-256
  wg2_scans.py prepare --work <folder> --out <folder>     each height map as <name>.npy (float32, 512 by 512, mean 0,
                                                          deviation 1, rows down the image as the scan was shot: up
                                                          the rock is up the rows) and <name>.json (its source, licence
                                                          and SHA-256); the builder reads them from WG_SCANS
  wg2_scans.py list

A height map is read as data only (Pillow's decoders); nothing in a download is run. The prepared files hold no game
data, but stay beside the remodel they made (local)."""
import argparse
import hashlib
import io
import json
import os
import sys
import urllib.request
import zipfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SIZE = 512


def manifest():
    return json.loads((HERE / "wg2_scans.json").read_text())


def source_file(name, scan, work):
    h = scan["height"]
    leaf = h["url"].rsplit("/", 1)[-1].split("=")[-1]
    return Path(work) / f"{name}__{leaf}"


def fetch(names, work, yes):
    m = manifest()["scans"]
    work = Path(work)
    for name in names or sorted(m):
        scan = m[name]
        path = source_file(name, scan, work)
        if not path.exists():
            if not yes:
                answer = input(f"fetch {scan['asset']} ({scan['licence']}) from {scan['height']['url']}? [y/N] ")
                if answer.strip().lower() not in ("y", "yes"):
                    raise SystemExit("nothing fetched")
            work.mkdir(parents=True, exist_ok=True)
            req = urllib.request.Request(scan["height"]["url"], headers={"User-Agent": "prism_world_scans"})
            with urllib.request.urlopen(req, timeout=300) as r:
                data = r.read()
            part = path.with_suffix(".part")
            part.write_bytes(data)
            part.rename(path)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != scan["height"]["sha256"]:
            raise SystemExit(f"{path}: SHA-256 {digest}, not the manifest's {scan['height']['sha256']}: refused")
        print(f"{name}: {path.name} ok")


def read_height(path, member=None):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = 64 * 1024 * 1024
    data = Path(path).read_bytes()
    if member:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.endswith(member)]
            if not names:
                raise SystemExit(f"{path}: no member ending {member}")
            data = z.read(names[0])
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode in ("I;16", "I;16B", "I;16L", "I"):
        a = np.asarray(im, dtype=np.float64)
    else:
        a = np.asarray(im.convert("F"), dtype=np.float64)
    if a.ndim == 3:
        a = a[..., 0]
    return a


def box_down(a, size):
    """area-average down to size by size (a square power-of-two source)"""
    h, w = a.shape
    if h != w:
        raise SystemExit("a height map is square")
    while a.shape[0] > size:
        a = 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])
    return a


def prepare(work, out):
    m = manifest()["scans"]
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for name, scan in sorted(m.items()):
        path = source_file(name, scan, work)
        if not path.exists():
            print(f"{name}: not fetched ({path.name}), skipped")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != scan["height"]["sha256"]:
            raise SystemExit(f"{path}: SHA-256 {digest}, not the manifest's: refused")
        a = box_down(read_height(path, scan["height"].get("member")), SIZE)
        a = (a - a.mean()) / max(float(a.std()), 1e-9)
        np.save(out / f"{name}.npy", a.astype(np.float32))
        meta = {k: scan[k] for k in ("asset", "source", "authors", "page", "licence", "size_m")}
        meta.update(name=name, url=scan["height"]["url"], sha256=digest, size=SIZE,
                    npy_sha256=hashlib.sha256((out / f"{name}.npy").read_bytes()).hexdigest())
        (out / f"{name}.json").write_text(json.dumps(meta, indent=1) + "\n")
        print(f"{name}: prepared {SIZE}x{SIZE}")


def load(name, folder=None):
    """the prepared height field (float32 (512, 512)) and its record; refused when its source is not the manifest's"""
    folder = Path(folder or os.environ.get("WG_SCANS", ""))
    scan = manifest()["scans"].get(name)
    if scan is None:
        raise SystemExit(f"scan {name}: not in wg2_scans.json")
    meta_p, npy_p = folder / f"{name}.json", folder / f"{name}.npy"
    if not meta_p.exists() or not npy_p.exists():
        raise SystemExit(f"scan {name}: not prepared in {folder} (WG_SCANS; wg2_scans.py fetch and prepare)")
    meta = json.loads(meta_p.read_text())
    if meta.get("sha256") != scan["height"]["sha256"]:
        raise SystemExit(f"scan {name}: prepared from another source than the manifest's: refused")
    if hashlib.sha256(npy_p.read_bytes()).hexdigest() != meta.get("npy_sha256"):
        raise SystemExit(f"scan {name}: {npy_p} changed since it was prepared: refused")
    return np.load(npy_p, allow_pickle=False), meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--work", required=True)
    f.add_argument("--yes", action="store_true")
    f.add_argument("names", nargs="*")
    p = sub.add_parser("prepare")
    p.add_argument("--work", required=True)
    p.add_argument("--out", required=True)
    sub.add_parser("list")
    a = ap.parse_args()
    if a.cmd == "fetch":
        fetch(a.names, a.work, a.yes)
    elif a.cmd == "prepare":
        prepare(a.work, a.out)
    else:
        for name, s in sorted(manifest()["scans"].items()):
            print(f"{name}: {s['asset']} ({s['source']}, {s['licence']}), {s['size_m']} m, {s['page']}")


if __name__ == "__main__":
    sys.exit(main())
