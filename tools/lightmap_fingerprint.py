#!/usr/bin/env python3
"""The lightmap layout fingerprint of each structure BSP of an Xbox map, for a mod's lightmap
(port/linux/game/mod_lightmaps.c, docs/mods.md): a mod's tags\\<name>.lightmap_layout beside its
tags\\<name>.bitmap holds the fingerprint of the BSP layout it was made for, and the game takes the
lightmap only for a BSP of that layout.

    python3 tools/lightmap_fingerprint.py maps/bloodgulch.map
    python3 tools/lightmap_fingerprint.py maps/bloodgulch.map --write mods/my_lightmaps

prints a line for each BSP, its lightmap bitmap's name and its fingerprint:

    bsp 0: levels\\test\\bloodgulch\\bloodgulch lm1-0123456789abcdef

and with --write, writes each one's layout file into the mod's tags folder (beside where its
lightmap's .bitmap goes). A Custom Edition map's fingerprints are in the game's log (debug.txt) as it
checks a mod's lightmap for it.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

HEADER_SIZE = 0x800
# where the game reads a map's tags, and the room there (physical_memory_map.c)
TAG_CACHE_BASE = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
SCENARIO_STRUCTURE_BSPS = 0x5A4          # scenario_definitions.h
STRUCTURE_BSP_LIGHTMAP_TAG_INDEX = 0x0C  # structure_bsp: lightmap_group (a tag reference), its index
STRUCTURE_BSP_SURFACES = 0xF8
STRUCTURE_BSP_LIGHTMAPS = 0x104
LIGHTMAP_SIZE, LIGHTMAP_MATERIALS = 0x20, 0x14
MATERIAL_SIZE = 0x100
MATERIAL_FIRST_SURFACE, MATERIAL_SURFACE_COUNT = 0x14, 0x18
MATERIAL_VERTEX_COUNT, MATERIAL_LIGHTMAP_VERTEX_COUNT = 0xB4, 0xC8
MATERIAL_COMPRESSED_SIZE, MATERIAL_COMPRESSED_ADDRESS = 0xEC, 0xF8
# the compressed environment vertex, and lightmap vertex (its incident radiosity, then u and v)
VERTEX_SIZE, LIGHTMAP_VERTEX_SIZE = 32, 8

OFFSET_BASIS, PRIME, MASK = 0xCBF29CE484222325, 0x100000001B3, (1 << 64) - 1
VERSION, NONE_WORD = 0x20316D6C, 0x656E6F6E  # the words of "lm1 " and "none"


def fingerprint(lightmaps):
    """The fingerprint (mod_lightmap_fingerprint) of a BSP's lightmaps: a list of (page index, materials), each
    material (surface count, corners), corners None for a material whose vertices cannot be read, else each
    surface's three corners' (u, v) as the compressed lightmap vertex keeps them (shorts), or None past its
    vertices."""
    value = OFFSET_BASIS

    def word(number):
        nonlocal value
        for byte in struct.pack("<I", number & 0xFFFFFFFF):
            value = ((value ^ byte) * PRIME) & MASK

    word(VERSION)
    word(max([page + 1 for page, _ in lightmaps] + [0]))
    for page, materials in lightmaps:
        if page < 0:
            continue
        word(page)
        word(len(materials))
        for surface_count, corners in materials:
            word(surface_count)
            if corners is None:
                word(NONE_WORD)
                continue
            for corner in corners:
                if corner is None:
                    word(NONE_WORD)
                    word(NONE_WORD)
                else:
                    word(corner[0])
                    word(corner[1])
    return value


def text(value):
    return f"lm1-{value:016x}"


class TagCache:
    """A map's tags where the game reads them, and its BSPs read there one by one."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        if len(data) < HEADER_SIZE or data[:4] != b"daeh":
            raise ValueError(f"{path}: not an Xbox map")
        length, self.tag_data_offset, self.tag_data_size = struct.unpack_from("<i4xii", data, 8)
        if len(data) != length:
            data = data[:HEADER_SIZE] + zlib.decompress(data[HEADER_SIZE:])
        self.file = data
        self.memory = bytearray(TAG_CACHE_SIZE)
        self.memory[:self.tag_data_size] = data[self.tag_data_offset:self.tag_data_offset + self.tag_data_size]

    def u32(self, address):
        return struct.unpack_from("<I", self.memory, address - TAG_CACHE_BASE)[0]

    def i32(self, address):
        return struct.unpack_from("<i", self.memory, address - TAG_CACHE_BASE)[0]

    def i16(self, address):
        return struct.unpack_from("<h", self.memory, address - TAG_CACHE_BASE)[0]

    def bytes(self, address, size):
        return bytes(self.memory[address - TAG_CACHE_BASE:address - TAG_CACHE_BASE + size])

    def name(self, address):
        start = address - TAG_CACHE_BASE
        return self.memory[start:self.memory.index(0, start)].decode("latin-1")

    def tag_name(self, tag_index):
        instances, _, _, count = struct.unpack_from("<IiIi", self.memory, 0)
        index = tag_index & 0xFFFF
        if not 0 <= index < count:
            return "(none)"
        return self.name(self.u32(instances + index * 0x20 + 0x10))

    def scenario(self):
        instances, scenario_index, _, _ = struct.unpack_from("<IiIi", self.memory, 0)
        return self.u32(instances + (scenario_index & 0xFFFF) * 0x20 + 0x14)

    def bsps(self):
        """(index, its structure's address, its lightmap bitmap's tag index), each BSP read in turn"""
        scenario = self.scenario()
        count, references = self.i32(scenario + SCENARIO_STRUCTURE_BSPS), self.u32(scenario + SCENARIO_STRUCTURE_BSPS + 4)
        for index in range(count):
            file_offset, file_size, base = struct.unpack_from("<iiI", self.memory,
                                                              references + index * 0x20 - TAG_CACHE_BASE)
            start = base - TAG_CACHE_BASE
            self.memory[start:start + file_size] = self.file[file_offset:file_offset + file_size]
            structure = self.u32(base)
            yield index, structure, self.i32(structure + STRUCTURE_BSP_LIGHTMAP_TAG_INDEX)


