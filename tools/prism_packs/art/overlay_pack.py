#!/usr/bin/env python3
"""Build the PRISM material overlay mod (Tier 0: our tiles and assignments only, no game data).

  python3 -I art/overlay_pack.py --tiles <work>/materials/tiles --uv <work>/materials/uv_density_b30.json \
      --out <data>/mods/prism_materials [--tuning art/overlay_tuning.json]

Reads microtex.py's library.json (classes, tile_m, fade_m), measure_uv_density.py's JSON (uv_wu per
shader, the first-person model's where a shader is shared with the third-person one) and the
assignments below, and writes
  <out>/mod.toml
  <out>/tags/materials/overlay.json      (format 1, read by opence port/linux/game/mod_materials.c)
  <out>/tags/materials/tiles/<class>.png (packed_v1 RGBA8 tiles, copied: level 0, the fallback)
  <out>/tags/materials/tiles/<class>_n.ktx2, <class>_cr.ktx2 (BC5, every mip level as microtex.py
                                         filtered them, Toksvig roughness on the smaller ones: "levels")
  <out>/build.json                       (what it was built from: the inputs' hashes, the tuning)
World units: 1 WU = 10 ft = 3.048 m (map_extract's glTF units; measure_uv_density.py checks it
against the Chief's height).

Class masks (v1, channel rules; Xbox channel order r specular/reflection mask, g self-illumination,
b change colour), checked on the extracted maps with material_masks.py:
  AR gun:        r > 0.55 bare machined metal (top rail, worn edges), r < 0.25 painted body
  Pistol silver: r > 0.35 the bright slide and sights, r < 0.15 the dark frame parts
  Plasma weapons: one alloy class; g > 0.1 (self-illuminated vents, lights) left alone;
                  the amber (glowing, translucent) shader has no overlay at all (cov_emissive)
  Chief armour:  b (change colour, the green) is the plate; g > 0.1 lights; the rest by its class mask
                 (M2: undersuit, hard polymer, the glove's grip), or the undersuit without one
  fp hands:      b is the plate on "armor hands", the rest of that shader hard polymer;
                 "rubber hands": the glove's grip, and with its class mask the sleeve's cloth and hardware
Class masks (M2) are built on the player's machine by class_masks.py from class_mask_recipes.json and the
player's own maps (a mod of their own, prism_class_masks); this pack names them but never carries them.
Characters (M3: Elites, Grunts, Jackals, Marines): overlay_characters.json (--assign), two classes a shader
by a channel rule, and aliases (a class's tile at other strengths: skin_grunt_half where CE's grunt detail
map is already on the skin); pass --uv once per map measured (the unarmoured Marine is only in a30/a50/c10).
"""
import argparse
import hashlib
import json
import os
import shutil

WU = 3.048

