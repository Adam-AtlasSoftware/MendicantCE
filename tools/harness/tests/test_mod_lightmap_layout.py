"""A mod's lightmap with no layout file is taken only laid out as the map's (OpenH1 issue 1172): the real
mod_bitmaps.c (lightmap_layout_differs) takes Blood Gulch's Xbox lightmap at 1x and 2x, leaves Halo PC's out (5 of
its 15 pages other sizes), naming the page, and map_requirements_differ checks any bitmap as before."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["xbox-layout", "pc-layout", "not-lightmap"]
NEGATIVE_CONTROLS = {
    # the old check, counts and types only: Halo PC's lightmap was taken
    "count-only": (("\tif (!bitmaps_scale_find(map_group, bitmap, &page))", "\tif (0)"), "pc-layout"),
}


def generated(fault=None):
    group = read("source/bitmaps/bitmap_group.h")
    tags = read("source/tag_files/tag_groups.h")
    loader = read("port/linux/game/mod_bitmaps.c")
    code = "".join(structure(tags, name) + "\n" for name in ["tag_block", "tag_data"])
    code += "".join(structure(group, name) + "\n" for name in
                    ["bitmap_data", "bitmap_group_sprite", "bitmap_group_sequence", "bitmap_group"])
    code += loader[loader.index("/* bitmap_group.c's usages"):loader.index("/* the HUD's digits' tag")]
    code += structure(loader, "mod_bitmap") + "\n"
    code += "\n".join(function(loader, name) for name in ["difference", "bitmaps_scale_find", "hud_scale_find",
                                                          "lightmap_layout_differs", "map_requirements_differ"]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("mod_lightmap_layout", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("mod_lightmap_layout", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
