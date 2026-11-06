"""display.scale_scoped_fov (port/linux/include/zoom_fov.h): a scope's zoom at the player's field of view, the Xbox's
zoom levels off, the player's view magnified as much on; and where the game takes it."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CONTROL = (
    # the half angle scaled linearly, not its tangent: the zoom magnifies the player's view less than the Xbox's
    "return native * (requested / unzoomed);",
    "return tanf(atanf(native) * (atanf(requested) / atanf(unzoomed)));",
    "zoom")


def generated(fault=None):
    text = read("port/linux/include/zoom_fov.h")
    if fault:
        text = mutated(text, *fault)
    return (("under_test.inc", text),)


def test_zoom():
    status, output = run(build("zoom_fov", generated()), "zoom")
    assert status == 0, output
    code = read("port/linux/game/view_fov.c")
    assert "adjusted_tangent = zoom_fov_tangent(native_tangent, unzoomed_tangent, requested_tangent, scoped_tangent,\n" \
        "\t\tscale_scoped);" in code
    assert 'config_boolean("display.scale_scoped_fov")' in code
    assert '{ "display.scale_scoped_fov", _config_boolean, "false", "HALO_SCALE_SCOPED_FOV",' in \
        read("port/linux/src/port_config.c")


def test_negative_control():
    status, output = run(build("zoom_fov", generated(CONTROL[:2])), CONTROL[2])
    assert status == CHECK_FAILED, output
