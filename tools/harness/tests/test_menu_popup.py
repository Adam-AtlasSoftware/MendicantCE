"""The menus' pop-ups (ui_widget.c's ui_widget_port_popups; docs/menu_screens.md, "A pop-up"): a hand-written screen
with a <dialog> (the Quit question, Save Changes) opens over the screen up without deleting it or adding to its
history, so a screen that decides as it is deleted (the new campaign's creating_profile) waits, and the editor
under Save Changes is the same editor when it closes. Its back closes it alone; a screen opened from it closes it
and the history remembers the screen under it; the screen under it deleted, every screen closed, or the map let go
of closes it; over no screen (a game's) it takes its player's events and is drawn in its player's view. Each with a
negative control, the first the old way: the screen under it deleted."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["kept", "back", "opened", "replaced", "closed", "game"]

NAMES = ("widget_instance_get_topmost_parent", "push_widget", "pop_widget", "dispose_widget_stack",
         "ui_widget_port_top_screen", "ui_widget_port_popup_in_view", "ui_widget_port_screen_gone",
         "ui_widget_load_by_name_or_tag", "widget_instance_go_back_to_previous", "ui_widgets_close_all",
         "ui_widget_port_popups_close")

NEGATIVE_CONTROLS = {
    # the old way: every screen opened in the place of the one up, which is deleted and kept in the history
    "under-deleted": ("ui_widget_load_by_name_or_tag", ("if (!parent && menu_screen_popup(tag_index))",
                                                        "if (!parent && menu_screen_popup(tag_index) && FALSE)"),
                      "kept"),
    # the screen under the pop-up given the events
    "events-under": ("ui_widget_port_top_screen", ("return ui_widget_port_popups[widget_stack] ?",
                                                   "return 0 ?"), "kept"),
    # a pop-up's back going back through the history, as a screen's
    "back-history": ("widget_instance_go_back_to_previous",
                     ("\t\t\tui_widget_delete(ui_widget_port_popups[popup_index]);\n\t\t\treturn;\n", ""), "back"),
    # the history remembering the pop-up a screen was opened from
    "history-popup": ("ui_widget_load_by_name_or_tag",
                      ("invoking_widget_tag = widget_globals.active_widgets[widget_stack] ?",
                       "invoking_widget_tag = FALSE ?"), "opened"),
    # a pop-up closed as the screen under it is replaced by another than it opened
    "replaced": ("ui_widget_load_by_name_or_tag", ("\t\t\t\tkept_popup = ui_widget_port_popups[widget_stack];\n",
                                                   "\t\t\t\tif (ui_widget_port_popups[widget_stack])\n"
                                                   "\t\t\t\t\tui_widget_delete(ui_widget_port_popups[widget_stack]);\n"),
                 "replaced"),
    # a pop-up left over a screen deleted
    "outlives": ("ui_widget_port_screen_gone",
                 ("\t\t\tif (ui_widget_port_popups[widget_index])\n\t\t\t\tui_widget_delete(ui_widget_port_popups"
                  "[widget_index]);\n", ""), "closed"),
    # close all missing a pop-up over no screen
    "close-all": ("ui_widgets_close_all",
                  ("\t\tif (ui_widget_port_popups[local_player_index])\n\t\t\tui_widget_delete(ui_widget_port_popups"
                   "[local_player_index]);\n", ""), "closed"),
    # a pop-up over a game drawn in every view
    "every-view": ("ui_widget_port_popup_in_view", ("popup->local_player_index == local_player_index",
                                                    "TRUE"), "game"),
}


def generated(fault=None):
    widgets = read("source/interface/ui_widget.c")
    code = {name: function(widgets, name) for name in NAMES}
    if fault:
        where, change, _ = NEGATIVE_CONTROLS[fault]
        code[where] = mutated(code[where], *change)
    under_test = "\n".join(code[name] for name in NAMES) + "\n"
    under_test = under_test.replace("struct widget_instance *widget_instance_get_topmost_parent(",
                                    "static struct widget_instance *widget_instance_get_topmost_parent(", 1)
    return (("under_test.inc", under_test),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("menu_popup", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("menu_popup", generated(control)), NEGATIVE_CONTROLS[control][2])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def dialog_problems(menu_screen):
    """What keeps the Quit question and Save Changes from being pop-ups over the screen they were opened from: a
    <dialog> not a pop-up (menu_screen_popup), or the dialog still copying the screen before into itself."""
    problems = []
    if "MENU_SCREEN_DIALOG" not in function(menu_screen, "menu_screen_popup"):
        problems.append("a <dialog> is not a pop-up")
    build_dialog = function(menu_screen, "dialog_build")
    if "previous_screen" in build_dialog or "dialog_still" in menu_screen:
        problems.append("the dialog copies the screen before")
    if "screens.dialog_dim_tag" not in build_dialog:
        problems.append("the screen under it is not dimmed")
    return problems


def test_dialogs_are_popups():
    for name in ("quit", "save_changes"):
        assert "<dialog " in read(f"port/assets/menus/screens/{name}.xml"), name
    assert dialog_problems(read("port/linux/game/menu_screen.c")) == []


def test_dialogs_are_popups_control():
    menu_screen = read("port/linux/game/menu_screen.c")
    popup = function(menu_screen, "menu_screen_popup")
    assert dialog_problems(menu_screen.replace(popup, popup.replace("MENU_SCREEN_DIALOG", "MENU_SCREEN_LIST")))
