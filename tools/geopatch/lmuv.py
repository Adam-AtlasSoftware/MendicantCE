"""The enhanced world's own lightmap layout (PLAN_FINAL.md F7, decision A4; geopatch M4): LMUV v2.

LMUV v1 (M2's stand-in) carried each remodel vertex's CE lightmap coordinates, so the enhanced bake was laid out
through CE's charts: derived retail data, and no room for an overhang, a cliff's underside or a trunk, which CE's
charts never had. v2 is our own layout, made here on the farm from the remodel's own triangles; nothing of it comes
from the retail map, and the decoder (port/linux/game/prism_geom.c) needs no stock lightmap vertex.

The layout, per BSP:
- Every triangle (near and far) has a chart. A near triangle's facing (its face normal) picks the nearest of 26
  directions (the cube's faces, edges and corners, at most about 35 degrees from any facing); the near triangles
  of a part that share that direction and a cell of CELL world units in its plane and touch (a shared vertex) are
  one chart, laid flat by projecting along the direction: a stretch of at most 1 / cos(35 degrees) = 1.22, and no
  fold (every face of a chart looks within 35 degrees of its axis). A far triangle (a coarser level of the same
  surface) takes the chart of the nearest near triangle of its own surface that faces its way, projected the same
  way, so the near and far levels of a surface read the same texels and switching level changes no light.
- A vertex used by triangles of several charts is copied, one copy per chart (a seam: the same position, its own
  coordinates), so a part's vertices grow; a part past 65535 vertices (the 16-bit indices) is cut into parts by its
  surfaces (a surface's triangles stay in one part, its far range with it).
- The charts are packed into pages of PAGE x PAGE texels at DENSITY texels per world unit, GUTTER texels apart,
  each material's charts on one page (the renderer draws a material's parts with one page: prism_deferred.c's page
  override); a material too large for a page at DENSITY is packed at a lower density (the bake's atlas scales each
  chart to its quality's density by its own measure, prism_bake_atlas.h, so a page's density only sets how fine
  the charts' gutters are). The bake's atlas joins triangles into charts by shared lightmap coordinates; the
  gutters keep ours apart.
- With DENSITY 1 and PAGE 1024 a chart's scale to MAX's tier 0 (16 texels a world unit) is 16 and the virtual page
  16384 texels, the atlas's largest (maximum_dimension).

LMUV v2 (little endian), one per BSP beside its GEOM:
    "LMU1", u32 version (2), u32 part count (GEOM's), u32 column count, u32 page count
    per page: u32 width, u32 height (texels)
    per part: u32 vertex count (its GEOM part's), u32 page (0 .. page count - 1)
    column table and columns as GEOM's:
        LUV2 u32  per vertex: lightmap u (low 16 bits) and v (high 16 bits), 0 to 1 across its part's page, in the
                  game's compressed form (compress_real_to_int16_clamp: floor(x * 32767.5))
The incident radiosity a lightmap vertex also carries (CE's lightmap pass's bump term) is not stored: the decoder
gives each vertex its own normal (Enhanced and Path Traced light from the bake, not from CE's lightmap pass).
"""
import math
import struct

import numpy as np

LMUV_MAGIC = b"LMU1"
LMUV_VERSION = 2
PAGE = 1024
DENSITY = 1.0
GUTTER = 2
CELL = 32.0
FACING_LEAST = 0.3
MOST_VERTICES = 0xFFFF
DENSITY_LEAST = 1.0 / 64.0


class LayoutError(Exception):
    pass


def directions():
    d = []
    for x in (-1, 0, 1):
        for y in (-1, 0, 1):
            for z in (-1, 0, 1):
                if x or y or z:
                    d.append((x, y, z))
    d = np.array(d, np.float64)
    return d / np.linalg.norm(d, axis=1)[:, None]


DIRECTIONS = directions()


def bases(dirs):
    t = np.where(np.abs(dirs[:, 2:3]) < 0.95, np.cross(dirs, [0.0, 0.0, 1.0]), np.cross(dirs, [1.0, 0.0, 0.0]))
    t /= np.linalg.norm(t, axis=1)[:, None]
    b = np.cross(dirs, t)
    return t, b


