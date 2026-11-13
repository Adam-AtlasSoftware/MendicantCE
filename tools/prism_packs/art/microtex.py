"""PRISM microtexture library v1: procedural, tileable, colour-free micro tiles (ours).

  python art/microtex.py --out <work>/materials/tiles \
      [--previews <folder>] [--classes a,b] [--no-ktx2]

Recipes: art/microtex_lib/recipes/<class>.json (physical sizes in mm). Height is the
source; normal, cavity and roughness offset are derived with wrap-around filters, authored at
author_px and box-downsampled to ship_px. Per class it writes to --out:
  <class>.png            packed v1 tile RGBA8: RG normal xy (0.5 bias, OpenGL +Y),
                         B cavity (0.5 = 1.0), A roughness offset (0.5 = 0)
  <class>_N.png          RGB tangent normal      <class>_cavity.png  L8 (0.5 = 1.0)
  <class>_rough.png      L8 (0.5 = 0)            <class>_height.png  L16 (0..height_scale_mm)
  <class>_n.ktx2         BC5 normal xy, full mip chain (box-filtered, renormalised)
  <class>_cr.ktx2        BC5 R = cavity/2, G = roughness offset + 0.5 with Toksvig on mips
  src/<class>_height_<author_px>.png   authored height L16
  <class>.recipe.json    the recipe as used        <class>.gates.json  gate results + stats
  library.json           manifest (classes, encodings, excluded classes, gate summary)
and <class>_tile2x2.png (numpy-lit 2x2 tiling check) to --previews.
Exit code 1 if any gate fails.

  --legacy-fp-arms DIR  also writes the v0 layout that fp_build.py / fp_fidelity.py read
  (mjolnir_glove = rubber_grip, mjolnir_undersuit): <cls>.json (world_size_m), <cls>_height.png,
  <cls>_N.png, <cls>_R.png (R rough+0.5, G cavity/2, B height, A spec occlusion).
"""
import argparse
import glob
import json
import os
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from microtex_lib import gates, ktx2, maps  # noqa: E402

RECIPES = os.path.join(HERE, "microtex_lib", "recipes")


def save_l16(path, a):
    lo, hi = float(a.min()), float(a.max())
    q = np.round((a - lo) / max(hi - lo, 1e-12) * 65535).astype(np.uint16)
    Image.fromarray(q).save(path)
    return hi - lo


