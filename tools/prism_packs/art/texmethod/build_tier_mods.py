"""Write the PRISM v2 texture tiers (tiers.py outputs) as Invader bitmap tags, check them against every map's own
tag (mod_bitmaps.c's rules, mod_bitmap_check.py), check the sizes against the texture cache's limits, and lay
out the two local mods:

  <work>/textures/mods/prism_v2_tex_enhanced   fusion, 2x-4x, at most 2048
  <work>/textures/mods/prism_v2_tex_max        fusion + detail2 where it passes the invented-texture check, at
                                             most 4096, else the Enhanced map

  python -I build_tier_mods.py [--skip-check]

Formats: base maps DXT1 (DXT5 where CE's has explicit alpha); multipurpose maps reordered to Halo PC's channel
order (R auxiliary, G self-illumination, B specular, A colour change: custom_edition_bitmaps.c reads a mod's
multipurpose map so) and DXT5. Tags with several bitmaps (the Grunts') are written from a colour plate (blue
background). Colour-plate key colours (pure blue, cyan, magenta) are nudged by one level.
Everything here is built from CE pixels: local only, never committed, shared or shipped.
"""
import glob
import json
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tier_recipe as R  # noqa: E402

E = R.E
INV = E.INVADER
MAPS = E.maps()
OUTS = E.work("textures", "out", "tiers")
W = E.work("textures", "tiers")
MODS = E.work("textures", "mods")  # (Enhanced and Max: the steps' own, which Bold and the Chief start from)
CHECK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mod_bitmap_check.py")
PY = sys.executable
ENV = dict(E.invader_env(), TMPDIR=W + "/tmp")
CACHE_MB = 256          # the texture cache on the desktop builds (halo_port_capacity.h)
PER_BITMAP_MB = 64      # a quarter of it (texture_cache_mod_bitmap_size_limit)
UPLOAD_MB = 32          # one image upload over this is skipped (xgpu_vulkan.c)
TIERS = {"enh": ("prism_v2_tex_enhanced", "PRISM v2 textures: Enhanced (local)",
                 "First-person weapons and arms, the Chief, Marines (with faces), Elites, Grunts and Jackals: base and multipurpose maps from the upscaler fusion at 2x-4x (at most 2048). CE's colours and designs. Built on this machine from your own maps: never shared or shipped."),
         "max": ("prism_v2_tex_max", "PRISM v2 textures: Max (local)",
                 "The Enhanced set with B2's detail stage at 8x (at most 4096) on every map where it passes the invented-texture check; the others are the Enhanced maps. Built on this machine from your own maps: never shared or shipped.")}


def u8(a):
    return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)


def nudge(px):
    """colour-plate key colours (Invader: blue background, magenta sequence divider, cyan dummy space)"""
    rgb = px[..., :3]
    for key in ((0, 0, 255), (255, 0, 255), (0, 255, 255)):
        m = (rgb[..., 0] == key[0]) & (rgb[..., 1] == key[1]) & (rgb[..., 2] == key[2])
        rgb[m, 2] = 254
    return px


def to_pc(xbox):
    """Xbox order (R spec, G self-illum, B colour change, A aux) -> Halo PC order (R aux, G self-illum, B spec, A cc)"""
    return np.stack([xbox[..., 3], xbox[..., 1], xbox[..., 0], xbox[..., 2]], -1)


def plate(images):
    """several bitmaps of one sequence in one colour plate: blue background, a one-pixel blue border each"""
    H = max(i.shape[0] for i in images) + 2
    Wd = sum(i.shape[1] + 1 for i in images) + 1
    P = np.zeros((H, Wd, images[0].shape[2]), np.uint8)
    P[..., :3] = (0, 0, 255)
    if P.shape[2] == 4:
        P[..., 3] = 255
    x = 1
    for i in images:
        P[1:1 + i.shape[0], x:x + i.shape[1]] = i
        x += i.shape[1] + 1
    return P


def keys_of(tag):
    import tier_cache as TC
    return [k for k, _ in TC.images(tag)]


