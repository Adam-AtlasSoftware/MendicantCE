"""PRISM v4 world geometry: the .pwg file's version 3 and the checks every built remodel passes.

The file (port/linux/game/prism_world_geometry.c reads versions 1 to 3), little endian:

  header, 64 bytes: "PWG1", version, the material records' count, flags, the BSP tag's path (48 bytes)
    flags: 1 far levels (version 2 on), 2 seal groups, 4 deviations, 8 the lightmap atlas (version 3)
  version 3: an extension, 64 bytes: the seal groups' count, the atlas's page count, a page's width and height
    (texels), its density (texels a world unit, a float), its chart count, 40 bytes zero
  each material record, 64 bytes (lightmap index and material index (16 bits each), the stock surface and vertex
    counts, the record's vertex and triangle counts, the topology check, the position check (a double), the record's
    first surface of the material's and its surface count, its near triangle count and far distance, 16 bytes zero),
    then its arrays, each padded to 4 bytes: surface_first_triangle[surfaces + 1], the vertices (56 bytes each), the
    lightmap vertices (20 bytes each), the triangles (three 16-bit indices); version 2: each surface's far triangles
    (first, count) and its bounding sphere (4 floats); version 3, after those:
      each surface's seal group (32 bits: an index of the file's groups)
      each surface's deviation, 4 floats (world units): the most its triangles (near and far) stand above its stock
        triangle's plane (along the level's outward normal: minus the cross product of its corners in the BSP's
        order) and below it, the most they reach past its edges in that plane, and the most its near level's vertices
        lie from its far level's surface (0 where both levels are the same triangles)
      each near triangle's lightmap atlas coordinates, 16 bytes: its chart (32 bits; 0xffffffff none), then its three
        corners' u and v (16 bits each, in eighths of a texel of the chart's page)
  version 3, after the last record: each chart's page (32 bits)

A seal group is a set of surfaces that switch between the remodel and their stock triangles together without a crack:
every stock edge between a surface of the group and one outside it is the stock edge on both sides, at both levels
(the edge's two corners joined by one edge of each side's triangles: no vertex between them, none moved off it). The
engine falls back whole groups. The atlas (xatlas, MIT) charts and packs each record's near triangles at
ATLAS_DENSITY texels a world unit on pages of ATLAS_PAGE texels; the baker scales its charts as it scales CE's.

The checks (check_shape, and wg2_cracks.py's seams for the open edges): no remodel vertex (near or far) further from its
stock triangle than NEEDLE_FLOOR or NEEDLE_RATIO times the triangle's longest edge, whichever is more (a needle: the
bevel's miter on a sliver, metres long), nor further off its plane or past its edges than BUILD_RELIEF and
BUILD_MARGIN (within the engine's SHAPE_RELIEF and SHAPE_MARGIN, a world unit each). fix_shape moves such vertices
back (every row at the position alike, so no edge opens): a needle's onto the stock triangle, the others just inside
those bounds, their texture and lightmap coordinates, frames and light taken from the stock triangle there.

Plain numpy (and scipy, xatlas). Everything it reads and writes is derived from the player's map: local only.
"""
import os
import struct

import numpy as np

MAGIC = b"PWG1"
FLAG_FAR, FLAG_GROUPS, FLAG_DEVIATION, FLAG_ATLAS = 1, 2, 4, 8
NO_CHART = 0xFFFFFFFF
WELD = 1e-4
# a remodel vertex this near a stock corner is at it (the engine's SEAL_CORNER)
CORNER = 1e-3
# the engine's bounds (prism_world_geometry.c: SHAPE_RELIEF, SHAPE_MARGIN) and the build's, inside them
SHAPE_RELIEF = 1.0
SHAPE_MARGIN = 1.0
BUILD_RELIEF = 0.9
BUILD_MARGIN = 0.9
NEEDLE_FLOOR = 0.1
NEEDLE_RATIO = 1.0
ATLAS_DENSITY = 4.0
ATLAS_PAGE = 4096
ATLAS_PADDING = 2
UV_SCALE = 8.0


def pad4(n):
    return (n + 3) & ~3


