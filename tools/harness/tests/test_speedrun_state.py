"""The speedrun state block of port/linux/game/speedrun_state.c: its layout as the header's table, the docs' table, the
sample script's and the Linux reader's give it; its magic; the run's ticks across a checkpoint's revert (which takes
the level's clock back, the block's own not); the loading and cutscene flags; when a run begins."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

SOURCE = "port/linux/game/speedrun_state.c"
CASES = ["magic", "revert", "loading", "cutscene", "runs", "practice"]
FUNCTIONS = ("speedrun_state_copy", "speedrun_state_wants_settings", "speedrun_state_update", "speedrun_state_loading",
             "speedrun_state_map_load", "speedrun_state_level_won")
GLOBALS = ("speedrun_state_block", "speedrun_state_tracker", "speedrun_state_magic_reversed",
           "speedrun_state_magic_mask")

# a fault in speedrun_state.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "ticks-are-the-level-clock": (("block->run_ticks += (unsigned long)input->frame_ticks;",
                                   "block->run_ticks = input->game_ticks;"), "speedrun_state_update", "revert"),
    "loading-never-ends": (("\t\tflags = 0;\n", "\t\tflags = SPEEDRUN_STATE_FLAG_LOADING;\n"),
                           "speedrun_state_update", "loading"),
    "cutscene-ignored": (("if (input->cutscene)", "if (0)"), "speedrun_state_update", "cutscene"),
    "every-map-a-new-run": (("(tracker->last_main_menu ||", "(TRUE ||"), "speedrun_state_update", "runs"),
    "magic-not-reversed": (("[SPEEDRUN_STATE_MAGIC_SIZE - 1 - index]", "[index]"), "speedrun_state_update",
                           "magic"),
    "practice-only-as-a-map-begins": (("\tif (input->practice)\n", "\tif (input->practice && tracker->map_loading)\n"),
                                      "speedrun_state_update", "practice"),
}


def generated(fault=None, faulty_function=None):
    source = read(SOURCE)
    text = structure(source, "speedrun_state_input") + "\n" + structure(source, "speedrun_state_tracker") + "\n"
    for name in GLOBALS:
        match = re.search(r"^static [^\n]*\b" + name + r"\b[^\n]*;\n", source, re.M)
        assert match, f"{name} not found in {SOURCE}"
        text += match.group(0)
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("speedrun_state", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("speedrun_state", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"speedrun_state.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_layout_matches_the_header_docs_and_script():
    """the compiled offsets (the header's structure) as the header's table, the docs' table, the sample script's
    offsets and the reader's format say"""
    status, output = run(build("speedrun_state", generated()), "layout")
    assert status == 0, output
    compiled = {name: int(offset, 16) for name, offset in (line.split() for line in output.splitlines())}

    header = read("port/linux/game/speedrun_state.h")
    table = {name: int(offset, 16) for offset, name in re.findall(r"^   (0x[0-9A-F]{2})    (\S+)", header, re.M)}
    assert table == compiled, "speedrun_state.h's table is not its structure's layout"

    docs = read("docs/speedrun_state.md")
    rows = {name: int(offset, 16) for offset, name in re.findall(r"^\| (0x[0-9A-F]{2}) \| (\S+) \|", docs, re.M)}
    assert rows == compiled, "docs/speedrun_state.md's table is not the block's layout"

    script = read("docs/speedrun_state.asl")
    for field, name in (("version", "Version"), ("flags", "Flags"), ("run", "Run"), ("run_ticks", "RunTicks"),
                        ("levels_completed", "LevelsCompleted")):
        match = re.search(r"vars\.Offset" + name + r" = (0x[0-9A-F]+);", script)
        assert match and int(match.group(1), 16) == compiled[field], f"the sample script's {field} offset"
    signature = re.search(r'SigScanTarget\(0, "([0-9A-F ]+)"\)', script).group(1)
    assert bytes.fromhex(signature) == b"GOAT H1 SPEEDRUN"

    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    import speedrun_state_read
    assert speedrun_state_read.LAYOUT.size == compiled["(end)"]
    # (the reader's fields in order, each at its compiled offset)
    position = 0
    names = iter(speedrun_state_read.FIELDS)
    for code in re.findall(r"\d*[sI]", speedrun_state_read.LAYOUT.format[1:]):
        count, kind = int(code[:-1] or 1), code[-1]
        for _ in range(1 if kind == "s" else count):
            assert compiled[next(names)] == position
            position += count if kind == "s" else 4
    assert position == compiled["(end)"]
