"""Which sources of maps a game has (port/linux/src/map_sources.c), in either order: the Xbox disc image first, then
Custom Edition's installer, or the other way; the game starts once it has its Xbox maps and no later start shows
the maps screen unasked, Settings' ADD MAPS asks for it, and its request is taken once."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

FUNCTIONS = ["map_sources_folder_has", "map_sources_child", "map_sources_read", "map_sources_read_game",
             "map_sources_request_path", "map_sources_request", "map_sources_requested", "map_sources_next",
             "map_sources_screen_at_start", "map_sources_summary"]


def generated(fault=None):
    source = read("port/linux/src/map_sources.c")
    defines = "".join(re.findall(r"^#define MAP_SOURCES_\w+ .*\n", source, re.M))
    start = source.index("static const char *const xbox_files")
    files = source[start:source.index("};", source.index("custom_edition_files[]")) + 2]
    code = "\n".join([read("port/linux/src/map_sources.h"), defines, files,
                      function(read("port/linux/src/map_import.c"), "map_import_custom_maps_folder"),
                      *(function(source, name) for name in FUNCTIONS)])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", ["iso-then-ce", "ce-then-iso", "rerun", "not-custom-edition", "settings-folders"])
def test_map_sources(case):
    status, output = run(build("map_sources", generated()), case)
    assert status == 0, output


def test_negative_control():
    """the maps screen shown only without the Xbox maps, Settings' request ignored (issue 723: Custom Edition's
    installer never offered again once the Xbox maps were in)"""
    fault = ("return !sources->present[_map_source_xbox] || requested;", "return !sources->present[_map_source_xbox];")
    status, output = run(build("map_sources", generated(fault)), "iso-then-ce")
    assert status == CHECK_FAILED, output
