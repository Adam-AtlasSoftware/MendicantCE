"""Settings' volume sliders: a percentage on a 60 dB curve over the amplitude config.toml keeps, old values carried over
(#1106)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["old-values-round-trip", "curve"]
NEGATIVE_CONTROLS = {
    # the percentage read as the old linear amplitude: a saved 0.5 shows at 50%, then writes about 0.03
    "linear-read": (("return 100.0 * (1.0 + log(amplitude) / log(SETTINGS_VOLUME_RANGE));", "return 100.0 * amplitude;"),
                    "old-values-round-trip"),
}


def generated(fault=None):
    menus = read("port/linux/game/menu_functions.c")
    defines = "\n".join(re.search(rf"^#define {name} .*$", menus, re.M).group(0)
                        for name in ["SETTINGS_VOLUME_RANGE", "SETTINGS_VOLUME_KNEE"])
    code = defines + "\n" + "\n".join(function(menus, name)
                                      for name in ["settings_volume_amplitude", "settings_volume_percent"])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("volume_curve", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("volume_curve", generated(fault)), case)
    assert status == CHECK_FAILED, f"{control} escaped {case}: {output}"
