#!/usr/bin/env python3
"""PRISM v2 review: a mod model tag against the map's own (freshly extracted), exactly.

Nodes (name, hierarchy, default translation/rotation, distance) and every marker instance (model and
permutation markers) must be bit-equal, not only within the game's 0.001; the per-part engine limits
come from mod_model_check.py. Prints one JSON line.

usage: prism_review_tags.py <mod tag> <map tag>
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_model_check import TagModel, part_checks  # noqa: E402


def bits(v):
    return [struct.pack(">f", x) for x in v]


def flat(m):
    out = []
    for nm, inst in m.markers:
        for (ri, pi, node, t, q) in inst:
            out.append((nm, ri, pi, node, tuple(bits(t)), tuple(bits(q))))
    for ri, r in enumerate(m.regions):
        for pi, p in enumerate(r["perms"]):
            for (nm, node, q, t) in p["markers"]:
                out.append((nm, ri, pi, node, tuple(bits(t)), tuple(bits(q))))
    return sorted(out)


def main():
    mod, ref = TagModel(sys.argv[1]), TagModel(sys.argv[2])
    nodes_equal = [(a["name"], a["sib"], a["child"], a["parent"], bits(a["t"]), bits(a["q"]), bits([a["dist"]]))
                   for a in mod.nodes] == \
                  [(a["name"], a["sib"], a["child"], a["parent"], bits(a["t"]), bits(a["q"]), bits([a["dist"]]))
                   for a in ref.nodes]
    fm, fr = flat(mod), flat(ref)
    regions = lambda m: [(r["name"], [(p["name"], p["flags"]) for p in r["perms"]]) for r in m.regions]
    tris = lambda m, lod: sum(p["nt"] for g in m.geometries for p in g)
    parts = [p for g in mod.geometries for p in g]
    v = dict(mod=sys.argv[1], ref=sys.argv[2],
             nodes=dict(count=len(mod.nodes), bit_equal=nodes_equal),
             markers=dict(instances=len(fm), map_instances=len(fr), bit_equal=fm == fr),
             regions_equal=regions(mod) == regions(ref),
             shaders_mod=mod.shaders, shaders_map=ref.shaders,
             cutoffs=dict(mod=mod.cutoffs, map=ref.cutoffs),
             max_part_vertices=max(p["nv"] for p in parts), max_part_strip=max(p["nt"] for p in parts),
             strip_elements=dict(mod=tris(mod, 0), map=tris(ref, 0)),
             part_checks=part_checks(mod) if callable(part_checks) else None)
    print(json.dumps(v, default=str))


if __name__ == "__main__":
    main()
