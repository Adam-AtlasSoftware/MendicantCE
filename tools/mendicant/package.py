#!/usr/bin/env python3
"""fork: checks a release build of MendicantCE and zips it as the download a player takes.

    python3 tools/mendicant/package.py zip linux-x86_64 dist/halo-linux-x86_64-release dist/release
    python3 tools/mendicant/package.py zip windows-x64 dist/halo-windows-release dist/release
    python3 tools/mendicant/package.py sums dist/release

zip takes the folder tools/ci_build.py <platform> release leaves in dist/ (the game as OpenH1's release lays it
out: the Linux tree of tools/linux_package.py, the Windows folder with halo.exe at its top) and writes
<output folder>/MendicantCE-PRISM-<platform>.zip, the folder's files at the zip's top with their Unix modes.
The build is made by tools/ci_build.py and nothing is added to it here: this script refuses a folder that is
not a whole package of the fork, and so is what stands between a build and a download. It refuses a folder

- that lacks one of the game's files, MENDICANT AI's trained models (the repository's
  port/assets/mendicant/models.aibw, as mendicant_models.aibw, where the game looks for them:
  port/linux/src/mendicant_shipped.h), the player's README (MENDICANT.md, as README.md), the AGPL's text, a
  third party's notice (tools/linux_package.py's notices) or the credits;
- whose SOURCE.txt does not name the commit the build was made from and where to read it (the AGPL's sections
  5 and 6), or names another commit than the workflow's (GITHUB_SHA);
- that tools/fork_gate.py refuses (a build that could update itself to, or pass for, OpenH1);
- that holds anything a player's run leaves or anything of the game's data: maps, saves, settings, logs, disc
  images, mods, PRISM's asset packs (fork_gate.py's check_game_data reads every file's first bytes for them).
  The packs are made from the game's data on their maker's computer and are in no download; a download plays
  from the player's own game with none;
- that lacks PRISM's pack tools (tools/prism_packs, with the player's guide as GUIDE.md: they build the packs
  from the player's own game data on the player's computer), or whose tools folder holds anything but code,
  recipes and text (fork_gate.py: no recipe a table of numbers long enough to be a mesh or a picture). The
  Linux package carries their extractor, bin/openh1-map-extract; the Windows package's tools run that one
  through WSL.

sums writes SHA256SUMS in the folder, of the downloads in it (sha256sum -c SHA256SUMS checks them).

The older port's script of this name (the branch main) built a mod folder around a build of another base; this
one replaces it.
"""

import argparse
import hashlib
import os
import re
import sys
import zipfile
from pathlib import Path
from typing import List

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import fork_gate  # noqa: E402
import linux_package  # noqa: E402

ZIP_NAME = "MendicantCE-PRISM-{platform}.zip"
SUMS_NAME = "SHA256SUMS"
DOCUMENT_NAMES = [name for _source, name in linux_package.DOCUMENTS] + [linux_package.SOURCE_NOTICE]

# each platform's package: tools/ci_build.py's name for its build (whose notices are linux_package.notices'),
# the folders its documents, licences and game files are in, the AGPL's name, and the game's own files
PLATFORMS = {
    "linux-x86_64": {
        "build": "linux_x86_64",
        "documents": "share/doc/openh1",
        "licenses": "share/licenses/openh1",
        "license": "LICENSE",
        "models": "share/openh1/" + linux_package.MENDICANT_MODELS_NAME,
        "notices": ["SDL3-LICENSE.txt", linux_package.MAP_EXTRACT_LGPL[1], linux_package.MAP_EXTRACT_NOTICE],
        "files": ["bin/openh1", "lib/openh1/libSDL3.so.0", "share/openh1/openh1.assets",
                  "share/openh1/prepared.records", "share/openh1/brokers.txt", "share/openh1/masters.txt",
                  "share/openh1/relays.txt", "share/applications/openh1.desktop",
                  "bin/" + linux_package.MAP_EXTRACT_NAME],
        "tools": "share/openh1/" + linux_package.PACK_TOOLS_NAME,
        "top": {"bin", "lib", "share"},
    },
    "windows-x64": {
        "build": "windows",
        "documents": "",
        "licenses": "",
        "license": "LICENSE.txt",
        "models": linux_package.MENDICANT_MODELS_NAME,
        "notices": [],
        "files": ["halo.exe", "SDL3.dll", "openh1.assets", "prepared.records", "brokers.txt", "masters.txt",
                  "relays.txt"],
        "tools": linux_package.PACK_TOOLS_NAME,
        "top": None,  # (every file is at the top: no folders but the pack tools')
    },
}

