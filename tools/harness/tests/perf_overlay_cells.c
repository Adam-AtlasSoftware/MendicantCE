/*
PERF_OVERLAY_CELLS.C (test)

The performance overlay's detailed view (port/linux/game/perf_overlay.c):
the CPU sections' short labels (perf_overlay_section_label), every time's
one format (perf_overlay_milliseconds), and the cells' place in a view
(perf_overlay_place) at 4:3, 16:9 and 21:9, from 720p to 4K, in one view
and in 2 and 4-player split screen. test_perf_overlay_cells.py takes the
code.
*/

#include "harness.h"

#include <ctype.h>

#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#include "under_test.inc"

static int label_is(char const *name, char const *expected)
{
	char label[PERF_OVERLAY_LABEL_SIZE];

	perf_overlay_section_label(name, label, (short)sizeof(label));
	CHECK(!strcmp(label, expected), "%s labeled \"%s\", not \"%s\"", name, label, expected);
	return 1;
}

static int milliseconds_are(real milliseconds, char const *expected)
{
	char value[PERF_OVERLAY_VALUE_SIZE];

	perf_overlay_milliseconds(milliseconds, value, (short)sizeof(value));
	CHECK(!strcmp(value, expected), "%g ms shown \"%s\", not \"%s\"", (double)milliseconds, value, expected);
	return 1;
}

/* the screens: units across (480 tall), and display pixels a unit */
static short const widths[] = { 640, 853, 1120 };
static char const *const shapes[] = { "4:3", "16:9", "21:9" };
static real const pixels[] = { 1.5f, 2.25f, 4.5f };
static char const *const heights[] = { "720p", "1080p", "4K" };
/* a cell as wide as the longest label and a value make them, and the
font's capitals */
#define CELL_WIDTH 220
#define CAPITAL_HEIGHT 10

