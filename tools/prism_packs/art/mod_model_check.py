#!/usr/bin/env python3
"""Check a mod model tag file the way port/linux/game/mod_models.c does.

Walks a .gbxmodel (or .model) tag file with the offsets and sizes the game
uses, then compares what the machines agree on (nodes, markers, regions,
permutations, shader names) against a reference tag of the map's model,
with the game's 0.001 tolerance. Also runs the per-part checks (shader and
link indices, strip indices within the part, vertex nodes, compressible
normals). Prints a JSON verdict.

Mesh hygiene (not a check of the game's; a fault the game draws): per geometry, triangles whose three
corners coincide with another triangle's (to 0.01 mm: z-fighting twins when wound alike, a zero-thickness
fin when opposite), corners that coincide within one triangle (zero area), and crumpled triangles (normal
more than 120 degrees from the area-weighted mean of its neighbours across position-welded edges: a
sawtooth tooth or a fold flap). The mod passes when it has no more twins, fins or zero-area triangles than
the map's own model, and no more than CRUMPLE_TOL more crumpled area (the remodels' terrace walls stand
at about 90 degrees and are never counted).

usage: mod_model_check.py <mod tag file> <reference tag file> [--out verdict.json] [--no-hygiene]
"""
import json
import math
import struct
import sys

HDR = 0x40
MODEL_SIZE = 0xE8
MARKER_SIZE, MARKER_INSTANCE_SIZE = 0x40, 0x20
NODE_SIZE, REGION_SIZE, PERMUTATION_SIZE, PERMUTATION_MARKER_SIZE = 0x9C, 0x4C, 0x58, 0x50
GEOMETRY_SIZE, PART_SIZE, GBX_PART_SIZE = 0x30, 0x68, 0x84
VERTEX_SIZE, COMPRESSED_VERTEX_SIZE, STRIP_ELEMENT_SIZE, SHADER_REF_SIZE = 0x44, 0x20, 6, 0x20
LIMITS = dict(markers=256, instances=32, nodes=64, regions=32, permutations=32, perm_markers=256,
              geometries=256, parts=32, vertices=65535, triangles=65535, shaders=32)
TOL = 0.001
# crumpled area a mod may have beyond the map's (share of its area): knife-edge rims on purpose (the
# first-person sleeves, 0.26%) pass; a sawtooth such as the round-2 Chief's first build (1.76%) does not
CRUMPLE_TOL = 0.005


def be32(b, o): return struct.unpack_from(">I", b, o)[0]
def s16(b, o): return struct.unpack_from(">h", b, o)[0]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def f32(b, o): return struct.unpack_from(">f", b, o)[0]
def name(b, o): return b[o:o + 32].split(b"\0")[0].decode("latin-1")


