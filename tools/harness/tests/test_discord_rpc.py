"""Discord's rich presence, invites and asks to join (port/linux/src/p2p_discord.c) against a fake Discord client
(discord_rpc.c): what the activity says in the menus, a party, a game hosted or joined; a Join taken as a link
opened; a friend's ask shown, let in (the spare party made, the invite sent) or declined, and let go when not
answered; nothing shown and no one let in with discord.rich_presence or discord.allow_join_requests off. The game
never reaches a real Discord client: the socket is the fake's, debug.discord_socket's. The party's spare and its
standby are tools/p2p_party_check.c's (tools/test_linux_port.py)."""

import re

import pytest

from tools.harness import ROOT, build, function, mutated, read, run

CASES = ["handshake", "states", "art", "join", "accept", "decline", "timeout", "game", "off", "nojoins",
         "reconnect"]


def generated(fault=None, fork=False):
    code = read("port/linux/src/p2p_discord.c")
    if fault:
        code = mutated(code, *fault)
    # fork: (the fork's switch, for its own case's unit alone: the other cases are upstream's behaviour, kept)
    if fork:
        code = "#define HALO_FORK_MENDICANT 1\n" + code
    # (platform.h's platform_log alone is used: discord_rpc.c declares it; the other headers are the sources')
    code = code.replace('#include "platform.h"\n', "")
    code = re.sub(r'#include "(posix|port_config|p2p_internal)\.h"',
                  lambda match: f'#include "{ROOT}/port/linux/src/{match.group(1)}.h"', code)
    p2p = read("port/linux/src/p2p.c")
    parts = "\n\n".join(function(p2p, name) for name in ("p2p_hex", "hex_value", "parse_invite", "p2p_parse_invite"))
    return (("under_test.inc", code), ("p2p_parts.inc", parts))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("discord_rpc", generated()), case)
    assert status == 0, output


def test_fork_uses_no_upstream_application():
    """fork: a build of this fork does not connect to Discord as OpenH1's or OpenCE's application, registers no
    discord-<id>:// handler for them (and gives back one it had), and says so once; a player's own application
    works, under the fork's name"""
    status, output = run(build("discord_rpc", generated(fork=True)), "fork")
    assert status == 0, output
    # (the guard is what does it: without it upstream's application connects)
    guard = ('\t\t\tapplication = "";\n', "")
    status, output = run(build("discord_rpc", generated(guard, fork=True)), "fork")
    assert status == 1, output


# a fault in p2p_discord.c, and the case that must catch it
FAULTS = {
    "socket-ignored": (('posix_discord_connect(config_string("debug.discord_socket"))', "posix_discord_connect(NULL)"),
                       "handshake"),
    "requests-unsubscribed": (("\\\"evt\\\":\\\"ACTIVITY_JOIN_REQUEST\\\",\\\"nonce", "\\\"evt\\\":\\\"NONE\\\",\\\"nonce"),
                              "handshake"),
    "no-spare-offered": (("\t\tactivity->spare = 1;\n", ""), "handshake"),
    "never-listened": (("p2p_party_standby(discord.shown && discord.sent.spare);", "p2p_party_standby(0);"),
                       "handshake"),
    "party-gives-code": (("snprintf(activity->secret, sizeof(activity->secret), \"%s\", party.link);",
                          "snprintf(activity->secret, sizeof(activity->secret), \"%s\", code);"), "states"),
    "hosting-unsaid": (('discord.game.hosting ? "Hosting " : ""', '""'), "states"),
    "quotes-unescaped": (("else if (*text == '\"' || *text == '\\\\')", "else if (0)"), "states"),
    "no-rate-limit": (("(!discord.sent_any || elapsed(discord.sent_time, ACTIVITY_INTERVAL))", "1"), "states"),
    "lan-gets-spare": (("\tif (in_game)\n\t{\n\t\tint count", "\tif (0)\n\t{\n\t\tint count"), "states"),
    "no-own-art": (("discord.own_art = strcmp(application, OPENCE_APPLICATION) != 0;", "discord.own_art = 0;"), "art"),
    "join-dropped": (("\t\t\t\tp2p_invite_received(secret);\n", ""), "join"),
    "spare-not-made": (("\t\t\tif (discord.sent.spare)\n\t\t\t\tp2p_party_spare_lead();\n", ""), "accept"),
    "accept-declines": (('send_request_command("SEND_ACTIVITY_JOIN_INVITE", request->id);',
                         'send_request_command("CLOSE_ACTIVITY_REQUEST", request->id);'), "accept"),
    "decline-invites": (('\t\t\tsend_request_command("CLOSE_ACTIVITY_REQUEST", request->id);',
                         '\t\t\tsend_request_command("SEND_ACTIVITY_JOIN_INVITE", request->id);'), "decline"),
    "asks-forever": (("(!allowed || elapsed(request->time, REQUEST_TIMEOUT))", "(!allowed)"), "timeout"),
    "game-gets-party": (("\t\tif (code[0] && count > 0 && maximum >= count)", "\t\tif (0)"), "game"),
    "presence-ignored": (('\tif (!config_boolean("discord.rich_presence"))\n\t\treturn 0;\n', ""), "off"),
    "joins-ignored": (('int joins = config_boolean("discord.allow_join_requests");', "int joins = 1;"), "nojoins"),
    "asks-kept-when-turned-off": (("requests_update(discord.shown && discord.sent.secret[0] && config_boolean("
                                   "\"discord.rich_presence\") &&\n\t\tconfig_boolean(\"discord.allow_join_requests\"));",
                                   "requests_update(1);"), "off"),
    "asks-kept-when-gone": (("\tmemset(discord.requests, 0, sizeof(discord.requests));\n\tp2p_party_standby(0);",
                             "\tp2p_party_standby(0);"), "reconnect"),
}


@pytest.mark.parametrize("fault", sorted(FAULTS))
def test_negative_control(fault):
    change, case = FAULTS[fault]
    status, output = run(build("discord_rpc", generated(change)), case)
    assert status == 1, f"{fault} was not caught by {case}: {output}"
