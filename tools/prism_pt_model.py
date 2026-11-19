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
  firefly_cap        the temporal pass's hold of a new sample (by its history once long, by its neighbours before)
  restir_di          ReSTIR DI's reservoir at a still pixel: its initial candidates, the last frame's reservoir, both
                     by the shadowed target (lights_restir), or by the unshadowed one (round 2's) for comparison
  held_history       a still pixel's slow history over frames of samples, each held by firefly_cap (the long rule) with
                     the moments of the samples as traced
  restir_gi          ReSTIR GI's reservoir at a pixel: its own bounce sample, the last frame's merged by its Jacobian
  visibility_step    the temporal pass's sun visibility history, held to the frame's own 5 x 5 range
  tree_build         the reference's light tree as xgpu_vulkan_pt.c's tree_node builds it
  tree_importance, tree_pick, tree_probability  the GLSL's walk of it
  specular_most      the temporal pass's longest specular history, by the gloss and the eye's parallax at the point
  catmull_rom, resample_row  the temporal pass's history resampling (Catmull-Rom held to its 2 x 2's range, or bilinear)
  temporal_step      the temporal pass's slow and fast histories, the moments, the fast one's hold
  anti_firefly       the first a-trous step's hold of a pixel to twice its eight neighbours' brightest (shown only)
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


def specular_most(roughness, parallax, most_d=32.0, level=True):
    """the temporal pass's most_s: 4 frames for a mirror to most_d at roughness 0.5, and on the level at least the
    reflection lobe's width (twice alpha) over the parallax (radians the eye turned about the point since the last frame)"""
    most = 4.0 + (most_d - 4.0) * min(max(roughness * 2.0, 0.0), 1.0)
    if level:
        alpha = max(roughness * roughness, 0.01)
        most = max(most, min(max(2.0 * alpha / max(parallax, 1.0e-6), 4.0), most_d))
    return most


def catmull_rom(f):
    """the weights of the taps 1 before, at, 1 and 2 after the place's floor, f its fraction"""
    f2, f3 = f * f, f * f * f
    return np.array([-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f,
                     0.5 * f3 - 0.5 * f2])


def resample_row(row, shift, cubic=True):
    """a history row moved by `shift` pixels (0 to 1) as the temporal pass reprojects it, by Catmull-Rom held to the
    two nearest taps' range, or bilinear; the edges' pixels kept"""
    out = row.copy()
    for x in range(2, len(row) - 2):
        at = x - shift
        a = int(np.floor(at))
        f = at - a
        if cubic:
            taps = row[a - 1:a + 3]
            v = float(np.dot(catmull_rom(f), taps))
            out[x] = min(max(v, min(row[a], row[a + 1])), max(row[a], row[a + 1]))
        else:
            out[x] = (1.0 - f) * row[a] + f * row[a + 1]
    return out


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


def anti_firefly(values, instances):
    """the first a-trous step's hold: each pixel's value held to twice the brightest of its eight neighbours of the
    same instance (a 2-D array of luminances; the edges' pixels by the neighbours they have)"""
    out = values.copy()
    h, w = values.shape
    for y in range(h):
        for x in range(w):
            most, any_ = 0.0, False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    qy, qx = y + dy, x + dx
                    if (dy or dx) and 0 <= qy < h and 0 <= qx < w and instances[qy, qx] == instances[y, x]:
                        most, any_ = max(most, values[qy, qx]), True
            if any_ and values[y, x] > 2.0 * most + 1.0e-4:
                out[y, x] = 2.0 * most + 1.0e-4
    return out


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


def firefly_cap(sample, history_length, history_mean=0.0, history_m2=0.0, neighbours=()):
    """the temporal pass's hold of a new sample's luminance: with 16 frames of history or more, the history's mean and
    8 times its spread; with fewer (or none: history_length 0), 4 times the brightest of its neighbours on the same
    surface (none given: no hold)"""
    cap = 1.0e30
    if history_length >= 16.0:
        sigma = np.sqrt(max(history_m2 - history_mean * history_mean, 0.0))
        cap = max(history_mean + 8.0 * sigma, 1.0e-4)
    elif len(neighbours):
        cap = max(4.0 * max(neighbours), 1.0e-3)
    return min(sample, cap)


