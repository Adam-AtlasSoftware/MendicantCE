#!/usr/bin/env python3
"""PRISM v2 r2: pack prism_bake_ce.py's raw bakes (CE's own UVs) into the engine's surface map, plus a
curvature map and a preview sheet.

  python -I art/prism_surface_pack.py <raw dir> <out dir> --name <stem>
      [--multipurpose <CE multipurpose PNG, the Xbox's channel order>] [--recipe <json>]

The surface map (port/linux/src/xgpu.h "The surface map"; the engine finds it next to the base map it
lines up with: tags\\<bitmap path>.surface.png): RGBA at the base map's coordinates, RG the tangent-space
normal's x and y (y up the picture, 0.5 flat), B the ambient occlusion (1 open), A the roughness (0
polished, 1 matte; 1 also means no highlight at all).

- normal: the high-poly over the game mesh, where a ray hit the high-poly's front; a texel the ray
  missed, or whose hit faces away (z < cos max_tilt), is filled from its island's good neighbours (a
  push-pull fill), never left black or flat in a hole;
- occlusion: the high-poly's (the game mesh hidden from its rays), else the game mesh's own; then
  floor + (1 - floor) * ao^gamma, so the in-game light is never put out;
- curvature: real units: the game mesh's own mean curvature (1/mm, from its vertex normals and edges)
  plus half the divergence of the baked normal in texture space through each face's mm per texel (the
  high-poly's detail), differences taken only between texels of one UV island; stored as
  0.5 + 0.5 tanh(H * radius) (radius in mm: recipe "curvature_radius_mm", default 2), convex bright;
- roughness: the shader's base (recipe "rough": {shader: value}), less the multipurpose map's specular
  mask (CE's own polish), optionally split by a multipurpose class channel ("mp_class": plates against
  bodysuit) and kept off the self-illuminated texels ("mp_glow"), less convex edges (worn bright), plus cavities and occluded texels (dirt);
  outside the baked islands 1 (no highlight, normal flat, occlusion 1: nothing changes there).
Every map is padded outward from its islands (push-pull), so the mips and the bilinear taps at CE's
seams read the islands' own values, not flat ones.
Local only: derived from the player's CE vertices and pixels.
"""
import argparse
import json
import os

import cv2
import numpy as np
from PIL import Image

DEFAULT = {"normal_gain": 1.0, "max_tilt_deg": 72, "ao_floor": 0.25, "ao_gamma": 0.8,
           "curvature_radius_mm": 2.0, "rough": {}, "rough_default": 0.6, "spec_polish": 0.25,
           "edge_polish": 0.18, "cavity_rough": 0.15, "ao_rough": 0.12, "outside_rough": 1.0}


