"""A replaced model's instance takes the draw's own function values and never looks its identifier up as an object
(port/linux/game/mod_meshes.c's mod_meshes_model_begin), whatever the interpolation: the b40 crash with
HALO_INTERPOLATION=false (a deleted object's datum read through render_interpolation.c) cannot come back."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["ghost-interpolation-off", "ghost-interpolation-on", "sky-interpolation-off", "object-interpolation-off"]

# the old code: the identifier looked up through the interpolation, which reads the object where it has no record
NEGATIVE_CONTROLS = {
    "looks-up-the-identifier": (("functions = object_index != NONE ? function_values : NULL;",
                                 "functions = object_index != NONE ? "
                                 "render_interpolation_object_function_values(object_index) : NULL;"),
                                "ghost-interpolation-off"),
}


def generated(fault=None):
    meshes = read("port/linux/game/mod_meshes.c")
    interpolation = read("port/linux/game/render_interpolation.c")
    config = enum_with(meshes, "_render_model_shadow_bit") + "\n"
    for name in ("MESH_INSTANCE_SHADOW", "MESH_INSTANCE_FIRST_PERSON", "MESH_INSTANCE_NO_SECONDARY"):
        config += re.search(r"^#define " + name + r" \w+$", meshes, re.M).group(0) + "\n"
    config += structure(meshes, "mesh_model") + "\n"
    config += structure(interpolation, "interpolated_object") + "\n"
    text = function(meshes, "node_matrix_constants") + "\n"
    text += function(interpolation, "render_interpolation_object_values") + "\n"
    text += function(interpolation, "render_interpolation_object_function_values") + "\n"
    begin = function(meshes, "mod_meshes_model_begin")
    if fault:
        begin = mutated(begin, *fault)
    text += begin + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("mesh_instance_values", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("mesh_instance_values", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
