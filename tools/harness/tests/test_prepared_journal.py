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
                       structure(device, 'draw_inputs'), structure(device, 'journal_state'), structure(device, 'journal_reader')))
    types += '\n#define JOURNAL_STATE_WORDS (sizeof(struct journal_state) / sizeof(uint32_t))\n'
    types += '#define JOURNAL_SEEN_SLOTS %d\n' % constant(device, 'JOURNAL_SEEN_SLOTS')
    types += '#define JOURNAL_PAGE_UNIT %#x\n' % constant(device, 'JOURNAL_PAGE_UNIT')
    code = '\n'.join(function(device, name) for name in ('journal_reserve', 'journal_append', 'journal_runs',
                                                          'journal_read',
                                                          'journal_settings_blind_of'))
    checks = '\n'.join((structure(prepared, 'prepared_header'),
                        *(re.search(r'^#define %s .*$' % name, prepared, re.M).group(0)
                          for name in ('PREPARED_MAGIC', 'PREPARED_VERSION', 'PREPARED_MAXIMUM_WORDS')),
                        function(prepared, 'prepared_hash_string'),
                        function(prepared, 'prepared_key_of'),
                        function(prepared, 'prepared_key'),
                        function(prepared, 'render_prepared_played'),
                        function(prepared, 'prepared_header_fits'),
                        function(prepared, 'render_prepared_draws_missing')))
    if fault == 'repeats':
        code = mutated(code, '\thash = xgpu_manifest_hash(state, sizeof(*state), hash);\n', '')
    if fault == 'delta':
        code = mutated(code, '\tjournal.last = *state;\n', '')
    if fault == 'type':
        code = mutated(code, '\t\telse\n\t\t{\n\t\t\treturn 0;\n\t\t}\n\t}\n\treturn 1;', '\t}\n\treturn 1;')
    if fault == 'numbers':
        # (the old form: the writer's own pages' numbers)
        code = mutated(code, '(uint32_t)(page * (page_size / JOURNAL_PAGE_UNIT))', '(uint32_t)page')
        code = mutated(code, '(uint32_t)(end * (page_size / JOURNAL_PAGE_UNIT))', '(uint32_t)end')
    if fault == 'own_pages':
        # (the old reader: the words as the reader's own pages, up to its own count)
        code = mutated(code, 'words[position + 1] > window_size / JOURNAL_PAGE_UNIT',
                       'words[position + 1] > window_size / page_size')
        code = mutated(code, 'first = words[position] * JOURNAL_PAGE_UNIT / page_size;', 'first = words[position];')
        code = mutated(code, 'end = (words[position + 1] * JOURNAL_PAGE_UNIT + page_size - 1) / page_size;',
                       'end = words[position + 1];')
    if fault == 'floor':
        code = mutated(code, '(words[position + 1] * JOURNAL_PAGE_UNIT + page_size - 1) / page_size',
                       'words[position + 1] * JOURNAL_PAGE_UNIT / page_size')
    if fault == 'shared':
        code = mutated(code, '\t\t\t\tif (first < given)\n\t\t\t\t\tfirst = given;\n', '')
    if fault == 'missing':
        checks = mutated(checks, 'if (other == made_count || made[other] != recorded[index])',
                         'if (other == made_count)')
    if fault == 'key':
        checks = mutated(checks, 'header->key == key &&', '')
    if fault == 'blind':
        code = mutated(code, '\twords[2] = (unsigned long)(of->hdr_view != 0) | (unsigned long)(of->lights_count > 0) << 1 |\n\t\t(unsigned long)(of->lights_environment != 0) << 2 | (unsigned long)of->world_span << 3;\n', '\twords[2] = 0;\n')
    if fault == 'soft':
        # (a record made with display.soft_particles off: an effect's soft group and its other draws merged)
        code = mutated(code, '\twords[2] |= (unsigned long)(of->soft_group != 0) << 16;\n', '')
    if fault == 'span':
        # (a record made with the setting off: a found window's draw and a mirror's merged)
        code = mutated(code, ' | (unsigned long)of->world_span << 3;', ';')
    if fault == 'tags':
        code = mutated(code, '\twords[1] = (unsigned long)of->hdr_kind;\n', '\twords[1] = 0;\n')
    if fault == 'derivation':
        checks = mutated(checks, 'return formed && !unknown && (!missing || !same_derivation);',
                         'return formed && !unknown;')
    if fault == 'scenario':
        checks = mutated(checks, 'return prepared_hash_string(scenario, xgpu_manifest_hash(words, sizeof(words), XGPU_MANIFEST_HASH_START));',
                         'return xgpu_manifest_hash(words, sizeof(words), XGPU_MANIFEST_HASH_START);')
    return (('journal.inc', types), ('journal_code.inc', code), ('prepared.inc', checks))


