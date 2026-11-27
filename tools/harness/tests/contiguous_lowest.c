/* The port's contiguous window (port/linux/src/xbox_memory.c): a block asked
at PLATFORM_LOWEST_PHYSICAL_ADDRESS goes at the lowest place it fits (bottom
up, its alignment kept, a block in the way passed whole), so that the
enhanced world's geometry, freed at a BSP change and asked for again larger,
finds the freed block and the free memory past it as one run, whatever the
game's own top-down blocks did meanwhile. */
#include "harness.h"
#include <stdint.h>

typedef unsigned long DWORD;
typedef int BOOL;
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOMBINE 0x400
#define PAGE_NOCACHE 0x200
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PLATFORM_CONTIGUOUS_BASE 0x80000000UL
#define PLATFORM_CONTIGUOUS_SIZE 0x20000000UL
#define PLATFORM_ANY_PHYSICAL_ADDRESS 0xffffffffUL
#define PLATFORM_LOWEST_PHYSICAL_ADDRESS 0xfffffffeUL
#define PAGE_SIZE_BYTES 0x1000UL
#define CONTIGUOUS_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES)

static DWORD page_protection[CONTIGUOUS_PAGE_COUNT];
static unsigned long block_page_count[CONTIGUOUS_PAGE_COUNT];
static unsigned long block_first_page[CONTIGUOUS_PAGE_COUNT];
static BOOL arena_reserved = TRUE;
/* (no window mapped: the blocks' addresses are only counted) */
#include <pthread.h>
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define MAP_FIXED 0
#define MAP_NORESERVE 0
#define mmap(address, size, protection, flags, fd, offset) ((void)(size), (void)(protection), (address))
#define memory_watch_forget(address, size) ((void)(address), (void)(size))
static int halo_fresh_memory_fill(void) { return 0; }

#include "under_test.inc"

static unsigned long page_of(void *address)
{
	return ((unsigned long)(uintptr_t)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
}

static void *lowest(unsigned long pages, unsigned long alignment)
{
	return platform_contiguous_alloc(pages * PAGE_SIZE_BYTES, alignment, PLATFORM_LOWEST_PHYSICAL_ADDRESS,
		PAGE_READWRITE);
}

static void *any(unsigned long pages)
{
	return platform_contiguous_alloc(pages * PAGE_SIZE_BYTES, 0, PLATFORM_ANY_PHYSICAL_ADDRESS, PAGE_READWRITE);
}

static void *at(unsigned long page, unsigned long pages)
{
	return platform_contiguous_alloc(pages * PAGE_SIZE_BYTES, 0, page * PAGE_SIZE_BYTES, PAGE_READWRITE);
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "switch"))
	{
		/* (the game's fixed block low, its caches from the top; BSP 0's
		geometry, then textures from the top, BSP 0 freed, BSP 1's larger
		geometry: it starts where BSP 0's did) */
		void *fixed = at(100, 50), *bsp0, *bsp1, *top;
		unsigned long page;

		CHECK(fixed != NULL, "the fixed block");
		top = any(30000);
		bsp0 = lowest(27000, 0x400000);
		CHECK(bsp0 && page_of(bsp0) == 1024, "BSP 0's block at page %lu", bsp0 ? page_of(bsp0) : 0UL);
		for (page = 0; page < 40; page++)
			CHECK(any(1000) != NULL, "a texture");
		platform_contiguous_free(bsp0);
		bsp1 = lowest(30200, 0x400000);
		CHECK(bsp1 && page_of(bsp1) == page_of(bsp0), "BSP 1's block at page %lu, BSP 0's at %lu",
			bsp1 ? page_of(bsp1) : 0UL, page_of(bsp0));
		CHECK(top != NULL, "the top block");
	}
	else if (!strcmp(which, "passes-blocks"))
	{
		/* (blocks in the way passed whole; the first free run that holds it) */
		void *a = at(0, 10), *b = at(12, 10), *c;

		CHECK(a && b, "the blocks");
		c = lowest(5, 0);
		CHECK(c && page_of(c) == 22, "a 5-page block at %lu", c ? page_of(c) : 0UL);
		c = lowest(2, 0);
		CHECK(c && page_of(c) == 10, "a 2-page block at %lu (the gap between)", c ? page_of(c) : 0UL);
	}
	else if (!strcmp(which, "aligned"))
	{
		void *a = at(0, 3), *c;

		CHECK(a != NULL, "the block");
		c = lowest(4, 0x10000);
		CHECK(c && page_of(c) == 16, "a 64 KB-aligned block at %lu", c ? page_of(c) : 0UL);
	}
	else if (!strcmp(which, "full"))
	{
		void *whole = lowest(CONTIGUOUS_PAGE_COUNT - 8, 0);

		CHECK(whole && page_of(whole) == 0, "the window's block");
		CHECK(lowest(9, 0) == NULL, "a block past what is free");
		CHECK(lowest(8, 0) != NULL, "the last 8 pages");
		CHECK(lowest(1, 0) == NULL, "a page of a full window");
	}
	else
	{
		fprintf(stderr, "no case %s\n", which);
		return 2;
	}
	return 0;
}
