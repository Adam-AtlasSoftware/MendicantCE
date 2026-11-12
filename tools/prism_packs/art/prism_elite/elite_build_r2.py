"""PRISM v2 round 2: the Elite (and elite_special) remodel, headless Blender:

  blender -b --factory-startup --python art/prism_elite/elite_build_r2.py -- <recipe.json>

Round 1 (elite_build.py) read as "CE, smoothed": an interpolating subdivision everywhere (chest and
shoulders inflated, CE's hard plate edges rounded off), plates cut at a coarse level and rolled by a
second smoothing pass (ragged, soft rims), the inset lights pushed under raised plates, weights by
nearest face across neighbouring mandibles, CE's own lower LODs. This version, every step still driven
by CE's own mesh and pixels:

  1. cage: CE's part welded within each dominant node (rigid pieces stay apart), triangles joined to
     quads; CE's hard edges creased, fully (1.0) where a plate is (CE's armour stays crisp), lightly on
     the bodysuit and skin;
  2. subdivision: Catmull-Clark pulled toward CE's own vertices (damped), then *clamped to CE's surface*:
     no vertex more than cap_out outside CE's (no inflation) or cap_in inside; the clamp's move is
     smoothed over the mesh so it leaves no kink;
  3. plates at the final density: the plate/bodysuit boundary CE paints (multipurpose map) as a
     scalar field per vertex, smoothed over the mesh (so it is continuous across UV seams: no ragged
     edges where CE painted the two sides differently), cut as an iso-contour, the contour relaxed
     along the surface (UVs carried with it); the bodysuit recessed, the plates extruded to a real
     thickness with vertical walls and a crisp chamfered rim (bevel, not a smoothing pass);
  4. secondary forms, the same machinery on CE's painted detail: panel grooves (the dark lines CE paints
     on the plates) recessed into the plates, ridges (the bright lines on the skin: mandible ridges)
     raised; the mandible teeth (CE's alpha-tested cards) cut to their painted outline and given a
     tooth's ridge-shaped cross-section;
  5. the glowing inset lights (and the special's emblem) ride the *outermost* new surface under them;
  6. weights: per connected piece, barycentric from the CE faces of that same piece only (a mandible
     takes only its own mandible's weights: no blends across mandibles or across the feet), 2 per
     vertex;
  7. LODs from the new mesh: superhigh (level 2, 2-segment rims, grooves, ridges, teeth), high (level
     1, 1-segment rims, teeth), medium (CE's cage with the plates cut and thickened), low and
     superlow (the medium decimated to the recipe's counts); one JMS per permutation per LOD.
Writes build.blend (rig, ce/<part>, new/<part> = superhigh, <lod>/<part>) and build_report.json.
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
sys.path.insert(0, HERE)
import mesh_hygiene as MH  # noqa: E402
import bl_util as U  # noqa: E402
import halo_formats as hf  # noqa: E402
import elite_build as EB  # noqa: E402  (round 1: textures, helpers)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()
REPORT = {"parts": {}, "lods": {}}


def P(rel):
    return E.path(rel)


def log(*a):
    print("[%6.1fs]" % (time.time() - T0), *a, flush=True)


def lum_of(rgba):
    return 0.2126 * rgba[..., 0] + 0.7152 * rgba[..., 1] + 0.0722 * rgba[..., 2]


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


class Field(EB.Texture):
    """a scalar field at a base map's coordinates (sampled like EB.Texture; channel 0)"""

    def __init__(self, arr):
        self.a = np.repeat(np.asarray(arr, np.float32)[..., None], 4, 2)
        self.h, self.w = arr.shape


def load_np(path):
    t = EB.Texture(path)
    return t.a


