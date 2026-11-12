"""PRISM v2: a remodelled, much denser Elite (or any CE biped) from the player's own extracted mesh.
Headless Blender:

  blender -b --factory-startup --python art/prism_elite/elite_build.py -- <recipe.json>

Real modelling steps on CE's mesh, every one driven by CE's own data (no outside shapes):
  1. import the map_extract glTF (rig + every region/permutation at superhigh) and the JMS
     templates (nodes, markers, materials, regions are copied verbatim);
  2. per part: the faces of the "follow" shaders (inset lights: the glows) and "keep" shaders
     (teeth) are set aside; the rest is welded, its triangles joined into quads where CE's own
     UVs, seams and hard edges allow (cleaner subdivision), and every edge whose two faces CE
     shades apart (its corner normals) gets a crease: a sharp CE edge stays a crisp bevel;
  3. Catmull-Clark subdivision made *interpolating*: the control cage is moved (a few damped
     iterations) until the limit surface passes through CE's own vertices, so the shape keeps CE's
     proportions and only the facets between the vertices round out (organic Covenant curves);
     UVs are not smoothed, so every CE vertex keeps its texel and textures/class masks line up;
  4. armour plates as geometry: the plate/bodysuit boundary CE paints (the multipurpose map's
     armour channel, the same rule as the PRISM overlay's class split) is cut into the mesh as an
     iso-contour, the plates are extruded outward by a plate thickness and their rims bevelled
     (the step and rim catch light where CE only painted a dark line);
  5. shading normals by angle; weights transferred from CE's mesh on the same rig, at most two
     per vertex, normalised (mandibles keep their frames);
  6. the glowing inset lights ride on the new surface (moved by the surface's own offset under
     them, plus a hair outward); teeth are CE's;
  7. one JMS per permutation per LOD for invader-model (superhigh = level 2, high = level 1,
     medium/low/superlow = CE's own), and a .blend (rig, ce/<part>, new/<part>) for the
     turntables, silhouettes and bakes.
Everything this writes is derived from CE vertices and pixels: local only, never shipped.
"""
import json
import math
import os
import sys
import time

import bpy
import bmesh
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bl_util as U  # noqa: E402
import halo_formats as hf  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()
REPORT = {"parts": {}, "lods": {}}


def P(rel):
    return E.path(rel)


def log(*a):
    print("[%6.1fs]" % (time.time() - T0), *a, flush=True)


def activate(obj):
    for o in bpy.context.scene.objects:
        if o is not None:
            o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def dup(obj, name):
    o = obj.copy()
    o.data = obj.data.copy()
    o.name = name
    o.data.name = name
    bpy.context.scene.collection.objects.link(o)
    return o


def mesh_np(obj):
    a = np.empty(len(obj.data.vertices) * 3, np.float32)
    obj.data.vertices.foreach_get("co", a)
    return a.reshape(-1, 3).astype(np.float64)


def set_mesh_np(obj, co):
    obj.data.vertices.foreach_set("co", np.asarray(co, np.float32).ravel())
    obj.data.update()


def shader_name(mat):
    return (mat.name if mat else "").replace("/", "\\").split("\\")[-1].split(".")[0].lower()


# ---------------------------------------------------------------- textures (CE's multipurpose maps)

class Texture:
    """an image as a float array, sampled bilinearly at Blender UVs (v up; wraps)"""

    def __init__(self, path):
        img = bpy.data.images.load(path, check_existing=False)
        w, h = img.size
        a = np.empty(w * h * 4, np.float32)
        img.pixels.foreach_get(a)
        self.a = a.reshape(h, w, 4)          # row 0 = bottom of the picture (v = 0)
        self.w, self.h = w, h
        bpy.data.images.remove(img)

    def sample(self, uv):
        x = (np.mod(uv[:, 0], 1.0)) * self.w - 0.5
        y = (np.mod(uv[:, 1], 1.0)) * self.h - 0.5
        x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = (x - x0)[:, None], (y - y0)[:, None]
        x0m, x1m = np.mod(x0, self.w), np.mod(x0 + 1, self.w)
        y0m, y1m = np.mod(y0, self.h), np.mod(y0 + 1, self.h)
        a = self.a
        return ((a[y0m, x0m] * (1 - fx) + a[y0m, x1m] * fx) * (1 - fy)
                + (a[y1m, x0m] * (1 - fx) + a[y1m, x1m] * fx) * fy)


