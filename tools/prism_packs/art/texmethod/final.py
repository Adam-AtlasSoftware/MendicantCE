"""Side-by-sides of the prototyped methods on whole maps (4K target), 1:1 crops at 4K.

  python -I final.py tex [tex ...]

Per texture: CE nearest | A: ML 4x chained to target, raw | A': ML 4x + Lanczos to target + low-band
back-projection | B1: spectral extension (noise octaves) | B2: MARS self-exemplar re-synthesis + redrawn
edges | C: smart material (B2 at 0.6 gain + wear/grime/scratches from CE's own curvature and palette);
second row lit (one directional light) with each method's normal map (A' with CE's bump only).
Writes <work>/textures/out/final/<tex>/*.png + metrics.json and the full 4K maps of B2 and C
(albedo, normal, roughness, height) under <work>/textures/out/final/<tex>/maps/. Local only.
"""
import json
import os
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mars as M  # noqa: E402
import sheet  # noqa: E402
import smart as S  # noqa: E402
import texlib as T  # noqa: E402

# name: file, F, wrap, kind, ML model for the base, bump file, multipurpose file, crop (x, y, size CE px)
CFG = {
    "chief_diffuse": ("chief_diffuse.png", 8, False, "hard", "general_wdn_v3", None, "chief_multi.png", (40, 40, 64)),
    "b30_metal_panels": ("b30_metal_panels.png", 8, True, "hard", "general_wdn_v3", "b30_metal_panels_bump.png", None, (160, 160, 64)),
    "a30_cliff_rock": ("a30_cliff_rock.png", 4, True, "organic", "nomos8ksc", None, None, (300, 96, 64)),
    "a50_cov_panel": ("a50_cov_panel.png", 16, True, "cov", "general_wdn_v3", "a50_cov_panel_bump.png", None, (112, 112, 32)),
    "elite_torso": ("elite_torso.png", 8, False, "hard", "general_wdn_v3", None, "elite_torso_multi.png", (262, 24, 64)),
    "marine_body": ("marine_body.png", 8, False, "cloth", "nomos8ksc", None, None, (20, 160, 64)),
    "a30_detail_grass": ("a30_detail_grass.png", 8, True, "organic", "nomos8ksc", None, None, (224, 224, 64)),
}
OUT = os.path.join(T.WORK, "out/final")
FLAT = None


