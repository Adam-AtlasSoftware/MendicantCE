"""Cache the 4x outputs of the upscalers fusion uses on character and weapon maps (one model at a time, one
GPU process): general_wdn_v3, general_v3 (torch), animev3, x4plus_anime, nomos8ksc (ncnn).

  python -I tier_cache.py [tag text ...]   (prism_packs.py's textures step; ../packs_env.py's folders)

ncnn runs on PRISM_PACKS_VULKAN_GPU's card, torch on PRISM_PACKS_TORCH's device.
Writes <work>/textures/out/tiers/cache/<safe tag>[_<i>]/<model>.png (local only: CE pixels).
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texlib as T  # noqa: E402
import tier_recipe as R  # noqa: E402

MODELS = ["general_wdn_v3", "general_v3", "animev3", "x4plus_anime", "nomos8ksc"]
OUT = os.path.join(T.WORK, "out/tiers/cache")


def images(tag):
    """[(key, png path)] of a bitmap tag's bitmaps, from its extraction json"""
    d = json.load(open(os.path.join(R.EXTRACT, tag + ".json")))
    pngs = [os.path.join(R.EXTRACT, b["png"][0].split("/", 1)[1]) for b in d["bitmaps"]]
    k = R.safe(tag)
    return [(k if len(pngs) == 1 else "%s_%d" % (k, i), p) for i, p in enumerate(pngs)]


if __name__ == "__main__":
    only = sys.argv[1:]
    for m in MODELS:
        for tag in R.BASES:
            if only and not any(o in tag for o in only):
                continue
            for key, png in images(tag):
                p = os.path.join(OUT, key, m + ".png")
                if os.path.exists(p):
                    continue
                t0 = time.time()
                ce, _ = T.load(png)
                T.save(p, T.upscale4(m, ce, False))
                print(m, key, round(time.time() - t0, 1), "s", flush=True)
        T._models.clear()
        try:
            import torch
            torch.cuda.empty_cache()
        except Exception:
            pass
    print("CACHE_OK", flush=True)
