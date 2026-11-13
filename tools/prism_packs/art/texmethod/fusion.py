"""Upscaler ensemble ("fusion"): per pixel, each upscaler's output is used only in the band and region
where it is good. CE's own lower band is kept (as in A'), so the art style and colours stay CE's.

  CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0 python -I fusion.py [--4k] [tex ...]

Inputs: the cached whole-map 4x outputs of each candidate (fusion_cache.py) in
<work>/textures/out/fusion/cache/<tex>/<model>.png. All work is at 4x (S = 4 px per CE texel).

Bands (each candidate first back-projected to CE's low band, so they all agree below ~2 texels):
  low   G(x, 0.6 S) of the general-wdn A' result (== CE's colours and shapes)
  high  x - G(x, 0.6 S), chosen per pixel:
    line  only where an edge is line-like (coherence-gated, as B2's edge redraw) AND long (skeleton of
          the gate at CE resolution >= LINE_MIN_TEXELS long): the high band of the sharp models
          (animev3, x4plus_anime, general_v3), the best one per 8-texel tile by sharpness minus
          overshoot and invented-edge penalties; then ringing/halo suppression (luminance clamped to the
          local range of the general-wdn A' result, +-1 texel, as a grey offset); the class grain's finest
          octave is kept under the gate.
    grain everywhere else: per material class (class masks where they exist, otherwise automatic
          classification by local statistics) the high band of the models best for that class, mixed
          and re-normalised to their local rms (averaging de-correlated GAN grain would thin it).
  Skin and faces: general-wdn only, no line band, never an anime model.
  The sum goes through a DXT-grid deblock (excess step at CE block boundaries spread as a ramp) and is
  back-projected to CE's low band once more.

Alternative ("select", learned-free): per 8-texel patch (half overlap, sin^2 windows) pick the candidate
with the best no-reference score: sharpness (log Laplacian energy) minus a naturalness penalty (fine to
mid octave amplitude ratio away from a 1/f^2.5 spectrum) minus the style checks measured on that patch
(low-band dE00, DXT grid ratio, invented-edge share). Skin patches: general-wdn only.

Keep-CE-shape mask (invented_check.keep_mask): small lit decals, glyph lights and self-illumination get a
plain bicubic resample of CE in fusion, select and B2 (every ML model draws glyph strokes into them).
Covenant characters without a class mask (the Elite): plate by the multipurpose colour-change rule, the
hex undersuit as fabric by its blue-teal colour (cov_char_classes).

At the 4K target: fusion 4x -> Lanczos -> low-band back-projection ("fusion"), and the same base through
B2's detail stage, second version (detail2.detail2: one edge pass aligned to the base's edge, detail only
in the octave(s) above the base's Nyquist, amplitude from the base's and CE's own local texture energy,
none on CE ramps, flat strips or dark gaps) ("fusion+B2"). Every result also goes through the
invented-texture check (invented_check.check); metrics.json["invented"].
Outputs: <work>/textures/out/fusion/<tex>/ (images, metrics.json; local only) and sheets and tables
in <work>/textures/out/fusion/doc.
"""
import json
import os
import sys
import time

import cv2
import numpy as np
from PIL import Image
from skimage.measure import label as cc_label
from skimage.morphology import skeletonize

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import detail2 as D  # noqa: E402
import invented_check as IC  # noqa: E402
import mars as M  # noqa: E402
import sheet  # noqa: E402
import texlib as T  # noqa: E402

S = 4
CACHE = os.path.join(T.WORK, "out/fusion/cache")
OUT = os.path.join(T.WORK, "out/fusion")
BEFORE = os.path.join(OUT, "review/before_b2fix")  # outputs of the first B2 detail stage, for the sheets
DOC = os.path.join(T.WORK, "out/fusion/doc")
MASKS = T.E.mod("prism_class_masks", "tags", "materials", "masks") if T.E.OUT else ""

ALL = ["general_wdn_v3", "general_v3", "x4plus", "x4plus_anime", "animev3", "nomos8ksc", "hat_s_nomos",
       "swinir_l_gan"]
LINE_MODELS = ["animev3", "x4plus_anime", "general_v3"]
GENTLE = "general_wdn_v3"
SHORT = {"general_wdn_v3": "wdn", "nomos8ksc": "nomos8ksc"}
ANIME = {"animev3", "x4plus_anime"}
LINE_MIN_TEXELS = 6
FINE_KEEP = 0.8  # share of the class grain's finest octave kept under the line gate

CLASSES = ["smooth_paint", "mottled_paint", "stone", "grass", "fabric", "skin"]
CLASS_MODELS = {
    "smooth_paint": {"general_wdn_v3": 1.0},
    "mottled_paint": {"nomos8ksc": 0.6, "general_wdn_v3": 0.4},
    "stone": {"nomos8ksc": 0.5, "hat_s_nomos": 0.25, "swinir_l_gan": 0.25},
    "grass": {"x4plus": 0.34, "general_wdn_v3": 0.33, "swinir_l_gan": 0.33},
    "fabric": {"nomos8ksc": 1.0},
    "skin": {"general_wdn_v3": 1.0},
}
MASK_CLASS = {"mjolnir_plate": "mottled_paint", "mjolnir_undersuit": "fabric", "hard_polymer": "smooth_paint",
              "rubber_grip": "fabric", "marine_bdu": "fabric", "marine_webbing": "fabric",
              "human_skin": "skin", "marine_paint": "mottled_paint", "marine_metal": "smooth_paint"}
