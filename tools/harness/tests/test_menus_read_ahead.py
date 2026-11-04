"""The PC menus' files, read on a thread from the start: the first load waits for it, and they are read once."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, mutated, CHECK_FAILED

SOURCE = 'port/linux/src/menu_files.c'


def generated(fault=None):
    source = read(SOURCE)
    state = re.search(r"^static struct halo_menus menus;\n.*?^static int menus_reading;\n", source, re.M | re.S).group(0)
    code = '\n'.join(function(source, name) for name in ('menus_files_reader', 'halo_menus_read_ahead', 'halo_menus_load'))
    if fault == 'join':
        code = mutated(code, '\t\tpthread_join(menus_reader, NULL);\n', '')
    if fault == 'once':
        code = mutated(code, 'if (!menus_read && !menus_reading && ', 'if (!menus_reading && ')
    return (('state.inc', state), ('under_test.inc', code))


def built(fault=None):
    return build('menus_read_ahead', generated(fault))


@pytest.mark.parametrize('case', ['ahead', 'again', 'none'])
def test_read_ahead(case):
    status, output = run(built(), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('join', 'ahead'), ('once', 'again')])
def test_negative(fault, case):
    status, output = run(built(fault), case)
    assert status == CHECK_FAILED, output
