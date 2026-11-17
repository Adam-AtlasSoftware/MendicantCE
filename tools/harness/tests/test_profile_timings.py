"""debug.timings (#1147): the CPU sections timed in a normal build behind a setting, off by default. Off, a section
reads no clock and calls nothing (profile_sections.h's sections through profile.h's profile_enter, which tests
profile_global_enable; profile.c's profile_frame_start keeps it FALSE); on, they time from the next frame's start, a
section left open as the console turned it off is taken for closed when it is turned on again, and
profile_port_busiest lists the busiest."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

NEGATIVE_CONTROLS = {
    # always timed: the off case's sections read the clock
    "always-on": ("off", "\tprofile_global_enable = profile_port_timings();", "\tprofile_global_enable = TRUE;"),
    # the setting never read: never timed
    "never-on": ("on", "\tprofile_global_enable = profile_port_timings();", "\tprofile_global_enable = FALSE;"),
    # a section left open stays open
    "left-open": ("reopened", "\tif (profile_global_enable && profile_globals.stack_depth)\n"
                  "\t\tprofile_port_sections_closed();", ""),
    # the least busy first
    "order": ("busiest", "milliseconds[place-1]<section_milliseconds", "milliseconds[place-1]>section_milliseconds"),
}


def generated(fault=None):
    profile_h = read("source/cseries/profile.h")
    macros = profile_h[profile_h.index("#define profile_enter(section)"):profile_h.index("/* ---------- structures")]
    sections_h = read("port/linux/include/profile_sections.h")
    defines = "\n".join(line for line in sections_h.splitlines()
                        if re.match(r"#define (PROFILE_SECTION|profile_scope)", line))
    profile_c = read("source/cseries/profile.c")
    code = "\n\n".join([macros, defines, function(profile_c, "profile_port_sections_closed"),
                        function(profile_c, "profile_port_busiest"), function(profile_c, "profile_frame_start")])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["off", "on", "reopened", "busiest"])
def test_case(case):
    status, output = run(build("profile_timings", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("profile_timings", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_sections_compiled_in_every_build():
    """the port's sections are the original's kind in every build, not only the profiling build's"""
    sections_h = read("port/linux/include/profile_sections.h")
    assert "#ifdef HALO_PROFILE" not in sections_h and '#include "cseries/profile.h"' in sections_h


def test_off_by_default():
    config = read("port/linux/src/port_config.c")
    assert re.search(r'\{ "debug\.timings", _config_boolean, "false",', config)
    assert "\tprofile_global_enable = FALSE;" in function(read("source/cseries/cseries.c"), "cseries_initialize")
