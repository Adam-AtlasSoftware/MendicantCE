"""Halo PC and mod sounds keep their own quality (OpenH1 issues 1110 and 1111): custom_edition_sounds.c keeps a
permutation the channels would not play as 16-bit PCM at its own rate, sample for sample (a 16 kHz tone is not folded
down, a 48 kHz stream stays at 48 kHz); one too long for that is Xbox ADPCM at a rate the channels play, resampled
band-limited (a tone above the new rate's band is gone, one inside it keeps its level)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, mutated, read, run, structure  # noqa: E402
from harness.audio import pcm_permutation_rate  # noqa: E402

MODULE = "port/linux/game/custom_edition_sounds.c"
CASES = ["high-tone", "48k-pcm", "too-long", "resource-entry"]
NEGATIVE_CONTROLS = {
    # the old conversion path (#1111): every permutation re-encoded as Xbox ADPCM at a rate the channels play
    "always-adpcm": (("if (decoded && frames_kept_native(", "if (decoded && FALSE && frames_kept_native("),
                     "high-tone"),
    # Halo PC's hit sound (#673) at its own rate, not its tag's pitch
    "no-pitch": (("\tpitch = base * one;", "\tpitch = base;"), "resource-entry"),
    # the old resampler (#1110): the nearest source frame, no low pass
    "nearest-frame": (("double weight = resample_weight((at - source) * cutoff);",
                       "double weight = source == (long)at ? 1.0 / cutoff : 0.0;"), "too-long"),
}


def generated(fault=None):
    definitions = read("source/sound/sound_definitions.h")
    tags = read("source/tag_files/tag_groups.h")
    types = "".join(structure(tags, name) + "\n" for name in ["tag_block", "tag_reference", "tag_data"])
    types += "".join(structure(definitions, name) + "\n" for name in
                     ["real_bounds", "sound_permutation", "sound_pitch_range", "sound_definition"])
    types += f"#define MAXIMUM_SOUND_DATA_SIZE {constant(definitions, 'MAXIMUM_SOUND_DATA_SIZE')}\n"
    types += "#define SOUND_DEFINITION_TAG 0x736E6421\n"
    # a small sound cache (a quarter of it, 64 KB, is the most PCM a permutation is kept in), so that the case of
    # one too long for it is short
    types += "#define HALO_PORT_SOUND_CACHE_SIZE 0x40000\n"
    types += pcm_permutation_rate()
    module = read(MODULE)
    code = module[module.index("/* ---------- constants */"):]
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("custom_edition_sound_import", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("custom_edition_sound_import", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
