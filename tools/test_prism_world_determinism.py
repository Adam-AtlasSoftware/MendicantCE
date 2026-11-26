"""fork: the remodel builds to the same file on every machine (a shipped light bake of the remodel is valid only where
the player's own build is the farm's): the written positions and coordinates snapped to fixed grids, the .pwg digests
and comparison (wg_pwg_hash.py), the metal relief's numpy steps the same whatever the hash seed, BLAS threads and
numpy kernels, and prism_packs' world step running every step of the build in Blender's Python with that environment.
Without Blender or the game's data (wg_determinism.py runs the real build)."""

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
WORLD = ROOT / "tools" / "prism_packs" / "art" / "world_geo"
sys.path.insert(0, str(WORLD))
sys.path.insert(0, str(ROOT / "tools" / "prism_packs"))

import wg_common as wg  # noqa: E402
import wg_pwg_hash  # noqa: E402


def test_a_few_ulps_off_snap_to_the_same_float():
    rng = np.random.default_rng(7)
    # positions across a level's range (within 512 wu of the origin two float32 ulps are a quarter of the grid),
    # kept off the grid's midpoints so that the test itself is not a coin toss
    x = (np.floor(rng.uniform(-500, 500, 20000) / wg.POSITION_GRID) + rng.uniform(-0.15, 0.15, 20000)) * \
        wg.POSITION_GRID
    a = x.astype(np.float32)
    b = np.nextafter(np.nextafter(a, np.float32(np.inf)), np.float32(np.inf))
    assert (wg.snap(a, wg.POSITION_GRID) == wg.snap(b, wg.POSITION_GRID)).all()
    s = wg.snap(a, wg.POSITION_GRID)
    # on the grid exactly, and snapping again changes nothing
    assert (np.asarray(s, np.float64) / wg.POSITION_GRID == np.round(np.asarray(s, np.float64) / wg.POSITION_GRID)).all()
    assert (wg.snap(s, wg.POSITION_GRID) == s).all()
    assert np.abs(s.astype(np.float64) - x).max() <= wg.POSITION_GRID


def test_snapped_spheres_still_bound():
    rng = np.random.default_rng(3)
    pts = rng.uniform(-50, 50, (200, 3)).astype(np.float32)
    cen = (pts.min(0) + pts.max(0)) * 0.5
    r = float(np.linalg.norm(pts - cen, axis=1).max())
    b = wg.snap_bounds([[cen[0], cen[1], cen[2], r]])[0]
    assert np.linalg.norm(pts.astype(np.float64) - b[:3], axis=1).max() <= b[3]


def stock(n=6):
    tri = np.array([[0, 1, 2], [0, 2, 3], [4, 5, 0]], np.int64)
    p = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [-1, 0, 0], [-1, -1, 0]], np.float32)
    z = np.zeros((n, 3), np.float32)
    uv = p[:, :2].copy()
    return wg.Material(name="m", shader="s", shader_group="senv", maps={}, lightmap_index=0, material_index=1,
                       has_lightmap=True, position=p, normal=z + [0, 0, 1], tangent=z + [1, 0, 0],
                       binormal=z + [0, 1, 0], uv0=uv, uv1=uv * 0.5, radiosity=z, triangles=tri)


def write(tmp_path, name, position, radiosity=None):
    m = stock()
    r = wg.Remodel(m, position, m.normal, m.binormal, m.tangent, m.uv0 + 0.123456, m.uv1,
                   m.radiosity if radiosity is None else radiosity, m.triangles, np.arange(3))
    path = str(tmp_path / name)
    wg.write_pwg(path, "levels\\x\\x", [r])
    return path


def test_a_build_a_few_ulps_off_writes_the_same_file(tmp_path):
    m = stock()
    p = m.position * np.float32(37.1) + np.float32(301.7)
    q = np.nextafter(p, np.float32(np.inf))
    a, b = write(tmp_path, "a.pwg", p), write(tmp_path, "b.pwg", q)
    assert open(a, "rb").read() == open(b, "rb").read()
    assert wg_pwg_hash.compare(a, b)[0] == 0
    d = wg_pwg_hash.digests(a)
    assert d["parts"] == 1 and d["bytes"] == os.path.getsize(a)


def test_the_comparison_tells_drawn_fields_from_geometry(tmp_path):
    m = stock()
    p = m.position * np.float32(37.1) + np.float32(301.7)
    a = write(tmp_path, "a.pwg", p)
    lit = write(tmp_path, "lit.pwg", p, radiosity=np.full((6, 3), 0.5, np.float32))
    moved = write(tmp_path, "moved.pwg", p + np.float32(wg.POSITION_GRID * 4))
    code, what = wg_pwg_hash.compare(a, lit)
    assert code == 1 and "radiosity" in what
    assert wg_pwg_hash.digests(a)["geometry"] == wg_pwg_hash.digests(lit)["geometry"]
    code, what = wg_pwg_hash.compare(a, moved)
    assert code == 2 and "positions" in what


