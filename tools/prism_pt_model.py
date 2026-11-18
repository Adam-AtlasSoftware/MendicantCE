"""PRISM Path Traced's arithmetic on the CPU (port/linux/vulkan/xgpu_vulkan_pt.glsl; docs/prism_pathtraced.md): the
parts of its passes whose correctness is arithmetic, written as the GLSL writes them, so that tools/test_prism_pt.py
can check them without a GPU:

  ris_lights         the light pass's resampled importance over the lamps (a mixture of densities, as the grid's
                     lamp_pick), the fixtures and the dynamic lights (each kind its own domain, by count)
  welford_frame      the reference pass's running means and squared deviations (one frame's paths as one sample)
  reference_noise    reference_stats' estimated relative MSE of the frame (linear), and the C side's stopping rule
  reference_noise_shown  the same as the gates' frames measure it: the drawn value's variance (none where it is held
                     at white or clipped), the gates' half-size bilinear frames' share of it (gate_share)
  mirror_normal      the G-buffer's object normal turned from the eye, mirrored about the plane square to it
  ggx_weight         the GGX visible-normals sample's weight (Fresnel by G2 / G1), as ggx_sample computes it
  cone_sample        a direction of a dome's cone (the bake's cone_dir), for its next-event estimate
  tree_build         the reference's light tree as xgpu_vulkan_pt.c's tree_node builds it
  tree_importance, tree_pick, tree_probability  the GLSL's walk of it
  temporal_step      the temporal pass's slow and fast histories, the moments, the fast one's hold
  atrous_step        an a-trous step's weights: the 3 x 3 kernel, the plane, the normal, the instance, the luminance
"""
import numpy as np

KERNEL = (1.0 / 2.0, 1.0 / 4.0)
REFERENCE_NOISE = 0.001
REFERENCE_LEAST = 256
GATE_WIDTH = 960


def luma(c):
    c = np.asarray(c, dtype=np.float64)
    return c[..., 0] * 0.2126 + c[..., 1] * 0.7152 + c[..., 2] * 0.0722


def ris_lights(rng, lamp_count, lamp_pick, lamp_sample, fixtures, fixture_count, dynamics, dynamic_count, visible,
               seen=False):
    """one estimate of the lights' light at a point, as the light pass's initial reservoir (lights_initial, its kept
    sample's shadow traced after) or, seen, as the reference's lights_ris_seen (each candidate's shadow traced, its
    visibility in its target): lamp_pick(rng) -> (lamp, probability);
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
        if seen and not visible("lamp", sample_id):
            continue
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
            if seen and not visible(kind, i):
                continue
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


def gate_share(width, gate_width=GATE_WIDTH):
    """xgpu_vulkan_pt.c's gate_share: the share of a pixel's independent variance the gates' frames keep, theirs
    this frame brought to gate_width by bilinear filtering (a triangle as wide as the scale on each side): the square
    of the weights' squares' sum along an axis"""
    scale = width / gate_width
    if scale <= 1.0:
        return 1.0
    reach = int(np.ceil(scale)) + 1
    w = np.array([1.0 - abs(k + 0.5) / scale for k in range(-reach, reach)])
    w = w[w > 0.0]
    a = (w * w).sum() / w.sum() ** 2
    return float(a * a)


def reference_noise_shown(means_y, m2s, n, clipped, exposure=1.0, shown_y=None, width=1920, gate_width=GATE_WIDTH):
    """reference_stats' second pair and the C side's ratio: each pixel's variance of its luminance's mean by the
    exposure squared, none where its drawn value is held at white or clipped (clipped: a bool array), over the drawn
    luminances squared (shown_y; default the means by the exposure, clipped at 1), by the gates' share"""
    means_y = np.asarray(means_y, float)
    if n < 2:
        return 1.0
    variance = np.maximum(np.asarray(m2s, float), 0.0) / (float(n) * float(n - 1)) * exposure * exposure
    variance = np.where(np.asarray(clipped, bool), 0.0, variance)
    y = np.minimum(means_y * exposure, 1.0) if shown_y is None else np.asarray(shown_y, float)
    square = float((y * y).sum())
    return gate_share(width, gate_width) * float(variance.sum()) / square if square > 0.0 else 1.0