def blur(a, sigma):
    """separable gaussian on a 2D array (wrapping), numpy only"""
    r = max(1, int(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * np.roll(a, i - r, axis=0)
    a2 = out
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * np.roll(a2, i - r, axis=1)
    return out


def armour_map(path, cfg):
    """plate weight at the base map's coordinates: the PRISM overlay's rule on the multipurpose map
    (smoothstep of its armour channel, nothing where it self-illuminates), opened and closed so
    specks and pinholes do not become plates, blurred so the contour is smooth"""
    t = Texture(path)
    ch = "rgba".index(cfg.get("channel", "b"))
    lo, hi = cfg.get("lo", 0.35), cfg.get("hi", 0.65)
    m = np.clip((t.a[..., ch] - lo) / (hi - lo), 0, 1)
    m = m * m * (3 - 2 * m)
    ex = "rgba".index(cfg.get("exclude_channel", "g"))
    m[t.a[..., ex] > cfg.get("exclude_above", 0.1)] = 0.0
    m = blur(m, cfg.get("blur_texels", 1.2))
    t.a = np.repeat(m[..., None], 4, 2)
    return t


# ---------------------------------------------------------------- mesh steps

def store_corner_normals(obj):
    me = obj.data
    cn = np.empty(len(me.loops) * 3, np.float32)
    me.corner_normals.foreach_get("vector", cn)
    if "ce_cn" in me.attributes:
        me.attributes.remove(me.attributes["ce_cn"])
    at = me.attributes.new("ce_cn", "FLOAT_VECTOR", "CORNER")
    at.data.foreach_set("vector", cn)


def weld_and_crease(obj, cfg):
    """weld, record CE's hard edges as creases (and seams for the quad join), join triangles into
    quads that keep CE's UVs, seams and hard edges"""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    dl = bm.verts.layers.int.get("dom")
    groups = {}
    for v in bm.verts:
        groups.setdefault(v[dl] if dl is not None else 0, []).append(v)
    for vs in groups.values():  # (welded within one dominant node only: CE's rigid pieces stay apart)
        bmesh.ops.remove_doubles(bm, verts=vs, dist=cfg.get("weld_distance", 1e-5))
    bm.normal_update()
    cn_layer = bm.loops.layers.float_vector.get("ce_cn")
    uv_layer = bm.loops.layers.uv.active
    crease = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
    sharp_deg, soft_deg = cfg.get("crease_sharp_deg", 30.0), cfg.get("crease_soft_deg", 10.0)
    c_sharp, c_soft = cfg.get("crease_sharp", 0.3), cfg.get("crease_soft", 0.12)
    n_sharp = n_soft = 0
    for e in bm.edges:
        e.seam = False
        e.smooth = True
        if len(e.link_faces) != 2:
            continue
        f1, f2 = e.link_faces
        angs = []
        uv_split = False
        for v in e.verts:
            l1 = next(l for l in f1.loops if l.vert is v)
            l2 = next(l for l in f2.loops if l.vert is v)
            a = Vector(l1[cn_layer]).normalized()
            b = Vector(l2[cn_layer]).normalized()
            angs.append(math.degrees(a.angle(b, 0.0)))
            if (Vector(l1[uv_layer].uv) - Vector(l2[uv_layer].uv)).length > 1e-6:
                uv_split = True
        ang = min(angs)
        dihedral = math.degrees(f1.normal.angle(f2.normal, 0.0))
        if uv_split:
            e.seam = True
        if ang > cfg.get("crease_hard_deg", 60.0) or dihedral > cfg.get("crease_dihedral_deg", 70.0):
            e[crease] = cfg.get("crease_hard", 1.0)
            e.smooth = False
            n_sharp += 1
        elif ang > sharp_deg:
            e[crease] = c_sharp
            e.smooth = False
            n_sharp += 1
        elif ang > soft_deg:
            e[crease] = c_soft
            e.smooth = False
            n_soft += 1
    tris = len(bm.faces)
    bmesh.ops.join_triangles(bm, faces=bm.faces[:], cmp_seam=True, cmp_sharp=True, cmp_uvs=True,
                             cmp_vcols=False, cmp_materials=True,
                             angle_face_threshold=math.radians(cfg.get("quad_face_deg", 25.0)),
                             angle_shape_threshold=math.radians(cfg.get("quad_shape_deg", 50.0)))
    quads = sum(1 for f in bm.faces if len(f.verts) == 4)
    bm.to_mesh(me)
    bm.free()
    me.update()
    if me.has_custom_normals:
        activate(obj)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    return {"ce_tris": tris, "faces": len(me.polygons), "quads": quads, "crease_sharp": n_sharp,
            "crease_soft": n_soft, "verts": len(me.vertices)}


def interpolating_subdivision(obj, level, cfg):
    """Catmull-Clark whose limit surface passes through the cage's own (CE) vertices: the cage is
    moved by the limit's miss, damped, a few times. UVs linear (every CE vertex keeps its texel)."""
    target = mesh_np(obj)
    n = len(target)
    m = obj.modifiers.new("subd", "SUBSURF")
    m.subdivision_type = "CATMULL_CLARK"
    m.levels = m.render_levels = level
    m.use_limit_surface = True
    m.uv_smooth = "NONE"
    m.boundary_smooth = "PRESERVE_CORNERS"
    m.use_creases = True
    m.quality = 4
    cage = target.copy()
    miss = []
    for it in range(cfg.get("interp_iterations", 6)):
        dg = bpy.context.evaluated_depsgraph_get()
        eo = obj.evaluated_get(dg)
        me = eo.to_mesh()
        lim = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", lim)
        eo.to_mesh_clear()
        lim = lim.reshape(-1, 3)[:n].astype(np.float64)
        d = target - lim
        miss.append(float(np.linalg.norm(d, axis=1).max()))
        cage += cfg.get("interp_damping", 0.85) * d
        set_mesh_np(obj, cage)
    dg = bpy.context.evaluated_depsgraph_get()
    eo = obj.evaluated_get(dg)
    me = eo.to_mesh()
    lim = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", lim)
    eo.to_mesh_clear()
    miss.append(float(np.linalg.norm(target - lim.reshape(-1, 3)[:n], axis=1).max()))
    activate(obj)
    bpy.ops.object.modifier_apply(modifier=m.name)
    return {"limit_miss_wu": miss, "verts": len(obj.data.vertices), "faces": len(obj.data.polygons)}


def vertex_normals(bm):
    bm.normal_update()
    return {v: v.normal.copy() for v in bm.verts}


def plate_step(obj, maps, cfg, extent, slot_shader):
    """cut the plate/bodysuit boundary CE paints into the mesh, extrude the plates outward by a plate
    thickness and bevel their rims"""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.faces.ensure_lookup_table()
    uvl = bm.loops.layers.uv.active
    nrm0 = vertex_normals(bm)
    # plate weight per vertex (mean over its corners, each sampled in its own face's map)
    acc = {}
    for f in bm.faces:
        t = maps.get(slot_shader.get(f.material_index))
        if t is None:
            continue
        uvs = np.array([l[uvl].uv[:] for l in f.loops])
        w = t.sample(uvs)[:, 0]
        for l, x in zip(f.loops, w):
            s = acc.setdefault(l.vert, [0.0, 0])
            s[0] += float(x)
            s[1] += 1
    mval = {v: s[0] / s[1] for v, s in acc.items()}
    iso = cfg.get("iso", 0.5)
    # 1. split the edges the contour crosses
    cross = []
    for e in bm.edges:
        a, b = e.verts
        if a in mval and b in mval and (mval[a] - iso) * (mval[b] - iso) < 0:
            cross.append(e)
    new_verts = set()
    for e in cross:
        a, b = e.verts
        t = (iso - mval[a]) / (mval[b] - mval[a])
        t = min(max(t, 0.02), 0.98)
        ne, nv = bmesh.utils.edge_split(e, a, t)
        mval[nv] = iso
        nrm0[nv] = (nrm0[a] * (1 - t) + nrm0[b] * t).normalized()
        new_verts.add(nv)
    # 2. connect each face's two contour points
    splits = 0
    for f in list(bm.faces):
        vs = [v for v in f.verts if v in new_verts]
        if len(vs) == 2 and not any(e for e in vs[0].link_edges if e.other_vert(vs[0]) is vs[1]):
            try:
                bmesh.utils.face_split(f, vs[0], vs[1])
                splits += 1
            except ValueError:
                pass
    # 3. the plate faces: mean weight above the iso value (the contour points count as iso)
    plates = []
    for f in bm.faces:
        if f.material_index not in slot_shader or maps.get(slot_shader[f.material_index]) is None:
            continue
        w = np.mean([mval.get(v, 0.0) for v in f.verts])
        if w > iso + 1e-4 and all(mval.get(v, 0.0) >= iso - 1e-4 for v in f.verts):
            plates.append(f)
    # drop tiny islands (specks)
    plate_set = set(plates)
    min_area = (cfg.get("min_island_pct", 0.6) / 100.0 * extent) ** 2
    seen, keep = set(), set()
    islands = 0
    for f in plates:
        if f in seen:
            continue
        stack, isl = [f], []
        seen.add(f)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h in plate_set and h not in seen:
                        seen.add(h)
                        stack.append(h)
        if sum(g.calc_area() for g in isl) >= min_area:
            keep.update(isl)
            islands += 1
    plates = [f for f in plates if f in keep]
    h = cfg.get("thickness_pct", 0.35) / 100.0 * extent
    res = {"contour_edges": len(cross), "face_splits": splits, "plate_faces": len(plates), "islands": islands,
           "thickness_wu": h}
    if not plates:
        bm.to_mesh(me)
        bm.free()
        return res
    nl = bm.verts.layers.float_vector.new("n0")
    for v in bm.verts:
        v[nl] = nrm0.get(v, v.normal)
    ret = bmesh.ops.extrude_face_region(bm, geom=plates, use_keep_orig=False, use_normal_flip=False,
                                        use_select_history=False)
    top = {g for g in ret["geom"] if isinstance(g, bmesh.types.BMVert)}
    # the lifted plate: each extruded vertex along the smooth normal of the surface it came from
    for v in top:
        v.co += Vector(v[nl]).normalized() * h
    # orientation: every face's winding agrees with the surface it came from (the extruded plate and
    # its walls are made consistent with their neighbours, then any face against its source normal
    # is turned)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.normal_update()
    flipped_faces = 0
    for f in bm.faces:
        ref = sum((Vector(v[nl]) for v in f.verts), Vector((0, 0, 0)))
        if f.normal.dot(ref) < 0 and not (any(v in top for v in f.verts) and not all(v in top for v in f.verts)):
            f.normal_flip()
            flipped_faces += 1
    res["faces_turned"] = flipped_faces
    wall_faces = [f for f in bm.faces if any(v in top for v in f.verts) and not all(v in top for v in f.verts)]
    rim = set()
    for f in wall_faces:
        for e in f.edges:
            if all(v in top for v in e.verts):
                rim.add(e)
    res["walls"] = len(wall_faces)
    res["rim_edges"] = len(rim)
    # creases for the smoothing pass after this: the plate's foot sharp, its rim a rolled edge,
    # everything else by its dihedral angle (CE's hard edges came through the first pass sharp)
    crease = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
    foot = set()
    for f in wall_faces:
        for e in f.edges:
            if not any(v in top for v in e.verts):
                foot.add(e)
    bm.normal_update()
    hard = 0
    for e in bm.edges:
        if e in foot:
            e[crease] = cfg.get("foot_crease", 1.0)
        elif e in rim:
            e[crease] = cfg.get("rim_crease", 0.55)
        elif len(e.link_faces) == 2:
            d = math.degrees(e.link_faces[0].normal.angle(e.link_faces[1].normal, 0.0))
            e[crease] = 1.0 if d > cfg.get("recrease_hard_deg", 45.0) else (
                0.5 if d > cfg.get("recrease_soft_deg", 28.0) else 0.0)
            hard += d > cfg.get("recrease_soft_deg", 28.0)
        else:
            e[crease] = 0.0
    res["recreased"] = hard
    bm.to_mesh(me)
    bm.free()
    me.update()
    return res


def smoothing_pass(obj, levels):
    """plain Catmull-Clark (limit surface) over the plated mesh, with its creases: rolls the plate
    rims, keeps the feet and CE's hard edges"""
    m = obj.modifiers.new("smooth", "SUBSURF")
    m.subdivision_type = "CATMULL_CLARK"
    m.levels = m.render_levels = levels
    m.use_limit_surface = True
    m.uv_smooth = "NONE"
    m.boundary_smooth = "PRESERVE_CORNERS"
    m.use_creases = True
    m.quality = 4
    activate(obj)
    bpy.ops.object.modifier_apply(modifier=m.name)
    return {"verts": len(obj.data.vertices), "faces": len(obj.data.polygons)}


def sanity(obj, ref, extent):
    """how far the new surface strays from CE's (both ways), and stray vertices"""
    tree = tree_from_objects([ref])
    mw = obj.matrix_world
    d = []
    for v in obj.data.vertices:
        p = mw @ v.co
        loc, n, _, dist = tree.find_nearest(p)
        d.append(dist if (p - loc).dot(n) >= 0 else -dist)
    d = np.array(d)
    return {"out_max_pct": float(d.max() / extent * 100), "in_max_pct": float(-d.min() / extent * 100),
            "out_p99_pct": float(np.percentile(d, 99) / extent * 100),
            "abs_mean_pct": float(np.abs(d).mean() / extent * 100),
            "strays_over_3pct": int((np.abs(d) > 0.03 * extent).sum())}


def surface_normals(obj, angle_deg):
    me = obj.data
    for p in me.polygons:
        p.use_smooth = True
    me.set_sharp_from_angle(angle=math.radians(angle_deg))
    me.update()


def transfer_weights(new, ref, node_names):
    for n in node_names:
        if n not in new.vertex_groups:
            new.vertex_groups.new(name=n)
    activate(new)
    dt = new.modifiers.new("wt", "DATA_TRANSFER")
    dt.object = ref
    dt.use_object_transform = True
    dt.use_vert_data = True
    dt.data_types_verts = {"VGROUP_WEIGHTS"}
    dt.vert_mapping = "POLYINTERP_NEAREST"
    dt.layers_vgroup_select_src = "ALL"
    dt.layers_vgroup_select_dst = "NAME"
    bpy.ops.object.modifier_apply(modifier=dt.name)
    bpy.ops.object.vertex_group_clean(group_select_mode="ALL", limit=0.001)
    bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=2)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode="ALL", lock_active=False)
    infl = [len([g for g in v.groups if g.weight > 0]) for v in new.data.vertices]
    return {"max_influences": max(infl), "unweighted": int(sum(1 for i in infl if i == 0)),
            "two_bone_share": float(np.mean([i == 2 for i in infl]))}


