"""The look stick's dead zone shape (input.look_dead_zone_shape, source/input/input_xbox.c's
input_port_fix_stick_dead_zones): Auto, the default, reads both sticks as before it, to the bit, under either aim style
and stick layout; Axial and Round set the look stick's shape whatever the aim style."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["default_is_the_original", "shape_takes_effect"]

CONTROLS = [
    # Auto no longer the aim style's: Modern's look stick read each direction its own
    ("(shape == HALO_LOOK_DEAD_ZONE_AUTO &&", "(shape == HALO_LOOK_DEAD_ZONE_AXIAL &&", "default_is_the_original"),
]


def generated(fault=None):
    xbox = read("source/input/input_xbox.c")
    code = "\n".join([
        function(xbox, "fix_dead_zone"),
        function(xbox, "fix_dead_zones"),
        function(xbox, "fix_round_dead_zone"),
        function(xbox, "input_port_fix_stick"),
        function(xbox, "input_port_fix_stick_dead_zones"),
    ]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_look_dead_zone_shape(case):
    status, output = run(build("look_dead_zone_shape", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_look_dead_zone_shape_negative_control(before, after, case):
    status, output = run(build("look_dead_zone_shape", generated((before, after))), case)
    assert status == CHECK_FAILED, output
