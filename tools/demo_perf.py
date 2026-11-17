#!/usr/bin/env python3
"""A demo and its performance log (debug.demo_perf_log: demos/<name>.perf beside demos/<name>.demo, written by
port/linux/src/report.c), joined by tick: the log's summaries, its events, the slowest frames and the ticks they were
at, and whether the log's ticks lie inside the demo's.

    python tools/demo_perf.py demos/20261117-142233-bloodgulch.demo
    python tools/demo_perf.py --slowest 20 demos/kept/....demo     # (a pinned demo's log stays in demos/)
"""

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from demo_info import Demo  # noqa: E402


def read_perf(path: Path):
    """the log's header (name, first tick), frames (tick, frame, frame_us, cpu_us, gpu_us, render_us, waited_us),
    events (tick, kind, value), summaries (tick, frames, average_ms, low1_fps, low01_fps) and end tick (None: cut
    short)"""
    header, frames, events, summaries, end = None, [], [], [], None
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if line.startswith("# demo perf log 1 "):
            header = (fields[5], int(fields[6]))
        elif fields[0] == "#" and len(fields) == 3 and fields[1] == "end":
            end = int(fields[2])
        elif fields[0] == "f" and len(fields) == 8:
            frames.append(tuple(int(value) for value in fields[1:]))
        elif fields[0] == "e" and len(fields) == 4:
            events.append((int(fields[1]), fields[2], float(fields[3])))
        elif fields[0] == "s" and len(fields) == 6:
            summaries.append((int(fields[1]), int(fields[2]), float(fields[3]), float(fields[4]), float(fields[5])))
    if header is None:
        raise ValueError(f"{path} is not a demo's performance log")
    return header, frames, events, summaries, end


def demo_ticks(demo: Demo):
    """the first and last tick the demo recorded (its T records)"""
    first = last = None
    for _, record_type, data in demo.records("R"):
        if record_type == "T":
            tick = struct.unpack_from("<I", data)[0]
            first = tick if first is None else first
            last = tick
    return first, last


def perf_path(demo_path: Path) -> Path:
    """a demo's log: beside it, or in demos/ for a pinned demo in demos/kept/"""
    beside = demo_path.with_suffix(".perf")
    if beside.exists() or demo_path.parent.name != "kept":
        return beside
    return demo_path.parent.parent / beside.name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("demo", type=Path)
    parser.add_argument("--slowest", type=int, default=10, help="the slowest frames listed (default 10)")
    args = parser.parse_args()
    demo = Demo(args.demo.read_bytes())
    first, last = demo_ticks(demo)
    path = perf_path(args.demo)
    (name, log_first), frames, events, summaries, end = read_perf(path)
    print(f"{args.demo.name}: ticks {first} to {last}; {path.name}: {len(frames)} frames from tick {log_first}"
          + (f" to {end}" if end is not None else ", cut short"))
    if name != args.demo.name:
        print(f"  the log is of {name}, not this demo")
    outside = [frame for frame in frames if first is None or not first <= frame[0] <= last + 1]
    if outside:
        print(f"  {len(outside)} frames at ticks outside the demo's (the first at tick {outside[0][0]})")
    for tick, count, average, low1, low01 in summaries:
        print(f"  to tick {tick}: {count} frames, {average:.2f} ms average, 1% low {low1:.0f} fps, "
              f"0.1% low {low01:.0f} fps")
    for tick, kind, value in events:
        print(f"  tick {tick}: {kind} {value:g}" + (" ms" if kind in ("stall", "load") else ""))
    for tick, frame, frame_us, cpu_us, gpu_us, render_us, waited_us in sorted(frames, key=lambda f: -f[2])[:args.slowest]:
        print(f"  frame {frame} at tick {tick}: {frame_us / 1000:.2f} ms (CPU {cpu_us / 1000:.2f}, "
              f"GPU {'--' if gpu_us < 0 else f'{gpu_us / 1000:.2f}'}, render {render_us / 1000:.2f}, "
              f"waited {waited_us / 1000:.2f})")
    return 1 if outside or name != args.demo.name else 0


if __name__ == "__main__":
    sys.exit(main())