def tophat_field(base, multi, spec, pc):
    """CE's painted thin lines as a [0, 1] field: 'dark' (panel seams on the plates: local mean minus
    the texel) or 'bright' (ridges on the skin: the texel minus the local mean), restricted by the
    multipurpose map's class (plates / not plates), normalised by its own 99th percentile"""
    lum = lum_of(base)
    s1, s2 = spec.get("sigma", 1.5), spec.get("broad_sigma", 5.0)
    if base.shape[:2] != multi.shape[:2]:
        # multipurpose maps are sometimes half the base map's size: resample nearest-ish
        ys = (np.arange(base.shape[0]) * multi.shape[0] // base.shape[0])
        xs = (np.arange(base.shape[1]) * multi.shape[1] // base.shape[1])
        multi = multi[ys][:, xs]
    arm = smoothstep(pc.get("lo", 0.35), pc.get("hi", 0.65), multi[..., "rgba".index(pc.get("channel", "b"))])
    glow = multi[..., "rgba".index(pc.get("exclude_channel", "g"))] > pc.get("exclude_above", 0.1)
    if spec["kind"] == "dark":
        s = np.clip(EB.blur(lum, s1) - lum, 0, None)
    else:
        s = np.clip(lum - EB.blur(lum, s1), 0, None)
    broad = np.abs(EB.blur(lum, s1) - EB.blur(lum, s2))
    s = np.clip(s - spec.get("broad_k", 0.5) * broad, 0, None)
    where = arm if spec.get("on", "plates") == "plates" else (1 - arm)
    where = np.where(glow, 0, where)
    s = s * where
    ref = np.percentile(s[where > 0.5], 99) if (where > 0.5).any() else 1.0
    f = np.clip(s / max(ref, 1e-6), 0, 1.5)
    return EB.blur(f, spec.get("blur", 0.7))


# ---------------------------------------------------------------- cage

def weld_and_crease(obj, cfg, plate_tex, slot_shader):
    """weld within each dominant node, crease CE's hard edges (1.0 on plates), join quads"""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    dl = bm.verts.layers.int.get("dom")
    groups = {}
    for v in bm.verts:
        groups.setdefault(v[dl] if dl is not None else 0, []).append(v)
    for vs in groups.values():
        bmesh.ops.remove_doubles(bm, verts=vs, dist=cfg.get("weld_distance", 1e-5))
    bm.normal_update()
    cn_layer = bm.loops.layers.float_vector.get("ce_cn")
    uv_layer = bm.loops.layers.uv.active
    crease = bm.edges.layers.float.get("crease_edge") or bm.edges.layers.float.new("crease_edge")
    # plate share per face (the multipurpose map's armour channel at its corners)
    plate_face = {}
    for f in bm.faces:
        t = plate_tex.get(slot_shader.get(f.material_index))
        if t is None:
            plate_face[f] = 0.0
            continue
        uvs = np.array([l[uv_layer].uv[:] for l in f.loops] + [np.mean([l[uv_layer].uv[:] for l in f.loops], 0)])
        plate_face[f] = float(t.sample(uvs)[:, 0].mean())
    sharp_deg, soft_deg = cfg.get("crease_sharp_deg", 30.0), cfg.get("crease_soft_deg", 10.0)
    n_hard = n_sharp = n_soft = 0
    for e in bm.edges:
        e.seam = False
        e.smooth = True
        e[crease] = 0.0
        if len(e.link_faces) != 2:
            continue
        f1, f2 = e.link_faces
        angs = []
        uv_split = False
        for v in e.verts:
            l1 = next(l for l in f1.loops if l.vert is v)
            l2 = next(l for l in f2.loops if l.vert is v)
            angs.append(math.degrees(Vector(l1[cn_layer]).normalized().angle(Vector(l2[cn_layer]).normalized(), 0.0)))
            if (Vector(l1[uv_layer].uv) - Vector(l2[uv_layer].uv)).length > 1e-6:
                uv_split = True
        ang = min(angs)
        dihedral = math.degrees(f1.normal.angle(f2.normal, 0.0))
        plate = max(plate_face[f1], plate_face[f2]) > cfg.get("plate_face_share", 0.5)
        if uv_split:
            e.seam = True
        if ang > cfg.get("crease_hard_deg", 60.0) or dihedral > cfg.get("crease_dihedral_deg", 70.0):
            e[crease] = 1.0
            e.smooth = False
            n_hard += 1
        elif ang > sharp_deg:
            e[crease] = cfg.get("crease_plate", 1.0) if plate else cfg.get("crease_sharp", 0.3)
            e.smooth = False
            n_sharp += 1
        elif ang > soft_deg:
            e[crease] = cfg.get("crease_plate_soft", 0.5) if plate else cfg.get("crease_soft", 0.12)
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
        EB.activate(obj)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    return {"ce_tris": tris, "faces": len(me.polygons), "quads": quads, "crease_hard": n_hard,
            "crease_sharp": n_sharp, "crease_soft": n_soft, "verts": len(me.vertices)}


def subdivide(obj, level, cfg):
    if level <= 0:
        return {"level": 0}
    c = dict(cfg)
    c["interp_damping"] = cfg.get("interp_damping", 0.6)
    c["interp_iterations"] = cfg.get("interp_iterations", 4)
    return EB.interpolating_subdivision(obj, level, c)


def tri_tree(co, faces):
    tris = []
    for f in faces:
        for i in range(1, len(f) - 1):
            tris.append((f[0], f[i], f[i + 1]))
    return BVHTree.FromPolygons([tuple(map(float, p)) for p in co], tris, all_triangles=True), np.array(tris)


def mesh_arrays(me):
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    ed = np.empty(len(me.edges) * 2, np.int64)
    me.edges.foreach_get("vertices", ed)
    return co.reshape(-1, 3).astype(np.float64), ed.reshape(-1, 2)


def laplace(vals, ed, n, iters, lam=0.5):
    """vertex values averaged with their edge neighbours (iters times)"""
    v = vals.astype(np.float64)
    deg = np.bincount(ed.ravel(), minlength=n).astype(np.float64)
    for _ in range(iters):
        acc = np.zeros_like(v)
        np.add.at(acc, ed[:, 0], v[ed[:, 1]])
        np.add.at(acc, ed[:, 1], v[ed[:, 0]])
        nb = acc / np.maximum(deg, 1)[(...,) + (None,) * (v.ndim - 1)]
        has = (deg > 0)[(...,) + (None,) * (v.ndim - 1)]
        v = np.where(has, v * (1 - lam) + nb * lam, v)
    return v


def clamp_to_ce(obj, ce_co, ce_faces, extent, cfg):
    """no vertex more than cap_out outside CE's surface or cap_in inside it: the excess is taken back
    along CE's normal there, the correction smoothed over the mesh (no kink)"""
    tree, _ = tri_tree(ce_co, ce_faces)
    co, ed = mesh_arrays(obj.data)
    cap_out = cfg.get("cap_out_pct", 0.35) / 100 * extent
    cap_in = cfg.get("cap_in_pct", 0.6) / 100 * extent
    corr = np.zeros_like(co)
    d_before = np.zeros(len(co))
    for i, p in enumerate(co):
        loc, n, _, dist = tree.find_nearest(Vector(p))
        if loc is None:
            continue
        s = dist if (Vector(p) - loc).dot(n) >= 0 else -dist
        d_before[i] = s
        if s > cap_out:
            corr[i] = -(s - cap_out) * np.array(n)
        elif s < -cap_in:
            corr[i] = (-cap_in - s) * np.array(n)
    corr = laplace(corr, ed, len(co), cfg.get("clamp_smooth", 3), 0.5) * cfg.get("clamp_gain", 1.15)
    co2 = co + corr
    EB.set_mesh_np(obj, co2)
    return {"out_max_before_pct": float(d_before.max() / extent * 100),
            "out_p99_before_pct": float(np.percentile(d_before, 99) / extent * 100),
            "moved": int((np.linalg.norm(corr, axis=1) > 1e-6).sum()),
            "move_max_pct": float(np.linalg.norm(corr, axis=1).max() / extent * 100)}


# ---------------------------------------------------------------- contour cut + offset

def vertex_field(bm, fields, slot_shader, uvl, restrict=None):
    """per vertex: the mean over its corners of the field of each corner's own face's shader"""
    acc = {}
    for f in bm.faces:
        if restrict is not None and not restrict(f):
            continue
        t = fields.get(slot_shader.get(f.material_index))
        if t is None:
            continue
        uvs = np.array([l[uvl].uv[:] for l in f.loops])
        w = t.sample(uvs)[:, 0]
        for l, x in zip(f.loops, w):
            s = acc.setdefault(l.vert, [0.0, 0])
            s[0] += float(x)
            s[1] += 1
    return {v: s[0] / s[1] for v, s in acc.items()}


def smooth_field(bm, mval, iters):
    """the field averaged over the mesh's edges (crosses UV seams: one value per position)"""
    if iters <= 0:
        return mval
    vs = list(mval.keys())
    for _ in range(iters):
        new = {}
        for v in vs:
            nb = [mval[e.other_vert(v)] for e in v.link_edges if e.other_vert(v) in mval]
            new[v] = 0.5 * mval[v] + 0.5 * (sum(nb) / len(nb)) if nb else mval[v]
        mval = new
    return mval


def uv_jacobian(face, uvl):
    """3D displacement -> UV displacement for one face (least squares over its corners)"""
    p0 = face.loops[0].vert.co
    uv0 = Vector(face.loops[0][uvl].uv)
    A, B = [], []
    for l in face.loops[1:]:
        A.append(list(l.vert.co - p0))
        B.append(list(Vector(l[uvl].uv) - uv0))
    A, B = np.array(A), np.array(B)
    J, *_ = np.linalg.lstsq(A, B, rcond=None)   # (3, 2)
    return J


def relax_contour(bm, region, uvl, iters, lam):
    """the boundary of `region` (a face set) made smooth: each boundary vertex with two boundary
    neighbours moves toward their midpoint, back onto the surface; its UVs follow the move"""
    if iters <= 0:
        return 0
    bmc = bm.copy()
    tree = BVHTree.FromBMesh(bmc)
    moved = set()
    for _ in range(iters):
        bnd = {}
        for e in bm.edges:
            lf = e.link_faces
            if len(lf) == 2 and ((lf[0] in region) != (lf[1] in region)):
                a, b = e.verts
                bnd.setdefault(a, []).append(b)
                bnd.setdefault(b, []).append(a)
        for v, nb in bnd.items():
            if len(nb) != 2 or v.is_boundary:
                continue
            target = (nb[0].co + nb[1].co) * 0.5
            d = (target - v.co) * lam
            elen = min(e.calc_length() for e in v.link_edges)
            if d.length > 0.4 * elen:
                d *= 0.4 * elen / d.length
            newp = v.co + d
            hit = tree.find_nearest(newp)
            if hit[0] is not None:
                newp = hit[0]
            delta = newp - v.co
            # the UVs follow the move through the corner's own face's 3D->UV map; a move a sliver
            # face cannot map sensibly (its UV step larger than the face's own UV extent) stops the
            # vertex where it is (a wild UV would wreck the whole part's UV compression in game)
            duvs = []
            ok = True
            for l in v.link_loops:
                try:
                    J = uv_jacobian(l.face, uvl)
                except Exception:
                    ok = False
                    break
                duv = np.array(list(delta)) @ J
                uvs = np.array([x[uvl].uv[:] for x in l.face.loops])
                ext = float(np.max(uvs.max(0) - uvs.min(0)))
                if not np.all(np.isfinite(duv)) or np.linalg.norm(duv) > 0.5 * ext:
                    ok = False
                    break
                duvs.append((l, duv))
            if not ok:
                continue
            for l, duv in duvs:
                l[uvl].uv = (l[uvl].uv[0] + duv[0], l[uvl].uv[1] + duv[1])
            v.co = newp
            moved.add(v)
    bmc.free()
    return len(moved)


def cut_and_offset(obj, fields, slot_shader, spec, extent, restrict_attr=None, mark_attr=None, nrm_attr="n0"):
    """cut the field's iso-contour into the mesh, offset the inside region along the surface's normal
    by spec height (+ out, - in) with vertical walls, bevel its rim; optionally recess the rest
    (spec 'recess_rest'). restrict_attr: a face int attribute that must be 1 for a face to take part.
    mark_attr: a face int attribute set to 1 on the region's (new) top faces."""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.faces.ensure_lookup_table()
    uvl = bm.loops.layers.uv.active
    bm.normal_update()
    nl = bm.verts.layers.float_vector.get(nrm_attr) or bm.verts.layers.float_vector.new(nrm_attr)
    bm.verts.ensure_lookup_table()
    fresh = all(Vector(bm.verts[i][nl]).length < 0.5 for i in range(min(50, len(bm.verts))))
    if fresh:
        for v in bm.verts:
            v[nl] = v.normal
    ra = bm.faces.layers.int.get(restrict_attr) if restrict_attr else None
    restrict = (lambda f: f[ra] == 1) if ra is not None else None
    mval = vertex_field(bm, fields, slot_shader, uvl, restrict)
    mval = smooth_field(bm, mval, spec.get("smooth_iters", 3))
    iso = spec.get("iso", 0.5)
    res = {"name": spec.get("name")}
    cross = [e for e in bm.edges if e.verts[0] in mval and e.verts[1] in mval and
             (mval[e.verts[0]] - iso) * (mval[e.verts[1]] - iso) < 0]
    if restrict is not None:
        cross = [e for e in cross if all(restrict(f) for f in e.link_faces)]
    new_verts = set()
    for e in cross:
        a, b = e.verts
        t = (iso - mval[a]) / (mval[b] - mval[a])
        t = min(max(t, 0.08), 0.92)
        ne, nv = bmesh.utils.edge_split(e, a, t)
        mval[nv] = iso
        nv[nl] = (Vector(a[nl]) * (1 - t) + Vector(b[nl]) * t).normalized()
        new_verts.add(nv)
    splits = 0
    for f in list(bm.faces):
        vs = [v for v in f.verts if v in new_verts]
        if len(vs) == 2 and not any(e.other_vert(vs[0]) is vs[1] for e in vs[0].link_edges):
            try:
                bmesh.utils.face_split(f, vs[0], vs[1])
                splits += 1
            except ValueError:
                pass
    ra = bm.faces.layers.int.get(restrict_attr) if restrict_attr else None
    rot = obj.matrix_world.to_3x3().normalized()
    bm.normal_update()
    region = []
    for f in bm.faces:
        if ra is not None and f[ra] != 1:
            continue
        if not all(v in mval for v in f.verts):
            continue
        w = np.mean([mval[v] for v in f.verts])
        if w > iso + 1e-4 and all(mval[v] >= iso - 1e-4 for v in f.verts):
            # (a face looking down, a sole, is never raised: the feet keep CE's ground contact)
            if spec.get("exclude_down") is not None and (rot @ f.normal).z < -spec["exclude_down"]:
                continue
            region.append(f)
    # islands: drop specks (area) and slivers (area against perimeter)
    rset = set(region)
    min_area = (spec.get("min_island_pct", 0.6) / 100.0 * extent) ** 2
    seen, keep = set(), set()
    islands = 0
    for f in region:
        if f in seen:
            continue
        stack, isl = [f], []
        seen.add(f)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h in rset and h not in seen:
                        seen.add(h)
                        stack.append(h)
        if sum(g.calc_area() for g in isl) >= min_area:
            keep.update(isl)
            islands += 1
    region = [f for f in region if f in keep]
    res.update({"contour_edges": len(cross), "face_splits": splits, "faces": len(region), "islands": islands})
    if not region:
        bm.to_mesh(me)
        bm.free()
        me.update()
        return res
    res["relaxed_verts"] = relax_contour(bm, set(region), uvl, spec.get("relax_iters", 4), spec.get("relax_lambda", 0.5))
    h = spec.get("height_pct", 0.5) / 100.0 * extent
    rest = spec.get("recess_rest", 0.0) * abs(h)
    if rest:
        for v in bm.verts:
            v.co -= Vector(v[nl]).normalized() * rest
    ret = bmesh.ops.extrude_face_region(bm, geom=region, use_keep_orig=False, use_normal_flip=False,
                                        use_select_history=False)
    top = {g for g in ret["geom"] if isinstance(g, bmesh.types.BMVert)}
    # (near the ground the plate grows sideways only: a hoof's sole keeps CE's ground contact and the
    # plate meets it flush, no ledge)
    mw = obj.matrix_world
    imw = mw.inverted().to_3x3()
    zmin = min((mw @ v.co).z for v in bm.verts)
    band = spec.get("ground_band_pct", 1.2) / 100.0 * extent
    for v in top:
        d = mw.to_3x3() @ (Vector(v[nl]).normalized() * h)
        zw = (mw @ v.co).z
        if zw < zmin + band:
            k = (zw - zmin) / band
            d.z = max(d.z, 0.0) * (1 - k) + d.z * k if d.z < 0 else d.z
        v.co += imw @ d
    # winding: made consistent, then the whole mesh turned if most of its (non-wall) faces disagree with
    # the surface they came from; any face still against its source normal is turned alone
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.normal_update()

    def is_wall(f):
        return any(v in top for v in f.verts) and not all(v in top for v in f.verts)

    agree = disagree = 0.0
    for f in bm.faces:
        if is_wall(f):
            continue
        ref = sum((Vector(v[nl]) for v in f.verts), Vector((0, 0, 0)))
        if f.normal.dot(ref) < 0:
            disagree += f.calc_area()
        else:
            agree += f.calc_area()
    if disagree > agree:
        bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
        bm.normal_update()
    turned = 0
    for f in bm.faces:
        ref = sum((Vector(v[nl]) for v in f.verts), Vector((0, 0, 0)))
        if not is_wall(f) and f.normal.dot(ref) < 0:
            f.normal_flip()
            turned += 1
    walls = [f for f in bm.faces if any(v in top for v in f.verts) and not all(v in top for v in f.verts)]
    rim = set()
    for f in walls:
        for e in f.edges:
            if all(v in top for v in e.verts):
                rim.add(e)
    res.update({"height_wu": h, "recess_wu": rest, "walls": len(walls), "rim_edges": len(rim), "faces_turned": turned})
    if mark_attr:
        ma = bm.faces.layers.int.get(mark_attr) or bm.faces.layers.int.new(mark_attr)
        for f in [f for f in bm.faces if all(v in top for v in f.verts)]:
            f[ma] = 1
    seg = spec.get("bevel_segments", 0)
    if seg > 0 and rim:
        off = spec.get("bevel_frac", 0.4) * abs(h)
        try:
            bv = bmesh.ops.bevel(bm, geom=list(rim), offset=off, offset_type="OFFSET", profile_type="SUPERELLIPSE",
                                 segments=seg, profile=spec.get("bevel_profile", 0.5), affect="EDGES",
                                 clamp_overlap=True, material=-1, loop_slide=True, mark_seam=False,
                                 mark_sharp=False, harden_normals=False, face_strength_mode="NONE",
                                 miter_outer="SHARP", miter_inner="SHARP", spread=0.0, vmesh_method="ADJ")
            res["bevel_faces"] = len(bv.get("faces", []))
            if mark_attr:
                ma = bm.faces.layers.int.get(mark_attr)
                for f in bv.get("faces", []):
                    f[ma] = 1
        except Exception as ex:  # (a failed bevel keeps the sharp rim)
            res["bevel_error"] = str(ex)
    # normals of new verts for the next pass
    bm.normal_update()
    for v in bm.verts:
        if Vector(v[nl]).length < 0.5:
            v[nl] = v.normal
    bm.to_mesh(me)
    bm.free()
    me.update()
    return res


def set_face_attr(obj, name, fn):
    me = obj.data
    if name in me.attributes:
        me.attributes.remove(me.attributes[name])
    at = me.attributes.new(name, "INT", "FACE")
    at.data.foreach_set("value", np.array([1 if fn(p) else 0 for p in me.polygons], np.int32))


# ---------------------------------------------------------------- teeth

def solid_teeth(src, keep_shader, teeth_tex, cfg):
    """CE's teeth cards (alpha-tested) cut to their painted outline and given a ridge-shaped
    cross-section (thickness grows with the distance to the outline: a pointed tooth), both sides"""
    o = EB.dup(src, src.name + "|teeth")
    o.modifiers.clear()
    shaders = [EB.shader_name(m) for m in o.data.materials]
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if shaders[f.material_index] != keep_shader], context="FACES")
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
    if not bm.faces:
        bm.free()
        bpy.data.objects.remove(o, do_unlink=True)
        return None, {}
    n_in = len(bm.faces)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=cfg.get("teeth_cuts", 7), use_grid_fill=True)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    uvl = bm.loops.layers.uv.active
    bm.faces.ensure_lookup_table()
    cen = np.array([np.mean([l[uvl].uv[:] for l in f.loops], 0) for f in bm.faces])
    alpha = teeth_tex.sample(cen)[:, 3]
    bmesh.ops.delete(bm, geom=[f for f, a in zip(bm.faces, alpha) if a < cfg.get("teeth_alpha", 0.5)], context="FACES")
    loose = [v for v in bm.verts if not v.link_faces]
    bmesh.ops.delete(bm, geom=loose, context="VERTS")
    bm.normal_update()
    # distance to the outline over the card's edges
    dist = {v: (0.0 if v.is_boundary else 1e9) for v in bm.verts}
    changed = True
    while changed:
        changed = False
        for e in bm.edges:
            a, b = e.verts
            L = e.calc_length()
            if dist[a] + L < dist[b] - 1e-12:
                dist[b] = dist[a] + L
                changed = True
            if dist[b] + L < dist[a] - 1e-12:
                dist[a] = dist[b] + L
                changed = True
    k = cfg.get("teeth_thickness_ratio", 0.55)
    nrm = {v: v.normal.copy() for v in bm.verts}
    back = bm.copy()
    for v in bm.verts:
        v.co += nrm[v] * k * dist[v]
    for v, vb in zip(bm.verts, back.verts):
        vb.co = vb.co - nrm[v] * k * dist[v]
    bmesh.ops.reverse_faces(back, faces=back.faces[:])
    me_b = bpy.data.meshes.new("_teeth_back")
    back.to_mesh(me_b)
    back.free()
    bm.from_mesh(me_b)
    bpy.data.meshes.remove(me_b)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
    bm.to_mesh(o.data)
    bm.free()
    o.data.update()
    EB.surface_normals(o, cfg.get("teeth_sharp_deg", 40.0))
    return o, {"ce_faces": n_in, "tris": sum(len(p.vertices) - 2 for p in o.data.polygons),
               "thickness_max_wu": float(k * max(dist.values()) if dist else 0)}