class TagModel:
    def __init__(self, path):
        b = open(path, "rb").read()
        self.path, self.b = path, b
        self.group = b[0x24:0x28].decode("latin-1")
        self.version = u16(b, 0x38)
        self.gbx = self.group == "mod2"
        psize = GBX_PART_SIZE if self.gbx else PART_SIZE
        r = HDR
        self.flags = be32(b, r)
        self.checksum = struct.unpack_from(">i", b, r + 4)[0]
        self.cutoffs = [f32(b, r + 0x08 + 4 * i) for i in range(5)]
        self.base_scale = (f32(b, r + 0x30), f32(b, r + 0x34))
        cnt = {k: be32(b, r + o) for k, o in
               (("markers", 0xAC), ("nodes", 0xB8), ("regions", 0xC4), ("geometries", 0xD0), ("shaders", 0xDC))}
        cur = HDR + MODEL_SIZE

        def take(n, lim, size):
            nonlocal cur
            if n > lim or cur + n * size > len(b):
                raise ValueError("block count past the tool's limit or not in file")
            o = cur
            cur += n * size
            return o

        self.markers = []
        mo = take(cnt["markers"], LIMITS["markers"], MARKER_SIZE)
        for i in range(cnt["markers"]):
            e = mo + i * MARKER_SIZE
            ni = be32(b, e + 0x34)
            io = take(ni, LIMITS["instances"], MARKER_INSTANCE_SIZE)
            inst = []
            for j in range(ni):
                x = io + j * MARKER_INSTANCE_SIZE
                inst.append((b[x], b[x + 1], b[x + 2], [f32(b, x + 4 + 4 * k) for k in range(3)],
                             [f32(b, x + 0x10 + 4 * k) for k in range(4)]))
            self.markers.append((name(b, e), inst))
        no = take(cnt["nodes"], LIMITS["nodes"], NODE_SIZE)
        self.nodes = []
        for i in range(cnt["nodes"]):
            e = no + i * NODE_SIZE
            self.nodes.append(dict(name=name(b, e), sib=s16(b, e + 0x20), child=s16(b, e + 0x22), parent=s16(b, e + 0x24),
                                   t=[f32(b, e + 0x28 + 4 * k) for k in range(3)],
                                   q=[f32(b, e + 0x34 + 4 * k) for k in range(4)], dist=f32(b, e + 0x44)))
        ro = take(cnt["regions"], LIMITS["regions"], REGION_SIZE)
        self.regions = []
        for i in range(cnt["regions"]):
            e = ro + i * REGION_SIZE
            npm = be32(b, e + 0x40)
            po = take(npm, LIMITS["permutations"], PERMUTATION_SIZE)
            perms = []
            for j in range(npm):
                p = po + j * PERMUTATION_SIZE
                nmk = be32(b, p + 0x4C)
                mko = take(nmk, LIMITS["perm_markers"], PERMUTATION_MARKER_SIZE)
                mk = []
                for k in range(nmk):
                    m = mko + k * PERMUTATION_MARKER_SIZE
                    mk.append((name(b, m), s16(b, m + 0x20), [f32(b, m + 0x24 + 4 * q) for q in range(4)],
                               [f32(b, m + 0x34 + 4 * q) for q in range(3)]))
                perms.append(dict(name=name(b, p), flags=be32(b, p + 0x20) & 1,
                                  lods=[s16(b, p + 0x40 + 2 * d) for d in range(5)], markers=mk))
            self.regions.append(dict(name=name(b, e), perms=perms))
        go = take(cnt["geometries"], LIMITS["geometries"], GEOMETRY_SIZE)
        self.geometries = []
        for i in range(cnt["geometries"]):
            e = go + i * GEOMETRY_SIZE
            npt = be32(b, e + 0x24)
            pto = take(npt, LIMITS["parts"], psize)
            parts = []
            for j in range(npt):
                p = pto + j * psize
                nv, nc, nt = be32(b, p + 0x20), be32(b, p + 0x2C), be32(b, p + 0x38)
                vo = take(nv, LIMITS["vertices"], VERTEX_SIZE)
                take(nc, LIMITS["vertices"], COMPRESSED_VERTEX_SIZE)
                to = take(nt, LIMITS["triangles"], STRIP_ELEMENT_SIZE)
                parts.append(dict(off=p, shader=s16(b, p + 4), prev=b[p + 6], next=b[p + 7],
                                  centroid=[f32(b, p + 0x14 + 4 * k) for k in range(3)],
                                  nv=nv, nc=nc, nt=nt, vo=vo, to=to,
                                  local_count=b[p + 0x6B] if self.gbx else 0))
            self.geometries.append(parts)
        so = take(cnt["shaders"], LIMITS["shaders"], SHADER_REF_SIZE)
        self.shaders = []
        for i in range(cnt["shaders"]):
            ln = be32(b, so + i * SHADER_REF_SIZE + 8)
            if not ln or ln > 255 or b[cur + ln] != 0:
                raise ValueError("a shader's name is not in the file")
            self.shaders.append(b[cur:cur + ln].decode("latin-1"))
            cur += ln + 1
        self.trailing = len(b) - cur


def close(a, b): return abs(a - b) <= TOL
def pts(a, b): return all(close(x, y) for x, y in zip(a, b))
def rots(a, b): return pts(a, b) or pts(a, [-y for y in b])


