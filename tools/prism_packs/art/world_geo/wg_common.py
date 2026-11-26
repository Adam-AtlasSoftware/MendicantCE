"""PRISM world geometry: the structure BSP as map_extract writes it (glTF), and the .pwg remodel file
the engine draws in its place (port/linux/game/prism_world_geometry.c, display.prism_world_geometry).

Plain numpy; imported both by the command-line tools and inside headless Blender.

Everything read here is derived from the player's own map, so everything written stays local
(never committed, never shipped).
"""
import json
import os
import struct

import numpy as np

PWG_MAGIC = b"PWG1"
PWG_VERSION = 1

# The grids a written remodel's positions and coordinates are snapped to: a shipped light bake of the remodel holds
# only where the player's own build of it is the same file, and floating point's last bits differ between machines
# (SIMD widths, FFT and BLAS kernels, thread counts). Snapped, a position that came out a few ulps off still writes
# the same bytes. Positions 1/4096 wu (0.07 mm), base map coordinates 1/65536 (a 64th of a 1024 texture's texel),
# lightmap coordinates 1/1048576; a bounding sphere's centre to the positions' grid, its radius rounded up.
POSITION_GRID = 1.0 / 4096.0
UV_GRID = 1.0 / 65536.0
LIGHTMAP_UV_GRID = 1.0 / 1048576.0


def numpy_kernels():
    """the CPU features numpy's kernels use here (NPY_DISABLE_CPU_FEATURES pins them to its baseline: prism_packs'
    deterministic_env), for the build's log"""
    try:
        core = getattr(np, "_core", None)
        um = core._multiarray_umath if core is not None and hasattr(core, "_multiarray_umath") else \
            np.core._multiarray_umath
        return " ".join(k for k, v in um.__cpu_features__.items() if v and k in set(um.__cpu_baseline__) |
                        set(um.__cpu_dispatch__))
    except AttributeError:
        return "?"


def digest(*arrays):
    """a short digest of arrays' bytes (the build's log: where two builds part)"""
    import hashlib
    h = hashlib.sha256()
    for a in arrays:
        a = np.ascontiguousarray(a)
        h.update(str(a.dtype).encode() + str(a.shape).encode())
        h.update(a.tobytes())
    return h.hexdigest()[:12]


def snap(a, grid):
    """a (float) rounded to the grid (a power of two: exact in float32 within the level's range), as float32"""
    return (np.round(np.asarray(a, np.float64) / grid) * grid).astype(np.float32)


def snap_vertices(verts):
    """a .pwg part's vertices (n, 14: position, normal, binormal, tangent, base coordinates) snapped"""
    v = np.array(verts, np.float32, copy=True)
    v[:, 0:3] = snap(v[:, 0:3], POSITION_GRID)
    v[:, 12:14] = snap(v[:, 12:14], UV_GRID)
    return v


def snap_lightmap(lm):
    """a .pwg part's lightmap vertices (n, 5: incident radiosity, lightmap coordinates) snapped"""
    v = np.array(lm, np.float32, copy=True)
    v[:, 3:5] = snap(v[:, 3:5], LIGHTMAP_UV_GRID)
    return v


def snap_bounds(bounds):
    """bounding spheres (n, 4: centre, radius) on the positions' grid, each radius rounded up (still bounding)"""
    b = np.array(bounds, np.float64, copy=True).reshape(-1, 4)
    b[:, 0:3] = np.round(b[:, 0:3] / POSITION_GRID) * POSITION_GRID
    b[:, 3] = np.ceil(b[:, 3] / POSITION_GRID + 1.0) * POSITION_GRID
    return b.astype(np.float32)


class Material:
    """One structure_material of a lightmap: its vertices in the material's order and its surfaces
    (stock triangles) in surface order, with the vertex indices as the game holds them."""

    def __init__(self, **kw):
        self.__dict__.update(kw)

    @property
    def surface_count(self):
        return len(self.triangles)

    @property
    def vertex_count(self):
        return len(self.position)


