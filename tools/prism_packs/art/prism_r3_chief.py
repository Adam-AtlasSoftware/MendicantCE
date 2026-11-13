"""PRISM engine unlock, round 3 all-out Master Chief (third person) for the mesh store, headless.

    blender -b --factory-startup --python art/prism_r3_chief.py -- <r3 remodel.blend> <out dir> [--pick <pick dir>]

Round 3 keeps round 2's design (hero_remodel2.py with recipes/chief_3p_r3.json: CE's flat facets crisp, plates
with real thickness, modelled vents, slots and bolts, the boot fix) and spends the mesh store's freedom where it
shows, instead of round 1-all-out's global subdivision and displacement (frayed brim, smeared relief):
- the game level is the r3 remodel's superhigh level as built (no budget collapse: 140k triangles), level 1 its
  high level, levels 2-4 collapses of level 1;
- gloves remodelled: CE's 52-face hands as a creased subdivision surface (a solid glove, not round 2's terraced
  shell), the back-of-hand plate raised from the plate layer's distance field, four knuckle pads modelled and
  seated on the knuckle row, the cuff closed into the sleeve; weights CE's;
- lamps: CE's light-shield faces (the chest, thigh, shoulder and back lamps) lifted per lamp along its normal
  to stand 1.5 mm above the remodelled plate under it (round 1-all-out buried them under the raised plates),
  drawn with the armour shader (the mesh store swaps opaque parts; CE's transparent light-shield parts are
  drawn stock, at CE's place) and lit by the armour material's emissive image (prism_r3_albedo.py --emissive);
- belt pouches modelled and conformed: their back faces laid on the body surface (1.5 mm embedded), the front
  following it, a strap over the waist band; weighted as the surface under them (they move with the hip), plus
  a damped spring share on their lower half only (6 degrees);
- helper bones (cyborg.rig.json): deltoid, elbow, knee, forearm twist, pouch springs; up to four weights;
- the bake pair: l0_bake (level 0's body and gloves, no tubes, pads or pouches, whose uvs lie over the body's)
  and r3_high (everything, the crisp rims bevelled 1 mm): normals and occlusion from geometry only, nothing from
  the base map.
Writes <out>/r3.blend, <out>/cyborg.rig.json, <out>/build.json. Local only (CE vertices and pixels).
"""
import argparse
import json
import math
import os
import sys
import time

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mesh_hygiene as MH  # noqa: E402
import packs_env as E  # noqa: E402
import hero_remodel2 as R2  # noqa: E402  (set_shading2: split above an angle, face-area weighted normals)

MM = 1.0 / 3048.0  # a millimetre in world units


def script_args():
    argv = sys.argv
    return argv[argv.index("--") + 1:] if "--" in argv else []


ap = argparse.ArgumentParser()
ap.add_argument("blend")
ap.add_argument("out")
ap.add_argument("--height2", default=None, help="hero_height2 dir (the plate field for the glove plates)")
ap.add_argument("--level-ratios", type=float, nargs=3, default=[0.35, 0.12, 0.05])
ap.add_argument("--glove-levels", type=int, nargs=2, default=[4, 3])
a = ap.parse_args(script_args())
os.makedirs(a.out, exist_ok=True)
t0 = time.time()
report = {"source": a.blend}
bpy.ops.wm.open_mainfile(filepath=os.path.abspath(a.blend))
scene = bpy.context.scene
rig = bpy.data.objects["rig"]
ce = bpy.data.objects["ce_ref"]
ARMOR, VISOR = 2, 3
# every mesh into model space (Z up, world units): its local coordinates are then the model's, no parent
for _o in list(bpy.data.objects):
    if _o.type == "MESH":
        _mw = _o.matrix_world.copy()
        _o.parent = None
        _o.data.transform(_mw)
        _o.matrix_world = Matrix.Identity(4)
        for _m in list(_o.modifiers):
            _o.modifiers.remove(_m)


def log(*x):
    print(*x, flush=True)