TANGENTS, BITANGENTS = bases(DIRECTIONS)


def pack_coordinates(c):
    """encode.py's: compress_real_to_int16_clamp's rounding, as unsigned 16-bit fields"""
    c = np.clip(np.asarray(c, np.float32), np.float32(-1.0), np.float32(1.0))
    return (np.floor(c * np.float32(32767.5)).astype(np.int64) & 0xFFFF).astype(np.uint32)


def decode_coordinates(u16):
    """the game's decode of a compressed coordinate: (2 s + 1) / 65535"""
    s = np.asarray(u16, np.int64)
    s = np.where(s >= 0x8000, s - 0x10000, s)
    return (2.0 * s + 1.0) / 65535.0


def _components(count, a, b):
    """connected components of count nodes joined by the edges (a[i], b[i]): a label each (the smallest node of its
    component)"""
    labels = np.arange(count, dtype=np.int64)
    if len(a) == 0:
        return labels
    a = np.asarray(a, np.int64)
    b = np.asarray(b, np.int64)
    while True:
        la, lb = labels[a], labels[b]
        low = np.minimum(la, lb)
        before = labels.copy()
        np.minimum.at(labels, a, low)
        np.minimum.at(labels, b, low)
        # (pointer jumping)
        while True:
            jumped = labels[labels]
            if np.array_equal(jumped, labels):
                break
            labels = jumped
        if np.array_equal(before, labels):
            return labels


def _face_normals(positions, triangles):
    p0, p1, p2 = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
    n = np.cross(p1 - p0, p2 - p0)
    length = np.linalg.norm(n, axis=1)
    out = np.zeros_like(n)
    ok = length > 1e-12
    out[ok] = n[ok] / length[ok, None]
    out[~ok] = (0.0, 0.0, 1.0)
    return out, 0.5 * length


