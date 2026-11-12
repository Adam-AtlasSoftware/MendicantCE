"""PRISM v3 cast: a remodel's JMS files (elite_build_r2.py's build/jms, one per permutation and level of detail) as a
mesh-store .pmesh (display.prism_models), checked as the host checks it against the map's model dump.

    python3 jms_to_pmesh.py <model dump.json> <jms dir> <stock jms dir> <out.pmesh> [--levels superhigh,high,...]
        [--check-stock]

- the model's nodes (the dump's) are the skeleton; a JMS vertex's two nodes and weight become its first two bones;
- each (region, permutation, shader) of a level is one piece; a JMS material is the model's shader of the same index,
  except that a name the model has twice (the Grunt's rank shaders) takes the index CE's own JMS of that region and
  permutation draws it with;
- positions in world units (JMS's are 100 times), uv0 as the dump's (v flipped: JMS's v is up the picture),
  tangents from the uvs (per triangle, summed at the vertices), triangles wound as the dump's parts are;
- each level is drawn from the model's own cutoff for that detail (the last at any size).
--check-stock converts CE's own JMS (stock jms dir as jms dir) and compares it with the dump's geometry: the
conversion's test. Everything here is the map's data: local only.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import halo_formats as hf  # noqa: E402

sys.path.insert(0, str(HERE.parent))
import packs_env as E  # noqa: E402

TOOLS = E.TOOLS
sys.path.insert(0, TOOLS)
import pmesh  # noqa: E402
import pmesh_from_dump as pfd  # noqa: E402

UV_V = eval(os.environ.get("CAST_UV_V", "lambda v: 1.0 - v"))

DETAILS = ("superlow", "low", "medium", "high", "superhigh")


def jms_files(folder):
    out = {}
    for f in os.listdir(folder):
        if not f.endswith(".jms"):
            continue
        perm, lod = f[:-4].rsplit(" ", 1)
        out.setdefault(perm, {})[lod] = os.path.join(folder, f)
    return out


def stock_shader_index(stock_dir, perm, lod, cache={}):
    """(region name, material name) -> the shader index CE's own JMS draws it with"""
    key = (stock_dir, perm, lod)
    if key not in cache:
        j = hf.read_jms(os.path.join(stock_dir, "%s %s.jms" % (perm, lod)))
        m = {}
        for t in j.triangles:
            m.setdefault((j.regions[t.region], j.materials[t.shader][0].lower()), t.shader)
        cache[key] = m
    return cache[key]


def tangents(pos, nrm, uv, tri):
    """per-vertex tangent and handedness from the uvs (summed over the triangles)"""
    p0, p1, p2 = pos[tri[:, 0]], pos[tri[:, 1]], pos[tri[:, 2]]
    t0, t1, t2 = uv[tri[:, 0]], uv[tri[:, 1]], uv[tri[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = t1 - t0, t2 - t0
    r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(r) < 1e-12, 1e-12, r)
    sdir = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / r[:, None]
    tdir = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) / r[:, None]
    T = np.zeros_like(pos)
    B = np.zeros_like(pos)
    for k in range(3):
        np.add.at(T, tri[:, k], sdir)
        np.add.at(B, tri[:, k], tdir)
    T = T - nrm * np.einsum("ij,ij->i", T, nrm)[:, None]
    sign = np.where(np.einsum("ij,ij->i", np.cross(nrm, T), B) < 0, -1, 1)
    return T, sign


class Attr:
    def __len__(self):
        return len(self.position)