def restir_di(rng, pick, light, visible, candidates, last=None, m_cap=20.0, shadowed=True):
    """one frame of the light pass's lights_restir at a pixel whose surface is the last frame's: pick (each light's
    candidate probability), light (each light's rgb integrand at the pixel, unshadowed), visible (each light's
    shadow ray from the pixel), candidates the initial reservoir's count; last = (light, W, M) or None. shadowed: the
    target holds the shadow (this round's), else the unshadowed target and the kept sample's shadow traced after
    (round 2's). Returns (rgb estimate, the reservoir kept (light or None, W, M))"""
    pick = np.asarray(pick, float) / float(np.sum(pick))
    target = luma(np.asarray(light, float))
    kept, total = None, 0.0
    for _ in range(candidates):
        i = int(np.searchsorted(np.cumsum(pick), rng.random(), side="right"))
        i = min(i, len(pick) - 1)
        w = target[i] / pick[i] / candidates
        if not w > 0.0:
            continue
        total += w
        if rng.random() * total <= w:
            kept = i
    if shadowed and kept is not None and not visible[kept]:
        kept, total = None, 0.0
    m_total, w_total = 1.0, total if kept is not None else 0.0
    if last is not None and last[0] is not None and last[1] > 0.0:
        m_last = min(last[2], m_cap)
        j = last[0]
        w_last = target[j] * last[1] * m_last
        if w_last > 0.0 and (visible[j] or not shadowed):
            w_total += w_last
            if rng.random() * w_total <= w_last:
                kept = j
        m_total += m_last
    elif last is not None:
        m_total += min(last[2], m_cap)
    big_w = w_total / (target[kept] * m_total) if kept is not None and target[kept] > 0.0 else 0.0
    estimate = np.asarray(light[kept], float) * big_w if kept is not None and visible[kept] else np.zeros(3)
    return estimate, (kept if big_w > 0.0 else None, big_w, m_total)


def restir_di_row(rng, pick, light, width, frames, spatial=1, feedback=False, m_cap=8.0, candidates=4, boil=0.0,
                  by_estimate=False):
    """lights_restir over a row of `width` pixels of one surface that sees the same lights (light: each light's rgb
    at every pixel, all seen), the eye still: each frame each pixel's initial reservoir, the last frame's at its own
    place, and `spatial` of the last frame's at 2 to 16 pixels from it (each neighbour counted, as Z counts it where
    the kept sample lights the neighbour: here everywhere). feedback: the reservoir kept for the next frame is the
    spatial one (round 4's), else the temporal one (round 7's); boil: the boiling filter's strength (0 none), each
    reservoir's W (by_estimate: its estimate, W by its target) against the others' mean over the whole row as one subgroup. Returns the frames' estimates' luminance, frames x width"""
    pick = np.asarray(pick, float) / float(np.sum(pick))
    cumulative = np.cumsum(pick)
    target = luma(np.asarray(light, float))
    lum = target
    last = [(None, 0.0, 0.0)] * width
    out = np.zeros((frames, width))

    def merge(state, offered):
        kept, w_total, m_total = state
        j, big_w, m = offered
        m = min(m, m_cap)
        if j is not None and big_w > 0.0:
            w = target[j] * big_w * m
            w_total += w
            if rng.random() * w_total <= w:
                kept = j
        return kept, w_total, m_total + m

    for f in range(frames):
        now = []
        for x in range(width):
            kept, total = None, 0.0
            for _ in range(candidates):
                i = min(int(np.searchsorted(cumulative, rng.random(), side="right")), len(pick) - 1)
                w = target[i] / pick[i] / candidates
                total += w
                if rng.random() * total <= w:
                    kept = i
            state = merge((kept, total, 1.0), last[x])
            temporal = state
            for _ in range(spatial):
                r = 2.0 + 14.0 * np.sqrt(rng.random())
                q = x + int(round(r if rng.random() < 0.5 else -r))
                if 0 <= q < width:
                    state = merge(state, last[q])
            kept, w_total, m_total = state
            big_w = w_total / (target[kept] * m_total) if kept is not None else 0.0
            out[f, x] = lum[kept] * big_w if kept is not None else 0.0
            keep = state if feedback else temporal
            k, wt, mt = keep
            now.append((k, wt / (target[k] * mt) if k is not None else 0.0, mt))
        if boil > 0.0:
            weights = np.array([(n[1] * target[n[0]] if by_estimate else n[1]) if n[0] is not None else 0.0
                                for n in now])
            total, count = weights.sum(), float((weights > 0.0).sum())
            most = 10.0 / boil - 9.0
            if count > 1.5:
                now = [(None, 0.0, 0.0) if w > 0.0 and w > most * (total - w) / (count - 1.0) else n
                       for n, w in zip(now, weights)]
        last = now
    return out


