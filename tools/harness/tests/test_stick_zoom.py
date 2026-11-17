"""The look stick's zoomed turn (input.stick_zoom_sensitivity, source/game/player_control.c's
player_control_stick_zoom_look): 1, the default, leaves the turn as the original's; another value scales the turn
zoomed in alone; the frame's turn and a sub-step's both take it, before the touch controls' turn is added."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["default_is_the_original", "scales_zoomed_alone"]

CONTROLS = [
    # the scale taken unzoomed too
    ("if (unit_index != NONE && zoom_level != NONE)", "if (unit_index != NONE)", "scales_zoomed_alone"),
]


def generated(fault=None):
    code = function(read("source/game/player_control.c"), "player_control_stick_zoom_look") + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_stick_zoom(case):
    status, output = run(build("stick_zoom", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_stick_zoom_negative_control(before, after, case):
    status, output = run(build("stick_zoom", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_frame_and_the_sub_step_both_take_it():
    code = read("source/game/player_control.c")
    assert code.count("player_control_stick_zoom_look(") == 3
    blob = function(code, "get_local_player_input_blob")
    assert blob.index("player_control_stick_zoom_look(") < blob.index("halo_linux_touch_look(")
