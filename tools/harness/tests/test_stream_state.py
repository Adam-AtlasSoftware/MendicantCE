"""The state file of port/linux/game/stream_state.c (display.state_file): its text, written only when it changed and at
most four times a second, and written whole (a file beside it renamed over it); off, the main loop writes nothing."""

import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, structure  # noqa: E402

SOURCE = "port/linux/game/stream_state.c"
CASES = ["text", "changes", "atomic"]
FUNCTIONS = ("stream_state_append_string", "stream_state_name", "stream_state_text", "stream_state_write",
             "stream_state_offer")

# a fault in stream_state.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "written-every-frame": (("\tif (tracker->ever_written && !strcmp(text, tracker->written))\n\t\treturn FALSE;\n",
                             ""), "stream_state_offer", "changes"),
    "no-rate-limit": (("now - tracker->written_at < STREAM_STATE_MILLISECONDS", "0"), "stream_state_offer",
                      "changes"),
    "written-in-place": (('snprintf(temporary, sizeof(temporary), "%s.tmp", path);',
                          'snprintf(temporary, sizeof(temporary), "%s", path);'), "stream_state_write", "atomic"),
    "backslash-unescaped": (("character == '\"' || character == '\\\\'", "character == '\"'"),
                            "stream_state_append_string", "text"),
    "pause-over-cutscene": (("\tif (input->paused)\n\t\treturn \"paused\";\n", ""), "stream_state_name", "text"),
}


def generated(fault=None, faulty_function=None):
    source = read(SOURCE)
    constants = re.search(r"^enum\n\{\n\t/\* the least time between two writes \*/.*?^\};\n", source, re.M | re.S)
    assert constants, "stream_state.c's constants not found"
    text = constants.group(0) + structure(source, "stream_state_input") + "\n" + \
        structure(source, "stream_state_tracker") + "\n"
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


def run_in(executable, case, folder):
    result = subprocess.run([str(executable), case, str(folder)], capture_output=True, text=True, errors="replace",
                            timeout=60)
    return result.returncode, (result.stdout + result.stderr).strip()


@pytest.mark.parametrize("case", CASES)
def test_case(case, tmp_path):
    status, output = run_in(build("stream_state", generated()), case, tmp_path)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, tmp_path):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run_in(build("stream_state", generated(fault, faulty_function)), case, tmp_path)
    assert status == CHECK_FAILED, f"stream_state.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_off_reads_and_writes_nothing():
    """the frame's first act is the setting's check, off by default, before anything is read or written"""
    frame = function(read(SOURCE), "stream_state_frame")
    body = frame[frame.index("{") + 1:]
    first = re.search(r"\n\t(?!struct |char )(\S[^\n]*)", body).group(1)
    assert first == "if (!stream_state_enabled())", first
    assert '{ "display.state_file", _config_boolean, "false",' in read("port/linux/src/port_config.c")