# ---------------------------------------------------------------- the file


def read(path):
    """a .pwg of version 1, 2 or 3: dict(header (its first 64 bytes), version, far, parts, ext (version 3's extension,
    or None), charts (each chart's page, version 3)); each part as wg2_seal.read_pwg's, with version 3's "group",
    "dev", "atlas_chart" and "atlas_uv" (None before)"""
    b = open(path, "rb").read()
    if b[:4] != MAGIC:
        raise ValueError("%s is not a .pwg" % path)
    ver, count, flags = struct.unpack_from("<III", b, 4)
    if not 1 <= ver <= 3:
        raise ValueError("%s is version %d" % (path, ver))
    far = ver >= 2 and bool(flags & FLAG_FAR)
    off, parts, ext = 64, [], None
    if ver >= 3:
        g, pages, pw, ph, density, charts = struct.unpack_from("<IIIIfI", b, 64)
        ext = dict(groups=g, pages=pages, page_width=pw, page_height=ph, density=density, charts=charts,
                   flags=flags)
        off = 128
    for _ in range(count):
        rec = b[off:off + 64]
        vc, tc = struct.unpack_from("<II", rec, 12)
        pfs, psc = struct.unpack_from("<II", rec, 32)
        ntc, dist = struct.unpack_from("<If", rec, 40) if far else (tc, 0.0)
        off += 64
        p = dict(rec=rec, lm=struct.unpack_from("<h", rec, 0)[0], mi=struct.unpack_from("<h", rec, 2)[0],
                 pfs=pfs, psc=psc, ntc=ntc, dist=dist, group=None, dev=None, atlas_chart=None, atlas_uv=None)
        p["sft"] = np.frombuffer(b, "<u4", psc + 1, off).astype(np.int64)
        off += pad4((psc + 1) * 4)
        p["verts"] = np.frombuffer(b, "<f4", vc * 14, off).reshape(vc, 14).copy()
        off += pad4(vc * 56)
        p["lmv"] = np.frombuffer(b, "<f4", vc * 5, off).reshape(vc, 5).copy()
        off += pad4(vc * 20)
        p["tri"] = np.frombuffer(b, "<u2", tc * 3, off).reshape(tc, 3).astype(np.int64)
        off += pad4(tc * 6)
        if far:
            p["pairs"] = np.frombuffer(b, "<u4", psc * 2, off).reshape(psc, 2).astype(np.int64)
            off += psc * 8
            p["bounds"] = np.frombuffer(b, "<f4", psc * 4, off).reshape(psc, 4).copy()
            off += psc * 16
        else:
            p["pairs"] = np.stack([p["sft"][:-1], p["sft"][1:] - p["sft"][:-1]], 1)
            p["bounds"] = None
        if ver >= 3:
            p["group"] = np.frombuffer(b, "<u4", psc, off).astype(np.int64)
            off += psc * 4
            p["dev"] = np.frombuffer(b, "<f4", psc * 4, off).reshape(psc, 4).copy()
            off += psc * 16
            raw = np.frombuffer(b, np.uint8, ntc * 16, off).reshape(ntc, 16)
            p["atlas_chart"] = raw[:, 0:4].copy().view("<u4").ravel().astype(np.int64)
            p["atlas_uv"] = raw[:, 4:16].copy().view("<u2").reshape(ntc, 2, 3).transpose(0, 2, 1).astype(np.int64)
            off += ntc * 16
        parts.append(p)
    charts = None
    if ver >= 3:
        charts = np.frombuffer(b, "<u4", ext["charts"], off).astype(np.int64)
        off += ext["charts"] * 4
    if off != len(b):
        raise ValueError("%s: %d bytes past its records" % (path, len(b) - off))
    return dict(header=b[:64], version=ver, far=far, parts=parts, ext=ext, charts=charts)


