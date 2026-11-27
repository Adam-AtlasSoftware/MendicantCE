"""The gameplay rule of PLAN_FINAL.md section 10 enforced by the content build (geopatch M5b): the enhanced geometry
stays close to the original where gameplay depends on it. encode.py runs it on each BSP's remodel before the
lightmap layout; check_levels.py measures a build with the same references (measure), so what the build holds to
and what the gate checks are one definition.

The reference. Every remodel triangle stands for one stock triangle (its surface: the .pwg's runs, near and far);
each of its corners is measured by its signed distance to that stock triangle's plane, positive in front (the way
the stock vertices' own normals look), from the stock positions as the game decodes them (native env). A stock
triangle whose face looks up (normal z at least GROUND_Z, about 45 degrees) is ground: where one walks and stands.
The rest are walls (walls, cover, ceilings, the undersides of things).

One exception, found in M5's tables: the remodel's runs hold a few triangles that lie nowhere near the stock
triangle they are listed under (c10, b40 and c40: about 300 to 600 slivers a BSP, nearly no area, all in the
first surface's run of the first material, their corners on other stock surfaces within 0.01 world units; the
seams' fill of the remodel pipeline). Measured against their listed surface's plane they read as walls 12 to 31
world units out, or c10's 1.05 wu "ground" dip. A triangle whose centre lies more than LATERAL_MOST world units
outside its listed stock triangle, along that triangle's plane (no true remodel triangle is past 1 wu: a bevel or
a rounded edge reaches a little over its triangle's edge), is measured corner by corner against the stock
triangle nearest each corner instead.

The rule:
- ground: every corner within GROUND_BOUND of its reference plane (the gate is TOLERANCE, 0.1 wu, section 2.2's
  budget; the bound leaves room for the encoder's grid of 1/1024 wu, its snap to a stock position within 4 steps
  and its nudge of a step);
- walls within reach (the point of the corner's stock triangle nearest it lies from REACH_BELOW below to REACH
  above stock ground within REACH_RADIUS of it across: where a player, a vehicle or a grenade meets the wall, and
  cover is taken): within WALL_BOUND (gate WALL_TOLERANCE);
- walls out of reach (cliff faces and ceilings above REACH, faces with no ground near): free, the remodel's look.
(The bounds' values come from M5's tables: the remodel's own relief on walls is 0.1 to 0.3 wu at its 99.9th
percentile on every level but d40's terrain, so WALL_BOUND keeps nearly all of it where it is reached; the ground
is section 2.2's budget. Where a wall and the ground cannot both hold, the ground wins.)
A vertex past a bound is moved along the reference's normal back onto it, alternately for each of its references
(the corner's every triangle) until every one holds (projections onto the slabs, in a fixed order: they meet,
since the stock surface itself lies in every slab); a wall it then reaches, or is no longer clear of, joins its
references in the next round (at most ROUNDS). Every vertex of the BSP on that position (the 1/1024 grid:
the copies at a material's seam, the far level's vertices on a stock edge) is moved the same, so no crack opens
between parts, materials or levels. Nothing else moves: base map coordinates, normals and the far level's
choices stay the remodel's, so the shading keeps its relief where the shape is flattened.

The arithmetic is single IEEE double operations in a fixed order (as lmuv.py's: no matrix product, no einsum, no
reduction whose order a SIMD path may choose), and every choice (nearest triangle, reach, which vertex moves) is a
comparison of such values with ties to the lowest index, so the same .pwg moves the same on every machine and the
sidecar's bytes stay identical.
"""
import numpy as np

from . import geom

GROUND_Z = 0.7071
TOLERANCE = 0.1
GROUND_BOUND = 0.09
WALL_TOLERANCE = 0.25
WALL_BOUND = 0.24
REACH = 2.0
REACH_BELOW = 0.5
REACH_RADIUS = 1.0
# (the clamp takes a wall as within reach this much further than the measure does: a corner at the edge of reach
# moves by the encoder's grid between the two, and must not be free in the clamp yet held in the gate)
REACH_MARGIN = 0.05
LATERAL_MOST = 2.0
PASSES = 64
ROUNDS = 12
CELL = 8.0
REACH_CHUNK = 4096
CHUNK = 1 << 18
# (the walls past which a corner's reach is looked for, when measuring: below it no wall is near its bound)
WALL_SCAN = 0.1


