"""One hidden, silent, isolated run of a staged OpenH1 tree for the quality gates, driven by a camera path
(paths.py) and read through a pipe of its frames (debug.screenshot_stream): no window (SDL's dummy video driver,
debug.hidden_window), no sound (HALO_NO_AUDIO, SDL's dummy audio), no controllers, nothing online (the run's own
empty brokers, relays and masters files), its own data, save and config folders, the clock fixed (a tick a frame,
debug.fixed_step) and interpolation off, so that a path's frames are the same in every run.

    run = Run(stage, data, work, "b30", timeline.text(), timeline.saves, config=..., env={...})
    run.wait(timeout)   # the frames saved (by the path's frame) written as PNG; raises if the game failed

A game that exits before the path's saved frames have all come, or a pipe that gives fewer, is a failure. Before every run the kit's game is checked for (kit_running): a run never starts
while the person this works for is playing.
"""
import ctypes
import hashlib
import os
import shutil
import signal
import subprocess
import threading
import time
from pathlib import Path

import numpy as np
from PIL import Image

# the kit (the game the person this works for plays): never run while it runs, never touched
KIT = Path(os.environ.get("PRISM_GATE_KIT", "/home/adam/Dev/repos/ce-reimagined/testkit/openh1-prism-v2"))


def kit_running():
    """the pid of a process whose executable is in the kit, or None"""
    kit = str(KIT.resolve()) + os.sep
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        if exe.startswith(kit):
            return int(pid)
    return None


def _die_with_parent():
    ctypes.CDLL("libc.so.6").prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG


def isolated_environment(work, data):
    """the variables every gate run is given (and only these, with HOME, PATH and XDG_RUNTIME_DIR)"""
    return {
        "HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": os.environ.get("XDG_RUNTIME_DIR", ""),
        "HALO_DATA_ROOT": str(data), "HALO_SAVE_ROOT": str(work / "saves"),
        "HALO_HIDDEN_WINDOW": "1", "HALO_FULLSCREEN": "0", "HALO_DISPLAY_MODE": "windowed",
        "SDL_VIDEO_DRIVER": "dummy", "HALO_NO_AUDIO": "1", "SDL_AUDIODRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0",
        "SDL_JOYSTICK_HIDAPI": "0", "HALO_VOICE_CHAT": "off",
        "HALO_UPDATE_AUTO": "false", "HALO_NO_CLIPBOARD": "1", "HALO_NET_ONLINE": "false",
        "HALO_NET_PUBLIC_LOBBY": "false", "HALO_NET_MAP_SHARE": "false", "HALO_NET_MAP_SEED": "false",
        "HALO_NET_MAP_DOWNLOAD": "false", "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
        "HALO_NET_BROKERS_FILE": str(data / "brokers.txt"), "HALO_NET_RELAYS_FILE": str(data / "relays.txt"),
        "HALO_NET_MASTERS_FILE": str(data / "masters.txt"), "HALO_NET_BROADCAST": "127.255.255.255",
        "HALO_NET_ADDRESS": "127.0.0.77", "HALO_DISCORD_APPLICATION": "", "HALO_CRASH_REPORTS": "no",
        "HALO_SKIP_CINEMATICS": "1", "HALO_FIXED_STEP": "1", "HALO_INTERPOLATION": "false", "HALO_MAX_FPS": "-1",
        "HALO_MAP_CACHE_MEGABYTES": "730", "HALO_PRISM_OVERLAY_VIEW": "0", "HALO_PANINI": "0",
    }


def stage_data(data_source, data, links=("prism_world", "prism_lightmaps", "world_sdf", "custom_maps")):
    """a run's own data folder: the maps and the PRISM folders linked from data_source (read only), the mods each
    linked with a copy of their load order (the game may rewrite it), the network's files empty"""
    data_source = Path(data_source)
    data.mkdir(parents=True, exist_ok=True)
    (data / "maps").symlink_to((data_source / "maps").resolve())
    for name in links:
        if (data_source / name).exists():
            (data / name).symlink_to((data_source / name).resolve())
    mods = data_source / "mods"
    if mods.exists():
        (data / "mods").mkdir()
        for item in sorted(mods.resolve().iterdir()):
            if item.name == "load_order.toml":
                shutil.copy(item, data / "mods" / item.name)
            elif item.is_dir():
                (data / "mods" / item.name).symlink_to(item)
    for name in ("brokers.txt", "relays.txt", "masters.txt"):
        (data / name).write_text("")


