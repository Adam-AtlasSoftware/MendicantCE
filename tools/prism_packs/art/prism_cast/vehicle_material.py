#!/usr/bin/env python3
"""PRISM v4: a vehicle's tyres as a PBR material (display.prism_materials "pbr", port/linux/game/mod_pbr.c), made
from CE's own tyre map on the player's machine.

    python3 vehicle_material.py <recipe.json> <report.json> <mod folder> [--sheet <png>]

The recipe's "material": the tyre shader's tag path, its map (one of the recipe's "maps"), and the rubber's and
hub's numbers; the report is vehicle_remodel.py's (each tyre's tread strip and side discs as fitted to CE's faces).
Writes <mod>/textures/materials/<model>/tyre_normal.ktx2 and tyre_orm.ktx2 (BC7, whole chains) and
<mod>/materials/<shader>.material.json. The colour stays CE's (no albedo image): the material gives the rubber its
relief and its sheen.

- **The relief** is CE's painted relief (vehicle_tread.relief_height: what is lighter than its neighbourhood stands,
  what is darker is cut in), the same field vehicle_remodel.py raises the round tyre's blocks from, at the material's
  size (bicubic from CE's), its slopes the normal (RG, y up the picture, 0.5 flat): the blocks' crisp edges where the
  mesh carries only their band-limited shape, and CE's own tyre (Remodels off) its tread too. Within each side disc's
  hub (hub.radius of the disc's radius about its centre, as the report fits them) the relief is hub.relief of it:
  the painted hub's bolts and rim stay shallow.
- **ORM**: rubber roughness rubber.roughness, smoother on the blocks' tops (worn: block_top), rougher in the grooves
  (groove), the occlusion the grooves' depth; the hub's painted metal hub.roughness; no metal (paint and rubber are
  dielectrics).
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import packs_env as E  # noqa: E402

sys.path.insert(0, E.TOOLS)
sys.path.insert(0, str(HERE))
import vehicle_tread as vt  # noqa: E402


def upsample(field, size):
    h, w = field.shape
    if (w, h) == (size, size):
        return field.astype(np.float32)
    # (wrapped: three tiles a side, the middle one kept, so the edges blend as the map repeats)
    tiled = np.tile(field.astype(np.float32), (3, 3))
    big = np.asarray(Image.fromarray(tiled, "F").resize((size * 3, size * 3), Image.BICUBIC), np.float32)
    return np.clip(big[size:2 * size, size:2 * size], 0.0, 1.0)


def hub_mask(size, discs, radius):
    """1 inside each disc's hub (radius of its radius about its centre), softened over two percent of it"""
    y, x = (np.mgrid[0:size, 0:size].astype(np.float32) + 0.5) / size
    m = np.zeros((size, size), np.float32)
    for centre, r in discs:
        cu, cv = centre[0] % 1.0, centre[1] % 1.0
        dx = (x - cu + 0.5) % 1.0 - 0.5
        dy = (y - cv + 0.5) % 1.0 - 0.5
        d = np.sqrt(dx * dx + dy * dy) / max(r * radius, 1e-6)
        m = np.maximum(m, np.clip((1.02 - d) / 0.04, 0.0, 1.0))
    return m


def discs_of(report, shader):
    """the side discs the report fitted (each once: every wheel lays CE's two discs alike)"""
    out = []
    for wheel in report.get("tyres", {}).get(str(shader), []):
        for d in wheel.get("discs", []):
            key = (round(d["centre"][0] % 1.0, 3), round(d["centre"][1] % 1.0, 3))
            if all((round(c[0] % 1.0, 3), round(c[1] % 1.0, 3)) != key for c, _ in out):
                out.append((d["centre"], d["radius_uv"]))
    return out


