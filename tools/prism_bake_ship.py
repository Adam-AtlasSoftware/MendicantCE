#!/usr/bin/env python3
"""PRISM's shipped bake (docs/prism_bake.md, "The shipped bake"): a release's
light, every campaign level baked at MAX for both worlds (the stock and the
remodel), lighting only, which the game uses wherever its key is the
player's own BSP's and bakes nothing.

    export <bake folder> <stock|remodel> <shipped folder> [--maps b30,a30]
           [--quality max] [--bsps d40:10,...] [--maps-folder <maps>]
           [--empty d40:9,...] [--archives <folder>] [--world-ids cwid.json]
        copies each finished BSP bake (<map>/bsp<n>.pbk) of the bake folder
        into <shipped>/<world>/<map>/, after checking that it is whole (a
        DONE record), of the quality asked or better, of this baker's file
        version, lighting only (no record of a kind that is not light: see
        LIGHT_RECORDS) and of the world named (its LYRB record's). A level
        ships whole or not at all (players never bake): its BSP count is
        the map's (--maps-folder), the worker's note's (baked.txt "bsps N")
        or --bsps, which must agree, and every BSP must have its file, but
        for one --empty names (a BSP with no lightmapped surface, which has
        nothing to bake: d40's BSP 9). The rig is always the units rig (the
        shipped one: a note that says otherwise is refused). Writes
        manifest.txt (the game's: each BSP's settings key,
        prism_bake_menu.c's) and manifest.json (the files' sizes and
        sha256, each BSP's per-BSP input hashes and world_id) and, with
        --archives, a release asset per level and world
        (prism_bake_<map>_<world>_v<baker>.ppk: an uncompressed tar of that
        level's files and its manifest lines; the records are packed
        already), each under GitHub's 2 GiB asset limit.
        v4 geopatch M4: every bake is keyed to its BSP's world_id (its
        header's, xgpu_vulkan_bake.c): a stock bake's must be the classic
        world_id of the map's classic_world_id (--world-ids: the farm's
        tools/geopatch/classic_world.py output, which also writes each
        level's "world <map> <base_id> <classic_world_id>" line, the game's
        lookup until M7's signed manifest), a remodel bake's an enhanced one
        (its key "geo:prsx:<world_id>"). Each level's folder gets its own
        manifest.txt (<world>/<map>/manifest.txt: its lines), which the
        game's ladder and its Lightmap Downloads rows read. A bake's
        nondeterminism (GPU bakes are not bit-identical run to run) never
        matters here: the world_id names the geometry the light is for, and
        the manifest's sha256 names the bytes this export produced.
        --reach: each BSP's reach (the farm's walk of the BSP's reachable
        space, <map>/bsp<n>.reach beside its bake) carried in its bake as a
        REAC record where the bake has none, so that a player's machine
        takes the farm's walk and walks none (d40's BSP 8's took 696 s at
        load); --require-reach: a BSP whose bake ends with no REAC record
        is refused.
    verify <shipped folder>
        every file manifest.json names is there, of its size and sha256, a
        whole lighting-only bake whose header matches its entry (and its
        REAC record, where it has one, a whole reach).
    plancheck <shipped folder> <plans folder> [--world stock|remodel]
              [--maps b30,a30]
        each shipped BSP's key, field by field, against the key a player's
        game made for that BSP without baking (HALO_BAKE_PLAN_ONLY, in play:
        <plans>/<map>/plan_bsp<n>.txt), its tiles against the plan's, and
        its REAC record's walk against the player's own walk (its inputs'
        hash, the bricks' and tiers' counts and CRC-32s): a bake the
        player's game would refuse, or plan otherwise, never ships. Every
        shipped BSP needs its plan.
    install <shipped folder> <archive>...
        a release's archives unpacked into the shipped folder (its paths
        checked: nothing outside it), the manifests merged, then verified.
    show <file.pbk>
        a bake file's key and records.

The bake file's format is prism_bake_cache.h's (read here independently of
the game: the header's key and checksum, the records' headers and
checksums). A shipped bake holds no triangle, albedo, texture or remodel
geometry: the player's machine rebuilds the plan from its own maps (and its
own remodel) and checks the key's per-BSP hashes before it uses the light.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import struct
import sys
import tarfile
import zlib
from pathlib import Path

HEADER_SIZE = 256
RECORD_HEADER_SIZE = 40
CACHE_VERSIONS = (3,)
MAGIC = b"PRISMBAK"
RECORD_MAGIC = b"PBKR"
QUALITIES = ("draft", "high", "ultra", "max")
# GitHub's release asset limit is 2 GiB a file: a level's archive past this
# is split into one archive a BSP
ASSET_LIMIT = int(1.9 * 1024 ** 3)


def fourcc(text: str) -> int:
    return struct.unpack("<I", text.encode("ascii"))[0]


# the records a shipped bake may hold: the plan (job ids), the light (the
# lightmap's tiles and their filtered texels, their layers' shares, the sun
# mask's tiles, the volume's bricks and their shares, the reflection probes),
# the probes' placement (the reach's bricks), the lamp grid's lamp indices,
# the relight footprints (tile ids), the layers' base (the light baked with)
# and DONE. Not ATLX (the filter's aux: texel normals and charts) nor CKPT (a
# job part-way): a finished file has neither
LIGHT_RECORDS = {fourcc(name): name for name in (
    "PLAN", "DONE", "ATLS", "ATLF", "ATLY", "MASK", "VOLB", "VOLY", "PROB", "IMPG", "REAC", "FOOT", "LYRB",
    # (the foliage-ready layers, M4: the sun mask and the probes' sun visibility with the static scenery's cutouts
    # passed, and the foliage classes, a bit a scenery triangle: light and classes, no geometry)
    "MSKF", "VOLF", "FCLS")}
GEOMETRY_RECORDS = {fourcc(name): name for name in ("ATLX", "CKPT")}
# the rig every shipped bake is made with (docs/prism_bake.md: the rig is
# pinned to its shipped default on the farm and in the game)
RIG = "units"


class BakeError(Exception):
    pass


def read_header(data: bytes) -> dict:
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise BakeError("not a PRISM bake file")
    version, size = struct.unpack_from("<II", data, 8)
    if version not in CACHE_VERSIONS or size != HEADER_SIZE:
        raise BakeError(f"bake file version {version} (this tool reads {CACHE_VERSIONS})")
    crc = struct.unpack_from("<I", data, HEADER_SIZE - 4)[0]
    if zlib.crc32(data[:HEADER_SIZE - 4]) & 0xFFFFFFFF != crc:
        raise BakeError("the header's checksum does not check out")
    map_checksum, bsp_index = struct.unpack_from("<Ii", data, 16)
    map_name = data[24:88].split(b"\0", 1)[0].decode("utf-8", "replace")
    bsp_name = data[88:152].split(b"\0", 1)[0].decode("utf-8", "replace")
    bsp_hash, geometry_hash, material_hash, light_hash, settings_hash = struct.unpack_from("<5Q", data, 152)
    baker_version, quality = struct.unpack_from("<II", data, 192)
    threshold = struct.unpack_from("<f", data, 200)[0]
    world_id = data[204:236].hex()
    scene_version, variant = struct.unpack_from("<II", data, 236)
    return {
        "world_id": world_id, "scene_version": scene_version, "variant": variant,
        "map_checksum": map_checksum, "map_name": map_name, "bsp_index": bsp_index, "bsp_name": bsp_name,
        "bsp_hash": f"{bsp_hash:016x}", "geometry_hash": f"{geometry_hash:016x}",
        "material_hash": f"{material_hash:016x}", "light_hash": f"{light_hash:016x}",
        "settings_hash": f"{settings_hash:016x}", "baker_version": baker_version,
        "quality": QUALITIES[quality] if quality < len(QUALITIES) else str(quality),
        "convergence_threshold": round(threshold, 6),
    }


def read_records(data: bytes) -> list[dict]:
    """the records whose headers and payloads check out, in order (a torn
    last record ends the list, as the game reads it)"""
    records = []
    at = HEADER_SIZE
    while at + RECORD_HEADER_SIZE <= len(data):
        h = data[at:at + RECORD_HEADER_SIZE]
        if h[:4] != RECORD_MAGIC or zlib.crc32(h[:RECORD_HEADER_SIZE - 4]) & 0xFFFFFFFF != \
                struct.unpack_from("<I", h, RECORD_HEADER_SIZE - 4)[0]:
            break
        kind, job, sequence, stored, payload_crc, generation, unpacked = struct.unpack_from("<IQIIIII", h, 4)
        start = at + RECORD_HEADER_SIZE
        if start + stored > len(data) or zlib.crc32(data[start:start + stored]) & 0xFFFFFFFF != payload_crc:
            break
        records.append({"type": kind, "job": job, "sequence": sequence, "stored": stored,
                        "size": unpacked or stored, "packed": unpacked != 0, "offset": start,
                        "generation": generation})
        at = start + ((stored + 7) & ~7)
    return records


def type_name(kind: int) -> str:
    try:
        return struct.pack("<I", kind).decode("ascii")
    except UnicodeDecodeError:
        return f"0x{kind:08x}"


def check_bake(data: bytes, minimum_quality: str = "draft") -> tuple[dict, list[dict], bool | None]:
    """a whole, lighting-only bake of minimum_quality or better: its header,
    records and whether it is of the stock world (its layers' base; None
    where unknown)"""
    header = read_header(data)
    records = read_records(data)
    if not any(r["type"] == fourcc("DONE") for r in records):
        raise BakeError("not whole (no DONE record): only a finished bake ships")
    for record in records:
        if record["type"] in GEOMETRY_RECORDS:
            raise BakeError(f"holds a {GEOMETRY_RECORDS[record['type']]} record (not light): the unfinished log, "
                            "not the finished file")
        if record["type"] not in LIGHT_RECORDS:
            raise BakeError(f"holds a record of a kind this tool does not know is light ({type_name(record['type'])})")
    if header["quality"] not in QUALITIES or QUALITIES.index(header["quality"]) < QUALITIES.index(minimum_quality):
        raise BakeError(f"its quality is {header['quality']}, under {minimum_quality}")
    stock = None
    for record in records:
        if record["type"] == fourcc("LYRB") and not record["packed"] and record["size"] >= 8:
            version, flag = struct.unpack_from("<II", data, record["offset"])
            stock = bool(flag & 1)
    return header, records, stock


# the reach's bytes (xgpu_vulkan_bake.c's reach_bytes: its file bsp<n>.reach
# and the REAC record are the same): "PRCH", version 1, the inputs' hash, the
# flight flag, 8 words of summary, the bricks' and tiers' counts, then the
# bricks (4 int16 each) and the tiers (a byte each)
REACH_MAGIC = b"PRCH"
REACH_VERSION = 1
REACH_HEAD = 60
# a record's payload at most (prism_bake_cache.h's PRISM_BAKE_CACHE_MAXIMUM_PAYLOAD)
MAXIMUM_PAYLOAD = 64 << 20


def read_reach(data: bytes) -> dict:
    """a whole reach's summary: its inputs' hash, flight flag, bricks' and
    tiers' counts and CRC-32s"""
    if len(data) < REACH_HEAD or data[:4] != REACH_MAGIC:
        raise BakeError("not a reach (no PRCH head)")
    version, input_hash, flight = struct.unpack_from("<IQi", data, 4)
    bricks, tiers = struct.unpack_from("<II", data, 52)
    if version != REACH_VERSION:
        raise BakeError(f"reach version {version} (this tool reads {REACH_VERSION})")
    if len(data) != REACH_HEAD + bricks * 8 + tiers:
        raise BakeError(f"a reach of {bricks} bricks and {tiers} tiers is {REACH_HEAD + bricks * 8 + tiers} bytes, "
                        f"not {len(data)}")
    return {"input": f"{input_hash:016x}", "flight": flight, "bricks": bricks, "tiers": tiers,
            "bricks_crc": f"{zlib.crc32(data[REACH_HEAD:REACH_HEAD + bricks * 8]) & 0xFFFFFFFF:08x}",
            "tiers_crc": f"{zlib.crc32(data[REACH_HEAD + bricks * 8:]) & 0xFFFFFFFF:08x}"}


