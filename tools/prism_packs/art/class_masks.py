#!/usr/bin/env python3
"""PRISM M2: the class masks of the material overlay, built locally from the player's own maps.

    python3 -I art/class_masks.py --extract <work>/extract/b30 \
        --recipes art/class_mask_recipes.json \
        --stats <work>/class_masks/island_stats \
        --out <data>/mods/prism_class_masks [--review <folder>]

For each shader in the recipes: its model's UV islands (the highest LOD; triangles joined by shared
vertices, map_extract splits vertices at UV seams), each island's dominant bone group (skin weights
summed over its vertices; the bone's name without 'bip01 ' / 'frame ' and its side), that group's class
(P-auto), the colour tiebreak on the base map inside the non-plate texels (T: blue above red and green
by a ratio, or, inverted, not), then the recipe's
polygons (P-paint: "uv" corners, or an "ellipse" [centre u, v, radius u, v]). The result is one RGBA8 PNG per (shader, base bitmap index) at the base map's size:
R G B A = the weights of the shader's slots A B C D (one-hot inside the islands, softened by a texel,
and grown 8 texels into the gutters so that bilinear filtering and the smaller mip levels do not pull
another island's class in). The renderer (port/linux/game/mod_materials.c, nv2a_psh.c) applies the
shader's live channel rule on top: slot A where the multipurpose channel is above hi.

A recipe with "mask_wins": true is for a shader whose overlay entry says "class_mask_wins": the mask's
slots A, C and D are kept over the rule, which decides A against B over the mask's B alone. Its
"keep_plate" ({"close_px": r}) names slot A in the mask where the rule's plate (above 0.5), closed by
r texels (dilated, then eroded), lies over the islands' default class (slot B): the thin dips a
scratch paints into the multipurpose channel inside a plate stay plate. Its "plate_tiebreak" entries
({"to", "max_rgb_above", "saturation_below", "channel_r_above", "blur_px"}) give another class inside
the rule's plate by the base map's colour and the multipurpose channel (a buckle's bare metal).

The gate: per shader, the class fractions over its islands' texels (each island counted as
island_stats.py counts it), the channel rule applied as the renderer applies it, against the island
statistics (island_stats/*.txt, CM 3.1): within tolerance_points (3) or the build fails.

Writes
  <out>/mod.toml, <out>/tags/materials/masks/<shader>_<index>.png   (local only: made from the game's maps)
  <out>/class_masks.json                                             (fractions, expectations, gate)
  <review>/<shader>.png (base | mask classes | final classes with the rule), if --review
Nothing here is shipped: only the recipes and this tool are (the first-time setup runs it).
"""

import argparse
import collections
import json
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

CH = {"r": 0, "g": 1, "b": 2, "a": 3}
LOD_ORDER = ["superhigh", "high", "medium", "low", "superlow"]
# the review's colours of the classes (others grey)
COLOURS = {"mjolnir_plate": [225, 225, 225], "mjolnir_undersuit": [120, 90, 220], "hard_polymer": [235, 150, 40],
           "rubber_grip": [40, 200, 90], "marine_paint": [225, 225, 225], "marine_bdu": [120, 90, 220],
           "marine_webbing": [120, 90, 220], "human_skin": [235, 150, 40], "cov_armour_worn": [225, 225, 225],
           "cov_cloth": [120, 90, 220], "skin_jackal": [235, 150, 40], "cov_armour": [225, 225, 225],
           "skin_elite": [235, 150, 40], "cov_bodysuit": [120, 90, 220],
           "marine_metal": [40, 200, 90]}
GUTTER = 8


def accessor(doc, buf, index):
    a = doc["accessors"][index]
    bv = doc["bufferViews"][a["bufferView"]]
    comp = {5126: ("f", 4), 5123: ("H", 2), 5125: ("I", 4), 5121: ("B", 1)}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", comp[1] * n)
    raw = np.frombuffer(buf, np.uint8, count=stride * (a["count"] - 1) + comp[1] * n, offset=off)
    rows = np.lib.stride_tricks.as_strided(raw, (a["count"], comp[1] * n), (stride, 1)).copy()
    out = np.frombuffer(rows.tobytes(), "<" + comp[0]).reshape(a["count"], n).astype(np.float64)
    if a.get("normalized"):
        out /= {5121: 255.0, 5123: 65535.0}[a["componentType"]]
    return out


