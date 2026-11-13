#!/usr/bin/env python3
"""cast_cracks.py <jms>... [--region R ...]: the open edges of a character as drawn (the given permutations' JMS
together, all materials, vertices welded at 1e-5 wu): their count and length, and the near misses, open-edge
vertices with another open-edge vertex 1e-5 to 0.02 wu away (a gap one can see through, where the mesh was whole
before its sides moved apart). Prints JSON."""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import halo_formats as hf  # noqa: E402


def load(paths, skip_shaders=()):
    P, T = [], []
    base = 0
    for p in paths:
        j = hf.read_jms(p)
        pos = np.array([v.pos for v in j.vertices], np.float64) / 100.0
        tri = np.array([t.v for t in j.triangles if j.materials[t.shader][0].lower() not in skip_shaders], np.int64)
        P.append(pos)
        T.append(tri + base)
        base += len(pos)
    return np.concatenate(P), np.concatenate(T)


def open_edges(P, T, weld=1e-5):
    key = np.round(P / weld).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    t = inv[T]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    e = np.sort(np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]]), axis=1)
    u, c = np.unique(e, axis=0, return_counts=True)
    ob = u[c == 1]
    rep = np.zeros(inv.max() + 1, np.int64)
    rep[inv] = np.arange(len(inv))
    Q = P[rep]
    return Q, ob


def report(paths, skip=()):
    P, T = load(paths, skip)
    Q, ob = open_edges(P, T)
    length = float(np.linalg.norm(Q[ob[:, 0]] - Q[ob[:, 1]], axis=1).sum())
    bv = np.unique(ob)
    B = Q[bv]
    near = []
    for i in range(0, len(B), 512):
        d = np.linalg.norm(B[i:i + 512, None, :] - B[None, :, :], axis=2)
        k = np.arange(i, min(i + 512, len(B)))
        d[np.arange(len(k)), k] = np.inf
        m = d.min(1)
        near.append(m[(m > 1e-5) & (m < 0.02)])
    near = np.concatenate(near) if near else np.zeros(0)
    return {"triangles": int(len(T)), "open_edges": int(len(ob)), "open_length_wu": round(length, 4),
            "near_miss_vertices": int(len(near)), "near_miss_p50_wu": float(np.median(near)) if len(near) else 0.0,
            "near_miss_max_wu": float(near.max()) if len(near) else 0.0}


if __name__ == "__main__":
    print(json.dumps(report(sys.argv[1:])))
