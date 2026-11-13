"""PRISM r3: the back-box panel, CE's additive light-shield quad that r3 draws opaque as armour (lamps_l0 faces 30-31):
its black albedo texels filled with the albedo of the box surface under it, the frame and dots added on top, as
CE's additive quad shows them over the box (otherwise the panel reads as a black slab); the small lamps' shared patch
likewise over the mean albedo of the plates under them (otherwise each lamp reads as a black-rimmed slot).

    blender -b --factory-startup --python art/prism_r3_panel_fill.py -- <r3.blend> <albedo.png in> <albedo.png out>
(blender.sh opens no file: the scene is opened here)"""
import bpy, bmesh, sys, numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform, intersect_point_tri_2d
args = sys.argv[sys.argv.index("--")+1:]
blend, src, dst = args
bpy.ops.wm.open_mainfile(filepath=blend)
lamp = bpy.data.objects["lamps_l0"]; body = bpy.data.objects["r3_body_l0"]
dg = bpy.context.evaluated_depsgraph_get()
bm = bmesh.new(); bm.from_object(body, dg); bmesh.ops.triangulate(bm, faces=bm.faces[:]); bm.transform(body.matrix_world)
bm.faces.ensure_lookup_table(); uvl = bm.loops.layers.uv.active
tree = BVHTree.FromBMesh(bm)
img = bpy.data.images.load(src); W, H = img.size
px = np.empty(W*H*4, dtype=np.float32); img.pixels.foreach_get(px); px = px.reshape(H, W, 4)  # row 0 = v 0
out = px.copy()
me = lamp.data; uv = me.uv_layers.active.data; mw = lamp.matrix_world
filled = 0
for pi in (30, 31):
    p = me.polygons[pi]
    li = list(p.loop_indices)
    t2 = [Vector((uv[l].uv[0], uv[l].uv[1], 0)) for l in li]
    t3 = [mw @ me.vertices[me.loops[l].vertex_index].co for l in li]
    n = (mw.to_3x3() @ p.normal).normalized()
    us = [t.x for t in t2]; vs = [t.y for t in t2]
    for y in range(int(min(vs)*H)-1, int(max(vs)*H)+2):
        for x in range(int(min(us)*W)-1, int(max(us)*W)+2):
            q = Vector(((x+0.5)/W, (y+0.5)/H, 0))
            if not intersect_point_tri_2d(q, t2[0], t2[1], t2[2]):
                continue
            pos = barycentric_transform(q, t2[0], t2[1], t2[2], t3[0], t3[1], t3[2])
            hit, hn, fi, d = tree.ray_cast(pos + n*0.001, -n, 0.02)
            if hit is None:
                continue
            f = bm.faces[fi]
            a, b, c = [l.vert.co for l in f.loops]
            ua, ub, uc = [Vector((l[uvl].uv[0], l[uvl].uv[1], 0)) for l in f.loops]
            bu = barycentric_transform(hit, a, b, c, ua, ub, uc)
            sx = int((bu.x % 1.0)*W) % W; sy = int((bu.y % 1.0)*H) % H
            under = px[sy, sx, :3]
            out[y, x, :3] = np.clip(under + px[y, x, :3], 0, 1)
            filled += 1
# the small lamps (faces 0-29) share one texel patch over different plates: its black filled with the mean albedo of
# the plates under them (they all sit on the green armour), the lamp texels added on top, as CE's additive quads show
unders, boxes = [], []
for p in me.polygons:
    if p.index >= 30:
        continue
    n = (mw.to_3x3() @ p.normal).normalized()
    li = list(p.loop_indices)
    t3 = [mw @ me.vertices[me.loops[l].vertex_index].co for l in li]
    boxes.append([(uv[l].uv[0], uv[l].uv[1]) for l in li])
    for wa, wb in ((1/3, 1/3), (0.6, 0.2), (0.2, 0.6), (0.2, 0.2)):
        pos = t3[0] * wa + t3[1] * wb + t3[2] * (1 - wa - wb)
        hit, hn, fi, d = tree.ray_cast(pos + n*0.001, -n, 0.02)
        if hit is None:
            continue
        f = bm.faces[fi]
        a, b, c = [l.vert.co for l in f.loops]
        ua, ub, uc = [Vector((l[uvl].uv[0], l[uvl].uv[1], 0)) for l in f.loops]
        bu = barycentric_transform(hit, a, b, c, ua, ub, uc)
        sx = int((bu.x % 1.0)*W) % W; sy = int((bu.y % 1.0)*H) % H
        unders.append(px[max(0, sy-6):sy+7, max(0, sx-6):sx+7, :3].reshape(-1, 3).mean(axis=0))
mean = np.median(np.array(unders), axis=0)
bx = np.array([q for b in boxes for q in b])
x0, x1 = int(bx[:, 0].min()*W), int(np.ceil(bx[:, 0].max()*W))
y0, y1 = int(bx[:, 1].min()*H), int(np.ceil(bx[:, 1].max()*H))
out[y0:y1, x0:x1, :3] = np.clip(mean[None, None, :] + px[y0:y1, x0:x1, :3], 0, 1)
img.pixels.foreach_set(out.reshape(-1)); img.filepath_raw = dst; img.file_format = "PNG"; img.save()
print("PANEL_FILLED", filled, "LAMPS_FILLED", (y1-y0)*(x1-x0), "under", [round(float(v), 3) for v in mean], len(unders))