# ---------------------------------------------------------------- weights

def components(nv, ed):
    parent = np.arange(nv)

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, b in ed:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    return np.array([find(i) for i in range(nv)])


def topo_weights(obj, node_names, cfg):
    """the weights the mesh carried through every step, cleaned to 2 per vertex and normalised"""
    gi = {g.index: g.name for g in obj.vertex_groups}
    nn = {n: i for i, n in enumerate(node_names)}
    nv = len(obj.data.vertices)
    W = np.zeros((nv, len(node_names)))
    for v in obj.data.vertices:
        for g in v.groups:
            if gi.get(g.group) in nn:
                W[v.index, nn[gi[g.group]]] += g.weight
    top = np.argsort(-W, axis=1)[:, :2]
    w2 = np.take_along_axis(W, top, 1)
    lost = W.sum(1) - w2.sum(1)
    w2[w2[:, 1] < cfg.get("weight_min", 0.02) * np.maximum(w2.sum(1), 1e-9), 1] = 0
    w2 /= np.maximum(w2.sum(1, keepdims=True), 1e-9)
    obj.vertex_groups.clear()
    vg = [obj.vertex_groups.new(name=n) for n in node_names]
    for i in range(nv):
        for j in range(2):
            if w2[i, j] > 0:
                vg[top[i, j]].add([i], float(w2[i, j]), "REPLACE")
    return {"two_bone_share": float((w2[:, 1] > 0).mean()), "unweighted": int((W.sum(1) <= 0).sum()),
            "third_node_weight_max": float(lost.max() if nv else 0)}