# ---------------------------------------------------------------- JMS

def body_triangles(obj, tmpl, slot_map, region_index, verts, key2i):
    """the new mesh's triangles (Blender winding = invader-model's) and vertices into a JMS"""
    names = [n.name for n in tmpl.nodes]
    idx = {n: i for i, n in enumerate(names)}
    vg_name = {g.index: g.name for g in obj.vertex_groups}
    mw = obj.matrix_world
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces, quad_method="BEAUTY", ngon_method="BEAUTY")
    bm.to_mesh(me)
    bm.free()
    me.update()
    uv = me.uv_layers.active.data
    nrm3 = mw.to_3x3().inverted().transposed()
    corner_n = me.corner_normals
    vw = []
    for v in me.vertices:
        gw = sorted(((g.weight, vg_name[g.group]) for g in v.groups
                     if g.weight > 0 and vg_name.get(g.group) in idx), reverse=True)[:2]
        if not gw:
            gw = [(1.0, names[0])]
        tot = sum(w for w, _ in gw)
        n0 = idx[gw[0][1]]
        n1, w1 = (idx[gw[1][1]], gw[1][0] / tot) if len(gw) > 1 else (-1, 0.0)
        vw.append((n0, n1, w1, mw @ v.co * 100.0))
    tris = []
    for poly in me.polygons:
        ids = []
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            n0, n1, w1, pw = vw[vi]
            nw = (nrm3 @ corner_n[li].vector).normalized()
            u, vv = uv[li].uv
            key = (vi, round(nw.x, 3), round(nw.y, 3), round(nw.z, 3), round(u, 6), round(vv, 6))
            if key not in key2i:
                key2i[key] = len(verts)
                verts.append(hf.JmsVertex(n0, tuple(pw), tuple(nw), n1, w1, u, vv))
            ids.append(key2i[key])
        tris.append(hf.JmsTriangle(region_index, slot_map[poly.material_index], tuple(ids)))
    bpy.data.meshes.remove(me)
    return tris


