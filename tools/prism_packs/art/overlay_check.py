#!/usr/bin/env python3
"""Check a material overlay (overlay.json) against the extracted maps.

    python3 -I art/overlay_check.py --overlay <data>/mods/prism_materials/tags/materials/overlay.json \
        --extracts <work>/extract --stats <work>/class_masks/island_stats \
        --out <folder> [--only characters]

For every shader the overlay names:
  resolve  which extracted maps (<work>/extract/<map>/...) have a model using it; a name no map has fails
           (a typo would leave the shader without its overlay, silently)
  classes  every slot is a class the overlay defines, its tile files exist in the mod
  rule     the class fractions the renderer's v1 rule gives (slot A = smoothstep(lo, hi) of the multipurpose
           channel, bilinear at the base map's size; texels whose exclude channel is above its value: none),
           counted per UV island of the model's highest LOD (mirrored islands twice, as island_stats counts
           them). Where the rule is on b (the change colour) with its step centred on 0.5, slot A (over all
           the texels, lights included, as B>.5 counts them) must match island_stats' B>.5 within
           --tolerance points (CM 3.1-3.5). Other channels, and soft steps set off 0.5 on purpose (the
           Marine faces' graded b): reported, not gated.
  lights   no slot weight on a texel whose g (self-illumination) is above 0.1 unless the entry excludes g
           (reported as lit_fraction_unexcluded; fails above 0.5% of the shader's texels)
Writes <out>/overlay_check.json and, per shader, <out>/<shader>.png: base | slot A weight (green) / slot B
(blue) / excluded (red). Local only: the sheets are made from the game's maps.
"""

import argparse
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import class_masks  # noqa: E402  (islands of a shader by the model's bones; island_stats reading)

CH = {"r": 0, "g": 1, "b": 2, "a": 3}


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / max(hi - lo, 1e-3), 0, 1)
    return t * t * (3 - 2 * t)


def models_by_shader(extract_root):
    """shader -> [(map, extract dir, model path relative to the extract)]"""
    out = {}
    for idx in sorted(glob.glob(os.path.join(extract_root, "**", "index.json"), recursive=True)):
        ex = os.path.dirname(idx)
        mp = os.path.relpath(ex, extract_root).split(os.sep)[0]
        for path in sorted(glob.glob(os.path.join(ex, "models", "**", "*.gltf"), recursive=True)):
            doc = json.load(open(path))
            for m in doc.get("materials", []):
                out.setdefault(m.get("name"), []).append((mp, ex, os.path.relpath(path, ex)))
    return out


