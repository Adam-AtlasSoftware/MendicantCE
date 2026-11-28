"""PRISM v4: a vehicle remodelled for the mesh store (display.prism_models "max"), in headless Blender.

    blender -b --python vehicle_remodel.py -- <model dump.json> <recipe.json> <out.pmesh> [<report.json>]

From the model as the game draws it (debug.prism_mesh_dump), each permutation's superhigh pieces. The shaders the
recipe names (the opaque ones: hull, tyres, gun) are remodelled; every other piece (decals, lamps, glass, meters,
the blur permutations) is CE's own, as it is, in every level. Per remodelled piece:
  1. welded by position and node (parts the animations move apart stay apart; a UV seam stays a seam, per corner);
  2. hard edges (faces folding more than hard_deg) bevelled in Blender (offset, segments, profile; clamped where
     they would overlap): the boxy panels get a chamfered rim that catches a highlight line;
  3. split into four, subdiv times, each midpoint on the PN curve of its edge where the edge is smooth (its corners'
     normals agree on both sides) and on the straight edge where it is hard: CE's curved surfaces (fenders, tyres,
     barrels) become round, flat panels and the bevels stay as they are;
  4. relief, by the recipe's rule for the shader: "grooves": the base map's dark thin lines set in a few millimetres
     (where the mesh is dense enough to carry them); every move per welded position, capped;
  5. normals per corner from the faces (area-weighted, split above split_deg), tangents from the uvs; each new
     vertex's bones those of its edge's ends (a rigid part's single node).
A tyre ("kind": "tyre") is rebuilt whole as a round lathe of CE's wheel (tyre_piece), its tread's map fitted to CE's
own tread faces (vehicle_tread.py) and its blocks raised from CE's painted tread.
Levels: one per entry of the recipe's "levels" (subdiv per level, relief on or off), then CE's own superhigh drawn
from the last cutoff. Markers, nodes, collision and physics are the game's: nothing here is simulated. The report
says, per tyre, the tread's and the discs' maps as fitted (vehicle_material.py lays the tyre's maps out by them).
Everything is derived from the player's maps: local only.
"""
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy  # noqa: F401
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import packs_env as E  # noqa: E402

sys.path.insert(0, E.TOOLS)
sys.path.insert(0, str(HERE))
import pmesh  # noqa: E402
import pmesh_from_dump as pfd  # noqa: E402
import vehicle_tread as vt  # noqa: E402
from jms_to_pmesh import tangents  # noqa: E402

DETAILS = ("superlow", "low", "medium", "high", "superhigh")


def norm(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)


def weld(P, extra=None, q=1e-5):
    k = np.round(P / q).astype(np.int64)
    if extra is not None:
        k = np.concatenate([k, extra.astype(np.int64)], 1)
    _, first, inv = np.unique(k, axis=0, return_index=True, return_inverse=True)
    return first, inv.reshape(-1)


# ---------------------------------------------------------------- bevel (Blender)

