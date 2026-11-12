"""Mesh hygiene for remodelled game meshes (Blender side): the faults a terrace offset and a quadric collapse
leave behind, removed before the mesh is exported.

  import mesh_hygiene as MH
  stats = MH.clean(obj)                # in place; returns counts before/after
  MH.audit(obj)                        # counts only

What it removes (found by the boot-sole glitch hunt, bootsole/):
  * zero-area faces and zero-length edges (bmesh dissolve_degenerate);
  * coincident duplicate faces: same three corner positions (to dist). Same winding: z-fighting twins, one
    is kept. Opposite winding: a zero-thickness fin (a fold collapsed flat), both are removed;
  * crumpled faces: a face whose normal turns more than ~120 degrees from the area-weighted mean normal of
    its edge neighbours (a sawtooth tooth or a fold flap seen as a red back face). The face's shortest edge
    is collapsed (UVs kept); repeated until none is left or the pass limit is reached. Walls stand at about
    90 degrees to their neighbours by design and are never touched.
Positions are compared at dist (world units: 1 wu = 3.048 m; default 0.01 mm).
"""
import collections

import bmesh
import numpy as np

MM_WU = 1.0 / 3048.0


def _tri_arrays(me):
    nv, nf = len(me.vertices), len(me.polygons)
    co = np.empty(nv * 3)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    lt = np.empty(nf, np.int32)
    me.polygons.foreach_get("loop_total", lt)
    assert (lt == 3).all()
    tri = np.empty(nf * 3, np.int32)
    me.polygons.foreach_get("vertices", tri)
    return co, tri.reshape(-1, 3)


def _detect(co, tri, dist, cos_lim):
    """numpy: crumpled faces, duplicate groups (kill list), zero-area faces, edges with > 2 faces"""
    A, B, C = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
    fn = np.cross(B - A, C - A)
    ar = np.linalg.norm(fn, axis=1)
    nu = fn / np.maximum(ar[:, None], 1e-30)
    na = nu * ar[:, None]
    E = np.sort(np.stack([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]], 1).reshape(-1, 2), 1)
    ue, inv, cnt = np.unique(E, axis=0, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    S = np.zeros((len(ue), 3))
    np.add.at(S, inv, np.repeat(na, 3, 0))
    acc = (S[inv] - np.repeat(na, 3, 0)).reshape(-1, 3, 3).sum(1)
    L = np.linalg.norm(acc, axis=1)
    crum = (L > 1e-30) & ((nu * acc).sum(1) < cos_lim * np.maximum(L, 1e-30)) & (ar > 0)
    q = np.ascontiguousarray(np.round(co / dist).astype(np.int64)[tri])   # F,3,3
    qs = np.sort(q.view([("x", np.int64), ("y", np.int64), ("z", np.int64)]).reshape(-1, 3), 1)
    keys = qs.view(np.int64).reshape(-1, 9)
    uk, kinv, kcnt = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
    kinv = kinv.ravel()
    same, fins, kill = 0, 0, []
    dupf = np.nonzero(kcnt[kinv] > 1)[0]
    groups = {}
    for f in dupf:
        groups.setdefault(int(kinv[f]), []).append(int(f))
    for fs in groups.values():
        n0 = nu[fs[0]]
        if any(nu[f] @ n0 < 0 for f in fs[1:]):
            fins += len(fs)
            kill.extend(fs)
        else:
            same += len(fs) - 1
            kill.extend(fs[1:])
    zero = int((ar / 2 < dist * dist).sum())
    return {"crumpled": np.nonzero(crum)[0], "kill": kill, "dup_same": same, "dup_fin_faces": fins, "zero_area": zero,
            "edges_gt2_faces": int((cnt > 2).sum())}


def _counts_me(me, dist, cos_lim):
    co, tri = _tri_arrays(me)
    d = _detect(co, tri, dist, cos_lim)
    return {"faces": len(tri), "dup_same": d["dup_same"], "dup_fin_faces": d["dup_fin_faces"],
            "zero_area": d["zero_area"], "crumpled": int(len(d["crumpled"])), "edges_gt2_faces": d["edges_gt2_faces"]}


def _triangulated_copy(obj):
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(me)
    bm.free()
    return me


def audit(obj, dist=0.01 * MM_WU, crumple_cos=-0.5):
    import bpy
    me = _triangulated_copy(obj)
    c = _counts_me(me, dist, crumple_cos)
    bpy.data.meshes.remove(me)
    return c


def _bm_pass(me, dist, weld):
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.dissolve_degenerate(bm, dist=dist, edges=bm.edges[:])
    if weld:
        bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=dist)
    loose_e = [e for e in bm.edges if not e.link_faces]
    if loose_e:
        bmesh.ops.delete(bm, geom=loose_e, context="EDGES")
    loose_v = [v for v in bm.verts if not v.link_faces]
    if loose_v:
        bmesh.ops.delete(bm, geom=loose_v, context="VERTS")
    tri = [f for f in bm.faces if len(f.verts) > 3]
    if tri:
        bmesh.ops.triangulate(bm, faces=tri, quad_method="BEAUTY", ngon_method="BEAUTY")
    bm.to_mesh(me)
    bm.free()
    me.update()


