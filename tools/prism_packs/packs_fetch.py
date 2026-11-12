"""The upscalers the texture packs use, fetched on the player's request (prism_packs.py models) from where
their authors publish them, each checked against the SHA-256 it had when the pack tools were written, with
its licence shown first. Nothing here is in the repository or in a download of the game, and nothing is
fetched without the player saying yes.

  <upscalers>/realesrgan/   realesrgan-ncnn-vulkan (MIT) and its models (BSD-3-Clause), from Real-ESRGAN's
                            release v0.2.5.0
  <upscalers>/weights/      PyTorch weights: Real-ESRGAN's general models (BSD-3-Clause) and Philip Hofmann's
                            4xNomos8kSC (CC-BY-4.0, credit required)
  <upscalers>/LICENSES.txt  what was fetched, from where, under which licence (written as it is fetched)
"""

import hashlib
import os
import shutil
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path

REAL_ESRGAN = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
LICENCES = {
    "BSD-3-Clause": "https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE",
    "MIT": "https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/blob/master/LICENSE",
    "CC-BY-4.0": "https://creativecommons.org/licenses/by/4.0/",
}

# name: what it is, where it comes from, its SHA-256, its licence, the credit the licence asks for, and where
# it goes in the upscalers' folder (a zip is unpacked into that folder)
DOWNLOADS = {
    "realesrgan-ncnn-vulkan": {
        "what": "Real-ESRGAN's Vulkan upscaler program with its x4plus, x4plus-anime and animevideov3 models "
                "(the base maps' upscaler, and three of the texture tiers' five)",
        "linux": (REAL_ESRGAN + "realesrgan-ncnn-vulkan-20220424-ubuntu.zip",
                  "e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96"),
        "windows": (REAL_ESRGAN + "realesrgan-ncnn-vulkan-20220424-windows.zip",
                    "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d"),
        "licence": "the program MIT (Real-ESRGAN-ncnn-vulkan, Xintao Wang), the models BSD-3-Clause (Real-ESRGAN, "
                   "Xintao Wang)",
        "licences": ["MIT", "BSD-3-Clause"],
        "credit": "Real-ESRGAN by Xintao Wang, Liangbin Xie, Chao Dong and Ying Shan (Tencent ARC Lab)",
        "into": "realesrgan",
    },
    "realesr-general-x4v3": {
        "what": "Real-ESRGAN's general model (one of the texture tiers' five upscalers)",
        "any": (REAL_ESRGAN + "realesr-general-x4v3.pth",
                "8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292"),
        "licence": "BSD-3-Clause (Real-ESRGAN, Xintao Wang)",
        "licences": ["BSD-3-Clause"],
        "credit": "Real-ESRGAN by Xintao Wang et al. (Tencent ARC Lab)",
        "into": "weights/realesr-general-x4v3.pth",
    },
    "realesr-general-wdn-x4v3": {
        "what": "Real-ESRGAN's general model, denoising (the texture tiers' gentle upscaler, and the prototype "
                "maps' of the Chief and the Assault Rifle)",
        "any": (REAL_ESRGAN + "realesr-general-wdn-x4v3.pth",
                "1641f8c4464b9f097c9fdda5589273713f67cf59f3d909e0bd688f0cee269dca"),
        "licence": "BSD-3-Clause (Real-ESRGAN, Xintao Wang)",
        "licences": ["BSD-3-Clause"],
        "credit": "Real-ESRGAN by Xintao Wang et al. (Tencent ARC Lab)",
        "into": "weights/realesr-general-wdn-x4v3.pth",
    },
    "4xNomos8kSC": {
        "what": "Philip Hofmann's photo upscaler (the texture tiers' upscaler for fabric and mottled paint)",
        "any": ("https://github.com/Phhofm/models/raw/main/4xNomos8kSC/4xNomos8kSC.pth",
                "07a7ccd6443d012d528999cd77e40c7d265aaf1f656c499b37936e49603a0b1d"),
        "licence": "CC-BY-4.0 (Philip Hofmann; trained on the Nomos8k_sfw photo set): use is free with credit",
        "licences": ["CC-BY-4.0"],
        "credit": "4xNomos8kSC by Philip Hofmann (Phhofm), CC-BY-4.0, https://github.com/Phhofm/models",
        "into": "weights/4xNomos8kSC.pth",
    },
}
# which packs need which (prism_packs.py checks these before a step runs)
NEEDED_BY = {
    "base_maps": ["realesrgan-ncnn-vulkan"],
    "textures": ["realesrgan-ncnn-vulkan", "realesr-general-x4v3", "realesr-general-wdn-x4v3", "4xNomos8kSC"],
}