# what no download holds: the game's data and what is made from it (PRISM's packs among it), and what a
# player's run leaves in the game's folder (names are compared in lower case)
FORBIDDEN_FOLDERS = {"maps", "custom_maps", "bink", "mods", "saves", "savegames", "profiles", "replays", "tags",
                     "prism", "prism_lightmaps", "prism_world", "textures", "meshes", "materials", "cache",
                     "screenshots", "recordings", "training"}
FORBIDDEN_SUFFIXES = {".map", ".iso", ".xiso", ".bik", ".sav", ".log", ".dmp", ".probes", ".prl", ".pwg", ".pth",
                      ".pt", ".replay", ".bitmaps", ".sounds", ".pdb", ".zip", ".psm", ".plm", ".pmesh", ".ktx2",
                      ".wsdf", ".gltf", ".glb", ".jms", ".jma", ".npz", ".npy", ".blend", ".bitmap", ".gbxmodel",
                      ".model", ".safetensors", ".onnx"}
FORBIDDEN_NAMES = {"config.toml", "debug.txt", "halo.log", "halo.previous.log"}


def join(folder: str, name: str) -> str:
    return f"{folder}/{name}" if folder else name


def problems_of(platform: str, tree: Path) -> List[str]:
    """the reasons tree is not a package of the fork for platform; none when it may be zipped"""
    layout = PLATFORMS[platform]
    problems: List[str] = []
    if not tree.is_dir():
        return [f"{tree} is not a folder: build it with tools/ci_build.py {layout['build']} release first"]
    names = sorted(path.relative_to(tree).as_posix() for path in tree.rglob("*") if path.is_file())
    present = set(names)

    required = list(layout["files"]) + [layout["models"], join(layout["licenses"], layout["license"])]
    # fork: PRISM's pack tools, as linux_package.stage_pack_tools lays them out
    required += [join(layout["tools"], name) for _source, name in linux_package.pack_tool_files()]
    required += [join(layout["documents"], name) for name in DOCUMENT_NAMES]
    required += [join(layout["licenses"], name) for _source, name in linux_package.notices(layout["build"])]
    required += [join(layout["licenses"], name) for name in layout["notices"]]
    for name in required:
        if name not in present:
            problems.append(f"{name} is missing")
        elif (tree / name).stat().st_size == 0 and not name.endswith("relays.txt"):
            problems.append(f"{name} is empty")

    # the files that are the repository's own, byte for byte: the trained models, the player's README (and
    # OpenH1's beside it), the licence and the notices
    same = [(linux_package.MENDICANT_MODELS, layout["models"]),
            (linux_package.PROJECT_LICENSE, join(layout["licenses"], layout["license"]))]
    same += [(source, join(layout["tools"], name)) for source, name in linux_package.pack_tool_files()]
    same += [(source, join(layout["documents"], name)) for source, name in linux_package.DOCUMENTS]
    same += [(source, join(layout["licenses"], name)) for source, name in linux_package.notices(layout["build"])]
    for source, name in same:
        if name in present and (tree / name).read_bytes() != source.read_bytes():
            problems.append(f"{name} is not the repository's {source.relative_to(ROOT).as_posix()}")
    models = tree / layout["models"]
    if models.is_file() and models.read_bytes()[:4] != b"AIBW":
        problems.append(f"{layout['models']} is not a models file")

    # where the source of exactly this build is
    notice = tree / join(layout["documents"], linux_package.SOURCE_NOTICE)
    if notice.is_file():
        text = notice.read_text()
        named = re.search(r"^  commit      ([0-9a-f]{40})$", text, re.MULTILINE)
        if not named:
            problems.append(f"{linux_package.SOURCE_NOTICE} names no commit: the build is of a tree with changes "
                            "in no commit, and is not a download")
        else:
            commit = named.group(1)
            if f"{linux_package.FORK_URL}/tree/{commit}" not in text:
                problems.append(f"{linux_package.SOURCE_NOTICE} does not say where commit {commit} is read")
            expected = os.environ.get("GITHUB_SHA", "")
            if expected and commit != expected:
                problems.append(f"{linux_package.SOURCE_NOTICE} names commit {commit}, this run's is {expected}")
        if "GNU Affero General Public License" not in text:
            problems.append(f"{linux_package.SOURCE_NOTICE} does not name the licence")

    # nothing of the game's data, and nothing a run leaves
    for name in names:
        parts = name.lower().split("/")
        if set(parts[:-1]) & FORBIDDEN_FOLDERS or parts[-1] in FORBIDDEN_NAMES or \
                Path(parts[-1]).suffix in FORBIDDEN_SUFFIXES:
            problems.append(f"{name} is not a file of the build: game data, a pack, or what a run leaves")
    tops = {name.split("/")[0] for name in names}
    if layout["top"] is not None and tops != layout["top"]:
        problems.append(f"the top of the tree is {sorted(tops)}, not {sorted(layout['top'])}")
    if layout["top"] is None and any("/" in name and name.split("/")[0] != layout["tools"] for name in names):
        problems.append("the folder has folders in it: " + ", ".join(
            sorted(name for name in names if "/" in name and name.split("/")[0] != layout["tools"])))
    # (and the pack tools' folder holds the repository's files and no other)
    expected = {join(layout["tools"], name) for _source, name in linux_package.pack_tool_files()}
    for name in names:
        if name.startswith(layout["tools"] + "/") and name not in expected:
            problems.append(f"{name} is not one of the pack tools' files")

    problems += fork_gate.check([tree])
    problems += fork_gate.check_game_data([tree])
    return problems


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package(platform: str, tree: Path, output: Path) -> int:
    problems = problems_of(platform, tree)
    for problem in problems:
        print(f"package: {problem}")
    if problems:
        print(f"package: {tree} is not zipped")
        return 1
    archive = output / ZIP_NAME.format(platform=platform)
    if archive.exists():
        archive.unlink()
    linux_package.write_zip(tree, archive)
    # the zip read back: whole, the folder's files and no others, the executables still executable
    with zipfile.ZipFile(archive) as written:
        damaged = written.testzip()
        if damaged:
            print(f"package: {archive}: {damaged} is damaged")
            return 1
        entries = {info.filename: info for info in written.infolist() if not info.is_dir()}
    names = {path.relative_to(tree).as_posix() for path in tree.rglob("*") if path.is_file()}
    if set(entries) != names:
        print(f"package: {archive} does not hold the folder's files")
        return 1
    if platform == "linux-x86_64" and (entries["bin/openh1"].external_attr >> 16) & 0o111 != 0o111:
        print(f"package: {archive}: bin/openh1 is not executable")
        return 1
    print(f"{archive.name}: {len(entries)} files, {archive.stat().st_size} bytes, sha256 {sha256(archive)}")
    return 0


def sums(folder: Path) -> int:
    archives = [folder / ZIP_NAME.format(platform=platform) for platform in sorted(PLATFORMS)]
    missing = [archive.name for archive in archives if not archive.is_file()]
    if missing:
        print(f"package: {folder} has no {', '.join(missing)}")
        return 1
    # (sha256sum's lines for files read as bytes: two spaces; LF ends, on Windows too)
    text = "".join(f"{sha256(archive)}  {archive.name}\n" for archive in archives)
    (folder / SUMS_NAME).write_bytes(text.encode("ascii"))
    sys.stdout.write(text)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    zip_command = commands.add_parser("zip", help="check a release build's folder and zip it")
    zip_command.add_argument("platform", choices=sorted(PLATFORMS))
    zip_command.add_argument("tree", type=Path, help="tools/ci_build.py's folder in dist/")
    zip_command.add_argument("output", type=Path, help="the folder the zip is written in")
    sums_command = commands.add_parser("sums", help=f"write {SUMS_NAME} of the folder's downloads")
    sums_command.add_argument("folder", type=Path)
    args = parser.parse_args()
    if args.command == "zip":
        return package(args.platform, args.tree, args.output)
    return sums(args.folder)


if __name__ == "__main__":
    sys.exit(main())
