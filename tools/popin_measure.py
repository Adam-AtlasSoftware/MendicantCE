#!/usr/bin/env python3
"""Pop-in counted in a run's frames: the frames debug.screenshot_stream writes
(BGRA, the top row first, no header; with debug.camera_path, each of the
path's) read from a pipe as the game writes them, and each frame's change
from the one before measured in blocks of the picture.

The camera moves smoothly along the path (and the world stands still, the
path's game_speed 0), so a block's change from frame to frame changes
smoothly too; a switch drawn at once (a texture's level, a model's level of
detail, a replacement for the stock, a surface near or far) is a change in
some blocks in one frame that the frames on either side do not have. A
block is counted as switched at frame t where its change is over --least
(of 255) and over --ratio times the larger of its changes at t-1 and t+1;
a frame is one with a visible switch where --blocks or more blocks did. A
switch blended over frames (a fade, a dither, a level's bias eased) stays
under the ratio.

A run made with the same path, the same world and the levels' details at
their most (--record writes its frames, grey at half size) is a reference:
with --reference, each frame's difference from the reference's frame at the
same pose is measured too, whose motion cancels (the camera's and the
reference's are the same), so that a block whose difference jumps in one
frame (over --least, and --ratio times its jumps at t-1 and t+1) is a switch
without the false ones of an edge crossing a block quickly.

  popin_measure.py <pipe> <width> <height> <out folder> [--shots 12]

writes <out>/popin.json (the counts, and each switched frame's blocks) and
<out>/frames.csv (each frame's mean change, its switched blocks), and a
picture of the first --shots switched frames: the frame before, the frame,
and the blocks marked.
"""

import argparse
import hashlib
import json
import os
import sys

import numpy

try:
    from PIL import Image
