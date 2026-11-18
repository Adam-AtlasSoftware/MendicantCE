"""PRISM's level remodels as the pack tools write them (tools/prism_packs/art/world_geo/wg3_pwg.py, wg3_build.py), on a
made-up level of four stock triangles: version 3's file read back as written, the seal groups (surfaces joined across
an edge that is not the stock edge on both sides), the needle check and its fix (no edge opened, near, far and with
neighbours at different levels), and the atlas. Needs numpy and scipy (xatlas for the atlas); no game data."""
import struct
import sys
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")
pytest.importorskip("scipy")

WORLD = Path(__file__).resolve().parent / "prism_packs" / "art" / "world_geo"
sys.path.insert(0, str(WORLD))
import wg_common as wg  # noqa: E402
import wg3_pwg as v3  # noqa: E402
from wg2_cracks import MODES, new_edges, seams  # noqa: E402

# a square of two triangles (0, 1) and its neighbour square (2, 3) to the right: corners in the BSP's order, wound so
# that minus their cross product (the engine's front) is +z, as the stock normals say
SQUARES = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [2, 0, 0], [2, 1, 0]], np.float32)
STOCK = np.array([[0, 2, 1], [0, 3, 2], [1, 5, 4], [1, 2, 5]], np.int64)


def material():
    n = len(SQUARES)
    normal = np.tile(np.array([0, 0, 1], np.float32), (n, 1))
    return wg.Material(name="m", shader="levels\\test\\shaders\\metal", shader_group="senv", maps={}, lightmap_index=0,
                       material_index=0, has_lightmap=True, position=SQUARES.copy(), normal=normal,
                       tangent=np.tile(np.array([1, 0, 0], np.float32), (n, 1)),
                       binormal=np.tile(np.array([0, 1, 0], np.float32), (n, 1)), uv0=SQUARES[:, :2].copy(),
                       uv1=SQUARES[:, :2].copy() / 2, radiosity=np.zeros((n, 3), np.float32), triangles=STOCK.copy())


def part(points, near, surface_of, far=None):
    """a record of the material: rows at the points, each surface's near triangles (and far ones: a dict)"""
    points = np.asarray(points, np.float32)
    near = np.asarray(near, np.int64)
    surface_of = np.asarray(surface_of, np.int64)
    order = np.argsort(surface_of, kind="stable")
    near, surface_of = near[order], surface_of[order]
    sft = np.searchsorted(surface_of, np.arange(5)).astype(np.int64)
    tri = [near]
    pairs = np.stack([sft[:-1], np.diff(sft)], 1)
    cursor = len(near)
    for s, t in sorted((far or {}).items()):
        t = np.asarray(t, np.int64)
        pairs[s] = [cursor, len(t)]
        cursor += len(t)
        tri.append(t)
    verts = np.zeros((len(points), 14), np.float32)
    verts[:, 0:3] = points
    verts[:, 3:6] = [0, 0, 1]
    m = material()
    rec = bytearray(64)
    struct.pack_into("<hhIIIII", rec, 0, 0, 0, 4, len(SQUARES), len(points), cursor, wg.topology_check(STOCK))
    struct.pack_into("<dII", rec, 24, wg.position_check(m.position), 0, 4)
    return dict(rec=bytes(rec), lm=0, mi=0, pfs=0, psc=4, ntc=len(near), dist=8.0, sft=sft, verts=verts,
                lmv=np.zeros((len(points), 5), np.float32), tri=np.concatenate(tri), pairs=pairs,
                bounds=np.zeros((4, 4), np.float32), group=None, dev=None, atlas_chart=None, atlas_uv=None)


def whole():
    """every surface its stock triangle: every side plain"""
    return part(SQUARES, STOCK, [0, 1, 2, 3])


def split_middle():
    """the edge x = 1 (surface 0's and surface 3's) split at (1, 0.5) on both sides (sealed, but not the stock edge):
    those two joined"""
    points = np.concatenate([SQUARES, [[1, 0.5, 0]]])
    near = [[0, 2, 6], [0, 6, 1], [0, 3, 2], [1, 5, 4], [1, 6, 5], [6, 2, 5]]
    return part(points, near, [0, 0, 1, 2, 3, 3])