def bevel(P, J, W, UV, T, r):
    """P welded positions (n,3), J/W bones per welded vertex (n,4), UV per corner (t,3,2), T (t,3) clockwise.
    Returns the bevelled mesh in the same form (bones of a new vertex: its source vertex's)."""
    bm = bmesh.new()
    lay = bm.verts.layers.int.new("src")
    uvl = bm.loops.layers.uv.new("uv")
    vs = []
    for i, p in enumerate(P):
        v = bm.verts.new(p.tolist())
        # (stored + 1: a vertex the bevel makes may carry 0, "none known")
        v[lay] = i + 1
        vs.append(v)
    bm.verts.ensure_lookup_table()
    faces = []
    for t, tri in enumerate(T):
        # (Blender winds counter-clockwise about the front: the dump's clockwise order reversed)
        order = [tri[0], tri[2], tri[1]]
        try:
            f = bm.faces.new([vs[k] for k in order])
        except ValueError:
            faces.append(None)
            continue
        for loop, k in zip(f.loops, (0, 2, 1)):
            loop[uvl].uv = UV[t, k].tolist()
        faces.append(f)
    bm.normal_update()
    cosl = math.cos(math.radians(r.get("hard_deg", 35.0)))
    hard = [e for e in bm.edges if len(e.link_faces) == 2 and
            e.link_faces[0].normal.dot(e.link_faces[1].normal) < cosl]
    if hard and r.get("bevel", 0) > 0:
        bmesh.ops.bevel(bm, geom=hard, offset=r["bevel"], offset_type="OFFSET", segments=r.get("segments", 2),
                        profile=r.get("profile", 0.5), affect="EDGES", clamp_overlap=True, loop_slide=True)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.verts.ensure_lookup_table()
    bm.verts.index_update()
    bm.faces.ensure_lookup_table()
    P2 = np.array([v.co[:] for v in bm.verts], np.float64)
    # (the layer is only trusted where the vertex still stands on its source: Blender interpolates an int layer
    # onto the vertices a bevel makes, which can name any vertex)
    def orig(w):
        k = w[lay] - 1
        return k if 0 <= k < len(P) and np.abs(P[k] - np.array(w.co[:])).max() < 1e-6 else -1
    src = np.array([orig(v) for v in bm.verts], np.int64)
    bad = src < 0
    if bad.any():
        # (a vertex the bevel made takes the bones of the nearest original vertex of the faces around it, two rings
        # out: the bevel never moves a vertex off its own part)
        for i in np.nonzero(bad)[0]:
            v = bm.verts[int(i)]
            near = {orig(w) for f in v.link_faces for w in f.verts} - {-1}
            if not near:
                near = {orig(x) for f in v.link_faces for w in f.verts for f2 in w.link_faces for x in f2.verts} - {-1}
            cand = np.array(sorted(near)) if near else np.arange(len(P))
            src[i] = int(cand[np.argmin(np.linalg.norm(P[cand] - P2[i], axis=1))])
    T2, UV2 = [], []
    for f in bm.faces:
        ls = list(f.loops)
        # back to clockwise
        idx = [ls[0].vert.index, ls[2].vert.index, ls[1].vert.index]
        T2.append(idx)
        UV2.append([ls[0][uvl].uv[:], ls[2][uvl].uv[:], ls[1][uvl].uv[:]])
    bm.free()
    return P2, J[src], W[src], np.array(UV2, np.float64).reshape(-1, 3, 2), np.array(T2, np.int64), len(hard)


# ---------------------------------------------------------------- corner normals, subdivision

def face_normals(P, T):
    # (CE's front faces wind clockwise about their outward normal)
    return -np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])


def corner_normals(P, T, split_deg):
    """(t,3,3): each corner's normal from the area-weighted faces at its position within split_deg of its face"""
    fn = face_normals(P, T)
    fu = norm(fn)
    cv = T.reshape(-1)
    nV = len(P)
    corners = np.repeat(np.arange(len(T)), 3)
    order = np.argsort(cv, kind="stable")
    cv_s, cf_s = cv[order], corners[order]
    starts = np.searchsorted(cv_s, np.arange(nV + 1))
    cosl = math.cos(math.radians(split_deg))
    out = np.zeros((len(T) * 3, 3))
    for v in range(nV):
        a, b = starts[v], starts[v + 1]
        if a == b:
            continue
        fs = cf_s[a:b]
        u = fu[fs]
        agree = (u @ u.T) >= cosl
        out[order[a:b]] = agree.astype(np.float64) @ fn[fs]
    return norm(out).reshape(-1, 3, 3)


