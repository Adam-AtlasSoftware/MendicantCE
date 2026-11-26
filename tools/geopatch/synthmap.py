"""Synthetic Xbox cache files for the asset-free tests (CI on Linux and Windows): a map the game's walk
(tools/prism_bake/ce_map.py, retail.py) reads like a retail one, with made-up geometry from a seed. No game data:
every byte is generated here.

The layout is the retail one at its smallest: the 0x800-byte header ('head', version 5, the inflated length, tag
data offset and size, name, build, type, checksum at 0x64, 'foot'), the tag data at the tag cache's address
(0x803A6000: tag header, two tag instances, the scenario with its structure_bsps block), then each BSP placed at its
own base address: the structure_bsp, its collision BSP (planes, vertices), surfaces, one lightmap with materials,
each material's compressed vertices (32 bytes) and compressed lightmap vertices (8 bytes).
"""
import random
import struct
import zlib

BASE = 0x803A6000
HEADER = 0x800
SCNR_SIZE = 0x5B0


def _tag(text):
    return struct.unpack(">I", text.encode())[0]


def pack_vector(x, y, z):
    """a unit vector packed 11:11:10 as the game compresses it (nearest field value)"""
    def field(value, bits):
        top = (1 << (bits - 1)) - 1
        s = int(round((value * ((1 << bits) - 1) - 1) / 2))
        s = max(-top - 1, min(top, s))
        return s & ((1 << bits) - 1)
    return field(x, 11) | field(y, 11) << 11 | field(z, 10) << 22


class Geometry:
    """made-up BSP content: per material, vertices [(x, y, z, packed normal, binormal, tangent, u, v, lm packed,
    lu, lv)] and surfaces [(a, b, c)]; collision vertices and planes"""

    def __init__(self, seed=1, materials=2, size=6, offset=(0.0, 0.0, 0.0)):
        rng = random.Random(seed)
        self.materials = []
        for m in range(materials):
            vertices = []
            for j in range(size):
                for i in range(size):
                    x = offset[0] + m * 40.0 + i * 3.1 + rng.uniform(-0.4, 0.4)
                    y = offset[1] + j * 3.3 + rng.uniform(-0.4, 0.4)
                    z = offset[2] + rng.uniform(-2.0, 2.0)
                    n = pack_vector(rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), 0.97)
                    b = pack_vector(1.0, 0.0, 0.0)
                    t = pack_vector(0.0, 1.0, 0.0)
                    vertices.append((x, y, z, n, b, t, i / size + rng.uniform(0, 0.01), j / size, n,
                                     rng.randint(-32768, 32767), rng.randint(-32768, 32767)))
            surfaces = []
            for j in range(size - 1):
                for i in range(size - 1):
                    a = j * size + i
                    surfaces.append((a, a + 1, a + size))
                    surfaces.append((a + 1, a + size + 1, a + size))
            self.materials.append((vertices, surfaces))
        self.collision_vertices = [(v[0], v[1], v[2]) for v in self.materials[0][0]]
        self.planes = []
        for k in range(8):
            i, j = rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3)
            norm = (i * i + j * j + 1.0) ** 0.5
            self.planes.append((i / norm, j / norm, 1.0 / norm, rng.uniform(-5, 5)))


class _Blob:
    """bytes placed at an address, growing; returns the address of each piece"""

    def __init__(self, base):
        self.base = base
        self.data = bytearray()

    def add(self, data, align=4):
        while len(self.data) % align:
            self.data += b"\0"
        address = self.base + len(self.data)
        self.data += data
        return address

    def put(self, address, data):
        at = address - self.base
        self.data[at:at + len(data)] = data