CLASS_COL = {"smooth_paint": (90, 150, 230), "mottled_paint": (240, 170, 60), "stone": (150, 150, 150),
             "grass": (80, 200, 80), "fabric": (200, 90, 200), "skin": (250, 120, 110)}

# tex: (F to the 4K target, wrap, domain, mars kind, best single model (README table), class mask spec)
# domain: hard (paint classes), env (stone / grass), char (paint, fabric, skin), face (all skin)
CFG = {
    "chief_diffuse": (8, False, "char", "hard", "general_wdn_v3",
                      ("characters_cyborg_shaders_armor_0.png",
                       ["mjolnir_plate", "mjolnir_undersuit", "hard_polymer", "rubber_grip"], "chief_multi.png")),
    "b30_metal_panels": (8, True, "hard", "hard", "general_wdn_v3", None),
    "a30_cliff_rock": (4, True, "env", "organic", "nomos8ksc", None),
    "a50_cov_panel": (16, True, "hard", "cov", "general_wdn_v3", None),
    "elite_torso": (8, False, "char", "hard", "general_wdn_v3", ("cov_rule", "elite_torso_multi.png")),
    "marine_body": (8, False, "char", "cloth", "nomos8ksc",
                    ("characters_marine_shaders_marine_body_0.png", ["marine_bdu", "human_skin", "hard_polymer"], None)),
    "a30_detail_grass": (8, True, "env", "organic", "general_wdn_v3", None),
    "marine_face": (16, False, "face", "skin", "general_wdn_v3", None),
}
gw = M.gw


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def up_ce(a, wrap, f=S, interp=cv2.INTER_LINEAR):
    h, w = a.shape[:2]
    if not wrap:
        return cv2.resize(a, (w * f, h * f), interpolation=interp)
    p = 2
    ap = T.pad_wrap(a, p)
    u = cv2.resize(ap, (ap.shape[1] * f, ap.shape[0] * f), interpolation=interp)
    return u[p * f:p * f + h * f, p * f:p * f + w * f]


def boxmean(a, k, wrap):
    """Mean over a k x k px window (wrap or reflect)."""
    if wrap:
        p = k
        return cv2.blur(T.pad_wrap(a, p), (k, k))[p:-p, p:-p]
    return cv2.blur(a, (k, k), borderType=cv2.BORDER_REFLECT)


# ---------------------------------------------------------------- metrics beyond texlib
def grid_ratio(img, F):
    """DXT block-grid ratio at F px per CE texel: mean |gradient| across CE 4x4 block boundaries / elsewhere."""
    L = T.lum(img)
    B = 4 * F
    dx = np.abs(L[:, 1:] - L[:, :-1])
    dy = np.abs(L[1:, :] - L[:-1, :])
    bx = (np.arange(dx.shape[1]) + 1) % B == 0
    by = (np.arange(dy.shape[0]) + 1) % B == 0
    num = dx[:, bx].mean() + dy[by, :].mean()
    den = dx[:, ~bx].mean() + dy[~by, :].mean()
    return float(num / (den + 1e-9))


def checks(up, ce):
    f = up.shape[0] // ce.shape[0]
    r = T.fidelity(up, ce)
    r["grid"] = grid_ratio(up, f)
    return r


GATES = {"deL_mean": ("<=", 0.15), "de_mean": ("<=", 1.2), "hue_deg": ("<=", 2.0), "ssim_L": (">=", 0.98),
         "grid": ("<=", 1.05), "invented_edges": ("<=", 0.01)}


def passes(r, grass=False):
    fails = []
    for k, (op, v) in GATES.items():
        if grass and k == "deL_mean":
            v = 0.5
        if grass and k == "de_mean":
            v = 2.1
        if (op == "<=" and r[k] > v) or (op == ">=" and r[k] < v):
            fails.append(k)
    if not (-5.0 <= r["slope_hi"] <= -2.0):
        fails.append("slope_hi")
    return fails


# ---------------------------------------------------------------- material classes (CE resolution)
def class_maps(name, ce, wrap):
    """Soft memberships (h, w, len(CLASSES)), summing to 1."""
    F, _, domain, _, _, mask = CFG[name]
    h, w = ce.shape[:2]
    P = np.zeros((h, w, len(CLASSES)), np.float32)
    ci = {c: i for i, c in enumerate(CLASSES)}
    if domain == "face":
        P[..., ci["skin"]] = 1
        return P, "face: whole map skin"
    if mask is not None and mask[0] == "cov_rule":
        return cov_char_classes(ce, wrap, domain, mask[1])
    if mask is not None:
        fn, slots, mpf = mask
        m = np.asarray(Image.open(os.path.join(MASKS, fn)).convert("RGBA")).astype(np.float32) / 255
        if m.shape[:2] != (h, w):
            m = cv2.resize(m, (w, h), interpolation=cv2.INTER_LINEAR)
        m = m[..., :len(slots)]
        if mpf:  # the renderer's live rule: slot A where the multipurpose blue channel is above 0.35..0.65
            mp = T.load(os.path.join(T.WORK, "src", mpf))[0]
            plate = smoothstep(0.35, 0.65, mp[..., 2])
            rest = m[..., 1:]
            rest = rest / np.maximum(rest.sum(-1, keepdims=True), 1e-6)
            m = np.concatenate([plate[..., None], rest * (1 - plate[..., None])], -1)
        for i, s in enumerate(slots):
            P[..., ci[MASK_CLASS[s]]] += m[..., i]
        cov = P.sum(-1)
        auto, _ = auto_classes(ce, wrap, domain)
        P = P + auto * (1 - np.clip(cov, 0, 1))[..., None]  # texels outside every island
        return P / np.maximum(P.sum(-1, keepdims=True), 1e-6), "class mask " + fn
    P, how = auto_classes(ce, wrap, domain)
    return P, how