# ---------- fixed-order arithmetic

def _dot(a, b):
    """row-wise dot products of N x 3 arrays, as ((a0 b0 + a1 b1) + a2 b2)"""
    return (a[:, 0] * b[:, 0] + a[:, 1] * b[:, 1]) + a[:, 2] * b[:, 2]


def _cross(a, b):
    return np.stack([a[:, 1] * b[:, 2] - a[:, 2] * b[:, 1], a[:, 2] * b[:, 0] - a[:, 0] * b[:, 2],
                     a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]], axis=1)


def closest_point(p, a, b, c):
    """the point of each triangle (a, b, c) nearest p (rows of N x 3 arrays; Ericson's regions); row by row, so
    taken in pieces (memory) with the same result"""
    if len(p) <= CHUNK:
        return _closest_point(p, a, b, c)
    return np.concatenate([_closest_point(p[k:k + CHUNK], a[k:k + CHUNK], b[k:k + CHUNK], c[k:k + CHUNK])
                           for k in range(0, len(p), CHUNK)])


def _closest_point(p, a, b, c):
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    bp = p - b
    d3, d4 = _dot(ab, bp), _dot(ac, bp)
    cp = p - c
    d5, d6 = _dot(ab, cp), _dot(ac, cp)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    with np.errstate(divide="ignore", invalid="ignore"):
        den = (va + vb) + vc
        v = vb / den
        w = vc / den
        inside = a + ab * v[:, None] + ac * w[:, None]
        t_ab = d1 / (d1 - d3)
        t_ac = d2 / (d2 - d6)
        t_bc = (d4 - d3) / ((d4 - d3) + (d5 - d6))
    out = inside
    done = np.zeros(len(p), bool)

    def take(mask, value):
        nonlocal out
        mask = mask & ~done
        out = np.where(mask[:, None], value, out)
        done[mask] = True
    take((d1 <= 0) & (d2 <= 0), a)
    take((d3 >= 0) & (d4 <= d3), b)
    take((d6 >= 0) & (d5 <= d6), c)
    take((vc <= 0) & (d1 >= 0) & (d3 <= 0), a + ab * t_ab[:, None])
    take((vb <= 0) & (d2 >= 0) & (d6 <= 0), a + ac * t_ac[:, None])
    take((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), b + (c - b) * t_bc[:, None])
    # (a degenerate triangle, whose regions all fail: its first corner)
    bad = ~np.isfinite(out).all(axis=1)
    out[bad] = a[bad]
    return out


def _distance(p, q):
    d = p - q
    return np.sqrt(_dot(d, d))


# ---------- the stock surfaces