def held_history(samples, most=32.0, raw_moments=True):
    """the temporal pass's slow history at a still pixel over its frames' samples (each frame's array, one value a
    pixel), each sample held as firefly_cap's long rule holds it once the history has 16 frames; its moments the
    samples as traced (raw_moments, the pass's) or as held (round 2's). Returns the slow histories after each frame"""
    first = np.asarray(samples[0], float)
    slow, m1, m2, n = np.zeros_like(first), np.zeros_like(first), np.zeros_like(first), 0.0
    out = []
    for s in samples:
        s = np.asarray(s, float)
        nn = min(n + 1.0, most)
        if n >= 16.0:
            cap = np.maximum(m1 + 8.0 * np.sqrt(np.maximum(m2 - m1 * m1, 0.0)), 1.0e-4)
            held = np.minimum(s, cap)
        else:
            held = s
        moment = s if raw_moments else held
        slow = slow + (held - slow) / nn
        m1 = m1 + (moment - m1) / nn
        m2 = m2 + (moment * moment - m2) / nn
        n = nn
        out.append(slow.copy())
    return out


def restir_gi(rng, point, normal, sample, last=None, m_cap=20.0):
    """the light pass's restir_gi: sample = (hit point, hit normal, radiance) of this pixel's cosine bounce (its
    radiance 0 where it left), last = None or (the last frame's point, its normal, its reservoir (hit point, hit
    normal, radiance, M, W)); the two merged by generalized resampled importance (Lin et al. 2022): each one's
    weight its MIS weight (the generalized balance heuristic over the two domains, each by its confidence and its
    target there, the reconnection's Jacobian taking a sample from one point's measure to the other's) by its target
    here by its contribution weight W; returns (the bounce's light: the kept radiance by the cosine over pi by W,
    the reservoir)"""
    def target(x, n, p_x, p_l):
        to = p_x - x
        d2 = max(float(to @ to), 1e-8)
        return float(luma(p_l)) * max(float(n @ to) / np.sqrt(d2), 0.0), d2, to

    def jacobian(p_x, p_n, from_x, to_x):
        a, b = p_x - from_x, p_x - to_x
        da, db = max(float(a @ a), 1e-8), max(float(b @ b), 1e-8)
        ca, cb = abs(float(p_n @ a)) / np.sqrt(da), abs(float(p_n @ b)) / np.sqrt(db)
        return (cb / ca) * (da / db) if ca > 1e-4 else 0.0

    y_x, y_n, y_l = (np.asarray(v, float) for v in sample)
    t_c, _, _ = target(point, normal, y_x, y_l)
    w_c = float(luma(y_l)) * np.pi if t_c > 0.0 else 0.0
    m_total = 1.0
    keep = (y_x, y_n, y_l, t_c)
    w_sum = w_c
    if last is not None:
        x_last, n_last, (p_x, p_n, p_l, m_last, w_last) = last
        x_last, n_last = np.asarray(x_last, float), np.asarray(n_last, float)
        p_x, p_n, p_l = (np.asarray(v, float) for v in (p_x, p_n, p_l))
        m_last = min(m_last, m_cap)
        if m_last > 0.0:
            # (this frame's sample: its MIS weight, the last point's target for it by the move's Jacobian; the last
            # reservoir's confidence counts whatever its sample, a miss's or a dark one's, or the light grows as it is
            # reused)
            t_cl, _, _ = target(x_last, n_last, y_x, y_l)
            j_cl = jacobian(y_x, y_n, point, x_last)
            m_c = t_c / (t_c + m_last * t_cl * j_cl) if t_c > 0.0 else 0.0
            w_sum = m_c * w_c
            # (the last frame's sample: its target here, by its Jacobian from there to here)
            j_lc = jacobian(p_x, p_n, x_last, point)
            t_here, _, _ = target(point, normal, p_x, p_l)
            t_ll, _, _ = target(x_last, n_last, p_x, p_l)
            if w_last > 0.0 and 0.1 < j_lc < 10.0 and t_here > 0.0:
                m_l = m_last * t_ll / (t_here * j_lc + m_last * t_ll)
                w = m_l * t_here * w_last * j_lc
                if w > 0.0:
                    w_sum += w
                    if rng.random() * w_sum <= w:
                        keep = (p_x, p_n, p_l, t_here)
            m_total += m_last
    y_x, y_n, y_l, y_t = keep
    big_w = w_sum / y_t if y_t > 0.0 else 0.0
    to = y_x - point
    cos_y = max(float(normal @ to) / np.linalg.norm(to), 0.0)
    return y_l * cos_y / np.pi * big_w, (y_x, y_n, y_l, min(m_total, m_cap), big_w)


