#!/usr/bin/env python3
"""The evidence a milestone is looked at by (docs/prism_gates.md): from a scene's captures (capture.py all_v4), a
still sheet and a moving camera's clip, side by side:

    python3 tools/prism_gate/evidence.py sheet <captures> G5 [--state v4pt] [--off v4off]  # <captures>/G5/sheet.png
    python3 tools/prism_gate/evidence.py clip <captures> G5 [--state v4pt] [--off v4off]   # <captures>/G5/clip.mp4

sheet: a row a probe pose: RT off's still (Classic), the path tracer's still after its settle, its moving frame at
the pose, the reference R, and |moving - R| (8-bit, x4).
clip: the camera's move at 30 frames a second, RT off beside the path tracer; at each probe pose the move stops for
a second and a half with R beside them (the reference exists at the probe poses only). H.264 (ffmpeg), under 20 MB.
The captures are the player's own maps: neither file is committed.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prism_gate import capture  # noqa: E402

TILE = (640, 360)


def tile(path, label, size=TILE):
    if path is None or not Path(path).exists():
        image = Image.new("RGB", size, (24, 24, 24))
        label = f"{label} (none)"
    else:
        image = Image.open(path).convert("RGB").resize(size, Image.BILINEAR)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 8 + 7 * len(label), 16), fill=(0, 0, 0))
    draw.text((4, 3), label, fill=(255, 255, 255))
    return image


def difference(a_path, b_path, label, size=TILE):
    if not (Path(a_path).exists() and Path(b_path).exists()):
        return tile(None, label, size)
    a = np.asarray(Image.open(a_path).convert("RGB").resize(size, Image.BILINEAR), np.float32)
    b = np.asarray(Image.open(b_path).convert("RGB").resize(size, Image.BILINEAR), np.float32)
    d = np.clip(np.abs(a - b).mean(2) * 4.0, 0, 255).astype(np.uint8)
    heat = np.stack([d, np.clip(d.astype(np.int32) * 2 - 255, 0, 255).astype(np.uint8), np.zeros_like(d)], 2)
    image = Image.fromarray(heat)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 8 + 7 * len(label), 16), fill=(0, 0, 0))
    draw.text((4, 3), label, fill=(255, 255, 255))
    return image


def sheet(args):
    scene = capture.load_scenes()[args.scene]
    folder = Path(args.captures) / args.scene
    rows = []
    for index, step in enumerate(capture.probe_steps(scene)):
        refs, moves = folder / "refs", folder / "moves"
        row = [tile(refs / f"p{index}_off.png", f"p{index} RT off (Classic)"),
               tile(refs / f"p{index}_live.png", f"p{index} Path Traced, still"),
               tile(moves / f"m_{args.state}_{step:03d}.png", f"p{index} Path Traced, moving (step {step})"),
               tile(refs / f"p{index}_ref.png", f"p{index} R"),
               difference(moves / f"m_{args.state}_{step:03d}.png", refs / f"p{index}_ref.png",
                          f"p{index} |moving - R| x4")]
        rows.append(row)
    out = Image.new("RGB", (TILE[0] * 5, TILE[1] * len(rows)))
    for r, row in enumerate(rows):
        for c, image in enumerate(row):
            out.paste(image, (c * TILE[0], r * TILE[1]))
    path = folder / "sheet.png"
    out.save(path)
    print(path)


def clip(args):
    scene = capture.load_scenes()[args.scene]
    folder = Path(args.captures) / args.scene
    total = scene["steps"] * (len(scene["keys"]) - 1)
    probes = {step: index for index, step in enumerate(capture.probe_steps(scene))}
    work = folder / "clip_frames"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    n = 0
    size = (640, 360)
    for step in range(1, total + 1):
        off = folder / "moves" / f"m_{args.off}_{step:03d}.png"
        pt = folder / "moves" / f"m_{args.state}_{step:03d}.png"
        if step in probes:
            index = probes[step]
            ref = folder / "refs" / f"p{index}_ref.png"
            panels = [tile(off, f"RT off (Classic), step {step}", size), tile(pt, f"Path Traced v1, step {step}", size),
                      tile(ref, f"R at probe p{index}", size)]
            repeat = 45
        else:
            panels = [tile(off, f"RT off (Classic), step {step}", size), tile(pt, f"Path Traced v1, step {step}", size),
                      tile(None, "R: at the probe poses only", size)]
            repeat = 1
        frame = Image.new("RGB", (size[0] * 3, size[1]))
        for c, image in enumerate(panels):
            frame.paste(image, (c * size[0], 0))
        for _ in range(repeat):
            frame.save(work / f"f{n:05d}.png")
            n += 1
    path = folder / "clip.mp4"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", str(work / "f%05d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-preset", "slow", str(path)], check=True)
    shutil.rmtree(work, ignore_errors=True)
    print(path, round(path.stat().st_size / 1e6, 2), "MB")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=("sheet", "clip"))
    parser.add_argument("captures")
    parser.add_argument("scene")
    parser.add_argument("--state", default="v4pt")
    parser.add_argument("--off", default="v4off")
    args = parser.parse_args()
    sheet(args) if args.what == "sheet" else clip(args)


if __name__ == "__main__":
    main()
