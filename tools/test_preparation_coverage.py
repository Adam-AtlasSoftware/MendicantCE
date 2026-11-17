"""A map's preparation's record makes every pipeline the map's draws need, under every setting that changes them, with
no game started: for each stock map, at 4:3 (the records' own screen) and 16:9, the draws its walk made and the draws
its shipped record made played back (tools/prepared_records.py records both, as no setting changes them with their
contexts: tools/prepared_coverage.txt.gz) are derived under each setting of the matrix (xgpu_manifest_derive, the
front end's own derivation), and each draw the walk derives must be one the playback derives; a miss names the fields
it differs in from the playback's nearest draw. The negative controls play back a record that lost an input of the
draws (the soft group, the high-res text's atlas, the shadows' regions): the test names what is missing."""
import gzip
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from test_render_manifest import compile_reader  # noqa: E402

COVERAGE = ROOT / "tools/prepared_coverage.txt.gz"
LABEL = re.compile(r"^# coverage (\S+) (walk|replay|scope) (\d+) ([0-9a-f]+)$")
VERSION = int(re.search(r"#define XGPU_MANIFEST_VERSION (\d+)", (ROOT / "port/linux/src/xgpu_manifest.h").read_text())[1])

CHECKER = r"""
#include "xgpu_manifest.h"
#include "xgpu_hdr.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the settings checked: the records' defaults (no multisampling, SDR in a
16-bit frame, the enhanced lights, soft effects, Vulkan's alpha tests that
cannot fail dropped), and each one changed */
static const struct { const char *name; struct xgpu_manifest_settings settings; } matrix[] = {
	{ "defaults", { 1, 0, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 1, 1, 0, 1 } },
	{ "soft_particles_off", { 1, 0, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 1, 0, 0, 1 } },
	{ "output_depth_8", { 1, 0, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 0, 1, 0, 1 } },
	{ "hdr", { 1, 1, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 1, 1, 0, 1 } },
	{ "hdr_boost", { 1, 1, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 1, 1, XGPU_HDR_BOOST_MOST, 1 } },
	{ "alpha_drop_off", { 1, 0, 1, 1, XGPU_MANIFEST_OCCLUSION_OFF, 1, 1, 0, 0 } },
};

#define FIELD(name, member) { name, offsetof(struct xgpu_manifest_draw, member), \
	sizeof(((struct xgpu_manifest_draw *)0)->member) }
static const struct { const char *name; size_t offset, size; } fields[] = {
	FIELD("vertex_program", key.vertex_program), FIELD("packed_attributes", key.packed_attributes),
	FIELD("immediate", key.immediate), FIELD("lighting", key.lighting), FIELD("views", key.views),
	FIELD("blend", key.blend), FIELD("blend_source", key.blend_source),
	FIELD("blend_destination", key.blend_destination), FIELD("color_mask", key.color_mask),
	FIELD("depth_test", key.depth_test), FIELD("depth_write", key.depth_write), FIELD("cull", key.cull),
	FIELD("color_format", key.color_format), FIELD("depth_format", key.depth_format), FIELD("samples", key.samples),
	FIELD("combiner_state", key.pixel.combiner_state), FIELD("texture_modes", key.pixel.texture_modes),
	FIELD("sampler_type", key.pixel.sampler_type), FIELD("alpha_test_function", key.pixel.alpha_test_function),
	FIELD("coverage_alpha", key.pixel.coverage_alpha), FIELD("per_pixel_lighting", key.pixel.per_pixel_lighting),
	FIELD("alpha_test_samples", key.pixel.alpha_test_samples), FIELD("dynamic_lights", key.pixel.dynamic_lights),
	FIELD("vr_effect", key.pixel.vr_effect), FIELD("ambient_occlusion", key.pixel.ambient_occlusion),
	FIELD("hdr", key.pixel.hdr), FIELD("immediate_written", immediate_written),
	FIELD("elements", elements), FIELD("strides", strides),
	FIELD("stage0", stages[0]), FIELD("stage1", stages[1]), FIELD("stage2", stages[2]), FIELD("stage3", stages[3]),
};

static int load(const char *path, struct xgpu_manifest *manifest)
{
	char error[256];

	if (!xgpu_manifest_read(path, manifest, error, sizeof(error)))
	{
		printf("error %s: %s\n", path, error);
		return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct xgpu_manifest walk, replay;
	struct xgpu_manifest_draw *made;
	size_t combination;
	uint32_t index, other;
	int misses = 0;

	if (argc < 3 || !load(argv[1], &walk) || !load(argv[2], &replay))
		return 2;
	made = calloc(replay.draw_count + 1, sizeof(*made));
	for (combination = 0; combination < sizeof(matrix) / sizeof(matrix[0]); combination++)
	{
		const struct xgpu_manifest_settings *settings = &matrix[combination].settings;

		for (other = 0; other < replay.draw_count; other++)
			xgpu_manifest_derive(&replay.draws[other], &replay.contexts[other], settings, 0, &made[other]);
		for (index = 0; index < walk.draw_count; index++)
		{
			struct xgpu_manifest_draw wanted;
			uint32_t nearest = 0, nearest_difference = 0xffffffffu;
			size_t field;
			int found = 0;

			xgpu_manifest_derive(&walk.draws[index], &walk.contexts[index], settings, 0, &wanted);
			for (other = 0; other < replay.draw_count && !found; other++)
			{
				const unsigned char *a = (const unsigned char *)&wanted, *b = (const unsigned char *)&made[other];
				uint32_t difference = 0;
				size_t at;

				for (at = 0; at < sizeof(wanted); at++)
					difference += a[at] != b[at];
				found = !difference;
				if (difference < nearest_difference)
				{
					nearest_difference = difference;
					nearest = other;
				}
			}
			if (found)
				continue;
			misses++;
			printf("miss %s: vertex %08x%08x, differing:", matrix[combination].name,
				(unsigned)wanted.key.vertex_program[1], (unsigned)wanted.key.vertex_program[0]);
			for (field = 0; replay.draw_count && field < sizeof(fields) / sizeof(fields[0]); field++)
			{
				if (memcmp((const char *)&wanted + fields[field].offset, (const char *)&made[nearest] + fields[field].offset,
					fields[field].size))
					printf(" %s", fields[field].name);
			}
			printf("\n");
		}
	}
	printf("misses %d\n", misses);
	return 0;
}
"""


