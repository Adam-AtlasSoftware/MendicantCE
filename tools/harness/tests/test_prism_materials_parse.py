"""material_parse (port/linux/game/prism_materials.c): a material of PRISM's format 2 read into what
SCENE_MATERIAL_MAPS sends (its images' files, flags, scalars and model), CE's type, detail functions and rescaled
detail scales where the file says "ce", the file's own where it gives them; another format or no base refused."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["ce", "explicit", "model", "refused"]
NAMES = ["json_skip", "json_string", "json_member", "json_real", "json_number", "json_rgb", "json_is", "field_short",
         "field_real", "reference_size", "detail_scale", "image_file", "layer_map", "detail_function",
         "material_parse"]

# a fault in the code, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "not-rescaled": (("scale[0] = value * (float)base_width / (float)width;", "scale[0] = value;"), "detail_scale",
                     "ce"),
    "normal-at-base": (("\"normal_uv\"), \"bump\")", "\"normal_uv\"), \"base\")"), "material_parse", "ce"),
    "function-ignored": (("\"multiply\") ? 1", "\"multiply\") ? 0"), "detail_function", "explicit"),
    "any-format": (("!= MATERIAL_FORMAT)", "< 0)"), "material_parse", "refused"),
}


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/prism_materials.c")
    constants = source.split("/* ---------- constants */", 1)[1].split("/* ---------- definitions */", 1)[0]
    types = constants + "\n" + structure(source, "json_document") + "\n" + structure(source, "material") + "\n"
    text = ""
    for name in NAMES:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("types.inc", types), ("under_test.inc", text), ("jsmn.h", read("port/third_party/jsmn/jsmn.h")))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("prism_materials_parse", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("prism_materials_parse", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
