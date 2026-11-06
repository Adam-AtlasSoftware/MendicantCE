"""The AI's command lists of a map (forge_ai.c, forge_ai_install_command_list): the scenario's lists with Forge's at
the end (its patrol and the squad followers' lists); a map reset, which keeps the map's tags, keeps those lists rather
than adding more, and a map's block is freed when the map after the next is made.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["copies", "reset", "new-maps", "no-lists", "retire"]

# a fault in forge_ai.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "copies-on-reset": (("scenario->ai_command_lists.address == forge_ai_globals.command_lists",
                         "scenario->ai_command_lists.address == NULL"),
                        "forge_ai_install_command_list", "reset"),
    "never-frees": (("free(forge_ai_globals.previous_command_lists);", ""), "forge_ai_retire_command_lists",
                    "new-maps"),
    "frees-at-once": (("forge_ai_globals.previous_command_lists = forge_ai_globals.command_lists;",
                       "free(forge_ai_globals.command_lists);"),
                      "forge_ai_retire_command_lists", "new-maps"),
    "keeps-index": (("\tforge_ai_globals.command_list_index = NONE;\n", ""), "forge_ai_retire_command_lists",
                    "retire"),
    "shares-follow-list": (("&forge_ai_globals.follow_commands[index - 1]", "forge_ai_globals.follow_commands"),
                           "forge_ai_install_command_list", "copies"),
}


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/forge_ai.c")
    text = ""
    for name in ("forge_ai_retire_command_lists", "forge_ai_install_command_list"):
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("forge_ai_command_list", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("forge_ai_command_list", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"forge_ai.c with a fault ({control}) passed '{case}': the test cannot see it"