def piece_weights(new, cage, node_names, cfg):
    """weights per connected piece: each new vertex's from the nearest point on the CE faces of the CE
    piece its own piece came from (barycentric over that triangle's CE weights), 2 per vertex"""
    nn = len(node_names)
    gi = {g.index: g.name for g in cage.vertex_groups}
    idx = {n: i for i, n in enumerate(node_names)}
    cco, ced = mesh_arrays(cage.data)
    W = np.zeros((len(cco), nn))
    for v in cage.data.vertices:
        for g in v.groups:
            nm = gi.get(g.group)
            if nm in idx:
                W[v.index, idx[nm]] += g.weight
    s = W.sum(1)
    W[s > 0] /= s[s > 0, None]
    cfaces = [list(p.vertices) for p in cage.data.polygons]
    ccomp = components(len(cco), ced)
    full_tree, full_tris = tri_tree(cco, cfaces)
    trees = {}
    for c in np.unique(ccomp):
        fs = [f for f in cfaces if ccomp[f[0]] == c]
        if fs:
            trees[c] = tri_tree(cco, fs)
    nco, ned = mesh_arrays(new.data)
    ncomp = components(len(nco), ned)
    # each new piece -> the CE piece most of its vertices lie nearest to
    pmap = {}
    rng = np.random.default_rng(1)
    for c in np.unique(ncomp):
        ids = np.nonzero(ncomp == c)[0]
        smp = ids if len(ids) <= 80 else rng.choice(ids, 80, replace=False)
        votes = {}
        for i in smp:
            loc, n, ti, d = full_tree.find_nearest(Vector(nco[i]))
            if ti is None:
                continue
            k = ccomp[full_tris[ti][0]]
            votes[k] = votes.get(k, 0) + 1
        pmap[c] = max(votes, key=votes.get) if votes else None
    out = np.zeros((len(nco), nn))
    for i, p in enumerate(nco):
        cc = pmap.get(ncomp[i])
        tree, tris = trees.get(cc, (full_tree, full_tris))
        loc, n, ti, d = tree.find_nearest(Vector(p))
        if ti is None:
            continue
        a, b, c = (cco[j] for j in tris[ti])
        q = np.array(loc)
        v0, v1, v2 = b - a, c - a, q - a
        d00, d01, d11 = v0 @ v0, v0 @ v1, v1 @ v1
        d20, d21 = v2 @ v0, v2 @ v1
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-18:
            bc = np.array([1.0, 0, 0])
        else:
            y = (d11 * d20 - d01 * d21) / den
            z = (d00 * d21 - d01 * d20) / den
            bc = np.clip(np.array([1 - y - z, y, z]), 0, 1)
            bc /= max(bc.sum(), 1e-9)
        out[i] = bc @ W[tris[ti]]
    # smoothing over the new mesh within a piece (the edges never cross pieces) to soften CE's coarse
    # joint blends, then 2 per vertex
    out = laplace(out, ned, len(nco), cfg.get("weight_smooth", 2), 0.4)
    top = np.argsort(-out, axis=1)[:, :2]
    w2 = np.take_along_axis(out, top, 1)
    w2[w2[:, 1] < cfg.get("weight_min", 0.02), 1] = 0
    w2 /= np.maximum(w2.sum(1, keepdims=True), 1e-9)
    new.vertex_groups.clear()
    vg = [new.vertex_groups.new(name=n) for n in node_names]
    for i in range(len(nco)):
        for j in range(2):
            if w2[i, j] > 0:
                vg[top[i, j]].add([i], float(w2[i, j]), "REPLACE")
    return {"pieces_new": int(len(np.unique(ncomp))), "pieces_ce": int(len(np.unique(ccomp))),
            "two_bone_share": float((w2[:, 1] > 0).mean()), "unweighted": int((w2[:, 0] <= 0).sum())}


