"""The PRSX sidecar container, in Python: the writer the farm uses and a reader for the tools. The game's reader is
port/linux/game/prism_prsx.c, which has the format; tools/test_geopatch.py checks that the two agree byte for byte
and refuse the same malformed files.

    header (48 bytes): "PRSX", u32 format, classic_world_id[32], u32 section_count, u32 flags
    directory (68 bytes each): fourcc, u16 bsp, u16 flags, u32 collv, u64 offset, u64 csize, u64 usize,
                               sha256_decoded[32]; sorted by (fourcc as a little-endian u32, bsp, collv), each
                               key once, so one content has one file and one world_id
    sections
"""
import hashlib
import shutil
import struct
import subprocess
import zlib

MAGIC = b"PRSX"
FORMAT = 1
FORMAT_OLDEST = 1
HEADER_SIZE = 48
ENTRY_SIZE = 68
BSP_ALL = 0xFFFF
MAXIMUM_SECTIONS = 4096
MAXIMUM_SECTION_BYTES = 1 << 30
TRAILER_SIZE = 24

REQUIRED = 0x0001
BAKE_RELEVANT = 0x0002
SET_SHIFT, SET_MASK = 4, 0x00F0
CODEC_SHIFT, CODEC_MASK = 12, 0xF000
FLAGS_KNOWN = REQUIRED | BAKE_RELEVANT | SET_MASK | CODEC_MASK

STORED, ZLIB, ZSTD = 0, 1, 2

# The type table (section 2.5's CI table; prism_prsx.c's prsx_types is its twin): bake sections enter the enhanced
# world_id, runtime and sim ones never do. Sim sections carry a collision version (collv), the others 0.
# Adam's 2026-11-25 decision (PLAN_FINAL.md section 10) replaced the add-on collision (ZSOL/PATO) with collision
# that matches the enhanced geometry: COLL (the enhanced collision BSP), COLM (scenery collision), PATH (its
# pathfinding data), per COLLV.
BAKE, RUNTIME, SIM = "bake", "runtime", "sim"
TYPES = {
    "GEOM": BAKE,     # per-BSP render geometry: parts, runs per stock surface, near/far
    "LMUV": BAKE,     # per-BSP unique lightmap UVs + page table for the enhanced bake
    "SCEN": BAKE,     # static scenery replacement meshes, keyed by placement index + palette index
    "RHGT": RUNTIME,  # per-BSP render-surface height/normal field (prints, decals, feet effects)
    "DOBJ": RUNTIME,  # stock detail objects and decals re-projected onto GEOM
    "COLL": SIM,      # enhanced collision BSP matching GEOM, per COLLV
    "COLM": SIM,      # scenery collision matching SCEN, per COLLV
    "PATH": SIM,      # pathfinding surfaces/edges for COLL, per COLLV
}


class PrsxError(Exception):
    pass


def fourcc(text):
    return struct.unpack("<I", text.encode("ascii"))[0]


def fourcc_text(value):
    return struct.pack("<I", value).decode("ascii", "replace")


def default_flags(kind, required=True):
    """an entry's flags for a known type: REQUIRED (an old reader refuses the file) and BAKE_RELEVANT as the table
    says"""
    flags = REQUIRED if required else 0
    if TYPES.get(kind) == BAKE:
        flags |= BAKE_RELEVANT
    return flags


class Section:
    """one section to write: its type, BSP (BSP_ALL: every BSP), collision version, flags and decoded bytes"""

    def __init__(self, kind, bsp, data, collv=0, flags=None, codec=ZLIB, level=9, static_set=0):
        if len(kind) != 4:
            raise PrsxError("a type is four characters")
        self.kind = kind
        self.bsp = bsp
        self.collv = collv
        self.data = bytes(data)
        self.flags = default_flags(kind) if flags is None else flags
        self.flags |= (static_set << SET_SHIFT) & SET_MASK
        self.codec = codec
        self.level = level


# the deflate a ZLIB section is written with: (data, level) -> zlib stream. The farm's builds set the vendored one
# (native.Native.deflate: port/third_party/zlib's deflate, the same bytes on every machine); Python's own zlib
# (zlib-ng on some systems, another zlib version on others) writes other, equally valid, streams, so a sidecar
# written with it is not reproducible across machines
DEFLATE = None


def compress(data, codec, level=9):
    if codec == STORED:
        return data
    if codec == ZLIB:
        return DEFLATE(data, level) if DEFLATE is not None else zlib.compress(data, level)
    if codec == ZSTD:
        return zstd_cli(data, level)
    raise PrsxError("unknown codec %d" % codec)


def zstd_cli(data, level=19):
    """zstd through the system's zstd program (sizes only: the game does not read zstd until it is vendored)"""
    program = shutil.which("zstd")
    if not program:
        raise PrsxError("zstd is not installed")
    arguments = [program, "-q", "-c", "--check"] + (["--ultra"] if level > 19 else []) + ["-%d" % level]
    result = subprocess.run(arguments, input=data, capture_output=True, check=True)
    return result.stdout


def directory_key(kind, bsp, collv):
    """an entry's place in the directory (prism_prsx.c's prsx_key_compare)"""
    return (fourcc(kind), bsp, collv)