def write_ktx2(out, name, recipe, Ns, nlen, cav, rough):
    # normal mips from the unnormalised (length-carrying) vectors
    chain = maps.mip_chain_normals(Ns * nlen[..., None])
    n_levels, cr_levels = [], []
    tok_means = []
    base = recipe.get("base_roughness_ref", 0.5)
    c, r = cav, rough
    for li, (N, L) in enumerate(chain):
        if li > 0:
            c = (c.reshape(c.shape[0] // 2, 2, c.shape[1] // 2, 2).mean((1, 3))
                 if c.shape[0] > 1 else c)
            r = (r.reshape(r.shape[0] // 2, 2, r.shape[1] // 2, 2).mean((1, 3))
                 if r.shape[0] > 1 else r)
        Ltot = L * (nlen if li == 0 else 1.0)
        tok = maps.toksvig_rough_offset(base, np.clip(Ltot if li == 0 else L, 1e-6, 1))
        tok_means.append(float(tok.mean()))
        rg = np.stack([maps.q8(N[..., 0] * 0.5 + 0.5), maps.q8(N[..., 1] * 0.5 + 0.5)], -1)
        n_levels.append(ktx2.bc5_encode(rg))
        cr = np.stack([maps.q8(np.clip(c * 0.5, 0, 1)), maps.q8(np.clip(0.5 + r + tok, 0, 1))], -1)
        cr_levels.append(ktx2.bc5_encode(cr))
    s = Ns.shape[0]
    p_n = os.path.join(out, name + "_n.ktx2")
    p_cr = os.path.join(out, name + "_cr.ktx2")
    sz_n = ktx2.write(p_n, ktx2.VK_BC5_UNORM, s, s, n_levels)
    sz_cr = ktx2.write(p_cr, ktx2.VK_BC5_UNORM, s, s, cr_levels)
    # verify: read back, decode level 0, compare with the 8-bit targets
    k = ktx2.read(p_n)
    dec = ktx2.bc5_decode(k["levels"][0], s, s).astype(int)
    ref = np.stack([maps.q8(Ns[..., 0] * 0.5 + 0.5), maps.q8(Ns[..., 1] * 0.5 + 0.5)], -1).astype(int)
    err = np.abs(dec - ref)
    return {"n_ktx2": os.path.basename(p_n), "cr_ktx2": os.path.basename(p_cr),
            "bytes": {"n": sz_n, "cr": sz_cr}, "levels": len(n_levels),
            "bc5_level0_err_max": int(err.max()), "bc5_level0_err_mean": round(float(err.mean()), 3),
            "toksvig_rough_add_mean_by_level": [round(t, 4) for t in tok_means]}


LEGACY = {"rubber_grip": "mjolnir_glove", "mjolnir_undersuit": "mjolnir_undersuit"}


def write_legacy(d, recipe, sh, gate):
    name = LEGACY[recipe["class"]]
    os.makedirs(d, exist_ok=True)
    h = sh["h"] - sh["h"].min()
    hn = h / max(h.max(), 1e-12)
    Image.fromarray((hn * 65535).astype(np.uint16)).save(os.path.join(d, name + "_height.png"))
    Image.fromarray(maps.q8(sh["N"] * 0.5 + 0.5), "RGB").save(os.path.join(d, name + "_N.png"))
    R = np.stack([np.clip(0.5 + sh["rough"], 0, 1), np.clip(sh["cav"] * 0.5, 0, 1), hn, np.clip(sh["cav"], 0, 1)], -1)
    Image.fromarray(maps.q8(R), "RGBA").save(os.path.join(d, name + "_R.png"))
    with open(os.path.join(d, name + ".json"), "w") as f:
        json.dump({"class": name, "source_class": recipe["class"], "world_size_m": recipe["tile_mm"] / 1000.0,
                   "size_px": recipe["ship_px"], "texel_mm": recipe["tile_mm"] / recipe["ship_px"],
                   "height_scale_mm": float(h.max()), "checks": gate}, f, indent=1)


def run_class(recipe, out, prev, do_ktx2, legacy_dir=None):
    name = recipe["class"]
    t0 = time.time()
    R = maps.build(recipe)
    sh = R["ship"]
    packed = maps.pack_v1(sh["N"], sh["cav"], sh["rough"])
    decoded = maps.unpack_v1(packed)
    rgbN = maps.q8(sh["N"] * 0.5 + 0.5)
    rgb_len = np.linalg.norm(rgbN.astype(np.float64) / 255 * 2 - 1, axis=-1)
    g = gates.run(recipe, sh, decoded, rgb_len, colour_channels=0)
    files = {}
    Image.fromarray(packed, "RGBA").save(os.path.join(out, name + ".png"))
    Image.fromarray(rgbN, "RGB").save(os.path.join(out, name + "_N.png"))
    Image.fromarray(maps.q8(np.clip(sh["cav"] * 0.5, 0, 1)), "L").save(os.path.join(out, name + "_cavity.png"))
    Image.fromarray(maps.q8(np.clip(0.5 + sh["rough"], 0, 1)), "L").save(os.path.join(out, name + "_rough.png"))
    hs = save_l16(os.path.join(out, name + "_height.png"), sh["h"])
    os.makedirs(os.path.join(out, "src"), exist_ok=True)
    ha = save_l16(os.path.join(out, "src", f"{name}_height_{recipe['author_px']}.png"), R["author"]["h"])
    for mk, mv in R["masks"].items():
        Image.fromarray(maps.q8(np.clip(mv, 0, 1)), "L").save(os.path.join(out, f"{name}_{mk}mask.png"))
        files[f"{mk}_mask"] = f"{name}_{mk}mask.png"
    files.update({"packed_v1": name + ".png", "normal": name + "_N.png", "cavity": name + "_cavity.png",
                  "rough": name + "_rough.png", "height": name + "_height.png",
                  "height_authored": f"src/{name}_height_{recipe['author_px']}.png"})
    kt = write_ktx2(out, name, recipe, sh["N"], sh["nlen"], sh["cav"], sh["rough"]) if do_ktx2 else None
    if kt:
        files["ktx2"] = kt
    if prev:
        Image.fromarray(maps.lit_preview(decoded[0], decoded[1]), "L").save(os.path.join(prev, name + "_tile2x2.png"))
    ctx = R["ctx"]
    result = {
        "class": name, "tile_mm": recipe["tile_mm"], "author_px": recipe["author_px"], "ship_px": recipe["ship_px"],
        "ship_texel_mm": round(ctx.ship_texel, 5), "min_feature_mm": round(ctx.min_feat, 4),
        "height_scale_mm_shipped": round(hs, 5), "height_scale_mm_authored": round(ha, 5),
        "slope_rms_physical": round(R["slope_rms_physical"], 4), "normal_gain": round(R["gain"], 3),
        "toksvig_len_mean_level0": round(float(sh["nlen"].mean()), 5),
        "layers": R["layers"], "clamps": ctx.notes, "gates": g, "files": files,
        "seconds": round(time.time() - t0, 1),
    }
    if legacy_dir and name in LEGACY:
        write_legacy(legacy_dir, recipe, sh, g)
    with open(os.path.join(out, name + ".recipe.json"), "w") as f:
        json.dump(recipe, f, indent=1)
    with open(os.path.join(out, name + ".gates.json"), "w") as f:
        json.dump(result, f, indent=1)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--previews")
    ap.add_argument("--recipes", default=RECIPES)
    ap.add_argument("--classes")
    ap.add_argument("--no-ktx2", action="store_true")
    ap.add_argument("--legacy-fp-arms", metavar="DIR")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.previews:
        os.makedirs(a.previews, exist_ok=True)
    recipes = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(a.recipes, "*.json")))]
    want = set(a.classes.split(",")) if a.classes else None
    lib = {"version": 1, "generator": "art/microtex.py", "colour": "none (normal, cavity, roughness only)",
           "encoding": {"packed_v1": "RGBA8: R,G normal x,y (0.5 bias, OpenGL +Y); B cavity (0.5 = 1.0); A roughness offset (0.5 = 0)",
                        "n_ktx2": "BC5 UNORM: R,G normal x,y (0.5 bias); z = sqrt(1 - x^2 - y^2); mips box-filtered and renormalised",
                        "cr_ktx2": "BC5 UNORM: R cavity/2; G roughness offset + 0.5, mips add Toksvig roughness for base_roughness_ref",
                        "tile_scale": "overlay tile scale = uv_m / (tile_mm / 1000)"},
           "gate_limits": gates.LIMITS, "classes": {}, "excluded": {}}
    ok = True
    for rc in recipes:
        if want and rc["class"] not in want:
            continue
        if rc.get("excluded"):
            lib["excluded"][rc["class"]] = {k: rc[k] for k in ("description", "suggested_shaders", "rule")}
            with open(os.path.join(a.out, rc["class"] + ".recipe.json"), "w") as f:
                json.dump(rc, f, indent=1)
            print(f"{rc['class']:22s} excluded (no tile)")
            continue
        r = run_class(rc, a.out, a.previews, not a.no_ktx2, a.legacy_fp_arms)
        g = r["gates"]
        ok &= g["pass"]
        lib["classes"][rc["class"]] = {
            "group": rc["group"], "first_person": rc.get("first_person", False),
            "tile_m": rc["tile_mm"] / 1000.0, "ship_px": rc["ship_px"], "author_px": rc["author_px"],
            "normal_gain": r["normal_gain"], "fade_m": rc.get("fade_m"), "suggested_shaders": rc.get("suggested_shaders"),
            "files": r["files"], "pass": g["pass"],
            "failed": [k for k, v in g["checks"].items() if not v]}
        print(f"{rc['class']:22s} {'PASS' if g['pass'] else 'FAIL'} seam+{g['seam_excess']:.3f} "
              f"meanN {g['mean_normal_deg_from_z']:.3f}deg cav {g['cavity_mean']:.4f} rough {g['rough_offset_mean']:+.4f} "
              f"lowf {max(g['lowfreq_frac'].values()):.1e} h {g['height_range_mm']:.3f}mm "
              f"medWL {g['median_wavelength_mm']:.2f}mm fine {g['fine_frac_below_4_texels']:.3f} "
              f"gain {r['normal_gain']:.2f} ({r['seconds']}s)"
              + ("" if g["pass"] else f"  FAILED {[k for k, v in g['checks'].items() if not v]}"))
    if not want:
        with open(os.path.join(a.out, "library.json"), "w") as f:
            json.dump(lib, f, indent=1)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