def flipped(tri):
    return hf.JmsTriangle(tri.region, tri.shader, (tri.v[0], tri.v[2], tri.v[1]))


# ---------------------------------------------------------------- main

def build_part(part, arm, tmpl, cfg, maps, level, extent_model):
    src = bpy.data.objects[part["object"]]
    shaders = [shader_name(m) for m in src.data.materials]
    tmpl_names = [m[0].lower() for m in tmpl.materials]
    slot_map = {i: tmpl_names.index(s) for i, s in enumerate(shaders)}
    keep = {s.lower() for s in cfg.get("keep_shaders", [])}
    follow = {s.lower() for s in cfg.get("follow_shaders", [])}
    store_corner_normals(src)
    body = dup(src, "%s|L%d" % (part["object"], level))
    # each vertex's dominant node, so the weld keeps CE's rigid splits (a mandible against the face)
    dom = np.zeros(len(body.data.vertices), np.int32)
    for v in body.data.vertices:
        if v.groups:
            dom[v.index] = max(v.groups, key=lambda g: g.weight).group
    if "dom" in body.data.attributes:
        body.data.attributes.remove(body.data.attributes["dom"])
    body.data.attributes.new("dom", "INT", "POINT").data.foreach_set("value", dom)
    body.vertex_groups.clear()
    body.modifiers.clear()
    bm = bmesh.new()
    bm.from_mesh(body.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if shaders[f.material_index] in keep | follow], context="FACES")
    bm.to_mesh(body.data)
    bm.free()
    info = {"level": level, "shaders": shaders}
    info["weld"] = weld_and_crease(body, cfg)
    slot_shader = {i: s for i, s in enumerate(shaders)}
    plates = cfg.get("plates") and level >= cfg["plates"].get("min_level", 1)
    first = level - 1 if (plates and level >= 2) else level
    if first > 0:
        info["subdivision"] = interpolating_subdivision(body, first, cfg)
    if plates:
        info["plates"] = plate_step(body, maps, dict(cfg["plates"]), extent_model, slot_shader)
    if level - first > 0:
        info["smoothing"] = smoothing_pass(body, level - first)
    info["sanity"] = sanity(body, src, extent_model)
    surface_normals(body, cfg.get("sharp_angle_deg", 48.0))
    body.parent = arm
    body.matrix_parent_inverse = src.matrix_parent_inverse.copy()
    body.matrix_world = src.matrix_world.copy()
    info["weights"] = transfer_weights(body, src, [n.name for n in tmpl.nodes])
    am = body.modifiers.new("Armature", "ARMATURE")
    am.object = arm
    info["tris"] = sum(len(p.vertices) - 2 for p in body.data.polygons)
    info["verts"] = len(body.data.vertices)
    return body, slot_map, info