def _accessor(gltf, binary, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    dtype = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[acc["componentType"]]
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    stride = view.get("byteStride", 0)
    item = np.dtype(dtype).itemsize * comps
    if stride and stride != item:
        raw = np.frombuffer(binary, np.uint8, count=stride * count, offset=offset).reshape(count, stride)[:, :item]
        return raw.copy().view(dtype).reshape(count, comps)
    return np.frombuffer(binary, dtype, count=count * comps, offset=offset).reshape(count, comps).copy()


def load_bsp(gltf_path):
    """The BSP's materials from a map_extract structure_bsp glTF. Returns (info, [Material])."""
    with open(gltf_path) as f:
        gltf = json.load(f)
    with open(os.path.join(os.path.dirname(gltf_path), gltf["buffers"][0]["uri"]), "rb") as f:
        binary = f.read()
    flipped = bool(gltf.get("extras", {}).get("winding_flipped", False))
    materials = []
    for prim in gltf["meshes"][0]["primitives"]:
        mat = gltf["materials"][prim["material"]]
        ex = mat["extras"]
        a = prim["attributes"]
        tri = _accessor(gltf, binary, prim["indices"]).reshape(-1, 3).astype(np.int64)
        if flipped:
            tri = tri[:, [0, 2, 1]]
        tangent = _accessor(gltf, binary, a["TANGENT"])
        normal = _accessor(gltf, binary, a["NORMAL"])
        binormal = np.cross(normal, tangent[:, :3]) * tangent[:, 3:4]
        n = len(normal)
        materials.append(Material(
            name=mat["name"],
            shader=ex.get("shader", mat["name"]),
            shader_group=ex.get("shader_group", ""),
            maps=ex.get("maps", {}),
            lightmap_index=int(ex["lightmap_index"]),
            material_index=int(ex["material_index"]),
            has_lightmap=bool(ex.get("has_lightmap_vertices", False)),
            position=_accessor(gltf, binary, a["POSITION"]).astype(np.float32),
            normal=normal.astype(np.float32),
            tangent=tangent[:, :3].astype(np.float32),
            binormal=binormal.astype(np.float32),
            uv0=_accessor(gltf, binary, a["TEXCOORD_0"]).astype(np.float32),
            uv1=(_accessor(gltf, binary, a["TEXCOORD_1"]).astype(np.float32) if "TEXCOORD_1" in a
                 else np.zeros((n, 2), np.float32)),
            radiosity=(_accessor(gltf, binary, a["_INCIDENT_RADIOSITY"]).astype(np.float32)
                       if "_INCIDENT_RADIOSITY" in a else np.zeros((n, 3), np.float32)),
            triangles=tri,
        ))
    info = {"tag": gltf.get("extras", {}).get("tag", ""), "gltf": gltf_path, "flipped": flipped}
    return info, materials


def topology_check(triangles):
    """sum over surfaces s of (3a + 5b + 7c)(s + 1) mod 2^32 (prism_world_geometry.c)"""
    t = triangles.astype(np.uint64)
    mask = np.uint64(0xFFFFFFFF)
    s = np.arange(1, len(t) + 1, dtype=np.uint64)
    terms = ((np.uint64(3) * t[:, 0] + np.uint64(5) * t[:, 1] + np.uint64(7) * t[:, 2]) & mask) * s & mask
    return int(sum(int(x) for x in terms) & 0xFFFFFFFF)


def position_check(position):
    """sum over vertices of x + 2y + 3z, in doubles, in the vertices' order (prism_world_geometry.c)"""
    p = position.astype(np.float64)
    total = 0.0
    for v in (p[:, 0] + 2.0 * p[:, 1] + 3.0 * p[:, 2]):
        total += float(v)
    return total


class Remodel:
    """A material's remodel: corner-split vertices and triangles grouped by the stock surface each
    stands for (surface_first_triangle[s] .. [s + 1])."""

    def __init__(self, stock, position, normal, binormal, tangent, uv0, uv1, radiosity, triangles,
                 surface_of_triangle):
        # by surface, and inside a surface along a Morton curve of the triangles' centres: neighbours drawn together,
        # so the GPU's vertex cache and fetches find what the last triangles used (the centres of the snapped
        # positions: a raw position a few ulps off moved a triangle across a cell's edge, and the order with it)
        pos = snap(position, POSITION_GRID).astype(np.float64)
        tri_ = np.asarray(triangles, np.int64)
        cen = pos[tri_].mean(1) if len(tri_) else np.zeros((0, 3))
        lo = cen.min(0) if len(cen) else np.zeros(3)
        cell = np.clip(((cen - lo) / 0.05).astype(np.int64), 0, (1 << 20) - 1)
        code = np.zeros(len(cen), np.int64)
        for bit in range(20):
            for axis in range(3):
                code |= ((cell[:, axis] >> bit) & 1) << (3 * bit + axis)
        # (ties, triangles in one cell, broken by their corners' snapped positions and base coordinates, not by the
        # order they came in: two builds that made the same triangles in another order write the same file)
        uvq = np.round(np.asarray(uv0, np.float64) / UV_GRID).astype(np.int64)
        grid = np.round(pos / POSITION_GRID).astype(np.int64)
        ties = []
        for k in range(3):
            if len(tri_):
                ties += [uvq[tri_[:, k], 1], uvq[tri_[:, k], 0], grid[tri_[:, k], 2], grid[tri_[:, k], 1],
                         grid[tri_[:, k], 0]]
        order = np.lexsort(tuple(ties[::-1]) + (code, np.asarray(surface_of_triangle))) if len(tri_) else \
            np.zeros(0, np.int64)
        self.stock = stock
        self.position = snap(position, POSITION_GRID)
        self.normal = np.asarray(normal, np.float32)
        self.binormal = np.asarray(binormal, np.float32)
        self.tangent = np.asarray(tangent, np.float32)
        self.uv0 = snap(uv0, UV_GRID)
        self.uv1 = snap(uv1, LIGHTMAP_UV_GRID)
        self.radiosity = np.asarray(radiosity, np.float32)
        self.triangles = np.asarray(triangles, np.int64)[order]
        counts = np.bincount(np.asarray(surface_of_triangle)[order], minlength=stock.surface_count)
        self.surface_first_triangle = np.concatenate([[0], np.cumsum(counts)]).astype(np.uint32)
        assert self.surface_first_triangle[-1] == len(self.triangles)

    def parts(self, limit=65535):
        """[(first surface, surface count, vertex ids, local triangles, local surface_first_triangle)], each part a
        run of surfaces using at most `limit` vertices (the engine's 16-bit indices)"""
        out = []
        sft = self.surface_first_triangle
        n = self.stock.surface_count
        start = 0
        used = set()
        for s in range(n + 1):
            new = set()
            if s < n:
                new = set(np.unique(self.triangles[sft[s]:sft[s + 1]]).tolist()) - used
                assert len(new) <= limit, "one surface over the vertex limit"
            if s == n or len(used) + len(new) > limit:
                tri = self.triangles[sft[start]:sft[s]]
                # (vertices numbered in the order the triangles first use them)
                _, first_use = np.unique(tri.ravel(), return_index=True)
                ids = tri.ravel()[np.sort(first_use)]
                remap = np.full(len(self.position), -1, np.int64)
                remap[ids] = np.arange(len(ids))
                out.append((start, s - start, ids, remap[tri], (sft[start:s + 1] - sft[start]).astype(np.uint32)))
                start = s
                used = set()
                if s < n:
                    # (the surface opens the new part: all its vertices count, not only those the last part lacked)
                    new = set(np.unique(self.triangles[sft[s]:sft[s + 1]]).tolist())
            used |= new
        return out


def _pad4(b):
    return b + b"\0" * (-len(b) % 4)


def write_pwg(path, bsp_tag, remodels, lod_distance=0.0):
    """The .pwg file of a BSP's remodelled materials (format: prism_world_geometry.c). Version 2 when any remodel
    has a far level (r.lod1: per stock surface, its far triangles as row ids, or None to draw the near ones far too):
    each part's far triangles follow its near ones, then a (first, count) pair per surface and each surface's bounding
    sphere (centre, radius) follow the triangles."""
    lod = any(getattr(r, "lod1", None) is not None for r in remodels)
    out = bytearray()
    out += PWG_MAGIC + struct.pack("<III", 2 if lod else PWG_VERSION, len(remodels), 1 if lod else 0)
    out += bsp_tag.encode()[:47].ljust(48, b"\0")
    assert len(out) == 64
    count = 0
    for r in remodels:
        st = r.stock
        topo, posc = topology_check(st.triangles), position_check(st.position)
        for first, scount, ids, tri, sft in r.parts():
            near_count = len(tri)
            if lod:
                remap = np.full(len(r.position), -1, np.int64)
                remap[ids] = np.arange(len(ids))
                far, pairs, bounds = [], np.zeros((scount, 2), np.uint32), np.zeros((scount, 4), np.float32)
                cursor = near_count
                l1 = getattr(r, "lod1", None)
                for j in range(scount):
                    s_ = first + j
                    a_, b_ = int(sft[j]), int(sft[j + 1])
                    pts = r.position[ids[np.unique(tri[a_:b_])]] if b_ > a_ else np.zeros((1, 3), np.float32)
                    lo, hi = pts.min(0), pts.max(0)
                    cen = (lo + hi) * 0.5
                    bounds[j] = [cen[0], cen[1], cen[2], float(np.linalg.norm(pts - cen, axis=1).max())]
                    f1 = l1[s_] if l1 is not None else None
                    if f1 is None or b_ <= a_:
                        pairs[j] = [a_, b_ - a_]
                        continue
                    loc = remap[f1]
                    assert (loc >= 0).all(), "a far triangle off its part's vertices"
                    far.append(loc)
                    pairs[j] = [cursor, len(loc)]
                    cursor += len(loc)
                if far:
                    tri = np.concatenate([tri] + far)
            begin = len(out)
            out += struct.pack("<hhIIIII", st.lightmap_index, st.material_index, st.surface_count, st.vertex_count,
                               len(ids), len(tri), topo)
            out += struct.pack("<dII", posc, first, scount)
            out += struct.pack("<If", near_count, float(lod_distance)) + b"\0" * 16
            assert len(out) - begin == 64
            out += _pad4(sft.astype("<u4").tobytes())
            verts = np.concatenate([r.position[ids], r.normal[ids], r.binormal[ids], r.tangent[ids], r.uv0[ids]],
                                   axis=1).astype("<f4")
            out += _pad4(verts.tobytes())
            lm = np.concatenate([r.radiosity[ids], r.uv1[ids]], axis=1).astype("<f4")
            out += _pad4(lm.tobytes())
            out += _pad4(tri.astype("<u2").tobytes())
            if lod:
                out += pairs.astype("<u4").tobytes()
                out += snap_bounds(bounds).astype("<f4").tobytes()
            count += 1
    struct.pack_into("<I", out, 8, count)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return len(out)


def identity_remodel(m):
    """The stock material as a remodel (for checking the engine path: draws as stock)."""
    return Remodel(m, m.position, m.normal, m.binormal, m.tangent, m.uv0, m.uv1, m.radiosity, m.triangles,
                   np.arange(m.surface_count))
