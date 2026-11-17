/*
STICK_TEST_MENU.INC

Stick Test (#1140), a hand-written screen under the new menu engine
(screens/stick_test.xml, docs/menu_screens.md, "Stick Test"), opened from
Advanced Stick Settings' STICK TEST ›. Included in menu_functions.c after
settings_value, whose values it reads.

Each stick of the player whose profile Settings edits, as their controller
read it last, the controller's alone, never the keyboard's
(halo_controller_stick: left to right, then down to up, in percent of its
travel), then after the dead zones and the look stick's shape Settings
shows, its changes kept before OK among them (settings_value), as
input_xbox.c's input_port_fix_stick_dead_zones would take it with them. The
screen's <stick>s show them: each its words ({stick.left}, raw | after),
DRIFT AT REST ({stick.left.drift}: the largest push the stick held still
near its middle since the screen opened) and its circle, which the engine
draws (menu_screen.c's <stick>: pc_menu_stick_test_draw): its travel, its
dead zones (round, or a square for each direction's own) and both readings
as dots. Read only while the screen is drawn (pc_menu_stick_test_frame, its
place's frame): nothing drawn or kept elsewhere.
*/

void menu_shape_rounded_box(real x0, real y0, real x1, real y1, real radius, real edge, pixel32 fill,
	pixel32 color);

/* still: moved less than this since it last moved (1% of the travel) */
#define STICK_TEST_STILL 328
/* at rest: still this long, within this much of its middle (30%) */
#define STICK_TEST_REST_MILLISECONDS 500
#define STICK_TEST_REST_MOST 9830
/* the screen gone this long: its drift seen begun again */
#define STICK_TEST_REOPEN_MILLISECONDS 1000

struct stick_test_reading
{
	point2d raw, adjusted;
	short dead_range, live_range;
	boolean round;
};

/* the circle's rings, as halves of their width, and its dots, from its
middle (down positive), each in units for a circle of a radius */
struct stick_test_view
{
	real inner, outer;
	boolean round;
	real raw_x, raw_y, adjusted_x, adjusted_y;
};

static struct
{
	boolean connected;
	struct stick_test_reading readings[2];
	/* each stick's rest: where it was when it last moved, since when, the
	largest push seen at rest, when the screen last read it */
	point2d anchors[2];
	unsigned long still_since[2];
	short drift[2];
	unsigned long seen[2];
	/* (when the screen last read the dead zones, with or without a
	controller) */
	unsigned long read_at;
} stick_test;

static int stick_test_percent(short value)
{
	return (int)PIN((long)value * 100 / 32767, -100, 100);
}

static void stick_test_read(short gamepad, short stick, struct stick_test_reading *reading)
{
	static char const *const inner[2] = { "input.left_stick_dead_zone", "input.right_stick_dead_zone" };
	static char const *const outer[2] = { "input.left_stick_outer_dead_zone", "input.right_stick_outer_dead_zone" };
	char value[32];
	long inner_percent = -1;
	long outer_percent = 0;

	reading->round = FALSE;
	if (settings_value(inner[stick], value, sizeof(value)))
		inner_percent = atol(value);
	if (settings_value(outer[stick], value, sizeof(value)))
		outer_percent = atol(value);
	halo_stick_ranges(inner_percent, outer_percent, &reading->dead_range, &reading->live_range);
	/* (the look stick's shape: Auto, or none set, Modern aim's round) */
	if (stick == input_abstraction_port_look_stick(gamepad))
	{
		if (!settings_value("input.look_dead_zone_shape", value, sizeof(value)))
			value[0] = 0;
		if (!strcmp(value, "round"))
			reading->round = TRUE;
		else if (strcmp(value, "axial") && settings_value("input.aim_style", value, sizeof(value)))
			reading->round = !strcmp(value, "modern");
	}
	halo_controller_stick(gamepad, stick, &reading->raw.x, &reading->raw.y);
	input_port_fix_stick(reading->raw.x, reading->raw.y, reading->dead_range, reading->live_range, reading->round,
		&reading->adjusted);
}

static void stick_test_text(struct stick_test_reading const *reading, char *text, unsigned int size)
{
	snprintf(text, size, "%d,%d | %d,%d", stick_test_percent(reading->raw.x), stick_test_percent(reading->raw.y),
		stick_test_percent(reading->adjusted.x), stick_test_percent(reading->adjusted.y));
}

/* a stick's rest, at a reading now: still (moved less than STICK_TEST_STILL
since it last moved) for STICK_TEST_REST_MILLISECONDS, within
STICK_TEST_REST_MOST of its middle, its push the drift if the largest yet;
the screen opened again (not read for a second): all begun again */
static void stick_test_rest(short stick, point2d raw, unsigned long now)
{
	if (now - stick_test.seen[stick] > STICK_TEST_REOPEN_MILLISECONDS || !stick_test.seen[stick])
	{
		stick_test.drift[stick] = 0;
		stick_test.anchors[stick] = raw;
		stick_test.still_since[stick] = now;
	}
	stick_test.seen[stick] = now;
	if (abs(raw.x - stick_test.anchors[stick].x) > STICK_TEST_STILL ||
		abs(raw.y - stick_test.anchors[stick].y) > STICK_TEST_STILL)
	{
		stick_test.anchors[stick] = raw;
		stick_test.still_since[stick] = now;
	}
	else if (now - stick_test.still_since[stick] >= STICK_TEST_REST_MILLISECONDS)
	{
		real length = (real)sqrt((real)raw.x * (real)raw.x + (real)raw.y * (real)raw.y);

		if (length <= (real)STICK_TEST_REST_MOST && length > (real)stick_test.drift[stick])
			stick_test.drift[stick] = (short)length;
	}
}