def subdivide(P, J, W, UV, T, CN, alpha):
    """one 1-to-4 split of welded P (t corners' UV and normals CN per corner); PN midpoints on smooth edges"""
    t = len(T)
    e = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    ce = np.concatenate([CN[:, [0, 1]], CN[:, [1, 2]], CN[:, [2, 0]]])      # (3t, 2, 3) the edge's corner normals
    flip = e[:, 0] > e[:, 1]
    key = np.sort(e, 1)
    ce_k = np.where(flip[:, None, None], ce[:, ::-1], ce)                    # normals at (lo, hi)
    uk, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    # an edge is smooth if every side has the same normals at its ends
    nE = len(uk)
    ref = np.zeros((nE, 2, 3))
    ref[inv] = ce_k
    smooth = np.ones(nE, bool)
    dev = np.einsum("ijk,ijk->ij", ce_k, ref[inv]).min(1) < 0.999
    np.logical_and.at(smooth, inv, ~dev)
    a, b = uk[:, 0], uk[:, 1]
    # an edge between vertices of different bones is not split: a vertex blended between them leaves the edge when
    # the part turns (linear skinning), where CE's own edge only stretches
    split = (J[a] == J[b]).all(1) & (W[a] == W[b]).all(1)
    pa, pb, na, nb = P[a], P[b], ref[:, 0], ref[:, 1]
    d = pb - pa
    pn = (pa + pb) * 0.5 - alpha * (((d * na).sum(1))[:, None] * na - ((d * nb).sum(1))[:, None] * nb) / 8.0
    m = np.where(smooth[:, None], pn, (pa + pb) * 0.5)[split]
    base = len(P)
    P2 = np.concatenate([P, m])
    J2 = np.concatenate([J, J[a[split]]])
    W2 = np.concatenate([W, W[a[split]]])
    mid = np.full(nE, -1)
    mid[split] = base + np.arange(split.sum())
    me = mid[inv].reshape(3, t).T                                           # (t, 3) midpoints of 01, 12, 20 or -1
    hu = (UV + UV[:, [1, 2, 0]]) * 0.5
    gn = norm(CN + CN[:, [1, 2, 0]])
    full = (me >= 0).all(1)
    A, B, C = T[full, 0], T[full, 1], T[full, 2]
    m01, m12, m20 = me[full, 0], me[full, 1], me[full, 2]
    T2 = [np.stack([A, m01, m20], 1), np.stack([m01, B, m12], 1), np.stack([m20, m12, C], 1),
          np.stack([m01, m12, m20], 1)]
    u0, u1, u2, h01, h12, h20 = UV[full, 0], UV[full, 1], UV[full, 2], hu[full, 0], hu[full, 1], hu[full, 2]
    UV2 = [np.stack([u0, h01, h20], 1), np.stack([h01, u1, h12], 1), np.stack([h20, h12, u2], 1),
           np.stack([h01, h12, h20], 1)]
    n0, n1, n2, g01, g12, g20 = CN[full, 0], CN[full, 1], CN[full, 2], gn[full, 0], gn[full, 1], gn[full, 2]
    CN2 = [np.stack([n0, g01, g20], 1), np.stack([g01, n1, g12], 1), np.stack([g20, g12, n2], 1),
           np.stack([g01, g12, g20], 1)]
    # a face with some edges whole: corners turned so the split edges come first, then a fan
    for f in np.nonzero(~full)[0]:
        s = me[f] >= 0
        r = next(k for k in range(3) if s[k] and not s[(k + 2) % 3]) if s.any() else 0
        o = [(r + k) % 3 for k in range(3)]
        v, uv, cn = list(T[f, o]), list(UV[f, o]), list(CN[f, o])
        ms = [me[f, (r + k) % 3] for k in range(3)]
        if not s.any():
            tris = [(0, 1, 2)]
        elif s.sum() == 1:                                                   # 0-1 split at 3
            tris = [(0, 3, 2), (3, 1, 2)]
            v.append(ms[0]); uv.append(hu[f, r]); cn.append(gn[f, r])
        else:                                                                # 0-1 at 3, 1-2 at 4
            tris = [(0, 3, 4), (3, 1, 4), (0, 4, 2)]
            v += [ms[0], ms[1]]
            uv += [hu[f, r], hu[f, (r + 1) % 3]]
            cn += [gn[f, r], gn[f, (r + 1) % 3]]
        for tri in tris:
            T2.append(np.array([[v[i] for i in tri]]))
            UV2.append(np.array([[uv[i] for i in tri]]))
            CN2.append(np.array([[cn[i] for i in tri]]))
    return P2, J2, W2, np.concatenate(UV2), np.concatenate(T2), np.concatenate(CN2)


# ---------------------------------------------------------------- relief

class Map:
    def __init__(self, png, fine_px, coarse_px, local_px=8):
        img = bpy.data.images.load(str(png))
        w, h = img.size
        px = np.array(img.pixels[:], np.float64).reshape(h, w, 4)[::-1, :, :3]  # (top row first, as uv v=0)
        bpy.data.images.remove(img)
        lum = vt.luminance(px)
        self.h, self.w = lum.shape
        f = lum - vt.box_blur(lum, max(fine_px, 1))
        c = lum - vt.box_blur(lum, max(coarse_px, 1))
        self.fine = np.clip(f / max(f.std() * 2.5, 1e-6), -1, 1)
        self.coarse = np.clip(c / max(c.std() * 2.5, 1e-6), -1, 1)
        # (the painted relief, 0 cut in to 1 standing: vehicle_material.py's normal map is made of the same)
        self.height = vt.relief_height(lum, local_px)

    def sample(self, field, uv):
        return vt.sample(field, uv)


