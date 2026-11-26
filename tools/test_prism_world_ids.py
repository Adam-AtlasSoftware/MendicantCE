"""The enhanced world's identities (port/linux/game/prism_identity.c, tools/geopatch/identity.py; PLAN_FINAL.md
section 2.5) on synthetic maps and sidecars, asset-free: base_id names a map's inflated body whatever its
compression; world_id depends on what a bake depends on and on nothing else, so a collision change, a new runtime
section, a recompressed sidecar or a format number never moves it; the game's C and the farm's Python agree, and
pinned values hold on every platform (the same derivation on Linux and Windows)."""
import hashlib
import os
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from geopatch import identity, native, prsx, synthmap  # noqa: E402

CWID = bytes(range(32))


@pytest.fixture(scope="module")
def tool(tmp_path_factory):
    try:
        return native.Native(tmp_path_factory.mktemp("native"))
    except native.NativeError as error:
        # (in CI a missing compiler fails: a green step must have run the game's C)
        (pytest.fail if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS") else pytest.skip)(
            "no C compiler builds the game's sidecar code: %s" % error)


def sections(geom0=b"geometry zero", geom1=b"geometry one", extra=()):
    out = [prsx.Section("GEOM", 0, geom0), prsx.Section("GEOM", 1, geom1),
           prsx.Section("LMUV", 0, b"lightmap uv zero"), prsx.Section("SCEN", 1, b"scenery one"),
           prsx.Section("RHGT", 0, b"height zero"), prsx.Section("COLL", 0, b"collision", collv=1)]
    return out + list(extra)


def ids_of(tool, tmp_path, blob, name="s.prsx"):
    path = tmp_path / name
    path.write_bytes(blob)
    verdict = tool.open(path)
    assert verdict["result"] == "ok", verdict
    python = identity.world_ids(prsx.Sidecar(blob))
    assert python == verdict["world"], "the game's C and the farm's Python disagree"
    return python


def test_type_table_twins():
    """prism_prsx.c's table and prsx.py's TYPES say the same class for every type"""
    code = (ROOT / "port/linux/game/prism_prsx.c").read_text()
    table = dict(re.findall(r"\{ PRSX_([A-Z]{4}), PRSX_TYPE_([A-Z]+) \}", code))
    names = {"BAKE": prsx.BAKE, "RUNTIME": prsx.RUNTIME, "SIM": prsx.SIM}
    assert {k: names[v] for k, v in table.items()} == prsx.TYPES
    header = (ROOT / "port/linux/game/prism_prsx.h").read_text()
    for kind in prsx.TYPES:
        assert "PRSX_FOURCC('%s', '%s', '%s', '%s')" % tuple(kind) in header


def test_world_ids_agree_and_are_pinned(tool, tmp_path):
    ids = ids_of(tool, tmp_path, prsx.write(CWID, sections()))
    assert sorted(ids) == [0, 1]
    # pinned: the same derivation on every platform and compiler (CI runs this on Linux and Windows)
    assert ids[0][0] == identity.world_id_classic(CWID, 0)
    assert hashlib.sha256(b"PRSX-WORLD-C" + CWID + b"\x00\x00\x01\x00\x00\x00").hexdigest() == ids[0][0]
    assert ids == PINNED


PINNED = {
    0: ("573812b9127094376a8c37b474993242d77d8d481ea9eec7405a8872bc83f84d",
        "6d23e162624000dfdcd52cd214e4d4b06b828401f68e3613acf4105d66eebbdc"),
    1: ("54fa5f3583460f332ed9221e1823d140efd4e5e499eada907f2c34fefd6e7d3a",
        "ae99531c889fff7dabcf1fbbf3c254587b3aa8105f58739248543d494844d9c0"),
}


