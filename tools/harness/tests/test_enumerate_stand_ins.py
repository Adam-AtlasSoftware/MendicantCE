"""A map's first frame's stand-ins: every material's shader and every lightmap's picture given back once the rounds
end, even where the last round stood in no material (the lightmap chosen to draw without its picture then)."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, structure, constant, mutated, CHECK_FAILED

SOURCE = 'port/linux/game/render_enumerate.c'
FIX = ('\tif (!stand_ins.count && stand_ins.unlit)\n\t{\n\t\tstand_ins.unlit->bitmap_index = stand_ins.unlit_bitmap_index;'
       '\n\t\tstand_ins.unlit = NULL;\n\t}\n')


def generated(fault=None):
    source = read(SOURCE)
    table = re.search(r"^static struct\n\{\n\t/\* the keys still to draw, and their shaders \*/.*?^\} stand_ins;",
                      source, re.M | re.S).group(0)
    code = '\n'.join(function(source, name) for name in ('material_key', 'materials_have', 'materials_add',
                                                         'stand_ins_restore', 'stand_ins_next'))
    if fault == 'leak':
        code = mutated(code, FIX, '')
    if fault == 'restore':
        code = mutated(code, '\tif (stand_ins.unlit)\n\t\tstand_ins.unlit->bitmap_index = stand_ins.unlit_bitmap_index;\n',
                       '')
    enums = '#define MAXIMUM_ENUMERATED_MATERIALS %d\n' % constant(source, 'MAXIMUM_ENUMERATED_MATERIALS')
    enums += '#define MAXIMUM_STAND_IN_ROUNDS %d\n' % constant(source, 'MAXIMUM_STAND_IN_ROUNDS')
    enums += re.search(r"^#define MATERIAL_KEY_MIRROR .*$", source, re.M).group(0) + '\n'
    enums += re.search(r"^#define MATERIAL_KEY_CLASS\(key\) .*$", source, re.M).group(0) + '\n'
    enums += structure(source, 'enumerated_materials') + '\n'
    return (('enums.inc', enums), ('under_test.inc', table + '\n' + code))


@pytest.mark.parametrize('case', ['none-stands-in', 'one-round', 'mixed', 'pictured'])
def test_stand_ins(case):
    status, output = run(build('enumerate_stand_ins', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('leak', 'none-stands-in'), ('leak', 'mixed'), ('restore', 'one-round')])
def test_negative(fault, case):
    status, output = run(build('enumerate_stand_ins', generated(fault)), case)
    assert status == CHECK_FAILED, output
