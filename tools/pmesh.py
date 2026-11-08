#!/usr/bin/env python3
"""PRISM's .pmesh files (display.prism_models; port/linux/vulkan/xgpu_mesh_store.h
has the format): written, read and checked as the host checks them.

    python3 tools/pmesh.py check <file.pmesh> [--dump <model>.json]   # as the host would, and more
    python3 tools/pmesh.py info <file.pmesh>

The exporters (tools/pmesh_from_dump.py; the art pipeline's
prism_pmesh_export.py, in Blender) build on write_pmesh. --dump checks the file
against the model as the game has it (debug.prism_mesh_dump's json): its node
count and checksum, and that every piece's region, permutation and shader is one
of a part the model draws, as the host's map record does.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

MAGIC = 0x48534D50  # "PMSH"
VERSION = 1
HEADER_BYTES = 64
SECTION_BYTES = 16
BONE_BYTES = 112
LOD_BYTES = 48
PIECE_BYTES = 24
VERTEX_BYTES = 48
BONES_MAXIMUM = 1024
LODS_MAXIMUM = 8
PIECES_MAXIMUM = 4096

VERTEX_DTYPE = np.dtype([
    ("position", "<f4", 3),
    ("normal", "<i2", 2),
    ("tangent", "<i2", 2),
    ("uv1", "<f2", 2),
    ("uv0", "<f4", 2),
    ("joints", "<u2", 4),
    ("weights", "u1", 4),
    ("sign", "i1"),
    ("pad", "u1", 3),
])
assert VERTEX_DTYPE.itemsize == VERTEX_BYTES


def kind(name: str) -> int:
    return struct.unpack("<I", name.encode())[0]


SKEL, LODS, SUBM, VERT, INDX, META, RIG = (kind(k) for k in ("SKEL", "LODS", "SUBM", "VERT", "INDX", "META", "RIG "))


@dataclass
class Bone:
    name: str
    parent: int
    node: int  # the model's node index, -1 a helper
    bind: np.ndarray  # 3x4, model space
    inverse: np.ndarray  # 3x4


@dataclass
class Level:
    cutoff: float
    piece_first: int
    piece_count: int
    vertex_first: int
    vertex_count: int
    index_first: int
    index_count: int
    center: tuple = (0.0, 0.0, 0.0)
    radius: float = 0.0


@dataclass
class Piece:
    region: int
    permutation: int
    shader: int
    flags: int
    material: int
    vertex_first: int
    vertex_count: int
    index_first: int
    index_count: int


@dataclass
class Mesh:
    node_count: int
    node_crc: int
    bones: list
    levels: list
    pieces: list
    vertices: np.ndarray  # VERTEX_DTYPE
    indices: np.ndarray  # uint32
    center: tuple = (0.0, 0.0, 0.0)
    radius: float = 0.0
    meta: dict = field(default_factory=dict)
    # the helper bones' rules: the RIG section (tools/prism_rig.py compile's bytes; b"" none)
    rig: bytes = b""


def octahedral(vectors: np.ndarray) -> np.ndarray:
    """unit vectors (n, 3) as octahedral snorm16x2 (n, 2)"""
    v = vectors / np.maximum(np.abs(vectors).sum(axis=1, keepdims=True), 1e-20)
    x, y, z = v[:, 0].copy(), v[:, 1].copy(), v[:, 2]
    negative = z < 0
    sx = np.where(x >= 0, 1.0, -1.0)
    sy = np.where(y >= 0, 1.0, -1.0)
    x2 = np.where(negative, (1.0 - np.abs(v[:, 1])) * sx, x)
    y2 = np.where(negative, (1.0 - np.abs(v[:, 0])) * sy, y)
    out = np.stack([x2, y2], axis=1)
    return np.clip(np.round(out * 32767.0), -32767, 32767).astype("<i2")


def octahedral_decode(packed: np.ndarray) -> np.ndarray:
    e = np.maximum(packed.astype(np.float64) / 32767.0, -1.0)
    v = np.stack([e[:, 0], e[:, 1], 1.0 - np.abs(e[:, 0]) - np.abs(e[:, 1])], axis=1)
    negative = v[:, 2] < 0
    sx = np.where(v[:, 0] >= 0, 1.0, -1.0)
    sy = np.where(v[:, 1] >= 0, 1.0, -1.0)
    x = np.where(negative, (1.0 - np.abs(v[:, 1])) * sx, v[:, 0])
    y = np.where(negative, (1.0 - np.abs(v[:, 0])) * sy, v[:, 1])
    v = np.stack([x, y, v[:, 2]], axis=1)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


def quantise_weights(weights: np.ndarray) -> np.ndarray:
    """(n, 4) weights as unorm8 summing to 255 (the rounding given to the largest)"""
    total = weights.sum(axis=1, keepdims=True)
    w = np.where(total > 0, weights / np.maximum(total, 1e-20), np.array([[1.0, 0, 0, 0]]))
    q = np.round(w * 255.0).astype(np.int32)
    largest = np.argmax(w, axis=1)
    q[np.arange(len(q)), largest] += 255 - q.sum(axis=1)
    return np.clip(q, 0, 255).astype(np.uint8)


def name_hash(name: str) -> int:
    return zlib.crc32(name.encode()) & 0xFFFFFFFF


def write_pmesh(path: Path, mesh: Mesh) -> int:
    """writes the file; returns its size"""
    if not 0 < len(mesh.bones) <= BONES_MAXIMUM or len(mesh.bones) < mesh.node_count:
        raise ValueError(f"{len(mesh.bones)} bones for {mesh.node_count} nodes")
    if not 0 < len(mesh.levels) <= LODS_MAXIMUM or not 0 < len(mesh.pieces) <= PIECES_MAXIMUM:
        raise ValueError(f"{len(mesh.levels)} levels, {len(mesh.pieces)} pieces")
    names = b""
    skeleton = b""
    for bone in mesh.bones:
        offset = len(names)
        names += bone.name.encode() + b"\0"
        flags = 1 if bone.node >= 0 else 2
        skeleton += struct.pack("<IhHhHI", name_hash(bone.name), bone.parent, flags, bone.node, 0, offset)
        skeleton += np.asarray(bone.bind, dtype="<f4").reshape(12).tobytes()
        skeleton += np.asarray(bone.inverse, dtype="<f4").reshape(12).tobytes()
    skeleton += names
    levels = b"".join(struct.pack("<fIIIIII3ffI", l.cutoff, l.piece_first, l.piece_count, l.vertex_first, l.vertex_count,
                                  l.index_first, l.index_count, *l.center, l.radius, 0) for l in mesh.levels)
    pieces = b"".join(struct.pack("<BBBBIIIII", p.region, p.permutation, p.shader, p.flags, p.material,
                                  p.vertex_first, p.vertex_count, p.index_first, p.index_count) for p in mesh.pieces)
    vertices = np.ascontiguousarray(mesh.vertices, dtype=VERTEX_DTYPE).tobytes()
    indices = np.ascontiguousarray(mesh.indices, dtype="<u4").tobytes()
    meta = json.dumps(mesh.meta, sort_keys=True).encode()
    sections = [(SKEL, skeleton), (LODS, levels), (SUBM, pieces), (VERT, vertices), (INDX, indices), (META, meta)]
    if mesh.rig:
        sections.insert(5, (RIG, bytes(mesh.rig)))
    table_offset = HEADER_BYTES
    at = table_offset + SECTION_BYTES * len(sections)
    table = b""
    body = b""
    for section_kind, data in sections:
        at = (at + 15) & ~15
        pad = at - (table_offset + SECTION_BYTES * len(sections) + len(body))
        body += b"\0" * pad
        table += struct.pack("<IIII", section_kind, at, len(data), zlib.crc32(data) & 0xFFFFFFFF)
        body += data
        at += len(data)
    size = HEADER_BYTES + len(table) + len(body)
    header = struct.pack("<IHHIIIIIII3ff3I", MAGIC, VERSION, 0, size, len(sections), table_offset, mesh.node_count,
                         mesh.node_crc & 0xFFFFFFFF, len(mesh.bones), len(mesh.levels), *mesh.center, mesh.radius, 0, 0, 0)
    assert len(header) == HEADER_BYTES
    data = header + table + body
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return len(data)


class FormatError(Exception):
    pass


def read_pmesh(path: Path, check_crcs: bool = True) -> Mesh:
    data = Path(path).read_bytes()
    if len(data) < HEADER_BYTES:
        raise FormatError("shorter than its header")
    (magic, version, flags, size, section_count, table_offset, node_count, node_crc, bone_count, level_count,
     cx, cy, cz, radius) = struct.unpack_from("<IHHIIIIIII3ff", data, 0)
    if magic != MAGIC or version != VERSION or size != len(data):
        raise FormatError(f"magic {magic:08x}, version {version}, size {size} of {len(data)}")
    sections = {}
    for index in range(section_count):
        k, offset, length, crc = struct.unpack_from("<IIII", data, table_offset + index * SECTION_BYTES)
        if offset + length > len(data):
            raise FormatError(f"section {index} outside the file")
        chunk = data[offset:offset + length]
        if check_crcs and zlib.crc32(chunk) & 0xFFFFFFFF != crc:
            raise FormatError(f"section {struct.pack('<I', k).decode(errors='replace')} damaged (CRC)")
        sections[k] = chunk
    for needed in (SKEL, LODS, SUBM, VERT, INDX):
        if needed not in sections:
            raise FormatError(f"no {struct.pack('<I', needed).decode()} section")
    skel = sections[SKEL]
    names_blob = skel[bone_count * BONE_BYTES:]
    bones = []
    for index in range(bone_count):
        h, parent, bone_flags, node, _, name_offset = struct.unpack_from("<IhHhHI", skel, index * BONE_BYTES)
        matrices = np.frombuffer(skel, dtype="<f4", count=24, offset=index * BONE_BYTES + 16)
        end = names_blob.find(b"\0", name_offset)
        name = names_blob[name_offset:end].decode(errors="replace") if end >= 0 else "?"
        bones.append(Bone(name, parent, node, matrices[:12].reshape(3, 4), matrices[12:].reshape(3, 4)))
    levels = []
    for index in range(level_count):
        values = struct.unpack_from("<fIIIIII3ffI", sections[LODS], index * LOD_BYTES)
        levels.append(Level(values[0], *values[1:7], tuple(values[7:10]), values[10]))
    pieces = [Piece(*struct.unpack_from("<BBBBIIIII", sections[SUBM], i * PIECE_BYTES))
              for i in range(len(sections[SUBM]) // PIECE_BYTES)]
    vertices = np.frombuffer(sections[VERT], dtype=VERTEX_DTYPE)
    indices = np.frombuffer(sections[INDX], dtype="<u4")
    meta = {}
    if META in sections:
        try:
            meta = json.loads(sections[META] or b"{}")
        except ValueError:
            meta = {}
    return Mesh(node_count, node_crc, bones, levels, pieces, vertices, indices, (cx, cy, cz), radius, meta,
                sections.get(RIG, b""))


def model_table(dump: dict) -> set:
    """the regions, permutations and shaders the model's parts draw with (mod_meshes.c's model_table)"""
    table = set()
    for region_index, region in enumerate(dump["regions"]):
        for permutation_index, permutation in enumerate(region["permutations"]):
            for geometry in permutation["geometries"]:
                if 0 <= geometry < len(dump["geometries"]):
                    for part in dump["geometries"][geometry]["parts"]:
                        if 0 <= part["shader"] < len(dump["shaders"]):
                            table.add((region_index, permutation_index, part["shader"]))
    return table


def check(mesh: Mesh, dump: dict | None = None) -> list:
    """the host's checks (xgpu_mesh_store.c's query_check and entry_check): the problems found"""
    problems = []
    if dump is not None:
        if mesh.node_count != len(dump["nodes"]) or mesh.node_crc != dump["node_crc"]:
            problems.append(f"nodes: {mesh.node_count}, {mesh.node_crc:08x}; the model's {len(dump['nodes'])}, "
                            f"{dump['node_crc']:08x}")
        table = model_table(dump)
        for index, piece in enumerate(mesh.pieces):
            if (piece.region, piece.permutation, piece.shader) not in table:
                problems.append(f"piece {index}: region {piece.region}, permutation {piece.permutation}, shader "
                                f"{piece.shader} is no part's of the model")
    for index, bone in enumerate(mesh.bones):
        if index < mesh.node_count and bone.node != index:
            problems.append(f"bone {index} is not node {index}")
        if index >= mesh.node_count and (bone.node != -1 or not 0 <= bone.parent < index):
            problems.append(f"helper bone {index} has no parent before it")
    vertex_total, index_total = len(mesh.vertices), len(mesh.indices)
    for index, piece in enumerate(mesh.pieces):
        if (piece.vertex_first + piece.vertex_count > vertex_total or piece.index_first + piece.index_count > index_total
                or not piece.vertex_count or not piece.index_count or piece.index_count % 3):
            problems.append(f"piece {index} is outside the mesh")
            continue
        chunk = mesh.indices[piece.index_first:piece.index_first + piece.index_count]
        if chunk.size and int(chunk.max()) >= piece.vertex_count:
            problems.append(f"piece {index}'s indices pass its {piece.vertex_count} vertices")
    previous = None
    for index, level in enumerate(mesh.levels):
        if (level.piece_first + level.piece_count > len(mesh.pieces) or level.vertex_first + level.vertex_count >
                vertex_total or not level.vertex_count or (previous is not None and level.cutoff > previous)):
            problems.append(f"level {index} is outside the mesh or more detailed than the one before")
        for piece in mesh.pieces[level.piece_first:level.piece_first + level.piece_count]:
            if piece.vertex_first < level.vertex_first or piece.vertex_first + piece.vertex_count > \
                    level.vertex_first + level.vertex_count:
                problems.append(f"level {index}'s piece is outside its vertices")
        previous = level.cutoff
    if not np.all(np.isfinite(mesh.vertices["position"])):
        problems.append("a position is not a number")
    joints, weights = mesh.vertices["joints"], mesh.vertices["weights"]
    if np.any((weights > 0) & (joints >= len(mesh.bones))):
        problems.append("a vertex names a bone past the skeleton")
    if not np.all(np.isfinite(mesh.vertices["uv0"])):
        problems.append("a uv0 is not a number")
    return problems


def uv_past_scale(mesh: Mesh, dump: dict) -> int:
    """the vertices whose uv0 is past the model's base map scale: not refused (the host clamps them in the
    stock compressed layout, so a draw without a material shows them clamped; a draw with a material reads
    them whole from the wide stream)"""
    scale = np.asarray(dump["base_map_scale"], dtype=np.float64)
    uv = mesh.vertices["uv0"] / scale
    return int(np.count_nonzero(np.any(np.abs(uv) > 1.0001, axis=1)))


def summary(mesh: Mesh) -> str:
    lines = [f"{mesh.node_count} nodes ({mesh.node_crc:08x}), {len(mesh.bones)} bones, {len(mesh.levels)} levels, "
             f"{len(mesh.pieces)} pieces, {len(mesh.vertices)} vertices, {len(mesh.indices) // 3} triangles, radius "
             f"{mesh.radius:.4f}"]
    for index, level in enumerate(mesh.levels):
        triangles = sum(p.index_count for p in mesh.pieces[level.piece_first:level.piece_first + level.piece_count]) // 3
        lines.append(f"  level {index}: from {level.cutoff:g} px, {level.piece_count} pieces, {level.vertex_count} "
                     f"vertices, {triangles} triangles")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("check", "info"))
    parser.add_argument("file", type=Path)
    parser.add_argument("--dump", type=Path, help="the model as the game has it (debug.prism_mesh_dump's json)")
    arguments = parser.parse_args()
    try:
        mesh = read_pmesh(arguments.file)
    except FormatError as problem:
        print(f"{arguments.file}: {problem}")
        return 1
    print(summary(mesh))
    if arguments.command == "check":
        problems = check(mesh, json.loads(arguments.dump.read_text()) if arguments.dump else None)
        for problem in problems:
            print(f"problem: {problem}")
        print("ok" if not problems else f"{len(problems)} problems")
        return 1 if problems else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