def lightmaps_read(cache, structure):
    """the BSP's lightmaps as fingerprint() takes them"""
    surface_count, surfaces = cache.i32(structure + STRUCTURE_BSP_SURFACES), cache.u32(structure + STRUCTURE_BSP_SURFACES + 4)
    count, address = cache.i32(structure + STRUCTURE_BSP_LIGHTMAPS), cache.u32(structure + STRUCTURE_BSP_LIGHTMAPS + 4)
    lightmaps = []
    for lightmap in range(count):
        element = address + lightmap * LIGHTMAP_SIZE
        page = cache.i16(element)
        material_count = cache.i32(element + LIGHTMAP_MATERIALS)
        materials_address = cache.u32(element + LIGHTMAP_MATERIALS + 4)
        materials = []
        for index in range(material_count if page >= 0 else 0):
            material = materials_address + index * MATERIAL_SIZE
            first, surfaces_of = cache.i32(material + MATERIAL_FIRST_SURFACE), cache.i32(material + MATERIAL_SURFACE_COUNT)
            vertices = cache.i32(material + MATERIAL_VERTEX_COUNT)
            size, data = cache.i32(material + MATERIAL_COMPRESSED_SIZE), cache.u32(material + MATERIAL_COMPRESSED_ADDRESS)
            if not data or vertices <= 0 or cache.i32(material + MATERIAL_LIGHTMAP_VERTEX_COUNT) != vertices or \
                    size < vertices * (VERTEX_SIZE + LIGHTMAP_VERTEX_SIZE) or surfaces_of < 0 or first < 0 or \
                    first > surface_count - surfaces_of:
                materials.append((surfaces_of, None))
                continue
            coordinates = cache.bytes(data + vertices * VERTEX_SIZE, vertices * LIGHTMAP_VERTEX_SIZE)
            triangles = cache.bytes(surfaces + first * 6, surfaces_of * 6)
            corners = []
            for vertex in struct.unpack(f"<{surfaces_of * 3}H", triangles):
                corners.append(struct.unpack_from("<hh", coordinates, vertex * LIGHTMAP_VERTEX_SIZE + 4)
                               if vertex < vertices else None)
            materials.append((surfaces_of, corners))
        lightmaps.append((page, materials))
    return lightmaps


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("map", help="an Xbox map (maps/<name>.map)")
    parser.add_argument("--write", metavar="MOD", help="write each BSP's layout file into MOD/tags")
    arguments = parser.parse_args(argv)
    try:
        cache = TagCache(arguments.map)
    except (OSError, ValueError, zlib.error) as failure:
        print(failure, file=sys.stderr)
        return 1
    for index, structure, lightmap_tag in cache.bsps():
        name = cache.tag_name(lightmap_tag)
        value = text(fingerprint(lightmaps_read(cache, structure)))
        print(f"bsp {index}: {name} {value}")
        if arguments.write and lightmap_tag != -1:
            parts = name.split("\\")
            path = Path(arguments.write, "tags", *parts[:-1], parts[-1] + ".lightmap_layout")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"# the lightmap layout of {Path(arguments.map).stem}'s BSP {index}\n{value}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