def piece_attributes(j, tri_list, dump_scale):
    ids = np.array(sorted({i for t in tri_list for i in t.v}), np.int64)
    remap = np.full(len(j.vertices), -1, np.int64)
    remap[ids] = np.arange(len(ids))
    vs = [j.vertices[i] for i in ids]
    a = Attr()
    a.position = np.array([v.pos for v in vs], np.float64) / 100.0
    a.normal = np.array([v.normal for v in vs], np.float64)
    a.normal /= np.maximum(np.linalg.norm(a.normal, axis=1, keepdims=True), 1e-12)
    a.uv = np.array([(v.u, UV_V(v.v)) for v in vs], np.float64)
    n = len(vs)
    a.joints = np.zeros((n, 4), np.int64)
    a.weights = np.zeros((n, 4))
    for k, v in enumerate(vs):
        a.joints[k, 0] = v.node0
        if v.node1 >= 0 and v.weight > 0:
            a.joints[k, 1] = v.node1
            a.weights[k, 0] = 1.0 - v.weight
            a.weights[k, 1] = v.weight
        else:
            a.joints[k, 1] = v.node0
            a.weights[k, 0] = 1.0
    # (wound as the dump's parts are, CE's front faces clockwise: JMS's own order)
    tri = remap[np.array([t.v for t in tri_list], np.int64)]
    # (elite_build_r2.py writes its JMS in Blender's winding, the other way from map_extract's: each piece is
    # wound so that its faces turn from its normals, as CE's front faces in the dump do)
    p_ = a.position
    fn = np.cross(p_[tri[:, 1]] - p_[tri[:, 0]], p_[tri[:, 2]] - p_[tri[:, 0]])
    agree = float((np.einsum("ij,ij->i", fn, a.normal[tri].sum(1)) > 0).mean())
    a.flipped = agree > 0.5
    if a.flipped:
        tri = tri[:, [0, 2, 1]]
    # (a triangle the cuts turned alone, wound against all three of its corners' normals, would be culled from
    # where it is seen and show a line through the body: it is written both ways)
    fn = -np.cross(p_[tri[:, 1]] - p_[tri[:, 0]], p_[tri[:, 2]] - p_[tri[:, 0]])
    against = (np.einsum("ij,ikj->ik", fn, a.normal[tri]) < 0).all(1)
    a.doubled = int(against.sum())
    if a.doubled:
        tri = np.concatenate([tri, tri[against][:, [0, 2, 1]]])
    a.tangent, a.sign = tangents(a.position, a.normal, a.uv, tri)
    a.uv = np.clip(a.uv, 0.0, None)
    return a, tri


def build(dump, jms_dir, stock_dir, levels, report, cutoffs=None):
    files = jms_files(jms_dir)
    region_of = {r["name"]: i for i, r in enumerate(dump["regions"])}
    cut = dump["detail_cutoff_pixels"]
    vertices, indices, pieces, mesh_levels = [], [], [], []
    vat = iat = 0
    scale = dump["base_map_scale"]
    for li, lod in enumerate(levels):
        lv0, li0, pf = vat, iat, len(pieces)
        pts = []
        for perm, by_lod in sorted(files.items()):
            if lod not in by_lod:
                continue
            j = hf.read_jms(by_lod[lod])
            stock = stock_shader_index(stock_dir, perm, lod)
            groups = {}
            for t in j.triangles:
                rname = j.regions[t.region]
                mname = j.materials[t.shader][0].lower()
                sh = stock.get((rname, mname), t.shader)
                ri = region_of[rname]
                perms = [p["name"] for p in dump["regions"][ri]["permutations"]]
                if perm not in perms:
                    raise SystemExit("%s: permutation %r is not region %s's" % (by_lod[lod], perm, rname))
                groups.setdefault((ri, perms.index(perm), sh), []).append(t)
            for (ri, pi, sh), tl in sorted(groups.items()):
                a, tri = piece_attributes(j, tl, scale)
                clipped = int(((a.uv[:, 0] > scale[0]) | (a.uv[:, 1] > scale[1])).sum())
                a.uv = np.minimum(a.uv, np.array(scale) - 1e-6)
                v = pfd.to_vertices(a)
                vertices.append(v)
                indices.append(tri.astype(np.uint32).reshape(-1))
                pieces.append(pmesh.Piece(ri, pi, sh, 0, 0, vat, len(v), iat, tri.size))
                vat += len(v)
                iat += tri.size
                pts.append(a.position)
                report.setdefault(lod, {"triangles": 0, "vertices": 0, "uv_clamped": 0})
                report[lod]["triangles"] += len(tri)
                report[lod]["vertices"] += len(v)
                report[lod]["uv_clamped"] += clipped
                report[lod]["pieces_flipped"] = report[lod].get("pieces_flipped", 0) + int(a.flipped)
                report[lod]["triangles_doubled"] = report[lod].get("triangles_doubled", 0) + a.doubled
        cutoff = (cutoffs[li] if cutoffs else cut[DETAILS.index(lod)]) if li + 1 < len(levels) else 0.0
        P = np.concatenate(pts)
        c = (P.max(0) + P.min(0)) * 0.5
        mesh_levels.append(pmesh.Level(float(cutoff), pf, len(pieces) - pf, lv0, vat - lv0, li0, iat - li0,
                                       tuple(float(x) for x in c), float(np.linalg.norm(P - c, axis=1).max())))
    allv = np.concatenate(vertices)
    bones = [pmesh.Bone(n["name"], n["parent"], i, b, inv) for i, (n, (b, inv)) in
             enumerate(zip(dump["nodes"], pfd.node_matrices(dump)))]
    radius = float(np.linalg.norm(allv["position"].astype(np.float64), axis=1).max()) * 1.0001
    return pmesh.Mesh(len(dump["nodes"]), dump["node_crc"], bones, mesh_levels, pieces, allv,
                      np.concatenate(indices), (0.0, 0.0, 0.0), radius,
                      {"source": "art/prism_cast/jms_to_pmesh.py", "tag": dump["tag"], "levels": report})


