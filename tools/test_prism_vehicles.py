"""PRISM v4's vehicles pack (tools/prism_packs/art/prism_cast/vehicle_tread.py, vehicle_material.py;
docs/prism_packs.md) on a made-up wheel laid out as CE lays the Warthog's, with no game data: the tread's map
fitted to CE's own tread faces (not to its vertex normals, which CE smooths across the tread's edge: the Warthog's
tyres were fitted to the hub disc's and the motion blur's part of the map and smeared into streaks where they stood
still), the side discs' maps, the painted relief, and the tyre material's normal and ORM maps and file. Needs numpy
and Pillow; writes only under pytest's own folders."""
import json
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "tools" / "prism_packs" / "art"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ART))
sys.path.insert(0, str(ART / "prism_cast"))
import vehicle_material as vm  # noqa: E402
import vehicle_tread as vt  # noqa: E402

# CE's Warthog tyre map's layout: the tread strip at u 0.745 to 0.995 (as CE's uvs say it, 1.745 to 1.995),
# twice round a turn, v falling as the wheel turns; the sharp side disc about (0.235, 0.76)
STRIP = (1.745, 1.995)
REPEATS = -2
DISC = (1.235, 1.76, 0.23)


def ce_wheel(segments=16, radius=0.2, half=0.1, tread=0.08, hub=0.12, dish=0.06):
    """a wheel as CE builds the Warthog's: a 16-sided tread band (its own vertices, uvs on the tread strip), two
    side discs (their own vertices, uvs the disc's projection) with a dished hub, and each vertex's normal smoothed
    across the tread's edge as CE's are. Returns positions, normals, uvs and triangles (clockwise about the
    outward normal)."""
    P, N, UV, T = [], [], [], []
    th = np.arange(segments) * 2 * np.pi / segments
    axle = np.array([0.0, 1.0, 0.0])

    def ring(r, y):
        return np.stack([np.cos(th) * r, np.full(segments, y), np.sin(th) * r], 1)

    def add(points, normals, uvs):
        base = len(P)
        P.extend(points)
        N.extend(normals)
        UV.extend(uvs)
        return base
    # the tread: two rings at +-tread, normals leaning 0.55 along the axle (CE's smoothing)
    radial = np.stack([np.cos(th), np.zeros(segments), np.sin(th)], 1)
    for y in (-tread, tread):
        lean = vt.unit(radial + np.sign(y) * 0.66 * axle)
        v = 1.0 + REPEATS * (-th / (2 * np.pi))
        u = np.full(segments, STRIP[0] if y < 0 else STRIP[1])
        add(ring(radius, y), lean, np.stack([u, v], 1))
    for s in range(segments):
        s2 = (s + 1) % segments
        a, b, c, d = s, s2, segments + s, segments + s2
        T += [[a, c, b], [b, c, d]]
    # the side discs: a rim ring at the radius, the hub ring set in (the dish) and the centre
    for side in (-1.0, 1.0):
        rim = ring(radius, side * half)
        hub_ring = ring(hub, side * (half - dish))
        centre = np.array([[0.0, side * (half - dish), 0.0]])
        pts = np.concatenate([rim, hub_ring, centre])
        nrm = np.concatenate([vt.unit(side * axle + 0.66 * radial), vt.unit(radial * 0.9 + side * 0.4 * axle),
                              [side * axle]])
        uv = np.stack([DISC[0] + pts[:, 0] / radius * DISC[2], DISC[1] + pts[:, 2] / radius * DISC[2]], 1)
        base = add(pts, nrm, uv)
        for s in range(segments):
            s2 = (s + 1) % segments
            a, b, c, d = base + s, base + s2, base + segments + s, base + segments + s2
            e = base + 2 * segments
            T += [[a, b, c] if side > 0 else [a, c, b], [b, d, c] if side > 0 else [b, c, d],
                  [c, d, e] if side > 0 else [c, e, d]]
    return np.array(P), vt.unit(np.array(N)), np.array(UV), np.array(T)