@pytest.mark.parametrize('case', ['roundtrip', 'malformed', 'pages', 'missing', 'stale', 'settings', 'key'])
def test_prepared_journal(case):
    status, output = run(build('prepared_journal', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('repeats', 'roundtrip'), ('delta', 'roundtrip'), ('type', 'malformed'),
                                        ('missing', 'missing'), ('key', 'stale'), ('blind', 'settings'),
                                        ('tags', 'settings'), ('span', 'settings'), ('soft', 'settings'),
                                        ('derivation', 'key'),
                                        ('scenario', 'key'), ('numbers', 'pages'), ('own_pages', 'pages'),
                                        ('floor', 'pages'), ('shared', 'pages')])
def test_negative(fault, case):
    status, output = run(build('prepared_journal', generated(fault)), case)
    assert status == CHECK_FAILED, output


# The front end's own state that a draw's pipeline or its shaders' variant are derived from, past the render and
# texture states (prepare_draw, bind_textures): each must be journaled as the draw is (journal_capture), set again
# as it plays (journal_restore) and given back after (halo_gpu_journal_replay), or a played record makes other
# pipelines than the walk did and play makes the walk's, missed (an effect's soft group, display.soft_particles; the
# shadows' atlas regions; the high-res text's atlas, whose distance fields a stand-in for it has not; a view of the
# screen's size, recorded on a display of another shape).
CARRIED = {
    # (the front end's own inputs, whole: struct draw_inputs, the soft group and the texture regions among them)
    'inputs': ('journal_capture', 'state->inputs = inputs;'),
    'inputs set': ('journal_restore', 'inputs = state->inputs;'),
    'inputs kept': ('halo_gpu_journal_replay', 'saved.inputs = inputs;'),
    'inputs given back': ('halo_gpu_journal_replay', 'inputs = saved.inputs;'),
    'text atlas named': ('journal_identify', 'if (object == text_hires_atlas_object())'),
    'text atlas found': ('journal_resolve', 'object->value == 2 ? (void *)text_hires_atlas_object() : NULL;'),
    # (a view over the device's targets the screen's size, played back at the screen's size now: a screen's target,
    # in the frame's format, whatever the shape of the display the record was made on)
    'screen view': ('journal_identify', 'to->value |= JOURNAL_VIEW_SCREEN;'),
    'screen view sized': ('journal_view', '(unsigned long)halo_screen_width(), SCREEN_HEIGHT);'),
}


def uncarried(device):
    """the draws' inputs (CARRIED) a record would not carry"""
    return [name for name, (where, line) in CARRIED.items() if line not in function(device, where)]


def test_draw_inputs_journaled():
    assert uncarried(read(DEVICE)) == []


@pytest.mark.parametrize('name', ['inputs', 'inputs set', 'text atlas named', 'screen view'])
def test_draw_inputs_negative(name):
    device = read(DEVICE)
    line = CARRIED[name][1]
    assert uncarried(mutated(device, line, '')) == [name]


# ---------- every input of a draw's pipeline kept in struct draw_inputs