def move_followers(tris, tmpl, ce_tree, new_tree, lift_wu):
    """CE's own vertices of the follow faces (the glows), moved by the surface's own offset under
    them (ray from the nearest CE surface point along its normal onto the new surface)"""
    moved = {}
    shifts = []
    for t in tris:
        for i in t.v:
            if i in moved:
                continue
            v = tmpl.vertices[i]
            p = Vector(v.pos) / 100.0
            loc, n, _, _ = ce_tree.find_nearest(p)
            if loc is None:
                moved[i] = v
                continue
            n = Vector(v.normal).normalized() if Vector(v.normal).length > 0 else n
            best = None
            for d in (n, -n):
                hit = new_tree.ray_cast(loc - d * 1e-4, d, 0.05)
                if hit[0] is not None:
                    dist = (hit[0] - loc).length
                    if best is None or dist < best[1]:
                        best = (hit[0], dist)
            delta = (best[0] - loc) if best else Vector((0, 0, 0))
            q = p + delta + n * lift_wu
            shifts.append(delta.length)
            moved[i] = hf.JmsVertex(v.node0, tuple(q * 100.0), v.normal, v.node1, v.weight, v.u, v.v)
    return moved, shifts


def tree_from_objects(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    bm = bmesh.new()
    for o in objs:
        tmp = bmesh.new()
        tmp.from_mesh(o.data)
        tmp.transform(o.matrix_world)
        m = bpy.data.meshes.new("_t")
        tmp.to_mesh(m)
        tmp.free()
        bm.from_mesh(m)
        bpy.data.meshes.remove(m)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    return tree


def main():
    cfg = U.load_config(U.script_args()[0])
    out = P(cfg["out_dir"])
    os.makedirs(os.path.join(out, "jms"), exist_ok=True)
    U.reset_scene()
    bpy.ops.import_scene.gltf(filepath=P(cfg["gltf"]), bone_heuristic="TEMPERANCE")
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    arm.name = "rig"
    wanted = {p["object"] for p in cfg["parts"]}
    for o in list(bpy.data.objects):
        if o.type == "MESH" and o.name not in wanted:
            bpy.data.objects.remove(o, do_unlink=True)
        elif o.type == "EMPTY":
            bpy.data.objects.remove(o, do_unlink=True)
    maps = {s.lower(): armour_map(P(path), cfg.get("plates", {})) for s, path in cfg.get("multi", {}).items()}
    # model extent (all parts at rest)
    allco = np.concatenate([np.array([o.matrix_world @ v.co for v in o.data.vertices]) for o in bpy.data.objects
                            if o.type == "MESH"])
    extent = float((allco.max(0) - allco.min(0)).max())
    REPORT["extent_wu"] = extent
    log("extent", extent)
    perms = {}
    for p in cfg["parts"]:
        perms.setdefault(p["perm"], []).append(p)
    lod_levels = cfg.get("lod_levels", {"superhigh": 2, "high": 1})
    ce_lods = cfg.get("ce_lods", ["medium", "low", "superlow"])
    jms_dir = P(cfg["jms_dir"])
    for perm, parts in perms.items():
        for lod, level in lod_levels.items():
            tmpl = hf.read_jms(os.path.join(jms_dir, "%s %s.jms" % (perm, lod)))
            tmpl_sh = hf.read_jms(os.path.join(jms_dir, "%s superhigh.jms" % perm))
            # the LOD's own template gives nodes, markers, regions; its materials are the superhigh's
            # (the new mesh at every remodelled level is built from the superhigh)
            verts, key2i, tris = [], {}, []
            mats = tmpl_sh.materials
            region_names = tmpl_sh.regions
            keep = {s.lower() for s in cfg.get("keep_shaders", [])}
            follow = {s.lower() for s in cfg.get("follow_shaders", [])}
            for part in parts:
                body, slot_map, info = build_part(part, arm, tmpl_sh, cfg, maps, level, extent)
                REPORT["parts"]["%s/%s" % (part["object"], lod)] = info
                log(part["object"], lod, json.dumps({k: info[k] for k in ("tris", "verts")}),
                    json.dumps(info.get("plates", {}))[:300])
                ri = region_names.index(part["region"])
                tris += body_triangles(body, tmpl_sh, slot_map, ri, verts, key2i)
                # CE's kept and following faces of this region (from the superhigh template, in its
                # own winding = map_extract's: flipped for invader-model)
                kept = [t for t in tmpl_sh.triangles if t.region == ri and mats[t.shader][0].lower() in keep]
                fol = [t for t in tmpl_sh.triangles if t.region == ri and mats[t.shader][0].lower() in follow]
                src = bpy.data.objects[part["object"]]
                moved = {}
                if fol:
                    ce_tree = tree_from_objects([src])
                    new_tree = tree_from_objects([body])
                    moved, shifts = move_followers(fol, tmpl_sh, ce_tree, new_tree,
                                                   cfg.get("follow_lift_pct", 0.08) / 100.0 * extent)
                    info["followers"] = {"verts": len(moved), "shift_max_wu": max(shifts) if shifts else 0,
                                         "shift_mean_wu": float(np.mean(shifts)) if shifts else 0}
                remap = {}
                for t in kept + fol:
                    vv = []
                    for i in t.v:
                        if i not in remap:
                            remap[i] = len(verts)
                            verts.append(moved.get(i, tmpl_sh.vertices[i]))
                        vv.append(remap[i])
                    tris.append(flipped(hf.JmsTriangle(t.region, t.shader, tuple(vv))))
                if lod == "superhigh":
                    body.name = "new/%s" % part["object"]
                else:
                    body.name = "%s/%s" % (lod, part["object"])
            j = hf.Jms(8200, tmpl.checksum, tmpl.nodes, mats, tmpl.markers, region_names, verts, tris)
            path = os.path.join(out, "jms", "%s %s.jms" % (perm, lod))
            hf.write_jms(path, j)
            by_shader = {}
            for t in tris:
                by_shader[mats[t.shader][0]] = by_shader.get(mats[t.shader][0], 0) + 1
            REPORT["lods"]["%s %s" % (perm, lod)] = {"verts": len(verts), "tris": len(tris), "by_shader": by_shader,
                                                     "ce_tris": len(tmpl.triangles), "ce_verts": len(tmpl.vertices),
                                                     "under_65535": len(verts) < 65535}
            log("JMS", perm, lod, len(verts), len(tris))
        for lod in ce_lods:
            j = hf.read_jms(os.path.join(jms_dir, "%s %s.jms" % (perm, lod)))
            j.triangles = [flipped(t) for t in j.triangles]
            hf.write_jms(os.path.join(out, "jms", "%s %s.jms" % (perm, lod)), j)
            REPORT["lods"]["%s %s" % (perm, lod)] = {"ce": True, "verts": len(j.vertices), "tris": len(j.triangles)}
    for o in bpy.data.objects:
        if o.type == "MESH" and o.name in wanted:
            o.name = "ce/" + o.name
    REPORT["seconds_total"] = round(time.time() - T0, 1)
    with open(os.path.join(out, "build_report.json"), "w") as f:
        json.dump(REPORT, f, indent=1, default=float)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "build.blend"))
    print("BUILD_OK", REPORT["seconds_total"])


if __name__ == "__main__":
    main()
