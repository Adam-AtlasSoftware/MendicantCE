/*
HOST_MAIN.C

Entry point of the Linux builds that run the game as an ILP32 guest (the
64-bit ARM and x86 builds, tools/linux_guest_build.py).

The host is the Android port's (port/android/host, port/android/README.md),
with this file in place of its host_main.c and host_touch.c; what both
mains do is in host_common.c there. It loads the guest image (the game,
built as ILP32 code) from inside the executable (guest_image.S), and runs
its main() on a thread of its own with its stack in guest memory
(host_thread.c). The main thread waits; the game ends the process.

The guest is the Linux desktop build of the game, so it finds the game data,
the saves and config.toml as port/linux/README.md says: it gets the part of
this process's environment that it reads (the HALO_ settings, HOME, the XDG
folders), and /proc/self/exe is this executable.
The log is the standard error stream.

The macOS build (tools/macos_build.py, port/macos) uses this file too. There
the guest's memory lies at a base the host chooses (HALO_GUEST_BASE), its
system calls are translated to Darwin's (port/macos/host_syscall.c), its
config.toml is in the app's settings folder (port/macos/host_app.m), and
the game runs on the process's main thread, which Cocoa's windows and
events need, after moving it onto a stack in guest memory.

The iOS build (tools/ios_build.py, port/ios) is the macOS one with UIKit's
app around it: main hands the process to UIKit's run loop (SDL_RunApp),
which calls back once the app has launched, and the game then runs on the
main thread as on macOS, its event pumps running UIKit's loop. The app's
folders are port/ios/host_app.m's, and the guest's code is mapped from the
executable (host_loader.c).
*/

#include "host.h"
#include "host_load_trace.h"
#include "install_layout.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <sys/mman.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#endif
#include <sys/uio.h>
#include <unistd.h>

/* guest_image.S */
extern const unsigned char halo_guest_image_start[];
extern const unsigned char halo_guest_image_end[];

extern char **environ;

#ifdef __APPLE__
/* port/macos/host_metal.m: the Metal backend, 1 if there is a Metal
device */
int host_metal_initialize(void);
/* port/macos/host_app.m (port/ios/host_app.m on iOS): the app's folders,
maps and window, when the executable runs inside OpenH1.app */
void host_app_start(void);
const char *host_app_settings_folder(void);
#endif

/* ---------- the log: the standard error stream, and on Linux halo.log */

#ifndef __APPLE__
/* halo.log, in the folder of config.toml (the game's folder, or
~/.config/halo-linux when the game is installed: install_layout.c): every
line of the log, the host's and the game's (which adds its own to the
file's end, port/linux/src/xbox_kernel.c: platform_log), for the player who
started the game from a desktop and has no terminal to read. The start
before this one's is halo.previous.log. (The Mac app keeps its error output
in a file itself, port/macos/host_app.m.) */
static int log_file = -1;
static char log_file_path[1024];

static void log_file_open(void)
{
	char previous[1024 + 16];
	int length = snprintf(log_file_path, sizeof(log_file_path), "%shalo.log", install_config_folder());

	if (length < 0 || length >= (int)sizeof(log_file_path) || log_file_path[0] != '/')
	{
		log_file_path[0] = '\0';
		return;
	}
	snprintf(previous, sizeof(previous), "%shalo.previous.log", install_config_folder());
	if (rename(log_file_path, previous) != 0 && errno != ENOENT)
		unlink(log_file_path);
	log_file = open(log_file_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	if (log_file < 0)
		log_file_path[0] = '\0';
}
#endif

/* one writev, so that the lines of threads do not mix (host_common.c: no
stdio, as signal handlers log through it too) */
static void log_to_stderr(int priority, const char *line)
{
	const char *level = priority >= HOST_LOG_FATAL ? "fatal: " : priority >= HOST_LOG_ERROR ? "error: " :
		priority >= HOST_LOG_WARN ? "warning: " : "";
	struct iovec parts[4];

	parts[0].iov_base = (void *)"halo-host: ";
	parts[0].iov_len = 11;
	parts[1].iov_base = (void *)level;
	parts[1].iov_len = strlen(level);
	parts[2].iov_base = (void *)line;
	parts[2].iov_len = strlen(line);
	parts[3].iov_base = (void *)"\n";
	parts[3].iov_len = 1;
#ifndef __APPLE__
	/* (a file that can no longer be written is let go) */
	if (log_file >= 0 && writev(log_file, parts, 4) < 0)
		log_file = -1;
#endif
	if (writev(2, parts, 4) < 0)
		return;
}

/* ---------- the guest's environment */

/* what the game reads: its settings (HALO_...), where the saves go
(port/linux/src/xbox_files.c) and where Discord's socket is (posix_net.c);
the display and sound are the host's. TZ is game_main's. */
static int environment_passes(const char *entry)
{
	static const char *const prefixes[] =
	{
		"HALO_", "HOME=", "XDG_", "USER=", "LANG=", "LC_", "TMPDIR=", "TMP=", "TEMP=",
	};
	size_t index;

	for (index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); index++)
	{
		if (!strncmp(entry, prefixes[index], strlen(prefixes[index])))
			return 1;
	}
	return 0;
}