def clean(obj, dist=0.01 * MM_WU, crumple_cos=-0.5, passes=8, weld=True, crumple=True):
    """crumple=False: twins, fins and zero-area faces only (meshes whose thin shells meet at knife-edge rims
    on purpose, e.g. the first-person sleeves' plate edges, where a collapse would eat the rim)"""
    me = obj.data
    _bm_pass(me, 0.0, False)   # triangulate only
    before = _counts_me(me, dist, crumple_cos)
    collapsed = removed = 0
    for _ in range(passes):
        _bm_pass(me, dist, weld)
        co, tri = _tri_arrays(me)
        d = _detect(co, tri, dist, crumple_cos)
        if not crumple:
            d["crumpled"] = []
        if not len(d["crumpled"]) and not d["kill"]:
            break
        kill = set(d["kill"])
        used = set()
        pairs = []
        for f in d["crumpled"]:
            if int(f) in kill:
                continue
            t = tri[f]
            el = [np.linalg.norm(co[t[k]] - co[t[(k + 1) % 3]]) for k in range(3)]
            k = int(np.argmin(el))
            a, b = int(t[k]), int(t[(k + 1) % 3])
            if a in used or b in used:
                continue
            used.update((a, b))
            pairs.append((a, b))
        bm = bmesh.new()
        bm.from_mesh(me)
        bm.faces.ensure_lookup_table()
        bm.verts.ensure_lookup_table()
        if kill:
            bmesh.ops.delete(bm, geom=[bm.faces[i] for i in sorted(kill)], context="FACES_ONLY")
            removed += len(kill)
        edges = []
        for a, b in pairs:
            va, vb = bm.verts[a], bm.verts[b]
            if not (va.is_valid and vb.is_valid):
                continue
            e = bm.edges.get((va, vb))
            if e is not None:
                edges.append(e)
        if edges:
            bmesh.ops.collapse(bm, edges=edges, uvs=True)
            collapsed += len(edges)
        bm.to_mesh(me)
        bm.free()
        me.update()
    # the last weld can make new twins: delete until none is left
    for _ in range(4):
        _bm_pass(me, dist, weld)
        co, tri = _tri_arrays(me)
        kill = _detect(co, tri, dist, crumple_cos)["kill"]
        if not kill:
            break
        bm = bmesh.new()
        bm.from_mesh(me)
        bm.faces.ensure_lookup_table()
        bmesh.ops.delete(bm, geom=[bm.faces[i] for i in sorted(set(kill))], context="FACES_ONLY")
        removed += len(set(kill))
        bm.to_mesh(me)
        bm.free()
        me.update()
    after = _counts_me(me, dist, crumple_cos)
    return {"before": before, "after": after, "edges_collapsed": collapsed, "dup_faces_removed": removed}
