"""display.ambient_occlusion "world"'s object proxies (port/linux/include/world_proxies.h,
port/linux/game/world_proxies.c, docs/world-ao.md): a sphere's analytic occlusion, the shapes' GPU layout alike on
both sides (workstream C grows the capsule's and the box's)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, mutated, read, run  # noqa: E402

CASES = ["sphere", "none"]
HEADER = "port/linux/include/world_proxies.h"
GAME = "port/linux/game/world_proxies.c"
CONTROLS = [
    # (r / l) for (r / l)^2
    ("return cosine > 0.0f ? cosine * radius * radius / length_squared : 0.0f;",
     "return cosine > 0.0f ? cosine * radius / length : 0.0f;", "sphere"),
    # behind the surface counted
    ("return cosine > 0.0f ? cosine * radius * radius / length_squared : 0.0f;",
     "return fabsf(cosine) * radius * radius / length_squared;", "sphere"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    return (("world_proxies.h", read(HEADER)), ("under_test.inc", game))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_proxies", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_proxies", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_gpu_takes_as_many_as_a_window_gives():
    gpu = read("port/linux/src/xgpu_world_ao.h")
    assert "#define XGPU_WORLD_AO_PROXIES_MAXIMUM 64" in gpu
    assert "WORLD_PROXY_MAXIMUM_PER_WINDOW = 64," in read(HEADER)
    assert "#define XGPU_WORLD_AO_PROXY_SPHERE 1" in gpu and "_world_proxy_sphere," in read(HEADER)
