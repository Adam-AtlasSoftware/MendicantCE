"""The performance overlay's detailed view (display.perf_overlay "detailed", debug.timings; port/linux/game/
perf_overlay.c): the CPU sections shown by short labels (GAME TICK, not GAME_TIME_UPDATE), every time in one format
(two decimals), and the cells compact in the view's bottom right corner at 4:3, 16:9 and 21:9, from 720p to 4K and in
2 and 4-player split screen. Each with a negative control."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

OVERLAY = "port/linux/game/perf_overlay.c"

NEGATIVE_CONTROLS = {
    # the console's names shown as they are, '_' and all
    "raw-names": ("labels", "label[length++] = *last == '_' ? ' ' : (char)toupper((unsigned char)*last);",
                  "label[length++] = (char)toupper((unsigned char)*last);"),
    # a name's start ('*') taken for a whole name
    "no-starts": ("labels", "pattern[pattern_length - 1] == '*' ?", "0 ?"),
    # one decimal, as some lines had
    "one-decimal": ("milliseconds", '"%.2f ms"', '"%.1f ms"'),
    # not scaled down to fit a small view
    "no-fit": ("fit", "\tif (fit < scale)\n\t\tscale = fit;\n", ""),
    # no least size for a low resolution's capitals
    "no-floor": ("fit", "\tif (pixels > 0.0f && scale * capital_height * pixels < PERF_OVERLAY_CAPITAL_PIXELS)",
                 "\tif (0)"),
    # drawn at the font's own size, as it was
    "full-size": ("compact", "#define PERF_OVERLAY_SCALE 0.45f", "#define PERF_OVERLAY_SCALE 1.0f"),
}


def generated(fault=None):
    header = read("port/linux/game/perf_overlay.h")
    source = read(OVERLAY)
    table = source[source.index("/* the CPU sections' labels:"):source.index("void perf_overlay_section_label(")]
    place = source[source.index("/* ---------- the cells' place */"):]
    code = "\n\n".join([enum_with(header, "PERF_OVERLAY_CELLS"), structure(header, "perf_overlay_place"), table,
                        function(source, "perf_overlay_section_label"), function(source, "perf_overlay_milliseconds"),
                        place])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["labels", "milliseconds", "fit", "compact"])
def test_case(case):
    status, output = run(build("perf_overlay_cells", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("perf_overlay_cells", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_console_keeps_names():
    """the labels are the overlay's alone: the sections' names (the developer console's) are as they were"""
    assert '{ "render_objects", NONE, TRUE }' in read("source/render/render_objects.c")
    assert 'PROFILE_SECTION(main_game_time_update_section, "game_time_update")' in read("source/main/main.c")
    assert "perf_overlay_section_label(names[index]" in function(read(OVERLAY), "perf_overlay_timing_cells")


def test_pass_times_follow_the_setting():
    """debug.timings turns the GPU's passes' timing on and off as it changes (no restart): the front end sends it in
    the render stream whenever the settings change, and the Vulkan backend follows it"""
    device = read("port/linux/src/d3d8_device.c")
    send = function(device, "pass_times_send")
    assert 'config_boolean("debug.timings")' in send and "xgpu_backend_pass_times(frames);" in send
    assert "pass_times_send();" in function(device, "halo_screen_commit")
    assert "CALL(pass_times, value->value);" in read("port/android/host/host_render_stream.c")
    assert "backend.pass_times = stream_pass_times;" in read("port/linux/vulkan/xgpu_vulkan_stream.c")
    follow = function(read("port/linux/vulkan/xgpu_vulkan.c"), "xgpu_vulkan_pass_times_follow")
    assert "host_pass_times = frames;" in follow and "host_pass_times = 0;" in follow
    assert "developer_timings" not in read("port/android/host/host_common.c")