def per_vertex(values_corner, T, n):
    s = np.zeros(n)
    c = np.zeros(n)
    np.add.at(s, T.reshape(-1), values_corner.reshape(-1))
    np.add.at(c, T.reshape(-1), 1.0)
    return s / np.maximum(c, 1)


def relief(P, J, UV, T, CN, rule, maps):
    """the displaced positions (per welded vertex)"""
    if not rule:
        return P
    m = maps[rule["map"]]
    n = len(P)
    uvc = UV.reshape(-1, 2)
    nrm_c = CN.reshape(-1, 3)
    vn = np.zeros((n, 3))
    np.add.at(vn, T.reshape(-1), nrm_c)
    vn = norm(vn)
    fine = per_vertex(m.sample(m.fine, uvc), T, n)
    coarse = per_vertex(m.sample(m.coarse, uvc), T, n)
    move = np.zeros(n)
    if rule["kind"] == "tread":
        # each wheel: its node's vertices, axle = their least-spread direction
        for node in np.unique(J[:, 0]):
            sel = J[:, 0] == node
            if sel.sum() < 8:
                continue
            Q = P[sel]
            c = Q.mean(0)
            w_, v_ = np.linalg.eigh(np.cov((Q - c).T))
            axle = v_[:, 0]
            along = np.abs(vn[sel] @ axle)
            tread = np.clip((0.55 - along) / 0.25, 0, 1)
            side = np.clip((along - 0.6) / 0.25, 0, 1)
            radial = (Q - c) - np.outer((Q - c) @ axle, axle)
            rr = np.linalg.norm(radial, axis=1)
            rmax = rr.max()
            hub = np.clip((0.62 * rmax - rr) / (0.15 * rmax), 0, 1)
            mv = tread * (rule["tread"] * (0.6 * fine[sel] + 0.4 * coarse[sel]))
            mv += side * hub * rule["hub"] * np.minimum(coarse[sel], 0.0)
            move[sel] = mv
    elif rule["kind"] == "grooves":
        move = rule["depth"] * np.minimum(fine, 0.0) + rule.get("raise", 0.0) * np.maximum(coarse, 0.0)
    move = np.clip(move, -rule.get("cap_in", 0.01), rule.get("cap_out", 0.004))
    return P + vn * move[:, None]


# ---------------------------------------------------------------- tyres: a lathe on CE's wheel

