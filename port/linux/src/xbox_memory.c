/*
XBOX_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) and page protection for the Linux
build.

On the Xbox, physical memory at address P is visible at virtual address
0x80000000 + P. The game asks for its game state and tag cache at fixed
addresses that way (physical_memory_map.c), and Direct3D resources carry
physical addresses in their Data fields. A 32-bit Linux process on a 64-bit
kernel owns the whole 4 GB address space, so the layer reserves the same
virtual window at start-up and allocates page-granular blocks inside it:
placed requests at exactly the address asked for, the rest top-down as the
Xbox kernel does.

Halo Custom Edition maps need the window their tag data is linked to,
0x40440000, reserved the same way when the game.custom_edition setting is on
(port/linux/game/custom_edition_cache.c).
*/

#include "platform.h"
#include "port_config.h"
#include "../game/cache_file_formats.h"
#ifdef HALO_GUEST
/* the window the guests' host reserves (port/android/host/host_memory.c) */
#include "halo_android_abi.h"
#endif

#include "contiguous_fit.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#ifdef HALO_GUEST
typedef char window_is_the_hosts[PLATFORM_CONTIGUOUS_BASE == HALO_GUEST_WINDOW_BASE &&
	PLATFORM_CONTIGUOUS_SIZE == HALO_GUEST_WINDOW_SIZE ? 1 : -1];
#endif
/* the texture and sound caches, which the window hands out top-down, fit
above the game state (halo_port_capacity.h) */
typedef char caches_fit_the_window[HALO_PORT_GAME_STATE_BASE_ADDRESS + HALO_PORT_GAME_STATE_SIZE -
	PLATFORM_CONTIGUOUS_BASE + HALO_PORT_TEXTURE_CACHE_SIZE + HALO_PORT_SOUND_CACHE_SIZE <=
	PLATFORM_CONTIGUOUS_SIZE ? 1 : -1];

#define PAGE_SIZE_BYTES 0x1000UL
#define CONTIGUOUS_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES)

/* per page: 0 free, otherwise the protection of the block (PAGE_*); the
first page of a block also records the block length */
static DWORD page_protection[CONTIGUOUS_PAGE_COUNT];
static unsigned long block_page_count[CONTIGUOUS_PAGE_COUNT];
/* each page in use: its block's first page (platform_contiguous_alloc's
search passes a block at once) */
static unsigned long block_first_page[CONTIGUOUS_PAGE_COUNT];
/* the pages in use now, and the most there have been */
static unsigned long pages_in_use, pages_high_water;
static BOOL arena_reserved = FALSE;
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;
static void *custom_edition_tag_cache = NULL;

static void contiguous_log_blocks(void);

static int protection_to_host(DWORD protect)
{
	switch (protect & 0xff)
	{
	case PAGE_NOACCESS: return PROT_NONE;
	case PAGE_READONLY: return PROT_READ;
	case PAGE_EXECUTE: return PROT_EXEC;
	case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
	case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return PROT_READ | PROT_WRITE;
	}
}