def tris(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def activate(obj, others=()):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in others:
        o.select_set(True)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def duplicate(obj, name, keep_groups=True):
    new = obj.copy()
    new.data = obj.data.copy()
    new.name = new.data.name = name
    scene.collection.objects.link(new)
    for m in list(new.modifiers):
        new.modifiers.remove(m)
    new.parent = None
    new.matrix_world = obj.matrix_world.copy()
    if not keep_groups:
        new.vertex_groups.clear()
    return new


def join(target, others):
    activate(target, others)
    bpy.ops.object.join()
    return target


def positions(obj):
    co = np.zeros(len(obj.data.vertices) * 3)
    obj.data.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def set_positions(obj, co):
    obj.data.vertices.foreach_set("co", np.asarray(co, dtype=np.float64).ravel())
    obj.data.update()


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def shade(obj, angle_deg=50.0):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    limit = math.radians(angle_deg)
    for f in bm.faces:
        f.smooth = True
    for e in bm.edges:
        e.smooth = len(e.link_faces) == 2 and e.calc_face_angle(0.0) <= limit
    bm.to_mesh(obj.data)
    bm.free()


def tree_of(obj):
    co = positions(obj)
    return BVHTree.FromPolygons([Vector(c) for c in co], [tuple(p.vertices) for p in obj.data.polygons])


def bone_head(name):
    return rig.matrix_world @ rig.data.bones[name].head_local


def group_weights(obj, name):
    gi = obj.vertex_groups[name].index
    w = np.zeros(len(obj.data.vertices))
    for v in obj.data.vertices:
        for g in v.groups:
            if g.group == gi:
                w[v.index] = g.weight
    return w


def delete_faces(obj, pred_faces):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    kill = [bm.faces[i] for i in pred_faces]
    bmesh.ops.delete(bm, geom=kill, context="FACES")
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context="VERTS")
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def transfer_weights(dst, src, mapping="POLYINTERP_NEAREST"):
    dst.vertex_groups.clear()
    for g in src.vertex_groups:
        dst.vertex_groups.new(name=g.name)
    activate(dst)
    m = dst.modifiers.new("weights", "DATA_TRANSFER")
    m.object = src
    m.use_vert_data = True
    m.data_types_verts = {"VGROUP_WEIGHTS"}
    m.vert_mapping = mapping
    m.layers_vgroup_select_src = "ALL"
    m.layers_vgroup_select_dst = "NAME"
    bpy.ops.object.modifier_apply(modifier=m.name)


def nearest_uv(dst, src_obj, src_tree):
    """each corner of dst takes the uv of the nearest point of src (barycentric on its face)"""
    me_s = src_obj.data
    uv_s = me_s.uv_layers.active.data
    co_s = positions(src_obj)
    me = dst.data
    if not me.uv_layers:
        me.uv_layers.new(name="UVMap")
    uv = me.uv_layers.active.data
    co = positions(dst)
    for poly in me.polygons:
        for li in poly.loop_indices:
            p = Vector(co[me.loops[li].vertex_index])
            hit, _, fi, _ = src_tree.find_nearest(p)
            sp = me_s.polygons[fi]
            vs = [Vector(co_s[i]) for i in sp.vertices[:3]]
            from mathutils.geometry import barycentric_transform
            ls = list(sp.loop_indices)[:3]
            u = barycentric_transform(hit, vs[0], vs[1], vs[2], Vector((*uv_s[ls[0]].uv, 0)),
                                      Vector((*uv_s[ls[1]].uv, 0)), Vector((*uv_s[ls[2]].uv, 0)))
            uv[li].uv = (u.x, u.y)


# ---------------------------------------------------------------- the plate field (uv space, mm)
H2 = a.height2 or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.blend))), "height2")
SDF_RAISE = np.load(os.path.join(H2, "sdf_raise.npy"))


def sample_uv(field, uv):
    n = field.shape[0]
    x = np.clip((uv[:, 0] % 1.0) * n - 0.5, 0, n - 1.001)
    y = np.clip((1.0 - (uv[:, 1] % 1.0)) * n - 0.5, 0, n - 1.001)
    x0, y0 = x.astype(int), y.astype(int)
    fx, fy = x - x0, y - y0
    return (field[y0, x0] * (1 - fx) * (1 - fy) + field[y0, x0 + 1] * fx * (1 - fy)
            + field[y0 + 1, x0] * (1 - fx) * fy + field[y0 + 1, x0 + 1] * fx * fy)


# ---------------------------------------------------------------- gloves
def hand_faces(obj, side, thr=0.5):
    w = group_weights(obj, "bip01 %s hand" % side)
    return [p.index for p in obj.data.polygons if all(w[i] >= thr for i in p.vertices)]