def _bsp_blob(base, geometry):
    blob = _Blob(base)
    blob.add(b"\0" * 4)  # the structure_bsp's address, first
    sbsp = blob.add(b"\0" * 0x300)
    blob.put(base, struct.pack("<I", sbsp))
    # surfaces: every material's, in order
    surfaces = bytearray()
    firsts = []
    for vertices, tris in geometry.materials:
        firsts.append(len(surfaces) // 6)
        for tri in tris:
            surfaces += struct.pack("<HHH", *tri)
    surfaces_address = blob.add(bytes(surfaces))
    blob.put(sbsp + 0xF8, struct.pack("<iI", len(surfaces) // 6, surfaces_address))
    # collision BSP: planes and vertices
    planes = b"".join(struct.pack("<4f", *plane) for plane in geometry.planes)
    planes_address = blob.add(planes)
    cverts = b"".join(struct.pack("<3fi", x, y, z, -1) for x, y, z in geometry.collision_vertices)
    cverts_address = blob.add(cverts)
    collision = bytearray(0x60)
    struct.pack_into("<iI", collision, 0x0C, len(geometry.planes), planes_address)
    struct.pack_into("<iI", collision, 0x54, len(geometry.collision_vertices), cverts_address)
    collision_address = blob.add(bytes(collision))
    blob.put(sbsp + 0xB0, struct.pack("<iI", 1, collision_address))
    blob.put(sbsp + 0x00, struct.pack("<IIIi", 0, 0, 0, -1))
    # one lightmap, its materials
    material_bytes = []
    for (vertices, tris), first in zip(geometry.materials, firsts):
        vdata = b"".join(struct.pack("<3f3I2f", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]) for v in vertices)
        ldata = b"".join(struct.pack("<Ihh", v[8], v[9], v[10]) for v in vertices)
        data_address = blob.add(vdata + ldata)
        vhw = blob.add(struct.pack("<II", 0, data_address))
        lhw = blob.add(struct.pack("<II", 0, data_address + len(vdata)))
        material = bytearray(0x100)
        struct.pack_into("<ii", material, 0x14, first, len(tris))
        struct.pack_into("<hHiiII", material, 0xB0, 1, 0, len(vertices), 0, 0, vhw)
        struct.pack_into("<hHiiII", material, 0xC4, 3, 0, len(vertices), 0, 0, lhw)
        struct.pack_into("<iIiII", material, 0xEC, len(vdata) + len(ldata), 0, 0, data_address, 0)
        material_bytes.append(bytes(material))
    materials_address = blob.add(b"".join(material_bytes))
    lightmap = bytearray(0x20)
    struct.pack_into("<h", lightmap, 0, 0)
    struct.pack_into("<iI", lightmap, 0x14, len(material_bytes), materials_address)
    lightmap_address = blob.add(bytes(lightmap))
    blob.put(sbsp + 0x104, struct.pack("<iI", 1, lightmap_address))
    return bytes(blob.data)


def build(name="synth", geometries=None, build_string="01.10.12.2276", checksum=0x5EED0001):
    """an inflated map's bytes (as a cache slot holds it, without the slot's padding)"""
    geometries = geometries or [Geometry(1)]
    tag_ids = [0x10000] + [0x10001 + i for i in range(len(geometries))]
    tags = _Blob(BASE)
    tags.add(b"\0" * 0x28)  # tag header
    instances_address = tags.add(b"\0" * (32 * len(tag_ids)))
    scenario_name = tags.add(b"levels\\synth\\synth\0")
    bsp_names = [tags.add(("levels\\synth\\synth_bsp%d\0" % i).encode()) for i in range(len(geometries))]
    scenario = tags.add(b"\0" * SCNR_SIZE)
    references_address = tags.add(b"\0" * (0x20 * len(geometries)))
    tags.put(scenario + 0x5A4, struct.pack("<iI", len(geometries), references_address))
    instances = struct.pack("<IIIiIIII", _tag("scnr"), 0, 0, tag_ids[0], scenario_name, scenario, 0, 0)
    for i in range(len(geometries)):
        instances += struct.pack("<IIIiIIII", _tag("sbsp"), 0, 0, tag_ids[1 + i], bsp_names[i], 0, 0, 0)
    tags.put(instances_address, instances)
    tags.put(BASE, struct.pack("<IiIi", instances_address, tag_ids[0], 0xC0FFEE00, len(tag_ids)))
    tag_data_size = (len(tags.data) + 0xFFF) & ~0xFFF
    tags.data += b"\0" * (tag_data_size - len(tags.data))
    # the BSPs after the tag data in the file, each at an address past the tag data
    file_offset = HEADER + tag_data_size
    address = BASE + tag_data_size
    blobs = []
    for index, geometry in enumerate(geometries):
        blob = _bsp_blob(address, geometry)
        tags.put(references_address + 0x20 * index,
                 struct.pack("<iiIIIIii", file_offset, len(blob), address, 0, 0, 0, 0, tag_ids[1 + index]))
        blobs.append(blob)
        file_offset += len(blob)
    body = bytes(tags.data) + b"".join(blobs)
    length = HEADER + len(body)
    header = bytearray(HEADER)
    struct.pack_into("<III", header, 0, 0x68656164, 5, length)
    struct.pack_into("<ii", header, 0x10, HEADER, tag_data_size)
    header[0x20:0x20 + len(name)] = name.encode()
    header[0x40:0x40 + len(build_string)] = build_string.encode()
    struct.pack_into("<iI", header, 0x60, 0, checksum)
    struct.pack_into("<I", header, 0x7FC, 0x666F6F74)
    return bytes(header) + body


def compress(inflated, level=6):
    """the disc form: the header, then one zlib stream of the rest"""
    return inflated[:HEADER] + zlib.compress(inflated[HEADER:], level)


def slot(inflated):
    """a cache slot's form: the inflated map and zeros to 64 KB"""
    size = (len(inflated) + 0xFFFF) & ~0xFFFF
    return inflated + b"\0" * (size - len(inflated))
