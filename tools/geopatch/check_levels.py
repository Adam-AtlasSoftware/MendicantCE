#!/usr/bin/env python3
"""The per-level gates of a content build (PLAN_FINAL.md sections 2.2 and 2.6; geopatch M5), run on the farm against
a build_levels.py build:

    check_levels.py <build folder> --maps <retail maps folder> [--map a10 ...] [--jobs N] [--native-dir DIR]
                    [--tolerance 0.1] [--out checks.json]

For each map with a sidecar:
- lint: lint_patch.py's checks with the map (our sections only, REFs where required, no retail value as a literal,
  the nudge rule) and the game's reader;
- decode: every BSP's GEOM and LMUV decoded by the game's own code (native geom: prism_geom.c against the map's
  stock vertices), which must take every one (a refusal is a finding);
- deviation (the gameplay rule of PLAN_FINAL.md section 10: the enhanced geometry stays close to the original where
  gameplay depends on it): every near triangle of the decoded remodel belongs to one stock surface; each of its
  corners' signed distance to that stock triangle's plane (positive: in front of the surface, toward the player),
  split into ground (the stock surface faces up: its normal's z at least GROUND_Z, about 45 degrees, where one walks
  and stands) and the rest (walls, cover, ceilings). The ground's largest move either way is checked against
  --tolerance (0.1 wu, section 2.2's budget); walls are reported, not gated (their relief is the remodel's look and
  the plan's tolerance for cover is the collision milestone's, M8);
- layout (LMUV v2, our own lightmap layout): pages per BSP (the decoder takes at most LMUV_PAGES_MOST, and the
  renderer's page override needs the BSP's stock lightmap pages plus ours under 127: prism_deferred.c), the largest
  part's vertex count (16-bit indices: at most 65535), charts and stretch (encode.py's report).

Exit 0 when every map is clean (lint, decode, layout, ground within tolerance), else 1. The farm only (it reads the
retail maps); the JSON holds numbers and positions, nothing of the maps.
"""
import argparse
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

GROUND_Z = 0.7071
LMUV_PAGES_MOST = 64         # prism_geom.c
PAGE_OVERRIDE_MOST = 126     # prism_deferred.c: the stock pages + ours + 1 <= 127
MOST_VERTICES = 0xFFFF


def _summary(values):
    """largest above, largest below, largest either way, the 99.9th percentile of |d|, count"""
    if not len(values):
        return {"count": 0, "above": 0.0, "below": 0.0, "most": 0.0, "p999": 0.0}
    a = np.abs(values)
    return {"count": int(len(values)), "above": round(float(max(values.max(), 0.0)), 4),
            "below": round(float(max(-values.min(), 0.0)), 4), "most": round(float(a.max()), 4),
            "p999": round(float(np.quantile(a, 0.999)), 4)}


def deviation(image_parts, stock_bsp, native, tolerance):
    """{ground, walls, ground_over, worst_ground_at} of one BSP's decoded remodel against its stock surfaces"""
    ground, walls = [], []
    worst = (0.0, None)
    over = 0
    decoded = {}
    for part in image_parts:
        key = (part.lightmap_index, part.material_index)
        material = stock_bsp.materials.get(key)
        if material is None or not len(part.triangles):
            continue
        if key not in decoded:
            env = np.frombuffer(native.environment_vertices(material.vertices), "<f4").reshape(-1, 14)
            decoded[key] = (env[:, 0:3].astype(np.float64), env[:, 3:6].astype(np.float64),
                            np.array(material.surfaces, np.int64).reshape(-1, 3))
        positions, normals, surfaces = decoded[key]
        near = int(part.near_triangle_count)
        sft = part.sft.astype(np.int64)
        counts = np.diff(sft)
        if not near or not len(counts):
            continue
        surface = int(part.first_surface) + np.repeat(np.arange(len(counts)), counts)[:near]
        tri = surfaces[surface]
        a, b, c = positions[tri[:, 0]], positions[tri[:, 1]], positions[tri[:, 2]]
        n = np.cross(b - a, c - a)
        length = np.sqrt((n[:, 0] * n[:, 0] + n[:, 1] * n[:, 1]) + n[:, 2] * n[:, 2])
        ok = length > 1e-9
        n[ok] /= length[ok, None]
        # (the face's front: the way its vertices' own normals look)
        vn = normals[tri[:, 0]] + normals[tri[:, 1]] + normals[tri[:, 2]]
        flip = ((n * vn).sum(axis=1) < 0)
        n[flip] *= -1.0
        p = part.vertices[:, 0:3].astype(np.float64)[part.triangles[:near]]  # (near, 3 corners, 3)
        d = ((p - a[:, None, :]) * n[:, None, :]).sum(axis=2)  # (near, 3)
        is_ground = ok & (n[:, 2] >= GROUND_Z)
        is_wall = ok & ~is_ground
        g = d[is_ground].reshape(-1)
        ground.append(g)
        walls.append(d[is_wall].reshape(-1))
        if len(g):
            over += int((np.abs(g) > tolerance).sum())
            k = int(np.argmax(np.abs(g)))
            if abs(g[k]) > worst[0]:
                corner = p[is_ground].reshape(-1, 3)[k]
                worst = (float(abs(g[k])), [round(float(x), 3) for x in corner])
    g = np.concatenate(ground) if ground else np.zeros(0)
    w = np.concatenate(walls) if walls else np.zeros(0)
    return {"ground": _summary(g), "walls": _summary(w), "ground_over_tolerance": over,
            "ground_share_over": round(over / max(1, len(g)), 6), "worst_ground_at": worst[1]}