def tyre_piece(raw, T, r, lv, maps, rep):
    """each wheel (a node's vertices) rebuilt as a round lathe of CE's radius and width: a rounded shoulder, the
    side wall and a dished hub; the tread's uvs CE's tread strip as CE lays it (fitted to CE's own tread faces,
    vehicle_tread.py), the hub's, side wall's and shoulder's CE's hub disc as CE projects it (fitted to its side
    faces), the shoulder folded into its rubber band (r["rubber_r"] of the radius); with lv["relief"] and
    r["tread_map"], the tread's blocks raised from CE's painted tread (the map's relief, r["depth"] from a groove
    to a block's top at CE's radius, band-limited to the lathe's spacing); one node per wheel. rep["tyres"]: each
    wheel's maps as fitted"""
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    a = pfd.Attributes(raw)
    seg = lv.get("segments", 112)
    depth = r.get("depth", 0.012)
    prof = np.array(r.get("profile", [[-0.62, 0.0], [-0.62, 0.20], [-0.74, 0.30], [-0.90, 0.40], [-1.0, 0.58],
                                       [-1.0, 0.80], [-0.97, 0.90], [-0.89, 0.97], [-0.80, 1.0]]), np.float64)
    tread_a = np.linspace(-0.80, 0.80, r.get("tread_steps", 11))[1:-1]
    full = np.concatenate([prof, np.stack([tread_a, np.ones(len(tread_a))], 1),
                           (prof * [-1, 1])[::-1]])
    # (the tread rows proper: the shoulder's last point stays whole, so the side wall's shading is not pulled by the
    # grooves)
    is_tread = np.zeros(len(full), bool)
    is_tread[len(prof):len(prof) + len(tread_a)] = True
    tm = maps.get(r.get("tread_map", "")) if lv.get("relief", True) else None
    Ps, UVs, Js, Ts = [], [], [], []
    wheels = []
    base = 0
    for node in np.unique(a.joints[:, 0]):
        sel = a.joints[:, 0] == node
        if sel.sum() < 8:
            continue
        idx = np.nonzero(sel)[0]
        tmask = np.isin(T[:, 0], idx)
        Tn = T[tmask]
        Q = a.position[sel]
        c = (Q.max(0) + Q.min(0)) * 0.5
        # (the side faces' normal: the vertices' spread alone tilts the axle a few degrees, CE's hub crowding its
        # middle, and a tilted lathe stands proud of CE's tyre at its edges and sinks into the ground)
        axle = vt.wheel_axle(a.position, Tn)
        e1 = norm(np.cross(axle, [0.0, 0.0, 1.0] if abs(axle[2]) < 0.9 else [1.0, 0.0, 0.0]))
        e2 = np.cross(axle, e1)
        ax = (Q - c) @ axle
        c = c + axle * (ax.max() + ax.min()) * 0.5
        ax = (Q - c) @ axle
        half = float(np.abs(ax).max())
        rad = np.linalg.norm((Q - c) - np.outer(ax, axle), axis=1)
        R = float(rad.max())
        th = np.linspace(0, 2 * np.pi, seg, endpoint=False)
        rows = []
        for pa, pr in full:
            rows.append(c + np.outer(np.cos(th) * pr * R, e1) + np.outer(np.sin(th) * pr * R, e2) + axle * (pa * half))
        G = np.stack(rows)  # (profile, seg, 3)
        nP = len(full)
        P = G.reshape(-1, 3)
        tri = []
        for k in range(nP - 1):
            for s in range(seg):
                s2 = (s + 1) % seg
                v00, v01, v10, v11 = k * seg + s, k * seg + s2, (k + 1) * seg + s, (k + 1) * seg + s2
                tri += [[v00, v10, v11], [v00, v11, v01]]
        tri = np.array(tri, np.int64)
        # (rings at radius 0, the hub's centre, collapse: their triangles with two corners there go)
        zero = np.linalg.norm(P - c - np.outer((P - c) @ axle, axle), axis=1) < 1e-9
        if zero.any():
            # (weld each centre ring to one vertex)
            for k in range(nP):
                if zero[k * seg]:
                    tri = np.where((tri >= k * seg) & (tri < (k + 1) * seg), k * seg, tri)
            tri = tri[(tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2]) & (tri[:, 2] != tri[:, 0])]
        # clockwise about the outward normal (CE's front faces)
        # (outward is the profile's own: its segment from row k to k+1 turned a quarter, (-dr, da) in (axle, radius),
        # the tyre's body on its left all the way round; a guess between axial and radial left the side wall's faces
        # wound either way, and the game culls the inward half: slots one sees through)
        fn = face_normals(P, tri)
        cen = P[tri].mean(1) - c
        radial = norm(cen - np.outer(cen @ axle, axle))
        k_row = np.minimum(tri.min(1) // seg, nP - 2)
        dprof = full[k_row + 1] - full[k_row]
        out_dir = np.outer(-dprof[:, 1], axle) + dprof[:, 0:1] * radial
        flip = (fn * out_dir).sum(1) < 0
        tri[flip] = tri[flip][:, [0, 2, 1]]
        # uvs (per face, below): the tread's CE's tread strip, the hub's, side wall's and shoulder's CE's hub disc,
        # held inside its rubber band so the shoulder is plain rubber, not the disc's ring of block ends smeared into
        # bright slots; the nearest point of CE's wheel where neither can be fitted
        bvh = BVHTree.FromPolygons([Vector(p) for p in a.position], [list(t) for t in Tn])
        UV = np.zeros((len(P), 2))
        for i, p in enumerate(P):
            loc, nrm, fi, dist = bvh.find_nearest(Vector(p))
            if fi is None:
                continue
            tt = Tn[fi]
            A_, B_, C_ = a.position[tt[0]], a.position[tt[1]], a.position[tt[2]]
            q = np.array(loc)
            v0, v1, v2 = B_ - A_, C_ - A_, q - A_
            d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
            den = d00 * d11 - d01 * d01
            if abs(den) < 1e-20:
                UV[i] = a.uv[tt[0]]
                continue
            bv = (d11 * d20 - d01 * d21) / den
            bw = (d00 * d21 - d01 * d20) / den
            UV[i] = (1 - bv - bw) * a.uv[tt[0]] + bv * a.uv[tt[1]] + bw * a.uv[tt[2]]
        q_all = P - c
        wheel = {"node": int(node), "radius": R, "half_width": half}
        # the tread's map from CE's tread faces: u across the width, v around, so many repeats a turn, so the painted
        # blocks run straight and the relief raised from them stays whole
        UVt = None
        tf = vt.tread_faces(a.position, Tn, c, axle)
        if tf.sum() >= 4:
            corners = np.unique(Tn[tf])
            fit = vt.fit_tread_map(a.position[corners] - c, a.uv[corners], axle, e1, e2, half)
            if fit["rms"] > r.get("fit_rms", 0.01):
                raise SystemExit("node %d: the tread's map fits CE's tread to %.4f only" % (node, fit["rms"]))
            UVt = vt.tread_uv(fit, q_all, axle, e1, e2)
            wheel["tread"] = fit
        # the disc: per side of the wheel, uv an affine map of the position across the axle (CE's side faces)
        UVd = None
        # (past the rubber band's outer edge the shoulder folds back into the band, r["shoulder_fold"] as far in
        # as it is out, so the band's grain stays whole; held on the band's edge, the whole shoulder took one ring
        # of texels and drew its grain out into radial streaks)
        rubber = r.get("rubber_r", 0.78) * R
        rad_all = np.linalg.norm(q_all - np.outer(q_all @ axle, axle), axis=1)
        folded = np.where(rad_all > rubber, rubber - (rad_all - rubber) * r.get("shoulder_fold", 0.4), rad_all)
        scale = folded / np.maximum(rad_all, 1e-12)
        X_all = np.stack([q_all @ e1 * scale, q_all @ e2 * scale, np.ones(len(P))], 1)
        side_all = np.sign(q_all @ axle)
        sf = vt.side_faces(a.position, Tn, axle)
        if sf.sum() >= 2:
            corners = np.unique(Tn[sf])
            Qs = a.position[corners] - c
            UVd = np.array(UV)
            wheel["discs"] = []
            for s in (-1.0, 1.0):
                one = np.sign(Qs @ axle) == s
                if one.sum() < 3:
                    continue
                Xs = np.stack([Qs[one] @ e1, Qs[one] @ e2, np.ones(one.sum())], 1)
                A = vt.fit_disc_map(Xs, a.uv[corners][one])
                UVd[side_all == s] = X_all[side_all == s] @ A
                # (the disc's centre in the map and CE's radius there: the tyre's material keys its hub to it)
                wheel["discs"].append({"side": s, "centre": A[2].tolist(),
                                       "radius_uv": float(np.sqrt(abs(np.linalg.det(A[:2]))) * R)})
        ring = np.repeat(is_tread, seg)
        if UVt is not None:
            UV[ring] = UVt[ring]
        if tm is not None and UVt is not None:
            # the painted relief band-limited to the lathe's spacing (its texels a segment around, halved): a groove
            # is never narrower than the faces that carry it, so nothing chips or sparkles; the fine edge is the
            # tyre's normal map's (vehicle_material.py)
            texels = tm.h * abs(wheel["tread"]["k"]) / seg
            field = vt.box_blur(tm.height, max(int(round(texels * 0.5)), 1))
            h = tm.sample(field, UV[ring])
            idx_t = np.nonzero(ring)[0]
            q = P[idx_t] - c
            P[idx_t] -= norm(q - np.outer(q @ axle, axle)) * (depth * (1.0 - h))[:, None]
        # per face, one map for all three corners: a face touching the tread the tread's, any other the disc's, so the
        # seam between them runs along an edge instead of a face squeezing the map between two places into a sliver
        UVf = UV[tri]
        on_tread = ring[tri].any(1)
        if UVt is not None:
            UVf[on_tread] = UVt[tri[on_tread]]
        if UVd is not None:
            UVf[~on_tread] = UVd[tri[~on_tread]]
        Ps.append(P)
        UVs.append(UVf)
        Js.append(np.full(len(P), node))
        Ts.append(tri + base)
        base += len(P)
        wheels.append(wheel)
    rep["tyres"] = wheels
    P = np.concatenate(Ps)
    J = np.zeros((len(P), 4), np.int64)
    J[:] = np.concatenate(Js)[:, None]
    W = np.zeros((len(P), 4))
    W[:, 0] = 1.0
    Tw = np.concatenate(Ts)
    UVc = np.concatenate(UVs)
    # (the tread's v wraps a whole number of times a turn: the triangle across the turn's seam takes its first
    # corner's repeat for all three, a seam per corner rather than the whole map squeezed into one triangle)
    UVc[:, :, 1] += np.round(UVc[:, :1, 1] - UVc[:, :, 1])
    return P, J, W, UVc, Tw


# ---------------------------------------------------------------- the model

def piece_input(dump, blob, parts):
    raws, tris, base = [], [], 0
    for part in parts:
        raw, t = pfd.part_arrays(dump, blob, part)
        raws.append(raw)
        tris.append(t + base)
        base += len(raw)
    return np.concatenate(raws), np.concatenate(tris)


def remodel_piece(raw, T, r, lv, maps, rep):
    if r.get("kind") == "tyre":
        P, J, W, UV, Tw = tyre_piece(raw, T, r, lv, maps, rep)
        CN = corner_normals(P, Tw, r.get("split_deg", 50.0))
        return finish(P, J, W, UV, Tw, CN)
    a = pfd.Attributes(raw)
    first, inv = weld(a.position, a.joints[:, :2])
    P = a.position[first]
    J, W = a.joints[first], a.weights[first]
    UV = a.uv[T]
    Tw = inv[T]
    keep = (Tw[:, 0] != Tw[:, 1]) & (Tw[:, 1] != Tw[:, 2]) & (Tw[:, 2] != Tw[:, 0])
    Tw, UV = Tw[keep], UV[keep]
    nb = 0
    if lv.get("bevel", True):
        P, J, W, UV, Tw, nb = bevel(P, J, W, UV, Tw, r)
        # (the bevel can leave coincident vertices: welded again by position and node)
        first, inv = weld(P, J[:, :2])
        P, J, W = P[first], J[first], W[first]
        Tw = inv[Tw]
        keep = (Tw[:, 0] != Tw[:, 1]) & (Tw[:, 1] != Tw[:, 2]) & (Tw[:, 2] != Tw[:, 0])
        Tw, UV = Tw[keep], UV[keep]
    CN = corner_normals(P, Tw, r.get("split_deg", 40.0))
    sub = lv.get("subdiv", 0)
    sub = sub.get(str(r["_shader"]), 0) if isinstance(sub, dict) else sub
    for _ in range(sub):
        P, J, W, UV, Tw, CN = subdivide(P, J, W, UV, Tw, CN, r.get("alpha", 0.75))
    if lv.get("relief", True):
        P = relief(P, J, UV, Tw, CN, r.get("relief"), maps)
    CN = corner_normals(P, Tw, r.get("split_deg", 40.0))
    rep["bevelled_edges"] = rep.get("bevelled_edges", 0) + nb
    return finish(P, J, W, UV, Tw, CN)


def finish(P, J, W, UV, Tw, CN):
    # per-corner vertices (position, normal, uv)
    flat = Tw.reshape(-1)
    k = np.concatenate([flat[:, None].astype(np.float64), np.round(CN.reshape(-1, 3) * 1000.0),
                        np.round(UV.reshape(-1, 2) * 1e5)], 1).astype(np.int64)
    _, f2, inv2 = np.unique(k, axis=0, return_index=True, return_inverse=True)
    out = type("A", (), {"__len__": lambda s: len(s.position)})()
    out.position = P[flat[f2]]
    out.normal = CN.reshape(-1, 3)[f2]
    out.uv = UV.reshape(-1, 2)[f2]
    out.joints = J[flat[f2]]
    out.weights = W[flat[f2]]
    tri = inv2.reshape(-1).reshape(-1, 3)
    out.tangent, out.sign = tangents(out.position, out.normal, out.uv, tri)
    return out, tri


def main():
    a = sys.argv[sys.argv.index("--") + 1:]
    dump_path, recipe_path, out_path = Path(a[0]), Path(a[1]), Path(a[2])
    report_path = Path(a[3]) if len(a) > 3 else None
    dump = json.loads(dump_path.read_text())
    blob = dump_path.with_suffix(".bin").read_bytes()
    recipe = json.loads(recipe_path.read_text())
    root = Path(recipe["maps_root"])
    maps = {k: Map(root / v["png"], v.get("fine_px", 2), v.get("coarse_px", 8), v.get("local_px", 8))
            for k, v in recipe.get("maps", {}).items()}
    shaders = {int(k): v for k, v in recipe["shaders"].items()}
    groups = {}
    for ri, region in enumerate(dump["regions"]):
        for pi, perm in enumerate(region["permutations"]):
            g = perm["geometries"][4]
            if not 0 <= g < len(dump["geometries"]):
                continue
            for part in dump["geometries"][g]["parts"]:
                if part["vertex_count"] <= 0 or part["flags"] & 1:
                    continue
                groups.setdefault((ri, pi, part["shader"]), []).append(part)
    blur_perm = {(ri, pi) for ri, region in enumerate(dump["regions"]) for pi, perm in enumerate(region["permutations"])
                 if perm.get("name", "").startswith("~")}
    levels_spec = recipe["levels"] + [{"ce": True}]
    cut = recipe["cutoffs"]
    vertices, indices, pieces, levels = [], [], [], []
    vat = iat = 0
    report = {"tag": dump["tag"], "levels": []}
    for li, lv in enumerate(levels_spec):
        lv0, li0, pf = vat, iat, len(pieces)
        pts, tcount, by_shader = [], 0, {}
        for (ri, pi, sh), parts in sorted(groups.items()):
            raw, T = piece_input(dump, blob, parts)
            r = shaders.get(sh)
            if lv.get("ce") or r is None or (ri, pi) in blur_perm:
                at = pfd.Attributes(raw)
                tri = T
            else:
                rep = {}
                at, tri = remodel_piece(raw, T, dict(r, _shader=sh), lv, maps, rep)
                if li == 0 and "tyres" in rep:
                    report.setdefault("tyres", {})[str(sh)] = rep["tyres"]
            v = pfd.to_vertices(at)
            vertices.append(v)
            indices.append(tri.astype(np.uint32).reshape(-1))
            pieces.append(pmesh.Piece(ri, pi, sh, 0, 0, vat, len(v), iat, tri.size))
            vat += len(v)
            iat += tri.size
            pts.append(at.position)
            tcount += len(tri)
            by_shader[str(sh)] = by_shader.get(str(sh), 0) + len(tri)
        Pall = np.concatenate(pts)
        c = (Pall.max(0) + Pall.min(0)) * 0.5
        levels.append(pmesh.Level(cut[li] if li + 1 < len(levels_spec) else 0.0, pf, len(pieces) - pf, lv0, vat - lv0,
                                  li0, iat - li0, tuple(float(x) for x in c),
                                  float(np.linalg.norm(Pall - c, axis=1).max())))
        report["levels"].append({"spec": lv, "triangles": int(tcount), "by_shader": by_shader})
        print("level", li, lv, "triangles", tcount, by_shader, flush=True)
    allv = np.concatenate(vertices)
    bones = [pmesh.Bone(n["name"], n["parent"], i, b, inv) for i, (n, (b, inv)) in
             enumerate(zip(dump["nodes"], pfd.node_matrices(dump)))]
    radius = float(np.linalg.norm(allv["position"].astype(np.float64), axis=1).max()) * 1.0001
    mesh = pmesh.Mesh(len(dump["nodes"]), dump["node_crc"], bones, levels, pieces, allv, np.concatenate(indices),
                      (0.0, 0.0, 0.0), radius, {"source": "prism_packs/art/prism_cast/vehicle_remodel.py",
                                                "tag": dump["tag"], "levels": report["levels"]})
    problems = pmesh.check(mesh, dump)
    for p in problems:
        print("problem:", p)
    if problems:
        sys.exit(1)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    report["bytes"] = pmesh.write_pmesh(out_path, mesh)
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=1))
    print("VEHICLE_OK", out_path, report["bytes"], flush=True)


main()
