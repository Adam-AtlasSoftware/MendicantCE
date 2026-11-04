#!/usr/bin/env python3
"""The stock maps' preparations' records shipped with the game (port/assets/prepared;
port/linux/game/render_prepared.c, port/linux/src/prepared_shipped.h), made again.

    python3 tools/prepared_records.py --maps ~/halo-x86-maps/maps [--halo build/linux_x86_64/halo] [--work <folder>]

Run on a Linux machine with the stock Xbox maps, on a build of the commit to be released. It starts the game once,
fresh, offscreen, with every setting at its default (a config.toml of its own): its first start's pass loads the
23 stock maps, then the main menu loads ui.map, and each map's preparation is recorded (z:\\prepared). The records
are compressed (zlib) into port/assets/prepared, every other file there removed, and manifest.json lists each one's
map, checksum, key, size and file with the records' form (render_prepared.c's PREPARED_VERSION and the journal's,
d3d8_device.c's halo_gpu_journal_form). The run's folder (about 3 GB of maps decompressed) is deleted as it ends.

A change that makes the records another form (PREPARED_VERSION, JOURNAL_FORM or the journal's state) fails
tools/harness/tests/test_prepared_shipped.py until this is run again and its records committed. A record holds no
map data: render states, textures by tag and bitmap index, mirror pages and shader ids.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "port/assets/prepared"
STOCK_MAP_COUNT = 24
# render_prepared.c's line, as a record is written
RECORDED = re.compile(r"render: the map's preparation recorded \((\d+) draws, (\d+) words; "
                      r"z:\\prepared\\([0-9a-f]{8})-([0-9a-f]{16})\.prepared, ([^,]*), form (\d+) (\d+)\)")
HEADER_SIZE = 40  # struct prepared_header
MAGIC = 0x50314F48


def run_game(halo: Path, maps: Path, work: Path, seconds: int) -> Path:
    """the game started fresh, until every stock map is recorded; the run's z:\\prepared"""
    run = work / "run"
    shutil.rmtree(run, ignore_errors=True)
    (run / "save").mkdir(parents=True)
    (run / "maps").symlink_to(maps.resolve())
    environment = dict(os.environ, HALO_DATA_ROOT=str(run), HALO_SAVE_ROOT=str(run / "save"),
                       HALO_CONFIG_FILE=str(run / "config.toml"), XDG_CACHE_HOME=str(run / "xdg-cache"),
                       XDG_CONFIG_HOME=str(run / "xdg-config"), XDG_DATA_HOME=str(run / "xdg-data"),
                       HALO_VULKAN_PIPELINE_CACHE=str(run / "save" / "vulkan.bin"),
                       HALO_NET_ONLINE="false", HALO_NET_PORT="5191", HALO_NET_BROADCAST="127.255.255.255",
                       HALO_UPDATE_ANSWER="no", HALO_NO_CLIPBOARD="1", HALO_DEBUG_MENU_PICTURES="1",
                       HALO_FIXED_STEP="1", HALO_PREPARED_RECORDS="1", HALO_MAP_CACHE_MEGABYTES="3500",
                       SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
    log = run / "stdout.txt"
    with open(log, "w") as output:
        game = subprocess.Popen([str(halo.resolve())], cwd=run, env=environment, stdout=output,
                                stderr=subprocess.STDOUT, start_new_session=True)
        started = time.monotonic()
        try:
            while game.poll() is None and time.monotonic() - started < seconds:
                time.sleep(1)
                if len(RECORDED.findall(log.read_text(errors="replace"))) >= STOCK_MAP_COUNT:
                    break
        finally:
            if game.poll() is None:
                os.killpg(game.pid, signal.SIGTERM)
                try:
                    game.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    os.killpg(game.pid, signal.SIGKILL)
                    game.wait()
    return run


def collect(run: Path, out: Path) -> dict:
    """the run's records compressed into out, and their manifest"""
    text = (run / "stdout.txt").read_text(errors="replace")
    lines = RECORDED.findall(text)
    if len(lines) < STOCK_MAP_COUNT:
        failed = [line for line in text.splitlines() if "is not recorded" in line or "could not be recorded" in line]
        sys.exit(f"{len(lines)} of {STOCK_MAP_COUNT} maps recorded (see {run / 'stdout.txt'})" +
                 (f": {failed[0]}" if failed else ""))
    forms = {(int(version), int(form)) for *_, version, form in lines}
    if len(forms) != 1:
        sys.exit(f"the records are of more than one form: {sorted(forms)}")
    (version, form), = forms
    records = {}
    for draws, words, checksum, key, scenario, _, _ in lines:
        source = run / "save" / "z" / "prepared" / f"{checksum}-{key}.prepared"
        data = source.read_bytes()
        magic, record_version, header_size, record_checksum = (int.from_bytes(data[at:at + 4], "little")
                                                               for at in (0, 4, 8, 12))
        record_key = int.from_bytes(data[16:24], "little")
        if (magic, record_version, header_size, record_checksum, record_key) != \
                (MAGIC, version, HEADER_SIZE, int(checksum, 16), int(key, 16)):
            sys.exit(f"{source.name}: its header is not its name's or the log's")
        name = scenario.replace("\\", "/").rsplit("/", 1)[-1]
        records[name] = {"map": name, "scenario": scenario, "checksum": checksum, "key": key,
                         "draws": int(draws), "words": int(words), "size": len(data),
                         "file": f"{name}.prepared.zlib", "data": zlib.compress(data, 9)}
    if len(records) != STOCK_MAP_COUNT:
        sys.exit(f"{len(records)} maps recorded, not {STOCK_MAP_COUNT}: {sorted(records)}")
    if out.is_dir():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for record in records.values():
        (out / record["file"]).write_bytes(record.pop("data"))
    manifest = {
        "comment": "The stock maps' preparations' records shipped with the game (render_prepared.c), made by "
                   "tools/prepared_records.py with every setting at its default: run it again whenever "
                   "tools/harness/tests/test_prepared_shipped.py says the records' form has changed.",
        "prepared_version": version,
        "journal_form": form,
        "records": [records[name] for name in sorted(records)],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--maps", required=True, type=Path, help="the stock Xbox maps' folder (a10.map, ui.map...)")
    parser.add_argument("--halo", type=Path, default=ROOT / "build/linux_x86_64/halo")
    parser.add_argument("--work", type=Path, help="where the game runs (deleted after; default: a temporary folder)")
    parser.add_argument("--out", type=Path, default=OUT)
    parser.add_argument("--seconds", type=int, default=600)
    arguments = parser.parse_args()
    if not (arguments.maps / "ui.map").is_file():
        sys.exit(f"{arguments.maps}: no ui.map")
    work = Path(tempfile.mkdtemp(prefix="prepared-records-", dir=arguments.work))
    try:
        run = run_game(arguments.halo, arguments.maps, work, arguments.seconds)
        manifest = collect(run, arguments.out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    size = sum(record["size"] for record in manifest["records"])
    compressed = sum((arguments.out / record["file"]).stat().st_size for record in manifest["records"])
    print(f"{len(manifest['records'])} records (form {manifest['prepared_version']}, journal "
          f"{manifest['journal_form']:#x}): {size / 1e6:.1f} MB, {compressed / 1e6:.2f} MB compressed, in {arguments.out}")


if __name__ == "__main__":
    main()