def main(names):
    for name in names:
        fn, F, wrap, kind, model, bumpf, mpf, (cx, cy, cs) = CFG[name]
        t0 = time.time()
        ce, _ = T.load(os.path.join(T.WORK, "src", fn))
        bump = T.load(os.path.join(T.WORK, "src", bumpf))[0] if bumpf else None
        mp = T.load(os.path.join(T.WORK, "src", mpf))[0] if mpf else None
        h, w = ce.shape[:2]
        H, W = h * F, w * F
        od = os.path.join(OUT, name)
        os.makedirs(os.path.join(od, "maps"), exist_ok=True)
        cut = lambda a: a[cy * F:(cy + cs) * F, cx * F:(cx + cs) * F]  # noqa: E731
        flat_n = np.zeros((H, W, 3), np.float32) + np.array([0.5, 0.5, 1.0], np.float32)
        r0 = np.full((H, W), 0.6, np.float32)
        res = {}
        up4 = T.upscale4(model, ce, wrap)
        base = cv2.resize(up4, (W, H), interpolation=cv2.INTER_LANCZOS4) if F != 4 else up4
        base = np.clip(base, 0, 1)
        clean = T.boxdown(up4, 4)
        # A raw: chained (as the study did) on the crop window only
        mdir = os.path.join(T.WORK, "out/ml", name)
        imgs = {"CE nearest": cut(T.nearest(ce, F))}
        araw = os.path.join(mdir, f"{model}_raw.png")
        if os.path.exists(araw):
            imgs[f"A {model} chained raw"] = T.load(araw)[0]
        a = T.backproject_low(base.copy(), ce, 6, wrap=wrap)
        imgs[f"A' {model} 4x+lanczos+BP-low"] = cut(a)
        res["A_bplow"] = T.fidelity(a, ce)
        nA = flat_n
        if bump is not None:
            hb = M.height_from_normal(bump)
            hb = cv2.resize(hb, (W, H), interpolation=cv2.INTER_CUBIC) * (W / bump.shape[1])
            nA = M.normal_from_height(hb, strength=0.5 * 4 / F, wrap=wrap)
        lit = {"CE nearest": cut(M.lit_preview(T.nearest(ce, F), flat_n, r0)),
               "A'": cut(M.lit_preview(a, nA, r0))}
        mk = "organic" if kind == "organic" else ("skin" if kind == "skin" else ("organic" if kind == "cloth" else "hard"))
        for src, lab in (("noise", "B1 spectral extension"), ("exemplar", "B2 MARS self-exemplar")):
            r = M.mars(ce, F, base, kind=mk, wrap=wrap, mp=mp, bump=bump, source=src,
                       crisp=kind in ("hard", "cov"), ce_clean=clean)
            imgs[lab] = cut(r["albedo"])
            lit[lab.split()[0]] = cut(M.lit_preview(r["albedo"], r["normal"], r["rough"]))
            res[lab.split()[0]] = dict(T.fidelity(r["albedo"], ce), stats=r["stats"])
            if src == "exemplar":
                for k in ("albedo", "normal"):
                    T.save(os.path.join(od, "maps", f"B2_{k}.png"), r[k])
                T.save(os.path.join(od, "maps", "B2_rough.png"), r["rough"])
                T.save(os.path.join(od, "B2_edges.png"), cut(r["edge"]))
                lab_img = (r["labels"].astype(np.float32) / max(1, r["labels"].max()))
                T.save(os.path.join(od, "B2_materials_ce_res.png"), lab_img)
        s = S.smart(ce, F, base, kind=kind, wrap=wrap, bump=bump, mp=mp, ce_clean=clean)
        imgs["C smart material"] = cut(s["albedo"])
        lit["C"] = cut(M.lit_preview(s["albedo"], s["normal"], s["rough"]))
        res["C"] = T.fidelity(s["albedo"], ce)
        for k in ("albedo", "normal"):
            T.save(os.path.join(od, "maps", f"C_{k}.png"), s[k])
        T.save(os.path.join(od, "maps", "C_rough.png"), s["rough"])
        hh = s["height"]
        T.save(os.path.join(od, "maps", "C_height.png"), (hh - hh.min()) / (np.ptp(hh) + 1e-6))
        for k in ("wear", "grime", "scratch"):
            T.save(os.path.join(od, f"C_{k}.png"), cut(s[k]))
        T.save(os.path.join(od, "C_normal.png"), cut(s["normal"]))
        # files + sheets
        items = []
        for lab, im in imgs.items():
            p = os.path.join(od, lab.split()[0].replace("'", "p") + "_albedo.png")
            T.save(p, im)
            items.append((lab, p))
        lit_items = []
        for lab, im in lit.items():
            p = os.path.join(od, "lit_" + lab.split()[0].replace("'", "p") + ".png")
            T.save(p, im)
            lit_items.append(("lit: " + lab, p))
        sheet.make(os.path.join(od, f"{name}_albedo_sheet.png"), 3, items)
        sheet.make(os.path.join(od, f"{name}_lit_sheet.png"), 3, lit_items)
        res["_meta"] = {"file": fn, "factor": F, "size_out": [W, H], "kind": kind, "model": model,
                        "crop_ce_px": [cx, cy, cs], "seconds": round(time.time() - t0)}
        json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
        print(name, "done", round(time.time() - t0), "s",
              {k: (round(v["de_mean"], 3), round(v["deL_mean"], 3), round(v["detail_rms"], 2), round(v["invented_edges"], 4))
               for k, v in res.items() if k != "_meta"}, flush=True)


if __name__ == "__main__":
    main(sys.argv[1:] or list(CFG))
