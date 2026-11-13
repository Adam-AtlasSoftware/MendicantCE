#!/usr/bin/env python3
"""World units (and metres) per base-map UV unit, per model shader, from map_extract's glTFs.

    python3 -I art/measure_uv_density.py --extract <work>/extract/b30 \
        [--shader "weapons\\pistol\\fp\\shaders\\silver" ...] [--json out.json]

For each shader: its triangles (the highest LOD of each model that uses it, so LODs are not counted
twice), split into UV islands (triangles joined by shared vertices: map_extract splits vertices at
UV seams), each island's density sqrt(world area / UV area), and the median over islands weighted
by world area (plus p10/p25/p75/p90, to show how even the density is). The base map's coordinates
are the model's (times its base_map_scale, if map_extract recorded one other than 1).

Units: map_extract's glTFs are in world units ("1 = 10 feet"). That is checked against the Chief:
the bind-pose height of characters\\cyborg\\cyborg's highest LOD, in world units and in metres at
3.048 m a unit, which should be about 2.1 m (CM: Spartan in armour, about 7 ft). The check is
printed and written to the JSON ("unit_check").

Output: one JSON line per shader on stdout, or all of them in --json. overlay_pack.py reads uv_wu.
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

WU_M = 3.048
LOD_ORDER = ["superhigh", "high", "medium", "low", "superlow"]


def accessor(doc, buf, index):
    a = doc["accessors"][index]
    bv = doc["bufferViews"][a["bufferView"]]
    comp = {5126: ("f", 4), 5123: ("H", 2), 5125: ("I", 4), 5121: ("B", 1)}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", comp[1] * n)
    raw = np.frombuffer(buf, np.uint8, count=stride * (a["count"] - 1) + comp[1] * n, offset=off)
    rows = np.lib.stride_tricks.as_strided(raw, (a["count"], comp[1] * n), (stride, 1)).copy()
    return np.frombuffer(rows.tobytes(), "<" + comp[0]).reshape(a["count"], n).astype(np.float64)


def model_meshes(path):
    """(doc, buffer, [(mesh index, lod)]) keeping, per region/permutation, the highest LOD only"""
    doc = json.load(open(path))
    buf = open(os.path.join(os.path.dirname(path), doc["buffers"][0]["uri"]), "rb").read()
    best = {}
    for node in doc["nodes"]:
        if "mesh" not in node:
            continue
        ex = node.get("extras", {})
        key = (ex.get("region"), ex.get("permutation"))
        lod = ex.get("lod", "superhigh")
        rank = LOD_ORDER.index(lod) if lod in LOD_ORDER else 0
        if key not in best or rank < best[key][1]:
            best[key] = (node["mesh"], rank)
    return doc, buf, [m for m, _ in best.values()]


def islands(tris, nverts):
    parent = np.arange(nverts)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b, c in tris:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[find(rc)] = ra
    return np.array([find(t[0]) for t in tris])


def weighted_quantiles(values, weights, qs):
    order = np.argsort(values)
    cum = np.cumsum(weights[order]) / weights.sum()
    return [float(values[order][min(len(order) - 1, np.searchsorted(cum, q))]) for q in qs]


def collect(extract, wanted):
    """shader -> list of per-primitive (world areas, uv areas, island ids, model path)"""
    found = {}
    for path in sorted(glob.glob(os.path.join(extract, "models", "**", "*.gltf"), recursive=True)):
        doc = json.load(open(path))
        names = [m.get("name") for m in doc.get("materials", [])]
        if wanted and not any(n in wanted for n in names):
            continue
        doc, buf, meshes = model_meshes(path)
        scale = np.array(doc.get("extras", {}).get("base_map_scale", [1, 1]), np.float64)
        scale[scale == 0] = 1
        for mi in meshes:
            for p in doc["meshes"][mi]["primitives"]:
                if "material" not in p or "TEXCOORD_0" not in p["attributes"]:
                    continue
                name = names[p["material"]]
                if not name or name == "none" or (wanted and name not in wanted):
                    continue
                pos = accessor(doc, buf, p["attributes"]["POSITION"])
                uv = accessor(doc, buf, p["attributes"]["TEXCOORD_0"]) * scale
                tri = accessor(doc, buf, p["indices"]).astype(np.int64).reshape(-1, 3)
                e1, e2 = pos[tri[:, 1]] - pos[tri[:, 0]], pos[tri[:, 2]] - pos[tri[:, 0]]
                aw = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
                u1, u2 = uv[tri[:, 1]] - uv[tri[:, 0]], uv[tri[:, 2]] - uv[tri[:, 0]]
                au = 0.5 * np.abs(u1[:, 0] * u2[:, 1] - u1[:, 1] * u2[:, 0])
                found.setdefault(name, []).append((aw, au, islands(tri, len(pos)), os.path.relpath(path, extract)))
    return found


def density(parts):
    dens, wts, tri_d, tri_w = [], [], [], []
    for aw, au, isl, _ in parts:
        ok = (au > 1e-12) & (aw > 0)
        tri_d.append(np.sqrt(aw[ok] / au[ok]))
        tri_w.append(aw[ok])
        for i in np.unique(isl[ok]):
            sel = ok & (isl == i)
            if au[sel].sum() > 1e-10:
                dens.append(np.sqrt(aw[sel].sum() / au[sel].sum()))
                wts.append(aw[sel].sum())
    dens, wts = np.array(dens), np.array(wts)
    tri_d, tri_w = np.concatenate(tri_d), np.concatenate(tri_w)
    q = weighted_quantiles(dens, wts, [0.1, 0.25, 0.5, 0.75, 0.9])
    return {
        "uv_wu": round(q[2], 5), "uv_m": round(q[2] * WU_M, 4),
        "islands": int(len(dens)), "island_p10_p25_p75_p90_wu": [round(v, 5) for v in (q[0], q[1], q[3], q[4])],
        "triangle_median_wu": round(weighted_quantiles(tri_d, tri_w, [0.5])[0], 5),
        "world_area_m2": round(float(tri_w.sum()) * WU_M * WU_M, 5),
        "models": sorted({p[3] for p in parts}),
    }


def by_model(parts):
    """the same per model (a shader shared by a first-person and a third-person model)"""
    out = {}
    for model in sorted({p[3] for p in parts}):
        sel = [p for p in parts if p[3] == model]
        if not any(((au > 1e-12) & (aw > 0)).any() for aw, au, _, _ in sel):
            continue                                    # (no triangle with UV area: nothing to measure)
        d = density(sel)
        out[model] = {k: d[k] for k in ("uv_wu", "islands", "island_p10_p25_p75_p90_wu", "world_area_m2")}
    return out


def chief_height(extract):
    path = os.path.join(extract, "models", "characters", "cyborg", "cyborg.gltf")
    if not os.path.exists(path):
        return None
    doc, buf, meshes = model_meshes(path)
    pts = np.concatenate([accessor(doc, buf, p["attributes"]["POSITION"])
                          for mi in meshes for p in doc["meshes"][mi]["primitives"]])
    ext = pts.max(0) - pts.min(0)
    # (map_extract writes Halo's model space, Z up; the root node turns it to glTF's Y up)
    height = float(ext[2])
    return {"model": os.path.relpath(path, extract), "extent_wu_xyz": [round(float(v), 4) for v in ext],
            "height_wu": round(height, 4), "height_m": round(height * WU_M, 3),
            "units_note": doc.get("extras", {}).get("units"),
            "ok": bool(1.8 <= height * WU_M <= 2.4)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extract", required=True)
    ap.add_argument("--shader", action="append", default=[])
    ap.add_argument("--json")
    a = ap.parse_args()
    check = chief_height(a.extract)
    print("unit check:", json.dumps(check), file=sys.stderr)
    found = collect(a.extract, set(a.shader))
    out = {"wu_m": WU_M, "unit_check": check, "shaders": {}}
    for name in (a.shader or sorted(found)):
        if name not in found:
            print(json.dumps({"shader": name, "error": "not found"}))
            continue
        out["shaders"][name] = density(found[name])
        if len(out["shaders"][name]["models"]) > 1:
            out["shaders"][name]["by_model"] = by_model(found[name])
        print(json.dumps(dict(shader=name, **out["shaders"][name])))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    return 0 if check is None or check["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
