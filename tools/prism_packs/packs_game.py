"""The player's own game, run hidden, silent and offline by the pack tools: to have it write its models as it
draws them (debug.prism_mesh_dump), the input of the mesh-store packs.

A run has a folder of its own in the work folder: a data root whose maps/ is a link to the player's maps
folder, a trigger mod (the mesh dump is written as the mods are applied, which needs one mod), the settings
file, the saves, and empty brokers, relays and masters files: the player's own settings, saves and mods are
not touched, nothing is shown or heard, and the game asks nothing of the network.
"""

import os
import subprocess
import time
from pathlib import Path


def link_folder(link, target):
    """link -> target, a symbolic link (a directory junction on Windows, which needs no rights)"""
    link, target = Path(link), Path(target).resolve()
    if link.is_symlink() or link.exists():
        return
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


def hidden_environment(run, data, extra=None):
    """the environment of a hidden, silent, offline run of the game in the folder run, its data root data"""
    env = dict(os.environ)
    for name in list(env):
        if name.startswith("HALO_"):
            del env[name]
    env.update({
        "HALO_DATA_ROOT": str(data), "HALO_SAVE_ROOT": str(run / "save"), "HALO_CONFIG_FILE": str(run / "config.toml"),
        "HALO_HIDDEN_WINDOW": "1", "HALO_FULLSCREEN": "0", "HALO_DISPLAY_MODE": "windowed",
        "HALO_NO_AUDIO": "1", "SDL_AUDIODRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0",
        "SDL_JOYSTICK_HIDAPI": "0",
        "HALO_VOICE_CHAT": "off", "HALO_UPDATE_AUTO": "false", "HALO_NO_CLIPBOARD": "1",
        "HALO_NET_ONLINE": "false", "HALO_NET_PUBLIC_LOBBY": "false", "HALO_NET_MAP_SHARE": "false",
        "HALO_NET_MAP_SEED": "false", "HALO_NET_MAP_DOWNLOAD": "false", "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
        "HALO_NET_BROKERS_FILE": str(data / "brokers.txt"), "HALO_NET_RELAYS_FILE": str(data / "relays.txt"),
        "HALO_NET_MASTERS_FILE": str(data / "masters.txt"),
        "HALO_DISCORD_APPLICATION": "", "HALO_CRASH_REPORTS": "no",
    })
    if os.name != "nt":
        env["SDL_VIDEO_DRIVER"] = "dummy"
    env.update(extra or {})
    return env


def prepare(run, maps):
    """a run's folder: its data root (maps linked, the trigger mod, empty network lists) and saves"""
    run = Path(run)
    data = run / "data"
    (data / "mods" / "prism_packs_trigger").mkdir(parents=True, exist_ok=True)
    (run / "save").mkdir(parents=True, exist_ok=True)
    link_folder(data / "maps", maps)
    (data / "mods" / "prism_packs_trigger" / "mod.toml").write_text(
        'name = "PRISM pack tools: model dump"\nid = "prism_packs_trigger"\nversion = "1"\n'
        'description = "Empty: the pack tools\' hidden run has the mods applied, which writes the models."\n'
        'format = 1\n')
    for name in ("brokers.txt", "relays.txt", "masters.txt"):
        (data / name).write_text("")
    return data


def dump_models(exe, maps, map_name, run, seconds=120, settle=6.0):
    """the game's models of map_name as it draws them (debug.prism_mesh_dump "*"), in
    <run>/data/prism_dump/<map>/; returns that folder. The run ends when the dump has stopped growing (the
    game is ended by its own process id), or at most after seconds."""
    run = Path(run)
    data = prepare(run, maps)
    (data / "init.txt").write_text(f"map_name {map_name}\n")
    out = data / "prism_dump" / map_name
    log = open(run / "game.log", "w")
    env = hidden_environment(run, data, {"HALO_PRISM_MESH_DUMP": "*", "HALO_EXIT_AFTER": str(seconds)})
    process = subprocess.Popen([str(exe)], cwd=str(data), env=env, stdout=log, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL)
    last, last_change = -1, time.time()
    try:
        while process.poll() is None:
            time.sleep(1.0)
            count = len(list(out.glob("*.json"))) if out.is_dir() else 0
            if count != last:
                last, last_change = count, time.time()
            elif count and time.time() - last_change > settle:
                break
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        log.close()
    if not out.is_dir() or not any(out.glob("*.json")):
        raise RuntimeError(f"the game wrote no models of {map_name} (its log: {run / 'game.log'}, "
                           f"{data / 'debug.txt'})")
    return out
