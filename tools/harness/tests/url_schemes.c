/*
URL_SCHEMES.C

The desktop's links on Linux (port/linux/src/p2p.c's register_link_schemes,
posix_net.c's posix_register_url_scheme and posix_forget_url_scheme), in a
folder of desktop entries of the test's own (XDG_DATA_HOME), with the
executable's path and xdg-mime stood in for: test_url_schemes.py takes
them from the sources (under_test.inc). Each case is run by its name:

- schemes: the game registers its scheme, openh1, alone, and drops halo,
  OpenCE's (whose links it still takes: invite_links.c);
- register: openh1's desktop entry runs this executable with the link,
  and xdg-mime makes it the scheme's default;
- forget: a halo entry that runs this executable (an older build's) is
  removed; one that runs another (OpenCE's), or a program whose path only
  starts with this one's, is left, as is no entry at all.
*/

#include "harness.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* (the executable, as /proc/self/exe names it) */
#define EXECUTABLE "/games/openh1/halo"
#define OPENCE "/games/opence/halo"

static ssize_t fake_readlink(const char *path, char *buffer, size_t size)
{
	size_t length = strlen(EXECUTABLE);

	if (strcmp(path, "/proc/self/exe") || length > size)
		return -1;
	memcpy(buffer, EXECUTABLE, length);
	return (ssize_t)length;
}

/* the programs run (xdg-mime), their arguments joined by spaces */
static char programs[4][512];
static int program_count;

static int run_program(char *const arguments[])
{
	int index;

	if (program_count < 4)
	{
		for (index = 0; arguments[index]; index++)
		{
			strcat(programs[program_count], index ? " " : "");
			strcat(programs[program_count], arguments[index]);
		}
		program_count++;
	}
	return 0;
}

/* the schemes p2p.c registers and drops (p2p_register_url_scheme's and
p2p_forget_url_scheme's, which call posix_net.c's) */
static char registered[4][32], forgotten[4][32];
static int registered_count, forgotten_count;

static void p2p_register_url_scheme(const char *scheme, const char *description)
{
	(void)description;
	if (registered_count < 4)
		snprintf(registered[registered_count++], sizeof(registered[0]), "%s", scheme);
}

static void p2p_forget_url_scheme(const char *scheme)
{
	if (forgotten_count < 4)
		snprintf(forgotten[forgotten_count++], sizeof(forgotten[0]), "%s", scheme);
}

#define readlink fake_readlink
#include "under_test.inc"
#undef readlink

static char folder[256];

/* the test's own desktop entries' folder, empty */
static void make_folder(void)
{
	const char *temporary = getenv("TMPDIR");

	snprintf(folder, sizeof(folder), "%s/url-schemes-XXXXXX", temporary && *temporary ? temporary : "/tmp");
	CHECK(mkdtemp(folder) != NULL, "%s", folder);
	setenv("XDG_DATA_HOME", folder, 1);
}

static void remove_folder(void)
{
	char command[400];

	snprintf(command, sizeof(command), "rm -rf %s", folder);
	CHECK(system(command) == 0, "%s", command);
}

static void entry_path(const char *scheme, char *path, int size)
{
	snprintf(path, (size_t)size, "%s/applications/halo-ce-universal-%s.desktop", folder, scheme);
}

static int read_entry(const char *scheme, char *text, int size)
{
	char path[400];
	FILE *file;
	size_t length;

	entry_path(scheme, path, sizeof(path));
	file = fopen(path, "r");
	if (!file)
		return 0;
	length = fread(text, 1, (size_t)size - 1, file);
	fclose(file);
	text[length] = 0;
	return 1;
}

/* a halo entry of the kind posix_register_url_scheme makes, running this
program */
static void write_halo_entry(const char *program)
{
	char path[400], applications[300];
	FILE *file;

	snprintf(applications, sizeof(applications), "%s/applications", folder);
	mkdir(applications, 0700);
	entry_path("halo", path, sizeof(path));
	file = fopen(path, "w");
	CHECK(file != NULL, "%s", path);
	fprintf(file, "[Desktop Entry]\nType=Application\nName=Halo: Combat Evolved invite\nExec=\"%s\" %%u\n"
		"NoDisplay=true\nMimeType=x-scheme-handler/halo;\n", program);
	fclose(file);
}