def bone_group(name):
    name = name.replace("bip01 ", "").replace("frame ", "")
    return name[2:] if name[:2] in ("l ", "r ") else name


def islands(path, shader):
    """the shader's UV islands in the model's highest LOD: [(bone group, uv triangles (n,3,2))], and its maps"""
    doc = json.load(open(path))
    buf = open(os.path.join(os.path.dirname(path), doc["buffers"][0]["uri"]), "rb").read()
    joints = [doc["nodes"][j]["name"] for j in doc["skins"][0]["joints"]]
    best = {}
    for node in doc["nodes"]:
        if "mesh" not in node:
            continue
        parts = node["name"].split("/")
        lod = parts[-1] if parts[-1] in LOD_ORDER else node.get("extras", {}).get("lod", "superhigh")
        key = tuple(parts[:-1])
        rank = LOD_ORDER.index(lod) if lod in LOD_ORDER else 0
        if key not in best or rank < best[key][1]:
            best[key] = (node["mesh"], rank)
    names = [m.get("name") for m in doc["materials"]]
    maps = None
    out = []
    for mesh, _ in best.values():
        for p in doc["meshes"][mesh]["primitives"]:
            if names[p.get("material", -1)] != shader:
                continue
            maps = {k: v.get("png") for k, v in doc["materials"][p["material"]]["extras"]["maps"].items()}
            uv = accessor(doc, buf, p["attributes"]["TEXCOORD_0"])
            idx = accessor(doc, buf, p["indices"]).astype(np.int64).reshape(-1, 3)
            J = accessor(doc, buf, p["attributes"]["JOINTS_0"]).astype(np.int64)
            W = accessor(doc, buf, p["attributes"]["WEIGHTS_0"])
            parent = np.arange(len(uv))

            def find(x):
                while parent[x] != x:
                    parent[x] = parent[parent[x]]
                    x = parent[x]
                return x
            for t in idx:
                a, b, c = find(t[0]), find(t[1]), find(t[2])
                parent[b] = a
                parent[find(c)] = a
            comp = collections.defaultdict(list)
            for ti, t in enumerate(idx):
                comp[find(t[0])].append(ti)
            for tris in comp.values():
                vs = np.unique(idx[tris].ravel())
                weights = collections.Counter()
                for v in vs:
                    for j, w in zip(J[v], W[v]):
                        if w > 0:
                            weights[bone_group(joints[j])] += w
                tri_uv = uv[idx[tris]]
                shift = np.floor(uv[vs].min(axis=0) + 1e-3)
                out.append((weights.most_common(1)[0][0], tri_uv - shift))
    return out, maps


def raster(tris, w, h):
    im = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(im)
    for t in tris:
        for du in (0, -1):
            for dv in (0, -1):
                draw.polygon([((u + du) * w, (v + dv) * h) for u, v in t], fill=255)
    return np.array(im) > 0


def box_blur(img, r):
    if r <= 0:
        return img
    out = img.astype(np.float64)
    for axis in (0, 1):
        acc = np.zeros_like(out)
        for d in range(-r, r + 1):
            acc += np.roll(out, d, axis=axis)
        out = acc / (2 * r + 1)
    return out


def grow(weights, covered, steps):
    """the classes grown into the empty texels around the islands (the gutters), a texel a step"""
    weights, covered = weights.copy(), covered.copy()
    for _ in range(steps):
        acc = np.zeros_like(weights)
        n = np.zeros(covered.shape)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            acc += np.roll(np.roll(weights * covered[..., None], dy, 0), dx, 1)
            n += np.roll(np.roll(covered, dy, 0), dx, 1)
        new = ~covered & (n > 0)
        weights[new] = acc[new] / n[new][:, None]
        covered = covered | new
    return weights


