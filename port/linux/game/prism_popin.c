/*
PRISM_POPIN.C

What PRISM does against pop-in on the game's side (prism_popin.h).

display.prism_texture_wait: the game's texture cache reads a map's bitmap
on its file thread when a draw first needs it, and a draw whose texture is
not in yet is left out of the frame (rasterizer_set_texture: "pending"), as
the Xbox did; the surface or the object then appears a frame or more later.
With a number of milliseconds, such a draw waits for its texture instead
(xbox_texture_cache.c), the read promoted, until it is in or the frame has
waited that long in all; then the draws are left out as before. 0 is the
Xbox's way.

display.prism_lod_hysteresis: the levels of detail PRISM chooses (the
remodel's near and far surfaces, prism_world_geometry.c; the mesh store's
levels, the host's) keep the more detailed level this fraction past the
distance or size it is made for before the lighter one is drawn, and take
it back at the distance, so that a camera standing near the distance does
not switch them back and forth; never a lighter level sooner than without.

display.prism_world_geometry_band: each surface of the remodel switches at
its own distance in a band this wide past the file's, the same each time
(by its number), so that the surfaces at one distance do not switch in one
frame, a ring of them at once, but one after another as the camera moves;
none nearer than the file's distance.

The counts (of draws left out and waited for, and of the remodel's
switches) are logged every LOG_FRAMES frames where any changed, and on the
camera path's end (camera_pose.c).
*/

#include "cseries.h"
#include "prism_popin.h"

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
long config_integer(char const *name);
unsigned long config_changes(void);
void platform_log(const char *format, ...);
unsigned long long platform_shared_clock_microseconds(void);

#define LOG_FRAMES 1800
#define MAXIMUM_WAIT_MILLISECONDS 50
#define MAXIMUM_PERCENT 50

static struct
{
	unsigned long settings_read;
	long wait_milliseconds;
	float hysteresis, band;

	/* the frame's waiting: whether it started, when, and the time spent */
	boolean waiting;
	double wait_started;
	double wait_spent;

	unsigned long frames;
	unsigned long skipped, waited, world_switches;
	double waited_seconds;
	unsigned long logged_skipped, logged_waited, logged_world_switches;
} prism_popin;

static double seconds_now(void)
{
	return (double)platform_shared_clock_microseconds() * 1e-6;
}

static long percent_setting(char const *name)
{
	long value = config_integer(name);

	return value < 0 ? 0 : value > MAXIMUM_PERCENT ? MAXIMUM_PERCENT : value;
}

static void settings_read(void)
{
	if (prism_popin.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_popin.settings_read = config_changes() + 1;
	prism_popin.wait_milliseconds = config_integer("display.prism_texture_wait");
	if (prism_popin.wait_milliseconds < 0)
	{
		prism_popin.wait_milliseconds = 0;
	}
	if (prism_popin.wait_milliseconds > MAXIMUM_WAIT_MILLISECONDS)
	{
		prism_popin.wait_milliseconds = MAXIMUM_WAIT_MILLISECONDS;
	}
	prism_popin.hysteresis = (float)percent_setting("display.prism_lod_hysteresis") / 100.0f;
	prism_popin.band = (float)percent_setting("display.prism_world_geometry_band") / 100.0f;
}

void prism_popin_frame(void)
{
	settings_read();
	if (prism_popin.waiting)
	{
		prism_popin.waited_seconds += prism_popin.wait_spent;
	}
	prism_popin.waiting = FALSE;
	prism_popin.wait_spent = 0.0;
	if (++prism_popin.frames >= LOG_FRAMES)
	{
		prism_popin.frames = 0;
		if (prism_popin.skipped != prism_popin.logged_skipped || prism_popin.waited != prism_popin.logged_waited ||
			prism_popin.world_switches != prism_popin.logged_world_switches)
		{
			platform_log("prism popin: %lu draws left out for a texture not in, %lu waited for it (%.1f ms in all); "
				"%lu remodel surfaces switched near or far", prism_popin.skipped, prism_popin.waited,
				prism_popin.waited_seconds * 1000.0, prism_popin.world_switches);
			prism_popin.logged_skipped = prism_popin.skipped;
			prism_popin.logged_waited = prism_popin.waited;
			prism_popin.logged_world_switches = prism_popin.world_switches;
		}
	}
}

int prism_popin_texture_wait_begin(void)
{
	settings_read();
	if (prism_popin.wait_milliseconds <= 0)
	{
		return FALSE;
	}
	if (!prism_popin.waiting)
	{
		prism_popin.waiting = TRUE;
		prism_popin.wait_spent = 0.0;
	}
	prism_popin.wait_started = seconds_now();
	return prism_popin.wait_spent * 1000.0 < (double)prism_popin.wait_milliseconds;
}

int prism_popin_texture_waiting(void)
{
	return (prism_popin.wait_spent + seconds_now() - prism_popin.wait_started) * 1000.0 <
		(double)prism_popin.wait_milliseconds;
}

void prism_popin_texture_waited(int loaded)
{
	prism_popin.wait_spent += seconds_now() - prism_popin.wait_started;
	if (loaded)
	{
		prism_popin.waited++;
	}
}

void prism_popin_texture_skipped(void)
{
	prism_popin.skipped++;
}

float prism_popin_hysteresis(void)
{
	settings_read();
	return prism_popin.hysteresis;
}

float prism_popin_band(void)
{
	settings_read();
	return prism_popin.band;
}

void prism_popin_world_switched(void)
{
	prism_popin.world_switches++;
}

void prism_popin_counts(unsigned long *skipped, unsigned long *waited, unsigned long *world_switches,
	double *waited_milliseconds)
{
	*skipped = prism_popin.skipped;
	*waited = prism_popin.waited;
	*world_switches = prism_popin.world_switches;
	*waited_milliseconds = (prism_popin.waited_seconds + prism_popin.wait_spent) * 1000.0;
}
