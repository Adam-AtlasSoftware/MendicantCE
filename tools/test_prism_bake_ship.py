"""tools/prism_bake_ship.py: a shipped bake's export, archives, install and
checks, on bake files made here in prism_bake_cache.h's format"""

import os
import struct
import tarfile
import zlib
from pathlib import Path

import pytest

import prism_bake_ship as ship


def _header(map_name="b30", bsp=0, quality=3, baker=19, world_id=b"\0" * 32, variant=0):
    h = bytearray(256)
    h[:8] = b"PRISMBAK"
    struct.pack_into("<IIIi", h, 8, 3, 256, 0x1234ABCD, bsp)
    h[24:24 + len(map_name)] = map_name.encode()
    h[88:92] = f"bsp{bsp}".encode()
    struct.pack_into("<5Q", h, 152, 1, 2, 3, 4, 5)
    struct.pack_into("<IIf", h, 192, baker, quality, 0.01)
    h[204:236] = world_id
    struct.pack_into("<II", h, 236, 1, variant)
    struct.pack_into("<I", h, 252, zlib.crc32(bytes(h[:252])) & 0xFFFFFFFF)
    return bytes(h)


def _record(kind, payload, sequence, job=0):
    h = bytearray(40)
    h[:4] = b"PBKR"
    struct.pack_into("<IQIIIII", h, 4, ship.fourcc(kind), job, sequence, len(payload),
                     zlib.crc32(payload) & 0xFFFFFFFF, 1, 0)
    struct.pack_into("<I", h, 36, zlib.crc32(bytes(h[:36])) & 0xFFFFFFFF)
    return bytes(h) + payload + b"\0" * (-len(payload) % 8)


def _bake(kinds=("PLAN", "LYRB", "ATLS", "ATLY", "MASK", "VOLB", "VOLY", "PROB", "DONE"), stock=True, **header):
    header.setdefault("variant", 0 if stock else 1)
    if not stock:
        header.setdefault("world_id", bytes([0xE0 + header.get("bsp", 0)]) * 32)
    data = _header(**header)
    for sequence, kind in enumerate(kinds):
        payload = struct.pack("<II", 1, 1 if stock else 0) + b"\0" * 88 if kind == "LYRB" else bytes(range(13))
        data += _record(kind, payload, sequence)
    return data


def _folder(tmp_path, maps=("b30", "a30"), bsps=2, baked=None, note=True, rig="units", **bake):
    folder = tmp_path / "prism_bake"
    for map_name in maps:
        (folder / map_name).mkdir(parents=True)
        for bsp in (range(bsps) if baked is None else baked):
            (folder / map_name / f"bsp{bsp}.pbk").write_bytes(_bake(map_name=map_name, bsp=bsp, **bake))
        if note:
            (folder / map_name / "baked.txt").write_text(
                f"bsps {bsps}\nbsp0 v19 ultra {rig} cutouts geo:stock:levels/{map_name}/{map_name}\n")
        (folder / map_name / f"bsp0.reach").write_bytes(b"walk")
    return folder


def test_export_writes_lighting_only_files_and_manifests(tmp_path):
    folder = _folder(tmp_path)
    shipped = tmp_path / "shipped"
    entries = ship.export(folder, "stock", shipped, None, "max", None)
    assert sorted(e["file"] for e in entries) == ["stock/a30/bsp0.pbk", "stock/a30/bsp1.pbk", "stock/b30/bsp0.pbk",
                                                  "stock/b30/bsp1.pbk"]
    # (the reach's walk and the note stay behind: only the bakes ship)
    assert not list(shipped.rglob("*.reach")) and not list(shipped.rglob("baked.txt"))
    lines = (shipped / "manifest.txt").read_text().splitlines()
    assert lines[0] == "prism_bake_shipped 1"
    assert "bsp stock b30 0 2 v19 max units cutouts geo:stock:levels/b30/b30" in lines
    assert "bsp stock b30 1 2 v19 max units cutouts geo:stock" in lines
    assert ship.verify(shipped) == []


