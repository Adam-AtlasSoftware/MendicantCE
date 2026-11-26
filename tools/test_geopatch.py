"""The enhanced world's sidecar (PLAN_FINAL.md sections 2.3 and 2.4), asset-free on synthetic maps
(tools/geopatch/synthmap.py): the game's reader (port/linux/game/prism_prsx.c) and the farm's writer agree and
refuse the same malformed files, the exact REF decode (prism_ref_decode.c) gives the game's own float decode bit
for bit, GEOM and COLL round-trip, the encoder resolves stock vertices to REF ops, the lint refuses literals where a
REF is required, the tuple scanner's negative controls all fail while a clean remodel passes, and the signed
manifest follows its schema and rules."""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from geopatch import geom, identity, lint_patch, manifest, native, prsx, retail, synthmap  # noqa: E402
from geopatch.geom import LIT, REF, REFC, REFM, REFMT, REFP, REFPT  # noqa: E402

CWID = bytes.fromhex("aa" * 32)


def _skip_or_fail(reason):
    """skip on a developer's machine without the tool; fail in CI, where a green step must have run every test"""
    if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS"):
        pytest.fail(reason)
    pytest.skip(reason)


def _numpy():
    try:
        import numpy
    except ImportError:
        _skip_or_fail("numpy is not installed")
    return numpy


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    try:
        return native.Native(tmp_path_factory.mktemp("native"))
    except native.NativeError as error:
        _skip_or_fail("no C compiler builds the game's sidecar code: %s" % error)


@pytest.fixture(scope="module")
def synth(tmp_path_factory):
    folder = tmp_path_factory.mktemp("maps")
    inflated = synthmap.build(geometries=[synthmap.Geometry(1), synthmap.Geometry(2, offset=(0.0, 300.0, 0.0))])
    path = folder / "synth.map"
    path.write_bytes(synthmap.compress(inflated))
    return path


# ---------- exact REF decode

