"""Settings > Video > Frame Rate & Sync's PERFORMANCE OVERLAY (display.perf_overlay, #1146; SHOW FRAME RATE's,
display.show_frame_rate, #512, before it): a setting off by default and kept in config.toml, read again as it changes
(port/linux/game/perf_overlay.c's perf_overlay_mode); "simple" drawn by main_framerate_render as the console's
display_framerate is, "detailed" with the frame's times above it. An older version's display.show_fps or
display.show_frame_rate = true reads as "simple" (port_config.c's config_migrate_frame_rate)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

NEGATIVE_CONTROLS = {
    # read once, as the game started
    "read-once": ("\tif (read_at != config_changes())", "\tif (read_at == (unsigned long)-1)"),
    # "detailed" taken for "simple"
    "detailed-simple": ('!strcmp(word, "detailed") ? _perf_overlay_detailed', '!strcmp(word, "detailed") ? '
                        '_perf_overlay_simple'),
}


def generated(fault=None):
    header = read("port/linux/game/perf_overlay.h")
    enum = header[header.index("enum\n{\n\t_perf_overlay_off"):header.index("};", header.index("_perf_overlay_off")) + 2]
    code = enum + "\n\n" + function(read("port/linux/game/perf_overlay.c"), "perf_overlay_mode")
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault])
    return (("under_test.inc", code + "\n"),)


def test_follows():
    status, output = run(build("perf_overlay", generated()), "follows")
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("perf_overlay", generated(control)), "follows")
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_older_settings_retired():
    """the counter's two older settings are retired, their true read as "simple" before they are taken out"""
    config = read("port/linux/src/port_config.c")
    retired = config[config.index("config_retired_settings[] ="):]
    retired = retired[:retired.index("NULL,")]
    assert '"display.show_fps",' in retired and '"display.show_frame_rate",' in retired
    load = config[config.index("char *frame_rate = config_migrate_frame_rate("):]
    assert load.index("config_migrate_frame_rate(") < load.index("config_migrate_retired(text)")
    migrate = function(config, "config_migrate_frame_rate")
    assert '"\\"simple\\""' in migrate and "set.type != TOML_UNKNOWN" in migrate
