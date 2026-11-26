#!/usr/bin/env python3
"""The farm's content build of every level's enhanced sidecar (PLAN_FINAL.md section 2.2 step 5 and the release gate
of section 2.6: "double content build in two clean places: identical bytes"), and the comparison of two builds.

    build_levels.py build --maps <retail maps folder> --levels <remodel folder> --out <empty folder>
                          [--map a10 ...] [--native-dir DIR] [--jobs N]
    build_levels.py compare <build A folder> <build B folder>

build: for each map (every *.map of --maps but ui, or the --map ones), its classic_world_id (classic_world.py)
and, where --levels/<map>/ holds remodel .pwg files, its sidecar (encode.py --pwg-dir: each .pwg on the BSP its
header names) at <out>/maps_enhanced/<map>.prsx with its report, then the game's own reader's verdict on it (native
open: every section verified, the world_id of each BSP). A map with no remodel, or a BSP of one with none, ships
classic for that BSP (no section). <out>/build.json records, per map: build string, base_id, classic_world_id, the
BSPs, which have enhanced geometry, the sidecar's size and SHA-256 (file_sha), and every BSP's classic and enhanced
world_id. It holds no time, host or path, so two builds of the same inputs write the same build.json too.

compare: the two builds' build.json and sidecars side by side; exit 0 only if every sidecar is the same bytes and
every world_id is equal (the proof of a reproducible build), else a line per difference and exit 1.

The farm only: it reads retail maps, which never leave the farm, and what it writes is Adam's data until the release
pipeline (M7): never committed.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _level(task):
    """one map's record (and sidecar); run in a worker process"""
    from geopatch import classic_world, encode, identity, native, retail
    map_path, levels, out, native_dir = task
    name = Path(map_path).stem
    data = Path(map_path).read_bytes()
    cwid, cwid_report = classic_world.classic_world_id(map_path)
    _, build, bsps = retail.read_map(map_path)
    record = {"build": build, "base_id": identity.base_id(data), "classic_world_id": cwid,
              "cwid_version": classic_world.CWID_VERSION,
              "bsps": [{"bsp": b.index, "name": b.name, "materials": len(b.materials)} for b in bsps]}
    del data
    folder = Path(levels) / name if levels else None
    pwgs = encode.pwgs_by_bsp(folder, bsps) if folder is not None and folder.is_dir() else {}
    for b in record["bsps"]:
        b["enhanced"] = b["bsp"] in pwgs
        b["pwg_sha256"] = _sha256_file(pwgs[b["bsp"]]) if b["enhanced"] else None
    if not pwgs:
        record["sidecar"] = None
        return name, record
    sidecar = Path(out) / "maps_enhanced" / (name + ".prsx")
    report = Path(out) / "reports" / (name + ".report.json")
    env = dict(os.environ)
    if native_dir:
        env["GEOPATCH_NATIVE_DIR"] = str(native_dir)
    command = [sys.executable, str(HERE / "encode.py"), "--map", str(map_path), "--classic-world-id", cwid,
               "--pwg-dir", str(folder), "-o", str(sidecar), "--report", str(report)]
    if native_dir:
        command += ["--native-dir", str(native_dir)]
    result = subprocess.run(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise RuntimeError("%s: encode failed: %s" % (name, result.stderr[-2000:]))
    verdict = native.Native(native_dir).open(sidecar)
    record["sidecar"] = {"file": "maps_enhanced/%s.prsx" % name, "bytes": sidecar.stat().st_size,
                         "file_sha": _sha256_file(sidecar), "reader": verdict.get("result"),
                         "sections_ok": sum(1 for e in verdict["entries"] if e["read"] == "ok"),
                         "sections": len(verdict["entries"])}
    for b in record["bsps"]:
        ids = verdict["world"].get(b["bsp"])
        b["world_id_classic"] = ids[0] if ids else None
        b["world_id_enhanced"] = ids[1] if ids and b["enhanced"] else None
    return name, record


def build(args):
    out = Path(args.out)
    if out.exists() and any(out.iterdir()):
        sys.exit("build_levels.py: %s is not empty (a build starts from clean)" % out)
    (out / "maps_enhanced").mkdir(parents=True, exist_ok=True)
    (out / "reports").mkdir(exist_ok=True)
    maps = sorted((p for p in Path(args.maps).glob("*.map") if p.stem.lower() != "ui"), key=lambda p: p.name.encode())
    if args.map:
        wanted = set(args.map)
        maps = [p for p in maps if p.stem in wanted]
        missing = wanted - {p.stem for p in maps}
        if missing:
            sys.exit("build_levels.py: no map %s" % ", ".join(sorted(missing)))
    from geopatch import native
    native.build(args.native_dir)  # (once, before the workers race to build it)
    tasks = [(str(p), args.levels, str(out), args.native_dir) for p in maps]
    records = {}
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for name, record in pool.map(_level, tasks):
            records[name] = record
            side = record["sidecar"]
            print("%s cwid %s %s" % (name, record["classic_world_id"][:16],
                  "sidecar %d bytes sha256 %s reader %s" % (side["bytes"], side["file_sha"][:16], side["reader"])
                  if side else "classic only"), flush=True)
    text = json.dumps({"format": 1, "levels": records}, indent=1, sort_keys=True) + "\n"
    (out / "build.json").write_text(text, "utf-8", newline="\n")
    return 0


def compare(args):
    a, b = (json.loads((Path(x) / "build.json").read_text("utf-8"))["levels"] for x in (args.a, args.b))
    problems = []
    if sorted(a) != sorted(b):
        problems.append("the builds have different maps: %s / %s" % (sorted(a), sorted(b)))
    rows = []
    for name in sorted(set(a) & set(b)):
        x, y = a[name], b[name]
        for key in ("build", "base_id", "classic_world_id"):
            if x[key] != y[key]:
                problems.append("%s: %s differs" % (name, key))
        if x["bsps"] != y["bsps"]:
            problems.append("%s: the BSP records (enhanced, world_ids) differ" % name)
        sx, sy = x["sidecar"], y["sidecar"]
        if (sx is None) != (sy is None):
            problems.append("%s: one build has a sidecar, the other not" % name)
            continue
        if sx is None:
            rows.append((name, "classic only", "", ""))
            continue
        fa, fb = (Path(f) / sx["file"] for f in (args.a, args.b))
        ha, hb = _sha256_file(fa), _sha256_file(fb)
        if ha != sx["file_sha"] or hb != sy["file_sha"]:
            problems.append("%s: a sidecar is not the one its build.json records" % name)
        if ha != hb:
            problems.append("%s: sidecars differ (%s / %s)" % (name, ha, hb))
        same_ids = [p["world_id_enhanced"] for p in x["bsps"]] == [p["world_id_enhanced"] for p in y["bsps"]]
        rows.append((name, ha, "same bytes" if ha == hb else "DIFFERENT", "world_ids equal" if same_ids else
                     "world_ids DIFFER"))
    for row in rows:
        print("%-16s %s %s %s" % row)
    for problem in problems:
        print("DIFFERENCE: " + problem)
    print("identical" if not problems else "%d differences" % len(problems))
    return 0 if not problems else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("--maps", required=True)
    p.add_argument("--levels")
    p.add_argument("--out", required=True)
    p.add_argument("--map", action="append")
    p.add_argument("--native-dir")
    p.add_argument("--jobs", type=int, default=1)
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b")
    args = parser.parse_args(argv)
    return build(args) if args.command == "build" else compare(args)


if __name__ == "__main__":
    sys.exit(main())
