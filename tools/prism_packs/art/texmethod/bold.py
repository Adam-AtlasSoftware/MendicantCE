"""PRISM v2 "Bold" textures (display.prism_material_definition = bold): the Enhanced map with the material
definition that reads at play distance (3-8 m), CE's layout, hue, chroma and designs kept. Production version of
the texvis_bold.py prototype, with its four fixes:

  1. ramps excluded: no sharpening at all on CE's smooth ramps (the banding check's own ramp definition,
     dilated 2 texels), where the prototype only damped it (it terraced CE's gradients);
  2. limited sharpening: every boosted L* is held inside CE's local range (+-1 texel) plus a small margin, so
     an edge gets steeper but never a halo or a second contour;
  3. class palette held: each material class's mean colour may move at most PAL_DE (CIEDE2000) from CE's;
     the excess of its mean L* shift is taken back from that class (the Chief's plate went to 2.7 before);
  4. strength chosen per map: the largest of STRENGTHS whose result passes the Bold gates (below).

Bold gates (replace the low-band pin of the faithful tiers): hue <= 2 deg; class-mean dE00 <= 2; DXT grid <=
1.05; invented edges <= 0.01; the invented-texture check fails nothing Enhanced did not, and has no more failing
ramp tiles; screen dE00 at 1.45x (3 m on 1080p) <= 2.5.

Surface map (<bitmap>.surface.png: RG normal, y up the picture; B occlusion; A roughness):
  - normal and occlusion from the remodel's geometry bake where one exists (the r2 Chief, fp arms and AR, the
    r2 Elites: CE's own UVs), never from the albedo's paint;
  - elsewhere (Marines) a structural normal only: the plate's rim (the class mask's plate against the rest,
    a 0.4-texel bevel) and the dark grooves CE paints along its strong edges; camouflage, mottling and
    painted reflections stay flat;
  - roughness per material class (paint 0.5, smooth polymer 0.42, fabric 0.85, skin 0.6), less CE's own
    specular mask; lights and glyphs 1 (no highlight).
Weathering (display.prism_weathering, its own mod): wear only on the plate's convex rims (the class border, and
the bake's convex curvature where there is one), 1-1.5 texels wide, broken up at >= 2 texels, toward the
class's own bright colour, never past +WEAR_MAX_L L*; grime only in the border creases and the bake's
cavities. Gated by the speckle, decal and contour checks.

  python -I bold.py [--jobs N] [key ...]   (keys: KEYS below; prism_packs.py's bold step)
Outputs (local only: CE pixels): <work>/textures/bold/maps/<key>/{bold.png, weathered.png, surface.png,
surface_weathered.png, metrics.json}.
"""
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fusion as FU  # noqa: E402
import invented_check as IC  # noqa: E402
import texlib as T  # noqa: E402
import texvis_bands as TB  # noqa: E402
import texvis_bold as VB  # noqa: E402
import tier_cache as TC  # noqa: E402
import tier_recipe as R  # noqa: E402
import tiers as TI  # noqa: E402

E = R.E
OUT = E.work("textures", "bold", "maps")
KEYS = dict(TB.KEYS)
KEYS.update({"marine_helmet": "characters__marine_armored__bitmaps__marine_helmet",
             "marine_arms_johnson": "characters__marine_armored__bitmaps__marine_arms_johnson",
             "elite_special": "characters__elite__elite_special__bitmaps__elite_special"})
TB.KEYS.update(KEYS)
# geometry bakes (surface maps of the r2 remodels, at CE's UVs): normal and occlusion come from these
# (the geometry bakes' surface maps, in the packs the chief_r2, fp_arms and elite_r2 steps wrote)
BAKE = {"chief": E.mod("prism_v2r2_chief_3p_maps", "tags/characters/cyborg/bitmaps/cyborg.surface.png"),
        "fp_ar": E.mod("prism_v2r2_fp_hands_ar", "tags/weapons/assault rifle/fp/bitmaps/assault rifle_fp.surface.png"),
        "fp_hands": E.mod("prism_v2r2_fp_hands_ar", "tags/characters/cyborg/fp/bitmaps/cyborg hands.surface.png")}
for _n in ("arms", "head", "head1", "legs", "legs1", "torso"):
    BAKE["elite_" + _n] = E.mod("prism_v2_elite_r2", "tags/characters/elite/bitmaps/elite %s.surface.png" % _n)
