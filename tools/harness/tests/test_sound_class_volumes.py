"""Speech plays at audio.dialogue_volume, every other class but music at audio.effects_volume (#1107)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure
from harness.audio import class_table

CASES = ["classes", "changed"]
NEGATIVE_CONTROLS = {
    # speech under the effects volume, as before
    "speech-as-effects": (("->speech ? dialogue_volume", "->speech && FALSE ? dialogue_volume"), "classes"),
}


def generated(fault=None):
    code = function(read("source/sound/sound_manager.c"), "sound_manager_port_volume")
    if fault:
        code = mutated(code, *fault)
    types = structure(read("source/sound/sound_classes.h"), "sound_class_definition") + "\n"
    return (("types.inc", types), ("classes.inc", class_table()), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sound_class_volumes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sound_class_volumes", generated(fault)), case)
    assert status == CHECK_FAILED, f"{control} escaped {case}: {output}"