def test_export_refuses_what_is_not_a_whole_light_bake(tmp_path):
    with pytest.raises(ship.BakeError, match="DONE"):
        ship.check_bake(_bake(kinds=("PLAN", "ATLS")))
    with pytest.raises(ship.BakeError, match="ATLX"):
        ship.check_bake(_bake(kinds=("PLAN", "ATLX", "DONE")))
    with pytest.raises(ship.BakeError, match="does not know"):
        ship.check_bake(_bake(kinds=("PLAN", "TRIS", "DONE")))
    with pytest.raises(ship.BakeError, match="under max"):
        ship.check_bake(_bake(quality=2), "max")
    folder = _folder(tmp_path, maps=("b30",), stock=False)
    with pytest.raises(ship.BakeError, match="remodel"):
        ship.export(folder, "stock", tmp_path / "shipped", None, "draft", None)


def test_remodel_set_keys_its_geometry_by_kind(tmp_path):
    folder = _folder(tmp_path, maps=("b30",), stock=False, quality=0)
    shipped = tmp_path / "shipped"
    ship.export(folder, "remodel", shipped, None, "draft", None)
    lines = (shipped / "manifest.txt").read_text().splitlines()
    # (its key names the enhanced world_id the bake is keyed to: prism_world_source.c's ladder looks for it)
    assert "bsp remodel b30 1 2 v19 draft units cutouts geo:prsx:" + "e1" * 32 in lines
    level = (shipped / "remodel/b30/manifest.txt").read_text().splitlines()
    assert "bsp remodel b30 1 2 v19 draft units cutouts geo:prsx:" + "e1" * 32 in level
    # (a remodel bake keyed to no world_id, or of the classic variant, is refused)
    (folder / "b30" / "bsp1.pbk").write_bytes(_bake(map_name="b30", bsp=1, stock=False, quality=0,
                                                    world_id=b"\0" * 32))
    with pytest.raises(ship.BakeError, match="no world_id"):
        ship.export(folder, "remodel", tmp_path / "again", None, "draft", None)
    (folder / "b30" / "bsp1.pbk").write_bytes(_bake(map_name="b30", bsp=1, stock=False, quality=0, variant=0))
    with pytest.raises(ship.BakeError, match="variant"):
        ship.export(folder, "remodel", tmp_path / "again", None, "draft", None)


def test_stock_bakes_keyed_to_the_maps_classic_world(tmp_path):
    """with the farm's classic_world_id (--world-ids) a stock bake must be keyed to that map version's classic
    world_id; each level's manifest carries its "world <map> <base_id> <classic_world_id>" line, through an archive
    and its install too"""
    import sys
    sys.path.insert(0, str(Path(ship.__file__).resolve().parent))
    from geopatch import identity
    cwid = "ab" * 32
    ids = {"b30": {"base_id": "cd" * 32, "classic_world_id": cwid}}
    folder = _folder(tmp_path, maps=("b30",))
    for bsp in (0, 1):
        world = bytes.fromhex(identity.world_id_classic(bytes.fromhex(cwid), bsp))
        (folder / "b30" / f"bsp{bsp}.pbk").write_bytes(_bake(map_name="b30", bsp=bsp, world_id=world))
    shipped = tmp_path / "shipped"
    archives = tmp_path / "release"
    ship.export(folder, "stock", shipped, None, "max", archives, world_ids=ids)
    line = "world b30 " + "cd" * 32 + " " + cwid
    assert line in (shipped / "manifest.txt").read_text().splitlines()
    assert line in (shipped / "stock/b30/manifest.txt").read_text().splitlines()
    installed = tmp_path / "player"
    assert ship.install(installed, sorted(archives.iterdir())) == []
    assert line in (installed / "stock/b30/manifest.txt").read_text().splitlines()
    # (a bake keyed to another world: refused)
    (folder / "b30" / "bsp1.pbk").write_bytes(_bake(map_name="b30", bsp=1, world_id=b"\1" * 32))
    with pytest.raises(ship.BakeError, match="classic BSP 1"):
        ship.export(folder, "stock", tmp_path / "again", None, "max", None, world_ids=ids)


