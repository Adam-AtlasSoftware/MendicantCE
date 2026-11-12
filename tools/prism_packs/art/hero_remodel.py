"""PRISM v2 hero remodel: a much denser third-person model from CE's own mesh, with the shape
detail CE painted into its textures turned into geometry (headless Blender).

  blender -b --factory-startup --python art/hero_remodel.py -- <recipe.json> <height_dir> <out_dir>

Per level of detail (recipe "remodel.lods"):
  1. CE's superhigh mesh, welded; the keep_shaders' faces (effect shells) are left out and copied
     from that LOD's CE JMS unchanged; each face remembers its CE triangle (ce_tri);
  2. SIMPLE subdivision (every CE vertex, edge and UV kept), then adaptive refinement: an edge is
     split where the relief (hero_height.py, at the base map's coordinates) bends along it by
     more than the LOD's tolerance, and around CE's hard edges, so plate borders, seams and ribs
     get vertices where CE drew them. Every new vertex still lies on its CE facet with a UV
     interpolated linearly inside its CE triangle, so CE's textures and class masks stay aligned;
  3. rounding: every vertex onto the Phong surface through CE's vertices and corner normals
     (fp_build.CEBand, alpha 1), capped per vertex;
  4. hard edges: CE's hard edges (where its corner normals split) are rounded into a small
     fillet (a few constrained Laplacian steps in a band around them): the bevel that carries a
     highlight, cut inward only;
  5. relief: each vertex moves along its smooth normal by the relief height (the average over its
     UV corners, so a UV seam never cracks); an inward move is capped by the local thickness;
  6. rims: the armour's open borders (the visor opening, the neck) get a rolled lip and a wall
     turned inward, a real edge thickness;
  7. shading: smooth, split above the recipe's angle, face-area weighted normals (flat plates
     stay flat, the bevels carry the light);
  8. weights: Data Transfer from CE's mesh on the same rig, at most 2 per vertex, normalised;
  9. lower LODs: collapse-decimated from the new high level (vertex groups and UVs carried).
A high-poly for the bakes is built the same way at a finer tolerance (recipe "highpoly").

Outputs (local only: derived from the player's CE vertices and pixels): <out>/export/__base <lod>.jms,
<out>/remodel.blend (objects rig, ce_ref, lod_<name>..., highpoly), <out>/remodel_report.json.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bl_util as U  # noqa: E402
import halo_formats as hf  # noqa: E402
import fp_build as FB  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packs_env as E  # noqa: E402

ROOT = E.WORK
T0 = time.time()
REPORT = {"steps": {}}
DECIMATED_FROM = {}
MOVES = {}      # level name -> (world positions on CE's facets, their moves)
CM_WU = 0.01 / 3.048        # one centimetre in world units


def P(rel):
    return E.path(rel)


def step(name, **kv):
    REPORT["steps"][name] = dict(kv, t=round(time.time() - T0, 1))
    print("STEP", name, json.dumps(kv, default=float)[:900], flush=True)


# ------------------------------------------------------------------ relief sampling
class Relief:
    def __init__(self, path):
        self.h = np.load(path).astype(np.float32)
        self.n = self.h.shape[0]

    def sample(self, uv):
        """bilinear, Blender UVs (v up), cm"""
        n = self.n
        x = np.clip(uv[:, 0] * n - 0.5, 0, n - 1.001)
        y = np.clip((1 - uv[:, 1]) * n - 0.5, 0, n - 1.001)
        x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
        fx, fy = x - x0, y - y0
        h = self.h
        return (h[y0, x0] * (1 - fx) * (1 - fy) + h[y0, x0 + 1] * fx * (1 - fy)
                + h[y0 + 1, x0] * (1 - fx) * fy + h[y0 + 1, x0 + 1] * fx * fy)


def mesh_arrays(me):
    nl = len(me.loops)
    lv = np.empty(nl, np.int32)
    me.loops.foreach_get("vertex_index", lv)
    uv = np.empty(nl * 2, np.float32)
    me.uv_layers["UVMap"].data.foreach_get("uv", uv)
    return lv, uv.reshape(-1, 2).astype(np.float64)


def vertex_height(obj, relief, skip_mat=()):
    """per vertex: the relief averaged over its UV corners (cm); vertices only on skipped
    materials get 0"""
    me = obj.data
    lv, uv = mesh_arrays(me)
    h = relief.sample(uv)
    lt = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_total", lt)
    mi = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("material_index", mi)
    lm = np.repeat(mi, lt)
    use = ~np.isin(lm, list(skip_mat))
    acc = np.zeros(len(me.vertices))
    cnt = np.zeros(len(me.vertices))
    np.add.at(acc, lv[use], h[use])
    np.add.at(cnt, lv[use], 1)
    return np.where(cnt > 0, acc / np.maximum(cnt, 1), 0.0), cnt > 0


# ------------------------------------------------------------------ refinement
def refine(obj, relief, tol_cm, min_len_wu, iters, detail_mats, sharp_band_wu=0.0, sharp_len_wu=0.0):
    """split edges where the relief bends along them (sampled at the quarter points of the edge's
    UV segment on each side of it) and, optionally, edges near CE's hard edges; repeat"""
    me = obj.data
    total = 0
    for it in range(iters):
        bm = bmesh.new()
        bm.from_mesh(me)
        uvl = bm.loops.layers.uv["UVMap"]
        sharp_layer = bm.edges.layers.bool.get("sharp_edge")
        bm.edges.ensure_lookup_table()
        ne = len(bm.edges)
        elen = np.empty(ne)
        uvs = []          # (edge, a, b) per loop side
        near_sharp = np.zeros(ne, bool)
        sharp_v = set()
        if sharp_layer is not None and sharp_band_wu > 0:
            for e in bm.edges:
                if e[sharp_layer]:
                    sharp_v.add(e.verts[0].index)
                    sharp_v.add(e.verts[1].index)
        for e in bm.edges:
            elen[e.index] = e.calc_length()
            for lp in e.link_loops:
                if lp.face.material_index not in detail_mats:
                    continue
                uvs.append((e.index, lp[uvl].uv[:], lp.link_loop_next[uvl].uv[:]))
            if sharp_v and (e.verts[0].index in sharp_v or e.verts[1].index in sharp_v):
                near_sharp[e.index] = True
        if not uvs:
            bm.free()
            break
        ei = np.array([u[0] for u in uvs])
        a = np.array([u[1] for u in uvs])
        b = np.array([u[2] for u in uvs])
        ts = np.array([0.0, 0.25, 0.5, 0.75, 1.0])
        hs = np.stack([relief.sample(a + (b - a) * t) for t in ts], 1)
        lin = hs[:, :1] + (hs[:, 4:] - hs[:, :1]) * ts[None, :]
        err = np.abs(hs - lin).max(1)
        step_ = np.abs(hs[:, 4] - hs[:, 0])
        bad = np.zeros(ne, bool)
        crit = (err > tol_cm) | (step_ > 4 * tol_cm)
        bad[ei[crit]] = True
        bad &= elen > min_len_wu
        if sharp_len_wu > 0:
            bad |= near_sharp & (elen > sharp_len_wu)
        sel = [bm.edges[i] for i in np.nonzero(bad)[0]]
        if not sel:
            bm.free()
            break
        bmesh.ops.subdivide_edges(bm, edges=sel, cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
        bm.to_mesh(me)
        bm.free()
        me.update()
        total += len(sel)
        print("refine", it, "split", len(sel), "faces", len(me.polygons), flush=True)
    return {"splits": total, "faces": len(me.polygons)}


# ------------------------------------------------------------------ contour snapping
def snap_to_contours(obj, ref, band, sdf, levels_px, detail_mats, reach=0.55):
    """move vertices that lie inside one CE triangle (not on its edges, so UV seams and CE's
    own edges are untouched) onto the nearest plate-border level line, within that CE triangle:
    the UV moves to the line (two Newton steps on the signed distance) and the position follows
    the same barycentric point on the CE facet, so texture and geometry stay locked together"""
    me = obj.data
    n = sdf.shape[0]
    rme = ref.data
    rme.calc_loop_triangles()
    nlt = len(rme.loop_triangles)
    tl = np.empty(nlt * 3, np.int32)
    rme.loop_triangles.foreach_get("loops", tl)
    ruv = np.empty(len(rme.loops) * 2, np.float32)
    rme.uv_layers["UVMap"].data.foreach_get("uv", ruv)
    tri_uv = ruv.reshape(-1, 2)[tl.reshape(-1, 3)].astype(np.float64)        # (T,3,2) per loop triangle
    tri_p = band.v[band.t]                                                     # (T,3,3)

    def S(uv):
        x = np.clip(uv[:, 0] * n - 0.5, 0, n - 1.001)
        y = np.clip((1 - uv[:, 1]) * n - 0.5, 0, n - 1.001)
        x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
        fx, fy = x - x0, y - y0
        return (sdf[y0, x0] * (1 - fx) * (1 - fy) + sdf[y0, x0 + 1] * fx * (1 - fy)
                + sdf[y0 + 1, x0] * (1 - fx) * fy + sdf[y0 + 1, x0 + 1] * fx * fy)

    lv, luv = mesh_arrays(me)
    npoly = len(me.polygons)
    lt = np.empty(npoly, np.int32)
    me.polygons.foreach_get("loop_total", lt)
    ce = np.empty(npoly, np.int32)
    me.attributes["ce_tri"].data.foreach_get("value", ce)
    mi = np.empty(npoly, np.int32)
    me.polygons.foreach_get("material_index", mi)
    ltri = band.poly2tri[np.repeat(ce, lt)]
    lmat = np.repeat(mi, lt)
    nv = len(me.vertices)
    # interior: every loop of the vertex has the same CE triangle and UV
    first = np.full(nv, -1, np.int64)
    first[lv[::-1]] = np.arange(len(lv))[::-1]
    same = np.ones(nv, bool)
    np.logical_and.at(same, lv, (ltri == ltri[first[lv]]) & (np.abs(luv - luv[first[lv]]).max(1) < 1e-7)
                      & np.isin(lmat, list(detail_mats)))
    co = FB.mesh_np(obj).astype(np.float64)
    # local edge length (pixels in UV) per vertex
    ev = edge_graph(me)
    elen = np.linalg.norm(co[ev[:, 0]] - co[ev[:, 1]], axis=1)
    emin = np.full(nv, np.inf)
    np.minimum.at(emin, ev[:, 0], elen)
    np.minimum.at(emin, ev[:, 1], elen)
    cand = np.nonzero(same)[0]
    uv0 = luv[first[cand]]
    tri = ltri[first[cand]]
    s0 = S(uv0)
    lev = np.asarray(levels_px)
    dlev = s0[:, None] - lev[None, :]
    pick = np.abs(dlev).argmin(1)
    target = lev[pick]
    # pixels per world unit along the surface, from the CE triangle's UV and 3D areas
    tu, tp = tri_uv[tri], tri_p[tri]
    a2 = np.abs(np.cross(tu[:, 1] - tu[:, 0], tu[:, 2] - tu[:, 0])) * n * n
    a3 = np.linalg.norm(np.cross(tp[:, 1] - tp[:, 0], tp[:, 2] - tp[:, 0]), axis=1)
    px_per_wu = np.sqrt(a2 / np.maximum(a3, 1e-20))
    reach_px = reach * emin[cand] * px_per_wu
    ok = np.abs(s0 - target) < reach_px
    uv = uv0.copy()
    h = 0.5 / n
    for _ in range(3):
        sv = S(uv)
        gx = (S(uv + [h, 0]) - S(uv - [h, 0])) / (2 * h)
        gy = (S(uv + [0, h]) - S(uv - [0, h])) / (2 * h)
        g2 = np.maximum(gx * gx + gy * gy, 1e-6)
        step_ = (sv - target) / g2
        uv = uv - np.stack([gx * step_, gy * step_], 1)
    # barycentric of the new UV in the CE triangle; must stay inside
    e0, e1, d = tu[:, 1] - tu[:, 0], tu[:, 2] - tu[:, 0], uv - tu[:, 0]
    den = e0[:, 0] * e1[:, 1] - e0[:, 1] * e1[:, 0]
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    b1 = (d[:, 0] * e1[:, 1] - d[:, 1] * e1[:, 0]) / den
    b2 = (e0[:, 0] * d[:, 1] - e0[:, 1] * d[:, 0]) / den
    b0 = 1 - b1 - b2
    inside = (b0 > 0.02) & (b1 > 0.02) & (b2 > 0.02)
    newp = b0[:, None] * tp[:, 0] + b1[:, None] * tp[:, 1] + b2[:, None] * tp[:, 2]
    moved = np.linalg.norm(newp - co[cand], axis=1)
    ok &= inside & (moved < reach * emin[cand]) & (np.abs(S(uv) - target) < 0.25)
    # no slivers: a snap that would leave any of the vertex's triangles with under a quarter
    # of its area (or flip it) is undone, a few rounds
    tri_l = lv.reshape(-1, 3)
    def areas(c):
        return np.cross(c[tri_l[:, 1]] - c[tri_l[:, 0]], c[tri_l[:, 2]] - c[tri_l[:, 0]])
    a0 = areas(co)
    a0n = np.linalg.norm(a0, axis=1)
    moved_ok = ok.copy()
    for _ in range(4):
        c2 = co.copy()
        c2[cand[moved_ok]] = newp[moved_ok]
        a1 = areas(c2)
        badf = ((a1 * a0).sum(1) < 0.25 * a0n * a0n)
        if not badf.any():
            break
        badv = np.zeros(nv, bool)
        badv[tri_l[badf].ravel()] = True
        moved_ok &= ~badv[cand]
    ok = moved_ok
    vi = cand[ok]
    co[vi] = newp[ok]
    FB.set_mesh_np(obj, co)
    # every loop of a moved vertex takes the new UV
    newuv = luv.copy()
    vmap = np.full(nv, -1, np.int64)
    vmap[vi] = np.arange(len(vi))
    sel = vmap[lv] >= 0
    newuv[sel] = uv[ok][vmap[lv[sel]]]
    me.uv_layers["UVMap"].data.foreach_set("uv", newuv.astype(np.float32).ravel())
    me.update()
    return {"candidates": int(len(cand)), "snapped": int(len(vi)), "by_level": np.bincount(pick[ok], minlength=len(lev)).tolist()}


# ------------------------------------------------------------------ helpers
def edge_graph(me):
    ev = np.empty(len(me.edges) * 2, np.int32)
    me.edges.foreach_get("vertices", ev)
    return ev.reshape(-1, 2)


def smooth_vertex_normals(obj):
    """angle-weighted normals over all faces (hard edges ignored): one direction per vertex"""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    n = np.array([v.normal[:] for v in bm.verts])
    bm.free()
    return n


def geodesic_band(me, seeds, radius):
    """edge-graph distance from the seed vertices, up to radius (Dijkstra on edge lengths)"""
    import heapq
    nv = len(me.vertices)
    cov = np.empty(nv * 3, np.float32)
    me.vertices.foreach_get("co", cov)
    cov = cov.reshape(-1, 3)
    ev = edge_graph(me)
    adj = [[] for _ in range(nv)]
    ln = np.linalg.norm(cov[ev[:, 0]] - cov[ev[:, 1]], axis=1)
    for (i, j), l_ in zip(ev, ln):
        adj[i].append((j, l_))
        adj[j].append((i, l_))
    dist = np.full(nv, np.inf)
    h = []
    for s in seeds:
        dist[s] = 0.0
        h.append((0.0, s))
    heapq.heapify(h)
    while h:
        d, i = heapq.heappop(h)
        if d > dist[i] or d > radius:
            continue
        for j, l_ in adj[i]:
            nd = d + l_
            if nd < dist[j] and nd <= radius:
                dist[j] = nd
                heapq.heappush(h, (nd, j))
    return dist, adj


def round_hard_edges(obj, radius_wu, iters, factor):
    """constrained Laplacian rounding in a band around the edges marked sharp"""
    me = obj.data
    sharp = np.zeros(len(me.edges), bool)
    if "sharp_edge" in me.attributes:
        me.attributes["sharp_edge"].data.foreach_get("value", sharp)
    ev = edge_graph(me)
    seeds = np.unique(ev[sharp].ravel())
    if len(seeds) == 0:
        return {"sharp_vertices": 0}
    dist, adj = geodesic_band(me, seeds, radius_wu)
    band = np.nonzero(np.isfinite(dist))[0]
    # boundary vertices (open borders) are not moved
    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    border = np.array([any(not e.is_manifold for e in v.link_edges) for v in bm.verts])
    bm.free()
    w = np.clip(1.0 - dist / radius_wu, 0, 1) * factor
    w[border] = 0
    co = FB.mesh_np(obj).astype(np.float64)
    for _ in range(iters):
        new = co.copy()
        for i in band:
            if w[i] <= 0:
                continue
            nb = adj[i]
            if not nb:
                continue
            m = np.mean([co[j] for j, _ in nb], axis=0)
            new[i] = co[i] + w[i] * (m - co[i])
        co = new
    FB.set_mesh_np(obj, co)
    return {"sharp_vertices": int(len(seeds)), "band_vertices": int(len(band))}


def thickness(obj, normals, max_wu):
    """distance along -normal to the mesh's other side (ray cast), capped at max_wu"""
    me = obj.data
    co = FB.mesh_np(obj).astype(np.float64)
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    t = np.full(len(co), max_wu)
    for i, (p, n) in enumerate(zip(co, normals)):
        d = Vector(-n)
        o = Vector(p) + d * 1e-5
        hit = tree.ray_cast(o, d, max_wu)
        if hit[0] is not None:
            t[i] = hit[3]
    return t


def border_loops(obj, mats):
    """vertices on open (one-face) edges of faces with material in mats"""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    out = set()
    for e in bm.edges:
        if len(e.link_faces) == 1 and e.link_faces[0].material_index in mats:
            out.add(e.verts[0].index)
            out.add(e.verts[1].index)
    bm.free()
    return np.array(sorted(out), np.int64)


def add_rim_walls(obj, mats, depth_wu):
    """turn every open border of the given materials inward by a wall of depth_wu (the edge
    thickness of a plate CE modelled as a single sheet); the wall's corners take the border's
    UVs (a sliver: the wall is lit by its geometry)"""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    edges = [e for e in bm.edges if len(e.link_faces) == 1 and e.link_faces[0].material_index in mats]
    if not edges:
        bm.free()
        return 0
    vn = {}
    for e in edges:
        for v in e.verts:
            vn[v] = v.normal.copy()
    ret = bmesh.ops.extrude_edge_only(bm, edges=edges)
    newv = [g for g in ret["geom"] if isinstance(g, bmesh.types.BMVert)]
    # each new vertex was made from one border vertex: find it through its extruded edge
    for v in newv:
        src = None
        for e in v.link_edges:
            o = e.other_vert(v)
            if o in vn:
                src = o
                break
        if src is not None:
            v.co = src.co - vn[src] * depth_wu
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return len(edges)


def set_shading(obj, split_deg):
    me = obj.data
    for p in me.polygons:
        p.use_smooth = True
    bm = bmesh.new()
    bm.from_mesh(me)
    lim = math.radians(split_deg)
    sharp_layer = bm.edges.layers.bool.get("sharp_edge") or bm.edges.layers.bool.new("sharp_edge")
    n = 0
    for e in bm.edges:
        s = (len(e.link_faces) == 2 and e.calc_face_angle(0.0) > lim)
        e[sharp_layer] = s
        n += s
    bm.to_mesh(me)
    bm.free()
    if me.has_custom_normals:
        FB.activate(obj)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    FB.activate(obj)
    m = obj.modifiers.new("wn", "WEIGHTED_NORMAL")
    m.mode = "FACE_AREA"
    m.weight = 50
    m.keep_sharp = True
    bpy.ops.object.modifier_apply(modifier=m.name)
    return n


def ce_weights(obj, ref, band, node_names):
    """per vertex: CE's skin weights interpolated at the vertex's own point of its CE triangle
    (found through the face attribute ce_tri and the vertex's UV, which stays inside that
    triangle): the exact correspondence, not a nearest-surface guess after the relief moved it"""
    rme = ref.data
    rme.calc_loop_triangles()
    nlt = len(rme.loop_triangles)
    tl = np.empty(nlt * 3, np.int32)
    rme.loop_triangles.foreach_get("loops", tl)
    tv = np.empty(nlt * 3, np.int32)
    rme.loop_triangles.foreach_get("vertices", tv)
    ruv = np.empty(len(rme.loops) * 2, np.float32)
    rme.uv_layers["UVMap"].data.foreach_get("uv", ruv)
    tri_uv = ruv.reshape(-1, 2)[tl.reshape(-1, 3)].astype(np.float64)
    tri_v = tv.reshape(-1, 3)
    gi = {g.index: node_names.index(g.name) for g in ref.vertex_groups if g.name in node_names}
    W = np.zeros((len(rme.vertices), len(node_names)))
    for v in rme.vertices:
        for g in v.groups:
            if g.group in gi:
                W[v.index, gi[g.group]] += g.weight
    W /= np.maximum(W.sum(1, keepdims=True), 1e-12)
    me = obj.data
    lv, luv = mesh_arrays(me)
    npoly = len(me.polygons)
    lt = np.empty(npoly, np.int32)
    me.polygons.foreach_get("loop_total", lt)
    ce = np.empty(npoly, np.int32)
    me.attributes["ce_tri"].data.foreach_get("value", ce)
    ltri = band.poly2tri[np.repeat(ce, lt)]
    nv = len(me.vertices)
    first = np.full(nv, -1, np.int64)
    first[lv[::-1]] = np.arange(len(lv))[::-1]
    t = ltri[first]
    uv = luv[first]
    tu = tri_uv[t]
    e0, e1, d = tu[:, 1] - tu[:, 0], tu[:, 2] - tu[:, 0], uv - tu[:, 0]
    den = e0[:, 0] * e1[:, 1] - e0[:, 1] * e1[:, 0]
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    b1 = (d[:, 0] * e1[:, 1] - d[:, 1] * e1[:, 0]) / den
    b2 = (e0[:, 0] * d[:, 1] - e0[:, 1] * d[:, 0]) / den
    b = np.clip(np.stack([1 - b1 - b2, b1, b2], 1), 0, 1)
    b /= np.maximum(b.sum(1, keepdims=True), 1e-12)
    Wv = (b[:, :, None] * W[tri_v[t]]).sum(1)
    # two influences, normalised
    top = np.argsort(-Wv, axis=1)[:, :2]
    w2 = np.take_along_axis(Wv, top, 1)
    w2 = np.where(w2 < 0.001, 0, w2)
    w2 /= np.maximum(w2.sum(1, keepdims=True), 1e-12)
    return top, w2


def transfer_weights(obj, ref, arm, node_names, band=None):
    obj.vertex_groups.clear()
    if band is not None and "ce_tri" in obj.data.attributes:
        for n in node_names:
            obj.vertex_groups.new(name=n)
        top, w2 = ce_weights(obj, ref, band, node_names)
        for i in range(len(obj.data.vertices)):
            for j in range(2):
                if w2[i, j] > 0:
                    obj.vertex_groups[int(top[i, j])].add([i], float(w2[i, j]), "REPLACE")
        obj.parent = arm
        obj.matrix_parent_inverse = ref.matrix_parent_inverse.copy()
        if not any(m.type == "ARMATURE" for m in obj.modifiers):
            am = obj.modifiers.new("Armature", "ARMATURE")
            am.object = arm
        return
    obj.vertex_groups.clear()
    for n in node_names:
        obj.vertex_groups.new(name=n)
    FB.activate(obj)
    dt = obj.modifiers.new("wt", "DATA_TRANSFER")
    dt.object = ref
    dt.use_object_transform = True
    dt.use_vert_data = True
    dt.data_types_verts = {"VGROUP_WEIGHTS"}
    dt.vert_mapping = "POLYINTERP_NEAREST"
    dt.layers_vgroup_select_src = "ALL"
    dt.layers_vgroup_select_dst = "NAME"
    bpy.ops.object.modifier_apply(modifier=dt.name)
    limit_weights(obj)
    obj.parent = arm
    obj.matrix_parent_inverse = ref.matrix_parent_inverse.copy()
    if not any(m.type == "ARMATURE" for m in obj.modifiers):
        am = obj.modifiers.new("Armature", "ARMATURE")
        am.object = arm


def limit_weights(obj):
    FB.activate(obj)
    bpy.ops.object.vertex_group_clean(group_select_mode="ALL", limit=0.001)
    bpy.ops.object.vertex_group_limit_total(group_select_mode="ALL", limit=2)
    bpy.ops.object.vertex_group_normalize_all(group_select_mode="ALL", lock_active=False)
    infl = [len([g for g in v.groups if g.weight > 0]) for v in obj.data.vertices]
    return max(infl), sum(1 for i in infl if i == 0)


# ------------------------------------------------------------------ JMS
def shader_index_map(obj, tmpl):
    names = [m[0].lower() for m in tmpl.materials]
    out = {}
    for i, slot in enumerate(obj.material_slots):
        nm = (slot.material.name if slot.material else "").replace("/", "\\").split("\\")[-1].lower().split(".")[0]
        out[i] = names.index(nm) if nm in names else -1
    return out


def follow_surface(tmpl, keep_tris, moves, lift_wu):
    """the kept effect shells' vertices (the armour's lamps are light-shield quads laid on the
    plates) moved as the remodel moved the armour under them, plus a small lift"""
    from mathutils.kdtree import KDTree
    base, mv = moves
    kd = KDTree(len(base))
    for i, p in enumerate(base):
        kd.insert(p, i)
    kd.balance()
    used = sorted({i for t in keep_tris for i in t.v})
    out = {}
    for i in used:
        v = tmpl.vertices[i]
        p = np.array(v.pos) / 100.0
        near = kd.find_n(p, 6)
        w = np.array([1.0 / max(d, 1e-6) for _, _, d in near])
        off = (w[:, None] * np.array([mv[j] for _, j, _ in near])).sum(0) / w.sum()
        n = np.array(v.normal)
        q = (p + off + n * lift_wu) * 100.0
        out[i] = hf.JmsVertex(v.node0, tuple(float(x) for x in q), v.normal, v.node1, v.weight, v.u, v.v)
    return out


def build_jms(obj, tmpl, keep_tris, keep_moved=None):
    """JMS 8200 from the rest-pose mesh (corner normals, UVs, 2 weights) plus the template's kept
    triangles with their own vertices; nodes, materials, markers and regions are the template's"""
    slot_map = shader_index_map(obj, tmpl)
    names = [n.name for n in tmpl.nodes]
    idx = {n: i for i, n in enumerate(names)}
    vg_name = {g.index: g.name for g in obj.vertex_groups}
    mw = obj.matrix_world
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(me)
    bm.free()
    me.update()
    uv = me.uv_layers["UVMap"].data
    nrm3 = mw.to_3x3().inverted().transposed()
    corner_n = me.corner_normals
    verts, key2i, tris = [], {}, []
    wcache = {}
    for poly in me.polygons:
        ids = []
        for li in poly.loop_indices:
            lp = me.loops[li]
            vi = lp.vertex_index
            if vi not in wcache:
                v = me.vertices[vi]
                gw = sorted(((g.weight, vg_name[g.group]) for g in v.groups
                             if g.weight > 0 and vg_name.get(g.group) in idx), reverse=True)[:2]
                if not gw:
                    gw = [(1.0, names[0])]
                tot = sum(w for w, _ in gw)
                n0 = idx[gw[0][1]]
                n1, w1 = (idx[gw[1][1]], gw[1][0] / tot) if len(gw) > 1 else (-1, 0.0)
                wcache[vi] = (n0, n1, w1, tuple(mw @ v.co * 100.0))
            n0, n1, w1, pw = wcache[vi]
            nw = (nrm3 @ corner_n[li].vector).normalized()
            u, vv = uv[li].uv
            key = (vi, round(nw.x, 3), round(nw.y, 3), round(nw.z, 3), round(u, 6), round(vv, 6))
            if key not in key2i:
                key2i[key] = len(verts)
                verts.append(hf.JmsVertex(n0, pw, tuple(nw), n1, w1, u, vv))
            ids.append(key2i[key])
        tris.append(hf.JmsTriangle(0, slot_map[poly.material_index], (ids[0], ids[2], ids[1])))
    bpy.data.meshes.remove(me)
    remap = {}
    for t in keep_tris:
        vv = []
        for i in t.v:
            if i not in remap:
                remap[i] = len(verts)
                verts.append(keep_moved[i] if keep_moved and i in keep_moved else tmpl.vertices[i])
            vv.append(remap[i])
        tris.append(hf.JmsTriangle(t.region, t.shader, tuple(vv)))
    return hf.Jms(8200, tmpl.checksum, tmpl.nodes, tmpl.materials, tmpl.markers, tmpl.regions, verts, tris)


# ------------------------------------------------------------------ one level
def build_level(name, weld, band, relief, rc, lv, detail_mats, visor_mats):
    rm = rc["remodel"]
    ext = REPORT["extent_wu"]
    new = FB.dup(weld, name)
    new.hide_render = False
    new.vertex_groups.clear()
    for m_ in list(new.modifiers):
        new.modifiers.remove(m_)
    FB.activate(new)
    if lv["subdiv"] > 0:
        m = new.modifiers.new("subd", "SUBSURF")
        m.subdivision_type = "SIMPLE"
        m.levels = m.render_levels = lv["subdiv"]
        m.uv_smooth = "NONE"
        bpy.ops.object.modifier_apply(modifier=m.name)
    # CE's hard edges, from its corner normals (marks sharp_edge where CE's normals split)
    FB.set_corner_normals(new, band.corner_normals(new))
    ref_info = refine(new, relief, lv["tol_cm"], lv["min_len_mm"] / 3048.0, lv["refine_iters"], detail_mats,
                      sharp_band_wu=rm["hard_round_mm"] / 3048.0, sharp_len_wu=lv.get("sharp_len_mm", 0) / 3048.0)
    snap = snap_to_contours(new, REPORT["_ref"], band, REPORT["_sdf"], REPORT["_levels"], detail_mats) \
        if REPORT.get("_sdf") is not None and lv.get("snap", True) else None
    # (the vertices on CE's facets, before any move: the kept effect shells follow the new
    # surface by the move of the nearest of these)
    mw = np.array(new.matrix_world)
    on_facets = FB.mesh_np(new).astype(np.float64) @ mw[:3, :3].T + mw[:3, 3]
    # rounding inside the CE band, capped
    co0 = FB.mesh_np(new).astype(np.float64)
    restore = FB.band_restore(new, band, 1.0, owned=True)
    co1 = FB.mesh_np(new).astype(np.float64)
    d = co1 - co0
    dl = np.linalg.norm(d, axis=1)
    cap = rm["cap_pct"] / 100.0 * ext
    k = np.minimum(1.0, cap / np.maximum(dl, 1e-12))
    # plates keep CE's flat planes (a little rounding only); the undersuit, gloves and other soft
    # forms round fully: the plate weight is the plate border's signed distance at the vertex
    if REPORT.get("_sdf") is not None and "plate_alpha" in rm:
        lv0, uv0 = mesh_arrays(new.data)
        sd = Relief.__new__(Relief)
        sd.h, sd.n = REPORT["_sdf"], REPORT["_sdf"].shape[0]
        sv = sd.sample(uv0)
        acc = np.zeros(len(co0))
        cnt = np.zeros(len(co0))
        np.add.at(acc, lv0, sv)
        np.add.at(cnt, lv0, 1)
        sv = acc / np.maximum(cnt, 1)
        px = REPORT["_px_per_texel"]
        wp = np.clip((sv + px) / (2 * px), 0, 1)
        k = k * (rm.get("suit_alpha", 1.0) * (1 - wp) + rm["plate_alpha"] * wp)
    FB.set_mesh_np(new, co0 + d * k[:, None])
    # (corner normals again: the sharp marks follow CE's splits on the refined mesh)
    FB.set_corner_normals(new, band.corner_normals(new))
    hr = round_hard_edges(new, rm["hard_round_mm"] / 3048.0, lv.get("hard_round_iters", rm["hard_round_iters"]),
                          rm["hard_round_factor"]) if lv.get("hard_round_iters", rm["hard_round_iters"]) > 0 else None
    # relief along the smooth normal
    nrm = smooth_vertex_normals(new)
    cn_ce = band.corner_normals(new)
    h_cm, has = vertex_height(new, relief, skip_mat=visor_mats)
    h_cm *= lv.get("relief_scale", 1.0)
    # smooth the heights over the mesh a little (not the positions): a plate border that runs
    # along a UV seam or a CE edge, where the vertex takes its corners' average, stays even
    ev = edge_graph(new.data)
    for _ in range(rm.get("height_smooth_iters", 2)):
        acc = np.zeros_like(h_cm)
        cnt = np.zeros_like(h_cm)
        np.add.at(acc, ev[:, 0], h_cm[ev[:, 1]])
        np.add.at(acc, ev[:, 1], h_cm[ev[:, 0]])
        np.add.at(cnt, ev[:, 0], 1)
        np.add.at(cnt, ev[:, 1], 1)
        h_cm = np.where(has & (cnt > 0), 0.5 * h_cm + 0.5 * acc / np.maximum(cnt, 1), h_cm)
    th = thickness(new, nrm, 4 * CM_WU * 2)
    inward_cap = rm["inward_thickness_share"] * th
    disp = h_cm * CM_WU
    disp = np.where(disp < 0, -np.minimum(-disp, inward_cap), disp)
    # move along a smoothed normal field (neighbours move nearly in parallel: no folds at a
    # plate's wall), then relax the height wherever a face still turned over
    dn = nrm.copy()
    for _ in range(rm.get("normal_smooth_iters", 6)):
        acc = dn.copy()
        np.add.at(acc, ev[:, 0], dn[ev[:, 1]])
        np.add.at(acc, ev[:, 1], dn[ev[:, 0]])
        dn = acc / np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-12)
    base = FB.mesh_np(new).astype(np.float64)
    me_ = new.data
    npo = len(me_.polygons)
    plt = np.empty(npo, np.int32)
    me_.polygons.foreach_get("loop_total", plt)
    assert (plt == 3).all()
    lvx, _ = mesh_arrays(me_)
    ftri = lvx.reshape(-1, 3)

    def fnorm(c):
        f = np.cross(c[ftri[:, 1]] - c[ftri[:, 0]], c[ftri[:, 2]] - c[ftri[:, 0]])
        return f / np.maximum(np.linalg.norm(f, axis=1, keepdims=True), 1e-20)
    fn0 = fnorm(base)
    folds0 = None
    for it in range(rm.get("fold_relax_iters", 12)):
        co = base + dn * disp[:, None]
        bad = (fnorm(co) * fn0).sum(1) < rm.get("fold_cos", 0.05)
        if folds0 is None:
            folds0 = int(bad.sum())
        if not bad.any():
            break
        # a turned-over face's corners move together (its own mean), and its normal field too
        fm = disp[ftri[bad]].mean(1)
        acc = np.zeros_like(disp)
        cnt = np.zeros_like(disp)
        np.add.at(acc, ftri[bad].ravel(), np.repeat(fm, 3))
        np.add.at(cnt, ftri[bad].ravel(), 1)
        sel = cnt > 0
        disp[sel] = acc[sel] / cnt[sel]
        dm = dn[ftri[bad]].mean(1)
        accn = np.zeros_like(dn)
        np.add.at(accn, ftri[bad].ravel(), np.repeat(dm, 3, 0))
        dn[sel] = accn[sel] / np.maximum(np.linalg.norm(accn[sel], axis=1, keepdims=True), 1e-12)
    badf = (fnorm(co) * fn0).sum(1) < rm.get("fold_cos", 0.05)
    folds_left = int(badf.sum())
    area = 0.5 * np.linalg.norm(np.cross(base[ftri[:, 1]] - base[ftri[:, 0]], base[ftri[:, 2]] - base[ftri[:, 0]]), axis=1)
    print("FOLDS", name, folds_left, "area_med_bad", np.median(area[badf]) if folds_left else 0, "area_med_all", np.median(area),
          "bad_area_share", area[badf].sum() / area.sum(), "relief_at_bad_mean_cm", np.abs(disp[np.unique(ftri[badf].ravel())]).mean() / CM_WU if folds_left else 0)
    # rims: a rolled lip at the armour's open borders
    rim_v = border_loops(new, detail_mats)
    if len(rim_v) and rm.get("rim_lip_cm", 0) > 0:
        dist, _ = geodesic_band(new.data, rim_v, rm["rim_width_mm"] / 3048.0)
        s = np.clip(dist / (rm["rim_width_mm"] / 3048.0), 0, 1)
        lip = np.where(np.isfinite(dist), np.cos(0.5 * np.pi * s), 0.0)
        co += nrm * (rm["rim_lip_cm"] * CM_WU * lip)[:, None]
    FB.set_mesh_np(new, co)
    walls = add_rim_walls(new, detail_mats, rm["rim_wall_cm"] * CM_WU) if rm.get("rim_wall_cm", 0) > 0 else 0
    if rm.get("shading", "ce_relief") == "ce_relief":
        # CE's own corner normals (its smooth groups and hard edges, rounded as CE shades them),
        # turned by the rotation the relief gave each vertex's smooth normal: the plates' walls,
        # bevels, seams and folds light as modelled, the rest as smooth as CE meant it
        n1 = smooth_vertex_normals(new)
        lv_, _ = mesh_arrays(new.data)
        a, b = nrm[lv_], n1[lv_]
        ax = np.cross(a, b)
        sn = np.linalg.norm(ax, axis=1)
        cs = np.clip((a * b).sum(1), -1, 1)
        ax = ax / np.maximum(sn, 1e-12)[:, None]
        v = cn_ce
        rot = (v * cs[:, None] + np.cross(ax, v) * sn[:, None] + ax * (ax * v).sum(1, keepdims=True) * (1 - cs)[:, None])
        rot /= np.maximum(np.linalg.norm(rot, axis=1, keepdims=True), 1e-12)
        sharp = FB.set_corner_normals(new, rot)
    else:
        sharp = set_shading(new, rm["split_deg"])
    ntri = sum(len(p.vertices) - 2 for p in new.data.polygons)
    step("level_" + name, refine=ref_info, snap=snap, restore={k_: restore[k_] for k_ in ("moved_max", "outward_of_facet_max")},
         hard_round=hr, folds_before_relax=folds0, folds_left=folds_left, relief_cm=[float(h_cm.min()), float(h_cm.max())],
         inward_capped=int((h_cm * CM_WU < -inward_cap).sum()), rim_vertices=int(len(rim_v)), rim_wall_edges=walls,
         split_edges=sharp, tris=ntri, verts=len(new.data.vertices))
    moved = FB.mesh_np(new).astype(np.float64) @ mw[:3, :3].T + mw[:3, 3]
    MOVES[name] = (on_facets, moved - on_facets)
    return new


