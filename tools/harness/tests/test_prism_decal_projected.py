"""PRISM v4's projected decals, the draw's side (source/rasterizer/xbox/rasterizer_xbox_decals.c's
rasterizer_decal_port_projected): in display.lighting's v4 modes a decal of the primary, secondary or light layer
is projected (SCENE_DECAL) with its bitmap's texture rather than drawn; the water's and the alpha-tested layers, a
decal with no bitmap, and one whose bitmap is not the 2D texture its draw's stage asks for (a volume, a cube map: a
map's decal definition may name any bitmap) are drawn as CE draws them, the last with CE's default texture."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

CASES = ['projected', 'type', 'drawn']
SOURCE = 'source/rasterizer/xbox/rasterizer_xbox_decals.c'


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, 'rasterizer_decal_port_projected')
    if fault:
        code = mutated(code, *fault)
    structures = '\n'.join([structure(source, 'decal_datum'), structure(source, 'decal_shader_definition')])
    return (('layers.inc', enum_with(source, '_decal_layer_water')), ('structures.inc', structures),
            ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('prism_decal_projected', generated()), case)
    assert status == 0, output


FAULTS = [
    # (any bitmap's texture projected: a cube map's or a volume's view reaches the host's 2D sampler)
    ('bitmap && bitmap->type == 0 ?', 'bitmap ?', 'type'),
    # (the alpha-tested layer projected, though its draw's stencil is what CE's texture pass leaves out)
    ('local_layer == _decal_layer_alpha_tested ||', '', 'drawn'),
]


@pytest.mark.parametrize('fault', FAULTS, ids=['type', 'alpha_tested'])
def test_negative_control(fault):
    status, output = run(build('prism_decal_projected', generated(fault[:2])), fault[2])
    assert status == CHECK_FAILED, output