def test_archives_install_and_verify(tmp_path):
    folder = _folder(tmp_path)
    shipped = tmp_path / "shipped"
    archives = tmp_path / "release"
    ship.export(folder, "stock", shipped, ["b30"], "max", archives)
    paths = sorted(archives.iterdir())
    assert [p.name for p in paths] == ["prism_bake_b30_stock_v19.ppk"]
    installed = tmp_path / "player" / "prism_bake_shipped"
    assert ship.install(installed, paths) == []
    assert (installed / "stock/b30/bsp1.pbk").read_bytes() == (shipped / "stock/b30/bsp1.pbk").read_bytes()
    # (a damaged file is found)
    target = installed / "stock/b30/bsp1.pbk"
    target.write_bytes(target.read_bytes()[:-8] + b"\1" * 8)
    assert ship.verify(installed) and "sha256" in ship.verify(installed)[0]


def test_install_refuses_paths_outside_the_folder(tmp_path):
    evil = tmp_path / "evil.ppk"
    source = tmp_path / "x"
    source.write_bytes(b"x")
    with tarfile.open(evil, "w") as tar:
        tar.add(source, arcname="../../outside.pbk")
    with pytest.raises(ship.BakeError, match="not a shipped"):
        ship.install(tmp_path / "shipped", [evil])
    assert not (tmp_path.parent / "outside.pbk").exists()


def test_export_refuses_a_level_with_a_bsp_missing(tmp_path):
    """players never bake: a level whose last BSP has no bake is refused, and nothing is written"""
    folder = _folder(tmp_path, maps=("d40",), bsps=3, baked=(0, 1))
    shipped = tmp_path / "shipped"
    with pytest.raises(ship.BakeError, match=r"BSP 2 of 3 not baked"):
        ship.export(folder, "stock", shipped, None, "max", None)
    assert not shipped.exists()
    # (the count from --bsps alone, with no note: still refused)
    folder = _folder(tmp_path / "b", maps=("d40",), bsps=3, baked=(0, 1), note=False)
    with pytest.raises(ship.BakeError, match=r"BSP 2 of 3 not baked"):
        ship.export(folder, "stock", shipped, None, "max", None, bsps="d40:3")
    # (no count known at all: refused, not guessed from the files)
    with pytest.raises(ship.BakeError, match="BSP count is unknown"):
        ship.export(folder, "stock", shipped, None, "max", None)
    # (counts that disagree: refused)
    with pytest.raises(ship.BakeError, match="disagree"):
        ship.export(_folder(tmp_path / "c", maps=("d40",), bsps=2), "stock", shipped, None, "max", None,
                    bsps="d40:3")
    # (a bare --bsps number with --maps naming one map)
    with pytest.raises(ship.BakeError, match=r"BSP 2 of 3 not baked"):
        ship.export(folder, "stock", shipped, ["d40"], "max", None, bsps="3")
    assert not shipped.exists()


