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
        "height_contrast": 0.5, "grain": 0.8, "lowpass": 0.125, "lit_limit": 0.6}]}


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
    # (the grain's weight in the first spare float, the renderer's values[6])
    values = struct.unpack("<8f", blob[48:80])
    assert abs(values[6] - 0.8) < 1e-6 and values[7] == 0.0
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
    # (the named layers the class table asks for: d40's desert sand, d20's cracked mud)
    assert {"sand_desert", "dirt_cracked"} <= set(names)
    classes = (ROOT / "port/linux/vulkan/xgpu_vulkan_terrain_classes.inc").read_text()
    for name in names:
        assert f'"{name}"' in classes or name in ("sand", "dirt", "mud", "rock", "snow", "ice")
    for layer in m["layers"]:
        assert 0.0 <= layer.get("grain", 0.0) <= 1.0
    for layer in m["layers"]:
        assert layer["source"]["licence"] == "CC0-1.0"
        assert len(layer["source"]["sha256"]) == 64
        assert layer["source"]["archive"].startswith("https://ambientcg.com/")
    credits = (ROOT / "port/assets/prism_terrain/LICENSES.md").read_text()
    for layer in m["layers"]:
        assert layer["source"]["asset"] in credits


def test_detail_fade_waits_for_the_renderers_pack():
    """CE's detail maps fade under the HQ layers only once the renderer says it loaded the pack (a folder named
    by display.terrain_pack_folder is no longer taken as there): host_vulkan_terrain_pack, a host import"""
    assert "host_vulkan_terrain_pack" in (ROOT / "port/linux/vulkan/host_imports.list").read_text().split()
    terrain = (ROOT / "port/linux/game/prism_terrain.c").read_text()
    level = terrain.split("long prism_terrain_detail_level(", 1)[1].split("\n}\n", 1)[0]
    assert "prism_terrain.pack_state != 1" in level
    assert "fopen" not in terrain
    platform = (ROOT / "port/linux/src/sdl_platform.c").read_text()
    assert "return host_vulkan_terrain_pack(folder);" in platform
    renderer = (ROOT / "port/linux/vulkan/xgpu_vulkan_terrain.c").read_text()
    loader = renderer.split("static int pack_load(void)", 1)[1].split("\nint xgpu_vulkan_terrain_pack_state", 1)[0]
    # (every way out of the loader says what came of it: loaded, or not)
    assert loader.count("__atomic_store_n(&terrain.pack.state, -1") == 3
    assert loader.count("__atomic_store_n(&terrain.pack.state, 1") == 1
    # (loaded is told only while the layers are drawn: the last whole frame's)
    state = renderer.split("\nint xgpu_vulkan_terrain_pack_state", 1)[1].split("\n}\n", 1)[0]
    assert "state == 1 && !__atomic_load_n(&terrain.drawn_shown" in state
    assert "terrain.drawn_frame |= terrain.layers_drawn;" in renderer
    end = renderer.split("void xgpu_vulkan_terrain_frame_end(void)", 1)[1].split("\n}\n", 1)[0]
    assert "__atomic_store_n(&terrain.drawn_shown, terrain.drawn_frame" in end


def test_named_layer_serves_both_slots_of_one_class():
    """A shader whose two classes are one (d40's sand and sand) draws its named layer in both slots: the second
    slot once fell back to the class's own layer (b30's beach sand, grain and all) over d40's paler texels"""
    renderer = (ROOT / "port/linux/vulkan/xgpu_vulkan_terrain.c").read_text()
    body = renderer.split("static unsigned pack_layer(", 1)[1].split("\n}\n", 1)[0]
    assert "(which == 0 || material->classes[1] == material->classes[0])" in body
