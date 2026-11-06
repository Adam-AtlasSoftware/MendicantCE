#!/usr/bin/env python3
"""Compare a recorded game's log with its replay's (port/linux/REPLAY.md).

The machine that recorded a replay and the machine that played it back
both log every player each second (debug.network_test: the host's
"host:<map>", the playback's "replay:<path>"), at the same ticks of the
game. This matches their lines by tick and by player (by name, from the
"player N is named" lines, else by index) and reports how far apart each
player stood, the ticks each log has, and whether the playback reached
the recording's last logged tick.

    python3 tools/replay_compare.py <recording's log> <playback's log> [--json out.json]
        [--tolerance 1.0] [--skip-ticks 0]

Exit status 0 when the playback reached the end and every player's
median distance is within the tolerance (world units), else 1.
"""

import argparse
import json
import re
import statistics
import sys

TICK = re.compile(r"network test: tick (\d+) (.*?)(?: \| |$)")
PLAYER = re.compile(r"player (\d+): \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)")
NAMED = re.compile(r"network test: player (\d+) is named (.+?)\s*$")
UNITS = re.compile(r"network test: units tick (\d+): (\d+)(.*)$")
UNIT = re.compile(r"\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)")


def read_log(path, units=None):
    ticks = {}
    names = {}
    ended = False
    with open(path, errors="replace") as handle:
        for line in handle:
            counted = UNITS.search(line)
            if counted is not None and units is not None:
                units[int(counted.group(1))] = (int(counted.group(2)), [
                    tuple(float(unit.group(index)) for index in (1, 2, 3)) for unit in UNIT.finditer(counted.group(3))])
                continue
            named = NAMED.search(line)
            if named:
                names[int(named.group(1))] = named.group(2)
                continue
            if "network test: replay ended" in line:
                ended = True
            match = TICK.search(line)
            if not match:
                continue
            tick = int(match.group(1))
            players = {}
            for player in PLAYER.finditer(match.group(2)):
                players[int(player.group(1))] = tuple(float(player.group(index)) for index in (2, 3, 4))
            ticks[tick] = players
    return ticks, names, ended


def key_for(index, names):
    return names.get(index, f"#{index}")


def match_units(recorded, played):
    """each recorded unit's distance to the nearest unmatched played one (greedy, nearest pairs first)"""
    pairs = sorted((sum((a - b) ** 2 for a, b in zip(r, p)) ** 0.5, i, j)
                   for i, r in enumerate(recorded) for j, p in enumerate(played))
    used_r, used_p, distances = set(), set(), []
    for distance, i, j in pairs:
        if i in used_r or j in used_p:
            continue
        used_r.add(i)
        used_p.add(j)
        distances.append(distance)
    return distances


def compare_units(rec_units, play_units, tolerance, skip_ticks):
    common = sorted(tick for tick in rec_units if tick in play_units and tick >= skip_ticks)
    if not common:
        return None
    count_differences = [abs(rec_units[tick][0] - play_units[tick][0]) for tick in common]
    distances = []
    for tick in common:
        distances += match_units(rec_units[tick][1], play_units[tick][1])
    return {
        "ticks": len(common),
        "recorded_units_mean": statistics.fmean(rec_units[tick][0] for tick in common),
        "count_equal_share": sum(1 for difference in count_differences if difference == 0) / len(common),
        "count_difference_max": max(count_differences),
        "matched": len(distances),
        "median": statistics.median(distances) if distances else None,
        "p90": sorted(distances)[int(0.9 * (len(distances) - 1))] if distances else None,
        "within_tolerance": (sum(1 for distance in distances if distance <= tolerance) / len(distances))
        if distances else None,
    }


def compare(recording, playback, tolerance=1.0, skip_ticks=0):
    rec_units, play_units = {}, {}
    rec_ticks, rec_names, _ = read_log(recording, rec_units)
    play_ticks, play_names, ended = read_log(playback, play_units)
    common = sorted(tick for tick in rec_ticks if tick in play_ticks and tick >= skip_ticks)
    distances = {}
    for tick in common:
        rec = {key_for(index, rec_names): place for index, place in rec_ticks[tick].items()}
        play = {key_for(index, play_names): place for index, place in play_ticks[tick].items()}
        for name in rec:
            if name in play:
                distance = sum((a - b) ** 2 for a, b in zip(rec[name], play[name])) ** 0.5
                distances.setdefault(name, []).append((tick, distance))
    players = {}
    for name, values in sorted(distances.items()):
        only = [distance for _, distance in values]
        worst_tick, worst = max(values, key=lambda value: value[1])
        players[name] = {
            "samples": len(only),
            "median": statistics.median(only),
            "mean": statistics.fmean(only),
            "max": worst,
            "max_tick": worst_tick,
            "within_tolerance": sum(1 for distance in only if distance <= tolerance) / len(only),
        }
    rec_last = max(rec_ticks) if rec_ticks else None
    play_last = max(play_ticks) if play_ticks else None
    # (the recording's last logged tick may fall after the replay's last
    # sync, which a recording cut short by its exit does not have: a second's
    # slack per five, the file's sync period)
    reached = play_last is not None and rec_last is not None and play_last >= rec_last - 5 * 30
    good = bool(players) and reached and all(player["median"] <= tolerance for player in players.values())
    units = compare_units(rec_units, play_units, tolerance, skip_ticks)
    # (units recorded that the playback has none of: its level lacks what
    # they are, a Forge game's played without Forge's tag pack)
    if units and units["recorded_units_mean"] > 0 and not units["matched"]:
        good = False
    return {
        "recording_ticks": [min(rec_ticks), rec_last] if rec_ticks else None,
        "playback_ticks": [min(play_ticks), play_last] if play_ticks else None,
        "common_ticks": len(common),
        "playback_reached_end": reached,
        "playback_logged_end": ended,
        "tolerance": tolerance,
        "players": players,
        "units": units,
        "pass": good,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("recording")
    parser.add_argument("playback")
    parser.add_argument("--tolerance", type=float, default=1.0)
    parser.add_argument("--skip-ticks", type=int, default=0)
    parser.add_argument("--json")
    args = parser.parse_args()
    result = compare(args.recording, args.playback, args.tolerance, args.skip_ticks)
    print(f"recording ticks {result['recording_ticks']}, playback ticks {result['playback_ticks']}, "
          f"{result['common_ticks']} in common; playback reached the end: {result['playback_reached_end']} "
          f"(logged its end: {result['playback_logged_end']})")
    for name, player in result["players"].items():
        print(f"  {name:>12}: {player['samples']} samples, median {player['median']:.3f}, mean {player['mean']:.3f}, "
              f"max {player['max']:.3f} at tick {player['max_tick']}, "
              f"{100 * player['within_tolerance']:.0f}% within {result['tolerance']}")
    units = result["units"]
    if units and not units["matched"]:
        print(f"  units (bipeds no player controls): {units['ticks']} ticks, {units['recorded_units_mean']:.1f} a tick "
              f"recorded; counts equal at {100 * units['count_equal_share']:.0f}% of ticks (most apart "
              f"{units['count_difference_max']}); none matched")
    elif units:
        print(f"  units (bipeds no player controls): {units['ticks']} ticks, {units['recorded_units_mean']:.1f} a tick "
              f"recorded; counts equal at {100 * units['count_equal_share']:.0f}% of ticks (most apart "
              f"{units['count_difference_max']}); {units['matched']} matched, median {units['median']:.3f}, "
              f"90th percentile {units['p90']:.3f}, {100 * units['within_tolerance']:.0f}% within {result['tolerance']}")
    print("PASS" if result["pass"] else "FAIL")
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=1)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