BAKE["elite_special"] = E.mod("prism_v2_elite_r2", "tags/characters/elite/elite_special/bitmaps/elite special.surface.png")

CI = {c: i for i, c in enumerate(FU.CLASSES)}
# per class: g0 (above CE, < 0.5 texel), g1 (1-4 texels), g2 (4-16 texels), roughness
CLS = {"smooth_paint": (1.0, 0.70, 0.40, 0.42),
       "mottled_paint": (1.0, 0.75, 0.40, 0.50),
       "stone": (0.8, 0.45, 0.25, 0.80),
       "grass": (0.5, 0.30, 0.15, 0.85),
       "fabric": (0.7, 0.50, 0.28, 0.85),
       "skin": (0.3, 0.18, 0.10, 0.60)}
STRENGTHS = (1.0, 1.4, 1.8, 2.2, 2.6, 3.0, 3.5, 4.0)
EPS2 = float(os.environ.get("BOLD_EPS2", "0.02"))  # guided filter eps of the material band (L*/100 squared: 0.02 keeps steps over ~14 L*)
LIMIT_MARGIN = 4.0   # L* past CE's local range a boosted texel may go (an edge steeper, no halo)
PAL_DE = 1.7         # class-mean CIEDE2000 kept under this (gate 2.0)
GATE = {"hue_deg": 2.0, "palette": 2.0, "grid": 1.05, "invented_edges": 0.01, "screen145": 2.5}
# specular anti-aliasing per map: (blur of the normal in texels, gain, cap). The AR's remodelled top rail has
# ribs a texel or two wide whose crossings caught the sun as white dots in first person at the default
# (1.5, 3.0, 0.45): its variance is taken over a wider footprint and turned into more roughness
SPEC_AA = {"fp_ar": (3.0, 5.0, 0.6)}
SPEC_AA_DEFAULT = (1.5, 3.0, 0.45)
NORMAL_CALM = {"fp_ar": 2.5}
WEAR_MAX_L = 14.0
WEAR_AMOUNT = {"smooth_paint": 0.55, "mottled_paint": 0.6, "stone": 0.3, "grass": 0.0, "fabric": 0.0, "skin": 0.0}
GRIME_AMOUNT = {"smooth_paint": 0.3, "mottled_paint": 0.3, "stone": 0.3, "grass": 0.2, "fabric": 0.35, "skin": 0.1}


def g(a, s):
    return cv2.GaussianBlur(a, (0, 0), s, borderType=cv2.BORDER_REFLECT) if s > 0 else a


def per_class(P4, col, table=CLS):
    v = np.array([table[c][col] if isinstance(table[c], tuple) else table[c] for c in FU.CLASSES], np.float32)
    return (P4 * v).sum(-1)


def up(a, W, H, interp=cv2.INTER_LINEAR):
    return cv2.resize(a.astype(np.float32), (W, H), interpolation=interp)


def plate_mask(tag, ce, mp, P):
    """CE-resolution 0..1: the hard plate (the class mask's slot A after its live channel rule; the Elite's
    colour-change plate; all of a weapon), against suit, webbing, skin and trim"""
    r = R.BASES[tag]
    h, w = ce.shape[:2]
    cl = r.get("classes")
    if r["domain"] == "hard":
        return np.ones((h, w), np.float32)
    if cl and cl[0] == "cov_rule" and mp is not None:
        return FU.smoothstep(0.35, 0.65, mp[..., 2]).astype(np.float32)
    if cl and cl[0] == "mask":
        _, fn, slots, rule = cl
        m = np.asarray(Image.open(os.path.join(R.MASKS, fn)).convert("RGBA")).astype(np.float32) / 255
        if m.shape[:2] != (h, w):
            m = cv2.resize(m, (w, h), interpolation=cv2.INTER_LINEAR)
        a = m[..., 0]
        if rule is not None and mp is not None:
            ch, lo, hi = rule
            a = FU.smoothstep(lo, hi, mp[..., ch])
        return a.astype(np.float32)
    return (P[..., CI["smooth_paint"]] + P[..., CI["mottled_paint"]]).astype(np.float32)


