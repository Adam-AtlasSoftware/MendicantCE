#!/usr/bin/env python3
"""PRISM's shipped bake (docs/prism_bake.md, "The shipped bake"): a release's
light, every campaign level baked at MAX for both worlds (the stock and the
remodel), lighting only, which the game uses wherever its key is the
player's own BSP's and bakes nothing.

    export <bake folder> <stock|remodel> <shipped folder> [--maps b30,a30]
           [--quality max] [--archives <folder>]
        copies each finished BSP bake (<map>/bsp<n>.pbk) of the bake folder
        into <shipped>/<world>/<map>/, after checking that it is whole (a
        DONE record), of the quality asked or better, of this baker's file
        version, and lighting only (no record of a kind that is not light:
        see LIGHT_RECORDS); writes manifest.txt (the game's: each BSP's
        settings key, prism_bake_menu.c's) and manifest.json (the files'
        sizes and sha256, each BSP's per-BSP input hashes) and, with
        --archives, a release asset per level and world
        (prism_bake_<map>_<world>_v<baker>.ppk: an uncompressed tar of that
        level's files and its manifest lines; the records are packed
        already), each under GitHub's 2 GiB asset limit.
    verify <shipped folder>
        every file manifest.json names is there, of its size and sha256, a
        whole lighting-only bake whose header matches its entry.
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
    "PLAN", "DONE", "ATLS", "ATLF", "ATLY", "MASK", "VOLB", "VOLY", "PROB", "IMPG", "REAC", "FOOT", "LYRB")}
GEOMETRY_RECORDS = {fourcc(name): name for name in ("ATLX", "CKPT")}


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
    return {
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
                        "size": unpacked or stored, "packed": unpacked != 0, "offset": start})
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


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def noted_tags(bake_folder: Path, map_name: str) -> dict[int, str]:
    """each BSP's tag as the main menu's note (baked.txt) has it, where it does"""
    tags = {}
    note = bake_folder / map_name / "baked.txt"
    if note.is_file():
        for line in note.read_text("utf-8", "replace").splitlines():
            match = re.match(r"bsp(\d+) .* geo:(?:stock|pwg):(.*)$", line)
            if match:
                tag = match.group(2)
                if " geo:pwg:" in line:
                    tag = tag.split(":", 1)[0]
                tags[int(match.group(1))] = tag
    return tags


def manifest_key(header: dict, rig: str, stock: bool, tag: str | None) -> str:
    """the BSP's settings key as the game's note keeps it (prism_bake_menu.c's
    prism_bake_key; its quality the file's, which is good for every quality
    under it), its geometry by kind ("geo:stock", "geo:pwg") and its tag
    where the note gave it"""
    geometry = "geo:stock" if stock else "geo:pwg"
    return f"v{header['baker_version']} {header['quality']} {rig} cutouts {geometry}" + (f":{tag}" if tag else "")


def manifest_load(shipped: Path) -> dict:
    path = shipped / "manifest.json"
    if path.is_file():
        return json.loads(path.read_text("utf-8"))
    return {"format": 1, "kind": "prism_bake_shipped", "bsps": []}


def manifest_save(shipped: Path, manifest: dict) -> None:
    manifest["bsps"].sort(key=lambda e: (e["world"], e["map"], e["bsp"]))
    (shipped / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n", "utf-8")
    lines = ["prism_bake_shipped 1"]
    for entry in manifest["bsps"]:
        lines.append(f"bsp {entry['world']} {entry['map']} {entry['bsp']} {entry['of']} {entry['key']}")
    (shipped / "manifest.txt").write_text("\n".join(lines) + "\n", "utf-8")


def export(bake_folder: Path, world: str, shipped: Path, maps: list[str] | None, quality: str, rig: str,
           archives: Path | None) -> list[dict]:
    if world not in ("stock", "remodel"):
        raise BakeError("the world is stock or remodel")
    shipped.mkdir(parents=True, exist_ok=True)
    manifest = manifest_load(shipped)
    exported = []
    folders = sorted(p for p in bake_folder.iterdir() if p.is_dir() and (not maps or p.name in maps))
    for folder in folders:
        files = sorted(folder.glob("bsp*.pbk"), key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
        files = [f for f in files if re.fullmatch(r"bsp\d+\.pbk", f.name)]
        if not files:
            continue
        tags = noted_tags(bake_folder, folder.name)
        entries = []
        for file in files:
            data = file.read_bytes()
            header, records, stock = check_bake(data, quality)
            if world == "stock" and stock is False:
                raise BakeError(f"{file}: a bake of the remodel, exported as the stock world's")
            index = int(file.stem[3:])
            target = shipped / world / folder.name / file.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, target)
            entries.append({
                "world": world, "map": folder.name, "bsp": index, "file": f"{world}/{folder.name}/{file.name}",
                "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "header": header,
                "records": len(records),
                "key": manifest_key(header, rig, stock if stock is not None else world == "stock", tags.get(index)),
            })
        of = max(e["bsp"] for e in entries) + 1
        for entry in entries:
            entry["of"] = of
        manifest["bsps"] = [e for e in manifest["bsps"] if not (e["world"] == world and e["map"] == folder.name)]
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
        version = entries[0]["header"]["baker_version"]
        groups = [entries]
        if sum(e["size"] for e in entries) > ASSET_LIMIT:
            groups = [[e] for e in entries]
        for group in groups:
            suffix = "" if len(groups) == 1 else f"_bsp{group[0]['bsp']}"
            path = archives / f"prism_bake_{map_name}_{world}_v{version}{suffix}.ppk"
            with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as tar:
                part = json.dumps({"format": 1, "kind": "prism_bake_shipped", "bsps": group}, indent=1,
                                  sort_keys=True).encode("utf-8")
                info = tarfile.TarInfo("manifest.part.json")
                info.size = len(part)
                info.mtime = 0
                tar.addfile(info, io.BytesIO(part))
                for entry in group:
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
    for entry in manifest["bsps"]:
        path = shipped / entry["file"]
        if not path.is_file():
            problems.append(f"{entry['file']}: missing")
            continue
        data = path.read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            problems.append(f"{entry['file']}: its size or sha256 is not the manifest's")
            continue
        try:
            header, _, _ = check_bake(data)
        except BakeError as error:
            problems.append(f"{entry['file']}: {error}")
            continue
        if header != entry["header"]:
            problems.append(f"{entry['file']}: its key is not the manifest's")
    if not (shipped / "manifest.txt").is_file():
        problems.append("manifest.txt: missing")
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
            files = {e["file"] for e in part["bsps"]}
            manifest["bsps"] = [e for e in manifest["bsps"] if e["file"] not in files]
            manifest["bsps"].extend(part["bsps"])
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
    p.add_argument("--rig", default="units", choices=("units", "fitted"))
    p.add_argument("--archives", type=Path)
    p = commands.add_parser("verify")
    p.add_argument("shipped", type=Path)
    p = commands.add_parser("install")
    p.add_argument("shipped", type=Path)
    p.add_argument("archives", type=Path, nargs="+")
    p = commands.add_parser("show")
    p.add_argument("file", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            entries = export(args.bake_folder, args.world, args.shipped,
                             [m for m in args.maps.split(",") if m] or None, args.quality, args.rig, args.archives)
            for entry in entries:
                print(f"{entry['file']}: {entry['size']} bytes, {entry['header']['quality']}, {entry['key']}")
            problems = verify(args.shipped)
        elif args.command == "verify":
            problems = verify(args.shipped)
        elif args.command == "install":
            problems = install(args.shipped, args.archives)
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
