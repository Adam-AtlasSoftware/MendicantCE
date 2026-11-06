/*
POSIX_MENDICANT.C

MENDICANT AI's platform side (port/linux/game/mendicant): the model bridge's
shared memory, which a trainer or brain.py maps from the same path, and the
clock, yield and sleep that the training wait and the paused episode use.
Desktop Linux only (the web build and Windows leave this file out; Android
skips posix_*). Port: in the Linux builds it is the guest's own (tools/android_build.py's guest_posix),
whose open, ftruncate, mmap (MAP_SHARED of a file), clock_gettime and
sched_yield the host performs (port/android/host/host_syscall.c), so the
trainer maps the same file. The other guests (Android, Windows, the Apple
builds) compile it empty.
*/

#if !defined(HALO_GUEST) || !(defined(HALO_WINDOWS) || defined(HALO_ANDROID) || defined(HALO_MACOS))

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* the file at path, created if missing (0600), sized and mapped shared;
NULL when any step fails */
void *posix_mendicant_bridge_map(char const *path, unsigned long size)
{
	void *memory;
	int descriptor = open(path, O_RDWR | O_CREAT, 0600);

	if (descriptor < 0)
		return NULL;
	if (ftruncate(descriptor, (off_t)size))
	{
		close(descriptor);
		return NULL;
	}
	memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	close(descriptor);
	return memory == MAP_FAILED ? NULL : memory;
}

void posix_mendicant_yield(void)
{
	sched_yield();
}

unsigned long long posix_mendicant_monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec;
}

void posix_mendicant_sleep_microseconds(unsigned long microseconds)
{
	usleep((useconds_t)microseconds);
}

#endif
