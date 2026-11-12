#!/usr/bin/env python3
"""Drop the triangles of a gbxmodel tag whose corners follow two bones that the map's model never joins in
one triangle and that are not parent and child (prism_review_tris.py finds them: e.g. a sole welded across
to the other foot, an upper and a lower mandible of opposite sides). Each one turns into a long spike once
the bones part.

A strip triangle at position k (indices k, k+1, k+2) is dropped by repeating index k+1 twice after it: the
triangles before and after keep their places' parity, so their winding is unchanged; the strip grows by two
indices per dropped triangle and is padded to whole elements with 0xFFFF. The tag's blocks follow one another,
so the part's strip count is updated and the bytes after it shift.

usage: prism_strip_cull.py <in tag> <map tag> <out tag>
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_model_check import TagModel, VERTEX_SIZE, STRIP_ELEMENT_SIZE  # noqa: E402
from prism_review_tris import tris  # noqa: E402


def main():
    src, ref_path, dst = sys.argv[1:4]
    mod, ref = TagModel(src), TagModel(ref_path)
    parent = {i: n["parent"] for i, n in enumerate(ref.nodes)}
    joined = set()
    for gi, pi, ids, v in tris(ref):
        ns = sorted({x[1] for x in v})
        for i in range(len(ns)):
            for j in range(i + 1, len(ns)):
                joined.add((ns[i], ns[j]))

    def bad(dom):
        ns = sorted(set(dom))
        for i in range(len(ns)):
            for j in range(i + 1, len(ns)):
                a, b = ns[i], ns[j]
                if (a, b) not in joined and parent.get(a) != b and parent.get(b) != a:
                    return True
        return False

    b = bytearray(mod.b)
    edits = []  # (offset of the part's strip data, old byte length, new bytes, part offset)
    report = []
    for gi, parts in enumerate(mod.geometries):
        for pi, p in enumerate(parts):
            idx = [struct.unpack_from(">H", b, p["to"] + 2 * k)[0] for k in range(p["nt"] * 3)]
            while idx and idx[-1] == 0xFFFF:
                idx.pop()
            dom = []
            for k in range(p["nv"]):
                o = p["vo"] + k * VERTEX_SIZE
                n0, n1 = struct.unpack_from(">hh", b, o + 0x38)
                w0, w1 = struct.unpack_from(">ff", b, o + 0x3C)
                dom.append(n0 if (n1 < 0 or w0 >= w1) else n1)
            drop = [k for k in range(len(idx) - 2)
                    if len({idx[k], idx[k + 1], idx[k + 2]}) == 3 and bad([dom[idx[k]], dom[idx[k + 1]], dom[idx[k + 2]]])]
            if not drop:
                continue
            ds = set(drop)
            # after index k+1 of a dropped triangle at k, that index twice more
            new = []
            for k, x in enumerate(idx):
                new.append(x)
                if (k - 1) in ds:
                    new += [x, x]
            while len(new) % 3:
                new.append(0xFFFF)
            data = struct.pack(">%dH" % len(new), *new)
            edits.append((p["to"], p["nt"] * STRIP_ELEMENT_SIZE, data, p["off"], len(new) // 3))
            report.append(dict(geometry=gi, part=pi, dropped=len(drop), strip_elements=[p["nt"], len(new) // 3]))
    for to, old_len, data, off, n_el in sorted(edits, reverse=True):
        if n_el > 65535:
            raise SystemExit("strip past 65535 elements")
        b[to:to + old_len] = data
        struct.pack_into(">I", b, off + 0x38, n_el)
    Path(dst).write_bytes(bytes(b))
    check = TagModel(dst)
    assert check.trailing == 0
    print(json.dumps(dict(parts=report), indent=1))


if __name__ == "__main__":
    main()
