"""A map's enumeration: each model drawn with each of its effects once a pass, every draw the tags ask for made."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, enum_with, constant, mutated, CHECK_FAILED

SOURCE = 'port/linux/game/render_enumerate.c'


def generated(fault=None):
    source = read(SOURCE)
    table = re.search(r"^static struct\n\{\n\tshort count;.*?^\} object_effects;", source, re.M | re.S).group(0)
    code = '\n'.join(function(source, name) for name in ('object_effect_drawn', 'object_effects_draw',
                                                         'first_person_model_draw', 'first_person_models_draw'))
    if fault == 'value':
        code = mutated(code, 'object_effects.drawn[index].value == value', 'TRUE')
    if fault == 'reset':
        code = mutated(code, '\tobject_effects.count = 0;\n\ttag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);',
                       '\ttag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);')
    if fault == 'model':
        code = mutated(code, 'object_effect_drawn(model_index, _object_effect_first_person, 0)', 'FALSE')
    if fault == 'full':
        code = mutated(code, '\t\tobject_effects.count++;\n\t}\n\treturn FALSE;',
                       '\t\tobject_effects.count++;\n\t}\n\treturn object_effects.count >= MAXIMUM_OBJECT_EFFECTS;')
    enums = '\n'.join(enum_with(source, member) for member in ('_render_model_effect_type_none',
                                                               '_render_model_immediate_bit', '_object_effect_modifier'))
    enums += '\n#define MAXIMUM_OBJECT_EFFECTS %d\n' % constant(source, 'MAXIMUM_OBJECT_EFFECTS') + table + '\n'
    return (('enums.inc', enums), ('under_test.inc', code))


@pytest.mark.parametrize('case', ['objects', 'passes', 'first-person', 'full'])
def test_object_effects(case):
    status, output = run(build('enumerate_object_effects', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('value', 'objects'), ('reset', 'passes'), ('model', 'first-person'),
                                        ('full', 'full')])
def test_negative(fault, case):
    status, output = run(build('enumerate_object_effects', generated(fault)), case)
    assert status == CHECK_FAILED, output