def sections():
    """{(map, width, models): {"walk": lines, "replay": lines}}"""
    if not COVERAGE.is_file():
        pytest.skip("no coverage recorded (tools/prepared_records.py)")
    groups, current = {}, None
    with gzip.open(COVERAGE, "rt") as file:
        for line in file:
            line = line.rstrip("\n")
            label = LABEL.match(line)
            if label:
                map_name, how, width, models = label.groups()
                current = groups.setdefault((map_name, width, models), {}).setdefault(how, {}) \
                    if how != "scope" else None
                continue
            if current is not None and line and not line.startswith("#"):
                current[line] = None
    return groups


@pytest.fixture(scope="module")
def checker(tmp_path_factory):
    folder = tmp_path_factory.mktemp("coverage")
    (folder / "checker.c").write_text(CHECKER)
    return compile_reader(folder, folder / "checker.c", folder / "checker")


def check(checker, folder, walk, replay):
    """the misses (walk's draws the playback does not make) under the matrix"""
    import subprocess
    for name, lines in (("walk.txt", walk), ("replay.txt", replay)):
        (folder / name).write_text(f"manifest {VERSION}\n" + "\n".join(lines) + "\n")
    result = subprocess.run([str(checker), str(folder / "walk.txt"), str(folder / "replay.txt")], capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return [line for line in result.stdout.splitlines() if line.startswith("miss ")]


def test_every_record_makes_its_walks_pipelines(checker, tmp_path):
    groups = sections()
    paired = {key: group for key, group in groups.items() if "walk" in group and "replay" in group}
    widths = {width for _, width, _ in paired}
    assert len({map_name for map_name, _, _ in paired}) >= 24 and len(widths) >= 2, \
        f"the coverage does not hold every map's walk and playback at two screens' shapes: {sorted(groups)}"
    failures = {}
    for key, group in sorted(paired.items()):
        misses = check(checker, tmp_path, list(group["walk"]), list(group["replay"]))
        if misses:
            failures[" ".join(key)] = misses[:6]
    assert not failures, "pipelines the map's draws need that its record's playback does not make: " + \
        "; ".join(f"{key}: {misses}" for key, misses in failures.items())


def mutated_replay(lines, pattern, replacement):
    changed = [re.sub(pattern, replacement, line) if line.startswith("d ") else line for line in lines]
    assert changed != lines, f"the negative control no longer applies: {pattern}"
    return changed


# (an input of the draws the playback lost, its draws' lines as such a record's would be, and the field it names)
NEGATIVES = {
    # the soft group not journaled: the effects never soft as played back
    "soft_group": (r" cs=1\b", " cs=0", "vr_effect"),
    # the high-res text's atlas named as the texture made in its order: its distance fields (MSDF on w) lost
    "text_atlas": (r"( t=(?:[^ /]*/)*\d+\.\d+\.\d+\.\d+)\.6\.", r"\1.0.", "stage"),
    # the shadows' regions not journaled: the receivers' stages sample the atlas whole (no REGION on u, v)
    "regions": (r"( t=(?:[^ /]*/)*\d+\.\d+)\.5\.5\.", r"\1.2.2.", "stage"),
}


@pytest.mark.parametrize("name", sorted(NEGATIVES))
def test_negative_a_lost_input_is_named(checker, tmp_path, name):
    pattern, replacement, field = NEGATIVES[name]
    groups = sections()
    for key, group in sorted(groups.items()):
        if "walk" not in group or "replay" not in group:
            continue
        replay = list(group["replay"])
        if not any(line.startswith("d ") and re.search(pattern, line) for line in replay):
            continue
        # (a map whose playback has every draw of its walk: the control's misses its own)
        if check(checker, tmp_path, list(group["walk"]), replay):
            continue
        misses = check(checker, tmp_path, list(group["walk"]), mutated_replay(replay, pattern, replacement))
        assert misses and all(field in miss for miss in misses), (key, misses[:4])
        return
    pytest.fail(f"no map's playback has draws the {name} control changes")
