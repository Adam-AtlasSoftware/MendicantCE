"""A stick's outer dead zone (input.left_stick_outer_dead_zone, input.right_stick_outer_dead_zone: source/input/
input_xbox.c): with none, the default, every deflection reads as the original's, each direction's and the round one;
with one, a stick that stops short of its rim reads as fully pushed."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["default_is_the_original", "outer_reads_full"]

CONTROLS = [
    # the outer dead zone ignored: full only at the rim, as before
    ("\tif (live_range >= SHORT_MAX)\n", "\tif (1)\n", "outer_reads_full"),
]


def generated(fault=None):
    xbox = read("source/input/input_xbox.c")
    code = "\n".join([
        function(xbox, "fix_dead_zone"),
        function(xbox, "fix_dead_zones"),
        function(xbox, "fix_round_dead_zone"),
    ]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_stick_outer_dead_zone(case):
    status, output = run(build("stick_outer_dead_zone", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_stick_outer_dead_zone_negative_control(before, after, case):
    status, output = run(build("stick_outer_dead_zone", generated((before, after))), case)
    assert status == CHECK_FAILED, output
