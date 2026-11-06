"""A layout's spawn lines (forge_spawns.c, forge_spawns_layout_load): a layout comes from a file or from the host of
a system link game, so a spawn's team is pinned into the game's 16, a game type the game has not is none, a facing
that is not a number faces along x, and a time is pinned into a short's.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["valid", "team", "game-types", "facing", "spawn-time", "position"]

# a fault in forge_spawns.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "team-unchecked": (("player->team_index = (short)PIN(numbers[0], 0, FORGE_SPAWNS_MAXIMUM_TEAM_INDEX)",
                        "player->team_index = (short)numbers[0]"),
                       "forge_spawns_layout_load", "team"),
    "any-game-type": (("? (short)game_type\n\t\t: (short)game_engine_none", "? (short)game_type\n\t\t: (short)game_type"),
                      "forge_spawns_layout_game_type", "game-types"),
    "facing-unchecked": (("? facing : 0.f", "? facing : facing"), "forge_spawns_layout_facing", "facing"),
    "time-unbounded": (("(short)PIN(numbers[1], 0, SHORT_MAX)", "(short)MAX(numbers[1], 0)"),
                       "forge_spawns_layout_load", "spawn-time"),
}

FUNCTIONS = ("forge_spawns_layout_game_type", "forge_spawns_layout_facing", "forge_spawns_layout_position",
             "forge_spawns_layout_load")


def defines(source, *names):
    """The #define lines of the names, as the source has them (some are reals)."""
    return "".join(re.search(r"^#define\s+" + re.escape(name) + r"\s.*$", source, re.M).group(0) + "\n"
                   for name in names)


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/forge_spawns.c")
    config = defines(source, "FORGE_SPAWNS_WORLD_BOUND", "FORGE_SPAWNS_MAXIMUM_TEAM_INDEX", "FORGE_SPAWNS_FACING_BOUND")
    config += enum_with(read("source/game/game_engine.h"), "game_engine_none = 0") + "\n"
    config += enum_with(source, "FORGE_SPAWNS_GAME_TYPE_ALL = 12") + "\n"
    config += enum_with(source, "_forge_spawn_player = 0") + "\n"
    text = ""
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("forge_spawns_layout", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("forge_spawns_layout", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"forge_spawns.c with a fault ({control}) passed '{case}': the test cannot see it"