def test_non_bake_sections_never_move_a_world_id(tool, tmp_path):
    base = ids_of(tool, tmp_path, prsx.write(CWID, sections()), "base.prsx")
    variants = {
        "collision changed": [s if s.kind != "COLL" else prsx.Section("COLL", 0, b"other collision", collv=1)
                              for s in sections()],
        "a new collision version": sections(extra=[prsx.Section("COLL", 0, b"v2", collv=2),
                                                   prsx.Section("PATH", 0, b"paths", collv=2),
                                                   prsx.Section("COLM", 1, b"scenery collision", collv=2)]),
        "runtime changed": [s if s.kind != "RHGT" else prsx.Section("RHGT", 0, b"other heights")
                            for s in sections()] + [prsx.Section("DOBJ", 1, b"grass")],
        "an unknown optional section": sections(extra=[prsx.Section("FUTR", 0, b"later", flags=0)]),
        "recompressed": [prsx.Section(s.kind, s.bsp, s.data, collv=s.collv, codec=prsx.STORED) for s in sections()],
        "zlib level 1": [prsx.Section(s.kind, s.bsp, s.data, collv=s.collv, level=1) for s in sections()],
    }
    for name, variant in variants.items():
        blob = prsx.write(CWID, variant)
        assert ids_of(tool, tmp_path, blob, "v.prsx") == base, name
        # (the file's own SHA differs: integrity only, never in a key)
        if name in ("recompressed", "zlib level 1"):
            assert hashlib.sha256(blob).digest() != hashlib.sha256(prsx.write(CWID, sections())).digest()


def test_bake_sections_move_only_their_bsp(tool, tmp_path):
    base = ids_of(tool, tmp_path, prsx.write(CWID, sections()), "base.prsx")
    changed = ids_of(tool, tmp_path, prsx.write(CWID, sections(geom0=b"geometry zero, remodelled")), "c.prsx")
    assert changed[0][1] != base[0][1] and changed[1] == base[1]
    assert changed[0][0] == base[0][0], "a classic world_id never depends on the sidecar"
    lmuv = [s if s.kind != "LMUV" else prsx.Section("LMUV", 0, b"another layout") for s in sections()]
    assert ids_of(tool, tmp_path, prsx.write(CWID, lmuv), "l.prsx")[0][1] != base[0][1]
    # a section for every BSP (BSP_ALL) enters every BSP's id
    shared = ids_of(tool, tmp_path, prsx.write(CWID, sections(extra=[prsx.Section("SCEN", prsx.BSP_ALL, b"x")])),
                    "a.prsx")
    assert shared[0][1] != base[0][1] and shared[1][1] != base[1][1]
    # another map version's inputs: every id moves
    other = ids_of(tool, tmp_path, prsx.write(bytes(32), sections()), "o.prsx")
    assert all(other[b][k] != base[b][k] for b in (0, 1) for k in (0, 1))


def test_bake_flag_is_the_tables(tool, tmp_path):
    """a section cannot opt out of (or into) the world_id by its flag: both readers refuse a known type whose flag
    disagrees with the table"""
    for section in (prsx.Section("GEOM", 0, b"g", flags=prsx.REQUIRED),
                    prsx.Section("RHGT", 0, b"r", flags=prsx.REQUIRED | prsx.BAKE_RELEVANT),
                    prsx.Section("COLL", 0, b"c", collv=1, flags=prsx.REQUIRED | prsx.BAKE_RELEVANT),
                    # a type this build does not know, bake relevant but optional: it would skip the section yet
                    # count it, and load packs baked for geometry it does not draw
                    prsx.Section("TREE", 0, b"trunks", flags=prsx.BAKE_RELEVANT)):
        blob = prsx.write(CWID, [prsx.Section("GEOM", 0, b"g"), section] if section.kind == "TREE" else [section])
        with pytest.raises(prsx.PrsxError):
            prsx.Sidecar(blob)
        path = tmp_path / "bad.prsx"
        path.write_bytes(blob)
        assert tool.open(path)["result"] == "a section whose bake class disagrees with this build's"


