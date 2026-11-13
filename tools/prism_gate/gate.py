#!/usr/bin/env python3
"""The quality gate (docs/prism_gates.md) over captures made by capture.py, against thresholds.json's thresholds:

    python3 tools/prism_gate/gate.py measure <captures> [scene ...]   # results.json and a table
    python3 tools/prism_gate/gate.py repro <captures a> <captures b>  # every frame of both byte for byte alike?

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


def measure_scene(scene, folder, limits):
    moves, refs = folder / "moves", folder / "refs"
    errors, blotches, flips, skies = [], [], [], []
    for index, step in enumerate(capture.probe_steps(scene)):
        ref = metrics.load(refs / f"p{index}_ref.png")
        moving = metrics.load(moves / f"m_pathtraced_{step:03d}.png")
        keep = metrics.keep_mask(metrics.load(moves / f"m_off_{step:03d}.png"),
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
              "flicker": metrics.flicker(frames("pathtraced"), frames("off")), "errors": errors,
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
        r = measure_scene(scenes[scene_id], root / scene_id, limits)
        results[scene_id] = r
        print("%-4s %-14s error %.4f  blotch %.4f  flip %.3f  flicker %.2f  %s" % (
            scene_id, scenes[scene_id]["name"], r["error"], r["blotch"], r["flip"], r["flicker"],
            "HARNESS FAULT (sky)" if r["harness_fault"] else "PASS" if r["pass"] else "FAIL"), flush=True)
    (root / "results.json").write_text(json.dumps({"thresholds": limits, "results": results}, indent=1))
    return 0 if all(r["pass"] for r in results.values()) else 1


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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("measure")
    m.add_argument("captures")
    m.add_argument("scenes", nargs="*")
    r = sub.add_parser("repro")
    r.add_argument("a")
    r.add_argument("b")
    args = parser.parse_args()
    return measure(args) if args.command == "measure" else repro(args)


if __name__ == "__main__":
    sys.exit(main())
