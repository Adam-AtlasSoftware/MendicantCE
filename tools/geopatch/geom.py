"""The decoded layouts of GEOM (v1) and COLL (v0) sections, and their canonical tuples for the lint and the scanner.

Every value in a section is ours or a REF op. A vertex is either
  LIT: a position on the 1/1024 world unit grid (int32, exact as a float while |x| < 16384 wu), its normal,
       binormal and tangent packed 11:11:10 (the game's compressed vertex form, decoded exactly by
       prism_ref_decode.c) and its base map coordinates (two floats);
  REF: the index of a stock vertex of the same material (GEOM) or of the stock collision BSP (COLL), resolved by
       the engine from the tags in memory at BSP load (prism_ref_decode.c decodes the retail 32-byte vertex);
  REFP (GEOM): a stock vertex's position (by index, as REF) with our own packed vectors and coordinates;
  REFM (GEOM): the same from a stock vertex of another material of the BSP (lightmap and material index, vertex);
  REFC (GEOM): the same from a vertex of the BSP's stock collision BSP.
  REFP and REFM may carry the STOCK_TEX bit (REFPT, REFMT): the referenced stock vertex's base map coordinates
       too, so only our packed vectors are carried. A remodel vertex that kept a stock vertex's coordinates is
       encoded so; the lint refuses REFP/REFM coordinates equal to the referenced vertex's own (a copy).
Border vertices at remodel/stock borders are always REF or REFP (E16); a remodel vertex whose grid position equals
a stock vertex's of its material is encoded REFP, one equal to another material's REFM, one equal to a collision
vertex REFC (encode.py), so no LIT position equals a retail one of its BSP.

GEOM v1, one per BSP (little endian):
    "GEO1", u32 version (1), u32 part count, u32 column count, u32 part record size (PART_SIZE today; a reader
        requires at least the size it knows and skips the rest of each record, so later fields need no version)
    each part, part record size bytes: u16 lightmap index, u16 material index, u32 stock surface count, u32 stock
        vertex count, u32 topology check (sum over the material's surfaces s of (3a + 5b + 7c)(s + 1), mod 2^32:
        the .pwg's), u32 first surface, u32 surface count (the part's run of the material's surfaces; 0: a part
        tied to no stock surface, new content drawn beside the stock), u32 vertex count, u32 triangle count, u32 near
        triangle count, u32 far distance (float bits), u32 flags (1: far level), u32 static set (0: none; a set n
        is skipped at run time with its visibility layer, PLAN 4.4), u32 material namespace, u32 PRISM material id:
        material namespace STOCK (0): the part draws with the stock (lightmap index, material index) pair and is
            checked against it (counts, topology check), the PRISM material id 0;
        material namespace PRISM (1): the part draws with a PRISM material (its id in the PRISM material registry:
            bark, new rock, drifts), tied to no stock material or surface: lightmap and material index 0xFFFF,
            stock counts, topology check, first surface and surface count 0, and no REF/REFP vertex (no own
            stock material to refer to; REFM and REFC still name stock positions of the BSP)
    column table, 16 bytes each: fourcc, u32 element width, u32 element count, u32 byte size (a reader ignores a
        column it does not know, so a later column needs no version either)
    the columns, each byte-plane shuffled by its element width (all first bytes, then all second bytes, ...):
        VOPS u8   per vertex: 0 LIT, 1 REF, 2 REFP, 3 REFM, 4 REFC, 10 REFPT, 11 REFMT (REFP/REFM | STOCK_TEX 8)
        VREF u32  per vertex but LIT: the stock vertex's index (in its material, or collision's)
        VKEY u32  per REFM and REFMT vertex: its material, lightmap index << 16 | material index
        VPOS u32  per LIT vertex, x y z: zigzag of the grid delta from the previous LIT vertex's same axis
        VNRM u32  per vertex but REF: normal, binormal, tangent packed 11:11:10
        VTEX u32  per vertex but REF, REFPT and REFMT: u, v float bits
        TIDX u32  per triangle corner: zigzag of the delta from the previous corner's index (from 0 per part)
        SRUN u32  per part: surface count + 1 values, the first triangle of each of its surfaces (and the end; a
                  part with no surface has the one value 0)
        FRUN u32  per far part, per surface: its far triangles' first and count
        FSPH u32  per far part, per surface: bounding sphere centre and radius, float bits
LMUV v1, one per BSP beside its GEOM (the lightmap vertices the enhanced world draws with until M4's own lightmap
layout replaces this one; prism_geom.h has the layout the game reads): "LMU1", u32 version (1), u32 part count
(GEOM's), u32 column count, then per part u32 vertex count (its GEOM part's), the column table and the columns:
    LOPS u8   per vertex: 0 LIT, 1 REF
    LREF u32  per REF vertex: the stock lightmap vertex's index in the part's own material
    LVTX u32  per LIT vertex, two words: the packed incident radiosity, and lightmap u | v << 16 (the game's
              compressed lightmap vertex, rasterizer_geometry_compress_vertices's rounding)
COLL v0 (the enhanced collision, laid out in full in M8; v0 carries what the scanner checks):
    "COL0", u32 version (0), u32 0, u32 column count, column table, columns VOPS VREF VPOS (collision vertices:
    LIT or REF to a stock collision vertex), POPS (u8 per plane: 0 LIT, 1 REF), PREF (u32 per REF plane: the
    stock collision BSP's plane index) and PLNE (u32 per LIT plane: normal x y z and d, float bits). Enhanced
    collision follows the stock on walkable ground (PLAN section 10), so its floor planes are often the stock
    planes: they are REFs, and only a LIT plane equal to a retail one is refused.
"""
import struct

