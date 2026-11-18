"""A map's preparation's record played back never reads memory let go (#1169): a draw from a block freed since is
skipped and counted unknown, one from a block still there is drawn, and a named geometry moved between two loads is
found where it is now."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, mutated, CHECK_FAILED

DEVICE = 'port/linux/src/d3d8_device.c'
MEMORY = 'port/linux/src/xbox_memory.c'


def generated(fault=None):
    device, memory = read(DEVICE), read(MEMORY)
    code = '\n'.join(function(memory, name) for name in ('protection_to_host', 'platform_is_contiguous',
                                                          'platform_contiguous_readable'))
    names = re.search(r'^#define GEOMETRY_NAMES .*?^static uint32_t journal_lost_streams;\n', device, re.M | re.S).group(0)
    code += '\n' + names + '\n'.join(function(device, name) for name in (
        'geometry_name_hash', 'd3d8_geometry_named', 'd3d8_geometry_unnamed', 'journal_stream_keep',
        'journal_stream_place', 'journal_streams_usable'))
    if fault == 'unchecked':
        # (the playback before #1169: the stream's memory read whatever it is now)
        code = mutated(code, '\t\tif (!platform_contiguous_readable(', '\t\tif (0 && !platform_contiguous_readable(')
    if fault == 'raw':
        # (a named geometry's stream kept by its address, as before)
        code = mutated(code, '\t\tif (geometry_names[index].name && address >= geometry_names[index].base &&',
                       '\t\tif (0 && geometry_names[index].name && address >= geometry_names[index].base &&')
    return (('streams.inc', code),)


@pytest.mark.parametrize('case', ['freed', 'allocated', 'moved', 'lost'])
def test_journal_streams(case):
    status, output = run(build('journal_streams', generated()), case)
    assert status == 0, output


def test_negative_unchecked_faults():
    """without the check, the draw from a block let go reads a page that is not there: the process faults"""
    status, output = run(build('journal_streams', generated('unchecked')), 'freed')
    assert status < 0, f'{status}: {output}'


def test_negative_raw_address():
    """a named geometry kept by its address: after it moves, its draw is lost"""
    status, output = run(build('journal_streams', generated('raw')), 'moved')
    assert status == CHECK_FAILED, output