class Stock:
    """a BSP's stock triangles as the game decodes them, every material's in one list (materials in key order,
    each its surfaces in order): corners a, b, c, unit normal n (front: the vertices' own normals' way), ok (not
    degenerate), ground; offset[(lightmap, material)] is the material's first triangle in the list"""

    def __init__(self, a, b, c, vertex_normals, offset):
        """corners (N x 3 float64 each), the sum of each triangle's three vertex normals, {material key: first}"""
        self.a, self.b, self.c, self.offset = a, b, c, offset
        vn = vertex_normals
        n = _cross(self.b - self.a, self.c - self.a)
        length = np.sqrt(_dot(n, n))
        self.ok = length > 1e-9
        n[self.ok] /= length[self.ok, None]
        n[~self.ok] = 0.0
        n[_dot(n, vn) < 0] *= -1.0
        self.n = n
        self.ground = self.ok & (n[:, 2] >= GROUND_Z)
        self.lo = np.minimum(np.minimum(self.a, self.b), self.c)
        self.hi = np.maximum(np.maximum(self.a, self.b), self.c)
        self._ground_grid = None

    @classmethod
    def of_bsp(cls, bsp, native):
        """a retail BSP's (retail.RetailBsp), decoded by the game's own code (native env)"""
        a, b, c, vn, offset = [], [], [], [], {}
        total = 0
        for key in sorted(bsp.materials):
            m = bsp.materials[key]
            env = np.frombuffer(native.environment_vertices(m.vertices), "<f4").reshape(-1, 14)
            positions = env[:, 0:3].astype(np.float64)
            normals = env[:, 3:6].astype(np.float64)
            s = np.array(m.surfaces, np.int64).reshape(-1, 3)
            offset[key] = total
            total += len(s)
            a.append(positions[s[:, 0]])
            b.append(positions[s[:, 1]])
            c.append(positions[s[:, 2]])
            vn.append((normals[s[:, 0]] + normals[s[:, 1]]) + normals[s[:, 2]])
        cat = lambda xs: np.concatenate(xs) if xs else np.zeros((0, 3))  # noqa: E731
        return cls(cat(a), cat(b), cat(c), cat(vn), offset)

    def plane_distance(self, p, tri):
        return _dot(p - self.a[tri], self.n[tri])

    def nearest(self, points):
        """the stock triangle nearest each point (closest-point distance; the lowest index among equals), among
        the triangles whose box is within a growing reach of it: -1 for none at all"""
        out = np.full(len(points), -1, np.int64)
        for i, p in enumerate(points):
            for reach in (1.0, 8.0, 64.0, 1.0e9):
                candidates = np.nonzero(np.all((self.lo - reach <= p) & (p <= self.hi + reach), axis=1) & self.ok)[0]
                if len(candidates):
                    break
            if not len(candidates):
                continue
            q = closest_point(np.repeat(p[None], len(candidates), 0), self.a[candidates], self.b[candidates],
                              self.c[candidates])
            d = _distance(np.repeat(p[None], len(candidates), 0), q)
            out[i] = candidates[int(np.argmax(d == d.min()))]
        return out

    def within_reach(self, points, grow=0.0):
        """whether each point is a wall a player meets: stock ground within REACH_RADIUS of it across (x, y), the
        point from REACH_BELOW below that ground to REACH above it; grow widens each of the three"""
        if self._ground_grid is None:
            self._build_ground_grid()
        out = np.zeros(len(points), bool)
        if not len(points) or not len(self._ground_grid[2]):
            return out
        # (in pieces: a cell may hold a few hundred triangles, each a row per point)
        for start in range(0, len(points), REACH_CHUNK):
            out[start:start + REACH_CHUNK] = self._within_reach(points[start:start + REACH_CHUNK], grow)
        return out

    def _within_reach(self, points, grow):
        keys, order, tri = self._ground_grid
        out = np.zeros(len(points), bool)
        cells = np.floor(points[:, 0:2] / CELL).astype(np.int64)
        wanted = (cells[:, 0] + (1 << 30)) << 32 | (cells[:, 1] + (1 << 30))
        lo = np.searchsorted(keys, wanted, "left")
        counts = np.searchsorted(keys, wanted, "right") - lo
        owner = np.repeat(np.arange(len(points)), counts)
        within = np.arange(len(owner)) - np.repeat(np.cumsum(counts) - counts, counts)
        t = tri[order[np.repeat(lo, counts) + within]]
        if not len(t):
            return out
        p = points[owner]
        flat = lambda x: np.stack([x[:, 0], x[:, 1], np.zeros(len(x))], axis=1)  # noqa: E731
        q = closest_point(flat(p), flat(self.a[t]), flat(self.b[t]), flat(self.c[t]))
        across = _distance(flat(p), q)
        n, a = self.n[t], self.a[t]
        height = a[:, 2] - (n[:, 0] * (q[:, 0] - a[:, 0]) + n[:, 1] * (q[:, 1] - a[:, 1])) / n[:, 2]
        h = p[:, 2] - height
        meets = (across <= REACH_RADIUS + grow) & (h >= -(REACH_BELOW + grow)) & (h <= REACH + grow)
        out[owner[meets]] = True
        return out

    def _build_ground_grid(self):
        """the ground triangles binned by (x, y) cells of CELL, each in every cell its box (grown by
        REACH_RADIUS and REACH_MARGIN) touches: (sorted cell keys, their order, the triangle of each entry)"""
        ground = np.nonzero(self.ground)[0]
        lo = np.floor((self.lo[ground, 0:2] - (REACH_RADIUS + REACH_MARGIN)) / CELL).astype(np.int64)
        hi = np.floor((self.hi[ground, 0:2] + (REACH_RADIUS + REACH_MARGIN)) / CELL).astype(np.int64)
        span = hi - lo + 1
        counts = span[:, 0] * span[:, 1]
        owner = np.repeat(np.arange(len(ground)), counts)
        k = np.arange(len(owner)) - np.repeat(np.cumsum(counts) - counts, counts)
        cx = lo[owner, 0] + k // span[owner, 1]
        cy = lo[owner, 1] + k % span[owner, 1]
        keys = (cx + (1 << 30)) << 32 | (cy + (1 << 30))
        order = np.argsort(keys, kind="stable")
        self._ground_grid = (keys[order], order, ground[owner])


