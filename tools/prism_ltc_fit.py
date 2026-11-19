#!/usr/bin/env python3
"""Fit PRISM Path Traced's linearly transformed cosines (LTC) to its own GGX lobe and write the table the path
tracer's analytic lamps read (port/linux/vulkan/xgpu_vulkan_pt_ltc.h).

The method is Heitz, Dupuy, Hill and Neubelt's ("Real-Time Polygonal-Light Shading with Linearly Transformed
Cosines", SIGGRAPH 2016): for each roughness and view angle, a 3 x 3 matrix M that turns the clamped cosine
distribution into one close to the BRDF times the cosine, so that the BRDF's integral over a polygon is the clamped
cosine's integral over the polygon transformed by M's inverse (a closed form). This fit and its table are our own
(numpy, no published table or code used): the lobe is the one xgpu_vulkan_pt.glsl's ggx() evaluates (GGX's
distribution at alpha = roughness squared, the height-correlated Smith visibility in Hammon's form, no Fresnel),
the error Heitz et al.'s (the cubed difference of the two normalised distributions, by multiple importance sampling
of both), minimised by Nelder and Mead's simplex from the neighbouring entry's solution.

Each entry: M's inverse (its four non-zero terms a, b, c, d of [[a, 0, b], [0, c, 0], [d, 0, 1]] in the frame of
the normal and the view's side, scaled so its last term is 1), the lobe's integral without Fresnel (A) and with
Schlick's (1 - v.h)^5 weight (B): the reflectance toward a lamp of radiance L over a polygon P is
    L * integral of the clamped cosine over normalize(M^-1 P) / pi * (F0 * A + (1 - F0) * B)
(A - B multiplies F0, B the rest: Schlick's F0 + (1 - F0)(1 - v.h)^5). Rows by roughness (0 to 1), columns by
sqrt(1 - cos theta_v) (0 to 1), as Heitz et al. index theirs.

    python3 tools/prism_ltc_fit.py [--size 32] [--check]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_pt_ltc.h"
MIN_ROUGHNESS = 0.03


def ggx_cos(l: np.ndarray, v: np.ndarray, alpha: float) -> tuple[np.ndarray, np.ndarray]:
    """the lobe times the cosine (xgpu_vulkan_pt.glsl's ggx() without the Fresnel and its clamp) toward each l
    (n = z), and (1 - v.h)^5"""
    nl = l[:, 2]
    nv = max(v[2], 1.0e-3)
    h = l + v
    h /= np.maximum(np.linalg.norm(h, axis=1, keepdims=True), 1.0e-12)
    nh = np.maximum(h[:, 2], 0.0)
    vh = np.maximum(h @ v, 0.0)
    a2 = alpha * alpha
    dd = nh * nh * (a2 - 1.0) + 1.0
    distribution = a2 / (np.pi * dd * dd)
    visibility = 0.5 / np.maximum(nl * (nv * (1.0 - alpha) + alpha) + nv * (nl * (1.0 - alpha) + alpha), 1.0e-4)
    value = np.where(nl > 0.0, distribution * visibility * nl, 0.0)
    return value, (1.0 - vh) ** 5


def vndf_sample(v: np.ndarray, alpha: float, u: np.ndarray) -> np.ndarray:
    """GGX's visible normals' sample (Heitz 2018), its reflections"""
    vh = np.array([alpha * v[0], alpha * v[1], v[2]])
    vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    t1 = np.array([-vh[1], vh[0], 0.0]) / np.sqrt(lensq) if lensq > 1.0e-12 else np.array([1.0, 0.0, 0.0])
    t2 = np.cross(vh, t1)
    r = np.sqrt(u[:, 0])
    phi = 2.0 * np.pi * u[:, 1]
    p1 = r * np.cos(phi)
    p2 = r * np.sin(phi)
    s = 0.5 * (1.0 + vh[2])
    p2 = (1.0 - s) * np.sqrt(np.maximum(1.0 - p1 * p1, 0.0)) + s * p2
    p3 = np.sqrt(np.maximum(1.0 - p1 * p1 - p2 * p2, 0.0))
    nh = p1[:, None] * t1 + p2[:, None] * t2 + p3[:, None] * vh
    m = np.stack([alpha * nh[:, 0], alpha * nh[:, 1], np.maximum(nh[:, 2], 1.0e-7)], axis=1)
    m /= np.linalg.norm(m, axis=1, keepdims=True)
    return 2.0 * (m @ v)[:, None] * m - v


def vndf_pdf(l: np.ndarray, v: np.ndarray, alpha: float) -> np.ndarray:
    nv = max(v[2], 1.0e-4)
    h = l + v
    h /= np.maximum(np.linalg.norm(h, axis=1, keepdims=True), 1.0e-12)
    nh = np.maximum(h[:, 2], 0.0)
    vh = np.maximum(h @ v, 1.0e-7)
    a2 = alpha * alpha
    dd = nh * nh * (a2 - 1.0) + 1.0
    distribution = a2 / (np.pi * dd * dd)
    g1 = 2.0 * nv / (nv + np.sqrt(a2 + (1.0 - a2) * nv * nv))
    return np.where(l[:, 2] > 0.0, g1 * distribution / (4.0 * nv), 0.0)


def matrix_of(p: np.ndarray, frame: np.ndarray | None = None) -> np.ndarray:
    """M: the frame of the lobe's mean direction (its columns X, Y, Z) by [[m11, 0, m13], [0, m22, 0], [0, 0, 1]]
    (Heitz et al.'s form: three parameters, the turn given by the mean direction)"""
    m11, m22, m13 = p
    local = np.array([[m11, 0.0, m13], [0.0, m22, 0.0], [0.0, 0.0, 1.0]])
    return (np.eye(3) if frame is None else frame) @ local


def ltc_eval(m_inv: np.ndarray, det_inv: float, w: np.ndarray) -> np.ndarray:
    """the transformed cosine's density at w (unit directions)"""
    o = w @ m_inv.T
    length = np.linalg.norm(o, axis=1)
    return np.maximum(o[:, 2], 0.0) / length / np.pi * abs(det_inv) / length ** 3


def stratified(n: int, rng: np.random.Generator) -> np.ndarray:
    i, j = np.meshgrid(np.arange(n), np.arange(n), indexing="ij")
    return np.stack([(i.ravel() + rng.random(n * n)) / n, (j.ravel() + rng.random(n * n)) / n], axis=1)


class Entry:
    def __init__(self, roughness: float, cos_v: float, samples: int, rng: np.random.Generator):
        self.alpha = max(roughness, MIN_ROUGHNESS) ** 2
        self.v = np.array([np.sqrt(max(1.0 - cos_v * cos_v, 0.0)), 0.0, cos_v])
        self.u_ltc = stratified(samples, rng)
        u = stratified(samples, rng)
        self.l_brdf = vndf_sample(self.v, self.alpha, u)
        value, fresnel = ggx_cos(self.l_brdf, self.v, self.alpha)
        pdf = vndf_pdf(self.l_brdf, self.v, self.alpha)
        ok = pdf > 0.0
        ratio = np.where(ok, value / np.where(ok, pdf, 1.0), 0.0)
        self.norm = float(ratio.mean())
        self.fresnel = float((ratio * fresnel).mean())
        self.brdf_value = value / max(self.norm, 1.0e-12)
        # (the lobe's mean direction, which a start's matrix turns z toward)
        mean = (ratio[:, None] * self.l_brdf).sum(axis=0)
        mean[1] = 0.0
        self.mean_dir = mean / max(np.linalg.norm(mean), 1.0e-12)
        z = self.mean_dir
        x = np.array([z[2], 0.0, -z[0]])
        x /= max(np.linalg.norm(x), 1.0e-12)
        self.frame = np.stack([x, np.array([0.0, 1.0, 0.0]), z], axis=1)
        self.brdf_pdf = pdf
        # (cosine samples, turned into directions by each candidate M)
        r = np.sqrt(self.u_ltc[:, 0])
        phi = 2.0 * np.pi * self.u_ltc[:, 1]
        self.cos_dirs = np.stack([r * np.cos(phi), r * np.sin(phi), np.sqrt(np.maximum(1.0 - r * r, 0.0))], axis=1)

    def error(self, p: np.ndarray) -> float:
        m = matrix_of(p, self.frame)
        det = np.linalg.det(m)
        if not np.isfinite(det) or abs(det) < 1.0e-12:
            return 1.0e30
        m_inv = np.linalg.inv(m)
        det_inv = 1.0 / det
        # (samples of the LTC)
        w = self.cos_dirs @ m.T
        w /= np.linalg.norm(w, axis=1, keepdims=True)
        pdf_ltc = ltc_eval(m_inv, det_inv, w)
        value, _ = ggx_cos(w, self.v, self.alpha)
        value /= max(self.norm, 1.0e-12)
        pdf_brdf = vndf_pdf(w, self.v, self.alpha)
        e1 = np.abs(value - pdf_ltc) ** 3 / np.maximum(pdf_ltc + pdf_brdf, 1.0e-12)
        # (samples of the lobe)
        w2 = self.l_brdf
        valid = w2[:, 2] > 0.0
        pdf_ltc2 = ltc_eval(m_inv, det_inv, w2)
        e2 = np.where(valid, np.abs(self.brdf_value - pdf_ltc2) ** 3 / np.maximum(pdf_ltc2 + self.brdf_pdf, 1.0e-12), 0.0)
        e = float(e1.mean() + e2.mean())
        return e if np.isfinite(e) else 1.0e30


def nelder_mead(f, x0: np.ndarray, step: np.ndarray, iterations: int = 600, tolerance: float = 1.0e-7) -> np.ndarray:
    n = len(x0)
    simplex = [x0.copy()]
    for k in range(n):
        x = x0.copy()
        x[k] += step[k]
        simplex.append(x)
    values = [f(x) for x in simplex]
    for _ in range(iterations):
        order = np.argsort(values)
        simplex = [simplex[i] for i in order]
        values = [values[i] for i in order]
        if abs(values[-1] - values[0]) <= tolerance * (abs(values[0]) + 1.0e-12):
            break
        centroid = np.mean(simplex[:-1], axis=0)
        reflected = centroid + (centroid - simplex[-1])
        fr = f(reflected)
        if values[0] <= fr < values[-2]:
            simplex[-1], values[-1] = reflected, fr
            continue
        if fr < values[0]:
            expanded = centroid + 2.0 * (centroid - simplex[-1])
            fe = f(expanded)
            if fe < fr:
                simplex[-1], values[-1] = expanded, fe
            else:
                simplex[-1], values[-1] = reflected, fr
            continue
        contracted = centroid + 0.5 * (simplex[-1] - centroid)
        fc = f(contracted)
        if fc < values[-1]:
            simplex[-1], values[-1] = contracted, fc
            continue
        for k in range(1, len(simplex)):
            simplex[k] = simplex[0] + 0.5 * (simplex[k] - simplex[0])
            values[k] = f(simplex[k])
    return simplex[int(np.argmin(values))]


def fit(size: int, samples: int, seed: int = 7) -> np.ndarray:
    rng = np.random.default_rng(seed)
    table = np.zeros((size, size, 6))
    for i in reversed(range(size)):
        roughness = i / (size - 1)
        previous = None
        for j in range(size):
            t = j / (size - 1)
            cos_v = max(1.0 - t * t, 1.0e-3)
            entry = Entry(roughness, cos_v, samples, rng)
            alpha = max(entry.alpha, 1.0e-4)
            step = np.array([0.25 * alpha + 1.0e-4, 0.25 * alpha + 1.0e-4, 0.1 * alpha + 1.0e-4])
            starts = [np.array([alpha, alpha, 0.0])]
            if previous is not None:
                starts.append(previous)
            best = None
            for start in starts:
                fitted = nelder_mead(entry.error, start, step)
                if best is None or entry.error(fitted) < entry.error(best):
                    best = fitted
            best = nelder_mead(entry.error, best, 0.5 * step)
            previous = best
            m_inv = np.linalg.inv(matrix_of(best, entry.frame))
            m_inv /= m_inv[2, 2]
            # (one sign of each mirror image: a row's sign flips x or y of the transformed direction, which leaves
            # the clamped cosine as it is, but the table's entries are interpolated, so neighbours must agree)
            if m_inv[0, 0] < 0.0:
                m_inv[0] = -m_inv[0]
            if m_inv[1, 1] < 0.0:
                m_inv[1] = -m_inv[1]
            table[i, j] = (m_inv[0, 0], m_inv[0, 2], m_inv[1, 1], m_inv[2, 0], entry.norm, entry.fresnel)
        print(f"roughness {roughness:.3f}: a {table[i, 0, 0]:.4g} .. {table[i, -1, 0]:.4g}, "
              f"A {table[i, 0, 4]:.3f} .. {table[i, -1, 4]:.3f}", file=sys.stderr, flush=True)
    return table


def c_float(v: float) -> str:
    text = f"{v:.8g}"
    return text + ("f" if any(c in text for c in ".e") else ".0f")


def header(table: np.ndarray) -> str:
    size = table.shape[0]
    lines = ["/* Generated by tools/prism_ltc_fit.py (our own fit of linearly transformed cosines to the path tracer's",
             "GGX lobe, after Heitz, Dupuy, Hill and Neubelt 2016). Do not edit. Rows by roughness 0 to 1, columns by",
             "sqrt(1 - cos theta_v) 0 to 1; each entry M's inverse (a, b, c, d of [[a, 0, b], [0, c, 0], [d, 0, 1]])",
             "then the lobe's integral without Fresnel (A) and with Schlick's weight (B), 8 floats (two unused). */",
             "", "#ifndef XGPU_VULKAN_PT_LTC_H", "#define XGPU_VULKAN_PT_LTC_H", "",
             f"#define XGPU_VULKAN_PT_LTC_SIZE {size}u", "#define XGPU_VULKAN_PT_LTC_FLOATS 8u", "",
             "static const float xgpu_vulkan_pt_ltc[XGPU_VULKAN_PT_LTC_SIZE * XGPU_VULKAN_PT_LTC_SIZE * "
             "XGPU_VULKAN_PT_LTC_FLOATS] =", "{"]
    for i in range(size):
        for j in range(size):
            values = list(table[i, j]) + [0.0, 0.0]
            lines.append("\t" + " ".join(f"{c_float(v)}," for v in values))
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--size", type=int, default=32)
    parser.add_argument("--samples", type=int, default=24, help="each distribution's samples: the square of this")
    parser.add_argument("--out", type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    table = fit(arguments.size, arguments.samples)
    arguments.out.write_text(header(table))


if __name__ == "__main__":
    main()