LIT, REF, REFP, REFM, REFC = 0, 1, 2, 3, 4
STOCK_TEX = 8
REFPT, REFMT = REFP | STOCK_TEX, REFM | STOCK_TEX
GEOM_OPS = (LIT, REF, REFP, REFM, REFC, REFPT, REFMT)
STOCK_POSITION = (REF, REFP, REFM, REFC, REFPT, REFMT)
# ops whose vertex carries no coordinates of its own
NO_TEX = (REF, REFPT, REFMT)
GRID = 1024
GRID_LIMIT = 1 << 24
GEOM_HEADER_SIZE = 20
PART_SIZE = 56
PART_FORMAT = "<HHIIIIIIIIIIIII"
NAMESPACE_STOCK, NAMESPACE_PRISM = 0, 1
NO_INDEX = 0xFFFF
GEOM_MAGIC, GEOM_VERSION = b"GEO1", 1
LMUV_MAGIC, LMUV_VERSION = b"LMU1", 1
COLL_MAGIC, COLL_VERSION = b"COL0", 0


class LayoutError(Exception):
    pass


def zigzag(value):
    return ((value << 1) ^ (value >> 31)) & 0xFFFFFFFF


def unzigzag(value):
    return (value >> 1) ^ -(value & 1)


def to_signed(value):
    return value - (1 << 32) if value & 0x80000000 else value


def shuffle(data, width):
    if width == 1:
        return bytes(data)
    return b"".join(bytes(data[plane::width]) for plane in range(width))


def unshuffle(data, width):
    if width == 1:
        return bytes(data)
    count = len(data) // width
    out = bytearray(len(data))
    for plane in range(width):
        out[plane::width] = data[plane * count:(plane + 1) * count]
    return bytes(out)