def build_glove(side, levels):
    """CE's hand as a creased subdivision surface, the back-of-hand plate raised, the cuff closed"""
    g = duplicate(ce, "glove_%s_L%d" % (side, levels))
    keep = set(hand_faces(ce, side))
    delete_faces(g, [p.index for p in g.data.polygons if p.index not in keep])
    bm = bmesh.new()
    bm.from_mesh(g.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    # the cuff: the wrist ring extruded into the sleeve, drawn in (no open edge to look into)
    ring = [e for e in bm.edges if len(e.link_faces) == 1]
    wrist = Vector(bone_head("bip01 %s hand" % side))
    elbow = Vector(bone_head("bip01 %s forearm" % side))
    axis = (elbow - wrist).normalized()
    ext = bmesh.ops.extrude_edge_only(bm, edges=ring)
    nv = [v for v in ext["geom"] if isinstance(v, bmesh.types.BMVert)]
    c = sum((v.co for v in nv), Vector()) / max(1, len(nv))
    for v in nv:
        v.co = c + (v.co - c) * 0.72 + axis * (10 * MM)
    crease = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
    bm.normal_update()
    for e in bm.edges:
        if len(e.link_faces) == 2:
            ang = e.calc_face_angle(0.0)
            e[crease] = 0.0 if ang < math.radians(35) else (0.55 if ang < math.radians(70) else 0.8)
        else:
            e[crease] = 1.0
    for f in bm.faces:
        f.smooth = True
        f.material_index = ARMOR
    bm.to_mesh(g.data)
    bm.free()
    activate(g)
    m = g.modifiers.new("cc", "SUBSURF")
    m.levels = m.render_levels = levels
    m.uv_smooth = "PRESERVE_CORNERS"
    m.boundary_smooth = "PRESERVE_CORNERS"
    bpy.ops.object.modifier_apply(modifier=m.name)
    # the back-of-hand plate: the plate layer's inside raised 2.2 mm (a 1.5 mm wall)
    me = g.data
    nv_ = len(me.vertices)
    uvl = np.zeros(len(me.loops) * 2)
    me.uv_layers.active.data.foreach_get("uv", uvl)
    uvl = uvl.reshape(-1, 2)
    lv = np.zeros(len(me.loops), dtype=np.int64)
    me.loops.foreach_get("vertex_index", lv)
    sd = sample_uv(SDF_RAISE, uvl)
    acc = np.zeros(nv_)
    cnt = np.zeros(nv_)
    np.add.at(acc, lv, sd)
    np.add.at(cnt, lv, 1)
    sdv = acc / np.maximum(cnt, 1)
    nrm = np.zeros(nv_ * 3)
    me.vertex_normals.foreach_get("vector", nrm)
    nrm = nrm.reshape(-1, 3)
    up = smoothstep(-0.2, 1.3, sdv) * 2.2 * MM
    co = positions(g)
    set_positions(g, co + nrm * up[:, None])
    R2.set_shading2(g, 60.0)
    g.data.materials.clear()
    for mat in ce.data.materials:
        g.data.materials.append(mat)
    # (materials.clear() sets every face's slot back to 0, CE's light shield: a transparent shader the mesh store
    # never draws, so the gloves went missing in game; the armour slot set again after the slots are back)
    g.data.polygons.foreach_set("material_index", np.full(len(g.data.polygons), ARMOR, dtype=np.int32))
    g.data.update()
    return g, {"faces": len(keep), "triangles": tris(g), "plate_vertices": int((up > 0.5 * MM).sum())}


KNUCKLES_L = [(0.0310, 0.1350, 0.3000), (0.0220, 0.1350, 0.3000), (0.0110, 0.1340, 0.3000), (0.0005, 0.1340, 0.3000)]
KNUCKLE_N_L = Vector((-0.02, 0.76, -0.65)).normalized()


def rounded_box(name, sx, sy, sz, bevel, segments=3, sub=0):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((v.co.x * sx, v.co.y * sy, v.co.z * sz))
    bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel, segments=segments, affect="EDGES", profile=0.55)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    scene.collection.objects.link(obj)
    if sub:
        activate(obj)
        m = obj.modifiers.new("s", "SUBSURF")
        m.levels = m.render_levels = sub
        bpy.ops.object.modifier_apply(modifier=m.name)
    return obj


def frame(origin, normal, along):
    """a matrix: local y = normal (out of the surface), x = along (projected), z = x cross ... (right handed)"""
    yv = Vector(normal).normalized()
    xv = (Vector(along) - yv * Vector(along).dot(yv)).normalized()
    zv = xv.cross(yv).normalized()
    return Matrix.Translation(origin) @ Matrix((xv, yv, zv)).transposed().to_4x4()


