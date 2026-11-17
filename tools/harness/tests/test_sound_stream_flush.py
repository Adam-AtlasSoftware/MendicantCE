"""Cancellation drains SDL packets without refilling the original game channel, and the mixer lets go of them."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run
from harness.audio import dsound_port_formats_state, hand_off_state, stream_types

CASES = ["mixed-head", "unfinished", "all-finished", "paused", "repeat", "ordinary",
         "wrapped", "no-callback", "empty", "taken-during-flush"]
NEGATIVE_CONTROLS = {
    "successful-cancellation": (("stream_complete_head(stream, XMEDIAPACKET_STATUS_FLUSHED,",
        "stream_complete_head(stream, head->finished ? XMEDIAPACKET_STATUS_SUCCESS : XMEDIAPACKET_STATUS_FLUSHED,"), "mixed-head"),
    "does-not-unlock-cache": (("sound_cache_sound_hardware_unlock(packet_context);", ""), "mixed-head"),
    # the mixer's hand-off: the voice not started over after a flush; the packets being flushed taken while the
    # game's callback runs; samples the mixer may hold freed at once; the completed packets kept by the mixer
    "does-not-reset-resampler": (("\t\tstream->cursor = 0;\n\t\tresampler_reset(stream);\n\t\tif (stream->flush == VOICE_FLUSHED)",
        "\t\tstream->cursor = 0;\n\t\tif (stream->flush == VOICE_FLUSHED)"), "unfinished"),
    "flushed-packets-taken": (("if (stream->flush != VOICE_FLUSHING)", "if (1)"), "taken-during-flush"),
    "held-samples-freed": (("\tif (entry->finished)\n\t\tfree(entry->samples);", "\tif (1)\n\t\tfree(entry->samples);"),
        "unfinished"),
    "completed-kept": (("(long)(first - (stream->mixing_next - stream->mixing_count)) > 0", "0"), "ordinary"),
}


def generated(fault=None):
    backend = read("port/linux/src/dsound_sdl.c")
    game = read("source/sound/sound_dsound_xbox.c")
    code = function(game, "dsound_channel_callback") + "\n" + function(game, "channel_stop") + "\n"
    code += "\n".join(function(backend, name) for name in ["voice_settings_take", "voice_finished_publish",
        "voice_hand_off", "retire", "retired_free", "packet_release", "stream_complete_head",
        "streams_complete_finished", "stream_from_interface", "stream_flush"])
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", stream_types()), ("state.inc", hand_off_state() + dsound_port_formats_state()),
            ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sound_stream_flush", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sound_stream_flush", generated(fault)), case)
    assert status == CHECK_FAILED, f"{control} escaped {case}: {output}"