def check_stock(dump, blob, mesh):
    """level 0 (CE's superhigh JMS) against the dump's superhigh parts: positions and uvs of each piece's vertices
    found among the part's (nearest), and the triangles' facing against the dump's"""
    worst_p = worst_uv = 0.0
    facing = []
    for pc in mesh.pieces[:mesh.levels[0].piece_count]:
        reg = dump["regions"][pc.region]["permutations"][pc.permutation]
        geo = dump["geometries"][reg["geometries"][4]]
        raws, tris = [], []
        base = 0
        for part in geo["parts"]:
            if part["shader"] != pc.shader:
                continue
            raw, t = pfd.part_arrays(dump, blob, part)
            raws.append(raw)
            tris.append(t + base)
            base += len(raw)
        if not raws:
            print("piece", pc, "has no stock part")
            continue
        raw = np.concatenate(raws)
        v = mesh.vertices[pc.vertex_first:pc.vertex_first + pc.vertex_count]
        p = v["position"].astype(np.float64)
        d = np.linalg.norm(p[:, None, :] - raw[None, :, 0:3], axis=2)
        k = d.argmin(1)
        worst_p = max(worst_p, float(d.min(1).max()))
        near = d <= d.min(1, keepdims=True) + 1e-5
        duv = np.abs(v["uv0"].astype(np.float64)[:, None, :] - raw[None, :, 12:14]).max(2)
        worst_uv = max(worst_uv, float(np.where(near, duv, 9.0).min(1).max()))
        idx = mesh.indices[pc.index_first:pc.index_first + pc.index_count].reshape(-1, 3).astype(np.int64)
        fn = np.cross(p[idx[:, 1]] - p[idx[:, 0]], p[idx[:, 2]] - p[idx[:, 0]])
        rn = raw[k][:, 3:6]
        facing.append(float((np.einsum("ij,ij->i", fn, rn[idx].sum(1)) > 0).mean()))
        t = np.concatenate(tris)
        rp = raw[:, 0:3]
        fr = np.cross(rp[t[:, 1]] - rp[t[:, 0]], rp[t[:, 2]] - rp[t[:, 0]])
        facing.append(-float((np.einsum("ij,ij->i", fr, raw[t][:, :, 3:6].sum(1)) > 0).mean()))
    print("stock check: position max %.6f wu, uv max %.6f; facing (ours +, dump's -):" % (worst_p, worst_uv),
          " ".join("%.2f" % f for f in facing))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("jms_dir")
    ap.add_argument("stock_dir")
    ap.add_argument("out")
    ap.add_argument("--levels", default="superhigh,high,medium,low,superlow")
    ap.add_argument("--check-stock", action="store_true")
    ap.add_argument("--cutoffs", default="", help="each level's size in pixels (a model whose own are all 0)")
    a = ap.parse_args()
    dump = json.loads(Path(a.dump).read_text())
    report = {}
    mesh = build(dump, a.jms_dir, a.stock_dir, a.levels.split(","), report,
                 [float(x) for x in a.cutoffs.split(",")] if a.cutoffs else None)
    if a.check_stock:
        check_stock(dump, Path(a.dump).with_suffix(".bin").read_bytes(), mesh)
    problems = pmesh.check(mesh, dump)
    for p in problems:
        print("problem:", p)
    if problems:
        return 1
    size = pmesh.write_pmesh(Path(a.out), mesh)
    print(json.dumps(report))
    print("%s: %d bytes" % (a.out, size))
    print(pmesh.summary(pmesh.read_pmesh(Path(a.out))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