RELIEF_RUN = r"""
import hashlib, sys
import numpy as np
sys.path.insert(0, sys.argv[1])
import wg2_relief as relief
rng = np.random.default_rng(11)
n = 128
H = np.zeros((n, n))
H[:, 40:44] = -2.0
H[70:80, 20:90] = -1.5
H += rng.normal(0, 0.05, H.shape)
gx = (np.roll(H, -1, 1) - np.roll(H, 1, 1)) / 2.0
gy = (np.roll(H, -1, 0) - np.roll(H, 1, 0)) / 2.0
nn = np.stack([-gx, -gy, np.ones_like(H)], -1)
nn /= np.linalg.norm(nn, axis=-1, keepdims=True)
px = np.concatenate([nn * 0.5 + 0.5, np.ones(H.shape + (1,))], -1)
back = relief.height_from_bump(px, drift_px=12)
h = hashlib.sha256(back.tobytes())
for level in (-0.4, -1.0):
    for P, closed in relief.contours(back, level):
        Q = relief.simplify(P, 0.45, closed)
        h.update(np.ascontiguousarray(Q).tobytes())
        h.update(bytes([closed]))
print(h.hexdigest())
"""


def relief_digest(env):
    full = dict(os.environ)
    full.update(env)
    r = subprocess.run([sys.executable, "-c", RELIEF_RUN, str(WORLD)], env=full, capture_output=True, text=True,
                       check=True)
    return r.stdout.strip()


def test_the_relief_comes_out_the_same_whatever_the_hash_seed_and_threads():
    import prism_packs
    pinned = prism_packs.deterministic_env(False)
    base = relief_digest(dict(pinned, PYTHONHASHSEED="0"))
    assert relief_digest(dict(pinned, PYTHONHASHSEED="12345", OMP_NUM_THREADS="8", OPENBLAS_NUM_THREADS="8")) == base
    assert relief_digest(dict(pinned, PYTHONHASHSEED="99")) == base


def test_the_world_step_builds_in_blenders_python_with_the_pinned_environment():
    import prism_packs
    env = prism_packs.deterministic_env()
    assert env["OMP_NUM_THREADS"] == env["OPENBLAS_NUM_THREADS"] == "1"
    # (Blender 4.5's numpy 1.26: every feature past its SSE3 baseline that it may dispatch to)
    assert {"AVX2", "AVX512F", "AVX512_SKX", "SSE41"} <= set(env["NPY_DISABLE_CPU_FEATURES"].split())
    src = (ROOT / "tools" / "prism_packs" / "prism_packs.py").read_text()
    world = src[src.index("def step_world"):src.index("def step_lightmaps")]
    for script in ("wg2_remodel.py", "wg2_seal.py", "wg2_far2.py"):
        call = world[world.index(script) - 40:world.index(script)]
        assert "blender_run" in call, script
    assert world.count("extra=det") >= 2 and "**det" in world


def test_the_same_triangles_in_another_order_write_the_same_file(tmp_path):
    # a surface of many small triangles in one Morton cell: the written order must not follow the order they came in
    rng = np.random.default_rng(5)
    n = 40
    p = np.concatenate([rng.uniform(0, 0.04, (n * 3, 2)), np.zeros((n * 3, 1))], 1).astype(np.float32) + 100.0
    tri = np.arange(n * 3).reshape(n, 3)
    m = wg.Material(name="m", shader="s", shader_group="senv", maps={}, lightmap_index=0, material_index=1,
                    has_lightmap=True, position=p, normal=np.tile([0, 0, 1], (n * 3, 1)).astype(np.float32),
                    tangent=np.tile([1, 0, 0], (n * 3, 1)).astype(np.float32),
                    binormal=np.tile([0, 1, 0], (n * 3, 1)).astype(np.float32), uv0=p[:, :2] * 0.1,
                    uv1=p[:, :2] * 0.01, radiosity=np.zeros((n * 3, 3), np.float32), triangles=tri[:1])
    paths = []
    for name, perm in (("a.pwg", np.arange(n)), ("b.pwg", rng.permutation(n))):
        r = wg.Remodel(m, p, m.normal, m.binormal, m.tangent, m.uv0, m.uv1, m.radiosity, tri[perm], np.zeros(n, int))
        paths.append(str(tmp_path / name))
        wg.write_pwg(paths[-1], "levels\\x\\x", [r])
    assert open(paths[0], "rb").read() == open(paths[1], "rb").read()