def check_shader(name, entry, uses, stats_dir, tol, out):
    mp_name, ex, model = uses[0]
    found, maps = class_masks.islands(os.path.join(ex, model), name)
    info = {"maps": sorted({u[0] for u in uses}), "model": model, "extract": os.path.relpath(ex),
            "slots": entry["slots"], "islands": len(found)}
    if not found:
        info["error"] = "no triangles in the highest LOD"
        return info, False
    base = np.array(Image.open(os.path.join(ex, maps["base_map"])).convert("RGB"))
    h, w = base.shape[:2]
    mp = None
    if "multipurpose_map" in maps:
        mp = np.array(Image.open(os.path.join(ex, maps["multipurpose_map"])).convert("RGBA")
                      .resize((w, h), Image.BILINEAR)).astype(np.float64) / 255.0
    rule = entry.get("mask") if len(entry["slots"]) > 1 else None
    if rule and mp is not None:
        wa = smoothstep(rule.get("lo", 0.4), rule.get("hi", 0.6), mp[..., CH[rule["channel"]]])
        if rule.get("invert"):
            wa = 1 - wa
    else:
        wa = np.ones((h, w))
    ex_rule = entry.get("exclude")
    excluded = (mp[..., CH[ex_rule["channel"]]] > ex_rule.get("above", 0.1)) if (ex_rule and mp is not None) else np.zeros((h, w), bool)
    lit = (mp[..., 1] > 0.1) if mp is not None else np.zeros((h, w), bool)
    sums = np.zeros(2)
    rule_a = 0.0
    n = n_ex = n_lit = 0
    union = np.zeros((h, w), bool)
    for _, tris in found:
        m = class_masks.raster(tris, w, h)
        union |= m
        live = m & ~excluded
        sums += [wa[live].sum(), (1 - wa[live]).sum()]
        rule_a += float(wa[m].sum())
        n += int(m.sum())
        n_ex += int((m & excluded).sum())
        n_lit += int((m & lit & ~excluded).sum())
    fr = {entry["slots"][0]: round(float(sums[0] / n), 4)}
    if len(entry["slots"]) > 1:
        fr[entry["slots"][1]] = round(float(sums[1] / n), 4)
    info.update({"has_multipurpose": mp is not None, "rule": rule, "class_fractions_island_weighted": fr,
                 "excluded_fraction": round(n_ex / n, 4), "lit_fraction_unexcluded": round(n_lit / n, 4)})
    ok = info["lit_fraction_unexcluded"] <= 0.005
    stats_file = os.path.join(stats_dir, model.split("/")[-1].replace(".gltf", ".txt"))
    centred = rule is not None and abs(0.5 * (rule.get("lo", 0.4) + rule.get("hi", 0.6)) - 0.5) <= 0.05
    if rule and rule["channel"] == "b" and not centred:
        info["gate_island_b"] = "not gated: the step (%g-%g) is not centred on 0.5, so B>.5 is not its truth" % (
            rule.get("lo", 0.4), rule.get("hi", 0.6))
    elif rule and rule["channel"] == "b" and os.path.exists(stats_file):
        stats = class_masks.read_stats(stats_file, name)
        if stats:
            e = class_masks.expected("island_b", stats)
            # (the rule's slot A over all the islands' texels, lights included: B>.5 counts them too; the
            # exclusion then takes the lights out of both slots, reported as excluded_fraction)
            got = round(rule_a / n, 4) if not rule.get("invert") else round(1 - rule_a / n, 4)
            d = round(100 * (got - e), 2)
            info["gate_island_b"] = {"expected": round(e, 4), "built": got, "difference_points": d,
                                     "pass": abs(d) <= tol, "stats_file": os.path.relpath(stats_file)}
            ok &= abs(d) <= tol
    # review sheet
    cls = np.zeros((h, w, 3))
    cls[..., 1] = wa * 220
    if len(entry["slots"]) > 1:
        cls[..., 2] = (1 - wa) * 220
    cls[excluded] = (230, 40, 40)
    cls[~union] = base[~union] * 0.2
    sheet = np.concatenate([base, np.full((h, 6, 3), 24), cls], 1).astype(np.uint8)
    s = max(1, 256 // h)
    im = Image.fromarray(sheet)
    if s > 1:
        im = im.resize((im.width * s, im.height * s), Image.NEAREST)
    im.save(os.path.join(out, name.replace("\\", "_").replace(" ", "_") + ".png"))
    info["pass"] = bool(ok)
    return info, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--overlay", required=True)
    ap.add_argument("--extracts", required=True, help="the folder of map_extract outputs, one per map")
    ap.add_argument("--stats", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", help="check only shaders whose name starts with this (e.g. characters)")
    ap.add_argument("--tolerance", type=float, default=3.0)
    a = ap.parse_args()
    overlay = json.load(open(a.overlay))
    mod_dir = os.path.dirname(a.overlay)
    os.makedirs(a.out, exist_ok=True)
    uses = models_by_shader(a.extracts)
    report = {"overlay": os.path.relpath(a.overlay), "shaders": {}, "classes": {}}
    ok = True
    for cname, c in overlay["classes"].items():
        files = [c["tile"]] + list(c.get("levels", {}).values())
        missing = [f for f in files if not os.path.exists(os.path.join(mod_dir, f.replace("\\", "/")))]
        report["classes"][cname] = {"files": files, "missing": missing}
        ok &= not missing
    for name, entry in overlay["shaders"].items():
        if a.only and not name.startswith(a.only):
            continue
        if name not in uses:
            report["shaders"][name] = {"error": "no extracted map has a model using it", "pass": False}
            ok = False
            continue
        if entry is None:
            report["shaders"][name] = {"maps": sorted({u[0] for u in uses[name]}), "overlay": None, "pass": True}
            continue
        undefined = [s for s in entry["slots"] if s not in overlay["classes"]]
        if undefined:
            report["shaders"][name] = {"error": "undefined classes %s" % undefined, "pass": False}
            ok = False
            continue
        info, good = check_shader(name, entry, uses[name], a.stats, a.tolerance, a.out)
        report["shaders"][name] = info
        ok &= good
    report["pass"] = bool(ok)
    with open(os.path.join(a.out, "overlay_check.json"), "w") as f:
        json.dump(report, f, indent=1)
    for name, r in report["shaders"].items():
        if r.get("overlay", 1) is None:
            continue
        g = r.get("gate_island_b")
        g = g if isinstance(g, dict) else None
        print("%-4s %-52s %-40s ex %5.3f lit %5.3f %s" % (
            "ok" if r.get("pass") else "FAIL", name.split("\\")[-1], r.get("class_fractions_island_weighted", r.get("error")),
            r.get("excluded_fraction", 0), r.get("lit_fraction_unexcluded", 0),
            ("B>.5 %.3f vs %.3f (%+.1f pt)" % (g["built"], g["expected"], g["difference_points"])) if g else ""))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
