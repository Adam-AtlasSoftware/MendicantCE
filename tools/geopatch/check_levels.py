#!/usr/bin/env python3
"""The per-level gates of a content build (PLAN_FINAL.md sections 2.2 and 2.6; geopatch M5, M5b), run on the farm
against a build_levels.py build:

    check_levels.py <build folder> --maps <retail maps folder> [--map a10 ...] [--jobs N] [--native-dir DIR]
                    [--tolerance 0.1] [--wall-tolerance 0.25] [--out checks.json]

For each map with a sidecar:
- lint: lint_patch.py's checks with the map (our sections only, REFs where required, no retail value as a literal,
  the nudge rule) and the game's reader;
- decode: every BSP's GEOM and LMUV decoded by the game's own code (native geom: prism_geom.c against the map's
  stock vertices), which must take every one (a refusal is a finding);
- deviation (the gameplay rule of PLAN_FINAL.md section 10: the enhanced geometry stays close to the original where
  gameplay depends on it), measured as the build holds to it (clamp.py: one definition for both): every corner of a
  near triangle of the decoded remodel by its signed distance to its stock triangle's plane (positive: in front of
  the surface, toward the player; a triangle lying outside its listed stock triangle, the remodel's seam slivers,
  corner by corner against the nearest stock triangle), split into ground (the stock surface faces up: its normal's
  z at least clamp.GROUND_Z, about 45 degrees, where one walks and stands) and walls (walls, cover, ceilings), the
  walls past clamp.WALL_SCAN further into those within reach (stock ground within clamp.REACH_RADIUS across, the
  wall from clamp.REACH_BELOW below it to clamp.REACH above) and those out of reach (cliff faces, high ceilings:
  the remodel's look, reported). The ground's largest move either way is checked against --tolerance (0.1 wu,
  section 2.2's budget), the walls within reach against --wall-tolerance (0.25 wu);
- layout (LMUV v2, our own lightmap layout): pages per BSP (the decoder takes at most LMUV_PAGES_MOST, and the
  renderer's page override needs the BSP's stock lightmap pages plus ours under 127: prism_deferred.c), the largest
  part's vertex count (16-bit indices: at most 65535), charts and stretch (encode.py's report).

Exit 0 when every map is clean (lint, decode, layout, ground and walls within reach within tolerance), else 1. The
farm only (it reads the retail maps); the JSON holds numbers and positions, nothing of the maps.
"""
import argparse
import json
import multiprocessing
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

LMUV_PAGES_MOST = 64         # prism_geom.c
PAGE_OVERRIDE_MOST = 126     # prism_deferred.c: the stock pages + ours + 1 <= 127
MOST_VERTICES = 0xFFFF


def _check(task):
    from geopatch import clamp, encode, lint_patch, lmuv, native as native_module, prsx, retail
    build, name, map_path, native_dir, tolerance, wall_tolerance = task
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
            row["deviation"] = clamp.measure(parts_image, clamp.Stock.of_bsp(by_index[b], native), tolerance,
                                             wall_tolerance)
        out["bsps"][b] = row
    # (only this level's folder: checks_tmp itself is removed by main once every worker is done, never by a worker,
    # which could take it from under another worker's mkdir)
    work.rmdir()
    return name, out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build")
    parser.add_argument("--maps", required=True)
    parser.add_argument("--map", action="append")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--native-dir")
    parser.add_argument("--tolerance", type=float, default=0.1)
    parser.add_argument("--wall-tolerance", type=float, default=0.25)
    parser.add_argument("--out")
    args = parser.parse_args(argv)
    levels = json.loads((Path(args.build) / "build.json").read_text("utf-8"))["levels"]
    names = [n for n in sorted(levels) if levels[n]["sidecar"] and (not args.map or n in args.map)]
    from geopatch import native as native_module
    native_module.build(args.native_dir)
    tasks = [(args.build, n, str(Path(args.maps) / (n + ".map")), args.native_dir, args.tolerance, args.wall_tolerance)
             for n in names]
    results, dirty = {}, 0
    # (spawned workers: Python 3.14's default, the fork server, binds a socket under TMPDIR, which a deep run folder
    # makes too long a path; spawn needs none, and is what every platform can do)
    with ProcessPoolExecutor(max_workers=max(1, args.jobs), mp_context=multiprocessing.get_context("spawn")) as pool:
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
                if dev and dev["walls_in_reach"]["most"] > args.wall_tolerance:
                    bad.append("bsp %s a wall within reach moves %.3f wu (%d corners past %.2f)" %
                               (b, dev["walls_in_reach"]["most"], dev["walls_in_reach_over_tolerance"],
                                args.wall_tolerance))
            out["clean"] = not bad
            out["problems"] = bad
            dirty += bool(bad)
            print("%s %s" % (name, "clean" if not bad else "; ".join(bad[:6]) + (" ..." if len(bad) > 6 else "")),
                  flush=True)
    try:
        (Path(args.build) / "checks_tmp").rmdir()
    except OSError:
        pass
    from geopatch import clamp
    text = json.dumps({"tolerance": args.tolerance, "wall_tolerance": args.wall_tolerance, "ground_z": clamp.GROUND_Z,
                       "reach": clamp.REACH, "levels": results}, indent=1, sort_keys=True) + "\n"
    if args.out:
        Path(args.out).write_text(text, "utf-8", newline="\n")
    return 0 if not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
