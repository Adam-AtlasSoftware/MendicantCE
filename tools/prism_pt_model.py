"""PRISM Path Traced's arithmetic on the CPU (port/linux/vulkan/xgpu_vulkan_pt.glsl; docs/prism_pathtraced.md): the
parts of its passes whose correctness is arithmetic, written as the GLSL writes them, so that tools/test_prism_pt.py
can check them without a GPU:

  ris_lights         the light pass's resampled importance over the lamps (a mixture of densities, as the grid's
                     lamp_pick), the fixtures and the dynamic lights (each kind its own domain, by count)
  welford_frame      the reference pass's running means and squared deviations (one frame's paths as one sample)
  reference_noise    reference_stats' estimated relative MSE of the frame, and the C side's stopping rule
  temporal_step      the temporal pass's slow and fast histories, the moments, the fast one's hold
  atrous_step        an a-trous step's weights: the B3 spline, the plane, the normal, the instance, the luminance
"""
import numpy as np

KERNEL = (3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0)
REFERENCE_NOISE = 0.001
REFERENCE_LEAST = 64


def luma(c):
    c = np.asarray(c, dtype=np.float64)
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722


def ris_lights(rng, lamp_count, lamp_pick, lamp_sample, fixtures, fixture_count, dynamics, dynamic_count, visible):
    """one estimate of the lights' light at a point, as lights_ris: lamp_pick(rng) -> (lamp, probability);
    lamp_sample(rng, lamp) -> (integrand rgb, inverse pdf over directions, sample id) or None; fixtures and dynamics:
    lists of integrand rgb (0 where none reaches); visible(kind, id) -> bool, the kept one's shadow ray. Returns the
    rgb estimate"""
    kept, kept_t, total = None, 0.0, 0.0
    kept_id = None
    for _ in range(lamp_count):
        lamp, probability = lamp_pick(rng)
        if probability <= 0.0:
            continue
        sample = lamp_sample(rng, lamp)
        if sample is None:
            continue
        integrand, inverse_pdf, sample_id = sample
        target = float(luma(integrand))
        w = target * inverse_pdf / probability / float(lamp_count)
        if not w > 0.0:
            continue
        total += w
        if rng.random() * total <= w:
            kept, kept_t, kept_id = np.asarray(integrand, float), target, ("lamp", sample_id)
    for kind, lights, count in (("fixture", fixtures, fixture_count), ("dynamic", dynamics, dynamic_count)):
        n = len(lights)
        if n == 0 or count <= 0:
            continue
        candidates = min(n, count)
        for c in range(candidates):
            i = c if n <= count else min(int(rng.random() * n), n - 1)
            e = np.asarray(lights[i], float)
            target = float(luma(e))
            w = target * (float(n) / float(candidates))
            if not w > 0.0:
                continue
            total += w
            if rng.random() * total <= w:
                kept, kept_t, kept_id = e, target, (kind, i)
    if kept is None or kept_t <= 0.0 or total <= 0.0 or not visible(*kept_id):
        return np.zeros(3)
    return kept / kept_t * total


def welford_frame(mean_d, mean_y, m2_y, n, ld, y):
    """the reference pass's update of a pixel's sums with frame n's sample (n from 1): the diffuse light's running
    mean, the luminance's mean and squared deviations"""
    fn = float(n)
    if n == 1:
        mean_d = np.array(ld, float)
        mean_y, m2_y = 0.0, 0.0
    else:
        mean_d = mean_d + (np.asarray(ld, float) - mean_d) / fn
    delta = y - mean_y
    mean_y += delta / fn
    m2_y += delta * (y - mean_y)
    return mean_d, mean_y, m2_y


def reference_noise(means, m2s, n):
    """reference_stats and the C side's sum: the frame's estimated relative MSE (each pixel's variance of its mean
    over the sum of the means squared)"""
    means = np.asarray(means, float)
    m2s = np.asarray(m2s, float)
    if n < 2:
        return 1.0
    variance = np.maximum(m2s, 0.0) / (float(n) * float(n - 1))
    square = float((means * means).sum())
    return float(variance.sum()) / square if square > 0.0 else 1.0


