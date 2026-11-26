"""The enhanced world's sidecar (PLAN_FINAL.md sections 2.3 and 2.4), asset-free on synthetic maps
(tools/geopatch/synthmap.py): the game's reader (port/linux/game/prism_prsx.c) and the farm's writer agree and
refuse the same malformed files, the exact REF decode (prism_ref_decode.c) gives the game's own float decode bit
for bit, GEOM and COLL round-trip, the encoder resolves stock vertices to REF ops, the lint refuses literals where a
REF is required, the tuple scanner's negative controls all fail while a clean remodel passes, and the signed
manifest follows its schema and rules."""
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from geopatch import geom, identity, lint_patch, manifest, native, prsx, retail, synthmap  # noqa: E402
from geopatch.geom import LIT, REF, REFC, REFM, REFP  # noqa: E402

CWID = bytes.fromhex("aa" * 32)


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    try:
        return native.Native(tmp_path_factory.mktemp("native"))
    except native.NativeError as error:
        pytest.skip("no C compiler builds the game's sidecar code: %s" % error)


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
        pytest.skip("no C compiler")
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
    zstd = prsx.write(CWID, [prsx.Section("GEOM", 0, b"z" * 100, codec=prsx.ZSTD)]) if shutil.which("zstd") else None
    if zstd:
        verdict = write_and_open(tool, tmp_path, zstd, "z.prsx")
        assert verdict["entries"][0]["read"] == "a section in a codec this build does not read"
    trailing = bytearray(prsx.write(CWID, [prsx.Section("GEOM", 0, b"q" * 3000)]))
    struct.pack_into("<Q", trailing, 48 + 28, 2999)  # usize one short of what the stream inflates to
    assert write_and_open(tool, tmp_path, bytes(trailing), "t.prsx")["entries"][0]["read"] == \
        "a section of the wrong size"


# ---------- GEOM and COLL

def _sample_part(material_index=0):
    vertices = [(REF, 0), (REF, 1), (LIT, (100, -200, 3000), (1, 2, 3), (4, 5)),
                (REFP, 2, (6, 7, 8), (9, 10)), (LIT, (-50, 0, 2990), (11, 12, 13), (14, 15)),
                (REFM, (1, 3, 77), (16, 17, 18), (19, 20)), (REFC, 5, (21, 22, 23), (24, 25))]
    return geom.Part(0, material_index, 4, 3, 0xDEADBEEF, 0, 2, vertices,
                     [(0, 1, 2), (1, 3, 2), (2, 3, 4), (4, 5, 6)],
                     [0, 2, 4], near_triangle_count=2, far_distance=55.5, far_runs=[(2, 1), (0, 0)],
                     spheres=[(1, 2, 3, 4), (5, 6, 7, 8)])


def test_geom_round_trip_and_numpy_twin():
    np = pytest.importorskip("numpy")
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
        e.refs = np.array([v[1][2] if v[0] == REFM else v[1] for v in p.vertices if v[0] != LIT], np.uint32)
        e.keys = np.array([v[1][0] << 16 | v[1][1] for v in p.vertices if v[0] == REFM], np.uint32)
        e.q = np.array([v[1] for v in p.vertices if v[0] == LIT], np.int64).reshape(-1, 3)
        e.nrm = np.array([v[2] for v in p.vertices if v[0] != REF], np.uint32)
        e.tex = np.array([v[3] for v in p.vertices if v[0] != REF], np.uint32)
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
        struct.pack_into("<I", data, 16 + 20, 5)  # surface count past its runs
    elif damage == "trailing":
        data += b"\0"
    elif damage == "column":
        at = 16 + geom.PART_SIZE + 16 * 2 + 8  # VPOS's element count
        struct.pack_into("<I", data, at, struct.unpack_from("<I", data, at)[0] + 1)
    elif damage == "op":
        at = 16 + geom.PART_SIZE + 16 * 9  # the first column's bytes: VOPS
        data[at] = 7
    with pytest.raises(geom.LayoutError):
        geom.decode_geom(bytes(data))


def test_coll_round_trip():
    planes = [(geom.float_bits(0.0), geom.float_bits(0.0), geom.float_bits(1.0), geom.float_bits(-3.5))]
    vertices = [(REF, 4), (LIT, (1, 2, 3)), (LIT, (-7, 8, 9))]
    assert geom.decode_coll(geom.encode_coll(vertices, planes)) == (vertices, planes)
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
    pytest.importorskip("numpy")
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


def test_negative_controls(tool, synth):
    """section 2.4: each control must fail and a clean remodel pass"""
    pytest.importorskip("numpy")
    from geopatch import scan_original
    results = scan_original.controls(synth, tool)
    names = [name for name, _, _ in results]
    assert names == ["clean remodel", "quantized stock vertex run", "compressed-to-uncompressed copy",
                     "single plane", "stolen run inserted before encoding", "raw retail bytes"]
    for name, should_fail, report in results:
        assert bool(report["findings"]) == should_fail, (name, report["findings"][:3])
        if not should_fail:
            assert not report["lint"], report["lint"]
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
    cases = {
        "REF past the stock vertices": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REF, material.vertex_count)] + ours[1:])]))],
        "REFM to no material": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFM, (5, 5, 0), (0, 0, 0), (0, 0))] + ours[1:])]))],
        "REFC past the collision vertices": [prsx.Section("GEOM", 0, geom.encode_geom([part(
            [(REFC, 10 ** 6, (0, 0, 0), (0, 0))] + ours[1:])]))],
        "another revision": [prsx.Section("GEOM", 0, geom.encode_geom([part(ours, topology_check=1)]))],
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
    np = pytest.importorskip("numpy")
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
