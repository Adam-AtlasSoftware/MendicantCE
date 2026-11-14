#!/usr/bin/env python3
"""PRISM's baker groundwork on the player's own maps, offline: each structure bsp's reachable space, its surfaces'
importance and its atlas at the `max` tier (port/linux/vulkan/prism_bake_*.c, run by tools/prism_bake/bake_probe.c),
with top-down pictures to check them by.

    python3 -I tools/prism_bake/bake_scene.py --data <data root> --out <folder> b30 a10 c40 ratrace bloodgulch
        [--bsp <index>]... [--density 16 --density 32] [--flight auto|on|off]

Each bsp is also seeded by the floors its map's other bsps walked (a bsp switch carries the player across), in rounds
until no walk grows. For each map and bsp, <out>/<map>/<bsp index>/ gets the scene the probe reads (scene.bin), what it writes
(voxels.bin, flight.bin, tiers.bin, unreached.bin, summary.json) and three pictures, each the bsp seen from above,
its floors shaded by height:
    reach.png      the walk's floors (green, brighter higher), the camera volume beyond them (blue), flight (violet),
                   the walk seeds (red; magenta crosses for those outside this bsp) and the views (yellow)
    tiers.png      each floor and wall by its importance tier: 0 green, 1 yellow, 2 red
    unreached.png  the floors (surfaces within the player's slope) the walk did not reach, red over the walk
<out>/summary.json collects every bsp's numbers. The maps are read, never written; nothing of them is kept but
these numbers and pictures, which stay local (game data is never committed).
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
import ce_map  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
VULKAN = ROOT / "port/linux/vulkan"
WALK_KINDS = {"bsp_transfer", "player_start", "netgame_flag", "netgame_equipment", "vehicle", "biped", "weapon", "equipment",
              "ai_start", "encounter_player_start"}
VIEW_KINDS = {"ai_firing", "cutscene_flag", "cutscene_camera"}
SEED_WALK, SEED_VIEW = 0, 1
FLAG_SEED, FLAG_WALK, FLAG_BODY, FLAG_CAMERA, FLAG_VIEW = 1, 2, 4, 8, 16
CELL = 0.25
FLIGHT_CELL = 2.0


def build_probe(out):
    """the probe, compiled once into out"""
    exe = out / "bake_probe"
    sources = [Path(__file__).resolve().parent / "bake_probe.c"] + sorted(VULKAN.glob("prism_bake_*.c"))
    if not exe.exists() or any(s.stat().st_mtime > exe.stat().st_mtime for s in sources):
        cc = shutil.which("cc") or shutil.which("clang")
        subprocess.run([cc, "-std=gnu89", "-O2", "-Wall", f"-I{VULKAN}", "-o", str(exe)] + [str(s) for s in sources] +
                       ["-lm"], check=True)
    return exe


def oriented(triangle):
    """the triangle's corners and UVs, wound to face its vertices' normals"""
    surface, page, a, b, c, ua, ub, uc, normal = triangle
    e1 = np.subtract(b, a)
    e2 = np.subtract(c, a)
    if np.dot(np.cross(e1, e2), normal) < 0:
        b, c, ub, uc = c, b, uc, ub
    return surface, page, a, b, c, ua, ub, uc


def write_scene(path, bsp, seeds, flight):
    out = bytearray()
    out += b"PBSC" + struct.pack("<i", 2)
    out += struct.pack("<6f", *bsp.world_bounds)
    out += struct.pack("<i", 1 if flight else 0)
    out += struct.pack("<i", len(bsp.water_planes))
    for plane in bsp.water_planes:
        out += struct.pack("<4f", *plane)
    out += struct.pack("<i", bsp.node_count) + bsp.nodes
    out += struct.pack("<i", bsp.plane_count) + bsp.planes
    out += struct.pack("<i", bsp.collision_leaf_count)
    out += struct.pack("<i", bsp.leaf_count)
    for index in range(bsp.leaf_count):
        cluster, count, first = struct.unpack_from("<hhi", bsp.leaves, index * 16 + 8)
        out += struct.pack("<hhi", cluster, count, first)
    out += struct.pack("<i", bsp.surface_reference_count)
    out += np.frombuffer(bsp.surface_references, dtype="<i4").reshape(-1, 2)[:, 0].astype("<i4").tobytes()
    words = (bsp.cluster_count + 31) // 32
    pvs = bsp.pvs if len(bsp.pvs) >= bsp.cluster_count * words * 4 else b""
    out += struct.pack("<ii", bsp.cluster_count, words if pvs else 0)
    out += pvs[:bsp.cluster_count * words * 4]
    out += struct.pack("<i", len(seeds))
    for kind, x, y, z in seeds:
        out += struct.pack("<i3f", SEED_VIEW if kind in VIEW_KINDS else SEED_WALK, x, y, z)
    out += struct.pack("<i", len(bsp.pages))
    for width, height in bsp.pages:
        out += struct.pack("<ii", width, height)
    triangles = [oriented(t) for t in bsp.triangles]
    out += struct.pack("<i", len(triangles))
    for surface, page, a, b, c, ua, ub, uc in triangles:
        out += struct.pack("<ii9f6f", surface, page, *a, *b, *c, *ua, *ub, *uc)
    Path(path).write_bytes(bytes(out))
    return triangles


def read_voxels(path):
    data = Path(path).read_bytes()
    if not data:
        return np.zeros((0, 3), np.int32), np.zeros(0, np.uint8)
    records = np.frombuffer(data, dtype=np.dtype([("xyz", "<i4", 3), ("flags", "u1")]))
    return records["xyz"].astype(np.int64), records["flags"]


class View:
    """a top-down picture of a box of the world"""

    def __init__(self, x0, x1, y0, y1, limit=2400):
        self.x0, self.y0 = x0, y0
        self.scale = min(limit / max(x1 - x0, 1e-3), limit / max(y1 - y0, 1e-3), 16.0)
        self.width = max(1, int((x1 - x0) * self.scale))
        self.height = max(1, int((y1 - y0) * self.scale))

    def point(self, x, y):
        return ((x - self.x0) * self.scale, self.height - (y - self.y0) * self.scale)


def floors_image(view, triangles, colours, z_range):
    """the triangles facing up (painted lowest first), each its colour shaded by height, on black"""
    image = Image.new("RGB", (view.width, view.height), (12, 12, 16))
    draw = ImageDraw.Draw(image)
    order = []
    for index, (_, _, a, b, c, _, _, _) in enumerate(triangles):
        n = np.cross(np.subtract(b, a), np.subtract(c, a))
        length = np.linalg.norm(n)
        if length <= 0:
            continue
        up = n[2] / length
        order.append((max(a[2], b[2], c[2]), index, up))
    order.sort()
    z0, z1 = z_range
    for z, index, up in order:
        if up < 0.2:
            continue
        _, _, a, b, c, _, _, _ = triangles[index]
        shade = 0.35 + 0.65 * min(1.0, max(0.0, (z - z0) / max(z1 - z0, 1e-3)))
        colour = tuple(int(channel * shade) for channel in colours(index))
        draw.polygon([view.point(a[0], a[1]), view.point(b[0], b[1]), view.point(c[0], c[1])], fill=colour)
    return image


def overlay_cells(image, view, xyz, cell, colour, alpha, z_range=None):
    """cells (their columns) blended over the image"""
    if not len(xyz):
        return image
    layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    columns = {}
    for x, y, z in xyz:
        key = (int(x), int(y))
        if key not in columns or z > columns[key]:
            columns[key] = z
    for (x, y), z in columns.items():
        c = colour
        if z_range:
            t = min(1.0, max(0.0, (z * cell - z_range[0]) / max(z_range[1] - z_range[0], 1e-3)))
            c = tuple(int(channel * (0.45 + 0.55 * t)) for channel in colour)
        p0 = view.point(x * cell, (y + 1) * cell)
        p1 = view.point((x + 1) * cell, y * cell)
        draw.rectangle([p0, (max(p0[0], p1[0] - 1), max(p0[1], p1[1] - 1))], fill=c + (alpha,))
    return Image.alpha_composite(image.convert("RGBA"), layer).convert("RGB")


def walked_floors(folder):
    """the walk's floors, one a world unit: (x, y, z) a little above each"""
    xyz, flags = read_voxels(folder / "voxels.bin")
    floors = {}
    for x, y, z in xyz[(flags & FLAG_WALK) != 0]:
        floors.setdefault((int(x) // 4, int(y) // 4, int(z) // 4), ((x + 0.5) * CELL, (y + 0.5) * CELL, z * CELL + 0.1))
    return list(floors.values())


def run_bsp(probe, out, game_map, index, densities, flight_mode, transfers=()):
    bsp = game_map.bsp(index)
    folder = out / game_map.name / str(index)
    folder.mkdir(parents=True, exist_ok=True)
    # (the scenario's seeds, and the floors the other bsps walked: a bsp switch carries the player across)
    seeds = game_map.seeds() + [("bsp_transfer", x, y, z) for x, y, z in transfers]
    palette = game_map.vehicle_palette()
    flying = [name for name, kind in palette if kind in ce_map.FLYING_VEHICLE_TYPES]
    placed_flying = game_map.placed_flying_vehicles()
    flight = bool(placed_flying) if flight_mode == "auto" else flight_mode == "on"
    triangles = write_scene(folder / "scene.bin", bsp, seeds, flight)
    command = [str(probe), str(folder / "scene.bin"), str(folder), "--flight", "1" if flight else "0"]
    for density in densities:
        command += ["--density", str(density)]
    subprocess.run(command, check=True)
    summary = json.loads((folder / "summary.json").read_text())
    summary["map"] = game_map.name
    summary["bsp"] = index
    summary["bsp_name"] = bsp.name
    summary["flying_vehicles"] = flying
    summary["placed_flying_vehicles"] = [
        {"name": name, "position": [round(x, 2), round(y, 2), round(z, 2)]}
        for name, _, (x, y, z) in placed_flying]
    summary["seed_kinds"] = {kind: sum(1 for s in seeds if s[0] == kind) for kind in sorted({s[0] for s in seeds})}

    # the pictures
    xyz, flags = read_voxels(folder / "voxels.bin")
    fxyz, _ = read_voxels(folder / "flight.bin")
    tiers = np.frombuffer((folder / "tiers.bin").read_bytes(), dtype=np.uint8)
    unreached = np.frombuffer((folder / "unreached.bin").read_bytes(), dtype=np.dtype([("t", "<i4"), ("a", "<f4")]))
    walk = xyz[(flags & FLAG_WALK) != 0]
    camera = xyz[(flags & (FLAG_CAMERA | FLAG_BODY)) != 0]
    inside = [s for s in seeds if s[0] in WALK_KINDS]
    if len(walk):
        lo = walk.min(axis=0) * CELL - 15.0
        hi = (walk.max(axis=0) + 1) * CELL + 15.0
    else:
        corners = np.array([p for t in triangles for p in t[2:5]])
        lo, hi = corners.min(axis=0), corners.max(axis=0)
    z_range = (float(lo[2]) + 15.0, float(hi[2]) - 15.0) if len(walk) else (float(lo[2]), float(hi[2]))
    view = View(float(lo[0]), float(hi[0]), float(lo[1]), float(hi[1]))
    grey = floors_image(view, triangles, lambda i: (200, 200, 200), z_range)

    reach = overlay_cells(grey, view, fxyz, FLIGHT_CELL, (150, 80, 220), 70)
    reach = overlay_cells(reach, view, camera, CELL, (60, 140, 255), 90)
    reach = overlay_cells(reach, view, walk, CELL, (60, 255, 90), 170, z_range)
    draw = ImageDraw.Draw(reach)
    seed_marks = {"walked": 0, "outside": 0}
    walked_columns = {(int(x), int(y)) for x, y, z in walk}
    for kind, x, y, z in seeds:
        px, py = view.point(x, y)
        if not (0 <= px < view.width and 0 <= py < view.height):
            continue
        if kind in VIEW_KINDS:
            draw.rectangle([px - 1, py - 1, px + 1, py + 1], fill=(255, 230, 40))
        elif any((int(x // CELL) + dx, int(y // CELL) + dy) in walked_columns for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            draw.ellipse([px - 3, py - 3, px + 3, py + 3], outline=(255, 40, 40), width=2)
        else:
            draw.line([px - 4, py - 4, px + 4, py + 4], fill=(255, 0, 255), width=2)
            draw.line([px - 4, py + 4, px + 4, py - 4], fill=(255, 0, 255), width=2)
    reach.save(folder / "reach.png")

    tier_colours = [(70, 220, 90), (235, 200, 50), (230, 60, 50)]
    floors_image(view, triangles, lambda i: tier_colours[int(tiers[i])] if i < len(tiers) else (90, 90, 90),
                 z_range).save(folder / "tiers.png")
    missed = set(int(t) for t in unreached["t"])
    image = floors_image(view, triangles, lambda i: (255, 50, 40) if i in missed else (150, 150, 150), z_range)
    overlay_cells(image, view, walk, CELL, (60, 255, 90), 120).save(folder / "unreached.png")

    # the biggest unreached floors, for the check by eye
    biggest = sorted(unreached, key=lambda r: -float(r["a"]))[:12]
    summary["unreached_largest"] = [
        {"area": round(float(r["a"]), 2),
         "centroid": [round(float(sum(triangles[int(r["t"])][2 + k][j] for k in range(3)) / 3), 1) for j in range(3)]}
        for r in biggest]
    # (the walk seeds within the walk's box, and those with a floor walked in their column or the next)
    in_bsp = [s for s in inside if all(lo[k] <= s[1 + k] <= hi[k] for k in range(3))]
    summary["walk_seeds_in_bsp"] = len(in_bsp)
    summary["walk_seeds_walked"] = sum(
        1 for kind, x, y, z in in_bsp
        if any((int(x // CELL) + dx, int(y // CELL) + dy) in walked_columns for dx in (-1, 0, 1) for dy in (-1, 0, 1)))
    (folder / "summary.json").write_text(json.dumps(summary, indent=2))
    return summary



def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--data", required=True, help="the data root (its maps folder holds <map>.map)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--bsp", type=int, action="append")
    parser.add_argument("--density", type=float, action="append")
    parser.add_argument("--flight", choices=("auto", "on", "off"), default="auto")
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    probe = build_probe(out)
    results = []
    for name in args.maps:
        path = Path(name) if name.endswith(".map") else Path(args.data) / "maps" / (name + ".map")
        game_map = ce_map.Map(path)
        indices = list(args.bsp or range(game_map.bsp_count()))
        summaries = {}
        walked = {}
        # (rounds: each bsp seeded by the others' floors until no walk grows)
        for round_index in range(4):
            grown = False
            for index in indices:
                transfers = [p for other, floors in walked.items() if other != index for p in floors]
                if round_index and not transfers:
                    continue
                before = summaries[index]["reach"]["walk"] if index in summaries else -1
                if round_index and before >= 0 and summaries[index].get("transfers") == len(transfers):
                    continue
                summary = run_bsp(probe, out, game_map, index, args.density or [16.0, 32.0], args.flight, transfers)
                summary["transfers"] = len(transfers)
                summary["round"] = round_index
                summaries[index] = summary
                walked[index] = walked_floors(out / game_map.name / str(index))
                grown = grown or summary["reach"]["walk"] != before
            if not grown or len(indices) < 2 or args.bsp:
                break
        for index in indices:
            summary = summaries[index]
            r, i, f = summary["reach"], summary["importance"], summary["floors"]
            print("%s bsp %d (%s): walk %d floors (%.0f wu2), %.1f s; seeds %d used, %d outside; floors reached %.1f%% "
                  "of %.0f wu2; tiers %s" % (game_map.name, index, summary["bsp_name"], r["walk"], r["walk_area_wu2"],
                                             r["seconds"], r["seeds_used"], r["seeds_outside"],
                                             100.0 * f["area_reached"] / max(f["area"], 1e-9), f["area"],
                                             i["triangles"]), flush=True)
            (out / game_map.name / str(index) / "summary.json").write_text(json.dumps(summary, indent=2))
            results.append(summary)
    (out / "summary.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