def dilate_bool(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def erode_bool(m, r):
    return ~dilate_bool(~m, r)


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / max(hi - lo, 1e-3), 0, 1)
    return t * t * (3 - 2 * t)


def read_stats(path, shader):
    """island_stats.py's lines of the shader: {group: (uvpx, b_over_half)}"""
    rows, on = {}, False
    for line in open(path):
        if line.startswith("## "):
            on = line[3:].strip() == shader
            continue
        if on:
            m = re.match(r"\s+(.+?)\s+isl=\s*\d+\s+tris=\s*\d+\s+uvpx=\s*(\d+).*B>\.5=\s*(\d+)%", line)
            if m:
                group = bone_group(m.group(1))
                uvpx, b = int(m.group(2)), int(m.group(3)) / 100.0
                old = rows.get(group, (0, 0.0))
                rows[group] = (old[0] + uvpx, (old[0] * old[1] + uvpx * b) / (old[0] + uvpx))
    return rows


def expected(spec, stats):
    total = sum(px for px, _ in stats.values())
    if spec == "island_b":
        return sum(px * b for px, b in stats.values()) / total
    kind, groups = spec.split(":")
    groups = [bone_group(g.strip()) for g in groups.split(",")]
    if kind == "island_bone_nonplate":
        return sum(stats[g][0] * (1 - stats[g][1]) for g in groups if g in stats) / total
    if kind == "island_bone":
        return sum(stats[g][0] for g in groups if g in stats) / total
    raise SystemExit("unknown expectation " + spec)


