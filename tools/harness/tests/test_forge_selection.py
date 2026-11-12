"""The Forge map chosen in a map list marks the game hosted on its base map and no other, and a game made by name
lets the choice go (forge_layout.c's forge_custom_map_select and forge_layout_hosting, network_server_manager.c's
network_game_server_change_map_name): kept between games, the choice loaded its layout into games that asked for the
stock map and marked every game made afterwards as Forge's, which builds without Forge then could not join."""

import pytest

from tools.harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["none", "chosen", "by-name", "other-map", "loaded"]

# a fault, and the case that must catch it
NEGATIVE_CONTROLS = {
    "choice-kept": (("forge_custom_map_select(NONE)", "(void)0"), "by-name"),
    "any-map-marked": (("return selected != NONE && forge_layout_hosted_map_is(forge_custom_map_base_name(selected));",
                        "return selected != NONE;"), "other-map"),
    "lobby-unmarked": (("return selected != NONE && forge_layout_hosted_map_is(forge_custom_map_base_name(selected));",
                        "return FALSE;"), "chosen"),
    "loaded-unmarked": (("\tif (forge_layout_globals.current_slot != 0)\n\t\treturn TRUE;\n", ""), "loaded"),
}


def generated(fault=None):
    layout = read("port/linux/game/forge_layout.c")
    maps = layout[layout.index("static char const *const forge_layout_multiplayer_maps["):]
    maps = maps[:maps.index("};") + 2]
    text = maps + "\n\n" + "\n\n".join(function(layout, name) for name in (
        "forge_custom_map_get", "forge_custom_map_base_index", "forge_custom_map_base_name",
        "forge_layout_hosted_map_is", "forge_custom_map_select", "forge_layout_hosting", "forge_custom_map_selected"))
    # (what network_game_server_change_map_name does with the choice: its own statement)
    change = function(read("source/networking/network_server_manager.c"), "network_game_server_change_map_name")
    assert change.count("forge_custom_map_select(NONE);") == 1
    text += "\n#define CHANGE_MAP_NAME_CLEARS forge_custom_map_select(NONE)\n"
    if fault:
        text = mutated(text, *fault)
    return (("under_test.inc", text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("forge_selection", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("forge_selection", generated(fault)), case)
    assert status == CHECK_FAILED, f"a fault ({control}) passed '{case}'"


def test_the_lists_choose_after_the_servers_map_is_set():
    """the two map lists choose their Forge map after the call that lets a choice go"""
    handlers = read("source/interface/ui_widget_event_handler_functions.c")
    choose = handlers[handlers.index("forge_level = ui_widget_port_forge_level("):]
    assert choose.index("network_game_server_change_map_name(server, map_name);") < \
        choose.index("forge_custom_map_select(forge_level);")
    menu = read("port/linux/game/menu_functions.c")
    forge = menu[menu.index("if (!ui_widget_port_multiplayer_map_choose(ui_widget_port_forge_map_level(chosen)))"):]
    assert forge.index("forge_custom_map_select(chosen);") < 400
