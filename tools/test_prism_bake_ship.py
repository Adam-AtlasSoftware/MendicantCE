"""tools/prism_bake_ship.py: a shipped bake's export, archives, install and
checks, on bake files made here in prism_bake_cache.h's format"""

import struct
import tarfile
import zlib

import pytest

import prism_bake_ship as ship


def _header(map_name="b30", bsp=0, quality=3, baker=19):
    h = bytearray(256)
    h[:8] = b"PRISMBAK"
    struct.pack_into("<IIIi", h, 8, 3, 256, 0x1234ABCD, bsp)
    h[24:24 + len(map_name)] = map_name.encode()
    h[88:92] = f"bsp{bsp}".encode()
    struct.pack_into("<5Q", h, 152, 1, 2, 3, 4, 5)
    struct.pack_into("<IIf", h, 192, baker, quality, 0.01)
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
    data = _header(**header)
    for sequence, kind in enumerate(kinds):
        payload = struct.pack("<II", 1, 1 if stock else 0) + b"\0" * 88 if kind == "LYRB" else bytes(range(13))
        data += _record(kind, payload, sequence)
    return data


def _folder(tmp_path, maps=("b30", "a30"), **bake):
    folder = tmp_path / "prism_bake"
    for map_name in maps:
        (folder / map_name).mkdir(parents=True)
        for bsp in range(2):
            (folder / map_name / f"bsp{bsp}.pbk").write_bytes(_bake(map_name=map_name, bsp=bsp, **bake))
        (folder / map_name / "baked.txt").write_text(
            f"bsps 2\nbsp0 v19 ultra units cutouts geo:stock:levels/{map_name}/{map_name}\n")
        (folder / map_name / f"bsp0.reach").write_bytes(b"walk")
    return folder


def test_export_writes_lighting_only_files_and_manifests(tmp_path):
    folder = _folder(tmp_path)
    shipped = tmp_path / "shipped"
    entries = ship.export(folder, "stock", shipped, None, "max", "units", None)
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
        ship.export(folder, "stock", tmp_path / "shipped", None, "draft", "units", None)


def test_remodel_set_keys_its_geometry_by_kind(tmp_path):
    folder = _folder(tmp_path, maps=("b30",), stock=False, quality=0)
    shipped = tmp_path / "shipped"
    ship.export(folder, "remodel", shipped, None, "draft", "units", None)
    lines = (shipped / "manifest.txt").read_text().splitlines()
    assert "bsp remodel b30 1 2 v19 draft units cutouts geo:pwg" in lines


def test_archives_install_and_verify(tmp_path):
    folder = _folder(tmp_path)
    shipped = tmp_path / "shipped"
    archives = tmp_path / "release"
    ship.export(folder, "stock", shipped, ["b30"], "max", "units", archives)
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
