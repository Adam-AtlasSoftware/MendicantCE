"""A map's preparation's record: read back as recorded, malformed or stale records refused."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, enum_with, structure, constant, mutated, CHECK_FAILED

DEVICE = 'port/linux/src/d3d8_device.c'
PREPARED = 'port/linux/game/render_prepared.c'


def generated(fault=None):
    device, prepared = read(DEVICE), read(PREPARED)
    types = '\n'.join((enum_with(device, 'JOURNAL_DRAW = 1'), structure(device, 'journal_object'),
                       structure(device, 'journal_state'), structure(device, 'journal_reader')))
    types += '\n#define JOURNAL_STATE_WORDS (sizeof(struct journal_state) / sizeof(uint32_t))\n'
    types += '#define JOURNAL_SEEN_SLOTS %d\n' % constant(device, 'JOURNAL_SEEN_SLOTS')
    code = '\n'.join(function(device, name) for name in ('journal_reserve', 'journal_append', 'journal_read'))
    checks = '\n'.join((structure(prepared, 'prepared_header'),
                        *(re.search(r'^#define %s .*$' % name, prepared, re.M).group(0)
                          for name in ('PREPARED_MAGIC', 'PREPARED_VERSION', 'PREPARED_MAXIMUM_WORDS')),
                        function(prepared, 'prepared_header_fits'),
                        function(prepared, 'render_prepared_draws_missing')))
    if fault == 'repeats':
        code = mutated(code, '\thash = xgpu_manifest_hash(state, sizeof(*state), hash);\n', '')
    if fault == 'delta':
        code = mutated(code, '\tjournal.last = *state;\n', '')
    if fault == 'type':
        code = mutated(code, '\t\telse\n\t\t{\n\t\t\treturn 0;\n\t\t}\n\t}\n\treturn 1;', '\t}\n\treturn 1;')
    if fault == 'missing':
        checks = mutated(checks, 'if (other == made_count || made[other] != recorded[index])',
                         'if (other == made_count)')
    if fault == 'key':
        checks = mutated(checks, 'header->key == key &&', '')
    return (('journal.inc', types), ('journal_code.inc', code), ('prepared.inc', checks))


@pytest.mark.parametrize('case', ['roundtrip', 'malformed', 'missing', 'stale'])
def test_prepared_journal(case):
    status, output = run(build('prepared_journal', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('repeats', 'roundtrip'), ('delta', 'roundtrip'), ('type', 'malformed'),
                                        ('missing', 'missing'), ('key', 'stale')])
def test_negative(fault, case):
    status, output = run(build('prepared_journal', generated(fault)), case)
    assert status == CHECK_FAILED, output
