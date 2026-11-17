"""An impulse sound plays on through the linked pieces of 16-bit PCM a long permutation is split into (OpenH1 issue
1167), each queued while the one before plays; the Xbox's own linked permutations (Xbox ADPCM) play as before."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402
from harness.audio import manager_types, pcm_permutation_rate  # noqa: E402

CASES = ["pieces", "cache-miss", "stock-adpcm", "unlinked", "unlinked-actual"]
NEGATIVE_CONTROLS = {
    # the pieces not followed: an impulse sound plays its first piece only
    "not-followed": (("\tupdate_channel_for_impulse_sound_piece(channel_index);\n", ""), "pieces"),
    # any linked chain followed: the Xbox's own impulse sounds would play on past their first permutation
    "any-chain": (("\t\t!sound_permutation_pcm_rate(channel->playing_permutation) ||\n", ""), "stock-adpcm"),
}


def generated(fault=None):
    manager = read("source/sound/sound_manager.c")
    code = "\n".join(function(manager, name) for name in ["update_channel_for_impulse_sound_piece",
                                                          "update_channel_for_impulse_sound"])
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", manager_types()), ("sound_pcm_rate.inc", pcm_permutation_rate()), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sound_impulse_pieces", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sound_impulse_pieces", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
