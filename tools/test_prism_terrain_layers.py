"""The terrain pack's tool (tools/prism_terrain_layers): a pack built from a made-up CC0-style archive is whole,
its detail's mean is 1 (CE's colour kept), a lit albedo is refused, an archive whose SHA-256 differs is refused,
and the manifest names CC0 sources only."""
import hashlib
import importlib.util
import io
import json
import struct
import zipfile
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/prism_terrain_layers/prism_terrain_layers.py"


def tool():
    spec = importlib.util.spec_from_file_location("prism_terrain_layers", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def png(array, mode):
    out = io.BytesIO()
    Image.fromarray(array.astype(np.uint8), mode).save(out, "JPEG", quality=95)
    return out.getvalue()


def archive(path, lit=False, size=256):
    rng = np.random.default_rng(3)
    yy, xx = np.mgrid[0:size, 0:size]
    height = (np.sin(xx / size * 2 * np.pi * 8) * np.cos(yy / size * 2 * np.pi * 5) * 0.5 + 0.5) * 255
    gx = np.gradient(height, axis=1) / 255 * 4
    gy = np.gradient(height, axis=0) / 255 * 4
    n = np.dstack([-gx, gy, np.ones_like(gx)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    normal = (n * 0.5 + 0.5) * 255
    grain = rng.normal(0, 18, (size, size))
    shade = (n[..., 0] * 0.7 + n[..., 2] * 0.7) * 120 if lit else 0
    albedo = np.clip(np.dstack([150 + grain + shade, 130 + grain + shade, 100 + grain + shade]), 0, 255)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("X_2K-JPG_Color.jpg", png(albedo, "RGB"))
        z.writestr("X_2K-JPG_NormalGL.jpg", png(normal, "RGB"))
        z.writestr("X_2K-JPG_Displacement.jpg", png(height, "L"))
        z.writestr("X_2K-JPG_Roughness.jpg", png(np.full((size, size), 200), "L"))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(work, digest, name="sand"):
    return {"format": 1, "macro_seed": 1, "layers": [{
        "name": name, "source": {"asset": "X", "author": "test", "page": "about:blank",
                                 "archive": "https://example.invalid/get?file=X_2K-JPG.zip", "sha256": digest,
                                 "licence": "CC0-1.0", "checked": "2026-11-19"},
        "tile_metres": 2.0, "strength": 1.0, "chroma": 0.2, "normal": 1.0, "roughness": 0.5,
        "height_contrast": 0.5, "lowpass": 0.125, "lit_limit": 0.6}]}


def build(module, tmp_path, lit=False, digest=None):
    work = tmp_path / "work"
    work.mkdir()
    real = archive(work / "X_2K-JPG.zip", lit=lit)
    (tmp_path / "manifest.json").write_text(json.dumps(manifest(work, digest or real)))
    module.MANIFEST = tmp_path / "manifest.json"
    data = tmp_path / "data"
    data.mkdir()
    args = type("A", (), {"data": str(data), "size": 256, "work": str(work), "yes": True})()
    module.build(args)
    return data / "prism_terrain"


def test_pack_whole_and_mean_kept(tmp_path):
    module = tool()
    out = build(module, tmp_path)
    blob = (out / "layers.ptl").read_bytes()
    assert blob[:4] == b"PTL1"
    version, size, layers, macro = struct.unpack("<4I", blob[4:20])
    assert (version, size, layers, macro) == (1, 256, 1, 512)
    assert len(blob) == 32 + 48 + 2 * 256 * 256 * 4 + 512 * 512 * 4
    name, tile = struct.unpack("<16sf", blob[32:52])
    assert name.rstrip(b"\0") == b"sand" and tile == 2.0
    detail = np.frombuffer(blob[80:80 + 256 * 256 * 4], np.uint8).reshape(256, 256, 4)
    # (half of the detail: its mean 1 is 127.5 within the bytes' rounding)
    assert np.allclose(detail[..., :3].reshape(-1, 3).mean(axis=0), 127.5, atol=1.5)
    assert detail[..., 3].min() == 0 and detail[..., 3].max() == 255
    assert "CC0" in (out / "LICENSES.txt").read_text()
    args = type("A", (), {"data": str(tmp_path / "data")})()
    module.check(args)


def test_lit_albedo_refused(tmp_path):
    with pytest.raises(SystemExit, match="not de-lit"):
        build(tool(), tmp_path, lit=True)


def test_digest_checked(tmp_path):
    with pytest.raises(SystemExit, match="SHA-256"):
        build(tool(), tmp_path, digest="0" * 64)


def test_manifest_cc0_only():
    m = json.loads((ROOT / "tools/prism_terrain_layers/manifest.json").read_text())
    names = [layer["name"] for layer in m["layers"]]
    assert names[:6] == ["sand", "dirt", "mud", "rock", "snow", "ice"]
    assert len(names) <= 8
    for layer in m["layers"]:
        assert layer["source"]["licence"] == "CC0-1.0"
        assert len(layer["source"]["sha256"]) == 64
        assert layer["source"]["archive"].startswith("https://ambientcg.com/")
    credits = (ROOT / "port/assets/prism_terrain/LICENSES.md").read_text()
    for layer in m["layers"]:
        assert layer["source"]["asset"] in credits