def reach_payload(data: bytes, records: list[dict]) -> bytes | None:
    """the bake's REAC record's payload, or None"""
    found = [r for r in records if r["type"] == fourcc("REAC")]
    if not found:
        return None
    record = found[-1]
    if record["packed"]:
        raise BakeError("its REAC record is packed (a reach is stored as it is)")
    return data[record["offset"]:record["offset"] + record["stored"]]


def _record_header(kind: int, job: int, sequence: int, stored: int, payload_crc: int, generation: int,
                   unpacked: int) -> bytes:
    h = bytearray(RECORD_HEADER_SIZE)
    h[:4] = RECORD_MAGIC
    struct.pack_into("<IQIIIII", h, 4, kind, job, sequence, stored, payload_crc, generation, unpacked)
    struct.pack_into("<I", h, RECORD_HEADER_SIZE - 4, zlib.crc32(bytes(h[:RECORD_HEADER_SIZE - 4])) & 0xFFFFFFFF)
    return bytes(h)


def with_reach(data: bytes, records: list[dict], reach: bytes | None = None) -> bytes:
    """the bake with its reach as its FIRST record (REAC, sequence 0, the DONE record's generation; every other record
    after it as it was, its sequence one more), so that a player's game finds the walk by reading the header and one
    record, not the whole log (a 1.4 GB bake's records' headers, read cold, took 16 s): the reach given, where the
    bake has none, or the bake's own REAC record moved first. The records' payloads are unchanged"""
    walk = reach_payload(data, records)
    if reach is not None and walk is not None:
        raise BakeError("has a REAC record already")
    reach = reach if reach is not None else walk
    if reach is None:
        raise BakeError("no reach to carry")
    read_reach(reach)
    if len(reach) > MAXIMUM_PAYLOAD:
        raise BakeError(f"its reach ({len(reach)} bytes) is past a record's {MAXIMUM_PAYLOAD}")
    if not records:
        raise BakeError("no records")
    last = records[-1]
    if last["offset"] + ((last["stored"] + 7) & ~7) != len(data):
        raise BakeError("its log does not end at its last record (torn)")
    generation = next((r["generation"] for r in reversed(records) if r["type"] == fourcc("DONE")), last["generation"])
    parts = [data[:HEADER_SIZE], _record_header(fourcc("REAC"), 0, 0, len(reach), zlib.crc32(reach) & 0xFFFFFFFF,
                                                generation, 0), reach, b"\0" * (-len(reach) % 8)]
    sequence = 1
    for r in records:
        if r["type"] == fourcc("REAC"):
            continue
        start = r["offset"]
        payload_crc = struct.unpack_from("<I", data, start - RECORD_HEADER_SIZE + 24)[0]
        parts.append(_record_header(r["type"], r["job"], sequence, r["stored"], payload_crc, r["generation"],
                                    r["size"] if r["packed"] else 0))
        parts.append(data[start:start + ((r["stored"] + 7) & ~7)])
        sequence += 1
    return b"".join(parts)


