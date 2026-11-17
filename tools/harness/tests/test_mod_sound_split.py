"""A mod's permutation of 16-bit PCM longer than one is kept in (OpenH1 issue 1167) stays PCM at its own rate: the real
mod_sounds.c (mod_sound_new) splits it at load into a chain of linked pieces, each under the cap of the ARM64 guests'
sound cache, whose frames joined are the source's, sample for sample; the chain ends where the permutation's did, a
looping sound's next pick is never a piece, and past the memory the mods may keep it is Xbox ADPCM, as before."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import (CHECK_FAILED, build, constant, enum_with, function, mutated, needs_32_bit_abi, read,  # noqa: E402
                     run, structure)
from harness.audio import pcm_permutation_rate, permutation_picker  # noqa: E402

# the tag file's sound, pitch ranges and permutations are read at the game's 32-bit offsets (offsetof)
pytestmark = needs_32_bit_abi("a sound tag file is read at the offsets of the game's 32-bit structures")

CASES = ["mono-30s", "stereo-15s-48k", "linked-chain", "under-cap", "loop-wrap", "fake-impulse-repeats",
         "too-many-pieces", "over-budget"]
NEGATIVE_CONTROLS = {
    # the old fallback (#1111): a permutation longer than the cap is Xbox ADPCM at 22.05 kHz
    "old-fallback": (("native_budget, FALSE, kept_rate", "native_budget, TRUE, kept_rate"), "mono-30s"),
    # a one-sample gap at each join: the next piece starts a sample on
    "gap-at-join": (("memcpy(piece->samples.address, samples + offset, size);",
                     "memcpy(piece->samples.address, samples + offset + 2, size - 2);"), "mono-30s"),
    # the split sound made a linked one (the first version): a looping sound that repeated forever plays once
    "play-once": (("\tdefinition->longest_permutation_length = (long)longest;",
                   "\tif (sound->permutation_count > permutation_total)\n"
                   "\t\tSET_FLAG(definition->flags, _sound_definition_linked_permutations_bit, TRUE);\n"
                   "\tdefinition->longest_permutation_length = (long)longest;"), "fake-impulse-repeats"),
    # the chain's last piece not followed by what followed the permutation
    "chain-cut": (("\t\t\tpiece->next_permutation_index = next;\n", "\t\t\tpiece->next_permutation_index = NONE;\n"),
                  "linked-chain"),
}


def generated(fault=None):
    definitions = read("source/sound/sound_definitions.h")
    tags = read("source/tag_files/tag_groups.h")
    mods_header = read("port/linux/game/mods.h")
    capacity = read("port/linux/include/halo_port_capacity.h")
    types = "".join(structure(tags, name) + "\n" for name in ["tag_block", "tag_reference", "tag_data"])
    types += "".join(structure(definitions, name) + "\n" for name in
                     ["real_bounds", "sound_permutation", "sound_pitch_range", "sound_definition"])
    types += enum_with(definitions, "_sound_definition_linked_permutations_bit") + "\n"
    for name in ["MAXIMUM_SOUND_DATA_SIZE", "MAXIMUM_PITCH_RANGES_PER_SOUND", "MAXIMUM_PERMUTATIONS_PER_PITCH_RANGE",
                 "MAXIMUM_PERMUTATIONS_PER_RANDOM_PITCH_RANGE", "MAXIMUM_SOUND_MOUTH_DATA_SIZE",
                 "MAXIMUM_SOUND_SUBTITLE_DATA_SIZE", "SOUND_DEFINITION_VERSION"]:
        types += f"#define {name} {constant(definitions, name)}\n"
    types += "#define SOUND_DEFINITION_TAG 0x736E6421\n"
    # the ARM64 guests' sound cache (halo_port_capacity.h), the smallest: a quarter of it, 1 MB, the cap
    arm64 = re.search(r"#if defined\(HALO_GUEST\) && defined\(__aarch64__\)\n#define HALO_PORT_SOUND_CACHE_SIZE (\w+)",
                      capacity).group(1)
    types += f"#define HALO_PORT_SOUND_CACHE_SIZE {arm64}\n"
    types += "".join(line + "\n" for line in mods_header.splitlines()
                     if line.startswith(("#define MOD_TAG_FILE_", "#define MAXIMUM_MOD_TAG_FILE_SIZE")))
    types += pcm_permutation_rate()
    encoder = read("port/linux/game/custom_edition_sounds.c")
    code = encoder[encoder.index("/* ---------- constants */"):]
    mods = read("port/linux/game/mods.c")
    code += "\n".join(function(mods, name) for name in ["mods_read_be16", "mods_read_be32", "mods_read_short",
                                                         "mods_read_real", "mods_in_file", "mods_finite_real",
                                                         "mods_tag_file_header_valid"]) + "\n"
    loader = read("port/linux/game/mod_sounds.c")
    code += loader[loader.index("/* ---------- constants */"):loader.index("/* Gives `definition` the values")]
    code += permutation_picker()
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("mod_sound_split", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("mod_sound_split", generated(fault)), case)
    assert status == CHECK_FAILED, f"the code with a fault ({control}) passed '{case}': the test cannot see it"