class Run:
    """the staged tree copied into work/stage (its config.toml, if given, put there), a data folder made, the game
    started on a map with a camera path; the wanted frames of the stream saved as they come"""

    def __init__(self, stage, data_source, work, game_map, path_text, wanted, config=None, env=None, width=1920,
                 height=1080, device=None, keep_every=None):
        pid = kit_running()
        if pid:
            raise SystemExit(f"stopped: the kit's game is running (pid {pid}); no game is run while it is")
        self.work = Path(work).resolve()
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)
        self.stage = self.work / "stage"
        shutil.copytree(stage, self.stage, symlinks=True)
        if config:
            shutil.copy(config, self.stage / "config.toml")
        self.data = self.work / "data"
        stage_data(data_source, self.data)
        (self.work / "saves").mkdir()
        (self.data / "init.txt").write_text(f"map_name {game_map}\n")
        (self.work / "path.txt").write_text(path_text)
        self.width, self.height = width, height
        # (the stream has the path's saved frames alone, in order: its n-th is the n-th of them)
        self.wanted = {index: wanted[frame] for index, frame in enumerate(sorted(wanted))}
        self.saved = {}
        self.hashes = {}
        self.count = 0
        self.error = None
        self.fifo = self.work / "frames.fifo"
        os.mkfifo(self.fifo)
        e = isolated_environment(self.work, self.data)
        # (the path's frames alone go to the stream: debug.camera_path saves each of its own, and
        # screenshot_every 0 none other, so that the stream's n-th frame is the path's)
        e.update({"HALO_WINDOW_SIZE": f"{width}x{height}", "HALO_CAMERA_PATH": str(self.work / "path.txt"),
                  "HALO_SCREENSHOT_EVERY": "0", "HALO_SCREENSHOT_STREAM": str(self.fifo)})
        if device:
            e["VK_LOADER_DEVICE_ID_FILTER"] = device
        e.update(env or {})
        self.environment = e
        self.stdout = open(self.work / "stdout.txt", "w")
        self.process = subprocess.Popen([str(self.stage / "bin" / "openh1")], cwd=self.data, env=e,
                                        stdout=self.stdout, stderr=subprocess.STDOUT, preexec_fn=_die_with_parent)
        (self.work / "pid").write_text(str(self.process.pid))
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        size = self.width * self.height * 4
        try:
            with open(self.fifo, "rb", buffering=0) as stream:
                buffer = bytearray(size)
                view = memoryview(buffer)
                while True:
                    got = 0
                    while got < size:
                        n = stream.readinto(view[got:])
                        if not n:
                            return
                        got += n
                    index = self.count
                    self.count += 1
                    if index in self.wanted:
                        rgb = np.frombuffer(buffer, np.uint8).reshape(self.height, self.width, 4)[..., [2, 1, 0]]
                        name = self.wanted[index]
                        Image.fromarray(np.ascontiguousarray(rgb)).save(self.work.parent / name)
                        self.hashes[name] = hashlib.sha256(rgb.tobytes()).hexdigest()
                        self.saved[index] = name
        except Exception as error:  # noqa: BLE001 (reported by wait)
            self.error = error

    def wait(self, timeout):
        """the run waited for until the path's saved frames have come (then the game is stopped) or it exits; the
        saved frames' hashes by name"""
        frames = len(self.wanted)
        end = time.time() + timeout
        try:
            while self.count < frames:
                if self.error:
                    raise RuntimeError(f"the frames' reader failed: {self.error}")
                if self.process.poll() is not None:
                    raise RuntimeError(f"the game exited ({self.process.returncode}) after {self.count} of {frames} "
                                       f"frames: {self.work / 'stdout.txt'}")
                if time.time() > end:
                    raise RuntimeError(f"timed out after {self.count} of {frames} frames")
                time.sleep(0.2)
            missing = sorted(set(self.wanted) - set(self.saved))
            if missing:
                time.sleep(1.0)
                missing = sorted(set(self.wanted) - set(self.saved))
            if missing:
                raise RuntimeError(f"frames not saved: {missing[:8]}")
            return dict(self.hashes)
        finally:
            self.close()

    def close(self):
        if self.process.poll() is None:
            os.kill(self.process.pid, signal.SIGKILL)
            try:
                self.process.wait(15)
            except subprocess.TimeoutExpired:
                pass
        self.stdout.close()
        shutil.rmtree(self.stage, ignore_errors=True)
        try:
            self.fifo.unlink()
        except OSError:
            pass

    def log(self):
        return (self.work / "stdout.txt").read_text(errors="replace")