# ---------------------------------------------------------------- one part at one LOD

def build_body(part, src, tmpl_sh, cfg, tex, extent, lod, ce_lod_tris=0):
    # (a part may override settings: e.g. the head, whose mandibles sit millimetres under the helmet's
    # rim, keeps tighter to CE and does not recess its bodysuit)
    cfg = dict(cfg, **part.get("cfg", {}))
    lc = cfg["lods"][lod]
    shaders = [EB.shader_name(m) for m in src.data.materials]
    keep = {s.lower() for s in cfg.get("keep_shaders", [])}
    follow = {s.lower() for s in cfg.get("follow_shaders", [])}
    body = EB.dup(src, "%s|%s" % (part["object"], lod))
    dom = np.zeros(len(body.data.vertices), np.int32)
    for v in body.data.vertices:
        if v.groups:
            dom[v.index] = max(v.groups, key=lambda g: g.weight).group
    if "dom" in body.data.attributes:
        body.data.attributes.remove(body.data.attributes["dom"])
    body.data.attributes.new("dom", "INT", "POINT").data.foreach_set("value", dom)
    body.modifiers.clear()
    bm = bmesh.new()
    bm.from_mesh(body.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if shaders[f.material_index] in keep | follow], context="FACES")
    bm.to_mesh(body.data)
    bm.free()
    slot_shader = {i: s for i, s in enumerate(shaders)}
    info = {"lod": lod}
    info["weld"] = weld_and_crease(body, cfg, tex["plates"], slot_shader)
    cage = EB.dup(body, body.name + "|cage")       # CE's welded cage at CE's positions, CE's weights
    ce_co, _ = mesh_arrays(cage.data)
    ce_faces = [list(p.vertices) for p in cage.data.polygons]
    # (CE's weights stay on the body: the subdivision, the cuts, the extrusions, bevels and the
    # decimation all interpolate them along the mesh's own edges, so a vertex only ever blends the
    # nodes of the CE corners it was made from: no blends across neighbouring mandibles or the feet)
    info["subdivision"] = subdivide(body, lc["level"], cfg)
    if lc["level"] > 0:
        info["clamp"] = clamp_to_ce(body, ce_co, ce_faces, extent, cfg)
    for step in lc.get("steps", []):
        spec = dict(cfg["relief"][step["use"]])
        spec.update(step)
        spec.update(part.get("relief", {}).get(spec["name"], {}))
        fields = tex[spec["field"]]
        if spec.get("restrict"):
            if spec["restrict"] not in body.data.attributes:
                set_face_attr(body, spec["restrict"], lambda p: False)
        info["step_" + spec["name"]] = cut_and_offset(body, fields, slot_shader, spec, extent,
                                                      restrict_attr=spec.get("restrict"), mark_attr=spec.get("mark"))
    if lc.get("decimate_ce_ratio"):
        n0 = sum(len(p.vertices) - 2 for p in body.data.polygons)
        target = max(lc.get("decimate_min", 40), int(lc["decimate_ce_ratio"] * ce_lod_tris))
        m = body.modifiers.new("dec", "DECIMATE")
        m.decimate_type = "COLLAPSE"
        # (the collapse's ratio counts the mesh's own faces, mostly quads here: the triangles come out
        # at about twice ratio x faces)
        m.ratio = min(1.0, target / max(2 * len(body.data.polygons), 1))
        m.use_collapse_triangulate = True
        EB.activate(body)
        bpy.ops.object.modifier_apply(modifier=m.name)
        info["decimate"] = {"from": n0, "to": sum(len(p.vertices) - 2 for p in body.data.polygons)}
    if cfg.get("hygiene", True):
        # coincident twins and zero-thickness fins, zero-area faces and crumpled teeth (the sole rims'
        # sawtooth) the cuts, extrusions and the collapse leave: removed before shading and export
        info["hygiene"] = MH.clean(body)
    info["sanity"] = EB.sanity(body, src, extent)
    EB.surface_normals(body, lc.get("sharp_angle_deg", cfg.get("sharp_angle_deg", 35.0)))
    return body, cage, info


