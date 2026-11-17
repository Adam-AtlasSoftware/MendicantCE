#!/usr/bin/env python3
"""PRISM v4 world geometry: the open edges a thinned remodel has that its input did not, as points to keep.

usage: python3 art/world_geo/wg3_fix.py <bsp.gltf> <in.pwg> <out.pwg> <keep.npz> [<report.txt>]

wg2_cracks.py's analysis (seams) is run on both files in every mode the engine can draw (near, far, and the two mixed
ones: neighbours at different levels); an open edge of the output (T-junction or gap, not the stock BSP's own border)
whose ends and middle are not all within 2 mm of the input's open edges is new. Their ends are added to <keep.npz>
("P"), which wg3_thin.py reads (WG3_KEEP_POINTS) to keep the surfaces there whole. Exit status 0: nothing new; 3:
points were added (run the thinning again).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wg_common as wg  # noqa: E402
import wg3_pwg  # noqa: E402
from wg2_cracks import MODES, new_edges, seams  # noqa: E402


def main():
    gltf, src, dst, keep_path = sys.argv[1:5]
    report = sys.argv[5] if len(sys.argv) > 5 else None
    _, mats = wg.load_bsp(gltf)
    before, after = wg3_pwg.read(src)["parts"], wg3_pwg.read(dst)["parts"]
    new_pts, lines = [], []
    for mode in MODES:
        a, b = seams(mats, before, mode), seams(mats, after, mode)
        new = new_edges(a, b)
        for i in new:
            new_pts += [b["A"][i], b["B"][i]]
        lines.append("%s: input %d open edges, output %d, new %d" % (
            mode, int((a["kind"] != "border").sum()), int((b["kind"] != "border").sum()), len(new)))
    old = np.load(keep_path)["P"] if os.path.exists(keep_path) else np.zeros((0, 3))
    if new_pts:
        np.savez(keep_path, P=np.concatenate([old.reshape(-1, 3), np.array(new_pts)]))
    text = "\n".join(lines) + "\nkeep points: %d before, %d new\n" % (len(old), len(new_pts))
    print(text, end="")
    if report:
        open(report, "a").write(text)
    sys.exit(3 if new_pts else 0)


if __name__ == "__main__":
    main()
