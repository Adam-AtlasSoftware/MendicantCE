#!/usr/bin/env python3
"""PRISM: a scenery boulder from a CC0 scanned rock mesh, fitted to CE's boulder, for the mesh store
(display.prism_models "max"). A prototype (prism-v4-rock-scan, round 2, option C).

    python3 -I rock_scan_boulder.py prepare <scan.gltf> <out.npz>
        the scan read as data only (positions and triangles of its first mesh, welded, Y up turned Z up)
    blender -b --factory-startup --python rock_scan_boulder.py -- decimate <in.npz> <out.npz> <triangles,...>
        Blender's collapse decimation to each level's triangle count
    python3 rock_scan_boulder.py fit <levels.npz> <model dump.json> <out.pmesh> [--cutoffs 140,45,0]
        [--cap-in 0.15] [--cap-out 0.035] [--report out.json]

The fit, per level: the scan turned about the vertical to the yaw where its outline best matches CE's, its box
scaled to CE's box (CE's model is the collision's shape); then, along rays from a point inside, each vertex's
distance kept as the scan's, less the scan's own median difference, within cap_in (a share of the size) inside and
cap_out outside CE's surface rounded (CE's facets split and set on their PN curves, as rock_remodel.py rounds them),
so the silhouette is CE's to within a few per cent and the scan's breaks, ledges and grain are set into it. Each
vertex takes CE's base map coordinates where its ray meets CE's rounded surface (a triangle across one of CE's uv
seams takes all three from one side): the colour is CE's painted boulder, the shape the scan's. Normals per corner,
smooth up to 60 degrees; tangents from the uvs. Collision, physics and markers stay the stock model's.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np


def cmd_prepare(a):
    path = Path(a.gltf)
    g = json.loads(path.read_text())
    ct = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
    nc = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
    bufs = []
    for b in g["buffers"]:
        uri = Path(b["uri"]).name  # (a file beside the glTF, never a path elsewhere)
        bufs.append((path.parent / uri).read_bytes())

    def acc(i):
        x = g["accessors"][i]
        bv = g["bufferViews"][x["bufferView"]]
        off = bv.get("byteOffset", 0) + x.get("byteOffset", 0)
        n = nc[x["type"]]
        dt = np.dtype(ct[x["componentType"]])
        stride = bv.get("byteStride", 0) or dt.itemsize * n
        if stride != dt.itemsize * n:
            raise SystemExit("interleaved buffers are not read")
        return np.frombuffer(bufs[bv["buffer"]], dt, x["count"] * n, off).reshape(x["count"], n)

    prim = g["meshes"][0]["primitives"][0]
    P = acc(prim["attributes"]["POSITION"]).astype(np.float64)
    T = acc(prim["indices"]).astype(np.int64).reshape(-1, 3)
    if T.max() >= len(P):
        raise SystemExit("an index past the vertices")
    # glTF is Y up: (x, y, z) -> (x, -z, y), a rotation (the winding is kept)
    P = np.stack([P[:, 0], -P[:, 2], P[:, 1]], 1)
    _, first, inv = np.unique(np.round(P / 1e-6).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    P = P[first]
    T = inv[T]
    T = T[(T[:, 0] != T[:, 1]) & (T[:, 1] != T[:, 2]) & (T[:, 0] != T[:, 2])]
    np.savez(a.out, P=P, T=T)
    print("prepared", a.gltf, len(P), "vertices", len(T), "triangles", P.min(0).round(3), P.max(0).round(3))


def cmd_decimate(args):
    import bpy
    src, out, counts = args[0], args[1], [int(x) for x in args[2].split(",")]
    d = np.load(src)
    P, T = d["P"], d["T"]
    levels = {}
    for i, n in enumerate(counts):
        me = bpy.data.meshes.new("scan")
        me.vertices.add(len(P))
        me.vertices.foreach_set("co", P.astype(np.float32).ravel())
        me.loops.add(len(T) * 3)
        me.loops.foreach_set("vertex_index", T.astype(np.int32).ravel())
        me.polygons.add(len(T))
        me.polygons.foreach_set("loop_start", (np.arange(len(T)) * 3).astype(np.int32))
        me.update(calc_edges=True)
        ob = bpy.data.objects.new("scan", me)
        bpy.context.scene.collection.objects.link(ob)
        mod = ob.modifiers.new("dec", "DECIMATE")
        mod.decimate_type = "COLLAPSE"
        mod.ratio = min(1.0, n / len(T))
        mod.use_collapse_triangulate = True
        dg = bpy.context.evaluated_depsgraph_get()
        ev = ob.evaluated_get(dg)
        m2 = ev.to_mesh()
        m2.calc_loop_triangles()
        V = np.empty(len(m2.vertices) * 3, np.float32)
        m2.vertices.foreach_get("co", V)
        LT = np.empty(len(m2.loop_triangles) * 3, np.int32)
        m2.loop_triangles.foreach_get("vertices", LT)
        levels[f"P{i}"] = V.reshape(-1, 3).astype(np.float64)
        levels[f"T{i}"] = LT.reshape(-1, 3).astype(np.int64)
        print("level", i, len(m2.vertices), "vertices", len(m2.loop_triangles), "triangles", flush=True)
        ev.to_mesh_clear()
        bpy.data.objects.remove(ob)
        bpy.data.meshes.remove(me)
    np.savez(out, n=len(counts), **levels)


# ---------- the fit

def _ray_hits(O, D, A, B, C):
    """for each ray (O[i] or one O, D[i]): the farthest hit's distance, triangle and barycentrics (u, v) on triangles A, B, C
    (Moller-Trumbore, both faces); distance inf where none"""
    n = len(D)
    best_t = np.full(n, -np.inf)
    best_f = np.full(n, -1)
    best_u = np.zeros(n)
    best_v = np.zeros(n)
    e1 = B - A
    e2 = C - A
    step = max(1, int(4e6 // max(len(A), 1)))
    O = np.broadcast_to(np.asarray(O, np.float64), D.shape)
    for s0 in range(0, n, step):
        d = D[s0:s0 + step]
        pv = np.cross(d[:, None, :], e2[None, :, :])
        det = (e1[None] * pv).sum(2)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        tv = O[s0:s0 + step, None, :] - A[None]
        u = (tv * pv).sum(2) * inv
        qv = np.cross(tv, e1[None])
        v = (d[:, None, :] * qv).sum(2) * inv
        t = (e2[None] * qv).sum(2) * inv
        hit = ok & (u >= -1e-9) & (v >= -1e-9) & (u + v <= 1 + 1e-9) & (t > 1e-9)
        tt = np.where(hit, t, -np.inf)
        f = tt.argmax(1)
        r = np.arange(len(d))
        best_t[s0:s0 + step] = tt[r, f]
        best_f[s0:s0 + step] = np.where(np.isfinite(tt[r, f]), f, -1)
        best_u[s0:s0 + step] = u[r, f]
        best_v[s0:s0 + step] = v[r, f]
    best_t[best_f < 0] = np.inf
    return best_t, best_f, best_u, best_v


def soft_clamp(d, lo, hi):
    return np.where(d < 0, lo * np.tanh(d / lo), hi * np.tanh(d / hi))


def cmd_fit(a):
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import rock_remodel as rr
    pmesh, pfd, tangents = rr.pmesh, rr.pfd, rr.tangents
    lv = np.load(a.levels)
    dump = json.loads(Path(a.dump).read_text())
    blob = Path(a.dump).with_suffix(".bin").read_bytes()
    groups = {}
    for ri, region in enumerate(dump["regions"]):
        for pi, perm in enumerate(region["permutations"]):
            g = perm["geometries"][4]
            for part in dump["geometries"][g]["parts"]:
                if part["vertex_count"] > 0 and not part["flags"] & 1:
                    groups.setdefault((ri, pi, part["shader"]), []).append(part)
    if len(groups) != 1:
        raise SystemExit(f"{dump['tag']}: {len(groups)} pieces; one is fitted")
    (key, parts), = groups.items()
    raws, tris, base = [], [], 0
    for part in parts:
        raw, t = pfd.part_arrays(dump, blob, part)
        raws.append(raw)
        tris.append(t + base)
        base += len(raw)
    raw = np.concatenate(raws)
    Tc = np.concatenate(tris)
    Pc = raw[:, 0:3].copy()
    UVc = raw[:, 12:14].copy()
    w = rr.weld_keys(Pc)
    Nc = rr.norm(rr.welded_mean(raw[:, 3:6], w))
    for _ in range(3):
        Pc, Nc, UVc, Tc = rr.subdivide(Pc, Nc, UVc, Tc, 0.75)
    lo_c, hi_c = Pc.min(0), Pc.max(0)
    size = float((hi_c - lo_c).max())
    cen = np.array([(lo_c[0] + hi_c[0]) * 0.5, (lo_c[1] + hi_c[1]) * 0.5, lo_c[2] + 0.4 * (hi_c[2] - lo_c[2])])
    hz = hi_c[2] - lo_c[2]

    def axis_at(Q):
        """the rays' origins: on the vertical axis at the point's own height (within the middle of the rock), so the
        sides are met square on and the base is not pulled toward a point"""
        o = np.tile(cen, (len(Q), 1))
        o[:, 2] = np.clip(Q[:, 2], lo_c[2] + 0.2 * hz, hi_c[2] - 0.3 * hz)
        return o
    A, B, C = Pc[Tc[:, 0]], Pc[Tc[:, 1]], Pc[Tc[:, 2]]
    cut = [float(x) for x in a.cutoffs.split(",")]
    nlev = int(lv["n"])
    report = {"tag": dump["tag"], "size_wu": size, "levels": []}

    # the yaw (and mirror) of the finest level whose outline best matches CE's
    P0 = lv["P0"]

    def place(P, yaw, mirror):
        Q = P - np.array([(P[:, 0].min() + P[:, 0].max()) * 0.5, (P[:, 1].min() + P[:, 1].max()) * 0.5,
                          P[:, 2].min()])
        if mirror:
            Q = Q * np.array([-1.0, 1.0, 1.0])
        c, s = np.cos(yaw), np.sin(yaw)
        Q = Q @ np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])
        q0, q1 = Q.min(0), Q.max(0)
        sc = (hi_c - lo_c) / np.maximum(q1 - q0, 1e-9)
        return (Q - q0) * sc + lo_c

    rng = np.random.default_rng(5)
    probe = P0[rng.choice(len(P0), min(len(P0), 1500), replace=False)]
    best = None
    for mirror in (False, True):
        for yaw in np.radians(np.arange(0, 360, 10)):
            Q = place(probe, yaw, mirror)
            O = axis_at(Q)
            D = Q - O
            r = np.maximum(np.linalg.norm(D, axis=1), 1e-9)
            t, f, _, _ = _ray_hits(O, D / r[:, None], A, B, C)
            m = np.isfinite(t)
            err = float(np.median(np.abs(r[m] - t[m]))) if m.sum() > 100 else np.inf
            if best is None or err < best[0]:
                best = (err, yaw, mirror)
    report["yaw_deg"] = float(np.degrees(best[1]))
    report["mirror"] = bool(best[2])
    report["outline_error_wu"] = best[0]
    vertices, indices, pieces, levels = [], [], [], []
    vat = iat = 0
    for li in range(nlev):
        P = place(lv[f"P{li}"], best[1], best[2])
        T = lv[f"T{li}"]
        if best[2]:
            T = T[:, ::-1]
        O = axis_at(P)
        D = P - O
        r = np.linalg.norm(D, axis=1)
        Du = D / np.maximum(r, 1e-12)[:, None]
        t, f, u, v = _ray_hits(O, Du, A, B, C)
        m = np.isfinite(t)
        res = np.where(m, r - t, 0.0)
        res = res - np.median(res[m])
        rn = np.where(m, t + soft_clamp(res, a.cap_in * size, a.cap_out * size), r)
        X = O + Du * rn[:, None]
        # (a ray past CE's open underside: the scan's own point, its base flattened just under CE's lowest point
        # (the ground), within CE's box sideways)
        base_z = lo_c[2] - 0.02 * size
        X[:, 2] = np.maximum(X[:, 2], base_z)
        X[:, :2] = np.clip(X[:, :2], lo_c[:2] - a.cap_out * size, hi_c[:2] + a.cap_out * size)
        fv = np.where(f >= 0, f, 0)
        uvv = (1 - u - v)[:, None] * UVc[Tc[fv, 0]] + u[:, None] * UVc[Tc[fv, 1]] + v[:, None] * UVc[Tc[fv, 2]]
        # the faces: outward wound as CE's (clockwise about the outward normal)
        fn = -np.cross(X[T[:, 1]] - X[T[:, 0]], X[T[:, 2]] - X[T[:, 0]])
        out = ((X[T].mean(1) - cen) * fn).sum(1)
        if (out < 0).mean() > 0.5:
            T = T[:, ::-1]
        # corners: a triangle whose corners' uvs come from far apart (across a CE seam) takes all three from the
        # first corner's CE triangle, extended over its plane
        cu = uvv[T]
        spread = np.max(np.linalg.norm(cu - cu.mean(1, keepdims=True), axis=2), axis=1)
        seam = spread > 0.15
        PX = X[T].reshape(-1, 3)
        UVX = cu.reshape(-1, 2).copy()
        sidx = np.nonzero(seam)[0]
        if len(sidx):
            f0 = fv[T[sidx, 0]]
            for k in range(3):
                p = X[T[sidx, k]]
                a0, b0, c0 = A[f0], B[f0], C[f0]
                n0 = rr.norm(np.cross(b0 - a0, c0 - a0))
                pp = p - ((p - a0) * n0).sum(1, keepdims=True) * n0
                bc = _bary(pp, a0, b0, c0)
                UVX[sidx * 3 + k] = (bc[:, :1] * UVc[Tc[f0, 0]] + bc[:, 1:2] * UVc[Tc[f0, 1]] +
                                     bc[:, 2:3] * UVc[Tc[f0, 2]])
        flatT = np.arange(len(T) * 3).reshape(-1, 3)
        # weld corners that share a position and a uv
        kk = np.round(np.concatenate([PX / 1e-6, UVX / 1e-6], 1)).astype(np.int64)
        _, first, inv = np.unique(kk, axis=0, return_index=True, return_inverse=True)
        PX, UVX, TT = PX[first], UVX[first], inv.reshape(-1)[flatT]
        CN = rr.corner_normals(PX, TT, 60.0)
        flat = TT.reshape(-1)
        k2 = np.round(np.concatenate([flat[:, None].astype(np.float64), CN * 1000.0], 1)).astype(np.int64)
        _, first2, inv2 = np.unique(k2, axis=0, return_index=True, return_inverse=True)
        verts = flat[first2]
        at = type("A", (), {"__len__": lambda s: len(s.position)})()
        at.position = PX[verts]
        at.normal = CN[first2]
        at.uv = UVX[verts]
        at.joints = np.zeros((len(verts), 4), np.int64)
        at.weights = np.zeros((len(verts), 4))
        at.weights[:, 0] = 1.0
        tri = inv2.reshape(-1, 3)
        at.tangent, at.sign = tangents(at.position, at.normal, at.uv, tri)
        vv = pfd.to_vertices(at)
        lv0, li0, pf = vat, iat, len(pieces)
        vertices.append(vv)
        indices.append(tri.astype(np.uint32).reshape(-1))
        pieces.append(pmesh.Piece(key[0], key[1], key[2], 0, 0, vat, len(vv), iat, tri.size))
        vat += len(vv)
        iat += tri.size
        c = (at.position.max(0) + at.position.min(0)) * 0.5
        levels.append(pmesh.Level(cut[li] if li + 1 < nlev else 0.0, pf, 1, lv0, vat - lv0, li0, iat - li0,
                                  tuple(float(x) for x in c), float(np.linalg.norm(at.position - c, axis=1).max())))
        offs = ((X - O) * Du).sum(1) - t
        report["levels"].append({"triangles": int(len(tri)), "vertices": int(len(vv)), "seam_faces": int(seam.sum()),
                                 "missed_rays": int((~m).sum()),
                                 "off_ce_wu_p5_p50_p95": [float(np.percentile(offs[m], q)) for q in (5, 50, 95)]})
    allv = np.concatenate(vertices)
    bones = [pmesh.Bone(nd["name"], nd["parent"], i, b, inv) for i, (nd, (b, inv)) in
             enumerate(zip(dump["nodes"], pfd.node_matrices(dump)))]
    radius = float(np.linalg.norm(allv["position"].astype(np.float64), axis=1).max()) * 1.0001
    mesh = pmesh.Mesh(len(dump["nodes"]), dump["node_crc"], bones, levels, pieces, allv, np.concatenate(indices),
                      (0.0, 0.0, 0.0), radius, {"source": "art/prism_cast/rock_scan_boulder.py", "tag": dump["tag"],
                                                "scan": a.scan_name, "levels": report["levels"]})
    problems = pmesh.check(mesh, dump)
    for p in problems:
        print("problem:", p)
    if problems:
        return 1
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    report["bytes"] = pmesh.write_pmesh(Path(a.out), mesh)
    if a.report:
        Path(a.report).write_text(json.dumps(report, indent=1))
    print(json.dumps(report))
    return 0


def _bary(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11 = (v0 * v0).sum(1), (v0 * v1).sum(1), (v1 * v1).sum(1)
    d20, d21 = (v2 * v0).sum(1), (v2 * v1).sum(1)
    den = np.where(np.abs(d00 * d11 - d01 * d01) < 1e-20, 1e-20, d00 * d11 - d01 * d01)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return np.stack([1 - v - w, v, w], 1)


def main():
    if "--" in sys.argv:  # (in Blender)
        args = sys.argv[sys.argv.index("--") + 1:]
        if args[0] == "decimate":
            cmd_decimate(args[1:])
            return 0
        raise SystemExit("in Blender: decimate only")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("gltf")
    p.add_argument("out")
    f = sub.add_parser("fit")
    f.add_argument("levels")
    f.add_argument("dump")
    f.add_argument("out")
    f.add_argument("--cutoffs", default="140,45,0")
    f.add_argument("--cap-in", type=float, default=0.15)
    f.add_argument("--cap-out", type=float, default=0.035)
    f.add_argument("--scan-name", default="")
    f.add_argument("--report", default="")
    a = ap.parse_args()
    if a.cmd == "prepare":
        cmd_prepare(a)
        return 0
    return cmd_fit(a)


if __name__ == "__main__":
    sys.exit(main())
