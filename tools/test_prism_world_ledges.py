"""fork: the scan ledges on a rock face (tools/prism_packs/art/world_geo/wg2_ledges.py) on a made-up wall and scan
field, without Blender or the game's data: the layer profile continuous across a riser and hard only over the
riser's height, the vertices next to a riser slid onto its two edges, no slide turning a triangle or leaving its stock
triangle, the layers faded where the mesh is too coarse, and the whole within the recipe's caps."""

import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
WORLD = ROOT / "tools" / "prism_packs" / "art" / "world_geo"
sys.path.insert(0, str(WORLD))

import wg2_ledges as L  # noqa: E402
import wg2_natural as nat  # noqa: E402


def field():
    rng = np.random.default_rng(3)
    a = nat.gauss_wrap(rng.standard_normal((128, 128)), 4.0)
    return nat.ScanField((a - a.mean()) / a.std(), levels=5)


def wall(h, w=8.0, ht=6.0, jitter=0.2):
    nx, nz = int(w / h) + 1, int(ht / h) + 1
    gx, gz = np.meshgrid(np.linspace(0, w, nx), np.linspace(0, ht, nz))
    V = np.stack([gx.ravel(), np.zeros(gx.size), gz.ravel()], 1)
    inner = (gx.ravel() > 0) & (gx.ravel() < w) & (gz.ravel() > 0) & (gz.ravel() < ht)
    rng = np.random.default_rng(1)
    V[inner, 0] += rng.uniform(-jitter, jitter, inner.sum()) * h
    V[inner, 2] += rng.uniform(-jitter, jitter, inner.sum()) * h
    idx = np.arange(nx * nz).reshape(nz, nx)
    T = []
    for j in range(nz - 1):
        for i in range(nx - 1):
            a, b, c, d = idx[j, i], idx[j, i + 1], idx[j + 1, i], idx[j + 1, i + 1]
            T += [(a, b, d), (a, d, c)]
    return V, np.array(T), inner


def test_profile_continuous_and_hard_only_over_the_riser():
    q = L.params({"ledges": {}})
    p = np.zeros((4000, 3))
    ph = np.linspace(0.0, 4.0, 4000, endpoint=False)
    d = L.profile(ph, p, q)
    step = np.abs(np.diff(d))
    wr = L.riser_width(q)
    t1 = ph[1:] - np.floor(ph[1:])
    t0 = ph[:-1] - np.floor(ph[:-1])
    in_riser = (t1 >= 1.0 - wr - 1e-3) | (t0 >= 1.0 - wr - 1e-3)
    # (outside the risers d changes only by the lean; inside, the step to the next layer's foot)
    assert step[~in_riser].max() < 2 * q["lean"] * (ph[1] - ph[0]) + 1e-9
    assert step.max() < 0.05
    assert np.abs(d).max() < q["setback"] / 2 + q["setback_drift"] + q["lean"]


def test_snapped_vertices_sit_on_riser_edges_and_no_triangle_turns():
    q = L.params({"ledges": {}})
    f = field()
    V, T, inner = wall(0.22)
    n = np.tile([0.0, -1.0, 0.0], (len(V), 1))
    up = np.tile([0.0, 0.0, 1.0], (len(V), 1))
    h = np.full(len(V), 0.22)
    slide = L.snap_targets(V, up, h, inner, q, f, n)
    assert (slide != 0).sum() > len(V) * 0.1
    assert np.abs(slide).max() <= q["snap_band"] * 0.22 * 1.2 + 1e-9
    big = np.array([[-100.0, 0, -100], [100.0, 0, -100], [0.0, 0, 100]])
    tri = tuple(np.tile(big[k], (len(V), 1)) for k in range(3))
    V2, moved = L.slide_vertices(V, T, np.arange(len(T)), up, slide, tri)
    assert moved.sum() > 0 and not moved[~inner].any()
    ph, _ = L.phase(V2[moved], n[moved], h[moved], q, f)
    t = ph - np.floor(ph)
    wr = L.riser_width(q)
    on_edge = (np.abs(t - (1.0 - wr)) < 0.02) | (t < 0.02) | (t > 0.98)
    assert on_edge.mean() > 0.9
    n0 = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    n1 = np.cross(V2[T[:, 1]] - V2[T[:, 0]], V2[T[:, 2]] - V2[T[:, 0]])
    assert ((n0 * n1).sum(1) > 0).all()