def part_charts(part):
    """the charts of one part's triangles: (chart per triangle (int, from 0), the chart's direction index each)"""
    positions = part.vertices[:, 0:3].astype(np.float64)
    triangles = part.triangles
    count = len(triangles)
    if count == 0:
        return np.zeros(0, np.int64), np.zeros(0, np.int64)
    normals, area = _face_normals(positions, triangles)
    near_count = int(part.near_triangle_count)
    # (a triangle faces the way its stock surface's remodel faces on the whole (its near triangles' area-weighted
    # normal), so the relief's small facets stay with their surface's chart; one turned past FACING_LEAST from that
    # way takes its own)
    facing = np.argmax(normals @ DIRECTIONS.T, axis=1)
    sft = part.sft.astype(np.int64)
    if near_count and len(sft) > 1:
        surface = np.repeat(np.arange(len(sft) - 1), np.diff(sft))[:near_count]
        mean = np.zeros((len(sft) - 1, 3))
        np.add.at(mean, surface, normals[:len(surface)] * area[:len(surface), None])
        way = np.argmax(mean @ DIRECTIONS.T, axis=1)
        chosen = way[surface]
        keep = np.einsum("ij,ij->i", normals[:len(surface)], DIRECTIONS[chosen]) >= FACING_LEAST
        facing[:len(surface)] = np.where(keep, chosen, facing[:len(surface)])
    centroids = positions[triangles].mean(axis=1)
    u = np.einsum("ij,ij->i", centroids, TANGENTS[facing])
    v = np.einsum("ij,ij->i", centroids, BITANGENTS[facing])
    cu = np.floor(u / CELL).astype(np.int64)
    cv = np.floor(v / CELL).astype(np.int64)
    near = np.arange(count) < near_count
    group = np.full(count, -1, np.int64)
    keys = np.stack([facing, cu, cv], axis=1)[near]
    if len(keys):
        _, group_near = np.unique(keys, axis=0, return_inverse=True)
        group[near] = group_near.reshape(-1)
    # (near triangles of one group sharing a corner's position (the remodel's vertices are split along CE's
    # lightmap seams and its surfaces, so its indices alone leave a surface in pieces): one chart)
    near_index = np.nonzero(near)[0]
    corners_t = np.repeat(near_index, 3)
    welded = np.unique(np.rint(positions * 1024.0).astype(np.int64), axis=0, return_inverse=True)[1].reshape(-1)
    corners_v = welded[triangles[near].reshape(-1)]
    order = np.lexsort((group[corners_t], corners_v))
    sv, st = corners_v[order], corners_t[order]
    same = (sv[1:] == sv[:-1]) & (group[st[1:]] == group[st[:-1]])
    labels = _components(count, st[1:][same], st[:-1][same])
    chart = np.full(count, -1, np.int64)
    if near_count:
        _, chart_near = np.unique(labels[near], return_inverse=True)
        chart[near] = chart_near.reshape(-1)
    charts = int(chart[near].max()) + 1 if near_count else 0
    chart_facing = np.zeros(charts, np.int64)
    if near_count:
        chart_facing[chart[near]] = facing[near]
    # (a far triangle: the chart of its surface's nearest near triangle that faces its way, else the nearest)
    far_index = np.nonzero(~near)[0]
    if len(far_index):
        surface_of = np.full(count, -1, np.int64)
        if part.far_pairs is not None:
            for s, (first, n) in enumerate(part.far_pairs.tolist()):
                if n:
                    surface_of[first:first + n] = np.where(surface_of[first:first + n] < 0, s,
                                                           surface_of[first:first + n])
        sft = part.sft.astype(np.int64)
        extra = []
        for s in np.unique(surface_of[far_index]):
            members = far_index[surface_of[far_index] == s]
            near_members = np.arange(sft[s], sft[s + 1]) if s >= 0 else np.zeros(0, np.int64)
            if len(near_members) == 0:
                extra.append(members)
                continue
            for k in range(0, len(members), 256):
                block = members[k:k + 256]
                d = np.linalg.norm(centroids[block][:, None, :] - centroids[near_members][None, :, :], axis=2)
                mismatch = facing[block][:, None] != facing[near_members][None, :]
                pick = np.argmin(d + mismatch * 1.0e6, axis=1)
                chart[block] = chart[near_members[pick]]
        for members in extra:
            # (a far run with no near triangle of its own surface: charts of its own, by facing)
            for f in np.unique(facing[members]):
                chart[members[facing[members] == f]] = charts
                chart_facing = np.append(chart_facing, f)
                charts += 1
    if (chart < 0).any():
        raise LayoutError("a triangle with no chart")
    return chart, chart_facing


class Laid:
    """a part with its own lightmap layout: the .pwg part's fields, its vertices copied per chart, its page and each
    vertex's packed coordinates (luv)"""


def _copy_part(part, chart):
    """the part's vertices, one per (vertex, chart) of the triangles that use them: (vertex source indices,
    triangles over the copies, each copy's chart). Vertices equal in all their drawn attributes (position, vectors,
    base map coordinates: the remodel split them only along CE's lightmap seams, which v2 has not) are one vertex
    within a chart"""
    rows = np.ascontiguousarray(part.vertices.view(np.uint32).reshape(len(part.vertices), -1))
    _, first, same = np.unique(rows.view(np.dtype((np.void, rows.shape[1] * 4))).reshape(-1), return_index=True,
                               return_inverse=True)
    canonical = first[same.reshape(-1)]
    corners = canonical[part.triangles.reshape(-1)]
    corner_chart = np.repeat(chart, 3)
    width = int(chart.max()) + 1 if len(chart) else 1
    pairs = corners.astype(np.int64) * width + corner_chart
    unique, inverse = np.unique(pairs, return_inverse=True)
    return unique // width, inverse.reshape(-1, 3), unique % width


