"""A replay's records walked (network_replay_playback.c's playback_record): every record is found by its size, one of
no bytes too (a crafted or damaged replay with one froze the game as it loaded), and a file cut short ends the walk.
And what a replay's file may say of itself before it is read: its header's size is bounded before anything is added
to it (0xFFFFFFF8 and more wrapped past the file's size in 32 bits, and was then allocated and filled), by the
Theater's list too; the recorder writes no record of no bytes; voice chat is neither recorded nor played."""

import pytest

from tools.harness import CHECK_FAILED, build, constant, function, mutated, read, run

CASES = ["sizes", "empty", "window", "cut"]

NEGATIVE_CONTROLS = {
    "empty-read-for-ever": (("\t\tif (sized)\n", "\t\tif (sized && length > REPLAY_RECORD_HEADER_SIZE)\n"), "empty"),
}


def generated(fault=None):
    source = read("port/linux/game/network_replay_playback.c")
    config = f"#define REPLAY_RECORD_HEADER_SIZE {constant(source, 'REPLAY_RECORD_HEADER_SIZE')}\n"
    # (a small window, so that the test's records cross it)
    config += "#define REPLAY_WINDOW_SIZE (256 * 1024)\n"
    text = function(source, "playback_record")
    if fault:
        text = mutated(text, *fault)
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("replay_record", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("replay_record", generated(fault)), case)
    assert status == CHECK_FAILED, f"playback_record with a fault ({control}) passed '{case}'"


def test_header_size_is_bounded():
    playback = read("port/linux/game/network_replay_playback.c")
    watch = function(playback, "network_replay_watch")
    assert "header_size + REPLAY_RECORD_HEADER_SIZE > (unsigned long)size" not in watch
    assert ("header_size > REPLAY_MAXIMUM_HEADER_SIZE ||\n"
            "\t\theader_size > (unsigned long)size - REPLAY_RECORD_HEADER_SIZE)") in watch
    assert watch.index("size < 32 ||") < watch.index("header_size > REPLAY_MAXIMUM_HEADER_SIZE")
    assert watch.index("header_size > REPLAY_MAXIMUM_HEADER_SIZE") < watch.index("malloc((size_t)header_size)")
    recorder = read("port/linux/game/network_replay.c")
    # (the recorder's own header fits, and the two files agree)
    assert int(constant(recorder, "REPLAY_HEADER_SIZE")) <= 64 * 1024
    assert "#define REPLAY_MAXIMUM_HEADER_SIZE (64 * 1024)" in playback
    assert "#define REPLAY_MAXIMUM_HEADER_SIZE (64 * 1024)" in recorder
    assert "header_size > REPLAY_MAXIMUM_HEADER_SIZE ||" in function(recorder, "network_replay_info")


def test_no_empty_record_and_no_voice():
    recorder = read("port/linux/game/network_replay.c")
    assert "size <= 0 || size > 0xFFFF" in function(recorder, "network_replay_record")
    received = function(recorder, "network_replay_record_received")
    assert received.index("network_voice_handles_message(") < received.index("network_replay_record(unreliable")
    voice = function(read("port/linux/game/network_voice.c"), "network_voice_handle_message")
    assert voice.index("if (network_replay_playing())\n\t\treturn;") < voice.index("switch (header.type)")
