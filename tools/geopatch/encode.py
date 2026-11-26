#!/usr/bin/env python3
"""Encode a level's enhanced world into its sidecar (PLAN_FINAL.md section 2.2 step 5): each BSP's remodel (today's
.pwg v2 intermediate) becomes a GEOM section in the runtime layout (geom.py). Every vertex on a stock position of
its BSP is a REF op the engine resolves from the player's own map (REF/REFP: its own material's vertex, REFM:
another material's, REFC: a collision vertex), so the file holds no retail value; a vertex on a retail position of
another BSP, which a BSP cannot refer to, moves up one grid step. Positions go on the 1/1024 world unit grid,
columns are byte-plane shuffled, sections compressed.

    encode.py --map <retail map> --classic-world-id <64 hex> --pwg 0=b30a.pwg --pwg 1=b30b.pwg -o b30.prsx
              [--codec zlib|zstd|stored] [--level N] [--lmuv] [--report report.json]

--lmuv adds an LMUV section per BSP sized like the farm's own lightmap layout (two 16-bit coordinates and a page
per vertex, here from the .pwg's projected coordinates): for measuring only, until the farm's xatlas layout (M4)
writes the real one. zstd uses the system's zstd program and is for measuring only: the game reads stored and
zlib sections (prism_prsx.c) until a zstd decoder is vendored.

The farm only: it reads retail maps, which never leave the farm; what it writes is checked by lint_patch.py and
scan_original.py before anything ships.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from geopatch import geom, prsx, retail  # noqa: E402
from geopatch.geom import LIT, REF, REFC, REFM, REFP  # noqa: E402

PWG_MAGIC = b"PWG1"


# ---------- the .pwg intermediate (port/linux/game/prism_world_geometry.c has the format)

class PwgPart:
    pass


def read_pwg(path):
    data = Path(path).read_bytes()
    if data[:4] != PWG_MAGIC:
        raise ValueError("%s: not a .pwg" % path)
    version, count, flags = struct.unpack_from("<III", data, 4)
    far = bool(flags & 1)
    at = 64
    parts = []

    def take(nbytes):
        nonlocal at
        chunk = data[at:at + nbytes]
        if len(chunk) != nbytes:
            raise ValueError("%s: cut short" % path)
        at += (nbytes + 3) & ~3
        return chunk

    for _ in range(count):
        p = PwgPart()
        (p.lightmap_index, p.material_index, p.stock_surface_count, p.stock_vertex_count, vcount, tcount,
         p.topology_check) = struct.unpack_from("<hhIIIII", data, at)
        p.position_check, p.first_surface, p.surface_count = struct.unpack_from("<dII", data, at + 24)
        p.near_triangle_count, p.far_distance = struct.unpack_from("<If", data, at + 40)
        if version < 2:
            p.near_triangle_count, p.far_distance = tcount, 0.0
        at += 64
        p.sft = np.frombuffer(take(4 * (p.surface_count + 1)), "<u4").copy()
        p.vertices = np.frombuffer(take(56 * vcount), "<f4").reshape(vcount, 14).copy()
        p.lightmap = np.frombuffer(take(20 * vcount), "<f4").reshape(vcount, 5).copy()
        p.triangles = np.frombuffer(take(6 * tcount), "<u2").reshape(tcount, 3).astype(np.int64)
        p.far_pairs = p.spheres = None
        if far:
            p.far_pairs = np.frombuffer(take(8 * p.surface_count), "<u4").reshape(-1, 2).copy()
            p.spheres = np.frombuffer(take(16 * p.surface_count), "<u4").reshape(-1, 4).copy()
        parts.append(p)
    return parts


# ---------- packing

def pack_vectors(v):
    """unit vectors (N x 3 floats) packed 11:11:10 (nearest field value, as synthmap.pack_vector)"""
    v = v.astype(np.float64)

    def field(x, bits):
        top = (1 << (bits - 1)) - 1
        s = np.clip(np.rint((x * ((1 << bits) - 1) - 1) / 2), -top - 1, top).astype(np.int64)
        return (s & ((1 << bits) - 1)).astype(np.uint64)
    return (field(v[:, 0], 11) | field(v[:, 1], 11) << np.uint64(11) | field(v[:, 2], 10) << np.uint64(22)) \
        .astype(np.uint32)


def grid(values):
    q = np.rint(values.astype(np.float64) * geom.GRID).astype(np.int64)
    if q.size and np.abs(q).max() >= geom.GRID_LIMIT:
        raise geom.LayoutError("a position beyond 16384 world units")
    return q


def zigzag(d):
    d = d.astype(np.int64)
    return (((d << 1) ^ (d >> 63)) & 0xFFFFFFFF).astype(np.uint32)


def shuffled(values, width):
    raw = np.ascontiguousarray(values.astype("<u4" if width == 4 else "u1"))
    if width == 1:
        return raw.tobytes()
    return raw.view(np.uint8).reshape(-1, width).T.tobytes()


def columns_bytes(columns):
    """geom._columns_bytes for numpy columns: [(name, width, array)]"""
    table, blob = bytearray(), bytearray()
    for name, width, values in columns:
        data = shuffled(values, width)
        table += struct.pack("<4sIII", name.encode(), width, len(values), len(data))
        blob += data
    return bytes(table), bytes(blob)


class EncodedPart:
    """a GEOM part as arrays: ops (u8), refs (u32), q (LIT x 3 int64), nrm (attributed x 3 u32), tex (attributed x 2
    u32), triangles (T x 3), sft, far pairs, spheres, and the header numbers"""


def encode_geom_arrays(parts):
    """GEOM v1 bytes from EncodedParts: the same bytes geom.encode_geom writes for the same content (tested)"""
    header = bytearray()
    ops, refs, keys, qs, nrm, tex, corners, runs, far, spheres = [], [], [], [], [], [], [], [], [], []
    for p in parts:
        has_far = p.far_pairs is not None
        header += struct.pack("<HHIIIIIIIIIII", p.lightmap_index & 0xFFFF, p.material_index & 0xFFFF,
                              p.stock_surface_count, p.stock_vertex_count, p.topology_check & 0xFFFFFFFF,
                              p.first_surface, p.surface_count, len(p.ops), len(p.triangles),
                              p.near_triangle_count, geom.float_bits(p.far_distance), 1 if has_far else 0,
                              getattr(p, "static_set", 0))
        ops.append(p.ops)
        refs.append(p.refs)
        keys.append(getattr(p, "keys", np.zeros(0, np.uint32)))
        qs.append(p.q)
        nrm.append(p.nrm)
        tex.append(p.tex)
        flat = p.triangles.reshape(-1).astype(np.int64)
        if flat.size and (flat.min() < 0 or flat.max() >= len(p.ops)):
            raise geom.LayoutError("a triangle's index outside its part")
        corners.append(zigzag(np.diff(flat, prepend=0)))
        runs.append(p.sft.astype(np.uint32))
        if has_far:
            far.append(p.far_pairs.reshape(-1).astype(np.uint32))
            spheres.append(p.spheres.reshape(-1).astype(np.uint32))
    cat = lambda xs, dtype: np.concatenate(xs).astype(dtype) if xs else np.zeros(0, dtype)  # noqa: E731
    q = np.concatenate(qs) if qs else np.zeros((0, 3), np.int64)
    pos = zigzag(np.diff(q, axis=0, prepend=np.zeros((1, 3), np.int64)).reshape(-1)) if len(q) else np.zeros(0)
    columns = [("VOPS", 1, cat(ops, np.uint8)), ("VREF", 4, cat(refs, np.uint32)), ("VPOS", 4, pos),
               ("VKEY", 4, cat(keys, np.uint32)), ("VNRM", 4, cat([x.reshape(-1) for x in nrm], np.uint32)),
               ("VTEX", 4, cat([x.reshape(-1) for x in tex], np.uint32)),
               ("TIDX", 4, cat(corners, np.uint32)), ("SRUN", 4, cat(runs, np.uint32)),
               ("FRUN", 4, cat(far, np.uint32)), ("FSPH", 4, cat(spheres, np.uint32))]
    table, blob = columns_bytes(columns)
    return geom.GEOM_MAGIC + struct.pack("<III", geom.GEOM_VERSION, len(parts), len(columns)) + bytes(header) + \
        table + blob


def _bsp_lookup(bsp):
    """{grid position: (op, reference)} of every stock position of a BSP: its materials' vertices (REFM: (lightmap,
    material, index)) and its collision vertices (REFC: index); the first one found is kept. Cached on the BSP"""
    cached = getattr(bsp, "_geopatch_lookup", None)
    if cached is not None:
        return cached
    lookup = {}
    for (lightmap, material_index), material in sorted(bsp.materials.items()):
        stock = grid(np.frombuffer(material.vertices, "<f4").reshape(-1, 8)[:, 0:3])
        for index, key in enumerate(map(tuple, stock.tolist())):
            lookup.setdefault(key, (REFM, (lightmap, material_index, index)))
    if bsp.collision_vertices:
        for index, key in enumerate(map(tuple, grid(np.array(bsp.collision_vertices, np.float32)).tolist())):
            lookup.setdefault(key, (REFC, index))
    bsp._geopatch_lookup = lookup
    return lookup


def encode_pwg_part(p, material, native=None, bsp=None, avoid=None):
    """one .pwg part as an EncodedPart against its stock material: a vertex equal in all 56 bytes to the stock
    vertex's exact decode is REF, one on a stock vertex's grid position REFP; with the BSP, one on another
    material's stock position REFM and one on a collision vertex REFC; others LIT. avoid: grid positions no LIT
    vertex may take (every retail position of the map's other BSPs, which a BSP cannot refer to: only one BSP is in
    memory): such a vertex moves up one grid step (1/1024 world unit) until it is free. Returns (part, counts)"""
    n = len(p.vertices)
    q = grid(p.vertices[:, 0:3])
    ops = np.zeros(n, np.uint8)
    refs = np.zeros(n, np.int64)
    keys = np.zeros(n, np.int64)
    lookup = {}
    decoded = None
    if material is not None and material.vertex_count:
        stock_q = grid(np.frombuffer(material.vertices, "<f4").reshape(-1, 8)[:, 0:3])
        for index, key in enumerate(map(tuple, stock_q.tolist())):
            lookup.setdefault(key, index)
        if native is not None:
            decoded = np.frombuffer(native.environment_vertices(material.vertices), "<u4").reshape(-1, 14)
    wide = _bsp_lookup(bsp) if bsp is not None else {}
    bits = p.vertices.view("<u4")
    for v, key in enumerate(map(tuple, q.tolist())):
        index = lookup.get(key)
        if index is not None:
            refs[v] = index
            ops[v] = REF if decoded is not None and np.array_equal(bits[v], decoded[index]) else REFP
            continue
        found = wide.get(key)
        if found is None:
            continue
        if found[0] == REFM:
            lightmap, material_index, refs[v] = found[1]
            keys[v] = (lightmap & 0xFFFF) << 16 | (material_index & 0xFFFF)
            ops[v] = REFM
        else:
            refs[v] = found[1]
            ops[v] = REFC
    lit = ops == LIT
    nudged = 0
    if avoid:
        for v in np.nonzero(lit)[0]:
            key = tuple(q[v].tolist())
            if key in avoid:
                while key in avoid:
                    key = (key[0], key[1], key[2] + 1)
                q[v] = key
                nudged += 1
    attributed = ops != REF
    e = EncodedPart()
    for name in ("lightmap_index", "material_index", "stock_surface_count", "stock_vertex_count", "topology_check",
                 "first_surface", "surface_count", "near_triangle_count", "far_distance"):
        setattr(e, name, getattr(p, name))
    e.ops = ops
    e.refs = refs[ops != LIT].astype(np.uint32)
    e.keys = keys[ops == REFM].astype(np.uint32)
    e.q = q[lit]
    vec = p.vertices[attributed]
    e.nrm = np.stack([pack_vectors(vec[:, 3:6]), pack_vectors(vec[:, 6:9]), pack_vectors(vec[:, 9:12])], axis=1)
    e.tex = vec[:, 12:14].copy().view("<u4")
    e.triangles = p.triangles
    e.sft = p.sft
    e.far_pairs, e.spheres = p.far_pairs, p.spheres
    counts = {"vertices": n, "lit": int(lit.sum()), "ref": int((ops == REF).sum()), "refp": int((ops == REFP).sum()),
              "refm": int((ops == REFM).sum()), "refc": int((ops == REFC).sum()), "nudged": nudged,
              "triangles": int(len(p.triangles))}
    return e, counts


def encode_lmuv(parts_pwg):
    """an LMUV-sized section for measuring (see the module's help): per part its vertex count and page, per vertex
    two 16-bit coordinates"""
    header = bytearray(b"LMU0" + struct.pack("<II", 0, len(parts_pwg)))
    u = []
    for p in parts_pwg:
        header += struct.pack("<IHH", len(p.lightmap), p.lightmap_index & 0xFFFF, 0)
        u.append(np.clip(np.rint(p.lightmap[:, 3:5].astype(np.float64) * 65535), 0, 65535).astype(np.uint16))
    flat = np.concatenate(u).reshape(-1) if u else np.zeros(0, np.uint16)
    planes = np.ascontiguousarray(flat.astype("<u2")).view(np.uint8).reshape(-1, 2).T.tobytes()
    return bytes(header) + planes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", help="the retail map (REF resolution); without it every vertex is LIT, which the "
                                      "lint refuses: for measuring only")
    parser.add_argument("--classic-world-id", required=True)
    parser.add_argument("--pwg", action="append", default=[], metavar="BSP=FILE")
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("--codec", choices=["zlib", "zstd", "stored"], default="zlib")
    parser.add_argument("--level", type=int)
    parser.add_argument("--lmuv", action="store_true")
    parser.add_argument("--report")
    parser.add_argument("--native-dir")
    args = parser.parse_args(argv)
    cwid = bytes.fromhex(args.classic_world_id)
    codec = {"zlib": prsx.ZLIB, "zstd": prsx.ZSTD, "stored": prsx.STORED}[args.codec]
    level = args.level if args.level is not None else (19 if codec == prsx.ZSTD else 9)
    bsps = {}
    native = None
    if args.map:
        from geopatch import native as native_module
        _, _, retail_bsps = retail.read_map(args.map)
        bsps = {b.index: b for b in retail_bsps}
        native = native_module.Native(args.native_dir)
    sections, report = [], {"bsps": {}}
    # (every retail position of the map, on the grid: a LIT vertex never takes one)
    avoid = set()
    for b in bsps.values():
        avoid |= set(_bsp_lookup(b))
    for spec in args.pwg:
        index, _, path = spec.partition("=")
        index = int(index)
        pwg_parts = read_pwg(path)
        encoded, totals = [], {}
        for p in pwg_parts:
            material = bsps[index].materials.get((p.lightmap_index, p.material_index)) if index in bsps else None
            e, counts = encode_pwg_part(p, material, native, bsps.get(index), avoid)
            encoded.append(e)
            for key, value in counts.items():
                totals[key] = totals.get(key, 0) + value
        data = encode_geom_arrays(encoded)
        sections.append(prsx.Section("GEOM", index, data, codec=codec, level=level))
        entry = {"pwg_bytes": Path(path).stat().st_size, "geom_decoded": len(data), **totals}
        if args.lmuv:
            lm = encode_lmuv(pwg_parts)
            sections.append(prsx.Section("LMUV", index, lm, codec=codec, level=level))
            entry["lmuv_decoded"] = len(lm)
        report["bsps"][index] = entry
    blob = prsx.write(cwid, sections)
    Path(args.out).write_bytes(blob)
    # (the directory as written: a zstd file is for measuring only, and the readers refuse it)
    for index in range(len(sections)):
        kind, bsp, _, _, _, csize = struct.unpack_from("<IHHIQQ", blob, prsx.HEADER_SIZE + prsx.ENTRY_SIZE * index)
        report["bsps"][bsp][prsx.fourcc_text(kind).lower() + "_compressed"] = csize
    if codec != prsx.ZSTD:
        prsx.Sidecar(blob)
    report["file_bytes"] = len(blob)
    report["codec"] = args.codec
    report["level"] = level
    text = json.dumps(report, indent=1)
    if args.report:
        Path(args.report).write_text(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