def ramp_mask(ce, keep):
    """CE's smooth ramps, as invented_check.check defines them (where sharpening terraces), dilated 2 texels"""
    emid, grad, _ = IC.regions(ce, False)
    Lc = T.lum(ce)
    k3, k7 = np.ones((3, 3), np.uint8), np.ones((7, 7), np.uint8)
    con = (cv2.dilate(Lc, k3) - cv2.erode(Lc, k3)) * 255
    con7 = (cv2.dilate(Lc, k7) - cv2.erode(Lc, k7)) * 255
    ramp = (grad >= 1.6) & (con7 >= 2.0 * con) & (emid < np.maximum(IC.E_LOW * 1.25, 0.6 * grad))
    ramp = cv2.dilate(ramp.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))).astype(np.float32)
    return np.clip(g(ramp, 0.8) * 1.3, 0, 1), emid


def prep(k):
    key = KEYS[k]
    tag = TB.TAG[key]
    ce, _ = T.load(TB.ce_path(key))
    r = R.BASES[tag]
    mp_png = dict(TC.images(r["multi"]))[R.safe(r["multi"])] if r.get("multi") else None
    VB.OUT = os.path.join(OUT, "_mp")
    os.makedirs(os.path.join(VB.OUT, k), exist_ok=True)
    P, keep, mp, how = VB.class_P(k, tag, ce, mp_png)
    enh_png = os.path.join(TI.OUT, key, "enh.png")
    enh = np.asarray(Image.open(enh_png).convert("RGBA")).astype(np.float32) / 255
    alpha = enh[..., 3]
    enh = enh[..., :3]
    H, W = enh.shape[:2]
    F = H // ce.shape[0]
    ramp, emid = ramp_mask(ce, keep)
    d = dict(k=k, key=key, tag=tag, ce=ce, P=P, keep=keep, mp=mp, how=how, enh=enh, alpha=alpha, F=F, H=H, W=W,
             P4=up(P, W, H), keep4=np.clip(up(keep, W, H), 0, 1), ramp4=up(ramp, W, H),
             tex4=up(FU.smoothstep(1.2, 3.5, emid), W, H), plate=plate_mask(tag, ce, mp, P))
    d["plate4"] = up(d["plate"], W, H)
    d["mp4"] = up(mp, W, H) if mp is not None else None
    return d


def unblock(a, F):
    """a field blurred across CE's DXT block boundaries (every 4 CE texels), unchanged elsewhere"""
    B = 4 * F
    yy, xx = np.indices(a.shape[:2])
    dist = np.minimum(np.minimum(xx % B, B - xx % B), np.minimum(yy % B, B - yy % B)).astype(np.float32)
    m = np.clip(1 - dist / (0.75 * F), 0, 1)
    return a * (1 - m) + g(a.astype(np.float32), 0.6 * F) * m


def sl(L):
    return 1 + 0.015 * (L - 50) ** 2 / np.sqrt(20 + (L - 50) ** 2)


