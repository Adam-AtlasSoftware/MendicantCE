"""The menus' Spartan's objects after the map they were made in (#1186, port/linux/game/menu_spartan.c): the scene's
view leaves out only the Spartans' own objects (menu_spartan_object), not the next map's objects that have the same
indices (ratrace's teleporters after the ui map's Spartan and rifle), nor another object in a Spartan's slot; and
spartan_delete deletes neither."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["own", "map-changed", "slot-reused"]

SPARTAN = "port/linux/game/menu_spartan.c"

# a fault in the code and the case that must catch it
NEGATIVE_CONTROLS = {
    # the indices matched alone, as before #1186: a teleporter left out of the view
    "index-alone": ("(menu_spartan.spartans[index].object_index == object_index &&\n"
                    "\t\t\t\tspartan_object_current(object_index, menu_spartan.spartan_definition))",
                    "menu_spartan.spartans[index].object_index == object_index", "map-changed"),
    # the tags' generation not checked: an old tag's index taken for the new map's
    "generation-ignored": ("definition_index == NONE ||\n\t\tmenu_spartan.generation != cache_files_tags_generation()",
                           "definition_index == NONE", "map-changed"),
    # the definition not checked: another object in the slot taken for the Spartan's
    "definition-ignored": ("return object && object->definition_index == definition_index;", "return object != NULL;",
                           "slot-reused"),
}


def generated(spartan):
    code = "\n\n".join(function(spartan, name) for name in
                       ("spartan_object_current", "spartan_delete", "menu_spartan_object"))
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_menu_spartan_stale(case):
    status, output = run(build("menu_spartan_stale", generated(read(SPARTAN))), case)
    assert status == 0, output


@pytest.mark.parametrize("name", sorted(NEGATIVE_CONTROLS))
def test_negative_control(name):
    before, after, case = NEGATIVE_CONTROLS[name]
    status, output = run(build("menu_spartan_stale", generated(mutated(read(SPARTAN), before, after))), case)
    assert status == CHECK_FAILED, f"{name}: {case} did not catch it ({status}): {output}"