def test_export_lists_an_empty_bsp_so_the_level_is_whole(tmp_path):
    """a BSP with nothing to bake (d40's BSP 9) is named --empty: listed with no file, the level whole"""
    folder = _folder(tmp_path, maps=("d40",), bsps=3, baked=(0, 1))
    shipped = tmp_path / "shipped"
    entries = ship.export(folder, "stock", shipped, None, "max", None, empty="d40:2")
    assert [(e["bsp"], e["of"], bool(e.get("empty"))) for e in entries] == [(0, 3, False), (1, 3, False),
                                                                            (2, 3, True)]
    lines = (shipped / "manifest.txt").read_text().splitlines()
    assert "bsp stock d40 2 3 v19 max units cutouts geo:stock" in lines
    assert not (shipped / "stock/d40/bsp2.pbk").exists()
    assert ship.verify(shipped) == []
    archives = tmp_path / "release"
    ship.write_archives(shipped, ship.manifest_load(shipped), archives, {("stock", "d40")})
    installed = tmp_path / "player"
    assert ship.install(installed, sorted(archives.iterdir())) == []
    # (a BSP named empty that has a bake is a mistake)
    with pytest.raises(ship.BakeError, match="named --empty but has a bake"):
        ship.export(folder, "stock", tmp_path / "s2", None, "max", None, empty="d40:1")


def test_verify_finds_a_level_not_whole(tmp_path):
    folder = _folder(tmp_path, maps=("b30",))
    shipped = tmp_path / "shipped"
    ship.export(folder, "stock", shipped, None, "max", None)
    manifest = ship.manifest_load(shipped)
    manifest["bsps"] = [e for e in manifest["bsps"] if e["bsp"] != 1]
    ship.manifest_save(shipped, manifest)
    assert any("not whole" in p for p in ship.verify(shipped))


def test_export_refuses_a_stock_bake_as_the_remodel(tmp_path):
    """the LYRB record says which world a bake is of: a stock one is never shipped as the remodel's"""
    folder = _folder(tmp_path, maps=("b30",), stock=True)
    with pytest.raises(ship.BakeError, match="stock world .*exported as the remodel"):
        ship.export(folder, "remodel", tmp_path / "shipped", None, "max", None)
    # (and a bake with no LYRB record is of no known world)
    folder = _folder(tmp_path / "b", maps=("b30",), kinds=("PLAN", "ATLS", "DONE"))
    with pytest.raises(ship.BakeError, match="no LYRB"):
        ship.export(folder, "stock", tmp_path / "shipped", None, "max", None)
    assert not (tmp_path / "shipped").exists()


def test_rig_is_always_units(tmp_path):
    """no --rig: every key says units, and a note of a bake made with the fitted rig is refused"""
    folder = _folder(tmp_path, maps=("b30",))
    shipped = tmp_path / "shipped"
    entries = ship.export(folder, "stock", shipped, None, "max", None)
    assert all(" units cutouts " in e["key"] + " " for e in entries)
    with pytest.raises(SystemExit):
        ship.main(["export", str(folder), "stock", str(tmp_path / "s2"), "--rig", "fitted"])
    folder = _folder(tmp_path / "b", maps=("b30",), rig="fitted")
    with pytest.raises(ship.BakeError, match="fitted rig"):
        ship.export(folder, "stock", tmp_path / "s3", None, "max", None)


@pytest.mark.skipif(not os.environ.get("HALO_DATA_ROOT"), reason="needs the player's maps (HALO_DATA_ROOT)")
def test_bsp_count_from_the_map(tmp_path):
    """the level's BSP count read from the player's own d40 (ten BSPs, the last a cinematic's with nothing to bake):
    its last BSP missing is refused, named --empty it ships whole"""
    maps = Path(os.environ["HALO_DATA_ROOT"]) / "maps"
    if not (maps / "d40.map").is_file():
        pytest.skip("no d40.map")
    assert ship.map_bsp_count(maps, "d40") == 10
    folder = _folder(tmp_path, maps=("d40",), bsps=10, baked=range(9), note=False)
    with pytest.raises(ship.BakeError, match=r"BSP 9 of 10 not baked"):
        ship.export(folder, "stock", tmp_path / "shipped", None, "max", None, maps_folder=maps)
    entries = ship.export(folder, "stock", tmp_path / "shipped", None, "max", None, maps_folder=maps, empty="d40:9")
    assert len(entries) == 10 and entries[-1]["empty"]
