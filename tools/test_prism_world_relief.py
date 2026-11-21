"""fork: the remodel's metal relief (tools/prism_packs/art/world_geo/wg2_relief.py) on made-up fields, without
Blender or the game's data: the height taken back from a normal map, the contour lines meeting across the tile's
edges, and a tile's mesh clipped into a triangle covering it exactly once. Also the recipes: no metal class grooves
CE's flat surfaces any more."""

import json
import math
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
WORLD = ROOT / "tools" / "prism_packs" / "art" / "world_geo"
sys.path.insert(0, str(WORLD))

import wg2_relief as relief  # noqa: E402


def channel_height(n=64):
    """plates at 0 with a channel 3 texels wide and 2 deep along x = 20, and a square pit"""
    H = np.zeros((n, n))
    H[:, 19:22] = -2.0
    H[40:46, 40:46] = -2.0
    return H


def normal_map(H):
    gx = (np.roll(H, -1, 1) - np.roll(H, 1, 1)) / 2.0
    gy = (np.roll(H, -1, 0) - np.roll(H, 1, 0)) / 2.0
    n = np.stack([-gx, -gy, np.ones_like(H)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return np.concatenate([n * 0.5 + 0.5, np.ones(H.shape + (1,))], -1)


def test_the_height_comes_back_from_the_normal_map():
    H = channel_height()
    back = relief.height_from_bump(normal_map(H), drift_px=12)
    # the channel and the pit lie well below the plates, which are flat
    assert back[:, 20].mean() < -1.0
    assert back[42, 42] < -1.0
    assert abs(np.median(back[:, 0:10])) < 0.2


def test_contour_lines_meet_across_the_tile_edges():
    H = channel_height()
    lines = relief.contours(H, -1.0)
    ends = [P[e] for P, closed in lines if not closed for e in (0, -1)]
    # the channel runs top to bottom: two lines, each ending on the tile's bottom and top edges at the same x
    ys = sorted({round(float(p[1]), 6) for p in ends})
    assert ys == [0.0, 64.0]
    bottom = sorted(round(float(p[0]), 6) for p in ends if p[1] == 0.0)
    top = sorted(round(float(p[0]), 6) for p in ends if p[1] == 64.0)
    assert bottom == top
    assert any(closed for _P, closed in lines)  # the pit


def test_simplify_keeps_ends_and_corners():
    P = np.array([[0, 0], [1, 0.01], [2, 0], [2, 1], [2, 2]], float)
    Q = relief.simplify(P, 0.1, False)
    assert Q.tolist() == [[0, 0], [2, 0], [2, 2]]


def grid_tile(n=4):
    xs = np.linspace(0, 1, n + 1)
    uv = np.array([(x, y) for y in xs for x in xs])
    tris = []
    for j in range(n):
        for i in range(n):
            a, b, c, d = j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i, (j + 1) * (n + 1) + i + 1
            tris += [(a, b, d), (a, d, c)]
    depth = -(uv[:, 0] * 2.0 + uv[:, 1])
    t = relief.Tile(uv + 0.5 / 64, depth, np.array(tris), (64, 64))
    return t


def tri_area(p):
    return 0.5 * abs((p[:, 1, 0] - p[:, 0, 0]) * (p[:, 2, 1] - p[:, 0, 1]) -
                     (p[:, 1, 1] - p[:, 0, 1]) * (p[:, 2, 0] - p[:, 0, 0]))


def test_the_clip_covers_the_triangle_once_and_keeps_the_depth():
    tile = grid_tile()
    tri = np.array([[0.13, -0.4], [2.71, 0.35], [0.9, 1.93]])
    pts, tris = relief.clip_tile(tile, tri)
    area = tri_area(pts[tris][..., :2]).sum()
    assert math.isclose(area, tri_area(tri[None])[0], rel_tol=1e-9)
    # the depth is linear in each tile triangle: a point's depth is the tile's own there (the plane through the
    # repeat it is in)
    for p in pts[:50]:
        u = (p[0] - tile.origin[0]) % 1.0
        v = (p[1] - tile.origin[1]) % 1.0
        assert math.isclose(p[2], -(u * 2.0 + v), abs_tol=1e-9) or u < 1e-9 or v < 1e-9 or u > 1 - 1e-9 or v > 1 - 1e-9


def test_no_recipe_grooves_flat_metal():
    recipes = json.loads((WORLD / "wg2_recipes.json").read_text())
    for key, level in recipes.items():
        if key == "_doc":
            continue
        for name, r in level["classes"].items():
            assert not r.get("grooves"), (key, name)
            if r.get("relief"):
                assert r["kind"] == "metal" and 0 < r["relief_max"] <= 0.05, (key, name)