def write_data(tier):
    """PNG sources for invader-bitmap: {tag: format}"""
    d = os.path.join(W, tier, "data")
    shutil.rmtree(d, ignore_errors=True)
    fmts = {}
    for tag, r in R.BASES.items():
        ks = keys_of(tag)
        bases, alpha = [], False
        for k in ks:
            rgb = np.asarray(Image.open(os.path.join(OUTS, k, tier + ".png")).convert("RGB"))
            ap = os.path.join(OUTS, k, "alpha_%s.png" % tier)
            if os.path.exists(ap):
                alpha = True
                a = np.asarray(Image.open(ap).convert("L"))
                rgb = np.dstack([rgb, a])
            else:
                rgb = np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)])
            bases.append(nudge(rgb.copy()))
        img = bases[0] if len(bases) == 1 else plate(bases)
        p = os.path.join(d, tag + ".png")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        Image.fromarray(img, "RGBA").save(p)
        fmts[tag] = "dxt5" if alpha else "dxt1"
        if r["multi"] and r["multi"] not in fmts:
            mks = keys_of(r["multi"])
            ms = []
            for k in ks[:len(mks)]:
                x = np.asarray(Image.open(os.path.join(OUTS, k, "multi_%s.png" % tier)).convert("RGBA"))
                ms.append(nudge(to_pc(x).copy()))
            img = ms[0] if len(ms) == 1 else plate(ms)
            p = os.path.join(d, r["multi"] + ".png")
            os.makedirs(os.path.dirname(p), exist_ok=True)
            Image.fromarray(img, "RGBA").save(p)
            fmts[r["multi"]] = "dxt5"
    return fmts


