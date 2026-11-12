"""PRISM v2 Bold textures as local mods (CE pixels: built on this machine, never shared or shipped):

  <data>/mods/prism_v2_tex_bold              display.prism_material_definition = bold
  <data>/mods/prism_v2_tex_bold_weathering   the same with display.prism_weathering = true

Each is the Enhanced tier mod with bold.py's base maps in place of Enhanced's for the Chief, the first-person arms
and Assault Rifle, the Elites (minor/major and special) and the Marines (body, arms, Johnson's arms, legs, helmet),
plus the surface map of each next to its bitmap (tags\\<bitmap>.surface.png: the r2 engine draws it with
display.prism_surface_maps = true and ignores it otherwise). Every other bitmap stays Enhanced.

Mip levels: Invader filters each level from the one above with a 2x2 box, and over the 2-3 levels a 4x map drops
before 8 m that read softer than CE's own (-0.5 to -2.7 % local contrast at 8 m). Here
every level is resampled from the full map with a Lanczos-3 filter plus a light unsharp mask (MIP_SHARPEN), each
compressed alone by Invader at its own size, and spliced into the full tag's pixel data in place of Invader's
(level 0 is byte-identical, checked).

  python -I bold_mods.py [--box-mips]
"""
import os
import shutil
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import bold as B  # noqa: E402
import mod_bitmap_check as MB  # noqa: E402
import texvis_bands as TB  # noqa: E402

E = B.E
INV = E.invader("invader-bitmap")
WORK = E.work("textures", "bold", "build")
ENH = E.work("textures", "mods", "prism_v2_tex_enhanced")
MODS = E.out("mods")
ENV = dict(E.invader_env(), TMPDIR=WORK + "/tmp")
MIP_SHARPEN = 0.25
VARIANTS = {
    "prism_v2_tex_bold": ("bold", "surface", "PRISM v2 textures: Bold (local)",
                          "display.prism_material_definition = bold. The Enhanced set with crisper material definition on the Chief, the first-person arms and Assault Rifle, the Elites and the Marines: CE's layout, hue, chroma and designs; value contrast within and between materials raised to read at play distance, sharper mip levels; surface maps (normal and occlusion from the r2 remodels' geometry, roughness per material) for display.prism_surface_maps. Built on this machine from your own maps: never shared or shipped."),
    "prism_v2_tex_bold_weathering": ("weathered", "surface_weathered", "PRISM v2 textures: Bold + Weathering (local)",
                                     "display.prism_material_definition = bold with display.prism_weathering = true: Bold plus edge wear on plate rims and grime in creases, which CE never had. Built on this machine from your own maps: never shared or shipped."),
}


def u8(a):
    return (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)


def nudge(px):
    rgb = px[..., :3]
    for key in ((0, 0, 255), (255, 0, 255), (0, 255, 255)):
        m = (rgb[..., 0] == key[0]) & (rgb[..., 1] == key[1]) & (rgb[..., 2] == key[2])
        rgb[m, 2] = 254
    return px


def mip_levels(px, box=False):
    """[level0, level1, ...] uint8 RGBA down to 1x1: Lanczos-3 from the full map plus a light unsharp mask"""
    H, W = px.shape[:2]
    out = [px]
    im = Image.fromarray(px, "RGBA")
    w, h = W, H
    prev = px
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        if box:
            lv = np.asarray(Image.fromarray(prev, "RGBA").resize((w, h), Image.BOX))
        else:
            lv = np.asarray(im.resize((w, h), Image.LANCZOS)).astype(np.float32)
            if min(w, h) >= 8 and MIP_SHARPEN > 0:
                bl = cv2.GaussianBlur(lv, (0, 0), 0.7)
                lv = lv + MIP_SHARPEN * (lv - bl)
            lv = np.clip(lv + 0.5, 0, 255).astype(np.uint8)
        out.append(lv)
        prev = lv
    return out


def level_bytes(w, h, fmt):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == "dxt1" else 16)