def _check(task):
    from geopatch import encode, lint_patch, lmuv, native as native_module, prsx, retail
    build, name, map_path, native_dir, tolerance = task
    build = Path(build)
    record = json.loads((build / "build.json").read_text("utf-8"))["levels"][name]
    sidecar_path = build / record["sidecar"]["file"]
    data = sidecar_path.read_bytes()
    native = native_module.Native(native_dir)
    _, _, bsps = retail.read_map(map_path)
    by_index = {b.index: b for b in bsps}
    sets = retail.RetailSets()
    for bsp in bsps:
        sets.add_bsp(bsp, windows=False)
    findings = lint_patch.lint(data, by_index, sets, native, str(sidecar_path))
    out = {"lint": findings, "bsps": {}}
    report = json.loads((build / "reports" / (name + ".report.json")).read_text("utf-8"))
    sidecar = prsx.Sidecar(data)
    # (this level's own scratch folder: the levels run in parallel)
    work = build / "checks_tmp" / name
    work.mkdir(parents=True, exist_ok=True)
    for entry_index, entry in enumerate(sidecar.entries):
        if entry.kind != "GEOM":
            continue
        b = entry.bsp
        stock = work / ("%s_%d.stock" % (name, b))
        native_module.write_stock(stock, by_index[b])
        image, line = native.geom(stock, sidecar_path, b)
        stock.unlink()
        row = {"decode": "ok" if image is not None else "refused: " + line, "decode_report": line}
        lm_index = [i for i, e in enumerate(sidecar.entries) if e.kind == "LMUV" and e.bsp == b][0]
        pages, parts = lmuv.decode(sidecar.section(lm_index))
        lightmaps = 1 + max((key[0] for key in by_index[b].materials), default=-1)
        lm_report = report["bsps"][str(b)].get("lmuv", {})
        row["layout"] = {"pages": len(pages), "stock_lightmaps": lightmaps, "parts": len(parts),
                         "most_part_vertices": max((len(c) for _, c in parts), default=0),
                         "charts": lm_report.get("charts"), "stretch_most": round(lm_report.get("stretch_most", 0), 4),
                         "stretch_mean": round(lm_report.get("stretch_mean", 0), 4),
                         "area_stretched_over_2": round(lm_report.get("area_stretched_over_2", 0), 6),
                         "densities": lm_report.get("densities"),
                         "vertices": lm_report.get("vertices")}
        problems = []
        if len(pages) > LMUV_PAGES_MOST:
            problems.append("%d pages, the decoder takes %d" % (len(pages), LMUV_PAGES_MOST))
        if lightmaps + len(pages) > PAGE_OVERRIDE_MOST:
            problems.append("%d stock + %d own pages past %d" % (lightmaps, len(pages), PAGE_OVERRIDE_MOST))
        if row["layout"]["most_part_vertices"] > MOST_VERTICES:
            problems.append("a part of %d vertices" % row["layout"]["most_part_vertices"])
        row["layout"]["problems"] = problems
        if image is not None:
            tmp = work / ("%s_%d.pwg" % (name, b))
            tmp.write_bytes(image)
            parts_image = encode.read_pwg(tmp)
            tmp.unlink()
            row["deviation"] = deviation(parts_image, by_index[b], native, tolerance)
        out["bsps"][b] = row
    work.rmdir()
    try:
        work.parent.rmdir()
    except OSError:
        pass
    return name, out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build")
    parser.add_argument("--maps", required=True)
    parser.add_argument("--map", action="append")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--native-dir")
    parser.add_argument("--tolerance", type=float, default=0.1)
    parser.add_argument("--out")
    args = parser.parse_args(argv)
    levels = json.loads((Path(args.build) / "build.json").read_text("utf-8"))["levels"]
    names = [n for n in sorted(levels) if levels[n]["sidecar"] and (not args.map or n in args.map)]
    from geopatch import native as native_module
    native_module.build(args.native_dir)
    tasks = [(args.build, n, str(Path(args.maps) / (n + ".map")), args.native_dir, args.tolerance) for n in names]
    results, dirty = {}, 0
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for name, out in pool.map(_check, tasks):
            results[name] = out
            bad = list(out["lint"])
            for b, row in out["bsps"].items():
                if row["decode"] != "ok":
                    bad.append("bsp %s decode %s" % (b, row["decode"]))
                bad += ["bsp %s layout: %s" % (b, p) for p in row["layout"]["problems"]]
                dev = row.get("deviation")
                if dev and dev["ground"]["most"] > args.tolerance:
                    bad.append("bsp %s ground moves %.3f wu (%d corners past %.2f)" %
                               (b, dev["ground"]["most"], dev["ground_over_tolerance"], args.tolerance))
            out["clean"] = not bad
            out["problems"] = bad
            dirty += bool(bad)
            print("%s %s" % (name, "clean" if not bad else "; ".join(bad[:6]) + (" ..." if len(bad) > 6 else "")),
                  flush=True)
    text = json.dumps({"tolerance": args.tolerance, "ground_z": GROUND_Z, "levels": results}, indent=1,
                      sort_keys=True) + "\n"
    if args.out:
        Path(args.out).write_text(text, "utf-8", newline="\n")
    return 0 if not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