# shader -> assignment (uv_wu filled in from the measured JSON)
SHADERS = {
    # weapons first: Assault Rifle, Pistol, Plasma Rifle, Plasma Pistol (first person)
    "weapons\\assault rifle\\fp\\shaders\\gun": {
        "slots": ["unsc_gunmetal", "unsc_painted_metal"], "mask": {"channel": "r", "lo": 0.25, "hi": 0.55}},
    "weapons\\assault rifle\\fp\\shaders\\grip": {"slots": ["unsc_polymer_stipple"]},
    "weapons\\pistol\\fp\\shaders\\silver": {
        "slots": ["unsc_gunmetal", "unsc_painted_metal"], "mask": {"channel": "r", "lo": 0.15, "hi": 0.35}},
    "weapons\\pistol\\fp\\shaders\\black": {"slots": ["unsc_painted_metal"]},
    "weapons\\plasma rifle\\fp\\shaders\\gun": {"slots": ["cov_alloy"], "exclude": {"channel": "g", "above": 0.1}},
    "weapons\\plasma rifle\\fp\\shaders\\dull": {"slots": ["cov_alloy"], "exclude": {"channel": "g", "above": 0.1}},
    "weapons\\plasma rifle\\fp\\shaders\\amber": None,
    "weapons\\plasma pistol\\shaders\\pp dull": {"slots": ["cov_alloy"], "exclude": {"channel": "g", "above": 0.1}},
    "weapons\\plasma pistol\\shaders\\pp shiney": {"slots": ["cov_alloy"], "exclude": {"channel": "g", "above": 0.1}},
    # the Chief (M2): plate on multipurpose B; the rest undersuit, black hardware or the glove's grip by
    # the locally built class mask (class_masks.py); without it plate / undersuit as in M1
    "characters\\cyborg\\shaders\\armor": {
        "slots": ["mjolnir_plate", "mjolnir_undersuit", "hard_polymer", "rubber_grip"],
        "mask": {"channel": "b", "lo": 0.35, "hi": 0.65}, "exclude": {"channel": "g", "above": 0.1},
        "class_mask": ["masks/characters_cyborg_shaders_armor_0.png"]},
    "characters\\cyborg\\shaders\\visor": None,
    "characters\\cyborg\\fp\\shaders\\armor hands": {
        "slots": ["mjolnir_plate", "hard_polymer"], "mask": {"channel": "b", "lo": 0.35, "hi": 0.65}},
    # the glove's grip on the hand and wrist islands, the sleeve's cloth and hardware on the forearm and
    # upper arm (class mask; without it the grip everywhere, as in M1)
    "characters\\cyborg\\fp\\shaders\\rubber hands": {
        "slots": ["rubber_grip", "mjolnir_undersuit", "hard_polymer"],
        "class_mask": ["masks/characters_cyborg_fp_shaders_rubber_hands_0.png"]},
}

# per-class strengths and fades (metres), tuned in game (ingame_compare.py); anything not given
# is the library's (fade_m) or 1.0
DEFAULT_TUNING = {"classes": {}, "shaders": {}}


def fade_wu(fade_m):
    if isinstance(fade_m, dict):
        values = [v for pair in fade_m.values() for v in pair]
        fade_m = [min(values), max(values)]
    return [round(fade_m[0] / WU, 4), round(fade_m[1] / WU, 4)]