# ---------- the references of a BSP's remodel

def _triangle_surfaces(part, stock):
    """each triangle's stock triangle (index into stock's list; -1 none) and whether it is a near one"""
    count = len(part.triangles)
    surfaces = np.full(count, -1, np.int64)
    key = (part.lightmap_index, part.material_index)
    if key not in stock.offset or not count:
        return surfaces, np.zeros(count, bool)
    base = stock.offset[key] + int(part.first_surface)
    near = int(part.near_triangle_count)
    runs = part.sft.astype(np.int64)
    counts = np.diff(runs)
    surfaces[runs[0]:runs[0] + int(counts.sum())] = base + np.repeat(np.arange(len(counts)), counts)
    if part.far_pairs is not None:
        pairs = part.far_pairs.astype(np.int64).reshape(-1, 2)
        lengths = pairs[:, 1]
        owner = np.repeat(np.arange(len(pairs)), lengths)
        index = np.repeat(pairs[:, 0], lengths) + (np.arange(len(owner)) - np.repeat(np.cumsum(lengths) - lengths,
                                                                                     lengths))
        far = (index >= near) & (index < count)
        surfaces[index[far]] = base + owner[far]
    is_near = np.zeros(count, bool)
    is_near[:near] = True
    return surfaces, is_near


def corner_references(parts, stock, positions=None):
    """for every part, its triangles' corners' references: (tri (T x 3 stock triangle indices, -1 none), near (T),
    reattributed (T: measured against the nearest stock triangle, its centre past LATERAL_MOST outside its own)).
    positions: each part's N x 3 float64 positions (default: its vertices')"""
    out = []
    for k, part in enumerate(parts):
        p = positions[k] if positions is not None else part.vertices[:, 0:3].astype(np.float64)
        surfaces, is_near = _triangle_surfaces(part, stock)
        tri = np.repeat(surfaces[:, None], 3, axis=1)
        moved = np.zeros(len(surfaces), bool)
        have = np.nonzero(surfaces >= 0)[0]
        if len(have):
            s = surfaces[have]
            corners = p[part.triangles[have]]
            centre = (corners[:, 0] + corners[:, 1]) + corners[:, 2]
            centre = centre / 3.0
            q = closest_point(centre, stock.a[s], stock.b[s], stock.c[s])
            off = centre - q
            d = _dot(off, stock.n[s])
            along = off - stock.n[s] * d[:, None]
            lateral = np.sqrt(_dot(along, along))
            far_out = have[(lateral > LATERAL_MOST) | ~stock.ok[s]]
            if len(far_out):
                nearest = stock.nearest(p[part.triangles[far_out]].reshape(-1, 3)).reshape(-1, 3)
                tri[far_out] = nearest
                moved[far_out] = True
        out.append((tri, is_near, moved))
    return out


# ---------- the clamp