def test_ref_decode_is_the_games_float_decode(tmp_path):
    """every packed field value and 16-bit coordinate, and 200,000 random whole vertices, against the game's own
    expressions (source/rasterizer/rasterizer_geometry.c) in the compiler's IEEE single arithmetic"""
    clang = shutil.which("clang") or shutil.which("cc")
    if not clang:
        _skip_or_fail("no C compiler")
    exe = tmp_path / ("check.exe" if sys.platform == "win32" else "check")
    flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"] \
        if sys.platform == "win32" else []
    result = subprocess.run([clang, *flags, "-std=gnu89", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                             "-I", str(ROOT / "port/linux/game"), str(ROOT / "tools/geopatch/ref_decode_check.c"),
                             str(ROOT / "port/linux/game/prism_ref_decode.c"), "-o", str(exe)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0 and run.stdout.strip() == "ok", run.stdout


def test_ref_decode_through_the_tool(tool):
    assert tool.reciprocal(2047) == geom.float_bits(1.0 / 2047.0)
    # the grid: exact, and refused at 16384 world units
    values = [0, 1, -1, 1024, -1024, 12345678, -(1 << 24) + 1]
    assert tool.grid(values) == [geom.float_bits(v / 1024.0) for v in values]
    with pytest.raises(native.NativeError):
        tool.grid([1 << 24])
    # a vertex through the tool equals the vertex's fields decoded one by one
    vertex = struct.pack("<3f3I2f", 1.5, -2.25, 3.0, synthmap.pack_vector(0, 0, 1), 0x12345678, 0xFFFFFFFF, 0.5, 0.25)
    out = tool.environment_vertices(vertex)
    assert out[:12] == vertex[:12] and out[48:] == vertex[24:]
    assert struct.unpack("<9I", out[12:48]) == tuple(
        bit for packed in struct.unpack("<3I", vertex[12:24]) for bit in tool.vectors([packed])[0])


# ---------- the container: writer and the game's reader agree

def write_and_open(tool, tmp_path, blob, name="s.prsx", appended=False):
    path = tmp_path / name
    path.write_bytes(blob)
    return tool.open(path, appended)


def test_container_round_trip(tool, tmp_path):
    big = bytes(range(256)) * 400
    sections = [prsx.Section("GEOM", 0, big), prsx.Section("GEOM", 1, b"", codec=prsx.STORED),
                prsx.Section("SCEN", prsx.BSP_ALL, big[:999], codec=prsx.STORED),
                prsx.Section("PATH", 0, b"p" * 77, collv=3), prsx.Section("FUTR", 0, b"later", flags=0)]
    blob = prsx.write(CWID, sections)
    # (the directory in its canonical order)
    sections.sort(key=lambda s: prsx.directory_key(s.kind, s.bsp, s.collv))
    verdict = write_and_open(tool, tmp_path, blob)
    assert verdict["result"] == "ok" and verdict["format"] == prsx.FORMAT and verdict["classic"] == CWID.hex()
    assert [e["read"] for e in verdict["entries"]] == ["ok"] * 5
    assert [(e["kind"], e["bsp"], e["collv"], e["usize"]) for e in verdict["entries"]] == \
        [(s.kind, s.bsp, s.collv, len(s.data)) for s in sections]
    sidecar = prsx.Sidecar(blob)
    assert [sidecar.section(i) for i in range(5)] == [s.data for s in sections]


def test_appended_container(tool, tmp_path):
    """the same reader finds our data appended to a whole map file (decision A1's other way)"""
    container = prsx.write(CWID, [prsx.Section("GEOM", 0, b"appended geometry")])
    whole = synthmap.build()
    verdict = write_and_open(tool, tmp_path, prsx.append_trailer(whole, container), "whole.map", appended=True)
    assert verdict["result"] == "ok" and verdict["entries"][0]["read"] == "ok"
    bad = bytearray(prsx.append_trailer(whole, container))
    struct.pack_into("<Q", bad, len(bad) - 16, len(whole) + 1)
    assert write_and_open(tool, tmp_path, bytes(bad), "bad.map", appended=True)["result"] == "no appended container"
    assert write_and_open(tool, tmp_path, whole, "plain.map", appended=True)["result"] == "no appended container"


def _patch(blob, offset, fmt, *values):
    out = bytearray(blob)
    struct.pack_into(fmt, out, offset, *values)
    return bytes(out)


MALFORMED = {
    "signature": (lambda b: b"PRSY" + b[4:], "not a PRSX file"),
    "future format": (lambda b: _patch(b, 4, "<I", prsx.FORMAT + 1), "a format this build does not read"),
    "header flags": (lambda b: _patch(b, 44, "<I", 1), "a malformed header"),
    "directory past the end": (lambda b: _patch(b, 40, "<I", 1000), "a malformed directory"),
    "section past the end": (lambda b: _patch(b, 48 + 20, "<Q", 1 << 40), "a malformed directory"),
    "section over the directory": (lambda b: _patch(b, 48 + 12, "<Q", 10), "a malformed directory"),
    "sections overlap": (lambda b: _patch(b, 48 + 68 + 12, "<Q", struct.unpack_from("<Q", b, 48 + 12)[0]),
                         "a malformed directory"),
    "unknown flag": (lambda b: _patch(b, 48 + 6, "<H", struct.unpack_from("<H", b, 54)[0] | 0x0100),
                     "a malformed directory"),
    "unknown codec": (lambda b: _patch(b, 48 + 6, "<H", (struct.unpack_from("<H", b, 54)[0] & 0x0FFF) | 0x7000),
                      "a malformed directory"),
    "unknown required type": (lambda b: _patch(b, 48, "<I", prsx.fourcc("ZZZZ")),
                              "a required section this build does not know"),
    "stored size disagrees": (lambda b: _patch(b, 48 + 68 + 28, "<Q", 3), "a malformed directory"),
    "an entry twice": (lambda b: _patch(b, 48 + 68, "<IH", prsx.fourcc("GEOM"), 0), "a malformed directory"),
}


@pytest.mark.parametrize("case", sorted(MALFORMED))
def test_malformed_refused_by_both(tool, tmp_path, case):
    good = prsx.write(CWID, [prsx.Section("GEOM", 0, b"geometry"), prsx.Section("GEOM", 1, b"geo2",
                                                                                 codec=prsx.STORED)])
    mutate, expected = MALFORMED[case]
    blob = mutate(good)
    assert write_and_open(tool, tmp_path, blob)["result"] == expected
    with pytest.raises(prsx.PrsxError):
        prsx.Sidecar(blob)


def test_section_corruption_refused(tool, tmp_path):
    blob = bytearray(prsx.write(CWID, [prsx.Section("GEOM", 0, b"x" * 5000), prsx.Section("RHGT", 0, b"y" * 64,
                                                                                         codec=prsx.STORED)]))
    blob[-1] ^= 1  # a byte of the stored RHGT
    verdict = write_and_open(tool, tmp_path, bytes(blob))
    assert verdict["result"] == "ok" and [e["read"] for e in verdict["entries"]] == \
        ["ok", "a section whose SHA-256 differs"]
    # a known section in zstd (not vendored yet) is refused at open, so the map falls to the classic world before
    # its load starts; an unknown optional one is skipped unread
    zstd = prsx.write(CWID, [prsx.Section("GEOM", 0, b"z" * 100, codec=prsx.STORED)])
    zstd = _patch(zstd, 48 + 6, "<H", (struct.unpack_from("<H", zstd, 54)[0] & 0x0FFF) | prsx.ZSTD << 12)
    assert write_and_open(tool, tmp_path, zstd, "z.prsx")["result"] == "a section in a codec this build does not read"
    with pytest.raises(prsx.PrsxError):
        prsx.Sidecar(zstd)
    later = prsx.write(CWID, [prsx.Section("FUTR", 0, b"later", flags=0, codec=prsx.STORED)])
    later = _patch(later, 48 + 6, "<H", prsx.ZSTD << 12)
    assert write_and_open(tool, tmp_path, later, "f.prsx")["result"] == "ok"
    prsx.Sidecar(later)
    trailing = bytearray(prsx.write(CWID, [prsx.Section("GEOM", 0, b"q" * 3000)]))
    struct.pack_into("<Q", trailing, 48 + 28, 2999)  # usize one short of what the stream inflates to
    assert write_and_open(tool, tmp_path, bytes(trailing), "t.prsx")["entries"][0]["read"] == \
        "a section of the wrong size"


# ---------- GEOM and COLL

def _sample_part(material_index=0):
    vertices = [(REF, 0), (REF, 1), (LIT, (100, -200, 3000), (1, 2, 3), (4, 5)),
                (REFP, 2, (6, 7, 8), (9, 10)), (LIT, (-50, 0, 2990), (11, 12, 13), (14, 15)),
                (REFM, (1, 3, 77), (16, 17, 18), (19, 20)), (REFC, 5, (21, 22, 23), (24, 25)),
                (REFPT, 3, (26, 27, 28), None), (REFMT, (2, 4, 9), (29, 30, 31), None)]
    return geom.Part(0, material_index, 4, 3, 0xDEADBEEF, 0, 2, vertices,
                     [(0, 1, 2), (1, 3, 2), (2, 3, 4), (4, 5, 6)],
                     [0, 2, 4], near_triangle_count=2, far_distance=55.5, far_runs=[(2, 1), (0, 0)],
                     spheres=[(1, 2, 3, 4), (5, 6, 7, 8)])


def test_geom_round_trip_and_numpy_twin():
    np = _numpy()
    from geopatch import encode
    parts = [_sample_part(0), _sample_part(1)]
    data = geom.encode_geom(parts)
    back = geom.decode_geom(data)
    assert [p.vertices for p in back] == [p.vertices for p in parts]
    assert [p.triangles for p in back] == [p.triangles for p in parts]
    assert back[0].far_runs == [(2, 1), (0, 0)] and back[0].far_distance == 55.5
    arrays = []
    for p in parts:
        e = encode.EncodedPart()
        for name in ("lightmap_index", "material_index", "stock_surface_count", "stock_vertex_count",
                     "topology_check", "first_surface", "surface_count", "near_triangle_count", "far_distance"):
            setattr(e, name, getattr(p, name))
        e.ops = np.array([v[0] for v in p.vertices], np.uint8)
        e.refs = np.array([v[1][2] if v[0] in (REFM, REFMT) else v[1] for v in p.vertices if v[0] != LIT],
                          np.uint32)
        e.keys = np.array([v[1][0] << 16 | v[1][1] for v in p.vertices if v[0] in (REFM, REFMT)], np.uint32)
        e.q = np.array([v[1] for v in p.vertices if v[0] == LIT], np.int64).reshape(-1, 3)
        e.nrm = np.array([v[2] for v in p.vertices if v[0] != REF], np.uint32)
        e.tex = np.array([v[3] for v in p.vertices if v[0] != REF and v[3] is not None], np.uint32).reshape(-1, 2)
        e.triangles = np.array(p.triangles, np.int64)
        e.sft = np.array(p.surface_first_triangle, np.uint32)
        e.far_pairs = np.array(p.far_runs, np.uint32)
        e.spheres = np.array(p.spheres, np.uint32)
        arrays.append(e)
    assert encode.encode_geom_arrays(arrays) == data


@pytest.mark.parametrize("damage", ["index", "runs", "trailing", "column", "op"])
def test_geom_layout_refusals(damage):
    data = bytearray(geom.encode_geom([_sample_part()]))
    if damage == "index":
        bad = _sample_part()
        bad.triangles = [(0, 1, 9)] + bad.triangles[1:]
        with pytest.raises(geom.LayoutError):
            geom.encode_geom([bad])
        return
    if damage == "runs":
        struct.pack_into("<I", data, geom.GEOM_HEADER_SIZE + 20, 5)  # surface count past its runs
    elif damage == "trailing":
        data += b"\0"
    elif damage == "column":
        at = geom.GEOM_HEADER_SIZE + geom.PART_SIZE + 16 * 2 + 8  # VPOS's element count
        struct.pack_into("<I", data, at, struct.unpack_from("<I", data, at)[0] + 1)
    elif damage == "op":
        at = geom.GEOM_HEADER_SIZE + geom.PART_SIZE + 16 * 10  # the first column's bytes: VOPS
        data[at] = 7
    with pytest.raises(geom.LayoutError):
        geom.decode_geom(bytes(data))


def test_geom_room_for_new_content():
    """GEOM v1 takes new content without a version: a part with a PRISM material tied to no stock material or
    surface, a part with no stock surface, a longer part record and a column this reader does not know"""
    prism_part = geom.Part(geom.NO_INDEX, geom.NO_INDEX, 0, 0, 0, 0, 0,
                           [(LIT, (0, 0, 0), (1, 2, 3), (4, 5)), (LIT, (1024, 0, 0), (1, 2, 3), (6, 7)),
                            (REFM, (0, 0, 2), (1, 2, 3), (8, 9)), (REFC, 1, (1, 2, 3), (10, 11))],
                           [(0, 1, 2), (1, 3, 2)], [0], static_set=2, material_namespace=geom.NAMESPACE_PRISM,
                           prism_material=0x1234)
    addition = geom.Part(0, 0, 4, 3, 0xDEADBEEF, 0, 0, [(LIT, (5, 5, 5), (0, 0, 0), (0, 0)), (REF, 0), (REF, 1)],
                         [(0, 1, 2)], [0])
    data = geom.encode_geom([_sample_part(), prism_part, addition])
    back = geom.decode_geom(data)
    assert (back[1].material_namespace, back[1].prism_material, back[1].static_set) == (geom.NAMESPACE_PRISM,
                                                                                        0x1234, 2)
    assert back[1].vertices == prism_part.vertices and back[2].surface_count == 0
    # a later version's longer part records and an extra column: this reader skips both
    count, columns = struct.unpack_from("<II", data, 8)
    extra = 8
    head, parts_end = geom.GEOM_HEADER_SIZE, geom.GEOM_HEADER_SIZE + geom.PART_SIZE * count
    records = b"".join(data[head + geom.PART_SIZE * i:head + geom.PART_SIZE * (i + 1)] + b"\xee" * extra
                       for i in range(count))
    table_end = parts_end + 16 * columns
    longer = (data[:12] + struct.pack("<II", columns + 1, geom.PART_SIZE + extra) + records +
              data[parts_end:table_end] + struct.pack("<4sIII", b"ZNEW", 4, 2, 8) + data[table_end:] + b"\1" * 8)
    later = geom.decode_geom(longer)
    assert [p.vertices for p in later] == [p.vertices for p in back]
    # a PRISM part names no stock material, surface or own-material vertex
    for bad in (dict(lightmap_index=0), dict(surface_count=1), dict(topology_check=1)):
        part = geom.Part(**{**dict(lightmap_index=geom.NO_INDEX, material_index=geom.NO_INDEX,
                                   stock_surface_count=0, stock_vertex_count=0, topology_check=0, first_surface=0,
                                   surface_count=0, vertices=prism_part.vertices, triangles=[(0, 1, 2)],
                                   surface_first_triangle=[0] * (1 + bad.get("surface_count", 0)),
                                   material_namespace=geom.NAMESPACE_PRISM, prism_material=1), **bad})
        with pytest.raises(geom.LayoutError):
            geom.encode_geom([part])
    own = geom.Part(geom.NO_INDEX, geom.NO_INDEX, 0, 0, 0, 0, 0, [(REF, 0), (REF, 1), (REF, 2)], [(0, 1, 2)], [0],
                    material_namespace=geom.NAMESPACE_PRISM, prism_material=1)
    with pytest.raises(geom.LayoutError):
        geom.decode_geom(geom.encode_geom([own]))


def test_coll_round_trip():
    planes = [(geom.float_bits(0.0), geom.float_bits(0.0), geom.float_bits(1.0), geom.float_bits(-3.5)), (REF, 7)]
    vertices = [(REF, 4), (LIT, (1, 2, 3)), (LIT, (-7, 8, 9))]
    assert geom.decode_coll(geom.encode_coll(vertices, planes)) == (vertices, planes)
    tuples = geom.section_tuples("COLL", geom.encode_coll(vertices, planes))
    assert [t[2] for t in tuples if t[0] == "plane"] == [False, True]
    with pytest.raises(geom.LayoutError):
        geom.encode_coll([(REFP, 1, (0, 0, 0), (0, 0))], planes)


# ---------- encoder, lint and scanner on a synthetic map

def _write_pwg(path, parts_np):
    """a .pwg v2 as tools/prism_packs/art/world_geo/wg_common.py writes it (the encoder's input)"""
    import numpy as np
    out = bytearray(b"PWG1" + struct.pack("<III", 2, len(parts_np), 1) + b"levels\\synth".ljust(48, b"\0"))

    def pad(b):
        return b + b"\0" * (-len(b) % 4)
    for p in parts_np:
        out += struct.pack("<hhIIIII", p.lightmap_index, p.material_index, p.stock_surface_count,
                           p.stock_vertex_count, len(p.vertices), len(p.triangles), p.topology_check)
        out += struct.pack("<dII", 0.0, p.first_surface, p.surface_count)
        out += struct.pack("<If", p.near_triangle_count, 40.0) + b"\0" * 16
        out += pad(p.sft.astype("<u4").tobytes())
        out += pad(p.vertices.astype("<f4").tobytes())
        out += pad(p.lightmap.astype("<f4").tobytes())
        out += pad(p.triangles.astype("<u2").tobytes())
        pairs = np.stack([p.sft[:-1], np.diff(p.sft)], axis=1).astype("<u4")
        out += pairs.tobytes() + np.zeros((p.surface_count, 4), "<f4").tobytes()
    path.write_bytes(bytes(out))


def test_encode_lint_scan(tool, synth, tmp_path, monkeypatch):
    _numpy()
    from geopatch import encode, scan_original
    monkeypatch.setenv("GEOPATCH_NATIVE_DIR", str(tool.exe.parent))
    _, _, bsps = retail.read_map(synth)
    for bsp in bsps:
        parts = [scan_original.remodel_part(m, tool) for m in bsp.materials.values()]
        _write_pwg(tmp_path / ("bsp%d.pwg" % bsp.index), parts)
    out = tmp_path / "synth.prsx"
    report = tmp_path / "report.json"
    assert encode.main(["--map", str(synth), "--classic-world-id", CWID.hex(),
                        "--pwg", "0=%s" % (tmp_path / "bsp0.pwg"), "--pwg", "1=%s" % (tmp_path / "bsp1.pwg"),
                        "-o", str(out), "--report", str(report)]) == 0
    stats = json.loads(report.read_text())
    for bsp in bsps:
        s = stats["bsps"][str(bsp.index)]
        stock = sum(m.vertex_count for m in bsp.materials.values())
        assert s["ref"] == stock and s["refp"] == 0 and s["lit"] == s["vertices"] - stock
    # the game's reader takes it; the lint and the scanner find nothing
    verdict = tool.open(out)
    assert verdict["result"] == "ok" and all(e["read"] == "ok" for e in verdict["entries"])
    assert lint_patch.main([str(out), "--map", str(synth), "--native", "--native-dir", str(tool.exe.parent)]) == 0
    assert scan_original.main([str(out), "--map", str(synth), "--native-dir", str(tool.exe.parent)]) == 0
    # without the map every vertex is LIT, and the lint refuses the stock ones
    raw = tmp_path / "raw.prsx"
    encode.main(["--classic-world-id", CWID.hex(), "--pwg", "0=%s" % (tmp_path / "bsp0.pwg"), "-o", str(raw)])
    assert lint_patch.main([str(raw), "--map", str(synth)]) == 1


def test_encoder_takes_stock_coordinates_by_reference(tool, synth):
    """a remodel that kept stock positions and coordinates but changed the normals: REFPT (and REFMT across
    materials), so no stock coordinate is written out, and the lint passes it; written as REFP it fails"""
    np = _numpy()
    from geopatch import encode, scan_original
    _, _, bsps = retail.read_map(synth)
    by_index = {b.index: b for b in bsps}
    material = by_index[0].materials[(0, 0)]
    other = by_index[0].materials[(0, 1)]
    p = scan_original.remodel_part(material, tool, extra=[other.position(k) for k in range(3)])
    p.vertices = p.vertices.copy()
    p.vertices[:, 3:6] = np.float32([0.0, 0.6, 0.8])  # every normal ours
    other_decoded = np.frombuffer(tool.environment_vertices(other.vertices), "<f4").reshape(-1, 14)
    p.vertices[-3:, 12:14] = other_decoded[0:3, 12:14]  # the other material's coordinates kept too
    e, counts = encode.encode_pwg_part(p, material, tool, by_index[0])
    assert counts["refpt"] == material.vertex_count and counts["refp"] == 0 and counts["ref"] == 0
    assert counts["refmt"] == 3 and counts["refm"] == 0
    data = prsx.write(CWID, [prsx.Section("GEOM", 0, encode.encode_geom_arrays([e]))])
    assert lint_patch.lint(data, by_index) == []
    assert not any(kind == "texcoord" and at[1] < material.vertex_count
                   for kind, _, _, at in geom.section_tuples("GEOM", prsx.Sidecar(data).section(0)))
    # the same vertices with their coordinates written out
    e.ops = np.where(e.ops == REFPT, REFP, np.where(e.ops == REFMT, REFM, e.ops)).astype(np.uint8)
    e.tex = p.vertices[e.ops != REF][:, 12:14].copy().view("<u4")
    data = prsx.write(CWID, [prsx.Section("GEOM", 0, encode.encode_geom_arrays([e]))])
    assert any("base map coordinates" in f for f in lint_patch.lint(data, by_index))


def test_encoder_snaps_kept_stock_positions(tool, synth):
    """a stock vertex the art pipeline left a grid step or two off (float noise) is that stock position, a REF:
    never LIT a hair from retail; one past SNAP_STEPS stays ours"""
    np = _numpy()
    from geopatch import encode, scan_original
    _, _, bsps = retail.read_map(synth)
    material = bsps[0].materials[(0, 0)]
    for steps, expected in ((1, "snapped"), (encode.SNAP_STEPS, "snapped"), (encode.SNAP_STEPS + 1, "lit")):
        p = scan_original.remodel_part(material, tool)
        p.vertices = p.vertices.copy()
        p.vertices[:material.vertex_count, 0] += np.float32(steps / 1024.0)
        e, counts = encode.encode_pwg_part(p, material, tool, bsps[0])
        stock = material.vertex_count
        if expected == "snapped":
            assert counts["snapped"] == stock and counts["refp"] + counts["refpt"] == stock, (steps, counts)
            assert counts["lit"] == counts["vertices"] - stock
        else:
            assert counts["snapped"] == 0 and counts["lit"] == counts["vertices"], (steps, counts)
    stock_points = np.array([[0, 0, 0], [10, 10, 10], [3, 0, 0]], np.int64)
    found = encode._nearest_within(np.array([[2, 0, 0], [6, 6, 6], [10, 14, 10], [-4, 0, 0]], np.int64),
                                   stock_points, 4)
    assert found.tolist() == [2, 1, 1, 0]


# ---------- the game's decode of GEOM and LMUV (port/linux/game/prism_geom.c, M2)

def test_lmuv_round_trip_and_numpy_twin():
    np = _numpy()
    from geopatch import encode
    parts = [[(REF, 3), (LIT, (0x12345678, 1, 65535)), (REF, 0)], [], [(LIT, (7, 32768, 2))]]
    data = geom.encode_lmuv(parts)
    assert geom.decode_lmuv(data) == parts
    twins = []
    for part in parts:
        e = encode.EncodedPart()
        e.ops = np.zeros(len(part), np.uint8)
        e.lops = np.array([v[0] for v in part], np.uint8)
        e.lrefs = np.array([v[1] for v in part if v[0] == REF], np.uint32)
        e.lwords = np.array([w for v in part if v[0] == LIT for w in (v[1][0], v[1][1] | v[1][2] << 16)], np.uint32)
        twins.append(e)
    assert encode.encode_lmuv_arrays(twins) == data
    for damage in (lambda d: d[:-1], lambda d: b"LMU0" + d[4:], lambda d: d[:8] + struct.pack("<I", 9) + d[12:]):
        with pytest.raises(geom.LayoutError):
            geom.decode_lmuv(damage(data))


def test_encoder_packs_as_the_game_compresses():
    """the packed vectors and lightmap coordinates are the game's compressor's (floor of x times 1023.5 / 511.5 /
    32767.5 in single precision), and the decode of a packed value packs to it again"""
    np = _numpy()
    from geopatch import encode
    rng = np.random.default_rng(5)
    v = rng.uniform(-1.0, 1.0, (4000, 3)).astype(np.float32)
    v[:6] = [[1, 1, 1], [-1, -1, -1], [0, 0, 0], [0.5, -0.5, 0.25], [1.5, -2.0, 0.0], [-0.0, 0.0, 1.0]]
    packed = encode.pack_vectors(v)
    for k in range(len(v)):
        x = np.clip(v[k], -1, 1)
        fields = [int(np.floor(np.float32(x[0]) * np.float32(1023.5))) & 0x7FF,
                  int(np.floor(np.float32(x[1]) * np.float32(1023.5))) & 0x7FF,
                  int(np.floor(np.float32(x[2]) * np.float32(511.5))) & 0x3FF]
        assert int(packed[k]) == fields[0] | fields[1] << 11 | fields[2] << 22
    # decode then pack: the same bits (the game packs the decoded image's vectors again as it loads it)
    def decode(p):
        def field(value, bits):
            s = value & ((1 << bits) - 1)
            return s - (1 << bits) if s >> (bits - 1) else s
        x = (2 * field(int(p), 11) + 1) * np.float32(1.0 / 2047.0)
        y = (2 * field(int(p) >> 11, 11) + 1) * np.float32(1.0 / 2047.0)
        z = (2 * field(int(p) >> 22, 10) + 1) * np.float32(1.0 / 1023.0)
        return np.float32([x, y, z])
    again = encode.pack_vectors(np.stack([decode(p) for p in packed]))
    assert np.array_equal(again, packed)
    c = np.float32([-1.0, 1.0, 0.0, 0.25, -0.75, 2.0])
    assert encode.pack_coordinates(c).tolist() == [int(np.floor(np.float32(min(max(x, -1), 1)) * np.float32(32767.5)))
                                                   & 0xFFFF for x in c]


def _synth_remodel(tool, synth, tmp_path):
    """the synthetic map's remodel (scan_original.remodel_part, with lightmap vertices: the stock ones decoded at
    the stock corners, ours at the centroids) as .pwg files and its encoded sidecar"""
    np = _numpy()
    from geopatch import encode, scan_original
    _, _, bsps = retail.read_map(synth)
    pwgs = {}
    for bsp in bsps:
        parts = []
        for m in bsp.materials.values():
            part = scan_original.remodel_part(m, tool)
            stock_lm = np.frombuffer(tool.lightmap_vertices(m.lightmap_vertices), "<f4").reshape(-1, 5)
            part.lightmap = np.zeros((len(part.vertices), 5), np.float32)
            part.lightmap[:m.vertex_count] = stock_lm
            part.lightmap[m.vertex_count:] = [0.0, 0.6, 0.8, 0.31, -0.42]
            parts.append(part)
        pwgs[bsp.index] = tmp_path / ("bsp%d.pwg" % bsp.index)
        _write_pwg(pwgs[bsp.index], parts)
    out = tmp_path / "synth.prsx"
    assert encode.main(["--map", str(synth), "--classic-world-id", CWID.hex(), "--native-dir", str(tool.exe.parent),
                        *[a for b in sorted(pwgs) for a in ("--pwg", "%d=%s" % (b, pwgs[b]))], "-o", str(out)]) == 0
    return bsps, pwgs, out


def test_game_decodes_the_sidecar_to_the_remodel(tool, synth, tmp_path, monkeypatch):
    """prism_geom.c's decode of each BSP's GEOM and LMUV against the player's map is the remodel the .pwg held, as
    the game draws it: positions on the grid (REF vertices the stock ones bit for bit), the packed vectors, base map
    coordinates and compressed lightmap vertices exactly, the triangles, runs and far levels equal"""
    np = _numpy()
    from geopatch import encode
    monkeypatch.setenv("GEOPATCH_NATIVE_DIR", str(tool.exe.parent))
    bsps, pwgs, out = _synth_remodel(tool, synth, tmp_path)
    for bsp in bsps:
        stock = tmp_path / ("stock%d.bin" % bsp.index)
        native.write_stock(stock, bsp)
        image, report = tool.geom(stock, out, bsp.index)
        assert image is not None, report
        assert "report ok" in report and " far 1 " in report
        (tmp_path / "image.pwg").write_bytes(image)
        got, want = encode.read_pwg(tmp_path / "image.pwg"), encode.read_pwg(pwgs[bsp.index])
        assert len(got) == len(want) == len(bsp.materials)
        lightmap_refs = 0
        for g, w, m in zip(got, want, bsp.materials.values()):
            assert (g.lightmap_index, g.material_index, g.stock_surface_count, g.stock_vertex_count,
                    g.topology_check) == (w.lightmap_index, w.material_index, w.stock_surface_count,
                                          w.stock_vertex_count, w.topology_check)
            assert np.array_equal(g.triangles, w.triangles) and np.array_equal(g.sft, w.sft)
            assert np.array_equal(g.far_pairs, w.far_pairs) and g.near_triangle_count == w.near_triangle_count
            # the stock corners: the retail vertices decoded, bit for bit
            stock_decoded = np.frombuffer(tool.environment_vertices(m.vertices), "<u4").reshape(-1, 14)
            assert np.array_equal(g.vertices[:m.vertex_count].view("<u4"), stock_decoded)
            assert np.abs(g.vertices[:, 0:3] - w.vertices[:, 0:3]).max() <= 0.5 / 1024 + 1e-6
            for k in (3, 6, 9):
                assert np.array_equal(encode.pack_vectors(g.vertices[:, k:k + 3]),
                                      encode.pack_vectors(w.vertices[:, k:k + 3]))
            assert np.array_equal(g.vertices[:, 12:14].view("<u4"), w.vertices[:, 12:14].view("<u4"))
            for a, b in zip(encode.compress_lightmap(g.lightmap), encode.compress_lightmap(w.lightmap)):
                assert np.array_equal(a, b)
            lightmap_refs += m.vertex_count
        # (and its world_id is what the identity says: the GEOM and LMUV sections both count)
        ids = tool.open(out)["world"][bsp.index]
        sidecar = prsx.Sidecar(out.read_bytes())
        assert ids[1] == identity.world_id_enhanced(sidecar, bsp.index)
        assert {e.kind for e in sidecar.entries if identity.counts(e, bsp.index)} == {"GEOM", "LMUV"}


def test_game_refuses_a_broken_or_foreign_sidecar(tool, synth, tmp_path, monkeypatch):
    """a map without the stock materials the sidecar refers to, a GEOM without its LMUV, a cut GEOM, an LMUV of
    another part count, a REF past the stock vertices: each refused with its reason, nothing drawn"""
    monkeypatch.setenv("GEOPATCH_NATIVE_DIR", str(tool.exe.parent))
    bsps, pwgs, out = _synth_remodel(tool, synth, tmp_path)
    sidecar = prsx.Sidecar(out.read_bytes())
    sections = {(e.kind, e.bsp): sidecar.section(i) for i, e in enumerate(sidecar.entries)}
    stock = tmp_path / "stock.bin"
    native.write_stock(stock, bsps[0])

    class Empty:
        materials = {}
        collision_vertices = []
    empty = tmp_path / "empty.bin"
    native.write_stock(empty, Empty())

    def verdict(geom_bytes, lmuv_bytes, stock_path=stock):
        parts = [prsx.Section("GEOM", 0, geom_bytes)] + ([prsx.Section("LMUV", 0, lmuv_bytes)] if lmuv_bytes else [])
        path = tmp_path / "case.prsx"
        path.write_bytes(prsx.write(CWID, parts))
        return tool.geom(stock_path, path, 0)
    g, lm = sections[("GEOM", 0)], sections[("LMUV", 0)]
    assert verdict(g, lm)[0] is not None
    image, report = verdict(g, lm, empty)
    assert image is None and "names no stock value" in report
    image, report = verdict(g, None)
    assert image is None and "lightmap" in report
    image, report = verdict(g[:-7], lm)
    assert image is None and "layout" in report
    lm_parts = geom.decode_lmuv(lm)
    image, report = verdict(g, geom.encode_lmuv(lm_parts[:-1]))
    assert image is None and "lightmap" in report
    # a lightmap REF past the stock material's vertices
    broken = [list(part) for part in lm_parts]
    broken[0][0] = (REF, 10 ** 6)
    image, report = verdict(g, geom.encode_lmuv(broken))
    assert image is None and "names no stock value" in report


def test_negative_controls(tool, synth):
    """section 2.4: each control must fail and a clean remodel pass"""
    _numpy()
    from geopatch import scan_original
    results = scan_original.controls(synth, tool)
    names = [name for name, _, _ in results]
    assert names == ["clean remodel", "quantized stock vertex run", "compressed-to-uncompressed copy",
                     "single plane", "stolen run inserted before encoding", "raw retail bytes",
                     "another BSP's mesh pasted through the encoder", "stock mesh moved 1/1024 world unit",
                     "stock mesh moved 0.01 world unit", "stock mesh moved 0.1 world unit",
                     "stock mesh scaled 1.0001"]
    for name, should_fail, report in results:
        assert scan_original.caught(report) == should_fail, (name, report["findings"][:3], report["lint"][:3],
                                                             report["sections"])
    by_name = {name: report for name, _, report in results}
    # the pasted mesh is a hard failure (its nudged run), not only a review flag
    assert any("nudged" in finding for finding in by_name["another BSP's mesh pasted through the encoder"]["lint"])
    # a hair's move is in reach of the near share; a tenth of a unit, or a scale about a far point, only the
    # transform share sees
    for name in ("stock mesh moved 0.1 world unit", "stock mesh scaled 1.0001"):
        section = by_name[name]["sections"][0]
        assert section["near_share"] <= scan_original.HIGH_NEAR_SHARE < section["transform_share"], (name, section)
    clean = by_name["clean remodel"]["sections"][0]
    assert clean["near_share"] == 0 and clean["transform_share"] == 0
    # the clean remodel's sections are REF-heavy but ours
    clean = results[0][2]["sections"][0]
    assert 0 < clean["ref_share"] < 1


def test_lint_refusals(tool, synth):
    _, _, bsps = retail.read_map(synth)
    by_index = {b.index: b for b in bsps}
    material = by_index[0].materials[(0, 0)]

    def part(vertices, **changes):
        p = geom.Part(0, 0, material.surface_count, material.vertex_count, material.topology_check(), 0, 1,
                      vertices, [(0, 1, 2)], [0, 1])
        for k, v in changes.items():
            setattr(p, k, v)
        return p

    ours = [(LIT, (999999, 999999, 99999), (0, 0, 0), (0, 0)), (REF, 0), (REF, 1)]
    clean = prsx.write(CWID, [prsx.Section("GEOM", 0, geom.encode_geom([part(ours)]))])
    assert lint_patch.lint(clean, by_index) == []
    other = prsx.write(CWID, [prsx.Section("GEOM", 0, geom.encode_geom([part(
        [(REFM, (0, 1, 3), (0, 0, 0), (0, 0)), (REFC, 2, (0, 0, 0), (0, 0)), (REF, 1)])]))])
    assert lint_patch.lint(other, by_index) == []
    # stock coordinates taken by reference (REFPT, REFMT) are clean; written out (REFP, REFM) they are a copy
    own_tex = lint_patch._stock_tex(material, 2)
    other_tex = lint_patch._stock_tex(by_index[0].materials[(0, 1)], 3)
    taken = prsx.write(CWID, [prsx.Section("GEOM", 0, geom.encode_geom([part(
        [(REFPT, 2, (0, 0, 0), None), (REFMT, (0, 1, 3), (0, 0, 0), None), (REF, 1)])]))])
    assert lint_patch.lint(taken, by_index) == []
    # COLL: a stock plane by reference is clean (enhanced floors are stock planes), past the stock planes not
    stock_plane = prsx.write(CWID, [prsx.Section("COLL", 0, geom.encode_coll([(REF, 0)], [(REF, 0)]), collv=1)])
    assert lint_patch.lint(stock_plane, by_index) == []
    cases = {
        "REF past the stock vertices": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REF, material.vertex_count)] + ours[1:])]))],
        "REFM to no material": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFM, (5, 5, 0), (0, 0, 0), (0, 0))] + ours[1:])]))],
        "REFC past the collision vertices": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFC, 10 ** 6, (0, 0, 0), (0, 0))] + ours[1:])]))],
        "another revision": [prsx.Section("GEOM", 0, geom.encode_geom([part(ours, topology_check=1)]))],
        "REFP writes out the stock coordinates": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFP, 2, (0, 0, 0), own_tex)] + ours[1:])]))],
        "REFM writes out the stock coordinates": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFM, (0, 1, 3), (0, 0, 0), other_tex)] + ours[1:])]))],
        "a plane past the stock planes": [prsx.Section("COLL", 0, geom.encode_coll(
            [(REF, 0)], [(REF, len(by_index[0].planes))]), collv=1)],
        "no such material": [prsx.Section("GEOM", 0, geom.encode_geom([part(ours, material_index=40)]))],
        "unknown type": [prsx.Section("ZZZZ", 0, b"x", flags=0)],
        "no layout": [prsx.Section("DOBJ", 0, b"grass")],
        "not GEOM": [prsx.Section("GEOM", 0, b"PWG1 not ours")],
        "optional GEOM": [prsx.Section("GEOM", 0, geom.encode_geom([part(ours)]), flags=prsx.BAKE_RELEVANT)],
    }
    for name, sections in cases.items():
        assert lint_patch.lint(prsx.write(CWID, sections), by_index), name