def reach_first(data: bytes, records: list[dict]) -> bool:
    """whether the bake's first record is its REAC record"""
    return bool(records) and records[0]["type"] == fourcc("REAC")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_note(bake_folder: Path, map_name: str) -> dict:
    """the main menu's note (baked.txt), where there is one: the map's BSP
    count ("bsps N", None where it has none), each BSP's tag and rig class
    (the key's third word: "v19 max units cutouts geo:...")"""
    note = {"bsps": None, "tags": {}, "rigs": {}}
    path = bake_folder / map_name / "baked.txt"
    if path.is_file():
        for line in path.read_text("utf-8", "replace").splitlines():
            match = re.match(r"bsps (\d+)$", line.strip())
            if match:
                note["bsps"] = int(match.group(1))
                continue
            match = re.match(r"bsp(\d+) (\S+) (\S+) (\S+) ", line)
            if match:
                note["rigs"][int(match.group(1))] = match.group(4)
            match = re.match(r"bsp(\d+) .* geo:(?:stock|pwg|prsx):(.*)$", line)
            if match:
                tag = match.group(2)
                if " geo:pwg:" in line:
                    tag = tag.split(":", 1)[0]
                if " geo:prsx:" in line:
                    tag = tag.split(":", 1)[1] if ":" in tag else ""
                note["tags"][int(match.group(1))] = tag
    return note


