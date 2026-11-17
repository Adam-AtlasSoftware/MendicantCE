"""The controller's rumble strength (input.rumble_strength, source/input/input_xbox.c's input_port_write_rumble):
100%, the default, writes the game's own motor speeds, to the bit; another strength scales both motors."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["default_is_the_game_s", "strength_scales_both"]

CONTROLS = [
    # the strength not applied to the right motor
    ("right_frequency * strength / 100)", "right_frequency)", "strength_scales_both"),
]


def generated(fault=None):
    code = function(read("source/input/input_xbox.c"), "input_port_write_rumble") + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_rumble_strength(case):
    status, output = run(build("rumble_strength", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_rumble_strength_negative_control(before, after, case):
    status, output = run(build("rumble_strength", generated((before, after))), case)
    assert status == CHECK_FAILED, output
