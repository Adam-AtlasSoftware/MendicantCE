"""#1151: the passes each graphics setting owns labeled by it ("SMAA 0.09 ms", not "ended by
xgpu_vulkan_set_target_views") in debug.pass_times' log and the performance overlay's detailed view (debug.timings).
Off (no passes timed), no owner is kept and the labels are what they were. The groups' log line is read back by the
overlay as the backend writes it; and each owner is set around its setting's work in the Vulkan stream's decoder."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

VULKAN = "port/linux/vulkan/xgpu_vulkan.c"
STREAM = "port/linux/vulkan/xgpu_vulkan_stream.c"

NEGATIVE_CONTROLS = {
    # an owner kept while the passes are not timed
    "kept-off": ("off", "\tif (host_pass_times)\n\t\tvk.pass_owner = owner;", "\tvk.pass_owner = owner;"),
    # the owner not used for the label
    "unlabeled": ("owned", "\tif (segment->owner)\n\t\treturn segment->owner;\n", ""),
    # every label taken for an owner's: the passes' detailed labels each a group
    "all-owned": ("groups", "return label[0] >= 'A' && label[0] <= 'Z';", "return label[0] != 0;"),
    # the overlay reads the first line logged, not the last
    "first-line": ("groups", "\twhile ((at = strstr(at, prefix)) != NULL)\n\t\tfound = at++;",
                   "\tfound = strstr(at, prefix);"),
}


def generated(fault=None):
    vulkan = read(VULKAN)
    fake = (f"#define PASS_TIMES_LABEL {constant(vulkan, 'PASS_TIMES_LABEL')}\n"
            f"#define PASS_TIMES_LABELS {constant(vulkan, 'PASS_TIMES_LABELS')}\n"
            "static struct\n{\n\tstruct\n\t{\n\t\tchar label[PASS_TIMES_LABEL];\n\t\tdouble milliseconds, count;\n"
            "\t} sums[PASS_TIMES_LABELS];\n\tuint32_t sum_count;\n\tdouble frames;\n"
            "\tuint32_t draws_before, frames_before;\n} pass_times;\n")
    code = "\n\n".join([fake, function(vulkan, "pass_times_owned"), function(vulkan, "xgpu_vulkan_pass_owner"),
                        function(vulkan, "pass_times_label"), function(vulkan, "pass_times_groups_log"),
                        function(read("port/linux/game/perf_overlay.c"), "perf_overlay_pass_groups")])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["off", "owned", "groups"])
def test_case(case):
    status, output = run(build("pass_owners", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("pass_owners", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


# each setting's work in the decoder, between its owner and NULL: (callback, owner, the work's call)
OWNED_WORK = [
    ("stream_anti_alias", "record->smaa ? PASS_OWNER_SMAA : PASS_OWNER_FXAA", "xgpu_vulkan_post_anti_alias("),
    ("stream_panini", "PASS_OWNER_PANINI", "xgpu_vulkan_post_panini("),
    ("stream_target_resolve", "PASS_OWNER_MSAA_RESOLVE", "\ttarget_resolve(target_get("),
    ("stream_frame_end", "PASS_OWNER_MSAA_RESOLVE", "target_resolve(back_buffer)"),
    ("stream_frame_end", "PASS_OWNER_FINAL", "xgpu_vulkan_frame_end("),
    ("stream_world_ao_prepass", "PASS_OWNER_AO", "xgpu_vulkan_set_target_views(prepass.inputs"),
    ("stream_world_ao_prepass", "PASS_OWNER_AO", "prepass_pass_draw("),
]


@pytest.mark.parametrize("callback, owner, work", OWNED_WORK)
def test_owner_around_its_work(callback, owner, work):
    """the owner is set before the setting's work and taken away after it (before the stream's targets are bound
    again for what follows)"""
    body = function(read(STREAM), callback)
    at = body.index(work)
    before = body[:at]
    set_at = before.rindex(f"xgpu_vulkan_pass_owner({owner});")
    assert "xgpu_vulkan_pass_owner(NULL);" not in before[set_at:], f"{owner} taken away before {work}"
    after = body[at:]
    assert "xgpu_vulkan_pass_owner(NULL);" in after, f"{owner} kept after {work}"


def test_owners_named_by_their_settings():
    stream = read(STREAM)
    owners = dict(re.findall(r'#define (PASS_OWNER_\w+) "([^"]+)"', stream))
    assert owners == {"PASS_OWNER_FXAA": "FXAA", "PASS_OWNER_SMAA": "SMAA", "PASS_OWNER_MSAA_RESOLVE": "MSAA RESOLVE",
                      "PASS_OWNER_PANINI": "PANINI", "PASS_OWNER_AO": "AO", "PASS_OWNER_FINAL": "FINAL PASS",
                      # (the terrain overhaul's HQ layers, display.terrain_layers "hq": docs/prism_terrain.md)
                      "PASS_OWNER_TERRAIN": "TERRAIN"}
    # (capitals: pass_times_owned tells them from the passes' own labels)
    assert all(re.fullmatch(r"[A-Z][A-Z ]*", name) for name in owners.values())


def test_owner_taken_as_a_pass_begins():
    vulkan = read(VULKAN)
    assert "segment->owner = vk.pass_owner;" in function(vulkan, "pass_segment_begin")
    assert "vk.direct_pass.owner = vk.pass_owner;" in vulkan
    assert "segment->owner = vk.pass_owner;" in function(vulkan, "outside_commands")
