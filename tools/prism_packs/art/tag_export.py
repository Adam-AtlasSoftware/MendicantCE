#!/usr/bin/env python3
"""Export a gbxmodel tag's LOD meshes exactly as the game draws them (strip order, parity winding,
degenerates dropped, local node map resolved, base UV scale applied) to .npz, for glitch hunting.

  tag_export.py <tag> <out.npz> [--lod superhigh]

Arrays: co (V,3), nrm (V,3), uv (V,2), nodes (V,2) global (-1 none), w (V,2), tri (T,3) in the game's
winding (the game culls back faces), tri_shader (T,), tri_part (T,) "geometry*100+part", tri_strip_k (T,),
region (T,), nodes_names. Local only when the tag is derived from CE data.
"""
import argparse, struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mod_model_check import TagModel, VERTEX_SIZE, u16, s16  # noqa: E402

LODS = ["superhigh", "high", "medium", "low", "superlow"]


def export(m, lod, perm_name=None):
    b = m.b
    local = m.gbx and (m.flags >> 1) & 1
    us, vs = m.base_scale
    us = us or 1.0; vs = vs or 1.0
    L = 4 - LODS.index(lod)  # the tag lists superlow, low, medium, high, superhigh
    # lods in the tag: superlow..superhigh order is [superlow, low, medium, high, superhigh]? use names
    co, nr, uv, nd, ww = [], [], [], [], []
    tri, tsh, tpart, tk, treg = [], [], [], [], []
    seen = set()
    for ri, reg in enumerate(m.regions):
        perms = reg["perms"]
        pick = perms if perm_name == "*" else ([p for p in perms if (perm_name is None and p is perms[0]) or p["name"] == perm_name] or perms[:1])
        for p in pick:
            gi = p["lods"][L]
            if gi < 0 or gi in seen:
                continue
            seen.add(gi)
            for pi, part in enumerate(m.geometries[gi]):
                base = len(co)
                for k in range(part["nv"]):
                    o = part["vo"] + k * VERTEX_SIZE
                    v = struct.unpack_from(">14f", b, o)
                    n0, n1 = s16(b, o + 0x38), s16(b, o + 0x3A)
                    w0, w1 = struct.unpack_from(">ff", b, o + 0x3C)
                    def g(n):
                        if n < 0: return -1
                        if local:
                            return b[part["off"] + 0x6C + n] if n < part["local_count"] else -2
                        return n
                    co.append(v[0:3]); nr.append(v[3:6]); uv.append((v[12] * us, v[13] * vs))
                    nd.append((g(n0), g(n1))); ww.append((w0, w1))
                idx = [u16(b, part["to"] + 2 * k) for k in range(part["nt"] * 3)]
                while idx and idx[-1] == 0xFFFF:
                    idx.pop()
                for k in range(len(idx) - 2):
                    a, c1, c2 = idx[k:k + 3]
                    if a == c1 or c1 == c2 or a == c2:
                        continue
                    if k % 2:
                        c1, c2 = c2, c1
                    tri.append((base + a, base + c1, base + c2)); tsh.append(part["shader"])
                    tpart.append(gi * 100 + pi); tk.append(k); treg.append(ri)
    return dict(co=np.array(co, np.float64), nrm=np.array(nr), uv=np.array(uv), nodes=np.array(nd, np.int32),
                w=np.array(ww), tri=np.array(tri, np.int32), tri_shader=np.array(tsh, np.int32),
                tri_part=np.array(tpart, np.int32), tri_strip_k=np.array(tk, np.int32), region=np.array(treg, np.int32),
                node_names=np.array([n["name"] for n in m.nodes]), shaders=np.array(m.shaders),
                regions=np.array([r["name"] for r in m.regions]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag"); ap.add_argument("out"); ap.add_argument("--lod", default="superhigh")
    ap.add_argument("--perm", default=None)
    a = ap.parse_args()
    m = TagModel(a.tag)
    d = export(m, a.lod, a.perm)
    np.savez(a.out, **d)
    print("EXPORT", a.tag, a.lod, "verts", len(d["co"]), "tris", len(d["tri"]), "regions", list(d["regions"]))


if __name__ == "__main__":
    main()
