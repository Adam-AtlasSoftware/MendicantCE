#!/usr/bin/env python3
"""The checks every PRISM milestone passes against the build it started from (docs/prism_gates.md, "Every milestone"):

    python3 tools/prism_gate/identity.py frames <base tree> <new tree> --data <data> --out <dir> [--config <toml>]
    python3 tools/prism_gate/identity.py determinism <base tree> <new tree> --data <data> --out <dir>

frames: each tree run twice (hidden, silent, isolated: game.py's rules), on each map given, with the clock fixed (a
tick a frame) and interpolation off, every frame from the simulation's second `--from` to `--until` saved; the
frames compared byte for byte: each tree with itself (the floor: a tree that differs from itself proves nothing)
and the new with the base. With no --config, the game's defaults (Classic); with one, its settings (the kit's, RT
off: display.lighting "ce").

stills: each tree run twice on each scene of the capture set given (capture.py's harness: its camera path, the
clock fixed and stopped, the kit's settings with --config) with RT off (display.lighting "ce"): the last 30 frames of
a 300-frame hold at the scene's first probe pose compared byte for byte, as `frames` compares. The path keeps to
what a build before the gates' path changes reads (its keys and commands few, no save lines).

determinism: each tree run twice on a10 with the determinism log (debug.determinism_log), the frames' time fixed and
a scripted player (debug.test_input "bot:3"); tools/determinism_compare.py --simulation compares the runs, each
tree with itself and the new with the base (--builds: the seeds' takers counted, not placed by their code's
addresses): the gameplay untouched.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prism_gate import capture, game, paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def run_tree(tree, data_source, work, game_map, env, config=None, timeout=900):
    pid = game.kit_running()
    if pid:
        raise SystemExit(f"stopped: the kit's game is running (pid {pid})")
    work = Path(work).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    (work / "shots").mkdir()
    stage = work / "stage"
    shutil.copytree(tree, stage, symlinks=True)
    if config:
        shutil.copy(config, stage / "config.toml")
    data = work / "data"
    game.stage_data(data_source, data)
    (work / "saves").mkdir()
    (data / "init.txt").write_text(f"map_name {game_map}\n")
    e = game.isolated_environment(work, data)
    e.update({"HALO_WINDOW_SIZE": "1280x720"})
    e.update(env)
    # (a variable given as None is left out: the determinism check's frames are timed by debug.frame_time alone)
    e = {k: v for k, v in e.items() if v is not None}
    with open(work / "stdout.txt", "w") as out:
        process = subprocess.Popen([str(stage / "bin" / "openh1")], cwd=data, env=e, stdout=out,
                                   stderr=subprocess.STDOUT, preexec_fn=game._die_with_parent)
        try:
            process.wait(timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    shutil.rmtree(stage, ignore_errors=True)
    return work, process.returncode


def frame_hashes(folder):
    return [hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(folder).glob("*.bmp"))]


def compare(a, b):
    n = min(len(a), len(b))
    return sum(x == y for x, y in zip(a, b)), n, len(a), len(b)


def frames(args):
    failed = False
    for game_map in args.maps:
        hashes = {}
        for tree_name, tree in (("base", args.base), ("new", args.new)):
            for run in (1, 2):
                work = (Path(args.out) / f"{game_map}_{tree_name}{run}").resolve()
                shots = work / "shots"
                env = {"HALO_SCREENSHOT_DIR": str(shots), "HALO_SCREENSHOT_EVERY": "1",
                       "HALO_SCREENSHOT_FROM": str(args.from_), "HALO_SCREENSHOT_UNTIL": str(args.until),
                       "HALO_EXIT_AFTER": str(args.exit_after), "VK_LOADER_DEVICE_ID_FILTER": args.device}
                started = time.time()
                work, code = run_tree(tree, args.data, work, game_map, env, args.config)
                (work / "shots").mkdir(exist_ok=True)
                hashes[f"{tree_name}{run}"] = frame_hashes(work / "shots")
                print(f"{game_map} {tree_name}{run}: {len(hashes[tree_name + str(run)])} frames, exit {code}, "
                      f"{time.time() - started:.0f} s", flush=True)
                if not args.keep:
                    for bmp in (work / "shots").glob("*.bmp"):
                        bmp.unlink()
        for a, b in (("base1", "base2"), ("new1", "new2"), ("base1", "new1"), ("base2", "new2")):
            same, n, na, nb = compare(hashes[a], hashes[b])
            ok = n > 0 and same == n and na == nb
            failed |= not ok
            print(f"{game_map}: {a} vs {b}: {same}/{n} frames byte-identical (of {na}/{nb}) {'OK' if ok else 'DIFFER'}")
    return 1 if failed else 0


def stills(args):
    failed = False
    scenes = capture.load_scenes()
    for scene_id in args.scenes:
        scene = scenes[scene_id]
        t = capture.begin(scene, by_tick=False)
        pose = paths.move_poses(scene["keys"], scene["steps"])[capture.probe_steps(scene)[0] - 1]
        t.hold(pose, 270)
        for index in range(30):
            t.hold(pose, 1, save=f"{index:02d}")
        hashes = {}
        for tree_name, tree in (("base", args.base), ("new", args.new)):
            for run in (1, 2):
                name = f"{tree_name}{run}"
                out = (Path(args.out) / f"{scene_id}_{name}").resolve()
                wanted = {frame: f"frame_{n}.png" for frame, n in t.saves.items()}
                r = game.Run(tree, args.data, out / "run", scene["map"],
                             t.text(scene["start"], save_lines=False, start_tick=0),
                             wanted, config=args.config, device=args.device, every_frame=True)
                got = r.wait(3600)
                hashes[name] = [got[wanted[f]] for f in sorted(wanted)]
                shutil.rmtree(out / "run", ignore_errors=True)
                print(f"{scene_id} {name}: {len(hashes[name])} frames", flush=True)
        for a, b in (("base1", "base2"), ("new1", "new2"), ("base1", "new1"), ("base2", "new2")):
            same, n, na, nb = compare(hashes[a], hashes[b])
            ok = n > 0 and same == n and na == nb
            failed |= not ok
            print(f"{scene_id}: {a} vs {b}: {same}/{n} frames byte-identical {'OK' if ok else 'DIFFER'}", flush=True)
    return 1 if failed else 0


def determinism(args):
    failed = False
    logs = {}
    for tree_name, tree in (("base", args.base), ("new", args.new)):
        for run in (1, 2):
            work = (Path(args.out) / f"determinism_{tree_name}{run}").resolve()
            # (the log in the data folder, the run's working folder)
            env = {"HALO_DETERMINISM_LOG": "determinism.txt", "HALO_FRAME_TIME": "0.0166667",
                   "HALO_TEST_INPUT": "bot:3", "HALO_EXIT_AFTER": str(args.exit_after), "HALO_FIXED_STEP": None,
                   "VK_LOADER_DEVICE_ID_FILTER": args.device}
            work, code = run_tree(tree, args.data, work, args.map, env)
            logs[f"{tree_name}{run}"] = work / "data" / "determinism.txt"
            print(f"determinism {tree_name}{run}: exit {code}", flush=True)
    for a, b in (("base1", "base2"), ("new1", "new2"), ("base1", "new1")):
        # (across the two builds, the seeds' takers by how many, not by their code's addresses: --builds)
        result = subprocess.run([sys.executable, str(ROOT / "tools/determinism_compare.py"), str(logs[a]),
                                 str(logs[b]), "--simulation"] + (["--builds"] if a[:-1] != b[:-1] else []),
                                capture_output=True, text=True)
        failed |= result.returncode != 0
        print(f"{a} vs {b}: exit {result.returncode}: {result.stdout.strip().splitlines()[-1:]}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("check", choices=("frames", "stills", "determinism"))
    parser.add_argument("base")
    parser.add_argument("new")
    parser.add_argument("--data", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--config", help="frames: a config.toml (none: the game's defaults)")
    parser.add_argument("--maps", nargs="*", default=["b30", "a10"])
    parser.add_argument("--scenes", nargs="*", default=["G2", "G4", "G5", "G7"], help="stills' scenes")
    parser.add_argument("--map", default="a10", help="determinism's map")
    parser.add_argument("--from", dest="from_", type=float, default=40.0)
    parser.add_argument("--until", type=float, default=41.5)
    parser.add_argument("--exit-after", type=int, default=100)
    parser.add_argument("--device", default="0x10de:0x2482")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    return {"frames": frames, "stills": stills, "determinism": determinism}[args.check](args)


if __name__ == "__main__":
    sys.exit(main())