def cov_char_classes(ce, wrap, domain, mpf):
    """Covenant characters (no class mask yet): armour plate where the multipurpose colour-change channel is
    on (Xbox order B, the renderer's rule 0.35..0.65), the bodysuit (the Elite's hex undersuit, cov_bodysuit)
    where it is off and CE's colour is the suit's blue-teal (Lab hue 185..250 deg, chroma >= 5, L >= 12),
    automatic paint classes elsewhere (dark trim, gem, glyph lights)."""
    mp = T.load(os.path.join(T.WORK, "src", mpf))[0]
    plate = smoothstep(0.35, 0.65, mp[..., 2])
    lab = T.srgb_to_lab(ce).astype(np.float32)
    a, b = gw(lab[..., 1], 1.5, wrap), gw(lab[..., 2], 1.5, wrap)
    ch = np.hypot(a, b)
    hue = (np.degrees(np.arctan2(b, a)) + 360) % 360
    suit = (smoothstep(3.5, 6.0, ch) * smoothstep(175, 190, hue) * (1 - smoothstep(245, 260, hue))
            * smoothstep(9, 14, lab[..., 0]) * (1 - plate))
    suit = gw(suit, 0.7, wrap)
    auto, _ = auto_classes(ce, wrap, domain)
    auto[..., CLASSES.index("skin")] = 0  # no human skin tones on a Covenant
    auto = auto / np.maximum(auto.sum(-1, keepdims=True), 1e-6)
    P = auto * (1 - suit)[..., None]
    P[..., CLASSES.index("fabric")] += suit
    return P / np.maximum(P.sum(-1, keepdims=True), 1e-6), "cov rule " + mpf + " (plate) + bodysuit by hue/chroma"


def mp_for(name):
    """The map's multipurpose (Xbox channel order), if the study has one."""
    mask = CFG[name][5]
    mpf = None if mask is None else mask[-1] if mask[0] == "cov_rule" else mask[2]
    return T.load(os.path.join(T.WORK, "src", mpf))[0] if mpf else None


def keep_ce(name, ce, wrap):
    """Keep-CE-shape mask (CE resolution): small lit decals, glyph lights and self-illumination."""
    return IC.keep_mask(ce, wrap, mp_for(name), decals=CFG[name][2] != "env")


def apply_keep(out, ce, keep, wrap, f=S):
    """Plain bicubic resample of CE under the keep mask (no ML band: every model draws glyphs into them)."""
    if keep.max() <= 0:
        return out
    k = up_ce(keep, wrap, f)[..., None]
    return k * IC.plain(ce, f, wrap) + (1 - k) * out


def auto_classes(ce, wrap, domain):
    """Texture type by local statistics: detail energy (std of L above 1 texel, over 2 texels), hue and
    chroma (grass green, skin), domain limits which classes may appear."""
    lab = T.srgb_to_lab(ce).astype(np.float32)
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    e = np.sqrt(gw((L - gw(L, 1.0, wrap)) ** 2, 2.0, wrap))
    las = gw(a, 1.5, wrap)
    lbs = gw(b, 1.5, wrap)
    ch = np.hypot(las, lbs)
    hue = (np.degrees(np.arctan2(lbs, las)) + 360) % 360
    ci = {c: i for i, c in enumerate(CLASSES)}
    P = np.zeros(ce.shape[:2] + (len(CLASSES),), np.float32)
    rough = smoothstep(1.2, 3.5, e)  # Lab L units
    if domain == "env":
        green = smoothstep(4, 9, ch) * smoothstep(85, 105, hue) * (1 - smoothstep(165, 185, hue))
        P[..., ci["grass"]] = green
        P[..., ci["stone"]] = (1 - green) * np.maximum(rough, 0.5)
        P[..., ci["smooth_paint"]] = (1 - green) * (1 - np.maximum(rough, 0.5))
        how = "auto: grass by green hue, stone vs smooth by detail energy"
    else:
        skin = np.zeros_like(L)
        if domain == "char":  # human skin tones only (Elite / Covenant purples are not caught)
            skin = (smoothstep(8, 14, ch) * (1 - smoothstep(40, 50, ch)) * smoothstep(20, 35, hue)
                    * (1 - smoothstep(70, 85, hue)) * smoothstep(25, 35, L) * (1 - smoothstep(80, 90, L)))
        P[..., ci["skin"]] = skin
        P[..., ci["mottled_paint"]] = (1 - skin) * rough
        P[..., ci["smooth_paint"]] = (1 - skin) * (1 - rough)
        how = "auto: smooth vs mottled paint by detail energy" + (", skin by hue/chroma" if domain == "char" else "")
    P = np.stack([gw(P[..., i], 1.0, wrap) for i in range(P.shape[-1])], -1)
    return P / np.maximum(P.sum(-1, keepdims=True), 1e-6), how