# The game's calls into the front end (d3d8_device.c's halo_* and d3d8_*) that keep state of their own, outside struct
# draw_inputs, and why that state is no input of a draw's pipeline the journal must carry (or how it carries it). A
# new call that keeps state elsewhere fails test_draw_inputs_kept until it keeps it in struct draw_inputs (which the
# journal records whole) or is named here with its reason.
KEPT_ELSEWHERE = {
    # (fork: PRISM's draw state, which its modes alone set: the journal records Classic's and Enhanced's draws, and
    # PRISM's pipelines are made as its frames draw them, docs/prism_materials.md, docs/prism_rt.md)
    'd3d8_set_material_overlay': "PRISM's material overlay and model light: its modes' pipelines, made as they draw",
    'd3d8_set_material_pbr': "PRISM's materials: its modes' pipelines, made as they draw",
    'd3d8_materials_reset': "PRISM's materials let go of at a map's end",
    'halo_prism_shadows_pass': "PRISM's cascaded sun shadows' pass: its own pass, made as it draws",
    'halo_world_ao_object': "the ray-traced scene's owner: a uniform (world_object), no key",
    'halo_world_ao_cards': "the cards' flag: a uniform (world_object), no key",
    'halo_shadow_map_scale': "display.shadow_resolution's scale: the shadow maps' size, made as a record plays",
    'halo_screen_ui_offset': "the menus' centring: the viewport, dynamic state",
    'halo_screen_ui_offset_to': "the menus' centring: the viewport, dynamic state",
    'halo_screen_ui_pointer_view': "the pointer's mapping, no draw's",
    'halo_gpu_preparation_loaded': "the preparation's texture budget",
    'halo_screen_commit': "the screen's size and the settings taken up between frames: the record's derivation",
    'd3d8_viewport_depth_range': "the viewport's depth range: dynamic state, and the journal's viewport",
    'halo_model_bounds': "the model's bounds: the cut-out prepass and the lights' reach, decided as each frame draws; "
                         "a map's preparation draws both (draw_indexed_cutout) and journal_restore lights its models",
    'halo_gpu_prepare': "the preparation's own bookkeeping",
    'halo_gpu_prepared': "the preparation's own bookkeeping",
    'd3d8_mirror_rewritten': "the mirror's pages, journaled as JOURNAL_PAGES",
    'd3d8_clear_viewport_alone': "a clear, journaled as JOURNAL_CLEAR",
    'halo_world_ao_prepass_begin': "the pre-pass's pass, journaled as its operation",
    'halo_world_ao_prepass_end': "the pre-pass's pass, journaled as its operation",
    'halo_view_begin': "an extra view's targets: the journal's objects (views of the device's targets)",
    'halo_view_end': "an extra view's targets: the journal's objects",
    'halo_view_sample': "a view sampled: its target as a stage's texture, the journal's objects",
    'halo_view_release': "an extra view's targets let go",
    # (display.upscaler (PRISM v4 M5, prism_upscale.c): never on under Classic's lighting, whose draws the journal
    # records; the view's targets drawn smaller are the screen's at another scale (the viewport, dynamic state))
    'halo_upscale_view_begin': "display.upscaler's view drawn smaller: never under Classic's lighting, which the "
                               "journal records; its targets the screen's at another scale",
    'halo_upscale_view_end': "display.upscaler's view upscaled: never under Classic's lighting",
    'halo_screenshot_png': "a screenshot", 'halo_screenshot_png_taken': "a screenshot",
    'halo_screenshot_named': "a screenshot", 'halo_vr_test_screenshot': "a screenshot",
    'halo_vr_pass': "the VR mode's, which records no preparation (render_prepared.c's prepared_enabled)",
    'halo_vr_draw_eye': "the VR mode's", 'halo_vr_mirrored_draws': "the VR mode's",
    'halo_vr_effect_depth': "the VR mode's", 'halo_vr_hud_group': "the VR mode's", 'halo_vr_hud_anchor': "the VR mode's",
    'd3d8_resource_created': "the resources' table, the journal's objects' numbers",
    'd3d8_resource_freed': "the resources' table",
    'halo_gpu_journal_record': "the journal's own", 'halo_gpu_journal_texture': "the journal's own",
    'halo_gpu_journal_take': "the journal's own", 'halo_gpu_journal_replay': "the journal's own",
}


def kept_outside(device):
    """the game's calls into the front end that keep state in the file's statics other than struct draw_inputs
    (its instance, inputs): {call: the statics}"""
    statics = set(re.findall(r"^static (?!inline)[^(;{=]*?\b(\w+)\s*(?:\[[^\]]*\])*\s*(?:=[^;]*)?;", device, re.M))
    statics |= set(re.findall(r"^} (\w+)(?: = [^;]*)?;", device, re.M))
    found = {}
    for match in re.finditer(r"^(?!static)[\w ]+?\**\s?\b((?:halo|d3d8)_\w+)\(([^)]*)\)\n\{", device, re.M):
        depth, at = 1, match.end()
        while depth:
            depth += {'{': 1, '}': -1}.get(device[at], 0)
            at += 1
        body = device[match.end():at]
        writes = set(re.findall(r"(?:^\s*|[;{(]\s*)(\w+)(?:\.\w+|\[[^\]]*\])*\s*(?:[-+|&]?=)(?!=)", body, re.M))
        writes |= set(re.findall(r"mem(?:cpy|set)\(\s*&?(\w+)", body))
        writes |= set(re.findall(r"(\w+)(?:\.\w+)*(?:\+\+|--)", body))
        writes = (writes & statics) - {'inputs'}
        if writes:
            found[match.group(1)] = sorted(writes)
    return found


def test_draw_inputs_kept():
    found = kept_outside(read(DEVICE))
    unexplained = {call: statics for call, statics in found.items() if call not in KEPT_ELSEWHERE}
    assert not unexplained, (f"the game's calls keep state outside struct draw_inputs (d3d8_device.c), which the "
                             f"journal would not record: {unexplained}. Keep it in struct draw_inputs, or name the call "
                             f"in KEPT_ELSEWHERE with why no draw's pipeline is built from it")
    assert set(KEPT_ELSEWHERE) <= set(found), sorted(set(KEPT_ELSEWHERE) - set(found))


def test_draw_inputs_kept_negative():
    """the soft group kept in a static of its own again (as #1159 kept it): seen"""
    device = mutated(read(DEVICE), '\tinputs.soft_group = soft != 0;\n', '\tsoft_effects.read = soft != 0;\n')
    assert kept_outside(device).get('halo_effect_soft') == ['soft_effects']