def albedo(d, s):
    """Bold albedo at strength s (L* only)"""
    enh, F, P4 = d["enh"], d["F"], d["P4"]
    free = 1 - d["keep4"]
    # the boost stays a texel clear of the lights and glyphs (its blur reached into them: the decal check)
    kd = cv2.dilate(d["keep"].astype(np.float32), np.ones((3, 3), np.uint8))
    clear = 1 - np.clip(up(g(kd, 0.5), d["W"], d["H"]), 0, 1)
    L = TB.Lstar(enh).astype(np.float32)
    d0 = L - g(L, 0.5 * F)
    d1 = g(L, 0.5 * F) - g(L, 2 * F)
    # the material band edge-aware (guided by L* itself): CE's painted form shading and soft value steps are
    # lifted, the hard steps between plate and suit are not, so no dark or bright ring forms around a plate
    Ln = L / 100.0
    d2 = 100.0 * (TI.guided(Ln, Ln, int(1.5 * F), EPS2) - TI.guided(Ln, Ln, int(6 * F), EPS2))
    noramp = 1 - d["ramp4"]
    fine = clear * noramp * np.maximum(d["tex4"], 0.15)
    coarse = clear * (1 - 0.6 * d["ramp4"])
    # the fine bands (edge definition) limited to CE's local range (+-1 texel of the smoothed Enhanced map)
    # plus a margin: an edge gets steeper, never a halo or a second contour
    L2 = L + s * (per_class(P4, 0) * d0 + per_class(P4, 1) * d1) * fine
    Ls = g(L, 0.35 * F)
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * F + 1, 2 * F + 1))
    lo, hi = cv2.erode(Ls, ker) - LIMIT_MARGIN, cv2.dilate(Ls, ker) + LIMIT_MARGIN
    L2 = np.clip(L2, lo, hi)
    # the material band (4-16 texels: plate against suit, CE's painted form shading), broad, not limited
    L2 = L2 + s * per_class(P4, 2) * d2 * coarse
    # CE's DXT block grid (every 4 texels): the boost is blurred across the block boundaries, so the
    # residual block steps Enhanced removed are not sharpened back (the grid check)
    L2 = L + unblock(L2 - L, F)
    # class palette: the excess of each class's mean L* shift (over PAL_DE in dE00) is taken back
    for _ in range(2):
        dl = L2 - L
        corr = np.zeros_like(L)
        for i, c in enumerate(FU.CLASSES):
            w = P4[..., i]
            if w.mean() < 0.01:
                continue
            mu = float((dl * w).sum() / w.sum())
            mL = float((L * w).sum() / w.sum())
            allow = PAL_DE * float(sl(mL))
            if abs(mu) > allow:
                corr += w * (np.sign(mu) * (abs(mu) - allow))
        L2 = L2 - corr
    out = VB.set_L(enh, L2)
    out = out * free[..., None] + enh * (1 - free[..., None])
    return np.clip(out, 0, 1).astype(np.float32)


def measure(d, img):
    m = VB.measure(img, d["ce"], d["P"], d["mp"], d["keep"])
    return m


def bold_fails(m, m0):
    st = m["style"]
    f = []
    if st["hue_deg"] > GATE["hue_deg"]:
        f.append("hue")
    if m["palette_dE"] and max(m["palette_dE"].values()) > GATE["palette"]:
        f.append("palette")
    if st["grid"] > GATE["grid"]:
        f.append("grid")
    if st["invented_edges"] > max(GATE["invented_edges"], m0["style"]["invented_edges"]):
        f.append("invented_edges")
    new = [x for x in m["invented_fails"] if x not in m0["invented_fails"]]
    if new:
        f.append("invented:" + ",".join(new))
    if m["invented"]["banding"] > m0["invented"]["banding"]:
        f.append("banding")
    if m["screen"]["m1.45"]["dE00_mean"] > GATE["screen145"]:
        f.append("screen145")
    return f


# ---------------------------------------------------------------- surface maps
def load_bake(k, W, H):
    p = BAKE.get(k)
    if not p or not os.path.exists(p):
        return None
    a = np.asarray(Image.open(p).convert("RGBA")).astype(np.float32) / 255
    return a


def structural_height(d):
    """height in CE texels at the base map's size (4x): the plate's rim and CE's painted grooves, nothing from
    the paint inside a material"""
    F, H, W = d["F"], d["H"], d["W"]
    ce = d["ce"]
    plate = d["plate"]
    # rim: plate raised 0.4 texel over the rest, the step 1 texel wide
    h = up(g(plate, 0.5), W, H) * 0.4
    # grooves: dark lines (black top-hat, 1.5 texels) on CE's strong edges
    L = TB.Lstar(ce).astype(np.float32)
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    bh = cv2.morphologyEx(g(L, 0.4), cv2.MORPH_BLACKHAT, k)
    Lc = T.lum(ce)
    k3 = np.ones((3, 3), np.uint8)
    con = (cv2.dilate(Lc, k3) - cv2.erode(Lc, k3)) * 255
    groove = FU.smoothstep(4.0, 12.0, bh) * FU.smoothstep(IC.CONTRAST_MIN, 2 * IC.CONTRAST_MIN, con)
    groove = groove * (1 - d["keep"])
    h = h - 0.3 * up(groove, W, H)
    return g(h.astype(np.float32), 0.35 * F), groove


