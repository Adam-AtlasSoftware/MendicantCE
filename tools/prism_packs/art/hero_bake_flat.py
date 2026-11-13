"""A detail-free twin of a remodel's high-poly, for a detail-only normal bake (headless Blender).

  blender -b --factory-startup --python art/hero_bake_flat.py -- <remodel.blend> <out.blend> [--target lod_sh_bake]
        [--subdiv 2] [--split-deg 50]

The high-poly (hero_remodel2.py) is the bake target subdivided and moved by CE's painted detail. Baked against
the smooth-shaded game mesh, its normals also carry the difference between the game mesh's flat facets and
its smoothed vertex normals (a decimated visor bakes as a field of facet triangles). highpoly_flat is the same
subdivision without the detail: its bake holds only that facet term, and hero_bake_delta.py subtracts it, so the
surface map carries the detail alone. Writes <out.blend> with the object highpoly_flat added.
"""
import argparse
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402
import fp_build as FB  # noqa: E402
import hero_remodel2 as H2  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blend")
    ap.add_argument("out")
    ap.add_argument("--target", default="lod_sh_bake")
    ap.add_argument("--subdiv", type=int, default=2)
    ap.add_argument("--split-deg", type=float, default=50.0)
    a = ap.parse_args(U.script_args())
    bpy.ops.wm.open_mainfile(filepath=a.blend)
    src = bpy.data.objects[a.target]
    o = FB.dup(src, "highpoly_flat")
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    FB.activate(o)
    m = o.modifiers.new("subd", "SUBSURF")
    m.subdivision_type = "SIMPLE"
    m.levels = m.render_levels = a.subdiv
    m.uv_smooth = "NONE"
    bpy.ops.object.modifier_apply(modifier=m.name)
    H2.set_shading2(o, a.split_deg)
    o.hide_render = True
    bpy.ops.wm.save_as_mainfile(filepath=a.out)
    print("FLAT_OK", len(o.data.polygons))


main()
