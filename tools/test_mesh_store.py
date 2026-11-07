"""PRISM's mesh store (port/linux/vulkan/xgpu_mesh_store.c) over a fake backend
(port/linux/vulkan/tests/mesh_store_test.c): the .pmesh checks at the map's record and as a mesh is read,
residency by level, the budget, generations; again with ASan and UBSan, and with the worker thread
under ThreadSanitizer (any report fails it). And the Python side (tools/pmesh.py): a file it writes
passes its own checks and the host's reading of the same bytes. No GPU or game data is needed."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "port/linux/vulkan/tests/mesh_store_test.c", ROOT / "port/linux/vulkan/xgpu_mesh_store.c"]
sys.path.insert(0, str(ROOT / "tools"))
import pmesh  # noqa: E402


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


@pytest.mark.parametrize("mode", ["plain", "address", "thread"])
def test_mesh_store(tmp_path, mode):
    cc = compiler()
    if not cc or not sys.platform.startswith(("linux", "darwin")):
        pytest.skip("needs a C compiler on Linux or macOS")
    flags = {"plain": [], "address": ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
             "thread": ["-fsanitize=thread"]}[mode]
    binary = tmp_path / "mesh_store_test"
    try:
        subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE", *flags,
                        f"-I{ROOT / 'port/linux/vulkan'}", f"-I{ROOT / 'port/linux/src'}", *map(str, SOURCES),
                        "-o", str(binary), "-lpthread"],
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        if mode != "plain" and "sanitize" in (error.stderr or ""):
            pytest.skip(f"the compiler has no {mode} sanitizer")
        raise AssertionError(error.stderr)
    folder = tmp_path / "files"
    folder.mkdir()
    environment = dict(os.environ, TSAN_OPTIONS="halt_on_error=1 exitcode=66",
                       ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
    arguments = [str(binary), str(folder)] + (["threads"] if mode == "thread" else [])
    run = subprocess.run(arguments, capture_output=True, text=True, env=environment, timeout=300)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "all passed" in run.stdout


def small_mesh(node_crc=0x1234):
    """a two-level mesh of three nodes and a helper, in the shapes the exporters write"""
    triangles = np.array([[0, 1, 2], [2, 1, 3]], dtype=np.uint32)
    vertices = np.zeros(4 + 3, dtype=pmesh.VERTEX_DTYPE)
    vertices["position"] = np.random.default_rng(1).normal(size=(7, 3))
    vertices["normal"] = pmesh.octahedral(np.tile([[0.0, 0.0, 1.0]], (7, 1)))
    vertices["tangent"] = pmesh.octahedral(np.tile([[1.0, 0.0, 0.0]], (7, 1)))
    vertices["uv0"] = 0.5
    vertices["joints"][:, 0] = [0, 1, 2, 3, 0, 1, 2]
    vertices["weights"] = pmesh.quantise_weights(np.array([[1.0, 0, 0, 0]] * 7))
    identity = np.hstack([np.eye(3), np.zeros((3, 1))])
    bones = [pmesh.Bone("root", -1, 0, identity, identity), pmesh.Bone("a", 0, 1, identity, identity),
             pmesh.Bone("b", 1, 2, identity, identity), pmesh.Bone("helper", 2, -1, identity, identity)]
    pieces = [pmesh.Piece(0, 0, 1, 0, 0, 0, 4, 0, 6), pmesh.Piece(0, 0, 1, 0, 0, 4, 3, 6, 3)]
    levels = [pmesh.Level(100.0, 0, 1, 0, 4, 0, 6), pmesh.Level(0.0, 1, 1, 4, 3, 6, 3)]
    indices = np.concatenate([triangles.reshape(-1), np.array([0, 1, 2], dtype=np.uint32)])
    return pmesh.Mesh(3, node_crc, bones, levels, pieces, vertices, indices, (0, 0, 0), 2.5, {"test": True})


def test_pmesh_round_trip(tmp_path):
    mesh = small_mesh()
    path = tmp_path / "x.pmesh"
    size = pmesh.write_pmesh(path, mesh)
    assert size == path.stat().st_size and size % 1 == 0
    back = pmesh.read_pmesh(path)
    assert back.node_count == 3 and back.node_crc == 0x1234 and len(back.bones) == 4
    assert [b.name for b in back.bones] == ["root", "a", "b", "helper"] and back.bones[3].node == -1
    assert back.levels[1].vertex_first == 4 and back.pieces[1].index_first == 6
    assert np.array_equal(back.vertices, mesh.vertices) and np.array_equal(back.indices, mesh.indices)
    assert back.meta == {"test": True}
    dump = {"nodes": [{}, {}, {}], "node_crc": 0x1234, "base_map_scale": [1.0, 1.0], "shaders": [{}, {}],
            "regions": [{"permutations": [{"geometries": [0, 0, 0, 0, 0]}]}],
            "geometries": [{"parts": [{"shader": 1}]}]}
    assert pmesh.check(back, dump) == []
    # (the host's refusals, as the checker finds them)
    dump["node_crc"] = 0x1235
    assert any("nodes" in p for p in pmesh.check(back, dump))
    dump["node_crc"] = 0x1234
    dump["geometries"][0]["parts"][0]["shader"] = 0
    assert any("no part's" in p for p in pmesh.check(back, dump))
    dump["geometries"][0]["parts"][0]["shader"] = 1
    dump["base_map_scale"] = [0.25, 1.0]
    assert any("uv0" in p for p in pmesh.check(back, dump))
    # (a damaged section)
    data = bytearray(path.read_bytes())
    data[-40] ^= 0xFF
    path.write_bytes(bytes(data))
    with pytest.raises(pmesh.FormatError):
        pmesh.read_pmesh(path)


def test_octahedral_round_trip():
    rng = np.random.default_rng(7)
    vectors = rng.normal(size=(2000, 3))
    vectors /= np.linalg.norm(vectors, axis=1, keepdims=True)
    back = pmesh.octahedral_decode(pmesh.octahedral(vectors))
    assert np.max(np.linalg.norm(back - vectors, axis=1)) < 2e-4
    weights = pmesh.quantise_weights(rng.random((500, 4)))
    assert np.all(weights.astype(int).sum(axis=1) == 255)
