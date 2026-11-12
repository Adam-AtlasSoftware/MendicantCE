#!/usr/bin/env python3
"""fork: refuses a package of this fork that could update itself to, report
crashes to, or pass for upstream OpenH1.

    python3 tools/fork_gate.py <package folder or file>...

Every file that is not a document is read whole; the run fails when one holds
an address or a file name of upstream's releases or crash reports (as bytes,
and as the UTF-16 of the Windows host's wide strings), or when none of them
names this fork. The release workflow runs it on each package before it is
published, so a build made without the fork's switch (tools/build_common.py's
FORK_DEFINE), whatever its build number, cannot be the download.

It also refuses a package that holds the game's data or anything made from it
(check_game_data): a file that begins as a map, a tag, a disc image, one of
PRISM's packs' files (.pmesh, .pwg, .prl, .psm, lightmap caches, probes, KTX2
textures) or a model or scene (glTF, a Blender file, numpy arrays) does; and in
PRISM's pack tools (a package's prism_packs folder: tools/prism_packs) only
code, recipes and text may be, each small, and no recipe a table of numbers
long enough to be a mesh's or a picture's.
"""

import json
import re
import sys
from pathlib import Path
from typing import Iterable, List

# what only a build with upstream's updater or crash reporter holds
# (port/linux/src/updater.c, port/windows/host/host_crash_win.c)
FORBIDDEN = ["/releases/latest", "OpenH1-Linux", "OpenH1-Windows", "OpenH1-Steam-Frame", "ingest.de.sentry.io",
             "/OpenH1/OpenH1/releases/download"]
# what a build with the switch holds (source/main/main.c's label)
REQUIRED = "MendicantCE %s | "
# the documents (upstream's own, which name its files) are not the build
DOCUMENTS = {".md", ".txt", ".toml", ".desktop", ".sh", ".json", ".vdf", ".pub", ".xml"}


def encodings(text: str) -> List[bytes]:
    return [text.encode("ascii"), text.encode("utf-16-le")]


def files_of(paths: Iterable[Path]) -> List[Path]:
    found: List[Path] = []
    for path in paths:
        if path.is_dir():
            found += sorted(entry for entry in path.rglob("*") if entry.is_file() and not entry.is_symlink())
        elif path.is_file():
            found.append(path)
    return [path for path in found if path.suffix.lower() not in DOCUMENTS]


def check(paths: Iterable[Path]) -> List[str]:
    """the reasons the files are refused; none when they may be published"""
    problems: List[str] = []
    named = False
    files = files_of(paths)
    if not files:
        return ["no files to check"]
    for path in files:
        data = path.read_bytes()
        for text in FORBIDDEN:
            if any(encoded in data for encoded in encodings(text)):
                problems.append(f"{path}: holds \"{text}\": a build with upstream's updater or crash reports")
        if any(encoded in data for encoded in encodings(REQUIRED)):
            named = True
    if not named:
        problems.append(f"no file holds \"{REQUIRED}\": not a build of this fork (HALO_FORK_MENDICANT)")
    return problems


# the first bytes of the game's data and of what is made from it, at an offset: maps (Xbox and PC), tags
# ("blam" ends a tag file's header), an Xbox disc image, the packs' files (tools/pmesh.py, prism_world_geometry.c,
# prism_lightmaps.c, prism_light.c, world_sdf.c), textures, models and scenes, numpy's arrays
GAME_DATA = [(0, b"daeh"), (0, b"head"), (60, b"blam"), (0x10000, b"MICROSOFT*XBOX*MEDIA"), (0, b"PMSH"),
             (0, b"PWG1"), (0, b"PRL1"), (0, b"PSM1"), (0, b"PLM1"), (0, b"PRBS"), (0, b"WSDF"),
             (0, b"\xabKTX 20\xbb"), (0, b"DDS "), (0, b"glTF"), (0, b"BLENDER"), (0, b"\x93NUMPY"), (0, b"8200\n"),
             (0, b"8200\r\n")]
# PRISM's pack tools in a package: the folder, what may be in it, and how big
PACK_TOOLS = "prism_packs"
PACK_TOOL_SUFFIXES = {".py", ".json", ".md", ".txt"}
PACK_TOOL_MAX_BYTES = 256 * 1024
# a recipe may name places and sizes (a few numbers a feature), never carry a mesh or a picture
RECIPE_MAX_NUMBERS = 4096
RECIPE_MAX_RUN = 64
NUMBER_RUN = re.compile(r"(?:-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?\s*,\s*){%d,}" % RECIPE_MAX_RUN)


def _numbers(value) -> List[int]:
    """(the count of numbers in a JSON value, the longest list of numbers in it)"""
    if isinstance(value, bool) or value is None or isinstance(value, str):
        return [0, 0]
    if isinstance(value, (int, float)):
        return [1, 0]
    items = list(value.values()) if isinstance(value, dict) else list(value)
    counts = [_numbers(item) for item in items]
    run = sum(1 for item in items if isinstance(item, (int, float)) and not isinstance(item, bool)) \
        if isinstance(value, list) else 0
    return [sum(c[0] for c in counts), max([run] + [c[1] for c in counts])]


def check_game_data(paths: Iterable[Path]) -> List[str]:
    """the reasons the files hold the game's data or what is made from it; none when they hold neither"""
    problems: List[str] = []
    for root in paths:
        files = sorted(entry for entry in root.rglob("*") if entry.is_file()) if root.is_dir() else [root]
        for path in files:
            with open(path, "rb") as file:
                head = file.read(0x10000 + 32)
            for offset, magic in GAME_DATA:
                if head[offset:offset + len(magic)] == magic:
                    problems.append(f"{path}: begins as the game's data or a pack's file ({magic!r})")
                    break
            parts = path.relative_to(root).parts if root.is_dir() else path.parts
            if PACK_TOOLS not in parts[:-1]:
                continue
            if path.suffix.lower() not in PACK_TOOL_SUFFIXES:
                problems.append(f"{path}: not code, a recipe or text: the pack tools carry nothing else")
                continue
            if path.stat().st_size > PACK_TOOL_MAX_BYTES:
                problems.append(f"{path}: {path.stat().st_size} bytes, more than a pack tool's file is")
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                problems.append(f"{path}: not text")
                continue
            if path.suffix.lower() == ".json":
                try:
                    count, run = _numbers(json.loads(text))
                except ValueError:
                    problems.append(f"{path}: not JSON")
                    continue
                if count > RECIPE_MAX_NUMBERS or run > RECIPE_MAX_RUN:
                    problems.append(f"{path}: {count} numbers, {run} in one list: a table, not a recipe")
            elif NUMBER_RUN.search(text):
                problems.append(f"{path}: a list of more than {RECIPE_MAX_RUN} numbers: a table, not code")
    return problems


def main(arguments: List[str]) -> int:
    if not arguments:
        print(__doc__)
        return 2
    paths = [Path(argument) for argument in arguments]
    problems = check(paths) + check_game_data(paths)
    for problem in problems:
        print("fork gate: " + problem)
    if not problems:
        print("fork gate: passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
