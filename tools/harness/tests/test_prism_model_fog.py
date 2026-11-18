"""PRISM v4's objects' fog (source/rasterizer/xbox/rasterizer_xbox_models.c's rasterizer_models_port_fog_by_host):
in display.lighting's v4 modes an opaque object's draw leaves its fog to the host's fog pass over the G-buffer only
while that pass is still to come in the window (render.h's render_prism_deferred_fog_pending, which render_window
clears as it sends the pass); the sky's models, a blended part and every model drawn after the pass (a partly
cloaked unit's with the transparent geometry, the game engine's after the objects) keep their own, as in Classic."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ['before', 'after', 'kept', 'classic']


def generated(fault=None):
    code = function(read('source/rasterizer/xbox/rasterizer_xbox_models.c'), 'rasterizer_models_port_fog_by_host')
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('prism_model_fog', generated()), case)
    assert status == 0, output


FAULTS = [
    # (the whole window's flag: a model drawn after the host's fog pass loses its own fog, and nothing fogs it)
    ('return render_prism_deferred_fog_pending &&', 'return render_prism_deferred_window &&', 'after'),
    # (the sky's models fogged by the host, which never covers the sky)
    ('&& !local_sky_flag', '', 'kept'),
]


@pytest.mark.parametrize('fault', FAULTS, ids=['window', 'sky'])
def test_negative_control(fault):
    status, output = run(build('prism_model_fog', generated(fault[:2])), fault[2])
    assert status == CHECK_FAILED, output


def test_render_window_clears_it_as_the_pass_is_sent():
    render = read('source/render/render.c')
    # (set with the window's flag, cleared as the window begins and ends)
    assert ('render_prism_deferred_window = prism_deferred && prism_deferred_screen_passes();\n'
            '\t\trender_prism_deferred_fog_pending = render_prism_deferred_window;') in render
    assert render.count('\trender_prism_deferred_fog_pending = FALSE;') == 3
    # (cleared right after the host's fog pass is sent, before every draw that follows it in the window)
    cleared = render.index('\t\t\tprism_deferred_fog();\n\t\t\trender_prism_deferred_fog_pending = FALSE;')
    assert render.index('structure_render_transparent_geometry();') < cleared
    for after in ('game_engine_post_rasterize_objects();', 'rasterizer_transparent_geometry_draw(TRUE);',
                  'rasterizer_transparent_geometry_draw(FALSE);', 'structure_render_fog_screen();'):
        assert cleared < render.index(after), after
