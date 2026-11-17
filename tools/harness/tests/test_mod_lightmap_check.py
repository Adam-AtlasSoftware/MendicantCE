"""A mod's lightmap is taken only for the BSP layout it was made for (OpenH1 issue 1172): the real mod_bitmaps.c and
mod_lightmaps.c keep the map's lightmap until the lightmap's BSP is read, each BSP of a map on its own, then take the
mod's or leave it out by its layout file's fingerprint, or with none by the map's pages' sizes and pictures (the
map's at 1x, 2x and re-encoded taken; Halo PC's sizes, and another layout's pictures at the map's sizes, left out).
tools/lightmap_fingerprint.py's fingerprint is the game's."""
import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import lightmap_fingerprint  # noqa: E402

CASES = ["fingerprint-matches", "fingerprint-differs", "fingerprint-stable", "pictures-same-layout",
         "pictures-pc-sizes", "pictures-other-layout", "per-bsp", "held-textures"]
NEGATIVE_CONTROLS = {
    # the size check alone: another layout's pictures at the map's sizes were taken
    "sizes-only": (("\t\t\tif (correlation < MOD_LIGHTMAP_MINIMUM_CORRELATION)", "\t\t\tif (0)"),
                   "pictures-other-layout"),
    # the layout file passed over: a lightmap made for another layout, with the map's pictures, was taken
    "fingerprint-ignored": (("\telse if (made->layout_file != NONE)\n", "\telse if (0)\n"), "fingerprint-differs"),
    # decided as any BSP is read: BSP 1's lightmap was decided by BSP 0's layout
    "any-bsp": (("\t\t\t\tmod_bitmaps_globals.bsps[index].lightmap_tag_index == made->tag_index)",
                 "\t\t\t\tTRUE)"), "per-bsp"),
    # taken as the mods load, before the BSP: the map's lightmap was not drawn meanwhile
    "not-waiting": (("\t\tif (made->waiting)\n\t\t\tcount++;", "\t\tcount++;"), "per-bsp"),
    # a fingerprint blind to the corners' coordinates
    "no-corners": (("\t\t\t\t\tfingerprint_word(&hash, lightmap_coordinate(vertices + vertex * lightmap_vertex_size, 0));\n",
                    ""), "fingerprint-stable"),
}

MODS_HEADER = read("port/linux/game/mods.h")
LOADER = read("port/linux/game/mod_bitmaps.c")
LIGHTMAPS = read("port/linux/game/mod_lightmaps.c")


def generated(fault=None):
    group = read("source/bitmaps/bitmap_group.h")
    tags = read("source/tag_files/tag_groups.h")
    structures = "".join(structure(tags, name) + "\n" for name in ["tag_block", "tag_reference", "tag_data"])
    structures += "".join(structure(group, name) + "\n" for name in
                          ["bitmap_data", "bitmap_group_sprite", "bitmap_group_sequence", "bitmap_group"])
    code = "".join(line + "\n" for line in re.findall(r"^#define MOD_LIGHTMAP_\w+ .*$", MODS_HEADER, re.M))
    code += LIGHTMAPS[LIGHTMAPS.index("/* ---------- constants */"):]
    code += enum_with(LOADER, "_bitmap_swizzled_bit") + "\n"
    code += LOADER[LOADER.index("/* bitmap_group.c's usages"):LOADER.index("/* the HUD's digits' tag")]
    code += re.search(r"^#define MAXIMUM_LAYOUT_FILE_SIZE .*$", LOADER, re.M).group(0) + "\n"
    code += "".join(structure(LOADER, name) + "\n" for name in ["mod_bitmap", "hud_placement_scale",
                                                                "mod_bitmap_override"])
    code += LOADER[LOADER.index("static struct\n{\n\t/* by the map's tag"):LOADER.index("} mod_bitmaps_globals;") + 22]
    code += "\n" + "\n".join(function(LOADER, name) for name in [
        "mod_bitmap_free", "override_get", "bitmap_plug", "bitmap_forget", "bitmaps_scale_find",
        "lightmap_layout_differs", "map_page_read", "lightmap_pictures_differ", "layout_file_read", "lightmap_decide",
        "lightmaps_decide", "mod_bitmaps_structure_bsp_read", "mod_bitmaps_waiting_count"]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("structures.inc", structures), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("mod_lightmap_check", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("mod_lightmap_check", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"


def test_tool_fingerprint_is_the_games():
    """the C case's BSP (fingerprint-print), described as the tool reads one: the same fingerprint"""
    status, output = run(build("mod_lightmap_check", generated()), "fingerprint-print")
    assert status == 0, output
    surfaces, vertices, layout, pages = 4, 6, 5, 3

    def coordinates(lightmap, vertex):
        return ((vertex * 4000 + lightmap * 300 + layout * 17) % 32000,
                (vertex * 2500 + lightmap * 700 + layout * 29) % 32000 - 16000)

    lightmaps = []
    for lightmap in range(pages + 1):
        corners = [coordinates(lightmap, vertex) if vertex < vertices else None
                   for surface in range(surfaces) for vertex in (surface, surface + 1, surface + 2)]
        if lightmap == 2:
            corners[1 * 3 + 2] = None  # (its corner past its vertices)
        lightmaps.append((lightmap if lightmap < pages else -1, [(surfaces, None if lightmap == 1 else corners)]))
    assert output.splitlines()[-1] == lightmap_fingerprint.text(lightmap_fingerprint.fingerprint(lightmaps))


def test_tool_reads_a_map(tmp_path):
    """the tool's command line on a map that is not one: refused, saying so"""
    path = tmp_path / "not.map"
    path.write_bytes(b"\0" * 4096)
    result = subprocess.run([sys.executable, str(Path(lightmap_fingerprint.__file__)), str(path)],
                            capture_output=True, text=True)
    assert result.returncode == 1 and "not an Xbox map" in result.stderr
