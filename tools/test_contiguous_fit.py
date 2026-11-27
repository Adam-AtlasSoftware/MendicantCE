"""Where the contiguous window places a block (port/linux/src/contiguous_fit.h, xbox_memory.c's allocator): the
header compiled as it is under a small driver that keeps the window's page tables as xbox_memory.c does, against a
reference in Python, and the enhanced world geometry's case (b30's BSP switch) played through both placements.
Needs a C compiler (clang, or CC), not the game.

    python -m pytest -q tools/test_contiguous_fit.py
"""

import os
import random
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "port/linux/src/contiguous_fit.h"

# a: "a <pages> <alignment pages> <t|b>" places a block (top-down or bottom-up) and prints its first page, or -1;
# "p <first> <pages>" places one at a page, as the game state and the tag cache are; "f <first>" frees a block
DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long DWORD;
#include "contiguous_fit.h"

int main(int argc, char **argv)
{
	unsigned long pages = strtoul(argv[1], NULL, 0), *first_of, *length, a, b, page;
	DWORD *protection;
	char kind, place;

	protection = calloc(pages, sizeof(*protection));
	first_of = calloc(pages, sizeof(*first_of));
	length = calloc(pages, sizeof(*length));
	while (scanf(" %c", &kind) == 1)
	{
		unsigned long first = pages;

		if (kind == 'f')
		{
			if (scanf("%lu", &a) != 1)
				return 2;
			for (page = a; page < a + length[a]; page++)
				protection[page] = 0;
			length[a] = 0;
			continue;
		}
		if (kind == 'p')
		{
			if (scanf("%lu %lu", &a, &b) != 2)
				return 2;
			first = a;
		}
		else
		{
			if (scanf("%lu %lu %c", &a, &b, &place) != 3)
				return 2;
			first = place == 't' ? contiguous_fit_top_down(protection, first_of, pages, a, b) :
				contiguous_fit_bottom_up(protection, first_of, length, pages, a, b);
			b = a;
		}
		if (first == pages)
		{
			printf("-1\n");
			continue;
		}
		for (page = first; page < first + b; page++)
		{
			protection[page] = 4;
			first_of[page] = first;
		}
		length[first] = b;
		printf("%lu\n", first);
	}
	return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    compiler = os.environ.get("CC", "clang")
    if not shutil.which(compiler):
        if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS"):
            pytest.fail(f"no {compiler}")
        pytest.skip(f"no {compiler}")
    work = tmp_path_factory.mktemp("contiguous_fit")
    shutil.copy(HEADER, work / "contiguous_fit.h")
    (work / "driver.c").write_text(DRIVER)
    subprocess.run([compiler, "-std=gnu89", "-O1", "-Wall", "-Werror", "-Wno-unused-function", "-I", str(work),
                    str(work / "driver.c"), "-o", str(work / "driver")], check=True)
    return work / "driver"


def run(driver, pages, commands):
    out = subprocess.run([str(driver), str(pages)], input="\n".join(commands) + "\n", capture_output=True, text=True,
                         check=True).stdout.split()
    return [int(value) for value in out]


class Window:
    """the reference: the same tables, the places found by trying every page"""

    def __init__(self, pages):
        self.pages = pages
        self.used = [False] * pages
        self.blocks = {}

    def free_at(self, first, count):
        return first + count <= self.pages and not any(self.used[first:first + count])

    def place(self, first, count):
        for page in range(first, first + count):
            self.used[page] = True
        self.blocks[first] = count
        return first

    def top_down(self, count, alignment):
        if count < 1 or count > self.pages:
            return -1
        start = (self.pages - count) // alignment * alignment
        for first in range(start, -1, -alignment):
            if self.free_at(first, count):
                return self.place(first, count)
        return -1

    def bottom_up(self, count, alignment):
        if count < 1 or count > self.pages:
            return -1
        first = alignment
        while first + count <= self.pages:
            if self.free_at(first, count):
                return self.place(first, count)
            first += alignment
        return -1

    def free(self, first):
        for page in range(first, first + self.blocks.pop(first)):
            self.used[page] = False


def test_both_find_the_place_every_page_tried_would(driver):
    """top-down the highest aligned place, bottom-up the lowest (never page 0), on long random sequences of blocks
    placed and freed; neither ever overlaps a block (the same answers as the reference, which tries every place)"""
    rng = random.Random(1169)
    for trial in range(6):
        pages = rng.choice([64, 257, 1024])
        window = Window(pages)
        commands, expected = [], []
        live = []
        for step in range(400):
            if live and rng.random() < 0.35:
                first = live.pop(rng.randrange(len(live)))
                window.free(first)
                commands.append(f"f {first}")
                continue
            count = rng.choice([1, 1, 2, 3, 5, 8, 13, rng.randrange(1, pages // 3)])
            alignment = rng.choice([1, 1, 2, 4, 16])
            kind = rng.choice("tb")
            first = window.top_down(count, alignment) if kind == "t" else window.bottom_up(count, alignment)
            commands.append(f"a {count} {alignment} {kind}")
            expected.append(first)
            if first >= 0:
                live.append(first)
        assert run(driver, pages, commands) == expected, trial


def test_never_page_zero_and_none_when_full(driver):
    """physical address 0 is no data in a resource's Data field: bottom-up starts at page 1 (or the first aligned
    page after it); a block larger than the room, or than the window, is not placed"""
    assert run(driver, 16, ["a 1 1 b", "a 1 4 b", "a 4 8 b", "a 2 1 b"]) == [1, 4, 8, 2]
    assert run(driver, 16, ["a 16 1 b", "a 15 1 b", "a 1 1 b"]) == [-1, 1, -1]
    assert run(driver, 16, ["a 17 1 t", "a 16 1 t", "a 1 1 t"]) == [-1, 0, -1]
    assert run(driver, 16, ["a 0 1 b", "a 0 1 t"]) == [-1, -1]


def test_the_bsp_switch_that_ran_out_of_memory(driver):
    """b30 under World Geometry Enhanced and Enhanced lighting, in 64 KB pages of the 512 MB window as it was
    measured: the tag cache and the game state placed, the texture and sound caches top-down, small blocks below
    them with holes too small for a texture between them (the Direct3D resources made as the map loaded), 181 MB
    free in one run below; BSP 0's geometry (109328 KB, 4 MB aligned), then 3 MB of textures made as it plays
    (1 MB and 512 KB each), top-down; BSP 0's geometry freed at the switch, and BSP 1's (120708 KB) asked for. From
    the top, the textures sit right below BSP 0's block and BSP 1's does not fit (the "out of memory" of the log);
    from the bottom it does, where BSP 0's was"""
    kb = 64
    pages = 512 * 1024 // kb
    sound_bottom = pages - (262144 + 16384) // kb
    fixed = ["p %d %d" % (0x3A6000 // 1024 // kb, 22528 // kb), "p %d %d" % (0x1A00000 // 1024 // kb, 20480 // kb),
             "a %d 1 t" % (262144 // kb), "a %d 1 t" % (16384 // kb)]
    fixed += ["p %d 3" % (sound_bottom - 6 * (k + 1)) for k in range(50)]
    textures = ["a 16 1 t", "a 16 1 t", "a 8 1 t", "a 8 1 t"]
    align = 4096 // kb
    for kind, fits in (("t", False), ("b", True)):
        commands = fixed + ["a %d %d %s" % (109328 // kb + 1, align, kind)] + textures
        placed = run(driver, pages, commands)
        bsp0 = placed[len(fixed)]
        assert bsp0 >= 0 and min(placed[len(fixed) + 1:]) >= 0
        commands += ["f %d" % bsp0, "a %d %d %s" % (120708 // kb + 1, align, kind)]
        bsp1 = run(driver, pages, commands)[-1]
        assert (bsp1 >= 0) == fits, kind
        if fits:
            # (where BSP 0's was, the textures above both)
            assert bsp1 == bsp0
            assert all(first > bsp1 + 120708 // kb for first in placed[len(fixed) + 1:])
        else:
            # (textures right below BSP 0's block, those of no room left above it)
            assert min(placed[len(fixed) + 1:]) < bsp0


def test_the_world_geometry_is_placed_from_the_bottom():
    """the enhanced world's block and its batches' indices are a BSP's: placed from the bottom
    (platform_contiguous_alloc_low), never with XPhysicalAlloc, which places from the top; the allocator's
    placements are contiguous_fit.h's"""
    geometry = (ROOT / "port/linux/game/prism_world_geometry.c").read_text()
    assert "world_geometry.geometry = platform_contiguous_alloc_low(total + 1, alignment);" in geometry
    assert "platform_contiguous_alloc_low((unsigned long)most * 3 * sizeof(word) + 64, 0x1000)" in geometry
    assert "XPhysicalAlloc(" not in geometry
    memory = (ROOT / "port/linux/src/xbox_memory.c").read_text()
    assert '#include "contiguous_fit.h"' in memory
    assert "contiguous_fit_top_down(" in memory and "contiguous_fit_bottom_up(" in memory
    assert "pages_used_highest" not in memory
    fixups = (ROOT / "port/linux/include/halo_linux_source_fixups.h").read_text()
    assert "void *platform_contiguous_alloc_low(unsigned long size, unsigned long alignment);" in fixups