def _split(part, source, triangles, copy_chart):
    """the part cut by its surfaces into pieces of at most MOST_VERTICES copies: [(first surface offset, surface
    count, triangle indices (old), far pairs (new), spheres)]"""
    if len(source) <= MOST_VERTICES:
        return None
    sft = part.sft.astype(np.int64)
    surfaces = len(sft) - 1
    near_count = int(part.near_triangle_count)
    far_pairs = part.far_pairs.astype(np.int64) if part.far_pairs is not None else None
    pieces, start = [], 0
    while start < surfaces:
        used = set()
        end = start
        while end < surfaces:
            tri = list(range(sft[end], sft[end + 1]))
            if far_pairs is not None:
                first, n = far_pairs[end]
                tri += [t for t in range(first, first + n) if t >= near_count]
            more = set(triangles[tri].reshape(-1).tolist()) - used
            if end > start and len(used) + len(more) > MOST_VERTICES:
                break
            used |= more
            end += 1
        if len(used) > MOST_VERTICES:
            raise LayoutError("one surface past %d vertices" % MOST_VERTICES)
        pieces.append((start, end))
        start = end
    return pieces


def _piece(part, start, end, source, triangles, copy_chart):
    """a Laid of the part's surfaces [start, end): its own near then far triangles, its copies renumbered"""
    sft = part.sft.astype(np.int64)
    near_count = int(part.near_triangle_count)
    far_pairs = part.far_pairs.astype(np.int64) if part.far_pairs is not None else None
    near = np.arange(sft[start], sft[end])
    far_old = []
    if far_pairs is not None:
        for s in range(start, end):
            first, n = far_pairs[s]
            if n and first >= near_count:
                far_old.append(np.arange(first, first + n))
    far = np.unique(np.concatenate(far_old)) if far_old else np.zeros(0, np.int64)
    old = np.concatenate([near, far])
    new_of = {int(t): i for i, t in enumerate(old.tolist())}
    p = Laid()
    for name in ("lightmap_index", "material_index", "stock_surface_count", "stock_vertex_count", "topology_check",
                 "position_check", "far_distance"):
        setattr(p, name, getattr(part, name, 0))
    p.first_surface = int(part.first_surface) + start
    p.surface_count = end - start
    p.near_triangle_count = len(near)
    p.sft = (sft[start:end + 1] - sft[start]).astype(np.uint32)
    if far_pairs is not None:
        pairs = []
        for s in range(start, end):
            first, n = (int(x) for x in far_pairs[s])
            if n == 0:
                pairs.append((0, 0))
                continue
            mapped = [new_of[t] for t in range(first, first + n)]
            if mapped != list(range(mapped[0], mapped[0] + n)):
                raise LayoutError("a far range is not one run after the cut")
            pairs.append((mapped[0], n))
        p.far_pairs = np.array(pairs, np.uint32).reshape(-1, 2)
        p.spheres = part.spheres[start:end].copy()
    else:
        p.far_pairs = p.spheres = None
    tri = triangles[old]
    used, inverse = np.unique(tri.reshape(-1), return_inverse=True)
    p.copies = used
    p.triangles = inverse.reshape(-1, 3).astype(np.int64)
    return p