def noted_tags(bake_folder: Path, map_name: str) -> dict[int, str]:
    """each BSP's tag as the main menu's note (baked.txt) has it, where it does"""
    return read_note(bake_folder, map_name)["tags"]


def map_bsp_count(maps_folder: Path, map_name: str) -> int:
    """the map's own BSP count (its scenario's structure BSPs), read from the
    retail map as the game reads it (tools/prism_bake/ce_map.py)"""
    sys.path.insert(0, str(Path(__file__).resolve().parent / "prism_bake"))
    try:
        import ce_map
    finally:
        sys.path.pop(0)
    path = maps_folder / f"{map_name}.map"
    if not path.is_file():
        raise BakeError(f"{path}: no such map (--maps-folder)")
    try:
        return ce_map.Map(str(path)).bsp_count()
    except ce_map.MapError as error:
        raise BakeError(f"{path}: {error}") from error


def per_map(text: str, option: str, single: list[str] | None) -> dict[str, list[int]]:
    """'d40:10,a10:1' as {map: [numbers]}; a bare number is the one map's
    (--maps naming exactly one)"""
    result: dict[str, list[int]] = {}
    for item in (t.strip() for t in (text or "").split(",")):
        if not item:
            continue
        name, _, number = item.rpartition(":")
        if not name:
            if not single or len(single) != 1:
                raise BakeError(f"{option} {item}: name the map (map:number) unless --maps names one map")
            name = single[0]
        if not number.isdigit():
            raise BakeError(f"{option} {item}: not map:number")
        result.setdefault(name, []).append(int(number))
    return result


def level_bsp_count(map_name: str, note: dict, bsps: dict[str, list[int]], maps_folder: Path | None) -> int:
    """the level's real BSP count, from every source given, which must agree"""
    counts = {}
    if map_name in bsps:
        if len(bsps[map_name]) != 1:
            raise BakeError(f"--bsps names {map_name} more than once")
        counts["--bsps"] = bsps[map_name][0]
    if note["bsps"] is not None:
        counts["baked.txt"] = note["bsps"]
    if maps_folder is not None:
        counts["the map"] = map_bsp_count(maps_folder, map_name)
    if not counts:
        raise BakeError(f"{map_name}: its BSP count is unknown (no baked.txt \"bsps N\"; give --bsps or "
                        "--maps-folder): a level ships whole or not at all")
    if len(set(counts.values())) != 1:
        raise BakeError(f"{map_name}: its BSP counts disagree ("
                        + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) + ")")
    count = next(iter(counts.values()))
    if count <= 0:
        raise BakeError(f"{map_name}: a BSP count of {count}")
    return count


def manifest_key(header: dict, rig: str, stock: bool, tag: str | None) -> str:
    """the BSP's settings key as the game's note keeps it (prism_bake_menu.c's
    prism_bake_key; its quality the file's, which is good for every quality
    under it), its geometry by kind ("geo:stock", or the enhanced world's
    "geo:prsx:<world_id>", which prism_world_source.c's ladder looks for) and
    its tag where the note gave it"""
    geometry = "geo:stock" if stock else f"geo:prsx:{header['world_id']}"
    return f"v{header['baker_version']} {header['quality']} {rig} cutouts {geometry}" + (f":{tag}" if tag else "")


def manifest_load(shipped: Path) -> dict:
    path = shipped / "manifest.json"
    if path.is_file():
        return json.loads(path.read_text("utf-8"))
    return {"format": 1, "kind": "prism_bake_shipped", "bsps": []}