def write(classic_world_id, sections):
    """a sidecar's bytes: the header, the directory sorted by directory_key whatever order the sections are given
    in, then the sections in that order"""
    if len(classic_world_id) != 32:
        raise PrsxError("classic_world_id is 32 bytes")
    if len(sections) > MAXIMUM_SECTIONS:
        raise PrsxError("too many sections")
    seen = set()
    packed = []
    for section in sections:
        key = (section.kind, section.bsp, section.collv)
        if key in seen:
            raise PrsxError("two %s sections for BSP %d collv %d" % key)
        seen.add(key)
        if TYPES.get(section.kind) == SIM and not section.collv:
            raise PrsxError("a sim section needs a collision version")
        if TYPES.get(section.kind) in (BAKE, RUNTIME) and section.collv:
            raise PrsxError("only sim sections carry a collision version")
    sections = sorted(sections, key=lambda section: directory_key(section.kind, section.bsp, section.collv))
    for section in sections:
        packed.append(compress(section.data, section.codec, section.level))
    offset = HEADER_SIZE + ENTRY_SIZE * len(sections)
    out = bytearray(MAGIC + struct.pack("<I", FORMAT) + classic_world_id + struct.pack("<II", len(sections), 0))
    for section, blob in zip(sections, packed):
        flags = (section.flags & ~CODEC_MASK) | (section.codec << CODEC_SHIFT)
        out += struct.pack("<IHHIQQQ", fourcc(section.kind), section.bsp, flags, section.collv, offset, len(blob),
                           len(section.data))
        out += hashlib.sha256(section.data).digest()
        offset += len(blob)
    for blob in packed:
        out += blob
    return bytes(out)


def append_trailer(whole, container):
    """a whole file with a container appended (the A1 alternative): whole, container, "PRSXTAIL", offset, size"""
    return whole + container + b"PRSXTAIL" + struct.pack("<QQ", len(whole), len(container))


class Entry:
    def __init__(self, kind, bsp, flags, collv, offset, csize, usize, sha256):
        self.kind, self.bsp, self.flags, self.collv = kind, bsp, flags, collv
        self.offset, self.csize, self.usize, self.sha256 = offset, csize, usize, sha256

    @property
    def codec(self):
        return (self.flags & CODEC_MASK) >> CODEC_SHIFT

    @property
    def bake_relevant(self):
        return bool(self.flags & BAKE_RELEVANT)


class Sidecar:
    """a sidecar read and checked as prism_prsx.c's prsx_open checks it"""

    def __init__(self, data):
        self.data = bytes(data)
        if len(self.data) < HEADER_SIZE or self.data[:4] != MAGIC:
            raise PrsxError("not a PRSX file")
        self.format = struct.unpack_from("<I", self.data, 4)[0]
        if not FORMAT_OLDEST <= self.format <= FORMAT:
            raise PrsxError("format %d" % self.format)
        self.classic_world_id = self.data[8:40]
        count, flags = struct.unpack_from("<II", self.data, 40)
        if flags or count > MAXIMUM_SECTIONS:
            raise PrsxError("malformed header")
        end = HEADER_SIZE + count * ENTRY_SIZE
        if end > len(self.data):
            raise PrsxError("malformed directory")
        self.entries = []
        last_key = None
        for index in range(count):
            at = HEADER_SIZE + index * ENTRY_SIZE
            kind, bsp, flags, collv, offset, csize, usize = struct.unpack_from("<IHHIQQQ", self.data, at)
            entry = Entry(fourcc_text(kind), bsp, flags, collv, offset, csize, usize, self.data[at + 36:at + 68])
            klass = TYPES.get(entry.kind)
            if flags & ~FLAGS_KNOWN or offset < end or offset > len(self.data) or csize > len(self.data) - offset \
                    or usize > MAXIMUM_SECTION_BYTES or (entry.codec == STORED and csize != usize) \
                    or entry.codec not in (STORED, ZLIB, ZSTD):
                raise PrsxError("malformed directory")
            if klass is None:
                if flags & REQUIRED:
                    raise PrsxError("required section %s unknown" % entry.kind)
                if entry.bake_relevant:
                    # (a section this reader skips never enters its world_id)
                    raise PrsxError("section %s's class disagrees: unknown and bake relevant" % entry.kind)
            elif (klass == BAKE) != entry.bake_relevant or (klass == SIM) != (collv != 0):
                raise PrsxError("section %s's class disagrees" % entry.kind)
            if klass is not None and entry.codec == ZSTD:
                raise PrsxError("a section in a codec this reader does not read")
            if last_key is not None and last_key >= (kind, bsp, collv):
                raise PrsxError("malformed directory: not sorted, or an entry twice")
            last_key = (kind, bsp, collv)
            self.entries.append(entry)
        spans = sorted((e.offset, e.offset + e.csize) for e in self.entries)
        if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
            raise PrsxError("malformed directory: sections overlap")

    def find(self, kind, bsp, collv=0):
        for index, entry in enumerate(self.entries):
            if entry.kind == kind and entry.bsp == bsp and entry.collv == collv:
                return index
        return -1

    def section(self, index):
        entry = self.entries[index]
        blob = self.data[entry.offset:entry.offset + entry.csize]
        if entry.codec == STORED:
            data = blob
        elif entry.codec == ZLIB:
            inflater = zlib.decompressobj()
            data = inflater.decompress(blob)
            if not inflater.eof or inflater.unused_data:
                raise PrsxError("a section of the wrong size")
        else:
            raise PrsxError("a section in a codec this reader does not read")
        if len(data) != entry.usize:
            raise PrsxError("a section of the wrong size")
        if hashlib.sha256(data).digest() != entry.sha256:
            raise PrsxError("a section whose SHA-256 differs")
        return data

    def bsps(self):
        return sorted({e.bsp for e in self.entries if e.bsp != BSP_ALL})