def write_v3(path, header, parts, ext, charts):
    """version 3 (the module's comment): every part with its far level (pairs, bounds), "group", "dev",
    "atlas_chart" and "atlas_uv"; ext's groups, pages, page_width, page_height, density"""
    out = bytearray(header[:64])
    flags = FLAG_FAR | FLAG_GROUPS | FLAG_DEVIATION | FLAG_ATLAS
    struct.pack_into("<III", out, 4, 3, len(parts), flags)
    out += struct.pack("<IIIIfI", ext["groups"], ext["pages"], ext["page_width"], ext["page_height"],
                       float(ext["density"]), len(charts)) + b"\0" * 40

    def arr(x):
        bts = x.tobytes()
        out.extend(bts + b"\0" * (pad4(len(bts)) - len(bts)))
    for p in parts:
        rec = bytearray(p["rec"])
        assert len(p["verts"]) <= 0xFFFF
        struct.pack_into("<II", rec, 12, len(p["verts"]), len(p["tri"]))
        struct.pack_into("<II", rec, 32, p["pfs"], p["psc"])
        struct.pack_into("<If", rec, 40, p["ntc"], p["dist"])
        rec[48:64] = b"\0" * 16
        out += rec
        arr(np.asarray(p["sft"], "<u4"))
        arr(np.asarray(p["verts"], "<f4"))
        arr(np.asarray(p["lmv"], "<f4"))
        arr(np.asarray(p["tri"], "<u2"))
        out += np.asarray(p["pairs"], "<u4").tobytes()
        out += np.asarray(p["bounds"], "<f4").tobytes()
        out += np.asarray(p["group"], "<u4").tobytes()
        out += np.asarray(p["dev"], "<f4").tobytes()
        raw = np.zeros((p["ntc"], 16), np.uint8)
        raw[:, 0:4] = np.asarray(p["atlas_chart"], "<u4").reshape(-1, 1).view(np.uint8)
        raw[:, 4:16] = np.ascontiguousarray(np.asarray(p["atlas_uv"], "<u2").transpose(0, 2, 1)).reshape(-1, 6).view(
            np.uint8)
        out += raw.tobytes()
    out += np.asarray(charts, "<u4").tobytes()
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return len(out)


# ---------------------------------------------------------------- the level as the checks see it