static void stick_test_view_of(struct stick_test_reading const *reading, real radius, struct stick_test_view *view)
{
	real scale = radius / 32767.0f;

	view->inner = reading->dead_range * scale;
	view->outer = reading->live_range * scale;
	view->round = reading->round;
	view->raw_x = PIN(reading->raw.x * scale, -radius, radius);
	view->raw_y = PIN(-reading->raw.y * scale, -radius, radius);
	view->adjusted_x = PIN(reading->adjusted.x * scale, -radius, radius);
	view->adjusted_y = PIN(-reading->adjusted.y * scale, -radius, radius);
}

/* (menu_screen.c's place's frame for screens/stick_test.xml) both sticks'
dead zones and readings read, and their rests, each frame the screen is
drawn (without a controller, its dead zones alone: the readings none) */
void pc_menu_stick_test_frame(struct widget_instance *screen)
{
	struct controller_device device;
	short gamepad = settings_local_player;
	unsigned long now = system_milliseconds();
	short stick;

	(void)screen;
	controller_device_get(gamepad, &device);
	stick_test.connected = device.name[0] != 0;
	stick_test.read_at = now;
	for (stick = 0; stick < 2; stick++)
	{
		stick_test_read(gamepad, stick, &stick_test.readings[stick]);
		if (stick_test.connected)
			stick_test_rest(stick, stick_test.readings[stick].raw, now);
	}
}

/* (menu_screen.c's bindings) stick.left and stick.right: the reading, raw
| after the dead zones, or NO CONTROLLER; stick.left.drift and
stick.right.drift: the drift at rest, none without a controller */
int pc_menu_stick_value(char const *name, char *value, unsigned long size)
{
	static char const *const readings[2] = { "stick.left", "stick.right" };
	static char const *const drifts[2] = { "stick.left.drift", "stick.right.drift" };
	short stick;

	for (stick = 0; stick < 2; stick++)
	{
		if (!strcmp(name, readings[stick]))
		{
			if (stick_test.connected)
				stick_test_text(&stick_test.readings[stick], value, (unsigned int)size);
			else
				snprintf(value, size, "%s", "NO CONTROLLER");
			return 1;
		}
		if (!strcmp(name, drifts[stick]) && stick_test.connected)
		{
			snprintf(value, size, "%d%%", stick_test_percent(stick_test.drift[stick]));
			return 1;
		}
	}
	return 0;
}

/* (menu_screen.c's <stick>, as the engine draws it) a stick's circle, its
middle at x, y: its travel, its outer dead zone (where a push reads as
full) and inner one (where none), the raw reading white, the reading after
them blue (without a controller, the circle and its dead zones alone);
FALSE (nothing drawn) without a reading of the screen's this second */
boolean pc_menu_stick_test_draw(short stick, real x, real y, real radius)
{
	struct stick_test_view view;

	if (stick < 0 || stick > 1 || !stick_test.read_at ||
		system_milliseconds() - stick_test.read_at > STICK_TEST_REOPEN_MILLISECONDS)
	{
		return FALSE;
	}
	stick_test_view_of(&stick_test.readings[stick], radius, &view);
	menu_shape_rounded_box(x - radius, y - radius, x + radius, y + radius, radius, 1.0f, 0x80000000UL,
		0xFF2896FFUL);
	if (view.outer < radius)
	{
		menu_shape_rounded_box(x - view.outer, y - view.outer, x + view.outer, y + view.outer,
			view.round ? view.outer : 0.0f, 1.0f, 0, 0xFF6F9CD0UL);
	}
	if (view.inner > 0.0f)
	{
		menu_shape_rounded_box(x - view.inner, y - view.inner, x + view.inner, y + view.inner,
			view.round ? view.inner : 0.0f, 1.0f, 0x40FFB000UL, 0xFFFFB000UL);
	}
	if (!stick_test.connected)
		return TRUE;
	menu_shape_rounded_box(x + view.raw_x - 3.0f, y + view.raw_y - 3.0f, x + view.raw_x + 3.0f,
		y + view.raw_y + 3.0f, 3.0f, 0.0f, 0xFFFFFFFFUL, 0);
	menu_shape_rounded_box(x + view.adjusted_x - 4.0f, y + view.adjusted_y - 4.0f, x + view.adjusted_x + 4.0f,
		y + view.adjusted_y + 4.0f, 4.0f, 1.0f, 0xFF2896FFUL, 0xFFFFFFFFUL);
	return TRUE;
}
