#!/usr/bin/env python3
"""PRISM M4, first-time setup: upscaled colour (base) maps for the characters and the first-person
weapons, built from the player's own maps into a local-only mod (never shipped: it holds CE pixels).

    python -I art/build_base_local.py --maps <data>/maps --work <work>/base_maps --mod <mod folder>
        [--only <text>] [--reuse] [--gpu auto]

(prism_packs.py's base_maps step runs it, with the folders of art/packs_env.py set.) What the tools carry
is this script, base_local_recipe.json (tag names only), upscale_base.py and the tools it calls. Steps:
  1. extract: the extractor (map_extract) writes each listed bitmap of every map that has it, as PNGs
     plus its JSON (uses, formats). A tag is taken only if its pixels are the same on every map that
     has it, and if every use of it is a model shader's base map (soso.base_map) or a transparent
     shader's map (the same image): a mod bitmap replaces the tag on every map.
  2. upscale: upscale_base.py per bitmap (Real-ESRGAN x4plus, BSD-3; 4x for a side <= 256, else 2x;
     back-projected onto CE's pixels), with its colour gate (CIEDE2000 mean <= 3, p95 <= 6,
     p99 <= 8, hue <= 4 deg). A bitmap that fails leaves its tag out.
  3. tags: invader-bitmap from the result (a colour plate for a tag of several bitmaps), in the
     map's own format (DXT1 or DXT3) and usage; mod_bitmap_check.py against each map's own tag
     (invader-extract), as mod_bitmaps.c checks it.
  4. mod: <mod>/mod.toml and tags/..., manifest.json with every gate's numbers; review sheets
     (<work>/review/<name>.png: CE nearest | Lanczos | model alone | result, at the window the model
     changed most, 2x) for the 1:1 check for invented detail.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import packs_env as E  # noqa: E402

ROOT = E.WORK
PY = sys.executable
RECIPE = os.path.join(HERE, "base_local_recipe.json")
OK_FIELDS = ("soso.base_map", "schi.transparent.maps.map", "sotr.transparent.maps.map")
FMT = {"dxt1": "dxt1", "dxt3": "dxt3", "dxt5": "dxt5"}
BLUE = (0, 0, 255)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, env=E.invader_env(), **kw)
    if r.returncode != 0:
        raise RuntimeError("%s failed: %s" % (os.path.basename(cmd[0]), (r.stdout + r.stderr)[-1500:]))
    return r.stdout


def slug(tag):
    return tag.split("\\")[-1].replace(" ", "_")


def extract(a, tags):
    """every map's copy of each tag: {tag: [(map, json dict, [png paths])]}"""
    src = os.path.join(a.work, "src")
    found = {t: [] for t in tags}
    for m in sorted(glob.glob(os.path.join(a.maps, "*.map"))):
        name = os.path.basename(m)[:-4]
        if name == "ui":
            continue
        out = os.path.join(src, name)
        if not (a.reuse and os.path.isdir(out)):
            cmd = ["--data", os.path.dirname(os.path.abspath(a.maps)), "--out", out, "--only", "bitmaps"]
            for t in tags:
                cmd += ["--tag", t]
            run(E.extractor_command(*cmd, m))
        for t in tags:
            j = os.path.join(out, name, "bitmaps", *t.split("\\")) + ".json"
            if os.path.isfile(j):
                d = json.load(open(j))
                if d.get("tag") != t:
                    continue
                pngs = [os.path.join(out, name, p) for b in d["bitmaps"] for p in b["png"]]
                found[t].append((name, d, pngs))
    return found


def vet(t, copies):
    """why a tag can't be replaced, or None"""
    if not copies:
        return "on none of the maps"
    hashes = set()
    for _, d, pngs in copies:
        h = hashlib.sha1()
        for p in pngs:
            h.update(np.asarray(Image.open(p).convert("RGBA")).tobytes())
        hashes.add(h.hexdigest())
        for u in d["used_as"]:
            if u["role"] != "predicted_resource" and u["field"] not in OK_FIELDS:
                return "used as %s by %s" % (u["field"], u["by"])
    if len(hashes) != 1:
        return "pixels differ between maps (%d versions)" % len(hashes)
    d = copies[0][1]
    if d["type"] != "2d_textures" or any(b["type"] != "2d" for b in d["bitmaps"]):
        return "not 2D textures"
    if len(d["sequences"]) != 1:
        return "%d sequences" % len(d["sequences"])
    if any(b["format"] not in FMT for b in d["bitmaps"]) or len({b["format"] for b in d["bitmaps"]}) != 1:
        return "format %s" % [b["format"] for b in d["bitmaps"]]
    return None