def grid_of(value):
    """a float position on the grid (round half to even of value * 1024, exact in double)"""
    q = round(value * GRID)
    if not -GRID_LIMIT < q < GRID_LIMIT:
        raise LayoutError("a position beyond 16384 world units")
    return q


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def bits_float(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


# ---------- columns

def _columns_bytes(columns):
    table = bytearray()
    blob = bytearray()
    for name, width, values in columns:
        if width == 1:
            raw = bytes(values)
        else:
            raw = struct.pack("<%dI" % len(values), *values)
        data = shuffle(raw, width)
        table += struct.pack("<4sIII", name.encode(), width, len(values), len(data))
        blob += data
    return bytes(table), bytes(blob)


def base_op(op):
    """REFP for REFPT, REFM for REFMT, the op itself otherwise"""
    return op & ~STOCK_TEX


def _read_columns(data, at, count):
    columns = {}
    table_end = at + 16 * count
    if table_end > len(data):
        raise LayoutError("column table outside the section")
    offset = table_end
    for index in range(count):
        name, width, elements, size = struct.unpack_from("<4sIII", data, at + 16 * index)
        name = name.decode("ascii", "replace")
        if width not in (1, 2, 4, 8) or size != width * elements or offset + size > len(data) or name in columns:
            raise LayoutError("malformed column %s" % name)
        raw = unshuffle(data[offset:offset + size], width)
        if width == 1:
            columns[name] = list(raw)
        elif width == 4:
            columns[name] = list(struct.unpack("<%dI" % elements, raw))
        else:
            # (a width no column of this version has: a later column, which the decoders ignore)
            columns[name] = raw
        offset += size
    if offset != len(data):
        raise LayoutError("bytes after the columns")
    return columns


def _vertex_columns(vertices, with_attributes):
    """vertices: [(LIT, (qx, qy, qz), (n, b, t), (u_bits, v_bits))], [(REF, index)],
    [(REFP or REFC, index, attributes...)] or [(REFM, (lightmap, material, index), attributes...)]
    (COLL: LIT and REF only, without attributes)"""
    ops, refs, keys, pos, nrm, tex = [], [], [], [], [], []
    last = [0, 0, 0]
    for vertex in vertices:
        op = vertex[0]
        if op not in (GEOM_OPS if with_attributes else (LIT, REF)):
            raise LayoutError("vertex op %r here" % (op,))
        ops.append(op)
        if op == REF:
            refs.append(vertex[1])
            continue
        if base_op(op) == REFM:
            lightmap, material, index = vertex[1]
            keys.append((lightmap & 0xFFFF) << 16 | (material & 0xFFFF))
            refs.append(index)
        elif base_op(op) in (REFP, REFC):
            refs.append(vertex[1])
        else:
            q = vertex[1]
            for axis in range(3):
                if not -GRID_LIMIT < q[axis] < GRID_LIMIT:
                    raise LayoutError("a position beyond 16384 world units")
                pos.append(zigzag(q[axis] - last[axis]))
                last[axis] = q[axis]
        if with_attributes:
            nrm.extend(vertex[2])
            if op not in NO_TEX:
                tex.extend(vertex[3])
    columns = [("VOPS", 1, ops), ("VREF", 4, refs), ("VPOS", 4, pos)]
    if with_attributes:
        columns += [("VKEY", 4, keys), ("VNRM", 4, nrm), ("VTEX", 4, tex)]
    return columns


def _vertices_of(columns, with_attributes, count=None):
    ops = columns.get("VOPS", [])
    ref_list, pos, key_list = columns.get("VREF", []), columns.get("VPOS", []), columns.get("VKEY", [])
    nrm, tex = columns.get("VNRM", []), columns.get("VTEX", [])
    lit = sum(1 for op in ops if op == LIT)
    attributed = sum(1 for op in ops if op != REF)
    textured = sum(1 for op in ops if op not in NO_TEX)
    refm = sum(1 for op in ops if base_op(op) == REFM)
    if any(op not in (GEOM_OPS if with_attributes else (LIT, REF)) for op in ops) or \
            len(pos) != 3 * lit or len(ref_list) != len(ops) - lit or len(key_list) != refm or \
            (with_attributes and (len(nrm) != 3 * attributed or len(tex) != 2 * textured)) or \
            (not with_attributes and (nrm or tex or key_list)):
        raise LayoutError("vertex columns disagree")
    if count is not None and count != len(ops):
        raise LayoutError("vertex count disagrees")
    out, last = [], [0, 0, 0]
    refs, keys = iter(ref_list), iter(key_list)
    k = a = t = 0
    for op in ops:
        if op == REF:
            out.append((REF, next(refs)))
            continue
        if op == LIT:
            q = []
            for axis in range(3):
                last[axis] = to_signed((last[axis] + unzigzag(pos[3 * k + axis])) & 0xFFFFFFFF)
                q.append(last[axis])
            k += 1
            head = (LIT, tuple(q))
        elif base_op(op) == REFM:
            key = next(keys)
            head = (op, (key >> 16, key & 0xFFFF, next(refs)))
        else:
            head = (op, next(refs))
        if with_attributes:
            if op in NO_TEX:
                coordinates = None
            else:
                coordinates = tuple(tex[2 * t:2 * t + 2])
                t += 1
            out.append(head + (tuple(nrm[3 * a:3 * a + 3]), coordinates))
            a += 1
        else:
            out.append(head)
    return out


# ---------- GEOM

class Part:
    def __init__(self, lightmap_index, material_index, stock_surface_count, stock_vertex_count, topology_check,
                 first_surface, surface_count, vertices, triangles, surface_first_triangle,
                 near_triangle_count=None, far_distance=0.0, far_runs=None, spheres=None, static_set=0,
                 material_namespace=NAMESPACE_STOCK, prism_material=0):
        self.lightmap_index = lightmap_index
        self.material_index = material_index
        self.stock_surface_count = stock_surface_count
        self.stock_vertex_count = stock_vertex_count
        self.topology_check = topology_check & 0xFFFFFFFF
        self.first_surface = first_surface
        self.surface_count = surface_count
        self.vertices = vertices
        self.triangles = triangles          # [(a, b, c)]
        self.surface_first_triangle = surface_first_triangle
        self.near_triangle_count = len(triangles) if near_triangle_count is None else near_triangle_count
        self.far_distance = far_distance
        self.far_runs = far_runs            # [(first, count)] per surface, or None
        self.spheres = spheres              # [(x, y, z, r)] per surface (float bits), or None
        self.static_set = static_set
        self.material_namespace = material_namespace
        self.prism_material = prism_material


def part_record(lightmap_index, material_index, stock_surface_count, stock_vertex_count, topology_check,
                first_surface, surface_count, vertex_count, triangle_count, near_triangle_count, far_distance,
                has_far, static_set, material_namespace, prism_material):
    """a part's PART_SIZE bytes (encode.py writes the same through this)"""
    if material_namespace not in (NAMESPACE_STOCK, NAMESPACE_PRISM):
        raise LayoutError("material namespace %r" % (material_namespace,))
    if material_namespace == NAMESPACE_PRISM and (
            (lightmap_index & 0xFFFF, material_index & 0xFFFF) != (NO_INDEX, NO_INDEX) or stock_surface_count or
            stock_vertex_count or topology_check & 0xFFFFFFFF or first_surface or surface_count):
        raise LayoutError("a PRISM material's part is tied to a stock material")
    if material_namespace == NAMESPACE_STOCK and prism_material:
        raise LayoutError("a stock material's part names a PRISM material")
    return struct.pack(PART_FORMAT, lightmap_index & 0xFFFF, material_index & 0xFFFF, stock_surface_count,
                       stock_vertex_count, topology_check & 0xFFFFFFFF, first_surface, surface_count, vertex_count,
                       triangle_count, near_triangle_count, float_bits(far_distance), 1 if has_far else 0,
                       static_set, material_namespace, prism_material)


def geom_header(part_count, column_count):
    return GEOM_MAGIC + struct.pack("<IIII", GEOM_VERSION, part_count, column_count, PART_SIZE)


def encode_geom(parts):
    header = bytearray()
    vertices, corners, runs, far, spheres = [], [], [], [], []
    for part in parts:
        if len(part.surface_first_triangle) != part.surface_count + 1:
            raise LayoutError("a part's surface runs")
        has_far = part.far_runs is not None
        header += part_record(part.lightmap_index, part.material_index, part.stock_surface_count,
                              part.stock_vertex_count, part.topology_check, part.first_surface, part.surface_count,
                              len(part.vertices), len(part.triangles), part.near_triangle_count,
                              part.far_distance, has_far, part.static_set, part.material_namespace,
                              part.prism_material)
        vertices.extend(part.vertices)
        last = 0
        for triangle in part.triangles:
            for index in triangle:
                if not 0 <= index < len(part.vertices):
                    raise LayoutError("a triangle's index outside its part")
                corners.append(zigzag(index - last))
                last = index
        runs.extend(part.surface_first_triangle)
        if has_far:
            if len(part.far_runs) != part.surface_count or len(part.spheres) != part.surface_count:
                raise LayoutError("a far part's runs")
            for first, count in part.far_runs:
                far += [first, count]
            for sphere in part.spheres:
                spheres.extend(sphere)
    columns = _vertex_columns(vertices, True) + [("TIDX", 4, corners), ("SRUN", 4, runs), ("FRUN", 4, far),
                                                 ("FSPH", 4, spheres)]
    table, blob = _columns_bytes(columns)
    return geom_header(len(parts), len(columns)) + bytes(header) + table + blob


def decode_geom(data):
    if len(data) < GEOM_HEADER_SIZE or data[:4] != GEOM_MAGIC:
        raise LayoutError("not GEOM v1")
    version, part_count, column_count, part_size = struct.unpack_from("<IIII", data, 4)
    if version != GEOM_VERSION or part_count > 1 << 20 or column_count > 64 or not PART_SIZE <= part_size <= 4096 \
            or part_size % 4:
        raise LayoutError("GEOM header")
    at = GEOM_HEADER_SIZE + part_size * part_count
    if at > len(data):
        raise LayoutError("GEOM parts outside the section")
    columns = _read_columns(data, at, column_count)
    vertices = _vertices_of(columns, True)
    corners, runs = columns.get("TIDX", []), columns.get("SRUN", [])
    far, spheres = columns.get("FRUN", []), columns.get("FSPH", [])
    parts = []
    v = c = r = f = s = 0
    for index in range(part_count):
        # (the fields this reader knows; a longer record's later ones are skipped)
        fields = struct.unpack_from(PART_FORMAT, data, GEOM_HEADER_SIZE + part_size * index)
        (lightmap, material, stock_surfaces, stock_vertices, topology, first_surface, surface_count, vertex_count,
         triangle_count, near, far_bits, flags, static_set, namespace, prism_material) = fields
        if flags & ~1 or near > triangle_count:
            raise LayoutError("GEOM part flags")
        if namespace not in (NAMESPACE_STOCK, NAMESPACE_PRISM) or \
                (namespace == NAMESPACE_STOCK and prism_material) or \
                (namespace == NAMESPACE_PRISM and ((lightmap, material) != (NO_INDEX, NO_INDEX) or stock_surfaces or
                                                   stock_vertices or topology or first_surface or surface_count)):
            raise LayoutError("GEOM part material namespace")
        part_vertices = vertices[v:v + vertex_count]
        if len(part_vertices) != vertex_count:
            raise LayoutError("GEOM vertices run out")
        v += vertex_count
        triangles, last = [], 0
        for t in range(triangle_count):
            corner = []
            for k in range(3):
                if c >= len(corners):
                    raise LayoutError("GEOM indices run out")
                last = to_signed((last + unzigzag(corners[c])) & 0xFFFFFFFF)
                c += 1
                if not 0 <= last < vertex_count:
                    raise LayoutError("a triangle's index outside its part")
                corner.append(last)
            triangles.append(tuple(corner))
        surface_runs = runs[r:r + surface_count + 1]
        r += surface_count + 1
        if len(surface_runs) != surface_count + 1 or any(b < a for a, b in zip(surface_runs, surface_runs[1:])) \
                or (surface_runs and surface_runs[-1] > triangle_count):
            raise LayoutError("GEOM surface runs")
        far_runs = part_spheres = None
        if flags & 1:
            far_runs = [tuple(far[f + 2 * k:f + 2 * k + 2]) for k in range(surface_count)]
            part_spheres = [tuple(spheres[s + 4 * k:s + 4 * k + 4]) for k in range(surface_count)]
            f += 2 * surface_count
            s += 4 * surface_count
            if any(len(x) != 2 or x[0] + x[1] > triangle_count for x in far_runs) or \
                    any(len(x) != 4 for x in part_spheres):
                raise LayoutError("GEOM far runs")
        if namespace == NAMESPACE_PRISM and any(op in (REF, REFP, REFPT) for op, *_ in part_vertices):
            raise LayoutError("a PRISM material's part refers to its own stock material")
        parts.append(Part(lightmap, material, stock_surfaces, stock_vertices, topology, first_surface,
                          surface_count, part_vertices, triangles, surface_runs, near, bits_float(far_bits),
                          far_runs, part_spheres, static_set, namespace, prism_material))
    if v != len(vertices) or c != len(corners) or r != len(runs) or f != len(far) or s != len(spheres):
        raise LayoutError("GEOM columns longer than its parts")
    return parts


# ---------- LMUV v1

def encode_lmuv(parts):
    """parts: per GEOM part, its lightmap vertices: [(REF, stock index)] or [(LIT, (radiosity, u, v))] (u, v the
    16-bit fields as unsigned)"""
    ops, refs, words = [], [], []
    header = bytearray()
    for part in parts:
        header += struct.pack("<I", len(part))
        for vertex in part:
            if vertex[0] == REF:
                ops.append(REF)
                refs.append(vertex[1])
            elif vertex[0] == LIT:
                radiosity, u, v = vertex[1]
                ops.append(LIT)
                words += [radiosity & 0xFFFFFFFF, (u & 0xFFFF) | (v & 0xFFFF) << 16]
            else:
                raise LayoutError("lightmap vertex op %r" % (vertex[0],))
    table, blob = _columns_bytes([("LOPS", 1, ops), ("LREF", 4, refs), ("LVTX", 4, words)])
    return LMUV_MAGIC + struct.pack("<III", LMUV_VERSION, len(parts), 3) + bytes(header) + table + blob


def decode_lmuv(data):
    if len(data) < 16 or data[:4] != LMUV_MAGIC:
        raise LayoutError("not LMUV v1")
    version, part_count, column_count = struct.unpack_from("<III", data, 4)
    if version != LMUV_VERSION or part_count > 1 << 20 or column_count > 64 or 16 + 4 * part_count > len(data):
        raise LayoutError("LMUV header")
    counts = struct.unpack_from("<%dI" % part_count, data, 16)
    columns = _read_columns(data, 16 + 4 * part_count, column_count)
    ops, refs, words = columns.get("LOPS", []), columns.get("LREF", []), columns.get("LVTX", [])
    if any(op not in (LIT, REF) for op in ops) or len(ops) != sum(counts) or \
            len(refs) != sum(1 for op in ops if op == REF) or len(words) != 2 * sum(1 for op in ops if op == LIT):
        raise LayoutError("LMUV columns disagree")
    out, r, w, at = [], iter(refs), 0, 0
    for count in counts:
        part = []
        for op in ops[at:at + count]:
            if op == REF:
                part.append((REF, next(r)))
            else:
                part.append((LIT, (words[w], words[w + 1] & 0xFFFF, words[w + 1] >> 16)))
                w += 2
        at += count
        out.append(part)
    return out


# ---------- COLL v0

def encode_coll(vertices, planes):
    """vertices: [(LIT, (qx, qy, qz))] or [(REF, stock index)]; planes: [(i, j, k, d) float bits] (ours) or
    [(REF, stock plane index)]"""
    ops, refs, flat = [], [], []
    for plane in planes:
        if len(plane) == 2 and plane[0] == REF:
            ops.append(REF)
            refs.append(plane[1])
        elif len(plane) == 4:
            ops.append(LIT)
            flat.extend(plane)
        else:
            raise LayoutError("a plane is four float bits or (REF, index)")
    columns = _vertex_columns(vertices, False) + [("POPS", 1, ops), ("PREF", 4, refs), ("PLNE", 4, flat)]
    table, blob = _columns_bytes(columns)
    return COLL_MAGIC + struct.pack("<III", COLL_VERSION, 0, len(columns)) + table + blob


def decode_coll(data):
    if len(data) < 16 or data[:4] != COLL_MAGIC:
        raise LayoutError("not COLL v0")
    version, zero, column_count = struct.unpack_from("<III", data, 4)
    if version != COLL_VERSION or zero or column_count > 64:
        raise LayoutError("COLL header")
    columns = _read_columns(data, 16, column_count)
    ops, refs, flat = columns.get("POPS", []), columns.get("PREF", []), columns.get("PLNE", [])
    if any(op not in (LIT, REF) for op in ops) or len(refs) != sum(1 for op in ops if op == REF) or \
            len(flat) != 4 * sum(1 for op in ops if op == LIT):
        raise LayoutError("COLL planes")
    planes, r, k = [], iter(refs), 0
    for op in ops:
        if op == REF:
            planes.append((REF, next(r)))
        else:
            planes.append(tuple(flat[k:k + 4]))
            k += 4
    return _vertices_of(columns, False), planes


# ---------- canonical tuples

def plane_key(i, j, k, d):
    """a plane quantized for the scanner: its normal at 1/65536, d on the grid"""
    return (round(i * 65536), round(j * 65536), round(k * 65536), round(d * GRID))


def section_tuples(kind, data):
    """[(tuple kind, value, is_ref, where)] of a decoded section: what the lint and the scanner compare against the
    retail sets. Positions and planes are hard (a LIT one equal to retail fails); normals and coordinate pairs are
    reported only (quantization makes them coincide by nature: a flat floor's normal is every flat floor's)."""
    out = []
    if kind == "GEOM":
        for p, part in enumerate(decode_geom(data)):
            for v, vertex in enumerate(part.vertices):
                where = (p, v)
                if vertex[0] in STOCK_POSITION:
                    out.append(("position", (vertex[0], vertex[1]), True, where))
                    if vertex[0] == REF:
                        continue
                else:
                    out.append(("position", vertex[1], False, where))
                out.append(("normal", vertex[2][0], False, where))
                if vertex[3] is not None:
                    out.append(("texcoord", vertex[3], False, where))
    elif kind == "COLL":
        vertices, planes = decode_coll(data)
        for v, vertex in enumerate(vertices):
            if vertex[0] == REF:
                out.append(("collision_vertex", vertex[1], True, (0, v)))
            else:
                out.append(("collision_vertex", vertex[1], False, (0, v)))
        for p, plane in enumerate(planes):
            if len(plane) == 2:
                out.append(("plane", plane, True, (1, p)))
            else:
                out.append(("plane", plane_key(*[bits_float(b) for b in plane]), False, (1, p)))
    return out


HARD_KINDS = ("position", "collision_vertex", "plane")


def runtime_bytes(kind, data):
    """the section's values as the engine lays them out (LIT vertices as the game's 32-byte compressed vertex:
    position floats, packed vectors, base map coordinates; planes as four floats): what the 48-byte window scan
    reads besides the section's own bytes"""
    out = bytearray()
    if kind == "GEOM":
        for part in decode_geom(data):
            for vertex in part.vertices:
                if vertex[0] == LIT:
                    out += struct.pack("<3f", *[q / GRID for q in vertex[1]])
                if vertex[0] != REF:
                    out += struct.pack("<3I", *vertex[2])
                    if vertex[3] is not None:
                        out += struct.pack("<2I", *vertex[3])
    elif kind == "COLL":
        vertices, planes = decode_coll(data)
        for vertex in vertices:
            if vertex[0] == LIT:
                out += struct.pack("<3f", *[q / GRID for q in vertex[1]])
        for plane in planes:
            if len(plane) == 4:
                out += struct.pack("<4I", *plane)
    return bytes(out)
