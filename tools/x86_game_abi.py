#!/usr/bin/env python3
"""The game's 32-bit x86 ABI on Linux, for tests and developer tools only:
nothing built with it ships (the Linux builds are 64-bit, the game an ILP32
guest in them: tools/linux_guest_build.py).

Some checks compile parts of the engine natively, where the game's 32-bit
data structures need 32-bit pointers: the asset-free harness
(tools/harness, -m32), the SDK declarations' compile check, internet play's
signatures (tools/p2p_lobby_check.c) and the tag validator alone on map
files (tools/map_validate.c). They need clang and the C library's 32-bit
files (gcc-multilib on Debian and Ubuntu, lib32-glibc on Arch Linux), on
64-bit x86 Linux.

    python tools/x86_game_abi.py [folder]

builds the tag validator's program, <folder>/map_validate (default:
build/map_validate), which tools/test_linux_port.py runs on the maps:

    build/map_validate/map_validate [--strict] map.map...

fork: and with --map-extract, PRISM's pack tools' extractor,
<folder>/map_extract (default: build/map_extract), which the fork's Linux
package carries as bin/openh1-map-extract (tools/prism_packs/README.md):

    python tools/x86_game_abi.py --map-extract [folder]
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import List, Optional, Sequence

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.build_common import (MONOCYPHER_DIR, OWN_CODE_ERRORS, QUIET_OWN_CODE_FLAGS, TOML_DIR,  # noqa: E402
                                EXPAT_DIR, KCP_DIR, XDK_INCLUDE, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES,
                                game_defines_and_includes)

# Flags shared by the game and the XDK-facing half of the platform layer.
# They reproduce the MSVC/Xbox ABI the source was written against:
#  - 16-bit wchar_t (UTF-16 strings in tag data and saved games),
#  - MSVC struct layout for 64-bit members (-malign-double),
#  - __declspec, __int64 and calling conventions,
#  - C89 with tentative definitions shared between units (-fcommon),
#  - small structures and unions returned in EAX:EDX, as Win32 does,
#  - no optimisations that assume the absence of MSVC-tolerated UB,
#  - no fused multiply-adds (port/include/halo_math.h).
GAME_ABI_FLAGS = [
    "--target=i686-linux-gnu",
    "-m32",
    "-march=x86-64",
    "-fms-extensions",
    "-fshort-wchar",
    "-malign-double",
    "-fcommon",
    "-fno-pic",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-freg-struct-return",
    "-fno-omit-frame-pointer",
    "-ffp-contract=off",
    "-O2",
    "-g",
    # glibc's wide string functions assume a 32-bit wchar_t; stop clang from
    # turning loops into calls to them
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

# The game, with glibc restricted to ISO C so that POSIX names (random,
# strnlen, ...) cannot collide with the game's own declarations; without
# warnings, as the guests build it, but for the mistakes it must not have
# (tools/build_common.py)
GAME_FLAGS = [
    "-std=gnu89",
    "-D__STRICT_ANSI__",
    *QUIET_OWN_CODE_FLAGS,
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-D_GNU_SOURCE",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
    *OWN_CODE_ERRORS,
]

# Code that talks to glibc alone, with its native 32-bit ABI (no
# -malign-double, no 16-bit wchar_t, no XDK headers), so that glibc's
# structures have their real layout.
POSIX_FLAGS = [
    "--target=i686-linux-gnu",
    "-m32",
    "-march=x86-64",
    "-std=gnu11",
    "-D_GNU_SOURCE",
    "-D_FILE_OFFSET_BITS=64",
    "-fno-pic",
    "-O2",
    "-g",
    "-Wall",
    *OWN_CODE_ERRORS,
]

LINK_FLAGS = ["--target=i686-linux-gnu", "-m32", "-no-pie", "-g"]

PORT_DIR = ROOT / "port/linux"
PREFIX_HEADER = PORT_DIR / "include" / "halo_linux_prefix.h"
SEMANTICS_SCRIPT = ROOT / "tools/linux_msvc_semantics.py"
GAME_DIR = PORT_DIR / "game"


def compiler() -> Optional[str]:
    """clang, if this computer has it"""
    return shutil.which("clang")


def can_build(work: Path) -> Optional[str]:
    """None if clang builds and links a 32-bit x86 Linux program here, else
    why not"""
    cc = compiler()
    if not cc:
        return "needs clang"
    if not sys.platform.startswith("linux"):
        return "needs 64-bit x86 Linux with the C library's 32-bit files"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "probe.c"
    source.write_text("#include <stdio.h>\nint main(void) { return puts(\"\") < 0; }\n")
    result = subprocess.run([cc, *POSIX_FLAGS, *LINK_FLAGS, str(source), "-o", str(work / "probe")],
                            capture_output=True, text=True)
    if result.returncode:
        return "needs the C library's 32-bit files (gcc-multilib, lib32-glibc): " + result.stderr.strip()[-300:]
    return None


def semantics_headers(folder: Path) -> Path:
    """halo_msvc_semantics.h (the game's) and platform_msvc_semantics.h (the
    platform layer's) in folder, as the guest builds have them
    (tools/build_common.py); returns folder"""
    folder.mkdir(parents=True, exist_ok=True)
    for name, scan in (("halo_msvc_semantics.h", ["--all-inlines", "--tags", "source", "--inlines", "source",
                                                   "--inlines", str(XDK_INCLUDE)]),
                       ("platform_msvc_semantics.h", ["--inlines", str(XDK_INCLUDE)])):
        subprocess.run([sys.executable, str(SEMANTICS_SCRIPT), "--output", str(folder / name), *scan],
                       cwd=ROOT, check=True)
    return folder


def game_cflags(semantics: Path) -> List[str]:
    """the game's units' flags (the game's sources and port/linux/game)"""
    config = json.loads((PORT_DIR / "port.json").read_text(encoding="utf-8"))
    return [*GAME_ABI_FLAGS, *GAME_FLAGS, "-include", str(PREFIX_HEADER),
            "-include", str(semantics / "halo_msvc_semantics.h"), f"-I{PORT_DIR / 'include'}",
            "-iquote", str(GAME_DIR), *shlex.split(game_defines_and_includes(config)), "-idirafter", str(XDK_INCLUDE)]


def platform_cflags(semantics: Path) -> List[str]:
    """the platform layer's units' flags (port/linux/src)"""
    return [*GAME_ABI_FLAGS, *PLATFORM_FLAGS, "-include", str(PREFIX_HEADER),
            "-include", str(semantics / "platform_msvc_semantics.h"), f"-I{PORT_DIR / 'src'}",
            f"-I{PORT_DIR / 'include'}", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
            f"-I{ZLIB_DIR}", "-Isource", "-Isource/cseries", "-idirafter", str(XDK_INCLUDE)]


def _compile(cc: str, source: Path, flags: Sequence[str], obj: Path) -> None:
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([cc, *flags, "-c", str(source), "-o", str(obj)], cwd=ROOT, capture_output=True,
                            text=True)
    if result.returncode:
        raise RuntimeError(f"{source} does not compile:\n{result.stderr[-4000:]}")


def _build_validator_tool(folder: Path, name: str, tool: Path, tool_flags: Sequence[str],
                          link_flags: Sequence[str]) -> Path:
    """a program of the tag validator's (port/linux/game/tag_validate.c,
    tag_schema*.c, and the Custom Edition maps' loader, cache_file_formats.c)
    and zlib with tool, its own C file compiled with tool_flags:
    folder/name, made again only when a source is newer"""
    cc = compiler()
    if not cc:
        raise RuntimeError("needs clang")
    folder = folder if folder.is_absolute() else ROOT / folder
    program = folder / name
    game_units = [GAME_DIR / "tag_validate.c", GAME_DIR / "cache_file_formats.c",
                  *sorted(GAME_DIR.glob("tag_schema*.c"))]
    zlib_units = [ZLIB_DIR / unit for unit in ZLIB_SOURCES]
    inputs = [*game_units, *zlib_units, tool, *tool.parent.glob("*.h"), *GAME_DIR.glob("*.h"),
              *GAME_DIR.glob("*.inc"), Path(__file__), SEMANTICS_SCRIPT]
    if program.is_file() and all(program.stat().st_mtime >= (ROOT / path).stat().st_mtime for path in inputs):
        return program
    semantics = semantics_headers(folder / "generated")
    game = game_cflags(semantics)
    zlib = [*GAME_ABI_FLAGS, "-std=gnu11", *ZLIB_DEFINES, "-w"]
    jobs = ([(source, game) for source in game_units] + [(source, zlib) for source in zlib_units]
            + [(tool, [*tool_flags, f"-I{ZLIB_DIR}", *ZLIB_DEFINES])])
    objects = [folder / "obj" / (Path(source).name + ".o") for source, _ in jobs]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for future in [pool.submit(_compile, cc, Path(source), flags, obj)
                       for (source, flags), obj in zip(jobs, objects)]:
            future.result()
    result = subprocess.run([cc, *LINK_FLAGS, *link_flags, *map(str, objects), "-lm", "-o", str(program)],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{name} does not link:\n{result.stderr[-4000:]}")
    return program


def build_map_validate(folder: Path) -> Path:
    """the tag validator alone on map files (tools/map_validate.c), with the
    game's objects of it and zlib: folder/map_validate, made again only when
    a source is newer"""
    return _build_validator_tool(folder, "map_validate", ROOT / "tools/map_validate.c", POSIX_FLAGS, [])


# fork: the asset extractor of PRISM's pack tools (tools/prism_packs/extract/map_extract_tool.c: a map's
# bitmaps, models and structure bsps as PNG, glTF and JMS, and its light sources, read as the validator reads
# them). The one 32-bit program
# of the fork's packages: linked statically (it needs no 32-bit C library on the player's computer, only a
# kernel that runs 32-bit programs, as every 64-bit x86 Linux does), with the baseline x86-64 instruction set
# and no fused multiply-adds, so that every computer extracts the same numbers
MAP_EXTRACT = ROOT / "tools/prism_packs/extract/map_extract_tool.c"


def build_map_extract(folder: Path) -> Path:
    """tools/prism_packs/extract/map_extract_tool.c (map_extract.c and its lights command) with the
    validator's objects and zlib: folder/map_extract, static, made again only when a source is newer"""
    return _build_validator_tool(folder, "map_extract", MAP_EXTRACT, [*POSIX_FLAGS, "-ffp-contract=off"],
                                 ["-static"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder", type=Path, nargs="?", default=None,
                        help="where the program goes (default: build/map_validate, or build/map_extract)")
    parser.add_argument("--map-extract", action="store_true",
                        help="build PRISM's pack tools' extractor (tools/prism_packs/extract) instead")
    args = parser.parse_args()
    name = "map_extract" if args.map_extract else "map_validate"
    folder = args.folder or Path("build") / name
    reason = can_build(folder if folder.is_absolute() else ROOT / folder)
    if reason:
        print(f"cannot build {name}: {reason}", file=sys.stderr)
        return 1
    print((build_map_extract if args.map_extract else build_map_validate)(folder))
    return 0


if __name__ == "__main__":
    sys.exit(main())