class Level:
    """a file's parts with the BSP: every surface (global numbers, in the parts' order), its stock corners (in the
    BSP's order) and plane, every row's welded position"""

    def __init__(self, mats, parts):
        self.parts = parts
        by = {(m.lightmap_index, m.material_index): m for m in mats}
        self.by = by
        first = [0]
        corners, owner, local, stock_rows = [], [], [], []
        for pi, p in enumerate(parts):
            m = by.get((p["lm"], p["mi"]))
            if m is None:
                raise SystemExit("record lightmap %d material %d is not the BSP's" % (p["lm"], p["mi"]))
            # (the BSP's own order: load_bsp's, as the topology check needs)
            st = m.triangles[p["pfs"]:p["pfs"] + p["psc"]]
            corners.append(m.position[st].astype(np.float64))
            stock_rows.append(st)
            owner.append(np.full(p["psc"], pi))
            local.append(np.arange(p["psc"]))
            first.append(first[-1] + p["psc"])
        self.first = np.array(first)
        self.C = np.concatenate(corners) if corners else np.zeros((0, 3, 3))
        self.stock_rows = stock_rows
        self.owner = np.concatenate(owner) if owner else np.zeros(0, np.int64)
        self.local = np.concatenate(local) if local else np.zeros(0, np.int64)
        self.N = len(self.C)
        a, b, c = self.C[:, 0], self.C[:, 1], self.C[:, 2]
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n, axis=1)
        self.area_ok = ln > 1e-12
        # (outward, as the stock vertices' normals and the engine's front: minus the corners' cross product)
        self.n = -n / np.maximum(ln, 1e-30)[:, None]
        self.longest = np.max(np.stack([np.linalg.norm(self.C[:, (k + 1) % 3] - self.C[:, k], axis=1)
                                        for k in range(3)], 1), 1)
        self.inward = []
        for k in range(3):
            u, v = self.C[:, k], self.C[:, (k + 1) % 3]
            iw = np.cross(-self.n, v - u)
            il = np.linalg.norm(iw, axis=1)
            self.inward.append(iw / np.maximum(il, 1e-12)[:, None] * (il > 1e-12)[:, None])
        self.reweld()

    def reweld(self):
        offs = np.cumsum([0] + [len(p["verts"]) for p in self.parts])
        allP = np.concatenate([p["verts"][:, 0:3] for p in self.parts]).astype(np.float64)
        _, first, gid = np.unique(np.round(allP / WELD).astype(np.int64), axis=0, return_index=True,
                                  return_inverse=True)
        self.offs = offs
        self.gid = gid.ravel()
        self.gpos = allP[first]
        self.G = len(self.gpos)

    def tris(self, level):
        """a level's triangles: (corners' welded ids (t, 3), global surface (t), part (t), rows (t, 3)); level "near" or
        "far"; also the near triangles' index in their part"""
        F, S, PI, R, IDX = [], [], [], [], []
        for pi, p in enumerate(self.parts):
            if level == "near":
                first, cnt = p["sft"][:-1], np.diff(p["sft"])
            else:
                first, cnt = p["pairs"][:, 0], p["pairs"][:, 1]
            if not len(cnt) or not cnt.sum():
                continue
            idx = np.repeat(first - np.concatenate([[0], np.cumsum(cnt)[:-1]]), cnt) + np.arange(cnt.sum())
            rows = p["tri"][idx]
            F.append(self.gid[self.offs[pi] + rows])
            S.append(self.first[pi] + np.repeat(np.arange(p["psc"]), cnt))
            PI.append(np.full(len(idx), pi))
            R.append(rows)
            IDX.append(idx)
        if not F:
            z = np.zeros((0, 3), np.int64)
            return z, np.zeros(0, np.int64), np.zeros(0, np.int64), z, np.zeros(0, np.int64)
        return np.concatenate(F), np.concatenate(S), np.concatenate(PI), np.concatenate(R), np.concatenate(IDX)

    def far_differs(self):
        out = np.zeros(self.N, bool)
        for pi, p in enumerate(self.parts):
            same = (p["pairs"][:, 0] == p["sft"][:-1]) & (p["pairs"][:, 1] == np.diff(p["sft"]))
            out[self.first[pi]:self.first[pi + 1]] = ~same
        return out


def point_metrics(level, X, s):
    """for points X (n, 3) of surfaces s (n): height off the stock plane (outward), the most past an edge in the plane
    (0 inside), the distance to the stock triangle"""
    a = level.C[s, 0]
    h = ((X - a) * level.n[s]).sum(1)
    out = np.zeros(len(X))
    for k in range(3):
        out = np.maximum(out, -((X - level.C[s, k]) * level.inward[k][s]).sum(1))
    return h, out, tri_distance(X, level.C[s, 0], level.C[s, 1], level.C[s, 2])[0]


def tri_distance(P, A, B, C):
    """each point's distance to its triangle and the closest point (Ericson's regions)"""
    ab, ac, ap = B - A, C - A, P - A
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = P - B
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = P - C
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2

    def safe(x):
        return np.where(np.abs(x) < 1e-30, 1e-30, x)
    den = safe(va + vb + vc)
    Q = A + ab * (vb / den)[:, None] + ac * (vc / den)[:, None]
    m = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0)
    t = (d4 - d3) / safe((d4 - d3) + (d5 - d6))
    Q[m] = (B + (C - B) * t[:, None])[m]
    m = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
    t = d2 / safe(d2 - d6)
    Q[m] = (A + ac * t[:, None])[m]
    m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
    t = d1 / safe(d1 - d3)
    Q[m] = (A + ab * t[:, None])[m]
    m = (d6 >= 0) & (d5 <= d6)
    Q[m] = C[m]
    m = (d3 >= 0) & (d4 <= d3)
    Q[m] = B[m]
    m = (d1 <= 0) & (d2 <= 0)
    Q[m] = A[m]
    return np.linalg.norm(P - Q, axis=1), Q