def test_window_keys():
    """the scanner's window keys: position independent, the same as a direct sum, and a retail run found at any
    offset of our bytes, in pieces"""
    np = _numpy()
    import random
    rng = random.Random(5)
    data = bytes(rng.randrange(256) for _ in range(500))
    keys = retail.rolling_keys(data, 1)
    inverse = pow(retail.ROLL_BASE, -1, 1 << 64)
    for at in (0, 7, 452):
        direct = sum(data[at + k] * pow(inverse, k, 1 << 64) for k in range(48)) % (1 << 64)
        assert int(keys[at]) == direct
    assert int(retail.rolling_keys(b"\0" * 9 + data[100:148], 1)[9]) == int(keys[100])
    sets = retail.RetailSets()
    sets._add_windows(data)
    ours = bytes(rng.randrange(256) for _ in range(5 << 20))
    ours = ours[:(4 << 20) - 20] + data[200:260] + ours[(4 << 20) - 20:]
    assert sets.window_hits(ours)[0] == (4 << 20) - 20 - (200 % 4 and 4 - 200 % 4)
    assert sets.window_hits(bytes(1000)) == []


# ---------- the signed manifest

def _manifest(sidecar_name, sidecar_blob, bsp_count=2):
    opened = prsx.Sidecar(sidecar_blob)
    enhanced = {str(b): identity.world_id_enhanced(opened, b) for b in range(bsp_count)}

    def packs(ids, variant):
        return [{"bsp": b, "world_id": ids[b], "name": "synth.%s.ppk" % variant, "file_sha": "0" * 64,
                 "size": 1000, "bake_version": 7} for b in range(bsp_count)]
    return {
        "schema": "prism-release/1", "build": "b10", "content_release": "content-b10-r1", "bake_versions": [7, 6],
        "scene_version": identity.SCENE_VERSION, "render_format": identity.RENDER_FORMAT, "pack_format": 1,
        "prsx_formats": [prsx.FORMAT],
        "maps": {"synth": {"worlds": [{
            "classic_world_id": CWID.hex(),
            "base_ids": [{"base_id": "b" * 64, "build": "01.10.12.2276", "region": "NTSC"}],
            "bsp_count": bsp_count,
            "sidecar": {"name": sidecar_name, "file_sha": hashlib.sha256(sidecar_blob).hexdigest(),
                        "size": len(sidecar_blob), "prsx_format": prsx.FORMAT, "world_ids": enhanced,
                        "collvs": sorted({e.collv for e in opened.entries if e.collv})},
            "packs": {"classic": packs({b: identity.world_id_classic(CWID, b) for b in range(bsp_count)}, "classic"),
                      "enhanced": packs({b: enhanced[str(b)] for b in range(bsp_count)}, "enhanced")},
        }]}},
    }