# ---------------------------------------------------------------- line gate (B2's, plus a length test)
def line_gate(ref, wrap, skin_up):
    L = T.lum(ref)
    Ls = gw(L, 0.45 * S, wrap)
    Ls, coh = M.straighten(Ls, S, wrap)
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * S + 1, 2 * S + 1))
    c = cv2.dilate(Ls, ker, borderType=cv2.BORDER_REFLECT) - cv2.erode(Ls, ker, borderType=cv2.BORDER_REFLECT)
    c_lo = max(0.015, float(np.percentile(c[::4, ::4], 45)))
    c_hi = max(c_lo + 0.02, float(np.percentile(c[::4, ::4], 75)))
    w = np.clip((c - c_lo) / (c_hi - c_lo), 0, 1) * np.clip((coh - 0.3) / 0.3, 0, 1)
    # long: skeleton of the gate at CE resolution, components >= LINE_MIN_TEXELS skeleton texels
    wc = T.boxdown(w, S)
    sk = skeletonize(wc > 0.25)
    lab = cc_label(sk, connectivity=2)
    sizes = np.bincount(lab.ravel())
    keep = np.isin(lab, np.nonzero(sizes >= LINE_MIN_TEXELS)[0]) & sk
    longm = cv2.dilate(keep.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(np.float32)
    longm = up_ce(longm, wrap, interp=cv2.INTER_NEAREST)
    longm = cv2.dilate(longm, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (S + 1, S + 1)))
    g = gw(w * longm, 0.5 * S, wrap) * (1 - smoothstep(0.15, 0.35, skin_up))
    return np.clip(g, 0, 1), w


def strong_edges(L, q=0.9):
    g = cv2.GaussianBlur(L, (0, 0), 1.0)
    m = np.hypot(cv2.Sobel(g, cv2.CV_32F, 1, 0), cv2.Sobel(g, cv2.CV_32F, 0, 1))
    return m, m > np.quantile(m, q)


def ce_edge_dist(ce, wrap):
    lc = T.lum(up_ce(ce, wrap, interp=cv2.INTER_LANCZOS4))
    m, _ = strong_edges(lc)
    sc = m > np.quantile(m, 0.8)
    return cv2.distanceTransform((~sc).astype(np.uint8), cv2.DIST_L2, 3)


def overshoot(Lc, Lref, wrap):
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * S + 1, 2 * S + 1))
    lo = cv2.erode(Lref, ker, borderType=cv2.BORDER_REFLECT)
    hi = cv2.dilate(Lref, ker, borderType=cv2.BORDER_REFLECT)
    return np.maximum(Lc - hi, 0) + np.maximum(lo - Lc, 0), lo, hi


# ---------------------------------------------------------------- fusion
def load_cands(name, ce, wrap):
    A = {}
    for m in ALL:
        u = T.load(os.path.join(CACHE, name, m + ".png"))[0]
        A[m] = T.backproject_low(u, ce, 10, wrap=wrap)
    return A


def hp(x, wrap):
    return x - np.stack([gw(x[..., i], 0.6 * S, wrap) for i in range(3)], -1)


def local_rms(x, wrap, s=2.0 * S):
    return np.sqrt(gw(T.lum(x) ** 2, s, wrap) + 1e-8)


def fuse(name, ce, A, wrap):
    P, how = class_maps(name, ce, wrap)
    Pu = up_ce(P, wrap)
    skin_up = Pu[..., CLASSES.index("skin")]
    ref = A[GENTLE]
    low = ref - hp(ref, wrap)
    H = {m: hp(A[m], wrap) for m in ALL}
    # grain band per class, mixed and re-normalised to the weighted local rms of its members
    grain = np.zeros_like(ref)
    for ci, cname in enumerate(CLASSES):
        wc = Pu[..., ci]
        if wc.max() < 1e-3:
            continue
        mix = sum(wt * H[m] for m, wt in CLASS_MODELS[cname].items())
        tgt = sum(wt * local_rms(H[m], wrap) for m, wt in CLASS_MODELS[cname].items())
        if len(CLASS_MODELS[cname]) > 1:
            mix = mix * np.clip(tgt / local_rms(mix, wrap), 1.0, 1.3)[..., None]
        grain += wc[..., None] * mix
    # line band: best sharp model per 8-texel tile
    gate, gate_raw = line_gate(ref, wrap, skin_up)
    dist = ce_edge_dist(ce, wrap)
    Lref = T.lum(ref)
    tile = 8 * S
    scores = []
    for m in LINE_MODELS:
        Lc = T.lum(A[m])
        gm, se = strong_edges(Lc)
        ov, _, _ = overshoot(Lc, Lref, wrap)
        inv = (se & (dist > S)).astype(np.float32)
        sharp = boxmean(gm * gate, tile, wrap) / (boxmean(gate, tile, wrap) + 1e-4)
        pen = boxmean(ov * gate, tile, wrap) / (boxmean(gate, tile, wrap) + 1e-4)
        invs = boxmean(inv, tile, wrap)
        grid = tile_grid(Lc, tile, wrap)
        scores.append(sharp - 6.0 * pen - 4.0 * invs - 0.3 * np.maximum(grid - 1.0, 0))
    sc = np.stack(scores, -1)
    sc = (sc - sc.max(-1, keepdims=True)) / (np.abs(sc).mean() + 1e-6)
    wl = np.exp(sc * 8)
    wl /= wl.sum(-1, keepdims=True)
    wl = np.stack([gw(wl[..., i], S, wrap) for i in range(len(LINE_MODELS))], -1)
    wl /= wl.sum(-1, keepdims=True)
    line = sum(wl[..., i:i + 1] * H[m] for i, m in enumerate(LINE_MODELS))
    # the line models flatten texture, so under the gate the class grain's finest octave (below ~1/4 texel)
    # is kept on top of the line band; otherwise grainy surfaces (webbing, pouches) go smooth along every
    # long edge and read as a smeared band
    fine = grain - np.stack([gw(grain[..., i], 0.3 * S, wrap) for i in range(3)], -1)
    out = low + gate[..., None] * (line + FINE_KEEP * fine) + (1 - gate[..., None]) * grain
    # ringing / halo suppression on the line band: luminance kept within the local range of the gentle
    # A' result (+-1 texel). Applied as a grey offset: a ratio (Ln + e) / (Lo + e) explodes where the sum
    # dips to or below black (high band on a dark edge) and left isolated bright / yellow specks
    Lo = T.lum(out)
    _, lo, hi = overshoot(Lo, Lref, wrap)
    tol = 0.01
    Lcl = np.clip(Lo, lo - tol, hi + tol)
    Ln = Lo + gate * (Lcl - Lo)
    out = np.clip(out + (Ln - Lo)[..., None], 0, 1)
    out, db = deblock_auto(out, wrap)
    out = T.backproject_low(out, ce, 4, wrap=wrap)
    out = apply_keep(out, ce, keep_ce(name, ce, wrap), wrap)
    # attribution map at CE resolution: share of each model in the final high band
    share = {m: np.zeros(ce.shape[:2], np.float32) for m in ALL}
    gc = T.boxdown(gate, S)
    for i, m in enumerate(LINE_MODELS):
        share[m] += gc * T.boxdown(wl[..., i], S)
    for ci, cname in enumerate(CLASSES):
        for m, wt in CLASS_MODELS[cname].items():
            share[m] += (1 - gc) * P[..., ci] * wt
    return out, {"deblock": db, "P": P, "how": how, "gate": gate, "gate_raw": gate_raw, "wl": wl, "share": share}


