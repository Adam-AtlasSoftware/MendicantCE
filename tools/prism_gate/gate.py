#!/usr/bin/env python3
"""The quality gate (docs/prism_gates.md) over captures made by capture.py, against thresholds.json's thresholds:

    python3 tools/prism_gate/gate.py measure <captures> [scene ...]   # results.json and a table
    python3 tools/prism_gate/gate.py measure <captures> --state v4pt --off v4off  # M3's Path Traced
    python3 tools/prism_gate/gate.py repro <captures a> <captures b>  # every frame of both byte for byte alike?
    python3 tools/prism_gate/gate.py equivalence <captures> [scene ...]  # M1: "ce_deferred" against "ce"
    python3 tools/prism_gate/gate.py material_source <captures> [scene ...]  # M1: every opaque pixel's material

measure, for each scene, at the three probe poses (the worst of them):
  error    relative MSE of the path traced moving frame at the pose against its reference (R)
  blotch   its 4-24 px band against R
  flip     mean FLIP against R
  and over the move: flicker, path traced over RT off
  where RT off's moving frame at the pose and its still frame there differ, left out of error, blotch and flip;
  a reference whose pose shows mostly the sky (its EXR's Z: no surface at more than `sky_most` of its pixels) fails
  the scene, as a harness fault. A scene passes where each measure is within its threshold.

repro compares two captures' manifest.json (the SHA-256 of each saved frame's pixels) and each EXR's bytes.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prism_gate import capture, exr, flip, metrics  # noqa: E402

HERE = Path(__file__).resolve().parent


def thresholds():
    return json.loads((HERE / "thresholds.json").read_text())["gate"]


def measure_scene(scene, folder, limits, state="pathtraced", off="off"):
    moves, refs = folder / "moves", folder / "refs"
    errors, blotches, flips, skies = [], [], [], []
    for index, step in enumerate(capture.probe_steps(scene)):
        ref = metrics.load(refs / f"p{index}_ref.png")
        moving = metrics.load(moves / f"m_{state}_{step:03d}.png")
        keep = metrics.keep_mask(metrics.load(moves / f"m_{off}_{step:03d}.png"),
                                 metrics.load(refs / f"p{index}_off.png"))
        errors.append(metrics.rel_mse(moving, ref, keep))
        blotches.append(metrics.blotch(moving, ref, keep))
        flip_map = flip.flip_map(ref, moving)
        flips.append(float(flip_map[keep].mean()))
        z = refs / f"p{index}.exr"
        skies.append(metrics.sky_share(exr.read(z)[0]["Z"]) if z.exists() else 1.0)
    total = scene["steps"] * (len(scene["keys"]) - 1)

    def frames(kind):
        return [metrics.load(moves / f"m_{kind}_{s:03d}.png") for s in range(1, total + 1)]

    result = {"error": max(errors), "blotch": max(blotches), "flip": max(flips),
              "flicker": metrics.flicker(frames(state), frames(off)), "errors": errors,
              "blotches": blotches, "flips": flips, "sky": skies}
    result["harness_fault"] = max(skies) > limits["sky_most"]
    result["pass"] = (not result["harness_fault"] and result["error"] <= limits["error"] and
                      result["blotch"] <= limits["blotch"] and result["flicker"] <= limits["flicker"] and
                      result["flip"] <= limits["flip_still"])
    return result


def measure(args):
    root = Path(args.captures)
    scenes = capture.load_scenes()
    limits = thresholds()
    results = {}
    for scene_id in args.scenes or sorted(p.name for p in root.iterdir() if p.name in scenes):
        r = measure_scene(scenes[scene_id], root / scene_id, limits, args.state, args.off)
        results[scene_id] = r
        print("%-4s %-14s error %.4f  blotch %.4f  flip %.3f  flicker %.2f  %s" % (
            scene_id, scenes[scene_id]["name"], r["error"], r["blotch"], r["flip"], r["flicker"],
            "HARNESS FAULT (sky)" if r["harness_fault"] else "PASS" if r["pass"] else "FAIL"), flush=True)
    (root / "results.json").write_text(json.dumps({"thresholds": limits, "results": results}, indent=1))
    return 0 if all(r["pass"] for r in results.values()) else 1


def equivalence(args):
    """M1's equivalence gate (docs/prism_gates.md): at each probe pose, mean FLIP of the deferred diagnostic's frame
    against Classic's ("ce"), the worst pose a scene's; and the share of the opaque pixels its base target covers
    (not white: the objects' opaque parts, lit in one pass, are not in it yet)"""
    root = Path(args.captures)
    scenes = capture.load_scenes()
    limit = thresholds()["equivalence_flip"]
    results = {}
    for scene_id in args.scenes or sorted(p.name for p in root.iterdir() if p.name in scenes):
        folder = root / scene_id / "equivalence"
        flips, over = [], []
        for index in range(len(scenes[scene_id]["probes"])):
            ce = metrics.load(folder / f"p{index}_ce.png")
            deferred = metrics.load(folder / f"p{index}_deferred.png")
            flips.append(float(flip.mean_flip(ce, deferred)))
            over.append(float((np.abs(ce - deferred).max(2) > 8).mean()))
        results[scene_id] = {"flip": max(flips), "flips": flips, "over_8": over, "pass": max(flips) <= limit}
        print("%-4s %-14s flip %.4f (poses %s)  %s" % (scene_id, scenes[scene_id]["name"], max(flips),
              " ".join("%.4f" % f for f in flips), "PASS" if results[scene_id]["pass"] else "FAIL"), flush=True)
    (root / "equivalence.json").write_text(json.dumps({"threshold": limit, "results": results}, indent=1))
    return 0 if all(r["pass"] for r in results.values()) else 1


# debug.gbuffer_view material_source's colours (xgpu_stream.h's XGPU_GBUFFER_TIER_*, the resolve's view 8): each
# opaque pixel by its material's source tier, a pixel the window drew that the G-buffer missed, one the G-buffer has
# that the window did not draw (extra), and nothing drawn
SOURCE_COLOURS = {"auto": (0, 255, 0), "pack": (0, 0, 255), "mod": (0, 255, 255), "class": (255, 255, 0),
                  "none": (255, 128, 0), "missed": (255, 0, 255), "extra": (255, 0, 0), "empty": (0, 0, 0)}
SOURCE_GOOD = ("auto", "pack", "mod")


def source_counts(image):
    """a material-source frame's pixels counted by their nearest of SOURCE_COLOURS"""
    pixels = np.asarray(image, dtype=np.int32)[..., :3].reshape(-1, 3)
    names = list(SOURCE_COLOURS)
    palette = np.array([SOURCE_COLOURS[n] for n in names], dtype=np.int32)
    nearest = np.argmin(((pixels[:, None, :] - palette[None, :, :]) ** 2).sum(2), axis=1)
    counts = np.bincount(nearest, minlength=len(names))
    return {name: int(counts[i]) for i, name in enumerate(names)}


def material_source(args):
    """M1's material-source gate (docs/prism_gates.md): at each probe pose of the equivalence capture, the share of
    the opaque pixels (every pixel the window drew opaque: not empty) whose material is at the automatic tier or
    better, the worst pose a scene's, against material_source_coverage (1.0: every one); and (M4) the pack tier's and
    the mod tier's shares of them, reported"""
    from PIL import Image

    root = Path(args.captures)
    scenes = capture.load_scenes()
    limit = thresholds()["material_source_coverage"]
    results = {}
    folders = sorted(p for p in root.iterdir() if p.is_dir() and (p / "equivalence").is_dir())
    for folder in folders:
        if args.scenes and folder.name not in args.scenes:
            continue
        poses = sorted((folder / "equivalence").glob("p*_source.png"))
        if not poses:
            continue
        coverage, totals, pack, mod = [], {name: 0 for name in SOURCE_COLOURS}, [], []
        for path in poses:
            counts = source_counts(Image.open(path).convert("RGB"))
            opaque = sum(counts[n] for n in SOURCE_COLOURS if n != "empty")
            good = sum(counts[n] for n in SOURCE_GOOD)
            coverage.append(good / opaque if opaque else 1.0)
            pack.append(counts["pack"] / opaque if opaque else 0.0)
            mod.append(counts["mod"] / opaque if opaque else 0.0)
            for name in counts:
                totals[name] += counts[name]
        name = scenes[folder.name]["name"] if folder.name in scenes else folder.name
        results[folder.name] = {"coverage": min(coverage), "coverages": coverage, "pixels": totals,
                                "pack_shares": pack, "mod_shares": mod, "pass": min(coverage) >= limit}
        print("%-4s %-14s coverage %.6f (poses %s; missed %d, extra %d, none %d; pack %s, mod %s)  %s" % (folder.name,
              name, min(coverage), " ".join("%.6f" % c for c in coverage), totals["missed"], totals["extra"],
              totals["none"], " ".join("%.3f" % c for c in pack), " ".join("%.3f" % c for c in mod),
              "PASS" if results[folder.name]["pass"] else "FAIL"), flush=True)
    (root / "material_source.json").write_text(json.dumps({"threshold": limit, "results": results}, indent=1))
    return 0 if results and all(r["pass"] for r in results.values()) else 1


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def repro(args):
    a, b = Path(args.a), Path(args.b)
    same = differ = 0
    for scene in sorted(p.name for p in a.iterdir() if (p / "manifest.json").exists()):
        fa = json.loads((a / scene / "manifest.json").read_text())["frames"]
        mb = b / scene / "manifest.json"
        fb = json.loads(mb.read_text())["frames"] if mb.exists() else {}
        names = sorted(set(fa) | set(fb))
        bad = [n for n in names if fa.get(n) != fb.get(n)]
        exrs = sorted(p.name for p in (a / scene / "refs").glob("*.exr"))
        bad += [n for n in exrs if not (b / scene / "refs" / n).exists() or
                digest(a / scene / "refs" / n) != digest(b / scene / "refs" / n)]
        total = len(names) + len(exrs)
        same += total - len(bad)
        differ += len(bad)
        print(f"{scene}: {total - len(bad)}/{total} alike" + (f"; differ: {', '.join(bad[:6])}" if bad else ""))
    print(f"all: {same} alike, {differ} differ")
    return 0 if differ == 0 and same else 1


def bake_rig(log_text):
    """the rig the bake was made with, from the capture's log (xgpu_vulkan_bake.c's "the rig" line): the sun's
    radiance (its light on a surface facing it squarely), the direction toward it, whether there is one, the gamma of
    CE's encoding"""
    import re
    match = re.search(r"the rig: sun ([-\d.e]+) ([-\d.e]+) ([-\d.e]+) toward ([-\d.e]+) ([-\d.e]+) ([-\d.e]+) "
                      r"\((present|none), disc [-\d.e]+\), sky [-\d.e]+ [-\d.e]+ [-\d.e]+(?: \([^)]*\))?, gamma ([-\d.e]+)",
                      log_text)
    if not match:
        raise SystemExit("no rig in the capture's log")
    values = [float(v) for v in match.groups()[:6]]
    return np.array(values[:3]), np.array(values[3:6]), match.group(7) == "present", float(match.group(8))


def bake_frames(folder, refs, index, sun, toward, present, gamma):
    """the bake's frame and the reference's at a probe, put together alike (as PRISM Raster's resolve does, without
    CE's additions): the base colour by the light in CE's encoding, the light the light less the sun's and the sun's
    light by the pixel's normal and visibility; the level's pixels with a surface (the objects left out: the bake's
    static light against the reference's static frame); and the light alone and the visibility alone"""
    channels, _ = exr.read(refs / f"p{index}.exr")

    def small(a):
        # (the reference's channels at the metrics' size: each 2x2 block's mean, as the frames are brought there)
        a = np.asarray(a, np.float64)
        h, w = metrics.SIZE[1], metrics.SIZE[0]
        if a.shape[:2] == (h, w):
            return a
        return a[:h * (a.shape[0] // h), :w * (a.shape[1] // w)].reshape(h, a.shape[0] // h, w, a.shape[1] // w,
                                                                           *a.shape[2:]).mean((1, 3))

    def load01(name):
        return metrics.load(folder / name).astype(np.float64) / 255.0
    ref_light = small(np.stack([channels["light.R"], channels["light.G"], channels["light.B"]], -1))
    ref_v = small(channels["structure.V"] if "structure.V" in channels else channels["sun.V"])
    z = small(channels["Z"])
    base = load01(f"p{index}_base.png")
    normal = load01(f"p{index}_normal.png") * 2.0 - 1.0
    instance = load01(f"p{index}_instance.png")
    light = np.clip(load01(f"p{index}_light.png"), 0.0, 1.0) ** gamma
    vis = load01(f"p{index}_sun.png")[..., 0]
    level = (np.abs(instance - 0.25).max(2) < 0.02) & (z > 0.0)
    facing = np.clip((normal * toward).sum(2), 0.0, None)[..., None]
    bake_vis = np.where(facing[..., 0] > 0.0, vis, 0.0)

    def encode(x):
        return np.clip(x, 0.0, None) ** (1.0 / gamma) if gamma > 1.001 else np.clip(x, 0.0, None)

    def quantised(x):
        return np.round(np.clip(x, 0.0, 1.0) * 255.0) / 255.0

    sun_b = sun * facing * bake_vis[..., None] if present else 0.0
    sun_r = sun * facing * ref_v[..., None] if present else 0.0
    frame_b = np.clip(base * encode(light + sun_b), 0.0, 1.0)
    frame_r = np.clip(base * quantised(encode(ref_light + sun_r)), 0.0, 1.0)
    frame_b = np.where(level[..., None], frame_b, 0.0)
    frame_r = np.where(level[..., None], frame_r, 0.0)
    light_b = np.where(level[..., None], quantised(encode(light)), 0.0)
    light_r = np.where(level[..., None], quantised(encode(ref_light)), 0.0)
    return frame_b, frame_r, light_b, light_r, np.where(level, bake_vis, 0.0), np.where(level, ref_v, 0.0), level


def bake(args):
    """M2's bake gate (docs/prism_gates.md): at each probe pose of a scene captured by capture.py bake, the bake's
    static light against the reference's (capture.py refs of the same tree: R, debug.pt_reference): mean FLIP over the
    level's pixels of the two put together alike, and of the light alone and the sun's visibility alone; the worst pose
    a scene's, against bake_flip"""
    root, refs_root = Path(args.captures), Path(args.refs)
    scenes = capture.load_scenes()
    limit = thresholds()["bake_flip"]
    results = {}
    for scene_id in args.scenes or sorted(p.name for p in root.iterdir() if p.name in scenes):
        folder = root / scene_id / "bake"
        sun, toward, present, gamma = bake_rig((root / scene_id / "log_bake.txt").read_text(errors="replace"))
        flips, light_flips, sun_flips, shares, raster_flips = [], [], [], [], []
        for index in range(len(scenes[scene_id]["probes"])):
            frame_b, frame_r, light_b, light_r, vis_b, vis_r, level = bake_frames(
                folder, refs_root / scene_id / "refs", index, sun, toward, present, gamma)
            share = float(level.mean())
            if share < 0.05:
                continue

            def masked(a, b):
                return float(flip.flip_map(a * 255.0, b * 255.0)[level].mean())
            flips.append(masked(frame_r, frame_b))
            light_flips.append(masked(light_r, light_b))
            sun_flips.append(masked(np.repeat(vis_r[..., None], 3, 2), np.repeat(vis_b[..., None], 3, 2)))
            shares.append(share)
            # (PRISM Raster's frame against R's converged frame: the design's looser bound)
            ref_frame = refs_root / scene_id / "refs" / f"p{index}_ref.png"
            if ref_frame.exists() and (folder / f"p{index}_raster.png").exists():
                raster_flips.append(float(flip.mean_flip(metrics.load(ref_frame), metrics.load(folder / f"p{index}_raster.png"))))
        if not flips:
            continue
        raster_limit = thresholds()["raster_flip"]
        results[scene_id] = {"flip": max(flips), "flips": flips, "light_flips": light_flips, "sun_flips": sun_flips,
                             "level_share": shares, "pass": max(flips) <= limit, "raster_flips": raster_flips,
                             "raster_pass": bool(raster_flips) and max(raster_flips) <= raster_limit}
        print("%-4s %-14s bake flip %.4f (poses %s; light alone %s; sun visibility %s)  %s" % (
            scene_id, scenes[scene_id]["name"], max(flips), " ".join("%.4f" % f for f in flips),
            " ".join("%.4f" % f for f in light_flips), " ".join("%.4f" % f for f in sun_flips),
            "PASS" if results[scene_id]["pass"] else "FAIL"), flush=True)
        if raster_flips:
            print("%-4s %-14s raster flip %.4f against R's frame (poses %s)  %s" % (
                scene_id, scenes[scene_id]["name"], max(raster_flips), " ".join("%.4f" % f for f in raster_flips),
                "PASS" if results[scene_id]["raster_pass"] else "FAIL"), flush=True)
    (root / "bake.json").write_text(json.dumps({"threshold": limit, "results": results}, indent=1))
    return 0 if results and all(r["pass"] for r in results.values()) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("measure")
    m.add_argument("captures")
    m.add_argument("scenes", nargs="*")
    m.add_argument("--state", default="pathtraced", help="the moving frames measured (M3's: v4pt, v4perf)")
    m.add_argument("--off", default="off", help="their RT off (M3's: v4off)")
    r = sub.add_parser("repro")
    r.add_argument("a")
    r.add_argument("b")
    e = sub.add_parser("equivalence")
    e.add_argument("captures")
    e.add_argument("scenes", nargs="*")
    s = sub.add_parser("material_source")
    s.add_argument("captures")
    s.add_argument("scenes", nargs="*")
    b = sub.add_parser("bake")
    b.add_argument("captures")
    b.add_argument("refs")
    b.add_argument("scenes", nargs="*")
    args = parser.parse_args()
    return {"measure": measure, "repro": repro, "equivalence": equivalence,
            "material_source": material_source, "bake": bake}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
