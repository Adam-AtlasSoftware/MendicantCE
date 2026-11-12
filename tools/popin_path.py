#!/usr/bin/env python3
"""A camera path (debug.camera_path, port/linux/game/camera_pose.c) through a
map's walkable ground, made from its places (the console's
debug_scenario_places): the AI's firing positions joined where they are
near each other, the shortest walk from the player's start to the place
furthest from it within a length, the camera at eye height looking along
the walk, keys every few world units at a steady speed; then a dolly (back
and up from the walk's end, and in again) through the distances the levels
of detail change at.

  popin_path.py <places> <path out> [--length 220] [--speed 4.5] [--dolly 60]

The places are the player's own map's: the path made from them stays on the
machine it was made on.
"""

import argparse
import heapq
import math


def read_places(path):
    places = {"player": [], "firing": [], "flag": []}
    for line in open(path):
        parts = line.split()
        if len(parts) >= 4 and parts[0] in places:
            places[parts[0]].append(tuple(float(v) for v in parts[1:4]) + (float(parts[4]) if len(parts) > 4 else 0.0,))
    return places


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("places")
    parser.add_argument("out")
    parser.add_argument("--length", type=float, default=220.0, help="the walk's most length (world units)")
    parser.add_argument("--speed", type=float, default=4.5, help="world units a second (of 60 frames)")
    parser.add_argument("--step", type=float, default=5.0, help="world units between keys")
    parser.add_argument("--eye", type=float, default=1.2, help="the camera over the ground")
    parser.add_argument("--dolly", type=float, default=60.0, help="the dolly's length back (0 none)")
    parser.add_argument("--start-frames", type=int, default=0, help="frames after the map's first")
    parser.add_argument("--from", dest="start", default="", help="x,y,z to start nearest (default the player's)")
    parser.add_argument("--fov", type=float, default=70.0)
    parser.add_argument("--hold", type=int, default=0, help="frames held still at the start")
    parser.add_argument("--link", type=float, default=7.0, help="places this near are joined (world units)")
    parser.add_argument("--rise", type=float, default=2.0, help="...and no more apart in height")
    args = parser.parse_args()

    places = read_places(args.places)
    points = []
    for p in places["firing"]:
        if all(math.dist(p[:3], q) > 0.75 for q in points[-200:]):
            points.append(p[:3])
    if args.start:
        start = tuple(float(v) for v in args.start.split(","))
    else:
        start = places["player"][0][:3]
    neighbours = [[] for _ in points]
    for i, p in enumerate(points):
        for j in range(i + 1, len(points)):
            q = points[j]
            d = math.dist(p, q)
            if d < args.link and abs(p[2] - q[2]) < args.rise:
                neighbours[i].append((j, d))
                neighbours[j].append((i, d))
    first = min(range(len(points)), key=lambda i: math.dist(points[i], start))
    distance = {first: 0.0}
    previous = {}
    queue = [(0.0, first)]
    while queue:
        d, i = heapq.heappop(queue)
        if d > distance.get(i, 1e30) or d > args.length:
            continue
        for j, w in neighbours[i]:
            if d + w < distance.get(j, 1e30):
                distance[j] = d + w
                previous[j] = i
                heapq.heappush(queue, (d + w, j))
    last = max((i for i in distance if distance[i] <= args.length), key=lambda i: math.dist(points[i], points[first]))
    walk = [last]
    while walk[-1] != first:
        walk.append(previous[walk[-1]])
    walk.reverse()
    route = [start] + [points[i] for i in walk]

    # keys every step along the route
    keys = []
    travelled = 0.0
    next_key = 0.0
    for a, b in zip(route, route[1:]):
        length = math.dist(a, b)
        while next_key <= travelled + length and length > 0:
            t = (next_key - travelled) / length
            keys.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(3)))
            next_key += args.step
        travelled += length
    keys.append(route[-1])
    poses = []
    for index, key in enumerate(keys):
        ahead = keys[min(index + 2, len(keys) - 1)]
        behind = keys[max(index - 2, 0)]
        yaw = math.degrees(math.atan2(ahead[1] - behind[1], ahead[0] - behind[0]))
        poses.append([key[0], key[1], key[2] + args.eye, yaw, -4.0])
    for index in range(1, len(poses)):
        while poses[index][3] - poses[index - 1][3] > 180:
            poses[index][3] -= 360
        while poses[index][3] - poses[index - 1][3] < -180:
            poses[index][3] += 360
    # the yaws smoothed (the walk turns at each place)
    yaws = [p[3] for p in poses]
    for index in range(len(poses)):
        window = yaws[max(0, index - 2):index + 3]
        poses[index][3] = sum(window) / len(window)

    frames_a_unit = 60.0 / args.speed
    out = open(args.out, "w")
    out.write("# made by tools/popin_path.py from %s: %d keys, the walk %.0f world units\n"
              % (args.places, len(poses), travelled))
    out.write("start %d\n" % args.start_frames)
    out.write("cmd show_hud 0\nat 20 game_speed 0\n")
    frame = 0
    if args.hold:
        p = poses[0]
        out.write("key %d %.3f %.3f %.3f %.2f %.2f 0 %.1f\n" % (frame, p[0], p[1], p[2], p[3], p[4], args.fov))
        frame += args.hold
    for index, p in enumerate(poses):
        if index:
            frame += max(1, int(round(math.dist(p[:3], poses[index - 1][:3]) * frames_a_unit)))
        out.write("key %d %.3f %.3f %.3f %.2f %.2f 0 %.1f\n" % (frame, p[0], p[1], p[2], p[3], p[4], args.fov))
    if args.dolly > 0:
        end = poses[-1]
        yaw = math.radians(end[3])
        steps = int(args.dolly / args.step)
        for direction in (1, -1):
            for s in range(1, steps + 1):
                k = s if direction == 1 else steps - s
                d = k * args.step
                frame += int(round(args.step * frames_a_unit))
                out.write("key %d %.3f %.3f %.3f %.2f %.2f 0 %.1f\n" % (
                    frame, end[0] - math.cos(yaw) * d, end[1] - math.sin(yaw) * d, end[2] + 0.25 * d, end[3],
                    end[4] - math.degrees(math.atan2(0.25 * d, d + 8.0)), args.fov))
    out.close()
    print("%d keys, %d frames, the walk %.0f world units, from (%.1f %.1f %.1f) to (%.1f %.1f %.1f)"
          % (len(poses), frame, travelled, route[0][0], route[0][1], route[0][2], route[-1][0], route[-1][1],
             route[-1][2]))


if __name__ == "__main__":
    main()
