"""PRISM world geometry: a .pwg's digests, for checking that the remodel builds the same file on every machine (a
shipped light bake of the remodel is valid only where the player's own build gives the same geometry).

usage: python3 wg_pwg_hash.py <a.pwg> [<b.pwg>]
  one file: its digests, as JSON: the whole file's sha256, and per part the geometry's (positions, base and lightmap
  coordinates, triangles, surfaces: what the light bake's geometry and BSP hashes read) and the rest's (normals,
  tangent frames, incident radiosity: drawn, not traced)
  two files: the first difference, by part and field; exit 0 when the files are byte for byte the same, 1 when
  only the drawn fields differ (the geometry is the same), 2 when the geometry differs

Plain Python and numpy; reads the format wg_common.write_pwg writes (prism_world_geometry.c).
"""
import hashlib
import json
import struct
import sys

import numpy as np


def _pad4(n):
    return n + (-n % 4)


def parts(data):
    """the file's parts: [{field: bytes}]"""
    magic, version, count, lod = data[:4], *struct.unpack_from("<III", data, 4)
    assert magic == b"PWG1", "not a .pwg"
    at = 64
    out = []
    for _ in range(count):
        # (each part: its 64-byte head, its surfaces' first triangles, its vertices (position, normal, binormal,
        # tangent, base coordinates: 14 floats), their incident radiosity and lightmap coordinates (5), its triangles
        # (3 u16 each), and with a far level each surface's (first, count) and bounding sphere)
        (lmi, mi, surfaces, stock_vertices, nv, nt, topo) = struct.unpack_from("<hhIIIII", data, at)
        posc, first, scount = struct.unpack_from("<dII", data, at + 24)
        near_count, = struct.unpack_from("<I", data, at + 40)
        head = data[at:at + 64]
        at += 64
        sft = data[at:at + 4 * (scount + 1)]
        at += _pad4(4 * (scount + 1))
        verts = np.frombuffer(data, "<f4", nv * 14, at).reshape(nv, 14)
        at += _pad4(nv * 14 * 4)
        lm = np.frombuffer(data, "<f4", nv * 5, at).reshape(nv, 5)
        at += _pad4(nv * 5 * 4)
        tri = data[at:at + 6 * nt]
        at += _pad4(6 * nt)
        extra = b""
        if version >= 2 and lod:
            extra = data[at:at + scount * 8 + scount * 16]
            at += scount * 8 + scount * 16
        out.append({
            "head": head,
            "surfaces": sft,
            "positions": verts[:, 0:3].tobytes(),
            "base_uv": verts[:, 12:14].tobytes(),
            "lightmap_uv": lm[:, 3:5].tobytes(),
            "triangles": tri,
            "far": extra,
            "normals": verts[:, 3:12].tobytes(),
            "radiosity": lm[:, 0:3].tobytes(),
        })
    assert at == len(data), "a .pwg whose parts do not end at its end"
    return out


GEOMETRY = ("head", "surfaces", "positions", "base_uv", "lightmap_uv", "triangles", "far")
DRAWN = ("normals", "radiosity")


def digests(path):
    data = open(path, "rb").read()
    ps = parts(data)
    geometry = hashlib.sha256()
    drawn = hashlib.sha256()
    for p in ps:
        for f in GEOMETRY:
            geometry.update(p[f])
        for f in DRAWN:
            drawn.update(p[f])
    return {"file": hashlib.sha256(data).hexdigest(), "geometry": geometry.hexdigest(), "drawn": drawn.hexdigest(),
            "parts": len(ps), "bytes": len(data)}


def compare(a, b):
    da, db = open(a, "rb").read(), open(b, "rb").read()
    if da == db:
        return 0, "the same, byte for byte"
    pa, pb = parts(da), parts(db)
    if len(pa) != len(pb):
        return 2, "parts: %d against %d" % (len(pa), len(pb))
    worst, first = 0, None
    for i, (x, y) in enumerate(zip(pa, pb)):
        for f in GEOMETRY + DRAWN:
            if x[f] != y[f]:
                level = 2 if f in GEOMETRY else 1
                if level > worst:
                    worst, first = level, "part %d's %s (%d against %d bytes)" % (i, f, len(x[f]), len(y[f]))
    return worst, first or "the header differs"


def main(argv):
    if len(argv) == 2:
        print(json.dumps(digests(argv[1])))
        return 0
    if len(argv) == 3:
        code, what = compare(argv[1], argv[2])
        print(what)
        return code
    print(__doc__)
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
