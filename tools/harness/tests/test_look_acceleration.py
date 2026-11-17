"""A controller's look acceleration settings (input.look_acceleration_strength and input.look_acceleration_time,
source/game/player_control.c's player_control_look_acceleration): the defaults give the original's ramp to the bit on
any map's globals; strength 0 turns it off; strength 2 and time 0.5 scale it as they say."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["default_is_the_original", "strength_zero_no_ramp", "strength_two", "time_half"]

CONTROLS = [
    # the stock maps' 3X written in, not the map's own: right on every stock map, wrong on another globals
    ("(constants->look_acceleration_scale - 1.f) * strength", "2.f * strength", "default_is_the_original"),
    # the ramp's time a hair off: no longer the original's to the bit
    ("(constants->look_acceleration_time * time)", "(constants->look_acceleration_time * time * 1.0000001f)",
     "default_is_the_original"),
    # strength 0 still counted as accelerating (its clock runs on)
    ("\t\t\taccelerating = FALSE;", "", "strength_zero_no_ramp"),
    # the strength ignored
    (" * strength * acceleration", " * acceleration", "strength_two"),
    # the time ignored
    ("(constants->look_acceleration_time * time)", "constants->look_acceleration_time", "time_half"),
]


def generated(fault=None):
    control = read("source/game/player_control.c")
    defines = "\n".join(re.findall(r"^#define LOOK_ACCELERATION_\w+ .*$", control, re.M))
    code = "\n".join([
        defines,
        structure(control, "player_control_aim"),
        function(control, "player_control_look_acceleration_tuning"),
        function(control, "player_control_look_acceleration"),
        function(control, "player_control_stick_look"),
    ]) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_look_acceleration(case):
    status, output = run(build("look_acceleration", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_look_acceleration_negative_control(before, after, case):
    status, output = run(build("look_acceleration", generated((before, after))), case)
    assert status == CHECK_FAILED, output
