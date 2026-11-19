"""PRISM Path Traced (display.lighting "pathtraced", M3; docs/prism_pathtraced.md): its shaders' SPIR-V still their
GLSL's, and its arithmetic (tools/prism_pt_model.py, the GLSL's as written) checked on the CPU: the lights'
resampled importance unbiased over its mixed domains, the reference's running sums and its noise's estimate, its
stopping rule, the temporal history's convergence and its hold on a change, the a-trous step's weights. No GPU, no
game data."""
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prism_pt_model as model  # noqa: E402

GLSL = ROOT / "port/linux/vulkan/xgpu_vulkan_pt.glsl"


@pytest.mark.skipif(not shutil.which("glslangValidator") or not shutil.which("spirv-val"),
                    reason="glslangValidator and spirv-val are not on PATH")
def test_shaders_are_their_glsl():
    subprocess.run([sys.executable, "-B", str(ROOT / "tools/vulkan_pt_shaders.py"), "--check"], check=True)


def test_the_glsl_has_every_pass_and_shares_the_bakes_scene():
    text = GLSL.read_text()
    for name in ("light", "temporal", "fix", "atrous", "composite", "reference", "reference_stats", "lamps", "cache"):
        assert f"//@pass {name}\n" in text
    # (the bake's own functions, not copies: the scene traced and the bake made by one estimator)
    for used in ("lamp_pick(", "lamp_direction(", "lamp_visible(", "domes_at(", "fixture_light(", "fixtures_ris(",
                 "sky_along(", "ambient()", "glow_of(", "albedo_of(", "sun_at("):
        assert used in text
    for copied in ("uint lamp_pick(", "uint list_pick(", "bool lamp_visible(", "bool lamp_direction(",
                   "vec3 domes_at(", "vec3 sky_along("):
        assert copied not in text


def _scene(rng):
    """a made-up point's lights: 5 lamps of 4 sample points each (a lamp picked by a mixture of a list's weights and
    by power, as the grid's lamp_pick; a point by its own density), 3 fixtures, 2 dynamic lights, and a visibility
    that hides some"""
    lamp_light = rng.uniform(0.0, 2.0, (5, 4, 3))
    point_pdf = rng.dirichlet(np.ones(4), 5)
    list_weights = np.array([0.5, 0.3, 0.2, 0.0, 0.0])
    power = rng.uniform(0.2, 1.0, 5)
    power /= power.sum()
    pick = 0.7 * list_weights + 0.3 * power
    hidden_lamp = rng.random((5, 4)) < 0.3
    fixtures = [rng.uniform(0.0, 1.0, 3) for _ in range(3)]
    dynamics = [rng.uniform(0.0, 0.5, 3), np.zeros(3)]
    hidden_fixture = [False, True, False]
    # (the exact shadowed light: each lamp point's integrand over its density's measure, here a sum over the points)
    exact = np.zeros(3)
    for lamp in range(5):
        for k in range(4):
            if not hidden_lamp[lamp, k]:
                exact += lamp_light[lamp, k]
    for i, f in enumerate(fixtures):
        if not hidden_fixture[i]:
            exact += f
    exact += dynamics[0]

    def lamp_pick(r):
        u = r.random()
        if u < 0.7:
            lamp = int(np.searchsorted(np.cumsum(list_weights), r.random() * list_weights.sum(), side="right"))
        else:
            lamp = int(np.searchsorted(np.cumsum(power), r.random(), side="right"))
        lamp = min(lamp, 4)
        return lamp, pick[lamp]

    def lamp_sample(r, lamp):
        k = min(int(np.searchsorted(np.cumsum(point_pdf[lamp]), r.random(), side="right")), 3)
        return lamp_light[lamp, k], 1.0 / point_pdf[lamp, k], (lamp, k)

    def visible(kind, which):
        if kind == "lamp":
            return not hidden_lamp[which]
        if kind == "fixture":
            return not hidden_fixture[which]
        return True

    return exact, lamp_pick, lamp_sample, fixtures, dynamics, visible


@pytest.mark.parametrize("seen", [False, True])
@pytest.mark.parametrize("lamp_count,fixture_count,dynamic_count", [(8, 8, 4), (2, 1, 1), (16, 2, 2)])
def test_the_lights_resampling_is_unbiased(lamp_count, fixture_count, dynamic_count, seen):
    rng = np.random.default_rng(7)
    exact, lamp_pick, lamp_sample, fixtures, dynamics, visible = _scene(rng)
    trials = 40000
    total = np.zeros(3)
    for _ in range(trials):
        total += model.ris_lights(rng, lamp_count, lamp_pick, lamp_sample, fixtures, fixture_count, dynamics,
                                  dynamic_count, visible, seen=seen)
    mean = total / trials
    assert np.allclose(mean, exact, rtol=0.03), (mean, exact)


def test_the_reference_sums_are_the_running_means():
    rng = np.random.default_rng(3)
    samples = rng.gamma(2.0, 0.5, (500, 3))
    mean_d, mean_y, m2_y = np.zeros(3), 0.0, 0.0
    for n, ld in enumerate(samples, start=1):
        mean_d, mean_y, m2_y = model.welford_frame(mean_d, mean_y, m2_y, n, ld, float(model.luma(ld)))
    y = model.luma(samples)
    assert np.allclose(mean_d, samples.mean(0), rtol=1e-9)
    assert mean_y == pytest.approx(y.mean(), rel=1e-9)
    assert m2_y / (len(y) - 1) == pytest.approx(y.var(ddof=1), rel=1e-9)


def test_the_reference_noise_is_the_means_error():
    """the estimate (each pixel's variance of its mean over the means squared) against the error the means truly
    have, over many runs of the same sum"""
    rng = np.random.default_rng(11)
    pixels, frames, runs = 64, 200, 300
    truth = rng.uniform(0.1, 2.0, pixels)
    spread = truth * rng.uniform(0.5, 3.0, pixels)
    estimates, errors = [], []
    for _ in range(runs):
        x = truth + spread * rng.standard_normal((frames, pixels))
        means = x.mean(0)
        m2s = ((x - means) ** 2).sum(0)
        estimates.append(model.reference_noise(means, m2s, frames))
        errors.append(((means - truth) ** 2).sum() / (truth ** 2).sum())
    assert np.mean(estimates) == pytest.approx(np.mean(errors), rel=0.15)


