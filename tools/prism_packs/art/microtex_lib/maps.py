"""Recipe -> height (mm) -> normal / cavity / roughness offset, author and shipped sizes.

Encodings:
  packed v1 tile  RGBA8: R,G = normal x,y (0.5 bias, OpenGL +Y), B = cavity (0.5 = 1.0),
                  A = roughness offset (0.5 = 0)
  _N.png          RGB tangent normal (OpenGL +Y)
  _cavity.png     L8, 0.5 = 1.0
  _rough.png      L8, 0.5 = 0 offset
  _height.png     L16, 0..65535 = 0..height_scale_mm
"""
import math

import numpy as np

from . import core, gen


def build_height(recipe, n, rng):
    ctx = gen.Ctx(n, recipe["tile_mm"], recipe["ship_px"], rng)
    h = np.zeros((n, n))
    masks = {}
    layers = []
    for layer in recipe["height_layers"]:
        p = {k: v for k, v in layer.items() if k not in ("gen", "note")}
        hl, m = gen.GENERATORS[layer["gen"]](ctx, **p)
        h += hl
        for k, v in m.items():
            masks[k] = np.maximum(masks[k], v) if k in masks else v
        layers.append({"gen": layer["gen"], "rms_mm": float(hl.std()),
                       "range_mm": float(hl.max() - hl.min())})
    return h, masks, ctx, layers


def normals(h_mm, texel_mm, gain):
    dx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) * 0.5 / texel_mm
    dy = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) * 0.5 / texel_mm
    nx, ny = -dx * gain, dy * gain                     # image row 0 = top, +Y = up
    ln = np.sqrt(nx ** 2 + ny ** 2 + 1)
    return np.stack([nx / ln, ny / ln, 1 / ln], -1)


def slope_rms(h_mm, texel_mm):
    dx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) * 0.5 / texel_mm
    dy = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) * 0.5 / texel_mm
    return float(np.sqrt((dx ** 2 + dy ** 2).mean()))


def cavity(h_mm, texel_mm, radius_mm, strength):
    """Concave (below its neighbourhood) -> darker. Mean exactly 1, no energy below k=4."""
    occl = core.gauss_blur(h_mm, radius_mm / texel_mm) - h_mm
    pos = occl[occl > 0]
    q = np.percentile(pos, 99.5) if pos.size else 1.0
    c = 1 - strength * np.clip(occl / (q + 1e-12), 0, 1)
    c = c / c.mean()
    c = 1 + core.highpass(c - 1)
    c = np.clip(c, 0.02, 1.98)
    return 1 + (c - c.mean())


def rough_offset(recipe, h_mm, cav, masks, ctx, rng):
    r = np.zeros_like(h_mm)
    hs = core.unit(h_mm)
    for layer in recipe.get("rough_layers", []):
        src = layer["src"]
        if src == "height":
            r += layer["k"] * hs
        elif src == "cavity":
            r += layer["k"] * (1 - cav)
        elif src == "noise":                            # also the "dust" generator
            lo = max(layer["wavelength_mm"][0], ctx.min_feat)
            r += layer["k"] * core.band_noise(ctx.n, ctx.tile_mm, lo, layer["wavelength_mm"][1], rng)
        elif src.startswith("mask:"):
            r += layer["k"] * masks[src[5:]]
        else:
            raise ValueError(src)
    r = core.highpass(r)
    r = np.clip(r - r.mean(), -0.45, 0.45)
    return r - r.mean()


def shrink(a, factor):
    while factor > 1:
        a = core.down2(a)
        factor //= 2
    return a


def build(recipe):
    """Returns dict of authored and shipped float maps plus metadata."""
    n, ship = recipe["author_px"], recipe["ship_px"]
    rng = np.random.default_rng(recipe["seed"])
    h, masks, ctx, layer_stats = build_height(recipe, n, rng)
    h = core.highpass(h)                                # no energy below tile/4
    s_rms = slope_rms(h, ctx.texel)
    gain = float(np.clip(recipe.get("target_slope_rms", 0.18) / max(s_rms, 1e-9), 1.0, recipe.get("max_gain", 8.0)))
    N = normals(h, ctx.texel, gain)
    cav = cavity(h, ctx.texel, recipe["cavity"]["radius_mm"], recipe["cavity"]["strength"])
    rough = rough_offset(recipe, h, cav, masks, ctx, rng)
    f = n // ship
    Ns = shrink(N, f)
    toksvig_len = np.linalg.norm(Ns, axis=-1)           # < 1 where detail averaged out
    Ns = Ns / toksvig_len[..., None]
    cav_s = shrink(cav, f)
    cav_s = 1 + (cav_s - cav_s.mean())
    rough_s = shrink(rough, f)
    rough_s = rough_s - rough_s.mean()
    h_s = shrink(h, f)
    extra = {k: shrink(v, f) for k, v in masks.items() if k in recipe.get("export_masks", [])}
    return {
        "ctx": ctx, "gain": gain, "slope_rms_physical": s_rms, "layers": layer_stats,
        "author": {"h": h, "N": N, "cav": cav, "rough": rough},
        "ship": {"h": h_s, "N": Ns, "cav": cav_s, "rough": rough_s, "nlen": toksvig_len},
        "masks": extra,
    }


def q8(a):
    return np.clip(np.round(a * 255), 0, 255).astype(np.uint8)


def pack_v1(N, cav, rough):
    return np.stack([q8(N[..., 0] * 0.5 + 0.5), q8(N[..., 1] * 0.5 + 0.5),
                     q8(np.clip(cav * 0.5, 0, 1)), q8(np.clip(0.5 + rough, 0, 1))], -1)


def unpack_v1(t):
    f = t.astype(np.float64) / 255.0
    nx, ny = f[..., 0] * 2 - 1, f[..., 1] * 2 - 1
    nz = np.sqrt(np.clip(1 - nx ** 2 - ny ** 2, 0, 1))
    return np.stack([nx, ny, nz], -1), f[..., 2] * 2, f[..., 3] - 0.5, nx ** 2 + ny ** 2


def lit_preview(N, cav, tiles=2):
    L = np.array([-0.62, 0.30, 0.42])
    L /= np.linalg.norm(L)
    shade = np.clip(N @ L, 0, 1) * cav * 0.9 + 0.06
    return q8(np.tile(np.clip(shade, 0, 1), (tiles, tiles)))


def mip_chain_normals(N0):
    """Box-filtered normal mips. Returns list of (renormalised normal, average length)."""
    out = [(N0, np.ones(N0.shape[:2]))]
    N, L = N0, np.ones(N0.shape[:2])
    while N.shape[0] > 1:
        Nd = core.down2(N * L[..., None])               # average the unnormalised vectors
        Ld = np.linalg.norm(Nd, axis=-1)
        N = Nd / np.maximum(Ld, 1e-9)[..., None]
        L = Ld
        out.append((N, L))
    return out


def toksvig_rough_offset(base_rough, nlen):
    """Perceptual-roughness increase for averaged normals (Toksvig 2005, GGX alpha = r^2)."""
    var = (1 - nlen) / np.maximum(nlen, 1e-6)
    a2 = base_rough ** 4 + var
    return a2 ** 0.25 - base_rough