def surface_vertices(level):
    """the (surface, welded vertex) pairs of both levels' triangles"""
    pairs = []
    for lv in ("near", "far"):
        F, S, _, _, _ = level.tris(lv)
        pairs.append(np.stack([np.repeat(S, 3), F.ravel()], 1))
    sv = np.unique(np.concatenate(pairs), axis=0)
    return sv[:, 0], sv[:, 1]


def needle_bound(level, s):
    return np.maximum(NEEDLE_FLOOR, NEEDLE_RATIO * level.longest[s])


def check_shape(level, relief=BUILD_RELIEF, margin=BUILD_MARGIN):
    """the remodel's vertices outside their surfaces' bounds: dict(needle, cap: (surface, welded vertex) pairs, and
    the surfaces' worst figures)"""
    s, g = surface_vertices(level)
    keep = level.area_ok[s]
    s, g = s[keep], g[keep]
    h, out, dist = point_metrics(level, level.gpos[g], s)
    needle = dist > needle_bound(level, s)
    cap = ~needle & ((np.abs(h) > relief) | (out > margin))
    return dict(needle=(s[needle], g[needle]), cap=(s[cap], g[cap]), dist=dist, h=h, out=out, s=s, g=g)


def _inside(level, X, s, relief, margin):
    h, out, _ = point_metrics(level, X, s)
    return (np.abs(h) <= relief) & (out <= margin)


def fix_shape(level, mats, relief=BUILD_RELIEF, margin=BUILD_MARGIN, rounds=8, report=None):
    """the vertices check_shape finds moved back (the module's comment), every row at each position alike, their
    attributes taken from the stock triangle; returns the welded positions moved"""
    moved_all = set()
    for _ in range(rounds):
        r = check_shape(level, relief, margin)
        sn, gn = r["needle"]
        sc, gc = r["cap"]
        if not len(sn) and not len(sc):
            break
        target = {}
        # needles onto the stock triangle
        if len(sn):
            _, Q = tri_distance(level.gpos[gn], level.C[sn, 0], level.C[sn, 1], level.C[sn, 2])
            for s_, g_, q in zip(sn, gn, Q):
                target.setdefault(int(g_), (int(s_), q))
        # the rest just inside the bounds: along the way to the stock triangle, the first point inside (bisection)
        if len(sc):
            X = level.gpos[gc]
            _, Q = tri_distance(X, level.C[sc, 0], level.C[sc, 1], level.C[sc, 2])
            lo, hi = np.zeros(len(X)), np.ones(len(X))
            for _ in range(30):
                mid = 0.5 * (lo + hi)
                ok = _inside(level, X + (Q - X) * mid[:, None], sc, relief * 0.995, margin * 0.995)
                hi = np.where(ok, mid, hi)
                lo = np.where(ok, lo, mid)
            Y = X + (Q - X) * hi[:, None]
            for s_, g_, y in zip(sc, gc, Y):
                if int(g_) not in target:
                    target[int(g_)] = (int(s_), y)
        for g_, (s_, y) in target.items():
            _move(level, mats, g_, s_, y)
            moved_all.add(g_)
        level.reweld()
    if report is not None:
        r = check_shape(level, relief, margin)
        report["positions_moved"] = len(moved_all)
        report["needles_left"] = int(len(r["needle"][0]))
        report["caps_left"] = int(len(r["cap"][0]))
    return moved_all


def _barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-30:
        return np.array([1.0, 0.0, 0.0])
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    bc = np.clip(np.array([1.0 - v - w, v, w]), 0.0, 1.0)
    return bc / max(bc.sum(), 1e-12)