def knuckle_pads(side, glove, tree):
    pads = []
    s = 1.0 if side == "l" else -1.0
    hand = Vector(bone_head("bip01 %s hand" % side))
    for k, p in enumerate(KNUCKLES_L):
        p = Vector((p[0], p[1] * s, p[2]))
        n = Vector((KNUCKLE_N_L.x, KNUCKLE_N_L.y * s, KNUCKLE_N_L.z))
        hit, hn, _, _ = tree.ray_cast(p + n * 0.02, -n, 0.05)
        if hit is None:
            hit, hn, _, _ = tree.find_nearest(p)
        hn = (hn + n).normalized()
        pad = rounded_box("pad_%s%d" % (side, k), 25 * MM, 10 * MM, 21 * MM, 3.2 * MM, 3)
        # (25 x 10 x 21 mm, its centre 2 mm above the glove: 3 mm of it below the surface, 7 mm proud)
        pad.matrix_world = frame(hit + hn * (2.0 * MM), hn, Vector((1, 0, 0)))
        activate(pad)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        pads.append(pad)
    obj = join(pads[0], pads[1:])
    obj.name = obj.data.name = "pads_%s" % side
    for mat in ce.data.materials:
        obj.data.materials.append(mat)
    for poly in obj.data.polygons:
        poly.material_index = ARMOR
    nearest_uv(obj, glove, tree)
    shade(obj, 50.0)
    transfer_weights(obj, glove)
    _ = hand
    return obj