def part_checks(m):
    out = []
    local = m.gbx and (m.flags >> 1) & 1
    b = m.b
    for gi, parts in enumerate(m.geometries):
        n = len(parts)
        for pi, p in enumerate(parts):
            prev, nxt = (255, 255) if p["prev"] == 0 and p["next"] == 0 else (p["prev"], p["next"])
            errs = []
            if not 0 <= p["shader"] < len(m.shaders):
                errs.append("shader index")
            if (prev != 255 and prev >= n) or (nxt != 255 and nxt >= n):
                errs.append("link index")
            if p["nv"] < 1:
                errs.append("no uncompressed vertices")
            idx = [u16(b, p["to"] + 2 * k) for k in range(p["nt"] * 3)]
            while idx and idx[-1] == 0xFFFF:
                idx.pop()
            if len(idx) < 3 or max(idx) >= p["nv"]:
                errs.append("strip")
            bad_node = bad_vec = bad_w = 0
            for k in range(p["nv"]):
                v = p["vo"] + k * VERTEX_SIZE
                vals = struct.unpack_from(">14f", b, v)
                n0, n1 = s16(b, v + 0x38), s16(b, v + 0x3A)
                w0, w1 = f32(b, v + 0x3C), f32(b, v + 0x40)
                for nn in ((n0,) if n1 == -1 else (n0, n1)):
                    if local:
                        nn = b[p["off"] + 0x6C + nn] if 0 <= nn < p["local_count"] else -1
                    if not 0 <= nn < len(m.nodes):
                        bad_node += 1
                if any(not math.isfinite(x) for x in vals) or not all(math.isfinite(x) for x in (w0, w1)):
                    bad_w += 1
                if any(abs(x) > 1.005 for x in vals[3:12]):
                    bad_vec += 1
            if bad_node: errs.append(f"{bad_node} vertex node refs out of range")
            if bad_vec: errs.append(f"{bad_vec} vectors not compressible")
            if bad_w: errs.append(f"{bad_w} non-finite values")
            out.append(dict(geometry=gi, part=pi, shader=m.shaders[p["shader"]] if 0 <= p["shader"] < len(m.shaders) else None,
                            vertices=p["nv"], compressed_vertices=p["nc"], strip_indices=len(idx),
                            triangles_approx=max(0, len(idx) - 2), local_nodes=p["local_count"],
                            prev=prev, next=nxt, ok=not errs, errors=errs))
    return out


def facing(m):
    """Fraction of strip triangles whose winding normal points along their vertex normals: by count,
    and by area (a dense remodel has slivers on steep relief whose smooth vertex normals lean past
    90 degrees from them; an inside-out model disagrees by count and by area alike)."""
    agree = total = 0
    agree_a = total_a = 0.0
    b = m.b
    for parts in m.geometries:
        for p in parts:
            idx = [u16(b, p["to"] + 2 * k) for k in range(p["nt"] * 3)]
            while idx and idx[-1] == 0xFFFF:
                idx.pop()
            vs = {}
            # (a triangle written both ways on its own vertices counts once, as its first side:
            # elite_seal_jms.py's rims)
            sides = set()

            def vert(i):
                if i not in vs:
                    vs[i] = struct.unpack_from(">6f", b, p["vo"] + i * VERTEX_SIZE)
                return vs[i]
            for i in range(len(idx) - 2):
                a, c1, c2 = idx[i:i + 3]
                if a == c1 or c1 == c2 or a == c2:
                    continue
                if i % 2:
                    c1, c2 = c2, c1
                corners = frozenset((a, c1, c2))
                if corners in sides:
                    continue
                sides.add(corners)
                A, B, C = vert(a), vert(c1), vert(c2)
                e1 = [B[k] - A[k] for k in range(3)]
                e2 = [C[k] - A[k] for k in range(3)]
                fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                vn = [A[3 + k] + B[3 + k] + C[3 + k] for k in range(3)]
                d = sum(fn[k] * vn[k] for k in range(3))
                if d == 0:
                    continue
                total += 1
                agree += d > 0
                ar = math.sqrt(sum(x * x for x in fn))
                total_a += ar
                agree_a += ar if d > 0 else 0.0
    return (agree / total if total else 0.0), (agree_a / total_a if total_a else 0.0)