def clamp_bsp(parts, stock):
    """parts (encode.read_pwg's) of one BSP with their positions moved to the rule (in place); a report"""
    report = {"ground_bound": GROUND_BOUND, "wall_bound": WALL_BOUND, "reach": REACH}
    if not parts:
        return report
    positions = [p.vertices[:, 0:3].astype(np.float64) for p in parts]
    references = corner_references(parts, stock, positions)
    # (every vertex of the BSP by its 1/1024 grid position: one group, moved as one)
    sizes = [len(p) for p in positions]
    allp = np.concatenate(positions)
    grid = np.rint(allp * geom.GRID).astype(np.int64)
    _, first, group = np.unique(grid, axis=0, return_index=True, return_inverse=True)
    group = group.reshape(-1)
    start_position = allp[first]
    starts = np.concatenate([[0], np.cumsum(sizes)])
    # (each corner's (group, stock triangle), deduplicated: a group's constraints, in the order of their triangles)
    pairs = []
    for k, part in enumerate(parts):
        tri = references[k][0]
        if not len(part.triangles):
            continue
        g = group[starts[k] + part.triangles.reshape(-1)]
        t = tri.reshape(-1)
        keep = t >= 0
        pairs.append(np.stack([g[keep], t[keep]], axis=1))
    pairs = np.concatenate(pairs) if pairs else np.zeros((0, 2), np.int64)
    pairs = np.unique(pairs, axis=0)
    pairs = pairs[stock.ok[pairs[:, 1]]]
    g, t = pairs[:, 0], pairs[:, 1]
    n, a, ground = stock.n[t], stock.a[t], stock.ground[t]
    place = start_position.copy()
    d = _dot(place[g] - a, n)
    report["ground_corners_past_bound"] = int((ground & (np.abs(d) > GROUND_BOUND)).sum())
    report["ground_most_before"] = round(float(np.abs(d[ground]).max()) if ground.any() else 0.0, 4)
    # (a wall's reach is its stock triangle's, at the point of it nearest the corner: found for the walls past
    # the bound, then for every wall of a group that moves, at its new place, round by round)
    reach = np.zeros(len(pairs), bool)
    known = np.zeros(len(pairs), bool)

    def find_reach(which):
        which = which & ~ground
        if which.any():
            foot = closest_point(place[g[which]], stock.a[t[which]], stock.b[t[which]], stock.c[t[which]])
            reach[which] = stock.within_reach(foot, REACH_MARGIN)
        known[which] = True
    find_reach(np.abs(d) > WALL_BOUND)
    report["walls_in_reach_past_bound"] = int((~ground & (np.abs(d) > WALL_BOUND) & reach).sum())
    report["walls_out_of_reach_past_bound"] = int((~ground & (np.abs(d) > WALL_BOUND) & ~reach).sum())
    # (constraint index range of each group: pairs are sorted by group)
    group_start = np.searchsorted(g, np.arange(len(place)), "left")
    rank = np.arange(len(g)) - group_start[g]

    def solve(use, passes):
        """projections onto the bounds of the constraints in use, rank by rank (a group's constraints in their
        order), pass after pass until every one holds"""
        if not use.any():
            return
        idx = np.nonzero(use)[0]
        top = int(rank[idx].max())
        for _ in range(passes):
            for r in range(top + 1):
                sel = idx[rank[idx] == r]
                at = g[sel]
                dd = _dot(place[at] - a[sel], n[sel])
                over = dd - bound[sel]
                under = -bound[sel] - dd
                shift = np.where(over > 0, -over, np.where(under > 0, under, 0.0))
                place[at] = place[at] + n[sel] * shift[:, None]
            dd = _dot(place[g[idx]] - a[idx], n[idx])
            if not (np.abs(dd) > bound[idx] + 1e-9).any():
                return
    moved = np.zeros(len(place), bool)
    rounds = 0
    for rounds in range(1, ROUNDS + 1):
        bound = np.where(ground, GROUND_BOUND, np.where(reach, WALL_BOUND, np.inf))
        dd = _dot(place[g] - a, n)
        past = np.abs(dd) > bound + 1e-9
        if not past.any():
            break
        moving = np.zeros(len(place), bool)
        moving[g[past]] = True
        # (every wall of a moving group: its reach where not yet known)
        find_reach(moving[g] & ~known)
        bound = np.where(ground, GROUND_BOUND, np.where(reach, WALL_BOUND, np.inf))
        solve(moving[g] & np.isfinite(bound), PASSES)
        moved |= moving
        # (the walls of the groups moved: their reach again at the new place)
        find_reach(moved[g])
    report["rounds"] = rounds
    bound = np.where(ground, GROUND_BOUND, np.where(reach, WALL_BOUND, np.inf))
    dd = _dot(place[g] - a, n)
    stuck = np.zeros(len(place), bool)
    stuck[g[np.abs(dd) > bound + 1e-9]] = True
    report["stuck_on_walls"] = int(stuck.sum())
    if stuck.any():
        # (where a wall and the ground cannot both hold, the ground wins: its references alone)
        bound = np.where(ground, GROUND_BOUND, np.inf)
        solve(stuck[g] & ground, PASSES)
        moved |= stuck
        dd = _dot(place[g] - a, n)
        bad = np.zeros(len(place), bool)
        bad[g[ground & (np.abs(dd) > GROUND_BOUND + 1e-9)]] = True
        report["residual"] = int(bad.sum())
    else:
        report["residual"] = 0
    moved &= np.any(place != start_position, axis=1)
    shift = place[moved] - start_position[moved]
    report["positions_moved"] = int(moved.sum())
    report["most_moved"] = round(float(np.sqrt(_dot(shift, shift)).max()) if len(shift) else 0.0, 4)
    # (every vertex of a moved group to its group's new place)
    count = 0
    for k, part in enumerate(parts):
        mine = group[starts[k]:starts[k + 1]]
        hit = moved[mine]
        if hit.any():
            part.vertices[hit, 0:3] = place[mine[hit]].astype(np.float32)
            count += int(hit.sum())
    report["vertices_moved"] = count
    return report


