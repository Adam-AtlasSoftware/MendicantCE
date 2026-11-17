"""demo.c's recording listener (demo_set_recording_listener): told the start with the demo's name and first tick and
the stop with its last tick; with none set, nothing is told."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

SOURCE = "port/linux/game/demo.c"
CASES = ["start_stop", "unset", "two"]
FUNCTIONS = ("demo_add_recording_listener", "demo_remove_recording_listener", "demo_set_recording_listener",
             "demo_recording_tell", "demo_record_stop")

# a fault in demo.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "stop-not-told": (("\tdemo_recording_tell(0, NULL, demo_recorder.last_tick);\n", ""), "demo_record_stop",
                      "start_stop"),
    "stop-told-the-wrong-tick": (("demo_recording_tell(0, NULL, demo_recorder.last_tick)",
                                  "demo_recording_tell(0, NULL, 0)"), "demo_record_stop", "start_stop"),
    "removed-kept": (("\t\tif (demo_listeners[index] != listener)\n", "\t\tif (TRUE)\n"),
                     "demo_remove_recording_listener", "unset"),
    "a-second-replaces-the-first": (("\t\tif (!demo_listeners[index])\n", "\t\tif (TRUE)\n"),
                                    "demo_add_recording_listener", "two"),
    "added-twice-told-twice": (("\t\tif (demo_listeners[index] == listener)\n\t\t\treturn TRUE;\n", ""),
                               "demo_add_recording_listener", "two"),
}


def generated(fault=None, faulty_function=None):
    source = read(SOURCE)
    match = re.search(r"^static demo_recording_listener demo_listeners\[DEMO_RECORDING_LISTENERS\];\n", source, re.M)
    assert match, "demo_listeners not found"
    text = match.group(0)
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("demo_recording_listener", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("demo_recording_listener", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"demo.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_start_tells_the_name_and_first_tick():
    """demo_record_start tells the listener once the recording has begun, with its name and the tick it began at"""
    start = function(read(SOURCE), "demo_record_start")
    begun = start.index("demo_recorder.recording = TRUE;")
    told = start.index("demo_recording_tell(1, demo_recorder.name, tick);")
    assert told > begun
    assert start.count("demo_recording_tell(") == 1
