"""What the geopatch tools read of a retail Xbox map: per structure BSP, each lightmap material's stock vertices (the
32-byte compressed environment vertices and 8-byte compressed lightmap vertices Xbox BSPs hold: VERIFY.md V1), its
surfaces, the collision BSP's vertices and planes, and the BSP's own bytes. Read through tools/prism_bake/ce_map.py
(the game's walk: every address checked), on the farm from the retail maps it keeps, in the tests from synthetic
maps (synthmap.py). Nothing read here is ever written into a sidecar: the scanner's sets are of hashes and
quantized tuples, held in memory.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "prism_bake"))
import ce_map  # noqa: E402

from . import geom  # noqa: E402

COLLISION_VERTICES, COLLISION_PLANES = 0x54, 0x0C
WINDOW = 48


class Material:
    def __init__(self, lightmap_index, material_index, first_surface, surface_count, vertices, lightmap_vertices,
                 surfaces):
        self.lightmap_index = lightmap_index
        self.material_index = material_index
        self.first_surface = first_surface
        self.surface_count = surface_count
        self.vertices = vertices                    # bytes, 32 per vertex (compressed) or 56 (uncompressed)
        self.vertex_size = 32
        self.lightmap_vertices = lightmap_vertices  # bytes, 8 per vertex, or b""
        self.surfaces = surfaces                    # [(a, b, c)] of the material's surfaces

    @property
    def vertex_count(self):
        return len(self.vertices) // self.vertex_size

    def position(self, index):
        return struct.unpack_from("<3f", self.vertices, index * self.vertex_size)

    def topology_check(self):
        """the .pwg's: sum over the material's surfaces s of (3a + 5b + 7c)(s + 1), mod 2^32"""
        total = 0
        for s, (a, b, c) in enumerate(self.surfaces):
            total += (3 * a + 5 * b + 7 * c) * (s + 1)
        return total & 0xFFFFFFFF


class RetailBsp:
    def __init__(self, index, name, raw, materials, collision_vertices, planes):
        self.index = index
        self.name = name
        self.raw = raw                              # the BSP's bytes as the map holds them
        self.materials = materials                  # {(lightmap index, material index): Material}
        self.collision_vertices = collision_vertices  # [(x, y, z)]
        self.planes = planes                        # [(i, j, k, d)]


def read_map(path):
    """(the map's header checksum, build, [RetailBsp])"""
    m = ce_map.Map(str(path))
    c = m.cache
    out = []
    references = c.block(m.scenario + ce_map.SCNR_STRUCTURE_BSPS, 0x20, 32)
    for index in range(len(references)):
        file_offset, file_size, base, _, _, _, _, tag_id = struct.unpack("<iiIIIIii", c.read(references[index], 32))
        if file_offset < 0 or file_size <= 0 or file_offset > m.length - file_size:
            raise ce_map.MapError("structure bsp %d is outside the map" % index)
        raw = m.file[file_offset:file_offset + file_size]
        c.loaded = c.loaded[:1]
        c.place(base, raw)
        bsp = c.u32(base)
        surfaces_count, surfaces = c.block_bytes(bsp + ce_map.SBSP_SURFACES, 6)
        materials = {}
        for li, lightmap in enumerate(c.block(bsp + ce_map.SBSP_LIGHTMAPS, 0x20, 4096)):
            for mi, material in enumerate(c.block(lightmap + 0x14, 0x100, 65536)):
                first, count = struct.unpack("<ii", c.read(material + 0x14, 8))
                vtype, _, vcount, _, _, vhw = struct.unpack("<hHiiII", c.read(material + 0xB0, 20))
                ltype, _, lcount, _, _, lhw = struct.unpack("<hHiiII", c.read(material + 0xC4, 20))
                if vcount <= 0 or count <= 0 or first < 0 or first + count > surfaces_count:
                    continue
                if vtype != 1:
                    raise ce_map.MapError("a BSP vertex buffer of type %d (Xbox BSPs hold type 1)" % vtype)
                vertices = c.read(c.u32(vhw + 4), vcount * 32)
                lightmap_vertices = b""
                if lcount > 0 and lhw:
                    if ltype != 3:
                        raise ce_map.MapError("a lightmap vertex buffer of type %d (Xbox BSPs hold type 3)" % ltype)
                    lightmap_vertices = c.read(c.u32(lhw + 4), lcount * 8)
                tris = [struct.unpack_from("<HHH", surfaces, s * 6) for s in range(first, first + count)]
                materials[(li, mi)] = Material(li, mi, first, count, vertices, lightmap_vertices, tris)
        collision = c.block(bsp + ce_map.SBSP_COLLISION_BSP, 0x60, 1)
        collision_vertices, planes = [], []
        if collision:
            n, data = c.block_bytes(collision[0] + COLLISION_VERTICES, 16)
            collision_vertices = [struct.unpack_from("<3f", data, k * 16) for k in range(n)]
            n, data = c.block_bytes(collision[0] + COLLISION_PLANES, 16)
            planes = [struct.unpack_from("<4f", data, k * 16) for k in range(n)]
        out.append(RetailBsp(index, m.tag_name(tag_id), raw, materials, collision_vertices, planes))
    return struct.unpack_from("<I", m.file, 0x64)[0], m.build, out


PLAIN_BYTES = 10


def _plain(window):
    """a window too regular to mean anything (zeros, a few repeated values: fewer than PLAIN_BYTES distinct bytes),
    never a finding; any run of real floats or indices has far more"""
    return len(set(window)) < PLAIN_BYTES


ROLL_BASE = 0x100000001B3  # odd: invertible modulo 2^64


def rolling_keys(data, stride=1):
    """the position-independent 64-bit polynomial key of every 48-byte window of data starting at a multiple of
    stride (sum over k of byte[i + k] * B^-k, modulo 2^64): numpy, vectorised"""
    import numpy as np
    a = np.frombuffer(bytes(data), np.uint8).astype(np.uint64)
    n = len(a) - WINDOW + 1
    if n <= 0:
        return np.zeros(0, np.uint64)
    inverse = pow(ROLL_BASE, -1, 1 << 64)
    with np.errstate(over="ignore"):
        inv_powers = np.cumprod(np.full(len(a), inverse, np.uint64))
        inv_powers = np.concatenate([np.ones(1, np.uint64), inv_powers[:-1]])
        prefix = np.concatenate([np.zeros(1, np.uint64), np.cumsum(a * inv_powers, dtype=np.uint64)])
        starts = np.arange(0, n, stride)
        sums = prefix[starts + WINDOW] - prefix[starts]
        powers = np.cumprod(np.full(len(a), ROLL_BASE, np.uint64))
        powers = np.concatenate([np.ones(1, np.uint64), powers[:-1]])
        return sums * powers[starts]


def plain_aligned(data):
    """for each 4-aligned 48-byte window of data, whether it is plain (_plain)"""
    import numpy as np
    raw = np.frombuffer(bytes(data), np.uint8)
    n = len(raw) - WINDOW + 1
    if n <= 0:
        return np.zeros(0, bool)
    out = np.empty((n + 3) // 4, bool)
    step = 1 << 16
    for first in range(0, len(out), step):
        starts = np.arange(first, min(first + step, len(out))) * 4
        view = raw[starts[:, None] + np.arange(WINDOW)[None, :]]
        ordered = np.sort(view, axis=1)
        distinct = 1 + (ordered[:, 1:] != ordered[:, :-1]).sum(axis=1)
        out[first:first + len(starts)] = distinct < PLAIN_BYTES
    return out


class RetailSets:
    """the scanner's sets of every retail tuple of the maps given (hashes and quantized tuples, in memory only)"""

    def __init__(self):
        self.position = set()
        self.collision_vertex = set()
        self.plane = set()
        self.normal = set()
        self.texcoord = set()
        self._window_parts = []
        self._windows = None

    def add_bsp(self, bsp, windows=True):
        for material in bsp.materials.values():
            for v in range(material.vertex_count):
                at = v * 32
                self.position.add(tuple(geom.grid_of(x) for x in struct.unpack_from("<3f", material.vertices, at)))
                self.normal.add(struct.unpack_from("<I", material.vertices, at + 12)[0])
                self.texcoord.add(struct.unpack_from("<2I", material.vertices, at + 24))
        for point in bsp.collision_vertices:
            self.collision_vertex.add(tuple(geom.grid_of(x) for x in point))
            # (a collision vertex is a position too: a render vertex copied from collision is caught)
            self.position.add(tuple(geom.grid_of(x) for x in point))
        for plane in bsp.planes:
            self.plane.add(geom.plane_key(*plane))
        if windows:
            self._add_windows(bsp.raw)

    def add_decoded(self, uncompressed):
        """retail vertices also in their decoded (56-byte) form, for the window scan: a compressed-to-uncompressed
        copy is caught by its bytes as well as by its positions"""
        self._add_windows(uncompressed)

    def _add_windows(self, data):
        keys = rolling_keys(data, 4)
        plain = plain_aligned(data)[:len(keys)]
        self._window_parts.append(keys[~plain])
        self._windows = None

    @property
    def windows(self):
        """the sorted keys of every retail window that is not plain"""
        import numpy as np
        if self._windows is None:
            self._windows = np.unique(np.concatenate(self._window_parts)) if self._window_parts else \
                np.zeros(0, np.uint64)
        return self._windows

    def window_hits(self, data):
        """offsets in data where a 48-byte window equals a retail one (plain windows never count)"""
        import numpy as np
        if not len(self.windows):
            return []
        hits = []
        chunk = 1 << 22
        # (in pieces that overlap by a window less one byte, so every window is seen once)
        for start in range(0, max(len(data) - WINDOW + 1, 0), chunk):
            piece = data[start:start + chunk + WINDOW - 1]
            keys = rolling_keys(piece, 1)
            found = np.nonzero(np.isin(keys, self.windows))[0]
            hits += [start + int(at) for at in found if not _plain(bytes(piece[at:at + WINDOW]))]
        return hits

    def of(self, kind):
        return getattr(self, kind)
