"""The first-person legs seen from the view drawn as the body has them from its own (port/linux/game/
render_interpolation.c's render_interpolation_view_body, #697): the body is drawn between the ticks, with the view
between them, but the view drawn on foot is the newest step's (display.direct_camera), a tick's motion on and aimed
in the frame. Drawn where the body is, the legs trailed the view by that, more or less as each step and each frame's
aim came, and stepped against it walking and strafing."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

# the negative control: the legs left on the body, a tick behind the view drawn
LEFT_ON_THE_BODY = ("\tto = &drawn_views[local_player_index];", "\tto = &blended_views[local_player_index];")


def generated(fault=None):
    code = read("port/linux/game/render_interpolation.c")
    text = function(code, "view_heading") + "\n" + function(code, "render_interpolation_view_body")
    if fault:
        text = mutated(text, *fault)
    return (("under_test.inc", text),)


def test_strafing_legs_keep_to_the_view():
    status, output = run(build("first_person_legs_view", generated()), "strafing")
    assert status == 0, output
    status, output = run(build("first_person_legs_view", generated(LEFT_ON_THE_BODY)), "strafing")
    assert status == CHECK_FAILED, output
