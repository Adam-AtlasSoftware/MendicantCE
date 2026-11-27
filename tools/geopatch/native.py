"""The game's sidecar C, built as one command (native_main.c) and run by the farm's tools and the tests: exact REF
decode, the sidecar reader and the world_id, from the code the game runs (D-B3: one function in both).

The command is built once per source digest into GEOPATCH_NATIVE_DIR (default build/geopatch_native under the
repository), with clang if there is one, else cc; on Windows clang for i686-pc-windows-msvc, as the other native
tests build.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "port/linux/game"
ZLIB = ROOT / "port/third_party/zlib"
SOURCES = [Path(__file__).resolve().parent / "native_main.c", GAME / "prism_prsx.c", GAME / "prism_identity.c",
           GAME / "prism_ref_decode.c", GAME / "prism_geom.c", GAME / "mcc_sha256.c"]
# (deflate.c and trees.c: the one deflate the sidecar sections are written with, the same on every machine)
ZLIB_SOURCES = ["adler32.c", "crc32.c", "deflate.c", "inffast.c", "inflate.c", "inftrees.c", "trees.c", "uncompr.c",
                "zutil.c"]
HEADERS = [GAME / "prism_prsx.h", GAME / "prism_identity.h", GAME / "prism_ref_decode.h", GAME / "prism_geom.h",
           GAME / "mcc_sha256.h"]


class NativeError(Exception):
    pass


def compiler():
    found = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not found:
        raise NativeError("no C compiler")
    return found


def build(folder=None):
    """the command's path, built if its sources changed"""
    digest = hashlib.sha256()
    for path in SOURCES + HEADERS + [ZLIB / name for name in ZLIB_SOURCES]:
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    folder = Path(folder or os.environ.get("GEOPATCH_NATIVE_DIR") or ROOT / "build/geopatch_native")
    folder.mkdir(parents=True, exist_ok=True)
    windows = sys.platform == "win32"
    exe = folder / ("native-%s%s" % (digest.hexdigest()[:16], ".exe" if windows else ""))
    if exe.exists():
        return exe
    cc = compiler()
    flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"] if windows else []
    zdefs = ["-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg"]
    # (zlib's own sources are taken as they are, not held to our warnings)
    objects = []
    partial = "%s.%d.partial" % (exe, os.getpid())
    for name in ZLIB_SOURCES:
        # (this process's own objects and output: several workers may build the same command at once)
        obj = folder / ("%s-%d-%s.o" % (exe.stem, os.getpid(), name))
        result = subprocess.run([cc, *flags, "-O2", *zdefs, "-I", str(ZLIB), "-c", str(ZLIB / name), "-o", str(obj)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise NativeError(result.stderr)
        objects.append(str(obj))
    # (ours strict, as the game's units: C89 with GNU extensions, every warning an error; the decode has no
    # floating point, so no float flags matter)
    result = subprocess.run([cc, *flags, "-std=gnu89", "-O2", "-Wall", "-Wextra", "-Werror", *zdefs, "-I", str(GAME),
                             "-I", str(ZLIB), *map(str, SOURCES), *objects, "-o", partial],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise NativeError(result.stderr)
    for obj in objects:
        os.remove(obj)
    os.replace(partial, exe)
    return exe


class Native:
    def __init__(self, folder=None):
        self.exe = build(folder)

    def _stream(self, command, data):
        result = subprocess.run([str(self.exe), command], input=data, capture_output=True)
        if result.returncode not in (0, 3):
            raise NativeError("%s failed (%d)" % (command, result.returncode))
        return result.stdout, result.returncode == 0

    def deflate(self, data, level=9):
        """data as one zlib stream by the vendored zlib's deflate (port/third_party/zlib): the same bytes on every
        machine, unlike the zlib a Python is linked with"""
        result = subprocess.run([str(self.exe), "deflate", str(int(level))], input=data, capture_output=True)
        if result.returncode != 0:
            raise NativeError("deflate failed (%d)" % result.returncode)
        return result.stdout

    def environment_vertices(self, compressed):
        """32-byte compressed environment vertices -> the 56-byte uncompressed ones"""
        return self._stream("env", compressed)[0]

    def lightmap_vertices(self, compressed):
        return self._stream("lightmap", compressed)[0]

    def vectors(self, packed):
        """packed u32 values -> [(x, y, z) float bits]"""
        out = self._stream("vector", struct.pack("<%dI" % len(packed), *packed))[0]
        flat = struct.unpack("<%dI" % (3 * len(packed)), out)
        return [flat[i:i + 3] for i in range(0, len(flat), 3)]

    def coordinates(self, values):
        out = self._stream("coordinate", struct.pack("<%dh" % len(values), *values))[0]
        return list(struct.unpack("<%dI" % len(values), out))

    def grid(self, values):
        """grid integers -> float bits; NativeError if one is out of range"""
        out, ok = self._stream("grid", struct.pack("<%di" % len(values), *values))
        if not ok:
            raise NativeError("a grid position out of range")
        return list(struct.unpack("<%dI" % len(values), out))

    def reciprocal(self, divisor):
        return int(subprocess.run([str(self.exe), "reciprocal", str(divisor)], capture_output=True, text=True,
                                  check=True).stdout, 16)

    def open(self, path, appended=False):
        """the game's verdict on a sidecar: {result, format, classic, entries: [...], world: {bsp: (classic,
        enhanced)}}"""
        args = [str(self.exe), "open", str(path)] + (["appended"] if appended else [])
        result = subprocess.run(args, capture_output=True, text=True)
        out = {"entries": [], "world": {}}
        for line in result.stdout.splitlines():
            word, _, rest = line.partition(" ")
            if word == "result":
                out["result"] = rest
            elif word == "format":
                out["format"] = int(rest)
            elif word == "classic":
                out["classic"] = rest
            elif word == "entry":
                fields = rest.split(" ", 6)
                out["entries"].append({"index": int(fields[0]), "kind": fields[1], "bsp": int(fields[2]),
                                       "flags": int(fields[3]), "collv": int(fields[4]), "usize": int(fields[5]),
                                       "read": fields[6]})
            elif word == "world":
                fields = rest.split()
                out["world"][int(fields[0])] = (fields[2], fields[4])
        return out

    def geom(self, stock_path, sidecar_path, bsp):
        """(the PWG1 image or None, the report line) of the game's decode of a sidecar's BSP (prism_geom.c)"""
        result = subprocess.run([str(self.exe), "geom", str(stock_path), str(sidecar_path), str(bsp)],
                                capture_output=True)
        return (result.stdout if result.returncode == 0 else None), result.stderr.decode(errors="replace").strip()

    def ladder(self):
        """{(classic, geometry, sidecar, packs, unbaked, path_traced): rung} of prism_world_ladder"""
        result = subprocess.run([str(self.exe), "ladder"], capture_output=True, text=True, check=True)
        out = {}
        for line in result.stdout.splitlines():
            fields = [int(x) for x in line.split()[1:]]
            out[tuple(fields[:6])] = fields[6]
        return out

    def base_id(self, path):
        result = subprocess.run([str(self.exe), "base-id", str(path)], capture_output=True, text=True)
        if result.returncode != 0:
            return None
        return result.stdout.split()[1]


def write_stock(path, bsp):
    """a RetailBsp (or anything with .materials {(lightmap, material): .vertices, .lightmap_vertices} and
    .collision_vertices) as the stock file native's geom reads"""
    out = bytearray(struct.pack("<I", len(bsp.materials)))
    for (lightmap, material_index), material in sorted(bsp.materials.items()):
        count = len(material.vertices) // 32
        out += struct.pack("<HHI", lightmap, material_index, count) + bytes(material.vertices[:32 * count])
        lightmapped = len(material.lightmap_vertices) >= 8 * count and count > 0
        out += struct.pack("<I", 1 if lightmapped else 0)
        if lightmapped:
            out += bytes(material.lightmap_vertices[:8 * count])
    out += struct.pack("<I", len(bsp.collision_vertices))
    for point in bsp.collision_vertices:
        out += struct.pack("<3f", *point)
    Path(path).write_bytes(bytes(out))
