#!/usr/bin/env python3
"""A .pmesh (display.prism_models) from a model as the game draws it
(debug.prism_mesh_dump's <model>.json and .bin): its geometries' parts as the
mesh's pieces, by region, permutation and shader, with the model's own nodes as
the skeleton.

    python3 tools/pmesh_from_dump.py <dump>.json <out>.pmesh [--subdivide N | --triangles T]
        [--lod0-cutoff PIXELS] [--levels superhigh,high,...]

Level 0 is the model's most detailed geometry (each permutation's superhigh),
split N times (each triangle into four at its edges' midpoints: the surface
stays CE's, every attribute interpolated, a new vertex's bones the four of its
edge's ends blended, up to four), or as many times as reaching T triangles
takes (the last split only of the largest triangles); the levels after it are
the model's own geometries named by --levels, as they are, each drawn from the
model's own cutoff. Without a split, level 0 is the model's superhigh as it is:
the host's draw of it should match the stock part's (the mesh store's
correctness check). The model's data stays the map's: keep what this writes to
yourself.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pmesh  # noqa: E402

DETAIL_NAMES = ("superlow", "low", "medium", "high", "superhigh")


def part_arrays(dump: dict, blob: bytes, part: dict):
    """a part's vertices (17 floats each) and its triangles, as the game's strip or list makes them, wound as
    Vulkan winds them (a strip's odd triangles (i, i + 2, i + 1)), degenerate ones left out"""
    count = part["vertex_count"]
    vertices = np.frombuffer(blob, dtype="<f4", count=count * 17, offset=part["vertex_offset"]).reshape(count, 17)
    indices = np.frombuffer(blob, dtype="<u2", count=part["index_count"], offset=part["index_offset"]).astype(np.int64)
    if part["strip"]:
        a, b, c = indices[:-2], indices[1:-1], indices[2:]
        odd = (np.arange(len(a)) & 1) == 1
        triangles = np.stack([a, np.where(odd, c, b), np.where(odd, b, c)], axis=1)
    else:
        triangles = indices.reshape(-1, 3)
    keep = (triangles[:, 0] != triangles[:, 1]) & (triangles[:, 1] != triangles[:, 2]) & \
        (triangles[:, 0] != triangles[:, 2]) & np.all(triangles < count, axis=1)
    return vertices.astype(np.float64), triangles[keep]


class Attributes:
    """a piece's vertices as arrays: position, normal, tangent, handedness, uv, and four bones and weights"""

    def __init__(self, raw: np.ndarray):
        self.position = raw[:, 0:3].copy()
        self.normal = raw[:, 3:6].copy()
        binormal = raw[:, 6:9]
        self.tangent = raw[:, 9:12].copy()
        self.sign = np.where(np.einsum("ij,ij->i", np.cross(self.normal, self.tangent), binormal) < 0, -1, 1)
        self.uv = raw[:, 12:14].copy()
        n = len(raw)
        self.joints = np.zeros((n, 4), dtype=np.int64)
        self.weights = np.zeros((n, 4))
        self.joints[:, 0] = raw[:, 14]
        self.joints[:, 1] = raw[:, 15]
        self.weights[:, 0] = raw[:, 16]
        self.weights[:, 1] = 1.0 - raw[:, 16]
        same = self.joints[:, 0] == self.joints[:, 1]
        self.weights[same, 0] = 1.0
        self.weights[same, 1] = 0.0

    def __len__(self):
        return len(self.position)

    def append_midpoints(self, a: np.ndarray, b: np.ndarray):
        """vertices at the midpoints of edges a-b appended"""
        self.position = np.concatenate([self.position, (self.position[a] + self.position[b]) * 0.5])
        self.normal = np.concatenate([self.normal, self.normal[a] + self.normal[b]])
        self.tangent = np.concatenate([self.tangent, self.tangent[a] + self.tangent[b]])
        self.sign = np.concatenate([self.sign, self.sign[a]])
        self.uv = np.concatenate([self.uv, (self.uv[a] + self.uv[b]) * 0.5])
        joints, weights = blend_bones(self.joints[a], self.weights[a], self.joints[b], self.weights[b])
        self.joints = np.concatenate([self.joints, joints])
        self.weights = np.concatenate([self.weights, weights])


def blend_bones(ja, wa, jb, wb):
    """two vertices' bones (n, 4) blended half and half: each bone's weights summed, the four largest kept"""
    joints = np.concatenate([ja, jb], axis=1)
    weights = np.concatenate([wa, wb], axis=1) * 0.5
    n = len(joints)
    out_j = np.zeros((n, 4), dtype=np.int64)
    out_w = np.zeros((n, 4))
    # each distinct bone's weight: sort by bone, sum runs
    order = np.argsort(joints, axis=1, kind="stable")
    sj = np.take_along_axis(joints, order, axis=1)
    sw = np.take_along_axis(weights, order, axis=1)
    summed = sw.copy()
    for k in range(6, -1, -1):
        same = sj[:, k] == sj[:, k + 1]
        summed[same, k] += summed[same, k + 1]
        summed[same, k + 1] = 0.0
    top = np.argsort(-summed, axis=1, kind="stable")[:, :4]
    out_j = np.take_along_axis(sj, top, axis=1)
    out_w = np.take_along_axis(summed, top, axis=1)
    out_j[out_w <= 0] = out_j[:, :1].repeat(4, axis=1)[out_w <= 0]
    return out_j, out_w


def subdivide(attributes: Attributes, triangles: np.ndarray, which: np.ndarray | None = None) -> np.ndarray:
    """each triangle (or those of which) split in four at its edges' midpoints, shared by its neighbours; a
    neighbour of a split triangle that is not split itself gets its edge's midpoint too (split in two or three)"""
    if which is None:
        which = np.ones(len(triangles), dtype=bool)
    edges = np.concatenate([triangles[which][:, [0, 1]], triangles[which][:, [1, 2]], triangles[which][:, [2, 0]]])
    keys = np.sort(edges, axis=1)
    unique, inverse = np.unique(keys, axis=0, return_inverse=True)
    base = len(attributes)
    attributes.append_midpoints(unique[:, 0], unique[:, 1])
    lookup = {(int(a), int(b)): base + i for i, (a, b) in enumerate(unique)}
    out = []
    split = triangles[which]
    m = len(split)
    inverse = inverse.reshape(-1)
    m01, m12, m20 = base + inverse[:m], base + inverse[m:2 * m], base + inverse[2 * m:]
    a, b, c = split[:, 0], split[:, 1], split[:, 2]
    out.append(np.stack([a, m01, m20], axis=1))
    out.append(np.stack([m01, b, m12], axis=1))
    out.append(np.stack([m20, m12, c], axis=1))
    out.append(np.stack([m01, m12, m20], axis=1))
    # the others: split along the edges that have a midpoint now (no cracks)
    for triangle in triangles[~which]:
        t = [int(x) for x in triangle]
        mids = [lookup.get((min(t[i], t[(i + 1) % 3]), max(t[i], t[(i + 1) % 3]))) for i in range(3)]
        out.append(np.asarray(fan_split(t, mids), dtype=np.int64).reshape(-1, 3))
    return np.concatenate(out)


def fan_split(t, mids):
    """a triangle with midpoints on some of its edges, as triangles (its winding kept)"""
    ring = []
    for i in range(3):
        ring.append(t[i])
        if mids[i] is not None:
            ring.append(mids[i])
    if len(ring) == 3:
        return [t]
    if len(ring) == 6:
        return [[t[0], mids[0], mids[2]], [mids[0], t[1], mids[1]], [mids[2], mids[1], t[2]], [mids[0], mids[1], mids[2]]]
    # (4 or 5 points: a fan from the first midpoint, whose neighbours on its edge are never in one triangle with it)
    start = next(i for i, v in enumerate(ring) if v in mids)
    ordered = ring[start:] + ring[:start]
    return [[ordered[0], ordered[i], ordered[i + 1]] for i in range(1, len(ordered) - 1)]


def areas(attributes: Attributes, triangles: np.ndarray) -> np.ndarray:
    p = attributes.position
    return 0.5 * np.linalg.norm(np.cross(p[triangles[:, 1]] - p[triangles[:, 0]], p[triangles[:, 2]] - p[triangles[:, 0]]),
                                axis=1)


def to_vertices(attributes: Attributes) -> np.ndarray:
    n = len(attributes)
    out = np.zeros(n, dtype=pmesh.VERTEX_DTYPE)
    normal = attributes.normal / np.maximum(np.linalg.norm(attributes.normal, axis=1, keepdims=True), 1e-20)
    tangent = attributes.tangent - normal * np.einsum("ij,ij->i", attributes.tangent, normal)[:, None]
    length = np.linalg.norm(tangent, axis=1, keepdims=True)
    fallback = np.cross(normal, np.where(np.abs(normal[:, :1]) < 0.9, [[1.0, 0, 0]], [[0, 1.0, 0]]))
    tangent = np.where(length > 1e-8, tangent / np.maximum(length, 1e-20), fallback)
    out["position"] = attributes.position
    out["normal"] = pmesh.octahedral(normal)
    out["tangent"] = pmesh.octahedral(tangent / np.linalg.norm(tangent, axis=1, keepdims=True))
    out["uv0"] = attributes.uv
    out["uv1"] = attributes.uv.astype(np.float16)
    out["joints"] = attributes.joints
    out["weights"] = pmesh.quantise_weights(attributes.weights)
    out["sign"] = np.where(attributes.sign < 0, -1, 1)
    return out


def node_matrices(dump: dict):
    """each node's bind pose (3x4, model space) and its inverse, from the dump's inverse"""
    out = []
    for node in dump["nodes"]:
        s, fi, fj, fk, li, lj, lk, ui, uj, uk, px, py, pz = node["inverse"]
        # real_matrix4x3: scale, forward, left, up, position (columns); x' = s * (fwd x + left y + up z) + pos
        m = np.eye(4)
        m[:3, 0] = np.array([fi, fj, fk]) * s
        m[:3, 1] = np.array([li, lj, lk]) * s
        m[:3, 2] = np.array([ui, uj, uk]) * s
        m[:3, 3] = [px, py, pz]
        bind = np.linalg.inv(m)
        out.append((bind[:3, :], m[:3, :]))
    return out


def build(dump: dict, blob: bytes, subdivisions: int, target: int, lod0_cutoff: float | None, levels: list[str]):
    pieces_by_level = []
    detail_index = {name: i for i, name in enumerate(DETAIL_NAMES)}
    plan = [("superhigh", True)] + [(name, False) for name in levels]
    report = []
    for level_number, (detail, split) in enumerate(plan):
        groups = {}
        for region_index, region in enumerate(dump["regions"]):
            for permutation_index, permutation in enumerate(region["permutations"]):
                geometry_index = permutation["geometries"][detail_index[detail]]
                if not 0 <= geometry_index < len(dump["geometries"]):
                    continue
                for part in dump["geometries"][geometry_index]["parts"]:
                    if part["vertex_count"] <= 0 or part["flags"] & 1:
                        continue
                    key = (region_index, permutation_index, part["shader"])
                    groups.setdefault(key, []).append(part)
        level_pieces = []
        for key, parts in sorted(groups.items()):
            raws, tris, base = [], [], 0
            for part in parts:
                raw, triangles = part_arrays(dump, blob, part)
                raws.append(raw)
                tris.append(triangles + base)
                base += len(raw)
            attributes = Attributes(np.concatenate(raws))
            triangles = np.concatenate(tris)
            level_pieces.append([key, attributes, triangles])
        if split and (subdivisions or target):
            done = 0
            while True:
                total = sum(len(t) for _, _, t in level_pieces)
                if subdivisions and done >= subdivisions:
                    break
                if target and total >= target:
                    break
                if target and total * 4 > target and not subdivisions:
                    # the last split: only the largest triangles, as many as reach the target (each adds 3 and its
                    # neighbours' halves)
                    wanted = (target - total) // 3 + 1
                    all_areas = np.concatenate([areas(a, t) for _, a, t in level_pieces])
                    threshold = np.sort(all_areas)[::-1][min(wanted, len(all_areas)) - 1]
                    for entry in level_pieces:
                        entry[2] = subdivide(entry[1], entry[2], areas(entry[1], entry[2]) >= threshold)
                    break
                for entry in level_pieces:
                    entry[2] = subdivide(entry[1], entry[2])
                done += 1
        pieces_by_level.append(level_pieces)
        report.append((detail, sum(len(t) for _, _, t in level_pieces), sum(len(a) for _, a, _ in level_pieces)))
    # the file's arrays
    vertices, indices, pieces, mesh_levels = [], [], [], []
    vertex_at = index_at = 0
    cutoffs = dump["detail_cutoff_pixels"]
    for level_number, level_pieces in enumerate(pieces_by_level):
        level_vertex_first, level_index_first, piece_first = vertex_at, index_at, len(pieces)
        for (region, permutation, shader), attributes, triangles in level_pieces:
            v = to_vertices(attributes)
            vertices.append(v)
            indices.append(triangles.astype(np.uint32).reshape(-1))
            pieces.append(pmesh.Piece(region, permutation, shader, 0, 0, vertex_at, len(v), index_at, triangles.size))
            vertex_at += len(v)
            index_at += triangles.size
        if level_number == 0:
            cutoff = lod0_cutoff if lod0_cutoff is not None else cutoffs[4]
        else:
            cutoff = cutoffs[detail_index[plan[level_number][0]]] if level_number + 1 < len(pieces_by_level) else 0.0
        points = np.concatenate([a.position for _, a, _ in level_pieces])
        centre = (points.max(axis=0) + points.min(axis=0)) * 0.5
        mesh_levels.append(pmesh.Level(float(cutoff), piece_first, len(pieces) - piece_first, level_vertex_first,
                                       vertex_at - level_vertex_first, level_index_first, index_at - level_index_first,
                                       tuple(float(x) for x in centre),
                                       float(np.linalg.norm(points - centre, axis=1).max())))
    all_vertices = np.concatenate(vertices)
    bones = []
    for index, (node, (bind, inverse)) in enumerate(zip(dump["nodes"], node_matrices(dump))):
        bones.append(pmesh.Bone(node["name"], node["parent"], index, bind, inverse))
    positions = all_vertices["position"].astype(np.float64)
    radius = float(np.linalg.norm(positions, axis=1).max()) * 1.0001
    mesh = pmesh.Mesh(len(dump["nodes"]), dump["node_crc"], bones, mesh_levels, pieces, all_vertices,
                      np.concatenate(indices), (0.0, 0.0, 0.0), radius,
                      {"source": "tools/pmesh_from_dump.py", "tag": dump["tag"], "map": dump.get("map"),
                       "subdivisions": subdivisions, "target_triangles": target,
                       "levels": [{"detail": d, "triangles": t, "vertices": v} for d, t, v in report]})
    return mesh, report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--subdivide", type=int, default=0)
    parser.add_argument("--triangles", type=int, default=0, help="split until level 0 has at least this many")
    parser.add_argument("--lod0-cutoff", type=float, default=None,
                        help="the size level 0 is drawn from (pixels of the 480-line screen; default the model's "
                             "superhigh cutoff)")
    parser.add_argument("--levels", default="", help="the model's geometries after level 0, e.g. superhigh,medium")
    arguments = parser.parse_args()
    dump = json.loads(arguments.dump.read_text())
    blob = arguments.dump.with_suffix(".bin").read_bytes()
    levels = [x for x in arguments.levels.split(",") if x]
    for name in levels:
        if name not in DETAIL_NAMES:
            parser.error(f"--levels: {name} is not one of {', '.join(DETAIL_NAMES)}")
    mesh, report = build(dump, blob, arguments.subdivide, arguments.triangles, arguments.lod0_cutoff, levels)
    problems = pmesh.check(mesh, dump)
    if problems:
        for problem in problems:
            print(f"problem: {problem}")
        return 1
    size = pmesh.write_pmesh(arguments.out, mesh)
    print(f"{arguments.out}: {size} bytes")
    print(pmesh.summary(pmesh.read_pmesh(arguments.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
