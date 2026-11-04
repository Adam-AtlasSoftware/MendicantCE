"""display.ambient_occlusion "world"'s scene (port/linux/include/world_scene.h, port/linux/game/world_scene.c,
docs/world-ao.md). The skeleton's empty scene; workstream A grows it (stable ids, drawn transforms, the light list)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run  # noqa: E402

CASES = ["empty"]


def generated():
    return (("world_scene.h", read("port/linux/include/world_scene.h")),
            ("under_test.inc", read("port/linux/game/world_scene.c")))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_scene", generated()), case)
    assert status == 0, output
