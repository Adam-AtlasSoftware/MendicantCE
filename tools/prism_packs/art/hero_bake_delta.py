#!/usr/bin/env python3
"""Detail-only tangent normals: the high-poly's bake minus its detail-free twin's (hero_bake_flat.py), both from
prism_bake_ce.py at the same size and target. Where both hit, n.xy = a.xy - b.xy (z rebuilt). Where only the
high-poly hit (the flat twin missed: typically a rim whose faces folded their UVs back), a's normal still holds
the facet term the delta exists to remove (the shipped round-2 map tilted 12% of the sole rim's texels past
30 degrees there), so those texels take the delta pushed in from the island's both-hit texels and their tilt
is capped at --one-hit-max-tilt-deg. Where neither hit, nothing (the pack fills holes). All other raw files
are copied from the high-poly's bake, so prism_surface_pack.py runs on the result unchanged.

  python -I art/hero_bake_delta.py <raw dir (high-poly)> <raw dir (flat twin)> <out raw dir>
        [--one-hit-max-tilt-deg 20]
"""
import json
import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prism_surface_pack import push_pull  # noqa: E402


def main():
    a_dir, b_dir, out = sys.argv[1:4]
    cap_deg = float(sys.argv[sys.argv.index("--one-hit-max-tilt-deg") + 1]) if "--one-hit-max-tilt-deg" in sys.argv else 20.0
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(a_dir):
        if f != "raw_normal.npy":
            src = os.path.join(a_dir, f)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(out, f))
    A = np.load(os.path.join(a_dir, "raw_normal.npy")).astype(np.float32)
    B = np.load(os.path.join(b_dir, "raw_normal.npy")).astype(np.float32)
    ha = A.max(-1) > 0
    hb = B.max(-1) > 0
    na = A * 2 - 1
    nb = B * 2 - 1
    both = ha & hb
    one = ha & ~hb
    xy = np.where(both[..., None], na[..., :2] - nb[..., :2], 0.0).astype(np.float32)
    if one.any():
        xy = np.where(one[..., None], push_pull(xy, both), xy)
        r1 = np.linalg.norm(xy, axis=-1, keepdims=True)
        lim = np.sin(np.radians(cap_deg))
        xy = np.where(one[..., None] & (r1 > lim), xy / np.maximum(r1, 1e-6) * lim, xy)
    r = np.linalg.norm(xy, axis=-1, keepdims=True)
    xy = np.where(r > 0.95, xy / np.maximum(r, 1e-6) * 0.95, xy)
    z = np.sqrt(np.clip(1 - (xy ** 2).sum(-1, keepdims=True), 0, 1))
    n = np.concatenate([xy, z], -1)
    outn = np.where(ha[..., None], n * 0.5 + 0.5, 0).astype(np.float16)
    np.save(os.path.join(out, "raw_normal.npy"), outn)
    tilt = lambda v: float(np.degrees(np.arccos(np.clip(v[..., 2], -1, 1)))[ha].mean())
    info = {"hit_share": float(ha.mean()), "both_share": float(both.mean()), "one_hit_share": float(one.mean()),
            "one_hit_max_tilt_deg": cap_deg,
            "tilt_deg_mean_before": tilt(na), "tilt_deg_mean_after": tilt(n), "tilt_deg_mean_flat_twin": tilt(nb)}
    json.dump(info, open(os.path.join(out, "delta.json"), "w"), indent=1)
    print("DELTA_OK", json.dumps(info))


if __name__ == "__main__":
    main()