def frame(P, T):
    axle = vt.wheel_axle(P, T)
    e1 = vt.unit(np.cross(axle, [0.0, 0.0, 1.0] if abs(axle[2]) < 0.9 else [1.0, 0.0, 0.0]))
    e2 = np.cross(axle, e1)
    c = (P.max(0) + P.min(0)) * 0.5
    half = float(np.abs((P - c) @ axle).max())
    return axle, e1, e2, c, half


def test_the_axle_is_the_side_faces_normal():
    P, N, UV, T = ce_wheel()
    axle, *_ = frame(P, T)
    assert abs(abs(axle[1]) - 1.0) < 1e-6


def test_the_tread_is_fitted_to_ces_tread_faces():
    P, N, UV, T = ce_wheel()
    axle, e1, e2, c, half = frame(P, T)
    faces = vt.tread_faces(P, T, c, axle)
    assert faces.sum() == 32
    corners = np.unique(T[faces])
    fit = vt.fit_tread_map(P[corners] - c, UV[corners], axle, e1, e2, half)
    assert (fit["u_lo"], fit["u_hi"]) == pytest.approx(STRIP)
    assert abs(fit["k"]) == abs(REPEATS)
    assert fit["rms"] < 1e-3
    # CE's own tread corners land on their own uvs again (v a whole repeat apart at most)
    uv = vt.tread_uv(fit, P[corners] - c, axle, e1, e2)
    assert np.abs(uv[:, 0] - UV[corners, 0]).max() < 1e-6
    dv = uv[:, 1] - UV[corners, 1]
    assert np.abs(dv - np.round(dv)).max() < 1e-3


def test_the_vertex_normals_would_have_taken_the_hub_for_the_tread():
    """the old choice (|normal . axle| < 0.5) on CE's smoothed normals: the tread's corners lean out of it and the
    dished hub's inner corners fall in, so the strip's u came from the disc (the map's left half)"""
    P, N, UV, T = ce_wheel()
    axle, *_ = frame(P, T)
    old = np.abs(N @ axle) < 0.5
    assert not (UV[old, 0] >= STRIP[0] - 1e-6).all()
    assert UV[old, 0].min() < STRIP[0] - 0.2


def test_the_side_discs_are_fitted_exactly():
    P, N, UV, T = ce_wheel()
    axle, e1, e2, c, half = frame(P, T)
    corners = np.unique(T[vt.side_faces(P, T, axle)])
    Q = P[corners] - c
    for s in (-1.0, 1.0):
        one = np.sign(Q @ axle) == s
        X = np.stack([Q[one] @ e1, Q[one] @ e2, np.ones(one.sum())], 1)
        A = vt.fit_disc_map(X, UV[corners][one])
        assert np.abs(X @ A - UV[corners][one]).max() < 1e-9
        assert A[2] == pytest.approx(DISC[:2])
        assert np.sqrt(abs(np.linalg.det(A[:2]))) * 0.2 == pytest.approx(DISC[2])


def blocks_picture(size=64, period=16, groove=5):
    """bright blocks between dark grooves, rows top first"""
    y, x = np.mgrid[0:size, 0:size]
    lum = np.where((y % period) < groove, 10.0, 120.0)
    lum = np.where((x % 32) < 3, 10.0, lum)
    return lum


def test_the_relief_stands_the_blocks_up_and_cuts_the_grooves_in():
    lum = blocks_picture()
    h = vt.relief_height(lum, local_px=8)
    assert h.shape == lum.shape and 0.0 <= h.min() and h.max() <= 1.0
    assert h[8, 16] > 0.8 and h[2, 16] < 0.2
    # (wrapped: the map repeats round the tyre)
    assert vt.relief_height(np.roll(lum, 7, 0))[15, 16] == pytest.approx(h[8, 16])


