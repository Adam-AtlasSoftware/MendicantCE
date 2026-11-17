#!/usr/bin/env python3
"""A small reader of the speedrun state block (docs/speedrun_state.md) on Linux: finds a running game, scans its
memory for the block's signature through /proc/<pid>/mem, and prints its fields, a line each time the flags, run,
levels or map change, and every --every seconds.

    python3 tools/speedrun_state_read.py                    the game named halo, as long as it runs
    python3 tools/speedrun_state_read.py --pid 1234 --seconds 60
    python3 tools/speedrun_state_read.py --environ HALO_DATA_ROOT=/path/to/run/data   the game with this setting

Reading another process's memory needs the same user, and where Yama's ptrace_scope is 1, a parent of the game or
CAP_SYS_PTRACE.
"""

import argparse
import errno
import os
import struct
import sys
import time

MAGIC = b"GOAT H1 SPEEDRUN"
LAYOUT = struct.Struct("<16s13I44s64s")
FIELDS = ("magic", "version", "size", "sequence", "frame", "flags", "run", "run_ticks", "levels_completed",
          "game_ticks", "ticks_per_second", "sim_rate", "settings", "build_number", "build", "map")
FLAGS = ((1, "loading"), (2, "cutscene"), (4, "menu"), (8, "game"))


def find_pid(environ):
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/comm") as comm:
                if comm.read().strip() != "halo":
                    continue
            if environ:
                with open(f"/proc/{name}/environ", "rb") as data:
                    if environ.encode() not in data.read().split(b"\0"):
                        continue
        except OSError:
            continue
        return int(name)
    return None


def find_block(pid):
    """the block's address: its signature in the process's writable memory, the low 4 GB (the guest's) first"""
    regions = []
    with open(f"/proc/{pid}/maps") as maps:
        for line in maps:
            parts = line.split()
            start, end = (int(x, 16) for x in parts[0].split("-"))
            if parts[1].startswith("rw"):
                regions.append((start >= 1 << 32, start, end))
    with open(f"/proc/{pid}/mem", "rb", buffering=0) as memory:
        for _, start, end in sorted(regions):
            at = start
            while at < end:
                size = min(end - at, 16 << 20)
                try:
                    memory.seek(at)
                    data = memory.read(size + len(MAGIC) - 1)
                except OSError:
                    break
                found = data.find(MAGIC)
                if found >= 0:
                    return at + found
                at += size
    return None


def read(memory, address):
    """the block's fields; OSError once the game has exited"""
    memory.seek(address)
    data = memory.read(LAYOUT.size)
    if len(data) != LAYOUT.size:
        raise OSError("the game exited")
    values = dict(zip(FIELDS, LAYOUT.unpack(data)))
    for name in ("build", "map"):
        values[name] = values[name].split(b"\0")[0].decode("latin-1")
    return values


def describe(values, started):
    flags = ",".join(name for bit, name in FLAGS if values["flags"] & bit) or "-"
    return (f"{time.monotonic() - started:7.2f}s frame={values['frame']} flags={flags} run={values['run']} "
            f"run_ticks={values['run_ticks']} game_ticks={values['game_ticks']} "
            f"levels={values['levels_completed']} map={values['map']} sim_rate={values['sim_rate']} "
            f"settings=0x{values['settings']:x} build={values['build_number']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pid", type=int)
    parser.add_argument("--environ", help="VAR=VALUE the game's environment has")
    parser.add_argument("--seconds", type=float, default=0, help="stop after this (0: when the game ends)")
    parser.add_argument("--every", type=float, default=1.0, help="a line at least this often")
    args = parser.parse_args()

    started = time.monotonic()
    pid = args.pid
    address = None
    while address is None:
        if args.seconds and time.monotonic() - started > args.seconds:
            sys.exit("no game with the block found")
        pid = args.pid or find_pid(args.environ)
        if pid:
            try:
                address = find_block(pid)
            except OSError as error:
                if error.errno in (errno.ESRCH, errno.ENOENT):
                    print("game exited", flush=True)
                    return
                sys.exit(f"cannot read process {pid}: {error}")
        if address is None:
            time.sleep(0.2)
    print(f"block of process {pid} at 0x{address:x}", flush=True)
    last_key, last_line = None, 0.0
    try:
        memory = open(f"/proc/{pid}/mem", "rb", buffering=0)
    except OSError:
        print("game exited", flush=True)
        return
    with memory:
        while not args.seconds or time.monotonic() - started < args.seconds:
            try:
                values = read(memory, address)
            except (OSError, struct.error):
                print("game exited", flush=True)
                return
            if values["sequence"] % 2:
                continue
            key = (values["flags"], values["run"], values["levels_completed"], values["map"])
            now = time.monotonic()
            if key != last_key or now - last_line >= args.every:
                print(describe(values, started), flush=True)
                last_key, last_line = key, now
            time.sleep(0.01)
    print("stopped after --seconds", flush=True)

if __name__ == "__main__":
    main()
