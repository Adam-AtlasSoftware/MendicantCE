"""The built-in benchmark (port/linux/game/benchmark.c, #1150): its results' math (report.c's report_measure_frames)
on a frame ring made up here, and its guard (benchmark_live_problem), which refuses a frozen, empty or unfought match
before it is measured; each with negative controls."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

NEGATIVE_CONTROLS = {
    # the negative control the brief asks for: a frozen match taken as a live one
    "frozen-taken": ("live", "(double)live->ticks < live->real_seconds * (double)TICKS_PER_SECOND / 4.0",
                     "(double)live->ticks < 0.0"),
    # a match measured though nobody has been killed
    "kills-ignored": ("live", "if (live->kills < 1)", "if (live->kills < 0)"),
    # the lows from the fastest frames, not the slowest (report_low_fps, shared with Save Report)
    "low-fastest": ("measure", "for (index = count - slowest; index < count; index++)",
                    "for (index = 0; index < slowest; index++)"),
    # the frames not measured on the GPU counted as 0 ms
    "gpu-unknown": ("measure", "if (work[index].gpu != REPORT_GPU_UNKNOWN)\n\t\t{", "if (1)\n\t\t{"),
    # the game's waits counted as its work
    "game-waits": ("measure", "game_total += micros[index] - waited;", "game_total += micros[index];"),
    # the median taken for the 99th percentile
    "p99-median": ("measure", "scratch[(count - 1) * 99 / 100]", "scratch[(count - 1) * 50 / 100]"),
}


def generated(fault=None):
    report = read("port/linux/src/report.c")
    header = read("port/linux/src/report.h")
    benchmark = read("port/linux/game/benchmark.c")
    frozen = re.search(r"^#define BENCHMARK_FROZEN_SECONDS .*$", benchmark, re.M).group(0)
    code = "\n\n".join([
        structure(header, "report_frame_work"), structure(header, "report_measure"),
        structure(read("port/linux/game/benchmark.h"), "benchmark_live"), frozen,
        function(report, "report_compare_micros"), function(report, "report_low_fps"),
        function(report, "report_measure_frames"), function(benchmark, "benchmark_live_problem")])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["measure", "live"])
def test_case(case):
    status, output = run(build("benchmark", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("benchmark", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"