def mirror_normal(n, eye):
    """surface_at's rule for an object's normal: turned from the eye (n . eye < 0), mirrored about the plane square
    to the eye (its sideways part kept, its part along the eye made toward it)"""
    n = np.asarray(n, float)
    eye = np.asarray(eye, float)
    toward = float(n @ eye)
    if toward >= 0.0:
        return n
    m = n - 2.0 * toward * eye
    return m / np.linalg.norm(m)


def ggx_weight(v_z, l_z, roughness, f0=0.04, v_dot_m=1.0):
    """ggx_sample's weight for a sampled direction (local frame: z the normal), Fresnel by G2 / G1, and 0 where the
    eye is at or below the surface or the direction is"""
    if l_z <= 0.0 or v_z <= 1.0e-4:
        return 0.0
    alpha = max(roughness * roughness, 1.0e-3)
    a2 = alpha * alpha
    g1v = 2.0 * v_z / (v_z + np.sqrt(a2 + (1.0 - a2) * v_z * v_z))
    g1l = 2.0 * l_z / (l_z + np.sqrt(a2 + (1.0 - a2) * l_z * l_z))
    g2 = g1v * g1l / max(g1v + g1l - g1v * g1l, 1.0e-4)
    fresnel = f0 + (1.0 - f0) * (1.0 - min(max(v_dot_m, 0.0), 1.0)) ** 5
    return fresnel * g2 / max(g1v, 1.0e-4)


def cone_sample(rng, axis, cos_most, count):
    """the bake's cone_dir: directions uniform over the cone about axis (unit) whose half angle's cosine is
    cos_most"""
    axis = np.asarray(axis, float)
    t = np.cross(axis, [0.0, 0.0, 1.0]) if abs(axis[2]) < 0.95 else np.cross(axis, [1.0, 0.0, 0.0])
    t /= np.linalg.norm(t)
    b = np.cross(axis, t)
    c = 1.0 + (cos_most - 1.0) * rng.random(count)
    sn = np.sqrt(np.maximum(1.0 - c * c, 0.0))
    a = rng.random(count) * 2.0 * np.pi
    return c[:, None] * axis + sn[:, None] * (np.cos(a)[:, None] * t + np.sin(a)[:, None] * b)


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
    sigma = np.sqrt(max(m2 - m1 * m1, fast_m2 - fast * fast, 0.0) / nf)
    lo = fast - gamma * sigma - 0.02 * fast
    hi = fast + gamma * sigma + 0.02 * fast
    if n > most_fast and (slow < lo or slow > hi):
        slow = min(max(slow, lo), hi)
        n = min(n, most_fast)
    return {"slow": slow, "n": n, "m1": m1, "m2": m2, "fast": fast, "fast_m2": fast_m2}


def atrous_weight(dx, dy, plane, plane_scale, normal_dot, same_instance, luminance_delta, sigma):
    """an a-trous tap's weight (the diffuse's): the 3 x 3 kernel's (1/4 1/2 1/4), the plane's exp(-|d . n| / scale), the normal's
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
            for dy in range(-1, 2):
                for dx in range(-1, 2):
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
    """a list's linear pick: the first entry whose cumulative weight reaches u x total"""
    x = u * cumulative[-1]
    k = 0
    while k + 1 < len(cumulative) and cumulative[k] < x:
        k += 1
    return k


def list_search(cumulative, u):
    """the bake's list_pick (xgpu_vulkan_bake_glsl.inc, by halves) and the entry's share"""
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


def guided_bounce(rng, sample_direction, light, guide, candidates=8, floor=0.002):
    """the reference's guided bounce (guided_bounce): candidates directions from the source density
    (sample_direction(rng)), each weighed by guide(direction) + floor, one kept by its weight, its light
    (light(rng, direction): an unbiased estimate) over its weight by the candidates' mean weight"""
    kept, kept_w, total = None, 0.0, 0.0
    for _ in range(candidates):
        d = sample_direction(rng)
        w = guide(d) + floor
        total += w
        if rng.random() * total <= w:
            kept, kept_w = d, w
    if kept is None:
        return 0.0
    return light(rng, kept) / kept_w * (total / candidates)


