"""A frame whose work waits for a tick's boundary (a checkpoint, a structure switch, a death: game_time_tick_open_wait)
at game.sim_rate 60 and 120 begins no tick, and builds no update for one: an update built for a tick not begun stayed
queued, and every tick after took one built a tick earlier, its input with it, until the map was loaded again (OpenH1
#590: presses 0.6 s late on The Silent Cartographer after a few structure switches). A stall drops the time past the
frame's most ticks and owes none after."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

CLAMP = "\t\t\telse if (steps_elapsed > substep_count - sim_substep_next())"
OLD_BREAK = ("\t\t\t\t\t\tif (server_updates <= 0)\n\t\t\t\t\t\t\tbreak;\n",
             "\t\t\t\t\t\tif (server_updates <= 0)\n\t\t\t\t\t\t\tbreak;\n"
             "\t\t\t\t\t\tif (game_time_substeps.boundary_wanted)\n\t\t\t\t\t\t{\n"
             "\t\t\t\t\t\t\tgame_time_globals->leftover_dt += (real)(steps_elapsed - step_index)/steps_per_second;\n"
             "\t\t\t\t\t\t\tbreak;\n\t\t\t\t\t\t}\n")


def generated(*faults):
    source = read("source/game/game_time.c")
    update = function(source, "game_time_update")
    for fault in faults:
        update = mutated(update, *fault)
    code = "\n".join((function(source, "game_time_finish_tick"), function(source, "game_time_tick_open_wait"), update))
    substeps = re.search(r"static struct\n\{\n\t/\* sub-steps run before.*?\} game_time_substeps;", source, re.S)
    large = re.search(r"enum\n\{\n\tSOME_LARGE_NUMBER_OF_TICKS[^}]*\};", source)
    waits = re.search(r"#define GAME_TIME_MAXIMUM_BOUNDARY_WAITS \d+", source)
    defines = "\n".join((large.group(0), structure(source, "game_time_globals_struct"), substeps.group(0), waits.group(0)))
    return (("under_test.inc", code), ("tick_boundary_defines.h", defines))


@pytest.mark.parametrize("case", ["local", "host", "stall"])
def test_no_update_left_queued(case):
    status, output = run(build("tick_boundary_queue", generated()), case)
    assert status == 0, output


def test_negative_control_ticks_begun_while_waiting():
    """without the clamp, the frame runs on past the boundary its work waits for"""
    status, output = run(build("tick_boundary_queue", generated((CLAMP, "\t\t\telse if (0)"))), "local")
    assert status == CHECK_FAILED and "boundary" in output, output


@pytest.mark.parametrize("case", ["local", "host"])
def test_negative_control_queue_left_behind(case):
    """the code before the fix: the loop stopped at the boundary, the update built for the tick after left queued"""
    status, output = run(build("tick_boundary_queue", generated((CLAMP, "\t\t\telse if (0)"), OLD_BREAK)), case)
    assert status == CHECK_FAILED and "queued" in output, output
