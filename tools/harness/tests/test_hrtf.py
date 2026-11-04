"""The HRTF of dsound_sdl.c against the Xbox's (hrtf.c): the time between the ears, their levels apart in each
octave, the pinna's notch and what a sound behind loses, over a grid of directions, and on stereo the Xbox's own
handling of a sound behind and of its front and back mix bins.

    python tools/harness/tests/test_hrtf.py metrics   our figures, beside the Xbox's
(MIXER_SOURCE=path: of another dsound_sdl.c, to compare)

The Xbox's figures in hrtf.c were measured on the Xbox's own filters, which this repository does not hold: the
XDK DirectSound linked into the game's default.xbe, once the game calls DirectSoundUseFullHRTF, works out each
voice's azimuth and elevation (45 degrees times the ratio of the lesser side over the greater in each octant),
rounds them to its grid (6 degrees of elevation; 3, 6 or 12 of azimuth as the elevation rises), and looks up a
pair of 31-tap filters with a whole-frame delay between the ears, which it writes to the MCPX's voice processor
(NV1BA0_PIO_SET_HRIR). Each tap is a byte: a sign, a 3-bit exponent and a 4-bit mantissa (the magnitude the
mantissa below 16, else 16 plus it, doubled each exponent past the first). To measure them again from one's own
default.xbe: find DirectSoundUseFullHRTF (it stores its lookup's address for the 3D calculator to call), run that
lookup as it is (x86, stdcall: azimuth, elevation, the stereo fold, and two pointers it sets to the left and
right ears' filters) for each direction of hrtf.c's grid with the fold off, make each ear's response at 48 kHz
from its 31 taps decoded, after its delay (the filter's last byte: the left ear's when the sound is to the
right), 512 frames each, and give them to "measure" (raw floats, in the grid's order) for its line of
xbox_figures.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, run  # noqa: E402
from harness.tests.test_mixer import generated  # noqa: E402

CASES = ["delays", "levels", "notch", "rear", "stereo"]

# a fault in the HRTF, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the direction's angles as arc tangents, not DirectSound's octants (30 degrees heard at 30, not 26)
    "arc-tangent-angles": (("*azimuth = side / front * 45.0f;", "*azimuth = atanf(side / front) * 57.2957795f;"),
                           "delays"),
    # the ears' delays to a fraction of a frame, not whole ones as the Xbox's
    "fractional-delays": (("(HRTF_HEAD_RADIUS / HRTF_SPEED_OF_SOUND) * OUTPUT_RATE + 0.5f);",
                           "(HRTF_HEAD_RADIUS / HRTF_SPEED_OF_SOUND) * OUTPUT_RATE + 0.25f) + 0.25f;"), "delays"),
    # the head shadow's corner as Brown and Duda's (2c / a, about 1.2 kHz): the ears too far apart at 1 to 4 kHz
    "brown-duda-corner": (("float pole = 2.0f * 3.14159265f * HRTF_SHADOW_CORNER;",
                           "float pole = 2.0f * HRTF_SPEED_OF_SOUND / (HRTF_HEAD_RADIUS * HRTF_DIFFUSE_ALPHA);"),
                          "levels"),
    # no level between the ears below the shelf
    "no-broadband-level": (("#define HRTF_LEVEL_DECIBELS 2.07f", "#define HRTF_LEVEL_DECIBELS 0.0f"), "levels"),
    # the notch's law as it was (6 kHz at 30 below to 11 kHz at 60 above)
    "notch-as-it-was": (("#define HRTF_NOTCH_ELEVATION_LOW -16.0f\n#define HRTF_NOTCH_ELEVATION_HIGH 17.4f",
                         "#define HRTF_NOTCH_ELEVATION_LOW -30.0f\n#define HRTF_NOTCH_ELEVATION_HIGH 60.0f"), "notch"),
    # behind as it was (up to 7 dB above 2.5 kHz)
    "rear-as-it-was": (("#define HRTF_REAR_DEPTH 0.99f", "#define HRTF_REAR_DEPTH 0.55f"), "rear"),
    # on stereo, a sound behind kept behind (not mirrored ahead), or its mix bins' split left out
    "stereo-no-mirror": (("if (fabsf(azimuth) > 90.0f)\n", "if (fabsf(azimuth) > 900.0f)\n"), "stereo"),
    "stereo-no-split": (("level = sqrtf(1.0f - back) + sqrtf(back);", "level = 1.0f;"), "stereo"),
}


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("hrtf", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("hrtf", generated(fault=fault)), case)
    assert status == CHECK_FAILED, f"the HRTF with a fault ({control}) passed '{case}': the test cannot see it"


if __name__ == "__main__":
    import os
    import subprocess

    other = os.environ.get("MIXER_SOURCE")
    executable = build("hrtf", generated(Path(other).read_text(encoding="latin-1") if other else None))
    sys.exit(subprocess.run([str(executable), *(sys.argv[1:] or ["metrics"])]).returncode)