def test_the_reference_stops_on_its_noise_or_its_frames():
    assert model.reference_stops(0.0009, 256, 4096, 256)
    assert not model.reference_stops(0.0009, 224, 4096, 224)
    assert not model.reference_stops(0.002, 1024, 4096, 1024)
    assert model.reference_stops(0.5, 1024, 1024, 1024)
    # (a pixel's relative variance v a frame: its mean's v / n; the frames to the bound, v / 0.001)
    rng = np.random.default_rng(5)
    v = 0.25
    n = 0
    noise = 1.0
    x = []
    while not model.reference_stops(noise, n, 1 << 20, n):
        x.append(1.0 + np.sqrt(v) * rng.standard_normal(256))
        n += 1
        if n % 32 == 0:
            a = np.array(x)
            noise = model.reference_noise(a.mean(0), ((a - a.mean(0)) ** 2).sum(0), n)
    assert 0.8 * v / model.REFERENCE_NOISE <= n <= 1.3 * v / model.REFERENCE_NOISE + 32


def test_the_gate_share_is_the_gates_bilinear_resize():
    """the share of a pixel's independent noise the gates' frames keep (metrics.load: PIL's bilinear resize to 960 x
    540), measured through PIL itself on white noise"""
    from PIL import Image
    assert model.gate_share(960) == 1.0
    assert model.gate_share(1920) == pytest.approx((2.0 * (0.375 ** 2 + 0.125 ** 2)) ** 2, rel=1e-9)
    rng = np.random.default_rng(7)
    for width, gate in ((1920, 960), (1280, 960)):
        height, gate_height = width * 9 // 16, gate * 9 // 16
        noise = rng.standard_normal((height, width)).astype(np.float32)
        small = np.asarray(Image.fromarray(noise, mode="F").resize((gate, gate_height), Image.BILINEAR))
        kept = float(small[4:-4, 4:-4].var())
        assert kept == pytest.approx(model.gate_share(width, gate), rel=0.08)