static int fits(short x0, short y0, short x1, short y1, real pixel, int players, char const *where)
{
	struct perf_overlay_place place;
	real left, top, scaled_height;

	perf_overlay_place(x0, y0, x1, y1, pixel, players > 1, PERF_OVERLAY_CELLS, CELL_WIDTH, CAPITAL_HEIGHT, &place);
	left = place.anchor_x - place.width * place.scale;
	scaled_height = place.height * place.scale;
	top = place.anchor_y - scaled_height;
	CHECK(place.rows > 0, "%s: no rows", where);
	/* (in the view, at its bottom right corner, above the counter) */
	CHECK(left >= x0 && top >= y0, "%s: past the view (left %.0f of %d, top %.0f of %d)", where, (double)left, x0,
		(double)top, y0);
	CHECK(x1 - place.anchor_x <= 20 && y1 - place.anchor_y <= 60 && y1 - place.anchor_y >= 50,
		"%s: not in the bottom right corner", where);
	/* (compact: under half the view's width, and the player names and the
	crosshair in the middle left clear) */
	CHECK(left >= x0 + (x1 - x0) * 0.5f, "%s: %.0f of %d units wide", where, (double)(place.anchor_x - left),
		x1 - x0);
	CHECK(top >= y0 + (y1 - y0) * 0.3f, "%s: %.0f of %d units tall", where, (double)scaled_height, y1 - y0);
	/* (every row in one view and in a 2-player split screen; readable
	there: capitals at least 9 display pixels tall) */
	if (players <= 2)
	{
		CHECK(place.rows == (PERF_OVERLAY_CELLS + 1) / 2, "%s: %d rows of %d", where, place.rows,
			(PERF_OVERLAY_CELLS + 1) / 2);
		CHECK(place.scale * CAPITAL_HEIGHT * pixel >= 8.99f, "%s: capitals %.1f pixels tall", where,
			(double)(place.scale * CAPITAL_HEIGHT * pixel));
	}
	return 1;
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	if (!strcmp(test, "labels"))
	{
		char label[4];
		static char const *const names[] = { "game_time_update", "game_update", "render_objects", "present",
			"game_tick.ai", "network_distributed_tick.send_pings", "render_structure_fog",
			"render_structure_diffuse_lights", "render_structure_visibility_portal_traversal",
			"memory_dynamic_array_resize", "emotions", "", NULL };
		short index;

		/* (the brief's three, and a table entry, a start's, and the
		console's names that have none) */
		label_is("game_time_update", "GAME TICK");
		label_is("game_update", "SIMULATION");
		label_is("render_objects", "OBJECTS");
		label_is("render_structure_fog", "FOG");
		label_is("render_structure_diffuse_lights", "LEVEL");
		label_is("render_structure_visibility_portal_traversal", "VISIBILITY");
		label_is("network_distributed_tick.send_pings", "NETWORK");
		label_is("game_tick.players_after", "PLAYERS");
		label_is("game_tick.remove_quitting_players", "REMOVE QUITT");
		label_is("memory_dynamic_array_resize", "MEMORY DYNAM");
		label_is("emotions", "EMOTIONS");
		label_is("", "");
		label_is(NULL, "");
		/* (every label in capitals, no '_' or '.', and short) */
		for (index = 0; names[index]; index++)
		{
			char shown[PERF_OVERLAY_LABEL_SIZE];
			char const *letter;

			perf_overlay_section_label(names[index], shown, (short)sizeof(shown));
			CHECK(strlen(shown) <= 12, "%s's label %s too long", names[index], shown);
			for (letter = shown; *letter; letter++)
				CHECK(!islower((unsigned char)*letter) && *letter != '_' && *letter != '.', "%s's label %s",
					names[index], shown);
		}
		/* (cut to the room given) */
		perf_overlay_section_label("game_time_update", label, (short)sizeof(label));
		CHECK(!strcmp(label, "GAM"), "cut to %s", label);
		return 0;
	}
	if (!strcmp(test, "milliseconds"))
	{
		milliseconds_are(5.14f, "5.14 ms");
		milliseconds_are(7.4f, "7.40 ms");
		milliseconds_are(0.983f, "0.98 ms");
		milliseconds_are(33.333f, "33.33 ms");
		milliseconds_are(0.0f, "0.00 ms");
		milliseconds_are(-1.0f, "--");
		return 0;
	}
	if (!strcmp(test, "fit"))
	{
		short shape, height;

		for (shape = 0; shape < 3; shape++)
		{
			for (height = 0; height < 3; height++)
			{
				short w = widths[shape], half = (short)(w / 2);
				char where[64];

				snprintf(where, sizeof(where), "%s %s, one view", shapes[shape], heights[height]);
				fits(0, 0, w, 480, pixels[height], 1, where);
				snprintf(where, sizeof(where), "%s %s, 2 players' top", shapes[shape], heights[height]);
				fits(0, 0, w, 240, pixels[height], 2, where);
				snprintf(where, sizeof(where), "%s %s, 2 players' bottom", shapes[shape], heights[height]);
				fits(0, 240, w, 480, pixels[height], 2, where);
				snprintf(where, sizeof(where), "%s %s, 4 players' top left", shapes[shape], heights[height]);
				fits(0, 0, half, 240, pixels[height], 4, where);
				snprintf(where, sizeof(where), "%s %s, 4 players' bottom right", shapes[shape], heights[height]);
				fits(half, 240, w, 480, pixels[height], 4, where);
			}
		}
		return 0;
	}
	if (!strcmp(test, "compact"))
	{
		struct perf_overlay_place place;

		/* (4K at 16:9, one view: the old overlay took 400 units of 853
		across and 310 of 480 up; now well under a third of each) */
		perf_overlay_place(0, 0, 853, 480, 4.5f, FALSE, PERF_OVERLAY_CELLS, CELL_WIDTH, CAPITAL_HEIGHT, &place);
		CHECK(place.width * place.scale <= 853 * 0.32f, "%.0f of 853 units wide", (double)(place.width * place.scale));
		CHECK(place.height * place.scale <= 480 * 0.25f, "%.0f of 480 units tall",
			(double)(place.height * place.scale));
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