def build_tag(tag, px, fmt, box=False):
    """the bitmap tag bytes for px (RGBA uint8): Invader's tag with every level of 4x4 and up replaced by our own
    (each compressed alone by Invader at its size); the levels under 4 texels stay Invader's"""
    d, t = os.path.join(WORK, "data"), os.path.join(WORK, "tags")
    levels = mip_levels(px, box)
    own = []
    for i, lv in enumerate(levels):
        if i and (box or min(lv.shape[:2]) < 4):
            continue
        n = "%s__l%d" % (tag, i) if i else tag
        p = os.path.join(d, n + ".png")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        Image.fromarray(nudge(lv.copy()), "RGBA").save(p)
        args = [INV, "-d", d, "-t", t, "-T", "2d_textures", "-F", fmt, "-u", "default"]
        if i:
            args += ["-M", "0"]
        r = subprocess.run(args + [n], env=ENV, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("invader-bitmap %s: %s" % (n, r.stdout + r.stderr))
        own.append((i, n))
    full = bytearray(open(os.path.join(t, tag + ".bitmap"), "rb").read())
    if box:
        return bytes(full)
    rf = MB.read(os.path.join(t, tag + ".bitmap"))
    o = MB.HDR + MB.GROUP + rf["color_plate_bytes"]
    fmt = rf["bitmaps"][0]["format"]  # Invader writes DXT1 when the alpha is all opaque
    sizes = [level_bytes(lv.shape[1], lv.shape[0], fmt) for lv in levels]
    if sum(sizes) != rf["pixel_bytes"] or rf["bitmaps"][0]["mipmaps"] != len(levels) - 1:
        raise SystemExit("mip layout mismatch %s: %d vs %d bytes, mipmaps %d levels %d" % (
            tag, sum(sizes), rf["pixel_bytes"], rf["bitmaps"][0]["mipmaps"], len(levels) - 1))
    offs = np.concatenate([[0], np.cumsum(sizes)]).astype(int)
    for i, n in own:
        if i == 0:
            continue
        tp = os.path.join(t, n + ".bitmap")
        rl = MB.read(tp)
        assert rl["pixel_bytes"] == sizes[i], (n, rl["pixel_bytes"], sizes[i])
        b = open(tp, "rb").read()
        ol = MB.HDR + MB.GROUP + rl["color_plate_bytes"]
        full[o + offs[i]:o + offs[i + 1]] = b[ol:ol + sizes[i]]
    return bytes(full)


def main():
    box = "--box-mips" in sys.argv
    os.makedirs(WORK + "/tmp", exist_ok=True)
    os.makedirs(WORK + "/tags", exist_ok=True)
    keys = [k for k in B.KEYS if os.path.exists(os.path.join(B.OUT, k, "bold.png"))]
    report = []
    for mid, (alb, srf, name, desc) in VARIANTS.items():
        M = os.path.join(MODS, mid + ("_boxmips" if box else ""))
        shutil.rmtree(M, ignore_errors=True)
        shutil.copytree(ENH, M)
        for k in keys:
            tag = TB.TAG[B.KEYS[k]]
            dst = os.path.join(M, "tags", tag + ".bitmap")
            fmt = MB.read(dst)["bitmaps"][0]["format"]
            rgb = np.asarray(Image.open(os.path.join(B.OUT, k, alb + ".png")).convert("RGB"))
            ap = os.path.join(B.TI.OUT, B.KEYS[k], "alpha_enh.png")  # Enhanced's alpha (tiers.py), unchanged
            if fmt == "dxt5" and os.path.exists(ap):
                al = np.asarray(Image.open(ap).convert("L"))
            else:
                al = np.full(rgb.shape[:2], 255, np.uint8)
            px = np.dstack([rgb, al])
            open(dst, "wb").write(build_tag(tag, px, fmt, box))
            shutil.copy2(os.path.join(B.OUT, k, srf + ".png"), os.path.join(M, "tags", tag + ".surface.png"))
            report.append((mid, k, fmt))
        open(os.path.join(M, "mod.toml"), "w").write(
            'name = "%s"\nid = "%s"\nversion = "0.1.0"\ndescription = "%s"\nformat = 1\n' % (name, mid, desc))
        open(os.path.join(M, "LOCAL_ONLY.txt"), "w").write(
            "Built on this machine from your own Halo CE maps. Never commit, share, upload or ship.\n")
        print(mid, len(keys), "maps", flush=True)
    print("BOLD_MODS_OK", report)


if __name__ == "__main__":
    main()