def tile_grid(Lc, k, wrap):
    """Per-pixel map of the DXT block-grid ratio over a k x k window (4x grid: blocks are 4S px)."""
    H, W = Lc.shape
    B = 4 * S
    bx = ((np.arange(W) + 1) % B == 0)[None, :]
    by = ((np.arange(H) + 1) % B == 0)[:, None]
    dx = np.abs(np.diff(Lc, axis=1, append=Lc[:, -1:]))
    dy = np.abs(np.diff(Lc, axis=0, append=Lc[-1:, :]))
    gb = boxmean(dx * bx + dy * by, k, wrap) / (2.0 / B)
    gn = boxmean(dx * ~bx + dy * ~by, k, wrap) / (2.0 * (1 - 1.0 / B))
    return gb / (gn + 1e-5)


def deblock(img, wrap, B=4 * S, c=1.25, strength=1.0):
    """DXT grid removal: at each CE 4x4 block boundary, the part of the step that exceeds the steps beside
    it (c times) is spread over +-4 px as a linear ramp (as a video deblocking filter). Real edges are a few px
    wide at 4x, so their neighbouring steps are as large and they are left alone."""
    out = img.copy()
    for ax in (1, 0):
        a = np.moveaxis(out, ax, 0)  # boundaries along axis 0 of the view
        n = a.shape[0]
        bs = np.arange(0 if wrap else B, n if wrap else n - 1, B)
        if len(bs) == 0:
            continue
        idx = lambda k: (bs + k) % n  # noqa: E731
        L = T.lum(a)
        d = L[idx(0)] - L[idx(-1)]
        nb = 0.5 * (np.abs(L[idx(-1)] - L[idx(-2)]) + np.abs(L[idx(1)] - L[idx(0)]))
        k = strength * np.clip(1 - c * nb / (np.abs(d) + 1e-6), 0, 1)
        e = (a[idx(0)] - a[idx(-1)]) * k[..., None]
        for j in range(4):
            wj = 0.5 * (4 - j) / 4
            a[idx(-1 - j)] += wj * e
            a[idx(j)] -= wj * e
    return np.clip(out, 0, 1)


def deblock_auto(img, wrap, target=1.02):
    """The gentlest deblock setting that brings the map's grid ratio to <= target (none if already there):
    a fixed setting would carve a reverse grid (ratio < 1) into maps that have no grid."""
    if grid_ratio(img, S) <= target:
        return img, None
    # fine steps: a coarse jump overshoots into a reverse grid (ratio < 1, over-smoothed block borders)
    for c, st in ((3.0, 0.25), (3.0, 0.5), (3.0, 0.75), (3.0, 1.0), (2.0, 0.5), (2.0, 0.75), (2.0, 1.0),
                  (1.5, 0.75), (1.5, 1.0), (1.25, 0.5), (1.25, 0.75), (1.25, 1.0)):
        o = deblock(img, wrap, c=c, strength=st)
        if grid_ratio(o, S) <= target:
            return o, (c, st)
    return o, (c, st)


