"""PRISM baked shadows: the remodel's geometry read from a .pwg (prism_world_geometry.c's format,
art/world_geo/wg_common.py's write_pwg), for the sun mask's bake (sunmask_bake.py).

read_pwg(path) -> {(lightmap_index, material_index): [part, ...]}, each part a dict of
  pos (n x 3), nrm (n x 3), uv1 (n x 2: the lightmap coordinates, as the map has them),
  tri (near triangles, m x 3 int64, indices into the part's vertices).
Pure numpy; no Blender."""
import struct

import numpy as np


def _pad4(n):
    return (n + 3) & ~3


def read_pwg(path):
    data = open(path, "rb").read()
    if data[:4] != b"PWG1":
        raise ValueError(f"{path}: not a .pwg")
    version, count, flags = struct.unpack_from("<III", data, 4)
    lod = bool(flags & 1)
    at = 64
    out = {}
    for _ in range(count):
        lmi, mi, s_count, v_count, n_vert, n_tri, topo = struct.unpack_from("<hhIIIII", data, at)
        posc, first, scount = struct.unpack_from("<dII", data, at + 24)
        near_count, distance = struct.unpack_from("<If", data, at + 40)
        at += 64
        at += _pad4((scount + 1) * 4)
        verts = np.frombuffer(data, "<f4", n_vert * 14, at).reshape(n_vert, 14)
        at += _pad4(n_vert * 56)
        lm = np.frombuffer(data, "<f4", n_vert * 5, at).reshape(n_vert, 5)
        at += _pad4(n_vert * 20)
        tri = np.frombuffer(data, "<u2", n_tri * 3, at).reshape(n_tri, 3).astype(np.int64)
        at += _pad4(n_tri * 6)
        if lod:
            at += scount * 8 + scount * 16
        near = near_count if lod and near_count else n_tri
        out.setdefault((lmi, mi), []).append({
            "pos": verts[:, 0:3].astype(np.float64), "nrm": verts[:, 3:6].astype(np.float64),
            "uv1": lm[:, 3:5].astype(np.float64), "tri": tri[:near].copy(),
            "first_surface": first, "surfaces": scount})
    return out
