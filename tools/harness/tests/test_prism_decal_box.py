"""PRISM v4's projected decals (port/linux/game/prism_deferred.c): a decal's box from its projection as decals.c makes
it (the extent's middle on the surface, its halves across and up, its reach out of the surface), the SCENE_DECAL
record sent in its draw's place (its sprite's rectangle, CE's colour and intensity, its blend and layer), and none
for a slot another decal has taken, a box of the map before, a decal with no box, no texture or a blend CE has not."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run, structure

CASES = ['box', 'stale', 'unloaded', 'refused']


def generated(fault=None):
    source = read('port/linux/game/prism_deferred.c')
    config = '\n'.join([
        f"#define DECALS_MOST {constant(source, 'DECALS_MOST')}",
        re.search(r"^#define DECAL_ALPHA_TESTED .*$", source, re.M).group(0),
    ]) + '\n'
    code = '\n'.join([
        structure(source, 'decal_record'),
        structure(source, 'decal_box'),
        'static struct decal_box decal_boxes[DECALS_MOST];',
        function(source, 'prism_deferred_decal_made'),
        function(source, 'prism_deferred_map_unloaded'),
        function(source, 'prism_deferred_decal'),
    ])
    if fault:
        code = mutated(code, *fault)
    return (('config.inc', config), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('prism_decal_box', generated()), case)
    assert status == 0, output


FAULTS = [
    # (the box's centre the projection's origin, not the extent's middle)
    ('decal_boxes[absolute].centre[0] = basis->position.x + basis->forward.i * across + basis->left.i * up;',
     'decal_boxes[absolute].centre[0] = basis->position.x;', 'box'),
    # (a slot's box taken for whichever decal is in it)
    ('decal_boxes[absolute].handle != decal_index ||', '', 'stale'),
    # (the boxes kept past the map)
    ('decal_boxes[absolute].handle = NONE;', '', 'unloaded'),
]


@pytest.mark.parametrize('fault', FAULTS, ids=['centre', 'handle', 'unloaded'])
def test_negative_control(fault):
    status, output = run(build('prism_decal_box', generated(fault[:2])), fault[2])
    assert status == CHECK_FAILED, output