def test_base_id_of_every_form(tool, tmp_path):
    inflated = synthmap.build(geometries=[synthmap.Geometry(3)])
    expected = hashlib.sha256(inflated[0x800:]).hexdigest()
    forms = {"disc": synthmap.compress(inflated), "recompressed": synthmap.compress(inflated, 9),
             "inflated": inflated, "slot": synthmap.slot(inflated)}
    for name, data in forms.items():
        path = tmp_path / ("%s.map" % name)
        path.write_bytes(data)
        assert identity.base_id(data) == expected, name
        assert tool.base_id(path) == expected, name
    # the header is not the body: a forged checksum is the same base_id; another body is another
    forged = bytearray(inflated)
    forged[0x64:0x68] = b"\x01\x02\x03\x04"
    assert identity.base_id(bytes(forged)) == expected
    other = synthmap.build(geometries=[synthmap.Geometry(4)])
    assert identity.base_id(other) != expected
    # a cut stream or a short file is no base_id
    cut = tmp_path / "cut.map"
    cut.write_bytes(forms["disc"][:-200])
    assert tool.base_id(cut) is None
    with pytest.raises(Exception):
        identity.base_id(forms["disc"][:-200])


def test_world_id_is_the_contents_not_the_order(tool, tmp_path):
    """the writer sorts the directory whatever order it is given, and both readers refuse one that is not sorted:
    one content, one file, one world_id"""
    forward = prsx.write(CWID, sections())
    backward = prsx.write(CWID, list(reversed(sections())))
    assert forward == backward
    keys = [(prsx.fourcc(e.kind), e.bsp, e.collv) for e in prsx.Sidecar(forward).entries]
    assert keys == sorted(keys)
    ids = ids_of(tool, tmp_path, forward)
    assert ids == ids_of(tool, tmp_path, backward, "b.prsx")
    # the first two directory entries swapped (each still pointing at its own bytes)
    swapped = bytearray(forward)
    first, second = prsx.HEADER_SIZE, prsx.HEADER_SIZE + prsx.ENTRY_SIZE
    swapped[first:first + prsx.ENTRY_SIZE], swapped[second:second + prsx.ENTRY_SIZE] = \
        forward[second:second + prsx.ENTRY_SIZE], forward[first:first + prsx.ENTRY_SIZE]
    path = tmp_path / "swapped.prsx"
    path.write_bytes(bytes(swapped))
    assert tool.open(path)["result"] == "a malformed directory"
    with pytest.raises(prsx.PrsxError):
        prsx.Sidecar(bytes(swapped))


def test_scene_version_is_one_number():
    """SCENE_VERSION, which every world_id holds, is one number in the game (prism_identity.h), the baker's key
    (xgpu_stream.h's XGPU_BAKE_SCENE_VERSION) and the farm's Python (identity.py)"""
    game = (ROOT / "port/linux/game/prism_identity.h").read_text()
    stream = (ROOT / "port/linux/src/xgpu_stream.h").read_text()
    a = int(re.search(r"#define PRISM_SCENE_VERSION (\d+)u", game).group(1))
    b = int(re.search(r"#define XGPU_BAKE_SCENE_VERSION (\d+)u", stream).group(1))
    assert a == b == identity.SCENE_VERSION


def test_classic_world_id_of_a_map(tmp_path):
    """the farm's classic_world_id (tools/geopatch/classic_world.py): the same map hashes the same, recompressed
    or not; a change to a bake input (a BSP's bytes) moves it; a change outside the bake's inputs (the header's
    name text, the zlib stream's level) does not"""
    from geopatch import classic_world
    inflated = synthmap.build(geometries=[synthmap.Geometry(1)])
    a, b = tmp_path / "a.map", tmp_path / "b.map"
    a.write_bytes(synthmap.compress(inflated))
    b.write_bytes(inflated)
    cwid_a, report = classic_world.classic_world_id(a)
    assert report["bsps"] == 1 and len(cwid_a) == 64
    assert classic_world.classic_world_id(b)[0] == cwid_a
    # (a BSP byte changed: its file offset from the scenario's reference)
    import struct
    sys.path.insert(0, str(ROOT / "tools/prism_bake"))
    import ce_map
    m = ce_map.Map(str(b))
    reference = m.cache.block(m.scenario + ce_map.SCNR_STRUCTURE_BSPS, 0x20, 32)[0]
    offset, size = struct.unpack("<ii", m.cache.read(reference, 8))
    changed = bytearray(inflated)
    changed[offset + size - 1] ^= 0xFF
    c = tmp_path / "c.map"
    c.write_bytes(bytes(changed))
    assert classic_world.classic_world_id(c)[0] != cwid_a
