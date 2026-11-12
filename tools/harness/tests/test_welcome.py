"""The host's welcome to OpenCE players (network_welcome.c): each OpenCE player sees its two lines once, a few seconds
after loading into a game; an OpenH1 player never (its chat leaves them out); no one with network.opence_welcome
false. The lines go as the chat's notices, which OpenCE shows: no new message between the machines."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["lines-fit", "once", "openh1-token", "delay", "rejoin", "replaced", "game-ends", "off", "client",
         "clock-wrap", "remember-full"]

# a fault in network_welcome.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    "welcomed-every-game": (("welcome_player_welcomed(welcome.welcomed, welcome.welcomed_count, &player) ||", ""),
                            "once"),
    "openh1-shows-it": (("if (!csstrcmp(text, welcome_lines[line]))\n\t\t\treturn TRUE;", ""), "once"),
    "no-delay": (("return now - loaded_at >= WELCOME_DELAY_MILLISECONDS;", "return TRUE;"), "delay"),
    "setting-ignored": (("||\n\t\t\t!config_boolean(\"network.opence_welcome\"))", ")"), "off"),
    "replaced-machine-taken": (("if (!welcome_same_player(&player, &welcome.pending_players[machine_index]) ||",
                                "if ("), "replaced"),
    "kept-past-game": (("csmemset(welcome.pending, 0, sizeof(welcome.pending));", ""), "game-ends"),
    "client-welcomes": (("if (game_connection() != _game_connection_network_server ||", "if ("), "client"),
    "clock-wrap-broken": (("return now - loaded_at >= WELCOME_DELAY_MILLISECONDS;",
                           "return now >= loaded_at + WELCOME_DELAY_MILLISECONDS;"), "clock-wrap"),
    "oldest-kept": (("players[*oldest] = *player;", ";"), "remember-full"),
    "openh1-token-welcomed": (("||\n\t\tnetwork_standard_machine_is_openh1(machine_index))", ")"), "openh1-token"),
}


def generated(fault=None, fork=False):
    source = read("port/linux/game/network_welcome.c")
    chat = read("port/linux/game/network_chat.c")
    config = f"#define CHAT_TEXT_SIZE {constant(chat, 'CHAT_TEXT_SIZE')}\n"
    config += "#define HALO_PORT_MAXIMUM_NETWORK_MACHINES " \
              f"{constant(read('port/linux/include/halo_port_limits.h'), 'HALO_PORT_MAXIMUM_NETWORK_MACHINES')}\n"
    config += function(chat, "chat_text_clean") + "\n"
    # (the whole file from its constants on: its globals and functions)
    text = source[source.index("/* ---------- constants */"):]
    if fault:
        text = mutated(text, *fault)
    # fork: (the fork's switch, for its own case's unit alone: the other cases are upstream's welcome, kept)
    if fork:
        config = "#define HALO_FORK_MENDICANT 1\n" + config
    return (("config.inc", config), ("under_test.inc", text))


def test_fork_says_what_it_runs():
    """fork: a host of this fork's build tells every other build's player that it runs MendicantCE, a modified OpenH1,
    and where its source is; its own players, and an OpenH1 host's welcome, show nothing"""
    status, output = run(build("welcome", generated(fork=True)), "fork")
    assert status == 0, output
    # (without the fork's lines recognised, its own machine shows them)
    fault = ("if (!csstrcmp(text, welcome_lines[line]))\n\t\t\treturn TRUE;", "")
    status, output = run(build("welcome", generated(fault, fork=True)), "fork")
    assert status == CHECK_FAILED, output


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("welcome", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("welcome", generated(fault)), case)
    assert status == CHECK_FAILED, f"network_welcome.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_the_game_calls_it():
    """The host welcomes a machine new at its index as its client ready comes, ticks it and lets it go with a game;
    an OpenH1 client's chat leaves the welcome's notices out, and shows every other."""
    distributed = read("port/linux/game/network_distributed.c")
    ready = distributed[distributed.index("case _distributed_message_client_ready:\n\t{"):]
    ready = ready[:ready.index("break;")]
    assert re.search(r"if \(loaded\)\n\t\t\{[^}]*network_welcome_machine_loaded\(machine_index\);", ready)
    assert "network_welcome_host_tick();" in function(distributed, "network_distributed_tick")
    assert "network_welcome_new_game();" in distributed
    down = function(read("port/linux/game/network_chat.c"), "network_chat_handle_down")
    notice = down[down.index("if (TEST_FLAG(line.flags, _chat_line_notice_bit))"):]
    assert notice.index("network_welcome_line(text)") < notice.index("chat_add_notice(text);")
