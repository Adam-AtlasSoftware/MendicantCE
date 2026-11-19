#!/usr/bin/env python3
"""The terrain overhaul's surface classes (T0; docs/prism_terrain.md): the curated table of the campaign's ground
shaders, written as port/assets/prism_terrain/classes.json (what the data pack ships: path hashes and our numbers,
no tag names, no asset) and as the C table the Vulkan host is built with
(port/linux/vulkan/xgpu_vulkan_terrain_classes.inc).

    python3 tools/prism_terrain_classes.py            # writes both
    python3 tools/prism_terrain_classes.py --check    # fails if either differs from what CURATED gives

A shader's class comes first from CE's material type (dirt, sand, stone, snow, ice: environment shaders only;
xgpu_vulkan_terrain.c's class_of_type); an entry here replaces it, keyed by SCENE_MATERIAL's path hash (the tag path's
64-bit FNV-1a, xgpu_scene_path_hash). The entries follow overhaul_research/prism_v4_renderer/terrain/inventory.md
sections 2 to 4: each shader's own two detail maps (CE's blended environment shaders mix them by the base map's alpha)
give the class pair, the first the primary detail's. layer: the HQ layer set T1 draws (by class unless named);
scale: one HQ tile's metres; porosity: Lagarde's, the class's unless given; drifts: sand or snow piles at the walls
there (T11)."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JSON = ROOT / "port/assets/prism_terrain/classes.json"
INCLUDE = ROOT / "port/linux/vulkan/xgpu_vulkan_terrain_classes.inc"

# xgpu_vulkan_terrain.h's XGPU_TERRAIN_CLASS_* (their order is their value)
CLASSES = ("none", "sand", "dirt", "mud", "rock", "snow", "ice", "water")
# (each class's default HQ tile in metres and porosity: design.md section 3.3)
SCALE = {"none": 0.0, "sand": 2.0, "dirt": 2.5, "mud": 2.0, "rock": 4.0, "snow": 3.0, "ice": 4.0, "water": 0.0}
POROSITY = {"none": 0.0, "sand": 0.6, "dirt": 0.7, "mud": 0.8, "rock": 0.3, "snow": 0.2, "ice": 0.0, "water": 0.0}

# tag path: (first class, second class, layer or None, drifts, the maps it is on, why)
CURATED = {
    # b30: the beach (sand, its secondary detail CE's grass), the sea bed (CE's type is water), the cliffs
    "levels\\b30\\shaders\\beach": ("sand", "dirt", "sand", True, "b30", "primary detail sand, secondary grass"),
    "levels\\b30\\shaders\\underwater": ("sand", "sand", "sand", False, "b30", "the sea bed, CE's type water"),
    "levels\\b30\\shaders\\cliffs": ("rock", "rock", None, False, "b30", "stone"),
    # b40 and c40 (the same BSPs): snow, snow over rock (secondary detail sandstone), ice, the cliff walls
    "levels\\b40\\shaders\\b40_cliff_snow": ("snow", "snow", None, True, "b40 c40", "snow"),
    "levels\\b40\\shaders\\b40_cliff_cmap": ("snow", "rock", None, True, "b40 c40", "snow detail, sandstone"),
    "levels\\b40\\shaders\\b40_cliff_cmapb": ("snow", "rock", None, True, "b40 c40", "snow detail, sandstone"),
    "levels\\b40\\shaders\\b40_cliff_ice": ("ice", "ice", None, False, "b40 c40", "ice"),
    "levels\\b40\\shaders\\b40_cliff_iceb": ("ice", "ice", None, False, "b40 c40", "ice"),
    "levels\\b40\\shaders\\b40_cliff_icefall": ("ice", "ice", None, False, "b40 c40", "ice"),
    "levels\\b40\\shaders\\b40_cliff_wall": ("rock", "rock", None, False, "b40 c40", "stone"),
    # c10: the swamp's ground (fungal moss over mud), its beds under the water
    "levels\\c10\\shaders\\swamp_a": ("dirt", "mud", None, False, "c10", "moss detail, mud detail"),
    "levels\\c10\\shaders\\swamp_b": ("dirt", "mud", None, False, "c10", "moss detail, mud detail"),
    "levels\\c10\\shaders\\swamp_a_outer": ("dirt", "mud", None, False, "c10", "moss detail, mud detail"),
    "levels\\c10\\shaders\\swamp_b_outer": ("dirt", "mud", None, False, "c10", "moss detail, mud detail"),
    "levels\\c10\\shaders\\swamp_a_underwater": ("mud", "mud", None, False, "c10", "the bed, CE's type water"),
    "levels\\c10\\shaders\\swamp_b_underwater": ("mud", "mud", None, False, "c10", "the bed, CE's type water"),
    # a30: the ground (CE's smooth rock detail and grass), the river beds (CE's type water), the cliffs
    "levels\\a30\\shaders\\cell01_ground": ("dirt", "dirt", None, False, "a30", "dirt"),
    "levels\\a30\\shaders\\cell01_ground_highland": ("dirt", "dirt", None, False, "a30", "dirt"),
    "levels\\a30\\shaders\\cell02_ground": ("dirt", "dirt", None, False, "a30", "dirt"),
    "levels\\a30\\shaders\\cell02_ground_highland": ("dirt", "dirt", None, False, "a30", "dirt"),
    "levels\\a30\\shaders\\cell02_riverbed": ("dirt", "dirt", None, False, "a30", "a bed, CE's type water"),
    "levels\\a30\\shaders\\riverbed": ("dirt", "dirt", None, False, "a30", "a bed, CE's type water"),
    "levels\\a30\\shaders\\cliff rock": ("rock", "rock", None, False, "a30", "stone"),
    "levels\\a30\\shaders\\cliff rock tiled": ("rock", "rock", None, False, "a30", "stone"),
    "levels\\a30\\shaders\\cliff rock fallen": ("rock", "rock", None, False, "a30", "stone"),
    # a50 and d20 (d20 re-uses a50's cliffs; two of d20's own cliffs are typed dirt)
    "levels\\a50\\shaders\\a50 cell01_ground": ("dirt", "dirt", None, False, "a50", "dirt"),
    "levels\\a50\\shaders\\a50 cell01_top": ("dirt", "dirt", None, False, "a50", "dirt"),
    "levels\\a50\\shaders\\a50 cliff_inner": ("rock", "rock", None, False, "a50 d20", "stone"),
    "levels\\a50\\shaders\\a50 cliff_innerb": ("rock", "rock", None, False, "a50", "stone"),
    "levels\\a50\\shaders\\a50 cliff_outer": ("rock", "rock", None, False, "a50", "stone"),
    "levels\\d20\\shaders\\a50 cliffs_innerb": ("rock", "rock", None, False, "d20", "cliffs typed dirt"),
    "levels\\d20\\shaders\\a50 cliffs_innerc": ("rock", "rock", None, False, "d20", "cliffs typed dirt"),
    "levels\\d20\\shaders\\d20_ground": ("dirt", "dirt", "dirt_cracked", False, "d20", "cracked mud detail"),
    "levels\\d20\\shaders\\d20_ground_outer": ("dirt", "dirt", "dirt_cracked", False, "d20", "cracked mud detail"),
    # d40: the sand terrain (dunes and its cliffs are one shader), its opaque water (an environment shader, which
    # the water layer draws over: T7), the Autumn's pieces (only the debris is rock; the engine's ceramics are not)
    "levels\\d40\\shaders\\d40_terrain": ("sand", "sand", "sand_desert", True, "d40", "sand"),
    "levels\\d40\\shaders\\d40_water": ("water", "water", None, False, "d40", "opaque environment water"),
    "levels\\a10\\shaders\\debris pile": ("rock", "rock", None, False, "d40", "stone"),
    "levels\\a10\\shaders\\heated ceramic": ("none", "none", None, False, "d40", "an engine, typed stone"),
    "levels\\a10\\shaders\\inner engine": ("none", "none", None, False, "d40", "an engine, typed stone"),
}


def fnv1a64(text):
    value = 0xcbf29ce484222325
    for byte in text.encode():
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def entries():
    out = []
    for path, (first, second, layer, drifts, maps, why) in CURATED.items():
        assert first in CLASSES and second in CLASSES, path
        out.append({"hash": f"{fnv1a64(path):016x}", "classes": [first, second], "layer": layer or first,
                    "scale": SCALE[first], "porosity": POROSITY[first], "drifts": drifts, "maps": maps.split(),
                    "why": why})
    out.sort(key=lambda e: e["hash"])
    return out


def json_text():
    data = {"format": 1,
            "about": "PRISM terrain classes (docs/prism_terrain.md): each campaign ground shader by its tag path's "
                     "FNV-1a 64 hash (SCENE_MATERIAL's path_hash), our classes and numbers. Generated by "
                     "tools/prism_terrain_classes.py.",
            "classes": list(CLASSES), "shaders": entries()}
    return json.dumps(data, indent=1) + "\n"


def layer_names():
    names = []
    for entry in entries():
        if entry["layer"] not in names:
            names.append(entry["layer"])
    return sorted(names)


def include_text():
    layers = layer_names()
    lines = ["/* Generated by tools/prism_terrain_classes.py from its curated table (port/assets/prism_terrain/",
             "classes.json). Do not edit. Each: the path hash's low and high words, the two classes",
             "(XGPU_TERRAIN_CLASS_*), the layer (its index in terrain_layer_names), flags (1: drifts), one HQ tile's",
             "metres, the porosity. */",
             "static const char *const terrain_layer_names[] =",
             "{"]
    lines += [f'\t"{name}",' for name in layers]
    lines += ["};", "static const struct terrain_curated terrain_curated[] =", "{"]
    for entry in entries():
        value = int(entry["hash"], 16)
        first, second = (CLASSES.index(c) for c in entry["classes"])
        lines.append(f"\t{{ {{ 0x{value & 0xffffffff:08x}u, 0x{value >> 32:08x}u }}, {{ {first}, {second} }}, "
                     f"{layers.index(entry['layer'])}, {1 if entry['drifts'] else 0}, {entry['scale']:.2f}f, "
                     f"{entry['porosity']:.2f}f }},")
    lines += ["};", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for path, text in ((JSON, json_text()), (INCLUDE, include_text())):
        if args.check:
            if not path.exists() or path.read_text() != text:
                raise SystemExit(f"{path.relative_to(ROOT)} differs; run tools/prism_terrain_classes.py")
        else:
            path.write_text(text)
    print(f"{len(CURATED)} shaders, {len(layer_names())} layers")


if __name__ == "__main__":
    main()
