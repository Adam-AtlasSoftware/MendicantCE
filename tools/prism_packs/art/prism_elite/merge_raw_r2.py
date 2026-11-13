#!/usr/bin/env python3
"""Merge prism_bake_ce.py raw bakes of several passes onto one base map (e.g. the Elite's base00 arms and
its blunt armoured arms share "elite arms"): the first pass keeps every texel it covers, each later pass
fills only the texels no earlier one covered (its island numbers moved past the earlier ones').

usage: merge_raw_r2.py <out raw dir> <raw dir pass 0> <raw dir pass 1> [...]
Local only (CE-derived bakes)."""
import json
import os
import sys

import numpy as np

out, srcs = sys.argv[1], sys.argv[2:]
os.makedirs(out, exist_ok=True)
names = [f[4:-4] for f in os.listdir(srcs[0]) if f.startswith("raw_") and f.endswith(".npy")]
L = lambda d, n: np.load(os.path.join(d, "raw_%s.npy" % n))  # noqa: E731
acc = {n: L(srcs[0], n).copy() for n in names}
cover = acc["cover"] > 0.5
off = int(acc["island32"].max())
rep = {"passes": srcs, "texels": [int(cover.sum())]}
for d in srcs[1:]:
    c = (L(d, "cover") > 0.5) & ~cover
    for n in names:
        v = L(d, n)
        if n in ("island32", "island"):
            v = np.where(v > 0, v + off, 0).astype(acc[n].dtype)
        acc[n] = np.where(c if acc[n].ndim == 2 else c[..., None], v, acc[n])
    off = int(acc["island32"].max())
    cover |= c
    rep["texels"].append(int(c.sum()))
for n, v in acc.items():
    np.save(os.path.join(out, "raw_%s.npy" % n), v)
b = json.load(open(os.path.join(srcs[0], "bake.json")))
b["merged"] = rep
json.dump(b, open(os.path.join(out, "bake.json"), "w"), indent=1)
print("MERGE_OK", json.dumps(rep))