def _move(level, mats, g, s, y):
    """every row at welded position g put at y; each row's attributes from its own surface's stock triangle there"""
    rows = np.nonzero(level.gid == g)[0]
    for r in rows:
        pi = int(np.searchsorted(level.offs, r, side="right") - 1)
        row = int(r - level.offs[pi])
        p = level.parts[pi]
        # (the row's surface: the first of its part's surfaces whose triangles use it, near or far; else s's)
        use = np.nonzero((p["tri"] == row).any(1))[0]
        sl = None
        if len(use):
            near = use[use < p["ntc"]]
            t = int(near[0]) if len(near) else int(use[0])
            if t < p["ntc"]:
                sl = int(np.searchsorted(p["sft"], t, side="right") - 1)
            else:
                hit = np.nonzero((p["pairs"][:, 0] <= t) & (t < p["pairs"][:, 0] + p["pairs"][:, 1]))[0]
                sl = int(hit[0]) if len(hit) else None
        gs = level.first[pi] + sl if sl is not None else s
        m = level.by[(p["lm"], p["mi"])] if level.owner[gs] == pi else level.by[(level.parts[level.owner[gs]]["lm"],
                                                                                  level.parts[level.owner[gs]]["mi"])]
        st = level.stock_rows[level.owner[gs]][level.local[gs]]
        bc = _barycentric(y, *(m.position[st].astype(np.float64)))

        def lerp(a):
            return (bc[:, None] * a[st].astype(np.float64)).sum(0)
        v = p["verts"][row].astype(np.float64)
        v[0:3] = y
        for a_, b_, arr in ((3, 6, m.normal), (6, 9, m.binormal), (9, 12, m.tangent)):
            x = lerp(arr)
            n = np.linalg.norm(x)
            v[a_:b_] = x / n if n > 1e-9 else v[a_:b_]
        v[12:14] = lerp(m.uv0)
        p["verts"][row] = v.astype(np.float32)
        lm = p["lmv"][row].astype(np.float64)
        lm[0:3] = lerp(m.radiosity)
        lm[3:5] = lerp(m.uv1)
        p["lmv"][row] = lm.astype(np.float32)


# ---------------------------------------------------------------- seal groups


def boundary_keys(level, F, S):
    """each surface's boundary edges at a level (used once by its own triangles), as sorted keys
    (surface * G + low) * G + high"""
    G = level.G
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
    ES = np.concatenate([S, S, S])
    lo, hi = np.minimum(E[:, 0], E[:, 1]), np.maximum(E[:, 0], E[:, 1])
    good = lo != hi
    key = (ES[good] * G + lo[good]) * G + hi[good]
    u, c = np.unique(key, return_counts=True)
    return u[c == 1]


def corner_ids(level):
    """each surface's stock corners' welded ids (N, 3): the remodel's vertex within CORNER of the corner, or -1"""
    from scipy.spatial import cKDTree
    tree = cKDTree(level.gpos)
    d, i = tree.query(level.C.reshape(-1, 3), k=1, distance_upper_bound=CORNER)
    i = np.where(np.isfinite(d), i, -1)
    return i.reshape(-1, 3)


def plain_sides(level):
    """(N, 3): whether each surface's side along each stock edge is that edge at both levels (one boundary edge of
    its triangles from the corner's vertex to the next corner's)"""
    cid = corner_ids(level)
    G = level.G
    plain = np.ones((level.N, 3), bool)
    for lv in ("near", "far"):
        F, S, _, _, _ = level.tris(lv)
        keys = boundary_keys(level, F, S)
        for k in range(3):
            a, b = cid[:, k], cid[:, (k + 1) % 3]
            ok = (a >= 0) & (b >= 0) & (a != b)
            lo, hi = np.minimum(a, b), np.maximum(a, b)
            q = (np.arange(level.N) * G + lo) * G + hi
            j = np.clip(np.searchsorted(keys, q), 0, max(len(keys) - 1, 0))
            found = (keys[j] == q) if len(keys) else np.zeros(level.N, bool)
            plain[:, k] &= ok & found
    return plain


def stock_edges(level):
    """each stock edge's sides: (key per side (N*3), sorted order) with the corner positions rounded to 1e-3"""
    a = np.round(level.C / 1e-3).astype(np.int64)
    b = np.roll(a, -1, axis=1)
    first = _first_less(a, b)[..., None]
    lo = np.where(first, a, b)
    hi = np.where(first, b, a)
    k6 = np.concatenate([lo, hi], -1).reshape(-1, 6)
    _, edge = np.unique(k6, axis=0, return_inverse=True)
    return edge.ravel()


