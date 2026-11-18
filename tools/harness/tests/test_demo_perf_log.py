"""debug.demo_perf_log (#1148): a performance log beside each demo recorded (port/linux/src/report.c), begun and ended
by the demos' listener (demo_set_recording_listener) and never touching the demo: a row for each frame at the game's
tick, so its rows line up with the demo's ticks, stalls, loads and pipelines built as events, a summary every 10 s,
and the logs of demos that are gone deleted as the next starts. Off (the default), the listener is never set and
nothing is read or written."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

NEGATIVE_CONTROLS = {
    # the rows a tick late
    "tick-late": ("rows", "\ttick = game_time_get();", "\ttick = game_time_get() + 1;"),
    # the listener set whatever the setting
    "always-listening": ("off", "\tif (demo_perf.on && !demo_perf.listening)", "\tif (!demo_perf.listening)"),
    # every log deleted at a start, the pinned demo's too
    "retain-all": ("rows", "\t\tif (demo_files_path(demo, path, sizeof(path)))\n\t\t\tcontinue;\n", ""),
    # the frame rows' numbers not counted
    "numbers": ("rows", "++demo_perf.frames", "demo_perf.frames"),
}

PROTOTYPES = """typedef void (*demo_recording_listener)(int started, const char *name, long tick);
static void demo_set_recording_listener(demo_recording_listener listener);
"""


def generated(fault=None):
    report = read("port/linux/src/report.c")
    start = report.index("#define DEMO_PERF_STALL_MS")
    end = report.index("/* ---------- what is kept */")
    code = report[start:end]
    for declaration in ("typedef void (*demo_recording_listener)(int started, const char *name, long tick);\n",
                        "void demo_set_recording_listener(demo_recording_listener listener);\n",
                        "unsigned char game_time_initialized(void);\n", "long game_time_get(void);\n"):
        assert declaration in code, declaration
        code = code.replace(declaration, "", 1)
    code = PROTOTYPES + code
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


def run_case(executable, case, folder):
    import subprocess
    result = subprocess.run([str(executable), case, str(folder)], capture_output=True, text=True, timeout=60)
    return result.returncode, (result.stdout + result.stderr).strip()


@pytest.mark.parametrize("case", ["off", "rows"])
def test_case(case, tmp_path):
    status, output = run_case(build("demo_perf_log", generated()), case, tmp_path)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, tmp_path):
    status, output = run_case(build("demo_perf_log", generated(control)), NEGATIVE_CONTROLS[control][0], tmp_path)
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_demo_code_untouched_by_the_log():
    """the log is only told: report.c calls nothing of the demos' but the listener's setter and the folder's paths"""
    report = read("port/linux/src/report.c")
    block = report[report.index("/* ---------- debug.demo_perf_log"):report.index("/* ---------- what is kept */")]
    for name in ("demo_record_", "demo_tick_begin(", "demo_writer_", "demo_files_retain(", "demo_files_delete("):
        assert name not in block, name


def test_off_by_default():
    assert '{ "debug.demo_perf_log", _config_boolean, "false",' in read("port/linux/src/port_config.c")


def test_join_tool_reads_the_log(tmp_path):
    """tools/demo_perf.py reads the rows, events, summaries and end the log writes"""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from demo_perf import read_perf
    path = tmp_path / "x.perf"
    path.write_text("# demo perf log 1 x.demo 100\n# f tick frame ...\nf 100 1 4166 1000 2000 800 1000\n"
                    "e 105 stall 60.0\ns 400 2880 4.2 160.0 16.6\n# end 460\n")
    header, frames, events, summaries, end = read_perf(path)
    assert header == ("x.demo", 100) and frames == [(100, 1, 4166, 1000, 2000, 800, 1000)]
    assert events == [(105, "stall", 60.0)] and summaries == [(400, 2880, 4.2, 160.0, 16.6)] and end == 460
