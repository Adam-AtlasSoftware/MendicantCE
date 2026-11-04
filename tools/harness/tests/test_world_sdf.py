"""display.ambient_occlusion "world"'s distance field (port/linux/include/world_sdf.h,
port/linux/game/world_sdf_bake.c, docs/world-ao.md): the layout within its budget, the encoding's round trip, the
trilinear sample as the GPU reads it, the bake (workstream B grows it)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["layout", "encoding", "sample", "bake"]
HEADER = "port/linux/include/world_sdf.h"
BAKE = "port/linux/game/world_sdf_bake.c"
CONTROLS = [
    # the voxel never grown: a huge map past the budget
    ("voxel *= 1.125f;", "voxel *= 1.0f; break;", "layout"),
    # the encoding truncated the wrong way round
    ("return (signed char)(value < 0.0f ? value - 0.5f : value + 0.5f);", "return (signed char)(value * 0.5f);",
     "encoding"),
    # the sample's weights for the far corner and the near swapped
    ("((corner & 1) ? fraction[0] : 1.0f - fraction[0])", "((corner & 1) ? 1.0f - fraction[0] : fraction[0])",
     "sample"),
]


def generated(fault=None):
    bake = read(BAKE)
    if fault:
        bake = mutated(bake, *fault)
    return (("world_sdf.h", read(HEADER)), ("under_test.inc", bake))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_sdf", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_sdf", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_bake_is_never_in_a_load():
    # (the owner's rule: a map loads as fast as without the feature) scenario_load only looks in the cache
    scenario = read("source/scenario/scenario.c")
    assert "world_ao_map_loaded();" in scenario
    assert "world_sdf_bake_slab" not in scenario
