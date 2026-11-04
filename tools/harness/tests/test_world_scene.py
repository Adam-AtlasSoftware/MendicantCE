"""display.ambient_occlusion "world"'s scene (port/linux/include/world_scene.h, port/linux/game/world_scene.c,
docs/world-ao.md): the whole file against a small fake game. The static triangles' copy of a hand-built collision
BSP."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, mutated, read, run, structure  # noqa: E402

CASES = ["empty", "triangles"]
GAME = "port/linux/game/world_scene.c"
CONTROLS = [
    # every edge's left side taken: the triangle's ring, which starts on the shared edge's right, is lost
    ("right = edge->surface_indices[1] == surface_index;", "right = 0;", "triangles"),
    # fanned from the wrong corner
    ("long const fan[3] = { ring[0], ring[corner], ring[corner + 1] };",
     "long const fan[3] = { ring[corner - 1], ring[corner], ring[corner + 1] };", "triangles"),
    # the surface's material lost
    ("materials[count] = surface->material_index;", "materials[count] = 0;", "triangles"),
    # the bounds' first vertex not taken
    ("if (!index || value < bounds[axis])", "if (value < bounds[axis])", "triangles"),
]


def collision():
    header = read("source/physics/collision_bsp_definitions.h")
    return "\n".join([enum_with(header, "MAXIMUM_EDGES_PER_COLLISION_SURFACE")] +
                     [structure(header, name) for name in ["collision_surface", "collision_edge", "collision_vertex"]])


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    # (the game's headers left out: the test's fake game stands for them)
    game = re.sub(r'^#include "(?!world_scene\.h).*"$', "", game, flags=re.M)
    return (("world_scene.h", read("port/linux/include/world_scene.h")), ("collision.inc", collision()),
            ("under_test.inc", game))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_scene", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_scene", generated((before, after))), case)
    assert status == CHECK_FAILED, output