def reference_stops(noise, frames_measured, frames, count):
    """the reference's stopping rule (xgpu_vulkan_pt.c): its noise under the bound with REFERENCE_LEAST frames, or
    its frames done"""
    return (noise <= REFERENCE_NOISE and frames_measured >= REFERENCE_LEAST) or count >= frames


def temporal_step(history, sample, most=32.0, most_fast=4.0, gamma=2.0):
    """the temporal pass at a pixel, its diffuse light's luminance alone: history None (no history) or a dict of the
    slow value, its length, the slow's moments (m1, m2) and the fast's (mean, m2); returns the new dict"""
    if history is None:
        return {"slow": sample, "n": 1.0, "m1": sample, "m2": sample * sample, "fast": sample,
                "fast_m2": sample * sample}
    n = min(history["n"] + 1.0, most)
    nf = min(history["n"] + 1.0, most_fast)
    slow = history["slow"] + (sample - history["slow"]) / n
    m1 = history["m1"] + (sample - history["m1"]) / n
    m2 = history["m2"] + (sample * sample - history["m2"]) / n
    fast = history["fast"] + (sample - history["fast"]) / nf
    fast_m2 = history["fast_m2"] + (sample * sample - history["fast_m2"]) / nf
    sigma = np.sqrt(max(fast_m2 - fast * fast, 0.0) / nf)
    lo = fast - gamma * sigma - 0.02 * fast
    hi = fast + gamma * sigma + 0.02 * fast
    if n > most_fast and (slow < lo or slow > hi):
        slow = min(max(slow, lo), hi)
        n = min(n, most_fast)
    return {"slow": slow, "n": n, "m1": m1, "m2": m2, "fast": fast, "fast_m2": fast_m2}


def atrous_weight(dx, dy, plane, plane_scale, normal_dot, same_instance, luminance_delta, sigma):
    """an a-trous tap's weight (the diffuse's): the B3 spline's, the plane's exp(-|d . n| / scale), the normal's
    cosine to the 64th, nothing across an instance's edge, the luminance's exp(-|dL| / sigma)"""
    if not same_instance:
        return 0.0
    k = KERNEL[abs(dx)] * KERNEL[abs(dy)]
    wz = np.exp(-abs(plane) / plane_scale)
    wn = max(normal_dot, 0.0) ** 64
    wl = np.exp(-abs(luminance_delta) / sigma)
    return k * wz * wn * wl


def atrous_step(values, instances, variance, stride, sigma_scale=4.0):
    """one a-trous step over a flat image of one luminance channel (every surface facing the eye, on one plane):
    values and instances (2D arrays), variance (2D), stride 2^step; returns the filtered values and variance"""
    h, w = values.shape
    out = np.empty_like(values)
    out_var = np.empty_like(variance)
    for y in range(h):
        for x in range(w):
            sigma = sigma_scale * np.sqrt(max(variance[y, x], 0.0)) + 1.0e-4
            total = wsum = var = 0.0
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    qy, qx = y + dy * stride, x + dx * stride
                    if not (0 <= qy < h and 0 <= qx < w):
                        continue
                    wt = atrous_weight(dx, dy, 0.0, 1.0, 1.0, instances[qy, qx] == instances[y, x],
                                       values[qy, qx] - values[y, x], sigma)
                    total += wt * values[qy, qx]
                    var += wt * wt * max(variance[qy, qx], 0.0)
                    wsum += wt
            out[y, x] = total / wsum if wsum > 0 else values[y, x]
            out_var[y, x] = var / (wsum * wsum) if wsum > 0 else variance[y, x]
    return out, out_var


def list_pick_linear(cumulative, u):
    """the bake's list_pick (xgpu_vulkan_bake_glsl.inc): the first entry whose cumulative weight reaches u x total"""
    x = u * cumulative[-1]
    k = 0
    while k + 1 < len(cumulative) and cumulative[k] < x:
        k += 1
    return k


def list_search(cumulative, u):
    """the path tracer's list_search: the same entry found by halves, and its share"""
    x = u * cumulative[-1]
    lo, hi = 0, len(cumulative) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if cumulative[mid] < x:
            lo = mid + 1
        else:
            hi = mid
    below = cumulative[lo - 1] if lo > 0 else 0.0
    return lo, (cumulative[lo] - below) / max(cumulative[-1], 1e-20)