def visibility_step(history, v, neighbourhood, most=32.0, most_fast=4.0):
    """the temporal pass's sun visibility at a pixel: history None or (slow, fast, m2, n); v this frame's visibility,
    neighbourhood this frame's visibilities of the 5 x 5 about it (the pixel's own among them); returns the history"""
    if history is None:
        return (v, v, v * v, 1.0)
    slow, fast, m2, n = history
    n = min(n + 1.0, most)
    slow = slow + (v - slow) / n
    fast = fast + (v - fast) / min(n, most_fast)
    m2 = m2 + (v * v - m2) / n
    lo, hi = min(neighbourhood), max(neighbourhood)
    slow, fast = min(max(slow, lo), hi), min(max(fast, lo), hi)
    sv = np.sqrt(max(m2 - slow * slow, 0.0))
    if slow < fast - 2.0 * sv - 0.1 or slow > fast + 2.0 * sv + 0.1:
        slow = min(max(slow, fast - 2.0 * sv - 0.1), fast + 2.0 * sv + 0.1)
        n = min(n, most_fast * 2.0)
    return (slow, fast, m2, n)


def specular_virtual_point(point, eye, hit_distance):
    """the temporal pass's reflection image (specular_virtual): the point along the eye's ray past the surface by the
    reflection's length; eye the unit vector from the point to the eye"""
    return np.asarray(point, float) - np.asarray(eye, float) * hit_distance


def specular_virtual_amount(roughness):
    """the share of the specular's history taken by the virtual motion: 1 - smoothstep(0.1, 0.45, roughness)"""
    x = min(max((roughness - 0.1) / 0.35, 0.0), 1.0)
    return 1.0 - x * x * (3.0 - 2.0 * x)


def preblur_offsets(taps=12, radius=6.0, turn=0.0):
    """the pre-blur's disc (the fix pass's preblur): a Vogel disc of `taps` points, radius pixels, turned by `turn`"""
    out = []
    for k in range(taps):
        r = np.sqrt((k + 0.5) / taps)
        a = k * 2.3999632 + turn
        out.append((int(np.round(r * np.cos(a) * radius)), int(np.round(r * np.sin(a) * radius))))
    return out


def preblur(image, taps=12, radius=6.0, turn=0.0):
    """one flat surface's pre-blur: each pixel the mean of itself and its disc's taps inside the image"""
    h, w = image.shape
    out = np.zeros_like(image)
    offsets = [o for o in preblur_offsets(taps, radius, turn) if o != (0, 0)]
    for y in range(h):
        for x in range(w):
            total, count = image[y, x], 1.0
            for dx, dy in offsets:
                if 0 <= x + dx < w and 0 <= y + dy < h:
                    total += image[y + dy, x + dx]
                    count += 1.0
            out[y, x] = total / count
    return out