def normal_from_height(h, F):
    """h in CE texels at F texels per CE texel; normal RG (y up the picture)"""
    gx = cv2.Sobel(h, cv2.CV_32F, 1, 0, ksize=3) / 8 * F
    gy = cv2.Sobel(h, cv2.CV_32F, 0, 1, ksize=3) / 8 * F
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def divergence(nx, ny):
    """convexity of a tangent normal map (y up the picture): + convex"""
    return cv2.Sobel(nx, cv2.CV_32F, 1, 0, ksize=3) / 8 - cv2.Sobel(ny, cv2.CV_32F, 0, 1, ksize=3) / 8


def relief_from_paint(nx, ny, d, at):
    """correlation of the normal's curvature with CE's mid-band L* (1-3 texels) away from plate rims and CE's
    strong edges, at the base map's resolution `at` (H, W): near 0 means no painted pattern became relief"""
    H, W = at
    ce = d["ce"]
    L = TB.Lstar(ce).astype(np.float32)
    band = up(g(L, 1.0) - g(L, 3.0), W, H)
    nx, ny = up(nx, W, H), up(ny, W, H)
    dv = divergence(g(nx, 1.0), g(ny, 1.0))
    rim = np.hypot(cv2.Sobel(g(d["plate"], 0.7), cv2.CV_32F, 1, 0), cv2.Sobel(g(d["plate"], 0.7), cv2.CV_32F, 0, 1))
    Lc = T.lum(ce)
    k3 = np.ones((3, 3), np.uint8)
    con = (cv2.dilate(Lc, k3) - cv2.erode(Lc, k3)) * 255
    edge = cv2.dilate(((rim > 0.05) | (con > IC.CONTRAST_MIN)).astype(np.uint8), np.ones((5, 5), np.uint8))
    away = (up(edge, W, H, cv2.INTER_NEAREST) < 0.5) & (up(d["keep"], W, H) < 0.5)
    if away.sum() < 100:
        return 0.0
    a, b = dv[away], band[away]
    a, b = a - a.mean(), b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9))


def surface(d, img, wear=None, grime=None):
    """RGBA uint8 surface map and its stats"""
    F, H, W = d["F"], d["H"], d["W"]
    bake = load_bake(d["k"], W, H)
    if bake is not None:
        SH, SW = bake.shape[:2]
        nx, ny = bake[..., 0] * 2 - 1, bake[..., 1] * 2 - 1
        ao = bake[..., 2]
        src = "geometry bake " + os.path.relpath(BAKE[d["k"]], E.OUT)
        cov = (np.abs(nx) + np.abs(ny) > 0.004).astype(np.float32)
    else:
        SH, SW = H, W
        h, _ = structural_height(d)
        n = normal_from_height(h, F)
        nx, ny = n[..., 0], n[..., 1]
        cav = np.clip((g(h, 1.5 * F) - h) / 0.25, 0, 1)
        ao = np.clip(1 - 0.45 * cav, 0.4, 1)
        src = "structural (plate rim and CE's painted grooves only)"
        cov = None
    P4 = up(d["P"], SW, SH)
    rough = per_class(P4, 3)
    if d["mp"] is not None:
        rough = rough - 0.25 * up(d["mp"], SW, SH)[..., 0]  # CE's specular mask (Xbox R): polish where CE shines
    if wear is not None:
        rough = rough - 0.2 * up(wear, SW, SH) + 0.15 * up(grime, SW, SH)
    rough = rough + 0.08 * np.clip(1 - ao, 0, 1)
    # specular anti-aliasing (Toksvig): where the normal turns within a texel or two (a bevel, a rib) the
    # highlight is widened, so a crisp edge does not sparkle into white specks as it minifies
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    r = max(1.0, SW / float(W))
    sa, ga, ca = SPEC_AA.get(d["k"], SPEC_AA_DEFAULT)
    mx, my, mz = g(nx.astype(np.float32), sa * r), g(ny.astype(np.float32), sa * r), g(nz.astype(np.float32), sa * r)
    var = np.clip(1 - np.sqrt(mx * mx + my * my + mz * mz), 0, 1)
    rough = rough + np.clip(ga * np.sqrt(var), 0, ca)
    if d["k"] in NORMAL_CALM:
        # and the normal itself calmed where it turns fast (the rail's rib crossings lit up white): a crisp
        # rib keeps its edge in the albedo and the occlusion, its bevel tilts less
        nk = np.clip(1 - NORMAL_CALM[d["k"]] * np.sqrt(var), 0.35, 1)
        nx, ny = nx * nk, ny * nk
    keep = np.clip(up(d["keep"], SW, SH), 0, 1)
    rough = np.clip(rough, 0.08, 1) * (1 - keep) + keep
    nx, ny = nx * (1 - keep), ny * (1 - keep)
    rgba = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, ao, rough], -1)
    tilt = np.degrees(np.arccos(np.clip(np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1)), 0, 1)))
    st = {"source": src, "size": [SW, SH], "tilt_mean_deg": float(tilt.mean()),
          "tilt_p95_deg": float(np.percentile(tilt, 95)), "ao_mean": float(ao.mean()), "rough_mean": float(rough.mean()),
          "relief_from_paint_corr": relief_from_paint(nx, ny, d, (H, W))}
    if cov is not None:
        st["bake_cover"] = float(cov.mean())
    return (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8), st