except ImportError:
    Image = None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pipe")
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("out")
    parser.add_argument("--block", type=int, default=16, help="a block's side, in pixels of the frame scaled by 1/2")
    parser.add_argument("--least", type=float, default=6.0, help="the least change of a switched block (of 255)")
    parser.add_argument("--ratio", type=float, default=3.0)
    parser.add_argument("--blocks", type=int, default=2, help="switched blocks that make a frame one with a switch")
    parser.add_argument("--skip", type=int, default=3, help="the path's first frames not judged")
    parser.add_argument("--shots", type=int, default=12)
    parser.add_argument("--keep", default="", help="frames to save whole (comma-separated), for before/after pictures")
    parser.add_argument("--record", default="", help="a file to write the frames to, grey at half size")
    parser.add_argument("--reference", default="", help="a --record file of the same path to measure against")
    parser.add_argument("--hashes", default="", help="a file to write each frame's SHA-256 to (two builds' frames "
                        "compared byte for byte)")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    size = args.width * args.height * 4
    keep = set(int(v) for v in args.keep.split(",") if v)
    stream = open(args.pipe, "rb", buffering=0)
    h2, w2 = args.height // 2, args.width // 2
    bh, bw = h2 // args.block, w2 // args.block

    def read_frame():
        data = bytearray()
        while len(data) < size:
            chunk = stream.read(size - len(data))
            if not chunk:
                return None
            data += chunk
        return numpy.frombuffer(bytes(data), dtype=numpy.uint8).reshape(args.height, args.width, 4)

    def grey(frame):
        g = (frame[:h2 * 2, :w2 * 2, 2].astype(numpy.float32) * 0.299 + frame[:h2 * 2, :w2 * 2, 1] * 0.587 +
             frame[:h2 * 2, :w2 * 2, 0] * 0.114)
        return g.reshape(h2, 2, w2, 2).mean(axis=(1, 3))

    def blocks(diff):
        return diff[:bh * args.block, :bw * args.block].reshape(bh, args.block, bw, args.block).mean(axis=(1, 3))

    record = open(args.record, "wb") if args.record else None
    hashes = open(args.hashes, "w") if args.hashes else None
    reference = numpy.memmap(args.reference, dtype=numpy.uint8, mode="r") if args.reference else None
    reference_frames = reference.size // (h2 * w2) if reference is not None else 0
    errors = []      # E_t's blocks, the last three, against the reference
    jumps = []
    ref_rows = []
    ref_switched = []
    def save_shot(before_frame, frame, ys, xs, name):
        a = before_frame[:, :, [2, 1, 0]].copy()
        b = frame[:, :, [2, 1, 0]].copy()
        marked = b.copy()
        s = args.block * 2
        for y, x in zip(ys, xs):
            marked[y * s:(y + 1) * s, x * s:x * s + 2] = (255, 0, 255)
            marked[y * s:(y + 1) * s, (x + 1) * s - 2:(x + 1) * s] = (255, 0, 255)
            marked[y * s:y * s + 2, x * s:(x + 1) * s] = (255, 0, 255)
            marked[(y + 1) * s - 2:(y + 1) * s, x * s:(x + 1) * s] = (255, 0, 255)
        picture = numpy.concatenate([a, b, marked], axis=1)
        Image.fromarray(picture).resize((picture.shape[1] // 2, picture.shape[0] // 2)).save(
            os.path.join(args.out, name))

    ref_shots = 0
    rows = []
    switched = []
    shots = 0
    frames = []      # the last three frames (colour), for the pictures
    greys = []
    changes = []     # D_t for the last three
    index = 0
    while True:
        frame = read_frame()
        if frame is None:
            break
        if hashes:
            hashes.write("%d %s\n" % (index, hashlib.sha256(frame.tobytes()).hexdigest()))
        g = grey(frame)
        if record:
            record.write(numpy.clip(g, 0, 255).astype(numpy.uint8).tobytes())
        if reference is not None and index < reference_frames:
            r = reference[index * h2 * w2:(index + 1) * h2 * w2].reshape(h2, w2).astype(numpy.float32)
            e = blocks(numpy.abs(g - r))
            jumps.append(numpy.abs(e - errors[-1]) if errors else numpy.zeros_like(e))
            errors.append(e)
            if len(jumps) > 3:
                jumps.pop(0)
                errors.pop(0)
            if len(jumps) == 3 and index - 1 >= args.skip:
                before, now, after = jumps
                mask = (now > args.least) & (now > args.ratio * numpy.maximum(before, after))
                count = int(mask.sum())
                ref_rows.append((index - 1, float(errors[1].mean()), count))
                if count >= args.blocks:
                    ys, xs = numpy.nonzero(mask)
                    ref_switched.append({"frame": index - 1, "blocks": count, "largest": float(now[mask].max()),
                                         "where": [[int(x), int(y)] for x, y in zip(xs[:64], ys[:64])]})
                    if ref_shots < args.shots and Image and len(frames) >= 2:
                        ref_shots += 1
                        save_shot(frames[-2], frames[-1], ys, xs, "refswitch%05d.png" % (index - 1))
        if index in keep and Image:
            Image.fromarray(frame[:, :, [2, 1, 0]]).save(os.path.join(args.out, "frame%05d.png" % index))
        if greys:
            changes.append(blocks(numpy.abs(g - greys[-1])))
        else:
            changes.append(numpy.zeros((bh, bw), numpy.float32))
        greys.append(g)
        frames.append(frame)
        if len(greys) > 3:
            greys.pop(0)
            frames.pop(0)
            changes.pop(0)
        # judge the frame before this one (t = index - 1), its neighbours known
        if len(changes) == 3 and index - 1 >= args.skip:
            before, now, after = changes
            mask = (now > args.least) & (now > args.ratio * numpy.maximum(before, after))
            count = int(mask.sum())
            rows.append((index - 1, float(now.mean()), count))
            if count >= args.blocks:
                ys, xs = numpy.nonzero(mask)
                switched.append({"frame": index - 1, "blocks": count, "largest": float(now[mask].max()),
                                 "where": [[int(x), int(y)] for x, y in zip(xs[:64], ys[:64])]})
                if shots < args.shots and Image and reference is None:
                    shots += 1
                    save_shot(frames[0], frames[1], ys, xs, "switch%05d.png" % (index - 1))
        index += 1
    if record:
        record.close()
    if hashes:
        hashes.close()
    judged = len(rows)
    result = {
        "reference": {
            "file": args.reference,
            "judged": len(ref_rows),
            "frames_with_switch": len(ref_switched),
            "switched_blocks": sum(s["blocks"] for s in ref_switched),
            "switches_per_1000_frames": 1000.0 * len(ref_switched) / len(ref_rows) if ref_rows else 0.0,
            "mean_difference": float(numpy.mean([r[1] for r in ref_rows])) if ref_rows else 0.0,
            "switched": ref_switched,
        } if reference is not None else None,
        "frames": index,
        "judged": judged,
        "frames_with_switch": len(switched),
        "switched_blocks": sum(s["blocks"] for s in switched),
        "switches_per_1000_frames": 1000.0 * len(switched) / judged if judged else 0.0,
        "mean_change": float(numpy.mean([r[1] for r in rows])) if rows else 0.0,
        "settings": {"block": args.block, "least": args.least, "ratio": args.ratio, "blocks": args.blocks},
        "switched": switched,
    }
    json.dump(result, open(os.path.join(args.out, "popin.json"), "w"), indent=1)
    with open(os.path.join(args.out, "frames.csv"), "w") as csv:
        csv.write("frame,mean_change,switched_blocks\n")
        for r in rows:
            csv.write("%d,%.3f,%d\n" % r)
    print("%d frames, %d judged, %d with a switch (%.1f per 1000), %d blocks" % (
        index, judged, len(switched), result["switches_per_1000_frames"], result["switched_blocks"]))
    if reference is not None:
        r = result["reference"]
        print("against the reference: %d judged, %d with a switch (%.1f per 1000), %d blocks, mean difference %.2f" % (
            r["judged"], r["frames_with_switch"], r["switches_per_1000_frames"], r["switched_blocks"],
            r["mean_difference"]))
        with open(os.path.join(args.out, "reference.csv"), "w") as csv:
            csv.write("frame,mean_difference,switched_blocks\n")
            for row in ref_rows:
                csv.write("%d,%.3f,%d\n" % row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
