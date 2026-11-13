#!/usr/bin/env python3
"""What PRISM's baker reads of a map, from an Xbox cache file, for the baker's offline checks
(tools/prism_bake/bake_scene.py, tools/test_prism_bake.py): never the game's data itself, only numbers read from
the player's own maps at run time.

The map is read as the game reads it (tools/prism_packs/extract/map_extract.c has the same walk in C): the file
inflated after its 0x800-byte header, its tag data placed at the tag cache's address (0x803A6000), and each
structure bsp at the base address its scenario reference names. Every address is checked against what was
loaded, so a malformed map raises MapError and never reads outside it.

What is read (the layouts are the game's: source/scenario/scenario_definitions.h,
source/structures/structure_bsp_definitions.h, source/physics/collision_bsp_definitions.h, the sizes checked in
port/linux/game/tag_schema_scenario.c):
- the scenario's seeds for the reachable space (design section 3.3): player starting locations, netgame flags and
  equipment, vehicles, bipeds, weapons and equipment placed, the AI's starting locations and firing positions,
  the encounters' player starting locations, the cutscene flags and camera points; and whether its vehicle
  palette has a flying vehicle (a vehicle tag's type: human plane or alien fighter);
- per structure bsp: the collision bsp's bsp3d (nodes, planes) and leaves, the structure leaves' clusters and
  surface references, the clusters' PVS, the world bounds, and the render triangles with their positions,
  lightmap UVs and lightmap page (each lightmap's bitmap index, its page's size from the lightmap bitmap tag), and
  its water planes (the fog planes with a material).
"""
import struct
import zlib

TAG_CACHE_BASE = 0x803A6000
TAG_CACHE_SIZE = 0x1600000
HEADER_SIZE = 0x800
NONE = -1


class MapError(Exception):
    pass


def _tag(text):
    return struct.unpack(">I", text.encode())[0]


SCNR, SBSP, BITM, VEHI = _tag("scnr"), _tag("sbsp"), _tag("bitm"), _tag("vehi")

# scenario (scenario_definitions.h; offsets computed from the structure, structure_bsps checked at 0x5A4)
SCNR_VEHICLES, SCNR_VEHICLE_PALETTE = 0x240, 0x24C
SCNR_BIPEDS = 0x228
SCNR_EQUIPMENT, SCNR_WEAPONS = 0x258, 0x270
SCNR_PLAYERS, SCNR_NETGAME_FLAGS, SCNR_NETGAME_EQUIPMENT = 0x354, 0x378, 0x384
SCNR_ENCOUNTERS = 0x42C
SCNR_CUTSCENE_FLAGS, SCNR_CUTSCENE_CAMERA_POINTS = 0x4E4, 0x4F0
SCNR_STRUCTURE_BSPS = 0x5A4
SCNR_SIZE = 0x5B0

# structure bsp
SBSP_LIGHTMAP_GROUP = 0x00
SBSP_COLLISION_BSP = 0xB0
SBSP_WORLD_BOUNDS = 0xC8
SBSP_LEAVES = 0xE0
SBSP_SURFACE_REFERENCES = 0xEC
SBSP_SURFACES = 0xF8
SBSP_LIGHTMAPS = 0x104
SBSP_CLUSTERS = 0x134
SBSP_CLUSTER_DATA = 0x140
SBSP_FOG_PLANES = 0x178

# a vehicle tag's type (vehicle_definitions: the object's 0x17C and the unit's 0x174 bytes, the vehicle's flags, its type at
# 0x2F4): human tank, jeep, boat, plane, alien scout, fighter, turret
VEHI_TYPE = 0x2F4
FLYING_VEHICLE_TYPES = (3, 5)

SEED_KINDS = ("player_start", "netgame_flag", "netgame_equipment", "vehicle", "biped", "weapon", "equipment",
              "ai_start", "ai_firing", "encounter_player_start", "cutscene_flag", "cutscene_camera")


def _uncompress_vector(packed):
    """a normal packed in 11, 11 and 10 bits (map_extract.c's uncompress_vector)"""
    def signed(value, bits):
        return value - (1 << bits) if value & (1 << (bits - 1)) else value
    x = (signed(packed & 0x7FF, 11) * 2.0 + 1.0) / 2047.0
    y = (signed((packed >> 11) & 0x7FF, 11) * 2.0 + 1.0) / 2047.0
    z = (signed((packed >> 22) & 0x3FF, 10) * 2.0 + 1.0) / 1023.0
    return (x, y, z)