def invader(tier, fmts):
    d, t = os.path.join(W, tier, "data"), os.path.join(W, tier, "tags")
    shutil.rmtree(t, ignore_errors=True)
    os.makedirs(t)
    for tag, f in fmts.items():
        r = subprocess.run([E.invader("invader-bitmap"), "-d", d, "-t", t, "-T", "2d_textures", "-F", f, "-u", "default", tag],
                           env=ENV, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("invader-bitmap %s: %s" % (tag, r.stdout + r.stderr))


def read_tag(path):
    sys.path.insert(0, os.path.dirname(CHECK))
    import mod_bitmap_check as MBC
    return MBC.read(path)


def pixels_md5(path):
    """md5 of a tag file's processed pixel data (is the map's bitmap the one the tiers were built from?)"""
    import hashlib
    b = open(path, "rb").read()
    g = read_tag(path)
    off = 0x40 + 0x6C + g["color_plate_bytes"]
    return hashlib.md5(b[off:off + g["pixel_bytes"]]).hexdigest()


def checks(tier, fmts):
    """every map that has the tag: extract the map's tag once, run mod_bitmap_check"""
    t = os.path.join(W, tier, "tags")
    log = []
    for tag in fmts:
        for m in sorted(glob.glob(MAPS + "/*.map")):
            n = os.path.basename(m)[:-4]
            if n == "ui":
                continue
            ref = os.path.join(W, "bitmap_check", n, tag + ".bitmap")
            miss = os.path.join(W, "bitmap_check", n, tag + ".absent")
            if not os.path.exists(ref) and not os.path.exists(miss):
                os.makedirs(os.path.dirname(ref), exist_ok=True)
                subprocess.run([E.invader("invader-extract"), "-m", MAPS, "-t", os.path.join(W, "bitmap_check", n), "-s",
                                tag + ".bitmap", m], env=ENV, capture_output=True)
                if not os.path.exists(ref):
                    open(miss, "w").close()
            if not os.path.exists(ref):
                continue
            r = subprocess.run([PY, "-B", "-I", CHECK, os.path.join(t, tag + ".bitmap"), ref], capture_output=True, text=True)
            ok = r.returncode == 0
            errs = [] if ok else json.loads(r.stdout).get("errors", [r.stderr[-300:]])
            log.append({"tag": tag, "map": n, "pass": ok, "errors": errs, "pixels_md5": pixels_md5(ref)})
    return log


def sizes(tier, fmts):
    t = os.path.join(W, tier, "tags")
    out = {}
    for tag in fmts:
        g = read_tag(os.path.join(t, tag + ".bitmap"))
        bms = g["bitmaps"]
        # per bitmap: its share of the pixel data (offsets), the largest one is what the cache limit sees
        offs = [b["pixels_offset"] for b in bms] + [g["pixel_bytes"]]
        per = [offs[i + 1] - offs[i] for i in range(len(bms))]
        out[tag] = {"bitmaps": [[b["width"], b["height"], b["format"], b["mipmaps"]] for b in bms],
                    "pixel_bytes": g["pixel_bytes"], "largest_bitmap_bytes": max(per)}
    return out


def b30_budget(sz_by_tier):
    """b30: every bitmap's pixels as the map has them (the extraction's index), and with each tier's tags"""
    idx = json.load(open(E.extract("b30", "index.json")))
    stock_all, stock_rep = 0, 0
    rep = set()
    for tag in sz_by_tier["enh"]:
        rep.add(tag.replace("/", "\\"))
    per_tag = {}
    for w in idx["written"]:
        if w.get("kind") != "bitmap":
            continue
        j = json.load(open(E.extract("b30", w["json"])))
        s = sum(b["pixels_size"] for b in j["bitmaps"])
        stock_all += s
        if j["tag"] in rep:
            stock_rep += s
            per_tag[j["tag"]] = s
    res = {"b30_all_bitmaps_stock_MB": stock_all / 2 ** 20, "replaced_tags_stock_MB": stock_rep / 2 ** 20,
           "replaced_tags_in_b30": len(per_tag)}
    for tier, sz in sz_by_tier.items():
        mod = sum(v["pixel_bytes"] for k, v in sz.items() if k.replace("/", "\\") in per_tag)
        res[tier] = {"replaced_tags_MB": mod / 2 ** 20,
                     "b30_all_bitmaps_MB": (stock_all - stock_rep + mod) / 2 ** 20,
                     "share_of_256MB_cache": (stock_all - stock_rep + mod) / 2 ** 20 / CACHE_MB,
                     "replaced_tags_share_of_cache": mod / 2 ** 20 / CACHE_MB}
    return res


def lay_out(tier):
    mid, name, desc = TIERS[tier]
    M = os.path.join(MODS, mid)
    shutil.rmtree(M, ignore_errors=True)
    t = os.path.join(W, tier, "tags")
    for f in glob.glob(t + "/**/*.bitmap", recursive=True):
        dst = os.path.join(M, "tags", os.path.relpath(f, t))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(f, dst)
    open(os.path.join(M, "mod.toml"), "w").write(
        'name = "%s"\nid = "%s"\nversion = "0.1.0"\ndescription = "%s"\nformat = 1\n' % (name, mid, desc))
    open(os.path.join(M, "LOCAL_ONLY.txt"), "w").write(
        "Built on this machine from the player's own Halo maps (CE pixels). Never commit, share or ship.\n")


def main():
    skip = "--skip-check" in sys.argv
    os.makedirs(W + "/tmp", exist_ok=True)
    report = {"tiers": {}}
    sz = {}
    for tier in TIERS:
        fmts = write_data(tier)
        invader(tier, fmts)
        sz[tier] = sizes(tier, fmts)
        log = [] if skip else checks(tier, fmts)
        over = {k: v for k, v in sz[tier].items() if v["largest_bitmap_bytes"] > PER_BITMAP_MB * 2 ** 20}
        up = {k: v for k, v in sz[tier].items() if v["largest_bitmap_bytes"] > UPLOAD_MB * 2 ** 20}
        report["tiers"][tier] = {"formats": fmts, "sizes": sz[tier], "checks_run": len(log),
                                 "checks_passed": sum(c["pass"] for c in log),
                                 "check_failures": [c for c in log if not c["pass"]],
                                 "maps_per_tag": {tag: sorted(c["map"] for c in log if c["tag"] == tag) for tag in fmts},
                                 "pixels_differ_from_b30": sorted({(c["tag"], c["map"]) for c in log for b in log
                                                                   if b["tag"] == c["tag"] and b["map"] == "b30"
                                                                   and b["pixels_md5"] != c["pixels_md5"]}),
                                 "over_per_bitmap_limit": list(over), "over_upload_limit": list(up),
                                 "total_pixel_MB": sum(v["pixel_bytes"] for v in sz[tier].values()) / 2 ** 20}
        print(tier, "checks", report["tiers"][tier]["checks_passed"], "/", len(log), "total MB",
              round(report["tiers"][tier]["total_pixel_MB"], 1), flush=True)
        if log and report["tiers"][tier]["check_failures"]:
            print(json.dumps(report["tiers"][tier]["check_failures"][:5], indent=1))
            raise SystemExit("bitmap checks failed")
        if over:
            raise SystemExit("over the per-bitmap limit: %s" % list(over))
        lay_out(tier)
    report["b30_cache"] = b30_budget(sz)
    json.dump(report, open(os.path.join(W, "mods_report.json"), "w"), indent=1, default=float)
    print(json.dumps(report["b30_cache"], indent=1))
    print("MODS_OK")


if __name__ == "__main__":
    main()
