#!/usr/bin/env python3
"""Encode a level's enhanced world into its sidecar (PLAN_FINAL.md section 2.2 step 5): each BSP's remodel (today's
.pwg v2 intermediate) becomes a GEOM section in the runtime layout (geom.py). Every vertex on a stock position of
its BSP is a REF op the engine resolves from the player's own map (REF/REFP: its own material's vertex, REFM:
another material's, REFC: a collision vertex), so the file holds no retail value; a vertex on a retail position of
another BSP, which a BSP cannot refer to, moves up one grid step. Positions go on the 1/1024 world unit grid,
columns are byte-plane shuffled, sections compressed.

    encode.py --map <retail map> --classic-world-id <64 hex> --pwg 0=b30a.pwg --pwg 1=b30b.pwg -o b30.prsx
              [--codec zlib|zstd|stored] [--level N] [--lmuv] [--report report.json]
    encode.py --map <retail map> --classic-world-id <64 hex> --pwg-dir <the level's .pwg folder> -o b30.prsx

--pwg-dir gives each .pwg of the folder the BSP whose tag path its header names (the scenario's own order), never
the order of a directory listing (which follows the locale: a10_space sorts first under C, after a10g under
en_US); with --map, a --pwg BSP=FILE whose header names another BSP is refused. A BSP with no .pwg is not in the
sidecar (it draws classic).

Sections are deflated by the vendored zlib (port/third_party/zlib, through native.py), so the same inputs give the
same bytes on every machine; --system-zlib (Python's own zlib) is for measuring only.

Before anything else each BSP's remodel is held to the gameplay rule (clamp.py, PLAN_FINAL.md section 10: the
ground within 0.09 wu of the stock surface, walls within reach within 0.24 wu; the report's "clamp" says what
moved); --no-clamp keeps the .pwg's positions, for measuring only.

Each GEOM comes with its LMUV: by default v2, our own lightmap layout (lmuv.py: charts of the remodel's own
triangles packed into our own pages; a vertex on a chart seam is copied, and a part past 65535 vertices is cut by
its surfaces), which carries nothing of the retail map; --lmuv-version 1 writes M2's stand-in (geom.py: each
vertex's CE lightmap vertex, a REF where it is the stock one). The packed vectors are rounded as the game's own
compressor rounds them (rasterizer_geometry_compress_vertices), so the game draws exactly what the .pwg drew; only
the positions move, onto the 1/1024 world unit grid. --lmuv is accepted and changes nothing. zstd uses
the system's zstd program and is for measuring only: the game reads stored and zlib sections (prism_prsx.c) until a
zstd decoder is vendored.

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
from geopatch import clamp, geom, lmuv, prsx, retail  # noqa: E402
from geopatch.geom import LIT, REF, REFC, REFM, REFMT, REFP, REFPT  # noqa: E402

PWG_MAGIC = b"PWG1"


def pwg_bsp_path(path):
    """the BSP tag path a .pwg's header names (bytes 16 to 63), lower case with backslashes, as the map's tag names"""
    with open(path, "rb") as f:
        head = f.read(64)
    if head[:4] != PWG_MAGIC:
        raise ValueError("%s: not a .pwg" % path)
    return head[16:64].split(b"\0")[0].decode("ascii", "replace").replace("/", "\\").lower()


def pwgs_by_bsp(folder, retail_bsps):
    """{BSP index: .pwg path} of every .pwg in folder, each given the BSP whose tag path its header names; a .pwg
    naming no BSP of the map, or two naming one, is refused"""
    names = {b.name.replace("/", "\\").lower(): b.index for b in retail_bsps}
    out = {}
    for path in sorted(Path(folder).glob("*.pwg"), key=lambda p: p.name.encode()):
        name = pwg_bsp_path(path)
        if name not in names:
            raise ValueError("%s names BSP %s, which the map does not have" % (path, name))
        index = names[name]
        if index in out:
            raise ValueError("%s and %s both name BSP %d (%s)" % (out[index], path, index, name))
        out[index] = path
    return dict(sorted(out.items()))
# a remodel vertex within this many grid steps (on every axis) of a stock position of its BSP is that stock
# position: float noise in the art pipeline leaves kept stock vertices a step or two off (b30's second BSP had
# about 2,000), and as LIT they would be stock data a hair away; snapped, they are REFs. The lint's nudge rule looks
# the same distance (lint_patch.NUDGE_STEPS)
SNAP_STEPS = 4


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
    """vectors (N x 3 floats) packed 11:11:10 as the game's compressor packs them
    (compress_real_vector3d_to_int32_clamp, rasterizer_geometry.c): each component pinned to [-1, 1], times 1023.5
    (511.5 for z) in single precision, floored; its decode (2 s + 1) / 2047 floors back to s, so the game packs the
    decoded vector again to the same bits"""
    v = np.clip(np.asarray(v, np.float32), np.float32(-1.0), np.float32(1.0))

    def field(x, scale, bits):
        s = np.floor(x * np.float32(scale)).astype(np.int64)
        return (s & ((1 << bits) - 1)).astype(np.uint64)
    return (field(v[:, 0], 1023.5, 11) | field(v[:, 1], 1023.5, 11) << np.uint64(11) |
            field(v[:, 2], 511.5, 10) << np.uint64(22)).astype(np.uint32)


def pack_coordinates(c):
    """lightmap coordinates (N floats) as the game's compress_real_to_int16_clamp: pinned to [-1, 1], times 32767.5
    in single precision, floored; as unsigned 16-bit fields"""
    c = np.clip(np.asarray(c, np.float32), np.float32(-1.0), np.float32(1.0))
    return (np.floor(c * np.float32(32767.5)).astype(np.int64) & 0xFFFF).astype(np.uint32)


def compress_lightmap(lightmap):
    """.pwg lightmap vertices (N x 5 floats: incident radiosity, u, v) -> (radiosity u32, u, v) arrays, the game's
    compressed lightmap vertex"""
    return pack_vectors(lightmap[:, 0:3]), pack_coordinates(lightmap[:, 3]), pack_coordinates(lightmap[:, 4])


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
        header += geom.part_record(p.lightmap_index, p.material_index, p.stock_surface_count,
                                   p.stock_vertex_count, p.topology_check, p.first_surface, p.surface_count,
                                   len(p.ops), len(p.triangles), p.near_triangle_count, p.far_distance, has_far,
                                   getattr(p, "static_set", 0), getattr(p, "material_namespace", 0),
                                   getattr(p, "prism_material", 0))
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
    return geom.geom_header(len(parts), len(columns)) + bytes(header) + table + blob


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


def _nearest_within(points, stock, steps):
    """retail.nearest_within's index alone"""
    return retail.nearest_within(points, stock, steps)[0]


