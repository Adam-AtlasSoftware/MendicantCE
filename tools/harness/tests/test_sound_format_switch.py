"""A channel plays an imported permutation kept as 16-bit PCM at its own rate (OpenH1 issue 1111) by switching its one
stream (sound_dsound_xbox.c, dsound_channel_use_format): the stream switched in has the channel's settings, the one
switched out is kept for the switch back, and the channels and the kept streams never come to more than the 256 the
game has, every channel of the desktop's pools switching; a permutation of another format queued after one waits for
its packets to play, and the linked pieces a long permutation is split into (OpenH1 issue 1167) play on one stream,
joined byte for byte, with no switch and no gap."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402
from harness.audio import dsound_port_formats_state, pcm_permutation_rate, stream_types  # noqa: E402

GAME = "source/sound/sound_dsound_xbox.c"
CASES = ["every-channel", "settings", "queued", "chained"]
NEGATIVE_CONTROLS = {
    # no stream let go to make room: the kept streams and the channels come to more than 256
    "nothing-let-go": (("if (dsound_port_formats.kept_count>=most_kept && dsound_port_formats.kept_count>0)",
                        "if (FALSE)"), "every-channel"),
    # the stream switched in not given the channel's settings
    "settings-not-given": (("\tdsound_port_formats.pcm_rates[index]= pcm_rate;\n\tdsound_channel_reapply(index);",
                            "\tdsound_port_formats.pcm_rates[index]= pcm_rate;"), "settings"),
    # the next permutation's format not looked at: PCM sent to the Xbox ADPCM stream
    "no-wait": (("\t\t\t\t\t\t\tdsound_port_formats.switch_pending[index]= TRUE;", ""), "queued"),
    # a one-sample gap at a join of a split permutation's pieces (#1167): the next piece starts a sample in
    "gap-at-join": (("\t\t\t\t\t\tchannel->queued_permutation= NULL;\n\t\t\t\t\t\tchannel->sample_offset= 0;",
                     "\t\t\t\t\t\tchannel->queued_permutation= NULL;\n\t\t\t\t\t\tchannel->sample_offset= 2;"), "chained"),
}

FUNCTIONS = ["dsound_volume_from_gain", "dsound_frequency_from_pitch", "dsound_angle_from_angle",
             "dsound_channel_create_stream", "dsound_channel_set_mix_bins", "dsound_channel_stream_release",
             "dsound_channel_reapply", "dsound_channel_use_format", "dsound_channel_samples_per_second",
             "dsound_channel_block_bytes", "dsound_channel_packet_bytes", "dsound_channel_callback",
             "dsound_channel_queue_packet", "dsound_channel_fill", "dsound_channel_queue_sound"]


def pools():
    """the desktop's actual channels of each type (halo_port_capacity.h) and the types' flags (sound_preferences.c)"""
    capacity = read("port/linux/include/halo_port_capacity.h")
    desktop = capacity[capacity.index("#define HALO_PORT_SOUND_VIRTUAL_CHANNELS"):].split("#else", 1)[1]
    actual = re.search(r"#define HALO_PORT_SOUND_ACTUAL_CHANNELS (\{[\d, ]+\})", desktop).group(1)
    flags = re.search(r"sound_channel_type_flags\[4\] = (\{[\d, ]+\});", read("source/sound/sound_preferences.c")).group(1)
    return f"#define DESKTOP_ACTUAL_CHANNELS {actual}\n#define SOUND_CHANNEL_TYPE_FLAGS {flags}\n"


def generated(fault=None):
    game = read(GAME)
    xdk = read("port/include/xdk/xdk_dsound.h")
    types = stream_types()
    types += structure(read("source/tag_files/tag_groups.h"), "tag_data") + "\n"
    types += structure(read("source/sound/sound_definitions.h"), "sound_permutation") + "\n"
    types += enum_with(game, "_sound_channel_3d_bit") + "\n"
    for name in ["MAXIMUM_SOUND_PACKETS", "MAXIMUM_SOUND_PACKET_SIZE", "MAXIMUM_COMPRESSED_SOUND_PACKET_SIZE",
                 "SOUND_COMPRESSED_BLOCK_SIZE", "SOUND_COMPRESSED_SAMPLES_PER_BLOCK", "MAXIMUM_DSOUND_MIXBINS"]:
        types += f"#define {name} {constant(game, name)}\n"
    types += "#define SOUND_CACHE_SIZE (1 << 20)\n"
    for name in ["WAVE_FORMAT_PCM", "WAVE_FORMAT_XBOX_ADPCM", "DSBVOLUME_MIN", "DSBFREQUENCY_MIN", "DSBFREQUENCY_MAX",
                 "DS3DMODE_NORMAL", "DS3DMODE_DISABLE", "DS3D_DEFERRED", "DS3D_IMMEDIATE", "DSMIXBIN_FRONT_LEFT",
                 "DSMIXBIN_FRONT_RIGHT", "DSMIXBIN_FRONT_CENTER", "DSMIXBIN_BACK_LEFT", "DSMIXBIN_BACK_RIGHT",
                 "DSMIXBIN_FXSEND_0", "DSMIXBIN_FXSEND_1", "DSSPEAKER_ENABLE_AC3", "DSSTREAMCAPS_CTRL3D",
                 "DSERR_OUTOFMEMORY"]:
        types += next(line for line in xdk.splitlines() if line.startswith(f"#define {name} ")) + "\n"
    for name in ["cache_base_address", "DSOUND_POINT3D", "DSOUND_VECTOR3D"]:
        types += re.search(rf"^#define {name}\b.*$", game, re.M).group(0) + "\n"
    types += pcm_permutation_rate() + dsound_port_formats_state() + pools()
    prototypes = game[game.index("static unsigned long dsound_channel_samples_per_second(\n\tshort index);"):
                      game.index("static void dsound_channel_stream_release(\n\tLPDIRECTSOUNDSTREAM stream);")]
    prototypes += "static void dsound_channel_stream_release(LPDIRECTSOUNDSTREAM stream);\n"
    prototypes += "static void dsound_channel_reapply(short index);\n"
    offset = re.search(r"^static long dsound_queue_sample_offset=.*;$", game, re.M).group(0)
    code = prototypes + offset + "\n" + "\n".join(function(game, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sound_format_switch", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sound_format_switch", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
