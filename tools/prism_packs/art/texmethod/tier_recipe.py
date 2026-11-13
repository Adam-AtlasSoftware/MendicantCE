"""PRISM v2 texture tiers: which CE bitmaps are built, and how (recipe only: tag paths and settings, no pixels).

Each base entry: tag path (as extracted, '/' separators), domain, detail kind, class spec, multipurpose tag.
  domain   hard (weapons: paint classes), char (paint, fabric, skin; class mask or automatic), face (all skin),
           cov (Covenant: automatic paint classes, no human skin; Elites add the colour-change plate rule
           and the blue-teal bodysuit as fabric, fusion.cov_char_classes)
  kind     detail2's kind ("hard": edge pass where the classes allow it; "skin": none)
  classes  None (automatic), ("mask", <class mask png>, [slots], rule) with rule (channel, lo, hi) in the
           multipurpose map's Xbox order or None, or ("cov_rule",)
  face     faces stay ML only, at most 4x: Max == Enhanced
  visor    region of the base map under the visor's triangles: plain resample, never armour grain
Source: the b30 extraction (<work>/extract/b30/bitmaps); the mods replace the tag in every map that has it,
after the per-map tag check.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import packs_env as E  # noqa: E402

EXTRACT = E.extract("b30", "bitmaps") if E.WORK else ""
MASKS = E.mod("prism_class_masks", "tags", "materials", "masks") if E.OUT else ""
VISOR = E.work("chief_maps", "tex", "visor_mask.png") if E.WORK else ""  # 4096^2, the Chief's own UVs (hero_tex_r2)

CHIEF_SLOTS = ["mjolnir_plate", "mjolnir_undersuit", "hard_polymer", "rubber_grip"]
HANDS_SLOTS = ["rubber_grip", "mjolnir_undersuit", "hard_polymer"]
B_RULE = (2, 0.35, 0.65)   # colour change (Xbox B), the Chief's plate
R_RULE = (0, 0.05, 0.15)   # specular (Xbox R), the Marines' painted armour

W = "weapons/"
C = "characters/"
BASES = {
    # first-person weapons
    W + "assault rifle/fp/bitmaps/assault rifle_fp": dict(domain="hard", multi=W + "assault rifle/fp/bitmaps/assault rifle multipurpose"),
    W + "pistol/fp/bitmaps/pistol": dict(domain="hard", multi=W + "pistol/fp/bitmaps/pistol multipurpose"),
    W + "plasma rifle/fp/bitmaps/plasma rifle_fp": dict(domain="hard", multi=W + "plasma rifle/fp/bitmaps/plasma rifle multipurpose"),
    W + "plasma pistol/bitmaps/plasma pistol": dict(domain="hard", multi=W + "plasma pistol/bitmaps/plasma pistol multipurpose"),
    # first-person arms and the Chief
    C + "cyborg/fp/bitmaps/cyborg hands": dict(domain="char", multi=C + "cyborg/fp/bitmaps/cyborg hands multipurpose",
                                              classes=("mask", "characters_cyborg_fp_shaders_rubber_hands_0.png", HANDS_SLOTS, None)),
    C + "cyborg/bitmaps/cyborg": dict(domain="char", multi=C + "cyborg/bitmaps/cyborg multipurpose", visor=True,
                                      classes=("mask", "characters_cyborg_shaders_armor_0.png", CHIEF_SLOTS, B_RULE)),
    # Marines (marine_armored) and their faces
    C + "marine_armored/bitmaps/marine_arms": dict(domain="char", multi=C + "marine_armored/bitmaps/marine_arms_multipurpose",
        classes=("mask", "characters_marine_armored_shaders_marine_arms_0.png", ["marine_paint", "marine_bdu", "human_skin"], R_RULE)),
    C + "marine_armored/bitmaps/marine_arms_johnson": dict(domain="char", multi=C + "marine_armored/bitmaps/marine_arms_multipurpose",
        classes=("mask", "characters_marine_armored_shaders_marine_arms_johnson_0.png", ["marine_paint", "marine_bdu", "human_skin"], R_RULE)),
    C + "marine_armored/bitmaps/marine_legs": dict(domain="char", multi=C + "marine_armored/bitmaps/marine_legs_multipurpose",
        classes=("mask", "characters_marine_armored_shaders_marine_legs_0.png", ["marine_paint", "marine_bdu", "hard_polymer"], R_RULE)),
    C + "marine_armored/bitmaps/marine_torso": dict(domain="char", multi=C + "marine_armored/bitmaps/marine_torso_multipurpose",
        classes=("mask", "characters_marine_armored_shaders_marine_torso_0.png", ["marine_paint", "marine_webbing", "marine_metal"], R_RULE)),
    C + "marine_armored/bitmaps/marine_helmet": dict(domain="char", multi=C + "marine_armored/bitmaps/marine_helmet_multipurpose"),
    C + "marine_armored/bitmaps/face_johnson_sarge": dict(domain="face", face=True),
}
for _n in ("charles", "chris", "marcus", "matt", "rob", "shiek"):
    BASES[C + "marine/bitmaps/face_" + _n] = dict(domain="face", face=True, multi=C + "marine/bitmaps/face_%s_multipurpose" % _n)
# Elites (minor and major share these maps through colour change; the special has its own)
for _n in ("arms", "head", "head1", "legs", "legs1", "torso"):
    BASES[C + "elite/bitmaps/elite " + _n] = dict(domain="cov", classes=("cov_rule",), multi=C + "elite/bitmaps/elite %s multi" % _n)
BASES[C + "elite/elite_special/bitmaps/elite special"] = dict(domain="cov", classes=("cov_rule",),
                                                           multi=C + "elite/elite_special/bitmaps/elite special multi")
# Grunts (two bitmaps per tag) and Jackals
for _n in ("arms and legs", "head and back", "torso"):
    BASES[C + "grunt/bitmaps/grunt " + _n] = dict(domain="cov", multi=C + "grunt/bitmaps/grunt %s multi" % _n)
for _n in ("arms", "head", "legs", "torso"):
    BASES[C + "jackal/bitmaps/jackal " + _n] = dict(domain="cov", multi=C + "jackal/bitmaps/jackal_%s multipurpose" % _n)

for _t, _r in BASES.items():
    _r.setdefault("classes", None)
    _r.setdefault("face", False)
    _r.setdefault("visor", False)
    _r.setdefault("multi", None)
    _r.setdefault("kind", "skin" if _r["face"] else "hard")

# tier sizes (factor per side, capped): Enhanced fusion 4x (2048 cap); Max fusion + detail2 at 8x (4096 cap),
# by texel density (one detail octave above the fusion base, the study's 4K target for character maps);
# multipurpose maps at half the base map's factor (masks are low-frequency; their edges follow the base map)
ENH_F, ENH_CAP = 4, 2048
MAX_F, MAX_CAP = 8, 4096
MULTI_ENH_F, MULTI_MAX_F, MULTI_CAP = 2, 4, 2048


def safe(tag):
    return tag.replace("/", "__").replace(" ", "_")