def hygiene(m, dist=0.01 / 3048.0, crumple_cos=-0.5):
    import numpy as np
    same = opp = zero = crum = total = two_sided = 0
    crum_area = area_sum = 0.0
    b = m.b
    for parts in m.geometries:
        P, T = [], []
        for p in parts:
            idx = [u16(b, p["to"] + 2 * k) for k in range(p["nt"] * 3)]
            while idx and idx[-1] == 0xFFFF:
                idx.pop()
            base = sum(len(x) for x in P)
            pos = [struct.unpack_from(">3f", b, p["vo"] + i * VERTEX_SIZE) for i in range(p["nv"])]
            P.append(pos)
            for i in range(len(idx) - 2):
                a, c1, c2 = idx[i:i + 3]
                if a == c1 or c1 == c2 or a == c2:
                    continue
                if i % 2:
                    c1, c2 = c2, c1
                T.append((base + a, base + c1, base + c2))
        if not T:
            continue
        co = np.array([x for pp in P for x in pp], np.float64)
        tri = np.array(T, np.int64)
        q = np.round(co / dist).astype(np.int64)
        _, wid = np.unique(q, axis=0, return_inverse=True)
        wid = wid.ravel()
        wt = wid[tri]
        A, B, C = co[tri[:, 0]], co[tri[:, 1]], co[tri[:, 2]]
        fn = np.cross(B - A, C - A)
        ar = np.linalg.norm(fn, axis=1)
        nu = fn / np.maximum(ar[:, None], 1e-30)
        degen = (wt[:, 0] == wt[:, 1]) | (wt[:, 1] == wt[:, 2]) | (wt[:, 0] == wt[:, 2])
        zero += int(degen.sum())
        ok = ~degen
        key = np.sort(wt, 1)
        _, kinv, kcnt = np.unique(key, axis=0, return_inverse=True, return_counts=True)
        kinv = kinv.ravel()
        groups = {}
        twins = []
        for f in np.nonzero(ok & (kcnt[kinv] > 1))[0]:
            groups.setdefault(int(kinv[f]), []).append(int(f))
        for fs in groups.values():
            for f in fs[1:]:
                if nu[f] @ nu[fs[0]] > 0:
                    same += 1
                elif set(tri[f].tolist()) == set(tri[fs[0]].tolist()):
                    # (one triangle written both ways on its own vertices: drawn from whichever side it is seen,
                    # elite_seal_jms.py's rims; not a fold collapsed flat)
                    two_sided += 1
                    twins.append(f)
                else:
                    opp += 1
        # (a two-sided triangle's second side takes no part in the crumple test: it is its first, turned)
        ok[twins] = False
        E = np.sort(np.stack([wt[:, [0, 1]], wt[:, [1, 2]], wt[:, [2, 0]]], 1).reshape(-1, 2), 1)
        _, einv = np.unique(E, axis=0, return_inverse=True)
        einv = einv.ravel()
        na = np.where(ok[:, None], nu * ar[:, None], 0.0)
        S = np.zeros((einv.max() + 1, 3))
        np.add.at(S, einv, np.repeat(na, 3, 0))
        acc = (S[einv] - np.repeat(na, 3, 0)).reshape(-1, 3, 3).sum(1)
        L = np.linalg.norm(acc, axis=1)
        c = ok & (L > 1e-30) & ((nu * acc).sum(1) < crumple_cos * np.maximum(L, 1e-30))
        crum += int(c.sum())
        crum_area += float(ar[c].sum() / 2)
        area_sum += float(ar[ok].sum() / 2)
        total += len(tri)
    return dict(triangles=total, coincident_same_winding=same, coincident_opposite=opp, two_sided=two_sided,
                zero_area=zero,
                crumpled=crum, crumpled_area_share=crum_area / max(area_sum, 1e-30))