def test_slide_stays_in_its_stock_triangle():
    q = L.params({"ledges": {}})
    f = field()
    V, T, inner = wall(0.22, w=2.0, ht=2.0)
    n = np.tile([0.0, -1.0, 0.0], (len(V), 1))
    up = np.tile([0.0, 0.0, 1.0], (len(V), 1))
    h = np.full(len(V), 0.22)
    slide = np.where(inner, 0.5, 0.0)
    tri = (np.tile([-0.1, 0, -0.1], (len(V), 1)), np.tile([2.1, 0, -0.1], (len(V), 1)), np.tile([1.0, 0, 2.3], (len(V), 1)))
    V2, moved = L.slide_vertices(V, T, np.arange(len(T)), up, slide, tri)
    bc = L.barycentric(V2[moved], *(t[moved] for t in tri))
    assert (bc.min(1) >= 1e-4).all()


def test_layers_fade_on_a_coarse_mesh_and_stay_within_the_caps():
    q = L.params({"ledges": {}})
    f = field()
    for h, lo in ((0.22, 0.04), (q["layer"] / 1.2, 0.0)):
        V, T, inner = wall(0.22)
        n = np.tile([0.0, -1.0, 0.0], (len(V), 1))
        d, parts = L.relief(V, n, np.full(len(V), h), np.ones(len(V)), q, f)
        if lo > 0:
            assert d.std() > lo and parts["fade"].min() == 1.0
        else:
            assert parts["fade"].max() == 0.0 and np.abs(d).max() < 4 * q["scan_amp"] * 2
        # (the builder's soft caps, cap_out 0.08 and cap_in 0.5, take the rest)
        assert d.max() < 0.12 and d.min() > -0.5


def test_round3_features_are_off_by_default_and_presets_switch_them_on():
    q = L.params({"ledges": {}})
    assert q["break"] == 0.0 and q["block"] == 0.0 and q["fine_amp"] == 0.0
    f = field()
    V, T, inner = wall(0.15, w=10.0, ht=8.0)
    n = np.tile([0.0, -1.0, 0.0], (len(V), 1))
    h = np.full(len(V), 0.15)
    d0, p0 = L.relief(V, n, h, np.ones(len(V)), q, f)
    assert "fine" not in p0
    for pre in ("a2", "a3"):
        qp = L.params({"ledges": {"preset": pre, "riser": 0.12}})
        assert qp["riser"] == 0.12 and qp["layer"] == L.PRESETS[pre]["layer"]
        d, parts = L.relief(V, n, h, np.ones(len(V)), qp, f)
        # deeper steps than round 2's A, and still mostly behind the collision surface
        assert d.std() > 1.3 * d0.std()
        assert np.percentile(d, 95) < 0.12 and np.percentile(d, 50) < 0.0
        assert parts["fine"].std() > 0.0


def test_blocks_make_vertical_breaks_inside_a_layer():
    q = L.params({"ledges": {"preset": "a2"}})
    xs = np.linspace(0.0, 30.0, 3000)
    k = np.full(len(xs), 3, np.int64)
    p = np.stack([xs, np.zeros_like(xs), np.full_like(xs, 6.5)], 1)
    off = L.block_offset(p, k, q)
    jumps = np.abs(np.diff(off)) > 1e-6
    # (a few cells set in or out along 30 wu; elsewhere the layer's own set-back)
    assert 2 <= jumps.sum() <= 40
    assert np.abs(off).max() <= q["block_setback"] + 1e-9
    # another layer cuts other blocks
    off2 = L.block_offset(p, k + 1, q)
    assert not np.allclose(off, off2)


