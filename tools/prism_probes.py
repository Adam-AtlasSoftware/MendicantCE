#!/usr/bin/env python3
"""PRISM v2's local reflections' probes (display.prism_reflections "local"), captured on this machine by the
game's own renderer from the player's own maps: never shipped (port/linux/game/prism_light.c reads them).

    python3 -I tools/prism_probes.py --exe build/linux_x86_64/halo --data <data root> --map a10 \
        --port <console port> --work <folder> [--points "x,y,z;x,y,z"] [--around-start 2.0 4] [--device 0x2482]

A hidden, silent run (SDL's dummy video and audio, no updates, internet play off with empty brokers, relays and
masters files, the gamepads left alone) of the map, stopped at --frame (game_speed 0, the AI erased, the HUD
and the player hidden); at each probe the script's camera (debug_camera_set, 89 degrees across a --width by
--height window) is turned to the six axes, the eight diagonals between them and eight views 45 degrees up and
down, each frame read from the screenshot stream once it has settled. Each probe's directions are folded
octahedrally (as xgpu_spirv.c's prism_probe reads them: o = d.xy / (|x| + |y| + |z|), below the horizon
(1 - |o.yx|) sign(o.xy), uv = o / 2 + 1/2, row 0 at v 0) into a cell of --cell texels, from the view that sees
the direction most squarely; the cells' levels are box-filtered and blurred a little more at each level (the
roughness's), and the atlas written as <data>/prism/probes/<map>.probes (prism_light.c's format). The probes go at --points, else at the player's eye
at the start and --around-start's ring about it (radius in world units, count). The console port must be free;
the run is ended by its own PID.
"""
import argparse
import math
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

MAGIC = 0x53425250
VERSION = 1
FOV = 89.0


def listener_pid(port):
    """the pid whose socket listens on the TCP port, or None"""
    inodes = set()
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            rows = open(table).read().splitlines()[1:]
        except OSError:
            continue
        for row in rows:
            fields = row.split()
            if int(fields[1].split(":")[1], 16) == port and fields[3] == "0A":
                inodes.add(fields[9])
    if not inodes:
        return None
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            for fd in os.listdir("/proc/%s/fd" % pid):
                link = os.readlink("/proc/%s/fd/%s" % (pid, fd))
                if link.startswith("socket:[") and link[8:-1] in inodes:
                    return int(pid)
        except OSError:
            continue
    return None