def compare(mod, ref):
    res = {}
    # nodes
    ne = []
    if len(mod.nodes) != len(ref.nodes):
        ne.append(f"count {len(mod.nodes)} vs {len(ref.nodes)}")
    worst_t = worst_q = worst_d = 0.0
    for i, (a, r) in enumerate(zip(mod.nodes, ref.nodes)):
        if a["name"] != r["name"] or (a["sib"], a["child"], a["parent"]) != (r["sib"], r["child"], r["parent"]):
            ne.append(f"node {i} name/hierarchy {a['name']} vs {r['name']}")
        worst_t = max(worst_t, max(abs(x - y) for x, y in zip(a["t"], r["t"])))
        dq = min(max(abs(x - y) for x, y in zip(a["q"], r["q"])), max(abs(x + y) for x, y in zip(a["q"], r["q"])))
        worst_q = max(worst_q, dq)
        worst_d = max(worst_d, abs(a["dist"] - r["dist"]))
        if not pts(a["t"], r["t"]) or not rots(a["q"], r["q"]) or not close(a["dist"], r["dist"]):
            ne.append(f"node {i} {a['name']}: t/q/dist off (dt={max(abs(x - y) for x, y in zip(a['t'], r['t'])):.6f} "
                      f"dq={dq:.6f} dist {a['dist']:.6f} vs {r['dist']:.6f})")
    res["nodes"] = dict(ok=not ne, count=len(mod.nodes), max_translation_diff=worst_t, max_rotation_diff=worst_q,
                        max_distance_diff=worst_d, errors=ne)
    # regions
    re_ = []
    if len(mod.regions) != len(ref.regions):
        re_.append("region count")
    for a, r in zip(mod.regions, ref.regions):
        if a["name"] != r["name"] or len(a["perms"]) != len(r["perms"]):
            re_.append(f"region {a['name']} vs {r['name']}")
            continue
        for pa, pr in zip(a["perms"], r["perms"]):
            if pa["name"] != pr["name"] or pa["flags"] != pr["flags"]:
                re_.append(f"permutation {pa['name']} vs {pr['name']}")
            for d, (ga, gr) in enumerate(zip(pa["lods"], pr["lods"])):
                if (gr == -1 and ga != -1) or (gr != -1 and not 0 <= ga < len(mod.geometries)):
                    re_.append(f"permutation {pa['name']} lod {d}: {ga} (map {gr})")
    res["regions"] = dict(ok=not re_, names=[(r["name"], [(p["name"], p["flags"], p["lods"]) for p in r["perms"]])
                                             for r in mod.regions], errors=re_)

    # markers: on the model, or on the permutations (both sides flattened)
    def flat(m):
        out = []
        for nm, inst in m.markers:
            for (ri, pi, node, t, q) in inst:
                out.append((nm, ri, pi, node, t, q))
        for ri, r in enumerate(m.regions):
            for pi, p in enumerate(r["perms"]):
                for (nm, node, q, t) in p["markers"]:
                    out.append((nm, ri, pi, node, t, q))
        return sorted(out, key=lambda x: (x[0], x[1], x[2], x[3]))
    fa, fr = flat(mod), flat(ref)
    me = []
    if len(fa) != len(fr):
        me.append(f"marker instance count {len(fa)} vs {len(fr)}")
    for a, r in zip(fa, fr):
        if a[:4] != r[:4] or not pts(a[4], r[4]) or not rots(a[5], r[5]):
            me.append(f"marker {a[0]} vs {r[0]}")
    res["markers"] = dict(ok=not me, instances=len(fa), errors=me)
    missing = [s for s in mod.shaders if s.lower() not in {x.lower() for x in ref.shaders}]
    res["shaders"] = dict(ok=not missing, mod=mod.shaders, map=ref.shaders, not_in_map=missing)
    res["checksum"] = dict(mod=mod.checksum, map=ref.checksum, equal=mod.checksum == ref.checksum)
    # winding: the game culls back faces, so the mod's triangles must face the way the map's do
    # (not a check of the game's; an inside-out model passes mod_models.c and draws with holes)
    (fm, fma), (fr, fra) = facing(mod), facing(ref)
    same = fm if fr >= 0.5 else 1.0 - fm
    same_a = fma if fra >= 0.5 else 1.0 - fma
    res["winding"] = dict(ok=same_a >= 0.98, mod_normal_agree=fm, map_normal_agree=fr, faces_as_map=same,
                          faces_as_map_by_area=same_a)
    return res


def main():
    args = [a for a in sys.argv[1:] if a != "--no-hygiene"]
    out = None
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    mod, ref = TagModel(args[0]), TagModel(args[1])
    v = dict(mod=args[0], reference=args[1],
             header=dict(group=mod.group, version=mod.version, ok=(mod.group == "mod2" and mod.version == 5) or
                         (mod.group == "mode" and mod.version == 4)),
             walk=dict(ok=mod.trailing == 0, trailing_bytes=mod.trailing),
             flags=dict(mod=mod.flags, map=ref.flags, blend_shared_normals=mod.flags & 1,
                        parts_have_local_nodes=(mod.flags >> 1) & 1),
             cutoffs=dict(mod=mod.cutoffs, map=ref.cutoffs), base_map_scale=dict(mod=mod.base_scale, map=ref.base_scale),
             geometries=len(mod.geometries))
    v.update(compare(mod, ref))
    v["parts"] = part_checks(mod)
    v["parts_ok"] = all(p["ok"] for p in v["parts"])
    if "--no-hygiene" not in sys.argv:
        hm, hr = hygiene(mod), hygiene(ref)
        bad = [k for k in ("coincident_same_winding", "coincident_opposite", "zero_area")
               if hm[k] > hr[k]]
        if hm["crumpled_area_share"] > hr["crumpled_area_share"] + CRUMPLE_TOL:
            bad.append("crumpled_area_share")
        v["hygiene"] = dict(ok=not bad, mod=hm, map=hr, over_map=bad)
    v["verdict"] = all(v[k]["ok"] for k in ("header", "walk", "nodes", "regions", "markers", "shaders", "winding")) and v["parts_ok"] \
        and v.get("hygiene", {"ok": True})["ok"]
    s = json.dumps(v, indent=1)
    if out:
        open(out, "w").write(s)
    print(s)
    sys.exit(0 if v["verdict"] else 1)


if __name__ == "__main__":
    main()