# ---------------------------------------------------------------- lamps
def lamps(surface_tree, lift_mm, name):
    """CE's light-shield faces, each lamp moved along its normal to stand lift_mm above the surface under it"""
    lamp = duplicate(ce, name)
    delete_faces(lamp, [p.index for p in lamp.data.polygons if p.material_index not in (0, 1)])
    me = lamp.data
    co = positions(lamp)
    # islands by shared vertices
    parent = list(range(len(co)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for p in me.polygons:
        vs = list(p.vertices)
        for v in vs[1:]:
            parent[find(v)] = find(vs[0])
    groups = {}
    for p in me.polygons:
        groups.setdefault(find(p.vertices[0]), []).append(p)
    moved = []
    for root, polys in groups.items():
        vids = sorted({v for p in polys for v in p.vertices})
        n = sum((p.normal for p in polys), Vector()).normalized()
        need = -1e9
        for v in vids:
            o = Vector(co[v]) + n * (40 * MM)
            hit, _, _, d = surface_tree.ray_cast(o, -n, 80 * MM)
            if hit is not None:
                need = max(need, 40 * MM - d)  # (positive: the surface is above CE's lamp)
        off = (max(need, 0.0) if need > -1e8 else 0.0) + lift_mm * MM
        for v in vids:
            co[v] = co[v] + np.array(n) * off
        moved.append(round(off / MM, 2))
    set_positions(lamp, co)
    # drawn as armour: the mesh store swaps opaque parts only (CE's light-shield shader is transparent and
    # sorted: its stock quads would stay at CE's place, under the raised plates); the armour material's
    # emissive image lights the lamp texels these faces sample
    for p in me.polygons:
        p.material_index = ARMOR
    return lamp, moved


# ---------------------------------------------------------------- pouches
def make_pouch(side, body, tree):
    """a pouch hung from the waist band at the back of the hip, conformed to the body"""
    s = 1.0 if side == "l" else -1.0
    pelvis = Vector(bone_head("bip01 pelvis"))
    az = math.radians(140.0) * s
    d = Vector((math.cos(az), math.sin(az), 0.0))
    z = pelvis.z + POUCH_DZ
    start = Vector((pelvis.x, pelvis.y, z)) + d * 0.3
    hit, hn, _, _ = tree.ray_cast(start, -d, 0.4)
    if hit is None:
        raise SystemExit("no hip surface for the %s pouch" % side)
    out = Vector((hn.x, hn.y, 0.0)).normalized()
    # (world units: the box 88 x 34 x 76 mm, a flap over its top, a strap from its back up over the waist band,
    # a snap on the flap)
    body_box = rounded_box("pouch_%s_box" % side, 0.029, 0.011, 0.025, 0.0035, 4)
    flap = rounded_box("pouch_%s_flap" % side, 0.0302, 0.0122, 0.0045, 0.0013, 3)
    for v in flap.data.vertices:
        v.co.z += 0.0125 - 0.0014
        v.co.y += 0.0007
    strap = rounded_box("pouch_%s_strap" % side, 0.0070, 0.0016, 0.0170, 0.0005, 2)
    for v in strap.data.vertices:
        v.co.z += 0.0125 + 0.0060
        v.co.y += -0.0055 + 0.0010
    snap = rounded_box("pouch_%s_snap" % side, 0.0040, 0.0020, 0.0040, 0.0009, 3)
    for v in snap.data.vertices:
        v.co.z += 0.0125 - 0.0040
        v.co.y += 0.0068
    obj = join(body_box, [flap, strap, snap])
    obj.name = obj.data.name = "pouch_%s" % side
    # conform: local y is depth (out of the body), x across, z up. Each vertex: the body surface behind it, then
    # its depth above that (the back face 1.5 mm into the body)
    center = hit
    # (local z up the body, y out of it, x = y cross z)
    zv = Vector((0.0, 0.0, 1.0))
    xv = out.cross(zv).normalized()
    basis = Matrix.Translation(center) @ Matrix((xv, out, zv)).transposed().to_4x4()
    co = positions(obj)
    ymin = co[:, 1].min()
    new = []
    for c in co:
        world = basis @ Vector(c)
        o_ = world + out * 0.05
        h2, _, _, dist = tree.ray_cast(o_, -out, 0.1)
        base = h2 if h2 is not None else world - out * (c[1] - ymin)
        new.append(base + out * ((c[1] - ymin) - 1.5 * MM))
    set_positions(obj, np.array([list(v) for v in new]))
    for mat in ce.data.materials:
        obj.data.materials.append(mat)
    for poly in obj.data.polygons:
        poly.material_index = ARMOR
    shade(obj, 45.0)
    return obj, {"centre": list(center), "outward": list(out)}


POUCH_DZ = 0.012


def pouch_uv(obj, patch):
    """a box unwrap scaled into a dark undersuit patch of the base map"""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    uv = bm.loops.layers.uv.active or bm.loops.layers.uv.new("UVMap")
    lo = np.array([min(v.co[i] for v in bm.verts) for i in range(3)])
    hi = np.array([max(v.co[i] for v in bm.verts) for i in range(3)])
    span = float((hi - lo).max())
    for f in bm.faces:
        n = f.normal
        axis = int(np.argmax(np.abs([n.x, n.y, n.z])))
        a_, b_ = [k for k in range(3) if k != axis]
        for loop in f.loops:
            c = loop.vert.co
            loop[uv].uv = (patch[0] + ((c[a_] - lo[a_]) / span - 0.5) * 0.010,
                           patch[1] + ((c[b_] - lo[b_]) / span - 0.5) * 0.010)
    bm.to_mesh(obj.data)
    bm.free()


def dark_patch_uv(body):
    """the uv of the darkest, flattest armour texels near the waist (the undersuit), from the base map"""
    # (the Max texture tier's base map: prism_packs.py's textures step)
    path = E.work("textures", "tiers", "max", "data", "characters", "cyborg", "bitmaps", "cyborg.png")
    img = bpy.data.images.load(path)
    wdt, h = img.size
    px = np.empty(wdt * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    img = px.reshape(h, wdt, 4)[:, :, :3]
    me = body.data
    uv = me.uv_layers.active.data
    pz = Vector(bone_head("bip01 pelvis")).z
    best = None
    co = positions(body)
    for pi in range(0, len(me.polygons), 17):
        poly = me.polygons[pi]
        if poly.material_index != ARMOR:
            continue
        if abs(co[poly.vertices[0]][2] - pz) > 0.06:
            continue
        u = np.mean([uv[li].uv[0] for li in poly.loop_indices])
        v = np.mean([uv[li].uv[1] for li in poly.loop_indices])
        x, y = int((u % 1.0) * (wdt - 1)), int((v % 1.0) * (h - 1))
        patch = img[max(0, y - 24):y + 25, max(0, x - 24):x + 25]
        lum, flat = float(patch.mean()), float(patch.std())
        sat = float((patch.max(axis=2) - patch.min(axis=2)).mean())
        if lum < 0.05:
            continue
        score = lum + 2.0 * sat + 1.5 * flat
        if best is None or score < best[0]:
            best = (score, u, v)
    return best[1], best[2]


# ---------------------------------------------------------------- build
t = time.time()
body0 = duplicate(bpy.data.objects["lod_sh"], "r3_body_l0")
body1 = duplicate(bpy.data.objects["lod_hi"], "r3_body_l1")
bake0 = duplicate(bpy.data.objects["lod_sh_bake"], "l0_bake", keep_groups=False)
removed = {}
for b in (body0, body1):
    kill = sorted(set(hand_faces(b, "l")) | set(hand_faces(b, "r")))
    removed[b.name] = len(kill)
    delete_faces(b, kill)
# the bake twin (the superhigh level before its tubes: the same faces first) loses the same hand faces
src = bpy.data.objects["lod_sh"]
nb = len(bake0.data.polygons)
kill = [i for i in hand_faces(src, "l") + hand_faces(src, "r") if i < nb]
sc, bc_ = positions(src), positions(bake0)
same = all(np.allclose(sc[list(src.data.polygons[i].vertices)], bc_[list(bake0.data.polygons[i].vertices)], atol=1e-7)
           for i in kill[:: max(1, len(kill) // 200)])
if not same:
    raise SystemExit("the bake twin's faces are not the superhigh level's")
delete_faces(bake0, sorted(set(kill)))
removed["l0_bake"] = len(kill)
report["hand_faces_removed"] = removed
log("HANDS removed", removed, round(time.time() - t, 1))

gloves = {0: [], 1: []}
report["gloves"] = {}
for lev, glev in ((0, a.glove_levels[0]), (1, a.glove_levels[1])):
    for s in "lr":
        g, info = build_glove(s, glev)
        gloves[lev].append(g)
        report["gloves"]["%s_l%d" % (s, lev)] = info
pads = {}
for s, g in zip("lr", gloves[0]):
    pads[s] = knuckle_pads(s, g, tree_of(g))
report["pad_triangles"] = {s: tris(o) for s, o in pads.items()}
log("GLOVES", report["gloves"], round(time.time() - t0, 1))

# pouches (conformed to level 0's body)
tree_l0_full = tree_of(body0)
pouches = {}
report["pouches"] = {}
patch = dark_patch_uv(body0)
report["pouch_uv"] = patch
for s in "lr":
    p, info = make_pouch(s, body0, tree_l0_full)
    pouch_uv(p, patch)
    pouches[s] = p
    report["pouches"][s] = info
log("POUCHES", report["pouches"])

# lamps lifted above level 0's surface (the gloves and pads too: a lamp never sits under anything)
surf = duplicate(body0, "tmp_surface", keep_groups=False)
surf = join(surf, [duplicate(g, "tmp_g%d" % i, keep_groups=False) for i, g in enumerate(gloves[0])])
st = tree_of(surf)
lamp0, moved0 = lamps(st, 1.5, "lamps_l0")
lamp1, _ = lamps(st, 2.5, "lamps_l1")
lamp2, _ = lamps(st, 4.0, "lamps_l2")
bpy.data.objects.remove(surf, do_unlink=True)
report["lamp_lift_mm"] = moved0
log("LAMPS", moved0)

# ---------------------------------------------------------------- skeleton helpers
J = lambda n: np.array(bone_head(n))  # noqa: E731
ce_names = [b.name for b in rig.data.bones]
HELPERS = []
for s in "lr":
    ua, fa, hand = "bip01 %s upperarm" % s, "bip01 %s forearm" % s, "bip01 %s hand" % s
    th, ca, ft = "bip01 %s thigh" % s, "bip01 %s calf" % s, "bip01 %s foot" % s
    HELPERS.append(("%s deltoid" % s, "bip01 %s clavicle" % s, J(ua), J(ua) + (J(fa) - J(ua)) * 0.3))
    HELPERS.append(("%s elbow" % s, ua, J(fa), J(fa) + (J(hand) - J(fa)) * 0.25))
    HELPERS.append(("%s forearm twist" % s, fa, J(fa) + (J(hand) - J(fa)) * 0.5, J(hand)))
    HELPERS.append(("%s knee" % s, th, J(ca), J(ca) + (J(ft) - J(ca)) * 0.25))
for s, p in pouches.items():
    top = positions(p)[:, 2].max()
    c = positions(p).mean(0)
    head = np.array([c[0], c[1], top - 0.004])
    HELPERS.append(("%s pouch" % s, "bip01 pelvis", head, head + np.array([0, 0, -0.02])))
activate(rig)
bpy.ops.object.mode_set(mode="EDIT")
inv = rig.matrix_world.inverted()
eb = rig.data.edit_bones
for name, parent, head, tail in HELPERS:
    b = eb.new(name)
    b.head = inv @ Vector(head)
    b.tail = inv @ Vector(tail)
    b.parent = eb[parent]
    b.use_connect = False
bpy.ops.object.mode_set(mode="OBJECT")
bone_names = ce_names + [h[0] for h in HELPERS]
RIG = {"version": 1, "comment": "PRISM r3 Chief: joint volume, forearm twist, pouch springs (attached pouches)",
       "helpers": []}
for s in "lr":
    RIG["helpers"] += [
        {"bone": "%s deltoid" % s, "type": "copy_rotation", "source": "bip01 %s upperarm" % s, "fraction": 0.5,
         "axes": "xyz", "space": "local"},
        {"bone": "%s elbow" % s, "type": "copy_rotation", "source": "bip01 %s forearm" % s, "fraction": 0.5,
         "axes": "xyz", "space": "local"},
        {"bone": "%s forearm twist" % s, "type": "twist", "source": "bip01 %s forearm" % s,
         "child": "bip01 %s hand" % s, "axis": "x", "helper_axis": "y", "fraction": 0.5},
        {"bone": "%s knee" % s, "type": "copy_rotation", "source": "bip01 %s calf" % s, "fraction": 0.5,
         "axes": "xyz", "space": "local"},
        {"bone": "%s pouch" % s, "type": "spring", "stiffness": 260.0, "damping": 14.0, "gravity": 1.0,
         "limit_deg": 6.0, "length": 0.02, "axis": "y"},
    ]
open(os.path.join(a.out, "cyborg.rig.json"), "w").write(json.dumps(RIG, indent=1))


def weights_array(obj, names):
    index = {g.index: names.index(g.name) for g in obj.vertex_groups if g.name in names}
    w = np.zeros((len(obj.data.vertices), len(names)))
    for v in obj.data.vertices:
        for g in v.groups:
            k = index.get(g.group)
            if k is not None:
                w[v.index, k] = g.weight
    return w


def weights_write(obj, names, w):
    obj.vertex_groups.clear()
    groups = [obj.vertex_groups.new(name=n) for n in names]
    order = np.argsort(-w, axis=1)[:, :4]
    for vi in range(len(w)):
        row = order[vi]
        vals = w[vi, row]
        tot = vals.sum()
        if tot <= 0:
            groups[0].add([vi], 1.0, "REPLACE")
            continue
        for k, val in zip(row, vals):
            if val > 1e-4:
                groups[k].add([vi], float(val / tot), "REPLACE")


def helper_shares(obj):
    w = weights_array(obj, bone_names)
    p = positions(obj)
    col = {n: i for i, n in enumerate(bone_names)}
    moved = {}

    def along(pts, a_, b_):
        d = b_ - a_
        return ((pts - a_) @ d) / (d @ d)
    for s in "lr":
        ua, fa = col["bip01 %s upperarm" % s], col["bip01 %s forearm" % s]
        th, ca = col["bip01 %s thigh" % s], col["bip01 %s calf" % s]
        t_ = along(p, J("bip01 %s upperarm" % s), J("bip01 %s forearm" % s))
        share = np.where(t_ > -0.4, 0.6 * (1 - smoothstep(0.0, 0.45, t_)), 0.0) * (w[:, ua] > 0)
        moved["%s deltoid" % s] = share * w[:, ua]
        w[:, ua] *= 1 - share
        for helper, upper, lower, j0, j1, j2 in (
                ("%s elbow" % s, ua, fa, "bip01 %s upperarm" % s, "bip01 %s forearm" % s, "bip01 %s hand" % s),
                ("%s knee" % s, th, ca, "bip01 %s thigh" % s, "bip01 %s calf" % s, "bip01 %s foot" % s)):
            above = along(p, J(j1), J(j0))
            below = along(p, J(j1), J(j2))
            near = np.where(above > 0, 1 - smoothstep(0.0, 0.3, above), 1 - smoothstep(0.0, 0.3, below))
            share = 0.5 * near
            tot = w[:, upper] + w[:, lower]
            moved[helper] = share * tot
            w[:, upper] *= 1 - share
            w[:, lower] *= 1 - share
        t_ = along(p, J("bip01 %s forearm" % s), J("bip01 %s hand" % s))
        share = 0.7 * smoothstep(0.35, 0.95, t_) * (t_ < 1.15)
        moved["%s forearm twist" % s] = share * w[:, fa]
        w[:, fa] *= 1 - share
    for name, values in moved.items():
        w[:, col[name]] += values
    weights_write(obj, bone_names, w)
    return {name: int((values > 0.01).sum()) for name, values in moved.items()}


def pouch_weights(p, s, src):
    """the body surface's weights under the pouch, plus the spring's share on its lower half"""
    transfer_weights(p, src)
    w = weights_array(p, bone_names)
    co = positions(p)
    z0, z1 = co[:, 2].min(), co[:, 2].max()
    share = 0.35 * (1 - smoothstep(z0 + 0.35 * (z1 - z0), z0 + 0.75 * (z1 - z0), co[:, 2]))
    w *= (1 - share)[:, None]
    w[:, bone_names.index("%s pouch" % s)] += share
    weights_write(p, bone_names, w)


# ---------------------------------------------------------------- levels
report["helpers_l0"] = helper_shares(body0)
helper_shares(body1)
for g in gloves[0] + gloves[1]:
    helper_shares(g)
for s, p in pads.items():
    helper_shares(p)
for s, p in pouches.items():
    pouch_weights(p, s, body0)
for lamp in (lamp0, lamp1, lamp2):
    weights_write(lamp, bone_names, weights_array(lamp, bone_names))

pouch1 = {}
for s, p in pouches.items():
    q = duplicate(p, "pouch_%s_l1" % s)
    activate(q)
    m = q.modifiers.new("d", "DECIMATE")
    m.ratio = 0.4
    bpy.ops.object.modifier_apply(modifier=m.name)
    pouch1[s] = q
level0 = [body0] + gloves[0] + list(pads.values()) + list(pouches.values()) + [lamp0]
level1 = [body1] + gloves[1] + list(pouch1.values()) + [lamp1]
# levels 2-4: level 1's body, gloves and pouches merged and collapsed
merged = duplicate(body1, "r3_l1_merged")
merged = join(merged, [duplicate(o, o.name + "_m") for o in gloves[1] + list(pouch1.values())])
levels = [level0, level1]
hyg = {}
for k, ratio in enumerate(a.level_ratios):
    o = duplicate(merged, "r3_body_l%d" % (k + 2))
    activate(o)
    m = o.modifiers.new("d", "DECIMATE")
    m.decimate_type = "COLLAPSE"
    m.ratio = ratio
    m.use_collapse_triangulate = True
    bpy.ops.object.modifier_apply(modifier=m.name)
    hyg["l%d" % (k + 2)] = MH.clean(o)
    R2.set_shading2(o, 50.0)
    lamp = lamp2 if k == 0 else duplicate(lamp2, "lamps_l%d" % (k + 2))
    levels.append([o, lamp])
bpy.data.objects.remove(merged, do_unlink=True)
# hygiene on the game levels' bodies (the remodel cleaned them; the hand cut and joins checked again)
for o in (body0, body1):
    bm = bmesh.new()
    bm.from_mesh(o.data)
    # (slivers the uncollapsed refinement left: edges under 0.25 mm merged, then triangulated again)
    nf = len(bm.faces)
    bmesh.ops.dissolve_degenerate(bm, dist=0.25 * MM, edges=bm.edges)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(o.data)
    bm.free()
    hyg[o.name] = MH.clean(o)
    hyg[o.name]["sliver_faces_merged"] = nf - len(o.data.polygons)
    R2.set_shading2(o, 50.0)
report["hygiene"] = hyg

CUTOFFS = [300.0, 150.0, 70.0, 25.0, 0.0]
for k, level in enumerate(levels):
    coll = bpy.data.collections.new("lod%d" % k)
    scene.collection.children.link(coll)
    coll["cutoff_pixels"] = CUTOFFS[k]
    for obj in level:
        for c in list(obj.users_collection):
            c.objects.unlink(obj)
        coll.objects.link(obj)
        obj["region"] = 0
        obj["permutation"] = 0
        # (the remodel hid its other levels: an exported piece must be visible to the depsgraph)
        obj.hide_viewport = False
        obj.hide_render = False
        obj.hide_set(False)
for i, mat in enumerate(ce.data.materials):
    mat["ce_shader"] = i
report["levels"] = [{"objects": [o.name for o in lv], "triangles": sum(tris(o) for o in lv),
                     "cutoff_pixels": CUTOFFS[k]} for k, lv in enumerate(levels)]

# ---------------------------------------------------------------- bake pair
bake = bake0
bake = join(bake, [duplicate(g, "bk_" + g.name, keep_groups=False) for g in gloves[0]])
bake.vertex_groups.clear()
R2.set_shading2(bake, 50.0)
for c in list(bake.users_collection):
    c.objects.unlink(bake)
scene.collection.objects.link(bake)
high = duplicate(body0, "r3_high", keep_groups=False)
high = join(high, [duplicate(o, "hi_" + o.name, keep_groups=False)
                   for o in gloves[0] + list(pads.values()) + list(pouches.values())])
high.vertex_groups.clear()
for c in list(high.users_collection):
    c.objects.unlink(high)
scene.collection.objects.link(high)
if high.data.has_custom_normals:
    activate(high)
    bpy.ops.mesh.customdata_custom_splitnormals_clear()
activate(high)
bv = high.modifiers.new("bevel", "BEVEL")
bv.width = 1.0 * MM
bv.segments = 3
bv.limit_method = "ANGLE"
bv.angle_limit = math.radians(40)
bv.use_clamp_overlap = True
bv.harden_normals = False
bpy.ops.object.modifier_apply(modifier=bv.name)
R2.set_shading2(high, 35.0)
report["high_triangles"] = tris(high)
report["bake_triangles"] = tris(bake)
for o in (bake, high):
    o.hide_render = True
report["bones"] = bone_names
report["seconds"] = round(time.time() - t0, 1)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(os.path.join(a.out, "r3.blend")))
open(os.path.join(a.out, "build.json"), "w").write(json.dumps(report, indent=1, default=float))
log("R3_OK", json.dumps(report["levels"]))