/* Reserve the window before anything else can map into it. */
__attribute__((constructor(101)))
static void contiguous_arena_reserve(void)
{
	void *wanted = (void *)PLATFORM_CONTIGUOUS_BASE;
	void *result = mmap(wanted, PLATFORM_CONTIGUOUS_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

	if (result == wanted)
	{
		arena_reserved = TRUE;
	}
	else
	{
		if (result != MAP_FAILED)
			munmap(result, PLATFORM_CONTIGUOUS_SIZE);
		platform_log("cannot reserve the Xbox contiguous memory window at %p (%s)",
			wanted, strerror(errno));
	}
}

/* The Custom Edition tag cache, reserved before anything else can map into
it (its pages are backed as they are touched); only when Custom Edition maps
may run, as it takes 23 MB of the address space. */
__attribute__((constructor(102)))
static void custom_edition_tag_cache_reserve(void)
{
	void *wanted = (void *)CUSTOM_EDITION_TAG_CACHE_ADDRESS;
	void *result;

	if (!config_boolean("game.custom_edition"))
		return;
	result = mmap(wanted, CUSTOM_EDITION_TAG_CACHE_BYTES, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (result == wanted)
	{
		custom_edition_tag_cache = result;
	}
	else
	{
		if (result != MAP_FAILED)
			munmap(result, CUSTOM_EDITION_TAG_CACHE_BYTES);
		platform_log("cannot reserve the Custom Edition tag cache at %p (%s): Custom Edition maps cannot run",
			wanted, strerror(errno));
	}
}

void *halo_custom_edition_tag_cache(void)
{
#ifdef HALO_WEB
	return web_custom_edition_tag_cache();
#else
	return custom_edition_tag_cache;
#endif
}

/* sysinfo's total, which the guests' hosts give (the Linux kernel's, or the
macOS and iOS host's hw.memsize: port/macos/host_syscall.c), read once */
unsigned long long platform_memory_bytes(void)
{
	static unsigned long long bytes;
	static int read;
	struct sysinfo info;

	if (!read)
	{
		read = 1;
		if (sysinfo(&info) == 0)
			bytes = (unsigned long long)info.totalram * (info.mem_unit ? info.mem_unit : 1);
	}
	return bytes;
}

BOOL platform_is_contiguous(const void *address)
{
	unsigned long value = (unsigned long)address;

	return value >= PLATFORM_CONTIGUOUS_BASE && value - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
}

BOOL platform_contiguous_readable(const void *address, unsigned long size)
{
	unsigned long value = (unsigned long)address, first, last, page;
	BOOL readable = TRUE;

	if (!size)
		size = 1;
	if (!platform_is_contiguous(address) || size > PLATFORM_CONTIGUOUS_SIZE - (value - PLATFORM_CONTIGUOUS_BASE))
		return FALSE;
	first = (value - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	last = (value - PLATFORM_CONTIGUOUS_BASE + size - 1) / PAGE_SIZE_BYTES;
	pthread_mutex_lock(&arena_lock);
	for (page = first; page <= last && readable; page++)
		readable = page_protection[page] && (protection_to_host(page_protection[page]) & PROT_READ);
	pthread_mutex_unlock(&arena_lock);
	return readable;
}

static BOOL pages_free(unsigned long first, unsigned long count)
{
	unsigned long page;

	if (first + count > CONTIGUOUS_PAGE_COUNT)
		return FALSE;
	for (page = first; page < first + count; page++)
	{
		if (page_protection[page])
			return FALSE;
	}
	return TRUE;
}

/* a block's place: CONTIGUOUS_TOP_DOWN, CONTIGUOUS_BOTTOM_UP, or the page
offset in the window it is placed at */
#define CONTIGUOUS_TOP_DOWN PLATFORM_ANY_PHYSICAL_ADDRESS
#define CONTIGUOUS_BOTTOM_UP (PLATFORM_ANY_PHYSICAL_ADDRESS - 1)

static void *contiguous_alloc(unsigned long size, unsigned long alignment, unsigned long place, DWORD protect)
{
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long alignment_pages = alignment > PAGE_SIZE_BYTES ? alignment / PAGE_SIZE_BYTES : 1;
	unsigned long first = CONTIGUOUS_PAGE_COUNT;
	unsigned long page;
	void *address;

	if (!count)
		count = 1;
	protect &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	if (!protect)
		protect = PAGE_READWRITE;

	pthread_mutex_lock(&arena_lock);
	if (!arena_reserved)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
	if (place == CONTIGUOUS_TOP_DOWN)
	{
		/* top-down first fit, like the Xbox contiguous allocator
		(contiguous_fit.h) */
		first = contiguous_fit_top_down(page_protection, block_first_page, CONTIGUOUS_PAGE_COUNT, count,
			alignment_pages);
	}
	else if (place == CONTIGUOUS_BOTTOM_UP)
	{
		first = contiguous_fit_bottom_up(page_protection, block_first_page, block_page_count, CONTIGUOUS_PAGE_COUNT,
			count, alignment_pages);
	}
	else
	{
		unsigned long wanted = place / PAGE_SIZE_BYTES;

		if (pages_free(wanted, count))
			first = wanted;
	}
	if (first == CONTIGUOUS_PAGE_COUNT)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}

	address = (void *)(PLATFORM_CONTIGUOUS_BASE + first * PAGE_SIZE_BYTES);
	memory_watch_forget(address, count * PAGE_SIZE_BYTES);
	/* map fresh zeroed pages over the reservation */
	if (mmap(address, count * PAGE_SIZE_BYTES, protection_to_host(protect),
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
	for (page = first; page < first + count; page++)
	{
		page_protection[page] = protect;
		block_first_page[page] = first;
	}
	block_page_count[first] = count;
	pages_in_use += count;
	if (pages_in_use > pages_high_water)
		pages_high_water = pages_in_use;
	pthread_mutex_unlock(&arena_lock);
	/* (debug.fresh_memory_fill: its pattern over the fresh zeroed pages) */
	if (halo_fresh_memory_fill() > 0 && (protection_to_host(protect) & PROT_WRITE))
		memset(address, halo_fresh_memory_fill(), count * PAGE_SIZE_BYTES);
	return address;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	return contiguous_alloc(size, alignment, physical_address == PLATFORM_ANY_PHYSICAL_ADDRESS ? CONTIGUOUS_TOP_DOWN :
		physical_address & ~PLATFORM_CONTIGUOUS_BASE, protect);
}

void *platform_contiguous_alloc_low(unsigned long size, unsigned long alignment)
{
	void *result = contiguous_alloc(size, alignment, CONTIGUOUS_BOTTOM_UP, PAGE_READWRITE);

	if (!result)
	{
		platform_log("contiguous memory: cannot place %lu bytes from the bottom", size);
		contiguous_log_blocks();
	}
	return result;
}

void platform_contiguous_usage(unsigned long *in_use, unsigned long *high_water, unsigned long *largest_free)
{
	unsigned long page, run = 0, largest = 0;

	pthread_mutex_lock(&arena_lock);
	for (page = 0; page < CONTIGUOUS_PAGE_COUNT; page++)
	{
		run = page_protection[page] ? 0 : run + 1;
		if (run > largest)
			largest = run;
	}
	if (in_use)
		*in_use = pages_in_use * PAGE_SIZE_BYTES;
	if (high_water)
		*high_water = pages_high_water * PAGE_SIZE_BYTES;
	if (largest_free)
		*largest_free = largest * PAGE_SIZE_BYTES;
	pthread_mutex_unlock(&arena_lock);
}

/* (a block that could not be placed: the window's state and its blocks of a
megabyte or more, so that what holds the room can be told) */
static void contiguous_log_blocks(void)
{
	unsigned long in_use, high_water, largest_free, page;

	platform_contiguous_usage(&in_use, &high_water, &largest_free);
	platform_log("contiguous memory: %lu KB in use (the most %lu KB), the largest free run %lu KB",
		in_use / 1024, high_water / 1024, largest_free / 1024);
	pthread_mutex_lock(&arena_lock);
	for (page = 0; page < CONTIGUOUS_PAGE_COUNT; page++)
	{
		if (page_protection[page] && block_first_page[page] == page && block_page_count[page] >= 256)
			platform_log("contiguous memory:   a block at 0x%08lx, %lu KB",
				PLATFORM_CONTIGUOUS_BASE + page * PAGE_SIZE_BYTES, block_page_count[page] * (PAGE_SIZE_BYTES / 1024));
	}
	pthread_mutex_unlock(&arena_lock);
}

void platform_contiguous_free(void *address)
{
	unsigned long first, count, page;

	if (!platform_is_contiguous(address))
		return;
	first = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	pthread_mutex_lock(&arena_lock);
	count = block_page_count[first];
	if (count)
	{
		memory_watch_forget(address, count * PAGE_SIZE_BYTES);
		mmap(address, count * PAGE_SIZE_BYTES, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
		for (page = first; page < first + count; page++)
			page_protection[page] = 0;
		block_page_count[first] = 0;
		pages_in_use -= count;
	}
	pthread_mutex_unlock(&arena_lock);
}

void platform_contiguous_discard(void *address, unsigned long size)
{
	unsigned long start = ((unsigned long)address + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);
	unsigned long end = ((unsigned long)address + size) & ~(PAGE_SIZE_BYTES - 1);
	unsigned long page, run;

	if (end <= start || !platform_is_contiguous((void *)start) || !platform_is_contiguous((void *)(end - 1)))
		return;
	pthread_mutex_lock(&arena_lock);
	/* (fresh zeroed pages mapped over them, run by run of one protection;
	pages of no block are left alone) */
	for (page = (start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES; page < (end - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
		page = run)
	{
		void *run_address = (void *)(PLATFORM_CONTIGUOUS_BASE + page * PAGE_SIZE_BYTES);

		for (run = page + 1; run < (end - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES &&
			page_protection[run] == page_protection[page]; run++)
			;
		if (!page_protection[page])
			continue;
		memory_watch_forget(run_address, (run - page) * PAGE_SIZE_BYTES);
		mmap(run_address, (run - page) * PAGE_SIZE_BYTES, protection_to_host(page_protection[page]),
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	}
	pthread_mutex_unlock(&arena_lock);
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* A highest-acceptable address inside the window places the block
	there, which is how the game gets its fixed game state and tag cache
	addresses; anything else may go anywhere. */
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address < PLATFORM_CONTIGUOUS_SIZE ? physical_address : PLATFORM_ANY_PHYSICAL_ADDRESS,
		protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx, alignment 0x%lx)",
			(unsigned long)size, (unsigned long)physical_address, (unsigned long)alignment);
		contiguous_log_blocks();
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	unsigned long start = (unsigned long)address & ~(PAGE_SIZE_BYTES - 1);
	unsigned long end = ((unsigned long)address + size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	if (old_protect)
		*old_protect = platform_is_contiguous(address) ?
			page_protection[(start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES] : PAGE_READWRITE;
	memory_watch_forget((void *)start, end - start);
	if (mprotect((void *)start, end - start, protection_to_host(new_protect)) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (platform_is_contiguous((void *)start))
	{
		unsigned long page;

		pthread_mutex_lock(&arena_lock);
		for (page = (start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
			page < (end - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES && page < CONTIGUOUS_PAGE_COUNT;
			page++)
		{
			if (page_protection[page])
				page_protection[page] = new_protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
		}
		pthread_mutex_unlock(&arena_lock);
	}
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	DWORD protect = PAGE_READWRITE;

	if (platform_is_contiguous(address))
	{
		pthread_mutex_lock(&arena_lock);
		protect = page_protection[((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES];
		pthread_mutex_unlock(&arena_lock);
		if (!protect)
			protect = PAGE_NOACCESS;
	}
	return protect;
}
