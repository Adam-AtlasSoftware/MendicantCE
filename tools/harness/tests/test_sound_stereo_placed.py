"""A stereo sound of the world (sound_manager.c's sound_stereo_placed, #1168): the game's own (an Xbox map's flybys,
Pelican wind and sound scenery, Halo PC's tags on a Custom Edition map) unplaced and faded linearly, as the Xbox played
them; a mod's or loose tag file's replacement, and a Custom Edition map's own, placed and faded as a 3D sound."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ['stock', 'replacement', 'custom-edition']
NEGATIVE_CONTROLS = {
    # every stereo sound placed, as since #1157 (the stock flyby averaged to mono and spatialized)
    'stock-placed': ('return mod_sounds_replaced(definition_index) ||', 'return TRUE ||', 'stock'),
    # a stock flyby unplaced but faded as a 3D sound
    'stock-inverse-fade': ('\tif (!placed)\n', '\tif (FALSE)\n', 'stock'),
    # Halo PC's own tags on a Custom Edition map placed as the map's own
    'halo-pc-tags-placed': (' && !custom_edition_cache_stock_tag(definition_index)', '', 'custom-edition'),
    # replacements left unplaced
    'replacement-unplaced': ('return mod_sounds_replaced(definition_index) ||', 'return FALSE ||', 'replacement'),
}


def generated(fault=None):
    manager = read('source/sound/sound_manager.c')
    code = '\n'.join(function(manager, name) for name in ['sound_stereo_placed', 'sound_stereo_fade'])
    if fault:
        code = mutated(code, fault[0], fault[1])
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('sound_stereo_placed', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    before, after, case = NEGATIVE_CONTROLS[control]
    status, output = run(build('sound_stereo_placed', generated((before, after))), case)
    assert status == CHECK_FAILED, f'{control} escaped {case}: {output}'
