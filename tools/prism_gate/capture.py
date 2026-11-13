#!/usr/bin/env python3
"""The quality gates' captures (docs/prism_gates.md): a scene of the frozen set (scenes.json) captured from a staged
tree, hidden, silent and isolated (game.py), driven by a camera path (paths.py) so that every frame is the same in
every run.

    python3 tools/prism_gate/capture.py moves G2 --stage <tree> --data <data> --config <config.toml> --out <dir>
    python3 tools/prism_gate/capture.py refs G2 ...      # the references at the probe poses
    python3 tools/prism_gate/capture.py all ...          # every scene, moves and references

moves: the camera moved along the scene's keys a step at a time (`steps` a leg, each step held `step_frames`
frames, its last saved) in each state: RT off, path traced (faithful, denoised) and raw (the denoiser off).
refs: at the probe poses (a quarter, a half and three quarters of the way), the camera still: RT off's frame, then
the path tracer's converged reference (debug.pt_reference, `reference_frames` frames: its linear EXR and the frame
it holds).

The harness's rules come first, at the path's frame 0: the console's lines and the HUD off, the player deathless,
the weather off, the AI erased (not in a live scene); then the game runs `settle` frames with the camera at the
first key (a level's opening plays out: the director takes the path's camera only once it has, and only in a tick;
projectiles land, a hit's flash fades) and is stopped (game_speed 0; a live scene's runs on). Every capture writes manifest.json: each frame's SHA-256 (of its
pixels) and the files' list, which `gate.py repro` compares between two runs.
"""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prism_gate import game, paths  # noqa: E402

HERE = Path(__file__).resolve().parent
RULES = ("terminal_render 0", "show_hud 0", "show_hud_help_text 0", "cinematic_show_letterbox 0", "cheat_deathless_player 1", "weather 0")
STATES = {
    "off": ('prism_set "rt off"',),
    "pathtraced": ('prism_set "rt pathtraced"', 'prism_set "rt_style faithful"',
                   'prism_set "rt_denoiser spatiotemporal"'),
    "raw": ('prism_set "rt pathtraced"', 'prism_set "rt_style faithful"', 'prism_set "rt_denoiser off"'),
}


def load_scenes(path=HERE / "scenes.json"):
    data = json.loads(Path(path).read_text())
    defaults = data["defaults"]
    scenes = {}
    for scene in data["scenes"]:
        merged = dict(defaults)
        merged.update(scene)
        merged["keys"] = [paths.parse_pose(k) for k in scene["keys"]]
        scenes[scene["id"]] = merged
    return scenes


def probe_steps(scene):
    total = scene["steps"] * (len(scene["keys"]) - 1)
    return [max(1, int(total * q)) for q in scene["probes"]]


def begin(scene):
    t = paths.Timeline()
    t.at(*RULES)
    if not scene.get("live"):
        t.at("ai_erase_all")
    if scene.get("bsp") is not None:
        t.at(f"switch_bsp {scene['bsp']}")
    t.at(*scene.get("setup", []))
    t.hold(scene["keys"][0], scene["settle"])
    if not scene.get("live"):
        t.at("game_speed 0")
    return t


def moves_timeline(scene, states):
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    for state in states:
        t.at(*STATES[state])
        t.hold(scene["keys"][0], scene["state_settle"])
        for step, pose in enumerate(poses, start=1):
            t.hold(pose, scene["step_frames"], save=f"moves/m_{state}_{step:03d}.png")
    return t


def refs_timeline(scene, frames):
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    for index, step in enumerate(probe_steps(scene)):
        pose = poses[step - 1]
        if scene.get("live"):
            # (a live scene's world at the probe: its moves' run replayed to the step, then stopped)
            for s in range(1 if index == 0 else probe_steps(scene)[index - 1] + 1, step + 1):
                t.hold(poses[s - 1], scene["step_frames"])
            t.at("game_speed 0")
        t.at(*STATES["off"])
        t.hold(pose, scene["state_settle"], save=f"refs/p{index}_off.png")
        t.at(*STATES["pathtraced"])
        t.hold(pose, scene["pt_settle"])
        t.at(f'prism_set "pt_reference_file p{index}.exr"', f'prism_set "pt_reference {frames}"')
        t.hold(pose, frames + scene["reference_margin"], save=f"refs/p{index}_ref.png")
        t.at('prism_set "pt_reference 0"')
        if scene.get("live"):
            t.at("game_speed 1")
    # (the last file written once the GPU is done with its sums: a few frames on)
    t.hold(t.keys[max(t.keys)], 60)
    return t


def run(scene, timeline, out, kind, args):
    out.mkdir(parents=True, exist_ok=True)
    for folder in ("moves", "refs"):
        (out / folder).mkdir(exist_ok=True)
    started = time.time()
    env = {}
    if scene.get("input"):
        env["HALO_TEST_INPUT"] = scene["input"]
    r = game.Run(args.stage, args.data, out / f"run_{kind}", scene["map"], timeline.text(scene["start"]),
                 timeline.saves, config=args.config, env=env, device=args.device)
    hashes = r.wait(args.timeout)
    work = out / f"run_{kind}"
    for exr in sorted((work / "data").glob("p*.exr")):
        shutil.move(str(exr), out / "refs" / exr.name)
    shutil.copy(work / "path.txt", out / f"path_{kind}.txt")
    shutil.copy(work / "stdout.txt", out / f"log_{kind}.txt")
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"frames": {}, "runs": {}}
    manifest["frames"].update({name: digest for name, digest in hashes.items()})
    manifest["runs"][kind] = {"frames": timeline.length, "seconds": round(time.time() - started, 1)}
    manifest_path.write_text(json.dumps(manifest, indent=1, sort_keys=True))
    print(f"{scene['id']} {kind}: {timeline.length} frames, {len(hashes)} saved, {time.time() - started:.0f} s",
          flush=True)


def capture(scene, what, args):
    out = Path(args.out) / scene["id"]
    if what in ("moves", "all"):
        if scene.get("live"):
            for state in STATES:
                run(scene, moves_timeline(scene, [state]), out, f"moves_{state}", args)
        else:
            run(scene, moves_timeline(scene, list(STATES)), out, "moves", args)
    if what in ("refs", "all"):
        run(scene, refs_timeline(scene, args.reference_frames or scene["reference_frames"]), out, "refs", args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=("moves", "refs", "all"))
    parser.add_argument("scenes", nargs="*", help="scene ids (default: every scene)")
    parser.add_argument("--stage", required=True, help="a Linux release tree (tools/linux_package.py tree)")
    parser.add_argument("--data", required=True, help="a data folder: maps, mods, prism_world, prism_lightmaps, "
                        "world_sdf (linked, read only)")
    parser.add_argument("--config", required=True, help="the config.toml every run takes (display.prism_rt off)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--device", default="0x10de:0x2482", help="VK_LOADER_DEVICE_ID_FILTER (default the 3070 Ti)")
    parser.add_argument("--reference-frames", type=int, default=0, help="debug.pt_reference's frames (default the "
                        "scene's)")
    parser.add_argument("--timeout", type=float, default=7200)
    parser.add_argument("--keep", action="store_true", help="keep each run's folder")
    args = parser.parse_args()
    scenes = load_scenes()
    for scene_id in args.scenes or list(scenes):
        capture(scenes[scene_id], args.what, args)


if __name__ == "__main__":
    main()