# ---------------------------------------------------------------- weathering (its own switch)
def fbm(H, W, lmin, lmax, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros((H, W), np.float32)
    lam, amp = lmax, 1.0
    while lam >= lmin:
        n = rng.standard_normal((H, W)).astype(np.float32)
        b = g(n, lam / 6) - g(n, lam / 3)
        out += amp * b / (b.std() + 1e-6)
        lam /= 2
        amp *= 0.7
    return out / (out.std() + 1e-6)


def weathering(d, img):
    F, H, W, P4 = d["F"], d["H"], d["W"], d["P4"]
    free = 1 - d["keep4"]
    # the plate's contour smoothed (CE's multipurpose map is DXT: its plate channel steps along the blocks)
    plate4 = up(FU.smoothstep(0.3, 0.7, g(d["plate"], 1.0)), W, H)
    # rims: where the plate share steps (class border), on the plate's side, ~1-1.5 texels wide
    pb = g(plate4, 0.6 * F)
    gm = np.hypot(cv2.Sobel(pb, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(pb, cv2.CV_32F, 0, 1, ksize=3)) / 8 * F
    rim = np.clip(gm / 0.35, 0, 1) * FU.smoothstep(0.45, 0.8, plate4)
    crease = np.clip(gm / 0.35, 0, 1) * (1 - FU.smoothstep(0.2, 0.55, plate4))
    bake = load_bake(d["k"], W, H)
    if bake is not None:  # the remodel's convex rims and cavities (real form), on the plate
        nx, ny = up(bake[..., 0] * 2 - 1, W, H), up(bake[..., 1] * 2 - 1, W, H)
        dv = divergence(g(nx, 0.5 * F), g(ny, 0.5 * F)) * F
        sd = np.percentile(np.abs(dv), 99) + 1e-6
        rim = np.maximum(rim, np.clip(dv / sd * 1.6 - 0.4, 0, 1) * FU.smoothstep(0.45, 0.8, plate4))
        ao = up(bake[..., 2], W, H)
        crease = np.maximum(crease, np.clip((0.75 - ao) / 0.3, 0, 1))
    n1 = fbm(H, W, 2 * F, 10 * F, 7)
    n2 = fbm(H, W, 2 * F, 12 * F, 9)
    wear = FU.smoothstep(0.35, 0.75, rim * (0.8 + 0.45 * n1))
    grime = FU.smoothstep(0.3, 0.8, crease * (0.75 + 0.4 * n2))
    # off CE's smooth ramps (no terraces) and well clear of the lights and glyphs (the decal check)
    kd = cv2.dilate(d["keep"].astype(np.float32), np.ones((5, 5), np.uint8))
    clear = (1 - np.clip(up(kd, W, H), 0, 1)) * (1 - d["ramp4"])
    wear = unblock(g(wear.astype(np.float32), 0.25 * F), F) * per_class(P4, None, WEAR_AMOUNT) * clear
    grime = unblock(g(grime.astype(np.float32), 0.4 * F), F) * per_class(P4, None, GRIME_AMOUNT) * clear
    # target colours: each class's own CE bright (90th percentile of L*) and dark (5th), hue of CE
    ce = d["ce"]
    lab_ce = T.srgb_to_lab(ce)
    cls = d["P"].argmax(-1)
    cls4 = up(cls.astype(np.uint8), W, H, cv2.INTER_NEAREST).astype(np.int32)
    hi = np.zeros((H, W, 3), np.float32)
    lo = np.zeros((H, W, 3), np.float32)
    for i, c in enumerate(FU.CLASSES):
        sel = cls == i
        if sel.sum() < 16:
            continue
        Lc = lab_ce[..., 0][sel]
        rgb = ce[sel]
        hi[cls4 == i] = rgb[Lc >= np.percentile(Lc, 90)].mean(0)
        lo[cls4 == i] = rgb[Lc <= np.percentile(Lc, 5)].mean(0)
    out = img + wear[..., None] * (hi - img)
    out = out + grime[..., None] * (lo - out)
    # never past +WEAR_MAX_L L* over the bold map (no white flecks on dark metal)
    L0, L1 = TB.Lstar(img), TB.Lstar(np.clip(out, 0, 1))
    over = L1 - L0 - WEAR_MAX_L
    if (over > 0).any():
        out = VB.set_L(np.clip(out, 0, 1), np.where(over > 0, L0 + WEAR_MAX_L, L1))
    return np.clip(out, 0, 1).astype(np.float32), wear.astype(np.float32), grime.astype(np.float32)


def weather_fails(m, m0):
    """the Weathering switch adds what CE never had (invented edges are its point): colour, grid and the
    speckle, decal, contour and banding checks still hold"""
    st = m["style"]
    f = []
    if st["hue_deg"] > 2.5:
        f.append("hue")
    if m["palette_dE"] and max(m["palette_dE"].values()) > 3.0:
        f.append("palette")
    if st["grid"] > GATE["grid"]:
        f.append("grid")
    new = [x for x in m["invented_fails"] if x not in m0["invented_fails"]]
    if new:
        f.append("invented:" + ",".join(new))
    if m["invented"]["banding"] > m0["invented"]["banding"]:
        f.append("banding")
    return f


def short(m):
    st = m["style"]
    return {"de": round(st["de_mean"], 3), "deL": round(st["deL_mean"], 3), "hue": round(st["hue_deg"], 2),
            "ssim": round(st["ssim_L"], 4), "grid": round(st["grid"], 3), "inv_edges": round(st["invented_edges"], 4),
            "inv_fails": m["invented_fails"], "banding": m["invented"]["banding"], "speckle": round(m["invented"]["speckle"], 1),
            "palette": {a: round(b, 2) for a, b in m["palette_dE"].items()},
            "screen": {a: round(b["dE00_mean"], 2) for a, b in m["screen"].items()},
            "band_rms_1.45": round(m["definition"]["m1.45"]["band_rms"], 2),
            "band_rms_0.54": round(m["definition"]["m0.54"]["band_rms"], 2),
            "class_step_1.45": round(m["definition"]["m1.45"]["class_step"], 2)}


def rerun_weathering(k):
    """the weathering stage again on the saved Bold map (metrics.json updated)"""
    od = os.path.join(OUT, k)
    d = prep(k)
    res = json.load(open(os.path.join(od, "metrics.json")))
    best = T.load(os.path.join(od, "bold.png"))[0]
    m0 = measure(d, d["enh"])
    wimg, wear, grime = weathering(d, best)
    mw = measure(d, wimg)
    res["weathered"] = short(mw)
    res["weathered_fails"] = weather_fails(mw, m0)
    res["wear_share"] = float((wear > 0.1).mean())
    res["grime_share"] = float((grime > 0.1).mean())
    T.save(os.path.join(od, "weathered.png"), wimg)
    T.save(os.path.join(od, "wear.png"), np.clip(wear, 0, 1))
    T.save(os.path.join(od, "grime.png"), np.clip(grime, 0, 1))
    srfw, sstw = surface(d, wimg, wear, grime)
    Image.fromarray(srfw, "RGBA").save(os.path.join(od, "surface_weathered.png"))
    res["surface_weathered"] = sstw
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(k, "WEATHER", res["weathered_fails"], res["weathered"]["speckle"], res["wear_share"], flush=True)
    return k


def run(k):
    t0 = time.time()
    od = os.path.join(OUT, k)
    os.makedirs(od, exist_ok=True)
    d = prep(k)
    res = {"key": d["key"], "classes": d["how"], "F": d["F"], "size": [d["W"], d["H"]],
           "class_share": {c: round(float(d["P"][..., i].mean()), 3) for i, c in enumerate(FU.CLASSES)},
           "ramp_share": float(d["ramp4"].mean()), "plate_share": float(d["plate"].mean())}
    m0 = measure(d, d["enh"])
    res["enhanced"] = short(m0)
    res["enhanced_fails"] = m0["invented_fails"]
    best, bests, tried = None, None, []
    for s in STRENGTHS:
        img = albedo(d, s)
        m = measure(d, img)
        f = bold_fails(m, m0)
        tried.append({"strength": s, "fails": f, **short(m)})
        print(k, "s=%.1f" % s, f, short(m), flush=True)
        if f:
            break
        best, bests, bm = img, s, m
    if best is None:  # even 1.0 fails: back off
        for s in (0.75, 0.5):
            img = albedo(d, s)
            m = measure(d, img)
            f = bold_fails(m, m0)
            tried.append({"strength": s, "fails": f, **short(m)})
            print(k, "s=%.2f" % s, f, flush=True)
            if not f:
                best, bests, bm = img, s, m
                break
    if best is None:
        best, bests, bm = img, s, m
        res["warning"] = "no strength passed the Bold gates"
    res["tried"] = tried
    res["strength"] = bests
    res["bold"] = short(bm)
    T.save(os.path.join(od, "bold.png"), best)
    srf, sst = surface(d, best)
    Image.fromarray(srf, "RGBA").save(os.path.join(od, "surface.png"))
    res["surface"] = sst
    wimg, wear, grime = weathering(d, best)
    mw = measure(d, wimg)
    res["weathered"] = short(mw)
    res["weathered_fails"] = weather_fails(mw, m0)
    res["wear_share"] = float((wear > 0.1).mean())
    res["grime_share"] = float((grime > 0.1).mean())
    T.save(os.path.join(od, "weathered.png"), wimg)
    T.save(os.path.join(od, "wear.png"), np.clip(wear, 0, 1))
    T.save(os.path.join(od, "grime.png"), np.clip(grime, 0, 1))
    srfw, sstw = surface(d, wimg, wear, grime)
    Image.fromarray(srfw, "RGBA").save(os.path.join(od, "surface_weathered.png"))
    res["surface_weathered"] = sstw
    np.save(os.path.join(od, "alpha.npy"), d["alpha"])
    res["seconds"] = round(time.time() - t0)
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(k, "DONE strength", bests, "weathered fails", res["weathered_fails"], "surface", sst, res["seconds"], "s", flush=True)
    return k


def rerun_surface(k):
    """only the surface maps again (bold.png and weathered.png as they are), for a SPEC_AA change"""
    od = os.path.join(OUT, k)
    d = prep(k)
    res = json.load(open(os.path.join(od, "metrics.json")))
    img = np.asarray(Image.open(os.path.join(od, "bold.png")).convert("RGB")).astype(np.float32) / 255
    srf, sst = surface(d, img)
    Image.fromarray(srf, "RGBA").save(os.path.join(od, "surface.png"))
    res["surface"] = sst
    wimg = np.asarray(Image.open(os.path.join(od, "weathered.png")).convert("RGB")).astype(np.float32) / 255
    wear = np.asarray(Image.open(os.path.join(od, "wear.png")).convert("L")).astype(np.float32) / 255
    grime = np.asarray(Image.open(os.path.join(od, "grime.png")).convert("L")).astype(np.float32) / 255
    srfw, sstw = surface(d, wimg, wear, grime)
    Image.fromarray(srfw, "RGBA").save(os.path.join(od, "surface_weathered.png"))
    res["surface_weathered"] = sstw
    json.dump(res, open(os.path.join(od, "metrics.json"), "w"), indent=1, default=float)
    print(k, "SURFACE", sst, flush=True)
    return k


if __name__ == "__main__":
    a = sys.argv[1:]
    jobs = 1
    if a and a[0] == "--jobs":
        jobs, a = int(a[1]), a[2:]
    fn = run
    if a and a[0] == "--weather-only":
        fn, a = rerun_weathering, a[1:]
    elif a and a[0] == "--surface-only":
        fn, a = rerun_surface, a[1:]
    keys = a or list(KEYS)
    if jobs == 1:
        for k in keys:
            fn(k)
    else:
        with ProcessPoolExecutor(jobs) as ex:
            for k in ex.map(fn, keys):
                pass