def _first_less(a, b):
    """a < b lexicographically, on the last axis"""
    out = np.zeros(a.shape[:-1], bool)
    decided = np.zeros(a.shape[:-1], bool)
    for k in range(a.shape[-1]):
        lt = (a[..., k] < b[..., k]) & ~decided
        gt = (a[..., k] > b[..., k]) & ~decided
        out |= lt
        decided |= lt | gt
    return out


def seal_groups(level, plain=None):
    """each surface's group (N) and the groups' count: surfaces joined across every stock edge where any side is
    not plain (plain_sides)"""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    if plain is None:
        plain = plain_sides(level)
    edge = stock_edges(level)
    side_plain = plain.ravel()
    surf = np.repeat(np.arange(level.N), 3)
    order = np.argsort(edge, kind="stable")
    e_sorted, s_sorted, p_sorted = edge[order], surf[order], side_plain[order]
    # each run of one edge's sides: joined (consecutive sides) where any side is not plain
    start = np.r_[True, e_sorted[1:] != e_sorted[:-1]]
    run = np.cumsum(start) - 1
    run_plain = np.ones(run.max() + 1 if len(run) else 0, bool)
    np.logical_and.at(run_plain, run, p_sorted)
    same = ~start[1:]
    join = same & ~run_plain[run[1:]]
    a, b = s_sorted[:-1][join], s_sorted[1:][join]
    graph = coo_matrix((np.ones(len(a)), (a, b)), shape=(level.N, level.N))
    count, labels = connected_components(graph, directed=False)
    return labels, count


# ---------------------------------------------------------------- deviations


def deviations(level):
    """(N, 4): the most above and below the stock plane, past its edges, and the near level's vertices from the far
    level's surface"""
    dev = np.zeros((level.N, 4))
    s, g = surface_vertices(level)
    h, out, _ = point_metrics(level, level.gpos[g], s)
    np.maximum.at(dev[:, 0], s, np.maximum(h, 0.0))
    np.maximum.at(dev[:, 1], s, np.maximum(-h, 0.0))
    np.maximum.at(dev[:, 2], s, out)
    differs = level.far_differs()
    if differs.any():
        dev[:, 3] = far_deviation(level, differs)
    return dev


def far_deviation(level, differs):
    """each surface's near vertices' greatest distance along the stock plane's normal from its far level's triangles
    over them (the far triangles near each vertex, by their middles; a vertex no far triangle covers seen in the
    plane counts its height's spread)"""
    from scipy.spatial import cKDTree
    out = np.zeros(level.N)
    Fn, Sn, _, _, _ = level.tris("near")
    Ff, Sf, _, _, _ = level.tris("far")
    keep = differs[Sf]
    Ff, Sf = Ff[keep], Sf[keep]
    if not len(Ff):
        return out
    pv = np.unique(np.stack([np.repeat(Sn, 3), Fn.ravel()], 1), axis=0)
    pv = pv[differs[pv[:, 0]]]
    s, g = pv[:, 0], pv[:, 1]
    X = level.gpos[g]
    cen = level.gpos[Ff].mean(1)
    tree = cKDTree(cen)
    k = min(24, len(cen))
    _, idx = tree.query(X, k=k)
    idx = idx.reshape(len(X), k)
    best = np.full(len(X), np.inf)
    n = level.n[s]
    a0 = level.C[s, 0]
    for j in range(k):
        t = idx[:, j]
        same = Sf[t] == s
        A, B, C = (level.gpos[Ff[t, c]] for c in range(3))
        # (in the stock plane: the vertex's foot within the far triangle; the gap along the normal)
        e1 = level.C[s, 1] - a0
        e1 /= np.maximum(np.linalg.norm(e1, axis=1, keepdims=True), 1e-12)
        e2 = np.cross(n, e1)

        def uv(P):
            d = P - a0
            return np.stack([(d * e1).sum(1), (d * e2).sum(1)], 1)
        pa, pb, pc, px = uv(A), uv(B), uv(C), uv(X)
        v0, v1, v2 = pb - pa, pc - pa, px - pa
        den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]
        den = np.where(np.abs(den) < 1e-18, 1e-18, den)
        w1 = (v2[:, 0] * v1[:, 1] - v1[:, 0] * v2[:, 1]) / den
        w2 = (v0[:, 0] * v2[:, 1] - v2[:, 0] * v0[:, 1]) / den
        w0 = 1 - w1 - w2
        inside = same & (np.minimum(np.minimum(w0, w1), w2) > -1e-6)
        hf = w0 * ((A - a0) * n).sum(1) + w1 * ((B - a0) * n).sum(1) + w2 * ((C - a0) * n).sum(1)
        d = np.abs(((X - a0) * n).sum(1) - hf)
        best = np.where(inside & (d < best), d, best)
    h = ((X - a0) * n).sum(1)
    spread = np.zeros(level.N)
    hmax = np.full(level.N, -np.inf)
    hmin = np.full(level.N, np.inf)
    np.maximum.at(hmax, s, h)
    np.minimum.at(hmin, s, h)
    spread = np.where(np.isfinite(hmax), hmax - hmin, 0.0)
    best = np.where(np.isfinite(best), best, spread[s])
    np.maximum.at(out, s, best)
    return out


