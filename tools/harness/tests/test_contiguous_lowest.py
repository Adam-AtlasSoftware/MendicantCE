"""The contiguous window's bottom-up blocks (PLATFORM_LOWEST_PHYSICAL_ADDRESS): the enhanced world's geometry of the
next BSP starts where the BSP before's did, whatever top-down blocks came between; blocks in the way passed whole,
alignment kept, a full window refused."""
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, mutated, CHECK_FAILED

MEMORY = 'port/linux/src/xbox_memory.c'


def generated(fault=None):
    memory = read(MEMORY)
    code = '\n'.join(function(memory, name) for name in ('protection_to_host', 'platform_is_contiguous', 'pages_free', 'pages_used_highest',
                                                          'platform_contiguous_alloc', 'platform_contiguous_free'))
    if fault == 'top-down':
        # (the block asked lowest placed top down, as every block was)
        code = mutated(code, '\tif (physical_address == PLATFORM_LOWEST_PHYSICAL_ADDRESS)\n',
                       '\tif (0 && physical_address == PLATFORM_LOWEST_PHYSICAL_ADDRESS)\n')
    if fault == 'unaligned':
        code = mutated(code, '\t\t\tcandidate = (candidate + alignment_pages - 1) / alignment_pages * alignment_pages;\n',
                       '')
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', ['switch', 'passes-blocks', 'aligned', 'full'])
def test_contiguous_lowest(case):
    status, output = run(build('contiguous_lowest', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('top-down', 'switch'), ('top-down', 'passes-blocks'),
                                        ('unaligned', 'aligned')])
def test_negative(fault, case):
    status, output = run(build('contiguous_lowest', generated(fault)), case)
    assert status == CHECK_FAILED, output