class Game:
    """the hidden run: its console (checked to be its own before each line) and its frames"""

    def __init__(self, exe, data, work, scenario, port, width, height, device, extra_env):
        if listener_pid(port) is not None:
            raise SystemExit("console port %d is taken" % port)
        self.port, self.width, self.height = port, width, height
        work.mkdir(parents=True, exist_ok=True)
        (work / "saves").mkdir(exist_ok=True)
        if not (work / "maps").exists():
            (work / "maps").symlink_to((Path(data) / "maps").resolve())
        for name in ("brokers.txt", "relays.txt", "masters.txt"):
            (work / name).write_text("")
        (work / "init.txt").write_text("map_name %s\n" % scenario)
        self.fifo = work / "frames.fifo"
        if self.fifo.exists():
            self.fifo.unlink()
        os.mkfifo(self.fifo)
        env = {"HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": os.environ.get("XDG_RUNTIME_DIR", ""),
               "HALO_DATA_ROOT": str(work), "HALO_SAVE_ROOT": str(work / "saves"),
               "HALO_HIDDEN_WINDOW": "1", "HALO_FULLSCREEN": "0", "HALO_DISPLAY_MODE": "windowed",
               "HALO_WINDOW_SIZE": "%dx%d" % (width, height), "SDL_VIDEO_DRIVER": "dummy",
               "HALO_NO_AUDIO": "1", "SDL_AUDIODRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy", "HALO_VOICE_CHAT": "off",
               "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0",
               "SDL_JOYSTICK_HIDAPI": "0", "HALO_UPDATE_AUTO": "false", "HALO_NO_CLIPBOARD": "1",
               "HALO_NET_ONLINE": "false", "HALO_NET_PUBLIC_LOBBY": "false", "HALO_NET_MAP_SHARE": "false",
               "HALO_NET_MAP_SEED": "false", "HALO_NET_MAP_DOWNLOAD": "false", "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
               "HALO_NET_BROKERS_FILE": str(work / "brokers.txt"), "HALO_NET_RELAYS_FILE": str(work / "relays.txt"),
               "HALO_NET_MASTERS_FILE": str(work / "masters.txt"), "HALO_DISCORD_APPLICATION": "",
               "HALO_CRASH_REPORTS": "no", "HALO_TELNET_CONSOLE": "1", "HALO_TELNET_CONSOLE_PORT": str(port),
               "HALO_MAP_CACHE_MEGABYTES": "730",
               "HALO_SCREENSHOT_EVERY": "1", "HALO_SCREENSHOT_STREAM": str(self.fifo), "HALO_SKIP_CINEMATICS": "1",
               "HALO_INTERPOLATION": "false", "HALO_MAX_FPS": "-1", "HALO_FRAME_TIME": str(1.0 / 30.0),
               "HALO_PRISM_REFLECTIONS": "stock", "HALO_PRISM_SPECULAR": "off", "HALO_PRISM_SHADOWS": "stock"}
        if device:
            env["VK_LOADER_DEVICE_ID_FILTER"] = device
        env.update(extra_env)
        self.frame, self.count = None, 0
        self.cond = threading.Condition()
        self.stdout = open(work / "stdout.txt", "w")
        self.process = subprocess.Popen([str(Path(exe).resolve())], cwd=work, env=env, stdout=self.stdout,
                                        stderr=subprocess.STDOUT)
        self.sock = None
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        size = self.width * self.height * 4
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
                with self.cond:
                    self.frame = bytes(buffer)
                    self.count += 1
                    self.cond.notify_all()

    def connect(self, timeout=300):
        end = time.time() + timeout
        while time.time() < end:
            if self.process.poll() is not None:
                raise RuntimeError("the game ended (%s)" % self.process.returncode)
            if listener_pid(self.port) == self.process.pid:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                self.sock.settimeout(0.05)
                return
            time.sleep(0.5)
        raise RuntimeError("no console on %d" % self.port)

    def wait_frames(self, n, timeout=120):
        with self.cond:
            target = self.count + n
            end = time.time() + timeout
            while self.count < target:
                if self.process.poll() is not None:
                    raise RuntimeError("the game ended (%s)" % self.process.returncode)
                if not self.cond.wait(1.0) and time.time() > end:
                    raise RuntimeError("no frames")
            return np.frombuffer(self.frame, np.uint8).reshape(self.height, self.width, 4)[..., [2, 1, 0]].copy()

    def command(self, line):
        if listener_pid(self.port) != self.process.pid:
            raise RuntimeError("console port %d is not this run's" % self.port)
        self.sock.sendall(line.encode() + b"\r\n")
        self.wait_frames((len(line) + 2) // 32 + 3)
        try:
            return self.sock.recv(65536).decode(errors="replace")
        except (socket.timeout, BlockingIOError):
            return ""

    def settled(self, settle=8):
        prev = self.wait_frames(settle)
        same = 0
        for _ in range(300):
            cur = self.wait_frames(1)
            same = same + 1 if np.array_equal(cur, prev) else 0
            if same >= 2:
                return cur
            prev = cur
        return cur

    def close(self):
        if self.sock:
            self.sock.close()
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(15)
        self.stdout.close()


def basis(yaw, pitch):
    """camera_pose.c's forward, up and right for a yaw and pitch (degrees, no roll)"""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    forward = np.array([cy * cp, sy * cp, sp])
    up = np.array([-cy * sp, -sy * sp, cp])
    return forward, up, np.cross(forward, up)


VIEWS = [(0, 0), (90, 0), (180, 0), (270, 0), (0, 90), (0, -90)] + \
    [(45 + 90 * k, p) for k in range(4) for p in (35.26, -35.26)] + \
    [(90 * k, p) for k in range(4) for p in (45.0, -45.0)]


def octahedral_directions(cell):
    """each texel's unit direction (cell x cell x 3), as prism_probe unfolds it"""
    uv = (np.arange(cell) + 0.5) / cell
    u, v = np.meshgrid(uv, uv)
    ox, oy = u * 2.0 - 1.0, v * 2.0 - 1.0
    z = 1.0 - np.abs(ox) - np.abs(oy)
    below = z < 0
    fx = np.where(below, (1.0 - np.abs(oy)) * np.where(ox >= 0, 1.0, -1.0), ox)
    fy = np.where(below, (1.0 - np.abs(ox)) * np.where(oy >= 0, 1.0, -1.0), oy)
    d = np.stack([fx, fy, z], axis=-1)
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def fold(images, cell):
    """a probe's views (VIEWS' order) folded into its octahedral cell (float RGB)"""
    d = octahedral_directions(cell).reshape(-1, 3)
    height, width = images[0].shape[:2]
    t = math.tan(math.radians(FOV) / 2.0)
    # (the field of view across; up and down by the frame's shape)
    t_up = t * height / width
    best = np.full(len(d), -1.0)
    out = np.zeros((len(d), 3))
    for (yaw, pitch), image in zip(VIEWS, images):
        forward, up, right = basis(yaw, pitch)
        along = d @ forward
        x = (d @ right) / np.maximum(along, 1e-6) / t
        y = (d @ up) / np.maximum(along, 1e-6) / t_up
        seen = (along > 0.2) & (np.abs(x) <= 0.995) & (np.abs(y) <= 0.995) & (along > best)
        px = (x + 1.0) * 0.5 * width - 0.5
        py = (1.0 - y) * 0.5 * height - 0.5
        x0 = np.clip(np.floor(px).astype(int), 0, width - 2)
        y0 = np.clip(np.floor(py).astype(int), 0, height - 2)
        fx = np.clip(px - x0, 0, 1)[:, None]
        fy = np.clip(py - y0, 0, 1)[:, None]
        img = image.astype(np.float64)
        sample = ((img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx) * (1 - fy) +
                  (img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx) * fy)
        out[seen] = sample[seen]
        best[seen] = along[seen]
    missing = int((best < 0).sum())
    return out.reshape(cell, cell, 3), missing


def levels_of(cell_image, count):
    """the cell's levels: each a 2x2 box of the one before, blurred once more by a 3x3 box (the roughness's)"""
    levels = [cell_image]
    for _ in range(1, count):
        prev = levels[-1]
        half = (prev[0::2, 0::2] + prev[1::2, 0::2] + prev[0::2, 1::2] + prev[1::2, 1::2]) / 4.0
        if half.shape[0] >= 4:
            padded = np.pad(half, ((1, 1), (1, 1), (0, 0)), mode="edge")
            half = sum(padded[i:i + half.shape[0], j:j + half.shape[1]] for i in range(3) for j in range(3)) / 9.0
        levels.append(half)
    return levels


def write_probes(path, places, cells_images, cell):
    count = len(places)
    side_cells = int(math.ceil(math.sqrt(count)))
    level_count = int(math.log2(cell)) - 1
    per_probe = [levels_of(image, level_count) for image in cells_images]
    with open(path, "wb") as f:
        f.write(struct.pack("<6I", MAGIC, VERSION, count, cell, side_cells, level_count))
        for place in places:
            f.write(struct.pack("<4f", place[0], place[1], place[2], 1.0))
        for level in range(level_count):
            texels = cell >> level
            atlas = np.zeros((side_cells * texels, side_cells * texels, 4), np.uint8)
            atlas[..., 3] = 255
            for index, levels in enumerate(per_probe):
                row, column = divmod(index, side_cells)
                atlas[row * texels:(row + 1) * texels, column * texels:(column + 1) * texels, :3] = \
                    np.clip(np.rint(levels[level]), 0, 255).astype(np.uint8)
            f.write(atlas.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--data", required=True, help="the data root (its maps folder; the probes are written there)")
    ap.add_argument("--map", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--points", default="")
    ap.add_argument("--around-start", type=float, nargs=2, default=None, metavar=("RADIUS", "COUNT"))
    ap.add_argument("--frame", type=int, default=300)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--cell", type=int, default=128)
    ap.add_argument("--device", default="")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--keep-views", action="store_true", help="the views saved as PNGs in --work")
    a = ap.parse_args()
    if a.cell & (a.cell - 1) or not 16 <= a.cell <= 512:
        raise SystemExit("--cell: a power of two, 16 to 512")
    extra = dict(kv.split("=", 1) for kv in a.env)
    work = a.work.resolve()
    game = Game(a.exe, a.data, work / "run", a.map, a.port, a.width, a.height, a.device, extra)
    print("pid", game.process.pid, flush=True)
    places, cells = [], []
    try:
        game.connect()
        while game.count < a.frame:
            game.wait_frames(1, timeout=600)
        for line in ("game_speed 0", "ai_erase_all", "terminal_render 0", "show_hud 0", "show_hud_help_text 0",
                     "cinematic_show_letterbox 0", "object_hide (unit (list_get (players) 0)) true"):
            game.command(line)
        game.command("debug_camera_save")
        rows = [list(map(float, r.split())) for r in (work / "run" / "camera.txt").read_text().splitlines() if r.strip()]
        eye = rows[0][:3]
        if a.points:
            places = [tuple(float(v) for v in p.split(",")) for p in a.points.split(";") if p.strip()]
        else:
            places = [tuple(eye)]
            if a.around_start:
                radius, count = a.around_start[0], int(a.around_start[1])
                places += [(eye[0] + radius * math.cos(2 * math.pi * k / count),
                            eye[1] + radius * math.sin(2 * math.pi * k / count), eye[2]) for k in range(count)]
        for index, place in enumerate(places):
            images = []
            for yaw, pitch in VIEWS:
                game.command('debug_camera_set "%.4f %.4f %.4f %.3f %.3f 0 %g"' % (place[0], place[1], place[2], yaw,
                                                                                   pitch, FOV))
                image = game.settled()
                images.append(image)
                if a.keep_views:
                    from PIL import Image
                    Image.fromarray(image).save(work / ("probe%02d_%03d_%+03d.png" % (index, yaw, int(pitch))))
            cell_image, missing = fold(images, a.cell)
            print("probe %d at %.3f %.3f %.3f: %d texels unseen" % (index, place[0], place[1], place[2], missing),
                  flush=True)
            cells.append(cell_image)
        game.command('debug_camera_set "off"')
    finally:
        game.close()
    folder = Path(a.data) / "prism" / "probes"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / ("%s.probes" % a.map)
    write_probes(path, places, cells, a.cell)
    print("wrote", path, len(places), "probes")


if __name__ == "__main__":
    main()
