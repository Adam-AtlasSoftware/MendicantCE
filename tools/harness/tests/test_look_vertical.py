"""A controller's vertical look sensitivity (input.look_vertical_sensitivity, source/game/player_control.c's
player_control_stick_look): its default turns up and down as the original, to the bit; another value scales the turn
up and down, and leaves side to side as it was."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["default_is_the_original", "scales_up_and_down_alone"]

CONTROLS = [
    # the setting not applied: the turn up and down never changes
    (" *\n\t\thalo_look_vertical_sensitivity(gamepad_index);", ";", "scales_up_and_down_alone"),
]


def generated(fault=None):
    control = read("source/game/player_control.c")
    code = "\n".join([
        structure(control, "player_control_aim"),
        function(control, "player_control_stick_look"),
    ]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_look_vertical(case):
    status, output = run(build("look_vertical", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_look_vertical_negative_control(before, after, case):
    status, output = run(build("look_vertical", generated((before, after))), case)
    assert status == CHECK_FAILED, output