def push_pull(values, valid):
    """values filled where not valid from the valid ones nearby (a pyramid of masked averages)"""
    v = values.astype(np.float32)
    if v.ndim == 2:
        v = v[..., None]
    w = valid.astype(np.float32)[..., None]
    levels = []
    cv_, cw = v * w, w
    while min(cw.shape[:2]) > 1:
        levels.append((cv_, cw))
        h, wd = cw.shape[:2]
        cv_ = cv_[: h // 2 * 2, : wd // 2 * 2].reshape(h // 2, 2, wd // 2, 2, -1).sum((1, 3))
        cw = cw[: h // 2 * 2, : wd // 2 * 2].reshape(h // 2, 2, wd // 2, 2, -1).sum((1, 3))
    est = cv_ / np.maximum(cw, 1e-6)
    for lv, lw in reversed(levels):
        h, wd = lw.shape[:2]
        up = cv2.resize(est, (wd, h), interpolation=cv2.INTER_LINEAR)
        if up.ndim == 2:
            up = up[..., None]
        here = lv / np.maximum(lw, 1e-6)
        a = np.clip(lw, 0, 1)
        est = here * a + up * (1 - a)
    out = np.where(valid[..., None], v, est)
    return out[..., 0] if values.ndim == 2 else out


def island_diff(field, island, axis):
    """central difference of field along axis (1: right, 0: down), 0 across islands or at their edge"""
    f = np.pad(field, 1, mode="edge")
    i = np.pad(island, 1, mode="constant")
    if axis == 1:
        fp, fm, ip, im = f[1:-1, 2:], f[1:-1, :-2], i[1:-1, 2:], i[1:-1, :-2]
    else:
        fp, fm, ip, im = f[2:, 1:-1], f[:-2, 1:-1], i[2:, 1:-1], i[:-2, 1:-1]
    c = island
    both = (ip == c) & (im == c)
    one_p = (ip == c) & ~both
    one_m = (im == c) & ~both
    d = np.where(both, (fp - fm) / 2, np.where(one_p, fp - field, np.where(one_m, field - fm, 0)))
    return np.where(c > 0, d, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out")
    ap.add_argument("--name", required=True)
    ap.add_argument("--multipurpose")
    ap.add_argument("--recipe")
    ap.add_argument("--size", type=int, default=0, help="output size (default: the bake's)")
    a = ap.parse_args()
    rc = dict(DEFAULT)
    if a.recipe:
        rc.update(json.load(open(a.recipe)))
    bake = json.load(open(os.path.join(a.raw, "bake.json")))
    L = lambda n: np.load(os.path.join(a.raw, "raw_%s.npy" % n)).astype(np.float32)  # noqa: E731
    cover = L("cover") > 0.5
    island = np.load(os.path.join(a.raw, "raw_island32.npy"))
    shader = np.round(L("shader")).astype(np.int32)
    dens = L("density")
    nraw = L("normal")
    ao_hi, ao_game = L("ao"), L("ao_game")
    kmesh_enc = L("curv_mesh")
    S = cover.shape[0]
    rep = {"size": S, "name": a.name, "recipe": rc, "bake": {k: bake.get(k) for k in (
        "coverage_share", "normal_missed_share_of_cover", "normal_backfacing_share_of_cover",
        "ao_missed_share_of_cover", "islands", "density_mm_per_texel_median")}}

    # normal: good hits only, holes filled from the island's good texels
    n = nraw * 2 - 1
    hit = nraw.sum(-1) > 0.01
    good = cover & hit & (n[..., 2] > np.cos(np.radians(rc["max_tilt_deg"])))
    n = n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    xy = np.where(good[..., None], n[..., :2], 0)
    xy = push_pull(xy, good)
    xy = np.where(cover[..., None], xy, push_pull(xy, cover))
    rep["normal_filled_share_of_cover"] = float((cover & ~good).sum() / max(cover.sum(), 1))
    xy = np.clip(xy * rc["normal_gain"], -0.95, 0.95)
    # (optional, recipe edge_band_px) the texels within a few of an island's edge (a UV seam or the cut
    # between two islands) take the island's values from farther inside: there the bake's rays graze the
    # neighbouring face and leave steep normals and dark occlusion, which the game draws as ragged dark
    # cracks along the seam (the Chief's tread)
    eb = int(rc.get("edge_band_px", 0))
    if eb > 0:
        isl = np.where(cover, island, -1).astype(np.float32)
        k_ = np.ones((2 * eb + 1, 2 * eb + 1), np.uint8)
        band = cover & ((cv2.dilate(isl, k_) != isl) | (cv2.erode(isl, k_) != isl))
        inner = cover & ~band
        # only where the edge texel is an outlier: tilted past edge_tilt_deg and further than that from
        # the island's inner normal (a real bevel that carries on inside the island is kept)
        xin = push_pull(np.where(inner[..., None], xy, 0), inner)
        st = np.sin(np.radians(rc.get("edge_tilt_deg", 30)))
        odd = band & (np.hypot(xy[..., 0], xy[..., 1]) > st) & (np.hypot(*(xy - xin).transpose(2, 0, 1)) > st)
        xy = np.where(odd[..., None], xin, xy)
        rep["edge_band_share_of_cover"] = float(band.sum() / max(cover.sum(), 1))
        rep["edge_normal_replaced_share_of_cover"] = float(odd.sum() / max(cover.sum(), 1))

    # occlusion
    ao_ok = cover & (ao_hi >= 0)
    ao = np.where(ao_ok, ao_hi, ao_game)
    ao = np.where(cover, np.clip(ao, 0, 1), 1)
    rep["ao_raw_mean"] = float(ao[cover].mean())
    rep["ao_raw_p1"] = float(np.percentile(ao[cover], 1))
    rep["ao_black_share_of_cover"] = float((ao[cover] < 0.05).mean())
    aoc = rc["ao_floor"] + (1 - rc["ao_floor"]) * ao ** rc["ao_gamma"]
    aoc = np.where(cover, aoc, 1.0)
    if eb > 0:
        # occlusion: an edge texel darker than its island's inside by more than edge_ao_drop is lifted to it
        ain = push_pull(np.where(inner, aoc, 1.0), inner)
        dark = band & (aoc < ain - rc.get("edge_ao_drop", 0.15))
        aoc = np.where(dark, ain, aoc)
        rep["edge_ao_lifted_share_of_cover"] = float(dark.sum() / max(cover.sum(), 1))
    aoc = push_pull(aoc, cover)

    # curvature (1/mm): the mesh's own + half the detail normal's divergence
    kmesh = np.arctanh(np.clip(kmesh_enc * 2 - 1, -0.999, 0.999)) / 5.0
    nx, ny = xy[..., 0] / max(rc["normal_gain"], 1e-6), xy[..., 1] / max(rc["normal_gain"], 1e-6)
    div = island_diff(nx, island, 1) - island_diff(ny, island, 0)
    mmpt = np.where(dens > 0, dens, np.median(dens[dens > 0]))
    H = np.where(cover, kmesh + 0.5 * div / mmpt, 0)
    H = cv2.GaussianBlur(H.astype(np.float32), (0, 0), 0.75)
    R = rc["curvature_radius_mm"]
    curv = np.where(cover, 0.5 + 0.5 * np.tanh(H * R), 0.5)
    curv = push_pull(curv, cover)
    convex = np.clip(np.tanh(H * R), 0, 1)
    concave = np.clip(-np.tanh(H * R), 0, 1)
    rep["curvature_per_mm_p1_p50_p99"] = [float(x) for x in np.percentile(H[cover], [1, 50, 99])]

    # roughness
    order = bake.get("shader_order", [])
    rough = np.full((S, S), rc["rough_default"], np.float32)
    for i, name in enumerate(order):
        if name in rc["rough"]:
            rough[shader == i + 1] = rc["rough"][name]
    if a.multipurpose:
        mp = np.asarray(Image.open(a.multipurpose).convert("RGBA"), np.float32) / 255
        mp = cv2.resize(mp, (S, S), interpolation=cv2.INTER_LINEAR)
        rough -= rc["spec_polish"] * mp[..., 0]
        # (optional, r2 Elite) a class split from a multipurpose channel: plates polished, bodysuit matte
        mc = rc.get("mp_class")
        if mc:
            t = np.clip((mp[..., mc["channel"]] - mc["lo"]) / (mc["hi"] - mc["lo"]), 0, 1)
            t = t * t * (3 - 2 * t)
            rough += (mc["on"] * t + mc["off"] * (1 - t)) - rc["rough_default"]
        mg = rc.get("mp_glow")
        if mg:
            glow_px = mp[..., mg["channel"]] > mg["above"]
    rough = rough - rc["edge_polish"] * convex + rc["cavity_rough"] * concave + rc["ao_rough"] * (1 - ao)
    if a.multipurpose and rc.get("mp_glow"):
        rough = np.where(glow_px, 1.0, rough)   # (self-illuminated texels: no highlight)
    for i, name in enumerate(order):
        if name in rc.get("rough_fixed", {}):
            rough[shader == i + 1] = rc["rough_fixed"][name]
    rough = np.clip(rough, 0.05, 1.0)
    rep["rough_mean"] = float(rough[cover].mean())
    # (outside the islands: a ring of the islands' own values for the bilinear taps and mips, then matte)
    ring = cv2.dilate(cover.astype(np.uint8), np.ones((9, 9), np.uint8)) > 0
    rough = np.where(cover, rough, np.where(ring, push_pull(rough, cover), rc["outside_rough"]))
    for i, name in enumerate(order):
        if name in rc.get("flat", []):
            m = shader == i + 1
            xy[m] = 0
    surf = np.dstack([xy[..., 0] * 0.5 + 0.5, xy[..., 1] * 0.5 + 0.5, aoc, rough])
    if a.size and a.size != S:
        surf = cv2.resize(surf, (a.size, a.size), interpolation=cv2.INTER_AREA)
        curv = cv2.resize(curv, (a.size, a.size), interpolation=cv2.INTER_AREA)
    os.makedirs(a.out, exist_ok=True)
    p = os.path.join(a.out, a.name + ".surface.png")
    Image.fromarray((np.clip(surf, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(p)
    Image.fromarray((np.clip(curv, 0, 1) * 255 + 0.5).astype(np.uint8), "L").save(
        os.path.join(a.out, a.name + ".curvature.png"))
    # preview: normal, AO, roughness, curvature (1024 each)
    pv = 1024
    tiles = [np.dstack([surf[..., 0], surf[..., 1], np.sqrt(np.clip(1 - (surf[..., 0] * 2 - 1) ** 2 -
                                                                         (surf[..., 1] * 2 - 1) ** 2, 0, 1)) * 0.5 + 0.5]),
             np.repeat(surf[..., 2:3], 3, 2), np.repeat(surf[..., 3:4], 3, 2), np.repeat(curv[..., None], 3, 2)]
    tiles = [cv2.resize(t.astype(np.float32), (pv, pv), interpolation=cv2.INTER_AREA) for t in tiles]
    sheet = (np.clip(np.hstack(tiles), 0, 1) * 255).astype(np.uint8)
    for i, lab in enumerate(("normal (CE UVs)", "occlusion", "roughness", "curvature")):
        cv2.putText(sheet, lab, (i * pv + 12, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (0, 0, 0), 5)
        cv2.putText(sheet, lab, (i * pv + 12, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (255, 255, 255), 2)
    Image.fromarray(sheet).save(os.path.join(a.out, a.name + ".preview.png"))
    json.dump(rep, open(os.path.join(a.out, a.name + ".pack.json"), "w"), indent=1)
    print("PACK_OK", p, json.dumps({k: rep[k] for k in rep if k not in ("recipe",)}))


if __name__ == "__main__":
    main()
