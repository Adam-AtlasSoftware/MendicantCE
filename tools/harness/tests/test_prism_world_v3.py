"""PRISM's level remodel files (port/linux/game/prism_world_geometry.c, docs/prism_packs.md): the reader of versions 2 and
3 on hand-made files, version 3's tables checked against the file's figures, the shape's checks (a needle along a
sliver, the declared deviations), the exact stock edge, and version 3's seal groups falling back whole only where their
border is the stock edges."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

GAME = "port/linux/game/prism_world_geometry.c"
CASES = ["parse", "refused", "facets", "plain", "groups", "groups_kept", "groups_stock"]
FUNCTIONS = ["read_u32", "read_s16", "read_f64", "pad4", "pwg_tables_check", "pwg_parse", "shape_vertex", "shape_index",
             "shape_triangle_distance", "shape_run_facets", "shape_edge_key", "shape_edge_compare", "shape_surface",
             "shape_edge_find", "shape_same_edge", "shape_runs", "shape_at", "shape_edge_plain", "shape_group_of",
             "shape_groups_named"]
CONTROLS = [
    # the needle test left out: only the prism's relief and margin (the old test's blind spot along a sliver)
    ("(beyond > 0.0 ? shape_triangle_distance(p, a, b, c) : fabs(height)) > needle",
     "(beyond > 0.0 ? shape_triangle_distance(p, a, b, c) : fabs(height)) > 1000.0 * needle", "facets"),
    # the declared deviation not read
    ("above = declared[0] + DEVIATION_SLACK;", "above = SHAPE_RELIEF;", "facets"),
    # a group's chart past the table let through
    ('if (chart >= file->chart_count)\n\t\t\t{\n\t\t\t\treturn "has a triangle of no chart";',
     'if (0)\n\t\t\t{\n\t\t\t\treturn "has a triangle of no chart";', "refused"),
    # the chart table's size not checked against the file's
    ("if ((size - offset) / 4 < file->chart_count)", "if (0)", "refused"),
    # an edge counted plain where two triangles run along it
    ("if (found != 1)", "if (!found)", "plain"),
    # the far level not checked
    ("for (range = 0; range < 2; range++)\n\t{\n\t\tunsigned long triangle, found = 0;",
     "for (range = 0; range < 1; range++)\n\t{\n\t\tunsigned long triangle, found = 0;", "plain"),
    # the border not checked: a group falls back whatever its neighbours' edges
    ("if (!shape_edge_plain(view, local, u, v))", "if (!shape_edge_plain(view, local, u, v) && 0)", "groups_kept"),
    # a surface of a material drawn stock not bringing its group down
    ("(check->state[surface] == _shape_facets || check->state[surface] == _shape_stock)",
     "(check->state[surface] == _shape_facets)", "groups_stock"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    defines = "\n".join(line for line in game.splitlines()
                        if re.match(r"#define (PWG_|SHAPE_|NEEDLE_|DEVIATION_|SEAL_|ENVIRONMENT_)", line))
    parts = [defines, structure(game, "pwg_file"), structure(game, "pwg_material_view"),
             structure(game, "shape_edge"), structure(game, "shape_check"), enum_with(game, "_shape_kept_whole")]
    parts += [function(game, name) for name in FUNCTIONS]
    return (("under_test.inc", "\n\n".join(parts)),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("prism_world_v3", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("prism_world_v3", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_pack_tools_write_what_the_game_reads():
    """the pack tools' writer (wg3_pwg.py) and the reader agree on the layout's figures"""
    tools = read("tools/prism_packs/art/world_geo/wg3_pwg.py")
    game = read(GAME)
    for flag, value in (("FLAG_FAR", 1), ("FLAG_GROUPS", 2), ("FLAG_DEVIATION", 4), ("FLAG_ATLAS", 8)):
        assert f"#define PWG_{flag} {value}" in game
        assert re.search(rf"\b{flag}\b[^\n]*", tools)
    assert "FLAG_FAR, FLAG_GROUPS, FLAG_DEVIATION, FLAG_ATLAS = 1, 2, 4, 8" in tools
    assert "#define PWG_ATLAS_UV_SCALE 8" in game and "UV_SCALE = 8.0" in tools
    assert "#define PWG_NO_CHART 0xFFFFFFFFUL" in game and "NO_CHART = 0xFFFFFFFF" in tools
    assert "#define SHAPE_RELIEF 1.0" in game and "SHAPE_RELIEF = 1.0" in tools
    assert "#define NEEDLE_FLOOR 0.1" in game and "NEEDLE_FLOOR = 0.1" in tools
    assert "#define NEEDLE_RATIO 1.0" in game and "NEEDLE_RATIO = 1.0" in tools