def manifest_save(shipped: Path, manifest: dict) -> None:
    manifest["bsps"].sort(key=lambda e: (e["world"], e["map"], e["bsp"]))
    (shipped / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", "utf-8", newline="\n")
    lines = ["prism_bake_shipped 1"]
    levels: dict[tuple, list[str]] = {}
    worlds = manifest.get("worlds", {})
    for entry in manifest["bsps"]:
        line = f"bsp {entry['world']} {entry['map']} {entry['bsp']} {entry['of']} {entry['key']}"
        lines.append(line)
        levels.setdefault((entry["world"], entry["map"]), []).append(line)
    for name, info in sorted(worlds.items()):
        lines.append(f"world {name} {info['base_id']} {info['classic_world_id']}")
    (shipped / "manifest.txt").write_text("\n".join(lines) + "\n", "utf-8", newline="\n")
    # (each level's own: the game's ladder and its Lightmap Downloads rows read <world>/<map>/manifest.txt)
    for (world, name), level_lines in sorted(levels.items()):
        folder = shipped / world / name
        folder.mkdir(parents=True, exist_ok=True)
        extra = [f"world {name} {worlds[name]['base_id']} {worlds[name]['classic_world_id']}"] if name in worlds else []
        (folder / "manifest.txt").write_text("\n".join(["prism_bake_shipped 1"] + level_lines + extra) + "\n", "utf-8",
                                             newline="\n")


def world_id_classic(classic_world_id: str, bsp: int) -> str:
    """tools/geopatch/identity.py's (port/linux/game/prism_identity.c's)"""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        from geopatch import identity
    finally:
        sys.path.pop(0)
    return identity.world_id_classic(bytes.fromhex(classic_world_id), bsp)


def export(bake_folder: Path, world: str, shipped: Path, maps: list[str] | None, quality: str,
           archives: Path | None, bsps: str = "", maps_folder: Path | None = None, empty: str = "",
           world_ids: dict | None = None, reach: bool = False, require_reach: bool = False) -> list[dict]:
    """every level's BSPs checked first, then copied: a level with a BSP
    missing, of the wrong world or not a whole light bake is refused and
    nothing of the export is written"""
    if world not in ("stock", "remodel"):
        raise BakeError("the world is stock or remodel")
    world_ids = world_ids or {}
    bsps_given = per_map(bsps, "--bsps", maps)
    empty_given = per_map(empty, "--empty", maps)
    folders = sorted(p for p in bake_folder.iterdir() if p.is_dir() and (not maps or p.name in maps))
    for name in maps or []:
        if not (bake_folder / name).is_dir():
            raise BakeError(f"{name}: no folder in {bake_folder}")
    for name in set(bsps_given) | set(empty_given):
        if not (bake_folder / name).is_dir() or (maps and name not in maps):
            raise BakeError(f"{name}: named by --bsps or --empty but not exported")
    levels = []
    for folder in folders:
        files = {int(f.stem[3:]): f for f in folder.glob("bsp*.pbk") if re.fullmatch(r"bsp\d+\.pbk", f.name)}
        if not files and not (maps and folder.name in maps) and folder.name not in empty_given:
            continue
        note = read_note(bake_folder, folder.name)
        count = level_bsp_count(folder.name, note, bsps_given, maps_folder)
        empties = set(empty_given.get(folder.name, []))
        beyond = sorted(i for i in set(files) | empties if i >= count)
        if beyond:
            raise BakeError(f"{folder.name}: BSP {beyond[0]} past its {count} BSPs")
        both = sorted(empties & set(files))
        if both:
            raise BakeError(f"{folder.name}: BSP {both[0]} is named --empty but has a bake")
        missing = [i for i in range(count) if i not in files and i not in empties]
        if missing:
            raise BakeError(f"{folder.name}: BSP " + ", ".join(str(i) for i in missing) + f" of {count} not baked "
                            "(bsp<n>.pbk missing): a level ships whole or not at all")
        entries = []
        for index in range(count):
            rig = note["rigs"].get(index, RIG)
            if rig != RIG:
                raise BakeError(f"{folder.name}: BSP {index} was baked with the {rig} rig (baked.txt): only the "
                                f"{RIG} rig ships")
            if index in empties:
                # (nothing to bake: listed so that the level is whole, with no file)
                entries.append({"world": world, "map": folder.name, "bsp": index, "of": count, "file": None,
                                "empty": True, "key": ""})
                continue
            file = files[index]
            data = file.read_bytes()
            try:
                header, records, stock = check_bake(data, quality)
            except BakeError as error:
                raise BakeError(f"{file}: {error}") from error
            if header["bsp_index"] != index:
                raise BakeError(f"{file}: its header is BSP {header['bsp_index']}'s")
            if stock is None:
                raise BakeError(f"{file}: no LYRB record says which world it was baked from")
            if world == "stock" and not stock:
                raise BakeError(f"{file}: a bake of the remodel, exported as the stock world's")
            # (a BSP with no lightmap tile, its light the volume's and the probes' alone, has no texel to say
            # it saw the remodel, and its LYRB says stock however it was baked: its key says its world, variant 1
            # and the enhanced world_id, checked below)
            if world == "remodel" and stock and any(r["type"] == fourcc("ATLS") for r in records):
                raise BakeError(f"{file}: a bake of the stock world (its LYRB record), exported as the remodel's")
            if header["variant"] != (0 if world == "stock" else 1):
                raise BakeError(f"{file}: its key's variant is {header['variant']}, not the {world} world's")
            if world == "remodel" and header["world_id"] == "00" * 32:
                raise BakeError(f"{file}: a remodel bake keyed to no world_id (not the sidecar's geometry)")
            if world == "stock" and folder.name in world_ids:
                want = world_id_classic(world_ids[folder.name]["classic_world_id"], index)
                if header["world_id"] != want:
                    raise BakeError(f"{file}: keyed to world_id {header['world_id'][:16]}..., not this map "
                                    f"version's classic BSP {index} ({want[:16]}...)")
            # (the farm's walk, carried in the bake: from its file where the bake has no REAC record)
            try:
                walk = reach_payload(data, records)
                walk_file = folder / f"bsp{index}.reach"
                if reach and ((walk is None and walk_file.is_file()) or (walk is not None and
                                                                      not reach_first(data, records))):
                    data = with_reach(data, records, walk_file.read_bytes() if walk is None else None)
                    header, records, stock = check_bake(data, quality)
                    walk = reach_payload(data, records)
                walk_summary = read_reach(walk) if walk is not None else None
            except BakeError as error:
                raise BakeError(f"{file}: {error}") from error
            if require_reach and walk_summary is None:
                raise BakeError(f"{file}: no reach (no REAC record, no {walk_file.name}): a player's machine would "
                                "walk it at load")
            entries.append({
                "world": world, "map": folder.name, "bsp": index, "of": count, "source": file, "data": data,
                "file": f"{world}/{folder.name}/{file.name}", "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(), "header": header, "records": len(records),
                "key": manifest_key(header, RIG, world == "stock", note["tags"].get(index)),
            })
            if walk_summary is not None:
                entries[-1]["reach"] = walk_summary
        # (an empty BSP's key: the level's own settings, its geometry by kind)
        baked = [e for e in entries if not e.get("empty")]
        if not baked:
            raise BakeError(f"{folder.name}: every BSP is --empty: nothing to ship")
        for entry in entries:
            if entry.get("empty"):
                head = baked[0]["key"].split(" geo:")[0]
                entry["key"] = f"{head} geo:{'stock' if world == 'stock' else 'pwg'}"
        levels.append((folder.name, entries))
    shipped.mkdir(parents=True, exist_ok=True)
    manifest = manifest_load(shipped)
    for name, info in world_ids.items():
        if not maps or name in maps:
            manifest.setdefault("worlds", {})[name] = {"base_id": info["base_id"],
                                                       "classic_world_id": info["classic_world_id"]}
    exported = []
    for name, entries in levels:
        for entry in entries:
            if entry.get("empty"):
                continue
            target = shipped / entry["file"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(entry.pop("data"))
            entry.pop("source")
        manifest["bsps"] = [e for e in manifest["bsps"] if not (e["world"] == world and e["map"] == name)]
        manifest["bsps"].extend(entries)
        exported.extend(entries)
    manifest_save(shipped, manifest)
    if archives:
        write_archives(shipped, manifest, archives, {(e["world"], e["map"]) for e in exported})
    return exported


def write_archives(shipped: Path, manifest: dict, archives: Path, levels: set) -> list[Path]:
    archives.mkdir(parents=True, exist_ok=True)
    written = []
    for world, map_name in sorted(levels):
        entries = [e for e in manifest["bsps"] if e["world"] == world and e["map"] == map_name]
        version = next(e for e in entries if not e.get("empty"))["header"]["baker_version"]
        groups = [entries]
        if sum(e.get("size", 0) for e in entries) > ASSET_LIMIT:
            # (one archive a BSP; the empty ones ride with the first)
            baked = [e for e in entries if not e.get("empty")]
            groups = [[e] for e in baked]
            groups[0] = [e for e in entries if e.get("empty")] + groups[0]
        for group in groups:
            first = next(e for e in group if not e.get("empty"))
            suffix = "" if len(groups) == 1 else f"_bsp{first['bsp']}"
            path = archives / f"prism_bake_{map_name}_{world}_v{version}{suffix}.ppk"
            with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as tar:
                worlds = {k: v for k, v in manifest.get("worlds", {}).items() if k == map_name}
                part = json.dumps({"format": 1, "kind": "prism_bake_shipped", "bsps": group, "worlds": worlds},
                                  indent=1, sort_keys=True).encode("utf-8")
                info = tarfile.TarInfo("manifest.part.json")
                info.size = len(part)
                info.mtime = 0
                tar.addfile(info, io.BytesIO(part))
                for entry in group:
                    if entry.get("empty"):
                        continue
                    info = tar.gettarinfo(str(shipped / entry["file"]), arcname=entry["file"])
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    with (shipped / entry["file"]).open("rb") as file:
                        tar.addfile(info, file)
            if path.stat().st_size > 2 * 1024 ** 3:
                raise BakeError(f"{path} is over GitHub's 2 GiB asset limit")
            written.append(path)
    return written


def verify(shipped: Path) -> list[str]:
    manifest = manifest_load(shipped)
    problems = []
    levels: dict[tuple, list[dict]] = {}
    for entry in manifest["bsps"]:
        levels.setdefault((entry["world"], entry["map"]), []).append(entry)
    for (world, map_name), entries in sorted(levels.items()):
        counts = {e["of"] for e in entries}
        indices = sorted(e["bsp"] for e in entries)
        if len(counts) != 1 or indices != list(range(next(iter(counts)))):
            problems.append(f"{world}/{map_name}: not whole (BSPs {indices} of {sorted(counts)})")
    for entry in manifest["bsps"]:
        if entry.get("empty"):
            if entry.get("file"):
                problems.append(f"{entry['world']}/{entry['map']} BSP {entry['bsp']}: empty but names a file")
            continue
        path = shipped / entry["file"]
        if not path.is_file():
            problems.append(f"{entry['file']}: missing")
            continue
        data = path.read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            problems.append(f"{entry['file']}: its size or sha256 is not the manifest's")
            continue
        try:
            header, records, _ = check_bake(data)
            walk = reach_payload(data, records)
            walk_summary = read_reach(walk) if walk is not None else None
        except BakeError as error:
            problems.append(f"{entry['file']}: {error}")
            continue
        if header != entry["header"]:
            problems.append(f"{entry['file']}: its key is not the manifest's")
        if "reach" in entry and walk_summary != entry["reach"]:
            problems.append(f"{entry['file']}: its reach is not the manifest's")
    if not (shipped / "manifest.txt").is_file():
        problems.append("manifest.txt: missing")
    return problems


# the key's fields a plan file names as the header does (plan file name, header name)
PLAN_KEY_FIELDS = ("map_checksum", "map_name", "bsp_index", "bsp_name", "bsp_hash", "geometry_hash", "material_hash",
                   "light_hash", "settings_hash", "baker_version", "quality", "convergence_threshold", "world_id",
                   "scene_version", "variant")


def read_plan(path: Path) -> dict:
    """a plan-only run's file (xgpu_vulkan_bake.c's plan_only_write: "name value" lines), its key as read_header
    gives a header's"""
    raw = {}
    for line in path.read_text("utf-8", "replace").splitlines():
        name, _, value = line.partition(" ")
        if name:
            raw[name] = value.strip()
    try:
        quality = int(raw["quality"])
        key = {
            "map_checksum": int(raw["map_checksum"], 16), "map_name": raw["map_name"],
            "bsp_index": int(raw["bsp_index"]), "bsp_name": raw["bsp_name"],
            "bsp_hash": raw["bsp_hash"], "geometry_hash": raw["geometry_hash"], "material_hash": raw["material_hash"],
            "light_hash": raw["light_hash"], "settings_hash": raw["settings_hash"],
            "baker_version": int(raw["baker_version"]),
            "quality": QUALITIES[quality] if quality < len(QUALITIES) else str(quality),
            "convergence_threshold": round(struct.unpack("<f", struct.pack("<I", int(raw["threshold"], 16)))[0], 6),
            "world_id": raw["world_id"], "scene_version": int(raw["scene_version"]), "variant": int(raw["variant"]),
        }
        tiles = int(raw["tiles"])
    except (KeyError, ValueError) as error:
        raise BakeError(f"{path}: not a plan-only run's file ({error})") from error
    walk = None
    if "reach_input" in raw:
        walk = {"input": raw["reach_input"], "bricks": int(raw["reach_bricks"]),
                "bricks_crc": raw["reach_bricks_crc"], "tiers": int(raw["reach_tiers"]),
                "tiers_crc": raw["reach_tiers_crc"], "flight": int(raw.get("reach_flight", "0"))}
    return {"key": key, "tiles": tiles, "reach": walk, "charts": raw.get("charts")}


def plancheck(shipped: Path, plans: Path, world: str | None = None, maps: list[str] | None = None) -> list[str]:
    """each shipped BSP against a player's plan of it (read_plan): its whole key, its tiles, its REAC walk"""
    manifest = manifest_load(shipped)
    problems = []
    checked = 0
    for entry in manifest["bsps"]:
        if entry.get("empty") or (world and entry["world"] != world) or (maps and entry["map"] not in maps):
            continue
        name = f"{entry['world']}/{entry['map']} BSP {entry['bsp']}"
        plan_path = plans / entry["map"] / f"plan_bsp{entry['bsp']}.txt"
        if not plan_path.is_file():
            problems.append(f"{name}: no player's plan ({plan_path})")
            continue
        try:
            plan = read_plan(plan_path)
            data = (shipped / entry["file"]).read_bytes()
            header, records, _ = check_bake(data)
            walk = reach_payload(data, records)
            walk_summary = read_reach(walk) if walk is not None else None
        except (BakeError, OSError) as error:
            problems.append(f"{name}: {error}")
            continue
        checked += 1
        differ = [f for f in PLAN_KEY_FIELDS if header[f] != plan["key"][f]]
        if differ:
            problems.append(f"{name}: the player's key differs in " + ", ".join(
                f"{f} (bake {header[f]}, player {plan['key'][f]})" for f in differ))
        tiles = sum(1 for r in records if r["type"] == fourcc("ATLS"))
        if tiles != plan["tiles"]:
            problems.append(f"{name}: {tiles} tiles baked, the player's plan has {plan['tiles']}")
        if walk_summary is not None:
            if plan["reach"] is None:
                problems.append(f"{name}: the bake carries a reach, the player's plan names none to check it by")
            else:
                differ = [f for f in ("input", "bricks", "bricks_crc", "tiers", "tiers_crc", "flight")
                          if walk_summary[f] != plan["reach"][f]]
                if differ:
                    problems.append(f"{name}: its reach is not the player's walk (" + ", ".join(
                        f"{f} {walk_summary[f]} / {plan['reach'][f]}" for f in differ) + ")")
    if not checked and not problems:
        problems.append("no shipped BSP checked")
    return problems


def install(shipped: Path, archives: list[Path]) -> list[str]:
    shipped.mkdir(parents=True, exist_ok=True)
    root = shipped.resolve()
    manifest = manifest_load(shipped)
    for archive in archives:
        with tarfile.open(archive, "r") as tar:
            part = None
            for member in tar.getmembers():
                target = (shipped / member.name).resolve()
                if not member.isfile() or (root not in target.parents and target != root) or \
                        not re.fullmatch(r"(stock|remodel)/[A-Za-z0-9_-]+/bsp\d+\.pbk|manifest\.part\.json", member.name):
                    raise BakeError(f"{archive}: {member.name} is not a shipped bake's file")
                source = tar.extractfile(member)
                if member.name == "manifest.part.json":
                    part = json.loads(source.read().decode("utf-8"))
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("wb") as out:
                    shutil.copyfileobj(source, out)
            if not part:
                raise BakeError(f"{archive}: no manifest.part.json")
            names = {(e["world"], e["map"], e["bsp"]) for e in part["bsps"]}
            manifest["bsps"] = [e for e in manifest["bsps"] if (e["world"], e["map"], e["bsp"]) not in names]
            manifest["bsps"].extend(part["bsps"])
            manifest.setdefault("worlds", {}).update(part.get("worlds", {}))
    manifest_save(shipped, manifest)
    return verify(shipped)


def show(path: Path) -> None:
    data = path.read_bytes()
    header = read_header(data)
    print(json.dumps(header, indent=1))
    counts: dict[str, int] = {}
    for record in read_records(data):
        counts[type_name(record["type"])] = counts.get(type_name(record["type"]), 0) + 1
    print(json.dumps(counts, indent=1, sort_keys=True))
    walk = reach_payload(data, read_records(data))
    if walk is not None:
        print(json.dumps({"reach": read_reach(walk)}, indent=1, sort_keys=True))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("export")
    p.add_argument("bake_folder", type=Path)
    p.add_argument("world", choices=("stock", "remodel"))
    p.add_argument("shipped", type=Path)
    p.add_argument("--maps", default="")
    p.add_argument("--quality", default="max", choices=QUALITIES,
                   help="the least quality shipped (a release's is max; a test pack's may be draft)")
    p.add_argument("--bsps", default="", help="each level's BSP count, map:N[,map:N] (a bare N with one --maps)")
    p.add_argument("--maps-folder", type=Path, help="the retail maps, each level's BSP count read from its map")
    p.add_argument("--empty", default="", help="BSPs with nothing to bake (no lightmapped surface), map:index[,...]")
    p.add_argument("--archives", type=Path)
    p.add_argument("--world-ids", type=Path, help="tools/geopatch/classic_world.py's json: each map's base_id and "
                   "classic_world_id")
    p.add_argument("--reach", action="store_true", help="each BSP's reach (<map>/bsp<n>.reach) carried in its bake "
                   "as a REAC record where it has none")
    p.add_argument("--require-reach", action="store_true", help="refuse a BSP whose bake carries no reach")
    p = commands.add_parser("verify")
    p.add_argument("shipped", type=Path)
    p = commands.add_parser("install")
    p.add_argument("shipped", type=Path)
    p.add_argument("archives", type=Path, nargs="+")
    p = commands.add_parser("plancheck")
    p.add_argument("shipped", type=Path)
    p.add_argument("plans", type=Path, help="the players' plans: <plans>/<map>/plan_bsp<n>.txt (HALO_BAKE_PLAN_ONLY)")
    p.add_argument("--world", choices=("stock", "remodel"))
    p.add_argument("--maps", default="")
    p = commands.add_parser("show")
    p.add_argument("file", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            entries = export(args.bake_folder, args.world, args.shipped,
                             [m for m in args.maps.split(",") if m] or None, args.quality, args.archives,
                             args.bsps, args.maps_folder, args.empty,
                             json.loads(args.world_ids.read_text()) if args.world_ids else None,
                             args.reach, args.require_reach)
            for entry in entries:
                if entry.get("empty"):
                    print(f"{entry['world']}/{entry['map']} BSP {entry['bsp']}: nothing to bake, {entry['key']}")
                else:
                    print(f"{entry['file']}: {entry['size']} bytes, {entry['header']['quality']}, {entry['key']}")
            problems = verify(args.shipped)
        elif args.command == "verify":
            problems = verify(args.shipped)
        elif args.command == "install":
            problems = install(args.shipped, args.archives)
        elif args.command == "plancheck":
            problems = plancheck(args.shipped, args.plans, args.world, [m for m in args.maps.split(",") if m] or None)
            if not problems:
                print("prism_bake_ship: every shipped BSP is the player's plan")
        else:
            show(args.file)
            return 0
    except BakeError as error:
        print(f"prism_bake_ship: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(f"prism_bake_ship: {problem}", file=sys.stderr)
    if not problems:
        print("prism_bake_ship: verified")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