def tree_build(lamps):
    """xgpu_vulkan_pt.c's tree_node over lamps (an (n, 16) array: corners, radiance, area, ...): a list of node dicts
    (lo, hi, power, left, right, parent, lamp, normal, plane) and each lamp's leaf"""
    lamps = np.asarray(lamps, float)
    nodes = []
    middles = (lamps[:, 0:3] + lamps[:, 3:6] + lamps[:, 6:9]) / 3.0

    def node(order, parent):
        index = len(nodes)
        corners = lamps[order][:, :9].reshape(-1, 3)
        power = float(((lamps[order][:, 9:12] @ np.array([0.2126, 0.7152, 0.0722])) * lamps[order][:, 12]).sum())
        entry = {"lo": corners.min(0), "hi": corners.max(0), "power": power, "left": 0, "right": 0, "parent": parent,
                 "lamp": 0, "normal": np.zeros(3), "plane": 0.0}
        nodes.append(entry)
        if len(order) == 1:
            lamp = lamps[order[0]]
            n = np.cross(lamp[3:6] - lamp[0:3], lamp[6:9] - lamp[0:3])
            n = n / np.linalg.norm(n)
            entry.update(lamp=int(order[0]), normal=n, plane=float(n @ lamp[0:3]))
            return index
        m = middles[order]
        axis = int(np.argmax(m.max(0) - m.min(0)))
        order = sorted(order, key=lambda i: (middles[i, axis], i))
        half = len(order) // 2
        entry["left"] = node(order[:half], index)
        entry["right"] = node(order[half:], index)
        return index

    node(list(range(len(lamps))), 0)
    leaf = np.zeros(len(lamps), int)
    for k, entry in enumerate(nodes):
        if entry["left"] == 0:
            leaf[entry["lamp"]] = k
    return nodes, leaf


def tree_importance(nodes, k, p, n):
    entry = nodes[k]
    if entry["left"] == 0 and float(entry["normal"] @ p) - entry["plane"] < -1.0e-4:
        return 0.0
    lo, hi = entry["lo"], entry["hi"]
    d = 0.5 * (lo + hi) - p
    r2 = 0.25 * float((hi - lo) @ (hi - lo))
    dist2 = float(d @ d)
    bound = 1.0
    if dist2 > r2:
        cos_u = np.sqrt(max(1.0 - r2 / dist2, 0.0))
        sin_u = np.sqrt(min(r2 / dist2, 1.0))
        cos_i = float(n @ d) / np.sqrt(dist2)
        sin_i = np.sqrt(max(1.0 - cos_i * cos_i, 0.0))
        bound = 1.0 if cos_i >= cos_u else max(cos_i * cos_u + sin_i * sin_u, 0.0)
    return entry["power"] * bound / max(dist2, r2, 1.0e-8)


def tree_left(nodes, left, right, p, n):
    il, ir = tree_importance(nodes, left, p, n), tree_importance(nodes, right, p, n)
    if il + ir > 0.0:
        return il / (il + ir)
    return nodes[left]["power"] / max(nodes[left]["power"] + nodes[right]["power"], 1e-30)


def tree_pick(rng, nodes, p, n):
    k, probability = 0, 1.0
    while nodes[k]["left"] != 0:
        left, right = nodes[k]["left"], nodes[k]["right"]
        pl = tree_left(nodes, left, right, p, n)
        if rng.random() < pl:
            k, probability = left, probability * pl
        else:
            k, probability = right, probability * (1.0 - pl)
    return nodes[k]["lamp"], probability


def tree_probability(nodes, leaf, lamp, p, n):
    k, probability = int(leaf[lamp]), 1.0
    while k != 0:
        parent = nodes[k]["parent"]
        left, right = nodes[parent]["left"], nodes[parent]["right"]
        pl = tree_left(nodes, left, right, p, n)
        probability *= pl if k == left else 1.0 - pl
        k = parent
    return probability
