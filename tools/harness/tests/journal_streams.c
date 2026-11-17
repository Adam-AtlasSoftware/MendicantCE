/* A map's preparation's record played back reads vertex streams only where
they are now (#1169: port/linux/src/d3d8_device.c's journal_stream_keep,
journal_stream_place and journal_streams_usable, and xbox_memory.c's
platform_contiguous_readable, compiled from the sources). A draw recorded
from a block of the contiguous window (XPhysicalAlloc's, as a Custom
Edition map's or a mod's geometry) that has since been let go, its pages
PROT_NONE, is skipped and counted unknown, never read; one still there is
drawn; a named geometry moved between two loads is found again where it is
now, at the same offset. The window here is a small one of real pages, so
that a read of a page let go faults as the game's did. */
#include "harness.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>

typedef int BOOL;
typedef unsigned long DWORD;

/* (the window wherever the host puts it: a 64-bit Mac's first 4 GB are none
of the process's; its "physical" addresses offsets in it, as the Xbox's) */
static unsigned long window_base;
#define PLATFORM_CONTIGUOUS_BASE window_base
#define PLATFORM_CONTIGUOUS_SIZE 0x1000000UL
#define PLATFORM_PHYSICAL_TO_VIRTUAL(physical) ((void *)((unsigned long)(physical) + PLATFORM_CONTIGUOUS_BASE))
#define PLATFORM_VIRTUAL_TO_PHYSICAL(address) ((unsigned long)(address) - PLATFORM_CONTIGUOUS_BASE)
#define PAGE_SIZE_BYTES 0x1000UL
#define CONTIGUOUS_PAGE_COUNT (0x1000000UL / PAGE_SIZE_BYTES)
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40

static DWORD page_protection[CONTIGUOUS_PAGE_COUNT];
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;

/* the device's streams and declaration, as setup_streams reads them */
#define XGPU_VERTEX_NONE_FORMAT 0
struct xgpu_vertex_element { unsigned char reg, stream, format, bytes; unsigned short offset; };
struct vertex_shader_object { unsigned long element_count; struct xgpu_vertex_element elements[16]; };
static struct
{
	struct vertex_shader_object *vertex_shader;
	struct { DWORD data; unsigned long stride; } streams[16];
} device;
static struct { unsigned long unknown; } journal;

#include "streams.inc"

/* a block of the window, as XPhysicalAlloc gives one, and let go */
static void *block_make(unsigned long offset, unsigned long size, unsigned char fill)
{
	void *at = (void *)(PLATFORM_CONTIGUOUS_BASE + offset);
	unsigned long page;

	CHECK(mmap(at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at, "block");
	for (page = offset / PAGE_SIZE_BYTES; page < (offset + size) / PAGE_SIZE_BYTES; page++)
		page_protection[page] = PAGE_READWRITE;
	memset(at, fill, size);
	return at;
}

static void block_free(void *at, unsigned long size)
{
	unsigned long offset = (unsigned long)at - PLATFORM_CONTIGUOUS_BASE, page;

	mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
	for (page = offset / PAGE_SIZE_BYTES; page < (offset + size) / PAGE_SIZE_BYTES; page++)
		page_protection[page] = 0;
}

static struct vertex_shader_object declaration = { 1, { { 0, 0, 1, 12, 0 } } };
#define STRIDE 32
#define BLOCK 0x4000

/* an indexed draw played back, as journal_play makes it: its stream from
the record, its one vertex at base read (as setup_streams' mirror reads it);
whether it was drawn, and the byte read */
static int play(const uint32_t kept[2], unsigned long base, unsigned char *read)
{
	BOOL lost;
	volatile const unsigned char *vertex;

	journal_lost_streams = 0;
	device.streams[0].data = journal_stream_place(kept, &lost);
	device.streams[0].stride = STRIDE;
	if (lost)
		journal_lost_streams |= 1u;
	if (!journal_streams_usable(base, 1))
	{
		journal.unknown++;
		return 0;
	}
	vertex = (volatile const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[0].data) + base * STRIDE;
	*read = *vertex;
	return 1;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	uint32_t kept[2];
	unsigned char read = 0;
	void *block, *moved;

	void *window = mmap(NULL, PLATFORM_CONTIGUOUS_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

	CHECK(window != MAP_FAILED, "window");
	window_base = (unsigned long)window;
	device.vertex_shader = &declaration;
	/* (top-down, as XPhysicalAlloc places what is not asked for at an address) */
	block = block_make(0xF00000, BLOCK, 0x5a);
	if (!strcmp(which, "freed") || !strcmp(which, "allocated"))
	{
		/* (a stock map's: no name, its address kept) */
		journal_stream_keep((uint32_t)PLATFORM_VIRTUAL_TO_PHYSICAL(block), kept);
		CHECK(kept[0] == 0, "an unnamed block kept by its address");
		if (!strcmp(which, "freed"))
			block_free(block, BLOCK);
		if (!strcmp(which, "freed"))
		{
			CHECK(!play(kept, 3, &read) && journal.unknown == 1, "a draw from memory let go skipped, unknown %lu",
				journal.unknown);
		}
		else
		{
			CHECK(play(kept, 3, &read) && read == 0x5a && journal.unknown == 0, "a draw from memory there made");
		}
		return 0;
	}
	if (!strcmp(which, "moved") || !strcmp(which, "lost"))
	{
		/* (a Custom Edition map's structure, named; its next load's top-down
		place another, below what else is loaded now) */
		d3d8_geometry_named(block, BLOCK, "custom edition structure 16384");
		journal_stream_keep((uint32_t)PLATFORM_VIRTUAL_TO_PHYSICAL(block) + 5 * STRIDE, kept);
		CHECK(kept[0] != 0 && kept[1] == 5 * STRIDE, "a named block kept by its name and offset");
		d3d8_geometry_unnamed(block);
		block_free(block, BLOCK);
		if (!strcmp(which, "lost"))
		{
			CHECK(!play(kept, 0, &read) && journal.unknown == 1, "a draw of geometry not loaded now skipped");
			return 0;
		}
		moved = block_make(0xE00000, BLOCK, 0x77);
		d3d8_geometry_named(moved, BLOCK, "custom edition structure 16384");
		CHECK(play(kept, 0, &read) && read == 0x77 && journal.unknown == 0,
			"the moved geometry's vertex read where it is now (read %02x, unknown %lu)", read, journal.unknown);
		CHECK(device.streams[0].data == PLATFORM_VIRTUAL_TO_PHYSICAL(moved) + 5 * STRIDE, "at the same offset");
		return 0;
	}
	CHECK(0, "no case %s", which);
	return 0;
}