def platform():
    return "windows" if os.name == "nt" else "linux"


def source(name):
    entry = DOWNLOADS[name]
    return entry.get("any") or entry[platform()]


def target(folder, name):
    return Path(folder) / DOWNLOADS[name]["into"]


def present(folder, name):
    """whether a download is in place (a zip's folder: its program)"""
    path = target(folder, name)
    if name == "realesrgan-ncnn-vulkan":
        return (path / ("realesrgan-ncnn-vulkan" + (".exe" if os.name == "nt" else ""))).is_file()
    return path.is_file()


def describe(name):
    entry = DOWNLOADS[name]
    url, digest = source(name)
    return "\n".join([f"  {name}: {entry['what']}",
                      f"    from     {url}",
                      f"    sha256   {digest}",
                      f"    licence  {entry['licence']}",
                      *[f"             {LICENCES[licence]}" for licence in entry["licences"]],
                      f"    credit   {entry['credit']}"])


def _download(url, digest, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    sha = hashlib.sha256()
    request = urllib.request.Request(url, headers={"User-Agent": "prism-packs"})
    with urllib.request.urlopen(request, timeout=60) as response, open(partial, "wb") as out:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            sha.update(block)
            done += len(block)
            if total:
                print(f"\r    {done >> 20} of {total >> 20} MB", end="", flush=True)
    print()
    if sha.hexdigest() != digest:
        partial.unlink()
        raise RuntimeError(f"{url}: its SHA-256 is {sha.hexdigest()}, not {digest}: not used")
    partial.replace(destination)


def fetch(folder, name):
    """one download into the upscalers' folder (a zip unpacked), its licence recorded"""
    folder = Path(folder)
    url, digest = source(name)
    into = target(folder, name)
    if url.endswith(".zip"):
        archive = folder / "downloads" / url.rsplit("/", 1)[1]
        if not archive.is_file():
            _download(url, digest, archive)
        if into.exists():
            shutil.rmtree(into)
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                # (only the program, its DLLs and the models: the demo pictures and video stay in the zip)
                if not (info.filename.startswith("models/") or info.filename.startswith("realesrgan-ncnn-vulkan")
                        or info.filename.endswith(".dll")) or info.is_dir():
                    continue
                out = into / info.filename
                if not out.resolve().is_relative_to(into.resolve()):
                    raise RuntimeError(f"{archive}: {info.filename} is outside the folder")
                out.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
        program = into / ("realesrgan-ncnn-vulkan" + (".exe" if os.name == "nt" else ""))
        program.chmod(program.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    else:
        _download(url, digest, into)
    entry = DOWNLOADS[name]
    with open(folder / "LICENSES.txt", "a", encoding="utf-8") as record:
        record.write(f"{name}\n  from {url}\n  sha256 {digest}\n  licence {entry['licence']}\n"
                     + "".join(f"  {LICENCES[licence]}\n" for licence in entry["licences"])
                     + f"  credit {entry['credit']}\n\n")


def fetch_all(folder, names, accept=False, ask=input):
    """the named downloads the folder lacks, each after its licence is shown and the player says yes"""
    missing = [name for name in names if not present(folder, name)]
    if not missing:
        print(f"upscalers: all in {folder}")
        return True
    print("The texture packs use these upscalers. None of them is part of the game or of its source; each is "
          "fetched from its authors' own page, if you say yes, into", folder)
    ok = True
    for name in missing:
        print()
        print(describe(name))
        if not accept:
            answer = ask(f"  fetch {name} under this licence? [y/N] ").strip().lower()
            if answer not in ("y", "yes"):
                print(f"  {name}: not fetched (the packs that need it will say so)")
                ok = False
                continue
        print(f"  fetching {name}")
        try:
            fetch(folder, name)
        except (OSError, RuntimeError) as error:
            print(f"  {name}: not fetched: {error}", file=sys.stderr)
            ok = False
    return ok