def decimate_level(name, src, ratio):
    o = FB.dup(src, name)
    for m in list(o.modifiers):
        o.modifiers.remove(m)
    FB.activate(o)
    m = o.modifiers.new("dec", "DECIMATE")
    m.decimate_type = "COLLAPSE"
    m.ratio = ratio
    m.use_collapse_triangulate = True
    bpy.ops.object.modifier_apply(modifier=m.name)
    set_shading(o, 50.0)
    return o


def main():
    recipe, height_dir, out = U.script_args()[:3]
    rc = U.load_config(recipe)
    rm = rc["remodel"]
    os.makedirs(os.path.join(out, "export"), exist_ok=True)
    relief = Relief(os.path.join(height_dir, "height.npy"))
    sdf_path = os.path.join(height_dir, "plate_sdf.npy")
    if os.path.exists(sdf_path):
        REPORT["_sdf"] = np.load(sdf_path)
        sj = json.load(open(os.path.join(height_dir, "snap.json")))
        REPORT["_levels"] = sj["levels_px"]
        REPORT["_px_per_texel"] = sj["px_per_texel"]
    U.reset_scene()
    arm, ref = U.import_ref_gltf(P(rc["ref_gltf"]), "superhigh")
    arm.name, ref.name = "rig", "ce_ref"
    REPORT["_ref"] = ref
    jdir = P(rc["ref_jms_dir"])
    tmpl = {lod: hf.read_jms(os.path.join(jdir, "__base %s.jms" % lod))
            for lod in ("superhigh", "high", "medium", "low", "superlow")}
    t0 = tmpl["superhigh"]
    slot_map = shader_index_map(ref, t0)
    keep_names = [s.lower() for s in rc.get("keep_shaders", [])]
    mats = [m[0].lower() for m in t0.materials]
    detail_mats = {i for i, j in slot_map.items() if mats[j] == rc["detail_shader"].lower()}
    visor_mats = {i for i, j in slot_map.items() if mats[j] in [s.lower() for s in rc.get("smooth_shaders", [])]}
    keep_slots = {i for i, j in slot_map.items() if mats[j] in keep_names}
    step("import", ce_verts=len(ref.data.vertices), ce_tris=len(ref.data.polygons), slot_map=slot_map,
         detail_slots=sorted(detail_mats), smooth_slots=sorted(visor_mats))
    if "ce_tri" not in ref.data.attributes:
        ref.data.attributes.new("ce_tri", "INT", "FACE")
    ref.data.attributes["ce_tri"].data.foreach_set("value", np.arange(len(ref.data.polygons), dtype=np.int32))
    weld = FB.dup(ref, "ce_weld")
    bm = bmesh.new()
    bm.from_mesh(weld.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index in keep_slots], context="FACES")
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(weld.data)
    bm.free()
    if weld.data.has_custom_normals:
        FB.activate(weld)
        bpy.ops.mesh.customdata_custom_splitnormals_clear()
    weld.hide_render = True
    co = FB.mesh_np(weld)
    REPORT["extent_wu"] = float((co.max(0) - co.min(0)).max())
    band = FB.CEBand(ref, rm.get("band_max_lean_deg", 60.0), rm.get("band_smooth_deg", 5.0))
    if rm.get("pinned_use_corner_normals", True):
        # a pinned (hard-edge) vertex rounds each face toward that face's own CE corner normal
        # instead of leaving the face flat there (model_build.py's rule): no pillowed facets
        fnc = np.repeat(band.fn[:, None, :], 3, 1)
        pinned = (np.abs(band.pn - fnc).sum(2) < 1e-12)
        band.pn = np.where(pinned[..., None], band.cn, band.pn)
    step("weld", verts=len(weld.data.vertices), faces=len(weld.data.polygons), extent_wu=REPORT["extent_wu"],
         ce_pinned=band.n_pinned)

    node_names = [n.name for n in t0.nodes]
    built = {}
    for lv in rm["lods"]:
        if lv.get("from"):
            o = decimate_level("lod_" + lv["name"], built[lv["from"]], lv["ratio"])
            DECIMATED_FROM[lv["name"]] = lv["from"]
            mx, unw = limit_weights(o)
            step("level_lod_" + lv["name"], decimated_from=lv["from"], ratio=lv["ratio"],
                 tris=sum(len(p.vertices) - 2 for p in o.data.polygons), verts=len(o.data.vertices),
                 max_influences=mx, unweighted=unw)
        else:
            o = build_level("lod_" + lv["name"], weld, band, relief, rc, lv, detail_mats, visor_mats)
            transfer_weights(o, ref, arm, node_names, band)
        built[lv["name"]] = o
    if rc.get("highpoly"):
        hp = build_level("highpoly", weld, band, relief, rc, rc["highpoly"], detail_mats, visor_mats)
        hp.hide_render = True

    # JMS per LOD
    counts = {}
    for lod in ("superhigh", "high", "medium", "low", "superlow"):
        src = rm["lod_map"].get(lod)
        tm = tmpl[lod]
        if src is None:
            continue
        keep_idx = {i for i, m in enumerate(tm.materials) if m[0].lower() in keep_names}
        keep_tris = [t for t in tm.triangles if t.shader in keep_idx]
        lvl = src
        while lvl in DECIMATED_FROM:
            lvl = DECIMATED_FROM[lvl]
        km = follow_surface(tm, keep_tris, MOVES["lod_" + lvl], rm.get("keep_lift_mm", 0.8) / 3048.0) \
            if ("lod_" + lvl) in MOVES and keep_tris else None
        j = build_jms(built[src], tm, keep_tris, km)
        hf.write_jms(os.path.join(out, "export", "__base %s.jms" % lod), j)
        by = {}
        for t in j.triangles:
            by[tm.materials[t.shader][0]] = by.get(tm.materials[t.shader][0], 0) + 1
        vby = {}
        for t in j.triangles:
            vby.setdefault(tm.materials[t.shader][0], set()).update(t.v)
        counts[lod] = {"from": src, "tris": len(j.triangles), "verts": len(j.vertices), "tris_by_shader": by,
                       "verts_by_shader": {k_: len(v) for k_, v in vby.items()},
                       "ce_tris": len(tm.triangles)}
    step("export", lods=counts)
    REPORT["seconds_total"] = round(time.time() - T0, 1)
    for k_ in [k_ for k_ in REPORT if k_.startswith("_")]:
        del REPORT[k_]
    with open(os.path.join(out, "remodel_report.json"), "w") as f:
        json.dump(REPORT, f, indent=1, default=float)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "remodel.blend"))
    print("REMODEL_OK", REPORT["seconds_total"])


if __name__ == "__main__":
    main()