def test_manifest_rules_and_signature(tmp_path):
    blob = prsx.write(CWID, [prsx.Section("GEOM", 0, b"g0"), prsx.Section("GEOM", 1, b"g1"),
                             prsx.Section("COLL", 0, b"c", collv=1)])
    name = "synth.%s.prsx" % hashlib.sha256(blob).hexdigest()[:8]
    (tmp_path / name).write_bytes(blob)
    good = _manifest(name, blob)
    assert manifest.check(good, tmp_path) == []
    path = tmp_path / "prism_release.json"
    path.write_text(manifest.dumps(good))
    assert manifest.main(["check", str(path), "--sidecars", str(tmp_path)]) == 0
    seed = bytes(range(32))
    key = tmp_path / "seed"
    key.write_text(seed.hex())
    assert manifest.main(["sign", str(path), "--key-file", str(key)]) == 0
    sys.path.insert(0, str(ROOT / "tools"))
    import ed25519_ref
    public = ed25519_ref.public_key(seed).hex()
    assert manifest.main(["verify", str(path), "--public-key", public]) == 0
    assert manifest.main(["verify", str(path)]) == 1, "a test key is not the release key"
    path.write_text(manifest.dumps(good).replace("b10", "b11"))
    assert manifest.main(["verify", str(path), "--public-key", public]) == 1, "a changed byte breaks the signature"

    def broken(change):
        m = json.loads(json.dumps(good))
        change(m)
        return manifest.check(m, tmp_path)
    world = lambda m: m["maps"]["synth"]["worlds"][0]  # noqa: E731
    cases = {
        "schema: unknown field": lambda m: m.update(extra=1),
        "schema: bad id": lambda m: world(m).update(classic_world_id="xyz"),
        "classic pack for another world": lambda m: world(m)["packs"]["classic"][0].update(world_id="c" * 64),
        "a missing BSP pack": lambda m: world(m)["packs"]["enhanced"].pop(),
        "a bake version not accepted": lambda m: world(m)["packs"]["classic"][1].update(bake_version=5),
        "base_id twice": lambda m: m["maps"].update(other={"worlds": [dict(world(m), classic_world_id="d" * 64,
                                                                          sidecar=None)]}) or
        m["maps"]["other"]["worlds"][0].pop("sidecar"),
        "sidecar name": lambda m: world(m)["sidecar"].update(name="synth.00000000.prsx"),
        "sidecar world id": lambda m: world(m)["sidecar"]["world_ids"].update({"0": "e" * 64}),
        "enhanced packs without a sidecar": lambda m: world(m).pop("sidecar"),
        "collision versions": lambda m: world(m)["sidecar"].update(collvs=[1, 2]),
    }
    for label, change in cases.items():
        assert broken(change), label
