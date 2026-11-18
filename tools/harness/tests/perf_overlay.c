/*
PERF_OVERLAY.C (test)

Settings > Video's PERFORMANCE OVERLAY (display.perf_overlay):
port/linux/game/perf_overlay.c's perf_overlay_mode, which
main_framerate_render draws the frame rate (and with "detailed" the frame's
times) by, follows the setting as it changes. test_perf_overlay.py takes the
code.
*/

#include "harness.h"

/* the setting, and config.toml's changes (port_config.c) */
static const char *perf_overlay = "off";
static unsigned long changes = 1;

static const char *config_string(const char *name)
{
	CHECK(!strcmp(name, "display.perf_overlay"), "read %s", name);
	return perf_overlay;
}

static unsigned long config_changes(void)
{
	return changes;
}

#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	if (!strcmp(test, "follows"))
	{
		CHECK(perf_overlay_mode() == _perf_overlay_off, "on by default");
		perf_overlay = "simple";
		changes++;
		CHECK(perf_overlay_mode() == _perf_overlay_simple, "not simple once set so");
		perf_overlay = "detailed";
		changes++;
		CHECK(perf_overlay_mode() == _perf_overlay_detailed, "not detailed once set so");
		perf_overlay = "nonsense";
		changes++;
		CHECK(perf_overlay_mode() == _perf_overlay_off, "a word it has not, not off");
		perf_overlay = "off";
		changes++;
		CHECK(perf_overlay_mode() == _perf_overlay_off, "not off once turned off");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