def test_the_shown_noise_estimates_the_gates_error():
    """the stopping rule's estimate (the drawn values' variance, none where clipped, by the gates' share) against the
    error a sum of the same frames truly leaves in the gates' frames (clipped at white, resized by PIL as metrics.load
    does), over many runs: within a fifth; the linear estimate, which counts the clipped pixels' noise and not the
    resize, far above it"""
    from PIL import Image
    rng = np.random.default_rng(13)
    width, height, gate = 192, 108, 96
    truth = rng.uniform(0.05, 0.9, (height, width))
    truth[:, :40] = rng.uniform(1.5, 3.0, (height, 40))
    spread = truth * rng.uniform(0.5, 4.0, (height, width))
    frames = 64

    def shown(image):
        clipped = np.minimum(image, 1.0).astype(np.float32)
        return np.asarray(Image.fromarray(clipped, mode="F").resize((gate, gate * height // width), Image.BILINEAR))

    reference = shown(truth)
    estimates, linear, errors = [], [], []
    for _ in range(60):
        x = truth + spread * rng.standard_normal((frames, height, width)) / np.sqrt(4.0)
        means = x.mean(0)
        m2s = ((x - means) ** 2).sum(0)
        clipped = means >= 1.0
        estimates.append(model.reference_noise_shown(means, m2s, frames, clipped, width=width, gate_width=gate))
        linear.append(model.reference_noise(means, m2s, frames))
        errors.append(float(((shown(means) - reference) ** 2)[2:-2, 2:-2].mean() / (reference ** 2)[2:-2, 2:-2].mean()))
    assert np.mean(estimates) == pytest.approx(np.mean(errors), rel=0.2)
    assert np.mean(linear) > 5.0 * np.mean(errors)


def test_a_normal_turned_from_the_eye_is_mirrored_and_its_reflection_never_negative():
    rng = np.random.default_rng(17)
    eye = np.array([0.0, 0.0, 1.0])
    for _ in range(200):
        n = rng.standard_normal(3)
        n /= np.linalg.norm(n)
        m = model.mirror_normal(n, eye)
        assert m @ eye >= -1e-12
        assert np.linalg.norm(m) == pytest.approx(1.0)
        if n @ eye < 0.0:
            # (its sideways part kept)
            assert np.allclose(m[:2] / np.linalg.norm(m[:2]), n[:2] / np.linalg.norm(n[:2]))
        else:
            assert np.allclose(m, n)
    # (the lobe's weight: never negative, 0 with the eye below the surface)
    for v_z in (-0.3, -1e-5, 0.0, 1e-5):
        assert model.ggx_weight(v_z, 0.5, 0.3) == 0.0
    for v_z in rng.uniform(1e-3, 1.0, 50):
        assert model.ggx_weight(v_z, float(rng.uniform(1e-3, 1.0)), float(rng.uniform(0.05, 1.0))) >= 0.0


@pytest.mark.parametrize("axis,cos_most", [((0.3, 0.2, 0.93), 0.95), ((0.9, 0.0, 0.2), 0.6), ((0.0, 0.0, 1.0), 0.2)])
def test_the_domes_by_both_directions_are_the_cosine_paths_light(axis, cos_most):
    """a dome (sky_along's radiance r pi / its cone's solid angle within the cone) seen by a surface facing z: the
    cosine paths' estimate (the sky where they leave), the next-event estimate (domes_at: a direction of the cone, r
    by its cosine, none below the surface) and the reference's at a hit (domes_mis_at and domes_missed: both, by the
    power heuristic) have one expectation; the power heuristic's noise is never far above the better one's"""
    rng = np.random.default_rng(19)
    axis = np.asarray(axis, float)
    axis /= np.linalg.norm(axis)
    r = 0.7
    solid = 2.0 * np.pi * (1.0 - cos_most)
    count = 400_000
    u, a = rng.random(count), rng.random(count) * 2.0 * np.pi
    d = np.stack([np.sqrt(u) * np.cos(a), np.sqrt(u) * np.sin(a), np.sqrt(1.0 - u)], 1)
    paths = np.where(d @ axis >= cos_most, r * np.pi / solid, 0.0)
    c = model.cone_sample(rng, axis, cos_most, count)[:, 2]
    nee = np.where(c > 0.0, r * c, 0.0)
    assert nee.mean() == pytest.approx(paths.mean(), rel=0.02, abs=1e-4)
    pl = 1.0 / solid
    pcl = np.maximum(c, 0.0) / np.pi
    nee_weighted = np.where(c > 0.0, r * c * pl * pl / (pl * pl + pcl * pcl), 0.0)
    pc = d[:, 2] / np.pi
    missed = np.where(d @ axis >= cos_most, r * np.pi / solid * pc * pc / (pl * pl + pc * pc), 0.0)
    both = nee_weighted + missed
    assert both.mean() == pytest.approx(paths.mean(), rel=0.02, abs=1e-4)
    assert both.std() <= 1.1 * min(nee.std(), paths.std())


def test_the_temporal_history_keeps_a_sparse_light():
    """a light a pixel sees in one frame of twenty (a lamp through a gap): the slow history's mean is its mean, not
    held to the frames that miss it"""
    rng = np.random.default_rng(29)
    h = None
    values = []
    for _ in range(3000):
        h = model.temporal_step(h, 20.0 if rng.random() < 0.05 else 0.0)
        values.append(h["slow"])
    assert np.mean(values[500:]) == pytest.approx(1.0, rel=0.15)


def test_the_temporal_history_converges_and_follows_a_change():
    rng = np.random.default_rng(13)
    h = None
    values = []
    for _ in range(400):
        h = model.temporal_step(h, 1.0 + 0.5 * rng.standard_normal())
        values.append(h["slow"])
    tail = np.array(values[100:])
    # (a slow history of up to 32 frames: its noise about a 32nd of the samples' variance, not more than triple: the
    # hold cuts the history now and then)
    assert tail.var() < 3.0 * 0.25 / 32.0
    assert abs(tail.mean() - 1.0) < 0.05
    # (the light doubles, a noiseless step: the fast history's hold, by the samples' spread the slow moments know,
    # brings the slow one most of the way in 16 frames and there in 24, where the slow history alone (32 frames)
    # would still be at 1.4 after 16)
    for k in range(16):
        h = model.temporal_step(h, 2.0)
    assert h["slow"] > 1.7
    for k in range(8):
        h = model.temporal_step(h, 2.0)
    assert h["slow"] > 1.8
    # (and a noisy step: most of the way within 32 frames)
    h = None
    for _ in range(200):
        h = model.temporal_step(h, 1.0 + 0.5 * rng.standard_normal())
    for _ in range(32):
        h = model.temporal_step(h, 2.0 + 0.5 * rng.standard_normal())
    assert h["slow"] > 1.7


def test_the_atrous_weights():
    assert sum(model.KERNEL[abs(d)] for d in range(-1, 2)) == pytest.approx(1.0)
    assert model.atrous_weight(0, 0, 0.0, 1.0, 1.0, False, 0.0, 1.0) == 0.0
    assert model.atrous_weight(1, 1, 0.0, 1.0, 0.0, True, 0.0, 1.0) == 0.0
    assert model.atrous_weight(0, 0, 5.0, 1.0, 1.0, True, 0.0, 1.0) < model.atrous_weight(0, 0, 0.0, 1.0, 1.0, True, 0.0,
                                                                                          1.0) * 0.01


def test_the_atrous_step_keeps_an_instance_edge_and_smooths_noise():
    rng = np.random.default_rng(17)
    h, w = 12, 16
    instances = np.zeros((h, w), int)
    instances[:, 8:] = 1
    clean = np.where(instances == 0, 0.2, 1.0)
    values = clean + 0.05 * rng.standard_normal((h, w))
    variance = np.full((h, w), 0.05 ** 2)
    out = values
    var = variance
    for step in range(5):
        out, var = model.atrous_step(out, instances, var, 1 << step)
    # (no light across the edge: each side's mean its own)
    assert abs(out[:, :8].mean() - 0.2) < 0.02
    assert abs(out[:, 8:].mean() - 1.0) < 0.02
    # (the noise down)
    assert (out - clean)[:, 2:6].std() < 0.5 * (values - clean)[:, 2:6].std()


def test_the_grids_search_by_halves_is_the_linear_pick():
    """the grid's list pick by halves (the bake's list_pick since 0ec8b7e48) picks the entry a linear pick picks,
    and its share is the entry's weight over the list's"""
    rng = np.random.default_rng(23)
    for _ in range(200):
        n = int(rng.integers(1, 65))
        weights = rng.uniform(0.0, 1.0, n) * (rng.random(n) > 0.2)
        if weights.sum() <= 0.0:
            weights[0] = 1.0
        cumulative = np.cumsum(weights)
        for u in rng.random(8):
            k, share = model.list_search(cumulative, u)
            assert k == model.list_pick_linear(cumulative, u)
            assert share == pytest.approx(weights[k] / weights.sum())


@pytest.mark.parametrize("guide_kind", ["right", "wrong", "blind"])
def test_the_guided_bounce_is_unbiased_whatever_the_guide(guide_kind):
    """the reference's bounce resampled by the bake's say: its mean is the light's whether the guide is right, wrong
    (a lit place it calls a tenth as bright, a dark one it calls as bright) or blind (0 everywhere), and with a right
    guide its noise is far less than the plain bounce's (8 candidates: a fifth of it)"""
    rng = np.random.default_rng(31)
    # (directions 0..1; the light: a small bright window (0.9 to 0.95) on a dark wall)
    def light(r, d):
        return (40.0 if 0.9 <= d < 0.95 else 0.01) * (0.5 + r.random())
    exact = 0.05 * 40.0 + 0.95 * 0.01
    guides = {"right": lambda d: 40.0 if 0.9 <= d < 0.95 else 0.01,
              "wrong": lambda d: 4.0 if 0.2 <= d < 0.25 or 0.9 <= d < 0.95 else 0.01,
              "blind": lambda d: 0.0}
    samples = [model.guided_bounce(rng, lambda r: r.random(), light, guides[guide_kind]) for _ in range(40000)]
    assert np.mean(samples) == pytest.approx(exact, rel=0.05)
    if guide_kind == "right":
        plain = [light(rng, rng.random()) for _ in range(40000)]
        assert np.var(samples) < 0.2 * np.var(plain)


def _lamps(rng, count):
    """made-up lamp triangles (the bake's 16 floats: corners, radiance, area) scattered in a room"""
    lamps = np.zeros((count, 16))
    for k in range(count):
        a0 = rng.uniform(-10.0, 10.0, 3)
        e1, e2 = rng.normal(0.0, 0.4, 3), rng.normal(0.0, 0.4, 3)
        lamps[k, 0:3], lamps[k, 3:6], lamps[k, 6:9] = a0, a0 + e1, a0 + e2
        lamps[k, 9:12] = rng.uniform(0.2, 20.0, 3)
        lamps[k, 12] = 0.5 * np.linalg.norm(np.cross(e1, e2))
    return lamps


def test_the_light_tree_is_a_distribution_that_misses_no_lamp_that_lights_a_point():
    """the reference's light tree: at any point its lamps' probabilities sum to 1, its pick's probability is the one
    its walk up from the leaf finds, and every lamp whose triangle a point is in front of and whose corners are not
    all below the point's surface has a probability above nought (the bounds hold its light)"""
    rng = np.random.default_rng(23)
    lamps = _lamps(rng, 37)
    nodes, leaf = model.tree_build(lamps)
    assert len(nodes) == 2 * len(lamps) - 1
    for _ in range(40):
        p = rng.uniform(-11.0, 11.0, 3)
        n = rng.standard_normal(3)
        n /= np.linalg.norm(n)
        probabilities = np.array([model.tree_probability(nodes, leaf, k, p, n) for k in range(len(lamps))])
        assert probabilities.sum() == pytest.approx(1.0, abs=1e-9)
        for _ in range(5):
            lamp, probability = model.tree_pick(rng, nodes, p, n)
            assert probability == pytest.approx(probabilities[lamp], rel=1e-9)
        for k, lamp in enumerate(lamps):
            corners = lamp[:9].reshape(3, 3)
            ne = np.cross(corners[1] - corners[0], corners[2] - corners[0])
            ne /= np.linalg.norm(ne)
            in_front = ne @ (p - corners[0]) > 1e-6
            above = ((corners - p) @ n > 0.0).any()
            if in_front and above:
                assert probabilities[k] > 0.0


def test_the_references_lamp_mixture_is_unbiased():
    """a lamp from the grid's mixture or the tree, half and half, its probability the two's mean: the estimate of a
    sum over the lamps (each one's light at the point) is the sum, with a grid that misses the brightest lamp as the
    grid's lists can"""
    rng = np.random.default_rng(29)
    lamps = _lamps(rng, 25)
    nodes, leaf = model.tree_build(lamps)
    p, n = np.array([0.5, -1.0, 2.0]), np.array([0.0, 0.0, 1.0])
    light = np.array([model.tree_importance(nodes, int(leaf[k]), p, n) for k in range(len(lamps))])
    light *= rng.uniform(0.3, 1.0, len(lamps))
    grid = rng.uniform(0.0, 1.0, len(lamps))
    grid[int(np.argmax(light))] = 0.0
    grid = 0.95 * grid / grid.sum() + 0.05 * lamps[:, 12] / lamps[:, 12].sum()
    mixed = 0.5 * grid + 0.5 * np.array([model.tree_probability(nodes, leaf, k, p, n) for k in range(len(lamps))])
    # (the exact expectation and spread of one sample, and a simulation of the picks themselves)
    assert (mixed * light / mixed).sum() == pytest.approx(light.sum())
    spread = np.sqrt(max((light * light / mixed).sum() - light.sum() ** 2, 0.0))
    count = 20_000
    estimates = np.empty(count)
    for i in range(count):
        if rng.random() < 0.5:
            k = int(rng.choice(len(lamps), p=grid))
        else:
            k, _ = model.tree_pick(rng, nodes, p, n)
        estimates[i] = light[k] / mixed[k]
    assert abs(estimates.mean() - light.sum()) <= 4.0 * spread / np.sqrt(count)
    # (and far quieter than the grid alone, which misses the brightest lamp but by power)
    grid_alone = np.sqrt(max((light * light / grid).sum() - light.sum() ** 2, 0.0))
    assert spread < 0.5 * grid_alone


def test_the_grids_probability_mirrors_lamp_pick():
    """grid_probability (the reference's mixture's) is lamp_pick's own arithmetic, line for line, so that the mixture's
    probability is the grid's: the bake's lamp_pick must still read as it did when grid_probability was written"""
    bake = (ROOT / "port/linux/vulkan/xgpu_vulkan_bake_glsl.inc").read_text()
    pt = GLSL.read_text()
    for line in ("float share_seen = seen > 0u && !term_off(32u) ? GRID_SEEN : 0.0;",
                 "float share_near = near > 0u && !term_off(32u) ? (seen > 0u ? GRID_NEAR : GRID_SEEN + GRID_NEAR) : 0.0;",
                 "uint near_at = seen_at + 1u + 2u * seen, near = cell >= 0 ? min(g.v[near_at], 64u) : 0u;",
                 "probability += share_seen * list_probability(seen_at, seen, l);",
                 "probability += share_near * list_probability(near_at, near, l);"):
        assert line in bake and line in pt, line
    assert "probability = (1.0 - share_seen - share_near) * lamp_power(l) / max(s.lamp.x, 1.0e-20);" in bake
    assert "float probability = (1.0 - share_seen - share_near) * lamp_power(l) / max(s.lamp.x, 1.0e-20);" in pt


def test_the_fireflies_are_held_by_the_neighbours_only_while_the_history_is_short():
    """a lone bright sample among dark neighbours is held while the history is short (a moving camera's), not once it
    is long (a still camera's: a light few pixels see is the light itself); a light several neighbours see passes"""
    dark = [0.01, 0.02, 0.015, 0.0, 0.01, 0.02, 0.01, 0.0]
    assert model.firefly_cap(5.0, 0, neighbours=dark) == pytest.approx(0.08)
    assert model.firefly_cap(5.0, 8, neighbours=dark) == pytest.approx(0.08)
    assert model.firefly_cap(5.0, 20, 0.05, 0.05 ** 2 + 0.7 ** 2, neighbours=dark) == pytest.approx(5.0)
    lit = [0.01, 1.5, 0.015, 2.0, 0.01, 0.02, 1.2, 0.0]
    assert model.firefly_cap(5.0, 2, neighbours=lit) == pytest.approx(5.0)
    # (a still camera: the light of a gap a pixel's samples see one frame in ten reaches its mean once the history is
    # long, though every neighbour is dark)
    rng = np.random.default_rng(31)
    h = None
    slow = []
    for frame in range(1200):
        sample = 1.0 if rng.random() < 0.1 else 0.0
        if h is not None:
            sample = model.firefly_cap(sample, h["n"], h["m1"], h["m2"], neighbours=[0.0] * 8)
        h = model.temporal_step(h, sample)
        slow.append(h["slow"])
    assert np.mean(slow[200:]) == pytest.approx(0.1, abs=0.03)


def test_restir_gis_temporal_merge_is_unbiased_across_a_moved_point():
    """ReSTIR GI's reservoir made at the last frame's point and merged at this frame's (a pixel's footprint along the
    same floor), its sample's target taken here by the move's Jacobian: the bounce's light at this point is the
    integral the cosine bounce estimates (a lit square ceiling of varying radiance), and its noise far less"""
    rng = np.random.default_rng(37)
    normal = np.array([0.0, 0.0, 1.0])
    here, there = np.array([0.0, 0.0, 0.0]), np.array([0.15, -0.1, 0.0])

    def radiance(x):
        return np.array([1.0, 0.9, 0.8]) * (0.2 + 3.0 * np.exp(-((x[0] - 0.6) ** 2 + (x[1] + 0.3) ** 2) / 0.05))

    def bounce(p):
        u, a = rng.random(), rng.random() * 2.0 * np.pi
        d = np.array([np.sqrt(u) * np.cos(a), np.sqrt(u) * np.sin(a), np.sqrt(1.0 - u)])
        t = 1.0 / d[2]
        x = p + d * t
        if abs(x[0]) > 1.5 or abs(x[1]) > 1.5:
            return (p + d * 1.0e4, -d, np.zeros(3))
        return (x, np.array([0.0, 0.0, -1.0]), radiance(x))

    # (the exact light: (1 / pi) times the ceiling's radiance by the cosines over the distance squared)
    g = np.linspace(-1.5, 1.5, 301)
    dx = g[1] - g[0]
    exact = np.zeros(3)
    for x in g:
        for y in g:
            to = np.array([x, y, 1.0]) - here
            d2 = to @ to
            c = to[2] / np.sqrt(d2)
            exact += radiance(np.array([x, y, 1.0])) * c * c / d2 * dx * dx / np.pi
    plain, merged = [], []
    for _ in range(12_000):
        _, reservoir = model.restir_gi(rng, there, normal, bounce(there))
        for _ in range(3):
            _, reservoir = model.restir_gi(rng, there, normal, bounce(there), (there, normal, reservoir))
        light, _ = model.restir_gi(rng, here, normal, bounce(here), (there, normal, reservoir))
        merged.append(light)
        plain.append(bounce(here)[2])
    merged, plain = np.array(merged), np.array(plain)
    assert merged.mean(0) == pytest.approx(exact, rel=0.04)
    assert plain.mean(0) == pytest.approx(exact, rel=0.04)
    assert model.luma(merged).std() < 0.7 * model.luma(plain).std()


def test_the_visibility_history_is_held_to_the_frames_own_range():
    """a lit pixel among lit neighbours whose history the reprojection blurred with a shadow beside it (0.6) is lit at
    once; in a penumbra (each of the 25 samples about it lit seven times in ten) the history still accumulates to its
    light, the range spanning 0 to 1"""
    h = model.visibility_step((0.6, 0.6, 0.45, 32.0), 1.0, [1.0] * 25)
    assert h[0] == pytest.approx(1.0) and h[1] == pytest.approx(1.0)
    rng = np.random.default_rng(41)
    h = None
    slow = []
    for frame in range(2000):
        samples = (rng.random(25) < 0.7).astype(float)
        h = model.visibility_step(h, samples[12], samples)
        slow.append(h[0])
    assert np.mean(slow[100:]) == pytest.approx(0.7, abs=0.03)
    # (and it seldom snaps: a frame's 25 samples all alike one frame in thousands)
    snaps = np.mean([abs(b - a) > 0.25 for a, b in zip(slow[100:], slow[101:])])
    assert snaps < 0.005


def _vndf(rng, v, alpha, count):
    """GGX's visible normals (Heitz 2018) about z for the eye v: the reflected directions, as ggx_sample makes them"""
    vh = np.array([alpha * v[0], alpha * v[1], v[2]])
    vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    t1 = np.array([-vh[1], vh[0], 0.0]) / np.sqrt(lensq) if lensq > 0 else np.array([1.0, 0.0, 0.0])
    t2 = np.cross(vh, t1)
    u1, u2 = rng.random(count), rng.random(count)
    r, phi = np.sqrt(u1), 2.0 * np.pi * u2
    p1, p2 = r * np.cos(phi), r * np.sin(phi)
    sh = 0.5 * (1.0 + vh[2])
    p2 = (1.0 - sh) * np.sqrt(np.maximum(1.0 - p1 * p1, 0.0)) + sh * p2
    nh = p1[:, None] * t1 + p2[:, None] * t2 + np.sqrt(np.maximum(0.0, 1.0 - p1 * p1 - p2 * p2))[:, None] * vh
    m = np.stack([alpha * nh[:, 0], alpha * nh[:, 1], np.maximum(nh[:, 2], 0.0)], 1)
    m /= np.linalg.norm(m, axis=1)[:, None]
    return 2.0 * (m @ v)[:, None] * m - v, m


def test_a_glossy_lobes_lamp_by_its_own_direction_is_the_next_event_estimates_light():
    """the reference's split: a glossy lobe takes a lamp's specular light by the reflection's own direction meeting the
    lamp (ggx_sample's weight by the lamp's radiance), a rougher one by next-event estimation (a direction of the lamp
    by GGX's reflectance and cosine): one light either way (a lamp a cone of directions of uniform radiance)"""
    rng = np.random.default_rng(43)
    v = np.array([0.5, 0.0, 0.8660254])
    axis = np.array([-0.45, 0.05, 0.89])
    axis /= np.linalg.norm(axis)
    cos_most, radiance, roughness, f0 = 0.995, 9.0, 0.3, 0.04
    alpha = roughness * roughness
    a2 = alpha * alpha
    count = 2_000_000
    l, m = _vndf(rng, v, alpha, count)
    inside = (l @ axis >= cos_most) & (l[:, 2] > 0.0)
    g1v = 2.0 * v[2] / (v[2] + np.sqrt(a2 + (1.0 - a2) * v[2] ** 2))
    lz = np.maximum(l[:, 2], 1e-6)
    g1l = 2.0 * lz / (lz + np.sqrt(a2 + (1.0 - a2) * lz * lz))
    g2 = g1v * g1l / (g1v + g1l - g1v * g1l)
    fresnel = f0 + (1.0 - f0) * (1.0 - np.clip(m @ v, 0.0, 1.0)) ** 5
    by_lobe = np.where(inside, fresnel * g2 / g1v * radiance, 0.0)
    # (next-event: a direction of the lamp's cone, its BRDF by its cosine by the cone's solid angle)
    solid = 2.0 * np.pi * (1.0 - cos_most)
    d = model.cone_sample(rng, axis, cos_most, count)
    h = d + v
    h /= np.linalg.norm(h, axis=1)[:, None]
    nh, nl, vh = h[:, 2], np.maximum(d[:, 2], 0.0), np.clip(h @ v, 0.0, 1.0)
    dist = a2 / (np.pi * (nh * nh * (a2 - 1.0) + 1.0) ** 2)
    g1l_n = 2.0 * np.maximum(nl, 1e-6) / (np.maximum(nl, 1e-6) + np.sqrt(a2 + (1.0 - a2) * nl * nl))
    g2_n = g1v * g1l_n / (g1v + g1l_n - g1v * g1l_n)
    brdf = dist * g2_n * (f0 + (1.0 - f0) * (1.0 - vh) ** 5) / (4.0 * v[2] * np.maximum(nl, 1e-6))
    by_light = np.where(nl > 0.0, brdf * nl * radiance * solid, 0.0)
    assert by_lobe.mean() == pytest.approx(by_light.mean(), rel=0.03)


def test_live_candidates_own_sources_unbiased():
    """the live frames' lamp candidates (lamp_pick_live): count - 1 from the grid, the last from the tree, each
    weighed by its own source's probability: the RIS sum's expectation is the lamps' light whatever the sources, as
    long as each reaches every lamp (here the grid nearly misses lamp 3, the tree favours it)"""
    import numpy as np
    rng = np.random.default_rng(7)
    light = np.array([1.0, 0.5, 0.2, 4.0, 0.1])
    grid = np.array([0.4, 0.3, 0.25, 0.001, 0.049])
    tree = np.array([0.2, 0.1, 0.05, 0.6, 0.05])
    count, trials = 8, 200000
    sums = np.zeros(trials)
    for c in range(count):
        p = tree if c == count - 1 else grid
        pick = rng.choice(5, size=trials, p=p)
        sums += light[pick] / p[pick] / count
    mean, error = sums.mean(), sums.std() / np.sqrt(trials)
    assert abs(mean - light.sum()) < 4.0 * error + 1e-9


def _restir_di_run(shadowed, chains=4000, frames=24, seed=11):
    """a still pixel in a room of 40 lamps, 34 of them hidden from it (the light-importance grid knows nothing of
    that: its picks by power alone), 4 initial candidates a frame: each chain's frame `frames` estimate"""
    rng = np.random.default_rng(seed)
    count = 40
    light = rng.uniform(0.2, 1.0, (count, 3)) * rng.uniform(0.5, 2.0, (count, 1))
    visible = np.zeros(count, bool)
    visible[rng.choice(count, 6, replace=False)] = True
    pick = rng.uniform(0.5, 1.0, count)
    exact = light[visible].sum(axis=0)
    estimates = np.zeros((chains, 3))
    for c in range(chains):
        last = None
        for _ in range(frames):
            e, last = model.restir_di(rng, pick, light, visible, 4, last, shadowed=shadowed)
        estimates[c] = e
    return exact, estimates


def test_restir_dis_shadowed_target_is_unbiased_and_quieter_than_the_unshadowed():
    exact, shadowed = _restir_di_run(True)
    _, unshadowed = _restir_di_run(False)
    # (each mean within 4 standard errors of the exact light: the unshadowed target's error is 5 times the other's)
    for estimates in (shadowed, unshadowed):
        y = model.luma(estimates)
        bound = 4.0 * y.std() / np.sqrt(len(y))
        assert abs(y.mean() - model.luma(exact)) < bound, (y.mean(), model.luma(exact), bound)
    # (the relative spread of a pixel's frame: the shadowed target's a fraction of the unshadowed one's)
    spread_s = model.luma(shadowed).std() / model.luma(exact)
    spread_u = model.luma(unshadowed).std() / model.luma(exact)
    assert spread_s < 0.6 * spread_u, (spread_s, spread_u)
    # (and its frames are rarely black where it sees a light)
    assert (model.luma(shadowed) <= 0.0).mean() < 0.05


def test_the_specular_history_is_whole_for_a_still_eye_and_short_for_a_moving_mirror():
    assert model.specular_most(0.05, 0.0) == 32.0
    assert model.specular_most(0.05, 0.02) < 8.0
    assert model.specular_most(0.3, 0.001) == 32.0
    assert 4.0 < model.specular_most(0.3, 0.01) < 32.0
    assert model.specular_most(0.6, 0.5) == 32.0
    assert model.specular_most(0.05, 0.0, level=False) < 8.0


def test_catmull_rom_keeps_a_moved_historys_detail_that_bilinear_blurs():
    for f in (0.0, 0.25, 0.5, 0.9):
        assert abs(model.catmull_rom(f).sum() - 1.0) < 1e-12
    assert np.allclose(model.catmull_rom(0.0), [0.0, 1.0, 0.0, 0.0])
    # (a sparse light's sample in a dark history, moved 30 times by a third of a pixel and back by whole pixels: the
    # bilinear taps spread it over many pixels, Catmull-Rom keeps most of it where it was)
    row = np.zeros(64)
    row[32] = 1.0
    cubic, linear = row.copy(), row.copy()
    for _ in range(30):
        cubic = np.roll(model.resample_row(cubic, 1.0 / 3.0), 0)
        linear = np.roll(model.resample_row(linear, 1.0 / 3.0, cubic=False), 0)
    assert cubic.max() > 2.0 * linear.max(), (cubic.max(), linear.max())
    assert (cubic >= 0.0).all()


def test_the_firefly_hold_keeps_a_light_few_frames_see_and_still_holds_a_lone_firefly():
    rng = np.random.default_rng(5)
    # (a light one frame in twenty sees, 20 times its mean: the held history's mean over a still eye's frames)
    frames = [np.where(rng.random(4000) < 0.05, 20.0, 0.0) for _ in range(400)]
    raw = np.mean([h.mean() for h in model.held_history(frames)[150:]])
    held = np.mean([h.mean() for h in model.held_history(frames, raw_moments=False)[150:]])
    assert raw > 0.93, raw
    assert held < 0.7, held
    # (a steady light with one sample a thousand times it: held to near the light)
    steady = [np.full(4, 1.0) for _ in range(40)]
    steady[30] = np.full(4, 1000.0)
    after = model.held_history(steady)[30]
    assert after.max() < 1.5, after


def test_the_anti_firefly_holds_a_lone_pixel_and_keeps_a_gradient_and_an_edge():
    ramp = np.tile(np.linspace(0.1, 1.0, 12), (8, 1))
    same = np.zeros((8, 12), int)
    assert np.allclose(model.anti_firefly(ramp, same), ramp)
    spot = ramp.copy()
    spot[4, 6] = 50.0
    held = model.anti_firefly(spot, same)
    assert held[4, 6] < 2.0 * ramp.max() + 1e-3
    # (a bright surface beside a dark one, another instance: not held to the dark one)
    instances = np.zeros((8, 12), int)
    instances[:, 6:] = 1
    edge = np.where(instances == 1, 10.0, 0.1)
    assert np.allclose(model.anti_firefly(edge, instances), edge)


def test_specular_virtual_point_is_a_plane_mirrors_image():
    """for a plane mirror the reflection's image (the point past the surface along the eye's ray by the reflection's
    length) is the reflected point mirrored through the plane, wherever the eye stands: so a last frame's camera
    sees the reflection at the image's place, and the history found there is the reflection's"""
    rng = np.random.default_rng(7)
    normal = np.array([0.0, 0.0, 1.0])
    for _ in range(50):
        camera = rng.uniform(-5, 5, 3)
        camera[2] = rng.uniform(0.5, 4.0)
        point = np.array([rng.uniform(-3, 3), rng.uniform(-3, 3), 0.0])
        eye = (camera - point) / np.linalg.norm(camera - point)
        r = 2.0 * np.dot(eye, normal) * normal - eye
        hit_distance = rng.uniform(0.5, 20.0)
        reflected = point + r * hit_distance
        mirrored = reflected * np.array([1.0, 1.0, -1.0])
        assert np.allclose(model.specular_virtual_point(point, eye, hit_distance), mirrored)


def test_specular_virtual_amount_mirror_to_rough():
    assert model.specular_virtual_amount(0.0) == 1.0
    assert model.specular_virtual_amount(0.1) == 1.0
    assert model.specular_virtual_amount(0.45) == 0.0
    assert 0.0 < model.specular_virtual_amount(0.3) < 1.0
    text = GLSL.read_text()
    assert "amount = 1.0 - smoothstep(0.1, 0.45, g.roughness);" in text
    assert "vec3 image = g.point - g.eye * (sum / count);" in text


def test_preblur_spreads_a_lucky_sample_and_keeps_the_light():
    """the pre-blur's disc spreads one bright sample over a dozen pixels (a twelfth as bright at each), keeps a flat
    light flat and, over a still eye's turned discs, keeps the light's mean (no luminance stop: energy kept)"""
    offsets = model.preblur_offsets()
    assert len(set(offsets)) >= 10
    assert max(max(abs(dx), abs(dy)) for dx, dy in offsets) <= 6
    flat = np.full((24, 24), 0.3)
    assert np.allclose(model.preblur(flat), 0.3)
    spike = np.zeros((24, 24))
    spike[12, 12] = 120.0
    blurred = model.preblur(spike)
    assert blurred.max() <= 120.0 / 10.0
    rng = np.random.default_rng(3)
    means = [model.preblur(spike, turn=rng.uniform(0, 2 * np.pi))[6:18, 6:18].sum() for _ in range(16)]
    assert abs(np.mean(means) - 120.0) < 120.0 * 0.15
    text = GLSL.read_text()
    assert "const int PREBLUR_TAPS = 12;" in text and "const float PREBLUR_RADIUS = 6.0;" in text
    assert "float a = float(k) * 2.3999632 + turn;" in text


def _tree_cone(a, b):
    """xgpu_vulkan_pt.c's tree_cone: the smallest cone (axis, cosine of its half angle) holding two cones"""
    (axis_a, cos_a), (axis_b, cos_b) = a, b
    ta, tb = np.arccos(np.clip(cos_a, -1, 1)), np.arccos(np.clip(cos_b, -1, 1))
    if ta < tb:
        axis_a, axis_b, ta, tb = axis_b, axis_a, tb, ta
    dot = float(np.dot(axis_a, axis_b))
    td = np.arccos(np.clip(dot, -1, 1))
    if min(td + tb, np.pi) <= ta:
        return axis_a, np.cos(ta)
    to = 0.5 * (ta + td + tb)
    if to >= np.pi or td < 1e-6:
        return axis_a, -1.0 if to >= np.pi else np.cos(to)
    tr = to - ta
    ortho = axis_b - dot * axis_a
    ortho = ortho / np.linalg.norm(ortho)
    axis = axis_a * np.cos(tr) + ortho * np.sin(tr)
    return axis / np.linalg.norm(axis), np.cos(to)


def test_the_light_trees_cone_holds_both_childrens_normals():
    rng = np.random.default_rng(5)
    for _ in range(2000):
        cones = []
        for _ in range(2):
            v = rng.normal(size=3)
            cones.append((v / np.linalg.norm(v), float(np.cos(rng.uniform(0, 1.5)))))
        axis, cos_o = _tree_cone(*cones)
        # (every direction of each child's cone is inside the merged one: its axis and its rim's farthest point)
        for child_axis, child_cos in cones:
            gap = np.arccos(np.clip(np.dot(axis, child_axis), -1, 1)) + np.arccos(child_cos)
            assert gap <= np.arccos(np.clip(cos_o, -1, 1)) + 1e-4


def _row_correlation(frames, distance):
    e = frames - frames.mean()
    a, b = e[:, :-distance].ravel(), e[:, distance:].ravel()
    return float(np.corrcoef(a, b)[0, 1])


def test_restir_dis_spatial_reuse_without_feedback_is_unbiased_and_decorrelated():
    # (a wall of one light far brighter than the rest that the candidates rarely pick: round 4's spatial reservoirs
    # kept for the next frame pass a lucky pick from pixel to pixel, so the row's pixels agree over spans of the
    # disc (the blobs in motion); the temporal reservoir kept, the neighbours' picks are this frame's light alone)
    rng = np.random.default_rng(7)
    count = 24
    light = np.full((count, 3), 0.05)
    light[3] = 6.0
    pick = np.ones(count)
    pick[3] = 0.03
    exact = model.luma(light).sum()
    rows = {fb: model.restir_di_row(rng, pick, light, 96, 160, spatial=1, feedback=fb) for fb in (True, False)}
    for fb, frames in rows.items():
        y = frames[40:]
        # (the mean near the exact light either way: one light far over the rest is a heavy tail, so within 12%)
        assert abs(y.mean() - exact) < 0.12 * exact, (fb, y.mean(), exact)
    near_fb = _row_correlation(rows[True][40:], 6)
    near = _row_correlation(rows[False][40:], 6)
    assert near < 0.5 * near_fb, (near, near_fb)
    assert near < 0.15, near


def test_the_boiling_filter_keeps_a_lucky_bright_light_and_an_estimate_filter_would_not():
    # (a light far over the rest that the candidates find rarely: its lucky sample's W is not an outlier, since its
    # target is as large, so RTXDI's filter (by W) keeps it and the mean; a filter by the estimate takes the light)
    count = 24
    light = np.full((count, 3), 0.05)
    light[3] = 40.0
    pick = np.ones(count)
    pick[3] = 0.01
    plain = model.restir_di_row(np.random.default_rng(3), pick, light, 64, 120, spatial=1)
    by_w = model.restir_di_row(np.random.default_rng(3), pick, light, 64, 120, spatial=1, boil=0.2)
    by_estimate = model.restir_di_row(np.random.default_rng(3), pick, light, 64, 120, spatial=1, boil=0.2,
                                      by_estimate=True)
    assert abs(by_w[30:].mean() - plain[30:].mean()) < 1e-9
    assert by_estimate[30:].mean() < 0.3 * plain[30:].mean()


def test_the_light_passs_specialization_constants_match_the_hosts():
    # (xgpu_vulkan_pt.c's spec_options fills SPEC_COUNT values, constant ids 0 to SPEC_COUNT - 1, as the GLSL declares)
    import re
    glsl = GLSL.read_text()
    host = (ROOT / "port/linux/vulkan/xgpu_vulkan_pt.c").read_text()
    ids = sorted(int(i) for i in re.findall(r"layout\(constant_id = (\d+)\) const", glsl))
    count = int(re.search(r"#define SPEC_COUNT (\d+)u", host).group(1))
    assert ids == list(range(count)), (ids, count)
    filled = sorted(set(int(i) for i in re.findall(r"\tvalues\[(\d+)\] = ", host)))
    assert filled == list(range(count)), filled



def test_the_specular_hold_drops_a_glint_that_moved_on_and_keeps_a_highlight_still_there():
    # (a glint the history carries over a wall whose samples now see nothing bright: held to them)
    assert model.specular_hold(5.0, 0.02, [0.01] * 8) == 0.02
    # (a lamp's highlight still there: one of the 3 x 3's samples finds it, the history kept)
    assert model.specular_hold(5.0, 0.02, [0.01] * 7 + [9.0]) == 5.0
    # (a still eye, or a rough surface: never held)
    assert model.specular_hold(5.0, 0.02, [0.01] * 8, moving=False) == 5.0
    assert model.specular_hold(5.0, 0.02, [0.01] * 8, roughness=0.8) == 5.0
