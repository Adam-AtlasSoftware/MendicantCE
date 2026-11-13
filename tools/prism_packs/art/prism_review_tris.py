#!/usr/bin/env python3
"""Triangles of a mod model whose corners follow bones the map's mesh never joins in one triangle.

Each strip is decoded to triangles (degenerates dropped); a corner's bone is its heavier weight. A pair
of bones joined by a mod triangle but by no map triangle (and not parent and child) stretches into a
spike when the two bones move apart (e.g. a foot and a pelvis).

usage: prism_review_tris.py <mod tag> <map tag>
"""
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_model_check import TagModel, VERTEX_SIZE  # noqa: E402


def tris(m):
    for gi, parts in enumerate(m.geometries):
        for pi, p in enumerate(parts):
            idx = [struct.unpack_from(">H", m.b, p["to"] + 2 * k)[0] for k in range(p["nt"] * 3)]
            while idx and idx[-1] == 0xFFFF:
                idx.pop()
            vtx = []
            for k in range(p["nv"]):
                o = p["vo"] + k * VERTEX_SIZE
                pos = struct.unpack_from(">3f", m.b, o)
                n0, n1 = struct.unpack_from(">hh", m.b, o + 0x38)
                w0, w1 = struct.unpack_from(">ff", m.b, o + 0x3C)
                dom = n0 if (n1 < 0 or w0 >= w1) else n1
                vtx.append((pos, dom))
            for k in range(len(idx) - 2):
                a, b, c = idx[k], idx[k + 1], idx[k + 2]
                if a == b or b == c or a == c:
                    continue
                yield gi, pi, (a, b, c), [vtx[a], vtx[b], vtx[c]]


def main():
    mod, ref = TagModel(sys.argv[1]), TagModel(sys.argv[2])
    names = [n["name"] for n in ref.nodes]
    parent = {i: n["parent"] for i, n in enumerate(ref.nodes)}

    def pairs_of(m):
        c = collections.Counter()
        for gi, pi, ids, v in tris(m):
            ns = sorted({x[1] for x in v})
            for i in range(len(ns)):
                for j in range(i + 1, len(ns)):
                    c[(ns[i], ns[j])] += 1
        return c
    pr = pairs_of(ref)
    out = collections.defaultdict(lambda: dict(count=0, sample=[]))
    for gi, pi, ids, v in tris(mod):
        ns = sorted({x[1] for x in v})
        for i in range(len(ns)):
            for j in range(i + 1, len(ns)):
                a, b = ns[i], ns[j]
                if (a, b) in pr or parent.get(a) == b or parent.get(b) == a:
                    continue
                e = out["%s+%s" % (names[a], names[b])]
                e["count"] += 1
                if len(e["sample"]) < 5:
                    e["sample"].append(dict(part="%d/%d" % (gi, pi), v=list(ids),
                                            pos=[[round(x, 4) for x in q[0]] for q in v]))
    print(json.dumps(dict(map_pairs=len(pr), only_mod=out), indent=1))


if __name__ == "__main__":
    main()