def build(extract, name, recipe, stats_dir, review):
    slots = recipe["slots"]
    found, maps = islands(os.path.join(extract, recipe["model"]), name)
    if not found:
        raise SystemExit("%s: no triangles in %s" % (name, recipe["model"]))
    base = np.array(Image.open(os.path.join(extract, maps["base_map"])).convert("RGB")).astype(np.float64)
    h, w = base.shape[:2]
    mp = None
    if "multipurpose_map" in maps:
        mp = Image.open(os.path.join(extract, maps["multipurpose_map"])).convert("RGBA").resize((w, h), Image.BILINEAR)
        mp = np.array(mp).astype(np.float64) / 255.0
    rule = recipe.get("rule")
    plate = smoothstep(rule["lo"], rule["hi"], mp[..., CH[rule["channel"]]]) if rule and mp is not None else np.zeros((h, w))
    # P-auto: each island's bone group's class (later islands over earlier ones: mirrored sides share a group)
    cls = np.full((h, w), -1, np.int64)
    bones = recipe["bones"]
    island_masks = []
    group_of = np.full((h, w), "", object)
    for group, tris in found:
        c = slots.index(bones.get(group, bones["*"]))
        m = raster(tris, w, h)
        cls[m] = c
        group_of[m] = group
        island_masks.append((group, m))
    covered = cls >= 0
    # T: the colour tiebreak (on the base map, a little blurred), inside the texels the rule leaves non-plate
    for tb in recipe.get("tiebreak", []):
        rgb = box_blur(base, tb.get("blur_px", 0))
        top = rgb[..., :2].max(-1)
        bluish = (rgb[..., 2] - top >= tb.get("b_over_rg_at_least", 0)) & \
            (rgb[..., 2] >= tb.get("b_ratio_at_least", 0) * top) & (rgb[..., 2] >= tb.get("b_at_least", 0))
        hit = covered & (cls == slots.index(tb["from"])) & (~bluish if tb.get("invert") else bluish)
        for group in tb.get("not_bones", []):
            hit &= group_of != group
        cls[hit] = slots.index(tb["to"])
    # (mask_wins) the plate kept: the rule's plate, closed, over the default class; and classes inside it
    wins = bool(recipe.get("mask_wins")) and rule is not None and mp is not None
    if wins and recipe.get("keep_plate"):
        r = int(recipe["keep_plate"].get("close_px", 3))
        hard = plate > 0.5
        closed = erode_bool(dilate_bool(hard, r), r)
        cls[covered & closed & (cls == 1)] = 0
    if wins:
        for tb in recipe.get("plate_tiebreak", []):
            rgb = box_blur(base, tb.get("blur_px", 0))
            top, low = rgb.max(-1), rgb.min(-1)
            sat = (top - low) / np.maximum(top, 1e-6)
            hit = covered & (plate > 0.5) & (top > tb.get("max_rgb_above", 0)) & \
                (sat < tb.get("saturation_below", 1.0)) & \
                (mp[..., CH[rule["channel"]]] > tb.get("channel_r_above", 0.0))
            cls[hit] = slots.index(tb["to"])
    # P-paint: the recipe's polygons, inside the islands only
    for poly in recipe.get("polygons", []):
        im = Image.new("L", (w, h), 0)
        if "ellipse" in poly:
            cu, cv, ru, rv = poly["ellipse"]
            ImageDraw.Draw(im).ellipse([((cu - ru) * w, (cv - rv) * h), ((cu + ru) * w, (cv + rv) * h)], fill=255)
        else:
            ImageDraw.Draw(im).polygon([(u * w, v * h) for u, v in poly["uv"]], fill=255)
        hit = covered & (np.array(im) > 0)
        cls[hit] = slots.index(poly["class"])
    weights = np.zeros((h, w, 4))
    for i in range(len(slots)):
        weights[..., i] = cls == i
    # (softened by a texel inside the islands, then grown into the gutters)
    soft = box_blur(weights, 1)
    soft_n = box_blur(covered.astype(np.float64), 1)
    inside = soft_n > 0
    weights = np.where(covered[..., None], soft / np.maximum(soft_n, 1e-9)[..., None], 0)
    weights = grow(weights, covered, GUTTER)
    rgba = np.clip(np.round(weights * 255), 0, 255).astype(np.uint8)
    del inside

    # the gate: fractions over the islands' texels, each island counted (as island_stats.py counts them),
    # the renderer's combination: slot A = the rule's weight, the rest shared by the mask's other slots
    q = rgba.astype(np.float64) / 255.0
    final = np.zeros((h, w, 4))
    if rule and recipe.get("mask_wins"):
        s = q.sum(-1, keepdims=True)
        qn = np.where(s > 1e-3, q / np.maximum(s, 1e-9), np.eye(4)[0])
        final[..., 0] = qn[..., 0] + qn[..., 1] * plate
        final[..., 1] = qn[..., 1] * (1 - plate)
        final[..., 2:] = qn[..., 2:]
    elif rule:
        rest = q[..., 1:]
        s = rest.sum(-1, keepdims=True)
        rest = np.where(s > 1e-3, rest / np.maximum(s, 1e-9), np.eye(3)[0])
        final[..., 0] = plate
        final[..., 1:] = (1 - plate)[..., None] * rest
    else:
        s = q.sum(-1, keepdims=True)
        final = np.where(s > 1e-3, q / np.maximum(s, 1e-9), np.eye(4)[0])
    sums = np.zeros(4)
    excl = 0.0
    count = 0
    ex = recipe.get("exclude")
    for group, m in island_masks:
        sums += final[m].sum(0)
        count += int(m.sum())
        if ex and mp is not None:
            excl += float((mp[..., CH[ex["channel"]]][m] > ex["above"]).sum())
    fractions = {slots[i]: round(float(sums[i] / count), 4) for i in range(len(slots))}
    unique = {slots[i]: round(float(final[covered][:, i].mean()), 4) for i in range(len(slots))}
    stats_file = os.path.join(stats_dir, recipe["model"].split("/")[-1].replace(".gltf", ".txt"))
    stats = read_stats(stats_file, name)
    checks = {}
    for cname, spec in recipe.get("expect", {}).items():
        if cname == "source":
            continue
        e = expected(spec, stats)
        built = sum(fractions[c] for c in cname.split("+"))
        checks[cname] = {"expected": round(e, 4), "built": round(built, 4),
                         "difference_points": round(100 * (built - e), 2)}
    info = {"base_map": maps["base_map"], "size": [w, h], "islands": len(found),
            "groups": dict(collections.Counter(g for g, _ in found)),
            "fractions_island_weighted": fractions, "fractions_unique_texels": unique,
            "excluded_fraction": round(excl / count, 4), "checks": checks, "stats_file": stats_file}
    if review:
        os.makedirs(review, exist_ok=True)
        colours = np.array([COLOURS.get(c, [128, 128, 128]) for c in slots], np.float64)
        panel_mask = (q[..., :len(slots)] @ colours).clip(0, 255)
        panel_mask[~covered] *= 0.25
        panel_final = (final[..., :len(slots)] @ colours).clip(0, 255)
        panel_final[~covered] = base[~covered] * 0.2
        lum = base.mean(-1, keepdims=True)
        panel_tint = 0.55 * panel_final + 0.45 * np.repeat(lum, 3, -1) * 1.6
        panel_tint[~covered] = base[~covered] * 0.2
        panels = [base, panel_mask, panel_final, panel_tint.clip(0, 255)]
        sheet = np.concatenate([np.pad(p, ((0, 0), (0, 6), (0, 0)), constant_values=24) for p in panels], 1)
        Image.fromarray(sheet.astype(np.uint8)).save(os.path.join(review, name.replace("\\", "_").replace(" ", "_") + ".png"))
    return rgba, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", required=True)
    ap.add_argument("--recipes", required=True)
    ap.add_argument("--stats", required=True, help="island_stats/ (island_stats.py's text per model)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--review")
    ap.add_argument("--shader", action="append", default=[],
                    help="build only these shaders of the recipes (repeat); the others' masks and report entries are kept")
    a = ap.parse_args()
    recipes = json.load(open(a.recipes))
    tol = recipes.get("tolerance_points", 3.0)
    masks = os.path.join(a.out, "tags", "materials", "masks")
    os.makedirs(masks, exist_ok=True)
    # (the report of masks built before, from other recipes or maps, is kept: one mod holds them all)
    report_path = os.path.join(a.out, "class_masks.json")
    report = {"tolerance_points": tol, "shaders": {}}
    if os.path.exists(report_path):
        report["shaders"] = json.load(open(report_path)).get("shaders", {})
    ok = True
    for name, recipe in recipes["shaders"].items():
        if a.shader and name not in a.shader:
            continue
        # (a recipe's "extract": the map its model is in, beside --extract, when it is not that one)
        extract = a.extract
        if recipe.get("extract"):
            extract = os.path.join(os.path.dirname(os.path.normpath(a.extract)), recipe["extract"])
            if not os.path.exists(os.path.join(extract, recipe["model"])):
                extract = os.path.join(extract, recipe["extract"])   # (map_extract's <out>/<map>/ layout)
        rgba, info = build(extract, name, recipe, a.stats, a.review)
        info["extract"] = os.path.basename(os.path.normpath(extract))
        file = "%s_%d.png" % (name.replace("\\", "_").replace(" ", "_"), recipe.get("bitmap_index", 0))
        Image.fromarray(rgba, "RGBA").save(os.path.join(masks, file))
        info["file"] = "masks/" + file
        info["pass"] = all(abs(c["difference_points"]) <= tol for c in info["checks"].values())
        ok &= info["pass"]
        report["shaders"][name] = info
    report["pass"] = all(v.get("pass", False) for v in report["shaders"].values())
    ok = ok and report["pass"]
    with open(os.path.join(a.out, "mod.toml"), "w") as f:
        f.write('name = "PRISM class masks (local)"\nid = "prism_class_masks"\nversion = "0.1"\n'
                'description = "Built on this machine by class_masks.py from your own maps: the material overlay\'s '
                'per-texel classes. Not for sharing."\n')
    with open(report_path, "w") as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