def uv_for(shader, uvs):
    """uv_wu of the shader from the first of the measured JSONs that has it"""
    have = [u for u in uvs if shader in u["shaders"]]
    if not have:
        raise SystemExit("%s: no UV density measured (measure_uv_density.py on a map that has it)" % shader)
    entry = have[0]["shaders"][shader]
    fp = [m for m in entry.get("by_model", {}) if "/fp/" in m]
    return entry["by_model"][fp[0]]["uv_wu"] if fp else entry["uv_wu"]


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--uv", required=True, action="append",
                    help="measure_uv_density.py --json output (repeat: the first that has a shader wins)")
    ap.add_argument("--assign", action="append",
                    help="more assignments: JSON {shaders: {...as overlay.json...}, aliases: {name: {of, normal, "
                         "cavity, rough}}} (default: overlay_characters.json beside this script, PRISM M3)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tuning", help="JSON: {classes: {name: {normal, cavity, rough, fade_m, footprint}}, "
                                     "shaders: {name: {strength, reflection}}}")
    a = ap.parse_args()
    library = json.load(open(os.path.join(a.tiles, "library.json")))
    uv = [json.load(open(p)) for p in a.uv]
    assign = [json.load(open(p)) for p in (a.assign if a.assign is not None else
                                            [os.path.join(os.path.dirname(os.path.abspath(__file__)), "overlay_characters.json")])]
    aliases = {k: v for d in assign for k, v in d.get("aliases", {}).items()}
    assigned = dict(SHADERS)
    for d in assign:
        for k, v in d["shaders"].items():
            assigned.setdefault(k, v)
    tuning = json.load(open(a.tuning)) if a.tuning else DEFAULT_TUNING
    shaders = {}
    for name, entry in assigned.items():
        if entry is None:
            shaders[name] = None
            continue
        if "uv_wu" in entry and not any(name in u["shaders"] for u in uv):
            entry = dict(entry)                         # (measured into the assignments: overlay_characters.json)
        else:
            entry = dict(entry, uv_wu=round(uv_for(name, uv), 5))
        entry.update(tuning.get("shaders", {}).get(name, {}))
        shaders[name] = entry
    used = sorted({c for s in shaders.values() if s for c in s["slots"]})
    classes = {}
    if os.path.isdir(os.path.join(a.out, "tags")):
        shutil.rmtree(os.path.join(a.out, "tags"))
    tiles = os.path.join(a.out, "tags", "materials", "tiles")
    os.makedirs(tiles, exist_ok=True)
    inputs = {"library.json": digest(os.path.join(a.tiles, "library.json")), "uv": [digest(p) for p in a.uv],
              "assign": [digest(p) for p in (a.assign or [])]}
    for name in used:
        alias = aliases.get(name)
        if alias:
            # (another class's tile at its own strengths: its tiles are copied under its own name)
            info = library["classes"][alias["of"]]
        else:
            info = library["classes"][name]
        if not info.get("pass", True):
            raise SystemExit("%s failed its gates: %s" % (name, info.get("failed")))
        src = os.path.join(a.tiles, info["files"]["packed_v1"])
        tile_name = alias["of"] if alias else name
        shutil.copyfile(src, os.path.join(tiles, tile_name + ".png"))
        inputs[name + ".png"] = digest(src)
        levels = None
        kt = info["files"].get("ktx2")
        if kt:
            levels = {}
            for key, fname in (("normal", kt["n_ktx2"]), ("cavity_rough", kt["cr_ktx2"])):
                shutil.copyfile(os.path.join(a.tiles, fname), os.path.join(tiles, fname))
                inputs[fname] = digest(os.path.join(a.tiles, fname))
                levels[key] = "tiles/" + fname
        t = dict(tuning.get("classes", {}).get(alias["of"] if alias else name, {}))
        if alias:
            for k in ("normal", "cavity", "rough"):
                t[k] = round(t.get(k, 1.0) * alias.get(k, 1.0), 4)
        classes[name] = {"tile": "tiles/%s.png" % tile_name, "tile_wu": round(info["tile_m"] / WU, 5),
                         "normal": t.get("normal", 1.0), "cavity": t.get("cavity", 1.0), "rough": t.get("rough", 1.0),
                         "fade": fade_wu(t.get("fade_m", info["fade_m"])), "footprint": t.get("footprint", [32, 8])}
        if levels:
            classes[name]["levels"] = levels
    overlay = {"format": 1, "classes": classes, "shaders": shaders}
    with open(os.path.join(a.out, "tags", "materials", "overlay.json"), "w") as f:
        json.dump(overlay, f, indent=1)
    with open(os.path.join(a.out, "mod.toml"), "w") as f:
        f.write('name = "PRISM materials"\nid = "prism_materials"\nversion = "0.4"\n'
                # (the game keeps 255 bytes of a description: port_mods.c; the setting's name as port_config.c has it)
                'description = "Fine surface grain (micro normal, cavity, roughness; no colour) on the first-person '
                'AR, Pistol, Plasma Rifle and Plasma Pistol, the Chief, Elites, Grunts, Jackals and Marines. '
                'Needs display.prism_material_overlay = true. Masks: mod prism_class_masks."\n')
    with open(os.path.join(a.out, "build.json"), "w") as f:
        json.dump({"inputs": inputs, "tuning": tuning, "wu_m": WU}, f, indent=1)
    print("wrote", a.out, len(classes), "classes", sum(1 for s in shaders.values() if s), "shaders")


if __name__ == "__main__":
    main()
