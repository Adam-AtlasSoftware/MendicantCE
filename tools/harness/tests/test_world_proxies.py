"""display.ambient_occlusion "world"'s object proxies (port/linux/include/world_proxies.h,
port/linux/game/world_proxies.c, docs/world-ao.md): each kind's analytic occlusion against a numerical integral of the
hemisphere, its reach, a window's culling, order and owner slots, the shapes made from a skeleton and placed on its
nodes, and the shapes' GPU layout alike on both sides."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["sphere", "capsule", "box", "reach", "culling", "skeleton", "owner", "none"]
HEADER = "port/linux/include/world_proxies.h"
SOURCE = "port/linux/include/world_proxies_source.h"
GAME = "port/linux/game/world_proxies.c"
CONTROLS = [
    # the sphere's horizon left out
    ("if (k2 > 0.001f)", "if (k2 > 1.0f)", "sphere"),
    # the capsule's tube half as wide
    ("line = 2.0f * radius * h / WORLD_PROXIES_PI * (f1 - f0);",
     "line = radius * h / WORLD_PROXIES_PI * (f1 - f0);", "capsule"),
    # the box's form factor twice over, then its back faces counted
    ("occlusion += fabsf(face_occlusion) / (2.0f * WORLD_PROXIES_PI);",
     "occlusion += fabsf(face_occlusion) / WORLD_PROXIES_PI;", "box"),
    ("if ((float)side * local[axis] <= extents[axis])", "if ((float)side * local[axis] <= -extents[axis])", "box"),
    # a box's reach too short
    ("return 17.0f * sqrtf(diagonal_squared);", "return 4.0f * sqrtf(diagonal_squared);", "reach"),
    # culled by its centre, not its reach; the farthest kept first
    ("if (dot3(planes[plane], centre) - planes[plane][3] > reach)",
     "if (dot3(planes[plane], centre) - planes[plane][3] > 0.0f)", "culling"),
    ("distance = length3(offset);", "distance = -length3(offset);", "culling"),
    # a slot per shape rather than per owner
    ("proxies[i].v[1][3] = j < i ? proxies[j].v[1][3] : (float)slots++;",
     "proxies[i].v[1][3] = (float)i;", "owner"),
    # a capsule's end carried as a direction, not a point
    ("frame_point(frame, shape->b, proxy->v[1]);", "frame_vector(frame, shape->b, proxy->v[1]);", "skeleton"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    return (("world_proxies.h", read(HEADER)), ("world_proxies_source.h", read(SOURCE)), ("under_test.inc", game))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_proxies", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_proxies", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_gpu_takes_as_many_as_a_window_gives():
    gpu = read("port/linux/src/xgpu_stream.h")
    assert "#define XGPU_WORLD_AO_PROXIES_MAXIMUM 64" in gpu
    assert "WORLD_PROXY_MAXIMUM_PER_WINDOW = 64," in read(HEADER)
    for name, kind in (("SPHERE", "sphere"), ("CAPSULE", "capsule"), ("BOX", "box")):
        number = ["sphere", "capsule", "box"].index(kind) + 1
        assert f"#define XGPU_WORLD_AO_PROXY_{name} {number}" in gpu
        assert f"_world_proxy_{kind}," in read(HEADER)


def test_the_game_side_reads_only_through_the_adapter():
    """world_proxies.c is pure arithmetic: the game is read in world_proxies_game.c alone"""
    game = read(GAME)
    for include in ("cseries.h", "objects/", "render/", "tag_files/"):
        assert include not in game
