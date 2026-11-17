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
    for name in ("light", "temporal", "fix", "atrous", "composite", "reference", "reference_stats"):
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
    assert model.reference_stops(0.0009, 64, 4096, 64)
    assert not model.reference_stops(0.0009, 32, 4096, 32)
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