# ---------- the measure (check_levels.py)

def _summary(values):
    """largest above, largest below, largest either way, the 99.9th percentile of |d|, count"""
    if not len(values):
        return {"count": 0, "above": 0.0, "below": 0.0, "most": 0.0, "p999": 0.0}
    a = np.abs(values)
    return {"count": int(len(values)), "above": round(float(max(values.max(), 0.0)), 4),
            "below": round(float(max(-values.min(), 0.0)), 4), "most": round(float(a.max()), 4),
            "p999": round(float(np.quantile(a, 0.999)), 4)}


def measure(parts, stock, tolerance=TOLERANCE, wall_tolerance=WALL_TOLERANCE):
    """the near triangles' corners of one BSP's remodel (decoded, or a .pwg's) against their references:
    {ground, walls, walls_in_reach, walls_out_of_reach (each a summary of signed distances; the reach sets hold
    the walls past WALL_SCAN only), ground_over_tolerance, ground_share_over, walls_in_reach_over_tolerance,
    reattributed_corners, worst_ground_at, worst_wall_in_reach_at}"""
    references = corner_references(parts, stock)
    ground, walls, points, reattributed = [], [], [], 0
    worst = (0.0, None)
    for part, (tri, is_near, moved) in zip(parts, references):
        near = is_near & (tri[:, 0] >= 0)
        if not near.any():
            continue
        t = tri[near].reshape(-1)
        p = part.vertices[:, 0:3].astype(np.float64)[part.triangles[near]].reshape(-1, 3)
        reattributed += int(moved[near].sum()) * 3
        keep = t >= 0
        keep[keep] = stock.ok[t[keep]]
        t, p = t[keep], p[keep]
        d = stock.plane_distance(p, t)
        g = stock.ground[t]
        ground.append(d[g])
        walls.append(d[~g])
        # (the stock wall's point nearest each corner, for its reach: only the walls past WALL_SCAN need one)
        wall_t, wall_p = t[~g], p[~g]
        foot = np.full((len(wall_t), 3), np.nan)
        scan = np.abs(d[~g]) > WALL_SCAN
        foot[scan] = closest_point(wall_p[scan], stock.a[wall_t[scan]], stock.b[wall_t[scan]], stock.c[wall_t[scan]])
        points.append(foot)
        if g.any():
            k = int(np.argmax(np.abs(d[g])))
            if abs(d[g][k]) > worst[0]:
                worst = (float(abs(d[g][k])), [round(float(x), 3) for x in p[g][k]])
    cat = lambda xs, shape: np.concatenate(xs) if xs else np.zeros(shape)  # noqa: E731
    g, w, wp = cat(ground, 0), cat(walls, 0), cat(points, (0, 3))
    over = int((np.abs(g) > tolerance).sum())
    scan = np.abs(w) > WALL_SCAN
    reach = np.zeros(len(w), bool)
    if scan.any():
        reach[scan] = stock.within_reach(wp[scan])
    in_reach = w[scan & reach]
    worst_wall = None
    if len(in_reach):
        k = int(np.argmax(np.abs(in_reach)))
        worst_wall = [round(float(x), 3) for x in wp[scan & reach][k]]
    return {"ground": _summary(g), "walls": _summary(w), "walls_in_reach": _summary(in_reach),
            "walls_out_of_reach": _summary(w[scan & ~reach]), "ground_over_tolerance": over,
            "ground_share_over": round(over / max(1, len(g)), 6),
            "walls_in_reach_over_tolerance": int((np.abs(in_reach) > wall_tolerance).sum()),
            "reattributed_corners": reattributed, "worst_ground_at": worst[1], "worst_wall_in_reach_at": worst_wall}
