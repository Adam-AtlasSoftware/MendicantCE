#!/usr/bin/env python3
"""The quality gates' captures (docs/prism_gates.md): a scene of the frozen set (scenes.json) captured from a staged
tree, hidden, silent and isolated (game.py), driven by a camera path (paths.py) so that every frame is the same in
every run.

    python3 tools/prism_gate/capture.py moves G2 --stage <tree> --data <data> --config <config.toml> --out <dir>
    python3 tools/prism_gate/capture.py refs G2 ...      # the references at the probe poses
    python3 tools/prism_gate/capture.py all ...          # every scene, moves and references
    python3 tools/prism_gate/capture.py equivalence G2 ... # M1's: "ce" and "ce_deferred" at the probe poses, and
                                                           # the G-buffer's views (gate.py material_source)
    python3 tools/prism_gate/capture.py all_v4 G5 ... --states v4off v4pt v4raw --env HALO_BAKE_FOLDER=<bakes>
                                                           # M3's: Path Traced's moves, and its references (refs_v4)

moves: the camera moved along the scene's keys a step at a time (`steps` a leg, each step held `step_frames`
frames, its last saved) in each state: RT off, path traced (faithful, denoised) and raw (the denoiser off).
refs: at the probe poses (a quarter, a half and three quarters of the way), the camera still: RT off's frame, then
the path tracer's converged reference (debug.pt_reference, `reference_frames` frames: its linear EXR and the frame
it holds).

The harness's rules come first, at the path's frame 0: the console's lines and the HUD off, the player deathless,
the weather off, the particles not drawn, the AI erased (not in a live scene, and again every 60 ticks); then the
game runs with the camera at the first key (a level's opening plays out: the director takes the path's camera only
once it has, and only in a tick; projectiles land, a hit's flash fades) and is stopped by its clock, at the same tick
every run (game_speed 0; a live scene's runs on), `settle` frames in all. Every capture writes
manifest.json: each frame's SHA-256 (of its pixels), which `gate.py repro` compares between two runs.
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
# (the particles, their systems, the weather's and the contrails not drawn: they move frame by frame while the game
# is stopped, by the frames' own randomness, which differs from run to run; and they are not the light's)
RULES = ("terminal_render 0", "show_hud 0", "show_hud_help_text 0", "cinematic_show_letterbox 0",
         "cheat_deathless_player 1", "weather 0", "render_particles 0", "render_psystems 0", "render_wsystems 0",
         "render_contrails 0")
STATES = {
    "off": ('prism_set "rt off"',),
    "pathtraced": ('prism_set "rt pathtraced"', 'prism_set "rt_style faithful"',
                   'prism_set "rt_denoiser spatiotemporal"'),
    "raw": ('prism_set "rt pathtraced"', 'prism_set "rt_style faithful"', 'prism_set "rt_denoiser off"'),
    # PRISM v4's Path Traced (M3, docs/prism_pathtraced.md): display.lighting "pathtraced", every CE light off, lit
    # from a bake made before (the run's HALO_BAKE_FOLDER; display.bake "off": nothing baked in the run): its quality
    # and performance profiles, the denoiser off (a diagnostic), PRISM Raster, and Classic's frame as the moves' RT
    # off (the flicker's base and the keep mask's)
    # (Path Traced's states at a fixed exposure (the light's units, CE's scale) and the clipped tone curve, the
    # moving frames and the references alike: the gates compare the frames as drawn, the diffuse light held at white,
    # on one side of the hold, with no exposure that adapts between them; docs/prism_pathtraced.md)
    "v4off": ('prism_set "rt off"', 'prism_set "display.lighting ce"', 'prism_set "debug.gbuffer_view off"'),
    "v4pt": ('prism_set "rt off"', 'prism_set "display.lighting pathtraced"', 'prism_set "display.pt_profile quality"',
             'prism_set "display.denoiser prism"', 'prism_set "display.prism_exposure fixed"',
             'prism_set "display.prism_tone_curve ce"', 'prism_set "debug.gbuffer_view off"'),
    "v4perf": ('prism_set "rt off"', 'prism_set "display.lighting pathtraced"',
               'prism_set "display.pt_profile performance"', 'prism_set "display.denoiser prism"',
               'prism_set "display.prism_exposure fixed"', 'prism_set "display.prism_tone_curve ce"',
               'prism_set "debug.gbuffer_view off"'),
    "v4raw": ('prism_set "rt off"', 'prism_set "display.lighting pathtraced"', 'prism_set "display.pt_profile quality"',
              'prism_set "display.denoiser off"', 'prism_set "display.prism_exposure fixed"',
              'prism_set "display.prism_tone_curve ce"', 'prism_set "debug.gbuffer_view off"'),
    # (diagnostics of the quality profile's moves: the sun's filtered visibility, the history's length)
    "v4sun": ('prism_set "rt off"', 'prism_set "display.lighting pathtraced"', 'prism_set "display.pt_profile quality"',
              'prism_set "display.denoiser prism"', 'prism_set "display.prism_exposure fixed"',
              'prism_set "display.prism_tone_curve ce"', 'prism_set "debug.gbuffer_view pt_sun"'),
    "v4hist": ('prism_set "rt off"', 'prism_set "display.lighting pathtraced"', 'prism_set "display.pt_profile quality"',
               'prism_set "display.denoiser prism"', 'prism_set "display.prism_exposure fixed"',
               'prism_set "display.prism_tone_curve ce"', 'prism_set "debug.gbuffer_view pt_history"'),
    "v4raster": ('prism_set "rt off"', 'prism_set "display.lighting raster"'),
}
# (the states that read the bake: entered from another, the bake is loaded again, BAKE_LOAD_FRAMES held)
BAKE_STATES = ("v4pt", "v4perf", "v4raw", "v4sun", "v4hist", "v4raster")


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


def begin(scene, by_tick=True):
    """the path's start: the harness's rules, the settle and the stop (by_tick False: by the path's frames, for a
    build before the path's tick lines: identity.py's stills)"""
    t = paths.Timeline()
    t.at(*RULES)
    if not scene.get("live"):
        t.at("ai_erase_all")
    if scene.get("bsp") is not None:
        t.at(f"switch_bsp {scene['bsp']}")
    t.at(*scene.get("setup", []))
    if scene.get("live"):
        t.hold(scene["keys"][0], scene["settle"])
        return t
    # (by the game's clock, not the path's frames, which need not each take a step of it: the AI erased again
    # every 60 ticks (a level's script places its AI as its opening ends; AI left to play parts from run to run),
    # and the game stopped at the same tick every run, a second (60 frames at most) before the settle ends)
    stop = (scene["settle"] - 60) // 2
    if not by_tick:
        t.hold(scene["keys"][0], scene["settle"])
        t.at("ai_erase_all", "game_speed 0")
        return t
    for ticks in range(60, stop, 60):
        t.at_tick(ticks, "ai_erase_all")
    t.at_tick(stop, "ai_erase_all", "game_speed 0")
    t.hold(scene["keys"][0], scene["settle"])
    return t


def moves_timeline(scene, states):
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    baked = False
    for state in states:
        t.at(*STATES[state])
        # (a bake's mode entered: the bake read again before the state's settle)
        if state in BAKE_STATES and not baked:
            t.hold(scene["keys"][0], BAKE_LOAD_FRAMES)
        baked = state in BAKE_STATES
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


def refs_v4_timeline(scene, frames, state="v4pt", only=None):
    """M3's references (docs/prism_pathtraced.md): RT off's stills (Classic's frame) at the probe poses first, then
    display.lighting "pathtraced" once (its bake read), and at each probe pose its live frame held, then
    debug.pt_reference in that mode (the same scene, materials and lights unbiased, no bake: summed until its noise is
    under the bound, `frames` at most), its linear file and the frame it holds"""
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    probes = probe_steps(scene)
    t.at(*STATES["v4off"])
    for index, step in enumerate(probes):
        t.hold(poses[step - 1], scene["state_settle"], save=f"refs/p{index}_off.png")
    t.at(*STATES[state])
    t.hold(poses[probes[0] - 1], BAKE_LOAD_FRAMES)
    for index, step in enumerate(probes):
        # (--probe: one probe's reference alone, for a long one in a run of its own)
        if only is not None and index != only:
            continue
        pose = poses[step - 1]
        t.hold(pose, scene["pt_settle"], save=f"refs/p{index}_live.png")
        t.at(f'prism_set "pt_reference_file p{index}.exr"', f'prism_set "pt_reference {frames}"')
        t.hold(pose, frames + scene["reference_margin"], save=f"refs/p{index}_ref.png")
        t.at('prism_set "pt_reference 0"')
    # (the last file written once the GPU is done with its sums: a few frames on)
    t.hold(t.keys[max(t.keys)], 60)
    return t


# M1's equivalence (docs/prism_gates.md): at each probe pose, the camera still, Classic's frame ("ce") and the
# deferred diagnostic's ("ce_deferred"), then its targets alone (debug.gbuffer_view), each after the same hold
EQUIVALENCE = (("ce", ('prism_set "display.lighting ce"',)),
               ("deferred", ('prism_set "display.lighting ce_deferred"', 'prism_set "debug.gbuffer_view off"')),
               ("base", ('prism_set "debug.gbuffer_view base"',)),
               ("light", ('prism_set "debug.gbuffer_view light"',)),
               ("additions", ('prism_set "debug.gbuffer_view additions"',)),
               # (the G-buffer's own views: the base colour, the normal, the roughness, the material source, which
               # gate.py material_source reads, and the instances)
               ("albedo", ('prism_set "debug.gbuffer_view albedo"',)),
               ("normal", ('prism_set "debug.gbuffer_view normal"',)),
               ("roughness", ('prism_set "debug.gbuffer_view roughness"',)),
               ("source", ('prism_set "debug.gbuffer_view material_source"',)),
               ("instance", ('prism_set "debug.gbuffer_view instance"',)))


def equivalence_timeline(scene):
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    for index, step in enumerate(probe_steps(scene)):
        for name, commands in EQUIVALENCE:
            t.at(*commands)
            t.hold(poses[step - 1], scene["state_settle"] // 2, save=f"equivalence/p{index}_{name}.png")
        t.at('prism_set "display.lighting ce"', 'prism_set "debug.gbuffer_view off"')
    return t


# M2's bake (docs/prism_gates.md, docs/prism_bake.md): display.lighting "raster" read from a bake made before
# (the run's HALO_BAKE_FOLDER; display.bake "off": nothing baked in the run), held while the bake loads, then at each
# probe pose PRISM Raster's frame and the views the gate puts the bake's light together from (the light less the
# sun's, the sun's visibility, the base colour, the normal, the instances: gate.py bake), and Classic's frame
BAKE_VIEWS = (("raster", ('prism_set "debug.gbuffer_view off"',)),
              ("light", ('prism_set "debug.gbuffer_view bake_light"',)),
              ("sun", ('prism_set "debug.gbuffer_view bake_sun"',)),
              ("base", ('prism_set "debug.gbuffer_view base"',)),
              ("normal", ('prism_set "debug.gbuffer_view normal"',)),
              ("instance", ('prism_set "debug.gbuffer_view instance"',)))
BAKE_LOAD_FRAMES = 600


def bake_timeline(scene):
    t = begin(scene)
    poses = paths.move_poses(scene["keys"], scene["steps"])
    t.at('prism_set "display.lighting raster"')
    t.hold(scene["keys"][0], BAKE_LOAD_FRAMES)
    for index, step in enumerate(probe_steps(scene)):
        for name, commands in BAKE_VIEWS:
            t.at(*commands)
            t.hold(poses[step - 1], scene["state_settle"] // 2, save=f"bake/p{index}_{name}.png")
    # (Classic last: leaving raster lets the level's scene go, which raster's next frame would build again)
    t.at('prism_set "debug.gbuffer_view off"', 'prism_set "display.lighting ce"')
    for index, step in enumerate(probe_steps(scene)):
        t.hold(poses[step - 1], scene["state_settle"] // 2, save=f"bake/p{index}_ce.png")
    return t


def run(scene, timeline, out, kind, args):
    out.mkdir(parents=True, exist_ok=True)
    for folder in ("moves", "refs", "equivalence", "bake"):
        (out / folder).mkdir(exist_ok=True)
    started = time.time()
    env = dict(kv.split("=", 1) for kv in args.env)
    if scene.get("input"):
        env["HALO_TEST_INPUT"] = scene["input"]
    r = game.Run(args.stage, args.data, out / f"run_{kind}", scene["map"],
                 timeline.text(scene["start"], start_tick=scene["start_tick"]),
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
    if what == "equivalence":
        run(scene, equivalence_timeline(scene), out, "equivalence", args)
        return
    if what == "bake":
        run(scene, bake_timeline(scene), out, "bake", args)
        return
    if what == "all_v4":
        what = "moves"
    if what in ("moves", "all"):
        if scene.get("live"):
            for state in args.states:
                run(scene, moves_timeline(scene, [state]), out, f"moves_{state}", args)
        else:
            run(scene, moves_timeline(scene, args.states), out, "moves", args)
    if what in ("refs", "all"):
        run(scene, refs_timeline(scene, args.reference_frames or scene["reference_frames"]), out, "refs", args)
    if what in ("refs_v4", "all_v4"):
        run(scene, refs_v4_timeline(scene, args.reference_frames or scene.get("reference_frames_v4") or
                                    scene["reference_frames"], args.v4_state, args.probe),
            out, "refs" if args.probe is None else f"refs_p{args.probe}", args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=("moves", "refs", "all", "equivalence", "bake", "refs_v4", "all_v4"))
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
    parser.add_argument("--states", nargs="+", default=["off", "pathtraced", "raw"], choices=list(STATES),
                        help="moves' states (M3's: v4off v4pt v4raw ...)")
    parser.add_argument("--v4-state", default="v4pt", choices=BAKE_STATES, help="refs_v4: the live state at each "
                        "probe before its reference")
    parser.add_argument("--probe", type=int, default=None, help="refs_v4: this probe's reference alone (0, 1, 2)")
    parser.add_argument("--env", action="append", default=[], help="K=V given to the game (diagnostics)")
    args = parser.parse_args()
    scenes = load_scenes()
    for scene_id in args.scenes or list(scenes):
        capture(scenes[scene_id], args.what, args)


if __name__ == "__main__":
    main()
