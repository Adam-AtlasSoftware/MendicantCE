"""The cache files of decompressed maps (source/cache/cache_files_windows.c,
port/linux/src/map_cache.c): the disk's budget, every stock map kept, the
least useful map given up first for room (never the open map's, nor the
cache file a copy is going to), an unfinished copy never taken for a map,
and a copy ahead that never gives a map up."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CACHE = "source/cache/cache_files_windows.c"
BUDGET = "port/linux/src/map_unpack.c"
FUNCTIONS = ["cached_map_type_maximum_size", "cached_map_files_bytes", "cached_map_file_resize", "cached_map_usefulness",
             "cached_map_files_least_useful", "cached_map_files_make_room", "cached_map_files_find_free_map",
             "cache_files_map_prefill_size"]
CASES = ["budget", "keeps_every_stock_map", "least_useful_first", "room_made", "open_map_kept", "copy_in_progress_kept",
         "unfinished_copy", "ahead_never_gives_up", "type_limits"]


def generated(fault=None, budget_fault=None):
    source = read(CACHE)
    constants = "\n".join(f"#define {name} {constant(source, name)}" for name in
                          ("SOLO_CACHE_FILE_MAXIMUM_SIZE", "MAIN_MENU_CACHE_FILE_MAXIMUM_SIZE",
                           "MULTIPLAYER_CACHE_FILE_MAXIMUM_SIZE"))
    constants += f"\n#define NUMBER_OF_CACHED_MAP_FILES {constant(source, 'NUMBER_OF_CACHED_MAP_FILES')}\n"
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    budget_source = read(BUDGET)
    budget = "\n".join(re.findall(r"^#define MAP_CACHE_(?:MEGABYTE|FREE_\w+) .*$", budget_source, re.M)) + "\n" + \
        function(budget_source, "map_cache_choose_budget")
    if budget_fault:
        budget = mutated(budget, *budget_fault)
    return (("constants.inc", constants), ("under_test.inc", budget + "\n" + code))


@pytest.mark.parametrize("case", CASES)
def test_map_cache(case):
    status, output = run(build("map_cache", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [
    # the open map's cache file given up
    (("map_file_index == cache_file_globals.open_map_file_index ||", ""), "open_map_kept"),
    # a played map given up before one never played
    (("played < best_played ||", "played > best_played ||"), "least_useful_first"),
    # the most useful map given up first
    (("usefulness < best_usefulness", "usefulness > best_usefulness"), "least_useful_first"),
    # no map given up for room: the budget passed
    (("while (cached_map_files_bytes(map_file_index) +", "while (0 && cached_map_files_bytes(map_file_index) +"),
     "room_made"),
    # the cache file a copy is going to chosen
    (("!(cache_file_globals.copy_in_progress && cache_file_globals.copying_to_map_file_index == map_file_index) &&",
      ""), "copy_in_progress_kept"),
    # an unfinished copy taken for a map to give up
    (("map_file->file == INVALID_HANDLE_VALUE ||\n\t\t\t!map_file->header.name[0])",
      "map_file->file == INVALID_HANDLE_VALUE)"), "unfinished_copy"),
    # a copy ahead that gives up a map for its room
    (("cached_map_file_get(map_file_index)->header.name[0] ||\n", ""), "ahead_never_gives_up"),
])
def test_negative_control(fault, case):
    status, output = run(build("map_cache", generated(fault)), case)
    assert status == CHECK_FAILED, output


def test_budget_negative_control():
    """the free-space margin, the smaller of the two, fails the budget"""
    status, output = run(build("map_cache", generated(budget_fault=("margin < MAP_CACHE_FREE_MARGIN",
                                                                     "margin > MAP_CACHE_FREE_MARGIN"))), "budget")
    assert status == CHECK_FAILED, output
