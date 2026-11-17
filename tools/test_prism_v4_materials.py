"""PRISM v4's material pack (tools/prism_packs/art/prism_v4_materials.py; docs/prism_materials.md) on made-up
pictures, with no game data: its encoders and the gate's maths (its own check), and a made-up extract's environment
and character shaders built into a pack whose files are of format 2 and whose images the texture store would read
(port/linux/vulkan/xgpu_ktx2.c's checks: KTX2 of BC7, BC5 or BC4, sides powers of two, every level, each level's bytes
its blocks'). Needs numpy and Pillow; writes under build/test_prism_v4_materials."""
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "prism_packs" / "art" / "prism_v4_materials.py"
WORK = ROOT / "build" / "test_prism_v4_materials"
BLOCK_BYTES = {139: 8, 141: 16, 145: 16}


def run(*args):
    return subprocess.run([sys.executable, "-B", str(SCRIPT), *map(str, args)], capture_output=True, text=True,
                          check=True).stdout


def ktx2(path):
    """a KTX2 file's format, size and levels, checked as the texture store checks them (xgpu_ktx2.c)"""
    data = path.read_bytes()
    assert data[:12] == bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    vk, type_size, width, height, depth, layers, faces, levels, supercompression = struct.unpack_from("<9I", data, 12)
    assert vk in BLOCK_BYTES and type_size == 1 and not depth and not layers and faces == 1 and not supercompression
    assert width & (width - 1) == 0 and height & (height - 1) == 0
    assert levels == max(width, height).bit_length()
    for level in range(levels):
        offset, length, _ = struct.unpack_from("<QQQ", data, 80 + 24 * level)
        w, h = max(1, width >> level), max(1, height >> level)
        assert length == ((w + 3) // 4) * ((h + 3) // 4) * BLOCK_BYTES[vk]
        assert offset + length <= len(data)
    return vk, width, height


def picture(path, rgba):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(path)


def bitmap(extract, tag, rgba):
    """a made-up bitmap tag in an extract: its JSON and its PNG"""
    relative = tag.replace("\\", "/")
    picture(extract / "bitmaps" / (relative + ".png"), rgba)
    meta = {"tag": tag, "bitmaps": [{"index": 0, "width": rgba.shape[1], "height": rgba.shape[0],
                                     "png": ["bitmaps/" + relative + ".png"]}]}
    (extract / "bitmaps" / (relative + ".json")).write_text(json.dumps(meta))


def shader(extract, tag, fields):
    path = extract / "shaders" / (tag.replace("\\", "/") + ".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"tag": tag, **fields}))


@pytest.fixture(scope="module")
def pack():
    shutil.rmtree(WORK, ignore_errors=True)
    extract = WORK / "extract" / "test"
    rng = np.random.default_rng(3)
    y, x = np.mgrid[0:64, 0:64] / 64.0
    # (a panel: a height of ridges, its normals CE's bump map, its relief painted into the base map)
    height = np.sin(2 * np.pi * 4 * x) * 0.5 + np.cos(2 * np.pi * 2 * y) * 0.5
    dx = np.gradient(height, axis=1) * 8
    dy = np.gradient(height, axis=0) * 8
    n = np.stack([-dx, -dy, np.ones_like(dx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    bump = np.concatenate([np.rint((n * 0.5 + 0.5) * 255).astype(np.uint8), np.full((64, 64, 1), 255, np.uint8)], -1)
    shade = 0.6 + 0.2 * (height - height.mean())
    base = np.clip(np.stack([shade * 0.7, shade * 0.6, shade * 0.5], -1) + rng.normal(0, 0.02, (64, 64, 3)), 0, 1)
    base = np.concatenate([np.rint(base * 255).astype(np.uint8), np.full((64, 64, 1), 200, np.uint8)], -1)
    detail = np.full((32, 32, 4), 128, np.uint8)
    bitmap(extract, "levels\\test\\bitmaps\\panel", base)
    bitmap(extract, "levels\\test\\bitmaps\\panel bump", bump)
    bitmap(extract, "levels\\test\\bitmaps\\detail", detail)
    shader(extract, "levels\\test\\shaders\\panel", {
        "group": "senv", "material_type": 7, "flags": 0, "type": 0, "detail_function": 0,
        "micro_detail_function": 0, "specular_brightness": 1.0, "runtime_bump_scale": [1, 1],
        "texture_animation": [0, 1, 1, 0, 1, 1], "reflection_perpendicular": 0.3, "reflection_parallel": 0.2,
        "maps": {"base_map": "levels\\test\\bitmaps\\panel", "bump_map": "levels\\test\\bitmaps\\panel bump",
                 "micro_detail_map": "levels\\test\\bitmaps\\detail",
                 "reflection_cube_map": "levels\\test\\bitmaps\\cube"}})
    # (a moving one: left to the automatic tier)
    shader(extract, "levels\\test\\shaders\\conveyor", {
        "group": "senv", "material_type": 2, "type": 0, "texture_animation": [2, 1, 0.5, 0, 1, 1],
        "maps": {"base_map": "levels\\test\\bitmaps\\panel"}})
    # (a character's: no texture pack, CE's base map; a multipurpose map)
    skin = np.concatenate([np.rint(np.clip(base[..., :3] * 1.1, 0, 255)).astype(np.uint8),
                           np.full((64, 64, 1), 255, np.uint8)], -1)
    bitmap(extract, "characters\\test\\bitmaps\\body", skin)
    bitmap(extract, "characters\\test\\bitmaps\\body multi", np.full((64, 64, 4), 64, np.uint8))
    shader(extract, "characters\\test\\shaders\\body", {
        "group": "soso", "material_type": 0, "flags": 0, "translucency": 0.0,
        "maps": {"base_map": "characters\\test\\bitmaps\\body",
                 "multipurpose_map": "characters\\test\\bitmaps\\body multi"}})
    out = WORK / "mods" / "prism_v4_materials"
    run("build", "--extract", extract, "--out", out, "--workers", 1)
    return out


def test_check():
    assert "check passed" in run("check")


def test_environment_material(pack):
    material = json.loads((pack / "materials" / "levels" / "test" / "shaders" / "panel.pack.json").read_text())
    assert material["format"] == 2 and material["family"] == "senv"
    assert material["base"]["normal_uv"] == "bump"
    assert material["source"].startswith("pack:prism_v4_materials@")
    assert [layer["slot"] for layer in material["layers"]] == ["micro_detail"]
    formats = {}
    for key in ("albedo", "normal", "orm", "height"):
        formats[key] = ktx2(pack / material["base"][key])[0]
    assert formats == {"albedo": 145, "normal": 141, "orm": 145, "height": 139}
    assert ktx2(pack / material["layers"][0]["albedo"]) == (145, 32, 32)


def test_character_material(pack):
    material = json.loads((pack / "materials" / "characters" / "test" / "shaders" / "body.pack.json").read_text())
    assert material["format"] == 2 and material["family"] == "soso"
    assert ktx2(pack / material["base"]["albedo"]) == (145, 64, 64)
    assert "normal" not in material["base"]


def test_report(pack):
    report = json.loads((pack / "reports" / "test.json").read_text())
    assert report["environment_shaders"] == 2 and report["environment_covered"] == 1
    assert report["model_shaders"] == 1 and report["model_covered"] == 1
    assert "levels\\test\\shaders\\conveyor" in report["skipped"]
    panel = next(s for s in report["shaders"] if s["tag"] == "levels\\test\\shaders\\panel")
    # (the gate: the composite's low band within CIEDE2000 1 on average and 3 at the 95th percentile)
    assert panel["low_band_delta_e"][0] <= 1.0 and panel["low_band_delta_e"][1] <= 3.0
    assert panel["painted_r2"] > 0.1
