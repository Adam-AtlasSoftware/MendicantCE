/*
CONTIGUOUS_FIT.H

Where a block of the contiguous window goes (xbox_memory.c), over its page
tables: per page, whether it is in use (its protection, a DWORD, non-zero)
and its block's first page; per block's first page, its length. Plain
functions of those tables, with no lock and no mapping, so that a test
compiles them as they are (tools/test_contiguous_fit.py).

contiguous_fit_top_down: the highest place, as the Xbox's allocator and
XPhysicalAlloc place everything not placed at an address: the caches, the
Direct3D resources.

contiguous_fit_bottom_up: the lowest place (never page 0, whose physical
address 0 reads as no data in a resource's Data field). For the large
blocks a BSP keeps while it is the one loaded (the enhanced world
geometry: port/linux/game/prism_world_geometry.c): placed top-down, the
small blocks made while it is loaded (textures made as the level plays)
are placed right below it, and when it goes the next BSP's block, larger,
finds only the room it left (b30's BSP 1, 120708 KB, after BSP 0's 109328
KB, with 3 MB of textures made below it: 110680 KB free in one run). From
the bottom it is kept apart from them, and the next finds its room and the
rest of the free run above it.
*/

#ifndef CONTIGUOUS_FIT_H
#define CONTIGUOUS_FIT_H

/* the highest page of [first, first + count) in use; -1 if they are all free */
static long contiguous_fit_used_highest(const DWORD *protection, unsigned long first, unsigned long count)
{
	unsigned long page;

	for (page = first + count; page > first; page--)
	{
		if (protection[page - 1])
			return (long)(page - 1);
	}
	return -1;
}

/* the first page of the highest place of count pages, at a multiple of
alignment_pages, among page_count; page_count if there is none. A place with
a page in use is passed with every place below it that would still hold a
page of that page's block: none of them fits (the menus' hundreds of
pictures each walked the caches' blocks a page at a time, a tenth of a
second) */
static unsigned long contiguous_fit_top_down(const DWORD *protection, const unsigned long *block_first,
	unsigned long page_count, unsigned long count, unsigned long alignment_pages)
{
	unsigned long candidate;

	if (!count || count > page_count || !alignment_pages)
		return page_count;
	candidate = page_count - count;
	for (;;)
	{
		long used;

		candidate -= candidate % alignment_pages;
		used = contiguous_fit_used_highest(protection, candidate, count);
		if (used < 0)
			return candidate;
		if (block_first[used] < count)
			return page_count;
		candidate = block_first[used] - count;
	}
}

/* the first page of the lowest place of count pages, at a multiple of
alignment_pages, among page_count (never page 0); page_count if there is
none. A place with a page in use is passed with every place that would
still hold that page's block: the search goes on from the block's end */
static unsigned long contiguous_fit_bottom_up(const DWORD *protection, const unsigned long *block_first,
	const unsigned long *block_length, unsigned long page_count, unsigned long count, unsigned long alignment_pages)
{
	unsigned long candidate = 1;

	if (!count || count > page_count || !alignment_pages)
		return page_count;
	for (;;)
	{
		long used;
		unsigned long next;

		candidate = (candidate + alignment_pages - 1) / alignment_pages * alignment_pages;
		if (candidate < 1 || candidate > page_count - count)
			return page_count;
		used = contiguous_fit_used_highest(protection, candidate, count);
		if (used < 0)
			return candidate;
		next = block_first[used] + block_length[block_first[used]];
		/* (a block's tables that do not reach past the page found in use:
		the page after it) */
		candidate = next > (unsigned long)used ? next : (unsigned long)used + 1;
	}
}

#endif