def layout(parts, density=DENSITY, page=PAGE, gutter=GUTTER):
    """a BSP's .pwg parts laid out: ([Laid], [(width, height)] pages, report)"""
    laid = []
    for part in parts:
        chart, chart_facing = part_charts(part)
        if len(chart) == 0:
            continue
        source, triangles, copy_chart = _copy_part(part, chart)
        positions = part.vertices[source, 0:3].astype(np.float64)
        f = chart_facing[copy_chart]
        uw = np.einsum("ij,ij->i", positions, TANGENTS[f])
        vw = np.einsum("ij,ij->i", positions, BITANGENTS[f])
        pieces = _split(part, source, triangles, copy_chart)
        if pieces is None:
            pieces_laid = [_piece(part, 0, len(part.sft) - 1, source, triangles, copy_chart)]
        else:
            pieces_laid = [_piece(part, a, b, source, triangles, copy_chart) for a, b in pieces]
        for p in pieces_laid:
            copies = p.copies
            p.vertices = part.vertices[source[copies]].copy()
            p.lightmap = np.zeros((len(copies), 5), np.float32)
            p.chart_local = copy_chart[copies]
            p.chart_facing = chart_facing
            p.uw, p.vw = uw[copies], vw[copies]
            laid.append(p)
    # (the charts: by material, each chart's box in world units on its plane)
    materials = {}
    for index, p in enumerate(laid):
        materials.setdefault((p.lightmap_index, p.material_index), []).append(index)
    charts = []  # (material key, part index, local chart, umin, vmin, w, h)
    for key, members in materials.items():
        for index in members:
            p = laid[index]
            n = int(p.chart_local.max()) + 1 if len(p.chart_local) else 0
            umin = np.full(n, np.inf)
            vmin = np.full(n, np.inf)
            umax = np.full(n, -np.inf)
            vmax = np.full(n, -np.inf)
            np.minimum.at(umin, p.chart_local, p.uw)
            np.minimum.at(vmin, p.chart_local, p.vw)
            np.maximum.at(umax, p.chart_local, p.uw)
            np.maximum.at(vmax, p.chart_local, p.vw)
            present = np.isfinite(umin)
            for c in np.nonzero(present)[0]:
                charts.append((key, index, int(c), umin[c], vmin[c], umax[c] - umin[c], vmax[c] - vmin[c]))
    pages = []
    place = {}  # (part index, local chart) -> (page, x, y, density)
    packer = _Packer(page, gutter)
    for key in sorted(materials):
        mine = [c for c in charts if c[0] == key]
        d = density
        while True:
            sized = sorted(((max(1, math.ceil(c[5] * d)) + 2 * gutter, max(1, math.ceil(c[6] * d)) + 2 * gutter, c)
                            for c in mine), key=lambda x: (-x[1], -x[0]))
            spots = packer.try_pack(sized)
            if spots is None and packer.used:
                packer.close(pages)
                spots = packer.try_pack(sized)
            if spots is not None:
                break
            d *= 0.7
            if d < DENSITY_LEAST:
                raise LayoutError("material %s does not fit a page" % (key,))
        for (w, h, c), (x, y) in zip(sized, spots):
            place[(c[1], c[2])] = (len(pages), x, y, d, c[3], c[4])
    packer.close(pages)
    # (each copy's coordinates on its page; each part's page)
    report = {"charts": len(charts), "pages": len(pages), "parts": len(laid),
              "vertices": int(sum(len(p.vertices) for p in laid)), "stretch_most": 0.0}
    for index, p in enumerate(laid):
        page_index = {place[(index, c)][0] for c in np.unique(p.chart_local)}
        if len(page_index) != 1:
            raise LayoutError("a part's charts on two pages")
        p.page = page_index.pop()
        x = np.zeros(len(p.vertices))
        y = np.zeros(len(p.vertices))
        for c in np.unique(p.chart_local):
            _, cx, cy, d, umin, vmin = place[(index, int(c))]
            m = p.chart_local == c
            x[m] = cx + gutter + (p.uw[m] - umin) * d
            y[m] = cy + gutter + (p.vw[m] - vmin) * d
        w, h = pages[p.page]
        p.luv = (pack_coordinates(x / w) | (pack_coordinates(y / h) << np.uint32(16))).astype(np.uint32)
        # (the stretch: a triangle's area over its area laid flat along its chart's axis)
        normals, area = _face_normals(p.vertices[:, 0:3].astype(np.float64), p.triangles)
        axes = DIRECTIONS[p.chart_facing[p.chart_local[p.triangles[:, 0]]]]
        cosine = np.abs(np.einsum("ij,ij->i", normals, axes))
        ok = area > 1e-9
        if ok.any():
            stretch = 1.0 / np.maximum(cosine[ok], 1e-6)
            report["stretch_most"] = max(report["stretch_most"], float(stretch.max()))
            report.setdefault("_stretch_area", 0.0)
            report.setdefault("_area", 0.0)
            report["_stretch_area"] += float((stretch * area[ok]).sum())
            report["_area"] += float(area[ok].sum())
            report.setdefault("_area_over_2", 0.0)
            report["_area_over_2"] += float(area[ok][stretch > 2.0].sum())
    whole = max(report.pop("_area", 0.0), 1e-9)
    report["stretch_mean"] = report.pop("_stretch_area", 0.0) / whole
    report["area_stretched_over_2"] = report.pop("_area_over_2", 0.0) / whole
    report["densities"] = sorted({round(v[3], 4) for v in place.values()})
    report["page_sizes"] = pages
    return laid, pages, report


