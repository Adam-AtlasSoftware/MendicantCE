#!/usr/bin/env python3
"""PRISM v4 world geometry: a built level remodel checked, its floating facets and needles moved back, and written as
.pwg version 3 (seal groups, each surface's deviation, the remodel's own lightmap atlas: wg3_pwg.py).

usage: python3 art/world_geo/wg3_build.py <bsp.gltf> <in.pwg> <out.pwg> <report.json>

  1. The shape (wg3_pwg.check_shape): every remodel vertex, near and far, within its stock triangle's bounds: no needle
     (further from the triangle than 0.1 wu or its longest edge, whichever is more) and nothing further off its
     plane or past its edges than 0.9 wu (the engine's bounds are a world unit). What is outside is moved back
     (wg3_pwg.fix_shape: every row at a position alike, so no edge opens), and checked again.
  2. The open edges (wg2_cracks.py's seams, in every mode the engine draws: near, far and the two mixed): none the
     input does not have.
  3. Seal groups, deviations and the atlas (xatlas), written as version 3, read back and its groups checked: every
     stock edge between a group and the rest the stock edge on both sides, at both levels.

Exit status: 0 the file is written; 4 the shape still fails (a facet or needle left); 5 an open edge the input lacks;
6 a seal group whose border is not the stock edges. Nothing is written but on 0. Everything it reads and writes is
derived from the player's map: local only.
"""
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wg_common as wg  # noqa: E402
import wg3_pwg as v3  # noqa: E402
from wg2_cracks import MODES, new_edges, seams  # noqa: E402

T0 = time.time()


def log(*a):
    print("WGB %7.1fs" % (time.time() - T0), *a, flush=True)


def surface_name(level, s):
    p = level.parts[level.owner[s]]
    m = level.by[(p["lm"], p["mi"])]
    return "lightmap %d material %d (%s) surface %d" % (p["lm"], p["mi"], m.shader.split("\\")[-1],
                                                         p["pfs"] + level.local[s])


def main():
    gltf, src, dst, report_path = sys.argv[1:5]
    # (the atlas's xatlas asked for first: a missing or old one stops the step before the checks' work, not after)
    v3.need_xatlas()
    _, mats = wg.load_bsp(gltf)
    f = v3.read(src)
    report = dict(src=src, version_in=f["version"])
    level = v3.Level(mats, f["parts"])
    log("read", src, len(f["parts"]), "parts", level.N, "surfaces")

    # ---- 1. the shape
    r = v3.check_shape(level)
    found = sorted(set(r["needle"][0].tolist()) | set(r["cap"][0].tolist()))
    report["shape_in"] = dict(needle_vertices=int(len(r["needle"][0])), cap_vertices=int(len(r["cap"][0])),
                              surfaces=[surface_name(level, s) for s in found])
    log("shape: needles", len(r["needle"][0]), "past the bounds", len(r["cap"][0]), "surfaces", len(found))
    before = {mode: seams(mats, [dict(p) for p in level.parts], mode) for mode in MODES} if found else None
    if found:
        fixed = {}
        v3.fix_shape(level, mats, report=fixed)
        report["shape_fix"] = fixed
        log("moved", fixed["positions_moved"], "positions; left", fixed["needles_left"], "needles,",
            fixed["caps_left"], "past the bounds")
        if fixed["needles_left"] or fixed["caps_left"]:
            json.dump(report, open(report_path, "w"), indent=1)
            log("FAILED: the shape still has floating facets or needles")
            sys.exit(4)

        # ---- 2. no open edge the input lacks
        new_total = 0
        report["open_edges"] = {}
        for mode in MODES:
            after = seams(mats, level.parts, mode)
            new = new_edges(before[mode], after)
            report["open_edges"][mode] = dict(input=int((before[mode]["kind"] != "border").sum()),
                                              output=int((after["kind"] != "border").sum()), new=int(len(new)))
            new_total += len(new)
        log("open edges", json.dumps(report["open_edges"]))
        if new_total:
            json.dump(report, open(report_path, "w"), indent=1)
            log("FAILED: the moves opened", new_total, "edges")
            sys.exit(5)

    # ---- 3. version 3
    plain = v3.plain_sides(level)
    labels, count = v3.seal_groups(level, plain)
    sizes = np.bincount(labels, minlength=count)
    report["seal_groups"] = dict(count=int(count), largest=int(sizes.max()) if count else 0,
                                 median=float(np.median(sizes)) if count else 0, singletons=int((sizes == 1).sum()),
                                 plain_sides=int(plain.sum()), sides=int(plain.size))
    log("seal groups", json.dumps(report["seal_groups"]))
    dev = v3.deviations(level)
    report["deviation_max"] = dev.max(0).round(4).tolist() if len(dev) else []
    log("deviations (above, below, past the edges, far): max", report["deviation_max"])
    per_part, charts, pages = v3.atlas(level)
    report["atlas"] = dict(pages, charts=int(len(charts)))
    log("atlas", json.dumps(report["atlas"]))
    for pi, p in enumerate(level.parts):
        sl = slice(level.first[pi], level.first[pi + 1])
        p["group"] = labels[sl]
        p["dev"] = dev[sl].astype(np.float32)
        p["atlas_chart"], p["atlas_uv"] = per_part[pi]
        if p["bounds"] is None:
            p["bounds"] = np.zeros((p["psc"], 4), np.float32)
    ext = dict(groups=int(count), pages=pages["pages"], page_width=pages["page_width"],
               page_height=pages["page_height"], density=pages["density"])
    size = v3.write_v3(dst + ".new", f["header"], level.parts, ext, charts)

    # ---- the file as written, read back: its groups' borders the stock edges
    g = v3.read(dst + ".new")
    back = v3.Level(mats, g["parts"])
    groups = np.concatenate([p["group"] for p in g["parts"]])
    plain2 = v3.plain_sides(back)
    edge = v3.stock_edges(back)
    side_group = np.repeat(groups, 3)
    order = np.argsort(edge, kind="stable")
    e, gr, pl = edge[order], side_group[order], plain2.ravel()[order]
    bad = 0
    run_start = np.r_[0, np.nonzero(e[1:] != e[:-1])[0] + 1, len(e)]
    for a, b in zip(run_start[:-1], run_start[1:]):
        if b - a > 1 and len(set(gr[a:b].tolist())) > 1 and not pl[a:b].all():
            bad += 1
    report["groups_check"] = dict(border_edges_not_plain=bad)
    if bad:
        os.remove(dst + ".new")
        json.dump(report, open(report_path, "w"), indent=1)
        log("FAILED: seal groups with", bad, "border edges that are not the stock edge")
        sys.exit(6)
    os.replace(dst + ".new", dst)
    report.update(bytes=size, seconds=round(time.time() - T0, 1))
    json.dump(report, open(report_path, "w"), indent=1)
    log("wrote", dst, size, "bytes")


if __name__ == "__main__":
    main()