class Cache:
    """the tag cache as the game has it: bytes at addresses, every read bounds checked"""

    def __init__(self, size):
        self.bytes = bytearray(size)
        self.loaded = []  # (start, end) of what was placed

    def place(self, address, data):
        offset = address - TAG_CACHE_BASE
        if offset < 0 or offset + len(data) > len(self.bytes):
            raise MapError("data placed outside the tag cache")
        self.bytes[offset:offset + len(data)] = data
        self.loaded.append((offset, offset + len(data)))

    def read(self, address, size):
        offset = address - TAG_CACHE_BASE
        if size < 0 or not any(start <= offset and offset + size <= end for start, end in self.loaded):
            raise MapError("read of %d bytes at 0x%08X outside what is loaded" % (size, address))
        return bytes(self.bytes[offset:offset + size])

    def u32(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def i32(self, address):
        return struct.unpack("<i", self.read(address, 4))[0]

    def block(self, address, element_size, limit=1 << 22):
        """a tag block's elements' addresses: (count, address) at address"""
        count, base = struct.unpack("<iI", self.read(address, 8))
        if count < 0 or count > limit:
            raise MapError("a block of %d elements" % count)
        if count:
            self.read(base, count * element_size)
        return [base + index * element_size for index in range(count)]

    def block_bytes(self, address, element_size, limit=1 << 22):
        count, base = struct.unpack("<iI", self.read(address, 8))
        if count < 0 or count > limit:
            raise MapError("a block of %d elements" % count)
        return count, (self.read(base, count * element_size) if count else b"")


class Map:
    def __init__(self, path):
        raw = open(path, "rb").read()
        if len(raw) < HEADER_SIZE:
            raise MapError("shorter than its header")
        signature, version, length = struct.unpack_from("<III", raw, 0)
        tag_data_offset, tag_data_size = struct.unpack_from("<ii", raw, 0x10)
        footer = struct.unpack_from("<I", raw, 0x7FC)[0]
        if signature != _tag("head") or footer != _tag("foot") or version != 5:
            raise MapError("not an Xbox cache file")
        if length < HEADER_SIZE or length > 0x40000000:
            raise MapError("its length is out of range")
        self.name = raw[0x20:0x40].split(b"\0")[0].decode("ascii", "replace")
        self.build = raw[0x40:0x60].split(b"\0")[0].decode("ascii", "replace")
        self.map_type = struct.unpack_from("<i", raw, 0x60)[0]
        if len(raw) == length:
            body = raw[HEADER_SIZE:]
        else:
            body = zlib.decompress(raw[HEADER_SIZE:])
            if len(body) != length - HEADER_SIZE:
                raise MapError("inflated to the wrong length")
        self.file = raw[:HEADER_SIZE] + body
        self.length = length
        if tag_data_offset < 0 or tag_data_size <= 0 or tag_data_size > TAG_CACHE_SIZE or \
                tag_data_offset > length - tag_data_size:
            raise MapError("its tag data is outside it")
        self.tag_data_size = tag_data_size
        self.cache = Cache(TAG_CACHE_SIZE)
        self.cache.place(TAG_CACHE_BASE, self.file[tag_data_offset:tag_data_offset + tag_data_size])
        c = self.cache
        instances, self.scenario_index, self.checksum, tag_count = struct.unpack("<IiIi", c.read(TAG_CACHE_BASE, 16))
        if tag_count <= 0 or tag_count > 65535:
            raise MapError("tag count out of range")
        self.tags = []
        for index in range(tag_count):
            group, _, _, tag_id, name, base, _, _ = struct.unpack("<IIIiIIII", c.read(instances + index * 32, 32))
            self.tags.append((group, tag_id, name, base))
        if not 0 <= self.scenario_index & 0xFFFF < tag_count:
            raise MapError("no scenario")
        self.scenario = self.tag_base(self.scenario_index, SCNR)
        c.read(self.scenario, SCNR_SIZE)

    def tag_base(self, tag_id, group):
        if tag_id == NONE:
            return None
        index = tag_id & 0xFFFF
        if not 0 <= index < len(self.tags) or self.tags[index][0] != group:
            raise MapError("tag %08X is not a %08X" % (tag_id & 0xFFFFFFFF, group))
        return self.tags[index][3]

    def tag_name(self, tag_id):
        index = tag_id & 0xFFFF
        address = self.tags[index][2]
        out = bytearray()
        while len(out) < 256:
            ch = self.cache.read(address + len(out), 1)
            if ch == b"\0":
                break
            out += ch
        return out.decode("ascii", "replace")

    # ---------- the scenario

    def seeds(self):
        """[(kind, x, y, z)] of every point of the scenario the reachable space starts from"""
        c, s = self.cache, self.scenario
        out = []

        def points(field, size, kind, position_offset=0):
            for address in c.block(s + field, size):
                out.append((kind,) + struct.unpack("<fff", c.read(address + position_offset, 12)))

        points(SCNR_PLAYERS, 0x34, "player_start")
        points(SCNR_NETGAME_FLAGS, 0x94, "netgame_flag")
        points(SCNR_NETGAME_EQUIPMENT, 0x90, "netgame_equipment", 0x40)
        points(SCNR_VEHICLES, 0x78, "vehicle", 0x08)
        points(SCNR_BIPEDS, 0x78, "biped", 0x08)
        points(SCNR_WEAPONS, 0x5C, "weapon", 0x08)
        points(SCNR_EQUIPMENT, 0x28, "equipment", 0x08)
        for encounter in c.block(s + SCNR_ENCOUNTERS, 0xB0, 1024):
            for squad in c.block(encounter + 0x80, 0xE8, 1024):
                for location in c.block(squad + 0xD0, 0x1C, 1024):
                    out.append(("ai_start",) + struct.unpack("<fff", c.read(location, 12)))
            for position in c.block(encounter + 0x98, 0x18, 4096):
                out.append(("ai_firing",) + struct.unpack("<fff", c.read(position, 12)))
            for location in c.block(encounter + 0xA4, 0x34, 1024):
                out.append(("encounter_player_start",) + struct.unpack("<fff", c.read(location, 12)))
        points(SCNR_CUTSCENE_FLAGS, 0x5C, "cutscene_flag", 0x24)
        points(SCNR_CUTSCENE_CAMERA_POINTS, 0x68, "cutscene_camera", 0x28)
        return out

    def vehicle_palette(self):
        """[(tag name, vehicle type)] of the scenario's vehicle palette"""
        out = []
        for entry in self.cache.block(self.scenario + SCNR_VEHICLE_PALETTE, 0x30):
            group, _, _, tag_id = struct.unpack("<IIii", self.cache.read(entry, 16))
            if tag_id == NONE or group != VEHI:
                continue
            base = self.tag_base(tag_id, VEHI)
            out.append((self.tag_name(tag_id), struct.unpack("<h", self.cache.read(base + VEHI_TYPE, 2))[0]))
        return out

    def has_flying_vehicle(self):
        return any(kind in FLYING_VEHICLE_TYPES for _, kind in self.vehicle_palette())

    def bsp_count(self):
        return len(self.cache.block(self.scenario + SCNR_STRUCTURE_BSPS, 0x20, 32))

    # ---------- a structure bsp

    def bsp(self, index):
        c = self.cache
        references = c.block(self.scenario + SCNR_STRUCTURE_BSPS, 0x20, 32)
        file_offset, file_size, base, _, _, _, _, tag_id = struct.unpack("<iiIIIIii", c.read(references[index], 32))
        if file_offset < 0 or file_size <= 0 or file_offset > self.length - file_size:
            raise MapError("structure bsp %d is outside the map" % index)
        if base - TAG_CACHE_BASE < self.tag_data_size:
            raise MapError("structure bsp %d loads over the tag data" % index)
        # (one bsp loaded at a time, as the game has it)
        c.loaded = c.loaded[:1]
        c.place(base, self.file[file_offset:file_offset + file_size])
        bsp_base = c.u32(base)
        return Bsp(self, index, self.tag_name(tag_id), bsp_base)


class Bsp:
    def __init__(self, map_, index, name, base):
        self.map = map_
        self.index = index
        self.name = name
        c = map_.cache
        self.world_bounds = struct.unpack("<6f", c.read(base + SBSP_WORLD_BOUNDS, 24))  # x0 x1 y0 y1 z0 z1
        collision = c.block(base + SBSP_COLLISION_BSP, 0x60, 1)
        if not collision:
            raise MapError("no collision bsp")
        cb = collision[0]
        n, self.nodes = c.block_bytes(cb + 0x00, 12)
        self.node_count = n
        n, self.planes = c.block_bytes(cb + 0x0C, 16)
        self.plane_count = n
        n, self.collision_leaves = c.block_bytes(cb + 0x18, 8)
        self.collision_leaf_count = n
        n, self.leaves = c.block_bytes(base + SBSP_LEAVES, 0x10)
        self.leaf_count = n
        n, self.surface_references = c.block_bytes(base + SBSP_SURFACE_REFERENCES, 8)
        self.surface_reference_count = n
        self.cluster_count = len(c.block(base + SBSP_CLUSTERS, 0x68, 8192))
        size, _, _, address = struct.unpack("<iIiI", c.read(base + SBSP_CLUSTER_DATA, 16))
        self.pvs = c.read(address, size) if size > 0 else b""
        # the water: the fog planes with a material (structure_fog_plane's runtime_material_type), (i, j, k, d)
        self.water_planes = []
        for plane in c.block(base + SBSP_FOG_PLANES, 0x20, 1024):
            _, material, i, j, k, d = struct.unpack("<hh4f", c.read(plane, 20))
            if material >= 0:
                self.water_planes.append((i, j, k, d))
        surfaces_count, surfaces = c.block_bytes(base + SBSP_SURFACES, 6)
        group, _, _, lightmap_bitmap = struct.unpack("<IIii", c.read(base + SBSP_LIGHTMAP_GROUP, 16))
        self.pages = []  # (width, height) of each lightmap page
        if lightmap_bitmap != NONE:
            bitmap = map_.tag_base(lightmap_bitmap, BITM)
            for data in c.block(bitmap + 0x60, 0x30, 4096):
                self.pages.append(struct.unpack("<hh", c.read(data + 4, 4)))
        # the render triangles: [(surface index, page, (x,y,z)*3, (u,v)*3, the vertices' normals summed)]
        self.triangles = []
        for lightmap in c.block(base + SBSP_LIGHTMAPS, 0x20, 4096):
            page = struct.unpack("<h", c.read(lightmap, 2))[0]
            for material in c.block(lightmap + 0x14, 0x100, 65536):
                first, count = struct.unpack("<ii", c.read(material + 0x14, 8))
                vtype, _, vcount, _, _, vhw = struct.unpack("<hHiiII", c.read(material + 0xB0, 20))
                ltype, _, lcount, _, _, lhw = struct.unpack("<hHiiII", c.read(material + 0xC4, 20))
                if vcount <= 0 or count <= 0 or first < 0 or first + count > surfaces_count:
                    continue
                vsize = 32 if vtype == 1 else 56
                lsize = 8 if ltype == 3 else 20
                vdata = c.read(c.u32(vhw + 4), vcount * vsize)
                ldata = c.read(c.u32(lhw + 4), lcount * lsize) if (lcount > 0 and lhw) else None
                positions = [struct.unpack_from("<fff", vdata, v * vsize) for v in range(vcount)]
                if vsize == 32:
                    normals = [_uncompress_vector(struct.unpack_from("<I", vdata, v * 32 + 12)[0])
                               for v in range(vcount)]
                else:
                    normals = [struct.unpack_from("<fff", vdata, v * 56 + 12) for v in range(vcount)]
                uvs = None
                if ldata is not None and lcount >= vcount and page != NONE:
                    if lsize == 8:
                        uvs = [tuple((value * 2.0 + 1.0) / 65535.0 for value in
                                     struct.unpack_from("<hh", ldata, v * 8 + 4)) for v in range(vcount)]
                    else:
                        uvs = [struct.unpack_from("<ff", ldata, v * 20 + 12) for v in range(vcount)]
                for surface in range(first, first + count):
                    a, b, cc = struct.unpack_from("<HHH", surfaces, surface * 6)
                    if a >= vcount or b >= vcount or cc >= vcount:
                        continue
                    normal = tuple(normals[a][k] + normals[b][k] + normals[cc][k] for k in range(3))
                    self.triangles.append((surface, page if uvs else NONE, positions[a], positions[b],
                                           positions[cc], uvs[a] if uvs else (0, 0), uvs[b] if uvs else (0, 0),
                                           uvs[cc] if uvs else (0, 0), normal))
        self.surface_count = surfaces_count