def encode_pwg_part(p, material, native=None, bsp=None, avoid=None):
    """one .pwg part as an EncodedPart against its stock material: a vertex equal in all 56 bytes to the stock
    vertex's exact decode is REF, one on a stock vertex's grid position REFP; with the BSP, one on another
    material's stock position REFM and one on a collision vertex REFC; one within SNAP_STEPS of such a position (and
    on none) is snapped to the nearest; others LIT. A REFP or REFM vertex whose base
    map coordinates are the referenced stock vertex's own bit for bit takes them from it (REFPT, REFMT), so no
    stock coordinates are written out. avoid: grid positions no LIT
    vertex may take (every retail position of the map's other BSPs, which a BSP cannot refer to: only one BSP is in
    memory): such a vertex moves up one grid step (1/1024 world unit) until it is free; counts["nudged_run"] is the
    longest run of consecutive nudged vertices, which the lint refuses past NUDGE_RUN. Returns (part, counts)"""
    n = len(p.vertices)
    if len(p.lightmap) != n:
        raise geom.LayoutError("a part's lightmap vertices are not one for each vertex")
    q = grid(p.vertices[:, 0:3])
    ops = np.zeros(n, np.uint8)
    refs = np.zeros(n, np.int64)
    keys = np.zeros(n, np.int64)
    lookup = {}
    decoded = None
    stock_q = np.zeros((0, 3), np.int64)
    if material is not None and material.vertex_count:
        stock_q = grid(np.frombuffer(material.vertices, "<f4").reshape(-1, 8)[:, 0:3])
        for index, key in enumerate(map(tuple, stock_q.tolist())):
            lookup.setdefault(key, index)
        if native is not None:
            decoded = np.frombuffer(native.environment_vertices(material.vertices), "<u4").reshape(-1, 14)
    wide = _bsp_lookup(bsp) if bsp is not None else {}
    bits = p.vertices.view("<u4")

    def stock_tex(stock_material, index):
        """the stock compressed vertex's base map coordinates, as float bits"""
        return struct.unpack_from("<2I", stock_material.vertices, 32 * int(index) + 24)

    def own(v, index, exact):
        refs[v] = index
        if exact and decoded is not None and np.array_equal(bits[v], decoded[index]):
            ops[v] = REF
        else:
            ops[v] = REFPT if tuple(bits[v, 12:14].tolist()) == stock_tex(material, index) else REFP

    def other(v, found):
        if found[0] == REFM:
            lightmap, material_index, refs[v] = found[1]
            keys[v] = (lightmap & 0xFFFF) << 16 | (material_index & 0xFFFF)
            same = tuple(bits[v, 12:14].tolist()) == stock_tex(bsp.materials[(lightmap, material_index)], refs[v])
            ops[v] = REFMT if same else REFM
        else:
            refs[v] = found[1]
            ops[v] = REFC

    for v, key in enumerate(map(tuple, q.tolist())):
        index = lookup.get(key)
        if index is not None:
            own(v, index, True)
            continue
        found = wide.get(key)
        if found is not None:
            other(v, found)
    snapped = 0
    if material is not None:
        # (a vertex a few steps off a stock position of its BSP: its own material's first, then the others')
        rest = np.nonzero(ops == LIT)[0]
        near = _nearest_within(q[rest], stock_q, SNAP_STEPS)
        for v, index in zip(rest[near >= 0], near[near >= 0]):
            own(v, int(index), False)
        snapped += int((near >= 0).sum())
        if wide:
            rest = np.nonzero(ops == LIT)[0]
            wide_keys = getattr(bsp, "_geopatch_lookup_keys", None)
            if wide_keys is None:
                wide_keys = np.array(list(wide), np.int64).reshape(-1, 3)
                bsp._geopatch_lookup_keys = wide_keys
            near = _nearest_within(q[rest], wide_keys, SNAP_STEPS)
            for v, index in zip(rest[near >= 0], near[near >= 0]):
                other(v, wide[tuple(wide_keys[index].tolist())])
            snapped += int((near >= 0).sum())
    lit = ops == LIT
    nudged = run = longest = 0
    previous = None
    if avoid:
        for v in np.nonzero(lit)[0]:
            key = tuple(q[v].tolist())
            if key in avoid:
                while key in avoid:
                    key = (key[0], key[1], key[2] + 1)
                q[v] = key
                nudged += 1
                run = run + 1 if previous == v - 1 else 1
                previous = v
                longest = max(longest, run)
    attributed = ops != REF
    textured = attributed & (ops != REFPT) & (ops != REFMT)
    e = EncodedPart()
    for name in ("lightmap_index", "material_index", "stock_surface_count", "stock_vertex_count", "topology_check",
                 "first_surface", "surface_count", "near_triangle_count", "far_distance"):
        setattr(e, name, getattr(p, name))
    e.ops = ops
    e.refs = refs[ops != LIT].astype(np.uint32)
    e.keys = keys[(ops == REFM) | (ops == REFMT)].astype(np.uint32)
    e.q = q[lit]
    vec = p.vertices[attributed]
    e.nrm = np.stack([pack_vectors(vec[:, 3:6]), pack_vectors(vec[:, 6:9]), pack_vectors(vec[:, 9:12])], axis=1)
    e.tex = p.vertices[textured][:, 12:14].copy().view("<u4")
    # (the lightmap vertices: a REF to the stock one where the vertex is its own material's stock vertex and its
    # compressed lightmap vertex is that one's bit for bit, else ours)
    radiosity, lu, lv = compress_lightmap(p.lightmap)
    lops = np.zeros(n, np.uint8)
    lrefs = np.zeros(n, np.int64)
    if material is not None and material.lightmap_vertices:
        stock_lm = np.frombuffer(material.lightmap_vertices, "<u4").reshape(-1, 2)
        own_ref = np.isin(ops, (REF, REFP, REFPT))
        idx = refs.copy()
        ok = own_ref & (idx < len(stock_lm))
        cand = np.nonzero(ok)[0]
        same = (stock_lm[idx[cand], 0] == radiosity[cand]) & \
            (stock_lm[idx[cand], 1] == (lu[cand] | (lv[cand] << np.uint32(16))))
        lops[cand[same]] = REF
        lrefs[cand[same]] = idx[cand[same]]
    lit_lm = lops == LIT
    e.lops = lops
    e.lrefs = lrefs[lops == REF].astype(np.uint32)
    e.lwords = np.stack([radiosity[lit_lm], (lu[lit_lm] | (lv[lit_lm] << np.uint32(16)))], axis=1) \
        .astype(np.uint32).reshape(-1)
    e.triangles = p.triangles
    e.sft = p.sft
    e.far_pairs, e.spheres = p.far_pairs, p.spheres
    counts = {"vertices": n, "lit": int(lit.sum()), "ref": int((ops == REF).sum()), "refp": int((ops == REFP).sum()),
              "refm": int((ops == REFM).sum()), "refc": int((ops == REFC).sum()), "refpt": int((ops == REFPT).sum()),
              "refmt": int((ops == REFMT).sum()), "snapped": snapped, "nudged": nudged, "nudged_run": longest,
              "triangles": int(len(p.triangles)), "lightmap_ref": int((lops == REF).sum()),
              "lightmap_lit": int(lit_lm.sum())}
    return e, counts