def test_version_3_read_back(tmp_path):
    level = v3.Level([material()], [whole()])
    labels, count = v3.seal_groups(level)
    dev = v3.deviations(level)
    p = level.parts[0]
    p["group"], p["dev"] = labels, dev.astype(np.float32)
    p["atlas_chart"] = np.array([0, 0, 1, 1])
    p["atlas_uv"] = np.arange(24).reshape(4, 3, 2) * 8
    path = str(tmp_path / "t.pwg")
    header = b"PWG1" + bytes(60)
    v3.write_v3(path, header, [p], dict(groups=count, pages=1, page_width=64, page_height=64, density=4.0),
                np.array([0, 0]))
    f = v3.read(path)
    assert f["version"] == 3 and f["far"] and f["ext"]["groups"] == count and f["ext"]["charts"] == 2
    q = f["parts"][0]
    assert (q["group"] == labels).all() and np.allclose(q["dev"], dev)
    assert (q["atlas_chart"] == [0, 0, 1, 1]).all() and (q["atlas_uv"] == np.arange(24).reshape(4, 3, 2) * 8).all()
    assert (q["tri"] == p["tri"]).all() and (q["sft"] == p["sft"]).all()
    # (the older tools read it too)
    import wg2_seal
    _, far, parts = wg2_seal.read_pwg(path)
    assert far and len(parts) == 1


def test_seal_groups():
    level = v3.Level([material()], [whole()])
    plain = v3.plain_sides(level)
    assert plain.all()
    _, count = v3.seal_groups(level, plain)
    assert count == 4
    level = v3.Level([material()], [split_middle()])
    labels, count = v3.seal_groups(level)
    assert count == 3 and labels[0] == labels[3] and len({labels[0], labels[1], labels[2]}) == 3


def test_a_needle_is_moved_back_and_no_edge_opens():
    p = whole()
    # a needle: a fifth point far out of surface 0, a fin of two triangles on its edge from (0, 0) to (1, 1)
    p = part(np.concatenate([SQUARES, [[5, -3, 2]]]), np.concatenate([STOCK, [[0, 2, 6], [2, 0, 6]]]),
             [0, 1, 2, 3, 0, 0])
    level = v3.Level([material()], [p])
    found = v3.check_shape(level)
    assert len(found["needle"][0]) == 1
    mats = [material()]
    before = {mode: seams(mats, [dict(p, verts=p["verts"].copy())], mode) for mode in MODES}
    report = {}
    v3.fix_shape(level, mats, report=report)
    assert report["positions_moved"] == 1 and report["needles_left"] == 0 and report["caps_left"] == 0
    assert np.abs(level.parts[0]["verts"][6, 0:3] - [1, 0, 0]).max() < 1e-6 or \
        v3.tri_distance(level.parts[0]["verts"][6:7, 0:3].astype(np.float64), *(SQUARES[STOCK[0]][None, k] for k in
                                                                                range(3)))[0][0] < 1e-6
    for mode in MODES:
        assert not len(new_edges(before[mode], seams(mats, level.parts, mode)))


def test_the_builds_needle_bound_lies_inside_the_engines():
    # a point past surface 0's corner (1, 0): 0.7 past its edges and 0.85 above it (inside the build's 0.9 for both),
    # 1.31 from the triangle: 0.92 of the needle's reach (its longest edge, the square's diagonal). The build moves it;
    # the engine's bound (the whole reach) would let it be.
    p = part(np.concatenate([SQUARES, [[1.7, -0.7, 0.85]]]), np.concatenate([STOCK, [[0, 6, 1], [6, 0, 1]]]),
             [0, 1, 2, 3, 0, 0])
    level = v3.Level([material()], [p])
    dist = v3.tri_distance(np.array([[1.7, -0.7, 0.85]]), *(level.C[0:1, k] for k in range(3)))[0][0]
    assert v3.BUILD_NEEDLE * np.sqrt(2) < dist < np.sqrt(2)
    found = v3.check_shape(level)
    assert len(found["needle"][0]) == 1 and not len(found["cap"][0])
    engine = v3.check_shape(level, relief=v3.SHAPE_RELIEF, margin=v3.SHAPE_MARGIN, reach=1.0)
    assert not len(engine["needle"][0]) and not len(engine["cap"][0])
    report = {}
    v3.fix_shape(level, [material()], report=report)
    assert report["positions_moved"] == 1 and report["needles_left"] == 0


def test_past_the_bounds_moved_just_inside():
    p = part(np.concatenate([SQUARES, [[0.5, 0.2, 0.95]]]), np.concatenate([STOCK[1:], [[0, 6, 1], [6, 2, 1],
                                                                                         [0, 2, 6]]]),
             [1, 2, 3, 0, 0, 0])
    level = v3.Level([material()], [p])
    assert len(v3.check_shape(level)["cap"][0]) == 1
    v3.fix_shape(level, [material()])
    h = level.parts[0]["verts"][6, 2]
    assert 0.85 < h <= v3.BUILD_RELIEF


def test_the_atlas():
    pytest.importorskip("xatlas")
    level = v3.Level([material()], [whole()])
    per_part, charts, pages = v3.atlas(level, density=8.0, page=64)
    chart, uv = per_part[0]
    assert pages["pages"] >= 1 and len(charts) >= 1
    assert ((chart >= 0) & (chart < len(charts))).all()
    assert (uv <= pages["page_width"] * v3.UV_SCALE).all()