static void case_schemes(void)
{
	int index;

	register_link_schemes();
	CHECK(registered_count == 1 && !strcmp(registered[0], "openh1"), "the game's scheme registered alone: %d, %s",
		registered_count, registered[0]);
	for (index = 0; index < registered_count; index++)
		CHECK(strcmp(registered[index], "halo"), "OpenCE's scheme never registered");
	CHECK(forgotten_count == 1 && !strcmp(forgotten[0], "halo"), "OpenCE's scheme dropped: %d, %s", forgotten_count,
		forgotten[0]);
}

/* fork: a build of the fork (HALO_FORK_MENDICANT, which the test defines for
this case's unit alone) registers nothing, and drops openh1 and halo: those
are other games' */
static void case_fork(void)
{
	char text[2048];

	register_link_schemes();
	CHECK(registered_count == 0, "nothing registered: %d, %s", registered_count, registered[0]);
	CHECK(forgotten_count == 2 && !strcmp(forgotten[0], "openh1") && !strcmp(forgotten[1], "halo"),
		"OpenH1's and OpenCE's schemes dropped: %d, %s, %s", forgotten_count, forgotten[0], forgotten[1]);
	/* (and dropped only where the entry runs this executable: an installed
	OpenH1's is left) */
	make_folder();
	CHECK(posix_register_url_scheme("openh1", "OpenH1 invite"), "an earlier build's registration");
	CHECK(posix_forget_url_scheme("openh1"), "this executable's entry dropped");
	CHECK(!read_entry("openh1", text, sizeof(text)), "and gone");
	{
		char path[400];
		FILE *file;

		entry_path("openh1", path, sizeof(path));
		file = fopen(path, "w");
		CHECK(file != NULL, "%s", path);
		fprintf(file, "[Desktop Entry]\nType=Application\nName=OpenH1 invite\nExec=\"%s\" %%u\n"
			"NoDisplay=true\nMimeType=x-scheme-handler/openh1;\n", OPENCE);
		fclose(file);
	}
	CHECK(!posix_forget_url_scheme("openh1"), "another game's entry left");
	CHECK(read_entry("openh1", text, sizeof(text)) && strstr(text, OPENCE), "and kept:\n%s", text);
	remove_folder();
}

static void case_register(void)
{
	char text[2048];

	make_folder();
	CHECK(posix_register_url_scheme("openh1", "OpenH1 invite"), "registered");
	CHECK(read_entry("openh1", text, sizeof(text)), "its desktop entry");
	CHECK(strstr(text, "\nExec=\"" EXECUTABLE "\" %u\n") && strstr(text, "\nMimeType=x-scheme-handler/openh1;\n") &&
		strstr(text, "\nName=OpenH1 invite\n"), "the entry:\n%s", text);
	CHECK(program_count == 1 && !strcmp(programs[0], "xdg-mime default halo-ce-universal-openh1.desktop "
		"x-scheme-handler/openh1"), "xdg-mime: %s", programs[0]);
	/* (unchanged, nothing is run again) */
	CHECK(posix_register_url_scheme("openh1", "OpenH1 invite") && program_count == 1, "again, as it was");
	remove_folder();
}

static void case_forget(void)
{
	char text[2048];

	make_folder();
	CHECK(!posix_forget_url_scheme("halo"), "no entry, nothing dropped");
	/* (an older build's: this executable's) */
	write_halo_entry(EXECUTABLE);
	CHECK(posix_forget_url_scheme("halo"), "this executable's entry dropped");
	CHECK(!read_entry("halo", text, sizeof(text)), "and gone");
	/* (OpenCE's) */
	write_halo_entry(OPENCE);
	CHECK(!posix_forget_url_scheme("halo"), "OpenCE's entry left");
	CHECK(read_entry("halo", text, sizeof(text)) && strstr(text, OPENCE), "and kept:\n%s", text);
	/* (a program whose path starts with this executable's) */
	write_halo_entry(EXECUTABLE "-opence");
	CHECK(!posix_forget_url_scheme("halo"), "another program's entry left");
	CHECK(read_entry("halo", text, sizeof(text)), "and kept");
	CHECK(program_count == 0, "nothing run: %s", programs[0]);
	remove_folder();
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "schemes"))
		case_schemes();
	else if (argc > 1 && !strcmp(argv[1], "register"))
		case_register();
	else if (argc > 1 && !strcmp(argv[1], "forget"))
		case_forget();
	else if (argc > 1 && !strcmp(argv[1], "fork"))
		case_fork();
	else
		return 2;
	return 0;
}