class _Packer:
    """shelves on one page at a time"""

    def __init__(self, page, gutter):
        self.page = page
        self.gutter = gutter
        self.reset()

    def reset(self):
        self.x = 0
        self.y = 0
        self.shelf = 0
        self.used = False

    def try_pack(self, sized):
        """spots (x, y) for every (w, h, _) in order, the page's state advanced; None (state kept) if they do not
        all fit"""
        x, y, shelf = self.x, self.y, self.shelf
        spots = []
        for w, h, _ in sized:
            if w > self.page or h > self.page:
                return None
            if x + w > self.page:
                y += shelf
                x, shelf = 0, 0
            if y + h > self.page:
                return None
            spots.append((x, y))
            x += w
            shelf = max(shelf, h)
        self.x, self.y, self.shelf = x, y, shelf
        self.used = self.used or bool(sized)
        return spots

    def close(self, pages):
        """the page done: its height cut to what its shelves use (a multiple of 16 texels)"""
        if self.used:
            pages.append((self.page, min(self.page, (self.y + self.shelf + 15) // 16 * 16)))
        self.reset()


def encode(laid, pages):
    """LMUV v2 bytes of laid parts (in GEOM's part order) and their pages"""
    header = struct.pack("<IIII", LMUV_VERSION, len(laid), 1, len(pages))
    header += b"".join(struct.pack("<II", w, h) for w, h in pages)
    header += b"".join(struct.pack("<II", len(p.vertices), p.page) for p in laid)
    words = np.concatenate([p.luv for p in laid]).astype("<u4") if laid else np.zeros(0, "<u4")
    data = words.tobytes()
    shuffled = np.frombuffer(data, np.uint8).reshape(-1, 4).T.copy().tobytes() if data else b""
    table = struct.pack("<4sIII", b"LUV2", 4, len(words), len(shuffled))
    return LMUV_MAGIC + header + table + shuffled


def decode(data):
    """(pages [(w, h)], parts [(page, [u32 coordinates])]) of LMUV v2 bytes"""
    if len(data) < 20 or data[:4] != LMUV_MAGIC:
        raise LayoutError("not LMUV")
    version, part_count, column_count, page_count = struct.unpack_from("<IIII", data, 4)
    if version != LMUV_VERSION or part_count > 1 << 20 or column_count > 64 or page_count > 1 << 10:
        raise LayoutError("LMUV v2 header")
    at = 20
    pages = [struct.unpack_from("<II", data, at + 8 * k) for k in range(page_count)]
    at += 8 * page_count
    heads = [struct.unpack_from("<II", data, at + 8 * k) for k in range(part_count)]
    at += 8 * part_count
    words = None
    for k in range(column_count):
        name, width, count, size = struct.unpack_from("<4sIII", data, at)
        at += 16
        if name == b"LUV2":
            words = (width, count, size)
    blob = data[at:]
    if words is None or words[0] != 4 or words[2] != 4 * words[1] or words[2] > len(blob):
        raise LayoutError("LMUV v2 columns")
    raw = np.frombuffer(blob[:words[2]], np.uint8).reshape(4, -1).T.copy().view("<u4").reshape(-1)
    if len(raw) != sum(n for n, _ in heads) or any(pg >= page_count for _, pg in heads):
        raise LayoutError("LMUV v2 parts disagree")
    out, cursor = [], 0
    for n, pg in heads:
        out.append((pg, raw[cursor:cursor + n]))
        cursor += n
    return pages, out
