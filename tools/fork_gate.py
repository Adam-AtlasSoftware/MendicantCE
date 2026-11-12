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
"""

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


def main(arguments: List[str]) -> int:
    if not arguments:
        print(__doc__)
        return 2
    problems = check(Path(argument) for argument in arguments)
    for problem in problems:
        print("fork gate: " + problem)
    if not problems:
        print("fork gate: passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