def plate(images):
    """a colour plate of one sequence: the images in a row on blue, a pixel of blue around each"""
    w = sum(i.width for i in images) + len(images) + 1
    h = max(i.height for i in images) + 2
    p = Image.new("RGBA", (w, h), BLUE + (255,))
    x = 1
    for i in images:
        rgb = np.asarray(i).copy()
        blue = np.all(rgb[..., :3] == BLUE, -1)
        rgb[blue, 2] = 254  # (pure blue is the plate's background)
        p.paste(Image.fromarray(rgb), (x, 1))
        x += i.width + 1
    return p


def review_sheet(src_png, raw_png, out_png, sheet, k):
    s = Image.open(src_png).convert("RGB")
    raw = Image.open(raw_png).convert("RGB")
    out = Image.open(out_png).convert("RGB")
    w, h = s.size
    lz = s.resize((k * w, k * h), Image.LANCZOS)
    y = lambda im: np.asarray(im, np.float64) @ [0.299, 0.587, 0.114]
    d = np.abs(y(out) - y(lz))
    win = min(64, w, h)  # CE pixels a side
    W = win * k
    # the window where the result differs most from interpolation
    ii = np.cumsum(np.cumsum(np.pad(d, ((1, 0), (1, 0))), 0), 1)
    sums = ii[W:, W:] - ii[:-W, W:] - ii[W:, :-W] + ii[:-W, :-W]
    step = max(1, k * 4)
    sub = sums[::step, ::step]
    yy, xx = np.unravel_index(np.argmax(sub), sub.shape)
    x0, y0 = int(xx * step), int(yy * step)
    box = (x0, y0, x0 + W, y0 + W)
    sbox = tuple(v // k for v in box)
    z = 2 if W <= 256 else 1
    tiles = [s.crop(sbox).resize((W, W), Image.NEAREST), lz.crop(box), raw.crop(box), out.crop(box)]
    tiles = [t.resize((W * z, W * z), Image.NEAREST) for t in tiles]
    gap = 6
    o = Image.new("RGB", (4 * W * z + 3 * gap, W * z), (40, 40, 40))
    for i, t in enumerate(tiles):
        o.paste(t, (i * (W * z + gap), 0))
    o.save(sheet)
    return {"window_out_px": list(box), "window_ce_px": list(sbox), "zoom": z,
            "panels": ["CE nearest", "Lanczos", "x4plus alone", "result"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", default=os.path.join(E.DATA, "maps"))
    ap.add_argument("--work", default=E.work("base_maps") if E.WORK else "")
    ap.add_argument("--mod", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--reuse", action="store_true", help="keep extractions and upscales already made")
    ap.add_argument("--gpu", default=E.VULKAN_GPU or "auto")
    a = ap.parse_args()
    recipe = json.load(open(RECIPE))
    tags = [(g, t) for g, ts in recipe["groups"].items() for t in ts if a.only in t]
    rejected = recipe.get("rejected", {})
    os.makedirs(a.work, exist_ok=True)
    os.environ["TMPDIR"] = os.path.join(a.work, "tmp")
    os.makedirs(os.environ["TMPDIR"], exist_ok=True)

    print("== 1/4 extract from", a.maps, flush=True)
    found = extract(a, [t for _, t in tags])
    up_dir, data_dir, tags_dir = (os.path.join(a.work, d) for d in ("up", "data", "tags"))
    review_dir = os.path.join(a.work, "review")
    shutil.rmtree(data_dir, ignore_errors=True)
    shutil.rmtree(tags_dir, ignore_errors=True)
    for d in (up_dir, review_dir, tags_dir):
        os.makedirs(d, exist_ok=True)
    manifest = {"recipe": os.path.basename(RECIPE), "model": recipe["model"], "entries": []}

    print("== 2/4 upscale", flush=True)
    for group, t in tags:
        e = {"tag": t, "group": group, "maps": [m for m, _, _ in found[t]]}
        manifest["entries"].append(e)
        why = rejected.get(t) or vet(t, found[t])
        if why:
            e["status"] = "left out: " + why
            print(" ", t, "->", e["status"], flush=True)
            continue
        _, d, pngs = found[t][0]
        e.update(format=d["bitmaps"][0]["format"], usage=d["usage"], bitmaps=[])
        for i, p in enumerate(pngs):
            base = "%s_%s%s" % (group, slug(t), "_%d" % i if len(pngs) > 1 else "")
            out = os.path.join(up_dir, base + ".png")
            raw = os.path.join(up_dir, base + "_raw.png")
            rep_path = os.path.join(up_dir, base + ".json")
            mw = float(recipe.get("overrides", {}).get(t, {}).get("model_weight", 1.0))
            if not (a.reuse and os.path.isfile(rep_path) and os.path.isfile(raw)
                    and json.load(open(rep_path)).get("model_weight", 1.0) == mw):
                run([PY, "-B", "-I", os.path.join(HERE, "upscale_base.py"), "--in", p, "--out", out,
                     "--raw-out", raw, "--model", recipe["model"], "--backproject", str(recipe["backproject"]), "--model-weight", str(mw),
                     "--gpu", a.gpu])
            rep = json.load(open(rep_path))
            rep["review"] = os.path.relpath(os.path.join(review_dir, base + ".png"), ROOT)
            rep.update(review_sheet(p, raw, out, os.path.join(review_dir, base + ".png"), rep["scale"]))
            rep["ce_png"] = os.path.relpath(p, ROOT)
            e["bitmaps"].append(rep)
            print("  %-60s %dx w%.2f -> %s dE %.2f/%.2f/%.2f hue %.2f %s" % (
                t + ("[%d]" % i if len(pngs) > 1 else ""), rep["scale"], mw, rep["size_out"], rep["de2000_mean"],
                rep["de2000_p95"], rep["de2000_p99"], rep["hue_shift_mean_deg_chromatic"],
                "pass" if rep["pass"] else "FAIL"), flush=True)
        e["status"] = "upscaled" if all(b["pass"] for b in e["bitmaps"]) else "left out: colour gate"

    print("== 3/4 tags (invader-bitmap) and checks against every map's tag", flush=True)
    src_tags = os.path.join(a.work, "tags_src")
    for e in manifest["entries"]:
        if e["status"] != "upscaled":
            continue
        t, rel = e["tag"], e["tag"].replace("\\", "/")
        os.makedirs(os.path.dirname(os.path.join(data_dir, rel)), exist_ok=True)
        ims = [Image.open(os.path.join(ROOT, b["output"])).convert("RGBA") for b in e["bitmaps"]]
        (plate(ims) if len(ims) > 1 else ims[0]).save(os.path.join(data_dir, rel + ".png"))
        run([E.invader("invader-bitmap"), "-d", data_dir, "-t", tags_dir, "-T", "2d_textures",
             "-F", FMT[e["format"]], "-u", e["usage"], rel])
        e["checks"] = {}
        for m in e["maps"]:
            ref_root = os.path.join(src_tags, m)
            ref = os.path.join(ref_root, rel + ".bitmap")
            if not os.path.isfile(ref):
                os.makedirs(ref_root, exist_ok=True)
                run([E.invader("invader-extract"), "-m", a.maps, "-t", ref_root, "-s", rel + ".bitmap",
                     os.path.join(a.maps, m + ".map")])
            r = subprocess.run([PY, "-B", "-I", os.path.join(HERE, "mod_bitmap_check.py"),
                                os.path.join(tags_dir, rel + ".bitmap"), ref], capture_output=True, text=True)
            e["checks"][m] = "pass" if r.returncode == 0 else "FAIL " + r.stdout[-300:]
        bad = [m for m, v in e["checks"].items() if v != "pass"]
        e["tag_bytes"] = os.path.getsize(os.path.join(tags_dir, rel + ".bitmap"))
        e["status"] = "in mod" if not bad else "left out: tag check failed on " + ",".join(bad)
        print("  %-60s %s (%d bytes, %d maps)" % (t, e["status"], e["tag_bytes"], len(e["maps"])), flush=True)

    print("== 4/4 mod", a.mod, flush=True)
    shutil.rmtree(a.mod, ignore_errors=True)
    os.makedirs(os.path.join(a.mod, "tags"))
    n = 0
    for e in manifest["entries"]:
        if e["status"] != "in mod":
            continue
        rel = e["tag"].replace("\\", "/") + ".bitmap"
        os.makedirs(os.path.dirname(os.path.join(a.mod, "tags", rel)), exist_ok=True)
        shutil.copyfile(os.path.join(tags_dir, rel), os.path.join(a.mod, "tags", rel))
        n += 1
    with open(os.path.join(a.mod, "mod.toml"), "w") as f:
        f.write('name = "PRISM base maps (local)"\nid = "prism_base_local"\nversion = "0.1"\n'
                'description = "The characters\' and first-person weapons\' colour maps at 2x or 4x, upscaled on this '
                'machine from your own maps by the pack tools (base_maps). Local only: never shared or shipped."\n'
                'format = 1\n')
    with open(os.path.join(a.mod, "LOCAL_ONLY.txt"), "w") as f:
        f.write("Built on this machine from the player's own Halo maps (CE pixels). Never commit, share or ship.\n"
                "Rebuild: prism_packs.py build base_maps\n")
    with open(os.path.join(a.work, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("MOD_OK %s (%d bitmap tags); manifest %s" % (a.mod, n, os.path.join(a.work, "manifest.json")))


if __name__ == "__main__":
    sys.exit(main())
