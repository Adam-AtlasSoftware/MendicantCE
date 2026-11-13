#!/usr/bin/env python3
"""Write a JMS in the winding the Halo toolset (tool.exe, invader-model) builds from.

The pipeline's JMS files follow the extractor (map_extract): triangles in the map's own
draw order. invader-model reverses that order as it strips a JMS, so a model built from
such a file comes out with every triangle facing inward (back-face culled in game: holes
and the insides of plates showing). Verified on the extracted CE JMS itself: compiled
as is, 1612 of 1612 faces disagree with the map's fp.model winding. This flips each
triangle's vertex order and leaves everything else as it is.

usage: jms_for_toolset.py <in.jms> <out.jms>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import halo_formats as hf  # noqa: E402


def main():
    src, dst = sys.argv[1], sys.argv[2]
    j = hf.read_jms(src)
    j.triangles = [hf.JmsTriangle(t.region, t.shader, (t.v[0], t.v[2], t.v[1])) for t in j.triangles]
    hf.write_jms(dst, j)
    print(f"JMS_TOOLSET {dst}: {len(j.vertices)} vertices, {len(j.triangles)} triangles flipped")


if __name__ == "__main__":
    main()