/* HALO_GUEST_IMAGE (tests): another guest image, run in place of the game's,
linked as the game's is: StageKit's map writer (tools/stagekit), whose tags
are laid out at the game's 32-bit addresses, so it runs as a guest wherever
the game does */
static void load_other_image(const char *path)
{
	FILE *file = fopen(path, "rb");
	unsigned char *image;
	long size;

	if (!file || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0)
		host_fatal("cannot read the guest image %s", path);
	image = malloc((size_t)size);
	if (!image || fread(image, 1, (size_t)size, file) != (size_t)size)
		host_fatal("cannot read the guest image %s", path);
	fclose(file);
	if (host_load_image(image, (size_t)size) != 0)
		host_fatal("cannot load the guest image %s", path);
	free(image);
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

/* the thread that runs the game: the guest's environment, the image, the
debugging settings of config.toml (where the game reads it:
port/linux/src/install_layout.c, compiled into the host too), and the
guest's main; never returns normally */
static void *game_main(void *unused)
{
	struct host_environment environment = { { 0 }, 0 };
	char zone[64];
	char path[1024];
	char **entry;

	(void)unused;
	for (entry = environ; *entry; entry++)
	{
		if (environment_passes(*entry))
			host_environment_add(&environment, *entry);
	}
	host_time_zone(zone, sizeof(zone));
	host_environment_set(&environment, "TZ", zone);
#ifndef __APPLE__
	if (log_file_path[0])
		host_environment_set(&environment, "OPENH1_HOST_LOG", log_file_path);
#endif

	if (getenv("HALO_GUEST_IMAGE") && *getenv("HALO_GUEST_IMAGE"))
		load_other_image(getenv("HALO_GUEST_IMAGE"));
	else if (host_load_image(halo_guest_image_start, (size_t)(halo_guest_image_end - halo_guest_image_start)) != 0)
		host_fatal("cannot load the game image");
#ifdef __APPLE__
	{
		/* (the app's in its settings folder, else beside the executable,
		where the game reads it) */
		const char *base = host_app_settings_folder() ? host_app_settings_folder() : SDL_GetBasePath();

		snprintf(path, sizeof(path), "%sconfig.toml", base ? base : "");
	}
#else
	snprintf(path, sizeof(path), "%sconfig.toml", install_config_folder());
#endif
	host_apply_debug_settings(path, NULL);
	host_load_trace("host.guest_main", "");
	host_run_guest_main(host_make_boot(&environment, "halo"));
}

#ifdef __APPLE__
static void game_main_on_stack(void *unused)
{
	game_main(unused);
}

static int apple_main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	{
		/* Cocoa's windows and events belong to the main thread, and the
		game's main thread is the one that makes its window and pumps its
		events: the game runs here, on a stack in guest memory (with a
		guard page at its bottom) */
		char *stack;

		host_log_set_sink(log_to_stderr);
		/* (start-up's measurement: debug.load_trace) */
		host_load_trace("host.main", "");
		host_app_start();
		host_install_signal_handlers();
		host_signal_stack_install();
		/* (the renderer's backend, before the game asks for it) */
		host_metal_initialize();
		stack = host_low_map(MAIN_STACK_SIZE, PROT_READ | PROT_WRITE);
		if (!stack)
			host_fatal("cannot allocate the game's stack");
		mprotect(stack, host_page_size(), PROT_NONE);
		host_debug_thread_started();
		host_run_on_stack(game_main_on_stack, NULL, stack + MAIN_STACK_SIZE);
		host_fatal("the game returned");
	}
	return 1;
}
#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
#if defined(__APPLE__) && TARGET_OS_IPHONE
	/* (UIKit's run loop, which calls apple_main once the app has launched) */
	return SDL_RunApp(argc, argv, apple_main, NULL);
#elif defined(__APPLE__)
	return apple_main(argc, argv);
#endif
#ifndef __APPLE__
	log_file_open();
#endif
	host_log_set_sink(log_to_stderr);
	/* (start-up's measurement: debug.load_trace) */
	host_load_trace("host.main", "");
	host_install_signal_handlers();
	host_signal_stack_install();
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}
