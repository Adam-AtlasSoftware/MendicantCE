"""A map's preparation's record: read back as recorded, malformed or stale records refused, one record for every
settings."""
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
    code = '\n'.join(function(device, name) for name in ('journal_reserve', 'journal_append', 'journal_read',
                                                          'journal_settings_blind_of', 'journal_occlusion'))
    checks = '\n'.join((structure(prepared, 'prepared_header'),
                        *(re.search(r'^#define %s .*$' % name, prepared, re.M).group(0)
                          for name in ('PREPARED_MAGIC', 'PREPARED_VERSION', 'PREPARED_MAXIMUM_WORDS')),
                        function(prepared, 'prepared_hash_string'),
                        function(prepared, 'render_prepared_key'),
                        function(prepared, 'render_prepared_played'),
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
    if fault == 'blind':
        code = mutated(code, '\twords[2] = (unsigned long)(hdr_view != 0) | (unsigned long)(prepass != 0) << 1 | (unsigned long)(ready != 0) << 2 |\n\t\t(unsigned long)(lights > 0) << 3 | (unsigned long)(environment != 0) << 4;\n', '\twords[2] = 0;\n')
    if fault == 'tags':
        code = mutated(code, '\twords[1] = (unsigned long)hdr_kind;\n', '\twords[1] = 0;\n')
    if fault == 'occlusion':
        code = mutated(code, 'return noted != 0 && quality != 0;', 'return noted != 0;')
    if fault == 'derivation':
        checks = mutated(checks, 'return formed && !unknown && (!missing || !same_derivation);',
                         'return formed && !unknown;')
    if fault == 'scenario':
        checks = mutated(checks, '\treturn prepared_hash_string(scenario, hash);', '\treturn hash;')
    return (('journal.inc', types), ('journal_code.inc', code), ('prepared.inc', checks))


@pytest.mark.parametrize('case', ['roundtrip', 'malformed', 'missing', 'stale', 'settings', 'key'])
def test_prepared_journal(case):
    status, output = run(build('prepared_journal', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('repeats', 'roundtrip'), ('delta', 'roundtrip'), ('type', 'malformed'),
                                        ('missing', 'missing'), ('key', 'stale'), ('blind', 'settings'),
                                        ('tags', 'settings'), ('occlusion', 'settings'), ('derivation', 'key'),
                                        ('scenario', 'key')])
def test_negative(fault, case):
    status, output = run(build('prepared_journal', generated(fault)), case)
    assert status == CHECK_FAILED, output