def encode_lmuv_arrays(parts):
    """LMUV v1 bytes from EncodedParts (geom.encode_lmuv writes the same for the same content)"""
    header = b"".join(struct.pack("<I", len(p.ops)) for p in parts)
    cat = lambda xs, dtype: np.concatenate(xs).astype(dtype) if xs else np.zeros(0, dtype)  # noqa: E731
    table, blob = columns_bytes([("LOPS", 1, cat([p.lops for p in parts], np.uint8)),
                                 ("LREF", 4, cat([p.lrefs for p in parts], np.uint32)),
                                 ("LVTX", 4, cat([p.lwords for p in parts], np.uint32))])
    return geom.LMUV_MAGIC + struct.pack("<III", geom.LMUV_VERSION, len(parts), 3) + header + table + blob


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", help="the retail map (REF resolution); without it every vertex is LIT, which the "
                                      "lint refuses: for measuring only")
    parser.add_argument("--classic-world-id", required=True)
    parser.add_argument("--pwg", action="append", default=[], metavar="BSP=FILE")
    parser.add_argument("--pwg-dir", help="every .pwg of the folder, each for the BSP its header names (needs --map)")
    parser.add_argument("--system-zlib", action="store_true",
                        help="Python's own zlib, not the vendored deflate: the bytes then depend on the machine "
                             "(for measuring only)")
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("--codec", choices=["zlib", "zstd", "stored"], default="zlib")
    parser.add_argument("--level", type=int)
    parser.add_argument("--lmuv", action="store_true")
    parser.add_argument("--lmuv-version", type=int, choices=(1, 2), default=2,
                        help="2 (default): our own lightmap layout (lmuv.py); 1: M2's stand-in (CE's coordinates)")
    parser.add_argument("--report")
    parser.add_argument("--native-dir")
    parser.add_argument("--no-clamp", action="store_true",
                        help="the remodel's positions as they are, not held to the gameplay rule (clamp.py): for "
                             "measuring only")
    args = parser.parse_args(argv)
    cwid = bytes.fromhex(args.classic_world_id)
    codec = {"zlib": prsx.ZLIB, "zstd": prsx.ZSTD, "stored": prsx.STORED}[args.codec]
    level = args.level if args.level is not None else (19 if codec == prsx.ZSTD else 9)
    bsps = {}
    native = None
    from geopatch import native as native_module
    if args.map or not args.system_zlib:
        native = native_module.Native(args.native_dir)
    prsx.DEFLATE = None if args.system_zlib else native.deflate
    if args.map:
        _, _, retail_bsps = retail.read_map(args.map)
        bsps = {b.index: b for b in retail_bsps}
    specs = []
    for spec in args.pwg:
        index, _, path = spec.partition("=")
        specs.append((int(index), path))
    if args.pwg_dir:
        if not args.map:
            parser.error("--pwg-dir needs --map (the BSPs' tag paths)")
        specs += [(index, str(path)) for index, path in pwgs_by_bsp(args.pwg_dir, retail_bsps).items()]
    if len({index for index, _ in specs}) != len(specs):
        parser.error("two .pwg files for one BSP")
    for index, path in specs:
        if bsps and (index not in bsps or pwg_bsp_path(path) != bsps[index].name.replace("/", "\\").lower()):
            parser.error("%s is not BSP %d's remodel (its header names %s)" % (path, index, pwg_bsp_path(path)))
    sections, report = [], {"bsps": {}}
    # (every retail position of the map, on the grid: a LIT vertex never takes one)
    avoid = set()
    for b in bsps.values():
        avoid |= set(_bsp_lookup(b))
    for index, path in sorted(specs):
        pwg_parts = read_pwg(path)
        clamp_report = None
        if not args.no_clamp and index in bsps:
            clamp_report = clamp.clamp_bsp(pwg_parts, clamp.Stock.of_bsp(bsps[index], native))
        lmuv_report = None
        if args.lmuv_version == 2:
            pwg_parts, pages, lmuv_report = lmuv.layout(pwg_parts)
        encoded, totals = [], {}
        for p in pwg_parts:
            material = bsps[index].materials.get((p.lightmap_index, p.material_index)) if index in bsps else None
            e, counts = encode_pwg_part(p, material, native, bsps.get(index), avoid)
            encoded.append(e)
            for key, value in counts.items():
                totals[key] = max(totals.get(key, 0), value) if key == "nudged_run" else totals.get(key, 0) + value
        data = encode_geom_arrays(encoded)
        sections.append(prsx.Section("GEOM", index, data, codec=codec, level=level))
        entry = {"pwg_bytes": Path(path).stat().st_size, "geom_decoded": len(data), **totals}
        lm = lmuv.encode(pwg_parts, pages) if args.lmuv_version == 2 else encode_lmuv_arrays(encoded)
        sections.append(prsx.Section("LMUV", index, lm, codec=codec, level=level))
        entry["lmuv_decoded"] = len(lm)
        if lmuv_report is not None:
            entry["lmuv"] = {k: v for k, v in lmuv_report.items()}
        if clamp_report is not None:
            entry["clamp"] = clamp_report
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
        Path(args.report).write_text(text + "\n", newline="\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
