"""display.ambient_occlusion "world"'s distance field (port/linux/include/world_sdf.h,
port/linux/game/world_sdf_bake.c, port/linux/game/world_sdf.c, docs/world-ao.md): the layout and plan within its
budget, the encoding's round trip, the sparse field's sample as the GPU reads it (the whole grid's it stands for,
exactly), its atlas, the bake against exact distances (a plane, a box room, a sphere, a box signed by a BSP's point
test, a soup of triangles against every triangle; the bricks' shared faces alike), and the cache file's round trip
and rejections."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["layout", "encoding", "sample", "atlas", "bake", "plane", "room", "sphere", "bsp", "soup", "round_trip", "rejection"]
HEADER = "port/linux/include/world_sdf.h"
BAKE = "port/linux/game/world_sdf_bake.c"
SCHEDULE = "port/linux/game/world_sdf.c"
FILE_FUNCTIONS = ["sdf_put32", "sdf_get32", "sdf_put_float", "sdf_get_float", "sdf_crc", "sdf_header_write",
                  "sdf_header_read", "sdf_scenery_most_bytes", "sdf_scenery_size", "sdf_scenery_free",
                  "sdf_scenery_write", "sdf_scenery_read", "sdf_header_good", "sdf_cells_size", "sdf_bricks_size", "sdf_cells_good",
                  "sdf_volume_free", "sdf_file_pack", "sdf_file_unpack", "sdf_file_write",
                  "sdf_file_read", "sdf_cache_load", "sdf_bsp_inside"]
CONTROLS = [
    # a cell marked only where a triangle is within range of its middle (not of each of its voxels)
    (BAKE, "float reach = header->range + 0.5f * step * 1.7320508f;", "float reach = header->range;", "soup"),
    # the table's kinds read the wrong way round
    (BAKE, "return entry[3] == WORLD_SDF_CELL_INSIDE ? -header->range : header->range;",
     "return entry[3] == WORLD_SDF_CELL_INSIDE ? header->range : -header->range;", "sample"),
    # a brick's local voxel taken from the wrong cell (its place as the next cell's)
    (BAKE, "local[axis] = coordinate - (float)(cell[axis] * WORLD_SDF_BRICK);",
     "local[axis] = coordinate - (float)(cell[axis] * WORLD_SDF_BRICK_SPAN);", "sample"),
    # the atlas's rows of bricks laid one voxel row apart
    (BAKE, "index / WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN * row;", "index / WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN;",
     "atlas"),
    # the encoding truncated the wrong way round
    (BAKE, "return (signed char)(value < 0.0f ? value - 0.5f : value + 0.5f);",
     "return (signed char)(value * 0.5f);", "encoding"),
    # the sample's weights for the far corner and the near swapped (along z, the test plane's)
    (BAKE, "(((corner >> 2) & 1) ? fraction[2] : 1.0f - fraction[2])",
     "(((corner >> 2) & 1) ? 1.0f - fraction[2] : fraction[2])", "sample"),
    # a nearer triangle never kept
    (BAKE, "if (distance < row_best[x] * row_best[x])", "if (distance > row_best[x] * row_best[x])", "plane"),
    # no sign: the solid outside the room as air
    (BAKE, "if (inside && inside(context, point))", "if (inside && 0)", "room"),
    # the rows cut to the plane's reach the wrong way (the walls' voxels near them missed)
    (BAKE, "else if (fabsf(plane_row) >= reach)", "else if (fabsf(plane_row) < reach)", "room"),
    # an edge's region taken as the face's (corners rounded wrongly)
    (BAKE, "if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)", "if (0)", "soup"),
    # the point test's children swapped
    (SCHEDULE, "- plane[3] >= 0.0f)];", "- plane[3] < 0.0f)];", "bsp"),
    # the CRC not checked
    (SCHEDULE, "sdf_get32(bytes + size - WORLD_SDF_FILE_CRC) != sdf_crc(0, bytes, size - WORLD_SDF_FILE_CRC)", "0",
     "rejection"),
    # a bad file kept
    (SCHEDULE, "\t\tremove(path);\n\t\tplatform_log(\"world sdf: %s is damaged", "\t\tplatform_log(\"world sdf: %s is damaged",
     "rejection"),
    # a cell's kind or its brick's place not checked
    (SCHEDULE, "if (entry[3] > WORLD_SDF_CELL_BRICK || (entry[3] == WORLD_SDF_CELL_BRICK &&",
     "if (0 && (entry[3] == WORLD_SDF_CELL_BRICK &&", "rejection"),
    # the header's fields out of order
    (SCHEDULE, "sdf_put32(bytes + 16, header->width);\n\tsdf_put32(bytes + 20, header->height);",
     "sdf_put32(bytes + 20, header->width);\n\tsdf_put32(bytes + 16, header->height);", "round_trip"),
]


def generated(fault=None):
    bake, schedule = read(BAKE), read(SCHEDULE)
    if fault and fault[0] == BAKE:
        bake = mutated(bake, *fault[1:])
    if fault and fault[0] == SCHEDULE:
        schedule = mutated(schedule, *fault[1:])
    file_code = "\n".join([enum_with(schedule, "WORLD_SDF_FILE_HEADER"), structure(schedule, "sdf_bsp")] +
                          [function(schedule, name) for name in FILE_FUNCTIONS])
    return (("world_sdf.h", read(HEADER)), ("under_test.inc", bake), ("file_under_test.inc", file_code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_sdf", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("path,before,after,case", CONTROLS)
def test_negative_control(path, before, after, case):
    status, output = run(build("world_sdf", generated((path, before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_bake_is_never_in_a_load():
    # (the owner's rule: a map loads as fast as without the feature) scenario_load only hands the cache's lookup to a
    # thread: no file read, no geometry copied, no bake
    scenario = read("source/scenario/scenario.c")
    assert "world_ao_map_loaded();" in scenario
    assert "world_sdf_bake_cell" not in scenario and "world_sdf_plan" not in scenario
    loaded = function(read(SCHEDULE), "world_sdf_map_loaded") + function(read(SCHEDULE), "sdf_current_set")
    for call in ("sdf_cache_load", "sdf_cache_present", "sdf_bake_begin", "world_scene_static_triangles", "fopen"):
        assert call not in loaded, call