# ---------------------------------------------------------------- learned-free per-patch selection
def select(name, ce, A, wrap):
    P, _ = class_maps(name, ce, wrap)
    skin = up_ce(P[..., CLASSES.index("skin")], wrap)
    patch = 8 * S
    dist = ce_edge_dist(ce, wrap)
    H, W = ce.shape[0] * S, ce.shape[1] * S
    tgt = T.srgb_to_lab(gw(ce, 0.8, wrap))
    cost = []
    for m in ALL:
        Lc = T.lum(A[m])
        lap = cv2.Laplacian(gw(Lc, 0.5, wrap), cv2.CV_32F)
        sharp = np.log(boxmean(lap ** 2, patch, wrap) + 1e-7)
        f1 = Lc - gw(Lc, 1.0, wrap)
        f2 = gw(Lc, 1.0, wrap) - gw(Lc, 2.0, wrap)
        ratio = np.sqrt((boxmean(f1 ** 2, patch, wrap) + 1e-8) / (boxmean(f2 ** 2, patch, wrap) + 1e-8))
        natural = np.abs(np.log(ratio / 0.42))
        grid = tile_grid(Lc, patch, wrap)
        d = T.boxdown(A[m], S)
        del_ = T.ciede2000(tgt, T.srgb_to_lab(gw(d, 0.8, wrap))).astype(np.float32)
        delu = up_ce(boxmean(del_, 8, wrap), wrap)
        _, se = strong_edges(Lc)
        inv = boxmean((se & (dist > S)).astype(np.float32), patch, wrap) / (boxmean(se.astype(np.float32), patch, wrap) + 1e-3)
        c = (-0.5 * sharp + 3.0 * natural + 8.0 * np.maximum(grid - 1.05, 0) + 4.0 * np.maximum(delu - 0.15, 0)
             + 20.0 * np.maximum(inv - 0.01, 0))
        if m != GENTLE:
            c = c + 50.0 * skin
        if m in ANIME:
            c = c + 50.0 * smoothstep(0.05, 0.2, skin)
        cost.append(c)
    cost = np.stack(cost, -1)
    # patch centres every half patch; sin^2 windows sum to one
    step = patch // 2
    pick = cost[step // 2::step, step // 2::step].argmin(-1)
    onehot = np.stack([(pick == i).astype(np.float32) for i in range(len(ALL))], -1)
    wts = cv2.resize(onehot, (W, H), interpolation=cv2.INTER_LINEAR)
    wts = np.stack([gw(wts[..., i], step / 2, wrap) for i in range(len(ALL))], -1)
    wts /= wts.sum(-1, keepdims=True)
    out = sum(wts[..., i:i + 1] * A[m] for i, m in enumerate(ALL))
    out, db = deblock_auto(out, wrap)
    out = T.backproject_low(out, ce, 4, wrap=wrap)
    out = apply_keep(out, ce, keep_ce(name, ce, wrap), wrap)
    share = {m: T.boxdown(wts[..., i], S) for i, m in enumerate(ALL)}
    return out, {"deblock": db, "pick": pick, "share": share}


# ---------------------------------------------------------------- pictures
def share_img(share, ce_shape):
    pal = {"general_wdn_v3": (90, 150, 230), "general_v3": (60, 220, 220), "x4plus": (80, 200, 80),
           "x4plus_anime": (255, 60, 60), "animev3": (255, 140, 0), "nomos8ksc": (200, 90, 200),
           "hat_s_nomos": (150, 60, 240), "swinir_l_gan": (230, 230, 80)}
    img = np.zeros(ce_shape[:2] + (3,), np.float32)
    for m, s in share.items():
        img += s[..., None] * np.array(pal[m], np.float32) / 255
    return img, pal


def class_img(P):
    return sum(P[..., i:i + 1] * np.array(CLASS_COL[c], np.float32) / 255 for i, c in enumerate(CLASSES))


def lit(albedo, F, wrap, bump):
    """One directional light; the same normal recipe for every method (CE bump integrated + a gentle
    luminance height of the method's own albedo), so the lit crops compare albedo detail fairly."""
    Hh, Ww = albedo.shape[:2]
    hgt = np.zeros((Hh, Ww), np.float32)
    if bump is not None:
        hb = M.height_from_normal(bump)
        hgt += cv2.resize(hb, (Ww, Hh), interpolation=cv2.INTER_CUBIC) * (Ww / bump.shape[1])
    La = T.lum(albedo)
    hgt += (La - gw(La, 2 * F, wrap)) * 255 * 0.25
    n = M.normal_from_height(hgt, strength=0.5 * 4 / F, wrap=wrap)
    return M.lit_preview(albedo, n, np.full((Hh, Ww), 0.55, np.float32))


def pick_window(score, cs, avoid=None):
    """Top-left (CE px) of the cs x cs window with the highest mean score (CE resolution)."""
    k = cs
    s = score.copy()
    if avoid is not None:
        ax, ay = avoid
        s[max(0, ay - k):ay + 2 * k, max(0, ax - k):ax + 2 * k] = s.min() - 1
    m = cv2.blur(s, (k, k), borderType=cv2.BORDER_CONSTANT)
    h, w = s.shape
    m[:k // 2] = m[h - k // 2:] = -1e9
    m[:, :k // 2] = m[:, w - k // 2:] = -1e9
    y, x = np.unravel_index(np.argmax(m), m.shape)
    return int(np.clip(x - k // 2, 0, w - k)), int(np.clip(y - k // 2, 0, h - k))


# ---------------------------------------------------------------- driver
def run(name, do4k):
    F, wrap, domain, kind, best, _ = CFG[name]
    t0 = time.time()
    ce, _ = T.load(os.path.join(T.WORK, "src", name + ".png"))
    bumpf = os.path.join(T.WORK, "src", name + "_bump.png")
    bump = T.load(bumpf)[0] if os.path.exists(bumpf) and kind != "organic" else None
    od = os.path.join(OUT, name)
    os.makedirs(od, exist_ok=True)
    A = load_cands(name, ce, wrap)
    fu, info = fuse(name, ce, A, wrap)
    se, sinfo = select(name, ce, A, wrap)
    T.save(os.path.join(od, "fusion_4x.png"), fu)
    T.save(os.path.join(od, "select_4x.png"), se)
    T.save(os.path.join(od, "diag_classes_ce.png"), class_img(info["P"]))
    T.save(os.path.join(od, "diag_line_gate_4x.png"), info["gate"])
    T.save(os.path.join(od, "diag_fusion_models_ce.png"), share_img(info["share"], ce.shape)[0])
    T.save(os.path.join(od, "diag_select_models_ce.png"), share_img(sinfo["share"], ce.shape)[0])
    res = {"_meta": {"F": F, "wrap": wrap, "domain": domain, "classes": info["how"], "best_single": best,
                     "line_px_share": float((info["gate"] > 0.5).mean()), "deblock_fusion": info["deblock"],
                     "deblock_select": sinfo["deblock"],
                     "fusion_model_share": {m: float(v.mean()) for m, v in info["share"].items()},
                     "select_model_share": {m: float(v.mean()) for m, v in sinfo["share"].items()}}}
    r4 = {}
    for m in ALL:
        r4["A' " + m] = checks(A[m], ce)
    Adb = T.backproject_low(deblock_auto(A[best], wrap)[0], ce, 4, wrap=wrap)  # what the deblock alone does
    r4["A' " + best + " +deblock"] = checks(Adb, ce)
    r4["fusion"] = checks(fu, ce)
    r4["select"] = checks(se, ce)
    keep = keep_ce(name, ce, wrap)
    mp = mp_for(name)
    decals = domain != "env"
    no_edge = info["P"][..., CLASSES.index("fabric")] + info["P"][..., CLASSES.index("skin")]  # no redraw on fabric or skin
    T.save(os.path.join(od, "diag_keep_ce.png"), np.clip(ce * (1 - 0.6 * keep[..., None]) + 0.6 * keep[..., None] * np.array([1, 0, 0], np.float32), 0, 1))
    # B2's detail stage (detail2): on the A' base (straightening + crisping) and on fusion (crisping only:
    # fusion's line band already drew the lines)
    b2_4 = D.detail2(ce, S, A[best], kind, wrap, P=info["P"], keep=keep, redrawn=False, no_edge=no_edge)
    fb2_4 = D.detail2(ce, S, fu, kind, wrap, P=info["P"], keep=keep, redrawn=True, no_edge=no_edge)
    r4["B2"] = checks(b2_4["albedo"], ce)
    r4["fusion+B2"] = checks(fb2_4["albedo"], ce)
    T.save(os.path.join(od, "B2_4x.png"), b2_4["albedo"])
    T.save(os.path.join(od, "fusionB2_4x.png"), fb2_4["albedo"])
    res["4x"] = r4
    inv = {}

    def icheck(tag, label, img):
        r = IC.check(img, ce, wrap, mp=mp, keep=keep, decals=decals, detail=(label == "fusion+B2"))
        mp_ = r.pop("_maps", None)
        inv.setdefault(tag, {})[label] = r
        return mp_
    before4 = os.path.join(BEFORE, name, "fusionB2_4x.png")
    V4 = {"CE nearest": T.nearest(ce, S), f"A' {SHORT[best]}": A[best], "B2": b2_4["albedo"],
          "fusion": fu, "fusion+B2": fb2_4["albedo"], "select": se,
          "anime: A' animev3": A["animev3"]}
    if os.path.exists(before4):
        V4["fB2 before: fusion+B2 (first B2)"] = T.load(before4)[0]
    if F == S:
        for lab, img in V4.items():
            if not lab.startswith("CE"):
                icheck("4x", lab.split(":")[0] if lab.startswith(("anime", "fB2")) else lab, img)
    sheets(name, ce, info, V4, S, wrap, bump, "4x", cs=64)
    print(name, "4x done", round(time.time() - t0), "s", flush=True)
    if do4k and F != S:
        Hh, Ww = ce.shape[0] * F, ce.shape[1] * F

        def to_target(x):
            return T.backproject_low(np.clip(cv2.resize(x, (Ww, Hh), interpolation=cv2.INTER_LANCZOS4), 0, 1),
                                     ce, 4, wrap=wrap)
        Ab = to_target(A[best])
        fuT = apply_keep(to_target(fu), ce, keep, wrap, F)  # keep-CE-shape again at the target size
        seT = apply_keep(to_target(se), ce, keep, wrap, F)
        b2T = D.detail2(ce, F, A[best], kind, wrap, P=info["P"], keep=keep, redrawn=False, no_edge=no_edge)["albedo"]
        t1 = time.time()
        fb2 = D.detail2(ce, F, fu, kind, wrap, P=info["P"], keep=keep, redrawn=True, no_edge=no_edge)
        fb2T = fb2["albedo"]
        res["_meta"]["detail2_seconds"] = round(time.time() - t1)
        T.save(os.path.join(od, "fusion_target.png"), fuT)
        T.save(os.path.join(od, "fusionB2_target.png"), fb2T)
        T.save(os.path.join(od, "B2_target.png"), b2T)
        T.save(os.path.join(od, "diag_detail_amp_target.png"), np.clip(fb2["amp"][::F // 2, ::F // 2] * 255 / 8, 0, 1))
        rT = {f"A' {best}": checks(Ab, ce), f"A' {best} +deblock": checks(to_target(Adb), ce), "B2": checks(b2T, ce), "fusion": checks(fuT, ce),
              "select": checks(seT, ce), "fusion+B2": checks(fb2T, ce)}
        res["target"] = rT
        VT = {"CE nearest": T.nearest(ce, F), f"A' {SHORT[best]}": Ab, "B2": b2T, "fusion": fuT,
              "fusion+B2": fb2T, "select": seT}
        beforeT = os.path.join(BEFORE, name, "fusionB2_target.png")
        if os.path.exists(beforeT):
            VT["fB2 before: fusion+B2 (first B2)"] = T.load(beforeT)[0]
        maps = None
        for lab, img in VT.items():
            if not lab.startswith("CE"):
                m_ = icheck("target", lab.split(":")[0] if lab.startswith("fB2") else lab, img)
                maps = m_ if m_ is not None else maps
        if maps is not None:
            check_img(od, ce, maps)
        sheets(name, ce, info, VT, F, wrap, bump, "target", cs=512 // F)
    elif do4k:
        res["target"] = {f"A' {best}": r4["A' " + best], f"A' {best} +deblock": r4["A' " + best + " +deblock"], "B2": r4["B2"], "fusion": r4["fusion"],
                         "select": r4["select"], "fusion+B2": r4["fusion+B2"]}
        res["_meta"]["target_note"] = "4x is the 4K target for this map"
        inv["target"] = inv.get("4x", {})
    res["invented"] = inv
    res["_meta"]["seconds"] = round(time.time() - t0)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(name, "done", round(time.time() - t0), "s", flush=True)


def check_img(od, ce, maps):
    """Where the invented-texture check looks (CE resolution, over CE): low-texture texels (blue), CE ramps
    (green), dark regions (purple), keep-CE-shape mask (red), excess-energy failures (yellow)."""
    img = ce * 0.45
    for key, col in (("low", (0.2, 0.35, 0.9)), ("ramp", (0.2, 0.85, 0.3)), ("dark", (0.6, 0.2, 0.7)),
                     ("keep", (1.0, 0.1, 0.1)), ("excess", (1.0, 0.9, 0.1))):
        m = maps[key].astype(np.float32)
        if key == "keep":
            m = (m > 0.5).astype(np.float32)
        img = img * (1 - 0.5 * m[..., None]) + 0.5 * m[..., None] * np.array(col, np.float32)
    T.save(os.path.join(od, "diag_check_regions_ce.png"), img)


def sheets(name, ce, info, V, F, wrap, bump, tag, cs):
    """Rows: lines, grain, face/skin (where there is any), lit (on the line window). Crops 1:1."""
    od = os.path.join(OUT, name, tag)
    os.makedirs(od, exist_ok=True)
    cs = min(cs, ce.shape[0], ce.shape[1])
    gate_ce = T.boxdown(info["gate"], S)
    P = info["P"]
    skin = P[..., CLASSES.index("skin")]
    rows = []
    if gate_ce.max() > 0.05:
        rows.append(("lines", pick_window(gate_ce, cs)))
    L = T.lum(ce)
    e = np.sqrt(gw((L - gw(L, 1.0, wrap)) ** 2, 2.0, wrap)) * (1 - gate_ce) * (1 - skin)
    rows.append(("grain", pick_window(e, cs, rows[0][1] if rows else None)))
    if CFG[name][2] == "face":  # eyes, nose and mouth (the ML study's window), not the hairline
        cx, cy = 40 + 16, 36 + 20
        rows = [("face", (min(max(cx - cs // 2, 0), ce.shape[1] - cs), min(max(cy - cs // 2, 0), ce.shape[0] - cs)))]
    elif skin.max() > 0.3:
        rows.append(("face/skin", pick_window(skin, cs)))
    rows.append(("lit", rows[0][1]))
    lits = {}
    items = []
    cols = len(V)
    for rname, (x, y) in rows:
        for lab, img in V.items():
            if rname == "lit":
                if lab not in lits:
                    sub = img[y * F:(y + cs) * F, x * F:(x + cs) * F]
                    lits[lab] = lit(sub, F, False, None if bump is None else bump[y:y + cs, x:x + cs])
                crop = lits[lab]
            else:
                crop = img[y * F:(y + cs) * F, x * F:(x + cs) * F]
            p = os.path.join(od, f"{rname.replace('/', '_')}_{lab.split()[0].replace('+', 'p')}.png")
            if lab.startswith("A'"):
                p = os.path.join(od, f"{rname.replace('/', '_')}_best.png")
            if lab.startswith("anime:"):
                p = os.path.join(od, f"{rname.replace('/', '_')}_anime.png")
            if lab.startswith("fB2 before"):
                p = os.path.join(od, f"{rname.replace('/', '_')}_fB2before.png")
            T.save(p, crop)
            items.append((f"{rname} ({x},{y}) {lab}" if lab.startswith("CE") else lab, p))
    os.makedirs(DOC, exist_ok=True)
    sheet.make(os.path.join(DOC, f"{name}_{tag}_sheet.png"), cols, items)


if __name__ == "__main__":
    args = sys.argv[1:]
    do4k = "--4k" in args
    names = [a for a in args if not a.startswith("--")] or list(CFG)
    for n in names:
        run(n, do4k)