def maps(lum, spec, discs, source_size):
    """(normal RGBA, ORM RGBA, height, hub) at spec["size"] from CE's luminance (rows top first)"""
    size = int(spec.get("size", 1024))
    height = upsample(vt.relief_height(lum, spec.get("local_px", 8)), size)
    hub = hub_mask(size, discs, spec["hub"]["radius"])
    amount = 1.0 - hub * (1.0 - spec["hub"]["relief"])
    rel = 0.5 + (height - 0.5) * amount
    # (the slopes per texel of CE's map: the same edge whatever the material's size)
    k = spec.get("relief_slope", 2.5) * size / source_size * 0.5
    gx = (np.roll(rel, -1, 1) - np.roll(rel, 1, 1)) * k
    gy = (np.roll(rel, -1, 0) - np.roll(rel, 1, 0)) * k
    # (x along u; y up the picture, against the rows)
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    normal = np.concatenate([0.5 + 0.5 * n[..., :2], np.ones(n.shape[:2] + (2,), np.float32)], -1)
    rub = spec["rubber"]
    rough = rub["roughness"] + rub["block_top"] * height + rub["groove"] * (1.0 - height)
    rough = rough * (1.0 - hub) + spec["hub"]["roughness"] * hub
    occlusion = 1.0 - 0.35 * (1.0 - rel) * (1.0 - hub)
    orm = np.stack([occlusion, np.clip(rough, 0.05, 1.0), np.zeros_like(rough), np.ones_like(rough)], -1)
    return normal, orm, height, hub


def to_picture(x):
    return Image.fromarray((np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "RGBA")


def material_json(shader, model, spec):
    folder = "textures/materials/" + model
    return {"format": 1, "normal": folder + "/tyre_normal.ktx2", "normal_scale": spec.get("normal_scale", 1.0),
            "orm": folder + "/tyre_orm.ktx2", "roughness": 1.0, "metal": 1.0, "occlusion": 1.0,
            "reflection": 1.0, "specular": spec.get("specular", 0.5), "wrap": False, "direct_specular": 1.0,
            "ce_reflection": 0.0,
            "multipurpose": {"specular_mask": True, "self_illumination": True, "reflection_tint": False}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recipe", type=Path)
    ap.add_argument("report", type=Path)
    ap.add_argument("mod", type=Path)
    ap.add_argument("--sheet", type=Path, help="a picture of CE's map, the relief, the normal and the ORM")
    a = ap.parse_args()
    recipe = json.loads(a.recipe.read_text())
    report = json.loads(a.report.read_text())
    spec = recipe["material"]
    m = recipe["maps"][spec["map"]]
    spec = dict(spec, local_px=m.get("local_px", 8))
    rgb = np.asarray(Image.open(Path(recipe["maps_root"]) / m["png"]).convert("RGB"), np.float64)
    lum = vt.luminance(rgb)
    discs = discs_of(report, spec["tyre_shader"])
    if not discs:
        raise SystemExit("%s: no side disc fitted for shader %s" % (a.report, spec["tyre_shader"]))
    normal, orm, height, hub = maps(lum, spec, discs, lum.shape[0])
    import prism_material_pack as pack  # (the BC7 encoder and the KTX2 writer)
    model = recipe.get("model", "vehicles/warthog")
    out = a.mod / "textures" / "materials" / model
    out.mkdir(parents=True, exist_ok=True)
    pack.write_chain(to_picture(normal), out / "tyre_normal.ktx2", 0, None)
    pack.write_chain(to_picture(orm), out / "tyre_orm.ktx2", 0, None)
    path = a.mod / "materials" / (spec["shader"].replace("\\", "/") + ".material.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(material_json(spec["shader"], model, spec), indent=2) + "\n")
    print(path, "discs", len(discs), flush=True)
    if a.sheet:
        size = normal.shape[0]
        ce = np.asarray(Image.fromarray(rgb.astype(np.uint8)).resize((size, size), Image.NEAREST), np.float32) / 255
        tiles = [ce, np.repeat(height[..., None], 3, -1), normal[..., :3], orm[..., :3]]
        sheet = np.concatenate([np.concatenate(tiles[:2], 1), np.concatenate(tiles[2:], 1)], 0)
        a.sheet.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray((np.clip(sheet, 0, 1) * 255 + 0.5).astype(np.uint8)).save(a.sheet)


if __name__ == "__main__":
    main()
