"""fork: the remodel's natural surfaces (tools/prism_packs/art/world_geo/wg2_natural.py) on made-up meshes and fields,
without Blender or the game's data: the relief band-limited by the mesh's edge (nothing finer than the mesh carries),
Taubin smoothing taking a facet off without shrinking a sphere, texture-space noise repeating with the texture, the
texel's size on a surface, the reach file read back, the budget's factor, the soft caps, and the recipes' natural
classes within their caps. The tessellation itself needs Blender's triangulation: tested when mathutils is there."""

import json
import math
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
WORLD = ROOT / "tools" / "prism_packs" / "art" / "world_geo"
sys.path.insert(0, str(WORLD))

import wg2_natural as nat  # noqa: E402


def icosphere(levels=3):
    t = (1.0 + 5 ** 0.5) / 2.0
    V = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
         (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    F = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
         (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
         (8, 6, 7), (9, 8, 1)]
    V = [np.array(v, float) / np.linalg.norm(v) for v in V]
    for _ in range(levels):
        mid = {}
        F2 = []
        for a, b, c in F:
            m = []
            for i, j in ((a, b), (b, c), (c, a)):
                k = (min(i, j), max(i, j))
                if k not in mid:
                    p = V[i] + V[j]
                    V.append(p / np.linalg.norm(p))
                    mid[k] = len(V) - 1
                m.append(mid[k])
            F2 += [(a, m[0], m[2]), (m[0], b, m[1]), (m[2], m[1], c), (m[0], m[1], m[2])]
        F = F2
    return np.array(V), np.array(F)


def test_band_weight_follows_the_edge():
    h = np.array([0.1, 0.1, 0.1])
    w = nat.band_weight(np.array([0.2, 0.45, 1.0]), h)
    assert w[0] == 0.0 and 0.0 < w[1] < 1.0 and w[2] == 1.0
    # a coarser mesh carries less of the same band
    assert nat.band_weight(np.array([0.45]), np.array([0.2]))[0] < w[1]


def test_taubin_keeps_a_sphere_and_takes_a_facet_off():
    V, F = icosphere(3)
    E = nat.edges_of(F)
    N = nat.normalize(V)
    r0 = np.linalg.norm(V, axis=1).mean()
    X = nat.taubin_along(V, N, E, np.ones(len(V), bool), 20)
    assert abs(np.linalg.norm(X, axis=1).mean() - r0) < 0.01 * r0
    # a dent (one vertex pushed in) is mostly smoothed out, its held neighbours untouched
    D = V.copy()
    D[5] *= 0.8
    mov = np.zeros(len(V), bool)
    mov[5] = True
    Y = nat.taubin_along(D, N, E, mov, 10)
    assert abs(np.linalg.norm(Y[5]) - 1.0) < 0.5 * 0.2
    assert np.allclose(np.delete(Y, 5, 0), np.delete(D, 5, 0))


def test_periodic_noise_repeats_with_the_texture():
    rng = np.random.default_rng(1)
    uv = rng.random((200, 2)) * 3.0
    a = nat.periodic_noise2(uv, 12, 4, salt=3)
    b = nat.periodic_noise2(uv + np.array([1.0, 0.0]), 12, 4, salt=3)
    c = nat.periodic_noise2(uv + np.array([0.0, 2.0]), 12, 4, salt=3)
    assert np.allclose(a, b) and np.allclose(a, c)
    assert -1.0 <= a.min() and a.max() <= 1.0 and a.std() > 0.1


def test_texel_size_on_a_surface():
    # a wall 4 wu wide and 2 tall mapped to one repeat of a 256 x 128 map
    P = np.array([[[0, 0, 0], [4, 0, 0], [0, 0, 2]]], float)
    UV = np.array([[[0, 0], [1, 0], [0, 1]]], float)
    t = nat.texel_size(P, UV, 256, 128)
    assert np.allclose(t, [[4 / 256, 2 / 128]])


def test_texture_bands_wavelengths_and_rows():
    h, w = 64, 128
    y = np.arange(h)[:, None] * np.ones((1, w))
    img = np.repeat((0.5 + 0.4 * np.sin(2 * np.pi * y / 16.0))[..., None], 3, 2)
    tb = nat.TextureBands(img)
    assert len(tb.bands) == len(tb.lambda_px) == len(tb.rows)
    assert all(b.shape == (h, w) for b in tb.bands)
    assert tb.lambda_px == sorted(tb.lambda_px)
    # the rows' profile carries the stripes along v
    prof = sum(tb.sample_row(k, np.linspace(0, 1, 64)) for k in range(len(tb.rows)))
    assert prof.std() > 0.2


def test_read_reach(tmp_path):
    bricks = np.array([[1, 2, 3, 3], [-5, 0, 7, 2]], np.int16)
    head = b"PRCH" + struct.pack("<I", 1) + struct.pack("<Q", 0) + struct.pack("<i", 0) + struct.pack("<8I", *range(8))
    p = tmp_path / "bsp0.reach"
    p.write_bytes(head + struct.pack("<2I", 2, 1) + bricks.tobytes() + b"\x00")
    c = nat.read_reach(p)
    assert np.allclose(c, [[1.5, 2.5, 3.5], [-4.5, 0.5, 7.5]])
    assert np.allclose(nat.read_reach(p, walk_only=True), [[1.5, 2.5, 3.5]])
    p.write_bytes(b"XXXX" + p.read_bytes()[4:])
    assert nat.read_reach(p) is None


def test_spacing_mult():
    m = nat.spacing_mult(np.array([0.0, 3.0, 16.5, 30.0, 100.0, 0.0]),
                         np.array([True, True, True, True, True, False]), 3.0, 30.0, 6.0, 6.0)
    assert m[0] == 1.0 and m[1] == 1.0 and 1.0 < m[2] < 6.0 and m[3] == 6.0 and m[4] == 6.0 and m[5] == 6.0


def test_soft_clamp_keeps_within_caps():
    d = np.linspace(-2, 2, 101)
    c = nat.soft_clamp(d, -0.16, 0.05)
    assert c.min() >= -0.16 and c.max() <= 0.05
    assert np.all(np.diff(c) >= 0)


def test_rock_relief_band_limited():
    rng = np.random.default_rng(2)
    n = 400
    p = rng.random((n, 3)) * 10
    nrm = np.tile([0.0, -1.0, 0.0], (n, 1))
    uv = rng.random((n, 2))
    img = rng.random((64, 256, 3))
    tb = nat.TextureBands(img)
    texel = np.tile([34.0 / 256, 17.0 / 64], (n, 1))
    r = {}
    fine, _ = nat.rock_relief(p, nrm, uv, texel, np.full(n, 0.05), np.ones(n), r, tb)
    coarse, _ = nat.rock_relief(p, nrm, uv, texel, np.full(n, 2.0), np.ones(n), r, tb)
    # a coarse mesh is asked for less relief than a fine one, and never more than the recipe's amplitudes
    assert np.abs(coarse).mean() < np.abs(fine).mean()
    assert np.abs(fine).max() < 0.5


def test_recipes_are_the_kits_with_b30a_ledges_only():
    """the builder is the kit's (prism-v4-remodel-floor) again: its recipes, rock within its caps, and the scan
    ledges (wg2_ledges.py) on b30a's rock alone until one option is picked"""
    rec = json.load(open(WORLD / "wg2_recipes.json"))
    seen = 0
    for name, level in rec.items():
        if name == "_doc":
            continue
        for cname, c in level["classes"].items():
            assert "scan" not in c, (name, cname)
            if c["kind"] == "rock":
                seen += 1
                assert c["cap_out"] <= 0.1, (name, cname)
                assert ("ledges" in c) == (name == "levels\\b30\\b30a"), (name, cname)
    assert seen >= 4


@pytest.mark.skipif(nat.geometry is None, reason="needs Blender's mathutils")
def test_tessellation_crack_free_and_well_shaped():
    n = 6
    xs = np.linspace(0, 5, n + 1)
    V = np.array([[x, y, 0.3 * math.sin(x) * math.cos(y)] for y in xs for x in xs])
    T = []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            T += [[a, a + 1, a + n + 2], [a, a + n + 2, a + n + 1]]
    T = np.array(T)
    sp = np.where(np.arange(len(T)) % 3 == 0, np.inf, 0.21)
    V2, T2, par, _, _, fans = nat.tessellate(V, T, sp, log=lambda *a: None)
    E = np.sort(np.concatenate([T2[:, [0, 1]], T2[:, [1, 2]], T2[:, [2, 0]]]), 1)
    u, c = np.unique(E, axis=0, return_counts=True)
    P = V2[u[c == 1]]
    border = ((np.abs(P[..., 0]) < 1e-9) | (np.abs(P[..., 0] - 5) < 1e-9) | (np.abs(P[..., 1]) < 1e-9) |
              (np.abs(P[..., 1] - 5) < 1e-9)).all(1)
    assert border.all() and c.max() == 2 and fans == 0


def test_limit_slope_caps_each_edge():
    X = np.array([[0.0, 0, 0], [0.1, 0, 0], [0.2, 0, 0], [0.3, 0, 0]])
    E = np.array([[0, 1], [1, 2], [2, 3]])
    d = np.array([0.0, 0.5, 0.0, 0.0])
    mov = np.array([False, True, True, True])
    out = nat.limit_slope(d, X, E, mov, 0.8, 50)
    assert out[0] == 0.0
    assert np.all(np.abs(np.diff(out)) <= 0.8 * 0.1 + 1e-9)


def _made_up_scan(res=128, seed=3):
    """a made-up periodic height field: level strata (rows) plus blocky cells, mean 0, deviation 1"""
    rng = np.random.default_rng(seed)
    y = np.arange(res)[:, None] / res
    strata = np.sin(2 * np.pi * 4 * y) + 0.5 * np.sin(2 * np.pi * 9 * y + 1.0)
    cells = np.kron(rng.standard_normal((8, 8)), np.ones((res // 8, res // 8)))
    h = strata + 0.6 * cells + 0.3 * rng.standard_normal((res, res))
    return (h - h.mean()) / h.std()


def test_scan_field_band_limited_and_periodic():
    f = nat.ScanField(_made_up_scan(), levels=5)
    uv = np.random.default_rng(1).random((4000, 2))
    fine = f.sample(uv, np.full(len(uv), 0.002))
    coarse = f.sample(uv, np.full(len(uv), 0.05))
    none = f.sample(uv, np.full(len(uv), 5.0))
    # (a coarser mesh carries less of the scan; a mesh coarser than its tile none of it)
    assert fine.std() > coarse.std() > 0
    assert np.abs(none).max() < 1e-9
    # (the field repeats with its tile)
    assert np.allclose(f.sample(uv + 1.0, np.full(len(uv), 0.01)), f.sample(uv, np.full(len(uv), 0.01)))


def test_scan_relief_keeps_strata_level_on_a_wall():
    nat._SCANS["made_up"] = nat.ScanField(_made_up_scan(), levels=5)
    y, z = np.meshgrid(np.linspace(0, 4, 80), np.linspace(0, 4, 80))
    p = np.stack([np.zeros(y.size), y.ravel(), z.ravel()], 1)
    n = np.tile([1.0, 0.0, 0.0], (len(p), 1))
    h = np.full(len(p), 0.02)
    r = {"scan": [{"name": "made_up", "tile_wu": 2.0}], "scan_amp": 0.05, "scan_bias": 0.0}
    d, parts = nat.rock_relief(p, n, None, None, h, np.ones(len(p)), r, None)
    g = d.reshape(80, 80)
    # (the strata run level: the relief varies more up the wall than along it)
    assert np.abs(np.diff(g, axis=0)).mean() > 1.3 * np.abs(np.diff(g, axis=1)).mean()
    assert "scan" in parts and "strata" not in parts
    # (deterministic)
    d2, _ = nat.rock_relief(p, n, None, None, h, np.ones(len(p)), r, None)
    assert np.array_equal(d, d2)
    del nat._SCANS["made_up"]


def test_scans_manifest_is_cc0_with_hashes():
    m = json.loads((WORLD / "wg2_scans.json").read_text())
    for name, s in m["scans"].items():
        assert s["licence"] == "CC0-1.0"
        assert len(s["height"]["sha256"]) == 64 and s["height"]["url"].startswith("https://")
        assert s["size_m"] > 0
