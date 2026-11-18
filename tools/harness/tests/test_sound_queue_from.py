"""A sound queued from a byte into its samples: idle channels only, at a block's start, never the next sound's."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure
from harness.audio import dsound_port_formats_state, pcm_permutation_rate, stream_types

CASES = ['from', 'uncompressed', 'next-sound', 'no-channel', 'playing', 'outside', 'pcm']
NEGATIVE_CONTROLS = {
    'kept-for-the-next': ('\tdsound_queue_sample_offset= 0;\n\n\treturn;', '\n\treturn;', 'next-sound'),
    'kept-without-a-channel': ('\tdsound_queue_sample_offset= 0;\n\n\treturn;', '\n\treturn;', 'no-channel'),
    'mid-block': ('dsound_queue_sample_offset-dsound_queue_sample_offset%block_size', 'dsound_queue_sample_offset',
                  'from'),
    'mono-blocks': ('2*channels : SOUND_COMPRESSED_BLOCK_SIZE*channels;', '2*channels : SOUND_COMPRESSED_BLOCK_SIZE;',
                    'from'),
    # a permutation kept as 16-bit PCM started on a block of Xbox ADPCM, or its stream left as it was
    'pcm-as-adpcm': ('dsound_port_formats.pcm_rates[index] ? 2*channels :', '0 ? 2*channels :', 'pcm'),
    'format-kept': ('if (!dsound_channel_use_format(index, sound_permutation_pcm_rate(sound)))',
                    'if (!dsound_channel_use_format(index, 0))', 'pcm'),
    'past-the-sound': ('dsound_queue_sample_offset<sound->samples.size', 'TRUE', 'outside'),
}


def generated(fault=None):
    game = read('source/sound/sound_dsound_xbox.c')
    types = stream_types()
    types += structure(read('source/tag_files/tag_groups.h'), 'tag_data') + '\n'
    types += structure(read('source/sound/sound_definitions.h'), 'sound_permutation') + '\n'
    types += enum_with(game, '_sound_channel_3d_bit') + '\n'
    types += f"#define SOUND_COMPRESSED_BLOCK_SIZE {constant(game, 'SOUND_COMPRESSED_BLOCK_SIZE')}\n"
    types += pcm_permutation_rate() + dsound_port_formats_state()
    offset = re.search(r'^static long dsound_queue_sample_offset=.*;$', game, re.M)
    assert offset, 'dsound_queue_sample_offset not found in sound_dsound_xbox.c'
    code = offset.group(0) + '\n'
    code += '\n'.join(function(game, name) for name in ['dsound_channel_block_bytes', 'dsound_channel_queue_sound',
                                                         'dsound_virtual_queue_sound', 'dsound_port_queue_sound_from'])
    if fault:
        code = mutated(code, fault[0], fault[1])
    return (('types.inc', types), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('sound_queue_from', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    before, after, case = NEGATIVE_CONTROLS[control]
    status, output = run(build('sound_queue_from', generated((before, after))), case)
    assert status == CHECK_FAILED, f'{control} escaped {case}: {output}'