def test_breaks_jog_the_bedding_by_at_most_half_the_break():
    q = L.params({"ledges": {"preset": "a3"}})
    rng = np.random.default_rng(2)
    p = rng.uniform(-20, 20, (5000, 3))
    b = L.breaks(p, q)
    assert np.abs(b).max() <= q["break"] / 2 + 1e-9
    assert (np.abs(b) > 0.45 * q["break"]).mean() > 0.4  # (mostly a whole jog either way, short transitions)


def _min_angle(V, T):
    out = []
    for k in range(3):
        a, b, c = V[T[:, k]], V[T[:, (k + 1) % 3]], V[T[:, (k + 2) % 3]]
        u, v = b - a, c - a
        cs = (u * v).sum(1) / (np.linalg.norm(u, axis=1) * np.linalg.norm(v, axis=1))
        out.append(np.degrees(np.arccos(np.clip(cs, -1, 1))))
    return np.min(out, 0)


def test_round4_cut_puts_vertices_on_the_riser_lines_and_keeps_the_surface():
    q = L.params({"ledges": {"preset": "a4"}})
    assert q["cut"] and q["block_soft"] > 0 and q["chunk_amp"] > 0
    assert not L.params({"ledges": {"preset": "a3"}}).get("cut")
    f = field()
    V, T, inner = wall(0.15, w=10.0, ht=8.0)
    n = np.tile([0.0, -1.0, 0.0], (len(V), 1))
    h = np.full(len(V), 0.15)
    V2, T2, par, lerps, rep = L.cut_risers(V, T, n, h, inner, np.ones(len(T), bool), q, f)
    assert rep["top"]["cut"] > 50 and rep["foot"]["cut"] > 50
    # every new vertex on a line (phase k + 1 - riser or k + 1), on its edge, between two free vertices
    nn = len(V2) - len(V)
    ph, _ = L.phase(V2[len(V):], n[:1].repeat(nn, 0), np.full(nn, 0.15), q, f)
    fr = ph - np.floor(ph)
    wr = L.riser_width(q)
    off = np.minimum.reduce([np.abs(fr - (1 - wr)), fr, 1 - fr])
    assert off.max() < 2e-3
    # the same surface: area kept, no face turned, each face inside its parent
    def nrm(X, TT):
        return np.cross(X[TT[:, 1]] - X[TT[:, 0]], X[TT[:, 2]] - X[TT[:, 0]])
    a0, a2 = nrm(V, T), nrm(V2, T2)
    assert abs(np.linalg.norm(a2, axis=1).sum() - np.linalg.norm(a0, axis=1).sum()) < 1e-6
    assert ((a2 * a0[par]).sum(1) > 0).all()
    # no needle made: the cut faces' smallest angle not under the lattice's own by much
    assert np.percentile(_min_angle(V2, T2), 1) > 8.0
    # the lerps rebuild the new vertices
    X = V.copy()
    for a_, b_, t_ in lerps:
        X = np.concatenate([X, X[a_] + (X[b_] - X[a_]) * t_[:, None]])
    assert np.allclose(X, V2)


def test_round4_soft_blocks_and_chunks_are_continuous():
    q = L.params({"ledges": {"preset": "a4"}})
    xs = np.linspace(0.0, 30.0, 30001)
    k = np.full(len(xs), 3, np.int64)
    p = np.stack([xs, np.zeros_like(xs), np.full_like(xs, 6.5)], 1)
    off = L.block_offset(p, k, q)
    # (no jump: over 1 mm at most a hundredth of the set-back; round 3's walls jumped by the whole of it)
    assert np.abs(np.diff(off)).max() < 0.01 * q["block_setback"] + 1e-6
    assert np.abs(off).max() <= q["block_setback"] + 1e-9 and off.std() > 0.01
    n = np.tile([0.0, -1.0, 0.0], (len(xs), 1))
    c = L.chunks(p, n, q)
    assert np.abs(np.diff(c)).max() < 0.002 and 0.01 < c.std() < 0.08