# ---------------------------------------------------------------- the atlas


def need_xatlas():
    """xatlas there and new enough (0.0.11: 0.0.9 has no Atlas.get_mesh_vertex_assignment nor get_mesh_chart_count,
    which atlas() reads each chart's vertices back with), else exit saying so (prism_packs.py's check asks the same)"""
    try:
        import xatlas
    except ImportError:
        raise SystemExit("no xatlas (pip install xatlas)")
    if not all(hasattr(xatlas.Atlas, m) for m in ("get_mesh_vertex_assignment", "get_mesh_chart_count")):
        raise SystemExit("xatlas is older than 0.0.11, which the lightmap atlas needs (pip install -U xatlas)")


def atlas(level, density=ATLAS_DENSITY, page=ATLAS_PAGE, padding=ATLAS_PADDING):
    """xatlas over each part's near triangles, packed together: per part (chart (ntc), uv (ntc, 3, 2) in eighths of
    a texel), each chart's page, and the pages' count and size"""
    import xatlas
    at = xatlas.Atlas()
    for p in level.parts:
        P = p["verts"][:, 0:3].astype(np.float32)
        T = p["tri"][:p["ntc"]].astype(np.uint32)
        at.add_mesh(P, T)
    co = xatlas.ChartOptions()
    po = xatlas.PackOptions()
    po.texels_per_unit = float(density)
    po.resolution = int(page)
    po.padding = int(padding)
    po.bilinear = True
    at.generate(co, po)
    W, H = at.width, at.height
    out, charts = [], []
    base = 0
    for i, p in enumerate(level.parts):
        vm, idx, uv = at[i]
        if len(idx) != p["ntc"]:
            raise SystemExit("xatlas gave %d triangles for %d" % (len(idx), p["ntc"]))
        page_of, chart_of = at.get_mesh_vertex_assignment(i)
        page_of = page_of.astype(np.int64)
        chart_of = chart_of.astype(np.int64)
        n_ch = at.get_mesh_chart_count(i)
        cpage = np.zeros(n_ch, np.int64)
        valid = (chart_of >= 0) & (chart_of < n_ch) & (page_of >= 0)
        cpage[chart_of[valid]] = page_of[valid]
        tc = chart_of[idx]
        tri_chart = np.where((tc[:, 0] == tc[:, 1]) & (tc[:, 1] == tc[:, 2]) & valid[idx].all(1), tc[:, 0], -1)
        texel = uv[idx].astype(np.float64) * np.array([W, H], np.float64)
        q = np.clip(np.round(texel * UV_SCALE), 0, 65535).astype(np.int64)
        out.append((np.where(tri_chart >= 0, tri_chart + base, NO_CHART), q))
        charts.append(cpage)
        base += n_ch
    charts = np.concatenate(charts) if charts else np.zeros(0, np.int64)
    return out, charts, dict(pages=max(1, int(at.atlas_count)), page_width=int(W), page_height=int(H),
                             density=float(density), utilization=float(np.mean(at.utilization)) if at.atlas_count else 0)
