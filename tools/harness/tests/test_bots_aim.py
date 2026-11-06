"""A bot's aim as a player's hand moves it (port/linux/game/bots.c's bots_turn and bots_aim_*): by its skill's spring the
easy bot flicks past a target and corrects while the legendary one settles cleanly, every skill within a second, and a
tracked target is not trailed; the aim error settles to a wander of its skill's floor that never reaches zero; a change
of the target's motion keeps the aim on the old motion for the bot's reaction time, then grows the error; a hit knocks
the aim off (more for a shield popped); the bot's form wanders slowly within its bounds. Where it aims: the sniper rifle
and the pistol at the head once the shield is down, explosives at the feet, the rest at the body, the head as often as
its skill's chance."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["spring", "tracks", "floor", "motion", "flinch", "form", "aim-point", "head-chance"]
FUNCTIONS = ("bots_random", "bots_random_real", "bots_random_signed", "bots_angle_difference", "bots_turn", "bots_aim_settle", "bots_aim_form",
             "bots_aim_flinch", "bots_aim_target_motion", "bots_aim_point_kind", "bots_aim_head_wanted")

# a fault, the function it is in (None: the skill table), and the case that must catch it
NEGATIVE_CONTROLS = {
    "damped-critically-always": (("friction = 2.0f * damping * frequency", "friction = 2.0f * frequency"), "bots_turn",
                                 "spring"),
    "legendary-flicks-past": (("20.0f, 1.0f,", "20.0f, 0.5f,"), None, "spring"),
    "track-ignored": (("(bot->yaw_rate - yaw_track)", "(bot->yaw_rate)"), "bots_turn", "tracks"),
    "settles-to-zero": (("BOTS_DEGREES * bot->form * disturbance * blur;",
                         "BOTS_DEGREES * bot->form * disturbance * blur * 0.0f;"), "bots_aim_settle", "floor"),
    "error-never-settles": (("bot->aim_error_yaw *= settle;", ";"), "bots_aim_settle", "floor"),
    "reacts-at-once": ((">= (long)(skill->reaction_seconds * 0.4f * TICKS_PER_SECOND)", ">= 0"), "bots_aim_target_motion",
                       "motion"),
    "no-error-on-reacting": (("BOTS_DEGREES * 0.2f *", "BOTS_DEGREES * 0.0f *"), "bots_aim_target_motion", "motion"),
    "drift-not-kept": (("bot->motion_drift.i -= di;", ";"), "bots_aim_target_motion", "motion"),
    "no-flinch": (("(MIN(loss * 2.0f, 1.5f) + (popped ? 0.5f : 0.0f))", "0.0f"), "bots_aim_flinch", "flinch"),
    "popped-no-worse": (("(popped ? 0.5f : 0.0f)", "0.0f"), "bots_aim_flinch", "flinch"),
    "form-jumps": (("bots_random_signed(bot) * 0.1f", "bots_random_signed(bot) * 0.5f"), "bots_aim_form", "form"),
    "pistol-head-through-shield": (("head_wanted && target_shield <= 0.05f", "head_wanted"), "bots_aim_point_kind",
                                   "aim-point"),
    "rockets-at-the-body": (("case _bots_family_explosive:\n\t\treturn _bots_aim_feet;",
                             "case _bots_family_explosive:\n\t\treturn _bots_aim_body;"), "bots_aim_point_kind",
                            "aim-point"),
    "sniper-body-always": (("case _bots_family_sniper:\n", ""), "bots_aim_point_kind", "aim-point"),
    "head-chance-ignored": (("bots_random_real(bot) < skill->head_chance", "TRUE"), "bots_aim_head_wanted",
                            "head-chance"),
    "head-every-tick": (("bot->head_tick = now + TICKS_PER_SECOND * 2;", "bot->head_tick = now;"),
                        "bots_aim_head_wanted", "head-chance"),
}


def _define(source, name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(.+)$", source, re.M)
    if not match:
        raise LookupError(f"#define not found in the sources: {name}")
    return f"#define {name} {match.group(1)}\n"


def _skill_table(source):
    match = re.search(r"^static struct bots_skill_definition const bots_skills\[NUMBER_OF_BOTS_SKILLS\] =\n\{.*?^\};",
                      source, re.M | re.S)
    if not match:
        raise LookupError("bots_skills not found in bots.c")
    return match.group(0)


def generated(fault=None, faulty_function=None):
    source = read("port/linux/game/bots.c")
    config = "".join(_define(source, name) for name in ("BOTS_PI", "BOTS_DEGREES", "BOTS_UNREACHABLE_ITEMS"))
    config += f"#define TICKS_PER_SECOND {constant(read('source/cseries/cseries.h'), 'TICKS_PER_SECOND')}\n"
    config += enum_with(source, "_bots_skill_easy") + "\n"
    config += enum_with(source, "_bots_family_other") + "\n"
    config += enum_with(source, "_bots_aim_body") + "\n"
    config += structure(source, "bots_skill_definition") + "\n"
    table = _skill_table(source)
    if fault and faulty_function is None:
        table = mutated(table, *fault)
    config += table + "\n"
    config += structure(source, "bots_bot") + "\n"
    code = ""
    for name in FUNCTIONS:
        text = function(source, name)
        if fault and name == faulty_function:
            text = mutated(text, *fault)
        code += text + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("bots_aim", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("bots_aim", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"bots.c with a fault ({control}) passed '{case}': the test cannot see it"