def test_the_tyre_maps():
    lum = blocks_picture(size=64)
    spec = {"size": 128, "local_px": 8, "relief_slope": 2.5, "hub": {"radius": 0.6, "roughness": 0.55, "relief": 0.3},
            "rubber": {"roughness": 0.86, "block_top": -0.08, "groove": 0.08}}
    discs = [((1.25, 1.25), 0.2)]
    normal, orm, height, hub = vm.maps(lum, spec, discs, 64)
    assert normal.shape == (128, 128, 4) and orm.shape == (128, 128, 4)
    n = normal[..., :2] * 2 - 1
    assert (np.abs(n) <= 1.0 + 1e-6).all()
    # a block's top is flat, its edge leans; along the rows (y up the picture: a groove above a block, at the
    # block's top edge, its normal leans up)
    assert np.abs(n[20, 32]).max() < 0.05
    assert n[9:12, 32, 1].max() > 0.3 and np.abs(n[9:12, 32, 0]).max() < 0.05
    rough = orm[..., 1]
    assert 0.77 <= rough[hub < 0.01].min() and rough[hub < 0.01].max() <= 0.95
    assert rough[hub > 0.99] == pytest.approx(0.55)
    assert (orm[..., 2] == 0).all()
    # the hub's relief is shallower than the rubber's
    gx = np.abs(np.diff(normal[..., 0], axis=1))
    assert gx[hub[:, 1:] > 0.99].mean() < gx[hub[:, 1:] < 0.01].mean()


def test_the_material_file_and_its_images(tmp_path):
    """the whole step on a made-up extract: format 1, its images KTX2 BC7 with every level, as mod_pbr.c reads them"""
    extract = tmp_path / "extract"
    png = extract / "bitmaps" / "tyre.png"
    png.parent.mkdir(parents=True)
    lum = blocks_picture(size=64).astype(np.uint8)
    Image.fromarray(np.stack([lum] * 3, -1)).save(png)
    recipe = {"maps_root": str(extract), "maps": {"tyre": {"png": "bitmaps/tyre.png", "local_px": 8}},
              "model": "vehicles/warthog",
              "material": {"shader": "vehicles\\warthog\\shaders\\warthog tires", "map": "tyre", "tyre_shader": "0",
                           "size": 128, "relief_slope": 2.5, "normal_scale": 1.0, "specular": 0.5,
                           "hub": {"radius": 0.6, "roughness": 0.55, "relief": 0.35},
                           "rubber": {"roughness": 0.86, "block_top": -0.08, "groove": 0.08}}}
    report = {"tyres": {"0": [{"node": 14, "discs": [{"side": -1.0, "centre": [1.25, 1.75], "radius_uv": 0.2}]}]}}
    (tmp_path / "recipe.json").write_text(json.dumps(recipe))
    (tmp_path / "report.json").write_text(json.dumps(report))
    mod = tmp_path / "mod"
    subprocess.run([sys.executable, "-B", str(ART / "prism_cast" / "vehicle_material.py"), tmp_path / "recipe.json",
                    tmp_path / "report.json", mod], check=True, capture_output=True, text=True)
    material = json.loads((mod / "materials" / "vehicles" / "warthog" / "shaders" /
                           "warthog tires.material.json").read_text())
    assert material["format"] == 1 and "albedo" not in material
    for key in ("normal", "orm"):
        data = (mod / material[key]).read_bytes()
        assert data[:12] == b"\xabKTX 20\xbb\r\n\x1a\n"
        vk, _, width, height = struct.unpack_from("<IIII", data, 12)
        levels = struct.unpack_from("<I", data, 40)[0]
        assert vk == 145 and (width, height) == (128, 128) and levels == 8


def test_no_disc_no_material(tmp_path):
    recipe = {"maps_root": str(tmp_path), "maps": {"tyre": {"png": "t.png"}},
              "material": {"shader": "x", "map": "tyre", "tyre_shader": "0"}}
    Image.fromarray(np.zeros((8, 8, 3), np.uint8)).save(tmp_path / "t.png")
    (tmp_path / "recipe.json").write_text(json.dumps(recipe))
    (tmp_path / "report.json").write_text(json.dumps({"tyres": {"0": [{"node": 1}]}}))
    result = subprocess.run([sys.executable, "-B", str(ART / "prism_cast" / "vehicle_material.py"),
                             tmp_path / "recipe.json", tmp_path / "report.json", tmp_path / "mod"],
                            capture_output=True, text=True)
    assert result.returncode != 0 and "no side disc" in result.stderr
