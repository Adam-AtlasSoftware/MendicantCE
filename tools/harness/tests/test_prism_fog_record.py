"""PRISM v4's fog record (port/linux/game/prism_deferred.c's fog_record): SCENE_FOG as the window's fog has it, with
the eye's densities CE's fog pass and the objects' draws take (the atmosphere's share by the camera's height over the
plane over its opaque distance, 1 where it dominates; the plane's by the camera's depth under it over its opaque
depth), the camera's cluster's plane, no plane without planar fog."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

CASES = ['above', 'inside', 'dominant', 'no_plane']


def generated(fault=None):
    source = read('port/linux/game/prism_deferred.c')
    config = '\n'.join(re.findall(r"^#define (?:FOG_\w+) .*$", source, re.M)) + '\n'
    code = '\n'.join([
        structure(source, 'fog_record'),
        function(source, 'fog_record'),
    ])
    if fault:
        code = mutated(code, *fault)
    return (('config.inc', config), ('render_fog.inc', structure(read('source/render/render_cameras.h'), 'render_fog')),
            ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('prism_fog_record', generated()), case)
    assert status == 0, output


FAULTS = [
    # (the camera's height taken the other way: over the plane as under it)
    ('real distance = eye->x * fog->plane.n.i + eye->y * fog->plane.n.j + eye->z * fog->plane.n.k - fog->plane.d;',
     'real distance = fog->plane.d - eye->x * fog->plane.n.i - eye->y * fog->plane.n.j - eye->z * fog->plane.n.k;',
     'above'),
    # (the atmosphere dominant forgotten)
    ('TEST_FLAG(fog->fog_definition_flags, FOG_ATMOSPHERE_DOMINANT) ? 1.0f :', '', 'dominant'),
]


@pytest.mark.parametrize('fault', FAULTS, ids=['height', 'dominant'])
def test_negative_control(fault):
    status, output = run(build('prism_fog_record', generated(fault[:2])), fault[2])
    assert status == CHECK_FAILED, output
