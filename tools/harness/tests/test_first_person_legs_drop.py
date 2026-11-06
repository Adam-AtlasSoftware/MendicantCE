"""The first-person legs lowered out of the near plane's cut (port/linux/game/first_person_legs.c's
first_person_legs_drop, #692): crouching in the air brings the eye down to the hips, and the near plane cut the thighs
there, which a wide view showed as black slivers. The legs are lowered by the least that keeps every point of them out
of the part of the view the near plane cuts away, at any field of view, window shape, split screen's view or pitch;
standing and crouching on the ground nothing is lowered. And where it is wired: the flat screen's pose, after it is
set back, with the view as drawn."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, inline, mutated, read, run  # noqa: E402

CASES = ["standing", "crouch_jump", "beside", "above", "chain", "most", "none"]
CONTROLS = [
    # the near plane's distance taken as the sides' are (outward the cut side): the cut's depth backwards
    ("\t\t\t\tdistance = -distance;\n", "", "crouch_jump"),
    # no margin: the legs left touching the near plane
    ("margin = FIRST_PERSON_LEGS_UNCUT;", "margin = 0.f;", "crouch_jump"),
    # the margin about the sides too: lowered for what is out of the view (crouching at 160, its waist 0.126 below)
    ("real margin = 0.f;", "real margin = FIRST_PERSON_LEGS_UNCUT;", "standing"),
    # the bottom and top alone, not the sides: lowered for what is beside the view
    ("for (plane = 0; plane < 5 && low < high; plane++)", "for (plane = 2; plane < 5 && low < high; plane++)",
     "beside"),
    # the near plane left out: lowered for all within the sides, however far
    ("for (plane = 0; plane < 5 && low < high; plane++)", "for (plane = 0; plane < 4 && low < high; plane++)",
     "standing"),
    # a span's start not weighed: lowered past points that are in the cut only lower down
    ("if (cut[index][0] <= drop && drop < cut[index][1])", "if (drop < cut[index][1])", "above"),
    # one pass over the spans: a span it is lowered into, met before, not left
    ("while (moved && drop < FIRST_PERSON_LEGS_DROP_MAXIMUM);", "while (moved && 0);", "chain"),
    # no most
    ("return MIN(drop, FIRST_PERSON_LEGS_DROP_MAXIMUM);", "return drop;", "most"),
]


def generated(fault=None):
    code = read("port/linux/game/first_person_legs.c")
    math = read("source/math/real_math.h")
    text = "\n".join(re.findall(r"^#define FIRST_PERSON_LEGS_(?:UNCUT|DROP_MAXIMUM) .*$", code, re.M)) + "\n" + \
        function(code, "first_person_legs_drop")
    if fault:
        text = mutated(text, *fault)
    helpers = "\n".join(inline(math, name) for name in ["dot_product3d", "plane3d_distance_to_point"])
    return (("under_test.inc", helpers + "\n" + text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("first_person_legs_drop", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("first_person_legs_drop", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_constants():
    # the test's margin, most, near distance and reach are the code's
    code = read("port/linux/game/first_person_legs.c")
    assert "#define FIRST_PERSON_LEGS_UNCUT 0.01f" in code
    assert "#define FIRST_PERSON_LEGS_DROP_MAXIMUM 0.5f" in code
    test = read("tools/harness/tests/first_person_legs_drop.c")
    assert "#define MARGIN 0.01f" in test and "#define MOST 0.5f" in test
    assert "#define REACH 0.2f" in test and "#define NEAR_CLIP_REACH 0.2f" in read("port/linux/game/near_clip.c")
    assert re.search(r"rasterizer_global_defaults =\s*\{\s*0\.0625f,", read("source/rasterizer/rasterizer.c"))
    # the frustum's planes in the order the test builds them
    assert re.search(r"_render_frustum_plane_left,\s*_render_frustum_plane_right,\s*_render_frustum_plane_bottom,"
                     r"\s*_render_frustum_plane_top,\s*_render_frustum_plane_near,",
                     read("source/render/render_cameras.c"))


def test_where_the_pose_takes_it():
    code = read("port/linux/game/first_person_legs.c")
    pose = function(code, "first_person_legs_node_matrices")
    # the posed surface weighed in the view as drawn (the rasterizer's frustum, its near plane near_clip.c's), every
    # node lowered by it, after the pose is set back
    wired = ("first_person_legs_drop(&global_window_parameters.frustum, first_person_legs_points, points,")
    assert wired in pose
    assert "first_person_legs_surface(mesh, first_person_legs_matrices, count)" in pose
    assert "first_person_legs_matrices[index].position.z -= drop;" in pose
    assert pose.index("matrix->position.z += back.k * set_back;") < pose.index(wired)
    assert pose.index(wired) < pose.rindex("return first_person_legs_matrices;")
    # the surface as the game skins it: each node's from the model's space, its two nodes blended by the weight
    surface = function(code, "first_person_legs_surface")
    assert "matrix4x3_multiply(&pose[index], &mesh->inverse[index], &skin[index]);" in surface
    assert "real weight = (real)vertex->weight / 32767.f;" in surface
    # drawing alone: the game's node matrices are read, never written
    assert "object_get_node_matrices" not in code