def outermost_followers(tris, tmpl, ce_tree, new_tree, lift_wu, reach_wu):
    """CE's own vertices of the follow faces (the glows), moved by the offset between CE's surface and
    the outermost new surface under them (a ray from outside down CE's normal), plus a lift"""
    moved, shifts = {}, []
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
            nn = Vector(v.normal).normalized() if Vector(v.normal).length > 0 else n
            hit = new_tree.ray_cast(loc + nn * reach_wu, -nn, 2 * reach_wu)
            delta = (hit[0] - loc) if hit[0] is not None else Vector((0, 0, 0))
            q = p + delta + nn * lift_wu
            shifts.append(delta.length)
            moved[i] = hf.JmsVertex(v.node0, tuple(q * 100.0), v.normal, v.node1, v.weight, v.u, v.v)
    return moved, shifts


def main():
    cfg = U.load_config(U.script_args()[0])
    # (iteration: ELITE_R2_PARTS / ELITE_R2_LODS comma lists, ELITE_R2_OUT another output folder)
    if os.environ.get("ELITE_R2_PARTS"):
        sel = os.environ["ELITE_R2_PARTS"].split(",")
        cfg["parts"] = [p for p in cfg["parts"] if any(s in p["object"] for s in sel)]
    if os.environ.get("ELITE_R2_LODS"):
        cfg["lods"] = {k: v for k, v in cfg["lods"].items() if k in os.environ["ELITE_R2_LODS"].split(",")}
    if os.environ.get("ELITE_R2_OUT"):
        cfg["out_dir"] = os.environ["ELITE_R2_OUT"]
    out = P(cfg["out_dir"])
    os.makedirs(os.path.join(out, "jms"), exist_ok=True)
    U.reset_scene()
    bpy.ops.import_scene.gltf(filepath=P(cfg["gltf"]), bone_heuristic="TEMPERANCE")
    arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    arm.name = "rig"
    wanted = {p["object"] for p in cfg["parts"]}
    for o in list(bpy.data.objects):
        if (o.type == "MESH" and o.name not in wanted) or o.type == "EMPTY":
            bpy.data.objects.remove(o, do_unlink=True)
    # (glTF truncates long material names: give each the template's full shader name)
    full = set()
    for perm in {p["perm"] for p in cfg["parts"]}:
        full |= {m[0].lower() for m in hf.read_jms(os.path.join(P(cfg["jms_dir"]), "%s superhigh.jms" % perm)).materials}
    for m in bpy.data.materials:
        sn = EB.shader_name(m)
        if sn not in full:
            cand = [f for f in full if f.startswith(sn)]
            if len(cand) == 1:
                m.name = cand[0]
    pc = cfg["plates_map"]
    tex = {"plates": {}}
    for s, spec in cfg["shader_maps"].items():
        base = load_np(P(os.path.join(cfg["bitmap_dir"], spec["base"] + ".png")))
        multi = load_np(P(os.path.join(cfg["bitmap_dir"], spec["multi"] + ".png")))
        tex["plates"][s] = EB.armour_map(P(os.path.join(cfg["bitmap_dir"], spec["multi"] + ".png")), pc)
        for rname, r in cfg["relief"].items():
            if r["field"] == "plates":
                continue
            if spec.get("no_relief") or (r.get("shaders") and s not in r["shaders"]):
                continue
            tex.setdefault(r["field"], {})[s] = Field(tophat_field(base, multi, r, pc))
    teeth_tex = EB.Texture(P(cfg["teeth_bitmap"])) if cfg.get("teeth_bitmap") else None
    allco = np.concatenate([np.array([o.matrix_world @ v.co for v in o.data.vertices]) for o in bpy.data.objects
                            if o.type == "MESH"])
    extent = float(cfg.get("extent_wu") or (allco.max(0) - allco.min(0)).max())
    REPORT["extent_wu"] = extent
    log("extent", extent)
    perms = {}
    for p in cfg["parts"]:
        perms.setdefault(p["perm"], []).append(p)
    jms_dir = P(cfg["jms_dir"])
    keep = {s.lower() for s in cfg.get("keep_shaders", [])}
    follow = {s.lower() for s in cfg.get("follow_shaders", [])}
    node_names = None
    for perm, parts in perms.items():
        tmpl_sh = hf.read_jms(os.path.join(jms_dir, "%s superhigh.jms" % perm))
        node_names = [n.name for n in tmpl_sh.nodes]
        mats = tmpl_sh.materials
        tmpl_names = [m[0].lower() for m in mats]
        for lod, lc in cfg["lods"].items():
            tmpl = hf.read_jms(os.path.join(jms_dir, "%s %s.jms" % (perm, lod)))
            lod_shaders = {tmpl.materials[t.shader][0].lower() for t in tmpl.triangles}
            verts, key2i, tris = [], {}, []
            for part in parts:
                src = bpy.data.objects[part["object"]]
                EB.store_corner_normals(src)
                shaders = [EB.shader_name(m) for m in src.data.materials]
                slot_map = {i: tmpl_names.index(s) for i, s in enumerate(shaders)}
                ri = tmpl_sh.regions.index(part["region"])
                n_ce = sum(1 for t in tmpl.triangles if t.region == tmpl.regions.index(part["region"])
                           and tmpl.materials[t.shader][0].lower() not in keep | follow)
                body, cage, info = build_body(part, src, tmpl_sh, cfg, tex, extent, lod, n_ce)
                body.parent = arm
                body.matrix_parent_inverse = src.matrix_parent_inverse.copy()
                body.matrix_world = src.matrix_world.copy()
                info["weights"] = topo_weights(body, node_names, cfg)
                am = body.modifiers.new("Armature", "ARMATURE")
                am.object = arm
                ri = tmpl_sh.regions.index(part["region"])
                tris += EB.body_triangles(body, tmpl_sh, slot_map, ri, verts, key2i)
                # teeth: solid at the levels that ask for them, else CE's cards (if this LOD has teeth)
                kept = [t for t in tmpl_sh.triangles if t.region == ri and mats[t.shader][0].lower() in keep]
                if kept and lc.get("solid_teeth") and teeth_tex is not None:
                    for ks in keep:
                        to, tinfo = solid_teeth(src, ks, teeth_tex, cfg)
                        if to is None:
                            continue
                        tcage = EB.dup(src, src.name + "|tcage")
                        to.parent = arm
                        to.matrix_parent_inverse = src.matrix_parent_inverse.copy()
                        to.matrix_world = src.matrix_world.copy()
                        info["teeth"] = tinfo
                        info["teeth"]["weights"] = topo_weights(to, node_names, cfg)
                        tris += EB.body_triangles(to, tmpl_sh, {i: tmpl_names.index(s) for i, s in
                                                                enumerate(EB.shader_name(m) for m in to.data.materials)},
                                                  ri, verts, key2i)
                        to.name = "%s_teeth/%s" % (lod, part["object"])
                        to.modifiers.new("Armature", "ARMATURE").object = arm
                        bpy.data.objects.remove(tcage, do_unlink=True)
                    kept = []
                elif kept and not (keep & lod_shaders):
                    kept = []
                fol = [t for t in tmpl_sh.triangles if t.region == ri and mats[t.shader][0].lower() in follow
                       and mats[t.shader][0].lower() in lod_shaders]
                moved = {}
                if fol:
                    ce_tree = EB.tree_from_objects([src])
                    new_tree = EB.tree_from_objects([body])
                    moved, shifts = outermost_followers(fol, tmpl_sh, ce_tree, new_tree,
                                                        cfg.get("follow_lift_pct", 0.06) / 100.0 * extent,
                                                        cfg.get("follow_reach_pct", 2.5) / 100.0 * extent)
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
                    tris.append(EB.flipped(hf.JmsTriangle(t.region, t.shader, tuple(vv))))
                info["tris"] = sum(len(p.vertices) - 2 for p in body.data.polygons)
                REPORT["parts"]["%s/%s" % (part["object"], lod)] = info
                log(part["object"], lod, info["tris"], json.dumps({k: v for k, v in info.items()
                                                                  if k.startswith("step_") or k in ("clamp", "weights", "sanity")}, default=float)[:900])
                body.name = ("new/%s" % part["object"]) if lod == "superhigh" else ("%s/%s" % (lod, part["object"]))
                bpy.data.objects.remove(cage, do_unlink=True)
            j = hf.Jms(8200, tmpl.checksum, tmpl.nodes, mats, tmpl.markers, tmpl_sh.regions, verts, tris)
            hf.write_jms(os.path.join(out, "jms", "%s %s.jms" % (perm, lod)), j)
            by_shader = {}
            for t in tris:
                by_shader[mats[t.shader][0]] = by_shader.get(mats[t.shader][0], 0) + 1
            REPORT["lods"]["%s %s" % (perm, lod)] = {"verts": len(verts), "tris": len(tris), "by_shader": by_shader,
                                                     "ce_tris": len(tmpl.triangles), "ce_verts": len(tmpl.vertices),
                                                     "under_65535": len(verts) < 65535}
            log("JMS", perm, lod, len(verts), len(tris), "CE", len(tmpl.triangles))
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
